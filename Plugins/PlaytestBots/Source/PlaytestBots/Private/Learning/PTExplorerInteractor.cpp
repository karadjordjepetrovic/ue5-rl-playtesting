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

			// --- Least-visited reachable cell nearby (ties broken by distance) ---
			const FIntVector Center = Coverage->LocationToCell(Feet);
			int32 BestVisits = TNumericLimits<int32>::Max();
			float BestDistanceSq = TNumericLimits<float>::Max();
			FVector BestLocation = FVector::ZeroVector;
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
						const int32 Visits = Coverage->GetVisitCount(CellId);
						const float DistanceSq = static_cast<float>(Dx * Dx + Dy * Dy);
						if (Visits < BestVisits || (Visits == BestVisits && DistanceSq < BestDistanceSq))
						{
							BestVisits = Visits;
							BestDistanceSq = DistanceSq;
							BestLocation = Coverage->CellToLocation(CellId);
							bFound = true;
						}
					}
				}
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
	Elements.Add(TEXT("Velocity"), ULearningAgentsObservations::MakeVelocityObservation(InObservationObject, Velocity, Frame));
	Elements.Add(TEXT("Grounded"), ULearningAgentsObservations::MakeBoolObservation(InObservationObject, bGrounded));
	Elements.Add(TEXT("Crouched"), ULearningAgentsObservations::MakeBoolObservation(InObservationObject, bCrouched));

	OutObservationObjectElement = ULearningAgentsObservations::MakeStructObservation(InObservationObject, Elements);
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
