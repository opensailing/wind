// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

struct FFlowVizWorkspaceModel;
class STextBlock;
class UCFDVizVolumeComponent;

/**
 * The on-screen diagnostics readout (plan.md section 17).
 *
 * THE OTHER HALF OF FlowVizDiagnostics. That header made a deliberate split:
 * `Collect` reads the numbers and `Format` turns them into rows, both as free
 * functions a test can assert exactly, so that no measurement lives inside a
 * paint path where the only way to check it is to scrape a widget. This widget
 * is what that split left over -- presentation, and nothing else. It computes no
 * value of its own, and it must not start: a row calculated here would be a
 * measurement with no test able to reach it, which is the shape
 * FlowVizDiagnostics.h exists to prevent.
 *
 * SO WHAT IS THERE TO GET WRONG, IF THE NUMBERS ARE ALREADY TESTED?
 *
 * Exactly one thing, and it is the reason this widget needs a test at all: WHEN
 * the text is computed. The obvious implementation calls Collect and Format in
 * Construct and hands the resulting FText to an STextBlock. That compiles, looks
 * correct in review, and produces a panel of entirely plausible numbers that
 * never change again -- frozen at the instant the workspace opened, which is
 * before a case is loaded, so every row reads zero forever. A screenshot of that
 * bug is indistinguishable from a screenshot of a working overlay on an idle
 * session, and "FPS 0.0, no case open" is what a correct overlay shows then too.
 *
 * The text is therefore a BOUND ATTRIBUTE re-evaluated every frame, and
 * GetDisplayText is public so a test can drive the model, ask this widget what
 * it displays, and require the answer to have moved. Recomputing per paint is
 * affordable because Collect only reads counters that are already in memory, and
 * because this widget is Collapsed unless the user asked for it -- a collapsed
 * widget is not painted, so a hidden overlay costs nothing.
 *
 * HIDDEN BY DEFAULT, AND HIT-TEST-INVISIBLE WHEN SHOWN. It sits on top of the
 * viewport region, which is the picture the user came to look at; an overlay
 * that is on by default covers that picture, and one that accepts hit tests
 * swallows clicks meant for the volume behind it. `FlowViz.ShowDiagnostics`
 * toggles it.
 *
 * A NULL MODEL IS A LEGAL, INERT STATE, as it is for every other panel here: the
 * workspace constructs its children in an order this widget does not get to
 * choose. It renders a single line saying so rather than a screen of zeros,
 * because zeros are a measurement claim.
 */
class FLOWVIZRUNTIME_API SFlowVizDiagnosticsOverlay : public SCompoundWidget
{
public:
	/**
	 * THE INITIALIZER IS LOAD-BEARING -- see SFlowVizProbePanel.h. SLATE_ARGUMENT
	 * expands to a bare member with no initializer, so an omitted pointer holds
	 * indeterminate memory and every null check downstream passes.
	 */
	SLATE_BEGIN_ARGS(SFlowVizDiagnosticsOverlay)
		: _Model(nullptr)
	{
	}
		/** Borrowed, not owned. The workspace owns the model and outlives this widget. */
		SLATE_ARGUMENT(FFlowVizWorkspaceModel*, Model)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * Point the overlay at the volume whose resolution it should report. Null
	 * unbinds.
	 *
	 * WEAK, for the reason SFlowVizWorkspace::SetVolume gives: the component's
	 * world can be torn down while a docked tab is still open. Without a volume
	 * the resolution row reports zeros -- which FlowVizDiagnostics::Collect
	 * documents as "no upload has happened", not as a 0x0x0 volume.
	 */
	void SetVolume(UCFDVizVolumeComponent* InVolume);

	/**
	 * The exact string on screen, recomputed now.
	 *
	 * This is what the bound attribute returns, so a test reading it is reading
	 * what a user sees rather than a parallel implementation of it. Public for
	 * that reason alone.
	 */
	FText GetDisplayText() const;

	/** Test seam: the block the text is actually bound to. See SFlowVizTransportBar.h. */
	TSharedPtr<STextBlock> GetTextBlock() const { return TextBlock; }

private:
	/** Borrowed. Null is legal and reported on screen. */
	FFlowVizWorkspaceModel* Model = nullptr;

	/** Weak, per SetVolume. */
	TWeakObjectPtr<UCFDVizVolumeComponent> Volume;

	TSharedPtr<STextBlock> TextBlock;
};
