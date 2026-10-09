#include "StudioHome4Recipes.h"
#include "StudioHome4Session.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RecipeCatalog,"Studio.Home4.Recipes.CatalogAndDepartures",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4RecipeCatalog::RunTest(const FString&)
{
    TestEqual(TEXT("All feedback recipe cards"),StudioHome4Recipes::All().Num(),10);TSet<FString> Ids;
    for(const auto& R:StudioHome4Recipes::All())
    {
        TestFalse(TEXT("Unique recipe identity"),Ids.Contains(R.Id));Ids.Add(R.Id);
        FString E;const bool Valid=StudioHome4Config::Validate(R.Template,E);TestTrue(*(R.Id+TEXT(": ")+E),Valid);
        TestTrue(TEXT("Provenance and gate description"),!R.Anchor.IsEmpty()&&!R.Gate.IsEmpty()&&!R.Driver.IsEmpty());
        TestTrue(TEXT("Unmodified template"),StudioHome4Recipes::Departures(R.Template).IsEmpty());
    }
    auto S=StudioHome4Recipes::Find(TEXT("hydrofoil-parkin"))->Template;S.Reference.Froude=1.2;
    TestTrue(TEXT("Changed recipe value requires new validation"),!StudioHome4Recipes::Departures(S).IsEmpty());
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RecipeLadder,"Studio.Home4.Recipes.RefinementAndReferenceGates",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4RecipeLadder::RunTest(const FString&)
{
    FStudioHome4Spec S;S.Reference.LengthCells=100;S.Reference.SpeedCellsPerStep=.02;S.Fluids.Xi=4;S.Fluids.NuHeavy=.01;S.Fluids.Mobility=.03;S.Fluids.Gravity=.0001;S.Lattice.Extents=FIntVector(100,50,25);S.Run.Steps=1000;
    S.Geometry.HeaveAmplitudeCells=2;S.Geometry.MotionFrequencyCyclesPerStep=.004;S.Geometry.SpinRadiansPerStep=.008;
    S.Geometry.InitialPositionCells=FVector(1,2,3);S.Geometry.InitialAngularVelocityRadiansPerStep=FVector(.004,.008,.012);
    S.Geometry.InitialAttitudeDegrees=FVector(4,5,6);S.Geometry.InitialVelocityCellsPerStep=FVector(.01,.02,.03);
    S.Lattice.StreamwiseCells=100;
    TArray<FStudioHome4LadderRung> R;FString E;TestTrue(TEXT("Generate ordered refinement"),StudioHome4Recipes::Ladder(S,{1,2,4},R,E));
    if(R.Num()!=3)return false;
    TestEqual(TEXT("Fixed Cn scales xi"),R[2].Spec.Fluids.Xi.GetValue(),16.);TestEqual(TEXT("Viscosity preserves Re"),R[2].Spec.Fluids.NuHeavy.GetValue(),.04);
    TestEqual(TEXT("Gravity preserves Fr"),R[2].Spec.Fluids.Gravity.GetValue(),.000025);
    TestEqual(TEXT("Heave amplitude scales with root cells"),R[2].Spec.Geometry.HeaveAmplitudeCells.GetValue(),8.);
    TestEqual(TEXT("Motion frequency scales inversely with steps"),R[2].Spec.Geometry.MotionFrequencyCyclesPerStep.GetValue(),.001);
    TestEqual(TEXT("Angular speed preserves physical spin"),R[2].Spec.Geometry.SpinRadiansPerStep.GetValue(),.002);
    TestTrue(TEXT("Initial angular velocity scales inversely"),R[2].Spec.Geometry.InitialAngularVelocityRadiansPerStep->Equals(FVector(.001,.002,.003)));
    TestTrue(TEXT("Initial attitude and translational speed remain fixed"),R[2].Spec.Geometry.InitialAttitudeDegrees==S.Geometry.InitialAttitudeDegrees&&R[2].Spec.Geometry.InitialVelocityCellsPerStep==S.Geometry.InitialVelocityCellsPerStep);
    TestEqual(TEXT("Explicit streamwise count refines"),R[2].Spec.Lattice.StreamwiseCells.GetValue(),int64(400));
    TestFalse(TEXT("Repeated rungs rejected"),StudioHome4Recipes::Ladder(S,{1,1},R,E));TestEqual(TEXT("Failed ladder retains output"),R.Num(),3);
    TestFalse(TEXT("Missing reference cannot pass"),StudioHome4Recipes::Compare({1},{1},0,0,TEXT("")).bEvaluated);
    TestTrue(TEXT("Exact reference matches"),StudioHome4Recipes::Compare({1,2},{1,2},0,0,TEXT("sha256:fixture")).bPassed);
    TestFalse(TEXT("Mismatch fails"),StudioHome4Recipes::Compare({1,3},{1,2},.01,.01,TEXT("sha256:fixture")).bPassed);
    TestEqual(TEXT("Second-order oracle"),StudioHome4Recipes::ObservedOrder(1.16,1.04,1.01,2).Get(0),2.);
    TestFalse(TEXT("Oscillating convergence has no claimed order"),StudioHome4Recipes::ObservedOrder(1,2,1.5,2).IsSet());return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4Editor,"Studio.Home4.Recipes.RetainedDraftAndPersistence",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4Editor::RunTest(const FString&)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-tests")/FGuid::NewGuid().ToString());FStudioHome4Session S(M);
    const auto Camera=M->Project.Camera;const auto Frame=M->SelectedFrame;
    TestTrue(TEXT("Select supplied template"),S.ApplyRecipe(TEXT("th01-hull")));
    S.Set(TEXT("reference.lengthCells"),TEXT("-invalid"));TestFalse(TEXT("Invalid draft rejected"),S.Apply());TestTrue(TEXT("Invalid draft retained"),S.IsDirty());
    S.Revert();S.Set(TEXT("reference.lengthCells"),TEXT("512"));TestTrue(TEXT("Apply"),S.Apply());
    TestEqual(TEXT("Applied length"),M->Project.Draft.Home4->Reference.LengthCells.GetValue(),512.);
    FString Error;FStudioProject P;TestTrue(TEXT("Project roundtrip"),StudioProjectIO::Parse(StudioProjectIO::Serialize(M->SnapshotProject()),P,Error));
    TestEqual(TEXT("Saved HOME4 length"),P.Draft.Home4->Reference.LengthCells.GetValue(),512.);
    S.Set(TEXT("reference.lengthCells"),TEXT("600"));M->UndoCase();S.Refresh();TestTrue(TEXT("Conflict preserves draft"),S.HasConflict());TestEqual(TEXT("Conflict text"),S.Get(TEXT("reference.lengthCells")),FString(TEXT("600")));
    TestTrue(TEXT("Camera remains independent"),StudioView::CameraEquals(Camera,M->Project.Camera));TestEqual(TEXT("Playback remains independent"),M->SelectedFrame,Frame);return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4TypedRecipeConstants,"Studio.Home4.Recipes.TypedBenchmarkConstantsAndDepartures",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4TypedRecipeConstants::RunTest(const FString&)
{
    const auto Wave=StudioHome4Recipes::Find(TEXT("breaking-wave-banari"))->Template;
    TestEqual(TEXT("Known successful wave phase speed"),Wave.Reference.WavePhaseSpeed.Get(-1),.015);
    TestEqual(TEXT("Banari slope"),Wave.Reference.WaveSlope.Get(-1),.08);
    TestTrue(TEXT("Wave aspects and streamwise count are typed"),Wave.Lattice.StreamwiseCells.Get(-1)==400&&Wave.Lattice.WidthLengthRatio.Get(-1)==.25&&Wave.Lattice.HeightLengthRatio.Get(-1)==.30);
    TestFalse(TEXT("Wave speed does not invent inlet speed"),Wave.Reference.SpeedCellsPerStep.IsSet());
    const auto Osc=StudioHome4Recipes::Find(TEXT("oscillating-cylinder"))->Template;
    TestTrue(TEXT("Dutsch KC and peak speed are independent typed anchors"),Osc.Reference.KeuleganCarpenter.Get(-1)==5&&Osc.Reference.OscillationPeakSpeed.Get(-1)==.04&&!Osc.Reference.SpeedCellsPerStep);
    const auto Spin=StudioHome4Recipes::Find(TEXT("couette-spin"))->Template;
    TestTrue(TEXT("Rotational Reynolds and surface speed are distinct"),Spin.Reference.RotationalReynolds.Get(-1)==100&&Spin.Reference.SpinSurfaceSpeed.Get(-1)==.04&&!Spin.Reference.Reynolds&&!Spin.Reference.SpeedCellsPerStep);
    const auto Sed=StudioHome4Recipes::Find(TEXT("sedimentation"))->Template;
    TestTrue(TEXT("Body density ratio and Galileo anchor"),Sed.Geometry.BodyFluidDensityRatio.Get(-1)==1.25&&Sed.Reference.Galileo.Get(-1)==19.6&&!Sed.Fluids.RhoHeavy);
    TestEqual(TEXT("Barge B/T anchor"),StudioHome4Recipes::Find(TEXT("vugts-barge"))->Template.Geometry.BeamDraftRatio.Get(-1),2.);
    TestEqual(TEXT("Foil h/c anchor"),StudioHome4Recipes::Find(TEXT("hydrofoil-parkin"))->Template.Geometry.SubmergenceChordRatio.Get(-1),1.8);
    for(const auto& R:StudioHome4Recipes::All())
    {
        auto S=R.Template;
        if(S.Reference.KeuleganCarpenter)S.Reference.KeuleganCarpenter=6;
        else if(S.Reference.RotationalReynolds)S.Reference.RotationalReynolds=101;
        else if(S.Reference.Galileo)S.Reference.Galileo=20;
        else if(S.Reference.WavePhaseSpeed)S.Reference.WavePhaseSpeed=.02;
        else if(S.Geometry.BeamDraftRatio)S.Geometry.BeamDraftRatio=3;
        else if(S.Geometry.SubmergenceChordRatio)S.Geometry.SubmergenceChordRatio=2;
        else continue;
        TestTrue(*(R.Id+TEXT(" typed departure requires new validation")),!StudioHome4Recipes::Departures(S).IsEmpty());
    }
    return !HasAnyErrors();
}
#endif
