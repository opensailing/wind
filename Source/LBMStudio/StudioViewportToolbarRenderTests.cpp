#include "StudioScene.h"
#include "StudioSnapshot.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
// Packaged renderer and routed Slate controls. This is not an OS input test.
class FStudioViewportToolbarCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioViewportToolbarCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(FPlatformTime::Seconds()-Started>100){Test->AddError(FString::Printf(TEXT("Viewport toolbar timed out in phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;const auto* Target=Scene->GetRenderTarget();
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||!Scene->HasCurrentFrame()||!Target||GFrameCounter-ChangedFrame<4||
            Scene->PresentedViewportSize()!=FIntPoint(Target->SizeX,Target->SizeY))return false;
        switch(Phase)
        {
        case 0:
        {
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/ViewportToolbar");Work=Root/FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*Work,true);FString Error;Workspace=M.Workspace;Expanded=M.bViewportExpanded;
            Test->TestTrue(TEXT("Preserve preceding project"),StudioProjectIO::Save(Work/TEXT("original.lbms"),M.SnapshotProject(),Error));
            M.NewProject(TEXT("Airfoil · mesh inspection"));M.Navigate(EStudioWorkspace::Solve);M.Pause();M.bViewportExpanded=false;
            M.EditView(TEXT("Inspect original mesh"),[](auto& S)
            {S.Display.bVectors=false;S.Display.bStreamlines=false;S.Display.bVolume=false;S.Display.bCutPlane=true;S.Display.ScalarField=TEXT("pressure");S.Display.MeshStyle=0;});
            Scene->AlignCamera(FIntVector(0,-1,0));Scene->FitCamera();Next();break;
        }
        case 1:
            Frame=M.SelectedFrame;Camera=Scene->SavedCameraState();Case=StudioCaseIO::Serialize(M.Project.Draft);
            Capture(TEXT("field.png"));ReadPixels(BasePixels);OpenMenu(TEXT("ViewMesh"));Next();break;
        case 2:Capture(TEXT("mesh-menu.png"));Press(TEXT("MeshStyle1"));FSlateApplication::Get().DismissAllMenus();Next();break;
        case 3:
            VerifyMesh();VerifyIsolation();TestPixelsChanged(BasePixels,TEXT("Overlay adds visible original edges"));
            Capture(TEXT("overlay.png"));Test->TestTrue(TEXT("Mesh mode undo"),M.UndoView());Next();break;
        case 4:
            Test->TestEqual(TEXT("Undo removes edge mesh"),Scene->PresentedMesh().Triangles,0);
            Test->TestTrue(TEXT("Mesh mode redo"),M.RedoView());Next();break;
        case 5:OpenMenu(TEXT("ViewMesh"));Next();break;
        case 6:Press(TEXT("MeshStyle2"));FSlateApplication::Get().DismissAllMenus();Next();break;
        case 7:
        {
            VerifyMesh();VerifyIsolation();Capture(TEXT("edges.png"));
            const auto Mesh=Scene->FindComponentByClass<UProceduralMeshComponent>();const auto Fill=Mesh?Mesh->GetProcMeshSection(2):nullptr;
            Test->TestTrue(TEXT("Edges-only hides the scalar cut-plane fill"),!Fill||Fill->ProcVertexBuffer.IsEmpty());
            Test->TestEqual(TEXT("Mesh display preserves selected scalar"),Scene->PresentedScalar().Id,FString(TEXT("pressure")));
            FStudioSnapshot Snapshot;Snapshot.Options.Size=FIntPoint(960,540);FString Error;TArray64<uint8> PNG;
            Test->TestTrue(*Error,Scene->CaptureSnapshot(Snapshot,nullptr,Error));
            Test->TestTrue(TEXT("Snapshot captures mesh identity"),Snapshot.Mesh.Triangles==Scene->PresentedMesh().Triangles&&!Snapshot.Mesh.bDerived);
            Test->TestTrue(TEXT("Snapshot reports hidden scalar fill"),Snapshot.Mesh.bFieldFillHidden);
            if(Test->TestTrue(*Error,StudioSnapshot::Encode(Snapshot,PNG,Error)))FFileHelper::SaveArrayToFile(PNG,*(Root/TEXT("mesh-snapshot.png")));
            Test->TestTrue(TEXT("Save wireframe view"),M.SaveProject(Work/TEXT("mesh.lbms")));
            M.NewProject(TEXT("Intervening project"));Test->TestTrue(TEXT("Reopen wireframe view"),M.RequestProjectOpen(Work/TEXT("mesh.lbms")));Next();break;
        }
        case 8:
            Test->TestEqual(TEXT("Mesh mode reopens"),M.MeshStyle,2);VerifyMesh();Camera=Scene->SavedCameraState();Camera.FieldOfView=48.123456789;Camera.OrthoWidth=6.123456789;
            Scene->RestoreCamera(Camera,TEXT("Exact lens test"));Press(TEXT("ViewProjection"));Next();break;
        case 9:
        {
            auto Expected=Camera;Expected.bOrthographic=!Expected.bOrthographic;
            Test->TestTrue(TEXT("Projection changes retain exact transform and focus"),StudioView::CameraEquals(Expected,Scene->SavedCameraState()));
            Capture(TEXT("projection.png"));Press(TEXT("ViewProjection"));Press(TEXT("ViewExpand"));Next();break;
        }
        case 10:
            Test->TestTrue(TEXT("Expand retains the camera"),M.bViewportExpanded&&StudioView::CameraEquals(Camera,Scene->SavedCameraState()));
            Capture(TEXT("expanded.png"));Press(TEXT("ViewExpand"));OpenMenu(TEXT("ViewportSettings"));Next();break;
        case 11:
            Test->TestFalse(TEXT("Restore reveals the working layout"),M.bViewportExpanded);
            Test->TestTrue(TEXT("Depth clipping has one accessible camera owner"),FindTag(TEXT("CameraDepthClipping")).IsValid());
            Capture(TEXT("camera-settings.png"));FSlateApplication::Get().DismissAllMenus();
            Test->TestTrue(TEXT("Import original point output"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json")));Next();break;
        case 12:OpenMenu(TEXT("ViewMesh"));Next();break;
        case 13:
            Test->TestEqual(TEXT("Original point dataset has no fabricated mesh"),Scene->PresentedMesh().Triangles,0);
            if(const auto W=FindTag(TEXT("MeshStyle1")))Test->TestFalse(TEXT("Unavailable topology is disabled"),W->IsEnabled());
            Capture(TEXT("no-topology.png"));FSlateApplication::Get().DismissAllMenus();
            Test->TestTrue(TEXT("Attach verified derived topology"),M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture/reconstruction.json")));Next();break;
        case 14:
            M.EditView(TEXT("Derived mesh overlay"),[](auto& S){S.Display.bReconstructedSurface=true;S.Display.MeshStyle=1;});Scene->FitCamera();Next();break;
        case 15:
            VerifyMesh();Test->TestTrue(TEXT("Derived topology clearly identified"),Scene->PresentedMesh().bDerived);Capture(TEXT("derived-overlay.png"));
            OpenMenu(TEXT("OrientationViews"));Next();break;
        case 16:
            Capture(TEXT("views.png"));FSlateApplication::Get().DismissAllMenus();
            Press(TEXT("ViewFit"));Test->TestTrue(TEXT("Restore preceding project"),M.RequestProjectOpen(Work/TEXT("original.lbms")));Next();break;
        case 17:M.bViewportExpanded=Expanded;M.SaveSession();M.Navigate(Workspace);return true;
        }
        return false;
    }
private:
    void Next(){++Phase;ChangedFrame=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag,bool Button=false)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if((!Button&&W->GetTag()==Tag)||(Button&&W->GetType()==TEXT("SButton")))return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Find(Children->GetChildAt(I),Tag,Button))return Found;return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto Found=Find(W,Tag))return Found;
        Test->AddError(TEXT("Missing viewport control: ")+Tag.ToString());return {};
    }
    void Enter(const TSharedPtr<SWidget>& W)
    {
        if(!W)return;auto& App=FSlateApplication::Get();Test->TestTrue(TEXT("Control enabled"),W->IsEnabled());
        App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag){Enter(FindTag(Tag));}
    void OpenMenu(FName Tag){if(auto W=FindTag(Tag))Enter(Find(W.ToSharedRef(),NAME_None,true));}
    void VerifyMesh()
    {
        const auto Component=Scene->FindComponentByClass<UProceduralMeshComponent>();const auto Field=Scene->PresentedField();
        const auto Section=Component?Component->GetProcMeshSection(9):nullptr;
        if(!Test->TestTrue(TEXT("Actual triangle mesh rendered"),Section&&Field&&Scene->PresentedMesh().Triangles>0))return;
        if(!Test->TestEqual(TEXT("All supplied faces reach the GPU mesh"),Section->ProcVertexBuffer.Num(),Field->MeshTriangleCount()*3))return;
        for(int32 Face=0;Face<Field->MeshTriangleCount();++Face)
        {
            FVector P[3];if(!Field->MeshTriangle(Face,P)){Test->AddError(TEXT("Missing verified triangle"));return;}
            for(int32 I=0;I<3;++I)
                if(!FVector(Section->ProcVertexBuffer[Face*3+I].Position).Equals(P[I]*100.,1.e-4))
                {Test->AddError(TEXT("Rendered topology changed source positions"));return;}
        }
    }
    void VerifyIsolation()
    {
        const auto& M=*Scene->Model;Test->TestEqual(TEXT("Mesh controls retain original frame"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Mesh controls retain case configuration"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        Test->TestTrue(TEXT("Mesh controls do not reposition camera"),StudioView::CameraEquals(Camera,Scene->SavedCameraState()));
    }
    void ReadPixels(TArray<FColor>& Pixels)
    {Test->TestTrue(TEXT("Read actual renderer pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels));}
    void TestPixelsChanged(const TArray<FColor>& Before,const TCHAR* Label)
    {
        TArray<FColor> After;ReadPixels(After);int32 Different=0;
        if(Before.Num()==After.Num())for(int32 I=0;I<After.Num();++I)Different+=Before[I]!=After[I];
        Test->TestTrue(Label,Different>100);
    }
    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native toolbar UI"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save viewport evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Work,Case;
    EStudioWorkspace Workspace=EStudioWorkspace::Solve;bool Expanded=false;
    FStudioCameraState Camera;TArray<FColor> BasePixels;double Started=0;uint64 ChangedFrame=0;int32 Phase=0,Frame=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioViewportToolbarRender,"Studio.ViewportToolbar.ControlsAndTopology",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioViewportToolbarRender::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioViewportToolbarCommand(this));return true;}
#endif
