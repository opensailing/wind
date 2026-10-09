#pragma once
#include "StudioFieldExport.h"

namespace StudioCSVExport
{
    constexpr int32 BlockRows=4096;
    /** Worker-only wide UTF-8 CSV: one metadata comment, then a header and every
     * original point row. Coordinates and scalar values use %.17g, IDs Int64.
     * Scalar columns keep their IDs; coordinate headers are collision-safe.
     * Units, source identity and operations live in the single-line JSON comment.
     *
     * Selected columns are spooled to private temporary files and transposed in
     * 4096-row blocks (at most 2 MiB). At most one extra scalar snapshot is held.
     * Temporary disk use is 8*points*scalars bytes; output limit is 512 MiB.
     * Failure/cancellation removes spools, but the caller owns output staging
     * and must publish it only after success and successful archive close. */
    FStudioFieldExportResult Write(const FStudioFieldExportRequest& Request,FArchive& Archive,
        const FStudioLoadCancellation& Cancellation={},TFunction<void(int64,int64)> Progress={});
}
