// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizCrc32C.h"
#include "CFDViz/CFDVizPayload.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The shared payload decoder (format sections 4.4 and 6.3).
 *
 * Every compressed byte array below was produced by Python's `zlib.compress(data, 6)`
 * and `lz4.block.compress(data, store_size=False)` - the exact calls the reference
 * writer in Tools/cfdviz makes. They are committed as literals rather than
 * generated here, because a fixture the test compresses itself proves only that
 * the engine can read its own output. These prove the engine reads what the
 * OTHER implementation writes, which is the entire point of having two.
 *
 * The bit patterns are asserted as integers, never as floats. A float
 * comparison cannot distinguish two different NaNs, and cannot detect a reader
 * that silently normalised a payload - so on this format, comparing `value`
 * where `bits` is available is not a check at all.
 */

namespace
{
	/** float32 [1, 2, 3, NaN] - uncompressed. */
	const uint8 F32Raw[] = {
		0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40,
		0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0xC0, 0x7F,
	};
	/** The same values through zlib level 6, RFC 1950 container (section 7). */
	const uint8 F32Zlib[] = {
		0x78, 0x9C, 0x63, 0x60, 0x68, 0xB0, 0x67, 0x60, 0x60, 0x70, 0x00,
		0x22, 0x20, 0x3E, 0x50, 0x0F, 0x00, 0x11, 0x42, 0x02, 0xBF,
	};
	/** The same values through LZ4 block, no size prefix - what NAME_LZ4 consumes. */
	const uint8 F32Lz4[] = {
		0xF0, 0x01, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40,
		0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0xC0, 0x7F,
	};

	/** float16 [1, NaN, -2, 65504] - 65504 is the largest finite half. */
	const uint8 F16Raw[] = { 0x00, 0x3C, 0x00, 0x7E, 0x00, 0xC0, 0xFF, 0x7B };
	const uint8 F16Zlib[] = {
		0x78, 0x9C, 0x63, 0xB0, 0x61, 0xA8, 0x63, 0x38,
		0xF0, 0xBF, 0x1A, 0x00, 0x08, 0xDB, 0x02, 0xF5,
	};

	/** uint8 [0, 1, 127, 255] - CVF's mask/palette type. */
	const uint8 U8Raw[] = { 0x00, 0x01, 0x7F, 0xFF };
	const uint8 U8Zlib[] = { 0x78, 0x9C, 0x63, 0x60, 0xAC, 0xFF, 0x0F, 0x00, 0x02, 0x04, 0x01, 0x80 };

	/** float64 [1, NaN, -2.5, 1e308] - CVA only; 1e308 would overflow a float32. */
	const uint8 F64Raw[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x7F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xC0,
		0xA0, 0xC8, 0xEB, 0x85, 0xF3, 0xCC, 0xE1, 0x7F,
	};
	const uint8 F64Zlib[] = {
		0x78, 0x9C, 0x63, 0x60, 0x00, 0x81, 0x0F, 0xF6, 0x60, 0x8A, 0xE1,
		0x47, 0x3D, 0x84, 0x66, 0x39, 0xB0, 0xE0, 0xC4, 0xEB, 0xD6, 0xCF,
		0x67, 0x1E, 0xD6, 0x03, 0x00, 0x5A, 0x72, 0x09, 0x62,
	};

	/**
	 * Decode a standalone payload placed at offset 0 of its own byte source.
	 *
	 * The CRC is computed here over the stored bytes rather than hard-coded, but
	 * the decoded RESULT is asserted against literals - so a wrong CRC still
	 * fails, via the corruption cases below, while these cases stay readable.
	 */
	FCFDVizResult DecodeStandalone(
		TArrayView<const uint8> Stored,
		int64 UncompressedBytes,
		int64 ValueCount,
		int32 ComponentCount,
		ECFDVizDataType DataType,
		ECFDVizCodec Codec,
		TArray<uint8>& OutBytes,
		bool bVerifyCrc = true)
	{
		const FCFDVizMemoryByteSource Source(Stored, TEXT("payload"));
		CFDViz::FPayloadSpec Spec;
		Spec.Offset = 0;
		Spec.CompressedBytes = Stored.Num();
		Spec.UncompressedBytes = UncompressedBytes;
		Spec.PayloadCrc32C = CFDViz::Crc32C::Compute(Stored);
		Spec.ValueCount = ValueCount;
		Spec.ComponentCount = ComponentCount;
		Spec.DataType = DataType;
		Spec.Codec = Codec;
		Spec.Context = TEXT("test payload");
		return CFDViz::DecodePayload(Source, Spec, OutBytes, bVerifyCrc);
	}
}

/* -------------------------------------------------------------------------- */
/* Every data type, every codec                                                 */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizPayloadCodecMatrixTest,
	"FlowViz.CFDViz.Payload.CodecMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizPayloadCodecMatrixTest::RunTest(const FString& Parameters)
{
	using namespace CFDViz;

	// --- float32, all three codecs. Every codec must produce byte-identical
	// output; a codec that decoded to *nearly* the same bytes would still be a
	// data-corrupting bug.
	{
		const TArray<uint8> Expected(F32Raw, UE_ARRAY_COUNT(F32Raw));

		struct FCase { const TCHAR* Name; TArrayView<const uint8> Stored; ECFDVizCodec Codec; };
		const FCase Cases[] = {
			{ TEXT("none"), TArrayView<const uint8>(F32Raw, UE_ARRAY_COUNT(F32Raw)), ECFDVizCodec::None },
			{ TEXT("zlib"), TArrayView<const uint8>(F32Zlib, UE_ARRAY_COUNT(F32Zlib)), ECFDVizCodec::Zlib },
			{ TEXT("lz4"),  TArrayView<const uint8>(F32Lz4, UE_ARRAY_COUNT(F32Lz4)),  ECFDVizCodec::LZ4 },
		};

		for (const FCase& Case : Cases)
		{
			TArray<uint8> Decoded;
			const FCFDVizResult Result = DecodeStandalone(
				Case.Stored, 16, 4, 1, ECFDVizDataType::Float32, Case.Codec, Decoded);
			if (!TestTrue(FString::Printf(TEXT("codec %s decodes"), Case.Name), Result.IsOk()))
			{
				AddError(Result.ToString());
				continue;
			}
			TestEqual(FString::Printf(TEXT("codec %s yields 16 bytes"), Case.Name), Decoded.Num(), 16);
			TestTrue(FString::Printf(TEXT("codec %s reproduces the payload byte for byte"), Case.Name),
				Decoded == Expected);

			// Values, including the NaN, compared as bit patterns.
			uint64 Bits = 0;
			TestTrue(TEXT("value 0 readable"), TryReadValueBits(Decoded, 0, ECFDVizDataType::Float32, Bits));
			TestEqual(TEXT("1.0f bits"), Bits, static_cast<uint64>(0x3F800000u));
			TestTrue(TEXT("value 3 readable"), TryReadValueBits(Decoded, 3, ECFDVizDataType::Float32, Bits));
			TestEqual(FString::Printf(TEXT("codec %s preserves the NaN bit pattern"), Case.Name),
				Bits, static_cast<uint64>(0x7FC00000u));
		}
	}

	// --- float16. Half precision is where a reader is most tempted to widen
	// early and lose the exact stored bits.
	{
		TArray<uint8> Decoded;
		if (TestTrue(TEXT("float16 zlib decodes"),
			DecodeStandalone(TArrayView<const uint8>(F16Zlib, UE_ARRAY_COUNT(F16Zlib)),
				8, 4, 1, ECFDVizDataType::Float16, ECFDVizCodec::Zlib, Decoded).IsOk()))
		{
			TestTrue(TEXT("float16 zlib matches the raw payload"),
				Decoded == TArray<uint8>(F16Raw, UE_ARRAY_COUNT(F16Raw)));

			uint64 Bits = 0;
			TestTrue(TEXT("half 0 readable"), TryReadValueBits(Decoded, 0, ECFDVizDataType::Float16, Bits));
			TestEqual(TEXT("half 1.0 bits"), Bits, static_cast<uint64>(0x3C00u));
			TestTrue(TEXT("half 1 readable"), TryReadValueBits(Decoded, 1, ECFDVizDataType::Float16, Bits));
			TestEqual(TEXT("half NaN bits survive widening-free storage"), Bits, static_cast<uint64>(0x7E00u));
			TestTrue(TEXT("half 3 readable"), TryReadValueBits(Decoded, 3, ECFDVizDataType::Float16, Bits));
			TestEqual(TEXT("largest finite half bits"), Bits, static_cast<uint64>(0x7BFFu));

			// Widening to double must be exact for every half, and must still
			// report NaN as NaN rather than as some finite substitute.
			double Value = 0.0;
			TestTrue(TEXT("half 0 widens"), TryReadValueAsDouble(Decoded, 0, ECFDVizDataType::Float16, Value));
			TestEqual(TEXT("half 1.0 widens exactly"), Value, 1.0);
			TestTrue(TEXT("half 2 widens"), TryReadValueAsDouble(Decoded, 2, ECFDVizDataType::Float16, Value));
			TestEqual(TEXT("half -2.0 widens exactly"), Value, -2.0);
			TestTrue(TEXT("half 3 widens"), TryReadValueAsDouble(Decoded, 3, ECFDVizDataType::Float16, Value));
			TestEqual(TEXT("65504 widens exactly"), Value, 65504.0);
			TestTrue(TEXT("half 1 widens"), TryReadValueAsDouble(Decoded, 1, ECFDVizDataType::Float16, Value));
			TestTrue(TEXT("half NaN widens to a NaN"), FMath::IsNaN(Value));
		}
	}

	// --- uint8. Must widen unsigned: reading 255 as -1 is the classic char bug,
	// and on a mask field it inverts the meaning of every masked cell.
	{
		TArray<uint8> Decoded;
		if (TestTrue(TEXT("uint8 zlib decodes"),
			DecodeStandalone(TArrayView<const uint8>(U8Zlib, UE_ARRAY_COUNT(U8Zlib)),
				4, 4, 1, ECFDVizDataType::UInt8, ECFDVizCodec::Zlib, Decoded).IsOk()))
		{
			TestTrue(TEXT("uint8 zlib matches the raw payload"),
				Decoded == TArray<uint8>(U8Raw, UE_ARRAY_COUNT(U8Raw)));
			double Value = 0.0;
			TestTrue(TEXT("u8 3 readable"), TryReadValueAsDouble(Decoded, 3, ECFDVizDataType::UInt8, Value));
			TestEqual(TEXT("255 widens to 255, not -1"), Value, 255.0);
			TestTrue(TEXT("u8 2 readable"), TryReadValueAsDouble(Decoded, 2, ECFDVizDataType::UInt8, Value));
			TestEqual(TEXT("127 widens to 127"), Value, 127.0);
		}
	}

	// --- float64, the CVA-only type.
	{
		TArray<uint8> Decoded;
		if (TestTrue(TEXT("float64 zlib decodes"),
			DecodeStandalone(TArrayView<const uint8>(F64Zlib, UE_ARRAY_COUNT(F64Zlib)),
				32, 4, 1, ECFDVizDataType::Float64, ECFDVizCodec::Zlib, Decoded).IsOk()))
		{
			TestTrue(TEXT("float64 zlib matches the raw payload"),
				Decoded == TArray<uint8>(F64Raw, UE_ARRAY_COUNT(F64Raw)));
			uint64 Bits = 0;
			TestTrue(TEXT("double 1 readable"), TryReadValueBits(Decoded, 1, ECFDVizDataType::Float64, Bits));
			TestEqual(TEXT("double NaN bits"), Bits, static_cast<uint64>(0x7FF8000000000000ull));
			TestTrue(TEXT("double 3 readable"), TryReadValueBits(Decoded, 3, ECFDVizDataType::Float64, Bits));
			TestEqual(TEXT("1e308 bits"), Bits, static_cast<uint64>(0x7FE1CCF385EBC8A0ull));
			double Value = 0.0;
			TestTrue(TEXT("double 2 readable"), TryReadValueAsDouble(Decoded, 2, ECFDVizDataType::Float64, Value));
			TestEqual(TEXT("-2.5 round-trips"), Value, -2.5);
		}
	}

	// Interleaving: 2 entities x 3 components over the float32 fixture-shaped
	// data. The index arithmetic is entity*components + component, so a reader
	// that stored planar (component-major) data would return the wrong value
	// with no error at all.
	{
		const uint8 Interleaved[] = {
			0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x40, 0x40, // 1,2,3
			0x00, 0x00, 0x80, 0x40, 0x00, 0x00, 0xA0, 0x40, 0x00, 0x00, 0xC0, 0x40, // 4,5,6
		};
		TArray<uint8> Decoded;
		if (TestTrue(TEXT("interleaved payload decodes"),
			DecodeStandalone(TArrayView<const uint8>(Interleaved, UE_ARRAY_COUNT(Interleaved)),
				24, 2, 3, ECFDVizDataType::Float32, ECFDVizCodec::None, Decoded).IsOk()))
		{
			double Value = 0.0;
			TestTrue(TEXT("element 3 readable"), TryReadValueAsDouble(Decoded, 3, ECFDVizDataType::Float32, Value));
			TestEqual(TEXT("entity 1 component 0 follows entity 0 component 2"), Value, 4.0);
		}
	}

	// Reading past the end returns false rather than reading adjacent memory.
	{
		const TArray<uint8> Decoded(F32Raw, UE_ARRAY_COUNT(F32Raw));
		double Value = 0.0;
		uint64 Bits = 0;
		TestFalse(TEXT("index past the end is refused"), TryReadValueAsDouble(Decoded, 4, ECFDVizDataType::Float32, Value));
		TestFalse(TEXT("negative index is refused"), TryReadValueAsDouble(Decoded, -1, ECFDVizDataType::Float32, Value));
		TestFalse(TEXT("bit read past the end is refused"), TryReadValueBits(Decoded, 4, ECFDVizDataType::Float32, Bits));
		// A float64 read of a 16-byte buffer: index 1 needs bytes [8,16), fine;
		// index 2 needs [16,24), which is out. The check must use the type's
		// size, not a fixed stride.
		TestTrue(TEXT("float64 index 1 fits in 16 bytes"), TryReadValueBits(Decoded, 1, ECFDVizDataType::Float64, Bits));
		TestFalse(TEXT("float64 index 2 does not"), TryReadValueBits(Decoded, 2, ECFDVizDataType::Float64, Bits));
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Rejection and integrity                                                      */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizPayloadRejectionTest,
	"FlowViz.CFDViz.Payload.Rejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizPayloadRejectionTest::RunTest(const FString& Parameters)
{
	using namespace CFDViz;

	// zstd: reserved, and the rejection message is normative to the character
	// (section 7). Asserting the exact string is deliberate - a paraphrase would
	// send a user looking for a re-export flag that does not match the docs.
	{
		TArray<uint8> Decoded;
		const FCFDVizResult Result = DecodeStandalone(
			TArrayView<const uint8>(F32Raw, UE_ARRAY_COUNT(F32Raw)),
			16, 4, 1, ECFDVizDataType::Float32, ECFDVizCodec::Zstd, Decoded);
		TestTrue(TEXT("zstd is rejected"), Result.Error == ECFDVizError::UnsupportedCodec);
		TestEqual(TEXT("with exactly the message the spec mandates"),
			Result.Message, FString(CFDViz::ZstdRejectionMessage));
		// "MUST NOT silently fall back to another codec" - so nothing comes back.
		TestEqual(TEXT("and nothing is decoded"), Decoded.Num(), 0);
	}

	// The declared size must equal values * components * sizeof(type), checked
	// BEFORE the allocation it would otherwise size (4.4.4 / 6.3.2).
	{
		TArray<uint8> Decoded;
		const FCFDVizResult Result = DecodeStandalone(
			TArrayView<const uint8>(F32Raw, UE_ARRAY_COUNT(F32Raw)),
			/*UncompressedBytes=*/20, 4, 1, ECFDVizDataType::Float32, ECFDVizCodec::None, Decoded);
		TestTrue(TEXT("uncompressedBytes inconsistent with the value count is rejected"),
			Result.Error == ECFDVizError::SizeMismatch);
		TestEqual(TEXT("nothing is allocated"), Decoded.Num(), 0);
	}
	{
		// The hostile case the check exists for: a self-consistent-looking but
		// enormous size. It must fail on the derived-size equality, before any
		// allocation - not by trying and running out of memory.
		TArray<uint8> Decoded;
		const FCFDVizResult Result = DecodeStandalone(
			TArrayView<const uint8>(F32Raw, UE_ARRAY_COUNT(F32Raw)),
			/*UncompressedBytes=*/1ll << 40, 4, 1, ECFDVizDataType::Float32, ECFDVizCodec::Zlib, Decoded);
		TestTrue(TEXT("a 1 TB size claim is rejected"), !Result.IsOk());
		TestEqual(TEXT("and allocates nothing"), Decoded.Num(), 0);
	}
	{
		// A payload that is self-consistent but larger than the allocation cap.
		// This is a different lie from the one above: values * components *
		// sizeof(type) genuinely equals uncompressedBytes here, so only the cap
		// catches it. Two checks, two distinct failure modes.
		const FCFDVizMemoryByteSource Source(
			TArrayView<const uint8>(F32Raw, UE_ARRAY_COUNT(F32Raw)), TEXT("cap"));
		FPayloadSpec Spec;
		Spec.Offset = 0;
		Spec.CompressedBytes = 16;
		Spec.UncompressedBytes = 4096;
		Spec.PayloadCrc32C = Crc32C::Compute(F32Raw, 16);
		Spec.ValueCount = 1024;
		Spec.ComponentCount = 1;
		Spec.DataType = ECFDVizDataType::Float32;
		Spec.Codec = ECFDVizCodec::Zlib;
		TArray<uint8> Decoded;
		const FCFDVizResult Result = DecodePayload(Source, Spec, Decoded, true, /*MaxAllocationBytes=*/1024);
		TestTrue(TEXT("a self-consistent payload above the allocation cap is refused"),
			Result.Error == ECFDVizError::AllocationTooLarge);
		TestEqual(TEXT("nothing is allocated"), Decoded.Num(), 0);
	}

	// A payload span that runs off the end of the file (rule 1.5).
	{
		const FCFDVizMemoryByteSource Source(
			TArrayView<const uint8>(F32Raw, UE_ARRAY_COUNT(F32Raw)), TEXT("short"));
		FPayloadSpec Spec;
		Spec.Offset = 8;
		Spec.CompressedBytes = 16; // only 8 bytes remain
		Spec.UncompressedBytes = 16;
		Spec.ValueCount = 4;
		Spec.ComponentCount = 1;
		Spec.DataType = ECFDVizDataType::Float32;
		Spec.Codec = ECFDVizCodec::None;
		Spec.PayloadCrc32C = 0;
		TArray<uint8> Decoded;
		TestTrue(TEXT("a payload running past the end of the source is rejected"),
			DecodePayload(Source, Spec, Decoded).Error == ECFDVizError::PayloadOutOfBounds);
	}
	{
		// Offset + Count overflowing int64. A reader that bounds-checks with an
		// addition sees a negative sum and may conclude the span is fine.
		const FCFDVizMemoryByteSource Source(
			TArrayView<const uint8>(F32Raw, UE_ARRAY_COUNT(F32Raw)), TEXT("overflow"));
		FPayloadSpec Spec;
		Spec.Offset = MAX_int64 - 4;
		Spec.CompressedBytes = 16;
		Spec.UncompressedBytes = 16;
		Spec.ValueCount = 4;
		Spec.ComponentCount = 1;
		Spec.DataType = ECFDVizDataType::Float32;
		Spec.Codec = ECFDVizCodec::None;
		Spec.PayloadCrc32C = 0;
		TArray<uint8> Decoded;
		TestTrue(TEXT("an offset that overflows on addition is still rejected"),
			DecodePayload(Source, Spec, Decoded).Error == ECFDVizError::PayloadOutOfBounds);
	}

	// CRC. The corruption is a single flipped bit in the middle of a valid zlib
	// stream, so it would be caught by the CRC and not by the inflater.
	{
		TArray<uint8> Corrupt(F32Zlib, UE_ARRAY_COUNT(F32Zlib));
		const uint32 GoodCrc = Crc32C::Compute(Corrupt);
		Corrupt[6] ^= 0x01;

		const FCFDVizMemoryByteSource Source(Corrupt, TEXT("corrupt"));
		FPayloadSpec Spec;
		Spec.Offset = 0;
		Spec.CompressedBytes = Corrupt.Num();
		Spec.UncompressedBytes = 16;
		Spec.PayloadCrc32C = GoodCrc; // the CRC the writer stored
		Spec.ValueCount = 4;
		Spec.ComponentCount = 1;
		Spec.DataType = ECFDVizDataType::Float32;
		Spec.Codec = ECFDVizCodec::Zlib;

		TArray<uint8> Decoded;
		const FCFDVizResult Result = DecodePayload(Source, Spec, Decoded);
		TestTrue(TEXT("single-bit payload corruption is caught by the CRC"),
			Result.Error == ECFDVizError::PayloadCrcMismatch);
		TestEqual(TEXT("and nothing is returned"), Decoded.Num(), 0);

		// The CRC covers the STORED bytes, so this was caught without inflating -
		// which is what keeps a corrupt stream away from the inflater in the
		// first place. With verification off, the same bytes reach the decoder,
		// and it fails there instead. That failure is logged at Error severity by
		// FCompression itself; downgrading it to keep this test quiet would be
		// hiding a genuine corruption signal, so it is declared expected instead.
		AddExpectedError(TEXT("Failed to uncompress memory"), EAutomationExpectedErrorFlags::Contains, 0);
		TArray<uint8> Unverified;
		const FCFDVizResult NoVerify = DecodePayload(Source, Spec, Unverified, /*bVerifyCrc=*/false);
		TestTrue(TEXT("skipping verification does not make corrupt data decode"),
			NoVerify.Error == ECFDVizError::DecompressionFailed);
	}

	// With codec none, the stored length and the decoded length are the same
	// number by definition; a file that says otherwise is describing a
	// transformation it did not perform.
	{
		const FCFDVizMemoryByteSource Source(
			TArrayView<const uint8>(F32Raw, UE_ARRAY_COUNT(F32Raw)), TEXT("none"));
		FPayloadSpec Spec;
		Spec.Offset = 0;
		Spec.CompressedBytes = 12;
		Spec.UncompressedBytes = 16;
		Spec.PayloadCrc32C = Crc32C::Compute(F32Raw, 12);
		Spec.ValueCount = 4;
		Spec.ComponentCount = 1;
		Spec.DataType = ECFDVizDataType::Float32;
		Spec.Codec = ECFDVizCodec::None;
		TArray<uint8> Decoded;
		TestTrue(TEXT("codec none with mismatched stored and decoded sizes is rejected"),
			DecodePayload(Source, Spec, Decoded).Error == ECFDVizError::SizeMismatch);
	}

	// ComputeStoredCrc must agree with a one-shot CRC and refuse an out-of-bounds
	// span rather than checksumming whatever it can reach.
	{
		const FCFDVizMemoryByteSource Source(
			TArrayView<const uint8>(F32Raw, UE_ARRAY_COUNT(F32Raw)), TEXT("crc"));
		uint32 Crc = 0;
		TestTrue(TEXT("streamed CRC succeeds"), ComputeStoredCrc(Source, 0, 16, Crc));
		TestEqual(TEXT("streamed CRC equals the one-shot CRC"),
			static_cast<int64>(Crc), static_cast<int64>(Crc32C::Compute(F32Raw, 16)));
		TestTrue(TEXT("a partial span also matches"), ComputeStoredCrc(Source, 4, 8, Crc));
		TestEqual(TEXT("partial span CRC"),
			static_cast<int64>(Crc), static_cast<int64>(Crc32C::Compute(F32Raw + 4, 8)));
		TestFalse(TEXT("an out-of-bounds span is refused"), ComputeStoredCrc(Source, 8, 16, Crc));
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* The allocation cap versus TArray's int32 indexing                            */
/* -------------------------------------------------------------------------- */

// NAMED namespace: unity build. The stub must live outside the file's
// anonymous namespace so a same-named helper elsewhere in the blob cannot
// collide with it.
namespace CFDVizPayloadTestLocal
{
	/**
	 * A byte source that CLAIMS an arbitrary size with no backing memory - the
	 * shape of a hostile or corrupt container header. Read refuses, which is
	 * fine: a correctly capped decoder must reject the declared size before it
	 * ever tries to read.
	 */
	class FDeclaredSizeByteSource final : public ICFDVizByteSource
	{
	public:
		explicit FDeclaredSizeByteSource(int64 InSize)
			: Size(InSize)
			, DisplayPath(TEXT("<declared-size stub>"))
		{
		}

		virtual int64 GetSize() const override { return Size; }
		virtual bool Read(int64, int64, void*) const override { return false; }
		virtual const FString& GetDisplayPath() const override { return DisplayPath; }

	private:
		int64 Size;
		FString DisplayPath;
	};
}

/**
 * WHAT THIS PINS. DecodePayload materialises payloads into TArray<uint8>,
 * whose element count is int32 - so nothing above MAX_int32 bytes can ever be
 * allocated, whatever the cap parameter says. The default cap used to be
 * 4 GiB (1LL << 32): a self-consistent header declaring a size in
 * (MAX_int32, 2^32] sailed past it and landed in SetNumUninitialized as a
 * NEGATIVE count - a check() crash on a data-dependent input. The CVF path
 * was immune (it passes MaxReadableBytes = MAX_int32); the CVA path used the
 * default. On pristine code this test CRASHES the process; that was the red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizPayloadAllocationCapTest,
	"FlowViz.CFDViz.Payload.AllocationCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizPayloadAllocationCapTest::RunTest(const FString& Parameters)
{
	using namespace CFDViz;

	// Just past int32: passes the old 4 GiB cap, breaks the int32 narrowing.
	const int64 HugeBytes = (1LL << 31) + 64;

	// --- Codec None: the STORED buffer is the one that would be materialised.
	{
		const CFDVizPayloadTestLocal::FDeclaredSizeByteSource Source(HugeBytes + 128);

		FPayloadSpec Spec;
		Spec.Offset = 0;
		Spec.CompressedBytes = HugeBytes;
		Spec.UncompressedBytes = HugeBytes;
		Spec.PayloadCrc32C = 0;
		Spec.ValueCount = HugeBytes / 4;
		Spec.ComponentCount = 1;
		Spec.DataType = ECFDVizDataType::Float32;
		Spec.Codec = ECFDVizCodec::None;
		Spec.Context = TEXT("huge uncompressed payload");

		TArray<uint8> Decoded;
		const FCFDVizResult Result = DecodePayload(Source, Spec, Decoded);
		TestTrue(TEXT("a declared size just past MAX_int32 fails with AllocationTooLarge"),
			Result.Error == ECFDVizError::AllocationTooLarge);
		TestEqual(TEXT("and nothing is returned"), Decoded.Num(), 0);

		// The clamp must hold against the CALLER's cap too: an explicit 4 GiB
		// budget cannot buy an allocation TArray cannot represent.
		TArray<uint8> DecodedWithCap;
		const FCFDVizResult WithCap =
			DecodePayload(Source, Spec, DecodedWithCap, /*bVerifyCrc=*/true, /*MaxAllocationBytes=*/1LL << 32);
		TestTrue(TEXT("an explicit 4 GiB cap is still clamped to MAX_int32"),
			WithCap.Error == ECFDVizError::AllocationTooLarge);
	}

	// --- Codec Zlib with a small stored span: the DECODED buffer is the one
	// that would be materialised, which is the other narrowing site.
	{
		const CFDVizPayloadTestLocal::FDeclaredSizeByteSource Source(1024);

		FPayloadSpec Spec;
		Spec.Offset = 0;
		Spec.CompressedBytes = 32;
		Spec.UncompressedBytes = HugeBytes;
		Spec.PayloadCrc32C = 0;
		Spec.ValueCount = HugeBytes / 4;
		Spec.ComponentCount = 1;
		Spec.DataType = ECFDVizDataType::Float32;
		Spec.Codec = ECFDVizCodec::Zlib;
		Spec.Context = TEXT("huge decoded payload");

		TArray<uint8> Decoded;
		const FCFDVizResult Result = DecodePayload(Source, Spec, Decoded);
		TestTrue(TEXT("a huge DECODED size behind a small stored span is also refused"),
			Result.Error == ECFDVizError::AllocationTooLarge);
		TestEqual(TEXT("and nothing is returned"), Decoded.Num(), 0);
	}

	// --- Control: a decoded size AT the MAX_int32 boundary passes the cap and
	// fails LATER, at the stub's refusing Read of the small stored span. This
	// is what proves the two arms above failed on the cap rather than on the
	// stub's Read - an all-refusing source would make any error look like one.
	// Zlib with a 32-byte stored span keeps the control allocation-free: the
	// cap check precedes every allocation, and only the 32 stored bytes would
	// ever be materialised before the read refuses.
	{
		const int64 AtLimit = MAX_int32 - 3; // divisible by 4 for float32
		const CFDVizPayloadTestLocal::FDeclaredSizeByteSource Source(1024);

		FPayloadSpec Spec;
		Spec.Offset = 0;
		Spec.CompressedBytes = 32;
		Spec.UncompressedBytes = AtLimit;
		Spec.PayloadCrc32C = 0;
		Spec.ValueCount = AtLimit / 4;
		Spec.ComponentCount = 1;
		Spec.DataType = ECFDVizDataType::Float32;
		Spec.Codec = ECFDVizCodec::Zlib;
		Spec.Context = TEXT("at-limit payload");

		TArray<uint8> Decoded;
		const FCFDVizResult Result = DecodePayload(Source, Spec, Decoded);
		TestTrue(TEXT("a size within MAX_int32 passes the cap and fails at the stub's read"),
			Result.Error == ECFDVizError::FileReadFailed);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
