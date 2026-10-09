#include "StudioModel.h"

namespace StudioCameraCollectionPrivate
{
    constexpr int32 MaxCameras=128;
    constexpr int32 MaxHistory=64;

    bool Equal(const TArray<FStudioCameraBookmark>& A,const TArray<FStudioCameraBookmark>& B)
    {
        if(A.Num()!=B.Num()) return false;
        for(int32 I=0;I<A.Num();++I)
            if(A[I].Id!=B[I].Id || A[I].Name!=B[I].Name || !StudioView::CameraEquals(A[I].Camera,B[I].Camera)) return false;
        return true;
    }
    bool ValidName(const FString& Name)
    {
        if(Name.IsEmpty() || Name.Len()>120 || Name!=Name.TrimStartAndEnd()) return false;
        for(const TCHAR C:Name) if(C<32 || C==127) return false;
        return true;
    }
}

bool FStudioModel::CameraCollectionMessage(const FString& Message,bool bError)
{
    CameraCollectionNotice=Message; bCameraCollectionError=bError; Notice=Message;
    return !bError;
}

const FStudioCameraBookmark* FStudioModel::FindCamera(const FGuid& Id) const
{ return Project.Cameras.FindByPredicate([Id](const auto& B){return B.Id==Id;}); }

bool FStudioModel::CommitCameras(const FString& Label,TArray<FStudioCameraBookmark> Cameras)
{
    if(IsProjectOpenPending()) return CameraCollectionMessage(TEXT("Wait for project opening to finish, or cancel it, before editing saved cameras."),true);
    if(Cameras.Num()>StudioCameraCollectionPrivate::MaxCameras)
        return CameraCollectionMessage(TEXT("This project has 128 saved cameras. Delete one before saving or duplicating another."),true);
    TSet<FGuid> Ids; TSet<FString> Names;
    for(const auto& B:Cameras)
    {
        if(!StudioCameraCollectionPrivate::ValidName(B.Name))
            return CameraCollectionMessage(TEXT("Use a camera name of 1–120 characters on one line."),true);
        if(Names.Contains(B.Name.ToLower()))
            return CameraCollectionMessage(TEXT("A camera already has that name. Choose a different name."),true);
        FStudioInspectionState Check; Check.Camera=B.Camera;
        if(!B.Id.IsValid() || Ids.Contains(B.Id) || !StudioView::IsValid(Check))
            return CameraCollectionMessage(TEXT("The saved camera contains invalid values. The previous cameras have been kept."),true);
        Ids.Add(B.Id); Names.Add(B.Name.ToLower());
    }
    if(StudioCameraCollectionPrivate::Equal(Project.Cameras,Cameras))
        return CameraCollectionMessage(TEXT("Saved camera is already up to date."));
    EndViewEdit();
    CameraUndo.Add({Label,Project.Cameras,Cameras});
    if(CameraUndo.Num()>StudioCameraCollectionPrivate::MaxHistory) CameraUndo.RemoveAt(0);
    CameraRedo.Reset(); Project.Cameras=MoveTemp(Cameras); ++CameraCollectionRevision; bDirty=true;
    return CameraCollectionMessage(Label);
}

bool FStudioModel::AddCamera(const FString& Name,const FStudioCameraState& Camera)
{
    auto Cameras=Project.Cameras; FStudioCameraBookmark B;
    B.Name=Name.TrimStartAndEnd(); B.Camera=Camera; Cameras.Add(B);
    return CommitCameras(TEXT("Saved camera: ")+B.Name,MoveTemp(Cameras));
}

bool FStudioModel::RenameCamera(const FGuid& Id,const FString& Name)
{
    auto Cameras=Project.Cameras;
    auto* B=Cameras.FindByPredicate([Id](const auto& Item){return Item.Id==Id;});
    if(!B) return CameraCollectionMessage(TEXT("This camera is no longer in the project."),true);
    B->Name=Name.TrimStartAndEnd();
    const FString Label=TEXT("Renamed camera: ")+B->Name;
    return CommitCameras(Label,MoveTemp(Cameras));
}

bool FStudioModel::UpdateCamera(const FGuid& Id,const FStudioCameraState& Camera)
{
    auto Cameras=Project.Cameras;
    auto* B=Cameras.FindByPredicate([Id](const auto& Item){return Item.Id==Id;});
    if(!B) return CameraCollectionMessage(TEXT("This camera is no longer in the project."),true);
    B->Camera=Camera;
    const FString Label=TEXT("Updated camera from current view: ")+B->Name;
    return CommitCameras(Label,MoveTemp(Cameras));
}

bool FStudioModel::DuplicateCamera(const FGuid& Id)
{
    const auto* Source=FindCamera(Id);
    if(!Source) return CameraCollectionMessage(TEXT("This camera is no longer in the project."),true);
    FStudioCameraBookmark Copy=*Source; Copy.Id=FGuid::NewGuid();
    for(int32 I=1;I<=StudioCameraCollectionPrivate::MaxCameras+1;++I)
    {
        const FString Suffix=I==1?TEXT(" copy"):FString::Printf(TEXT(" copy %d"),I);
        Copy.Name=Source->Name.Left(120-Suffix.Len()).TrimEnd()+Suffix;
        if(!Project.Cameras.ContainsByPredicate([&Copy](const auto& B){return B.Name.Equals(Copy.Name,ESearchCase::IgnoreCase);})) break;
    }
    auto Cameras=Project.Cameras; Cameras.Add(Copy);
    return CommitCameras(TEXT("Duplicated camera: ")+Copy.Name,MoveTemp(Cameras));
}

bool FStudioModel::DeleteCamera(const FGuid& Id)
{
    const auto* B=FindCamera(Id);
    if(!B) return CameraCollectionMessage(TEXT("This camera is no longer in the project."),true);
    const FString Label=TEXT("Deleted camera: ")+B->Name;
    auto Cameras=Project.Cameras; Cameras.RemoveAll([Id](const auto& Item){return Item.Id==Id;});
    return CommitCameras(Label,MoveTemp(Cameras));
}

bool FStudioModel::RestoreSavedCamera(const FGuid& Id)
{
    const auto* B=FindCamera(Id);
    if(!B) return CameraCollectionMessage(TEXT("This camera is no longer in the project."),true);
    EndViewEdit();
    if(!EditCamera(TEXT("Restore ")+B->Name,B->Camera)) return CameraCollectionMessage(Notice,true);
    return CameraCollectionMessage(TEXT("Activated camera: ")+B->Name);
}

bool FStudioModel::ApplyCameraCollectionHistory(bool bRedo)
{
    if(IsProjectOpenPending()) return CameraCollectionMessage(TEXT("Wait for project opening to finish, or cancel it, before editing saved cameras."),true);
    auto& From=bRedo?CameraRedo:CameraUndo;
    auto& To=bRedo?CameraUndo:CameraRedo;
    if(From.IsEmpty()) return CameraCollectionMessage(TEXT("No saved-camera edit is available to restore."),true);
    const auto& Entry=From.Last();
    if(!StudioCameraCollectionPrivate::Equal(Project.Cameras,bRedo?Entry.Before:Entry.After))
        return CameraCollectionMessage(TEXT("Saved cameras changed outside this history. The current cameras have been kept."),true);
    EndViewEdit(); Project.Cameras=bRedo?Entry.After:Entry.Before;
    const FString Label=(bRedo?TEXT("Redo: "):TEXT("Undo: "))+Entry.Label;
    To.Add(From.Pop()); ++CameraCollectionRevision; bDirty=true;
    return CameraCollectionMessage(Label);
}

bool FStudioModel::UndoSavedCameras() { return ApplyCameraCollectionHistory(false); }
bool FStudioModel::RedoSavedCameras() { return ApplyCameraCollectionHistory(true); }
void FStudioModel::ClearCameraCollectionHistory()
{
    Placement.Reset();++CameraPlacementRevision;CameraPlacementNotice.Empty();bCameraPlacementError=false;
    CameraUndo.Reset(); CameraRedo.Reset(); CameraCollectionNotice.Empty(); bCameraCollectionError=false;
    ++CameraCollectionRevision;
}

bool FStudioModel::IsCameraPlacementCurrent() const
{
    return Placement.IsSet()&&Placement->ProjectId==Project.Id&&Placement->CollectionRevision==CameraCollectionRevision&&
        FindCamera(Placement->CameraId)!=nullptr&&!IsProjectOpenPending();
}
bool FStudioModel::BeginCameraPlacement(const FGuid& Id)
{
    if(Placement.IsSet())return CameraCollectionMessage(TEXT("Apply or cancel the current camera placement first."),true);
    if(IsProjectOpenPending())return CameraCollectionMessage(TEXT("Wait for project opening to finish before placing a camera."),true);
    const auto* Saved=FindCamera(Id);
    if(!Saved)return CameraCollectionMessage(TEXT("This camera is no longer in the project."),true);
    FStudioCameraPlacementDraft Draft;Draft.ProjectId=Project.Id;Draft.CameraId=Id;
    Draft.CollectionRevision=CameraCollectionRevision;Draft.Camera=Saved->Camera;
    Placement=Draft;++CameraPlacementRevision;bCameraPlacementError=false;
    CameraPlacementNotice=TEXT("Use Frame camera to see its handles. Drag an axis, then Apply to save or Cancel to keep the original.");
    return true;
}
bool FStudioModel::EditCameraPlacement(const FStudioCameraState& Camera)
{
    if(!IsCameraPlacementCurrent())
    {CameraPlacementNotice=TEXT("Saved cameras changed. Cancel this placement and select the camera again.");bCameraPlacementError=true;return false;}
    FStudioInspectionState Check;Check.Camera=Camera;
    if(!StudioView::IsValid(Check))
    {CameraPlacementNotice=TEXT("Camera values are outside the supported range. Previous draft kept.");bCameraPlacementError=true;return false;}
    Placement->Camera=Camera;++CameraPlacementRevision;bCameraPlacementError=false;
    CameraPlacementNotice=TEXT("Placement draft changed. Apply saves this pose; the active view is unchanged.");
    return true;
}
bool FStudioModel::PreviewCameraPlacement()
{
    if(!IsCameraPlacementCurrent())
    {CameraPlacementNotice=TEXT("Saved cameras changed. Cancel this placement and select the camera again.");bCameraPlacementError=true;return false;}
    EndViewEdit();
    if(StudioView::CameraEquals(Project.Camera,Placement->Camera))
    {
        bCameraPlacementError=false;
        CameraPlacementNotice=TEXT("Already viewing this draft. Frame camera shows its handles again.");
        return true;
    }
    if(!EditCamera(TEXT("Preview camera placement"),Placement->Camera))
    {CameraPlacementNotice=Notice;bCameraPlacementError=true;return false;}
    bCameraPlacementError=false;
    CameraPlacementNotice=TEXT("Viewing the draft camera. Undo view returns to the previous view; Frame camera shows its handles again.");
    return true;
}
void FStudioModel::SetCameraPlacementTool(EStudioCameraPlacementTool Tool)
{
    if(!IsCameraPlacementCurrent())return;
    Placement->Tool=Tool;++CameraPlacementRevision;
    CameraPlacementNotice=Tool==EStudioCameraPlacementTool::Move?TEXT("Drag X, Y or Z to move along a scene axis."):
        TEXT("Drag a colored ring to rotate around a scene axis.");bCameraPlacementError=false;
}
bool FStudioModel::ApplyCameraPlacement()
{
    if(!IsCameraPlacementCurrent())
    {CameraPlacementNotice=TEXT("Saved cameras changed. Cancel this placement and select the camera again.");bCameraPlacementError=true;return false;}
    auto Cameras=Project.Cameras;
    auto* Saved=Cameras.FindByPredicate([this](const auto& C){return C.Id==Placement->CameraId;});
    Saved->Camera=Placement->Camera;
    const FString Label=TEXT("Placed camera: ")+Saved->Name;
    if(!CommitCameras(Label,MoveTemp(Cameras)))
    {CameraPlacementNotice=CameraCollectionNotice;bCameraPlacementError=true;return false;}
    Placement.Reset();++CameraPlacementRevision;CameraPlacementNotice.Empty();bCameraPlacementError=false;
    return true;
}
void FStudioModel::CancelCameraPlacement()
{
    if(!Placement.IsSet())return;
    Placement.Reset();++CameraPlacementRevision;CameraPlacementNotice.Empty();bCameraPlacementError=false;
    CameraCollectionMessage(TEXT("Camera placement cancelled. Saved pose retained."));
}
