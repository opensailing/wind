using UnrealBuildTool;
public class LBMStudioEditorTarget : TargetRules
{
    public LBMStudioEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("LBMStudio");
    }
}
