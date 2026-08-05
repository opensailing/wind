// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizClipViewModel.h"

/**
 * See FlowVizClipViewModel.h for the convention. The single rule this file
 * exists to keep is:
 *
 *     dot(N, LocalPos) + D >= 0 is KEPT.  Local space, solver units.
 *
 * It is the shader's, not this class's. Every function below either produces a
 * plane in that convention or evaluates that exact expression - there is no
 * second test anywhere, because a keep-below or a world-space variant would clip
 * a plausible wrong half of the domain and read as a data problem.
 */

// NAMED, not anonymous. Unreal compiles this module as a unity build, which
// concatenates several .cpp files into one translation unit. Two anonymous
// namespaces in the same TU are the SAME namespace, so identically named
// helpers in sibling view models are a redefinition error rather than two
// private helpers. A file-unique namespace name keeps them apart no matter how
// UBT chunks the module.
namespace FlowVizClipViewModelLocal
{
	FCFDVizResult MakeNoDomainResult()
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("No domain size has been set, so there is nothing to place a plane or a crop in"));
	}

	FCFDVizResult MakeBadIndexResult(int32 Index, int32 Count)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("Clip plane %d is outside the %d planes present"), Index, Count));
	}

	bool IsFiniteVector(const FVector& V)
	{
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
	}
}


/* ========================================================================== */
/* FFlowVizClipPlane                                                           */
/* ========================================================================== */

bool FFlowVizClipPlane::IsValid() const
{
	if (!FlowVizClipViewModelLocal::IsFiniteVector(Normal) || !FMath::IsFinite(Distance))
	{
		return false;
	}
	// A zero normal makes dot(N,P) + D constant, so the plane keeps either
	// everything or nothing depending only on D's sign - a "clip plane" that is
	// not a plane. SMALL_NUMBER rather than exactly zero because a normal that
	// short cannot be normalised without the result being noise.
	return Normal.SizeSquared() > UE_SMALL_NUMBER;
}

double FFlowVizClipPlane::SignedDistance(const FVector& LocalPosition) const
{
	// THE SHADER'S EXPRESSION, VERBATIM. This is the CPU twin used by the probe
	// view model and by tests; if the two ever disagree, a probe reports a point
	// as kept that the render clipped away.
	return FVector::DotProduct(Normal, LocalPosition) + Distance;
}

/* ========================================================================== */
/* Domain                                                                      */
/* ========================================================================== */

FCFDVizResult FFlowVizClipViewModel::SetDomainSize(const FVector& PhysicalSize)
{
	if (!FlowVizClipViewModelLocal::IsFiniteVector(PhysicalSize))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A domain size must be finite on every axis"));
	}
	if (PhysicalSize.X <= 0.0 || PhysicalSize.Y <= 0.0 || PhysicalSize.Z <= 0.0)
	{
		// A zero-size axis divides by zero when the crop is normalised, and a
		// negative one inverts the crop fractions - which would render as a crop
		// box that grows when dragged smaller.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("A domain size must be positive on every axis; got (%g, %g, %g)"),
				PhysicalSize.X, PhysicalSize.Y, PhysicalSize.Z));
	}

	DomainSize = PhysicalSize;
	bHasDomain = true;

	// The crop follows the new domain rather than being kept in the old units. A
	// crop authored for a 10 m domain means something entirely different in a
	// 0.1 m one, and keeping the numbers would silently re-scope the selection.
	ResetCropBox();
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Planes                                                                      */
/* ========================================================================== */

FCFDVizResult FFlowVizClipViewModel::AddPlane(const FFlowVizClipPlane& Plane)
{
	if (!CanAddPlane())
	{
		// REFUSED, NOT DROPPED. The parameter array is sized by MaxClipPlanes, so
		// a seventh plane cannot reach the shader. Accepting it here and losing it
		// at bind time is a control that appears to work and does nothing.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("The renderer supports %d clip planes and %d are already present"),
				FlowVizRayMarch::MaxClipPlanes, Planes.Num()));
	}
	if (!Plane.IsValid())
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("A clip plane needs a finite, non-degenerate normal and a finite distance"));
	}

	FFlowVizClipPlane Added = Plane;
	// NORMALISED ON THE WAY IN, ONCE. An unnormalised N scales what D means, so
	// the numeric readout would claim a plane sits somewhere it does not, and two
	// planes with the same D would sit at different depths.
	Added.Normal = Plane.Normal.GetSafeNormal();
	Planes.Add(MoveTemp(Added));
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizClipViewModel::AddPresetPlane(EFlowVizClipPreset Preset)
{
	if (!bHasDomain)
	{
		// A preset is "through the centre", and without a domain there is no
		// centre. Falling back to the origin would put the plane on the domain's
		// minimum CORNER - clipping the entire volume away, which looks like the
		// feature is broken rather than like a missing domain.
		return FlowVizClipViewModelLocal::MakeNoDomainResult();
	}

	// Local space runs [0, DomainSize] with the minimum corner at the origin, so
	// the centre is half the size.
	const FVector Centre = DomainSize * 0.5;

	FFlowVizClipPlane Plane;
	switch (Preset)
	{
		case EFlowVizClipPreset::KeepPlusX:
			Plane.Normal = FVector(1.0, 0.0, 0.0);
			Plane.Label = TEXT("Keep +X");
			break;
		case EFlowVizClipPreset::KeepMinusX:
			Plane.Normal = FVector(-1.0, 0.0, 0.0);
			Plane.Label = TEXT("Keep -X");
			break;
		case EFlowVizClipPreset::KeepPlusY:
			Plane.Normal = FVector(0.0, 1.0, 0.0);
			Plane.Label = TEXT("Keep +Y");
			break;
		case EFlowVizClipPreset::KeepMinusY:
			Plane.Normal = FVector(0.0, -1.0, 0.0);
			Plane.Label = TEXT("Keep -Y");
			break;
		case EFlowVizClipPreset::KeepPlusZ:
			Plane.Normal = FVector(0.0, 0.0, 1.0);
			Plane.Label = TEXT("Keep +Z");
			break;
		case EFlowVizClipPreset::KeepMinusZ:
			Plane.Normal = FVector(0.0, 0.0, -1.0);
			Plane.Label = TEXT("Keep -Z");
			break;
		default:
			return FCFDVizResult::Fail(
				ECFDVizError::IndexOutOfRange, TEXT("Unknown clip plane preset"));
	}

	// The plane passes through Centre, so dot(N, Centre) + D == 0, hence
	// D = -dot(N, Centre). Derived rather than special-cased per axis: a per-axis
	// sign table is six chances to get one sign wrong.
	Plane.Distance = -FVector::DotProduct(Plane.Normal, Centre);
	return AddPlane(Plane);
}

FCFDVizResult FFlowVizClipViewModel::RemovePlane(int32 Index)
{
	if (!Planes.IsValidIndex(Index))
	{
		return FlowVizClipViewModelLocal::MakeBadIndexResult(Index, Planes.Num());
	}
	Planes.RemoveAt(Index);
	return FCFDVizResult::Ok();
}

void FFlowVizClipViewModel::RemoveAllPlanes()
{
	Planes.Reset();
}

FCFDVizResult FFlowVizClipViewModel::SetPlane(int32 Index, const FFlowVizClipPlane& Plane)
{
	if (!Planes.IsValidIndex(Index))
	{
		return FlowVizClipViewModelLocal::MakeBadIndexResult(Index, Planes.Num());
	}
	if (!Plane.IsValid())
	{
		// Validated BEFORE assignment, so a rejected edit leaves the previous
		// plane intact rather than a degenerate one the render would refuse.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("A clip plane needs a finite, non-degenerate normal and a finite distance"));
	}

	FFlowVizClipPlane Updated = Plane;
	Updated.Normal = Plane.Normal.GetSafeNormal();
	Planes[Index] = MoveTemp(Updated);
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizClipViewModel::SetPlaneEnabled(int32 Index, bool bEnabled)
{
	if (!Planes.IsValidIndex(Index))
	{
		return FlowVizClipViewModelLocal::MakeBadIndexResult(Index, Planes.Num());
	}
	Planes[Index].bEnabled = bEnabled;
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizClipViewModel::InvertPlane(int32 Index)
{
	if (!Planes.IsValidIndex(Index))
	{
		return FlowVizClipViewModelLocal::MakeBadIndexResult(Index, Planes.Num());
	}

	// BOTH, NOT JUST THE NORMAL. Keeping the other side of dot(N,P) + D >= 0
	// means testing dot(-N,P) - D >= 0. Negating N alone reflects the plane
	// through the origin, and since local space puts the domain's minimum corner
	// AT the origin, the reflected plane lands outside the domain entirely: the
	// volume then either vanishes or is untouched, and both read as a broken
	// feature rather than a sign error.
	Planes[Index].Normal = -Planes[Index].Normal;
	Planes[Index].Distance = -Planes[Index].Distance;
	return FCFDVizResult::Ok();
}

int32 FFlowVizClipViewModel::GetEnabledPlaneCount() const
{
	int32 Count = 0;
	for (const FFlowVizClipPlane& Plane : Planes)
	{
		if (Plane.bEnabled)
		{
			++Count;
		}
	}
	return Count;
}

const FFlowVizClipPlane* FFlowVizClipViewModel::FindPlane(int32 Index) const
{
	return Planes.IsValidIndex(Index) ? &Planes[Index] : nullptr;
}

bool FFlowVizClipViewModel::KeepsPoint(const FVector& LocalPosition) const
{
	// EVERY enabled plane must keep it - the planes intersect, they do not union.
	// A union would make each added plane reveal more of the volume, which is the
	// opposite of what a clip does.
	for (const FFlowVizClipPlane& Plane : Planes)
	{
		if (Plane.bEnabled && !Plane.Keeps(LocalPosition))
		{
			return false;
		}
	}
	return true;
}

/* ========================================================================== */
/* Crop box                                                                    */
/* ========================================================================== */

FCFDVizResult FFlowVizClipViewModel::SetCropBox(const FVector& LocalMin, const FVector& LocalMax)
{
	if (!bHasDomain)
	{
		return FlowVizClipViewModelLocal::MakeNoDomainResult();
	}
	if (!FlowVizClipViewModelLocal::IsFiniteVector(LocalMin) || !FlowVizClipViewModelLocal::IsFiniteVector(LocalMax))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A crop box must be finite on every axis"));
	}
	if (LocalMax.X <= LocalMin.X || LocalMax.Y <= LocalMin.Y || LocalMax.Z <= LocalMin.Z)
	{
		// An inverted or zero-thickness box selects nothing, which renders as an
		// empty volume - indistinguishable from a case that failed to load.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("A crop box needs max > min on every axis"));
	}

	// NOT CLAMPED INTO THE DOMAIN, deliberately. A box wider than the domain is
	// meaningful - it selects everything - and clamping would silently rewrite
	// the numbers the user typed into the transform panel.
	CropMin = LocalMin;
	CropMax = LocalMax;
	return FCFDVizResult::Ok();
}

void FFlowVizClipViewModel::ResetCropBox()
{
	CropMin = FVector::ZeroVector;
	CropMax = bHasDomain ? DomainSize : FVector::OneVector;
}

bool FFlowVizClipViewModel::IsCropActive() const
{
	if (!bHasDomain)
	{
		return false;
	}
	const FVector Full = DomainSize;
	// A tolerance relative to the domain, not an absolute one: a millimetre case
	// and a kilometre case need different notions of "the same as the full box",
	// and an absolute epsilon would call every crop of a small domain inactive.
	const double Tolerance = 1.0e-9 * FMath::Max(1.0, Full.GetMax());
	return !CropMin.Equals(FVector::ZeroVector, Tolerance) || !CropMax.Equals(Full, Tolerance);
}

/* ========================================================================== */
/* What the render layer consumes                                              */
/* ========================================================================== */

FCFDVizResult FFlowVizClipViewModel::ApplyToRayMarchParameters(
	FFlowVizVolumeRayMarchParameters& OutParameters) const
{
	if (!bHasDomain)
	{
		return FlowVizClipViewModelLocal::MakeNoDomainResult();
	}

	// VALIDATE EVERY PLANE BEFORE WRITING ANY. A half-written plane array renders
	// a plausible wrong clip, and nobody investigates an image that looks fine.
	for (int32 Index = 0; Index < Planes.Num(); ++Index)
	{
		if (Planes[Index].bEnabled && !Planes[Index].IsValid())
		{
			return FCFDVizResult::Fail(
				ECFDVizError::IndexOutOfRange,
				FString::Printf(TEXT("Clip plane %d is degenerate"), Index));
		}
	}

	int32 Written = 0;
	for (const FFlowVizClipPlane& Plane : Planes)
	{
		if (!Plane.bEnabled)
		{
			continue;
		}
		// float, because the constant buffer is float4. The narrowing is the
		// shader's own precision and is the reason plane placement is not
		// meaningful below ~1e-7 of the domain size.
		OutParameters.ClipPlanes[Written] = FVector4f(
			static_cast<float>(Plane.Normal.X),
			static_cast<float>(Plane.Normal.Y),
			static_cast<float>(Plane.Normal.Z),
			static_cast<float>(Plane.Distance));
		++Written;
	}

	// THE UNUSED TAIL IS ZEROED. A stale plane left in slot 3 while NumClipPlanes
	// says 2 is invisible until someone raises the count, at which point a plane
	// nobody authored starts clipping the volume.
	for (int32 Index = Written; Index < FlowVizRayMarch::MaxClipPlanes; ++Index)
	{
		OutParameters.ClipPlanes[Index] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	}
	OutParameters.NumClipPlanes = static_cast<uint32>(Written);

	// THE ONE PLACE SOLVER UNITS BECOME CROP FRACTIONS. The parameter block
	// carries the crop normalised to PhysicalSize while the planes stay in solver
	// units - two spaces in one struct - so the conversion lives here and nowhere
	// else. DomainSize is guaranteed positive by SetDomainSize, so this cannot
	// divide by zero.
	const FVector NormalizedMin = CropMin / DomainSize;
	const FVector NormalizedMax = CropMax / DomainSize;
	OutParameters.CropBoxMin = FVector3f(
		static_cast<float>(NormalizedMin.X),
		static_cast<float>(NormalizedMin.Y),
		static_cast<float>(NormalizedMin.Z));
	OutParameters.CropBoxMax = FVector3f(
		static_cast<float>(NormalizedMax.X),
		static_cast<float>(NormalizedMax.Y),
		static_cast<float>(NormalizedMax.Z));

	return FCFDVizResult::Ok();
}
