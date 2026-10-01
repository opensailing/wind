#include "StudioSurfaceReconstruction.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Algo/Sort.h"
#include "CompGeom/PolygonTriangulation.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioSurfacePrivate
{
constexpr int32 MaxTriangles=500000, MaxBoundary=4096;
bool Cancelled(const FStudioLoadCancellation& C){return C&&C->load(std::memory_order_relaxed);}
bool HashValid(const FString& H)
{if(H.Len()!=64)return false;for(TCHAR C:H)if(!FChar::IsHexDigit(C))return false;return true;}
bool Clean(const FString& S,int32 Limit)
{if(S.TrimStartAndEnd().IsEmpty()||S.Len()>Limit)return false;for(TCHAR C:S)if(C<32||C==127)return false;return true;}
bool String(const FJsonObject& O,const TCHAR* Key,FString& Out,int32 Limit=2048)
{return O.TryGetStringField(Key,Out)&&Clean(Out,Limit);}
bool Integer(const FJsonObject& O,const TCHAR* Key,int64 Minimum,int64 Maximum,int64& Out)
{double V;if(!O.TryGetNumberField(Key,V)||!FMath::IsFinite(V)||V<Minimum||V>Maximum||V!=FMath::FloorToDouble(V))return false;Out=int64(V);return true;}
bool Read(const FString& Path,int64 Limit,TArray<uint8>& Bytes,FString& Hash,const FStudioLoadCancellation& Cancel)
{
    FStudioFileAccess Access(Path);
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if(!File||File->TotalSize()<1||File->TotalSize()>Limit)return false;
    Bytes.SetNumUninitialized(int32(File->TotalSize()));
    for(int32 Offset=0;Offset<Bytes.Num();)
    {
        if(Cancelled(Cancel))return false;
        const int32 Count=FMath::Min(65536,Bytes.Num()-Offset);File->Serialize(Bytes.GetData()+Offset,Count);Offset+=Count;
        if(File->IsError())return false;
    }
    uint8 Digest[EVP_MAX_MD_SIZE];unsigned int Length=0;
    if(Cancelled(Cancel)||EVP_Digest(Bytes.GetData(),Bytes.Num(),Digest,&Length,EVP_sha256(),nullptr)!=1||Length!=32)return false;
    Hash=BytesToHex(Digest,Length).ToLower();return !Cancelled(Cancel);
}
bool Array(const FJsonObject& Object,const TCHAR* Key,int32 Width,int32 Limit,const FString& Folder,
    int32 PointCount,TArray<int32>& Out,const FStudioLoadCancellation& Cancel)
{
    const TSharedPtr<FJsonObject>* A=nullptr;const TArray<TSharedPtr<FJsonValue>>* Shape=nullptr;
    if(!Object.TryGetObjectField(Key,A)||!A||!A->IsValid())return false;
    FString Path,Type,Order,Expected;int64 BytesExpected;
    if(!String(**A,TEXT("path"),Path,128)||FPaths::GetCleanFilename(Path)!=Path||Path.Contains(TEXT(":"))||Path.Contains(TEXT("\\"))||Path==TEXT(".")||Path==TEXT("..")||
        !String(**A,TEXT("dtype"),Type)||Type!=TEXT("uint32")||!String(**A,TEXT("byteOrder"),Order)||Order!=TEXT("little")||
        !String(**A,TEXT("sha256"),Expected)||!HashValid(Expected)||!(*A)->TryGetArrayField(TEXT("shape"),Shape)||Shape->Num()!=(Width==1?1:2))return false;
    double N=0,W=1;if(!(*Shape)[0]->TryGetNumber(N)||!FMath::IsFinite(N)||N<3||N>Limit||N!=FMath::FloorToDouble(N))return false;
    if(Width!=1&&(!(*Shape)[1]->TryGetNumber(W)||W!=Width))return false;
    const int64 Size=int64(N)*Width*sizeof(uint32);
    if(!Integer(**A,TEXT("byteLength"),Size,Size,BytesExpected))return false;
    TArray<uint8> Bytes;FString Hash;
    if(!Read(Folder/Path,Size,Bytes,Hash,Cancel)||Bytes.Num()!=Size||!Hash.Equals(Expected,ESearchCase::IgnoreCase))return false;
    Out.SetNumUninitialized(Bytes.Num()/4);
    for(int32 I=0;I<Out.Num();++I)
    {
        if((I&1023)==0&&Cancelled(Cancel))return false;
        const int32 P=I*4;const uint32 V=uint32(Bytes[P])|(uint32(Bytes[P+1])<<8)|(uint32(Bytes[P+2])<<16)|(uint32(Bytes[P+3])<<24);
        if(V>=uint32(PointCount))return false;Out[I]=int32(V);
    }
    return !Cancelled(Cancel);
}
uint64 Edge(int32 A,int32 B){return (uint64(FMath::Min(A,B))<<32)|uint32(FMath::Max(A,B));}
double Cross(const FVector2D& A,const FVector2D& B){return A.X*B.Y-A.Y*B.X;}
bool Inside(const FVector2D& P,const TArray<FVector2D>& Polygon)
{
    bool Result=false;
    for(int32 I=0;I<Polygon.Num();++I)
    {const auto A=Polygon[I],B=Polygon[(I+1)%Polygon.Num()];if((A.Y>P.Y)!=(B.Y>P.Y))Result^=P.X<A.X+(P.Y-A.Y)*(B.X-A.X)/(B.Y-A.Y);}
    return Result;
}
bool Crosses(const FVector2D& A,const FVector2D& B,const FVector2D& C,const FVector2D& D)
{
    const double U=Cross(B-A,C-A),V=Cross(B-A,D-A),W=Cross(D-C,A-C),X=Cross(D-C,B-C);
    return ((U<0&&V>0)||(U>0&&V<0))&&((W<0&&X>0)||(W>0&&X<0));
}
bool ValidateHole(const FStudioPointGeometry& G,const TArray<FIntVector>& Faces,const TArray<int32>& Rows,
    TArray<FVector2D>& Boundary,const FStudioLoadCancellation& Cancel,FString& Error)
{
    auto Fail=[&](const TCHAR* Message){Error=Message;return false;};
    if(int64(G.Positions.Num()+Faces.Num())*Rows.Num()>50000000)
        return Fail(TEXT("Reconstruction exceeds the bounded topology-validation budget."));
    TSet<int32> BoundarySet;TSet<uint64> HoleEdges;
    auto Point=[&](int32 I){const auto P=G.Positions[I];return FVector2D(P.X,P.Y);};
    for(int32 I=0;I<Rows.Num();++I)
    {if(BoundarySet.Contains(Rows[I]))return Fail(TEXT("Reconstructed boundary repeats a source row."));BoundarySet.Add(Rows[I]);Boundary.Add(Point(Rows[I]));HoleEdges.Add(Edge(Rows[I],Rows[(I+1)%Rows.Num()]));}
    double Area=0;for(int32 I=0;I<Boundary.Num();++I)Area+=Cross(Boundary[I],Boundary[(I+1)%Boundary.Num()]);
    if(!FMath::IsFinite(Area)||Area<=0)return Fail(TEXT("Reconstructed boundary has invalid orientation or area."));
    for(int32 I=0;I<Boundary.Num();++I)
    {
        if(Cancelled(Cancel))return false;
        for(int32 J=I+1;J<Boundary.Num();++J)
            if(Crosses(Boundary[I],Boundary[(I+1)%Boundary.Num()],Boundary[J],Boundary[(J+1)%Boundary.Num()]))
                return Fail(TEXT("Reconstructed boundary crosses itself."));
    }
    for(int32 I=0;I<G.Positions.Num();++I)
    {
        if((I&255)==0&&Cancelled(Cancel))return false;
        if(!BoundarySet.Contains(I)&&Inside(Point(I),Boundary))return Fail(TEXT("Reconstructed solid contains original source samples."));
    }
    TArray<uint64> Edges;Edges.Reserve(Faces.Num()*3);
    TArray<int32> Parent;Parent.SetNumUninitialized(G.Positions.Num());for(int32 I=0;I<Parent.Num();++I)Parent[I]=I;
    TBitArray<> Used(false,G.Positions.Num());
    auto Root=[&](int32 I){while(Parent[I]!=I){Parent[I]=Parent[Parent[I]];I=Parent[I];}return I;};
    for(int32 I=0;I<Faces.Num();++I)
    {
        if((I&127)==0&&Cancelled(Cancel))return false;
        const auto T=Faces[I];const FVector2D V[]={Point(T.X),Point(T.Y),Point(T.Z)};
        if(Inside(V[0]/3+V[1]/3+V[2]/3,Boundary))return Fail(TEXT("A fluid triangle fills the reconstructed solid."));
        for(int32 E=0;E<3;++E)for(int32 B=0;B<Boundary.Num();++B)
            if(Crosses(V[E],V[(E+1)%3],Boundary[B],Boundary[(B+1)%Boundary.Num()]))
                return Fail(TEXT("A fluid triangle crosses the reconstructed solid boundary."));
        for(int32 E=0;E<3;++E)
        {const int32 A=T[E],B=T[(E+1)%3];Used[A]=true;Edges.Add(Edge(A,B));Parent[Root(A)]=Root(B);}
    }
    int32 Component=INDEX_NONE;
    for(int32 I=0;I<Parent.Num();++I)
    {
        if(!Used[I])return Fail(TEXT("Reconstruction discards an original source point."));
        const int32 R=Root(I);if(Component==INDEX_NONE)Component=R;else if(Component!=R)return Fail(TEXT("Reconstructed fluid surface is disconnected."));
    }
    Algo::Sort(Edges);int32 Unique=0,MatchedHole=0;
    for(int32 I=0;I<Edges.Num();)
    {
        if((I&1023)==0&&Cancelled(Cancel))return false;
        int32 End=I+1;while(End<Edges.Num()&&Edges[End]==Edges[I])++End;
        const int32 Count=End-I;const bool bHole=HoleEdges.Contains(Edges[I]);
        if(Count>2||(bHole&&Count!=1))return Fail(TEXT("Reconstructed surface has invalid edge neighbours."));
        MatchedHole+=bHole?1:0;++Unique;I=End;
    }
    if(MatchedHole!=Rows.Num()||G.Positions.Num()-Unique+Faces.Num()!=0)
        return Fail(TEXT("Reconstruction does not form the declared single solid hole."));
    return !Cancelled(Cancel);
}
}

FStudioSurfaceLoadResult StudioSurfaceReconstructions::Load(const FString& Path,const FStudioPointRecordingDescriptor& Source,
    TSharedRef<const FStudioPointGeometry,ESPMode::ThreadSafe> Geometry,const FStudioLoadCancellation& Cancel,const FString& ExpectedMetadata)
{
    using namespace StudioSurfacePrivate;
    auto Fail=[&](const TCHAR* Message){return FStudioSurfaceLoadResult{{},Cancelled(Cancel)?TEXT("Reconstruction loading cancelled."):Message};};
    if(Cancelled(Cancel))return Fail(TEXT("Reconstruction loading cancelled."));
    if(Source.SpatialDimensions!=2||Source.PointCount!=Geometry->Positions.Num()||Source.PointCount!=Geometry->PointIds.Num())
        return Fail(TEXT("Reconstruction requires a matching verified two-dimensional point recording."));
    TArray<uint8> Bytes;FString Hash;
    if(!Read(Path,65536,Bytes,Hash,Cancel))return Fail(TEXT("Reconstruction descriptor is missing, inaccessible or too large."));
    if(!ExpectedMetadata.IsEmpty()&&(!StudioSurfacePrivate::HashValid(ExpectedMetadata)||!Hash.Equals(ExpectedMetadata,ESearchCase::IgnoreCase)))
        return Fail(TEXT("Reconstruction contents differ from the saved interpretation. Locate its original files."));
    FString Text;FFileHelper::BufferToString(Text,Bytes.GetData(),Bytes.Num());TSharedPtr<FJsonObject> O;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O)||!O)return Fail(TEXT("Reconstruction descriptor is invalid JSON."));
    int64 Version,Dimensions;FString Kind,Origin,Association,Unit;
    auto R=MakeShared<FStudioSurfaceReconstruction,ESPMode::ThreadSafe>();R->MetadataSHA256=Hash;
    if(!Integer(*O,TEXT("version"),1,1,Version)||!Integer(*O,TEXT("spatialDimensions"),2,2,Dimensions)||
        !String(*O,TEXT("kind"),Kind)||Kind!=TEXT("spatial_reconstruction")||!String(*O,TEXT("origin"),Origin)||Origin!=TEXT("reconstructed")||
        !String(*O,TEXT("association"),Association)||Association!=TEXT("original_source_row")||!String(*O,TEXT("coordinateUnit"),Unit)||Unit!=TEXT("m")||
        !String(*O,TEXT("title"),R->Title,256)||!String(*O,TEXT("method"),R->Method)||
        R->Method!=TEXT("Delaunay with verified stationary-sample hole; piecewise-linear field interpolation")||!String(*O,TEXT("boundaryOrigin"),R->BoundaryOrigin))
        return Fail(TEXT("Unsupported reconstruction format, method or coordinate convention."));
    const TSharedPtr<FJsonObject>* Binding=nullptr;FString Id,Metadata,Coordinates,PointIds;int64 Points,Frames;
    if(!O->TryGetObjectField(TEXT("source"),Binding)||!Binding||!Binding->IsValid()||
        !String(**Binding,TEXT("recordingId"),Id)||!String(**Binding,TEXT("descriptorSHA256"),Metadata)||
        !String(**Binding,TEXT("coordinatesSHA256"),Coordinates)||!String(**Binding,TEXT("pointIdsSHA256"),PointIds)||
        !Integer(**Binding,TEXT("pointCount"),3,1000000,Points)||!Integer(**Binding,TEXT("frameCount"),1,100000,Frames)||
        Id!=Source.Id||!Metadata.Equals(Source.MetadataSHA256,ESearchCase::IgnoreCase)||
        !Coordinates.Equals(Source.Coordinates.SHA256,ESearchCase::IgnoreCase)||!PointIds.Equals(Source.PointIds.SHA256,ESearchCase::IgnoreCase)||
        Points!=Source.PointCount||Frames!=Source.Frames.Num())return Fail(TEXT("Reconstruction belongs to a different recording, coordinate set or point order."));
    const TArray<TSharedPtr<FJsonValue>>* Limitations=nullptr;
    if(!O->TryGetArrayField(TEXT("limitations"),Limitations)||Limitations->Num()<1||Limitations->Num()>32)
        return Fail(TEXT("Reconstruction limitations are missing or invalid."));
    for(const auto& V:*Limitations){FString S;if(!V->TryGetString(S)||!Clean(S,2048))return Fail(TEXT("Invalid reconstruction limitation."));R->Limitations.Add(MoveTemp(S));}
    TArray<int32> Indices;
    if(!Array(*O,TEXT("triangles"),3,MaxTriangles,FPaths::GetPath(Path),Source.PointCount,Indices,Cancel)||
        !Array(*O,TEXT("solidBoundary"),1,MaxBoundary,FPaths::GetPath(Path),Source.PointCount,R->BoundaryRows,Cancel))
        return Fail(TEXT("Reconstruction indices are missing, corrupt, oversized or out of source bounds."));
    TArray<FIntVector> Faces;Faces.Reserve(Indices.Num()/3);
    for(int32 I=0;I<Indices.Num();I+=3)Faces.Add(FIntVector(Indices[I],Indices[I+1],Indices[I+2]));Indices.Empty();
    FString Error;
    if(!ValidateHole(*Geometry,Faces,R->BoundaryRows,R->Boundary,Cancel,Error))
        return {{},Cancelled(Cancel)?TEXT("Reconstruction loading cancelled."):Error};
    auto Built=FStudioPlanarSurface::Create(Geometry,MoveTemp(Faces),64LL*1024*1024,Cancel);
    if(!Built.Surface)return {{},Built.Error};
    R->Surface=MoveTemp(Built.Surface);
    // Bounded by MaxBoundary, on the loader rather than once per displayed frame.
    TArray<UE::Geometry::FIndex3i> Caps;
    PolygonTriangulation::TriangulateSimplePolygon(R->Boundary,Caps,false);
    for(const auto& T:Caps)R->BoundaryCaps.Add(FIntVector(T.A,T.B,T.C));
    if(Cancelled(Cancel))return Fail(TEXT("Reconstruction loading cancelled."));
    return {R,{}};
}
