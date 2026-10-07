#include "SStudioSnapshotOverlay.h"
#include "StudioHome4Readouts.h"
#include "Brushes/SlateColorBrush.h"
#include "Rendering/DrawElements.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Styling/CoreStyle.h"

int32 SStudioSnapshotOverlay::OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool) const
{
    const auto& S=Snapshot;const FVector2D Size=G.GetLocalSize(),SourceSize(S.SourceSize);
    const double Scale=FMath::Clamp(FMath::Min(Size.X/800.,Size.Y/500.),.25,4.);
    const FLinearColor Panel=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("101E2B")));
    const FLinearColor Text=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("E0E9F3")));
    const FLinearColor Muted=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("99ACBF")));
    const FLinearColor Cyan=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("00C8EC")));
    const FLinearColor Amber=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("F5BD59")));
    static const FSlateColorBrush White(FLinearColor::White);
    const auto Font=FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),FMath::Max(6,FMath::RoundToInt(10*Scale)));
    const auto Measure=FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
    TArray<FSlateRect> Reserved;
    auto Box=[&](FVector2D At,FVector2D Extent,FLinearColor Color,int32 L)
    {FSlateDrawElement::MakeBox(Out,L,G.ToPaintGeometry(Extent,FSlateLayoutTransform(At)),&White,ESlateDrawEffect::None,Color);};
    auto Label=[&](const FString& Value,FVector2D At,FLinearColor Color,int32 L)
    {FSlateDrawElement::MakeText(Out,L,G.ToPaintGeometry(FVector2D(1,1),FSlateLayoutTransform(At)),Value,Font,ESlateDrawEffect::None,Color);};
    // Interpretation belongs to the frozen rendered traces, including exports
    // whose scalar legend or source/frame label has been switched off.
    const bool HasStreamAnnotation=S.Options.bAnnotations&&S.Streams.Segments>0;
    const bool HasMeshAnnotation=S.Options.bAnnotations&&S.Mesh.Triangles>0;
    const bool HasFocusAnnotation=S.Options.bAnnotations&&S.DisplaySettings.bFocusWingRegion&&S.DisplaySettings.bReconstructedSurface&&
        S.DisplaySettings.MeshStyle==0&&S.Identity.Interpolation==EStudioFieldInterpolation::ReconstructedTriangles;
    const int32 FooterRows=int32(S.Options.bFrameInfo)+int32(HasStreamAnnotation)+int32(HasMeshAnnotation)+
        int32(HasStreamAnnotation&&S.Streams.bBudgetExhausted)+int32(HasFocusAnnotation);
    const double Margin=12*Scale,Footer=FooterRows?(12+20*FooterRows)*Scale:0;
    if(FooterRows)
    {
        Box(FVector2D(0,Size.Y-Footer),FVector2D(Size.X,Footer),Panel,Layer+6);
        double Y=Size.Y-Footer+7*Scale;
        if(S.Options.bFrameInfo)
        {
            const FString Tail=S.SourceUnitMap.IsSet()?TEXT(" · ")+StudioHome4Readouts::Time(S.Identity.Frame.Index,&S.SourceUnitMap.GetValue(),S.Identity.Frame.Time):FString::Printf(TEXT(" · frame %d · %.9g s"),S.Identity.Frame.Index,S.Identity.Frame.Time);
            FString Title=S.SourceTitle;
            while(Title.Len()>1&&Measure->Measure(Title+Tail,Font).X>Size.X-2*Margin)Title.LeftChopInline(1);
            Label(Title+Tail,FVector2D(Margin,Y),Text,Layer+7);Y+=20*Scale;
        }
        if(HasMeshAnnotation)
        {
            Label(FString::Printf(TEXT("%s · %s triangles · edges show topology%s"),
                S.Mesh.bDerived?TEXT("Derived mesh"):TEXT("Original CFD mesh"),*FText::AsNumber(S.Mesh.Triangles).ToString(),
                S.Mesh.bFieldFillHidden?TEXT(" · field fill hidden"):TEXT("")),
                FVector2D(Margin,Y),Muted,Layer+7);Y+=20*Scale;
        }
        if(HasFocusAnnotation)
        {
            Label(TEXT("Focused 2D region · inferred wing extrusion · no spanwise flow"),FVector2D(Margin,Y),Muted,Layer+7);Y+=20*Scale;
        }
        if(HasStreamAnnotation)
        {
            FString Meaning=TEXT("Instantaneous velocity streamlines");
            if(S.Identity.Interpolation==EStudioFieldInterpolation::ReconstructedGrid)
                Meaning+=S.SourceUnitMap.IsSet()?TEXT(" · original 3D grid interpolation"):TEXT(" · derived 3D grid interpolation");
            else if(S.Identity.Interpolation==EStudioFieldInterpolation::ReconstructedTriangles)
                Meaning+=TEXT(" · derived 2D triangle interpolation");
            Label(Meaning,FVector2D(Margin,Y),Muted,Layer+7);Y+=20*Scale;
            if(S.Streams.bBudgetExhausted)
                Label(TEXT("Work limit reached · tracing incomplete"),FVector2D(Margin,Y),Amber,Layer+7);
        }
        Reserved.Add(FSlateRect(0,Size.Y-Footer,Size.X,Size.Y));
    }
    if(S.Options.bLegend)
    {
        const FString Title=S.Scalar.Label;
        const bool HasVectors=S.Vectors.GlyphCount>0;
        const FString VectorLength=FString::Printf(TEXT("Arrow length: %.4g m"),S.Vectors.ReferenceLengthMeters);
        const FString VectorMeaning=S.Vectors.bUniformLength?TEXT("Equal length · direction only"):
            FString::Printf(TEXT("= %.4g m/s (sample max)"),S.Vectors.MaximumSpeed);
        double TextWidth=Measure->Measure(Title,Font).X;
        if(HasVectors)TextWidth=FMath::Max3(TextWidth,double(Measure->Measure(VectorLength,Font).X),double(Measure->Measure(VectorMeaning,Font).X));
        const double W=FMath::Min(Size.X-2*Margin,FMath::Max(170*Scale,TextWidth+20*Scale)),H=(HasVectors?232:190)*Scale;
        const FVector2D At(Margin,FMath::Max(Margin,Size.Y-Footer-H-Margin));
        Box(At,FVector2D(W,H),Panel,Layer+6);Label(Title,At+FVector2D(10,8)*Scale,Text,Layer+7);
        const FVector2D Bar=At+FVector2D(10,34)*Scale;
        for(int32 I=0;I<64;++I)
            Box(Bar+FVector2D(0,I*2*Scale),FVector2D(14*Scale,2*Scale+.25),
                StudioColor::Map(FMath::Lerp(S.Mapping.Maximum,S.Mapping.Minimum,I/63.),S.Mapping),Layer+7);
        for(int32 I=0;I<5;++I)Label(StudioHome4Readouts::Scalar(FMath::Lerp(S.Mapping.Maximum,S.Mapping.Minimum,I/4.),S.Scalar.Unit,S.UnitDisplay,S.SourceUnitMap.IsSet()?&S.SourceUnitMap.GetValue():nullptr),
            Bar+FVector2D(23,I*30-3)*Scale,Text,Layer+7);
        Label(S.Mapping.bManualRange?TEXT("Custom range"):S.Scalar.DefaultDisplayMaximum.IsSet()?TEXT("First-frame 99% range"):TEXT("Source range"),At+FVector2D(10,168)*Scale,Muted,Layer+7);
        if(HasVectors)
        {
            Label(VectorLength,At+FVector2D(10,190)*Scale,Text,Layer+7);
            Label(VectorMeaning,At+FVector2D(10,208)*Scale,Muted,Layer+7);
        }
        Reserved.Add(FSlateRect(At.X-4*Scale,At.Y-4*Scale,At.X+W+4*Scale,At.Y+H+4*Scale));
    }
    if(!S.Options.bAnnotations)return Layer+7;
    const double Aspect=double(S.SourceSize.X)/S.SourceSize.Y;
    auto Project=[&](const FVector& World,FVector2D& Pixel)
    {
        FVector2D SourcePixel;if(!StudioCameraPlacement::Project(S.Camera,SourceSize,World,SourcePixel,Aspect))return false;
        Pixel=S.Framing.ToOutput(SourcePixel,SourceSize,Size);
        return Pixel.X>=6*Scale&&Pixel.Y>=6*Scale&&Pixel.X<=Size.X-6*Scale&&Pixel.Y<=Size.Y-6*Scale;
    };
    auto Color=[&](FGuid Id){return Id==S.SelectedObject?Cyan:Muted;};
    // Clip annotation lines to the final crop in addition to the source camera.
    auto Clip=[&](FVector2D& A,FVector2D& B)
    {
        const FVector2D D=B-A;double Lo=0,Hi=1;
        auto Plane=[&](double FA,double FB)
        {if(FA<0&&FB<0)return false;if(FA<0)Lo=FMath::Max(Lo,FA/(FA-FB));else if(FB<0)Hi=FMath::Min(Hi,FA/(FA-FB));return Lo<=Hi;};
        if(!Plane(A.X,B.X)||!Plane(Size.X-A.X,Size.X-B.X)||!Plane(A.Y,B.Y)||!Plane(Size.Y-A.Y,Size.Y-B.Y))return false;
        B=A+D*Hi;A+=D*Lo;return true;
    };
    for(const auto& Edge:S.Overlay.Lines)
    {
        FVector2D A,B;if(!StudioCameraPlacement::ProjectLine(S.Camera,SourceSize,Edge.A,Edge.B,A,B,Aspect))continue;
        A=S.Framing.ToOutput(A,SourceSize,Size);B=S.Framing.ToOutput(B,SourceSize,Size);if(!Clip(A,B))continue;
        FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),TArray<FVector2D>{A,B},ESlateDrawEffect::None,Color(Edge.Object),true,Edge.Width*Scale);
    }
    for(const auto& Point:S.Overlay.Markers)
    {
        FVector2D P;if(!Project(Point.Position,P))continue;
        for(const FVector2D D:{FVector2D(5*Scale,0),FVector2D(0,5*Scale)})
            FSlateDrawElement::MakeLines(Out,Layer+3,G.ToPaintGeometry(),TArray<FVector2D>{P-D,P+D},ESlateDrawEffect::None,Color(Point.Object),true,2*Scale);
        if(Point.Label.IsEmpty())continue;
        const FVector2D Extent=FVector2D(Measure->Measure(Point.Label,Font))+FVector2D(8,4)*Scale;
        for(const FVector2D Offset:{FVector2D(9,-10)*Scale,FVector2D(9,8)*Scale,FVector2D(-Extent.X-9*Scale,8*Scale),FVector2D(-Extent.X-9*Scale,-10*Scale)})
        {
            const FVector2D At(FMath::Clamp(P.X+Offset.X,Margin,FMath::Max(Margin,Size.X-Extent.X-Margin)),
                FMath::Clamp(P.Y+Offset.Y,Margin,FMath::Max(Margin,Size.Y-Extent.Y-Margin)));
            const FSlateRect Rect(At.X,At.Y,At.X+Extent.X,At.Y+Extent.Y);
            if(Reserved.ContainsByPredicate([&](const auto& R){return Rect.Left<R.Right&&Rect.Right>R.Left&&Rect.Top<R.Bottom&&Rect.Bottom>R.Top;}))continue;
            Box(At,Extent,Panel,Layer+3);Label(Point.Label,At+FVector2D(4,2)*Scale,Color(Point.Object),Layer+4);Reserved.Add(Rect);break;
        }
    }
    return Layer+7;
}
