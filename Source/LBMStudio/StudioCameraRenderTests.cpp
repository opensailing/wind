#include "StudioScene.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioCameraManagerCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioCameraManagerCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Start)Start=Now;
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(Now-Start>40){Test->AddError(TEXT("Camera manager acceptance timed out"));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            if(M.State==EStudioRunState::Running)M.Pause();
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/CameraManager");
            Original=M.SnapshotProject();Original.Camera=Scene->SavedCameraState();
            FString Error;
            Test->TestTrue(TEXT("Preserve original project"),StudioProjectIO::Save(Root/TEXT("original.lbms"),Original,Error));
            auto Candidate=Original;Candidate.Id=FGuid::NewGuid();Candidate.Name=TEXT("Saved camera acceptance");Candidate.Cameras.Reset();
            Candidate.Camera=FStudioCameraState();Candidate.View=FStudioViewSettings();
            Test->TestTrue(TEXT("Create isolated camera test document"),StudioProjectIO::Save(Root/TEXT("library.lbms"),Candidate,Error));
            Test->TestTrue(TEXT("Open isolated document"),M.RequestProjectOpen(Root/TEXT("library.lbms")));
            Phase=1;break;
        }
        case 1:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            Scene->FitCamera();
            OpenManager();Phase=2;break;
        case 2:
            Capture(TEXT("empty.png"));
            Press(TEXT("SaveCamera"));
            Test->TestTrue(TEXT("Blank name has visible validation state"),M.bCameraCollectionError);
            EditText(TEXT("NewCameraName"),TEXT("Wing overview"),false);
            Press(TEXT("SaveCamera"));Phase=3;break;
        case 3:
            if(!Test->TestEqual(TEXT("Routed Save adds a camera"),M.Project.Cameras.Num(),1))return true;
            Id=M.Project.Cameras[0].Id;
            CapturesBeforeEdits=Scene->GetCaptureCount();
            Test->TestTrue(TEXT("Saved view uses exact document values"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Scene->SavedCameraState()));
            EditText(Tag(TEXT("CameraName-"),Id),TEXT("Wake detail"),true);Phase=4;break;
        case 4:
            Test->TestEqual(TEXT("Keyboard rename commits"),M.FindCamera(Id)->Name,FString(TEXT("Wake detail")));
            if(const auto Name=FindTag(Tag(TEXT("CameraName-"),Id)))
                Test->TestTrue(TEXT("Rename retains keyboard focus inside camera manager"),Name->HasKeyboardFocus()||Name->HasFocusedDescendants());
            Press(Tag(TEXT("CameraCopy-"),Id));Phase=5;break;
        case 5:
            if(!Test->TestEqual(TEXT("Routed Copy adds an independent entry"),M.Project.Cameras.Num(),2))return true;
            Test->TestEqual(TEXT("Renaming and copying do not submit new 3D captures"),Scene->GetCaptureCount(),CapturesBeforeEdits);
            CopyId=M.Project.Cameras.Last().Id;
            Test->TestNotEqual(TEXT("Duplicate identity differs"),CopyId,Id);
            EditText(Tag(TEXT("CameraName-"),CopyId),TEXT("Wake detail"),true);
            Test->TestTrue(TEXT("Duplicate name is rejected in UI"),M.bCameraCollectionError);
            Test->TestEqual(TEXT("Rejected rename preserves saved name"),M.FindCamera(CopyId)->Name,FString(TEXT("Wake detail copy")));
            Phase=6;break;
        case 6:
            Capture(TEXT("invalid-name.png"));
            EditText(Tag(TEXT("CameraName-"),CopyId),TEXT("Span view"),true);
            M.Run();Scene->Orbit(33,21);Updated=Scene->SavedCameraState();
            {
                const int32 Selected=M.SelectedFrame,Playback=M.PlaybackFrame;
                const FString Case=StudioCaseIO::Serialize(M.Project.Draft);
                Press(Tag(TEXT("CameraUpdate-"),Id));
                Test->TestTrue(TEXT("Routed Update retains running playback"),M.State==EStudioRunState::Running);
                Test->TestEqual(TEXT("Update does not scrub source"),M.SelectedFrame,Selected);
                Test->TestEqual(TEXT("Update does not move playback cursor"),M.PlaybackFrame,Playback);
                Test->TestEqual(TEXT("Update does not edit case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
            }
            M.Pause();Phase=7;break;
        case 7:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Updated saved camera matches current pose"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Updated));
            Capture(TEXT("library.png"));
            Press(Tag(TEXT("CameraDelete-"),CopyId));
            Phase=8;break;
        case 8:
            Test->TestTrue(TEXT("Delete removes only selected identity"),M.FindCamera(CopyId)==nullptr&&M.FindCamera(Id)!=nullptr);
            Press(TEXT("UndoCameraCollection"));
            Phase=9;break;
        case 9:
            Test->TestTrue(TEXT("UI undo restores deleted entry"),M.FindCamera(CopyId)!=nullptr);
            Press(TEXT("RedoCameraCollection"));
            Phase=10;break;
        case 10:
            Test->TestTrue(TEXT("UI redo repeats deletion"),M.FindCamera(CopyId)==nullptr);
            Press(TEXT("UndoCameraCollection"));
            Phase=11;break;
        case 11:
            Test->TestTrue(TEXT("Second UI undo restores deleted camera"),M.FindCamera(CopyId)!=nullptr);
            Scene->Orbit(-60,17);BeforeActivation=Scene->SavedCameraState();
            Press(Tag(TEXT("CameraActivate-"),Id));Phase=12;break;
        case 12:
            if(!Scene->HasCurrentFrame()||!StudioView::CameraEquals(Scene->SavedCameraState(),Updated))return false;
            Test->TestTrue(TEXT("Renderer applied restored camera"),StudioView::CameraEquals(Scene->CameraState(),Updated));
            Test->TestTrue(TEXT("Saved camera activation can be undone independently"),M.UndoView());
            Test->TestTrue(TEXT("Undo restores prior active pose"),StudioView::CameraEquals(M.Project.Camera,BeforeActivation));
            Test->TestTrue(TEXT("Redo saved view activation"),M.RedoView());
            Test->TestTrue(TEXT("Save camera library through normal document persistence"),M.SaveProject(Root/TEXT("library.lbms")));
            Saved=M.SnapshotProject();
            Test->TestTrue(TEXT("Reopen saved camera library"),M.RequestProjectOpen(Root/TEXT("library.lbms")));
            Phase=13;break;
        case 13:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Library and active view survive reopen"),StudioProjectIO::Serialize(M.SnapshotProject()),StudioProjectIO::Serialize(Saved));
            Test->TestFalse(TEXT("Reopen clears transient collection history"),M.CanUndoSavedCameras()||M.CanRedoSavedCameras());
            Test->TestTrue(TEXT("Exact saved pose survives reopen"),StudioView::CameraEquals(Scene->SavedCameraState(),Updated));
            OpenManager();Phase=14;break;
        case 14:
            Capture(TEXT("reopened.png"));FSlateApplication::Get().DismissAllMenus();
            Test->TestTrue(TEXT("Restore original project"),M.RequestProjectOpen(Root/TEXT("original.lbms")));
            Phase=15;break;
        case 15:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Original document restored"),M.Project.Id,Original.Id);
            return true;
        }
        return false;
    }
private:
    static FName Tag(const TCHAR* Prefix,const FGuid& Id)
    {return FName(FString(Prefix)+Id.ToString(EGuidFormats::Digits));}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Name,bool bButton=false)
    {
        if(!W->GetVisibility().IsVisible())return nullptr;
        if((!bButton&&W->GetTag()==Name)||(bButton&&W->GetType()==TEXT("SButton")))return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)
            if(auto Found=Find(Children->GetChildAt(I),Name,bButton))return Found;
        return nullptr;
    }
    TSharedPtr<SWidget> FindTag(FName Name)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto Found=Find(W,Name))return Found;
        Test->AddError(TEXT("Camera control not found: ")+Name.ToString());return nullptr;
    }
    void Enter(const TSharedPtr<SWidget>& W)
    {
        if(!W)return;
        Test->TestTrue(TEXT("Camera control enabled"),W->IsEnabled());
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
        App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Name){Enter(FindTag(Name));}
    void EditText(FName Name,const FString& Value,bool bCommit)
    {
        const auto Widget=FindTag(Name);if(!Widget)return;
        const auto Box=StaticCastSharedPtr<SEditableTextBox>(Widget);
        FSlateApplication::Get().SetKeyboardFocus(Box,EFocusCause::Navigation);
        Box->SetText(FText::FromString(Value));if(bCommit)Enter(Box);
    }
    void OpenManager()
    {
        const auto Menu=FindTag(TEXT("CameraManager"));if(Menu)Enter(Find(Menu.ToSharedRef(),NAME_None,true));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        Test->TestTrue(TEXT("Capture camera UI"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        if(Pixels.IsEmpty())return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save camera UI evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;
    FStudioProject Original,Saved;FStudioCameraState Updated,BeforeActivation;
    FString Root;FGuid Id,CopyId;int32 Phase=0;double Start=0;uint64 CapturesBeforeEdits=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCameraManagerRender,"Studio.Cameras.ManagerControlsAndPersistence",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioCameraManagerRender::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioCameraManagerCommand(this));return true;}
#endif
