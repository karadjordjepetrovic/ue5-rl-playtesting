// PTExplorerEnvironment.h
// Rewards, episode completion and resets for training explorer bots.
// Reward = episodic novelty (+ global rarity) + new height levels + capped detector bonus - idle/time penalties.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Learning/PTExplorerEnvironment.h

#pragma once

#include "CoreMinimal.h"
#include "LearningAgentsTrainingEnvironment.h"
#include "PTExplorerEnvironment.generated.h"

class APawn;

UCLASS()
class PLAYTESTBOTS_API UPTExplorerEnvironment : public ULearningAgentsTrainingEnvironment
{
	GENERATED_BODY()

public:
	virtual void GatherAgentReward_Implementation(float& OutReward, const int32 AgentId) override;
	virtual void GatherAgentCompletion_Implementation(ELearningAgentsCompletion& OutCompletion, const int32 AgentId) override;
	virtual void ResetAgentEpisode_Implementation(const int32 AgentId) override;

	// Reward weights (starting values; tuned in step 5)
	float EpisodicNoveltyReward = 0.5f;  // first visit to a cell in this episode
	float RecentVisitWindow = 30.f;      // s; the episodic novelty only pays fully if NO bot stood on the cell in this window
	float GlobalRarityReward = 0.5f;     // extra, scaled by 1/sqrt(1 + visits by all bots so far)
	float NewHeightReward = 0.5f;        // per new height level (cell Z) reached this episode
	float IssueReward = 2.f;             // when a detector fires for this bot ...
	float OutOfWorldReward = 0.5f;       // ... falls are cheap to find, so worth less ...
	float IssueRepeatDecay = 1.f;        // ... divided by (1 + decay * times this spot was already rewarded), so farming one bug stops paying
	float IdlePenalty = 0.01f;           // per step while barely moving
	float IdleSpeed = 50.f;              // cm/s
	float TimePenalty = 0.001f;          // per step
	float JumpPenalty = 0.02f;           // per take-off, so jumps have to pay for themselves (free when there is a ledge/gap ahead)
	float BlockedPenalty = 0.01f;        // per step while pushing into something (input direction, but little speed that way)
	float BlockedSpeedFraction = 0.4f;   // below this fraction of max walk speed along the input = blocked
	float OutOfWorldCooldown = 180.f;    // s; a bot gets the fall bonus at most once per this window ...
	float RepeatFallPenalty = 0.f;       // ... later falls inside it get no bonus (a penalty here made bots avoid all edges)
	float CrowdingPenalty = 0.02f;       // per step at zero distance to another bot, fading to 0 at CrowdingRadius
	float CrowdingRadius = 300.f;        // cm
	float IssueClusterSize = 400.f;      // cm; one planted bug = one spot (the hole and the gap are 4 m wide)

	/** Start episodes at random reachable navmesh cells instead of the Player Start. */
	bool bRandomStarts = true;

	/** Below this Z the episode terminates (fell out of the level and was not caught by the detectors). */
	float KillHeight = -2000.f;

private:
	struct FEpisodeState
	{
		TSet<FIntVector> VisitedCells;
		int32 MaxCellZ = TNumericLimits<int32>::Lowest();
		float PendingIssueReward = 0.f;
		TMap<FString, int32> RewardedSpots; // per episode: finding a bug always pays, farming one spot doesn't
		bool bWasOnGround = true;           // for detecting jump take-offs
		bool bTeleportPending = false;      // fell out / got stuck: move to a random spot, SAME episode (see GatherAgentReward)
	};

	void ProcessNewIssues(UWorld* World);

	/** Random reachable navmesh spot (or the Player Start), standing on the floor; tells the detectors about the jump. */
	void PlaceAtRandomSpot(APawn* Bot) const;

	TMap<int32, FEpisodeState> Episodes;
	TMap<int32, double> LastFallRewardTime; // per agent, across episodes
	TMap<FIntVector, double> LastCellVisitTime; // any bot, any episode: stops laps along routes others just ran
	TMap<TWeakObjectPtr<APawn>, int32> PawnToAgent;
	int32 ProcessedIssueCount = 0;

	// Simple progress log
	double StatRewardSum = 0.0;
	int32 StatSteps = 0;
	int32 StatNewCells = 0;
	int32 StatIssues = 0;
};
