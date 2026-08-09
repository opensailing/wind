// Copyright FlowViz contributors. All Rights Reserved.

using UnrealBuildTool;

public class FlowVizApp : ModuleRules
{
	public FlowVizApp(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"FlowVizRuntime"
		});
	}
}
