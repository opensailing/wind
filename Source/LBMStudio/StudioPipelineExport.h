#pragma once
#include "StudioPipelineEvaluation.h"
#include "StudioFieldExport.h"

/** Frozen evaluated output, independent of later recipe/frame/camera changes.
 * Geometry exports keep the evaluated topology, including derived positions.
 * Probe CSV retains every requested row and missing-value status. */
struct FStudioPipelineExportRequest
{
    FStudioPipelineEvaluationResult Evaluation;
    EStudioExportCoordinates Coordinates=EStudioExportCoordinates::Scene;
    EStudioFieldExportFormat Format=EStudioFieldExportFormat::VTK;
};

namespace StudioPipelineExport
{
    constexpr int64 MaximumBytes=StudioFieldExport::MaximumBytes;
    /** Worker-only bounded UTF-8 writer. VTK PolyData (.vtp) preserves points,
     * segments and triangles in Float64; CSV writes vertices or full probe rows.
     * Probe tables require CSV. Empty evaluated geometry is a valid export.
     * Metadata embeds source/reconstruction hashes, original frame, full recipe,
     * scalar meaning, coordinate transform and the evaluator's numerical method.
     * Derived positions have no original ID (blank CSV; explicit VTK validity
     * mask). Coordinate choice never rotates scalar components.
     * Caller owns staging and must publish only after success and archive close.
     */
    FStudioFieldExportResult Write(const FStudioPipelineExportRequest& Request,FArchive& Archive,
        const FStudioLoadCancellation& Cancellation={},TFunction<void(int64,int64)> Progress={});
}
