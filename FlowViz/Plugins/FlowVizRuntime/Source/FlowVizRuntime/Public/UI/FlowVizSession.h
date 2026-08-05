// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizTypes.h"
#include "Playback/FlowVizCasePlayer.h"
#include "UI/FlowVizClipViewModel.h"
#include "UI/FlowVizProbeViewModel.h"
#include "UI/FlowVizSliceViewModel.h"
#include "UI/FlowVizTransferFunctionViewModel.h"

/**
 * `.cfdvizsession` - the view-model state, written to JSON (plan.md section 14).
 *
 * NOT A NEW FILE FORMAT. plan.md section 14 specifies this one; the CFDViz CASE
 * format (Docs/CFDVIZ_FORMAT.md) is a different, settled thing and nothing here
 * touches it. A session REFERENCES a case by path and stores none of its data.
 *
 * WHY THE PUBLIC API IS FString AND NOT FJsonObject. Json and JsonUtilities are
 * PRIVATE dependencies of this module (FlowVizRuntime.Build.cs), so a public
 * header that named an FJsonObject would not compile for a downstream module.
 * The serialisation is therefore declared in terms of text and FCFDVizResult,
 * with every JSON type confined to the .cpp. This is a constraint, not a
 * preference.
 *
 * THE CASE PATH IS RELATIVE WHEN IT CAN BE. plan.md section 14: "Use relative
 * case paths when possible" and "Loading a session with a missing case must
 * allow the user to relink". Both are here: SaveToString takes the directory the
 * session will be written to and stores a relative path when the case is
 * reachable from it, and LoadFromString NEVER fails because the case is missing.
 * It reports the resolved path and a bCaseFound flag, so the workspace can offer
 * a relink dialog. A load that failed on a missing case would make a session
 * unopenable on any machine with a different directory layout, which is the same
 * defect as storing an absolute path.
 *
 * ROUND-TRIPPING IS NOT THE TEST THAT MATTERS. A save/load pair that agree with
 * each other agree just as happily on a field neither writes. The tests
 * therefore assert against the JSON TEXT for the fields plan.md names, and
 * check that a value changed before saving comes back changed - not merely that
 * two view models compare equal.
 *
 * FORWARD COMPATIBILITY. An unknown key is ignored, not rejected; a session
 * written by a newer minor version still opens. A newer MAJOR version is
 * refused, because a major bump means a key changed meaning and reading it
 * anyway restores a plausible wrong scene.
 */

namespace FlowVizSession
{
	/** The `format` value written into every session file. */
	FLOWVIZRUNTIME_API const TCHAR* GetFormatName();

	/** Major version. A file with a different major is refused. */
	inline constexpr int32 FormatVersionMajor = 1;

	/** Minor version. A file with a newer minor is accepted and its unknown keys ignored. */
	inline constexpr int32 FormatVersionMinor = 0;

	/** The extension, without the dot. */
	FLOWVIZRUNTIME_API const TCHAR* GetFileExtension();
}

/**
 * Everything a session persists, as plain values.
 *
 * A struct rather than a bag of pointers to live view models, so a session can
 * be read, inspected and relinked BEFORE anything is bound to a player - which
 * is what makes "the case is missing, offer a relink" expressible at all.
 */
struct FFlowVizSessionState
{
	/* --- Case ------------------------------------------------------------- */

	/** Path to the case's manifest.json, as stored in the file: relative to the session when possible. */
	FString CasePath;

	/** Resolved against the session's own directory at load time. Empty when CasePath is empty. */
	FString ResolvedCasePath;

	/** False when ResolvedCasePath does not exist. NOT an error - it is what a relink prompt is keyed on. */
	bool bCaseFound = false;

	/** Field bound for display. */
	FName FieldId;

	/* --- Playback (plan.md section 14 "current physical time", "playback settings") --- */

	double PhysicalTime = 0.0;
	FFlowVizPlaybackSettings Playback;

	/* --- Colouring -------------------------------------------------------- */

	ECFDVizColorMap ColorMap = CFDViz::ColorMaps::Default;
	bool bReverseColorMap = false;
	int32 ColorBands = 0;
	EFlowVizComponentChoice Component = EFlowVizComponentChoice::Magnitude;
	EFlowVizRangeSource RangeSource = EFlowVizRangeSource::Global;
	float RangeMin = 0.0f;
	float RangeMax = 1.0f;
	FFlowVizOpacityCurve Opacity;

	/* --- Clipping and slicing --------------------------------------------- */

	/** Solver units, local space - the same convention the ray-marcher reads. */
	TArray<FFlowVizClipPlane> ClipPlanes;
	FVector CropMin = FVector::ZeroVector;
	FVector CropMax = FVector::ZeroVector;
	bool bHasCropBox = false;

	FVector SliceOrigin = FVector::ZeroVector;
	FVector SliceNormal = FVector(0.0, 0.0, 1.0);
	double SliceThickness = 0.0;
	int32 SliceSlabSamples = 1;
	EFlowVizSlabOp SliceSlabOp = EFlowVizSlabOp::None;
	bool bSliceVisible = true;
	bool bHasSlice = false;

	/* --- Probes ----------------------------------------------------------- */

	/** Positions in SOLVER units, never centimetres - a session opened against a differently scaled case must still probe the same cell. */
	TArray<FFlowVizProbe> Probes;

	bool bHasLineProbe = false;
	FVector LineStart = FVector::ZeroVector;
	FVector LineEnd = FVector::ZeroVector;
	int32 LineSamples = 32;

	/* --- Workspace -------------------------------------------------------- */

	/** plan.md section 14 "scientific/presentation mode". Scientific is the default: it is the mode whose fidelity is stated. */
	bool bPresentationMode = false;

	/** plan.md section 14 "camera". Identity when never set. */
	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	bool bHasCamera = false;

	/** plan.md section 14 "UI panel visibility", by panel name. */
	TMap<FString, bool> PanelVisibility;

	/** plan.md section 14 "annotations". Free text, preserved verbatim. */
	TArray<FString> Annotations;
};

namespace FlowVizSession
{
	/**
	 * Serialise to `.cfdvizsession` JSON.
	 *
	 * @param SessionDirectory Directory the file will be written to. When
	 *        non-empty and the case is reachable from it, CasePath is stored
	 *        RELATIVE to it; otherwise absolute. Pass empty to force absolute.
	 * @param OutJson Overwritten. Untouched on failure.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult SaveToString(
		const FFlowVizSessionState& State,
		const FString& SessionDirectory,
		FString& OutJson);

	/**
	 * Parse a `.cfdvizsession`.
	 *
	 * A MISSING CASE IS NOT A FAILURE. OutState.bCaseFound is false and
	 * ResolvedCasePath names what was looked for; everything else still loads, so
	 * the workspace can relink and keep the rest of the scene. Malformed JSON, a
	 * wrong `format`, or a newer MAJOR version ARE failures.
	 *
	 * @param SessionDirectory Directory the file came from, used to resolve a
	 *        relative CasePath. Pass empty when the session came from memory.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult LoadFromString(
		const FString& Json,
		const FString& SessionDirectory,
		FFlowVizSessionState& OutState);

	/** Write to disk. The directory used for relative-path resolution is the file's own. */
	FLOWVIZRUNTIME_API FCFDVizResult SaveToFile(const FFlowVizSessionState& State, const FString& FilePath);

	/** Read from disk. Fails with FileNotFound when the session itself is missing - which is different from its case being missing. */
	FLOWVIZRUNTIME_API FCFDVizResult LoadFromFile(const FString& FilePath, FFlowVizSessionState& OutState);

	/**
	 * Point a loaded session at a case the user picked (plan.md section 14
	 * "must allow the user to relink").
	 *
	 * @return A failure when the path does not exist. State is untouched then, so
	 *         a mistaken relink does not lose the original path.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult RelinkCase(FFlowVizSessionState& State, const FString& NewManifestPath);

	/* --- Capture and apply ------------------------------------------------ */

	/**
	 * Read the live view models into a session state.
	 *
	 * Every argument is optional: a workspace with no slice passes nullptr and
	 * the corresponding bHas* flag stays false, rather than writing a default
	 * slice that would materialise out of nowhere on reload.
	 */
	FLOWVIZRUNTIME_API void CaptureFromViewModels(
		const FFlowVizCasePlayer* Player,
		const FFlowVizTransferFunctionViewModel* TransferFunction,
		const FFlowVizClipViewModel* Clip,
		const FFlowVizSliceViewModel* Slice,
		const FFlowVizProbeViewModel* Probes,
		FFlowVizSessionState& OutState);

	/**
	 * Push a loaded session back into live view models.
	 *
	 * Applied best-effort per view model: a session whose clip planes are legal
	 * but whose playback speed is not restores the planes and reports the speed
	 * failure, rather than discarding the whole scene over one bad number.
	 *
	 * @return The FIRST failure encountered, after applying everything it could.
	 *         Ok when everything applied.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult ApplyToViewModels(
		const FFlowVizSessionState& State,
		FFlowVizCasePlayer* Player,
		FFlowVizTransferFunctionViewModel* TransferFunction,
		FFlowVizClipViewModel* Clip,
		FFlowVizSliceViewModel* Slice,
		FFlowVizProbeViewModel* Probes);
}
