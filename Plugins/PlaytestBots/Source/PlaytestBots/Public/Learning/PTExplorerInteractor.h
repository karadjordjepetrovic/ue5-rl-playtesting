// PTExplorerInteractor.h
// What an explorer bot senses (observations) and what it can do (actions).
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Learning/PTExplorerInteractor.h

#pragma once

#include "CoreMinimal.h"
#include "LearningAgentsInteractor.h"
#include "PTExplorerInteractor.generated.h"

UCLASS()
class PLAYTESTBOTS_API UPTExplorerInteractor : public ULearningAgentsInteractor
{
	GENERATED_BODY()

public:
	virtual void SpecifyAgentObservation_Implementation(
		FLearningAgentsObservationSchemaElement& OutObservationSchemaElement,
		ULearningAgentsObservationSchema* InObservationSchema) override;

	virtual void GatherAgentObservation_Implementation(
		FLearningAgentsObservationObjectElement& OutObservationObjectElement,
		ULearningAgentsObservationObject* InObservationObject,
		const int32 AgentId) override;

	virtual void SpecifyAgentAction_Implementation(
		FLearningAgentsActionSchemaElement& OutActionSchemaElement,
		ULearningAgentsActionSchema* InActionSchema) override;

	virtual void PerformAgentAction_Implementation(
		const ULearningAgentsActionObject* InActionObject,
		const FLearningAgentsActionObjectElement& InActionObjectElement,
		const int32 AgentId) override;

	/** Is a jump in direction Dir (unit, horizontal) worth it? Ledge: something taller than a step but lower than
	 *  the max jump height within Distance. Gap: no floor just ahead but floor again within jumping distance.
	 *  Shared by the "Jumpable" observation and the training environment's jump cost. */
	static void ProbeJump(const class ACharacter* Bot, const FVector& Dir, float Distance, bool& bOutLedge, bool& bOutGap);

	/** Decisions happen ~10 times a second; this re-applies the last decision every frame. */
	void ApplyHeldActions();

	/** Describes the observation/action layout; a saved model only loads if this matches. Bump "v1" when changing them. */
	FString GetSchemaSignature() const
	{
		return FString::Printf(TEXT("explorer-v3 rays=%d patch=%d frontier=4 nearestbot=4 jumpable=%d velocity=3 grounded=1 crouched=1 | move=2 jump crouch"),
			RayValueCount(), PatchValueCount(), JumpProbeCount * 2);
	}

	// Sensing settings
	int32 RayYawCount = 12;            // rays around the bot, each one horizontal + one angled down
	float HorizontalRayLength = 1500.f; // cm
	float DownRayLength = 400.f;       // cm; "no hit" on a down ray means a drop or a hole
	float DownRayPitch = -45.f;        // degrees
	int32 PatchRadius = 3;             // coverage patch is (2R+1)^2 cells around the bot
	int32 FrontierRadius = 15;         // cells searched for the least-visited reachable cell
	int32 ClaimRadius = 4;             // cells; a frontier this close to another bot's target counts as taken ...
	float ClaimPenalty = 3.f;          // ... and scores as if it had this many extra visits (stops bots chasing the same target in a row)
	float NeighbourRange = 1000.f;
	int32 JumpProbeCount = 4;          // directions (forward, right, back, left) checked for "worth jumping": a ledge or a gap
	float JumpProbeDistance = 200.f;   // cm ahead     // cm; nearest-bot observation range (beyond it: "nobody near")

private:
	struct FHeldAction
	{
		FVector WorldMove = FVector::ZeroVector; // fixed at decision time (the character turns to face movement)
		bool bJump = false;
		bool bCrouch = false;
	};

	TMap<int32, FHeldAction> HeldActions;

	/** Frontier cell each bot is currently heading for, so other bots pick different ones. */
	TMap<int32, FIntVector> FrontierClaims;

	int32 RayValueCount() const { return RayYawCount * 2; }
	int32 PatchValueCount() const { return (2 * PatchRadius + 1) * (2 * PatchRadius + 1); }
};
