#include "StudioInspectionPlacement.h"

TOptional<FStudioInspectionPlacement> FStudioInspectionPlacement::Begin(const FGuid& Project,const FGuid& Object,
    const FStudioInspectionSource& Source,int32 Revision,FVector Anchor,int32 Points,
    int32 Dimensions,FVector SourceOffset,const FStudioCameraState& Observer)
{
    if(!Project.IsValid()||!Object.IsValid()||!StudioInspectionObjects::IsValid(Source)||
        Anchor.ContainsNaN()||SourceOffset.ContainsNaN()||Points<1||Points>3||(Dimensions!=2&&Dimensions!=3)||
        Observer.Orientation.ContainsNaN()||!Observer.Orientation.IsNormalized())return {};
    FStudioInspectionPlacement Out;Out.ProjectId=Project;Out.ObjectId=Object;Out.Source=Source;
    Out.ObjectsRevision=Revision;Out.RequiredPoints=Points;Out.SpatialDimensions=Dimensions;Out.PlaneOrigin=Anchor;
    Out.PlaneNormal=Dimensions==2?FVector::RightVector:Observer.Orientation.GetForwardVector();
    if(Dimensions==2)Out.PlaneOrigin.Y=SourceOffset.Y;
    Out.Accepted.Reserve(Points);return Out;
}

bool FStudioInspectionPlacement::IsCurrent(const FGuid& Project,const FGuid& Selection,
    const FStudioInspectionSource& CurrentSource,int32 Revision) const
{return ProjectId==Project&&ObjectId==Selection&&Source==CurrentSource&&ObjectsRevision==Revision;}

bool FStudioInspectionPlacement::UpdatePreview(const FStudioCameraState& Observer,FVector2D Size,
    const TOptional<FVector2D>& Pixel,double ProjectionAspect)
{
    TOptional<FVector> Next;
    if(!IsComplete()&&Pixel.IsSet()&&Pixel->X>=0&&Pixel->Y>=0&&Pixel->X<=Size.X&&Pixel->Y<=Size.Y)
    {
        StudioCameraPlacement::FRay Ray;
        if(StudioCameraPlacement::Ray(Observer,Size,*Pixel,Ray,ProjectionAspect))
        {
            FStudioSliceObject Plane;Plane.Origin=PlaneOrigin;Plane.Normal=PlaneNormal;FVector Position;
            if(StudioInspectionObjects::IntersectSlice(Plane,Ray.Origin,Ray.Direction,Position))
            {
                if(SpatialDimensions==2)Position.Y=PlaneOrigin.Y;
                // A clipped plane is not a visible placement target. Use the
                // same near/far convention as viewport overlay projection.
                FVector2D Visible;
                if(StudioCameraPlacement::Project(Observer,Size,Position,Visible,ProjectionAspect))Next=Position;
            }
        }
    }
    const bool Changed=Preview.IsSet()!=Next.IsSet()||(Preview.IsSet()&&Next.IsSet()&&Preview.GetValue()!=Next.GetValue());
    Preview=Next;return Changed;
}

bool FStudioInspectionPlacement::AcceptPreview()
{
    if(!Preview.IsSet()||IsComplete())return false;
    Accepted.Add(Preview.GetValue());Preview.Reset();return true;
}
