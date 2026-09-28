#include "StudioMonitor.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformProcess.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FStudioHistoryLoadResult MonitorFixtureHistory()
{return StudioHistories::Load(FPaths::ProjectContentDir()/TEXT("Samples/NaluWind_NACA0021_Re270k_AoA30/history.json"));}
bool FinishMonitorModel(FStudioModel& Model)
{
    const double Deadline=FPlatformTime::Seconds()+10;
    do {Model.Tick(.001);if(!Model.IsMonitorLoading())return true;FPlatformProcess::Sleep(.001);}while(FPlatformTime::Seconds()<Deadline);
    return false;
}
FString MonitorTestJSON(const TSharedRef<FJsonObject>& O)
{FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMonitorPersistence,"Studio.Monitor.PersistenceMigrationAndValidation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioMonitorPersistence::RunTest(const FString&)
{
    const auto R=MonitorFixtureHistory();if(!TestTrue(TEXT("Published source available"),R.History.IsValid()))return false;
    FStudioProject P;P.Monitor=StudioMonitor::Defaults(*R.History);P.Monitor.bLogY=true;
    FString Error;TestTrue(TEXT("Exact original time range"),StudioMonitor::SetTimeWindow(*R.History,.567890123456789,2.987654321098765,P.Monitor,Error));
    FStudioProject Reopened;const auto Text=StudioProjectIO::Serialize(P);
    TestTrue(TEXT("Schema18 parses"),StudioProjectIO::Parse(Text,Reopened,Error));
    TestEqual(TEXT("All chart choices round trip exactly"),StudioProjectIO::Serialize(Reopened),Text);
    TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);
    O->SetNumberField(TEXT("version"),17);O->RemoveField(TEXT("monitor"));
    TestTrue(TEXT("Version17 migrates without a silently selected independent history"),StudioProjectIO::Parse(MonitorTestJSON(O.ToSharedRef()),Reopened,Error));
    TestTrue(TEXT("Migrated charts remain unselected"),Reopened.Monitor.HistoryId.IsEmpty());
    O->SetNumberField(TEXT("version"),18);
    TestFalse(TEXT("Missing schema18 monitor settings rejected"),StudioProjectIO::Parse(MonitorTestJSON(O.ToSharedRef()),Reopened,Error));
    for(int32 Case=0;Case<5;++Case)
    {
        auto Bad=P.Monitor;
        if(Case==0)Bad.MetadataSHA256=TEXT("bad");
        if(Case==1){const FString Duplicate=Bad.Series[0];Bad.Series.Add(Duplicate);}
        if(Case==2)Bad.TimeMaximum=Bad.TimeMinimum;
        if(Case==3)Bad.TimeMinimum=std::numeric_limits<double>::quiet_NaN();
        if(Case==4)Bad.HistoryId.Empty();
        TestFalse(TEXT("Malformed saved monitor settings refused"),StudioMonitor::Validate(Bad,Error));
    }
    auto Mixed=P.Monitor;Mixed.Series={TEXT("CL"),TEXT("Fpx")};
    TestFalse(TEXT("Different physical units cannot share one axis"),StudioMonitor::ValidateSource(Mixed,*R.History,Error));
    Mixed.Series={TEXT("not_supplied")};
    TestFalse(TEXT("Missing series stays unavailable"),StudioMonitor::ValidateSource(Mixed,*R.History,Error));
    Mixed=P.Monitor;const auto Before=MonitorTestJSON(StudioMonitor::ToJSON(Mixed));
    TestFalse(TEXT("Outside-source window refused"),StudioMonitor::SetTimeWindow(*R.History,0,4,Mixed,Error));
    TestEqual(TEXT("Rejected window is atomic"),MonitorTestJSON(StudioMonitor::ToJSON(Mixed)),Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMonitorSamples,"Studio.Monitor.OriginalSamplesExtremaAndAxes",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioMonitorSamples::RunTest(const FString&)
{
    const auto R=MonitorFixtureHistory();if(!TestTrue(TEXT("Published source available"),R.History.IsValid()))return false;
    const auto& H=*R.History;auto S=StudioMonitor::Defaults(H);
    const auto P=StudioMonitor::BuildPlot(H,S,57);
    TestTrue(TEXT("Plot is valid"),P.Error.IsEmpty());TestEqual(TEXT("Original physical time begins unchanged"),P.TimeMinimum,.4004);
    TestEqual(TEXT("Original physical time ends unchanged"),P.TimeMaximum,3.1868);
    for(const auto& T:P.Traces)
    {
        const auto& C=*H.FindColumn(T.Id);TestTrue(TEXT("Geometry has a pixel-bounded point count"),T.Samples.Num()<=57*4);
        int32 Previous=INDEX_NONE;for(int32 I:T.Samples){TestTrue(TEXT("Only ordered original samples"),I>Previous&&C.Values.IsValidIndex(I));Previous=I;}
        for(int32 B=0;B<57;++B)
        {
            int32 First=INDEX_NONE,Last=INDEX_NONE,Minimum=INDEX_NONE,Maximum=INDEX_NONE;
            for(int32 I=0;I<H.Times.Num();++I)
                if(FMath::Min(56,int32((H.Times[I]-.4004)/(3.1868-.4004)*57))==B)
                {
                    if(First==INDEX_NONE)First=Minimum=Maximum=I;
                    if(C.Values[I]<C.Values[Minimum])Minimum=I;if(C.Values[I]>C.Values[Maximum])Maximum=I;Last=I;
                }
            for(int32 I:{First,Minimum,Maximum,Last})if(I!=INDEX_NONE)
                TestTrue(TEXT("Every pixel bucket retains source endpoints and extrema"),T.Samples.Contains(I));
        }
    }
    TestEqual(TEXT("Sample lookup returns exact source index"),StudioMonitor::NearestSample(H.Times,H.Times[3456]),3456);
    TestEqual(TEXT("Early lookup clamps to original first sample"),StudioMonitor::NearestSample(H.Times,-100),0);
    TestEqual(TEXT("Late lookup clamps to original last sample"),StudioMonitor::NearestSample(H.Times,100),H.Times.Num()-1);
    const auto OriginalValues=H.FindColumn(TEXT("CL"))->Values;
    S.bLogY=true;const auto Log=StudioMonitor::BuildPlot(H,S,120);
    TestTrue(TEXT("Positive published coefficients support log axis"),Log.Error.IsEmpty());
    TestTrue(TEXT("Log axis retains all original values exactly"),H.FindColumn(TEXT("CL"))->Values==OriginalValues);
    FString Error;StudioMonitor::SetTimeWindow(H,H.Times[1000],H.Times[1001],S,Error);
    const auto Narrow=StudioMonitor::BuildPlot(H,S,120);TestEqual(TEXT("Zoom uses original sample bounds"),Narrow.FirstSample,1000);
    TestEqual(TEXT("Zoom includes exact end sample"),Narrow.LastSample,1001);
    S.Series.Reset();TestTrue(TEXT("Empty selection is explicit"),StudioMonitor::BuildPlot(H,S,100).Error.Contains(TEXT("Select a series")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMonitorLogGaps,"Studio.Monitor.LogGapsRemainBounded",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioMonitorLogGaps::RunTest(const FString&)
{
    // Structural chart vectors only: these are not CFD samples or installed fixtures.
    FStudioHistory H;H.Id=TEXT("axis-unit-test");H.MetadataSHA256=FString::ChrN(64,'a');H.TimeUnit=TEXT("s");
    FStudioHistoryColumn C;C.Id=TEXT("value");C.Label=TEXT("Structural axis test");C.Unit=TEXT("1");
    for(int32 I=0;I<10000;++I){H.Times.Add(double(I));C.Values.Add(I%2?1.+I:0.);}
    H.Columns.Add(C);auto S=StudioMonitor::Defaults(H);S.bLogY=true;
    const auto Plot=StudioMonitor::BuildPlot(H,S,10);
    TestTrue(TEXT("Many log gaps do not allocate unbounded geometry"),Plot.Traces[0].Samples.Num()<=80);
    TestEqual(TEXT("All omitted zeros are disclosed"),Plot.Traces[0].OmittedNonPositive,5000);
    bool Previous=false;for(int32 I:Plot.Traces[0].Samples)
    {
        if(I==INDEX_NONE){Previous=false;continue;}
        TestFalse(TEXT("Never join across a missing log sample"),Previous);Previous=true;
        TestTrue(TEXT("Log point refers to a positive original value"),C.Values[I]>0);
    }
    H.Columns[0].Values.Init(0.,H.Times.Num());
    TestTrue(TEXT("No positive samples reports unavailable axis"),StudioMonitor::BuildPlot(H,S,100).Error.Contains(TEXT("No positive")));
    H.Columns[0].Values.Init(1.,H.Times.Num());const auto Constant=StudioMonitor::BuildPlot(H,S,100);
    TestTrue(TEXT("Constant data has finite visible range"),Constant.ValueMinimum<Constant.ValueMaximum&&FMath::IsFinite(Constant.ValueMaximum));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMonitorModel,"Studio.Monitor.LoadCancellationPersistenceAndIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioMonitorModel::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectSavedDir()/TEXT("Automation/MonitorModel")/FGuid::NewGuid().ToString();FStudioModel M(Dir);
    const FString Id=TEXT("NaluWind_NACA0021_Re270k_AoA30");
    const FString Case=StudioCaseIO::Serialize(M.Project.Draft);const auto Frame=M.SelectedFrame;const auto Revision=M.RenderIntentRevision;
    TestTrue(TEXT("Published source request accepted"),M.RequestMonitorHistory(Id));
    TestFalse(TEXT("Only one history worker can be outstanding"),M.RequestMonitorHistory(Id));
    M.CancelMonitorHistory();TestTrue(TEXT("Cancellation completes"),FinishMonitorModel(M));
    TestFalse(TEXT("Cancelled candidate is never published"),M.MonitorHistory().IsValid());
    TestTrue(TEXT("Cancel preserves empty saved choice"),M.Project.Monitor.HistoryId.IsEmpty());
    TestTrue(TEXT("Explicit retry accepted"),M.RequestMonitorHistory(Id));TestTrue(TEXT("Verified load completes"),FinishMonitorModel(M));
    if(!TestTrue(TEXT("Verified immutable history published"),M.MonitorHistory().IsValid()))return false;
    TestFalse(TEXT("Independent run is never joined to SU2 fields"),M.MonitorHistory()->FieldRecordingId.IsSet());
    TestEqual(TEXT("History selection preserves case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    TestEqual(TEXT("History selection preserves frame"),M.SelectedFrame,Frame);TestEqual(TEXT("History never invalidates field geometry"),M.RenderIntentRevision,Revision);
    auto Settings=M.Project.Monitor;Settings.bLogY=true;FString Error;
    StudioMonitor::SetTimeWindow(*M.MonitorHistory(),.5,2.5,Settings,Error);TestTrue(TEXT("Chart settings applied"),M.UpdateMonitorSettings(Settings));
    TestTrue(TEXT("Save chart configuration"),M.SaveProject(Dir/TEXT("chart.lbms")));
    const FString Saved=MonitorTestJSON(StudioMonitor::ToJSON(Settings));
    FStudioProject P;TestTrue(TEXT("Read persisted chart"),StudioProjectIO::Load(Dir/TEXT("chart.lbms"),P,Error));
    FStudioModel Reopened(Dir/TEXT("reader"));Reopened.Project=P;
    TestTrue(TEXT("Reopen loads pinned history"),FinishMonitorModel(Reopened));TestTrue(TEXT("Reopened history available"),Reopened.MonitorHistory().IsValid());
    TestEqual(TEXT("Reopen preserves choices exactly"),MonitorTestJSON(StudioMonitor::ToJSON(Reopened.Project.Monitor)),Saved);
    TestTrue(TEXT("Harness selected"),M.SetControlHarness(true));TestTrue(TEXT("Frozen control run submitted"),M.Control(EStudioJobCommand::Submit));
    const auto Frozen=StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration());
    Settings.bLogY=false;M.UpdateMonitorSettings(Settings);
    TestEqual(TEXT("Chart edits preserve frozen run"),StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration()),Frozen);
    TestTrue(TEXT("Request starts before project replacement"),Reopened.RequestMonitorHistory(Id));
    Reopened.Project=FStudioProject();TestTrue(TEXT("Stale worker reaped"),FinishMonitorModel(Reopened));
    TestFalse(TEXT("Old project cannot republish its chart"),Reopened.MonitorHistory().IsValid());
    TestTrue(TEXT("New project keeps empty monitor choice"),Reopened.Project.Monitor.HistoryId.IsEmpty());
    P.Monitor.MetadataSHA256=FString::ChrN(64,'0');Reopened.Project=P;Reopened.Tick(.001);
    TestFalse(TEXT("Different source interpretation rejected"),Reopened.MonitorHistory().IsValid());
    TestTrue(TEXT("Unavailable saved source has recovery message"),Reopened.MonitorNotice.Contains(TEXT("unavailable")));
    Reopened.ClearMonitorHistory();TestTrue(TEXT("Remove clears persisted source"),Reopened.Project.Monitor.HistoryId.IsEmpty());
    return true;
}
#endif
