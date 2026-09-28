#pragma once
#include "CoreMinimal.h"

/** Geometric coverage, independent of field values. A pair of valid samples is
 * insufficient: the complete segment must belong to the interpolation mesh. */
namespace StudioSegmentCoverage
{
    bool Triangle(const FVector2D& Start,const FVector2D& End,const FVector2D& A,
        const FVector2D& B,const FVector2D& C,FVector2D& Interval);
    bool Complete(TArray<FVector2D>& Intervals);
}
