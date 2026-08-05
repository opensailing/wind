// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// CoreMinimal forward-declares TArrayView without defining it, and the
// declarations below take one by value, which needs the complete type.
#include "Containers/ArrayView.h"
#include "Math/Box.h"

#include "CFDViz/CFDVizTypes.h"

/**
 * CVM reader - boundary and structure triangle meshes (format section 5).
 *
 * A `.cvm` file is a fixed 96-byte header followed by up to five flat arrays:
 * positions, optional normals, triangle indices, optional per-triangle patch
 * IDs and optional per-vertex node IDs. There is no compression and no brick
 * directory - unlike CVF, a mesh is small enough to be read in one pass.
 *
 * THREADING. Nothing here allocates a UObject, touches a UWorld, or hops to the
 * game thread; the whole class is plain structs and TArray so it can be called
 * from a worker thread (engineering rule 1 - no game-thread file I/O).
 *
 * SOLVER UNITS. Positions come back in the canonical CFDViz frame - right-handed,
 * Z-up, X-forward, in the manifest's `units.length`. They are NOT Unreal
 * centimetres and NOT Unreal's left-handed frame. Conversion happens in exactly
 * one adapter (format section 2, ADR 004), so nothing in this reader calls
 * SolverToUnreal*. When that adapter does mirror Y, it MUST also swap two indices
 * of every triangle: section 5.3 winding is counter-clockwise seen from outside,
 * and a mirror reverses it - see TransformReversesWinding in CFDVizTypes.h.
 *
 * INTEGRITY. CVM checksums its header and nothing else. A corrupt vertex block
 * is therefore undetectable by this format, which is a deliberate 1.0 trade-off:
 * meshes are static, small, and usually regenerated rather than shipped. Do not
 * infer from a passing header CRC that the geometry is intact.
 *
 * The normative contract is `Docs/CFDVIZ_FORMAT.md` section 5, and the Python
 * reference this reader must agree with byte-for-byte is
 * `Tools/cfdviz/src/cfdviz/cvm.py`.
 */

/**
 * Random-access byte source behind a load - a file handle or a memory view.
 *
 * Private to the implementation. It exists so LoadFromFile and LoadFromMemory
 * run the *same* validation code: a test that constructs a malformed mesh in
 * memory then exercises the identical bounds checks a file would.
 */
class FCFDVizMeshByteSource;

namespace CFDViz::Cvm
{
	/** Flag bits, format section 5.2. The numeric values are on-disk format. */
	inline constexpr uint32 FlagNormals = 1u << 0;
	inline constexpr uint32 FlagPatchIds = 1u << 1;
	inline constexpr uint32 FlagNodeIds = 1u << 2;
	inline constexpr uint32 FlagFloat64Positions = 1u << 3;

	/**
	 * Every flag bit this reader understands.
	 *
	 * A file setting anything outside this mask is rejected rather than read with
	 * the unknown bits ignored: an unknown bit may change how an array is laid
	 * out, and a reader that guessed would silently produce wrong geometry. The
	 * Python reference applies the identical mask (CVM_KNOWN_FLAGS).
	 */
	inline constexpr uint32 KnownFlags = FlagNormals | FlagPatchIds | FlagNodeIds | FlagFloat64Positions;

	/** Stored element sizes. Positions are float32 unless FlagFloat64Positions is set. */
	inline constexpr int64 Float32PositionComponentBytes = 4;
	inline constexpr int64 Float64PositionComponentBytes = 8;
	inline constexpr int64 NormalComponentBytes = 4;
	inline constexpr int64 IndexBytes = 4;
	inline constexpr int64 PatchIdBytes = 4;
	inline constexpr int64 NodeIdBytes = 8;

	/** Components per position, normal and triangle: XYZ, and three corners. */
	inline constexpr int64 ComponentsPerVector = 3;
	inline constexpr int64 IndicesPerTriangle = 3;

	/**
	 * The writer pads each array up to this alignment. Readers do not rely on it -
	 * every array is located by its own stored offset - but a reader that assumed
	 * arrays were tightly packed would silently misread a padded file.
	 */
	inline constexpr int64 ArrayAlignment = 8;
}

/* -------------------------------------------------------------------------- */
/* Header                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * The fixed 96-byte CVM header (format section 5.1).
 *
 * Counts and offsets are uint64 exactly as stored, not int32, so a hostile value
 * is visible to the validation code as the number the file actually contains
 * rather than as something a narrowing cast invented. FCFDVizMeshReader applies
 * its own capacity limits on top; Parse deliberately does not, so that Parse
 * accepts precisely the set of headers the Python reference accepts.
 *
 * The header is decoded field by field, never by memcpy over a native struct:
 * C++ padding is compiler-dependent and the on-disk layout is not (section 4,
 * which CVM follows).
 */
struct FCFDVizMeshHeader
{
	/** Container version. Major must be 1; a newer minor is accepted (format rule 1.4). */
	uint16 MajorVersion = 0;
	uint16 MinorVersion = 0;

	/** Presence and precision bits, section 5.2. Only CFDViz::Cvm::KnownFlags may be set. */
	uint32 Flags = 0;

	/** Vertices in the positions (and normals, and nodeIds) arrays. */
	uint64 VertexCount = 0;

	/** Triangles. The indices array holds three uint32 per triangle; patchIds holds one uint32 per triangle. */
	uint64 TriangleCount = 0;

	/**
	 * Absolute byte offsets of each array.
	 *
	 * An array whose presence bit is clear has offset 0 and MUST be ignored
	 * (section 5.2). This reader additionally rejects a non-zero offset behind a
	 * clear bit: it means the writer was buggy, and honouring it later would read
	 * whatever happens to sit at that offset.
	 */
	uint64 PositionsOffset = 0;
	uint64 NormalsOffset = 0;
	uint64 IndicesOffset = 0;
	uint64 PatchIdsOffset = 0;
	uint64 NodeIdsOffset = 0;

	/** CRC-32C over bytes [0, 96) with [80, 84) zeroed, as stored in the file. */
	uint32 HeaderCrc32C = 0;

	/** Section 5.2 flag accessors. Use these rather than testing Flags at a call site. */
	bool HasNormals() const { return (Flags & CFDViz::Cvm::FlagNormals) != 0; }
	bool HasPatchIds() const { return (Flags & CFDViz::Cvm::FlagPatchIds) != 0; }
	bool HasNodeIds() const { return (Flags & CFDViz::Cvm::FlagNodeIds) != 0; }
	bool ArePositionsFloat64() const { return (Flags & CFDViz::Cvm::FlagFloat64Positions) != 0; }

	/** Bytes per stored position component: 8 when flag bit 3 is set, otherwise 4. */
	int64 GetPositionComponentBytes() const
	{
		return ArePositionsFloat64()
			? CFDViz::Cvm::Float64PositionComponentBytes
			: CFDViz::Cvm::Float32PositionComponentBytes;
	}

	/** Bytes one stored vertex position occupies: 12 for float32, 24 for float64. */
	int64 GetPositionVertexBytes() const
	{
		return GetPositionComponentBytes() * CFDViz::Cvm::ComponentsPerVector;
	}

	/**
	 * Parse and validate 96 header bytes.
	 *
	 * Rejects, in the same order and for the same reasons as the Python reference:
	 * short input, a magic other than `CFDMESH1`, `endianMarker != 0x01020304`
	 * (rejected, never byte-swapped - format rule 1.1), a major version other
	 * than 1, `headerBytes != 96`, any flag bit outside KnownFlags, non-zero
	 * reserved bytes [84, 96), a header CRC mismatch, and a non-zero offset for
	 * an array whose presence bit is clear.
	 *
	 * @param HeaderBytes At least 96 bytes; trailing bytes are ignored so a caller
	 *                    can hand over a larger buffer.
	 * @param OutHeader   Filled in only on success; untouched on failure.
	 * @param FilePath    Named in the returned error, per format section 10.
	 * @param bVerifyCrc  Leave true. Only a validator that wants to *report* a CRC
	 *                    mismatch alongside the rest of the header should pass false.
	 * @return Ok, or a failure carrying the offending byte offset within the header.
	 */
	FLOWVIZRUNTIME_API static FCFDVizResult Parse(
		TArrayView<const uint8> HeaderBytes,
		FCFDVizMeshHeader& OutHeader,
		const FString& FilePath = FString(),
		bool bVerifyCrc = true);

	/**
	 * CRC-32C over 96 header bytes with the CRC field [80, 84) treated as zero.
	 *
	 * Exposed so a validation report can show stored and recomputed values side by
	 * side instead of only "mismatch". Returns 0 for fewer than 96 bytes, which is
	 * indistinguishable from a legitimate CRC of 0 - call it only on input you
	 * have already length-checked, as Parse does.
	 */
	FLOWVIZRUNTIME_API static uint32 ComputeHeaderCrc(TArrayView<const uint8> HeaderBytes);
};

/* -------------------------------------------------------------------------- */
/* Load options                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Optional knobs for a load. The defaults are what a viewer wants.
 *
 * The three "load this array" switches exist because the optional arrays are not
 * free - nodeIds alone is 8 bytes per vertex - and a renderer that never shows
 * solver node numbers should not pay for them. Skipping an array does not skip
 * its bounds check: a file whose nodeIds span runs off the end is still rejected,
 * because a truncated file is a truncated file whether or not this caller cares
 * about that array.
 */
struct FCFDVizMeshLoadOptions
{
	/** Verify the header CRC-32C. Turn off only in a validator that reports corruption instead of failing on it. */
	bool bVerifyHeaderCrc = true;

	/** Load per-vertex normals when present. */
	bool bLoadNormals = true;

	/** Load per-TRIANGLE patch IDs when present. FCFDVizMeshReader::BuildForPatch needs these. */
	bool bLoadPatchIds = true;

	/** Load per-VERTEX solver node IDs when present. */
	bool bLoadNodeIds = true;

	/**
	 * Also keep positions at their stored precision when the file is float64.
	 *
	 * Positions are always narrowed to FVector3f for rendering, and that narrowing
	 * is lossy: a domain measured in kilometres loses centimetres of detail, which
	 * matters for probing and measurement even though it is invisible on screen.
	 * Set this when the caller does arithmetic on coordinates rather than drawing
	 * them. It costs a further 24 bytes per vertex and does nothing for a float32
	 * file, where GetDoublePositions stays empty because no precision was lost.
	 */
	bool bRetainDoublePrecisionPositions = false;
};

/* -------------------------------------------------------------------------- */
/* Reader                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * Loads one `.cvm` triangle mesh and hands out its arrays.
 *
 * PER-TRIANGLE VERSUS PER-VERTEX. `patchIds` is one uint32 per TRIANGLE;
 * `nodeIds` is one uint64 per VERTEX (section 5.3). The two are the easiest
 * thing in this format to swap, and swapping them produces a mesh that loads
 * cleanly and is wrong - patches land on arbitrary triangles and node numbering
 * shifts. The accessors below are named and typed to make the mistake hard, and
 * the counts are asserted against the header on load.
 *
 * FAILURE LEAVES THE READER EMPTY. Arrays are built into locals and only moved
 * into the object once every check has passed, so a rejected file can never
 * leave half a mesh behind for a caller who ignored the result.
 */
class FLOWVIZRUNTIME_API FCFDVizMeshReader
{
public:
	FCFDVizMeshReader() = default;

	/**
	 * Read a mesh from disk.
	 *
	 * The file is read through an archive and only the spans the header describes
	 * are pulled in, after each has been checked against the real file size - so a
	 * header claiming four billion vertices is rejected without ever allocating
	 * for them (format rule 1.5, engineering rule 2).
	 *
	 * @param Path    Filesystem path. Path-traversal validation of a *manifest*
	 *                path (format rule 1.3, CFDViz::IsSafeRelativePath) belongs to
	 *                the manifest loader and has already happened by here; this
	 *                function is given a resolved path and does not re-check it.
	 * @param Options See FCFDVizMeshLoadOptions.
	 * @return Ok, or a failure naming the file and the offending byte offset.
	 */
	FCFDVizResult LoadFromFile(const FString& Path, const FCFDVizMeshLoadOptions& Options = FCFDVizMeshLoadOptions());

	/**
	 * Read a mesh from bytes already in memory.
	 *
	 * Same validation as LoadFromFile - the bounds checks run against
	 * FileBytes.Num() exactly as they otherwise run against the file size. Used by
	 * tests, which need to construct malformed files without touching disk, and by
	 * callers that already hold the bytes.
	 *
	 * @param DiagnosticPath Reported in errors. Purely for the message.
	 */
	FCFDVizResult LoadFromMemory(
		TArrayView<const uint8> FileBytes,
		const FString& DiagnosticPath = FString(),
		const FCFDVizMeshLoadOptions& Options = FCFDVizMeshLoadOptions());

	/** Drop every array and return to the unloaded state. */
	void Reset();

	/** True once a load has fully succeeded. False after a failed load, always. */
	bool IsLoaded() const { return bLoaded; }

	/** The header exactly as stored, for diagnostics and validation reports. */
	const FCFDVizMeshHeader& GetHeader() const { return Header; }

	/** Where this mesh came from, for error messages. */
	const FString& GetSourcePath() const { return SourcePath; }

	/* -- geometry ---------------------------------------------------------- */

	/**
	 * Vertex positions in canonical solver coordinates, narrowed to float.
	 *
	 * See FCFDVizMeshLoadOptions::bRetainDoublePrecisionPositions for what that
	 * narrowing costs on a float64 file and how to get the stored precision back.
	 */
	const TArray<FVector3f>& GetPositions() const { return Positions; }

	/**
	 * Triangle corner indices, three consecutive entries per triangle, in file
	 * order. Every entry has been proven `< VertexCount` during load (section 5.3):
	 * an out-of-range index is a rejected file, never a clamped index.
	 *
	 * Winding is counter-clockwise viewed from outside the solid.
	 */
	const TArray<uint32>& GetIndices() const { return Indices; }

	int32 GetVertexCount() const { return Positions.Num(); }
	int32 GetTriangleCount() const { return Indices.Num() / static_cast<int32>(CFDViz::Cvm::IndicesPerTriangle); }

	/* -- optional arrays --------------------------------------------------- */

	/** True when per-vertex normals were present in the file and loaded. */
	bool HasNormals() const { return bHasNormals; }

	/** One normal per VERTEX, parallel to GetPositions. Empty when absent or not requested. */
	const TArray<FVector3f>& GetNormals() const { return Normals; }

	/** True when per-triangle patch IDs were present in the file and loaded. Required by BuildForPatch. */
	bool HasPatchIds() const { return bHasPatchIds; }

	/**
	 * One patch ID per TRIANGLE - not per vertex - parallel to GetIndices in
	 * groups of three. Matches `manifest.meshes[].patches[].id`. Empty when absent
	 * or not requested.
	 */
	const TArray<uint32>& GetPatchIds() const { return PatchIds; }

	/** True when per-vertex solver node IDs were present in the file and loaded. */
	bool HasNodeIds() const { return bHasNodeIds; }

	/** One solver node ID per VERTEX - not per triangle - parallel to GetPositions. Empty when absent or not requested. */
	const TArray<uint64>& GetNodeIds() const { return NodeIds; }

	/** True when the file stored float64 positions, i.e. GetPositions has been narrowed. */
	bool ArePositionsFloat64() const { return Header.ArePositionsFloat64(); }

	/**
	 * Positions at their stored precision.
	 *
	 * Non-empty only when the file stored float64 AND
	 * bRetainDoublePrecisionPositions was set; a float32 file lost nothing, so
	 * GetPositions is already exact and this stays empty rather than duplicating it.
	 */
	const TArray<FVector>& GetDoublePositions() const { return DoublePositions; }

	/* -- element access ---------------------------------------------------- */

	/** Bounds-checked triangle lookup. Returns false, touching no output, for an out-of-range triangle. */
	bool TryGetTriangle(int32 TriangleIndex, uint32& OutA, uint32& OutB, uint32& OutC) const;

	/** Bounds-checked patch ID lookup. Returns false when there are no patch IDs, or the triangle is out of range. */
	bool TryGetTrianglePatchId(int32 TriangleIndex, uint32& OutPatchId) const;

	/** Bounds-checked node ID lookup. Returns false when there are no node IDs, or the vertex is out of range. */
	bool TryGetVertexNodeId(int32 VertexIndex, uint64& OutNodeId) const;

	/* -- derived ----------------------------------------------------------- */

	/**
	 * Axis-aligned bounds of the vertices, in solver units.
	 *
	 * Returns an invalid FBox3f (IsValid == 0) for an empty mesh, so a caller
	 * cannot frame a camera on nothing and believe it succeeded.
	 *
	 * Non-finite vertices are excluded from the bounds - one NaN coordinate would
	 * otherwise poison the whole box and, through it, camera framing and culling.
	 * This does not alter the stored positions, which are returned bit-exactly by
	 * GetPositions; it only declines to let a value that bounds nothing define a
	 * bound. A mesh that is entirely non-finite therefore yields an invalid box.
	 */
	FBox3f ComputeBounds() const;

	/**
	 * Every distinct patch ID present, ascending.
	 *
	 * Note the name: GetPatchIds is the raw per-triangle array, this is the set.
	 * Empty when the mesh carries no patch IDs.
	 */
	void GetUniquePatchIds(TArray<uint32>& OutUniquePatchIds) const;

	/** Triangles carrying PatchId. Zero when the mesh has no patch IDs. Use this to test for a patch without building it. */
	int32 CountTrianglesInPatch(uint32 PatchId) const;

	/**
	 * Extract one boundary patch as a standalone mesh.
	 *
	 * The UI shows and hides `inlet`, `outlet`, `sideWalls` and `cylinderWall`
	 * independently, so each needs to be its own renderable mesh rather than a
	 * subrange of a shared index buffer.
	 *
	 * Only the triangles whose patch ID matches are emitted, and only the vertices
	 * those triangles reference are copied, renumbered from zero. Corner order
	 * within each triangle is preserved, so the extracted mesh keeps the section
	 * 5.3 counter-clockwise-from-outside winding.
	 *
	 * FAILS rather than returning an empty mesh when the patch is not present, or
	 * when the mesh carries no patch IDs at all. An empty result would be
	 * indistinguishable from a patch the user hid, and the caller would render
	 * nothing while believing the case was fine. Call CountTrianglesInPatch or
	 * GetUniquePatchIds first if an absent patch is expected and acceptable.
	 *
	 * All outputs are reset before anything is written, including on failure.
	 */
	FCFDVizResult BuildForPatch(
		uint32 PatchId,
		TArray<FVector3f>& OutPositions,
		TArray<uint32>& OutIndices) const;

	/**
	 * As above, additionally emitting the patch's normals and the mapping back to
	 * the source mesh.
	 *
	 * @param OutNormals             Optional. Filled only when this mesh has normals;
	 *                               left empty otherwise, since a fabricated normal
	 *                               is a guess this reader will not make.
	 * @param OutSourceVertexIndices Optional. Entry i is the index in GetPositions
	 *                               that extracted vertex i came from - the way to
	 *                               recover per-vertex node IDs for the patch.
	 */
	FCFDVizResult BuildForPatch(
		uint32 PatchId,
		TArray<FVector3f>& OutPositions,
		TArray<uint32>& OutIndices,
		TArray<FVector3f>* OutNormals,
		TArray<int32>* OutSourceVertexIndices = nullptr) const;

private:
	/** Shared implementation behind LoadFromFile and LoadFromMemory, so both take the identical validation path. */
	FCFDVizResult LoadInternal(FCFDVizMeshByteSource& Source, const FString& Path, const FCFDVizMeshLoadOptions& Options);

	FCFDVizMeshHeader Header;
	FString SourcePath;

	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<uint32> Indices;
	TArray<uint32> PatchIds;
	TArray<uint64> NodeIds;
	TArray<FVector> DoublePositions;

	bool bLoaded = false;
	bool bHasNormals = false;
	bool bHasPatchIds = false;
	bool bHasNodeIds = false;
};
