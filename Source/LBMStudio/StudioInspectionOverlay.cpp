#include "StudioInspectionOverlay.h"
#include "StudioProbeMarkers.h"
#include "StudioStreamlines.h"

StudioInspectionOverlay::FGeometry StudioInspectionOverlay::Build(const FStudioInspectionObjects& Objects,
    const FStudioInspectionSource& Source,const FGuid& Project,const FGuid& Selected,const FBox& Bounds,const FStudioProbeMarkerResult* Markers,
    int32 Dimensions,double SourcePlaneY,const FBox& SeedBounds)
{
    FGeometry Out;
    for(const auto& S:Objects.Slices)if(S.bVisible&&S.Source==Source)
    {
        const auto Polygon=StudioInspectionObjects::SlicePolygon(S,Bounds);
        for(int32 I=0;I<Polygon.Num();++I)Out.Lines.Add({S.Id,Polygon[I],Polygon[(I+1)%Polygon.Num()],1.5f});
        Out.Markers.Add({S.Id,S.Origin,S.Id==Selected?S.Name:FString()});
    }
    for(const auto& P:Objects.Probes)if(P.bVisible&&P.Source==Source)
    {
        FVector At=P.A;
        if(P.Method==EStudioProbeMethod::OriginalPoint)
        {
            const auto Position=Markers?Markers->Position(Project,P):TOptional<FVector>();
            if(!Position.IsSet())continue;
            At=Position.GetValue();
        }
        Out.Markers.Add({P.Id,At,P.Id==Selected?P.Name:FString()});
        if(P.Kind==EStudioProbeKind::Line)
        {
            Out.Lines.Add({P.Id,P.A,P.B});
            Out.Markers.Add({P.Id,P.B,P.Id==Selected?TEXT("B"):TEXT("")});
        }
    }
    for(const auto& R:Objects.Rulers)if(R.bVisible&&R.Source==Source)
    {
        Out.Lines.Add({R.Id,R.A,R.B});
        if(R.Kind==EStudioRulerKind::Angle)Out.Lines.Add({R.Id,R.B,R.C});
        const auto Value=StudioInspectionObjects::Measurement(R);
        const FString Label=R.Id!=Selected?FString():R.Name+(Value.IsSet()?
            FString::Printf(TEXT(" · %.6g %s"),Value.GetValue(),R.Kind==EStudioRulerKind::Angle?TEXT("deg"):*R.Unit):TEXT(" · undefined angle"));
        Out.Markers.Add({R.Id,R.A,FString()});Out.Markers.Add({R.Id,R.B,Label});
        if(R.Kind==EStudioRulerKind::Angle)Out.Markers.Add({R.Id,R.C,FString()});
    }
    for(const auto& S:Objects.Seeds)if(S.bVisible&&S.Source==Source)
    {
        TArray<FVector> Positions;FString Error;
        if(!StudioStreamlines::Seeds(S,SeedBounds.IsValid?SeedBounds:Bounds,Dimensions,SourcePlaneY,Positions,Error))continue;
        if(S.Kind==EStudioSeedKind::Line)Out.Lines.Add({S.Id,S.A,S.B,1.5f});
        if(S.Kind==EStudioSeedKind::Plane)
        {
            const FVector Corners[]={S.A-S.B*.5-S.C*.5,S.A+S.B*.5-S.C*.5,S.A+S.B*.5+S.C*.5,S.A-S.B*.5+S.C*.5};
            for(int32 I=0;I<4;++I)Out.Lines.Add({S.Id,Corners[I],Corners[(I+1)%4],1.5f});
        }
        if(S.Kind==EStudioSeedKind::Inlet&&Dimensions==2&&Positions.Num()>1)
            Out.Lines.Add({S.Id,Positions[0],Positions.Last(),1.5f});
        for(int32 I=0;I<Positions.Num();++I)
            Out.Markers.Add({S.Id,Positions[I],I==0&&S.Id==Selected?S.Name:FString()});
    }
    return Out;
}

bool StudioInspectionOverlay::ProjectMarker(const FStudioCameraState& Camera,FVector2D Size,const FVector& World,FVector2D& Pixel,double ProjectionAspect)
{
    return StudioCameraPlacement::Project(Camera,Size,World,Pixel,ProjectionAspect)&&
        Pixel.X>=6&&Pixel.Y>=6&&Pixel.X<=Size.X-6&&Pixel.Y<=Size.Y-6;
}

FGuid StudioInspectionOverlay::Pick(const FGeometry& Geometry,const FStudioCameraState& Camera,FVector2D Size,
    FVector2D Pixel,const FGuid& Selected,double ProjectionAspect)
{
    if(Pixel.ContainsNaN()||Pixel.X<0||Pixel.Y<0||Pixel.X>Size.X||Pixel.Y>Size.Y)return {};
    FGuid Best;double BestDistanceSquared=49.;
    auto Consider=[&](const FGuid& Id,double DistanceSquared)
    {
        if(DistanceSquared>49.)return;
        // Stable paint order resolves equal distances unless the selected
        // object's handle already occupies the same point.
        if(!Best.IsValid()||DistanceSquared<BestDistanceSquared-1.e-6||
            (FMath::IsNearlyEqual(DistanceSquared,BestDistanceSquared,1.e-6)&&(Best!=Selected||Id==Selected)))
        {Best=Id;BestDistanceSquared=DistanceSquared;}
    };
    for(const auto& Line:Geometry.Lines)
    {
        FVector2D A,B;if(!StudioCameraPlacement::ProjectLine(Camera,Size,Line.A,Line.B,A,B,ProjectionAspect))continue;
        const FVector2D Delta=B-A;const double Length=Delta.SizeSquared();
        const double T=Length>0?FMath::Clamp(FVector2D::DotProduct(Pixel-A,Delta)/Length,0.,1.):0;
        Consider(Line.Object,(Pixel-(A+Delta*T)).SizeSquared());
    }
    for(const auto& Marker:Geometry.Markers)
    {
        FVector2D At;if(ProjectMarker(Camera,Size,Marker.Position,At,ProjectionAspect))
            Consider(Marker.Object,(Pixel-At).SizeSquared());
    }
    return Best;
}
