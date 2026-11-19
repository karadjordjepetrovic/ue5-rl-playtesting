// PTBotControllerBase.h
// Shared base for all bot brains (random, frontier, later the Learning Agents policy).
// Gives every bot a stable index and its own deterministic random stream.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Walkers/PTBotControllerBase.h

#pragma once

#include "AIController.h"
#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "PTBotControllerBase.generated.h"

UCLASS(Abstract)
class PLAYTESTBOTS_API APTBotControllerBase : public AAIController
{
	GENERATED_BODY()

public:
	APTBotControllerBase();

	/** Called by UPTPlaytestSubsystem right after spawning. Same seed + index = same random choices. */
	virtual void InitBot(int32 InBotIndex, int32 RunSeed);

	int32 GetBotIndex() const { return BotIndex; }

protected:
	virtual void OnPossess(APawn* InPawn) override;

	FRandomStream Random;
	int32 BotIndex = INDEX_NONE;
};
