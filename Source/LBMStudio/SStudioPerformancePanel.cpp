#include "SStudioPerformancePanel.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Rendering/DrawElements.h"
#include "InputCoreTypes.h"

namespace
{
    FString Number(const TOptional<double>& V,const TCHAR* Unit,int32 Digits=1)
    {return V?FString::Printf(TEXT("%.*f %s"),Digits,*V,Unit):TEXT("Unavailable");}
    FString Bytes(const TOptional<uint64>& V)
    {return V?FString::Printf(TEXT("%.1f MiB"),double(*V)/1048576.):TEXT("Unavailable");}
    FString Counter(const TOptional<int64>& V)
    {return V?FString::Printf(TEXT("%lld"),*V):TEXT("Unavailable");}
    TSharedRef<SWidget> Reading(const TCHAR* Caption,FName Tag,TFunction<FString()> Value,const TCHAR* Help=TEXT(""))
    {
        using namespace StudioUI;
        return SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,3)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted)
                .Text(FText::FromString(Caption)).ToolTipText(FText::FromString(Help))]
            +SHorizontalBox::Slot().AutoWidth().Padding(4,3)[SNew(STextBlock).Tag(Tag).Font(Font(9)).ColorAndOpacity(Text)
                .Text_Lambda([Value]{return FText::FromString(Value());}).ToolTipText(FText::FromString(Help))];
    }
    class SPerformanceChart final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SPerformanceChart):_Memory(false){} SLATE_ARGUMENT(TFunction<const TArray<FStudioPerformanceSample>&()>,Samples)
            SLATE_ARGUMENT(bool,Memory) SLATE_END_ARGS()
        void Construct(const FArguments& A){Read=A._Samples;bMemory=A._Memory;}
        FVector2D ComputeDesiredSize(float) const override{return FVector2D(250,80);}
        int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool) const override
        {
            using namespace StudioUI;
            const auto& H=Read();const auto Size=G.GetLocalSize();
            auto TextAt=[&](const FString& S,FVector2D At,FLinearColor Color)
            {FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(FVector2D(1,1),FSlateLayoutTransform(At)),S,Font(8),ESlateDrawEffect::None,Color);};
            if(H.Num()<2){TextAt(TEXT("Collecting readings…"),FVector2D(0,22),Muted);return Layer+2;}
            const double End=H.Last().At,Start=FMath::Max(H[0].At,End-120.);
            double Low=TNumericLimits<double>::Max(),High=0;
            auto Value=[&](const FStudioPerformanceSample& S)->TOptional<double>
            {return bMemory?(S.Counters.FootprintBytes?TOptional<double>(double(*S.Counters.FootprintBytes)/1048576.):TOptional<double>()):TOptional<double>(S.UICadenceMs);};
            for(const auto& S:H)if(S.At>=Start)if(const auto V=Value(S)){Low=FMath::Min(Low,*V);High=FMath::Max(High,*V);}
            if(Low==TNumericLimits<double>::Max()){TextAt(TEXT("Measurement unavailable"),FVector2D(0,22),Muted);return Layer+2;}
            const double Pad=FMath::Max(1.,(High-Low)*.1);Low=bMemory?FMath::Max(0.,Low-Pad):0.;High+=Pad;
            const double Left=43,Right=Size.X-3,Top=4,Bottom=Size.Y-19;
            for(int32 I=0;I<2;++I)
            {
                const double Y=I?Bottom:Top;TArray<FVector2D> P{{Left,Y},{Right,Y}};
                FSlateDrawElement::MakeLines(Out,Layer,G.ToPaintGeometry(),P,ESlateDrawEffect::None,Muted.CopyWithNewOpacity(.2),true,1);
                TextAt(FString::Printf(TEXT("%.1f"),I?Low:High),FVector2D(0,FMath::Max(0.,Y-6)),Muted);
            }
            TextAt(FString::Printf(TEXT("−%.0f s"),End-Start),FVector2D(Left,Bottom+3),Muted);
            TextAt(TEXT("Latest"),FVector2D(FMath::Max(Left,Right-34.),Bottom+3),Muted);
            TArray<FVector2D> Points;
            auto Flush=[&]{if(Points.Num()>1)FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Cyan,true,1.5);Points.Reset();};
            double Previous=-1;
            for(const auto& S:H)
            {
                if(S.At<Start)continue;const auto V=Value(S);
                if(!V){Flush();Previous=-1;continue;}
                if(Previous>=0&&S.At-Previous>2.5)Flush();Previous=S.At;
                Points.Add(FVector2D(Left+(Right-Left)*(S.At-Start)/FMath::Max(.001,End-Start),Bottom-(Bottom-Top)*(*V-Low)/(High-Low)));
            }
            Flush();return Layer+2;
        }
    private:
        TFunction<const TArray<FStudioPerformanceSample>&()> Read;bool bMemory=false;
    };
}

// THESIS: Inspect application and solver measurements beside the visible flow.
// OWN-WORLD: Existing dense blue-black Solve inspector, aligned values and cyan traces.
// STORY: Open once, read named sources and units, pause readings, return to settings.
// FIRST VIEWPORT: Application memory/timings lead; solver availability is explicit below.
// FORM: Local Operate inspector. Sidebar remains the only workspace navigation.
// FINISH: Two native sizes, scoped fresh finish review and documentation.
void SStudioPerformancePanel::Construct(const FArguments& A)
{
    using namespace StudioUI;
    Model=A._Model;History=A._History;Close=A._OnClose;
    auto Rows=SNew(SVerticalBox);
    auto Heading=[&](const TCHAR* Title){Rows->AddSlot().AutoHeight().Padding(0,12,0,5)[Label(Title,11,Text,true)];};
    auto AppRow=[&](const TCHAR* Caption,FName Key,const TCHAR* Help=TEXT(""))
    {Rows->AddSlot().AutoHeight()[Reading(Caption,FName(*(TEXT("Performance_")+Key.ToString())),[this,Key]{return AppValue(Key);},Help)];};
    auto JobRow=[&](const TCHAR* Caption,FName Key)
    {Rows->AddSlot().AutoHeight()[Reading(Caption,FName(*(TEXT("PerformanceJob_")+Key.ToString())),[this,Key]{return JobValue(Key);})];};
    Heading(TEXT("Application"));
    Rows->AddSlot().AutoHeight().Padding(0,0,0,4)[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this]{return FText::FromString(AppValue(TEXT("Host")));})];
    AppRow(TEXT("Process footprint"),TEXT("Footprint"),TEXT("macOS phys_footprint for LBM Studio, including compressed memory. MiB = 1,048,576 bytes."));
    AppRow(TEXT("CPU · 100% per core"),TEXT("CPU"),TEXT("Measured user + system CPU time for this process. One fully used CPU core = 100%; multithreaded work can exceed 100%."));
    AppRow(TEXT("UI cadence"),TEXT("Cadence"),TEXT("Mean elapsed interval between root workspace ticks. This is not GPU frame time."));
    AppRow(TEXT("UI update work"),TEXT("Update"),TEXT("Mean CPU duration of the workspace update method, excluding this measurement read. Slate paint and GPU rendering are not included."));
    Rows->AddSlot().AutoHeight().Padding(0,7,0,0)[Label(TEXT("UI cadence · ms / tick"),8,Muted)];
    Rows->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(76)[SNew(SPerformanceChart).Samples([this]() -> const auto& {return Samples();})]];
    Rows->AddSlot().AutoHeight().Padding(0,7,0,0)[Label(TEXT("Process footprint · MiB"),8,Muted)];
    Rows->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(76)[SNew(SPerformanceChart).Memory(true).Samples([this]() -> const auto& {return Samples();})]];
    Heading(TEXT("Flow rendering"));
    AppRow(TEXT("Scene captures"),TEXT("Captures"),TEXT("On-demand scene captures per second. A stationary paused scene can correctly report zero."));
    AppRow(TEXT("CPU capture submit · last"),TEXT("CaptureTime"),TEXT("CPU wall time spent submitting the last scene capture. This is not GPU execution time."));
    AppRow(TEXT("Field build · last"),TEXT("BuildTime"),TEXT("CPU wall time of the background field geometry build used in the presented flow frame."));
    AppRow(TEXT("Active geometry workers"),TEXT("Workers"));
    AppRow(TEXT("CPU mesh buffers"),TEXT("MeshBytes"),TEXT("Allocated procedural vertex/index buffers; not total process memory."));
    AppRow(TEXT("Texture allocations"),TEXT("TextureBytes"),TEXT("Engine-reported flow render-target and scalar-texture allocations; not total device memory or residency."));
    Rows->AddSlot().AutoHeight().Padding(0,4)[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this]{return FText::FromString(AppValue(TEXT("BuildSource")));})];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this]{return FText::FromString(AppValue(TEXT("Device"))+TEXT("\nGPU time / utilization: unavailable"));})];
    Heading(TEXT("Solver job"));
    Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[SNew(STextBlock).Tag(TEXT("PerformanceJobSource")).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this]{return FText::FromString(JobValue(TEXT("Source")));})];
    JobRow(TEXT("Measurement state"),TEXT("Status"));JobRow(TEXT("Solver steps"),TEXT("Steps"));
    JobRow(TEXT("Physical time"),TEXT("Physical"));JobRow(TEXT("Solver wall time"),TEXT("Wall"));
    JobRow(TEXT("Stop-limit progress"),TEXT("Progress"));JobRow(TEXT("Steps / second"),TEXT("Rate"));
    JobRow(TEXT("Estimated remaining"),TEXT("ETA"));JobRow(TEXT("Host resident memory"),TEXT("HostMemory"));
    JobRow(TEXT("Device memory used"),TEXT("DeviceMemory"));JobRow(TEXT("Device utilization"),TEXT("DeviceUsage"));
    Rows->AddSlot().AutoHeight().Padding(0,5,0,12)[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this]{return FText::FromString(JobValue(TEXT("Attribution")));})];
    ChildSlot[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Performance"),15,Text,true)]
                +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("PerformanceClose")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,5))
                    .OnClicked_Lambda([this]{Close.ExecuteIfBound();return FReply::Handled();})[Label(TEXT("Close"),9)]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Tag(TEXT("PerformanceReadingState")).Font(Font(8)).ColorAndOpacity(Muted)
                    .Text_Lambda([this]{return FText::FromString(bPaused?TEXT("Paused · ")+FrozenAt:TEXT("Live · 1 s sampling"));})]
                +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("PerformancePause")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,5))
                    .OnClicked_Lambda([this]{ToggleReadings();return FReply::Handled();})[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text)
                        .Text_Lambda([this]{return FText::FromString(bPaused?TEXT("Resume readings"):TEXT("Pause readings"));})]]]
            +SVerticalBox::Slot().FillHeight(1).Padding(0,4,0,0)[SAssignNew(Scroll,SScrollBox).Tag(TEXT("PerformanceScroll"))
                .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Rows]]]];
}

const TArray<FStudioPerformanceSample>& SStudioPerformancePanel::Samples() const
{return bPaused?Frozen:History->Samples();}
FStudioJobTelemetryView SStudioPerformancePanel::JobView() const
{const auto M=Model.Pin();return bPaused?FrozenJob:M?M->Job().Telemetry():FStudioJobTelemetryView();}
void SStudioPerformancePanel::ToggleReadings()
{
    if(bPaused){ResumeReadings();return;}
    Frozen=History->Samples();FrozenJob=JobView();FrozenState=JobValue(TEXT("Source"));
    FrozenAt=FDateTime::UtcNow().ToString(TEXT("%H:%M:%S UTC"));bPaused=true;
}
void SStudioPerformancePanel::ResumeReadings(){bPaused=false;Frozen.Reset();FrozenJob={};FrozenState.Empty();}
FReply SStudioPerformancePanel::OnKeyDown(const FGeometry& G,const FKeyEvent& E)
{
    const auto Key=E.GetKey();
    if(Key==EKeys::Escape){Close.ExecuteIfBound();return FReply::Handled();}
    if(Key==EKeys::Home){Scroll->ScrollToStart();return FReply::Handled();}
    if(Key==EKeys::End){Scroll->ScrollToEnd();return FReply::Handled();}
    if(Key==EKeys::PageDown||Key==EKeys::PageUp)
    {Scroll->SetScrollOffset(FMath::Max(0.f,Scroll->GetScrollOffset()+(Key==EKeys::PageDown?1.f:-1.f)*Scroll->GetCachedGeometry().GetLocalSize().Y*.8f));return FReply::Handled();}
    return SCompoundWidget::OnKeyDown(G,E);
}

FString SStudioPerformancePanel::AppValue(FName Key) const
{
    const auto& H=Samples();if(H.IsEmpty())return TEXT("Collecting…");
    const auto& S=H.Last();const auto& C=S.Counters;
    if(Key==TEXT("Host"))return TEXT("LBM Studio on ")+C.Host+TEXT(" · last 120 readings");
    if(Key==TEXT("Footprint"))return Bytes(C.FootprintBytes);
    if(Key==TEXT("CPU"))return Number(S.ProcessCPUPercent,TEXT("%"));
    if(Key==TEXT("Cadence"))return FString::Printf(TEXT("%.2f ms"),S.UICadenceMs);
    if(Key==TEXT("Update"))return FString::Printf(TEXT("%.2f ms"),S.UIUpdateMs);
    if(Key==TEXT("Captures"))return Number(S.CapturesPerSecond,TEXT("/ s"));
    if(Key==TEXT("CaptureTime"))return Number(C.LastCaptureSubmitMs,TEXT("ms"),2);
    if(Key==TEXT("BuildTime"))return Number(C.LastFieldBuildMs,TEXT("ms"),2);
    if(Key==TEXT("Workers"))return FString::FromInt(C.Workers);
    if(Key==TEXT("MeshBytes"))return Bytes(uint64(FMath::Max(int64(0),C.MeshBytes)));
    if(Key==TEXT("TextureBytes"))return Bytes(uint64(FMath::Max(int64(0),C.TextureBytes)));
    if(Key==TEXT("BuildSource"))return C.BuildSource.IsEmpty()?TEXT("No presented field build."):
        FString::Printf(TEXT("Last field build: %s · source frame %d"),*C.BuildSource,C.BuildFrame);
    if(Key==TEXT("Device"))return C.Device.IsEmpty()?TEXT("Render device unavailable"):TEXT("Render device: ")+C.Device;
    return FString();
}
FString SStudioPerformancePanel::JobValue(FName Key) const
{
    const auto M=Model.Pin();const auto V=JobView();
    if(Key==TEXT("Source"))
    {
        if(bPaused)return FrozenState;
        if(!M)return TEXT("No connected solver.");
        if(M->Job().Capabilities().bControlHarness)return TEXT("Control harness · ")+StudioJobs::StateName(M->Job().State())+TEXT("\nNo solver measurements. Recorded playback is independent.");
        return V.BackendId;
    }
    if(Key==TEXT("Status"))
    {
        switch(V.Status)
        {
        case EStudioTelemetryStatus::Current:return FString::Printf(TEXT("Current · %.1f s old"),V.AgeSeconds);
        case EStudioTelemetryStatus::Stale:return FString::Printf(TEXT("Stale · %.1f s old"),V.AgeSeconds);
        case EStudioTelemetryStatus::Disconnected:return TEXT("Disconnected");
        case EStudioTelemetryStatus::Final:return TEXT("Final sample");
        default:return TEXT("Unavailable");
        }
    }
    if(Key==TEXT("Attribution"))
    {
        if(!V.Sample)return TEXT("Throughput and ETA require measured solver counters. Application memory above belongs to LBM Studio.");
        const auto& S=*V.Sample;
        return FString::Printf(TEXT("%s · run %s\nHost: %s\nDevice: %s\nSample %llu · %.1f s old. ETA assumes recent rates continue."),
            *S.Reporter,*S.RunId.ToString(EGuidFormats::Short),*S.Host,*S.Device,S.Sequence,V.AgeSeconds);
    }
    if(Key==TEXT("Progress"))return V.Progress?FString::Printf(TEXT("%.1f %%"),*V.Progress*100.):TEXT("Indeterminate");
    if(Key==TEXT("Rate"))return Number(V.StepsPerSecond,TEXT("/ s"));
    if(Key==TEXT("ETA"))return Number(V.EstimatedRemainingSeconds,TEXT("s"));
    if(!V.Sample)return TEXT("Unavailable");const auto& S=*V.Sample;
    if(Key==TEXT("Steps"))return Counter(S.CompletedSteps);
    if(Key==TEXT("Physical"))return Number(S.PhysicalSeconds,TEXT("s"),4);
    if(Key==TEXT("Wall"))return Number(S.WallSeconds,TEXT("s"));
    if(Key==TEXT("HostMemory"))return Bytes(S.HostResidentBytes);
    if(Key==TEXT("DeviceMemory"))return Bytes(S.DeviceUsedBytes);
    if(Key==TEXT("DeviceUsage"))return Number(S.DeviceUtilizationPercent,TEXT("%"));
    return FString();
}
