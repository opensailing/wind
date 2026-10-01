#include "SStudioComparisonWorkspace.h"
#include "StudioSavedFieldView.h"
#include "StudioFieldExportUI.h"
#include "StudioScene.h"
#include "StudioFileDialog.h"
#include "StudioAuthoringTestCapture.h"
#include "StudioAutomationForeground.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Export through routed Slate input, retaining the original camera state for an
 * independent VTK/CSV reader. Injected paths do not test native panel permissions. */
class FStudioComparisonExportUICommand final : public IAutomationLatentCommand
{
    struct FPublishGate
    {
        FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
        FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
        ~FPublishGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
    };
public:
    explicit FStudioComparisonExportUICommand(FAutomationTestBase* In):Test(In){}
    ~FStudioComparisonExportUICommand()
    {if(Gate)Gate->Release->Trigger();if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Foreground.WasInterrupted()){Test->AddError(Foreground.Describe(Phase));return true;}
        if(Now-Started>180){Test->AddError(FString::Printf(TEXT("Comparison export timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)if(It->Model&&!It->Model->IsSnapshotView())Scene=*It;
        if(!Scene.IsValid()||GFrameCounter<Changed+8)return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        if(bOpenExport)
        {
            bOpenExport=false;Press(TEXT("ExportMenu"));const auto A=StaticCastSharedPtr<SMenuAnchor>(Find(TEXT("ExportMenu")));
            Test->TestTrue(TEXT("Export anchor opens through routed input"),A&&A->IsOpen());Changed=GFrameCounter;return false;
        }
        if(!PendingFolder.IsEmpty())
        {
            const auto Folder=StaticCastSharedPtr<SEditableTextBox>(Find(TEXT("ExportFolderName")));
            Test->TestTrue(TEXT("Committed export folder matches routed text"),Folder&&Folder->GetText().ToString()==PendingFolder);
            PendingFolder.Empty();StudioFileDialog::SetNextExportFolderForAutomation(Work);Press(TEXT("SaveFieldVTK"));Changed=GFrameCounter;return false;
        }
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/ComparisonExportUI");IFileManager::Get().MakeDirectory(*Root,true);
            Work=Root/FGuid::NewGuid().ToString();FString Error;
            Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            FFileHelper::SaveStringToFile(Work,*(Root/TEXT("last-run.txt")));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();Foreground.Begin();
            M.NewProject(TEXT("Comparison exports"));M.ReviewRecordedFrame(420);Next();break;
        }
        case 1:if(!Scene->HasCurrentFrame())return false;Press(TEXT("Workspace9"));Next();break;
        case 2:Press(TEXT("ResultsCompare"));Open();Next();break;
        case 3:
            Test->TestTrue(TEXT("Unavailable comparison never falls through to Solve"),!Enabled(TEXT("SaveFieldVTK"))&&HasText(TEXT("Choose both recordings"))&&HasText(TEXT("Export comparison")));
            Capture(TEXT("unavailable.png"));App.DismissAllMenus();Press(TEXT("CompareSourceB"));Next();break;
        case 4:Press(TEXT("CompareB_MeshGraphNets_Airfoil_test010"));Next();break;
        case 5:if(!Enabled(TEXT("CompareSourceB")))return false;Press(TEXT("CompareAlignment"));Next();break;
        case 6:Press(TEXT("CompareAlignment1"));Press(TEXT("CompareScalar"));Next();break;
        case 7:Press(TEXT("CompareScalar_pressure"));Type(TEXT("CompareFrame"),TEXT("421"));Press(TEXT("CompareApply"));Next();break;
        case 8:
            if(!Ready())return false;Press(TEXT("CompareCameraA"));Next();break;
        case 9:Type(TEXT("CompareCamera0Axis0"),TEXT("0.375"));App.DismissAllMenus();Press(TEXT("CompareCommonRange"));Next();break;
        case 10:
            if(!Ready())return false;if(!Oracle(TEXT("exact")))return true;Baseline=StudioProjectIO::Serialize(M.SnapshotProject());Open();Next();break;
        case 11:
            Test->TestTrue(TEXT("Pair identity and original aligned times visible"),HasText(TEXT("Frozen A · frame 421"))&&HasText(TEXT("Frozen B · frame 421"))&&HasText(TEXT("Recorded timestamps"))&&Enabled(TEXT("SaveFieldVTK")));
            Test->TestFalse(TEXT("Comparison does not expose Solve arrays or ranges"),Find(TEXT("VTKSelectAll")).IsValid()||Find(TEXT("ExportScopeRange")).IsValid());
            Capture(TEXT("exact-vtk.png"));Type(TEXT("ExportFolderName"),TEXT("../bad"));Next();break;
        case 12:
            Test->TestTrue(TEXT("Unsafe folder name disables save"),!Enabled(TEXT("SaveFieldVTK"))&&HasText(TEXT("Enter a new folder name")));
            Capture(TEXT("invalid-folder.png"));Type(TEXT("ExportFolderName"),TEXT("exact-source"));Next();break;
        case 13:
            Capture(TEXT("corrected-folder.png"));Test->AddInfo(MenuInfo());
            StudioFileDialog::SetNextExportFolderForAutomation(FString());Press(TEXT("SaveFieldVTK"));Next();break;
        case 14:
            Test->TestTrue(TEXT("Native picker cancellation reported"),M.Notice.Contains(TEXT("folder selection cancelled")));Open();Next();break;
        case 15:
            OldA=Side(false)->PresentedField();OldB=Side(true)->PresentedField();Gate=MakeShared<FPublishGate,ESPMode::ThreadSafe>();
            FStudioFieldExportUI::SetBeforeNextPublishForAutomation([G=Gate]{G->Reached->Trigger();G->Release->Wait(20000);});Save(TEXT("exact-source"));Next();break;
        case 16:
            if(!Gate->Reached->Wait(0))return false;Type(TEXT("CompareFrame"),TEXT("422"));Open();Next();break;
        case 17:
            Test->TestTrue(TEXT("Pending export pins original pair after comparison edit destroys its scenes"),!Side(false)&&!Side(true)&&OldA.IsValid()&&OldB.IsValid()&&HasText(TEXT("Frozen A · frame 421"))&&Enabled(TEXT("CancelFieldVTK"))&&!Enabled(TEXT("SaveFieldVTK")));
            Capture(TEXT("frozen.png"));App.DismissAllMenus();Gate->Release->Trigger();Next();break;
        case 18:
            if(!Saved(TEXT("exact-source")))return false;
            Test->TestTrue(TEXT("Completed closed export releases original snapshots"),!OldA.IsValid()&&!OldB.IsValid());CheckSolve();Open();Next();break;
        case 19:
            Test->TestTrue(TEXT("Stale comparison requires Compare frames"),!Enabled(TEXT("SaveFieldVTK"))&&HasText(TEXT("Compare frames")));
            Capture(TEXT("stale.png"));App.DismissAllMenus();Type(TEXT("CompareFrame"),TEXT("421"));Press(TEXT("CompareApply"));Next();break;
        case 20:if(!Ready())return false;if(!Oracle(TEXT("csv")))return true;Open();Next();break;
        case 21:
            Press(TEXT("ExportFormatCSV"));Press(TEXT("VTKCoordinatesScene"));Capture(TEXT("csv-scene.png"));Save(TEXT("exact-scene"));Next();break;
        case 22:if(!Saved(TEXT("exact-scene")))return false;CheckSolve();Open();Next();break;
        case 23:
            Test->TestTrue(TEXT("Saved bundle can be revealed"),Enabled(TEXT("RevealFieldVTK")));Capture(TEXT("saved.png"));Save(TEXT("exact-scene"));Next();break;
        case 24:if(!M.Notice.StartsWith(TEXT("Comparison export failed")))return false;Open();Next();break;
        case 25:
            Test->TestTrue(TEXT("Existing destination has actionable recovery"),HasText(TEXT("already exists")));Capture(TEXT("existing-folder.png"));App.DismissAllMenus();
            Press(TEXT("CompareBack"));StudioFileDialog::SetNextRecordingFolderForAutomation(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture"));Press(TEXT("ResultsImport"));Next();break;
        case 26:Baseline=StudioProjectIO::Serialize(M.SnapshotProject());Press(TEXT("ResultsCompare"));Press(TEXT("CompareSourceB"));Next();break;
        case 27:Press(TEXT("CompareB_MeshGraphNets_Airfoil_test010"));Next();break;
        case 28:if(!Enabled(TEXT("CompareSourceB")))return false;Press(TEXT("CompareAlignment"));Next();break;
        case 29:Press(TEXT("CompareAlignment3"));Type(TEXT("CompareOffset"),TEXT("2.50245"));Press(TEXT("CompareMatch"));Next();break;
        case 30:Press(TEXT("CompareNearest"));Type(TEXT("CompareTolerance"),TEXT("0.00006"));Press(TEXT("CompareScalar"));Next();break;
        case 31:Press(TEXT("CompareScalar_pressure"));Press(TEXT("CompareApply"));Next();break;
        case 32:if(!Ready())return false;if(!Oracle(TEXT("nearest")))return true;Open();Next();break;
        case 33:
            Test->TestTrue(TEXT("Nearest comparison explains its signed mismatch"),HasText(TEXT("Manual B offset"))&&HasText(TEXT("nearest original frame"))&&HasText(TEXT("B − A -5e-05")));
            Press(TEXT("ExportFormatVTK"));Press(TEXT("VTKCoordinatesSource"));Capture(TEXT("nearest.png"));Save(TEXT("nearest-source"));Next();break;
        case 34:if(!Saved(TEXT("nearest-source")))return false;CheckSolve();Open();Next();break;
        case 35:
            Gate=MakeShared<FPublishGate,ESPMode::ThreadSafe>();
            FStudioFieldExportUI::SetBeforeNextPublishForAutomation([G=Gate]{G->Reached->Trigger();G->Release->Wait(20000);});Save(TEXT("cancelled"));Next();break;
        case 36:if(!Gate->Reached->Wait(0))return false;Press(TEXT("Workspace7"));Open();Next();break;
        case 37:
            Test->TestTrue(TEXT("Pending pair owns Export in Solve"),HasText(TEXT("Export comparison"))&&Enabled(TEXT("CancelFieldVTK"))&&!Enabled(TEXT("SaveFieldVTK")));
            Capture(TEXT("progress-in-solve.png"));App.DismissAllMenus();M.NewProject(TEXT("Replacement during pair export"));Open();Next();break;
        case 38:
            Test->TestTrue(TEXT("Old project export remains cancellable and prevents second write"),HasText(TEXT("Export comparison"))&&Enabled(TEXT("CancelFieldVTK"))&&!Enabled(TEXT("SaveFieldVTK")));
            Capture(TEXT("progress-new-project.png"));Press(TEXT("CancelFieldVTK"));Gate->Release->Trigger();Next();break;
        case 39:
            if(Find(TEXT("CancelFieldVTK")))return false;
            Test->TestFalse(TEXT("Cancelled pair never publishes"),IFileManager::Get().DirectoryExists(*(Work/TEXT("cancelled"))));
            Test->TestFalse(TEXT("Old completion does not enter new project"),M.Notice.Contains(TEXT("Comparison export cancelled")));
            App.DismissAllMenus();Open();Next();break;
        case 40:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Original export restored after pair drains"),HasText(TEXT("Export original field data"))&&Find(TEXT("VTKSelectAll"))&&Enabled(TEXT("SaveFieldVTK")));
            Capture(TEXT("original-solve.png"));App.DismissAllMenus();Test->TestTrue(TEXT("Restore prior project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 41:return true;
        }
        return false;
    }
private:
    AStudioScene* Side(bool B)
    {for(TActorIterator<AStudioScene> It(Scene->GetWorld());It;++It)if(IsValid(*It)&&It->ActorHasTag(B?TEXT("StudioComparisonB"):TEXT("StudioComparisonA")))return *It;return nullptr;}
    bool Ready(){auto* A=Side(false);auto* B=Side(true);return A&&B&&A->HasCurrentFrame()&&B->HasCurrentFrame()&&Enabled(TEXT("CompareApply"));}
    bool Oracle(const TCHAR* Name)
    {
        const auto W=Find(TEXT("ComparisonWorkspace"));FString Error;
        const auto R=W?StaticCastSharedPtr<SStudioComparisonWorkspace>(W)->ExportSnapshot(Error):TOptional<FStudioComparisonExportRequest>();
        if(!Test->TestTrue(TEXT("Capture frozen camera oracle: ")+Error,R.IsSet()))return false;
        auto J=MakeShared<FJsonObject>();
        for(bool B:{false,true})
        {FStudioSavedFieldView V;const auto& S=B?R->Pair.Secondary:R->Pair.Primary;V.Title=S.Title;V.Identity=S.Identity;V.Camera=B?R->SecondaryCamera:R->PrimaryCamera;
         J->SetObjectField(B?TEXT("secondary"):TEXT("primary"),StudioSavedFieldViews::ToJSON(V));}
        J->SetBoolField(TEXT("shared"),R->bSharedRange);FString JSON;FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&JSON));
        return Test->TestTrue(TEXT("Retain original camera state"),FFileHelper::SaveStringToFile(JSON,*(Work/(FString(Name)+TEXT("-oracle.json"))),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    }
    void Save(const TCHAR* Name)
    {Type(TEXT("ExportFolderName"),Name);PendingFolder=Name;}
    bool Saved(const TCHAR* Name){return Scene->Model->Notice.StartsWith(FString(TEXT("Saved "))+Name);}
    void CheckSolve(){Test->TestEqual(TEXT("Export leaves Solve state unchanged"),StudioProjectIO::Serialize(Scene->Model->SnapshotProject()),Baseline);}
    void Next(){++Phase;Changed=GFrameCounter;}
    void Open(){bOpenExport=true;}
    TSharedPtr<SWidget> FindIn(const TSharedRef<SWidget>& W,FName Tag)
    {W->UpdateAllAttributes();if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=FindIn(C->GetChildAt(I),Tag))return F;return {};}
    TSharedPtr<SWidget> FindWindow(const TSharedRef<SWindow>& W,FName Tag)
    {if(auto Found=FindIn(W,Tag))return Found;for(const auto& Child:W->GetChildWindows())if(auto Found=FindWindow(Child,Tag))return Found;return {};}
    TSharedPtr<SWidget> Find(FName Tag)
    {for(const auto& W:FSlateApplication::Get().GetInteractiveTopLevelWindows())if(auto Found=FindWindow(W,Tag))return Found;return {};}
    FString MenuInfo()
    {
        auto& App=FSlateApplication::Get();const auto Anchor=StaticCastSharedPtr<SMenuAnchor>(Find(TEXT("ExportMenu")));const auto Focus=App.GetKeyboardFocusedWidget();
        const auto Folder=StaticCastSharedPtr<SEditableTextBox>(Find(TEXT("ExportFolderName")));const auto Notice=StaticCastSharedPtr<STextBlock>(Find(TEXT("VTKNotice")));
        const FString Details=FString::Printf(TEXT(" folder=%s validation=%s"),Folder?*Folder->GetText().ToString():TEXT("missing"),Notice?*Notice->GetText().ToString():TEXT("missing"));
        return FString::Printf(TEXT("phase=%d menus=%d anchor=%d panel=%d windows=%d focus=%s/%s frame=%d notice=%s"),Phase,App.AnyMenusVisible(),Anchor&&Anchor->IsOpen(),Find(TEXT("FieldExportPanel")).IsValid(),
            App.GetInteractiveTopLevelWindows().Num(),Focus?*Focus->GetTypeAsString():TEXT("none"),Focus?*Focus->GetTag().ToString():TEXT("none"),Scene->Model->SelectedFrame,*Scene->Model->Notice)+Details;
    }
    bool Enabled(FName Tag){const auto W=Find(Tag);return W&&W->IsEnabled();}
    bool HasTextIn(const TSharedRef<SWidget>& W,const FString& Text)
    {if(!W->GetVisibility().IsVisible())return false;if(W->GetType()==TEXT("STextBlock")&&StaticCastSharedRef<STextBlock>(W)->GetText().ToString().Contains(Text))return true;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(HasTextIn(C->GetChildAt(I),Text))return true;return false;}
    bool HasWindowText(const TSharedRef<SWindow>& W,const FString& Text)
    {if(HasTextIn(W,Text))return true;for(const auto& Child:W->GetChildWindows())if(HasWindowText(Child,Text))return true;return false;}
    bool HasText(const FString& Text)
    {for(const auto& W:FSlateApplication::Get().GetInteractiveTopLevelWindows())if(HasWindowText(W,Text))return true;return false;}
    TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& W)
    {if(!W->GetVisibility().IsVisible()||!W->IsEnabled())return {};if(W->SupportsKeyboardFocus())return W;auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=Focusable(C->GetChildAt(I)))return F;return {};}
    void Key(FKey K,const FModifierKeysState& Mod={})
    {auto& A=FSlateApplication::Get();A.ProcessKeyDownEvent(FKeyEvent(K,Mod,0,false,0,0));A.ProcessKeyUpEvent(FKeyEvent(K,Mod,0,false,0,0));}
    bool Focus(FName Tag)
    {const auto W=Find(Tag);if(!Test->TestTrue(TEXT("Export widget available: ")+Tag.ToString()+TEXT(" · ")+MenuInfo(),W&&W->IsEnabled()))return false;
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Focusable export widget"),Target.IsValid()))return false;
        FSlateApplication::Get().SetKeyboardFocus(Target,EFocusCause::Navigation);return Test->TestTrue(TEXT("Export keyboard focus"),Target->HasKeyboardFocus()||Target->HasFocusedDescendants());}
    void Press(FName Tag){if(Focus(Tag))Key(EKeys::Enter);}
    void Type(FName Tag,const FString& Text)
    {if(!Focus(Tag))return;Key(EKeys::A,FModifierKeysState(false,false,true,false,false,false,false,false,false));for(const TCHAR C:Text)FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));if(Tag!=TEXT("ExportFolderName"))Key(EKeys::Tab);}
    void Capture(const TCHAR* Name)
    {
        const auto Panel=Find(TEXT("FieldExportPanel"));Test->TestTrue(TEXT("Export panel visible: ")+MenuInfo(),Panel.IsValid());
        if(Panel)
        {
            const auto Bounds=Panel->GetCachedGeometry().GetLayoutBoundingRect();
            for(const TCHAR* Tag:{TEXT("VTKFrozenFrame"),TEXT("SaveFieldVTK"),TEXT("CancelFieldVTK"),TEXT("VTKNotice"),TEXT("RevealFieldVTK")})if(const auto W=Find(Tag))
            {const auto R=W->GetCachedGeometry().GetLayoutBoundingRect();Test->TestTrue(FString(Name)+TEXT(" keeps ")+Tag+TEXT(" visible"),R.Top>=Bounds.Top&&R.Bottom<=Bounds.Bottom&&R.Left>=Bounds.Left&&R.Right<=Bounds.Right);}
        }
        TArray<FColor> Pixels;FIntVector Size;if(!Test->TestTrue(TEXT("Capture comparison export"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);Test->TestTrue(TEXT("Save export capture"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FStudioAutomationForeground Foreground;
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;TSharedPtr<FPublishGate,ESPMode::ThreadSafe> Gate;
    TWeakPtr<const IStudioField,ESPMode::ThreadSafe> OldA,OldB;
    FString Root,Work,Baseline,PendingFolder;int32 Phase=0;uint64 Changed=0;double Started=0,LastActivation=0;bool bTooltips=true,bCaptured=false,bOpenExport=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioComparisonExportUI,"Studio.ComparisonExportUI.FrozenPairsAndLifecycle",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioComparisonExportUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioComparisonExportUICommand(this));return true;}
#endif
