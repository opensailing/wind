#include "StudioScene.h"
#include "StudioPointRecording.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Controls use three unchanged original snapshots. The separate full-sequence
 * and soak gates own duration, streaming and long-run resource acceptance. */
class FStudioColorControlsCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioColorControlsCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(Now-Started>90){Test->AddError(FString::Printf(TEXT("Color workflow exceeded 90 seconds in phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        if(Phase==0)M.InspectorTab=3; // This workflow starts in the Display owner.

        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            if(M.State==EStudioRunState::Running)M.Pause();
            M.SetControlHarness(false);Original=M.SnapshotProject();Original.Camera=Scene->SavedCameraState();
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/ColorControls");Work=Root/FGuid::NewGuid().ToString();
            FString Error;
            Test->TestTrue(TEXT("Preserve initial project"),StudioProjectIO::Save(Work/TEXT("original.lbms"),Original,Error));
            Test->TestTrue(TEXT("Import original point fixture for controls"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json")));
            Phase=1;break;
        }
        case 1:
            if(M.IsRecordingLoadPending()||!Scene->HasCurrentFrame())return false;
            if(!Test->TestEqual(TEXT("Three original control-test snapshots"),M.Solver->FrameCount(),3))return true;
            M.EditView(TEXT("Color controls setup"),[](auto& S)
            {S.Display.ScalarField=TEXT("pressure");S.Display.ScalarStyles.Reset();S.Display.bSourcePoints=true;S.Display.bVectors=false;});
            M.Scrub(.5);Scene->FitCamera();Phase=2;break;
        case 2:
            if(!Scene->HasCurrentFrame())return false;
            Camera=Scene->SavedCameraState();Frame=M.SelectedFrame;
            Test->TestEqual(TEXT("Real fixture step identity"),Scene->PresentedFrame().Index,5001);
            Test->TestTrue(TEXT("Baseline scalar image"),Scene->Snapshot(Root/TEXT("source-field.png")));
            Test->TestTrue(TEXT("Baseline scientific values"),M.ExportField(Work/TEXT("before.csv")));
            OpenMenu(TEXT("ColorSettings"));Phase=3;break;
        case 3:
            Capture(TEXT("source-range.png"));Press(TEXT("ColorPalette1"));
            EditText(TEXT("ColorMinimum"),TEXT("-100"));EditText(TEXT("ColorMaximum"),TEXT("100"));Phase=4;break;
        case 4:
            Press(TEXT("ApplyColorRange"));
            Test->TestTrue(TEXT("Range applies through visible action"),M.ActiveColorMapping().bManualRange);
            Test->TestEqual(TEXT("Range action retains frame ordinal"),M.SelectedFrame,Frame);
            Test->TestTrue(TEXT("Range action retains arbitrary camera"),StudioView::CameraEquals(Camera,Scene->SavedCameraState()));
            Test->TestFalse(TEXT("Pending range cannot export mismatched pixels"),Scene->Snapshot(Work/TEXT("pending.png")));
            Test->TestFalse(TEXT("Presented legend retains its old range while new colors build"),Scene->PresentedColorMapping().bManualRange);
            Phase=5;break;
        case 5:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Rendered range minimum"),Scene->PresentedColorMapping().Minimum,-100.);
            Test->TestEqual(TEXT("Rendered range maximum"),Scene->PresentedColorMapping().Maximum,100.);
            Test->TestEqual(TEXT("Rendered palette"),Scene->PresentedColorMapping().Palette,1);
            VerifyPointColors();Capture(TEXT("custom-range.png"));FSlateApplication::Get().DismissAllMenus();Phase=6;break;
        case 6:
        {
            Capture(TEXT("pressure-field.png"));
            Test->TestTrue(TEXT("Custom scalar image"),Scene->Snapshot(Root/TEXT("custom-field.png")));
            TArray<uint8> Before,After;FFileHelper::LoadFileToArray(Before,*(Root/TEXT("source-field.png")));FFileHelper::LoadFileToArray(After,*(Root/TEXT("custom-field.png")));
            Test->TestTrue(TEXT("Palette/range changes actual field pixels at the same frame and camera"),!Before.IsEmpty()&&!After.IsEmpty()&&Before!=After);
            Test->TestTrue(TEXT("Scientific export after color edit"),M.ExportField(Work/TEXT("after.csv")));
            FString OldCSV,NewCSV;FFileHelper::LoadFileToString(OldCSV,*(Work/TEXT("before.csv")));FFileHelper::LoadFileToString(NewCSV,*(Work/TEXT("after.csv")));
            Test->TestTrue(TEXT("Color edits leave all original exported values and source identity unchanged"),!OldCSV.IsEmpty()&&OldCSV==NewCSV);
            if(auto Legend=FindTag(TEXT("ColorLegend")))Test->TestTrue(TEXT("Visible legend identifies custom range"),Text(Legend.ToSharedRef()).Contains(TEXT("Custom range")));
            OpenMenu(TEXT("ColorSettings"));Phase=7;break;
        }
        case 7:
            BeforeInvalid=M.InspectionState();EditText(TEXT("ColorMinimum"),TEXT("NaN"));Press(TEXT("ApplyColorRange"));
            Test->TestTrue(TEXT("Nonfinite text leaves view unchanged"),M.InspectionState().Equals(BeforeInvalid));
            if(auto Feedback=FindTag(TEXT("ColorFeedback")))Test->TestTrue(TEXT("Inline error identifies finite-number requirement"),Text(Feedback.ToSharedRef()).Contains(TEXT("finite numbers")));
            EditText(TEXT("ColorMinimum"),TEXT("200"));Press(TEXT("ApplyColorRange"));Phase=8;break;
        case 8:
            Test->TestTrue(TEXT("Reversed range leaves view unchanged"),M.InspectionState().Equals(BeforeInvalid));
            if(auto Feedback=FindTag(TEXT("ColorFeedback")))Test->TestTrue(TEXT("Inline error explains range ordering"),Text(Feedback.ToSharedRef()).Contains(TEXT("below maximum")));
            Capture(TEXT("invalid-range.png"));
            EditText(TEXT("ColorMinimum"),TEXT("-123.45678901234567"));EditText(TEXT("ColorMaximum"),TEXT("1.2345678901234567e2"));Press(TEXT("ApplyColorRange"));Phase=9;break;
        case 9:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Exact minimum from visible input"),M.ActiveColorMapping().Minimum,-123.45678901234567);
            Test->TestEqual(TEXT("Exact scientific-notation maximum"),M.ActiveColorMapping().Maximum,123.45678901234567);
            BeforeInvalid=M.InspectionState();
            if(auto Input=FindTag(TEXT("ColorMinimum")))Key(Input,EKeys::Tab);
            Test->TestTrue(TEXT("Focus traversal does not round saved values"),M.InspectionState().Equals(BeforeInvalid));
            FSlateApplication::Get().DismissAllMenus();OpenMenu(TEXT("ScalarSelector"));Phase=10;break;
        case 10:Press(TEXT("Scalar_velocity_magnitude"));Phase=11;break;
        case 11:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestFalse(TEXT("New field uses original range"),M.ActiveColorMapping().bManualRange);
            Test->TestEqual(TEXT("New field uses its own palette"),M.ActiveColorMapping().Palette,0);
            OpenMenu(TEXT("ColorSettings"));Phase=12;break;
        case 12:Press(TEXT("ColorPalette2"));Phase=13;break;
        case 13:
            if(!Scene->HasCurrentFrame())return false;
            VerifyPointColors();Capture(TEXT("grayscale.png"));
            // Hold this source-bound popover while another command changes field.
            M.EditView(TEXT("Return to pressure"),[](auto& S){S.Display.ScalarField=TEXT("pressure");});Phase=14;break;
        case 14:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Returning field restores its palette"),M.ActiveColorMapping().Palette,1);
            Test->TestEqual(TEXT("Returning field restores exact custom range"),M.ActiveColorMapping().Minimum,-123.45678901234567);
            if(auto Context=FindTag(TEXT("ColorFieldContext")))Test->TestFalse(TEXT("Stale field popover is disabled"),Context->IsEnabled());
            BeforeInvalid=M.InspectionState();Key(FindTag(TEXT("ColorPalette0")),EKeys::Enter,false);
            Test->TestTrue(TEXT("Stale field action cannot modify current field"),M.InspectionState().Equals(BeforeInvalid));
            Capture(TEXT("changed-field.png"));FSlateApplication::Get().DismissAllMenus();Phase=24;break;
        case 24:
            // Slate deliberately suppresses reopening an anchor during the same
            // frame in which it was dismissed. A separate input frame matches
            // the user's close/reopen interaction.
            OpenMenu(TEXT("ColorSettings"));Phase=15;break;
        case 15:Press(TEXT("ResetColorRange"));Phase=16;break;
        case 16:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestFalse(TEXT("Source range reset operates through UI"),M.ActiveColorMapping().bManualRange);
            Test->TestEqual(TEXT("Reset uses actual source minimum"),M.ActiveColorMapping().Minimum,M.ActiveScalar().Minimum);
            Test->TestEqual(TEXT("Reset retains selected palette"),M.ActiveColorMapping().Palette,1);
            FSlateApplication::Get().DismissAllMenus();
            Test->TestTrue(TEXT("Reset can be undone as a display edit"),M.UndoView());Phase=17;break;
        case 17:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Undo restores custom range"),M.ActiveColorMapping().bManualRange);
            OpenMenu(TEXT("ColorSettings"));Phase=25;break;
        case 25:
            Press(TEXT("ColorPalette3"));Phase=26;break;
        case 26:
            EditText(TEXT("CustomColor0"),TEXT("#003366"));EditText(TEXT("CustomColor1"),TEXT("#33DD88"));EditText(TEXT("CustomColor2"),TEXT("#FFAA11"));
            Press(TEXT("ApplyCustomColors"));Phase=27;break;
        case 27:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Custom palette applied through visible controls"),Scene->PresentedColorMapping().Palette,3);
            Test->TestTrue(TEXT("Custom middle color uses exact requested sRGB value"),Scene->PresentedColorMapping().MiddleColor==FLinearColor::FromSRGBColor(FColor(51,221,136)));
            VerifyPointColors();Capture(TEXT("custom-colors.png"));
            BeforeInvalid=M.InspectionState();EditText(TEXT("CustomColor0"),TEXT("#INVALID"));Press(TEXT("ApplyCustomColors"));
            Test->TestTrue(TEXT("Invalid color input retains the rendered transfer function"),M.InspectionState().Equals(BeforeInvalid));
            FSlateApplication::Get().DismissAllMenus();
            Test->TestTrue(TEXT("Save colors"),M.SaveProject(Work/TEXT("colors.lbms")));Saved=M.SnapshotProject();
            Test->TestTrue(TEXT("Reopen saved styles"),M.RequestProjectOpen(Work/TEXT("colors.lbms")));Phase=18;break;
        case 18:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Saved project restored exactly"),StudioProjectIO::Serialize(M.SnapshotProject()),StudioProjectIO::Serialize(Saved));
            VerifyPointColors();Capture(TEXT("reopened.png"));IdleStart=Now;IdleCaptures=Scene->GetCaptureCount();Phase=19;break;
        case 19:
            if(Now-IdleStart<2)return false;
            Test->TestEqual(TEXT("Unchanged colors do not cause recurring scene captures"),Scene->GetCaptureCount(),IdleCaptures);
            Test->TestTrue(TEXT("Legacy source remains available"),M.RequestRecording(TEXT("MeshGraphNets_Airfoil_test009")));Phase=20;break;
        case 20:
            if(M.IsRecordingLoadPending()||!Scene->HasCurrentFrame())return false;
            M.EditView(TEXT("Legacy vectors"),[](auto& S){S.Display.bVectors=true;});
            M.SetScalarStyle(0,false,M.ActiveScalar().Minimum,M.ActiveScalar().Maximum);Phase=21;break;
        case 21:
            if(!Scene->HasCurrentFrame())return false;
            if(const auto* Section=RequireSection(4,TEXT("Legacy vector section before color edit")))
                for(const auto& V:Section->ProcVertexBuffer)VectorPositions.Add(V.Position);
            Test->TestTrue(TEXT("Legacy vectors exist"),!VectorPositions.IsEmpty());
            M.SetScalarStyle(2,true,0,100);Phase=22;break;
        case 22:
            if(!Scene->HasCurrentFrame())return false;
            if(const auto* Section=RequireSection(4,TEXT("Legacy vector section after color edit")))
            {
                Test->TestEqual(TEXT("Color range retains legacy vector count"),Section->ProcVertexBuffer.Num(),VectorPositions.Num());
                bool Same=Section->ProcVertexBuffer.Num()==VectorPositions.Num();
                for(int32 I=0;Same&&I<VectorPositions.Num();++I)Same=Section->ProcVertexBuffer[I].Position==VectorPositions[I];
                Test->TestTrue(TEXT("Color range never changes physical vector lengths or direction"),Same);
            }
            Test->TestTrue(TEXT("Restore original project"),M.RequestProjectOpen(Work/TEXT("original.lbms")));Phase=23;break;
        case 23:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            Test->TestEqual(TEXT("Original project restored"),M.Project.Id,Original.Id);
            Test->TestTrue(TEXT("Original camera restored"),StudioView::CameraEquals(Scene->SavedCameraState(),Original.Camera));
            IFileManager::Get().DeleteDirectory(*Work,false,true);return true;
        }
        return false;
    }
private:
    const FProcMeshSection* RequireSection(int32 Index,const TCHAR* Description)
    {
        auto* Mesh=Scene->FindComponentByClass<UProceduralMeshComponent>();
        if(!Test->TestNotNull(TEXT("Scientific mesh component"),Mesh))return nullptr;
        const auto* Section=Mesh->GetProcMeshSection(Index);
        Test->TestNotNull(Description,Section);
        return Section;
    }
    void VerifyPointColors()
    {
        const auto& M=*Scene->Model;
        const auto Field=M.Solver->CaptureViewField(M.SelectedFrame,M.ActiveScalar().Id,false);
        const auto Points=Field->OriginalPoints();
        const auto* Section=RequireSection(0,TEXT("Point glyph section"));
        if(!Section||!Test->TestTrue(TEXT("Original point values"),Points.IsValid()))return;
        const auto* Values=Points->FindValues(M.ActiveScalar().Id);
        if(!Test->TestNotNull(TEXT("Selected scalar supplied"),Values))return;
        if(!Test->TestEqual(TEXT("All source glyph vertices exist"),Section->ProcVertexBuffer.Num(),Values->Num()*18))return;
        for(int32 I:{0,1000,Values->Num()-1})
            Test->TestEqual(TEXT("Actual vertex colors use the legend's presented mapping"),Section->ProcVertexBuffer[I*18].Color,
                StudioColor::Map((*Values)[I],Scene->PresentedColorMapping()).ToFColor(false));
    }
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag,bool bButton=false)
    {
        if(!W->GetVisibility().IsVisible())return nullptr;
        if((!bButton&&W->GetTag()==Tag)||(bButton&&W->GetType()==TEXT("SButton")))return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)
            if(auto Found=Find(Children->GetChildAt(I),Tag,bButton))return Found;
        return nullptr;
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto Found=Find(W,Tag))return Found;
        Test->AddError(TEXT("Color control missing: ")+Tag.ToString());return nullptr;
    }
    FString Text(const TSharedRef<SWidget>& W)
    {
        FString Result;if(W->GetType()==TEXT("STextBlock"))Result=StaticCastSharedRef<STextBlock>(W)->GetText().ToString();
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)Result+=TEXT(" ")+Text(Children->GetChildAt(I));
        return Result;
    }
    void Key(const TSharedPtr<SWidget>& W,FKey K=EKeys::Enter,bool bExpectEnabled=true)
    {
        if(!W)return;if(bExpectEnabled)Test->TestTrue(TEXT("Color control enabled"),W->IsEnabled());
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(K,FModifierKeysState(),0,false,0,0));
        App.ProcessKeyUpEvent(FKeyEvent(K,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag){Key(FindTag(Tag));}
    void OpenMenu(FName Tag){if(const auto W=FindTag(Tag))Key(Find(W.ToSharedRef(),NAME_None,true));}
    void EditText(FName Tag,const FString& Value)
    {
        if(const auto W=FindTag(Tag))
        {
            const auto Box=StaticCastSharedPtr<SEditableTextBox>(W);FSlateApplication::Get().SetKeyboardFocus(Box,EFocusCause::Navigation);
            Box->SetText(FText::FromString(Value));
        }
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture color controls"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save color UI evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioProject Original,Saved;FStudioCameraState Camera;
    FStudioInspectionState BeforeInvalid;FString Root,Work;TArray<FVector> VectorPositions;
    int32 Phase=0,Frame=0;double Started=0,IdleStart=0;uint64 IdleCaptures=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioColorControlsTest,"Studio.Colors.ControlsAndPersistence",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioColorControlsTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioColorControlsCommand(this));return true;}
#endif
