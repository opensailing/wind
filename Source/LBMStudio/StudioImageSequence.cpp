#include "StudioImageSequence.h"
#include "StudioModel.h"
#include "StudioFileDialog.h"
#include "StudioFieldSequence.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"

namespace
{
constexpr int32 MaximumMetadataCharacters=1024*1024;
bool SameSource(const FStudioFieldIdentity& A,const FStudioFieldIdentity& B)
{
    return A.Dataset==B.Dataset&&A.MetadataSHA256==B.MetadataSHA256&&A.PayloadSHA256==B.PayloadSHA256&&
        A.ReconstructionSHA256==B.ReconstructionSHA256&&A.Interpolation==B.Interpolation&&
        A.SpatialDimensions==B.SpatialDimensions&&A.SourceOffset==B.SourceOffset;
}
bool SameScalar(const FStudioScalarDescriptor& A,const FStudioScalarDescriptor& B)
{return A.Id==B.Id&&A.Unit==B.Unit&&A.Label==B.Label&&A.Origin==B.Origin&&A.Minimum==B.Minimum&&A.Maximum==B.Maximum;}
TSharedPtr<FJsonObject> ViewJSON(const FStudioSnapshot& S)
{
    TSharedPtr<FJsonObject> Full;
    const FString Text=StudioSnapshot::Metadata(S);
    if(Text.Len()>MaximumMetadataCharacters||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Full))return {};
    auto View=MakeShared<FJsonObject>();
    for(const TCHAR* Key:{TEXT("project"),TEXT("size"),TEXT("presented_size"),TEXT("crop_minimum"),TEXT("crop_span"),
        TEXT("camera"),TEXT("projection_matrix_row_major"),TEXT("scalar"),TEXT("inspection_objects"),TEXT("display_schema_version"),TEXT("display_settings"),
        TEXT("selected_object"),TEXT("annotations"),TEXT("legend"),TEXT("frame_label")})
        View->SetField(Key,Full->Values.FindChecked(Key));
    return View;
}
FString JSON(const TSharedRef<FJsonObject>& Object)
{FString Out;FJsonSerializer::Serialize(Object,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));return Out;}
bool Text(FArchive& File,const FString& Value,int64& Bytes)
{
    const FTCHARToUTF8 UTF8(*Value);File.Serialize(const_cast<char*>(UTF8.Get()),UTF8.Length());
    Bytes+=UTF8.Length();return !File.IsError();
}
TUniquePtr<FArchive> CreateFile(const FString& Path)
{
    auto& Files=IFileManager::Get();
    if(Files.FileExists(*Path)||Files.DirectoryExists(*Path))return {};
    return TUniquePtr<FArchive>(Files.CreateFileWriter(*Path,FILEWRITE_NoReplaceExisting));
}
}

bool StudioImageSequence::Validate(const FStudioImageSequenceRequest& R,FString& Error)
{
    if(!StudioMovie::Validate(R.Movie,R.View.Options.Size,Error))return false;
    if(R.Stride<1||R.FirstOrdinal<0||R.LastOrdinal<R.FirstOrdinal||
        (int64(R.LastOrdinal)-R.FirstOrdinal)/FMath::Max(1,R.Stride)+1>MaximumFrames)
    {Error=TEXT("Choose an original frame range and a positive stride, up to 100,000 exported images.");return false;}
    // Reuse the source, scalar and original-timeline checks, including strict
    // times. The source range may exceed the exported count when striding.
    if(!R.Source){Error=TEXT("Choose a verified recording.");return false;}
    // Validate the range in bounded overlapping chunks for long sources;
    // the overlap also checks each chunk boundary's time/step ordering.
    for(int64 First=R.FirstOrdinal;First<=R.LastOrdinal;First+=StudioFieldSequence::MaximumFrames-1)
    {
        const int32 Last=int32(FMath::Min<int64>(R.LastOrdinal,First+StudioFieldSequence::MaximumFrames-1));
        if(!StudioFieldSequence::Validate({R.Source,int32(First),Last,{R.View.Scalar.Id}},Error))return false;
        if(Last==R.LastOrdinal)break;
    }
    const auto& V=R.View;const auto& D=R.Source->Descriptor();
    const auto* Scalar=D.Scalars.FindByPredicate([&](const auto& S){return S.Id==V.Scalar.Id;});
    FString Reconstruction;auto Interpolation=D.bSourcePoints?EStudioFieldInterpolation::None:EStudioFieldInterpolation::SourceTriangles;
    if(const auto Surface=R.Source->Reconstruction()){Reconstruction=Surface->MetadataSHA256;Interpolation=EStudioFieldInterpolation::ReconstructedTriangles;}
    if(const auto Volume=R.Source->VolumeReconstruction()){Reconstruction=Volume->MetadataSHA256;Interpolation=EStudioFieldInterpolation::ReconstructedGrid;}
    Error=TEXT("Freeze a valid presented camera, scalar, image size and original source before exporting images.");
    if(!V.Pixels.IsEmpty()||!V.Project.IsValid()||!StudioSnapshot::ValidSize(V.Options.Size)||
        V.SourceSize.X<=0||V.SourceSize.Y<=0||V.SourceSize.X>16384||V.SourceSize.Y>16384||
        !StudioView::IsValid({V.Camera,V.DisplaySettings})||V.Projection.ContainsNaN()||!(V.Objects==V.DisplaySettings.InspectionObjects)||
        (V.DisplaySettings.ScalarField.IsEmpty()?D.DefaultScalar:V.DisplaySettings.ScalarField)!=V.Scalar.Id||
        !Scalar||!SameScalar(*Scalar,V.Scalar)||
        V.SourceTitle!=D.Title||V.Identity.Dataset!=D.Id||V.Identity.MetadataSHA256!=D.MetadataSHA256||V.Identity.PayloadSHA256!=D.PayloadSHA256||
        V.Identity.SpatialDimensions!=D.SpatialDimensions||V.Identity.SourceOffset!=D.SourceOffset||
        V.Identity.ReconstructionSHA256!=Reconstruction||V.Identity.Interpolation!=Interpolation||
        !D.Frames.IsValidIndex(V.Identity.Ordinal)||V.Identity.Frame.Index!=D.Frames[V.Identity.Ordinal].Index||
        V.Identity.Frame.Time!=D.Frames[V.Identity.Ordinal].Time)return false;
    const auto Framing=StudioSnapshot::Frame(V.SourceSize,V.Options.Size);
    FStudioScalarStyle Style;Style.Dataset=D.Id;Style.Field=V.Scalar.Id;Style.Palette=V.Mapping.Palette;
    Style.bManualRange=V.Mapping.bManualRange;Style.Minimum=V.Mapping.Minimum;Style.Maximum=V.Mapping.Maximum;
    Style.LowColor=V.Mapping.LowColor;Style.MiddleColor=V.Mapping.MiddleColor;Style.HighColor=V.Mapping.HighColor;
    if(!StudioColor::IsValid(Style)||V.Framing.Minimum!=Framing.Minimum||V.Framing.Span!=Framing.Span||!ViewJSON(V))return false;
    Error.Empty();return true;
}

bool StudioImageSequence::Matches(const FStudioImageSequenceRequest& R,int32 Ordinal,const FStudioSnapshot& S,FString& Error)
{
    Error=TEXT("The rendered image differs from the requested original frame or frozen camera/display settings. No sequence published.");
    if(!R.Source||Ordinal<R.FirstOrdinal||Ordinal>R.LastOrdinal||R.Stride<=0||(Ordinal-R.FirstOrdinal)%R.Stride||
        !R.Source->Descriptor().Frames.IsValidIndex(Ordinal))return false;
    const auto& F=R.Source->Descriptor().Frames[Ordinal];
    if(!SameSource(S.Identity,R.View.Identity)||S.Identity.Ordinal!=Ordinal||S.Identity.Frame.Index!=F.Index||S.Identity.Frame.Time!=F.Time||
        S.SourceTitle!=R.View.SourceTitle||!StudioSnapshot::ValidSize(S.Options.Size)||
        S.Pixels.Num()!=int64(S.Options.Size.X)*S.Options.Size.Y)return false;
    const auto A=ViewJSON(R.View),B=ViewJSON(S);
    if(!A||!B)return false;
    // Restoring a component quaternion performs a rotation round trip in UE.
    // Actual captures differed by <4e-11 per component with identical pixels.
    // Bound only this numeric round-off; all other frozen metadata stays exact.
    if(S.Camera.Orientation.ContainsNaN()||!S.Camera.Orientation.Equals(R.View.Camera.Orientation,1.e-9))return false;
    A->GetObjectField(TEXT("camera"))->RemoveField(TEXT("orientation_xyzw"));
    B->GetObjectField(TEXT("camera"))->RemoveField(TEXT("orientation_xyzw"));
    if(!FJsonValue::CompareEqual(FJsonValueObject(A),FJsonValueObject(B)))return false;
    Error.Empty();return true;
}

struct FStudioImageSequenceWork
{
    std::atomic<EStudioFieldExportState> State{EStudioFieldExportState::Writing};
    std::atomic<EStudioImageSequencePhase> Phase{EStudioImageSequencePhase::Preparing};
    std::atomic<int32> Completed{0};
    std::atomic<int64> Bytes{0};
    int32 Total=0,Ordinal=INDEX_NONE;
    FCriticalSection Mutex;
    bool bTaken=false,bAnswered=false;
    TOptional<FStudioSnapshot> Image;
    FString Error;
    FEvent* Wake=FPlatformProcess::GetSynchEventFromPool(false);
    ~FStudioImageSequenceWork(){FPlatformProcess::ReturnSynchEventToPool(Wake);}
};

FStudioImageSequenceTask::~FStudioImageSequenceTask(){Shutdown();}
bool FStudioImageSequenceTask::Start(FStudioImageSequenceRequest R,const FString& Parent,const FString& Name,FString& Error)
{
    check(IsInGameThread());
    if(bShutdown||Pending.IsValid()){Error=TEXT("Wait for the current image export to finish or cancel.");return false;}
    bool ValidName=!Name.IsEmpty()&&Name.Len()<=128&&!Name.StartsWith(TEXT("."))&&Name.TrimStartAndEnd()==Name;
    for(const TCHAR C:Name)ValidName&=C>=32&&C!=127&&C!=TEXT('/')&&C!=TEXT('\\')&&C!=TEXT(':');
    if(!ValidName||Parent.IsEmpty()||FPaths::IsRelative(Parent))
    {Error=TEXT("Choose an existing parent folder and a new folder name without slashes, leading dots or outer spaces.");return false;}
    if(!R.Source||!R.View.Pixels.IsEmpty()||R.FirstOrdinal<0||R.LastOrdinal<R.FirstOrdinal||R.Stride<1||
        !R.Source->Descriptor().Frames.IsValidIndex(R.LastOrdinal)||
        (int64(R.LastOrdinal)-R.FirstOrdinal)/R.Stride+1>StudioImageSequence::MaximumFrames)
    {Error=TEXT("Choose a valid original frame range, stride and frozen view.");return false;}
    FModuleManager::Get().LoadModuleChecked<IModuleInterface>(TEXT("ImageWrapper"));
    Work=MakeShared<FStudioImageSequenceWork,ESPMode::ThreadSafe>();
    Work->Total=(R.LastOrdinal-R.FirstOrdinal)/R.Stride+1;
    Pending=Async(EAsyncExecution::Thread,[R=MoveTemp(R),Parent,Name,W=Work
#if WITH_DEV_AUTOMATION_TESTS
        ,BeforePublish=MoveTemp(BeforePublishForAutomation)
#endif
    ]() mutable
    {
        FStudioImageSequenceResult Out;Out.Path=Parent/Name;Out.Project=R.View.Project;
        const FStudioFileAccess Access(Parent);FString Stage;
        ON_SCOPE_EXIT
        {
            if(!Stage.IsEmpty())IFileManager::Get().DeleteDirectory(*Stage,false,true);
            R.Source.Reset();FScopeLock Lock(&W->Mutex);W->Image.Reset();W->Ordinal=INDEX_NONE;
            W->Phase.store(EStudioImageSequencePhase::Complete);
        };
        auto Cancelled=[&]{return W->State.load()==EStudioFieldExportState::Cancelled;};
        auto Fail=[&](const FString& Reason)
        {
            auto Expected=EStudioFieldExportState::Writing;
            W->State.compare_exchange_strong(Expected,EStudioFieldExportState::Complete);
            Out.bCancelled=Cancelled();Out.bSuccess=false;
            Out.Error=Out.bCancelled?TEXT("Image sequence cancelled. Destination unchanged."):Reason;return Out;
        };
        if(Cancelled()||!StudioImageSequence::Validate(R,Out.Error))return Fail(Out.Error);
        if(IFileManager::Get().FileExists(*Out.Path)||IFileManager::Get().DirectoryExists(*Out.Path))
            return Fail(TEXT("That destination already exists. Choose a new export folder name."));
        if(!StudioFileDialog::CreateExportStage(Parent,Stage,Out.Error))return Fail(Out.Error);
        TUniquePtr<IStudioMovieEncoder> Movie;
        if(R.Movie.bEnabled)
        {
            Movie=IStudioMovieEncoder::Create();
            const FString Description=FString::Printf(TEXT("%s. One selected original CFD frame per movie frame at %d fps. Playback time is not simulation time. Original times and hashes: frames.jsonl and sequence.json. Lossless images: adjacent PNGs."),*R.View.SourceTitle,R.Movie.FrameRate);
            if(!Movie||!Movie->Begin(Stage/TEXT("flow.mp4"),R.View.Options.Size,R.Movie.FrameRate,Description,Out.Error))
                return Fail(Out.Error.IsEmpty()?TEXT("Native movie encoding is unavailable."):Out.Error);
        }
        // Index entries stream to disk; neither images nor per-frame metadata
        // accumulate with sequence length.
        auto Index=CreateFile(Stage/TEXT("frames.jsonl"));
        if(!Index)return Fail(TEXT("Could not create the image index. Check folder access and free space."));
        for(int64 Ordinal=R.FirstOrdinal;Ordinal<=R.LastOrdinal;Ordinal+=R.Stride)
        {
            Out.FailedOrdinal=int32(Ordinal);
            if(Cancelled())return Fail({});
            {
                FScopeLock Lock(&W->Mutex);W->Ordinal=int32(Ordinal);W->bTaken=false;W->bAnswered=false;
                W->Phase.store(EStudioImageSequencePhase::AwaitingImage);
            }
            TOptional<FStudioSnapshot> Image;FString RenderError;
            for(;;)
            {
                if(Cancelled())return Fail({});
                {
                    FScopeLock Lock(&W->Mutex);
                    if(W->bAnswered){Image=MoveTemp(W->Image);W->Image.Reset();RenderError=MoveTemp(W->Error);W->Ordinal=INDEX_NONE;break;}
                }
                W->Wake->Wait();
            }
            if(!Image)return Fail(RenderError.IsEmpty()?TEXT("Could not render the requested original frame."):RenderError);
            if(!StudioImageSequence::Matches(R,int32(Ordinal),*Image,Out.Error))return Fail(Out.Error);
            if(Cancelled())return Fail({});
            W->Phase.store(EStudioImageSequencePhase::Encoding);
            TArray64<uint8> PNG;
            if(!StudioSnapshot::Encode(*Image,PNG,Out.Error))return Fail(Out.Error);
            if(Movie&&!Movie->Append(Image->Pixels,Cancelled,Out.Error))return Fail(Out.Error);
            Image.Reset(); // Release raw pixels before writing or requesting another image.
            if(PNG.Num()>StudioImageSequence::MaximumImageBytes)return Fail(TEXT("An encoded image exceeds the 128 MiB limit."));
            if(Cancelled())return Fail({});
            const FString FileName=FString::Printf(TEXT("frame_%06d.png"),int32(Ordinal));
            auto File=CreateFile(Stage/FileName);
            if(!File)return Fail(TEXT("Could not create a staged image. Check folder access and free space."));
            File->Serialize(PNG.GetData(),PNG.Num());
            if(!File->Close()||File->IsError())return Fail(TEXT("Could not write a complete image. Check free space."));
            File.Reset();const int64 ImageBytes=PNG.Num();PNG.Reset();
            Out.Bytes+=ImageBytes;
            auto Entry=MakeShared<FJsonObject>();const auto F=R.Source->Descriptor().Frames[int32(Ordinal)];
            Entry->SetStringField(TEXT("file"),FileName);Entry->SetNumberField(TEXT("ordinal"),double(Ordinal));
            Entry->SetNumberField(TEXT("source_step"),F.Index);Entry->SetNumberField(TEXT("source_time_seconds"),F.Time);
            Entry->SetNumberField(TEXT("png_bytes"),double(ImageBytes));
            if(Movie)
            {
                Entry->SetNumberField(TEXT("movie_frame"),Out.CompletedFrames);
                Entry->SetNumberField(TEXT("movie_time_seconds"),double(Out.CompletedFrames)/R.Movie.FrameRate);
            }
            if(!Text(*Index,JSON(Entry)+TEXT("\n"),Out.Bytes))return Fail(TEXT("Could not write the image index. Check free space."));
            ++Out.CompletedFrames;W->Completed.store(Out.CompletedFrames);W->Bytes.store(Out.Bytes);
        }
        if(!Index->Close()||Index->IsError())return Fail(TEXT("Could not close the image index. Check free space."));Index.Reset();
        if(Cancelled())return Fail({});
        if(Movie)
        {
            W->Phase.store(EStudioImageSequencePhase::FinalizingMovie);
            if(!Movie->Finish(Cancelled,Out.Error))return Fail(Out.Error);
            Movie.Reset();const int64 MovieBytes=IFileManager::Get().FileSize(*(Stage/TEXT("flow.mp4")));
            if(MovieBytes<=0)return Fail(TEXT("The native encoder did not produce a complete MP4."));
            Out.Bytes+=MovieBytes;W->Bytes.store(Out.Bytes);
        }
        auto Manifest=MakeShared<FJsonObject>();const auto& D=R.Source->Descriptor();
        Manifest->SetStringField(TEXT("format"),TEXT("LBMStudio.ImageSequence"));Manifest->SetNumberField(TEXT("version"),1);
        Manifest->SetStringField(TEXT("dataset"),D.Id);Manifest->SetStringField(TEXT("title"),D.Title);
        Manifest->SetStringField(TEXT("metadata_sha256"),D.MetadataSHA256);Manifest->SetStringField(TEXT("payload_sha256"),D.PayloadSHA256);
        Manifest->SetStringField(TEXT("reconstruction_sha256"),R.View.Identity.ReconstructionSHA256);
        Manifest->SetStringField(TEXT("source_url"),D.SourceURL);Manifest->SetStringField(TEXT("source_time_note"),D.TimeNote);
        Manifest->SetNumberField(TEXT("first_ordinal"),R.FirstOrdinal);Manifest->SetNumberField(TEXT("last_ordinal_inclusive"),R.LastOrdinal);
        Manifest->SetNumberField(TEXT("stride"),R.Stride);Manifest->SetNumberField(TEXT("image_count"),Out.CompletedFrames);
        Manifest->SetStringField(TEXT("index"),TEXT("frames.jsonl"));Manifest->SetObjectField(TEXT("view"),ViewJSON(R.View));
        Manifest->SetStringField(TEXT("time_mapping"),TEXT("One PNG per selected original frame; original times in frames.jsonl. No temporal interpolation or assigned movie frame rate."));
        if(R.Movie.bEnabled)
        {
            auto Video=MakeShared<FJsonObject>();Video->SetStringField(TEXT("file"),TEXT("flow.mp4"));
            Video->SetStringField(TEXT("codec"),TEXT("H.264"));Video->SetStringField(TEXT("container"),TEXT("MPEG-4"));
            Video->SetNumberField(TEXT("frame_rate"),R.Movie.FrameRate);Video->SetNumberField(TEXT("frame_count"),Out.CompletedFrames);
            Video->SetNumberField(TEXT("duration_seconds"),double(Out.CompletedFrames)/R.Movie.FrameRate);
            Video->SetStringField(TEXT("time_mapping"),TEXT("One selected original per movie frame at a fixed presentation rate. No temporal interpolation. Playback time is not simulation time; original times remain in frames.jsonl."));
            Video->SetStringField(TEXT("quality"),TEXT("Lossy presentation movie. Use adjacent lossless PNGs and embedded metadata for image analysis."));
            Manifest->SetObjectField(TEXT("movie"),Video);
            Manifest->SetStringField(TEXT("time_mapping"),TEXT("One PNG and one movie frame per selected original. Original times in frames.jsonl; movie presentation times are separate."));
        }
        auto File=CreateFile(Stage/TEXT("sequence.json"));
        if(!File||!Text(*File,JSON(Manifest)+TEXT("\n"),Out.Bytes)||!File->Close()||File->IsError())
            return Fail(TEXT("Could not finish sequence metadata. Check free space."));
        File.Reset();W->Bytes.store(Out.Bytes);
#if WITH_DEV_AUTOMATION_TESTS
        if(BeforePublish)BeforePublish();
#endif
        auto Expected=EStudioFieldExportState::Writing;
        if(!W->State.compare_exchange_strong(Expected,EStudioFieldExportState::Publishing))return Fail({});
        W->Phase.store(EStudioImageSequencePhase::Publishing);
        Out.bSuccess=StudioFileDialog::PublishExportDirectory(Stage,Out.Path,Out.Error);
        if(Out.bSuccess){Stage.Empty();Out.FailedOrdinal=INDEX_NONE;}
        W->State.store(EStudioFieldExportState::Complete);return Out;
    });Error.Empty();return true;
}
TOptional<int32> FStudioImageSequenceTask::TakeFrameRequest()
{
    check(IsInGameThread());if(!Work||!IsBusy())return {};
    FScopeLock Lock(&Work->Mutex);
    if(Work->State.load()!=EStudioFieldExportState::Writing||Work->Ordinal==INDEX_NONE||Work->bTaken)return {};
    Work->bTaken=true;return Work->Ordinal;
}
bool FStudioImageSequenceTask::Submit(FStudioSnapshot&& Image,FString& Error)
{
    check(IsInGameThread());Error=TEXT("No image is currently requested by this export.");
    if(!Work||!IsBusy())return false;
    FScopeLock Lock(&Work->Mutex);
    if(Work->State.load()!=EStudioFieldExportState::Writing||Work->Ordinal==INDEX_NONE||!Work->bTaken||Work->bAnswered)return false;
    Work->Image=MoveTemp(Image);Work->bAnswered=true;Work->Wake->Trigger();Error.Empty();return true;
}
bool FStudioImageSequenceTask::FailFrame(const FString& Error)
{
    check(IsInGameThread());if(!Work||!IsBusy())return false;
    FScopeLock Lock(&Work->Mutex);
    if(Work->State.load()!=EStudioFieldExportState::Writing||Work->Ordinal==INDEX_NONE||!Work->bTaken||Work->bAnswered)return false;
    Work->Error=Error;Work->bAnswered=true;Work->Wake->Trigger();return true;
}
bool FStudioImageSequenceTask::Cancel()
{
    if(!Work)return false;auto Expected=EStudioFieldExportState::Writing;
    if(!Work->State.compare_exchange_strong(Expected,EStudioFieldExportState::Cancelled))return false;
    Work->Wake->Trigger();return true;
}
void FStudioImageSequenceTask::Shutdown()
{if(bShutdown)return;bShutdown=true;Cancel();if(Pending.IsValid()){Pending.Wait();Pending={};}Work.Reset();}
FStudioImageSequenceProgress FStudioImageSequenceTask::Progress() const
{
    if(!Work)return {};FStudioImageSequenceProgress P;
    P.State=Work->State.load();P.Phase=Work->Phase.load();P.CompletedFrames=Work->Completed.load();
    P.TotalFrames=Work->Total;P.Bytes=Work->Bytes.load();FScopeLock Lock(&Work->Mutex);P.RequestedOrdinal=Work->Ordinal;return P;
}
TOptional<FStudioImageSequenceResult> FStudioImageSequenceTask::Poll()
{if(!Pending.IsValid()||!Pending.IsReady())return {};auto Out=Pending.Consume();Work.Reset();return Out;}
