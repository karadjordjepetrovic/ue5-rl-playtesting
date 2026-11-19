// PTCollision.h
// Location: Plugins/PlaytestBots/Source/PlaytestBots/Public/PTCollision.h

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

// Object channel created in Project Settings -> Collision.
// Must match the slot in Config/DefaultEngine.ini (see below).
#define ECC_PlaytestBot ECC_GameTraceChannel1

// Collision preset (profile) applied to every bot's capsule.
static const FName PTCollisionProfile_Bot(TEXT("PlaytestBot"));