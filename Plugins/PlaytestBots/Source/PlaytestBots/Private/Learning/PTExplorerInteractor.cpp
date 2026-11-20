// PTExplorerInteractor.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Learning/PTExplorerInteractor.cpp

#include "Learning/PTExplorerInteractor.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "LearningAgentsActions.h"
#include "LearningAgentsObservations.h"

// ---------------------------------------------------------------------------
// Observations (step 1: velocity in the bot's frame + grounded flag)
// ---------------------------------------------------------------------------
void UPTExplorerInteractor::SpecifyAgentObservation_Implementation(
	FLearningAgentsObservationSchemaElement& OutObservationSchemaElement,
	ULearningAgentsObservationSchema* InObservationSchema)
{
	TMap<FName, FLearningAgentsObservationSchemaElement> Elements;
	Elements.Add(TEXT("Velocity"), ULearningAgentsObservations::SpecifyVelocityObservation(InObservationSchema));
	Elements.Add(TEXT("Grounded"), ULearningAgentsObservations::SpecifyBoolObservation(InObservationSchema));

	OutObservationSchemaElement = ULearningAgentsObservations::SpecifyStructObservation(InObservationSchema, Elements);
}

void UPTExplorerInteractor::GatherAgentObservation_Implementation(
	FLearningAgentsObservationObjectElement& OutObservationObjectElement,
	ULearningAgentsObservationObject* InObservationObject,
	const int32 AgentId)
{
	FVector Velocity = FVector::ZeroVector;
	FTransform Frame = FTransform::Identity;
	bool bGrounded = false;

	if (const ACharacter* BotCharacter = Cast<ACharacter>(GetAgent(AgentId)))
	{
		Velocity = BotCharacter->GetVelocity();
		Frame = FTransform(FRotator(0.f, BotCharacter->GetActorRotation().Yaw, 0.f), BotCharacter->GetActorLocation());
		if (const UCharacterMovementComponent* Movement = BotCharacter->GetCharacterMovement())
		{
			bGrounded = Movement->IsMovingOnGround();
		}
	}

	TMap<FName, FLearningAgentsObservationObjectElement> Elements;
	Elements.Add(TEXT("Velocity"), ULearningAgentsObservations::MakeVelocityObservation(InObservationObject, Velocity, Frame));
	Elements.Add(TEXT("Grounded"), ULearningAgentsObservations::MakeBoolObservation(InObservationObject, bGrounded));

	OutObservationObjectElement = ULearningAgentsObservations::MakeStructObservation(InObservationObject, Elements);
}

// ---------------------------------------------------------------------------
// Actions (step 1: 2D move in the bot's frame + jump)
// ---------------------------------------------------------------------------
void UPTExplorerInteractor::SpecifyAgentAction_Implementation(
	FLearningAgentsActionSchemaElement& OutActionSchemaElement,
	ULearningAgentsActionSchema* InActionSchema)
{
	TMap<FName, FLearningAgentsActionSchemaElement> Elements;
	Elements.Add(TEXT("Move"), ULearningAgentsActions::SpecifyContinuousAction(InActionSchema, 2));
	Elements.Add(TEXT("Jump"), ULearningAgentsActions::SpecifyBoolAction(InActionSchema, /*PriorProbability*/ 0.1f));

	OutActionSchemaElement = ULearningAgentsActions::SpecifyStructAction(InActionSchema, Elements);
}

void UPTExplorerInteractor::PerformAgentAction_Implementation(
	const ULearningAgentsActionObject* InActionObject,
	const FLearningAgentsActionObjectElement& InActionObjectElement,
	const int32 AgentId)
{
	ACharacter* BotCharacter = Cast<ACharacter>(GetAgent(AgentId));
	if (!BotCharacter)
	{
		return;
	}

	TMap<FName, FLearningAgentsActionObjectElement> Elements;
	if (!ULearningAgentsActions::GetStructAction(Elements, InActionObject, InActionObjectElement))
	{
		return;
	}

	// Move: (forward, right) in the bot's yaw frame, clamped to unit length.
	TArray<float> MoveValues;
	if (const FLearningAgentsActionObjectElement* MoveElement = Elements.Find(TEXT("Move")))
	{
		if (ULearningAgentsActions::GetContinuousAction(MoveValues, InActionObject, *MoveElement) && MoveValues.Num() == 2)
		{
			const FRotator YawOnly(0.f, BotCharacter->GetActorRotation().Yaw, 0.f);
			FVector Local(MoveValues[0], MoveValues[1], 0.f);
			Local = Local.GetClampedToMaxSize(1.f);
			BotCharacter->AddMovementInput(YawOnly.RotateVector(Local), 1.f);
		}
	}

	// Jump: pressed while true.
	bool bJump = false;
	if (const FLearningAgentsActionObjectElement* JumpElement = Elements.Find(TEXT("Jump")))
	{
		ULearningAgentsActions::GetBoolAction(bJump, InActionObject, *JumpElement);
	}
	if (bJump)
	{
		BotCharacter->Jump();
	}
	else
	{
		BotCharacter->StopJumping();
	}
}
