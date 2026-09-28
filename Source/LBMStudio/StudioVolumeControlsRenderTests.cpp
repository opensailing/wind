#include "StudioScene.h"
#include "StudioFileDialog.h"
#include "StudioVolume.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Exercises visible Slate commands with deterministic native-folder results.
 * This does not substitute for NSOpenPanel or sandbox-grant/relaunch acceptance.
 */
class FStudioVolumeControlsCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioVolumeControlsCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()||Now-Started>80)
        {FSlateApplication::Get().DismissAllMenus();if(!Test->HasAnyErrors())Test->AddError(FString::Printf(TEXT("Surface controls timed out in phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        if(GFrameCounter<ResumeFrame)return false;
        auto& M=*Scene->Model;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/VolumeControls");
            Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;Test->TestTrue(TEXT("Retain preceding test project"),StudioProjectIO::Save(Work/TEXT("original.lbms"),M.SnapshotProject(),Error));
            First=CopySurface(TEXT("first"));Second=CopySurface(TEXT("relocated"));
            M.NewProject(TEXT("3D cylinder · volume review"));
            Test->TestTrue(TEXT("Open authentic original-point fixture"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json")));
            Phase=1;break;
        }
        case 1:
            if(!Ready())return false;
            M.Scrub(.5);M.EditView(TEXT("Wing field inspection"),[](auto& S)
            {
                S.Display.bVectors=false;S.Display.bSourcePoints=true;S.Display.bVolume=true;S.Display.bCutPlane=false;S.Display.bVolumeIsosurface=false;
                S.Display.ScalarField=TEXT("velocity_magnitude");
            });Scene->FitCamera();Phase=2;break;
        case 2:
            if(!Ready())return false;
            Camera=Scene->SavedCameraState();Frame=M.SelectedFrame;Dataset=M.Project.Dataset;
            Case=StudioCaseIO::Serialize(M.Project.Draft);SourceHash=M.Project.Recordings[0].MetadataSHA256;
            OpenSurface(3);break;
        case 3:
            Capture(TEXT("empty.png"));StudioFileDialog::SetNextReconstructionFolderForAutomation(FPaths::GetPath(First));
            Press(TEXT("ImportSurface"));Test->TestTrue(TEXT("Visible import begins verification"),M.IsRecordingLoadPending());Phase=4;break;
        case 4:
            if(!Ready())return false;
            VerifyIdentity();Test->TestTrue(TEXT("Visible import attaches verified topology"),M.Solver->VolumeReconstruction().IsValid());
            Test->TestTrue(TEXT("Attached topology creates continuous surface"),Scene->ResourceStats().ScalarTextureBytes>0);
            Capture(TEXT("surface.png"));FSlateApplication::Get().DismissAllMenus();
            M.EditView(TEXT("Small physical scalar control test"),[](auto& S){S.Display.ScalarField=TEXT("cell_volume");});Phase=40;break;
        case 40:
            if(!Ready())return false;
            SelectInspector(3,400);break;
        case 400:
            if(const auto Anchor=FindTag(TEXT("VolumeSettings")))Key(Find(Anchor.ToSharedRef(),NAME_None,true),EKeys::Enter);
            Phase=41;ResumeFrame=GFrameCounter+2;break;
        case 41:
            if(auto Widget=FindTag(TEXT("VolumeIsovalue")))
            {
                const auto Box=StaticCastSharedPtr<SEditableTextBox>(Widget);
                Test->TestTrue(TEXT("Sub-nanoscopic original field displays a nonzero scientific value"),Box->GetText().ToString().Contains(TEXT("e-")));
                Box->SetText(FText::FromString(TEXT("1.2345678901234567e-9")));Key(Box,EKeys::Enter);
                Test->TestEqual(TEXT("Visible isovalue input retains full double precision"),M.VolumeIsovalue,1.2345678901234567e-9);
                Box->SetText(FText::FromString(TEXT("NaN")));Key(Box,EKeys::Enter);
                Test->TestEqual(TEXT("Invalid physical input retains preceding finite value"),M.VolumeIsovalue,1.2345678901234567e-9);
                Box->SetText(FText::FromString(TEXT("1.2345678901234567e-9")));Key(Box,EKeys::Enter);
            }
            Phase=42;break;
        case 42:
            if(!Ready())return false;
            Capture(TEXT("physical-value-controls.png"));FSlateApplication::Get().DismissAllMenus();
            M.EditView(TEXT("Restore velocity field"),[](auto& S){S.Display.ScalarField=TEXT("velocity_magnitude");});Phase=43;break;
        case 43:
            if(!Ready())return false;
            Press(TEXT("VolumeDisplay"),EKeys::SpaceBar);Phase=5;break;
        case 5:
            if(!Ready())return false;
            VerifyIdentity();Test->TestFalse(TEXT("Visible representation toggle switches to points"),M.bVolume);
            Test->TestEqual(TEXT("Point choice releases scalar texture"),Scene->ResourceStats().ScalarTextureBytes,int64(0));
            Capture(TEXT("points.png"));Press(TEXT("VolumeDisplay"),EKeys::SpaceBar);Phase=6;break;
        case 6:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Representation toggle restores surface"),M.bVolume);
            OpenSurface(7);break;
        case 7:
            Capture(TEXT("loaded-menu.png"));StudioFileDialog::SetNextReconstructionFolderForAutomation(FPaths::GetPath(Second));
            Press(TEXT("LocateSurface"));Phase=8;break;
        case 8:
            if(!Ready())return false;
            VerifyIdentity();Test->TestEqual(TEXT("Visible Locate retains matching relocated file"),ReferencePath(),Second);
            Test->TestTrue(TEXT("Save source, representation and camera"),M.SaveProject(Work/TEXT("inspection.lbms")));
            M.NewProject(TEXT("Temporary workspace"));Test->TestTrue(TEXT("Reopen saved reconstruction"),M.RequestProjectOpen(Work/TEXT("inspection.lbms")));Phase=9;break;
        case 9:
            if(!Ready())return false;
            VerifyIdentity();Test->TestTrue(TEXT("Reopening restores continuous surface"),M.bVolume&&M.Solver->VolumeReconstruction().IsValid());
            Capture(TEXT("reopened.png"));M.NewProject(TEXT("Repair test workspace"));
            // The retained document is deliberately clean. Unsaved replacement
            // protection is covered separately; unattended dialogs choose Cancel.
            Test->TestTrue(TEXT("Save retained workspace before repair"),M.SaveProject(Work/TEXT("retained.lbms")));
            Test->TestTrue(TEXT("Move only the temporary test manifest"),IFileManager::Get().Move(*(Second+TEXT(".held")),*Second));
            Test->TestTrue(TEXT("Open project with relocated manifest missing"),M.RequestProjectOpen(Work/TEXT("inspection.lbms")));Phase=10;break;
        case 10:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            if(!Test->TestTrue(TEXT("Missing reconstruction has distinct repair action"),M.RecordingRepair.IsSet()&&M.RecordingRepair->bReconstruction))return true;
            // The repair banner resizes the viewport during Slate layout, after
            // the model/frame transaction. Allow that layout and its on-demand
            // capture to reach the screen before recording the repair state.
            if(!RepairReadyFrame){RepairReadyFrame=GFrameCounter;return false;}
            if(GFrameCounter-RepairReadyFrame<3)return false;
            {
                auto* Target=Scene->GetRenderTarget();
                if(Scene->PresentedViewportSize()!=FIntPoint(Target->SizeX,Target->SizeY))return false;
                TArray<FColor> FieldPixels;
                Test->TestTrue(TEXT("Read retained field after repair layout"),Target->GameThread_GetRenderTargetResource()->ReadPixels(FieldPixels));
                int32 Colored=0;
                for(const FColor& P:FieldPixels)
                    if(FMath::Max3(P.R,P.G,P.B)>80&&FMath::Max3(P.R,P.G,P.B)-FMath::Min3(P.R,P.G,P.B)>40)++Colored;
                if(!Test->TestTrue(TEXT("Missing-source repair retains visible flow pixels"),Colored>1000))return true;
            }
            Capture(TEXT("missing.png"));StudioFileDialog::SetNextReconstructionFolderForAutomation(FPaths::GetPath(First));
            Press(TEXT("LocateRepairSource"));Test->TestTrue(TEXT("Visible repair begins project verification"),M.IsProjectOpenPending());Phase=11;break;
        case 11:
            if(!Ready())return false;
            VerifyIdentity();Test->TestFalse(TEXT("Visible Locate clears repair banner"),M.RecordingRepair.IsSet());
            Test->TestEqual(TEXT("Repair uses matching chosen source"),ReferencePath(),First);
            Capture(TEXT("repaired.png"));OpenSurface(12);break;
        case 12:Press(TEXT("RemoveSurface"));Phase=13;break;
        case 13:
            if(!Ready())return false;
            VerifyIdentity();Test->TestFalse(TEXT("Visible removal detaches topology"),M.Solver->VolumeReconstruction().IsValid());
            Test->TestEqual(TEXT("Removal restores original-point renderer"),Scene->ResourceStats().ScalarTextureBytes,int64(0));
            Test->TestTrue(TEXT("Removal preserves reconstruction file"),IFileManager::Get().FileExists(*First));
            Capture(TEXT("removed.png"));Test->TestTrue(TEXT("Restore preceding test project"),M.RequestProjectOpen(Work/TEXT("original.lbms")));Phase=14;break;
        case 14:return Ready();
        case 100:
            if(const auto Anchor=FindTag(TEXT("SurfaceSettings")))Key(Find(Anchor.ToSharedRef(),NAME_None,true),EKeys::Enter);
            Phase=MenuNextPhase;ResumeFrame=GFrameCounter+2;break;
        }
        return false;
    }
private:
    bool Ready() const{return !Scene->Model->IsProjectOpenPending()&&!Scene->Model->IsRecordingLoadPending()&&Scene->HasCurrentFrame();}
    FString ReferencePath() const
    {
        const auto& M=*Scene->Model;
        const auto* R=M.Project.Recordings.FindByPredicate([&](const auto& Item){return Item.Id==M.Project.Dataset;});
        return R&&R->Reconstruction.IsSet()?FPaths::ConvertRelativePathToFull(R->Reconstruction->Path):FString();
    }
    void VerifyIdentity()
    {
        const auto& M=*Scene->Model;
        Test->TestEqual(TEXT("Surface commands retain source dataset"),M.Project.Dataset,Dataset);
        Test->TestEqual(TEXT("Surface commands retain original frame"),M.SelectedFrame,Frame);
        Test->TestTrue(TEXT("Surface commands retain exact camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
        Test->TestEqual(TEXT("Surface commands retain authoring case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        const auto* R=M.Project.Recordings.FindByPredicate([&](const auto& Item){return Item.Id==Dataset;});
        if(Test->TestNotNull(TEXT("Original recording reference remains"),R))Test->TestEqual(TEXT("Original metadata hash is unchanged"),R->MetadataSHA256,SourceHash);
    }
    FString CopySurface(const TCHAR* Name)
    {
        const FString Folder=FPaths::ConvertRelativePathToFull(Work/Name);IFileManager::Get().MakeDirectory(*Folder,true);
        for(const TCHAR* File:{TEXT("reconstruction.json"),TEXT("rows.u32"),TEXT("weights.f64"),TEXT("classification.u8")})
            Test->TestEqual(TEXT("Copy immutable test reconstruction"),IFileManager::Get().Copy(*(Folder/File),*(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture")/File)),uint32(COPY_OK));
        return Folder/TEXT("reconstruction.json");
    }
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag,bool bButton=false)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if((!bButton&&W->GetTag()==Tag)||(bButton&&W->GetType()==TEXT("SButton")))return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto Result=Find(C->GetChildAt(I),Tag,bButton))return Result;return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto Match=Find(W,Tag))return Match;
        Test->AddError(TEXT("Missing surface control: ")+Tag.ToString());return {};
    }
    void Key(const TSharedPtr<SWidget>& Widget,FKey KeyValue)
    {
        if(!Widget)return;auto& App=FSlateApplication::Get();
        Test->TestTrue(TEXT("Surface command enabled"),Widget->IsEnabled());App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(KeyValue,FModifierKeysState(),0,false,0,0));
        App.ProcessKeyUpEvent(FKeyEvent(KeyValue,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag,FKey KeyValue=EKeys::Enter){Key(FindTag(Tag),KeyValue);}
    void SelectInspector(int32 Index,int32 NextPhase)
    {
        Press(FName(*FString::Printf(TEXT("InspectorTab%d"),Index)));
        // Let the bound switcher expose the selected child before searching it.
        Phase=NextPhase;ResumeFrame=GFrameCounter+2;
    }
    void OpenSurface(int32 NextPhase){MenuNextPhase=NextPhase;SelectInspector(0,100);}
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native surface controls"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size))||Pixels.IsEmpty())return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Retain surface UI evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;double Started=0;int32 Phase=0,Frame=0;
    uint64 RepairReadyFrame=0,ResumeFrame=0;int32 MenuNextPhase=0;
    FStudioCameraState Camera;FString Root,Work,First,Second,Dataset,Case,SourceHash;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVolumeControls,"Studio.VolumeControls.ImportDisplayRelocateRepairRemove",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioVolumeControls::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioVolumeControlsCommand(this));return true;}
#endif
