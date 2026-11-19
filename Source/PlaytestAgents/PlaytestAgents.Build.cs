// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class PlaytestAgents : ModuleRules
{
	public PlaytestAgents(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"PlaytestAgents",
			"PlaytestAgents/Variant_Platforming",
			"PlaytestAgents/Variant_Platforming/Animation",
			"PlaytestAgents/Variant_Combat",
			"PlaytestAgents/Variant_Combat/AI",
			"PlaytestAgents/Variant_Combat/Animation",
			"PlaytestAgents/Variant_Combat/Gameplay",
			"PlaytestAgents/Variant_Combat/Interfaces",
			"PlaytestAgents/Variant_Combat/UI",
			"PlaytestAgents/Variant_SideScrolling",
			"PlaytestAgents/Variant_SideScrolling/AI",
			"PlaytestAgents/Variant_SideScrolling/Gameplay",
			"PlaytestAgents/Variant_SideScrolling/Interfaces",
			"PlaytestAgents/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
