// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizByteSource.h"
#include "CFDViz/CFDVizCrc32C.h"
#include "CFDViz/CFDVizPayload.h"
#include "CFDViz/CFDVizVolumeReader.h"
#include "Misc/AutomationTest.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * CVF REJECTION AND INTEGRITY (format section 4).
 *
 * SCOPE, AND WHY THIS IS A SEPARATE FILE FROM CFDVizVolumeReaderTest.cpp.
 *
 * That file proves the reader reads a GOOD file correctly: field offsets, decoded
 * values, header CRC, truncation. This one proves it REFUSES a bad one, which is
 * a different property and the one that is easy to leave untested - a reader that
 * ignores every check still passes every happy-path test. Kept separate so the
 * two suites can be edited concurrently without overwriting each other.
 *
 * Covered here and nowhere else in the layer:
 *   - a deliberately corrupted payload, which MUST be rejected (4.4.8)
 *   - uncompressedBytes that disagrees with the brick geometry (4.4.4)
 *   - the reserved zstd codec, refused with the EXACT section 7 wording
 *   - unknown dataType / association / codec byte values
 *   - foreign byte order, rejected rather than swapped (4.1)
 *   - a CVM file handed to the CVF reader
 *   - backgroundValue semantics for an ABSENT brick (4.4.5)
 *
 * WHY THE FIXTURE IS HAND-BUILT BYTES. A write-then-read-back test passes even
 * when reader and writer share the same wrong idea of the layout, which is the
 * bug two implementations exist to catch. Every byte below is transcribed from
 * the section 4.1 / 4.3 offset tables, and the three CRC constants were computed
 * by the independent Python CRC-32C in Tools/cfdviz - never by the code here:
 *
 *     cd FlowViz/Tools/cfdviz && PYTHONPATH=src python3 -c \
 *       "from cfdviz.crc32c import crc32c; ..."
 *
 * That module is itself anchored by the standard CRC-32C check value
 * crc32c(b"123456789") == 0xE3069283, so a bug in it could not silently agree
 * with a matching bug in the C++ implementation under test.
 *
 * EVERY REJECTION IS A ONE-FIELD EDIT of a fixture that is asserted to open
 * first. Without that control, a rejection test would also pass if the fixture
 * were malformed for some unrelated reason, and would prove nothing about the
 * check it names.
 */

/* -------------------------------------------------------------------------- */
/* NAMED NAMESPACE ON PURPOSE. FlowVizRuntime is a unity build: every .cpp in   */
/* the module is concatenated into one translation unit, so an anonymous        */
/* namespace here would NOT be private to this file and these names would       */
/* collide with a sibling test's. See Docs/BUILD.md.                            */
/* -------------------------------------------------------------------------- */
namespace CvfIntegrityTest
{
	/**
	 * A complete, valid CVF file: 128-byte header, two 80-byte directory entries,
	 * two payloads. Codec none, so a mutation's effect is visible in the bytes.
	 *
	 *   dimensions   3 x 2 x 2 cells, brick size 2 x 2 x 1  ->  2 x 1 x 2 bricks
	 *   float32, 3 components, cell-associated
	 *
	 *   brick (0,0,0)  validSize 2x2x1, 4 voxels, values 1..12 with one NaN
	 *   brick (1,0,1)  validSize 1x2x1 - the EDGE brick. dimensionX is 3 and the
	 *                  brick edge is 2, so it stores 1 column, not 2. Edge bricks
	 *                  are not padded (4.4.3). All six values are NaN, so its
	 *                  statistics carry the +inf/-inf "no valid data" sentinel
	 *                  (4.4.7).
	 *   bricks (1,0,0) and (0,0,1) are ABSENT - section 4.4.5 lets a writer omit
	 *                  any brick that is entirely backgroundValue, so an absent
	 *                  brick is a normal answer and not an error. backgroundValue
	 *                  is (1,2,3), deliberately NOT zero: a zero background is
	 *                  indistinguishable from a zero-filled buffer, so it could
	 *                  not detect a reader that returned zeros instead.
	 */
	const uint8 IntegrityCvfBytes[] = {
		// ---- header, offset 0 -------------------------------------------------
		0x43, 0x46, 0x44, 0x56, 0x4F, 0x4C, 0x31, 0x00, // 0   magic "CFDVOL1\0"
		0x80, 0x00, 0x00, 0x00,                         // 8   headerBytes = 128
		0x01, 0x00,                                     // 12  majorVersion = 1
		0x00, 0x00,                                     // 14  minorVersion = 0
		0x04, 0x03, 0x02, 0x01,                         // 16  endianMarker
		0x01, 0x00, 0x00, 0x00,                         // 20  flags = FlagSparse
		0x07, 0x00, 0x00, 0x00,                         // 24  frameIndex = 7
		0x2A, 0x00, 0x00, 0x00,                         // 28  fieldNumericId = 42
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x3F, // 32  simulationTime = 0.5
		0x03, 0x00, 0x00, 0x00,                         // 40  dimensionX = 3
		0x02, 0x00, 0x00, 0x00,                         // 44  dimensionY = 2
		0x02, 0x00, 0x00, 0x00,                         // 48  dimensionZ = 2
		0x02, 0x00,                                     // 52  brickSizeX = 2
		0x02, 0x00,                                     // 54  brickSizeY = 2
		0x01, 0x00,                                     // 56  brickSizeZ = 1
		0x03,                                           // 58  componentCount = 3
		0x02,                                           // 59  dataType = float32
		0x00,                                           // 60  association = cell
		0x00,                                           // 61  codec = none
		0x00, 0x00,                                     // 62  reserved = 0
		0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 64  brickCount = 2
		0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 72  directoryOffset = 128
		0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 80  payloadOffset = 288
		0x00, 0x00, 0x80, 0x3F,                         // 88  backgroundValue[0] = 1
		0x00, 0x00, 0x00, 0x40,                         // 92  backgroundValue[1] = 2
		0x00, 0x00, 0x40, 0x40,                         // 96  backgroundValue[2] = 3
		0x00, 0x00, 0x00, 0x00,                         // 100 backgroundValue[3] = 0
		0xA0, 0x73, 0x6C, 0x02,                         // 104 headerCrc32c
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 108 reserved[20] = 0
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,

		// ---- directory entry 0, offset 128 -----------------------------------
		0x00, 0x00, 0x00, 0x00,                         // 0   brickIndexX = 0
		0x00, 0x00, 0x00, 0x00,                         // 4   brickIndexY = 0
		0x00, 0x00, 0x00, 0x00,                         // 8   brickIndexZ = 0
		0x02, 0x00,                                     // 12  validSizeX = 2
		0x02, 0x00,                                     // 14  validSizeY = 2
		0x01, 0x00,                                     // 16  validSizeZ = 1
		0x00, 0x00,                                     // 18  flags = 0
		0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 20  payloadOffset = 288
		0x30, 0x00, 0x00, 0x00,                         // 28  compressedBytes = 48
		0x30, 0x00, 0x00, 0x00,                         // 32  uncompressedBytes = 48
		0x00, 0x00, 0x80, 0x3F,                         // 36  componentMin[0] = 1
		0x00, 0x00, 0x00, 0x40,                         // 40  componentMin[1] = 2
		0x00, 0x00, 0x40, 0x40,                         // 44  componentMin[2] = 3
		0x00, 0x00, 0x00, 0x00,                         // 48  componentMin[3] = 0
		0x00, 0x00, 0x20, 0x41,                         // 52  componentMax[0] = 10
		0x00, 0x00, 0x30, 0x41,                         // 56  componentMax[1] = 11
		0x00, 0x00, 0x40, 0x41,                         // 60  componentMax[2] = 12
		0x00, 0x00, 0x00, 0x00,                         // 64  componentMax[3] = 0
		0x6F, 0x84, 0x1C, 0xF3,                         // 68  payloadCrc32c
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 72  reserved[8] = 0

		// ---- directory entry 1, offset 208 - the edge brick ------------------
		0x01, 0x00, 0x00, 0x00,                         // 0   brickIndexX = 1
		0x00, 0x00, 0x00, 0x00,                         // 4   brickIndexY = 0
		0x01, 0x00, 0x00, 0x00,                         // 8   brickIndexZ = 1
		0x01, 0x00,                                     // 12  validSizeX = 1 (not 2)
		0x02, 0x00,                                     // 14  validSizeY = 2
		0x01, 0x00,                                     // 16  validSizeZ = 1
		0x00, 0x00,                                     // 18  flags = 0
		0x50, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 20  payloadOffset = 336
		0x18, 0x00, 0x00, 0x00,                         // 28  compressedBytes = 24
		0x18, 0x00, 0x00, 0x00,                         // 32  uncompressedBytes = 24
		0x00, 0x00, 0x80, 0x7F,                         // 36  componentMin[0] = +inf
		0x00, 0x00, 0x80, 0x7F,                         // 40  componentMin[1] = +inf
		0x00, 0x00, 0x80, 0x7F,                         // 44  componentMin[2] = +inf
		0x00, 0x00, 0x00, 0x00,                         // 48  componentMin[3] = 0
		0x00, 0x00, 0x80, 0xFF,                         // 52  componentMax[0] = -inf
		0x00, 0x00, 0x80, 0xFF,                         // 56  componentMax[1] = -inf
		0x00, 0x00, 0x80, 0xFF,                         // 60  componentMax[2] = -inf
		0x00, 0x00, 0x00, 0x00,                         // 64  componentMax[3] = 0
		0xD0, 0x49, 0xBD, 0xE7,                         // 68  payloadCrc32c
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 72  reserved[8] = 0

		// ---- payload for brick 0, offset 288: 4 voxels x 3 float32 ----------
		// X fastest then Y then Z, components interleaved per voxel (4.4.1-2).
		0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x40, 0x40, // (0,0,0) 1,2,3
		0x00, 0x00, 0x80, 0x40, 0x00, 0x00, 0xA0, 0x40, 0x00, 0x00, 0xC0, 0x40, // (1,0,0) 4,5,6
		0x00, 0x00, 0xE0, 0x40, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0xC0, 0x7F, // (0,1,0) 7,8,NaN
		0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x30, 0x41, 0x00, 0x00, 0x40, 0x41, // (1,1,0) 10,11,12

		// ---- payload for brick 1, offset 336: 2 voxels x 3 float32, all NaN --
		0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0xC0, 0x7F,
		0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0xC0, 0x7F,
	};

	static_assert(sizeof(IntegrityCvfBytes) == 360, "fixture must stay 128 + 160 + 48 + 24 bytes");

	/** Fixture offsets, so no test spells one as a bare number twice. */
	constexpr int32 Entry0 = 128;
	constexpr int32 Payload0 = 288;

	TArray<uint8> MakeCvf()
	{
		return TArray<uint8>(IntegrityCvfBytes, UE_ARRAY_COUNT(IntegrityCvfBytes));
	}

	/**
	 * Recompute headerCrc32c after a test edits a header field.
	 *
	 * Section 4.1: CRC-32C over [0,128) with [104,108) ZEROED - not skipped.
	 * Spelled out here rather than borrowed from the reader, so a test cannot
	 * pass by agreeing with a bug in the reader's own CRC-window arithmetic.
	 */
	void ResealHeaderCrc(TArray<uint8>& Bytes)
	{
		check(Bytes.Num() >= CFDViz::CvfHeaderBytes);
		FMemory::Memzero(Bytes.GetData() + CFDViz::CvfHeaderCrcOffset, CFDViz::HeaderCrcFieldBytes);
		const uint32 Crc = CFDViz::Crc32C::Compute(Bytes.GetData(), CFDViz::CvfHeaderBytes);
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes[static_cast<int32>(CFDViz::CvfHeaderCrcOffset) + Index] =
				static_cast<uint8>((Crc >> (8 * Index)) & 0xFF);
		}
	}

	/**
	 * Recompute a directory entry's payloadCrc32c over a given span, so a
	 * mutation meant to exercise a SIZE rule is not short-circuited by the CRC
	 * check that runs before it. Without this the test would pass for the wrong
	 * reason and the size check would stay unverified.
	 */
	void ResealPayloadCrc(TArray<uint8>& Bytes, int32 EntryOffset, int32 PayloadOffset, int32 PayloadBytes)
	{
		const uint32 Crc = CFDViz::Crc32C::Compute(Bytes.GetData() + PayloadOffset, PayloadBytes);
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes[EntryOffset + 68 + Index] = static_cast<uint8>((Crc >> (8 * Index)) & 0xFF);
		}
	}

	void PokeU16(TArray<uint8>& Bytes, int32 Offset, uint16 Value)
	{
		Bytes[Offset + 0] = static_cast<uint8>(Value & 0xFF);
		Bytes[Offset + 1] = static_cast<uint8>((Value >> 8) & 0xFF);
	}

	void PokeU32(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (8 * Index)) & 0xFF);
		}
	}

	void PokeU64(TArray<uint8>& Bytes, int32 Offset, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (8 * Index)) & 0xFF);
		}
	}

	/**
	 * One float32 out of a decoded buffer, read through the SHARED payload
	 * decoder rather than by casting bytes here - so this asserts what a real
	 * caller sees, not what a second, test-local reading of the layout produces.
	 *
	 * Index is an ELEMENT index: voxel * componentCount + component.
	 */
	double ValueAt(const TArray<uint8>& Buffer, int64 ElementIndex)
	{
		double Value = 0.0;
		const bool bOk = CFDViz::TryReadValueAsDouble(
			TArrayView<const uint8>(Buffer), ElementIndex, ECFDVizDataType::Float32, Value);
		check(bOk);
		return Value;
	}

	uint64 BitsAt(const TArray<uint8>& Buffer, int64 ElementIndex)
	{
		uint64 Bits = 0;
		const bool bOk = CFDViz::TryReadValueBits(
			TArrayView<const uint8>(Buffer), ElementIndex, ECFDVizDataType::Float32, Bits);
		check(bOk);
		return Bits;
	}
}

/* -------------------------------------------------------------------------- */
/* Malformed headers                                                            */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeRejectionTest,
	"FlowViz.CFDViz.VolumeReader.Rejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeRejectionTest::RunTest(const FString& Parameters)
{
	// Each case corrupts exactly ONE thing and asserts the SPECIFIC error. A
	// blanket "returns some failure" assertion would pass even if every malformed
	// file produced the same useless message, which is what section 10 forbids.
	auto ExpectOpenError = [this](const TCHAR* What, TArray<uint8>& Bytes, ECFDVizError Expected)
	{
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("bad.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		if (Result.Error != Expected)
		{
			AddError(FString::Printf(TEXT("%s: expected %s, got %s (%s)"),
				What, CFDVizErrorToString(Expected), CFDVizErrorToString(Result.Error), *Result.ToString()));
			return;
		}
		// The right category with an empty message is still an unusable error.
		TestTrue(FString::Printf(TEXT("%s reports a message"), What), !Result.ToString().IsEmpty());
	};

	// THE CONTROL. Every rejection below is a one-field edit of these bytes; if
	// the unmutated fixture did not open, none of them would mean anything.
	{
		TArray<uint8> Clean = CvfIntegrityTest::MakeCvf();
		const FCFDVizMemoryByteSource Source(Clean, TEXT("clean.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		if (!TestTrue(TEXT("the unmutated fixture opens"), Result.IsOk()))
		{
			AddError(Result.ToString());
			return false;
		}
	}

	// --- magic ---------------------------------------------------------------
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[3] = 'X';
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("wrong magic"), Bytes, ECFDVizError::BadMagic);
	}
	{
		// A CVM file handed to the CVF reader: right family, wrong container. The
		// containers share the version and endian layout, so if the magic did not
		// distinguish them nothing later would - the reader would parse a mesh
		// header as a volume header and produce numbers.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes[Index] = static_cast<uint8>(CFDViz::CvmMagic[Index]);
		}
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("a CVM file is not a CVF"), Bytes, ECFDVizError::BadMagic);
	}

	// --- byte order ----------------------------------------------------------
	{
		// The marker byte-swapped, exactly as a big-endian writer would leave it.
		// Section 4.1 says reject, never swap: a swapped read produces numbers,
		// and wrong numbers are worse than a refusal because nothing downstream
		// can tell they are wrong.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU32(Bytes, 16, 0x04030201u);
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("foreign byte order"), Bytes, ECFDVizError::UnsupportedEndianness);
	}

	// --- version policy (rule 1.4) -------------------------------------------
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU16(Bytes, 12, 2);
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("major version 2"), Bytes, ECFDVizError::UnsupportedVersion);
	}
	{
		// A newer MINOR must be ACCEPTED. This is the direction that is easy to
		// get wrong by treating any version difference as fatal, which would make
		// every 1.x file unreadable the day 1.1 ships.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU16(Bytes, 14, 9);
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("minor.cvf"));
		FCFDVizVolumeReader Reader;
		TestTrue(TEXT("a newer MINOR version is accepted, not rejected"), Reader.Open(Source).IsOk());
	}

	// --- headerBytes and reserved --------------------------------------------
	{
		// headerBytes must be exactly 128: it is what every later offset trusts.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU32(Bytes, 8, 64);
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("headerBytes != 128"), Bytes, ECFDVizError::InvalidHeader);
	}
	{
		// Ignoring a non-zero reserved field is how a future extension gets
		// silently misread as 1.0 data.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU16(Bytes, 62, 1);
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("reserved uint16 @62 is non-zero"), Bytes, ECFDVizError::InvalidHeader);
	}
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[127] = 1; // the last byte of reserved[20] at @108
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("reserved tail @108 is non-zero"), Bytes, ECFDVizError::InvalidHeader);
	}

	// --- enum bytes ----------------------------------------------------------
	{
		// float64 is legal in a CVA and NOT in a CVF (section 3.3). It must be
		// refused here even though the byte is a valid ECFDVizDataType.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[59] = 4;
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("float64 is not a CVF data type"), Bytes, ECFDVizError::UnsupportedDataType);
	}
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[59] = 99; // outside the enum entirely
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("unknown data type"), Bytes, ECFDVizError::UnsupportedDataType);
	}
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[60] = 7;
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("unknown association"), Bytes, ECFDVizError::UnsupportedAssociation);
	}
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[61] = 42;
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("unknown codec"), Bytes, ECFDVizError::UnsupportedCodec);
	}
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[58] = 0; // componentCount
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("componentCount 0"), Bytes, ECFDVizError::InvalidHeader);
	}
	{
		// The header carries only four componentMin/Max slots, so a fifth
		// component would have nowhere to record its range.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[58] = 5;
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("componentCount 5"), Bytes, ECFDVizError::InvalidHeader);
	}

	// --- the reserved zstd codec (section 7) ---------------------------------
	{
		// Refused with the EXACT wording of the spec - not a paraphrase, and never
		// a silent fallback to another codec. The message is asserted verbatim
		// because the Python reference emits the same string, and section 9's
		// cross-language bridge compares them.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[61] = 1; // Zstd
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("zstd.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestTrue(TEXT("zstd is rejected as an unsupported codec"),
			Result.Error == ECFDVizError::UnsupportedCodec);
		TestEqual(TEXT("and the rejection uses the exact section 7 message"),
			Result.Message, FString(CFDViz::ZstdRejectionMessage));
	}

	// --- degenerate geometry --------------------------------------------------
	{
		// A zero dimension makes the brick-grid arithmetic produce an empty volume
		// that otherwise reads as perfectly valid.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU32(Bytes, 40, 0);
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("dimensionX 0"), Bytes, ECFDVizError::InvalidHeader);
	}
	{
		// A zero brick edge is worse: it is a division by zero in the brick-count
		// computation.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU16(Bytes, 52, 0);
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("brickSizeX 0"), Bytes, ECFDVizError::InvalidHeader);
	}

	// --- directory bounds -----------------------------------------------------
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU64(Bytes, 72, 64); // inside the header
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("directory overlaps the header"), Bytes, ECFDVizError::DirectoryOutOfBounds);
	}
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU64(Bytes, 72, 1ull << 40); // far past a 360-byte file
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		ExpectOpenError(TEXT("directory offset past the file"), Bytes, ECFDVizError::DirectoryOutOfBounds);
	}
	{
		// brickCount * 80 must not be allowed to WRAP into a small positive
		// number. A reader that computed `offset + count*80` in unchecked 64-bit
		// arithmetic would see a bogus pass here and then index an
		// attacker-chosen distance into memory.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU64(Bytes, 64, 0xFFFFFFFFFFFFFFFFull / 80 + 2);
		CvfIntegrityTest::ResealHeaderCrc(Bytes);
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("overflow.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("a brickCount whose directory size overflows is rejected"), Result.IsOk());
		TestTrue(TEXT("and it is a bounds/allocation refusal, not a crash"),
			Result.Error == ECFDVizError::DirectoryOutOfBounds
				|| Result.Error == ECFDVizError::AllocationTooLarge
				|| Result.Error == ECFDVizError::InvalidHeader);
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Payload integrity                                                            */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeIntegrityTest,
	"FlowViz.CFDViz.VolumeReader.Integrity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeIntegrityTest::RunTest(const FString& Parameters)
{
	// A corrupted payload MUST be rejected. This is the test that gives the CRC
	// field its meaning: without it, "the format stores a payload CRC" is an
	// unfalsifiable claim about the file rather than a property of the reader.
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[CvfIntegrityTest::Payload0] ^= 0x01; // one bit of the first float of brick 0
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("corrupt.cvf"));
		FCFDVizVolumeReader Reader;
		if (TestTrue(TEXT("a payload-corrupted file still opens"), Reader.Open(Source).IsOk()))
		{
			TArray<uint8> Brick;
			const FCFDVizResult Result = Reader.ReadBrick(0, Brick);
			TestTrue(TEXT("single-bit payload corruption is rejected"),
				Result.Error == ECFDVizError::PayloadCrcMismatch);
			// Section 4.4.8: a reader MUST NOT silently substitute zeros. Handing
			// back a zero-filled buffer would let a caller render corrupt data as
			// a perfectly plausible uniform field.
			TestEqual(TEXT("and no data is handed back for a bad brick"), Brick.Num(), 0);

			// The sweep must NAME the offending brick, so a section 10 validation
			// report can say which one rather than "the file is bad".
			TArray<int32> FailedBricks;
			TestFalse(TEXT("the sweep also reports failure through IsOk"),
				Reader.VerifyAllPayloadCrcs(FailedBricks).IsOk());
			if (TestEqual(TEXT("the sweep finds exactly one bad brick"), FailedBricks.Num(), 1))
			{
				TestEqual(TEXT("and names brick 0"), FailedBricks[0], 0);
			}
			TestFalse(TEXT("and the whole-file sweep fails too"), Reader.VerifyAllCrcs().IsOk());

			// The UNDAMAGED brick still reads. That is the entire point of
			// per-brick CRCs - one bad brick must not cost the whole file.
			TArray<uint8> Good;
			TestTrue(TEXT("the undamaged brick still decodes"), Reader.ReadBrick(1, Good).IsOk());
			TestEqual(TEXT("and yields its full payload"), Good.Num(), 24);

			// With verification OFF - a validator's mode - the bytes come back so
			// the corruption can be reported, not silently repaired.
			TArray<uint8> Unverified;
			TestTrue(TEXT("a validator can opt out of verification"),
				Reader.ReadBrick(0, Unverified, /*bVerifyCrc=*/false).IsOk());
			TestEqual(TEXT("and gets the real bytes back"), Unverified.Num(), 48);
			// And they really are the CORRUPT bytes: 1.0f with its low bit
			// flipped. Asserting only the length would pass against a reader that
			// returned zeros, which is the behaviour 4.4.8 forbids.
			TestEqual(TEXT("opting out means seeing the damage, not repaired data"),
				CvfIntegrityTest::BitsAt(Unverified, 0), static_cast<uint64>(0x3F800001ull));
		}
	}

	// THE SIZE AND BOUNDS RULES ARE ENFORCED EAGERLY, AT Open().
	//
	// I first wrote these four cases expecting Open() to succeed and the
	// individual ReadBrick to fail, i.e. validation deferred to the point of use.
	// It is not: the reader walks the whole directory during Open and refuses the
	// FILE if any entry is inconsistent. That is the stronger contract, and the
	// right one - a caller that opens a file successfully and then iterates
	// bricks would otherwise discover corruption halfway through a frame, with
	// half a volume already uploaded. Asserted here in the form the reader
	// actually guarantees, so these tests pin that behaviour rather than a weaker
	// one that would still pass if validation regressed to lazy.
	auto ExpectOpenRejects = [this](const TCHAR* What, TArray<uint8>& Bytes, ECFDVizError Expected)
	{
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("bad.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		if (Result.Error != Expected)
		{
			AddError(FString::Printf(TEXT("%s: expected %s, got %s (%s)"),
				What, CFDVizErrorToString(Expected), CFDVizErrorToString(Result.Error), *Result.ToString()));
			return;
		}
		// The offending brick must be identifiable from the message, or a
		// validation report can only say "somewhere in this file".
		TestTrue(FString::Printf(TEXT("%s reports a message"), What), !Result.ToString().IsEmpty());
	};

	// uncompressedBytes must equal validX*validY*validZ*components*sizeof(type),
	// checked BEFORE the allocation it would otherwise size (4.4.4). The payload
	// CRC is left intact so the failure is attributable to the size rule alone.
	{
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU32(Bytes, CvfIntegrityTest::Entry0 + 32, 47); // one byte short of 48
		ExpectOpenRejects(TEXT("uncompressedBytes inconsistent with the brick geometry"),
			Bytes, ECFDVizError::SizeMismatch);
	}
	{
		// A hostile size field: 4 GB claimed for a 4-voxel brick. It must be
		// caught by the derived-size equality, not by an allocation failure - a
		// reader that allocated first and failed second would be a trivial denial
		// of service on a malformed file. Rejecting at Open means the allocation
		// is never even reached.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU32(Bytes, CvfIntegrityTest::Entry0 + 32, 0xFFFFFFFFu);
		ExpectOpenRejects(TEXT("a 4 GB claim for a 4-voxel brick"),
			Bytes, ECFDVizError::SizeMismatch);
	}
	{
		// With codec none, compressedBytes must equal uncompressedBytes: anything
		// else means the file describes a transformation it did not apply. The
		// payload CRC is RESEALED over the 40 bytes the entry now claims, so a CRC
		// failure cannot mask the size check and let this pass for the wrong
		// reason.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU32(Bytes, CvfIntegrityTest::Entry0 + 28, 40);
		CvfIntegrityTest::ResealPayloadCrc(Bytes, CvfIntegrityTest::Entry0, CvfIntegrityTest::Payload0, 40);
		ExpectOpenRejects(TEXT("codec none with compressedBytes != uncompressedBytes"),
			Bytes, ECFDVizError::SizeMismatch);
	}
	{
		// A payload offset that points outside the file (rule 1.5).
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU64(Bytes, CvfIntegrityTest::Entry0 + 20, 100000);
		ExpectOpenRejects(TEXT("a payload offset past the end of the file"),
			Bytes, ECFDVizError::PayloadOutOfBounds);
	}
	{
		// No per-brick flag bits exist in 1.0, so an unknown one may change how
		// the payload decodes and must not be ignored.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU16(Bytes, CvfIntegrityTest::Entry0 + 18, 1);
		ExpectOpenRejects(TEXT("an undefined per-brick flag bit"),
			Bytes, ECFDVizError::InvalidHeader);
	}
	{
		// A directory entry's reserved[8] must be zero, same reasoning as the
		// header's: a non-zero reserved field means a future writer put something
		// there that this reader would otherwise silently ignore.
		TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
		Bytes[CvfIntegrityTest::Entry0 + 72] = 1;
		const FCFDVizMemoryByteSource Source(Bytes, TEXT("res.cvf"));
		FCFDVizVolumeReader Reader;
		TestTrue(TEXT("non-zero reserved bytes in a directory entry are rejected"),
			Reader.Open(Source).Error == ECFDVizError::InvalidHeader);
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Sparse storage: absent bricks evaluate to backgroundValue (4.4.5)            */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeBackgroundTest,
	"FlowViz.CFDViz.VolumeReader.Background",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeBackgroundTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Bytes = CvfIntegrityTest::MakeCvf();
	const FCFDVizMemoryByteSource Source(Bytes, TEXT("sparse.cvf"));

	FCFDVizVolumeReader Reader;
	if (!TestTrue(TEXT("the sparse fixture opens"), Reader.Open(Source).IsOk()))
	{
		return false;
	}

	// Two of the four brick slots are stored, so the absent-brick path below is
	// genuinely exercised rather than silently skipped. Without this the whole
	// test could pass against a dense file and prove nothing about 4.4.5.
	const FCFDVizVolumeHeader& Header = Reader.GetHeader();
	TestEqual(TEXT("four brick slots in the grid"), Header.GetTotalBrickCount(), static_cast<int64>(4));
	TestEqual(TEXT("but only two are stored"), Reader.GetBrickCount(), 2);

	// IsSparse() reports the header FLAG BIT, which is a writer's advertisement,
	// NOT a fact derived from the directory. Worth pinning explicitly because the
	// natural misreading - "sparse means some bricks are missing" - would make
	// this a tautology of the two lines above. Section 4.4.5 is deliberate that
	// absence means backgroundValue whether or not the flag is set, so a reader
	// must never gate the background path on this bit. The fixture sets the flag
	// AND omits bricks; the assertions below rely only on the omission.
	TestTrue(TEXT("the writer advertised sparse storage via the header flag"), Header.IsSparse());

	// backgroundValue materialised into storage bytes is what an absent brick
	// must produce. Asserted against the header field first, so the expectations
	// below are anchored to the file rather than to a number repeated by hand.
	TArray<uint8> Background;
	if (!TestTrue(TEXT("the background voxel materialises"),
		Reader.GetBackgroundVoxelBytes(Background).IsOk()))
	{
		return false;
	}
	TestEqual(TEXT("one voxel is 3 float32s"), Background.Num(), 12);
	TestEqual(TEXT("background.x is the header's 1"), CvfIntegrityTest::ValueAt(Background, 0), 1.0);
	TestEqual(TEXT("background.y is the header's 2"), CvfIntegrityTest::ValueAt(Background, 1), 2.0);
	TestEqual(TEXT("background.z is the header's 3"), CvfIntegrityTest::ValueAt(Background, 2), 3.0);

	// A voxel inside a STORED brick returns the stored value, not the background.
	TArray<uint8> Voxel;
	if (TestTrue(TEXT("a stored voxel reads"), Reader.ReadVoxel(1, 1, 0, Voxel).IsOk()))
	{
		TestEqual(TEXT("value (1,1,0).x is the stored 10"), CvfIntegrityTest::ValueAt(Voxel, 0), 10.0);
		TestEqual(TEXT("value (1,1,0).z is the stored 12"), CvfIntegrityTest::ValueAt(Voxel, 2), 12.0);
	}

	// A voxel inside an ABSENT brick returns backgroundValue - NOT an error, and
	// NOT zeros. Voxel (2,0,0) lies in brick (1,0,0), which the fixture omits.
	// The background is (1,2,3) precisely so this cannot be satisfied by a
	// zero-filled buffer.
	if (TestTrue(TEXT("a voxel in an absent brick reads without error"),
		Reader.ReadVoxel(2, 0, 0, Voxel).IsOk()))
	{
		TestEqual(TEXT("and yields backgroundValue, not zero"), Voxel, Background);
	}
	if (TestTrue(TEXT("a voxel in the other absent brick reads"),
		Reader.ReadVoxel(0, 0, 1, Voxel).IsOk()))
	{
		TestEqual(TEXT("and also yields backgroundValue"), Voxel, Background);
	}

	// ReadDense must agree with ReadVoxel at EVERY voxel. The two take different
	// paths - one reconstructs the whole grid, the other decodes a single brick -
	// so a disagreement means one of them places bricks wrongly. Spot-checking a
	// couple of voxels would miss exactly the off-by-one-brick error this catches,
	// because a shifted brick still produces plausible numbers everywhere.
	TArray<uint8> Dense;
	if (!TestTrue(TEXT("the dense volume reconstructs"), Reader.ReadDense(Dense).IsOk()))
	{
		return false;
	}
	TestEqual(TEXT("dense volume is 3*2*2 voxels of 12 bytes"), Dense.Num(), 144);

	const FIntVector Extent = Header.GetValueCounts();
	int32 Disagreements = 0;
	for (int32 K = 0; K < Extent.Z; ++K)
	{
		for (int32 J = 0; J < Extent.Y; ++J)
		{
			for (int32 I = 0; I < Extent.X; ++I)
			{
				TArray<uint8> Single;
				if (!Reader.ReadVoxel(I, J, K, Single).IsOk() || Single.Num() != 12)
				{
					++Disagreements;
					continue;
				}
				const int64 Base = (static_cast<int64>(K) * Extent.Y * Extent.X
					+ static_cast<int64>(J) * Extent.X + I) * 12;
				// Compared as BYTES, so the all-NaN voxels take part: a float
				// comparison would silently skip them (NaN != NaN), and NaN
				// placement is exactly what a brick-offset bug would move.
				if (FMemory::Memcmp(Dense.GetData() + Base, Single.GetData(), 12) != 0)
				{
					++Disagreements;
				}
			}
		}
	}
	TestEqual(TEXT("ReadDense and ReadVoxel agree at every one of the 12 voxels"), Disagreements, 0);

	// The dense volume really does carry the absent brick's background at
	// (2,0,0), and real data at (0,0,0) - so the agreement above is not two
	// implementations of "return background everywhere".
	TestEqual(TEXT("dense voxel (2,0,0).x is backgroundValue"), CvfIntegrityTest::ValueAt(Dense, 2 * 3), 1.0);
	TestEqual(TEXT("dense voxel (0,0,0).y is the stored 2"), CvfIntegrityTest::ValueAt(Dense, 1), 2.0);
	TestEqual(TEXT("dense voxel (1,0,0).y is the stored 5"), CvfIntegrityTest::ValueAt(Dense, 1 * 3 + 1), 5.0);
	// NaN survives the dense reconstruction bit-exactly (rule 1.7). Compared as
	// BITS: a float comparison would pass against any NaN, and also against a
	// reader that substituted a different payload entirely.
	TestEqual(TEXT("the stored NaN survives ReadDense bit-exactly"),
		CvfIntegrityTest::BitsAt(Dense, (1 * 3 + 0) * 3 + 2), static_cast<uint64>(0x7FC00000ull));

	// A budget below the volume size must be REFUSED rather than truncated. The
	// cap is the only defence against a self-consistent header that declares a
	// terabyte of empty space: absent bricks cost no file bytes, so there is no
	// file-size correlate to catch it.
	TArray<uint8> Capped;
	const FCFDVizResult CapResult = Reader.ReadDense(Capped, /*MaxBytes=*/100);
	TestTrue(TEXT("a dense read past its budget is refused"),
		CapResult.Error == ECFDVizError::AllocationTooLarge);
	TestEqual(TEXT("and nothing is handed back"), Capped.Num(), 0);

	// AND THE FLAG IS NOT WHAT MAKES IT WORK. Same file with FlagSparse cleared
	// and the header re-sealed: the bricks are still absent, so section 4.4.5
	// still says backgroundValue. This is the differential that stops the
	// assertions above from passing against a reader that gates its background
	// path on the advertisement bit - such a reader would return zeros, or fail,
	// for exactly the files a conservative writer produces without the flag.
	{
		TArray<uint8> Unflagged = CvfIntegrityTest::MakeCvf();
		CvfIntegrityTest::PokeU32(Unflagged, 20, 0);
		CvfIntegrityTest::ResealHeaderCrc(Unflagged);
		const FCFDVizMemoryByteSource UnflaggedSource(Unflagged, TEXT("unflagged.cvf"));
		FCFDVizVolumeReader UnflaggedReader;
		if (TestTrue(TEXT("the same file without the sparse flag still opens"),
			UnflaggedReader.Open(UnflaggedSource).IsOk()))
		{
			TestFalse(TEXT("and does not advertise sparse storage"),
				UnflaggedReader.GetHeader().IsSparse());
			TArray<uint8> UnflaggedVoxel;
			if (TestTrue(TEXT("a voxel in an absent brick still reads"),
				UnflaggedReader.ReadVoxel(2, 0, 0, UnflaggedVoxel).IsOk()))
			{
				TestEqual(TEXT("and absence still means backgroundValue, flag or no flag"),
					UnflaggedVoxel, Background);
			}
		}
	}

	// The edge brick's "no valid data" sentinel must not be reported as a usable
	// range. An inverted range (+inf, -inf) silently normalises a colour map to
	// nothing, which looks like a blank field rather than an error (4.4.7).
	const FCFDVizBrickEntry& Edge = Reader.GetBrickEntries()[1];
	float RangeMin = 0.0f;
	float RangeMax = 0.0f;
	TestFalse(TEXT("the all-NaN brick's sentinel is NOT reported as a range"),
		Edge.TryGetComponentRange(0, RangeMin, RangeMax));
	// While a real brick's range is reported, so the refusal above is specific to
	// the sentinel and not a function that always returns false.
	const FCFDVizBrickEntry& Full = Reader.GetBrickEntries()[0];
	TestTrue(TEXT("a real brick does report its range"),
		Full.TryGetComponentRange(0, RangeMin, RangeMax));
	TestEqual(TEXT("and it is the stored min"), RangeMin, 1.0f);
	TestEqual(TEXT("and the stored max"), RangeMax, 10.0f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
