#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"
#include "StudioHome4Session.h"
#include "StudioMeshImport.h"

struct FStudioHome4PreviewCell {FIntVector Index=FIntVector::ZeroValue;double SignedDistance=0;bool bInside=false;};
struct FStudioHome4PreviewLink {FIntVector Index=FIntVector::ZeroValue,Direction=FIntVector::ZeroValue;FVector Position=FVector::ZeroVector;double Fraction=0;};
struct FStudioHome4PreviewRegion {FString Id,Kind,Profile,Axis;FBox Bounds=FBox(ForceInit);int32 Level=0;double Strength=0,LevelExponent=1;bool bFollowBody=false;};
struct FStudioHome4Hydrostatics
{
    double TotalVolume=0,SubmergedVolume=0,DisplacedMass=0,Heave=0,TrimDegrees=0;
    FVector SubmergedCentroid=FVector::ZeroVector,CenterOfGravity=FVector::ZeroVector;
    double VerticalResidual=0,PitchMomentResidual=0,K33=0,K35=0,K53=0,K55=0;
    int32 Iterations=0;bool bConverged=false,bEquilibrated=false;
};
/** Geometric next-run preparation, never a recording or a numerical CFD result. */
struct FStudioHome4AuthoringPreview
{
    FGuid ProjectId,CaseId;
    FString RequestSHA256,SourceSHA256,SourcePath,Method,Error,HydrostaticError;
    FStudioHome4Spec Spec;
    TSharedPtr<const FStudioImportedMesh,ESPMode::ThreadSafe> Mesh;
    FBox Tank=FBox(ForceInit),Body=FBox(ForceInit);
    TArray<FStudioHome4PreviewCell> Cells;
    TArray<FStudioHome4PreviewLink> Links;
    TArray<FVector> SdfSurfacePositions;TArray<int32> SdfSurfaceIndices; // Interpolated zero surface of actual sampled SDF.
    TArray<FStudioHome4PreviewRegion> Regions;
    TOptional<FStudioHome4Hydrostatics> Hydrostatics;
    int64 RequestedCells=0;int32 Stride=1;
    bool bClosed=false,bCancelled=false,bCacheHit=false;
    bool IsValid()const{return Mesh.IsValid()&&Error.IsEmpty()&&!bCancelled;}
};
struct FStudioHome4AuthoringRequest
{
    FGuid ProjectId,CaseId;
    FStudioHome4Spec Spec;
    int32 SampleBudget=32768;
};
namespace StudioHome4Authoring
{
    FString Fingerprint(const FStudioHome4Spec& Spec);
    /** Worker-only. Source SHA, units and axes are explicit. Cancellation bounds all loops. */
    FStudioHome4AuthoringPreview Build(const FStudioHome4AuthoringRequest&,const FStudioAssetCancellation&);
    bool Hydrostatics(const FStudioImportedMesh&,double Waterline,double RhoHeavy,double RhoLight,double Mass,
        const FVector& CoG,double Gravity,bool Equilibrate,FStudioHome4Hydrostatics& Out,FString& Error,
        const FStudioAssetCancellation& Cancel);
    /** Intrinsic source XYZ kinematics from declared sinusoidal/constant inputs. */
    FTransform Motion(const FStudioHome4Spec&,double Step);
    /** Compose source-XYZ pose with a verified geometric equilibrium; Out changes only on success. */
    bool AcceptEquilibrium(const FStudioHome4AuthoringPreview&,FStudioHome4Spec& Out,FString& Error);
    double ZoneWeight(const FStudioHome4PreviewRegion&,const FVector& Position,int32 Level=0);
}
/** One retained geometric camera for all authoring pages, independent of recorded fields. */
struct FStudioHome4AuthoringCamera
{
    double Yaw=.65,Pitch=.4,Zoom=1;
    FVector2D Pan=FVector2D::ZeroVector;
};
/** Retains current immutable response, rejects stale project/case/draft responses, and caches one exact request. */
class FStudioHome4AuthoringSession
{
public:
    explicit FStudioHome4AuthoringSession(TSharedPtr<FStudioHome4Session> InSession):Session(InSession){}
    ~FStudioHome4AuthoringSession();
    bool Request();
    void Poll();
    void Cancel();
    bool IsPreparing()const{return Pending.IsValid();}
    TSharedPtr<const FStudioHome4AuthoringPreview,ESPMode::ThreadSafe> Preview()const{return Response;}
    const TSharedPtr<FStudioHome4Session>& Draft()const{return Session;}
    FString Status;
    FStudioHome4AuthoringCamera& Camera(){return View;}
    const FStudioHome4AuthoringCamera& Camera()const{return View;}
private:
    FStudioHome4AuthoringCamera View;
    TSharedPtr<FStudioHome4Session> Session;
    FStudioAssetCancellation Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    TFuture<FStudioHome4AuthoringPreview> Pending;
    TSharedPtr<const FStudioHome4AuthoringPreview,ESPMode::ThreadSafe> Response;
    FGuid ProjectId,CaseId;FString RequestedSHA;
};
