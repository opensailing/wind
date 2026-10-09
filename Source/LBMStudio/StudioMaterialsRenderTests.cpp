#include "StudioScene.h"
#include "StudioAuthoringTestCapture.h"
#include "StudioMaterials.h"
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
class FStudioMaterialsCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioMaterialsCommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioMaterialsCommand(){if(bCapturedTooltips&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(FPlatformTime::Seconds()-Started>100){Test->AddError(FString::Printf(TEXT("Materials workflow timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter-ChangedFrame<4)return false;
        auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/Materials");Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;PreviousWorkspace=M.Workspace;
            Test->TestTrue(TEXT("Save prior session project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=FSlateApplication::Get().GetAllowTooltips();bCapturedTooltips=true;FSlateApplication::Get().SetAllowTooltips(false);
            // Retired native tooltip windows must leave the capture tree before the next frame.
            StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing materials"));M.Pause();
            // Case geometry fixture only: no CFD values or solver output is generated.
            const FString Path=Work/TEXT("wing.obj");FFileHelper::SaveStringToFile(TEXT("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"),*Path);
            FString Hash;Test->TestTrue(TEXT("Hash explicit geometry fixture"),StudioAssets::HashFile(Path,MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false),Hash,Error));
            FStudioGeometryAsset Geometry;Geometry.Name=TEXT("Wing section");Geometry.SourcePath=Path;Geometry.SourceSHA256=Hash;Geometry.Format=TEXT("obj");GeometryId=Geometry.Id;
            Test->TestTrue(TEXT("Add saved case fixture"),M.EditCase(TEXT("Material geometry fixture"),[Geometry](auto& D){D.Geometry.Add(Geometry);D.Setup.BackendId=TEXT("test-control-harness");}));
            Camera=M.Project.Camera;Frame=M.SelectedFrame;Revision=M.RenderIntentRevision;Press(TEXT("Workspace4"));Next();break;
        }
        case 1:
            Test->TestTrue(TEXT("Sidebar opens Materials"),M.Workspace==EStudioWorkspace::Materials);Capture(TEXT("empty.png"));Press(TEXT("AddFluidMaterial"));Next();break;
        case 2:
            Test->TestEqual(TEXT("Fluid created through UI"),M.Project.Draft.Materials.Num(),1);
            if(M.Project.Draft.Materials.Num()!=1)return true;
            Fluid=M.Project.Draft.Materials[0].Id;
            Test->TestFalse(TEXT("Unknown field stays unset"),M.Project.Draft.Materials[0].Density.IsSet());
            if(!bFluidTyped)
            {
                Type(TEXT("MaterialName"),TEXT("Wing fluid"),false);Type(TEXT("MaterialProperty0"),TEXT("1.2"),false);Type(TEXT("MaterialProperty1"),TEXT("0.000015"),false);
                bFluidTyped=true;ChangedFrame=GFrameCounter;return false;
            }
            Test->TestEqual(TEXT("Density text reached the editor"),Text(TEXT("MaterialProperty0")),FString(TEXT("1.2")));
            if(auto Apply=FindTag(TEXT("ApplyMaterial")))Test->TestTrue(TEXT("Apply enables after Slate updates the dirty form"),Apply->IsEnabled());
            Press(TEXT("ApplyMaterial"));Next();break;
        case 3:
            Test->TestEqual(TEXT("Density applied"),M.Project.Draft.Materials[0].Density.Get(0),1.2);
            Press(TEXT("MaterialDomainAssignment"));Next();break;
        case 4:
            Test->TestEqual(TEXT("Domain assignment applied"),M.Project.Draft.Domain.FluidMaterialId,Fluid);
            Capture(TEXT("fluid.png"));Open(TEXT("MaterialUnit1"));Next();break;
        case 5:Press(TEXT("MaterialUnitOption1_1"));Next();break;
        case 6:
            Test->TestTrue(TEXT("Unit selector displays converted viscosity"),FMath::Abs(FCString::Atod(*Text(TEXT("MaterialProperty1")))-15.)<1.e-10);
            Test->TestEqual(TEXT("Display unit leaves stored SI unchanged"),M.Project.Draft.Materials[0].KinematicViscosity.Get(0),.000015);
            Type(TEXT("MaterialProperty0"),TEXT("nan"),true);Next();break;
        case 7:
            Test->TestEqual(TEXT("Invalid density rejected"),M.Project.Draft.Materials[0].Density.Get(0),1.2);Capture(TEXT("invalid.png"));Press(TEXT("Workspace7"));Next();break;
        case 8:Press(TEXT("Workspace4"));Next();break;
        case 9:
            Test->TestEqual(TEXT("Invalid draft survives workspace round trip"),Text(TEXT("MaterialProperty0")),FString(TEXT("nan")));
            Capture(TEXT("retained-draft.png"));Press(TEXT("AddSolidMaterial"));Next();break;
        case 10:
            Test->TestEqual(TEXT("Add solid preserves first draft"),M.Project.Draft.Materials.Num(),2);Solid=M.Project.Draft.Materials.Last().Id;
            if(!bSolidTyped)
            {
                Type(TEXT("MaterialName"),TEXT("Wing solid"),false);Type(TEXT("MaterialProperty0"),TEXT("2700"),false);Type(TEXT("MaterialProperty2"),TEXT("205"),false);Type(TEXT("MaterialProperty3"),TEXT("900"),false);
                bSolidTyped=true;ChangedFrame=GFrameCounter;return false;
            }
            Press(TEXT("ApplyMaterial"));Next();break;
        case 11:Press(FName(*(FString(TEXT("MaterialGeometry_"))+GeometryId.ToString())));Next();break;
        case 12:
            Test->TestEqual(TEXT("Object assignment applied"),M.Project.Draft.Geometry[0].MaterialId,Solid);Capture(TEXT("solid.png"));
            Press(FName(*(FString(TEXT("MaterialRow_"))+Fluid.ToString())));Next();break;
        case 13:
            Test->TestEqual(TEXT("Invalid draft survives material selection"),Text(TEXT("MaterialProperty0")),FString(TEXT("nan")));
            Press(TEXT("Workspace7"));Next();break;
        case 14:SaveShortcut();Next();break;
        case 15:
            Test->TestTrue(TEXT("Save returns to unapplied material"),M.Workspace==EStudioWorkspace::Materials);
            Test->TestTrue(TEXT("Save explains required draft resolution"),M.Notice.Contains(TEXT("Apply or revert")));
            Press(TEXT("RevertMaterial"));Next();break;
        case 16:
            Test->TestFalse(TEXT("Reverting properties resolves the footer save warning"),M.Notice.Contains(TEXT("Apply or revert")));
            Test->TestEqual(TEXT("Revert restores applied property"),Text(TEXT("MaterialProperty0")),FString(TEXT("1.2")));
            Type(TEXT("MaterialProperty0"),TEXT("1.3"),false);
            Test->TestTrue(TEXT("External edit creates a real conflict"),M.EditCase(TEXT("External material update"),[Id=Fluid](auto& D){for(auto& Material:D.Materials)if(Material.Id==Id)Material.Density=1.4;}));Next();break;
        case 17:
            Test->TestEqual(TEXT("Conflict retains typed value"),Text(TEXT("MaterialProperty0")),FString(TEXT("1.3")));
            if(auto Apply=FindTag(TEXT("ApplyMaterial")))Test->TestFalse(TEXT("Conflict cannot overwrite saved edit"),Apply->IsEnabled());
            Capture(TEXT("conflict.png"));Press(TEXT("RevertMaterial"));Next();break;
        case 18:
            Test->TestEqual(TEXT("Revert adopts the changed property"),Text(TEXT("MaterialProperty0")),FString(TEXT("1.4")));
            M.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Saved material setup"),M.Project.Draft,EStudioRunOrigin::ControlHarness));
            Frozen=StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration());Press(TEXT("DeleteMaterial"));Next();break;
        case 19:
            Test->TestEqual(TEXT("Delete removes one material"),M.Project.Draft.Materials.Num(),1);
            Test->TestFalse(TEXT("Delete clears domain assignment"),M.Project.Draft.Domain.FluidMaterialId.IsValid());Press(TEXT("MaterialsUndo"));Next();break;
        case 20:
            Test->TestEqual(TEXT("Undo restores domain assignment"),M.Project.Draft.Domain.FluidMaterialId,Fluid);
            Test->TestEqual(TEXT("Undo restores material"),M.Project.Draft.Materials.Num(),2);
            Test->TestEqual(TEXT("Frozen run remains exact"),StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration()),Frozen);
            Test->TestTrue(TEXT("Save case with materials"),M.SaveProject(Work/TEXT("materials.lbms")));Saved=StudioCaseIO::Serialize(M.Project.Draft);
            VerifyIsolation();M.NewProject(TEXT("Other project"));M.Navigate(EStudioWorkspace::Materials);Next();break;
        case 21:
            Test->TestTrue(TEXT("Other project has no stale materials"),M.Project.Draft.Materials.IsEmpty());
            Test->TestTrue(TEXT("Reopen authored project"),M.RequestProjectOpen(Work/TEXT("materials.lbms")));Next();break;
        case 22:
            Test->TestEqual(TEXT("Materials and assignments reopen exactly"),StudioCaseIO::Serialize(M.Project.Draft),Saved);
            M.Navigate(EStudioWorkspace::Materials);Next();break;
        case 23:
            Capture(TEXT("reopened.png"));Test->TestTrue(TEXT("Restore prior project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 24:M.Navigate(PreviousWorkspace);return true;
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
        Test->AddError(TEXT("Missing material control: ")+Tag.ToString());return {};
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
    void VerifyIsolation()
    {
        const auto& M=*Scene->Model;Test->TestEqual(TEXT("Material edits keep source frame"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Material edits never rebuild CFD"),M.RenderIntentRevision,Revision);
        Test->TestTrue(TEXT("Material edits keep camera"),StudioView::CameraEquals(M.Project.Camera,Camera));
    }
    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native material workspace"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Retain material evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Work,Frozen,Saved;
    FGuid Fluid,Solid,GeometryId;FStudioCameraState Camera;EStudioWorkspace PreviousWorkspace=EStudioWorkspace::Solve;
    bool bTooltips=true,bCapturedTooltips=false,bFluidTyped=false,bSolidTyped=false;int32 Phase=0,Frame=0;uint64 ChangedFrame=0,Revision=0;double Started=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMaterialsRender,"Studio.Materials.ControlsDraftsAssignmentsAndReopen",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioMaterialsRender::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioMaterialsCommand(this));return true;}
#endif
