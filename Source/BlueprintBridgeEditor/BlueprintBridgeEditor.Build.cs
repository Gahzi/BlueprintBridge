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
				"BlueprintGraph",
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
				"Projects",
				"SourceControl",
				"UnrealEd",
				"KismetCompiler"
			}
		);
	}
}
