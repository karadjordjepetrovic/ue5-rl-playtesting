// PTRandomWalkerController.h
// Baseline bot #1: drives the character with random MOVEMENT INPUT (not navmesh paths),
// plus random jumps and crouches. Input-driven movement finds collision bugs path-following never will.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Walkers/PTRandomWalkerController.h

#pragma once

#include "CoreMinimal.h"
#include "Walkers/PTBotControllerBase.h"
#include "PTRandomWalkerController.generated.h"

UCLASS()
class PLAYTESTBOTS_API APTRandomWalkerController : public APTBotControllerBase
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaSeconds) override;

	// Current action, read later by the action recorder / detectors.
	FVector GetMoveDirection() const { return MoveDirection; }
	bool IsJumpHeld() const { return JumpTimeLeft > 0.f; }
	bool IsCrouchHeld() const { return CrouchTimeLeft > 0.f; }

	// Tuning (starting values)
	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float MinSegmentTime = 1.f;            // s a direction is held

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float MaxSegmentTime = 3.f;

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float KeepHeadingChance = 0.5f;        // new segment turns at most +/-MaxHeadingChange instead of a fully random direction

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float MaxHeadingChange = 45.f;         // degrees

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float IdleChance = 0.05f;              // chance a segment is "stand still"

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float JumpChancePerSecond = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float BlockedJumpChancePerSecond = 4.f;  // extra jump chance while pushing into an obstacle (like a human tester)

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float BlockedSpeedThreshold = 250.f;     // cm/s along the input direction (~half walk speed); below this on the ground = blocked or sliding along an obstacle

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float MinJumpHold = 0.05f;             // s; varies jump height if the character supports held jumps

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float MaxJumpHold = 0.4f;

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float CrouchChancePerSecond = 0.05f;

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float MinCrouchTime = 1.f;

	UPROPERTY(EditAnywhere, Category = "Random Walker")
	float MaxCrouchTime = 3.f;

private:
	void PickNewSegment();

	FVector MoveDirection = FVector::ZeroVector;
	float HeadingYaw = 0.f;
	float SegmentTimeLeft = 0.f;
	float JumpTimeLeft = 0.f;
	float CrouchTimeLeft = 0.f;
};
