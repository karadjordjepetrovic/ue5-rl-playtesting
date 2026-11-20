// PTExplorerInteractor.h
// What an explorer bot senses (observations) and what it can do (actions).
// Phase 2 step 1: minimal version for the training smoke test. Step 2 adds raycasts and the coverage patch.
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
};
