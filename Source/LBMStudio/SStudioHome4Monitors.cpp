#include "SStudioHome4Monitors.h"
#include "StudioHome4Runtime.h"
#include "StudioHome4BodyDiagnostics.h"
#include "StudioHome4EnergyBudget.h"
#include "SStudioHome4EnergyBudget.h"
#include "StudioModel.h"
#include "StudioFileDialog.h"
#include "StudioTheme.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Images/SImage.h"
#include "Rendering/DrawElements.h"
#include "InputCoreTypes.h"
#include <cerrno>
#include <cstdlib>
#include <memory>
#define UI UI_HOME4_SCIENCE_IMPORT
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioHome4MonitorPrivate
{
    FString Number(const TOptional<double>& V, const TCHAR* Unit = TEXT(""))
    { return V ? FString::Printf(TEXT("%.6g%s%s"), *V, *Unit ? TEXT(" ") : TEXT(""), Unit) : TEXT("Unavailable"); }
    FString Count(const TOptional<int64>& V)
    { return V ? FString::Printf(TEXT("%lld"), *V) : TEXT("Unavailable"); }
    FString Fact(const TOptional<bool>& V)
    { return V ? (*V ? TEXT("yes") : TEXT("no")) : TEXT("unavailable"); }
    FString Cell(const TOptional<FIntVector>& V)
    { return V ? FString::Printf(TEXT("[%d, %d, %d]"), V->X, V->Y, V->Z) : TEXT("Unavailable"); }
    const TCHAR* KindName(EStudioHome4OutputKind Kind)
    {
        switch (Kind)
        {
        case EStudioHome4OutputKind::Trace: return TEXT("Trace");
        case EStudioHome4OutputKind::Slice: return TEXT("Slice");
        case EStudioHome4OutputKind::Visualization: return TEXT("Visualization snapshot");
        case EStudioHome4OutputKind::Restart: return TEXT("Restart state");
        }
        return TEXT("Unknown");
    }
    bool PolicyNumber(const FString& Text, TOptional<double>& Out)
    {
        const FString Trimmed = Text.TrimStartAndEnd();
        if (Trimmed.IsEmpty()) { Out.Reset(); return true; }
        if (Trimmed.Len() > 64) return false;
        int32 I = 0, Digits = 0;
        auto Digit = [](TCHAR C) { return C >= '0' && C <= '9'; };
        if (Trimmed[I] == '+') ++I;
        while (I < Trimmed.Len() && Digit(Trimmed[I])) { ++I; ++Digits; }
        if (I < Trimmed.Len() && Trimmed[I] == '.')
        { ++I; while (I < Trimmed.Len() && Digit(Trimmed[I])) { ++I; ++Digits; } }
        if (!Digits) return false;
        if (I < Trimmed.Len() && (Trimmed[I] == 'e' || Trimmed[I] == 'E'))
        {
            ++I;
            if (I < Trimmed.Len() && (Trimmed[I] == '+' || Trimmed[I] == '-')) ++I;
            int32 Exponents = 0;
            while (I < Trimmed.Len() && Digit(Trimmed[I])) { ++I; ++Exponents; }
            if (!Exponents) return false;
        }
        if (I != Trimmed.Len()) return false;
        const FTCHARToUTF8 UTF8(*Trimmed);
        char* End = nullptr; errno = 0;
        const double N = std::strtod(UTF8.Get(), &End);
        if (errno == ERANGE || End != UTF8.Get() + UTF8.Length() || !FMath::IsFinite(N) || N < 0) return false;
        Out = N; return true;
    }
    class SHome4HistoryPlot final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SHome4HistoryPlot) {}
            SLATE_ARGUMENT(TFunction<StudioHome4SciencePresentation::FHistory()>, Read)
        SLATE_END_ARGS()
        void Construct(const FArguments& A) { Read = A._Read; }
        FVector2D ComputeDesiredSize(float) const override { return FVector2D(300, 125); }
        int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool) const override
        {
            using namespace StudioUI;
            const auto H=Read();
            auto TextAt=[&](const FString& Value,FVector2D At){FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(FVector2D(1,1),FSlateLayoutTransform(At)),Value,Font(8),ESlateDrawEffect::None,Muted);};
            if(H.X.Num()<2){TextAt(TEXT("History unavailable · two original samples required"),FVector2D(0,20));return Layer+2;}
            double Low=TNumericLimits<double>::Max(),High=-TNumericLimits<double>::Max();
            for(const auto& Series:H.Series)for(const auto& V:Series.Values)if(V){Low=FMath::Min(Low,*V);High=FMath::Max(High,*V);}
            if(Low==TNumericLimits<double>::Max()){TextAt(TEXT("Selected measured history unavailable"),FVector2D(0,20));return Layer+2;}
            const double Range=High-Low,XRange=H.X.Last()-H.X[0];
            if(!FMath::IsFinite(Range)||!FMath::IsFinite(XRange)){TextAt(TEXT("Source range exceeds finite chart scale"),FVector2D(0,20));return Layer+2;}
            const double Pad=FMath::Max(FMath::Max(FMath::Abs(Low),FMath::Abs(High))*.05,1e-12);
            if(!FMath::IsFinite(Low-Pad)||!FMath::IsFinite(High+Pad)){TextAt(TEXT("Source range exceeds finite chart scale"),FVector2D(0,20));return Layer+2;}
            Low-=Pad;High+=Pad;
            const auto Size=G.GetLocalSize();const double Left=55,Right=FMath::Max(Left+1,Size.X-3),Top=6,Bottom=FMath::Max(Top+1,Size.Y-22);
            for(int32 I=0;I<2;++I)
            {
                const double Y=I?Bottom:Top;TArray<FVector2D> Points{{Left,Y},{Right,Y}};
                FSlateDrawElement::MakeLines(Out,Layer,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Muted.CopyWithNewOpacity(.25),true,1);
                TextAt(FString::Printf(TEXT("%.3g"),I?Low:High),FVector2D(0,FMath::Max(0.,Y-5)));
            }
            TextAt(FString::Printf(TEXT("%.4g"),H.X[0]),FVector2D(Left,Bottom+4));
            TextAt(H.Axis,FVector2D(FMath::Max(Left,Right-115),Bottom+4));
            for(const auto& Series:H.Series)
            {
                TArray<FVector2D> Points;
                auto Flush=[&]{if(Points.Num()>1)FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Series.Color,true,1.5);
                    else if(Points.Num()==1)FSlateDrawElement::MakeBox(Out,Layer+1,G.ToPaintGeometry(FVector2D(3,3),FSlateLayoutTransform(Points[0]-FVector2D(1.5,1.5))),&PanelBrush,ESlateDrawEffect::None,Series.Color);Points.Reset();};
                for(int32 I=0;I<Series.Values.Num();++I)
                {
                    if(!Series.Values[I]){Flush();continue;}
                    Points.Add(FVector2D(Left+(Right-Left)*(H.X[I]-H.X[0])/FMath::Max(1e-12,XRange),Bottom-(Bottom-Top)*(*Series.Values[I]-Low)/(High-Low)));
                }
                Flush();
            }
            return Layer+2;
        }
    private:
        TFunction<StudioHome4SciencePresentation::FHistory()> Read;
    };
    class SHome4BudgetBars final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SHome4BudgetBars) {}
            SLATE_ARGUMENT(TFunction<TArray<StudioHome4SciencePresentation::FBar>()>, Read)
        SLATE_END_ARGS()
        void Construct(const FArguments& A){Read=A._Read;}
        FVector2D ComputeDesiredSize(float) const override{return FVector2D(300,180);}
        int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool)const override
        {
            using namespace StudioUI;const auto Bars=Read();double Max=0;
            for(const auto& B:Bars)if(B.Value.Number)Max=FMath::Max(Max,FMath::Abs(*B.Value.Number));
            auto TextAt=[&](const FString& Value,FVector2D At,FLinearColor Color){FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(FVector2D(1,1),FSlateLayoutTransform(At)),Value,Font(8),ESlateDrawEffect::None,Color);};
            if(Bars.IsEmpty()){TextAt(TEXT("Budget terms unavailable"),FVector2D(0,15),Muted);return Layer+2;}
            const double Width=G.GetLocalSize().X,Center=Width*.55,Span=FMath::Max(5.,Width*.20);
            TArray<FVector2D> Zero{{Center,0},{Center,180}};FSlateDrawElement::MakeLines(Out,Layer,G.ToPaintGeometry(),Zero,ESlateDrawEffect::None,Muted.CopyWithNewOpacity(.3),true,1);
            for(int32 I=0;I<Bars.Num();++I)
            {
                const auto& B=Bars[I];const double Y=I*20.;TextAt(B.Label,FVector2D(0,Y),B.Color);
                if(B.Value.Number)
                {
                    const double Length=Max>0?Span*(*B.Value.Number/Max):0;
                    FSlateDrawElement::MakeBox(Out,Layer,G.ToPaintGeometry(FVector2D(FMath::Max(1.,FMath::Abs(Length)),9),FSlateLayoutTransform(FVector2D(Center+FMath::Min(0.,Length),Y+3))),&PanelBrush,ESlateDrawEffect::None,B.Color.CopyWithNewOpacity(.65));
                    TextAt(FString::Printf(TEXT("%.4g"),*B.Value.Number),FVector2D(Width-58,Y),Text);
                }
                else TextAt(TEXT("Unavailable"),FVector2D(Width-85,Y),Muted);
            }
            return Layer+2;
        }
    private:TFunction<TArray<StudioHome4SciencePresentation::FBar>()> Read;
    };
    class SHome4ThicknessHistogram final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SHome4ThicknessHistogram) : _UnitDisplay(EStudioHome4UnitDisplay::Lattice) {}
            SLATE_ARGUMENT(TFunction<const FStudioHome4Sample*()>, Read)
            SLATE_ATTRIBUTE(EStudioHome4UnitDisplay, UnitDisplay)
        SLATE_END_ARGS()
        void Construct(const FArguments& A){Read=A._Read;Display=A._UnitDisplay;}
        FVector2D ComputeDesiredSize(float) const override{return FVector2D(300,100);}
        int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool)const override
        {
            using namespace StudioUI;const auto* S=Read();
            auto TextAt=[&](const FString& Value,FVector2D At){FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(FVector2D(1,1),FSlateLayoutTransform(At)),Value,Font(8),ESlateDrawEffect::None,Muted);};
            if(!S||S->bNonfinite||S->InterfaceThickness.Counts.IsEmpty()){TextAt(TEXT("Thickness histogram unavailable"),FVector2D(0,20));return Layer+2;}
            const auto& H=S->InterfaceThickness;int64 Max=0;for(int64 N:H.Counts)Max=FMath::Max(Max,N);
            const double Width=G.GetLocalSize().X,Range=H.BinEdges.Last()-H.BinEdges[0];
            if(!FMath::IsFinite(Range)||Range<=0){TextAt(TEXT("Histogram range unavailable"),FVector2D(0,20));return Layer+2;}
            for(int32 I=0;I<H.Counts.Num();++I)
            {
                const double Height=Max?65.*double(H.Counts[I])/double(Max):0;
                FSlateDrawElement::MakeBox(Out,Layer,G.ToPaintGeometry(FVector2D(FMath::Max(1.,Width*(H.BinEdges[I+1]-H.BinEdges[I])/Range-2),FMath::Max(1.,Height)),FSlateLayoutTransform(FVector2D(Width*(H.BinEdges[I]-H.BinEdges[0])/Range,70-Height))),&PanelBrush,ESlateDrawEffect::None,Cyan.CopyWithNewOpacity(.65));
            }
            const auto First=StudioHome4SciencePresentation::Quantity(H.BinEdges[0],*S,EStudioHome4Quantity::Length,Display.Get());
            const auto Last=StudioHome4SciencePresentation::Quantity(H.BinEdges.Last(),*S,EStudioHome4Quantity::Length,Display.Get());
            TextAt(StudioHome4SciencePresentation::Text(First),FVector2D(0,78));TextAt(StudioHome4SciencePresentation::Text(Last),FVector2D(FMath::Max(0.,Width-180),78));
            return Layer+2;
        }
    private:TFunction<const FStudioHome4Sample*()> Read;TAttribute<EStudioHome4UnitDisplay> Display;
    };

}

void SStudioHome4Monitors::Construct(const FArguments& A)
{
    using namespace StudioUI;
    using namespace StudioHome4MonitorPrivate;
    Model = A._Model; Runtime = A._Runtime; SessionStream = Runtime ? Runtime->ScienceStream() : A._Stream; OnLocate = A._OnLocateCell; UnitDisplay = A._UnitDisplay;
    if (const auto M = Model.Pin()) { ScopedProjectId = M->Project.Id; ScopedCaseId = M->Project.Draft.Id; }
    PolicyDraft.SetNum(9);
    Status = TEXT("Choose an original HOME4 JSONL log, or connect a session science stream.");
    auto Rows = SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 8)[SNew(STextBlock).Tag(TEXT("Home4MonitorSource")).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this] { return FText::FromString(SourceText()); })];
    for (int32 I = 0; I < 5; ++I)
    {
        const FName Tag(*FString::Printf(TEXT("Home4Health%d"), I));
        Rows->AddSlot().AutoHeight().Padding(0, 5, 0, 5)[SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Tag(Tag).Font(Font(11, true))
                .ColorAndOpacity_Lambda([this, I] { const auto H = Health(I); return FSlateColor(H.Status == EStudioHome4Health::Healthy ? Cyan : H.Status == EStudioHome4Health::Warning ? Amber : Muted); })
                .Text_Lambda([this, I] { const auto H = Health(I); const TCHAR* State = H.Status == EStudioHome4Health::Healthy ? TEXT("Healthy") : H.Status == EStudioHome4Health::Warning ? TEXT("Warning") : TEXT("Unavailable");
                    return FText::FromString(H.Label + TEXT(" · ") + State + (H.Value ? TEXT(" · ") + Number(H.Value) : TEXT(""))); })]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 2)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true)
                .Text_Lambda([this, I] { const auto H = Health(I); return FText::FromString(H.Reason + (H.Threshold ? TEXT(" Threshold: ") + Number(H.Threshold) : TEXT(""))); })]
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
                .Text_Lambda([this, I] { return FText::FromString(Health(I).Remedy); })]];
    }
    Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 6)[SNew(SButton).Tag(TEXT("Home4TailTelemetry")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda([this] { return Runtime.IsValid(); })
        .OnClicked_Lambda([this]
        {
            FGuid Run; if (!Runtime || !FGuid::Parse(OriginalRunIdDraft, Run) || !Run.IsValid()) { Status = TEXT("Live tail requires an explicit original run GUID and owning runtime."); return FReply::Handled(); }
            FString Path; if (StudioFileDialog::DataFile(false, TEXT("Attach original growing HOME4 JSONL"), TEXT(""), TEXT("jsonl"), Path))
                if (Runtime->AttachLiveLog(Path, Run, Status)) { SessionStream = Runtime->ScienceStream(); bShowImported = false; RefreshOutputs(); }
            return FReply::Handled();
        })[Label(TEXT("Tail original live JSONL…"), 9)]];
    auto Section=[&](const TCHAR* Title,FName Key,bool Open)
    {
        Sections.Add(Key,Open);auto Content=SNew(SVerticalBox);
        Rows->AddSlot().AutoHeight().Padding(0,12,0,4)[SNew(SButton).Tag(FName(*(TEXT("Home4Section_")+Key.ToString()))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,5))
            .OnClicked_Lambda([this,Key]{Sections[Key]=!Sections[Key];return FReply::Handled();})
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,6,0)
                    [SNew(SImage).ColorAndOpacity(Text).Image_Lambda([this,Key]{return FCoreStyle::Get().GetBrush(Sections[Key]?TEXT("TreeArrow_Expanded"):TEXT("TreeArrow_Collapsed"));})]
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                    [SNew(STextBlock).Font(Font(11,true)).ColorAndOpacity(Text).Text(FText::FromString(Title))]]];
        Rows->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([this,Key]{return Sections[Key]?EVisibility::Visible:EVisibility::Collapsed;})[Content]];
        return Content;
    };
    auto Detail=[&](const TSharedRef<SVerticalBox>& Content,FName Key)
    {Content->AddSlot().AutoHeight()[SNew(STextBlock).Tag(FName(*(TEXT("Home4Detail_")+Key.ToString()))).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true).Text_Lambda([this,Key]{return FText::FromString(DetailText(Key));}).ToolTipText_Lambda([this,Key]{return FText::FromString(ScienceTooltips(Key));})];};
    auto Budget=Section(TEXT("Energy budget and residual"),TEXT("Budget"),true);Detail(Budget,TEXT("Budget"));
    Budget->AddSlot().AutoHeight().Padding(0,6)[SNew(SHome4BudgetBars).Read([this]{return StudioHome4SciencePresentation::Budget(Sample(),UnitDisplay.Get(),SelectedPhase);})];
    if(A._Editor||A._Model)Budget->AddSlot().AutoHeight().Padding(0,6)[SNew(SStudioHome4EnergyBudget).Model(A._Model).Editor(A._Editor)];
    auto Forces=Section(TEXT("Independent force channels"),TEXT("Forces"),true);
    auto Components=SNew(SHorizontalBox);const TCHAR* Names[]={TEXT("Fx"),TEXT("Fy"),TEXT("Fz"),TEXT("My")};
    for(int32 I=0;I<4;++I)Components->AddSlot().FillWidth(1).Padding(0,0,4,0)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("Home4PlotComponent%d"),I))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(5,4))
        .OnClicked_Lambda([this,I]{PlotComponent=I;return FReply::Handled();})[SNew(STextBlock).Font(Font(9)).Text(FText::FromString(Names[I]))
            .ColorAndOpacity_Lambda([this,I]{return FSlateColor(PlotComponent==I?Cyan:Muted);})]];
    Forces->AddSlot().AutoHeight()[Components];
    Forces->AddSlot().AutoHeight().Padding(0,5)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SButton).Tag(TEXT("Home4SelectBody")).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([this]{CycleBody();return FReply::Handled();})
            [SNew(STextBlock).Font(Font(9)).Text_Lambda([this]{return FText::FromString(SelectedBody.IsEmpty()?TEXT("All-body source forces · choose body"):TEXT("Body: ")+SelectedBody);})]]
        +SHorizontalBox::Slot().AutoWidth().Padding(5,0)[SNew(SButton).Tag(TEXT("Home4NormalizeForces")).ButtonStyle(&ButtonStyle())
            .IsEnabled_Lambda([this]{return bNormalizeForces||StudioHome4SciencePresentation::CanNormalize(Sample(),PlotComponent,SelectedBody);})
            .OnClicked_Lambda([this]{bNormalizeForces=!bNormalizeForces;return FReply::Handled();})
            [SNew(STextBlock).Font(Font(9)).Text_Lambda([this]{return FText::FromString(bNormalizeForces?TEXT("Benchmark normalization"):TEXT("Source units"));})]]];
    Detail(Forces,TEXT("Forces"));
    Forces->AddSlot().AutoHeight().Padding(0,5)[SNew(STextBlock).Tag(TEXT("Home4ForceCaption")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(HistoryCaption(true));})];
    auto Legend=SNew(SHorizontalBox);const TCHAR* Channels[]={TEXT("Stress"),TEXT("Momentum"),TEXT("Pressure"),TEXT("Viscous"),TEXT("Difference")};
    const FLinearColor Colors[]={Cyan,Amber,FLinearColor(.63,.54,.95),FLinearColor(.4,.8,.52),Text};
    for(int32 I=0;I<5;++I)Legend->AddSlot().FillWidth(1)[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Colors[I]).Text(FText::FromString(Channels[I]))];
    Forces->AddSlot().AutoHeight()[Legend];Forces->AddSlot().AutoHeight()[SNew(SHome4HistoryPlot).Read([this]{return PresentedForces();})];
    auto History=Section(TEXT("Measured history"),TEXT("History"),true);
    History->AddSlot().AutoHeight().Padding(0,4)[Label(TEXT("Replay interval in original solver steps; empty bounds load the latest preview. Exact original log remains bundled in reports."),8,Muted)];
    for(bool Start:{true,false})History->AddSlot().AutoHeight().Padding(0,3)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1)[Label(Start?TEXT("First original step"):TEXT("Last original step"),8,Muted)]
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).Tag(Start?TEXT("Home4ReplayStepStart"):TEXT("Home4ReplayStepEnd")).Style(&InputStyle()).Font(Font(9))
            .Text_Lambda([this,Start]{return FText::FromString(Start?ReplayStartDraft:ReplayEndDraft);}).OnTextChanged_Lambda([this,Start](const FText& T){(Start?ReplayStartDraft:ReplayEndDraft)=T.ToString();})]];
    History->AddSlot().AutoHeight().Padding(0,4)[SNew(SButton).Tag(TEXT("Home4LoadReplayInterval")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda([this]{return ImportedStream.IsValid()&&!IsImporting();})
        .OnClicked_Lambda([this]{LoadReplayInterval();return FReply::Handled();})[Label(TEXT("Load verified original replay interval"),9)]];
    History->AddSlot().AutoHeight()[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4PreviousMetric")).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([this]{SelectedMetric=StudioHome4SciencePresentation::EMetric((int32(SelectedMetric)+int32(StudioHome4SciencePresentation::EMetric::Count)-1)%int32(StudioHome4SciencePresentation::EMetric::Count));return FReply::Handled();})[Label(TEXT("Previous"),8)]]
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(6,0)[SNew(STextBlock).Tag(TEXT("Home4SelectedMetric")).Font(Font(10,true)).Text_Lambda([this]{return FText::FromString(StudioHome4SciencePresentation::Name(SelectedMetric));})]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4NextMetric")).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([this]{SelectedMetric=StudioHome4SciencePresentation::EMetric((int32(SelectedMetric)+1)%int32(StudioHome4SciencePresentation::EMetric::Count));return FReply::Handled();})[Label(TEXT("Next"),8)]]];
    History->AddSlot().AutoHeight().Padding(0,5)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SButton).Tag(TEXT("Home4SelectLevel")).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([this]{CycleLevel();return FReply::Handled();})[SNew(STextBlock).Font(Font(9)).Text_Lambda([this]{return FText::FromString(FString::Printf(TEXT("Level %d"),SelectedLevel));})]]
        +SHorizontalBox::Slot().FillWidth(1).Padding(5,0)[SNew(SButton).Tag(TEXT("Home4SelectPhase")).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([this]{CyclePhase();return FReply::Handled();})[SNew(STextBlock).Font(Font(9)).Text_Lambda([this]{return FText::FromString(SelectedPhase.IsEmpty()?TEXT("Root energy · choose phase"):TEXT("Phase: ")+SelectedPhase);})]]];
    History->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4HistoryCaption")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(HistoryCaption(false));})];
    History->AddSlot().AutoHeight()[SNew(SHome4HistoryPlot).Read([this]{return PresentedHistory();})];
    Detail(Section(TEXT("Mass ledgers and per-level work"),TEXT("Mass"),false),TEXT("Mass"));
    Detail(Section(TEXT("Interface and phase energies"),TEXT("Interface"),false),TEXT("Interface"));
    Rows->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([this]{return Sections[TEXT("Interface")]?EVisibility::Visible:EVisibility::Collapsed;})[SNew(SHome4ThicknessHistogram).Read([this]{return Sample();}).UnitDisplay_Lambda([this]{return UnitDisplay.Get();})]];
    auto Bodies=Section(TEXT("Body state, attitude and fitted coefficients"),TEXT("Bodies"),false);Detail(Bodies,TEXT("Bodies"));
    for(const auto Metric:{StudioHome4SciencePresentation::EMetric::BodyHeaveComparison,StudioHome4SciencePresentation::EMetric::BodyPitchComparison,StudioHome4SciencePresentation::EMetric::AddedMass,StudioHome4SciencePresentation::EMetric::Damping})
    {
        Bodies->AddSlot().AutoHeight().Padding(0,6,0,2)[Label(StudioHome4SciencePresentation::Name(Metric),9,Muted)];
        Bodies->AddSlot().AutoHeight()[SNew(SHome4HistoryPlot).Tag(FName(*FString::Printf(TEXT("Home4BodyComparison%d"),int32(Metric))))
            .Read([this,Metric]{return StudioHome4SciencePresentation::History(DisplayStream(),Metric,UnitDisplay.Get(),0,false,SelectedBody);})];
    }
    Detail(Section(TEXT("Current [previous] averaging window"),TEXT("Window"),false),TEXT("Window"));
    Detail(Section(TEXT("Measured work and bandwidth"),TEXT("Performance"),false),TEXT("Performance"));
    Detail(Section(TEXT("Safeguards and extrema"),TEXT("Safeguards"),false),TEXT("Safeguards"));
    Rows->AddSlot().AutoHeight().Padding(0,6)[SNew(SButton).Tag(TEXT("Home4QueueRestTest")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda([this]{return Runtime.IsValid()&&Model.IsValid();})
        .OnClicked_Lambda([this]{QueueRestTest();return FReply::Handled();})[Label(TEXT("Queue U=0 / full-gravity WB rest request"),9)]];
    auto Trouble=Section(TEXT("Trouble locator"),TEXT("Trouble"),false);Detail(Trouble,TEXT("Trouble"));
    Trouble->AddSlot().AutoHeight().Padding(0,6)[SNew(SButton).Tag(TEXT("Home4LocateCell")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,5))
        .IsEnabled_Lambda([this]{return CanLocate();}).OnClicked_Lambda([this]{Locate();return FReply::Handled();})[Label(TEXT("Locate reported cell"),9)]];
    auto Outputs=Section(TEXT("Four output kinds"),TEXT("Outputs"),false);Outputs->AddSlot().AutoHeight()[SAssignNew(OutputRows,SVerticalBox).Tag(TEXT("Home4OutputTimeline"))];
    auto Thresholds=Section(TEXT("Health thresholds"),TEXT("Thresholds"),false);
    Thresholds->AddSlot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Mass: |drift| < 1e-4. All health values and thresholds use original source units, independent of the chart unit view. Other gates need explicit limits."),8,Muted)];
    const TCHAR* Captions[]={TEXT("Budget absolute tolerance"),TEXT("Force relative tolerance"),TEXT("Force absolute tolerance"),TEXT("Force reference magnitude"),
        TEXT("Window relative tolerance"),TEXT("Window absolute tolerance"),TEXT("Window reference magnitude"),TEXT("WB rest pressure tolerance"),TEXT("Maximum speed trouble trigger")};
    for(int32 I=0;I<UE_ARRAY_COUNT(Captions);++I)Thresholds->AddSlot().AutoHeight().Padding(0,3)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(Captions[I],9,Muted)]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(120)[SNew(SEditableTextBox).Tag(FName(*FString::Printf(TEXT("Home4Policy%d"),I))).Style(&InputStyle()).Font(Font(9)).HintText(FText::FromString(TEXT("Unavailable")))
            .Text_Lambda([this,I]{return FText::FromString(PolicyDraft[I]);}).OnTextChanged_Lambda([this,I](const FText& T){PolicyDraft[I]=T.ToString();})]]];
    for(bool Force:{true,false})
    {
        Thresholds->AddSlot().AutoHeight().Padding(0,7,0,3)[Label(Force?TEXT("Force comparison component"):TEXT("Window comparison component"),9,Muted)];auto Buttons=SNew(SHorizontalBox);
        for(int32 I=0;I<4;++I)Buttons->AddSlot().FillWidth(1).Padding(0,0,5,0)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("Home4%sComponent%d"),Force?TEXT("Force"):TEXT("Window"),I))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(6,4))
            .OnClicked_Lambda([this,Force,I]{(Force?Policy.ForceComponent:Policy.WindowComponent)=FStudioHome4DiagnosticPolicy::EComponent(I);return FReply::Handled();})
            [SNew(STextBlock).Font(Font(9)).Text(FText::FromString(Names[I]))
                .ColorAndOpacity_Lambda([this,Force,I]{return FSlateColor(int32(Force?Policy.ForceComponent:Policy.WindowComponent)==I?Cyan:Muted);})]];
        Thresholds->AddSlot().AutoHeight()[Buttons];
    }
    Thresholds->AddSlot().AutoHeight().Padding(0,7,0,12)[SNew(SButton).Tag(TEXT("Home4ApplyPolicy")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,5))
        .OnClicked_Lambda([this]{ApplyPolicy();return FReply::Handled();})[Label(TEXT("Apply health thresholds"),9)]];
    ChildSlot[SNew(SBorder).BorderImage(&PanelBrush).Padding(14)[SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[Label(TEXT("HOME4 Monitors"), 17, Text, true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Original run ID (optional GUID)"), 9, Muted)]
            + SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).Tag(TEXT("Home4OriginalRunId")).Style(&InputStyle()).Font(Font(9))
                .HintText(FText::FromString(TEXT("Replay stays unbound when omitted")))
                .Text_Lambda([this] { return FText::FromString(OriginalRunIdDraft); })
                .OnTextChanged_Lambda([this](const FText& T) { OriginalRunIdDraft = T.ToString(); })]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8)[SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4ImportTelemetry")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
                .IsEnabled_Lambda([this] { return !IsImporting(); }).OnClicked_Lambda([this] { ImportDialog(); return FReply::Handled(); })[Label(TEXT("Import JSONL"), 9)]]
            + SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SButton).Tag(TEXT("Home4CancelTelemetryImport")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
                .IsEnabled_Lambda([this] { return IsImporting(); }).OnClicked_Lambda([this] { CancelImport(); return FReply::Handled(); })[Label(TEXT("Cancel read"), 9)]]
            + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4SessionTelemetry")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
                .IsEnabled_Lambda([this] { return SessionStream.IsValid(); }).OnClicked_Lambda([this] { bShowImported = false; RefreshOutputs(); return FReply::Handled(); })[Label(TEXT("Session stream"), 9)]]
            + SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SButton).Tag(TEXT("Home4ReplayTelemetry")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
                .IsEnabled_Lambda([this] { return ImportedStream.IsValid(); }).OnClicked_Lambda([this] { bShowImported = true; RefreshOutputs(); return FReply::Handled(); })[Label(TEXT("Imported replay"), 9)]]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[SNew(STextBlock).Tag(TEXT("Home4MonitorStatus")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
            .Text_Lambda([this] { return FText::FromString(Status); })]
        + SVerticalBox::Slot().FillHeight(1)[SAssignNew(Scroll, SScrollBox).Tag(TEXT("Home4MonitorScroll"))
            .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll) + SScrollBox::Slot()[Rows]]]];
    RefreshOutputs();
}

SStudioHome4Monitors::~SStudioHome4Monitors() { CancelImport(); }
void SStudioHome4Monitors::Tick(const FGeometry& G, double At, float Delta)
{ SCompoundWidget::Tick(G, At, Delta); ScopeProject(); if (Runtime) { Runtime->Scope(); SessionStream = Runtime->ScienceStream(); } PollImport(); RefreshOutputs(); }
FReply SStudioHome4Monitors::OnKeyDown(const FGeometry& G, const FKeyEvent& E)
{
    if (E.GetKey() == EKeys::Home) { Scroll->ScrollToStart(); return FReply::Handled(); }
    if (E.GetKey() == EKeys::End) { Scroll->ScrollToEnd(); return FReply::Handled(); }
    if (E.GetKey() == EKeys::PageUp || E.GetKey() == EKeys::PageDown)
    { Scroll->SetScrollOffset(FMath::Max(0.f, Scroll->GetScrollOffset() + (E.GetKey() == EKeys::PageDown ? 1.f : -1.f) * Scroll->GetCachedGeometry().GetLocalSize().Y * .8f)); return FReply::Handled(); }
    return SCompoundWidget::OnKeyDown(G, E);
}
FStudioHome4TelemetryStream* SStudioHome4Monitors::DisplayStream() const
{ return IsImportedReplay() ? ImportedStream.Get() : SessionStream.Get(); }
TOptional<FGuid> SStudioHome4Monitors::OriginalRunIdentity() const
{
    const auto M=Model.Pin();
    if(M ? M->Project.Id!=ScopedProjectId||M->Project.Draft.Id!=ScopedCaseId : ScopedProjectId.IsValid()||ScopedCaseId.IsValid())return {};
    if (IsImportedReplay() && !bImportedOriginalRunIdentity) return {};
    const auto* Stream = DisplayStream();
    if (Stream && Stream->Latest()) return Stream->Latest()->Source.RunId;
    if (Stream && !Stream->OutputEvents().IsEmpty()) return Stream->OutputEvents().Last().Source.RunId;
    return {};
}
TOptional<FStudioHome4TelemetryProvenance> SStudioHome4Monitors::ReportProvenance()
{
    ScopeProject();const auto* Stream=DisplayStream();const auto M=Model.Pin();
    if(!Stream||!M)return {};
    if (Runtime && !IsImportedReplay()) return Runtime->ScienceProvenance();
    const FStudioHome4Source* Source=nullptr;
    if(Stream->Latest())Source=&Stream->Latest()->Source;
    else if(!Stream->OutputEvents().IsEmpty())Source=&Stream->OutputEvents().Last().Source;
    if(!Source)return {};
    FStudioHome4TelemetryProvenance P;P.StreamRunId=Source->RunId;P.SourceId=Source->SourceId;
    P.bImportedReplay=IsImportedReplay();P.OriginalRunId=OriginalRunIdentity();
    if(P.bImportedReplay){P.SourcePath=ImportPath;P.SourceSHA256=ImportSHA256;P.CapturedByteCount=ImportedOriginalBytes.Num();P.SelectedStepStart=ReplayStart;P.SelectedStepEnd=ReplayEnd;}
    P.AttachedProjectId=ScopedProjectId;P.AttachedCaseId=ScopedCaseId;return P;
}
const TArray<uint8>& SStudioHome4Monitors::ReportOriginalBytes()
{
    ScopeProject();
    if(!IsImportedReplay()&&Runtime)return Runtime->CapturedScienceBytes();
    return ImportedOriginalBytes;
}
bool SStudioHome4Monitors::QueueRestTest()
{
    const auto M=Model.Pin();if(!M||!M->Project.Draft.Home4||!Runtime){Status=TEXT("Apply a HOME4 request and review a shared runtime target before the rest test.");return false;}
    FStudioHome4Spec Rest=*M->Project.Draft.Home4;
    if(!Rest.Fluids.Gravity||*Rest.Fluids.Gravity<=0){Status=TEXT("Rest test requires explicit positive full gravity; no gravity value is guessed.");return false;}
    Rest.Run.InitState.Empty();Rest.Reference.SpeedCellsPerStep=0;Rest.Reference.Mach=0;Rest.Geometry.BodyMotion=TEXT("fixed");Rest.Geometry.InitialVelocityCellsPerStep=FVector::ZeroVector;
    Rest.Geometry.InitialAngularVelocityRadiansPerStep=FVector::ZeroVector;Rest.Run.Tag=Rest.Run.Tag.Left(90)+TEXT("_wb_rest");
    Rest.Run.OutDirectory=(Rest.Run.OutDirectory.IsEmpty()?FString(TEXT("home4")):Rest.Run.OutDirectory)+TEXT("_wb_rest_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Rest.Run.VizDirectory=Rest.Run.OutDirectory/TEXT("viz");Rest.Run.SaveState.Empty();
    if(!Runtime->Submit(Rest,FGuid::NewGuid(),FPlatformProcess::UserName(),FPlatformTime::Seconds(),Status))return false;
    Status=TEXT("Immutable U=0/full-gravity WB rest development request queued. Exact-zero pressure gate awaits original at_rest measurement; no numerical result generated.");return true;
}
FString SStudioHome4Monitors::ScienceTooltips(FName Key)const
{
    const auto* S=Sample();if(!S)return TEXT("Original science unavailable; no current request units are borrowed.");
    using namespace StudioHome4SciencePresentation;FString Result;
    auto Forms=[&](const TCHAR* Name,const TOptional<double>& Value,EStudioHome4Quantity Quantity)
    {
        Result+=FString(Name)+TEXT("\nLU: ")+Text(StudioHome4SciencePresentation::Quantity(Value,*S,Quantity,EStudioHome4UnitDisplay::Lattice,false,SelectedBody))+
            TEXT("\nSI: ")+Text(StudioHome4SciencePresentation::Quantity(Value,*S,Quantity,EStudioHome4UnitDisplay::Physical,false,SelectedBody))+
            TEXT("\nNondimensional: ")+Text(StudioHome4SciencePresentation::Quantity(Value,*S,Quantity,EStudioHome4UnitDisplay::Nondimensional,false,SelectedBody))+TEXT("\n");
    };
    if(Key==TEXT("Safeguards")||Key==TEXT("Interface")){Forms(TEXT("Maximum speed"),S->MaximumSpeed,EStudioHome4Quantity::Velocity);Forms(TEXT("Spurious speed"),S->SpuriousSpeed,EStudioHome4Quantity::Velocity);}
    if(Key==TEXT("Budget")||Key==TEXT("Interface"))
    {
        const auto* Budget=SelectedPhase.IsEmpty()?&S->Budget:S->PhaseBudgets.Find(SelectedPhase);
        if(Budget){Forms(TEXT("Selected original work"),Budget->Work,EStudioHome4Quantity::Energy);Forms(TEXT("Selected original residual"),Budget->Residual,EStudioHome4Quantity::Energy);}
        Forms(TEXT("Water KE"),S->WaterKE,EStudioHome4Quantity::Energy);
    }
    if(Key==TEXT("Forces")||Key==TEXT("Window")||Key==TEXT("Bodies"))
    {
        const auto* Body=S->Bodies.FindByPredicate([this](const auto& B){return B.Id==SelectedBody;});
        const auto* F=SelectedBody.IsEmpty()?&S->Forces:Body?&Body->Forces:nullptr;
        if(Key==TEXT("Window"))
        {
            const auto* W=SelectedBody.IsEmpty()?&S->Window:Body?&Body->Window:nullptr;
            if(W){Forms(TEXT("Current window Fx"),W->Fx,EStudioHome4Quantity::Force);Forms(TEXT("Previous window Fx"),W->PreviousFx,EStudioHome4Quantity::Force);Forms(TEXT("Current window Fz"),W->Fz,EStudioHome4Quantity::Force);Forms(TEXT("Current window My"),W->My,EStudioHome4Quantity::Moment);}
        }
        if(Body&&Key==TEXT("Bodies")){Forms(TEXT("Held heave"),Body->EquilibriumHeave,EStudioHome4Quantity::Length);Forms(TEXT("Running heave"),Body->RunningHeave,EStudioHome4Quantity::Length);Forms(TEXT("Reference heave"),Body->ReferenceHeave,EStudioHome4Quantity::Length);}
        if(F){Forms(TEXT("Fx"),F->Fx,EStudioHome4Quantity::Force);Forms(TEXT("Fy"),F->Fy,EStudioHome4Quantity::Force);Forms(TEXT("Fz"),F->Fz,EStudioHome4Quantity::Force);Forms(TEXT("My"),F->My,EStudioHome4Quantity::Moment);}
    }
    return Result+TEXT("Conversions use immutable original source metadata; unavailable maps remain raw source values explicitly labeled.");
}
const FStudioHome4Sample* SStudioHome4Monitors::Sample() const
{ const auto* Stream = DisplayStream(); return Stream && Stream->Latest() ? &*Stream->Latest() : nullptr; }
FStudioHome4HealthSignal SStudioHome4Monitors::Health(int32 Index) const
{ const auto* S = Sample(); auto SelectedPolicy = Policy; SelectedPolicy.BodyId = SelectedBody; const auto H = FStudioHome4Diagnostics::Evaluate(S ? *S : FStudioHome4Sample(), SelectedPolicy); return H[Index]; }
FString SStudioHome4Monitors::SourceText() const
{
    using namespace StudioHome4MonitorPrivate;
    const auto* S = Sample();
    if (!S)
    {
        const auto M = Model.Pin();
        return M && M->Job().Capabilities().bControlHarness ? TEXT("Science telemetry unavailable. The control harness supplies no solver measurements. Import an original JSONL log to inspect replay data.") : TEXT("Science telemetry unavailable. No HOME4 numerical measurements have been supplied.");
    }
    return FString::Printf(TEXT("%s · %s\nRun %s · source %s\nStep %s · t lattice %s · t physical %s · t* %s%s"),
        IsImportedReplay() ? TEXT("Imported replay") : TEXT("Session measurements"), S->Backend.IsEmpty() ? TEXT("Backend unavailable") : *S->Backend,
        *S->Source.RunId.ToString(), *S->Source.SourceId, *Count(S->Step), *Number(S->LatticeTime), *Number(S->PhysicalTime, TEXT("s")),
        *Number(S->DimensionlessTime), IsImportedReplay() ? *FString(TEXT("\nOriginal log: ") + ImportPath + TEXT("\nOriginal SHA256: ") + ImportSHA256 +
            (bImportedOriginalRunIdentity ? TEXT("\nOriginal run ID supplied by owner; spatial binding requires an exact original-grid match.") : TEXT("\nIndependent replay identity; original run ID unavailable, spatial binding disabled.")) +
            TEXT("\nRecovery records are historical; importing never stops or checkpoints a job.")) : TEXT(""));
}
FString SStudioHome4Monitors::DetailText(FName Key) const
{
    using namespace StudioHome4MonitorPrivate;
    const auto* S = Sample();
    if (!S) return TEXT("Unavailable · requires an original science measurement.");
    if (Key == TEXT("Mass"))
    {
        FString Text = TEXT("Root drift ") + Number(S->Mass.PhiDrift) + TEXT(" · injected ") + Number(S->Mass.Injected) +
            TEXT(" · change in injection magnitude ") + Number(S->Mass.InjectionMagnitudeChange);
        const int32 N = FMath::Max(S->Mass.LevelDrifts.Num(), S->Mass.LevelInjections.Num());
        for (int32 I = 0; I < N; ++I)
            Text += FString::Printf(TEXT("\nLevel %d · drift %s · injected %s"), I,
                *Number(S->Mass.LevelDrifts.IsValidIndex(I) ? S->Mass.LevelDrifts[I] : TOptional<double>()),
                *Number(S->Mass.LevelInjections.IsValidIndex(I) ? S->Mass.LevelInjections[I] : TOptional<double>()));
        for(const auto& L:S->Levels)
        {
            FStudioHome4Sample Work;Work.Work=L.Work;const auto P=FStudioHome4Diagnostics::Performance(Work);
            Text+=FString::Printf(TEXT("\nLevel %d · drift %s · injected %s · measured %s MLUPS · reported %s MLUPS"),L.Level,*Number(L.MassDrift),*Number(L.Injection),*Number(P.MLUPSInstant),*Number(L.ReportedMLUPS));
        }
        Text+=FString(TEXT("\nCorrection-injection coverage "))+(S->Mass.InjectionMagnitudeChange?TEXT("root adjacent trend supplied"):TEXT("root trend unavailable"));
        if(S->Metadata&&!S->Metadata->DeclaredLevels.IsEmpty()){Text+=TEXT(" · declared original levels");for(int32 L:S->Metadata->DeclaredLevels)Text+=TEXT(" ")+LexToString(L);}else Text+=TEXT(" · expected original MD level inventory unknown");
        return Text;
    }
    auto Value=[&](const TOptional<double>& V,EStudioHome4Quantity Q,bool Normalize=false)
    {return StudioHome4SciencePresentation::Text(StudioHome4SciencePresentation::Quantity(V,*S,Q,UnitDisplay.Get(),Normalize,SelectedBody));};
    const auto* Body=SelectedBody.IsEmpty()?nullptr:S->Bodies.FindByPredicate([this](const auto& B){return B.Id==SelectedBody;});
    if (Key == TEXT("Budget"))
    {
        const auto* B=SelectedPhase.IsEmpty()?&S->Budget:S->PhaseBudgets.Find(SelectedPhase);
        if(!B)return TEXT("Selected phase budget unavailable at this original sample.");
        return (SelectedPhase.IsEmpty()?TEXT("Root budget"):TEXT("Phase ")+SelectedPhase)+TEXT(" · ")+Value(B->Work,EStudioHome4Quantity::Energy)+TEXT(" work")+
            TEXT("\nReported residual ")+Value(B->Residual,EStudioHome4Quantity::Energy)+TEXT(" · computed W − losses − ΔE ")+Value(StudioHome4SciencePresentation::BudgetImbalance(*B),EStudioHome4Quantity::Energy)+
            TEXT("\nOriginal energy domains: ")+(S->Metadata?StudioHome4EnergyBudget::Description(S->Metadata->EnergyBudgetDomains,&S->Metadata->UnitMap):TEXT("not supplied"))+
            (S->Metadata&&!S->Metadata->EnergyBudgetDomains.IsEmpty()?TEXT("\nOriginal domain source: ")+S->Metadata->EnergyBudgetDomainSource+TEXT("\nOriginal sampling convention: ")+S->Metadata->EnergyBudgetDomainConvention:TEXT(""));
    }
    if (Key == TEXT("Forces"))
    {
        if(!SelectedBody.IsEmpty()&&!Body)return TEXT("Selected body forces unavailable at this original sample.");
        const auto& F=Body?Body->Forces:S->Forces;
        const TOptional<double> Stress[]={F.Fx,F.Fy,F.Fz,F.My},Momentum[]={F.MomentumFx,F.MomentumFy,F.MomentumFz,F.MomentumMy};
        const TCHAR* Names[]={TEXT("Fx"),TEXT("Fy"),TEXT("Fz"),TEXT("My")};FString Result;
        for(int32 I=0;I<4;++I)
        {
            const auto Q=I==3?EStudioHome4Quantity::Moment:EStudioHome4Quantity::Force;
            Result+=(I?TEXT("\n"):TEXT(""))+FString(Names[I])+TEXT(" stress ")+Value(Stress[I],Q,bNormalizeForces)+TEXT(" · momentum ")+Value(Momentum[I],Q,bNormalizeForces);
        }
        return Result;
    }
    if (Key == TEXT("Window"))
    {
        if(!SelectedBody.IsEmpty()&&!Body)return TEXT("Selected body window unavailable.");const auto& W=Body?Body->Window:S->Window;
        return TEXT("Fx ")+Value(W.Fx,EStudioHome4Quantity::Force)+TEXT(" [")+Value(W.PreviousFx,EStudioHome4Quantity::Force)+TEXT("]\nFy ")+Value(W.Fy,EStudioHome4Quantity::Force)+TEXT(" [")+Value(W.PreviousFy,EStudioHome4Quantity::Force)+
            TEXT("]\nFz ")+Value(W.Fz,EStudioHome4Quantity::Force)+TEXT(" [")+Value(W.PreviousFz,EStudioHome4Quantity::Force)+TEXT("]\nMy ")+Value(W.My,EStudioHome4Quantity::Moment)+TEXT(" [")+Value(W.PreviousMy,EStudioHome4Quantity::Moment)+
            TEXT("]\nInterval ")+Number(W.Start)+TEXT("–")+Number(W.End)+TEXT(" [")+Number(W.PreviousStart)+TEXT("–")+Number(W.PreviousEnd)+TEXT("] · averaging length ")+Number(W.AverageLength,TEXT("body lengths"))+
            TEXT("\nAbscissa convention ")+(W.AbscissaUnit.IsEmpty()?TEXT("unknown"):W.AbscissaUnit)+TEXT(" · epoch ")+(W.Epoch.IsEmpty()?TEXT("unknown"):W.Epoch);
    }
    if(Key==TEXT("Interface"))
    {
        FString Result=TEXT("Thickness histogram: ")+(S->InterfaceThickness.Counts.IsEmpty()?TEXT("Unavailable"):FString::Printf(TEXT("%d original bins"),S->InterfaceThickness.Counts.Num()));
        const auto& H=S->InterfaceThickness;
        Result+=TEXT("\nOriginal sampling φ interval: ")+Number(H.PhiMinimum)+TEXT(" to ")+Number(H.PhiMaximum)+TEXT(" · expected ξ ")+Number(H.ExpectedXi)+TEXT(" ")+(H.ThicknessUnit.IsEmpty()?TEXT("unknown units"):H.ThicknessUnit)+TEXT(" · source ")+(H.SamplingSource.IsEmpty()?TEXT("unknown"):H.SamplingSource);
        if(!H.Counts.IsEmpty())
        {
            int32 Peak=0;for(int32 I=1;I<H.Counts.Num();++I)if(H.Counts[I]>H.Counts[Peak])Peak=I;
            Result+=FString::Printf(TEXT("\nLargest supplied bin %.6g to %.6g; no inferred smooth peak."),H.BinEdges[Peak],H.BinEdges[Peak+1]);
            if(H.ExpectedXi)
            {
                int64 Tail=0,Total=0;for(int32 I=0;I<H.Counts.Num();++I){Total+=H.Counts[I];if(H.BinEdges[I]>=*H.ExpectedXi)Tail+=H.Counts[I];}
                Result+=TEXT(" · complete-bin count above expected ξ ")+LexToString(Tail)+TEXT(" / ")+LexToString(Total);
            }
        }
        Result+=TEXT("\nSpurious speed ")+Value(S->SpuriousSpeed,EStudioHome4Quantity::Velocity)+TEXT(" · sampling mask: ")+(S->SpuriousMask.IsEmpty()?TEXT("Unavailable"):S->SpuriousMask);
        Result+=FString(TEXT("\nOriginal forcing-free/rest condition "))+(S->SpuriousForcingFree?(*S->SpuriousForcingFree?TEXT("forcing-free"):TEXT("forcing present")):TEXT("forcing unknown"))+TEXT(" / ")+(S->SpuriousAtRest?(*S->SpuriousAtRest?TEXT("at rest"):TEXT("not at rest")):TEXT("rest unknown"));
        if(S->SpuriousForcingFree&&*S->SpuriousForcingFree&&S->SpuriousAtRest&&*S->SpuriousAtRest&&S->SpuriousSpeed&&S->SpuriousReferenceSpeed&&S->SpuriousAbsoluteTolerance)
        {
            const double Difference=FMath::Abs(*S->SpuriousSpeed-*S->SpuriousReferenceSpeed);
            Result+=FString(TEXT("\nOriginal supplied spurious-reference comparison "))+(FMath::IsFinite(Difference)&&Difference<=*S->SpuriousAbsoluteTolerance?TEXT("passed"):TEXT("failed"))+TEXT(" · difference ")+Number(Difference)+TEXT(" · tolerance ")+Number(S->SpuriousAbsoluteTolerance)+TEXT(" ")+S->SpuriousUnit+TEXT(" · source ")+S->SpuriousReferenceSource+TEXT(". Recipe gate remains independent.");
        }else Result+=TEXT("\nSpurious-reference comparison not_evaluated: identified reference, original forcing-free/rest condition, units and explicit source tolerance required.");
        Result+=TEXT("\nWater KE ")+Value(S->WaterKE,EStudioHome4Quantity::Energy)+TEXT(" · air KE ")+Value(S->AirKE,EStudioHome4Quantity::Energy)+TEXT(" · surface ")+Value(S->SurfaceEnergy,EStudioHome4Quantity::Energy);
        TArray<FString> Names;S->PhaseEnergies.GetKeys(Names);Names.Sort();for(const auto& Name:Names){const auto& P=S->PhaseEnergies[Name];Result+=TEXT("\n")+Name+TEXT(" · KE ")+Value(P.KE,EStudioHome4Quantity::Energy)+TEXT(" · PE ")+Value(P.PE,EStudioHome4Quantity::Energy)+TEXT(" · surface ")+Value(P.Surface,EStudioHome4Quantity::Energy);}
        return Result;
    }
    if(Key==TEXT("Bodies"))
    {
        if(S->Bodies.IsEmpty())return TEXT("Per-body measurements unavailable.");
        if(!Body)return TEXT("Choose an original body above to inspect its state and fitted coefficients.");
        auto Vector=[&](const TOptional<FVector>& V,EStudioHome4Quantity Q){return V?TEXT("[")+Value(V->X,Q)+TEXT(", ")+Value(V->Y,Q)+TEXT(", ")+Value(V->Z,Q)+TEXT("]"):FString(TEXT("Unavailable"));};
        auto RawVector=[](const TOptional<FVector>& V,const FString& Unit){return V?FString::Printf(TEXT("[%.6g, %.6g, %.6g] %s"),V->X,V->Y,V->Z,*Unit):FString(TEXT("Unavailable"));};
        const auto& B=*Body;FStudioHome4Sample Retab;Retab.Work=B.RetabulationWork;const auto Cost=FStudioHome4Diagnostics::Performance(Retab);const auto Quasi=StudioHome4BodyDiagnostics::QuasiStatic(*S,B);
        return B.Id+(B.Name.IsEmpty()?TEXT(""):TEXT(" · ")+B.Name)+TEXT("\nPosition ")+Vector(B.Position,EStudioHome4Quantity::Length)+TEXT("\nVelocity ")+Vector(B.Velocity,EStudioHome4Quantity::Velocity)+
            TEXT("\nRoll/pitch/yaw ")+RawVector(B.AttitudeDegrees,TEXT("degrees"))+TEXT(" · angular velocity ")+RawVector(B.AngularVelocity,B.AngularVelocityUnit.IsEmpty()?TEXT("raw source units"):B.AngularVelocityUnit)+
            TEXT("\nVirtual-mass integrator: ")+(B.IntegratorStatus.IsEmpty()?TEXT("Unavailable"):B.IntegratorStatus)+TEXT(" · retabulation every ")+Count(B.RetabulationEvery)+TEXT(" steps · measured cost ")+Number(Cost.MLUPSInstant,TEXT("MLUPS"))+TEXT(" / ")+Number(B.RetabulationWork.ElapsedSeconds,TEXT("s"))+
            TEXT("\nHydrostatic k33/k35/k55 ")+Number(B.K33)+TEXT(" / ")+Number(B.K35)+TEXT(" / ")+Number(B.K55)+TEXT(" ")+(B.StiffnessUnit.IsEmpty()?TEXT("raw source units"):B.StiffnessUnit)+
            TEXT("\nHeld heave ")+Value(B.EquilibriumHeave,EStudioHome4Quantity::Length)+TEXT(" · running heave ")+Value(B.RunningHeave,EStudioHome4Quantity::Length)+TEXT(" · reference heave ")+Value(B.ReferenceHeave,EStudioHome4Quantity::Length)+TEXT("\nHeld attitude ")+RawVector(B.EquilibriumAttitudeDegrees,TEXT("degrees"))+TEXT(" · running ")+RawVector(B.RunningAttitudeDegrees,TEXT("degrees"))+TEXT("\nReference attitude ")+RawVector(B.ReferenceAttitudeDegrees,TEXT("degrees"))+TEXT(" · source ")+(B.ReferenceSource.IsEmpty()?TEXT("Unavailable"):B.ReferenceSource)+
            TEXT("\nCalculated quasi-static heave ")+Number(Quasi.HeaveMeters,TEXT("m"))+TEXT(" · pitch ")+Number(Quasi.PitchDegrees,TEXT("degrees"))+TEXT(" · ")+Quasi.Method+TEXT(" · ")+Quasi.Reason+
            TEXT("\nStiffness component units ")+B.K33Unit+TEXT(" / ")+B.K35Unit+TEXT(" / ")+B.K55Unit+TEXT(" · convention ")+B.StiffnessConvention+
            TEXT("\nSource-computed quasi-static heave ")+Value(B.QuasiStaticHeave,EStudioHome4Quantity::Length)+TEXT(" · pitch ")+Number(B.QuasiStaticPitchDegrees,TEXT("degrees"))+TEXT(" · method ")+(B.QuasiStaticMethod.IsEmpty()?TEXT("unknown"):B.QuasiStaticMethod)+TEXT(" · source ")+(B.QuasiStaticSource.IsEmpty()?TEXT("unknown"):B.QuasiStaticSource)+
            TEXT("\nAdded mass ")+Number(B.AddedMass)+TEXT(" [reference ")+Number(B.ReferenceAddedMass)+TEXT("] ")+(B.AddedMassUnit.IsEmpty()?TEXT("raw source units"):B.AddedMassUnit)+
            TEXT("\nDamping ")+Number(B.Damping)+TEXT(" [reference ")+Number(B.ReferenceDamping)+TEXT("] ")+(B.DampingUnit.IsEmpty()?TEXT("raw source units"):B.DampingUnit)+TEXT(" · source ")+(B.FitReferenceSource.IsEmpty()?TEXT("Unavailable"):B.FitReferenceSource)+
            TEXT("\nOriginal fit method ")+(B.FitMethod.IsEmpty()?TEXT("unknown"):B.FitMethod)+TEXT(" · frequency ")+Number(B.FitFrequency)+TEXT(" ")+(B.FitFrequencyUnit.IsEmpty()?TEXT("unknown units"):B.FitFrequencyUnit)+TEXT(" · window ")+Number(B.FitWindowStart)+TEXT(" to ")+Number(B.FitWindowEnd)+TEXT(" ")+B.FitWindowUnit+TEXT(" · epoch ")+(B.FitEpoch.IsEmpty()?TEXT("unknown"):B.FitEpoch)+TEXT("\nReference comparison not evaluated; explicit Validation source/metric/tolerance controls evaluate original BEM/tank evidence.");
    }
    if (Key == TEXT("Performance"))
    {
        const auto P = FStudioHome4Diagnostics::Performance(*S);TOptional<double> PeakRatio;
        if(P.GigabytesPerSecond&&S->Metadata&&S->Metadata->DevicePeakGBps){const double R=*P.GigabytesPerSecond/ *S->Metadata->DevicePeakGBps;if(FMath::IsFinite(R))PeakRatio=R;}
        return TEXT("Measured instant/cumulative MLUPS: ") + Number(P.MLUPSInstant) + TEXT(" / ") + Number(P.MLUPSCumulative) + TEXT("\nAchieved bandwidth: ") + Number(P.GigabytesPerSecond, TEXT("GB/s")) +
            TEXT("\nDriver reported instant/cumulative MLUPS: ") + Number(S->ReportedMLUPSInstant) + TEXT(" / ") + Number(S->ReportedMLUPSCumulative) +
            TEXT("\nElapsed window ") + Number(S->Work.ElapsedSeconds, TEXT("s")) + TEXT(" · node updates ") + Number(S->Work.NodeUpdates) + TEXT(" · transfer bytes ") + Number(S->Work.TransferredBytes)+
            TEXT("\nOriginal device peak ")+Number(S->Metadata?S->Metadata->DevicePeakGBps:TOptional<double>(),TEXT("GB/s"))+TEXT(" · achieved/peak ")+Number(PeakRatio)+TEXT(" · source ")+(S->Metadata&&!S->Metadata->DevicePeakSource.IsEmpty()?S->Metadata->DevicePeakSource:TEXT("unknown"));
    }
    if (Key == TEXT("Safeguards"))
    {
        auto Trend = [&](bool Limiter)
        {
            const auto* Stream = DisplayStream(); if (!Stream || Stream->History().Num() < 2) return FString(TEXT("trend unknown"));
            const auto& Previous = Stream->History()[Stream->History().Num() - 2];
            const auto A = Limiter ? Previous.LimiterCells : Previous.ThresholdCells, B = Limiter ? S->LimiterCells : S->ThresholdCells;
            if (!A || !B || !Previous.Step || !S->Step || *S->Step <= *Previous.Step) return FString(TEXT("trend unknown"));
            return *B > *A ? FString(TEXT("rising · inspect active cells and limiter/threshold settings")) : *B == *A ? FString(TEXT("flat")) : FString(TEXT("decreasing"));
        };
        return TEXT("Limiter/threshold cells: ") + Count(S->LimiterCells) + TEXT(" / ") + Count(S->ThresholdCells) + TEXT("\nLimiter ") + Trend(true) + TEXT(" · threshold ") + Trend(false) +
            TEXT("\nMax speed ") + Value(S->MaximumSpeed, EStudioHome4Quantity::Velocity) + TEXT(" at ") + Cell(S->MaximumSpeedCell) + TEXT(" · Mach ") + Number(S->Mach) +
            TEXT(" · minimum τ ") + Number(S->TauMinimum) + TEXT(" · τ−½ margin ") + Number(S->TauMinimum ? TOptional<double>(*S->TauMinimum - .5) : TOptional<double>()) +
            TEXT("\nDivergence norm ") + Number(S->DivergenceNorm) + TEXT(" · acoustic scale Ma² ") + Number(S->Mach ? TOptional<double>(*S->Mach * *S->Mach) : TOptional<double>()) +
            TEXT("\nNorm convention ")+(S->Metadata&&!S->Metadata->DivergenceConvention.IsEmpty()?S->Metadata->DivergenceConvention:TEXT("unknown"))+TEXT(" · units ")+(S->Metadata&&!S->Metadata->DivergenceUnit.IsEmpty()?S->Metadata->DivergenceUnit:TEXT("unknown"))+TEXT(" · domain ")+(S->Metadata&&!S->Metadata->DivergenceDomain.IsEmpty()?S->Metadata->DivergenceDomain:TEXT("unknown"))+
            TEXT(" (norm units must be compatible before comparing; bulk relaxation s_bulk controls acoustic ringing)") + TEXT(" · trouble trigger ") + Number(Policy.MaximumSpeedTrigger) + TEXT(" (original speed units)");
    }
    if (Key == TEXT("Trouble"))
    {
        const auto* Stream = DisplayStream();
        const auto& F = !Stream->ActionRequests().IsEmpty() ? Stream->ActionRequests().Last().Facts : S->Trouble;
        FString Text = TEXT("Cell ") + Cell(F.Cell ? F.Cell : S->MaximumSpeedCell) + TEXT(" · level ") + (F.Level ? FString::FromInt(*F.Level) : TEXT("Unavailable")) + TEXT(" · original patch ") + (F.PatchId.IsEmpty() ? TEXT("Unavailable") : F.PatchId) + TEXT(" · φ ") + Number(F.Phi) + TEXT(" · τ ") + Number(F.Tau) +
            TEXT("\nLimiter ") + Fact(F.Limiter) + TEXT(" · force threshold ") + Fact(F.ForceThreshold) + TEXT("\nBand ") + Fact(F.InBand) + TEXT(" · sponge ") + Fact(F.InSponge) + TEXT(" · beach ") + Fact(F.InBeach) +
            TEXT(" · cut-link shell ") + Fact(F.InCutLinkShell) + TEXT("\nZone: ") + (F.Zone.IsEmpty() ? TEXT("Unavailable") : F.Zone);
        if (!Stream->ActionRequests().IsEmpty())
        {
            const auto& A = Stream->ActionRequests().Last();
            Text += TEXT("\n") + A.Reason + TEXT(" Last good step: ") + Count(A.LastGoodStep) + TEXT(". Reported restart: ") + A.RestartPath.Get(TEXT("Unavailable"));
        }
        if (Runtime && !IsImportedReplay() && !Runtime->GuardOutcomes().IsEmpty())
        { const auto& O = Runtime->GuardOutcomes().Last(); Text += TEXT("\nLive guard outcome: ") + O.Recovery + TEXT("\n") + O.Stop + TEXT("\n") + O.Locate; }
        return Text;
    }
    return TEXT("Unavailable");
}
StudioHome4SciencePresentation::FHistory SStudioHome4Monitors::PresentedHistory() const
{return StudioHome4SciencePresentation::History(DisplayStream(),SelectedMetric,UnitDisplay.Get(),PlotComponent,bNormalizeForces,SelectedBody,SelectedLevel,SelectedPhase);}
StudioHome4SciencePresentation::FHistory SStudioHome4Monitors::PresentedForces() const
{return StudioHome4SciencePresentation::History(DisplayStream(),StudioHome4SciencePresentation::EMetric::Forces,UnitDisplay.Get(),PlotComponent,bNormalizeForces,SelectedBody);}
FString SStudioHome4Monitors::HistoryCaption(bool Forces) const
{
    using namespace StudioHome4SciencePresentation;const auto H=Forces?PresentedForces():PresentedHistory();
    FString Text=H.Axis+TEXT(" · ")+H.Unit;
    if(Forces)
    {
        const TCHAR* Components[]={TEXT("Fx"),TEXT("Fy"),TEXT("Fz"),TEXT("My")};
        Text=FString(Components[PlotComponent])+TEXT(" · ")+Text;
    }
    for(const auto& Series:H.Series)Text+=TEXT(" · ")+Series.Label+TEXT(": ")+StudioHome4MonitorPrivate::Number(Series.Values.IsEmpty()?TOptional<double>():Series.Values.Last());
    return Text+(H.Note.IsEmpty()?TEXT(""):TEXT("\n")+H.Note);
}
void SStudioHome4Monitors::CycleBody()
{
    TArray<FString> Ids;Ids.Add(FString());
    if(const auto* Stream=DisplayStream())
    {
        for(const auto& S:Stream->History())for(const auto& B:S.Bodies)Ids.AddUnique(B.Id);
    }
    SelectedBody=Ids[(Ids.Find(SelectedBody)+1)%Ids.Num()];Policy.BodyId=SelectedBody;bNormalizeForces=false;
}
void SStudioHome4Monitors::CycleLevel()
{
    TArray<int32> Ids{0};
    if(const auto* Stream=DisplayStream())
    {
        for(const auto& S:Stream->History())
        {for(const auto& L:S.Levels)Ids.AddUnique(L.Level);for(int32 I=0;I<S.Mass.LevelDrifts.Num();++I)Ids.AddUnique(I);}
    }
    Ids.Sort();
    SelectedLevel=Ids[(Ids.Find(SelectedLevel)+1)%Ids.Num()];
}
void SStudioHome4Monitors::CyclePhase()
{
    TArray<FString> Ids;Ids.Add(FString());
    if(const auto* Stream=DisplayStream())
    {
        for(const auto& S:Stream->History())
        {for(const auto& P:S.PhaseEnergies)Ids.AddUnique(P.Key);for(const auto& P:S.PhaseBudgets)Ids.AddUnique(P.Key);}
    }
    SelectedPhase=Ids[(Ids.Find(SelectedPhase)+1)%Ids.Num()];
}
void SStudioHome4Monitors::ApplyPolicy()
{
    using namespace StudioHome4MonitorPrivate;
    FStudioHome4DiagnosticPolicy Candidate = Policy;
    TOptional<double>* Fields[] = {&Candidate.BudgetAbsoluteTolerance, &Candidate.ForceRelativeTolerance, &Candidate.ForceAbsoluteTolerance,
        &Candidate.ForceReferenceMagnitude, &Candidate.WindowRelativeTolerance, &Candidate.WindowAbsoluteTolerance, &Candidate.WindowReferenceMagnitude, &Candidate.RestPressureTolerance, &Candidate.MaximumSpeedTrigger};
    for (int32 I = 0; I < UE_ARRAY_COUNT(Fields); ++I)
        if (!PolicyNumber(PolicyDraft[I], *Fields[I]))
        { Status = FString::Printf(TEXT("Threshold field %d must be a finite nonnegative number or empty. Previous limits retained."), I + 1); return; }
    Policy = Candidate;
    if (SessionStream) SessionStream->SetDiagnosticPolicy(Policy);
    if (Runtime) Runtime->SetDiagnosticPolicy(Policy);
    if (ImportedStream) ImportedStream->SetDiagnosticPolicy(Policy);
    Status = TEXT("Explicit health thresholds applied. Empty limits keep their gate unavailable.");
}
bool SStudioHome4Monitors::CanLocate() const
{
    const auto* S = Sample(); if (!S || !OnLocate.IsBound() || !OriginalRunIdentity()) return false;
    const auto* Stream = DisplayStream();
    if (!Stream->ActionRequests().IsEmpty()) return Stream->ActionRequests().Last().Facts.Cell.IsSet();
    return S->Trouble.Cell.IsSet() || S->MaximumSpeedCell.IsSet();
}
void SStudioHome4Monitors::Locate()
{
    if (!CanLocate()) return;
    const auto* Stream = DisplayStream();
    FStudioHome4CellFacts Facts = Stream->ActionRequests().IsEmpty() ? Sample()->Trouble : Stream->ActionRequests().Last().Facts;
    if (!Facts.Cell) Facts.Cell = Sample()->MaximumSpeedCell;
    OnLocate.ExecuteIfBound(Facts);
}
void SStudioHome4Monitors::RefreshOutputs()
{
    using namespace StudioHome4MonitorPrivate;
    const auto* Stream = DisplayStream();
    const uint64 Last = Stream && !Stream->OutputEvents().IsEmpty() ? Stream->OutputEvents().Last().RecordIndex : 0;
    if (DisplayedStream == Stream && DisplayedOutputIndex == Last) return;
    DisplayedStream = Stream; DisplayedOutputIndex = Last; OutputRows->ClearChildren();
    for (int32 I = 0; I < 4; ++I)
    {
        const auto Kind = EStudioHome4OutputKind(I);
        FString Latest = TEXT("Unavailable");
        if (Stream) for (const auto& E : Stream->OutputEvents()) if (E.Kind == Kind) Latest = TEXT("step ") + Count(E.Step) + TEXT(" · ") + E.Path;
        OutputRows->AddSlot().AutoHeight().Padding(0, 3)[SNew(STextBlock).Tag(FName(*FString::Printf(TEXT("Home4OutputKind%d"), I)))
            .Font(StudioUI::Font(9)).ColorAndOpacity(Kind == EStudioHome4OutputKind::Restart ? StudioUI::Amber : StudioUI::Text).AutoWrapText(true)
            .Text(FText::FromString(FString(KindName(Kind)) + TEXT(" · ") + Latest))];
    }
    if (Stream)
    {
        const auto& Events = Stream->OutputEvents();
        for (int32 I = FMath::Max(0, Events.Num() - 12); I < Events.Num(); ++I)
        {
            const auto& E = Events[I];
            OutputRows->AddSlot().AutoHeight().Padding(0, 2)[StudioUI::Label(FString::Printf(TEXT("%s · step %s · %s"), KindName(E.Kind), *Count(E.Step), *E.Path), 8, StudioUI::Muted)];
        }
    }
}
void SStudioHome4Monitors::ImportDialog()
{
    const FString Identity = OriginalRunIdDraft.TrimStartAndEnd(); FGuid Original;
    if (!Identity.IsEmpty() && (!FGuid::Parse(Identity, Original) || !Original.IsValid()))
    { Status = TEXT("Original run ID must be a valid GUID or empty. Previous science source retained."); return; }
    FString Path;
    if (!StudioFileDialog::DataFile(false, TEXT("Import original HOME4 science JSONL log"), TEXT(""), TEXT("jsonl"), Path))
    { Status = TEXT("Import cancelled. Previous science source retained."); return; }
    BeginImportPath(Path, OriginalRunIdDraft);
}
bool SStudioHome4Monitors::BeginImportPath(const FString& Path, const FString& OriginalRunId)
{
    ScopeProject();
    if (Pending.IsValid()) { Status = TEXT("A science log is already being read."); return false; }
    if (Path.IsEmpty()) { Status = TEXT("Import cancelled. Previous science source retained."); return false; }
    const FString Identity = OriginalRunId.TrimStartAndEnd(); FGuid Run = FGuid::NewGuid();
    if (!Identity.IsEmpty() && (!FGuid::Parse(Identity, Run) || !Run.IsValid()))
    { Status = TEXT("Original run ID must be a valid GUID or empty. Previous science source retained."); return false; }
    TOptional<int64> Start,End;
    auto Bound=[](const FString& Draft,TOptional<int64>& Value)
    {
        const auto Trimmed=Draft.TrimStartAndEnd();if(Trimmed.IsEmpty())return true;
        if(Trimmed.Len()>13)return false;int64 N=0;for(TCHAR C:Trimmed){if(C<'0'||C>'9'||N>(1000000000000LL-(C-'0'))/10)return false;N=N*10+(C-'0');}Value=N;return true;
    };
    if(!Bound(ReplayStartDraft,Start)||!Bound(ReplayEndDraft,End)||Start.IsSet()!=End.IsSet()||(Start&&*End<*Start))
    {Status=TEXT("Replay selection needs two original nonnegative integer step bounds in increasing order, or both empty. Previous replay retained.");return false;}
    const FString FullPath=FPaths::ConvertRelativePathToFull(Path);
    const FString ExpectedSHA=bLoadingReplayInterval&&FullPath==ImportPath?ImportSHA256:FString();
    if(bLoadingReplayInterval&&Identity.IsEmpty()&&FullPath==ImportPath&&ImportedStream&&ImportedStream->Latest())Run=ImportedStream->Latest()->Source.RunId;
    const FStudioHome4Source Source{Run, FullPath};
    const bool bOriginalIdentity = !Identity.IsEmpty();
    const auto M = Model.Pin(); ImportProjectId = M ? M->Project.Id : FGuid(); ImportCaseId = M ? M->Project.Draft.Id : FGuid();
    Cancellation = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    Status = TEXT("Reading original JSONL in the background. Previous science source remains visible.");
    TFunction<void()> BeforeVerify;
#if WITH_DEV_AUTOMATION_TESTS
    BeforeVerify = BeforeImportVerify;
#endif
    Pending = Async(EAsyncExecution::ThreadPool, [Path, Source, bOriginalIdentity, Cancel = Cancellation, BeforeVerify,Start,End,ExpectedSHA]
        { return ReadImport(Path, Source, bOriginalIdentity, Cancel, BeforeVerify,Start,End,ExpectedSHA); });
    return true;
}
bool SStudioHome4Monitors::LoadReplayInterval()
{
    ScopeProject();if(!ImportedStream||ImportPath.IsEmpty()){Status=TEXT("Import a completed original replay before selecting its interval.");return false;}
    TGuardValue<bool> Selecting(bLoadingReplayInterval,true);
    const auto Identity=bImportedOriginalRunIdentity&&ImportedStream->Latest()?TOptional<FGuid>(ImportedStream->Latest()->Source.RunId):TOptional<FGuid>();
    return BeginImportPath(ImportPath,Identity?Identity->ToString():FString());
}
void SStudioHome4Monitors::CancelImport()
{ if (Cancellation) Cancellation->store(true, std::memory_order_relaxed); }
void SStudioHome4Monitors::PollImport()
{
    ScopeProject();
    if (!Pending.IsValid() || !Pending.IsReady()) return;
    const bool bCancelled = Cancellation && Cancellation->load(std::memory_order_relaxed);
    auto Result = MoveTemp(Pending.GetMutable()); Pending = {}; Cancellation.Reset();
    if (ScopedProjectId != ImportProjectId || ScopedCaseId != ImportCaseId)
    { Status = TEXT("Project or case changed while importing; the previous replay was cleared and the new replay was not attached."); return; }
    if (bCancelled) { Status = TEXT("Science import cancelled. Previous science source retained."); return; }
    if (!Result.Error.IsEmpty()) { Status = Result.Error + TEXT(" Previous science source retained."); return; }
    ImportedStream = MakeShared<FStudioHome4TelemetryStream>(MoveTemp(*Result.Stream)); ImportedStream->SetDiagnosticPolicy(Policy);
    bImportedOriginalRunIdentity = Result.bOriginalRunIdentity;
    ImportPath = Result.Path; ImportSHA256 = Result.SHA256; bShowImported = true;
    ImportedOriginalBytes=MoveTemp(Result.OriginalBytes);
    ReplayStart=Result.SelectedStepStart;ReplayEnd=Result.SelectedStepEnd;
    Status = FString::Printf(TEXT("Imported replay · %lld original bytes, %lld lines; %lld unknown records skipped. %d measurements retained."), Result.Bytes, Result.Lines, Result.Unknown, ImportedStream->History().Num());
    Status+=ReplayStart?FString::Printf(TEXT(" Exact original step interval %lld to %lld; every selected measurement retained."),*ReplayStart,*ReplayEnd):TEXT(" Latest preview only; full verified original log retained for replay/report.");
    RefreshOutputs();
}
void SStudioHome4Monitors::ScopeProject()
{
    const auto M = Model.Pin();
    if(M ? M->Project.Id == ScopedProjectId && M->Project.Draft.Id == ScopedCaseId : !ScopedProjectId.IsValid() && !ScopedCaseId.IsValid())return;
    ScopedProjectId=M?M->Project.Id:FGuid();ScopedCaseId=M?M->Project.Draft.Id:FGuid();
    SessionStream.Reset(); ImportedStream.Reset(); ImportedOriginalBytes.Reset(); ImportPath.Empty(); ImportSHA256.Empty(); bImportedOriginalRunIdentity = false;
    OriginalRunIdDraft.Empty(); bShowImported = false; SelectedBody.Empty(); SelectedPhase.Empty(); SelectedLevel=0; bNormalizeForces=false; CancelImport();
    ReplayStart.Reset();ReplayEnd.Reset();ReplayStartDraft.Empty();ReplayEndDraft.Empty();
    Status = TEXT("Project or case changed; imported replay cleared. Supply explicitly identified original science for this scope.");
    RefreshOutputs();
}
SStudioHome4Monitors::FImportResult SStudioHome4Monitors::ReadImport(const FString& Path,
    const FStudioHome4Source& Source, bool bOriginalRunIdentity, const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& Cancel, const TFunction<void()>& BeforeVerify,
    TOptional<int64> Start,TOptional<int64> End,const FString& ExpectedSHA)
{
    FImportResult R;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> FirstHash(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> VerifyHash(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!FirstHash || !VerifyHash || EVP_DigestInit_ex(FirstHash.get(), EVP_sha256(), nullptr) != 1 || EVP_DigestInit_ex(VerifyHash.get(), EVP_sha256(), nullptr) != 1)
    { R.Error = TEXT("Science source identity could not be initialized."); return R; }
    auto Cancelled = [&] { return Cancel->load(std::memory_order_relaxed); };
    auto Fail = [&](const TCHAR* Error) { R.Error = Error; R.Stream.Reset(); return MoveTemp(R); };
    if (Cancelled()) return Fail(TEXT("Science import cancelled."));
    FStudioFileAccess Access(Path);
    const auto Before = IFileManager::Get().GetTimeStamp(*Path);
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!File) return Fail(TEXT("Original science log is missing or inaccessible."));
    const int64 Size = File->TotalSize();
    if (Size <= 0 || Size > 64LL * 1024 * 1024) return Fail(TEXT("Science log must contain between 1 byte and 64 MiB."));
    FStudioHome4TailLimits Limits;Limits.SelectedStepStart=Start;Limits.SelectedStepEnd=End;if(Start)Limits.MaxHistory=4096;
    R.Stream = MakeUnique<FStudioHome4TelemetryStream>(Limits);
    if(!R.Stream->BeginRun(Source))return Fail(TEXT("Original replay source identity or interval is invalid."));
    R.bOriginalRunIdentity = bOriginalRunIdentity;R.SelectedStepStart=Start;R.SelectedStepEnd=End;
    TArray<uint8> Chunk; Chunk.SetNumUninitialized(65536);
    uint8 LastByte = '\n';
    auto Consume = [&](const uint8* Bytes, int32 N)
    {
        int32 Offset = 0;
        while (Offset < N)
        {
            if (Cancelled()) return false;
            const auto Batch = R.Stream->AppendBytes(Bytes + Offset, N - Offset);
            if (Batch.ConsumedBytes <= 0) { R.Error = TEXT("Science parser could not advance."); return false; }
            Offset += Batch.ConsumedBytes; R.Lines += Batch.CompleteLines; R.Malformed += Batch.Malformed;
            R.Unknown += Batch.Unknown; R.Oversized += Batch.Oversized; R.Regressing += Batch.Regressing;
            if(Start&&R.Stream->SelectedMeasurementCount()>4096){R.Error=TEXT("Selected replay interval exceeds 4096 measurements; choose a narrower original interval. Full log and prior replay retained.");return false;}
            if (R.Malformed || R.Oversized || R.Regressing) { R.Error = FString::Printf(TEXT("Science import rejected invalid or regressing data near original line %lld."), R.Lines); return false; }
            if (R.Lines > 1000000) { R.Error = TEXT("Science log exceeds the one-million-line import budget."); return false; }
        }
        return true;
    };
    while (R.Bytes < Size)
    {
        if (Cancelled()) return Fail(TEXT("Science import cancelled."));
        const int32 N = int32(FMath::Min<int64>(Chunk.Num(), Size - R.Bytes));
        File->Serialize(Chunk.GetData(), N);
        if (File->IsError()) return Fail(TEXT("Original science log could not be read completely."));
        if (EVP_DigestUpdate(FirstHash.get(), Chunk.GetData(), N) != 1) return Fail(TEXT("Science source identity could not be measured."));
        R.OriginalBytes.Append(Chunk.GetData(),N);
        R.Bytes += N; LastByte = Chunk[N - 1];
        if (!Consume(Chunk.GetData(), N))
        { if (Cancelled()) return Fail(TEXT("Science import cancelled.")); R.Stream.Reset(); return R; }
    }
    // A completed import may end after a whole JSON value without a final LF.
    // Tailing keeps this fragment pending; import explicitly closes it at EOF.
    if (LastByte != '\n')
    {
        const uint8 LF = '\n';
        if (!Consume(&LF, 1)) { R.Stream.Reset(); return R; }
    }
    if (Cancelled()) return Fail(TEXT("Science import cancelled."));
    uint8 OriginalDigest[32], VerifiedDigest[32]; unsigned int OriginalLength=0, VerifiedLength=0;
    if (EVP_DigestFinal_ex(FirstHash.get(), OriginalDigest, &OriginalLength) != 1 || OriginalLength != 32)
        return Fail(TEXT("Science source identity could not be completed."));
    // Finish and release the first named-file reader before the independent
    // verification pass. Editor readers hold a shared file lock on macOS; keeping
    // it open prevents ordinary writers from exercising the rewrite check.
    if (File->TotalSize() != Size || !File->Close() || File->IsError())
        return Fail(TEXT("Original science log changed or could not finish its first read."));
    File.Reset();
    if (BeforeVerify) BeforeVerify();
    TUniquePtr<FArchive> Verify(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!Verify || Verify->TotalSize() != Size) return Fail(TEXT("Original science log changed during import. Select a completed log."));
    int64 VerifiedBytes=0;
    while (VerifiedBytes < Size)
    {
        if (Cancelled()) return Fail(TEXT("Science import cancelled."));
        const int32 N=int32(FMath::Min<int64>(Chunk.Num(), Size-VerifiedBytes));
        Verify->Serialize(Chunk.GetData(), N);
        if (Verify->IsError() || EVP_DigestUpdate(VerifyHash.get(), Chunk.GetData(), N) != 1)
            return Fail(TEXT("Original science log could not be verified completely."));
        VerifiedBytes+=N;
    }
    if (EVP_DigestFinal_ex(VerifyHash.get(), VerifiedDigest, &VerifiedLength) != 1 || VerifiedLength != 32)
        return Fail(TEXT("Science source verification could not be completed."));
    if (FMemory::Memcmp(OriginalDigest, VerifiedDigest, 32) != 0 || Verify->TotalSize() != Size || IFileManager::Get().GetTimeStamp(*Path) != Before || IFileManager::Get().FileSize(*Path) != Size)
        return Fail(TEXT("Original science log changed during import. Select a completed log."));
    R.SHA256=BytesToHex(OriginalDigest, 32).ToLower();
    if(!ExpectedSHA.IsEmpty()&&R.SHA256!=ExpectedSHA)return Fail(TEXT("Original replay bytes differ from the imported source; previous interval retained. Choose the changed source separately."));
    if (R.Stream->History().IsEmpty() && R.Stream->OutputEvents().IsEmpty()) return Fail(TEXT("No recognized HOME4 science measurements or outputs were found."));
    R.Path = FPaths::ConvertRelativePathToFull(Path); return R;
}
