// PTIssueSubsystem.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Detectors/PTIssueSubsystem.cpp

#include "Detectors/PTIssueSubsystem.h"

#include "AIController.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Coverage/PTCoverageSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "JsonObjectConverter.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Navigation/PathFollowingComponent.h"
#include "PTPlaytestSubsystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlaytestIssues, Log, All);

static TAutoConsoleVariable<int32> CVarPTTrackPlayer(
	TEXT("pt.TrackPlayer"),
	0,
	TEXT("1 = the issue detectors also watch the player's pawn during a run (for testing detectors by hand)."));

static TAutoConsoleVariable<float> CVarPTViewBots(
	TEXT("pt.ViewBots"),
	0.f,
	TEXT("During a run, switch the player view to a different bot every N seconds (0 = off). Perf samples use the current view."));

bool UPTIssueSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UPTIssueSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	CoverageSubsystem = Collection.InitializeDependency<UPTCoverageSubsystem>();

	UPTPlaytestSubsystem* Run = Collection.InitializeDependency<UPTPlaytestSubsystem>();
	if (Run)
	{
		RunSubsystem = Run;
		Run->OnRunStarted.AddUObject(this, &UPTIssueSubsystem::HandleRunStarted);
		Run->OnRunFinished.AddUObject(this, &UPTIssueSubsystem::HandleRunFinished);
	}
}

void UPTIssueSubsystem::Deinitialize()
{
	if (UPTPlaytestSubsystem* Run = RunSubsystem.Get())
	{
		Run->OnRunStarted.RemoveAll(this);
		Run->OnRunFinished.RemoveAll(this);
	}
	Super::Deinitialize();
}

TStatId UPTIssueSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPTIssueSubsystem, STATGROUP_Tickables);
}

void UPTIssueSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bActive)
	{
		SampleAccumulator += DeltaTime;
		if (SampleAccumulator >= SampleInterval)
		{
			SampleAccumulator = 0.f;
			SampleBots();
		}

		TickViewCycling(DeltaTime);
		TickPerf();
	}

	static const IConsoleVariable* DebugVar = IConsoleManager::Get().FindConsoleVariable(TEXT("pt.Debug"));
	if (DebugVar && DebugVar->GetInt() > 0)
	{
		DrawDebug();
	}
}

FString UPTIssueSubsystem::IssueTypeToString(EPTIssueType Type)
{
	switch (Type)
	{
	case EPTIssueType::Stuck:         return TEXT("Stuck");
	case EPTIssueType::OutOfWorld:    return TEXT("OutOfWorld");
	case EPTIssueType::Clip:          return TEXT("Clip");
	case EPTIssueType::ForbiddenArea: return TEXT("ForbiddenArea");
	case EPTIssueType::PerfSpike:     return TEXT("PerfSpike");
	default:                          return TEXT("Unknown");
	}
}

// ---------------------------------------------------------------------------
// Run lifecycle
// ---------------------------------------------------------------------------
void UPTIssueSubsystem::HandleRunStarted(const FPTRunConfig& Config)
{
	Issues.Reset();
	BotStates.Reset();
	SampleAccumulator = 0.f;
	bHasNavFloor = false;

	PerfWindowTime = 0.0;
	PerfWindowFrames = 0;
	PerfBaselineMs = 0.f;
	PerfRecentMs.Reset();
	PerfCooldownUntil = 0.f;
	ViewSwitchTimer = 0.f;
	ViewBotIndex = INDEX_NONE;

	float ViewSeconds = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("PTViewBots="), ViewSeconds))
	{
		CVarPTViewBots->Set(ViewSeconds);
	}

	GatherClipCandidates();
	GatherForbiddenZones();

	// Optional: watch the player too, so detectors can be tested by hand.
	if (CVarPTTrackPlayer.GetValueOnGameThread() > 0)
	{
		const APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (APawn* PlayerPawn = PlayerController ? PlayerController->GetPawn() : nullptr)
		{
			FBotState& State = BotStates.AddDefaulted_GetRef();
			State.Pawn = PlayerPawn;
			State.History.SetNum(ActionHistorySamples);
			State.SpawnTransform = PlayerPawn->GetActorTransform();
			State.LastKnownLocation = PlayerPawn->GetActorLocation();
			State.LastGroundedLocation = State.LastKnownLocation;
			UE_LOG(LogPlaytestIssues, Display, TEXT("Tracking the player pawn as bot %d."), BotStates.Num() - 1);
		}
	}

	if (const UPTPlaytestSubsystem* Run = RunSubsystem.Get())
	{
		for (const TWeakObjectPtr<APawn>& BotPtr : Run->GetBots())
		{
			FBotState& State = BotStates.AddDefaulted_GetRef();
			State.Pawn = BotPtr;
			State.History.SetNum(ActionHistorySamples);
			if (const APawn* Bot = BotPtr.Get())
			{
				State.SpawnTransform = Bot->GetActorTransform();
				State.LastKnownLocation = Bot->GetActorLocation();
				State.LastGroundedLocation = State.LastKnownLocation;
			}
		}
	}

	bActive = true;
}

void UPTIssueSubsystem::HandleRunFinished(const FPTRunConfig& Config)
{
	if (!bActive)
	{
		return;
	}
	bActive = false;

	// Give the camera back to the player if we were cycling through bot views.
	if (ViewBotIndex != INDEX_NONE)
	{
		if (APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
		{
			PlayerController->SetViewTarget(PlayerController->GetPawn());
		}
		ViewBotIndex = INDEX_NONE;
	}

	TMap<EPTIssueType, int32> Counts;
	for (const FPTIssueRecord& Issue : Issues)
	{
		Counts.FindOrAdd(Issue.Type)++;
	}
	FString Summary;
	for (const TPair<EPTIssueType, int32>& Pair : Counts)
	{
		Summary += FString::Printf(TEXT(" %s=%d"), *IssueTypeToString(Pair.Key), Pair.Value);
	}
	UE_LOG(LogPlaytestIssues, Display, TEXT("Issues found: %d%s"), Issues.Num(), *Summary);

	WriteReport();
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------
void UPTIssueSubsystem::SampleBots()
{
	const UPTPlaytestSubsystem* Run = RunSubsystem.Get();
	if (!Run)
	{
		return;
	}
	const float Now = Run->GetElapsedSeconds();

	for (int32 Index = 0; Index < BotStates.Num(); ++Index)
	{
		FBotState& State = BotStates[Index];
		APawn* Bot = State.Pawn.Get();

		if (!Bot)
		{
			// Destroyed mid-run: almost always fell below KillZ.
			if (!State.bLost)
			{
				State.bLost = true;
				ReportIssue(Index, EPTIssueType::OutOfWorld, State.LastGroundedLocation,
					TEXT("Bot was destroyed (most likely fell below KillZ); location = last grounded position"));
			}
			continue;
		}

		FPTActionSample Sample;
		Sample.Time = Now;
		Sample.Location = Bot->GetActorLocation();
		Sample.Velocity = Bot->GetVelocity();
		Sample.MoveInput = Bot->GetLastMovementInputVector();

		if (const ACharacter* BotCharacter = Cast<ACharacter>(Bot))
		{
			Sample.bJumping = BotCharacter->bPressedJump;
			Sample.bCrouched = BotCharacter->bIsCrouched;
		}
		if (const UPawnMovementComponent* Movement = Bot->GetMovementComponent())
		{
			Sample.bFalling = Movement->IsFalling();
		}

		const AAIController* BotAI = Cast<AAIController>(Bot->GetController());
		const bool bPathFollowing = BotAI && BotAI->GetMoveStatus() == EPathFollowingStatus::Moving;
		if (bPathFollowing && Sample.MoveInput.IsNearlyZero())
		{
			// Path-following bots have no input vector; use the direction the path wants instead.
			if (const UPathFollowingComponent* PathFollowing = BotAI->GetPathFollowingComponent())
			{
				Sample.MoveInput = PathFollowing->GetCurrentDirection();
			}
		}
		Sample.bMoveIntent = Sample.MoveInput.SizeSquared() > 0.25f || bPathFollowing;

		FPTActionSample PrevSample;
		const FPTActionSample* PrevPtr = GetSampleAgo(State, 0);
		const bool bHasPrev = PrevPtr != nullptr;
		if (bHasPrev)
		{
			PrevSample = *PrevPtr;
		}

		PushSample(State, Sample);
		State.LastKnownLocation = Sample.Location;
		if (!Sample.bFalling)
		{
			State.LastGroundedLocation = Sample.Location;
		}

		// Out of world is always checked (even during cooldown), so falls are caught right away.
		if (CheckOutOfWorld(Index, Sample))
		{
			continue;
		}
		if (Now < State.CooldownUntil)
		{
			continue;
		}
		if (bHasPrev && CheckClip(Index, PrevSample, Sample))
		{
			continue;
		}
		if (CheckForbidden(Index, Sample))
		{
			continue;
		}
		CheckStuck(Index, Sample);
	}
}

void UPTIssueSubsystem::PushSample(FBotState& State, const FPTActionSample& Sample) const
{
	const int32 Capacity = State.History.Num();
	if (Capacity == 0)
	{
		return;
	}
	State.History[State.HistoryHead] = Sample;
	State.HistoryHead = (State.HistoryHead + 1) % Capacity;
	State.HistoryCount = FMath::Min(State.HistoryCount + 1, Capacity);
}

const FPTActionSample* UPTIssueSubsystem::GetSampleAgo(const FBotState& State, int32 SamplesAgo) const
{
	const int32 Capacity = State.History.Num();
	if (SamplesAgo < 0 || SamplesAgo >= State.HistoryCount || Capacity == 0)
	{
		return nullptr;
	}
	const int32 SlotIndex = (State.HistoryHead - 1 - SamplesAgo + Capacity * 2) % Capacity;
	return &State.History[SlotIndex];
}

void UPTIssueSubsystem::GetHistory(const FBotState& State, TArray<FPTActionSample>& OutHistory) const
{
	OutHistory.Reset(State.HistoryCount);
	for (int32 Ago = State.HistoryCount - 1; Ago >= 0; --Ago)
	{
		OutHistory.Add(*GetSampleAgo(State, Ago)); // oldest first
	}
}

// ---------------------------------------------------------------------------
// Detectors
// ---------------------------------------------------------------------------
FVector UPTIssueSubsystem::FindFallStart(const FBotState& State) const
{
	const float FloorZ = State.LastGroundedLocation.Z;
	for (int32 Ago = 0; Ago < State.HistoryCount; ++Ago)
	{
		const FPTActionSample* Sample = GetSampleAgo(State, Ago);
		if (Sample && Sample->Location.Z >= FloorZ - 50.f)
		{
			return FVector(Sample->Location.X, Sample->Location.Y, FloorZ);
		}
	}
	return State.LastGroundedLocation;
}

bool UPTIssueSubsystem::CheckOutOfWorld(int32 BotIndex, const FPTActionSample& Now)
{
	// Lowest navmesh floor, computed once the coverage grid has its reachable cells.
	if (!bHasNavFloor)
	{
		const UPTCoverageSubsystem* Coverage = CoverageSubsystem.Get();
		if (Coverage && !Coverage->GetReachableCellArray().IsEmpty())
		{
			int32 MinCellZ = TNumericLimits<int32>::Max();
			for (const FIntVector& Cell : Coverage->GetReachableCellArray())
			{
				MinCellZ = FMath::Min(MinCellZ, Cell.Z);
			}
			LowestNavZ = MinCellZ * Coverage->CellSize;
			bHasNavFloor = true;
		}
	}

	if (bHasNavFloor && Now.Location.Z < LowestNavZ - FallMarginBelowNav)
	{
		const FVector FallStart = FindFallStart(BotStates[BotIndex]);
		ReportIssue(BotIndex, EPTIssueType::OutOfWorld, FallStart,
			FString::Printf(TEXT("Fell %.0f cm below the lowest navmesh floor; location = where the fall started"),
				LowestNavZ - Now.Location.Z));
		return true;
	}

	// Navmesh-independent: no normal jump or drop lasts this long.
	const int32 FallSamples = FMath::RoundToInt(MaxFallSeconds / SampleInterval);
	// Also require a real drop, so bots hopping against a wall (brief landings missed at 10 Hz) don't count.
	if (Now.bFalling && BotStates[BotIndex].HistoryCount > FallSamples &&
		Now.Location.Z < BotStates[BotIndex].LastGroundedLocation.Z - 200.f)
	{
		bool bFallingWholeTime = true;
		for (int32 Ago = 0; Ago < FallSamples && bFallingWholeTime; ++Ago)
		{
			const FPTActionSample* Sample = GetSampleAgo(BotStates[BotIndex], Ago);
			bFallingWholeTime = Sample && Sample->bFalling;
		}
		if (bFallingWholeTime)
		{
			ReportIssue(BotIndex, EPTIssueType::OutOfWorld, FindFallStart(BotStates[BotIndex]),
				FString::Printf(TEXT("Falling for over %.0f s and %.0f cm below the last floor; location = where the fall started"),
					MaxFallSeconds, BotStates[BotIndex].LastGroundedLocation.Z - Now.Location.Z));
			return true;
		}
	}

	const AWorldSettings* Settings = GetWorld() ? GetWorld()->GetWorldSettings() : nullptr;
	if (Settings && Settings->bEnableWorldBoundsChecks && Now.Location.Z < Settings->KillZ + 200.f)
	{
		ReportIssue(BotIndex, EPTIssueType::OutOfWorld, FindFallStart(BotStates[BotIndex]),
			TEXT("Near KillZ; location = where the fall started"));
		return true;
	}

	return false;
}

bool UPTIssueSubsystem::CheckStuck(int32 BotIndex, const FPTActionSample& Now)
{
	const FBotState& State = BotStates[BotIndex];
	const int32 WindowSamples = FMath::RoundToInt(StuckWindowSeconds / SampleInterval);

	const FPTActionSample* Old = GetSampleAgo(State, WindowSamples);
	if (!Old)
	{
		return false; // not enough history yet (e.g. just spawned or respawned)
	}

	// Every position in the window must stay close to the current one. Comparing only the window's start and
	// end flags bots that walked away and came back.
	float Displacement = 0.f;
	for (int32 Ago = 0; Ago <= WindowSamples; ++Ago)
	{
		if (const FPTActionSample* Sample = GetSampleAgo(State, Ago))
		{
			// Horizontal only: a trapped bot that keeps jumping moves up and down but goes nowhere.
			Displacement = FMath::Max(Displacement, FVector::Dist2D(Now.Location, Sample->Location));
		}
	}
	if (Displacement > StuckMaxDisplacement)
	{
		return false;
	}

	int32 IntentSamples = 0;
	int32 DirectionSamples = 0;
	FVector DirectionSum = FVector::ZeroVector;
	for (int32 Ago = 0; Ago < WindowSamples; ++Ago)
	{
		const FPTActionSample* Sample = GetSampleAgo(State, Ago);
		if (Sample && Sample->bMoveIntent)
		{
			++IntentSamples;
			const FVector Direction = Sample->MoveInput.GetSafeNormal2D();
			if (!Direction.IsNearlyZero())
			{
				DirectionSum += Direction;
				++DirectionSamples;
			}
		}
	}
	const float IntentFraction = static_cast<float>(IntentSamples) / WindowSamples;
	if (IntentFraction < StuckMinIntentFraction)
	{
		return false; // standing still on purpose
	}

	// Length of the average direction: ~1 = kept pushing the same way (e.g. into a wall), not a trap.
	const float DirectionConsistency = DirectionSamples > 0 ? DirectionSum.Size() / DirectionSamples : 1.f;
	if (DirectionConsistency > StuckMaxDirectionConsistency)
	{
		return false;
	}

	ReportIssue(BotIndex, EPTIssueType::Stuck, Now.Location,
		FString::Printf(TEXT("Stayed within %.0f cm for %.0f s while trying to move %.0f%% of the time in varied directions (consistency %.2f)"),
			Displacement, StuckWindowSeconds, IntentFraction * 100.f, DirectionConsistency));
	return true;
}

// ---------------------------------------------------------------------------
// Clip, forbidden area, perf
// ---------------------------------------------------------------------------
void UPTIssueSubsystem::GatherClipCandidates()
{
	NoCollisionMeshes.Reset();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Visible static meshes with collision disabled: anything a bot passes through here is "missing collision".
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (Actor->IsA<APawn>())
		{
			continue;
		}

		TInlineComponentArray<UStaticMeshComponent*> MeshComponents(Actor);
		for (UStaticMeshComponent* MeshComponent : MeshComponents)
		{
			const UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
			if (!Mesh || !MeshComponent->IsVisible() || MeshComponent->bHiddenInGame)
			{
				continue;
			}
			if (MeshComponent->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
			{
				continue;
			}
			if (MeshComponent->Bounds.BoxExtent.GetMax() * 2.f > IgnoreMeshesLargerThan)
			{
				continue; // sky sphere and similar
			}

			FNoCollisionMesh& Entry = NoCollisionMeshes.AddDefaulted_GetRef();
			Entry.Component = MeshComponent;
			Entry.Transform = MeshComponent->GetComponentTransform();
			Entry.LocalBox = Mesh->GetBounds().GetBox();
			Entry.Name = Actor->GetActorNameOrLabel();
		}
	}

	UE_LOG(LogPlaytestIssues, Display, TEXT("Clip detector: %d visible meshes without collision."), NoCollisionMeshes.Num());
}

void UPTIssueSubsystem::GatherForbiddenZones()
{
	ForbiddenZones.Reset();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor->ActorHasTag(TEXT("PTForbidden")))
		{
			continue;
		}

		FForbiddenZone& Zone = ForbiddenZones.AddDefaulted_GetRef();
		Zone.Actor = Actor;
		Zone.Name = Actor->GetActorNameOrLabel();

		if (const UBoxComponent* Box = Actor->FindComponentByClass<UBoxComponent>())
		{
			FTransform BoxTransform = Box->GetComponentTransform();
			BoxTransform.SetScale3D(FVector::OneVector);
			Zone.Transform = BoxTransform;
			Zone.Extent = Box->GetScaledBoxExtent();
		}
		else
		{
			// Fallback for other actors: their axis-aligned bounds.
			FVector Origin, Extent;
			Actor->GetActorBounds(/*bOnlyCollidingComponents*/ false, Origin, Extent);
			Zone.Transform = FTransform(Origin);
			Zone.Extent = Extent;
		}
	}

	UE_LOG(LogPlaytestIssues, Display, TEXT("Forbidden-area detector: %d volumes tagged PTForbidden."), ForbiddenZones.Num());
}

bool UPTIssueSubsystem::CheckClip(int32 BotIndex, const FPTActionSample& Prev, const FPTActionSample& Now)
{
	const float StepDistance = FVector::Dist(Prev.Location, Now.Location);
	if (StepDistance < 1.f || StepDistance > ClipMaxStepDistance)
	{
		return false; // not moving, or teleported
	}

	// 1) Missing collision: the bot's path since the last sample crossed a visible mesh that has no collision.
	for (const FNoCollisionMesh& Entry : NoCollisionMeshes)
	{
		if (!Entry.Component.IsValid())
		{
			continue;
		}
		const FVector LocalStart = Entry.Transform.InverseTransformPosition(Prev.Location);
		const FVector LocalEnd = Entry.Transform.InverseTransformPosition(Now.Location);
		const FVector LocalDirection = (LocalEnd - LocalStart).GetSafeNormal();
		if (Entry.LocalBox.IsInside(LocalEnd) ||
			FMath::LineBoxIntersection(Entry.LocalBox, LocalStart, LocalEnd, LocalDirection))
		{
			ReportIssue(BotIndex, EPTIssueType::Clip, Now.Location,
				FString::Printf(TEXT("Passed through visible mesh without collision: %s"), *Entry.Name));
			return true;
		}
	}

	// 2) Tunnelling: the bot's path crossed geometry that DOES have collision (it should have been blocked).
	UWorld* World = GetWorld();
	const APawn* Bot = BotStates[BotIndex].Pawn.Get();
	if (World && Bot)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PTClipTrace), /*bTraceComplex*/ false, Bot);
		FHitResult Hit;
		if (World->LineTraceSingleByObjectType(Hit, Prev.Location, Now.Location, FCollisionObjectQueryParams(ECC_WorldStatic), Params))
		{
			const AActor* HitActor = Hit.GetActor();
			ReportIssue(BotIndex, EPTIssueType::Clip, Hit.ImpactPoint,
				FString::Printf(TEXT("Moved through blocking geometry: %s"), HitActor ? *HitActor->GetActorNameOrLabel() : TEXT("unknown")));
			return true;
		}
	}

	return false;
}

bool UPTIssueSubsystem::CheckForbidden(int32 BotIndex, const FPTActionSample& Now)
{
	for (const FForbiddenZone& Zone : ForbiddenZones)
	{
		const FVector Local = Zone.Transform.InverseTransformPosition(Now.Location);
		// The volume marks a forbidden footprint: anything above its bottom counts (bots standing, jumping or perched higher up).
		if (FMath::Abs(Local.X) <= Zone.Extent.X && FMath::Abs(Local.Y) <= Zone.Extent.Y && Local.Z >= -Zone.Extent.Z)
		{
			ReportIssue(BotIndex, EPTIssueType::ForbiddenArea, Now.Location,
				FString::Printf(TEXT("Entered forbidden volume: %s"), *Zone.Name));
			return true;
		}
	}
	return false;
}

void UPTIssueSubsystem::TickViewCycling(float DeltaTime)
{
	const float Interval = CVarPTViewBots.GetValueOnGameThread();
	if (Interval <= 0.f || BotStates.IsEmpty())
	{
		return;
	}

	ViewSwitchTimer -= DeltaTime;
	if (ViewSwitchTimer > 0.f)
	{
		return;
	}
	ViewSwitchTimer = Interval;

	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		return;
	}

	// Next bot that still exists.
	for (int32 Attempt = 0; Attempt < BotStates.Num(); ++Attempt)
	{
		ViewBotIndex = (ViewBotIndex + 1) % BotStates.Num();
		if (APawn* Bot = BotStates[ViewBotIndex].Pawn.Get())
		{
			PlayerController->SetViewTarget(Bot);
			return;
		}
	}
}

void UPTIssueSubsystem::TickPerf()
{
	if (!FApp::CanEverRender())
	{
		return; // headless (-nullrhi) runs have no meaningful frame time
	}
	if (const UPTPlaytestSubsystem* TrainingRun = RunSubsystem.Get())
	{
		if (TrainingRun->GetConfig().bTrain)
		{
			return; // training updates stall the game on purpose; those are not level perf problems
		}
	}

	PerfWindowTime += FApp::GetDeltaTime();
	++PerfWindowFrames;
	if (PerfWindowTime < PerfWindowSeconds)
	{
		return;
	}

	const float WindowMs = static_cast<float>(1000.0 * PerfWindowTime / PerfWindowFrames);
	PerfWindowTime = 0.0;
	PerfWindowFrames = 0;

	const UPTPlaytestSubsystem* Run = RunSubsystem.Get();
	const float Now = Run ? Run->GetElapsedSeconds() : 0.f;
	if (Now < PerfWarmupSeconds)
	{
		return; // shader compiles and streaming at start
	}

	// Baseline = median of the last ~30 s. A sustained slower frame rate becomes the new normal;
	// only short jumps above it count as spikes.
	const int32 MaxRecent = FMath::Max(1, FMath::RoundToInt(30.f / PerfWindowSeconds));
	const bool bHasBaseline = PerfRecentMs.Num() >= 10;
	if (bHasBaseline)
	{
		TArray<float> Sorted = PerfRecentMs;
		Sorted.Sort();
		PerfBaselineMs = Sorted[Sorted.Num() / 2];
	}
	PerfRecentMs.Add(WindowMs);
	if (PerfRecentMs.Num() > MaxRecent)
	{
		PerfRecentMs.RemoveAt(0);
	}
	if (!bHasBaseline)
	{
		return;
	}

	const bool bSpike = WindowMs > PerfBaselineMs * PerfSpikeFactor && WindowMs > PerfBaselineMs + PerfMinSpikeMs;
	if (!bSpike)
	{
		return;
	}
	if (Now < PerfCooldownUntil)
	{
		return;
	}
	PerfCooldownUntil = Now + PerfCooldownSeconds;

	// Perf depends on what is being rendered: report where the active camera is and where it looks.
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	if (const APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (PlayerController->PlayerCameraManager)
		{
			ViewLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
			ViewRotation = PlayerController->PlayerCameraManager->GetCameraRotation();
		}
	}

	// Report the point the camera is looking at (where the expensive content most likely is),
	// and keep the camera position in the detail text.
	FVector LookAt = ViewLocation;
	if (UWorld* World = GetWorld())
	{
		FHitResult Hit;
		const FVector TraceEnd = ViewLocation + ViewRotation.Vector() * 20000.f;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PTPerfLookAt), /*bTraceComplex*/ false);
		if (World->LineTraceSingleByChannel(Hit, ViewLocation, TraceEnd, ECC_Visibility, Params))
		{
			LookAt = Hit.ImpactPoint;
		}
	}

	const int32 ViewedBot = (CVarPTViewBots.GetValueOnGameThread() > 0.f) ? ViewBotIndex : INDEX_NONE;
	ReportIssue(ViewedBot, EPTIssueType::PerfSpike, LookAt,
		FString::Printf(TEXT("Frame time %.1f ms vs baseline %.1f ms; location = camera look-at point; camera at (%.0f, %.0f, %.0f) P=%.0f Y=%.0f"),
			WindowMs, PerfBaselineMs, ViewLocation.X, ViewLocation.Y, ViewLocation.Z, ViewRotation.Pitch, ViewRotation.Yaw),
		/*bAffectsBot*/ false);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
void UPTIssueSubsystem::ReportIssue(int32 BotIndex, EPTIssueType Type, const FVector& Location, const FString& Detail, bool bAffectsBot)
{
	const UPTPlaytestSubsystem* Run = RunSubsystem.Get();

	if (!BotStates.IsValidIndex(BotIndex))
	{
		// Not tied to a bot (e.g. perf spike seen from the player's camera).
		FPTIssueRecord& Issue = Issues.AddDefaulted_GetRef();
		Issue.Type = Type;
		Issue.Location = Location;
		Issue.Time = Run ? Run->GetElapsedSeconds() : 0.f;
		Issue.BotIndex = INDEX_NONE;
		Issue.Detail = Detail;
		if (Run)
		{
			Issue.Seed = Run->GetConfig().Seed;
			Issue.Walker = UPTPlaytestSubsystem::WalkerToString(Run->GetConfig().Walker);
		}
		UE_LOG(LogPlaytestIssues, Warning, TEXT("ISSUE %s  (view)  t=%.1fs  at (%.0f, %.0f, %.0f)  %s"),
			*IssueTypeToString(Type), Issue.Time, Location.X, Location.Y, Location.Z, *Detail);
		return;
	}

	FBotState& State = BotStates[BotIndex];

	FPTIssueRecord& Issue = Issues.AddDefaulted_GetRef();
	Issue.Type = Type;
	Issue.Location = Location;
	Issue.Time = Run ? Run->GetElapsedSeconds() : 0.f;
	Issue.BotIndex = BotIndex;
	Issue.Detail = Detail;
	Issue.SpawnTransform = State.SpawnTransform;
	if (Run)
	{
		Issue.Seed = Run->GetConfig().Seed;
		Issue.Walker = UPTPlaytestSubsystem::WalkerToString(Run->GetConfig().Walker);
	}
	if (const APawn* Bot = State.Pawn.Get())
	{
		Issue.Rotation = Bot->GetActorRotation();
	}
	GetHistory(State, Issue.Actions);

	UE_LOG(LogPlaytestIssues, Warning, TEXT("ISSUE %s  bot=%d  t=%.1fs  at (%.0f, %.0f, %.0f)  %s"),
		*IssueTypeToString(Type), BotIndex, Issue.Time, Location.X, Location.Y, Location.Z, *Detail);

	if (!bAffectsBot)
	{
		return;
	}

	State.CooldownUntil = Issue.Time + IssueCooldownSeconds;

	// In training the learning environment ends the episode and restarts the bot instead.
	const bool bTraining = Run && Run->GetConfig().bTrain;
	if (bRespawnAfterIssue && !bTraining)
	{
		RespawnBot(State);
	}
}

void UPTIssueSubsystem::NotifyBotTeleported(const APawn* Bot)
{
	for (FBotState& State : BotStates)
	{
		if (State.Pawn.Get() == Bot)
		{
			State.HistoryCount = 0;
			State.HistoryHead = 0;
			State.LastGroundedLocation = Bot->GetActorLocation();
			State.LastKnownLocation = Bot->GetActorLocation();
			return;
		}
	}
}

void UPTIssueSubsystem::RespawnBot(FBotState& State) const
{
	APawn* Bot = State.Pawn.Get();
	if (!Bot)
	{
		return;
	}

	Bot->SetActorLocationAndRotation(State.SpawnTransform.GetLocation(), State.SpawnTransform.GetRotation().Rotator(),
		/*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);

	if (const ACharacter* BotCharacter = Cast<ACharacter>(Bot))
	{
		if (UCharacterMovementComponent* Movement = BotCharacter->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
		}
	}
	if (AAIController* BotAI = Cast<AAIController>(Bot->GetController()))
	{
		BotAI->StopMovement(); // frontier walker picks a new target from the spawn
	}

	// Don't compare positions across the teleport.
	State.HistoryCount = 0;
	State.HistoryHead = 0;
	State.LastGroundedLocation = State.SpawnTransform.GetLocation();
}

void UPTIssueSubsystem::WriteReport() const
{
	const UPTPlaytestSubsystem* Run = RunSubsystem.Get();
	if (!Run)
	{
		return;
	}

	const FString Dir = Run->GetRunOutputDir();
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("runId"), Run->GetRunId());
	Root->SetStringField(TEXT("config"), Run->GetConfig().ToString());
	Root->SetNumberField(TEXT("issueCount"), Issues.Num());

	// Clusters: same type within the same ClusterSize grid cell = one bug.
	struct FCluster
	{
		EPTIssueType Type = EPTIssueType::Stuck;
		int32 Count = 0;
		TSet<int32> Bots;
		FVector LocationSum = FVector::ZeroVector;
		int32 FirstIssueIndex = INDEX_NONE;
	};
	TMap<FString, FCluster> Clusters;
	// Full records (with 10 s of action history each) only for the first few issues of every cluster:
	// a long training run produces tens of thousands of repeats, which made report.json exceed 2 GB.
	constexpr int32 MaxRecordsPerCluster = 5;
	TArray<int32> KeptIssueIndices;
	for (int32 IssueIndex = 0; IssueIndex < Issues.Num(); ++IssueIndex)
	{
		const FPTIssueRecord& Issue = Issues[IssueIndex];
		const FIntVector Bucket(
			FMath::FloorToInt(Issue.Location.X / ClusterSize),
			FMath::FloorToInt(Issue.Location.Y / ClusterSize),
			FMath::FloorToInt(Issue.Location.Z / ClusterSize));
		const FString Key = FString::Printf(TEXT("%d_%d_%d_%d"), static_cast<int32>(Issue.Type), Bucket.X, Bucket.Y, Bucket.Z);

		FCluster& Cluster = Clusters.FindOrAdd(Key);
		if (Cluster.Count == 0)
		{
			Cluster.Type = Issue.Type;
			Cluster.FirstIssueIndex = IssueIndex;
		}
		++Cluster.Count;
		if (Cluster.Count <= MaxRecordsPerCluster)
		{
			KeptIssueIndices.Add(IssueIndex);
		}
		Cluster.Bots.Add(Issue.BotIndex);
		Cluster.LocationSum += Issue.Location;
	}

	TArray<FCluster> SortedClusters;
	Clusters.GenerateValueArray(SortedClusters);
	SortedClusters.Sort([](const FCluster& A, const FCluster& B) { return A.Count > B.Count; });

	TArray<TSharedPtr<FJsonValue>> ClustersJson;
	for (const FCluster& Cluster : SortedClusters)
	{
		const FVector Center = Cluster.LocationSum / Cluster.Count;
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("type"), IssueTypeToString(Cluster.Type));
		Entry->SetNumberField(TEXT("count"), Cluster.Count);
		Entry->SetNumberField(TEXT("distinctBots"), Cluster.Bots.Num());
		Entry->SetArrayField(TEXT("location"), {
			MakeShared<FJsonValueNumber>(FMath::RoundToInt(Center.X)),
			MakeShared<FJsonValueNumber>(FMath::RoundToInt(Center.Y)),
			MakeShared<FJsonValueNumber>(FMath::RoundToInt(Center.Z)) });
		Entry->SetNumberField(TEXT("firstIssueIndex"), Cluster.FirstIssueIndex);
		ClustersJson.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Root->SetNumberField(TEXT("clusterCount"), SortedClusters.Num());
	Root->SetArrayField(TEXT("clusters"), ClustersJson);

	// Full records (with action history) for replay later, capped per cluster.
	TArray<TSharedPtr<FJsonValue>> IssuesJson;
	for (const int32 IssueIndex : KeptIssueIndices)
	{
		if (TSharedPtr<FJsonObject> IssueObject = FJsonObjectConverter::UStructToJsonObject(Issues[IssueIndex]))
		{
			IssueObject->SetNumberField(TEXT("issueIndex"), IssueIndex);
			IssuesJson.Add(MakeShared<FJsonValueObject>(IssueObject));
		}
	}
	Root->SetNumberField(TEXT("recordsWritten"), IssuesJson.Num());
	Root->SetArrayField(TEXT("issues"), IssuesJson);

	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root, Writer);

	const FString Path = FPaths::Combine(Dir, TEXT("report.json"));
	if (FFileHelper::SaveStringToFile(Output, *Path))
	{
		UE_LOG(LogPlaytestIssues, Display, TEXT("Wrote %s (%d issues in %d clusters)"),
			*FPaths::ConvertRelativePathToFull(Path), Issues.Num(), SortedClusters.Num());
	}
	else
	{
		UE_LOG(LogPlaytestIssues, Error, TEXT("Failed to write %s"), *Path);
	}
}

void UPTIssueSubsystem::DrawDebug() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (const FPTIssueRecord& Issue : Issues)
	{
		FColor Color = FColor::White;
		switch (Issue.Type)
		{
		case EPTIssueType::Stuck:         Color = FColor::Orange; break;
		case EPTIssueType::OutOfWorld:    Color = FColor::Red;    break;
		case EPTIssueType::Clip:          Color = FColor::Cyan;   break;
		case EPTIssueType::ForbiddenArea: Color = FColor::Purple; break;
		case EPTIssueType::PerfSpike:     Color = FColor::Yellow; break;
		default: break;
		}
		DrawDebugSphere(World, Issue.Location, 40.f, 8, Color, false, -1.f, 0, 2.f);
	}
}
