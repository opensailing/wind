// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Playback/FlowVizCasePlayer.h"
#include "UI/FlowVizClipViewModel.h"
#include "UI/FlowVizProbeViewModel.h"
#include "UI/FlowVizSliceViewModel.h"
#include "UI/FlowVizTimelineViewModel.h"
#include "UI/FlowVizTransferFunctionViewModel.h"

/**
 * The live view models a workspace's panels share (plan.md section 5F).
 *
 * THIS ADDS NO STATE AND IT MUST NOT. Every field is one of the existing,
 * separately tested view models. This type exists only so the five panels can
 * be handed ONE object instead of five pointers, and so their lifetime is
 * managed in one place.
 *
 * WHY THAT RESTRICTION IS WRITTEN DOWN RATHER THAN ASSUMED. The obvious
 * shortcut when wiring a widget is to cache the value it displays - a bool for
 * "is playing", a float for the current range - and refresh it on a timer. That
 * cache is a SECOND copy of state whose owner is already authoritative, and the
 * two drift the moment anything changes the view model without going through
 * the widget: a session load, a keyboard shortcut, playback advancing on its
 * own. The drift shows up as a control that displays the wrong value until it
 * is touched, which reads as a refresh bug rather than as a duplicated model.
 * So the panels bind to ATTRIBUTES that read straight through to the view
 * model, and this struct holds no display state for them to read instead.
 *
 * THE PLAYER IS OWNED HERE, THE VIEW MODELS BORROW IT. FFlowVizTimelineViewModel
 * takes a borrowed FFlowVizCasePlayer* and its header is explicit that the
 * workspace "outlives the player at its own peril". Owning both here, with the
 * player declared BEFORE the view models, makes destruction order correct by
 * construction rather than by remembering to call Unbind.
 *
 * THREADING. Game thread only, because every member is.
 */
struct FLOWVIZRUNTIME_API FFlowVizWorkspaceModel
{
	FFlowVizWorkspaceModel();
	~FFlowVizWorkspaceModel();

	FFlowVizWorkspaceModel(const FFlowVizWorkspaceModel&) = delete;
	FFlowVizWorkspaceModel& operator=(const FFlowVizWorkspaceModel&) = delete;

	/**
	 * Declared FIRST so it is destroyed LAST.
	 *
	 * Members are destroyed in reverse declaration order, so the timeline view
	 * model - which holds a raw pointer to this player - dies before the player
	 * it points at. The alternative is a dangling borrowed pointer during
	 * teardown, which is a use-after-free that only reproduces on shutdown.
	 */
	FFlowVizCasePlayer Player;

	FFlowVizTimelineViewModel Timeline;
	FFlowVizTransferFunctionViewModel TransferFunction;
	FFlowVizClipViewModel Clip;
	FFlowVizSliceViewModel Slice;
	FFlowVizProbeViewModel Probes;

	/**
	 * Open a case directory and point every view model at it.
	 *
	 * ONE ENTRY POINT, because the alternative - each panel loading what it needs
	 * - is how the clip panel ends up sized for the previous case's domain. The
	 * domain size in particular reaches three separate view models, and a partial
	 * update leaves a slice slider calibrated in the wrong units.
	 *
	 * @param CaseDirectory Path to a `.cfdviz` directory or its manifest.json.
	 * @param FieldId       Field to display; NAME_None takes the first volume field.
	 * @return The first failure encountered. On failure the previous case is
	 *         closed rather than half-replaced, so no panel shows a mix of two.
	 */
	FCFDVizResult OpenCase(const FString& CaseDirectory, FName FieldId = NAME_None);

	/** True when a case is open and the panels have something to drive. */
	bool IsCaseOpen() const;

	/** Close the case and unbind every view model. Safe when nothing is open. */
	void CloseCase();

	/** The open case, or null. Panels use it to enumerate fields. */
	const FCFDVizCase* GetCase() const;

	/** Field ids of the open case's volume fields, in manifest order. Empty when no case is open. */
	TArray<FName> GetVolumeFieldIds() const;

	/**
	 * Bind a different field of the ALREADY OPEN case.
	 *
	 * Re-binds the transfer function too, so the colour range follows the field.
	 * A field change that left the previous field's range in place would colour
	 * pressure against a velocity domain - plausible-looking and wrong.
	 */
	FCFDVizResult SetField(FName FieldId);

private:
	/**
	 * The parsed case, kept alive for as long as the workspace holds it.
	 *
	 * FFlowVizCasePlayer takes a TSharedRef and keeps its own reference, but does
	 * not hand it back - the member is private. SetField needs to re-open the
	 * SAME case under a different field, so the workspace keeps its own reference
	 * rather than re-parsing the manifest from disk. Re-parsing would be file I/O
	 * on the game thread for data that has not changed (engineering rule 1), and
	 * would drop the frame cache for no reason.
	 */
	TSharedPtr<const FCFDVizCase> SharedCase;
};
