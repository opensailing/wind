// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizMeshReader.h"
#include "CFDViz/CFDVizCrc32C.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * CVM reader conformance (format section 5).
 *
 * BOTH FIXTURES BELOW ARE BYTES THE PYTHON REFERENCE WRITER ACTUALLY PRODUCED,
 * captured from `cfdviz.cvm.write_cvm(...)` and pasted verbatim. That is what
 * makes this a cross-implementation check rather than a self-consistency check:
 * every offset, every CRC and the float64 bit patterns come from the other
 * implementation. A round-trip through this reader alone would pass just as
 * happily if both sides shared the same wrong idea of the layout, which is the
 * entire reason two implementations exist.
 */

namespace
{
	/**
	 * 4 vertices, 2 triangles, float32 positions, ALL optional arrays present
	 * (flags = 7: normals | patchIds | nodeIds).
	 *
	 * THE VERTEX AND TRIANGLE COUNTS DELIBERATELY DIFFER (4 vs 2). patchIds is
	 * one uint32 per TRIANGLE and nodeIds is one uint64 per VERTEX (section
	 * 5.3), and the header itself calls this "the easiest thing in this format
	 * to swap". With 4 vertices and 2 triangles a reader that confuses the two
	 * associations reads the wrong number of elements and cannot quietly pass;
	 * had the fixture used 3 vertices and 3 triangles, a swap would produce
	 * correctly-sized garbage and this test would be worthless.
	 *
	 * From `write_cvm(positions=[(0,0,0),(1,0,0),(0,1,0),(1,1,0)],
	 *                 indices=[(0,1,2),(1,3,2)], normals=[(0,0,1)]*4,
	 *                 patch_ids=[7,9], node_ids=[100,200,300,400])`.
	 */
	const uint8 GoldenCvmBytes[] = {
		// ---- header, offset 0 ------------------------------------------------
		0x43, 0x46, 0x44, 0x4D, 0x45, 0x53, 0x48, 0x31, // 0   magic "CFDMESH1"
		0x01, 0x00,                                     // 8   majorVersion = 1
		0x00, 0x00,                                     // 10  minorVersion = 0
		0x04, 0x03, 0x02, 0x01,                         // 12  endianMarker
		0x07, 0x00, 0x00, 0x00,                         // 16  flags = 7
		0x60, 0x00, 0x00, 0x00,                         // 20  headerBytes = 96
		0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 24  vertexCount = 4
		0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 32  triangleCount = 2
		0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 40  positionsOffset = 96
		0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 48  normalsOffset = 144
		0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 56  indicesOffset = 192
		0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 64  patchIdsOffset = 216
		0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 72  nodeIdsOffset = 224
		0x29, 0xC9, 0xA7, 0xAA,                         // 80  headerCrc32c
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 84  reserved[12] = 0
		0x00, 0x00, 0x00, 0x00,

		// ---- positions, offset 96: 4 vertices x 3 float32 --------------------
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // (0,0,
		0x00, 0x00, 0x00, 0x00,                         //  0)
		0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, // (1,0,
		0x00, 0x00, 0x00, 0x00,                         //  0)
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, // (0,1,
		0x00, 0x00, 0x00, 0x00,                         //  0)
		0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, // (1,1,
		0x00, 0x00, 0x00, 0x00,                         //  0)

		// ---- normals, offset 144: 4 vertices x 3 float32, all +Z -------------
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x80, 0x3F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x80, 0x3F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x80, 0x3F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x80, 0x3F,

		// ---- indices, offset 192: 2 triangles x 3 uint32 ---------------------
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // (0,1,
		0x02, 0x00, 0x00, 0x00,                         //  2)
		0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, // (1,3,
		0x02, 0x00, 0x00, 0x00,                         //  2)

		// ---- patchIds, offset 216: ONE PER TRIANGLE, so 2 entries ------------
		0x07, 0x00, 0x00, 0x00,                         // triangle 0 -> patch 7
		0x09, 0x00, 0x00, 0x00,                         // triangle 1 -> patch 9

		// ---- nodeIds, offset 224: ONE PER VERTEX, so 4 entries ---------------
		0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 100
		0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 200
		0x2C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 300
		0x90, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 400
	};

	/**
	 * 3 vertices, 1 triangle, FLOAT64 positions (flag bit 3), no optional arrays.
	 *
	 * Two things are under test here that the float32 fixture cannot reach:
	 * the 24-byte position stride, and section 5.2's rule that an absent array's
	 * offset MUST be written as 0 - normalsOffset, patchIdsOffset and
	 * nodeIdsOffset are all zero below.
	 *
	 * The coordinates 1e300 and -1e300 are chosen because they are representable
	 * in float64 and NOT in float32: they narrow to +/-inf. That makes the
	 * narrowing observable, so a reader that silently narrowed and then claimed
	 * to have preserved precision would be caught.
	 *
	 * From `write_cvm(positions=[(1.5,-2.5,3.25),(0,0,0),(1e300,-1e300,0.1)],
	 *                 indices=[(0,1,2)], use_float64=True)`.
	 */
	const uint8 Float64CvmBytes[] = {
		// ---- header, offset 0 ------------------------------------------------
		0x43, 0x46, 0x44, 0x4D, 0x45, 0x53, 0x48, 0x31, // 0   magic "CFDMESH1"
		0x01, 0x00,                                     // 8   majorVersion = 1
		0x00, 0x00,                                     // 10  minorVersion = 0
		0x04, 0x03, 0x02, 0x01,                         // 12  endianMarker
		0x08, 0x00, 0x00, 0x00,                         // 16  flags = float64 only
		0x60, 0x00, 0x00, 0x00,                         // 20  headerBytes = 96
		0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 24  vertexCount = 3
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 32  triangleCount = 1
		0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 40  positionsOffset = 96
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 48  normalsOffset = 0
		0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 56  indicesOffset = 168
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 64  patchIdsOffset = 0
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 72  nodeIdsOffset = 0
		0x3A, 0xAE, 0x62, 0x49,                         // 80  headerCrc32c
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 84  reserved[12] = 0
		0x00, 0x00, 0x00, 0x00,

		// ---- positions, offset 96: 3 vertices x 3 FLOAT64 = 72 bytes ---------
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F, // 1.5
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xC0, // -2.5
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x40, // 3.25
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0
		0x9C, 0x75, 0x00, 0x88, 0x3C, 0xE4, 0x37, 0x7E, // 1e300
		0x9C, 0x75, 0x00, 0x88, 0x3C, 0xE4, 0x37, 0xFE, // -1e300
		0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F, // 0.1

		// ---- indices, offset 168: 1 triangle x 3 uint32 ----------------------
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x02, 0x00, 0x00, 0x00,
	};

	/** A modifiable copy, so a test can corrupt one byte without disturbing the fixture. */
	TArray<uint8> CopyOf(const uint8* Bytes, int32 Count)
	{
		TArray<uint8> Copy;
		Copy.Append(Bytes, Count);
		return Copy;
	}

	/**
	 * Re-seal the header CRC after a test has edited a header field.
	 *
	 * Without this, every mutation test would fail at the CRC and never reach the
	 * check it was actually written to exercise - a test that always fails for
	 * the same reason proves nothing about the field it names.
	 */
	void ResealHeaderCrc(TArray<uint8>& Bytes)
	{
		FMemory::Memzero(Bytes.GetData() + 80, 4);
		const uint32 Crc = CFDViz::Crc32C::Compute(Bytes.GetData(), 96);
		FMemory::Memcpy(Bytes.GetData() + 80, &Crc, 4);
	}

	void WriteUInt64At(TArray<uint8>& Bytes, int32 Offset, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (Index * 8)) & 0xFF);
		}
	}

	void WriteUInt32At(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (Index * 8)) & 0xFF);
		}
	}
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                       */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizMeshReaderLayoutTest,
	"FlowViz.CFDViz.MeshReader.Layout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizMeshReaderLayoutTest::RunTest(const FString& Parameters)
{
	FCFDVizMeshReader Reader;
	const FCFDVizResult Result = Reader.LoadFromMemory(
		TArrayView<const uint8>(GoldenCvmBytes, UE_ARRAY_COUNT(GoldenCvmBytes)), TEXT("golden.cvm"));

	if (!TestTrue(TEXT("the golden mesh loads"), Result.IsOk()))
	{
		AddError(FString::Printf(TEXT("load failed: %s"), *Result.Message));
		return false;
	}

	// --- header fields, asserted against the section 5.1 offsets ------------
	const FCFDVizMeshHeader& Header = Reader.GetHeader();
	TestEqual(TEXT("majorVersion @8"), static_cast<int32>(Header.MajorVersion), 1);
	TestEqual(TEXT("minorVersion @10"), static_cast<int32>(Header.MinorVersion), 0);
	TestEqual(TEXT("flags @16"), static_cast<int32>(Header.Flags), 7);
	TestEqual(TEXT("vertexCount @24"), static_cast<int64>(Header.VertexCount), static_cast<int64>(4));
	TestEqual(TEXT("triangleCount @32"), static_cast<int64>(Header.TriangleCount), static_cast<int64>(2));
	TestEqual(TEXT("positionsOffset @40"), static_cast<int64>(Header.PositionsOffset), static_cast<int64>(96));
	TestEqual(TEXT("normalsOffset @48"), static_cast<int64>(Header.NormalsOffset), static_cast<int64>(144));
	TestEqual(TEXT("indicesOffset @56"), static_cast<int64>(Header.IndicesOffset), static_cast<int64>(192));
	TestEqual(TEXT("patchIdsOffset @64"), static_cast<int64>(Header.PatchIdsOffset), static_cast<int64>(216));
	TestEqual(TEXT("nodeIdsOffset @72"), static_cast<int64>(Header.NodeIdsOffset), static_cast<int64>(224));
	TestEqual(TEXT("headerCrc32c @80"), static_cast<int64>(Header.HeaderCrc32C), static_cast<int64>(0xAAA7C929));

	TestTrue(TEXT("flag bit 0 -> normals"), Header.HasNormals());
	TestTrue(TEXT("flag bit 1 -> patch IDs"), Header.HasPatchIds());
	TestTrue(TEXT("flag bit 2 -> node IDs"), Header.HasNodeIds());
	TestFalse(TEXT("flag bit 3 clear -> float32 positions"), Header.ArePositionsFloat64());
	TestEqual(TEXT("float32 stride is 12 bytes"), Header.GetPositionVertexBytes(), static_cast<int64>(12));

	// --- positions ----------------------------------------------------------
	const TArray<FVector3f>& Positions = Reader.GetPositions();
	if (TestEqual(TEXT("4 vertices"), Positions.Num(), 4))
	{
		TestEqual(TEXT("v0"), Positions[0], FVector3f(0.0f, 0.0f, 0.0f));
		TestEqual(TEXT("v1"), Positions[1], FVector3f(1.0f, 0.0f, 0.0f));
		TestEqual(TEXT("v2"), Positions[2], FVector3f(0.0f, 1.0f, 0.0f));
		TestEqual(TEXT("v3"), Positions[3], FVector3f(1.0f, 1.0f, 0.0f));
	}

	// --- indices, and the winding order within each triangle -----------------
	const TArray<uint32>& Indices = Reader.GetIndices();
	if (TestEqual(TEXT("2 triangles x 3 indices"), Indices.Num(), 6))
	{
		// Corner ORDER matters, not just the set: section 5.3 fixes the winding
		// as counter-clockwise viewed from outside, so a reader that sorted or
		// rotated the corners would flip normals and light the mesh from inside.
		TestEqual(TEXT("t0[0]"), Indices[0], static_cast<uint32>(0));
		TestEqual(TEXT("t0[1]"), Indices[1], static_cast<uint32>(1));
		TestEqual(TEXT("t0[2]"), Indices[2], static_cast<uint32>(2));
		TestEqual(TEXT("t1[0]"), Indices[3], static_cast<uint32>(1));
		TestEqual(TEXT("t1[1]"), Indices[4], static_cast<uint32>(3));
		TestEqual(TEXT("t1[2]"), Indices[5], static_cast<uint32>(2));
	}
	TestEqual(TEXT("GetTriangleCount agrees with the header"), Reader.GetTriangleCount(), 2);
	TestEqual(TEXT("GetVertexCount agrees with the header"), Reader.GetVertexCount(), 4);

	// --- THE ASSOCIATION CHECK ----------------------------------------------
	// patchIds is per TRIANGLE (2 entries), nodeIds is per VERTEX (4 entries).
	// These two assertions are the reason the fixture has 4 vertices and 2
	// triangles rather than an equal number of each.
	const TArray<uint32>& PatchIds = Reader.GetPatchIds();
	if (TestEqual(TEXT("patchIds has one entry per TRIANGLE"), PatchIds.Num(), 2))
	{
		TestEqual(TEXT("triangle 0 is in patch 7"), PatchIds[0], static_cast<uint32>(7));
		TestEqual(TEXT("triangle 1 is in patch 9"), PatchIds[1], static_cast<uint32>(9));
	}

	const TArray<uint64>& NodeIds = Reader.GetNodeIds();
	if (TestEqual(TEXT("nodeIds has one entry per VERTEX"), NodeIds.Num(), 4))
	{
		TestEqual(TEXT("node 0"), NodeIds[0], static_cast<uint64>(100));
		TestEqual(TEXT("node 1"), NodeIds[1], static_cast<uint64>(200));
		TestEqual(TEXT("node 2"), NodeIds[2], static_cast<uint64>(300));
		TestEqual(TEXT("node 3"), NodeIds[3], static_cast<uint64>(400));
	}

	// --- normals ------------------------------------------------------------
	if (TestEqual(TEXT("one normal per vertex"), Reader.GetNormals().Num(), 4))
	{
		TestEqual(TEXT("n0 is +Z"), Reader.GetNormals()[0], FVector3f(0.0f, 0.0f, 1.0f));
	}

	// --- bounds -------------------------------------------------------------
	const FBox3f Bounds = Reader.ComputeBounds();
	TestTrue(TEXT("bounds are valid"), Bounds.IsValid != 0);
	TestEqual(TEXT("bounds min"), Bounds.Min, FVector3f(0.0f, 0.0f, 0.0f));
	TestEqual(TEXT("bounds max"), Bounds.Max, FVector3f(1.0f, 1.0f, 0.0f));

	return true;
}

/* -------------------------------------------------------------------------- */
/* float64 positions and absent arrays                                          */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizMeshReaderFloat64Test,
	"FlowViz.CFDViz.MeshReader.Float64",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizMeshReaderFloat64Test::RunTest(const FString& Parameters)
{
	FCFDVizMeshLoadOptions Options;
	Options.bRetainDoublePrecisionPositions = true;

	FCFDVizMeshReader Reader;
	const FCFDVizResult Result = Reader.LoadFromMemory(
		TArrayView<const uint8>(Float64CvmBytes, UE_ARRAY_COUNT(Float64CvmBytes)), TEXT("f64.cvm"), Options);

	if (!TestTrue(TEXT("the float64 mesh loads"), Result.IsOk()))
	{
		AddError(FString::Printf(TEXT("load failed: %s"), *Result.Message));
		return false;
	}

	const FCFDVizMeshHeader& Header = Reader.GetHeader();
	TestTrue(TEXT("flag bit 3 -> float64 positions"), Header.ArePositionsFloat64());
	TestEqual(TEXT("float64 stride is 24 bytes, not 12"), Header.GetPositionVertexBytes(), static_cast<int64>(24));

	// Section 5.2: an absent array's offset MUST be zero and MUST be ignored.
	TestFalse(TEXT("no normals"), Header.HasNormals());
	TestFalse(TEXT("no patch IDs"), Header.HasPatchIds());
	TestFalse(TEXT("no node IDs"), Header.HasNodeIds());
	TestEqual(TEXT("absent normals offset is 0"), static_cast<int64>(Header.NormalsOffset), static_cast<int64>(0));
	TestEqual(TEXT("absent patchIds offset is 0"), static_cast<int64>(Header.PatchIdsOffset), static_cast<int64>(0));
	TestEqual(TEXT("absent nodeIds offset is 0"), static_cast<int64>(Header.NodeIdsOffset), static_cast<int64>(0));
	TestEqual(TEXT("absent arrays load empty"), Reader.GetNormals().Num(), 0);
	TestEqual(TEXT("absent patch IDs load empty"), Reader.GetPatchIds().Num(), 0);
	TestEqual(TEXT("absent node IDs load empty"), Reader.GetNodeIds().Num(), 0);

	// The double-precision positions must be bit-exact, because the whole point
	// of retaining them is that the float32 copy is lossy.
	const TArray<FVector>& Doubles = Reader.GetDoublePositions();
	if (TestEqual(TEXT("3 double-precision vertices retained"), Doubles.Num(), 3))
	{
		TestEqual(TEXT("v0.x is exactly 1.5"), Doubles[0].X, 1.5);
		TestEqual(TEXT("v0.y is exactly -2.5"), Doubles[0].Y, -2.5);
		TestEqual(TEXT("v0.z is exactly 3.25"), Doubles[0].Z, 3.25);
		TestEqual(TEXT("v2.x survives as 1e300"), Doubles[2].X, 1.0e300);
		TestEqual(TEXT("v2.y survives as -1e300"), Doubles[2].Y, -1.0e300);

		// 0.1 has no exact binary representation. Comparing the BITS rather than
		// the value is what proves no arithmetic touched it: a reader that
		// round-tripped through float32 would land on a different double here
		// and still pass a tolerance-based comparison.
		const uint64 ExpectedBits = 0x3FB999999999999AULL;
		uint64 ActualBits = 0;
		const double Stored = Doubles[2].Z;
		FMemory::Memcpy(&ActualBits, &Stored, 8);
		TestEqual(TEXT("0.1 survives bit-exactly"), ActualBits, ExpectedBits);
	}

	// 1e300 is representable in float64 and NOT in float32, so narrowing must
	// overflow to infinity. Asserting this makes the lossy narrowing visible
	// rather than letting it hide behind values that happen to survive.
	const TArray<FVector3f>& Narrowed = Reader.GetPositions();
	if (TestEqual(TEXT("3 narrowed vertices"), Narrowed.Num(), 3))
	{
		TestEqual(TEXT("1.5 narrows exactly"), Narrowed[0].X, 1.5f);
		TestTrue(TEXT("1e300 narrows to +inf, and the reader does not pretend otherwise"),
			Narrowed[2].X > 0.0f && !FMath::IsFinite(Narrowed[2].X));
		TestTrue(TEXT("-1e300 narrows to -inf"),
			Narrowed[2].Y < 0.0f && !FMath::IsFinite(Narrowed[2].Y));
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Field-offset probes                                                          */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizMeshReaderOffsetProbeTest,
	"FlowViz.CFDViz.MeshReader.OffsetProbes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizMeshReaderOffsetProbeTest::RunTest(const FString& Parameters)
{
	// A DIFFERENTIAL test, not an assertion of parsed values.
	//
	// "vertexCount parses to 4" passes just as happily when vertexCount and
	// triangleCount are read from each other's offsets, as long as the fixture
	// is symmetric. Here each probe changes ONE field in the file and asserts
	// that exactly that field changed in the parse - so a transposition shows up
	// as the wrong field moving, which a value assertion cannot see.

	FCFDVizMeshHeader Baseline;
	{
		const FCFDVizResult Result = FCFDVizMeshHeader::Parse(
			TArrayView<const uint8>(GoldenCvmBytes, 96), Baseline, TEXT("golden.cvm"));
		if (!TestTrue(TEXT("baseline header parses"), Result.IsOk()))
		{
			return false;
		}
	}

	// vertexCount lives at 24 and nothing else may move when it does.
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, UE_ARRAY_COUNT(GoldenCvmBytes));
		WriteUInt64At(Bytes, 24, 4242);
		ResealHeaderCrc(Bytes);

		FCFDVizMeshHeader Probe;
		const FCFDVizResult Result = FCFDVizMeshHeader::Parse(TArrayView<const uint8>(Bytes.GetData(), 96), Probe, TEXT("probe"));
		if (TestTrue(TEXT("probe at 24 parses"), Result.IsOk()))
		{
			TestEqual(TEXT("offset 24 IS vertexCount"), static_cast<int64>(Probe.VertexCount), static_cast<int64>(4242));
			TestEqual(TEXT("and triangleCount did not move"),
				static_cast<int64>(Probe.TriangleCount), static_cast<int64>(Baseline.TriangleCount));
		}
	}

	// triangleCount lives at 32.
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, UE_ARRAY_COUNT(GoldenCvmBytes));
		WriteUInt64At(Bytes, 32, 1);
		ResealHeaderCrc(Bytes);

		FCFDVizMeshHeader Probe;
		const FCFDVizResult Result = FCFDVizMeshHeader::Parse(TArrayView<const uint8>(Bytes.GetData(), 96), Probe, TEXT("probe"));
		if (TestTrue(TEXT("probe at 32 parses"), Result.IsOk()))
		{
			TestEqual(TEXT("offset 32 IS triangleCount"), static_cast<int64>(Probe.TriangleCount), static_cast<int64>(1));
			TestEqual(TEXT("and vertexCount did not move"),
				static_cast<int64>(Probe.VertexCount), static_cast<int64>(Baseline.VertexCount));
		}
	}

	// The five array offsets are the most transposable block in the header:
	// five consecutive uint64s that all hold plausible offsets. Probing each in
	// turn pins every one of them to its own slot.
	struct FOffsetProbe
	{
		int32 ByteOffset;
		const TCHAR* Name;
		uint64 FCFDVizMeshHeader::* Member;
	};
	const FOffsetProbe Probes[] = {
		{ 40, TEXT("positionsOffset"), &FCFDVizMeshHeader::PositionsOffset },
		{ 48, TEXT("normalsOffset"),   &FCFDVizMeshHeader::NormalsOffset   },
		{ 56, TEXT("indicesOffset"),   &FCFDVizMeshHeader::IndicesOffset   },
		{ 64, TEXT("patchIdsOffset"),  &FCFDVizMeshHeader::PatchIdsOffset  },
		{ 72, TEXT("nodeIdsOffset"),   &FCFDVizMeshHeader::NodeIdsOffset   },
	};

	for (const FOffsetProbe& Probe : Probes)
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, UE_ARRAY_COUNT(GoldenCvmBytes));
		const uint64 Sentinel = 0x0000000000ABCDEFULL;
		WriteUInt64At(Bytes, Probe.ByteOffset, Sentinel);
		ResealHeaderCrc(Bytes);

		FCFDVizMeshHeader Parsed;
		const FCFDVizResult Result = FCFDVizMeshHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), 96), Parsed, TEXT("probe"));
		if (!TestTrue(FString::Printf(TEXT("probe at %d parses"), Probe.ByteOffset), Result.IsOk()))
		{
			continue;
		}

		TestEqual(FString::Printf(TEXT("offset %d IS %s"), Probe.ByteOffset, Probe.Name),
			Parsed.*(Probe.Member), Sentinel);

		// Every OTHER offset must be untouched.
		for (const FOffsetProbe& Other : Probes)
		{
			if (Other.ByteOffset == Probe.ByteOffset)
			{
				continue;
			}
			TestEqual(
				FString::Printf(TEXT("changing %s left %s alone"), Probe.Name, Other.Name),
				Parsed.*(Other.Member), Baseline.*(Other.Member));
		}
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Rejection                                                                    */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizMeshReaderRejectionTest,
	"FlowViz.CFDViz.MeshReader.Rejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizMeshReaderRejectionTest::RunTest(const FString& Parameters)
{
	const int32 GoldenSize = UE_ARRAY_COUNT(GoldenCvmBytes);

	// --- a corrupt header CRC must be REJECTED, not warned about ------------
	// This is the check that proves the CRC is actually being verified. Without
	// a deliberately corrupted fixture, a reader that never checked the CRC
	// would pass every other test in this file.
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
		Bytes[24] ^= 0xFF; // vertexCount, without re-sealing the CRC

		FCFDVizMeshReader Reader;
		const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("corrupt.cvm"));
		TestFalse(TEXT("a corrupted header is rejected"), Result.IsOk());
		TestEqual(TEXT("and reported as a header CRC mismatch"),
			Result.Error, ECFDVizError::HeaderCrcMismatch);
		TestFalse(TEXT("the reader is left unloaded"), Reader.IsLoaded());
	}

	// --- bad magic ----------------------------------------------------------
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
		Bytes[0] = 'X';
		ResealHeaderCrc(Bytes);

		FCFDVizMeshReader Reader;
		const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("notacvm"));
		TestFalse(TEXT("a file that is not a CVM is rejected"), Result.IsOk());
		TestEqual(TEXT("reported as bad magic"), Result.Error, ECFDVizError::BadMagic);
	}

	// --- byte-swapped endian marker: REJECTED, never swapped ----------------
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
		Bytes[12] = 0x01; Bytes[13] = 0x02; Bytes[14] = 0x03; Bytes[15] = 0x04;
		ResealHeaderCrc(Bytes);

		FCFDVizMeshReader Reader;
		const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("bigendian.cvm"));
		TestFalse(TEXT("a big-endian file is rejected, not silently swapped"), Result.IsOk());
		TestEqual(TEXT("reported as unsupported endianness"),
			Result.Error, ECFDVizError::UnsupportedEndianness);
	}

	// --- unsupported major version ------------------------------------------
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
		Bytes[8] = 2;
		ResealHeaderCrc(Bytes);

		FCFDVizMeshReader Reader;
		const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("v2.cvm"));
		TestFalse(TEXT("a major version 2 file is rejected"), Result.IsOk());
		TestEqual(TEXT("reported as unsupported version"),
			Result.Error, ECFDVizError::UnsupportedVersion);
	}

	// --- a NEWER MINOR version is ACCEPTED (format rule 1.4) ----------------
	// The mirror of the check above. Rejecting a newer minor would make every
	// future additive revision unreadable, so this must not be "fixed" by
	// tightening the version gate.
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
		Bytes[10] = 7;
		ResealHeaderCrc(Bytes);

		FCFDVizMeshReader Reader;
		const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("v1.7.cvm"));
		TestTrue(TEXT("a newer MINOR version is accepted"), Result.IsOk());
		TestEqual(TEXT("and its minor version is reported as stored"),
			static_cast<int32>(Reader.GetHeader().MinorVersion), 7);
	}

	// --- an unknown flag bit ------------------------------------------------
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
		Bytes[16] = 0x07 | 0x40; // an undefined bit alongside the real ones
		ResealHeaderCrc(Bytes);

		FCFDVizMeshReader Reader;
		const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("future.cvm"));
		TestFalse(TEXT("an unknown flag bit is rejected rather than ignored"), Result.IsOk());
	}

	// --- an out-of-range triangle index -------------------------------------
	// Section 5.3 requires every index to be < vertexCount and requires the
	// reader to validate it. An unvalidated index is a buffer overrun in the
	// renderer, which is why this is a hard rejection and not a clamp.
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
		WriteUInt32At(Bytes, 192, 4); // vertexCount is 4, so index 4 is one past the end
		ResealHeaderCrc(Bytes);

		FCFDVizMeshReader Reader;
		const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("badindex.cvm"));
		TestFalse(TEXT("an index == vertexCount is rejected"), Result.IsOk());
		TestFalse(TEXT("and nothing is left loaded"), Reader.IsLoaded());
	}

	// --- a non-zero offset behind a clear presence bit -----------------------
	// Section 5.2 says such an offset must be ignored; this reader rejects it,
	// because a writer that emitted one is buggy and honouring it later would
	// read whatever happens to sit there.
	{
		TArray<uint8> Bytes = CopyOf(Float64CvmBytes, UE_ARRAY_COUNT(Float64CvmBytes));
		WriteUInt64At(Bytes, 48, 96); // normalsOffset non-zero while bit 0 is clear
		ResealHeaderCrc(Bytes);

		FCFDVizMeshReader Reader;
		const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("ghostnormals.cvm"));
		TestFalse(TEXT("a non-zero offset behind a clear flag is rejected"), Result.IsOk());
	}

	// --- truncation, at every array boundary --------------------------------
	// Each of these cuts the file inside a different array, so a missing bounds
	// check on any one array surfaces as its own failure rather than being
	// masked by the first one.
	{
		const int32 TruncationPoints[] = { 0, 50, 95, 100, 150, 200, 220, 230, GoldenSize - 1 };
		for (int32 Length : TruncationPoints)
		{
			TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, Length);

			FCFDVizMeshReader Reader;
			const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("truncated.cvm"));
			TestFalse(FString::Printf(TEXT("a %d byte file is rejected"), Length), Result.IsOk());
			TestFalse(FString::Printf(TEXT("nothing is loaded from %d bytes"), Length), Reader.IsLoaded());
		}
	}

	// --- a header claiming an impossible vertex count ------------------------
	// The allocation guard: this must fail on the bounds check, without first
	// trying to reserve room for four billion vertices.
	{
		TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
		WriteUInt64At(Bytes, 24, 0xFFFFFFFFULL);
		ResealHeaderCrc(Bytes);

		FCFDVizMeshReader Reader;
		const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("huge.cvm"));
		TestFalse(TEXT("an impossible vertexCount is rejected"), Result.IsOk());
	}

	// --- headerBytes disagreeing with the spec ------------------------------
	// The reader rejects any headerBytes != 96, and nothing tested it. This is
	// the field a writer from a future revision would grow, and honouring a
	// declared size while parsing at fixed spec offsets reads every subsequent
	// field from the wrong place. Both directions are checked because a reader
	// that clamped instead of rejecting would pass a one-sided test.
	{
		const uint32 WrongSizes[] = { 0, 64, 95, 97, 128, 0xFFFFFFFFu };
		for (uint32 Wrong : WrongSizes)
		{
			TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
			WriteUInt32At(Bytes, 20, Wrong);
			ResealHeaderCrc(Bytes);

			FCFDVizMeshReader Reader;
			const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("headerbytes.cvm"));
			TestFalse(FString::Printf(TEXT("headerBytes = %u is rejected"), Wrong), Result.IsOk());
			TestEqual(FString::Printf(TEXT("headerBytes = %u reports InvalidHeader"), Wrong),
				Result.Error, ECFDVizError::InvalidHeader);
		}

		// The control: the correct value must still load. Without it, a reader
		// that rejected EVERY file would pass all six assertions above.
		TArray<uint8> Good = CopyOf(GoldenCvmBytes, GoldenSize);
		WriteUInt32At(Good, 20, 96);
		ResealHeaderCrc(Good);

		FCFDVizMeshReader Reader;
		TestTrue(TEXT("headerBytes = 96 still loads"),
			Reader.LoadFromMemory(Good, TEXT("ok.cvm")).IsOk());
	}

	// --- a non-zero reserved byte -------------------------------------------
	// Section 5.1 fixes reserved[12] at zero. The reader enforces it and no test
	// covered it. Every one of the twelve is checked separately: a loop bound
	// that stopped one short would leave the last byte unguarded, and that is
	// precisely the off-by-one this file's other loops are written to catch.
	{
		for (int32 Index = 0; Index < 12; ++Index)
		{
			TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
			Bytes[84 + Index] = 0x01;
			ResealHeaderCrc(Bytes);

			FCFDVizMeshReader Reader;
			const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("reserved.cvm"));
			TestFalse(FString::Printf(TEXT("a non-zero reserved byte %d is rejected"), Index),
				Result.IsOk());
			TestEqual(FString::Printf(TEXT("reserved byte %d reports InvalidHeader"), Index),
				Result.Error, ECFDVizError::InvalidHeader);
		}
	}

	// --- an array offset that overlaps the header ---------------------------
	// A non-empty array starting inside the 96-byte header means the array and
	// the header describe the same bytes. The reader rejects it; nothing tested
	// it. Offset 0 is included because it is the value the format uses to mean
	// "absent" - a non-empty array at 0 is a header claiming its own magic is
	// vertex data.
	{
		const uint64 OverlappingOffsets[] = { 0, 1, 40, 95 };
		for (uint64 Offset : OverlappingOffsets)
		{
			TArray<uint8> Bytes = CopyOf(GoldenCvmBytes, GoldenSize);
			WriteUInt64At(Bytes, 40, Offset); // positionsOffset, which is never empty here
			ResealHeaderCrc(Bytes);

			FCFDVizMeshReader Reader;
			const FCFDVizResult Result = Reader.LoadFromMemory(Bytes, TEXT("overlap.cvm"));
			TestFalse(FString::Printf(
					TEXT("a non-empty positions array at offset %llu is rejected"),
					static_cast<unsigned long long>(Offset)),
				Result.IsOk());
			TestFalse(TEXT("and nothing is left loaded"), Reader.IsLoaded());
		}

		// The boundary that must still be ACCEPTED. 96 is the first legal byte,
		// so a check written as `Offset <= CvmHeaderBytes` rather than `<` would
		// reject the golden file itself - this assertion is what distinguishes
		// the two.
		TArray<uint8> AtBoundary = CopyOf(GoldenCvmBytes, GoldenSize);
		WriteUInt64At(AtBoundary, 40, 96);
		ResealHeaderCrc(AtBoundary);

		FCFDVizMeshReader Reader;
		TestTrue(TEXT("positions at offset 96, the first legal byte, is accepted"),
			Reader.LoadFromMemory(AtBoundary, TEXT("boundary.cvm")).IsOk());
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Patch extraction                                                             */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizMeshReaderPatchTest,
	"FlowViz.CFDViz.MeshReader.Patches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizMeshReaderPatchTest::RunTest(const FString& Parameters)
{
	FCFDVizMeshReader Reader;
	const FCFDVizResult Result = Reader.LoadFromMemory(
		TArrayView<const uint8>(GoldenCvmBytes, UE_ARRAY_COUNT(GoldenCvmBytes)), TEXT("golden.cvm"));
	if (!TestTrue(TEXT("the golden mesh loads"), Result.IsOk()))
	{
		return false;
	}

	TArray<uint32> Unique;
	Reader.GetUniquePatchIds(Unique);
	if (TestEqual(TEXT("two distinct patches"), Unique.Num(), 2))
	{
		TestEqual(TEXT("ascending: 7 first"), Unique[0], static_cast<uint32>(7));
		TestEqual(TEXT("then 9"), Unique[1], static_cast<uint32>(9));
	}

	TestEqual(TEXT("patch 7 has one triangle"), Reader.CountTrianglesInPatch(7), 1);
	TestEqual(TEXT("patch 9 has one triangle"), Reader.CountTrianglesInPatch(9), 1);
	TestEqual(TEXT("an absent patch has none"), Reader.CountTrianglesInPatch(1234), 0);

	// Extracting patch 9 (triangle 1, vertices 1,3,2) must renumber from zero
	// while PRESERVING corner order - the extracted mesh has to keep the section
	// 5.3 winding or it renders inside-out.
	{
		TArray<FVector3f> Positions;
		TArray<uint32> Indices;
		const FCFDVizResult PatchResult = Reader.BuildForPatch(9, Positions, Indices);
		if (TestTrue(TEXT("patch 9 extracts"), PatchResult.IsOk()))
		{
			TestEqual(TEXT("only the 3 referenced vertices are copied"), Positions.Num(), 3);
			TestEqual(TEXT("one triangle"), Indices.Num(), 3);

			// The source triangle is (1,3,2) -> positions (1,0,0) (1,1,0) (0,1,0).
			// Checking the POSITIONS the indices resolve to, rather than the
			// index values, is what makes this independent of which order the
			// extractor happened to assign new vertex numbers.
			TestEqual(TEXT("corner 0 is the old v1"), Positions[Indices[0]], FVector3f(1.0f, 0.0f, 0.0f));
			TestEqual(TEXT("corner 1 is the old v3"), Positions[Indices[1]], FVector3f(1.0f, 1.0f, 0.0f));
			TestEqual(TEXT("corner 2 is the old v2"), Positions[Indices[2]], FVector3f(0.0f, 1.0f, 0.0f));
		}
	}

	// An absent patch must FAIL rather than yield an empty mesh: an empty result
	// is indistinguishable from a patch the user hid, and the caller would
	// render nothing while believing the case was fine.
	{
		TArray<FVector3f> Positions;
		TArray<uint32> Indices;
		const FCFDVizResult PatchResult = Reader.BuildForPatch(1234, Positions, Indices);
		TestFalse(TEXT("an absent patch fails rather than returning empty"), PatchResult.IsOk());
		TestEqual(TEXT("and leaves no positions behind"), Positions.Num(), 0);
		TestEqual(TEXT("and no indices"), Indices.Num(), 0);
	}

	// Bounds-checked accessors.
	{
		uint32 A = 0, B = 0, C = 0;
		TestTrue(TEXT("triangle 0 reads"), Reader.TryGetTriangle(0, A, B, C));
		TestFalse(TEXT("triangle 2 is out of range"), Reader.TryGetTriangle(2, A, B, C));
		TestFalse(TEXT("a negative triangle is out of range"), Reader.TryGetTriangle(-1, A, B, C));

		uint32 PatchId = 0;
		TestTrue(TEXT("triangle 1's patch reads"), Reader.TryGetTrianglePatchId(1, PatchId));
		TestEqual(TEXT("and it is 9"), PatchId, static_cast<uint32>(9));
		TestFalse(TEXT("patch of triangle 2 is out of range"), Reader.TryGetTrianglePatchId(2, PatchId));

		uint64 NodeId = 0;
		TestTrue(TEXT("vertex 3's node ID reads"), Reader.TryGetVertexNodeId(3, NodeId));
		TestEqual(TEXT("and it is 400"), NodeId, static_cast<uint64>(400));
		TestFalse(TEXT("vertex 4 is out of range"), Reader.TryGetVertexNodeId(4, NodeId));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
