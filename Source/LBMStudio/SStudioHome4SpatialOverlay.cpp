#include "SStudioHome4SpatialOverlay.h"
#include "StudioScene.h"
#include "StudioVolume.h"
#include "StudioCameraPlacement.h"
#include "StudioTheme.h"
#include "Rendering/DrawElements.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
bool StudioHome4SpatialView::Regions(const FStudioHome4SpatialEvidence& Evidence,const FStudioPointStructuredGrid& Grid,
    TArray<FStudioHome4SpatialRegion>& Out,FString& Error)
{
    Out.Reset();FGuid Run;
    if(!FGuid::Parse(Grid.SourceRunId,Run)||Run!=Evidence.RunId)
    {Error=TEXT("Spatial regions need the same original recorded run.");return false;}
    double Scale=1;
    if(Evidence.CoordinateUnit==TEXT("lattice")||Evidence.CoordinateUnit==TEXT("cells")||Evidence.CoordinateUnit==TEXT("lu_length"))
    {
        if(!Grid.Units.DxMeters||*Grid.Units.DxMeters<=0)
        {Error=TEXT("Spatial regions need the original metres-per-cell map.");return false;}
        Scale=*Grid.Units.DxMeters;
    }
    else if(Evidence.CoordinateUnit!=TEXT("physical")&&Evidence.CoordinateUnit!=TEXT("m"))
    {Error=TEXT("Spatial coordinate convention cannot be placed in this scene.");return false;}
    auto Add=[&](FString Name,const FBox& B,FLinearColor Color)
    {
        if(!B.IsValid)return true;
        const FVector A=B.Min*Scale,Z=B.Max*Scale;
        if(A.ContainsNaN()||Z.ContainsNaN()){Out.Reset();Error=TEXT("Spatial coordinates exceed the finite original source map.");return false;}
        Out.Add({MoveTemp(Name),FBox(FVector(A.X,A.Z,A.Y),FVector(Z.X,Z.Z,Z.Y)),Color});return true;
    };
    for(const auto& P:Evidence.Patches)if(const auto B=P.Bounds())
        if(!Add(P.Id+FString::Printf(TEXT(" · level %d"),P.Level),*B,FLinearColor::LerpUsingHSV(StudioUI::Cyan,StudioUI::Amber,FMath::Clamp(P.Level/8.f,0.f,1.f))))return false;
    for(const auto& Z:Evidence.Zones)if(Z.Minimum&&Z.Maximum)
        if(!Add(Z.Id+TEXT(" · ")+Z.Kind,FBox(*Z.Minimum,*Z.Maximum),StudioUI::Amber))return false;
    Error.Empty();return true;
}
void SStudioHome4SpatialOverlay::Construct(const FArguments& Args)
{Scene=Args._Scene;Session=Args._Session;SetVisibility(EVisibility::HitTestInvisible);}
int32 SStudioHome4SpatialOverlay::OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,
    int32 Layer,const FWidgetStyle&,bool) const
{
    if(!Scene.IsValid()||!Session)return Layer;
    const auto Evidence=Session->Evidence();const auto Field=Scene->PresentedField();const auto Volume=Field?Field->VolumeReconstruction():nullptr;
    if(!Evidence||Evidence->AttachedProjectId!=TOptional<FGuid>(Scene->PresentedProjectId())||!Volume||!Volume->OriginalGrid)return Layer;
    TArray<FStudioHome4SpatialRegion> Regions;FString Error;
    if(!StudioHome4SpatialView::Regions(*Evidence,*Volume->OriginalGrid,Regions,Error))return Layer;
    const auto Size=G.GetLocalSize();const auto Capture=Scene->PresentedViewportSize();const double Aspect=Capture.Y>0?double(Capture.X)/Capture.Y:0;
    const auto& Camera=Scene->PresentedCamera();const auto White=FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
    const auto Resource=FSlateApplication::Get().GetRenderer()->GetResourceHandle(*White);
    Out.PushClip(FSlateClippingZone(G));
    for(const auto& Region:Regions)
    {
        FVector Corners[8];FVector2D Projected[8];bool Inside[8];
        for(int32 I=0;I<8;++I)
        {
            Corners[I]=FVector(I&1?Region.Bounds.Max.X:Region.Bounds.Min.X,I&2?Region.Bounds.Max.Y:Region.Bounds.Min.Y,I&4?Region.Bounds.Max.Z:Region.Bounds.Min.Z);
            Inside[I]=StudioCameraPlacement::Project(Camera,Size,Corners[I],Projected[I],Aspect);
        }
        // Faces are transparent annotations. Edges are clipped against all six camera planes.
        const int32 Faces[6][4]={{0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3}};
        for(const auto& Face:Faces)
        {
            bool Visible=true;for(int32 I:Face)Visible&=Inside[I];if(!Visible)continue;
            TArray<FSlateVertex> Vertices;auto Fill=Region.Color;Fill.A=.035f;
            for(int32 I:Face)Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),FVector2f(Projected[I]),FVector2f::ZeroVector,Fill.ToFColor(true)));
            FSlateDrawElement::MakeCustomVerts(Out,Layer+1,Resource,Vertices,TArray<SlateIndex>{0,1,2,0,2,3},nullptr,0,0);
        }
        for(int32 I=0;I<8;++I)for(int32 Axis=0;Axis<3;++Axis)if(!(I&(1<<Axis)))
        {
            FVector2D A,B;if(StudioCameraPlacement::ProjectLine(Camera,Size,Corners[I],Corners[I|(1<<Axis)],A,B,Aspect))
                FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),{A,B},ESlateDrawEffect::None,Region.Color,true,1);
        }
        FVector2D Label;if(StudioCameraPlacement::Project(Camera,Size,Region.Bounds.Max,Label,Aspect))
            FSlateDrawElement::MakeText(Out,Layer+3,G.ToPaintGeometry(FVector2D(220,14),FSlateLayoutTransform(Label)),Region.Name,StudioUI::Font(8),ESlateDrawEffect::None,Region.Color);
    }
    Out.PopClip();return Layer+3;
}
