#pragma once
#include "StudioHome4Recipes.h"

class FJsonObject;
struct FStudioHome4ReferenceExpectation
{
    FString RecipeId;
    TOptional<FGuid> RunId;
    FString ActualSource, ReferenceSource; // Optional exact identity constraints.
};
struct FStudioHome4ReferenceSeries
{
    FString Id, Name, AbscissaName, AbscissaUnit, Unit;
    TArray<double> Abscissae, Actual, Reference;
    double AbsoluteTolerance = 0, RelativeTolerance = 0;
    FStudioHome4GateResult Gate;
};
struct FStudioHome4ScalarRun
{
    FGuid RunId;
    double Refinement = 1, Value = 0;
};
struct FStudioHome4ReferenceEvidence
{
    FString RecipeId, ActualSource, ReferenceSource, SourcePath, SourceSHA256;
    FGuid RunId;
    TArray<FStudioHome4ReferenceSeries> Series;
    FString OrderMetric, OrderUnit;
    TArray<FStudioHome4ScalarRun> OrderRuns;
    TOptional<double> ObservedOrder;
    /** Applies only to supplied aligned series; it never establishes recipe coverage. */
    FString ComparisonStatus() const;
    FString RecipeCoverage() const { return TEXT("unknown"); }
    /** Display wording includes both facts, preserving useful comparison pass/fail. */
    FString GateStatus() const { return TEXT("Supplied-series comparisons: ") + ComparisonStatus() + TEXT(" · recipe gate not_evaluated (coverage unknown)"); }
};
/** Shared session evidence for Validation and Reports. Imports replace evidence
 * only after complete validation. It never modifies the case, camera or playback. */
struct FStudioHome4ValidationState
{
    FGuid ProjectId, CaseId;
    FString RecipeId;
    TSharedPtr<const FStudioHome4ReferenceEvidence> Evidence;
    int32 SelectedSeries = 0;
    TArray<FStudioHome4LadderRung> Ladder;
    FString Status = TEXT("not_evaluated · import identified, aligned reference evidence with explicit tolerances.");
};
namespace StudioHome4Validation
{
    /** Strict bounded JSON contract. Shared x[] explicitly aligns actual/reference;
     * names and units are retained exactly, with no interpolation or unit inference.
     * schema='LBMStudio.Home4Reference', version=1, recipe_id, run_id GUID,
     * actual_source, reference_source, series[] {id,name,x_name,x_unit,unit,x,
     * actual,reference,absolute_tolerance,relative_tolerance}.
     * Optional order {metric,unit,runs:[{run_id,refinement,value} x3]} needs a
     * constant increasing refinement ratio and monotone scalar convergence. */
    bool Parse(const FString& JSON, const FStudioHome4ReferenceExpectation& Expected,
        FStudioHome4ReferenceEvidence& Out, FString& Error);
    bool Load(const FString& Path, const FStudioHome4ReferenceExpectation& Expected,
        FStudioHome4ReferenceEvidence& Out, FString& Error);
    TSharedRef<FJsonObject> EvidenceMetadata(const FStudioHome4ReferenceEvidence& Evidence);
    FString SerializeEvidence(const FStudioHome4ReferenceEvidence& Evidence);
    /** Export original aligned measurements and provenance atomically without overwrite. */
    bool ExportEvidence(const FString& Parent, const FString& Folder, const FStudioHome4ReferenceEvidence& Evidence,
        FString& OutPath, FString& Error);
    bool ExportLadder(const FString& Parent, const FString& Folder, const TArray<FStudioHome4LadderRung>& Ladder,
        FString& OutPath, FString& Error);
}
