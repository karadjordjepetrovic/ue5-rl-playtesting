// PTBotControllerBase.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Walkers/PTBotControllerBase.cpp

#include "Walkers/PTBotControllerBase.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

APTBotControllerBase::APTBotControllerBase()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void APTBotControllerBase::InitBot(int32 InBotIndex, int32 RunSeed)
{
	BotIndex = InBotIndex;
	Random.Initialize(static_cast<int32>(HashCombine(GetTypeHash(RunSeed), GetTypeHash(InBotIndex))));
}

void APTBotControllerBase::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	// Bots should be able to crouch even if the template character has it disabled.
	if (const ACharacter* BotCharacter = Cast<ACharacter>(InPawn))
	{
		if (UCharacterMovementComponent* Movement = BotCharacter->GetCharacterMovement())
		{
			Movement->GetNavAgentPropertiesRef().bCanCrouch = true;
		}
	}
}
