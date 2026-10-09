#pragma once
#include "StudioCase.h"

/** Retained next-run requests. These are not recorded counters or proof that an
 * adapter supports automatic stopping, data output or scheduled checkpoints. */
struct FStudioRunSettingsEdit
{
    enum EField : int32 { MaxSteps, MaxPhysicalTime, OutputInterval, CheckpointInterval, FieldCount };
    FGuid CaseId;
    FStudioCaseSetup Saved;
    FString Values[FieldCount];
    bool bCheckpoints = false;
    FString Error;
    int32 ErrorField = INDEX_NONE;

    void Reset(const FStudioCaseDraft& Case);
    bool IsDirty() const;
    /** Compare only the fields this form owns, preserving other case edits. */
    bool Matches(const FStudioCaseDraft& Case) const;
    /** Structural validation only; leaves Out unchanged on failure. */
    bool Build(FStudioCaseSetup& Out);
    static void CopySettings(const FStudioCaseSetup& From, FStudioCaseSetup& To);
};
