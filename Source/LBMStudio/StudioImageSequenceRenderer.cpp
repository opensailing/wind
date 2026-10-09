#include "StudioImageSequenceRenderer.h"
#include "StudioScene.h"
#include "StudioProbeProfile.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SWindow.h"

FStudioImageSequencePreparedFrame StudioImageSequence::Prepare(const FStudioImageSequenceRequest& R,int32 N,
    const FStudioLoadCancellation& Cancellation)
{
    FStudioImageSequencePreparedFrame Out;
    if(!R.Source||N<R.FirstOrdinal||N>R.LastOrdinal||R.Stride<1||(N-R.FirstOrdinal)%R.Stride)
    {Out.Error=TEXT("The requested image is outside the frozen original frame selection.");return Out;}
    const auto& D=R.Source->Descriptor();const auto& V=R.View;
    const bool Velocity=V.DisplaySettings.bVectors||(V.DisplaySettings.bStreamlines&&
        (!D.bSourcePoints||(D.bPointVelocity&&(R.Source->Reconstruction()||R.Source->VolumeReconstruction()))));
    Out.Snapshot=FStudioSnapshotSource::CreateView(*R.Source,N,V.Scalar.Id,Velocity,Cancellation,Out.Error);
    if(!Out.Snapshot)return Out;
    const auto Field=Out.Snapshot->ReadScalarFrame(N,V.Scalar.Id,Cancellation).Field;
    if(!Field){Out.Error=TEXT("The prepared image lost its original frame.");Out.Snapshot.Reset();return Out;}
    if(const auto Points=Field->OriginalPoints())
    {
        const auto I=Field->Identity();FStudioProbeMarkerRequest Markers;
        Markers.Project=V.Project;Markers.Source={I->Dataset,I->MetadataSHA256,I->PayloadSHA256};
        Markers.Offset=I->SourceOffset;Markers.Geometry=Points->Geometry;
        for(const auto& P:V.Objects.Probes)if(P.bVisible&&P.Source==Markers.Source&&P.Method==EStudioProbeMethod::OriginalPoint&&P.PointId.IsSet())
            Markers.Queries.Add({P.Id,P.PointId.GetValue()});
        Out.Markers=StudioProbeMarkers::Resolve(Markers,Cancellation);
    }
    if(const auto* Probe=V.Objects.Probes.FindByPredicate([&](const auto& P){return P.Id==V.SelectedObject&&P.bVisible;}))
    {
        // Nonzero preparation serial is replaced with the actual capture ID
        // after rendering, before these values can enter exported metadata.
        auto Samples=StudioProbeSampling::Evaluate({V.Project,uint64(N)+1,*Probe,V.Scalar.Id,Field},Cancellation);
        if(Samples.Status==EStudioProbeStatus::Ready)Out.Probe=MoveTemp(Samples);
        else if(Samples.Status!=EStudioProbeStatus::SourceMismatch&&Samples.Status!=EStudioProbeStatus::FieldUnavailable)
        {Out.Snapshot.Reset();Out.Error=Samples.Message.IsEmpty()?TEXT("The selected probe could not be prepared from the original image frame."):Samples.Message;}
        // A genuinely unavailable probe remains absent, just as in a single
        // snapshot. Never carry the anchor frame's previous sample CSV forward.
    }
    if(Cancellation&&Cancellation->load())
    {Out.Snapshot.Reset();Out.Probe.Reset();Out.Error=TEXT("Image frame preparation cancelled.");}
    return Out;
}

FStudioImageSequenceRenderer::~FStudioImageSequenceRenderer(){Shutdown();}
#if WITH_DEV_AUTOMATION_TESTS
AStudioScene* FStudioImageSequenceRenderer::SceneForAutomation() const {return Scene.Get();}
#endif
bool FStudioImageSequenceRenderer::Start(UWorld* InWorld,FStudioImageSequenceRequest Request,
    const FString& Parent,const FString& Name,FString& Error)
{
    check(IsInGameThread());
    if(bShutdown||bActive){Error=TEXT("Wait for the current image export to finish or cancel.");return false;}
    if(!IsValid(InWorld)||!InWorld->IsGameWorld()||Request.View.SourceSize.X<320||Request.View.SourceSize.X>3840||
        Request.View.SourceSize.Y<240||Request.View.SourceSize.Y>2400)
    {Error=TEXT("Freeze a current flow viewport before exporting an image sequence.");return false;}
#if WITH_DEV_AUTOMATION_TESTS
    Writer.BeforePublishForAutomation=MoveTemp(BeforePublishForAutomation);
#endif
    if(!Writer.Start(Request,Parent,Name,Error))return false;
    Frozen=MoveTemp(Request);World=InWorld;Completed.Reset();LastProgress={};
    Visible=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    bActive=true;LastTick=FPlatformTime::Seconds();RenderSeconds=0;return true;
}
void FStudioImageSequenceRenderer::Tick()
{
    check(IsInGameThread());if(!bActive||Completed)return;
    LastProgress=Writer.Progress();
    if(const auto Result=Writer.Poll())
    {Completed=*Result;Release();LastProgress.State=EStudioFieldExportState::Complete;LastProgress.Phase=EStudioImageSequencePhase::Complete;return;}
    if(LastProgress.State==EStudioFieldExportState::Cancelled)
    {if(Cancellation)Cancellation->store(true);if(Visible)Visible->store(false);return;}
    if(!World.IsValid()){Cancel();return;}
    const double Now=FPlatformTime::Seconds(),Delta=FMath::Clamp(Now-LastTick,0.,1.);LastTick=Now;
    bool Minimized=false;
    if(GEngine&&GEngine->GameViewport)if(const auto Window=GEngine->GameViewport->GetWindow())Minimized=Window->IsWindowMinimized();
    if(Preparing.IsValid())
    {
        if(!Preparing.IsReady())return;
        Prepared=Preparing.Consume();
        if(!Prepared->Snapshot){Fail(Prepared->Error);return;}
        if(!Scene.IsValid())
        {
            auto M=MakeShared<FStudioModel>(Prepared->Snapshot.ToSharedRef());
            M->UnitDisplay=Frozen.View.UnitDisplay;M->Project.Id=Frozen.View.Project;M->Project.Camera=Frozen.View.Camera;
            static_cast<FStudioViewSettings&>(*M)=Frozen.View.DisplaySettings;
            M->SelectedInspectionObject=Frozen.View.SelectedObject;M->Project.View=Frozen.View.DisplaySettings;
            auto* Actor=World->SpawnActor<AStudioScene>();
            if(!Actor){Fail(TEXT("Could not create the image export scene."));return;}
            Scene=Actor;Actor->Tags.Add(TEXT("StudioImageSequence"));
            Actor->SetViewVisibility([Flag=Visible]{return Flag->load();});
            Visible->store(true);Actor->Initialize(M);Actor->ResizeViewport(Frozen.View.SourceSize.X,Frozen.View.SourceSize.Y,true);
        }
        else
        {
            auto& M=*Scene->Model;M.Solver=Prepared->Snapshot;M.SelectedFrame=M.PlaybackFrame=Ordinal;
            M.Notice.Empty();M.DisplayChanged();Visible->store(true);
        }
        RenderSeconds=0;
    }
    if(Prepared)
    {
        if(Minimized)return;
        RenderSeconds+=Delta;
        if(!Scene.IsValid()){Fail(TEXT("The image export scene was closed."));return;}
        if(!Scene->HasCurrentFrame())
        {
            if(!Scene->bBuilding&&!Scene->Model->Notice.IsEmpty())Fail(Scene->Model->Notice);
            else if(RenderSeconds>120)Fail(TEXT("The original frame could not finish rendering. Try a smaller image or simpler display settings."));
            return;
        }
        FStudioSnapshot Image;Image.Options=Frozen.View.Options;FString Error;
        if(!Scene->CaptureSnapshot(Image,&Prepared->Markers,Error)){Fail(Error);return;}
        if(Prepared->Probe)
        {
            auto& Probe=*Prepared->Probe;Probe.PresentationId=Image.Capture;
            const FStudioProbeRequest Current{Image.Project,Image.Capture,Probe.Probe,Image.Scalar.Id,Scene->PresentedField()};
            if(!Probe.Matches(Current)||!StudioProbeProfile::CSV(Probe,Image.ProbeCSV,Error))
            {Fail(Error.IsEmpty()?TEXT("The selected probe does not match the captured original image."):Error);return;}
        }
#if WITH_DEV_AUTOMATION_TESTS
        if(CapturedForAutomation)CapturedForAutomation(Image);
#endif
        if(!Writer.Submit(MoveTemp(Image),Error)){Fail(Error);return;}
        Prepared.Reset();Ordinal=INDEX_NONE;Visible->store(false);return;
    }
    if(Minimized)return;
    if(const auto Next=Writer.TakeFrameRequest())
    {
        Ordinal=*Next;
        Preparing=Async(EAsyncExecution::ThreadPool,[Request=Frozen,N=Ordinal,C=Cancellation]
        {return StudioImageSequence::Prepare(Request,N,C);});
    }
}
void FStudioImageSequenceRenderer::Fail(const FString& Error)
{
    Writer.FailFrame(Error);Prepared.Reset();Ordinal=INDEX_NONE;
    if(Visible)Visible->store(false);
}
bool FStudioImageSequenceRenderer::Cancel()
{
    check(IsInGameThread());if(!bActive||!Writer.Cancel())return false;
    if(Cancellation)Cancellation->store(true);if(Visible)Visible->store(false);return true;
}
void FStudioImageSequenceRenderer::Release()
{
    if(Cancellation)Cancellation->store(true);
    if(Visible)Visible->store(false);
    if(Preparing.IsValid()){Preparing.Wait();Preparing={};}
    Prepared.Reset();
    if(Scene.IsValid())Scene->Destroy();Scene.Reset();
    Frozen.Source.Reset();World.Reset();Cancellation.Reset();Visible.Reset();Ordinal=INDEX_NONE;
}
void FStudioImageSequenceRenderer::Shutdown()
{
    check(IsInGameThread());if(bShutdown)return;bShutdown=true;
    Cancel();Writer.Shutdown();Release();Completed.Reset();bActive=false;
}
FStudioImageSequenceProgress FStudioImageSequenceRenderer::Progress() const
{return Completed?LastProgress:Writer.Progress();}
TOptional<FStudioImageSequenceResult> FStudioImageSequenceRenderer::Poll()
{
    check(IsInGameThread());if(!Completed)return {};
    auto Out=MoveTemp(Completed);Completed.Reset();bActive=false;return Out;
}
