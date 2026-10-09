#include "StudioScene.h"
#include "StudioPointRecording.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioPlatformDiagnostics.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Widgets/SWindow.h"
#include "RHI.h"
#include "RHIResources.h"

#if WITH_DEV_AUTOMATION_TESTS
// Explicit, separate acceptance gate. A short rehearsal verifies the driver;
// only 1200 seconds in each phase constitutes the one-hour M1 baseline.
class FStudioMixedUseCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioMixedUseCommand(FAutomationTestBase* InTest):Test(InTest)
    {
        FParse::Value(FCommandLine::Get(),TEXT("StudioMixedPhaseSeconds="),PhaseSeconds);
        FParse::Value(FCommandLine::Get(),TEXT("StudioPointRecording="),PointPath);
        FParse::Value(FCommandLine::Get(),TEXT("StudioSurfaceReconstruction="),SurfacePath);
        PhaseSeconds=FMath::Max(30.,PhaseSeconds);
        Interval=FMath::Min(10.,PhaseSeconds/6.);
    }
    ~FStudioMixedUseCommand()
    {
        if(Window&&Window->IsWindowMinimized())Window->Restore();
        StudioPlatformDiagnostics::EndWindowEvents(WindowEvents);
    }
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();
        if(!Started)Started=PhaseStarted=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>PhaseSeconds*3+180.){Test->AddError(TEXT("Mixed-use driver timed out"));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        if(!Window)Window=GEngine->GameViewport->GetWindow();
        if(!Window)return false;
        const double Elapsed=Now-PhaseStarted;
        if(Phase>0&&Now-LastSample>=Interval){Sample(Now);LastSample=Now;}
        switch(Phase)
        {
        case 0:
            if(!Scene->HasCurrentFrame())return false;
            if(Window->GetNativeWindow())WindowEvents=StudioPlatformDiagnostics::BeginWindowEvents(Window->GetNativeWindow()->GetOSWindowHandle());
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/MixedUse");IFileManager::Get().MakeDirectory(*Root,true);
            Test->TestTrue(TEXT("Initialize resource telemetry"),FFileHelper::SaveStringToFile(
                TEXT("elapsed_s,phase,phase_s,footprint_bytes,captures,cache_bytes,cache_budget,cache_frames,live_readers,live_frames,live_frame_bytes,mesh_bytes,vertices,indices,sections,workers,render_target_bytes,rhi_count,rhi_bytes,source_load,project_load,selected_frame,presented_frame,minimized,device_allocated_bytes,source_slot,source_frames,point_readers,point_value_bytes,playback_loops,surface_attached,surface_enabled,scalar_texture_bytes,points_enabled,frame_current\n"),
                *(Root/TEXT("resources.csv"))));
            // Development RHIInit enables engine resource tracking before the
            // renderer starts; query it without restarting its global tracker.
            M.NewProject(TEXT("Mixed-use stability acceptance"));M.Navigate(EStudioWorkspace::Solve);
            M.SetControlHarness(false);M.bLoopPlayback=true;M.PlaybackRate=4.;
            if(!PointPath.IsEmpty())
            {
                Test->TestTrue(TEXT("Import full point recording for stability"),M.RequestExternalRecording(PointPath));
                Next(7,Now);break;
            }
            M.Run();
            Next(1,Now);break;
        case 7:
        {
            if(Elapsed>60.){Test->AddError(TEXT("Full point recording import exceeded 60 seconds"));return true;}
            if(M.IsRecordingLoadPending()||!Scene->HasCurrentFrame())break;
            const auto* Ref=M.Project.Recordings.FindByPredicate([&](const auto& R){return R.Id==M.Project.Dataset;});
            if(!Test->TestNotNull(TEXT("Point stability source has a pinned reference"),Ref))return true;
            Test->TestEqual(TEXT("Stability uses full audited recording"),Ref->MetadataSHA256,
                FString(TEXT("1ce4f9f4a7d71f060e60e62ecd0aa52de78e7f7d0328930ef0cdc952cd852a67")));
            Test->TestEqual(TEXT("Stability source contains all original frames"),M.Solver->FrameCount(),8000);
            Test->TestTrue(TEXT("Stability source supplies original points"),M.Solver->Descriptor().bSourcePoints);
            PointSource=M.Project.Dataset;
            M.EditView(TEXT("Point stability display"),[](auto& V)
            {V.Display.ScalarField=TEXT("velocity_magnitude");V.Display.bSourcePoints=true;V.Display.bVectors=true;V.Display.bReconstructedSurface=true;});
            if(!SurfacePath.IsEmpty())
            {
                Test->TestTrue(TEXT("Attach full-source surface for stability"),M.RequestReconstruction(SurfacePath));
                Next(9,Now);break;
            }
            Scene->FitCamera();M.Run();Next(1,Now);break;
        }
        case 9:
            if(Elapsed>60.){Test->AddError(TEXT("Full-source surface import exceeded 60 seconds"));return true;}
            if(M.IsRecordingLoadPending()||!Scene->HasCurrentFrame())break;
            if(!Test->TestTrue(TEXT("Stability source has explicit reconstruction"),M.Solver->Reconstruction().IsValid()))return true;
            Test->TestEqual(TEXT("Stability uses audited full reconstruction"),M.Solver->Reconstruction()->MetadataSHA256,
                FString(TEXT("b9194cdcf7500163650c6a78d696aa8214a06988e245c4e5ea707fdcb511265d")));
            Test->TestTrue(TEXT("Surface texture ready before sustained playback"),Scene->ResourceStats().ScalarTextureBytes>0);
            Scene->FitCamera();M.Run();Next(1,Now);break;
        case 1:
            if(!PointSource.IsEmpty())
            {
                if(M.PlaybackFrame<LastPlayback)++PlaybackLoops;
                LastPlayback=M.PlaybackFrame;
                if(Scene->HasPresentedFrame()&&Scene->PresentedDatasetId()==PointSource&&Scene->PresentedFrame().Index!=LastPresented)
                {
                    LastPresented=Scene->PresentedFrame().Index;++PresentedPointFrames;
                    MinPresented=FMath::Min(MinPresented,LastPresented);MaxPresented=FMath::Max(MaxPresented,LastPresented);
                }
            }
            if(Elapsed<PhaseSeconds)break;
            if(!PointSource.IsEmpty())
            {
                Test->TestTrue(TEXT("Point recording is visibly evolving"),PresentedPointFrames>100);
                if(PhaseSeconds>=1200.)
                {
                    Test->TestTrue(TEXT("Full source naturally loops during sustained playback"),PlaybackLoops>=1);
                    Test->TestTrue(TEXT("Early and late original frames are presented"),MinPresented<2000&&MaxPresented>8500);
                }
            }
            M.Pause();Scene->FitCamera();BaseCamera=Scene->SavedCameraState();
            NextAction=NextSwitch=0;Action=0;Switches=0;Next(2,Now);break;
        case 2:
            // Camera continuously responds while field workers serve bounded,
            // deterministic scrubs, display edits and alternate real recordings.
            {
                auto C=BaseCamera;C.Position+=FVector(FMath::Sin(Elapsed*.7)*.25,0,FMath::Cos(Elapsed*.5)*.15);
                C.Orientation=(C.Focus-C.Position).Rotation().Quaternion();
                Scene->RestoreCamera(C,TEXT("Soak camera"));
            }
            if(M.IsRecordingLoadPending())
            {
                if(Now-SwitchStarted>30.){Test->AddError(TEXT("Recording switch failed to drain within 30 seconds"));return true;}
            }
            else if(!ExpectedSource.IsEmpty())
            {
                Test->TestEqual(TEXT("Requested recording became active"),M.Project.Dataset,ExpectedSource);
                if(ExpectedSource==PointSource)++PointSwitches;
                ExpectedSource.Empty();++Switches;Scene->FitCamera();BaseCamera=Scene->SavedCameraState();
            }
            else if(Elapsed>=NextSwitch)
            {
                if(!PointSource.IsEmpty())
                    ExpectedSource=M.Project.Dataset==PointSource?TEXT("MeshGraphNets_Airfoil_test009"):
                        M.Project.Dataset==TEXT("MeshGraphNets_Airfoil_test009")?TEXT("MeshGraphNets_Airfoil_test010"):PointSource;
                else ExpectedSource=M.Project.Dataset==TEXT("MeshGraphNets_Airfoil_test009")?TEXT("MeshGraphNets_Airfoil_test010"):TEXT("MeshGraphNets_Airfoil_test009");
                Test->TestTrue(TEXT("Mixed-use source switch accepted"),M.RequestRecording(ExpectedSource));SwitchStarted=Now;
                NextSwitch=Elapsed+FMath::Min(30.,PhaseSeconds/4.);
            }
            else if(Elapsed>=NextAction)
            {
                const int32 Count=M.Solver->FrameCount();
                M.Scrub(double((Action*137+41)%Count)/FMath::Max(1,Count-1));
                M.EditView(TEXT("Soak slice"),[&](auto& V)
                {
                    V.Display.SliceAxis=Action%3;const auto& B=M.Solver->Descriptor().DisplayBounds;
                    V.Display.SlicePosition=FMath::Lerp(B.Min[Action%3],B.Max[Action%3],.25+.5*((Action%5)/4.));
                    V.Display.bVectors=(Action%4)!=0;
                    if(M.Solver->Descriptor().bSourcePoints)
                    {
                        const auto& Fields=M.Solver->Descriptor().Scalars;
                        V.Display.ScalarField=Fields[Action%Fields.Num()].Id;
                        // Turning off the surface must actually show original
                        // points in this gate, rather than hiding both layers.
                        V.Display.bSourcePoints=!SurfacePath.IsEmpty()||(Action%3)!=0;
                        V.Display.PointSize=.5+.5*(Action%3);
                        if(!SurfacePath.IsEmpty())V.Display.bReconstructedSurface=(Action%3)!=0;
                    }
                });
                ++Action;NextAction=Elapsed+2.;
            }
            if(Elapsed<PhaseSeconds||M.IsRecordingLoadPending())break;
            Test->TestTrue(TEXT("Mixed phase completed multiple source switches"),Switches>=2);
            Test->TestTrue(TEXT("Mixed phase exercised source scrubbing"),Action>=5);
            if(!PointSource.IsEmpty())Test->TestTrue(TEXT("Mixed use reloaded the long recording"),PointSwitches>=1);
            if(M.State==EStudioRunState::Running)M.Pause();
            if(!PointSource.IsEmpty()&&M.Project.Dataset!=PointSource)
                Test->TestTrue(TEXT("Return to point source for idle acceptance"),M.RequestRecording(PointSource));
            Next(8,Now);break;
        case 8:
            if(Elapsed>60.){Test->AddError(TEXT("Final source failed to settle"));return true;}
            if(M.IsRecordingLoadPending()||!Scene->HasCurrentFrame())break;
            if(!PointSource.IsEmpty())Test->TestEqual(TEXT("Idle tests the full point source"),M.Project.Dataset,PointSource);
            M.EditView(TEXT("Restore display"),[&](auto& V)
            {
                V.Display.SliceAxis=1;V.Display.SlicePosition=0;V.Display.bVectors=true;
                V.Display.bSourcePoints=true;V.Display.PointSize=1;
                V.Display.bReconstructedSurface=true;
                V.Display.ScalarField=M.Solver->Descriptor().DefaultScalar;
            });
            Scene->FitCamera();Next(3,Now);break;
        case 3:
            if(!Scene->HasCurrentFrame()||Scene->bBuilding||Elapsed<1.)break;
            IdleHash=Pixels();IdleCaptures=Scene->GetCaptureCount();
            IdleViewport=Scene->PresentedViewportSize();IdleRevision=M.Revision;IdleIntent=M.RenderIntentRevision;
            Test->TestTrue(TEXT("Save settled inspection before idle"),M.SaveProject(Root/TEXT("retained.lbms")));
            Saved=M.SnapshotProject();Next(4,Now);break;
        case 4:
            if(Scene->GetCaptureCount()!=IdleCaptures)
            {
                const auto C=Scene->SavedCameraState();const auto Presented=Scene->PresentedCamera();
                const auto Target=Scene->GetRenderTarget();
                Test->AddInfo(FString::Printf(TEXT("Unexpected idle capture: count %llu -> %llu; viewport %dx%d -> %dx%d; revision %d -> %d; intent %llu -> %llu; step %d -> %d; cameraEqual=%d displayEqual=%d; minimized=%d backgroundPlayback=%d; savedPosition=%s currentPosition=%s presentedPosition=%s; savedRotation=%s currentRotation=%s"),
                    IdleCaptures,Scene->GetCaptureCount(),IdleViewport.X,IdleViewport.Y,Target->SizeX,Target->SizeY,
                    IdleRevision,M.Revision,IdleIntent,M.RenderIntentRevision,Saved.SelectedFrame,M.SelectedFrame,
                    int32(StudioView::CameraEquals(Saved.Camera,C)),int32(StudioView::DisplayEquals(Saved.View,M.InspectionState().Display)),
                    int32(Window->IsWindowMinimized()),int32(bBackgroundPlayback),*Saved.Camera.Position.ToString(),*C.Position.ToString(),*Presented.Position.ToString(),
                    *Saved.Camera.Orientation.Rotator().ToString(),*C.Orientation.Rotator().ToString()));
            }
            Test->TestEqual(TEXT("Idle/minimized view submits no 3D captures"),Scene->GetCaptureCount(),IdleCaptures);
            if(!bMinimized&&Elapsed>=PhaseSeconds*.5)
            {
                // Minimized playback must not rebuild or capture the unseen scene.
                UE_LOG(LogTemp,Display,TEXT("Studio mixed-use requests native minimize at %.3fs"),Now-Started);
                Window->Minimize();bMinimized=true;
            }
            if(bMinimized&&!bBackgroundPlayback&&Window->IsWindowMinimized()) { M.Run();bBackgroundPlayback=true; }
            if(bMinimized&&Elapsed>=PhaseSeconds*.5+2.)
                Test->TestTrue(TEXT("Native window is actually minimized"),Window->IsWindowMinimized());
            if(Elapsed<PhaseSeconds)break;
            Test->TestEqual(TEXT("Retained texture survives idle/minimized playback"),Pixels(),IdleHash);
            UE_LOG(LogTemp,Display,TEXT("Studio mixed-use requests native restore at %.3fs"),Now-Started);
            M.Pause();Window->Restore();Next(5,Now);break;
        case 5:
            if(!Scene->HasCurrentFrame()||Scene->bBuilding||Elapsed<1.)break;
            Test->TestTrue(TEXT("Restoration presents advanced playback"),Scene->GetCaptureCount()>IdleCaptures);
            Test->TestTrue(TEXT("Saved view reopen accepted"),M.RequestProjectOpen(Root/TEXT("retained.lbms")));Next(6,Now);break;
        case 6:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame()||Scene->bBuilding)break;
            Test->TestEqual(TEXT("Saved frame restored after mixed use"),M.SelectedFrame,Saved.SelectedFrame);
            Test->TestTrue(TEXT("Saved camera restored after mixed use"),StudioView::CameraEquals(Scene->SavedCameraState(),Saved.Camera));
            Test->TestEqual(TEXT("Saved source restored after mixed use"),M.Project.Dataset,Saved.Dataset);
            Test->TestTrue(TEXT("Saved display restored after mixed use"),StudioView::DisplayEquals(M.InspectionState().Display,Saved.View));
            Test->TestTrue(TEXT("Final snapshot succeeds"),Scene->Snapshot(Root/TEXT("final.png")));
            Sample(Now);UE_LOG(LogTemp,Display,TEXT("Studio mixed-use complete: %.1fs; phase target %.0fs; %d switches (%d point); %d scrub/display actions; %d full playback loops; %d presented point frames"),
                Now-Started,PhaseSeconds,Switches,PointSwitches,Action,PlaybackLoops,PresentedPointFrames);
            return true;
        }
        return false;
    }
private:
    void Next(int32 P,double Now)
    { Phase=P;PhaseStarted=Now;UE_LOG(LogTemp,Display,TEXT("Studio mixed-use phase %d started at %.1fs"),P,Now-Started); }
    uint32 Pixels()
    {
        TArray<FColor> Data;
        if(!Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Data)||Data.IsEmpty())
        {Test->AddError(TEXT("Mixed-use GPU readback failed"));return 0;}
        int32 Nonuniform=0;for(const auto& P:Data)if(P!=Data[0])++Nonuniform;
        Test->TestTrue(TEXT("Mixed-use retained GPU texture is nonblank"),Nonuniform>Data.Num()/100);
        return FCrc::MemCrc32(Data.GetData(),Data.Num()*sizeof(FColor));
    }
    void Sample(double Now)
    {
        const auto& M=*Scene->Model;const auto C=M.Solver->CacheStats();const auto L=StudioRecordings::LiveStats();const auto S=Scene->ResourceStats();
        const auto P=StudioPointRecordings::LiveStats();
        uint64 RHIBytes=0;int32 RHICount=0;
#if RHI_ENABLE_RESOURCE_INFO
        TArray<TSharedPtr<FRHIResourceStats>> Stats;RHIGetTrackedResourceStats(Stats);RHICount=Stats.Num();
        for(const auto& R:Stats)RHIBytes+=R->SizeInBytes;
#endif
        const uint64 Footprint=FPlatformMemory::GetStats().UsedPhysical;
        const int64 DeviceBytes=StudioPlatformDiagnostics::DeviceAllocatedBytes();
        const int32 SourceSlot=M.Solver->Descriptor().bSourcePoints?2:M.Project.Dataset==TEXT("MeshGraphNets_Airfoil_test010")?1:0;
        const FString Line=FString::Printf(TEXT("%.3f,%d,%.3f,%llu,%llu,%lld,%lld,%d,%d,%d,%lld,%lld,%lld,%lld,%d,%d,%lld,%d,%llu,%d,%d,%d,%d,%d,%lld,%d,%d,%d,%lld,%d,%d,%d,%lld,%d,%d\n"),
            Now-Started,Phase,Now-PhaseStarted,Footprint,Scene->GetCaptureCount(),C.ResidentBytes,C.BudgetBytes,C.ResidentFrames,
            L.Readers,L.Frames,L.FrameBytes,S.MeshBytes,S.Vertices,S.Indices,S.Sections,S.Workers,S.RenderTargetBytes,RHICount,RHIBytes,
            int32(M.IsRecordingLoadPending()),int32(M.IsProjectOpenPending()),M.SelectedFrame,Scene->PresentedFrame().Index,int32(Window->IsWindowMinimized()),DeviceBytes,
            SourceSlot,M.Solver->FrameCount(),P.Readers,P.AllocatedValueBytes,PlaybackLoops,
            int32(M.Solver->Reconstruction().IsValid()),int32(M.bReconstructedSurface),S.ScalarTextureBytes,
            int32(M.bSourcePoints),int32(Scene->HasCurrentFrame()));
        Test->TestTrue(TEXT("Resource telemetry appended"),FFileHelper::SaveStringToFile(Line,*(Root/TEXT("resources.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&IFileManager::Get(),FILEWRITE_Append));
        Test->TestTrue(TEXT("Cache stays within its byte budget"),C.ResidentBytes<=C.BudgetBytes);
        Test->TestTrue(TEXT("Recorded readers stay bounded across replacements"),L.Readers<=3);
        Test->TestTrue(TEXT("Pinned and cached frames stay bounded across replacements"),L.FrameBytes<=3*8LL*1024*1024+1024*1024);
        Test->TestTrue(TEXT("Derived geometry workers stay bounded"),S.Workers<=2);
        Test->TestTrue(TEXT("Scalar texture remains bounded during mixed use"),S.ScalarTextureBytes<=1024*1024);
        if(!SurfacePath.IsEmpty()&&(Phase==1||Phase==4))
        {
            Test->TestTrue(TEXT("Sustained and idle phases retain reconstruction"),M.Solver->Reconstruction().IsValid());
            Test->TestTrue(TEXT("Sustained and idle phases retain surface display"),M.bReconstructedSurface&&S.ScalarTextureBytes>0);
        }
        if(SourceSlot==2&&!PointSource.IsEmpty()&&Scene->HasCurrentFrame())
        {
            Test->TestEqual(TEXT("Point source step and time agree"),Scene->PresentedFrame().Time,Scene->PresentedFrame().Index*.0025);
            Test->TestEqual(TEXT("Point source displayed identity agrees"),Scene->PresentedDatasetId(),PointSource);
        }
        UE_LOG(LogTemp,Display,TEXT("Studio mixed-use %.0fs phase %d: footprint %.1f MiB, mesh %.1f MiB, cache %.1f MiB, readers %d, workers %d, device %.1f MiB, RHI %d/%.1f MiB, captures %llu"),
            Now-Started,Phase,Footprint/1048576.,S.MeshBytes/1048576.,C.ResidentBytes/1048576.,L.Readers,S.Workers,DeviceBytes/1048576.,RHICount,RHIBytes/1048576.,Scene->GetCaptureCount());
    }
    FAutomationTestBase* Test;void* WindowEvents=nullptr;
    TWeakObjectPtr<AStudioScene> Scene;TSharedPtr<SWindow> Window;
    FStudioCameraState BaseCamera;FStudioProject Saved;FString Root,ExpectedSource,PointPath,PointSource,SurfacePath;
    int32 Phase=0,Action=0,Switches=0;bool bMinimized=false,bBackgroundPlayback=false;
    int32 PointSwitches=0,PlaybackLoops=0,LastPlayback=0,LastPresented=0,PresentedPointFrames=0,MinPresented=MAX_int32,MaxPresented=0;
    double Started=0,PhaseStarted=0,LastSample=0,PhaseSeconds=1200,Interval=10,NextAction=0,NextSwitch=0,SwitchStarted=0;
    uint32 IdleHash=0;uint64 IdleCaptures=0;
    FIntPoint IdleViewport=FIntPoint::ZeroValue;int32 IdleRevision=0;uint64 IdleIntent=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMixedUseTest,"Studio.Stability.MixedUse",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioMixedUseTest::RunTest(const FString&)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("StudioMixedUse")))
    {AddInfo(TEXT("Mixed-use acceptance requires -StudioMixedUse; run Tools/test-stability.sh."));return true;}
    ADD_LATENT_AUTOMATION_COMMAND(FStudioMixedUseCommand(this));return true;
}
#endif
