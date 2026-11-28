// PTExplorerEnvironment.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Learning/PTExplorerEnvironment.cpp

#include "Learning/PTExplorerEnvironment.h"

#include "Coverage/PTCoverageSubsystem.h"
#include "Detectors/PTIssueSubsystem.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "LearningAgentsCompletions.h"
#include "NavigationSystem.h"
#include "PTPlaytestSubsystem.h"
#include "Learning/PTExplorerInteractor.h"
#include "Gym/PTGymLayout.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlaytestReward, Log, All);

void UPTExplorerEnvironment::ProcessNewIssues(UWorld* World)
{
	const UPTIssueSubsystem* IssueSystem = World ? World->GetSubsystem<UPTIssueSubsystem>() : nullptr;
	const UPTPlaytestSubsystem* Run = World ? World->GetSubsystem<UPTPlaytestSubsystem>() : nullptr;
	if (!IssueSystem || !Run)
	{
		return;
	}

	const TArray<FPTIssueRecord>& Issues = IssueSystem->GetIssues();
	if (Issues.Num() < ProcessedIssueCount)
	{
		ProcessedIssueCount = 0; // a new run started
	}

	const TArray<TWeakObjectPtr<APawn>>& Bots = Run->GetBots();
	for (int32 IssueIndex = ProcessedIssueCount; IssueIndex < Issues.Num(); ++IssueIndex)
	{
		const FPTIssueRecord& Issue = Issues[IssueIndex];
		if (!Bots.IsValidIndex(Issue.BotIndex))
		{
			continue; // perf spikes from the camera, the player, etc.
		}
		const int32* AgentId = PawnToAgent.Find(Bots[Issue.BotIndex]);
		if (!AgentId)
		{
			continue;
		}

		FEpisodeState& Episode = Episodes.FindOrAdd(*AgentId);

		// Same type at the same spot is worth less every time it is found again IN THIS EPISODE.
		// (A lifetime decay made bugs worthless after a few hundred finds, so the policy learned to avoid them.)
		const FString Spot = FString::Printf(TEXT("%d_%d_%d"), static_cast<int32>(Issue.Type),
			FMath::FloorToInt(Issue.Location.X / IssueClusterSize), FMath::FloorToInt(Issue.Location.Y / IssueClusterSize));
		int32& TimesRewarded = Episode.RewardedSpots.FindOrAdd(Spot);
		float Bonus = 0.f;
		const APTGymLayout* Gym = Run->GetGymForBot(Bots[Issue.BotIndex].Get());
		if (Issue.Type == EPTIssueType::OutOfWorld && Gym)
		{
			// Procedural gyms: each fall spot pays once per bot per LAYOUT. Every regenerated layout is worth
			// exploring for falls again, but jumping into the same hole twice pays nothing.
			// (A once-per-3-minutes rule made the bots too cautious around edges: Random found twice as many falls.)
			const FString Key = FString::Printf(TEXT("%d_%s_%d_%s"), *AgentId, *Gym->GetName(), Gym->GetCurrentSeed(), *Spot);
			bool bAlreadyRewarded = false;
			RewardedFallSpots.Add(Key, &bAlreadyRewarded);
			Bonus = bAlreadyRewarded ? -RepeatFallPenalty : OutOfWorldReward;
		}
		else if (Issue.Type == EPTIssueType::OutOfWorld)
		{
			// Hand-made maps: the bonus once per cooldown window per bot; repeats inside the window cost reward.
			const double Now = World->GetTimeSeconds();
			const double* LastTime = LastFallRewardTime.Find(*AgentId);
			if (!LastTime || Now - *LastTime >= OutOfWorldCooldown)
			{
				Bonus = OutOfWorldReward;
				LastFallRewardTime.Add(*AgentId, Now);
			}
			else
			{
				Bonus = -RepeatFallPenalty;
			}
		}
		else
		{
			Bonus = IssueReward / (1.f + IssueRepeatDecay * TimesRewarded);
		}
		++TimesRewarded;

		Episode.PendingIssueReward += Bonus;
		if (Issue.Type == EPTIssueType::OutOfWorld || Issue.Type == EPTIssueType::Stuck)
		{
			Episode.bTeleportPending = true; // the bot can't continue usefully from there
		}
		++StatIssues;
	}
	ProcessedIssueCount = Issues.Num();
}

void UPTExplorerEnvironment::GatherAgentReward_Implementation(float& OutReward, const int32 AgentId)
{
	OutReward = -TimePenalty;

	APawn* Bot = Cast<APawn>(GetAgent(AgentId));
	UWorld* World = Bot ? Bot->GetWorld() : nullptr;
	if (!Bot || !World)
	{
		return;
	}
	PawnToAgent.FindOrAdd(Bot) = AgentId;

	ProcessNewIssues(World);
	FEpisodeState& Episode = Episodes.FindOrAdd(AgentId);

	// Detector bonus collected since the last step.
	OutReward += Episode.PendingIssueReward;
	Episode.PendingIssueReward = 0.f;

	// Idle penalty.
	if (Bot->GetVelocity().Size2D() < IdleSpeed)
	{
		OutReward -= IdlePenalty;
	}

	// Jump cost (counted once per take-off) and blocked-movement cost (pushing into a wall without getting anywhere).
	if (const ACharacter* JumpCharacter = Cast<ACharacter>(Bot))
	{
		if (const UCharacterMovementComponent* Move = JumpCharacter->GetCharacterMovement())
		{
			const bool bOnGround = Move->IsMovingOnGround();
			if (Episode.bWasOnGround && Move->IsFalling() && Bot->GetVelocity().Z > 150.f)
			{
				// Walking off a ledge has ~0 vertical speed, a jump has JumpZVelocity.
				// Jumping is free when there is something to jump onto or across in the direction of travel.
				FVector Dir = Bot->GetVelocity().GetSafeNormal2D();
				if (Dir.IsNearlyZero())
				{
					Dir = Bot->GetActorForwardVector().GetSafeNormal2D();
				}
				bool bLedge = false, bGap = false;
				UPTExplorerInteractor::ProbeJump(JumpCharacter, Dir, 200.f, bLedge, bGap);
				if (!bLedge && !bGap)
				{
					OutReward -= JumpPenalty;
				}
			}
			Episode.bWasOnGround = bOnGround;

			const FVector Input = Bot->GetLastMovementInputVector().GetSafeNormal2D();
			if (bOnGround && !Input.IsNearlyZero())
			{
				const float SpeedAlongInput = FVector::DotProduct(Bot->GetVelocity(), Input);
				const float Threshold = BlockedSpeedFraction * Move->GetMaxSpeed();
				if (SpeedAlongInput < Threshold)
				{
					OutReward -= BlockedPenalty * (1.f - FMath::Max(0.f, SpeedAlongInput) / Threshold);
				}
			}
		}
	}

	// Crowding penalty: walking right next to (or behind) another bot covers nothing new.
	if (const UPTPlaytestSubsystem* Run = World->GetSubsystem<UPTPlaytestSubsystem>())
	{
		float Nearest = CrowdingRadius;
		for (const TWeakObjectPtr<APawn>& Other : Run->GetBots())
		{
			if (Other.IsValid() && Other.Get() != Bot)
			{
				const FVector ToOther = Other->GetActorLocation() - Bot->GetActorLocation();
				if (FMath::Abs(ToOther.Z) < 300.f)
				{
					Nearest = FMath::Min(Nearest, static_cast<float>(ToOther.Size2D()));
				}
			}
		}
		OutReward -= CrowdingPenalty * (1.f - Nearest / CrowdingRadius);
	}

	// Novelty and height, only while standing on something (like the coverage grid).
	const ACharacter* BotCharacter = Cast<ACharacter>(Bot);
	const UCharacterMovementComponent* Movement = BotCharacter ? BotCharacter->GetCharacterMovement() : nullptr;
	const UPTCoverageSubsystem* Coverage = World->GetSubsystem<UPTCoverageSubsystem>();
	if (Coverage && Movement && Movement->IsMovingOnGround())
	{
		const FIntVector Cell = Coverage->LocationToCell(Bot->GetNavAgentLocation());

		// How long since ANY bot stood here: 0 = just now, 1 = longer than RecentVisitWindow ago (or never).
		// Without this, "follow the outer wall" paid well: every lap was new for this episode,
		// even with 15 other bots running the same lap.
		const double Now = World->GetTimeSeconds();
		float Staleness = 1.f;
		if (const double* LastVisit = LastCellVisitTime.Find(Cell))
		{
			Staleness = FMath::Clamp(static_cast<float>((Now - *LastVisit) / RecentVisitWindow), 0.f, 1.f);
		}

		bool bAlreadyVisited = false;
		Episode.VisitedCells.Add(Cell, &bAlreadyVisited);
		if (!bAlreadyVisited)
		{
			const int32 GlobalVisits = Coverage->GetVisitCount(Cell);
			OutReward += EpisodicNoveltyReward * Staleness + GlobalRarityReward / FMath::Sqrt(1.f + GlobalVisits);
			++StatNewCells;
		}
		LastCellVisitTime.FindOrAdd(Cell) = Now;

		if (Cell.Z > Episode.MaxCellZ)
		{
			if (Episode.MaxCellZ != TNumericLimits<int32>::Lowest())
			{
				OutReward += NewHeightReward * (Cell.Z - Episode.MaxCellZ);
			}
			Episode.MaxCellZ = Cell.Z;
		}
	}

	// Fell out / got stuck: continue the SAME episode from a random spot.
	// (Ending the episode as a truncation made the critic estimate the value of "falling through the void",
	//  a state it is never trained on; those guesses drifted and collapsed training after ~2 h.)
	if (Episode.bTeleportPending)
	{
		Episode.bTeleportPending = false;
		PlaceAtRandomSpot(Bot);
		Episode.bWasOnGround = true;
	}

	// Progress log every ~2000 agent steps.
	StatRewardSum += OutReward;
	++StatSteps;
	if (StatSteps >= 2000)
	{
		UE_LOG(LogPlaytestReward, Display, TEXT("Last %d steps: mean reward %.3f, new cells/step %.3f, issues %d"),
			StatSteps, StatRewardSum / StatSteps, static_cast<float>(StatNewCells) / StatSteps, StatIssues);
		StatRewardSum = 0.0;
		StatSteps = 0;
		StatNewCells = 0;
		StatIssues = 0;
	}
}

void UPTExplorerEnvironment::GatherAgentCompletion_Implementation(ELearningAgentsCompletion& OutCompletion, const int32 AgentId)
{
	OutCompletion = ELearningAgentsCompletion::Running;

	const APawn* Bot = Cast<APawn>(GetAgent(AgentId));
	if (!Bot || Bot->GetActorLocation().Z < KillHeight)
	{
		OutCompletion = ELearningAgentsCompletion::Termination;
		return;
	}

	// Truncation after MaxEpisodeStepNum is handled by the trainer settings.
}

void UPTExplorerEnvironment::ResetAgentEpisode_Implementation(const int32 AgentId)
{
	// New episode: forget what this bot visited (global coverage counts stay).
	Episodes.FindOrAdd(AgentId) = FEpisodeState();

	APawn* Bot = Cast<APawn>(GetAgent(AgentId));
	UWorld* World = Bot ? Bot->GetWorld() : nullptr;
	if (!Bot || !World)
	{
		return;
	}

	// Start each episode at a random reachable spot, so the policy practises the whole level,
	// not just the first seconds around the Player Start.
	PlaceAtRandomSpot(Bot);
}

void UPTExplorerEnvironment::PlaceAtRandomSpot(APawn* Bot) const
{
	UWorld* World = Bot ? Bot->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	// Procedural gyms: stay inside the bot's own gym.
	if (UPTPlaytestSubsystem* Run = World->GetSubsystem<UPTPlaytestSubsystem>())
	{
		if (APTGymLayout* Gym = Run->GetGymForBot(Bot))
		{
			Run->PlaceBotInGym(Bot, Gym);
			return;
		}
	}

	FVector Location(0.f, 0.f, 200.f);
	bool bPlaced = false;
	const UPTCoverageSubsystem* Coverage = World->GetSubsystem<UPTCoverageSubsystem>();
	if (bRandomStarts && Coverage && !Coverage->GetReachableCellArray().IsEmpty())
	{
		const TArray<FIntVector>& Cells = Coverage->GetReachableCellArray();
		const FVector CellCenter = Coverage->CellToLocation(Cells[FMath::RandHelper(Cells.Num())]);

		// Snap to the navmesh, then lift by the capsule half-height so the bot stands on the floor.
		FVector Floor = CellCenter;
		if (const UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
		{
			FNavLocation Projected;
			if (NavSys->ProjectPointToNavigation(CellCenter, Projected, FVector(50.f, 50.f, 200.f)))
			{
				Floor = Projected.Location;
			}
		}
		Location = Floor + FVector(0.f, 0.f, Bot->GetDefaultHalfHeight() + 5.f);
		bPlaced = true;
	}
	if (!bPlaced)
	{
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			Location = It->GetActorLocation() + FVector(FMath::FRandRange(-100.f, 100.f), FMath::FRandRange(-100.f, 100.f), 0.f);
			break;
		}
	}

	Bot->SetActorLocationAndRotation(Location, FRotator(0.f, FMath::FRandRange(0.f, 360.f), 0.f),
		/*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);

	if (const ACharacter* BotCharacter = Cast<ACharacter>(Bot))
	{
		if (UCharacterMovementComponent* Movement = BotCharacter->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
		}
	}

	if (UPTIssueSubsystem* Issues = World->GetSubsystem<UPTIssueSubsystem>())
	{
		Issues->NotifyBotTeleported(Bot);
	}
}
