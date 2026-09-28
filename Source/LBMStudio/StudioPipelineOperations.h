#pragma once
#include "CoreMinimal.h"

enum class EStudioPipelineOperation : uint8 { Field, Magnitude, ClipBox, Slice, Contour, Probe };

/** Ordered immutable recipe, not numerical output. Geometry uses scene meters.
 * Field: Field/Unit identify the selected array. Magnitude: Components contain
 * 2 or 3 same-unit input arrays; Field names a new array, Unit pins its meaning.
 * ClipBox: A/B are inclusive minimum/maximum. Slice: A is origin, B unit normal.
 * Contour: Value is an isovalue in the current scalar's unit. Probe: A/B are
 * point/line endpoints; bLine and Samples choose the sampling layout.
 * Only parameters for Kind are serialized; disabled entries retain parameters. */
struct FStudioPipelineOperation
{
    FGuid Id=FGuid::NewGuid();
    FString Name;
    EStudioPipelineOperation Kind=EStudioPipelineOperation::Field;
    bool bEnabled=true;
    FString Field,Unit;
    TArray<FString> Components;
    FVector A=FVector::ZeroVector,B=FVector::RightVector;
    double Value=0;
    bool bLine=false;
    int32 Samples=64;
};
