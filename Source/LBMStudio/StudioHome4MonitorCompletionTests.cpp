#include "StudioHome4Telemetry.h"
#include "StudioHome4BodyDiagnostics.h"
#include "StudioHome4Runtime.h"
#include "SStudioHome4Monitors.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4MonitorCompletionFixtures
{
    void Append(FStudioHome4TelemetryStream& Stream,const FString& Line)
    {const FTCHARToUTF8 Bytes(*Line);Stream.AppendBytes(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());}
    FString Metadata()
    {return TEXT("{\"kind\":\"source_metadata\",\"source_metadata\":{\"force_units\":\"physical\",\"velocity_units\":\"physical\",\"length_units\":\"physical\",\"declared_levels\":[0,1],\"divergence_convention\":\"volume L2\",\"divergence_unit\":\"1/s\",\"divergence_domain\":\"original liquid mask\",\"device_peak_gbps\":100,\"device_peak_source\":\"identified original fixture device specification\",\"unit_map\":{\"dx_m\":0.01,\"dt_s\":0.001,\"rho_kg_m3\":1000,\"length_cells\":10,\"time_steps\":100}}}\n");}
    FString Measurement()
    {return TEXT("{\"step\":1,\"umax\":2,\"ma_inst\":0.01,\"tau_min\":0.51,\"div_u_norm\":0.0001,\"mass_ledger\":{\"phi\":0.00001},\"wb_rest\":{\"at_rest\":true,\"full_gravity\":true,\"pd_max\":0},\"performance\":{\"elapsed_seconds\":1,\"node_updates\":1000000,\"transferred_bytes\":50000000000},\"interface\":{\"spurious_speed\":0.01,\"spurious_mask\":\"forcing-free droplet\",\"spurious_unit\":\"m/s\",\"forcing_free\":true,\"at_rest\":true,\"spurious_reference\":{\"speed\":0.01,\"absolute_tolerance\":0,\"source\":\"identified droplet fixture reference\"},\"thickness_histogram\":{\"edges\":[3,4,5,6],\"counts\":[1,5,20],\"phi_min\":0.1,\"phi_max\":0.9,\"expected_xi\":5,\"thickness_unit\":\"m\",\"sampling_source\":\"original interface cells\"}},\"bodies\":[{\"id\":\"hull\",\"forces\":{\"Fz\":14,\"My\":8},\"window\":{\"Fz\":14,\"Fz_prev\":14,\"abscissa_unit\":\"s\",\"epoch\":\"identified physical zero\",\"start\":0,\"end\":1},\"attitude\":{\"k33\":10,\"k35\":2,\"k55\":4,\"k33_unit\":\"N/m\",\"k35_unit\":\"N\",\"k55_unit\":\"N m\",\"stiffness_convention\":\"symmetric_heave_m_pitch_rad_load_Fz_N_My_Nm\"},\"fit\":{\"added_mass\":2,\"damping\":3,\"frequency\":0.5,\"frequency_unit\":\"Hz\",\"method\":\"original least-squares harmonic fit\",\"window_start\":2,\"window_end\":10,\"window_unit\":\"s\",\"epoch\":\"identified physical zero\"}}]}\n");}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4OriginalDiagnosticMetadataTest,"Studio.Home4.Telemetry.OriginalLevelHistogramNormFitAndRestConventions",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4OriginalDiagnosticMetadataTest::RunTest(const FString&)
{
    using namespace StudioHome4MonitorCompletionFixtures;FStudioHome4TelemetryStream S;S.BeginRun({FGuid::NewGuid(),TEXT("original diagnostic fixture")});Append(S,Metadata()+Measurement());
    if(!TestTrue(TEXT("Original convention records accepted"),S.Latest().IsSet()))return false;
    const auto& Sample=*S.Latest();TestEqual(TEXT("Declared original level inventory retained"),Sample.Metadata->DeclaredLevels.Num(),2);
    TestEqual(TEXT("Original norm definition retained"),Sample.Metadata->DivergenceConvention,FString(TEXT("volume L2")));
    TestEqual(TEXT("Histogram phi selection retained"),Sample.InterfaceThickness.PhiMinimum.Get(-1),.1);
    TestEqual(TEXT("Histogram original expected xi retained"),Sample.InterfaceThickness.ExpectedXi.Get(-1),5.);
    TestEqual(TEXT("Original forcing frequency unit retained"),Sample.Bodies[0].FitFrequencyUnit,FString(TEXT("Hz")));
    FStudioHome4DiagnosticPolicy P;P.RestPressureTolerance=0;auto Health=FStudioHome4Diagnostics::Evaluate(Sample,P);
    TestTrue(TEXT("Missing declared MD ledger blocks full drift health"),Health[0].Status==EStudioHome4Health::Unavailable);
    TestTrue(TEXT("Missing adjacent correction history is explicit"),Health[0].Reason.Contains(TEXT("trend coverage is unknown")));
    TestTrue(TEXT("Original full-gravity exact-zero criterion can be healthy"),Health[4].Status==EStudioHome4Health::Healthy);
    TestTrue(TEXT("Exact-zero criterion is distinct from relaxed tolerance"),Health[4].Label.Contains(TEXT("exact zero")));
    auto Changed=Sample;Changed.RestFullGravity=false;TestTrue(TEXT("Gravity-disabled rest sample cannot pass full-gravity gate"),FStudioHome4Diagnostics::Evaluate(Changed,P)[4].Status==EStudioHome4Health::Unavailable);
    FStudioHome4TelemetryStream Bad;Bad.BeginRun({FGuid::NewGuid(),TEXT("invalid convention fixture")});Append(Bad,TEXT("{\"step\":1,\"interface\":{\"thickness_histogram\":{\"edges\":[1,2],\"counts\":[1],\"phi_min\":0.9,\"phi_max\":0.1}}}\n"));
    TestFalse(TEXT("Reversed source sampling interval cannot become a measurement"),Bad.Latest().IsSet());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ExplicitHydrostaticInverseTest,"Studio.Home4.Telemetry.ExplicitHydrostaticInverseRejectsMissingAndSingularConventions",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ExplicitHydrostaticInverseTest::RunTest(const FString&)
{
    using namespace StudioHome4MonitorCompletionFixtures;FStudioHome4TelemetryStream S;S.BeginRun({FGuid::NewGuid(),TEXT("original hydrostatic fixture")});Append(S,Metadata()+Measurement());
    if(!S.Latest()||S.Latest()->Bodies.IsEmpty()){AddError(TEXT("Original body fixture unavailable."));return false;}
    auto B=S.Latest()->Bodies[0];const auto Result=StudioHome4BodyDiagnostics::QuasiStatic(*S.Latest(),B);
    TestTrue(TEXT("Declared compatible original K inverse is available"),Result.HeaveMeters.IsSet()&&Result.PitchDegrees.IsSet());
    TestTrue(TEXT("Declared K reproduces supplied heave load"),FMath::IsNearlyEqual(Result.HeaveMeters.Get(0),10./9.,1.e-12));
    TestTrue(TEXT("Declared K reproduces supplied pitch load"),FMath::IsNearlyEqual(FMath::DegreesToRadians(Result.PitchDegrees.Get(0)),13./9.,1.e-12));
    B.K35Unit.Empty();TestFalse(TEXT("Mixed stiffness units cannot be silently inferred"),StudioHome4BodyDiagnostics::QuasiStatic(*S.Latest(),B).HeaveMeters.IsSet());
    B=S.Latest()->Bodies[0];B.K33=1;B.K35=1;B.K55=1;TestFalse(TEXT("Singular matrix has no invented attitude"),StudioHome4BodyDiagnostics::QuasiStatic(*S.Latest(),B).HeaveMeters.IsSet());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4NativeSecondaryDiagnosticsTest,"Studio.Home4.Monitors.NativeOriginalSecondaryDiagnosticsAndRestRequest",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4NativeSecondaryDiagnosticsTest::RunTest(const FString&)
{
    using namespace StudioHome4MonitorCompletionFixtures;if(!FSlateApplication::IsInitialized()){AddError(TEXT("Native Slate unavailable."));return false;}
    auto Model=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-monitor-secondary")/FGuid::NewGuid().ToString());FStudioHome4Spec Spec;Spec.Fluids.Gravity=.001;Spec.Reference.SpeedCellsPerStep=.04;Spec.Geometry.BodyMotion=TEXT("free");Model->Project.Draft.Home4=Spec;
    auto Runtime=MakeShared<FStudioHome4RuntimeSession>(Model);FStudioHome4BackendVerification V;V.Id=FGuid::NewGuid();V.Target=TEXT("unit");V.Host=TEXT("unit");V.Device=TEXT("unit");V.Source=TEXT("Development fixture only");V.EffectiveBackend=EStudioHome4Backend::Metal;V.ExtensionImported=true;V.bDevelopmentResponse=true;FString Error;Runtime->SetBackendVerification(V,Error);
    auto Stream=MakeShared<FStudioHome4TelemetryStream>();Stream->BeginRun({FGuid::NewGuid(),TEXT("original secondary fixture")});Append(*Stream,Metadata()+Measurement());
    {
    const auto Panel=SNew(SStudioHome4Monitors).Model(Model).Stream(Stream);
    FStudioHeadlessSlate UI(*this,Panel,FVector2D(1000,2400));
    UI.Press(TEXT("Home4Section_Interface"));TestTrue(TEXT("Histogram selection and xi explicitly visible"),UI.Text(TEXT("Home4Detail_Interface")).Contains(TEXT("complete-bin count above expected")));
    TestTrue(TEXT("Original forcing-free reference criterion shown independently"),UI.Text(TEXT("Home4Detail_Interface")).Contains(TEXT("comparison passed")));
    UI.Press(TEXT("Home4SelectBody"));UI.Press(TEXT("Home4Section_Bodies"));TestTrue(TEXT("Original K inverse method reachable"),UI.Text(TEXT("Home4Detail_Bodies")).Contains(TEXT("Calculated quasi-static")));
    UI.Press(TEXT("Home4Section_Performance"));TestTrue(TEXT("Attributed device bandwidth ratio shown"),UI.Text(TEXT("Home4Detail_Performance")).Contains(TEXT("achieved/peak 0.5")));
    UI.Press(TEXT("Home4Section_Safeguards"));TestTrue(TEXT("Original divergence norm convention shown"),UI.Text(TEXT("Home4Detail_Safeguards")).Contains(TEXT("volume L2")));
    }
    const auto RestPanel=SNew(SStudioHome4Monitors).Model(Model).Runtime(Runtime);
    FStudioHeadlessSlate UI(*this,RestPanel,FVector2D(1000,2400));
    TestTrue(TEXT("Native U=0 full-gravity request action is routed"),UI.Press(TEXT("Home4QueueRestTest")));
    if(!TestEqual(TEXT("One immutable development rest request queued"),Runtime->QueueJobs().Num(),1))return false;
    const auto& Frozen=Runtime->QueueJobs()[0].FrozenSpec;TestEqual(TEXT("Rest request freezes exact zero U"),Frozen.Reference.SpeedCellsPerStep.Get(-1),0.);TestEqual(TEXT("Rest request retains explicitly supplied gravity"),Frozen.Fluids.Gravity.Get(-1),.001);
    TestEqual(TEXT("Next-run draft U stays unchanged"),Model->Project.Draft.Home4->Reference.SpeedCellsPerStep.Get(-1),.04);TestFalse(TEXT("Rest request does not fabricate CFD stream"),Runtime->ScienceStream().IsValid());
    return true;
}
#endif
