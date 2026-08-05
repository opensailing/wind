// Copyright FlowViz contributors. All Rights Reserved.

#include "Render/FlowVizTransferFunction.h"

#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "Runtime/Launch/Resources/Version.h"

#include <limits>

/**
 * ADR 002 accepts a hand-written ray-marcher and names its cost: RHI resource
 * APIs churn between engine releases. The mitigation it names is that every
 * version-sensitive call lives in a file like this one. This assert is the
 * tripwire for that promise - an engine upgrade must land here deliberately
 * rather than silently recompiling against changed semantics.
 *
 * When it fires: re-check FRHITextureCreateDesc::Create2D, the
 * FRHICommandListBase::CreateTexture / UpdateTexture2D signatures, and whether
 * UpdateTexture2D still interprets SourcePitch as a BYTE count of the source
 * buffer's row. Then move the version below.
 */
static_assert(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8,
	"FlowVizTransferFunction.cpp is pinned to UE 5.8. Re-verify the RHI texture "
	"create/update API against the new engine, then update this assert. See "
	"the FlowVizTransferFunctionRHI comment in FlowVizTransferFunction.h.");

/* -------------------------------------------------------------------------- */
/* Local helpers                                                                */
/* -------------------------------------------------------------------------- */

namespace
{
	/**
	 * A failure whose message names the offending quantity and its value
	 * (engineering rule 12).
	 *
	 * Named TransferFunctionFailWith rather than FailWith because this module is a
	 * unity build: a bare FailWith in an anonymous namespace collides with the
	 * identically named helper in FlowVizVolumeTexture.cpp the moment the two land
	 * in the same chunk. See Docs/BUILD.md.
	 */
	FCFDVizResult TransferFunctionFailWith(ECFDVizError Error, FString Message)
	{
		return FCFDVizResult::Fail(Error, MoveTemp(Message));
	}

	/**
	 * The pixel-centre coordinate of LUT entry `Index` in a LUT of `Size` entries.
	 *
	 * ONE DEFINITION, used by both the colour side (via ColorMaps::BuildLut, which
	 * computes the identical expression) and the opacity side. `(i + 0.5) / N`,
	 * never `i / (N - 1)`; the difference is a half-texel hue shift against every
	 * matplotlib and ParaView reference figure. See the sampling-convention note
	 * at the top of FlowVizTransferFunction.h.
	 */
	float PixelCentre(int32 Index, int32 Size)
	{
		return (static_cast<float>(Index) + 0.5f) / static_cast<float>(Size);
	}
}

/* -------------------------------------------------------------------------- */
/* The opacity curve                                                            */
/* -------------------------------------------------------------------------- */

FFlowVizOpacityCurve FFlowVizOpacityCurve::MakeLinearRamp()
{
	FFlowVizOpacityCurve Curve;
	Curve.Points.Add(FFlowVizOpacityPoint(0.0f, 0.0f));
	Curve.Points.Add(FFlowVizOpacityPoint(1.0f, 1.0f));
	return Curve;
}

FFlowVizOpacityCurve FFlowVizOpacityCurve::MakeConstant(float Opacity)
{
	FFlowVizOpacityCurve Curve;
	// Clamped rather than rejected: this is a convenience constructor with no
	// error channel, and a curve that silently carried an out-of-range opacity
	// would be rejected later by Validate with no clue where it came from.
	Curve.Points.Add(FFlowVizOpacityPoint(0.0f, FMath::Clamp(Opacity, 0.0f, 1.0f)));
	return Curve;
}

void FFlowVizOpacityCurve::SortPoints()
{
	// StableSort, so two points authored at the same position keep the order the
	// user put them in - an unstable sort would make an editor's control points
	// swap places under the cursor.
	Points.StableSort([](const FFlowVizOpacityPoint& A, const FFlowVizOpacityPoint& B)
	{
		return A.Position < B.Position;
	});
}

float FFlowVizOpacityCurve::Evaluate(float Position) const
{
	const float Multiplier = FMath::IsFinite(OpacityMultiplier)
		? FMath::Clamp(OpacityMultiplier, 0.0f, 1.0f)
		: 1.0f;

	// EMPTY IS OPAQUE, NOT TRANSPARENT. A default transfer function must show the
	// data; an invisible volume reads as a failed load, and the user then goes
	// looking for a bug in the reader.
	if (Points.Num() == 0)
	{
		return Multiplier;
	}

	// The curve is evaluated per LUT entry, and the caller may hand over points in
	// any order, so sorting has to happen somewhere. A local copy keeps Evaluate
	// const and keeps an editor free to reorder points between calls.
	TArray<FFlowVizOpacityPoint, TInlineAllocator<16>> Sorted;
	Sorted.Reserve(Points.Num());
	for (const FFlowVizOpacityPoint& Point : Points)
	{
		Sorted.Add(Point);
	}
	Sorted.StableSort([](const FFlowVizOpacityPoint& A, const FFlowVizOpacityPoint& B)
	{
		return A.Position < B.Position;
	});

	// A NaN query must not reach the alpha channel of a texture: a NaN alpha
	// propagates through the compositing arithmetic and blanks whole pixels. The
	// first point is the answer because every comparison below would be false and
	// the endpoint hold is the honest default.
	if (!FMath::IsFinite(Position))
	{
		return FMath::Clamp(Sorted[0].Opacity, 0.0f, 1.0f) * Multiplier;
	}

	// ENDS ARE HELD, NEVER EXTRAPOLATED. Extrapolating an authored ramp past its
	// ends produces opacities outside [0,1] that then clamp, which reads as an
	// invisible or a solid region the user did not ask for. This is also what
	// gives an out-of-range value an opacity at all.
	if (Position <= Sorted[0].Position)
	{
		return FMath::Clamp(Sorted[0].Opacity, 0.0f, 1.0f) * Multiplier;
	}

	const FFlowVizOpacityPoint& Last = Sorted[Sorted.Num() - 1];
	if (Position >= Last.Position)
	{
		return FMath::Clamp(Last.Opacity, 0.0f, 1.0f) * Multiplier;
	}

	for (int32 Index = 0; Index < Sorted.Num() - 1; ++Index)
	{
		const FFlowVizOpacityPoint& A = Sorted[Index];
		const FFlowVizOpacityPoint& B = Sorted[Index + 1];
		if (Position >= A.Position && Position <= B.Position)
		{
			const float Span = B.Position - A.Position;
			// Two points at the same position: take the first, rather than
			// dividing by zero and producing a NaN alpha.
			const float Alpha = (Span <= 0.0f) ? 0.0f : (Position - A.Position) / Span;
			const float Value = FMath::Lerp(A.Opacity, B.Opacity, Alpha);
			return FMath::Clamp(Value, 0.0f, 1.0f) * Multiplier;
		}
	}

	// Unreachable for finite input given the guards above; the endpoint is the
	// safe answer rather than an uninitialised one.
	return FMath::Clamp(Last.Opacity, 0.0f, 1.0f) * Multiplier;
}

FCFDVizResult FFlowVizOpacityCurve::Validate() const
{
	if (Points.Num() > FlowVizTransferFunction::MaxOpacityPoints)
	{
		return TransferFunctionFailWith(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("the opacity curve has %d control points, the limit is %d"),
			Points.Num(), FlowVizTransferFunction::MaxOpacityPoints));
	}

	if (!FMath::IsFinite(OpacityMultiplier))
	{
		return TransferFunctionFailWith(ECFDVizError::InvalidHeader,
			TEXT("the opacity multiplier is not finite"));
	}
	if (OpacityMultiplier < 0.0f || OpacityMultiplier > 1.0f)
	{
		return TransferFunctionFailWith(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("the opacity multiplier is %f, outside 0..1"), OpacityMultiplier));
	}

	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const FFlowVizOpacityPoint& Point = Points[Index];

		if (!FMath::IsFinite(Point.Position) || !FMath::IsFinite(Point.Opacity))
		{
			return TransferFunctionFailWith(ECFDVizError::InvalidHeader, FString::Printf(
				TEXT("opacity point %d is not finite (position %f, opacity %f)"),
				Index, Point.Position, Point.Opacity));
		}
		if (Point.Position < 0.0f || Point.Position > 1.0f)
		{
			return TransferFunctionFailWith(ECFDVizError::IndexOutOfRange, FString::Printf(
				TEXT("opacity point %d sits at %f, outside the normalized domain 0..1"),
				Index, Point.Position));
		}
		if (Point.Opacity < 0.0f || Point.Opacity > 1.0f)
		{
			return TransferFunctionFailWith(ECFDVizError::IndexOutOfRange, FString::Printf(
				TEXT("opacity point %d has opacity %f, outside 0..1"),
				Index, Point.Opacity));
		}
	}

	// Unsorted points are deliberately NOT an error: an editor drags control
	// points past each other, and Evaluate sorts a working copy.
	return FCFDVizResult::Ok();
}

bool FFlowVizOpacityCurve::Equals(const FFlowVizOpacityCurve& Other, float Tolerance) const
{
	if (Points.Num() != Other.Points.Num())
	{
		return false;
	}
	if (!FMath::IsNearlyEqual(OpacityMultiplier, Other.OpacityMultiplier, Tolerance))
	{
		return false;
	}
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		if (!FMath::IsNearlyEqual(Points[Index].Position, Other.Points[Index].Position, Tolerance)
			|| !FMath::IsNearlyEqual(Points[Index].Opacity, Other.Points[Index].Opacity, Tolerance))
		{
			return false;
		}
	}
	return true;
}

/* -------------------------------------------------------------------------- */
/* The transfer function                                                        */
/* -------------------------------------------------------------------------- */

void FFlowVizTransferFunction::MakeDefaultDomain(
	ECFDVizColorMap Map,
	float DataMin,
	float DataMax,
	float& OutMin,
	float& OutMax)
{
	// A degenerate or non-finite range yields the unit domain rather than a
	// zero-width one, which would divide by zero in every mapping and paint the
	// whole field a single colour.
	if (!FMath::IsFinite(DataMin) || !FMath::IsFinite(DataMax) || !(DataMax > DataMin))
	{
		OutMin = 0.0f;
		OutMax = 1.0f;
		return;
	}

	if (CFDViz::ColorMaps::IsDiverging(Map))
	{
		// VISUAL_QA RULE 7. The neutral colour of a diverging map means "zero", so
		// the domain is symmetric about zero and the data range is widened to fit
		// inside it. Centring on the data midpoint instead puts white at
		// (min+max)/2 - for [-2, 8] that is 3, a value with no physical meaning,
		// presented as the neutral one.
		//
		// Driven by IsDiverging rather than a local list, so the rule and the
		// colormap metadata cannot drift apart.
		const float Magnitude = FMath::Max(FMath::Abs(DataMin), FMath::Abs(DataMax));
		if (Magnitude > 0.0f)
		{
			OutMin = -Magnitude;
			OutMax = Magnitude;
			return;
		}

		// The data is entirely zero. A symmetric unit domain still puts the
		// neutral colour on zero, which is the honest picture of a zero field.
		OutMin = -1.0f;
		OutMax = 1.0f;
		return;
	}

	// A sequential map keeps the data range: re-centring it would throw away half
	// the dynamic range for nothing.
	OutMin = DataMin;
	OutMax = DataMax;
}

FFlowVizTransferFunction FFlowVizTransferFunction::MakeDefault(
	ECFDVizColorMap Map,
	float DataMin,
	float DataMax)
{
	FFlowVizTransferFunction TransferFunction;
	TransferFunction.ColorMap = Map;
	MakeDefaultDomain(Map, DataMin, DataMax, TransferFunction.ValueRangeMin, TransferFunction.ValueRangeMax);
	return TransferFunction;
}

FFlowVizMappedValue FFlowVizTransferFunction::MapValueToNormalized(float Value) const
{
	FFlowVizMappedValue Mapped;

	// NaN and +/-inf are Invalid, NOT out-of-range. +inf is how CVF spells "no
	// valid data here", which is a different statement from "a very large
	// measurement", and colouring them the same would hide missing data behind a
	// plausible over-range flag.
	if (!FMath::IsFinite(Value))
	{
		Mapped.ValueClass = EFlowVizValueClass::Invalid;
		Mapped.Normalized = std::numeric_limits<float>::quiet_NaN();
		return Mapped;
	}

	const float Range = GetValueRange();
	if (!(Range > 0.0f) || !FMath::IsFinite(Range))
	{
		// Validate rejects such a transfer function, but MapValueToNormalized has
		// no error channel and is called per voxel. Reporting Invalid is the only
		// answer that cannot be mistaken for data.
		Mapped.ValueClass = EFlowVizValueClass::Invalid;
		Mapped.Normalized = std::numeric_limits<float>::quiet_NaN();
		return Mapped;
	}

	// NEVER CLAMPED. The coordinate runs negative below the domain and past 1
	// above it, so the shader can tell a value that sat exactly at ValueRangeMin
	// from one a thousand units below it. Rule 2 of this feature and VISUAL_QA
	// rule 4. bClampToRange deliberately does not appear here: it is a display
	// choice the renderer applies AFTER the classification, with full knowledge
	// of what it is hiding.
	Mapped.Normalized = (Value - ValueRangeMin) / Range;

	// Classified from the VALUE, not from the coordinate, so float division near
	// the endpoints cannot flip a boundary value into the wrong class.
	if (Value < ValueRangeMin)
	{
		Mapped.ValueClass = EFlowVizValueClass::UnderRange;
	}
	else if (Value > ValueRangeMax)
	{
		Mapped.ValueClass = EFlowVizValueClass::OverRange;
	}
	else
	{
		// The domain is inclusive at both ends: a value sitting exactly on the
		// maximum is data, not an overflow.
		Mapped.ValueClass = EFlowVizValueClass::InRange;
	}

	return Mapped;
}

float FFlowVizTransferFunction::MapNormalizedToValue(float Normalized) const
{
	// Extrapolates outside [0,1], matching the mapping's own refusal to clamp -
	// so a round trip through an out-of-range coordinate returns the value that
	// produced it.
	return ValueRangeMin + Normalized * GetValueRange();
}

FLinearColor FFlowVizTransferFunction::EvaluateColor(TArrayView<const FLinearColor> Lut, float Value) const
{
	const FFlowVizMappedValue Mapped = MapValueToNormalized(Value);

	switch (Mapped.ValueClass)
	{
	case EFlowVizValueClass::Invalid:
		return NaNColor;

	case EFlowVizValueClass::UnderRange:
		// VISUAL_QA RULE 4. The flag colour, NOT the colormap's minimum. Drawing
		// the minimum would present out-of-range data as the smallest real value
		// in the field - a quantitative lie that looks like a correct render.
		// bClampToRange is the user's explicit opt-out of that protection.
		return bClampToRange ? FlowVizTransferFunction::SampleLut(Lut, 0.0f) : UnderRangeColor;

	case EFlowVizValueClass::OverRange:
		return bClampToRange ? FlowVizTransferFunction::SampleLut(Lut, 1.0f) : OverRangeColor;

	default:
		return FlowVizTransferFunction::SampleLut(Lut, Mapped.Normalized);
	}
}

FCFDVizResult FFlowVizTransferFunction::Validate() const
{
	if (LutSize < FlowVizTransferFunction::MinLutSize || LutSize > FlowVizTransferFunction::MaxLutSize)
	{
		return TransferFunctionFailWith(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("LUT size %d is outside %d..%d"),
			LutSize, FlowVizTransferFunction::MinLutSize, FlowVizTransferFunction::MaxLutSize));
	}

	if (ColorMap < ECFDVizColorMap::Viridis || ColorMap >= ECFDVizColorMap::Count)
	{
		return TransferFunctionFailWith(ECFDVizError::UnsupportedDataType, FString::Printf(
			TEXT("colormap %d is outside the enum"), static_cast<int32>(ColorMap)));
	}

	if (ColorBands < 0)
	{
		return TransferFunctionFailWith(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("band count %d is negative"), ColorBands));
	}

	if (!FMath::IsFinite(ValueRangeMin) || !FMath::IsFinite(ValueRangeMax))
	{
		return TransferFunctionFailWith(ECFDVizError::InvalidHeader, FString::Printf(
			TEXT("the value domain [%f, %f] is not finite"), ValueRangeMin, ValueRangeMax));
	}

	if (!(ValueRangeMax > ValueRangeMin))
	{
		return TransferFunctionFailWith(ECFDVizError::InvalidHeader, FString::Printf(
			TEXT("the value domain [%f, %f] is empty or inverted"), ValueRangeMin, ValueRangeMax));
	}

	return Opacity.Validate();
}

bool FFlowVizTransferFunction::Equals(const FFlowVizTransferFunction& Other, float Tolerance) const
{
	return ColorMap == Other.ColorMap
		&& bReverseColorMap == Other.bReverseColorMap
		&& ColorBands == Other.ColorBands
		&& LutSize == Other.LutSize
		&& bClampToRange == Other.bClampToRange
		&& FMath::IsNearlyEqual(ValueRangeMin, Other.ValueRangeMin, Tolerance)
		&& FMath::IsNearlyEqual(ValueRangeMax, Other.ValueRangeMax, Tolerance)
		&& UnderRangeColor.Equals(Other.UnderRangeColor, Tolerance)
		&& OverRangeColor.Equals(Other.OverRangeColor, Tolerance)
		&& NaNColor.Equals(Other.NaNColor, Tolerance)
		&& MaskedColor.Equals(Other.MaskedColor, Tolerance)
		&& Opacity.Equals(Other.Opacity, Tolerance);
}

/* -------------------------------------------------------------------------- */
/* LUT assembly                                                                 */
/* -------------------------------------------------------------------------- */

FCFDVizResult FlowVizTransferFunction::BuildLut(
	const FFlowVizTransferFunction& TransferFunction,
	TArray<FLinearColor>& OutLut)
{
	const FCFDVizResult ValidateResult = TransferFunction.Validate();
	if (!ValidateResult.IsOk())
	{
		// Emptied, never left half-written: a partial LUT would upload and render
		// as a plausible wrong image.
		OutLut.Empty();
		return ValidateResult;
	}

	// RGB comes from the shared colour tables. Not re-implemented here, because a
	// second implementation of colormap sampling is a second thing to drift away
	// from colormaps.py - and drift there is invisible until someone compares two
	// figures side by side.
	CFDViz::ColorMaps::BuildLut(
		TransferFunction.ColorMap,
		OutLut,
		TransferFunction.LutSize,
		TransferFunction.bReverseColorMap,
		TransferFunction.ColorBands);

	if (OutLut.Num() != TransferFunction.LutSize)
	{
		OutLut.Empty();
		return TransferFunctionFailWith(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("the colour table returned %d entries, %d were requested"),
			OutLut.Num(), TransferFunction.LutSize));
	}

	for (int32 Index = 0; Index < OutLut.Num(); ++Index)
	{
		// THE SAME COORDINATE THE COLOUR USED. ColorMaps::BuildLut samples entry
		// `i` at (i + 0.5) / N, so the opacity curve is evaluated there too - a
		// control point at t and a colour stop at t then land on the same entry.
		// Evaluating alpha on any other grid would slide the ramp against the
		// colours by a fraction of a texel.
		const float Position = PixelCentre(Index, OutLut.Num());

		// ALPHA IS NOT PREMULTIPLIED. The ray-marcher composites front-to-back
		// with its own step-size correction and needs the un-premultiplied
		// colour; premultiplying here would make every opacity edit also change
		// the hue a Python reference figure of the same colormap renders.
		OutLut[Index].A = TransferFunction.Opacity.Evaluate(Position);
	}

	return FCFDVizResult::Ok();
}

FLinearColor FlowVizTransferFunction::SampleLut(TArrayView<const FLinearColor> Lut, float U)
{
	if (Lut.Num() == 0)
	{
		return FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	}

	// A NaN coordinate must not index an array. The first entry is arbitrary but
	// bounded; callers classify NaN before they get here.
	if (!FMath::IsFinite(U))
	{
		return Lut[0];
	}

	// THE MATCHING INVERSE OF THE PIXEL-CENTRE CONVENTION. Entry `i` sits at
	// (i + 0.5) / N, so the continuous texel coordinate is U*N - 0.5. Using
	// U*(N-1) instead - the natural-looking simplification - shifts every sample
	// by half a texel and breaks the round trip that proves the two halves agree.
	const float Texel = U * static_cast<float>(Lut.Num()) - 0.5f;

	// Ends are HELD, not wrapped or extrapolated. A wrapped LUT would hand an
	// out-of-range value the OPPOSITE end of the colormap, which is the most
	// misleading possible answer.
	if (Texel <= 0.0f)
	{
		return Lut[0];
	}
	if (Texel >= static_cast<float>(Lut.Num() - 1))
	{
		return Lut[Lut.Num() - 1];
	}

	const int32 Lower = FMath::FloorToInt(Texel);
	const int32 Upper = FMath::Min(Lower + 1, Lut.Num() - 1);
	const float Alpha = Texel - static_cast<float>(Lower);

	// Lerped in the same sRGB space the colour tables interpolate in - see the
	// interpolation-space note in CFDVizColorMaps.h. Lerping here in a different
	// space from BuildLut would make the sampled value disagree with the entry it
	// sits between.
	return FMath::Lerp(Lut[Lower], Lut[Upper], Alpha);
}

int64 FlowVizTransferFunction::GetPackedBytes(int32 LutSize)
{
	if (LutSize < MinLutSize || LutSize > MaxLutSize)
	{
		return INDEX_NONE;
	}
	// MaxLutSize * LutEntryBytes is 32 KB, so this cannot overflow an int64 - but
	// it is computed in int64 anyway so a future limit change cannot make it wrap
	// a bounds check into a rubber stamp.
	return static_cast<int64>(LutSize) * static_cast<int64>(LutEntryBytes);
}

FCFDVizResult FlowVizTransferFunction::PackLutBytes(
	TArrayView<const FLinearColor> Lut,
	TArray<uint8>& OutBytes)
{
	const int64 Needed = GetPackedBytes(Lut.Num());
	if (Needed == INDEX_NONE)
	{
		OutBytes.Empty();
		return TransferFunctionFailWith(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("a LUT of %d entries is outside %d..%d"),
			Lut.Num(), MinLutSize, MaxLutSize));
	}

	OutBytes.SetNumUninitialized(static_cast<int32>(Needed));

	// float16 rather than 8-bit UNORM is a posterization decision, not a
	// performance one: an 8-bit LUT collapses viridis to 240 distinct triples out
	// of 256 entries, which is VISUAL_QA rule 11's failure introduced by the
	// texture format rather than by the colormap. See the LutPixelFormat comment.
	//
	// Written channel by channel rather than by memcpy of an FFloat16Color array,
	// so the RGBA order and the little-endian byte order this produces are
	// explicit and are what the device test reads back.
	uint8* Write = OutBytes.GetData();
	for (int32 Index = 0; Index < Lut.Num(); ++Index)
	{
		const FLinearColor& Entry = Lut[Index];
		const float Channels[4] = { Entry.R, Entry.G, Entry.B, Entry.A };
		for (int32 Channel = 0; Channel < 4; ++Channel)
		{
			const FFloat16 Half(Channels[Channel]);
			*Write++ = static_cast<uint8>(Half.Encoded & 0xFF);
			*Write++ = static_cast<uint8>((Half.Encoded >> 8) & 0xFF);
		}
	}

	return FCFDVizResult::Ok();
}

FCFDVizResult FlowVizTransferFunction::MakeShaderParameters(
	const FFlowVizTransferFunction& TransferFunction,
	FFlowVizTransferFunctionShaderParameters& OutParams)
{
	const FCFDVizResult ValidateResult = TransferFunction.Validate();
	if (!ValidateResult.IsOk())
	{
		// OutParams is deliberately untouched: a half-populated constant buffer
		// renders a plausible wrong image, which is worse than not rendering.
		return ValidateResult;
	}

	const float Range = TransferFunction.GetValueRange();
	const float LutSizeF = static_cast<float>(TransferFunction.LutSize);

	FFlowVizTransferFunctionShaderParameters Params;
	Params.ValueRangeMin = TransferFunction.ValueRangeMin;
	Params.ValueRangeMax = TransferFunction.ValueRangeMax;
	// Precomputed so the shader never divides per sample, and so a degenerate
	// domain is caught here - by Validate above - rather than arriving as a
	// per-pixel infinity.
	Params.InvValueRange = 1.0f / Range;
	Params.LutSize = TransferFunction.LutSize;

	Params.OpacityMultiplier = FMath::Clamp(TransferFunction.Opacity.OpacityMultiplier, 0.0f, 1.0f);
	Params.bClampToRange = TransferFunction.bClampToRange ? 1u : 0u;

	// The pixel-centre convention, carried to the GPU. The shader turns a
	// normalized value into a texture coordinate as
	// U = LutHalfTexel + Normalized * LutCoordScale, which maps 0 to the first
	// texel centre and 1 to the last. Precomputed so the convention has ONE home
	// and a shader cannot quietly adopt the i/(N-1) version of it.
	Params.LutHalfTexel = 0.5f / LutSizeF;
	Params.LutCoordScale = (LutSizeF - 1.0f) / LutSizeF;

	Params.UnderRangeColor = TransferFunction.UnderRangeColor;
	Params.OverRangeColor = TransferFunction.OverRangeColor;
	Params.NaNColor = TransferFunction.NaNColor;
	Params.MaskedColor = TransferFunction.MaskedColor;

	OutParams = Params;
	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* Version-isolated RHI wrapper                                                 */
/* -------------------------------------------------------------------------- */

FCFDVizResult FlowVizTransferFunctionRHI::CheckDeviceSupport(int32 LutSize)
{
	if (LutSize < FlowVizTransferFunction::MinLutSize || LutSize > FlowVizTransferFunction::MaxLutSize)
	{
		return TransferFunctionFailWith(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("LUT size %d is outside %d..%d"),
			LutSize, FlowVizTransferFunction::MinLutSize, FlowVizTransferFunction::MaxLutSize));
	}

	// No device to fail against, so there is nothing to report. GUsingNullRHI is
	// checked as well as GIsRHIInitialized because the null RHI DOES set
	// GIsRHIInitialized while leaving the pixel-format capability table empty -
	// judging a format against that table would declare every format unsupported,
	// and every logic test would fail for a reason that has nothing to do with
	// the logic.
	if (!GIsRHIInitialized || GUsingNullRHI)
	{
		return FCFDVizResult::Ok();
	}

	const int32 MaxDimension = GMaxTextureDimensions;
	if (MaxDimension > 0 && LutSize > MaxDimension)
	{
		return TransferFunctionFailWith(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("a %d-entry LUT exceeds this device's %d-texel 2D texture limit"),
			LutSize, MaxDimension));
	}

	if (!UE::PixelFormat::HasCapabilities(
			FlowVizTransferFunction::LutPixelFormat, EPixelFormatCapabilities::Texture2D))
	{
		return TransferFunctionFailWith(ECFDVizError::UnsupportedDataType, FString::Printf(
			TEXT("this device cannot create %s as a 2D texture"),
			GetPixelFormatString(FlowVizTransferFunction::LutPixelFormat)));
	}

	// Checked separately from Texture2D: a format the device can allocate but not
	// filter would give a stepped LUT - posterization introduced by the sampler,
	// which VISUAL_QA rule 11 forbids just as firmly as posterization introduced
	// by the format.
	if (!UE::PixelFormat::HasCapabilities(
			FlowVizTransferFunction::LutPixelFormat, EPixelFormatCapabilities::TextureSample))
	{
		return TransferFunctionFailWith(ECFDVizError::UnsupportedDataType, FString::Printf(
			TEXT("this device cannot sample %s in a shader"),
			GetPixelFormatString(FlowVizTransferFunction::LutPixelFormat)));
	}

	return FCFDVizResult::Ok();
}

FTextureRHIRef FlowVizTransferFunctionRHI::CreateLutTexture(
	FRHICommandListBase& RHICmdList,
	int32 LutSize,
	const TCHAR* DebugName,
	FCFDVizResult& OutResult)
{
	OutResult = CheckDeviceSupport(LutSize);
	if (!OutResult.IsOk())
	{
		// A malformed session must produce a message, not a crash.
		return FTextureRHIRef();
	}

	// Nx1 2D, not 1D. RHI 1D textures are not creatable on every platform this
	// project targets; an Nx1 2D texture is universally supported, samples
	// identically with V fixed at 0.5, and costs nothing extra. See the
	// FlowVizTransferFunctionRHI comment in the header.
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(
			DebugName != nullptr ? DebugName : TEXT("FlowVizTransferFunctionLut"),
			FIntPoint(LutSize, 1),
			FlowVizTransferFunction::LutPixelFormat)
		.SetFlags(ETextureCreateFlags::ShaderResource);

	FTextureRHIRef Texture = RHICmdList.CreateTexture(Desc);
	if (!Texture.IsValid())
	{
		OutResult = TransferFunctionFailWith(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("the RHI refused a %d x 1 %s LUT texture"),
			LutSize, GetPixelFormatString(FlowVizTransferFunction::LutPixelFormat)));
	}
	return Texture;
}

FCFDVizResult FlowVizTransferFunctionRHI::UpdateLutTexture(
	FRHICommandListBase& RHICmdList,
	FRHITexture* Texture,
	int32 LutSize,
	TArrayView<const uint8> LutBytes)
{
	if (Texture == nullptr)
	{
		return TransferFunctionFailWith(ECFDVizError::InvalidHeader,
			TEXT("a LUT update needs a texture"));
	}

	const int64 Needed = FlowVizTransferFunction::GetPackedBytes(LutSize);
	if (Needed == INDEX_NONE)
	{
		return TransferFunctionFailWith(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("LUT size %d is outside %d..%d"),
			LutSize, FlowVizTransferFunction::MinLutSize, FlowVizTransferFunction::MaxLutSize));
	}

	// Checked here rather than inside the RHI call: UpdateTexture2D has no idea
	// how long the source buffer is and will happily walk off the end of it.
	if (static_cast<int64>(LutBytes.Num()) != Needed)
	{
		return TransferFunctionFailWith(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("the LUT upload buffer is %d bytes, a %d-entry LUT needs %lld"),
			LutBytes.Num(), LutSize, Needed));
	}

	const FUpdateTextureRegion2D Region(
		/*DestX*/ 0, /*DestY*/ 0,
		/*SrcX*/ 0, /*SrcY*/ 0,
		static_cast<uint32>(LutSize), /*Height*/ 1u);

	// SourcePitch is the SOURCE buffer's row length in bytes. The whole LUT is one
	// row, so it is the whole buffer.
	RHICmdList.UpdateTexture2D(
		Texture,
		/*MipIndex*/ 0,
		Region,
		static_cast<uint32>(Needed),
		LutBytes.GetData());

	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* The persistent resource                                                      */
/* -------------------------------------------------------------------------- */

FFlowVizTransferFunctionResource::~FFlowVizTransferFunctionResource()
{
	// The owner is required to have flushed rendering commands; this is the last
	// line of defence, dropping the reference on whatever thread runs the
	// destructor rather than leaking it.
	LutTexture.SafeRelease();
}

FCFDVizResult FFlowVizTransferFunctionResource::Update(const FFlowVizTransferFunction& InTransferFunction)
{
	const FCFDVizResult ValidateResult = InTransferFunction.Validate();
	if (!ValidateResult.IsOk())
	{
		// NOTHING changes on a rejected update: the resident texture keeps its
		// previous contents rather than being left half-updated, so a bad edit in
		// the UI cannot blank a view that was showing correct data.
		return ValidateResult;
	}

	// ENGINEERING RULE 2. An identical transfer function is a complete no-op - no
	// LUT build, no render command, no upload - which is what makes this safe to
	// call every frame from a UI that does not track its own dirtiness. An idle
	// viewport costs zero RHI work.
	if (HasContent() && TransferFunction.Equals(InTransferFunction))
	{
		return FCFDVizResult::Ok();
	}

	// Built on the CALLING thread and handed over as bytes, so the render thread
	// never touches the colormap tables. The LUT is 2 KB and a human editing a
	// control point changes it tens of times a second at worst.
	TArray<FLinearColor> NewLut;
	const FCFDVizResult BuildResult = FlowVizTransferFunction::BuildLut(InTransferFunction, NewLut);
	if (!BuildResult.IsOk())
	{
		return BuildResult;
	}

	TArray<uint8> Bytes;
	const FCFDVizResult PackResult = FlowVizTransferFunction::PackLutBytes(NewLut, Bytes);
	if (!PackResult.IsOk())
	{
		return PackResult;
	}

	TransferFunction = InTransferFunction;
	Lut = MoveTemp(NewLut);
	++BuildCount;

	// Counted on the game thread from the DECISION - "did LutSize change?" - not
	// from whether an RHI texture currently exists. Two reasons: a test can then
	// observe it without flushing the render thread, and under -nullrhi (where
	// CreateTexture returns nothing and LutTexture is never valid) a
	// validity-based count would climb on every Update and report a rule-2
	// violation on a resource that has none.
	if (EnqueuedLutSize != TransferFunction.LutSize)
	{
		++TextureCreateCount;
		EnqueuedLutSize = TransferFunction.LutSize;
	}

	FFlowVizTransferFunctionResource* Self = this;
	const int32 InLutSize = TransferFunction.LutSize;
	ENQUEUE_RENDER_COMMAND(FlowVizTransferFunctionUpload)(
		[Self, InLutSize, Payload = MoveTemp(Bytes)](FRHICommandListImmediate& RHICmdList) mutable
		{
			Self->UploadOnRenderThread(RHICmdList, InLutSize, MoveTemp(Payload));
		});

	return FCFDVizResult::Ok();
}

void FFlowVizTransferFunctionResource::UploadOnRenderThread(
	FRHICommandListBase& RHICmdList,
	int32 InLutSize,
	TArray<uint8>&& LutBytes)
{
	// CREATED ONCE and updated in place for the life of the transfer function
	// (engineering rule 2). A LutSize change is the ONLY reason to release and
	// recreate - an opacity edit, a colormap change or a re-range all reuse the
	// existing texture.
	if (!LutTexture.IsValid() || TextureLutSize != InLutSize)
	{
		LutTexture.SafeRelease();

		FCFDVizResult CreateResult;
		LutTexture = FlowVizTransferFunctionRHI::CreateLutTexture(
			RHICmdList, InLutSize, TEXT("FlowVizTransferFunctionLut"), CreateResult);
		if (!CreateResult.IsOk())
		{
			CreateResult.LogIfFailed();
			TextureLutSize = 0;
			return;
		}
		TextureLutSize = InLutSize;
	}

	if (!LutTexture.IsValid())
	{
		// No device (the null RHI returns nothing from CreateTexture). Not an
		// error to log every frame: the CPU-side LUT is still correct and is what
		// the legend and the probe readout sample.
		return;
	}

	const FCFDVizResult UpdateResult = FlowVizTransferFunctionRHI::UpdateLutTexture(
		RHICmdList, LutTexture.GetReference(), InLutSize, LutBytes);
	UpdateResult.LogIfFailed();
}

FCFDVizResult FFlowVizTransferFunctionResource::MakeShaderParameters(
	FFlowVizTransferFunctionShaderParameters& OutParams) const
{
	if (!HasContent())
	{
		// Refused rather than emitting a zeroed block, which would render a
		// plausible wrong image instead of nothing.
		return TransferFunctionFailWith(ECFDVizError::InvalidHeader,
			TEXT("the transfer-function resource has no content yet"));
	}
	return FlowVizTransferFunction::MakeShaderParameters(TransferFunction, OutParams);
}

void FFlowVizTransferFunctionResource::ReleaseResources()
{
	if (!LutTexture.IsValid())
	{
		return;
	}

	FFlowVizTransferFunctionResource* Self = this;
	ENQUEUE_RENDER_COMMAND(FlowVizTransferFunctionRelease)(
		[Self](FRHICommandListImmediate&)
		{
			Self->LutTexture.SafeRelease();
			Self->TextureLutSize = 0;
		});

	// Cleared on the game thread so a later Update at the same LutSize is
	// correctly counted as a create - the texture really is gone.
	EnqueuedLutSize = 0;
}
