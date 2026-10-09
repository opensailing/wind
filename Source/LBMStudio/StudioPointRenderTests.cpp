#include "StudioScene.h"
#include "StudioPointRecording.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Uses the complete original sequence, never the sparse reader fixture.
 * Reader acceptance verifies all source arrays separately. This gate verifies
 * natural playback, presented identity and real Slate controls at display rate. */
class FStudioPointViewportCommand final : public IAutomationLatentCommand
{
public:
    FStudioPointViewportCommand(FAutomationTestBase* InTest,FString InPath,bool bInFullPlayback=true)
        :Test(InTest),Path(MoveTemp(InPath)),bFullPlayback(bInFullPlayback){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(Now-Started>210){Test->AddError(FString::Printf(TEXT("Full point viewport workflow exceeded 210 seconds in phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        if(Phase==0)M.InspectorTab=3; // This workflow starts in the Display owner.

        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            if(M.State==EStudioRunState::Running)M.Pause();
            M.SetControlHarness(false);Original=M.SnapshotProject();Original.Camera=Scene->SavedCameraState();
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/PointViewport");
            IFileManager::Get().MakeDirectory(*Root,true);
            Work=Root/FGuid::NewGuid().ToString();
            FString Error;
            Test->TestTrue(TEXT("Preserve original document"),StudioProjectIO::Save(Work/TEXT("original.lbms"),Original,Error));
            Test->TestTrue(TEXT("Import full original point recording"),M.RequestExternalRecording(Path));
            Phase=1;break;
        }
        case 1:
        {
            if(M.IsRecordingLoadPending()||!Scene->HasCurrentFrame())return false;
            if(!Test->TestEqual(TEXT("Complete original sequence imported"),M.Solver->FrameCount(),8000))return true;
            const auto* Ref=M.Project.Recordings.FindByPredicate([&](const auto& R){return R.Id==M.Project.Dataset;});
            if(!Test->TestNotNull(TEXT("External source reference retained"),Ref))return true;
            Test->TestEqual(TEXT("Exact audited descriptor"),Ref->MetadataSHA256,
                FString(TEXT("1ce4f9f4a7d71f060e60e62ecd0aa52de78e7f7d0328930ef0cdc952cd852a67")));
            Test->TestTrue(TEXT("Import preserves camera"),StudioView::CameraEquals(M.Project.Camera,Original.Camera));
            Test->TestEqual(TEXT("Import preserves case"),StudioCaseIO::Serialize(M.Project.Draft),StudioCaseIO::Serialize(Original.Draft));
            M.EditView(TEXT("Full source acceptance"),[](auto& S)
            {S.Display.ScalarField=TEXT("velocity_magnitude");S.Display.bSourcePoints=true;S.Display.bVectors=true;});
            // Playback settings intentionally live outside inspection undo/redo,
            // matching the separate Replay speed and Loop recording controls.
            M.PlaybackRate=4;M.bLoopPlayback=false;
            M.Scrub(0);Scene->FitCamera();Phase=2;break;
        }
        case 2:
            if(!Scene->HasCurrentFrame())return false;
            View=Scene->SavedCameraState();SourceId=M.Project.Dataset;
            Test->TestEqual(TEXT("Original first step"),Scene->PresentedFrame().Index,1001);
            Test->TestEqual(TEXT("Original first time"),Scene->PresentedFrame().Time,2.5025);
            Test->TestEqual(TEXT("Supplied speed label"),Scene->PresentedScalar().Label,FString(TEXT("Exported speed")));
            Test->TestTrue(TEXT("First original field snapshot"),Scene->Snapshot(Root/TEXT("first-field.png")));
            Capture(TEXT("first.png"));OpenFields();Phase=3;break;
        case 3:
            Capture(TEXT("fields.png"));Press(TEXT("Scalar_pressure"));
            Test->TestFalse(TEXT("Changed scalar invalidates current pixels"),Scene->HasCurrentFrame());
            Test->TestFalse(TEXT("Pending scalar cannot export mismatched pixels"),Scene->Snapshot(Work/TEXT("pending.png")));
            Test->TestEqual(TEXT("Legend retains actual presented scalar while pending"),Scene->PresentedScalar().Id,FString(TEXT("velocity_magnitude")));
            Phase=4;break;
        case 4:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("UI-selected pressure is presented"),Scene->PresentedScalar().Id,FString(TEXT("pressure")));
            Test->TestEqual(TEXT("Pressure units"),Scene->PresentedScalar().Unit,FString(TEXT("Pa")));
            Test->TestTrue(TEXT("Negative source pressure has a valid range"),Scene->PresentedScalar().Minimum<0);
            Capture(TEXT("pressure.png"));OpenFields();Phase=5;break;
        case 5:Press(TEXT("Scalar_cell_volume"));Phase=6;break;
        case 6:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Unknown source units stay unknown"),Scene->PresentedScalar().Unit,FString(TEXT("unspecified")));
            Capture(TEXT("unspecified-units.png"));Press(TEXT("SourcePoints"),EKeys::SpaceBar);Phase=7;break;
        case 7:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestFalse(TEXT("UI can hide source glyphs independently"),M.bSourcePoints);
            Test->TestTrue(TEXT("Vectors remain visible"),M.bVectors);
            Test->TestTrue(TEXT("Hidden glyph geometry released"),Scene->ResourceStats().Vertices<40000);
            Press(TEXT("SourcePoints"),EKeys::SpaceBar);Press(TEXT("PointVectors"),EKeys::SpaceBar);Phase=8;break;
        case 8:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("UI restores point glyphs"),M.bSourcePoints);
            Test->TestFalse(TEXT("Vectors toggle independently"),M.bVectors);
            Press(TEXT("PointVectors"),EKeys::SpaceBar);OpenFields();Phase=9;break;
        case 9:Press(TEXT("Scalar_velocity_magnitude"));Phase=10;break;
        case 10:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Full playback acceptance speed"),M.PlaybackRate,4.);
            if(!bFullPlayback){M.Scrub(1);M.Run();Scene->RestoreCamera(View,TEXT("Restore acceptance camera"));Phase=12;break;}
            M.Run();PlaybackStart=Now;LastPresented=1000;Phase=11;break;
        case 11:
        {
            // Do not scrub, manually tick or duplicate frames during this phase.
            // Real wall-time drives the ordinary playback clock end to end.
            if(Scene->HasPresentedFrame())
            {
                const auto F=Scene->PresentedFrame();
                if(F.Index!=LastPresented)
                {
                    Test->TestTrue(TEXT("Presented original steps are monotonic"),F.Index>LastPresented);
                    Test->TestEqual(TEXT("Presented step/time alignment"),F.Time,F.Index*.0025);
                    Test->TestEqual(TEXT("Presented source identity"),Scene->PresentedDatasetId(),SourceId);
                    LastPresented=F.Index;++PresentedFrames;
                }
            }
            if(Now-LastSample>=1)
            {
                LastSample=Now;const auto R=Scene->ResourceStats();const auto P=StudioPointRecordings::LiveStats();
                const auto C=M.Solver->CacheStats();
                Samples+=FString::Printf(TEXT("%.3f,%d,%d,%lld,%lld,%lld,%d\n"),Now-PlaybackStart,M.PlaybackFrame,LastPresented,
                    C.ResidentBytes,P.AllocatedValueBytes,R.MeshBytes,R.Workers);
                PeakMesh=FMath::Max(PeakMesh,R.MeshBytes);PeakValues=FMath::Max(PeakValues,P.AllocatedValueBytes);
                Test->TestTrue(TEXT("One bounded render worker"),R.Workers<=1);
                Test->TestTrue(TEXT("Source cache bounded"),C.ResidentBytes<=C.BudgetBytes);
                Test->TestTrue(TEXT("Point value allocations bounded"),P.AllocatedValueBytes<=48LL*1024*1024);
                Test->TestTrue(TEXT("Scene mesh bounded"),R.MeshBytes<=128LL*1024*1024);
                const auto Selected=M.SelectedFrame,Playback=M.PlaybackFrame;
                Scene->Orbit(2,1);
                Test->TestEqual(TEXT("Camera gesture does not scrub"),M.SelectedFrame,Selected);
                Test->TestEqual(TEXT("Camera gesture does not advance replay"),M.PlaybackFrame,Playback);
            }
            if(M.State!=EStudioRunState::Complete)return false;
            Test->TestEqual(TEXT("Natural playback reaches final original ordinal"),M.PlaybackFrame,7999);
            Test->TestTrue(TEXT("Full recording has sustained visible evolution"),PresentedFrames>500);
            Test->TestTrue(TEXT("Playback used wall time"),Now-PlaybackStart>=95);
            Scene->RestoreCamera(View,TEXT("Restore acceptance camera"));Phase=12;break;
        }
        case 12:
            if(!Scene->HasCurrentFrame()||M.State!=EStudioRunState::Complete)return false;
            Test->TestEqual(TEXT("Original final step"),Scene->PresentedFrame().Index,9000);
            Test->TestEqual(TEXT("Original final time"),Scene->PresentedFrame().Time,22.5);
            Test->TestTrue(TEXT("Last original field snapshot"),Scene->Snapshot(Root/TEXT("last-field.png")));
            {
                TArray<uint8> First,Last;
                FFileHelper::LoadFileToArray(First,*(Root/TEXT("first-field.png")));
                FFileHelper::LoadFileToArray(Last,*(Root/TEXT("last-field.png")));
                Test->TestTrue(TEXT("Different original fields produce different pixels at the same camera"),!First.IsEmpty()&&!Last.IsEmpty()&&First!=Last);
            }
            Capture(TEXT("complete.png"));
            {
                const FString CSV=Work/TEXT("displayed-frame.csv");FString Text;
                Test->TestTrue(TEXT("Export displayed original frame"),M.ExportField(CSV));
                Test->TestTrue(TEXT("Read exported frame"),FFileHelper::LoadFileToString(Text,*CSV));
                Test->TestTrue(TEXT("Export identity matches displayed source step and time"),Text.Contains(TEXT(",9000,\"t_9000\",22.5,")));
            }
            Test->TestTrue(TEXT("Save complete point view"),M.SaveProject(Work/TEXT("point.lbms")));Saved=M.SnapshotProject();
            Test->TestTrue(TEXT("Reopen point view"),M.RequestProjectOpen(Work/TEXT("point.lbms")));Phase=13;break;
        case 13:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Saved point view restored exactly"),StudioProjectIO::Serialize(M.SnapshotProject()),StudioProjectIO::Serialize(Saved));
            Capture(TEXT("reopened.png"));IdleStart=Now;IdleCaptures=Scene->GetCaptureCount();Phase=14;break;
        case 14:
            if(Now-IdleStart<2)return false;
            Test->TestEqual(TEXT("Unchanged point view makes no extra captures"),Scene->GetCaptureCount(),IdleCaptures);
            if(bFullPlayback)
            {
                FFileHelper::SaveStringToFile(TEXT("seconds,ordinal,presented_step,cache_bytes,point_value_bytes,mesh_bytes,workers\n")+Samples,*(Root/TEXT("resources.csv")));
                Test->AddInfo(FString::Printf(TEXT("Natural 8000-frame playback: %d distinct presented steps; peak point values=%lld mesh=%lld bytes."),PresentedFrames,PeakValues,PeakMesh));
            }
            else Test->AddInfo(TEXT("Routed display controls, first/last frame export and reopen. This short gate does not establish full-sequence playback or long-duration stability."));
            Test->TestTrue(TEXT("Restore original source/document"),M.RequestProjectOpen(Work/TEXT("original.lbms")));Phase=15;break;
        case 15:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Original project restored"),M.Project.Id,Original.Id);
            Test->TestTrue(TEXT("Original camera restored"),StudioView::CameraEquals(Scene->SavedCameraState(),Original.Camera));
            Test->TestEqual(TEXT("Point source released after switching"),StudioPointRecordings::LiveStats().Readers,0);
            Test->TestEqual(TEXT("Point arrays released after switching"),StudioPointRecordings::LiveStats().AllocatedValueBytes,int64(0));
            IFileManager::Get().DeleteDirectory(*Work,false,true);return true;
        }
        return false;
    }
private:
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag,bool bButton=false)
    {
        if(!W->GetVisibility().IsVisible())return nullptr;
        if((!bButton&&W->GetTag()==Tag)||(bButton&&W->GetType()==TEXT("SButton")))return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)
            if(auto Found=Find(Children->GetChildAt(I),Tag,bButton))return Found;
        return nullptr;
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto Found=Find(W,Tag))return Found;
        Test->AddError(TEXT("Point display control missing: ")+Tag.ToString());return nullptr;
    }
    void Key(const TSharedPtr<SWidget>& W,FKey K=EKeys::Enter)
    {
        if(!W)return;Test->TestTrue(TEXT("Point display control enabled"),W->IsEnabled());
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(K,FModifierKeysState(),0,false,0,0));
        App.ProcessKeyUpEvent(FKeyEvent(K,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag,FKey K=EKeys::Enter){Key(FindTag(Tag),K);}
    void OpenFields(){if(const auto W=FindTag(TEXT("ScalarSelector")))Key(Find(W.ToSharedRef(),NAME_None,true));}
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture point display UI"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save point UI evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioProject Original,Saved;FStudioCameraState View;
    FString Path,Root,Work,SourceId,Samples;int32 Phase=0,LastPresented=0,PresentedFrames=0;
    double Started=0,PlaybackStart=0,LastSample=0,IdleStart=0;uint64 IdleCaptures=0;int64 PeakMesh=0,PeakValues=0;
    bool bFullPlayback=true;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointViewportFull,"ScientificAcceptance.PointRecording.ViewportFullSequence",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioPointViewportFull::RunTest(const FString&)
{
    FString Path;if(!FParse::Value(FCommandLine::Get(),TEXT("StudioPointRecording="),Path))
    {AddError(TEXT("Supply the complete audited recording with -StudioPointRecording."));return false;}
    ADD_LATENT_AUTOMATION_COMMAND(FStudioPointViewportCommand(this,Path));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointViewportControls,"ScientificAcceptance.PointRecording.ViewportControls",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioPointViewportControls::RunTest(const FString&)
{
    FString Path;if(!FParse::Value(FCommandLine::Get(),TEXT("StudioPointRecording="),Path))
    {AddError(TEXT("Supply the complete audited recording with -StudioPointRecording."));return false;}
    ADD_LATENT_AUTOMATION_COMMAND(FStudioPointViewportCommand(this,Path,false));return true;
}
#endif
