#include "StudioProbeProfile.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Serialization/Csv/CsvParser.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FStudioProbeRequest ProfileRequest(FRecordedSolver& Solver)
{
    FStudioProbeRequest R;R.ProjectId=FGuid::NewGuid();R.PresentationId=27;R.Field=Solver.CaptureViewField(0,TEXT("pressure"),false);
    const auto& D=Solver.Descriptor();R.Probe.Source={D.Id,D.MetadataSHA256,D.PayloadSHA256};R.Probe.Name=TEXT("Wing, \"reference\"");
    R.Probe.Kind=EStudioProbeKind::Line;R.Probe.Samples=65;R.DisplayedScalar=TEXT("pressure");return R;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProfileGaps,"Studio.Inspection.LineProfileCoverage",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioProfileGaps::RunTest(const FString&)
{
    FRecordedSolver Solver;auto R=ProfileRequest(Solver);
    R.Probe.A=FVector(-.8,0,0);R.Probe.B=FVector(.9,0,0);
    const auto Samples=StudioProbeSampling::Evaluate(R);const auto Profile=StudioProbeProfile::Build(Samples);
    if(!TestTrue(TEXT("Published wing crossing produces a profile"),Profile.IsValid()))return false;
    auto Recaptured=Samples;++Recaptured.PresentationId;
    TestTrue(TEXT("Profile identifies its original presentation"),Profile->Matches(Samples));
    TestFalse(TEXT("Camera-only recapture requires refreshed profile provenance"),Profile->Matches(Recaptured));
    const auto Refreshed=StudioProbeProfile::Build(Recaptured);
    TestTrue(TEXT("Refreshed profile retains samples with current capture identity"),Refreshed&&Refreshed->Matches(Recaptured)&&
        Refreshed->Snapshot.PresentationId==Recaptured.PresentationId&&Refreshed->ValidSamples==Profile->ValidSamples&&
        Refreshed->Minimum==Profile->Minimum&&Refreshed->Maximum==Profile->Maximum);
    TestTrue(TEXT("Solid interior remains a gap between supported runs"),Profile->Segments.Num()>=2&&Profile->ValidSamples>0&&Profile->ValidSamples<R.Probe.Samples);
    for(const auto& Run:Profile->Segments)for(int32 I=0;I<Run.Num();++I)
    {
        TestTrue(TEXT("Only supplied finite samples enter a trace"),Samples.Samples[Run[I]].Value.IsSet());
        if(I)TestEqual(TEXT("Trace never bridges a missing source sample"),Run[I],Run[I-1]+1);
        const auto N=Profile->NormalizedPosition(Run[I]);TestTrue(TEXT("Chart coordinates are finite and bounded"),!N.ContainsNaN()&&N.X>=0&&N.X<=1&&N.Y>=0&&N.Y<=1);
    }
    R.Probe.A.Y=.1;R.Probe.B.Y=.1;const auto Empty=StudioProbeProfile::Build(StudioProbeSampling::Evaluate(R));
    TestTrue(TEXT("Off-plane line has no invented zero trace"),Empty&&Empty->Segments.IsEmpty()&&Empty->ValidSamples==0);
    R.Probe.A=FVector(-.3,0,.3);R.Probe.B=FVector(.7,0,.3);R.Probe.Samples=8;
    const auto Narrow=StudioProbeProfile::Build(StudioProbeSampling::Evaluate(R));
    TestTrue(TEXT("Narrow real pressure range has distinct axis labels"),Narrow&&Narrow->Minimum!=Narrow->Maximum&&Narrow->RangeLabel(false)!=Narrow->RangeLabel(true));
    R.Probe.A=R.Probe.B=FVector(.03250676393508911,0,.1194048523902893);R.Probe.Samples=2;
    const auto Constant=StudioProbeProfile::Build(StudioProbeSampling::Evaluate(R));
    TestTrue(TEXT("Zero-length line preserves a constant real value"),Constant&&Constant->ValidSamples==2&&Constant->Minimum==Constant->Maximum&&Constant->NormalizedPosition(0)==FVector2D(.5,.5));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbeCSV,"Studio.Inspection.ProbeCSVSnapshotAndPrecision",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioProbeCSV::RunTest(const FString&)
{
    FRecordedSolver Solver;auto R=ProfileRequest(Solver);R.Probe.Samples=3;
    R.Probe.A=FVector(.03250676393508911,0,.1194048523902893);R.Probe.B=R.Probe.A+FVector(0,.2,0);
    const auto Frozen=StudioProbeSampling::Evaluate(R);FString CSV,Error;
    if(!TestTrue(TEXT("Export original captured samples"),StudioProbeProfile::CSV(Frozen,CSV,Error)))return false;
    FCsvParser Parser(CSV);const auto& Rows=Parser.GetRows();
    if(!TestEqual(TEXT("Header and every requested sample exported"),Rows.Num(),4))return false;
    for(const auto& Row:Rows)if(!TestEqual(TEXT("Quoted CSV retains all columns"),Row.Num(),31))return false;
    TestEqual(TEXT("Name commas and quotes round-trip"),FString(Rows[1][4]),R.Probe.Name);
    TestEqual(TEXT("Known original source pressure remains exact"),FCString::Atod(Rows[1][30]),98699.78125);
    TestEqual(TEXT("Original physical time is retained"),FCString::Atod(Rows[1][14]),Frozen.Identity->Frame.Time);
    TestEqual(TEXT("Scene coordinate keeps double precision"),FCString::Atod(Rows[1][22]),R.Probe.A.X);
    TestEqual(TEXT("Original source axes/offset are explicit"),FCString::Atod(Rows[1][25]),R.Probe.A.X+.5);
    TestEqual(TEXT("Absent value is an empty cell"),FString(Rows[2][30]),FString());
    TestEqual(TEXT("Absent value records its cause"),FString(Rows[2][29]),FString(TEXT("outside_source_plane")));
    const FString Directory=FPaths::ProjectSavedDir()/TEXT("Automation/ProbeCSV");IFileManager::Get().MakeDirectory(*Directory,true);
    const FString Path=Directory/FGuid::NewGuid().ToString()+TEXT(".csv");
    FStudioProbeExportTask Writer;TestTrue(TEXT("Start captured export"),Writer.Start(Frozen,Path));
    TestFalse(TEXT("One export at a time bounds queued copies"),Writer.Start(Frozen,Path+TEXT(".second")));
    R.Field=Solver.CaptureViewField(40,TEXT("density"),false);R.DisplayedScalar=TEXT("density");R.PresentationId=100;
    R.Probe.A=FVector(-1,0,.3);const auto New=StudioProbeSampling::Evaluate(R);
    TestFalse(TEXT("Later field/frame no longer matches frozen result"),Frozen.Matches(R));
    TOptional<FStudioProbeExportResult> Finished;const double End=FPlatformTime::Seconds()+5;
    while(!Finished.IsSet()&&FPlatformTime::Seconds()<End){Finished=Writer.Poll();FPlatformProcess::Sleep(.001f);}
    if(!TestTrue(TEXT("Atomic export completed"),Finished.IsSet()&&Finished->bSuccess))return false;
    FString Written;TestTrue(TEXT("Read exported CSV independently"),FFileHelper::LoadFileToString(Written,*Path));
    TestEqual(TEXT("Source/frame changes cannot rewrite captured export"),Written,CSV);
    auto Invalid=Frozen;Invalid.Samples.Pop();FString Sentinel=TEXT("untouched");
    TestFalse(TEXT("Incomplete result cannot export"),StudioProbeProfile::CSV(Invalid,Sentinel,Error));
    TestEqual(TEXT("Rejected export leaves prior output intact"),Sentinel,FString(TEXT("untouched")));
    return true;
}
#endif
