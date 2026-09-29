#pragma once
#include "StudioRecording.h"

class IStudioField;
class FArchive;

enum class EStudioExportCoordinates : uint8 { Source, Scene };
enum class EStudioFieldExportFormat : uint8 { VTK, CSV };

/** A frozen original frame, retained while a destination is selected/written.
 * Scalar components always retain their source basis; only point coordinates
 * change when Scene is requested. No display reconstruction is exported. */
struct FStudioFieldExportRequest
{
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Field;
    TArray<FString> Scalars;
    EStudioExportCoordinates Coordinates=EStudioExportCoordinates::Source;
    EStudioFieldExportFormat Format=EStudioFieldExportFormat::VTK;
};
struct FStudioFieldExportResult
{
    bool bSuccess=false,bCancelled=false;
    FString Error,Path;
    FStudioFieldIdentity Identity;
    int32 Points=0,Triangles=0,Lines=0,Rows=0;
    FGuid PipelineId;
    FString PipelineName;
    int64 Bytes=0;
};

namespace StudioFieldExport
{
    constexpr int64 MaximumBytes=512LL*1024*1024;
    /** Shared original-row validation and exact additional-scalar reads. */
    bool Validate(const FStudioFieldExportRequest& Request,FArchive& Archive,
        const FStudioLoadCancellation& Cancellation,FStudioFieldExportResult& Out);
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> LoadScalar(const FStudioFieldExportRequest& Request,
        const FString& Id,const FStudioLoadCancellation& Cancellation,FStudioFieldExportResult& Out);
    bool SamePoint(const IStudioField& Original,const IStudioField& Loaded,int32 Row);
    /** Call after Validate succeeds; condensed single-line JSON for CSV. */
    FString Metadata(const FStudioFieldExportRequest& Request,const FStudioFieldIdentity& Identity,
        int32 Triangles,const FString& PointIdName);
}
