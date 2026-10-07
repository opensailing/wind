using UnrealBuildTool;
using System.IO;
public class LBMStudio : ModuleRules
{
    public LBMStudio(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InputCore", "Slate", "SlateCore", "ProceduralMeshComponent", "RenderCore", "RHI", "Json", "JsonUtilities", "ImageWrapper" });
        PrivateDependencyModuleNames.AddRange(new[] { "ApplicationCore", "GeometryCore", "UMG" });
        if (Target.bBuildEditor)
            PrivateDependencyModuleNames.Add("SlateRHIRenderer");
        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "zlib");
        string Home4CAD = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../Content/ThirdParty/Home4CAD"));
        if (Directory.Exists(Home4CAD))
            foreach (string FilePath in Directory.GetFiles(Home4CAD, "*", SearchOption.AllDirectories))
                RuntimeDependencies.Add(FilePath, StagedFileType.NonUFS);
        string Home4Templates = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../Content/Samples/HOME4"));
        if (Directory.Exists(Home4Templates))
            foreach (string FilePath in Directory.GetFiles(Home4Templates, "*", SearchOption.TopDirectoryOnly))
                RuntimeDependencies.Add(FilePath, StagedFileType.NonUFS);
        if (Target.Platform == UnrealTargetPlatform.Mac)
            PublicFrameworks.AddRange(new[] { "Cocoa", "UniformTypeIdentifiers", "Metal", "AVFoundation", "CoreMedia", "CoreVideo" });
    }
}
