// PTGymLayout.h
// A procedurally generated training gym: a seeded grid of 4 m tiles (walls, ledges, platforms with ramps or
// stairs, pillars, low ceilings) inside a perimeter wall, with a few planted bugs whose locations are recorded
// as ground truth. Built at runtime from cubes, so it works in headless -game runs. Regenerates on gym Reset.
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/Gym/PTGymLayout.h

#pragma once

#include "CoreMinimal.h"
#include "LearningAgentsGym.h"
#include "PTGymLayout.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
class FJsonValue;

UCLASS(Blueprintable)
class PLAYTESTBOTS_API APTGymLayout : public ALearningAgentsGymBase
{
	GENERATED_BODY()

public:
	APTGymLayout();

	// --- Layout ---------------------------------------------------------------------------------------------------
	/** Tiles per side (the gym is GridSize x GridSize tiles). */
	UPROPERTY(EditAnywhere, Category = "Gym|Layout", meta = (ClampMin = "4", ClampMax = "32"))
	int32 GridSize = 8;

	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	float TileSize = 400.f;

	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	float WallHeight = 300.f;

	/** Raised 2 x 2 platforms, reached by one ramp or staircase each. */
	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	FIntPoint PlatformCountRange = FIntPoint(1, 3);

	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	float PlatformHeight = 160.f;

	/** Interior wall runs (each 2-4 tiles long). */
	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	FIntPoint WallRunCountRange = FIntPoint(3, 6);

	/** Jumpable blocks: taller than a step, lower than the character's jump. */
	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	float LedgeChance = 0.08f;

	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	FVector2D LedgeHeightRange = FVector2D(55.f, 80.f);

	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	float PillarChance = 0.05f;

	/** Slabs a standing character can't pass under, but a crouching one can. */
	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	float LowCeilingChance = 0.03f;

	UPROPERTY(EditAnywhere, Category = "Gym|Layout")
	float CeilingClearance = 120.f;

	// --- Planted bugs ---------------------------------------------------------------------------------------------
	/** How many bugs each layout gets (floor hole, clip wall, stuck shaft, forbidden ledge, missing perimeter wall). */
	UPROPERTY(EditAnywhere, Category = "Gym|Bugs")
	FIntPoint BugCountRange = FIntPoint(2, 4);

	// --- Seeds ----------------------------------------------------------------------------------------------------
	/** Fixed layout seed. -1 = derived from RandomSeed and the gym's position (or -PTGymSeed=N on the command line). */
	UPROPERTY(EditAnywhere, Category = "Gym|Seed")
	int32 LayoutSeed = -1;

	/** Build a new layout every time the gym is Reset. */
	UPROPERTY(EditAnywhere, Category = "Gym|Seed")
	bool bRegenerateOnReset = true;

	/** Builds a new layout from Seed (connectivity-checked; retries with derived seeds if needed). */
	UFUNCTION(BlueprintCallable, Category = "Gym")
	void Generate(int32 Seed);

	UFUNCTION(BlueprintPure, Category = "Gym")
	int32 GetCurrentSeed() const { return CurrentSeed; }

	/** Planted bugs and control areas of the current layout, in the scorer's ground_truth.json format. */
	void AppendGroundTruth(TArray<TSharedPtr<FJsonValue>>& OutBugs, TArray<TSharedPtr<FJsonValue>>& OutControls) const;

	/** Writes the ground truth of every gym in the world to Path. Returns false if there are no gyms. */
	static bool WriteWorldGroundTruth(UWorld* World, const FString& Path);

	// --- ALearningAgentsGymBase ------------------------------------------------------------------------------------
	virtual FRotator GenerateRandomRotationInGym() const override;
	virtual FVector GenerateRandomLocationInGym() const override;
	virtual FVector ProjectPointToGym(const FVector& InPoint) const override;
	virtual void GetGymExtents(FVector& OutMinExtents, FVector& OutMaxExtents) const override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	enum class ETile : uint8 { Floor, Wall, Ledge, Pillar, LowCeiling, Platform, Ramp, Stairs };
	enum class EBug : uint8 { None, FloorHole, ClipWall, Shaft, ForbiddenLedge, MissingPerimeter };

	struct FTile
	{
		ETile Type = ETile::Floor;
		EBug Bug = EBug::None;
		FIntPoint Up = FIntPoint::ZeroValue; // ramp/stairs: direction towards the platform
		float Height = 0.f;                   // ledge height
		bool bReserved = false;               // keep as floor (spawn area, ramp entries)
		bool bReachable = false;
	};

	struct FMarker
	{
		FString Id;
		TArray<FString> AcceptedIssues; // empty = control area
		FVector Location = FVector::ZeroVector;
		float Radius = 0.f;
		FString Description;
	};

	UFUNCTION()
	void HandleGymReset();

	bool PlanLayout(FRandomStream& Rng);
	void PlaceBugs(FRandomStream& Rng);
	void BuildGeometry();
	void ClearGeometry();
	bool ComputeReachability(); // fills bReachable; true if almost every open tile is reachable from the spawn area

	bool InBounds(const FIntPoint& P) const { return P.X >= 0 && P.Y >= 0 && P.X < GridSize && P.Y < GridSize; }
	FTile& At(const FIntPoint& P) { return Tiles[P.Y * GridSize + P.X]; }
	const FTile& At(const FIntPoint& P) const { return Tiles[P.Y * GridSize + P.X]; }
	bool IsCenterTile(const FIntPoint& P) const;
	float HalfSize() const { return GridSize * TileSize * 0.5f; }
	FVector TileCenterLocal(const FIntPoint& P) const;
	FVector ToWorld(const FVector& Local) const { return GetActorLocation() + Local; }

	void AddBlock(const FVector& CenterLocal, const FVector& Size, const FRotator& Rotation = FRotator::ZeroRotator);
	void AddMeshWithoutCollision(const FVector& CenterLocal, const FVector& Size, const FString& Name);
	void AddMarker(const FString& Id, TArray<FString> Accepted, const FVector& Local, float Radius, const FString& Description);

	UPROPERTY(VisibleAnywhere, Category = "Gym")
	TObjectPtr<UInstancedStaticMeshComponent> Blocks;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> LooseMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> SpawnedActors;

	TArray<FTile> Tiles;
	TArray<FIntPoint> ReachableTiles;   // tiles a bot can stand on (for random starts)
	TArray<FIntVector> MissingPerimeter; // X, Y, side (0 = -X, 1 = +X, 2 = -Y, 3 = +Y)
	TArray<FMarker> Markers;
	int32 CurrentSeed = 0;
	int32 GenerationCount = 0;
};
