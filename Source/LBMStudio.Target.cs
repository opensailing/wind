using UnrealBuildTool;
public class LBMStudioTarget : TargetRules
{
    public LBMStudioTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("LBMStudio");
    }
}
