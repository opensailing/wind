#include "StudioModel.h"
#include "StudioSegmentCoverage.h"
#include "StudioPointRecording.h"
#include "StudioFileDialog.h"
#include "CompGeom/PolygonTriangulation.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/Crc.h"
#include "HAL/FileManager.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace
{
std::atomic<int32> LiveRecordingReaders{0},LiveRecordingFrames{0};
std::atomic<int64> LiveRecordingFrameBytes{0};
}
FStudioRecordingLiveStats StudioRecordings::LiveStats()
{
    const auto Points=StudioPointRecordings::LiveStats();
    return {LiveRecordingReaders.load()+Points.Readers,LiveRecordingFrames.load()+Points.Snapshots,
        LiveRecordingFrameBytes.load()+Points.AllocatedValueBytes};
}

struct FRecordedFlowData
{
    struct FValues { float U,V,Pressure,Density; };
    struct FFrame
    {
        explicit FFrame(int32 Count) { Values.SetNumZeroed(Count);++LiveRecordingFrames;LiveRecordingFrameBytes+=Values.GetAllocatedSize(); }
        ~FFrame() { --LiveRecordingFrames;LiveRecordingFrameBytes-=Values.GetAllocatedSize(); }
        TArray<FValues> Values;
    };
    FRecordedFlowData() { ++LiveRecordingReaders; }
    ~FRecordedFlowData() { --LiveRecordingReaders; }
    struct FCacheEntry { TSharedPtr<const FFrame,ESPMode::ThreadSafe> Frame; uint64 Use=0; };
    FStudioRecordingDescriptor Meta;
    FString Path;
    TArray<FVector2D> Nodes, Outline;
    TArray<FIntVector> Triangles;
    TArray<FIntVector> BoundaryCaps;
    TArray<int64> Offsets;
    TArray<uint32> Checksums;
    uint32 MeshChecksum=0;
    static constexpr int32 NX=128, NZ=64;
    TArray<TArray<int32>> Bins;
    mutable FCriticalSection Mutex;
    mutable TMap<int32,FCacheEntry> Cache;
    mutable FString ReadError;
    mutable uint64 Clock=0, Loads=0, Hits=0;
    int64 Budget=0;
    int64 FrameBytes() const { return int64(Nodes.Num())*sizeof(FValues); }
    int32 XBin(double X) const { return FMath::Clamp(FMath::FloorToInt((X-Meta.DisplayBounds.Min.X)/Meta.DisplayBounds.GetSize().X*NX),0,NX-1); }
    int32 ZBin(double Z) const { return FMath::Clamp(FMath::FloorToInt((Z-Meta.DisplayBounds.Min.Z)/Meta.DisplayBounds.GetSize().Z*NZ),0,NZ-1); }
    bool BuildBins(const FStudioLoadCancellation& Cancellation)
    {
        Bins.SetNum(NX*NZ); int64 Entries=0;
        for(int32 I=0;I<Triangles.Num();++I)
        {
            if((I&255)==0 && Cancellation && Cancellation->load()) return false;
            const auto T=Triangles[I]; const auto A=Nodes[T.X],B=Nodes[T.Y],C=Nodes[T.Z];
            const double X0=FMath::Min3(A.X,B.X,C.X),X1=FMath::Max3(A.X,B.X,C.X);
            const double Z0=FMath::Min3(A.Y,B.Y,C.Y),Z1=FMath::Max3(A.Y,B.Y,C.Y);
            if(X1<Meta.DisplayBounds.Min.X||X0>Meta.DisplayBounds.Max.X||Z1<Meta.DisplayBounds.Min.Z||Z0>Meta.DisplayBounds.Max.Z) continue;
            Entries+=int64(ZBin(Z1)-ZBin(Z0)+1)*(XBin(X1)-XBin(X0)+1);
            if(Entries>32LL*1024*1024) return false;
            for(int32 Z=ZBin(Z0);Z<=ZBin(Z1);++Z) for(int32 X=XBin(X0);X<=XBin(X1);++X) Bins[Z*NX+X].Add(I);
        }
        return true;
    }
    TSharedPtr<const FFrame,ESPMode::ThreadSafe> ReadFrame(int32 Ordinal, const FStudioLoadCancellation& Cancellation = {},FString* AnalysisError = nullptr) const
    {
        if(AnalysisError)AnalysisError->Empty();
        auto Fail=[&](const FString& Message)
        {
            if(AnalysisError)*AnalysisError=Message;
            else {FScopeLock Lock(&Mutex);ReadError=Message;}
        };
        if(Cancellation && Cancellation->load()) return nullptr;
        if(!Offsets.IsValidIndex(Ordinal)){if(AnalysisError)*AnalysisError=TEXT("Frame ordinal is outside the recording.");return nullptr;}
        {
            FScopeLock Lock(&Mutex);
            if(auto* Entry=Cache.Find(Ordinal)) { Entry->Use=++Clock; ++Hits; return Entry->Frame; }
        }
        // Each worker owns its archive; neither a file cursor nor a mutable
        // frame is shared with the renderer. No file parsing occurs in Sample.
        FStudioFileAccess Access(Path);
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
        if(!Reader) { Fail(TEXT("Recording file is unavailable. Relink or reopen the dataset.")); return nullptr; }
        Reader->Seek(Offsets[Ordinal]);
        auto Frame=MakeShared<FFrame,ESPMode::ThreadSafe>(Nodes.Num());
        uint32 CRC=0;
        for(int64 Offset=0;Offset<FrameBytes();)
        {
            if(Cancellation && Cancellation->load()) return nullptr;
            const int32 Count=int32(FMath::Min<int64>(65536,FrameBytes()-Offset));
            auto* Bytes=reinterpret_cast<uint8*>(Frame->Values.GetData())+Offset;
            Reader->Serialize(Bytes,Count); CRC=FCrc::MemCrc32(Bytes,Count,CRC); Offset+=Count;
        }
        bool Valid=!Reader->IsError()&&CRC==Checksums[Ordinal];
        int32 Validated=0;
        for(const auto& V:Frame->Values)
        {
            if((Validated++&4095)==0 && Cancellation && Cancellation->load()) return nullptr;
            if(!FMath::IsFinite(V.U)||!FMath::IsFinite(V.V)||!FMath::IsFinite(V.Pressure)||!FMath::IsFinite(V.Density)||V.Density<=0) { Valid=false; break; }
            if(Meta.Scalars.Num()==5)
            {
                const double Values[]={FMath::Sqrt(double(V.U)*V.U+double(V.V)*V.V),V.U,V.V,V.Pressure,V.Density};
                for(int32 I=0;I<5;++I)
                {
                    const auto& Range=Meta.Scalars[I];
                    const double Tolerance=I==0?1.e-12*FMath::Max(1.,FMath::Abs(Range.Maximum)):0.;
                    if(Values[I]<Range.Minimum-Tolerance||Values[I]>Range.Maximum+Tolerance) {Valid=false;break;}
                }
                if(!Valid)break;
            }
        }
        if(Cancellation && Cancellation->load()) return nullptr;
        if(!Valid) { Fail(FString::Printf(TEXT("Source frame %d failed integrity/value validation. Reimport the recording."),Meta.Frames[Ordinal].Index)); return nullptr; }
        FScopeLock Lock(&Mutex);
        if(auto* Existing=Cache.Find(Ordinal)) { Existing->Use=++Clock; ++Hits; return Existing->Frame; }
        while((Cache.Num()+1)*FrameBytes()>Budget)
        {
            int32 Oldest=INDEX_NONE; uint64 Use=MAX_uint64;
            for(const auto& Entry:Cache) if(Entry.Value.Use<Use) { Oldest=Entry.Key; Use=Entry.Value.Use; }
            if(Oldest==INDEX_NONE) break;
            Cache.Remove(Oldest);
        }
        ++Loads; if(!AnalysisError)ReadError.Empty(); Cache.Add(Ordinal,{Frame,++Clock}); return Frame;
    }
};
namespace
{
bool ReadDescriptor(const FString& Path,FStudioRecordingDescriptor& M,TArray<uint32>& Checksums,uint32& MeshChecksum,FString& Error,
    const FStudioLoadCancellation& Cancellation)
{
    TUniquePtr<FArchive> Metadata(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    const int64 Size=Metadata?Metadata->TotalSize():-1;
    FString Text; TSharedPtr<FJsonObject> O;TArray<uint8> Bytes;
    if(Size<0||Size>4*1024*1024)
    { Error=TEXT("Recording metadata is missing or invalid. Reimport the dataset."); return false; }
    Bytes.SetNumUninitialized(int32(Size));Metadata->Serialize(Bytes.GetData(),Size);
    if(Metadata->IsError())
    {Error=TEXT("Recording metadata could not be read completely.");return false;}
    // Hash the same bytes that are parsed, retaining BOM/encoding identity.
    uint8 Digest[EVP_MAX_MD_SIZE];unsigned int Count=0;
    if(EVP_Digest(Bytes.GetData(),Bytes.Num(),Digest,&Count,EVP_sha256(),nullptr)!=1||Count!=32)
    {Error=TEXT("Cannot verify recording metadata.");return false;}
    M.MetadataSHA256=BytesToHex(Digest,Count).ToLower();
    FFileHelper::BufferToString(Text,Bytes.GetData(),Bytes.Num());
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O)||!O)
    {Error=TEXT("Recording metadata is missing or invalid. Reimport the dataset.");return false;}
    auto String=[&](const TCHAR* Key,FString& Out,int32 Max=2048)
    { return O->TryGetStringField(Key,Out)&&!Out.IsEmpty()&&Out.Len()<=Max; };
    auto Vector=[&](const TCHAR* Key,FVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
        if(!O->TryGetArrayField(Key,A)||A->Num()!=3) return false;
        for(int32 I=0;I<3;++I) if(!(*A)[I]->TryGetNumber(Out[I])||!FMath::IsFinite(Out[I])||FMath::Abs(Out[I])>1e9) return false;
        return true;
    };
    double Version=0,Dimensions=0; FString Unit,Hash;
    bool Valid=O->TryGetNumberField(TEXT("version"),Version)&&Version==1&&
        O->TryGetNumberField(TEXT("spatialDimensions"),Dimensions)&&Dimensions==2&&
        String(TEXT("id"),M.Id,256)&&String(TEXT("title"),M.Title,256)&&
        String(TEXT("sourceLabel"),M.SourceLabel,256)&&!M.SourceLabel.Contains(TEXT(","))&&!M.SourceLabel.Contains(TEXT("\n"))&&!M.SourceLabel.Contains(TEXT("\r"))&&
        String(TEXT("sourceURL"),M.SourceURL)&&String(TEXT("timeNote"),M.TimeNote)&&String(TEXT("fieldNote"),M.FieldNote)&&
        Vector(TEXT("sourceOffset"),M.SourceOffset)&&Vector(TEXT("displayMin"),M.DisplayBounds.Min)&&Vector(TEXT("displayMax"),M.DisplayBounds.Max)&&
        String(TEXT("coordinateUnit"),Unit)&&Unit==TEXT("m")&&String(TEXT("velocityUnit"),Unit)&&Unit==TEXT("m/s")&&
        String(TEXT("pressureUnit"),Unit)&&Unit==TEXT("Pa")&&String(TEXT("densityUnit"),Unit)&&Unit==TEXT("kg/m3")&&
        String(TEXT("payloadSHA256"),Hash,64)&&Hash.Len()==64;
    for(TCHAR C:Hash) Valid&=FChar::IsHexDigit(C);
    M.PayloadSHA256=Hash.ToLower();
    if(!Valid||M.DisplayBounds.GetSize().GetMin()<=1e-9||M.SourceOffset.Y!=0)
    { Error=TEXT("Unsupported recording metadata. This reader requires 2D nodal SI fields and valid display bounds."); return false; }
    M.DisplayBounds.IsValid=1;
    if(O->HasField(TEXT("scalars")))
    {
        const TArray<TSharedPtr<FJsonValue>>* Scalars=nullptr;
        if(!O->TryGetArrayField(TEXT("scalars"),Scalars)||Scalars->Num()!=5)
        {Error=TEXT("Recording scalar metadata must describe all five supported fields.");return false;}
        const TCHAR* Ids[]={TEXT("velocity_magnitude"),TEXT("velocity_x"),TEXT("velocity_y"),TEXT("pressure"),TEXT("density")};
        const TCHAR* Units[]={TEXT("m/s"),TEXT("m/s"),TEXT("m/s"),TEXT("Pa"),TEXT("kg/m3")};
        M.Scalars.Reset();
        for(int32 I=0;I<5;++I)
        {
            const TSharedPtr<FJsonObject>* Field=nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Range=nullptr;
            FStudioScalarDescriptor Scalar;
            if(!(*Scalars)[I]->TryGetObject(Field)||!(*Field)->TryGetStringField(TEXT("id"),Scalar.Id)||Scalar.Id!=Ids[I]||
                !(*Field)->TryGetStringField(TEXT("label"),Scalar.Label)||Scalar.Label.IsEmpty()||Scalar.Label.Len()>128||
                !(*Field)->TryGetStringField(TEXT("unit"),Scalar.Unit)||Scalar.Unit!=Units[I]||
                !(*Field)->TryGetStringField(TEXT("origin"),Scalar.Origin)||Scalar.Origin!=(I<3?TEXT("derived"):TEXT("source"))||
                !(*Field)->TryGetArrayField(TEXT("range"),Range)||Range->Num()!=2||
                !(*Range)[0]->TryGetNumber(Scalar.Minimum)||!(*Range)[1]->TryGetNumber(Scalar.Maximum)||
                !FMath::IsFinite(Scalar.Minimum)||!FMath::IsFinite(Scalar.Maximum)||Scalar.Minimum>Scalar.Maximum||
                (I==0&&Scalar.Minimum<0)||(I==4&&Scalar.Minimum<=0))
            {Error=TEXT("Recording scalar identity, units, origin or range is invalid.");return false;}
            M.Scalars.Add(MoveTemp(Scalar));
        }
    }
    double MeshCRC;
    if(!O->TryGetNumberField(TEXT("meshCRC32"),MeshCRC)||!FMath::IsFinite(MeshCRC)||MeshCRC<0||MeshCRC>MAX_uint32||MeshCRC!=FMath::FloorToDouble(MeshCRC))
    { Error=TEXT("Recording mesh checksum is missing or invalid."); return false; }
    MeshChecksum=uint32(MeshCRC);
    const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
    if(!O->TryGetArrayField(TEXT("frameCRC32"),A)||A->IsEmpty()||A->Num()>100000)
    { Error=TEXT("Recording frame checksums are missing or exceed supported limits."); return false; }
    for(const auto& Value:*A)
    {
        if(Cancellation && Cancellation->load()) { Error=TEXT("Recording load cancelled."); return false; }
        double N;
        if(!Value->TryGetNumber(N)||!FMath::IsFinite(N)||N<0||N>MAX_uint32||N!=FMath::FloorToDouble(N))
        { Error=TEXT("Recording frame checksum is invalid."); return false; }
        Checksums.Add(uint32(N));
    }
    return true;
}
class FRecordedField final : public IStudioField
{
public:
    FRecordedField(TSharedPtr<const FRecordedFlowData,ESPMode::ThreadSafe> InData,int32 InFrame,
        const FStudioLoadCancellation& Cancellation = {},FString* AnalysisError = nullptr)
        : Data(InData),Frame(Data->ReadFrame(InFrame,Cancellation,AnalysisError)),Ordinal(InFrame) {}
    bool IsValid() const override { return Frame.IsValid(); }
    TOptional<FStudioFieldIdentity> Identity() const override
    {
        if(!Frame||!Data->Meta.Frames.IsValidIndex(Ordinal))return {};
        FStudioFieldIdentity I;I.Dataset=Data->Meta.Id;I.MetadataSHA256=Data->Meta.MetadataSHA256;
        I.PayloadSHA256=Data->Meta.PayloadSHA256;I.Ordinal=Ordinal;I.Frame=Data->Meta.Frames[Ordinal];
        I.SpatialDimensions=2;I.SourceOffset=Data->Meta.SourceOffset;I.Interpolation=EStudioFieldInterpolation::SourceTriangles;
        return I;
    }
    TOptional<FStudioScalarDescriptor> Scalar(const FString& Id) const override
    {
        if(const auto* S=Data->Meta.Scalars.FindByPredicate([&](const auto& Item){return Item.Id==Id;}))return *S;
        return {};
    }
    const TArray<FVector2D>& Boundary() const override { return Data->Outline; }
    const TArray<FIntVector>& BoundaryTriangles() const override { return Data->BoundaryCaps; }
    int32 MeshTriangleCount() const override { return Data->Triangles.Num(); }
    bool MeshTriangle(int32 Index,FVector (&Out)[3]) const override
    {
        if(!Data->Triangles.IsValidIndex(Index))return false;
        const auto T=Data->Triangles[Index];
        for(int32 I=0;I<3;++I){const auto P=Data->Nodes[T[I]];Out[I]=FVector(P.X,Data->Meta.SourceOffset.Y,P.Y);}
        return true;
    }
    bool IsSolid(const FVector& P) const override
    {
        bool Inside=false; const auto& Points=Data->Outline;
        for(int32 I=0,J=Points.Num()-1;I<Points.Num();J=I++)
        {
            const auto A=Points[I],B=Points[J];
            if((A.Y>P.Z)!=(B.Y>P.Z) && P.X<(B.X-A.X)*(P.Z-A.Y)/(B.Y-A.Y)+A.X) Inside=!Inside;
        }
        return Inside;
    }
    bool Sample(const FVector& P,FStudioFieldValue& Out) const override
    {
        Out=FStudioFieldValue();
        if(!Frame||P.ContainsNaN()||!Data->Meta.DisplayBounds.IsInsideOrOn(P)) return false;
        const FVector2D Q(P.X,P.Z);
        for(int32 Index:Data->Bins[Data->ZBin(P.Z)*FRecordedFlowData::NX+Data->XBin(P.X)])
        {
            const auto T=Data->Triangles[Index]; const auto A=Data->Nodes[T.X],B=Data->Nodes[T.Y],C=Data->Nodes[T.Z];
            const auto V0=B-A,V1=C-A,V2=Q-A;
            const double D=V0.X*V1.Y-V1.X*V0.Y;
            if(FMath::Abs(D)<1e-18) continue;
            const double WB=(V2.X*V1.Y-V1.X*V2.Y)/D,WC=(V0.X*V2.Y-V2.X*V0.Y)/D,WA=1.-WB-WC;
            if(WA< -1e-9||WB< -1e-9||WC< -1e-9) continue;
            const auto& VA=Frame->Values[T.X]; const auto& VB=Frame->Values[T.Y]; const auto& VC=Frame->Values[T.Z];
            Out.Velocity=FVector(WA*VA.U+WB*VB.U+WC*VC.U,0,WA*VA.V+WB*VB.V+WC*VC.V);
            Out.Pressure=WA*VA.Pressure+WB*VB.Pressure+WC*VC.Pressure;
            Out.Density=WA*VA.Density+WB*VB.Density+WC*VC.Density;
            return true;
        }
        return false;
    }
    bool SupportsSegment(const FVector& A,const FVector& B,const FStudioLoadCancellation& Cancellation) const override
    {
        if(!Frame||A.ContainsNaN()||B.ContainsNaN()||!Data->Meta.DisplayBounds.IsInsideOrOn(A)||
            !Data->Meta.DisplayBounds.IsInsideOrOn(B))return false;
        const FVector2D P(A.X,A.Z),Q(B.X,B.Z);
        TSet<int32> Visited;TArray<FVector2D> Intervals;
        for(int32 Z=Data->ZBin(FMath::Min(A.Z,B.Z));Z<=Data->ZBin(FMath::Max(A.Z,B.Z));++Z)
            for(int32 X=Data->XBin(FMath::Min(A.X,B.X));X<=Data->XBin(FMath::Max(A.X,B.X));++X)
                for(int32 Index:Data->Bins[Z*FRecordedFlowData::NX+X])
                {
                    if(Cancellation&&Cancellation->load(std::memory_order_relaxed))return false;
                    if(Visited.Contains(Index))continue;
                    if(Visited.Num()>=65536)return false;
                    Visited.Add(Index);const auto T=Data->Triangles[Index];FVector2D Interval;
                    if(StudioSegmentCoverage::Triangle(P,Q,Data->Nodes[T.X],Data->Nodes[T.Y],Data->Nodes[T.Z],Interval))Intervals.Add(Interval);
                }
        return StudioSegmentCoverage::Complete(Intervals);
    }
private:
    TSharedPtr<const FRecordedFlowData,ESPMode::ThreadSafe> Data;
    TSharedPtr<const FRecordedFlowData::FFrame,ESPMode::ThreadSafe> Frame;
    int32 Ordinal=INDEX_NONE;
};
}
FRecordedSolver::FRecordedSolver(const FString& Path,int64 CacheBudgetBytes,const FStudioLoadCancellation& Cancellation)
{
    auto Mutable=MakeShared<FRecordedFlowData,ESPMode::ThreadSafe>(); Data=Mutable;
    const auto Cancelled=[&]
    {
        if(!Cancellation || !Cancellation->load()) return false;
        Error=TEXT("Recording load cancelled."); return true;
    };
    if(Cancelled()) return;
    Mutable->Path=Path.IsEmpty()?FPaths::ProjectContentDir()/TEXT("Samples/MeshGraphNets_Airfoil/flow.bin"):Path;
    Mutable->Budget=FMath::Clamp<int64>(CacheBudgetBytes,16*1024,1024LL*1024*1024);
    FStudioFileAccess PayloadAccess(Mutable->Path),MetadataAccess(FPaths::GetPath(Mutable->Path)/TEXT("recording.json"));
    if(!ReadDescriptor(FPaths::GetPath(Mutable->Path)/TEXT("recording.json"),Mutable->Meta,Mutable->Checksums,Mutable->MeshChecksum,Error,Cancellation)||Cancelled()) return;
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Mutable->Path));
    if(!Reader||Reader->TotalSize()<24) { Error=TEXT("Recording payload is missing or truncated."); return; }
    int32 Magic,Version,Nodes,Triangles,Boundary,Frames;
    *Reader<<Magic<<Version<<Nodes<<Triangles<<Boundary<<Frames;
    if(Magic!=0x53553246||Version!=2||Nodes<3||Nodes>4000000||Triangles<1||Triangles>8000000||Boundary<3||Boundary>Nodes||Boundary>4096||Frames<1||Frames>100000)
    { Error=TEXT("Unsupported recording format or mesh/frame count."); return; }
    const int64 MeshEnd=24LL+16LL*Nodes+12LL*Triangles+4LL*Boundary;
    const int64 Expected=MeshEnd+int64(Frames)*(12LL+16LL*Nodes);
    if(Expected!=Reader->TotalSize()||Mutable->Checksums.Num()!=Frames||16LL*Nodes>Mutable->Budget)
    { Error=TEXT("Recording size/checksums disagree, or one frame exceeds the configured cache budget."); return; }
    // Verify topology once without loading any flow arrays or duplicating the
    // entire mesh payload in memory. Per-frame checks run only when requested.
    Reader->Seek(0); uint8 Chunk[65536]; uint32 MeshCRC=0;
    for(int64 Remaining=MeshEnd;Remaining>0;)
    {
        if(Cancelled()) return;
        const int32 Count=int32(FMath::Min<int64>(Remaining,sizeof(Chunk)));
        Reader->Serialize(Chunk,Count); MeshCRC=FCrc::MemCrc32(Chunk,Count,MeshCRC); Remaining-=Count;
    }
    if(Reader->IsError()||MeshCRC!=Mutable->MeshChecksum) { Error=TEXT("Recording mesh failed integrity validation. Reimport the dataset."); return; }
    Reader->Seek(24);
    Mutable->Meta.NodeCount=Nodes; Mutable->Meta.TriangleCount=Triangles;
    Mutable->Nodes.SetNum(Nodes);
    for(auto& P:Mutable->Nodes)
    {
        if(Cancelled()) return;
        *Reader<<P.X<<P.Y;
        if(!FMath::IsFinite(P.X)||!FMath::IsFinite(P.Y)||FMath::Abs(P.X)>1e9||FMath::Abs(P.Y)>1e9) { Error=TEXT("Invalid source mesh coordinates."); return; }
        P.X+=Mutable->Meta.SourceOffset.X; P.Y+=Mutable->Meta.SourceOffset.Z;
    }
    Mutable->Triangles.SetNum(Triangles);
    for(auto& T:Mutable->Triangles)
    {
        if(Cancelled()) return;
        *Reader<<T.X<<T.Y<<T.Z;
        if(T.GetMin()<0||T.GetMax()>=Nodes||T.X==T.Y||T.X==T.Z||T.Y==T.Z) { Error=TEXT("Invalid source mesh connectivity."); return; }
    }
    TSet<int32> BoundaryIds;
    for(int32 I=0;I<Boundary;++I)
    {
        if(Cancelled()) return;
        int32 Index; *Reader<<Index;
        if(!Mutable->Nodes.IsValidIndex(Index)||BoundaryIds.Contains(Index)) { Error=TEXT("Invalid source boundary."); return; }
        BoundaryIds.Add(Index); Mutable->Outline.Add(Mutable->Nodes[Index]);
    }
    if(Cancelled())return;
    // Triangulate once on the loader, never per frame. The explicit boundary
    // limit bounds the ear-clipping cost for an external polygon.
    TArray<UE::Geometry::FIndex3i> Caps;
    PolygonTriangulation::TriangulateSimplePolygon(Mutable->Outline,Caps,false);
    for(const auto& T:Caps)Mutable->BoundaryCaps.Add(FIntVector(T.A,T.B,T.C));
    if(Cancelled())return;
    TArray<FStudioFrame> LoadedFrames; TArray<int64> Offsets;
    for(int32 I=0;I<Frames;++I)
    {
        if(Cancelled()) return;
        Reader->Seek(MeshEnd+int64(I)*(12LL+16LL*Nodes));
        FStudioFrame F; *Reader<<F.Index<<F.Time;
        if(F.Index<0||!FMath::IsFinite(F.Time)||F.Time<0||(!LoadedFrames.IsEmpty()&&(F.Index<=LoadedFrames.Last().Index||F.Time<=LoadedFrames.Last().Time)))
        { Error=TEXT("Recording frames must have increasing source indices and finite times."); return; }
        LoadedFrames.Add(F); Offsets.Add(Reader->Tell());
    }
    if(Reader->IsError()||!Mutable->BuildBins(Cancellation))
    { if(!Cancelled()) Error=TEXT("Recording mesh read failed or its sampling index exceeds the memory limit."); return; }
    if(Cancelled()) return;
    Mutable->Meta.Frames=MoveTemp(LoadedFrames); Mutable->Offsets=MoveTemp(Offsets);
}
bool FRecordedSolver::PrepareFrame(int32 Ordinal,const FStudioLoadCancellation& Cancellation)
{ return Data->ReadFrame(Ordinal,Cancellation).IsValid(); }
int32 FRecordedSolver::FrameCount() const { return Data->Meta.Frames.Num(); }
FStudioFrame FRecordedSolver::EvaluateFrame(int32 Ordinal) const
{ return Data->Meta.Frames.IsValidIndex(Ordinal)?Data->Meta.Frames[Ordinal]:FStudioFrame(); }
TSharedRef<const IStudioField,ESPMode::ThreadSafe> FRecordedSolver::CaptureField(int32 Ordinal) const
{ return MakeShared<FRecordedField,ESPMode::ThreadSafe>(Data,Ordinal); }
TSharedRef<const IStudioField,ESPMode::ThreadSafe> FRecordedSolver::CaptureViewField(int32 Ordinal,const FString& ScalarId,
    bool bVectors,const FStudioLoadCancellation& Cancellation) const
{ return MakeShared<FRecordedField,ESPMode::ThreadSafe>(Data,Ordinal,Cancellation); }
FStudioFieldReadResult FRecordedSolver::ReadScalarFrame(int32 Ordinal,const FString& ScalarId,
    const FStudioLoadCancellation& Cancellation) const
{
    FStudioFieldReadResult Out;
    if(!Data->Meta.Frames.IsValidIndex(Ordinal)||!Data->Meta.Scalars.ContainsByPredicate([&](const auto& S){return S.Id==ScalarId;}))
    {Out.Error=TEXT("The exact requested frame or scalar is not supplied by this recording.");return Out;}
    if(!Error.IsEmpty()){Out.Error=Error;return Out;}
    auto Field=MakeShared<FRecordedField,ESPMode::ThreadSafe>(Data,Ordinal,Cancellation,&Out.Error);
    if(Cancellation&&Cancellation->load())Out.Error=TEXT("Recorded-frame analysis cancelled.");
    else if(Field->IsValid())Out.Field=MoveTemp(Field);
    else if(Out.Error.IsEmpty())Out.Error=TEXT("Could not read the requested recorded frame.");
    return Out;
}
const FStudioRecordingDescriptor& FRecordedSolver::Descriptor() const { return Data->Meta; }
FString FRecordedSolver::LoadError() const
{ if(!Error.IsEmpty()) return Error; FScopeLock Lock(&Data->Mutex); return Data->ReadError; }
FStudioFrameCacheStats FRecordedSolver::CacheStats() const
{
    FScopeLock Lock(&Data->Mutex);
    return {Data->Budget,Data->Cache.Num()*Data->FrameBytes(),Data->Cache.Num(),Data->Loads,Data->Hits};
}
bool FRecordedSolver::ExportField(int32 Ordinal,const FString& Path) const
{
    const auto F=Data->ReadFrame(Ordinal); if(!F) return false;
    FString CSV=TEXT("source,frame,elapsed_time_s,node,source_x_m,source_y_m,velocity_x_m_s,velocity_y_m_s,pressure_Pa,density_kg_m3\n");
    const auto Meta=Data->Meta.Frames[Ordinal];
    for(int32 I=0;I<Data->Nodes.Num();++I)
    {
        const auto P=Data->Nodes[I]; const auto V=F->Values[I];
        CSV+=FString::Printf(TEXT("%s,%d,%.17g,%d,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g\n"),*Data->Meta.SourceLabel,Meta.Index,Meta.Time,I,P.X-Data->Meta.SourceOffset.X,P.Y-Data->Meta.SourceOffset.Z,V.U,V.V,V.Pressure,V.Density);
    }
    return FFileHelper::SaveStringToFile(CSV,*Path);
}
