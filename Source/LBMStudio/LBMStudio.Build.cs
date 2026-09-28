using UnrealBuildTool;
public class LBMStudio : ModuleRules
{
    public LBMStudio(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InputCore", "Slate", "SlateCore", "ProceduralMeshComponent", "RenderCore", "RHI", "Json", "JsonUtilities", "ImageWrapper" });
        PrivateDependencyModuleNames.AddRange(new[] { "ApplicationCore", "GeometryCore", "UMG" });
        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
        if (Target.Platform == UnrealTargetPlatform.Mac)
            PublicFrameworks.AddRange(new[] { "Cocoa", "UniformTypeIdentifiers", "Metal" });
    }
}
