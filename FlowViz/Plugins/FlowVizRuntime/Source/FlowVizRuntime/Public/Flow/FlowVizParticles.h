// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FFlowVizFieldMask;
class FFlowVizFieldSampler;

/**
 * Passive tracer advection (renderer overhaul P5; the Niagara oracle).
 *
 * WHY A CPU REFERENCE WHEN P8's PARTICLES ARE GPU: the GPU sim cannot be
 * headless-tested, so this is the tested statement of the math -- RK2
 * midpoint over the sampled velocity field, kill on mask entry or domain
 * exit, deterministic respawn -- that the Niagara system must reproduce.
 * The workspace can also run it directly at small counts (the Scientific
 * profile's fallback when Niagara is unavailable).
 *
 * UNSTEADY HONESTY: tracers advected through a SINGLE frame's field are
 * streaklines of a frozen flow. True pathlines need the field pair the
 * player already blends; AdvanceParticles takes an optional second sampler
 * and blend weight for exactly that, matching the display's interpolation.
 */
namespace FlowVizParticles
{
	struct FParticle
	{
		FVector Position = FVector::ZeroVector;

		/** Seconds this particle has lived; drives trail fade. */
		double Age = 0.0;

		/** False after a kill until the caller respawns it. */
		bool bAlive = false;
	};

	struct FAdvanceSettings
	{
		/** Wall-clock step, seconds of SOLVER time. */
		double DeltaSeconds = 1.0 / 60.0;

		/** Particles older than this are killed for respawn. <= 0 disables. */
		double MaxAge = 10.0;
	};

	/**
	 * Advance every live particle one step: RK2 midpoint through the blended
	 * field, kill on mask entry, domain exit, stagnation or age-out.
	 *
	 * @param SamplerA The displayed frame's velocity sampler.
	 * @param SamplerB The blend partner, or null for a frozen field.
	 * @param BlendAlpha SamplerB's weight, the player's own blend.
	 * @return Live particles after the step.
	 */
	FLOWVIZRUNTIME_API int32 AdvanceParticles(
		const FFlowVizFieldSampler& SamplerA,
		const FFlowVizFieldSampler* SamplerB,
		double BlendAlpha,
		const FFlowVizFieldMask& Mask,
		const FAdvanceSettings& Settings,
		TArray<FParticle>& Particles);

	/**
	 * Respawn dead particles across an upstream rake, deterministically:
	 * particle i goes to the rake position seeded by (i * Stride) mod 1 --
	 * reproducible for captures, no RNG state to carry.
	 */
	FLOWVIZRUNTIME_API void RespawnDead(
		const FVector& RakeStart,
		const FVector& RakeEnd,
		TArray<FParticle>& Particles);
}
