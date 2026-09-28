#include "StudioScene.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if WITH_DEV_AUTOMATION_TESTS
class FStudioInspectorCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioInspectorCommand(FAutomationTestBase* T):Test(T){}
    ~FStudioInspectorCommand(){if(bTooltipsCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(FPlatformTime::Seconds()-Started>90){Test->AddError(FString::Printf(TEXT("Inspector timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;const auto* Target=Scene->GetRenderTarget();
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||!Scene->HasCurrentFrame()||!Target||GFrameCounter-ChangedFrame<4||
            Scene->PresentedViewportSize()!=FIntPoint(Target->SizeX,Target->SizeY))return false;
        switch(Phase)
        {
        case 0:
        {
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/Inspector");Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            bTooltips=FSlateApplication::Get().GetAllowTooltips();bTooltipsCaptured=true;FSlateApplication::Get().SetAllowTooltips(false);
            FString Error;Workspace=M.Workspace;Tab=M.InspectorTab;Expanded=M.bViewportExpanded;
            Test->TestTrue(TEXT("Save prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            M.NewProject(TEXT("Airfoil · Solve inspectors"));M.Navigate(EStudioWorkspace::Solve);M.Pause();M.bViewportExpanded=false;M.InspectorTab=3;
            M.EditView(TEXT("Readable field"),[](auto& S){S.Display.bVolume=false;S.Display.bStreamlines=false;S.Display.bVectors=true;S.Display.bCutPlane=true;});
            M.EditCase(TEXT("Inspector case fixture"),[](auto& D)
            {
                D.Name=TEXT("Wing setup");D.Setup.CollisionModel=TEXT("BGK");
                FStudioBoundaryCondition B;B.Name=TEXT("Inlet");B.TargetId=D.Domain.Faces[0];B.Type=EStudioBoundaryType::VelocityInlet;B.Velocity=FVector(1,0,0);D.Boundaries.Add(B);
            });
            Scene->AlignCamera(FIntVector(0,-1,0));Scene->FitCamera();Next();break;
        }
        case 1:
            Camera=Scene->SavedCameraState();Frame=M.SelectedFrame;Case=StudioCaseIO::Serialize(M.Project.Draft);Revision=M.RenderIntentRevision;
            Test->TestEqual(TEXT("Only one visible scalar selector"),Count(TEXT("ScalarSelector")),1);
            Capture(TEXT("display.png"));Open(TEXT("FieldVectorSettings"));Next();break;
        case 2:
            Type(TEXT("VectorScale"),TEXT("nan"));OldEditor=FindTag(TEXT("VectorScale"));
            Test->TestEqual(TEXT("Invalid draft retains applied scale"),M.VectorScale,1.);Next();break;
        case 3:
            Capture(TEXT("invalid.png"));FSlateApplication::Get().DismissAllMenus();Press(TEXT("InspectorTab0"));Next();break;
        case 4:
            Test->TestEqual(TEXT("Setup category selected"),M.InspectorTab,0);
            Test->TestEqual(TEXT("Display actions not duplicated in Setup"),Count(TEXT("ScalarSelector")),0);
            Test->TestEqual(TEXT("Recording selector belongs to Setup"),Count(TEXT("RecordingSelector")),1);
            Capture(TEXT("setup.png"));Press(TEXT("InspectorTab1"));Next();break;
        case 5:Test->TestTrue(TEXT("Physics reads saved case choice"),VisibleText(GEngine->GameViewport->GetWindow().ToSharedRef()).Contains(TEXT("BGK")));Capture(TEXT("physics.png"));Press(TEXT("InspectorTab2"));Next();break;
        case 6:Test->TestTrue(TEXT("BCs reads assigned domain condition"),VisibleText(GEngine->GameViewport->GetWindow().ToSharedRef()).Contains(TEXT("Velocity inlet")));Capture(TEXT("boundaries.png"));Press(TEXT("InspectorTab3"));Next();break;
        case 7:Open(TEXT("FieldVectorSettings"));Next();break;
        case 8:
            Test->TestEqual(TEXT("Rejected draft survives category switches"),Text(TEXT("VectorScale")),FString(TEXT("nan")));
            Test->TestTrue(TEXT("The same editor owns the draft"),OldEditor.Pin()==FindTag(TEXT("VectorScale")));
            VerifyIsolation();Test->TestEqual(TEXT("Category switches do not schedule scientific work"),M.RenderIntentRevision,Revision);
            Capture(TEXT("retained-draft.png"));Type(TEXT("VectorScale"),TEXT("1.625"));Next();break;
        case 9:
            Test->TestEqual(TEXT("Repaired draft applies"),M.VectorScale,1.625);
            FSlateApplication::Get().DismissAllMenus();Press(TEXT("InspectorTab0"));Press(TEXT("InspectorTab3"));Next(90);break;
        case 90:Open(TEXT("FieldVectorSettings"));Next(10);break;
        case 10:
            Test->TestEqual(TEXT("Applied value retained"),Text(TEXT("VectorScale")),FString(TEXT("1.625")));
            Test->TestTrue(TEXT("Shared view undo"),M.UndoView());Next();break;
        case 11:
            Test->TestEqual(TEXT("Cached editor follows undo"),Text(TEXT("VectorScale")),FString(TEXT("1")));
            Test->TestTrue(TEXT("Shared view redo"),M.RedoView());FSlateApplication::Get().DismissAllMenus();
            Press(TEXT("InspectionTools"));Next();break;
        case 12:
            Test->TestEqual(TEXT("Active inspection replaces categories"),Count(TEXT("InspectorTab3")),0);
            Capture(TEXT("inspection.png"));Press(TEXT("CloseInspection"));Next();break;
        case 13:
            Test->TestEqual(TEXT("Closing inspection restores chosen category"),Count(TEXT("InspectorTab3")),1);
            VerifyIsolation();Test->TestTrue(TEXT("Save display view"),M.SaveProject(Work/TEXT("view.lbms")));
            M.NewProject(TEXT("Different project"));Next();break;
        case 14:Open(TEXT("FieldVectorSettings"));Next();break;
        case 15:
            Test->TestTrue(TEXT("Different project receives a fresh editor"),OldEditor.Pin()!=FindTag(TEXT("VectorScale")));
            Test->TestEqual(TEXT("Different project has its own scale"),Text(TEXT("VectorScale")),FString(TEXT("1")));
            FSlateApplication::Get().DismissAllMenus();Test->TestTrue(TEXT("Reopen settings"),M.RequestProjectOpen(Work/TEXT("view.lbms")));Next();break;
        case 16:
            Test->TestEqual(TEXT("Display scale reopens exactly"),M.VectorScale,1.625);Open(TEXT("StreamlineSettings"));Next();break;
        case 17:Type(TEXT("StreamSeedCount"),TEXT("1.5"));Next();break;
        case 18:
            FSlateApplication::Get().DismissAllMenus();Press(TEXT("InspectorTab1"));Press(TEXT("InspectorTab3"));Next(180);break;
        case 180:Open(TEXT("StreamlineSettings"));Next(19);break;
        case 19:
            Test->TestEqual(TEXT("Streamline draft survives switches"),Text(TEXT("StreamSeedCount")),FString(TEXT("1.5")));
            Capture(TEXT("streamline-draft.png"));FSlateApplication::Get().DismissAllMenus();
            Test->TestTrue(TEXT("Restore prior project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 20:M.InspectorTab=Tab;M.bViewportExpanded=Expanded;M.SaveSession();M.Navigate(Workspace);return true;
        }
        return false;
    }
private:
    void Next(int32 Target=-1){Phase=Target<0?Phase+1:Target;ChangedFrame=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag,bool Button=false)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if((!Button&&W->GetTag()==Tag)||(Button&&W->GetType()==TEXT("SButton")))return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=Find(C->GetChildAt(I),Tag,Button))return F;return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto F=Find(W,Tag))return F;
        Test->AddError(TEXT("Missing inspector control: ")+Tag.ToString());return {};
    }
    int32 CountIn(const TSharedRef<SWidget>& W,FName Tag)
    {
        if(!W->GetVisibility().IsVisible())return 0;int32 N=W->GetTag()==Tag?1:0;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)N+=CountIn(C->GetChildAt(I),Tag);return N;
    }
    int32 Count(FName Tag){return CountIn(GEngine->GameViewport->GetWindow().ToSharedRef(),Tag);}
    void Enter(const TSharedPtr<SWidget>& W)
    {
        if(!W)return;auto& A=FSlateApplication::Get();A.SetKeyboardFocus(W,EFocusCause::Navigation);
        A.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));A.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag){Enter(FindTag(Tag));}
    void Open(FName Tag){if(auto W=FindTag(Tag))Enter(Find(W.ToSharedRef(),NAME_None,true));}
    FString Text(FName Tag){auto W=FindTag(Tag);return W?StaticCastSharedPtr<SEditableTextBox>(W)->GetText().ToString():FString();}
    void Type(FName Tag,const FString& Value)
    {
        auto W=FindTag(Tag);if(!W)return;auto& A=FSlateApplication::Get();A.SetKeyboardFocus(W,EFocusCause::Navigation);
        const FModifierKeysState All(false,false,true,false,false,false,false,false,false);
        A.ProcessKeyDownEvent(FKeyEvent(EKeys::A,All,0,false,0,0));A.ProcessKeyUpEvent(FKeyEvent(EKeys::A,All,0,false,0,0));
        for(TCHAR C:Value)A.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));Enter(W);
    }
    void VerifyIsolation()
    {
        auto& M=*Scene->Model;Test->TestEqual(TEXT("Category retains original frame"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Category retains case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        Test->TestTrue(TEXT("Category retains camera"),StudioView::CameraEquals(Camera,Scene->SavedCameraState()));
    }
    FString VisibleText(const TSharedRef<SWidget>& W)
    {
        if(!W->GetVisibility().IsVisible())return {};
        FString S;if(W->GetType()==TEXT("STextBlock"))S=StaticCastSharedRef<STextBlock>(W)->GetText().ToString()+TEXT("\n");
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)S+=VisibleText(C->GetChildAt(I));return S;
    }
    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native inspector"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);FFileHelper::SaveArrayToFile(PNG,*(Root/Name));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Work,Case;FStudioCameraState Camera;
    EStudioWorkspace Workspace=EStudioWorkspace::Solve;int32 Phase=0,Tab=3,Frame=0;bool Expanded=false;
    TWeakPtr<SWidget> OldEditor;bool bTooltips=true,bTooltipsCaptured=false;double Started=0;uint64 ChangedFrame=0,Revision=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectorRender,"Studio.Inspector.ControlsAndDraftOwnership",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioInspectorRender::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioInspectorCommand(this));return true;}
#endif
