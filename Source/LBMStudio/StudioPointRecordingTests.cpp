#include "StudioPointRecording.h"
#include "StudioAssets.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
FString FixturePath() { return FPaths::ProjectContentDir() / TEXT("Samples/NACA0018_ReaderFixture/recording.json"); }
TArray<FString> AllFields() { return {TEXT("velocity_u"), TEXT("velocity_v"), TEXT("pressure"), TEXT("velocity_magnitude"), TEXT("cell_volume")}; }
TSharedPtr<FJsonObject> JSON(const FString& Path)
{
    FString Text; TSharedPtr<FJsonObject> O;
    if (FFileHelper::LoadFileToString(Text, *Path)) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O);
    return O;
}
FStudioPointOpenResult OpenWorker(const FString& Path = FixturePath(), const FStudioPointReadOptions& Options = {},
    const FStudioLoadCancellation& Cancel = {}, const FString& Hash = FString())
{ return Async(EAsyncExecution::ThreadPool, [=] { return StudioPointRecordings::Open(Path, Options, Cancel, Hash); }).Get(); }
FStudioPointReadResult ReadWorker(const TSharedPtr<FStudioPointRecording, ESPMode::ThreadSafe>& R, int32 Ordinal,
    const TArray<FString>& Fields = AllFields(), const FStudioLoadCancellation& Cancel = {})
{ return Async(EAsyncExecution::ThreadPool, [=] { return R->ReadFrame(Ordinal, Fields, Cancel); }).Get(); }

struct FPointFixtureCopy
{
    FString Directory = FPaths::ProjectSavedDir() / TEXT("Automation/PointRecording") / FGuid::NewGuid().ToString();
    FPointFixtureCopy()
    {
        IFileManager::Get().MakeDirectory(*Directory, true);
        for (const TCHAR* Name : {TEXT("recording.json"), TEXT("provenance.json"), TEXT("ATTRIBUTION.txt"),
            TEXT("coordinates.f64"), TEXT("point-ids.i64"), TEXT("velocity_u.f64"), TEXT("velocity_v.f64"),
            TEXT("pressure.f64"), TEXT("velocity_magnitude.f64"), TEXT("cell_volume.f64")})
            IFileManager::Get().Copy(*(Directory / Name), *(FPaths::GetPath(FixturePath()) / Name));
    }
    ~FPointFixtureCopy() { IFileManager::Get().DeleteDirectory(*Directory, false, true); }
    FString Path() const { return Directory / TEXT("recording.json"); }
    bool Edit(TFunctionRef<void(FJsonObject&)> Change)
    {
        auto O = JSON(Path()); if (!O) return false;
        Change(*O); FString Text; FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
        return FFileHelper::SaveStringToFile(Text, *Path(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    bool Corrupt(const TCHAR* Name, int64 Byte)
    {
        TArray<uint8> Bytes; const FString File = Directory / Name;
        if (!FFileHelper::LoadFileToArray(Bytes, *File) || Byte < 0 || Byte >= Bytes.Num()) return false;
        Bytes[int32(Byte)] ^= 1; return FFileHelper::SaveArrayToFile(Bytes, *File);
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointPublished, "Studio.PointRecording.PublishedValuesAndOptionalFields", TestFlags)
bool FStudioPointPublished::RunTest(const FString&)
{
    const auto Loaded = OpenWorker();
    if (!TestTrue(*Loaded.Error, Loaded.Recording.IsValid())) return false;
    const auto& D = Loaded.Recording->Descriptor();
    TestEqual(TEXT("Three sparse test snapshots only"), D.Frames.Num(), 3);
    TestEqual(TEXT("Original point count"), D.PointCount, 18706);
    TestEqual(TEXT("Original coordinates remain 2D"), D.SpatialDimensions, 2);
    TestEqual(TEXT("Actual last step"), D.Frames.Last().Index, 9000);
    TestEqual(TEXT("Original first time"), D.Frames[0].Time, 2.5025);
    TestEqual(TEXT("Original final time"), D.Frames.Last().Time, 22.5);
    TestNull(TEXT("No fabricated density"), D.FindField(TEXT("density")));
    TestEqual(TEXT("Undocumented volume unit retained"), D.FindField(TEXT("cell_volume"))->Unit, FString(TEXT("unspecified")));
    TestEqual(TEXT("Supplied speed is explicitly separate"), D.DefaultScalar, FString(TEXT("velocity_magnitude")));
    const auto Expected = JSON(FPaths::GetPath(FixturePath()) / TEXT("expected.json"));
    if (!TestTrue(TEXT("Independent pandas golden values present"), Expected.IsValid())) return false;
    int32 Compared = 0;
    for (int32 I = 0; I < 3; ++I)
    {
        const auto Read = ReadWorker(Loaded.Recording, I);
        if (!TestTrue(*Read.Error, Read.Frame.IsValid())) return false;
        TestEqual(TEXT("Requested ordinal retained"), Read.Frame->Ordinal, I);
        TestNull(TEXT("Absent field has no zero-filled array"), Read.Frame->FindValues(TEXT("density")));
        for (const auto& Value : Expected->GetArrayField(TEXT("samples")))
        {
            const auto E = Value->AsObject();
            if (int32(E->GetNumberField(TEXT("frame"))) != I) continue;
            const auto* Values = Read.Frame->FindValues(E->GetStringField(TEXT("field")));
            if (!TestNotNull(TEXT("Selected original field exists"), Values)) return false;
            TestEqual(TEXT("Exact independent original HDF5 value"), (*Values)[int32(E->GetNumberField(TEXT("point")))], E->GetNumberField(TEXT("value")));
            ++Compared;
        }
        for (const auto& Value : Expected->GetArrayField(TEXT("points")))
        {
            const auto E = Value->AsObject(); const int32 Row = int32(E->GetNumberField(TEXT("row")));
            const auto XY = E->GetArrayField(TEXT("position"));
            TestEqual(TEXT("Original point label"), Read.Frame->Geometry->PointIds[Row], int64(E->GetNumberField(TEXT("id"))));
            TestEqual(TEXT("Source X without view transform"), Read.Frame->Geometry->Positions[Row].X, XY[0]->AsNumber());
            TestEqual(TEXT("Source Y without extrusion"), Read.Frame->Geometry->Positions[Row].Y, XY[1]->AsNumber());
            TestEqual(TEXT("No invented spanwise coordinate"), Read.Frame->Geometry->Positions[Row].Z, 0.);
        }
    }
    TestEqual(TEXT("Every independent golden field sample checked"), Compared, 135);
    TestTrue(TEXT("Pinned descriptor can reopen"), OpenWorker(FixturePath(), {}, {}, D.MetadataSHA256).Recording.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointMalformed, "Studio.PointRecording.RejectMalformedDescriptors", TestFlags)
bool FStudioPointMalformed::RunTest(const FString&)
{
    const TArray<TFunction<void(FJsonObject&)>> Changes = {
        [](FJsonObject& O) { O.SetNumberField(TEXT("pointCount"), 1e20); },
        [](FJsonObject& O) { O.SetNumberField(TEXT("spatialDimensions"), 4); },
        [](FJsonObject& O) { O.SetStringField(TEXT("coordinateUnit"), TEXT("unknown")); },
        [](FJsonObject& O) { O.SetStringField(TEXT("defaultScalar"), TEXT("density")); },
        [](FJsonObject& O) { O.GetObjectField(TEXT("coordinates"))->SetStringField(TEXT("path"), TEXT("../outside.f64")); },
        [](FJsonObject& O) { O.GetObjectField(TEXT("coordinates"))->SetStringField(TEXT("path"), TEXT("ATTRIBUTION.txt")); },
        [](FJsonObject& O) { O.GetObjectField(TEXT("coordinates"))->SetNumberField(TEXT("byteLength"), 4); },
        [](FJsonObject& O) { O.GetObjectField(TEXT("topology"))->SetStringField(TEXT("kind"), TEXT("triangles")); },
        [](FJsonObject& O) { O.GetArrayField(TEXT("frames"))[1]->AsObject()->SetNumberField(TEXT("time"), 2.5025); },
        [](FJsonObject& O) { O.GetArrayField(TEXT("frames"))[1]->AsObject()->SetStringField(TEXT("label"), TEXT("t_1001")); },
        [](FJsonObject& O) { O.GetArrayField(TEXT("fields"))[0]->AsObject()->SetStringField(TEXT("association"), TEXT("cell")); },
        [](FJsonObject& O) { O.GetArrayField(TEXT("fields"))[0]->AsObject()->SetStringField(TEXT("origin"), TEXT("derived")); },
        [](FJsonObject& O) { O.GetArrayField(TEXT("fields"))[0]->AsObject()->SetStringField(TEXT("component"), TEXT("z")); },
        [](FJsonObject& O) { O.GetArrayField(TEXT("fields"))[1]->AsObject()->SetStringField(TEXT("component"), TEXT("x")); },
        [](FJsonObject& O) { O.GetArrayField(TEXT("fields"))[1]->AsObject()->SetStringField(TEXT("unit"), TEXT("Pa")); },
        [](FJsonObject& O) { O.GetArrayField(TEXT("fields"))[0]->AsObject()->GetObjectField(TEXT("array"))->RemoveField(TEXT("frameCRC32")); },
        [](FJsonObject& O) { O.GetArrayField(TEXT("fields"))[1]->AsObject()->SetStringField(TEXT("id"), TEXT("velocity_u")); },
        [](FJsonObject& O) { O.GetObjectField(TEXT("pointIds"))->SetStringField(TEXT("byteOrder"), TEXT("big")); }
    };
    for (const auto& Change : Changes)
    {
        FPointFixtureCopy Copy; TestTrue(TEXT("Write invalid descriptor"), Copy.Edit(Change));
        const auto R = OpenWorker(Copy.Path());
        TestFalse(TEXT("Invalid interpretation never publishes a recording"), R.Recording.IsValid());
        TestFalse(TEXT("Invalid descriptor provides a reason"), R.Error.IsEmpty());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointIntegrity, "Studio.PointRecording.IntegrityAndChangedFiles", TestFlags)
bool FStudioPointIntegrity::RunTest(const FString&)
{
    TestFalse(TEXT("Saved metadata identity cannot change"), OpenWorker(FixturePath(), {}, {}, FString::ChrN(64, '0')).Recording.IsValid());
    {
        FPointFixtureCopy Copy; TestTrue(TEXT("Corrupt original dynamic array"), Copy.Corrupt(TEXT("pressure.f64"), 16));
        TestFalse(TEXT("Whole-source hash detects corruption before publication"), OpenWorker(Copy.Path()).Recording.IsValid());
    }
    {
        FPointFixtureCopy Copy; IFileManager::Get().Delete(*(Copy.Directory / TEXT("ATTRIBUTION.txt")));
        TestFalse(TEXT("Attribution remains part of source integrity"), OpenWorker(Copy.Path()).Recording.IsValid());
    }
    {
        FPointFixtureCopy Copy; const auto R = OpenWorker(Copy.Path());
        if (!TestTrue(TEXT("Valid before disk change"), R.Recording.IsValid())) return false;
        TestTrue(TEXT("Change unread frame after opening"), Copy.Corrupt(TEXT("pressure.f64"), 18706 * 8 + 16));
        const auto Read = ReadWorker(R.Recording, 1, {TEXT("pressure")});
        TestFalse(TEXT("Changed dynamic frame rejected"), Read.Frame.IsValid());
        TestTrue(TEXT("Frame checksum failure explained"), Read.Error.Contains(TEXT("checksum")));
        TestTrue(TEXT("Other verified frame still readable"), ReadWorker(R.Recording, 0, {TEXT("pressure")}).Frame.IsValid());
        TestTrue(TEXT("Change static field after opening"), Copy.Corrupt(TEXT("cell_volume.f64"), 16));
        TestFalse(TEXT("Static frame also rechecks integrity"), ReadWorker(R.Recording, 0, {TEXT("cell_volume")}).Frame.IsValid());
    }
    {
        FPointFixtureCopy Copy;
        TestTrue(TEXT("Change declared bounds"), Copy.Edit([](FJsonObject& O)
        { auto A = O.GetObjectField(TEXT("sourceBounds"))->GetArrayField(TEXT("max")); A[0] = MakeShared<FJsonValueNumber>(1.); O.GetObjectField(TEXT("sourceBounds"))->SetArrayField(TEXT("max"), A); }));
        TestFalse(TEXT("Exact coordinates determine bounds"), OpenWorker(Copy.Path()).Recording.IsValid());
    }
    {
        FPointFixtureCopy Copy;
        TestTrue(TEXT("Narrow claimed field range"), Copy.Edit([](FJsonObject& O)
        { O.GetArrayField(TEXT("fields"))[0]->AsObject()->SetArrayField(TEXT("range"), {MakeShared<FJsonValueNumber>(0.), MakeShared<FJsonValueNumber>(0.)}); }));
        const auto R = OpenWorker(Copy.Path());
        if (!TestTrue(TEXT("Array hashes remain valid"), R.Recording.IsValid())) return false;
        TestFalse(TEXT("Value read validates declared range"), ReadWorker(R.Recording, 0, {TEXT("velocity_u")}).Frame.IsValid());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointBudgets, "Studio.PointRecording.PinnedBudgetsAndCancellation", TestFlags)
bool FStudioPointBudgets::RunTest(const FString&)
{
    const auto Cancel = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Cancelled opening publishes nothing"), OpenWorker(FixturePath(), {}, Cancel).Recording.IsValid());
    constexpr int64 ArrayBytes = 18706 * 8;
    FStudioPointReadOptions Options; Options.CacheBytes = ArrayBytes; Options.LiveArrayBytes = 2 * ArrayBytes;
    auto Loaded = OpenWorker(FixturePath(), Options);
    if (!TestTrue(TEXT("Bounded reader opens"), Loaded.Recording.IsValid())) return false;
    auto A = ReadWorker(Loaded.Recording, 0, {TEXT("pressure")});
    auto B = ReadWorker(Loaded.Recording, 1, {TEXT("pressure")});
    if (!TestTrue(TEXT("Two retained frames fit"), A.Frame.IsValid() && B.Frame.IsValid())) return false;
    const double Original = (*A.Frame->FindValues(TEXT("pressure")))[17];
    TestFalse(TEXT("Evicted but pinned storage counts against budget"), ReadWorker(Loaded.Recording, 2, {TEXT("pressure")}).Frame.IsValid());
    TestTrue(TEXT("Cache stays within its own budget"), Loaded.Recording->Stats().CacheBytes <= Options.CacheBytes);
    TestEqual(TEXT("All retained arrays counted"), Loaded.Recording->Stats().LiveArrayBytes, 2 * ArrayBytes);
    TestEqual(TEXT("Eviction cannot mutate retained frame"), (*A.Frame->FindValues(TEXT("pressure")))[17], Original);
    A.Frame.Reset();
    auto C = ReadWorker(Loaded.Recording, 2, {TEXT("pressure")});
    TestTrue(TEXT("Release makes room for a new frame"), C.Frame.IsValid());
    TestTrue(TEXT("Peak stayed within value-storage budget"), Loaded.Recording->Stats().PeakLiveArrayBytes <= Options.LiveArrayBytes);
    const auto Before = Loaded.Recording->Stats();
    TestFalse(TEXT("Cancelled cached read publishes nothing"), ReadWorker(Loaded.Recording, 2, {TEXT("pressure")}, Cancel).Frame.IsValid());
    TestEqual(TEXT("Cancelled read allocates nothing"), Loaded.Recording->Stats().Loads, Before.Loads);
    TestFalse(TEXT("Absent selection is explicit"), ReadWorker(Loaded.Recording, 0, {TEXT("density")}).Frame.IsValid());
    TestFalse(TEXT("Duplicate selection rejected"), ReadWorker(Loaded.Recording, 0, {TEXT("pressure"), TEXT("pressure")}).Frame.IsValid());
    Options.GeometryBytes = 40;
    TestFalse(TEXT("Geometry allocation bounded before reading"), OpenWorker(FixturePath(), Options).Recording.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointLifetime, "Studio.PointRecording.SnapshotLifetimeAndConcurrentReads", TestFlags)
bool FStudioPointLifetime::RunTest(const FString&)
{
    auto Loaded = OpenWorker();
    if (!TestTrue(TEXT("Original point reader opens"), Loaded.Recording.IsValid())) return false;
    TArray<TFuture<FStudioPointReadResult>> Futures;
    for (int32 I = 0; I < 12; ++I)
    {
        auto R = Loaded.Recording;
        Futures.Add(Async(EAsyncExecution::ThreadPool, [R, I] { return R->ReadFrame(I % 3, {TEXT("velocity_u"), TEXT("cell_volume")}); }));
    }
    TArray<TSharedPtr<const FStudioPointFrame, ESPMode::ThreadSafe>> Frames;
    for (auto& Future : Futures)
    {
        auto Read = Future.Get();
        if (!TestTrue(*Read.Error, Read.Frame.IsValid())) return false;
        Frames.Add(Read.Frame);
    }
    TestEqual(TEXT("Serialized cache avoids duplicate loads"), Loaded.Recording->Stats().Loads, uint64(4));
    TestTrue(TEXT("Static values shared across source frames"), Frames[0]->Fields[TEXT("cell_volume")] == Frames[1]->Fields[TEXT("cell_volume")]);
    TestNull(TEXT("Unrequested pressure remains unavailable"), Frames[0]->FindValues(TEXT("pressure")));
    const FString Id = Loaded.Recording->Descriptor().Id;
    Loaded.Recording.Reset();
    TestEqual(TEXT("Snapshot retains interpretation after reader closes"), Frames[0]->Descriptor->Id, Id);
    TestEqual(TEXT("Snapshot retains geometry after reader closes"), Frames[0]->Geometry->PointIds.Num(), 18706);
    TestEqual(TEXT("Snapshot retains source field after reader closes"), Frames[0]->FindValues(TEXT("velocity_u"))->Num(), 18706);
    return true;
}

// Explicit large-data gate, excluded from the ordinary Studio.* regression filter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointFullSequence, "ScientificAcceptance.PointRecording.FullSequence", TestFlags)
bool FStudioPointFullSequence::RunTest(const FString&)
{
    FString Path;
    if (!FParse::Value(FCommandLine::Get(), TEXT("StudioPointRecording="), Path))
    { AddError(TEXT("Supply -StudioPointRecording=<full recording.json>; the small reader fixture cannot pass this gate.")); return false; }
    const auto Expected = JSON(FPaths::GetPath(FixturePath()) / TEXT("expected.json"));
    if (!TestTrue(TEXT("Independent source values available"), Expected.IsValid())) return false;
    const auto Loaded = OpenWorker(Path, {}, {}, TEXT("1ce4f9f4a7d71f060e60e62ecd0aa52de78e7f7d0328930ef0cdc952cd852a67"));
    if (!TestTrue(*Loaded.Error, Loaded.Recording.IsValid())) return false;
    const auto& D = Loaded.Recording->Descriptor();
    if (!TestEqual(TEXT("Full original sequence"), D.Frames.Num(), 8000) || !TestEqual(TEXT("All source points"), D.PointCount, 18706)) return false;
    TestEqual(TEXT("Physical evolution remains original"), D.Frames.Last().Time - D.Frames[0].Time, 19.9975);
    const auto Stream = Async(EAsyncExecution::ThreadPool, [R = Loaded.Recording, Expected]
    {
        FString Error; int32 GoldenCount = 0;
        const int32 OriginalOrdinals[] = {0, 4000, 7999};
        for (int32 I = 0; I < 8000; ++I)
        {
            const auto Read = R->ReadFrame(I, AllFields());
            if (!Read.Frame) return FString::Printf(TEXT("Frame %d failed: %s"), I, *Read.Error);
            if (Read.Frame->Ordinal != I || Read.Frame->Descriptor->Frames[I].Index != I + 1001)
                return FString(TEXT("Original frame identity changed."));
            for (const auto& V : Expected->GetArrayField(TEXT("samples")))
            {
                const auto E = V->AsObject();
                if (I != OriginalOrdinals[int32(E->GetNumberField(TEXT("frame")))]) continue;
                if ((*Read.Frame->FindValues(E->GetStringField(TEXT("field"))))[int32(E->GetNumberField(TEXT("point")))] != E->GetNumberField(TEXT("value")))
                    return FString(TEXT("Independent original value mismatch."));
                ++GoldenCount;
            }
        }
        return GoldenCount == 135 ? FString() : FString(TEXT("Independent comparisons incomplete."));
    }).Get();
    TestTrue(*Stream, Stream.IsEmpty());
    const auto S = Loaded.Recording->Stats();
    TestTrue(TEXT("Resident cache bounded across all original frames"), S.CacheBytes <= 8LL * 1024 * 1024);
    TestTrue(TEXT("Live cache/pinned/in-flight values bounded"), S.PeakLiveArrayBytes <= 32LL * 1024 * 1024);
    TestEqual(TEXT("Every dynamic field frame read; static field read once"), S.Loads, uint64(32001));
    AddInfo(FString::Printf(TEXT("8000 original frames; 32000 dynamic checksums; 135 independent values; cache=%lld peak-values=%lld geometry=%lld bytes. This gate covers the native reader, not viewport playback."),
        S.CacheBytes, S.PeakLiveArrayBytes, S.GeometryBytes));
    return true;
}
#endif
