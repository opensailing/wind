// Copyright FlowViz contributors. All Rights Reserved.

using UnrealBuildTool;

public class FlowVizRuntime : ModuleRules
{
	public FlowVizRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",
			"RHI",
			"Renderer",
			"Projects",
			"Niagara"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"UMG",
			"Json",
			"JsonUtilities",
			"InputCore",
			"ApplicationCore"
		});
	}
}
