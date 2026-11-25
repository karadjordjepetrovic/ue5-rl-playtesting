// PTLearningActor.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Learning/PTLearningActor.cpp

#include "Learning/PTLearningActor.h"

#include "Engine/EngineTypes.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Learning/PTExplorerEnvironment.h"
#include "Learning/PTExplorerInteractor.h"
#include "LearningAgentsCommunicator.h"
#include "LearningAgentsCritic.h"
#include "LearningAgentsInteractor.h"
#include "LearningAgentsManager.h"
#include "LearningAgentsNeuralNetwork.h"
#include "LearningAgentsPolicy.h"
#include "LearningAgentsTrainingEnvironment.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PTPlaytestSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlaytestLearning, Log, All);

namespace PTModelFiles
{
	static const TCHAR* Encoder = TEXT("encoder.bin");
	static const TCHAR* Policy = TEXT("policy.bin");
	static const TCHAR* Decoder = TEXT("decoder.bin");
	static const TCHAR* Critic = TEXT("critic.bin");
	static const TCHAR* Signature = TEXT("schema.txt");

	static FFilePath In(const FString& Dir, const TCHAR* Name)
	{
		FFilePath Path;
		Path.FilePath = FPaths::Combine(Dir, Name);
		return Path;
	}
}

APTLearningActor::APTLearningActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics; // decide actions before movement runs

	Manager = CreateDefaultSubobject<ULearningAgentsManager>(TEXT("LearningAgentsManager"));

	// UE 5.8: the default trainer module is behavior cloning; PPO must be selected explicitly.
	ProcessSettings.TrainerFileName = TEXT("learning_core.train_ppo");

	// Starting the Python trainer imports PyTorch, which can take well over the default 10 s on Windows.
	SharedMemorySettings.Timeout = 120.f;
}

FString APTLearningActor::GetModelDir() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PlaytestModels"), Options.ModelName));
}

FString APTLearningActor::GetSchemaSignature() const
{
	// A saved model only fits the observations/actions it was trained with.
	const UPTExplorerInteractor* Explorer = Cast<UPTExplorerInteractor>(Interactor);
	return Explorer ? Explorer->GetSchemaSignature() : FString();
}

bool APTLearningActor::LoadModel(bool bIncludeCritic)
{
	const FString Dir = GetModelDir();
	const FString PolicyFile = FPaths::Combine(Dir, PTModelFiles::Policy);
	if (!FPaths::FileExists(PolicyFile))
	{
		return false;
	}

	FString SavedSignature;
	FFileHelper::LoadFileToString(SavedSignature, *FPaths::Combine(Dir, PTModelFiles::Signature));
	if (SavedSignature.TrimStartAndEnd() != GetSchemaSignature())
	{
		UE_LOG(LogPlaytestLearning, Error,
			TEXT("Model '%s' was trained with different observations/actions (saved: '%s', current: '%s'). Not loading it."),
			*Options.ModelName, *SavedSignature.TrimStartAndEnd(), *GetSchemaSignature());
		return false;
	}

	Policy->GetEncoderNetworkAsset()->LoadNetworkFromSnapshot(PTModelFiles::In(Dir, PTModelFiles::Encoder));
	Policy->GetPolicyNetworkAsset()->LoadNetworkFromSnapshot(PTModelFiles::In(Dir, PTModelFiles::Policy));
	Policy->GetDecoderNetworkAsset()->LoadNetworkFromSnapshot(PTModelFiles::In(Dir, PTModelFiles::Decoder));

	if (bIncludeCritic && Critic && FPaths::FileExists(FPaths::Combine(Dir, PTModelFiles::Critic)))
	{
		Critic->GetCriticNetworkAsset()->LoadNetworkFromSnapshot(PTModelFiles::In(Dir, PTModelFiles::Critic));
	}

	UE_LOG(LogPlaytestLearning, Display, TEXT("Loaded model '%s' from %s"), *Options.ModelName, *Dir);
	return true;
}

void APTLearningActor::SaveModel()
{
	if (!Policy)
	{
		return;
	}

	const FString Dir = GetModelDir();
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);

	Policy->GetEncoderNetworkAsset()->SaveNetworkToSnapshot(PTModelFiles::In(Dir, PTModelFiles::Encoder));
	Policy->GetPolicyNetworkAsset()->SaveNetworkToSnapshot(PTModelFiles::In(Dir, PTModelFiles::Policy));
	Policy->GetDecoderNetworkAsset()->SaveNetworkToSnapshot(PTModelFiles::In(Dir, PTModelFiles::Decoder));
	if (Critic)
	{
		Critic->GetCriticNetworkAsset()->SaveNetworkToSnapshot(PTModelFiles::In(Dir, PTModelFiles::Critic));
	}
	FFileHelper::SaveStringToFile(GetSchemaSignature(), *FPaths::Combine(Dir, PTModelFiles::Signature));

	UE_LOG(LogPlaytestLearning, Display, TEXT("Saved model '%s' to %s"), *Options.ModelName, *Dir);
}

void APTLearningActor::SaveCheckpoint()
{
	SaveModel();
	CopyModelToCheckpoint(FString::Printf(TEXT("%04dmin"), FMath::RoundToInt(TrainingSeconds / 60.f)));
}

void APTLearningActor::CopyModelToCheckpoint(const FString& Label) const
{
	// Prefixed with the run id, so a later run (whose minutes start at 0 again) can't overwrite these.
	const UPTPlaytestSubsystem* Run = GetWorld() ? GetWorld()->GetSubsystem<UPTPlaytestSubsystem>() : nullptr;
	const FString RunId = Run ? Run->GetRunId() : FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));

	const FString Dir = GetModelDir();
	const FString CheckpointDir = FPaths::Combine(Dir, TEXT("checkpoints"), RunId + TEXT("_") + Label);
	IFileManager::Get().MakeDirectory(*CheckpointDir, /*Tree*/ true);

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*")), /*Files*/ true, /*Directories*/ false);
	for (const FString& File : Files)
	{
		IFileManager::Get().Copy(*FPaths::Combine(CheckpointDir, File), *FPaths::Combine(Dir, File));
	}
	UE_LOG(LogPlaytestLearning, Display, TEXT("Checkpoint saved to %s"), *CheckpointDir);
}

void APTLearningActor::Setup(const TArray<APawn*>& InBots, const FPTLearningOptions& InOptions)
{
	Options = InOptions;
	bReady = false;
	bSaved = false;

	if (InBots.IsEmpty())
	{
		UE_LOG(LogPlaytestLearning, Error, TEXT("No bots to control."));
		return;
	}

	Manager->SetMaxAgentNum(InBots.Num());

	// The Make* functions take references to pointers, so use local variables.
	ULearningAgentsManager* ManagerPtr = Manager;

	ULearningAgentsInteractor* InteractorPtr = ULearningAgentsInteractor::MakeInteractor(
		ManagerPtr, UPTExplorerInteractor::StaticClass(), TEXT("ExplorerInteractor"));
	Interactor = InteractorPtr;

	ULearningAgentsPolicy* PolicyPtr = ULearningAgentsPolicy::MakePolicy(
		ManagerPtr, InteractorPtr, ULearningAgentsPolicy::StaticClass(), TEXT("ExplorerPolicy"),
		/*Encoder*/ nullptr, /*Policy*/ nullptr, /*Decoder*/ nullptr,
		/*bReinitializeEncoder*/ true, /*bReinitializePolicy*/ true, /*bReinitializeDecoder*/ true,
		FLearningAgentsPolicySettings(), Options.Seed);
	Policy = PolicyPtr;

	if (Options.bTrain)
	{
		ULearningAgentsCritic* CriticPtr = ULearningAgentsCritic::MakeCritic(
			ManagerPtr, InteractorPtr, PolicyPtr, ULearningAgentsCritic::StaticClass(), TEXT("ExplorerCritic"),
			/*CriticAsset*/ nullptr, /*bReinitialize*/ true, FLearningAgentsCriticSettings(), Options.Seed);
		Critic = CriticPtr;
	}

	// Load before the trainer exists: the trainer sends the current networks to Python as its starting point.
	bool bLoaded = false;
	if (!Options.bTrain || !Options.bFresh)
	{
		bLoaded = LoadModel(/*bIncludeCritic*/ Options.bTrain);
		if (bLoaded && Options.bTrain)
		{
			// Keep the model this run starts from: continuing training overwrites it.
			CopyModelToCheckpoint(TEXT("start"));
		}
	}
	if (!bLoaded)
	{
		UE_LOG(LogPlaytestLearning, Display, TEXT("%s"), Options.bTrain
			? TEXT("Training a new model from scratch.")
			: TEXT("No saved model found: bots will run an UNTRAINED policy. Train first with -PTTrain."));
	}

	if (Options.bTrain)
	{
		ULearningAgentsCritic* CriticPtr = Critic;

		ULearningAgentsTrainingEnvironment* EnvironmentPtr = ULearningAgentsTrainingEnvironment::MakeTrainingEnvironment(
			ManagerPtr, UPTExplorerEnvironment::StaticClass(), TEXT("ExplorerEnvironment"));
		Environment = EnvironmentPtr;

		// Every bot's episode fits in the batch: no experience is thrown away.
		TrainerSettings.MaxEpisodeStepNum = EpisodeSteps;
		TrainerSettings.MaximumRecordedStepsPerIteration = EpisodeSteps * InBots.Num();
		TrainerSettings.MaximumRecordedEpisodesPerIteration = FMath::Max(InBots.Num() * 4, 64);

		// Fewer optimisation passes per batch: shorter game pauses at each update (was 32, ~7.5 s).
		TrainingSettings.IterationsPerGather = 8;

		TrainingSettings.Device = Options.bUseGpu ? ELearningAgentsTrainingDevice::GPU : ELearningAgentsTrainingDevice::CPU;
		TrainingSettings.RandomSeed = Options.Seed;

		const FLearningAgentsCommunicator Communicator =
			ULearningAgentsCommunicatorLibrary::MakeSharedMemoryTrainingProcess(ProcessSettings, SharedMemorySettings);

		Trainer = ULearningAgentsPPOTrainer::MakePPOTrainer(
			ManagerPtr, InteractorPtr, EnvironmentPtr, PolicyPtr, CriticPtr, Communicator,
			ULearningAgentsPPOTrainer::StaticClass(), TEXT("ExplorerPPOTrainer"), TrainerSettings);
	}

	for (APawn* Bot : InBots)
	{
		if (Bot)
		{
			Manager->AddAgent(Bot);
		}
	}

	bReady = true;
	UE_LOG(LogPlaytestLearning, Display, TEXT("Learning setup done: %d agents, mode=%s, model=%s (%s), device=%s, episode=%d steps"),
		InBots.Num(), Options.bTrain ? TEXT("TRAIN") : TEXT("INFERENCE"), *Options.ModelName,
		bLoaded ? TEXT("loaded") : TEXT("new"), Options.bUseGpu ? TEXT("GPU") : TEXT("CPU"), EpisodeSteps);
}

void APTLearningActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bReady)
	{
		return;
	}

	if (Options.bTrain && Trainer && Trainer->HasTrainingFailed())
	{
		if (!bLoggedFailure)
		{
			UE_LOG(LogPlaytestLearning, Error, TEXT("Training failed. Check the LogLearning lines above (Python / PyTorch / CUDA). Try -PTCpu."));
			bLoggedFailure = true;
		}
		return;
	}

	// Decide ~10 times a second (one decision = one training step), like the detectors' sampling rate.
	DecisionAccumulator += DeltaSeconds;
	if (DecisionAccumulator >= DecisionInterval)
	{
		DecisionAccumulator = FMath::Fmod(DecisionAccumulator, DecisionInterval);

		if (Options.bTrain && Trainer)
		{
			// Gathers observations, runs the policy, performs actions, gathers rewards and sends experience to Python.
			Trainer->RunTraining(TrainingSettings, GameSettings, /*bResetAgentsOnBegin*/ true, /*bResetAgentsOnUpdate*/ true);
		}
		else if (Policy)
		{
			// Inference: sample actions (noise 1) by default, so 16 bots explore differently instead of copying each other.
			Policy->RunInference(Options.InferenceNoise);
		}
	}

	// Keep applying the last decision every frame.
	if (UPTExplorerInteractor* Explorer = Cast<UPTExplorerInteractor>(Interactor))
	{
		Explorer->ApplyHeldActions();
	}

	// Periodic save during long training runs.
	if (Options.bTrain)
	{
		SaveAccumulator += DeltaSeconds;
		CheckpointAccumulator += DeltaSeconds;
		TrainingSeconds += DeltaSeconds;
		if (CheckpointAccumulator >= CheckpointIntervalSeconds)
		{
			CheckpointAccumulator = 0.f;
			SaveCheckpoint();
		}
		if (SaveAccumulator >= SaveIntervalSeconds)
		{
			SaveAccumulator = 0.f;
			SaveModel();
		}
	}
}

void APTLearningActor::Shutdown()
{
	if (Options.bTrain && bReady && !bSaved)
	{
		SaveModel();
		bSaved = true;
	}
	if (Trainer && Trainer->IsTraining())
	{
		Trainer->EndTraining();
		UE_LOG(LogPlaytestLearning, Display, TEXT("Training ended."));
	}
	bReady = false;
}

void APTLearningActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Shutdown();
	Super::EndPlay(EndPlayReason);
}
