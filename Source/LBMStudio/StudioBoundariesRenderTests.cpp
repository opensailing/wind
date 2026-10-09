#include "StudioScene.h"
#include "StudioAuthoringTestCapture.h"
#include "StudioBoundaries.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioBoundariesCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioBoundariesCommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioBoundariesCommand(){if(bCapturedTooltips&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(FPlatformTime::Seconds()-Started>100){Test->AddError(FString::Printf(TEXT("Boundary workflow timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter-ChangedFrame<4)return false;
        auto& M=*Scene->Model;if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/Boundaries");Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;Previous=M.Workspace;
            Test->TestTrue(TEXT("Save preceding project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=FSlateApplication::Get().GetAllowTooltips();bCapturedTooltips=true;FSlateApplication::Get().SetAllowTooltips(false);
            // Retired native tooltip windows must leave the capture tree before the next frame.
            StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing boundary conditions"));M.Pause();Camera=M.Project.Camera;Frame=M.SelectedFrame;Intent=M.RenderIntentRevision;
            Faces=M.Project.Draft.Domain.Faces;Press(TEXT("Workspace5"));Next();break;
        }
        case 1:
            if(!Scene->HasBoundaryPreview())return false;
            Test->TestTrue(TEXT("Sidebar opens boundary editor"),M.Workspace==EStudioWorkspace::BoundaryConditions);
            Capture(TEXT("unassigned.png"));Open(TEXT("BoundaryType"));Next();break;
        case 2:Press(TEXT("BoundaryType1"));Next();break;
        case 3:Type(TEXT("BoundaryValue1"),TEXT("1.2345678901234567"),false);Press(TEXT("BoundaryApply"));Next();break;
        case 4:
            Test->TestTrue(TEXT("Partial velocity cannot create an assignment"),M.Project.Draft.Boundaries.IsEmpty());Capture(TEXT("partial-vector.png"));
            Press(TEXT("Workspace7"));Next();break;
        case 5:Press(TEXT("Workspace5"));Next();break;
        case 6:
            Test->TestEqual(TEXT("Partial draft retained across route change"),Text(TEXT("BoundaryValue1")),FString(TEXT("1.2345678901234567")));
            Type(TEXT("BoundaryFilter"),TEXT("no matching surface"),false);
            Test->TestEqual(TEXT("Filtering cannot discard the selected physical draft"),Text(TEXT("BoundaryValue1")),FString(TEXT("1.2345678901234567")));
            Test->TestTrue(TEXT("Filtered-out selected target remains reachable"),FindTag(FName(*(TEXT("BoundaryTarget_")+Faces[0].ToString()))).IsValid());
            Test->TestEqual(TEXT("Editor selection is shared with geometry preview"),M.SelectedBoundaryTarget,Faces[0]);
            Type(TEXT("BoundaryFilter"),TEXT(" "),false);
            Type(TEXT("BoundaryValue2"),TEXT("0"),false);Type(TEXT("BoundaryValue3"),TEXT("0"),false);Press(TEXT("BoundaryApply"));Next();break;
        case 7:
            if(!Test->TestEqual(TEXT("Inlet assignment created"),M.Project.Draft.Boundaries.Num(),1))return true;
            if(const auto Input=FindTag(TEXT("BoundaryValue1")))Test->TestTrue(TEXT("Velocity entry uses readable inspector width"),Input->GetCachedGeometry().GetLocalSize().X>=180.f);
            Test->TestEqual(TEXT("Exact inlet velocity saved"),M.Project.Draft.Boundaries[0].Velocity.GetValue().X,1.2345678901234567);
            Test->TestEqual(TEXT("Only one target configured"),StudioBoundaries::Analyze(M.Project.Draft).Configured,1);
            Capture(TEXT("inlet.png"));Type(TEXT("BoundaryValue1"),TEXT("2"),false);
            M.EditCase(TEXT("External inlet edit"),[](auto& Case){Case.Boundaries[0].Velocity=FVector(3,0,0);});Next();break;
        case 8:
            Test->TestEqual(TEXT("Conflicting draft retained"),Text(TEXT("BoundaryValue1")),FString(TEXT("2")));
            if(auto Apply=FindTag(TEXT("BoundaryApply")))Test->TestFalse(TEXT("Conflict disables Apply"),Apply->IsEnabled());
            Capture(TEXT("conflict.png"));Press(TEXT("BoundaryRevert"));Select(2);Next();break;
        case 9:Open(TEXT("BoundaryType"));Next();break;
        case 10:Press(TEXT("BoundaryType6"));Next();break;
        case 11:Press(TEXT("BoundaryApply"));Next();break;
        case 12:
            Test->TestEqual(TEXT("Periodic pair adds both assignments"),M.Project.Draft.Boundaries.Num(),3);
            Capture(TEXT("periodic.png"));Open(TEXT("BoundaryType"));Next();break;
        case 13:Press(TEXT("BoundaryType3"));Next();break;
        case 14:
            if(auto Apply=FindTag(TEXT("BoundaryApply")))Test->TestFalse(TEXT("Ordinary Apply cannot unpair"),Apply->IsEnabled());
            Capture(TEXT("unpair.png"));Press(TEXT("BoundaryUnpairApply"));Next();break;
        case 15:
            Test->TestEqual(TEXT("Explicit unpair removes counterpart"),M.Project.Draft.Boundaries.Num(),2);Press(TEXT("BoundaryUndo"));Next();break;
        case 16:
            Test->TestEqual(TEXT("One undo restores pair"),M.Project.Draft.Boundaries.Num(),3);Select(4);Next();break;
        case 17:Open(TEXT("BoundaryType"));Next();break;
        case 18:Press(TEXT("BoundaryType2"));Next();break;
        case 19:Type(TEXT("BoundaryValue4"),TEXT("NaN"),false);SaveShortcut();Next();break;
        case 20:
            Test->TestTrue(TEXT("Save protects unapplied boundary values"),M.Notice.Contains(TEXT("Apply or revert")));
            Test->TestEqual(TEXT("Invalid pressure stays editable"),Text(TEXT("BoundaryValue4")),FString(TEXT("NaN")));
            Press(TEXT("BoundaryRevert"));Select(0);Next();break;
        case 21:
            Test->TestFalse(TEXT("Reverting condition resolves the footer save warning"),M.Notice.Contains(TEXT("Apply or revert")));
            Type(TEXT("BoundaryValue1"),TEXT("4"),false);
            M.RemoveBoundary(Faces[0]);Next();break;
        case 22:
            Test->TestEqual(TEXT("Removed assignment retains conflicting draft"),Text(TEXT("BoundaryValue1")),FString(TEXT("4")));
            Press(TEXT("BoundaryRevert"));Next();break;
        case 23:
            Test->TestFalse(TEXT("Revert after deletion keeps target unassigned"),M.Project.Draft.Boundaries.ContainsByPredicate([&](const auto& B){return B.TargetId==Faces[0];}));
            Test->TestEqual(TEXT("Revert loads current target name"),Text(TEXT("BoundaryValue0")),M.Project.Draft.Domain.FaceNames[0]);
            Test->TestTrue(TEXT("Authoring retains saved flow camera"),StudioView::CameraEquals(M.Project.Camera,Camera));
            Test->TestEqual(TEXT("Authoring retains source frame"),M.SelectedFrame,Frame);Test->TestEqual(TEXT("Authoring does not request CFD"),M.RenderIntentRevision,Intent);
            Test->TestTrue(TEXT("Save authored assignments"),M.SaveProject(Work/TEXT("boundaries.lbms")));Saved=StudioCaseIO::Serialize(M.Project.Draft);
            M.NewProject(TEXT("Other case"));Next();break;
        case 24:
            Test->TestTrue(TEXT("New case has no stale assignments"),M.Project.Draft.Boundaries.IsEmpty());
            Test->TestTrue(TEXT("Reopen boundary case"),M.RequestProjectOpen(Work/TEXT("boundaries.lbms")));Next();break;
        case 25:
            Test->TestEqual(TEXT("Exact assignments reopen"),StudioCaseIO::Serialize(M.Project.Draft),Saved);Press(TEXT("Workspace5"));Next();break;
        case 26:
            Capture(TEXT("reopened.png"));Test->TestTrue(TEXT("Restore preceding project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 27:M.Navigate(Previous);return true;
        }
        return false;
    }
private:
    void Select(int32 Face){Press(FName(*(TEXT("BoundaryTarget_")+Faces[Face].ToString())));}
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
        Test->AddError(TEXT("Missing boundary control: ")+Tag.ToString());return {};
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

    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native boundary workspace"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Retain boundary evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Work,Saved;
    TArray<FGuid> Faces;FStudioCameraState Camera;EStudioWorkspace Previous=EStudioWorkspace::Solve;
    bool bTooltips=true,bCapturedTooltips=false;int32 Phase=0,Frame=0;uint64 ChangedFrame=0,Intent=0;double Started=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioBoundariesRender,"Studio.Boundaries.ControlsDraftsPairsAndReopen",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioBoundariesRender::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioBoundariesCommand(this));return true;}
#endif
