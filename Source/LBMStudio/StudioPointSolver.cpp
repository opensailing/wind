#include "StudioModel.h"
#include "StudioPointRecording.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeLock.h"
#include <limits>

namespace
{
class FPointField final : public IStudioField
{
public:
    FPointField(TSharedPtr<const FStudioPointFrame,ESPMode::ThreadSafe> InFrame,
        TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> InSurface,
        TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> InVolume,
        TSharedRef<FStudioPointRecording,ESPMode::ThreadSafe> InRecording)
        : Frame(MoveTemp(InFrame)), Surface(MoveTemp(InSurface)), Volume(MoveTemp(InVolume)), Recording(MoveTemp(InRecording)) {}
    bool IsValid() const override { return Frame.IsValid(); }
    TOptional<FStudioFieldIdentity> Identity() const override
    {
        if(!Frame||!Frame->Descriptor->Frames.IsValidIndex(Frame->Ordinal))return {};
        FStudioFieldIdentity I;I.Dataset=Frame->Descriptor->Id;I.MetadataSHA256=Frame->Descriptor->MetadataSHA256;
        I.Ordinal=Frame->Ordinal;I.Frame=Frame->Descriptor->Frames[I.Ordinal];I.SpatialDimensions=Frame->Descriptor->SpatialDimensions;
        if(Surface){I.Interpolation=EStudioFieldInterpolation::ReconstructedTriangles;I.ReconstructionSHA256=Surface->MetadataSHA256;}
        if(Volume){I.Interpolation=EStudioFieldInterpolation::ReconstructedGrid;I.ReconstructionSHA256=Volume->MetadataSHA256;}
        return I;
    }
    TOptional<FStudioScalarDescriptor> Scalar(const FString& Id) const override
    {
        if(Frame)if(const auto* S=Frame->Descriptor->FindField(Id))return FStudioScalarDescriptor{S->Id,S->Label,S->Unit,S->Minimum,S->Maximum,S->Origin};
        return {};
    }
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> LoadScalarSnapshot(const FString& Id,
        const FStudioLoadCancellation& Cancellation,FString& Error) const override
    {
        if(!Frame||!Frame->Descriptor->FindField(Id))
        {Error=TEXT("The requested scalar is not supplied by this recording.");return {};}
        const auto Read=Recording->ReadFrame(Frame->Ordinal,{Id},Cancellation);
        Error=Read.Error;
        if(!Read.Frame)return {};
        return MakeShared<FPointField,ESPMode::ThreadSafe>(Read.Frame,Surface,Volume,Recording);
    }
    // A point recording has optional arrays, never the legacy complete tuple.
    bool Sample(const FVector&,FStudioFieldValue& Out) const override { Out={};return false; }
    bool SampleScalar(const FVector& P,const FString& Id,double& Out) const override
    {
        Out=std::numeric_limits<double>::quiet_NaN();
        if(Frame&&Volume)
        {const auto* Values=Frame->FindValues(Id);return Values&&Volume->Sample(FVector(P.X,P.Z,P.Y),*Values,Out);}
        if(!OnSurfacePlane(P))return false;
        const auto* Values=Frame->FindValues(Id);
        return Values&&Surface->Surface->Sample(FVector2D(P.X,P.Z),*Values,Out);
    }
    bool SampleVelocity(const FVector& P,FVector& Out) const override
    {
        Out=FVector(std::numeric_limits<double>::quiet_NaN());
        if(!Volume&&!OnSurfacePlane(P))return false;
        if(!Frame)return false;
        const FStudioPointFieldDescriptor* X=nullptr;
        const FStudioPointFieldDescriptor* Z=nullptr;
        const FStudioPointFieldDescriptor* Y=nullptr;
        for(const auto& F:Frame->Descriptor->Fields)
        {
            if(F.Vector!=TEXT("velocity")||F.Unit!=TEXT("m/s"))continue;
            if(F.Component==TEXT("x"))X=&F;
            else if(F.Component==TEXT("y"))Y=&F;
            else if(F.Component==TEXT("z"))Z=&F;
        }
        double U,V;
        if(!X||!Y||!SampleScalar(P,X->Id,U)||!SampleScalar(P,Y->Id,V))return false;
        double W=0;if(Volume&&(!Z||!SampleScalar(P,Z->Id,W)))return false;
        Out=FVector(U,W,V); // Source XYZ maps to scene XZY.
        return true;
    }
    // No material classification is supplied by a point recording. Triangle
    // coverage, including the explicitly reconstructed hole, governs sampling.
    bool IsSolid(const FVector& P) const override { return Volume&&Volume->IsSolid(FVector(P.X,P.Z,P.Y)); }
    bool SupportsSegment(const FVector& A,const FVector& B,const FStudioLoadCancellation& Cancellation) const override
    {
        if(!Frame||A.ContainsNaN()||B.ContainsNaN())return false;
        if(Volume)
        {
            FBox Region(ForceInit);Region+=FVector(A.X,A.Z,A.Y);Region+=FVector(B.X,B.Z,B.Y);
            return Volume->SupportsRegion(Region,Cancellation);
        }
        return OnSurfacePlane(A)&&OnSurfacePlane(B)&&
            Surface->Surface->SupportsSegment(FVector2D(A.X,A.Z),FVector2D(B.X,B.Z),Cancellation);
    }
    const TArray<FVector2D>& Boundary() const override { static const TArray<FVector2D> Empty;return Empty; }
    const TArray<FIntVector>& BoundaryTriangles() const override { static const TArray<FIntVector> Empty;return Empty; }
    TSharedPtr<const FStudioPointFrame,ESPMode::ThreadSafe> OriginalPoints() const override { return Frame; }
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Reconstruction() const override { return Surface; }
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> VolumeReconstruction() const override { return Volume; }
private:
    bool OnSurfacePlane(const FVector& P) const
    {
        return Frame&&Surface&&Surface->Surface&&Frame->Descriptor->SpatialDimensions==2&&
            P.Y==0&&FMath::IsFinite(P.X)&&FMath::IsFinite(P.Z);
    }
    TSharedPtr<const FStudioPointFrame,ESPMode::ThreadSafe> Frame;
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Surface;
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> Volume;
    TSharedRef<FStudioPointRecording,ESPMode::ThreadSafe> Recording;
};
FVector DisplayPoint(const FVector& P) { return FVector(P.X,P.Z,P.Y); }
FString CSVCell(FString Value) { Value.ReplaceInline(TEXT("\""),TEXT("\"\""));return TEXT("\"")+Value+TEXT("\""); }
}

FPointRecordedSolver::FPointRecordedSolver(TSharedRef<FStudioPointRecording,ESPMode::ThreadSafe> InRecording,
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> InReconstruction,
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> InVolume)
    : Recording(MoveTemp(InRecording)), SurfaceReconstruction(MoveTemp(InReconstruction)), Volume(MoveTemp(InVolume))
{
    const auto& D=Recording->Descriptor();
    Meta.Id=D.Id;Meta.Title=D.Title;Meta.SourceURL=D.SourceURL;Meta.SourceLabel=D.Id;
    Meta.MetadataSHA256=D.MetadataSHA256;
    Meta.SpatialDimensions=D.SpatialDimensions;Meta.NodeCount=D.PointCount;Meta.Frames=D.Frames;
    Meta.TimeNote=D.TimeOrigin;Meta.DefaultScalar=D.DefaultScalar;Meta.bSourcePoints=true;
    Meta.FieldNote=D.SpatialDimensions==2?
        TEXT("Original 2D points on the X/Z plane. View depth adds no spanwise data. No supplied mesh or solid boundary."):
        TEXT("Original 3D points. No supplied mesh or solid boundary.");
    if(SurfaceReconstruction)
        Meta.FieldNote=TEXT("Original 2D values on an explicitly reconstructed X/Z surface. Derived connectivity and boundary; no spanwise data.");
    if(Volume)Meta.FieldNote=TEXT("Original 3D source values on an explicitly reconstructed display grid. Probes use derived grid interpolation; raw CSV retains the original points.");
    Meta.DisplayBounds=FBox(DisplayPoint(D.SourceBounds.Min),DisplayPoint(D.SourceBounds.Max));
    if(D.SpatialDimensions==2)
    {
        const double Depth=FMath::Max(Meta.DisplayBounds.GetSize().GetMax()*.2,.00001);
        Meta.DisplayBounds.Min.Y=-Depth;Meta.DisplayBounds.Max.Y=Depth;
    }
    // A renderer cannot fit infinite/degenerate view extents even when every individual coordinate is finite.
    const auto Size=Meta.DisplayBounds.GetSize();
    if(Size.ContainsNaN()||Size.GetMin()<=0||Size.GetMax()>1.e8)
        Error=TEXT("Point coordinates cannot form a finite, non-degenerate viewing region.");
    Meta.Scalars.Reset();TSet<FString> VelocityComponents;
    for(const auto& F:D.Fields)
    {
        Meta.Scalars.Add({F.Id,F.Label,F.Unit,F.Minimum,F.Maximum,F.Origin});
        if(F.Vector==TEXT("velocity")&&F.Unit==TEXT("m/s"))VelocityComponents.Add(F.Component);
    }
    Meta.bPointVelocity=VelocityComponents.Contains(TEXT("x"))&&VelocityComponents.Contains(TEXT("y"))&&
        (D.SpatialDimensions==2||VelocityComponents.Contains(TEXT("z")));
}
void FPointRecordedSolver::SetError(const FString& Message) const { FScopeLock Lock(&ErrorMutex);Error=Message; }
FString FPointRecordedSolver::LoadError() const { FScopeLock Lock(&ErrorMutex);return Error; }
int32 FPointRecordedSolver::FrameCount() const { return Meta.Frames.Num(); }
FStudioFrame FPointRecordedSolver::EvaluateFrame(int32 Ordinal) const { return Meta.Frames.IsValidIndex(Ordinal)?Meta.Frames[Ordinal]:FStudioFrame(); }
TArray<FString> FPointRecordedSolver::RequestedFields(const FString& ScalarId,bool bVectors) const
{
    const auto& D=Recording->Descriptor();TArray<FString> Fields;
    Fields.Add(D.FindField(ScalarId)?ScalarId:D.DefaultScalar);
    if(bVectors&&Meta.bPointVelocity)for(const auto& F:D.Fields)
        if(F.Vector==TEXT("velocity"))Fields.AddUnique(F.Id);
    return Fields;
}
bool FPointRecordedSolver::PrepareFrame(int32 Ordinal,const FStudioLoadCancellation& Cancellation)
{
    if(!LoadError().IsEmpty())return false;
    const auto R=Recording->ReadFrame(Ordinal,RequestedFields(Meta.DefaultScalar,true),Cancellation);
    SetError(R.Error);return R.Frame.IsValid();
}
TSharedRef<const IStudioField,ESPMode::ThreadSafe> FPointRecordedSolver::CaptureField(int32 Ordinal) const
{ return CaptureViewField(Ordinal,Meta.DefaultScalar,true); }
TSharedRef<const IStudioField,ESPMode::ThreadSafe> FPointRecordedSolver::CaptureViewField(int32 Ordinal,const FString& ScalarId,
    bool bVectors,const FStudioLoadCancellation& Cancellation) const
{
    const auto R=Recording->ReadFrame(Ordinal,RequestedFields(ScalarId,bVectors),Cancellation);
    if(!Cancellation||!Cancellation->load())SetError(R.Error);
    return MakeShared<FPointField,ESPMode::ThreadSafe>(R.Frame,SurfaceReconstruction,Volume,Recording);
}
FStudioFieldReadResult FPointRecordedSolver::ReadScalarFrame(int32 Ordinal,const FString& ScalarId,
    const FStudioLoadCancellation& Cancellation) const
{
    FStudioFieldReadResult Out;
    if(!Meta.Frames.IsValidIndex(Ordinal)||!Recording->Descriptor().FindField(ScalarId))
    {Out.Error=TEXT("The exact requested frame or scalar is not supplied by this recording.");return Out;}
    const auto Read=Recording->ReadFrame(Ordinal,{ScalarId},Cancellation);Out.Error=Read.Error;
    if(Cancellation&&Cancellation->load())Out.Error=TEXT("Recorded-frame analysis cancelled.");
    else if(Read.Frame)Out.Field=MakeShared<FPointField,ESPMode::ThreadSafe>(Read.Frame,SurfaceReconstruction,Volume,Recording);
    else if(Out.Error.IsEmpty())Out.Error=TEXT("Could not read the requested recorded frame.");
    return Out;
}
FStudioFrameCacheStats FPointRecordedSolver::CacheStats() const
{
    const auto S=Recording->Stats();FStudioFrameCacheStats Out;
    Out.BudgetBytes=S.CacheBudgetBytes;Out.ResidentBytes=S.CacheBytes;Out.Loads=S.Loads;Out.Hits=S.Hits;
    // A partial scalar-array cache is not a count of complete CFD frames.
    return Out;
}
bool FPointRecordedSolver::ExportField(int32 Ordinal,const FString& Path) const
{
    TArray<FString> Fields;for(const auto& F:Recording->Descriptor().Fields)Fields.Add(F.Id);
    const auto R=Recording->ReadFrame(Ordinal,Fields);if(!R.Frame){SetError(R.Error);return false;}
    FStudioFileAccess Access(Path);TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*Path));
    if(!Writer)return false;
    auto Write=[&](const FString& Row){const FTCHARToUTF8 Bytes(*Row);Writer->Serialize(const_cast<char*>(Bytes.Get()),Bytes.Length());return !Writer->IsError();};
    const auto& D=*R.Frame->Descriptor;
    FString Header=TEXT("dataset_id,metadata_sha256,source_step,source_label,source_time_s,point_id,source_x_m,source_y_m");
    if(D.SpatialDimensions==3)Header+=TEXT(",source_z_m");
    for(const auto& F:D.Fields)Header+=TEXT(",")+CSVCell(F.Id+TEXT(" [")+F.Unit+TEXT("]"));
    if(!Write(Header+TEXT("\n")))return false;
    const auto Frame=D.Frames[Ordinal];
    for(int32 I=0;I<D.PointCount;++I)
    {
        const auto P=R.Frame->Geometry->Positions[I];
        FString Row=FString::Printf(TEXT("%s,%s,%d,%s,%.17g,%lld,%.17g,%.17g"),*D.Id,*D.MetadataSHA256,Frame.Index,
            *CSVCell(D.FrameLabels[Ordinal]),Frame.Time,R.Frame->Geometry->PointIds[I],P.X,P.Y);
        if(D.SpatialDimensions==3)Row+=FString::Printf(TEXT(",%.17g"),P.Z);
        for(const auto& F:D.Fields)Row+=FString::Printf(TEXT(",%.17g"),(*R.Frame->FindValues(F.Id))[I]);
        if(!Write(Row+TEXT("\n")))return false;
    }
    Writer->Close();return !Writer->IsError();
}
