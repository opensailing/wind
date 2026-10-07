#pragma once
#include "StudioHome4Config.h"
#include "StudioHome4Telemetry.h"
struct FStudioProject;
struct FStudioHome4ReferenceEvidence;
struct FStudioHome4SpatialEvidence;
struct FStudioHome4ValidationState;
struct FStudioHome4ReportInputs
{
    const TArray<uint8>* OriginalTelemetryBytes=nullptr;
    const FStudioHome4ValidationState* Validation=nullptr;
    EStudioHome4UnitDisplay Display=EStudioHome4UnitDisplay::Lattice;
    FString BodyId,Phase,WindowAxis;
    int32 Level=0;
    TOptional<double> WindowStart,WindowEnd;
    bool bFigurePublication=false;
    const FStudioHome4ReferenceEvidence* PublishedEvidence=nullptr;
    TOptional<double> PublicationAbsoluteTolerance,PublicationRelativeTolerance;
};
namespace StudioHome4Reports
{
    FString EscapeLaTeX(const FString& Value);
    /** Export a new directory atomically. Existing reports are never overwritten. */
    bool Export(const FString& Parent,const FString& Folder,const FStudioProject& Project,
        const FStudioHome4TelemetryStream* Telemetry,FString& OutPath,FString& Error,const FStudioHome4ReferenceEvidence* Evidence=nullptr,
        const FStudioHome4SpatialEvidence* Spatial=nullptr,const FStudioHome4TelemetryProvenance* TelemetryProvenance=nullptr,
        const FStudioHome4ReportInputs* Inputs=nullptr);
    bool FigureRun(const FStudioHome4Spec& Base,FStudioHome4Spec& Out,FString& Error);
    bool PublicationCheck(const FStudioHome4ReferenceEvidence& Current,const FStudioHome4ReferenceEvidence& Published,
        double AbsoluteTolerance,double RelativeTolerance,FString& Error);
}
