#include "StudioCSVExport.h"
#include "StudioFieldExportTask.h"
#include "StudioFieldSequence.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/BufferArchive.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioCSVTests
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FString Root(){return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/CSVExport"));}
TArray<FString> Staging()
{TArray<FString> Names;IFileManager::Get().FindFiles(Names,*(FPaths::ProjectSavedDir()/TEXT("ExportStaging/*")),true,true);Names.Sort();return Names;}
FStudioFieldExportResult Save(const FStudioFieldExportRequest& R,const TCHAR* Name)
{
    IFileManager::Get().MakeDirectory(*Root(),true);
    TUniquePtr<FArchive> A(IFileManager::Get().CreateFileWriter(*(Root()/Name)));
    if(!A){FStudioFieldExportResult Result;Result.Error=TEXT("Could not create test output.");return Result;}
    auto Result=StudioCSVExport::Write(R,*A);
    if(!A->Close()||A->IsError()){Result.bSuccess=false;Result.Error=TEXT("Could not close test output.");}return Result;
}
class FFailingCSVArchive final:public FArchive
{
public:
    FFailingCSVArchive(){SetIsSaving(true);}
    void Serialize(void*,int64 Count) override{Largest=FMath::Max(Largest,Count);Written+=Count;if(Written>100000)SetError();}
    int64 Written=0,Largest=0;
};
struct FGate
{
    FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
    FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
    ~FGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
};
TOptional<FStudioFieldExportResult> Await(FStudioFieldExportTask& Task)
{
    const double End=FPlatformTime::Seconds()+10;
    while(FPlatformTime::Seconds()<End){auto R=Task.Poll();if(R)return R;FPlatformProcess::SleepNoStats(.001f);}return {};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSVOriginalRows,"Studio.CSVExport.PublishedOriginalPrecisionAndFields",StudioCSVTests::Flags)
bool FCSVOriginalRows::RunTest(const FString&)
{
    using namespace StudioCSVTests;
    const auto Before=Staging();FRecordedSolver Source;
    auto Read=Source.ReadScalarFrame(420,TEXT("pressure"));if(!TestTrue(*Read.Error,Read.Field.IsValid()))return false;
    FStudioFieldExportRequest R{Read.Field,{TEXT("pressure"),TEXT("density"),TEXT("velocity_x"),TEXT("velocity_y"),TEXT("velocity_magnitude")}};
    auto Result=Save(R,TEXT("airfoil-source.csv"));TestTrue(*Result.Error,Result.bSuccess&&Result.Triangles==0);
    R.Coordinates=EStudioExportCoordinates::Scene;Result=Save(R,TEXT("airfoil-scene.csv"));TestTrue(*Result.Error,Result.bSuccess);
    for(const auto* Name:{TEXT("NACA0018_ReaderFixture"),TEXT("Cylinder3D_ReaderFixture")})
    {
        const auto Loaded=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples")/Name/TEXT("recording.json"),2,{});
        if(!TestTrue(*Loaded.Error,Loaded.Source.IsValid()))return false;
        const auto Field=Loaded.Source->ReadScalarFrame(2,TEXT("pressure"));if(!Field.Field)return false;
        R={Field.Field,{}};for(const auto& S:Loaded.Source->Descriptor().Scalars)R.Scalars.Add(S.Id);
        Result=Save(R,Loaded.Source->Descriptor().SpatialDimensions==3?TEXT("cylinder.csv"):TEXT("naca.csv"));TestTrue(*Result.Error,Result.bSuccess);
    }
    TestTrue(TEXT("All scratch columns removed after success"),Staging()==Before);
    AddInfo(TEXT("Independent CSV audit inputs: ")+Root());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSVWriteSafety,"Studio.CSVExport.BoundedChunksCancellationAndDiskFailure",StudioCSVTests::Flags)
bool FCSVWriteSafety::RunTest(const FString&)
{
    using namespace StudioCSVTests;
    const auto Before=Staging();FRecordedSolver Source;const auto Read=Source.ReadScalarFrame(420,TEXT("pressure"));if(!Read.Field)return false;
    FStudioFieldExportRequest R{Read.Field,{TEXT("pressure"),TEXT("density")}};
    auto Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);FBufferArchive Pre;
    auto Result=StudioCSVExport::Write(R,Pre,Cancellation);
    TestTrue(TEXT("Pre-cancel creates no output or scratch"),Result.bCancelled&&Pre.IsEmpty()&&Staging()==Before);
    for(bool Rows:{false,true})
    {
        FBufferArchive Partial;Cancellation->store(false);int64 Last=0,Total=0;bool Monotonic=true;
        Result=StudioCSVExport::Write(R,Partial,Cancellation,[&](int64 Done,int64 T)
        {
            Monotonic&=Done>Last&&Done<=T&&(!Total||Total==T);Last=Done;Total=T;
            if(Rows?Done>int64(Read.Field->OriginalPointCount())*2:Done>=4096)Cancellation->store(true);
        });
        TestTrue(TEXT("Column/row cancellation is explicit, bounded and cleans scratch"),Monotonic&&Result.bCancelled&&!Result.bSuccess&&Last<Total&&Staging()==Before);
        if(!Rows)TestTrue(TEXT("Column cancellation emits no CSV bytes"),Partial.IsEmpty());
    }
    FFailingCSVArchive Failure;Result=StudioCSVExport::Write(R,Failure);
    TestTrue(TEXT("Disk failure uses small UTF-8 writes and cleans scratch"),!Result.bSuccess&&!Result.Error.IsEmpty()&&Failure.Written>100000&&Failure.Largest<40000&&Staging()==Before);
    for(const TArray<FString>& Fields:{TArray<FString>{},TArray<FString>{TEXT("density"),TEXT("density")},TArray<FString>{TEXT("temperature")}})
    {FBufferArchive Invalid;R.Scalars=Fields;Result=StudioCSVExport::Write(R,Invalid);TestTrue(TEXT("Invalid arrays produce no output"),!Result.bSuccess&&Invalid.IsEmpty()&&Staging()==Before);}
    TestTrue(TEXT("Analysis errors leave viewport status untouched"),Source.LoadError().IsEmpty());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSVPublication,"Studio.CSVExport.AtomicSingleAndSequencePublishing",StudioCSVTests::Flags)
bool FCSVPublication::RunTest(const FString&)
{
    using namespace StudioCSVTests;
    const auto Before=Staging();const FString Directory=Root()/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*Directory,true);ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Directory,false,true);};
    const FString Path=Directory/TEXT("existing.csv");FFileHelper::SaveStringToFile(TEXT("sentinel"),*Path);
    auto Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();const auto Read=Source->ReadScalarFrame(420,TEXT("pressure"));if(!Read.Field)return false;
    FStudioFieldExportRequest R{Read.Field,{TEXT("pressure")},EStudioExportCoordinates::Source,EStudioFieldExportFormat::CSV};
    FStudioFieldExportTask Task;auto Gate=MakeShared<FGate,ESPMode::ThreadSafe>();
    Task.BeforePublishForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(10000);};
    TestTrue(TEXT("CSV task starts"),Task.Start(R,Path));TestTrue(TEXT("CSV ready to publish"),Gate->Reached->Wait(10000));
    TestTrue(TEXT("Cancellation accepted before atomic publish"),Task.Cancel());Gate->Release->Trigger();auto Result=Await(Task);
    FString Text;FFileHelper::LoadFileToString(Text,*Path);
    TestTrue(TEXT("Cancelled CSV preserves destination and removes all staging"),Result&&Result->bCancelled&&Text==TEXT("sentinel")&&Staging()==Before);
    if(Task.IsBusy())return false;
    TestTrue(TEXT("CSV retry starts"),Task.Start(R,Path));Result=Await(Task);FFileHelper::LoadFileToString(Text,*Path);
    TestTrue(TEXT("CSV uses native atomic replacement"),Result&&Result->bSuccess&&Text.StartsWith(TEXT("# LBMStudioMetadataUTF8 "))&&Text.EndsWith(TEXT("\n"))&&Staging()==Before);
    FStudioFieldSequenceRequest Sequence{Source,419,421,{TEXT("pressure")},EStudioExportCoordinates::Source,EStudioFieldExportFormat::CSV};
    FStudioFieldSequenceTask Series;FString Error;TestTrue(*Error,Series.Start(Sequence,Directory,TEXT("series"),Error));
    TOptional<FStudioFieldSequenceResult> SeriesResult;const double End=FPlatformTime::Seconds()+10;
    while(FPlatformTime::Seconds()<End){SeriesResult=Series.Poll();if(SeriesResult)break;FPlatformProcess::SleepNoStats(.001f);}
    TestTrue(TEXT("CSV sequence publishes index and every frame"),SeriesResult&&SeriesResult->bSuccess&&SeriesResult->CompletedFrames==3&&
        IFileManager::Get().FileExists(*(Directory/TEXT("series/frames.csv")))&&IFileManager::Get().FileExists(*(Directory/TEXT("series/frame_000421.csv")))&&Staging()==Before);
    return true;
}
#endif
