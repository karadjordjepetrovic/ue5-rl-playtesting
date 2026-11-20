// PTRandomWalkerController.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Walkers/PTRandomWalkerController.cpp

#include "Walkers/PTRandomWalkerController.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

void APTRandomWalkerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	ACharacter* BotCharacter = Cast<ACharacter>(GetPawn());
	if (!BotCharacter)
	{
		return;
	}

	// Movement: hold one direction for a while, then pick a new one.
	SegmentTimeLeft -= DeltaSeconds;
	if (SegmentTimeLeft <= 0.f)
	{
		PickNewSegment();
	}
	if (!MoveDirection.IsNearlyZero())
	{
		BotCharacter->AddMovementInput(MoveDirection, 1.f);
	}

	// Blocked = pushing on the ground but barely moving along the input direction (walked into something).
	const UCharacterMovementComponent* GroundMovement = BotCharacter->GetCharacterMovement();
	const bool bOnGround = GroundMovement && GroundMovement->IsMovingOnGround();
	const float SpeedAlongInput = FVector::DotProduct(BotCharacter->GetVelocity(), MoveDirection);
	const bool bBlocked = bOnGround && !MoveDirection.IsNearlyZero() && SpeedAlongInput < BlockedSpeedThreshold;

	// Jump: random presses with a random hold time, much more likely when blocked.
	// Note: per-frame chances depend on frame rate; fine for Phase 1, replays will use a fixed timestep.
	const float JumpChance = JumpChancePerSecond + (bBlocked ? BlockedJumpChancePerSecond : 0.f);
	if (JumpTimeLeft > 0.f)
	{
		JumpTimeLeft -= DeltaSeconds;
		if (JumpTimeLeft <= 0.f)
		{
			BotCharacter->StopJumping();
		}
	}
	else if (Random.FRand() < JumpChance * DeltaSeconds)
	{
		BotCharacter->Jump();
		JumpTimeLeft = Random.FRandRange(MinJumpHold, MaxJumpHold);
	}

	// Crouch: occasionally, only when on the ground.
	const UCharacterMovementComponent* Movement = BotCharacter->GetCharacterMovement();
	if (CrouchTimeLeft > 0.f)
	{
		CrouchTimeLeft -= DeltaSeconds;
		if (CrouchTimeLeft <= 0.f)
		{
			BotCharacter->UnCrouch();
		}
	}
	else if (Movement && !Movement->IsFalling() && Random.FRand() < CrouchChancePerSecond * DeltaSeconds)
	{
		BotCharacter->Crouch();
		CrouchTimeLeft = Random.FRandRange(MinCrouchTime, MaxCrouchTime);
	}
}

void APTRandomWalkerController::PickNewSegment()
{
	SegmentTimeLeft = Random.FRandRange(MinSegmentTime, MaxSegmentTime);

	if (Random.FRand() < IdleChance)
	{
		MoveDirection = FVector::ZeroVector;
		return;
	}

	// Mostly keep a rough heading (covers ground faster than pure Brownian motion),
	// sometimes pick a completely new one.
	if (!MoveDirection.IsNearlyZero() && Random.FRand() < KeepHeadingChance)
	{
		HeadingYaw += Random.FRandRange(-MaxHeadingChange, MaxHeadingChange);
	}
	else
	{
		HeadingYaw = Random.FRandRange(0.f, 360.f);
	}

	MoveDirection = FRotator(0.f, HeadingYaw, 0.f).Vector();
}
