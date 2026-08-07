// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "UI/FlowVizRenderSettingsViewModel.h"
#include "UI/SFlowVizTransferFunctionPanel.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SButton;

/**
 * Compositing, lighting, marching quality and sampling (plan.md sections 9 and
 * 10.4) -- the render controls that are not colour and not geometry.
 *
 * WHY THIS PANEL EXISTS, IN ONE SENTENCE: it is the production caller. The
 * sixteen parameters BACKLOG 2e names gained a writer when
 * FFlowVizRenderSettingsViewModel was built, and check_frozen_params.sh went
 * green -- while 14 of the view model's 17 setters still had no production
 * caller, so thirteen controls stayed welded to the view model's member
 * initialisers instead of FillDefaults' literals (#74). The freeze had moved
 * exactly one level up, past the edge of what that checker measures.
 * check_uncalled_setters.sh now asks the setter-level question, and this panel
 * is what makes its answer "called".
 *
 * NO STATE. Every control reads through to the view model on each paint, for
 * the reason SFlowVizClipPanel gives: a widget-side mirror is a second copy of
 * the model, and the first session load leaves the two disagreeing.
 *
 * THE CHANNEL TO THE RENDERER IS AN EVENT, NOT A CALL. This panel edits a view
 * model and fires OnRenderSettingsChanged; the workspace subscribes and calls
 * FFlowVizWorkspaceModel::PushRenderSettingsToVolume. The panel never mentions
 * a volume component, which is what lets it be built and operated before any
 * case is open -- and unlike the clip panel it needs no advisory for that
 * state, because render settings are value state: a composite mode chosen
 * before the case opens is simply ready when the case arrives.
 *
 * ANNOUNCED ONLY ON AN ACTUAL CHANGE. Refused edits (a zero step, a zero light
 * direction, unparseable text) and no-op edits (re-clicking the selected mode,
 * a clamp landing on the value already held) do not fire: a push on a refused
 * edit is a render-thread flush for a frame that is byte-identical.
 *
 * A NULL VIEW MODEL IS A LEGAL, INERT STATE -- the workspace builds panels
 * before a case is open. Every accessor tolerates it, which disables the
 * controls rather than crashing.
 */
class FLOWVIZRUNTIME_API SFlowVizRenderSettingsPanel : public SCompoundWidget
{
public:
	// nullptr initializer required -- SLATE_ARGUMENT does not zero-initialize,
	// and an omitted pointer argument holds indeterminate memory. That SIGBUS
	// already shipped once (FlowViz.UI.TransferFunctionPanel.Unbound).
	SLATE_BEGIN_ARGS(SFlowVizRenderSettingsPanel)
		: _ViewModel(nullptr)
	{
	}
		/** Borrowed, not owned. The workspace owns both. */
		SLATE_ARGUMENT(FFlowVizRenderSettingsViewModel*, ViewModel)

		/**
		 * Fired after an edit that CHANGES THE MODEL. The workspace subscribes
		 * and pushes the settings into the bound volume; nobody else should.
		 * Unbound is legal and inert.
		 */
		SLATE_EVENT(FSimpleDelegate, OnRenderSettingsChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* --- Test seams. See SFlowVizTransportBar.h for why these are public. --- */

	/**
	 * The button that selects a composite mode, or null for a mode with no
	 * button. A test must press the REAL button and observe the REAL view model
	 * change; calling SetCompositeMode directly would pass on a panel whose
	 * OnClicked was never wired -- the defect this seam exists to expose.
	 */
	TSharedPtr<SButton> GetCompositeModeButton(EFlowVizCompositeMode Mode) const;

	/** Iso-surface threshold entry. Enabled in IsoSurface mode only. */
	TSharedPtr<SFlowVizNumericEntry> GetIsoValueBox() const { return IsoValueBox; }

	/** Lighting on/off toggle. */
	TSharedPtr<SButton> GetLightingButton() const { return LightingButton; }

	/** Ambient term entry. Live only while lighting is on. */
	TSharedPtr<SFlowVizNumericEntry> GetAmbientBox() const { return AmbientBox; }

	/** Diffuse term entry. Live only while lighting is on. */
	TSharedPtr<SFlowVizNumericEntry> GetDiffuseBox() const { return DiffuseBox; }

	/** Light direction component entry: 0 = X, 1 = Y, 2 = Z. Null outside that range. */
	TSharedPtr<SFlowVizNumericEntry> GetLightDirectionBox(int32 Axis) const;

	/** Step size entry, in voxels. */
	TSharedPtr<SFlowVizNumericEntry> GetStepVoxelsBox() const { return StepVoxelsBox; }

	/** Opacity-correction reference step entry. */
	TSharedPtr<SFlowVizNumericEntry> GetReferenceStepBox() const { return ReferenceStepBox; }

	/** Step ceiling entry. */
	TSharedPtr<SFlowVizNumericEntry> GetMaxStepsBox() const { return MaxStepsBox; }

	/** Early-termination alpha entry. */
	TSharedPtr<SFlowVizNumericEntry> GetEarlyOutBox() const { return EarlyOutBox; }

	/** Jitter on/off toggle. */
	TSharedPtr<SButton> GetJitterButton() const { return JitterButton; }

	/** Jitter magnitude entry. Live only while jitter is on. */
	TSharedPtr<SFlowVizNumericEntry> GetJitterAmountBox() const { return JitterAmountBox; }

	/** Jitter seed entry. Live only while jitter is on. */
	TSharedPtr<SFlowVizNumericEntry> GetJitterSeedBox() const { return JitterSeedBox; }

	/** Trilinear / nearest field filtering toggle. */
	TSharedPtr<SButton> GetFilteringButton() const { return FilteringButton; }

	/** Strict status filtering toggle. */
	TSharedPtr<SButton> GetStrictFilterButton() const { return StrictFilterButton; }

	/** The no-data colour swatch at Index, or null if there is no such swatch. */
	TSharedPtr<SButton> GetNoDataSwatchButton(int32 Index) const;

	/** How many no-data swatches the panel offers. Static: the palette is a design decision, not state. */
	static int32 GetNoDataSwatchCount();

	/** The colour the swatch at Index applies. Black transparent for an out-of-range index. */
	static FLinearColor GetNoDataSwatchColor(int32 Index);

private:
	bool IsBound() const { return ViewModel != nullptr; }

	/* --- Handlers. Every one is null-safe and announces only on change. ----- */

	FReply OnCompositeModeClicked(EFlowVizCompositeMode Mode);
	FReply OnLightingClicked();
	FReply OnJitterClicked();
	FReply OnFilteringClicked();
	FReply OnStrictFilterClicked();
	FReply OnNoDataSwatchClicked(int32 Index);

	void OnIsoValueCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnAmbientCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnDiffuseCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnLightDirectionCommitted(
		const FText& NewText, ETextCommit::Type CommitType, int32 Axis);
	void OnStepVoxelsCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnReferenceStepCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnMaxStepsCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnEarlyOutCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnJitterAmountCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnJitterSeedCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/* --- Text bindings, so the boxes redisplay the REAL value each paint. --- */

	FText GetIsoValueText() const;
	FText GetAmbientText() const;
	FText GetDiffuseText() const;
	FText GetLightDirectionText(int32 Axis) const;
	FText GetStepVoxelsText() const;
	FText GetReferenceStepText() const;
	FText GetMaxStepsText() const;
	FText GetEarlyOutText() const;
	FText GetJitterAmountText() const;
	FText GetJitterSeedText() const;
	FText GetLightingLabel() const;
	FText GetJitterLabel() const;
	FText GetFilteringLabel() const;
	FText GetStrictFilterLabel() const;

	void NotifyRenderSettingsChanged();

	/** Borrowed. Null is the unbound state. */
	FFlowVizRenderSettingsViewModel* ViewModel = nullptr;

	/** Subscriber for edits. Unbound is legal and inert. */
	FSimpleDelegate OnRenderSettingsChanged;

	TMap<EFlowVizCompositeMode, TSharedPtr<SButton>> CompositeModeButtons;
	TSharedPtr<SFlowVizNumericEntry> IsoValueBox;
	TSharedPtr<SButton> LightingButton;
	TSharedPtr<SFlowVizNumericEntry> AmbientBox;
	TSharedPtr<SFlowVizNumericEntry> DiffuseBox;
	TSharedPtr<SFlowVizNumericEntry> LightDirectionBoxes[3];
	TSharedPtr<SFlowVizNumericEntry> StepVoxelsBox;
	TSharedPtr<SFlowVizNumericEntry> ReferenceStepBox;
	TSharedPtr<SFlowVizNumericEntry> MaxStepsBox;
	TSharedPtr<SFlowVizNumericEntry> EarlyOutBox;
	TSharedPtr<SButton> JitterButton;
	TSharedPtr<SFlowVizNumericEntry> JitterAmountBox;
	TSharedPtr<SFlowVizNumericEntry> JitterSeedBox;
	TSharedPtr<SButton> FilteringButton;
	TSharedPtr<SButton> StrictFilterButton;
	TArray<TSharedPtr<SButton>> NoDataSwatchButtons;
};
