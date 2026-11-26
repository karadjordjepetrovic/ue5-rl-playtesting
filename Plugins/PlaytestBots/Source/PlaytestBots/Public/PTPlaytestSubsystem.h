// PTPlaytestSubsystem.h
// Owns one playtest run: reads config, spawns explorer bots, ends the run on timeout.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/PTPlaytestSubsystem.h

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "Subsystems/WorldSubsystem.h"
#include "PTPlaytestSubsystem.generated.h"

class AController;
class APawn;
class APTLearningActor;

/** Which brain drives the bots. Random/Frontier controllers arrive later in step 5. */
UENUM(BlueprintType)
enum class EPTWalkerType : uint8
{
	Idle,      // plain AIController, bots stand still (useful for testing spawn/teardown)
	Random,    // step 5: random movement input
	Frontier,  // step 5: navmesh walker toward least-visited cells
	Policy     // Phase 2: Learning Agents policy
};

/** Everything that defines a run. Filled from the command line or the pt.Start console command. */
USTRUCT(BlueprintType)
struct PLAYTESTBOTS_API FPTRunConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	int32 NumBots = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	float DurationSeconds = 600.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	int32 Seed = 42;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	EPTWalkerType Walker = EPTWalkerType::Random;

	/** Optional pawn class path, e.g. /Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter_C
	 *  Empty = use the GameMode's DefaultPawnClass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	FString PawnClassPath;

	/** Policy walker only: train the policy (PPO) during the run instead of just running it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	bool bTrain = false;

	/** Policy walker: model name, stored in Saved/PlaytestModels/<ModelName>/. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	FString ModelName = TEXT("explorer");

	/** Training: ignore an existing saved model and start from scratch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	bool bFreshModel = false;

	/** Policy inference: action noise (1 = sample the learned distribution, 0 = deterministic). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	float InferenceNoise = 1.f;

	/** Procedural gyms: rebuild each gym's layout every this many minutes (staggered across gyms). 0 = never. */
	float GymRegenMinutes = 0.f;

	/** Training: use the CPU instead of the GPU (slower; for machines without CUDA). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	bool bTrainOnCpu = false;

	/** Quit the process when the run ends (always true for standalone/headless runs). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playtest")
	bool bQuitOnEnd = false;

	FString ToString() const;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FPTOnRunEvent, const FPTRunConfig& /*Config*/);

class APTGymLayout;

UCLASS()
class PLAYTESTBOTS_API UPTPlaytestSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// UWorldSubsystem
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	/** Starts a run. Ignored if one is already running. */
	UFUNCTION(BlueprintCallable, Category = "Playtest")
	void StartRun(const FPTRunConfig& InConfig);

	/** Ends the current run: broadcasts OnRunFinished, destroys bots, optionally quits. */
	UFUNCTION(BlueprintCallable, Category = "Playtest")
	void EndRun();

	UFUNCTION(BlueprintPure, Category = "Playtest")
	bool IsRunning() const { return bRunning; }

	UFUNCTION(BlueprintPure, Category = "Playtest")
	float GetElapsedSeconds() const;

	const FPTRunConfig& GetConfig() const { return Config; }
	const FString& GetRunId() const { return RunId; }
	FString GetRunOutputDir() const;
	FRandomStream& GetRandom() { return Random; }
	const TArray<TWeakObjectPtr<APawn>>& GetBots() const { return Bots; }

	/** The procedural gym a bot was spawned in (null on hand-made maps). */
	APTGymLayout* GetGymForBot(const APawn* Bot) const;

	/** Called by a gym after it built a new layout: moves its bots onto the new floor. */
	void HandleGymRegenerated(APTGymLayout* Gym);

	/** Puts a bot at a random standing spot in its gym and tells the detectors about the teleport. */
	void PlaceBotInGym(APawn* Bot, APTGymLayout* Gym);

	static FString WalkerToString(EPTWalkerType Walker);
	static bool ParseWalker(const FString& Text, EPTWalkerType& OutWalker);

	/** Hooks for the coverage grid, detectors and reporter (added later in Phase 1). */
	FPTOnRunEvent OnRunStarted;
	FPTOnRunEvent OnRunFinished;

protected:
	/** Only exist in real game worlds (standalone/headless and PIE), never in editor preview worlds. */
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	static bool ParseCommandLine(FPTRunConfig& OutConfig);

	UClass* ResolvePawnClass() const;
	TSubclassOf<AController> ResolveControllerClass() const;

	void SpawnBots();
	APawn* SpawnBot(int32 Index, const TArray<AActor*>& PlayerStarts);
	void DestroyBots();

	FPTRunConfig Config;
	FRandomStream Random;
	FString RunId;
	double RunStartTime = 0.0;
	bool bRunning = false;
	FTimerHandle EndTimerHandle;

	TArray<TWeakObjectPtr<APawn>> Bots;

	// Procedural gyms in the map (empty on hand-made maps); bots are spread over them round-robin.
	TArray<TWeakObjectPtr<APTGymLayout>> Gyms;
	TMap<TWeakObjectPtr<APawn>, TWeakObjectPtr<APTGymLayout>> BotGyms;
	FTimerHandle GymRegenTimerHandle;
	int32 NextGymToRegen = 0;
	void RegenerateNextGym();

	/** Learning Agents owner, only for the Policy walker. */
	UPROPERTY()
	TObjectPtr<APTLearningActor> LearningActor;

	void SpawnLearningActor();
	void DestroyLearningActor();
};