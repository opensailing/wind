#include "SStudioHome4Checkpoint.h"
#include "StudioHome4Checkpoint.h"
#include "StudioHome4Session.h"
#include "StudioFileDialog.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace { TOptional<FString> NextCheckpointPath; }
void SStudioHome4Checkpoint::SetNextPathForAutomation(const FString& Path) { NextCheckpointPath = Path; }
#endif
void SStudioHome4Checkpoint::Construct(const FArguments& Args)
{
    Editor = Args._Editor; Session = Args._Session;
    if (!Editor) Editor = MakeShared<FStudioHome4Session>(Args._Model);
    if (!Session) Session = MakeShared<FStudioHome4CheckpointSession>(Args._Model, Editor);
    using namespace StudioUI;
    ChildSlot
    [SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
        [SNew(STextBlock).Font(Font(10)).AutoWrapText(true).ColorAndOpacity(Muted)
            .Text(FText::FromString(TEXT("Warm start needs an original full checkpoint and a matching next-run grid. Source metadata must explicitly attest restart state.")))]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(SEditableTextBox).Tag(TEXT("Home4CheckpointPath")).Style(&InputStyle()).Font(Font(10))
            .Text_Lambda([this] { return FText::FromString(Editor->Get(TEXT("run.initState"))); })
            .OnTextChanged_Lambda([this](const FText& Value) { Editor->Set(TEXT("run.initState"), Value.ToString()); Error.Empty(); })
            .ToolTipText(FText::FromString(TEXT("Original NPZ checkpoint path. Changing any next-run field invalidates its inspection.")))]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 0)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)
            [SNew(SButton).Tag(TEXT("Home4CheckpointPick")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(6, 5))
                .IsEnabled_Lambda([this] { return !Session->IsBusy(); }).OnClicked_Lambda([this] { Pick(); return FReply::Handled(); })[Label(TEXT("Choose checkpoint…"), 9)]]
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)
            [SNew(SButton).Tag(TEXT("Home4CheckpointInspect")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(6, 5))
                .IsEnabled_Lambda([this] { return !Session->IsBusy(); }).OnClicked_Lambda([this] { Inspect(); return FReply::Handled(); })[Label(TEXT("Inspect grid and state"), 9)]]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Tag(TEXT("Home4CheckpointCancel")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(6, 5))
                .IsEnabled_Lambda([this] { return Session->IsBusy() || Session->State() == EStudioHome4CheckpointState::GridCompatible; })
                .OnClicked_Lambda([this] { Session->Cancel(); Error.Empty(); return FReply::Handled(); })[Label(TEXT("Cancel"), 9)]]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
        [SNew(STextBlock).Tag(TEXT("Home4CheckpointStatus")).Font(Font(9)).AutoWrapText(true).ColorAndOpacity(Muted)
            .Text_Lambda([this] { return FText::FromString(Error.IsEmpty() ? Session->Status() : Error); })]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
        [SNew(STextBlock).Tag(TEXT("Home4CheckpointMembers")).Font(Font(9)).AutoWrapText(true).ColorAndOpacity(Muted)
            .Text_Lambda([this] { return FText::FromString(Details()); })]
    ];
}
void SStudioHome4Checkpoint::Pick()
{
    FString Path; bool bSelected = false;
#if WITH_DEV_AUTOMATION_TESTS
    if (NextCheckpointPath.IsSet()) { Path = MoveTemp(*NextCheckpointPath); NextCheckpointPath.Reset(); bSelected = !Path.IsEmpty(); }
    else
#endif
    bSelected = StudioFileDialog::DataFile(false, TEXT("Choose original HOME4 full restart checkpoint"), {}, TEXT("npz"), Path);
    if (!bSelected) { Error = TEXT("Checkpoint selection cancelled."); return; }
    Editor->Set(TEXT("run.initState"), Path); Error.Empty();
}
void SStudioHome4Checkpoint::Inspect()
{
    FStudioHome4Spec Spec;
    Editor->Refresh();
    if (!Editor->Build(Spec, Error)) return;
    Session->Start(Spec.Run.InitState, Spec, Error);
}
FString SStudioHome4Checkpoint::Details() const
{
    const auto* Metadata = Session->Metadata(); if (!Metadata) return {};
    FString Text = TEXT("Original source: ") + Metadata->Source.Path;
    if (!Metadata->Source.SHA256.IsEmpty()) Text += TEXT("\nSHA-256: ") + Metadata->Source.SHA256;
    if (!Metadata->AxisOrder.IsEmpty()) Text += TEXT("\nOriginal array axes: ") + Metadata->AxisOrder;
    if (Metadata->OriginalStep.IsSet()) Text += TEXT(" · solver step ") + LexToString(*Metadata->OriginalStep);
    if (!Metadata->RequiredMembers.IsEmpty()) Text += TEXT("\nProducer-declared required state: ") + FString::Join(Metadata->RequiredMembers, TEXT(", "));
    for (const auto& Patch : Metadata->Patches)
        Text += FString::Printf(TEXT("\nPatch %s · body %s · level %d · origin %.17g, %.17g, %.17g · grid %d × %d × %d · %s · %d required members"), *Patch.Id, *Patch.BodyId, Patch.Level, Patch.OriginXYZ.X, Patch.OriginXYZ.Y, Patch.OriginXYZ.Z, Patch.DimensionsXYZ.X, Patch.DimensionsXYZ.Y, Patch.DimensionsXYZ.Z, *Patch.AxisOrder, Patch.RequiredMembers.Num());
    if (!Metadata->UndeclaredMembers.IsEmpty()) Text += TEXT("\nRetained additional members, restart status unknown: ") + FString::Join(Metadata->UndeclaredMembers, TEXT(", "));
    return Text;
}
