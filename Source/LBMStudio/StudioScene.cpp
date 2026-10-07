#include "StudioScene.h"
#include "StudioFlowPresentation.h"
#include "StudioWorkspace.h"
#include "StudioPointRecording.h"
#include "StudioOrientation.h"
#include "StudioSurfaceRenderData.h"
#include "StudioFieldDisplay.h"
#include "StudioSliceRendering.h"
#include "StudioVolume.h"
#include "StudioVolumeComponent.h"
#include "StudioSnapshot.h"
#include "StudioSnapshotSource.h"
#include "StudioPipelineRenderData.h"
#include "SStudioSnapshotOverlay.h"
#include "Slate/WidgetRenderer.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "Math/PerspectiveMatrix.h"
#include "Math/OrthoMatrix.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Async/Async.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Widgets/SWindow.h"

struct FStudioSection
{
    TArray<FVector> Vertices;
    TArray<int32> Indices;
    TArray<FLinearColor> Colors;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs,VelocityUVs;
    void Triangle(const FVector& A,const FVector& B,const FVector& C,const FLinearColor& Color)
    {
        const int32 N=Vertices.Num(); Vertices.Append({A,B,C}); Colors.Append({Color,Color,Color}); Indices.Append({N,N+1,N+2});
    }
    void Tube(const FVector& A,const FVector& B,double Radius,const FLinearColor& Color,const FVector* KnownDirection=nullptr)
    {
        const FVector D=KnownDirection?*KnownDirection:(B-A).GetSafeNormal();
        if(D.IsNearlyZero()) return;
        const FVector N=FVector::CrossProduct(D,FMath::Abs(D.Z)<0.9?FVector::UpVector:FVector::RightVector).GetSafeNormal()*Radius;
        const FVector T=FVector::CrossProduct(D,N);
        constexpr int32 Sides=4;
        for(int32 I=0;I<Sides;++I)
        {
            const double Angle=2.*PI*I/Sides,Next=2.*PI*(I+1)/Sides;
            const FVector P=N*FMath::Cos(Angle)+T*FMath::Sin(Angle),Q=N*FMath::Cos(Next)+T*FMath::Sin(Next);
            Triangle(A+P,B+P,B+Q,Color); Triangle(A+P,B+Q,A+Q,Color);
        }
    }
};
struct FStudioGeometry
{
    FStudioSection Sections[13];
    bool bOriginalSlice=false;
    bool bFocusedSurface=false;
    bool bAirfoilSolid=false;
    TMap<FGuid,FString> SliceNotices;
    TSet<FGuid> RenderedSlices;
    TArray<float> SurfaceScalars;
    TOptional<FStudioVolumeRenderData> VolumeData;
    FStudioViewSettings VolumeSettings;
    FIntPoint ScalarTextureSize=FIntPoint::ZeroValue;
    double BuildMs=0;
    FString Error,Dataset,Title;
    FStudioFrame Frame;
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Field;
    FStudioScalarDescriptor Scalar;
    FStudioColorMapping ColorMapping;
    FStudioVectorSummary Vectors;
    FStudioStreamlineSummary Streams;
    FStudioMeshSummary Mesh;
    FBox Bounds=FBox(ForceInit);
};

struct FRenderRequest
{
    double Time,SlicePosition,Density,VectorScale,Opacity;
    int32 SliceAxis;
    bool Streamlines,Vectors,CutPlane,Volume,Mesh;
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Field;
    FBox Bounds=FBox(ForceInit);
    FStudioScalarDescriptor Scalar;
    FStudioColorMapping ColorMapping;
    FStudioViewSettings VolumeSettings;
    bool bSourcePoints=true;
    bool bReconstructedSurface=true;
    double PointSize=1;
    FStudioLoadCancellation Cancellation;
    bool IsCancelled() const { return Cancellation&&Cancellation->load(std::memory_order_relaxed); }
};
static TSharedPtr<FStudioGeometry> BuildPipelineGeometry(const FRenderRequest& R,const FStudioPipelineOutput& Output)
{
    const double Start=FPlatformTime::Seconds();
    auto Data=StudioPipelineRendering::Build(Output,R.ColorMapping,R.Cancellation);
    if(Data.bCancelled)return {};
    auto Out=MakeShared<FStudioGeometry>();Out->Error=Data.Error;Out->Scalar=R.Scalar;Out->ColorMapping=R.ColorMapping;
    Out->Bounds=Data.Bounds.IsValid?Data.Bounds:R.Bounds;
    auto MoveMesh=[](FStudioPipelineRenderMesh& From,FStudioSection& To)
    {To.Vertices=MoveTemp(From.Vertices);To.Normals=MoveTemp(From.Normals);To.Colors=MoveTemp(From.Colors);To.Indices=MoveTemp(From.Indices);};
    MoveMesh(Data.Glyphs,Out->Sections[0]);MoveMesh(Data.Contour,Out->Sections[7]);
    auto& S=Out->Sections[2];S.Vertices=MoveTemp(Data.Surface.Vertices);S.Indices=MoveTemp(Data.Surface.Indices);S.UVs=MoveTemp(Data.Surface.TextureCoordinates);
    Out->SurfaceScalars=MoveTemp(Data.Surface.Scalars);Out->ScalarTextureSize=Data.Surface.TextureSize;
    Out->BuildMs=(FPlatformTime::Seconds()-Start)*1000.;return Out;
}
static void AppendVectorGlyphs(const TArray<FStudioVectorGlyph>& Glyphs,double ReferenceMeters,
    FStudioGeometry& Out,const FStudioLoadCancellation& Cancellation)
{
    for(const auto& Glyph:Glyphs)
    {
        if(Cancellation&&Cancellation->load())return;
        const FVector A=Glyph.PositionMeters*100.,Tip=Glyph.TipMeters*100.,D=Glyph.Direction;
        const double Length=(Tip-A).Size(),Head=FMath::Min(ReferenceMeters*.6,Length*.25);
        Out.Sections[4].Tube(A,Tip,FMath::Min(ReferenceMeters*.035,Length*.05),Glyph.Color,&D);
        const FVector Side=FVector::CrossProduct(D,FMath::Abs(D.Y)<.9?FVector::RightVector:FVector::UpVector).GetSafeNormal();
        Out.Sections[4].Triangle(Tip,Tip-D*Head+Side*Head*.42,Tip-D*Head-Side*Head*.42,Glyph.Color);
    }
}
// Neutral visual extrusion only. The caller explicitly distinguishes original
// geometry from an attached inferred hole; neither supplies spanwise flow.
static void WingBody(const TArray<FVector2D>& Boundary,const TArray<FIntVector>& Caps,
    const FBox& Bounds,FStudioGeometry& Out,const FStudioLoadCancellation& Cancel)
{
    const double Stroke=FMath::Min(Bounds.GetSize().X,Bounds.GetSize().Z)*100.;
    const double SpanMin=FMath::Lerp(Bounds.Min.Y,Bounds.Max.Y,.175)*100.,SpanMax=FMath::Lerp(Bounds.Min.Y,Bounds.Max.Y,.825)*100.;
    Out.bAirfoilSolid=!Boundary.IsEmpty();
    double Winding=0;TArray<FVector> EdgeNormals;
    for(int32 I=0;I<Boundary.Num();++I)
    {
        const auto P=Boundary[I],Q=Boundary[(I+1)%Boundary.Num()];
        Winding+=P.X*Q.Y-Q.X*P.Y;
        EdgeNormals.Add(FVector(Q.Y-P.Y,0,P.X-Q.X).GetSafeNormal());
    }
    if(Winding<0)for(auto& N:EdgeNormals)N=-N;
    const auto CornerNormal=[&](int32 Corner,int32 Face)
    {
        const auto A=EdgeNormals[(Corner+Boundary.Num()-1)%Boundary.Num()],B=EdgeNormals[Corner];
        // Smooth the original curved skin; preserve the sharp trailing edge.
        return FVector::DotProduct(A,B)>.5?(A+B).GetSafeNormal():EdgeNormals[Face];
    };
    for(int32 I=0;I<Boundary.Num();++I)
    {
        if(Cancel&&Cancel->load())return;
        const auto P=Boundary[I],Q=Boundary[(I+1)%Boundary.Num()];
        const FVector A(P.X*100.,SpanMin,P.Y*100.),B(Q.X*100.,SpanMin,Q.Y*100.);
        const FVector C(Q.X*100.,SpanMax,Q.Y*100.),D(P.X*100.,SpanMax,P.Y*100.);
        const FLinearColor Solid(.015,.027,.04);
        Out.Sections[0].Triangle(A,B,C,Solid);
        Out.Sections[0].Triangle(A,C,D,Solid);
        const auto NP=CornerNormal(I,I),NQ=CornerNormal((I+1)%Boundary.Num(),I);
        Out.Sections[0].Normals.Append({NP,NQ,NQ,NP,NQ,NP});
        for(double Y:{SpanMin,SpanMax})
        {
            const FVector U(P.X*100.,Y,P.Y*100.),V(Q.X*100.,Y,Q.Y*100.);
            Out.Sections[5].Tube(U,V,Stroke*.0007,FLinearColor(.12,.2,.28));
        }
    }
    for(double Y:{SpanMin,SpanMax})for(const auto& T:Caps)
    {
        if(Cancel&&Cancel->load())return;
        const auto P=Boundary[T.X],Q=Boundary[T.Y],S=Boundary[T.Z];
        Out.Sections[0].Triangle(FVector(P.X*100.,Y,P.Y*100.),FVector(Q.X*100.,Y,Q.Y*100.),FVector(S.X*100.,Y,S.Y*100.),FLinearColor(.012,.024,.036));
        const FVector N(0,Y==SpanMin?-1.:1.,0);Out.Sections[0].Normals.Append({N,N,N});
    }
}
static void PointGeometry(const FRenderRequest& R,const FStudioPointFrame& Frame,FStudioGeometry& Out)
{
    const auto* Values=Frame.FindValues(R.Scalar.Id);
    if(!Values){Out.Error=TEXT("Selected point field is unavailable.");return;}
    const double Extent=R.Bounds.GetSize().GetMax()*100.;
    const double Radius=Extent*.0011*R.PointSize;
    auto Color=[&](int32 I){return StudioColor::Map((*Values)[I],R.ColorMapping);};
    auto Position=[&](int32 I){const auto P=Frame.Geometry->Positions[I];return FVector(P.X,P.Z,P.Y)*100.;};
    const auto Surface=R.Field->Reconstruction();
    const bool bSurface=R.bReconstructedSurface&&Surface.IsValid();
    if(bSurface)
    {
        const bool Focused=R.VolumeSettings.bFocusWingRegion&&R.VolumeSettings.MeshStyle==0;
        auto Data=StudioSurfaceRendering::Build(Frame,*Surface,R.Scalar.Id,R.ColorMapping,R.Cancellation,Focused?R.Bounds:FBox(ForceInit));
        if(!Data.Error.IsEmpty()){Out.Error=Data.Error;return;}
        auto& Section=Out.Sections[2];
        Section.Vertices=MoveTemp(Data.Vertices);Section.Indices=MoveTemp(Data.Indices);Section.UVs=MoveTemp(Data.TextureCoordinates);
        Out.SurfaceScalars=MoveTemp(Data.Scalars);Out.ScalarTextureSize=Data.TextureSize;
        Out.bFocusedSurface=Focused;
        if(Focused)WingBody(Surface->Boundary,Surface->BoundaryCaps,R.Bounds,Out,R.Cancellation);
    }
    if(R.bSourcePoints&&!bSurface)
    {
        // Three crossed glyph planes make original points visible from arbitrary
        // camera directions without turning their neighbourhood into a source mesh.
        const FVector Axis[]={FVector(Radius,0,0),FVector(0,Radius,0),FVector(0,0,Radius)};
        // Three crossed planes use 18 procedural vertices per point. Bound the
        // 3D overview below the mesh budget even before its volume is attached.
        const int32 MaximumPoints=Frame.Descriptor->SpatialDimensions==3?40000:50000;
        const int32 PointStride=FMath::Max(1,FMath::DivideAndRoundUp(Values->Num(),MaximumPoints));
        for(int32 I=0;I<Values->Num();I+=PointStride)
        {
            if(R.IsCancelled())return;
            const FVector P=Position(I);const auto C=Color(I);
            for(int32 A=0;A<3;++A)
            {
                const FVector U=Axis[A],V=Axis[(A+1)%3];
                Out.Sections[0].Triangle(P-U-V,P+U-V,P+U+V,C);
                Out.Sections[0].Triangle(P-U-V,P+U+V,P-U+V,C);
            }
        }
    }
    const TArray<double>* Components[3]={nullptr,nullptr,nullptr};
    for(const auto& F:Frame.Descriptor->Fields)
        if(F.Vector==TEXT("velocity")&&F.Unit==TEXT("m/s"))
            Components[F.Component==TEXT("x")?0:F.Component==TEXT("y")?1:2]=Frame.FindValues(F.Id);
    if(R.Vectors&&Components[0]&&Components[1]&&(Frame.Descriptor->SpatialDimensions==2||Components[2]))
    {
        // Deterministic decimation of original point rows. No resampled vector grid.
        const auto Rows=StudioFieldDisplay::VectorRows(Values->Num(),R.VolumeSettings.VectorCount);
        TArray<FStudioVectorSample> Samples;Samples.Reserve(Rows.Num());
        for(int32 I:Rows)
        {
            if(R.IsCancelled())return;
            const FVector V((*Components[0])[I],Components[2]?(*Components[2])[I]:0,(*Components[1])[I]);
            if(R.Bounds.IsInsideOrOn(Position(I)/100.))Samples.Add({Position(I)/100.,V,Color(I),true});
        }
        TArray<FStudioVectorGlyph> Glyphs;
        if(!StudioFieldDisplay::VectorGlyphs(Samples,Extent/100.*.035*R.VectorScale,
            R.VolumeSettings.bUniformVectors,Glyphs,Out.Vectors,R.Cancellation))return;
        AppendVectorGlyphs(Glyphs,Extent/100.,Out,R.Cancellation);
    }
}
static TSharedPtr<FStudioGeometry> BuildGeometry(const FRenderRequest& R)
{
    const double Start=FPlatformTime::Seconds();
    if(R.IsCancelled())return {};
    auto Out=MakeShared<FStudioGeometry>(); const IStudioField& Field=*R.Field;
    const FBox Bounds=R.Bounds;Out->Bounds=Bounds;Out->Scalar=R.Scalar;Out->ColorMapping=R.ColorMapping;
    if(!Bounds.IsValid)return Out;
    const FVector Size=Bounds.GetSize(),Center=Bounds.GetCenter();
    const double Stroke=FMath::Min(Size.X,Size.Z)*100.;
    const auto Points=Field.OriginalPoints();
    if(R.VolumeSettings.MeshStyle>0)
    {
        const auto Topology=StudioMeshDisplay::Build(Field,R.Cancellation);
        if(R.IsCancelled())return {};
        Out->Mesh={Topology.TriangleCount,Topology.bDerived,Topology.Notice};
        auto& Section=Out->Sections[9];
        for(int32 I=0;I<Topology.PositionsMeters.Num();++I)
        {
            Section.Vertices.Add(Topology.PositionsMeters[I]*100.);
            Section.UVs.Add(I%3==1?FVector2D(1,0):I%3==2?FVector2D(0,1):FVector2D::ZeroVector);
        }
        Section.Indices=Topology.Indices;
    }
    const bool MeshOnly=R.VolumeSettings.MeshStyle==2&&Out->Mesh.Triangles>0;
    Out->Mesh.bFieldFillHidden=MeshOnly;
    if(Points)
    {
        auto PointRequest=R;
        if(MeshOnly){PointRequest.bSourcePoints=false;PointRequest.bReconstructedSurface=false;}
        if(Field.VolumeReconstruction()&&(R.Volume||R.VolumeSettings.bVolumeIsosurface))
        {
            auto Grid=StudioVolumes::Build(*Points,*Field.VolumeReconstruction(),R.Scalar.Id,R.ColorMapping,R.Cancellation,R.VolumeSettings.bHome4AirMask);
            if(R.VolumeSettings.bHome4Vorticity&&R.Scalar.Id==TEXT("q")&&Field.VolumeReconstruction()->OriginalGrid&&Grid.Error.IsEmpty())
            {
                FString Error;const auto Omega=Field.LoadScalarSnapshot(TEXT("vorticity_magnitude"),R.Cancellation,Error);
                const auto Description=Field.Scalar(TEXT("vorticity_magnitude"));
                if(!Omega||!Description.IsSet()||!Omega->OriginalPoints())Grid.Error=Error.IsEmpty()?TEXT("Vorticity volume requires original Q and vorticity magnitude fields."):Error;
                else
                {
                    const auto Mapping=StudioColor::Resolve(R.Field->Identity()->Dataset,Description.GetValue(),{});
                    const double Ref=FMath::Max(FMath::Abs(Mapping.Minimum),FMath::Abs(Mapping.Maximum));
                    FStudioColorMapping Weight;Weight.Minimum=0;Weight.Maximum=Ref>0?Ref:1;
                    const auto O=StudioVolumes::Build(*Omega->OriginalPoints(),*Field.VolumeReconstruction(),TEXT("vorticity_magnitude"),Weight,R.Cancellation,R.VolumeSettings.bHome4AirMask);
                    if(!O.Error.IsEmpty()||O.Texels.Num()!=Grid.Texels.Num())Grid.Error=O.Error.IsEmpty()?TEXT("Opacity field grid differs from the displayed Q grid."):O.Error;
                    else
                    {
                        Grid.OpacityTexels.SetNumZeroed(Grid.Texels.Num());Grid.OpacitySource=TEXT("vorticity_magnitude squared");Grid.OpacityReference=Ref*Ref;
                        for(int32 I=0;I<Grid.Texels.Num();++I)
                        {
                            if((I&255)==0&&R.IsCancelled())return {};
                            if(O.Texels[I].Y<.5f){Grid.Texels[I].Y=0;continue;}
                            const double WeightValue=double(O.Texels[I].X)*O.Texels[I].X;
                            Grid.OpacityTexels[I]=float(FMath::Min(WeightValue,1.e20));
                        }
                    }
                }
            }
            Out->VolumeSettings=R.VolumeSettings;
            if(!Grid.Error.IsEmpty())Out->Error=Grid.Error;
            else if(R.VolumeSettings.bVolumeIsosurface)
            {
                auto Iso=StudioVolumes::Isosurface(Grid,R.ColorMapping.Maximum>R.ColorMapping.Minimum?
                    (R.VolumeSettings.VolumeIsovalue-R.ColorMapping.Minimum)/(R.ColorMapping.Maximum-R.ColorMapping.Minimum):.5,R.Cancellation);
                if(!Iso.Error.IsEmpty())Out->Error=Iso.Error;
                else
                {
                    auto& S=Out->Sections[7];
                    const FVector CL=R.VolumeSettings.VolumeClipMinimum,CH=R.VolumeSettings.VolumeClipMaximum;
                    const FVector Low=Grid.SourceBounds.Min+Grid.SourceBounds.GetSize()*FVector(CL.X,CL.Z,CL.Y),
                        High=Grid.SourceBounds.Min+Grid.SourceBounds.GetSize()*FVector(CH.X,CH.Z,CH.Y);
                    const FLinearColor Color=StudioColor::Map(R.VolumeSettings.VolumeIsovalue,R.ColorMapping);
                    for(int32 Face=0;Face<Iso.Indices.Num();Face+=3)
                    {
                        if((Face&255)==0&&R.IsCancelled())return {};
                        TArray<FVector,TInlineAllocator<12>> Polygon;
                        for(int32 I=0;I<3;++I)Polygon.Add(Iso.PositionsMeters[Iso.Indices[Face+I]]);
                        for(int32 Axis=0;Axis<3;++Axis)for(int32 Side=0;Side<2;++Side)
                        {
                            TArray<FVector,TInlineAllocator<12>> Clipped;
                            const double Plane=Side?High[Axis]:Low[Axis];
                            for(int32 I=0;I<Polygon.Num();++I)
                            {
                                const FVector A=Polygon[I],B=Polygon[(I+1)%Polygon.Num()];
                                const bool InsideA=Side?A[Axis]<=Plane:A[Axis]>=Plane,InsideB=Side?B[Axis]<=Plane:B[Axis]>=Plane;
                                if(InsideA)Clipped.Add(A);
                                if(InsideA!=InsideB)Clipped.Add(FMath::Lerp(A,B,(Plane-A[Axis])/(B[Axis]-A[Axis])));
                            }
                            Polygon=MoveTemp(Clipped);
                        }
                        for(int32 I=1;I+1<Polygon.Num();++I)
                        {
                            const auto Scene=[](const FVector& P){return FVector(P.X,P.Z,P.Y)*100.;};
                            const FVector A=Scene(Polygon[0]),B=Scene(Polygon[I]),C=Scene(Polygon[I+1]);
                            const FVector Normal=FVector::CrossProduct(B-A,C-A).GetSafeNormal();
                            S.Triangle(A,B,C,Color);S.Normals.Append({Normal,Normal,Normal});
                        }
                        if(S.Indices.Num()>900000){Out->Error=TEXT("Clipped isosurface exceeds the display budget.");return Out;}
                    }
                }
            }
            if(R.Volume)Out->VolumeData=MoveTemp(Grid);
            PointRequest.bSourcePoints=false;
            const auto V=Field.VolumeReconstruction();
            constexpr int32 Segments=64;
            const FLinearColor SolidColor(.07,.09,.11);
            for(int32 I=0;!V->OriginalGrid&&I<Segments;++I)
            {
                const double A=2*PI*I/Segments,B=2*PI*(I+1)/Segments;
                const auto At=[&](double Angle,double Z){return FVector(V->CylinderCenter.X+V->CylinderRadius*FMath::Cos(Angle),Z,
                    V->CylinderCenter.Y+V->CylinderRadius*FMath::Sin(Angle))*100.;};
                const FVector P=At(A,V->SourceBounds.Min.Z),Q=At(B,V->SourceBounds.Min.Z),
                    T=At(A,V->SourceBounds.Max.Z),U=At(B,V->SourceBounds.Max.Z);
                Out->Sections[0].Triangle(P,Q,U,SolidColor);Out->Sections[0].Triangle(P,U,T,SolidColor);
                Out->Sections[0].Triangle(P,FVector(V->CylinderCenter.X,V->SourceBounds.Min.Z,V->CylinderCenter.Y)*100.,Q,SolidColor);
                Out->Sections[0].Triangle(T,U,FVector(V->CylinderCenter.X,V->SourceBounds.Max.Z,V->CylinderCenter.Y)*100.,SolidColor);
            }
        }
        if(const auto V=Field.VolumeReconstruction();V&&V->OriginalGrid)
        {
            struct FSourceLayer{bool Enabled;const TCHAR* Id;double Value;int32 Section;FLinearColor Color;};
            const FSourceLayer Layers[]={
                {R.VolumeSettings.bHome4InterfaceSurface,TEXT("phi"),R.VolumeSettings.Home4InterfaceIsovalue,10,FLinearColor(.12,.66,.76,.48)},
                {R.VolumeSettings.bHome4ObstacleSurface,TEXT("solid"),.5,11,FLinearColor(.25,.3,.36)},
                {R.VolumeSettings.bHome4SdfSurface,TEXT("sdf"),0,12,FLinearColor(.9,.62,.18)}};
            for(const auto& Layer:Layers)if(Layer.Enabled)
            {
                FString Error;const auto Snapshot=Field.LoadScalarSnapshot(Layer.Id,R.Cancellation,Error);
                if(!Snapshot||!Snapshot->OriginalPoints()){Out->Error=Error.IsEmpty()?FString(TEXT("Missing layer "))+Layer.Id:Error;continue;}
                FStudioColorMapping Raw;Raw.Minimum=0;Raw.Maximum=1;
                const auto Grid=StudioVolumes::Build(*Snapshot->OriginalPoints(),*V,Layer.Id,Raw,R.Cancellation,false);
                if(!Grid.Error.IsEmpty()){Out->Error=Grid.Error;continue;}
                const auto Iso=StudioVolumes::Isosurface(Grid,Layer.Value,R.Cancellation);
                if(!Iso.Error.IsEmpty()){Out->Error=Iso.Error;continue;}
                auto& Section=Out->Sections[Layer.Section];
                const FVector CL=R.VolumeSettings.VolumeClipMinimum,CH=R.VolumeSettings.VolumeClipMaximum;
                const FVector Low=Grid.SourceBounds.Min+Grid.SourceBounds.GetSize()*FVector(CL.X,CL.Z,CL.Y),
                    High=Grid.SourceBounds.Min+Grid.SourceBounds.GetSize()*FVector(CH.X,CH.Z,CH.Y);
                const auto ToScene=[](const FVector& P){return FVector(P.X,P.Z,P.Y)*100.;};
                for(int32 Face=0;Face<Iso.Indices.Num();Face+=3)
                {
                    if((Face&255)==0&&R.IsCancelled())return {};
                    TArray<FVector,TInlineAllocator<12>> Polygon;
                    for(int32 I=0;I<3;++I)Polygon.Add(Iso.PositionsMeters[Iso.Indices[Face+I]]);
                    for(int32 Axis=0;Axis<3;++Axis)for(int32 Side=0;Side<2;++Side)
                    {
                        TArray<FVector,TInlineAllocator<12>> Clipped;const double Plane=Side?High[Axis]:Low[Axis];
                        for(int32 I=0;I<Polygon.Num();++I)
                        {
                            const FVector A=Polygon[I],B=Polygon[(I+1)%Polygon.Num()];
                            const bool IA=Side?A[Axis]<=Plane:A[Axis]>=Plane,IB=Side?B[Axis]<=Plane:B[Axis]>=Plane;
                            if(IA)Clipped.Add(A);if(IA!=IB)Clipped.Add(FMath::Lerp(A,B,(Plane-A[Axis])/(B[Axis]-A[Axis])));
                        }
                        Polygon=MoveTemp(Clipped);
                    }
                    for(int32 I=1;I+1<Polygon.Num();++I)
                    {
                        const FVector A=ToScene(Polygon[0]),B=ToScene(Polygon[I]),C=ToScene(Polygon[I+1]);
                        Section.Triangle(A,B,C,Layer.Color);
                        const auto N=FVector::CrossProduct(B-A,C-A).GetSafeNormal();Section.Normals.Append({N,N,N});
                    }
                    if(Section.Indices.Num()>900000){Out->Error=TEXT("Clipped source layer exceeds the display budget.");return Out;}
                }
            }
        }
        PointGeometry(PointRequest,*Points,*Out);
        const auto VolumeGrid=Field.VolumeReconstruction();
        FBox SliceBounds=Bounds;
        if(VolumeGrid)
        {
            const auto& B=VolumeGrid->SourceBounds;
            const FBox SceneGrid(FVector(B.Min.X,B.Min.Z,B.Min.Y),FVector(B.Max.X,B.Max.Z,B.Max.Y));
            SliceBounds=FBox(SceneGrid.Min+SceneGrid.GetSize()*R.VolumeSettings.VolumeClipMinimum,
                SceneGrid.Min+SceneGrid.GetSize()*R.VolumeSettings.VolumeClipMaximum);
        }
        if(VolumeGrid&&R.CutPlane&&R.SlicePosition>=SliceBounds.Min[R.SliceAxis]&&R.SlicePosition<=SliceBounds.Max[R.SliceAxis])
        {
            // A derived inspection slice samples the same immutable 3D field.
            // Unsupported cells are omitted, never bridged with zero-valued data.
            TArray<uint8> OriginalMask;
            if(VolumeGrid->OriginalGrid)
            {
                const auto Frame=Field.OriginalPoints();FString MaskError;
                if(Frame)OriginalMask=StudioVolumes::SourceMask(*Frame,*VolumeGrid,R.Scalar.Id,false,MaskError,R.Cancellation,R.VolumeSettings.bHome4AirMask);
                if(!Frame||!MaskError.IsEmpty()||OriginalMask.IsEmpty()){Out->Error=MaskError.IsEmpty()?TEXT("Original source masks are unavailable."):MaskError;return Out;}
            }
            const int32 Axis=R.SliceAxis,A=(Axis+1)%3,B=(Axis+2)%3;
            const FIntVector SceneDimensions(VolumeGrid->Dimensions.X,VolumeGrid->Dimensions.Z,VolumeGrid->Dimensions.Y);
            const int32 NX=SceneDimensions[A]-1,NY=SceneDimensions[B]-1,Width=512,Height=FMath::DivideAndRoundUp((NX+1)*(NY+1),Width);
            const FVector SliceSize=SliceBounds.GetSize();
            auto& S=Out->Sections[2];TBitArray<> Valid(false,(NX+1)*(NY+1));
            Out->SurfaceScalars.SetNumZeroed(Width*Height);Out->ScalarTextureSize=FIntPoint(Width,Height);
            const double Range=R.ColorMapping.Maximum-R.ColorMapping.Minimum;
            for(int32 J=0;J<=NY;++J)for(int32 I=0;I<=NX;++I)
            {
                if(R.IsCancelled())return {};
                const int32 Index=J*(NX+1)+I;FVector P=SliceBounds.Min;
                P[Axis]=R.SlicePosition;P[A]+=SliceSize[A]*I/NX;P[B]+=SliceSize[B]*J/NY;
                double Scalar;
                if(VolumeGrid->OriginalGrid){const FVector Source(P.X,P.Z,P.Y);Valid[Index]=StudioVolumes::SampleSource(*Points,*VolumeGrid,OriginalMask,R.Scalar.Id,Source,Scalar);}
                else Valid[Index]=Field.SampleScalar(P,R.Scalar.Id,Scalar);
                if(Valid[Index])
                {
                    const double Normalized=Range>0?(Scalar-R.ColorMapping.Minimum)/Range:.5;
                    if(!FMath::IsFinite(Normalized)||FMath::Abs(Normalized)>1.e30)
                    {Out->Error=TEXT("Slice display range exceeds GPU precision. Widen the color range.");return Out;}
                    Out->SurfaceScalars[Index]=float(Normalized);
                }
                S.Vertices.Add(P*100.);S.UVs.Add(FVector2D((Index%Width+.5)/Width,(Index/Width+.5)/Height));
            }
            for(int32 J=0;J<NY;++J)for(int32 I=0;I<NX;++I)
            {
                const int32 N=J*(NX+1)+I;
                if(Valid[N]&&Valid[N+1]&&Valid[N+NX+1]&&Valid[N+NX+2])
                {
                    const auto SourcePosition=[](const FVector& P){return FVector(P.X,P.Z,P.Y)/100.;};
                    const FBox Cell(SourcePosition(S.Vertices[N]),SourcePosition(S.Vertices[N+NX+2]));
                    if(Field.VolumeReconstruction()->OriginalGrid?
                        StudioVolumes::SupportsSourceRegion(*VolumeGrid,OriginalMask,Cell,R.Cancellation):
                        !Field.VolumeReconstruction()->ContainsSolid(Cell))
                        S.Indices.Append({N,N+1,N+NX+2,N,N+NX+2,N+NX+1});
                }
            }
        }
    }
    else
    {
    WingBody(Field.Boundary(),Field.BoundaryTriangles(),Bounds,*Out,R.Cancellation);
    auto Plane=[&](int32 Section,int32 Axis,double Position,double Alpha)
    {
        const int32 NX=Axis==0?38:116,NY=44;
        const int32 Base=Out->Sections[Section].Vertices.Num();
        for(int32 J=0;J<=NY;++J) for(int32 I=0;I<=NX;++I)
        {
            if(R.IsCancelled())return;
            const double U=static_cast<double>(I)/NX,V=static_cast<double>(J)/NY;
            FVector P=Axis==0?FVector(Position,Bounds.Min.Y+Size.Y*U,Bounds.Min.Z+Size.Z*V):Axis==1?FVector(Bounds.Min.X+Size.X*U,Position,Bounds.Min.Z+Size.Z*V):FVector(Bounds.Min.X+Size.X*U,Bounds.Min.Y+Size.Y*V,Position);
            double Value; const bool Valid=Field.SampleScalar(P,R.Scalar.Id,Value);
            FLinearColor C=Valid?StudioColor::Map(Value,R.ColorMapping):FLinearColor::Transparent; C.A=Valid?Alpha:0;
            Out->Sections[Section].Vertices.Add(P*100.); Out->Sections[Section].Colors.Add(C);
        }
        for(int32 J=0;J<NY;++J) for(int32 I=0;I<NX;++I)
        { const int32 N=Base+J*(NX+1)+I; Out->Sections[Section].Indices.Append({N,N+1,N+NX+2,N,N+NX+2,N+NX+1}); }
    };
    if(!MeshOnly&&R.Volume) for(double T:{.225,.3625,.6375,.775}) Plane(1,1,FMath::Lerp(Bounds.Min.Y,Bounds.Max.Y,T),R.Opacity*0.12);
    if(!MeshOnly&&R.CutPlane&&R.SlicePosition>=Bounds.Min[R.SliceAxis]&&R.SlicePosition<=Bounds.Max[R.SliceAxis])
    {
        if(R.SliceAxis==1&&Field.OriginalTriangleCount()>0&&Field.OriginalTriangleCount()<=131072)
        {
            auto Slice=StudioFlowPresentation::OriginalSlice(Field,Bounds,R.SlicePosition,R.ColorMapping,R.Scalar.Id,R.Cancellation);
            auto& S=Out->Sections[2];S.Vertices=MoveTemp(Slice.Positions);S.UVs=MoveTemp(Slice.ScalarOpacity);S.Indices=MoveTemp(Slice.Indices);S.VelocityUVs=MoveTemp(Slice.Velocity);
            Out->bOriginalSlice=true;
        }
        else Plane(2,R.SliceAxis,R.SlicePosition,0.28);
    }
    if(R.Vectors)
    {
        const auto Positions=StudioFieldDisplay::VectorGrid(Bounds,R.VolumeSettings.VectorCount);
        TArray<FStudioVectorGlyph> Glyphs;
        if(!StudioFieldDisplay::VectorGlyphs(Field,Positions,R.Scalar.Id,R.ColorMapping,Size.GetMax(),R.VectorScale,Glyphs,R.Cancellation,
            R.VolumeSettings.bUniformVectors,&Out->Vectors))return {};
        AppendVectorGlyphs(Glyphs,Size.GetMax(),*Out,R.Cancellation);
    }
    }
    if(R.Streamlines)
    {
        const FBox StreamBounds=StudioStreamlines::DomainBounds(Field,Bounds);
        TArray<FStudioSeedObject> Seeds=R.VolumeSettings.InspectionObjects.Seeds;
        const auto Identity=Field.Identity();const auto& Settings=R.VolumeSettings.StreamlineSettings;
        Out->Streams.bAutomaticSeeds=Settings.bAutomaticSeeds;
        const auto Volume=Field.VolumeReconstruction();const bool Native=Volume&&Volume->OriginalGrid;
        const auto Speed=Native?Field.Scalar(TEXT("speed")):TOptional<FStudioScalarDescriptor>();
        const FString StreamScalar=Speed.IsSet()?TEXT("speed"):R.Scalar.Id;
        const FStudioColorMapping StreamMapping=Speed.IsSet()?StudioColor::Resolve(Identity->Dataset,Speed.GetValue(),R.VolumeSettings.ScalarStyles):R.ColorMapping;
        if(Settings.bAutomaticSeeds&&Identity.IsSet())
        {
            FStudioSeedObject Seed;Seed.Id=FGuid(0x5354524d,0x4155544f,0,1);Seed.Name=TEXT("Automatic flow");
            Seed.Source={Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256};Seed.Count=Settings.AutomaticSeedCount;
            Seed.Kind=EStudioSeedKind::Points;FString SeedError;
            Seeds.Reset();
            if(!StudioStreamlines::AutomaticSeeds(Field,StreamBounds,Seed.Count,Settings.Direction,Seed.Points,SeedError,R.Cancellation))
            {if(R.IsCancelled())return {};Out->Streams.Notice=SeedError;}
            else if(!Seed.Points.IsEmpty()){if(!SeedError.IsEmpty())Out->Streams.Notice=SeedError;Seeds.Add(MoveTemp(Seed));}
            else Out->Streams.Notice=Native?TEXT("No supported liquid cells found in the bounded seed preview."):TEXT("No supported flow crosses the domain faces. Use saved seed sets for internal circulation.");
        }
        FStudioStreamlineOutput Stream;FString Error;
        if(!StudioStreamlines::Build(Field,StreamBounds,Seeds,Settings,StreamScalar,Stream,Error,R.Cancellation))
        {if(R.IsCancelled())return {};Out->Streams.Notice=Error;}
        else
        {
            auto& Summary=Out->Streams;Summary.Seeds=Stream.SeedCount;Summary.Segments=Stream.Segments;
            Summary.Attempts=Stream.Attempts;Summary.Method=Stream.Method;Summary.RejectedAttempts=Stream.RejectedAttempts;
            Summary.VelocityEvaluations=Stream.VelocityEvaluations;Summary.ScalarEvaluations=Stream.ScalarEvaluations;Summary.SupportEvaluations=Stream.SupportEvaluations;
            Summary.WidthMeters=Stream.WidthMeters;Summary.bBudgetExhausted=Stream.bBudgetExhausted;
            Summary.SeedNotices=MoveTemp(Stream.Notices);
            for(const auto& Path:Stream.Paths)
            {
                if(Path.PositionsMeters.Num()>1)++Summary.Lines;
                for(int32 I=1;I<Path.PositionsMeters.Num();++I)
                {
                    if(R.IsCancelled())return {};
                    const FVector A=Path.PositionsMeters[I-1],B=Path.PositionsMeters[I],Delta=B-A;
                    const FVector Direction=Delta/Delta.Size();
                    auto& S=Out->Sections[3];const int32 First=S.Vertices.Num();
                    const auto StartColor=StudioColor::Map(Path.Scalars[I-1],StreamMapping),EndColor=StudioColor::Map(Path.Scalars[I],StreamMapping);
                    S.Tube(A*100.,B*100.,Stream.WidthMeters*50.,StartColor,&Direction);
                    // Continuous endpoint color along each unchanged integration segment.
                    for(int32 V=First;V<S.Vertices.Num();++V)if((V-First)%6==1||(V-First)%6==2||(V-First)%6==4)S.Colors[V]=EndColor;
                }
            }
            if(Settings.bDirectionMarkers)
            {
                const auto Markers=StudioFlowPresentation::DirectionMarkers(Field,Stream,StreamBounds,R.Cancellation);
                for(const auto& Marker:Markers)
                {
                    if(R.IsCancelled())return {};
                    const FVector D=Marker.Direction,P=Marker.PositionMeters*100.;
                    const FVector Tip=P+D*Marker.LengthMeters*50.,Base=P-D*Marker.LengthMeters*50.;
                    const FVector N=FVector::CrossProduct(D,FMath::Abs(D.Z)<.9?FVector::UpVector:FVector::RightVector).GetSafeNormal();
                    const FVector T=FVector::CrossProduct(D,N);const auto Color=StudioColor::Map(Marker.Scalar,StreamMapping);
                    for(int32 Side=0;Side<6;++Side)
                    {
                        const double A=2*PI*Side/6.,B=2*PI*(Side+1)/6.;
                        const FVector U=Base+(N*FMath::Cos(A)+T*FMath::Sin(A))*Marker.RadiusMeters*100.;
                        const FVector V=Base+(N*FMath::Cos(B)+T*FMath::Sin(B))*Marker.RadiusMeters*100.;
                        Out->Sections[3].Triangle(Tip,U,V,Color);Out->Sections[3].Triangle(Base,V,U,Color);
                    }
                }
            }
            if(!Summary.Lines&&Summary.Notice.IsEmpty())Summary.Notice=Summary.Seeds?TEXT("No traces at these seeds. Check coverage, source plane and recorded velocity."):
                Settings.bAutomaticSeeds?TEXT("Streamlines need recorded velocity and verified interpolation."):TEXT("No visible seed sets for this recording.");
        }
    }
    if(R.IsCancelled())return {};
    FVector Corners[8]; for(int32 I=0;I<8;++I) Corners[I]=FVector(I&1?Bounds.Max.X:Bounds.Min.X,I&2?Bounds.Max.Y:Bounds.Min.Y,I&4?Bounds.Max.Z:Bounds.Min.Z)*100.;
    for(int32 I=0;I<8;++I) for(int32 Bit=0;Bit<3;++Bit) if(!(I&(1<<Bit))) Out->Sections[5].Tube(Corners[I],Corners[I|(1<<Bit)],Stroke*.0013,FLinearColor(0.075,0.12,0.17));
    if(R.Mesh)
    {
        for(int32 I=0;I<=32;++I) { const double X=Bounds.Min.X+Size.X*I/32.; Out->Sections[6].Tube(FVector(X,Bounds.Min.Y,Bounds.Min.Z)*100.,FVector(X,Bounds.Max.Y,Bounds.Min.Z)*100.,Stroke*.000375,FLinearColor(0.05,0.13,0.17)); }
        for(int32 I=0;I<=12;++I) { const double Y=Bounds.Min.Y+Size.Y*I/12.; Out->Sections[6].Tube(FVector(Bounds.Min.X,Y,Bounds.Min.Z)*100.,FVector(Bounds.Max.X,Y,Bounds.Min.Z)*100.,Stroke*.000375,FLinearColor(0.05,0.13,0.17)); }
    }
    const auto Slices=StudioSliceRendering::Build(Field,Bounds,R.VolumeSettings.InspectionObjects.Slices,R.Scalar.Id,R.Cancellation,R.VolumeSettings.bHome4AirMask);
    Out->SliceNotices=Slices.Notices;
    Out->RenderedSlices=Slices.RenderedSlices;
    auto& InspectionSection=Out->Sections[8];const double ScalarRange=R.ColorMapping.Maximum-R.ColorMapping.Minimum;
    bool ValidTransport=true;
    for(int32 I=0;I<Slices.PositionsMeters.Num();++I)
    {
        const double Normalized=ScalarRange>0?(Slices.Scalars[I]-R.ColorMapping.Minimum)/ScalarRange:.5;
        if(!FMath::IsFinite(Normalized)||FMath::Abs(Normalized)>1.e30){ValidTransport=false;break;}
        InspectionSection.Vertices.Add(Slices.PositionsMeters[I]*100.);
        InspectionSection.UVs.Add(FVector2D(Normalized,Slices.Opacities[I]));
    }
    if(ValidTransport)InspectionSection.Indices=Slices.Indices;
    else
    {
        InspectionSection=FStudioSection();
        for(const auto& S:R.VolumeSettings.InspectionObjects.Slices)if(S.bVisible)
            Out->SliceNotices.Add(S.Id,TEXT("Slice color range exceeds GPU precision. Widen the range."));
    }
    Out->BuildMs=(FPlatformTime::Seconds()-Start)*1000.; return Out;
}

AStudioScene::AStudioScene()
{
    PrimaryActorTick.bCanEverTick=true;
    Mesh=CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("FlowScene")); SetRootComponent(Mesh);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision); Mesh->SetCastShadow(false);
    Backdrop=CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ViewportBackdrop"));
    Backdrop->SetupAttachment(RootComponent);Backdrop->SetCollisionEnabled(ECollisionEnabled::NoCollision);Backdrop->SetCastShadow(false);
    VolumeComponent=CreateDefaultSubobject<UStudioVolumeComponent>(TEXT("ScientificVolume"));
    VolumeComponent->SetupAttachment(RootComponent);VolumeComponent->SetVisibility(false);
    Capture=CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("ViewportCamera"));
    Capture->SetupAttachment(RootComponent);
    // Slate displays the last texture while idle. Submit a new view only when
    // its geometry, camera or dimensions change; retain reusable view resources.
    Capture->bCaptureEveryFrame=false; Capture->bCaptureOnMovement=false;
    Capture->bAlwaysPersistRenderingState=true;
    Capture->CaptureSource=ESceneCaptureSource::SCS_SceneColorHDRNoAlpha;
    Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->FOVAngle=48.; Capture->ShowFlags.SetAtmosphere(false); Capture->ShowFlags.SetFog(false);
    Capture->ShowFlags.SetMotionBlur(false); Capture->ShowFlags.SetBloom(false); Capture->ShowFlags.SetTonemapper(false);
    Capture->ShowFlags.SetEyeAdaptation(false);
}
void AStudioScene::Initialize(TSharedRef<FStudioModel> InModel)
{
    Model=InModel;
    OpaqueMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Studio/M_Flow.M_Flow"));
    BodyMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Studio/M_FlowBody.M_FlowBody"));
    MeshEdgeMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Studio/M_MeshEdges.M_MeshEdges"));
    TransparentMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Studio/M_FlowAlpha.M_FlowAlpha"));
    ScalarMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Studio/M_FlowScalar.M_FlowScalar"));
    // The component's UPROPERTY material array owns this separate constant-value
    // surface material. Shape shading does not alter volume/slice field colors.
    Mesh->SetMaterial(7,LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Studio/M_FlowIsosurface.M_FlowIsosurface")));
    if(ScalarMaterial)ScalarInstance=UMaterialInstanceDynamic::Create(ScalarMaterial,this);
    if(auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Studio/M_FlowScalarFocus.M_FlowScalarFocus")))
        FocusScalarInstance=UMaterialInstanceDynamic::Create(Material,this);
    if(auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Studio/M_InspectionScalar.M_InspectionScalar")))
        {InspectionInstance=UMaterialInstanceDynamic::Create(Material,this);OriginalSliceInstance=UMaterialInstanceDynamic::Create(Material,this);}
    RenderTarget=NewObject<UTextureRenderTarget2D>(this);
    RenderTarget->ClearColor=FLinearColor(0.003,0.009,0.016,1);
    RenderTarget->InitCustomFormat(1280,720,PF_B8G8R8A8,false); RenderTarget->UpdateResourceImmediate(true);
    Backdrop->SetMaterial(0,OpaqueMaterial);
    Capture->TextureTarget=RenderTarget; Capture->ShowOnlyActorComponents(this);
    const bool bWasDirty=Model->HasUnsavedChanges();
    ApplyCamera(Model->Project.Camera);
    if(!bWasDirty) Model->AcceptLoadedView();
    RequestGeometry();
}
bool AStudioScene::InitializePipeline(TSharedRef<FStudioSnapshotSource,ESPMode::ThreadSafe> Snapshot)
{
    if(Model||!Snapshot->PipelineOutput())return false;
    FrozenPipelineOutput=Snapshot->PipelineOutput();Initialize(MakeShared<FStudioModel>(Snapshot));return true;
}
void AStudioScene::FitCamera()
{
    if(bGeometryView)
    {
        if(!PreviewBounds.IsValid)return;
        auto C=CameraState();C.Focus=PreviewBounds.GetCenter();const double Radius=FMath::Max(.001,PreviewBounds.GetExtent().Size());
        C.OrbitDistance=Radius*3.8;C.Position=C.Focus+FVector(1.5,2.7,1.7).GetSafeNormal()*C.OrbitDistance;
        C.Orientation=(C.Focus-C.Position).Rotation().Quaternion();C.OrthoWidth=Radius*3.;RestoreCamera(C,TEXT("Fit geometry"));return;
    }
    if(!Model||!RenderTarget)return;
    const FBox Bounds=(FrozenPipelineOutput||HasCurrentFrame())&&RenderedFlowBounds.IsValid?RenderedFlowBounds:Model->Solver->Descriptor().DisplayBounds;
    RestoreCamera(StudioView::FitBounds(SavedCameraState(),Bounds,
        double(RenderTarget->SizeX)/RenderTarget->SizeY,
        (Capture->bOverride_CustomNearClippingPlane?Capture->CustomNearClippingPlane:GNearClippingPlane)/100.),TEXT("Fit camera"));
}
void AStudioScene::FlowOverview()
{
    if(!HasCurrentFrame()||!CapturedField||FrozenPipelineOutput||Model->IsSnapshotView())return;
    FStudioInspectionState Next;
    if(!StudioFlowPresentation::Overview(*CapturedField,Model->Solver->Descriptor().DisplayBounds,CapturedScalar.Id,
        double(RenderTarget->SizeX)/RenderTarget->SizeY,Model->InspectionState(),Next))
    {Model->Notice=TEXT("This field does not support a flow overview. Use the camera and Display controls.");return;}
    Model->EndViewEdit();
    Model->EditView(TEXT("Flow overview"),[&](auto& State){State=Next;});ApplyCamera(Next.Camera);
    Model->Notice=TEXT("Flow overview · custom range from the displayed field · Undo view restores the previous view.");
}
void AStudioScene::UpdateBackdrop()
{
    if(!Backdrop||!Model)return;
    const bool Show=!bGeometryView&&!FrozenPipelineOutput&&(!Model->Solver->Descriptor().bSourcePoints||(Model->bFocusWingRegion&&Model->bReconstructedSurface&&Model->MeshStyle==0&&Model->Solver->Reconstruction()))&&!bCameraDepthClipping;
    Backdrop->SetVisibility(Show);if(!Show)return;
    const auto& B=Model->Solver->Descriptor().DisplayBounds;const auto C=CameraState();
    const double Depth=FMath::Max(1.,(C.Position-B.GetCenter()).Size()+B.GetExtent().Size()*2.)*100.;
    const FVector Forward=C.Orientation.GetForwardVector(),Right=C.Orientation.GetRightVector(),Up=C.Orientation.GetUpVector();
    const double Width=C.bOrthographic?C.OrthoWidth*60.:Depth*FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5))*1.2;
    const double Height=Width*RenderTarget->SizeY/RenderTarget->SizeX;
    const FVector Center=C.Position*100.+Forward*Depth;
    TArray<FVector> P={Center-Right*Width-Up*Height,Center+Right*Width-Up*Height,Center+Right*Width+Up*Height,Center-Right*Width+Up*Height};
    TArray<FLinearColor> Colors={FLinearColor(.001,.004,.01),FLinearColor(.001,.004,.01),FLinearColor(.0015,.008,.016),FLinearColor(.0015,.008,.016)};
    Backdrop->CreateMeshSection_LinearColor(0,P,{0,1,2,0,2,3},{},{},Colors,{},false,false);
}
void AStudioScene::Orbit(double DX,double DY)
{
    auto C=CameraState(); C.Orientation=StudioView::Turn(C.Orientation,DX,DY,.25);
    C.Position=C.Focus-C.Orientation.GetForwardVector()*C.OrbitDistance;
    RestoreCamera(C,TEXT("Orbit camera"));
}
void AStudioScene::Pan(double DX,double DY)
{
    auto C=CameraState(); const FVector Shift=(-C.Orientation.GetRightVector()*DX+C.Orientation.GetUpVector()*DY)*C.OrbitDistance*.0015;
    C.Focus+=Shift; C.Position+=Shift; RestoreCamera(C,TEXT("Pan camera"));
}
void AStudioScene::Zoom(double Amount)
{
    if(!FMath::IsFinite(Amount)) return;
    auto C=CameraState();
    if(C.bOrthographic) C.OrthoWidth=FMath::Clamp(C.OrthoWidth*FMath::Exp(-Amount*.12),.001,1.e6);
    else { C.OrbitDistance=FMath::Clamp(C.OrbitDistance*FMath::Exp(-Amount*.12),.0001,1.e8); C.Position=C.Focus-C.Orientation.GetForwardVector()*C.OrbitDistance; }
    RestoreCamera(C,TEXT("Zoom camera"));
}
void AStudioScene::Look(double DX,double DY)
{
    auto C=CameraState(); C.Orientation=StudioView::Turn(C.Orientation,DX,DY,.2);
    C.Focus=C.Position+C.Orientation.GetForwardVector()*C.OrbitDistance; RestoreCamera(C,TEXT("Look camera"));
}
void AStudioScene::Fly(const FVector& Direction,double Delta)
{
    if(Direction.IsNearlyZero()||Delta<=0.) return;
    const double Speed=Model&&Model->Solver->VolumeReconstruction()?Model->Solver->Descriptor().DisplayBounds.GetSize().GetMax()*.8:1.3;
    auto C=CameraState(); const FVector Shift=C.Orientation.RotateVector(Direction)*Delta*Speed;
    C.Position+=Shift; C.Focus+=Shift; RestoreCamera(C,TEXT("Fly camera"));
}
FVector AStudioScene::CameraPosition() const { return Capture->GetComponentLocation()/100.; }
FRotator AStudioScene::CameraRotation() const { return Capture->GetComponentRotation(); }
void AStudioScene::SetCameraPosition(const FVector& P)
{ auto C=CameraState(); C.Position=P; C.Focus=P+C.Orientation.GetForwardVector()*C.OrbitDistance; RestoreCamera(C,TEXT("Camera position")); }
void AStudioScene::SetCameraRotation(const FRotator& R)
{ auto C=CameraState(); C.Orientation=R.Quaternion(); C.Focus=C.Position+C.Orientation.GetForwardVector()*C.OrbitDistance; RestoreCamera(C,TEXT("Camera orientation")); }
void AStudioScene::SetCameraMode(bool bFree)
{ auto C=CameraState(); C.bFreeCamera=bFree; RestoreCamera(C,bFree?TEXT("Free fly mode"):TEXT("Orbit mode")); }
void AStudioScene::AlignCamera(const FIntVector& Direction)
{
    if(!StudioOrientation::IsDirection(Direction))return;
    if(Model)Model->EndViewEdit();
    const auto C=bGeometryView?(bDomainView?DomainCamera:GeometryCamera):SavedCameraState();
    RestoreCamera(StudioOrientation::Align(C,Direction),TEXT("View from ")+StudioOrientation::Label(Direction));
}
void AStudioScene::RestoreCamera(const FStudioCameraState& C,const FString& Label)
{
    if(bGeometryView){(bDomainView?DomainCamera:GeometryCamera)=C;ApplyCamera(C);return;}
    if(!Model || !Model->EditCamera(Label,C)) return;
    if(!StudioView::CameraEquals(CameraState(),C)) ApplyCamera(C);
}
FStudioCameraState AStudioScene::CameraState() const
{
    FStudioCameraState C; C.Position=CameraPosition(); C.Focus=Focus/100.;
    C.Orientation=Capture->GetComponentQuat(); C.OrbitDistance=CameraDistance/100.;
    C.FieldOfView=Capture->FOVAngle; C.OrthoWidth=Capture->OrthoWidth/100.;
    C.bOrthographic=Capture->ProjectionType==ECameraProjectionMode::Orthographic;
    C.bFreeCamera=bFreeCamera;
    C.bDepthClipping=bCameraDepthClipping;C.NearClipMeters=CameraNearClipMeters;C.FarClipMeters=CameraFarClipMeters;
    return C;
}
bool AStudioScene::SetDepthClipping(bool bEnabled,double NearMeters,double FarMeters)
{
    auto C=bGeometryView?(bDomainView?DomainCamera:GeometryCamera):SavedCameraState();
    C.bDepthClipping=bEnabled;C.NearClipMeters=NearMeters;C.FarClipMeters=FarMeters;
    if(!StudioView::IsValidClipping(C))return false;
    RestoreCamera(C,TEXT("Camera depth clipping"));return true;
}
void AStudioScene::ApplyCamera(const FStudioCameraState& C)
{
    if(!StudioView::IsValidClipping(C))return;
    Capture->SetWorldLocationAndRotation(C.Position*100.,C.Orientation);
    Focus=C.Focus*100.; CameraDistance=C.OrbitDistance*100.; bFreeCamera=C.bFreeCamera;
    Capture->FOVAngle=C.FieldOfView; Capture->OrthoWidth=C.OrthoWidth*100.;
    Capture->ProjectionType=C.bOrthographic?ECameraProjectionMode::Orthographic:ECameraProjectionMode::Perspective;
    bCameraDepthClipping=C.bDepthClipping;CameraNearClipMeters=C.NearClipMeters;CameraFarClipMeters=C.FarClipMeters;
    UpdateProjection();
    bCaptureDirty=true;
    // The document owns the exact camera values. Reading them back from the
    // renderer introduces float FOV/width and meter/centimeter round-off and
    // would mark a just-opened project dirty without any user edit.
    if(Model&&!bGeometryView) { Model->Project.Camera=C; AppliedCameraRevision=Model->CameraRevision; }
}
void AStudioScene::UpdateProjection()
{
    // Small physical 3D recordings need a near plane scaled to their domain.
    // Explicit camera planes remain authoritative through the custom matrix.
    const auto Volume=Model&&!bGeometryView?Model->Solver->VolumeReconstruction():nullptr;
    Capture->bOverride_CustomNearClippingPlane=Volume.IsValid();
    if(Volume)Capture->CustomNearClippingPlane=FMath::Max(.0001,Volume->SourceBounds.GetSize().GetMin()*.1);
    // On-demand captures cannot wait for a later frame to retire visibility
    // queries from the preceding frustum. A restored near/far range must be
    // visible on its first capture, including after the whole flow was clipped.
    Capture->bCameraCutThisFrame=true;
    Capture->bUseCustomProjectionMatrix=bCameraDepthClipping;
    if(bCameraDepthClipping&&RenderTarget)
    {
        FMatrix Projection;
        if(StudioView::BuildClippedProjection(CameraState(),FIntPoint(RenderTarget->SizeX,RenderTarget->SizeY),Projection))
            Capture->CustomProjectionMatrix=Projection;
    }
}
void AStudioScene::ResizeViewport(int32 W,int32 H,bool bExact)
{
    if(W<=0||H<=0)return;
    W=FMath::Clamp(W,320,3840); H=FMath::Clamp(H,240,2400);
    if(Model&&!bGeometryView&&!FrozenPipelineOutput&&Model->FitNewFlowView(double(W)/H))ApplyCamera(Model->Project.Camera);
    if(RenderTarget&&((bExact&&(RenderTarget->SizeX!=W||RenderTarget->SizeY!=H))||FMath::Abs(RenderTarget->SizeX-W)>8||FMath::Abs(RenderTarget->SizeY-H)>8))
    { RenderTarget->ResizeTarget(W,H);UpdateProjection();bCaptureDirty=true; }
}
void AStudioScene::RequestGeometry()
{
    if(!Model||bBuilding||PendingPreview.IsValid()) return;
    const auto& M=*Model; FRenderRequest R{M.DisplayFrame().Time,M.SlicePosition,M.StreamlineDensity,M.VectorScale,M.VolumeOpacity,M.SliceAxis,M.bStreamlines,M.bVectors,M.bCutPlane,M.bVolume,M.bMesh,nullptr};
    R.Bounds=M.Solver->Descriptor().DisplayBounds;
    R.Scalar=M.ActiveScalar();R.ColorMapping=M.ActiveColorMapping();R.bSourcePoints=M.bSourcePoints;R.PointSize=M.PointSize;
    R.bReconstructedSurface=M.bReconstructedSurface;R.VolumeSettings=M;
    GeometryCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);R.Cancellation=GeometryCancellation;
    BuildingSolver=M.Solver;BuildingProjectId=M.Project.Id;BuildingIntentRevision=M.RenderIntentRevision;
    BuildingRevision=M.Revision; bBuilding=true;
    // Keep the source alive across project changes and load/cache the frame on
    // this worker. Camera/UI input never waits for a frame's disk read.
    PendingGeometry=Async(EAsyncExecution::ThreadPool,[R,Solver=M.Solver,Frame=M.SelectedFrame,Pipeline=FrozenPipelineOutput]() mutable -> TSharedPtr<FStudioGeometry>
    {
        if(R.IsCancelled())return {};
        const auto& Descriptor=Solver->Descriptor();
        const bool TraceVelocity=R.Streamlines&&(!Descriptor.bSourcePoints||(Descriptor.bPointVelocity&&(Solver->Reconstruction()||Solver->VolumeReconstruction())));
        R.Field=Solver->CaptureViewField(Frame,R.Scalar.Id,R.Vectors||TraceVelocity,R.Cancellation);
        if(R.IsCancelled())return {};
        if(!Pipeline)R.Bounds=StudioFlowPresentation::DisplayBounds(*R.Field,R.Bounds,R.VolumeSettings);
        auto Geometry=Pipeline?BuildPipelineGeometry(R,*Pipeline):BuildGeometry(R);
        if(!Geometry||R.IsCancelled())return {};
        // Validity belongs to this immutable snapshot. Another successful read
        // may clear the source's latest error, but cannot validate this frame.
        if(!R.Field->IsValid()) Geometry->Error=TEXT("The requested source frame is unavailable or failed validation. Reopen the recording.");
        Geometry->Dataset=Solver->Descriptor().Id; Geometry->Title=Solver->Descriptor().Title;
        Geometry->Frame=Solver->EvaluateFrame(Frame);Geometry->Field=R.Field;return Geometry;
    });
}
bool AStudioScene::IsGeometryRequestCurrent() const
{
    return Model&&BuildingProjectId==Model->Project.Id&&BuildingSolver.Pin()==Model->Solver&&
        BuildingIntentRevision==Model->RenderIntentRevision;
}
void AStudioScene::CancelBuild(const FStudioLoadCancellation& Cancellation)
{
    if(Cancellation&&!Cancellation->exchange(true))++CancelledBuildCount;
}
bool AStudioScene::HasCurrentFrame() const
{
    return Model&&(!ViewVisibility||ViewVisibility())&&Model->Workspace==EStudioWorkspace::Solve&&CapturedRevision==Model->Revision&&
        CapturedIntentRevision==Model->RenderIntentRevision&&CapturedProjectId==Model->Project.Id&&
        CapturedSolver.Pin()==Model->Solver&&PresentedDataset==Model->Project.Dataset;
}
void AStudioScene::ApplyGeometry(const FStudioGeometry& G)
{
    if(G.Mesh.Triangles>0&&!MeshEdgeMaterial)
    {Model->Notice=TEXT("The mesh edge material is missing. Repair application rendering assets.");RenderedDataset.Empty();Mesh->ClearAllMeshSections();bCaptureDirty=true;return;}
    if(!G.Sections[7].Vertices.IsEmpty()&&!Mesh->GetMaterial(7))
    {Model->Notice=TEXT("The isosurface material is missing. Rebuild application materials.");RenderedDataset.Empty();Mesh->ClearAllMeshSections();bCaptureDirty=true;return;}
    if(G.VolumeData.IsSet())
    {
        FString VolumeError;
        if(!VolumeComponent->Present(*G.VolumeData,G.ColorMapping,G.VolumeSettings,VolumeError))
        {Model->Notice=VolumeError;RenderedDataset.Empty();Mesh->ClearAllMeshSections();bCaptureDirty=true;return;}
    }
    else VolumeComponent->ClearVolume();
    const bool bSurface=!G.SurfaceScalars.IsEmpty();
    if(bSurface)
    {
        if(!ScalarInstance||(G.bFocusedSurface&&!FocusScalarInstance))
        {
            Mesh->ClearAllMeshSections();RenderedDataset.Empty();bCaptureDirty=true;
            Model->Notice=TEXT("The scalar surface material is missing. Rebuild the application materials or use original-point mode.");
            return;
        }
        if(!ScalarTexture||ScalarTexture->GetSizeX()!=G.ScalarTextureSize.X||ScalarTexture->GetSizeY()!=G.ScalarTextureSize.Y)
        {
            ScalarTexture=UTexture2D::CreateTransient(G.ScalarTextureSize.X,G.ScalarTextureSize.Y,PF_R32_FLOAT);
            if(!ScalarTexture)
            {
                Mesh->ClearAllMeshSections();RenderedDataset.Empty();bCaptureDirty=true;
                Model->Notice=TEXT("The scalar surface texture could not be allocated. Use original-point mode.");return;
            }
            ScalarTexture->SRGB=false;ScalarTexture->Filter=TF_Nearest;ScalarTexture->NeverStream=true;
            ScalarTexture->AddressX=TA_Clamp;ScalarTexture->AddressY=TA_Clamp;
            ScalarTexture->UpdateResource();
            ScalarInstance->SetTextureParameterValue(TEXT("SourceScalars"),ScalarTexture);
            if(FocusScalarInstance)FocusScalarInstance->SetTextureParameterValue(TEXT("SourceScalars"),ScalarTexture);
        }
        // The render command owns this immutable upload until the RHI consumes
        // it. Reuse the bounded texture; never retain a frame behind a raw pointer.
        auto Upload=MakeShared<TArray<float>,ESPMode::ThreadSafe>(G.SurfaceScalars);
        auto Region=MakeShared<FUpdateTextureRegion2D,ESPMode::ThreadSafe>(0,0,0,0,G.ScalarTextureSize.X,G.ScalarTextureSize.Y);
        ScalarTexture->UpdateTextureRegions(0,1,&Region.Get(),G.ScalarTextureSize.X*sizeof(float),sizeof(float),
            reinterpret_cast<uint8*>(Upload->GetData()),[Upload,Region](uint8*,const FUpdateTextureRegion2D*){});
        ScalarInstance->SetScalarParameterValue(TEXT("Palette"),G.ColorMapping.Palette);
        ScalarInstance->SetVectorParameterValue(TEXT("LowColor"),G.ColorMapping.LowColor);
        ScalarInstance->SetVectorParameterValue(TEXT("MiddleColor"),G.ColorMapping.MiddleColor);
        ScalarInstance->SetVectorParameterValue(TEXT("HighColor"),G.ColorMapping.HighColor);
    }
    else if(ScalarTexture)
    {
        if(ScalarInstance)ScalarInstance->SetTextureParameterValue(TEXT("SourceScalars"),nullptr);
        if(FocusScalarInstance)FocusScalarInstance->SetTextureParameterValue(TEXT("SourceScalars"),nullptr);
        ScalarTexture=nullptr;
    }
    for(auto* Instance:{InspectionInstance.Get(),OriginalSliceInstance.Get(),FocusScalarInstance.Get()})if(Instance)
    {
        Instance->SetScalarParameterValue(TEXT("Palette"),G.ColorMapping.Palette);
        Instance->SetVectorParameterValue(TEXT("LowColor"),G.ColorMapping.LowColor);
        Instance->SetVectorParameterValue(TEXT("MiddleColor"),G.ColorMapping.MiddleColor);
        Instance->SetVectorParameterValue(TEXT("HighColor"),G.ColorMapping.HighColor);
    }
    if(OriginalSliceInstance)
    {
        OriginalSliceInstance->SetScalarParameterValue(TEXT("UseVelocity"),G.Scalar.Id==TEXT("velocity_magnitude")?1:0);
        OriginalSliceInstance->SetScalarParameterValue(TEXT("RangeMinimum"),G.ColorMapping.Minimum);
        OriginalSliceInstance->SetScalarParameterValue(TEXT("RangeSpan"),G.ColorMapping.Maximum-G.ColorMapping.Minimum);
    }
    GeometrySliceNotices=G.SliceNotices;
    if(!InspectionInstance)
        for(const auto& Id:G.RenderedSlices)
            GeometrySliceNotices.Add(Id,TEXT("Slice rendering is unavailable. Repair or reinstall the application to restore its rendering assets."));
    for(int32 I=0;I<13;++I)
    {
        if((I==8||(I==2&&G.bOriginalSlice))&&!InspectionInstance){Mesh->ClearMeshSection(I);continue;}
        const auto& S=G.Sections[I]; if(S.Vertices.IsEmpty()) { Mesh->ClearMeshSection(I); continue; }
        TArray<FVector> Normals=S.Normals;if(Normals.IsEmpty())Normals.Init(FVector::UpVector,S.Vertices.Num());
        Mesh->CreateMeshSection_LinearColor(I,S.Vertices,S.Indices,Normals,S.UVs,S.VelocityUVs,{},{},S.Colors,TArray<FProcMeshTangent>(),false,false);
        if(I!=7)Mesh->SetMaterial(I,I==10?TransparentMaterial.Get():I==11||I==12?OpaqueMaterial.Get():I==0&&G.bAirfoilSolid&&BodyMaterial?BodyMaterial.Get():I==9?MeshEdgeMaterial.Get():I==8?InspectionInstance.Get():(I==2&&G.bOriginalSlice)?OriginalSliceInstance.Get():I==2&&bSurface?(G.bFocusedSurface?FocusScalarInstance.Get():ScalarInstance.Get()):(I==1||I==2?TransparentMaterial.Get():OpaqueMaterial.Get()));
    }
    RenderMilliseconds=G.BuildMs;
    if(!G.Dataset.IsEmpty())RenderedFlowBounds=G.Bounds;
    RenderedDataset=G.Error.IsEmpty()?G.Dataset:FString(); RenderedTitle=G.Title; GeometryFrame=G.Frame;GeometryScalar=G.Scalar;GeometryColorMapping=G.ColorMapping;
    GeometryVectors=G.Vectors;GeometryStreams=G.Streams;GeometryMesh=G.Mesh;
    RenderedField=G.Error.IsEmpty()?G.Field:nullptr;
    if(!G.Error.IsEmpty()&&Model->Notice!=G.Error) { Model->Notice=G.Error; Model->AddLog(G.Error,EStudioLogSeverity::Error); }
    // Captures are on demand, so there may be no later frame to retire an
    // occlusion result from the previous mesh or the empty Geometry preview.
    // Replacing the scene invalidates that history even at an unchanged camera.
    Capture->bCameraCutThisFrame=true;
    bCaptureDirty=true;
    UE_LOG(LogTemp,Verbose,TEXT("Studio geometry applied: %d wing vertices, %.1f ms"),G.Sections[0].Vertices.Num(),G.BuildMs);
}
void AStudioScene::Tick(float Delta)
{
    Super::Tick(Delta); if(!Model) return; Model->Tick(Delta);
    bool bMinimized=false;
    if(GEngine&&GEngine->GameViewport)
        if(const auto Window=GEngine->GameViewport->GetWindow())bMinimized=Window->IsWindowMinimized();
    const bool WantsBoundary=Model->Workspace==EStudioWorkspace::BoundaryConditions;
    const bool WantsLattice=Model->Workspace==EStudioWorkspace::Meshing;
    const bool WantsDomain=Model->Workspace==EStudioWorkspace::Domain||WantsBoundary||WantsLattice;
    const bool WantsGeometry=Model->Workspace==EStudioWorkspace::Geometry||WantsDomain;
    const int32 WantedPreviewRevision=WantsDomain?Model->DomainPreviewRevision:Model->GeometryRevision;
    const bool WantsFlow=Model->Workspace==EStudioWorkspace::Solve&&!Model->bActivityLogExpanded&&!bMinimized&&(!ViewVisibility||ViewVisibility());
    if(bBuilding&&(!WantsFlow||!IsGeometryRequestCurrent()))CancelBuild(GeometryCancellation);
    if(PendingPreview.IsValid()&&(!WantsGeometry||bMinimized||BuildingPreviewRevision!=WantedPreviewRevision||bBuildingDomainPreview!=WantsDomain||bBuildingBoundaryPreview!=WantsBoundary||bBuildingLatticePreview!=WantsLattice||BuildingPreviewProjectId!=Model->Project.Id))CancelBuild(PreviewCancellation);
    if(WantsGeometry!=bGeometryView||WantsDomain!=bDomainView||WantsBoundary!=bBoundaryView||WantsLattice!=bLatticeView)
    {
        if(bGeometryView)(bDomainView?DomainCamera:GeometryCamera)=CameraState();bGeometryView=WantsGeometry;bDomainView=WantsDomain;bBoundaryView=WantsBoundary;bLatticeView=WantsLattice;
        Mesh->ClearAllMeshSections();VolumeComponent->ClearVolume();PresentedDataset.Empty();RenderedDataset.Empty();PreviewRevision=-1;RenderedRevision=CapturedRevision=-1;
        RenderedField.Reset();CapturedField.Reset();
        ApplyCamera(bGeometryView?(bDomainView?DomainCamera:GeometryCamera):Model->Project.Camera);bFitPreview=bGeometryView&&(!bDomainView||DomainCameraProject!=Model->Project.Id);
    }
    if(bBuilding&&PendingGeometry.IsReady())
    {
        auto G=PendingGeometry.Get(); PendingGeometry=TFuture<TSharedPtr<FStudioGeometry>>(); bBuilding=false;
        if(G&&WantsFlow&&IsGeometryRequestCurrent()&&!GeometryCancellation->load())
        {
            ApplyGeometry(*G);RenderedRevision=BuildingRevision;RenderedIntentRevision=BuildingIntentRevision;
            RenderedProjectId=BuildingProjectId;RenderedSolver=BuildingSolver;
        }
        else ++DiscardedBuildCount;
        GeometryCancellation.Reset();BuildingSolver.Reset();
    }
    // Drain obsolete preview work even when its workspace is hidden. A cancelled
    // worker must release its buffers before either pipeline starts another.
    if(PendingPreview.IsValid()&&PreviewCancellation->load()&&PendingPreview.IsReady())
    {PendingPreview.Get();PendingPreview={};PreviewCancellation.Reset();++DiscardedBuildCount;}
    if(bMinimized)return;
    if(bGeometryView){if(bDomainView)UpdateDomainPreview();else UpdateGeometryPreview();CaptureIfChanged();return;}
    if(AppliedCameraRevision!=Model->CameraRevision) ApplyCamera(Model->Project.Camera);
    // Background playback advances its cursor without hidden mesh/capture work.
    if(!WantsFlow)return;
    if(!bBuilding&&(RenderedRevision!=Model->Revision||RenderedIntentRevision!=Model->RenderIntentRevision||
        RenderedProjectId!=Model->Project.Id||RenderedSolver.Pin()!=Model->Solver))RequestGeometry();
    CaptureIfChanged();
}
FStudioSceneResourceStats AStudioScene::ResourceStats() const
{
    FStudioSceneResourceStats Stats;
    Stats.Workers=int32(PendingGeometry.IsValid())+int32(PendingPreview.IsValid());
    Stats.CancelledBuilds=CancelledBuildCount;Stats.DiscardedBuilds=DiscardedBuildCount;
    if(RenderTarget)Stats.RenderTargetBytes=RenderTarget->GetResourceSizeBytes(EResourceSizeMode::Exclusive);
    if(ScalarTexture)Stats.ScalarTextureBytes=ScalarTexture->GetResourceSizeBytes(EResourceSizeMode::Exclusive);
    if(VolumeComponent)Stats.ScalarTextureBytes+=VolumeComponent->TextureBytes();
    for(auto* Component:{Mesh.Get(),Backdrop.Get()})if(Component)for(int32 I=0;I<Component->GetNumSections();++I)
        if(const auto* S=Component->GetProcMeshSection(I);S&&!S->ProcVertexBuffer.IsEmpty())
        {
            ++Stats.Sections;Stats.Vertices+=S->ProcVertexBuffer.Num();Stats.Indices+=S->ProcIndexBuffer.Num();
            Stats.MeshBytes+=S->ProcVertexBuffer.GetAllocatedSize()+S->ProcIndexBuffer.GetAllocatedSize();
        }
    return Stats;
}
void AStudioScene::CaptureIfChanged()
{
    if(!RenderTarget) return;
    const FTransform Transform=Capture->GetComponentTransform();
    if(!bCaptureDirty&&Transform.Equals(LastCapturedTransform)) return;
    UE_LOG(LogTemp,Verbose,TEXT("Studio capture %llu: dirty=%d cameraChanged=%d geometry=%d model=%d frame=%d target=%dx%d position=%s rotation=%s"),
        CaptureCount+1,bCaptureDirty,!Transform.Equals(LastCapturedTransform),RenderedRevision,Model?Model->Revision:-1,
        Model?Model->SelectedFrame:-1,RenderTarget->SizeX,RenderTarget->SizeY,
        *Transform.GetLocation().ToString(),*Transform.Rotator().ToString());
    UpdateProjection();
    if(VolumeComponent)VolumeComponent->CameraChanged(CameraState(),FIntPoint(RenderTarget->SizeX,RenderTarget->SizeY),
        Capture->bOverride_CustomNearClippingPlane?Capture->CustomNearClippingPlane:GNearClippingPlane);
    // CaptureScene flushes deferred component changes before rendering. Deferred
    // captures depend on a main world view, which this Slate application omits.
    const double CaptureStart=FPlatformTime::Seconds();
    UpdateBackdrop();
    Capture->CaptureScene();
    CaptureSubmitMs=(FPlatformTime::Seconds()-CaptureStart)*1000.;
    CapturedBuildMs=RenderMilliseconds;
    PresentedDataset=RenderedDataset; PresentedTitle=RenderedTitle; CapturedFrame=GeometryFrame;CapturedScalar=GeometryScalar;CapturedColorMapping=GeometryColorMapping;
    CapturedVectors=GeometryVectors;CapturedStreams=GeometryStreams;CapturedMesh=GeometryMesh;
    CapturedField=RenderedField;CapturedSliceNotices=GeometrySliceNotices;
    CapturedRevision=RenderedRevision;CapturedIntentRevision=RenderedIntentRevision;
    CapturedProjectId=RenderedProjectId;CapturedSolver=RenderedSolver;
    CapturedCamera=CameraState();CapturedViewportSize=FIntPoint(RenderTarget->SizeX,RenderTarget->SizeY);
    CapturedNearClipMeters=CapturedCamera.bDepthClipping?CapturedCamera.NearClipMeters:CapturedCamera.bOrthographic?0.:
        (Capture->bOverride_CustomNearClippingPlane?Capture->CustomNearClippingPlane:GNearClippingPlane)/100.;
    LastCapturedTransform=Transform; bCaptureDirty=false; ++CaptureCount;
}
void AStudioScene::EndPlay(const EEndPlayReason::Type Reason)
{
    CapturedField.Reset();RenderedField.Reset();
    CancelBuild(GeometryCancellation);CancelBuild(PreviewCancellation);
    if(PendingGeometry.IsValid()) PendingGeometry.Wait();
    if(PendingPreview.IsValid())PendingPreview.Wait();
    PendingGeometry={};PendingPreview={};BuildingSolver.Reset();RenderedSolver.Reset();CapturedSolver.Reset();
    FrozenPipelineOutput.Reset();
    ViewVisibility={};
    // Comparison views may be replaced many times before the next GC. Release
    // their pinned original arrays as soon as the scene is destroyed.
    if(Model&&Model->IsSnapshotView())
    {
        Mesh->ClearAllMeshSections();Backdrop->ClearAllMeshSections();VolumeComponent->ClearVolume(true);
        if(ScalarInstance)ScalarInstance->SetTextureParameterValue(TEXT("SourceScalars"),nullptr);
        if(FocusScalarInstance)FocusScalarInstance->SetTextureParameterValue(TEXT("SourceScalars"),nullptr);
        if(ScalarTexture){ScalarTexture->ReleaseResource();ScalarTexture=nullptr;}
        Capture->TextureTarget=nullptr;
        if(RenderTarget){RenderTarget->ReleaseResource();RenderTarget=nullptr;}
        Model.Reset();
    }
    Super::EndPlay(Reason);
}

void AStudioScene::UpdateGeometryPreview()
{
    if(PendingPreview.IsValid()&&PendingPreview.IsReady())
    {
        const auto G=PendingPreview.Get();PendingPreview=TFuture<TSharedPtr<FStudioGeometry>>();
        if(G&&!PreviewCancellation->load()&&!bBuildingDomainPreview&&BuildingPreviewRevision==Model->GeometryRevision&&BuildingPreviewProjectId==Model->Project.Id)
        {
            ApplyGeometry(*G);PreviewBounds=G->Bounds;PreviewRevision=BuildingPreviewRevision;
            if(bFitPreview){FitCamera();bFitPreview=false;}
        }
        else ++DiscardedBuildCount;
        PreviewCancellation.Reset();
    }
    if(bBuilding||PendingPreview.IsValid()||PreviewRevision==Model->GeometryRevision)return;
    const auto Source=Model->GeometrySource.Mesh;
    if(!Source){Mesh->ClearAllMeshSections();PreviewBounds=FBox(ForceInit);PreviewRevision=Model->GeometryRevision;bCaptureDirty=true;bFitPreview=true;return;}
    FStudioGeometryAsset Asset;FString Error;
    if(!Model->GeometryAssetForPreview(Asset,Error))
    {
        // Before unit selection show the original shape, clearly labelled in UI.
        if(!Model->bImportPreview)return;
        Asset.MetersPerSourceUnit=Model->ImportOptions.MetersPerUnit>0?Model->ImportOptions.MetersPerUnit:1.;
        Asset.Rotation=StudioMeshImport::AxisRotation(Model->ImportOptions.UpAxis,Model->ImportOptions.ForwardAxis);
    }
    bBuildingDomainPreview=false;bBuildingBoundaryPreview=false;bBuildingLatticePreview=false;BuildingPreviewRevision=Model->GeometryRevision;
    BuildingPreviewProjectId=Model->Project.Id;
    PreviewCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    bFitPreview=true;
    PendingPreview=Async(EAsyncExecution::ThreadPool,[Source,Asset,Cancel=PreviewCancellation]() -> TSharedPtr<FStudioGeometry>
    {
        if(Cancel->load())return {};
        auto G=MakeShared<FStudioGeometry>();auto& S=G->Sections[0];S.Indices=Source->Indices;
        S.Vertices.Reserve(Source->Positions.Num());
        int32 Converted=0;
        for(const auto& P:Source->Positions)
        {if((Converted++&1023)==0&&Cancel->load())return {};const FVector V=StudioMeshImport::TransformPosition(P,Asset);G->Bounds+=V;S.Vertices.Add(V*100.);}
        S.Normals.Init(FVector::ZeroVector,S.Vertices.Num());S.Colors.Init(FLinearColor(.22,.43,.52),S.Vertices.Num());
        for(int32 I=0;I<S.Indices.Num();I+=3)
        {
            if((I&1023)==0&&Cancel->load())return {};
            const int32 A=S.Indices[I],B=S.Indices[I+1],C=S.Indices[I+2];
            const FVector N=FVector::CrossProduct(S.Vertices[B]-S.Vertices[A],S.Vertices[C]-S.Vertices[A]);
            S.Normals[A]+=N;S.Normals[B]+=N;S.Normals[C]+=N;
        }
        for(int32 I=0;I<S.Normals.Num();++I)
        {if((I&1023)==0&&Cancel->load())return {};auto& N=S.Normals[I];N.Normalize();const double Shade=.35+.65*FMath::Abs(FVector::DotProduct(N,FVector(.3,.5,.8).GetSafeNormal()));S.Colors[I]=FLinearColor(.27,.55,.65)*Shade;S.Colors[I].A=1;}
        return G;
    });
}
void AStudioScene::UpdateDomainPreview()
{
    if(DomainCameraProject!=Model->Project.Id)bFitPreview=true;
    if(PendingPreview.IsValid()&&PendingPreview.IsReady())
    {
        const auto G=PendingPreview.Get();PendingPreview={};
        if(G&&!PreviewCancellation->load()&&bBuildingDomainPreview&&bBuildingBoundaryPreview==bBoundaryView&&bBuildingLatticePreview==bLatticeView&&BuildingPreviewRevision==Model->DomainPreviewRevision&&BuildingPreviewProjectId==Model->Project.Id)
        {
            ApplyGeometry(*G);PreviewBounds=G->Bounds;PreviewRevision=BuildingPreviewRevision;
            if(bFitPreview){FitCamera();bFitPreview=false;DomainCameraProject=Model->Project.Id;}
        }
        else ++DiscardedBuildCount;
        PreviewCancellation.Reset();
    }
    if(bBuilding||PendingPreview.IsValid()||PreviewRevision==Model->DomainPreviewRevision)return;
    bBuildingDomainPreview=true;bBuildingBoundaryPreview=bBoundaryView;bBuildingLatticePreview=bLatticeView;BuildingPreviewRevision=Model->DomainPreviewRevision;BuildingPreviewProjectId=Model->Project.Id;
    PreviewCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const auto Domain=Model->Project.Draft.Domain;
    const FGuid SelectedPatch=bBoundaryView?Model->SelectedBoundaryTarget:FGuid();
    const int32 Face=bLatticeView?INDEX_NONE:bBoundaryView?Domain.Faces.IndexOfByKey(SelectedPatch):Model->SelectedDomainFace;
    const auto Checked=Model->DomainGeometry;
    const auto Lattice=bLatticeView?Model->LatticePreview:nullptr;
    PendingPreview=Async(EAsyncExecution::ThreadPool,[Domain,Face,SelectedPatch,Checked,Lattice,Cancel=PreviewCancellation]() -> TSharedPtr<FStudioGeometry>
    {
        if(Cancel->load())return {};
        auto G=MakeShared<FStudioGeometry>();G->Bounds=FBox(Domain.Min,Domain.Max);
        auto& S=G->Sections[0];
        if(Checked&&Checked->Preview)
        {
            const auto Source=Checked->Preview;
            if(Checked->Bounds.IsValid)G->Bounds+=Checked->Bounds;
            S.Indices=Source->Indices;S.Vertices.Reserve(Source->Positions.Num());
            int32 I=0;for(const auto& Position:Source->Positions)
            {if((I++&1023)==0&&Cancel->load())return {};S.Vertices.Add(Position*100.);}
            S.Normals.Init(FVector::ZeroVector,S.Vertices.Num());S.Colors.Init(FLinearColor(.22,.43,.52),S.Vertices.Num());
            for(I=0;I<S.Indices.Num();I+=3)
            {
                if((I&1023)==0&&Cancel->load())return {};
                const int32 A=S.Indices[I],B=S.Indices[I+1],C=S.Indices[I+2];
                const FVector N=FVector::CrossProduct(S.Vertices[B]-S.Vertices[A],S.Vertices[C]-S.Vertices[A]);
                S.Normals[A]+=N;S.Normals[B]+=N;S.Normals[C]+=N;
            }
            for(I=0;I<S.Normals.Num();++I)
            {
                if((I&1023)==0&&Cancel->load())return {};
                auto& N=S.Normals[I];N.Normalize();
                const double Shade=.35+.65*FMath::Abs(FVector::DotProduct(N,FVector(.3,.5,.8).GetSafeNormal()));
                S.Colors[I]=FLinearColor(.27,.55,.65)*Shade;S.Colors[I].A=1;
            }
            if(SelectedPatch.IsValid()&&Checked->PatchBounds.Contains(SelectedPatch)&&Checked->TriangleTargets.Num()==Source->Indices.Num()/3)
            {
                // Split the displayed faces so a shared edge cannot leak the
                // selection color onto a neighboring original surface patch.
                auto& Highlight=G->Sections[4];TArray<int32> Retained;Retained.Reserve(S.Indices.Num());
                TArray<int32> HighlightVertices;HighlightVertices.Init(INDEX_NONE,S.Vertices.Num());
                auto AddVertex=[&](int32 Original)
                {
                    int32& Mapped=HighlightVertices[Original];
                    if(Mapped==INDEX_NONE)
                    {
                        Mapped=Highlight.Vertices.Add(S.Vertices[Original]);Highlight.Normals.Add(S.Normals[Original]);
                        Highlight.Colors.Add(FLinearColor(0,.75,.9));
                    }
                    Highlight.Indices.Add(Mapped);
                };
                for(int32 Triangle=0;Triangle<Checked->TriangleTargets.Num();++Triangle)
                {
                    if((Triangle&1023)==0&&Cancel->load())return {};
                    const int32 A=S.Indices[Triangle*3],B=S.Indices[Triangle*3+1],C=S.Indices[Triangle*3+2];
                    if(Checked->TriangleTargets[Triangle]==SelectedPatch)
                    {AddVertex(A);AddVertex(B);AddVertex(C);}
                    else {Retained.Add(A);Retained.Add(B);Retained.Add(C);}
                }
                S.Indices=MoveTemp(Retained);
            }
        }
        FVector Corners[8];for(int32 I=0;I<8;++I)Corners[I]=FVector(I&1?Domain.Max.X:Domain.Min.X,I&2?Domain.Max.Y:Domain.Min.Y,I&4?Domain.Max.Z:Domain.Min.Z)*100.;
        const double Radius=FMath::Max(.00001,(Domain.Max-Domain.Min).GetMax()*.08);
        for(int32 I=0;I<8;++I)for(int32 Axis=0;Axis<3;++Axis)if(!(I&(1<<Axis)))
            G->Sections[3].Tube(Corners[I],Corners[I|(1<<Axis)],Radius,FLinearColor(.28,.43,.51));
        if(Lattice&&Lattice->Complete())
        {
            int32 I=0;auto& Cells=G->Sections[1];
            const int32 LayerAxis=Lattice->Plan.Settings.Axis;
            for(const auto& Cell:Lattice->Samples)
            {
                if((I++&127)==0&&Cancel->load())return {};
                FVector Center;FBox Box;if(!Lattice->Plan.Layout.Cell(Cell.Index,Center,Box))continue;
                FLinearColor Color;
                switch(Cell.Kind)
                {
                case EStudioLatticeCell::OutsideGeometry:Color=FLinearColor(.08,.48,.62,.45);break;
                case EStudioLatticeCell::InsideClosedGeometry:Color=FLinearColor(.48,.38,.7,.75);break;
                case EStudioLatticeCell::Surface:Color=FLinearColor(1,.65,.15,.85);break;
                default:Color=FLinearColor(.7,.28,.34,.7);break;
                }
                // Three percent display gap reveals original cells. Sampling
                // never coarsens or changes their physical positions or sizes.
                const FVector Extent=Box.GetExtent()*.97*100.;Center*=100.;
                auto Quad=[&](int32 Axis,double Side)
                {
                    const int32 A=(Axis+1)%3,B=(Axis+2)%3;FVector P[4];
                    for(int32 K=0;K<4;++K)
                    {P[K]=Center;P[K][Axis]+=Side*Extent[Axis];P[K][A]+=(K==1||K==2?1:-1)*Extent[A];P[K][B]+=(K>=2?1:-1)*Extent[B];}
                    Cells.Triangle(P[0],P[1],P[2],Color);Cells.Triangle(P[0],P[2],P[3],Color);
                };
                if(LayerAxis>=0)Quad(LayerAxis,0);
                else for(int32 Axis=0;Axis<3;++Axis){Quad(Axis,-1);Quad(Axis,1);}
            }
        }
        if(Face==INDEX_NONE)return G;
        const int32 Axis=FMath::Clamp(Face/2,0,2),Side=Face%2,A=(Axis+1)%3,B=(Axis+2)%3;
        const int32 Base=Side<<Axis;
        const int32 Indices[4]={Base,Base|(1<<A),Base|(1<<A)|(1<<B),Base|(1<<B)};
        for(int32 I=0;I<4;++I)G->Sections[4].Tube(Corners[Indices[I]],Corners[Indices[(I+1)%4]],Radius*2,FLinearColor(0,.75,.9));
        G->Sections[1].Triangle(Corners[Indices[0]],Corners[Indices[1]],Corners[Indices[2]],FLinearColor(0,.65,.8,.07));
        G->Sections[1].Triangle(Corners[Indices[0]],Corners[Indices[2]],Corners[Indices[3]],FLinearColor(0,.65,.8,.07));
        return G;
    });
}
bool AStudioScene::CaptureSnapshot(FStudioSnapshot& Out,const FStudioProbeMarkerResult* Markers,FString& Error)
{
    check(IsInGameThread());
    if(!StudioSnapshot::ValidSize(Out.Options.Size))
    {Error=TEXT("Choose image dimensions from 64 to 4096 pixels, up to 16 megapixels.");return false;}
    if(!HasCurrentFrame()||!HasPresentedFrame()||!CapturedField)
    {Error=TEXT("Wait for the requested flow frame before saving a snapshot.");return false;}
    CaptureIfChanged();const auto Identity=CapturedField->Identity();
    Out.Pixels.Reset();
    if(!Identity.IsSet()){Error=TEXT("The displayed field has no recorded frame identity.");return false;}
    Out.Identity=*Identity;Out.Project=CapturedProjectId;Out.Capture=CaptureCount;Out.SourceTitle=PresentedTitle;
    Out.Camera=CapturedCamera;Out.SourceSize=CapturedViewportSize;Out.Framing=StudioSnapshot::Frame(Out.SourceSize,Out.Options.Size);
    Out.Scalar=CapturedScalar;Out.Mapping=CapturedColorMapping;
    Out.UnitDisplay=Model->UnitDisplay;Out.SourceUnitMap.Reset();
    if(const auto V=CapturedField->VolumeReconstruction();V&&V->OriginalGrid)Out.SourceUnitMap=V->OriginalGrid->UnitContext();
    Out.OriginalSourceJSON=StudioSnapshot::SourceMetadata(*CapturedField,CapturedScalar.Id,CapturedColorMapping,Model->bHome4AirMask);Out.Objects=Model->InspectionObjects;Out.SelectedObject=Model->SelectedInspectionObject;
    Out.DisplaySettings=static_cast<const FStudioViewSettings&>(*Model);Out.FlowBounds=RenderedFlowBounds;Out.SliceNotices=CapturedSliceNotices;
    Out.Vectors=CapturedVectors;Out.Streams=CapturedStreams;Out.Mesh=CapturedMesh;
    Out.bVolumeRendererActive=VolumeComponent&&VolumeComponent->IsVisible()&&VolumeComponent->TextureBytes()>0;
    Out.Overlay=StudioInspectionOverlay::Build(Out.Objects,{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256},
        Out.Project,Out.SelectedObject,RenderedFlowBounds,Markers,Identity->SpatialDimensions,Identity->SourceOffset.Y,StudioStreamlines::DomainBounds(*CapturedField,RenderedFlowBounds));
    const TStrongObjectPtr<UTextureRenderTarget2D> Target(NewObject<UTextureRenderTarget2D>(this));
    Target->ClearColor=RenderTarget->ClearColor;Target->InitCustomFormat(Out.Options.Size.X,Out.Options.Size.Y,PF_B8G8R8A8,false);Target->UpdateResourceImmediate(true);
    // An independent on-demand capture has no persistent view state. Exporting
    // 4K must not resize and retain the live camera's temporal render buffers.
    const TStrongObjectPtr<USceneCaptureComponent2D> SnapshotCapture(NewObject<USceneCaptureComponent2D>(this));
    SnapshotCapture->bCaptureEveryFrame=false;SnapshotCapture->bCaptureOnMovement=false;SnapshotCapture->bAlwaysPersistRenderingState=false;
    SnapshotCapture->CaptureSource=Capture->CaptureSource;SnapshotCapture->PrimitiveRenderMode=Capture->PrimitiveRenderMode;
    SnapshotCapture->ShowFlags=Capture->ShowFlags;SnapshotCapture->PostProcessSettings=Capture->PostProcessSettings;
    SnapshotCapture->PostProcessBlendWeight=Capture->PostProcessBlendWeight;
    SnapshotCapture->RegisterComponent();SnapshotCapture->SetWorldTransform(Capture->GetComponentTransform());SnapshotCapture->ShowOnlyActorComponents(this);
    const double DefaultNear=Capture->bOverride_CustomNearClippingPlane?Capture->CustomNearClippingPlane:GNearClippingPlane;
    ON_SCOPE_EXIT
    {
        if(VolumeComponent)VolumeComponent->CameraChanged(CapturedCamera,CapturedViewportSize,DefaultNear);
        SnapshotCapture->DestroyComponent();
        Target->ReleaseResource();
    };
    auto Cropped=Out.Camera;Cropped.OrthoWidth*=Out.Framing.Span.X;
    const double HalfFOV=FMath::Atan(FMath::Tan(FMath::DegreesToRadians(Cropped.FieldOfView*.5))*Out.Framing.Span.X);
    Cropped.FieldOfView=FMath::RadiansToDegrees(2*HalfFOV);
    const double Aspect=double(Out.Options.Size.X)/Out.Options.Size.Y;
    const double Near=Cropped.bDepthClipping?Cropped.NearClipMeters*100.:Cropped.bOrthographic?0:DefaultNear;
    // Match SceneCaptureRendering::BuildOrthoMatrix. A much farther automatic
    // plane loses depth precision and changes inside-volume ray origins.
    const double Far=Cropped.bDepthClipping?Cropped.FarClipMeters*100.:Cropped.bOrthographic?UE_FLOAT_HUGE_DISTANCE/4.:Near;
    Out.Projection=Cropped.bOrthographic?FMatrix(FReversedZOrthoMatrix(Cropped.OrthoWidth*50.,Cropped.OrthoWidth*50./Aspect,1./(Far-Near),-Near)):
        FMatrix(FReversedZPerspectiveMatrix(HalfFOV,HalfFOV,1.,Aspect,Near,Far));
    SnapshotCapture->TextureTarget=Target.Get();SnapshotCapture->bUseCustomProjectionMatrix=true;SnapshotCapture->CustomProjectionMatrix=Out.Projection;
    SnapshotCapture->FOVAngle=Cropped.FieldOfView;SnapshotCapture->OrthoWidth=Cropped.OrthoWidth*100.;
    SnapshotCapture->ProjectionType=Cropped.bOrthographic?ECameraProjectionMode::Orthographic:ECameraProjectionMode::Perspective;
    SnapshotCapture->bOverride_CustomNearClippingPlane=true;SnapshotCapture->CustomNearClippingPlane=Near;
    if(VolumeComponent)VolumeComponent->CameraChanged(Cropped,Out.Options.Size,DefaultNear);
    SnapshotCapture->CaptureScene();
    if(Out.Options.bAnnotations||Out.Options.bLegend||Out.Options.bFrameInfo)
    {
        auto* Renderer=new FWidgetRenderer(false,false);
        Renderer->SetApplyColorDeficiencyCorrection(false);
        Renderer->DrawWidget(Target.Get(),SNew(SStudioSnapshotOverlay).Snapshot(Out),FVector2D(Out.Options.Size),0);
        const bool Read=Target->GameThread_GetRenderTargetResource()->ReadPixels(Out.Pixels);
        BeginCleanup(Renderer);
        if(!Read){Error=TEXT("Could not read the annotated snapshot image.");return false;}
    }
    else if(!Target->GameThread_GetRenderTargetResource()->ReadPixels(Out.Pixels))
    {Error=TEXT("Could not read the snapshot image.");return false;}
    for(auto& Pixel:Out.Pixels)Pixel.A=255;
    Error.Empty();return true;
}
bool AStudioScene::Snapshot(const FString& Path)
{
    // Do not save a previous frame and silently label it as the requested one.
    if(!HasCurrentFrame()||!HasPresentedFrame()) return false;
    CaptureIfChanged();
    TArray<FColor> Pixels; if(!RenderTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels)) return false;
    for(FColor& Pixel:Pixels) Pixel.A=255;
    TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(RenderTarget->SizeX,RenderTarget->SizeY,Pixels,PNG);
    return FFileHelper::SaveArrayToFile(PNG,*Path);
}
AStudioGameMode::AStudioGameMode() { DefaultPawnClass=nullptr; }
void AStudioGameMode::BeginPlay()
{
    Super::BeginPlay();
    const bool bAutomation=FParse::Param(FCommandLine::Get(),TEXT("StudioAutomation"));
    const auto M=MakeShared<FStudioModel>(bAutomation?FPaths::ProjectSavedDir()/TEXT("Automation/Session"):FString());
    if(!bAutomation){M->OpenSession();if(M->ProjectPath.IsEmpty()&&!M->IsProjectOpenPending()&&M->PendingRecovery.IsEmpty())M->Workspace=EStudioWorkspace::Validation;}
#if WITH_DEV_AUTOMATION_TESTS
    // Native relaunch checks reuse only the isolated acceptance session.
    else if(FParse::Param(FCommandLine::Get(),TEXT("StudioRestoreAutomationSession")))M->OpenSession();
#endif
    Scene=GetWorld()->SpawnActor<AStudioScene>(); Scene->Initialize(M);
    if(GEngine&&GEngine->GameViewport)
    {
        // The workspace covers the game viewport and owns its camera texture.
        // Rendering a second world view behind it wastes GPU work even at rest.
        bPreviousWorldRenderingDisabled=GEngine->GameViewport->bDisableWorldRendering;
        GEngine->GameViewport->bDisableWorldRendering=true;
        Workspace=SNew(SStudioWorkspace).Model(M).Scene(Scene);
        GEngine->GameViewport->AddViewportWidgetContent(Workspace.ToSharedRef(),10);
        GEngine->GameViewport->OnWindowCloseRequested().BindSP(Workspace.ToSharedRef(),&SStudioWorkspace::CanClose);
    }
    if(auto PC=GetWorld()->GetFirstPlayerController()) { PC->bShowMouseCursor=true; PC->SetInputMode(FInputModeUIOnly()); }
}
void AStudioGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
    if(Workspace&&GEngine&&GEngine->GameViewport)
    {
        if(Scene && Scene->Model) { Scene->Model->Project.Camera=Scene->SavedCameraState(); Scene->Model->WriteRecovery(); }
        GEngine->GameViewport->OnWindowCloseRequested().Unbind();
        GEngine->GameViewport->RemoveViewportWidgetContent(Workspace.ToSharedRef());
        GEngine->GameViewport->bDisableWorldRendering=bPreviousWorldRenderingDisabled;
    }
    Workspace.Reset(); Super::EndPlay(Reason);
}
