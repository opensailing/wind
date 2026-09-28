#pragma once
#include "StudioRecording.h"

class IStudioField;
class FArchive;

enum class EStudioExportCoordinates : uint8 { Source, Scene };

/** A frozen original frame, retained while a destination is selected/written.
 * Scalar components always retain their source basis; only point coordinates
 * change when Scene is requested. No display reconstruction is exported. */
struct FStudioVTKExportRequest
{
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Field;
    TArray<FString> Scalars;
    EStudioExportCoordinates Coordinates=EStudioExportCoordinates::Source;
};
struct FStudioVTKExportResult
{
    bool bSuccess=false,bCancelled=false;
    FString Error,Path;
    FStudioFieldIdentity Identity;
    int32 Points=0,Triangles=0;
    int64 Bytes=0;
};

namespace StudioVTKExport
{
    constexpr int64 MaximumBytes=512LL*1024*1024;
    /** Worker-only VTK XML PolyData (.vtp). Writes bounded UTF-8 chunks using
     * Float64 point/scalar arrays and Int64 original IDs. Original triangles
     * become polygons; point-only sources get vertex cells, never inferred
     * connectivity. Additional scalar snapshots must match the pinned frame.
     *
     * The caller owns a private staging archive and may publish it only after
     * success AND successful close. Failure/cancellation may leave partial
     * staging bytes. Progress counts completed original rows/cells, monotonically.
     * Metadata is UTF-8 JSON in UInt8 FieldData LBMStudioMetadataUTF8; TimeValue
     * carries original seconds for independent VTK time-aware readers.
     */
    FStudioVTKExportResult Write(const FStudioVTKExportRequest& Request,FArchive& Archive,
        const FStudioLoadCancellation& Cancellation={},TFunction<void(int64,int64)> Progress={});
}
