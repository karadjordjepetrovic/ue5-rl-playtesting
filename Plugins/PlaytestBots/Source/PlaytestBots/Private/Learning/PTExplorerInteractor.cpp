// PTExplorerInteractor.cpp
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Private/Learning/PTExplorerInteractor.cpp

#include "Learning/PTExplorerInteractor.h"

#include "CollisionQueryParams.h"
#include "Coverage/PTCoverageSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "LearningAgentsActions.h"
#include "LearningAgentsObservations.h"
#include "PTPlaytestSubsystem.h"

namespace PTExplorer
{
	// Manual normalisation with scale 1 / offset 0: every value we pass is already in [-1, 1].
	static FLearningAgentsObservationNormalizationSettings UnitNormalization(int32 Size)
	{
		FLearningAgentsObservationNormalizationSettings Settings;
		Settings.Scales.Init(1.f, Size);
		Settings.Offsets.Init(0.f, Size);
		return Settings;
	}

	static constexpr float MaxSpeed = 500.f; // cm/s, used to scale velocity into roughly [-1, 1]
}

// ---------------------------------------------------------------------------
// Observations
// ---------------------------------------------------------------------------
void UPTExplorerInteractor::SpecifyAgentObservation_Implementation(
	FLearningAgentsObservationSchemaElement& OutObservationSchemaElement,
	ULearningAgentsObservationSchema* InObservationSchema)
{
	TMap<FName, FLearningAgentsObservationSchemaElement> Elements;

	// Distances to geometry around the bot (horizontal) and to the floor ahead (angled down).
	Elements.Add(TEXT("Rays"), ULearningAgentsObservations::SpecifyContinuousObservation(
		InObservationSchema, RayValueCount(), PTExplorer::UnitNormalization(RayValueCount())));

	// Novelty of the cells around the bot, in the bot's own frame (1 = never visited, -1 = not walkable).
	Elements.Add(TEXT("Coverage"), ULearningAgentsObservations::SpecifyContinuousObservation(
		InObservationSchema, PatchValueCount(), PTExplorer::UnitNormalization(PatchValueCount())));

	// Where the nearest least-visited reachable cell is.
	Elements.Add(TEXT("FrontierDirection"), ULearningAgentsObservations::SpecifyDirectionObservation(InObservationSchema));
	Elements.Add(TEXT("FrontierDistance"), ULearningAgentsObservations::SpecifyFloatObservation(InObservationSchema));

	// Nearest other bot (direction in the bot's frame, distance 0..1; 1 = nobody within range), so bots can spread out.
	Elements.Add(TEXT("NearestBotDirection"), ULearningAgentsObservations::SpecifyDirectionObservation(InObservationSchema));
	Elements.Add(TEXT("NearestBotDistance"), ULearningAgentsObservations::SpecifyFloatObservation(InObservationSchema));

	// Per direction (forward, right, back, left): [ledge low enough to jump onto, gap narrow enough to jump across], 0 or 1.
	// Lets the policy learn to jump only when there is something to jump on (jumping costs a little reward).
	Elements.Add(TEXT("Jumpable"), ULearningAgentsObservations::SpecifyContinuousObservation(
		InObservationSchema, JumpProbeCount * 2, PTExplorer::UnitNormalization(JumpProbeCount * 2)));

	// Own movement state.
	Elements.Add(TEXT("Velocity"), ULearningAgentsObservations::SpecifyVelocityObservation(InObservationSchema));
	Elements.Add(TEXT("Grounded"), ULearningAgentsObservations::SpecifyBoolObservation(InObservationSchema));
	Elements.Add(TEXT("Crouched"), ULearningAgentsObservations::SpecifyBoolObservation(InObservationSchema));

	OutObservationSchemaElement = ULearningAgentsObservations::SpecifyStructObservation(InObservationSchema, Elements);
}

void UPTExplorerInteractor::GatherAgentObservation_Implementation(
	FLearningAgentsObservationObjectElement& OutObservationObjectElement,
	ULearningAgentsObservationObject* InObservationObject,
	const int32 AgentId)
{
	TArray<float> Rays;
	Rays.Init(1.f, RayValueCount());
	TArray<float> Patch;
	Patch.Init(-1.f, PatchValueCount());
	FVector FrontierDirection = FVector::ZeroVector;
	float FrontierDistance = 1.f;
	TArray<float> Jumpable;
	Jumpable.Init(0.f, JumpProbeCount * 2);
	FVector NearestBotDirection = FVector::ZeroVector;
	float NearestBotDistance = 1.f;
	FVector Velocity = FVector::ZeroVector;
	FTransform Frame = FTransform::Identity;
	bool bGrounded = false;
	bool bCrouched = false;

	const ACharacter* BotCharacter = Cast<ACharacter>(GetAgent(AgentId));
	UWorld* World = BotCharacter ? BotCharacter->GetWorld() : nullptr;
	if (BotCharacter && World)
	{
		const FVector Origin = BotCharacter->GetActorLocation();
		const FVector Feet = BotCharacter->GetNavAgentLocation();
		const FRotator YawOnly(0.f, BotCharacter->GetActorRotation().Yaw, 0.f);
		Frame = FTransform(YawOnly, Origin);

		Velocity = BotCharacter->GetVelocity() / PTExplorer::MaxSpeed;
		bCrouched = BotCharacter->bIsCrouched;
		if (const UCharacterMovementComponent* Movement = BotCharacter->GetCharacterMovement())
		{
			bGrounded = Movement->IsMovingOnGround();
		}

		// --- Rays (static world geometry only, so other bots don't block them) ---
		FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(PTExplorerRays), /*bTraceComplex*/ false, BotCharacter);
		const FCollisionObjectQueryParams StaticObjects(ECC_WorldStatic);
		for (int32 RayIndex = 0; RayIndex < RayYawCount; ++RayIndex)
		{
			const float LocalYaw = 360.f * RayIndex / RayYawCount;
			FHitResult Hit;

			const FVector Flat = YawOnly.RotateVector(FRotator(0.f, LocalYaw, 0.f).Vector());
			if (World->LineTraceSingleByObjectType(Hit, Origin, Origin + Flat * HorizontalRayLength, StaticObjects, TraceParams))
			{
				Rays[RayIndex * 2] = Hit.Distance / HorizontalRayLength;
			}

			const FVector Down = YawOnly.RotateVector(FRotator(DownRayPitch, LocalYaw, 0.f).Vector());
			if (World->LineTraceSingleByObjectType(Hit, Origin, Origin + Down * DownRayLength, StaticObjects, TraceParams))
			{
				Rays[RayIndex * 2 + 1] = Hit.Distance / DownRayLength;
			}
		}

		// --- Jump probes: a step/ledge we could land on, or a gap we could clear ---
		for (int32 Probe = 0; Probe < JumpProbeCount; ++Probe)
		{
			const FVector Dir = YawOnly.RotateVector(FRotator(0.f, 360.f * Probe / JumpProbeCount, 0.f).Vector());
			bool bLedge = false, bGap = false;
			ProbeJump(BotCharacter, Dir, JumpProbeDistance, bLedge, bGap);
			Jumpable[Probe * 2] = bLedge ? 1.f : 0.f;
			Jumpable[Probe * 2 + 1] = bGap ? 1.f : 0.f;
		}

		// --- Nearest other bot ---
		if (const UPTPlaytestSubsystem* Run = World->GetSubsystem<UPTPlaytestSubsystem>())
		{
			float BestDistance = NeighbourRange;
			for (const TWeakObjectPtr<APawn>& Other : Run->GetBots())
			{
				if (!Other.IsValid() || Other.Get() == BotCharacter)
				{
					continue;
				}
				const FVector ToOther = Other->GetActorLocation() - Origin;
				const float Distance = ToOther.Size2D();
				if (Distance < BestDistance && FMath::Abs(ToOther.Z) < 300.f)
				{
					BestDistance = Distance;
					NearestBotDirection = ToOther.GetSafeNormal2D();
					NearestBotDistance = Distance / NeighbourRange;
				}
			}
		}

		const UPTCoverageSubsystem* Coverage = World->GetSubsystem<UPTCoverageSubsystem>();
		if (Coverage)
		{
			const float Cell = Coverage->CellSize;

			// --- Coverage patch in the bot's frame (row = forward, column = right) ---
			int32 PatchIndex = 0;
			for (int32 Forward = -PatchRadius; Forward <= PatchRadius; ++Forward)
			{
				for (int32 Right = -PatchRadius; Right <= PatchRadius; ++Right)
				{
					const FVector Sample = Feet + YawOnly.RotateVector(FVector(Forward * Cell, Right * Cell, 0.f));
					const FIntVector CellId = Coverage->LocationToCell(Sample);
					const int32 Visits = Coverage->GetVisitCount(CellId);
					if (Visits > 0 || Coverage->IsReachable(CellId))
					{
						Patch[PatchIndex] = 1.f / FMath::Sqrt(1.f + Visits);
					}
					++PatchIndex;
				}
			}

			// --- Least-visited reachable cell nearby, avoiding cells other bots are already heading for ---
			// Score = visits (+ ClaimPenalty if another bot claimed a cell within ClaimRadius) + a small distance term.
			// Without the claim penalty, bots next to each other get the same target and walk in a row.
			const FIntVector Center = Coverage->LocationToCell(Feet);
			TArray<FIntVector> OtherClaims;
			for (const TPair<int32, FIntVector>& Claim : FrontierClaims)
			{
				if (Claim.Key != AgentId)
				{
					OtherClaims.Add(Claim.Value);
				}
			}
			const int32 ClaimRadiusSq = ClaimRadius * ClaimRadius;
			const float MaxDistanceSq = 2.f * FrontierRadius * FrontierRadius;

			float BestScore = TNumericLimits<float>::Max();
			FIntVector BestCell = FIntVector::ZeroValue;
			bool bFound = false;
			for (int32 Dz = -1; Dz <= 1; ++Dz)
			{
				for (int32 Dx = -FrontierRadius; Dx <= FrontierRadius; ++Dx)
				{
					for (int32 Dy = -FrontierRadius; Dy <= FrontierRadius; ++Dy)
					{
						if (Dx == 0 && Dy == 0)
						{
							continue;
						}
						const FIntVector CellId = Center + FIntVector(Dx, Dy, Dz);
						if (!Coverage->IsReachable(CellId))
						{
							continue;
						}
						float Score = static_cast<float>(Coverage->GetVisitCount(CellId))
							+ 0.5f * static_cast<float>(Dx * Dx + Dy * Dy) / MaxDistanceSq; // < 1 visit: only breaks ties
						for (const FIntVector& Claimed : OtherClaims)
						{
							const FIntVector D = Claimed - CellId;
							if (D.X * D.X + D.Y * D.Y <= ClaimRadiusSq && FMath::Abs(D.Z) <= 1)
							{
								Score += ClaimPenalty;
								break;
							}
						}
						if (Score < BestScore)
						{
							BestScore = Score;
							BestCell = CellId;
							bFound = true;
						}
					}
				}
			}
			const FVector BestLocation = bFound ? Coverage->CellToLocation(BestCell) : FVector::ZeroVector;
			if (bFound)
			{
				FrontierClaims.FindOrAdd(AgentId) = BestCell;
			}
			else
			{
				FrontierClaims.Remove(AgentId);
			}
			if (bFound)
			{
				const FVector ToBest = BestLocation - Feet;
				FrontierDirection = ToBest.GetSafeNormal2D();
				FrontierDistance = FMath::Clamp(ToBest.Size2D() / (FrontierRadius * Cell), 0.f, 1.f);
			}
		}
	}

	TMap<FName, FLearningAgentsObservationObjectElement> Elements;
	Elements.Add(TEXT("Rays"), ULearningAgentsObservations::MakeContinuousObservation(InObservationObject, Rays));
	Elements.Add(TEXT("Coverage"), ULearningAgentsObservations::MakeContinuousObservation(InObservationObject, Patch));
	Elements.Add(TEXT("FrontierDirection"), ULearningAgentsObservations::MakeDirectionObservation(InObservationObject, FrontierDirection, Frame));
	Elements.Add(TEXT("FrontierDistance"), ULearningAgentsObservations::MakeFloatObservation(InObservationObject, FrontierDistance));
	Elements.Add(TEXT("NearestBotDirection"), ULearningAgentsObservations::MakeDirectionObservation(InObservationObject, NearestBotDirection, Frame));
	Elements.Add(TEXT("NearestBotDistance"), ULearningAgentsObservations::MakeFloatObservation(InObservationObject, NearestBotDistance));
	Elements.Add(TEXT("Jumpable"), ULearningAgentsObservations::MakeContinuousObservation(InObservationObject, Jumpable));
	Elements.Add(TEXT("Velocity"), ULearningAgentsObservations::MakeVelocityObservation(InObservationObject, Velocity, Frame));
	Elements.Add(TEXT("Grounded"), ULearningAgentsObservations::MakeBoolObservation(InObservationObject, bGrounded));
	Elements.Add(TEXT("Crouched"), ULearningAgentsObservations::MakeBoolObservation(InObservationObject, bCrouched));

	OutObservationObjectElement = ULearningAgentsObservations::MakeStructObservation(InObservationObject, Elements);
}

void UPTExplorerInteractor::ProbeJump(const ACharacter* Bot, const FVector& Dir, float Distance, bool& bOutLedge, bool& bOutGap)
{
	bOutLedge = false;
	bOutGap = false;
	UWorld* World = Bot ? Bot->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	const UCharacterMovementComponent* Move = Bot->GetCharacterMovement();
	const float Gravity = Move ? FMath::Max(1.f, FMath::Abs(Move->GetGravityZ())) : 980.f;
	const float JumpZ = Move ? Move->JumpZVelocity : 420.f;
	const float MaxJumpHeight = JumpZ * JumpZ / (2.f * Gravity); // ~90 cm with default settings
	const float MaxStep = Move ? Move->MaxStepHeight : 45.f;     // anything lower is walked over
	const FVector FeetPoint = Bot->GetActorLocation() - FVector(0.f, 0.f, Bot->GetSimpleCollisionHalfHeight());

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(PTJumpProbe), /*bTraceComplex*/ false, Bot);
	const FCollisionObjectQueryParams StaticObjects(ECC_WorldStatic);
	FHitResult Hit;

	// Ledge: blocked just above step height, but clear just above jump height.
	const FVector Low = FeetPoint + FVector(0.f, 0.f, MaxStep + 10.f);
	const FVector High = FeetPoint + FVector(0.f, 0.f, MaxJumpHeight - 5.f);
	if (World->LineTraceSingleByObjectType(Hit, Low, Low + Dir * Distance, StaticObjects, TraceParams))
	{
		FHitResult HighHit;
		bOutLedge = !World->LineTraceSingleByObjectType(HighHit, High, High + Dir * (Hit.Distance + 60.f), StaticObjects, TraceParams);
	}

	// Gap: no floor just ahead, but floor again within jumping distance.
	const FVector Near = FeetPoint + Dir * 120.f + FVector(0.f, 0.f, 20.f);
	const FVector Far = FeetPoint + Dir * 300.f + FVector(0.f, 0.f, 20.f);
	const bool bNearFloor = World->LineTraceSingleByObjectType(Hit, Near, Near - FVector(0.f, 0.f, 150.f), StaticObjects, TraceParams);
	const bool bFarFloor = World->LineTraceSingleByObjectType(Hit, Far, Far - FVector(0.f, 0.f, 150.f), StaticObjects, TraceParams);
	bOutGap = !bNearFloor && bFarFloor;
}

// ---------------------------------------------------------------------------
// Actions: move (2D, bot frame), jump, crouch. Same abilities as the random walker.
// ---------------------------------------------------------------------------
void UPTExplorerInteractor::SpecifyAgentAction_Implementation(
	FLearningAgentsActionSchemaElement& OutActionSchemaElement,
	ULearningAgentsActionSchema* InActionSchema)
{
	TMap<FName, FLearningAgentsActionSchemaElement> Elements;
	Elements.Add(TEXT("Move"), ULearningAgentsActions::SpecifyContinuousAction(InActionSchema, 2));
	Elements.Add(TEXT("Jump"), ULearningAgentsActions::SpecifyBoolAction(InActionSchema, /*PriorProbability*/ 0.1f));
	Elements.Add(TEXT("Crouch"), ULearningAgentsActions::SpecifyBoolAction(InActionSchema, /*PriorProbability*/ 0.05f));

	OutActionSchemaElement = ULearningAgentsActions::SpecifyStructAction(InActionSchema, Elements);
}

void UPTExplorerInteractor::PerformAgentAction_Implementation(
	const ULearningAgentsActionObject* InActionObject,
	const FLearningAgentsActionObjectElement& InActionObjectElement,
	const int32 AgentId)
{
	const ACharacter* BotCharacter = Cast<ACharacter>(GetAgent(AgentId));
	if (!BotCharacter)
	{
		HeldActions.Remove(AgentId);
		return;
	}

	TMap<FName, FLearningAgentsActionObjectElement> Elements;
	if (!ULearningAgentsActions::GetStructAction(Elements, InActionObject, InActionObjectElement))
	{
		return;
	}

	FHeldAction& Held = HeldActions.FindOrAdd(AgentId);

	TArray<float> MoveValues;
	if (const FLearningAgentsActionObjectElement* MoveElement = Elements.Find(TEXT("Move")))
	{
		if (ULearningAgentsActions::GetContinuousAction(MoveValues, InActionObject, *MoveElement) && MoveValues.Num() == 2)
		{
			// Convert to a world direction now; applying it in the bot frame every frame would make it spin,
			// because the character turns to face its movement.
			const FRotator YawOnly(0.f, BotCharacter->GetActorRotation().Yaw, 0.f);
			Held.WorldMove = YawOnly.RotateVector(FVector(MoveValues[0], MoveValues[1], 0.f).GetClampedToMaxSize(1.f));
		}
	}
	if (const FLearningAgentsActionObjectElement* JumpElement = Elements.Find(TEXT("Jump")))
	{
		ULearningAgentsActions::GetBoolAction(Held.bJump, InActionObject, *JumpElement);
	}
	if (const FLearningAgentsActionObjectElement* CrouchElement = Elements.Find(TEXT("Crouch")))
	{
		ULearningAgentsActions::GetBoolAction(Held.bCrouch, InActionObject, *CrouchElement);
	}
}

void UPTExplorerInteractor::ApplyHeldActions()
{
	for (auto It = HeldActions.CreateIterator(); It; ++It)
	{
		ACharacter* BotCharacter = Cast<ACharacter>(GetAgent(It.Key()));
		if (!BotCharacter)
		{
			It.RemoveCurrent();
			continue;
		}
		const FHeldAction& Held = It.Value();

		if (!Held.WorldMove.IsNearlyZero())
		{
			BotCharacter->AddMovementInput(Held.WorldMove, 1.f);
		}

		if (Held.bJump)
		{
			BotCharacter->Jump();
		}
		else
		{
			BotCharacter->StopJumping();
		}

		if (UCharacterMovementComponent* Movement = BotCharacter->GetCharacterMovement())
		{
			Movement->GetNavAgentPropertiesRef().bCanCrouch = true;
		}
		if (Held.bCrouch && !BotCharacter->bIsCrouched)
		{
			BotCharacter->Crouch();
		}
		else if (!Held.bCrouch && BotCharacter->bIsCrouched)
		{
			BotCharacter->UnCrouch();
		}
	}
}
