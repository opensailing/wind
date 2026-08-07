// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizColorMaps.h"
#include "CFDViz/CFDVizTypes.h"
#include "UI/FlowVizClipViewModel.h"

/**
 * A slice plane through the domain (plan.md sections 10.4 and 5F).
 *
 * SAME SPACE AS THE CLIP PLANES, DELIBERATELY. Origin and Normal are in LOCAL
 * space - [0, PhysicalSize], solver units, minimum corner at the origin - which
 * is what FFlowVizClipPlane uses and what the ray-marcher reads. A slice
 * authored in Unreal centimetres would be off by a factor of 100 and mirrored in
 * Y, and would still draw a plausible plane through the middle of something.
 *
 * A SLICE IS NOT A CLIP PLANE EVEN THOUGH BOTH ARE PLANES. A clip plane throws
 * half the domain away; a slice samples a surface. They are separate types here
 * because plan.md gives them separate control sets and because a slab (thickness
 * > 0 with an aggregation) has no clip-plane equivalent. What they share -
 * the plane convention - is shared by construction: MakeClipPlane() converts,
 * so there is one definition of "which side is which".
 *
 * THE RENDERER CONSUMES THIS AS A SLAB OF THE VOLUME (#77). There is still no
 * UCFDVizSliceComponent: a slab is two opposed half-spaces, and the clip
 * machinery already renders half-spaces end to end, so SFlowVizWorkspace
 * composes MakeSlabPlanes() into the PUSHED clip model -- one definition of
 * "which side", one shader path, no new parameters. The composition happens at
 * push time, never inside the clip view model the user edits, so the clip
 * panel shows no phantom rows and hiding the slice retracts exactly its own
 * two planes.
 *
 * THREADING. Game thread, pure value state.
 */

/** How a slab of finite thickness collapses to one value per sample (plan.md 10.4). */
enum class EFlowVizSlabOp : uint8
{
	/** Thickness 0: a single sample on the plane. The slab controls are inert and report themselves so. */
	None = 0,
	Average = 1,
	Minimum = 2,
	Maximum = 3,
};

/** Axis presets for the slice normal (plan.md 10.4 "X/Y/Z presets"). */
enum class EFlowVizSliceAxis : uint8
{
	X = 0,
	Y = 1,
	Z = 2,
};

class FLOWVIZRUNTIME_API FFlowVizSliceViewModel
{
public:
	FFlowVizSliceViewModel() = default;

	/* --- Domain ----------------------------------------------------------- */

	/** @param PhysicalSize FFlowVizVolumeTransform::GetPhysicalSize(), solver units. Non-positive axes are refused. */
	FCFDVizResult SetDomainSize(const FVector& PhysicalSize);
	const FVector& GetDomainSize() const { return DomainSize; }
	bool HasDomain() const { return bHasDomain; }

	/* --- Plane ------------------------------------------------------------ */

	/** @return A failure - and no change - for a non-finite origin. The origin is NOT clamped into the domain; an outside slice is empty, not illegal. */
	FCFDVizResult SetOrigin(const FVector& LocalOrigin);
	const FVector& GetOrigin() const { return Origin; }

	/** @return A failure for a zero-length or non-finite normal. Stored normalised. */
	FCFDVizResult SetNormal(const FVector& InNormal);
	const FVector& GetNormal() const { return Normal; }

	/** Axis-aligned normal, origin left where it is. Use CenterOnDomain to also recentre. */
	FCFDVizResult SetAxisPreset(EFlowVizSliceAxis Axis);

	/** Move the origin to the domain's centre, leaving the normal alone. Requires a domain. */
	FCFDVizResult CenterOnDomain();

	/**
	 * Position along the normal as a fraction of the domain's extent in that
	 * direction, which is what a slice slider is calibrated in.
	 *
	 * Uses the domain's projected extent along N - the sum of |N.i| * Size.i -
	 * so an oblique normal is handled correctly rather than by picking the
	 * dominant axis, which would make a 45-degree slice run out of travel
	 * partway across the box.
	 */
	FCFDVizResult SetNormalizedPosition(double Fraction);
	double GetNormalizedPosition() const;

	/* --- Slab ------------------------------------------------------------- */

	/** @param Thickness Solver units, >= 0. Zero means a single-sample plane. Negative or non-finite is refused. */
	FCFDVizResult SetThickness(double Thickness);
	double GetThickness() const { return Thickness; }

	/** @param Samples >= 1. Refused for a zero-thickness slice: a slab sample count with no slab is a control that does nothing (rule 15). */
	FCFDVizResult SetSlabSamples(int32 Samples);
	int32 GetSlabSamples() const { return SlabSamples; }

	/** @return A failure when setting an aggregation on a zero-thickness slice, and when clearing it to None on a thick one. */
	FCFDVizResult SetSlabOp(EFlowVizSlabOp Op);
	EFlowVizSlabOp GetSlabOp() const { return SlabOp; }

	/** True when Thickness > 0, which is what enables the slab controls. */
	bool IsSlab() const { return Thickness > 0.0; }

	/* --- Appearance ------------------------------------------------------- */

	FCFDVizResult SetOpacity(float InOpacity);
	float GetOpacity() const { return Opacity; }

	void SetVisible(bool bInVisible);
	bool IsVisible() const { return bVisible; }

	/*
	 * TWO CONSUMERS, TWO SWITCHES (renderer overhaul P3). bVisible now means
	 * "the plane is on screen" -- as the opaque CUT PLANE MESH, the genre's
	 * default picture. Slabbing the VOLUME to the plane is a second, opt-in
	 * consumer: composed only when this flag is also up, because a
	 * default-visible slice that always slabbed would sliver the volume on
	 * every open (the hazard that used to force the slice to default hidden).
	 */
	void SetVolumeSlabEnabled(bool bInEnabled) { bVolumeSlab = bInEnabled; }
	bool IsVolumeSlabEnabled() const { return bVolumeSlab; }

	/** Show the manipulation widget. Separate from IsVisible: a hidden gizmo over a visible slice is a legitimate presentation state. */
	void SetShowWidget(bool bShow);
	bool IsWidgetShown() const { return bShowWidget; }

	/** Trilinear sampling versus nearest (plan.md 10.4). */
	void SetTrilinear(bool bEnable);
	bool IsTrilinear() const { return bTrilinear; }

	/* --- Geometry --------------------------------------------------------- */

	/** Signed distance from the slice plane: dot(N, P - Origin). Zero on the plane, positive on the normal's side. */
	double SignedDistance(const FVector& LocalPosition) const;

	/**
	 * This slice expressed as a clip plane in the ray-marcher's convention,
	 * keeping the half-space the normal points into.
	 *
	 * dot(N, P) + D >= 0 with D = -dot(N, Origin), so a point ON the plane is
	 * kept - matching the shader's >=. This is what makes the slice's orientation
	 * checkable against a consumer that actually exists.
	 */
	FFlowVizClipPlane MakeClipPlane() const;

	/**
	 * The slab as two opposed clip planes: keep >= (slice - t/2) AND
	 * keep <= (slice + t/2). Appended, so a caller composes onto an existing
	 * plane list.
	 *
	 * A LITERAL ZERO-THICKNESS SLAB IS NEVER EMITTED. The ray-marcher
	 * integrates a 3D field, and two coincident opposed planes keep a
	 * measure-zero set -- a "slice" that renders as nothing, indistinguishable
	 * from a slice outside the domain. So thickness 0 (a plane, per the panel)
	 * renders as the MINIMUM slab: MinSlabFraction of the domain's extent along
	 * the normal. The panel's own thickness readout still says 0; the disclosure
	 * is this comment plus the fraction being a named constant rather than a
	 * magic number.
	 *
	 * @return The number of planes appended: 2, or 0 with no domain.
	 */
	int32 MakeSlabPlanes(TArray<FFlowVizClipPlane>& OutPlanes) const;

	/** The minimum rendered slab, as a fraction of the domain extent along the normal. */
	static constexpr double MinSlabFraction = 0.02;

private:
	bool bHasDomain = false;
	FVector DomainSize = FVector::OneVector;

	FVector Origin = FVector::ZeroVector;
	FVector Normal = FVector(0.0, 0.0, 1.0);

	double Thickness = 0.0;
	int32 SlabSamples = 1;
	EFlowVizSlabOp SlabOp = EFlowVizSlabOp::None;

	float Opacity = 1.0f;
	/*
	 * HIDDEN BY DEFAULT, and this flipped when the slice gained a renderer
	 * (#77). While nothing consumed the slice, "visible" was aspiration and
	 * true was harmless. Now visible means "clip the volume to the slab" -- a
	 * default-visible slice would slab every volume to a sliver the moment a
	 * case opened, before the user touched anything.
	 */
	bool bVisible = false;
	bool bVolumeSlab = false;
	bool bShowWidget = true;
	bool bTrilinear = true;
};
