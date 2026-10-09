#include "StudioModel.h"
#include "StudioMaterials.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags FlowTestFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FStudioCaseDraft FlowCaseFixture()
{
    FStudioCaseDraft C;FStudioMaterial Fluid;Fluid.KinematicViscosity=.000015;
    C.Materials.Add(Fluid);C.Domain.FluidMaterialId=Fluid.Id;
    C.Setup.InletVelocity=FVector(3,4,0);C.Setup.ReferenceLength=.6;C.Setup.ReferenceDensity=1.225;
    C.Setup.OutletPressure=-123.45678901234567;C.Setup.ReynoldsNumber=200000.;return C;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFlowUnits,"Studio.FlowConditions.UnitsAndRetainedInvalidValues",FlowTestFlags)
bool FStudioFlowUnits::RunTest(const FString&)
{
    auto C=FlowCaseFixture();FStudioFlowConditionsEdit E;E.Reset(C,false);FStudioCaseSetup Out;
    TestTrue(TEXT("Original exact inputs build"),E.Build(Out));
    TestFalse(TEXT("Loaded form clean"),E.IsDirty());
    for(auto F:{E.VelocityX,E.Pressure,E.Length,E.Density})
    {
        TestTrue(TEXT("Change to alternative unit"),E.ChangeUnit(F,1));
        TestFalse(TEXT("Unit selection is not a case edit"),E.IsDirty());
        TestTrue(TEXT("Converted inputs build"),E.Build(Out));
        TestTrue(TEXT("Unchanged SI values remain bit exact"),Out.InletVelocity==C.Setup.InletVelocity && Out.OutletPressure==C.Setup.OutletPressure &&
            Out.ReferenceLength==C.Setup.ReferenceLength && Out.ReferenceDensity==C.Setup.ReferenceDensity);
        TestTrue(TEXT("Restore SI"),E.ChangeUnit(F,0));
    }
    E.ChangeUnit(E.VelocityX,1);E.Values[0]=TEXT("36");E.Values[1]=TEXT("-72");E.Values[2]=TEXT("0");
    E.ChangeUnit(E.Pressure,1);E.Values[E.Pressure]=TEXT("-3.25");
    E.ChangeUnit(E.Length,2);E.Values[E.Length]=TEXT("450");
    E.ChangeUnit(E.Density,1);E.Values[E.Density]=TEXT("0.0012");
    TestTrue(TEXT("Explicit units convert"),E.Build(Out));
    TestTrue(TEXT("Known SI dimensional oracle"),Out.InletVelocity==TOptional<FVector>(FVector(10,-20,0)) && Out.OutletPressure.Get(0)==-3250 &&
        FMath::IsNearlyEqual(Out.ReferenceLength.Get(0),.45,1.e-15) && Out.ReferenceDensity.Get(0)==1.2);
    const auto Kept=Out;
    for(const TCHAR* Bad:{TEXT("nan"),TEXT("inf"),TEXT("12 mph"),TEXT("1,25"),TEXT("1e999"),TEXT("1e-999"),TEXT("1e13")})
    {
        E.Reset(C,false);E.Values[E.Length]=Bad;TestFalse(TEXT("Invalid number rejected"),E.Build(Out));
        TestEqual(TEXT("Retain invalid draft"),E.Values[E.Length],FString(Bad));
        TestTrue(TEXT("Failure leaves output unchanged"),Out.InletVelocity==Kept.InletVelocity&&Out.ReferenceLength==Kept.ReferenceLength);
    }
    E.Reset(C,false);E.Values[1]=TEXT("bad");const FString X=E.Values[0];
    TestFalse(TEXT("Unit change rejects malformed vector atomically"),E.ChangeUnit(E.VelocityX,1));
    TestTrue(TEXT("All component units and drafts retained"),E.Units[0]==0&&E.Units[1]==0&&E.Values[0]==X&&E.Values[1]==TEXT("bad"));
    E.Values[1].Empty();TestFalse(TEXT("Partial inlet is not zero-filled"),E.Build(Out));
    E.Values[0].Empty();E.Values[2].Empty();E.Values[E.Length].Empty();E.Values[E.Density].Empty();E.Values[E.Pressure].Empty();E.Values[E.Reynolds].Empty();
    TestTrue(TEXT("Unknown values remain unspecified"),E.Build(Out)&&!Out.InletVelocity&&!Out.ReferenceLength&&!Out.ReferenceDensity&&!Out.OutletPressure&&!Out.ReynoldsNumber);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFlowCalculator,"Studio.FlowConditions.ReynoldsAndDirectionOracle",FlowTestFlags)
bool FStudioFlowCalculator::RunTest(const FString&)
{
    auto C=FlowCaseFixture();FStudioFlowConditionsEdit E;E.Reset(C,false);
    // Independent 3-4-5 speed, 0.6 m length, 15 mm^2/s => Re 200000.
    TestTrue(TEXT("Published Re definition gives known dimensional result"),FMath::IsNearlyEqual(E.CalculatedReynolds().Get(0),200000.,1.e-8));
    E.Values[E.Reynolds]=TEXT("broken");TestTrue(TEXT("Calculator repairs target without parsing it"),E.UseCalculatedReynolds());
    E.Values[E.Reynolds]=TEXT("400000");TestTrue(TEXT("Target updates draft speed"),E.UseTargetSpeed());
    FStudioCaseSetup Out;TestTrue(TEXT("Calculated draft builds"),E.Build(Out));
    TestTrue(TEXT("Direction preserved; double target doubles every component"),Out.InletVelocity.Get(FVector::ZeroVector).Equals(FVector(6,8,0),1.e-12));
    TestTrue(TEXT("Calculated Re follows changed draft"),FMath::IsNearlyEqual(E.CalculatedReynolds().Get(0),400000.,1.e-8));
    TestEqual(TEXT("Applied case remains unchanged"),C.Setup.InletVelocity.GetValue(),FVector(3,4,0));
    E.Reset(C,false);E.Values[0]=TEXT("0");E.Values[1]=TEXT("0");
    TestTrue(TEXT("Zero speed has calculable zero Reynolds"),E.CalculatedReynolds().IsSet()&&E.CalculatedReynolds().GetValue()==0);
    TestFalse(TEXT("Target cannot invent a direction"),E.UseTargetSpeed());
    E.Reset(C,false);E.Viscosity.Reset();TestFalse(TEXT("No invented viscosity"),E.CalculatedReynolds().IsSet()||E.UseTargetSpeed()||E.UseCalculatedReynolds());
    E.Reset(C,false);E.Values[E.Length]=TEXT("1e-300");E.Values[E.Reynolds]=TEXT("1e12");
    TestFalse(TEXT("Speed overflow rejected"),E.UseTargetSpeed());
    E.Reset(C,false);E.Values[E.Length]=TEXT("1e12");E.Viscosity=1.e-300;E.Values[E.Reynolds]=TEXT("1e-100");
    TestFalse(TEXT("Speed underflow rejected"),E.UseTargetSpeed());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFlowTransaction,"Studio.FlowConditions.TransactionsDependenciesAndIsolation",FlowTestFlags)
bool FStudioFlowTransaction::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/FlowConditions")/FGuid::NewGuid().ToString());M.Pause();
    if(!TestTrue(TEXT("Prepare flow fixture"),M.EditCase(TEXT("Flow fixture"),[](auto& C){const auto Id=C.Id;C=FlowCaseFixture();C.Id=Id;})))return false;
    auto Solver=M.Solver;auto Camera=M.Project.Camera;auto Render=M.RenderIntentRevision;auto Frame=M.SelectedFrame;
    FStudioFlowConditionsEdit E;E.Reset(M.Project.Draft);E.Values[0]=TEXT("9");
    M.EditCase(TEXT("Other settings"),[](auto& C){C.Setup.MaxSteps=1234;C.Setup.TimeStep=.000123456789;C.Setup.LatticeResolution=FIntVector(12,34,56);});
    auto Before=M.Project.Draft;TestTrue(TEXT("Unrelated edit does not conflict"),E.Matches(Before));
    TestTrue(TEXT("Flow transaction applies"),M.UpdateFlowConditions(E));auto Applied=M.Project.Draft;
    TestTrue(TEXT("Other setup ownership preserved"),Applied.Setup.MaxSteps==1234&&Applied.Setup.TimeStep==Before.Setup.TimeStep&&Applied.Setup.LatticeResolution==Before.Setup.LatticeResolution);
    TestTrue(TEXT("No recorded field or camera edit"),M.Solver==Solver&&StudioView::CameraEquals(M.Project.Camera,Camera)&&M.RenderIntentRevision==Render&&M.SelectedFrame==Frame);
    TestTrue(TEXT("One undo"),M.UndoCase());auto Restored=M.Project.Draft;Restored.Revision=Before.Revision;
    TestEqual(TEXT("Whole prior case restored"),StudioCaseIO::Serialize(Restored),StudioCaseIO::Serialize(Before));TestTrue(TEXT("Redo"),M.RedoCase());
    FStudioProject Reopened;FString Error;
    TestTrue(TEXT("Project roundtrip"),StudioProjectIO::Parse(StudioProjectIO::Serialize(M.SnapshotProject()),Reopened,Error));
    TestEqual(TEXT("All flow values persist exactly"),StudioCaseIO::Serialize(Reopened.Draft),StudioCaseIO::Serialize(M.Project.Draft));
    E.Reset(M.Project.Draft);E.Values[0]=TEXT("99");M.EditCase(TEXT("Viscosity dependency"),[](auto& C){C.Materials[0].KinematicViscosity=.01;});
    const FString Current=StudioCaseIO::Serialize(M.Project.Draft);
    TestFalse(TEXT("Viscosity conflict blocks stale draft"),M.UpdateFlowConditions(E));TestEqual(TEXT("Conflict is atomic"),StudioCaseIO::Serialize(M.Project.Draft),Current);
    E.Reset(M.Project.Draft);M.EditCase(TEXT("Material rename"),[](auto& C){C.Materials[0].Name=TEXT("Renamed");});
    TestTrue(TEXT("Unrelated material name does not conflict"),E.Matches(M.Project.Draft));
    TestTrue(TEXT("Enable control harness"),M.SetControlHarness(true));TestTrue(TEXT("Submit case"),M.Control(EStudioJobCommand::Submit));M.Tick(.1);
    const FString Frozen=StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration());E.Reset(M.Project.Draft);E.Values[0]=TEXT("21");
    TestTrue(TEXT("Next-run edits allowed"),M.UpdateFlowConditions(E));TestEqual(TEXT("Active run frozen"),StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration()),Frozen);
    M.Control(EStudioJobCommand::Stop);M.Tick(.1);return true;
}
#endif
