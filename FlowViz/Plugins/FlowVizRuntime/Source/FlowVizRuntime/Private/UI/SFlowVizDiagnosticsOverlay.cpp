// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizDiagnosticsOverlay.h"

#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizDiagnostics.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlowVizDiagnosticsOverlay"

void SFlowVizDiagnosticsOverlay::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;

	const float U = FlowVizWorkspaceStyle::GetUnit();

	ChildSlot
	[
		SNew(SBorder)
			.BorderImage(FlowVizWorkspaceStyle::GetPanelBrush())
			/*
			 * TRANSLUCENT, so the volume behind stays readable. An opaque panel
			 * over the viewport hides the picture the numbers describe, which
			 * makes the overlay useless for the thing it is for: watching the
			 * frame rate while the image does something.
			 */
			.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f))
			.Padding(FMargin(2.0f * U))
		[
			SAssignNew(TextBlock, STextBlock)
				/*
				 * BOUND, NOT ASSIGNED. See the header: the failure this guards
				 * against is a Construct-time snapshot, which produces a panel
				 * of plausible numbers frozen at the instant the workspace
				 * opened. FlowViz.UI.DiagnosticsOverlay.Live drives the model
				 * between two reads and requires the text to have moved.
				 */
				.Text(TAttribute<FText>::CreateSP(this, &SFlowVizDiagnosticsOverlay::GetDisplayText))
				/*
				 * MONOSPACED, and for the reason FlowVizWorkspaceStyle gives:
				 * every row here is a number that changes every frame, and a
				 * proportional digit set makes the whole block jitter
				 * horizontally while playback runs.
				 */
				.Font(FlowVizWorkspaceStyle::GetNumericFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextPrimaryColor()))
		]
	];

	/*
	 * HIT-TEST INVISIBLE, ALWAYS. This widget sits on top of the viewport
	 * region. Slate's default visibility accepts hit tests, so an overlay left
	 * at the default would swallow every click and drag meant for the volume
	 * underneath it -- and the symptom is "the viewport stopped responding",
	 * which reads as a broken viewport rather than as a text panel in front of
	 * it. It displays; it is never a target.
	 *
	 * The workspace owns whether it is SHOWN, by wrapping this in a slot whose
	 * own visibility it drives. Putting the shown/hidden state here as well
	 * would give two objects an opinion about one thing.
	 */
	SetVisibility(EVisibility::HitTestInvisible);
}

void SFlowVizDiagnosticsOverlay::SetVolume(UCFDVizVolumeComponent* InVolume)
{
	Volume = InVolume;
}

FText SFlowVizDiagnosticsOverlay::GetDisplayText() const
{
	if (Model == nullptr)
	{
		/*
		 * SAID, NOT ZEROED. FlowVizDiagnostics::Format takes the same position
		 * for a closed case, and for the same reason: a screen of zeros is a
		 * measurement claim, and here it would be a claim about a workspace that
		 * does not exist.
		 */
		return LOCTEXT("NoModel", "FlowViz diagnostics\n  (no workspace)");
	}

	/*
	 * THE SAME COLLECTOR AND FORMATTER FlowViz.ShowDiagnostics prints. Two
	 * readouts of one session that formatted themselves independently would
	 * drift, and the drift would look like a measurement disagreeing with
	 * itself.
	 */
	return FText::FromString(
		FlowVizDiagnostics::Format(FlowVizDiagnostics::Collect(*Model, Volume.Get())));
}

#undef LOCTEXT_NAMESPACE
