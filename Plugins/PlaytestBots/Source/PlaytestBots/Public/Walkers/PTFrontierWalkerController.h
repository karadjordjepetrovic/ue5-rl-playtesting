// PTFrontierWalkerController.h
// Baseline bot #2 ("smart scripted"): repeatedly walks on the navmesh to a nearby, rarely visited
// reachable cell, avoiding cells other frontier bots are already heading to.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Walkers/PTFrontierWalkerController.h

#pragma once

#include "CoreMinimal.h"
#include "Walkers/PTBotControllerBase.h"
#include "PTFrontierWalkerController.generated.h"

UCLASS()
class PLAYTESTBOTS_API APTFrontierWalkerController : public APTBotControllerBase
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result) override;

	bool HasTarget() const { return bHasTarget; }
	FIntVector GetTargetCell() const { return TargetCell; }

	// Tuning (starting values)
	UPROPERTY(EditAnywhere, Category = "Frontier Walker")
	int32 CandidateCount = 256;        // random reachable cells scored per decision

	UPROPERTY(EditAnywhere, Category = "Frontier Walker")
	float VisitWeight = 5.f;           // score per log2(1 + visits): prefers unvisited cells

	UPROPERTY(EditAnywhere, Category = "Frontier Walker")
	float DistanceWeight = 0.2f;       // score per metre: prefers nearby cells

	UPROPERTY(EditAnywhere, Category = "Frontier Walker")
	float ClaimRadius = 500.f;         // cm; other bots' targets inside this radius are penalised

	UPROPERTY(EditAnywhere, Category = "Frontier Walker")
	float ClaimPenalty = 20.f;

	UPROPERTY(EditAnywhere, Category = "Frontier Walker")
	float MinTargetDistance = 150.f;   // cm; ignore cells right next to the bot

	UPROPERTY(EditAnywhere, Category = "Frontier Walker")
	float AcceptanceRadius = 50.f;     // cm

	UPROPERTY(EditAnywhere, Category = "Frontier Walker")
	float MaxMoveTime = 20.f;          // s before giving up on a target

	UPROPERTY(EditAnywhere, Category = "Frontier Walker")
	float JumpChancePerSecond = 0.1f;  // occasional jumps while walking

private:
	bool TryPickTarget();

	bool bHasTarget = false;
	FIntVector TargetCell = FIntVector::ZeroValue;
	float MoveTimeLeft = 0.f;
	float JumpTimeLeft = 0.f;
	TSet<FIntVector> FailedCells;     // cells pathfinding could not reach from here
};
