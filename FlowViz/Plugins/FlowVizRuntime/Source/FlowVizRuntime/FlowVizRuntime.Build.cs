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
			"Niagara",
			"ProceduralMeshComponent"
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

		// The native "Browse..." folder picker. A Developer module, so it
		// exists in Development desktop builds (our packaged config) but not
		// Shipping -- the panel compiles the button out there and the path
		// text box remains.
		if (Target.Configuration != UnrealTargetConfiguration.Shipping)
		{
			PrivateDependencyModuleNames.Add("DesktopPlatform");
			PrivateDefinitions.Add("FLOWVIZ_WITH_FILE_DIALOG=1");
		}
		else
		{
			PrivateDefinitions.Add("FLOWVIZ_WITH_FILE_DIALOG=0");
		}
	}
}
