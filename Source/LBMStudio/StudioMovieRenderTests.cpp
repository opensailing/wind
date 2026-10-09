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

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_MAC
/** Every original SU2 snapshot exactly once: 601 movie frames / 30.05 seconds
 * at 20 fps. Source evolution remains 0.12 seconds. No retained image array,
 * invented CFD frame or time interpolation. */
class FStudioFullMovieCommand final:public IAutomationLatentCommand
{
public:
    explicit FStudioFullMovieCommand(FAutomationTestBase* In):Test(In){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        Export.Tick();if(Test->HasAnyErrors())return true;
        if(Now-Started>600){Test->AddError(TEXT("Full original-frame movie exceeded 600 seconds."));return true;}
        if(!Live.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)if(It->Model&&!It->Model->IsSnapshotView())Live=*It;
        if(!Live.IsValid())return false;auto& M=*Live->Model;
        if(Phase!=3&&(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||!Live->HasCurrentFrame()))return false;
        FString Error;
        switch(Phase)
        {
        case 0:
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/FullMovie");
            IFileManager::Get().DeleteDirectory(*Root,false,true);IFileManager::Get().MakeDirectory(*Root,true);
            Original=M.Project.Id;
            if(!Test->TestTrue(TEXT("Save original project"),StudioProjectIO::Save(Root/TEXT("original.lbms"),M.SnapshotProject(),Error)))return true;
            M.NewProject(TEXT("Complete original recording movie"));M.Navigate(EStudioWorkspace::Solve);
            M.EditView(TEXT("Full movie pressure view"),[](auto& S)
            {S.Display.ScalarField=TEXT("pressure");S.Display.bStreamlines=true;S.Display.StreamlineSettings.AutomaticSeedCount=16;S.Display.bVectors=true;S.Display.VectorCount=64;});
            ++Phase;break;
        case 1:Live->FitCamera();++Phase;break;
        case 2:
        {
            if(!StudioView::CameraEquals(Live->PresentedCamera(),Live->CameraState()))return false;
            FStudioSnapshot S;S.Options.Size=FIntPoint(640,360);
            if(!Test->TestTrue(*Error,Live->CaptureSnapshot(S,nullptr,Error)))return true;
            Request.Source=M.Solver;Request.FirstOrdinal=0;Request.LastOrdinal=M.Solver->FrameCount()-1;Request.Stride=1;Request.Movie={true,20};
            Test->TestEqual(TEXT("Complete authentic SU2 recording"),M.Solver->FrameCount(),601);
            S.Pixels.Reset();Request.View=MoveTemp(S);
            FFileHelper::SaveStringToFile(StudioSnapshot::Metadata(Request.View),*(Root/TEXT("anchor.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Export.CapturedForAutomation=[this](const FStudioSnapshot& Image)
            {
                Test->TestEqual(TEXT("Each original captured once in order"),Image.Identity.Ordinal,Captured);
                FString Error;Test->TestTrue(*Error,StudioImageSequence::Matches(Request,Captured,Image,Error));
                if(Captured==0||Captured==300||Captured==600)
                {TArray64<uint8> PNG;Test->TestTrue(*Error,StudioSnapshot::Encode(Image,PNG,Error)&&StudioFileDialog::WriteAtomicBytes(Root/FString::Printf(TEXT("captured_%06d.png"),Captured),PNG,Error));}
                ++Captured;
            };
            if(!Test->TestTrue(*Error,Export.Start(Live->GetWorld(),Request,Root,TEXT("export"),Error)))return true;
            M.bLoopPlayback=true;M.Run();++Phase;break;
        }
        case 3:
        {
            if(Captured>=LastCameraEdit+50)
            {LastCameraEdit=Captured;auto Camera=Live->SavedCameraState();Camera.Position.X+=.01;Live->RestoreCamera(Camera,TEXT("Live camera during full movie"));}
            int32 Scenes=0;for(TActorIterator<AStudioScene> It(Live->GetWorld());It;++It)if(It->Tags.Contains(TEXT("StudioImageSequence")))++Scenes;
            Test->TestTrue(TEXT("Full export retains at most one reusable scene"),Scenes<=1);
            if(const auto Result=Export.Poll())
            {
                if(!Test->TestTrue(*Result->Error,Result->bSuccess&&Result->CompletedFrames==601&&Captured==601))return true;
                Test->TestTrue(TEXT("Full export releases render scene"),Export.SceneForAutomation()==nullptr);
                Test->TestTrue(TEXT("Live camera stayed interactive"),LastCameraEdit>=550);
                M.Pause();Request.Source.Reset();
                Test->TestTrue(TEXT("Restore project after full movie"),M.RequestProjectOpen(Root/TEXT("original.lbms")));++Phase;
            }
            break;
        }
        case 4:
            Test->TestEqual(TEXT("Original project restored"),M.Project.Id,Original);
            FlushRenderingCommands();CollectGarbage(RF_NoFlags,true);FlushRenderingCommands();
            for(TActorIterator<AStudioScene> It(Live->GetWorld());It;++It)Test->TestFalse(TEXT("No export scene survives GC"),It->Tags.Contains(TEXT("StudioImageSequence")));
            Test->AddInfo(FString::Printf(TEXT("Exported all 601 originals in %.3f wall seconds; movie 30.05 s, source 0.12 s."),Now-Started));return true;
        }
        return false;
    }
private:
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Live;FGuid Original;
    FStudioImageSequenceRenderer Export;FStudioImageSequenceRequest Request;
    FString Root;int32 Phase=0,Captured=0,LastCameraEdit=0;double Started=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFullMovie,"Studio.MovieRendering.CompleteOriginalRecording",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioFullMovie::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioFullMovieCommand(this));return true;}
#endif
