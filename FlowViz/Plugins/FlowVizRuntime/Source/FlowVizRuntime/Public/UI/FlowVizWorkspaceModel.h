// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Flow/FlowVizChartSeries.h"
#include "Flow/FlowVizFlowInspection.h"
#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"
#include "Flow/FlowVizParticles.h"
#include "Scene/FlowVizMeshPayload.h"
#include "Playback/FlowVizCasePlayer.h"
#include "UI/FlowVizClipViewModel.h"
#include "UI/FlowVizProbeViewModel.h"
#include "UI/FlowVizRenderSettingsViewModel.h"
#include "UI/FlowVizSliceViewModel.h"
#include "UI/FlowVizTimelineViewModel.h"
#include "UI/FlowVizTransferFunctionViewModel.h"

class UCFDVizVolumeComponent;
struct FFlowVizSessionState;

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
	 * Compositing, lighting, marching and sampling -- the render controls that
	 * are not colour and not geometry.
	 *
	 * Added with #74: the view model existed and the dispatcher read one, but
	 * the workspace never held one, so no panel could edit what the renderer
	 * saw. Default-constructed it is an identity over FillDefaults' output
	 * (asserted by FlowViz.UI.RenderSettings), so a workspace that never touches
	 * it renders exactly as before.
	 */
	FFlowVizRenderSettingsViewModel RenderSettings;

	/* --- Profiles (#83, plan.md section 14) --------------------------------- */

	/**
	 * Scientific or Presentation (VISUAL_QA.md's two bars).
	 *
	 * WHAT THE TOGGLE HONESTLY DOES in v0.1: it owns the session's
	 * bPresentationMode field (which previously round-tripped with no owner)
	 * and applies a render preset bundle -- Scientific: unlit, no jitter,
	 * disclosure colours in force; Presentation: gradient lighting and jitter
	 * on. It does NOT deliver VISUAL_QA section 2's film-grade bar (multiple
	 * scattering, multi-scale density, tone mapping); that bar is stated in
	 * the doc and open in the backlog, and pretending a lighting preset meets
	 * it would be the exact fake-success rule 15 forbids.
	 *
	 * SWITCHING APPLIES THE BUNDLE ONCE, then the user may adjust freely: a
	 * profile that kept re-imposing itself would fight every slider in the
	 * render panel. So this is "is presentation mode SELECTED", not "are the
	 * current settings presentation-shaped".
	 */
	void SetPresentationMode(bool bInPresentation);
	bool IsPresentationMode() const { return bPresentationMode; }

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

	/* --- Sessions ------------------------------------------------------------ */

	/**
	 * Write every view model to a `.cfdvizsession` file.
	 *
	 * WHAT DOES NOT ROUND-TRIP, STATED HERE RATHER THAN DISCOVERED. FFlowVizSessionState
	 * carries four things this workspace has no owner for:
	 *
	 *     bPresentationMode   PanelVisibility   Camera{Location,Rotation}   Annotations
	 *
	 * They are in the session struct because plan.md section 14 names them, and
	 * they are read and written by the JSON layer - but nothing in
	 * FFlowVizWorkspaceModel holds them, so a save writes their defaults and a
	 * load discards them. That is not a bug to fix here: presentation mode and
	 * panel visibility belong to the Slate workspace, the camera to the viewport
	 * client, and annotations to a scene actor. When those owners exist, the
	 * fix is to route them through here - NOT to add copies to this struct, for
	 * the reason the class comment gives.
	 *
	 * Until then, saying so in the signature's own documentation is what keeps a
	 * future reader from concluding, from a green round-trip test, that the
	 * camera is persisted.
	 *
	 * @param FilePath Destination. The extension is not enforced; a caller that
	 *        wants `.cfdvizsession` appends FlowVizSession::GetFileExtension().
	 *        Parent directories are created by the write.
	 * @return The write's failure, or Ok. Saving with NO CASE OPEN is legal and
	 *         produces a session with an empty case path - which is what makes a
	 *         "save my colour setup" workflow expressible.
	 */
	FCFDVizResult SaveSession(const FString& FilePath) const;

	/**
	 * Load a `.cfdvizsession` and become it.
	 *
	 * THE CASE IS RE-OPENED FIRST, AND THAT ORDER IS THE WHOLE FUNCTION.
	 * FlowVizSession::ApplyToViewModels opens nothing - it pushes values into
	 * whatever the view models currently are. Applying before opening would be
	 * undone twice over: FFlowVizCasePlayer::Open resets Settings and seeks to
	 * frame 0, and FFlowVizClipViewModel::SetDomainSize (which OpenCase calls)
	 * invokes ResetCropBox. Both discard restored state while reporting Ok, so
	 * the failure would present as a session that loads successfully and comes
	 * back subtly wrong.
	 *
	 * A MISSING CASE IS NOT A FAILURE OF THE LOAD, matching the format's own
	 * contract (plan.md section 14: "must allow the user to relink"). When the
	 * session's case cannot be found, this reports the failure but STILL applies
	 * everything that does not need a case - so a relink can follow without the
	 * user losing their colour map, clip planes and probes. Callers that want to
	 * offer a relink dialog should use FlowVizSession::LoadFromFile and
	 * RelinkCase directly, then LoadState.
	 *
	 * @return The FIRST failure, after applying everything it could. Ok when the
	 *         whole session applied. A non-Ok result does NOT mean nothing
	 *         happened - see ApplyToViewModels.
	 */
	FCFDVizResult LoadSession(const FString& FilePath);

	/**
	 * Apply an already-parsed session. The half of LoadSession after the read.
	 *
	 * Separate so a relink flow - load, discover the case is missing, ask the
	 * user, RelinkCase, apply - does not have to write the file back out to a
	 * temporary just to re-read it.
	 *
	 * @param State A state from FlowVizSession::LoadFromFile, possibly relinked.
	 */
	FCFDVizResult LoadState(const FFlowVizSessionState& State);

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

	/**
	 * Compose the user's clip with the slice's slab planes into one model (#77).
	 *
	 * A visible slice IS two opposed clip planes; the volume receives ONE clip
	 * model, so the composition happens here at push time -- never inside the
	 * clip view model the user edits, which would show phantom rows in the clip
	 * panel and let a deleted "slice plane" half-disable a control that lives
	 * in another panel. A hidden slice contributes nothing, so hiding it
	 * retracts exactly its own two planes on the next push.
	 *
	 * @return The composed model: the user's planes and crop, plus the slab
	 *         when the slice is visible and has a domain. When the slab would
	 *         not fit (the user already has more than MaxClipPlanes - 2
	 *         enabled), the slab is DROPPED and the user's planes win -- a
	 *         slice that silently deleted a user's plane would be worse than a
	 *         slice that does not appear, and the panel's advisory names the
	 *         six-plane budget.
	 */
	static FFlowVizClipViewModel ComposeClipWithSlice(
		const FFlowVizClipViewModel& Clip, const FFlowVizSliceViewModel& Slice);

	/**
	 * Copy the transfer function into a volume component.
	 *
	 * WHY THIS IS SO MUCH SIMPLER THAN PushClipToVolume, and why that difference
	 * is real rather than an omission. A clip model has to be renormalised
	 * against the volume's physical extent, which is why that function re-reads
	 * GetPhysicalSize, refuses a volume with no case bound, and goes to some
	 * trouble to preserve a dragged crop across the domain change.
	 *
	 * A transfer function has no such dependency. It maps VALUES to colours, and
	 * the value range is the field's, which the view model already tracks
	 * through BindField -- nothing about it is expressed in the volume's
	 * geometry. So there is no domain to impose and nothing to preserve across
	 * imposing it, and pushing into a volume with no case bound is harmless: the
	 * colours are simply ready when the data arrives.
	 *
	 * That is worth stating because the SYMMETRY IS TEMPTING AND WRONG. Adding a
	 * GetPhysicalSize guard here to match the sibling would refuse the push
	 * during exactly the window a user spends setting up -- pick a colormap
	 * before opening a case, and it would be silently dropped.
	 *
	 * @param Source The model the panels edit. Not modified.
	 * @param Volume Destination. Null is refused, not dereferenced.
	 * @return False only when there was no component to push into.
	 */
	static bool PushTransferFunctionToVolume(
		const FFlowVizTransferFunctionViewModel& Source, UCFDVizVolumeComponent* Volume);

	/**
	 * Copy the render settings into a volume component.
	 *
	 * SHAPED LIKE THE TRANSFER FUNCTION'S PUSH, NOT THE CLIP'S, and for the
	 * same reason its header spells out: render settings are VALUE state. A
	 * composite mode, a step size and a light direction mean the same thing
	 * whether or not a case is open, so there is no domain to renormalise
	 * against and no extent guard to refuse on. Adding the clip's
	 * GetPhysicalSize guard here would silently drop every mode chosen while
	 * the case dialog was still open.
	 *
	 * @param Source The model the panel edits. Not modified.
	 * @param Volume Destination. Null is refused, not dereferenced.
	 * @return False only when there was no component to push into.
	 */
	static bool PushRenderSettingsToVolume(
		const FFlowVizRenderSettingsViewModel& Source, UCFDVizVolumeComponent* Volume);

	/* --- The sampling service (#75) ----------------------------------------- */
	/*
	 * The production caller of the last two orphaned setters. SetProbeReading
	 * was "the seam an async readback would call", and nothing did -- probes
	 * could be placed, named and persisted, and could never display a value.
	 * SetCurrentFrameRange was why the "Per frame" range source stayed
	 * permanently disabled: its enablement gate is HasCurrentFrameRange(), and
	 * nothing ever measured one.
	 *
	 * Request -> worker -> drain, the same shape as the player's decode path
	 * and for the same reason: sampling is disk I/O and zlib, which engineering
	 * rule 1 forbids on the game thread. Everything the task touches is copied
	 * or shared, never a pointer back into this model.
	 */

	/**
	 * Sample every visible probe and measure the displayed frame's value range,
	 * on a worker. A no-op with no case open, or while a request is in flight
	 * (the newest data wins; queueing stale requests would apply readings for
	 * a frame the display has left).
	 */
	void RequestSampleUpdate();

	/**
	 * Apply completed results: probe readings through SetProbeReading, the
	 * frame range through SetCurrentFrameRange.
	 *
	 * GAME THREAD, like every other mutation of these view models. Call it
	 * from a tick (SFlowVizWorkspace::TickClock does) or after
	 * WaitForPendingSamples in a test.
	 *
	 * @return True when anything was applied.
	 */
	bool DrainSampleResults();

	/** Block until the in-flight sample (if any) reports. Test plumbing, matching FFlowVizCasePlayer::WaitForPendingLoads. */
	bool WaitForPendingSamples(double TimeoutSeconds = 30.0);

	/**
	 * The line probe's latest distance series (#85), measured by the sampling
	 * service at the displayed frame. Empty (no range) until a line exists and
	 * a sample has drained. AN EXCEPTION to "this struct adds no state", made
	 * knowingly: the series is a MEASUREMENT with no view-model owner -- it
	 * belongs to no panel's editable state, and the probe view model is pure
	 * value state that must not grow a worker-fed cache.
	 */
	const FFlowVizChartSeries& GetLineSeries() const { return LineSeries; }

	/** The drained cut-plane mesh (renderer overhaul P3). ConsumeCutPlane clears the fresh flag; the payload stays readable. */
	bool HasFreshCutPlane() const { return bCutPlaneFresh; }
	const FFlowVizMeshPayload& ConsumeCutPlane()
	{
		bCutPlaneFresh = false;
		return CutPlanePayload;
	}
	double GetCutPlaneRangeMin() const { return CutPlaneRangeMin; }
	double GetCutPlaneRangeMax() const { return CutPlaneRangeMax; }

	/** The drained iso surface (renderer overhaul P4), same consume contract. */
	bool HasFreshIsoSurface() const { return bIsoSurfaceFresh; }
	const FFlowVizMeshPayload& ConsumeIsoSurface()
	{
		bIsoSurfaceFresh = false;
		return IsoSurfacePayload;
	}
	double GetLastIsoValueUsed() const { return LastIsoValueUsed; }

	/*
	 * THE MODE TOGGLES (renderer overhaul P6). Independent switches, the
	 * FluidX3D interaction model: obstacle / cut plane / iso / streamlines /
	 * volume compose freely because everything except the volume is opaque
	 * and z-writing. The VOLUME defaults OFF -- the research doc's demotion:
	 * neither reference draws a volume cloud by default, and ours is what
	 * made the picture read as fog.
	 */
	void SetVolumeVisible(bool bVisible);
	bool IsVolumeVisible() const { return bVolumeVisible; }
	void SetObstacleVisible(bool bVisible);
	bool IsObstacleVisible() const { return bObstacleVisible; }

	/** The drained streamlines (renderer overhaul P5), same consume contract. */
	bool HasFreshStreamlines() const { return bStreamlinesFresh; }
	const TArray<FFlowVizStreamline>& ConsumeStreamlines()
	{
		bStreamlinesFresh = false;
		return Streamlines;
	}
	void SetStreamlinesEnabled(bool bEnabled);
	bool AreStreamlinesEnabled() const { return bStreamlinesEnabled; }

	/** The P8 CPU particle population: advanced by the workspace clock, rendered as sprites. */
	void SetParticlesEnabled(bool bEnabled);
	bool AreParticlesEnabled() const { return bParticlesEnabled; }
	TArray<FlowVizParticles::FParticle>& GetParticles() { return Particles; }

	/** The displayed frame's velocity sampler/mask, shared from the sampling worker. Null before the first drain. */
	TSharedPtr<const FFlowVizFieldSampler, ESPMode::ThreadSafe> GetDisplayedVelocitySampler() const
	{
		return DisplayedVelocitySampler;
	}
	TSharedPtr<const FFlowVizFieldMask, ESPMode::ThreadSafe> GetDisplayedVelocityMask() const
	{
		return DisplayedVelocityMask;
	}

	/** The iso toggle and value (0 = the P90 default). Both re-request on change. */
	void SetIsoSurfaceEnabled(bool bEnabled);
	bool IsIsoSurfaceEnabled() const { return bIsoSurfaceEnabled; }
	void SetIsoValue(double InIsoValue);
	double GetIsoValue() const { return IsoValue; }

private:
	/** Shared with the sampling task; outlives this model if a task is still running. */
	struct FSampleQueue;

	TSharedPtr<FSampleQueue, ESPMode::ThreadSafe> SampleQueue;

	FFlowVizChartSeries LineSeries;

	FFlowVizMeshPayload CutPlanePayload;
	double CutPlaneRangeMin = 0.0;
	double CutPlaneRangeMax = 0.0;
	bool bCutPlaneFresh = false;

	bool bVolumeVisible = false;
	bool bObstacleVisible = true;

	TArray<FFlowVizStreamline> Streamlines;
	bool bStreamlinesFresh = false;
	bool bStreamlinesEnabled = true;

	TArray<FlowVizParticles::FParticle> Particles;
	bool bParticlesEnabled = false;
	TSharedPtr<const FFlowVizFieldSampler, ESPMode::ThreadSafe> DisplayedVelocitySampler;
	TSharedPtr<const FFlowVizFieldMask, ESPMode::ThreadSafe> DisplayedVelocityMask;

	FFlowVizMeshPayload IsoSurfacePayload;
	double LastIsoValueUsed = 0.0;
	bool bIsoSurfaceFresh = false;
	bool bIsoSurfaceEnabled = true;
	double IsoValue = 0.0;

	bool bPresentationMode = false;

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
