// PTPlaytestSubsystem.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/PTPlaytestSubsystem.cpp

#include "PTPlaytestSubsystem.h"

#include "AIController.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PTCollision.h"
#include "TimerManager.h"
#include "Walkers/PTBotControllerBase.h"
#include "Walkers/PTRandomWalkerController.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlaytest, Log, All);

// ---------------------------------------------------------------------------
// Console commands (type them in the in-game console: ` or ~ key during PIE)
//   pt.Start [bots] [seconds] [walker] [seed]   e.g.  pt.Start 8 30 Idle
//   pt.Stop
//   pt.Status
// ---------------------------------------------------------------------------
namespace PTConsole
{
	static UPTPlaytestSubsystem* GetSubsystem(UWorld* World)
	{
		UPTPlaytestSubsystem* Subsystem = World ? World->GetSubsystem<UPTPlaytestSubsystem>() : nullptr;
		if (!Subsystem)
		{
			UE_LOG(LogPlaytest, Warning, TEXT("No playtest subsystem in this world. Use the in-game console during PIE or a -game run."));
		}
		return Subsystem;
	}

	static FAutoConsoleCommandWithWorldAndArgs StartCommand(
		TEXT("pt.Start"),
		TEXT("Start a playtest run: pt.Start [bots] [seconds] [Idle|Random|Frontier|Policy] [seed]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UPTPlaytestSubsystem* Subsystem = GetSubsystem(World);
			if (!Subsystem)
			{
				return;
			}

			FPTRunConfig RunConfig;
			if (Args.Num() > 0) { LexFromString(RunConfig.NumBots, *Args[0]); }
			if (Args.Num() > 1) { LexFromString(RunConfig.DurationSeconds, *Args[1]); }
			if (Args.Num() > 2 && !UPTPlaytestSubsystem::ParseWalker(Args[2], RunConfig.Walker))
			{
				UE_LOG(LogPlaytest, Warning, TEXT("Unknown walker '%s', using Random."), *Args[2]);
			}
			if (Args.Num() > 3) { LexFromString(RunConfig.Seed, *Args[3]); }

			Subsystem->StartRun(RunConfig);
		}));

	static FAutoConsoleCommandWithWorldAndArgs StopCommand(
		TEXT("pt.Stop"),
		TEXT("End the current playtest run."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UPTPlaytestSubsystem* Subsystem = GetSubsystem(World))
			{
				Subsystem->EndRun();
			}
		}));

	static FAutoConsoleCommandWithWorldAndArgs StatusCommand(
		TEXT("pt.Status"),
		TEXT("Print the current playtest run state."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UPTPlaytestSubsystem* Subsystem = GetSubsystem(World))
			{
				UE_LOG(LogPlaytest, Display, TEXT("Running=%d elapsed=%.1fs bots=%d config: %s"),
					Subsystem->IsRunning(), Subsystem->GetElapsedSeconds(), Subsystem->GetBots().Num(),
					*Subsystem->GetConfig().ToString());
			}
		}));
}

// ---------------------------------------------------------------------------
// FPTRunConfig
// ---------------------------------------------------------------------------
FString FPTRunConfig::ToString() const
{
	return FString::Printf(TEXT("bots=%d duration=%.0fs seed=%d walker=%s pawn=%s quitOnEnd=%d"),
		NumBots, DurationSeconds, Seed, *UPTPlaytestSubsystem::WalkerToString(Walker),
		PawnClassPath.IsEmpty() ? TEXT("<GameMode default>") : *PawnClassPath, bQuitOnEnd);
}

// ---------------------------------------------------------------------------
// UPTPlaytestSubsystem
// ---------------------------------------------------------------------------
bool UPTPlaytestSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UPTPlaytestSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// Auto-start only when the command line asks for it (-PTBots=...). In PIE use pt.Start instead.
	FPTRunConfig CommandLineConfig;
	if (ParseCommandLine(CommandLineConfig))
	{
		StartRun(CommandLineConfig);
	}
}

void UPTPlaytestSubsystem::Deinitialize()
{
	// The world is going away and will destroy the bots itself; just stop tracking the run.
	if (bRunning)
	{
		UE_LOG(LogPlaytest, Warning, TEXT("World torn down during run %s (elapsed %.1fs)."), *RunId, GetElapsedSeconds());
		bRunning = false;
	}
	Bots.Reset();
	Super::Deinitialize();
}

bool UPTPlaytestSubsystem::ParseCommandLine(FPTRunConfig& OutConfig)
{
	const TCHAR* Cmd = FCommandLine::Get();

	const bool bRequested = FParse::Value(Cmd, TEXT("PTBots="), OutConfig.NumBots);
	FParse::Value(Cmd, TEXT("PTDuration="), OutConfig.DurationSeconds);
	FParse::Value(Cmd, TEXT("PTSeed="), OutConfig.Seed);
	FParse::Value(Cmd, TEXT("PTPawn="), OutConfig.PawnClassPath);

	FString WalkerText;
	if (FParse::Value(Cmd, TEXT("PTWalker="), WalkerText) && !ParseWalker(WalkerText, OutConfig.Walker))
	{
		UE_LOG(LogPlaytest, Warning, TEXT("Unknown -PTWalker=%s, using Random."), *WalkerText);
	}

	// Standalone and headless runs should exit by themselves; PIE should not close the editor.
	OutConfig.bQuitOnEnd = FParse::Param(Cmd, TEXT("PTQuitOnEnd")) || !GIsEditor;

	return bRequested;
}

void UPTPlaytestSubsystem::StartRun(const FPTRunConfig& InConfig)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (bRunning)
	{
		UE_LOG(LogPlaytest, Warning, TEXT("Run %s already in progress. Use pt.Stop first."), *RunId);
		return;
	}

	Config = InConfig;
	Config.NumBots = FMath::Clamp(Config.NumBots, 0, 256);
	Config.DurationSeconds = FMath::Max(Config.DurationSeconds, 1.f);

	Random.Initialize(Config.Seed);

	const FString MapName = UWorld::RemovePIEPrefix(World->GetMapName());
	RunId = FString::Printf(TEXT("%s_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), *MapName);
	RunStartTime = World->GetTimeSeconds();
	bRunning = true;

	UE_LOG(LogPlaytest, Display, TEXT("Run %s starting: %s"), *RunId, *Config.ToString());

	SpawnBots();

	World->GetTimerManager().SetTimer(EndTimerHandle, this, &UPTPlaytestSubsystem::EndRun, Config.DurationSeconds, false);

	OnRunStarted.Broadcast(Config);
}

void UPTPlaytestSubsystem::EndRun()
{
	if (!bRunning)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World)
	{
		World->GetTimerManager().ClearTimer(EndTimerHandle);
	}

	UE_LOG(LogPlaytest, Display, TEXT("Run %s finished after %.1fs with %d bots. Output dir: %s"),
		*RunId, GetElapsedSeconds(), Bots.Num(), *GetRunOutputDir());

	// Listeners (reporter, coverage grid) read the bots and run state before teardown.
	OnRunFinished.Broadcast(Config);

	bRunning = false;
	DestroyBots();

	if (Config.bQuitOnEnd && !GIsEditor)
	{
		UE_LOG(LogPlaytest, Display, TEXT("Quitting (bQuitOnEnd)."));
		FPlatformMisc::RequestExit(false, TEXT("PlaytestBots.EndRun"));
	}
}

float UPTPlaytestSubsystem::GetElapsedSeconds() const
{
	const UWorld* World = GetWorld();
	return (bRunning && World) ? static_cast<float>(World->GetTimeSeconds() - RunStartTime) : 0.f;
}

FString UPTPlaytestSubsystem::GetRunOutputDir() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Playtest"), RunId);
}

FString UPTPlaytestSubsystem::WalkerToString(EPTWalkerType Walker)
{
	switch (Walker)
	{
	case EPTWalkerType::Idle:     return TEXT("Idle");
	case EPTWalkerType::Random:   return TEXT("Random");
	case EPTWalkerType::Frontier: return TEXT("Frontier");
	case EPTWalkerType::Policy:   return TEXT("Policy");
	default:                      return TEXT("Unknown");
	}
}

bool UPTPlaytestSubsystem::ParseWalker(const FString& Text, EPTWalkerType& OutWalker)
{
	if (Text.Equals(TEXT("Idle"), ESearchCase::IgnoreCase))     { OutWalker = EPTWalkerType::Idle;     return true; }
	if (Text.Equals(TEXT("Random"), ESearchCase::IgnoreCase))   { OutWalker = EPTWalkerType::Random;   return true; }
	if (Text.Equals(TEXT("Frontier"), ESearchCase::IgnoreCase)) { OutWalker = EPTWalkerType::Frontier; return true; }
	if (Text.Equals(TEXT("Policy"), ESearchCase::IgnoreCase))   { OutWalker = EPTWalkerType::Policy;   return true; }
	return false;
}

UClass* UPTPlaytestSubsystem::ResolvePawnClass() const
{
	if (!Config.PawnClassPath.IsEmpty())
	{
		UClass* PawnClass = LoadClass<APawn>(nullptr, *Config.PawnClassPath);
		if (!PawnClass)
		{
			UE_LOG(LogPlaytest, Error, TEXT("Could not load pawn class '%s'."), *Config.PawnClassPath);
		}
		return PawnClass;
	}

	const UWorld* World = GetWorld();
	const AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr;
	if (GameMode && GameMode->DefaultPawnClass)
	{
		return GameMode->DefaultPawnClass.Get();
	}

	UE_LOG(LogPlaytest, Error, TEXT("No pawn class: set -PTPawn=... or a DefaultPawnClass on the GameMode."));
	return nullptr;
}

TSubclassOf<AController> UPTPlaytestSubsystem::ResolveControllerClass() const
{
	switch (Config.Walker)
	{
	case EPTWalkerType::Random:
		return APTRandomWalkerController::StaticClass();
	// TODO step 5: return APTFrontierWalkerController::StaticClass();
	case EPTWalkerType::Frontier:
	// TODO Phase 2: Learning Agents controller
	case EPTWalkerType::Policy:
	case EPTWalkerType::Idle:
	default:
		return AAIController::StaticClass();
	}
}

void UPTPlaytestSubsystem::SpawnBots()
{
	UWorld* World = GetWorld();

	TArray<AActor*> PlayerStarts;
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		PlayerStarts.Add(*It);
	}
	if (PlayerStarts.IsEmpty())
	{
		UE_LOG(LogPlaytest, Warning, TEXT("No PlayerStart in map; spawning bots at the world origin."));
	}

	if (!ResolvePawnClass())
	{
		return;
	}

	for (int32 Index = 0; Index < Config.NumBots; ++Index)
	{
		if (APawn* Bot = SpawnBot(Index, PlayerStarts))
		{
			Bots.Add(Bot);
		}
	}

	UE_LOG(LogPlaytest, Display, TEXT("Spawned %d/%d bots at %d PlayerStart(s)."), Bots.Num(), Config.NumBots, PlayerStarts.Num());
}

APawn* UPTPlaytestSubsystem::SpawnBot(int32 Index, const TArray<AActor*>& PlayerStarts)
{
	UWorld* World = GetWorld();
	UClass* PawnClass = ResolvePawnClass();
	if (!World || !PawnClass)
	{
		return nullptr;
	}

	// Round-robin over PlayerStarts, small random jitter so bots don't start on the exact same spot.
	FVector Location(0.f, 0.f, 200.f);
	if (PlayerStarts.Num() > 0)
	{
		Location = PlayerStarts[Index % PlayerStarts.Num()]->GetActorLocation();
	}
	Location += FVector(Random.FRandRange(-100.f, 100.f), Random.FRandRange(-100.f, 100.f), 0.f);
	const FRotator Rotation(0.f, Random.FRandRange(0.f, 360.f), 0.f);

	FActorSpawnParameters Params;
	// Bots ignore each other once the PlaytestBot profile is applied, so overlap at spawn is fine.
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	APawn* Bot = World->SpawnActor<APawn>(PawnClass, Location, Rotation, Params);
	if (!Bot)
	{
		UE_LOG(LogPlaytest, Warning, TEXT("Failed to spawn bot %d."), Index);
		return nullptr;
	}

	Bot->Tags.Add(TEXT("PlaytestBot"));

	if (ACharacter* Character = Cast<ACharacter>(Bot))
	{
		Character->GetCapsuleComponent()->SetCollisionProfileName(PTCollisionProfile_Bot);
	}

	Bot->AIControllerClass = ResolveControllerClass();
	Bot->SpawnDefaultController();

	// Give each bot brain its index and a seed derived from the run seed.
	if (APTBotControllerBase* BotController = Cast<APTBotControllerBase>(Bot->GetController()))
	{
		BotController->InitBot(Index, Config.Seed);
	}

	return Bot;
}

void UPTPlaytestSubsystem::DestroyBots()
{
	for (const TWeakObjectPtr<APawn>& BotPtr : Bots)
	{
		if (APawn* Bot = BotPtr.Get())
		{
			AController* Controller = Bot->GetController();
			Bot->Destroy();
			if (Controller)
			{
				Controller->Destroy();
			}
		}
	}
	Bots.Reset();
}