#pragma once
#include "CoreMinimal.h"
#include "StudioRecording.h"

/** Saved display choices for one source/field; never edits scientific metadata. */
struct FStudioScalarStyle
{
    FString Dataset, Field;
    int32 Palette = 0; // Spectrum, blue-white-red, grayscale, custom.
    bool bManualRange = false;
    double Minimum = 0, Maximum = 1;
    FLinearColor LowColor=FLinearColor(.03,.15,.6), MiddleColor=FLinearColor(.94,.94,.94), HighColor=FLinearColor(.7,.035,.025);
    bool operator==(const FStudioScalarStyle& Other) const;
};

/** Immutable mapping accompanies the rendered field and its legend. */
struct FStudioColorMapping
{
    int32 Palette = 0;
    bool bManualRange = false;
    double Minimum = 0, Maximum = 1;
    FLinearColor LowColor=FLinearColor(.03,.15,.6), MiddleColor=FLinearColor(.94,.94,.94), HighColor=FLinearColor(.7,.035,.025);
};

namespace StudioColor
{
    bool IsValid(const FStudioScalarStyle& Style);
    bool IsValid(const TArray<FStudioScalarStyle>& Styles);
    FStudioColorMapping Resolve(const FString& Dataset,const FStudioScalarDescriptor& Field,
        const TArray<FStudioScalarStyle>& Styles);
    FString PaletteName(int32 Palette);
    /** Locale-independent decimal/scientific input; rejects partial values and nonfinite numbers. */
    bool ParseNumber(const FString& Text,double& Out);
    /** Six sRGB hex digits, with an optional leading #; stored as linear RGB. */
    bool ParseHexColor(const FString& Text,FLinearColor& Out);
    /** Value-space clamping; missing/nonfinite values must be masked by caller. */
    FLinearColor Map(double Value,const FStudioColorMapping& Mapping);
}
