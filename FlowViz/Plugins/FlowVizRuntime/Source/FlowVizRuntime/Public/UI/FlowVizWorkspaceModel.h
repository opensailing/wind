// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Playback/FlowVizCasePlayer.h"
#include "UI/FlowVizClipViewModel.h"
#include "UI/FlowVizProbeViewModel.h"
#include "UI/FlowVizSliceViewModel.h"
#include "UI/FlowVizTimelineViewModel.h"
#include "UI/FlowVizTransferFunctionViewModel.h"

class UCFDVizVolumeComponent;

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

	/* --- The channel to the renderer ---------------------------------------- */

	/**
	 * Copy an edited clip model into a volume component so the image changes.
	 *
	 * THE LINK THAT DID NOT EXIST. The render path already clipped: the
	 * dispatcher calls FFlowVizClipViewModel::ApplyToRayMarchParameters on the
	 * proxy's model, fed from UCFDVizVolumeComponent::SetClip. What was missing
	 * was this direction - the panels edit FFlowVizWorkspaceModel::Clip, and
	 * nothing carried that into a component. Every production SetClip caller was
	 * UFlowVizCaptureLibrary, which builds its own model from Volume->GetClip().
	 * So a plane added in the UI was authored, validated, listed and persisted,
	 * and still did not change a pixel, while the identical plane added from
	 * Python did.
	 *
	 * STATIC, AND IT TAKES THE VIEW MODEL RATHER THAN THE WORKSPACE. The
	 * workspace model owns a case player and five view models; none of that is
	 * needed to move a clip model. Taking the smallest thing that could work
	 * keeps this callable from the capture library and from a test that has no
	 * player, and it makes the const-correctness obvious - the source is not
	 * modified, only read.
	 *
	 * THE DOMAIN COMES FROM THE VOLUME, NOT FROM THE SOURCE MODEL. A clip model
	 * carries whatever domain it was configured with, and the workspace's model
	 * gets its domain at OpenCase. Those agree right up until someone loads a
	 * second case into the same actor, at which point pushing the UI's domain
	 * would normalise the crop against the previous case's extent - which does
	 * not fail, it clips at a plausible wrong place. FlowVizCaptureLibrary.cpp
	 * re-reads GetPhysicalSize on every call for the same reason.
	 *
	 * @param Source The model the panels edit. Not modified.
	 * @param Volume Destination. Null is refused, not dereferenced.
	 * @return False when there was nothing to push into - a null component, or
	 *         one with no case bound and therefore no domain to normalise
	 *         against. False is "did not happen", not "went wrong": the
	 *         workspace pushes on every edit and a case is often not open yet.
	 */
	static bool PushClipToVolume(const FFlowVizClipViewModel& Source, UCFDVizVolumeComponent* Volume);

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
