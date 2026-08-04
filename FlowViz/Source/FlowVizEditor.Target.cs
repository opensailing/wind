// Copyright FlowViz contributors. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class FlowVizEditorTarget : TargetRules
{
	public FlowVizEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("FlowVizApp");
	}
}
