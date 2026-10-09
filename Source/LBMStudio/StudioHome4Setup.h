#pragma once
#include "StudioHome4Authoring.h"

struct FStudioHome4RampPoint
{
    double Step=0,Duration=0,Speed=0,Acceleration=0;
    // An absent no-frame-acceleration choice leaves the requested frame force unknown.
    TOptional<double> HeavyFrameForceDensity,LightFrameForceDensity;
};
struct FStudioHome4WaveProperties
{
    double WaveNumber=0,AngularFrequency=0,Period=0,PhaseSpeed=0;
};
struct FStudioHome4BoundaryFace
{
    FString Id,Constraint,PhaseConstraint;
    FBox Bounds=FBox(ForceInit);
    TOptional<double> Phase;
};
namespace StudioHome4Setup
{
    /** Explicit frontend layout: pads in body lengths, depth/air measured from waterline.
     * Integer extents round up; body, CoG, patches and authored zones translate together. */
    bool Tank(const FStudioHome4AuthoringPreview&,FStudioHome4Spec& Out,FString& Error);
    /** Reviewed frontend convention only: sponge/side widths and x-beach start in ZoneUnits.
     * Reserved frontend-width-* IDs are reconciled; hand-authored regions are preserved. */
    bool Zones(const FStudioHome4Spec&,FStudioHome4Spec& Out,FString& Error);
    /** Explicit contiguous patch levels, root-inclusive counts; no topology is inferred. */
    bool PatchCounts(const FStudioHome4Spec&,FStudioHome4Spec& Out,FString& Error);
    /** Clear stale authored count/allocation values when patch topology changes. */
    void InvalidatePatchCounts(FStudioHome4Spec&);
    bool ZoneScale(const FStudioHome4Spec&,double& RootCellsPerZoneUnit,FString& Error);
    TOptional<FStudioHome4RampPoint> Ramp(const FStudioHome4Spec&,double Step);
    TOptional<FStudioHome4WaveProperties> WaveProperties(const FStudioHome4Spec&,FString& Error);
    /** Linear finite-depth geometric elevation. No wave transport/CFD is simulated. */
    TOptional<double> Wave(const FStudioHome4Spec&,const FVector& Position,double Step,FString& Error);
    bool WaveCurve(const FStudioHome4Spec&,double Step,int32 Segments,TArray<FVector>& Out,FString& Error);
    /** Six named Cartesian face requests. Unspecified phase values remain absent. */
    bool BoundaryFaces(const FStudioHome4Spec&,TArray<FStudioHome4BoundaryFace>& Out,FString& Error);
}
