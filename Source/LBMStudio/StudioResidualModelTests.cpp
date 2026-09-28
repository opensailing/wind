#include "StudioModel.h"
#include "StudioResiduals.h"
#include "StudioAssetPaths.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioResidualModelTestPrivate
{
// Minimal format fixture for state/path tests only. Scientific acceptance uses
// the complete original published log in ScientificAcceptance.Residuals.
const FString Text=TEXT("OpenFOAM: structural model fixture\nTime = 1\n")
    TEXT("GAMG: Solving for p, Initial residual = 1, Final residual = 0.1, No Iterations 2\n")
    TEXT("Time = 2\nGAMG: Solving for p, Initial residual = 0.2, Final residual = 0.01, No Iterations 1\nEnd\n");
struct FFixture
{
    FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/ResidualModel")/FGuid::NewGuid().ToString());
    FFixture(){IFileManager::Get().MakeDirectory(*Root,true);Write(Path());}
    ~FFixture(){IFileManager::Get().DeleteDirectory(*Root,false,true);}
    FString Path() const{return Root/TEXT("source.log");}
    bool Write(const FString& Path,const FString& Value=Text)
    {return FFileHelper::SaveStringToFile(Value,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);}
};
bool Finish(FStudioModel& M)
{
    const double End=FPlatformTime::Seconds()+15;
    do{M.Tick(.001);if(!M.IsResidualLoading()&&!M.IsMonitorLoading()&&!M.IsProjectOpenPending())return true;FPlatformProcess::Sleep(.001);}while(FPlatformTime::Seconds()<End);
    return false;
}
FString JSON(const TSharedRef<FJsonObject>& O)
{FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
FString SavedResidual(const FStudioModel& M){return JSON(StudioResidualSettings::ToJSON(M.Project.Residual));}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResidualSchema,"Studio.ResidualModel.Schema19MigrationAndValidation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioResidualSchema::RunTest(const FString&)
{
    using namespace StudioResidualModelTestPrivate;
    FFixture F;const auto H=StudioResiduals::Load(F.Path());if(!TestTrue(TEXT("Structural reader available"),H.History.IsValid()))return false;
    FStudioProject P;P.Residual.Path=F.Path();P.Residual.Chart=StudioMonitor::Defaults(*H.History);
    const auto Encoded=StudioProjectIO::Serialize(P);FStudioProject Q;FString Error;
    TestTrue(TEXT("Schema19 validates"),StudioProjectIO::Parse(Encoded,Q,Error));
    TestEqual(TEXT("Exact source/interpretation/settings roundtrip"),StudioProjectIO::Serialize(Q),Encoded);
    TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Encoded),O);
    O->SetNumberField(TEXT("version"),18);O->RemoveField(TEXT("residual"));
    TestTrue(TEXT("Version18 migrates"),StudioProjectIO::Parse(JSON(O.ToSharedRef()),Q,Error));
    TestTrue(TEXT("Migration never chooses a residual source"),Q.Residual.Path.IsEmpty()&&Q.Residual.Chart.HistoryId.IsEmpty());
    O->SetNumberField(TEXT("version"),19);
    TestFalse(TEXT("Current version requires explicit residual state"),StudioProjectIO::Parse(JSON(O.ToSharedRef()),Q,Error));
    for(int32 I=0;I<6;++I)
    {
        auto Bad=P.Residual;
        if(I==0)Bad.Path.Empty();if(I==1)Bad.Path=TEXT("https://example.invalid/log");if(I==2)Bad.Path=TEXT("bad\npath");
        if(I==3)Bad.Chart.HistoryId=TEXT("unverified-source");if(I==4)Bad.Chart.MetadataSHA256=TEXT("invalid");
        if(I==5){const FString Duplicate=Bad.Chart.Series[0];Bad.Chart.Series.Add(Duplicate);}
        TestFalse(TEXT("Invalid saved residual reference rejected"),StudioResidualSettings::Validate(Bad,Error));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResidualPaths,"Studio.ResidualModel.PortableSaveDuplicateAndRecovery",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioResidualPaths::RunTest(const FString&)
{
    using namespace StudioResidualModelTestPrivate;
    FFixture F;FStudioModel M(F.Root/TEXT("session"));
    TestTrue(TEXT("Residual read requested"),M.RequestResidualLog(F.Path()));TestTrue(TEXT("Reader settles"),Finish(M));
    if(!TestTrue(TEXT("Residual source accepted"),M.ResidualHistory().IsValid()))return false;
    TestTrue(TEXT("Force source requested independently"),M.RequestMonitorHistory(TEXT("NaluWind_NACA0021_Re270k_AoA30")));
    TestTrue(TEXT("Force reader settles"),Finish(M));
    const FString Force=JSON(StudioMonitor::ToJSON(M.Project.Monitor));const FString Residual=SavedResidual(M);
    const FString A=F.Root/TEXT("first/case.lbms"),B=F.Root/TEXT("second/case.lbms"),Copy=F.Root/TEXT("copy/case.lbms");
    TestTrue(TEXT("Initial project saves"),M.SaveProject(A));TestTrue(TEXT("Save As keeps exact source"),M.SaveProject(B));
    TestEqual(TEXT("Live source stays absolute after Save As"),SavedResidual(M),Residual);
    FStudioProject P;FString Error,Stored;TestTrue(TEXT("Saved case loads"),StudioProjectIO::Load(B,P,Error));
    TestEqual(TEXT("Source resolves relative to new owner"),P.Residual.Path,F.Path());
    FFileHelper::LoadFileToString(Stored,*B);TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Stored),O);
    TestEqual(TEXT("Stored source is relative"),O->GetObjectField(TEXT("residual"))->GetStringField(TEXT("path")),FString(TEXT("../source.log")));
    const FGuid Original=M.Project.Id;TestTrue(TEXT("Independent duplicate saves"),M.DuplicateProject(Copy,TEXT("copy")));
    TestTrue(TEXT("Duplicate reads saved histories"),Finish(M));TestNotEqual(TEXT("Duplicate has new project ID"),M.Project.Id,Original);
    TestEqual(TEXT("Duplicate keeps independent force settings"),JSON(StudioMonitor::ToJSON(M.Project.Monitor)),Force);
    TestEqual(TEXT("Duplicate keeps residual settings"),SavedResidual(M),Residual);
    M.Project.Name=TEXT("recovery changes");M.WriteRecovery();
    const FString Recovery=F.Root/TEXT("session/Recovery/StudioRecovery.lbms");
    TestTrue(TEXT("Recovery document valid"),StudioProjectIO::Load(Recovery,P,Error));
    TestEqual(TEXT("Recovery source path remains absolute"),P.Residual.Path,F.Path());
    FStudioModel Restored(F.Root/TEXT("restored-session"));Restored.PendingRecovery=Recovery;
    TestTrue(TEXT("Recovery restores"),Restored.RestoreRecovery());TestTrue(TEXT("Recovered histories verify"),Finish(Restored));
    TestTrue(TEXT("Recovered residual history available"),Restored.ResidualHistory().IsValid());
    TestTrue(TEXT("Recovered force history available"),Restored.MonitorHistory().IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResidualTransactions,"Studio.ResidualModel.FailureCancelLocateAndIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioResidualTransactions::RunTest(const FString&)
{
    using namespace StudioResidualModelTestPrivate;
    FFixture F;FStudioModel M(F.Root/TEXT("session"));
    TestTrue(TEXT("Force history requested"),M.RequestMonitorHistory(TEXT("NaluWind_NACA0021_Re270k_AoA30")));Finish(M);
    const auto Force=M.MonitorHistory();const auto Camera=M.Project.Camera;const auto Source=M.Solver;const int32 Frame=M.SelectedFrame;
    const FString Draft=StudioCaseIO::Serialize(M.Project.Draft),Forces=JSON(StudioMonitor::ToJSON(M.Project.Monitor));
    TestTrue(TEXT("Residual import starts"),M.RequestResidualLog(F.Path()));TestTrue(TEXT("Import completes"),Finish(M));
    if(!TestTrue(TEXT("Imported history available"),M.ResidualHistory().IsValid()))return false;
    const auto Original=M.ResidualHistory();auto Settings=M.Project.Residual.Chart;Settings.Series={TEXT("p.FinalLast")};Settings.bLogY=false;
    TestTrue(TEXT("Residual chart changes"),M.UpdateResidualSettings(Settings));const FString Saved=SavedResidual(M);
    TestTrue(TEXT("Missing source request starts"),M.RequestResidualLog(F.Root/TEXT("missing.log")));
    TestTrue(TEXT("Prior history retained during load"),M.ResidualHistory()==Original);Finish(M);
    TestEqual(TEXT("Failure preserves saved settings"),SavedResidual(M),Saved);TestTrue(TEXT("Failure preserves history"),M.ResidualHistory()==Original);
    TestTrue(TEXT("Next candidate starts"),M.RequestResidualLog(F.Path()));M.CancelResidualLog();Finish(M);
    TestEqual(TEXT("Cancellation preserves settings"),SavedResidual(M),Saved);TestTrue(TEXT("Cancellation preserved history"),M.ResidualHistory()==Original);
    const FString Located=F.Root/TEXT("located.log");F.Write(Located,Text+TEXT("\n"));
    TestTrue(TEXT("Locate changed source starts"),M.RequestResidualLog(Located,true));Finish(M);
    TestEqual(TEXT("Changed bytes cannot relink"),SavedResidual(M),Saved);
    TestTrue(TEXT("Exact-copy guidance retained"),M.ResidualNotice.Contains(TEXT("exact copy")));
    F.Write(Located);TestTrue(TEXT("Locate exact source starts"),M.RequestResidualLog(Located,true));Finish(M);
    TestEqual(TEXT("Exact source path updated"),M.Project.Residual.Path,Located);
    TestEqual(TEXT("Locate preserves user chart choices"),JSON(StudioMonitor::ToJSON(M.Project.Residual.Chart)),JSON(StudioMonitor::ToJSON(Settings)));
    TestTrue(TEXT("Force history pointer unchanged"),M.MonitorHistory()==Force);TestEqual(TEXT("Force settings unchanged"),JSON(StudioMonitor::ToJSON(M.Project.Monitor)),Forces);
    TestEqual(TEXT("Case unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Draft);TestTrue(TEXT("Camera unchanged"),StudioView::CameraEquals(M.Project.Camera,Camera));
    TestTrue(TEXT("Recording object unchanged"),M.Solver==Source);TestEqual(TEXT("Recording frame unchanged"),M.SelectedFrame,Frame);
    TestTrue(TEXT("Harness selectable"),M.SetControlHarness(true));TestTrue(TEXT("Frozen job submits"),M.Control(EStudioJobCommand::Submit));
    const auto Frozen=StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration());
    TestTrue(TEXT("Import independent of active job"),M.RequestResidualLog(F.Path()));Finish(M);
    TestEqual(TEXT("Residual import preserves frozen run"),StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration()),Frozen);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResidualReopen,"Studio.ResidualModel.ReopenMissingAndObsoleteReads",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioResidualReopen::RunTest(const FString&)
{
    using namespace StudioResidualModelTestPrivate;
    FFixture F;FStudioModel M(F.Root/TEXT("session"));M.RequestResidualLog(F.Path());Finish(M);
    if(!TestTrue(TEXT("Initial residual available"),M.ResidualHistory().IsValid()))return false;
    const FString File=F.Root/TEXT("case.lbms");TestTrue(TEXT("Save before reopen"),M.SaveProject(File));
    const FString Saved=SavedResidual(M);
    auto Changed=M.Project.Residual.Chart;Changed.Series={TEXT("p.FinalLast")};M.UpdateResidualSettings(Changed);
    TestTrue(TEXT("Candidate starts before same-ID reopen"),M.RequestResidualLog(F.Path()));
    TestTrue(TEXT("Same project reopen starts"),M.RequestProjectOpen(File));TestTrue(TEXT("Same-ID reopen settles"),Finish(M));
    TestEqual(TEXT("Reopen restores saved chart, not late candidate"),SavedResidual(M),Saved);
    TestTrue(TEXT("Restored residual history verified"),M.ResidualHistory().IsValid());
    TestTrue(TEXT("Next candidate starts"),M.RequestResidualLog(F.Path()));M.Project=FStudioProject();Finish(M);
    TestFalse(TEXT("New project cannot receive stale source"),M.ResidualHistory().IsValid());TestTrue(TEXT("New project residual stays empty"),M.Project.Residual.Path.IsEmpty());
    IFileManager::Get().Delete(*F.Path());TestTrue(TEXT("Missing log does not prevent project reopen"),M.RequestProjectOpen(File));Finish(M);
    TestFalse(TEXT("Missing source unavailable"),M.ResidualHistory().IsValid());TestEqual(TEXT("Missing source reference retained for repair"),SavedResidual(M),Saved);
    TestTrue(TEXT("Missing source has recovery notice"),M.ResidualNotice.Contains(TEXT("Locate")));
    F.Write(F.Path());TestTrue(TEXT("Explicit locate retries exact source"),M.RequestResidualLog(F.Path(),true));Finish(M);
    TestTrue(TEXT("Repaired history published"),M.ResidualHistory().IsValid());
    M.ClearResidualLog();TestTrue(TEXT("Remove clears reference"),M.Project.Residual.Path.IsEmpty());
    TestTrue(TEXT("Remove never deletes source"),IFileManager::Get().FileExists(*F.Path()));
    return true;
}
#endif
