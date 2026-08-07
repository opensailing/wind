// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizTypes.h"
#include "Render/FlowVizVolumeRayMarchShader.h"

/**
 * The render controls: how the ray-marcher composites, lights and steps
 * (plan.md sections 10.1 and 10.4).
 *
 * WHY THIS CLASS EXISTS. Until it did, `FlowVizRayMarch::FillDefaults` was the
 * ONLY non-test writer of sixteen shader parameters, and it writes a literal
 * constant to each. `CompositeMode` was welded to `Alpha`, so five of the six
 * modes the shader implements could not be selected in a shipped build - four of
 * them required by plan.md section 9. `bEnableLighting` was welded to 0, so the
 * whole FlowVizGradient path in the .usf was dead, taking AmbientStrength,
 * DiffuseStrength and LightDirection with it. Every one of those branches was
 * covered by a passing test, because a test that writes `Params->CompositeMode`
 * itself cannot observe that nothing else does. Docs/BACKLOG.md item 2e.
 *
 * THE DEFAULTS ARE NOT THE BUG AND MUST SURVIVE. Jitter is off because per-ray
 * jitter causes temporal shimmer (ADR 002). The render is unlit because
 * VISUAL_QA rule 1 forbids lighting from modulating apparent scalar value. A
 * default-constructed view model therefore changes NOTHING: it is an identity
 * over FillDefaults, and FlowViz.UI.RenderSettings asserts that first, before
 * any other assertion, because it is the control for all of them. What was wrong
 * was that those values were the only REACHABLE ones.
 *
 * APPLIED AFTER FillDefaults, AND IT MUTATES RATHER THAN REPLACES. The dispatcher
 * calls FillDefaults, then FillFromVolumeParameters, then SetVolumeTextures, then
 * the camera - so a settings source that assigned a fresh struct would erase the
 * geometry, format and camera rows and render a black screen. Every setter here
 * writes exactly the fields it owns.
 *
 * THE CLAMP IS RE-APPLIED, NOT INHERITED. FillDefaults ends by clamping MaxSteps
 * into [1, MaxStepsLimit] so that no caller can ask the GPU for an unbounded
 * loop. Anything writing MaxSteps AFTER that point reopens precisely the hole the
 * clamp closed, so SetMaxSteps clamps on the way in and ApplyToRayMarchParameters
 * clamps again on the way out.
 *
 * REFUSAL, NOT CORRECTION, FOR VALUES THAT WOULD RENDER PLAUSIBLY. A zero step
 * size, a zero light direction, a NaN iso value and a zero reference step all
 * produce a render that looks like a data problem rather than a rejected input:
 * a smeared volume, a uniformly ambient one, an empty iso-surface, a division by
 * zero in the opacity correction. Each setter returns false and keeps the
 * previous value, so a control that was given a bad number does not silently
 * become a control that does nothing (rule 15).
 *
 * THREADING. Game thread, pure value state, no RHI, no case reference - the same
 * contract as FFlowVizClipViewModel and FFlowVizTransferFunctionViewModel.
 */
struct FLOWVIZRUNTIME_API FFlowVizRenderSettingsViewModel
{
public:
	/* --- Compositing -------------------------------------------------------- */

	/** Select a composite mode. Every value of the enum is selectable; that is the point of this class. */
	void SetCompositeMode(EFlowVizCompositeMode InMode);

	/**
	 * Select a composite mode from a raw value, e.g. a console command or a
	 * saved session.
	 *
	 * REFUSED WHEN OUT OF RANGE, and the previous mode is kept. The .usf
	 * switches on this value; an unrecognised one falls through to a default
	 * branch and renders as a mode nobody selected, which reads as a broken
	 * shader rather than a rejected input.
	 *
	 * @return false when the value is not a member of EFlowVizCompositeMode.
	 */
	bool SetCompositeModeByValue(uint32 InValue);

	EFlowVizCompositeMode GetCompositeMode() const
	{
		return CompositeMode;
	}

	/**
	 * The iso-surface threshold, in field units. Only read in IsoSurface mode.
	 *
	 * @return false for a non-finite value, keeping the previous one. NaN
	 *         compares false against everything, so an iso-surface at NaN finds
	 *         no crossing and renders empty - the same picture as a threshold
	 *         outside the data range, and a different fix.
	 */
	bool SetIsoValue(float InValue);

	float GetIsoValue() const
	{
		return IsoValue;
	}

	/* --- Lighting ----------------------------------------------------------- */

	/**
	 * Turn gradient lighting on.
	 *
	 * OFF BY DEFAULT AND THAT IS DELIBERATE: VISUAL_QA rule 1 requires that
	 * lighting not modulate apparent scalar value, so the Scientific profile
	 * renders unlit. This control exists for the Presentation profile, where the
	 * trade is made knowingly.
	 */
	void SetLightingEnabled(bool bInEnabled);

	bool IsLightingEnabled() const
	{
		return bEnableLighting;
	}

	/** Ambient term. Clamped to [0, 1]; outside that it is a brightness bug, not a lighting choice. */
	void SetAmbientStrength(float InStrength);

	/** Diffuse term. Clamped to [0, 1]. */
	void SetDiffuseStrength(float InStrength);

	float GetAmbientStrength() const
	{
		return AmbientStrength;
	}

	float GetDiffuseStrength() const
	{
		return DiffuseStrength;
	}

	/**
	 * Light direction, LOCAL volume space, solver axes.
	 *
	 * NORMALISED ON THE WAY IN. The shader dots this against a unit gradient, so
	 * an unnormalised direction scales the diffuse term - (0,0,10) would be ten
	 * times as bright as (0,0,1) pointing the same way, an aim control that is
	 * secretly a brightness control.
	 *
	 * @return false for a zero or non-finite direction, keeping the previous
	 *         one. Normalising zero yields zero, and a dot product against that
	 *         is 0 everywhere: a uniformly ambient render that reads as
	 *         "lighting does nothing".
	 */
	bool SetLightDirection(const FVector3f& InDirection);

	FVector3f GetLightDirection() const
	{
		return LightDirection;
	}

	/* --- Marching ----------------------------------------------------------- */

	/**
	 * Sample step in VOXEL units. Smaller is more accurate and costs fill rate.
	 *
	 * @return false for a non-positive or non-finite step, keeping the previous
	 *         one. At zero the ray never advances and the volume renders as its
	 *         first sample smeared over every step.
	 */
	bool SetStepVoxels(float InStepVoxels);

	/**
	 * The opacity-correction reference step.
	 *
	 * Alpha is compensated by StepVoxels/ReferenceStepVoxels so that changing the
	 * step size changes the noise rather than the apparent density.
	 *
	 * @return false for a non-positive or non-finite value - it is a divisor.
	 */
	bool SetReferenceStepVoxels(float InReferenceStepVoxels);

	float GetStepVoxels() const
	{
		return StepVoxels;
	}

	float GetReferenceStepVoxels() const
	{
		return ReferenceStepVoxels;
	}

	/** Step ceiling. CLAMPED to [1, FlowVizRayMarch::MaxStepsLimit] here and again on apply - see the class comment. */
	void SetMaxSteps(uint32 InMaxSteps);

	uint32 GetMaxSteps() const
	{
		return MaxSteps;
	}

	/** Alpha at which the march stops early. Clamped to [0, 1]; 1.0 never triggers, which is reference quality. */
	void SetEarlyTerminationAlpha(float InAlpha);

	float GetEarlyTerminationAlpha() const
	{
		return EarlyTerminationAlpha;
	}

	/* --- Jitter ------------------------------------------------------------- */

	/** Sub-step jitter. Off by default per ADR 002 - it trades banding for temporal shimmer. */
	void SetJitterEnabled(bool bInEnabled);

	/** Jitter magnitude as a fraction of one step. Clamped to [0, 1]; never changes the step COUNT. */
	void SetJitterAmount(float InAmount);

	/** Jitter seed. Any value is valid; a fixed seed is what makes a capture reproducible. */
	void SetJitterSeed(uint32 InSeed);

	bool IsJitterEnabled() const
	{
		return bEnableJitter;
	}

	float GetJitterAmount() const
	{
		return JitterAmount;
	}

	uint32 GetJitterSeed() const
	{
		return JitterSeed;
	}

	/* --- Sampling ----------------------------------------------------------- */

	/** Trilinear field filtering. On by default. Off gives nearest-neighbour, which is what a voxel-accurate readout wants. */
	void SetFieldFilteringEnabled(bool bInEnabled);

	/**
	 * Reject a filtered sample whose trilinear footprint touches an invalid
	 * voxel.
	 *
	 * The difference between a correct edge and a plausible one that has bled
	 * invalid data into it. Off by default because it thins the volume at every
	 * boundary, which is a visible change and the user's call.
	 */
	void SetStrictStatusFilter(bool bInStrict);

	bool IsFieldFilteringEnabled() const
	{
		return bFilterField;
	}

	bool IsStrictStatusFilter() const
	{
		return bStrictStatusFilter;
	}

	/* --- Disclosure --------------------------------------------------------- */

	/**
	 * The colour for a voxel that carries no data at all. Transparent by default.
	 *
	 * PART OF THE DISCLOSURE PALETTE. VISUAL_QA rule 4 requires NaN, masked,
	 * no-data, under-range and over-range to remain distinguishable; collapsing
	 * any two is the failure that rule names. The other four belong to
	 * FFlowVizTransferFunctionViewModel, which is where that constraint is
	 * enforced across the set - this one has no counterpart in
	 * FFlowVizTransferFunction, which is why it lives here.
	 */
	void SetNoDataColor(const FLinearColor& InColor);

	FLinearColor GetNoDataColor() const
	{
		return NoDataColor;
	}

	/* --- Apply -------------------------------------------------------------- */

	/**
	 * Write these settings over an already-defaulted parameter block.
	 *
	 * MUTATES. Called after FillDefaults, FillFromVolumeParameters,
	 * SetVolumeTextures and the camera, so every field this does not own must be
	 * left exactly as found. FlowViz.UI.RenderSettings asserts that with a
	 * marker value rather than by inspection.
	 */
	void ApplyToRayMarchParameters(FFlowVizVolumeRayMarchParameters& OutParameters) const;

private:
	/*
	 * INITIALISED TO THE SAME CONSTANTS FillDefaults WRITES, so that a
	 * default-constructed view model is an identity over it. Duplicating the
	 * values is a real hazard - two sets of defaults that can drift - and the
	 * identity test in FlowViz.UI.RenderSettings is what catches the drift,
	 * because it compares against FillDefaults' output rather than against these
	 * literals. Where a named constant exists it is used instead of a literal.
	 */
	EFlowVizCompositeMode CompositeMode = EFlowVizCompositeMode::Alpha;
	float IsoValue = 0.0f;

	bool bEnableLighting = false;
	float AmbientStrength = 0.35f;
	float DiffuseStrength = 0.65f;
	FVector3f LightDirection = FVector3f(-0.5f, -0.6f, -0.6f).GetSafeNormal();

	float StepVoxels = FlowVizRayMarch::DefaultStepVoxels;
	float ReferenceStepVoxels = FlowVizRayMarch::DefaultReferenceStepVoxels;
	uint32 MaxSteps = FlowVizRayMarch::DefaultMaxSteps;
	float EarlyTerminationAlpha = FlowVizRayMarch::DefaultEarlyTerminationAlpha;

	/*
	 * ON BY DEFAULT since P6: the jitter trades banding for grain the
	 * temporal AA resolves, and the profile system stops toggling it --
	 * committing to good defaults instead of exposing switches is the
	 * research doc's "FluidX3D ships zero lighting toggles" lesson.
	 */
	bool bEnableJitter = true;
	float JitterAmount = 1.0f;
	uint32 JitterSeed = 0u;

	bool bFilterField = true;
	bool bStrictStatusFilter = false;

	FLinearColor NoDataColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
};
