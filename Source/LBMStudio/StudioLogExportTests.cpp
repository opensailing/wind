#include "StudioLogExport.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLogExportTest,"Studio.Log.FrozenCSVExportAndBounds",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioLogExportTest::RunTest(const FString&)
{
    FStudioLogJournal Journal;
    Journal.Append(TEXT("α, \"quoted\"\r\nsecond line"),EStudioLogSeverity::Warning,EStudioLogSource::Application,{}, {},TEXT("source, one"),FDateTime(2026,9,28,14,2,3));
    auto Entries=Journal.Snapshot();FString Text,Error;
    TestTrue(TEXT("CSV accepts a bounded snapshot"),StudioLogExport::CSV(Entries,Text,Error));
    TestTrue(TEXT("UTC observation time explicitly named"),Text.StartsWith(TEXT("observation_sequence,observed_at_utc,")));
    TestTrue(TEXT("Quoted Unicode and line breaks preserved"),Text.Contains(TEXT("\"α, \"\"quoted\"\"\r\nsecond line\"\r\n")));
    TestTrue(TEXT("Unknown run not fabricated"),Text.Contains(TEXT("\"Application\",\"\",\"\",\"source, one\",false,")));
    TestTrue(TEXT("UTC timestamp exact"),Text.Contains(TEXT("2026-09-28T14:02:03.000Z")));
    const FString Expected=Text;
    auto Invalid=Entries;Invalid.Add(Entries[0]);
    TestFalse(TEXT("Duplicate observations refused"),StudioLogExport::CSV(Invalid,Text,Error));
    TestEqual(TEXT("Rejected export preserves destination buffer"),Text,Expected);
    Invalid=Entries;Invalid[0].Message=FString::ChrN(FStudioLogJournal::MessageLimit+1,TEXT('x'));
    TestFalse(TEXT("Unbounded message rejected before task"),StudioLogExport::CSV(Invalid,Text,Error));
    TestFalse(TEXT("Empty export reports no output"),StudioLogExport::CSV({},Text,Error));
    const FString Directory=FPaths::ProjectSavedDir()/TEXT("Automation/LogExport")/FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*Directory,true);const FString Path=Directory/TEXT("visible.csv");
    FStudioLogExportTask Task;TestTrue(TEXT("Frozen write starts"),Task.Start(Entries,Path));
    TestFalse(TEXT("Concurrent writes refused"),Task.Start(Entries,Path));Entries[0].Message=TEXT("Changed after submit");
    TOptional<FStudioLogExportResult> Result;const double Deadline=FPlatformTime::Seconds()+10;
    while(!Result&&FPlatformTime::Seconds()<Deadline){Result=Task.Poll();if(!Result)FPlatformProcess::Sleep(.001);}
    if(!TestTrue(TEXT("Export completes"),Result.IsSet()))return false;
    TestTrue(TEXT("Atomic write succeeds: ")+Result->Error,Result->bSuccess);
    TestEqual(TEXT("Visible count reported"),Result->Entries,1);
    FString Written;TestTrue(TEXT("File readable"),FFileHelper::LoadFileToString(Written,*Path));
    TestEqual(TEXT("Written CSV is the frozen snapshot"),Written,Expected);
    return true;
}
#endif
