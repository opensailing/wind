// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizColorMaps.h"
#include "CFDViz/CFDVizTypes.h"

// CoreMinimal.h forward-declares TArrayView but does not define it, and several
// entry points below take one by value.
#include "Containers/ArrayView.h"
#include "PixelFormat.h"
#include "RHIFwd.h"

/**
 * The transfer function - the one place a field value becomes a colour and an
 * opacity (plan.md section 10.3, Milestone D).
 *
 * WHAT THIS FILE IS FOR. `CFDViz::ColorMaps` owns the colour tables, which are
 * generated from Tools/cfdviz/src/cfdviz/colormaps.py and are shared with the
 * Python tools. This file owns everything the colour tables deliberately do NOT:
 * the opacity curve, the value domain in solver units, the mapping from a solver
 * value to a LUT coordinate, and the persistent RHI texture that carries the
 * combined result to the shader. It does not ray-march and it does not decide
 * what any particular field means.
 *
 * THREE RULES THIS FILE EXISTS TO ENFORCE.
 *
 *  1. OPACITY IS NOT BAKED INTO THE COLOUR TABLE. `ColorMaps::BuildLut` sets
 *     alpha to 1 and says so in its own comment. Opacity is a separately
 *     editable curve (FFlowVizOpacityCurve) that is combined with the colour
 *     table only at the moment the RGBA LUT is assembled, so editing the opacity
 *     ramp cannot perturb a colour that a Python reference figure also renders.
 *     FlowVizTransferFunction::BuildLut is the only function that combines them.
 *
 *  2. VALUES ARE NEVER CLAMPED INTO THE DOMAIN. MapValueToNormalized returns a
 *     coordinate that goes negative below ValueRangeMin and above 1 above
 *     ValueRangeMax, and FFlowVizValueClass reports which. A value outside the
 *     domain must remain DISTINGUISHABLE so the shader can colour it distinctly
 *     (VISUAL_QA rule 4: invalid and out-of-range data is visibly invalid, never
 *     silently the colormap's minimum). Clamping happens - if at all - in the
 *     sampler, after the classification, and only when bClampToRange says so.
 *
 *  3. A DIVERGING MAP CENTRES ON ZERO, NOT ON THE DATA MIDPOINT. VISUAL_QA
 *     rule 7. MakeDefaultDomain applies it: with coolwarm or blue-white-red
 *     selected, a data range of [-2, 8] becomes [-8, 8] so the neutral colour
 *     lands on zero rather than on 3, which means nothing. A sequential map
 *     keeps the data range unchanged.
 *
 * THE SAMPLING CONVENTION IS NOT NEGOTIABLE. Both this file and colormaps.py
 * sample LUT entry `i` at `t = (i + 0.5) / N` - PIXEL CENTRES. Using
 * `i / (N - 1)` shifts the whole map by half a texel. That is a real, visible
 * hue shift against a matplotlib or ParaView reference figure and it is the
 * specific bug the two-implementation design exists to catch; see the sampling
 * convention section of CFDVizColorMaps.h. SampleLut below reconstructs with
 * the matching inverse, so a LUT round-trips through it. Do not "simplify"
 * either half.
 *
 * THE SPLIT THAT MAKES THIS TESTABLE. Everything that decides WHAT bytes go into
 * the texture - the opacity curve, the domain mapping, the classification, the
 * LUT assembly, the pixel-format packing - is a pure function of plain data and
 * is exercised by FlowVizTransferFunctionTest with no RHI device present. Only
 * the three functions behind FlowVizTransferFunctionRHI touch the RHI, and what
 * that leaves unverified is stated on that namespace rather than glossed over.
 */

/* -------------------------------------------------------------------------- */
/* Policy limits                                                                */
/* -------------------------------------------------------------------------- */

namespace FlowVizTransferFunction
{
	/**
	 * Default LUT width, in entries.
	 *
	 * 256 is the width the Python side defaults to and the width the committed
	 * cross-check values in FlowVizTransferFunctionTest were generated at, so it
	 * is the size at which the two implementations are actually compared.
	 */
	inline constexpr int32 DefaultLutSize = 256;

	/**
	 * Narrowest LUT this path will build.
	 *
	 * Two entries is the minimum that can express a gradient at all; one entry is
	 * a solid colour wearing a transfer function's clothes, and it would make the
	 * SampleLut reconstruction degenerate. Rejected rather than silently widened.
	 */
	inline constexpr int32 MinLutSize = 2;

	/**
	 * Widest LUT this path will build. 4096 is the smallest 1D texture width every
	 * target guarantees well past, and no scientific colormap carries information
	 * beyond it: at 4096 entries an 8-bit output is oversampled sixteen times over.
	 */
	inline constexpr int32 MaxLutSize = 4096;

	/** Most opacity control points one curve may carry. Far beyond any hand-authored curve; the cap exists so a corrupt session file cannot allocate unboundedly. */
	inline constexpr int32 MaxOpacityPoints = 256;

	/**
	 * The LUT texture's pixel format.
	 *
	 * PF_FloatRGBA (RGBA16F), not PF_B8G8R8A8. An 8-bit LUT quantises viridis to
	 * 240 distinct triples out of 256 entries - sixteen adjacent pairs collapse
	 * into one colour, which is posterization in the transfer function and is
	 * exactly what VISUAL_QA rule 11 forbids. float16 keeps all 256 distinct
	 * (verified in FlowVizTransferFunctionTest against the Python tables) at a cost
	 * of 2 KB for the whole texture.
	 */
	inline constexpr EPixelFormat LutPixelFormat = PF_FloatRGBA;

	/** Bytes one LUT entry occupies in LutPixelFormat: four float16 channels. */
	inline constexpr int32 LutEntryBytes = 8;
}

/* -------------------------------------------------------------------------- */
/* Where a value fell relative to the domain                                    */
/* -------------------------------------------------------------------------- */

/**
 * What happened when a solver value was mapped into the transfer function.
 *
 * THIS ENUM IS THE POINT OF RULE 4. A shader that only receives a clamped [0,1]
 * coordinate cannot tell a value that sat exactly at ValueRangeMin from one that
 * sat a thousand units below it, and it draws them the same colour - which is a
 * quantitative lie about where the data actually is. Every mapping returns one of
 * these alongside the coordinate so under-range, over-range, NaN and in-range are
 * four separately colourable outcomes.
 */
enum class EFlowVizValueClass : uint8
{
	/** ValueRangeMin <= Value <= ValueRangeMax. The only class the colormap itself describes. */
	InRange = 0,

	/** Value < ValueRangeMin. Coloured with FFlowVizTransferFunction::UnderRangeColor. */
	UnderRange,

	/** Value > ValueRangeMax. Coloured with FFlowVizTransferFunction::OverRangeColor. */
	OverRange,

	/** The value is NaN or infinite. Distinct from out-of-range: +inf is how CVF spells "no valid data" and is not a large measurement. */
	Invalid
};

/** A solver value mapped into the transfer function's domain. */
struct FFlowVizMappedValue
{
	/**
	 * LUT coordinate. In [0,1] exactly when ValueClass is InRange; NEGATIVE below
	 * the domain and GREATER THAN 1 above it. Not clamped - see rule 2 on this
	 * file. NaN when ValueClass is Invalid, because there is no coordinate that
	 * honestly represents a NaN.
	 */
	float Normalized = 0.0f;

	/** Which of the four outcomes this was. */
	EFlowVizValueClass ValueClass = EFlowVizValueClass::InRange;

	/** True only for EFlowVizValueClass::InRange. */
	bool IsInRange() const
	{
		return ValueClass == EFlowVizValueClass::InRange;
	}
};

/* -------------------------------------------------------------------------- */
/* The opacity curve                                                            */
/* -------------------------------------------------------------------------- */

/** One editable opacity control point. */
struct FFlowVizOpacityPoint
{
	/** Position over the NORMALIZED domain, [0,1]. Not a solver value: the curve survives a domain change. */
	float Position = 0.0f;

	/** Opacity at this point, [0,1]. */
	float Opacity = 1.0f;

	FFlowVizOpacityPoint() = default;

	FFlowVizOpacityPoint(float InPosition, float InOpacity)
		: Position(InPosition)
		, Opacity(InOpacity)
	{
	}
};

/**
 * An editable opacity transfer function: control points with interpolation
 * between them, plus a global multiplier.
 *
 * SEPARATE FROM THE COLOUR TABLE ON PURPOSE. See rule 1 on this file. The colour
 * tables are shared with Python and are compared against reference figures; the
 * opacity ramp is a per-view display choice a user drags around. Merging them
 * would mean every opacity edit invalidates a colour cross-check.
 *
 * POSITIONS ARE NORMALIZED, NOT SOLVER UNITS. A curve authored against velocity
 * in m/s keeps its shape when the domain is re-ranged to a percentile, which is
 * what a user dragging the range slider expects. Converting to solver units is
 * FFlowVizTransferFunction::MapValueToNormalized's job and happens first.
 *
 * INTERPOLATION IS LINEAR IN sRGB-SPACE ALPHA, matching how the colour side
 * interpolates. Opacity has no perceptual-uniformity argument attached to it, but
 * a curve that interpolates differently from the colours it multiplies produces
 * a composite whose two halves disagree at every point between control points.
 */
struct FFlowVizOpacityCurve
{
	/**
	 * Control points. Need not be sorted on entry - Evaluate and BuildLut sort a
	 * working copy - but SortPoints exists so an editor can keep them ordered.
	 *
	 * EMPTY IS NOT ZERO OPACITY. An empty curve evaluates to 1.0 everywhere (times
	 * the multiplier), so a default-constructed transfer function shows the data
	 * rather than an invisible volume that reads as a failed load.
	 */
	TArray<FFlowVizOpacityPoint> Points;

	/**
	 * Global opacity multiplier applied after interpolation, [0, 1].
	 *
	 * Separate from the curve so "make the whole thing more transparent" is one
	 * slider rather than an edit to every control point - and so it can be undone
	 * without losing the curve's shape (plan.md section 11, transfer-function
	 * edits are undoable).
	 */
	float OpacityMultiplier = 1.0f;

	/** The ramp every volume renderer wants as a starting point: transparent at the domain minimum, opaque at the maximum. */
	FLOWVIZRUNTIME_API static FFlowVizOpacityCurve MakeLinearRamp();

	/** A constant-opacity curve. Useful for MIP and for iso-surface modes, where the ramp is meaningless. */
	FLOWVIZRUNTIME_API static FFlowVizOpacityCurve MakeConstant(float Opacity);

	/** Sort Points ascending by Position. Stable, so two points at the same position keep their authored order. */
	FLOWVIZRUNTIME_API void SortPoints();

	/**
	 * Opacity at a normalized position, INCLUDING OpacityMultiplier.
	 *
	 * - No points: OpacityMultiplier (i.e. fully opaque by default).
	 * - One point: that point's opacity, everywhere. A single control point is a
	 *   constant, not a ramp from zero.
	 * - Outside the first/last point: that endpoint's opacity, held flat. The
	 *   curve is NOT extrapolated - extrapolating an authored ramp past its ends
	 *   produces opacities outside [0,1] that then clamp, which reads as an
	 *   invisible or a solid region the user did not ask for.
	 *
	 * @param Position Normalized domain coordinate. Values outside [0,1] are
	 *                 answered by the endpoint hold above, which is what makes an
	 *                 out-of-range value still get an opacity.
	 * @return Opacity in [0,1]. Always finite: a NaN Position yields the first
	 *         point's opacity rather than propagating NaN into the alpha channel.
	 */
	FLOWVIZRUNTIME_API float Evaluate(float Position) const;

	/**
	 * Check the curve is usable before it is built into a texture.
	 *
	 * Rejects: more than FlowVizTransferFunction::MaxOpacityPoints points; a
	 * non-finite position, opacity or multiplier; a position or opacity outside
	 * [0,1]; a multiplier outside [0,1]. Unsorted points are NOT an error - they
	 * are sorted on use.
	 *
	 * @return Ok, or a failure naming the offending point and its value.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult Validate() const;

	/** Value-equality, for change detection - see FFlowVizTransferFunctionResource::Update. */
	FLOWVIZRUNTIME_API bool Equals(const FFlowVizOpacityCurve& Other, float Tolerance = UE_KINDA_SMALL_NUMBER) const;
};

/* -------------------------------------------------------------------------- */
/* The transfer function itself                                                 */
/* -------------------------------------------------------------------------- */

/**
 * A complete transfer function: which colormap, over which value domain, with
 * which opacity curve, and what to draw where the data is not in the domain.
 *
 * This is plain data. It is what a session file stores, what the editor UI edits,
 * and what FFlowVizTransferFunctionResource turns into a texture.
 */
struct FFlowVizTransferFunction
{
	/** Which colour table. Defaults to CFDViz::ColorMaps::Default (viridis) - never a rainbow (VISUAL_QA rule 6). */
	ECFDVizColorMap ColorMap = CFDViz::ColorMaps::Default;

	/** Flip the colour table end-for-end. Applied by ColorMaps::BuildLut, not re-implemented here. */
	bool bReverseColorMap = false;

	/**
	 * Quantize the colours into this many discrete bands, or 0 for continuous.
	 *
	 * Banding is a legitimate scientific display choice - it makes level sets
	 * readable - which is why it is a parameter of ColorMaps::BuildLut rather than
	 * a posterize applied afterwards that would also corrupt the legend. Note that
	 * it is the ONE case in which a stepped LUT is intentional; the
	 * no-posterization check in FlowVizTransferFunctionTest tests continuous LUTs,
	 * because asserting smoothness on a banded one would be asserting the feature
	 * away.
	 */
	int32 ColorBands = 0;

	/**
	 * Domain minimum, IN SOLVER UNITS. The value that maps to LUT coordinate 0.
	 *
	 * For under-range colouring only - values below it are NOT clamped up into the
	 * domain. See rule 2 on this file.
	 */
	float ValueRangeMin = 0.0f;

	/** Domain maximum, in solver units. The value that maps to LUT coordinate 1. */
	float ValueRangeMax = 1.0f;

	/** The opacity ramp. Separate from the colour table by design - rule 1. */
	FFlowVizOpacityCurve Opacity;

	/**
	 * Colour drawn for a value BELOW ValueRangeMin. Cyan by default.
	 *
	 * The four flag colours below were chosen to be far from every entry of every
	 * shipped colormap AND from each other, so a flagged voxel cannot be mistaken
	 * for data. FlowVizTransferFunctionTest asserts those separations numerically
	 * against the Python tables, so a future colormap addition that collides with
	 * a flag colour fails a test rather than quietly making invalid data look
	 * plausible.
	 */
	FLinearColor UnderRangeColor = FLinearColor(0.0f, 1.0f, 1.0f, 1.0f);

	/** Colour drawn for a value ABOVE ValueRangeMax. Magenta by default. */
	FLinearColor OverRangeColor = FLinearColor(1.0f, 0.0f, 1.0f, 1.0f);

	/** Colour drawn for NaN or infinity. Pure green by default. */
	FLinearColor NaNColor = FLinearColor(0.0f, 1.0f, 0.0f, 1.0f);

	/** Colour drawn for a cell the mask field rejected. Dark green by default - distinct from NaN, since the diagnostics panel counts them separately. */
	FLinearColor MaskedColor = FLinearColor(0.0f, 0.55f, 0.0f, 1.0f);

	/**
	 * Whether the SHADER should clamp an out-of-range value into the domain
	 * instead of drawing the under/over colours (plan.md section 10.3's "clamp to
	 * range" control).
	 *
	 * FALSE BY DEFAULT, and this flag never changes what MapValueToNormalized
	 * returns: the classification always happens, so turning clamping on is a
	 * display choice the renderer applies with full knowledge of what it is
	 * hiding, not an erasure of the information. A flag that silently rewrote the
	 * mapping would make rule 4 unimplementable downstream.
	 */
	bool bClampToRange = false;

	/** Entries in the LUT texture. FlowVizTransferFunction::MinLutSize..MaxLutSize. */
	int32 LutSize = FlowVizTransferFunction::DefaultLutSize;

	/**
	 * Build the default transfer function for a field with this data range.
	 *
	 * DIVERGING MAPS ARE CENTRED ON ZERO (VISUAL_QA rule 7). For coolwarm or
	 * blue-white-red the domain becomes [-M, +M] with M = max(|DataMin|, |DataMax|),
	 * so the map's neutral colour lands exactly on zero. Centring on the data
	 * midpoint instead puts white at (min+max)/2, which for a range of [-2, 8] is
	 * 3 - a value with no physical meaning, presented as the neutral one.
	 *
	 * A sequential map keeps [DataMin, DataMax] unchanged; re-centring it would
	 * throw away half the dynamic range for nothing.
	 *
	 * @param DataMin,DataMax The field's actual range. A degenerate or non-finite
	 *                        range yields the unit domain [0,1] rather than a
	 *                        zero-width one, which would divide by zero in every
	 *                        mapping.
	 */
	FLOWVIZRUNTIME_API static FFlowVizTransferFunction MakeDefault(
		ECFDVizColorMap Map,
		float DataMin,
		float DataMax);

	/**
	 * The domain MakeDefault would choose, without building the rest.
	 *
	 * Exposed separately because the range controls in plan.md section 10.3 (reset
	 * range, global range, per-frame range) all need the same rule-7 decision, and
	 * a second implementation of it would be a second place to get zero-centring
	 * wrong.
	 */
	FLOWVIZRUNTIME_API static void MakeDefaultDomain(
		ECFDVizColorMap Map,
		float DataMin,
		float DataMax,
		float& OutMin,
		float& OutMax);

	/** Domain width in solver units. Zero or negative means a degenerate domain, which Validate rejects. */
	float GetValueRange() const
	{
		return ValueRangeMax - ValueRangeMin;
	}

	/**
	 * Map a solver value to a LUT coordinate and a classification.
	 *
	 * NEVER CLAMPS. The returned Normalized runs negative below the domain and
	 * past 1 above it, so a shader can tell how far outside a value was and colour
	 * it distinctly. This is rule 2 and VISUAL_QA rule 4; the accompanying
	 * EFlowVizValueClass is what makes it actionable.
	 *
	 * @param Value A solver-unit field value. NaN and +/-inf yield
	 *              EFlowVizValueClass::Invalid with a NaN coordinate.
	 */
	FLOWVIZRUNTIME_API FFlowVizMappedValue MapValueToNormalized(float Value) const;

	/** Exact inverse of MapValueToNormalized for an in-range coordinate. Extrapolates outside [0,1], matching the mapping's own refusal to clamp. */
	FLOWVIZRUNTIME_API float MapNormalizedToValue(float Normalized) const;

	/**
	 * The colour AND opacity for a solver value, as the shader would draw it.
	 *
	 * The CPU reference for the shader's behaviour: in-range values sample the
	 * combined LUT, out-of-range and invalid values take their flag colour.
	 *
	 * This comment used to say "the legend, the probe readout and any CPU-side
	 * check use this, so there is one definition rather than one per consumer."
	 * THAT WAS FALSE, and it overstated what this function buys. There is no
	 * legend and no probe readout; the only caller outside this header is
	 * FlowVizTransferFunctionTest.cpp. One definition shared by one test is not
	 * the same guarantee as one definition shared by every consumer.
	 *
	 * It remains the right place to put that definition. But it is a CPU
	 * reference NOT CURRENTLY CHECKED AGAINST THE SHADER: nothing compares this
	 * against what FlowVizVolumeRayMarch.usf actually draws, so the two can drift
	 * apart and every test here will still pass.
	 *
	 * @param Lut A LUT built by BuildLut from THIS transfer function. Passing a LUT
	 *            from another one is not detectable here and gives wrong colours.
	 */
	FLOWVIZRUNTIME_API FLinearColor EvaluateColor(TArrayView<const FLinearColor> Lut, float Value) const;

	/**
	 * Check every field before anything is built or uploaded.
	 *
	 * Rejects: a LutSize outside MinLutSize..MaxLutSize; a non-finite or inverted
	 * domain (Max <= Min); a colormap outside the enum; negative bands; and
	 * anything FFlowVizOpacityCurve::Validate rejects.
	 *
	 * @return Ok, or a failure naming the offending quantity and its value
	 *         (engineering rule 12).
	 */
	FLOWVIZRUNTIME_API FCFDVizResult Validate() const;

	/** Value-equality, for change detection. Compares the curve too. */
	FLOWVIZRUNTIME_API bool Equals(const FFlowVizTransferFunction& Other, float Tolerance = UE_KINDA_SMALL_NUMBER) const;
};

/* -------------------------------------------------------------------------- */
/* LUT assembly                                                                 */
/* -------------------------------------------------------------------------- */

namespace FlowVizTransferFunction
{
	/**
	 * Build the combined colour+opacity LUT.
	 *
	 * RGB comes from CFDViz::ColorMaps::BuildLut - this function does not
	 * re-implement colormap sampling, because a second implementation is a second
	 * thing to drift from colormaps.py. Alpha comes from
	 * FFlowVizOpacityCurve::Evaluate at the SAME pixel-centre coordinate the
	 * colour used, `t = (i + 0.5) / N`, so a control point at t and a colour stop
	 * at t land on the same entry.
	 *
	 * ALPHA IS NOT PREMULTIPLIED. The ray-marcher composites front-to-back with
	 * its own step-size correction, which needs the un-premultiplied colour;
	 * premultiplying here would make every opacity change also change the hue that
	 * a Python reference figure of the same colormap renders.
	 *
	 * @param OutLut Resized to TransferFunction.LutSize. Emptied on failure, never
	 *               left half-written.
	 * @return Ok, or whatever FFlowVizTransferFunction::Validate rejected.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult BuildLut(
		const FFlowVizTransferFunction& TransferFunction,
		TArray<FLinearColor>& OutLut);

	/**
	 * Sample a LUT at a normalized coordinate the way a linear-filtered texture
	 * sampler does, with the PIXEL-CENTRE convention BuildLut used.
	 *
	 * Entry `i` sits at `t = (i + 0.5) / N`, so this maps `U` to the continuous
	 * texel coordinate `U*N - 0.5` and lerps the two neighbours, holding the end
	 * entries outside. That inverse is what makes the round trip exact: a LUT
	 * built at pixel centres and read back at pixel centres returns its own
	 * entries. Reading a pixel-centre LUT with the `i/(N-1)` convention instead
	 * shifts every sample by half a texel - the hue shift described at the top of
	 * this file.
	 *
	 * @param Lut Any LUT; empty yields black.
	 * @param U   Normalized coordinate. Outside [0,1] the end entries are held,
	 *            since the flag colours - not an extrapolated LUT - are what an
	 *            out-of-range value is supposed to get.
	 */
	FLOWVIZRUNTIME_API FLinearColor SampleLut(TArrayView<const FLinearColor> Lut, float U);

	/**
	 * Pack a LUT into the bytes FlowVizTransferFunction::LutPixelFormat wants -
	 * RGBA float16, little-endian, in entry order.
	 *
	 * float16 rather than 8-bit UNORM is a posterization decision, not a
	 * performance one; see the LutPixelFormat comment.
	 *
	 * @param OutBytes Resized to Lut.Num() * LutEntryBytes. Emptied on failure.
	 * @return Ok, or SizeMismatch for a LUT whose size is outside
	 *         MinLutSize..MaxLutSize.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult PackLutBytes(
		TArrayView<const FLinearColor> Lut,
		TArray<uint8>& OutBytes);

	/** Bytes a LUT of this many entries occupies once packed. INDEX_NONE for a size outside MinLutSize..MaxLutSize. */
	FLOWVIZRUNTIME_API int64 GetPackedBytes(int32 LutSize);
}

/* -------------------------------------------------------------------------- */
/* Shader parameters                                                            */
/* -------------------------------------------------------------------------- */

/**
 * The transfer-function half of the ray-marcher's constant buffer. Laid out for
 * a direct memcpy, in explicit 16-byte rows, with every offset pinned by a
 * static_assert at the bottom of this file.
 *
 * The offsets are asserted for the same reason FFlowVizVolumeShaderParameters'
 * are: this module is a unity build, so a sibling .cpp's constants share the
 * translation unit, and an unasserted offset is how a shader ends up reading the
 * opacity multiplier out of the range slot - which still renders, wrongly. See
 * Docs/BUILD.md.
 */
struct alignas(16) FFlowVizTransferFunctionShaderParameters
{
	/* row 0 */
	/** Domain minimum in solver units. For under-range colouring; the shader must not clamp to it unless bClampToRange. */
	float ValueRangeMin = 0.0f;
	/** Domain maximum, same caveat. */
	float ValueRangeMax = 1.0f;
	/** 1 / (Max - Min). Precomputed so the shader never divides per sample, and so a degenerate domain is caught here rather than as a per-pixel infinity. */
	float InvValueRange = 1.0f;
	/** Entries in the bound LUT texture. The shader needs it for the pixel-centre coordinate transform. */
	int32 LutSize = 0;

	/* row 1 */
	/** Global opacity multiplier. Already folded into the LUT's alpha; carried so the shader can apply a live slider without a texture rebuild. */
	float OpacityMultiplier = 1.0f;
	/** 1 when the shader should clamp out-of-range values into the domain instead of drawing the flag colours. */
	uint32 bClampToRange = 0;
	/** 0.5 / LutSize - the half-texel that turns a normalized value into a pixel-centre texture coordinate. Precomputed so the convention has one home. */
	float LutHalfTexel = 0.0f;
	/** (LutSize - 1) / LutSize - the span a normalized coordinate covers between the first and last texel centre. */
	float LutCoordScale = 0.0f;

	/* row 2 */
	/** Colour for a value below ValueRangeMin. */
	FLinearColor UnderRangeColor = FLinearColor(0.0f, 1.0f, 1.0f, 1.0f);

	/* row 3 */
	/** Colour for a value above ValueRangeMax. */
	FLinearColor OverRangeColor = FLinearColor(1.0f, 0.0f, 1.0f, 1.0f);

	/* row 4 */
	/** Colour for NaN or infinity. */
	FLinearColor NaNColor = FLinearColor(0.0f, 1.0f, 0.0f, 1.0f);

	/* row 5 */
	/** Colour for a masked cell. Distinct from NaNColor: the two are counted and reported separately. */
	FLinearColor MaskedColor = FLinearColor(0.0f, 0.55f, 0.0f, 1.0f);
};

namespace FlowVizTransferFunction
{
	/**
	 * Fill the shader parameter block.
	 *
	 * @param OutParams Written only on success, so a rejected transfer function
	 *                  cannot leave a half-populated constant buffer that renders
	 *                  a plausible wrong image.
	 * @return Ok, or whatever FFlowVizTransferFunction::Validate rejected.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult MakeShaderParameters(
		const FFlowVizTransferFunction& TransferFunction,
		FFlowVizTransferFunctionShaderParameters& OutParams);
}

/* -------------------------------------------------------------------------- */
/* Version-isolated RHI wrapper                                                 */
/* -------------------------------------------------------------------------- */

/**
 * THE ONLY CODE IN THIS FEATURE THAT CALLS A TEXTURE CREATE OR UPDATE API.
 *
 * The same isolation FlowVizVolumeRHI documents, for the same reason ADR 002
 * gives: RHI resource APIs churn between engine releases, and a shader problem
 * surfaces at runtime rather than at build time. Everything version-sensitive -
 * FRHITextureCreateDesc, FUpdateTextureRegion2D, the command-list plumbing -
 * lives in FlowVizTransferFunction.cpp, so an engine upgrade is a diff against
 * one file. Do not call RHICreateTexture or UpdateTexture2D anywhere else; the
 * isolation only holds if it is total.
 *
 * The .cpp carries a static_assert on ENGINE_MINOR_VERSION that fails on a newer
 * engine with instructions. That is intentional - an upgrade that silently
 * compiles against changed semantics is the failure being prevented.
 *
 * WHY A 2D Nx1 TEXTURE AND NOT A 1D ONE. RHI 1D textures are not creatable on
 * every platform this project targets and are not exposed through
 * FRHITextureCreateDesc::Create2D's guarantees; an Nx1 2D texture is universally
 * supported, samples identically with V fixed at 0.5, and costs nothing extra.
 * The shader samples it at (U, 0.5).
 *
 * WHAT IS NOT COVERED BY TESTS. These functions need a live RHI device, so the
 * suite's default -nullrhi run cannot exercise them; FlowViz.Render.
 * TransferFunctionDevice runs them only under RHI=1 and SKIPS WITH A LOGGED
 * REASON when it cannot. Specifically unverified without a device: that the
 * driver accepts PF_FloatRGBA as a sampleable 2D texture, that its row-pitch
 * interpretation matches ours, and that a sampled texel returns the half we
 * uploaded. Everything that decides WHICH bytes go where is pure and is covered.
 */
namespace FlowVizTransferFunctionRHI
{
	/**
	 * Is a LUT texture of this size creatable on the device actually present?
	 *
	 * @return Ok, or a failure naming the limit and the device's value. Returns Ok
	 *         when no RHI is initialised or the null RHI is in use, because a
	 *         null-RHI run has no device to fail against and must not report a
	 *         phantom incompatibility - the null RHI leaves the pixel-format
	 *         capability table empty, so judging a format against it would declare
	 *         every format unsupported.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult CheckDeviceSupport(int32 LutSize);

	/**
	 * Create the persistent LUT texture. Render thread only.
	 *
	 * Created ONCE and updated in place for the life of the transfer function -
	 * engineering rule 2, no per-frame resource recreation. Only a LutSize change
	 * is a reason to release and recreate.
	 *
	 * @return A null ref on failure, with the reason in OutResult. Never asserts on
	 *         a bad size; a malformed session must produce a message, not a crash.
	 */
	FLOWVIZRUNTIME_API FTextureRHIRef CreateLutTexture(
		FRHICommandListBase& RHICmdList,
		int32 LutSize,
		const TCHAR* DebugName,
		FCFDVizResult& OutResult);

	/**
	 * Overwrite the whole LUT texture. Render thread only.
	 *
	 * @param LutBytes Exactly GetPackedBytes(LutSize) bytes, as PackLutBytes
	 *                 produced. A short buffer is rejected before the RHI call
	 *                 rather than read past - UpdateTexture2D has no idea how long
	 *                 the source is and will walk off the end.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult UpdateLutTexture(
		FRHICommandListBase& RHICmdList,
		FRHITexture* Texture,
		int32 LutSize,
		TArrayView<const uint8> LutBytes);
}

/* -------------------------------------------------------------------------- */
/* The persistent resource                                                      */
/* -------------------------------------------------------------------------- */

/**
 * The transfer function's GPU resource: one persistent LUT texture, created once
 * and updated in place when the curve changes.
 *
 * NOT RECREATED PER FRAME, AND NOT RE-UPLOADED PER FRAME. Update compares the new
 * transfer function against the resident one and does nothing at all when they
 * are equal, so an idle viewport costs zero RHI work; when they differ it
 * enqueues one texture update, and it recreates the texture only when LutSize
 * itself changed. A transfer function is edited by a human dragging a control
 * point - tens of times a second at worst - so rebuilding the 2 KB LUT on the
 * game thread and handing over the bytes is correct and keeps the render thread
 * free of the colormap tables.
 *
 * LIFETIME. The texture is an RHI resource and may only be created, updated or
 * released on the render thread. ReleaseResources enqueues that work; the owner
 * must flush rendering commands before destroying the resource, exactly as any
 * FRenderResource owner must.
 */
class FLOWVIZRUNTIME_API FFlowVizTransferFunctionResource
{
public:
	FFlowVizTransferFunctionResource() = default;
	~FFlowVizTransferFunctionResource();

	// Owns an RHI reference; copying one would double-release it.
	FFlowVizTransferFunctionResource(const FFlowVizTransferFunctionResource&) = delete;
	FFlowVizTransferFunctionResource& operator=(const FFlowVizTransferFunctionResource&) = delete;

	/**
	 * Set or change the transfer function.
	 *
	 * Validates it, rebuilds the LUT on the calling thread, and enqueues at most
	 * one render command. An identical transfer function is a no-op: no LUT build,
	 * no command, no upload. That is what makes this safe to call every frame from
	 * a UI that does not track its own dirtiness.
	 *
	 * @return Ok - including for the no-op case - or a validation failure, in which
	 *         case NOTHING changes: the resident texture keeps its previous
	 *         contents rather than being left half-updated.
	 */
	FCFDVizResult Update(const FFlowVizTransferFunction& InTransferFunction);

	/** The resident transfer function. Default-constructed before the first successful Update. */
	const FFlowVizTransferFunction& GetTransferFunction() const
	{
		return TransferFunction;
	}

	/**
	 * The CPU-side LUT the resident texture was built from. Empty before the
	 * first successful Update.
	 *
	 * This comment used to claim "this is what the legend and the probe readout
	 * sample." THAT WAS FALSE. Neither a legend nor a probe readout exists
	 * anywhere in this plugin, and every caller of this accessor is in
	 * FlowVizTransferFunctionTest.cpp. Grep before relying on the claim; a
	 * consumer that does not exist cannot corroborate the LUT.
	 *
	 * Why the correction matters more than the accessor does: the same sentence
	 * is load-bearing at FlowVizTransferFunction.cpp:875, where "the CPU-side LUT
	 * is still correct and is what the legend and the probe readout sample" is
	 * the stated reason NOT to log a missing LUT texture. A reassurance about an
	 * imaginary consumer is buying silence on a real GPU failure path.
	 */
	TArrayView<const FLinearColor> GetLut() const
	{
		return Lut;
	}

	/** The LUT texture, or null before the first upload completes or when no RHI exists. */
	FRHITexture* GetLutTexture() const
	{
		return LutTexture.GetReference();
	}

	/** True once at least one successful Update has built a LUT. False does NOT mean the texture is invalid - it means nothing has been set yet. */
	bool HasContent() const
	{
		return Lut.Num() > 0;
	}

	/**
	 * How many times a LUT rebuild has actually happened.
	 *
	 * Exposed so a test can prove the no-op path is a no-op. A resource that
	 * silently rebuilt every frame would pass every correctness assertion while
	 * violating engineering rule 2, and this counter is the only way to tell the
	 * difference from outside.
	 */
	int32 GetBuildCount() const
	{
		return BuildCount;
	}

	/**
	 * How many texture-create commands have been enqueued. A resource that
	 * recreates rather than updating in place shows up here as a climbing count.
	 *
	 * Counted from the GAME thread's decision - "did LutSize change?" - not from
	 * whether an RHI texture currently exists. Counting the latter would make this
	 * climb on every single Update under -nullrhi, where no texture is ever
	 * created, and the counter would then report a rule-2 violation on a resource
	 * that has none.
	 */
	int32 GetTextureCreateCount() const
	{
		return TextureCreateCount;
	}

	/** Shader parameters for the resident transfer function. Fails if nothing has been set. */
	FCFDVizResult MakeShaderParameters(FFlowVizTransferFunctionShaderParameters& OutParams) const;

	/** Enqueue release of the texture. Safe when nothing was created. Flush rendering commands before destroying the resource. */
	void ReleaseResources();

private:
	/** Render-thread body of Update. */
	void UploadOnRenderThread(FRHICommandListBase& RHICmdList, int32 InLutSize, TArray<uint8>&& LutBytes);

	FFlowVizTransferFunction TransferFunction;
	TArray<FLinearColor> Lut;
	FTextureRHIRef LutTexture;

	/** Size the resident texture was created at. Render-thread state; a change is the only reason to recreate rather than update. */
	int32 TextureLutSize = 0;

	/** Size the LAST enqueued create used, tracked on the game thread so TextureCreateCount does not depend on render-thread timing or on a device existing. */
	int32 EnqueuedLutSize = 0;

	int32 BuildCount = 0;
	int32 TextureCreateCount = 0;
};

/* -------------------------------------------------------------------------- */
/* Constant-buffer layout assertions                                            */
/* -------------------------------------------------------------------------- */

/*
 * These pin FFlowVizTransferFunctionShaderParameters against the HLSL constant
 * buffer the ray-marcher declares, for the reason given on the struct: in a unity
 * build a sibling's constants share this translation unit, and a member that
 * drifts produces a shader reading the opacity multiplier out of the range slot -
 * a wrong image that still renders. Docs/BUILD.md, commit 9d5d5ac.
 */
static_assert(sizeof(FLinearColor) == 16, "FFlowVizTransferFunctionShaderParameters assumes a packed 4-float colour.");

static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, ValueRangeMin) == 0, "cbuffer row 0");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, ValueRangeMax) == 4, "cbuffer row 0");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, InvValueRange) == 8, "cbuffer row 0");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, LutSize) == 12, "cbuffer row 0");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, OpacityMultiplier) == 16, "cbuffer row 1");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, bClampToRange) == 20, "cbuffer row 1");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, LutHalfTexel) == 24, "cbuffer row 1");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, LutCoordScale) == 28, "cbuffer row 1");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, UnderRangeColor) == 32, "cbuffer row 2");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, OverRangeColor) == 48, "cbuffer row 3");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, NaNColor) == 64, "cbuffer row 4");
static_assert(offsetof(FFlowVizTransferFunctionShaderParameters, MaskedColor) == 80, "cbuffer row 5");
static_assert(sizeof(FFlowVizTransferFunctionShaderParameters) == 96, "cbuffer total: six 16-byte rows.");
