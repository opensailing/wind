#include "StudioResiduals.h"
#include "StudioMonitor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioResidualTestPrivate
{
// Structural snippets only, never installed CFD examples or scientific acceptance.
const FString Snippet=TEXT("OpenFOAM: structural parser fixture\nTime = 1.25\nPIMPLE: iteration 1\n")
    TEXT("smoothSolver: Solving for Ux, Initial residual = 1.23456789e-3, Final residual = 2e-6, No Iterations 2\n")
    TEXT("GAMG: Solving for p, Initial residual = 3e-2, Final residual = 4e-7, No Iterations 8\n")
    TEXT("PIMPLE: iteration 2\n")
    TEXT("smoothSolver: Solving for Ux, Initial residual = 2e-6, Final residual = 0, No Iterations 0\n")
    TEXT("GAMG: Solving for p, Initial residual = 7e-4, Final residual = 9e-8, No Iterations 3\n")
    TEXT("Time = 1.375\n")
    TEXT("smoothSolver: Solving for Ux, Initial residual = 5e-5, Final residual = 8e-6, No Iterations 1\n")
    TEXT("GAMG: Solving for p, Initial residual = 6e-4, Final residual = 1e-7, No Iterations 4\nEnd\n");
struct FFixture
{
    FString Folder=FPaths::ProjectSavedDir()/TEXT("Automation/ResidualParser")/FGuid::NewGuid().ToString();
    FFixture(){IFileManager::Get().MakeDirectory(*Folder,true);}
    ~FFixture(){IFileManager::Get().DeleteDirectory(*Folder,false,true);}
    FString Path() const{return Folder/TEXT("structural.log");}
    bool Write(const FString& Text){return FFileHelper::SaveStringToFile(Text,*Path(),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);}
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResidualSelection,"Studio.Residuals.SelectionIdentityAndOriginalLines",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioResidualSelection::RunTest(const FString&)
{
    using namespace StudioResidualTestPrivate;
    FFixture Fixture;if(!TestTrue(TEXT("Write structural input"),Fixture.Write(Snippet)))return false;
    const auto R=StudioResiduals::Load(Fixture.Path());
    if(!TestTrue(*R.Error,R.History.IsValid()))return false;
    const auto& H=*R.History;
    TestTrue(TEXT("Residual kind explicit"),H.bResiduals);TestFalse(TEXT("No field association"),H.FieldRecordingId.IsSet());
    TestEqual(TEXT("All solves counted"),H.SourceRecordCount,int64(6));
    TestEqual(TEXT("Original time blocks"),H.Times,TArray<double>({1.25,1.375}));
    TestEqual(TEXT("Time source lines"),H.TimeSourceLines,TArray<int32>({2,9}));
    TestEqual(TEXT("Units are not guessed"),H.TimeUnit,FString(TEXT("source units")));
    const auto* A=H.FindColumn(TEXT("Ux.InitialFirst"));const auto* B=H.FindColumn(TEXT("Ux.FinalLast"));
    if(!TestNotNull(TEXT("First initial"),A)||!TestNotNull(TEXT("Last final"),B))return false;
    TestEqual(TEXT("Original initial values"),A->Values,TArray<double>({1.23456789e-3,5e-5}));
    TestEqual(TEXT("Zero retained exactly"),B->Values,TArray<double>({0,8e-6}));
    TestEqual(TEXT("Initial source lines"),A->SourceLines,TArray<int32>({4,10}));
    TestEqual(TEXT("Final source lines"),B->SourceLines,TArray<int32>({7,10}));
    auto S=StudioMonitor::Defaults(H);
    TestTrue(TEXT("Residual chart defaults logarithmic"),S.bLogY);
    TestEqual(TEXT("Default series explicitly first initial"),S.Series,TArray<FString>({TEXT("Ux.InitialFirst"),TEXT("p.InitialFirst")}));
    S.Series={B->Id};const auto P=StudioMonitor::BuildPlot(H,S,100);
    TestTrue(TEXT("Actual log plot supported"),P.Error.IsEmpty());
    TestEqual(TEXT("Zero remains reported as omitted from log axis"),P.Traces[0].OmittedNonPositive,1);
    TestTrue(TEXT("Exact saved interpretation reopens"),StudioResiduals::Load(Fixture.Path(),{},H.MetadataSHA256).History.IsValid());
    FFixture Relocated;Relocated.Write(Snippet);const auto Other=StudioResiduals::Load(Relocated.Path());
    TestEqual(TEXT("Identity independent of location"),Other.History->MetadataSHA256,H.MetadataSHA256);
    Fixture.Write(Snippet+TEXT("\n"));
    TestFalse(TEXT("Changed source bytes fail exact reopen"),StudioResiduals::Load(Fixture.Path(),{},H.MetadataSHA256).History.IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResidualRejection,"Studio.Residuals.RejectIncompleteOrInventedSamples",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioResidualRejection::RunTest(const FString&)
{
    using namespace StudioResidualTestPrivate;
    TArray<FString> Invalid;
    for(const TCHAR* Value:{TEXT("nan"),TEXT("-1"),TEXT("1e999"),TEXT("1e-999"),TEXT("1e"),TEXT("1.0junk")})
        Invalid.Add(Snippet.Replace(TEXT("1.23456789e-3"),Value));
    Invalid.Append({Snippet.Replace(TEXT("Time = 1.375"),TEXT("Time = 1.25")),
        Snippet.Replace(TEXT("Time = 1.375"),TEXT("Time = NaN")),Snippet.Replace(TEXT("End\n"),TEXT("")),
        Snippet+TEXT("Time = 2\n"),Snippet+Snippet,Snippet.Replace(TEXT("OpenFOAM:"),TEXT("Unknown:")),
        Snippet.Replace(TEXT("No Iterations 2"),TEXT("No Iterations -1")),
        Snippet.Replace(TEXT("Time = 1.375"),TEXT("Time = 1.375\nTime = 1.5")),
        Snippet.Replace(TEXT("GAMG: Solving for p, Initial residual = 6e-4, Final residual = 1e-7, No Iterations 4\n"),TEXT("")),
        Snippet.Replace(TEXT("GAMG: Solving for p, Initial residual = 6e-4"),TEXT("GAMG: Solving for q, Initial residual = 6e-4")),
        Snippet.Replace(TEXT("Initial residual = 6e-4"),TEXT("Initial residual unknown 6e-4"))});
    FFixture Fixture;
    for(int32 I=0;I<Invalid.Num();++I)
    {
        Fixture.Write(Invalid[I]);const auto R=StudioResiduals::Load(Fixture.Path());
        TestFalse(*FString::Printf(TEXT("Malformed source %d publishes no history"),I),R.History.IsValid());
        TestFalse(TEXT("Actionable failure returned"),R.Error.IsEmpty());
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResidualBounds,"Studio.Residuals.CancellationAndReadBudgets",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioResidualBounds::RunTest(const FString&)
{
    using namespace StudioResidualTestPrivate;
    FFixture Fixture;Fixture.Write(Snippet);
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Cancelled reader publishes nothing"),StudioResiduals::Load(Fixture.Path(),Cancel).History.IsValid());
    TestFalse(TEXT("Absent file rejects"),StudioResiduals::Load(Fixture.Folder/TEXT("missing.log")).History.IsValid());
    Fixture.Write(FString::ChrN(32769,'x')+TEXT("\n")+Snippet);
    TestFalse(TEXT("Oversized line rejected"),StudioResiduals::Load(Fixture.Path()).History.IsValid());
    FString Many=TEXT("OpenFOAM: structural budget fixture\nTime = 1\n");
    for(int32 I=0;I<4097;++I)Many+=TEXT("GAMG: Solving for p, Initial residual = 1, Final residual = 0, No Iterations 1\n");
    Fixture.Write(Many+TEXT("End\n"));
    TestTrue(TEXT("Per-time record budget rejects explicitly"),StudioResiduals::Load(Fixture.Path()).Error.Contains(TEXT("budget")));
    Many=TEXT("OpenFOAM: structural field-budget fixture\nTime = 1\n");
    for(int32 I=0;I<32;++I)Many+=FString::Printf(TEXT("GAMG: Solving for field%d, Initial residual = 1, Final residual = 0, No Iterations 1\n"),I);
    Fixture.Write(Many+TEXT("End\n"));
    TestTrue(TEXT("Field allocation budget rejects explicitly"),StudioResiduals::Load(Fixture.Path()).Error.Contains(TEXT("31")));
    return true;
}

// Explicit opt-in acceptance uses the actual external author's source and writes
// selected original values/lines for an independent whole-source comparison.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPublishedResiduals,"ScientificAcceptance.Residuals.PublishedFullLog",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioPublishedResiduals::RunTest(const FString&)
{
    FString Path;
    if(!TestTrue(TEXT("Explicit original log path supplied"),FParse::Value(FCommandLine::Get(),TEXT("StudioResidualLog="),Path)))return false;
    const auto R=StudioResiduals::Load(Path);
    if(!TestTrue(*R.Error,R.History.IsValid()))return false;
    const auto& H=*R.History;
    TestEqual(TEXT("Original author's full source hash"),H.SourceSHA256,FString(TEXT("6ff51d5a70736df30adfff60eaa9ae96f1a4a9d60d565483fbe744571d5408e2")));
    TestEqual(TEXT("Complete source times"),H.Times.Num(),20000);
    TestEqual(TEXT("All original linear solves consumed"),H.SourceRecordCount,int64(291444));
    TestEqual(TEXT("All six selected residual series"),H.Columns.Num(),6);
    TestEqual(TEXT("Original time extent begins"),H.Times[0],.0005);TestEqual(TEXT("Original time extent ends"),H.Times.Last(),10.);
    TestFalse(TEXT("Does not attach cylinder history to wing fields"),H.FieldRecordingId.IsSet());
    FString Audit=TEXT("Time,TimeSourceLine,Series,SourceLine,Value\n");
    for(int32 I=0;I<H.Times.Num();++I)for(const auto& C:H.Columns)
        Audit+=FString::Printf(TEXT("%.17g,%d,%s,%d,%.17g\n"),H.Times[I],H.TimeSourceLines[I],*C.Id,C.SourceLines[I],C.Values[I]);
    const FString Folder=FPaths::ProjectSavedDir()/TEXT("Automation/ResidualAudit");IFileManager::Get().MakeDirectory(*Folder,true);
    TestTrue(TEXT("Write independent audit input"),FFileHelper::SaveStringToFile(Audit,*(Folder/TEXT("native-selected.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    const auto S=StudioMonitor::Defaults(H);const auto P=StudioMonitor::BuildPlot(H,S,1280);
    TestTrue(TEXT("Original residuals produce a logarithmic chart"),P.Error.IsEmpty()&&P.bLogY&&P.Traces.Num()==3);
    for(const auto& T:P.Traces)TestTrue(TEXT("Retained original samples are bounded by chart width"),T.Samples.Num()<=8*1280);
    return true;
}
#endif
