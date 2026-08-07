// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizBoundaryMesh.h"

#include "CFDViz/CFDVizMeshReader.h"
#include "CFDViz/CFDVizTypes.h"

FCFDVizResult FlowVizBoundary::BuildPatches(
	const FString& MeshPath,
	const FCFDVizMesh& Mesh,
	double MetersToUnrealUnits,
	TArray<FFlowVizBoundaryPatchGeometry>& OutPatches)
{
	OutPatches.Reset();

	FCFDVizMeshReader Reader;
	const FCFDVizResult Loaded = Reader.LoadFromFile(MeshPath);
	if (!Loaded.IsOk())
	{
		return Loaded;
	}

	const TArray<FVector3f>& Positions = Reader.GetPositions();
	const TArray<FVector3f>& SourceNormals = Reader.GetNormals();
	const int32 TriangleCount = Reader.GetTriangleCount();

	const FMatrix SolverToUnreal = MakeSolverToUnrealTransform(MetersToUnrealUnits);

	/*
	 * PATCH TABLE: the manifest's declarations, in manifest order, plus one
	 * synthesized bucket for triangles whose patch id the manifest does not
	 * declare. Dropped triangles would read as a hole in the boundary; a loud
	 * grey "undeclared" patch is a data problem stated as one.
	 */
	TMap<uint32, int32> PatchIndexById;
	for (const FCFDVizBoundaryPatch& Declared : Mesh.Patches)
	{
		FFlowVizBoundaryPatchGeometry& Geometry = OutPatches.AddDefaulted_GetRef();
		Geometry.PatchId = Declared.Id;
		Geometry.Name = Declared.Name;
		Geometry.Color = Declared.Color;
		Geometry.Opacity = Declared.Opacity;
		Geometry.bDefaultVisible = Declared.bDefaultVisible;
		PatchIndexById.Add(Declared.Id, OutPatches.Num() - 1);
	}

	int32 UndeclaredIndex = INDEX_NONE;

	/*
	 * PER-PATCH VERTEX WELDING. Each patch gets its own vertex list; a shared
	 * list would tie every patch's section to one buffer and per-patch
	 * visibility would need per-triangle filtering every change. The map is
	 * source-vertex-index -> per-patch index.
	 */
	TArray<TMap<uint32, int32>> Remaps;
	Remaps.SetNum(OutPatches.Num());

	const auto AddTriangle = [&](int32 PatchIndex, uint32 A, uint32 B, uint32 C)
	{
		if (!Remaps.IsValidIndex(PatchIndex))
		{
			Remaps.SetNum(OutPatches.Num());
		}
		FFlowVizBoundaryPatchGeometry& Geometry = OutPatches[PatchIndex];
		TMap<uint32, int32>& Remap = Remaps[PatchIndex];

		const auto MapVertex = [&](uint32 SourceIndex) -> int32
		{
			if (const int32* Existing = Remap.Find(SourceIndex))
			{
				return *Existing;
			}
			const FVector Solver(Positions[SourceIndex]);
			const int32 NewIndex = Geometry.Vertices.Add(
				SolverToUnreal.TransformPosition(Solver));
			if (SourceNormals.IsValidIndex(SourceIndex))
			{
				// A DIRECTION, not a position: rotate and mirror, never translate
				// or scale -- TransformVector then renormalise, because the
				// mirrored transform's scale would stretch it.
				Geometry.Normals.Add(
					SolverToUnreal.TransformVector(FVector(SourceNormals[SourceIndex]))
						.GetSafeNormal());
			}
			Remap.Add(SourceIndex, NewIndex);
			return NewIndex;
		};

		/*
		 * WINDING SWAPPED: MakeSolverToUnrealTransform mirrors Y (negative
		 * determinant), which reverses triangle orientation. Emitting A,C,B
		 * restores it -- the CVM reader's own header calls this out, and
		 * getting it wrong renders every boundary inside out, which looks like
		 * missing geometry from outside the domain.
		 */
		const int32 IndexA = MapVertex(A);
		const int32 IndexC = MapVertex(C);
		const int32 IndexB = MapVertex(B);
		Geometry.Indices.Add(IndexA);
		Geometry.Indices.Add(IndexC);
		Geometry.Indices.Add(IndexB);
	};

	for (int32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
	{
		uint32 A = 0, B = 0, C = 0;
		if (!Reader.TryGetTriangle(Triangle, A, B, C))
		{
			continue;
		}

		uint32 PatchId = 0;
		const bool bHasPatch = Reader.TryGetTrianglePatchId(Triangle, PatchId);

		int32 PatchIndex;
		if (bHasPatch && PatchIndexById.Contains(PatchId))
		{
			PatchIndex = PatchIndexById[PatchId];
		}
		else
		{
			if (UndeclaredIndex == INDEX_NONE)
			{
				FFlowVizBoundaryPatchGeometry& Undeclared = OutPatches.AddDefaulted_GetRef();
				Undeclared.PatchId = MAX_uint32;
				Undeclared.Name = TEXT("undeclared");
				Undeclared.Color = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);
				UndeclaredIndex = OutPatches.Num() - 1;
				Remaps.SetNum(OutPatches.Num());
			}
			PatchIndex = UndeclaredIndex;
		}
		AddTriangle(PatchIndex, A, B, C);
	}

	// Declared-but-empty patches are REMOVED: a section with no triangles is a
	// row in the patch list whose visibility toggle can never do anything --
	// rule 15's nonfunctional control, in list form.
	OutPatches.RemoveAll([](const FFlowVizBoundaryPatchGeometry& Geometry)
	{
		return Geometry.Indices.Num() == 0;
	});

	return FCFDVizResult::Ok();
}
