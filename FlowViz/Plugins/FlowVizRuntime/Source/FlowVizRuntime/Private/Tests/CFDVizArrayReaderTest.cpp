// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizArrayReader.h"
#include "CFDViz/CFDVizCrc32C.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * CVA reader conformance (format section 6).
 *
 * BOTH FIXTURES BELOW ARE BYTES THE PYTHON REFERENCE WRITER ACTUALLY PRODUCED.
 * They were captured from `cfdviz.cva.write_cva(...)` and pasted verbatim, which
 * makes these tests a genuine cross-implementation check: the offsets, the
 * 8-byte section alignment, the zlib stream, and every CRC come from the other
 * implementation, so a disagreement about the spec shows up here rather than in
 * a user's case file. A round-trip through this reader alone could not detect
 * that class of bug at all.
 */

namespace
{
	/**
	 * 5 vertices x 3 components, float32, zlib, per-frame statistics.
	 *
	 * Values: (1,2,3) (4,5,6) (NaN,-1.5,0.25) (-7,8,9) (0,0,0).
	 * The NaN in component 0 is the point of the fixture: it must survive the
	 * payload bit-exactly AND be excluded from the statistics, so validCount for
	 * component 0 is 4 while components 1 and 2 report 5.
	 *
	 * From `write_cva(values, frame_index=7, simulation_time=0.5,
	 *                 association='mesh-vertex', field_numeric_id=42,
	 *                 dtype='float32', codec=3)`.
	 */
	const uint8 GoldenCvaBytes[] = {
		// ---- header, offset 0 ------------------------------------------------
		0x43, 0x46, 0x44, 0x41, 0x52, 0x52, 0x31, 0x00, // 0   magic "CFDARR1\0"
		0x01, 0x00,                                     // 8   majorVersion = 1
		0x00, 0x00,                                     // 10  minorVersion = 0
		0x04, 0x03, 0x02, 0x01,                         // 12  endianMarker
		0x01, 0x00, 0x00, 0x00,                         // 16  flags = frame stats
		0x60, 0x00, 0x00, 0x00,                         // 20  headerBytes = 96
		0x07, 0x00, 0x00, 0x00,                         // 24  frameIndex = 7
		0x2A, 0x00, 0x00, 0x00,                         // 28  fieldNumericId = 42
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x3F, // 32  simulationTime = 0.5
		0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 40  valueCount = 5
		0x03,                                           // 48  componentCount = 3
		0x02,                                           // 49  dataType = float32
		0x01,                                           // 50  association = vertex
		0x03,                                           // 51  codec = zlib
		0x99, 0x0A, 0xC1, 0xAE,                         // 52  payloadCrc32c
		0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 56  payloadOffset = 200
		0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 64  compressedBytes = 46
		0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 72  uncompressedBytes = 60
		0x2D, 0xB3, 0x7A, 0xF9,                         // 80  headerCrc32c
		0x00, 0x00, 0x00, 0x00,                         // 84  reserved = 0
		0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 88  statisticsOffset = 96

		// ---- frame statistics, offset 96: 8 + 32*3 = 104 bytes ---------------
		0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0    valueCount = 5
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0xC0, // 8    min[0] = -7
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0xBF, // 16   min[1] = -1.5
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 24   min[2] = 0
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 32   max[0] = 4
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x40, // 40   max[1] = 8
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x40, // 48   max[2] = 9
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0xBF, // 56   mean[0] = -0.5
		0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0x05, 0x40, // 64   mean[1] = 2.7
		0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x0D, 0x40, // 72   mean[2] = 3.65
		0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 80   validCount[0] = 4
		0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 88   validCount[1] = 5
		0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 96   validCount[2] = 5

		// ---- payload, offset 200: 60 bytes of float32 through zlib level 6 ---
		0x78, 0x9C, 0x63, 0x60, 0x68, 0xB0, 0x67, 0x60, 0x60, 0x70, 0x00, 0x22,
		0x20, 0x6E, 0x00, 0xE2, 0x05, 0x40, 0x7C, 0x00, 0x84, 0xEB, 0x81, 0x78,
		0x3F, 0x50, 0xCC, 0x8E, 0x81, 0xE1, 0xC1, 0x01, 0xA0, 0x1A, 0x47, 0x06,
		0x06, 0x01, 0x20, 0x46, 0x00, 0x00, 0x52, 0x60, 0x09, 0xCE,
	};

	static_assert(sizeof(GoldenCvaBytes) == 246, "golden CVA fixture must stay 96 + 104 + 46 bytes");

	/**
	 * 3 elements x 1 component, float16, codec none, EVERY value NaN, with both
	 * a frame and a global statistics section.
	 *
	 * This is the degenerate case the format calls out explicitly: with no valid
	 * samples, minimum is +inf, maximum is -inf and mean is NaN (4.4.7). Those
	 * are sentinels, not a range - a colour scale built from +inf..-inf is
	 * inverted and meaningless, so TryGetComponentRange has to refuse it.
	 *
	 * Note the two sections back to back at offset 96 and 136: frame first, then
	 * global (6.4). Reading them in the other order would swap two blocks that
	 * are here byte-identical, which is exactly why the fixture below also
	 * carries the frame/global flag bits for the reader to key off.
	 */
	const uint8 AllNanCvaBytes[] = {
		0x43, 0x46, 0x44, 0x41, 0x52, 0x52, 0x31, 0x00, // 0   magic
		0x01, 0x00, 0x00, 0x00,                         // 8   version 1.0
		0x04, 0x03, 0x02, 0x01,                         // 12  endianMarker
		0x03, 0x00, 0x00, 0x00,                         // 16  flags = frame|global
		0x60, 0x00, 0x00, 0x00,                         // 20  headerBytes = 96
		0x00, 0x00, 0x00, 0x00,                         // 24  frameIndex = 0
		0x03, 0x00, 0x00, 0x00,                         // 28  fieldNumericId = 3
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 32  simulationTime = 0
		0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 40  valueCount = 3
		0x01,                                           // 48  componentCount = 1
		0x01,                                           // 49  dataType = float16
		0x00,                                           // 50  association = element
		0x00,                                           // 51  codec = none
		0x66, 0x8D, 0x72, 0xC3,                         // 52  payloadCrc32c
		0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 56  payloadOffset = 176
		0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 64  compressedBytes = 6
		0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 72  uncompressedBytes = 6
		0xD8, 0x8C, 0xBC, 0x8D,                         // 80  headerCrc32c
		0x00, 0x00, 0x00, 0x00,                         // 84  reserved = 0
		0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 88  statisticsOffset = 96

		// frame statistics, offset 96: 8 + 32 = 40 bytes
		0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // valueCount = 3
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x7F, // min = +inf
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0xFF, // max = -inf
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x7F, // mean = NaN
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // validCount = 0

		// global statistics, offset 136
		0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x7F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0xFF,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x7F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		// payload, offset 176: three float16 NaNs
		0x00, 0x7E, 0x00, 0x7E, 0x00, 0x7E,
	};

	static_assert(sizeof(AllNanCvaBytes) == 182, "all-NaN CVA fixture must stay 96 + 40 + 40 + 6 bytes");

	TArray<uint8> MakeGoldenCva()
	{
		return TArray<uint8>(GoldenCvaBytes, UE_ARRAY_COUNT(GoldenCvaBytes));
	}

	TArray<uint8> MakeAllNanCva()
	{
		return TArray<uint8>(AllNanCvaBytes, UE_ARRAY_COUNT(AllNanCvaBytes));
	}

	/** Section 6.1: CRC-32C over [0,96) with [80,84) zeroed - the same rule as CVM. */
	void ResealCvaHeaderCrc(TArray<uint8>& Bytes)
	{
		check(Bytes.Num() >= CFDViz::CvaHeaderBytes);
		FMemory::Memzero(Bytes.GetData() + CFDViz::CvaHeaderCrcOffset, CFDViz::HeaderCrcFieldBytes);
		const uint32 Crc = CFDViz::Crc32C::Compute(Bytes.GetData(), CFDViz::CvaHeaderBytes);
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes[CFDViz::CvaHeaderCrcOffset + Index] = static_cast<uint8>((Crc >> (8 * Index)) & 0xFF);
		}
	}

	void PokeUInt32(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (8 * Index)) & 0xFF);
		}
	}

	void PokeUInt64(TArray<uint8>& Bytes, int32 Offset, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (8 * Index)) & 0xFF);
		}
	}
}

/* -------------------------------------------------------------------------- */
/* Header layout                                                                */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizArrayLayoutTest,
	"FlowViz.CFDViz.ArrayReader.Layout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizArrayLayoutTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Bytes = MakeGoldenCva();
	const FCFDVizMemoryByteSource Source(Bytes, TEXT("golden.cva"));

	FCFDVizArrayData Array;
	const FCFDVizResult Result = FCFDVizArrayReader::Read(Source, Array);
	if (!TestTrue(TEXT("the Python-written fixture reads"), Result.IsOk()))
	{
		AddError(Result.ToString());
		return false;
	}

	const FCFDVizArrayHeader& Header = Array.Header;

	// Section 6.1, field by field. Note that CVA orders version, endian marker
	// and headerBytes DIFFERENTLY from CVF: major/minor at 8/10, endian at 12,
	// headerBytes at 20. CVF puts headerBytes at 8 and the marker at 16. A
	// reader that reuses the CVF field order here parses a plausible-looking
	// header full of wrong numbers.
	TestEqual(TEXT("majorVersion @8"), static_cast<int32>(Header.MajorVersion), 1);
	TestEqual(TEXT("minorVersion @10"), static_cast<int32>(Header.MinorVersion), 0);
	TestEqual(TEXT("flags @16"), static_cast<int32>(Header.Flags), 1);
	TestEqual(TEXT("headerBytes @20"), static_cast<int32>(Header.HeaderBytes), 96);
	TestEqual(TEXT("frameIndex @24"), static_cast<int32>(Header.FrameIndex), 7);
	TestEqual(TEXT("fieldNumericId @28"), static_cast<int32>(Header.FieldNumericId), 42);
	TestEqual(TEXT("simulationTime @32"), Header.SimulationTime, 0.5);
	TestEqual(TEXT("valueCount @40"), Header.ValueCount, static_cast<int64>(5));
	TestEqual(TEXT("componentCount @48"), Header.ComponentCount, 3);
	TestTrue(TEXT("dataType @49 is float32"), Header.DataType == ECFDVizDataType::Float32);
	TestTrue(TEXT("association @50 is mesh-vertex"), Header.Association == ECFDVizAssociation::Point);
	TestTrue(TEXT("codec @51 is zlib"), Header.Codec == ECFDVizCodec::Zlib);
	TestEqual(TEXT("payloadCrc32c @52"), static_cast<int64>(Header.PayloadCrc32C), static_cast<int64>(0xAEC10A99u));
	TestEqual(TEXT("payloadOffset @56"), Header.PayloadOffset, static_cast<int64>(200));
	TestEqual(TEXT("compressedBytes @64"), Header.CompressedBytes, static_cast<int64>(46));
	TestEqual(TEXT("uncompressedBytes @72"), Header.UncompressedBytes, static_cast<int64>(60));
	TestEqual(TEXT("headerCrc32c @80"), static_cast<int64>(Header.HeaderCrc32C), static_cast<int64>(0xF97AB32Du));
	TestEqual(TEXT("statisticsOffset @88"), Header.StatisticsOffset, static_cast<int64>(96));

	// The flags decode to the section that is actually present.
	TestTrue(TEXT("frame statistics flag set"), Header.HasFrameStatistics());
	TestFalse(TEXT("global statistics flag clear"), Header.HasGlobalStatistics());

	// Bytes [0,24) are declared identical in meaning and position to a CVM
	// header (6.1), and the header CRC sits at [80,84) in both. Asserted here so
	// a future "tidy-up" of either layout fails loudly.
	TestEqual(TEXT("headerCrc lives at the same offset as CVM's"),
		CFDViz::CvaHeaderCrcOffset, CFDViz::CvmHeaderCrcOffset);
	TestEqual(TEXT("CVA and CVM headers are the same size"),
		CFDViz::CvaHeaderBytes, CFDViz::CvmHeaderBytes);

	// Payload: interleaved per entity, decoded from the Python-written zlib
	// stream. Entity 1 component 0 must be 4.0 - if the reader treated the data
	// as planar it would return -1.5 here with no error.
	TestEqual(TEXT("payload decodes to 60 bytes"), Array.Bytes.Num(), 60);
	double Value = 0.0;
	TestTrue(TEXT("(0,0) readable"), Array.TryGetValue(0, 0, Value));
	TestEqual(TEXT("entity 0 component 0"), Value, 1.0);
	TestTrue(TEXT("(0,2) readable"), Array.TryGetValue(0, 2, Value));
	TestEqual(TEXT("entity 0 component 2"), Value, 3.0);
	TestTrue(TEXT("(1,0) readable"), Array.TryGetValue(1, 0, Value));
	TestEqual(TEXT("components are interleaved per entity"), Value, 4.0);
	TestTrue(TEXT("(3,0) readable"), Array.TryGetValue(3, 0, Value));
	TestEqual(TEXT("entity 3 component 0"), Value, -7.0);

	// The NaN survives bit-exactly (6.3.4). Compared as bits: NaN != NaN, so a
	// float comparison would pass against any NaN at all and also against a
	// reader that swapped the payload for a different one.
	uint64 Bits = 0;
	TestTrue(TEXT("(2,0) readable as bits"), Array.TryGetValueBits(2, 0, Bits));
	TestEqual(TEXT("NaN keeps its exact float32 bit pattern"), Bits, static_cast<uint64>(0x7FC00000u));
	TestTrue(TEXT("(2,1) readable"), Array.TryGetValue(2, 1, Value));
	TestEqual(TEXT("the value beside the NaN is untouched"), Value, -1.5);

	// Out-of-range indexing is refused, not wrapped.
	TestEqual(TEXT("entity past valueCount"), Array.GetElementIndex(5, 0), static_cast<int64>(INDEX_NONE));
	TestEqual(TEXT("component past componentCount"), Array.GetElementIndex(0, 3), static_cast<int64>(INDEX_NONE));

	// Statistics (6.4), read from the section the Python writer emitted.
	if (TestTrue(TEXT("frame statistics were read"), Array.FrameStatistics.IsSet()))
	{
		const FCFDVizArrayStatistics& Stats = Array.FrameStatistics.GetValue();
		TestEqual(TEXT("statistics valueCount @0"), Stats.ValueCount, static_cast<int64>(5));
		TestEqual(TEXT("statistics component count"), Stats.GetComponentCount(), 3);
		TestEqual(TEXT("minimum[0] @8"), Stats.Minimum[0], -7.0);
		TestEqual(TEXT("minimum[1]"), Stats.Minimum[1], -1.5);
		TestEqual(TEXT("maximum[0] @8+8C"), Stats.Maximum[0], 4.0);
		TestEqual(TEXT("maximum[2]"), Stats.Maximum[2], 9.0);
		TestEqual(TEXT("mean[0] @8+16C"), Stats.Mean[0], -0.5);
		// validCount is the field that proves NaN was excluded rather than
		// averaged: component 0 has one NaN out of five, the others have none.
		TestEqual(TEXT("validCount[0] @8+24C excludes the NaN"), static_cast<int64>(Stats.ValidCount[0]), static_cast<int64>(4));
		TestEqual(TEXT("validCount[1] counts every sample"), static_cast<int64>(Stats.ValidCount[1]), static_cast<int64>(5));
		TestEqual(TEXT("validCount[2] counts every sample"), static_cast<int64>(Stats.ValidCount[2]), static_cast<int64>(5));

		// And the mean really is the NaN-excluding mean: (1+4-7+0)/4 = -0.5, not
		// (1+4+NaN-7+0)/5, which would be NaN.
		TestFalse(TEXT("mean is not poisoned by the NaN"), FMath::IsNaN(Stats.Mean[0]));

		double Min = 0.0;
		double Max = 0.0;
		TestTrue(TEXT("component 0 has a usable range"), Stats.TryGetComponentRange(0, Min, Max));
		TestEqual(TEXT("range min"), Min, -7.0);
		TestEqual(TEXT("range max"), Max, 4.0);
		TestFalse(TEXT("an out-of-range component index is refused"), Stats.TryGetComponentRange(3, Min, Max));
	}

	// Section size arithmetic, 8 + 32C.
	TestEqual(TEXT("statistics section is 8 + 32C bytes"),
		FCFDVizArrayStatistics::GetSectionBytes(3), static_cast<int64>(104));
	TestEqual(TEXT("scalar statistics section"),
		FCFDVizArrayStatistics::GetSectionBytes(1), static_cast<int64>(40));

	return true;
}

/* -------------------------------------------------------------------------- */
/* The all-NaN sentinel and both statistics sections                            */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizArrayNoValidDataTest,
	"FlowViz.CFDViz.ArrayReader.NoValidData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizArrayNoValidDataTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Bytes = MakeAllNanCva();
	const FCFDVizMemoryByteSource Source(Bytes, TEXT("allnan.cva"));

	FCFDVizArrayData Array;
	const FCFDVizResult Result = FCFDVizArrayReader::Read(Source, Array);
	if (!TestTrue(TEXT("the all-NaN fixture reads"), Result.IsOk()))
	{
		AddError(Result.ToString());
		return false;
	}

	TestTrue(TEXT("dataType float16"), Array.Header.DataType == ECFDVizDataType::Float16);
	TestTrue(TEXT("association mesh-element"), Array.Header.Association == ECFDVizAssociation::Cell);
	TestTrue(TEXT("codec none"), Array.Header.Codec == ECFDVizCodec::None);
	TestEqual(TEXT("payload is 6 bytes"), Array.Bytes.Num(), 6);

	// float16 NaN must survive as 0x7E00. Widening through a float32 with naive
	// exponent handling is the usual way this becomes an Inf or a large finite.
	for (int64 Entity = 0; Entity < 3; ++Entity)
	{
		uint64 Bits = 0;
		TestTrue(TEXT("half readable"), Array.TryGetValueBits(Entity, 0, Bits));
		TestEqual(FString::Printf(TEXT("entity %lld keeps the float16 NaN pattern"), Entity),
			Bits, static_cast<uint64>(0x7E00u));
		double Value = 0.0;
		TestTrue(TEXT("half widens"), Array.TryGetValue(Entity, 0, Value));
		TestTrue(TEXT("and is still a NaN as a double"), FMath::IsNaN(Value));
	}

	// Both sections present, frame first then global (6.4).
	TestTrue(TEXT("frame statistics present"), Array.FrameStatistics.IsSet());
	if (!TestTrue(TEXT("global statistics present"), Array.GlobalStatistics.IsSet()))
	{
		return false;
	}

	const FCFDVizArrayStatistics& Frame = Array.FrameStatistics.GetValue();
	TestEqual(TEXT("no valid samples"), static_cast<int64>(Frame.ValidCount[0]), static_cast<int64>(0));
	// The mandated sentinel, 4.4.7. Asserted as exact infinities: a reader that
	// clamped these to finite extremes would produce a range that looks usable.
	TestTrue(TEXT("minimum is +inf"), Frame.Minimum[0] == std::numeric_limits<double>::infinity());
	TestTrue(TEXT("maximum is -inf"), Frame.Maximum[0] == -std::numeric_limits<double>::infinity());
	TestTrue(TEXT("mean is NaN"), FMath::IsNaN(Frame.Mean[0]));

	// And the sentinel must NOT be handed out as a range. +inf..-inf is an
	// inverted axis; every colour lookup against it is meaningless.
	double Min = 0.0;
	double Max = 0.0;
	TestFalse(TEXT("the no-valid-data sentinel is refused as a range"),
		Frame.TryGetComponentRange(0, Min, Max));
	TestEqual(TEXT("and the outputs are left untouched"), Min, 0.0);

	const FCFDVizArrayStatistics& Global = Array.GlobalStatistics.GetValue();
	TestEqual(TEXT("global section also decodes"), Global.ValueCount, static_cast<int64>(3));
	TestEqual(TEXT("global component count"), Global.GetComponentCount(), 1);

	return true;
}

/* -------------------------------------------------------------------------- */
/* Component naming                                                             */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizArrayComponentOrderTest,
	"FlowViz.CFDViz.ArrayReader.ComponentOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizArrayComponentOrderTest::RunTest(const FString& Parameters)
{
	using namespace CFDViz;

	TestTrue(TEXT("1 component is legal"), IsValidCvaComponentCount(1));
	TestTrue(TEXT("3 components are legal"), IsValidCvaComponentCount(3));
	TestTrue(TEXT("6 components are legal"), IsValidCvaComponentCount(6));
	TestTrue(TEXT("9 components are legal"), IsValidCvaComponentCount(9));
	TestFalse(TEXT("0 components is not"), IsValidCvaComponentCount(0));
	TestFalse(TEXT("2 components is not"), IsValidCvaComponentCount(2));
	TestFalse(TEXT("4 components is not"), IsValidCvaComponentCount(4));

	// The symmetric-tensor order is XX, YY, ZZ, XY, YZ, XZ - NOT the other
	// common Voigt order XX, YY, ZZ, YZ, XZ, XY. Section 6.3 calls the
	// difference "the classic way for two solvers to silently disagree": the
	// numbers stay plausible either way, so nothing downstream catches it. This
	// assertion is the only thing standing between the two conventions.
	{
		const TArrayView<const TCHAR* const> Names = GetCvaComponentNames(6);
		if (TestEqual(TEXT("six names for a symmetric tensor"), Names.Num(), 6))
		{
			TestEqual(TEXT("component 0"), FString(Names[0]), FString(TEXT("XX")));
			TestEqual(TEXT("component 1"), FString(Names[1]), FString(TEXT("YY")));
			TestEqual(TEXT("component 2"), FString(Names[2]), FString(TEXT("ZZ")));
			TestEqual(TEXT("component 3 is XY, not YZ"), FString(Names[3]), FString(TEXT("XY")));
			TestEqual(TEXT("component 4 is YZ, not XZ"), FString(Names[4]), FString(TEXT("YZ")));
			TestEqual(TEXT("component 5 is XZ, not XY"), FString(Names[5]), FString(TEXT("XZ")));
		}
	}

	// Full tensors are row-major.
	{
		const TArrayView<const TCHAR* const> Names = GetCvaComponentNames(9);
		if (TestEqual(TEXT("nine names for a full tensor"), Names.Num(), 9))
		{
			TestEqual(TEXT("row-major component 1"), FString(Names[1]), FString(TEXT("XY")));
			TestEqual(TEXT("row-major component 3"), FString(Names[3]), FString(TEXT("YX")));
			TestEqual(TEXT("row-major component 8"), FString(Names[8]), FString(TEXT("ZZ")));
		}
	}

	TestEqual(TEXT("vector names"), GetCvaComponentNames(3).Num(), 3);
	TestEqual(TEXT("scalar names"), GetCvaComponentNames(1).Num(), 1);
	TestEqual(TEXT("an unsupported count has no names"), GetCvaComponentNames(4).Num(), 0);

	return true;
}

/* -------------------------------------------------------------------------- */
/* Rejection paths                                                              */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizArrayRejectionTest,
	"FlowViz.CFDViz.ArrayReader.Rejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizArrayRejectionTest::RunTest(const FString& Parameters)
{
	auto ExpectError = [this](const TCHAR* What, TArray<uint8>& Bytes, ECFDVizError Expected)
	{
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("bad.cva"));
		FCFDVizArrayData Array;
		const FCFDVizResult Result = FCFDVizArrayReader::Read(Source, Array);
		if (Result.Error != Expected)
		{
			AddError(FString::Printf(TEXT("%s: expected %s, got %s (%s)"),
				What, CFDVizErrorToString(Expected), CFDVizErrorToString(Result.Error), *Result.ToString()));
			return;
		}
		TestTrue(FString::Printf(TEXT("%s reports a message"), What), !Result.ToString().IsEmpty());
	};

	// Truncation.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes.SetNum(95);
		ExpectError(TEXT("shorter than the 96-byte header"), Bytes, ECFDVizError::FileTooSmall);
	}
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes.SetNum(150); // header fine, statistics section cut in half
		ExpectError(TEXT("statistics section truncated"), Bytes, ECFDVizError::PayloadOutOfBounds);
	}
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes.SetNum(220); // statistics fine, payload cut short
		ExpectError(TEXT("payload truncated"), Bytes, ECFDVizError::PayloadOutOfBounds);
	}

	// Magic. A CVF handed to the CVA reader is the realistic mix-up.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		FMemory::Memcpy(Bytes.GetData(), CFDViz::CvfMagic, 8);
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("a CVF is not a CVA"), Bytes, ECFDVizError::BadMagic);
	}

	// Endianness: reject, never swap.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt32(Bytes, 12, 0x04030201u);
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("foreign byte order"), Bytes, ECFDVizError::UnsupportedEndianness);
	}

	// Versions: a newer major is refused, a newer minor is accepted (rule 1.4).
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[8] = 2;
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("major version 2"), Bytes, ECFDVizError::UnsupportedVersion);
	}
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[10] = 4;
		ResealCvaHeaderCrc(Bytes);
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("minor.cva"));
		FCFDVizArrayData Array;
		TestTrue(TEXT("a newer minor version is accepted"),
			FCFDVizArrayReader::Read(Source, Array).IsOk());
	}

	// headerBytes must be exactly 96.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt32(Bytes, 20, 128);
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("headerBytes != 96"), Bytes, ECFDVizError::InvalidHeader);
	}

	// reserved @84 must be zero.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt32(Bytes, 84, 1);
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("reserved @84 is non-zero"), Bytes, ECFDVizError::InvalidHeader);
	}

	// Header CRC - deliberately not resealed.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt32(Bytes, 24, 999);
		ExpectError(TEXT("header edited without updating its CRC"), Bytes, ECFDVizError::HeaderCrcMismatch);
	}

	// Payload CRC: one flipped bit inside the zlib stream. The CRC covers the
	// stored bytes, so this is caught before the inflater ever sees it.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[210] ^= 0x01;
		ExpectError(TEXT("single-bit payload corruption"), Bytes, ECFDVizError::PayloadCrcMismatch);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("corrupt.cva"));
		FCFDVizArrayData Array;
		FCFDVizArrayReader::Read(Source, Array);
		TestEqual(TEXT("nothing is returned for a corrupt payload"), Array.Bytes.Num(), 0);
	}

	// Enums. float64 is legal in CVA (unlike CVF) but uint8 is reserved for CVF
	// and must be refused here - the two containers share an enum but not a
	// domain, which is exactly the kind of asymmetry a reader elides.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[49] = 3; // uint8
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("uint8 is reserved for CVF and not offered by CVA"), Bytes, ECFDVizError::UnsupportedDataType);
	}
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[49] = 77;
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("unknown data type"), Bytes, ECFDVizError::UnsupportedDataType);
	}
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[50] = 5;
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("unknown association"), Bytes, ECFDVizError::UnsupportedAssociation);
	}
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[51] = 9;
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("unknown codec"), Bytes, ECFDVizError::UnsupportedCodec);
	}

	// Component counts outside {1,3,6,9}.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[48] = 2;
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("componentCount 2"), Bytes, ECFDVizError::InvalidHeader);
	}
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[48] = 0;
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("componentCount 0"), Bytes, ECFDVizError::InvalidHeader);
	}

	// zstd, with the exact spec message and no fallback.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		Bytes[51] = 1;
		ResealCvaHeaderCrc(Bytes);
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("zstd.cva"));
		FCFDVizArrayData Array;
		const FCFDVizResult Result = FCFDVizArrayReader::Read(Source, Array);
		TestTrue(TEXT("zstd is rejected"), Result.Error == ECFDVizError::UnsupportedCodec);
		TestEqual(TEXT("with exactly the spec's wording"),
			Result.Message, FString(CFDViz::ZstdRejectionMessage));
		TestEqual(TEXT("and no fallback decode happens"), Array.Bytes.Num(), 0);
	}

	// uncompressedBytes must equal valueCount * componentCount * sizeof(dataType),
	// checked before allocating (6.3.2).
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt64(Bytes, 72, 64); // should be 60
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("uncompressedBytes inconsistent with the value count"), Bytes, ECFDVizError::SizeMismatch);
	}
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt64(Bytes, 40, 0x0FFFFFFFFFFFFFFFll); // hostile valueCount
		ResealCvaHeaderCrc(Bytes);
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("huge.cva"));
		FCFDVizArrayData Array;
		const FCFDVizResult Result = FCFDVizArrayReader::Read(Source, Array);
		TestTrue(TEXT("an absurd valueCount is rejected"), !Result.IsOk());
		TestEqual(TEXT("and nothing is allocated"), Array.Bytes.Num(), 0);
	}

	// A statisticsOffset that overlaps the header, and one past the file.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt64(Bytes, 88, 64);
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("statistics overlap the header"), Bytes, ECFDVizError::InvalidHeader);
	}
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt64(Bytes, 88, 100000);
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("statistics offset past the file"), Bytes, ECFDVizError::PayloadOutOfBounds);
	}
	{
		// A statistics section whose own valueCount disagrees with the header's.
		// Silently trusting it would make a viewer scale a 5-entity field by
		// statistics computed over some other number of entities.
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt64(Bytes, 96, 9);
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("statistics valueCount disagrees with the header"), Bytes, ECFDVizError::InvalidHeader);
	}

	// A payload offset that overlaps the header.
	{
		TArray<uint8> Bytes = MakeGoldenCva();
		PokeUInt64(Bytes, 56, 32);
		ResealCvaHeaderCrc(Bytes);
		ExpectError(TEXT("payload overlaps the header"), Bytes, ECFDVizError::InvalidHeader);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
