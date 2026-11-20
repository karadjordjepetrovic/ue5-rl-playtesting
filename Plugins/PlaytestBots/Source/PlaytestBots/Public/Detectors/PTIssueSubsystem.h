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

	/** Adds an issue. With bAffectsBot, attaches the bot's action history, applies cooldown and optionally respawns it.
	 *  BotIndex may be INDEX_NONE for issues not tied to a bot (e.g. perf spikes seen from the player camera). */
	void ReportIssue(int32 BotIndex, EPTIssueType Type, const FVector& Location, const FString& Detail, bool bAffectsBot = true);

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
	float MaxFallSeconds = 3.f;           // falling continuously this long = out of world (works without a navmesh)
	float IssueCooldownSeconds = 10.f;    // per bot, avoids spamming the same issue
	bool bRespawnAfterIssue = true;       // teleport the bot back to its spawn after an issue
	float ClusterSize = 200.f;            // cm grid used to group issues in the report

	// Clip detector
	float ClipMaxStepDistance = 300.f;    // cm between two samples; larger = teleport, not a clip
	float IgnoreMeshesLargerThan = 5000.f; // cm; skips sky spheres and similar huge meshes

	// Perf detector (rendered runs only)
	float PerfWindowSeconds = 0.5f;       // frame times are averaged over this window
	float PerfWarmupSeconds = 5.f;        // ignore shader-compile hitches at run start
	float PerfSpikeFactor = 1.5f;         // window average vs. running baseline
	float PerfMinSpikeMs = 4.f;           // and at least this many ms above baseline
	float PerfCooldownSeconds = 5.f;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FNoCollisionMesh
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		FTransform Transform = FTransform::Identity;
		FBox LocalBox = FBox(ForceInit);
		FString Name;
	};

	struct FForbiddenZone
	{
		TWeakObjectPtr<AActor> Actor;
		FTransform Transform = FTransform::Identity; // box transform without scale
		FVector Extent = FVector::ZeroVector;        // scaled half-size
		FString Name;
	};

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
	bool CheckClip(int32 BotIndex, const FPTActionSample& Prev, const FPTActionSample& Now);
	bool CheckForbidden(int32 BotIndex, const FPTActionSample& Now);
	void TickPerf();
	void TickViewCycling(float DeltaTime);

	void GatherClipCandidates();
	void GatherForbiddenZones();

	void RespawnBot(FBotState& State) const;
	void WriteReport() const;
	void DrawDebug() const;

	TWeakObjectPtr<UPTPlaytestSubsystem> RunSubsystem;
	TWeakObjectPtr<UPTCoverageSubsystem> CoverageSubsystem;

	TArray<FBotState> BotStates;
	TArray<FPTIssueRecord> Issues;
	TArray<FNoCollisionMesh> NoCollisionMeshes;
	TArray<FForbiddenZone> ForbiddenZones;

	// Perf state
	double PerfWindowTime = 0.0;
	int32 PerfWindowFrames = 0;
	float PerfBaselineMs = 0.f;
	TArray<float> PerfRecentMs;   // last ~30 s of window averages, baseline = their median
	float PerfCooldownUntil = 0.f;

	// View cycling (pt.ViewBots)
	float ViewSwitchTimer = 0.f;
	int32 ViewBotIndex = INDEX_NONE;

	float SampleAccumulator = 0.f;
	float LowestNavZ = 0.f;
	bool bHasNavFloor = false;
	bool bActive = false;
};
