#pragma once
#include "CoreMinimal.h"

class FStudioModel;

enum class EStudioCommand : uint8
{
    Help, Status, ProjectSave, ProjectSaveAs, ViewFit, ViewUndo, ViewRedo,
    ReplayRun, ReplayPause, ReplayResume, ReplayStop, ReplayStep,
    JobSubmit, JobPause, JobResume, JobStop, JobStep, JobCheckpoint, JobReconnect
};

struct FStudioCommandSpec
{
    EStudioCommand Id;
    const TCHAR* Name;
    const TCHAR* Description;
};

/** Exact application vocabulary. No arguments, aliases, shell, or engine console
 * fallback. Validation is repeated against current state immediately before use. */
namespace StudioCommands
{
    constexpr int32 InputLimit=256;
    TConstArrayView<FStudioCommandSpec> Registry();
    const FStudioCommandSpec* Find(EStudioCommand Id);
    bool Parse(const FString& Input,EStudioCommand& Out,FString& Error);
    TArray<FString> Complete(const FString& Prefix);
    bool Validate(const FStudioModel& Model,EStudioCommand Command,FString& Error);
    /** Model commands only. Project save and camera actions require the workspace
     * to resolve retained edits, native dialogs and the scene through its handlers. */
    bool ExecuteModel(FStudioModel& Model,EStudioCommand Command,FString& Response);
}

/** Session-only, bounded command recall. Only recognized commands enter history.
 * Recall and completion return text; neither can dispatch an action. */
class FStudioCommandHistory
{
public:
    static constexpr int32 Capacity=64;
    void Remember(EStudioCommand Command);
    FString Previous(const FString& Draft);
    FString Next(const FString& Draft);
    FString Complete(const FString& Draft,bool bReverse=false);
    void Edited();
    int32 Num() const { return Entries.Num(); }
private:
    TArray<FString> Entries,Completions;
    FString SavedDraft,LastCompletion;
    int32 Cursor=INDEX_NONE,CompletionIndex=INDEX_NONE;
};
