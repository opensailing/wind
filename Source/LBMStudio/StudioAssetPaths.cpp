#include "StudioAssetPaths.h"
#include "StudioProject.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"

namespace
{
    using FMapPath = TFunctionRef<bool(FString&, FString&)>;

    bool MapDraft(FStudioCaseDraft& Draft, FMapPath Map, FString& Error)
    {
        for (auto& Asset : Draft.Geometry)
            if (!Map(Asset.SourcePath, Error))
            { Error = Asset.Name + TEXT(": ") + Error; return false; }
        return true;
    }

    bool MapProject(FStudioProject& Project, FMapPath Map, FString& Error)
    {
        if(!Project.Residual.Path.IsEmpty()&&!Map(Project.Residual.Path,Error))return false;
        for (auto& Recording : Project.Recordings)
        {
            if (!Map(Recording.Path, Error)) return false;
            if (Recording.Reconstruction.IsSet() && !Map(Recording.Reconstruction->Path, Error)) return false;
        }
        for(auto& Comparison:Project.Comparisons)for(auto* Side:{&Comparison.Primary,&Comparison.Secondary})
        {
            if(!Side->Reference.IsSet())continue;
            if(!Map(Side->Reference->Path,Error))return false;
            if(Side->Reference->Reconstruction.IsSet()&&!Map(Side->Reference->Reconstruction->Path,Error))return false;
        }
        if (!MapDraft(Project.Draft, Map, Error)) return false;
        for (auto& Run : Project.Runs)
        {
            if (!Run.GetConfiguration()) continue;
            auto Configuration = *Run.GetConfiguration();
            if (!MapDraft(Configuration, Map, Error)) return false;
            // Reconstruct a value, retaining the run ID and all immutable settings.
            // Only its storage location changes; this does not capture a new run.
            auto Object = Run.ToJSON();
            Object->SetObjectField(TEXT("configuration"), StudioCaseIO::ToJSON(Configuration));
            FStudioRunRecord Resolved;
            if (!FStudioRunRecord::FromJSON(Object, Resolved, Error)) return false;
            Run = MoveTemp(Resolved);
        }
        return true;
    }

    bool ResolvePath(FString& Path, const FString& Directory, FString& Error)
    {
        if (Path.IsEmpty() || Path.Contains(TEXT("://")))
        { Error = TEXT("Assets must reference local files."); return false; }
        if (FPaths::IsRelative(Path))
        {
            if (Directory.IsEmpty() || FPaths::IsRelative(Directory))
            { Error = TEXT("A relative asset path needs its original project folder. Select an absolute source file before saving."); return false; }
            Path = Directory / Path;
        }
        FPaths::NormalizeFilename(Path);
        if (!FPaths::CollapseRelativeDirectories(Path) || Path.Len() > 4096)
        { Error = TEXT("Asset path cannot be resolved."); return false; }
        return true;
    }
}

bool StudioAssetPaths::Resolve(FStudioCaseDraft& Draft, const FString& Directory, FString& Error)
{
    auto Candidate = Draft;
    if (!MapDraft(Candidate, [&Directory](FString& Path, FString& Why)
        { return ResolvePath(Path, Directory, Why); }, Error)) return false;
    Draft = MoveTemp(Candidate); Error.Empty(); return true;
}

bool StudioAssetPaths::Resolve(FStudioProject& Project, const FString& Directory, FString& Error)
{
    auto Candidate = Project;
    if (!MapProject(Candidate, [&Directory](FString& Path, FString& Why)
        { return ResolvePath(Path, Directory, Why); }, Error)) return false;
    Candidate.AssetBaseDirectory = Directory;
    Project = MoveTemp(Candidate); Error.Empty(); return true;
}

bool StudioAssetPaths::ForStorage(const FStudioProject& Project, const FString& Destination,
    FStudioProject& Out, FString& Error)
{
    auto Candidate = Project;
    if (!Resolve(Candidate, Project.AssetBaseDirectory, Error)) return false;
    // Recovery metadata names the original owner, so store absolute locations.
    if (!Candidate.RecoverySource.IsEmpty())
    { Out = MoveTemp(Candidate); return true; }
    FString Directory = FPaths::GetPath(FPaths::ConvertRelativePathToFull(Destination));
    FPaths::NormalizeDirectoryName(Directory);
    const FString Base = Directory + TEXT("/");
    if (!MapProject(Candidate, [&Base](FString& Path, FString&)
    {
        FString Relative = Path;
        // Keep an absolute location if a platform cannot express a relative one.
        if (FPaths::MakePathRelativeTo(Relative, *Base)) Path = MoveTemp(Relative);
        return true;
    }, Error)) return false;
    Candidate.AssetBaseDirectory = Directory;
    Out = MoveTemp(Candidate); Error.Empty(); return true;
}
