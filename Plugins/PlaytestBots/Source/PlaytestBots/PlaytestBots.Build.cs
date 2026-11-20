// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class PlaytestBots : ModuleRules
{
	public PlaytestBots(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "AIModule",          // AAIController, MoveTo (walkers)
            "NavigationSystem",  // navmesh queries (frontier walker, coverage denominator)
            "LearningAgents",         // interactor, policy, critic, manager (Phase 2)
            "LearningAgentsTraining", // training environment, PPO trainer, communicator (Phase 2)
            "Learning",               // LearningCore runtime (needed by Learning Agents public headers)
            "LearningTraining"        // LearningCore training (LearningAgentsTrainer.h includes LearningTrainer.h)
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Json",              // FJsonObject
            "JsonUtilities"      // FJsonObjectConverter (report.json)
        });
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
