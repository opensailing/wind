#include "StudioScene.h"
#include "StudioFileDialog.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioExternalRecordingRenderCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioExternalRecordingRenderCommand(FAutomationTestBase* T):Test(T){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>45){Test->AddError(TEXT("External recording UI acceptance timed out"));return true;}
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
            if(M.State==EStudioRunState::Running)M.Pause();
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/RecordingUI");
            M.SetControlHarness(false);Original=M.SnapshotProject();Original.Camera=Scene->SavedCameraState();
            FString Error;Test->TestTrue(TEXT("Save previous view"),StudioProjectIO::Save(Root/TEXT("original.lbms"),Original,Error));
            Folder=Root/TEXT("recording");Moved=Root/TEXT("moved");
            IFileManager::Get().MakeDirectory(*Folder,true);IFileManager::Get().MakeDirectory(*Moved,true);
            const FString Source=FPaths::ProjectContentDir()/TEXT("Samples/MeshGraphNets_Airfoil_test010");
            Test->TestEqual(TEXT("Copy actual CFD payload"),IFileManager::Get().Copy(*(Folder/TEXT("flow.bin")),*(Source/TEXT("flow.bin"))),COPY_OK);
            FString Text;TSharedPtr<FJsonObject> Meta;
            FFileHelper::LoadFileToString(Text,*(Source/TEXT("recording.json")));
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Meta);
            if(!Meta){Test->AddError(TEXT("Source metadata missing"));return true;}
            Meta->SetStringField(TEXT("id"),TEXT("external-ui-SU2-010"));Meta->SetStringField(TEXT("title"),TEXT("External SU2 010"));
            auto Vector=[&](const TCHAR* Key,FVector V){TArray<TSharedPtr<FJsonValue>> A;for(int32 I=0;I<3;++I)A.Add(MakeShared<FJsonValueNumber>(V[I]));Meta->SetArrayField(Key,A);};
            // A display translation/crop proves general bounds without changing
            // any physical source coordinates, field values or source times.
            Vector(TEXT("sourceOffset"),FVector(9.5,0,5));
            Bounds=FBox(FVector(8.5,1.2,4.2),FVector(12.9,2.8,5.8));
            Vector(TEXT("displayMin"),Bounds.Min);Vector(TEXT("displayMax"),Bounds.Max);
            Text.Empty();FJsonSerializer::Serialize(Meta,TJsonWriterFactory<>::Create(&Text));
            Test->TestTrue(TEXT("Write translated display metadata"),FFileHelper::SaveStringToFile(Text,*(Folder/TEXT("recording.json"))));
            for(const TCHAR* File:{TEXT("flow.bin"),TEXT("recording.json")})
                Test->TestEqual(TEXT("Copy matching relocation pair"),IFileManager::Get().Copy(*(Moved/File),*(Folder/File)),COPY_OK);
            OpenSelector(1);break;
        }
        case 1:
            Capture(TEXT("source-menu.png"));
            StudioFileDialog::SetNextRecordingFolderForAutomation(Folder);Press(TEXT("Import recording…"));Phase=2;break;
        case 2:
            if(M.IsRecordingLoadPending()||!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Routed import commits verified source"),M.Project.Dataset,FString(TEXT("external-ui-SU2-010")));
            Test->TestTrue(TEXT("Import retains exact camera"),StudioView::CameraEquals(M.Project.Camera,Original.Camera));
            Test->TestEqual(TEXT("Translated renderer uses descriptor minimum"),Scene->GetRenderedFlowBounds().Min,Bounds.Min);
            Test->TestEqual(TEXT("Translated renderer uses descriptor maximum"),Scene->GetRenderedFlowBounds().Max,Bounds.Max);
            Test->TestEqual(TEXT("Imported provenance remains literal"),M.Project.Runs.Last().GetOrigin(),EStudioRunOrigin::ImportedRecording);
            M.EditView(TEXT("Place translated slice"),[&](auto& V){V.Display.SliceAxis=1;V.Display.SlicePosition=2;});
            Route(Find(GEngine->GameViewport->GetWindow().ToSharedRef(),FString(),TEXT("ViewFit")));Phase=3;break;
        case 3:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Fit centers actual bounds"),Scene->CameraState().Focus.Equals(Bounds.GetCenter(),1.e-8));
            Test->TestEqual(TEXT("Camera operation retains source frame"),M.SelectedFrame,0);
            Test->TestTrue(TEXT("Translated field snapshot"),Scene->Snapshot(Root/TEXT("translated-field.png")));
            Capture(TEXT("imported-fit.png"));OpenSelector(4);break;
        case 4:
            Capture(TEXT("external-menu.png"));
            StudioFileDialog::SetNextRecordingFolderForAutomation(Moved);Press(TEXT("Locate…"));Phase=5;break;
        case 5:
            if(M.IsRecordingLoadPending()||!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Routed Locate updates external location"),FPaths::IsSamePath(M.Project.Recordings.Last().Path,Moved/TEXT("flow.bin")));
            M.Scrub(.4);Phase=6;break;
        case 6:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Save external view"),M.SaveProject(Root/TEXT("external.lbms")));
            Saved=M.SnapshotProject();
            Test->TestTrue(TEXT("Remove only test-owned moved payload"),IFileManager::Get().Delete(*(Moved/TEXT("flow.bin"))));
            Test->TestTrue(TEXT("Missing source open begins"),M.RequestProjectOpen(Root/TEXT("external.lbms")));Phase=7;break;
        case 7:
            if(M.IsProjectOpenPending())return false;
            Test->TestTrue(TEXT("Repair action offered"),M.RecordingRepair.IsSet());Phase=70;break;
        case 70:
            // The banner changes viewport height. Let Slate finish layout and
            // the on-demand target resize/capture before recording evidence.
            if(++SettlingFrames<3||!Scene->HasCurrentFrame())return false;
            Capture(TEXT("missing-recording.png"));
            StudioFileDialog::SetNextRecordingFolderForAutomation(Folder);Press(TEXT("Locate recording…"));Phase=8;break;
        case 8:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            Test->TestFalse(TEXT("Repair prompt clears"),M.RecordingRepair.IsSet());
            Test->TestEqual(TEXT("Source frame restored after repair"),Scene->PresentedFrame().Index,240);
            Test->TestTrue(TEXT("Camera restored after repair"),StudioView::CameraEquals(Scene->SavedCameraState(),Saved.Camera));
            Test->TestTrue(TEXT("Relocated path needs saving"),M.HasUnsavedChanges());Capture(TEXT("repaired.png"));
            Test->TestTrue(TEXT("Restore previous test view"),M.RequestProjectOpen(Root/TEXT("original.lbms")));Phase=9;break;
        case 9:return !M.IsProjectOpenPending()&&Scene->HasCurrentFrame();
        case 100:
        {
            const auto Selector=Find(GEngine->GameViewport->GetWindow().ToSharedRef(),FString(),TEXT("RecordingSelector"));
            if(!Selector){Test->AddError(TEXT("Recording selector unavailable"));return true;}
            Route(Find(Selector.ToSharedRef(),FString()));
            Phase=MenuNextPhase;ResumeFrame=GFrameCounter+2;break;
        }
        }
        return false;
    }
private:
    bool HasLabel(const TSharedRef<SWidget>& W,const FString& Label)
    {
        if(W->GetType()==TEXT("STextBlock")&&StaticCastSharedRef<STextBlock>(W)->GetText().ToString()==Label)return true;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(HasLabel(C->GetChildAt(I),Label))return true;return false;
    }
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,const FString& Label,FName Tag=NAME_None)
    {
        if(!W->GetVisibility().IsVisible())return nullptr;
        if(!Tag.IsNone()&&W->GetTag()==Tag)return W;
        if(Tag.IsNone()&&W->GetType()==TEXT("SButton")&&(Label.IsEmpty()||HasLabel(W,Label)))return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto Found=Find(C->GetChildAt(I),Label,Tag))return Found;
        return nullptr;
    }
    void Route(const TSharedPtr<SWidget>& Button)
    {
        if(!Test->TestTrue(TEXT("Routed UI button found"),Button.IsValid()))return;
        Test->TestTrue(TEXT("Routed UI button enabled"),Button->IsEnabled());
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Button,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
        App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(const FString& Label)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto Button=Find(W,Label)){Route(Button);return;}
        Test->AddError(TEXT("UI button not found: ")+Label);
    }
    void OpenSelector(int32 NextPhase)
    {
        Route(Find(GEngine->GameViewport->GetWindow().ToSharedRef(),FString(),TEXT("InspectorTab0")));
        // The selected inspector is exposed after Slate evaluates its attribute.
        MenuNextPhase=NextPhase;Phase=100;ResumeFrame=GFrameCounter+2;
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        Test->TestTrue(TEXT("Capture native Slate window"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        if(Pixels.IsEmpty())return;TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Write UI evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;
    FStudioProject Original,Saved;FString Root,Folder,Moved;FBox Bounds=FBox(ForceInit);
    int32 Phase=0,SettlingFrames=0,MenuNextPhase=0;double Started=0;uint64 ResumeFrame=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioExternalRecordingRenderTest,"Studio.Rendering.ExternalRecordingControlsAndBounds",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioExternalRecordingRenderTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioExternalRecordingRenderCommand(this));return true;}
#endif
