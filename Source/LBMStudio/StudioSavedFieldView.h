#pragma once
#include "StudioProject.h"

/** Shared original-frame/source/camera contract for independent analysis views.
 * JSON shape remains compatible with schema20 saved-comparison sides. */
namespace StudioSavedFieldViews
{
    bool IsValid(const FStudioSavedFieldView& View);
    bool SameIdentity(const FStudioFieldIdentity& A,const FStudioFieldIdentity& B);
    bool ReferenceMatches(const FStudioRecordingReference& Reference,const FStudioFieldIdentity& Identity);
    TSharedRef<FJsonObject> ToJSON(const FStudioSavedFieldView& View);
    bool FromJSON(const TSharedPtr<FJsonObject>& JSON,FStudioSavedFieldView& Out);
    /** Repair only paths from references with equal scientific hashes. */
    TArray<FStudioRecordingReference> ResolvedReference(const FStudioSavedFieldView& View,
        const TArray<FStudioRecordingReference>& Current);
}
