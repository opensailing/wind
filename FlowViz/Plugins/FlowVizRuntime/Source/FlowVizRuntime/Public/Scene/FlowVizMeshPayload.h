// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * One renderable surface, built on a worker, applied on the game thread
 * (renderer overhaul P2; architecture brief's "typed geometry payloads").
 *
 * WHY A PAYLOAD TYPE AND NOT COMPONENT CALLS FROM THE WORKER: UE components
 * are game-thread objects. The pure builders (boundary patches now; cut
 * planes and marching cubes in P3/P4) run on UE::Tasks workers, so their
 * output must cross as plain data -- the same worker-build/drain-apply split
 * the sampling service uses. Everything here is arrays and POD.
 *
 * SECTIONS. A payload is a list of sections; each becomes one mesh section
 * on the applying component (one boundary patch, one cut plane, one iso
 * surface). Per-section visibility is how "hide the inlet" works, so a
 * section carries its own identity and default.
 */
struct FFlowVizMeshSection
{
	/** Stable identity for UI toggles: the patch id for boundary sections. */
	uint32 SectionId = 0;
	FString Name;

	/** Honoured at apply time -- an inlet with defaultVisible=false arrives hidden. */
	bool bDefaultVisible = true;

	/** Unreal-space triangle list. Indices index THIS section's arrays. */
	TArray<FVector> Vertices;
	TArray<int32> Indices;
	TArray<FVector> Normals;

	/**
	 * Per-vertex normalized scalar for colormap surfaces (P3+): baked into
	 * UV0.x by the applier. Empty for uncolored surfaces (the P2 obstacle);
	 * when non-empty it must match Vertices in length.
	 */
	TArray<float> ScalarUVs;

	/**
	 * Per-vertex COLORS, sampled from CFDViz::ColorMaps on the CPU (renderer
	 * overhaul P9). The GPU-LUT-texture route died of Metal ambiguities --
	 * sampler-type compile rejections, byte-order swaps, MID timing -- each
	 * invisible until a capture. Vertex colors have no failure surface: the
	 * authority's values ride the mesh, and 8-bit per channel is exactly the
	 * precision the framebuffer displays. A colormap change re-requests the
	 * build (payloads already rebuild per frame, so this costs nothing new).
	 */
	TArray<FColor> Colors;
};

struct FFlowVizMeshPayload
{
	TArray<FFlowVizMeshSection> Sections;

	int32 TotalVertexCount() const
	{
		int32 Count = 0;
		for (const FFlowVizMeshSection& Section : Sections)
		{
			Count += Section.Vertices.Num();
		}
		return Count;
	}
};
