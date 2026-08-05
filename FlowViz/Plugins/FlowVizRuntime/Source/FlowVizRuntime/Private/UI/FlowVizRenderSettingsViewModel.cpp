// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizRenderSettingsViewModel.h"

namespace
{
	/**
	 * Is this a member of EFlowVizCompositeMode?
	 *
	 * WRITTEN AS AN EXHAUSTIVE SWITCH, NOT AS `Value <= Diagnostic`. A range
	 * check silently accepts whatever is added next: a seventh mode declared in
	 * the header but not yet handled by the .usf would pass validation here and
	 * render as the shader's default branch. The switch stops compiling instead,
	 * which is the correct place to find out.
	 */
	bool IsKnownCompositeMode(uint32 Value)
	{
		switch (static_cast<EFlowVizCompositeMode>(Value))
		{
		case EFlowVizCompositeMode::Alpha:
		case EFlowVizCompositeMode::Maximum:
		case EFlowVizCompositeMode::Minimum:
		case EFlowVizCompositeMode::Average:
		case EFlowVizCompositeMode::IsoSurface:
		case EFlowVizCompositeMode::Diagnostic:
			return true;
		}
		return false;
	}
}

/* -------------------------------------------------------------------------- */
/* Compositing                                                                  */
/* -------------------------------------------------------------------------- */

void FFlowVizRenderSettingsViewModel::SetCompositeMode(EFlowVizCompositeMode InMode)
{
	CompositeMode = InMode;
}

bool FFlowVizRenderSettingsViewModel::SetCompositeModeByValue(uint32 InValue)
{
	if (!IsKnownCompositeMode(InValue))
	{
		return false;
	}

	CompositeMode = static_cast<EFlowVizCompositeMode>(InValue);
	return true;
}

bool FFlowVizRenderSettingsViewModel::SetIsoValue(float InValue)
{
	// Finite, not merely non-NaN: an infinite threshold is never crossed either.
	if (!FMath::IsFinite(InValue))
	{
		return false;
	}

	IsoValue = InValue;
	return true;
}

/* -------------------------------------------------------------------------- */
/* Lighting                                                                     */
/* -------------------------------------------------------------------------- */

void FFlowVizRenderSettingsViewModel::SetLightingEnabled(bool bInEnabled)
{
	bEnableLighting = bInEnabled;
}

void FFlowVizRenderSettingsViewModel::SetAmbientStrength(float InStrength)
{
	// A non-finite strength would propagate through the whole shaded colour, so
	// it is dropped rather than clamped -- FMath::Clamp of NaN returns NaN.
	if (!FMath::IsFinite(InStrength))
	{
		return;
	}

	AmbientStrength = FMath::Clamp(InStrength, 0.0f, 1.0f);
}

void FFlowVizRenderSettingsViewModel::SetDiffuseStrength(float InStrength)
{
	if (!FMath::IsFinite(InStrength))
	{
		return;
	}

	DiffuseStrength = FMath::Clamp(InStrength, 0.0f, 1.0f);
}

bool FFlowVizRenderSettingsViewModel::SetLightDirection(const FVector3f& InDirection)
{
	if (!InDirection.ContainsNaN())
	{
		// GetSafeNormal returns the zero vector when the input is too small to
		// normalise, which is precisely the case that must be refused: a zero
		// direction dots to 0 everywhere and renders uniformly ambient.
		const FVector3f Normalized = InDirection.GetSafeNormal();
		if (!Normalized.IsNearlyZero())
		{
			LightDirection = Normalized;
			return true;
		}
	}

	return false;
}

/* -------------------------------------------------------------------------- */
/* Marching                                                                     */
/* -------------------------------------------------------------------------- */

bool FFlowVizRenderSettingsViewModel::SetStepVoxels(float InStepVoxels)
{
	// `> 0` is false for NaN, so this rejects it -- but stating IsFinite
	// separately keeps the intent readable and also catches +inf, which passes
	// a bare positivity test and marches nowhere.
	if (!FMath::IsFinite(InStepVoxels) || InStepVoxels <= 0.0f)
	{
		return false;
	}

	StepVoxels = InStepVoxels;
	return true;
}

bool FFlowVizRenderSettingsViewModel::SetReferenceStepVoxels(float InReferenceStepVoxels)
{
	if (!FMath::IsFinite(InReferenceStepVoxels) || InReferenceStepVoxels <= 0.0f)
	{
		return false;
	}

	ReferenceStepVoxels = InReferenceStepVoxels;
	return true;
}

void FFlowVizRenderSettingsViewModel::SetMaxSteps(uint32 InMaxSteps)
{
	// Clamped HERE and again in ApplyToRayMarchParameters. FillDefaults ends
	// with this same clamp specifically so no caller can hand the GPU an
	// unbounded loop; a settings source that writes MaxSteps afterwards would
	// reopen that hole unless it clamps too.
	MaxSteps = FMath::Clamp(InMaxSteps, 1u, FlowVizRayMarch::MaxStepsLimit);
}

void FFlowVizRenderSettingsViewModel::SetEarlyTerminationAlpha(float InAlpha)
{
	if (!FMath::IsFinite(InAlpha))
	{
		return;
	}

	EarlyTerminationAlpha = FMath::Clamp(InAlpha, 0.0f, 1.0f);
}

/* -------------------------------------------------------------------------- */
/* Jitter                                                                       */
/* -------------------------------------------------------------------------- */

void FFlowVizRenderSettingsViewModel::SetJitterEnabled(bool bInEnabled)
{
	bEnableJitter = bInEnabled;
}

void FFlowVizRenderSettingsViewModel::SetJitterAmount(float InAmount)
{
	if (!FMath::IsFinite(InAmount))
	{
		return;
	}

	JitterAmount = FMath::Clamp(InAmount, 0.0f, 1.0f);
}

void FFlowVizRenderSettingsViewModel::SetJitterSeed(uint32 InSeed)
{
	JitterSeed = InSeed;
}

/* -------------------------------------------------------------------------- */
/* Sampling                                                                     */
/* -------------------------------------------------------------------------- */

void FFlowVizRenderSettingsViewModel::SetFieldFilteringEnabled(bool bInEnabled)
{
	bFilterField = bInEnabled;
}

void FFlowVizRenderSettingsViewModel::SetStrictStatusFilter(bool bInStrict)
{
	bStrictStatusFilter = bInStrict;
}

/* -------------------------------------------------------------------------- */
/* Disclosure                                                                   */
/* -------------------------------------------------------------------------- */

void FFlowVizRenderSettingsViewModel::SetNoDataColor(const FLinearColor& InColor)
{
	NoDataColor = InColor;
}

/* -------------------------------------------------------------------------- */
/* Apply                                                                        */
/* -------------------------------------------------------------------------- */

void FFlowVizRenderSettingsViewModel::ApplyToRayMarchParameters(
	FFlowVizVolumeRayMarchParameters& OutParameters) const
{
	// FIELD BY FIELD, NEVER `OutParameters = ...`. This runs after the geometry,
	// format, texture and camera rows have been filled; assigning a fresh struct
	// would erase all of them and render a black screen while every
	// single-setting test above still passed.
	OutParameters.CompositeMode = static_cast<uint32>(CompositeMode);
	OutParameters.IsoValue = IsoValue;

	OutParameters.bEnableLighting = bEnableLighting ? 1u : 0u;
	OutParameters.AmbientStrength = AmbientStrength;
	OutParameters.DiffuseStrength = DiffuseStrength;
	OutParameters.LightDirection = LightDirection;

	OutParameters.StepVoxels = StepVoxels;
	OutParameters.ReferenceStepVoxels = ReferenceStepVoxels;
	OutParameters.EarlyTerminationAlpha = EarlyTerminationAlpha;

	OutParameters.bEnableJitter = bEnableJitter ? 1u : 0u;
	OutParameters.JitterAmount = JitterAmount;
	OutParameters.JitterSeed = JitterSeed;

	OutParameters.bFilterField = bFilterField ? 1u : 0u;
	OutParameters.bStrictStatusFilter = bStrictStatusFilter ? 1u : 0u;

	OutParameters.NoDataColor = NoDataColor;

	// LAST, AND CLAMPED AGAIN. FillDefaults' own clamp ran before this function,
	// so writing MaxSteps here without re-clamping would step around the one
	// place that bounds the GPU loop. Cheap, and the alternative is a hang.
	OutParameters.MaxSteps = FMath::Clamp(MaxSteps, 1u, FlowVizRayMarch::MaxStepsLimit);
}
