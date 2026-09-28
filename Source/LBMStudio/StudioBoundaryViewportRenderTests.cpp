#include "StudioScene.h"
#include "StudioAuthoringTestCapture.h"
#include "StudioBoundarySelection.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioBoundaryViewportCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioBoundaryViewportCommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioBoundaryViewportCommand(){if(bCapturedTooltips&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(FPlatformTime::Seconds()-Started>100){Test->AddError(FString::Printf(TEXT("Boundary viewport workflow timed out at phase %d"),Phase));return true;}
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
            M.NewProject(TEXT("Boundary selection fixture"));M.Pause();FlowCamera=M.Project.Camera;Frame=M.SelectedFrame;Intent=M.RenderIntentRevision;
            // Explicit geometric automation fixture; no scientific output is generated.
            FString Source=TEXT("# Boundary selection test geometry; no CFD\n");
            for(int32 I=0;I<260;++I)
            {
                if(I)Source+=FString::Printf(TEXT("g Patch %03d\n"),I);
                Source+=FString::Printf(TEXT("v %.6f -0.8 -0.8\nv %.6f 0.8 -0.8\nv %.6f 0 0.8\nf %d %d %d\n"),
                    I*.002,I*.002,I*.002,I*3+1,I*3+2,I*3+3);
            }
            const FString Path=Work/TEXT("boundary-selection.obj");
            Test->TestTrue(TEXT("Write labeled geometry fixture"),FFileHelper::SaveStringToFile(Source,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test->TestTrue(TEXT("Import original geometry fixture"),M.RequestGeometryImport(Path));Next();break;
        }
        case 1:
            if(M.IsReadingGeometry())return false;
            M.ImportOptions.MetersPerUnit=1.;M.ImportOptions.Name=TEXT("Selection test surfaces");
            if(!Test->TestTrue(TEXT("Commit original patch topology"),M.CommitGeometryImport()))return true;
            if(!Test->TestEqual(TEXT("All original patches imported"),M.Project.Draft.Geometry[0].Patches.Num(),260))return true;
            for(const auto& Patch:M.Project.Draft.Geometry[0].Patches)Patches.Add(Patch.Id);
            SavedCase=StudioCaseIO::Serialize(M.Project.Draft);Press(TEXT("Workspace5"));Next();break;
        case 2:
            if(M.IsReadingDomainGeometry()||!Scene->HasBoundaryPreview())return false;
            if(!Test->TestTrue(TEXT("Original source verified for selection"),M.DomainGeometry&&M.DomainGeometry->Complete()))return true;
            Test->TestEqual(TEXT("Original triangle-to-patch mapping complete"),M.DomainGeometry->TriangleTargets.Num(),260);
            Observer=Scene->CameraState();Observer.Position=FVector(-3,0,0);Observer.Focus=FVector(.25,0,0);Observer.Orientation=FQuat::Identity;
            Observer.OrbitDistance=3.25;Observer.FieldOfView=48;Observer.OrthoWidth=3;Observer.bOrthographic=false;Observer.bFreeCamera=false;
            Observer.bDepthClipping=true;Observer.NearClipMeters=.05;Observer.FarClipMeters=10;
            Scene->RestoreCamera(Observer,TEXT("Boundary fixture view"));Press(TEXT("BoundaryNextPage"));Next();break;
        case 3:
            Test->TestTrue(TEXT("Next page exposes original patch 122"),FindTag(TargetTag(Patches[122])).IsValid());
            Test->TestFalse(TEXT("Previous page rows are released"),FindTag(TargetTag(Patches[0]),false).IsValid());
            Press(TEXT("BoundaryNextPage"));Next();break;
        case 4:
            Test->TestTrue(TEXT("Last page reaches final original patch"),FindTag(TargetTag(Patches.Last())).IsValid());
            Type(TEXT("BoundaryFilter"),TEXT("Patch 259"));Next();break;
        case 5:
            Test->TestTrue(TEXT("Filtering reaches a patch outside initial page"),FindTag(TargetTag(Patches.Last())).IsValid());
            Press(TargetTag(Patches.Last()));Type(TEXT("BoundaryFilter"),TEXT(""));Next();break;
        case 6:
            if(!Scene->HasBoundaryPreview())return false;
            Test->TestEqual(TEXT("List selects exact original patch"),M.SelectedBoundaryTarget,Patches.Last());
            Test->TestTrue(TEXT("Read pixels before front-surface selection"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(BeforePixels));
            ClickWorld(FVector(0,.18,-.22));Next();break;
        case 7:
        {
            if(!Scene->HasBoundaryPreview())return false;
            Test->TestEqual(TEXT("Routed click picks nearest original surface through domain box"),M.SelectedBoundaryTarget,Patches[0]);
            TArray<FColor> After;Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(After);
            Test->TestTrue(TEXT("Actual selected surface highlight changes rendered pixels"),BeforePixels!=After);BeforePixels.Reset();
            Test->TestFalse(TEXT("Geometry preview makes no recorded-field claim"),Scene->HasPresentedFrame());Capture(TEXT("patch-selected.png"));
            BeforeOrbit=Scene->CameraState();Press(TEXT("BoundaryOrbitMode"));Next();break;
        }
        case 8:DragView();Next();break;
        case 9:
            if(!Scene->HasBoundaryPreview())return false;
            Test->TestFalse(TEXT("Orbit mode moves authoring camera"),StudioView::CameraEquals(BeforeOrbit,Scene->CameraState()));
            Test->TestEqual(TEXT("Orbit gesture does not select another patch"),M.SelectedBoundaryTarget,Patches[0]);
            Test->TestTrue(TEXT("Authoring camera leaves flow camera exact"),StudioView::CameraEquals(FlowCamera,M.Project.Camera));Capture(TEXT("patch-orbit.png"));
            Press(TEXT("BoundaryProjection"));Next();break;
        case 10:
            if(!Scene->HasBoundaryPreview())return false;
            Test->TestTrue(TEXT("Projection control changes authoring view"),Scene->CameraState().bOrthographic);
            Observer.bOrthographic=true;Scene->RestoreCamera(Observer,TEXT("Orthographic fixture view"));Press(TEXT("BoundarySelectMode"));Next();break;
        case 11:
            if(!Scene->HasBoundaryPreview())return false;
            ClickWorld(StudioDomain::FaceCenter(M.Project.Draft.Domain,3));Next();break;
        case 12:
            if(!Scene->HasBoundaryPreview())return false;
            Test->TestEqual(TEXT("Visible handle selects stable positive Y face"),M.SelectedBoundaryTarget,M.Project.Draft.Domain.Faces[3]);Capture(TEXT("boundary-face-handle.png"));
            Test->TestTrue(TEXT("Set explicit near/far clipping"),Scene->SetDepthClipping(true,3.101,3.4));Next();break;
        case 13:
            if(!Scene->HasBoundaryPreview())return false;
            ClickWorld(FVector(.102,.18,-.22));Next();break;
        case 14:
            if(!Scene->HasBoundaryPreview())return false;
            Test->TestEqual(TEXT("Clipped originals cannot intercept visible patch"),M.SelectedBoundaryTarget,Patches[51]);Capture(TEXT("boundary-clipped-selection.png"));
            Test->TestEqual(TEXT("Camera and selection preserve exact case"),StudioCaseIO::Serialize(M.Project.Draft),SavedCase);
            Test->TestEqual(TEXT("Source frame retained"),M.SelectedFrame,Frame);Test->TestEqual(TEXT("No CFD requests from authoring controls"),M.RenderIntentRevision,Intent);
            Press(TEXT("Workspace7"));Next();break;
        case 15:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Returning to Solve restores actual flow"),Scene->HasPresentedFrame());
            Test->TestTrue(TEXT("Saved flow camera restored exactly"),StudioView::CameraEquals(FlowCamera,M.Project.Camera));Press(TEXT("Workspace5"));Next();break;
        case 16:
            if(!Scene->HasBoundaryPreview())return false;
            Test->TestTrue(TEXT("Returning retains authoring projection"),Scene->CameraState().bOrthographic);
            Test->TestEqual(TEXT("Returning retains authoring near plane"),Scene->CameraState().NearClipMeters,3.101);
            Test->TestEqual(TEXT("Returning retains selected patch"),M.SelectedBoundaryTarget,Patches[51]);Capture(TEXT("boundary-view-return.png"));
            Test->TestTrue(TEXT("Restore preceding project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 17:M.Navigate(Previous);return true;
        }
        return false;
    }
private:
    static FName TargetTag(const FGuid& Id){return FName(*(TEXT("BoundaryTarget_")+Id.ToString()));}
    void Next(){++Phase;ChangedFrame=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& Widget,FName Tag)
    {
        if(!Widget->GetVisibility().IsVisible())return {};
        if(Widget->GetTag()==Tag)return Widget;
        auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Result=Find(Children->GetChildAt(I),Tag))return Result;
        return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag,bool Required=true)
    {
        const auto Window=GEngine->GameViewport->GetWindow();const auto Widget=Window?Find(Window.ToSharedRef(),Tag):TSharedPtr<SWidget>();
        if(Required&&!Widget)Test->AddError(TEXT("Missing boundary control: ")+Tag.ToString());return Widget;
    }
    void Press(FName Tag)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Type(FName Tag,const FString& Value)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        const FModifierKeysState All(false,false,true,false,false,false,false,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,All,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,All,0,false,0,0));
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::BackSpace,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::BackSpace,FModifierKeysState(),0,false,0,0));
        for(TCHAR C:Value)App.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
    }
    void ClickWorld(const FVector& World)
    {
        const auto View=FindTag(TEXT("BoundaryViewport"));if(!View)return;const auto& G=View->GetCachedGeometry();const auto Size=Scene->PresentedViewportSize();FVector2D Local;
        if(!Test->TestTrue(TEXT("Visible source point projects"),StudioCameraPlacement::Project(Scene->PresentedCamera(),G.GetLocalSize(),World,Local,double(Size.X)/Size.Y)))return;
        Pointer(View,G.LocalToAbsolute(Local),FVector2D::ZeroVector);
    }
    void DragView()
    {
        const auto View=FindTag(TEXT("BoundaryViewport"));if(!View)return;const auto& G=View->GetCachedGeometry();
        Pointer(View,G.LocalToAbsolute(G.GetLocalSize()*.55),FVector2D(32,16));
    }
    void Pointer(const TSharedPtr<SWidget>& View,FVector2D Start,FVector2D Delta)
    {
        auto& App=FSlateApplication::Get();const auto Hit=App.LocateWindowUnderMouse(Start,App.GetInteractiveTopLevelWindows(),false,0);
        if(!Test->TestTrue(TEXT("Original geometry viewport receives routed pointer"),Hit.ContainsWidget(View.Get())))return;
        const TSet<FKey> Down={EKeys::LeftMouseButton};const FVector2D End=Start+Delta;
        App.ProcessMouseButtonDownEvent(GEngine->GameViewport->GetWindow()->GetNativeWindow(),FPointerEvent(0,Start,Start,Down,EKeys::LeftMouseButton,0,FModifierKeysState()));
        if(!Delta.IsNearlyZero())App.ProcessMouseMoveEvent(FPointerEvent(0,End,Start,Down,FKey(),0,FModifierKeysState()));
        App.ProcessMouseButtonUpEvent(FPointerEvent(0,End,End,TSet<FKey>(),EKeys::LeftMouseButton,0,FModifierKeysState()));
    }
    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture boundary viewport evidence"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Retain native boundary viewport evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Work,SavedCase;
    TArray<FGuid> Patches;TArray<FColor> BeforePixels;FStudioCameraState FlowCamera,Observer,BeforeOrbit;EStudioWorkspace Previous=EStudioWorkspace::Solve;
    bool bTooltips=true,bCapturedTooltips=false;int32 Phase=0,Frame=0;uint64 ChangedFrame=0,Intent=0;double Started=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioBoundaryViewportRender,"Studio.Boundaries.SourcePickingPaginationAndCameraIsolation",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioBoundaryViewportRender::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioBoundaryViewportCommand(this));return true;}
#endif
