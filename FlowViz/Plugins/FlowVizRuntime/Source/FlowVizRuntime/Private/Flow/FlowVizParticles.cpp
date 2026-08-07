// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizParticles.h"

#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizParticlesLocal
{
	/** The blended velocity at a position; false when either sampler refuses. */
	bool SampleBlended(
		const FFlowVizFieldSampler& SamplerA,
		const FFlowVizFieldSampler* SamplerB,
		double BlendAlpha,
		const FVector& Position,
		FVector& OutVelocity)
	{
		FVector VelocityA;
		if (!SamplerA.SampleVector(Position, VelocityA))
		{
			return false;
		}
		if (SamplerB == nullptr || BlendAlpha <= 0.0)
		{
			OutVelocity = VelocityA;
			return true;
		}
		FVector VelocityB;
		if (!SamplerB->SampleVector(Position, VelocityB))
		{
			// The partner refusing where A did not happens at the frame pair's
			// shared mask edge; A's answer is the honest one available.
			OutVelocity = VelocityA;
			return true;
		}
		OutVelocity = FMath::Lerp(VelocityA, VelocityB, BlendAlpha);
		return true;
	}
}

int32 FlowVizParticles::AdvanceParticles(
	const FFlowVizFieldSampler& SamplerA,
	const FFlowVizFieldSampler* SamplerB,
	double BlendAlpha,
	const FFlowVizFieldMask& Mask,
	const FAdvanceSettings& Settings,
	TArray<FParticle>& Particles)
{
	using namespace FlowVizParticlesLocal;

	if (!SamplerA.IsBuilt() || SamplerA.GetComponentCount() != 3
		|| !FMath::IsFinite(Settings.DeltaSeconds) || Settings.DeltaSeconds <= 0.0)
	{
		return 0;
	}

	int32 LiveCount = 0;
	for (FParticle& Particle : Particles)
	{
		if (!Particle.bAlive)
		{
			continue;
		}

		/*
		 * RK2 MIDPOINT, not Euler and not RK4. Euler visibly spirals tracer
		 * orbits outward at display timesteps; RK4 doubles the samples for
		 * accuracy below the field's own interpolation error. The midpoint
		 * rule is the standard tracer integrator at this fidelity.
		 */
		FVector Velocity;
		if (!SampleBlended(SamplerA, SamplerB, BlendAlpha, Particle.Position, Velocity))
		{
			Particle.bAlive = false;
			continue;
		}
		const FVector Midpoint =
			Particle.Position + Velocity * (Settings.DeltaSeconds * 0.5);
		FVector MidVelocity;
		if (!SampleBlended(SamplerA, SamplerB, BlendAlpha, Midpoint, MidVelocity))
		{
			// The midpoint left the domain: the particle is exiting this step.
			Particle.bAlive = false;
			continue;
		}
		const FVector Next = Particle.Position + MidVelocity * Settings.DeltaSeconds;

		/*
		 * KILL ON MASK ENTRY -- the P1 policy in particle form. A tracer that
		 * enters the obstacle and keeps integrating on clamped samples draws
		 * flow THROUGH the cylinder, the exact lie rule 10 names.
		 */
		if (Mask.IsMaskedAt(Next))
		{
			Particle.bAlive = false;
			continue;
		}

		Particle.Position = Next;
		Particle.Age += Settings.DeltaSeconds;
		if (Settings.MaxAge > 0.0 && Particle.Age > Settings.MaxAge)
		{
			// Age-out keeps the population cycling through the wake instead of
			// accumulating at the outlet.
			Particle.bAlive = false;
			continue;
		}
		++LiveCount;
	}
	return LiveCount;
}

void FlowVizParticles::RespawnDead(
	const FVector& RakeStart,
	const FVector& RakeEnd,
	TArray<FParticle>& Particles)
{
	/*
	 * DETERMINISTIC BY CONSTRUCTION: particle i's rake fraction is the
	 * low-discrepancy golden-ratio sequence frac(i * phi) -- even coverage
	 * without RNG state, so a capture rendered twice is the same capture
	 * (the P8 determinism rule, enforced here where the math lives).
	 */
	constexpr double GoldenFraction = 0.6180339887498949;
	for (int32 Index = 0; Index < Particles.Num(); ++Index)
	{
		FParticle& Particle = Particles[Index];
		if (Particle.bAlive)
		{
			continue;
		}
		const double Fraction = FMath::Frac(Index * GoldenFraction);
		Particle.Position = FMath::Lerp(RakeStart, RakeEnd, Fraction);
		Particle.Age = 0.0;
		Particle.bAlive = true;
	}
}
