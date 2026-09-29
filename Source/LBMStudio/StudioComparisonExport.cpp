#include "StudioComparisonExport.h"
#include "StudioSnapshotSource.h"
#include "StudioSavedFieldView.h"
#include "StudioCSVExport.h"
#include "StudioFileDialog.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"

namespace
{
constexpr int64 SideProgress=1000000,TotalProgress=2*SideProgress+1;
bool Cancelled(const FStudioLoadCancellation& C){return C&&C->load();}
bool TextValid(const FString& Text,int32 Limit,bool Required=true)
{
    if((Required&&Text.IsEmpty())||Text.Len()>Limit||Text.TrimStartAndEnd()!=Text)return false;
    for(const TCHAR C:Text)if(C<32||C==127)return false;return true;
}
bool SameScalar(const FStudioScalarDescriptor& A,const FStudioScalarDescriptor& B)
{return A.Id==B.Id&&A.Unit==B.Unit&&A.Label==B.Label&&A.Origin==B.Origin&&A.Minimum==B.Minimum&&A.Maximum==B.Maximum;}
FStudioSavedFieldView View(const FStudioComparisonSide& Side,const FStudioCameraState& Camera)
{FStudioSavedFieldView V;V.Title=Side.Title;V.Identity=Side.Identity;V.Camera=Camera;return V;}
TSharedRef<FJsonObject> SideJSON(const FStudioComparisonSide& Side,const FStudioCameraState& Camera,const FString& File,double Aligned)
{
    auto J=StudioSavedFieldViews::ToJSON(View(Side,Camera));
    J->SetStringField(TEXT("file"),File);J->SetStringField(TEXT("source_url"),Side.SourceURL);J->SetStringField(TEXT("time_note"),Side.TimeNote);
    J->SetNumberField(TEXT("aligned_time_seconds"),Aligned);
    J->SetNumberField(TEXT("source_scalar_minimum"),Side.Scalar.Minimum);J->SetNumberField(TEXT("source_scalar_maximum"),Side.Scalar.Maximum);
    J->SetStringField(TEXT("scalar_label"),Side.Scalar.Label);J->SetStringField(TEXT("scalar_origin"),Side.Scalar.Origin);
    J->SetStringField(TEXT("scalar_expression"),Side.Field->ScalarExpression(Side.Scalar.Id));
    J->SetNumberField(TEXT("original_points"),Side.Field->OriginalPointCount());J->SetNumberField(TEXT("original_triangles"),Side.Field->OriginalTriangleCount());return J;
}
FString Metadata(const FStudioComparisonExportRequest& R)
{
    const auto& P=R.Pair;auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("format"),TEXT("LBMStudio.OriginalComparison"));J->SetNumberField(TEXT("version"),1);
    J->SetStringField(TEXT("name"),R.Name);J->SetStringField(TEXT("project_id"),P.ProjectId.ToString());
    J->SetStringField(TEXT("scalar_id"),P.Scalar);J->SetStringField(TEXT("scalar_unit"),P.Primary.Scalar.Unit);
    J->SetStringField(TEXT("coordinate_system"),R.Coordinates==EStudioExportCoordinates::Source?TEXT("source_xyz"):TEXT("scene_xzy_plus_offset"));
    J->SetStringField(TEXT("coordinate_unit"),TEXT("m"));J->SetBoolField(TEXT("shared_display_range"),R.bSharedRange);
    J->SetStringField(TEXT("alignment"),P.Alignment.Mode==EStudioTimeAlignment::RecordedTime?TEXT("recorded_time"):
        P.Alignment.Mode==EStudioTimeAlignment::ElapsedFromStart?TEXT("elapsed_from_each_start"):TEXT("secondary_manual_offset"));
    J->SetStringField(TEXT("matching"),P.Alignment.Match==EStudioTimeMatch::Exact?TEXT("exact"):TEXT("nearest_earlier_on_tie"));
    J->SetNumberField(TEXT("secondary_offset_seconds"),P.Alignment.SecondaryOffsetSeconds);J->SetNumberField(TEXT("maximum_mismatch_seconds"),P.Alignment.MaximumMismatchSeconds);
    J->SetNumberField(TEXT("secondary_minus_primary_aligned_seconds"),P.Frames.MismatchSeconds);
    J->SetStringField(TEXT("method"),TEXT("Independent original snapshots with explicit time alignment; no temporal interpolation, spatial resampling, subtraction or mesh equivalence assumption."));
    J->SetStringField(TEXT("scalar_basis"),TEXT("Source components; coordinate and camera choices do not rotate scalar values."));
    J->SetStringField(TEXT("topology"),R.Format==EStudioFieldExportFormat::CSV?TEXT("original_points_no_connectivity"):TEXT("original_source_triangles_or_vertex_cells"));
    J->SetBoolField(TEXT("display_reconstruction_applied"),false);
    const FString Extension=R.Format==EStudioFieldExportFormat::CSV?TEXT(".csv"):TEXT(".vtp");
    J->SetObjectField(TEXT("primary"),SideJSON(P.Primary,R.PrimaryCamera,TEXT("primary")+Extension,P.Frames.PrimaryAlignedTime));
    J->SetObjectField(TEXT("secondary"),SideJSON(P.Secondary,R.SecondaryCamera,TEXT("secondary")+Extension,P.Frames.SecondaryAlignedTime));
    FString JSON;FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&JSON));return JSON;
}
TUniquePtr<FArchive> StagedFile(const FString& Path)
{
    // FFileManagerGeneric ignores FILEWRITE_NoReplaceExisting. These paths
    // are inside caller-owned private staging; reject accidental reuse here.
    // Final destination races are handled by exclusive directory publication.
    auto& Files=IFileManager::Get();
    if(Files.FileExists(*Path)||Files.DirectoryExists(*Path))return {};
    return TUniquePtr<FArchive>(Files.CreateFileWriter(*Path,FILEWRITE_NoReplaceExisting));
}
bool WriteText(const FString& Path,const FString& Text,int64& Bytes)
{
    auto File=StagedFile(Path);if(!File)return false;
    const FTCHARToUTF8 UTF8(*Text);File->Serialize(const_cast<char*>(UTF8.Get()),UTF8.Length());Bytes+=UTF8.Length();return File->Close()&&!File->IsError();
}
}

bool StudioComparisonExport::Validate(const FStudioComparisonExportRequest& R,FString& Error,const FStudioLoadCancellation& C)
{
    const auto& P=R.Pair;Error=TEXT("Export requires a completed comparison with matching original fields, units and time alignment.");
    if(Cancelled(C)){Error=TEXT("Comparison export cancelled.");return false;}
    if(!P.ProjectId.IsValid()||P.Frames.Status!=EStudioComparisonStatus::Ready||!P.Frames.Error.IsEmpty()||
        !TextValid(R.Name,120)||!TextValid(P.Scalar,256)||!StudioComparison::IsValidAlignment(P.Alignment)||
        (R.Coordinates!=EStudioExportCoordinates::Source&&R.Coordinates!=EStudioExportCoordinates::Scene)||
        (R.Format!=EStudioFieldExportFormat::CSV&&R.Format!=EStudioFieldExportFormat::VTK))return false;
    const FString Unit=P.Primary.Scalar.Unit;
    if(!TextValid(Unit,256)||Unit.Equals(TEXT("unknown"),ESearchCase::IgnoreCase)||Unit.Equals(TEXT("unspecified"),ESearchCase::IgnoreCase)||Unit!=P.Secondary.Scalar.Unit)return false;
    for(int32 K=0;K<2;++K)
    {
        const auto& S=K?P.Secondary:P.Primary;
        if(!S.Field||!S.Field->IsValid()||!S.Snapshot||!StudioSavedFieldViews::IsValid(View(S,K?R.SecondaryCamera:R.PrimaryCamera))||
            !TextValid(S.SourceURL,8192,false)||S.TimeNote.Len()>8192||!S.Field->Identity()||!StudioSavedFieldViews::SameIdentity(*S.Field->Identity(),S.Identity)||
            S.Scalar.Id!=P.Scalar||S.Snapshot->Ordinal()!=S.Identity.Ordinal||S.Snapshot->ScalarId()!=P.Scalar)return false;
        const auto Scalar=S.Field->Scalar(P.Scalar);const auto& D=S.Snapshot->Descriptor();
        if(!Scalar||!SameScalar(*Scalar,S.Scalar)||S.Title!=D.Title||S.SourceURL!=D.SourceURL||S.TimeNote!=D.TimeNote||
            S.Snapshot->ReadScalarFrame(S.Identity.Ordinal,P.Scalar,C).Field!=S.Field)return false;
    }
    const auto A=StudioComparison::Align(P.Primary.Snapshot->Descriptor(),P.Secondary.Snapshot->Descriptor(),P.Frames.PrimaryOrdinal,P.Alignment,C);
    const auto& B=P.Frames;
    if(A.Status!=EStudioComparisonStatus::Ready||A.PrimaryOrdinal!=P.Primary.Identity.Ordinal||A.SecondaryOrdinal!=P.Secondary.Identity.Ordinal||
        A.PrimaryFrame.Index!=P.Primary.Identity.Frame.Index||A.PrimaryFrame.Time!=P.Primary.Identity.Frame.Time||
        A.SecondaryFrame.Index!=P.Secondary.Identity.Frame.Index||A.SecondaryFrame.Time!=P.Secondary.Identity.Frame.Time||
        A.SecondaryOrdinal!=B.SecondaryOrdinal||A.PrimaryFrame.Index!=B.PrimaryFrame.Index||A.PrimaryFrame.Time!=B.PrimaryFrame.Time||
        A.SecondaryFrame.Index!=B.SecondaryFrame.Index||A.SecondaryFrame.Time!=B.SecondaryFrame.Time||
        A.PrimaryAlignedTime!=B.PrimaryAlignedTime||A.SecondaryAlignedTime!=B.SecondaryAlignedTime||A.MismatchSeconds!=B.MismatchSeconds)return false;
    if(Cancelled(C)){Error=TEXT("Comparison export cancelled.");return false;}Error.Empty();return true;
}

FStudioComparisonExportResult StudioComparisonExport::Write(const FStudioComparisonExportRequest& R,const FString& Directory,
    const FStudioLoadCancellation& C,TFunction<void(int64,int64)> Progress)
{
    FStudioComparisonExportResult Out;Out.Name=R.Name;Out.ProjectId=R.Pair.ProjectId;Out.PrimaryIdentity=R.Pair.Primary.Identity;Out.SecondaryIdentity=R.Pair.Secondary.Identity;
    auto Fail=[&](const FString& Error){Out.bCancelled=Cancelled(C);Out.Error=Out.bCancelled?TEXT("Comparison export cancelled."):Error;return Out;};
    if(!Validate(R,Out.Error,C))return Fail(Out.Error);
    const auto JSON=Metadata(R);if(JSON.Len()>1024*1024)return Fail(TEXT("Comparison metadata exceeds the 1 Mi-character limit."));
    const bool CSV=R.Format==EStudioFieldExportFormat::CSV;
    if(Progress)Progress(0,TotalProgress);
    for(int32 K=0;K<2;++K)
    {
        if(Cancelled(C))return Fail({});
        const auto& Side=K?R.Pair.Secondary:R.Pair.Primary;
        const FString Name=FString(K?TEXT("secondary"):TEXT("primary"))+(CSV?TEXT(".csv"):TEXT(".vtp"));
        auto File=StagedFile(Directory/Name);
        if(!File)return Fail(TEXT("Could not create a staged comparison field. Check directory access and free space."));
        const FStudioFieldExportRequest Request{Side.Field,{R.Pair.Scalar},R.Coordinates,R.Format};
        auto Update=[&](int64 Done,int64 Total){if(Progress)Progress(K*SideProgress+(Total?int64(double(SideProgress)*Done/Total):0),TotalProgress);};
        const auto Result=CSV?StudioCSVExport::Write(Request,*File,C,Update):StudioVTKExport::Write(Request,*File,C,Update);
        const bool Closed=File->Close()&&!File->IsError();File.Reset();Out.Bytes+=Result.Bytes;
        if(!Result.bSuccess)return Fail(Result.Error);if(!Closed)return Fail(TEXT("Could not close a comparison field. Check free space."));
        ++Out.CompletedSides;if(Progress)Progress(Out.CompletedSides*SideProgress,TotalProgress);
    }
    if(Cancelled(C))return Fail({});
    if(!WriteText(Directory/TEXT("comparison.json"),JSON,Out.Bytes))return Fail(TEXT("Could not finish comparison metadata. Check free space."));
    if(!CSV&&!WriteText(Directory/TEXT("comparison.vtm"),TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<VTKFile type=\"vtkMultiBlockDataSet\" version=\"1.0\" byte_order=\"LittleEndian\">\n<vtkMultiBlockDataSet>\n<DataSet index=\"0\" name=\"A\" file=\"primary.vtp\"/>\n<DataSet index=\"1\" name=\"B\" file=\"secondary.vtp\"/>\n</vtkMultiBlockDataSet>\n</VTKFile>\n"),Out.Bytes))
        return Fail(TEXT("Could not finish the comparison collection. Check free space."));
    if(Cancelled(C))return Fail({});if(Progress)Progress(TotalProgress,TotalProgress);Out.bSuccess=true;return Out;
}

struct FStudioComparisonExportWork
{
    std::atomic<EStudioFieldExportState> State{EStudioFieldExportState::Writing};
    std::atomic<int64> Completed{0};
    FStudioLoadCancellation Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
};
FStudioComparisonExportTask::~FStudioComparisonExportTask(){Shutdown();}
bool FStudioComparisonExportTask::Start(FStudioComparisonExportRequest Request,const FString& Parent,const FString& Name,FString& Error)
{
    if(bShutdown||Pending.IsValid()){Error=TEXT("Wait for the comparison export to finish or cancel.");return false;}
    bool ValidName=TextValid(Name,128)&&!Name.StartsWith(TEXT("."));for(const TCHAR C:Name)ValidName&=C!=TEXT('/')&&C!=TEXT('\\')&&C!=TEXT(':');
    if(!ValidName||Parent.IsEmpty()||FPaths::IsRelative(Parent))
    {Error=TEXT("Choose an existing destination folder and a simple new folder name.");return false;}
    Work=MakeShared<FStudioComparisonExportWork,ESPMode::ThreadSafe>();
    Pending=Async(EAsyncExecution::ThreadPool,[Request=MoveTemp(Request),Parent,Name,State=Work
#if WITH_DEV_AUTOMATION_TESTS
        ,BeforePublish=MoveTemp(BeforePublishForAutomation)
#endif
    ]() mutable
    {
        FStudioComparisonExportResult Out;Out.Path=Parent/Name;Out.ProjectId=Request.Pair.ProjectId;Out.Name=Request.Name;
        const FStudioFileAccess Access(Parent);FString Stage;
        ON_SCOPE_EXIT{if(!Stage.IsEmpty())IFileManager::Get().DeleteDirectory(*Stage,false,true);Request.Pair={};};
        auto Fail=[&]
        {
            auto Expected=EStudioFieldExportState::Writing;
            if(!State->State.compare_exchange_strong(Expected,EStudioFieldExportState::Complete)&&Expected==EStudioFieldExportState::Cancelled)
            {Out.bCancelled=true;Out.Error=TEXT("Comparison export cancelled. Destination unchanged.");}
            Out.bSuccess=false;return Out;
        };
        if(State->Cancellation->load())return Fail();
        if(IFileManager::Get().FileExists(*Out.Path)||IFileManager::Get().DirectoryExists(*Out.Path))
        {Out.Error=TEXT("That destination already exists. Choose a new export folder name.");return Fail();}
        if(!StudioFileDialog::CreateExportStage(Parent,Stage,Out.Error))return Fail();
        Out=StudioComparisonExport::Write(Request,Stage,State->Cancellation,[State](int64 Done,int64){State->Completed.store(Done);});Out.Path=Parent/Name;
        if(!Out.bSuccess)return Fail();Out.bSuccess=false;
#if WITH_DEV_AUTOMATION_TESTS
        if(BeforePublish)BeforePublish();
#endif
        auto Expected=EStudioFieldExportState::Writing;
        if(!State->State.compare_exchange_strong(Expected,EStudioFieldExportState::Publishing))return Fail();
        Out.bSuccess=StudioFileDialog::PublishExportDirectory(Stage,Out.Path,Out.Error);
        if(Out.bSuccess)Stage.Empty();State->State.store(EStudioFieldExportState::Complete);return Out;
    });Error.Empty();return true;
}
bool FStudioComparisonExportTask::Cancel()
{
    if(!Work)return false;auto Expected=EStudioFieldExportState::Writing;
    if(!Work->State.compare_exchange_strong(Expected,EStudioFieldExportState::Cancelled))return false;Work->Cancellation->store(true);return true;
}
void FStudioComparisonExportTask::Shutdown()
{if(bShutdown)return;bShutdown=true;Cancel();if(Pending.IsValid()){Pending.Wait();Pending={};}Work.Reset();}
FStudioFieldExportProgress FStudioComparisonExportTask::Progress() const
{return Work?FStudioFieldExportProgress{Work->State.load(),Work->Completed.load(),TotalProgress}:FStudioFieldExportProgress{};}
TOptional<FStudioComparisonExportResult> FStudioComparisonExportTask::Poll()
{if(!Pending.IsValid()||!Pending.IsReady())return {};return Pending.Consume();}
