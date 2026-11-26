// PTGymLayout.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Gym/PTGymLayout.cpp

#include "Gym/PTGymLayout.h"

#include "AI/NavigationSystemBase.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Coverage/PTCoverageSubsystem.h"
#include "Detectors/PTIssueSubsystem.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/TriggerBox.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NavigationSystem.h"
#include "PTPlaytestSubsystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlaytestGym, Log, All);

namespace PTGym
{
	static constexpr float FloorThickness = 20.f;
	static constexpr float PerimeterThickness = 50.f;
	static constexpr int32 StairSteps = 8;
	static constexpr float ShaftHalfGap = 60.f; // 1.2 m wide
	static constexpr float ShaftDepth = 200.f;
	static constexpr float MinReachableFraction = 0.9f;

	static const FIntPoint Directions[4] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
}

// Regenerate every gym in the world: pt.GymRegenerate [seed]
static FAutoConsoleCommandWithWorldAndArgs GPTGymRegenerateCommand(
	TEXT("pt.GymRegenerate"),
	TEXT("Builds a new layout for every APTGymLayout in the world. Usage: pt.GymRegenerate [seed]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}
		const int32 BaseSeed = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : FMath::RandRange(0, 1000000);
		int32 GymIndex = 0;
		for (TActorIterator<APTGymLayout> It(World); It; ++It)
		{
			It->Generate(BaseSeed + GymIndex++ * 1000003);
		}
	}));

APTGymLayout::APTGymLayout()
{
	Blocks = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Blocks"));
	Blocks->SetMobility(EComponentMobility::Movable);
	Blocks->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	RootComponent = Blocks;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	CubeMesh = Cube.Object;
	Blocks->SetStaticMesh(CubeMesh);
}

void APTGymLayout::BeginPlay()
{
	Super::BeginPlay();
	OnPostGymReset.AddDynamic(this, &APTGymLayout::HandleGymReset);

	int32 Seed = LayoutSeed;
	int32 CommandLineSeed = 0;
	if (Seed < 0 && FParse::Value(FCommandLine::Get(), TEXT("PTGymSeed="), CommandLineSeed))
	{
		Seed = static_cast<int32>(HashCombine(GetTypeHash(CommandLineSeed), GetTypeHash(GetActorLocation())) & 0x7FFFFFFF);
	}
	if (Seed < 0)
	{
		Seed = static_cast<int32>(HashCombine(GetTypeHash(RandomSeed), GetTypeHash(GetActorLocation())) & 0x7FFFFFFF);
	}
	Generate(Seed);
}

void APTGymLayout::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearGeometry();
	Super::EndPlay(EndPlayReason);
}

void APTGymLayout::HandleGymReset()
{
	if (!bRegenerateOnReset)
	{
		return;
	}
	++GenerationCount;
	const int32 NextSeed = RandomStream.IsValid()
		? RandomStream->RandRange(0, MAX_int32 - 1)
		: static_cast<int32>(HashCombine(GetTypeHash(CurrentSeed), GetTypeHash(GenerationCount)) & 0x7FFFFFFF);
	Generate(NextSeed);
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------
void APTGymLayout::Generate(int32 Seed)
{
	ClearGeometry();

	bool bConnected = false;
	for (int32 Attempt = 0; Attempt < 20 && !bConnected; ++Attempt)
	{
		FRandomStream Rng(Seed + Attempt * 7919);
		bConnected = PlanLayout(Rng);
		if (bConnected || Attempt == 19)
		{
			PlaceBugs(Rng);
		}
	}
	CurrentSeed = Seed;

	BuildGeometry();
	FNavigationSystem::UpdateComponentData(*Blocks);

	if (UWorld* World = GetWorld())
	{
		if (UPTIssueSubsystem* Issues = World->GetSubsystem<UPTIssueSubsystem>())
		{
			Issues->RefreshLevelMarkers(); // new no-collision meshes and forbidden volumes
		}
		if (UPTCoverageSubsystem* Coverage = World->GetSubsystem<UPTCoverageSubsystem>())
		{
			FVector Min, Max;
			GetGymExtents(Min, Max);
			Coverage->OnLevelChanged(FBox(Min, Max)); // old visits are meaningless; re-sample reachable area
		}
		if (UPTPlaytestSubsystem* Run = World->GetSubsystem<UPTPlaytestSubsystem>())
		{
			Run->HandleGymRegenerated(this); // move this gym's bots onto the new floor
		}
	}

	int32 BugCount = 0;
	FString BugList;
	for (const FMarker& Marker : Markers)
	{
		if (!Marker.AcceptedIssues.IsEmpty())
		{
			++BugCount;
			BugList += TEXT(" ") + Marker.Id;
		}
	}
	UE_LOG(LogPlaytestGym, Display, TEXT("Gym %s: seed %d, %d x %d tiles, %d reachable, %d bugs:%s%s"),
		*GetName(), Seed, GridSize, GridSize, ReachableTiles.Num(), BugCount, *BugList,
		bConnected ? TEXT("") : TEXT(" (WARNING: connectivity check failed, using the last attempt)"));

	// Keep the answer key of every layout for inspection.
	if (GetWorld())
	{
		TArray<TSharedPtr<FJsonValue>> BugsJson, ControlsJson;
		AppendGroundTruth(BugsJson, ControlsJson);
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("gym"), GetName());
		Root->SetNumberField(TEXT("seed"), Seed);
		Root->SetArrayField(TEXT("bugs"), BugsJson);
		Root->SetArrayField(TEXT("controls"), ControlsJson);
		FString Output;
		FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Output));
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Playtest"), TEXT("GymLayouts"));
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		FFileHelper::SaveStringToFile(Output, *FPaths::Combine(Dir, FString::Printf(TEXT("%s_seed%d.ground_truth.json"), *GetName(), Seed)));
	}
}

bool APTGymLayout::IsCenterTile(const FIntPoint& P) const
{
	// The 2 x 2 (even grid) or 1 x 1 (odd grid) tiles around the gym centre stay open floor: the spawn area.
	return FMath::Abs(2 * P.X - (GridSize - 1)) <= 1 && FMath::Abs(2 * P.Y - (GridSize - 1)) <= 1;
}

FVector APTGymLayout::TileCenterLocal(const FIntPoint& P) const
{
	return FVector((P.X + 0.5f) * TileSize - HalfSize(), (P.Y + 0.5f) * TileSize - HalfSize(), 0.f);
}

bool APTGymLayout::PlanLayout(FRandomStream& Rng)
{
	Tiles.Reset();
	Tiles.SetNum(GridSize * GridSize);
	MissingPerimeter.Reset();
	Markers.Reset();
	ReachableTiles.Reset();

	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			At(FIntPoint(X, Y)).bReserved = IsCenterTile(FIntPoint(X, Y));
		}
	}

	// Platforms (2 x 2, raised) with one ramp or staircase each.
	const int32 PlatformCount = Rng.RandRange(PlatformCountRange.X, PlatformCountRange.Y);
	for (int32 Placed = 0, Tries = 0; Placed < PlatformCount && Tries < 60; ++Tries)
	{
		const FIntPoint Origin(Rng.RandRange(0, GridSize - 2), Rng.RandRange(0, GridSize - 2));
		bool bFree = true;
		for (int32 Dy = 0; Dy < 2 && bFree; ++Dy)
		{
			for (int32 Dx = 0; Dx < 2 && bFree; ++Dx)
			{
				const FTile& T = At(Origin + FIntPoint(Dx, Dy));
				bFree = T.Type == ETile::Floor && !T.bReserved;
			}
		}
		if (!bFree)
		{
			continue;
		}

		TArray<TPair<FIntPoint, FIntPoint>> Candidates; // ramp tile, up direction
		for (int32 K = 0; K < 2; ++K)
		{
			Candidates.Add({ Origin + FIntPoint(K, -1), FIntPoint(0, 1) });
			Candidates.Add({ Origin + FIntPoint(K, 2), FIntPoint(0, -1) });
			Candidates.Add({ Origin + FIntPoint(-1, K), FIntPoint(1, 0) });
			Candidates.Add({ Origin + FIntPoint(2, K), FIntPoint(-1, 0) });
		}
		Candidates.RemoveAll([this](const TPair<FIntPoint, FIntPoint>& C)
		{
			const FIntPoint Entry = C.Key - C.Value;
			return !InBounds(C.Key) || !InBounds(Entry) || At(C.Key).Type != ETile::Floor || At(C.Key).bReserved ||
				At(Entry).Type != ETile::Floor;
		});
		if (Candidates.IsEmpty())
		{
			continue;
		}

		for (int32 Dy = 0; Dy < 2; ++Dy)
		{
			for (int32 Dx = 0; Dx < 2; ++Dx)
			{
				At(Origin + FIntPoint(Dx, Dy)).Type = ETile::Platform;
			}
		}
		const TPair<FIntPoint, FIntPoint>& Choice = Candidates[Rng.RandHelper(Candidates.Num())];
		FTile& Access = At(Choice.Key);
		Access.Type = Rng.FRand() < 0.5f ? ETile::Ramp : ETile::Stairs;
		Access.Up = Choice.Value;
		Access.bReserved = true;
		At(Choice.Key - Choice.Value).bReserved = true; // keep the bottom of the ramp open
		++Placed;
	}

	// Interior wall runs.
	const int32 WallRuns = Rng.RandRange(WallRunCountRange.X, WallRunCountRange.Y);
	for (int32 Run = 0; Run < WallRuns; ++Run)
	{
		const FIntPoint Start(Rng.RandRange(0, GridSize - 1), Rng.RandRange(0, GridSize - 1));
		const FIntPoint Dir = Rng.RandBool() ? FIntPoint(1, 0) : FIntPoint(0, 1);
		const int32 Length = Rng.RandRange(2, 4);
		for (int32 Step = 0; Step < Length; ++Step)
		{
			const FIntPoint P = Start + Dir * Step;
			if (!InBounds(P))
			{
				break;
			}
			FTile& T = At(P);
			if (T.Type == ETile::Floor && !T.bReserved)
			{
				T.Type = ETile::Wall;
			}
		}
	}

	// Small features on the remaining floor.
	for (FTile& T : Tiles)
	{
		if (T.Type != ETile::Floor || T.bReserved)
		{
			continue;
		}
		const float Roll = Rng.FRand();
		if (Roll < LedgeChance)
		{
			T.Type = ETile::Ledge;
			T.Height = Rng.FRandRange(LedgeHeightRange.X, LedgeHeightRange.Y);
		}
		else if (Roll < LedgeChance + PillarChance)
		{
			T.Type = ETile::Pillar;
		}
		else if (Roll < LedgeChance + PillarChance + LowCeilingChance)
		{
			T.Type = ETile::LowCeiling;
		}
	}

	return ComputeReachability();
}

bool APTGymLayout::ComputeReachability()
{
	auto IsLevelZero = [](ETile Type)
	{
		return Type == ETile::Floor || Type == ETile::Ledge || Type == ETile::Pillar || Type == ETile::LowCeiling;
	};
	auto IsAccess = [](ETile Type) { return Type == ETile::Ramp || Type == ETile::Stairs; };

	auto CanMove = [&](const FIntPoint& From, const FIntPoint& To) -> bool
	{
		const FTile& A = At(From);
		const FTile& B = At(To);
		if (B.Type == ETile::Wall)
		{
			return false;
		}
		if (IsAccess(A.Type))
		{
			return To == From + A.Up ? B.Type == ETile::Platform : IsLevelZero(B.Type);
		}
		if (A.Type == ETile::Platform)
		{
			if (B.Type == ETile::Platform || IsLevelZero(B.Type))
			{
				return true; // walking along the platform, or dropping off it
			}
			return IsAccess(B.Type) && To + B.Up == From;
		}
		// Level zero
		if (IsAccess(B.Type))
		{
			return To + B.Up != From;
		}
		return IsLevelZero(B.Type);
	};

	TArray<FIntPoint> Queue;
	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			FTile& T = At(FIntPoint(X, Y));
			T.bReachable = false;
			if (IsCenterTile(FIntPoint(X, Y)))
			{
				T.bReachable = true;
				Queue.Add(FIntPoint(X, Y));
			}
		}
	}
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const FIntPoint From = Queue[Head];
		for (const FIntPoint& Dir : PTGym::Directions)
		{
			const FIntPoint To = From + Dir;
			if (InBounds(To) && !At(To).bReachable && CanMove(From, To))
			{
				At(To).bReachable = true;
				Queue.Add(To);
			}
		}
	}

	int32 Open = 0;
	for (const FTile& T : Tiles)
	{
		Open += T.Type != ETile::Wall ? 1 : 0;
	}
	if (Queue.Num() < FMath::CeilToInt(Open * PTGym::MinReachableFraction))
	{
		return false;
	}

	// Seal the few unreachable pockets, so they don't count as level the bots "should" cover.
	for (FTile& T : Tiles)
	{
		if (!T.bReachable && T.Type != ETile::Wall)
		{
			T.Type = ETile::Wall;
		}
	}
	return true;
}

void APTGymLayout::PlaceBugs(FRandomStream& Rng)
{
	auto IsFeature = [](const FTile& T)
	{
		return T.Type == ETile::Ledge || T.Type == ETile::Pillar || T.Type == ETile::LowCeiling ||
			T.Type == ETile::Platform || T.Type == ETile::Ramp || T.Type == ETile::Stairs || T.Bug != EBug::None;
	};
	// Bugs keep a tile of distance from other bugs and from features (so scoring stays unambiguous).
	auto IsIsolated = [&](const FIntPoint& P)
	{
		for (int32 Dy = -1; Dy <= 1; ++Dy)
		{
			for (int32 Dx = -1; Dx <= 1; ++Dx)
			{
				const FIntPoint Q = P + FIntPoint(Dx, Dy);
				if ((Dx != 0 || Dy != 0) && InBounds(Q) && IsFeature(At(Q)))
				{
					return false;
				}
			}
		}
		return true;
	};
	auto IsOpenFloor = [&](const FIntPoint& P)
	{
		const FTile& T = At(P);
		return T.Type == ETile::Floor && T.bReachable && !T.bReserved && T.Bug == EBug::None && !IsCenterTile(P);
	};

	const int32 BugCount = Rng.RandRange(BugCountRange.X, BugCountRange.Y);
	for (int32 Placed = 0, Tries = 0; Placed < BugCount && Tries < 200; ++Tries)
	{
		const EBug Bug = static_cast<EBug>(Rng.RandRange(1, 5));
		const FIntPoint P(Rng.RandRange(0, GridSize - 1), Rng.RandRange(0, GridSize - 1));
		FTile& T = At(P);
		const FVector C = TileCenterLocal(P);
		const FString Suffix = FString::Printf(TEXT("%d_%d"), P.X, P.Y);

		switch (Bug)
		{
		case EBug::FloorHole:
			if (!IsOpenFloor(P) || !IsIsolated(P)) continue;
			T.Bug = Bug;
			AddMarker(TEXT("floor_hole_") + Suffix, { TEXT("OutOfWorld"), TEXT("Clip") }, C, 350.f,
				TEXT("Floor tile is visible but has no collision; bots fall through."));
			break;

		case EBug::Shaft:
			if (!IsOpenFloor(P) || !IsIsolated(P)) continue;
			T.Bug = Bug;
			AddMarker(TEXT("stuck_shaft_") + Suffix, { TEXT("Stuck") }, C - FVector(0.f, 0.f, PTGym::ShaftDepth), 250.f,
				TEXT("Narrow 2 m deep shaft; a bot that falls in cannot get out."));
			break;

		case EBug::ForbiddenLedge:
			if (!IsOpenFloor(P) || !IsIsolated(P)) continue;
			T.Type = ETile::Ledge;
			T.Height = Rng.FRandRange(LedgeHeightRange.X, LedgeHeightRange.Y);
			T.Bug = Bug;
			AddMarker(TEXT("forbidden_ledge_") + Suffix, { TEXT("ForbiddenArea") }, C + FVector(0.f, 0.f, T.Height), 250.f,
				TEXT("Ledge designers did not intend to be reachable; reachable only with a jump."));
			break;

		case EBug::ClipWall:
		{
			if (T.Type != ETile::Wall || T.Bug != EBug::None || !IsIsolated(P)) continue;
			bool bNextToOpen = false;
			for (const FIntPoint& Dir : PTGym::Directions)
			{
				bNextToOpen |= InBounds(P + Dir) && At(P + Dir).bReachable && At(P + Dir).Type != ETile::Wall;
			}
			if (!bNextToOpen) continue;
			T.Bug = Bug;
			AddMarker(TEXT("clip_wall_") + Suffix, { TEXT("Clip") }, C + FVector(0.f, 0.f, WallHeight * 0.5f), 350.f,
				TEXT("Wall block with no collision; bots walk through it."));
			break;
		}

		case EBug::MissingPerimeter:
		{
			if (!IsOpenFloor(P) || !IsIsolated(P)) continue;
			TArray<int32> Sides;
			if (P.X == 0) Sides.Add(0);
			if (P.X == GridSize - 1) Sides.Add(1);
			if (P.Y == 0) Sides.Add(2);
			if (P.Y == GridSize - 1) Sides.Add(3);
			if (Sides.IsEmpty()) continue;
			const int32 Side = Sides[Rng.RandHelper(Sides.Num())];
			T.Bug = Bug;
			MissingPerimeter.Add(FIntVector(P.X, P.Y, Side));
			const FVector Outward = Side == 0 ? FVector(-1, 0, 0) : Side == 1 ? FVector(1, 0, 0) : Side == 2 ? FVector(0, -1, 0) : FVector(0, 1, 0);
			AddMarker(TEXT("edge_gap_") + Suffix, { TEXT("OutOfWorld") }, C + Outward * (TileSize * 0.5f), 500.f,
				TEXT("Missing perimeter wall section; bots walk off the edge."));
			break;
		}

		default:
			continue;
		}
		++Placed;
	}

	// Control areas: normal traversal where detectors must stay silent.
	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			const FIntPoint P(X, Y);
			const FTile& T = At(P);
			if (!T.bReachable || T.Bug != EBug::None)
			{
				continue;
			}
			const FString Suffix = FString::Printf(TEXT("%d_%d"), X, Y);
			switch (T.Type)
			{
			case ETile::Ramp:       AddMarker(TEXT("ramp_") + Suffix, {}, TileCenterLocal(P), 300.f, TEXT("Ramp up to a platform.")); break;
			case ETile::Stairs:     AddMarker(TEXT("stairs_") + Suffix, {}, TileCenterLocal(P), 300.f, TEXT("Stairs up to a platform.")); break;
			case ETile::Ledge:      AddMarker(TEXT("ledge_") + Suffix, {}, TileCenterLocal(P), 250.f, TEXT("Jumpable ledge (allowed).")); break;
			case ETile::LowCeiling: AddMarker(TEXT("low_ceiling_") + Suffix, {}, TileCenterLocal(P), 250.f, TEXT("Crouch-only passage.")); break;
			case ETile::Pillar:     AddMarker(TEXT("pillar_") + Suffix, {}, TileCenterLocal(P), 200.f, TEXT("Pillar in the open.")); break;
			default: break;
			}
		}
	}

	// Where bots may start: standing room on floor or platforms, no bugs.
	ReachableTiles.Reset();
	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			const FTile& T = At(FIntPoint(X, Y));
			if (T.bReachable && T.Bug == EBug::None && (T.Type == ETile::Floor || T.Type == ETile::Platform))
			{
				ReachableTiles.Add(FIntPoint(X, Y));
			}
		}
	}
}

void APTGymLayout::AddMarker(const FString& Id, TArray<FString> Accepted, const FVector& Local, float Radius, const FString& Description)
{
	FMarker& Marker = Markers.AddDefaulted_GetRef();
	Marker.Id = Id;
	Marker.AcceptedIssues = MoveTemp(Accepted);
	Marker.Location = Local; // converted to world when written
	Marker.Radius = Radius;
	Marker.Description = Description;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
void APTGymLayout::ClearGeometry()
{
	if (Blocks)
	{
		Blocks->ClearInstances();
	}
	for (UStaticMeshComponent* Mesh : LooseMeshes)
	{
		if (Mesh)
		{
			Mesh->DestroyComponent();
		}
	}
	LooseMeshes.Reset();
	for (AActor* Actor : SpawnedActors)
	{
		if (IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
	SpawnedActors.Reset();
}

void APTGymLayout::AddBlock(const FVector& CenterLocal, const FVector& Size, const FRotator& Rotation)
{
	Blocks->AddInstance(FTransform(Rotation, ToWorld(CenterLocal), Size / 100.f), /*bWorldSpace*/ true);
}

void APTGymLayout::AddMeshWithoutCollision(const FVector& CenterLocal, const FVector& Size, const FString& Name)
{
	// A separate component (not an instance): the clip detector checks each no-collision mesh by its own transform.
	UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this, *Name);
	Mesh->SetStaticMesh(CubeMesh);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->SetupAttachment(RootComponent);
	Mesh->RegisterComponent();
	Mesh->SetWorldTransform(FTransform(FRotator::ZeroRotator, ToWorld(CenterLocal), Size / 100.f));
	LooseMeshes.Add(Mesh);
}

void APTGymLayout::BuildGeometry()
{
	const float T = TileSize;
	const float FT = PTGym::FloorThickness;

	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			const FIntPoint P(X, Y);
			const FTile& Tile = At(P);
			const FVector C = TileCenterLocal(P);

			// Floor (top at Z = 0)
			if (Tile.Bug == EBug::FloorHole)
			{
				AddMeshWithoutCollision(C - FVector(0, 0, FT / 2), FVector(T, T, FT), FString::Printf(TEXT("BUG_FloorHole_%d_%d"), X, Y));
			}
			else if (Tile.Bug == EBug::Shaft)
			{
				const float G = PTGym::ShaftHalfGap;
				const float D = PTGym::ShaftDepth;
				const float Side = T * 0.5f - G; // floor strip width around the hole
				AddBlock(C + FVector(-(G + Side / 2), 0, -FT / 2), FVector(Side, T, FT));
				AddBlock(C + FVector(G + Side / 2, 0, -FT / 2), FVector(Side, T, FT));
				AddBlock(C + FVector(0, -(G + Side / 2), -FT / 2), FVector(2 * G, Side, FT));
				AddBlock(C + FVector(0, G + Side / 2, -FT / 2), FVector(2 * G, Side, FT));
				const float WallZ = -FT - D / 2;
				AddBlock(C + FVector(-G - 10, 0, WallZ), FVector(20, 2 * G + 40, D));
				AddBlock(C + FVector(G + 10, 0, WallZ), FVector(20, 2 * G + 40, D));
				AddBlock(C + FVector(0, -G - 10, WallZ), FVector(2 * G, 20, D));
				AddBlock(C + FVector(0, G + 10, WallZ), FVector(2 * G, 20, D));
				AddBlock(C + FVector(0, 0, -FT - D - 10), FVector(2 * G + 40, 2 * G + 40, 20));
			}
			else
			{
				AddBlock(C - FVector(0, 0, FT / 2), FVector(T, T, FT));
			}

			switch (Tile.Type)
			{
			case ETile::Wall:
				if (Tile.Bug == EBug::ClipWall)
				{
					AddMeshWithoutCollision(C + FVector(0, 0, WallHeight / 2), FVector(T, T, WallHeight), FString::Printf(TEXT("BUG_ClipWall_%d_%d"), X, Y));
				}
				else
				{
					AddBlock(C + FVector(0, 0, WallHeight / 2), FVector(T, T, WallHeight));
				}
				break;

			case ETile::Ledge:
				AddBlock(C + FVector(0, 0, Tile.Height / 2), FVector(T * 0.75f, T * 0.75f, Tile.Height));
				if (Tile.Bug == EBug::ForbiddenLedge && GetWorld())
				{
					FActorSpawnParameters Params;
					Params.Owner = this;
					Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					if (ATriggerBox* Trigger = GetWorld()->SpawnActor<ATriggerBox>(ToWorld(C + FVector(0, 0, Tile.Height + 60)), FRotator::ZeroRotator, Params))
					{
						if (UBoxComponent* Box = Cast<UBoxComponent>(Trigger->GetCollisionComponent()))
						{
							Box->SetBoxExtent(FVector(T * 0.375f, T * 0.375f, 60.f));
						}
						Trigger->Tags.Add(TEXT("PTForbidden"));
						SpawnedActors.Add(Trigger);
					}
				}
				break;

			case ETile::Pillar:
				AddBlock(C + FVector(0, 0, WallHeight / 2), FVector(100, 100, WallHeight));
				break;

			case ETile::LowCeiling:
				AddBlock(C + FVector(0, 0, CeilingClearance + 15), FVector(T, T, 30));
				break;

			case ETile::Platform:
				AddBlock(C + FVector(0, 0, PlatformHeight / 2), FVector(T, T, PlatformHeight));
				break;

			case ETile::Ramp:
			{
				const float Length = FMath::Sqrt(T * T + PlatformHeight * PlatformHeight);
				const float Pitch = FMath::RadiansToDegrees(FMath::Atan2(PlatformHeight, T));
				const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(Tile.Up.Y), static_cast<float>(Tile.Up.X)));
				AddBlock(C + FVector(0, 0, PlatformHeight / 2), FVector(Length, T, 20), FRotator(Pitch, Yaw, 0));
				break;
			}

			case ETile::Stairs:
			{
				const float Depth = T / PTGym::StairSteps;
				for (int32 Step = 0; Step < PTGym::StairSteps; ++Step)
				{
					const float Height = PlatformHeight * (Step + 1) / PTGym::StairSteps;
					const float Along = -T / 2 + Depth * (Step + 0.5f);
					const FVector Offset(Tile.Up.X * Along, Tile.Up.Y * Along, Height / 2);
					const FVector Size = Tile.Up.X != 0 ? FVector(Depth, T, Height) : FVector(T, Depth, Height);
					AddBlock(C + Offset, Size);
				}
				break;
			}

			default:
				break;
			}
		}
	}

	// Perimeter walls, one segment per edge tile (some missing = edge-gap bugs), plus corner posts.
	const float H = HalfSize();
	const float PT = PTGym::PerimeterThickness;
	for (int32 I = 0; I < GridSize; ++I)
	{
		const float Along = (I + 0.5f) * T - H;
		if (!MissingPerimeter.Contains(FIntVector(0, I, 0)))             AddBlock(FVector(-H - PT / 2, Along, WallHeight / 2), FVector(PT, T, WallHeight));
		if (!MissingPerimeter.Contains(FIntVector(GridSize - 1, I, 1)))  AddBlock(FVector(H + PT / 2, Along, WallHeight / 2), FVector(PT, T, WallHeight));
		if (!MissingPerimeter.Contains(FIntVector(I, 0, 2)))             AddBlock(FVector(Along, -H - PT / 2, WallHeight / 2), FVector(T, PT, WallHeight));
		if (!MissingPerimeter.Contains(FIntVector(I, GridSize - 1, 3)))  AddBlock(FVector(Along, H + PT / 2, WallHeight / 2), FVector(T, PT, WallHeight));
	}
	for (int32 Corner = 0; Corner < 4; ++Corner)
	{
		const float SX = (Corner & 1) ? 1.f : -1.f;
		const float SY = (Corner & 2) ? 1.f : -1.f;
		AddBlock(FVector(SX * (H + PT / 2), SY * (H + PT / 2), WallHeight / 2), FVector(PT, PT, WallHeight));
	}
}

// ---------------------------------------------------------------------------
// Gym interface
// ---------------------------------------------------------------------------
FRotator APTGymLayout::GenerateRandomRotationInGym() const
{
	const float Yaw = RandomStream.IsValid() ? RandomStream->FRandRange(0.f, 360.f) : FMath::FRandRange(0.f, 360.f);
	return FRotator(0.f, Yaw, 0.f);
}

FVector APTGymLayout::GenerateRandomLocationInGym() const
{
	if (ReachableTiles.IsEmpty())
	{
		return GetActorLocation() + FVector(0, 0, 100);
	}
	auto Rand01 = [this]() { return RandomStream.IsValid() ? RandomStream->FRand() : FMath::FRand(); };
	const int32 Index = FMath::Min(static_cast<int32>(Rand01() * ReachableTiles.Num()), ReachableTiles.Num() - 1);
	const FIntPoint P = ReachableTiles[Index];
	const float Top = At(P).Type == ETile::Platform ? PlatformHeight : 0.f;
	const FVector Jitter((Rand01() - 0.5f) * TileSize * 0.6f, (Rand01() - 0.5f) * TileSize * 0.6f, Top + 100.f);
	return ProjectPointToGym(ToWorld(TileCenterLocal(P) + Jitter));
}

FVector APTGymLayout::ProjectPointToGym(const FVector& InPoint) const
{
	if (const UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
	{
		FNavLocation Projected;
		if (NavSys->ProjectPointToNavigation(InPoint, Projected, FVector(TileSize * 0.5f, TileSize * 0.5f, 300.f)))
		{
			return Projected.Location;
		}
	}
	return InPoint;
}

void APTGymLayout::GetGymExtents(FVector& OutMinExtents, FVector& OutMaxExtents) const
{
	const float H = HalfSize() + PTGym::PerimeterThickness;
	OutMinExtents = GetActorLocation() + FVector(-H, -H, -PTGym::ShaftDepth - 100.f);
	OutMaxExtents = GetActorLocation() + FVector(H, H, WallHeight + 50.f);
}

// ---------------------------------------------------------------------------
// Ground truth
// ---------------------------------------------------------------------------
void APTGymLayout::AppendGroundTruth(TArray<TSharedPtr<FJsonValue>>& OutBugs, TArray<TSharedPtr<FJsonValue>>& OutControls) const
{
	for (const FMarker& Marker : Markers)
	{
		const FVector World = ToWorld(Marker.Location);
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("id"), GetName() + TEXT("_") + Marker.Id);
		Entry->SetArrayField(TEXT("location"), {
			MakeShared<FJsonValueNumber>(FMath::RoundToInt(World.X)),
			MakeShared<FJsonValueNumber>(FMath::RoundToInt(World.Y)),
			MakeShared<FJsonValueNumber>(FMath::RoundToInt(World.Z)) });
		Entry->SetNumberField(TEXT("radius"), Marker.Radius);
		Entry->SetStringField(TEXT("description"), Marker.Description);
		Entry->SetNumberField(TEXT("layoutSeed"), CurrentSeed);

		if (Marker.AcceptedIssues.IsEmpty())
		{
			OutControls.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}
		Entry->SetStringField(TEXT("expectedIssue"), Marker.AcceptedIssues[0]);
		TArray<TSharedPtr<FJsonValue>> Accepted;
		for (const FString& Issue : Marker.AcceptedIssues)
		{
			Accepted.Add(MakeShared<FJsonValueString>(Issue));
		}
		Entry->SetArrayField(TEXT("acceptedIssues"), Accepted);
		OutBugs.Add(MakeShared<FJsonValueObject>(Entry));
	}
}

bool APTGymLayout::WriteWorldGroundTruth(UWorld* World, const FString& Path)
{
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Bugs, Controls;
	int32 GymCount = 0;
	for (TActorIterator<APTGymLayout> It(World); It; ++It)
	{
		It->AppendGroundTruth(Bugs, Controls);
		++GymCount;
	}
	if (GymCount == 0)
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("map"), UWorld::RemovePIEPrefix(World->GetMapName()));
	Root->SetBoolField(TEXT("generated"), true);
	Root->SetNumberField(TEXT("gyms"), GymCount);
	Root->SetArrayField(TEXT("bugs"), Bugs);
	Root->SetArrayField(TEXT("controls"), Controls);

	FString Output;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Output));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	const bool bSaved = FFileHelper::SaveStringToFile(Output, *Path);
	UE_LOG(LogPlaytestGym, Display, TEXT("Ground truth for %d gym(s) (%d bugs, %d controls) written to %s"),
		GymCount, Bugs.Num(), Controls.Num(), *FPaths::ConvertRelativePathToFull(Path));
	return bSaved;
}
