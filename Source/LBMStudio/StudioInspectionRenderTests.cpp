#include "StudioScene.h"
#include "StudioProbeSampling.h"
#include "StudioPointRecording.h"
#include "StudioProbeProfile.h"
#include "StudioFileDialog.h"
#include "SStudioProbeProfile.h"
#include "Serialization/Csv/CsvParser.h"
#include "StudioSliceRendering.h"
#include "StudioVolume.h"
#include "StudioSnapshot.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ProceduralMeshComponent.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/HittestGrid.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "ImageUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioInspectionUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioInspectionUICommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioInspectionUICommand() override
    {
        RestoreInspectionMaterial();
        // Failure injection touches only this run's copy of an authentic array.
        if(!RetryArray.IsEmpty()&&IFileManager::Get().FileExists(*(RetryArray+TEXT(".held"))))
            IFileManager::Get().Move(*RetryArray,*(RetryArray+TEXT(".held")));
    }
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors())return true;
        if(FPlatformTime::Seconds()-Started>120){Test->AddError(FString::Printf(TEXT("Inspection UI timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+3)return false;
        auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||!Scene->HasCurrentFrame())return false;
        switch(Phase)
        {
        case 0:
        {
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/InspectionUI");IFileManager::Get().MakeDirectory(*Root,true);
            Original=M.SnapshotProject();OriginalWorkspace=M.Workspace;FString Error;
            if(!Test->TestTrue(TEXT("Preserve preceding project"),StudioProjectIO::Save(Root/TEXT("original.lbms"),Original,Error)))return true;
            M.NewProject(TEXT("Saved wing inspection"));M.Navigate(EStudioWorkspace::Solve);M.Pause();Next(1);break;
        }
        case 1:Press(TEXT("InspectionTools"));Next(2);break;
        case 2:Capture(TEXT("empty.png"));Add(0);Next(3);break;
        case 3:
            if(!Test->TestEqual(TEXT("Create first slice through UI"),M.InspectionObjects.Slices.Num(),1))return true;
            Slice=M.SelectedInspectionObject;Type(TEXT("InspectionName"),TEXT("Wing midplane"));Next(4);break;
        case 4:
            Test->TestEqual(TEXT("UI rename stored"),M.FindSlice(Slice)->Name,FString(TEXT("Wing midplane")));
            Test->TestFalse(TEXT("Named slice produced sampled cells"),Scene->PresentedSliceNotice(Slice).IsEmpty());
            Type(TEXT("InspectionNormalX"),TEXT("3"));Type(TEXT("InspectionNormalY"),TEXT("4"));Type(TEXT("InspectionNormalZ"),TEXT("0"));
            Test->TestTrue(TEXT("Normal components remain a draft until Apply"),M.FindSlice(Slice)->Normal==FVector::RightVector);
            Press(TEXT("InspectionApplyNormal"));Next(31);break;
        case 5:
            Test->TestEqual(TEXT("Second slice has its own identity"),M.InspectionObjects.Slices.Num(),2);
            Test->TestTrue(TEXT("Slice IDs differ"),M.SelectedInspectionObject!=Slice);Capture(TEXT("slices.png"));Add(1);Next(6);break;
        case 6:
            Probe=M.SelectedInspectionObject;
            if(!Test->TestNotNull(TEXT("Point probe created"),M.FindProbe(Probe)))return true;
            Type(TEXT("InspectionAX"),TEXT("0.03250676393508911"));Type(TEXT("InspectionAY"),TEXT("0"));Type(TEXT("InspectionAZ"),TEXT("0.1194048523902893"));
            M.EditView(TEXT("Pressure field"),[](auto& V){V.Display.ScalarField=TEXT("pressure");});Next(7);break;
        case 7:
        {
            Test->TestEqual(TEXT("Typed probe retains exact X"),M.FindProbe(Probe)->A.X,.03250676393508911);
            FStudioProbeRequest R;R.ProjectId=M.Project.Id;R.PresentationId=Scene->GetCaptureCount();R.Probe=*M.FindProbe(Probe);R.DisplayedScalar=Scene->PresentedScalar().Id;R.Field=Scene->PresentedField();
            const auto Sample=StudioProbeSampling::Evaluate(R);
            if(!Test->TestTrue(TEXT("UI coordinates sample original recorded pressure"),Sample.Samples.Num()==1&&Sample.Samples[0].Value.IsSet()))return true;
            Test->TestTrue(TEXT("Known original pressure preserved"),FMath::IsNearlyEqual(Sample.Samples[0].Value.GetValue(),98699.78125,1.e-5));
            Next(8);break;
        }
        case 8:Capture(TEXT("point-probe.png"));Add(3);Next(9);break;
        case 9:
            Ruler=M.SelectedInspectionObject;Type(TEXT("InspectionAX"),TEXT("0"));Type(TEXT("InspectionAY"),TEXT("0"));Type(TEXT("InspectionAZ"),TEXT("0"));
            Type(TEXT("InspectionBX"),TEXT("0.03"));Type(TEXT("InspectionBY"),TEXT("0"));Type(TEXT("InspectionBZ"),TEXT("0.04"));Next(10);break;
        case 10:
        {
            const auto Value=StudioInspectionObjects::Measurement(*M.FindRuler(Ruler));
            Test->TestTrue(TEXT("UI ruler measures the known 3-4-5 distance"),Value.IsSet()&&FMath::IsNearlyEqual(Value.GetValue(),.05,1.e-12));
            const auto Before=*M.FindRuler(Ruler);Observer=Scene->SavedCameraState();Source=M.Project.Dataset;Frame=M.SelectedFrame;
            Press(TEXT("PlaceInspection"));ClickScene(.36,.45);
            Test->TestTrue(TEXT("First placement point remains an uncommitted draft"),*M.FindRuler(Ruler)==Before);
            Escape();Test->TestTrue(TEXT("Escape retains saved ruler coordinates"),*M.FindRuler(Ruler)==Before);
            Capture(TEXT("ruler.png"));RulerBefore=Before;
            Press(TEXT("PlaceInspection"));HoverWorld(FVector(0,0,.2));Next(35);break;
        }
        case 11:
            Line=M.SelectedInspectionObject;Type(TEXT("InspectionAX"),TEXT("-0.3"));Type(TEXT("InspectionAY"),TEXT("0"));Type(TEXT("InspectionAZ"),TEXT("0.3"));
            Type(TEXT("InspectionBX"),TEXT("0.7"));Type(TEXT("InspectionBY"),TEXT("0"));Type(TEXT("InspectionBZ"),TEXT("0.3"));Type(TEXT("InspectionSamples"),TEXT("8"));
            Press(TEXT("PlaceInspection"));ClickWorld(FVector(-.3,0,.3));Next(42);break;
        case 12:
            if(!Readout().Contains(TEXT("8/8 samples")))return false;
            Test->TestEqual(TEXT("Line sample count is saved"),M.FindProbe(Line)->Samples,8);
            Test->TestTrue(TEXT("Object edits preserve camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Observer));
            Test->TestEqual(TEXT("Object edits preserve source"),M.Project.Dataset,Source);Test->TestEqual(TEXT("Object edits preserve physical frame"),M.SelectedFrame,Frame);
            ExpectedLine=*M.FindProbe(Line);
            FSlateApplication::Get().SetKeyboardFocus(FindTag(TEXT("InspectionProfile")),EFocusCause::Navigation);
            Key(EKeys::Home);Key(EKeys::Right);
            Test->TestEqual(TEXT("Right arrow advances exactly one profile sample"),StaticCastSharedPtr<SStudioProbeProfile>(FindTag(TEXT("InspectionProfile")))->SelectedSample(),1);
            Next(25);break;
        case 13:
            Test->TestTrue(TEXT("Every saved object reopens exactly"),M.InspectionObjects==Expected);
            Test->TestTrue(TEXT("Saved inspection retains exact camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Observer));
            M.SelectInspectionObject(Probe);Next(14);break;
        case 14:
            Capture(TEXT("reopened.png"));
            PointCopy=Root/TEXT("source-copy")/FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*PointCopy,true);
            for(const TCHAR* File:{TEXT("recording.json"),TEXT("coordinates.f64"),TEXT("point-ids.i64"),TEXT("velocity_u.f64"),TEXT("velocity_v.f64"),
                TEXT("velocity_magnitude.f64"),TEXT("pressure.f64"),TEXT("cell_volume.f64"),TEXT("provenance.json"),TEXT("ATTRIBUTION.txt")})
                Test->TestEqual(TEXT("Copy authentic fixture for recoverable read-failure test"),IFileManager::Get().Copy(*(PointCopy/File),
                    *(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture")/File)),uint32(COPY_OK));
            Test->TestTrue(TEXT("Open authentic source-point fixture"),M.RequestExternalRecording(PointCopy/TEXT("recording.json")));
            Next(15);break;
        case 15:
            Scene->FitCamera();M.EditView(TEXT("Original pressure"),[](auto& V){V.Display.ScalarField=TEXT("pressure");});Add(1);Next(16);break;
        case 16:
            OriginalProbe=M.SelectedInspectionObject;
            if(auto Menu=FindTag(TEXT("InspectionSampling")))Enter(Find(Menu.ToSharedRef(),NAME_None,true));
            Press(TEXT("InspectionSampling1"));Next(17);break;
        case 17:
            Test->TestTrue(TEXT("Original sampling mode is stored"),M.FindProbe(OriginalProbe)->Method==EStudioProbeMethod::OriginalPoint);
            Type(TEXT("InspectionPointId"),TEXT("9223372036854775807"));Next(18);break;
        case 18:
            Test->TestEqual(TEXT("Text input retains all 64 ID bits"),M.FindProbe(OriginalProbe)->PointId.GetValue(),MAX_int64);
            if(!Readout().Contains(TEXT("original point ID not found")))return false;
            Capture(TEXT("missing-point-id.png"));
            Type(TEXT("InspectionPointId"),TEXT("9223372036854775808"));
            Test->TestEqual(TEXT("Overflow input cannot change stored ID"),M.FindProbe(OriginalProbe)->PointId.GetValue(),MAX_int64);
            Type(TEXT("InspectionPointId"),TEXT("17"));Next(19);break;
        case 19:
            if(!Readout().Contains(TEXT("9.552859306 Pa")))return false;
            Test->TestEqual(TEXT("Exact original ID entered through UI"),M.FindProbe(OriginalProbe)->PointId.GetValue(),int64(17));
            Capture(TEXT("original-point-id.png"));
            // Focus traversal must not round IDs; history must refresh text in
            // the same existing editor, not resurrect a previous edit on blur.
            Type(TEXT("InspectionName"),TEXT("Original pressure reference"));
            M.EndViewEdit();Test->TestTrue(TEXT("Undo original probe name"),M.UndoView());Next(20);break;
        case 20:
            Test->TestEqual(TEXT("Undo restores saved name"),M.FindProbe(OriginalProbe)->Name,FString(TEXT("Point probe 2")));
            Type(TEXT("InspectionPointId"),TEXT("100"));Next(21);break;
        case 21:
            if(!Readout().Contains(TEXT("8.365919113 Pa")))return false;
            Test->TestEqual(TEXT("Moving focus after undo keeps restored name"),M.FindProbe(OriginalProbe)->Name,FString(TEXT("Point probe 2")));
            FieldObserver=Scene->SavedCameraState();FieldSource=M.Project.Dataset;FieldFrame=M.SelectedFrame;
            OpenFieldMenu();Next(44);break;
        case 22:
            Test->TestTrue(TEXT("Original IDs and source bindings reopen exactly"),M.InspectionObjects==Expected);
            M.SelectInspectionObject(OriginalProbe);Next(23);break;
        case 23:
            if(!Readout().Contains(TEXT("8.365919113 Pa")))return false;
            Test->TestEqual(TEXT("Fixed pressure selection survives reopen"),M.FindProbe(OriginalProbe)->Field,FString(TEXT("pressure")));
            Test->TestEqual(TEXT("Viewport scalar remains independently saved"),Scene->PresentedScalar().Id,FString(TEXT("velocity_u")));
            Capture(TEXT("original-point-reopened.png"));OpenFieldMenu();Press(TEXT("InspectionFieldFollow"));Next(51);break;
        case 24:M.Navigate(OriginalWorkspace);return true;
        case 25:
            Test->TestEqual(TEXT("Keyboard profile selection survives layout ticks"),StaticCastSharedPtr<SStudioProbeProfile>(FindTag(TEXT("InspectionProfile")))->SelectedSample(),1);
            Capture(TEXT("line-profile.png"));
            CSVPath=Root/TEXT("captured-line.csv");IFileManager::Get().Delete(*CSVPath);
            StudioFileDialog::SetNextProbeCSVForAutomation(CSVPath);Press(TEXT("ExportProbeCSV"));
            M.Scrub(.75);Next(26);break;
        case 26:
        {
            FString CSV;if(!FFileHelper::LoadFileToString(CSV,*CSVPath))return false;
            FCsvParser Parser(CSV);const auto& Rows=Parser.GetRows();
            if(!Test->TestEqual(TEXT("Native export contains all eight line samples"),Rows.Num(),9))return true;
            for(int32 I=1;I<Rows.Num();++I)
            {
                if(!Test->TestEqual(TEXT("Native export has every metadata/sample column"),Rows[I].Num(),31))return true;
                Test->TestEqual(TEXT("Export keeps click-time physical frame after scrub"),FCString::Atoi(Rows[I][13]),0);
                Test->TestEqual(TEXT("Export keeps click-time scalar"),FString(Rows[I][15]),FString(TEXT("pressure")));
                Test->TestEqual(TEXT("Export keeps source identity"),FString(Rows[I][7]),Source);
            }
            Test->TestTrue(TEXT("Scene changed frame while export remained frozen"),Scene->PresentedFrame().Index!=0);
            M.Scrub(0);Next(27);break;
        }
        case 27:
            if(!Readout().Contains(TEXT("8/8 samples")))return false;
            if(const auto B=FindTag(TEXT("ExportProbeCSV"));!B||!B->IsEnabled())return false;
            StudioFileDialog::SetNextProbeCSVForAutomation(FString());Press(TEXT("ExportProbeCSV"));
            Test->TestEqual(TEXT("Native save cancellation is explicit"),M.InspectionNotice,FString(TEXT("Probe export cancelled.")));
            Type(TEXT("InspectionAX"),TEXT("-0.8"));Type(TEXT("InspectionAZ"),TEXT("0"));
            Type(TEXT("InspectionBX"),TEXT("0.9"));Type(TEXT("InspectionBZ"),TEXT("0"));Type(TEXT("InspectionSamples"),TEXT("65"));Next(28);break;
        case 28:
        {
            if(!Readout().Contains(TEXT("/65 samples")))return false;
            FStudioProbeRequest R;R.ProjectId=M.Project.Id;R.PresentationId=Scene->GetCaptureCount();R.Probe=*M.FindProbe(Line);R.DisplayedScalar=Scene->PresentedScalar().Id;R.Field=Scene->PresentedField();
            const auto Profile=StudioProbeProfile::Build(StudioProbeSampling::Evaluate(R));
            Test->TestTrue(TEXT("Displayed line crosses the real solid gap"),Profile&&Profile->Segments.Num()>=2&&Profile->ValidSamples<65);
            FSlateApplication::Get().SetKeyboardFocus(FindTag(TEXT("InspectionProfile")),EFocusCause::Navigation);Key(EKeys::Home);Key(EKeys::Right);Next(29);break;
        }
        case 29:
            Capture(TEXT("line-profile-gaps.png"));M.EndViewEdit();M.EditProbe(Line,[this](auto& P){P=ExpectedLine;});M.EndViewEdit();Next(30);break;
        case 30:
            if(!Readout().Contains(TEXT("8/8 samples")))return false;
            Test->TestTrue(TEXT("Chart navigation/export preserve camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Observer));
            Expected=M.InspectionObjects;Test->TestTrue(TEXT("Save all named inspection objects"),M.SaveProject(Root/TEXT("inspection.lbms")));
            M.NewProject(TEXT("Different inspection"));Test->TestTrue(TEXT("Reopen inspection"),M.RequestProjectOpen(Root/TEXT("inspection.lbms")));Next(13);break;
        case 31:
            Test->TestTrue(TEXT("Apply normal normalizes all three components together"),M.FindSlice(Slice)->Normal.Equals(FVector(.6,.8,0),1.e-12));
            Capture(TEXT("slice-normal.png"));
            Type(TEXT("InspectionNormalX"),TEXT("0.9"));
            Test->TestTrue(TEXT("Undo the applied normal"),M.UndoView());
            Press(TEXT("InspectionApplyNormal"));
            Test->TestTrue(TEXT("Immediate Apply after Undo cannot restore the obsolete vector draft"),M.FindSlice(Slice)->Normal==FVector::RightVector);
            Next(32);break;
        case 32:
            Type(TEXT("InspectionNormalX"),TEXT("0"));Type(TEXT("InspectionNormalY"),TEXT("0"));Type(TEXT("InspectionNormalZ"),TEXT("0"));
            Press(TEXT("InspectionApplyNormal"));
            Test->TestTrue(TEXT("Zero normal is rejected without changing the slice"),M.FindSlice(Slice)->Normal==FVector::RightVector);
            Test->TestTrue(TEXT("Zero normal rejection explains the correction"),M.InspectionNotice.Contains(TEXT("nonzero")));
            Type(TEXT("InspectionNormalX"),TEXT("nan"));
            Test->TestTrue(TEXT("Nonfinite component is rejected explicitly"),M.InspectionNotice.Contains(TEXT("finite")));
            Type(TEXT("InspectionNormalX"),TEXT("3e-200"));Type(TEXT("InspectionNormalY"),TEXT("4e-200"));
            Press(TEXT("InspectionApplyNormal"));Next(33);break;
        case 33:
            Test->TestTrue(TEXT("Finite tiny normal avoids norm underflow"),M.FindSlice(Slice)->Normal.Equals(FVector(.6,.8,0),1.e-12));
            Type(TEXT("InspectionNormalX"),TEXT("0.25"),false);
            Test->TestTrue(TEXT("Undo while a normal text field has an uncommitted edit"),M.UndoView());
            Key(EKeys::Enter);Press(TEXT("InspectionApplyNormal"));
            Test->TestTrue(TEXT("Stale Enter and Apply preserve the restored vector"),M.FindSlice(Slice)->Normal==FVector::RightVector);
            Press(TEXT("InspectionNormalAxisZ"));
            Test->TestTrue(TEXT("Named slice axis preset applies through UI"),M.FindSlice(Slice)->Normal==FVector::UpVector);
            Test->TestTrue(TEXT("Axis preset is independently undoable"),M.UndoView());
            Press(TEXT("InspectionApplyNormal"));
            Test->TestTrue(TEXT("Axis undo remains exact after focus changes"),M.FindSlice(Slice)->Normal==FVector::RightVector);
            Next(34);break;
        case 34:
            Capture(TEXT("slice-normal-restored.png"));Add(0);Next(5);break;
        case 35:
            HoverWorld(FVector(0,0,.2));Capture(TEXT("ruler-placement-preview.png"));ClickWorld(FVector(0,0,.2));
            Test->TestTrue(TEXT("First visible endpoint stays uncommitted"),*M.FindRuler(Ruler)==RulerBefore);
            Test->TestTrue(TEXT("Viewport prompts for next endpoint"),PlacementHint().Contains(TEXT("Click B")));
            LookScene(20,10);Next(36);break;
        case 36:
            HoverWorld(FVector(.35,0,.2));Capture(TEXT("ruler-placement-camera.png"));ClickWorld(FVector(.35,0,.2));
            PlacementObserver=Scene->SavedCameraState();
            Test->AddInfo(FString::Printf(TEXT("Placed ruler A=(%.17g,%.17g,%.17g) B=(%.17g,%.17g,%.17g), distance %.17g; endpoint error %.17g / %.17g m"),
                M.FindRuler(Ruler)->A.X,M.FindRuler(Ruler)->A.Y,M.FindRuler(Ruler)->A.Z,
                M.FindRuler(Ruler)->B.X,M.FindRuler(Ruler)->B.Y,M.FindRuler(Ruler)->B.Z,
                (M.FindRuler(Ruler)->B-M.FindRuler(Ruler)->A).Size(),
                (M.FindRuler(Ruler)->A-FVector(0,0,.2)).Size(),(M.FindRuler(Ruler)->B-FVector(.35,0,.2)).Size()));
            // Slate Geometry converts screen coordinates to float: the measured
            // ~3e-5 pixel round trip produces ~0.25 micrometre endpoint error at
            // this fixture's scale. Numeric edits and pure geometry stay exact.
            Test->TestTrue(TEXT("Ruler endpoints match clicked world positions within one micrometre"),M.FindRuler(Ruler)->A.Equals(FVector(0,0,.2),1.e-6)&&M.FindRuler(Ruler)->B.Equals(FVector(.35,0,.2),1.e-6));
            Test->TestTrue(TEXT("Click placement retains the exact 2D plane"),M.FindRuler(Ruler)->A.Y==0&&M.FindRuler(Ruler)->B.Y==0);
            Test->TestTrue(TEXT("Placed ruler measures known distance"),MeasurementNear(Ruler,.35));
            Test->TestTrue(TEXT("Two clicks form one undoable object edit"),M.UndoView());
            Test->TestTrue(TEXT("One undo restores both old endpoints"),*M.FindRuler(Ruler)==RulerBefore);
            Test->TestTrue(TEXT("Object undo retains changed observer camera"),StudioView::CameraEquals(Scene->SavedCameraState(),PlacementObserver));
            Scene->RestoreCamera(Observer,TEXT("Restore observer after placement"));Add(4);Next(37);break;
        case 37:
            Angle=M.SelectedInspectionObject;AngleBefore=*M.FindRuler(Angle);
            Press(TEXT("PlaceInspection"));Escape();
            Test->TestFalse(TEXT("Escape cancels before the first viewport click"),PlacementHint().Contains(TEXT("PLACE")));
            Press(TEXT("PlaceInspection"));ClickWorld(FVector(0,0,.2));ClickWorld(FVector(.35,0,.2));Next(38);break;
        case 38:
            HoverWorld(FVector(.35,0,.5));Capture(TEXT("angle-placement-preview.png"));
            Test->TestTrue(TEXT("Two angle clicks keep all saved coordinates untouched"),*M.FindRuler(Angle)==AngleBefore);
            ClickWorld(FVector(.35,0,.5));Next(39);break;
        case 39:
            Test->TestTrue(TEXT("Three clicks commit the known right angle"),MeasurementNear(Angle,90.,.001));
            Test->TestEqual(TEXT("Placement preserves physical frame"),M.SelectedFrame,Frame);
            Test->TestEqual(TEXT("Placement preserves source"),M.Project.Dataset,Source);
            AngleBefore=*M.FindRuler(Angle);
            Press(TEXT("PlaceInspection"));ClickWorld(FVector(0,0,.2));M.SelectInspectionObject(Ruler);
            ClickWorld(FVector(.35,0,.2));
            Test->TestTrue(TEXT("Selection change discards the old object's partial gesture"),*M.FindRuler(Angle)==AngleBefore&&*M.FindRuler(Ruler)==RulerBefore);
            Test->TestTrue(TEXT("Stale final click cannot orbit the camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Observer));
            Next(40);break;
        case 40:Add(2);Next(11);break;
        case 42:
            HoverWorld(FVector(.7,0,.3));Capture(TEXT("line-placement-preview.png"));ClickWorld(FVector(.7,0,.3));Next(43);break;
        case 43:
            Test->TestTrue(TEXT("Line probe accepts both scene endpoints within one micrometre"),M.FindProbe(Line)->A.Equals(FVector(-.3,0,.3),1.e-6)&&M.FindProbe(Line)->B.Equals(FVector(.7,0,.3),1.e-6));
            OpenFieldMenu();Press(TEXT("InspectionField_pressure"));Next(50);break;
        case 50:
            Test->TestEqual(TEXT("Line probe supports a fixed source field"),M.FindProbe(Line)->Field,FString(TEXT("pressure")));
            M.EditView(TEXT("Velocity display with pressure line"),[](auto& V){V.Display.ScalarField=TEXT("velocity_magnitude");});
            Next(12);break;
        case 44:
            Capture(TEXT("probe-field-menu.png"));Press(TEXT("InspectionField_pressure"));Next(45);break;
        case 45:
            Test->TestEqual(TEXT("Fixed source pressure is stored"),M.FindProbe(OriginalProbe)->Field,FString(TEXT("pressure")));
            M.EndViewEdit();Test->TestTrue(TEXT("Fixed field selection is undoable"),M.UndoView());
            Test->TestTrue(TEXT("Undo restores following viewport"),M.FindProbe(OriginalProbe)->Field.IsEmpty());
            Test->TestTrue(TEXT("Fixed field selection can be redone"),M.RedoView());
            M.EditView(TEXT("Speed display with pressure probe"),[](auto& V){V.Display.ScalarField=TEXT("velocity_magnitude");});Next(46);break;
        case 46:
        {
            if(!Readout().Contains(TEXT("8.365919113 Pa")))return false;
            const auto Points=Scene->PresentedField()->OriginalPoints();
            if(!Test->TestTrue(TEXT("Displayed speed snapshot has original points"),Points.IsValid()))return true;
            Test->TestNull(TEXT("Pressure samples did not mutate the speed snapshot"),Points->FindValues(TEXT("pressure")));
            Test->TestEqual(TEXT("Probe selection leaves displayed speed unchanged"),Scene->PresentedScalar().Id,FString(TEXT("velocity_magnitude")));
            Test->TestTrue(TEXT("Probe scalar preserves camera"),StudioView::CameraEquals(Scene->SavedCameraState(),FieldObserver));
            Test->TestEqual(TEXT("Probe scalar preserves source"),M.Project.Dataset,FieldSource);
            Test->TestEqual(TEXT("Probe scalar preserves physical frame"),M.SelectedFrame,FieldFrame);
            Capture(TEXT("fixed-pressure-speed-view.png"));
            CSVPath=Root/TEXT("fixed-pressure.csv");IFileManager::Get().Delete(*CSVPath);
            StudioFileDialog::SetNextProbeCSVForAutomation(CSVPath);Press(TEXT("ExportProbeCSV"));
            M.EditView(TEXT("Velocity X display"),[](auto& V){V.Display.ScalarField=TEXT("velocity_u");});Next(47);break;
        }
        case 47:
        {
            FString CSV;if(!FFileHelper::LoadFileToString(CSV,*CSVPath)||!Readout().Contains(TEXT("8.365919113 Pa")))return false;
            FCsvParser Parser(CSV);const auto& Rows=Parser.GetRows();
            if(!Test->TestEqual(TEXT("Fixed pressure CSV contains one original point"),Rows.Num(),2)||
                !Test->TestEqual(TEXT("Fixed pressure CSV preserves metadata columns"),Rows[1].Num(),31))return true;
            Test->TestEqual(TEXT("CSV scalar is pressure independently of viewport"),FString(Rows[1][15]),FString(TEXT("pressure")));
            Test->TestEqual(TEXT("CSV units belong to pressure"),FString(Rows[1][17]),FString(TEXT("Pa")));
            Test->TestEqual(TEXT("CSV retains original point ID"),FCString::Atoi64(Rows[1][28]),int64(100));
            Test->TestEqual(TEXT("CSV retains the original pressure value"),FCString::Atod(Rows[1][30]),8.365919113);
            Test->TestEqual(TEXT("CSV keeps original physical step"),FCString::Atoi(Rows[1][13]),1001);
            M.Scrub(1.);Next(48);break;
        }
        case 48:
            if(!Readout().Contains(TEXT("8.117749214 Pa")))return false;
            Test->TestEqual(TEXT("Fixed scalar follows the actual presented frame"),Scene->PresentedFrame().Index,9000);
            Capture(TEXT("fixed-pressure-scrubbed.png"));M.Scrub(0);Next(49);break;
        case 49:
            if(!Readout().Contains(TEXT("8.365919113 Pa")))return false;
            Test->TestTrue(TEXT("Save original ID and fixed field"),M.SaveProject(Root/TEXT("original-point.lbms")));
            Expected=M.InspectionObjects;M.NewProject(TEXT("Before ID reopen"));
            Test->TestTrue(TEXT("Reopen exact source-point probe"),M.RequestProjectOpen(Root/TEXT("original-point.lbms")));Next(22);break;
        case 51:
            if(!Readout().Contains(TEXT("24.92522621 m/s")))return false;
            Test->TestTrue(TEXT("Follow viewport menu clears the fixed selection"),M.FindProbe(OriginalProbe)->Field.IsEmpty());
            M.EditProbe(OriginalProbe,[](auto& P){P.Field=TEXT("density");});Next(52);break;
        case 52:
            if(!Readout().Contains(TEXT("not supplied by this recording")))return false;
            Capture(TEXT("probe-field-unavailable.png"));OpenFieldMenu();Press(TEXT("InspectionField_pressure"));Next(53);break;
        case 53:
            if(!Readout().Contains(TEXT("8.365919113 Pa")))return false;
            RetryArray=PointCopy/TEXT("cell_volume.f64");
            Test->TestTrue(TEXT("Temporarily remove only the private optional array copy"),IFileManager::Get().Move(*(RetryArray+TEXT(".held")),*RetryArray));
            OpenFieldMenu();Press(TEXT("InspectionField_cell_volume"));Next(54);break;
        case 54:
            if(!Readout().Contains(TEXT("Could not load probe samples")))return false;
            Test->TestFalse(TEXT("Failed optional read disables sample export"),FindTag(TEXT("ExportProbeCSV"))->IsEnabled());
            Test->TestEqual(TEXT("Failed probe read retains the velocity viewport"),Scene->PresentedScalar().Id,FString(TEXT("velocity_u")));
            Test->TestTrue(TEXT("Failed probe read leaves the source usable"),M.Solver->LoadError().IsEmpty());
            Capture(TEXT("probe-retry-needed.png"));
            Test->TestTrue(TEXT("Restore unchanged private source array"),IFileManager::Get().Move(*RetryArray,*(RetryArray+TEXT(".held"))));
            Press(TEXT("RetryProbeSamples"));Next(55);break;
        case 55:
            if(!Readout().Contains(TEXT("0.0002105111198 unspecified")))return false;
            Capture(TEXT("probe-retry-recovered.png"));OpenFieldMenu();Press(TEXT("InspectionField_pressure"));Next(56);break;
        case 56:
            if(!Readout().Contains(TEXT("8.365919113 Pa")))return false;
            BeforeSourceObjects=M.InspectionObjects;OldField=Scene->PresentedField();OldSolver=M.Solver;
            FieldObserver=Scene->SavedCameraState();CaseState=StudioCaseIO::Serialize(M.Project.Draft);
            Test->TestTrue(TEXT("Switch to authentic 3D source with existing saved objects"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json")));Next(60);break;
        case 60:
            if(!Readout().Contains(TEXT("different recording")))return false;
            Test->TestTrue(TEXT("Source switch preserves all saved objects"),M.InspectionObjects==BeforeSourceObjects);
            Test->TestFalse(TEXT("Foreign probe cannot select another source's scalar"),FindTag(TEXT("InspectionField"))->IsEnabled());
            Test->TestFalse(TEXT("Foreign probe cannot export misleading samples"),FindTag(TEXT("ExportProbeCSV"))->IsEnabled());
            Test->TestTrue(TEXT("Source switch keeps the camera"),StudioView::CameraEquals(Scene->SavedCameraState(),FieldObserver));
            Test->TestEqual(TEXT("Source switch keeps editable physics"),StudioCaseIO::Serialize(M.Project.Draft),CaseState);
            Test->TestFalse(TEXT("Old field snapshot releases after source publication"),OldField.IsValid());
            Test->TestFalse(TEXT("Old source solver is not retained by inspection"),OldSolver.IsValid());
            Capture(TEXT("probe-other-source.png"));
            Test->TestTrue(TEXT("Attach the verified 3D reconstruction"),M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json")));Next(61);break;
        case 61:
            if(!Test->TestTrue(TEXT("Genuine 3D interpolation is attached"),M.Solver->Descriptor().SpatialDimensions==3&&M.Solver->VolumeReconstruction().IsValid()))return true;
            Scene->FitCamera();M.EditView(TEXT("Named 3D slice inspection"),[](auto& V)
            {V.Display.bVolume=false;V.Display.bVolumeIsosurface=false;V.Display.bCutPlane=false;V.Display.bSourcePoints=false;V.Display.bVectors=false;V.Display.bStreamlines=false;V.Display.ScalarField=TEXT("velocity_magnitude");});
            Add(0);Next(62);break;
        case 62:
            VolumeSlice=M.SelectedInspectionObject;Type(TEXT("InspectionName"),TEXT("Oblique cylinder slice"));
            Type(TEXT("InspectionNormalX"),TEXT("1"));Type(TEXT("InspectionNormalY"),TEXT("2"));Type(TEXT("InspectionNormalZ"),TEXT("3"));Press(TEXT("InspectionApplyNormal"));Next(63);break;
        case 63:
        {
            Test->TestTrue(TEXT("Named arbitrary 3D plane produced source cells"),Scene->PresentedSliceNotice(VolumeSlice).Contains(TEXT("Sampled")));
            Test->TestTrue(TEXT("Arbitrary 3D normal is stored"),M.FindSlice(VolumeSlice)->Normal.Equals(FVector(1,2,3).GetSafeNormal(),1.e-12));
            const auto Mesh=Scene->FindComponentByClass<UProceduralMeshComponent>();
            Test->TestTrue(TEXT("Supported named 3D slice reaches the GPU mesh and material"),Mesh&&Mesh->GetProcMeshSection(8)&&
                Mesh->GetProcMeshSection(8)->ProcIndexBuffer.Num()>12000&&Mesh->GetMaterial(8));
            TArray<FColor> Pixels;
            Test->TestTrue(TEXT("Read the actual 3D slice render target"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels));
            int32 Colored=0;for(const auto& P:Pixels)if(FMath::Max3(P.R,P.G,P.B)>80&&FMath::Max3(P.R,P.G,P.B)-FMath::Min3(P.R,P.G,P.B)>40)++Colored;
            Test->TestTrue(TEXT("Named slice alone produces visible scientific color pixels"),Colored>1000);
            Capture(TEXT("three-d-oblique-slice.png"));
            // Remove only this scene's material reference; the original asset
            // remains strongly held and is restored even if the test fails.
            InspectionMaterialProperty=FindFProperty<FObjectPropertyBase>(AStudioScene::StaticClass(),TEXT("InspectionInstance"));
            if(!Test->TestNotNull(TEXT("Inspection material is a reflected GC reference"),InspectionMaterialProperty))return true;
            HeldInspectionMaterial.Reset(InspectionMaterialProperty->GetObjectPropertyValue_InContainer(Scene.Get()));
            InspectionMaterialProperty->SetObjectPropertyValue_InContainer(Scene.Get(),nullptr);
            MaterialField=Scene->PresentedFrame().Index;MaterialCamera=Scene->SavedCameraState();MaterialObjects=M.InspectionObjects;
            M.DisplayChanged();Next(57);break;
        }
        case 57:
        {
            if(!Readout().Contains(TEXT("Slice rendering is unavailable")))return false;
            const auto* Section=Scene->FindComponentByClass<UProceduralMeshComponent>()->GetProcMeshSection(8);
            Test->TestTrue(TEXT("Missing slice material clears stale scientific geometry"),!Section||Section->ProcIndexBuffer.IsEmpty());
            Test->TestTrue(TEXT("Missing material leaves the original field available"),Scene->PresentedField().IsValid());
            Test->TestEqual(TEXT("Material failure keeps physical time"),Scene->PresentedFrame().Index,MaterialField);
            Test->TestTrue(TEXT("Material failure keeps camera and saved inspection"),StudioView::CameraEquals(Scene->SavedCameraState(),MaterialCamera)&&M.InspectionObjects==MaterialObjects);
            Capture(TEXT("slice-rendering-unavailable.png"));RestoreInspectionMaterial();M.DisplayChanged();Next(58);break;
        }
        case 58:
        {
            if(!Readout().Contains(TEXT("Sampled")))return false;
            const auto* Section=Scene->FindComponentByClass<UProceduralMeshComponent>()->GetProcMeshSection(8);
            Test->TestTrue(TEXT("Restored material renders the same slice"),Section&&Section->ProcIndexBuffer.Num()>12000);
            Capture(TEXT("slice-rendering-restored.png"));Add(1);Next(64);break;
        }
        case 64:
            VolumeProbe=M.SelectedInspectionObject;
            Type(TEXT("InspectionAX"),TEXT("0.07635210442046325"));Type(TEXT("InspectionAY"),TEXT("0.04232257921549101"));Type(TEXT("InspectionAZ"),TEXT("0.0031429173820835404"));
            OpenFieldMenu();Press(TEXT("InspectionField_pressure"));Next(65);break;
        case 65:
            // Original-HDF5-backed independent volume query, source XYZ -> scene XZY.
            if(!Readout().Contains(TEXT("-0.2300278575 Pa")))return false;
            Test->TestEqual(TEXT("3D pressure probe is independent of visible slice scalar"),Scene->PresentedScalar().Id,FString(TEXT("velocity_magnitude")));
            Test->TestEqual(TEXT("Probe uses original physical 3D step"),Scene->PresentedFrame().Index,2200);
            Capture(TEXT("three-d-pressure-probe.png"));Expected=M.InspectionObjects;FieldObserver=Scene->SavedCameraState();
            Test->TestTrue(TEXT("Save source-bound 2D and 3D inspection together"),M.SaveProject(Root/TEXT("three-d-inspection.lbms")));
            M.NewProject(TEXT("Before 3D reopen"));Test->TestTrue(TEXT("Reopen 3D inspection"),M.RequestProjectOpen(Root/TEXT("three-d-inspection.lbms")));Next(66);break;
        case 66:
            Test->TestTrue(TEXT("All mixed-source objects reopen exactly"),M.InspectionObjects==Expected);
            Test->TestTrue(TEXT("3D view reopens exactly"),StudioView::CameraEquals(Scene->SavedCameraState(),FieldObserver));
            M.SelectInspectionObject(VolumeProbe);Next(67);break;
        case 67:
            if(!Readout().Contains(TEXT("-0.2300278575 Pa")))return false;
            Capture(TEXT("three-d-reopened.png"));Add(3);Next(70);break;
        case 70:
            VolumeRuler=M.SelectedInspectionObject;Type(TEXT("InspectionName"),TEXT("3D ruler"));
            Scene->SetCameraRotation(FRotator(-15,-105,10));Scene->FitCamera();Next(71);break;
        case 71:
        {
            Test->TestTrue(TEXT("Newly added 3D ruler is visible before placement"),SelectedRowIsVisible());
            VolumeRulerBefore=*M.FindRuler(VolumeRuler);PlaneAnchor=VolumeRulerBefore.A;
            const auto Observer3D=Scene->PresentedCamera();PlaneNormal=Observer3D.Orientation.GetForwardVector();
            const auto Right=Observer3D.Orientation.GetRightVector(),Up=Observer3D.Orientation.GetUpVector();
            PlacementA=PlaneAnchor-Right*.012+Up*.003;PlacementB=PlaneAnchor+Right*.012+Up*.008;
            Press(TEXT("PlaceInspection"));ClickWorld(PlacementA);
            Test->TestTrue(TEXT("3D first endpoint remains a draft"),*M.FindRuler(VolumeRuler)==VolumeRulerBefore);
            Test->TestTrue(TEXT("3D plane is genuinely oblique"),FMath::Abs(PlaneNormal.X)>.1&&FMath::Abs(PlaneNormal.Z)>.1);
            PlaybackRateBefore=M.PlaybackRate;LoopBefore=M.bLoopPlayback;M.PlaybackRate=.25;M.bLoopPlayback=true;
            ReplayStartFrame=Scene->PresentedFrame().Index;M.Run();Next(72);break;
        }
        case 72:
            if(Scene->PresentedFrame().Index==ReplayStartFrame)return false;
            Test->TestTrue(TEXT("The real replay clock advances while placement remains active"),M.State==EStudioRunState::Running&&PlacementHint().Contains(TEXT("Click B")));
            Test->TestTrue(TEXT("Changing physical 3D frames does not commit the draft"),*M.FindRuler(VolumeRuler)==VolumeRulerBefore);
            HoverWorld(PlacementB);Capture(TEXT("three-d-placement-replay.png"));LookScene(10,5);Next(73);break;
        case 73:
        {
            Test->TestTrue(TEXT("Camera motion keeps the running 3D draft"),M.State==EStudioRunState::Running&&PlacementHint().Contains(TEXT("Click B")));
            HoverWorld(PlacementB);Capture(TEXT("three-d-placement-camera.png"));ClickWorld(PlacementB);
            const auto* R=M.FindRuler(VolumeRuler);
            Test->TestTrue(TEXT("Oblique placement reaches both intended world endpoints"),R->A.Equals(PlacementA,1.e-6)&&R->B.Equals(PlacementB,1.e-6));
            Test->TestTrue(TEXT("Both endpoints remain on the original frozen 3D plane"),FMath::Abs(FVector::DotProduct(R->A-PlaneAnchor,PlaneNormal))<1.e-10&&FMath::Abs(FVector::DotProduct(R->B-PlaneAnchor,PlaneNormal))<1.e-10);
            Test->TestTrue(TEXT("3D ruler measures the independently defined distance"),MeasurementNear(VolumeRuler,FMath::Sqrt(.024*.024+.005*.005)));
            const int32 LiveFrame=M.SelectedFrame;PlacementObserver=Scene->SavedCameraState();
            Test->TestTrue(TEXT("One undo restores both original 3D endpoints"),M.UndoView()&&*M.FindRuler(VolumeRuler)==VolumeRulerBefore);
            Test->TestTrue(TEXT("Object undo preserves the moved camera and replay"),StudioView::CameraEquals(Scene->SavedCameraState(),PlacementObserver)&&M.State==EStudioRunState::Running&&M.SelectedFrame==LiveFrame);
            Test->TestTrue(TEXT("Redo restores the complete 3D placement"),M.RedoView()&&MeasurementNear(VolumeRuler,FMath::Sqrt(.024*.024+.005*.005)));
            Test->TestEqual(TEXT("3D placement and replay leave editable physics untouched"),StudioCaseIO::Serialize(M.Project.Draft),CaseState);
            M.Pause();M.PlaybackRate=PlaybackRateBefore;M.bLoopPlayback=LoopBefore;Next(74);break;
        }
        case 74:
            Capture(TEXT("three-d-placement-committed.png"));Expected=M.InspectionObjects;FieldObserver=Scene->SavedCameraState();
            Test->TestTrue(TEXT("Save the placed 3D ruler"),M.SaveProject(Root/TEXT("three-d-placed.lbms")));
            M.NewProject(TEXT("Before placed 3D reopen"));Test->TestTrue(TEXT("Reopen placed 3D inspection"),M.RequestProjectOpen(Root/TEXT("three-d-placed.lbms")));Next(75);break;
        case 75:
            Test->TestTrue(TEXT("Placed 3D objects and camera reopen exactly"),M.InspectionObjects==Expected&&StudioView::CameraEquals(Scene->SavedCameraState(),FieldObserver));
            M.SelectInspectionObject(VolumeProbe);SelectionFrame=M.SelectedFrame;Next(78);break;
        case 78:
            ClickWorld(M.FindRuler(VolumeRuler)->A);Next(79);break;
        case 79:
            Test->TestTrue(TEXT("Clicking a visible ruler selects its inspector"),M.SelectedInspectionObject==VolumeRuler&&SelectedRowIsVisible());
            Test->TestTrue(TEXT("Scene selection changes neither coordinates nor camera"),M.InspectionObjects==Expected&&StudioView::CameraEquals(Scene->SavedCameraState(),FieldObserver));
            Test->TestEqual(TEXT("Scene selection preserves the recorded frame"),M.SelectedFrame,SelectionFrame);
            Type(TEXT("InspectionName"),TEXT("Scene-selected ruler"),false);ClickWorld(M.FindProbe(VolumeProbe)->A);Next(80);break;
        case 80:
            Test->TestEqual(TEXT("Click away commits a pending name to its original ruler"),M.FindRuler(VolumeRuler)->Name,FString(TEXT("Scene-selected ruler")));
            Test->TestTrue(TEXT("A click that commits values cannot select using stale geometry"),M.SelectedInspectionObject==VolumeRuler);
            M.EndViewEdit();Test->TestTrue(TEXT("The focus-loss edit is one undo"),M.UndoView()&&M.InspectionObjects==Expected);Next(81);break;
        case 81:
            ClickWorld(M.FindProbe(VolumeProbe)->A);Next(82);break;
        case 82:
            if(!Readout().Contains(TEXT("Pa")))return false;
            Test->TestTrue(TEXT("A visible probe marker selects its source-bound inspector"),M.SelectedInspectionObject==VolumeProbe&&SelectedRowIsVisible());
            Test->TestEqual(TEXT("Picking the pressure probe retains the speed viewport"),Scene->PresentedScalar().Id,FString(TEXT("velocity_magnitude")));
            Capture(TEXT("scene-selected-probe.png"));Next(85);break;
        case 85:
        {
            const auto BeforeCamera=Scene->SavedCameraState();const auto BeforeObjects=M.InspectionObjects;
            const uint64 BeforeCapture=Scene->GetCaptureCount();const auto* BeforeTarget=Scene->GetRenderTarget();
            TArray<FColor> LiveBefore,LiveAfter;Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(LiveBefore);
            FStudioSnapshot Native;Native.Options.Size=Scene->PresentedViewportSize();Native.Options.bAnnotations=false;Native.Options.bLegend=false;Native.Options.bFrameInfo=false;
            FString Error;Test->TestTrue(TEXT("Capture native-resolution flow without controls"),Scene->CaptureSnapshot(Native,nullptr,Error));
            Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(LiveAfter);
            Test->TestTrue(TEXT("Export leaves the live texture and all its pixels unchanged"),Scene->GetRenderTarget()==BeforeTarget&&LiveBefore==LiveAfter);
            Test->TestEqual(TEXT("Export does not publish a different live frame"),Scene->GetCaptureCount(),BeforeCapture);
            Test->TestTrue(TEXT("Export leaves camera and inspection objects exact"),StudioView::CameraEquals(Scene->SavedCameraState(),BeforeCamera)&&M.InspectionObjects==BeforeObjects);
            if(Test->TestEqual(TEXT("Native export has the displayed pixel count"),Native.Pixels.Num(),LiveBefore.Num()))
            {
                double ErrorSum=0;for(int32 I=0;I<LiveBefore.Num();++I)ErrorSum+=PixelDifference(Native.Pixels[I],LiveBefore[I]);
                Test->AddInfo(FString::Printf(TEXT("Native snapshot mean RGB error %.9g byte levels"),ErrorSum/FMath::Max(1,3*LiveBefore.Num())));
                Test->TestTrue(TEXT("Native export reproduces the displayed scientific image"),ErrorSum/FMath::Max(1,3*LiveBefore.Num())<2.);
            }
            FStudioSnapshot Square;Square.Options.Size=FIntPoint(1920,1920);Square.Options.bAnnotations=false;Square.Options.bLegend=false;Square.Options.bFrameInfo=false;
            Test->TestTrue(TEXT("Render square crop at actual output resolution"),Scene->CaptureSnapshot(Square,nullptr,Error));
            SnapshotPlainPixels=Square.Pixels;SnapshotFrame=Square.Identity.Frame.Index;SnapshotTime=Square.Identity.Frame.Time;
            TArray64<uint8> PNG;Test->TestTrue(TEXT("Encode bare square reference"),StudioSnapshot::Encode(Square,PNG,Error));
            Test->TestTrue(TEXT("Save bare square reference"),StudioFileDialog::WriteAtomicBytes(Root/TEXT("snapshot-square-bare.png"),PNG,Error));
            OpenSnapshotMenu();Press(TEXT("SnapshotAspect3"));Next(86);break;
        }
        case 86:
            Capture(TEXT("snapshot-options-square.png"));SnapshotPath=Root/TEXT("snapshot-square-annotated.png");
            IFileManager::Get().Delete(*SnapshotPath);StudioFileDialog::SetNextSnapshotPNGForAutomation(SnapshotPath);
            Press(TEXT("SaveSnapshotPNG"));M.Scrub(1.);Next(87);break;
        case 87:
        {
            if(!IFileManager::Get().FileExists(*SnapshotPath))return false;
            TArray64<uint8> PNG;Test->TestTrue(TEXT("Read saved annotated PNG"),FFileHelper::LoadFileToArray(PNG,*SnapshotPath));
            const auto JSON=SnapshotMetadata(PNG);
            if(!Test->TestTrue(TEXT("Saved snapshot carries embedded identity"),JSON.IsValid()))return true;
            Test->TestEqual(TEXT("Export retains click-time frame after scrubbing"),int32(JSON->GetNumberField(TEXT("frame"))),SnapshotFrame);
            Test->TestEqual(TEXT("Export retains click-time physical time"),JSON->GetNumberField(TEXT("time_seconds")),SnapshotTime);
            Test->TestEqual(TEXT("Export retains the actual displayed scalar"),JSON->GetObjectField(TEXT("scalar"))->GetStringField(TEXT("id")),FString(TEXT("velocity_magnitude")));
            Test->TestTrue(TEXT("The independently selected pressure samples accompany the same image"),JSON->GetStringField(TEXT("probe_samples_csv")).Contains(TEXT("pressure"))&&JSON->GetStringField(TEXT("probe_samples_csv")).Contains(FString::FromInt(SnapshotFrame)));
            const auto Overlay=JSON->GetObjectField(TEXT("resolved_overlay"));bool bFrozenProbe=false;
            for(const auto& Value:Overlay->GetArrayField(TEXT("markers")))
            {
                const auto Marker=Value->AsObject();if(Marker->GetStringField(TEXT("object"))!=VolumeProbe.ToString())continue;
                const auto& P=Marker->GetArrayField(TEXT("position"));
                bFrozenProbe=P.Num()==3&&FVector(P[0]->AsNumber(),P[1]->AsNumber(),P[2]->AsNumber()).Equals(M.FindProbe(VolumeProbe)->A,1.e-12);
            }
            Test->TestTrue(TEXT("Metadata stores the actual visible probe position and annotation edges"),bFrozenProbe&&!Overlay->GetArrayField(TEXT("lines")).IsEmpty());
            Test->TestTrue(TEXT("Replay moved beyond the frozen image"),Scene->PresentedFrame().Index!=SnapshotFrame);
            auto& Module=FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));const auto Reader=Module.CreateImageWrapper(EImageFormat::PNG);
            Test->TestTrue(TEXT("Decode annotated image"),Reader->SetCompressed(PNG.GetData(),PNG.Num()));
            Test->TestEqual(TEXT("Requested width is rendered"),Reader->GetWidth(),int64(1920));Test->TestEqual(TEXT("Requested square height is rendered"),Reader->GetHeight(),int64(1920));
            TArray64<uint8> Raw;if(!Test->TestTrue(TEXT("Decode annotated pixels"),Reader->GetRaw(ERGBFormat::RGBA,8,Raw)))return true;
            if(!Test->TestEqual(TEXT("Decoded image contains every output pixel"),Raw.Num(),int64(SnapshotPlainPixels.Num())*4))return true;
            int64 ChangedPixels=0,Near=0;
            for(int32 I=0;I<SnapshotPlainPixels.Num();++I)
            {
                const FColor C(Raw[I*4],Raw[I*4+1],Raw[I*4+2],Raw[I*4+3]);const int32 Delta=PixelDifference(C,SnapshotPlainPixels[I]);
                if(Delta>30)++ChangedPixels;if(Delta<=6)++Near;
            }
            Test->AddInfo(FString::Printf(TEXT("Annotated snapshot: %lld changed / %lld near-original pixels of %d"),ChangedPixels,Near,SnapshotPlainPixels.Num()));
            Test->TestTrue(TEXT("Annotations, legend and frame label add visible pixels"),ChangedPixels>2000);
            Test->TestTrue(TEXT("Annotation pass preserves the underlying scientific image"),Near>SnapshotPlainPixels.Num()*.7);
            SnapshotPlainPixels.Reset();M.Scrub(.5);Next(88);break;
        }
        case 88:
            if(!Readout().Contains(TEXT("Pa")))return false;
            OpenSnapshotMenu();SnapshotPath=Root/TEXT("snapshot-refreshed-camera.png");
            IFileManager::Get().Delete(*SnapshotPath);StudioFileDialog::SetNextSnapshotPNGForAutomation(SnapshotPath);
            SnapshotPreviousCapture=Scene->GetCaptureCount();
            Scene->SetCameraPosition(FieldObserver.Position+FVector(.001,0,0));
            Test->TestEqual(TEXT("Camera edit remains pending before immediate export"),Scene->GetCaptureCount(),SnapshotPreviousCapture);
            Press(TEXT("SaveSnapshotPNG"));Next(89);break;
        case 89:
        {
            if(!IFileManager::Get().FileExists(*SnapshotPath))return false;
            TArray64<uint8> PNG;FFileHelper::LoadFileToArray(PNG,*SnapshotPath);const auto JSON=SnapshotMetadata(PNG);
            if(!Test->TestTrue(TEXT("Immediate camera-edit export includes metadata"),JSON.IsValid()))return true;
            Test->TestTrue(TEXT("Export flushes the pending camera capture"),FCString::Strtoui64(*JSON->GetStringField(TEXT("capture")),nullptr,10)>SnapshotPreviousCapture);
            const FString CSVPrefix=TEXT("1,\"")+JSON->GetStringField(TEXT("project"))+TEXT("\",")+JSON->GetStringField(TEXT("capture"))+TEXT(",");
            Test->TestTrue(TEXT("Reused pressure samples have the image's new capture identity"),JSON->GetStringField(TEXT("probe_samples_csv")).Contains(CSVPrefix));
            const auto& P=JSON->GetObjectField(TEXT("camera"))->GetArrayField(TEXT("position_meters"));
            Test->TestTrue(TEXT("Export metadata includes the immediate camera edit"),FVector(P[0]->AsNumber(),P[1]->AsNumber(),P[2]->AsNumber()).Equals(FieldObserver.Position+FVector(.001,0,0),1.e-10));
            Scene->RestoreCamera(FieldObserver,TEXT("Restore snapshot test camera"));Next(90);break;
        }
        case 90:
            if(!Readout().Contains(TEXT("Pa")))return false;
            OpenSnapshotMenu();StudioFileDialog::SetNextSnapshotPNGForAutomation(FString());Press(TEXT("SaveSnapshotPNG"));
            Test->TestEqual(TEXT("Destination cancellation is explicit"),M.Notice,FString(TEXT("Snapshot export cancelled.")));
            ClickWorld(M.FindSlice(VolumeSlice)->Origin);Next(83);break;
        case 83:
            Test->TestTrue(TEXT("Slice origin selects the named slice"),M.SelectedInspectionObject==VolumeSlice&&SelectedRowIsVisible());
            Test->TestTrue(TEXT("Selected slice inspector shows current sampling state"),Readout().Contains(TEXT("Sampled")));
            ClickWorld((M.FindRuler(VolumeRuler)->A+M.FindRuler(VolumeRuler)->B)*.5);Next(84);break;
        case 84:
            Test->TestTrue(TEXT("Ruler segment selects without requiring an endpoint"),M.SelectedInspectionObject==VolumeRuler&&SelectedRowIsVisible());
            Test->TestTrue(TEXT("Mixed scene selection preserves saved objects and camera"),M.InspectionObjects==Expected&&StudioView::CameraEquals(Scene->SavedCameraState(),FieldObserver));
            Test->TestEqual(TEXT("Mixed scene selection preserves source time"),M.SelectedFrame,SelectionFrame);
            Test->TestEqual(TEXT("Mixed scene selection preserves editable physics"),StudioCaseIO::Serialize(M.Project.Draft),CaseState);
            Capture(TEXT("scene-selected-ruler.png"));Next(76);break;
        case 76:
        {
            Test->TestTrue(TEXT("The selected reopened object is fully visible in its list"),SelectedRowIsVisible());
            Capture(TEXT("three-d-placement-reopened.png"));Press(TEXT("PlaceInspection"));ClickWorld(PlacementA);
            Test->TestTrue(TEXT("Second 3D gesture has one uncommitted point"),PlacementHint().Contains(TEXT("Click B")));
            OldField=Scene->PresentedField();OldSolver=M.Solver;
            Test->TestTrue(TEXT("Replace the source during a partial 3D placement"),M.RequestRecording(FieldSource));Next(77);break;
        }
        case 77:
            Test->TestFalse(TEXT("Source replacement cancels the partial 3D gesture"),PlacementHint().Contains(TEXT("PLACE")));
            Test->TestTrue(TEXT("Cancelled source-change gesture retains all saved coordinates"),M.InspectionObjects==Expected);
            M.SelectInspectionObject(VolumeProbe);Next(68);break;
        case 68:
            if(!Readout().Contains(TEXT("different recording")))return false;
            Test->TestTrue(TEXT("Inactive 3D slice produces no geometry notice"),Scene->PresentedSliceNotice(VolumeSlice).IsEmpty());
            Test->TestFalse(TEXT("3D field releases after returning to 2D"),OldField.IsValid());
            Test->TestFalse(TEXT("3D solver releases after returning to 2D"),OldSolver.IsValid());
            Test->TestTrue(TEXT("Source return retains every object's exact state"),M.InspectionObjects==Expected);
            M.SelectInspectionObject(OriginalProbe);Next(69);break;
        case 69:
            if(!Readout().Contains(TEXT("8.365919113 Pa")))return false;
            Capture(TEXT("probe-original-source-returned.png"));Press(TEXT("CloseInspection"));
            Test->TestTrue(TEXT("Restore preceding project"),M.RequestProjectOpen(Root/TEXT("original.lbms")));Next(24);break;
        }
        return false;
    }
private:
    static int32 PixelDifference(FColor A,FColor B)
    {return FMath::Abs(int32(A.R)-B.R)+FMath::Abs(int32(A.G)-B.G)+FMath::Abs(int32(A.B)-B.B);}
    TSharedPtr<FJsonObject> SnapshotMetadata(const TArray64<uint8>& PNG)
    {
        for(int64 At=8;At+12<=PNG.Num();)
        {
            const auto* P=PNG.GetData()+At;const uint32 Length=(uint32(P[0])<<24)|(uint32(P[1])<<16)|(uint32(P[2])<<8)|P[3];
            if(At+12+Length>PNG.Num())break;
            if(Length>14&&FMemory::Memcmp(P+4,"iTXt",4)==0)
            {
                const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(P+22),Length-14);
                TSharedPtr<FJsonObject> JSON;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(),Text.Get())),JSON);return JSON;
            }
            At+=12+Length;
        }
        return {};
    }
    void OpenSnapshotMenu()
    {if(auto Menu=FindTag(TEXT("SnapshotOptions")))Enter(Find(Menu.ToSharedRef(),NAME_None,true));}
    bool SelectedRowIsVisible()
    {
        const auto Row=FindTag(FName(*(TEXT("Inspection_")+Scene->Model->SelectedInspectionObject.ToString(EGuidFormats::Digits))));
        const auto List=FindTag(TEXT("InspectionObjectList"));if(!Row||!List)return false;
        const auto& RowGeometry=Row->GetCachedGeometry();const auto& ListGeometry=List->GetCachedGeometry();
        const auto RowTop=ListGeometry.AbsoluteToLocal(RowGeometry.LocalToAbsolute(FVector2D::ZeroVector));
        const auto RowBottom=ListGeometry.AbsoluteToLocal(RowGeometry.LocalToAbsolute(RowGeometry.GetLocalSize()));
        Test->AddInfo(FString::Printf(TEXT("Selected row layout: top %.6g bottom %.6g list height %.6g"),RowTop.Y,RowBottom.Y,ListGeometry.GetLocalSize().Y));
        return RowGeometry.GetLocalSize().Y>0&&RowTop.Y>=-1&&RowBottom.Y<=ListGeometry.GetLocalSize().Y+1;
    }
    void RestoreInspectionMaterial()
    {
        if(Scene.IsValid()&&InspectionMaterialProperty&&HeldInspectionMaterial.IsValid())
            InspectionMaterialProperty->SetObjectPropertyValue_InContainer(Scene.Get(),HeldInspectionMaterial.Get());
        HeldInspectionMaterial.Reset();
    }
    bool MeasurementNear(const FGuid& Id,double ExpectedValue,double Tolerance=1.e-6)
    {const auto* R=Scene->Model->FindRuler(Id);if(!R)return false;const auto Value=StudioInspectionObjects::Measurement(*R);return Value.IsSet()&&FMath::IsNearlyEqual(Value.GetValue(),ExpectedValue,Tolerance);}
    void Next(int32 N){Test->AddInfo(FString::Printf(TEXT("Inspection phase %d at tick %llu"),N,GFrameCounter));Phase=N;Changed=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag,bool Button=false)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if((!Button&&W->GetTag()==Tag)||(Button&&W->GetType()==TEXT("SButton")))return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Match=Find(Children->GetChildAt(I),Tag,Button))return Match;return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Match=Find(Window,Tag))return Match;
        Test->AddError(TEXT("Missing inspection control: ")+Tag.ToString());return {};
    }
    void Enter(const TSharedPtr<SWidget>& Widget)
    {
        if(!Widget)return;auto& App=FSlateApplication::Get();Test->TestTrue(TEXT("Inspection control enabled"),Widget->IsEnabled());
        if(!Widget->HasKeyboardFocus()&&!Widget->HasFocusedDescendants())App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag){Enter(FindTag(Tag));}
    void Key(FKey Key)
    {auto& App=FSlateApplication::Get();App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));}
    FString Readout()
    {
        const auto Box=FindTag(TEXT("InspectionReadout"));if(!Box)return FString();
        const auto Child=Box->GetChildren()->GetChildAt(0);
        return Child->GetType()==TEXT("STextBlock")?StaticCastSharedRef<STextBlock>(Child)->GetText().ToString():FString();
    }
    FString PlacementHint()
    {
        const auto Box=FindTag(TEXT("InspectionPlacementHint"));if(!Box)return FString();
        const auto Child=Box->GetChildren()->GetChildAt(0);
        return Child->GetType()==TEXT("STextBlock")?StaticCastSharedRef<STextBlock>(Child)->GetText().ToString():FString();
    }
    void Add(int32 Kind)
    {if(auto Menu=FindTag(TEXT("AddInspection")))Enter(Find(Menu.ToSharedRef(),NAME_None,true));Press(FName(*FString::Printf(TEXT("AddInspection%d"),Kind)));}
    void OpenFieldMenu()
    {if(auto Menu=FindTag(TEXT("InspectionField")))Enter(Find(Menu.ToSharedRef(),NAME_None,true));}
    void Type(FName Tag,const FString& Value,bool Commit=true)
    {
        const auto W=FindTag(Tag);if(!W)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        const FModifierKeysState SelectAll(false,false,true,false,false,false,false,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,SelectAll,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,SelectAll,0,false,0,0));
        for(TCHAR Ch:Value)App.ProcessKeyCharEvent(FCharacterEvent(Ch,FModifierKeysState(),0,false));if(Commit)Enter(W);
    }
    void ClickScene(double X,double Y)
    {
        const auto W=FindTag(TEXT("FlowViewport"));if(!W)return;
        const auto G=W->GetCachedGeometry();const auto P=G.LocalToAbsolute(G.GetLocalSize()*FVector2D(X,Y));
        auto& App=FSlateApplication::Get();
        const auto Hit=WindowHit(P);
        if(!Hit.ContainsWidget(W.Get()))LogMissedHit(P,Hit);
        if(!Test->TestTrue(TEXT("Placement click reaches exposed flow area"),Hit.ContainsWidget(W.Get())))return;
        App.RoutePointerDownEvent(Hit,FPointerEvent(0,P,P,TSet<FKey>{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
        App.RoutePointerUpEvent(Hit,FPointerEvent(0,P,P,TSet<FKey>(),EKeys::LeftMouseButton,0,FModifierKeysState()));
    }
    void PointerWorld(const FVector& World,bool Click)
    {
        const auto W=FindTag(TEXT("FlowViewport"));if(!W)return;
        const auto G=W->GetCachedGeometry();const auto Size=Scene->PresentedViewportSize();FVector2D Pixel;
        if(!Test->TestTrue(TEXT("Known world endpoint projects into captured view"),StudioCameraPlacement::Project(Scene->PresentedCamera(),G.GetLocalSize(),World,Pixel,double(Size.X)/Size.Y)))return;
        const auto P=G.LocalToAbsolute(Pixel);auto& App=FSlateApplication::Get();
        if(Click)Test->AddInfo(FString::Printf(TEXT("Placement pixel round trip error %.17g px at %s"),(FVector2D(G.AbsoluteToLocal(P))-Pixel).Size(),*Pixel.ToString()));
        const auto Hit=WindowHit(P);
        if(!Hit.ContainsWidget(W.Get()))LogMissedHit(P,Hit);
        if(!Test->TestTrue(TEXT("Known world endpoint is exposed to actual Slate input"),Hit.ContainsWidget(W.Get())))return;
        App.RoutePointerMoveEvent(Hit,FPointerEvent(0,P,P,TSet<FKey>(),FKey(),0,FModifierKeysState()),false);
        if(!Click)return;
        App.RoutePointerDownEvent(Hit,FPointerEvent(0,P,P,TSet<FKey>{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
        App.RoutePointerUpEvent(Hit,FPointerEvent(0,P,P,TSet<FKey>(),EKeys::LeftMouseButton,0,FModifierKeysState()));
    }
    void HoverWorld(const FVector& World){PointerWorld(World,false);}
    FWidgetPath WindowHit(const FVector2D& Pixel)
    {
        auto& App=FSlateApplication::Get();
        // LocateWindowUnderMouse first asks the OS for the real cursor's
        // window, which may differ from this simulated event's position.
        const auto Window=GEngine->GameViewport->GetWindow().ToSharedRef();
        if(!Window->IsVisible()||Window->IsWindowMinimized()||!Window->AcceptsInput()||
            !App.GetInteractiveTopLevelWindows().Contains(Window)||!Window->IsScreenspaceMouseWithin(Pixel))return FWidgetPath();
        // Same public hit grid used by Slate's protected LocateWidgetInWindow;
        // zero radius is the point mouse event used by this windowed test.
        auto Widgets=Window->GetHittestGrid().GetBubblePath(Pixel,0,false,0);
        const FWidgetPath Hit(Widgets);
        if(!App.LocateWindowUnderMouse(Pixel,App.GetInteractiveTopLevelWindows(),false,0).IsValid()&&Hit.IsValid())
            Test->AddInfo(TEXT("Simulated pointer resolves in test window; OS-cursor window lookup returned no hit."));
        return Hit;
    }
    void LogMissedHit(const FVector2D& Pixel,const FWidgetPath& Hit)
    {
        FString Path;for(int32 I=0;I<Hit.Widgets.Num();++I)
        {const auto& Entry=Hit.Widgets[I];Path+=Entry.Widget->GetTypeAsString()+TEXT("[")+Entry.Widget->GetTag().ToString()+TEXT("] / ");}
        Test->AddInfo(FString::Printf(TEXT("Missed flow hit at %s: %s"),*Pixel.ToString(),*Path));
        const auto Window=GEngine->GameViewport->GetWindow().ToSharedRef();
        auto& Grid=Window->GetHittestGrid();
        const auto Raw=Grid.GetBubblePath(Pixel,0,false,0);
        Test->AddInfo(FString::Printf(TEXT("Window hit diagnostics phase %d: visible=%d minimized=%d accepts=%d interactive=%d contains=%d position=%s size=%s gridOrigin=%s gridWindowOrigin=%s gridSize=%s rawPath=%d"),
            Phase,Window->IsVisible(),Window->IsWindowMinimized(),Window->AcceptsInput(),
            FSlateApplication::Get().GetInteractiveTopLevelWindows().Contains(Window),Window->IsScreenspaceMouseWithin(Pixel),
            *FVector2D(Window->GetPositionInScreen()).ToString(),*FVector2D(Window->GetSizeInScreen()).ToString(),
            *FVector2D(Grid.GetGridOrigin()).ToString(),*FVector2D(Grid.GetGridWindowOrigin()).ToString(),*FVector2D(Grid.GetGridSize()).ToString(),Raw.Num()));
        if(const auto Flow=FindTag(TEXT("FlowViewport")))
            Test->AddInfo(FString::Printf(TEXT("Flow hit geometry: tickPosition=%s paintPosition=%s localSize=%s"),
                *FVector2D(Flow->GetCachedGeometry().LocalToAbsolute(FVector2D::ZeroVector)).ToString(),
                *FVector2D(Flow->GetPaintSpaceGeometry().LocalToAbsolute(FVector2D::ZeroVector)).ToString(),
                *FVector2D(Flow->GetCachedGeometry().GetLocalSize()).ToString()));
    }
    void ClickWorld(const FVector& World){PointerWorld(World,true);}
    void LookScene(double DX,double DY)
    {
        const auto W=FindTag(TEXT("FlowViewport"));if(!W)return;const auto G=W->GetCachedGeometry();
        const auto P=G.LocalToAbsolute(G.GetLocalSize()*FVector2D(.4,.45)),Q=P+FVector2D(DX,DY);
        auto& App=FSlateApplication::Get();const auto Hit=WindowHit(P);
        if(!Test->TestTrue(TEXT("Right-drag reaches exposed flow area during placement"),Hit.ContainsWidget(W.Get())))return;
        const auto Before=Scene->SavedCameraState();const TSet<FKey> Down{EKeys::RightMouseButton};
        App.RoutePointerDownEvent(Hit,FPointerEvent(0,P,P,Down,EKeys::RightMouseButton,0,FModifierKeysState()));
        Test->TestTrue(TEXT("Viewport captures right-drag during a partial placement"),W->HasMouseCapture());
        App.RoutePointerMoveEvent(WindowHit(Q),FPointerEvent(0,Q,P,Down,FKey(),0,FModifierKeysState()),false);
        App.RoutePointerUpEvent(WindowHit(Q),FPointerEvent(0,Q,Q,TSet<FKey>(),EKeys::RightMouseButton,0,FModifierKeysState()));
        Test->TestFalse(TEXT("Routed right-drag changes viewing camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Before));
        Test->TestFalse(TEXT("Right-drag releases capture before next placement point"),W->HasMouseCapture());
    }
    void Escape(){auto& App=FSlateApplication::Get();App.ProcessKeyDownEvent(FKeyEvent(EKeys::Escape,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Escape,FModifierKeysState(),0,false,0,0));}
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;Test->TestTrue(TEXT("Capture inspection window"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        if(Pixels.IsEmpty())return;TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save inspection evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;double Started=0;int32 Phase=0,Frame=0;uint64 Changed=0;
    FString Root,Source,CSVPath;FGuid Slice,Probe,Ruler,Line,OriginalProbe,Angle;FStudioProject Original;FStudioInspectionObjects Expected;FStudioCameraState Observer,PlacementObserver;FStudioProbeObject ExpectedLine;
    FStudioRulerObject RulerBefore,AngleBefore;FStudioCameraState FieldObserver;FString FieldSource;int32 FieldFrame=0;
    FString PointCopy,RetryArray,CaseState;FGuid VolumeSlice,VolumeProbe;FStudioInspectionObjects BeforeSourceObjects;
    FObjectPropertyBase* InspectionMaterialProperty=nullptr;TStrongObjectPtr<UObject> HeldInspectionMaterial;
    FStudioInspectionObjects MaterialObjects;FStudioCameraState MaterialCamera;int32 MaterialField=0;
    FGuid VolumeRuler;FStudioRulerObject VolumeRulerBefore;FVector PlaneAnchor,PlaneNormal,PlacementA,PlacementB;
    double PlaybackRateBefore=1;bool LoopBefore=false;int32 ReplayStartFrame=0;
    int32 SelectionFrame=0;
    FString SnapshotPath;TArray<FColor> SnapshotPlainPixels;int32 SnapshotFrame=0;double SnapshotTime=0;uint64 SnapshotPreviousCapture=0;
    TWeakPtr<const IStudioField,ESPMode::ThreadSafe> OldField;TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> OldSolver;
    EStudioWorkspace OriginalWorkspace=EStudioWorkspace::Solve;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionUI,"Studio.InspectionUI.ControlsSamplesAndPersistence",EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioInspectionUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioInspectionUICommand(this));return true;}
#endif
