#include "StudioScene.h"
#include "StudioFlowPresentation.h"
#include "StudioAutomationForeground.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Widgets/SWindow.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Uses the complete externally supplied recording. This is a bounded interaction
 * and ordinary-clock playback check, not a long-session stability claim. */
class FStudioFocusedWingCommand final : public IAutomationLatentCommand
{
public:
    FStudioFocusedWingCommand(FAutomationTestBase* T,FString R,FString S):Test(T),Recording(MoveTemp(R)),Surface(MoveTemp(S)){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Foreground.WasInterrupted()){Test->AddError(Foreground.Describe(Phase));return true;}
        if(Now-Started>150){Test->AddError(FString::Printf(TEXT("Focused wing timed out in phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& App=FSlateApplication::Get();
        if(!App.IsActive())
        {
            if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}
            Changed=GFrameCounter;return false;
        }
        auto& M=*Scene->Model;
        if(Phase==15)
        {
            if(Scene->HasPresentedFrame())Frames.Add(Scene->PresentedFrame().Index);
            if(Now-PlayStart<8)return false;
            M.Pause();Test->TestTrue(TEXT("Ordinary-clock playback presents distinct original frames"),Frames.Num()>20);
            Test->AddInfo(FString::Printf(TEXT("Full-recording focused playback: %d distinct original frames in %.3f wall seconds"),Frames.Num(),Now-PlayStart));
            Phase=16;Changed=GFrameCounter;return false;
        }
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||!Scene->HasCurrentFrame()||GFrameCounter-Changed<4)return false;
        const auto Target=Scene->GetRenderTarget();
        if(!Target||Scene->PresentedViewportSize()!=FIntPoint(Target->SizeX,Target->SizeY))return false;
        switch(Phase)
        {
        case 0:
        {
            Foreground.Begin();Root=FPaths::ProjectSavedDir()/TEXT("Automation/FocusedWing");
            Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;Test->TestTrue(TEXT("Retain current project"),StudioProjectIO::Save(Work/TEXT("original.lbms"),M.SnapshotProject(),Error));
            M.NewProject(TEXT("NACA 0018 · recorded wing flow"));
            Test->TestTrue(TEXT("Open complete authentic recording"),M.RequestExternalRecording(Recording));break;
        }
        case 1:
            Test->TestEqual(TEXT("Full source has 8,000 original snapshots"),M.Solver->Descriptor().Frames.Num(),8000);
            Test->TestTrue(TEXT("Full source spans nearly twenty physical seconds"),M.Solver->Descriptor().Frames.Last().Time-M.Solver->Descriptor().Frames[0].Time>19.99);
            Test->TestTrue(TEXT("Attach explicit verified reconstruction"),M.RequestReconstruction(Surface));break;
        case 2:
            M.Scrub(.5);M.EditView(TEXT("Recorded speed"),[](auto& S){S.Display.ScalarField=TEXT("velocity_magnitude");});break;
        case 3:
            Frame=M.SelectedFrame;Source=M.Project.Dataset;Case=StudioCaseIO::Serialize(M.Project.Draft);
            Press(TEXT("FlowOverview"));break;
        case 4:
            Focus=M.InspectionState();Region=Scene->GetRenderedFlowBounds();
            if(const auto Mesh=Scene->FindComponentByClass<UProceduralMeshComponent>())
                if(const auto Material=Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(2)))
                    {float Opacity=0;Test->TestTrue(TEXT("Focused scalar surface leaves velocity traces visible"),Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("SurfaceOpacity")),Opacity)&&Opacity>0&&Opacity<.3);}
            Test->TestTrue(TEXT("Overview turns on focused reconstruction"),M.bFocusWingRegion&&M.bReconstructedSurface);
            Test->TestTrue(TEXT("Focused flow produces many supported traces"),Scene->PresentedStreams().Lines>30);
            Test->TestTrue(TEXT("Focused bounds are distinct from source extent"),!Region.Equals(M.Solver->Descriptor().DisplayBounds));
            Test->TestTrue(TEXT("Original boundary identity stays unavailable"),Scene->PresentedField()->Boundary().IsEmpty());
            Press(TEXT("InspectorTab3"));break;
        case 5:
            Capture(TEXT("focused.png"));Press(TEXT("FocusWingRegion"),EKeys::SpaceBar);break;
        case 6:
            Test->TestFalse(TEXT("Visible focus checkbox turns crop off"),M.bFocusWingRegion);
            Test->TestTrue(TEXT("Full source bounds return"),Scene->GetRenderedFlowBounds().Equals(M.Solver->Descriptor().DisplayBounds));
            Test->TestTrue(TEXT("Checkbox retains arbitrary camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Focus.Camera));
            Press(TEXT("ViewFit"));break;
        case 7:
            Capture(TEXT("full-field.png"));Test->TestTrue(TEXT("Undo full Fit"),M.UndoView());Test->TestTrue(TEXT("Undo focus toggle"),M.UndoView());break;
        case 8:
            Test->TestTrue(TEXT("Undo restores exact focused view"),M.InspectionState().Equals(Focus));
            Test->TestTrue(TEXT("Focused geometry restored"),Scene->GetRenderedFlowBounds().Equals(Region));
            Scene->Orbit(45,-22);break;
        case 9:
            Test->TestFalse(TEXT("Arbitrary camera orbit changes perspective"),StudioView::CameraEquals(Scene->SavedCameraState(),Focus.Camera));
            Capture(TEXT("orbit.png"));Test->TestTrue(TEXT("Undo arbitrary camera"),M.UndoView());break;
        case 10:
            VerifyIdentity();Test->TestTrue(TEXT("Save focused view"),M.SaveProject(Work/TEXT("focused.lbms")));
            M.NewProject(TEXT("Temporary view"));Test->TestTrue(TEXT("Reopen focus and source"),M.RequestProjectOpen(Work/TEXT("focused.lbms")));break;
        case 11:
            VerifyIdentity();Test->TestTrue(TEXT("Saved focus camera and settings restored"),M.InspectionState().Equals(Focus));
            Test->TestTrue(TEXT("Saved focused bounds restored"),Scene->GetRenderedFlowBounds().Equals(Region));
            Capture(TEXT("reopened.png"));M.Scrub(.8);break;
        case 12:
            Test->TestTrue(TEXT("Later original snapshot reaches renderer"),M.SelectedFrame>Frame);
            Capture(TEXT("later-frame.png"));M.EditView(TEXT("Playback rate"),[](auto& S){S.Display.PlaybackRate=1;});break;
        case 13:
            M.Scrub(.1);break;
        case 14:
            PlayStart=Now;M.Run();break;
        case 16:
            Capture(TEXT("playback.png"));Test->TestTrue(TEXT("Restore preceding project"),M.RequestProjectOpen(Work/TEXT("original.lbms")));break;
        case 17:return true;
        }
        ++Phase;Changed=GFrameCounter;return false;
    }
private:
    void VerifyIdentity()
    {
        const auto& M=*Scene->Model;Test->TestEqual(TEXT("Source stays unchanged"),M.Project.Dataset,Source);
        Test->TestEqual(TEXT("Frame stays unchanged"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Case stays unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    }
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if(W->GetTag()==Tag)return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto R=Find(C->GetChildAt(I),Tag))return R;return {};
    }
    void Press(FName Tag,FKey Key=EKeys::Enter)
    {
        const auto W=Find(GEngine->GameViewport->GetWindow().ToSharedRef(),Tag);
        if(!Test->TestTrue(*FString::Printf(TEXT("Visible control %s"),*Tag.ToString()),W.IsValid()))return;
        Test->TestTrue(TEXT("Control enabled"),W->IsEnabled());auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native focused flow"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Write native evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioAutomationForeground Foreground;
    FString Recording,Surface,Root,Work,Source,Case;int32 Phase=0,Frame=0;uint64 Changed=0;
    double Started=0,LastActivation=0,PlayStart=0;TSet<int32> Frames;
    FStudioInspectionState Focus;FBox Region=FBox(ForceInit);
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFocusedWing,"ScientificAcceptance.Surface.FocusedWing",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioFocusedWing::RunTest(const FString&)
{
    FString Recording,Surface;
    if(!FParse::Value(FCommandLine::Get(),TEXT("StudioPointRecording="),Recording)||!FParse::Value(FCommandLine::Get(),TEXT("StudioSurfaceReconstruction="),Surface))
    {AddError(TEXT("Focused wing acceptance requires the complete recording and explicit reconstruction paths."));return false;}
    ADD_LATENT_AUTOMATION_COMMAND(FStudioFocusedWingCommand(this,Recording,Surface));return true;
}
#endif
