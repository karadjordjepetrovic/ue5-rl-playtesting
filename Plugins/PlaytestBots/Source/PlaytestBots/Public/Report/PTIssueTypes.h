// PTIssueTypes.h
// Data written to report.json: one record per detected issue, including the bot's recent action history.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Report/PTIssueTypes.h

#pragma once

#include "CoreMinimal.h"
#include "PTIssueTypes.generated.h"

UENUM(BlueprintType)
enum class EPTIssueType : uint8
{
	Stuck,
	OutOfWorld,
	Clip,           // next step
	ForbiddenArea,  // next step
	PerfSpike       // next step
};

/** One 10 Hz sample of what a bot was doing. The last ~10 s are attached to every issue for replay. */
USTRUCT(BlueprintType)
struct PLAYTESTBOTS_API FPTActionSample
{
	GENERATED_BODY()

	UPROPERTY() float Time = 0.f;
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FVector Velocity = FVector::ZeroVector;
	UPROPERTY() FVector MoveInput = FVector::ZeroVector;
	UPROPERTY() bool bMoveIntent = false;   // input held, or path following active
	UPROPERTY() bool bJumping = false;
	UPROPERTY() bool bCrouched = false;
	UPROPERTY() bool bFalling = false;
};

USTRUCT(BlueprintType)
struct PLAYTESTBOTS_API FPTIssueRecord
{
	GENERATED_BODY()

	UPROPERTY() EPTIssueType Type = EPTIssueType::Stuck;
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;
	UPROPERTY() float Time = 0.f;
	UPROPERTY() int32 BotIndex = INDEX_NONE;
	UPROPERTY() int32 Seed = 0;
	UPROPERTY() FString Walker;
	UPROPERTY() FString Detail;
	UPROPERTY() FTransform SpawnTransform = FTransform::Identity;
	UPROPERTY() TArray<FPTActionSample> Actions;
};
