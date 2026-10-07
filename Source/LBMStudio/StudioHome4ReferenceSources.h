#pragma once
#include "StudioHome4Validation.h"
struct FStudioHome4SeriesSource
{
    bool bReference=false;
    FStudioHome4ReferenceEvidence Data;
    FString Epoch;
};
namespace StudioHome4ReferenceSources
{
    /** Original independent source contract LBMStudio.Home4Series v1:
     * recipe_id/run_id/source_id/kind(actual|reference)/epoch;
     * series[] id/name/x_name/x_unit/unit/x/values; actual original_run_spec
     * and reference reference_verification are optional, unknown if absent. */
    bool Parse(const FString& JSON,const FString& Recipe,FStudioHome4SeriesSource& Out,FString& Error);
    bool Load(const FString& Path,const FString& Recipe,FStudioHome4SeriesSource& Out,FString& Error);
    bool AlignExact(const FStudioHome4SeriesSource& Actual,const FStudioHome4SeriesSource& Reference,
        double AbsoluteTolerance,double RelativeTolerance,TOptional<double> WindowStart,TOptional<double> WindowEnd,
        FStudioHome4ReferenceEvidence& Out,FString& Error);
    bool VerifyComposed(const FStudioHome4ReferenceEvidence& Evidence,FString& Error);
}
