// PTLearningActor.h
// Owns the Learning Agents objects for one run: manager, interactor, policy and (when training)
// critic, training environment, PPO trainer and the Python training process.
// Spawned by UPTPlaytestSubsystem when the walker is "Policy".
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Learning/PTLearningActor.h

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LearningAgentsCommunicator.h"
#include "LearningAgentsPPOTrainer.h"
#include "LearningAgentsTrainer.h"
#include "PTLearningActor.generated.h"

class APawn;
class ULearningAgentsCritic;
class ULearningAgentsInteractor;
class ULearningAgentsManager;
class ULearningAgentsPolicy;
class ULearningAgentsTrainingEnvironment;

UCLASS()
class PLAYTESTBOTS_API APTLearningActor : public AActor
{
	GENERATED_BODY()

public:
	APTLearningActor();

	/** Creates all Learning Agents objects and registers the bots as agents. */
	void Setup(const TArray<APawn*>& InBots, bool bInTrain, int32 InSeed);

	/** Stops training cleanly (ends the Python process). Safe to call more than once. */
	void Shutdown();

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	bool IsTrainingMode() const { return bTrain; }

	// Training settings (step 1 keeps them simple; tuned in step 5)
	UPROPERTY(EditAnywhere, Category = "Playtest Learning")
	FLearningAgentsPPOTrainerSettings TrainerSettings;

	UPROPERTY(EditAnywhere, Category = "Playtest Learning")
	FLearningAgentsPPOTrainingSettings TrainingSettings;

	UPROPERTY(EditAnywhere, Category = "Playtest Learning")
	FLearningAgentsTrainingGameSettings GameSettings;

	UPROPERTY(EditAnywhere, Category = "Playtest Learning")
	FLearningAgentsTrainerProcessSettings ProcessSettings;

	UPROPERTY(EditAnywhere, Category = "Playtest Learning")
	FLearningAgentsSharedMemoryCommunicatorSettings SharedMemorySettings;

private:
	UPROPERTY(VisibleAnywhere, Category = "Playtest Learning")
	TObjectPtr<ULearningAgentsManager> Manager;

	UPROPERTY()
	TObjectPtr<ULearningAgentsInteractor> Interactor;

	UPROPERTY()
	TObjectPtr<ULearningAgentsPolicy> Policy;

	UPROPERTY()
	TObjectPtr<ULearningAgentsCritic> Critic;

	UPROPERTY()
	TObjectPtr<ULearningAgentsTrainingEnvironment> Environment;

	UPROPERTY()
	TObjectPtr<ULearningAgentsPPOTrainer> Trainer;

	bool bTrain = false;
	bool bReady = false;
	bool bLoggedFailure = false;
};
