// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class ACFDVizCaseActor;

/**
 * The Presentation profile's world (renderer overhaul P7; research doc 7.1).
 *
 * PROFILES ARE WORLD STATE, NOT SHADER FLAGS (architecture brief): the
 * Presentation switch swaps in a three-point studio rig -- key light angled
 * across the flow axis so it grazes iso tubes and brings out curvature, cool
 * fill from the opposite side, warm rim from behind the wake so silhouettes
 * glow -- and Scientific removes it, leaving the map's flat lighting. The
 * renderer code paths never branch on profile; the SCENE does.
 *
 * COMPONENTS ON THE CASE ACTOR, not spawned actors: the rig belongs to the
 * case (its axes are the DOMAIN's axes), travels with it, and dies with it.
 * Pure configuration -- positions/colors/intensities -- lives in the static
 * Describe function so a headless test can pin the design without a world.
 *
 * THREADING AND LIFETIME. Describe is pure and may run on any thread. Apply,
 * Remove, and IsApplied inspect or mutate actor components and are game-thread
 * only. The actor owns every created light, so world teardown destroys the rig;
 * callers only choose whether to install or remove it while the actor is live.
 */
namespace FlowVizStudioRig
{
	struct FLightDescription
	{
		FName Name;

		/** Unit direction the light POINTS (directional semantics). */
		FVector Direction;
		FLinearColor Color;
		float IntensityLux;
	};

	/**
	 * The rig's design: key, fill, rim. UnrealDomainSize is intentionally unused
	 * while all three lights are directional; it remains part of the seam so a
	 * future area-light rig can scale without changing every caller. Pure. The
	 * test pins the three-point grammar (key brightest, fill cooler and dimmer,
	 * rim from behind-above the +X wake axis).
	 */
	FLOWVIZRUNTIME_API void Describe(
		const FVector& UnrealDomainSize, TArray<FLightDescription>& OutLights);

	/** Spawn/refresh the rig's light components on the actor. Idempotent. */
	FLOWVIZRUNTIME_API void Apply(ACFDVizCaseActor& Actor);

	/** Remove the rig's components. Idempotent; other components untouched. */
	FLOWVIZRUNTIME_API void Remove(ACFDVizCaseActor& Actor);

	/** True when the actor currently carries the rig. */
	FLOWVIZRUNTIME_API bool IsApplied(const ACFDVizCaseActor& Actor);
}
