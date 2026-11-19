// PTIssueSubsystem.h
// Records every bot's recent actions, runs the issue detectors at 10 Hz and writes report.json at run end.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Detectors/PTIssueSubsystem.h

#pragma once

#include "CoreMinimal.h"
#include "Report/PTIssueTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "PTIssueSubsystem.generated.h"

struct FPTRunConfig;
class APawn;
class UPTCoverageSubsystem;
class UPTPlaytestSubsystem;

UCLASS()
class PLAYTESTBOTS_API UPTIssueSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem / UTickableWorldSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Adds an issue for a bot (attaches its action history, applies cooldown, optionally respawns it). */
	void ReportIssue(int32 BotIndex, EPTIssueType Type, const FVector& Location, const FString& Detail);

	const TArray<FPTIssueRecord>& GetIssues() const { return Issues; }
	static FString IssueTypeToString(EPTIssueType Type);

	// Settings (starting values, tune later)
	float SampleInterval = 0.1f;          // s (10 Hz)
	int32 ActionHistorySamples = 100;     // 10 s at 10 Hz
	float StuckWindowSeconds = 5.f;
	float StuckMaxDisplacement = 50.f;    // cm moved within the window
	float StuckMinIntentFraction = 0.8f;  // share of the window the bot was trying to move
	float StuckMaxDirectionConsistency = 0.5f; // 1 = always the same direction (just pushing into a wall), lower = tried different directions
	float FallMarginBelowNav = 500.f;     // cm below the lowest navmesh floor = out of world
	float IssueCooldownSeconds = 10.f;    // per bot, avoids spamming the same issue
	bool bRespawnAfterIssue = true;       // teleport the bot back to its spawn after an issue
	float ClusterSize = 200.f;            // cm grid used to group issues in the report

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FBotState
	{
		TWeakObjectPtr<APawn> Pawn;
		FTransform SpawnTransform = FTransform::Identity;
		FVector LastKnownLocation = FVector::ZeroVector;
		FVector LastGroundedLocation = FVector::ZeroVector; // where a fall started
		TArray<FPTActionSample> History; // ring buffer
		int32 HistoryHead = 0;
		int32 HistoryCount = 0;
		float CooldownUntil = 0.f;
		bool bLost = false;
	};

	void HandleRunStarted(const FPTRunConfig& Config);
	void HandleRunFinished(const FPTRunConfig& Config);

	void SampleBots();
	void PushSample(FBotState& State, const FPTActionSample& Sample) const;
	const FPTActionSample* GetSampleAgo(const FBotState& State, int32 SamplesAgo) const;
	void GetHistory(const FBotState& State, TArray<FPTActionSample>& OutHistory) const;

	bool CheckOutOfWorld(int32 BotIndex, const FPTActionSample& Now);
	bool CheckStuck(int32 BotIndex, const FPTActionSample& Now);

	void RespawnBot(FBotState& State) const;
	void WriteReport() const;
	void DrawDebug() const;

	TWeakObjectPtr<UPTPlaytestSubsystem> RunSubsystem;
	TWeakObjectPtr<UPTCoverageSubsystem> CoverageSubsystem;

	TArray<FBotState> BotStates;
	TArray<FPTIssueRecord> Issues;

	float SampleAccumulator = 0.f;
	float LowestNavZ = 0.f;
	bool bHasNavFloor = false;
	bool bActive = false;
};
