// Copyright Odyssey Interactive. All Rights Reserved.

using UnrealBuildTool;

public class BlueprintBridgeEditor : ModuleRules
{
	public BlueprintBridgeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"AssetTools",
				"BlueprintGraph",
				"Core",
				"CoreUObject",
				"DeveloperSettings",
				"Engine",
				"Json",
				"JsonUtilities",
				"Projects",
				"SourceControl",
				"UnrealEd",
				"KismetCompiler",
				"UMG",
				"UMGEditor"
			}
		);
	}
}
