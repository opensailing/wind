#pragma once
#include "StudioHome4SpatialDiagnostics.h"
#include "StudioPointRecording.h"
namespace StudioHome4PatchBinding
{
    /** Bound by the immutable recording metadata; editable case units never participate. */
    bool Matches(const FStudioHome4SpatialEvidence& Evidence,const FString& PatchId,
        const FStudioPointStructuredGrid& Grid,FString& Error);
}
