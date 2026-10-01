#include "StudioScene.h"
#include "StudioAuthoringTestCapture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SWindow.h"
#include "ImageUtils.h"

#include "StudioAutomationForeground.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioFlowUICommand final:public IAutomationLatentCommand
{
public:
    explicit FStudioFlowUICommand(FAutomationTestBase* In):Test(In){}
    ~FStudioFlowUICommand(){if(bStarted&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>90){Test->AddError(FString::Printf(TEXT("Flow conditions timed out at %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+5)return false;
        if(Foreground.WasInterrupted()){Test->AddError(Foreground.Describe(Phase));return true;}
        auto& App=FSlateApplication::Get();if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}return false;}
        auto& M=*Scene->Model;if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/FlowConditionsUI");Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;if(!Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error)))return true;
            bTooltips=App.GetAllowTooltips();bStarted=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();Foreground.Begin();
            M.NewProject(TEXT("Wing · Flow conditions"));M.Pause();M.Navigate(EStudioWorkspace::Solve);M.InspectorTab=0;M.bViewportExpanded=false;
            Test->TestTrue(TEXT("Author known calculator fixture"),M.EditCase(TEXT("Fluid fixture"),[](auto& C)
            {FStudioMaterial F;F.Name=TEXT("Reference fluid");F.KinematicViscosity=.000015;C.Materials.Add(F);C.Domain.FluidMaterialId=F.Id;}));
            Next();break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Camera=Scene->SavedCameraState();Frame=M.SelectedFrame;RenderRevision=M.RenderIntentRevision;
            Capture(TEXT("defaults.png"));Press(TEXT("FlowAdvanced"));Next();break;
        case 2:
            Type(TEXT("FlowValue0"),TEXT("3"));Type(TEXT("FlowValue1"),TEXT("4"));Type(TEXT("FlowValue2"),TEXT("0"));
            Type(TEXT("FlowValue3"),TEXT("-12.345678901234567"));Type(TEXT("FlowValue4"),TEXT("0.6"));Type(TEXT("FlowValue5"),TEXT("1.225"));
            Next();break;
        case 3:
            Test->TestFalse(TEXT("Typing does not edit case"),M.Project.Draft.Setup.InletVelocity.IsSet());
            Capture(TEXT("draft.png"));Press(TEXT("FlowCalculate"));Next();break;
        case 4:
            Test->TestTrue(TEXT("Known calculated target"),FMath::IsNearlyEqual(FCString::Atod(*Text(TEXT("FlowValue6"))),200000.,1.e-8));
            Type(TEXT("FlowValue6"),TEXT("400000"));Press(TEXT("FlowTargetSpeed"));Next();break;
        case 5:
            Test->TestEqual(TEXT("Direction X scaled"),Text(TEXT("FlowValue0")),FString(TEXT("6")));
            Test->TestEqual(TEXT("Direction Y scaled"),Text(TEXT("FlowValue1")),FString(TEXT("8")));
            Capture(TEXT("calculated.png"));Press(TEXT("FlowUnits0"));Next();break;
        case 6:Press(TEXT("FlowUnit0_1"));Next();break;
        case 7:
            Test->TestTrue(TEXT("Velocity unit converts all components"),FMath::IsNearlyEqual(FCString::Atod(*Text(TEXT("FlowValue0"))),21.6,1.e-12)&&FMath::IsNearlyEqual(FCString::Atod(*Text(TEXT("FlowValue1"))),28.8,1.e-12));
            Capture(TEXT("units.png"));Press(TEXT("InspectorTab3"));SaveShortcut();Next();break;
        case 8:
            Test->TestTrue(TEXT("Save routes unresolved draft to Setup"),M.Workspace==EStudioWorkspace::Solve&&M.InspectorTab==0&&M.Notice.Contains(TEXT("Apply or revert flow conditions")));
            Capture(TEXT("save-guard.png"));Press(TEXT("FlowMaterials"));Next();break;
        case 9:
            Test->TestTrue(TEXT("Existing Materials owner reached"),M.Workspace==EStudioWorkspace::Materials);
            Press(TEXT("Workspace7"));Next();break;
        case 10:
            Test->TestTrue(TEXT("Draft retained across sidebar navigation"),FMath::IsNearlyEqual(FCString::Atod(*Text(TEXT("FlowValue1"))),28.8,1.e-12));
            Type(TEXT("FlowValue4"),TEXT("-1"));Press(TEXT("FlowApply"));Next();break;
        case 11:
            Test->TestFalse(TEXT("Invalid apply leaves case untouched"),M.Project.Draft.Setup.InletVelocity.IsSet());
            Test->TestEqual(TEXT("Invalid draft preserved"),Text(TEXT("FlowValue4")),FString(TEXT("-1")));
            Capture(TEXT("invalid-length.png"));Type(TEXT("FlowValue4"),TEXT("0.6"));Press(TEXT("FlowApply"));Next();break;
        case 12:
            Test->TestTrue(TEXT("Apply original SI velocity"),M.Project.Draft.Setup.InletVelocity.Get(FVector::ZeroVector).Equals(FVector(6,8,0),1.e-12));
            Test->TestTrue(TEXT("Exact signed pressure"),M.Project.Draft.Setup.OutletPressure.Get(0)==-12.345678901234567);
            Test->TestFalse(TEXT("Own save warning cleared"),M.Notice.Contains(TEXT("Apply or revert flow conditions")));
            VerifyIsolation();Capture(TEXT("applied.png"));Test->TestTrue(TEXT("Undo case"),M.UndoCase());Next();break;
        case 13:
            Test->TestFalse(TEXT("Undo restores missing inlet"),M.Project.Draft.Setup.InletVelocity.IsSet());
            Test->TestEqual(TEXT("Undo refreshes text"),Text(TEXT("FlowValue0")),FString());M.RedoCase();Next();break;
        case 14:
            Type(TEXT("FlowValue6"),TEXT("99"));M.EditCase(TEXT("External viscosity"),[](auto& C){C.Materials[0].KinematicViscosity=.00002;});Next();break;
        case 15:
            Test->TestFalse(TEXT("Viscosity conflict disables Apply"),FindTag(TEXT("FlowApply"))->IsEnabled());
            Test->TestEqual(TEXT("Conflict retains typed target"),Text(TEXT("FlowValue6")),FString(TEXT("99")));
            Capture(TEXT("conflict.png"));Press(TEXT("FlowRevert"));Next();break;
        case 16:
            Test->TestTrue(TEXT("Select control harness"),M.SetControlHarness(true));Type(TEXT("FlowValue6"),TEXT("300000"));Press(TEXT("RunControl"));Next();break;
        case 17:
            Test->TestFalse(TEXT("Submit blocked for unapplied flow"),M.HasActiveJob());Press(TEXT("FlowApply"));Next();break;
        case 18:Press(TEXT("RunControl"));Next();break;
        case 19:
            if(M.Job().State()!=EStudioJobState::Running)return false;
            Frozen=StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration());Type(TEXT("FlowValue6"),TEXT("200000"));Press(TEXT("FlowTargetSpeed"));Press(TEXT("FlowApply"));Next();break;
        case 20:
            Test->TestEqual(TEXT("Active run retains submitted flow"),StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration()),Frozen);
            VerifyIsolation();Capture(TEXT("active-run.png"));M.Control(EStudioJobCommand::Stop);Next();break;
        case 21:
            if(M.HasActiveJob())return false;
            Saved=StudioCaseIO::Serialize(M.Project.Draft);Test->TestTrue(TEXT("Save case"),M.SaveProject(Work/TEXT("flow.lbms")));
            M.NewProject(TEXT("Before reopen"));M.RequestProjectOpen(Work/TEXT("flow.lbms"));Next();break;
        case 22:
            Test->TestEqual(TEXT("Exact case reopened"),StudioCaseIO::Serialize(M.Project.Draft),Saved);M.Navigate(EStudioWorkspace::Solve);M.InspectorTab=0;Next();break;
        case 23:
            Capture(TEXT("reopened.png"));M.RequestProjectOpen(Work/TEXT("prior.lbms"));Next();break;
        case 24:return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag)
    {W->UpdateAllAttributes();if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=Find(C->GetChildAt(I),Tag))return F;return {};}
    TSharedPtr<SWidget> FindWindow(const TSharedRef<SWindow>& W,FName Tag)
    {if(auto Found=Find(W,Tag))return Found;for(const auto& Child:W->GetChildWindows())if(auto Found=FindWindow(Child,Tag))return Found;return {};}
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        for(const auto& W:FSlateApplication::Get().GetInteractiveTopLevelWindows())if(auto Found=FindWindow(W,Tag))return Found;
        Test->AddError(TEXT("Missing flow control: ")+Tag.ToString());return {};
    }
    TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& W)
    {if(!W->GetVisibility().IsVisible()||!W->IsEnabled())return {};if(W->SupportsKeyboardFocus())return W;auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=Focusable(C->GetChildAt(I)))return F;return {};}
    void Press(FName Tag,FKey Key=EKeys::Enter)
    {
        const auto W=FindTag(Tag);if(!W)return;const auto Target=Focusable(W.ToSharedRef());
        if(!Test->TestTrue(TEXT("Focusable flow control: ")+Tag.ToString(),Target.IsValid()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Target,EFocusCause::Navigation);
        if(!Test->TestTrue(TEXT("Flow keyboard focus: ")+Tag.ToString(),Target->HasKeyboardFocus()||Target->HasFocusedDescendants()))return;
        App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
    }
    FString Text(FName Tag)
    {const auto Widget=FindTag(Tag);return Widget?StaticCastSharedPtr<SEditableTextBox>(Widget)->GetText().ToString():FString();}
    void Type(FName Tag,const FString& Value)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        const FModifierKeysState All(false,false,true,false,false,false,false,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,All,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,All,0,false,0,0));
        if(Value.IsEmpty())
        {App.ProcessKeyDownEvent(FKeyEvent(EKeys::BackSpace,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::BackSpace,FModifierKeysState(),0,false,0,0));}
        else for(TCHAR Character:Value)App.ProcessKeyCharEvent(FCharacterEvent(Character,FModifierKeysState(),0,false));
    }
    void SaveShortcut()
    {
        auto& App=FSlateApplication::Get();const FModifierKeysState Command(false,false,false,false,false,false,true,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));
    }

    void VerifyIsolation()
    {
        const auto& M=*Scene->Model;Test->TestEqual(TEXT("Recorded frame unchanged"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("No recording render mutation"),M.RenderIntentRevision,RenderRevision);
        Test->TestTrue(TEXT("Camera independent"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Native flow conditions capture"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;FStudioAutomationForeground Foreground;TWeakObjectPtr<AStudioScene> Scene;
    FString Root,Work,Frozen,Saved;FStudioCameraState Camera;
    uint64 Changed=0,RenderRevision=0;int32 Phase=0,Frame=0;
    double Started=0,LastActivation=0;bool bStarted=false,bTooltips=true;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFlowUI,"Studio.FlowConditionsUI.UnitsCalculatorAndCaseIsolation",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioFlowUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioFlowUICommand(this));return true;}
#endif
