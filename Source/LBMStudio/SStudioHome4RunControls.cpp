#include "SStudioHome4RunControls.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include <cerrno>
#include <cstdlib>

namespace StudioHome4RunControlsPrivate
{
    bool PositiveNumber(const FString& Text, double& Out)
    {
        const FString Trimmed = Text.TrimStartAndEnd();
        if (Trimmed.IsEmpty() || Trimmed.Len() > 64) return false;
        int32 I = 0, Digits = 0;
        auto Digit = [](TCHAR C) { return C >= '0' && C <= '9'; };
        if (Trimmed[I] == '+') ++I;
        while (I < Trimmed.Len() && Digit(Trimmed[I])) { ++I; ++Digits; }
        if (I < Trimmed.Len() && Trimmed[I] == '.')
        { ++I; while (I < Trimmed.Len() && Digit(Trimmed[I])) { ++I; ++Digits; } }
        if (!Digits) return false;
        if (I < Trimmed.Len() && (Trimmed[I] == 'e' || Trimmed[I] == 'E'))
        {
            ++I; if (I < Trimmed.Len() && (Trimmed[I] == '+' || Trimmed[I] == '-')) ++I;
            int32 ExponentDigits = 0;
            while (I < Trimmed.Len() && Digit(Trimmed[I])) { ++I; ++ExponentDigits; }
            if (!ExponentDigits) return false;
        }
        if (I != Trimmed.Len()) return false;
        const FTCHARToUTF8 Bytes(*Trimmed);
        char* End = nullptr; errno = 0;
        Out = std::strtod(Bytes.Get(), &End);
        return errno != ERANGE && End == Bytes.Get() + Bytes.Length() && FMath::IsFinite(Out) && Out > 0;
    }
    FString Interval(const TOptional<int64>& Value)
    { return Value ? FString::Printf(TEXT("every %lld control steps"), *Value) : TEXT("cadence unavailable"); }
    const TCHAR* KindName(EStudioJobScheduledOutputKind Kind)
    {
        switch (Kind)
        {
        case EStudioJobScheduledOutputKind::Trace: return TEXT("Trace");
        case EStudioJobScheduledOutputKind::Slice: return TEXT("Slice");
        case EStudioJobScheduledOutputKind::Visualization: return TEXT("Visualization");
        case EStudioJobScheduledOutputKind::Restart: return TEXT("Restart");
        }
        return TEXT("Unknown");
    }
}
void SStudioHome4RunControls::Construct(const FArguments& A)
{
    using namespace StudioUI;
    Model = A._Model; OnSubmit = A._OnSubmit; OutputSizeDraft.SetNum(4);
    auto Action = [&](const TCHAR* Caption, FName Tag, TFunction<bool()> Enabled, TFunction<void()> Work)
    {
        return SNew(SButton).Tag(Tag).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7, 5))
            .IsEnabled_Lambda([Enabled] { return Enabled(); }).OnClicked_Lambda([Work] { Work(); return FReply::Handled(); })[Label(Caption, 9)];
    };
    const auto Rows = SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 7)[Label(TEXT("Development run controls"), 12, Text, true)];
    Rows->AddSlot().AutoHeight()[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 5, 0)[Action(TEXT("Development harness"), TEXT("Home4RunHarness"),
            [this] { const auto M = Model.Pin(); return M && !M->IsProjectOpenPending() && (!M->HasActiveJob() || M->Project.bControlHarness); },
            [this] { if (const auto M = Model.Pin()) { M->SetControlHarness(true); InputError.Empty(); } })]
        + SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("Recorded replay"), TEXT("Home4RunReplay"),
            [this] { const auto M = Model.Pin(); return M && !M->IsProjectOpenPending() && (!M->HasActiveJob() || !M->Project.bControlHarness); },
            [this] { if (const auto M = Model.Pin()) { M->SetControlHarness(false); InputError.Empty(); } })]];
    Rows->AddSlot().AutoHeight().Padding(0, 6)[SNew(STextBlock).Tag(TEXT("Home4RunCapabilities")).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this] { return FText::FromString(CapabilityText()); })];
    Rows->AddSlot().AutoHeight()[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 5, 0)[Action(TEXT("Submit / Resume"), TEXT("Home4RunSubmit"),
            [this] { const auto M = Model.Pin(); return M && M->Project.bControlHarness && M->CanControl(EStudioJobCommand::Submit); }, [this] { Submit(); })]
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 5, 0)[Action(TEXT("Pause"), TEXT("Home4RunPause"),
            [this] { const auto M = Model.Pin(); return M && M->Project.bControlHarness && M->Job().Can(EStudioJobCommand::Pause); },
            [this] { if (const auto M = Model.Pin()) { M->Control(EStudioJobCommand::Pause); InputError.Empty(); } })]
        + SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("Stop"), TEXT("Home4RunStop"),
            [this] { const auto M = Model.Pin(); return M && M->Project.bControlHarness && M->CanControl(EStudioJobCommand::Stop); },
            [this] { if (const auto M = Model.Pin()) { M->Control(EStudioJobCommand::Stop); InputError.Empty(); } })]];
    Rows->AddSlot().AutoHeight().Padding(0, 8)[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Label(TEXT("N"), 9, Muted)]
        + SHorizontalBox::Slot().FillWidth(1).Padding(6, 0)[SNew(SEditableTextBox).Tag(TEXT("Home4RunStepCount")).Style(&InputStyle()).Font(Font(9))
            .Text_Lambda([this] { return FText::FromString(StepDraft); }).OnTextChanged_Lambda([this](const FText& Value) { StepDraft = Value.ToString(); })]
        + SHorizontalBox::Slot().AutoWidth()[Action(TEXT("Step N"), TEXT("Home4RunStepN"),
            [this] { const auto M = Model.Pin(); return M && M->CanHome4StepN(); }, [this] { StepRange(); })]];
    Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 8)[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Label(TEXT("t*"), 9, Muted)]
        + SHorizontalBox::Slot().FillWidth(1).Padding(6, 0)[SNew(SEditableTextBox).Tag(TEXT("Home4RunTargetTime")).Style(&InputStyle()).Font(Font(9)).HintText(FText::FromString(TEXT("Explicit target")))
            .Text_Lambda([this] { return FText::FromString(TargetDraft); }).OnTextChanged_Lambda([this](const FText& Value) { TargetDraft = Value.ToString(); })]
        + SHorizontalBox::Slot().AutoWidth()[Action(TEXT("Run to t*"), TEXT("Home4RunToTime"),
            [this] { const auto M = Model.Pin(); return M && M->CanHome4RunToDimensionless(); }, [this] { RunTo(); })]];
    Rows->AddSlot().AutoHeight().Padding(0, 3)[SNew(STextBlock).Tag(TEXT("Home4RunFrozenCase")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this] { return FText::FromString(FrozenText()); })];
    Rows->AddSlot().AutoHeight().Padding(0, 4)[SNew(STextBlock).Tag(TEXT("Home4RunControlStatus")).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true)
        .Text_Lambda([this] { return FText::FromString(StatusText()); })];
    Rows->AddSlot().AutoHeight().Padding(0, 4)[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text(FText::FromString(TEXT("Warm-start grid compatibility: unavailable without actual restart metadata. Shared GPU occupancy, memory, power and temperature: unavailable without a rack connection.")))];
    Rows->AddSlot().AutoHeight().Padding(0, 7, 0, 3)[Label(TEXT("Scheduled output requests · no files generated"), 10, Text, true)];
    const TCHAR* OutputNames[]={TEXT("Trace"),TEXT("Slice"),TEXT("Visualization"),TEXT("Restart")};
    Rows->AddSlot().AutoHeight().Padding(0,4)[Label(TEXT("Optional user storage estimates · bytes per output"),8,Muted)];
    for(int32 I=0;I<4;++I)
        Rows->AddSlot().AutoHeight().Padding(0,2)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(OutputNames[I],8,Muted)]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(120)[SNew(SEditableTextBox)
                .Tag(FName(*FString::Printf(TEXT("Home4OutputSize%d"),I))).Style(&InputStyle()).Font(Font(8)).HintText(FText::FromString(TEXT("Unknown")))
                .Text_Lambda([this,I]{return FText::FromString(OutputSizeDraft[I]);})
                .OnTextChanged_Lambda([this,I](const FText& Value){OutputSizeDraft[I]=Value.ToString();ScheduleCount=INDEX_NONE;if(ScheduleRows)RefreshSchedule();})]]];
    Rows->AddSlot().AutoHeight()[SAssignNew(ScheduleRows, SVerticalBox)];
    ChildSlot[SNew(SBorder).BorderImage(&PanelBrush).Padding(10)[Rows]];
    RefreshSchedule();
}
void SStudioHome4RunControls::Tick(const FGeometry& G, double Now, float Delta)
{ SCompoundWidget::Tick(G, Now, Delta); RefreshSchedule(); }
void SStudioHome4RunControls::Submit()
{
    InputError.Empty();
    if (OnSubmit.IsBound()) OnSubmit.Execute();
    else if (const auto M = Model.Pin()) M->Control(EStudioJobCommand::Submit);
}
void SStudioHome4RunControls::StepRange()
{
    using namespace StudioHome4RunControlsPrivate;
    double Value = 0;
    if (!PositiveNumber(StepDraft, Value) || Value > double(FStudioJobController::MaxSimulatedControlSteps) || FMath::FloorToDouble(Value) != Value)
    { InputError = TEXT("Step N requires a positive bounded integer."); return; }
    InputError.Empty(); if (const auto M = Model.Pin()) M->Home4StepN(int64(Value));
}
void SStudioHome4RunControls::RunTo()
{
    using namespace StudioHome4RunControlsPrivate;
    double Value = 0;
    if (!PositiveNumber(TargetDraft, Value)) { InputError = TEXT("Run to t* requires a finite positive target."); return; }
    InputError.Empty(); if (const auto M = Model.Pin()) M->Home4RunToDimensionless(Value);
}
FString SStudioHome4RunControls::CapabilityText() const
{
    const auto M = Model.Pin(); if (!M) return TEXT("No owning session.");
    if (!M->Project.bControlHarness) return TEXT("Recorded replay selected. HOME4 development range commands are unavailable; recording playback remains on the toolbar.");
    const auto& C = M->Job().Capabilities();
    FString Requested = TEXT("Unknown");
    FString QueueTarget = TEXT("Unavailable"), Device = TEXT("Unavailable");
    if(M->Project.Draft.Home4)
    {
        const auto& Run=M->Project.Draft.Home4->Run;
        if(!Run.QueueTarget.IsEmpty())QueueTarget=Run.QueueTarget;
        if(!Run.Device.IsEmpty())Device=Run.Device;
    }
    if (M->Project.Draft.Home4) switch (M->Project.Draft.Home4->Run.Backend)
    {
    case EStudioHome4Backend::Metal: Requested = TEXT("Metal"); break;
    case EStudioHome4Backend::CUDA: Requested = TEXT("CUDA"); break;
    case EStudioHome4Backend::PyTorch: Requested = TEXT("PyTorch fallback"); break;
    default: break;
    }
    return FString::Printf(TEXT("Development harness · %s · %s\nRequested numerical backend: %s · device: %s · queue: %s. No solver extension or remote queue is connected.\nStep N: %s · run to t*: %s · cadence scheduling: %s. No CFD measurements or numerical gate results are generated."),
        *C.BackendId, *StudioJobs::StateName(M->Job().State()), *Requested, *Device, *QueueTarget, C.bStepN ? TEXT("supported") : TEXT("unavailable"),
        C.bRunToDimensionless ? TEXT("supported") : TEXT("unavailable"), C.bOutputSchedule ? TEXT("supported") : TEXT("unavailable"));
}
FString SStudioHome4RunControls::FrozenText() const
{
    const auto M = Model.Pin(); if (!M || !M->Job().Run()) return TEXT("Submit captures the current case as an immutable development run.");
    const auto& R = *M->Job().Run();
    return FString::Printf(TEXT("Captured case: %s · run %s. Range controls use this run's frozen HOME4 reference and output cadences. Later draft edits apply to the next run."), *R.GetName(), *R.GetId().ToString(EGuidFormats::Short));
}
FString SStudioHome4RunControls::StatusText() const
{
    if (!InputError.IsEmpty()) return InputError;
    const auto M = Model.Pin(); if (!M) return TEXT("Unavailable");
    return FString::Printf(TEXT("Acknowledged simulated control steps: %lld · range replies: %llu\n%s"), M->Job().SimulatedControlSteps(), M->Job().CompletedStepCommands(), *M->Notice);
}
void SStudioHome4RunControls::RefreshSchedule()
{
    using namespace StudioHome4RunControlsPrivate;
    const auto M = Model.Pin(); if (!M) return;
    const auto& Notices = M->Job().ScheduledOutputs();
    const FGuid RunId = M->Job().Run() ? M->Job().Run()->GetId() : FGuid();
    const uint64 LastCommand = Notices.IsEmpty() ? 0 : Notices.Last().CommandId;
    if (ScheduleRun == RunId && ScheduleCount == Notices.Num() && ScheduleLastCommand == LastCommand) return;
    ScheduleRun = RunId; ScheduleCount = Notices.Num(); ScheduleLastCommand = LastCommand; ScheduleRows->ClearChildren();
    const FStudioHome4Run* Run = nullptr;
    if (M->Job().Run()) if (const auto* Captured = M->Job().Run()->GetConfiguration()) if (Captured->Home4) Run = &Captured->Home4->Run;
    const TOptional<int64> Cadences[] = {Run ? Run->MeasureEvery : TOptional<int64>(), Run ? Run->SaveEvery : TOptional<int64>(),
        Run ? Run->VizEvery : TOptional<int64>(), Run ? Run->RestartEvery : TOptional<int64>()};
    for (int32 I = 0; I < 4; ++I)
    {
        const auto Kind = EStudioJobScheduledOutputKind(I);
        FString Storage=TEXT("storage size unavailable");
        TOptional<uint64> BytesPerOutput;
        if(!OutputSizeDraft[I].TrimStartAndEnd().IsEmpty())
        {
            double Value=0;
            if(PositiveNumber(OutputSizeDraft[I],Value)&&Value<=1.e12&&FMath::FloorToDouble(Value)==Value)
            {BytesPerOutput=uint64(Value);Storage=FString::Printf(TEXT("user estimate %llu bytes/output"),*BytesPerOutput);}
            else Storage=TEXT("invalid storage estimate; enter positive integer bytes");
        }
        FString Text = FString(KindName(Kind)) + TEXT(" · ") + Interval(Cadences[I]) + TEXT(" · ")+Storage;
        for (const auto& N : Notices) if (N.Kind == Kind)
        {
            FString RangeStorage=Storage;
            if(BytesPerOutput)
                RangeStorage=uint64(N.Crossings)<=MAX_uint64/ *BytesPerOutput ?
                    FString::Printf(TEXT("user range estimate %llu bytes"),uint64(N.Crossings)* *BytesPerOutput):TEXT("storage estimate exceeds integer range");
            Text = FString::Printf(TEXT("%s · %lld scheduled crossings, steps %lld–%lld · %s"), KindName(Kind), N.Crossings, N.FirstStep, N.LastStep,*RangeStorage);
        }
        ScheduleRows->AddSlot().AutoHeight().Padding(0, 3)[SNew(STextBlock).Tag(FName(*FString::Printf(TEXT("Home4RunOutput%d"), I))).Font(StudioUI::Font(8)).ColorAndOpacity(StudioUI::Muted)
            .AutoWrapText(true).Text(FText::FromString(Text))];
    }
}
