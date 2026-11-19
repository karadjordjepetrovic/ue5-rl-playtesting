// PTCoverageSubsystem.h
// Records which cells the bots visit and compares that against the reachable navmesh area.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Coverage/PTCoverageSubsystem.h

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PTCoverageSubsystem.generated.h"

struct FPTRunConfig;
class UPTPlaytestSubsystem;

UCLASS()
class PLAYTESTBOTS_API UPTCoverageSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem / UTickableWorldSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// Cell helpers (also used later by the frontier walker and the RL observations)
	FIntVector LocationToCell(const FVector& Location) const;
	FVector CellToLocation(const FIntVector& Cell) const;
	int32 GetVisitCount(const FIntVector& Cell) const;
	bool IsReachable(const FIntVector& Cell) const { return ReachableCells.Contains(Cell); }

	const TMap<FIntVector, int32>& GetVisits() const { return Visits; }
	const TSet<FIntVector>& GetReachableCells() const { return ReachableCells; }
	const TArray<FIntVector>& GetReachableCellArray() const { return ReachableCellArray; } // sorted, for random sampling
	int32 GetVisitedReachableCount() const { return VisitedReachable; }
	float GetCoveragePercent() const;

	// Settings (starting values, tune later)
	float CellSize = 100.f;          // cm
	float SampleInterval = 0.1f;     // s between position samples (10 Hz)
	float TimelineInterval = 10.f;   // s between coverage log lines / timeline points
	int32 ReachableSamples = 30000;  // random navmesh samples used to estimate the reachable area
	float ReachableRadius = 50000.f; // cm around the first PlayerStart

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FTimelinePoint
	{
		float Time = 0.f;
		int32 Visited = 0;
		int32 VisitedReachable = 0;
		float CoveragePct = 0.f;
	};

	void HandleRunStarted(const FPTRunConfig& Config);
	void HandleRunFinished(const FPTRunConfig& Config);

	void ResetCoverage();
	void SampleReachableCells();
	void SampleBots();
	void RecordTimelinePoint();
	void WriteCoverageJson() const;
	void DrawDebug() const;

	TWeakObjectPtr<UPTPlaytestSubsystem> RunSubsystem;

	TMap<FIntVector, int32> Visits;   // cell -> number of samples a bot was in it
	TSet<FIntVector> ReachableCells;  // cells the navmesh says are reachable from the start
	TArray<FIntVector> ReachableCellArray;
	int32 VisitedReachable = 0;
	TArray<FTimelinePoint> Timeline;

	float SampleAccumulator = 0.f;
	float TimelineAccumulator = 0.f;
	bool bActive = false;
};
