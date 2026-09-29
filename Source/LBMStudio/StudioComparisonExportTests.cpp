#include "StudioComparisonExport.h"
#include "StudioModel.h"
#include "StudioSavedFieldView.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioComparisonExportTests
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FString Root(){return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/ComparisonExport"));}
TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source(const TCHAR* Name)
{
    const FString Path=FPaths::ProjectContentDir()/TEXT("Samples")/Name;
    return FString(Name).StartsWith(TEXT("MeshGraphNets"))?TSharedPtr<IStudioSolver,ESPMode::ThreadSafe>(MakeShared<FRecordedSolver,ESPMode::ThreadSafe>(Path/TEXT("flow.bin"))):
        StudioRecordings::Import(Path/TEXT("recording.json"),0,{}).Source;
}
FStudioComparisonExportRequest Request(int32 Kind=0)
{
    const auto A=Source(Kind==3?TEXT("NACA0018_ReaderFixture"):Kind==2?TEXT("Cylinder3D_ReaderFixture"):TEXT("MeshGraphNets_Airfoil"));
    const auto B=Source(Kind==0?TEXT("MeshGraphNets_Airfoil_test010"):Kind==1?TEXT("NACA0018_ReaderFixture"):TEXT("Cylinder3D_ReaderFixture"));
    FStudioComparisonRequest Read;Read.ProjectId=FGuid::NewGuid();Read.Primary=A;Read.Secondary=B;Read.PrimaryOrdinal=Kind==3?0:Kind==2?1:420;Read.Scalar=TEXT("pressure");
    Read.Alignment.Mode=Kind==3?EStudioTimeAlignment::ElapsedFromStart:Kind==1?EStudioTimeAlignment::ManualOffset:EStudioTimeAlignment::RecordedTime;
    if(Kind==1){Read.Alignment.SecondaryOffsetSeconds=-2.5025;Read.Alignment.Match=EStudioTimeMatch::Nearest;Read.Alignment.MaximumMismatchSeconds=.1;}
    FStudioComparisonExportRequest R;R.Pair=StudioComparison::Evaluate(Read);R.Name=TEXT("Wing \"A/B\" · α");
    R.PrimaryCamera.Position.X+=.25;R.SecondaryCamera.Position.Z+=.5;R.bSharedRange=true;return R;
}
TArray<FString> Entries(const FString& Path)
{TArray<FString> Names;IFileManager::Get().FindFiles(Names,*(Path/TEXT("*")),true,true);Names.Sort();return Names;}
struct FDirectory
{
    FString Path=Root()/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FDirectory(){IFileManager::Get().MakeDirectory(*Path,true);}
    ~FDirectory(){IFileManager::Get().DeleteDirectory(*Path,false,true);}
};
TOptional<FStudioComparisonExportResult> Await(FStudioComparisonExportTask& Task)
{
    const double Deadline=FPlatformTime::Seconds()+20;
    do{if(auto R=Task.Poll())return R;FPlatformProcess::SleepNoStats(.001f);}while(FPlatformTime::Seconds()<Deadline);return {};
}
struct FGate
{
    FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
    FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
    ~FGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComparisonExportOriginals,"Studio.ComparisonExport.PublishedPairsAndMetadata",StudioComparisonExportTests::Flags)
bool FComparisonExportOriginals::RunTest(const FString&)
{
    using namespace StudioComparisonExportTests;
    for(int32 Kind=0;Kind<4;++Kind)for(bool CSV:{false,true})for(bool Scene:{false,true})
    {
        auto R=Request(Kind);R.Format=CSV?EStudioFieldExportFormat::CSV:EStudioFieldExportFormat::VTK;
        R.Coordinates=Scene?EStudioExportCoordinates::Scene:EStudioExportCoordinates::Source;FString Error;
        if(!TestTrue(*Error,StudioComparisonExport::Validate(R,Error)))return false;
        TestTrue(TEXT("Export works after live source readers have expired"),!R.Pair.PrimarySource.IsValid()&&!R.Pair.SecondarySource.IsValid());
        const FString Folder=Root()/FString::Printf(TEXT("pair%d-%s-%s"),Kind,CSV?TEXT("csv"):TEXT("vtk"),Scene?TEXT("scene"):TEXT("source"));
        IFileManager::Get().DeleteDirectory(*Folder,false,true);IFileManager::Get().MakeDirectory(*Folder,true);
        int64 Last=0,Total=0;bool Monotonic=true;
        const auto Result=StudioComparisonExport::Write(R,Folder,{},[&](int64 Done,int64 All){Monotonic&=Done>=Last&&Done<=All;Last=Done;Total=All;});
        if(!TestTrue(*Result.Error,Result.bSuccess))return false;
        TestTrue(TEXT("Both sides finish with bounded monotonic progress"),Result.CompletedSides==2&&Monotonic&&Last==Total&&Total>0);
        TestEqual(TEXT("Whole comparison file set"),Entries(Folder).Num(),CSV?3:4);
        TestTrue(TEXT("Frozen output identities"),StudioSavedFieldViews::SameIdentity(Result.PrimaryIdentity,R.Pair.Primary.Identity)&&StudioSavedFieldViews::SameIdentity(Result.SecondaryIdentity,R.Pair.Secondary.Identity));
        FString Text;TSharedPtr<FJsonObject> J;FFileHelper::LoadFileToString(Text,*(Folder/TEXT("comparison.json")));
        if(!TestTrue(TEXT("Metadata is JSON"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),J)))return false;
        TestEqual(TEXT("Unicode comparison name"),J->GetStringField(TEXT("name")),R.Name);
        TestEqual(TEXT("Exact signed time mismatch"),J->GetNumberField(TEXT("secondary_minus_primary_aligned_seconds")),R.Pair.Frames.MismatchSeconds);
        FStudioSavedFieldView A,B;
        TestTrue(TEXT("Independent identities and cameras preserved"),StudioSavedFieldViews::FromJSON(J->GetObjectField(TEXT("primary")),A)&&
            StudioSavedFieldViews::FromJSON(J->GetObjectField(TEXT("secondary")),B)&&StudioView::CameraEquals(A.Camera,R.PrimaryCamera)&&StudioView::CameraEquals(B.Camera,R.SecondaryCamera));
    }
    AddInfo(TEXT("Independent pair audit directory: ")+Root());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComparisonExportValidation,"Studio.ComparisonExport.ValidationAndPartialWriteFailures",StudioComparisonExportTests::Flags)
bool FComparisonExportValidation::RunTest(const FString&)
{
    using namespace StudioComparisonExportTests;const auto R=Request();FString Error;
    for(int32 K=0;K<12;++K)
    {
        auto Bad=R;
        switch(K)
        {
        case 0:Bad.Pair.Frames.Status=EStudioComparisonStatus::NoMatch;break;
        case 1:Bad.Pair.Primary.Field.Reset();break;
        case 2:Bad.Pair.Secondary.Identity.Ordinal++;break;
        case 3:Bad.Pair.Secondary.Scalar.Unit=TEXT("unknown");break;
        case 4:Bad.Pair.Frames.MismatchSeconds=.01;break;
        case 5:Bad.Pair.Frames.SecondaryFrame.Time+=1;break;
        case 6:Bad.Pair.Alignment.Mode=EStudioTimeAlignment::Unset;break;
        case 7:Bad.Format=EStudioFieldExportFormat(255);break;
        case 8:Bad.Coordinates=EStudioExportCoordinates(255);break;
        case 9:Bad.PrimaryCamera.Position.X=std::numeric_limits<double>::infinity();break;
        case 10:Bad.Name=TEXT("bad\nname");break;
        case 11:Bad.Pair.Secondary.Field=Bad.Pair.Primary.Field;break;
        }
        FDirectory D;const auto Result=StudioComparisonExport::Write(Bad,D.Path);
        TestTrue(TEXT("Invalid pair produces no bytes"),!Result.bSuccess&&!Result.Error.IsEmpty()&&Entries(D.Path).IsEmpty());
    }
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);FDirectory D;
    auto Result=StudioComparisonExport::Write(R,D.Path,Cancel);
    TestTrue(TEXT("Pre-cancel leaves staging empty"),Result.bCancelled&&Entries(D.Path).IsEmpty());
    Cancel->store(false);Result=StudioComparisonExport::Write(R,D.Path,Cancel,[&](int64 Done,int64){if(Done>=1000000)Cancel->store(true);});
    TestTrue(TEXT("Cancellation between sides leaves no published pair metadata"),Result.bCancelled&&!Result.bSuccess&&!IFileManager::Get().FileExists(*(D.Path/TEXT("comparison.json"))));
    FDirectory Blocked;TestTrue(TEXT("Create second-file obstruction"),FFileHelper::SaveStringToFile(TEXT("Keep this"),*(Blocked.Path/TEXT("secondary.vtp"))));
    Result=StudioComparisonExport::Write(R,Blocked.Path);FString Sentinel;FFileHelper::LoadFileToString(Sentinel,*(Blocked.Path/TEXT("secondary.vtp")));
    TestTrue(TEXT("Failure after first side keeps existing bytes and no metadata"),!Result.bSuccess&&Result.CompletedSides==1&&Sentinel==TEXT("Keep this")&&!IFileManager::Get().FileExists(*(Blocked.Path/TEXT("comparison.json"))));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComparisonExportPublication,"Studio.ComparisonExport.AtomicPublicationCancellationAndLifetime",StudioComparisonExportTests::Flags)
bool FComparisonExportPublication::RunTest(const FString&)
{
    using namespace StudioComparisonExportTests;FDirectory D;FString Error;auto R=Request();FStudioComparisonExportTask Task;
    for(const auto* Name:{TEXT("../escape"),TEXT(".hidden"),TEXT("sub/name"),TEXT("a:b"),TEXT("")})
        TestFalse(TEXT("Reject invalid new directory name"),Task.Start(R,D.Path,Name,Error));
    TestFalse(TEXT("Reject relative parent"),Task.Start(R,TEXT("relative"),TEXT("pair"),Error));
    auto Gate=MakeShared<FGate,ESPMode::ThreadSafe>();ON_SCOPE_EXIT{Gate->Release->Trigger();};
    Task.BeforePublishForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(10000);};
    TestTrue(TEXT("Begin staged pair"),Task.Start(R,D.Path,TEXT("cancelled"),Error));
    TestTrue(TEXT("Whole pair is staged before publication"),Gate->Reached->Wait(10000));
    TestTrue(TEXT("Progress reaches total at publication boundary"),Task.Progress().Completed==Task.Progress().Total);
    TestFalse(TEXT("One task without a queue"),Task.Start(R,D.Path,TEXT("other"),Error));
    TestTrue(TEXT("Cancellation wins before publication"),Task.Cancel());Gate->Release->Trigger();auto Result=Await(Task);
    TestTrue(TEXT("Cancelled staging is removed"),Result&&Result->bCancelled&&!Result->bSuccess&&Entries(D.Path).IsEmpty());
    Gate->Reached->Reset();Gate->Release->Reset();
    Task.BeforePublishForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(10000);};
    TestTrue(TEXT("Start raced destination"),Task.Start(R,D.Path,TEXT("raced"),Error));TestTrue(TEXT("Reach destination race"),Gate->Reached->Wait(10000));
    TestTrue(TEXT("Create destination before publish"),FFileHelper::SaveStringToFile(TEXT("Keep raced file"),*(D.Path/TEXT("raced"))));Gate->Release->Trigger();Result=Await(Task);
    FString Text;FFileHelper::LoadFileToString(Text,*(D.Path/TEXT("raced")));
    TestTrue(TEXT("Race never replaces existing destination"),Result&&!Result->bSuccess&&!Result->bCancelled&&Text==TEXT("Keep raced file")&&Entries(D.Path).Num()==1);
    const auto Frozen=R.Pair.Primary.Identity;const auto Project=R.Pair.ProjectId;const auto Name=R.Name;
    TWeakPtr<const IStudioField,ESPMode::ThreadSafe> Field=R.Pair.Primary.Field;
    TestTrue(TEXT("Start frozen export"),Task.Start(R,D.Path,TEXT("complete"),Error));R={};Result=Await(Task);
    TestTrue(TEXT("Snapshot survives caller replacement"),Result&&Result->bSuccess&&Result->Name==Name&&Result->ProjectId==Project&&StudioSavedFieldViews::SameIdentity(Result->PrimaryIdentity,Frozen));
    TestTrue(TEXT("Worker releases original snapshots after completion"),!Field.IsValid());
    TestFalse(TEXT("Published export cannot be cancelled"),Task.Cancel());
    R=Request();Field=R.Pair.Primary.Field;TestTrue(TEXT("Start shutdown export"),Task.Start(R,D.Path,TEXT("shutdown"),Error));R={};Task.Shutdown();
    TestTrue(TEXT("Shutdown joins and releases ownership"),!Task.IsBusy()&&!Field.IsValid());
    TestFalse(TEXT("Shutdown task cannot restart"),Task.Start(Request(),D.Path,TEXT("restart"),Error));return true;
}
#endif
