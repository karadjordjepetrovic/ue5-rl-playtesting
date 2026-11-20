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

	/** Decisions happen ~10 times a second; this re-applies the last decision every frame. */
	void ApplyHeldActions();

	// Sensing settings
	int32 RayYawCount = 12;            // rays around the bot, each one horizontal + one angled down
	float HorizontalRayLength = 1500.f; // cm
	float DownRayLength = 400.f;       // cm; "no hit" on a down ray means a drop or a hole
	float DownRayPitch = -45.f;        // degrees
	int32 PatchRadius = 3;             // coverage patch is (2R+1)^2 cells around the bot
	int32 FrontierRadius = 15;         // cells searched for the least-visited reachable cell

private:
	struct FHeldAction
	{
		FVector WorldMove = FVector::ZeroVector; // fixed at decision time (the character turns to face movement)
		bool bJump = false;
		bool bCrouch = false;
	};

	TMap<int32, FHeldAction> HeldActions;

	int32 RayValueCount() const { return RayYawCount * 2; }
	int32 PatchValueCount() const { return (2 * PatchRadius + 1) * (2 * PatchRadius + 1); }
};
