// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizSliceViewModel.h"

/**
 * See FlowVizSliceViewModel.h.
 *
 * NOTHING DRAWS A SLICE YET. There is no slice component in the tree, so every
 * function here models state and legal transitions and none of it reaches a
 * pixel. MakeClipPlane is the exception worth knowing about: its consumer (the
 * ray-marcher's clip-plane convention) does exist, which is what lets the
 * orientation logic be checked against something real instead of against itself.
 */

// Named rather than anonymous: see the note in FlowVizClipViewModel.cpp. Under a
// unity build these helpers share a translation unit with the sibling view
// models, and an anonymous namespace would collide with their same-named ones.
namespace FlowVizSliceViewModelLocal
{
	FCFDVizResult MakeNoDomainResult()
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("No domain size has been set, so there is nothing to place a slice in"));
	}

	bool IsFiniteVector(const FVector& V)
	{
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
	}
}


/* ========================================================================== */
/* Domain                                                                      */
/* ========================================================================== */

FCFDVizResult FFlowVizSliceViewModel::SetDomainSize(const FVector& PhysicalSize)
{
	if (!FlowVizSliceViewModelLocal::IsFiniteVector(PhysicalSize))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A domain size must be finite on every axis"));
	}
	if (PhysicalSize.X <= 0.0 || PhysicalSize.Y <= 0.0 || PhysicalSize.Z <= 0.0)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("A domain size must be positive on every axis; got (%g, %g, %g)"),
				PhysicalSize.X, PhysicalSize.Y, PhysicalSize.Z));
	}
	DomainSize = PhysicalSize;
	bHasDomain = true;
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Plane                                                                       */
/* ========================================================================== */

FCFDVizResult FFlowVizSliceViewModel::SetOrigin(const FVector& LocalOrigin)
{
	if (!FlowVizSliceViewModelLocal::IsFiniteVector(LocalOrigin))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A slice origin must be finite"));
	}
	// NOT CLAMPED INTO THE DOMAIN. A slice outside the box samples nothing, which
	// is an empty result rather than an illegal state - and clamping would move a
	// plane the user positioned deliberately, e.g. while animating it in.
	Origin = LocalOrigin;
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizSliceViewModel::SetNormal(const FVector& InNormal)
{
	if (!FlowVizSliceViewModelLocal::IsFiniteVector(InNormal))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A slice normal must be finite"));
	}
	if (InNormal.SizeSquared() <= UE_SMALL_NUMBER)
	{
		// A zero normal has no orientation: the slice would have no defined side,
		// and SignedDistance would return the same value everywhere.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A slice normal cannot be zero-length"));
	}
	// Stored normalised, so SignedDistance is a true distance in solver units
	// rather than one scaled by |N|. A slab thickness compared against a scaled
	// distance would select the wrong depth.
	Normal = InNormal.GetSafeNormal();
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizSliceViewModel::SetAxisPreset(EFlowVizSliceAxis Axis)
{
	switch (Axis)
	{
		case EFlowVizSliceAxis::X: return SetNormal(FVector(1.0, 0.0, 0.0));
		case EFlowVizSliceAxis::Y: return SetNormal(FVector(0.0, 1.0, 0.0));
		case EFlowVizSliceAxis::Z: return SetNormal(FVector(0.0, 0.0, 1.0));
		default:
			return FCFDVizResult::Fail(
				ECFDVizError::IndexOutOfRange, TEXT("Unknown slice axis preset"));
	}
}

FCFDVizResult FFlowVizSliceViewModel::CenterOnDomain()
{
	if (!bHasDomain)
	{
		return FlowVizSliceViewModelLocal::MakeNoDomainResult();
	}
	// Local space runs [0, DomainSize] from the minimum corner, so the centre is
	// half the size regardless of where the domain sits in solver coordinates.
	Origin = DomainSize * 0.5;
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizSliceViewModel::SetNormalizedPosition(double Fraction)
{
	if (!bHasDomain)
	{
		return FlowVizSliceViewModelLocal::MakeNoDomainResult();
	}
	if (!FMath::IsFinite(Fraction))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A normalized slice position must be finite"));
	}

	/*
	 * THE PROJECTED EXTENT, NOT THE DOMINANT AXIS.
	 *
	 * The domain's extent along an arbitrary N is sum(|N.i| * Size.i) - the
	 * support width of the box in that direction. Using the dominant axis instead
	 * would make a 45-degree slice run out of travel partway across the box: the
	 * slider would reach 1.0 with the plane still inside the domain, which looks
	 * like the slice "sticking" rather than like a projection error.
	 */
	const double Extent = FMath::Abs(Normal.X) * DomainSize.X
		+ FMath::Abs(Normal.Y) * DomainSize.Y
		+ FMath::Abs(Normal.Z) * DomainSize.Z;

	if (Extent <= UE_SMALL_NUMBER)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("The domain has no extent along this slice normal"));
	}

	// The travel runs from the corner that projects LOWEST along N to the one
	// that projects highest. That low corner is not the origin for a normal with
	// negative components - for N = -Z it is the far corner - so it is computed
	// from the sign of each component rather than assumed to be zero.
	const FVector LowCorner(
		Normal.X >= 0.0 ? 0.0 : DomainSize.X,
		Normal.Y >= 0.0 ? 0.0 : DomainSize.Y,
		Normal.Z >= 0.0 ? 0.0 : DomainSize.Z);
	const double LowProjection = FVector::DotProduct(Normal, LowCorner);

	// Clamped, because this is a SLIDER: dragging past the end is an ordinary
	// interaction. SetOrigin, which is the typed path, does not clamp.
	const double Clamped = FMath::Clamp(Fraction, 0.0, 1.0);
	const double TargetProjection = LowProjection + Clamped * Extent;

	// Move the origin ALONG THE NORMAL only, so the slice's in-plane position -
	// which does not affect an infinite plane but does affect a rendered quad's
	// centre - is not silently reset by a slider drag.
	const double CurrentProjection = FVector::DotProduct(Normal, Origin);
	Origin += Normal * (TargetProjection - CurrentProjection);
	return FCFDVizResult::Ok();
}

double FFlowVizSliceViewModel::GetNormalizedPosition() const
{
	if (!bHasDomain)
	{
		return 0.0;
	}
	const double Extent = FMath::Abs(Normal.X) * DomainSize.X
		+ FMath::Abs(Normal.Y) * DomainSize.Y
		+ FMath::Abs(Normal.Z) * DomainSize.Z;
	if (Extent <= UE_SMALL_NUMBER)
	{
		return 0.0;
	}

	const FVector LowCorner(
		Normal.X >= 0.0 ? 0.0 : DomainSize.X,
		Normal.Y >= 0.0 ? 0.0 : DomainSize.Y,
		Normal.Z >= 0.0 ? 0.0 : DomainSize.Z);
	const double LowProjection = FVector::DotProduct(Normal, LowCorner);

	// NOT clamped on the way out: a slice deliberately parked outside the domain
	// should read as outside, not as pinned to an end. The setter clamps because
	// it is a slider; the getter reports the truth.
	return (FVector::DotProduct(Normal, Origin) - LowProjection) / Extent;
}

/* ========================================================================== */
/* Slab                                                                        */
/* ========================================================================== */

FCFDVizResult FFlowVizSliceViewModel::SetThickness(double InThickness)
{
	if (!FMath::IsFinite(InThickness) || InThickness < 0.0)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("A slice thickness must be finite and non-negative"));
	}

	Thickness = InThickness;

	// THE SLAB CONTROLS FOLLOW THE THICKNESS, because rule 15 is about the pair.
	// Collapsing to zero leaves an aggregation selected over nothing; growing
	// from zero leaves EFlowVizSlabOp::None on a slab, which has no defined
	// meaning - a slab must say how it collapses.
	if (Thickness <= 0.0)
	{
		SlabOp = EFlowVizSlabOp::None;
		SlabSamples = 1;
	}
	else if (SlabOp == EFlowVizSlabOp::None)
	{
		// Average is the neutral default: it is the only aggregation that does not
		// bias the result toward an extreme, so a slab that appears without the
		// user choosing an operation does not exaggerate peaks.
		SlabOp = EFlowVizSlabOp::Average;
		SlabSamples = FMath::Max(2, SlabSamples);
	}
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizSliceViewModel::SetSlabSamples(int32 Samples)
{
	if (!IsSlab())
	{
		// A sample count with no slab to sample is a control that does nothing.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("A slab sample count needs a non-zero thickness; set the thickness first"));
	}
	if (Samples < 2)
	{
		// One sample across a thickness is not a slab - it is the plane, sampled
		// once, while the UI claims an aggregation is happening.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("A slab needs at least 2 samples; got %d"), Samples));
	}
	SlabSamples = Samples;
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizSliceViewModel::SetSlabOp(EFlowVizSlabOp Op)
{
	if (Op == EFlowVizSlabOp::None)
	{
		if (IsSlab())
		{
			// A thick slice must say how it collapses. Allowing None here would
			// leave the renderer to invent one.
			return FCFDVizResult::Fail(
				ECFDVizError::IndexOutOfRange,
				TEXT("A slab with non-zero thickness needs an aggregation; set the thickness "
					 "to zero to return to a single-sample plane"));
		}
		SlabOp = Op;
		return FCFDVizResult::Ok();
	}

	if (!IsSlab())
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("An aggregation needs a non-zero thickness; set the thickness first"));
	}
	SlabOp = Op;
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Appearance                                                                  */
/* ========================================================================== */

FCFDVizResult FFlowVizSliceViewModel::SetOpacity(float InOpacity)
{
	if (!FMath::IsFinite(InOpacity) || InOpacity < 0.0f || InOpacity > 1.0f)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A slice opacity must be finite and within [0, 1]"));
	}
	Opacity = InOpacity;
	return FCFDVizResult::Ok();
}

void FFlowVizSliceViewModel::SetVisible(bool bInVisible)
{
	bVisible = bInVisible;
}

void FFlowVizSliceViewModel::SetShowWidget(bool bShow)
{
	bShowWidget = bShow;
}

void FFlowVizSliceViewModel::SetTrilinear(bool bEnable)
{
	bTrilinear = bEnable;
}

/* ========================================================================== */
/* Geometry                                                                    */
/* ========================================================================== */

double FFlowVizSliceViewModel::SignedDistance(const FVector& LocalPosition) const
{
	return FVector::DotProduct(Normal, LocalPosition - Origin);
}

FFlowVizClipPlane FFlowVizSliceViewModel::MakeClipPlane() const
{
	FFlowVizClipPlane Plane;
	Plane.Normal = Normal;
	// dot(N, P) + D >= 0 with D = -dot(N, Origin) is exactly dot(N, P - Origin) >= 0,
	// which is SignedDistance >= 0. The two are the same expression rearranged,
	// so the slice's notion of "which side" and the shader's cannot disagree.
	Plane.Distance = -FVector::DotProduct(Normal, Origin);
	Plane.Label = TEXT("Slice");
	Plane.bEnabled = true;
	return Plane;
}
