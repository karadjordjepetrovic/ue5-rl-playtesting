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
            "NavigationSystem"   // navmesh queries (frontier walker, coverage denominator)
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
