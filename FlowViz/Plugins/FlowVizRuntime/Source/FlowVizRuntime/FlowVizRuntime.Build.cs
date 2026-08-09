// Copyright FlowViz contributors. All Rights Reserved.

using UnrealBuildTool;

public class FlowVizRuntime : ModuleRules
{
	public FlowVizRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Public: what our PUBLIC HEADERS need. The render surface there is
		// RenderCore/RHI only (shader declarations, upload types); Renderer is
		// consumed solely by Private/Render/'s dispatcher, so it lives below.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",
			"RHI",
			"Projects",
			"ProceduralMeshComponent"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Renderer",
			"Slate",
			"SlateCore",
			"Json"
		});

		/*
		 * The Renderer's INTERNAL include dir, for PostProcess/PostProcessInputs.h:
		 * FPostProcessingInputs is the documented payload of the
		 * PrePostProcessPass_RenderThread seam (engine plugins like
		 * ColorCorrectRegions consume it the same way), but UBT only hands the
		 * Internal paths to engine modules. The struct is stable across 5.x and
		 * the alternative -- redeclaring it -- is an ODR trap.
		 */
		PrivateIncludePaths.Add(
			System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"));

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
