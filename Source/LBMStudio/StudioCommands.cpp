#include "StudioCommands.h"
#include "StudioModel.h"

namespace
{
    const FStudioCommandSpec Specs[]={
        {EStudioCommand::Help,TEXT("help"),TEXT("List supported commands")},
        {EStudioCommand::Status,TEXT("status"),TEXT("Show replay and control state")},
        {EStudioCommand::ProjectSave,TEXT("project save"),TEXT("Save the current project")},
        {EStudioCommand::ProjectSaveAs,TEXT("project save-as"),TEXT("Save to a chosen project file")},
        {EStudioCommand::ViewFit,TEXT("view fit"),TEXT("Fit the camera to the flow domain")},
        {EStudioCommand::ViewUndo,TEXT("view undo"),TEXT("Undo the last view edit")},
        {EStudioCommand::ViewRedo,TEXT("view redo"),TEXT("Redo the last view edit")},
        {EStudioCommand::ReplayRun,TEXT("replay run"),TEXT("Start recorded playback")},
        {EStudioCommand::ReplayPause,TEXT("replay pause"),TEXT("Pause running playback")},
        {EStudioCommand::ReplayResume,TEXT("replay resume"),TEXT("Resume paused playback")},
        {EStudioCommand::ReplayStop,TEXT("replay stop"),TEXT("Stop recorded playback")},
        {EStudioCommand::ReplayStep,TEXT("replay step"),TEXT("Advance one source frame")},
        {EStudioCommand::JobSubmit,TEXT("job submit"),TEXT("Submit a new control job")},
        {EStudioCommand::JobPause,TEXT("job pause"),TEXT("Request job pause")},
        {EStudioCommand::JobResume,TEXT("job resume"),TEXT("Request job resume")},
        {EStudioCommand::JobStop,TEXT("job stop"),TEXT("Request job stop")},
        {EStudioCommand::JobStep,TEXT("job step"),TEXT("Request one control step")},
        {EStudioCommand::JobCheckpoint,TEXT("job checkpoint"),TEXT("Request a checkpoint command")},
        {EStudioCommand::JobReconnect,TEXT("job reconnect"),TEXT("Query a disconnected job")}
    };

    bool Normalize(const FString& Input,FString& Out)
    {
        Out.Empty();if(Input.Len()>StudioCommands::InputLimit)return false;
        bool bSpace=false;
        for(TCHAR C:Input)
        {
            // Only ordinary spaces are accepted. Pasted newlines must never
            // turn a sequence of commands into a single accepted action.
            if(C==TEXT(' ')){bSpace=!Out.IsEmpty();continue;}
            if(!((C>=TEXT('a')&&C<=TEXT('z'))||(C>=TEXT('A')&&C<=TEXT('Z'))||C==TEXT('-')))return false;
            if(bSpace){Out+=TEXT(' ');bSpace=false;}
            Out+=FChar::ToLower(C);
        }
        return true;
    }

    TOptional<EStudioJobCommand> JobCommand(EStudioCommand Id)
    {
        switch(Id)
        {
        case EStudioCommand::JobSubmit:return EStudioJobCommand::Submit;
        case EStudioCommand::JobPause:return EStudioJobCommand::Pause;
        case EStudioCommand::JobResume:return EStudioJobCommand::Resume;
        case EStudioCommand::JobStop:return EStudioJobCommand::Stop;
        case EStudioCommand::JobStep:return EStudioJobCommand::Step;
        case EStudioCommand::JobCheckpoint:return EStudioJobCommand::Checkpoint;
        case EStudioCommand::JobReconnect:return EStudioJobCommand::Reconnect;
        default:return {};
        }
    }
}

TConstArrayView<FStudioCommandSpec> StudioCommands::Registry(){return MakeArrayView(Specs);}
const FStudioCommandSpec* StudioCommands::Find(EStudioCommand Id)
{for(const auto& S:Specs)if(S.Id==Id)return &S;return nullptr;}
bool StudioCommands::Parse(const FString& Input,EStudioCommand& Out,FString& Error)
{
    Error.Empty();FString Name;
    if(!Normalize(Input,Name))
    {Error=TEXT("Use one command of at most 256 letters, spaces and hyphens. Type help for supported commands.");return false;}
    if(Name.IsEmpty()){Error=TEXT("Enter a command, or type help.");return false;}
    for(const auto& S:Specs)if(Name==S.Name){Out=S.Id;return true;}
    Error=TEXT("Unsupported command. Type help for the exact names; commands take no arguments.");return false;
}
TArray<FString> StudioCommands::Complete(const FString& Prefix)
{
    FString Name;TArray<FString> Matches;if(!Normalize(Prefix,Name))return Matches;
    for(const auto& S:Specs)if(Name.IsEmpty()||FString(S.Name).StartsWith(Name))Matches.Add(S.Name);
    return Matches;
}
bool StudioCommands::Validate(const FStudioModel& M,EStudioCommand C,FString& Error)
{
    Error.Empty();const auto* Spec=Find(C);
    if(!Spec){Error=TEXT("Unsupported command.");return false;}
    if(C==EStudioCommand::Help||C==EStudioCommand::Status)return true;
    if(M.IsProjectOpenPending()){Error=TEXT("Finish or cancel project opening before sending this command.");return false;}
    if(const auto J=JobCommand(C))
    {
        if(!M.Project.bControlHarness)
        {Error=TEXT("Select Control harness in Solve > Setup before using job commands.");return false;}
        // The toolbar's Run/Pause controls toggle to Resume. Literal commands
        // must instead satisfy the controller's exact operation and capability.
        if(M.Job().Can(J.GetValue())&&M.CanControl(J.GetValue()))return true;
        Error=FString::Printf(TEXT("%s is unavailable: control job is %s. Wait for pending replies or use an available command."),Spec->Name,*StudioJobs::StateName(M.Job().State()));
        return false;
    }
    if(C>=EStudioCommand::ReplayRun&&C<=EStudioCommand::ReplayStep)
    {
        if(M.Project.bControlHarness)
        {Error=TEXT("Select Replay in Solve > Setup before using replay commands.");return false;}
        bool OK=false;
        switch(C)
        {
        case EStudioCommand::ReplayRun:OK=M.State!=EStudioRunState::Paused&&M.CanControl(EStudioJobCommand::Submit);break;
        case EStudioCommand::ReplayPause:OK=M.State==EStudioRunState::Running;break;
        case EStudioCommand::ReplayResume:OK=M.State==EStudioRunState::Paused;break;
        case EStudioCommand::ReplayStop:OK=M.CanControl(EStudioJobCommand::Stop);break;
        case EStudioCommand::ReplayStep:OK=M.CanControl(EStudioJobCommand::Step);break;
        default:break;
        }
        if(OK)return true;
        Error=FString::Printf(TEXT("%s is unavailable: playback is %s.%s"),Spec->Name,*M.StatusText(),
            M.State==EStudioRunState::Paused?TEXT(" Use replay resume to continue."):TEXT(""));return false;
    }
    if(C==EStudioCommand::ViewUndo&&!M.CanUndoView()){Error=TEXT("No view edit to undo.");return false;}
    if(C==EStudioCommand::ViewRedo&&!M.CanRedoView()){Error=TEXT("No view edit to redo.");return false;}
    return true;
}
bool StudioCommands::ExecuteModel(FStudioModel& M,EStudioCommand C,FString& Response)
{
    if(!Validate(M,C,Response))return false;
    if(C==EStudioCommand::Help)
    {
        Response=TEXT("help · status\nproject save · project save-as\nview fit · view undo · view redo\nreplay run · replay pause · replay resume · replay stop · replay step\njob submit · job pause · job resume · job stop · job step · job checkpoint · job reconnect");return true;
    }
    if(C==EStudioCommand::Status)
    {
        Response=FString::Printf(TEXT("Controls: %s. Playback: %s, recording position %d / %d. Control job: %s%s."),
            M.Project.bControlHarness?TEXT("Control harness"):TEXT("Recorded playback"),*M.StatusText(),M.Solver->FrameCount()?M.PlaybackFrame+1:0,M.Solver->FrameCount(),
            *StudioJobs::StateName(M.Job().State()),M.Job().IsPending()?TEXT(" (reply pending)"):TEXT(""));return true;
    }
    if(const auto J=JobCommand(C))
    {const bool OK=M.Control(J.GetValue());Response=M.Notice;return OK;}
    switch(C)
    {
    case EStudioCommand::ReplayRun:case EStudioCommand::ReplayResume:M.Run();break;
    case EStudioCommand::ReplayPause:M.Pause();break;
    case EStudioCommand::ReplayStop:M.Stop();break;
    case EStudioCommand::ReplayStep:M.Step();break;
    default:Response=TEXT("This application command requires the workspace handler.");return false;
    }
    Response=FString::Printf(TEXT("Playback: %s · recording position %d / %d."),*M.StatusText(),M.PlaybackFrame+1,M.Solver->FrameCount());return true;
}

void FStudioCommandHistory::Edited()
{Cursor=INDEX_NONE;SavedDraft.Empty();Completions.Reset();CompletionIndex=INDEX_NONE;LastCompletion.Empty();}
void FStudioCommandHistory::Remember(EStudioCommand Command)
{
    const auto* S=StudioCommands::Find(Command);if(!S)return;
    if(Entries.IsEmpty()||Entries.Last()!=S->Name)
    {if(Entries.Num()==Capacity)Entries.RemoveAt(0,1,EAllowShrinking::No);Entries.Add(S->Name);}
    Edited();
}
FString FStudioCommandHistory::Previous(const FString& Draft)
{
    Completions.Reset();CompletionIndex=INDEX_NONE;
    if(Entries.IsEmpty())return Draft;
    if(Cursor==INDEX_NONE){SavedDraft=Draft.Left(StudioCommands::InputLimit);Cursor=Entries.Num();}
    Cursor=FMath::Max(0,Cursor-1);return Entries[Cursor];
}
FString FStudioCommandHistory::Next(const FString& Draft)
{
    Completions.Reset();CompletionIndex=INDEX_NONE;
    if(Cursor==INDEX_NONE)return Draft;
    if(++Cursor>=Entries.Num()){Cursor=INDEX_NONE;return SavedDraft;}
    return Entries[Cursor];
}
FString FStudioCommandHistory::Complete(const FString& Draft,bool bReverse)
{
    Cursor=INDEX_NONE;
    if(CompletionIndex==INDEX_NONE||Draft!=LastCompletion)
    {Completions=StudioCommands::Complete(Draft);CompletionIndex=bReverse?Completions.Num():-1;}
    if(Completions.IsEmpty())return Draft;
    CompletionIndex=(CompletionIndex+(bReverse?-1:1)+Completions.Num())%Completions.Num();
    LastCompletion=Completions[CompletionIndex];return LastCompletion;
}
