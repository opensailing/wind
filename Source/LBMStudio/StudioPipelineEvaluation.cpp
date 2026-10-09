#include "StudioPipelineEvaluation.h"
#include "StudioSliceRendering.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "Async/Async.h"

namespace StudioPipelineEvaluationPrivate
{
bool Cancelled(const FStudioLoadCancellation& C){return C&&C->load(std::memory_order_relaxed);}
FBox Intersection(const FBox& A,const FBox& B)
{
    if(!A.IsValid||!B.IsValid)return FBox(ForceInit);
    const FVector L=A.Min.ComponentMax(B.Min),H=A.Max.ComponentMin(B.Max);
    return L.X<=H.X&&L.Y<=H.Y&&L.Z<=H.Z?FBox(L,H):FBox(ForceInit);
}
void Discard(FStudioPipelineEvaluationResult& R)
{R.Output.Reset();R.Prepared.Field.Reset();R.Prepared.Source.Reset();R.Prepared.Stages.Reset();}
struct FBuilder
{
    const FStudioPipelinePrepared& Prepared;
    const FStudioLoadCancellation& Cancellation;
    FStudioPipelineOutput Out;
    FString Error;
    const FStudioPipelineField& Field() const {return *Prepared.Field;}
    bool Fail(const TCHAR* Why){Error=Why;return false;}
    bool Capacity(int32 Vertices,int32 Triangles=0,int32 Lines=0)
    {
        if(Cancelled(Cancellation))return false;
        if(Out.Vertices.Num()+Vertices>StudioPipelineEvaluation::MaxOutputVertices||
            Out.Triangles.Num()+Triangles>StudioPipelineEvaluation::MaxOutputTriangles||Out.Lines.Num()+Lines>StudioPipelineEvaluation::MaxOutputLines)
            return Fail(TEXT("Pipeline geometry exceeds the output budget. Use a smaller clip region or a different contour value."));
        return true;
    }
    FStudioPipelineVertex Interpolate(const FStudioPipelineVertex& A,const FStudioPipelineVertex& B,double T)
    {
        if(T<=0)return A;if(T>=1)return B;
        FStudioPipelineVertex V;V.PositionMeters=FMath::Lerp(A.PositionMeters,B.PositionMeters,T);V.Scalar=FMath::Lerp(A.Scalar,B.Scalar,T);return V;
    }
    TArray<FStudioPipelineVertex> Clip(TArray<FStudioPipelineVertex> Polygon,const FBox& Box,bool Resample)
    {
        if(!Box.IsValid)return {};
        for(int32 Axis=0;Axis<3;++Axis)for(int32 Side=0;Side<2;++Side)
        {
            if(Polygon.IsEmpty())return {};TArray<FStudioPipelineVertex> Result;
            const double Plane=Side?Box.Max[Axis]:Box.Min[Axis];
            auto Distance=[&](const FStudioPipelineVertex& V){return Side?Plane-V.PositionMeters[Axis]:V.PositionMeters[Axis]-Plane;};
            auto A=Polygon.Last();double DA=Distance(A);
            for(const auto& B:Polygon)
            {
                const double DB=Distance(B);
                if((DA>=0)!=(DB>=0))
                {
                    auto V=Interpolate(A,B,DA/(DA-DB));V.PositionMeters[Axis]=Plane;
                    Result.Add(V);
                }
                if(DB>=0)Result.Add(B);A=B;DA=DB;
            }
            Polygon=MoveTemp(Result);
        }
        // Intermediate intersections may still be outside another clipping
        // plane (or the reader's display bounds). Sample only retained derived
        // positions after the complete geometric intersection.
        if(Resample)for(auto& V:Polygon)if(V.OriginalRow==INDEX_NONE&&
            !Field().SampleUnderlyingScalar(V.PositionMeters,Field().SelectedScalar().Id,V.Scalar))
        {Error=TEXT("A clipped surface vertex has no supported original field value.");return {};}
        return Polygon;
    }
    bool Triangle(const FStudioPipelineVertex& A,const FStudioPipelineVertex& B,const FStudioPipelineVertex& C,const FBox& Box,bool Resample)
    {
        const auto Polygon=Clip({A,B,C},Box,Resample);if(!Error.IsEmpty())return false;
        for(int32 I=1;I+1<Polygon.Num();++I)
        {
            if(FVector::CrossProduct(Polygon[I].PositionMeters-Polygon[0].PositionMeters,Polygon[I+1].PositionMeters-Polygon[0].PositionMeters).SizeSquared()<=1.e-30)continue;
            if(!Capacity(3,1))return false;const int32 Base=Out.Vertices.Num();Out.Vertices.Append({Polygon[0],Polygon[I],Polygon[I+1]});Out.Triangles.Add(FIntVector(Base,Base+1,Base+2));
        }
        return true;
    }
    bool Line(FStudioPipelineVertex A,FStudioPipelineVertex B,const FBox& Box)
    {
        if(!Box.IsValid)return true;double Entry=0,Exit=1;const FVector Delta=B.PositionMeters-A.PositionMeters;
        for(int32 Axis=0;Axis<3;++Axis)
        {
            if(Delta[Axis]==0){if(A.PositionMeters[Axis]<Box.Min[Axis]||A.PositionMeters[Axis]>Box.Max[Axis])return true;}
            else
            {
                double L=(Box.Min[Axis]-A.PositionMeters[Axis])/Delta[Axis],H=(Box.Max[Axis]-A.PositionMeters[Axis])/Delta[Axis];if(L>H)Swap(L,H);
                Entry=FMath::Max(Entry,L);Exit=FMath::Min(Exit,H);if(Entry>=Exit)return true;
            }
        }
        const auto P=Interpolate(A,B,Entry),Q=Interpolate(A,B,Exit);
        if((P.PositionMeters-Q.PositionMeters).SizeSquared()<=1.e-30)return true;
        if(!Capacity(2,0,1))return false;const int32 Base=Out.Vertices.Num();Out.Vertices.Append({P,Q});Out.Lines.Add(FIntPoint(Base,Base+1));return true;
    }
    bool Points()
    {
        Out.Kind=EStudioPipelineOutputKind::OriginalPoints;Out.Method=TEXT("Unmodified original point IDs and coordinates, filtered by the ordered clip domain; scalar expressions retain their declared origin.");
        if(Field().HasEmptyDomain())return true;
        Out.Vertices.Reserve(Field().OriginalPointCount());
        for(int32 Row=0;Row<Field().OriginalPointCount();++Row)
        {
            if((Row&255)==0&&Cancelled(Cancellation))return false;FStudioPipelineVertex V;FVector P;
            if(!Field().OriginalPoint(Row,V.OriginalPointId,P))return Fail(TEXT("Original pipeline coordinates are unavailable."));
            V.PositionMeters=FVector(P.X,P.Z,P.Y)+Prepared.Recipe.Source.Identity.SourceOffset;if(!Field().Includes(V.PositionMeters))continue;
            V.OriginalRow=Row;if(!Field().OriginalScalar(Row,Field().SelectedScalar().Id,V.Scalar))return Fail(TEXT("Original pipeline values are unavailable."));
            if(!Capacity(1))return false;Out.Vertices.Add(V);
        }
        return true;
    }
    bool Surface(const FBox& ClipBounds,const FBox& SliceBounds)
    {
        Out.Kind=EStudioPipelineOutputKind::Surface;
        if(Field().Slice().IsSet())
        {
            Out.bDerivedGeometry=true;Out.Method=TEXT("Sampled slice grid using verified source interpolation; unsupported and solid cells omitted, followed by ordered box clipping.");
            if(!ClipBounds.IsValid||!SliceBounds.IsValid)return true;
            const auto& S=*Field().Slice();FStudioSliceObject Plane;Plane.Id=S.Id;Plane.Name=S.Name;Plane.Origin=S.A;Plane.Normal=S.B;
            const auto& I=Prepared.Recipe.Source.Identity;Plane.Source={I.Dataset,I.MetadataSHA256,I.PayloadSHA256};
            const auto Query=Field().WithDomain(SliceBounds);auto Mesh=StudioSliceRendering::Build(*Query,SliceBounds,{Plane},Field().SelectedScalar().Id,Cancellation);
            for(int32 T=0;T<Mesh.Indices.Num();T+=3)
            {
                FStudioPipelineVertex V[3];for(int32 K=0;K<3;++K){const int32 Id=Mesh.Indices[T+K];V[K].PositionMeters=Mesh.PositionsMeters[Id];V[K].Scalar=Mesh.Scalars[Id];}
                if(!Triangle(V[0],V[1],V[2],ClipBounds,true))return false;
            }
            return !Cancelled(Cancellation);
        }
        const auto Reconstruction=Field().Reconstruction();
        const int32 Count=Reconstruction?Reconstruction->Surface->TriangleCount():Field().OriginalTriangleCount();
        Out.bDerivedGeometry=Reconstruction.IsValid();Out.Method=Reconstruction?TEXT("Verified reconstructed surface triangles over original CFD rows; box intersections are derived positions."):
            TEXT("Original source triangles and rows; box intersections are derived positions.");
        if(!ClipBounds.IsValid)return true;
        for(int32 T=0;T<Count;++T)
        {
            if((T&255)==0&&Cancelled(Cancellation))return false;
            FIntVector Rows;if(Reconstruction)Rows=Reconstruction->Surface->Triangles()[T];else if(!Field().OriginalTriangle(T,Rows))return Fail(TEXT("Original surface connectivity is unavailable."));
            FStudioPipelineVertex V[3];
            for(int32 K=0;K<3;++K)
            {
                FVector P;V[K].OriginalRow=Rows[K];
                if(!Field().OriginalPoint(Rows[K],V[K].OriginalPointId,P)||!Field().OriginalScalar(Rows[K],Field().SelectedScalar().Id,V[K].Scalar))return Fail(TEXT("Original surface rows are unavailable."));
                V[K].PositionMeters=FVector(P.X,P.Z,P.Y)+Prepared.Recipe.Source.Identity.SourceOffset;
            }
            if(!Triangle(V[0],V[1],V[2],ClipBounds,true))return false;
        }
        return true;
    }
    bool Contour2D(const FStudioPipelineOutput& Mesh,double Level,const FBox& FinalBounds)
    {
        Out.Kind=EStudioPipelineOutputKind::ContourLines;Out.bDerivedGeometry=true;
        Out.Method=TEXT("Linear contour of scalar samples on surface triangles. Derived magnitudes and sampled slices are approximated between triangle vertices; box clipping retains that interpolant.");
        for(const auto& T:Mesh.Triangles)
        {
            if(Cancelled(Cancellation))return false;TArray<FStudioPipelineVertex> Crossing;
            for(int32 K=0;K<3;++K)
            {
                const auto& A=Mesh.Vertices[T[K]];const auto& B=Mesh.Vertices[T[(K+1)%3]];
                if((A.Scalar<Level)==(B.Scalar<Level))continue;
                auto V=Interpolate(A,B,(Level-A.Scalar)/(B.Scalar-A.Scalar));V.Scalar=Level;V.OriginalRow=INDEX_NONE;
                Crossing.Add(V);
            }
            if(Crossing.Num()==2&&!Line(Crossing[0],Crossing[1],FinalBounds))return false;
        }
        return true;
    }
    bool Contour3D(double Level,const FBox& Bounds)
    {
        Out.Kind=EStudioPipelineOutputKind::ContourSurface;Out.bDerivedGeometry=true;
        Out.Method=TEXT("Linear tetrahedral contour of full-precision scalar samples at verified reconstructed grid nodes. Unsupported cells and cylinder intersections are omitted. This is an approximation to the reconstructed trilinear field; box intersections retain the tetrahedral interpolant.");
        if(!Bounds.IsValid)return true;const auto Volume=Field().VolumeReconstruction();
        if(!Volume)return Fail(TEXT("A verified 3D interpolation grid is required for volume contours."));
        const auto D=Volume->Dimensions;const int64 Count=int64(D.X)*D.Y*D.Z;
        if(Count<=0||Count>StudioVolumes::MaximumVoxels)return Fail(TEXT("Volume contour grid exceeds the supported node budget."));
        TArray<double> Values;Values.SetNumZeroed(Count);TBitArray<> Valid(false,Count);
        const FString Scalar=Field().SelectedScalar().Id;
        for(int32 I=0;I<Count;++I){if((I&255)==0&&Cancelled(Cancellation))return false;Valid[I]=Field().SampleVolumeNode(I,Scalar,Values[I]);}
        constexpr int32 Tets[6][4]={{0,1,3,7},{0,3,2,7},{0,2,6,7},{0,6,4,7},{0,4,5,7},{0,5,1,7}};
        const FVector Spacing=Volume->SourceBounds.GetSize()/FVector(D-FIntVector(1));
        for(int32 Z=0;Z<D.Z-1;++Z)for(int32 Y=0;Y<D.Y-1;++Y)for(int32 X=0;X<D.X-1;++X)
        {
            if((X&31)==0&&Cancelled(Cancellation))return false;
            const FVector Low=Volume->SourceBounds.Min+FVector(X,Y,Z)*Spacing,High=Low+Spacing;
            const FBox SceneBox(FVector(Low.X,Low.Z,Low.Y)+Prepared.Recipe.Source.Identity.SourceOffset,FVector(High.X,High.Z,High.Y)+Prepared.Recipe.Source.Identity.SourceOffset);
            if(!Intersection(SceneBox,Bounds).IsValid||Volume->ContainsSolid(FBox(Low,High)))continue;
            FStudioPipelineVertex V[8];bool Supported=true;
            for(int32 I=0;I<8;++I)
            {
                const int32 Node=X+(I&1)+D.X*(Y+((I>>1)&1)+D.Y*(Z+((I>>2)&1)));
                if(!Valid[Node]){Supported=false;break;}const FVector P=Volume->Position(Node);
                V[I].PositionMeters=FVector(P.X,P.Z,P.Y)+Prepared.Recipe.Source.Identity.SourceOffset;V[I].Scalar=Values[Node];
            }
            if(!Supported)continue;
            for(const auto& Tet:Tets)
            {
                int32 Below[4],Above[4],NB=0,NA=0;for(int32 I:Tet)if(V[I].Scalar<Level)Below[NB++]=I;else Above[NA++]=I;
                if(!NB||!NA)continue;
                auto Edge=[&](int32 A,int32 B){auto P=Interpolate(V[A],V[B],(Level-V[A].Scalar)/(V[B].Scalar-V[A].Scalar));P.Scalar=Level;return P;};
                FVector Direction=FVector::ZeroVector;
                for(int32 I=0;I<NA;++I)Direction+=V[Above[I]].PositionMeters/NA;
                for(int32 I=0;I<NB;++I)Direction-=V[Below[I]].PositionMeters/NB;
                auto Emit=[&](FStudioPipelineVertex A,FStudioPipelineVertex B,FStudioPipelineVertex C)
                {
                    if(FVector::DotProduct(FVector::CrossProduct(B.PositionMeters-A.PositionMeters,C.PositionMeters-A.PositionMeters),Direction)<0)Swap(B,C);
                    return Triangle(A,B,C,Bounds,false);
                };
                if(NB==1){if(!Emit(Edge(Below[0],Above[0]),Edge(Below[0],Above[1]),Edge(Below[0],Above[2])))return false;}
                else if(NA==1){if(!Emit(Edge(Above[0],Below[2]),Edge(Above[0],Below[1]),Edge(Above[0],Below[0])))return false;}
                else
                {
                    const auto A=Edge(Below[0],Above[0]),B=Edge(Below[0],Above[1]),C=Edge(Below[1],Above[0]),E=Edge(Below[1],Above[1]);
                    if(!Emit(A,B,C)||!Emit(B,E,C))return false;
                }
            }
        }
        return true;
    }
    bool Build()
    {
        const FStudioPipelineOperation* Contour=nullptr,*Probe=nullptr;FBox Current=Prepared.Source->Descriptor().DisplayBounds,SliceBounds=Current,ContourBounds=Current;
        for(const auto& Op:Prepared.Recipe.Operations)if(Op.bEnabled)
        {
            if(Op.Kind==EStudioPipelineOperation::ClipBox)Current=Intersection(Current,FBox(Op.A,Op.B));
            if(Op.Kind==EStudioPipelineOperation::Slice)SliceBounds=Current;
            if(Op.Kind==EStudioPipelineOperation::Contour){Contour=&Op;ContourBounds=Current;}
            if(Op.Kind==EStudioPipelineOperation::Probe)Probe=&Op;
        }
        if(Probe)
        {
            Out.Kind=EStudioPipelineOutputKind::ProbeTable;Out.Method=TEXT("Ordered-domain queries on the verified original frame; unsupported, clipped and off-plane positions retain missing values.");
            FStudioProbeRequest R;R.ProjectId=Prepared.ProjectId;R.PresentationId=1;R.Field=Prepared.Field;R.DisplayedScalar=Field().SelectedScalar().Id;
            R.Probe.Id=Probe->Id;R.Probe.Name=Probe->Name;R.Probe.A=Probe->A;R.Probe.B=Probe->B;R.Probe.Samples=Probe->Samples;R.Probe.Kind=Probe->bLine?EStudioProbeKind::Line:EStudioProbeKind::Point;
            const auto& I=Prepared.Recipe.Source.Identity;R.Probe.Source={I.Dataset,I.MetadataSHA256,I.PayloadSHA256};
            Out.Probe=StudioProbeSampling::Evaluate(R,Cancellation);
            if(Out.Probe->Status!=EStudioProbeStatus::Ready){Error=Out.Probe->Message;return false;}
        }
        else if(Contour&&Prepared.Recipe.Source.Identity.SpatialDimensions==3&&!Field().Slice().IsSet())
        {if(!Contour3D(Contour->Value,Current))return false;}
        else if(Field().Slice().IsSet()||Field().MeshTriangleCount()>0)
        {
            if(!Surface(Contour?ContourBounds:Current,SliceBounds))return false;
            if(Contour){auto Mesh=MoveTemp(Out);Out={};if(!Contour2D(Mesh,Contour->Value,Current))return false;}
        }
        else if(!Points())return false;
        if(Cancelled(Cancellation))return false;
        if(Out.AllocatedBytes()>StudioPipelineEvaluation::MaxOutputBytes)return Fail(TEXT("Pipeline output exceeds the 128 MiB geometry budget."));
        for(const auto& V:Out.Vertices)
        {
            if(V.PositionMeters.ContainsNaN()||!FMath::IsFinite(V.Scalar))return Fail(TEXT("Pipeline generated a nonfinite geometry value."));
            if(!Out.Range.IsSet())Out.Range=FVector2D(V.Scalar,V.Scalar);else{Out.Range->X=FMath::Min(Out.Range->X,V.Scalar);Out.Range->Y=FMath::Max(Out.Range->Y,V.Scalar);}
            if(V.OriginalRow==INDEX_NONE)Out.bDerivedGeometry=true;
        }
        if(Out.Probe.IsSet())for(const auto& S:Out.Probe->Samples)if(S.Value.IsSet())
        {if(!Out.Range.IsSet())Out.Range=FVector2D(*S.Value,*S.Value);else{Out.Range->X=FMath::Min(Out.Range->X,*S.Value);Out.Range->Y=FMath::Max(Out.Range->Y,*S.Value);}}
        return true;
    }
};
}

bool FStudioPipelineOutput::IsEmpty() const
{return Kind==EStudioPipelineOutputKind::ProbeTable?(!Probe.IsSet()||!Probe->Samples.ContainsByPredicate([](const auto& S){return S.Value.IsSet();})):Vertices.IsEmpty();}
int64 FStudioPipelineOutput::AllocatedBytes() const
{return Vertices.GetAllocatedSize()+Triangles.GetAllocatedSize()+Lines.GetAllocatedSize()+(Probe.IsSet()?Probe->Samples.GetAllocatedSize():0);}
bool FStudioPipelineEvaluationResult::Matches(const FGuid& Project,uint64 Revision,const FStudioSavedPipeline& Recipe) const
{return !bCancelled&&Error.IsEmpty()&&Output&&Prepared.Matches(Project,Revision,Recipe);}
FStudioPipelineEvaluationResult StudioPipelineEvaluation::Evaluate(const FStudioPipelinePrepareRequest& R,const FStudioLoadCancellation& C)
{
    using namespace StudioPipelineEvaluationPrivate;
    FStudioPipelineEvaluationResult Out;Out.Prepared=StudioPipelineFields::Prepare(R,C);
    auto Fail=[&](const FString& Error){Out.Error=Error;Out.bCancelled=Cancelled(C)||Out.Prepared.bCancelled;Discard(Out);if(Out.bCancelled)Out.Error=TEXT("Pipeline evaluation cancelled. Current result kept.");return MoveTemp(Out);};
    if(!Out.Prepared.Matches(R.ProjectId,R.Revision,R.Recipe))return Fail(Out.Prepared.Error);
    FBuilder Builder{Out.Prepared,C};if(!Builder.Build())return Fail(Builder.Error);
    if(Cancelled(C))return Fail({});Out.Output=MakeShared<const FStudioPipelineOutput,ESPMode::ThreadSafe>(MoveTemp(Builder.Out));return Out;
}
FStudioPipelineEvaluationTask::~FStudioPipelineEvaluationTask(){Shutdown();}
bool FStudioPipelineEvaluationTask::Start(FStudioPipelinePrepareRequest R,FString& Error)
{
    if(bShutdown||Pending.IsValid()){Error=TEXT("Wait for the pipeline evaluation to finish or cancel it before starting another.");return false;}
    if(!R.ProjectId.IsValid()){Error=TEXT("The pipeline needs a project identity.");return false;}
    if(!StudioPipelines::IsValid(R.Recipe,Error))return false;
    Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Pending=Async(EAsyncExecution::ThreadPool,[Request=MoveTemp(R),Cancel=Cancellation]{return StudioPipelineEvaluation::Evaluate(Request,Cancel);});Error.Empty();return true;
}
void FStudioPipelineEvaluationTask::Cancel(){if(Cancellation)Cancellation->store(true,std::memory_order_relaxed);}
void FStudioPipelineEvaluationTask::Shutdown(){bShutdown=true;Cancel();if(Pending.IsValid()){Pending.Wait();Pending=TFuture<FStudioPipelineEvaluationResult>();}Cancellation.Reset();}
TOptional<FStudioPipelineEvaluationResult> FStudioPipelineEvaluationTask::Poll()
{
    if(!Pending.IsValid()||!Pending.IsReady())return {};auto R=Pending.Consume();
    if(StudioPipelineEvaluationPrivate::Cancelled(Cancellation)){StudioPipelineEvaluationPrivate::Discard(R);R.bCancelled=true;R.Error=TEXT("Pipeline evaluation cancelled. Current result kept.");}
    Cancellation.Reset();return R;
}
