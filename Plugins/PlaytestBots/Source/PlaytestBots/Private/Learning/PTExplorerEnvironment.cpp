// PTExplorerEnvironment.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Learning/PTExplorerEnvironment.cpp

#include "Learning/PTExplorerEnvironment.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "LearningAgentsCompletions.h"

void UPTExplorerEnvironment::GatherAgentReward_Implementation(float& OutReward, const int32 AgentId)
{
	// Step 1 placeholder: small reward for horizontal speed, so training has a signal to learn from.
	OutReward = 0.f;
	if (const APawn* Bot = Cast<APawn>(GetAgent(AgentId)))
	{
		const float Speed = Bot->GetVelocity().Size2D();
		OutReward = 0.01f * FMath::Clamp(Speed / 500.f, 0.f, 1.f);
	}
}

void UPTExplorerEnvironment::GatherAgentCompletion_Implementation(ELearningAgentsCompletion& OutCompletion, const int32 AgentId)
{
	OutCompletion = ELearningAgentsCompletion::Running;

	const APawn* Bot = Cast<APawn>(GetAgent(AgentId));
	if (!Bot || Bot->GetActorLocation().Z < KillHeight)
	{
		OutCompletion = ELearningAgentsCompletion::Termination;
	}
	// Truncation after MaxEpisodeStepNum is handled by the trainer settings.
}

void UPTExplorerEnvironment::ResetAgentEpisode_Implementation(const int32 AgentId)
{
	APawn* Bot = Cast<APawn>(GetAgent(AgentId));
	UWorld* World = GetWorld();
	if (!Bot || !World)
	{
		return;
	}

	// Respawn at a PlayerStart (spread by agent id) with a small offset and random yaw.
	TArray<AActor*> Starts;
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		Starts.Add(*It);
	}
	FVector Location(0.f, 0.f, 200.f);
	if (Starts.Num() > 0)
	{
		Location = Starts[AgentId % Starts.Num()]->GetActorLocation();
	}
	Location += FVector(FMath::FRandRange(-100.f, 100.f), FMath::FRandRange(-100.f, 100.f), 0.f);

	Bot->SetActorLocationAndRotation(Location, FRotator(0.f, FMath::FRandRange(0.f, 360.f), 0.f),
		/*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);

	if (const ACharacter* BotCharacter = Cast<ACharacter>(Bot))
	{
		if (UCharacterMovementComponent* Movement = BotCharacter->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
		}
	}
}
