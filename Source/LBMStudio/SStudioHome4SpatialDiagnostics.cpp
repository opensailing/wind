#include "SStudioHome4SpatialDiagnostics.h"
#include "StudioFileDialog.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"

namespace StudioHome4SpatialWidgetPrivate
{
    FString Value(const FStudioHome4SpatialValue& V){return V.Value?FString::Printf(TEXT("%.6g %s"),*V.Value,*V.Unit):TEXT("Unavailable");}
    FString Number(const TOptional<double>& V){return V?FString::Printf(TEXT("%.6g"),*V):TEXT("Unavailable");}
    FString Count(const TOptional<int64>& V){return V?FString::Printf(TEXT("%lld"),*V):TEXT("Unavailable");}
    FString Flag(const TOptional<bool>& V){return V?(*V?TEXT("on"):TEXT("off")):TEXT("Unavailable");}
    FString Text(const FString& V){return V.IsEmpty()?TEXT("Unavailable"):V;}
    FString Vector(const TOptional<FVector>& V){return V?FString::Printf(TEXT("[%.6g, %.6g, %.6g]"),V->X,V->Y,V->Z):TEXT("Unavailable");}
    class SBounds final:public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SBounds){}
            SLATE_ARGUMENT(TFunction<const FStudioHome4SpatialEvidence*()>,Read)
            SLATE_ARGUMENT(TFunction<int32()>,Plane)
        SLATE_END_ARGS()
        void Construct(const FArguments& A){Read=A._Read;Plane=A._Plane;}
        FVector2D ComputeDesiredSize(float)const override{return FVector2D(300,190);}
        int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool)const override
        {
            using namespace StudioUI;const auto* E=Read();
            auto LabelAt=[&](const FString& V,FVector2D At,FLinearColor Color){FSlateDrawElement::MakeText(Out,Layer+2,G.ToPaintGeometry(FVector2D(1,1),FSlateLayoutTransform(At)),V,Font(8),ESlateDrawEffect::None,Color);};
            if(!E){LabelAt(TEXT("Original patch/zone bounds unavailable"),FVector2D(0,20),Muted);return Layer+3;}
            struct FRegion{FBox Bounds;FString Label;FLinearColor Color;};TArray<FRegion> Regions;FBox Total(ForceInit);
            for(const auto& P:E->Patches)if(const auto B=P.Bounds()){const FLinearColor Color=FLinearColor::MakeFromHSV8(uint8((P.Level*43+125)%255),160,210);Regions.Add({*B,P.Id+FString::Printf(TEXT(" L%d"),P.Level),Color});Total+=*B;}
            for(const auto& Z:E->Zones)if(Z.Minimum&&Z.Maximum){const FBox B(*Z.Minimum,*Z.Maximum);Regions.Add({B,Z.Id+TEXT(" ")+Z.Kind,Amber});Total+=B;}
            for(const auto& B:E->Geometry)if(B.BoundsMinimum&&B.BoundsMaximum){const FBox Bounds(*B.BoundsMinimum,*B.BoundsMaximum);Regions.Add({Bounds,B.BodyId,Cyan});Total+=Bounds;}
            if(Regions.IsEmpty()){LabelAt(TEXT("Complete original bounds unavailable; no grids inferred"),FVector2D(0,20),Muted);return Layer+3;}
            const int32 AxisA=Plane()==2?1:0,AxisB=Plane()==0?1:2;
            const FVector Span=Total.GetSize();const auto Size=G.GetLocalSize();const double Scale=FMath::Min((Size.X-12)/FMath::Max(Span[AxisA],1e-12),(Size.Y-30)/FMath::Max(Span[AxisB],1e-12));
            for(const auto& R:Regions)
            {
                const FVector2D At(6+(R.Bounds.Min[AxisA]-Total.Min[AxisA])*Scale,5+(Total.Max[AxisB]-R.Bounds.Max[AxisB])*Scale);
                const FVector2D Extent(FMath::Max(2.,R.Bounds.GetSize()[AxisA]*Scale),FMath::Max(2.,R.Bounds.GetSize()[AxisB]*Scale));
                FSlateDrawElement::MakeBox(Out,Layer,G.ToPaintGeometry(Extent,FSlateLayoutTransform(At)),&PanelBrush,ESlateDrawEffect::None,R.Color.CopyWithNewOpacity(.12));
                TArray<FVector2D> Line{At,At+FVector2D(Extent.X,0),At+Extent,At+FVector2D(0,Extent.Y),At};FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Line,ESlateDrawEffect::None,R.Color,true,1.5);LabelAt(R.Label,At+FVector2D(2,2),R.Color);
            }
            const TCHAR* Axes[]={TEXT("X"),TEXT("Y"),TEXT("Z")};LabelAt(FString::Printf(TEXT("Original %s / %s · %s"),Axes[AxisA],Axes[AxisB],*E->CoordinateUnit),FVector2D(0,Size.Y-18),Muted);return Layer+3;
        }
    private:TFunction<const FStudioHome4SpatialEvidence*()> Read;TFunction<int32()> Plane;
    };
    class SProfile final:public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SProfile){}
            SLATE_ARGUMENT(TFunction<const FStudioHome4SpatialZone*()>,Read)
        SLATE_END_ARGS()
        void Construct(const FArguments& A){Read=A._Read;}
        FVector2D ComputeDesiredSize(float)const override{return FVector2D(300,120);}
        int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool)const override
        {
            using namespace StudioUI;const auto* Z=Read();
            auto LabelAt=[&](const FString& V,FVector2D At){FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(FVector2D(1,1),FSlateLayoutTransform(At)),V,Font(8),ESlateDrawEffect::None,Muted);};
            if(!Z||Z->ProfileX.IsEmpty()){LabelAt(TEXT("Original profile samples unavailable"),FVector2D(0,20));return Layer+2;}
            double Min=Z->ProfileValues[0],Max=Min;for(double V:Z->ProfileValues){Min=FMath::Min(Min,V);Max=FMath::Max(Max,V);}const auto Size=G.GetLocalSize();TArray<FVector2D> Points;
            const double XRange=Z->ProfileX.Last()-Z->ProfileX[0],YRange=Max-Min;
            for(int32 I=0;I<Z->ProfileX.Num();++I)Points.Add(FVector2D(50+(Size.X-55)*(Z->ProfileX[I]-Z->ProfileX[0])/XRange,95-(YRange?80*(Z->ProfileValues[I]-Min)/YRange:40)));
            FSlateDrawElement::MakeLines(Out,Layer,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Amber,true,1.5);
            LabelAt(FString::Printf(TEXT("%.4g"),Max),FVector2D(0,10));LabelAt(FString::Printf(TEXT("%.4g"),Min),FVector2D(0,84));LabelAt(Z->ProfileAxis+TEXT(" [")+Z->ProfileCoordinateUnit+TEXT("] · ")+Z->ProfileValueUnit,FVector2D(50,102));return Layer+2;
        }
    private:TFunction<const FStudioHome4SpatialZone*()> Read;
    };
}
void SStudioHome4SpatialDiagnostics::Construct(const FArguments& A)
{
    using namespace StudioUI;Session=A._Session?A._Session:MakeShared<FStudioHome4SpatialSession>(A._Model);Expected=A._ExpectedRunId;OnLocate=A._OnLocate;
    auto Rows=SNew(SVerticalBox);
    auto Detail=[&](FName Tag,TFunction<FString()> Read){Rows->AddSlot().AutoHeight().Padding(0,5)[SNew(STextBlock).Tag(Tag).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true).Text_Lambda([Read]{return FText::FromString(Read());})];};
    Detail(TEXT("Home4SpatialSource"),[this]{return SourceText();});
    auto Planes=SNew(SHorizontalBox);const TCHAR* Names[]={TEXT("XY"),TEXT("XZ"),TEXT("YZ")};
    for(int32 I=0;I<3;++I)Planes->AddSlot().FillWidth(1).Padding(0,0,5,0)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("Home4SpatialPlane%d"),I))).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([this,I]{Plane=I;return FReply::Handled();})[SNew(STextBlock).Font(Font(9)).Text(FText::FromString(Names[I])).ColorAndOpacity_Lambda([this,I]{return FSlateColor(Plane==I?Cyan:Muted);})]];
    Rows->AddSlot().AutoHeight()[Planes];Rows->AddSlot().AutoHeight().Padding(0,5)[SNew(StudioHome4SpatialWidgetPrivate::SBounds).Read([this]{return Session->Evidence().Get();}).Plane([this]{return Plane;})];
    if(A._View==EStudioHome4SpatialView::All||A._View==EStudioHome4SpatialView::Multidomain)
    {
        Rows->AddSlot().AutoHeight()[Label(TEXT("Original patches and level parameters"),11,Text,true)];
        Rows->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("Home4SpatialNextPatch")).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([this]{const auto E=Session->Evidence();if(E&&!E->Patches.IsEmpty()){SelectedPatch=(SelectedPatch+1)%E->Patches.Num();Session->OpenBoundPatch(E->Patches[SelectedPatch].Id);}return FReply::Handled();})[Label(TEXT("Choose next original patch"),9)]];
        Detail(TEXT("Home4SpatialPatch"),[this]{return PatchText();});
        Rows->AddSlot().AutoHeight().Padding(0,5)[SNew(SButton).Tag(TEXT("Home4SpatialOpenPatch")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda([this]{return Patch()&&!Session->IsPatchImporting();})
            .OnClicked_Lambda([this]{FString Path;if(Patch()&&StudioFileDialog::DataFile(false,TEXT("Choose selected patch recording.json"),{},TEXT("json"),Path))Session->BeginPatchRecording(Patch()->Id,Path);return FReply::Handled();})[Label(TEXT("Open selected patch recording…"),9)]];
        Rows->AddSlot().AutoHeight().Padding(0,7)[SNew(SScrollBox).Orientation(Orient_Horizontal)
            +SScrollBox::Slot()[SNew(SBox).MinDesiredWidth(640)[SAssignNew(LevelRows,SVerticalBox)]]];
        Detail(TEXT("Home4SpatialLevels"),[this]{return LevelText();});
        Rows->AddSlot().AutoHeight().Padding(0,5)[SNew(SEditableTextBox).Tag(TEXT("Home4SpatialCell")).Style(&InputStyle()).Font(Font(9)).HintText(FText::FromString(TEXT("Original cell i,j,k · explicit patch-local indices"))).Text_Lambda([this]{return FText::FromString(CellDraft);}).OnTextChanged_Lambda([this](const FText& V){CellDraft=V.ToString();})];
        Rows->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("Home4SpatialLocate")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda([this]{return OnLocate.IsBound()&&Patch()&&Patch()->Extents.IsSet();}).OnClicked_Lambda([this]{Locate();return FReply::Handled();})[Label(TEXT("Locate original patch cell"),9)]];
    }
    if(A._View==EStudioHome4SpatialView::All||A._View==EStudioHome4SpatialView::Zones)
    {
        Rows->AddSlot().AutoHeight().Padding(0,10)[Label(TEXT("Original zone bounds and strength profiles"),11,Text,true)];
        Rows->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("Home4SpatialNextZone")).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([this]{const auto E=Session->Evidence();if(E&&!E->Zones.IsEmpty())SelectedZone=(SelectedZone+1)%E->Zones.Num();return FReply::Handled();})[Label(TEXT("Choose next original zone"),9)]];
        Detail(TEXT("Home4SpatialZone"),[this]{return ZoneText();});Rows->AddSlot().AutoHeight()[SNew(StudioHome4SpatialWidgetPrivate::SProfile).Read([this]{return Zone();})];
    }
    if(A._View==EStudioHome4SpatialView::All||A._View==EStudioHome4SpatialView::Geometry)
    {Rows->AddSlot().AutoHeight().Padding(0,10)[Label(TEXT("CAD / SDF adapter and flotation diagnostics"),11,Text,true)];Detail(TEXT("Home4SpatialGeometry"),[this]{return GeometryText();});}
    Detail(TEXT("Home4SpatialAction"),[this]{return ActionStatus;});
    RefreshLevels();
    ChildSlot[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[Label(TEXT("Original spatial diagnostics"),15,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,7)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4SpatialImport")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda([this]{return !Session->IsImporting();}).OnClicked_Lambda([this]{Dialog();return FReply::Handled();})[Label(TEXT("Import original JSON"),9)]]
            +SHorizontalBox::Slot().AutoWidth().Padding(5,0)[SNew(SButton).Tag(TEXT("Home4SpatialCancel")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda([this]{return Session->IsImporting();}).OnClicked_Lambda([this]{Session->Cancel();return FReply::Handled();})[Label(TEXT("Cancel read"),9)]]]
        +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4SpatialStatus")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Session->Status());})]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Rows]]]];
}
void SStudioHome4SpatialDiagnostics::Tick(const FGeometry& G,double At,float Delta){SCompoundWidget::Tick(G,At,Delta);Session->Poll();RefreshLevels();}
void SStudioHome4SpatialDiagnostics::Dialog(){FString Path;if(!StudioFileDialog::DataFile(false,TEXT("Import original HOME4 spatial diagnostics"),TEXT(""),TEXT("json"),Path)){ActionStatus=TEXT("Import cancelled; previous source retained.");return;}BeginImportPath(Path);}
const FStudioHome4SpatialPatch* SStudioHome4SpatialDiagnostics::Patch()const{const auto E=Session->Evidence();return E&&E->Patches.IsValidIndex(SelectedPatch)?&E->Patches[SelectedPatch]:nullptr;}
const FStudioHome4SpatialZone* SStudioHome4SpatialDiagnostics::Zone()const{const auto E=Session->Evidence();return E&&E->Zones.IsValidIndex(SelectedZone)?&E->Zones[SelectedZone]:nullptr;}
FString SStudioHome4SpatialDiagnostics::SourceText()const
{
    const auto E=Session->Evidence();if(!E)return TEXT("Spatial measurements unavailable. Import identified original evidence; the development adapter supplies no science.");
    return TEXT("Original run ")+E->RunId.ToString()+TEXT(" · source ")+E->SourceId+TEXT("\nOriginal XYZ · ")+E->CoordinateUnit+TEXT(" · ")+E->GateStatus()+TEXT("\nOriginal path ")+E->SourcePath+TEXT("\nSHA256 ")+E->SourceSHA256;
}
FString SStudioHome4SpatialDiagnostics::PatchText()const
{
    using namespace StudioHome4SpatialWidgetPrivate;const auto* P=Patch();if(!P)return TEXT("Original patch metadata unavailable.");
    FString Result=P->Id+FString::Printf(TEXT(" · level %d"),P->Level)+TEXT("\nOrigin ")+Vector(P->Origin)+TEXT(" · spacing ")+Vector(P->Spacing)+TEXT("\nOriginal extents ")+(P->Extents?P->Extents->ToString():TEXT("Unavailable"))+TEXT(" · reported cells ")+Count(P->CellCount);
    for(const auto& M:P->Masks)Result+=TEXT("\n")+M.Kind+TEXT(" mask · count ")+Count(M.Count)+TEXT(" · original field ")+Text(M.FieldId);
    return Result+TEXT("\nMask arrays are available only through separately identified original field recordings.");
}
FString SStudioHome4SpatialDiagnostics::LevelText()const
{
    using namespace StudioHome4SpatialWidgetPrivate;const auto E=Session->Evidence();if(!E||E->Levels.IsEmpty())return TEXT("Per-level measurements unavailable.");FString Result;
    for(const auto& L:E->Levels)
    {
        if(L.Level!=SelectedLevel)continue;
        FStudioHome4Sample S;S.Work=L.Work;const auto P=FStudioHome4Diagnostics::Performance(S);
        Result+=(Result.IsEmpty()?TEXT(""):TEXT("\n\n"))+FString::Printf(TEXT("Level %d · acoustic scale 2^%d = %.6g"),L.Level,L.Level,FMath::Pow(2.,L.Level))+TEXT("\nnu ")+Value(L.Nu)+TEXT(" · sigma ")+Value(L.Sigma)+TEXT(" · M ")+Value(L.Mobility)+TEXT(" · g ")+Value(L.Gravity)+TEXT(" · tau ")+Value(L.Tau)+
            TEXT("\nActual substeps ")+Count(L.Substeps)+TEXT(" · measured ")+Number(P.MLUPSInstant)+TEXT(" MLUPS · ")+Number(P.GigabytesPerSecond)+TEXT(" GB/s")+TEXT("\nTau floor ")+Flag(L.TauFloor)+TEXT(" (physics choice) · phase-free root ")+Flag(L.RootPhaseFree)+TEXT(" · even wrap ")+Flag(L.EvenWrap)+TEXT(" · Sneq ")+Text(L.SneqMode)+
            TEXT("\nBand ")+Value(L.BandDepth)+TEXT(" · overlap ")+Value(L.Overlap)+TEXT(" · restriction margin ")+Value(L.RestrictionMargin)+TEXT(" · sponge ")+Value(L.SpongeStrength);
    }
    if(Result.IsEmpty())return TEXT("Choose a measured level in the table to inspect its original settings.");
    return Result+TEXT("\nScale labels describe acoustic refinement; supplied source parameters are not replaced by inferred values.");
}
FString SStudioHome4SpatialDiagnostics::ZoneText()const
{
    using namespace StudioHome4SpatialWidgetPrivate;const auto* Z=Zone();if(!Z)return TEXT("Original zone measurements unavailable.");
    FString Result=Z->Id+TEXT(" · ")+Z->Kind+TEXT("\nMinimum ")+Vector(Z->Minimum)+TEXT(" · maximum ")+Vector(Z->Maximum)+TEXT("\nProfile ")+Text(Z->ProfileAxis)+TEXT(" · ")+Text(Z->ProfileCoordinateUnit)+TEXT(" / ")+Text(Z->ProfileValueUnit)+FString::Printf(TEXT(" · %d original samples"),Z->ProfileX.Num());
    TArray<int32> Levels;Z->LevelStrengths.GetKeys(Levels);Levels.Sort();for(int32 L:Levels)Result+=FString::Printf(TEXT("\nLevel %d strength "),L)+Value(Z->LevelStrengths[L]);return Result;
}
FString SStudioHome4SpatialDiagnostics::GeometryText()const
{
    using namespace StudioHome4SpatialWidgetPrivate;const auto E=Session->Evidence();if(!E||E->Geometry.IsEmpty())return TEXT("Original geometry-adapter diagnostics unavailable.");FString Result;
    for(const auto& G:E->Geometry)Result+=(Result.IsEmpty()?TEXT(""):TEXT("\n\n"))+G.BodyId+TEXT(" · CAD ")+Text(G.CADSource)+TEXT(" · SDF ")+Text(G.SDFBackend)+TEXT("\nTessellation ")+Text(G.TessellationStatus)+TEXT(" · triangles ")+Count(G.TessellationTriangles)+TEXT(" · cache ")+Text(G.CacheStatus)+TEXT("\nCut links ")+Text(G.CutLinkStatus)+TEXT(" · count ")+Count(G.CutLinks)+TEXT(" · fraction ")+Number(G.CutLinkFraction)+TEXT("\nFlotation ")+Text(G.FlotationStatus)+TEXT(" · held heave ")+Value(G.EquilibriumHeave)+TEXT(" · running heave ")+Value(G.RunningHeave)+TEXT("\nTrim ")+Value(G.Trim)+TEXT(" · draft ")+Value(G.Draft)+TEXT(" · buoyancy ")+Value(G.Buoyancy)+TEXT(" · weight ")+Value(G.Weight)+TEXT(" · reference ")+Text(G.ReferenceSource);
    return Result+TEXT("\nRecipe validation and restart compatibility remain not evaluated.");
}
void SStudioHome4SpatialDiagnostics::Locate()
{
    const auto E=Session->Evidence();const auto* P=Patch();if(!E||!P||!OnLocate.IsBound())return;
    TArray<FString> Parts;CellDraft.ParseIntoArray(Parts,TEXT(","),false);FIntVector Cell;
    if(Parts.Num()!=3){ActionStatus=TEXT("Enter three original patch-local integer indices: i,j,k.");return;}
    for(int32 I=0;I<3;++I)
    {
        const FString V=Parts[I].TrimStartAndEnd();if(V.IsEmpty()||V.Len()>7){ActionStatus=TEXT("Original indices must be bounded nonnegative integers.");return;}
        for(TCHAR C:V)if(C<'0'||C>'9'){ActionStatus=TEXT("Original indices must be bounded nonnegative integers.");return;}
        Cell[I]=FCString::Atoi(*V);
    }
    FStudioHome4SpatialLocation Location;if(!StudioHome4SpatialDiagnostics::Locate(*E,P->Id,Cell,Location,ActionStatus))return;
    OnLocate.Execute(Location);ActionStatus=TEXT("Original patch-cell location sent for viewer compatibility checking.");
}

void SStudioHome4SpatialDiagnostics::RefreshLevels()
{
    using namespace StudioUI;using namespace StudioHome4SpatialWidgetPrivate;
    const auto E=Session->Evidence();if(!LevelRows||DisplayedEvidence==E)return;DisplayedEvidence=E;LevelRows->ClearChildren();
    SelectedPatch=0;SelectedZone=0;SelectedLevel=E&&!E->Levels.IsEmpty()?E->Levels[0].Level:0;ActionStatus.Empty();
    const TCHAR* Headings[]={TEXT("Level"),TEXT("nu"),TEXT("sigma"),TEXT("M"),TEXT("g"),TEXT("tau"),TEXT("Substeps"),TEXT("MLUPS")};
    auto Header=SNew(SHorizontalBox);for(const auto* H:Headings)Header->AddSlot().FillWidth(1).Padding(1)[Label(H,8,Muted,true)];LevelRows->AddSlot().AutoHeight()[Header];
    if(!E)return;
    for(const auto& L:E->Levels)
    {
        FStudioHome4Sample Work;Work.Work=L.Work;const auto P=FStudioHome4Diagnostics::Performance(Work);
        const FString Values[]={FString::FromInt(L.Level),Value(L.Nu),Value(L.Sigma),Value(L.Mobility),Value(L.Gravity),Value(L.Tau),Count(L.Substeps),Number(P.MLUPSInstant)};
        auto Row=SNew(SHorizontalBox);for(const auto& V:Values)Row->AddSlot().FillWidth(1).Padding(1)[SNew(STextBlock).Font(Font(8)).AutoWrapText(true).Text(FText::FromString(V))];
        LevelRows->AddSlot().AutoHeight().Padding(0,2)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("Home4SpatialLevel%d"),L.Level))).ButtonStyle(&ButtonStyle()).ContentPadding(2)
            .OnClicked_Lambda([this,Id=L.Level]{SelectedLevel=Id;return FReply::Handled();})[Row]];
    }
}
