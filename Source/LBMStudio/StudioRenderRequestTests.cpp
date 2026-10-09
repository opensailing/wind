#include "StudioScene.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRenderIntentTest,"Studio.RenderRequests.IntentAndReaderCancellation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioRenderIntentTest::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/RenderIntent"));
    auto Intent=M.RenderIntentRevision;M.Run();Intent=M.RenderIntentRevision;const auto Revision=M.Revision;
    M.Tick(.1);
    TestTrue(TEXT("Natural playback advances frame revision"),M.Revision>Revision);
    TestEqual(TEXT("Natural playback lets a slower worker finish"),M.RenderIntentRevision,Intent);
    M.Pause();TestTrue(TEXT("Pause invalidates pending playback"),M.RenderIntentRevision>Intent);
    Intent=M.RenderIntentRevision;M.Scrub(.5);TestTrue(TEXT("Scrub invalidates pending selection"),M.RenderIntentRevision>Intent);
    Intent=M.RenderIntentRevision;M.EditView(TEXT("Palette"),[](auto& S){S.Display.bVectors=!S.Display.bVectors;});
    TestTrue(TEXT("Display edit invalidates pending geometry"),M.RenderIntentRevision>Intent);
    Intent=M.RenderIntentRevision;auto Camera=M.Project.Camera;Camera.Position.X+=.1;M.EditCamera(TEXT("Move"),Camera);
    TestEqual(TEXT("Camera keeps pending field work"),M.RenderIntentRevision,Intent);
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    const FString Error=M.Solver->LoadError();
    const auto Field=M.Solver->CaptureViewField(300,M.ActiveScalar().Id,true,Cancel);
    TestFalse(TEXT("Cancelled legacy field has no snapshot"),Field->IsValid());
    TestEqual(TEXT("Cancellation does not report source corruption"),M.Solver->LoadError(),Error);
    const auto PointSource=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    if(TestTrue(*PointSource.Error,PointSource.Source.IsValid()))
    {
        const auto PointError=PointSource.Source->LoadError();
        const auto Points=PointSource.Source->CaptureViewField(1,TEXT("pressure"),true,Cancel);
        TestFalse(TEXT("Cancelled point capture has no snapshot"),Points->IsValid());
        TestEqual(TEXT("Point cancellation preserves source health"),PointSource.Source->LoadError(),PointError);
        TestTrue(TEXT("Point capture retries after cancellation"),PointSource.Source->CaptureViewField(1,TEXT("pressure"),true)->IsValid());
    }
    return true;
}

/** Delays acquisition of real, unchanged source fields. It never supplies
 * invented numerical values and cancellation always releases the wait. */
class FDelayedStudioSolver final : public IStudioSolver
{
public:
    explicit FDelayedStudioSolver(TSharedRef<IStudioSolver,ESPMode::ThreadSafe> InSource):Source(InSource){}
    TSharedRef<IStudioSolver,ESPMode::ThreadSafe> Source;
    mutable std::atomic<bool> Hold{false},Entered{false},Cancelled{false},TimedOut{false};
    mutable std::atomic<int32> DelayMilliseconds{0};
    void Arm(){Entered=false;Cancelled=false;Hold=true;}
    int32 FrameCount() const override{return Source->FrameCount();}
    FStudioFrame EvaluateFrame(int32 N) const override{return Source->EvaluateFrame(N);}
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32 N) const override{return Source->CaptureField(N);}
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureViewField(int32 N,const FString& Scalar,bool Vectors,
        const FStudioLoadCancellation& Cancellation={}) const override
    {
        Entered=true;const double Start=FPlatformTime::Seconds();
        while(Hold.load()||(FPlatformTime::Seconds()-Start)*1000.<DelayMilliseconds.load())
        {
            if(Cancellation&&Cancellation->load()){Cancelled=true;break;}
            if(FPlatformTime::Seconds()-Start>8){TimedOut=true;break;}
            FPlatformProcess::SleepNoStats(.001f);
        }
        return Source->CaptureViewField(N,Scalar,Vectors,Cancellation);
    }
    bool ExportField(int32 N,const FString& P) const override{return Source->ExportField(N,P);}
    FString LoadError() const override{return Source->LoadError();}
    const FStudioRecordingDescriptor& Descriptor() const override{return Source->Descriptor();}
    FStudioFrameCacheStats CacheStats() const override{return Source->CacheStats();}
};

class FStudioRenderRequestCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioRenderRequestCommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioRenderRequestCommand(){if(Delayed)Delayed->Hold=false;}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()||Now-Started>45)
        {if(Delayed)Delayed->Hold=false;if(!Test->HasAnyErrors())Test->AddError(TEXT("Render request gate timed out"));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        Test->TestTrue(TEXT("Flow and preview share one worker budget"),Scene->ResourceStats().Workers<=1);
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            if(M.State==EStudioRunState::Running)M.Pause();
            Original=FPaths::ProjectSavedDir()/TEXT("Automation/RenderRequestOriginal.lbms");FString Error;
            Test->TestTrue(TEXT("Preserve project"),StudioProjectIO::Save(Original,M.SnapshotProject(),Error));
            M.NewProject(TEXT("Render request verification"));M.Navigate(EStudioWorkspace::Solve);Phase=1;break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Delayed=MakeShared<FDelayedStudioSolver,ESPMode::ThreadSafe>(M.Solver.ToSharedRef());M.Solver=Delayed;
            Delayed->Arm();M.Scrub(.1);Phase=2;break;
        case 2:
            if(!Delayed->Entered)return false;
            BaselineCaptures=Scene->GetCaptureCount();BaselineFrame=Scene->PresentedFrame().Index;
            M.Scrub(.2);M.Scrub(.8);
            M.SetScalarStyle(1,true,-100,100);
            Scene->Orbit(20,4);Phase=3;break;
        case 3:
            Test->TestNotEqual(TEXT("Superseded frame is never presented"),Scene->PresentedFrame().Index,60);
            if(!Delayed->Cancelled)return false;
            Test->TestTrue(TEXT("Camera captures while original field request is blocked"),Scene->GetCaptureCount()>BaselineCaptures);
            Test->TestEqual(TEXT("Camera retains last valid field identity"),Scene->PresentedFrame().Index,BaselineFrame);
            Delayed->Hold=false;Phase=4;break;
        case 4:
            Test->TestNotEqual(TEXT("No late upload of superseded frame"),Scene->PresentedFrame().Index,60);
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Latest original step presented"),Scene->PresentedFrame().Index,480);
            Test->TestEqual(TEXT("Latest display mapping presented"),Scene->PresentedColorMapping().Palette,1);
            Test->TestEqual(TEXT("Latest mapping maximum"),Scene->PresentedColorMapping().Maximum,100.);
            Test->TestTrue(TEXT("Discard is recorded"),Scene->ResourceStats().DiscardedBuilds>0);
            Delayed->Arm();M.Scrub(.3);Phase=5;break;
        case 5:
            if(!Delayed->Entered)return false;
            PreviousProject=M.Project.Id;M.NewProject(TEXT("Same source replacement"));Phase=6;break;
        case 6:
            Test->TestNotEqual(TEXT("Old project request never reaches replacement"),Scene->PresentedFrame().Index,180);
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Old source acquisition was cancelled"),Delayed->Cancelled.load());
            Test->TestNotEqual(TEXT("Replacement has independent project identity"),M.Project.Id,PreviousProject);
            Test->TestEqual(TEXT("Captured project is current"),Scene->PresentedProjectId(),M.Project.Id);
            Test->TestEqual(TEXT("Replacement source frame"),Scene->PresentedFrame().Index,0);
            Delayed=MakeShared<FDelayedStudioSolver,ESPMode::ThreadSafe>(M.Solver.ToSharedRef());M.Solver=Delayed;
            Delayed->DelayMilliseconds=180;M.DisplayChanged();M.PlaybackRate=4;M.Run();PlaybackStart=Now;Phase=7;break;
        case 7:
            if(Scene->PresentedFrame().Index!=LastFrame){LastFrame=Scene->PresentedFrame().Index;++Presentations;}
            Scene->Orbit(.5,0);
            if(Now-PlaybackStart<2.5)return false;
            Test->TestTrue(TEXT("Playback progresses with reads slower than its clock"),Presentations>=4&&LastFrame>10);
            Test->TestFalse(TEXT("Controlled reader never times out"),Delayed->TimedOut.load());
            M.Pause();Delayed->DelayMilliseconds=0;Phase=8;break;
        case 8:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Pause resolves exact selected frame"),Scene->PresentedFrame().Index,M.DisplayFrame().Index);
            Delayed->Arm();M.Scrub(.4);Phase=10;break;
        case 10:
            if(!Delayed->Entered)return false;
            M.Navigate(EStudioWorkspace::Dashboard);BaselineCaptures=Scene->GetCaptureCount();Phase=11;break;
        case 11:
            if(Scene->ResourceStats().Workers)return false;
            Test->TestTrue(TEXT("Hidden workspace cancels its pending reader"),Delayed->Cancelled.load());
            Test->TestEqual(TEXT("Hidden cancellation does not capture"),Scene->GetCaptureCount(),BaselineCaptures);
            Delayed->Hold=false;M.Navigate(EStudioWorkspace::Solve);Phase=12;break;
        case 12:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Resume presents latest real frame"),Scene->PresentedFrame().Index,240);
            Test->TestTrue(TEXT("Capture carries its camera pose"),StudioView::CameraEquals(Scene->PresentedCamera(),Scene->CameraState()));
            Test->TestTrue(TEXT("Capture carries nonempty viewport dimensions"),Scene->PresentedViewportSize().X>0&&Scene->PresentedViewportSize().Y>0);
            M.Scrub(1);Phase=13;break;
        case 13:
            if(!Scene->HasCurrentFrame())return false;
            M.State=EStudioRunState::Paused;M.Step();Phase=14;break;
        case 14:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Stepping at final source frame still settles capture intent"),Scene->PresentedFrame().Index,600);
            Test->TestEqual(TEXT("Final step completes replay"),M.State,EStudioRunState::Complete);
            Test->TestTrue(TEXT("Restore original project"),M.LoadProject(Original));Phase=9;break;
        case 9:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Restored project captured"),Scene->PresentedProjectId(),M.Project.Id);
            return true;
        }
        return false;
    }
private:
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;
    TSharedPtr<FDelayedStudioSolver,ESPMode::ThreadSafe> Delayed;
    int32 Phase=0,BaselineFrame=0,LastFrame=-1,Presentations=0;uint64 BaselineCaptures=0;
    double Started=0,PlaybackStart=0;FGuid PreviousProject;FString Original;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRenderRequestGPU,"Studio.RenderRequests.ObsoleteWorkAndPlayback",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioRenderRequestGPU::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioRenderRequestCommand(this));return true;}
#endif
