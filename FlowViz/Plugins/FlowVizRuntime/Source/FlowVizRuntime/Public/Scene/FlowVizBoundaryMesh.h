// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CFDViz/CFDVizManifest.h"
#include "CoreMinimal.h"

/**
 * Boundary patch geometry builders (#78 / Milestone D, DoD 13).
 *
 * PURE BUILDERS, ONE SECTION PER PATCH. Each patch of a CVM mesh becomes an
 * independent triangle list, so the scene component maps patch -> mesh
 * section and "hide the inlet" is one section's visibility -- the per-patch
 * independence DoD 13 requires, decided at build time rather than filtered
 * per frame.
 *
 * COORDINATES. Input is the CVM's solver-frame float positions; output is
 * UNREAL space through MakeSolverToUnrealTransform, which mirrors Y (the
 * canonical frame is right-handed, Unreal's is left-handed). A mirrored
 * transform flips triangle winding, so the builder ALSO swaps two indices per
 * triangle -- the CVM reader's own header note -- or every patch would face
 * inside out.
 */
struct FFlowVizBoundaryPatchGeometry
{
	/** The manifest patch this section renders. */
	uint32 PatchId = 0;
	FString Name;
	FLinearColor Color = FLinearColor::White;
	float Opacity = 1.0f;
	bool bDefaultVisible = true;

	/** Unreal-space triangle list for this patch alone. */
	TArray<FVector> Vertices;
	TArray<int32> Indices;
	TArray<FVector> Normals;
};

namespace FlowVizBoundary
{
	/**
	 * Build one mesh's patches from its CVM file.
	 *
	 * Patches come from the MANIFEST's declaration for the mesh; triangles are
	 * assigned by the CVM's per-triangle patch IDs. Triangles carrying a patch
	 * id the manifest does not declare are collected under a synthesized
	 * "undeclared" entry rather than dropped -- geometry that silently
	 * disappears reads as a hole in the boundary (rule 10's shape, in mesh
	 * form).
	 *
	 * @param MeshPath Absolute path to the .cvm (FCFDVizCase::ResolveMeshPath).
	 * @param Mesh     The manifest's descriptor, for the patch table.
	 * @param MetersToUnrealUnits The case's length scale.
	 * @param OutPatches One entry per patch WITH triangles, manifest order.
	 * @return Ok, or the reader's failure.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult BuildPatches(
		const FString& MeshPath,
		const FCFDVizMesh& Mesh,
		double MetersToUnrealUnits,
		TArray<FFlowVizBoundaryPatchGeometry>& OutPatches);
}
