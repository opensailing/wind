#include "StudioMonitorExport.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMonitorCSVTest,"Studio.Monitor.OriginalCSVExportAndFrozenSelection",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioMonitorCSVTest::RunTest(const FString&)
{
    const auto R=StudioHistories::Load(FPaths::ProjectContentDir()/TEXT("Samples/NaluWind_NACA0021_Re270k_AoA30/history.json"));
    if(!TestTrue(TEXT("Published force history available"),R.History.IsValid()))return false;
    const auto& H=*R.History;auto S=StudioMonitor::Defaults(H);S.bLogY=true;FString CSV,Error;int32 Count=0;
    TestTrue(TEXT("Source CSV exported"),StudioMonitorExport::CSV(H,S,CSV,Count,Error));TestEqual(TEXT("All original rows exported despite display reduction"),Count,6967);
    TestTrue(TEXT("Interpretation hash retained"),CSV.Contains(H.MetadataSHA256));TestTrue(TEXT("Normalization expression retained"),CSV.Contains(H.FindColumn(TEXT("CL"))->Expression));
    TArray<FString> Lines;CSV.ParseIntoArrayLines(Lines,false);int32 Row=0;
    for(const auto& Line:Lines)
    {
        if(Line.StartsWith(TEXT("#"))||Line.StartsWith(TEXT("source_sample"))||Line.IsEmpty())continue;
        TArray<FString> Cells;Line.ParseIntoArray(Cells,TEXT(","),false);
        if(!TestEqual(TEXT("CSV has sample/time/two coefficient columns"),Cells.Num(),4))return false;
        TestEqual(TEXT("Original row ordinal"),FCString::Atoi(*Cells[0]),Row);
        TestEqual(TEXT("Original time round trips"),FCString::Atod(*Cells[1]),H.Times[Row]);
        TestEqual(TEXT("Original lift round trips"),FCString::Atod(*Cells[2]),H.FindColumn(TEXT("CL"))->Values[Row]);
        TestEqual(TEXT("Original drag round trips"),FCString::Atod(*Cells[3]),H.FindColumn(TEXT("CD"))->Values[Row]);++Row;
    }
    TestEqual(TEXT("Every source row independently compared"),Row,6967);
    StudioMonitor::SetTimeWindow(H,H.Times[100],H.Times[104],S,Error);
    const FString Path=FPaths::ProjectSavedDir()/TEXT("Automation/MonitorExport")/FGuid::NewGuid().ToString()/TEXT("selected.csv");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    FStudioMonitorExportTask Task;TestTrue(TEXT("Frozen export starts"),Task.Start(R.History,S,Path));
    TestFalse(TEXT("One export at a time"),Task.Start(R.History,S,Path));
    S.Series.Reset();S.bManualTime=false;
    TOptional<FStudioMonitorExportResult> Result;const double End=FPlatformTime::Seconds()+10;
    while(!Result.IsSet()&&FPlatformTime::Seconds()<End){Result=Task.Poll();if(!Result.IsSet())FPlatformProcess::Sleep(.001);}
    if(!TestTrue(TEXT("Worker export finishes"),Result.IsSet()))return false;
    TestTrue(TEXT("Atomic file write succeeded: ")+Result->Error,Result->bSuccess);TestEqual(TEXT("Frozen range retained"),Result->Samples,5);
    FString Written;TestTrue(TEXT("Read completed export"),FFileHelper::LoadFileToString(Written,*Path));
    TestTrue(TEXT("Frozen columns retained"),Written.Contains(TEXT("\"CL\",\"CD\"")));
    TestFalse(TEXT("Empty selection has no pretend output"),StudioMonitorExport::CSV(H,S,CSV,Count,Error));
    return true;
}
#endif
