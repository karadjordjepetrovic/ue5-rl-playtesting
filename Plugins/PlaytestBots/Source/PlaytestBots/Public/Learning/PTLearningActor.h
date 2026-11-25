// PTLearningActor.h
// Owns the Learning Agents objects for one run: manager, interactor, policy and (when training)
// critic, training environment, PPO trainer and the Python training process.
// Also saves/loads the trained networks to Saved/PlaytestModels/<ModelName>/.
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
class ULearningAgentsNeuralNetwork;
class ULearningAgentsPolicy;
class ULearningAgentsTrainingEnvironment;

/** How a Policy run should behave (filled from FPTRunConfig). */
struct FPTLearningOptions
{
	bool bTrain = false;
	int32 Seed = 1;
	FString ModelName = TEXT("explorer");
	bool bFresh = false;    // training: ignore an existing model and start from scratch
	bool bUseGpu = true;    // training device
	float InferenceNoise = 1.f; // 1 = sample from the learned action distribution (varied explorers), 0 = always the most likely action
};

UCLASS()
class PLAYTESTBOTS_API APTLearningActor : public AActor
{
	GENERATED_BODY()

public:
	APTLearningActor();

	/** Creates all Learning Agents objects, loads a saved model if there is one, and registers the bots as agents. */
	void Setup(const TArray<APawn*>& InBots, const FPTLearningOptions& InOptions);

	/** Saves the model (training runs) and stops training cleanly. Safe to call more than once. */
	void Shutdown();

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	bool IsTrainingMode() const { return Options.bTrain; }

	/** Seconds between decisions (observe + act). Actions are held and re-applied every frame in between. */
	UPROPERTY(EditAnywhere, Category = "Playtest Learning")
	float DecisionInterval = 0.1f;

	/** Decisions per episode (512 x 0.1 s = ~51 s of play before a reset). The batch is bots x EpisodeSteps. */
	UPROPERTY(EditAnywhere, Category = "Playtest Learning")
	int32 EpisodeSteps = 512;

	/** Training runs also save the model every this many seconds, so a crash doesn't lose hours of training. */
	UPROPERTY(EditAnywhere, Category = "Playtest Learning")
	float SaveIntervalSeconds = 300.f;
	float CheckpointIntervalSeconds = 1800.f; // also keep a copy in checkpoints/<RunId>_<minutes>min every 30 min

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
	FString GetModelDir() const;
	FString GetSchemaSignature() const;
	bool LoadModel(bool bIncludeCritic);
	void SaveModel();

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

	FPTLearningOptions Options;
	bool bReady = false;
	bool bLoggedFailure = false;
	bool bSaved = false;
	float DecisionAccumulator = 0.f;
	float SaveAccumulator = 0.f;
	float CheckpointAccumulator = 0.f;
	float TrainingSeconds = 0.f;

	void SaveCheckpoint();

	/** Copies the model files currently on disk into checkpoints/<RunId>_<Label>. */
	void CopyModelToCheckpoint(const FString& Label) const;
};
