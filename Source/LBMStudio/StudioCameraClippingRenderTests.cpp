#include "StudioScene.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableText.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
// Exercises the shipped controls and GPU with the bundled published SU2 data.
// The test changes only camera state; no scientific field is manufactured.
class FStudioCameraClippingCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioCameraClippingCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()||Now-Started>90)
        {
            FSlateApplication::Get().DismissAllMenus();
            if(!Test->HasAnyErrors())Test->AddError(FString::Printf(TEXT("Camera clipping timed out in phase %d"),Phase));
            return true;
        }
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        if(Phase>0&&!Ready())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/CameraClipping");Work=Root/FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;
            OriginalWorkspace=M.Workspace;OriginalSidebar=M.bSidebarCollapsed;
            Test->TestTrue(TEXT("Retain preceding project"),StudioProjectIO::Save(Work/TEXT("original.lbms"),M.SnapshotProject(),Error));
            M.NewProject(TEXT("Airfoil · camera clipping"));M.Navigate(EStudioWorkspace::Solve);M.bSidebarCollapsed=false;
            Next(1);break;
        }
        case 1:
            Scene->FitCamera();OpenViewportSettings();ScrollToControls();Next(2);break;
        case 2:
            Dataset=M.Project.Dataset;Case=StudioCaseIO::Serialize(M.Project.Draft);Frame=M.SelectedFrame;Playback=M.PlaybackFrame;
            FieldRevision=M.Revision;Bounds=M.Solver->Descriptor().DisplayBounds;
            Test->TestFalse(TEXT("Clipping starts disabled"),Scene->SavedCameraState().bDepthClipping);
            if(const auto Near=FindTag(TEXT("CameraNearClip")))Test->TestFalse(TEXT("Near input disabled with automatic planes"),Near->IsEnabled());
            if(const auto Far=FindTag(TEXT("CameraFarClip")))Test->TestFalse(TEXT("Far input disabled with automatic planes"),Far->IsEnabled());
            Capture(TEXT("automatic.png"));Press(TEXT("CameraDepthClipping"),EKeys::SpaceBar);
            BeforeTyping=Scene->SavedCameraState();Type(TEXT("CameraNearClip"),TEXT("0.02"),false);Next(3);break;
        case 3:
            Test->TestTrue(TEXT("Typing alone preserves camera"),StudioView::CameraEquals(Scene->SavedCameraState(),BeforeTyping));
            Press(TEXT("CameraNearClip"));Next(4);break;
        case 4:
            Test->TestEqual(TEXT("Enter commits near distance"),Scene->SavedCameraState().NearClipMeters,.02);
            VerifyVisibleNumber(TEXT("CameraNearClip"),.02);
            Type(TEXT("CameraNearClip"),TEXT("0.025"));
            Test->TestEqual(TEXT("Second edit works without leaving the focused field"),Scene->SavedCameraState().NearClipMeters,.025);
            Type(TEXT("CameraNearClip"),TEXT("0.02"));
            Test->TestEqual(TEXT("Each committed edit starts a fresh revision guard"),Scene->SavedCameraState().NearClipMeters,.02);
            BeforeInvalid=Scene->SavedCameraState();Type(TEXT("CameraFarClip"),TEXT("0.01"));ScrollToControls();Next(5);break;
        case 5:
        {
            Test->TestTrue(TEXT("Reversed range preserves exact view"),StudioView::CameraEquals(Scene->SavedCameraState(),BeforeInvalid));
            if(const auto Feedback=FindTag(TEXT("CameraClippingFeedback")))
            {
                FeedbackWidget=Feedback;
                Test->TestTrue(TEXT("Invalid range provides recovery instruction"),StaticCastSharedPtr<STextBlock>(Feedback)->GetText().ToString().Contains(TEXT("increase far distance")));
                const auto Message=StaticCastSharedPtr<STextBlock>(Feedback)->GetText().ToString();
                Test->TestTrue(TEXT("Error identifies rejected values and retained state"),Message.Contains(TEXT("Rejected near 0.02 m / far 0.01 m"))&&Message.Contains(TEXT("Previous range kept")));
            }
            Capture(TEXT("invalid-range.png"));
            auto Changed=BeforeInvalid;Changed.FieldOfView+=1.;
            Scene->RestoreCamera(Changed,TEXT("Restore valid inspection camera"));Next(26);break;
        }
        case 6:
            Test->TestEqual(TEXT("Far control accepts valid recovery"),Scene->SavedCameraState().FarClipMeters,1000.);
            Capture(TEXT("perspective.png"));BeforeInvalid=Scene->SavedCameraState();
            Type(TEXT("CameraNearClip"),TEXT("nan"));Next(27);break;
        case 7:
            VerifyVisibleNumber(TEXT("CameraNearClip"),Scene->SavedCameraState().NearClipMeters);
            VerifyVisibleNumber(TEXT("CameraFarClip"),Scene->SavedCameraState().FarClipMeters);
            Broad=Scene->SavedCameraState();BroadPixels=ReadPixels();VerifyPopulated(BroadPixels);
            DepthRange(Broad,NearDepth,FarDepth);
            Test->TestTrue(TEXT("Fit places domain in front of camera"),NearDepth>.02);
            Test->TestTrue(TEXT("Set far plane before whole domain"),Scene->SetDepthClipping(true,.001,NearDepth*.4));Next(8);break;
        case 8:
            VerifyEmpty(ReadPixels(),TEXT("Far plane clips all scene geometry"));
            Test->TestTrue(TEXT("Undo far clipping"),M.UndoView());Next(9);break;
        case 9:
            Test->TestTrue(TEXT("Undo restores exact broad camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Broad));
            VerifyPixels(BroadPixels,TEXT("Undo restores broad projection pixels"));
            Test->TestTrue(TEXT("Set near plane after whole domain"),Scene->SetDepthClipping(true,FarDepth+Bounds.GetSize().Size(),FarDepth+3.*Bounds.GetSize().Size()));Next(10);break;
        case 10:
            VerifyEmpty(ReadPixels(),TEXT("Near plane clips all scene geometry"));
            Test->TestTrue(TEXT("Undo near clipping"),M.UndoView());Next(11);break;
        case 11:
            VerifyPixels(BroadPixels,TEXT("Near undo restores visible scene"));
            Test->TestTrue(TEXT("Clip through source domain"),Scene->SetDepthClipping(true,Broad.NearClipMeters,(NearDepth+FarDepth)*.5));Next(12);break;
        case 12:
            VerifyVisibleNumber(TEXT("CameraFarClip"),Scene->SavedCameraState().FarClipMeters);
            PartialPixels=ReadPixels();VerifyPopulated(PartialPixels);
            Test->TestTrue(TEXT("Partial clip changes rendered pixels"),ChangedPixels(BroadPixels,PartialPixels)>100);
            Test->TestTrue(TEXT("Undo partial clipping"),M.UndoView());Next(13);break;
        case 13:
            VerifyVisibleNumber(TEXT("CameraFarClip"),Scene->SavedCameraState().FarClipMeters);
            VerifyPixels(BroadPixels,TEXT("Partial undo restores exact rendering"));
            Test->TestTrue(TEXT("Redo partial clipping"),M.RedoView());Next(14);break;
        case 14:
            VerifyVisibleNumber(TEXT("CameraFarClip"),Scene->SavedCameraState().FarClipMeters);
            VerifyPixels(PartialPixels,TEXT("Redo restores clipped rendering"));VerifyIsolation();
            if(!Broad.bOrthographic)
            {
                Broad.bOrthographic=true;Scene->RestoreCamera(Broad,TEXT("Orthographic depth test"));Scene->FitCamera();Next(7);
            }
            else
            {
                Saved=Scene->SavedCameraState();Capture(TEXT("orthographic.png"));
                Test->TestTrue(TEXT("Save clipped named view"),M.AddCamera(TEXT("Clipped wake"),Saved));
                Bookmark=M.Project.Cameras.Last().Id;
                Test->TestTrue(TEXT("Save camera clipping project"),M.SaveProject(Work/TEXT("clipped.lbms")));
                OriginalSize=Scene->PresentedViewportSize();M.bSidebarCollapsed=true;Next(15);
            }
            break;
        case 15:
            Test->TestTrue(TEXT("Sidebar resizing changes actual render target"),Scene->PresentedViewportSize()!=OriginalSize);
            Test->TestTrue(TEXT("Resizing retains exact camera and planes"),StudioView::CameraEquals(Scene->SavedCameraState(),Saved));
            VerifyPopulated(ReadPixels());M.bSidebarCollapsed=false;Next(16);break;
        case 16:
            Test->TestEqual(TEXT("Restoring sidebar restores viewport dimensions"),Scene->PresentedViewportSize(),OriginalSize);
            VerifyPixels(PartialPixels,TEXT("Aspect restoration rebuilds original projection"));
            Press(TEXT("CameraDepthClipping"),EKeys::SpaceBar);Next(17);break;
        case 17:
            Test->TestFalse(TEXT("Visible checkbox disables custom projection"),Scene->SavedCameraState().bDepthClipping);
            Test->TestTrue(TEXT("Named camera restores depth settings"),M.RestoreSavedCamera(Bookmark));Next(18);break;
        case 18:
            VerifyVisibleNumber(TEXT("CameraNearClip"),Saved.NearClipMeters);
            VerifyVisibleNumber(TEXT("CameraFarClip"),Saved.FarClipMeters);
            VerifyPixels(PartialPixels,TEXT("Named camera restores clipped pixels"));
            Test->TestTrue(TEXT("Named camera retains exact clip settings"),StudioView::CameraEquals(Scene->SavedCameraState(),Saved));
            // Leave an edit pending across a document replacement. Focus loss or
            // Enter must not apply the preceding document's typed value.
            Type(TEXT("CameraFarClip"),TEXT("999"),false);
            Test->TestTrue(TEXT("Reopen persisted clipping"),M.RequestProjectOpen(Work/TEXT("clipped.lbms")));Next(19);break;
        case 19:
            Press(TEXT("CameraFarClip"));Next(20);break;
        case 20:
            VerifyVisibleNumber(TEXT("CameraFarClip"),Saved.FarClipMeters);
            Test->TestTrue(TEXT("Reopen and stale Enter retain saved clipping"),StudioView::CameraEquals(Scene->SavedCameraState(),Saved));
            VerifyPixels(PartialPixels,TEXT("Project reopen restores actual clipped pixels"));
            VerifyIsolation(false);ScrollToControls();Next(21);break;
        case 21:
        {
            Capture(TEXT("reopened.png"));
            auto Inside=Saved;const auto Size=Bounds.GetSize();
            Inside.bOrthographic=false;Inside.bFreeCamera=true;
            Inside.Position=Bounds.GetCenter()+FVector(-.2*Size.X,-.25*Size.Y,.1*Size.Z);
            Inside.Focus=Bounds.GetCenter()+FVector(.25*Size.X,.3*Size.Y,0);
            Inside.Orientation=(Inside.Focus-Inside.Position).Rotation().Quaternion();
            Inside.OrbitDistance=(Inside.Focus-Inside.Position).Size();
            Inside.NearClipMeters=FMath::Max(1.e-6,Size.Size()*1.e-4);Inside.FarClipMeters=Size.Size()*2.;
            Test->TestTrue(TEXT("Inspection camera placed inside original bounds"),Bounds.IsInside(Inside.Position));
            Scene->RestoreCamera(Inside,TEXT("Inside-domain clipping"));Next(22);break;
        }
        case 22:
            VerifyVisibleNumber(TEXT("CameraNearClip"),Scene->SavedCameraState().NearClipMeters);
            VerifyVisibleNumber(TEXT("CameraFarClip"),Scene->SavedCameraState().FarClipMeters);
            VerifyPopulated(ReadPixels());VerifyIsolation(false);Capture(TEXT("inside-domain.png"));
            Scene->RestoreCamera(Saved,TEXT("Return to saved clipping"));Next(23);break;
        case 23:
            VerifyPixels(PartialPixels,TEXT("Inside/outside transition restores original render"));
            IdleCaptures=Scene->GetCaptureCount();IdleStarted=Now;Next(24);break;
        case 24:
            if(Now-IdleStarted<2)return false;
            Test->TestEqual(TEXT("Unchanged clipped view submits no new 3D captures"),Scene->GetCaptureCount(),IdleCaptures);
            M.bSidebarCollapsed=OriginalSidebar;
            Test->TestTrue(TEXT("Restore preceding project after acceptance"),M.RequestProjectOpen(Work/TEXT("original.lbms")));Next(25);break;
        case 25:
            M.Navigate(OriginalWorkspace);return true;
        case 26:
            Test->TestTrue(TEXT("Another valid camera clears stale clipping feedback"),
                FeedbackWidget.IsValid()&&!FeedbackWidget.Pin()->GetVisibility().IsVisible());
            Scene->RestoreCamera(BeforeInvalid,TEXT("Return to tested camera"));
            Type(TEXT("CameraFarClip"),TEXT("1000"));ScrollToControls();Next(6);break;
        case 27:
            Test->TestTrue(TEXT("Nonfinite text retains exact camera"),StudioView::CameraEquals(Scene->SavedCameraState(),BeforeInvalid));
            VerifyVisibleNumber(TEXT("CameraNearClip"),BeforeInvalid.NearClipMeters);
            if(const auto Feedback=FindTag(TEXT("CameraClippingFeedback")))
                Test->TestTrue(TEXT("Nonfinite input explains recovery and retained range"),
                    StaticCastSharedPtr<STextBlock>(Feedback)->GetText().ToString().Contains(TEXT("Enter a finite distance in meters. Previous range kept.")));
            Type(TEXT("CameraNearClip"),TEXT("0.02"));
            // Keep Far focused while undo/redo and camera restores replace it.
            // Enter without an edit must not round the stored camera value.
            Press(TEXT("CameraFarClip"));Next(7);break;
        }
        return false;
    }
private:
    TSharedPtr<SEditableText> FindEditor(const TSharedRef<SWidget>& W)
    {
        if(W->GetType()==TEXT("SEditableText"))return StaticCastSharedRef<SEditableText>(W);
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)
            if(auto Editor=FindEditor(Children->GetChildAt(I)))return Editor;
        return {};
    }
    void VerifyVisibleNumber(FName Tag,double Expected)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;
        const auto Editor=FindEditor(Widget.ToSharedRef());
        if(!Test->TestTrue(TEXT("Numeric editing surface exists"),Editor.IsValid()))return;
        // GetText reports the bound attribute, not necessarily the focused
        // buffer. Selected text reads the actual visible editable layout.
        Editor->SelectAllText();
        const FString Visible=Editor->GetSelectedText().ToString();double Actual=0;
        Test->TestTrue(*FString::Printf(TEXT("Visible %s value '%s' matches current camera"),*Tag.ToString(),*Visible),
            StudioColor::ParseNumber(Visible,Actual)&&FMath::Abs(Actual-Expected)<=.51e-6);
    }
    TWeakPtr<SWidget> FeedbackWidget;
    void Next(int32 Value){Phase=Value;ChangedFrame=GFrameCounter;}
    bool Ready() const
    {
        const auto& M=*Scene->Model;const auto* Target=Scene->GetRenderTarget();
        auto Rendered=Scene->SavedCameraState();
        // UE stores FOV/orthographic width in float. Document values remain
        // exact; compare the rendered pose at that declared storage precision.
        Rendered.FieldOfView=float(Rendered.FieldOfView);
        Rendered.OrthoWidth=double(float(Rendered.OrthoWidth*100.))/100.;
        return !M.IsProjectOpenPending()&&!M.IsRecordingLoadPending()&&Scene->HasCurrentFrame()&&Target&&
            GFrameCounter-ChangedFrame>=3&&Scene->PresentedViewportSize()==FIntPoint(Target->SizeX,Target->SizeY)&&
            StudioView::CameraEquals(Scene->PresentedCamera(),Rendered);
    }
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if(W->GetTag()==Tag)return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)
            if(auto Result=Find(Children->GetChildAt(I),Tag))return Result;
        return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Result=Find(Window,Tag))return Result;
        Test->AddError(TEXT("Missing camera clipping control: ")+Tag.ToString());return {};
    }
    TSharedPtr<SWidget> FirstButton(const TSharedRef<SWidget>& W)
    {
        if(W->GetType()==TEXT("SButton"))return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)
            if(auto Button=FirstButton(Children->GetChildAt(I)))return Button;
        return {};
    }
    void OpenViewportSettings()
    {
        if(const auto Menu=FindTag(TEXT("ViewportSettings")))if(const auto Button=FirstButton(Menu.ToSharedRef()))
        {
            auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Button,EFocusCause::Navigation);
            App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
            App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
        }
    }
    bool Scroll(const TSharedRef<SWidget>& W,const TSharedPtr<SWidget>& Target)
    {
        if(W->GetType()==TEXT("SScrollBox")&&Find(W,TEXT("CameraClippingControls")))
        {StaticCastSharedRef<SScrollBox>(W)->ScrollDescendantIntoView(Target,false,EDescendantScrollDestination::Center);return true;}
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(Scroll(Children->GetChildAt(I),Target))return true;
        return false;
    }
    void ScrollToControls()
    {
        if(const auto Controls=FindTag(TEXT("CameraClippingControls")))
        {
            TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
            bool Scrolled=false;for(const auto& Window:Windows)Scrolled=Scroll(Window,Controls)||Scrolled;
            Test->TestTrue(TEXT("Clipping inspector scrolls into view"),Scrolled);
        }
    }
    void Press(FName Tag,FKey Key=EKeys::Enter)
    {
        if(const auto Widget=FindTag(Tag))
        {
            auto& App=FSlateApplication::Get();Test->TestTrue(TEXT("Camera control enabled"),Widget->IsEnabled());
            // Enter must remain in the numeric editor. Refocusing its parent
            // commits once on focus loss, then again on Enter after rebinding.
            if(!Widget->HasKeyboardFocus()&&!Widget->HasFocusedDescendants())
                App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
            App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
            App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
        }
    }
    void Type(FName Tag,const FString& Value,bool bCommit=true)
    {
        if(const auto Widget=FindTag(Tag))
        {
            auto& App=FSlateApplication::Get();
            if(!Widget->HasKeyboardFocus()&&!Widget->HasFocusedDescendants())
                App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
            // Select all through Slate so this exercises numeric edit ownership,
            // not SetText bypasses that skip the actual OnValueChanged path.
            // Slate maps the platform Command shortcut to its Control chord on
            // Mac (FGenericCommands::SelectAll). No extra Alt modifier.
            const FModifierKeysState SelectAll(false,false,true,false,false,false,false,false,false);
            Test->TestTrue(TEXT("Numeric Select All is handled"),App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,SelectAll,0,false,0,0)));
            App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,SelectAll,0,false,0,0));
            for(TCHAR Ch:Value)App.ProcessKeyCharEvent(FCharacterEvent(Ch,FModifierKeysState(),0,false));
            if(bCommit)Press(Tag);
        }
    }
    TArray<FColor> ReadPixels()
    {
        TArray<FColor> Pixels;
        Test->TestTrue(TEXT("Read current camera render target"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels));
        Test->TestEqual(TEXT("Readback covers viewport"),Pixels.Num(),Scene->GetRenderTarget()->SizeX*Scene->GetRenderTarget()->SizeY);
        return Pixels;
    }
    int32 ChangedPixels(const TArray<FColor>& A,const TArray<FColor>& B)
    {
        if(A.Num()!=B.Num())return MAX_int32;int32 Changed=0;
        for(int32 I=0;I<A.Num();++I)if(A[I].R!=B[I].R||A[I].G!=B[I].G||A[I].B!=B[I].B)++Changed;return Changed;
    }
    int32 NonBackgroundPixels(const TArray<FColor>& Pixels)
    {
        if(Pixels.IsEmpty())return 0;const auto Background=Pixels[0];int32 Count=0;
        for(const auto P:Pixels)if(FMath::Abs(int(P.R)-Background.R)+FMath::Abs(int(P.G)-Background.G)+FMath::Abs(int(P.B)-Background.B)>6)++Count;
        return Count;
    }
    void VerifyPopulated(const TArray<FColor>& Pixels)
    {Test->TestTrue(TEXT("Recorded flow is visible in current projection"),NonBackgroundPixels(Pixels)>1000);}
    void VerifyEmpty(const TArray<FColor>& Pixels,const TCHAR* Label)
    {Test->TestTrue(Label,!Pixels.IsEmpty()&&NonBackgroundPixels(Pixels)==0);}
    void VerifyPixels(const TArray<FColor>& Expected,const TCHAR* Label)
    {
        const auto Actual=ReadPixels();const int32 Changed=ChangedPixels(Expected,Actual);
        if(Changed)
        {
            const auto Size=Scene->PresentedViewportSize();
            Test->AddInfo(FString::Printf(TEXT("Phase %d: %d changed pixels; populated expected=%d actual=%d; capture=%llu; viewport=%dx%d"),
                Phase,Changed,NonBackgroundPixels(Expected),NonBackgroundPixels(Actual),Scene->GetCaptureCount(),Size.X,Size.Y));
            const auto Save=[&](const TArray<FColor>& Pixels,const TCHAR* Suffix)
            {
                if(Pixels.Num()!=Size.X*Size.Y)return;
                TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
                FFileHelper::SaveArrayToFile(PNG,*(Root/FString::Printf(TEXT("failure-phase-%d-%s.png"),Phase,Suffix)));
            };
            Save(Expected,TEXT("expected"));Save(Actual,TEXT("actual"));
        }
        Test->TestEqual(Label,Changed,0);
    }
    void DepthRange(const FStudioCameraState& Camera,double& Minimum,double& Maximum)
    {
        Minimum=MAX_dbl;Maximum=-MAX_dbl;
        for(int32 I=0;I<8;++I)
        {
            const FVector P(I&1?Bounds.Max.X:Bounds.Min.X,I&2?Bounds.Max.Y:Bounds.Min.Y,I&4?Bounds.Max.Z:Bounds.Min.Z);
            const double Depth=FVector::DotProduct(P-Camera.Position,Camera.Orientation.GetForwardVector());
            Minimum=FMath::Min(Minimum,Depth);Maximum=FMath::Max(Maximum,Depth);
        }
    }
    void VerifyIsolation(bool bSameRevision=true)
    {
        const auto& M=*Scene->Model;
        Test->TestEqual(TEXT("Camera depth does not change source"),M.Project.Dataset,Dataset);
        Test->TestEqual(TEXT("Camera depth retains selected original frame"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Camera depth retains playback cursor"),M.PlaybackFrame,Playback);
        Test->TestEqual(TEXT("Camera depth leaves authoring case intact"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        if(bSameRevision)Test->TestEqual(TEXT("Camera edits do not regenerate flow geometry"),M.Revision,FieldRevision);
    }
    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();
        const auto Controls=FindTag(TEXT("CameraClippingControls"));const auto Window=GEngine->GameViewport->GetWindow();
        if(!Controls||!Window)return;
        const auto G=Controls->GetCachedGeometry(),W=Window->GetCachedGeometry();
        const auto Top=W.AbsoluteToLocal(G.LocalToAbsolute(FVector2D::ZeroVector));
        const auto Bottom=W.AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()));
        Test->TestTrue(TEXT("Entire clipping group visible in native capture"),Top.Y>=0&&Bottom.Y<=W.GetLocalSize().Y&&Top.X>=0&&Bottom.X<=W.GetLocalSize().X);
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture complete native window"),FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(),Pixels,Size))||Pixels.IsEmpty())return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save camera clipping evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;
    FStudioCameraState BeforeTyping,BeforeInvalid,Broad,Saved;TArray<FColor> BroadPixels,PartialPixels;
    FBox Bounds;FString Root,Work,Dataset,Case;FGuid Bookmark;FIntPoint OriginalSize;
    EStudioWorkspace OriginalWorkspace=EStudioWorkspace::Solve;bool OriginalSidebar=false;
    int32 Phase=0,Frame=0,Playback=0,FieldRevision=0;uint64 ChangedFrame=0,IdleCaptures=0;
    double Started=0,IdleStarted=0,NearDepth=0,FarDepth=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCameraClippingRender,"Studio.CameraClipping.ControlsPixelsAndPersistence",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioCameraClippingRender::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioCameraClippingCommand(this));return true;}
#endif
