// PTExplorerEnvironment.h
// Rewards, episode completion and resets for training explorer bots.
// Phase 2 step 1: placeholder reward (keep moving) for the smoke test. Step 3 adds the novelty reward.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Learning/PTExplorerEnvironment.h

#pragma once

#include "CoreMinimal.h"
#include "LearningAgentsTrainingEnvironment.h"
#include "PTExplorerEnvironment.generated.h"

UCLASS()
class PLAYTESTBOTS_API UPTExplorerEnvironment : public ULearningAgentsTrainingEnvironment
{
	GENERATED_BODY()

public:
	virtual void GatherAgentReward_Implementation(float& OutReward, const int32 AgentId) override;
	virtual void GatherAgentCompletion_Implementation(ELearningAgentsCompletion& OutCompletion, const int32 AgentId) override;
	virtual void ResetAgentEpisode_Implementation(const int32 AgentId) override;

	/** Below this Z the episode terminates (fell out of the level). */
	float KillHeight = -1000.f;
};
