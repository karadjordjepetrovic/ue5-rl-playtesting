// PTFrontierWalkerController.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Walkers/PTFrontierWalkerController.cpp

#include "Walkers/PTFrontierWalkerController.h"

#include "Coverage/PTCoverageSubsystem.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "Navigation/PathFollowingComponent.h"

void APTFrontierWalkerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	ACharacter* BotCharacter = Cast<ACharacter>(GetPawn());
	if (!BotCharacter)
	{
		return;
	}

	if (!bHasTarget)
	{
		// Retried every tick until the coverage grid has its reachable cells (ready right after run start).
		TryPickTarget();
	}
	else
	{
		MoveTimeLeft -= DeltaSeconds;
		if (MoveTimeLeft <= 0.f)
		{
			// Taking too long (blocked, circling): give up. StopMovement triggers OnMoveCompleted.
			StopMovement();
			bHasTarget = false;
		}
	}

	// Occasional jumps, so this baseline is not purely path-following.
	if (JumpTimeLeft > 0.f)
	{
		JumpTimeLeft -= DeltaSeconds;
		if (JumpTimeLeft <= 0.f)
		{
			BotCharacter->StopJumping();
		}
	}
	else if (Random.FRand() < JumpChancePerSecond * DeltaSeconds)
	{
		BotCharacter->Jump();
		JumpTimeLeft = Random.FRandRange(0.05f, 0.3f);
	}
}

void APTFrontierWalkerController::OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result)
{
	Super::OnMoveCompleted(RequestID, Result);

	if (bHasTarget && Result.Code != EPathFollowingResult::Success)
	{
		// Blocked or invalid path: don't keep choosing the same cell.
		FailedCells.Add(TargetCell);
	}
	bHasTarget = false;
}

bool APTFrontierWalkerController::TryPickTarget()
{
	UWorld* World = GetWorld();
	const APawn* BotPawn = GetPawn();
	const UPTCoverageSubsystem* Coverage = World ? World->GetSubsystem<UPTCoverageSubsystem>() : nullptr;
	if (!BotPawn || !Coverage)
	{
		return false;
	}

	const TArray<FIntVector>& Cells = Coverage->GetReachableCellArray();
	if (Cells.IsEmpty())
	{
		return false;
	}

	const FVector Here = BotPawn->GetNavAgentLocation();

	// Where the other frontier bots are already heading.
	TArray<FVector> OtherTargets;
	for (TActorIterator<APTFrontierWalkerController> It(World); It; ++It)
	{
		if (*It != this && It->HasTarget())
		{
			OtherTargets.Add(Coverage->CellToLocation(It->GetTargetCell()));
		}
	}

	// Score a random sample of reachable cells; lowest score wins.
	bool bFound = false;
	FIntVector BestCell = FIntVector::ZeroValue;
	float BestScore = TNumericLimits<float>::Max();
	const float ClaimRadiusSq = ClaimRadius * ClaimRadius;

	for (int32 Sample = 0; Sample < CandidateCount; ++Sample)
	{
		const FIntVector& Cell = Cells[Random.RandHelper(Cells.Num())];
		if (FailedCells.Contains(Cell))
		{
			continue;
		}

		const FVector CellLocation = Coverage->CellToLocation(Cell);
		const float DistanceCm = FVector::Dist(Here, CellLocation);
		if (DistanceCm < MinTargetDistance)
		{
			continue;
		}

		float Score = VisitWeight * FMath::Log2(1.f + Coverage->GetVisitCount(Cell))
			+ DistanceWeight * (DistanceCm / 100.f)
			+ Random.FRand() * 0.5f; // tie-breaker

		for (const FVector& Other : OtherTargets)
		{
			if (FVector::DistSquared(Other, CellLocation) < ClaimRadiusSq)
			{
				Score += ClaimPenalty;
			}
		}

		if (Score < BestScore)
		{
			BestScore = Score;
			BestCell = Cell;
			bFound = true;
		}
	}

	if (!bFound)
	{
		return false;
	}

	TargetCell = BestCell;
	bHasTarget = true;
	MoveTimeLeft = MaxMoveTime;

	const EPathFollowingRequestResult::Type Request = MoveToLocation(
		Coverage->CellToLocation(BestCell),
		AcceptanceRadius,
		/*bStopOnOverlap*/ true,
		/*bUsePathfinding*/ true,
		/*bProjectDestinationToNavigation*/ true,
		/*bCanStrafe*/ false);

	if (Request == EPathFollowingRequestResult::Failed)
	{
		FailedCells.Add(BestCell);
		bHasTarget = false;
		return false;
	}
	if (Request == EPathFollowingRequestResult::AlreadyAtGoal)
	{
		bHasTarget = false;
	}
	return true;
}
