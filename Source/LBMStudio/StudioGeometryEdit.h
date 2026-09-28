#pragma once

#include "StudioCase.h"

/** Retained object edits. Import units, source bytes, patches and assignments
 * are owned elsewhere; this form changes only the name and case transform. */
struct FStudioGeometryEdit
{
    FStudioGeometryAsset Saved;
    FString Name;
    // Translation XYZ (m), roll/pitch/yaw (degrees), source-axis scale XYZ.
    FString Values[9];
    FString Error;
    int32 ErrorField = INDEX_NONE; // 0 name; 1-9 Values.

    void Reset(const FStudioGeometryAsset& Asset);
    bool IsDirty() const;
    /** A byte-identical relocation or material assignment does not conflict. */
    bool Matches(const FStudioGeometryAsset& Asset) const;
    /** Leaves Out untouched on failure. Unedited rotations remain bit-exact. */
    bool Build(FStudioGeometryAsset& Out);
};
