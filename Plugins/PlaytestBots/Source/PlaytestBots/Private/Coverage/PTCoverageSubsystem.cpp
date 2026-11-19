// PTCoverageSubsystem.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Coverage/PTCoverageSubsystem.cpp

#include "Coverage/PTCoverageSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NavigationSystem.h"
#include "PTPlaytestSubsystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlaytestCoverage, Log, All);

static TAutoConsoleVariable<int32> CVarPTDebug(
	TEXT("pt.Debug"),
	0,
	TEXT("Playtest debug draw. 0 = off, 1 = visited cells, 2 = visited + unvisited reachable cells."));

bool UPTCoverageSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UPTCoverageSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UPTPlaytestSubsystem* Run = Collection.InitializeDependency<UPTPlaytestSubsystem>();
	if (Run)
	{
		RunSubsystem = Run;
		Run->OnRunStarted.AddUObject(this, &UPTCoverageSubsystem::HandleRunStarted);
		Run->OnRunFinished.AddUObject(this, &UPTCoverageSubsystem::HandleRunFinished);
	}
}

void UPTCoverageSubsystem::Deinitialize()
{
	if (UPTPlaytestSubsystem* Run = RunSubsystem.Get())
	{
		Run->OnRunStarted.RemoveAll(this);
		Run->OnRunFinished.RemoveAll(this);
	}
	Super::Deinitialize();
}

TStatId UPTCoverageSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPTCoverageSubsystem, STATGROUP_Tickables);
}

void UPTCoverageSubsystem::Tick(float DeltaTime)
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

		TimelineAccumulator += DeltaTime;
		if (TimelineAccumulator >= TimelineInterval)
		{
			TimelineAccumulator -= TimelineInterval;
			RecordTimelinePoint();
		}
	}

	// Drawing also works after the run ends, so you can inspect the result in PIE.
	if (CVarPTDebug.GetValueOnGameThread() > 0)
	{
		DrawDebug();
	}
}

// ---------------------------------------------------------------------------
// Cells
// ---------------------------------------------------------------------------
FIntVector UPTCoverageSubsystem::LocationToCell(const FVector& Location) const
{
	// Z is offset by half a cell so points lying on a floor don't flicker between two cells.
	return FIntVector(
		FMath::FloorToInt(Location.X / CellSize),
		FMath::FloorToInt(Location.Y / CellSize),
		FMath::FloorToInt((Location.Z + CellSize * 0.5f) / CellSize));
}

FVector UPTCoverageSubsystem::CellToLocation(const FIntVector& Cell) const
{
	return FVector((Cell.X + 0.5f) * CellSize, (Cell.Y + 0.5f) * CellSize, Cell.Z * CellSize);
}

int32 UPTCoverageSubsystem::GetVisitCount(const FIntVector& Cell) const
{
	const int32* Count = Visits.Find(Cell);
	return Count ? *Count : 0;
}

float UPTCoverageSubsystem::GetCoveragePercent() const
{
	return ReachableCells.Num() > 0 ? 100.f * VisitedReachable / ReachableCells.Num() : 0.f;
}

// ---------------------------------------------------------------------------
// Run lifecycle
// ---------------------------------------------------------------------------
void UPTCoverageSubsystem::HandleRunStarted(const FPTRunConfig& Config)
{
	ResetCoverage();
	SampleReachableCells();
	bActive = true;

	SampleBots();
	RecordTimelinePoint();
}

void UPTCoverageSubsystem::HandleRunFinished(const FPTRunConfig& Config)
{
	if (!bActive)
	{
		return;
	}

	SampleBots();
	// Avoid a duplicate final point when the run ends exactly on a timeline tick.
	const UPTPlaytestSubsystem* Run = RunSubsystem.Get();
	const float Now = Run ? Run->GetElapsedSeconds() : 0.f;
	if (Timeline.IsEmpty() || Now - Timeline.Last().Time > 0.5f)
	{
		RecordTimelinePoint();
	}
	bActive = false;

	UE_LOG(LogPlaytestCoverage, Display, TEXT("Final coverage: %d cells visited, %d/%d reachable (%.1f%%), %d visited cells outside the navmesh."),
		Visits.Num(), VisitedReachable, ReachableCells.Num(), GetCoveragePercent(), Visits.Num() - VisitedReachable);

	WriteCoverageJson();
}

void UPTCoverageSubsystem::ResetCoverage()
{
	Visits.Reset();
	ReachableCells.Reset();
	VisitedReachable = 0;
	Timeline.Reset();
	SampleAccumulator = 0.f;
	TimelineAccumulator = 0.f;
}

void UPTCoverageSubsystem::SampleReachableCells()
{
	UWorld* World = GetWorld();
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys || !NavSys->GetDefaultNavDataInstance())
	{
		UE_LOG(LogPlaytestCoverage, Warning, TEXT("No navmesh in this map: coverage %% will be 0. Add a NavMeshBoundsVolume covering the playable area."));
		return;
	}

	// Start from the first PlayerStart, projected onto the navmesh.
	FVector Origin = FVector::ZeroVector;
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		Origin = It->GetActorLocation();
		break;
	}

	FNavLocation Projected;
	if (!NavSys->ProjectPointToNavigation(Origin, Projected, FVector(500.f, 500.f, 1000.f)))
	{
		UE_LOG(LogPlaytestCoverage, Warning, TEXT("PlayerStart is not near the navmesh: coverage %% will be unreliable."));
		return;
	}
	Origin = Projected.Location;

	// "Reachable" = connected to the start on the navmesh. Areas bots reach that are NOT in this set
	// are interesting: sealed rooms, places you can only get to by jumping or by a collision bug.
	const double StartTime = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < ReachableSamples; ++Index)
	{
		FNavLocation Point;
		if (NavSys->GetRandomReachablePointInRadius(Origin, ReachableRadius, Point))
		{
			ReachableCells.Add(LocationToCell(Point.Location));
		}
	}

	UE_LOG(LogPlaytestCoverage, Display, TEXT("Reachable area: %d cells of %.0f cm (%d samples, %.0f ms)."),
		ReachableCells.Num(), CellSize, ReachableSamples, (FPlatformTime::Seconds() - StartTime) * 1000.0);
}

void UPTCoverageSubsystem::SampleBots()
{
	const UPTPlaytestSubsystem* Run = RunSubsystem.Get();
	if (!Run)
	{
		return;
	}

	for (const TWeakObjectPtr<APawn>& BotPtr : Run->GetBots())
	{
		const APawn* Bot = BotPtr.Get();
		if (!Bot)
		{
			continue; // destroyed, e.g. fell below KillZ
		}

		// Feet location for characters, so it lines up with navmesh points.
		const FIntVector Cell = LocationToCell(Bot->GetNavAgentLocation());
		int32& Count = Visits.FindOrAdd(Cell);
		if (Count == 0 && ReachableCells.Contains(Cell))
		{
			++VisitedReachable;
		}
		++Count;
	}
}

void UPTCoverageSubsystem::RecordTimelinePoint()
{
	const UPTPlaytestSubsystem* Run = RunSubsystem.Get();

	FTimelinePoint Point;
	Point.Time = Run ? Run->GetElapsedSeconds() : 0.f;
	Point.Visited = Visits.Num();
	Point.VisitedReachable = VisitedReachable;
	Point.CoveragePct = GetCoveragePercent();
	Timeline.Add(Point);

	UE_LOG(LogPlaytestCoverage, Display, TEXT("t=%4.0fs  visited=%d  coverage=%.1f%%  outsideNav=%d"),
		Point.Time, Point.Visited, Point.CoveragePct, Point.Visited - Point.VisitedReachable);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
void UPTCoverageSubsystem::WriteCoverageJson() const
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
	Root->SetNumberField(TEXT("cellSizeCm"), CellSize);
	Root->SetNumberField(TEXT("reachableCells"), ReachableCells.Num());
	Root->SetNumberField(TEXT("visitedCells"), Visits.Num());
	Root->SetNumberField(TEXT("visitedReachableCells"), VisitedReachable);
	Root->SetNumberField(TEXT("visitedOutsideNavCells"), Visits.Num() - VisitedReachable);
	Root->SetNumberField(TEXT("coveragePct"), GetCoveragePercent());

	TArray<TSharedPtr<FJsonValue>> TimelineJson;
	for (const FTimelinePoint& Point : Timeline)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("t"), Point.Time);
		Entry->SetNumberField(TEXT("visited"), Point.Visited);
		Entry->SetNumberField(TEXT("visitedReachable"), Point.VisitedReachable);
		Entry->SetNumberField(TEXT("coveragePct"), Point.CoveragePct);
		TimelineJson.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Root->SetArrayField(TEXT("timeline"), TimelineJson);

	// Each visited cell as [x, y, z, visits, reachable(0/1)] in cell coordinates.
	TArray<TSharedPtr<FJsonValue>> CellsJson;
	CellsJson.Reserve(Visits.Num());
	for (const TPair<FIntVector, int32>& Pair : Visits)
	{
		TArray<TSharedPtr<FJsonValue>> CellJson;
		CellJson.Add(MakeShared<FJsonValueNumber>(Pair.Key.X));
		CellJson.Add(MakeShared<FJsonValueNumber>(Pair.Key.Y));
		CellJson.Add(MakeShared<FJsonValueNumber>(Pair.Key.Z));
		CellJson.Add(MakeShared<FJsonValueNumber>(Pair.Value));
		CellJson.Add(MakeShared<FJsonValueNumber>(ReachableCells.Contains(Pair.Key) ? 1 : 0));
		CellsJson.Add(MakeShared<FJsonValueArray>(CellJson));
	}
	Root->SetArrayField(TEXT("cells"), CellsJson);

	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root, Writer);

	const FString Path = FPaths::Combine(Dir, TEXT("coverage.json"));
	if (FFileHelper::SaveStringToFile(Output, *Path))
	{
		UE_LOG(LogPlaytestCoverage, Display, TEXT("Wrote %s"), *FPaths::ConvertRelativePathToFull(Path));
	}
	else
	{
		UE_LOG(LogPlaytestCoverage, Error, TEXT("Failed to write %s"), *Path);
	}
}

void UPTCoverageSubsystem::DrawDebug() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	int32 MaxVisits = 1;
	for (const TPair<FIntVector, int32>& Pair : Visits)
	{
		MaxVisits = FMath::Max(MaxVisits, Pair.Value);
	}

	const FVector Lift(0.f, 0.f, 10.f);

	// Visited cells: green (rarely visited) -> red (heavily visited). Magenta = outside the navmesh.
	for (const TPair<FIntVector, int32>& Pair : Visits)
	{
		FColor Color;
		if (!ReachableCells.IsEmpty() && !ReachableCells.Contains(Pair.Key))
		{
			Color = FColor::Magenta;
		}
		else
		{
			const float Alpha = FMath::Sqrt(static_cast<float>(Pair.Value) / MaxVisits);
			Color = FLinearColor::LerpUsingHSV(FLinearColor::Green, FLinearColor::Red, Alpha).ToFColor(true);
		}
		DrawDebugPoint(World, CellToLocation(Pair.Key) + Lift, 8.f, Color, false, -1.f);
	}

	// Mode 2: also show reachable cells nobody has visited yet.
	if (CVarPTDebug.GetValueOnGameThread() >= 2)
	{
		for (const FIntVector& Cell : ReachableCells)
		{
			if (!Visits.Contains(Cell))
			{
				DrawDebugPoint(World, CellToLocation(Cell) + Lift, 4.f, FColor(128, 128, 128), false, -1.f);
			}
		}
	}
}
