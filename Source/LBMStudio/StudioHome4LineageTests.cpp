#include "StudioHome4Lineage.h"
#include "SStudioHome4Lineage.h"
#include "StudioHome4Session.h"
#include "StudioHome4Authoring.h"
#include "StudioHeadlessSlate.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4LineageTree,"Studio.Home4.Authoring.LineageTreeEdgesHashesOpaqueSourcesAndChanges",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4LineageTree::RunTest(const FString&)
{
    FStudioProject P;P.Runs.Reset();P.Dataset.Empty();P.Draft.Home4=FStudioHome4Spec();auto& S=*P.Draft.Home4;S.RecipeId=TEXT("th01-hull");S.Run.Backend=EStudioHome4Backend::Metal;S.Reference.Froude=.3;S.LineageId=TEXT("hull-series");
    P.Runs.Add(FStudioRunRecord::Capture(TEXT("parent"),P.Draft,EStudioRunOrigin::ControlHarness));const auto Parent=P.Runs[0];S.ParentRunId=Parent.GetId().ToString();S.ParentSpecSHA256=StudioHome4Authoring::Fingerprint(*Parent.GetConfiguration()->Home4);S.Reference.Froude=.45;P.Runs.Add(FStudioRunRecord::Capture(TEXT("derived"),P.Draft,EStudioRunOrigin::ControlHarness));
    auto Nodes=StudioHome4Lineage::Tree(P);TestTrue(TEXT("Child is retained under exact immutable parent"),Nodes.Num()==2&&Nodes[1].Parent==0&&Nodes[1].Depth==1);
    TestTrue(TEXT("Concrete parameter change is readable"),Nodes.Num()==2&&Nodes[1].Changes.ContainsByPredicate([](const auto& V){return V.Contains(TEXT("0.3"))&&V.Contains(TEXT("0.45"))&&V.Contains(TEXT("→"));}));
    FStudioProject Roundtrip;FString Error;TestTrue(*Error,StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Roundtrip,Error));TestEqual(TEXT("Captured backend is typed HOME4, independent of legacy Setup"),Roundtrip.Runs[0].GetBackendId(),FString(TEXT("metal")));
    FStudioRecordedRunProvenance Original;Original.RunId=TEXT("opaque-source-run");Original.RecipeId=S.RecipeId;Original.ManifestSHA256=FString::ChrN(64,'a');Original.OriginalArchives={TEXT("snap_1000.npz")};Original.OriginalTag=TEXT("hull_viz");Original.OriginalBackend=TEXT("metal");Original.MeasurementSource=TEXT("original trace");Original.MeasurementSHA256=FString::ChrN(64,'b');Original.MeasuredMLUPS=37.2;Original.GateStatus=TEXT("passed");Original.GateSourceSHA256=FString::ChrN(64,'c');Original.OriginalRunSpec=*Parent.GetConfiguration()->Home4;
    P.Runs.Add(FStudioRunRecord::Recording(TEXT("opaque replay"),TEXT("fixture"),false).WithProvenance(Original));TestTrue(*Error,StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Roundtrip,Error));if(Roundtrip.Runs.Num()>2)TestTrue(TEXT("Original opaque identity, snapshot and measured evidence persist"),Roundtrip.Runs[2].GetProvenance()->RunId==Original.RunId&&Roundtrip.Runs[2].GetProvenance()->MeasuredMLUPS==37.2&&Roundtrip.Runs[2].GetProvenance()->OriginalRunSpec.IsSet());
    Original.MeasurementSHA256.Empty();P.Runs.Last()=P.Runs.Last().WithProvenance(Original);TestFalse(TEXT("Unattributed measured values cannot become original persisted metadata"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Roundtrip,Error));
    S.ParentSpecSHA256=FString::ChrN(64,'f');P.Runs[1]=FStudioRunRecord::Capture(TEXT("bad parent SHA"),P.Draft,EStudioRunOrigin::ControlHarness);Nodes=StudioHome4Lineage::Tree(P);const auto* Bad=Nodes.FindByPredicate([](const auto& N){return N.RunIndex==1;});TestTrue(TEXT("A mismatched edge is surfaced and never attached"),Bad&&Bad->Parent==INDEX_NONE&&Bad->Issue.Contains(TEXT("mismatch")));
    auto Cycle=Original;Cycle.MeasuredMLUPS.Reset();Cycle.GateStatus.Empty();Cycle.RunId=TEXT("cycle-a");Cycle.ParentRunId=TEXT("cycle-b");Cycle.ParentSpecSHA256=StudioHome4Authoring::Fingerprint(*Cycle.OriginalRunSpec);P.Runs.Add(FStudioRunRecord::Recording(TEXT("cycle a"),TEXT("a"),false).WithProvenance(Cycle));
    Cycle.RunId=TEXT("cycle-b");Cycle.ParentRunId=TEXT("cycle-a");P.Runs.Add(FStudioRunRecord::Recording(TEXT("cycle b"),TEXT("b"),false).WithProvenance(Cycle));Nodes=StudioHome4Lineage::Tree(P);
    TestEqual(TEXT("Cycle checking retains every card once"),Nodes.Num(),P.Runs.Num());TestTrue(TEXT("Cyclic declared parents are rejected visibly"),Nodes.FilterByPredicate([](const auto& N){return N.Issue.Contains(TEXT("Cyclic"))&&N.Parent==INDEX_NONE;}).Num()==2);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4LineageNative,"Studio.HeadlessUI.Home4.Authoring.DeriveImmutableSnapshotIntoRetainedBranch",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4LineageNative::RunTest(const FString&)
{
    auto Model=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-lineage-ui")/FGuid::NewGuid().ToString());auto Session=MakeShared<FStudioHome4Session>(Model);Session->ApplyRecipe(TEXT("th01-hull"));Session->Set(TEXT("reference.froude"),TEXT("0.3"));Session->Apply();Model->Project.Runs.Reset();Model->Project.Runs.Add(FStudioRunRecord::Capture(TEXT("frozen parent"),Model->Project.Draft,EStudioRunOrigin::ControlHarness));const auto Parent=Model->Project.Runs[0];const FString Before=StudioHome4Config::Serialize(*Model->Project.Draft.Home4);bool Navigated=false;
    auto Widget=SNew(SStudioHome4Lineage).Model(Model).Session(Session).OnAuthoring_Lambda([&]{Navigated=true;});FStudioHeadlessSlate UI(*this,Widget,FVector2D(690,640));const FName Tag(*(TEXT("Home4Lineage.derive.")+Parent.GetId().ToString()));UI.Press(Tag);
    FStudioHome4Spec Draft;FString Error;TestTrue(*Error,Session->Build(Draft,Error));TestTrue(TEXT("Native derive creates a retained branch with exact immutable parent"),Navigated&&Session->IsDirty()&&Draft.ParentRunId==Parent.GetId().ToString()&&Draft.ParentSpecSHA256==StudioHome4Authoring::Fingerprint(*Parent.GetConfiguration()->Home4)&&!Draft.BranchId.IsEmpty());
    TestEqual(TEXT("Derivation never changes the applied request"),StudioHome4Config::Serialize(*Model->Project.Draft.Home4),Before);Session->Set(TEXT("reference.froude"),TEXT("0.45"));TestTrue(TEXT("Apply branch saves one next-run edit"),Session->Apply());TestTrue(TEXT("Parent remains unchanged"),Parent.GetConfiguration()->Home4->Reference.Froude==.3&&Model->Project.Draft.Home4->Reference.Froude==.45);
    return !HasAnyErrors();
}
#endif
