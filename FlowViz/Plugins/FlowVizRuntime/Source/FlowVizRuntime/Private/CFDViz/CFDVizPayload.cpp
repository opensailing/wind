// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizPayload.h"

#include "CFDViz/CFDVizCrc32C.h"
#include "Misc/Compression.h"

namespace CFDViz
{
namespace
{
	/**
	 * Chunk size for streaming CRC verification.
	 *
	 * 64 KiB keeps validating a multi-gigabyte case off the heap: the whole
	 * point of checksumming the STORED bytes is that integrity is checkable
	 * without materialising the decoded payload, and slurping the compressed
	 * payload instead would give most of that back.
	 */
	constexpr int64 CrcChunkBytes = 64 * 1024;

	/** Context suffix for error messages, e.g. " in brick 12". Empty when unnamed. */
	FString FormatContext(const FString& Context)
	{
		return Context.IsEmpty() ? FString() : FString::Printf(TEXT(" in %s"), *Context);
	}
}

bool ComputeStoredCrc(const ICFDVizByteSource& Source, int64 Offset, int64 Count, uint32& OutCrc)
{
	if (!Source.Contains(Offset, Count))
	{
		return false;
	}

	TArray<uint8> Chunk;
	Chunk.SetNumUninitialized(static_cast<int32>(FMath::Min(Count, CrcChunkBytes)));

	uint32 Crc = 0;
	int64 Remaining = Count;
	int64 Cursor = Offset;
	while (Remaining > 0)
	{
		const int64 ThisChunk = FMath::Min(Remaining, CrcChunkBytes);
		if (!Source.Read(Cursor, ThisChunk, Chunk.GetData()))
		{
			return false;
		}
		Crc = Crc32C::Compute(Chunk.GetData(), ThisChunk, Crc);
		Cursor += ThisChunk;
		Remaining -= ThisChunk;
	}

	OutCrc = Crc;
	return true;
}

FCFDVizResult DecodePayload(
	const ICFDVizByteSource& Source,
	const FPayloadSpec& Spec,
	TArray<uint8>& OutBytes,
	bool bVerifyCrc,
	int64 MaxAllocationBytes)
{
	const FString Where = FormatContext(Spec.Context);
	const FString& Path = Source.GetDisplayPath();

	// --- 1. zstd, first and unconditionally.
	//
	// Ahead of every other check on purpose: a zstd payload is one this build
	// cannot decode at all, so the actionable answer is "re-export", not
	// whatever the next check would have said about its size. The message is
	// normative to the character (section 7) and there is deliberately no
	// fallback codec - silently inflating zstd bytes as zlib would either fail
	// obscurely or, worse, produce data.
	if (Spec.Codec == ECFDVizCodec::Zstd)
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedCodec,
			FString(ZstdRejectionMessage), Path, Spec.Offset);
	}
	if (!IsCodecSupported(Spec.Codec))
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedCodec,
			FString::Printf(TEXT("unsupported codec %d%s"), static_cast<int32>(Spec.Codec), *Where),
			Path, Spec.Offset);
	}

	// --- 2. The declared decoded size must equal the size the geometry implies,
	// checked BEFORE anything is allocated (4.4.4 / 6.3.2). This is the primary
	// defence against a hostile size field, and it is worthless if it runs after
	// the allocation it exists to guard.
	const int64 ExpectedBytes = ComputePayloadBytes(Spec.ValueCount, Spec.ComponentCount, Spec.DataType);
	if (ExpectedBytes == INDEX_NONE)
	{
		return FCFDVizResult::Fail(ECFDVizError::SizeMismatch,
			FString::Printf(
				TEXT("payload size overflows: %lld values x %d components x %s%s"),
				Spec.ValueCount, Spec.ComponentCount, DataTypeToString(Spec.DataType), *Where),
			Path, Spec.Offset);
	}
	if (Spec.UncompressedBytes != ExpectedBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::SizeMismatch,
			FString::Printf(
				TEXT("uncompressedBytes is %lld but %lld values x %d components x %s requires %lld%s"),
				Spec.UncompressedBytes, Spec.ValueCount, Spec.ComponentCount,
				DataTypeToString(Spec.DataType), ExpectedBytes, *Where),
			Path, Spec.Offset);
	}

	// With no codec the stored length and the decoded length are the same number
	// by definition. A file that says otherwise is describing a transformation
	// it did not perform, and trusting either number would be a guess.
	if (Spec.Codec == ECFDVizCodec::None && Spec.CompressedBytes != Spec.UncompressedBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::SizeMismatch,
			FString::Printf(
				TEXT("codec is none but compressedBytes (%lld) != uncompressedBytes (%lld)%s"),
				Spec.CompressedBytes, Spec.UncompressedBytes, *Where),
			Path, Spec.Offset);
	}

	// A separate limit from the equality above, catching a different lie: a
	// header can be entirely self-consistent and still declare a petabyte.
	if (Spec.UncompressedBytes > MaxAllocationBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::AllocationTooLarge,
			FString::Printf(TEXT("payload of %lld bytes exceeds the %lld byte limit%s"),
				Spec.UncompressedBytes, MaxAllocationBytes, *Where),
			Path, Spec.Offset);
	}
	if (Spec.CompressedBytes < 0 || Spec.CompressedBytes > MaxAllocationBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::AllocationTooLarge,
			FString::Printf(TEXT("stored payload of %lld bytes exceeds the %lld byte limit%s"),
				Spec.CompressedBytes, MaxAllocationBytes, *Where),
			Path, Spec.Offset);
	}

	// --- 3. The stored span must lie inside the file (rule 1.5). Contains()
	// compares against the remaining length rather than adding, so an offset
	// near 2^63 cannot wrap into a passing comparison.
	if (!Source.Contains(Spec.Offset, Spec.CompressedBytes))
	{
		return FCFDVizResult::Fail(ECFDVizError::PayloadOutOfBounds,
			FString::Printf(
				TEXT("payload of %lld bytes at offset %lld runs past the end of the %lld byte file%s"),
				Spec.CompressedBytes, Spec.Offset, Source.GetSize(), *Where),
			Path, Spec.Offset);
	}

	// Read the stored bytes once; both the CRC and the decoder work from this
	// buffer, so a file that changed between the two would not slip through.
	TArray<uint8> Stored;
	Stored.SetNumUninitialized(static_cast<int32>(Spec.CompressedBytes));
	if (Spec.CompressedBytes > 0 && !Source.Read(Spec.Offset, Spec.CompressedBytes, Stored.GetData()))
	{
		return FCFDVizResult::Fail(ECFDVizError::FileReadFailed,
			FString::Printf(TEXT("could not read %lld payload bytes at offset %lld%s"),
				Spec.CompressedBytes, Spec.Offset, *Where),
			Path, Spec.Offset);
	}

	// --- 4. CRC over the COMPRESSED bytes as stored. Because the checksum
	// covers what is on disk, corruption is caught WITHOUT decompressing -
	// which matters, because handing corrupt bytes to an inflater is how a
	// reader turns a data error into a crash.
	if (bVerifyCrc)
	{
		const uint32 Actual = Crc32C::Compute(Stored.GetData(), Stored.Num());
		if (Actual != Spec.PayloadCrc32C)
		{
			return FCFDVizResult::Fail(ECFDVizError::PayloadCrcMismatch,
				FString::Printf(TEXT("payload CRC mismatch%s: expected 0x%08X, computed 0x%08X"),
					*Where, Spec.PayloadCrc32C, Actual),
				Path, Spec.Offset);
		}
	}

	// --- 5. Only now decode. OutBytes is not touched until this point, so a
	// caller cannot mistake a partially written buffer for data.
	if (Spec.Codec == ECFDVizCodec::None)
	{
		OutBytes = MoveTemp(Stored);
		return FCFDVizResult::Ok();
	}

	TArray<uint8> Decoded;
	Decoded.SetNumUninitialized(static_cast<int32>(Spec.UncompressedBytes));

	// The decoded size is always known from the header, so it is passed in
	// rather than discovered. FCompression rejects a stream that decodes to a
	// different length, which is the check that makes a truncated or spliced
	// payload fail instead of yielding a short buffer.
	const bool bOk = FCompression::UncompressMemory(
		CodecToFName(Spec.Codec),
		Decoded.GetData(), Spec.UncompressedBytes,
		Stored.GetData(), Spec.CompressedBytes);

	if (!bOk)
	{
		return FCFDVizResult::Fail(ECFDVizError::DecompressionFailed,
			FString::Printf(TEXT("%s decompression failed for %lld stored bytes%s"),
				CodecToString(Spec.Codec), Spec.CompressedBytes, *Where),
			Path, Spec.Offset);
	}

	OutBytes = MoveTemp(Decoded);
	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* Value access                                                                 */
/* -------------------------------------------------------------------------- */

bool TryReadValueBits(TArrayView<const uint8> Bytes, int64 Index, ECFDVizDataType DataType, uint64& OutBits)
{
	const int32 ElementBytes = SizeOfDataType(DataType);
	if (ElementBytes <= 0 || Index < 0)
	{
		return false;
	}

	// Multiplied, then compared against the remaining length - never added to a
	// base offset, which is where these checks usually overflow.
	const int64 ByteOffset = Index * static_cast<int64>(ElementBytes);
	if (ByteOffset < 0 || ByteOffset > Bytes.Num() || ElementBytes > Bytes.Num() - ByteOffset)
	{
		return false;
	}

	const uint8* Data = Bytes.GetData() + ByteOffset;

	// Assembled byte by byte from the little-endian stream, so the result is the
	// same on a big-endian host and no unaligned load is performed. That
	// matters: a brick payload is not aligned to anything in particular.
	uint64 Bits = 0;
	for (int32 Byte = ElementBytes - 1; Byte >= 0; --Byte)
	{
		Bits = (Bits << 8) | static_cast<uint64>(Data[Byte]);
	}
	OutBits = Bits;
	return true;
}

bool TryReadValueAsDouble(TArrayView<const uint8> Bytes, int64 Index, ECFDVizDataType DataType, double& OutValue)
{
	uint64 Bits = 0;
	if (!TryReadValueBits(Bytes, Index, DataType, Bits))
	{
		return false;
	}

	switch (DataType)
	{
	case ECFDVizDataType::UInt8:
		// Widened as unsigned. Reading 255 as -1 is the classic char bug, and on
		// a mask field it inverts the meaning of every masked cell.
		OutValue = static_cast<double>(static_cast<uint8>(Bits));
		return true;

	case ECFDVizDataType::Float16:
	{
		// LoadHalf handles the Inf/NaN exponent explicitly, so a float16 NaN
		// widens to a float NaN rather than to a large finite value.
		const uint16 Half = static_cast<uint16>(Bits);
		OutValue = static_cast<double>(FPlatformMath::LoadHalf(&Half));
		return true;
	}

	case ECFDVizDataType::Float32:
	{
		// Memcpy, not a pointer cast: type punning through a cast is undefined
		// behaviour, and this is the only form that reliably preserves a NaN
		// payload (rule 1.7).
		const uint32 Word = static_cast<uint32>(Bits);
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Word, sizeof(Value));
		OutValue = static_cast<double>(Value);
		return true;
	}

	case ECFDVizDataType::Float64:
	{
		double Value = 0.0;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		OutValue = Value;
		return true;
	}

	default:
		return false;
	}
}

} // namespace CFDViz
