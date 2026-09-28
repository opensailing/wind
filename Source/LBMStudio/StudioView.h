#pragma once
#include "StudioProject.h"

/** Inspection history deliberately excludes playback, case data and bookmarks. */
struct FStudioInspectionState
{
    FStudioCameraState Camera;
    FStudioViewSettings Display;
    bool Equals(const FStudioInspectionState& Other) const;
};

namespace StudioView
{
    bool CameraEquals(const FStudioCameraState& A, const FStudioCameraState& B);
    bool DisplayEquals(const FStudioViewSettings& A, const FStudioViewSettings& B);
    /** Labels, point probes and rulers update overlays without rebuilding CFD. */
    bool RenderEquals(const FStudioViewSettings& A, const FStudioViewSettings& B);
    bool IsValid(const FStudioInspectionState& State);
    bool IsValidClipping(const FStudioCameraState& Camera);
    /** Camera-space reversed-Z projection; source distances are meters, UE uses centimeters. */
    bool BuildClippedProjection(const FStudioCameraState& Camera,FIntPoint Viewport,FMatrix& Out);
    /** Continuous camera-local rotation, including across the poles. */
    FQuat Turn(const FQuat& Orientation, double DX, double DY, double DegreesPerPixel);
    /** Fits all corners beyond the renderer near plane, retaining orientation/projection. */
    FStudioCameraState FitBounds(FStudioCameraState Camera,const FBox& Bounds,double AspectRatio,double NearPlaneMeters = 0);
}

/** Bounded value history. An input gesture produces at most one entry. */
class FStudioViewHistory
{
public:
    static constexpr int32 MaxEntries = 64;
    static constexpr int64 MaxStoredBytes = 16LL*1024*1024;
    int64 StoredBytes() const;
    void Begin(const FString& Label, const FStudioInspectionState& Current);
    void Record(const FString& Label, const FStudioInspectionState& Before, const FStudioInspectionState& After);
    void End();
    void Clear();
    bool CanUndo() const;
    bool CanRedo() const;
    bool IsEditing() const { return Gesture.IsSet(); }
    FString UndoLabel() const;
    FString RedoLabel() const;
    bool Restore(bool bRedo, const FStudioInspectionState& Current, FStudioInspectionState& Out, FString& Label);
private:
    struct FEntry { FString Label; FStudioInspectionState Before, After; };
    TArray<FEntry> Undo, Redo;
    TOptional<FEntry> Gesture;
    void Push(FEntry Entry);
    static int64 EntryBytes(const FEntry& Entry);
};
