// PTLearningActor.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Learning/PTLearningActor.cpp

#include "Learning/PTLearningActor.h"

#include "GameFramework/Pawn.h"
#include "Learning/PTExplorerEnvironment.h"
#include "Learning/PTExplorerInteractor.h"
#include "LearningAgentsCommunicator.h"
#include "LearningAgentsCritic.h"
#include "LearningAgentsInteractor.h"
#include "LearningAgentsManager.h"
#include "LearningAgentsPolicy.h"
#include "LearningAgentsTrainingEnvironment.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlaytestLearning, Log, All);

APTLearningActor::APTLearningActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics; // decide actions before movement runs

	Manager = CreateDefaultSubobject<ULearningAgentsManager>(TEXT("LearningAgentsManager"));

	// UE 5.8: the default trainer module is behavior cloning; PPO must be selected explicitly.
	ProcessSettings.TrainerFileName = TEXT("learning_core.train_ppo");

	// Starting the Python trainer imports PyTorch, which can take well over the default 10 s on Windows.
	SharedMemorySettings.Timeout = 120.f;

	// Step 1 smoke test: CPU training avoids CUDA setup issues. Switch to GPU in step 5.
	TrainingSettings.Device = ELearningAgentsTrainingDevice::CPU;
}

void APTLearningActor::Setup(const TArray<APawn*>& InBots, bool bInTrain, int32 InSeed)
{
	bTrain = bInTrain;
	bReady = false;

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
		FLearningAgentsPolicySettings(), InSeed);
	Policy = PolicyPtr;

	if (bTrain)
	{
		ULearningAgentsCritic* CriticPtr = ULearningAgentsCritic::MakeCritic(
			ManagerPtr, InteractorPtr, PolicyPtr, ULearningAgentsCritic::StaticClass(), TEXT("ExplorerCritic"),
			/*CriticAsset*/ nullptr, /*bReinitialize*/ true, FLearningAgentsCriticSettings(), InSeed);
		Critic = CriticPtr;

		ULearningAgentsTrainingEnvironment* EnvironmentPtr = ULearningAgentsTrainingEnvironment::MakeTrainingEnvironment(
			ManagerPtr, UPTExplorerEnvironment::StaticClass(), TEXT("ExplorerEnvironment"));
		Environment = EnvironmentPtr;

		const FLearningAgentsCommunicator Communicator =
			ULearningAgentsCommunicatorLibrary::MakeSharedMemoryTrainingProcess(ProcessSettings, SharedMemorySettings);

		Trainer = ULearningAgentsPPOTrainer::MakePPOTrainer(
			ManagerPtr, InteractorPtr, EnvironmentPtr, PolicyPtr, CriticPtr, Communicator,
			ULearningAgentsPPOTrainer::StaticClass(), TEXT("ExplorerPPOTrainer"), TrainerSettings);

		TrainingSettings.RandomSeed = InSeed;
	}

	for (APawn* Bot : InBots)
	{
		if (Bot)
		{
			Manager->AddAgent(Bot);
		}
	}

	bReady = true;
	UE_LOG(LogPlaytestLearning, Display, TEXT("Learning setup done: %d agents, mode=%s, trainer module=%s"),
		InBots.Num(), bTrain ? TEXT("TRAIN") : TEXT("INFERENCE"), *ProcessSettings.TrainerFileName);
}

void APTLearningActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bReady)
	{
		return;
	}

	if (bTrain && Trainer)
	{
		if (Trainer->HasTrainingFailed())
		{
			if (!bLoggedFailure)
			{
				UE_LOG(LogPlaytestLearning, Error, TEXT("Training failed. Check the LogLearning lines above (Python / PyTorch setup)."));
				bLoggedFailure = true;
			}
			return;
		}
		// Gathers observations, runs the policy, performs actions, gathers rewards and sends experience to Python.
		Trainer->RunTraining(TrainingSettings, GameSettings, /*bResetAgentsOnBegin*/ true, /*bResetAgentsOnUpdate*/ true);
	}
	else if (Policy)
	{
		// Inference only: no exploration noise.
		Policy->RunInference(/*ActionNoiseScale*/ 0.f);
	}
}

void APTLearningActor::Shutdown()
{
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
