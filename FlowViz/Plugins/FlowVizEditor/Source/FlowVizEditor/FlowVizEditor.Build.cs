// Copyright FlowViz contributors. All Rights Reserved.

using UnrealBuildTool;

public class FlowVizEditor : ModuleRules
{
	public FlowVizEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"FlowVizRuntime"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"UnrealEd",
			"ToolMenus",
			"EditorStyle",
			"Projects"
		});
	}
}
