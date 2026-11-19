// PTIssueSubsystem.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Detectors/PTIssueSubsystem.cpp

#include "Detectors/PTIssueSubsystem.h"

#include "AIController.h"
#include "Coverage/PTCoverageSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Navigation/PathFollowingComponent.h"
#include "PTPlaytestSubsystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlaytestIssues, Log, All);

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
				ReportIssue(Index, EPTIssueType::OutOfWorld, State.LastKnownLocation,
					TEXT("Bot was destroyed (most likely fell below KillZ)"));
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

		PushSample(State, Sample);
		State.LastKnownLocation = Sample.Location;

		if (Now < State.CooldownUntil)
		{
			continue;
		}

		// Out of world first: a falling bot is not "stuck".
		if (!CheckOutOfWorld(Index, Sample))
		{
			CheckStuck(Index, Sample);
		}
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
		ReportIssue(BotIndex, EPTIssueType::OutOfWorld, Now.Location,
			FString::Printf(TEXT("%.0f cm below the lowest navmesh floor"), LowestNavZ - Now.Location.Z));
		return true;
	}

	const AWorldSettings* Settings = GetWorld() ? GetWorld()->GetWorldSettings() : nullptr;
	if (Settings && Settings->bEnableWorldBoundsChecks && Now.Location.Z < Settings->KillZ + 200.f)
	{
		ReportIssue(BotIndex, EPTIssueType::OutOfWorld, Now.Location, TEXT("Near KillZ"));
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

	const float Displacement = FVector::Dist(Now.Location, Old->Location);
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
		FString::Printf(TEXT("Moved %.0f cm in %.0f s while trying to move %.0f%% of the time in varied directions (consistency %.2f)"),
			Displacement, StuckWindowSeconds, IntentFraction * 100.f, DirectionConsistency));
	return true;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
void UPTIssueSubsystem::ReportIssue(int32 BotIndex, EPTIssueType Type, const FVector& Location, const FString& Detail)
{
	if (!BotStates.IsValidIndex(BotIndex))
	{
		return;
	}
	FBotState& State = BotStates[BotIndex];
	const UPTPlaytestSubsystem* Run = RunSubsystem.Get();

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

	State.CooldownUntil = Issue.Time + IssueCooldownSeconds;

	if (bRespawnAfterIssue)
	{
		RespawnBot(State);
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

	// Full records (with action history) for replay later.
	TArray<TSharedPtr<FJsonValue>> IssuesJson;
	for (const FPTIssueRecord& Issue : Issues)
	{
		if (TSharedPtr<FJsonObject> IssueObject = FJsonObjectConverter::UStructToJsonObject(Issue))
		{
			IssuesJson.Add(MakeShared<FJsonValueObject>(IssueObject));
		}
	}
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
