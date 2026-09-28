#include "StudioPointRecording.h"
#include "StudioAssets.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioPointPrivate
{
std::atomic<int32> LiveReaders{0}, LiveSnapshots{0}, LiveArrays{0};
std::atomic<int64> LiveAllocatedValueBytes{0};
constexpr int64 MaxMetadataBytes = 8LL * 1024 * 1024;
constexpr int32 MaxPoints = 1000000, MaxFrames = 100000, MaxFields = 32;

bool Cancelled(const FStudioLoadCancellation& C) { return C && C->load(std::memory_order_relaxed); }
bool Clean(const FString& S, int32 Limit)
{
    if (S.TrimStartAndEnd().IsEmpty() || S.Len() > Limit) return false;
    for (TCHAR C : S) if (C < 32 || C == 127) return false;
    return true;
}
bool Identifier(const FString& S)
{
    if (S.IsEmpty() || S.Len() > 128) return false;
    for (TCHAR C : S)
        if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
            (C >= '0' && C <= '9') || C == '_' || C == '-' || C == '.')) return false;
    return S != TEXT(".") && S != TEXT("..");
}
bool HashValid(const FString& S)
{
    if (S.Len() != 64) return false;
    for (TCHAR C : S) if (!FChar::IsHexDigit(C)) return false;
    return true;
}
bool String(const FJsonObject& O, const TCHAR* Key, FString& Out, int32 Limit = 2048)
{ return O.TryGetStringField(Key, Out) && Clean(Out, Limit); }
bool Integer(const FJsonObject& O, const TCHAR* Key, int64 Minimum, int64 Maximum, int64& Out)
{
    double N;
    if (!O.TryGetNumberField(Key, N) || !FMath::IsFinite(N) || N < Minimum || N > Maximum || N != FMath::FloorToDouble(N)) return false;
    Out = int64(N); return true;
}
bool Digest(const uint8* Bytes, int64 Size, FString& Out)
{
    uint8 Hash[EVP_MAX_MD_SIZE]; unsigned int Count = 0;
    if (EVP_Digest(Bytes, Size, Hash, &Count, EVP_sha256(), nullptr) != 1 || Count != 32) return false;
    Out = BytesToHex(Hash, Count).ToLower(); return true;
}
bool SmallFile(const FString& Path, int64 Limit, TArray<uint8>& Bytes, const FStudioLoadCancellation& Cancel)
{
    FStudioFileAccess Access(Path);
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!File || File->TotalSize() < 1 || File->TotalSize() > Limit) return false;
    Bytes.SetNumUninitialized(int32(File->TotalSize()));
    for (int64 Offset = 0; Offset < Bytes.Num();)
    {
        if (Cancelled(Cancel)) return false;
        const int64 Count = FMath::Min<int64>(65536, Bytes.Num() - Offset);
        File->Serialize(Bytes.GetData() + Offset, Count); Offset += Count;
        if (File->IsError()) return false;
    }
    return !Cancelled(Cancel);
}
bool Array(const FJsonObject& O, const TArray<int64>& Shape, FStudioPointArrayDescriptor& A)
{
    FString DType, Order;
    const TArray<TSharedPtr<FJsonValue>>* Dimensions = nullptr;
    if (!String(O, TEXT("path"), A.Path, 128) || !Identifier(A.Path) ||
        !String(O, TEXT("dtype"), DType) || DType != TEXT("float64") ||
        !String(O, TEXT("byteOrder"), Order) || Order != TEXT("little") ||
        !String(O, TEXT("sha256"), A.SHA256) || !HashValid(A.SHA256) ||
        !O.TryGetArrayField(TEXT("shape"), Dimensions) || Dimensions->Num() != Shape.Num()) return false;
    int64 Expected = sizeof(double);
    for (int32 I = 0; I < Shape.Num(); ++I)
    {
        double N;
        if (!(*Dimensions)[I]->TryGetNumber(N) || N != Shape[I] || Shape[I] < 1 || Expected > MAX_int64 / Shape[I]) return false;
        Expected *= Shape[I];
    }
    if (!Integer(O, TEXT("byteLength"), Expected, Expected, A.ByteLength)) return false;
    A.SHA256.ToLowerInline(); return true;
}
bool Bounds(const FJsonObject& O, const TCHAR* Key, int32 Dimensions, FVector& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!O.TryGetArrayField(Key, Values) || Values->Num() != Dimensions) return false;
    Out = FVector::ZeroVector;
    for (int32 I = 0; I < Dimensions; ++I)
        if (!(*Values)[I]->TryGetNumber(Out[I]) || !FMath::IsFinite(Out[I])) return false;
    return true;
}
bool Parse(const TArray<uint8>& Bytes, FStudioPointRecordingDescriptor& D, FString& ProvenanceHash,
    FString& AttributionHash, const FStudioLoadCancellation& Cancel, FString& Error)
{
    FString Text; FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());
    TSharedPtr<FJsonObject> O;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) || !O)
    { Error = TEXT("Point recording descriptor is not valid JSON."); return false; }
    auto Fail = [&](const TCHAR* Message) { Error = Message; return false; };
    FString Kind, TopologyKind, TopologyOrigin;
    int64 Version, Dimensions, Points, Frames;
    const TSharedPtr<FJsonObject>* Topology = nullptr;
    if (!Integer(*O, TEXT("version"), 3, 3, Version) || !String(*O, TEXT("kind"), Kind) || Kind != TEXT("field_recording") ||
        !String(*O, TEXT("id"), D.Id, 128) || !Identifier(D.Id) || !String(*O, TEXT("title"), D.Title, 256) ||
        !String(*O, TEXT("sourceURL"), D.SourceURL) || !String(*O, TEXT("coordinateUnit"), D.CoordinateUnit) || D.CoordinateUnit != TEXT("m") ||
        !String(*O, TEXT("timeUnit"), D.TimeUnit) || D.TimeUnit != TEXT("s") || !String(*O, TEXT("timeOrigin"), D.TimeOrigin) ||
        !Integer(*O, TEXT("spatialDimensions"), 2, 3, Dimensions) || !Integer(*O, TEXT("pointCount"), 1, MaxPoints, Points) ||
        !Integer(*O, TEXT("frameCount"), 1, MaxFrames, Frames) || !String(*O, TEXT("defaultScalar"), D.DefaultScalar, 128) ||
        !String(*O, TEXT("provenanceSHA256"), ProvenanceHash) || !HashValid(ProvenanceHash) ||
        !String(*O, TEXT("attributionSHA256"), AttributionHash) || !HashValid(AttributionHash) ||
        !O->TryGetObjectField(TEXT("topology"), Topology) || !Topology || !Topology->IsValid() ||
        !String(**Topology, TEXT("kind"), TopologyKind) || TopologyKind != TEXT("points") ||
        !String(**Topology, TEXT("origin"), TopologyOrigin) || TopologyOrigin != TEXT("source") ||
        !(*Topology)->HasTypedField<EJson::Null>(TEXT("connectivity")))
        return Fail(TEXT("Unsupported point recording: version 3, explicit source points, meters and seconds are required."));
    D.SpatialDimensions = int32(Dimensions); D.PointCount = int32(Points);
    const TSharedPtr<FJsonObject>* Coordinates = nullptr; const TSharedPtr<FJsonObject>* Ids = nullptr;
    const TSharedPtr<FJsonObject>* SourceBounds = nullptr;
    FString IdType, IdOrder; int64 IdCount;
    if (!O->TryGetObjectField(TEXT("coordinates"), Coordinates) || !Array(**Coordinates, {Points, Dimensions}, D.Coordinates) ||
        !O->TryGetObjectField(TEXT("pointIds"), Ids) || !String(**Ids, TEXT("path"), D.PointIds.Path, 128) || !Identifier(D.PointIds.Path) ||
        !String(**Ids, TEXT("dtype"), IdType) || IdType != TEXT("int64") || !String(**Ids, TEXT("byteOrder"), IdOrder) || IdOrder != TEXT("little") ||
        !Integer(**Ids, TEXT("count"), Points, Points, IdCount) || !String(**Ids, TEXT("sha256"), D.PointIds.SHA256) || !HashValid(D.PointIds.SHA256) ||
        !O->TryGetObjectField(TEXT("sourceBounds"), SourceBounds) ||
        !Bounds(**SourceBounds, TEXT("min"), D.SpatialDimensions, D.SourceBounds.Min) ||
        !Bounds(**SourceBounds, TEXT("max"), D.SpatialDimensions, D.SourceBounds.Max))
        return Fail(TEXT("Invalid source coordinates, point IDs or array shape."));
    for (int32 I = 0; I < Dimensions; ++I)
        if (D.SourceBounds.Min[I] > D.SourceBounds.Max[I]) return Fail(TEXT("Source bounds are reversed."));
    D.SourceBounds.IsValid = 1; D.PointIds.ByteLength = Points * sizeof(int64); D.PointIds.SHA256.ToLowerInline();
    const TArray<TSharedPtr<FJsonValue>>* Timeline = nullptr;
    if (!O->TryGetArrayField(TEXT("frames"), Timeline) || Timeline->Num() != Frames) return Fail(TEXT("Original frame timeline is incomplete."));
    TSet<FString> Labels;
    for (const auto& Value : *Timeline)
    {
        if (Cancelled(Cancel)) return Fail(TEXT("Point recording load cancelled."));
        const TSharedPtr<FJsonObject>* F = nullptr; FStudioFrame Frame; int64 Index; FString Label;
        if (!Value->TryGetObject(F) || !Integer(**F, TEXT("index"), 0, MAX_int32, Index) ||
            !String(**F, TEXT("label"), Label, 128) || Labels.Contains(Label) ||
            !(*F)->TryGetNumberField(TEXT("time"), Frame.Time) || !FMath::IsFinite(Frame.Time) ||
            (!D.Frames.IsEmpty() && (Frame.Time <= D.Frames.Last().Time || Index <= D.Frames.Last().Index)))
            return Fail(TEXT("Frames require unique labels and strictly increasing original steps and finite times."));
        Frame.Index = int32(Index); D.Frames.Add(Frame); D.FrameLabels.Add(Label); Labels.Add(Label);
    }
    const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
    if (!O->TryGetArrayField(TEXT("fields"), Fields) || Fields->IsEmpty() || Fields->Num() > MaxFields)
        return Fail(TEXT("Point recording field count is unsupported."));
    TSet<FString> Names, Paths, Components;
    Paths.Add(TEXT("recording.json")); Paths.Add(TEXT("provenance.json")); Paths.Add(TEXT("attribution.txt"));
    for (const FString& Path : {D.Coordinates.Path, D.PointIds.Path})
    { if (Paths.Contains(Path.ToLower())) return Fail(TEXT("Recording array paths must be distinct.")); Paths.Add(Path.ToLower()); }
    TMap<FString, FString> VectorUnits;
    for (const auto& Value : *Fields)
    {
        if (Cancelled(Cancel)) return Fail(TEXT("Point recording load cancelled."));
        const TSharedPtr<FJsonObject>* F = nullptr; const TSharedPtr<FJsonObject>* A = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Range = nullptr;
        FStudioPointFieldDescriptor Field; FString Association;
        if (!Value->TryGetObject(F) || !String(**F, TEXT("id"), Field.Id, 128) || !Identifier(Field.Id) || Names.Contains(Field.Id) ||
            !String(**F, TEXT("label"), Field.Label, 256) || !String(**F, TEXT("unit"), Field.Unit, 128) ||
            !String(**F, TEXT("association"), Association) || Association != TEXT("point") ||
            !String(**F, TEXT("origin"), Field.Origin) || (Field.Origin != TEXT("source") && Field.Origin != TEXT("derived")) ||
            !(*F)->TryGetBoolField(TEXT("static"), Field.bStatic) || !(*F)->TryGetArrayField(TEXT("range"), Range) || Range->Num() != 2 ||
            !(*Range)[0]->TryGetNumber(Field.Minimum) || !(*Range)[1]->TryGetNumber(Field.Maximum) ||
            !FMath::IsFinite(Field.Minimum) || !FMath::IsFinite(Field.Maximum) || Field.Minimum > Field.Maximum ||
            !(*F)->TryGetObjectField(TEXT("array"), A) ||
            !Array(**A, Field.bStatic ? TArray<int64>{Points} : TArray<int64>{Frames, Points}, Field.Array) ||
            Paths.Contains(Field.Array.Path.ToLower())) return Fail(TEXT("Fields require distinct IDs/paths, explicit units/origin, point association and valid ranges/shapes."));
        if (Field.Origin == TEXT("derived") && !String(**F, TEXT("expression"), Field.Expression))
            return Fail(TEXT("Derived fields require their calculation."));
        const bool bVector = (*F)->HasField(TEXT("vector")), bComponent = (*F)->HasField(TEXT("component"));
        if (bVector || bComponent)
        {
            if (!bVector || !bComponent || !String(**F, TEXT("vector"), Field.Vector, 128) || !Identifier(Field.Vector) ||
                !String(**F, TEXT("component"), Field.Component, 1) ||
                (Field.Component != TEXT("x") && Field.Component != TEXT("y") && !(Dimensions == 3 && Field.Component == TEXT("z"))))
                return Fail(TEXT("Invalid vector component association."));
            const FString Key = Field.Vector + TEXT("/") + Field.Component;
            if (Components.Contains(Key) || (VectorUnits.Contains(Field.Vector) && VectorUnits[Field.Vector] != Field.Unit))
                return Fail(TEXT("Vector components must be unique and use the same units."));
            Components.Add(Key); VectorUnits.Add(Field.Vector, Field.Unit);
        }
        if (!Field.bStatic)
        {
            const TArray<TSharedPtr<FJsonValue>>* CRCs = nullptr;
            if (!(*A)->TryGetArrayField(TEXT("frameCRC32"), CRCs) || CRCs->Num() != Frames)
                return Fail(TEXT("Dynamic arrays require a checksum for every original frame."));
            for (const auto& CRC : *CRCs)
            {
                double N;
                if (!CRC->TryGetNumber(N) || !FMath::IsFinite(N) || N < 0 || N > MAX_uint32 || N != FMath::FloorToDouble(N))
                    return Fail(TEXT("Invalid frame checksum."));
                Field.Array.FrameCRC32.Add(uint32(N));
            }
        }
        else if ((*A)->HasField(TEXT("frameCRC32"))) return Fail(TEXT("Static arrays cannot carry dynamic frame checksums."));
        Paths.Add(Field.Array.Path.ToLower()); Names.Add(Field.Id); D.Fields.Add(MoveTemp(Field));
    }
    if (!D.FindField(D.DefaultScalar)) return Fail(TEXT("Default scalar is not supplied by this recording."));
    const TArray<TSharedPtr<FJsonValue>>* Notes = nullptr;
    if (!O->TryGetArrayField(TEXT("limitations"), Notes) || Notes->Num() > 32) return Fail(TEXT("Recording limitations are missing or invalid."));
    for (const auto& Value : *Notes)
    { FString Note; if (!Value->TryGetString(Note) || !Clean(Note, 2048)) return Fail(TEXT("Invalid recording limitation.")); D.Limitations.Add(Note); }
    return !Cancelled(Cancel) || Fail(TEXT("Point recording load cancelled."));
}

/** The hash is always calculated on the source little-endian bytes, before host conversion. */
bool ReadBytes(const FString& Path, int64 TotalBytes, int64 Offset, uint8* Destination, int64 Count,
    const FStudioLoadCancellation& Cancel, uint32* CRC, FString* SHA, FString& Error)
{
    FStudioFileAccess Access(Path);
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!File || File->TotalSize() != TotalBytes || Offset < 0 || Count < 0 || Offset > TotalBytes - Count)
    { Error = TEXT("Recording array is missing or its size changed."); return false; }
    File->Seek(Offset); uint32 Checksum = 0;
    for (int64 Done = 0; Done < Count;)
    {
        if (Cancelled(Cancel)) { Error = TEXT("Point recording read cancelled."); return false; }
        const int32 Chunk = int32(FMath::Min<int64>(65536, Count - Done));
        File->Serialize(Destination + Done, Chunk);
        if (File->IsError()) { Error = TEXT("Recording array could not be read completely."); return false; }
        if (CRC) Checksum = FCrc::MemCrc32(Destination + Done, Chunk, Checksum);
        Done += Chunk;
    }
    if (CRC) *CRC = Checksum;
    if (SHA && !Digest(Destination, Count, *SHA)) { Error = TEXT("Cannot verify recording array checksum."); return false; }
    if (Cancelled(Cancel)) { Error = TEXT("Point recording read cancelled."); return false; }
    return true;
}
template<typename T> void FromLittleEndian(TArray<T>& Values)
{
    static_assert(sizeof(T) == 8);
#if !PLATFORM_LITTLE_ENDIAN
    for (T& Value : Values)
    {
        uint8* Bytes = reinterpret_cast<uint8*>(&Value);
        for (int32 I = 0; I < 4; ++I) Swap(Bytes[I], Bytes[7 - I]);
    }
#endif
}
}

struct FStudioPointMemory
{
    std::atomic<int64> LiveBytes{0}, PeakBytes{0};
};

FStudioPointValues::FStudioPointValues(int32 Count, const TSharedRef<FStudioPointMemory, ESPMode::ThreadSafe>& InMemory) : Memory(InMemory)
{
    Values.Reserve(Count); Values.SetNumUninitialized(Count);
    ++StudioPointPrivate::LiveArrays; StudioPointPrivate::LiveAllocatedValueBytes += Values.GetAllocatedSize();
    // These counters count scalar value storage, not allocator or object overhead.
    const int64 Live = Memory->LiveBytes.fetch_add(int64(Count) * sizeof(double)) + int64(Count) * sizeof(double);
    int64 Peak = Memory->PeakBytes.load();
    while (Live > Peak && !Memory->PeakBytes.compare_exchange_weak(Peak, Live)) {}
}
FStudioPointValues::~FStudioPointValues()
{
    --StudioPointPrivate::LiveArrays; StudioPointPrivate::LiveAllocatedValueBytes -= Values.GetAllocatedSize();
    Memory->LiveBytes -= int64(Values.Num()) * sizeof(double);
}
FStudioPointFrame::FStudioPointFrame() { ++StudioPointPrivate::LiveSnapshots; }
FStudioPointFrame::~FStudioPointFrame() { --StudioPointPrivate::LiveSnapshots; }
FStudioPointLiveStats StudioPointRecordings::LiveStats()
{
    using namespace StudioPointPrivate;
    return {LiveReaders.load(), LiveSnapshots.load(), LiveArrays.load(), LiveAllocatedValueBytes.load()};
}

struct FStudioPointRecordingData
{
    FStudioPointRecordingData() { ++StudioPointPrivate::LiveReaders; }
    ~FStudioPointRecordingData() { --StudioPointPrivate::LiveReaders; }
    FString Folder;
    TSharedRef<FStudioPointRecordingDescriptor, ESPMode::ThreadSafe> Meta = MakeShared<FStudioPointRecordingDescriptor, ESPMode::ThreadSafe>();
    TSharedRef<FStudioPointGeometry, ESPMode::ThreadSafe> Geometry = MakeShared<FStudioPointGeometry, ESPMode::ThreadSafe>();
    TSharedRef<FStudioPointMemory, ESPMode::ThreadSafe> Memory = MakeShared<FStudioPointMemory, ESPMode::ThreadSafe>();
    FStudioPointReadOptions Options;
    struct FEntry { TSharedPtr<const FStudioPointValues, ESPMode::ThreadSafe> Values; uint64 Use = 0; };
    mutable FCriticalSection ReadMutex, CacheMutex;
    mutable TMap<FString, FEntry> Cache;
    mutable uint64 Clock = 0, Loads = 0, Hits = 0;
    int64 ArrayBytes() const { return int64(Meta->PointCount) * sizeof(double); }
    // Caller owns CacheMutex. Snapshots can retain evicted arrays; their memory stays counted.
    bool EvictOldest() const
    {
        FString Key; uint64 Use = MAX_uint64;
        for (const auto& Pair : Cache) if (Pair.Value.Use < Use) { Key = Pair.Key; Use = Pair.Value.Use; }
        return !Key.IsEmpty() && Cache.Remove(Key) != 0;
    }
};

const FStudioPointFieldDescriptor* FStudioPointRecordingDescriptor::FindField(const FString& FieldId) const
{ return Fields.FindByPredicate([&](const auto& F) { return F.Id == FieldId; }); }
const TArray<double>* FStudioPointFrame::FindValues(const FString& FieldId) const
{ const auto* V = Fields.Find(FieldId); return V ? &(*V)->Values : nullptr; }
FStudioPointRecording::FStudioPointRecording(TSharedRef<FStudioPointRecordingData, ESPMode::ThreadSafe> InData) : Data(MoveTemp(InData)) {}
const FStudioPointRecordingDescriptor& FStudioPointRecording::Descriptor() const { return *Data->Meta; }
TSharedRef<const FStudioPointGeometry, ESPMode::ThreadSafe> FStudioPointRecording::Geometry() const { return Data->Geometry; }
FStudioPointReadStats FStudioPointRecording::Stats() const
{
    FScopeLock Lock(&Data->CacheMutex); FStudioPointReadStats S;
    S.CacheBudgetBytes = Data->Options.CacheBytes;
    S.CachedArrays = Data->Cache.Num(); S.CacheBytes = S.CachedArrays * Data->ArrayBytes();
    S.LiveArrayBytes = Data->Memory->LiveBytes.load(); S.PeakLiveArrayBytes = Data->Memory->PeakBytes.load();
    S.GeometryBytes = Data->Geometry->Positions.GetAllocatedSize() + Data->Geometry->PointIds.GetAllocatedSize();
    S.Loads = Data->Loads; S.Hits = Data->Hits; return S;
}

FStudioPointOpenResult StudioPointRecordings::Open(const FString& Path, const FStudioPointReadOptions& Options,
    const FStudioLoadCancellation& Cancellation, const FString& ExpectedMetadataSHA256)
{
    using namespace StudioPointPrivate;
    FStudioPointOpenResult Result;
    auto Fail = [&](const FString& Error) { Result.Error = Cancelled(Cancellation) ? TEXT("Point recording load cancelled.") : Error; return Result; };
    if (Cancelled(Cancellation)) return Fail(TEXT("Point recording load cancelled."));
    if (!Clean(Path, 4096) || Path.Contains(TEXT("://")) || FPaths::GetCleanFilename(Path) != TEXT("recording.json"))
        return Fail(TEXT("Choose the local recording.json descriptor."));
    if (Options.CacheBytes < 0 || Options.LiveArrayBytes < 8 || Options.LiveArrayBytes > 1024LL * 1024 * 1024 ||
        Options.CacheBytes > Options.LiveArrayBytes || Options.GeometryBytes < 40 || Options.GeometryBytes > 1024LL * 1024 * 1024)
        return Fail(TEXT("Invalid point recording memory budgets."));
    auto Data = MakeShared<FStudioPointRecordingData, ESPMode::ThreadSafe>();
    Data->Folder = FPaths::GetPath(FPaths::ConvertRelativePathToFull(Path)); Data->Options = Options;
    TArray<uint8> Bytes; FString ProvenanceHash, AttributionHash;
    if (!SmallFile(Path, MaxMetadataBytes, Bytes, Cancellation)) return Fail(TEXT("Point recording metadata is missing or exceeds 8 MiB."));
    if (!Digest(Bytes.GetData(), Bytes.Num(), Data->Meta->MetadataSHA256)) return Fail(TEXT("Cannot verify recording metadata."));
    if (!ExpectedMetadataSHA256.IsEmpty() && (!HashValid(ExpectedMetadataSHA256) ||
        !Data->Meta->MetadataSHA256.Equals(ExpectedMetadataSHA256, ESearchCase::IgnoreCase)))
        return Fail(TEXT("Recording metadata differs from the saved source. Locate an exact copy."));
    if (!Parse(Bytes, *Data->Meta, ProvenanceHash, AttributionHash, Cancellation, Result.Error)) return Fail(Result.Error);
    Bytes.Empty();
    // Includes 3 doubles + int64 per point and a sorted ID copy used only during validation.
    if (int64(Data->Meta->PointCount) * 40 > Options.GeometryBytes || Data->ArrayBytes() > Options.LiveArrayBytes)
        return Fail(TEXT("Recording geometry or one scalar frame exceeds its memory budget."));
    const auto Cancel = Cancellation.IsValid() ? Cancellation.ToSharedRef() : MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    for (const auto& File : {TPair<FString, FString>(TEXT("provenance.json"), ProvenanceHash),
                            TPair<FString, FString>(TEXT("ATTRIBUTION.txt"), AttributionHash)})
    {
        FString Hash;
        if (!SmallFile(Data->Folder / File.Key, 256 * 1024, Bytes, Cancel) || !Digest(Bytes.GetData(), Bytes.Num(), Hash) ||
            !Hash.Equals(File.Value, ESearchCase::IgnoreCase)) return Fail(TEXT("Recording provenance or attribution is missing or changed."));
    }
    Bytes.Empty();
    // Verify the complete source once without retaining its time series in memory.
    TArray<FStudioPointArrayDescriptor> Arrays{Data->Meta->Coordinates, Data->Meta->PointIds};
    for (const auto& Field : Data->Meta->Fields) Arrays.Add(Field.Array);
    for (const auto& A : Arrays)
    {
        FString Hash;
        const FString File = Data->Folder / A.Path;
        if (IFileManager::Get().FileSize(*File) != A.ByteLength) return Fail(TEXT("Recording array byte length differs from metadata."));
        if (!StudioAssets::HashFile(File, Cancel, Hash, Result.Error)) return Fail(Result.Error);
        if (Hash != A.SHA256) return Fail(TEXT("Recording array SHA-256 differs from metadata. Obtain an intact source."));
    }
    auto& G = *Data->Geometry; const auto& D = *Data->Meta;
    // Read coordinates directly into final storage one point at a time, avoiding a duplicate coordinate array.
    G.Positions.Reserve(D.PointCount); G.Positions.SetNumUninitialized(D.PointCount);
    FStudioFileAccess CoordinateAccess(Data->Folder / D.Coordinates.Path);
    TUniquePtr<FArchive> Coordinates(IFileManager::Get().CreateFileReader(*(Data->Folder / D.Coordinates.Path), FILEREAD_Silent));
    if (!Coordinates || Coordinates->TotalSize() != D.Coordinates.ByteLength) return Fail(TEXT("Coordinates changed while opening."));
    // Rehash the exact coordinate bytes retained below, closing the hash/read race.
    struct FHashContext { EVP_MD_CTX* P = EVP_MD_CTX_new(); ~FHashContext() { EVP_MD_CTX_free(P); } } HashContext;
    if (!HashContext.P || EVP_DigestInit_ex(HashContext.P, EVP_sha256(), nullptr) != 1) return Fail(TEXT("Cannot verify retained coordinates."));
    FBox ActualBounds(ForceInit);
    for (int32 I = 0; I < D.PointCount; ++I)
    {
        if ((I & 1023) == 0 && Cancelled(Cancel)) return Fail(TEXT("Point recording load cancelled."));
        double Point[3] = {0, 0, 0}; Coordinates->Serialize(Point, D.SpatialDimensions * sizeof(double));
        if (Coordinates->IsError() || EVP_DigestUpdate(HashContext.P, Point, D.SpatialDimensions * sizeof(double)) != 1)
            return Fail(TEXT("Cannot read retained coordinates."));
#if !PLATFORM_LITTLE_ENDIAN
        for (int32 Axis = 0; Axis < D.SpatialDimensions; ++Axis)
        { auto* P = reinterpret_cast<uint8*>(&Point[Axis]); for (int32 J = 0; J < 4; ++J) Swap(P[J], P[7 - J]); }
#endif
        const FVector Position(Point[0], Point[1], Point[2]);
        if (Position.ContainsNaN()) return Fail(TEXT("Source coordinates must be finite."));
        G.Positions[I] = Position; ActualBounds += Position;
    }
    uint8 CoordinateDigest[EVP_MAX_MD_SIZE]; unsigned int DigestLength = 0;
    if (EVP_DigestFinal_ex(HashContext.P, CoordinateDigest, &DigestLength) != 1 || DigestLength != 32 ||
        BytesToHex(CoordinateDigest, DigestLength).ToLower() != D.Coordinates.SHA256)
        return Fail(TEXT("Coordinates changed during loading."));
    if (ActualBounds.Min != D.SourceBounds.Min || ActualBounds.Max != D.SourceBounds.Max)
        return Fail(TEXT("Declared source bounds do not match original points."));
    G.PointIds.Reserve(D.PointCount); G.PointIds.SetNumUninitialized(D.PointCount); FString IdHash;
    if (!ReadBytes(Data->Folder / D.PointIds.Path, D.PointIds.ByteLength, 0, reinterpret_cast<uint8*>(G.PointIds.GetData()),
        D.PointIds.ByteLength, Cancel, nullptr, &IdHash, Result.Error)) return Fail(Result.Error);
    if (IdHash != D.PointIds.SHA256) return Fail(TEXT("Point IDs changed during loading."));
    FromLittleEndian(G.PointIds);
    {
        TArray<int64> SortedIds = G.PointIds; SortedIds.Sort();
        for (int32 I = 1; I < SortedIds.Num(); ++I)
            if (SortedIds[I] == SortedIds[I - 1]) return Fail(TEXT("Original point IDs must be unique."));
    }
    // The interpretation must still be the descriptor whose member checksums were verified.
    FString FinalMetadataHash;
    if (!SmallFile(Path, MaxMetadataBytes, Bytes, Cancel) || !Digest(Bytes.GetData(), Bytes.Num(), FinalMetadataHash) || FinalMetadataHash != D.MetadataSHA256)
        return Fail(TEXT("Recording metadata changed during opening."));
    if (Cancelled(Cancel)) return Fail(TEXT("Point recording load cancelled."));
    Result.Recording = MakeShared<FStudioPointRecording, ESPMode::ThreadSafe>(MoveTemp(Data)); return Result;
}

FStudioPointReadResult FStudioPointRecording::ReadFrame(int32 Ordinal, const TArray<FString>& FieldIds,
    const FStudioLoadCancellation& Cancellation) const
{
    using namespace StudioPointPrivate;
    FStudioPointReadResult Result;
    auto Fail = [&](const TCHAR* Error) { Result.Error = Error; return Result; };
    if (Cancelled(Cancellation)) return Fail(TEXT("Point recording read cancelled."));
    if (!Data->Meta->Frames.IsValidIndex(Ordinal) || FieldIds.IsEmpty() || FieldIds.Num() > MaxFields)
        return Fail(TEXT("Choose an available original frame and at least one supplied field."));
    TSet<FString> Seen;
    for (const FString& Id : FieldIds)
    {
        if (!Data->Meta->FindField(Id) || Seen.Contains(Id)) return Fail(TEXT("Requested field is absent or duplicated."));
        Seen.Add(Id);
    }
    if (int64(FieldIds.Num()) * Data->ArrayBytes() > Data->Options.LiveArrayBytes)
        return Fail(TEXT("Requested fields exceed the live scalar-array budget."));
    // A cancelled queued request never starts another allocation or disk read.
    while (!Data->ReadMutex.TryLock())
    { if (Cancelled(Cancellation)) return Fail(TEXT("Point recording read cancelled.")); FPlatformProcess::SleepNoStats(.001f); }
    struct FUnlock { FCriticalSection& Mutex; ~FUnlock() { Mutex.Unlock(); } } Unlock{Data->ReadMutex};
    auto Frame = MakeShared<FStudioPointFrame, ESPMode::ThreadSafe>();
    Frame->Ordinal = Ordinal; Frame->Descriptor = Data->Meta; Frame->Geometry = Data->Geometry;
    for (const FString& Id : FieldIds)
    {
        if (Cancelled(Cancellation)) return Fail(TEXT("Point recording read cancelled."));
        const auto& Field = *Data->Meta->FindField(Id);
        const FString Key = Id + TEXT("/") + LexToString(Field.bStatic ? -1 : Ordinal);
        {
            FScopeLock Lock(&Data->CacheMutex);
            if (auto* Cached = Data->Cache.Find(Key))
            { Cached->Use = ++Data->Clock; ++Data->Hits; Frame->Fields.Add(Id, Cached->Values); continue; }
            while (Data->Memory->LiveBytes.load() + Data->ArrayBytes() > Data->Options.LiveArrayBytes && Data->EvictOldest()) {}
            if (Data->Memory->LiveBytes.load() + Data->ArrayBytes() > Data->Options.LiveArrayBytes)
                return Fail(TEXT("Live scalar-array budget is held by snapshots. Release an old frame before retrying."));
        }
        auto Values = MakeShared<FStudioPointValues, ESPMode::ThreadSafe>(Data->Meta->PointCount, Data->Memory);
        uint32 CRC = 0; FString SHA;
        if (!ReadBytes(Data->Folder / Field.Array.Path, Field.Array.ByteLength, Field.bStatic ? 0 : int64(Ordinal) * Data->ArrayBytes(),
            reinterpret_cast<uint8*>(Values->Values.GetData()), Data->ArrayBytes(), Cancellation, Field.bStatic ? nullptr : &CRC,
            Field.bStatic ? &SHA : nullptr, Result.Error)) return Result;
        if (Field.bStatic ? SHA != Field.Array.SHA256 : CRC != Field.Array.FrameCRC32[Ordinal])
            return Fail(TEXT("Source field frame failed its checksum. Obtain the intact recording."));
        FromLittleEndian(Values->Values);
        for (int32 I = 0; I < Values->Values.Num(); ++I)
        {
            if ((I & 4095) == 0 && Cancelled(Cancellation)) return Fail(TEXT("Point recording read cancelled."));
            const double V = Values->Values[I];
            if (!FMath::IsFinite(V) || V < Field.Minimum || V > Field.Maximum)
                return Fail(TEXT("Source field contains a nonfinite value or exceeds its declared range."));
        }
        Frame->Fields.Add(Id, Values);
        FScopeLock Lock(&Data->CacheMutex); ++Data->Loads;
        if (Data->ArrayBytes() <= Data->Options.CacheBytes)
        {
            while ((Data->Cache.Num() + 1) * Data->ArrayBytes() > Data->Options.CacheBytes && Data->EvictOldest()) {}
            Data->Cache.Add(Key, {Values, ++Data->Clock});
        }
    }
    if (Cancelled(Cancellation)) return Fail(TEXT("Point recording read cancelled."));
    Result.Frame = MoveTemp(Frame); return Result;
}
