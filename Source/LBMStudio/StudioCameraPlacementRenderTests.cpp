#include "StudioScene.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioCameraPlacementCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioCameraPlacementCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()||Now-Started>60)
        {if(!Test->HasAnyErrors())Test->AddError(FString::Printf(TEXT("Camera placement timed out in phase %d"),Phase));FSlateApplication::Get().DismissAllMenus();return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame()||GFrameCounter-ChangedFrame<3)return false;
        auto Expected=M.Project.Camera;Expected.FieldOfView=float(Expected.FieldOfView);Expected.OrthoWidth=double(float(Expected.OrthoWidth*100))/100;
        if(!StudioView::CameraEquals(Scene->PresentedCamera(),Expected))return false;
        switch(Phase)
        {
        case 0:
        {
            if(M.State==EStudioRunState::Running)M.Pause();Original=M.SnapshotProject();OriginalWorkspace=M.Workspace;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/CameraPlacement");FString Error;
            Test->TestTrue(TEXT("Preserve original project"),StudioProjectIO::Save(Root/TEXT("original.lbms"),Original,Error));
            auto Candidate=Original;Candidate.Id=FGuid::NewGuid();Candidate.Name=TEXT("Camera placement acceptance");Candidate.Cameras.Reset();
            Candidate.Camera=FStudioCameraState();Candidate.View=FStudioViewSettings();
            Test->TestTrue(TEXT("Create isolated placement document"),StudioProjectIO::Save(Root/TEXT("placement.lbms"),Candidate,Error));
            Test->TestTrue(TEXT("Open isolated placement document"),M.RequestProjectOpen(Root/TEXT("placement.lbms")));Next(1);break;
        }
        case 1:
        {
            M.Navigate(EStudioWorkspace::Solve);Scene->FitCamera();
            auto Saved=Scene->SavedCameraState();
            // Place the observer inside the domain with the actual wing in
            // view. Reusing the overview's orientation at the domain centre
            // aimed out along the span and produced a featureless preview.
            Saved.Position=FVector(.85,.65,.5);Saved.Focus=FVector::ZeroVector;
            Saved.OrbitDistance=(Saved.Focus-Saved.Position).Size();
            Saved.Orientation=(Saved.Focus-Saved.Position).Rotation().Quaternion();
            Test->TestTrue(TEXT("Inspection camera is inside the published display domain"),M.Solver->Descriptor().DisplayBounds.IsInside(Saved.Position));
            Test->TestTrue(TEXT("Prepare inside-domain saved camera"),M.AddCamera(TEXT("Inside the wing domain"),Saved));Id=M.Project.Cameras[0].Id;
            Test->TestTrue(TEXT("Prepare independent overview camera"),M.AddCamera(TEXT("Overview"),Scene->SavedCameraState()));OtherId=M.Project.Cameras[1].Id;
            Before=Saved;Case=StudioCaseIO::Serialize(M.Project.Draft);Source=M.Project.Dataset;Frame=M.SelectedFrame;Playback=M.PlaybackFrame;
            OpenManager();Next(2);break;
        }
        case 2:Press(Tag(TEXT("CameraPlace-"),Id));Next(3);break;
        case 3:
            if(!Test->TestNotNull(TEXT("Routed Place begins draft"),M.CameraPlacement()))return true;
            Test->TestEqual(TEXT("Selected stable camera identity"),M.CameraPlacement()->CameraId,Id);
            Press(TEXT("PlacementFrame"));Next(4);break;
        case 4:
            Observer=Scene->SavedCameraState();Pixels=ReadPixels();CaptureCount=Scene->GetCaptureCount();
            FieldRevision=M.Revision;VerifyHandlesExposed(EStudioCameraPlacementTool::Move);Capture(TEXT("move-before.png"));
            Type(TEXT("PlacementPositionX"),FString::Printf(TEXT("%.15g"),Before.Position.X+.123456789));Next(5);break;
        case 5:
            Test->TestTrue(TEXT("Numeric position edits draft"),FMath::IsNearlyEqual(M.CameraPlacement()->Camera.Position.X,Before.Position.X+.123456789,1.e-12));
            VerifyIsolation();
            PendingY=M.CameraPlacement()->Camera.Position.Y+.0123456789;
            Type(TEXT("PlacementPositionY"),FString::Printf(TEXT("%.15g"),PendingY),false);
            DragHandle(EStudioCameraPlacementTool::Move,true,false);
            Test->TestTrue(TEXT("Numeric focus loss applies the entered value"),FMath::IsNearlyEqual(M.CameraPlacement()->Camera.Position.Y,PendingY,1.e-12));
            Next(21);break;
        case 21:
            VerifyIsolation();DragHandle(EStudioCameraPlacementTool::Move);Next(6);break;
        case 6:
            VerifyIsolation();VerifyHandlesExposed(EStudioCameraPlacementTool::Move);Capture(TEXT("move.png"));Press(TEXT("PlacementRotate"));Next(7);break;
        case 7:VerifyHandlesExposed(EStudioCameraPlacementTool::Rotate);DragHandle(EStudioCameraPlacementTool::Rotate);Next(8);break;
        case 8:
            VerifyIsolation();Placed=M.CameraPlacement()->Camera;Capture(TEXT("rotate.png"));
            Press(TEXT("PlacementPreview"));Next(22);break;
        case 22:
            Test->TestTrue(TEXT("Routed preview reaches viewing camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Placed));
            Test->TestTrue(TEXT("Preview retains the unapplied draft"),M.IsCameraPlacementCurrent()&&StudioView::CameraEquals(M.CameraPlacement()->Camera,Placed));
            Test->TestTrue(TEXT("Preview keeps saved pose unchanged"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Before));
            Test->TestEqual(TEXT("Preview retains source frame"),M.SelectedFrame,Frame);
            Test->TestEqual(TEXT("Preview retains playback cursor"),M.PlaybackFrame,Playback);
            Test->TestEqual(TEXT("Preview retains field geometry"),M.Revision,FieldRevision);
            Test->TestEqual(TEXT("Preview retains case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
            VerifySceneDetail();
            Capture(TEXT("preview.png"));Test->TestTrue(TEXT("Undo preview restores observer"),M.UndoView());Next(23);break;
        case 23:
        {
            Test->TestTrue(TEXT("Undo preview reaches exact previous observer"),StudioView::CameraEquals(Scene->SavedCameraState(),Observer));
            const auto Restored=ReadPixels();Test->TestEqual(TEXT("Preview return retains render size"),Restored.Num(),Pixels.Num());
            int32 Changed=0;for(int32 I=0;I<FMath::Min(Restored.Num(),Pixels.Num());++I)
                if(Restored[I].R!=Pixels[I].R||Restored[I].G!=Pixels[I].G||Restored[I].B!=Pixels[I].B)++Changed;
            Test->TestEqual(TEXT("Undo preview restores the previous CFD image"),Changed,0);
            Pixels=Restored;CaptureCount=Scene->GetCaptureCount();VerifyIsolation();
            Press(TEXT("PlacementApply"));Next(9);break;
        }
        case 9:
            Test->TestNull(TEXT("Apply closes placement inspector"),M.CameraPlacement());
            Test->TestTrue(TEXT("Applied camera matches manipulated draft"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Placed));
            Test->TestTrue(TEXT("One saved-camera undo reverts entire placement"),M.UndoSavedCameras());
            Test->TestTrue(TEXT("Original saved pose restored"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Before));
            Test->TestTrue(TEXT("Redo restores placement"),M.RedoSavedCameras());OpenManager();Next(10);break;
        case 10:Press(Tag(TEXT("CameraPlace-"),OtherId));Next(11);break;
        case 11:
            Other=M.FindCamera(OtherId)->Camera;Type(TEXT("PlacementPositionZ"),TEXT("6.1234567890123"));Next(12);break;
        case 12:
            Press(TEXT("PlacementCancel"));Test->TestTrue(TEXT("Cancel keeps second saved pose exact"),StudioView::CameraEquals(M.FindCamera(OtherId)->Camera,Other));
            VerifyIsolation();M.Run();
            {
                const int32 Selected=M.SelectedFrame,Playing=M.PlaybackFrame;
                Test->TestTrue(TEXT("Activate placed camera during playback"),M.RestoreSavedCamera(Id));
                Test->TestTrue(TEXT("Activation retains running playback"),M.State==EStudioRunState::Running);
                Test->TestEqual(TEXT("Activation retains frame"),M.SelectedFrame,Selected);Test->TestEqual(TEXT("Activation retains playback cursor"),M.PlaybackFrame,Playing);
            }
            M.Pause();Next(13);break;
        case 13:
            Test->TestTrue(TEXT("Placed camera reaches renderer"),StudioView::CameraEquals(Scene->SavedCameraState(),Placed));VerifySceneDetail();Capture(TEXT("activated.png"));
            Test->TestTrue(TEXT("Persist both cameras"),M.SaveProject(Root/TEXT("placement.lbms")));
            Test->TestTrue(TEXT("Reopen placed cameras"),M.RequestProjectOpen(Root/TEXT("placement.lbms")));Next(14);break;
        case 14:
            Test->TestNull(TEXT("Reopen has no transient draft"),M.CameraPlacement());
            Test->TestTrue(TEXT("Placed transform reopens exactly"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Placed));
            Test->TestTrue(TEXT("Independent camera reopens exactly"),StudioView::CameraEquals(M.FindCamera(OtherId)->Camera,Other));
            OpenManager();Next(15);break;
        case 15:Capture(TEXT("reopened.png"));Press(Tag(TEXT("CameraPlace-"),Id));Next(16);break;
        case 16:Press(TEXT("PlacementFrame"));Next(19);break;
        case 19:
        {
            Observer=Scene->SavedCameraState();const auto Initial=M.CameraPlacement()->Camera;
            DragHandle(EStudioCameraPlacementTool::Move,false);
            auto& App=FSlateApplication::Get();
            App.ProcessKeyDownEvent(FKeyEvent(EKeys::Escape,FModifierKeysState(),0,false,0,0));
            App.ProcessKeyUpEvent(FKeyEvent(EKeys::Escape,FModifierKeysState(),0,false,0,0));
            if(!Test->TestNotNull(TEXT("Escape cancels gesture while retaining placement"),M.CameraPlacement()))return true;
            Test->TestTrue(TEXT("Escape restores the gesture's initial draft"),StudioView::CameraEquals(M.CameraPlacement()->Camera,Initial));
            if(const auto W=FindTag(TEXT("FlowViewport")))Test->TestFalse(TEXT("Escape releases pointer capture"),W->HasMouseCapture());
            ReleasePointer();
            DragHandle(EStudioCameraPlacementTool::Move,false);
            if(const auto Control=FindTag(TEXT("PlacementRotate")))App.SetKeyboardFocus(Control,EFocusCause::Navigation);
            if(const auto W=FindTag(TEXT("FlowViewport")))Test->TestFalse(TEXT("Changing focus releases only the viewport capture"),W->HasMouseCapture());
            Test->TestTrue(TEXT("Focus loss retains the most recent valid draft"),!StudioView::CameraEquals(M.CameraPlacement()->Camera,Initial));
            ReleasePointer();
            Next(20);break;
        }
        case 20:
            DragHandle(EStudioCameraPlacementTool::Move,false);
            RetainedDraft=M.CameraPlacement()->Camera;
            Test->TestTrue(TEXT("Rename invalidates captured draft"),M.RenameCamera(Id,TEXT("Renamed independently")));Next(17);break;
        case 17:
            if(const auto W=FindTag(TEXT("FlowViewport")))Test->TestFalse(TEXT("Stale drag releases pointer before another event"),W->HasMouseCapture());
            ReleasePointer();
            Test->TestTrue(TEXT("Stale drag keeps its last valid draft"),StudioView::CameraEquals(M.CameraPlacement()->Camera,RetainedDraft));
            Test->TestTrue(TEXT("Stale drag preserves observer"),StudioView::CameraEquals(Scene->SavedCameraState(),Observer));
            if(const auto Apply=FindTag(TEXT("PlacementApply")))Test->TestFalse(TEXT("Stale Apply is disabled"),Apply->IsEnabled());Capture(TEXT("stale-draft.png"));
            Press(TEXT("PlacementCancel"));FSlateApplication::Get().DismissAllMenus();
            Test->TestTrue(TEXT("Restore original project"),M.RequestProjectOpen(Root/TEXT("original.lbms")));Next(18);break;
        case 18:M.Navigate(OriginalWorkspace);return true;
        }
        return false;
    }
private:
    void Next(int32 P){Phase=P;ChangedFrame=GFrameCounter;}
    static FName Tag(const TCHAR* Prefix,const FGuid& Id){return FName(FString(Prefix)+Id.ToString(EGuidFormats::Digits));}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Name,bool Button=false)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if((!Button&&W->GetTag()==Name)||(Button&&W->GetType()==TEXT("SButton")))return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Result=Find(Children->GetChildAt(I),Name,Button))return Result;return {};
    }
    TSharedPtr<SWidget> FindTag(FName Name)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto Result=Find(W,Name))return Result;
        Test->AddError(TEXT("Missing placement control: ")+Name.ToString());return {};
    }
    void Enter(const TSharedPtr<SWidget>& W)
    {
        if(!W)return;auto& App=FSlateApplication::Get();Test->TestTrue(TEXT("Placement control enabled"),W->IsEnabled());
        if(!W->HasKeyboardFocus()&&!W->HasFocusedDescendants())App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Name){Enter(FindTag(Name));}
    void OpenManager(){if(auto W=FindTag(TEXT("CameraManager")))Enter(Find(W.ToSharedRef(),NAME_None,true));}
    void Type(FName Name,const FString& Value,bool bCommit=true)
    {
        if(auto W=FindTag(Name))
        {
            auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
            const FModifierKeysState SelectAll(false,false,true,false,false,false,false,false,false);
            Test->TestTrue(TEXT("Select existing numeric text"),App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,SelectAll,0,false,0,0)));
            App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,SelectAll,0,false,0,0));
            for(const TCHAR Ch:Value)App.ProcessKeyCharEvent(FCharacterEvent(Ch,FModifierKeysState(),0,false));if(bCommit)Enter(W);
        }
    }
    void DragHandle(EStudioCameraPlacementTool Tool,bool bRelease=true,bool bExpectCapture=true)
    {
        const auto Viewport=FindTag(TEXT("FlowViewport"));if(!Viewport)return;
        auto& M=*Scene->Model;const auto Initial=M.CameraPlacement()->Camera;const auto Geometry=Viewport->GetCachedGeometry();const auto Size=Geometry.GetLocalSize();
        const auto RenderSize=Scene->PresentedViewportSize();const double Aspect=double(RenderSize.X)/RenderSize.Y;
        const auto Lines=StudioCameraPlacement::Handles(Initial,Scene->PresentedCamera(),Size,Tool);
        auto& App=FSlateApplication::Get();
        FVector2D Start,End;bool Found=false;
        for(const auto& L:Lines)
        {
            FVector2D A,B;if(!StudioCameraPlacement::ProjectLine(Scene->PresentedCamera(),Size,L.A,L.B,A,B,Aspect))continue;
            if((B-A).Size()<3)continue;
            Start=A*.35+B*.65;End=Tool==EStudioCameraPlacementTool::Move?Start+(B-A).GetSafeNormal()*14:B;
            if(Start.X<76||Start.X>Size.X-190||Start.Y<65||Start.Y>Size.Y-40)continue;
            // Floating controls can cover part of a handle at any window
            // size. Route only through an exposed point in Slate's real hit
            // path; a rectangular margin cannot account for the Views menu.
            const auto Hit=App.LocateWindowUnderMouse(Geometry.LocalToAbsolute(Start),App.GetInteractiveTopLevelWindows(),false,0);
            if(!Hit.ContainsWidget(Viewport.Get()))continue;
            if(StudioCameraPlacement::HitHandle(Lines,Scene->PresentedCamera(),Size,Start,Aspect)!=L.Axis)continue;
            StudioCameraPlacement::FDrag Check;
            if(!StudioCameraPlacement::BeginDrag(Initial,Scene->PresentedCamera(),Size,Start,Tool,L.Axis,Check,Aspect))continue;
            Found=true;break;
        }
        if(!Test->TestTrue(TEXT("An exposed scene handle is available"),Found))return;
        const auto Window=GEngine->GameViewport->GetWindow();
        const auto P=Geometry.LocalToAbsolute(Start),Q=Geometry.LocalToAbsolute(End);const TSet<FKey> Down={EKeys::LeftMouseButton};
        LastPointer=Q;
        App.ProcessMouseButtonDownEvent(Window->GetNativeWindow(),FPointerEvent(0,P,P,Down,EKeys::LeftMouseButton,0,FModifierKeysState()));
        Test->TestEqual(TEXT("Pointer capture follows displayed-handle ownership"),Viewport->HasMouseCapture(),bExpectCapture);
        if(!bExpectCapture)
        {
            App.ProcessMouseButtonUpEvent(FPointerEvent(0,P,P,TSet<FKey>(),EKeys::LeftMouseButton,0,FModifierKeysState()));
            Test->TestTrue(TEXT("Committing a numeric draft on press never orbits the observer"),StudioView::CameraEquals(Scene->SavedCameraState(),Observer));
            return;
        }
        App.ProcessMouseMoveEvent(FPointerEvent(1,Q,P,TSet<FKey>(),FKey(),0,FModifierKeysState()));
        Test->TestTrue(TEXT("Another pointer cannot move the captured camera draft"),StudioView::CameraEquals(M.CameraPlacement()->Camera,Initial));
        Test->TestTrue(TEXT("Another pointer leaves this gesture captured"),Viewport->HasMouseCapture());
        App.ProcessMouseMoveEvent(FPointerEvent(0,Q,P,Down,FKey(),0,FModifierKeysState()));
        if(bRelease)App.ProcessMouseButtonUpEvent(FPointerEvent(0,Q,Q,TSet<FKey>(),EKeys::LeftMouseButton,0,FModifierKeysState()));
        Test->TestFalse(TEXT("Routed scene drag edits draft"),StudioView::CameraEquals(M.CameraPlacement()->Camera,Initial));
        Test->TestTrue(TEXT("Scene drag preserves observer pose"),StudioView::CameraEquals(Scene->SavedCameraState(),Observer));
        Test->TestFalse(TEXT("Scene drag leaves no open view gesture"),M.IsViewEditActive());
    }
    void VerifyHandlesExposed(EStudioCameraPlacementTool Tool)
    {
        const auto Viewport=FindTag(TEXT("FlowViewport"));if(!Viewport)return;
        const auto Geometry=Viewport->GetCachedGeometry();const auto Size=Geometry.GetLocalSize();
        const auto& Draft=Scene->Model->CameraPlacement()->Camera;const auto& ObserverCamera=Scene->PresentedCamera();
        const auto RenderSize=Scene->PresentedViewportSize();const double Aspect=double(RenderSize.X)/RenderSize.Y;
        const auto Lines=StudioCameraPlacement::Handles(Draft,ObserverCamera,Size,Tool);auto& App=FSlateApplication::Get();
        for(int32 Axis=0;Axis<3;++Axis)
        {
            bool Applicable=false,Exposed=false;
            for(const auto& L:Lines)
            {
                if(L.Axis!=Axis)continue;FVector2D A,B;
                if(!StudioCameraPlacement::ProjectLine(ObserverCamera,Size,L.A,L.B,A,B,Aspect)||(B-A).Size()<1)continue;
                for(double T:{.35,.65,.85})
                {
                    const auto P=FMath::Lerp(A,B,T);StudioCameraPlacement::FDrag DragState;
                    if(!StudioCameraPlacement::BeginDrag(Draft,ObserverCamera,Size,P,Tool,Axis,DragState,Aspect))continue;
                    Applicable=true;
                    const auto Hit=App.LocateWindowUnderMouse(Geometry.LocalToAbsolute(P),App.GetInteractiveTopLevelWindows(),false,0);
                    if(Hit.ContainsWidget(Viewport.Get())&&StudioCameraPlacement::HitHandle(Lines,ObserverCamera,Size,P,Aspect)==Axis)Exposed=true;
                }
            }
            if(Applicable)Test->TestTrue(*FString::Printf(TEXT("%s axis %c has an exposed hit segment"),Tool==EStudioCameraPlacementTool::Move?TEXT("Move"):TEXT("Rotate"),TEXT("XYZ")[Axis]),Exposed);
            if(Applicable&&Tool==EStudioCameraPlacementTool::Move)
            {
                FVector Direction=FVector::ZeroVector;Direction[Axis]=1;FVector2D Label;
                const double Scale=StudioCameraPlacement::HandleScale(ObserverCamera,Size,Draft.Position);
                const bool Visible=StudioCameraPlacement::Project(ObserverCamera,Size,Draft.Position+Direction*Scale*1.12,Label,Aspect);
                const auto Hit=Visible?App.LocateWindowUnderMouse(Geometry.LocalToAbsolute(Label+FVector2D(5,5)),App.GetInteractiveTopLevelWindows(),false,0):FWidgetPath();
                Test->TestTrue(*FString::Printf(TEXT("Move axis %c label is unobscured"),TEXT("XYZ")[Axis]),Visible&&Hit.ContainsWidget(Viewport.Get()));
            }
        }
    }
    void ReleasePointer()
    {FSlateApplication::Get().ProcessMouseButtonUpEvent(FPointerEvent(0,LastPointer,LastPointer,TSet<FKey>(),EKeys::LeftMouseButton,0,FModifierKeysState()));}
    TArray<FColor> ReadPixels()
    {
        TArray<FColor> Result;Test->TestTrue(TEXT("Read CFD render pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Result));return Result;
    }
    void VerifySceneDetail()
    {
        const auto Current=ReadPixels();TMap<uint32,int32> Colors;int32 Largest=0;
        for(const auto& P:Current)
        {
            const uint32 RGB=(uint32(P.R)<<16)|(uint32(P.G)<<8)|uint32(P.B);
            Largest=FMath::Max(Largest,++Colors.FindOrAdd(RGB));
        }
        // Read the CFD target itself, so Slate labels/handles cannot make a
        // missing or uniformly filled render pass this acceptance check.
        Test->TestTrue(TEXT("Inside-domain camera presents scene detail"),Colors.Num()>32&&Largest<double(Current.Num())*.95);
    }
    void VerifyIsolation()
    {
        auto& M=*Scene->Model;Test->TestTrue(TEXT("Observer is independent of draft"),StudioView::CameraEquals(Scene->SavedCameraState(),Observer));
        Test->TestEqual(TEXT("Draft does not rebuild field geometry"),M.Revision,FieldRevision);
        Test->TestEqual(TEXT("Draft does not recapture CFD scene"),Scene->GetCaptureCount(),CaptureCount);
        Test->TestEqual(TEXT("Source frame retained"),M.SelectedFrame,Frame);Test->TestEqual(TEXT("Playback cursor retained"),M.PlaybackFrame,Playback);
        Test->TestEqual(TEXT("Original source retained"),M.Project.Dataset,Source);Test->TestEqual(TEXT("Case retained"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        const auto Current=ReadPixels();Test->TestEqual(TEXT("Render dimensions retained"),Current.Num(),Pixels.Num());
        int32 Changes=0;for(int32 I=0;I<FMath::Min(Current.Num(),Pixels.Num());++I)if(Current[I]!=Pixels[I])++Changes;
        Test->TestEqual(TEXT("Draft handles do not alter CFD pixels"),Changes,0);
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Image;FIntVector Size;Test->TestTrue(TEXT("Capture placement UI"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Image,Size));
        if(Image.IsEmpty())return;TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Image,PNG);
        Test->TestTrue(TEXT("Save placement evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioProject Original;
    EStudioWorkspace OriginalWorkspace=EStudioWorkspace::Solve;
    FGuid Id,OtherId;FStudioCameraState Before,Placed,Other,Observer,RetainedDraft;FString Root,Case,Source;TArray<FColor> Pixels;
    FVector2D LastPointer=FVector2D::ZeroVector;
    int32 Phase=0,Frame=0,Playback=0,FieldRevision=0;uint64 ChangedFrame=0,CaptureCount=0;double Started=0,PendingY=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCameraPlacementRender,"Studio.CameraPlacement.SceneControlsAndPersistence",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioCameraPlacementRender::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioCameraPlacementCommand(this));return true;}
#endif
