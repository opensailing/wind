#pragma once
#include "StudioFieldExport.h"

namespace StudioVTKExport
{
    constexpr int64 MaximumBytes=StudioFieldExport::MaximumBytes;
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
    FStudioFieldExportResult Write(const FStudioFieldExportRequest& Request,FArchive& Archive,
        const FStudioLoadCancellation& Cancellation={},TFunction<void(int64,int64)> Progress={});
}
