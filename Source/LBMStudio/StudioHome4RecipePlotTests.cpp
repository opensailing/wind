#include "StudioHome4RecipePlot.h"
#include "SStudioHome4Sizing.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"
#include "InputCoreTypes.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RecipeFamilies,"Studio.Home4.Authoring.RecipeFamiliesAndRequestedNumericalEnvelope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4RecipeFamilies::RunTest(const FString&)
{
    FStudioHome4Spec S;S.Reference.LengthCells=256;S.Reference.SpeedCellsPerStep=.04;S.Reference.Reynolds=100;S.Fluids.NuHeavy=.04*256/100;S.Fluids.NuLight=*S.Fluids.NuHeavy;
    const auto Before=StudioHome4Config::Serialize(StudioHome4Recipes::Find(TEXT("th01-hull"))->Template);const auto F=StudioHome4RecipePlot::Families(S);TestEqual(TEXT("Every recipe has an explicit original/user-selected sizing family"),F.Num(),10);
    for(const auto& P:F){TestFalse(TEXT("Incomplete original catalog never fabricates validated fixed points"),P.bOriginalPoint);TestTrue(TEXT("Every recipe remains visibly parametrized"),!P.Label.IsEmpty()&&!P.Reason.IsEmpty()&&!P.Points.IsEmpty());}
    const auto* Magnus=F.FindByPredicate([](const auto& P){return P.Id==TEXT("magnus");});TestTrue(TEXT("Original Magnus U defines horizontal Mach family"),Magnus&&Magnus->Points.Num()==2&&FMath::IsNearlyEqual(Magnus->Points[0].Y,.05*FMath::Sqrt(3.),1e-12));
    const auto* Hull=F.FindByPredicate([](const auto& P){return P.Id==TEXT("th01-hull");});TestTrue(TEXT("Original hull resolution remains a vertical family, not fake Mach"),Hull&&Hull->Points.Num()==2&&Hull->Points[0].X==256&&Hull->Points[1].X==256);
    TestTrue(TEXT("Numerical envelope admits bounded Ma/tau only"),StudioHome4RecipePlot::WithinNumericalEnvelope(S,256,.05));TestFalse(TEXT("Numerical envelope excludes excessive Mach"),StudioHome4RecipePlot::WithinNumericalEnvelope(S,256,.2));TestFalse(TEXT("Numerical envelope excludes tau too close to one-half"),StudioHome4RecipePlot::WithinNumericalEnvelope(S,16,.001));
    TestEqual(TEXT("Viewing parametrized recipes never changes documented catalog values"),StudioHome4Config::Serialize(StudioHome4Recipes::Find(TEXT("th01-hull"))->Template),Before);return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SizingNative,"Studio.HeadlessUI.Home4.Authoring.NativeSizingUpdatesActualLatticeAndAllocation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SizingNative::RunTest(const FString&)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-sizing-native")/FGuid::NewGuid().ToString());auto Session=MakeShared<FStudioHome4Session>(M);Session->ApplyRecipe(TEXT("th01-hull"));Session->Set(TEXT("reference.speedCellsPerStep"),TEXT("0.02"));Session->Set(TEXT("reference.reynolds"),TEXT("100"));Session->Set(TEXT("lattice.extents"),TEXT("256,128,128"));Session->AddAllocation();Session->SetAllocation(0,5,TEXT("root"));Session->Apply();const auto Before=*M->Project.Draft.Home4;
    auto Widget=SNew(SStudioHome4Sizing).Session(Session);FStudioHeadlessSlate UI(*this,Widget,FVector2D(690,520));UI.Focus(TEXT("Home4Size0"));UI.Key(EKeys::Right);FStudioHome4Spec After;FString Error;TestTrue(*Error,Session->Build(After,Error));TestTrue(TEXT("Native slider key changes actual root extent and scoped allocation"),Session->IsDirty()&&After.Lattice.Extents!=Before.Lattice.Extents&&After.Performance.Allocations[0].Nodes!=Before.Performance.Allocations[0].Nodes);TestTrue(TEXT("Native slider conserves requested Reynolds"),After.Reference.Reynolds==100);return !HasAnyErrors();
}
#endif
