#include "StudioModel.h"
#include "StudioPipeline.h"

bool FStudioModel::PipelineMessage(const FString& Message,bool bError)
{PipelineNotice=Message;bPipelineError=bError;Notice=Message;return !bError;}
const FStudioSavedPipeline* FStudioModel::FindPipeline(const FGuid& Id) const
{return Project.Pipelines.FindByPredicate([Id](const auto& S){return S.Id==Id;});}
bool FStudioModel::CommitPipelines(const FString& Label,TArray<FStudioSavedPipeline> Pipelines)
{
    if(bSnapshotView)return PipelineMessage(TEXT("Save pipelines from the Post-Processing workspace."),true);
    if(IsProjectOpenPending())return PipelineMessage(TEXT("Wait for project opening to finish, or cancel it, before editing saved pipelines."),true);
    FString Error;if(!StudioPipelines::IsValid(Pipelines,Error))return PipelineMessage(Error,true);
    if(StudioPipelines::Equals(Project.Pipelines,Pipelines))return PipelineMessage(TEXT("Saved pipeline is already up to date."));
    const int64 Bytes=StudioPipelines::StoredBytes(Project.Pipelines)+StudioPipelines::StoredBytes(Pipelines);
    constexpr int64 Budget=16LL*1024*1024;
    if(Bytes>Budget)return PipelineMessage(TEXT("These pipeline settings exceed the saved-edit memory limit. Remove a saved pipeline first."),true);
    PipelineRedo.Reset();PipelineUndo.Add({Label,Project.Pipelines,Pipelines,Bytes});
    int64 Total=0;for(const auto& Edit:PipelineUndo)Total+=Edit.Bytes;
    while(PipelineUndo.Num()>64||Total>Budget){Total-=PipelineUndo[0].Bytes;PipelineUndo.RemoveAt(0);}
    Project.Pipelines=MoveTemp(Pipelines);++PipelineRevision;bDirty=true;return PipelineMessage(Label);
}
bool FStudioModel::AddPipeline(FStudioSavedPipeline Saved)
{
    Saved.Id=FGuid::NewGuid();Saved.Name=Saved.Name.TrimStartAndEnd();auto Items=Project.Pipelines;Items.Add(MoveTemp(Saved));
    const FString Label=TEXT("Saved pipeline: ")+Items.Last().Name;return CommitPipelines(Label,MoveTemp(Items));
}
bool FStudioModel::UpdatePipeline(const FGuid& Id,FStudioSavedPipeline Saved)
{
    auto Items=Project.Pipelines;auto* Existing=Items.FindByPredicate([Id](const auto& S){return S.Id==Id;});
    if(!Existing)return PipelineMessage(TEXT("This pipeline is no longer saved. Save it as a new pipeline."),true);
    Saved.Id=Id;Saved.Name=Existing->Name;*Existing=MoveTemp(Saved);const FString Label=TEXT("Updated pipeline: ")+Existing->Name;
    return CommitPipelines(Label,MoveTemp(Items));
}
bool FStudioModel::RenamePipeline(const FGuid& Id,const FString& Name)
{
    auto Items=Project.Pipelines;auto* Existing=Items.FindByPredicate([Id](const auto& S){return S.Id==Id;});
    if(!Existing)return PipelineMessage(TEXT("This pipeline is no longer saved."),true);
    Existing->Name=Name.TrimStartAndEnd();const FString Label=TEXT("Renamed pipeline: ")+Existing->Name;return CommitPipelines(Label,MoveTemp(Items));
}
bool FStudioModel::DeletePipeline(const FGuid& Id)
{
    const auto* Existing=FindPipeline(Id);if(!Existing)return PipelineMessage(TEXT("This pipeline is no longer saved."),true);
    const FString Label=TEXT("Deleted pipeline: ")+Existing->Name+TEXT(". Undo pipelines restores it.");auto Items=Project.Pipelines;
    Items.RemoveAll([Id](const auto& S){return S.Id==Id;});return CommitPipelines(Label,MoveTemp(Items));
}
bool FStudioModel::ApplyPipelineHistory(bool bRedo)
{
    if(bSnapshotView||IsProjectOpenPending())return PipelineMessage(TEXT("The current view cannot edit saved pipelines while a project is opening."),true);
    auto& From=bRedo?PipelineRedo:PipelineUndo;auto& To=bRedo?PipelineUndo:PipelineRedo;
    if(From.IsEmpty())return PipelineMessage(TEXT("No saved-pipeline edit is available to restore."),true);
    const auto& Edit=From.Last();
    if(!StudioPipelines::Equals(Project.Pipelines,bRedo?Edit.Before:Edit.After))
        return PipelineMessage(TEXT("Saved pipelines changed outside this history. Current settings kept."),true);
    Project.Pipelines=bRedo?Edit.After:Edit.Before;const FString Label=(bRedo?TEXT("Redo: "):TEXT("Undo: "))+Edit.Label;
    To.Add(From.Pop());++PipelineRevision;bDirty=true;return PipelineMessage(Label);
}
bool FStudioModel::UndoPipelines(){return ApplyPipelineHistory(false);}
bool FStudioModel::RedoPipelines(){return ApplyPipelineHistory(true);}
void FStudioModel::ClearPipelineHistory()
{PipelineUndo.Reset();PipelineRedo.Reset();PipelineNotice.Empty();bPipelineError=false;++PipelineRevision;}
