#include "StudioModel.h"
#include "StudioSavedComparison.h"

bool FStudioModel::ComparisonMessage(const FString& Message,bool bError)
{ComparisonNotice=Message;bComparisonError=bError;Notice=Message;return !bError;}
const FStudioSavedComparison* FStudioModel::FindComparison(const FGuid& Id) const
{return Project.Comparisons.FindByPredicate([Id](const auto& S){return S.Id==Id;});}
bool FStudioModel::CommitComparisons(const FString& Label,TArray<FStudioSavedComparison> Comparisons)
{
    if(bSnapshotView)return ComparisonMessage(TEXT("Save comparisons from the Results workspace."),true);
    if(IsProjectOpenPending())return ComparisonMessage(TEXT("Wait for project opening to finish, or cancel it, before editing saved comparisons."),true);
    FString Error;if(!StudioSavedComparisons::IsValid(Comparisons,Error))return ComparisonMessage(Error,true);
    if(StudioSavedComparisons::Equals(Project.Comparisons,Comparisons))return ComparisonMessage(TEXT("Saved comparison is already up to date."));
    const int64 Bytes=StudioSavedComparisons::StoredBytes(Project.Comparisons)+StudioSavedComparisons::StoredBytes(Comparisons);
    constexpr int64 Budget=16LL*1024*1024;
    if(Bytes>Budget)return ComparisonMessage(TEXT("These comparison settings exceed the saved-edit memory limit. Remove a saved comparison first."),true);
    ComparisonRedo.Reset();ComparisonUndo.Add({Label,Project.Comparisons,Comparisons,Bytes});
    int64 Total=0;for(const auto& Edit:ComparisonUndo)Total+=Edit.Bytes;
    while(ComparisonUndo.Num()>64||Total>Budget){Total-=ComparisonUndo[0].Bytes;ComparisonUndo.RemoveAt(0);}
    Project.Comparisons=MoveTemp(Comparisons);++ComparisonRevision;bDirty=true;return ComparisonMessage(Label);
}
bool FStudioModel::AddComparison(FStudioSavedComparison Saved)
{
    Saved.Id=FGuid::NewGuid();Saved.Name=Saved.Name.TrimStartAndEnd();auto Items=Project.Comparisons;Items.Add(MoveTemp(Saved));
    const FString Label=TEXT("Saved comparison: ")+Items.Last().Name;return CommitComparisons(Label,MoveTemp(Items));
}
bool FStudioModel::UpdateComparison(const FGuid& Id,FStudioSavedComparison Saved)
{
    auto Items=Project.Comparisons;auto* Existing=Items.FindByPredicate([Id](const auto& S){return S.Id==Id;});
    if(!Existing)return ComparisonMessage(TEXT("This comparison is no longer saved. Save it as a new comparison."),true);
    Saved.Id=Id;Saved.Name=Existing->Name;*Existing=MoveTemp(Saved);const FString Label=TEXT("Updated comparison: ")+Existing->Name;
    return CommitComparisons(Label,MoveTemp(Items));
}
bool FStudioModel::RenameComparison(const FGuid& Id,const FString& Name)
{
    auto Items=Project.Comparisons;auto* Existing=Items.FindByPredicate([Id](const auto& S){return S.Id==Id;});
    if(!Existing)return ComparisonMessage(TEXT("This comparison is no longer saved."),true);
    Existing->Name=Name.TrimStartAndEnd();const FString Label=TEXT("Renamed comparison: ")+Existing->Name;return CommitComparisons(Label,MoveTemp(Items));
}
bool FStudioModel::DeleteComparison(const FGuid& Id)
{
    const auto* Existing=FindComparison(Id);if(!Existing)return ComparisonMessage(TEXT("This comparison is no longer saved."),true);
    const FString Label=TEXT("Deleted comparison: ")+Existing->Name+TEXT(". Undo comparisons restores it.");auto Items=Project.Comparisons;
    Items.RemoveAll([Id](const auto& S){return S.Id==Id;});return CommitComparisons(Label,MoveTemp(Items));
}
bool FStudioModel::ApplyComparisonHistory(bool bRedo)
{
    if(bSnapshotView||IsProjectOpenPending())return ComparisonMessage(TEXT("The current view cannot edit saved comparisons while a project is opening."),true);
    auto& From=bRedo?ComparisonRedo:ComparisonUndo;auto& To=bRedo?ComparisonUndo:ComparisonRedo;
    if(From.IsEmpty())return ComparisonMessage(TEXT("No saved-comparison edit is available to restore."),true);
    const auto& Edit=From.Last();
    if(!StudioSavedComparisons::Equals(Project.Comparisons,bRedo?Edit.Before:Edit.After))
        return ComparisonMessage(TEXT("Saved comparisons changed outside this history. Current settings kept."),true);
    Project.Comparisons=bRedo?Edit.After:Edit.Before;const FString Label=(bRedo?TEXT("Redo: "):TEXT("Undo: "))+Edit.Label;
    To.Add(From.Pop());++ComparisonRevision;bDirty=true;return ComparisonMessage(Label);
}
bool FStudioModel::UndoComparisons(){return ApplyComparisonHistory(false);}
bool FStudioModel::RedoComparisons(){return ApplyComparisonHistory(true);}
void FStudioModel::ClearComparisonHistory()
{ComparisonUndo.Reset();ComparisonRedo.Reset();ComparisonNotice.Empty();bComparisonError=false;++ComparisonRevision;}
