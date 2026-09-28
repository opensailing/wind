#include "StudioDomain.h"
#include "StudioMaterials.h"
#include "StudioColor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FStudioDomainGeometry::Complete() const
{
    if(bCancelled||Objects.IsEmpty()||!Bounds.IsValid)return false;
    for(const auto& Object:Objects)if(!Object.Error.IsEmpty()||!Object.Bounds.IsValid)return false;
    return true;
}
FString StudioDomain::GeometryKey(const FStudioCaseDraft& Case)
{
    FString Key;auto GeometryOnly=Case; for(auto& Asset:GeometryOnly.Geometry)Asset.MaterialId.Invalidate();
    const auto Json=StudioCaseIO::ToJSON(GeometryOnly);
    FJsonSerializer::Serialize(Json->GetArrayField(TEXT("geometry")),TJsonWriterFactory<>::Create(&Key));return Key;
}
FStudioDomainGeometry StudioDomain::InspectGeometry(const FStudioCaseDraft& Case,const FStudioAssetCancellation& Cancel)
{
    FStudioDomainGeometry Result;Result.Key=GeometryKey(Case);
    auto Preview=MakeShared<FStudioImportedMesh,ESPMode::ThreadSafe>();
    for(const auto& Asset:Case.Geometry)
    {
        if(Cancel->load()){Result.bCancelled=true;return Result;}
        FStudioDomainGeometryEntry Entry;Entry.Id=Asset.Id;Entry.Name=Asset.Name;
        const auto Source=StudioMeshImport::Read(Asset.SourcePath,Cancel);
        if(Source.bCancelled){Result.bCancelled=true;return Result;}
        if(!Source.IsValid())Entry.Error=Source.Error;
        else if(!Asset.SourceSHA256.Equals(Source.SHA256,ESearchCase::IgnoreCase)||Asset.Format!=Source.Format)Entry.Error=TEXT("Source changed. Locate the original geometry or import the changed file separately.");
        else
        {
            bool PatchMatch=Asset.Patches.Num()==Source.Mesh->PatchNames.Num();
            if(PatchMatch)for(int32 I=0;I<Asset.Patches.Num();++I)PatchMatch&=Asset.Patches[I].Name==Source.Mesh->PatchNames[I];
            PatchMatch&=Source.Mesh->Indices.Num()%3==0&&Source.Mesh->TrianglePatches.Num()==Source.Mesh->Indices.Num()/3;
            if(PatchMatch)for(int32 Patch:Source.Mesh->TrianglePatches)if(!Asset.Patches.IsValidIndex(Patch)){PatchMatch=false;break;}
            if(!PatchMatch){Entry.Error=TEXT("The saved surface patches differ from the original geometry. Locate the original file.");Result.Objects.Add(MoveTemp(Entry));continue;}
            Entry.bClosedSurface=Source.Mesh->BoundaryEdges==0&&Source.Mesh->NonmanifoldEdges==0&&
                Source.Mesh->InconsistentEdges==0&&Source.Mesh->DuplicateFaces==0;
            // Check transformed positions in cancellable batches, including objects omitted from the preview budget.
            int32 Checked=0;
            for(const auto& Position:Source.Mesh->Positions)
            {
                if((Checked++&1023)==0&&Cancel->load()){Result.bCancelled=true;return Result;}
                const FVector World=StudioMeshImport::TransformPosition(Position,Asset);
                if(World.ContainsNaN()||World.GetAbsMax()>1.e8){Entry.Error=TEXT("Geometry lies outside the supported domain coordinate range of ±1e8 m.");break;}
                Entry.Bounds+=World;
            }
            if(!Entry.Error.IsEmpty()){Entry.Bounds=FBox(ForceInit);Result.Objects.Add(MoveTemp(Entry));continue;}
            Result.Bounds+=Entry.Bounds;
            const bool Fits=int64(Preview->Positions.Num())+Source.Mesh->Positions.Num()<=MaximumPreviewVertices&&
                int64(Preview->Indices.Num())+Source.Mesh->Indices.Num()<=int64(MaximumPreviewTriangles)*3;
            if(Fits)
            {
                Entry.bInPreview=true;
                const int32 Offset=Preview->Positions.Num();int32 I=0;
                for(const auto& Position:Source.Mesh->Positions)
                {if((I++&1023)==0&&Cancel->load()){Result.bCancelled=true;return Result;}Preview->Positions.Add(StudioMeshImport::TransformPosition(Position,Asset));}
                for(int32 Index:Source.Mesh->Indices)
                {if((I++&1023)==0&&Cancel->load()){Result.bCancelled=true;return Result;}Preview->Indices.Add(Offset+Index);}
                for(int32 Triangle=0;Triangle<Source.Mesh->Indices.Num()/3;++Triangle)
                {
                    if((Triangle&1023)==0&&Cancel->load()){Result.bCancelled=true;return Result;}
                    const int32 Patch=Source.Mesh->TrianglePatches[Triangle];
                    const FGuid Id=Asset.Patches[Patch].Id;Result.TriangleTargets.Add(Id);
                    FBox* Bounds=Result.PatchBounds.Find(Id);
                    if(!Bounds)Bounds=&Result.PatchBounds.Add(Id,FBox(ForceInit));
                    for(int32 Vertex=0;Vertex<3;++Vertex)*Bounds+=Preview->Positions[Offset+Source.Mesh->Indices[Triangle*3+Vertex]];
                }
            }
            else Result.bPreviewLimited=true;
        }
        Result.Objects.Add(MoveTemp(Entry));
    }
    Preview->Bounds=Result.Bounds;Result.Preview=Preview;return Result;
}
bool StudioDomain::Contains(const FStudioDomain& Domain,const FBox& Bounds)
{
    if(!Bounds.IsValid)return false;
    return Bounds.Min.X>=Domain.Min.X&&Bounds.Min.Y>=Domain.Min.Y&&Bounds.Min.Z>=Domain.Min.Z&&
        Bounds.Max.X<=Domain.Max.X&&Bounds.Max.Y<=Domain.Max.Y&&Bounds.Max.Z<=Domain.Max.Z;
}
bool StudioDomain::Padded(const FBox& Bounds,const double (&Padding)[6],FVector& Min,FVector& Max,FString& Error)
{
    if(!Bounds.IsValid){Error=TEXT("Check the case geometry before fitting the domain.");return false;}
    for(double Value:Padding)if(!FMath::IsFinite(Value)||Value<0||Value>1.e8)
    {Error=TEXT("Padding must be a nonnegative finite distance, at most 1e8 m.");return false;}
    const FVector Low=Bounds.Min-FVector(Padding[0],Padding[2],Padding[4]);
    const FVector High=Bounds.Max+FVector(Padding[1],Padding[3],Padding[5]);
    if(Low.ContainsNaN()||High.ContainsNaN()||Low.GetAbsMax()>1.e8||High.GetAbsMax()>1.e8||
        Low.X>=High.X||Low.Y>=High.Y||Low.Z>=High.Z)
    {Error=TEXT("The padded domain must have positive X, Y and Z dimensions within ±1e8 m. Add padding to any planar direction.");return false;}
    Min=Low;Max=High;Error.Empty();return true;
}
FVector StudioDomain::FaceCenter(const FStudioDomain& Domain,int32 Index)
{
    FVector Center=(Domain.Min+Domain.Max)*.5;
    if(Index>=0&&Index<6)Center[Index/2]=Index%2?Domain.Max[Index/2]:Domain.Min[Index/2];
    return Center;
}
void FStudioDomainEdit::Reset(const FStudioDomain& Domain)
{
    Saved=Domain;Error.Empty();ErrorField=INDEX_NONE;
    for(int32 I=0;I<3;++I)bDimensionInput[I]=false;
    for(int32 I=0;I<3;++I){Minimum[I]=StudioMaterials::ExactNumber(Domain.Min[I]);Maximum[I]=StudioMaterials::ExactNumber(Domain.Max[I]);}
    for(int32 I=0;I<6;++I){FaceNames[I]=Domain.FaceNames.IsValidIndex(I)?Domain.FaceNames[I]:FString();if(Padding[I].IsEmpty())Padding[I]=TEXT("0");}
    RefreshDimensions();
}
bool FStudioDomainEdit::Matches(const FStudioDomain& Domain) const
{return Saved.Id==Domain.Id&&Saved.Min==Domain.Min&&Saved.Max==Domain.Max&&Saved.FaceNames==Domain.FaceNames&&Saved.Faces==Domain.Faces;}
bool FStudioDomainEdit::IsDirty() const
{
    for(int32 I=0;I<3;++I)if(bDimensionInput[I]&&Dimensions[I]!=StudioMaterials::ExactNumber(Saved.Max[I]-Saved.Min[I]))return true;
    for(int32 I=0;I<3;++I)if(Minimum[I]!=StudioMaterials::ExactNumber(Saved.Min[I])||Maximum[I]!=StudioMaterials::ExactNumber(Saved.Max[I]))return true;
    for(int32 I=0;I<6;++I)if(!Saved.FaceNames.IsValidIndex(I)||FaceNames[I]!=Saved.FaceNames[I])return true;
    return false;
}
bool FStudioDomainEdit::Build(FStudioDomain& Out)
{
    FStudioDomain Domain=Saved;Error.Empty();ErrorField=INDEX_NONE;
    for(int32 I=0;I<3;++I)if(bDimensionInput[I])
    {
        double Size=0;
        if(!StudioColor::ParseNumber(Dimensions[I],Size)||Size<=0||Size>2.e8)
        {Error=FString::Printf(TEXT("%c dimension: enter a positive finite distance, at most 2e8 m."),TEXT("XYZ")[I]);ErrorField=18+I;return false;}
    }
    for(int32 I=0;I<3;++I)
    {
        double Low=0,High=0;
        if(!StudioColor::ParseNumber(Minimum[I],Low)||FMath::Abs(Low)>1.e8)
        {Error=FString::Printf(TEXT("%c minimum: enter a finite coordinate from -1e8 to 1e8 m."),TEXT("XYZ")[I]);ErrorField=I*2;return false;}
        if(bDimensionInput[I])
        {
            double Size=0;StudioColor::ParseNumber(Dimensions[I],Size);const double Expected=Low+Size;
            if(!FMath::IsFinite(Expected)||Expected<=Low||FMath::Abs(Expected)>1.e8)
            {Error=TEXT("This dimension places the maximum outside ±1e8 m, or is too small at this coordinate. Reduce the minimum or dimension.");ErrorField=18+I;return false;}
        }
        if(!StudioColor::ParseNumber(Maximum[I],High)||FMath::Abs(High)>1.e8||High<=Low)
        {Error=FString::Printf(TEXT("%c maximum: enter a finite coordinate greater than the minimum, at most 1e8 m."),TEXT("XYZ")[I]);ErrorField=I*2+1;return false;}
        Domain.Min[I]=Low;Domain.Max[I]=High;
    }
    Domain.FaceNames.Reset();TSet<FString> Used;
    for(int32 I=0;I<6;++I)
    {
        const FString Name=FaceNames[I].TrimStartAndEnd();
        if(Name.IsEmpty()||Name.Len()>120||Used.Contains(Name.ToLower()))
        {Error=TEXT("Face names: enter six unique names with 1–120 characters each.");ErrorField=6+I;return false;}
        Used.Add(Name.ToLower());Domain.FaceNames.Add(Name);
    }
    Out=MoveTemp(Domain);return true;
}
bool FStudioDomainEdit::Fit(const FBox& Bounds)
{
    double Distances[6];Error.Empty();ErrorField=INDEX_NONE;
    for(int32 I=0;I<6;++I)if(!StudioColor::ParseNumber(Padding[I],Distances[I])||Distances[I]<0||Distances[I]>1.e8)
    {Error=TEXT("Enter six nonnegative padding distances in meters.");ErrorField=12+I;return false;}
    FVector Min,Max;if(!StudioDomain::Padded(Bounds,Distances,Min,Max,Error))return false;
    for(int32 I=0;I<3;++I){SetCoordinate(I*2,StudioMaterials::ExactNumber(Min[I]));SetCoordinate(I*2+1,StudioMaterials::ExactNumber(Max[I]));}
    return true;
}

void FStudioDomainEdit::RefreshDimensions()
{
    for(int32 I=0;I<3;++I)if(!bDimensionInput[I])
    {double Low=0,High=0;Dimensions[I]=StudioColor::ParseNumber(Minimum[I],Low)&&StudioColor::ParseNumber(Maximum[I],High)?StudioMaterials::ExactNumber(High-Low):FString();}
}
void FStudioDomainEdit::SetCoordinate(int32 Index,const FString& Text)
{
    if(Index<0||Index>=6)return;
    (Index%2?Maximum[Index/2]:Minimum[Index/2])=Text;bDimensionInput[Index/2]=false;RefreshDimensions();
}
void FStudioDomainEdit::SetDimension(int32 Axis,const FString& Text)
{
    if(Axis<0||Axis>=3)return;
    Dimensions[Axis]=Text;bDimensionInput[Axis]=true;double Low=0,Size=0;
    if(StudioColor::ParseNumber(Minimum[Axis],Low)&&StudioColor::ParseNumber(Text,Size)&&Size>0&&Size<=2.e8)
    {
        const double High=Low+Size;
        if(FMath::IsFinite(High)&&High>Low&&FMath::Abs(High)<=1.e8)
            Maximum[Axis]=Minimum[Axis]==StudioMaterials::ExactNumber(Saved.Min[Axis])&&Text==StudioMaterials::ExactNumber(Saved.Max[Axis]-Saved.Min[Axis])?
                StudioMaterials::ExactNumber(Saved.Max[Axis]):StudioMaterials::ExactNumber(High);
    }
}
namespace
{
    bool DomainAxisDistance(const StudioCameraPlacement::FRay& Ray,const FVector& Center,int32 Axis,double& Out)
    {
        const double K=Ray.Direction[Axis],Denominator=1-K*K;if(Denominator<1.e-6)return false;
        const FVector Offset=Ray.Origin-Center;Out=(Offset[Axis]-K*FVector::DotProduct(Ray.Direction,Offset))/Denominator;
        return FMath::IsFinite(Out);
    }
}
bool StudioDomain::BeginFaceDrag(const FStudioDomain& Domain,int32 Face,const FStudioCameraState& Observer,
    FVector2D Viewport,FVector2D Pixel,FFaceDrag& Out,double ProjectionAspect)
{
    if(Face<0||Face>=6||Domain.Min.ContainsNaN()||Domain.Max.ContainsNaN()||Domain.Min.GetAbsMax()>1.e8||Domain.Max.GetAbsMax()>1.e8||
        Domain.Min.X>=Domain.Max.X||Domain.Min.Y>=Domain.Max.Y||Domain.Min.Z>=Domain.Max.Z)return false;
    StudioCameraPlacement::FRay Ray;if(!StudioCameraPlacement::Ray(Observer,Viewport,Pixel,Ray,ProjectionAspect))return false;
    FFaceDrag Start;Start.Domain=Domain;Start.Face=Face;Start.Observer=Observer;Start.Viewport=Viewport;Start.ProjectionAspect=ProjectionAspect;
    if(!DomainAxisDistance(Ray,FaceCenter(Domain,Face),Face/2,Start.StartDistance))return false;
    Out=Start;return true;
}
bool StudioDomain::DragFace(const FFaceDrag& Start,FVector2D Pixel,FStudioDomain& Out)
{
    if(Start.Face<0||Start.Face>=6)return false;StudioCameraPlacement::FRay Ray;
    if(!StudioCameraPlacement::Ray(Start.Observer,Start.Viewport,Pixel,Ray,Start.ProjectionAspect))return false;
    double Distance=0;if(!DomainAxisDistance(Ray,FaceCenter(Start.Domain,Start.Face),Start.Face/2,Distance))return false;
    const int32 Axis=Start.Face/2;auto Domain=Start.Domain;
    double& Coordinate=Start.Face%2?Domain.Max[Axis]:Domain.Min[Axis];Coordinate+=Distance-Start.StartDistance;
    if(!FMath::IsFinite(Coordinate)||FMath::Abs(Coordinate)>1.e8||Domain.Max[Axis]<=Domain.Min[Axis])return false;
    Out=MoveTemp(Domain);return true;
}
