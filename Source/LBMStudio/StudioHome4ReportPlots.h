#pragma once
#include "StudioHome4Validation.h"

/** Display-only selection. Original arrays are retained by evidence and CSV export. */
struct FStudioHome4ReportPlot
{
    FString Title, XLabel, YLabel;
    TArray<double> X;
    TArray<TArray<double>> Channels;
    TArray<FString> Legends;
    TArray<int32> PreviewIndices;
    double XMin = 0, XMax = 0, YMin = 0, YMax = 0;
    FVector2D Normalized(int32 Channel, int32 Index) const;
};
namespace StudioHome4ReportPlots
{
    constexpr int32 PreviewLimit = 2000;
    bool Reference(const FStudioHome4ReferenceSeries& Series, FStudioHome4ReportPlot& Out, FString& Error);
    bool Convergence(const FStudioHome4ReferenceEvidence& Evidence, FStudioHome4ReportPlot& Out, FString& Error);
    FString SelectionDescription();
    FString SVG(const FStudioHome4ReportPlot& Plot, const FString& Identity);
    /** TikZ fragment for report input. Wrap with TeXPreamble/TeXEnd for standalone use. */
    FString TikZ(const FStudioHome4ReportPlot& Plot);
    FString TeXPreamble();
    FString TeXEnd();
    FString CSV(const FStudioHome4ReportPlot& Plot, const TArray<FGuid>& RunIds = {});
}
