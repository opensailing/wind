#pragma once
#include "StudioHistory.h"

namespace StudioResiduals
{
    /** Worker-only, streamed reader of one completed OpenFOAM text log.
     * Selects the first initial and last final residual per field/time, retaining
     * original line numbers. No missing values, normalization, physical time
     * units, convergence criteria or association to a flow recording are inferred.
     * Cancelling or rejecting a log publishes no partial history. The source is
     * read only; callers retain its path and interpretation hash for reopening.
     */
    FStudioHistoryLoadResult Load(const FString& Path,
        const FStudioLoadCancellation& Cancellation = {},
        const FString& ExpectedMetadataSHA256 = FString());
}
