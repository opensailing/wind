#include "SStudioHome4Authoring.h"
#include "StudioHome4Readouts.h"
#include "StudioHome4Body.h"
#include "StudioHome4Setup.h"
#include "StudioColor.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Rendering/DrawElements.h"
#include "InputCoreTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
namespace StudioHome4AuthoringWidgetLocal
{
class SGeometry final:public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SGeometry){} SLATE_ARGUMENT(TSharedPtr<FStudioHome4AuthoringSession>,Session)
        SLATE_ARGUMENT(TFunction<FString()>,Mode) SLATE_ARGUMENT(TFunction<double()>,Step) SLATE_ARGUMENT(TFunction<int32()>,Link) SLATE_END_ARGS()
    void Construct(const FArguments& A){Session=A._Session;Mode=A._Mode;Step=A._Step;Link=A._Link;SetToolTipText(FText::FromString(TEXT("Drag to orbit actual prepared geometry. All coordinates are next-run source XYZ root cells. Sampled SDF nodes retain original indices. This is a geometric preview, not a CFD field.")));}
    FVector2D ComputeDesiredSize(float)const override{return FVector2D(420,240);}
    FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent& E)override
    {if(E.GetEffectingButton()==EKeys::LeftMouseButton||E.GetEffectingButton()==EKeys::RightMouseButton){bPan=E.GetEffectingButton()==EKeys::RightMouseButton;return FReply::Handled().CaptureMouse(SharedThis(this));}return FReply::Unhandled();}
    FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& E)override
    {if(E.GetEffectingButton()==EKeys::LeftMouseButton||E.GetEffectingButton()==EKeys::RightMouseButton)return FReply::Handled().ReleaseMouseCapture();return FReply::Unhandled();}
    FReply OnMouseMove(const FGeometry&,const FPointerEvent& E)override
    {if(!HasMouseCapture())return FReply::Unhandled();auto& V=Session->Camera();if(bPan)V.Pan+=E.GetCursorDelta();else{V.Yaw+=E.GetCursorDelta().X*.008;V.Pitch=FMath::Clamp(V.Pitch-E.GetCursorDelta().Y*.008,-1.4,1.4);}Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();}
    FReply OnMouseWheel(const FGeometry&,const FPointerEvent& E)override
    {auto& V=Session->Camera();V.Zoom=FMath::Clamp(V.Zoom*FMath::Pow(1.2,E.GetWheelDelta()),.1,20.);Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();}
    FReply OnKeyDown(const FGeometry&,const FKeyEvent& E)override
    {auto& V=Session->Camera();if(E.GetKey()==EKeys::One){V.Yaw=0;V.Pitch=PI/2;}else if(E.GetKey()==EKeys::Two){V.Yaw=-PI/2;V.Pitch=0;}else if(E.GetKey()==EKeys::Three){V.Yaw=0;V.Pitch=0;}else if(E.GetKey()==EKeys::Home){V=FStudioHome4AuthoringCamera();}else return FReply::Unhandled();Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();}
    bool SupportsKeyboardFocus()const override{return true;}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool)const override
    {
        const auto P=Session->Preview();const auto& Camera=Session->Camera();const double Yaw=Camera.Yaw,Pitch=Camera.Pitch,Zoom=Camera.Zoom;const FVector2D Pan=Camera.Pan;const FVector2D Size=G.GetLocalSize();
        auto Label=[&](const FString& T,FVector2D At,FLinearColor C){FSlateDrawElement::MakeText(Out,Layer+3,G.ToPaintGeometry(FVector2D(1),FSlateLayoutTransform(At)),T,StudioUI::Font(9),ESlateDrawEffect::None,C);};
        if(!P||!P->IsValid()){Label(TEXT("Prepare the current geometric request to preview it"),FVector2D(8,24),StudioUI::Muted);return Layer+4;}
        const FString View=Mode();FBox Bounds=P->Mesh&&(View==TEXT("mesh")||View==TEXT("iso")||View==TEXT("motion")||View==TEXT("cut-links")||View==TEXT("distance")||View==TEXT("voxels"))?P->Body.ExpandBy(P->Spec.Geometry.BandCells.Get(4)):P->Tank;
        for(const auto& R:P->Regions)if(View==TEXT("zones")||View==TEXT("multidomain"))Bounds+=R.Bounds;
        const FVector Center=Bounds.GetCenter();const double Scale=Zoom*FMath::Min(Size.X-24,Size.Y-36)/FMath::Max(1.,Bounds.GetSize().Size());
        const FVector Right(-FMath::Sin(Yaw),FMath::Cos(Yaw),0),Up(-FMath::Sin(Pitch)*FMath::Cos(Yaw),-FMath::Sin(Pitch)*FMath::Sin(Yaw),FMath::Cos(Pitch));
        auto Project=[&](const FVector& V){const FVector D=V-Center;return Pan+FVector2D(Size.X*.5+FVector::DotProduct(D,Right)*Scale,Size.Y*.5-FVector::DotProduct(D,Up)*Scale);};
        auto Line=[&](const FVector& A,const FVector& B,FLinearColor C,float Width=1){FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),{Project(A),Project(B)},ESlateDrawEffect::None,C,true,Width);};
        auto Box=[&](const FBox& B,FLinearColor C,const FTransform& Transform=FTransform::Identity)
        {for(int32 I=0;I<8;++I)for(int32 Axis=0;Axis<3;++Axis)if(!(I&(1<<Axis))){FVector A,Bb;for(int32 J=0;J<3;++J){A[J]=I&(1<<J)?B.Max[J]:B.Min[J];Bb[J]=(I|(1<<Axis))&(1<<J)?B.Max[J]:B.Min[J];}Line(Transform.TransformPosition(A),Transform.TransformPosition(Bb),C);}};
        const FTransform Motion=View==TEXT("motion")?StudioHome4Authoring::Motion(P->Spec,Step()):FTransform::Identity;
        if(View==TEXT("tank")||View==TEXT("zones")||View==TEXT("multidomain"))Box(P->Tank,StudioUI::Muted);
        const TArray<FVector> EmptyPositions;const TArray<int32> EmptyIndices;const auto& RenderPositions=View==TEXT("iso")?P->SdfSurfacePositions:P->Mesh?P->Mesh->Positions:EmptyPositions;const auto& RenderIndices=View==TEXT("iso")?P->SdfSurfaceIndices:P->Mesh?P->Mesh->Indices:EmptyIndices;
        const int32 Faces=RenderIndices.Num()/3,TriangleStride=FMath::Max(1,(Faces+8191)/8192);
        if(View==TEXT("iso")&&Faces>0)
        {
            struct FZeroTriangle{int32 Id;double Depth;};TArray<FZeroTriangle> Sorted;const FVector Forward=FVector::CrossProduct(Right,Up);
            for(int32 T=0;T<Faces;T+=TriangleStride){double Depth=0;for(int32 C=0;C<3;++C)Depth+=FVector::DotProduct(RenderPositions[RenderIndices[T*3+C]]-Center,Forward);Sorted.Add({T,Depth});}
            Sorted.Sort([](const auto& A,const auto& B){return A.Depth<B.Depth;});TArray<FSlateVertex> Vertices;TArray<SlateIndex> Indices;
            for(const auto& T:Sorted){const FVector A=RenderPositions[RenderIndices[T.Id*3]],B=RenderPositions[RenderIndices[T.Id*3+1]],C=RenderPositions[RenderIndices[T.Id*3+2]],Normal=FVector::CrossProduct(B-A,C-A).GetSafeNormal();const auto Color=StudioUI::Cyan.CopyWithNewOpacity(.45+.35*FMath::Abs(FVector::DotProduct(Normal,Forward))).ToFColor(true);for(int32 J=0;J<3;++J){Indices.Add(SlateIndex(Vertices.Num()));Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),FVector2f(Project(RenderPositions[RenderIndices[T.Id*3+J]])),FVector2f::ZeroVector,Color));}}
            FSlateDrawElement::MakeCustomVerts(Out,Layer,FSlateApplication::Get().GetRenderer()->GetResourceHandle(StudioUI::PanelBrush),Vertices,Indices,nullptr,0,0);
        }
        for(int32 T=0;T<Faces;T+=TriangleStride)
        {const auto& I=RenderIndices;const auto& V=RenderPositions;for(int32 E=0;E<3;++E)Line(Motion.TransformPosition(V[I[T*3+E]]),Motion.TransformPosition(V[I[T*3+(E+1)%3]]),StudioUI::Cyan.CopyWithNewOpacity(.6));}
        if(View==TEXT("distance")||View==TEXT("voxels"))
        {
            const int32 DrawStride=FMath::Max(1,(P->Cells.Num()+4095)/4096);
            for(int32 I=0;I<P->Cells.Num();I+=DrawStride)
            {
                const auto& C=P->Cells[I];const FVector Position(C.Index);
                if(View==TEXT("voxels")){if(C.bInside)Box(FBox(Position-FVector(.5),Position+FVector(.5)),StudioUI::Amber.CopyWithNewOpacity(.5));}
                else{const double A=FMath::Clamp(FMath::Abs(C.SignedDistance)/FMath::Max(1.,P->Spec.Geometry.BandCells.Get(4)),0.,1.);const auto Color=C.bInside?FLinearColor(.9f,.3f,float(A)):FLinearColor(.2f,float(.4+.5*A),.9f);FSlateDrawElement::MakeBox(Out,Layer+2,G.ToPaintGeometry(FVector2D(3),FSlateLayoutTransform(Project(Position)-FVector2D(1.5))),&StudioUI::PanelBrush,ESlateDrawEffect::None,Color);}
            }
        }
        if(View==TEXT("cut-links"))
        {
            const int32 DrawStride=FMath::Max(1,(P->Links.Num()+8191)/8192);
            for(int32 I=0;I<P->Links.Num();I+=DrawStride){const auto& L=P->Links[I];Line(FVector(L.Index),L.Position,I==Link()?StudioUI::Cyan:StudioUI::Amber,I==Link()?3.f:1.4f);}
        }
        struct FFace{TArray<FVector> Corners;FLinearColor Color;double Depth;};TArray<FFace> FacesToDraw;
        const FVector Forward=FVector::CrossProduct(Right,Up);
        auto TranslucentBox=[&](const FBox& B,FLinearColor Color,const FTransform& Transform)
        {const int32 FaceIndices[][4]={{0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3}};for(const auto& Indices:FaceIndices){FFace F;F.Color=Color;F.Depth=0;for(int32 I:Indices){FVector V;for(int32 Axis=0;Axis<3;++Axis)V[Axis]=I&(1<<Axis)?B.Max[Axis]:B.Min[Axis];V=Transform.TransformPosition(V);F.Corners.Add(V);F.Depth+=FVector::DotProduct(V-Center,Forward);}FacesToDraw.Add(MoveTemp(F));}};
        for(const auto& R:P->Regions)if((View==TEXT("zones")&&R.Kind!=TEXT("multidomain"))||(View==TEXT("multidomain")&&R.Kind==TEXT("multidomain")))
        {const FLinearColor RegionColor=R.Kind==TEXT("multidomain")?FLinearColor::MakeFromHSV8(uint8(60+R.Level*35),180,240):StudioUI::Amber;const FTransform Moving=R.bFollowBody?StudioHome4Authoring::Motion(P->Spec,Step()):FTransform::Identity;TranslucentBox(R.Bounds,RegionColor.CopyWithNewOpacity(.08),Moving);Box(R.Bounds,RegionColor,Moving);Label(R.Id+FString::Printf(TEXT(" L%d"),R.Level),Project(Moving.TransformPosition(R.Bounds.GetCenter())),StudioUI::Text);}
        if(View==TEXT("tank"))
        {
            const TOptional<bool> Periodic[]={P->Spec.Zones.PeriodicX,P->Spec.Zones.PeriodicY,P->Spec.Zones.PeriodicZ};
            for(int32 Axis=0;Axis<3;++Axis)if(Periodic[Axis].Get(false))for(int32 Side=0;Side<2;++Side){FBox Face=P->Tank;Face.Min[Axis]=Face.Max[Axis]=Side?P->Tank.Max[Axis]:P->Tank.Min[Axis];TranslucentBox(Face,FLinearColor(.5,.3,.9,.12),FTransform::Identity);}
            if(P->Spec.Zones.PhiTop||P->Spec.Zones.PinPhaseWalls.Get(false)){FBox Face=P->Tank;Face.Min.Z=Face.Max.Z=P->Tank.Max.Z;TranslucentBox(Face,FLinearColor(.2,.5,.9,.12),FTransform::Identity);}
            if(P->Spec.Zones.PhiBottom||P->Spec.Zones.PinPhaseWalls.Get(false)){FBox Face=P->Tank;Face.Min.Z=Face.Max.Z=0;TranslucentBox(Face,FLinearColor(.9,.4,.2,.12),FTransform::Identity);}
        }
        if(View==TEXT("tank")||View==TEXT("zones"))
        {
            TArray<FStudioHome4BoundaryFace> Boundaries;FString Error;
            if(StudioHome4Setup::BoundaryFaces(P->Spec,Boundaries,Error))for(const auto& Face:Boundaries)
            {Label(Face.Id+TEXT(" · ")+Face.Constraint,Project(Face.Bounds.GetCenter()),StudioUI::Muted);if(Face.Phase||Face.PhaseConstraint.Contains(TEXT("phase-pinned")))TranslucentBox(Face.Bounds,StudioUI::Cyan.CopyWithNewOpacity(.06),FTransform::Identity);}
            TArray<FVector> Curve;
            if(StudioHome4Setup::WaveCurve(P->Spec,Step(),128,Curve,Error))
            {
                const bool AlongX=P->Spec.Authoring.WaveAxis==TEXT("x");const int32 CrossAxis=AlongX?1:0;
                for(int32 Strip=0;Strip<7;++Strip)for(int32 I=1;I<Curve.Num();++I){FVector A=Curve[I-1],B=Curve[I];A[CrossAxis]=B[CrossAxis]=P->Tank.Min[CrossAxis]+P->Tank.GetSize()[CrossAxis]*Strip/6.;Line(A,B,FLinearColor(.15,.6,.95),Strip==0||Strip==6?2.f:1.f);}
                Label(TEXT("Computed linear wave elevation · pinned-wall intersection where declared"),FVector2D(8,6),StudioUI::Cyan);
            }
            else if(P->Spec.Authoring.WaterlineCells&&P->Spec.Authoring.PierceMode==TEXT("phase-pinned"))for(int32 Side=0;Side<2;++Side)
            {const double Y=Side?P->Tank.Max.Y:P->Tank.Min.Y;Line(FVector(P->Tank.Min.X,Y,*P->Spec.Authoring.WaterlineCells),FVector(P->Tank.Max.X,Y,*P->Spec.Authoring.WaterlineCells),StudioUI::Cyan,2);}
        }
        if(View==TEXT("mesh")||View==TEXT("motion"))if(P->Spec.Geometry.CenterOfGravity)
        {const FVector CoG=Motion.TransformPosition(*P->Spec.Geometry.CenterOfGravity);const double Radius=FMath::Max(.1,Bounds.GetSize().GetMax()*.025);for(int32 Axis=0;Axis<3;++Axis){FVector Offset=FVector::ZeroVector;Offset[Axis]=Radius;Line(CoG-Offset,CoG+Offset,StudioUI::Amber,2);}Label(TEXT("CoG"),Project(CoG),StudioUI::Amber);}
        FacesToDraw.Sort([](const FFace& A,const FFace& B){return A.Depth<B.Depth;});
        const auto Resource=FSlateApplication::Get().GetRenderer()->GetResourceHandle(StudioUI::PanelBrush);
        for(const auto& F:FacesToDraw){TArray<FSlateVertex> Vertices;for(const auto& V:F.Corners)Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),FVector2f(Project(V)),FVector2f::ZeroVector,F.Color.ToFColor(true)));FSlateDrawElement::MakeCustomVerts(Out,Layer,Resource,Vertices,TArray<SlateIndex>{0,1,2,0,2,3},nullptr,0,0);}
        if(P->Spec.Authoring.WaterlineCells&&(View==TEXT("tank")||View==TEXT("mesh")))
        {FBox Water=P->Tank;Water.Min.Z=Water.Max.Z=*P->Spec.Authoring.WaterlineCells;Box(Water,FLinearColor(.2,.4,.8));}
        Label(FString::Printf(TEXT("XYZ root cells · %d/%d triangle edges · stride %d original grid"),(Faces+TriangleStride-1)/TriangleStride,Faces,P->Stride),FVector2D(8,Size.Y-18),StudioUI::Muted);return Layer+4;
    }
private:TSharedPtr<FStudioHome4AuthoringSession> Session;TFunction<FString()> Mode;TFunction<double()> Step;TFunction<int32()> Link;bool bPan=false;
};
class SZoneProfile final:public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SZoneProfile){} SLATE_ARGUMENT(TSharedPtr<FStudioHome4AuthoringSession>,Session) SLATE_END_ARGS()
    void Construct(const FArguments& A){Session=A._Session;}
    FVector2D ComputeDesiredSize(float)const override{return FVector2D(400,120);}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool)const override
    {
        const auto P=Session->Preview();if(!P)return Layer;const auto Size=G.GetLocalSize();int32 Z=0;
        for(const auto& R:P->Regions)if(R.Kind!=TEXT("multidomain"))
        {TArray<FVector2D> Points;const int32 Axis=R.Axis==TEXT("y")?1:R.Axis==TEXT("z")?2:0;for(int32 I=0;I<=64;++I){FVector Position=R.Bounds.GetCenter();Position[Axis]=FMath::Lerp(R.Bounds.Min[Axis],R.Bounds.Max[Axis],double(I)/64);Points.Add(FVector2D(10+(Size.X-20)*I/64,Size.Y-20-(Size.Y-30)*StudioHome4Authoring::ZoneWeight(R,Position)));}const auto C=FLinearColor::MakeFromHSV8(uint8(30+Z*45),180,240);FSlateDrawElement::MakeLines(Out,Layer,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,C,true,1.5);FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(FVector2D(1),FSlateLayoutTransform(FVector2D(12,8+Z*14))),R.Id+TEXT(" ")+R.Profile+TEXT(" · 0 → ")+FString::Printf(TEXT("%.3g"),R.Strength),StudioUI::Font(9),ESlateDrawEffect::None,C);++Z;}
        return Layer+2;
    }
private:TSharedPtr<FStudioHome4AuthoringSession> Session;
};
}
void SStudioHome4Authoring::Construct(const FArguments& A)
{
    Session=A._Session;Page=A._Page;
    {FStudioHome4Spec Spec;FString E;const auto M=Session->Draft()->Owner();if(M&&Session->Draft()->Build(Spec,E)&&StudioHome4Config::ConvertUnits(1,EStudioHome4Quantity::Density,EStudioHome4UnitDisplay::Lattice,M->UnitDisplay,Spec))DensityDisplay=M->UnitDisplay;}SetCanTick(true);auto Rows=SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[StudioUI::Label(TEXT("Next-run geometric preparation"),15,StudioUI::Text,true)];
    auto Buttons=SNew(SHorizontalBox);
    auto Button=[&](const FString& Label,FName Tag,TFunction<void()> Action){Buttons->AddSlot().AutoWidth().Padding(0,0,6,0)[SNew(SButton).Tag(Tag).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([Action]{Action();return FReply::Handled();})[StudioUI::Label(Label,10)]];};
    Button(TEXT("Prepare geometry & SDF"),TEXT("Home4Prepare"),[this]{Session->Request();});Button(TEXT("Cancel"),TEXT("Home4PrepareCancel"),[this]{Session->Cancel();});Button(TEXT("Pin source SHA"),TEXT("Home4PreparePin"),[this]{PinSource();});
    if(Page==TEXT("Geometry")||Page==TEXT("Bodies"))Button(TEXT("Use equilibrium"),TEXT("Home4PrepareEquilibrium"),[this]{ApplyEquilibrium();});
    Rows->AddSlot().AutoHeight()[Buttons];
    if(Page==TEXT("Geometry")||Page==TEXT("Bodies"))
    {
        auto Mass=SNew(SHorizontalBox);Mass->AddSlot().AutoWidth().VAlign(VAlign_Center)[StudioUI::Label(TEXT("Uniform body density · ")+StudioHome4Readouts::Unit(EStudioHome4Quantity::Density,DensityDisplay),9,StudioUI::Muted)];
        Mass->AddSlot().FillWidth(1).Padding(8,0)[SNew(SEditableTextBox).Tag(TEXT("Home4MassDensity")).Style(&StudioUI::InputStyle()).Text(FText::FromString(DensityText)).OnTextChanged_Lambda([this](const FText& T){DensityText=T.ToString();})];
        Mass->AddSlot().AutoWidth().Padding(8,0)[SNew(SButton).Tag(TEXT("Home4AdoptMass")).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this]{AdoptMass();return FReply::Handled();})[StudioUI::Label(TEXT("Calculate & retain mass"),9)]];
        Rows->AddSlot().AutoHeight().Padding(0,8)[Mass];
        Rows->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("Home4AdoptHydrostaticK")).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this]{AdoptStiffness();return FReply::Handled();})[StudioUI::Label(TEXT("Retain geometric heave/pitch stiffness block"),9)]];
    }
    auto Views=SNew(SHorizontalBox);const TArray<FString> Modes=Page==TEXT("Lattice")?TArray<FString>{TEXT("tank"),TEXT("multidomain"),TEXT("voxels")}:Page==TEXT("Boundaries & Zones")?TArray<FString>{TEXT("tank"),TEXT("zones")}:Page==TEXT("Bodies")?TArray<FString>{TEXT("mesh"),TEXT("motion")}:TArray<FString>{TEXT("mesh"),TEXT("iso"),TEXT("distance"),TEXT("voxels"),TEXT("cut-links"),TEXT("tank")};
    Mode=Modes[0];for(const auto& V:Modes)Views->AddSlot().FillWidth(1).Padding(0,8,4,4)[SNew(SButton).Tag(FName(*(TEXT("Home4Preview.")+V))).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this,V]{Mode=V;return FReply::Handled();})[SNew(STextBlock).Font(StudioUI::Font(9)).Text(FText::FromString(V)).ColorAndOpacity_Lambda([this,V]{return FSlateColor(Mode==V?StudioUI::Cyan:StudioUI::Muted);})]];
    Rows->AddSlot().AutoHeight()[Views];Rows->AddSlot().AutoHeight()[SNew(StudioHome4AuthoringWidgetLocal::SGeometry).Tag(TEXT("Home4PreparedGeometry")).Session(Session).Mode([this]{return Mode;}).Step([this]{return Step;}).Link([this]{return SelectedLink;})];
    if(Page==TEXT("Bodies")||Page==TEXT("Lattice")||Page==TEXT("Boundaries & Zones"))Rows->AddSlot().AutoHeight().Padding(0,8)[SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth()[StudioUI::Label(TEXT("Kinematic preview step"),10,StudioUI::Muted)]+SHorizontalBox::Slot().FillWidth(1).Padding(12,0)[SNew(SEditableTextBox).Tag(TEXT("Home4PreviewStep")).Style(&StudioUI::InputStyle()).Text(FText::FromString(TimeText)).OnTextChanged_Lambda([this](const FText& T){TimeText=T.ToString();double V;if(LexTryParseString(V,*TimeText)&&FMath::IsFinite(V)&&FMath::Abs(V)<=1e9)Step=V;})]];
    if(Page==TEXT("Bodies")||Page==TEXT("Boundaries & Zones"))Rows->AddSlot().AutoHeight().Padding(0,5)[SNew(SButton).Tag(TEXT("Home4PreviewPlay")).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this]{bPlaying=!bPlaying;return FReply::Handled();})[SNew(STextBlock).Tag(TEXT("Home4PreviewPlayLabel")).Font(StudioUI::Font(9)).Text_Lambda([this]{return FText::FromString((bPlaying?FString(TEXT("Pause geometric motion")):FString(TEXT("Play geometric motion · 30 steps/s")))+FString::Printf(TEXT(" · step %.6g"),Step));})]];
    if(Page==TEXT("Geometry"))Rows->AddSlot().AutoHeight().Padding(0,6)[SNew(SButton).Tag(TEXT("Home4PreviewNextLink")).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this]{const auto P=Session->Preview();if(P&&!P->Links.IsEmpty())SelectedLink=(SelectedLink+1)%P->Links.Num();Mode=TEXT("cut-links");return FReply::Handled();})[StudioUI::Label(TEXT("Inspect next cut fraction"),10)]];
    if(Page==TEXT("Boundaries & Zones"))Rows->AddSlot().AutoHeight()[SNew(StudioHome4AuthoringWidgetLocal::SZoneProfile).Session(Session)];
    Rows->AddSlot().AutoHeight().Padding(0,8)[SNew(SBox).HeightOverride(124)[SNew(SMultiLineEditableTextBox).Tag(TEXT("Home4PreparedDetails")).IsReadOnly(true).ToolTipText_Lambda([this]{return FText::FromString(ReadoutTooltip());}).Text_Lambda([this]{return FText::FromString(Detail());}).Font(StudioUI::Font(9)).AutoWrapText(true)]];
    Rows->AddSlot().AutoHeight().Padding(0,8)[SNew(STextBlock).Tag(TEXT("Home4PrepareStatus")).Text_Lambda([this]{return FText::FromString(Session->Status);}).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Amber).AutoWrapText(true)];
    ChildSlot[SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(12)[Rows]];
}
void SStudioHome4Authoring::Tick(const FGeometry&,double,float Delta){Session->Poll();if(bPlaying){Step=FMath::Min(1e9,Step+30*Delta);TimeText=FString::Printf(TEXT("%.5g"),Step);}}
FString SStudioHome4Authoring::Detail()const
{
    const auto P=Session->Preview();if(!P)return TEXT("The preparation uses declared units, source axes, pose, tank, zone and patch geometry. Drag to orbit. No numerical flow evolution is shown.");
    FString Text=P->Method+TEXT("\nRequest SHA256: ")+P->RequestSHA256+TEXT("\nOriginal geometry SHA256: ")+(P->SourceSHA256.IsEmpty()?TEXT("not applicable · declared primitive"):P->SourceSHA256);
    if(!P->Mesh)return Text+TEXT("\nTank-only request: named boundary faces, phase constraints and computed linear-wave elevation. No body/SDF is invented.");
    Text+=FString::Printf(TEXT("\nClosed mesh · %d original-index SDF samples of %lld nodes · stride %d · %d exact triangle/link intersections"),P->Cells.Num(),P->RequestedCells,P->Stride,P->Links.Num());
    Text+=TEXT("\nCAD mesh and sampled SDF zero surface are separate layers. Zero surface uses marching tetrahedra with distance interpolation; it is a sampled geometric approximation.\nOrbit: left drag · Pan: right drag · Zoom: wheel · XY/XZ/YZ: keys 1/2/3 · Reset: Home");
    if(P->Links.IsValidIndex(SelectedLink)){const auto& L=P->Links[SelectedLink];Text+=FString::Printf(TEXT("\nSelected cut link: cell [%d,%d,%d], direction [%d,%d,%d], fraction %.9g"),L.Index.X,L.Index.Y,L.Index.Z,L.Direction.X,L.Direction.Y,L.Direction.Z,L.Fraction);}
    Text+=TEXT("\nStairsteps show original unit cells at sampled nodes; line display is bounded separately. Signed distance/cut fractions come from the complete prepared mesh.");

    Text+=FString::Printf(TEXT("\nMeasured local geometric SDF/link preparation: %.6g s (not numerical-solver MLUPS). Retabulation policy: %s; cadence %s."),P->GeometricPreparationSeconds,*P->Spec.Geometry.RetabulationPolicy,*(P->Spec.Geometry.RetabulateEvery?FString::Printf(TEXT("%.0f steps"),*P->Spec.Geometry.RetabulateEvery):TEXT("unspecified")));
    if(P->Spec.Geometry.BodyMotion==TEXT("free"))Text+=TEXT("\nFree-body preview extrapolates only declared initial linear/angular velocities. No hydrodynamic response is fabricated.");
    const auto M=Session->Draft()->Owner();const auto Display=M?M->UnitDisplay:EStudioHome4UnitDisplay::Lattice;
    auto Read=[&](const FString& Name,const TOptional<double>& Value,EStudioHome4Quantity Q){if(Value)Text+=TEXT("\n")+Name+TEXT(": ")+StudioHome4Readouts::Value(*Value,Q,EStudioHome4UnitDisplay::Lattice,Display,&P->Spec);};
    if(P->Hydrostatics)
    {
        const auto& H=*P->Hydrostatics;Text+=FString::Printf(TEXT("\nTessellated geometric hydrostatics · %d iterations. Submerged volume source: %.8g cells³."),H.Iterations,H.SubmergedVolume);
        Read(TEXT("Displaced mass"),H.DisplacedMass,EStudioHome4Quantity::Mass);Read(TEXT("Heave"),H.Heave,EStudioHome4Quantity::Length);Read(TEXT("Trim"),H.TrimDegrees,EStudioHome4Quantity::Angle);Read(TEXT("Vertical residual"),H.VerticalResidual,EStudioHome4Quantity::Force);Read(TEXT("Pitch residual"),H.PitchMomentResidual,EStudioHome4Quantity::Moment);Read(TEXT("K33"),H.K33,EStudioHome4Quantity::StiffnessTranslation);Read(TEXT("K35"),H.K35,EStudioHome4Quantity::StiffnessCoupling);Read(TEXT("K53"),H.K53,EStudioHome4Quantity::StiffnessCoupling);Read(TEXT("K55"),H.K55,EStudioHome4Quantity::StiffnessRotation);
    }
    Read(TEXT("Body mass"),P->Spec.Geometry.BodyMass,EStudioHome4Quantity::Mass);
    if(P->Spec.Geometry.CenterOfGravity)for(int32 Axis=0;Axis<3;++Axis)Read(FString::Printf(TEXT("CoG %c"),'X'+Axis),(*P->Spec.Geometry.CenterOfGravity)[Axis],EStudioHome4Quantity::Length);
    if(!P->Spec.Geometry.MassPropertySource.IsEmpty())Text+=TEXT("\nMass properties: ")+P->Spec.Geometry.MassPropertySource;
    if(!P->HydrostaticError.IsEmpty())Text+=TEXT("\n")+P->HydrostaticError;
    return Text;
}
void SStudioHome4Authoring::PinSource()
{
    const auto P=Session->Preview();if(!P){Session->Status=TEXT("Prepare the actual geometry before pinning its source identity.");return;}
    FStudioHome4Spec S;FString E;if(!Session->Draft()->Build(S,E)||StudioHome4Authoring::Fingerprint(S)!=P->RequestSHA256){Session->Status=TEXT("Prepare the current draft before pinning its source.");return;}
    Session->Draft()->Set(TEXT("authoring.sourceSHA256"),P->SourceSHA256);Session->Draft()->Set(TEXT("authoring.preparationMethod"),P->Method);Session->Status=TEXT("Original source SHA and preparation method are retained in the draft; Apply saves them.");
}
void SStudioHome4Authoring::ApplyEquilibrium()
{
    const auto P=Session->Preview();if(!P||!P->Hydrostatics||!P->Hydrostatics->bConverged){Session->Status=TEXT("Prepare a converged geometric flotation response before using equilibrium.");return;}
    FStudioHome4Spec S;FString Error;if(!Session->Draft()->Build(S,Error)||StudioHome4Authoring::Fingerprint(S)!=P->RequestSHA256){Session->Status=TEXT("The draft changed; prepare it again before using equilibrium.");return;}
    if(!StudioHome4Authoring::AcceptEquilibrium(*P,S,Error)){Session->Status=Error;return;}
    if(Session->Draft()->Replace(S,Error))Session->Status=TEXT("Geometric equilibrium pose and CoG retained. The complete source-XYZ starting rotation was composed exactly; Apply saves this pose.");else Session->Status=Error;
}

void SStudioHome4Authoring::AdoptMass()
{
    const auto P=Session->Preview();FStudioHome4Spec Current,Out;FString Error;double Density;
    if(!P||!Session->Draft()->Build(Current,Error)||StudioHome4Authoring::Fingerprint(Current)!=P->RequestSHA256){Session->Status=TEXT("Prepare the current geometric request before calculating mass properties.");return;}
    if(!StudioColor::ParseNumber(DensityText,Density)||Density<=0){Session->Status=TEXT("Supply a finite positive uniform body density in LU mass/cell³.");return;}
    const auto Converted=StudioHome4Config::ConvertUnits(Density,EStudioHome4Quantity::Density,DensityDisplay,EStudioHome4UnitDisplay::Lattice,Current);if(!Converted){Session->Status=TEXT("Uniform density conversion requires the declared next-run map.");return;}Density=*Converted;
    if(!StudioHome4Body::AdoptMassProperties(*P,Density,Out,Error)||!Session->Draft()->Replace(Out,Error)){Session->Status=Error;return;}
    Session->Status=TEXT("Full closed-mesh mass, CoG and inertia retained in source-XYZ frame. Apply saves this explicitly uniform-density model.");
}
void SStudioHome4Authoring::AdoptStiffness()
{
    const auto P=Session->Preview();FStudioHome4Spec Current,Out;FString Error;
    if(!P||!Session->Draft()->Build(Current,Error)||StudioHome4Authoring::Fingerprint(Current)!=P->RequestSHA256){Session->Status=TEXT("Prepare the current geometric request before adopting stiffness.");return;}
    if(!StudioHome4Body::AdoptHydrostaticStiffness(*P,Out,Error)||!Session->Draft()->Replace(Out,Error)){Session->Status=Error;return;}
    Session->Status=TEXT("Geometric heave/pitch block retained. Other supplied components are preserved; newly initialized components remain zero and are not measured stiffness.");
}

FString SStudioHome4Authoring::ReadoutTooltip()const
{
    const auto P=Session->Preview();if(!P)return TEXT("Prepare the current geometric request to inspect all unit forms.");FString Text;
    auto Add=[&](const FString& Name,double Value,EStudioHome4Quantity Q){Text+=Name+TEXT("\n")+StudioHome4Readouts::Tooltip(Value,Q,EStudioHome4UnitDisplay::Lattice,&P->Spec)+TEXT("\n\n");};
    if(P->Spec.Geometry.BodyMass)Add(TEXT("Body mass"),*P->Spec.Geometry.BodyMass,EStudioHome4Quantity::Mass);
    if(P->Spec.Geometry.CenterOfGravity)for(int32 Axis=0;Axis<3;++Axis)Add(FString::Printf(TEXT("CoG %c"),'X'+Axis),(*P->Spec.Geometry.CenterOfGravity)[Axis],EStudioHome4Quantity::Length);
    if(P->Spec.Geometry.InertiaDiagonal)for(int32 Axis=0;Axis<3;++Axis)Add(FString::Printf(TEXT("Inertia %c%c"),'X'+Axis,'X'+Axis),(*P->Spec.Geometry.InertiaDiagonal)[Axis],EStudioHome4Quantity::Inertia);
    if(P->Hydrostatics){const auto& H=*P->Hydrostatics;Add(TEXT("Displaced mass"),H.DisplacedMass,EStudioHome4Quantity::Mass);Add(TEXT("Heave"),H.Heave,EStudioHome4Quantity::Length);Add(TEXT("Trim"),H.TrimDegrees,EStudioHome4Quantity::Angle);Add(TEXT("Vertical residual"),H.VerticalResidual,EStudioHome4Quantity::Force);Add(TEXT("Pitch residual"),H.PitchMomentResidual,EStudioHome4Quantity::Moment);Add(TEXT("K33"),H.K33,EStudioHome4Quantity::StiffnessTranslation);Add(TEXT("K35"),H.K35,EStudioHome4Quantity::StiffnessCoupling);Add(TEXT("K53"),H.K53,EStudioHome4Quantity::StiffnessCoupling);Add(TEXT("K55"),H.K55,EStudioHome4Quantity::StiffnessRotation);Text+=FString::Printf(TEXT("Submerged volume LU: %.9g cells³\n"),H.SubmergedVolume);Text+=P->Spec.Units.DxMeters?FString::Printf(TEXT("SI: %.9g m³\n"),H.SubmergedVolume*FMath::Pow(*P->Spec.Units.DxMeters,3)):TEXT("SI: original length map required\n");Text+=P->Spec.Reference.LengthCells?FString::Printf(TEXT("ND: %.9g L³\n"),H.SubmergedVolume/FMath::Pow(*P->Spec.Reference.LengthCells,3)):TEXT("ND: declared reference length required\n");}
    return Text.IsEmpty()?TEXT("Coordinates use declared root XYZ cells. SDF samples/cut fractions retain original grid indices; no CFD values are present."):Text;
}
