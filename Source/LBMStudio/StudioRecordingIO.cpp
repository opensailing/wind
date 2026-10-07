#include "StudioRecording.h"
#include "StudioModel.h"
#include "StudioAssets.h"
#include "StudioFileDialog.h"
#include "StudioPointRecording.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
bool CleanText(const FString& S,int32 Limit)
{
    if(S.TrimStartAndEnd().IsEmpty()||S.Len()>Limit) return false;
    for(TCHAR C:S) if(C<32||C==127) return false;
    return true;
}
bool HashValid(const FString& S)
{
    if(S.Len()!=64) return false;
    for(TCHAR C:S) if(!FChar::IsHexDigit(C)) return false;
    return true;
}
template<typename T> bool Ready(const TSharedRef<T,ESPMode::ThreadSafe>& Source,int32 Frame,
    const FStudioLoadCancellation& Cancel,FString& Error)
{
    Error=Source->LoadError();
    if(!Error.IsEmpty()) return false;
    if(Frame<0||Frame>=Source->FrameCount())
    {Error=TEXT("The saved source frame is unavailable. Current project kept.");return false;}
    if(!Source->PrepareFrame(Frame,Cancel))
    {Error=Source->LoadError().IsEmpty()?TEXT("Source frame preparation was cancelled or failed."):Source->LoadError();return false;}
    return true;
}
}

bool StudioRecordings::IsValidReference(const FStudioRecordingReference& R)
{
    if(!CleanText(R.Id,256)||!CleanText(R.Title,256)||!CleanText(R.Path,4096)||R.Path.Contains(TEXT("://"))||!HashValid(R.MetadataSHA256))return false;
    if(R.Reconstruction.IsSet()&&(R.Format!=TEXT("point_v3")||!CleanText(R.Reconstruction->Path,4096)||R.Reconstruction->Path.Contains(TEXT("://"))||
        FPaths::GetCleanFilename(R.Reconstruction->Path)!=TEXT("reconstruction.json")||!HashValid(R.Reconstruction->MetadataSHA256)))return false;
    if(R.Format==TEXT("flow_v2"))return FPaths::GetCleanFilename(R.Path)==TEXT("flow.bin")&&HashValid(R.PayloadSHA256);
    return R.Format==TEXT("point_v3")&&FPaths::GetCleanFilename(R.Path)==TEXT("recording.json")&&R.PayloadSHA256.IsEmpty();
}

FString StudioRecordings::DescriptorOrPayloadInFolder(const FString& Folder)
{
    FStudioFileAccess Access(Folder/TEXT("recording.json"));
    return Folder/(IFileManager::Get().FileExists(*(Folder/TEXT("flow.bin")))?TEXT("flow.bin"):TEXT("recording.json"));
}

static FStudioRecordingLoadResult ReadPointRecording(const FString& Path,int32 Frame,const FStudioLoadCancellation& Cancel,
    bool bCheckInstalledIdentity,const FString& ExpectedMetadata=FString(),
    const TOptional<FStudioReconstructionReference>& Reconstruction = {})
{
    FStudioRecordingLoadResult Result;
    auto Opened=StudioPointRecordings::Open(Path,{},Cancel,ExpectedMetadata);
    if(!Opened.Recording){Result.Error=Opened.Error;return Result;}
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Surface;
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> Volume;
    auto BoundReconstruction=Reconstruction;
    if(Opened.Recording->Descriptor().StructuredGrid)
    {
        if(Reconstruction.IsSet()){Result.Error=TEXT("Original structured grids use their supplied nodes and masks; a separate reconstruction attachment is unsupported.");return Result;}
        auto Loaded=StudioVolumes::OriginalSource(Opened.Recording->Descriptor(),Opened.Recording->Geometry(),Cancel);
        if(!Loaded.Volume){Result.Error=Loaded.Error;return Result;}Volume=MoveTemp(Loaded.Volume);
    }
    else if(Reconstruction.IsSet()&&Opened.Recording->Descriptor().SpatialDimensions==3)
    {
        auto Loaded=StudioVolumes::Load(Reconstruction->Path,Opened.Recording->Descriptor(),Opened.Recording->Geometry(),Cancel,Reconstruction->MetadataSHA256);
        if(!Loaded.Volume){Result.Error=Loaded.Error;Result.bReconstructionFailed=true;return Result;}
        BoundReconstruction->MetadataSHA256=Loaded.Volume->MetadataSHA256;Volume=MoveTemp(Loaded.Volume);
    }
    else if(Reconstruction.IsSet())
    {
        auto Loaded=StudioSurfaceReconstructions::Load(Reconstruction->Path,Opened.Recording->Descriptor(),
            Opened.Recording->Geometry(),Cancel,Reconstruction->MetadataSHA256);
        if(!Loaded.Reconstruction){Result.Error=Loaded.Error;Result.bReconstructionFailed=true;return Result;}
        BoundReconstruction->MetadataSHA256=Loaded.Reconstruction->MetadataSHA256;
        Surface=MoveTemp(Loaded.Reconstruction);
    }
    auto Source=MakeShared<FPointRecordedSolver,ESPMode::ThreadSafe>(Opened.Recording.ToSharedRef(),MoveTemp(Surface),MoveTemp(Volume));
    FStudioRecordingReference Ref;
    Ref.Format=TEXT("point_v3");Ref.Id=Source->Descriptor().Id;Ref.Title=Source->Descriptor().Title;
    Ref.Path=FPaths::ConvertRelativePathToFull(Path);Ref.MetadataSHA256=Opened.Recording->Descriptor().MetadataSHA256;
    Ref.Reconstruction=MoveTemp(BoundReconstruction);
    if(!StudioRecordings::IsValidReference(Ref)){Result.Error=TEXT("Invalid point recording reference.");return Result;}
    if(bCheckInstalledIdentity&&!StudioRecordings::PathForId(Ref.Id).IsEmpty())
    {Result.Error=TEXT("This recording ID already identifies an installed dataset. Use a distinct ID for different output.");return Result;}
    if(!Ready(Source,Frame,Cancel,Result.Error))return Result;
    Result.Reference=MoveTemp(Ref);Result.Source=MoveTemp(Source);return Result;
}

static FStudioRecordingLoadResult ReadExternalRecording(const FString& Path,int32 Frame,const FStudioLoadCancellation& Cancellation,bool bCheckInstalledIdentity)
{
    if(FPaths::GetCleanFilename(Path)==TEXT("recording.json"))return ReadPointRecording(Path,Frame,Cancellation,bCheckInstalledIdentity);
    FStudioRecordingLoadResult Result;
    const auto Cancel=Cancellation.IsValid()?Cancellation.ToSharedRef():MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    if(!CleanText(Path,4096)||Path.Contains(TEXT("://"))||FPaths::GetCleanFilename(Path)!=TEXT("flow.bin"))
    {Result.Error=TEXT("Choose the recording folder containing recording.json and its data files. Other CFD formats require conversion.");return Result;}
    const FString Full=FPaths::ConvertRelativePathToFull(Path),Meta=FPaths::GetPath(Full)/TEXT("recording.json");
    FStudioFileAccess PayloadAccess(Full),MetadataAccess(Meta);
    const auto PayloadTime=IFileManager::Get().GetTimeStamp(*Full),MetaTime=IFileManager::Get().GetTimeStamp(*Meta);
    const int64 PayloadSize=IFileManager::Get().FileSize(*Full),MetaSize=IFileManager::Get().FileSize(*Meta);
    if(MetaSize<0||MetaSize>4*1024*1024||PayloadSize<24)
    {Result.Error=TEXT("Recording files are missing, inaccessible or invalid. Locate the folder containing flow.bin and recording.json.");return Result;}
    FStudioRecordingReference Ref; Ref.Path=Full;
    if(!StudioAssets::HashFile(Meta,Cancel,Ref.MetadataSHA256,Result.Error)) return Result;
    auto Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>(Full,8LL*1024*1024,Cancel);
    if(!Source->LoadError().IsEmpty()) {Result.Error=Source->LoadError();return Result;}
    if(Source->Descriptor().MetadataSHA256!=Ref.MetadataSHA256)
    {Result.Error=TEXT("Recording metadata changed while opening. Wait for writing to finish and try again.");return Result;}
    Ref.Id=Source->Descriptor().Id;Ref.Title=Source->Descriptor().Title;
    if(!StudioAssets::HashFile(Full,Cancel,Ref.PayloadSHA256,Result.Error)) return Result;
    if(Ref.PayloadSHA256!=Source->Descriptor().PayloadSHA256)
    {Result.Error=TEXT("Recording payload SHA-256 does not match its metadata. Obtain the original complete recording.");return Result;}
    FString FinalMetadataHash;
    if(!StudioAssets::HashFile(Meta,Cancel,FinalMetadataHash,Result.Error)) return Result;
    if(FinalMetadataHash!=Ref.MetadataSHA256||PayloadTime!=IFileManager::Get().GetTimeStamp(*Full)||
        MetaTime!=IFileManager::Get().GetTimeStamp(*Meta)||PayloadSize!=IFileManager::Get().FileSize(*Full)||MetaSize!=IFileManager::Get().FileSize(*Meta))
    {Result.Error=TEXT("Recording files changed while opening. Wait for writing to finish and try again.");return Result;}
    if(!StudioRecordings::IsValidReference(Ref)) {Result.Error=TEXT("Invalid recording identity or local path.");return Result;}
    // A published installed ID cannot be reused for different metadata/output.
    // An exact portable copy of an installed recording is accepted.
    const FString InstalledPath=bCheckInstalledIdentity?StudioRecordings::PathForId(Ref.Id):FString();
    if(!InstalledPath.IsEmpty())
    {
        FString InstalledHash;
        if(!StudioAssets::HashFile(FPaths::GetPath(InstalledPath)/TEXT("recording.json"),Cancel,InstalledHash,Result.Error)) return Result;
        if(InstalledHash!=Ref.MetadataSHA256)
        {Result.Error=TEXT("This ID belongs to an installed recording with different metadata. Use a distinct recording ID for a different dataset.");return Result;}
    }
    if(!Ready(Source,Frame,Cancel,Result.Error)||Cancel->load()) return Result;
    Result.Reference=MoveTemp(Ref);Result.Source=MoveTemp(Source);return Result;
}

FStudioRecordingLoadResult StudioRecordings::Import(const FString& Path,int32 Frame,const FStudioLoadCancellation& Cancellation)
{
    return ReadExternalRecording(Path,Frame,Cancellation,true);
}

FStudioRecordingLoadResult StudioRecordings::ImportReconstruction(const FStudioRecordingReference& Source,const FString& Path,
    int32 Frame,const FStudioLoadCancellation& Cancellation)
{
    if(!IsValidReference(Source)||Source.Format!=TEXT("point_v3")||!CleanText(Path,4096)||
        Path.Contains(TEXT("://"))||FPaths::GetCleanFilename(Path)!=TEXT("reconstruction.json"))
    {FStudioRecordingLoadResult R;R.Error=TEXT("Choose a reconstruction folder for the current point recording.");return R;}
    auto Result=ReadPointRecording(Source.Path,Frame,Cancellation,false,Source.MetadataSHA256,
        FStudioReconstructionReference{FPaths::ConvertRelativePathToFull(Path),FString()});
    if(Result.Source&&(Result.Reference->Id!=Source.Id||Result.Reference->Title!=Source.Title))
    {Result.Source.Reset();Result.Reference.Reset();Result.Error=TEXT("Reconstruction source identity differs from the saved recording.");}
    return Result;
}

FStudioRecordingLoadResult StudioRecordings::Open(const FString& Id,const TArray<FStudioRecordingReference>& References,
    int32 Frame,const FStudioLoadCancellation& Cancel,const FString& ReplacementPath)
{
    const auto* Ref=References.FindByPredicate([&](const auto& R){return R.Id==Id;});
    if(Ref)
    {
        if(!IsValidReference(*Ref)) {FStudioRecordingLoadResult R;R.Error=TEXT("Saved recording reference is invalid.");return R;}
        // A saved external pair is pinned by its own hashes. Later installed
        // catalog changes cannot override it or make its valid copy unusable.
        auto Result=Ref->Format==TEXT("point_v3")?
            ReadPointRecording(ReplacementPath.IsEmpty()?Ref->Path:ReplacementPath,Frame,Cancel,false,Ref->MetadataSHA256,Ref->Reconstruction):
            ReadExternalRecording(ReplacementPath.IsEmpty()?Ref->Path:ReplacementPath,Frame,Cancel,false);
        if(Result.Source&&(Result.Reference->Id!=Id||Result.Reference->Title!=Ref->Title||
            Result.Reference->Format!=Ref->Format||Result.Reference->MetadataSHA256!=Ref->MetadataSHA256.ToLower()||Result.Reference->PayloadSHA256!=Ref->PayloadSHA256.ToLower()))
        {
            Result.Source.Reset();Result.Reference.Reset();
            Result.Error=TEXT("Recording contents differ from the saved source. Locate an exact copy of the recording folder; import different output separately.");
        }
        return Result;
    }
    FStudioRecordingLoadResult Result;
    if(!ReplacementPath.IsEmpty()) {Result.Error=TEXT("Only saved external recordings can be relocated.");return Result;}
    const FString Path=PathForId(Id);
    if(Path.IsEmpty()) {Result.Error=TEXT("Recording is not installed and has no saved external location. Current project kept.");return Result;}
    auto Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>(Path,8LL*1024*1024,Cancel);
    if(Source->Descriptor().Id!=Id) {Result.Error=TEXT("Installed recording identity does not match its registry.");return Result;}
    if(Ready(Source,Frame,Cancel,Result.Error)) Result.Source=MoveTemp(Source);
    return Result;
}
