#include "StudioImageSequenceRenderer.h"
#include "StudioScene.h"
#include "StudioFileDialog.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Actual original SU2, reconstructed wing and 3D volume output. References
 * come from direct live snapshots before the export, at the same fixed view.
 * No generated flow fields or synthetic-pixel fidelity fixtures. */
class FImageSequenceRenderingCommand final:public IAutomationLatentCommand
{
public:
    explicit FImageSequenceRenderingCommand(FAutomationTestBase* In):Test(In){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        Export.Tick();
        if(Test->HasAnyErrors())return true;
        if(Now-Started>240){Test->AddError(FString::Printf(TEXT("Image sequence render timeout: phase %d, case %d"),Phase,Case));return true;}
        if(!Live.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)if(It->Model&&!It->Model->IsSnapshotView())Live=*It;
        if(!Live.IsValid()||!Live->Model)return false;
        auto& M=*Live->Model;
        if(Phase!=7&&Phase!=8&&Phase!=9&&(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||!Live->HasCurrentFrame()||
            !StudioView::CameraEquals(Live->PresentedCamera(),Live->CameraState())))return false;
        switch(Phase)
        {
        case 0:
        {
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/ImageSequenceRendering");
            IFileManager::Get().DeleteDirectory(*Root,false,true);IFileManager::Get().MakeDirectory(*Root,true);
            Original=M.SnapshotProject();FString Error;
            if(!Test->TestTrue(TEXT("Preserve current project"),StudioProjectIO::Save(Root/TEXT("original.lbms"),Original,Error)))return true;
            M.NewProject(TEXT("Image sequence rendering acceptance"));M.Navigate(EStudioWorkspace::Solve);Phase=1;break;
        }
        case 1:
        {
            if(Case==1)
            {if(!Test->TestTrue(TEXT("Open original NACA fixture"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"))))return true;}
            if(Case==2)
            {if(!Test->TestTrue(TEXT("Open original volume fixture"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json"))))return true;}
            Phase=2;break;
        }
        case 2:
            if(Case)if(!Test->TestTrue(TEXT("Attach matching reconstruction"),M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples")/
                (Case==1?TEXT("NACA0018_SurfaceFixture"):TEXT("Cylinder3D_VolumeFixture"))/TEXT("reconstruction.json"))))return true;
            Phase=3;break;
        case 3:
        {
            M.EditView(TEXT("Frozen export view"),[&](auto& S)
            {
                S.Display.ScalarField=TEXT("pressure");S.Display.bVectors=true;S.Display.VectorCount=64;
                S.Display.bStreamlines=Case!=2;S.Display.bMesh=false;S.Display.bSourcePoints=false;
                S.Display.bReconstructedSurface=Case==1;S.Display.bFocusWingRegion=Case==1;
                S.Display.bVolume=Case!=1;S.Display.bCutPlane=Case==0;
                S.Display.StreamlineSettings.AutomaticSeedCount=16;
            });
            if(Case)
            {
                const auto Field=Live->PresentedField();const auto I=Field->Identity();int64 Id;FVector Point;
                if(!Test->TestTrue(TEXT("Read an authentic probe ID"),Field->OriginalPoint(17,Id,Point)))return true;
                Probe={};Probe.Name=Case==1?TEXT("Wing original point 17"):TEXT("Volume original point 17");Probe.Source={I->Dataset,I->MetadataSHA256,I->PayloadSHA256};
                Probe.Method=EStudioProbeMethod::OriginalPoint;Probe.PointId=Id;
                Test->TestTrue(TEXT("Add source-bound annotation"),M.AddProbe(Probe));M.SelectedInspectionObject=Probe.Id;
            }
            Phase=30;break;
        }
        case 30:
        {
            // Fit after the selected field presentation is current. Orthographic
            // width must be fitted after changing projection for a small domain.
            if(Case==2){auto C=Live->SavedCameraState();C.bOrthographic=true;Live->RestoreCamera(C,TEXT("Orthographic sequence"));}
            Live->FitCamera();
            Ordinals=Case?TArray<int32>{0,1,2}:TArray<int32>{0,300,600};Index=0;Baselines.Reset();Captured=0;
            Name=Case==0?TEXT("su2"):Case==1?TEXT("wing"):TEXT("volume");
            Directory=Root/Name;IFileManager::Get().MakeDirectory(*Directory,true);
            M.ReviewRecordedFrame(Ordinals[Index]);Phase=4;break;
        }
        case 4:
        {
            FStudioSnapshot S;S.Options.Size=Case==1?FIntPoint(640,640):FIntPoint(640,360);FString Error;
            FStudioProbeMarkerResult Markers;
            if(Case)
            {
                const auto Field=Live->PresentedField();int64 Id;FVector P;Field->OriginalPoint(17,Id,P);
                const auto I=Field->Identity();Markers.Project=M.Project.Id;Markers.Source=Probe.Source;Markers.Queries={{Probe.Id,Id}};
                Markers.Positions.Add(Probe.Id,FVector(P.X,P.Z,P.Y)+I->SourceOffset);
            }
            if(!Test->TestTrue(*Error,Live->CaptureSnapshot(S,Case?&Markers:nullptr,Error)))return true;
            Write(S,Directory/FString::Printf(TEXT("reference_%06d.png"),Ordinals[Index]));
            Baselines.Add(Ordinals[Index],S.Pixels);
            Request.Source=M.Solver;Request.FirstOrdinal=Ordinals[0];Request.LastOrdinal=Ordinals.Last();Request.Stride=Case?1:300;
            S.Pixels.Reset();Request.View=MoveTemp(S);
            if(++Index<Ordinals.Num())M.ReviewRecordedFrame(Ordinals[Index]);else Phase=5;
            break;
        }
        case 5:
        {
            FrozenCamera=Live->SavedCameraState();FString Error;
            FFileHelper::SaveStringToFile(StudioSnapshot::Metadata(Request.View),*(Directory/TEXT("anchor.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Export.CapturedForAutomation=[this](const FStudioSnapshot& S)
            {
                ++Captured;FString Error;
                FFileHelper::SaveStringToFile(StudioSnapshot::Metadata(S),*(Directory/FString::Printf(TEXT("captured_%06d.json"),S.Identity.Ordinal)),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
                if(!StudioImageSequence::Matches(Request,S.Identity.Ordinal,S,Error))Test->AddError(Error);
                const auto* Reference=Baselines.Find(S.Identity.Ordinal);
                if(!Test->TestTrue(TEXT("Only selected original images captured"),Reference&&Reference->Num()==S.Pixels.Num()))return;
                double Difference=0;int32 Colored=0;
                for(int32 I=0;I<S.Pixels.Num();++I)
                {
                    const auto A=(*Reference)[I],B=S.Pixels[I];
                    Difference+=FMath::Abs(int32(A.R)-B.R)+FMath::Abs(int32(A.G)-B.G)+FMath::Abs(int32(A.B)-B.B);
                    if(FMath::Max3(B.R,B.G,B.B)>55&&FMath::Max3(B.R,B.G,B.B)-FMath::Min3(B.R,B.G,B.B)>30)++Colored;
                }
                const double Mean=Difference/(3.*S.Pixels.Num());
                Test->AddInfo(FString::Printf(TEXT("Sequence %s original %d: mean RGB difference %.9g; colored %d"),*Name,S.Identity.Ordinal,Mean,Colored));
                Test->TestTrue(TEXT("Independent render matches direct frozen view"),Mean<2.);
                Test->TestTrue(TEXT("Scientific field is visible"),Colored>S.Pixels.Num()/500);
                if(Case)Test->TestFalse(TEXT("Per-frame original probe samples exported"),S.ProbeCSV.IsEmpty());
                if(Captured==1&&Case==2)
                {Live->Model->NewProject(TEXT("Replacement during image export"));Live->Model->Navigate(EStudioWorkspace::Projects);}
            };
            if(!Test->TestTrue(*Error,Export.Start(Live->GetWorld(),Request,Directory,TEXT("export"),Error)))return true;
            Test->TestFalse(TEXT("Image job has one owner"),Export.Start(Live->GetWorld(),Request,Directory,TEXT("second"),Error));
            // Live camera and playback intentionally diverge from the export.
            auto Camera=FrozenCamera;Camera.Position.X+=.25;Live->RestoreCamera(Camera,TEXT("Camera remains interactive during export"));
            InteractiveCamera=Live->SavedCameraState();M.ReviewRecordedFrame(0);M.bLoopPlayback=true;M.Run();
            PlaybackAtStart=M.PlaybackFrame;bPlaybackAdvanced=false;Phase=7;break;
        }
        case 7:
        {
            bPlaybackAdvanced|=M.PlaybackFrame!=PlaybackAtStart;
            int32 Scenes=0;for(TActorIterator<AStudioScene> It(Live->GetWorld());It;++It)if(It->Tags.Contains(TEXT("StudioImageSequence")))++Scenes;
            Test->TestTrue(TEXT("One reusable export scene"),Scenes<=1);
            if(Case!=2)Test->TestTrue(TEXT("Export does not restore or move live camera"),StudioView::CameraEquals(Live->SavedCameraState(),InteractiveCamera));
            const auto Result=Export.Poll();if(!Result)return false;
            if(!Test->TestTrue(*Result->Error,Result->bSuccess&&!Result->bCancelled&&Result->CompletedFrames==3&&Captured==3))return true;
            Test->TestTrue(TEXT("Renderer released its scene before completion"),Export.SceneForAutomation()==nullptr);
            if(Case!=2)Test->TestTrue(TEXT("Playback advanced independently during export"),bPlaybackAdvanced);
            else Test->TestTrue(TEXT("Project replacement cannot retarget export"),M.Project.Id!=Request.View.Project&&M.Workspace==EStudioWorkspace::Projects);
            M.Pause();M.Navigate(EStudioWorkspace::Solve);Phase=8;break;
        }
        case 8:
        {
            FString Error;Export.CapturedForAutomation={};
            if(!Test->TestTrue(TEXT("Start cancellable rendering job"),Export.Start(Live->GetWorld(),Request,Directory,TEXT("cancelled"),Error)))return true;
            Test->TestTrue(TEXT("Cancel before any complete sequence"),Export.Cancel());Phase=9;break;
        }
        case 9:
        {
            const auto Result=Export.Poll();if(!Result)return false;
            Test->TestTrue(TEXT("Cancelled native job drains"),Result->bCancelled&&!Result->bSuccess);
            Test->TestFalse(TEXT("Cancellation publishes no partial images"),IFileManager::Get().DirectoryExists(*(Directory/TEXT("cancelled"))));
            Request.Source.Reset();
            if(++Case<3){Phase=1;break;}
            Test->TestTrue(TEXT("Restore original project"),M.RequestProjectOpen(Root/TEXT("original.lbms")));Phase=10;break;
        }
        case 10:
            Test->TestEqual(TEXT("Original project restored"),M.Project.Id,Original.Id);
            FlushRenderingCommands();CollectGarbage(RF_NoFlags,true);FlushRenderingCommands();
            for(TActorIterator<AStudioScene> It(Live->GetWorld());It;++It)
                Test->TestFalse(TEXT("No export actor survives completion and collection"),It->Tags.Contains(TEXT("StudioImageSequence")));
            return true;
        }
        return false;
    }
private:
    void Write(const FStudioSnapshot& S,const FString& Path)
    {FString Error;TArray64<uint8> PNG;Test->TestTrue(*Error,StudioSnapshot::Encode(S,PNG,Error)&&StudioFileDialog::WriteAtomicBytes(Path,PNG,Error));}
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Live;FStudioProject Original;
    FStudioImageSequenceRenderer Export;FStudioImageSequenceRequest Request;FStudioProbeObject Probe;
    TMap<int32,TArray<FColor>> Baselines;TArray<int32> Ordinals;
    FStudioCameraState FrozenCamera,InteractiveCamera;
    FString Root,Name,Directory;int32 Phase=0,Case=0,Index=0,Captured=0,PlaybackAtStart=0;double Started=0;
    bool bPlaybackAdvanced=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageSequenceRendering,"Studio.ImageSequenceRendering.OriginalViewsAndIsolation",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FImageSequenceRendering::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FImageSequenceRenderingCommand(this));return true;}
#endif
