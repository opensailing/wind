#pragma once

#include "CoreMinimal.h"
#include "StudioColor.h"

class IStudioField;

struct FStudioVectorGlyph
{
    FVector PositionMeters = FVector::ZeroVector;
    FVector TipMeters = FVector::ZeroVector;
    FVector Direction = FVector::ZeroVector;
    FLinearColor Color = FLinearColor::Transparent;
};

/** The length key belongs to the immutable geometry, never the requested frame. */
struct FStudioVectorSummary
{
    int32 SampleCount = 0;
    int32 GlyphCount = 0;
    double MaximumSpeed = 0;
    double ReferenceLengthMeters = 0;
    bool bUniformLength = false;
    FString Field = TEXT("velocity"),Unit = TEXT("m/s");
    FString UnavailableReason;
};

struct FStudioVectorSample
{
    FVector PositionMeters = FVector::ZeroVector;
    FVector Velocity = FVector::ZeroVector;
    FLinearColor Color = FLinearColor::Transparent;
    bool bHasColor = false;
};

namespace StudioFieldDisplay
{
    constexpr int32 MaximumVectorCount = 4096;
    /** Deterministic source rows; never resample an original-point recording. */
    TArray<int32> VectorRows(int32 Total, int32 Count);
    /** A bounded, evenly spaced grid on the legacy 2D display plane. */
    TArray<FVector> VectorGrid(const FBox& Bounds, int32 Count);
    /** Relative mode is exactly proportional to speed. Uniform mode encodes
     * direction only. Missing scalar samples do not change the speed reference. */
    bool VectorGlyphs(const TArray<FStudioVectorSample>& Samples, double ReferenceLengthMeters,
        bool bUniformLength, TArray<FStudioVectorGlyph>& Out, FStudioVectorSummary& Summary,
        const FStudioLoadCancellation& Cancellation = {});
    /** Colors always sample the selected scalar, independently of vector speed. */
    bool SampleColor(const IStudioField& Field,const FVector& PositionMeters,const FString& ScalarId,
        const FStudioColorMapping& Mapping,FLinearColor& Out);
    /** Directions and relative lengths use velocity; colors use ScalarId.
     * Missing samples produce no glyph. Cancellation/invalid options preserve Out.
     */
    bool VectorGlyphs(const IStudioField& Field,const TArray<FVector>& PositionsMeters,const FString& ScalarId,
        const FStudioColorMapping& Mapping,double ReferenceSizeMeters,double RelativeScale,
        TArray<FStudioVectorGlyph>& Out,const FStudioLoadCancellation& Cancellation = {},
        bool bUniformLength = false,FStudioVectorSummary* Summary = nullptr);
}
