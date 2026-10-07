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
    TArray<FStudioHome4LadderRung> R;FString E;TestTrue(TEXT("Generate ordered refinement"),StudioHome4Recipes::Ladder(S,{1,2,4},R,E));
    if(R.Num()!=3)return false;
    TestEqual(TEXT("Fixed Cn scales xi"),R[2].Spec.Fluids.Xi.GetValue(),16.);TestEqual(TEXT("Viscosity preserves Re"),R[2].Spec.Fluids.NuHeavy.GetValue(),.04);
    TestEqual(TEXT("Gravity preserves Fr"),R[2].Spec.Fluids.Gravity.GetValue(),.000025);
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
#endif
