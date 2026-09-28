#include "StudioScene.h"
#include "StudioAuthoringTestCapture.h"
#include "StudioDomain.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioDomainHandlesCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioDomainHandlesCommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioDomainHandlesCommand(){if(FSlateApplication::IsInitialized()){ReleasePointer();if(bCapturedTooltips)FSlateApplication::Get().SetAllowTooltips(bTooltips);}}
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors())return true;
        if(FPlatformTime::Seconds()-Started>90){Test->AddError(FString::Printf(TEXT("Domain handles timed out at phase %d"),Phase));return true;}
        // Native capture requests are ignored by Slate while macOS has another
        // app active. Establish this input precondition before sending gestures.
        if(!FSlateApplication::Get().IsActive())
        {
            if(FPlatformTime::Seconds()-LastActivation>1.)
            {
                Test->AddInfo(TEXT("Activating the native window before Domain pointer input."));
                FPlatformApplicationMisc::ActivateApplication();
                if(GEngine&&GEngine->GameViewport)GEngine->GameViewport->GetWindow()->BringToFront(true);
                LastActivation=FPlatformTime::Seconds();ChangedFrame=GFrameCounter;
            }
            return false;
        }
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter-ChangedFrame<4)return false;
        auto& M=*Scene->Model;if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        switch(Phase)
        {
        case 0:
        {
            M.Navigate(EStudioWorkspace::Solve);if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/Domain");Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;Test->TestTrue(TEXT("Save previous session"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=FSlateApplication::Get().GetAllowTooltips();bCapturedTooltips=true;FSlateApplication::Get().SetAllowTooltips(false);
            // Retired native tooltip windows must leave the capture tree before the next frame.
            StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Direct domain resizing"));M.Pause();FlowCamera=M.Project.Camera;Frame=M.SelectedFrame;Revision=M.RenderIntentRevision;Faces=M.Project.Draft.Domain.Faces;
            Press(TEXT("Workspace3"));Next();break;
        }
        case 1:
            if(!Scene->HasDomainPreview())return false;
            Observer=Scene->CameraState();Observer.Position=FVector(5,6,4);Observer.Focus=FVector::ZeroVector;
            Observer.Orientation=(-Observer.Position).Rotation().Quaternion();Observer.OrbitDistance=Observer.Position.Size();
            Scene->RestoreCamera(Observer,TEXT("Test observer"));Next();break;
        case 2:
            if(!Scene->HasDomainPreview())return false;
            Type(TEXT("DomainValue18"),TEXT("4"),true);Next();break;
        case 3:
            if(!Scene->HasDomainPreview())return false;
            Test->TestEqual(TEXT("Typed dimension anchors minimum"),M.Project.Draft.Domain.Min.X,-1.);
            Test->TestEqual(TEXT("Typed dimension controls maximum"),M.Project.Draft.Domain.Max.X,3.);
            Capture(TEXT("dimensions.png"));Press(TEXT("DomainUndo"));Next();break;
        case 4:
            if(!Scene->HasDomainPreview())return false;
            Test->TestEqual(TEXT("Dimension is one case edit"),M.Project.Draft.Domain.Max.X,1.);
            Observer=Scene->CameraState();Case=StudioCaseIO::Serialize(M.Project.Draft);DragFace(false);Next();break;
        case 5:
            Test->TestTrue(TEXT("Handle changes retained numeric draft"),FMath::Abs(FCString::Atod(*Text(TEXT("DomainValue3")))-1.5)<1.e-6);
            Test->TestEqual(TEXT("Drag does not apply case implicitly"),StudioCaseIO::Serialize(M.Project.Draft),Case);
            Test->TestTrue(TEXT("Dragging does not orbit observer"),StudioView::CameraEquals(Scene->CameraState(),Observer));
            Capture(TEXT("handle-draft.png"));Escape();Next();break;
        case 6:
            Test->TestEqual(TEXT("Escape restores pre-drag draft"),Text(TEXT("DomainValue3")),FString(TEXT("1")));
            Test->TestEqual(TEXT("Escape leaves case unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Case);
            if(!Scene->HasDomainPreview())return false;DragFace(true);Next();break;
        case 7:Capture(TEXT("handle-released.png"));Press(TEXT("DomainApply"));Next();break;
        case 8:
            if(!Scene->HasDomainPreview())return false;
            Test->TestTrue(TEXT("Apply commits physical face coordinate"),FMath::Abs(M.Project.Draft.Domain.Max.Y-1.5)<1.e-6);
            Test->TestTrue(TEXT("Resize keeps boundary targets"),M.Project.Draft.Domain.Faces==Faces);
            Capture(TEXT("handle-applied.png"));Press(TEXT("DomainUndo"));Next();break;
        case 9:
            if(!Scene->HasDomainPreview())return false;
            Test->TestEqual(TEXT("One undo restores entire drag"),M.Project.Draft.Domain.Max.Y,1.);DragFace(false);
            if(auto Input=FindTag(TEXT("DomainValue0")))FSlateApplication::Get().SetKeyboardFocus(Input,EFocusCause::Navigation);
            ReleasePointer();Next();break;
        case 10:
            Test->TestEqual(TEXT("Losing focus cancels active resize"),Text(TEXT("DomainValue3")),FString(TEXT("1")));
            if(!Scene->HasDomainPreview())return false;DragFace(false);
            {auto External=M.Project.Draft.Domain;External.Max.Z=2.;Test->TestTrue(TEXT("Concurrent case edit applies"),M.UpdateDomain(External));}Next();break;
        case 11:
            ReleasePointer();Test->TestEqual(TEXT("Stale gesture cannot overwrite external edit"),M.Project.Draft.Domain.Max.Z,2.);
            Test->TestTrue(TEXT("Conflicting text remains visible"),FMath::Abs(FCString::Atod(*Text(TEXT("DomainValue3")))-1.5)<1.e-6);
            if(auto Apply=FindTag(TEXT("DomainApply")))Test->TestFalse(TEXT("Stale draft requires reconciliation"),Apply->IsEnabled());
            Capture(TEXT("handle-conflict.png"));Press(TEXT("DomainRevert"));Next();break;
        case 12:
            Test->TestEqual(TEXT("Revert loads external bounds"),Text(TEXT("DomainValue5")),FString(TEXT("2")));
            Test->TestTrue(TEXT("Flow camera remains exact"),StudioView::CameraEquals(M.Project.Camera,FlowCamera));
            Test->TestEqual(TEXT("Source frame remains exact"),M.SelectedFrame,Frame);Test->TestEqual(TEXT("No CFD recomputation intent"),M.RenderIntentRevision,Revision);
            Test->TestTrue(TEXT("Restore earlier session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 13:M.Navigate(EStudioWorkspace::Solve);return true;
        }
        return false;
    }
private:
    void Next(){++Phase;ChangedFrame=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& Widget,FName Tag,bool bFocusable=false)
    {
        if(!Widget->GetVisibility().IsVisible())return {};
        if((bFocusable&&Widget->SupportsKeyboardFocus()&&Widget->IsEnabled())||(!bFocusable&&Widget->GetTag()==Tag))return Widget;
        auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Result=Find(Children->GetChildAt(I),Tag,bFocusable))return Result;
        return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Result=Find(Window,Tag))return Result;
        Test->AddError(TEXT("Missing domain control: ")+Tag.ToString());return {};
    }
    void Enter(const TSharedPtr<SWidget>& Widget)
    {
        if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag){Enter(FindTag(Tag));}
    void Open(FName Tag){if(auto Widget=FindTag(Tag))Enter(Find(Widget.ToSharedRef(),NAME_None,true));}
    FString Text(FName Tag){auto Widget=FindTag(Tag);return Widget?StaticCastSharedPtr<SEditableTextBox>(Widget)->GetText().ToString():FString();}
    void Type(FName Tag,const FString& Value,bool bCommit)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        const FModifierKeysState All(false,false,true,false,false,false,false,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,All,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,All,0,false,0,0));
        for(TCHAR C:Value)App.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));if(bCommit)Enter(Widget);
    }
    void SaveShortcut()
    {
        auto& App=FSlateApplication::Get();const FModifierKeysState Command(false,false,false,false,false,false,true,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));
    }
    void DragFace(bool bRelease)
    {
        const auto Viewport=FindTag(TEXT("DomainViewport"));if(!Viewport)return;
        const auto& G=Viewport->GetCachedGeometry();const auto Size=G.GetLocalSize();const auto Pixels=Scene->PresentedViewportSize();
        const double Aspect=Pixels.Y>0?double(Pixels.X)/Pixels.Y:0.;
        const FVector Center=StudioDomain::FaceCenter(Scene->Model->Project.Draft.Domain,3);FVector2D A,B;
        if(!Test->TestTrue(TEXT("Positive Y handle projects"),StudioCameraPlacement::Project(Scene->PresentedCamera(),Size,Center,A,Aspect)))return;
        if(!Test->TestTrue(TEXT("Half meter movement projects"),StudioCameraPlacement::Project(Scene->PresentedCamera(),Size,Center+FVector(0,.5,0),B,Aspect)))return;
        auto& App=FSlateApplication::Get();const auto P=G.LocalToAbsolute(A),Q=G.LocalToAbsolute(B);
        if(!Test->TestTrue(TEXT("Native application is active before pointer input"),App.IsActive()))return;
        const auto Hit=App.LocateWindowUnderMouse(P,App.GetInteractiveTopLevelWindows(),false,0);
        if(!Test->TestTrue(TEXT("Displayed resize handle is exposed"),Hit.ContainsWidget(Viewport.Get())))return;
        const TSet<FKey> Down={EKeys::LeftMouseButton};LastPointer=Q;
        const auto User=App.GetUser(0);const auto CaptorBefore=User?User->GetPointerCaptor(0):nullptr;
        const auto FocusBefore=App.GetKeyboardFocusedWidget();const int32 FaceBefore=Scene->Model->SelectedDomainFace;
        StudioDomain::FFaceDrag Probe;
        Test->TestTrue(TEXT("Displayed handle supports a physical resize"),StudioDomain::BeginFaceDrag(Scene->Model->Project.Draft.Domain,3,Scene->PresentedCamera(),Size,A,Probe,Aspect));
        App.ProcessMouseButtonDownEvent(GEngine->GameViewport->GetWindow()->GetNativeWindow(),FPointerEvent(0,P,P,Down,EKeys::LeftMouseButton,0,FModifierKeysState()));
        if(!Test->TestTrue(TEXT("Resize handle owns the pointer"),Viewport->HasMouseCapture()))
        {
            const auto CaptorAfter=User?User->GetPointerCaptor(0):nullptr;
            Test->AddInfo(FString::Printf(TEXT("Handle routing: phase=%d face=%d->%d active=%d enabled=%d preview=%d capture=%s/%s -> %s/%s focus=%s/%s size=%s point=%s applied=%s..%s draftY=%s..%s"),
                Phase,FaceBefore,Scene->Model->SelectedDomainFace,App.IsActive(),Viewport->IsEnabled(),Scene->HasDomainPreview(),
                CaptorBefore?*CaptorBefore->GetTypeAsString():TEXT("none"),CaptorBefore?*CaptorBefore->GetTag().ToString():TEXT("none"),
                CaptorAfter?*CaptorAfter->GetTypeAsString():TEXT("none"),CaptorAfter?*CaptorAfter->GetTag().ToString():TEXT("none"),
                FocusBefore?*FocusBefore->GetTypeAsString():TEXT("none"),FocusBefore?*FocusBefore->GetTag().ToString():TEXT("none"),
                *Size.ToString(),*A.ToString(),*Scene->Model->Project.Draft.Domain.Min.ToString(),*Scene->Model->Project.Draft.Domain.Max.ToString(),
                *Text(TEXT("DomainValue2")),*Text(TEXT("DomainValue3"))));
            Capture(TEXT("handle-input-failure.png"));return;
        }
        const FString Before=Text(TEXT("DomainValue3"));
        App.ProcessMouseMoveEvent(FPointerEvent(1,Q,P,TSet<FKey>(),FKey(),0,FModifierKeysState()));
        Test->TestEqual(TEXT("Another pointer cannot move the face"),Text(TEXT("DomainValue3")),Before);
        App.ProcessMouseMoveEvent(FPointerEvent(0,Q,P,Down,FKey(),0,FModifierKeysState()));
        if(bRelease)ReleasePointer();
    }
    void Escape()
    {auto& App=FSlateApplication::Get();App.ProcessKeyDownEvent(FKeyEvent(EKeys::Escape,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Escape,FModifierKeysState(),0,false,0,0));ReleasePointer();}
    void ReleasePointer()
    {FSlateApplication::Get().ProcessMouseButtonUpEvent(FPointerEvent(0,LastPointer,LastPointer,TSet<FKey>(),EKeys::LeftMouseButton,0,FModifierKeysState()));}
    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native domain workspace"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Retain domain evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Work,Case;
    TArray<FGuid> Faces;FStudioCameraState FlowCamera,Observer;FVector2D LastPointer=FVector2D::ZeroVector;
    bool bTooltips=true,bCapturedTooltips=false;int32 Phase=0,Frame=0;uint64 ChangedFrame=0,Revision=0;double Started=0,LastActivation=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioDomainHandlesTest,"Studio.Domain.FaceHandlesDimensionsAndCancellation",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioDomainHandlesTest::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioDomainHandlesCommand(this));return true;}
#endif
