#include "StudioHeadlessSlate.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "SStudioFlowConditions.h"
#include "StudioModel.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"

namespace StudioHeadlessTests
{
bool FlowWorkflow(FAutomationTestBase& Test,double Width)
{
    if(!Test.TestTrue(TEXT("Slate is initialized in headless editor"),FSlateApplication::IsInitialized()))return false;
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/headless-flow")/FGuid::NewGuid().ToString();
    auto Model=MakeShared<FStudioModel>(Root);auto& M=*Model;
    M.EditCase(TEXT("Known reference fluid"),[](auto& Case)
    {FStudioMaterial Fluid;Fluid.Name=TEXT("Reference fluid");Fluid.KinematicViscosity=.000015;Case.Materials.Add(Fluid);Case.Domain.FluidMaterialId=Fluid.Id;});
    const auto Camera=M.Project.Camera;const int32 Frame=M.SelectedFrame;const auto Intent=M.RenderIntentRevision;
    int32 MaterialsVisits=0;
    auto Form=SNew(SStudioFlowConditions).Model(Model).OnMaterials_Lambda([&]{++MaterialsVisits;});
    FStudioHeadlessSlate UI(Test,Form,FVector2D(Width,900));
    const FString Prefix=FString::Printf(TEXT("flow-%d"),int32(Width));
    if(!UI.Inspect(Prefix+TEXT("-initial"),{TEXT("FlowValue0"),TEXT("FlowValue1"),TEXT("FlowValue2"),TEXT("FlowApply")}))return false;
    Test.TestFalse(TEXT("Clean draft disables Apply"),UI.Find(TEXT("FlowApply"))->IsEnabled());
    if(!UI.Press(TEXT("FlowAdvanced")))return false;
    for(const auto Pair:TArray<TPair<FName,FString>>{{TEXT("FlowValue0"),TEXT("3")},{TEXT("FlowValue1"),TEXT("4")},
        {TEXT("FlowValue2"),TEXT("0")},{TEXT("FlowValue3"),TEXT("-12.345678901234567")},
        {TEXT("FlowValue4"),TEXT("0.6")},{TEXT("FlowValue5"),TEXT("1.225")}})
        if(!UI.Type(Pair.Key,Pair.Value))return false;
    Test.TestFalse(TEXT("Routed typing preserves case until Apply"),M.Project.Draft.Setup.InletVelocity.IsSet());
    Test.TestTrue(TEXT("Widget reports unapplied changes"),Form->HasUnapplied());
    if(!UI.Press(TEXT("FlowCalculate")))return false;
    Test.TestEqual(TEXT("Widget calculator uses source fluid"),FCString::Atod(*UI.Text(TEXT("FlowValue6"))),200000.);
    if(!UI.Type(TEXT("FlowValue6"),TEXT("400000"))||!UI.Press(TEXT("FlowTargetSpeed")))return false;
    Test.TestEqual(TEXT("Target speed preserves X direction"),UI.Text(TEXT("FlowValue0")),FString(TEXT("6")));
    Test.TestEqual(TEXT("Target speed preserves Y direction"),UI.Text(TEXT("FlowValue1")),FString(TEXT("8")));
    if(!UI.Press(TEXT("FlowUnits0"))||!UI.Press(TEXT("FlowUnit0_1")))return false;
    Test.TestTrue(TEXT("Virtual popup converts velocity units"),FMath::IsNearlyEqual(FCString::Atod(*UI.Text(TEXT("FlowValue0"))),21.6,1.e-12));
    if(!UI.Type(TEXT("FlowValue4"),TEXT("-1"))||!UI.Press(TEXT("FlowApply")))return false;
    Test.TestFalse(TEXT("Invalid UI submission is transactional"),M.Project.Draft.Setup.InletVelocity.IsSet());
    Test.TestEqual(TEXT("Invalid input retained"),UI.Text(TEXT("FlowValue4")),FString(TEXT("-1")));
    Test.TestTrue(TEXT("Invalid field receives focus"),UI.Find(TEXT("FlowValue4"))->HasFocusedDescendants());
    if(!UI.Inspect(Prefix+TEXT("-invalid"),{TEXT("FlowValue4"),TEXT("FlowStatus"),TEXT("FlowApply")}))return false;
    if(!UI.Type(TEXT("FlowValue4"),TEXT("0.6"))||!UI.Press(TEXT("FlowApply")))return false;
    Test.TestTrue(TEXT("Apply converts draft to exact SI"),M.Project.Draft.Setup.InletVelocity.Get(FVector::ZeroVector)==FVector(6,8,0));
    Test.TestEqual(TEXT("Pressure retains full precision"),M.Project.Draft.Setup.OutletPressure.Get(0),-12.345678901234567);
    Test.TestFalse(TEXT("Applied widget is clean"),Form->HasUnapplied());
    Test.TestTrue(TEXT("Undo succeeds"),M.UndoCase());Form->Refresh();
    Test.TestEqual(TEXT("Undo updates actual text box"),UI.Text(TEXT("FlowValue0")),FString());
    Test.TestTrue(TEXT("Redo succeeds"),M.RedoCase());Form->Refresh();
    if(!UI.Type(TEXT("FlowValue6"),TEXT("99")))return false;
    M.EditCase(TEXT("External fluid edit"),[](auto& Case){Case.Materials[0].KinematicViscosity=.00002;});Form->Refresh();
    Test.TestFalse(TEXT("Conflict disables the actual Apply button"),UI.Find(TEXT("FlowApply"))->IsEnabled());
    Test.TestEqual(TEXT("Conflict retains entered value"),UI.Text(TEXT("FlowValue6")),FString(TEXT("99")));
    Test.TestTrue(TEXT("Conflict is visible in widget text"),UI.Text(TEXT("FlowStatus")).Contains(TEXT("changed")));
    if(!UI.Inspect(Prefix+TEXT("-conflict"),{TEXT("FlowApply"),TEXT("FlowRevert"),TEXT("FlowStatus")}))return false;
    if(!UI.Press(TEXT("FlowRevert"))||!UI.Press(TEXT("FlowMaterials")))return false;
    Test.TestEqual(TEXT("Materials action uses its existing owner"),MaterialsVisits,1);
    Test.TestTrue(TEXT("Camera unaffected"),StudioView::CameraEquals(Camera,M.Project.Camera));
    Test.TestEqual(TEXT("Original frame unaffected"),M.SelectedFrame,Frame);
    Test.TestEqual(TEXT("Field render intent unaffected"),M.RenderIntentRevision,Intent);
    return !Test.HasAnyErrors();
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHeadlessFlowCompact,"Studio.HeadlessUI.FlowConditions.Compact",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHeadlessFlowCompact::RunTest(const FString&){return StudioHeadlessTests::FlowWorkflow(*this,300);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHeadlessFlowWide,"Studio.HeadlessUI.FlowConditions.Wide",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHeadlessFlowWide::RunTest(const FString&){return StudioHeadlessTests::FlowWorkflow(*this,360);}
#endif
