#include "StudioVolume.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioVolumePrivate
{
bool Cancelled(const FStudioLoadCancellation& C) { return C && C->load(std::memory_order_relaxed); }
bool HashValid(const FString& S)
{ if(S.Len()!=64)return false; for(TCHAR C:S)if(!FChar::IsHexDigit(C))return false; return true; }
bool Read(const FString& Path,int64 Limit,TArray<uint8>& Bytes,FString& Hash,const FStudioLoadCancellation& C)
{
    FStudioFileAccess Access(Path);
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if(Cancelled(C)||!File||File->TotalSize()<1||File->TotalSize()>Limit||File->TotalSize()>MAX_int32)return false;
    Bytes.SetNumUninitialized(int32(File->TotalSize()));
    for(int32 Offset=0;Offset<Bytes.Num();)
    {
        if(Cancelled(C))return false;
        const int32 N=FMath::Min(65536,Bytes.Num()-Offset);
        File->Serialize(Bytes.GetData()+Offset,N);Offset+=N;
        if(File->IsError())return false;
    }
    uint8 Digest[EVP_MAX_MD_SIZE];unsigned int N=0;
    if(EVP_Digest(Bytes.GetData(),Bytes.Num(),Digest,&N,EVP_sha256(),nullptr)!=1||N!=32)return false;
    Hash=BytesToHex(Digest,N).ToLower();return !Cancelled(C);
}
bool Vector(const FJsonObject& O,const TCHAR* Key,FVector& V)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if(!O.TryGetArrayField(Key,A)||A->Num()!=3)return false;
    for(int32 I=0;I<3;++I)if(!(*A)[I]->TryGetNumber(V[I])||!FMath::IsFinite(V[I])||FMath::Abs(V[I])>1.e8)return false;
    return true;
}
bool Array(const FJsonObject& O,const TCHAR* Key,const FString& Folder,const TCHAR* Type,int64 Size,
    TArray<uint8>& Bytes,const FStudioLoadCancellation& C)
{
    const TSharedPtr<FJsonObject>* A;
    FString Path,DType,Order,Expected,Actual;double Length;
    if(!O.TryGetObjectField(Key,A)||!(*A)->TryGetStringField(TEXT("path"),Path)||Path.IsEmpty()||Path.Len()>128||
        FPaths::GetCleanFilename(Path)!=Path||Path.Contains(TEXT(".."))||Path.Contains(TEXT(":"))||Path.Contains(TEXT("\\"))||
        !(*A)->TryGetStringField(TEXT("dtype"),DType)||DType!=Type||
        !(*A)->TryGetStringField(TEXT("byteOrder"),Order)||Order!=TEXT("little")||
        !(*A)->TryGetNumberField(TEXT("byteLength"),Length)||Length!=Size||
        !(*A)->TryGetStringField(TEXT("sha256"),Expected)||!HashValid(Expected))return false;
    return Read(Folder/Path,Size,Bytes,Actual,C)&&Bytes.Num()==Size&&Actual.Equals(Expected,ESearchCase::IgnoreCase);
}
uint32 U32(const uint8* P) { return uint32(P[0])|(uint32(P[1])<<8)|(uint32(P[2])<<16)|(uint32(P[3])<<24); }
double F64(const uint8* P)
{
    uint64 Bits=0;for(int32 I=0;I<8;++I)Bits|=uint64(P[I])<<(8*I);
    double V;FMemory::Memcpy(&V,&Bits,8);return V;
}
bool Coordinates(const FStudioVolumeReconstruction& V,const FVector& P,FIntVector& Cell,FVector& Fraction)
{
    if(!V.SourceBounds.IsValid||P.ContainsNaN()||!V.SourceBounds.IsInsideOrOn(P))return false;
    const FVector Q=(P-V.SourceBounds.Min)/V.SourceBounds.GetSize()*FVector(V.Dimensions-FIntVector(1));
    for(int32 A=0;A<3;++A)
    { Cell[A]=FMath::Clamp(FMath::FloorToInt(Q[A]),0,V.Dimensions[A]-2);Fraction[A]=Q[A]-Cell[A]; }
    return true;
}
double Value(const FStudioVolumeStencil& S,const TArray<double>& Values)
{
    double Result=0;for(int32 I=0;I<4;++I)Result+=S.Weights[I]*Values[S.Rows[I]];return Result;
}
double CylinderClearance(const FVector P[4],const FVector2D& Center)
{
    FVector2D V[4];for(int32 I=0;I<4;++I)V[I]=FVector2D(P[I].X,P[I].Y)-Center;
    auto Cross=[](const FVector2D& A,const FVector2D& B){return A.X*B.Y-A.Y*B.X;};
    double DistanceSquared=MAX_dbl;
    for(int32 I=0;I<4;++I)for(int32 J=I+1;J<4;++J)
    {
        const auto Edge=V[J]-V[I];const double Length=Edge.SizeSquared();
        const double T=Length>0?FMath::Clamp(-FVector2D::DotProduct(V[I],Edge)/Length,0.,1.):0;
        DistanceSquared=FMath::Min(DistanceSquared,(V[I]+T*Edge).SizeSquared());
    }
    for(int32 Omit=0;Omit<4;++Omit)
    {
        FVector2D T[3];int32 K=0;for(int32 I=0;I<4;++I)if(I!=Omit)T[K++]=V[I];
        const double Area=Cross(T[1]-T[0],T[2]-T[0]);
        const double A=Cross(T[1]-T[0],-T[0]),B=Cross(T[2]-T[1],-T[1]),C=Cross(T[0]-T[2],-T[2]);
        if(FMath::Abs(Area)>1.e-24&&((A>=0&&B>=0&&C>=0)||(A<=0&&B<=0&&C<=0)))return 0;
    }
    return FMath::Sqrt(DistanceSquared);
}
}

FVector FStudioVolumeReconstruction::Position(int32 I) const
{
    const FVector Q(I%Dimensions.X,(I/Dimensions.X)%Dimensions.Y,I/(Dimensions.X*Dimensions.Y));
    return SourceBounds.Min+Q/FVector(Dimensions-FIntVector(1))*SourceBounds.GetSize();
}
bool FStudioVolumeReconstruction::Sample(const FVector& P,const TArray<double>& Values,double& Out) const
{
    using namespace StudioVolumePrivate;
    Out=std::numeric_limits<double>::quiet_NaN();FIntVector Cell;FVector F;
    if(!Geometry||Values.Num()!=Geometry->Positions.Num()||IsSolid(P)||!Coordinates(*this,P,Cell,F))return false;
    const FVector Spacing=SourceBounds.GetSize()/FVector(Dimensions-FIntVector(1));
    const FVector Minimum=SourceBounds.Min+FVector(Cell)*Spacing;
    if(ContainsSolid(FBox(Minimum,Minimum+Spacing)))return false;
    double Sum=0;
    for(int32 Corner=0;Corner<8;++Corner)
    {
        const int32 X=Corner&1,Y=(Corner>>1)&1,Z=(Corner>>2)&1;
        const int32 I=Cell.X+X+Dimensions.X*(Cell.Y+Y+Dimensions.Y*(Cell.Z+Z));
        if(!Classification.IsValidIndex(I)||Classification[I]!=1)return false;
        const double Weight=(X?F.X:1-F.X)*(Y?F.Y:1-F.Y)*(Z?F.Z:1-F.Z);
        Sum+=Weight*Value(Stencils[I],Values);
    }
    if(!FMath::IsFinite(Sum))return false;Out=Sum;return true;
}
bool FStudioVolumeReconstruction::IsSolid(const FVector& P) const
{
    return SourceBounds.IsInsideOrOn(P)&&(FVector2D(P.X,P.Y)-CylinderCenter).SizeSquared()<CylinderRadius*CylinderRadius;
}

bool FStudioVolumeReconstruction::ContainsSolid(const FBox& Region) const
{
    if(!Region.IsValid||CylinderRadius<=0)return false;
    const double X=FMath::Clamp(CylinderCenter.X,Region.Min.X,Region.Max.X)-CylinderCenter.X;
    const double Y=FMath::Clamp(CylinderCenter.Y,Region.Min.Y,Region.Max.Y)-CylinderCenter.Y;
    return X*X+Y*Y<CylinderRadius*CylinderRadius;
}

bool FStudioVolumeReconstruction::SupportsRegion(const FBox& Region,const FStudioLoadCancellation& Cancellation) const
{
    using namespace StudioVolumePrivate;
    FIntVector First,Last;FVector Fraction;
    if(Cancelled(Cancellation)||!Region.IsValid||Dimensions.GetMin()<2||
        !Coordinates(*this,Region.Min,First,Fraction)||!Coordinates(*this,Region.Max,Last,Fraction))return false;
    const FVector Spacing=SourceBounds.GetSize()/FVector(Dimensions-FIntVector(1));
    // Include the full interpolation-cell support, not just the small rendered
    // quad. Scalar sampling also excludes cells touching the analytic solid.
    if(ContainsSolid(FBox(SourceBounds.Min+FVector(First)*Spacing,
        SourceBounds.Min+FVector(Last+FIntVector(1))*Spacing)))return false;
    for(int32 Z=First.Z;Z<=Last.Z+1;++Z)for(int32 Y=First.Y;Y<=Last.Y+1;++Y)
    {
        if(Cancelled(Cancellation))return false;
        for(int32 X=First.X;X<=Last.X+1;++X)
        {
            const int32 I=X+Dimensions.X*(Y+Dimensions.Y*Z);
            if(!Classification.IsValidIndex(I)||Classification[I]!=1)return false;
        }
    }
    return true;
}

FStudioVolumeLoadResult StudioVolumes::Load(const FString& Path,const FStudioPointRecordingDescriptor& Source,
    TSharedRef<const FStudioPointGeometry,ESPMode::ThreadSafe> Geometry,const FStudioLoadCancellation& C,const FString& Expected)
{
    using namespace StudioVolumePrivate;
    auto Fail=[&](const TCHAR* S){return FStudioVolumeLoadResult{{},Cancelled(C)?TEXT("Volume reconstruction cancelled."):S};};
    if(Source.SpatialDimensions!=3||Source.PointCount!=Geometry->Positions.Num())return Fail(TEXT("A verified three-dimensional point recording is required."));
    TArray<uint8> Bytes;FString Hash;
    if(!Read(Path,1024*1024,Bytes,Hash,C)||(!Expected.IsEmpty()&&!Hash.Equals(Expected,ESearchCase::IgnoreCase)))
        return Fail(TEXT("Volume reconstruction metadata is missing, changed or too large."));
    FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());
    TSharedPtr<FJsonObject> O;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Converted.Length(),Converted.Get())),O)||!O)return Fail(TEXT("Invalid volume reconstruction metadata."));
    auto V=MakeShared<FStudioVolumeReconstruction,ESPMode::ThreadSafe>();
    double Version;FString Kind,Origin,Order;FVector Dimensions;
    const TArray<TSharedPtr<FJsonValue>>* Limits;
    if(!O->TryGetNumberField(TEXT("version"),Version)||Version!=1||!O->TryGetStringField(TEXT("kind"),Kind)||Kind!=TEXT("volume_reconstruction")||
        !O->TryGetStringField(TEXT("origin"),Origin)||Origin!=TEXT("derived")||!O->TryGetStringField(TEXT("layout"),Order)||Order!=TEXT("x_fastest_node_grid")||
        !O->TryGetStringField(TEXT("sourceMetadataSHA256"),V->SourceMetadataSHA256)||V->SourceMetadataSHA256!=Source.MetadataSHA256||
        !O->TryGetStringField(TEXT("title"),V->Title)||V->Title.IsEmpty()||V->Title.Len()>256||
        !O->TryGetStringField(TEXT("method"),V->Method)||V->Method.IsEmpty()||V->Method.Len()>4096||
        !O->TryGetArrayField(TEXT("limitations"),Limits)||Limits->IsEmpty()||Limits->Num()>32||
        !Vector(*O,TEXT("dimensions"),Dimensions)||!Vector(*O,TEXT("minimum"),V->SourceBounds.Min)||!Vector(*O,TEXT("maximum"),V->SourceBounds.Max))
        return Fail(TEXT("Volume reconstruction needs matching source identity, grid and method metadata."));
    for(const auto& L:*Limits){FString Text;if(!L->TryGetString(Text)||Text.IsEmpty()||Text.Len()>2048)return Fail(TEXT("Invalid volume limitations."));V->Limitations.Add(Text);}
    for(int32 A=0;A<3;++A)
        if(Dimensions[A]<2||Dimensions[A]>512||Dimensions[A]!=FMath::FloorToDouble(Dimensions[A]))return Fail(TEXT("Volume grid dimension is outside its limit."));
    V->Dimensions=FIntVector(int32(Dimensions.X),int32(Dimensions.Y),int32(Dimensions.Z));
    const int64 Count=int64(V->Dimensions.X)*V->Dimensions.Y*V->Dimensions.Z;
    V->SourceBounds.IsValid=1;
    if(Count>MaximumVoxels||V->SourceBounds.GetSize().GetMin()<=0||
        !Source.SourceBounds.IsInsideOrOn(V->SourceBounds.Min)||!Source.SourceBounds.IsInsideOrOn(V->SourceBounds.Max))
        return Fail(TEXT("Volume grid exceeds source coverage or its memory budget."));
    const TSharedPtr<FJsonObject>* Solid;const TArray<TSharedPtr<FJsonValue>>* Center;FString SolidKind,BoundaryOrigin;
    if(!O->TryGetObjectField(TEXT("solid"),Solid)||!(*Solid)->TryGetStringField(TEXT("kind"),SolidKind)||SolidKind!=TEXT("cylinder_z")||
        !(*Solid)->TryGetStringField(TEXT("origin"),BoundaryOrigin)||BoundaryOrigin.IsEmpty()||
        !(*Solid)->TryGetNumberField(TEXT("radiusMeters"),V->CylinderRadius)||!FMath::IsFinite(V->CylinderRadius)||V->CylinderRadius<=0||
        !(*Solid)->TryGetArrayField(TEXT("centerXY"),Center)||Center->Num()!=2||
        !(*Center)[0]->TryGetNumber(V->CylinderCenter.X)||!(*Center)[1]->TryGetNumber(V->CylinderCenter.Y)||V->CylinderCenter.ContainsNaN())
        return Fail(TEXT("This volume reconstruction requires an explicit finite cylindrical solid boundary."));
    // Original wall-adjacent rows may lie inside the documented analytic
    // cylinder. They remain unchanged; every actual interpolation tetrahedron
    // is checked below and cannot pass through that conservative solid mask.
    TArray<uint8> Rows,Weights,Classes;const FString Folder=FPaths::GetPath(Path);
    if(!Array(*O,TEXT("rows"),Folder,TEXT("uint32"),Count*16,Rows,C)||
        !Array(*O,TEXT("weights"),Folder,TEXT("float64"),Count*32,Weights,C)||
        !Array(*O,TEXT("classification"),Folder,TEXT("uint8"),Count,Classes,C))
        return Fail(TEXT("Volume mapping arrays are missing or failed their size/hash checks."));
    V->Geometry=Geometry;V->MetadataSHA256=Hash;V->Classification=MoveTemp(Classes);V->Stencils.SetNum(int32(Count));
    const double Tolerance=FMath::Max(1.e-9,V->SourceBounds.GetSize().Size()*1.e-10);
    int32 Fluid=0;
    for(int32 I=0;I<Count;++I)
    {
        if((I&255)==0&&Cancelled(C))return Fail(TEXT("Volume mapping cancelled."));
        if(V->Classification[I]>2)return Fail(TEXT("Unknown volume cell classification."));
        auto& S=V->Stencils[I];double Sum=0;FVector P=FVector::ZeroVector;FVector Vertices[4];
        for(int32 J=0;J<4;++J)
        {
            const uint32 Row=U32(Rows.GetData()+I*16+J*4);const double W=F64(Weights.GetData()+I*32+J*8);
            if(V->Classification[I]!=1)
            {if(Row!=MAX_uint32||W!=0)return Fail(TEXT("Masked voxels must not reference source values."));continue;}
            if(Row>=uint32(Source.PointCount)||!FMath::IsFinite(W)||W<0||W>1)return Fail(TEXT("Invalid volume interpolation stencil."));
            S.Rows[J]=int32(Row);S.Weights[J]=W;Sum+=W;Vertices[J]=Geometry->Positions[Row];P+=W*Vertices[J];
        }
        if(V->Classification[I]==1)
        {
            if(FMath::Abs(Sum-1)>1.e-10||!P.Equals(V->Position(I),Tolerance))return Fail(TEXT("Volume interpolation weights do not reproduce grid coordinates."));
            if(CylinderClearance(Vertices,V->CylinderCenter)+1.e-12<V->CylinderRadius)
                return Fail(TEXT("Volume interpolation crosses the declared solid."));
            ++Fluid;
        }
        const bool SolidNode=V->IsSolid(V->Position(I));
        if((V->Classification[I]==2)!=SolidNode)return Fail(TEXT("Volume solid mask disagrees with its declared boundary."));
    }
    if(Fluid<8)return Fail(TEXT("Volume reconstruction has insufficient supported fluid voxels."));
    return {V,{}};
}

FStudioVolumeLoadResult StudioVolumes::OriginalSource(const FStudioPointRecordingDescriptor& Source,
    TSharedRef<const FStudioPointGeometry,ESPMode::ThreadSafe> Geometry,const FStudioLoadCancellation& C)
{
    using namespace StudioVolumePrivate;
    auto Fail=[&](const TCHAR* S){return FStudioVolumeLoadResult{{},Cancelled(C)?TEXT("Original grid attachment cancelled."):S};};
    const auto S=Source.StructuredGrid;
    if(!S||Source.SpatialDimensions!=3||S->Dimensions.GetMin()<2||Source.PointCount!=Geometry->Positions.Num()||
        Geometry->PointIds.Num()!=Source.PointCount||int64(S->Dimensions.X)*S->Dimensions.Y*S->Dimensions.Z!=Source.PointCount||
        Source.PointCount>MaximumVoxels||S->SpacingMeters.GetMin()<=0)return Fail(TEXT("A verified original structured grid is required."));
    auto V=MakeShared<FStudioVolumeReconstruction,ESPMode::ThreadSafe>();
    V->OriginalGrid=S;V->Dimensions=S->Dimensions;V->Geometry=Geometry;V->SourceBounds=Source.SourceBounds;
    V->MetadataSHA256=Source.MetadataSHA256;V->SourceMetadataSHA256=Source.MetadataSHA256;
    V->Title=TEXT("Original structured grid");V->Method=TEXT("Original affine XYZ node grid; identity source rows; trilinear sampling. Frame-specific source masks.");
    V->Limitations=Source.Limitations;V->Classification.Init(1,Source.PointCount);V->Stencils.SetNum(Source.PointCount);
    for(int32 I=0;I<Source.PointCount;++I)
    {
        if((I&1023)==0&&Cancelled(C))return Fail(TEXT("Original grid attachment cancelled."));
        FIntVector Index;if(!S->OriginalIndex(I,Index))return Fail(TEXT("Original source grid row identity is invalid."));
        const int64 Id=int64(Index.X)+int64(S->OriginalDimensions.X)*(int64(Index.Y)+int64(S->OriginalDimensions.Y)*Index.Z);
        const FVector P=V->Position(I);const double Tolerance=64*std::numeric_limits<double>::epsilon()*FMath::Max(1.,P.GetAbsMax());
        if(Id!=Geometry->PointIds[I]||!P.Equals(Geometry->Positions[I],Tolerance))return Fail(TEXT("Original grid coordinates and IDs do not match the retained source."));
        auto& Stencil=V->Stencils[I];for(int32 J=0;J<4;++J)Stencil.Rows[J]=I;Stencil.Weights[0]=1;
    }
    return {V,{}};
}

TArray<uint8> StudioVolumes::SourceMask(const FStudioPointFrame& Frame,const FStudioVolumeReconstruction& V,
    const FString& Field,bool bVelocity,FString& Error,const FStudioLoadCancellation& C,TOptional<bool> AirMaskOverride)
{
    using namespace StudioVolumePrivate;
    auto Fail=[&](const TCHAR* Message){Error=Cancelled(C)?TEXT("Original source mask cancelled."):Message;return TArray<uint8>();};
    const auto S=V.OriginalGrid;const auto* F=Frame.Descriptor?Frame.Descriptor->FindField(Field):nullptr;
    if(!S||!Frame.Descriptor||Frame.Geometry.Get()!=V.Geometry.Get()||Frame.Descriptor->MetadataSHA256!=V.SourceMetadataSHA256||
        (!F&&!bVelocity))return Fail(TEXT("Source mask requires its bound original frame and field."));
    const auto* Solid=Frame.FindValues(S->SolidField);const auto* Phi=Frame.FindValues(S->PhaseField);
    const auto* SolidSupport=Frame.FindValues(S->SolidSupportField);const auto* LiquidSupport=Frame.FindValues(S->LiquidSupportField);
    const auto* Valid=F&&!F->ValidityMask.IsEmpty()?Frame.FindValues(F->ValidityMask):nullptr;
    const int32 Count=Frame.Descriptor->PointCount;
    if(!Solid||!Phi||!SolidSupport||!LiquidSupport||SolidSupport->Num()!=Count||LiquidSupport->Num()!=Count||
        Solid->Num()!=Count||Phi->Num()!=Count||int64(V.Dimensions.X)*V.Dimensions.Y*V.Dimensions.Z!=Count||
        (F&&!F->ValidityMask.IsEmpty()&&(!Valid||Valid->Num()!=Count)))return Fail(TEXT("Original phase, solid or field-validity arrays are unavailable."));
    for(int32 I=0;I<Count;++I)
    {
        if((I&4095)==0&&Cancelled(C))return Fail(TEXT("Original source mask cancelled."));
        if(!FMath::IsFinite((*Phi)[I])||((*Solid)[I]!=0&&(*Solid)[I]!=1)||
            ((*SolidSupport)[I]!=0&&(*SolidSupport)[I]!=1)||((*LiquidSupport)[I]!=0&&(*LiquidSupport)[I]!=1)||
            (Valid&&(*Valid)[I]!=0&&(*Valid)[I]!=1))return Fail(TEXT("Original masks contain invalid values."));
    }
    const bool bSolidDisplay=!bVelocity&&Field==S->SolidField;
    const bool bScientific=bVelocity||(F&&(F->bAirMaskDefault||!F->ValidityMask.IsEmpty()));
    const bool bAir=bVelocity||(F&&F->bAirMaskDefault&&AirMaskOverride.Get(true));
    TArray<uint8> Out;Out.SetNumZeroed(Count);
    for(int32 Z=0;Z<V.Dimensions.Z;++Z)for(int32 Y=0;Y<V.Dimensions.Y;++Y)
    {
        if(Cancelled(C))return Fail(TEXT("Original source mask cancelled."));
        for(int32 X=0;X<V.Dimensions.X;++X)
        {
            const int32 I=X+V.Dimensions.X*(Y+V.Dimensions.Y*Z);
            if(bSolidDisplay){Out[I]=1;continue;}
            if((*Solid)[I]!=0){Out[I]=2;continue;}
            if((*SolidSupport)[I]!=1||(bAir&&(*LiquidSupport)[I]!=1))continue;
            if(Valid&&(*Valid)[I]==0)continue;
            bool Supported=true;
            // phi keeps both phases. Liquid diagnostics/streamlines omit the complete one-node interface/solid halo.
            if(bScientific)for(int32 DZ=-1;DZ<=1&&Supported;++DZ)for(int32 DY=-1;DY<=1&&Supported;++DY)for(int32 DX=-1;DX<=1;++DX)
            {
                const int32 NX=X+DX,NY=Y+DY,NZ=Z+DZ;
                if(NX<0||NY<0||NZ<0||NX>=V.Dimensions.X||NY>=V.Dimensions.Y||NZ>=V.Dimensions.Z){Supported=false;break;}
                const int32 N=NX+V.Dimensions.X*(NY+V.Dimensions.Y*NZ);
                if((*Solid)[N]!=0||(bAir&&(*Phi)[N]<S->LiquidMinimum)){Supported=false;break;}
            }
            if(Supported)Out[I]=1;
        }
    }
    Error.Reset();return Out;
}

bool StudioVolumes::SampleSource(const FStudioPointFrame& Frame,const FStudioVolumeReconstruction& V,
    const TArray<uint8>& Mask,const FString& Field,const FVector& P,double& Out)
{
    using namespace StudioVolumePrivate;
    Out=std::numeric_limits<double>::quiet_NaN();FIntVector Cell;FVector F;const auto* Values=Frame.FindValues(Field);
    if(!V.OriginalGrid||!Frame.Descriptor||Frame.Descriptor->MetadataSHA256!=V.SourceMetadataSHA256||Frame.Geometry.Get()!=V.Geometry.Get()||!Values||Values->Num()!=Mask.Num()||
        Mask.Num()!=V.Classification.Num()||!Coordinates(V,P,Cell,F))return false;
    double Sum=0;
    for(int32 Corner=0;Corner<8;++Corner)
    {
        const int32 X=Corner&1,Y=(Corner>>1)&1,Z=(Corner>>2)&1;
        const int32 I=Cell.X+X+V.Dimensions.X*(Cell.Y+Y+V.Dimensions.Y*(Cell.Z+Z));
        if(Mask[I]!=1)return false;
        Sum+=(X?F.X:1-F.X)*(Y?F.Y:1-F.Y)*(Z?F.Z:1-F.Z)*(*Values)[I];
    }
    if(!FMath::IsFinite(Sum))return false;Out=Sum;return true;
}

bool StudioVolumes::SourceNodeSupported(const FStudioPointFrame& Frame,const FStudioVolumeReconstruction& V,
    const FString& Field,int32 I,bool bVelocity)
{
    const auto S=V.OriginalGrid;const auto* F=Frame.Descriptor?Frame.Descriptor->FindField(Field):nullptr;
    if(!S||!F||Frame.Geometry.Get()!=V.Geometry.Get()||Frame.Descriptor->MetadataSHA256!=V.SourceMetadataSHA256)return false;
    const auto* Phi=Frame.FindValues(S->PhaseField);const auto* Solid=Frame.FindValues(S->SolidField);
    const auto* SolidSupport=Frame.FindValues(S->SolidSupportField);const auto* LiquidSupport=Frame.FindValues(S->LiquidSupportField);
    const auto* Valid=F->ValidityMask.IsEmpty()?nullptr:Frame.FindValues(F->ValidityMask);
    if(!Phi||!Solid||Phi->Num()!=V.Classification.Num()||Solid->Num()!=Phi->Num()||!Phi->IsValidIndex(I)||
        !FMath::IsFinite((*Phi)[I])||(!F->ValidityMask.IsEmpty()&&(!Valid||!Valid->IsValidIndex(I)||(*Valid)[I]!=1)))return false;
    if(!bVelocity&&Field==S->SolidField)return (*Solid)[I]==0||(*Solid)[I]==1;
    if((*Solid)[I]!=0||!SolidSupport||!LiquidSupport||SolidSupport->Num()!=Phi->Num()||LiquidSupport->Num()!=Phi->Num()||
        (*SolidSupport)[I]!=1||((F->bAirMaskDefault||bVelocity)&&(*LiquidSupport)[I]!=1))return false;
    if(!F->bAirMaskDefault&&F->ValidityMask.IsEmpty()&&!bVelocity)return true;
    const int32 X=I%V.Dimensions.X,Y=(I/V.Dimensions.X)%V.Dimensions.Y,Z=I/(V.Dimensions.X*V.Dimensions.Y);
    for(int32 DZ=-1;DZ<=1;++DZ)for(int32 DY=-1;DY<=1;++DY)for(int32 DX=-1;DX<=1;++DX)
    {
        const int32 NX=X+DX,NY=Y+DY,NZ=Z+DZ;
        if(NX<0||NY<0||NZ<0||NX>=V.Dimensions.X||NY>=V.Dimensions.Y||NZ>=V.Dimensions.Z)return false;
        const int32 N=NX+V.Dimensions.X*(NY+V.Dimensions.Y*NZ);
        if((*Solid)[N]!=0||!FMath::IsFinite((*Phi)[N])||((F->bAirMaskDefault||bVelocity)&&(*Phi)[N]<S->LiquidMinimum))return false;
    }
    return true;
}

bool StudioVolumes::SupportsSourceRegion(const FStudioVolumeReconstruction& V,const TArray<uint8>& Mask,
    const FBox& Region,const FStudioLoadCancellation& C)
{
    using namespace StudioVolumePrivate;
    FIntVector First,Last;FVector F;
    if(!V.OriginalGrid||Cancelled(C)||Mask.Num()!=V.Classification.Num()||!Region.IsValid||
        !Coordinates(V,Region.Min,First,F)||!Coordinates(V,Region.Max,Last,F))return false;
    for(int32 Z=First.Z;Z<=Last.Z+1;++Z)for(int32 Y=First.Y;Y<=Last.Y+1;++Y)
    {
        if(Cancelled(C))return false;
        for(int32 X=First.X;X<=Last.X+1;++X)if(Mask[X+V.Dimensions.X*(Y+V.Dimensions.Y*Z)]!=1)return false;
    }
    return true;
}

bool StudioVolumes::IsSourceSolid(const FStudioPointFrame& Frame,const FStudioVolumeReconstruction& V,const FVector& P)
{
    using namespace StudioVolumePrivate;
    FIntVector Cell;FVector F;const auto* Solid=V.OriginalGrid?Frame.FindValues(V.OriginalGrid->SolidField):nullptr;
    if(!Solid||Solid->Num()!=V.Classification.Num()||!Coordinates(V,P,Cell,F))return false;
    for(int32 Corner=0;Corner<8;++Corner)
    {const int32 I=Cell.X+(Corner&1)+V.Dimensions.X*(Cell.Y+((Corner>>1)&1)+V.Dimensions.Y*(Cell.Z+((Corner>>2)&1)));if((*Solid)[I]!=0)return true;}
    return false;
}

FStudioVolumeRenderData StudioVolumes::Build(const FStudioPointFrame& Frame,const FStudioVolumeReconstruction& V,
    const FString& Field,const FStudioColorMapping& Mapping,const FStudioLoadCancellation& C,TOptional<bool> AirMaskOverride)
{
    using namespace StudioVolumePrivate;
    auto Fail=[&](const TCHAR* S){FStudioVolumeRenderData R;R.Error=Cancelled(C)?TEXT("Volume upload cancelled."):S;return R;};
    const auto* Values=Frame.FindValues(Field);
    const double Span=Mapping.Maximum-Mapping.Minimum;
    if(!Frame.Descriptor||Frame.Descriptor->SpatialDimensions!=3||Frame.Geometry.Get()!=V.Geometry.Get()||
        Frame.Descriptor->MetadataSHA256!=V.SourceMetadataSHA256||!Values||Values->Num()!=V.Geometry->Positions.Num()||
        V.Stencils.Num()>MaximumVoxels||V.Stencils.Num()!=V.Classification.Num()||!FMath::IsFinite(Span)||Span<0)
        return Fail(TEXT("Volume upload requires its bound source frame, field and finite display range."));
    FStudioVolumeRenderData Out;Out.Dimensions=V.Dimensions;Out.SourceBounds=V.SourceBounds;
    TArray<uint8> Mask;
    if(V.OriginalGrid)
    {Mask=SourceMask(Frame,V,Field,false,Out.Error,C,AirMaskOverride);if(!Out.Error.IsEmpty())return Out;}
    Out.CylinderCenter=V.CylinderCenter;Out.CylinderRadius=V.CylinderRadius;
    Out.Texels.SetNumZeroed(V.Stencils.Num());
    for(int32 I=0;I<Out.Texels.Num();++I)
    {
        if((I&255)==0&&Cancelled(C))return Fail(TEXT("Volume upload cancelled."));
        if((V.OriginalGrid?Mask[I]:V.Classification[I])!=1)continue;
        const double Scalar=Value(V.Stencils[I],*Values);
        const double Normalized=Span>0?(Scalar-Mapping.Minimum)/Span:.5;
        if(!FMath::IsFinite(Normalized)||FMath::Abs(Normalized)>1.e30)return Fail(TEXT("Volume display range exceeds GPU precision. Widen the color range."));
        Out.Texels[I]=FVector2f(float(Normalized),1.f);
        Out.MaximumTransportError=FMath::Max(Out.MaximumTransportError,FMath::Abs(double(Out.Texels[I].X)-Normalized));
    }
    return Out;
}

bool StudioVolumes::Intersect(const FVector& Origin,const FVector& Direction,const FBox& B,double Limit,double& Entry,double& Exit)
{
    Entry=0;Exit=Limit;
    if(!B.IsValid||B.GetSize().GetMin()<=0||Origin.ContainsNaN()||Direction.ContainsNaN()||!FMath::IsFinite(Limit)||Limit<=0||Direction.IsNearlyZero())return false;
    for(int32 A=0;A<3;++A)
    {
        if(FMath::Abs(Direction[A])<1.e-12)
        {if(Origin[A]<B.Min[A]||Origin[A]>B.Max[A])return false;continue;}
        const double T0=(B.Min[A]-Origin[A])/Direction[A],T1=(B.Max[A]-Origin[A])/Direction[A];
        Entry=FMath::Max(Entry,FMath::Min(T0,T1));Exit=FMath::Min(Exit,FMath::Max(T0,T1));
        if(Exit<=Entry)return false;
    }
    return Exit>Entry;
}

FStudioVolumeIsosurface StudioVolumes::Isosurface(const FStudioVolumeRenderData& Grid,double Level,const FStudioLoadCancellation& C)
{
    auto Fail=[&](const TCHAR* Message){FStudioVolumeIsosurface R;R.Error=Message;return R;};
    const FIntVector D=Grid.Dimensions;
    if(!Grid.Error.IsEmpty()||D.GetMin()<2||Grid.Texels.Num()!=int64(D.X)*D.Y*D.Z||
        !FMath::IsFinite(Level)||FMath::Abs(Level)>1.e30)return Fail(TEXT("Invalid scalar grid for isosurface extraction."));
    FStudioVolumeIsosurface Out;
    constexpr int32 Tets[6][4]={{0,1,3,7},{0,3,2,7},{0,2,6,7},{0,6,4,7},{0,4,5,7},{0,5,1,7}};
    const FVector Spacing=Grid.SourceBounds.GetSize()/FVector(D-FIntVector(1));
    auto Emit=[&](const FVector& A,const FVector& B,const FVector& C0)
    {
        if(FVector::CrossProduct(B-A,C0-A).SizeSquared()<1.e-30)return;
        const int32 Base=Out.PositionsMeters.Num();Out.PositionsMeters.Append({A,B,C0});Out.Indices.Append({Base,Base+1,Base+2});
    };
    for(int32 Z=0;Z<D.Z-1;++Z)for(int32 Y=0;Y<D.Y-1;++Y)
    {
        if(StudioVolumePrivate::Cancelled(C))return Fail(TEXT("Isosurface extraction cancelled."));
        for(int32 X=0;X<D.X-1;++X)
        {
            const FVector CellMinimum=Grid.SourceBounds.Min+FVector(X,Y,Z)*Spacing;
            const double DX=FMath::Clamp(Grid.CylinderCenter.X,CellMinimum.X,CellMinimum.X+Spacing.X)-Grid.CylinderCenter.X;
            const double DY=FMath::Clamp(Grid.CylinderCenter.Y,CellMinimum.Y,CellMinimum.Y+Spacing.Y)-Grid.CylinderCenter.Y;
            if(DX*DX+DY*DY<Grid.CylinderRadius*Grid.CylinderRadius)continue;
            FVector P[8];double V[8];bool Valid=true;
            for(int32 I=0;I<8;++I)
            {
                const FIntVector N(X+(I&1),Y+((I>>1)&1),Z+((I>>2)&1));
                const auto Texel=Grid.Texels[N.X+D.X*(N.Y+D.Y*N.Z)];
                if(!FMath::IsFinite(Texel.X)||!FMath::IsFinite(Texel.Y))return Fail(TEXT("Nonfinite volume scalar."));
                if(Texel.Y<.5){Valid=false;break;}
                V[I]=Texel.X;P[I]=Grid.SourceBounds.Min+FVector(N)*Spacing;
            }
            if(!Valid)continue;
            for(const auto& Tet:Tets)
            {
                int32 Below[4],Above[4],NBelow=0,NAbove=0;
                for(int32 I:Tet)if(V[I]<Level)Below[NBelow++]=I;else Above[NAbove++]=I;
                if(!NBelow||!NAbove)continue;
                auto Edge=[&](int32 A,int32 B){return FMath::Lerp(P[A],P[B],(Level-V[A])/(V[B]-V[A]));};
                if(NBelow==1)Emit(Edge(Below[0],Above[0]),Edge(Below[0],Above[1]),Edge(Below[0],Above[2]));
                else if(NAbove==1)Emit(Edge(Above[0],Below[2]),Edge(Above[0],Below[1]),Edge(Above[0],Below[0]));
                else
                {
                    const FVector A=Edge(Below[0],Above[0]),B=Edge(Below[0],Above[1]),
                        C0=Edge(Below[1],Above[0]),E=Edge(Below[1],Above[1]);
                    Emit(A,B,C0);Emit(B,E,C0);
                }
                if(Out.Indices.Num()>900000)return Fail(TEXT("Isosurface exceeds the 300,000-triangle display budget. Use a coarser display grid or a different value."));
            }
        }
    }
    return Out;
}
