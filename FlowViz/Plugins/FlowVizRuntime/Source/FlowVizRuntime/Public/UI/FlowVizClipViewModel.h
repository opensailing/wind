// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizTypes.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Render/FlowVizVolumeTexture.h"

/**
 * Clip planes and the crop box (plan.md sections 10.5 and 5F).
 *
 * THE CONVENTION IS NOT NEGOTIABLE AND IS NOT REDEFINED HERE. The ray-marcher
 * already fixes it, in FlowVizVolumeRayMarchShader.h:
 *
 *     dot(N, LocalPos) + D >= 0 is KEPT.  Local space, solver units.
 *
 * "Local space" is [0, PhysicalSize] with the domain's minimum corner at the
 * origin - the space FFlowVizVolumeTransform::GetLocalToUnrealTransform maps to
 * the world. So a plane authored here is in SOLVER units and SOLVER axes, and
 * the Y-mirror of ADR 004 has not been applied to it. Writing a second
 * convention - keep-below, world space, centimetres, or a normal that has been
 * through SolverToUnrealDirection - produces a render that clips a plausible
 * wrong half of the domain, which reads as a data problem rather than a sign
 * error.
 *
 * INVERT IS A NEGATION OF THE WHOLE PLANE, NOT OF THE NORMAL ALONE. Keeping the
 * other side of dot(N,P) + D >= 0 means testing dot(-N,P) - D >= 0, so D flips
 * too. Negating only N moves the plane to the mirror image of its position
 * through the origin, which for a domain whose origin is a corner is a plane
 * outside the domain entirely - the volume then either vanishes or is untouched,
 * and both look like the feature is broken rather than like a sign bug.
 *
 * THE SIX-PLANE LIMIT IS THE SHADER'S. FlowVizRayMarch::MaxClipPlanes is 6 and
 * the parameter array is sized by it, so a seventh plane is REFUSED here rather
 * than accepted and dropped at bind time - a dropped plane is a control that
 * appears to work and does nothing (rule 15).
 *
 * THE CROP BOX IS NORMALISED, THE PLANES ARE NOT. FFlowVizVolumeRayMarchParameters
 * carries CropBoxMin/Max as fractions of PhysicalSize "so a crop survives a
 * spacing change", while ClipPlanes are in solver units. Two different spaces in
 * one parameter block is a standing invitation to mix them up, so this class
 * takes the crop in SOLVER units - the same units the user sees on a numeric
 * transform panel - and normalises on the way out, in one place.
 *
 * THREADING. Game thread, pure value state, no RHI, no case reference.
 */

/** One clip plane in the shader's own convention. */
struct FFlowVizClipPlane
{
	/**
	 * Plane normal in SOLVER axes, local space. Need not be unit length on the
	 * way in - Normalize() is applied when the plane is added, because an
	 * unnormalised N scales D's meaning and makes the numeric readout lie about
	 * where the plane sits.
	 */
	FVector Normal = FVector(1.0, 0.0, 0.0);

	/** Plane offset, solver units: the set kept is dot(Normal, LocalPos) + Distance >= 0. */
	double Distance = 0.0;

	/** Shown in the UI so a user can tell two planes apart. Not used by the shader. */
	FString Label;

	/** A disabled plane is retained in the list and excluded from the shader - a show/hide toggle, not a delete. */
	bool bEnabled = true;

	/** True when Normal is finite and non-degenerate and Distance is finite. */
	FLOWVIZRUNTIME_API bool IsValid() const;

	/** dot(N, LocalPos) + D. Positive is kept. The CPU twin of the shader's test, used by the probe view model and by tests. */
	FLOWVIZRUNTIME_API double SignedDistance(const FVector& LocalPosition) const;

	/** True when this plane keeps LocalPosition. A point exactly on the plane is KEPT, matching the shader's >= 0. */
	bool Keeps(const FVector& LocalPosition) const { return SignedDistance(LocalPosition) >= 0.0; }
};

/** Where a plane preset faces. Named rather than a raw axis index so a call site reads as a direction. */
enum class EFlowVizClipPreset : uint8
{
	KeepPlusX = 0,
	KeepMinusX = 1,
	KeepPlusY = 2,
	KeepMinusY = 3,
	KeepPlusZ = 4,
	KeepMinusZ = 5,
};

class FLOWVIZRUNTIME_API FFlowVizClipViewModel
{
public:
	FFlowVizClipViewModel() = default;

	/* --- Domain ----------------------------------------------------------- */

	/**
	 * Tell the model how big the domain is, in SOLVER units, so presets and
	 * "reset to bounds" have something to reset to.
	 *
	 * @param PhysicalSize FFlowVizVolumeTransform::GetPhysicalSize(). A
	 *                     non-positive or non-finite axis is refused: a zero-size
	 *                     domain would make every normalised crop fraction a
	 *                     division by zero.
	 */
	FCFDVizResult SetDomainSize(const FVector& PhysicalSize);
	const FVector& GetDomainSize() const { return DomainSize; }
	bool HasDomain() const { return bHasDomain; }

	/* --- Planes ----------------------------------------------------------- */

	/** @return A failure when the plane is degenerate, or when MaxClipPlanes are already present. Nothing is added on failure. */
	FCFDVizResult AddPlane(const FFlowVizClipPlane& Plane);

	/**
	 * Add an axis-aligned plane through the domain's centre.
	 *
	 * Requires a domain: without one there is no centre, and a preset that
	 * silently used the origin would clip the whole volume away on a domain whose
	 * minimum corner is not at zero.
	 */
	FCFDVizResult AddPresetPlane(EFlowVizClipPreset Preset);

	FCFDVizResult RemovePlane(int32 Index);
	void RemoveAllPlanes();

	FCFDVizResult SetPlane(int32 Index, const FFlowVizClipPlane& Plane);
	FCFDVizResult SetPlaneEnabled(int32 Index, bool bEnabled);

	/** Keep the other side: N -> -N AND D -> -D. See the file comment on why both. */
	FCFDVizResult InvertPlane(int32 Index);

	int32 GetPlaneCount() const { return Planes.Num(); }
	int32 GetEnabledPlaneCount() const;
	const TArray<FFlowVizClipPlane>& GetPlanes() const { return Planes; }
	const FFlowVizClipPlane* FindPlane(int32 Index) const;

	/** True when another plane would fit - what greys out an "add plane" button (rule 15). */
	bool CanAddPlane() const { return Planes.Num() < FlowVizRayMarch::MaxClipPlanes; }

	/** True when every enabled plane keeps this local-space, solver-unit point. */
	bool KeepsPoint(const FVector& LocalPosition) const;

	/* --- Crop box --------------------------------------------------------- */

	/**
	 * Axis-aligned crop, in SOLVER units relative to the domain's minimum corner
	 * (that is, in local space).
	 *
	 * @return A failure for a non-finite or inverted box, or when no domain is
	 *         set. The box is NOT clamped into the domain: a crop wider than the
	 *         domain is meaningful (it selects everything) and clamping it would
	 *         silently rewrite what the user typed.
	 */
	FCFDVizResult SetCropBox(const FVector& LocalMin, const FVector& LocalMax);

	/** Back to the whole domain. */
	void ResetCropBox();

	const FVector& GetCropMin() const { return CropMin; }
	const FVector& GetCropMax() const { return CropMax; }

	/** True when the crop is anything other than the full domain. */
	bool IsCropActive() const;

	/* --- What the render layer consumes ----------------------------------- */

	/**
	 * Write ClipPlanes, NumClipPlanes, CropBoxMin and CropBoxMax into the
	 * ray-march parameter block.
	 *
	 * This is the only place the normalisation from solver units to crop
	 * fractions happens, and the only place the plane list is packed. It writes
	 * ONLY those four members, so it composes with FillDefaults and
	 * FillFromVolumeParameters in any order.
	 *
	 * Disabled planes are omitted, and the unused tail of the array is zeroed -
	 * a stale plane left in slot 3 while NumClipPlanes says 2 is invisible until
	 * someone raises the count.
	 *
	 * @return A failure, leaving OutParameters untouched, when a plane is
	 *         degenerate or no domain is set.
	 */
	FCFDVizResult ApplyToRayMarchParameters(FFlowVizVolumeRayMarchParameters& OutParameters) const;

private:
	TArray<FFlowVizClipPlane> Planes;

	bool bHasDomain = false;
	FVector DomainSize = FVector::OneVector;

	FVector CropMin = FVector::ZeroVector;
	FVector CropMax = FVector::OneVector;
};
