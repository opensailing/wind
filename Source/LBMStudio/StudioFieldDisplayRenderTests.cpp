#include "StudioScene.h"
#include "StudioFieldDisplay.h"
#include "StudioPointRecording.h"
#include "StudioSnapshot.h"
#include "ProceduralMeshComponent.h"
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
class FStudioFieldDisplayCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioFieldDisplayCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(Now-Started>90){Test->AddError(FString::Printf(TEXT("Field display timed out in phase %d"),Phase));return true;}
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
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/FieldDisplay");Work=Root/FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*Work,true);FString Error;
            Test->TestTrue(TEXT("Preserve preceding field project"),StudioProjectIO::Save(Work/TEXT("original.lbms"),M.SnapshotProject(),Error));
            Workspace=M.Workspace;M.NewProject(TEXT("Airfoil · field colors"));M.Navigate(EStudioWorkspace::Solve);
            Test->TestTrue(TEXT("Select supplied pressure for inspection"),M.EditView(TEXT("Inspect vector and streamline colors"),[](auto& S)
            {S.Display.bVectors=true;S.Display.bStreamlines=true;S.Display.bVolume=false;S.Display.bCutPlane=false;S.Display.ScalarField=TEXT("pressure");}));
            Test->TestEqual(TEXT("Recording advertises the requested pressure field"),M.ActiveScalar().Id,FString(TEXT("pressure")));
            Next();break;
        }
        case 1:
            VerifyGeometry(TEXT("pressure"));
            if(const auto* Section=VectorSection())for(const auto& Vertex:Section->ProcVertexBuffer)VectorPositions.Add(Vertex.Position);
            Camera=Scene->SavedCameraState();Dataset=M.Project.Dataset;Frame=M.SelectedFrame;
            Capture(TEXT("pressure.png"));Select(TEXT("density"));Next();break;
        case 2:
            VerifyGeometry(TEXT("density"));VerifyVectorPositions();Select(TEXT("velocity_magnitude"));Next();break;
        case 3:
            VerifyGeometry(TEXT("velocity_magnitude"));VerifyVectorPositions();
            Test->TestTrue(TEXT("Field selection retains exact camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
            Test->TestEqual(TEXT("Field selection retains source"),M.Project.Dataset,Dataset);
            Test->TestEqual(TEXT("Field selection retains original frame"),M.SelectedFrame,Frame);
            OpenVectorMenu(false);Next();break;
        case 4:
            Type(TEXT("VectorCount"),TEXT("1.5"));Test->TestEqual(TEXT("Fractional count does not change rendering"),M.VectorCount,384);
            Type(TEXT("VectorCount"),TEXT("4097"));Test->TestEqual(TEXT("Over-budget count refused"),M.VectorCount,384);
            Type(TEXT("VectorScale"),TEXT("nan"));Test->TestEqual(TEXT("Nonfinite typed scale refused"),M.VectorScale,1.);
            Next(40);break;
        case 40:
            Test->TestEqual(TEXT("Rejected scale stays editable across layout ticks"),InputText(TEXT("VectorScale")),FString(TEXT("nan")));
            if(const auto Error=FindTag(TEXT("VectorInputError")))
                Test->TestTrue(TEXT("Persistent error explains recovery"),StaticCastSharedPtr<STextBlock>(Error)->GetText().ToString().Contains(TEXT("Previous setting retained")));
            Capture(TEXT("vector-invalid.png"));
            Type(TEXT("VectorCount"),TEXT("257"));Type(TEXT("VectorScale"),TEXT("1.625"));Press(TEXT("VectorUniform"));Next(5);break;
        case 5:
            Test->TestTrue(TEXT("Exact controls applied"),M.VectorCount==257&&M.VectorScale==1.625&&M.bUniformVectors);
            VerifyVectorLengths();
            Test->TestTrue(TEXT("Vector mode undo"),M.UndoView()&&!M.bUniformVectors);
            Test->TestTrue(TEXT("Exact scale undo"),M.UndoView()&&M.VectorScale==1.);
            Test->TestTrue(TEXT("Exact count undo"),M.UndoView()&&M.VectorCount==384);Next(50);break;
        case 50:
            Test->TestEqual(TEXT("Count editor reflects undo"),InputText(TEXT("VectorCount")),FString(TEXT("384")));
            Test->TestEqual(TEXT("Scale editor reflects undo"),InputText(TEXT("VectorScale")),FString(TEXT("1")));
            Test->TestTrue(TEXT("Restore the three exact edits"),M.RedoView()&&M.RedoView()&&M.RedoView());Next(51);break;
        case 51:
            Test->TestEqual(TEXT("Count editor reflects redo"),InputText(TEXT("VectorCount")),FString(TEXT("257")));
            Test->TestEqual(TEXT("Scale editor reflects redo"),InputText(TEXT("VectorScale")),FString(TEXT("1.625")));
            Capture(TEXT("vector-uniform-menu.png"));Press(TEXT("VectorProportional"));Next(6);break;
        case 6:
            VerifyVectorLengths();Capture(TEXT("vector-proportional-menu.png"));
            FSlateApplication::Get().DismissAllMenus();
            CaptureAnnotatedSnapshot();
            Test->TestTrue(TEXT("Save vector display"),M.SaveProject(Work/TEXT("vectors.lbms")));
            M.NewProject(TEXT("Intervening project"));
            Test->TestTrue(TEXT("Reopen exact vector display"),M.RequestProjectOpen(Work/TEXT("vectors.lbms")));Next();break;
        case 7:
            Test->TestTrue(TEXT("Reopen retains count, scale and mode"),M.VectorCount==257&&M.VectorScale==1.625&&!M.bUniformVectors);
            VerifyVectorLengths();
            Test->TestTrue(TEXT("Import original 2D point vectors"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json")));Next();break;
        case 8:Scene->FitCamera();OpenVectorMenu(true);Next();break;
        case 9:Type(TEXT("VectorCount"),TEXT("97"));Press(TEXT("VectorUniform"));Next();break;
        case 10:
            VerifyVectorLengths();Capture(TEXT("vector-original-menu.png"));
            Press(TEXT("VectorProportional"));Next();break;
        case 11:
            VerifyVectorLengths();FSlateApplication::Get().DismissAllMenus();
            Test->TestTrue(TEXT("Import original 3D vectors"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json")));Next();break;
        case 12:Scene->FitCamera();Next();break;
        case 13:
            VerifyVectorLengths();Capture(TEXT("vector-3d.png"));
            Test->TestTrue(TEXT("Restore preceding field project"),M.RequestProjectOpen(Work/TEXT("original.lbms")));Next();break;
        case 14:M.Navigate(Workspace);return true;
        }
        return false;
    }
private:
    void Next(int32 Target=-1){Phase=Target<0?Phase+1:Target;ChangedFrame=GFrameCounter;}
    void Select(const TCHAR* Scalar)
    {Scene->Model->EditView(TEXT("Selected scalar"),[Scalar](auto& S){S.Display.ScalarField=Scalar;});}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag,bool Button=false)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if((!Button&&W->GetTag()==Tag)||(Button&&W->GetType()==TEXT("SButton")))return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Find(Children->GetChildAt(I),Tag,Button))return Found;
        return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto Found=Find(W,Tag))return Found;
        Test->AddError(TEXT("Missing vector control: ")+Tag.ToString());return {};
    }
    void Enter(const TSharedPtr<SWidget>& W)
    {
        if(!W)return;auto& App=FSlateApplication::Get();Test->TestTrue(TEXT("Vector control enabled"),W->IsEnabled());
        if(!W->HasKeyboardFocus()&&!W->HasFocusedDescendants())App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
        App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag){Enter(FindTag(Tag));}
    FString InputText(FName Tag)
    {const auto W=FindTag(Tag);return W?StaticCastSharedPtr<SEditableTextBox>(W)->GetText().ToString():FString();}
    void OpenVectorMenu(bool Original)
    {if(auto W=FindTag(Original?TEXT("PointVectorSettings"):TEXT("FieldVectorSettings")))Enter(Find(W.ToSharedRef(),NAME_None,true));}
    void Type(FName Tag,const FString& Value)
    {
        const auto W=FindTag(Tag);if(!W)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        const FModifierKeysState All(false,false,true,false,false,false,false,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,All,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,All,0,false,0,0));
        for(TCHAR Ch:Value)App.ProcessKeyCharEvent(FCharacterEvent(Ch,FModifierKeysState(),0,false));Enter(W);
    }
    const FProcMeshSection* VectorSection()
    {
        const auto Mesh=Scene->FindComponentByClass<UProceduralMeshComponent>();
        return Mesh?Mesh->GetProcMeshSection(4):nullptr;
    }
    void VerifyVectorPositions()
    {
        const auto* Section=VectorSection();
        if(!Test->TestNotNull(TEXT("Vector geometry remains present"),Section)||
            !Test->TestEqual(TEXT("Scalar field does not change vector vertex count"),Section->ProcVertexBuffer.Num(),VectorPositions.Num()))return;
        for(int32 I=0;I<VectorPositions.Num();++I)
            if(Section->ProcVertexBuffer[I].Position!=VectorPositions[I])
            {Test->AddError(FString::Printf(TEXT("Scalar selection moved vector vertex %d"),I));return;}
    }
    void VerifyVectorLengths()
    {
        const auto* Section=VectorSection();const auto Field=Scene->PresentedField();const auto& Summary=Scene->PresentedVectors();
        const auto& M=*Scene->Model;
        if(!Test->TestTrue(TEXT("Vector geometry and immutable field available"),Section&&Field&&Summary.GlyphCount>0))return;
        Test->TestEqual(TEXT("Displayed mode belongs to rendered arrows"),Summary.bUniformLength,M.bUniformVectors);
        Test->TestEqual(TEXT("Exact sample budget reached"),Summary.SampleCount,M.VectorCount);
        if(!Test->TestEqual(TEXT("Legend counts actual complete glyphs"),Section->ProcVertexBuffer.Num(),Summary.GlyphCount*27))return;
        const auto Points=Field->OriginalPoints();
        const TArray<double>* Components[3]={nullptr,nullptr,nullptr};
        if(Points)for(const auto& F:Points->Descriptor->Fields)if(F.Vector==TEXT("velocity")&&F.Unit==TEXT("m/s"))
            Components[F.Component==TEXT("x")?0:F.Component==TEXT("y")?1:2]=Points->FindValues(F.Id);
        for(int32 Base=0;Base<Section->ProcVertexBuffer.Num();Base+=27)
        {
            const auto& V=Section->ProcVertexBuffer;
            const FVector Origin=(V[Base].Position+V[Base+12].Position)/200.,Tip=V[Base+24].Position/100.;
            FVector Velocity;bool Found=false;
            if(Points&&Components[0]&&Components[1])
            {
                for(int32 I=0;I<Points->Geometry->Positions.Num();++I)
                {
                    const auto P=Points->Geometry->Positions[I];
                    if(Origin.Equals(FVector(P.X,P.Z,P.Y),1.e-8))
                    {Velocity=FVector((*Components[0])[I],Components[2]?(*Components[2])[I]:0,(*Components[1])[I]);Found=true;break;}
                }
            }
            else Found=Field->SampleVelocity(Origin,Velocity);
            if(!Test->TestTrue(TEXT("Rendered origin has real original velocity"),Found))return;
            const double Speed=Velocity.Size(),Expected=Summary.ReferenceLengthMeters*(M.bUniformVectors?1.:Speed/Summary.MaximumSpeed);
            if(!Test->TestTrue(TEXT("Rendered arrow encodes recorded direction and length"),Speed>0&&(Tip-Origin).Equals(Velocity/Speed*Expected,1.e-6)))return;
        }
        Test->AddInfo(FString::Printf(TEXT("Verified %d actual arrows against %s velocity; %.9g m reference, %.9g m/s sample maximum"),Summary.GlyphCount,
            Points?TEXT("original point"):TEXT("recorded interpolated"),Summary.ReferenceLengthMeters,Summary.MaximumSpeed));
    }
    void VerifyGeometry(const TCHAR* Scalar)
    {
        Test->TestEqual(TEXT("Presented legend identifies selected scalar"),Scene->PresentedScalar().Id,FString(Scalar));
        const auto Mesh=Scene->FindComponentByClass<UProceduralMeshComponent>();
        if(!Test->TestNotNull(TEXT("Rendered field mesh exists"),Mesh))return;
        const auto Field=Scene->Model->Solver->CaptureViewField(Scene->Model->SelectedFrame,Scalar,true);
        for(int32 SectionIndex:{3,4})
        {
            const auto* Section=Mesh->GetProcMeshSection(SectionIndex);
            if(!Test->TestTrue(TEXT("Actual stream/vector section populated"),Section&&Section->ProcVertexBuffer.Num()>100))return;
            // Tube() emits four quad faces (24 vertices); each vector adds one
            // arrow triangle. Opposite ring vertices recover its source origin.
            const int32 Stride=SectionIndex==3?24:27;
            if(!Test->TestEqual(TEXT("Complete glyph primitive records"),Section->ProcVertexBuffer.Num()%Stride,0))return;
            int32 Checked=0;
            for(int32 Base=0;Base<Section->ProcVertexBuffer.Num();Base+=Stride)
            {
                const auto& Vertices=Section->ProcVertexBuffer;
                const FVector P=(Vertices[Base].Position+Vertices[Base+12].Position)/200.;
                FLinearColor Expected;
                if(!StudioFieldDisplay::SampleColor(*Field,P,Scalar,Scene->PresentedColorMapping(),Expected))
                {Test->AddError(TEXT("Rendered glyph origin lacks its selected source scalar"));return;}
                const FColor Packed=Expected.ToFColor(false),Actual=Vertices[Base].Color;
                // Recovering a tube center involves two floating additions;
                // tolerate one 8-bit rounding step, never a different field.
                if(FMath::Abs(int(Packed.R)-Actual.R)>1||FMath::Abs(int(Packed.G)-Actual.G)>1||FMath::Abs(int(Packed.B)-Actual.B)>1)
                {Test->AddError(FString::Printf(TEXT("Section %d glyph %d color disagrees with %s legend"),SectionIndex,Base/Stride,Scalar));return;}
                ++Checked;
            }
            Test->AddInfo(FString::Printf(TEXT("Verified %d %s primitives against original %s samples"),Checked,SectionIndex==3?TEXT("streamline"):TEXT("vector"),Scalar));
        }
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture selected-scalar native view"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Retain selected-scalar evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    void CaptureAnnotatedSnapshot()
    {
        FStudioSnapshot Snapshot;Snapshot.Options.Size=FIntPoint(1280,720);FString Error;
        if(!Test->TestTrue(TEXT("Capture annotated export with vector key"),Scene->CaptureSnapshot(Snapshot,nullptr,Error)))return;
        Test->TestEqual(TEXT("Snapshot legend freezes actual glyph count"),Snapshot.Vectors.GlyphCount,Scene->PresentedVectors().GlyphCount);
        Test->TestEqual(TEXT("Snapshot legend freezes actual speed maximum"),Snapshot.Vectors.MaximumSpeed,Scene->PresentedVectors().MaximumSpeed);
        TArray64<uint8> PNG;
        Test->TestTrue(TEXT("Encode annotated export and frozen metadata"),StudioSnapshot::Encode(Snapshot,PNG,Error));
        Test->TestTrue(TEXT("Retain vector legend export"),FFileHelper::SaveArrayToFile(PNG,*(Root/TEXT("vector-snapshot.png"))));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;TArray<FVector> VectorPositions;
    FStudioCameraState Camera;FString Root,Work,Dataset;EStudioWorkspace Workspace=EStudioWorkspace::Solve;
    double Started=0;uint64 ChangedFrame=0;int32 Phase=0,Frame=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFieldDisplayRender,"Studio.FieldDisplay.RenderedGlyphsUseSelectedScalar",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioFieldDisplayRender::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioFieldDisplayCommand(this));return true;}
#endif
