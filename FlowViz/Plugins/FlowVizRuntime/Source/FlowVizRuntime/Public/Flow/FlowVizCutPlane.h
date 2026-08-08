// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CFDViz/CFDVizColorMaps.h"
#include "CoreMinimal.h"
#include "Scene/FlowVizMeshPayload.h"

class FFlowVizFieldMask;
class FFlowVizFieldSampler;

/**
 * The cut plane builder (renderer overhaul P3; research doc section 1
 * ingredient 3).
 *
 * THE CANONICAL 2D DIAGNOSTIC: a plane through the wake, sampled bilinearly,
 * rendered OPAQUE with Gouraud-smooth color. OpenFOAM's motorBike centreline
 * plane with cellPoint interpolation is the reference; for the quasi-2D demo
 * case a z-mid plane of |U| or vorticity IS the honest hero image.
 *
 * MASK CELLS ARE HARD-EDGED. A grid vertex whose interpolation footprint
 * touches the solid is dropped, and with it every quad that uses it -- the
 * plane has a HOLE where the cylinder is, exactly like the reference images.
 * Interpolating colour across the mask would smear data into the obstacle
 * (rule 10 in cut-plane form); drawing the mask in a flag colour is the
 * volume path's job, not this one's.
 *
 * OUTPUT IS SOLVER-INDEPENDENT GEOMETRY: one FFlowVizMeshSection in Unreal
 * space with per-vertex normalized scalars in ScalarUVs, ready for the
 * surface component and the LUT material. Build on a worker.
 */
namespace FlowVizCutPlane
{
	struct FCutPlaneRequest
	{
		/** Plane origin and unit normal, SOLVER space (the slice view model's). */
		FVector Origin = FVector::ZeroVector;
		FVector Normal = FVector::ZAxisVector;

		/** Domain extent in solver units; the plane is clipped to this box. */
		FVector DomainSize = FVector::ZeroVector;

		/** Grid resolution along the plane's longer axis. The shorter scales by aspect. */
		int32 Resolution = 96;

		/** Scalar range mapped to [0,1] in ScalarUVs. Max <= Min means "compute from the plane's own samples". */
		double RangeMin = 0.0;
		double RangeMax = 0.0;
	};

	/**
	 * Sample the field over the plane and build the mesh section.
	 *
	 * For vector fields the scalar is the MAGNITUDE; scalars sample directly.
	 * Vertices whose footprint touches the mask (or leaves the domain) are
	 * dropped with their quads. Returns false when nothing survives -- a
	 * plane entirely outside the domain, or entirely inside the solid.
	 *
	 * @param OutSection Unreal-space geometry, both-sides normals along the
	 *        plane normal, ScalarUVs filled per vertex.
	 * @param OutRangeMin/Max The range actually used for normalization --
	 *        echoed back so the legend and the mesh can never disagree.
	 */
	FLOWVIZRUNTIME_API bool BuildCutPlaneMesh(
		const FFlowVizFieldSampler& Sampler,
		const FFlowVizFieldMask& Mask,
		const FCutPlaneRequest& Request,
		FFlowVizMeshSection& OutSection,
		double& OutRangeMin,
		double& OutRangeMax);

	/**
	 * Fill Section.Colors from Section.ScalarUVs through the color authority
	 * (renderer overhaul P9): one CPU sample per vertex, sRGB-encoded the way
	 * the framebuffer stores color anyway. The whole GPU LUT hop -- texture,
	 * sampler type, MID -- is deliberately absent; see FFlowVizMeshSection.
	 */
	FLOWVIZRUNTIME_API void ColorizeSection(
		FFlowVizMeshSection& Section, ECFDVizColorMap Map);
}
