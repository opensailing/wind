// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizColorMaps.h"
#include "CFDViz/CFDVizManifest.h"
#include "CFDViz/CFDVizTypes.h"
#include "Render/FlowVizTransferFunction.h"
#include "Render/FlowVizVolumeRayMarchShader.h"

/**
 * Colouring: which field, which component, which map, over which range
 * (plan.md sections 10.3 and 5F).
 *
 * WHAT THIS OWNS AND WHAT IT DOES NOT. It owns the CHOICE - a field id, a
 * component, a colormap, a range mode - and the rules that make one choice legal
 * given another. It does not own the mapping: the moment a choice is made it is
 * expressed as an FFlowVizTransferFunction, and every subsequent question about
 * colour, opacity, LUT layout or shader parameters is answered by the existing
 * Render/FlowVizTransferFunction.* code. A second implementation of "what colour
 * is this value" is the drift that makes two figures of the same data disagree.
 *
 * ENGINEERING RULE 8 IS THE DEFAULT HERE, NOT A SETTING. "Stable global color
 * ranges must be the default for animations." A fresh view model bound to a
 * field takes that field's manifest-declared global range, so an animation does
 * not pulse as the range rescales per frame. Per-frame ranging is available and
 * REPORTS ITSELF - see IsRangeStableAcrossAnimation - so the workspace can label
 * it, which is the rest of rule 8.
 *
 * RULE 15 AND THE PER-FRAME RANGE. Nothing in this codebase computes a
 * per-frame range yet, so EFlowVizRangeSource::CurrentFrame is REFUSED until a
 * caller has supplied one through SetCurrentFrameRange. That is rule 15 stated
 * in the model rather than in a widget: a control whose producer does not exist
 * reports itself unavailable instead of selecting a mode that silently behaves
 * like Global.
 *
 * DIVERGING MAPS AND ZERO. Resetting the range routes through
 * FFlowVizTransferFunction::MakeDefaultDomain, so the "centre a diverging map on
 * zero" rule (VISUAL_QA rule 7) has one implementation. Pressure in the shipped
 * sample is coolwarm over [-62.9375, 27.6875]; the reset range is therefore
 * +/-62.9375 and NOT the data midpoint, which would put white at -17.6.
 *
 * THREADING. Game thread. Holds no RHI resource: it produces the value type,
 * and FFlowVizTransferFunctionResource - which does own GPU state - consumes it.
 */

/** Where the colour range comes from before and after the user touches it (plan.md 10.3). */
enum class EFlowVizRangeSource : uint8
{
	/**
	 * The field's manifest-declared global statistics. Stable across the whole
	 * animation, which engineering rule 8 makes the default.
	 */
	Global = 0,
	/**
	 * The current frame's own range. Maximises per-frame contrast at the cost of
	 * frame-to-frame comparability, so rule 8 requires it to be labelled.
	 * Selectable only once SetCurrentFrameRange has supplied one.
	 */
	CurrentFrame = 1,
	/** Typed by the user, or the manifest's recommendedRange. */
	Manual = 2,
};

/** Which scalar a vector field is coloured by. Mirrors EFlowVizComponentMode, which is what the shader reads. */
enum class EFlowVizComponentChoice : uint8
{
	X = 0,
	Y = 1,
	Z = 2,
	W = 3,
	Magnitude = 4,
};

class FLOWVIZRUNTIME_API FFlowVizTransferFunctionViewModel
{
public:
	FFlowVizTransferFunctionViewModel();

	/* --- Field binding ---------------------------------------------------- */

	/**
	 * Bind a field of an open case and adopt its display defaults.
	 *
	 * Adopts, in this order: the manifest's defaultColorMap when it names a map
	 * this build knows; the manifest's defaultComponent; and a range from the
	 * field's global statistics, widened to a zero-centred domain when the map
	 * diverges.
	 *
	 * A field with NO declared statistics does not fall back to [0,1] silently -
	 * it reports IsRangeKnown() == false, so the workspace can say "range
	 * unknown" rather than colour the field against a fabricated domain.
	 *
	 * @return A failure naming the field when the case declares no such field.
	 *         Nothing is changed on failure.
	 */
	FCFDVizResult BindField(const FCFDVizCase& Case, FName FieldId);

	/** Forget the field, keeping no stale range. */
	void Unbind();

	bool IsBound() const { return bBound; }
	FName GetFieldId() const { return FieldId; }
	int32 GetComponentCount() const { return ComponentCount; }

	/** True when a real declared or supplied range backs the current domain. */
	bool IsRangeKnown() const { return bRangeKnown; }

	/* --- Component -------------------------------------------------------- */

	/**
	 * @return A failure when the component does not exist on this field - X on a
	 *         1-component field is refused rather than silently coloured, because
	 *         a scalar has no Y and colouring it as though it did shows zeros.
	 *         Magnitude is legal for any component count.
	 */
	FCFDVizResult SetComponent(EFlowVizComponentChoice Component);
	EFlowVizComponentChoice GetComponent() const { return Component; }

	/** The shader's own enum for the same choice - this is what reaches FFlowVizVolumeRayMarchParameters::ComponentMode. */
	EFlowVizComponentMode GetShaderComponentMode() const;

	/* --- Colormap --------------------------------------------------------- */

	/**
	 * Choose a map. Changing to or from a diverging map does NOT re-derive the
	 * range: a user who typed a domain keeps it. ResetRange() is the explicit
	 * way to ask for the map's preferred domain.
	 */
	FCFDVizResult SetColorMap(ECFDVizColorMap Map);
	ECFDVizColorMap GetColorMap() const { return TransferFunction.ColorMap; }

	void SetReverseColorMap(bool bReverse);
	bool IsColorMapReversed() const { return TransferFunction.bReverseColorMap; }

	/** @param Bands 0 for a continuous map, otherwise the number of discrete bands. Negative is refused. */
	FCFDVizResult SetColorBands(int32 Bands);
	int32 GetColorBands() const { return TransferFunction.ColorBands; }

	/** False for Turbo and the rainbow-adjacent maps. The workspace must mark such a selection (CFDViz::ColorMaps::IsPerceptuallyUniform). */
	bool IsColorMapPerceptuallyUniform() const;

	/* --- Range ------------------------------------------------------------ */

	/**
	 * Switch where the range comes from and apply it immediately.
	 *
	 * @return A failure for CurrentFrame when no per-frame range has been
	 *         supplied. Rule 15: the control is unavailable rather than
	 *         quietly equivalent to Global.
	 */
	FCFDVizResult SetRangeSource(EFlowVizRangeSource Source);
	EFlowVizRangeSource GetRangeSource() const { return RangeSource; }

	/** Selecting Manual is always legal; this is what a typed range does. Inverted or non-finite bounds are refused. */
	FCFDVizResult SetManualRange(float Min, float Max);

	/**
	 * Supply the range measured over the frame currently on screen.
	 *
	 * Until something calls this, EFlowVizRangeSource::CurrentFrame is refused.
	 * Nothing in the shipping code calls it yet, which is exactly why the refusal
	 * exists rather than a silent fallback.
	 */
	FCFDVizResult SetCurrentFrameRange(float Min, float Max);
	bool HasCurrentFrameRange() const { return bHasFrameRange; }

	/**
	 * Return to the range this field's declared statistics imply, through
	 * FFlowVizTransferFunction::MakeDefaultDomain - so a diverging map re-centres
	 * on zero rather than on the data midpoint.
	 */
	FCFDVizResult ResetRange();

	float GetRangeMin() const { return TransferFunction.ValueRangeMin; }
	float GetRangeMax() const { return TransferFunction.ValueRangeMax; }

	/** Engineering rule 8's disclosure: false for CurrentFrame, true otherwise. */
	bool IsRangeStableAcrossAnimation() const { return RangeSource != EFlowVizRangeSource::CurrentFrame; }

	/* --- Opacity ---------------------------------------------------------- */

	/** @return A failure - and no change - for anything FFlowVizOpacityCurve::Validate rejects. */
	FCFDVizResult SetOpacityCurve(const FFlowVizOpacityCurve& Curve);
	const FFlowVizOpacityCurve& GetOpacityCurve() const { return TransferFunction.Opacity; }

	/**
	 * Global scale on the opacity curve, applied by the renderer.
	 *
	 * NOT part of FFlowVizTransferFunction, deliberately: the curve describes the
	 * DATA - which values are transparent - and the multiplier describes the
	 * VIEW, how dense this particular render should look. Baking it into the
	 * curve would make the LUT depend on a viewing preference and force a rebuild
	 * on every slider tick.
	 */
	FCFDVizResult SetOpacityMultiplier(float Multiplier);
	float GetOpacityMultiplier() const { return OpacityMultiplier; }

	/* --- Invalid-value colours (rule 10, VISUAL_QA rule 4) ---------------- */

	void SetUnderRangeColor(const FLinearColor& Color);
	void SetOverRangeColor(const FLinearColor& Color);
	void SetNaNColor(const FLinearColor& Color);
	void SetMaskedColor(const FLinearColor& Color);

	/**
	 * Clamp out-of-range values into the domain instead of colouring them
	 * distinctly.
	 *
	 * COLOUR ONLY, ON BOTH SIDES. It reaches the CPU mapping through
	 * FFlowVizTransferFunction::EvaluateColor and the GPU through
	 * FFlowVizVolumeRayMarchParameters::bClampToRange, which
	 * ApplyToRayMarchParameters writes. What it must NOT reach is the
	 * classification: OutValue.x and the reason bits are identical either way,
	 * because a display choice that also moved the reported number would turn
	 * "this value is off the scale" into a quantitative lie.
	 */
	void SetClampToRange(bool bClamp);
	bool IsClampToRange() const { return TransferFunction.bClampToRange; }

	/* --- The value the render layer consumes ------------------------------ */

	/**
	 * The transfer function as the LUT builder and the shader parameter packer
	 * want it. This is the whole output of this view model.
	 */
	const FFlowVizTransferFunction& GetTransferFunction() const { return TransferFunction; }

	/**
	 * Write the colouring choices into the ray-march parameter block - the
	 * volume renderer's actual constant buffer.
	 *
	 * Writes ValueRangeMin/Max, ComponentMode, OpacityMultiplier, bClampToRange
	 * and the four invalid-value colours, because those members exist in the
	 * block and are read by the shader.
	 *
	 * bClampToRange IS WRITTEN, AND THAT IS RECENT. The ray-march block grew the
	 * member; before it did, this function had nowhere to put the flag and said
	 * so. If it stopped being written, the user's clamp choice would stop HERE -
	 * one layer above the gap that used to exist, and harder to find, because a
	 * passing shader-level test would sit next to it attesting that the plumbing
	 * works. FlowViz.UI.TransferFunctionViewModel.Consumer asserts it
	 * differentially rather than reading back the field this function assigns.
	 *
	 * NoDataColor is also left alone: it has no counterpart in
	 * FFlowVizTransferFunction, so it belongs to whoever fills the defaults.
	 *
	 * @return A failure, leaving OutParameters untouched, when the current
	 *         choices do not Validate.
	 */
	FCFDVizResult ApplyToRayMarchParameters(FFlowVizVolumeRayMarchParameters& OutParameters) const;

	/** Ok when the current choices form a transfer function the render layer will accept. */
	FCFDVizResult Validate() const { return TransferFunction.Validate(); }

private:
	FCFDVizResult ApplyRangeFromSource();

	FFlowVizTransferFunction TransferFunction;

	bool bBound = false;
	FName FieldId;
	int32 ComponentCount = 1;
	EFlowVizComponentChoice Component = EFlowVizComponentChoice::Magnitude;

	EFlowVizRangeSource RangeSource = EFlowVizRangeSource::Global;

	/** The field's declared global range, before any zero-centring. */
	bool bRangeKnown = false;
	float GlobalMin = 0.0f;
	float GlobalMax = 1.0f;

	bool bHasFrameRange = false;
	float FrameMin = 0.0f;
	float FrameMax = 1.0f;

	float ManualMin = 0.0f;
	float ManualMax = 1.0f;

	/** Matches FlowVizRayMarch::FillDefaults, so applying a fresh view model is a no-op on this member. */
	float OpacityMultiplier = 1.0f;
};
