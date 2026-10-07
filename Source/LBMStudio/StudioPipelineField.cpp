#include "StudioPipelineField.h"
#include "StudioSavedFieldView.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "Async/Async.h"
#include <cmath>
#include <limits>

namespace StudioPipelineFieldPrivate
{
bool Cancelled(const FStudioLoadCancellation& C){return C&&C->load(std::memory_order_relaxed);}
void Discard(FStudioPipelinePrepared& R){R.Field.Reset();R.Source.Reset();R.Stages.Reset();}
bool SameScalar(const FStudioScalarDescriptor& A,const FStudioScalarDescriptor& B)
{return A.Id==B.Id&&A.Label==B.Label&&A.Unit==B.Unit&&A.Origin==B.Origin&&A.Minimum==B.Minimum&&A.Maximum==B.Maximum;}
FBox Intersection(const FBox& A,const FBox& B)
{
    if(!A.IsValid||!B.IsValid)return FBox(ForceInit);
    const FVector Low=A.Min.ComponentMax(B.Min),High=A.Max.ComponentMin(B.Max);
    return Low.X<=High.X&&Low.Y<=High.Y&&Low.Z<=High.Z?FBox(Low,High):FBox(ForceInit);
}
}

int32 FStudioPipelineField::NodeIndex(const FString& Id) const
{return Nodes.IndexOfByPredicate([&](const auto& N){return N.Scalar.Id==Id;});}
const IStudioField& FStudioPipelineField::Base() const
{check(!Nodes.IsEmpty()&&Nodes[0].Original);return *Nodes[0].Original;}
TOptional<FStudioScalarDescriptor> FStudioPipelineField::Scalar(const FString& Id) const
{const int32 N=NodeIndex(Id);return N==INDEX_NONE?TOptional<FStudioScalarDescriptor>():Nodes[N].Scalar;}
FString FStudioPipelineField::ScalarExpression(const FString& Id) const
{const int32 N=NodeIndex(Id);return N==INDEX_NONE?FString():Nodes[N].Expression;}
bool FStudioPipelineField::OriginalPoint(int32 Row,int64& Id,FVector& Position) const
{return IsValid()&&Base().OriginalPoint(Row,Id,Position);}
int32 FStudioPipelineField::OriginalPointCount() const {return IsValid()?Base().OriginalPointCount():0;}
int32 FStudioPipelineField::OriginalTriangleCount() const {return IsValid()?Base().OriginalTriangleCount():0;}
bool FStudioPipelineField::OriginalTriangle(int32 Index,FIntVector& Triangle) const {return IsValid()&&Base().OriginalTriangle(Index,Triangle);}
bool FStudioPipelineField::Values(int32 Last,int32 Row,const FVector* Position,double* Out,const FStudioVolumeStencil* Stencil) const
{
    if(!Nodes.IsValidIndex(Last))return false;
    // Evaluate only the requested dependency closure, once per node. A preceding
    // unrelated scalar must not make an otherwise valid query unavailable.
    bool Needed[StudioPipelineFields::MaxScalarNodes]={};Needed[Last]=true;
    for(int32 I=Last;I>=0;--I)if(Needed[I])for(int32 C:Nodes[I].Components)Needed[C]=true;
    for(int32 I=0;I<=Last;++I)
    {
        if(!Needed[I])continue;const auto& N=Nodes[I];double V=0;
        if(N.Original)
        {
            if(Stencil)
            {
                const auto SourceVolume=N.Original->VolumeReconstruction();const auto Frame=N.Original->OriginalPoints();
                for(int32 K=0;K<4;++K)
                {
                    if(Stencil->Weights[K]==0)continue;
                    if(SourceVolume&&SourceVolume->OriginalGrid&&(!Frame||!StudioVolumes::SourceNodeSupported(*Frame,*SourceVolume,N.Scalar.Id,Stencil->Rows[K])))return false;
                    double S;if(!N.Original->OriginalScalar(Stencil->Rows[K],N.Scalar.Id,S))return false;V+=Stencil->Weights[K]*S;
                }
            }
            else if(Position?!N.Original->SampleScalar(*Position,N.Scalar.Id,V):!N.Original->OriginalScalar(Row,N.Scalar.Id,V))return false;
        }
        else for(int32 C:N.Components)V=std::hypot(V,Out[C]);
        if(!FMath::IsFinite(V))return false;Out[I]=V;
    }
    return true;
}
bool FStudioPipelineField::OriginalScalar(int32 Row,const FString& Id,double& Out) const
{
    const int32 N=NodeIndex(Id);double Computed[StudioPipelineFields::MaxScalarNodes];
    if(!IsValid()||Row<0||Row>=OriginalPointCount()||!Values(N,Row,nullptr,Computed))return false;
    Out=Computed[N];return true;
}
bool FStudioPipelineField::Includes(const FVector& P) const
{
    if(P.ContainsNaN()||!Bounds.IsValid||!Bounds.IsInsideOrOn(P))return false;
    if(PinnedIdentity.SpatialDimensions==2&&P.Y!=PinnedIdentity.SourceOffset.Y)return false;
    if(Plane.IsSet())
    {
        const double Tolerance=1.e-9+32*std::numeric_limits<double>::epsilon()*FMath::Max(1.,P.GetAbsMax()+Plane->A.GetAbsMax());
        if(FMath::Abs(FVector::DotProduct(P-Plane->A,Plane->B))>Tolerance)return false;
    }
    return true;
}
TSharedRef<const FStudioPipelineField,ESPMode::ThreadSafe> FStudioPipelineField::WithDomain(const FBox& Domain) const
{auto Copy=MakeShared<FStudioPipelineField,ESPMode::ThreadSafe>(*this);Copy->Bounds=Domain;return Copy;}
bool FStudioPipelineField::SampleScalar(const FVector& P,const FString& Id,double& Out) const
{return IsValid()&&Includes(P)&&SampleUnderlyingScalar(P,Id,Out);}
bool FStudioPipelineField::SampleUnderlyingScalar(const FVector& P,const FString& Id,double& Out) const
{
    const int32 N=NodeIndex(Id);double Computed[StudioPipelineFields::MaxScalarNodes];
    if(!IsValid()||P.ContainsNaN()||!Values(N,0,&P,Computed))return false;
    Out=Computed[N];return true;
}
bool FStudioPipelineField::SampleVolumeNode(int32 Index,const FString& Id,double& Out) const
{
    if(!IsValid())return false;const auto V=VolumeReconstruction();
    if(!V||!V->Stencils.IsValidIndex(Index)||!V->Classification.IsValidIndex(Index)||V->Classification[Index]!=1)return false;
    const int32 N=NodeIndex(Id);double Computed[StudioPipelineFields::MaxScalarNodes];
    if(!Values(N,0,nullptr,Computed,&V->Stencils[Index]))return false;Out=Computed[N];return true;
}
bool FStudioPipelineField::IsSolid(const FVector& P) const {return IsValid()&&Base().IsSolid(P);}
bool FStudioPipelineField::SupportsSegment(const FVector& A,const FVector& B,const FStudioLoadCancellation& C) const
{return IsValid()&&!StudioPipelineFieldPrivate::Cancelled(C)&&Includes(A)&&Includes(B)&&Base().SupportsSegment(A,B,C);}
const TArray<FVector2D>& FStudioPipelineField::Boundary() const {return Base().Boundary();}
const TArray<FIntVector>& FStudioPipelineField::BoundaryTriangles() const {return Base().BoundaryTriangles();}
TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> FStudioPipelineField::Reconstruction() const {return Base().Reconstruction();}
TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> FStudioPipelineField::VolumeReconstruction() const {return Base().VolumeReconstruction();}
int32 FStudioPipelineField::MeshTriangleCount() const {return IsValid()?Base().MeshTriangleCount():0;}
bool FStudioPipelineField::MeshTriangle(int32 Index,FVector (&P)[3]) const {return IsValid()&&Base().MeshTriangle(Index,P);}

struct FStudioPipelineFieldBuilder
{
    FStudioPipelinePrepared& Result;
    const FStudioLoadCancellation& Cancellation;
    TSharedRef<FStudioPipelineField,ESPMode::ThreadSafe> Field=MakeShared<FStudioPipelineField,ESPMode::ThreadSafe>();
    bool Fail(const FString& Why){Result.Error=Why;return false;}
    bool AddOriginal(const FString& Id)
    {
        if(Field->NodeIndex(Id)!=INDEX_NONE)return true;
        if(StudioPipelineFieldPrivate::Cancelled(Cancellation))return false;
        const auto& D=Result.Source->Descriptor();const auto* S=D.Scalars.FindByPredicate([&](const auto& V){return V.Id==Id;});
        if(!S||!FMath::IsFinite(S->Minimum)||!FMath::IsFinite(S->Maximum)||S->Minimum>S->Maximum)return Fail(TEXT("Invalid source scalar metadata."));
        const int64 Bytes=int64(D.NodeCount)*sizeof(double);
        if(D.NodeCount<=0||D.NodeCount>StudioPipelineFields::MaxSourcePoints||Field->Nodes.Num()>=StudioPipelineFields::MaxScalarNodes||
            Bytes>StudioPipelineFields::MaxInputValueBytes-Field->ValueBytes)
            return Fail(TEXT("Pipeline input exceeds the 2,097,152-point or 64 MiB scalar-value budget. Reduce the enabled fields or input size."));
        auto Read=Result.Source->ReadScalarFrame(Result.Recipe.Source.Identity.Ordinal,Id,Cancellation);
        if(StudioPipelineFieldPrivate::Cancelled(Cancellation))return false;
        if(!Read.Field||!Read.Field->IsValid())return Fail(TEXT("Could not read the exact original pipeline field. ")+Read.Error);
        const auto I=Read.Field->Identity();const auto Actual=Read.Field->Scalar(Id);
        if(!I.IsSet()||!StudioSavedFieldViews::SameIdentity(*I,Result.Recipe.Source.Identity)||!Actual.IsSet()||
            !StudioPipelineFieldPrivate::SameScalar(*Actual,*S)||Read.Field->OriginalPointCount()!=D.NodeCount||
            Read.Field->Reconstruction()!=Result.Source->Reconstruction()||Read.Field->VolumeReconstruction()!=Result.Source->VolumeReconstruction())
            return Fail(TEXT("Pipeline array, geometry or frame differs from its pinned source. No field published."));
        for(int32 Row=0;Row<D.NodeCount;++Row)
        {
            if((Row&255)==0&&StudioPipelineFieldPrivate::Cancelled(Cancellation))return false;
            int64 PointId,ExpectedId;FVector P,Expected;double V;
            if(!Read.Field->OriginalPoint(Row,PointId,P)||P.ContainsNaN()||!Read.Field->OriginalScalar(Row,Id,V)||!FMath::IsFinite(V))
                return Fail(TEXT("Original pipeline rows or scalar values are unavailable or nonfinite."));
            if(!Field->Nodes.IsEmpty()&&(!Field->Base().OriginalPoint(Row,ExpectedId,Expected)||PointId!=ExpectedId||P!=Expected))
                return Fail(TEXT("Pipeline component arrays have different point IDs or coordinates."));
        }
        FStudioPipelineScalarNode N;N.Scalar=*S;N.Expression=Read.Field->ScalarExpression(Id);N.Original=MoveTemp(Read.Field);
        Field->Nodes.Add(MoveTemp(N));Field->ValueBytes+=Bytes;return true;
    }
    bool Build()
    {
        const auto& D=Result.Source->Descriptor();Field->PinnedIdentity=Result.Recipe.Source.Identity;Field->PreparedRecipe=Result.Recipe;Field->Bounds=D.DisplayBounds;
        if(Result.Source->FrameCount()!=D.Frames.Num()||!D.DisplayBounds.IsValid||D.DisplayBounds.Min.ContainsNaN()||D.DisplayBounds.Max.ContainsNaN())
            return Fail(TEXT("Pipeline source timeline or display bounds are invalid."));
        FString Reconstruction;auto Method=D.bSourcePoints?EStudioFieldInterpolation::None:EStudioFieldInterpolation::SourceTriangles;
        if(const auto S=Result.Source->Reconstruction()){Reconstruction=S->MetadataSHA256;Method=EStudioFieldInterpolation::ReconstructedTriangles;}
        if(const auto V=Result.Source->VolumeReconstruction()){Reconstruction=V->ReconstructionIdentity();Method=V->Interpolation();}
        if(Reconstruction!=Field->PinnedIdentity.ReconstructionSHA256||Method!=Field->PinnedIdentity.Interpolation)
            return Fail(TEXT("The pinned pipeline reconstruction or interpolation method is unavailable."));
        for(const auto& Op:Result.Recipe.Operations)
        {
            if(StudioPipelineFieldPrivate::Cancelled(Cancellation))return false;
            if(!Op.bEnabled)continue;
            if(Op.Kind==EStudioPipelineOperation::Field)
            {if(!AddOriginal(Op.Field))return false;Field->Selected=Field->NodeIndex(Op.Field);}
            else if(Op.Kind==EStudioPipelineOperation::Magnitude)
            {
                FStudioPipelineScalarNode N;N.Scalar.Id=Op.Field;N.Scalar.Label=Op.Name;N.Scalar.Unit=Op.Unit;
                N.Scalar.Origin=TEXT("pipeline-derived");N.Scalar.Minimum=0;N.Scalar.Maximum=0;N.Expression=TEXT("hypot(");
                for(const auto& Id:Op.Components)
                {
                    if(!AddOriginal(Id))return false;
                    const int32 Index=Field->NodeIndex(Id);N.Components.Add(Index);
                    const auto& Scalar=Field->Nodes[Index].Scalar;
                    N.Scalar.Maximum=std::hypot(N.Scalar.Maximum,FMath::Max(FMath::Abs(Scalar.Minimum),FMath::Abs(Scalar.Maximum)));
                    if(N.Components.Num()>1)N.Expression+=TEXT(", ");N.Expression+=Id;
                }
                N.Expression+=TEXT("); source components interpolated before magnitude");
                if(!FMath::IsFinite(N.Scalar.Maximum)||Field->Nodes.Num()>=StudioPipelineFields::MaxScalarNodes)
                    return Fail(TEXT("Derived magnitude range or scalar graph exceeds the supported numerical budget."));
                Field->Selected=Field->Nodes.Add(MoveTemp(N));
            }
            else if(Op.Kind==EStudioPipelineOperation::ClipBox)
                Field->Bounds=StudioPipelineFieldPrivate::Intersection(Field->Bounds,FBox(Op.A,Op.B));
            else if(Op.Kind==EStudioPipelineOperation::Slice)Field->Plane=Op;
        }
        if(!Field->IsValid())return Fail(TEXT("No scalar field was prepared."));
        Result.Field=Field;return true;
    }
};

FStudioPipelinePrepared StudioPipelineFields::Prepare(const FStudioPipelinePrepareRequest& R,const FStudioLoadCancellation& C)
{
    using namespace StudioPipelineFieldPrivate;
    FStudioPipelinePrepared Out;Out.ProjectId=R.ProjectId;Out.Revision=R.Revision;Out.Recipe=R.Recipe;
    auto Fail=[&](const FString& Why)
    {Discard(Out);Out.bCancelled=Cancelled(C);Out.Error=Out.bCancelled?TEXT("Pipeline read cancelled. Current result kept."):Why;return MoveTemp(Out);};
    FString Error;
    if(Cancelled(C))return Fail({});
    if(!R.ProjectId.IsValid()||!StudioPipelines::IsValid(R.Recipe,Error))return Fail(Error.IsEmpty()?TEXT("The pipeline needs a project identity."):Error);
    if(R.Source)Out.Source=R.Source;
    else
    {
        auto Open=StudioRecordings::Open(R.Recipe.Source.Identity.Dataset,StudioSavedFieldViews::ResolvedReference(R.Recipe.Source,R.References),R.Recipe.Source.Identity.Ordinal,C);
        if(!Open.Source)return Fail(Open.Error);Out.Source=MoveTemp(Open.Source);
    }
    if(Cancelled(C))return Fail({});
    if(!StudioPipelines::Compile(R.Recipe,Out.Source->Descriptor(),Out.Stages,Error))return Fail(Error);
    FStudioPipelineFieldBuilder Builder{Out,C};if(!Builder.Build())return Fail(Out.Error);
    if(Cancelled(C))return Fail({});Out.Error.Empty();return Out;
}
bool FStudioPipelinePrepared::Matches(const FGuid& Project,uint64 CurrentRevision,const FStudioSavedPipeline& Current) const
{
    return ProjectId==Project&&Revision==CurrentRevision&&!bCancelled&&Error.IsEmpty()&&Source&&Field&&Field->IsValid()&&
        StudioPipelines::Equals(Recipe,Current)&&Field->MatchesRecipe(Current)&&Field->Identity().IsSet()&&StudioSavedFieldViews::SameIdentity(*Field->Identity(),Current.Source.Identity);
}
bool FStudioPipelineField::MatchesRecipe(const FStudioSavedPipeline& Recipe) const
{return StudioPipelines::Equals(PreparedRecipe,Recipe);}
TSharedPtr<const FStudioPipelineField,ESPMode::ThreadSafe> FStudioPipelineField::WithPresentation(const FStudioSavedPipeline& Recipe) const
{
    auto Expected=PreparedRecipe;Expected.Name=Recipe.Name;Expected.Source.Camera=Recipe.Source.Camera;FString Error;
    if(!StudioPipelines::Equals(Expected,Recipe)||!StudioPipelines::IsValid(Recipe,Error))return {};
    auto Copy=MakeShared<FStudioPipelineField,ESPMode::ThreadSafe>(*this);Copy->PreparedRecipe=Recipe;return Copy;
}
FStudioPipelinePrepareTask::~FStudioPipelinePrepareTask(){Shutdown();}
bool FStudioPipelinePrepareTask::Start(FStudioPipelinePrepareRequest R,FString& Error)
{
    if(bShutdown||Pending.IsValid()){Error=TEXT("Wait for the pipeline read to finish or cancel it before starting another.");return false;}
    if(!R.ProjectId.IsValid()||!StudioPipelines::IsValid(R.Recipe,Error))return false;
    Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Pending=Async(EAsyncExecution::ThreadPool,[Request=MoveTemp(R),Cancel=Cancellation]{return StudioPipelineFields::Prepare(Request,Cancel);});Error.Empty();return true;
}
void FStudioPipelinePrepareTask::Cancel(){if(Cancellation)Cancellation->store(true,std::memory_order_relaxed);}
void FStudioPipelinePrepareTask::Shutdown(){bShutdown=true;Cancel();if(Pending.IsValid()){Pending.Wait();Pending=TFuture<FStudioPipelinePrepared>();}Cancellation.Reset();}
TOptional<FStudioPipelinePrepared> FStudioPipelinePrepareTask::Poll()
{
    if(!Pending.IsValid()||!Pending.IsReady())return {};auto R=Pending.Consume();
    if(StudioPipelineFieldPrivate::Cancelled(Cancellation)){StudioPipelineFieldPrivate::Discard(R);R.bCancelled=true;R.Error=TEXT("Pipeline read cancelled. Current result kept.");}
    Cancellation.Reset();return R;
}
