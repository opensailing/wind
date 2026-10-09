#include "SStudioHome4Authoring.h"
#include "SStudioHome4Regions.h"
#include "SStudioHome4Settings.h"
#include "SStudioHome4Stiffness.h"
#include "SStudioHome4Panel.h"
#include "StudioHeadlessSlate.h"
#include "StudioModel.h"
#include "StudioHome4Authoring.h"
#include "StudioHome4Setup.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "InputCoreTypes.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4AuthoringUITestsLocal
{
TSharedPtr<FStudioHome4Session> Setup(const TSharedPtr<FStudioModel>& M)
{
    auto Draft=MakeShared<FStudioHome4Session>(M);Draft->ApplyRecipe(TEXT("th01-hull"));Draft->Set(TEXT("authoring.primitive"),TEXT("box"));Draft->Set(TEXT("authoring.primitiveSizeCells"),TEXT("4.5,4.5,4.5"));Draft->Set(TEXT("geometry.initialPositionCells"),TEXT("8,8,8"));Draft->Set(TEXT("lattice.extents"),TEXT("16,16,16"));return Draft;
}
struct FPreviewLatent final:IAutomationLatentCommand
{
    FAutomationTestBase* Test;TSharedPtr<FStudioModel> Model;TSharedPtr<FStudioHome4Session> Draft;TSharedPtr<FStudioHome4AuthoringSession> Authoring;
    double Deadline=FPlatformTime::Seconds()+15;
    explicit FPreviewLatent(FAutomationTestBase* T):Test(T)
    {
        Model=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-authoring-ui")/FGuid::NewGuid().ToString());Draft=Setup(Model);Authoring=MakeShared<FStudioHome4AuthoringSession>(Draft);
        auto Widget=SNew(SStudioHome4Authoring).Session(Authoring).Page(TEXT("Geometry"));FStudioHeadlessSlate UI(*Test,Widget,FVector2D(690,620));UI.Press(TEXT("Home4Prepare"));
    }
    bool Update()override
    {
        Authoring->Poll();if(Authoring->IsPreparing()&&FPlatformTime::Seconds()<Deadline)return false;
        Test->TestFalse(TEXT("Preparation finishes within a bounded UI deadline"),Authoring->IsPreparing());const auto P=Authoring->Preview();Test->TestTrue(*Authoring->Status,P&&P->IsValid());if(!P)return true;
        auto Widget=SNew(SStudioHome4Authoring).Session(Authoring).Page(TEXT("Geometry"));FStudioHeadlessSlate UI(*Test,Widget,FVector2D(690,640));
        UI.Inspect(TEXT("home4-geometric-preview"),{TEXT("Home4Prepare"),TEXT("Home4PreparedGeometry"),TEXT("Home4Preview.iso"),TEXT("Home4Preview.voxels"),TEXT("Home4Preview.cut-links"),TEXT("Home4PreparedDetails")});
        Test->TestTrue(TEXT("Computed sampled SDF zero surface is separate from CAD triangles"),!P->SdfSurfaceIndices.IsEmpty()&&P->SdfSurfacePositions.Num()!=P->Mesh->Positions.Num());
        UI.Press(TEXT("Home4Preview.iso"));UI.Focus(TEXT("Home4PreparedGeometry"));UI.Key(EKeys::One);
        Test->TestTrue(TEXT("XY preset changes the actual retained camera"),FMath::IsNearlyEqual(Authoring->Camera().Pitch,PI/2.,1e-12));
        UI.Key(EKeys::Two);Test->TestTrue(TEXT("XZ preset changes actual yaw"),FMath::IsNearlyEqual(Authoring->Camera().Yaw,-PI/2.,1e-12));
        UI.Key(EKeys::Three);Test->TestTrue(TEXT("YZ preset changes actual yaw/pitch"),Authoring->Camera().Yaw==0&&Authoring->Camera().Pitch==0);
        Authoring->Camera().Pan=FVector2D(17,23);Authoring->Camera().Zoom=2.;
        {auto Body=SNew(SStudioHome4Authoring).Session(Authoring).Page(TEXT("Bodies"));FStudioHeadlessSlate Other(*Test,Body,FVector2D(690,640));Test->TestTrue(TEXT("Camera survives authoring subpage construction"),Authoring->Camera().Pan==FVector2D(17,23)&&Authoring->Camera().Zoom==2.);}
        UI.Focus(TEXT("Home4PreparedGeometry"));UI.Key(EKeys::Home);Test->TestTrue(TEXT("Home resets actual pan and zoom"),Authoring->Camera().Pan.IsNearlyZero()&&Authoring->Camera().Zoom==1.);
        Test->TestTrue(TEXT("Actual orbit viewer has native keyboard focus/presets"),UI.Find(TEXT("Home4PreparedGeometry"))->SupportsKeyboardFocus());
        UI.Press(TEXT("Home4PreviewNextLink"));Test->TestTrue(TEXT("Selected exact cut fraction is visible"),UI.Text(TEXT("Home4PreparedDetails")).Contains(TEXT("Selected cut link")));
        const auto Original=StudioHome4Config::Serialize(Model->Project.Draft.Home4.GetValue());Draft->Set(TEXT("geometry.sinkCells"),TEXT("1"));Authoring->Poll();Test->TestFalse(TEXT("Changed draft cannot display stale geometric response"),Authoring->Preview().IsValid());Test->TestEqual(TEXT("Preview never silently applies the next-run draft"),StudioHome4Config::Serialize(Model->Project.Draft.Home4.GetValue()),Original);
        return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PreviewUI,"Studio.HeadlessUI.Home4.Authoring.ComputedPreviewCameraAndStaleScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4PreviewUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(StudioHome4AuthoringUITestsLocal::FPreviewLatent(this));return true;}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RegionsUI,"Studio.HeadlessUI.Home4.Authoring.RegionRetentionAndPatchPersistence",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4RegionsUI::RunTest(const FString&)
{
    using namespace StudioHome4AuthoringUITestsLocal;auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-regions-ui")/FGuid::NewGuid().ToString());auto Draft=Setup(M);
    {auto Widget=SNew(SStudioHome4Regions).Session(Draft).Patches(false);FStudioHeadlessSlate UI(*this,Widget,FVector2D(690,620));UI.Press(TEXT("Home4Region.addZone"));UI.Type(TEXT("Home4Region.minimum"),TEXT("10,0,0"));UI.Type(TEXT("Home4Region.strength"),TEXT("nan"));UI.Press(TEXT("Home4Region.retain"));TestTrue(TEXT("Invalid region prevents a global Apply instead of disappearing"),Draft->HasPending()&&!Draft->Apply());}
    {auto Widget=SNew(SStudioHome4Regions).Session(Draft).Patches(false);FStudioHeadlessSlate UI(*this,Widget,FVector2D(690,620));TestEqual(TEXT("Invalid region survives subpage reconstruction"),UI.Text(TEXT("Home4Region.strength")),FString(TEXT("nan")));UI.Type(TEXT("Home4Region.strength"),TEXT("0.6"));UI.Press(TEXT("Home4Region.retain"));TestFalse(TEXT("Validated region clears its pending-text guard"),Draft->HasPending());}
    {auto Widget=SNew(SStudioHome4Regions).Session(Draft).Patches(true);FStudioHeadlessSlate UI(*this,Widget,FVector2D(690,620));UI.Press(TEXT("Home4Region.addPatch"));UI.Type(TEXT("Home4Region.level"),TEXT("2"));UI.Type(TEXT("Home4Region.origin"),TEXT("6,6,6"));UI.Type(TEXT("Home4Region.extents"),TEXT("16,16,16"));UI.Press(TEXT("Home4Region.retain"));}
    TestTrue(TEXT("Parent Apply saves retained regions together"),Draft->Apply());FStudioProject Saved;FString E;TestTrue(*E,StudioProjectIO::Parse(StudioProjectIO::Serialize(M->SnapshotProject()),Saved,E));TestEqual(TEXT("Zone strength persists"),Saved.Draft.Home4->Authoring.Zones[0].Strength,.6);TestTrue(TEXT("Explicit root origin/local extents persist"),Saved.Draft.Home4->Authoring.Patches[0].Origin==FVector(6)&&Saved.Draft.Home4->Authoring.Patches[0].Level==2);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4UnitsUI,"Studio.HeadlessUI.Home4.Authoring.ConvertedInputsAndPendingNavigation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4UnitsUI::RunTest(const FString&)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-converted-ui")/FGuid::NewGuid().ToString());auto Draft=StudioHome4AuthoringUITestsLocal::Setup(M);Draft->Set(TEXT("units.dxMeters"),TEXT("0.01"));Draft->Set(TEXT("units.dtSeconds"),TEXT("0.001"));Draft->Set(TEXT("units.densityReferenceKgM3"),TEXT("1000"));Draft->Set(TEXT("reference.speedCellsPerStep"),TEXT("0.02"));Draft->Apply();M->UnitDisplay=EStudioHome4UnitDisplay::Physical;
    {auto Panel=SNew(SStudioHome4Panel).Model(M).Session(Draft).Page(TEXT("Fluids & Interface"));FStudioHeadlessSlate UI(*this,Panel,FVector2D(1040,740));TestEqual(TEXT("SI speed editor displays actual converted value"),UI.Text(TEXT("reference.speedCellsPerStep")),FString(TEXT("0.2")));UI.Type(TEXT("reference.speedCellsPerStep"),TEXT("bad"));UI.Press(TEXT("Home4Apply"));TestTrue(TEXT("Invalid converted input remains retained and unapplied"),Draft->HasPending()&&M->Project.Draft.Home4->Reference.SpeedCellsPerStep==.02);}
    {auto Panel=SNew(SStudioHome4Panel).Model(M).Session(Draft).Page(TEXT("Fluids & Interface"));FStudioHeadlessSlate UI(*this,Panel,FVector2D(1040,740));TestEqual(TEXT("Invalid converted input survives page navigation"),UI.Text(TEXT("reference.speedCellsPerStep")),FString(TEXT("bad")));UI.Type(TEXT("reference.speedCellsPerStep"),TEXT("0.4"));UI.Press(TEXT("Home4Apply"));TestTrue(TEXT("SI editing inverse-converts exactly once"),FMath::IsNearlyEqual(M->Project.Draft.Home4->Reference.SpeedCellsPerStep.Get(0),.04,1e-12));}
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SummaryUnitsUI,"Studio.HeadlessUI.Home4.Authoring.FeasibilityLevelMapAndWakeDisplayUnits",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SummaryUnitsUI::RunTest(const FString&)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-summary-units")/FGuid::NewGuid().ToString());auto Draft=StudioHome4AuthoringUITestsLocal::Setup(M);
    for(const auto& Value:{TPair<const TCHAR*,const TCHAR*>(TEXT("units.dxMeters"),TEXT("0.01")),{TEXT("units.dtSeconds"),TEXT("0.001")},{TEXT("units.densityReferenceKgM3"),TEXT("1000")},{TEXT("reference.speedCellsPerStep"),TEXT("0.02")},{TEXT("fluids.nuHeavy"),TEXT("0.01")},{TEXT("fluids.nuLight"),TEXT("0.005")},{TEXT("fluids.sigma"),TEXT("0.02")},{TEXT("fluids.mobility"),TEXT("0.03")},{TEXT("fluids.gravity"),TEXT("0.001")},{TEXT("multidomain.levels"),TEXT("2")}})Draft->Set(Value.Key,Value.Value);
    M->UnitDisplay=EStudioHome4UnitDisplay::Physical;
    auto Panel=SNew(SStudioHome4Panel).Model(M).Session(Draft).Page(TEXT("Lattice"));FStudioHeadlessSlate UI(*this,Panel,FVector2D(1040,800));
    const auto Physical=UI.Text(TEXT("Home4FeasibilitySummary"));TestTrue(TEXT("Wake and duplicate MD summary use the active physical form"),Physical.Contains(TEXT("Wake wavelength"))&&Physical.Contains(TEXT(" m\n"))&&Physical.Contains(TEXT("0.001 m²/s"))&&Physical.Contains(TEXT("10 m/s²")));
    M->UnitDisplay=EStudioHome4UnitDisplay::Nondimensional;Panel->Tick(FGeometry(),0,0);UI.Layout();const auto ND=UI.Text(TEXT("Home4FeasibilitySummary"));TestFalse(TEXT("Nondimensional view never leaves a raw dimensional MD label"),ND.Contains(TEXT("m²/s"))||ND.Contains(TEXT(" cells\n")));TestTrue(TEXT("Per-level display changes while request remains retained"),ND!=Physical&&Draft->Get(TEXT("fluids.nuHeavy"))==TEXT("0.01"));return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SettingsUI,"Studio.HeadlessUI.Home4.Authoring.ViewerDefaultsAndMixedStiffness",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SettingsUI::RunTest(const FString&)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-settings-ui")/FGuid::NewGuid().ToString());auto Draft=StudioHome4AuthoringUITestsLocal::Setup(M);Draft->Set(TEXT("units.dxMeters"),TEXT("0.01"));Draft->Set(TEXT("units.dtSeconds"),TEXT("0.001"));Draft->Set(TEXT("units.densityReferenceKgM3"),TEXT("1000"));Draft->Apply();
    {auto Settings=SNew(SStudioHome4Settings).Model(M);FStudioHeadlessSlate UI(*this,Settings,FVector2D(690,640));UI.Press(TEXT("Home4Settings.Unit1"));UI.Press(TEXT("Home4Settings.saveDefaults"));TestTrue(TEXT("Current viewer defaults retained"),M->Project.bHasViewerDefaults);M->EditView(TEXT("Change opacity for test"),[](auto& S){S.Display.VolumeOpacity=.75;});UI.Press(TEXT("Home4Settings.restoreDefaults"));TestTrue(TEXT("Restore defaults changes real display state"),FMath::IsNearlyEqual(M->VolumeOpacity,.28,1e-12));}
    {auto Matrix=SNew(SStudioHome4Stiffness).Session(Draft);FStudioHeadlessSlate UI(*this,Matrix,FVector2D(690,600));UI.Type(TEXT("Home4Stiffness.0"),TEXT("1000"));UI.Type(TEXT("Home4Stiffness.3"),TEXT("10"));UI.Type(TEXT("Home4Stiffness.21"),TEXT("0.1"));UI.Press(TEXT("Home4Stiffness.retain"));FStudioHome4Spec S;FString E;TestTrue(*E,Draft->Build(S,E));TestTrue(TEXT("Mixed K component dimensions inverse-convert independently"),S.Geometry.Stiffness.Num()==36&&FMath::IsNearlyEqual(S.Geometry.Stiffness[0],1.,1e-12)&&FMath::IsNearlyEqual(S.Geometry.Stiffness[3],1.,1e-12)&&FMath::IsNearlyEqual(S.Geometry.Stiffness[21],1.,1e-12));}
    FStudioProject P;FString E;TestTrue(TEXT("Viewer defaults persist with the project"),StudioProjectIO::Parse(StudioProjectIO::Serialize(M->SnapshotProject()),P,E)&&P.bHasViewerDefaults);
    return !HasAnyErrors();
}
namespace StudioHome4AuthoringCompletionTestsLocal
{
struct FActualBody final:IAutomationLatentCommand
{
    FAutomationTestBase* Test;TSharedPtr<FStudioModel> Model;TSharedPtr<FStudioHome4Session> Draft;TSharedPtr<FStudioHome4AuthoringSession> Authoring;double Deadline=FPlatformTime::Seconds()+15;
    explicit FActualBody(FAutomationTestBase* T):Test(T)
    {Model=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-mass-native")/FGuid::NewGuid().ToString());Draft=StudioHome4AuthoringUITestsLocal::Setup(Model);Authoring=MakeShared<FStudioHome4AuthoringSession>(Draft);Authoring->Request();}
    bool Update()override
    {
        Authoring->Poll();if(Authoring->IsPreparing()&&FPlatformTime::Seconds()<Deadline)return false;const auto P=Authoring->Preview();Test->TestTrue(*Authoring->Status,P&&P->Mesh);if(!P||!P->Mesh)return true;
        auto Widget=SNew(SStudioHome4Authoring).Session(Authoring).Page(TEXT("Bodies"));FStudioHeadlessSlate UI(*Test,Widget,FVector2D(690,800));UI.Type(TEXT("Home4MassDensity"),TEXT("2"));UI.Press(TEXT("Home4AdoptMass"));FStudioHome4Spec S;FString E;Test->TestTrue(*E,Draft->Build(S,E));Test->TestTrue(TEXT("Native mass action changes actual retained mass and provenance"),S.Geometry.BodyMass&&FMath::IsNearlyEqual(*S.Geometry.BodyMass,2*4.5*4.5*4.5,1e-9)&&S.Geometry.InertiaFrame==TEXT("source-xyz")&&S.Geometry.MassPropertySource.Contains(P->RequestSHA256));Test->TestTrue(TEXT("Native action remains a draft until Apply"),Draft->IsDirty());
        UI.Press(TEXT("Home4Preview.motion"));UI.Press(TEXT("Home4PreviewPlay"));Widget->Tick(FGeometry(),0,.5f);Test->TestTrue(TEXT("Actual geometric play changes time instead of a static label"),UI.Text(TEXT("Home4PreviewPlayLabel")).Contains(TEXT("step 15")));return true;
    }
};
struct FActualWave final:IAutomationLatentCommand
{
    FAutomationTestBase* Test;TSharedPtr<FStudioModel> Model;TSharedPtr<FStudioHome4Session> Draft;TSharedPtr<FStudioHome4AuthoringSession> Authoring;double Deadline=FPlatformTime::Seconds()+15;
    explicit FActualWave(FAutomationTestBase* T):Test(T)
    {Model=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-wave-native")/FGuid::NewGuid().ToString());Draft=MakeShared<FStudioHome4Session>(Model);Draft->ApplyRecipe(TEXT("breaking-wave-banari"));Draft->Set(TEXT("lattice.extents"),TEXT("80,24,24"));Draft->Set(TEXT("authoring.waveModel"),TEXT("linear-gravity"));Draft->Set(TEXT("authoring.waveAxis"),TEXT("x"));Draft->Set(TEXT("authoring.waveLengthCells"),TEXT("40"));Draft->Set(TEXT("authoring.waveDepthCells"),TEXT("12"));Draft->Set(TEXT("authoring.waveAmplitudeCells"),TEXT("1"));Draft->Set(TEXT("authoring.wavePeriodSteps"),TEXT(""));Draft->Set(TEXT("reference.wavePhaseSpeed"),TEXT(""));Draft->Set(TEXT("reference.waveSlope"),TEXT(""));Draft->Set(TEXT("fluids.gravity"),TEXT("0.001"));Draft->Set(TEXT("authoring.waterlineCells"),TEXT("12"));Draft->Set(TEXT("authoring.pierceMode"),TEXT("phase-pinned"));Authoring=MakeShared<FStudioHome4AuthoringSession>(Draft);Authoring->Request();}
    bool Update()override
    {Authoring->Poll();if(Authoring->IsPreparing()&&FPlatformTime::Seconds()<Deadline)return false;const auto P=Authoring->Preview();Test->TestTrue(*Authoring->Status,P&&P->IsValid()&&!P->Mesh);if(!P)return true;
        TArray<FVector> Curve;TArray<FStudioHome4BoundaryFace> Faces;FString E;Test->TestTrue(*E,StudioHome4Setup::WaveCurve(P->Spec,0,128,Curve,E));Test->TestTrue(*E,StudioHome4Setup::BoundaryFaces(P->Spec,Faces,E));Test->TestTrue(TEXT("Real computed wave and declared pinned side faces drive meshless render"),Curve.Num()==129&&Curve[0].Z==13&&Faces.Num()==6&&Faces[2].PhaseConstraint.Contains(TEXT("phase-pinned")));
        auto Widget=SNew(SStudioHome4Authoring).Session(Authoring).Page(TEXT("Boundaries & Zones"));FStudioHeadlessSlate UI(*Test,Widget,FVector2D(690,780));UI.Press(TEXT("Home4Preview.tank"));UI.Focus(TEXT("Home4PreparedGeometry"));UI.Key(EKeys::Two);UI.Layout();Test->TestTrue(TEXT("Shared orbit camera renders actual tank-only prepared request"),Authoring->Camera().Yaw==-PI/2.);Test->TestTrue(TEXT("No fabricated body mesh is described"),UI.Text(TEXT("Home4PreparedDetails")).Contains(TEXT("Tank-only")));return true;}
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ActualBodyUI,"Studio.HeadlessUI.Home4.Authoring.ActualMassAdoptionAndGeometricPlay",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ActualBodyUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(StudioHome4AuthoringCompletionTestsLocal::FActualBody(this));return true;}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ActualWaveUI,"Studio.HeadlessUI.Home4.Authoring.MeshlessWaveAndPhaseFaceRendering",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ActualWaveUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(StudioHome4AuthoringCompletionTestsLocal::FActualWave(this));return true;}

#endif
