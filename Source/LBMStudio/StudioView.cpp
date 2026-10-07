#include "StudioView.h"
#include "Math/OrthoMatrix.h"
#include "Math/PerspectiveMatrix.h"

bool StudioView::CameraEquals(const FStudioCameraState& A, const FStudioCameraState& B)
{
    return A.Position.Equals(B.Position,1.e-8) && A.Focus.Equals(B.Focus,1.e-8) &&
        A.Orientation.Equals(B.Orientation,1.e-8) && FMath::IsNearlyEqual(A.OrbitDistance,B.OrbitDistance,1.e-8) &&
        FMath::IsNearlyEqual(A.FieldOfView,B.FieldOfView,1.e-5) && FMath::IsNearlyEqual(A.OrthoWidth,B.OrthoWidth,1.e-7) &&
        A.bOrthographic==B.bOrthographic && A.bFreeCamera==B.bFreeCamera &&
        A.bDepthClipping==B.bDepthClipping && A.NearClipMeters==B.NearClipMeters && A.FarClipMeters==B.FarClipMeters;
}
bool StudioView::IsValidClipping(const FStudioCameraState& C)
{
    return FMath::IsFinite(C.NearClipMeters)&&FMath::IsFinite(C.FarClipMeters)&&
        C.NearClipMeters>=1.e-6&&C.FarClipMeters<=1.e8&&
        C.FarClipMeters-C.NearClipMeters>=FMath::Max(1.e-6,C.NearClipMeters*1.e-6);
}
bool StudioView::BuildClippedProjection(const FStudioCameraState& C,FIntPoint Size,FMatrix& Out)
{
    if(!IsValidClipping(C)||Size.X<=0||Size.Y<=0||!FMath::IsFinite(C.FieldOfView)||
        C.FieldOfView<5||C.FieldOfView>160||!FMath::IsFinite(C.OrthoWidth)||C.OrthoWidth<.001||C.OrthoWidth>1.e6)return false;
    const double Aspect=double(Size.X)/Size.Y,Near=C.NearClipMeters*100.,Far=C.FarClipMeters*100.;
    if(C.bOrthographic)
    {
        const double HalfWidth=C.OrthoWidth*50.;
        Out=FReversedZOrthoMatrix(HalfWidth,HalfWidth/Aspect,1./(Far-Near),-Near);
    }
    else
    {
        const double HalfFOV=FMath::DegreesToRadians(C.FieldOfView*.5);
        Out=FReversedZPerspectiveMatrix(HalfFOV,HalfFOV,1.,Aspect,Near,Far);
    }
    return true;
}
static bool FieldDisplayEquals(const FStudioViewSettings& A, const FStudioViewSettings& B)
{
    return A.bStreamlines==B.bStreamlines && A.bVectors==B.bVectors && A.bCutPlane==B.bCutPlane &&
        A.bVolume==B.bVolume && A.bHome4AirMask==B.bHome4AirMask && A.bMesh==B.bMesh && A.MeshStyle==B.MeshStyle && A.SliceAxis==B.SliceAxis && A.SlicePosition==B.SlicePosition &&
        A.StreamlineDensity==B.StreamlineDensity && A.StreamlineSettings==B.StreamlineSettings && A.VectorScale==B.VectorScale &&
        A.VectorCount==B.VectorCount && A.bUniformVectors==B.bUniformVectors && A.VolumeOpacity==B.VolumeOpacity &&
        A.ScalarField==B.ScalarField && A.bSourcePoints==B.bSourcePoints && A.bReconstructedSurface==B.bReconstructedSurface && A.bFocusWingRegion==B.bFocusWingRegion &&
        A.PointSize==B.PointSize && A.ScalarStyles==B.ScalarStyles &&
        A.VolumeClipMinimum==B.VolumeClipMinimum && A.VolumeClipMaximum==B.VolumeClipMaximum &&
        A.VolumeOpacityCurve==B.VolumeOpacityCurve && A.VolumeStepVoxels==B.VolumeStepVoxels &&
        A.bVolumeThreshold==B.bVolumeThreshold && A.VolumeThresholdMinimum==B.VolumeThresholdMinimum &&
        A.VolumeThresholdMaximum==B.VolumeThresholdMaximum && A.bVolumeIsosurface==B.bVolumeIsosurface && A.VolumeIsovalue==B.VolumeIsovalue;
}
bool StudioView::DisplayEquals(const FStudioViewSettings& A,const FStudioViewSettings& B)
{return FieldDisplayEquals(A,B)&&A.InspectionObjects==B.InspectionObjects;}
bool StudioView::RenderEquals(const FStudioViewSettings& A,const FStudioViewSettings& B)
{
    if(!FieldDisplayEquals(A,B))return false;
    const auto& ASeeds=A.InspectionObjects.Seeds;const auto& BSeeds=B.InspectionObjects.Seeds;
    int32 SI=0,SJ=0;
    while(true)
    {
        while(SI<ASeeds.Num()&&!ASeeds[SI].bVisible)++SI;
        while(SJ<BSeeds.Num()&&!BSeeds[SJ].bVisible)++SJ;
        if(SI==ASeeds.Num()||SJ==BSeeds.Num()){if(SI!=ASeeds.Num()||SJ!=BSeeds.Num())return false;break;}
        auto X=ASeeds[SI++],Y=BSeeds[SJ++];
        // Names affect the inspector, not the generated trajectories.
        X.Name=Y.Name;
        if(!(X==Y))return false;
    }
    const auto& AS=A.InspectionObjects.Slices;const auto& BS=B.InspectionObjects.Slices;
    int32 I=0,J=0;
    while(true)
    {
        while(I<AS.Num()&&!AS[I].bVisible)++I;
        while(J<BS.Num()&&!BS[J].bVisible)++J;
        if(I==AS.Num()||J==BS.Num())return I==AS.Num()&&J==BS.Num();
        const auto& X=AS[I++];const auto& Y=BS[J++];
        if(!(X.Source==Y.Source)||X.Origin!=Y.Origin||X.Normal!=Y.Normal||X.Opacity!=Y.Opacity)return false;
    }
}
bool FStudioInspectionState::Equals(const FStudioInspectionState& Other) const
{ return StudioView::CameraEquals(Camera,Other.Camera) && StudioView::DisplayEquals(Display,Other.Display); }
bool StudioView::IsValid(const FStudioInspectionState& S)
{
    auto Range=[](double V,double Min,double Max) { return FMath::IsFinite(V)&&V>=Min&&V<=Max; };
    auto Point=[&](const FVector& P) { return Range(P.X,-1.e8,1.e8)&&Range(P.Y,-1.e8,1.e8)&&Range(P.Z,-1.e8,1.e8); };
    const auto& C=S.Camera; const auto& V=S.Display;
    FString InspectionError;if(!StudioInspectionObjects::IsValid(V.InspectionObjects,InspectionError))return false;
    if(!StudioStreamlines::IsValid(V.StreamlineSettings))return false;
    if(V.ScalarField.Len()>128)return false;
    for(TCHAR Ch:V.ScalarField)if(!FChar::IsAlnum(Ch)&&Ch!='_'&&Ch!='-'&&Ch!='.')return false;
    for(int32 A=0;A<3;++A)if(!Range(V.VolumeClipMinimum[A],0,1)||!Range(V.VolumeClipMaximum[A],0,1)||
        V.VolumeClipMinimum[A]>=V.VolumeClipMaximum[A]||!Range(V.VolumeOpacityCurve[A],0,1))return false;
    if(!Range(V.VolumeIsovalue,-1.e20,1.e20)||!Range(V.VolumeStepVoxels,.25,4)||!Range(V.VolumeThresholdMinimum,-1.e20,1.e20)||!Range(V.VolumeThresholdMaximum,-1.e20,1.e20)||
        V.VolumeThresholdMinimum>V.VolumeThresholdMaximum)return false;
    return Point(C.Position) && Point(C.Focus) && !C.Orientation.ContainsNaN() &&
        FMath::IsNearlyEqual(C.Orientation.SizeSquared(),1.,1.e-5) && Range(C.OrbitDistance,.0001,1.e8) &&
        Range(C.FieldOfView,5,160) && Range(C.OrthoWidth,.001,1.e6) && IsValidClipping(C) && V.SliceAxis>=0 && V.SliceAxis<=2 &&
        V.MeshStyle>=0 && V.MeshStyle<=2 && Range(V.SlicePosition,-1.e9,1.e9) && Range(V.StreamlineDensity,0,1) &&
        Range(V.VectorScale,.2,3) && V.VectorCount>=1 && V.VectorCount<=4096 &&
        Range(V.VolumeOpacity,0,1) && Range(V.PointSize,.25,3) && StudioColor::IsValid(V.ScalarStyles);
}
FQuat StudioView::Turn(const FQuat& Q,double DX,double DY,double Scale)
{
    if(!FMath::IsFinite(DX)||!FMath::IsFinite(DY)) return Q;
    return (Q*FQuat(FVector::UpVector,FMath::DegreesToRadians(DX*Scale))*
        FQuat(FVector::RightVector,FMath::DegreesToRadians(DY*Scale))).GetNormalized();
}
FStudioCameraState StudioView::FitBounds(FStudioCameraState C,const FBox& B,double Aspect,double NearPlane)
{
    if(!B.IsValid||!FMath::IsFinite(Aspect)||Aspect<=0||!FMath::IsFinite(NearPlane)||NearPlane<0||
        (C.bDepthClipping&&!IsValidClipping(C)))return C;
    C.Focus=B.GetCenter();
    const FVector Forward=C.Orientation.GetForwardVector(),Right=C.Orientation.GetRightVector(),Up=C.Orientation.GetUpVector();
    const double TanH=FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5)),TanV=TanH/Aspect;
    double Distance=.001,Width=.001;
    // A corner on the optical axis has no projected width/height. Frustum
    // coverage alone can place it at depth zero at wide FOV. Preserve positive
    // clearance in meters, including very small externally supplied bounds.
    if(C.bDepthClipping)NearPlane=C.NearClipMeters;
    const double Margin=FMath::Max3(1.e-8,NearPlane*.01,B.GetSize().Size()*1.e-6);
    const double Clearance=NearPlane+Margin;
    double FurthestDepth=0;
    for(int32 I=0;I<8;++I)
    {
        const FVector P=FVector(I&1?B.Max.X:B.Min.X,I&2?B.Max.Y:B.Min.Y,I&4?B.Max.Z:B.Min.Z)-C.Focus;
        const double Depth=FVector::DotProduct(P,Forward),X=FMath::Abs(FVector::DotProduct(P,Right)),Y=FMath::Abs(FVector::DotProduct(P,Up));
        Distance=FMath::Max(Distance,FMath::Max(X*1.1/TanH,Y*1.1/TanV)-Depth);
        Distance=FMath::Max(Distance,Clearance-Depth);
        Width=FMath::Max(Width,2.2*FMath::Max(X,Y*Aspect));
        FurthestDepth=FMath::Max(FurthestDepth,Depth);
    }
    C.OrbitDistance=Distance;C.Position=C.Focus-Forward*Distance;
    if(C.bOrthographic)C.OrthoWidth=Width;
    // Fit includes the complete domain. Extending the far plane participates in
    // the same view-history edit; it never edits the computational bounds.
    if(C.bDepthClipping)C.FarClipMeters=FMath::Max(C.FarClipMeters,Distance+FurthestDepth+Margin);
    return C;
}
void FStudioViewHistory::Push(FEntry Entry)
{
    if(Entry.Before.Equals(Entry.After)) return;
    Redo.Reset();const int64 Added=EntryBytes(Entry);int64 Total=Added;
    for(const auto& Existing:Undo)Total+=EntryBytes(Existing);
    while(!Undo.IsEmpty()&&(Undo.Num()>=MaxEntries||Total>MaxStoredBytes))
    {Total-=EntryBytes(Undo[0]);Undo.RemoveAt(0);}
    if(Added<=MaxStoredBytes)Undo.Add(MoveTemp(Entry));
}
int64 FStudioViewHistory::EntryBytes(const FEntry& Entry)
{
    auto StateBytes=[](const FStudioInspectionState& State)
    {
        const auto& D=State.Display;const auto& O=D.InspectionObjects;
        int64 Bytes=D.ScalarField.GetAllocatedSize()+D.ScalarStyles.GetAllocatedSize()+
            O.Slices.GetAllocatedSize()+O.Probes.GetAllocatedSize()+O.Rulers.GetAllocatedSize()+O.Seeds.GetAllocatedSize()+D.StreamlineSettings.VelocityField.GetAllocatedSize();
        for(const auto& Style:D.ScalarStyles)Bytes+=Style.Dataset.GetAllocatedSize()+Style.Field.GetAllocatedSize();
        auto Common=[](const FStudioInspectionObject& Item)
        {return Item.Name.GetAllocatedSize()+Item.Source.Dataset.GetAllocatedSize()+Item.Source.MetadataSHA256.GetAllocatedSize()+Item.Source.PayloadSHA256.GetAllocatedSize();};
        for(const auto& S:O.Slices)Bytes+=Common(S);
        for(const auto& P:O.Probes)Bytes+=Common(P)+P.Field.GetAllocatedSize();
        for(const auto& R:O.Rulers)Bytes+=Common(R)+R.Unit.GetAllocatedSize();
        for(const auto& Seed:O.Seeds)Bytes+=Common(Seed)+Seed.Points.GetAllocatedSize();
        return Bytes;
    };
    return sizeof(FEntry)+Entry.Label.GetAllocatedSize()+StateBytes(Entry.Before)+StateBytes(Entry.After);
}
int64 FStudioViewHistory::StoredBytes() const
{
    int64 Bytes=0;for(const auto& E:Undo)Bytes+=EntryBytes(E);for(const auto& E:Redo)Bytes+=EntryBytes(E);return Bytes;
}
void FStudioViewHistory::Begin(const FString& Label,const FStudioInspectionState& Current)
{ End(); Gesture=FEntry{Label.Left(120),Current,Current}; }
void FStudioViewHistory::Record(const FString& Label,const FStudioInspectionState& Before,const FStudioInspectionState& After)
{
    if(Before.Equals(After)) return;
    if(Gesture.IsSet()) Gesture->After=After;
    else Push({Label.Left(120),Before,After});
}
void FStudioViewHistory::End()
{ if(Gesture.IsSet()) { auto Entry=MoveTemp(Gesture.GetValue()); Gesture.Reset(); Push(MoveTemp(Entry)); } }
void FStudioViewHistory::Clear() { Undo.Reset(); Redo.Reset(); Gesture.Reset(); }
bool FStudioViewHistory::CanUndo() const
{ return (Gesture.IsSet()&&!Gesture->Before.Equals(Gesture->After)) || !Undo.IsEmpty(); }
bool FStudioViewHistory::CanRedo() const
{ return (!Gesture.IsSet()||Gesture->Before.Equals(Gesture->After)) && !Redo.IsEmpty(); }
FString FStudioViewHistory::UndoLabel() const
{ return Gesture.IsSet()&&!Gesture->Before.Equals(Gesture->After)?Gesture->Label:Undo.IsEmpty()?FString():Undo.Last().Label; }
FString FStudioViewHistory::RedoLabel() const { return CanRedo()?Redo.Last().Label:FString(); }
bool FStudioViewHistory::Restore(bool bRedo,const FStudioInspectionState& Current,FStudioInspectionState& Out,FString& Label)
{
    End(); auto& From=bRedo?Redo:Undo; auto& To=bRedo?Undo:Redo;
    if(From.IsEmpty()) return false;
    const auto& Entry=From.Last();
    if(!(bRedo?Entry.Before:Entry.After).Equals(Current)) return false;
    Out=bRedo?Entry.After:Entry.Before; Label=Entry.Label;
    To.Add(From.Pop()); return true;
}
