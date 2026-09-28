#include "StudioScene.h"
#include "StudioCameraPlacement.h"
#include "StudioPointRecording.h"
#include "StudioVolume.h"
#include "StudioSnapshot.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/HittestGrid.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
// Routed Slate acceptance: real packaged rendering and hit testing, not OS input.
class FStudioStreamlineRenderCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioStreamlineRenderCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(Now-Started>120){Test->AddError(FString::Printf(TEXT("Streamline acceptance timed out in phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        if(Phase==0)M.InspectorTab=3; // This workflow starts in the Display owner.
        const auto* Target=Scene->GetRenderTarget();
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||!Scene->HasCurrentFrame()||!Target||
            GFrameCounter-ChangedFrame<3||Scene->PresentedViewportSize()!=FIntPoint(Target->SizeX,Target->SizeY))return false;
        switch(Phase)
        {
        case 0:
        {
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/Streamlines");Work=Root/FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*Work,true);FString Error;Workspace=M.Workspace;
            Test->TestTrue(TEXT("Preserve preceding project"),StudioProjectIO::Save(Work/TEXT("original.lbms"),M.SnapshotProject(),Error));
            M.NewProject(TEXT("Airfoil · streamline inspection"));M.Navigate(EStudioWorkspace::Solve);M.Pause();
            M.EditView(TEXT("Inspect recorded streamlines"),[](auto& S)
            {S.Display.bVectors=false;S.Display.bStreamlines=true;S.Display.bVolume=false;S.Display.bCutPlane=false;S.Display.ScalarField=TEXT("pressure");});
            Next();break;
        }
        case 1:VerifyGeometry();OpenMenu(TEXT("StreamlineSettings"));Next();break;
        case 2:
            Type(TEXT("StreamSeedCount"),TEXT("513"));Test->TestEqual(TEXT("Oversize inlet count rejected"),M.StreamlineSettings.AutomaticSeedCount,84);
            Type(TEXT("StreamWorkBudget"),TEXT("3.5"));Test->TestEqual(TEXT("Fractional work budget rejected"),M.StreamlineSettings.WorkBudget,32768);
            Type(TEXT("StreamWidth"),TEXT("nan"));Next();break;
        case 3:
        {
            Test->TestEqual(TEXT("Rejected width draft survives layout"),InputText(TEXT("StreamWidth")),FString(TEXT("nan")));
            if(const auto Error=FindTag(TEXT("StreamlineInputError")))Test->TestTrue(TEXT("Rejection gives a recovery instruction"),
                StaticCastSharedPtr<STextBlock>(Error)->GetText().ToString().Contains(TEXT("Previous setting retained")));
            Capture(TEXT("invalid.png"));
            const double Scale=M.Solver->Descriptor().DisplayBounds.GetSize().GetMax();
            Type(TEXT("StreamSeedCount"),TEXT("31"));Press(TEXT("StreamDirection2"));
            Type(TEXT("StreamWidth"),FString::Printf(TEXT("%.17g"),.001*Scale));
            Type(TEXT("StreamStep"),FString::Printf(TEXT("%.17g"),.004*Scale));
            Type(TEXT("StreamLength"),FString::Printf(TEXT("%.17g"),.15*Scale));
            Type(TEXT("StreamMaxSteps"),TEXT("35"));Type(TEXT("StreamWorkBudget"),TEXT("2048"));Next();break;
        }
        case 4:
            Test->TestTrue(TEXT("Exact controls apply"),M.StreamlineSettings.AutomaticSeedCount==31&&M.StreamlineSettings.Direction==EStudioStreamDirection::Both&&
                M.StreamlineSettings.MaximumSteps==35&&M.StreamlineSettings.WorkBudget==2048);
            VerifyGeometry();Capture(TEXT("settings.png"));Type(TEXT("StreamWorkBudget"),TEXT("1"));Next();break;
        case 5:
            Test->TestTrue(TEXT("Actual tracing obeys aggregate work limit"),Scene->PresentedStreams().Attempts<=1&&Scene->PresentedStreams().bBudgetExhausted);
            Capture(TEXT("budget.png"));Test->TestTrue(TEXT("Budget undo"),M.UndoView());Next();break;
        case 6:
            Test->TestEqual(TEXT("Editor follows undo"),InputText(TEXT("StreamWorkBudget")),FString(TEXT("2048")));
            Test->TestTrue(TEXT("Budget redo"),M.RedoView());Next();break;
        case 7:
            Test->TestEqual(TEXT("Editor follows redo"),InputText(TEXT("StreamWorkBudget")),FString(TEXT("1")));
            Type(TEXT("StreamWorkBudget"),TEXT("2048"));SavedSettings=M.StreamlineSettings;
            FSlateApplication::Get().DismissAllMenus();Test->TestTrue(TEXT("Save controls"),M.SaveProject(Work/TEXT("settings.lbms")));
            M.NewProject(TEXT("Intervening project"));Test->TestTrue(TEXT("Reopen controls"),M.RequestProjectOpen(Work/TEXT("settings.lbms")));Next();break;
        case 8:
            Test->TestTrue(TEXT("All settings round trip exactly"),M.StreamlineSettings==SavedSettings);VerifyGeometry();
            OpenMenu(TEXT("StreamlineSettings"));Press(TEXT("StreamSavedSeeds"));Next();break;
        case 9:
            Test->TestEqual(TEXT("Empty saved mode invents no traces"),Scene->PresentedStreams().Segments,0);
            Press(TEXT("StreamManageSeeds"));Next();break;
        case 10:Capture(TEXT("saved-empty.png"));Add(7);Next();break;
        case 11:
            Line=M.SelectedInspectionObject;Type(TEXT("SeedCount"),TEXT("9"));
            Type(TEXT("SeedAX"),TEXT("-0.2"));Type(TEXT("SeedAZ"),TEXT("-0.12"));
            Type(TEXT("SeedBX"),TEXT("-0.2"));Type(TEXT("SeedBZ"),TEXT("0.12"));
            Scene->AlignCamera(FIntVector(0,-1,0));Scene->FitCamera();Next();break;
        case 12:
            Test->TestEqual(TEXT("Simple exact coordinate is readable without roundoff padding"),InputText(TEXT("SeedAX")),FString(TEXT("-0.2")));
            VerifyGeometry();Capture(TEXT("line.png"));PriorLine=*M.FindSeed(Line);Press(TEXT("PlaceInspection"));Next();break;
        case 13:
            ClickWorld(FVector(-.3,0,-.16));Test->TestTrue(TEXT("Partial placement retains saved line"),M.FindSeed(Line)->A==PriorLine.A&&M.FindSeed(Line)->B==PriorLine.B);Next();break;
        case 14:Capture(TEXT("line-draft.png"));ClickWorld(FVector(-.3,0,.16));Next();break;
        case 15:
            Test->TestTrue(TEXT("Routed placement commits both line endpoints"),M.FindSeed(Line)->A.Equals(FVector(-.3,0,-.16),1.e-5)&&M.FindSeed(Line)->B.Equals(FVector(-.3,0,.16),1.e-5));
            Test->TestTrue(TEXT("Placement is one undo step"),M.UndoView()&&M.FindSeed(Line)->A==PriorLine.A&&M.FindSeed(Line)->B==PriorLine.B);
            Test->TestTrue(TEXT("Placement redo"),M.RedoView());Add(6);Next();break;
        case 16:Plane=M.SelectedInspectionObject;Type(TEXT("SeedCount"),TEXT("5"));Press(TEXT("PlaceInspection"));Next();break;
        case 17:ClickWorld(FVector(.2,0,.18));ClickWorld(FVector(.35,0,.18));ClickWorld(FVector(.2,0,.25));Next();break;
        case 18:
            Test->TestTrue(TEXT("Plane placement stores full spans"),M.FindSeed(Plane)->A.Equals(FVector(.2,0,.18),1.e-5)&&
                M.FindSeed(Plane)->B.Equals(FVector(.3,0,0),1.e-5)&&M.FindSeed(Plane)->C.Equals(FVector(0,0,.14),1.e-5));
            Capture(TEXT("plane.png"));Add(8);Next();break;
        case 19:Points=M.SelectedInspectionObject;Press(TEXT("PlaceInspection"));Next();break;
        case 20:ClickWorld(FVector(.25,0,.22));Next();break;
        case 21:Press(TEXT("SeedAddPoint"));Next();break;
        case 22:ClickWorld(FVector(.3,0,-.2));Next();break;
        case 23:
            Test->TestTrue(TEXT("Selected positions retain both routed clicks"),M.FindSeed(Points)->Points.Num()==2&&
                M.FindSeed(Points)->Points[0].Equals(FVector(.25,0,.22),1.e-5)&&M.FindSeed(Points)->Points[1].Equals(FVector(.3,0,-.2),1.e-5));
            Capture(TEXT("points.png"));SavedSeeds=M.InspectionObjects;Press(TEXT("CloseInspection"));
            Test->TestTrue(TEXT("Save all seed types"),M.SaveProject(Work/TEXT("seeds.lbms")));
            M.NewProject(TEXT("Intervening seed project"));Test->TestTrue(TEXT("Reopen seed objects"),M.RequestProjectOpen(Work/TEXT("seeds.lbms")));Next();break;
        case 24:
            Test->TestTrue(TEXT("Seeds reopen with exact coordinates and IDs"),M.InspectionObjects==SavedSeeds);VerifyGeometry();
            OpenMenu(TEXT("StreamlineSettings"));Press(TEXT("StreamManageSeeds"));Next(33);break;
        case 33:Add(5);Next();break;
        case 34:Inlet=M.SelectedInspectionObject;Type(TEXT("SeedCount"),TEXT("7"));Press(TEXT("SeedFace01"));Next();break;
        case 35:
            Test->TestTrue(TEXT("Inlet face controls select the requested domain side"),M.FindSeed(Inlet)->InletAxis==0&&M.FindSeed(Inlet)->bUpperFace&&M.FindSeed(Inlet)->Count==7);
            VerifyGeometry();Capture(TEXT("inlet.png"));Press(TEXT("CloseInspection"));
            Test->TestTrue(TEXT("Import real original point recording"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json")));Next(25);break;
        case 25:
            Test->TestEqual(TEXT("Original points do not invent interpolation"),Scene->PresentedStreams().Segments,0);Capture(TEXT("original-unavailable.png"));
            Test->TestTrue(TEXT("Attach verified surface"),M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture/reconstruction.json")));Next();break;
        case 26:
            M.EditView(TEXT("Surface streamlines"),[](auto& S){S.Display.StreamlineSettings.bAutomaticSeeds=true;S.Display.StreamlineSettings.AutomaticSeedCount=13;S.Display.bReconstructedSurface=true;});
            Scene->FitCamera();Next();break;
        case 27:VerifyGeometry();Capture(TEXT("surface.png"));M.Scrub(1);M.Scrub(0);M.Scrub(1);Next();break;
        case 28:
            Test->TestEqual(TEXT("Latest frame wins over obsolete tracing"),Scene->PresentedFrame().Index,9000);VerifyGeometry();
            Test->TestTrue(TEXT("Import real 3D cylinder recording"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json")));Next();break;
        case 29:
            Test->TestTrue(TEXT("Attach verified volume"),M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json")));Next();break;
        case 30:
        {
            const auto Field=Scene->PresentedField();const auto Grid=M.Solver->VolumeReconstruction();
            if(!Test->TestTrue(TEXT("Captured 3D field and topology present"),Field&&Grid))return true;
            FStudioSeedObject S;S.Name=TEXT("Cylinder interior");S.Kind=EStudioSeedKind::Points;
            const FBox B(FVector(Grid->SourceBounds.Min.X,Grid->SourceBounds.Min.Z,Grid->SourceBounds.Min.Y),
                FVector(Grid->SourceBounds.Max.X,Grid->SourceBounds.Max.Z,Grid->SourceBounds.Max.Y));
            for(int32 X=1;X<=4;++X)for(int32 Y=1;Y<=4;++Y)for(int32 Z=1;Z<=4;++Z)
            {const FVector P=B.Min+B.GetSize()*FVector(X/5.,Y/5.,Z/5.);FVector V;if(Field->SampleVelocity(P,V)&&V.Size()>0)S.Points.Add(P);}
            Test->TestTrue(TEXT("3D seeds are placed in genuine supported data"),!S.Points.IsEmpty()&&M.AddSeed(S));
            M.EditView(TEXT("3D trace display"),[](auto& V){V.Display.bVolume=false;V.Display.bSourcePoints=false;V.Display.bReconstructedSurface=false;V.Display.bVectors=false;V.Display.bCutPlane=false;});
            Scene->FitCamera();Next();break;
        }
        case 31:
        {
            VerifyGeometry();Capture(TEXT("volume.png"));FStudioSnapshot Snapshot;Snapshot.Options.Size=FIntPoint(1280,720);FString Error;
            Test->TestTrue(TEXT("Snapshot freezes streamline display"),Scene->CaptureSnapshot(Snapshot,nullptr,Error));
            Test->TestEqual(TEXT("Snapshot freezes actual segment count"),Snapshot.Streams.Segments,Scene->PresentedStreams().Segments);
            Test->TestTrue(TEXT("Snapshot contains trace provenance"),StudioSnapshot::Metadata(Snapshot).Contains(TEXT("streamline_display")));
            TArray64<uint8> PNG;Test->TestTrue(TEXT("Encode trace snapshot"),StudioSnapshot::Encode(Snapshot,PNG,Error));
            Test->TestTrue(TEXT("Retain annotated snapshot"),FFileHelper::SaveArrayToFile(PNG,*(Root/TEXT("snapshot.png"))));
            Test->TestTrue(TEXT("Restore preceding project"),M.RequestProjectOpen(Work/TEXT("original.lbms")));Next();break;
        }
        case 32:M.Navigate(Workspace);return true;
        }
        return false;
    }
private:
    void Next(int32 Target=-1){Phase=Target<0?Phase+1:Target;ChangedFrame=GFrameCounter;}
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
        Test->AddError(TEXT("Missing streamline control: ")+Tag.ToString());return {};
    }
    void Enter(const TSharedPtr<SWidget>& W)
    {
        if(!W)return;auto& App=FSlateApplication::Get();Test->TestTrue(TEXT("Control enabled"),W->IsEnabled());
        if(!W->HasKeyboardFocus()&&!W->HasFocusedDescendants())App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag){Enter(FindTag(Tag));}
    void OpenMenu(FName Tag){if(auto W=FindTag(Tag))Enter(Find(W.ToSharedRef(),NAME_None,true));}
    void Add(int32 Kind){OpenMenu(TEXT("AddInspection"));Press(FName(*FString::Printf(TEXT("AddInspection%d"),Kind)));}
    FString InputText(FName Tag){const auto W=FindTag(Tag);return W?StaticCastSharedPtr<SEditableTextBox>(W)->GetText().ToString():FString();}
    void Type(FName Tag,const FString& Value)
    {
        const auto W=FindTag(Tag);if(!W)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        const FModifierKeysState All(false,false,true,false,false,false,false,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,All,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,All,0,false,0,0));
        for(TCHAR Ch:Value)App.ProcessKeyCharEvent(FCharacterEvent(Ch,FModifierKeysState(),0,false));Enter(W);
    }
    void ClickWorld(const FVector& P)
    {
        const auto W=FindTag(TEXT("FlowViewport"));if(!W)return;const auto G=W->GetCachedGeometry();FVector2D Pixel;
        const auto Size=Scene->PresentedViewportSize();if(!Test->TestTrue(TEXT("Seed projects into rendered view"),
            StudioCameraPlacement::Project(Scene->PresentedCamera(),G.GetLocalSize(),P,Pixel,double(Size.X)/Size.Y)))return;
        Pixel=G.LocalToAbsolute(Pixel);auto& App=FSlateApplication::Get();const auto Window=GEngine->GameViewport->GetWindow().ToSharedRef();
        if(!Test->TestTrue(TEXT("Placement window is exposed"),Window->IsVisible()&&!Window->IsWindowMinimized()&&Window->AcceptsInput()&&
            App.GetInteractiveTopLevelWindows().Contains(Window)&&Window->IsScreenspaceMouseWithin(Pixel)))return;
        const FWidgetPath Hit(Window->GetHittestGrid().GetBubblePath(Pixel,0,false,0));
        if(!Test->TestTrue(TEXT("Routed click hits actual flow viewport"),Hit.ContainsWidget(W.Get())))return;
        App.RoutePointerMoveEvent(Hit,FPointerEvent(0,Pixel,Pixel,{},FKey(),0,FModifierKeysState()),false);
        App.RoutePointerDownEvent(Hit,FPointerEvent(0,Pixel,Pixel,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
        App.RoutePointerUpEvent(Hit,FPointerEvent(0,Pixel,Pixel,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
    }
    void VerifyGeometry()
    {
        const auto Mesh=Scene->FindComponentByClass<UProceduralMeshComponent>();const auto Field=Scene->PresentedField();
        const auto* Section=Mesh?Mesh->GetProcMeshSection(3):nullptr;const auto& Summary=Scene->PresentedStreams();
        if(!Test->TestTrue(TEXT("Recorded velocity traces render with vectors hidden"),!Scene->Model->bVectors&&Field&&Section&&Summary.Segments>0))return;
        if(!Test->TestEqual(TEXT("Summary counts actual complete tubes"),Section->ProcVertexBuffer.Num(),Summary.Segments*24))return;
        Test->TestTrue(TEXT("Work budget bounds captured tracing"),Summary.Attempts<=Scene->Model->StreamlineSettings.WorkBudget);
        for(int32 I=0;I<Section->ProcVertexBuffer.Num();I+=24)
        {
            const auto& V=Section->ProcVertexBuffer;FVector P=(V[I].Position+V[I+12].Position)/200.,Q=(V[I+1].Position+V[I+13].Position)/200.;
            const auto Identity=Field->Identity();
            if(Identity.IsSet()&&Identity->SpatialDimensions==2)
            {
                // Diametrically opposite tube vertices recover the center to
                // roundoff; sin(pi) leaves a tiny component perpendicular to
                // the exact 2D source plane. Verify it before resampling there.
                if(!Test->TestTrue(TEXT("Rendered centers stay on the exact 2D plane within roundoff"),
                    FMath::Abs(P.Y-Identity->SourceOffset.Y)<1.e-12&&FMath::Abs(Q.Y-Identity->SourceOffset.Y)<1.e-12))return;
                P.Y=Q.Y=Identity->SourceOffset.Y;
            }
            FVector Velocity,MidVelocity;double Scalar;
            if(!Test->TestTrue(TEXT("Actual tube origin has recorded velocity and scalar"),Field->SampleVelocity(P,Velocity)&&Field->SampleScalar(P,Scene->PresentedScalar().Id,Scalar)))
            {Test->AddError(FString::Printf(TEXT("Unsampleable rendered center in phase %d, tube %d at (%.17g, %.17g, %.17g)"),Phase,I/24,P.X,P.Y,P.Z));return;}
            const double Step=(Q-P).Size(),Sign=FVector::DotProduct(Q-P,Velocity)<0?-1.:1.;
            if(!Test->TestTrue(TEXT("Actual tube follows midpoint integration"),Field->SampleVelocity(P+Velocity/Velocity.Size()*(Sign*Step*.5),MidVelocity)&&
                Q.Equals(P+MidVelocity/MidVelocity.Size()*(Sign*Step),1.e-7)))return;
            if(!Test->TestTrue(TEXT("Actual tube has requested full diameter"),FMath::Abs((V[I].Position/100.-P).Size()-Summary.WidthMeters*.5)<1.e-8))return;
            const auto Packed=StudioColor::Map(Scalar,Scene->PresentedColorMapping()).ToFColor(false),Actual=V[I].Color;
            if(!Test->TestTrue(TEXT("Trace color uses captured scalar mapping"),FMath::Abs(int(Packed.R)-Actual.R)<=1&&FMath::Abs(int(Packed.G)-Actual.G)<=1&&FMath::Abs(int(Packed.B)-Actual.B)<=1))return;
        }
        Test->AddInfo(FString::Printf(TEXT("Verified %d rendered streamline segments against immutable %s frame %d"),Summary.Segments,*Scene->PresentedDatasetId(),Scene->PresentedFrame().Index));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture streamline native UI"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Retain streamline UI evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Work;
    FStudioStreamlineSettings SavedSettings;FStudioSeedObject PriorLine;FStudioInspectionObjects SavedSeeds;
    FGuid Line,Plane,Points,Inlet;EStudioWorkspace Workspace=EStudioWorkspace::Solve;
    double Started=0;uint64 ChangedFrame=0;int32 Phase=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioStreamlineRender,"Studio.Streamlines.RenderedControlsAndPlacement",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioStreamlineRender::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioStreamlineRenderCommand(this));return true;}
#endif
