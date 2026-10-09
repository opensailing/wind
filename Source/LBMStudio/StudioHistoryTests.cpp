#include "StudioHistory.h"
#include "StudioAssets.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FString PublishedHistoryPath()
{ return FPaths::ProjectContentDir()/TEXT("Samples/NaluWind_NACA0021_Re270k_AoA30/history.json"); }

struct FHistoryFixtureCopy
{
    FString Directory=FPaths::ProjectSavedDir()/TEXT("Automation/HistoryTests")/FGuid::NewGuid().ToString();
    FHistoryFixtureCopy()
    {
        IFileManager::Get().MakeDirectory(*Directory,true);
        const FString Source=FPaths::GetPath(PublishedHistoryPath());
        for(const TCHAR* Name:{TEXT("history.json"),TEXT("history.csv"),TEXT("history-provenance.json"),TEXT("LICENSE.txt")})
            IFileManager::Get().Copy(*(Directory/Name),*(Source/Name));
    }
    ~FHistoryFixtureCopy() {IFileManager::Get().DeleteDirectory(*Directory,false,true);}
    FString Path() const {return Directory/TEXT("history.json");}
    bool Edit(TFunctionRef<void(FJsonObject&)> Change)
    {
        FString Text;TSharedPtr<FJsonObject> O;
        if(!FFileHelper::LoadFileToString(Text,*Path())||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O)||!O) return false;
        Change(*O);Text.Empty();FJsonSerializer::Serialize(O.ToSharedRef(),TJsonWriterFactory<>::Create(&Text));
        return FFileHelper::SaveStringToFile(Text,*Path(),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    bool ChangePayload(TFunctionRef<void(FString&)> Change)
    {
        const FString CSV=Directory/TEXT("history.csv");FString Text,Hash,Error;
        if(!FFileHelper::LoadFileToString(Text,*CSV)) return false;
        Change(Text);
        if(!FFileHelper::SaveStringToFile(Text,*CSV,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)||
            !StudioAssets::HashFile(CSV,MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false),Hash,Error)) return false;
        return Edit([&](FJsonObject& O){O.SetStringField(TEXT("payloadSHA256"),Hash);});
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHistoryPublished,"Studio.History.PublishedValuesAndIndependentIdentity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioHistoryPublished::RunTest(const FString&)
{
    const auto Catalog=StudioHistories::Installed();
    const auto* Entry=Catalog.FindByPredicate([](const auto& E){return E.Id==TEXT("NaluWind_NACA0021_Re270k_AoA30");});
    if(!TestNotNull(TEXT("Published history is independently discoverable"),Entry)) return false;
    const auto Registered=StudioHistories::Load(Entry->Path,{},Entry->MetadataSHA256);
    TestTrue(TEXT("Catalog interpretation hash verified"),Registered.History.IsValid());
    auto Future=Async(EAsyncExecution::ThreadPool,[]{return StudioHistories::Load(PublishedHistoryPath());});
    const auto Result=Future.Get();
    TestTrue(TEXT("Published history loads on worker"),Result.Error.IsEmpty());
    if(!TestTrue(TEXT("Immutable history returned"),Result.History.IsValid())) return false;
    const auto& H=*Result.History;
    TestEqual(TEXT("Complete source sample count"),H.Times.Num(),6967);
    TestEqual(TEXT("Time remains original solver time"),H.Times[0],.4004);
    TestEqual(TEXT("Actual final source sample"),H.Times.Last(),3.1868);
    TestEqual(TEXT("Verified physical time unit"),H.TimeUnit,FString(TEXT("s")));
    TestFalse(TEXT("History is not attached to unrelated SU2 fields"),H.FieldRecordingId.IsSet());
    TestEqual(TEXT("Normalization reference retained"),H.ReferenceValues.FindRef(TEXT("denominator_N")),6000.);
    TestNull(TEXT("Residuals remain unavailable"),H.FindColumn(TEXT("residual")));
    const auto* CL=H.FindColumn(TEXT("CL"));const auto* CD=H.FindColumn(TEXT("CD"));const auto* Fpy=H.FindColumn(TEXT("Fpy"));
    if(!TestNotNull(TEXT("Lift column"),CL)||!TestNotNull(TEXT("Drag column"),CD)||!TestNotNull(TEXT("Pressure force column"),Fpy)) return false;
    TestEqual(TEXT("Original pressure force retained"),Fpy->Values[0],4955.1);
    TestEqual(TEXT("Force unit"),Fpy->Unit,FString(TEXT("N")));
    TestEqual(TEXT("Coefficient is explicit derivation"),CL->Origin,FString(TEXT("derived")));
    TestTrue(TEXT("Known initial coefficient"),FMath::IsNearlyEqual(CL->Values[0],.825892816,1e-14));
    TestTrue(TEXT("Known final coefficient"),FMath::IsNearlyEqual(CD->Values.Last(),.5107034333333333,1e-14));
    double Lift=0,Drag=0;
    for(int32 I=H.Times.Num()-4000;I<H.Times.Num();++I) {Lift+=CL->Values[I];Drag+=CD->Values[I];}
    TestTrue(TEXT("Authors last-4000 lift calculation"),FMath::IsNearlyEqual(Lift/4000,.9784452971963332,1e-12));
    TestTrue(TEXT("Authors last-4000 drag calculation"),FMath::IsNearlyEqual(Drag/4000,.5163203640541667,1e-12));
    TestTrue(TEXT("Pinned interpretation reloads"),StudioHistories::Load(PublishedHistoryPath(),{},H.MetadataSHA256).History.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHistoryIntegrity,"Studio.History.IntegrityCancellationAndBudgets",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioHistoryIntegrity::RunTest(const FString&)
{
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    const auto Cancelled=StudioHistories::Load(PublishedHistoryPath(),Cancel);
    TestFalse(TEXT("Cancelled load publishes nothing"),Cancelled.History.IsValid());
    TestTrue(TEXT("Cancellation reported"),Cancelled.Error.Contains(TEXT("cancelled")));
    TestFalse(TEXT("Different saved metadata rejected"),StudioHistories::Load(PublishedHistoryPath(),{},FString::ChrN(64,TCHAR('0'))).History.IsValid());
    {
        FHistoryFixtureCopy Copy;
        TestTrue(TEXT("Modify payload"),FFileHelper::SaveStringToFile(TEXT("corrupt"),*(Copy.Directory/TEXT("history.csv"))));
        TestFalse(TEXT("Changed payload rejected"),StudioHistories::Load(Copy.Path()).History.IsValid());
    }
    {
        FHistoryFixtureCopy Copy;IFileManager::Get().Delete(*(Copy.Directory/TEXT("LICENSE.txt")));
        TestFalse(TEXT("Missing source notice rejected"),StudioHistories::Load(Copy.Path()).History.IsValid());
    }
    {
        FHistoryFixtureCopy Copy;
        TestTrue(TEXT("Oversize metadata count"),Copy.Edit([](FJsonObject& O){O.SetNumberField(TEXT("sampleCount"),1000000);}));
        const auto R=StudioHistories::Load(Copy.Path());
        TestFalse(TEXT("Excessive allocation rejected before CSV read"),R.History.IsValid());
        TestTrue(TEXT("Allocation limit reported"),R.Error.Contains(TEXT("budget")));
    }
    {
        FHistoryFixtureCopy Copy;
        TestTrue(TEXT("Path escape descriptor"),Copy.Edit([](FJsonObject& O){O.SetStringField(TEXT("payload"),TEXT("../history.csv"));}));
        TestFalse(TEXT("Payload is confined to its pair"),StudioHistories::Load(Copy.Path()).History.IsValid());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHistoryStructure,"Studio.History.RejectMalformedSamples",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioHistoryStructure::RunTest(const FString&)
{
    for(const FString Bad:{TEXT("nan"),TEXT("0.4004junk"),TEXT("1e9999"),TEXT("1e-9999"),TEXT(""),TEXT("1e")})
    {
        FHistoryFixtureCopy Copy;
        TestTrue(TEXT("Alter one source lexeme and rehash"),Copy.ChangePayload([&](FString& S){S.ReplaceInline(TEXT("0.4004,"),*(Bad+TEXT(",")),ESearchCase::CaseSensitive);}));
        TestFalse(TEXT("Invalid sample rejected even with matching payload hash"),StudioHistories::Load(Copy.Path()).History.IsValid());
    }
    {
        FHistoryFixtureCopy Copy;
        TestTrue(TEXT("Duplicate source time"),Copy.ChangePayload([](FString& S){S.ReplaceInline(TEXT("0.4008,"),TEXT("0.4004,"),ESearchCase::CaseSensitive);}));
        TestFalse(TEXT("Duplicate time rejected"),StudioHistories::Load(Copy.Path()).History.IsValid());
    }
    {
        FHistoryFixtureCopy Copy;
        TestTrue(TEXT("Truncate valid final line"),Copy.ChangePayload([](FString& S){S.TrimEndInline();int32 End;S.FindLastChar('\n',End);S.LeftInline(End+1);}));
        TestFalse(TEXT("Incomplete sequence rejected"),StudioHistories::Load(Copy.Path()).History.IsValid());
    }
    {
        FHistoryFixtureCopy Copy;
        TestTrue(TEXT("Reorder claimed source columns"),Copy.Edit([](FJsonObject& O)
        {auto A=O.GetArrayField(TEXT("columns"));Swap(A[1],A[2]);O.SetArrayField(TEXT("columns"),A);}));
        TestFalse(TEXT("Wrong semantic column mapping rejected"),StudioHistories::Load(Copy.Path()).History.IsValid());
    }
    return true;
}
#endif
