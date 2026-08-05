// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizByteSource.h"
#include "CFDViz/CFDVizTypes.h"

/**
 * The one payload decoder, shared by CVF bricks and CVA arrays.
 *
 * Format section 6.3 opens with "These mirror CVF section 4.4 exactly, so both
 * payload decoders behave the same way". Two decoders that are *meant* to behave
 * identically will eventually not, so there is one, and both containers call it.
 *
 * The ordering below is normative, not stylistic. Each step is a precondition
 * for the next:
 *
 *  1. Reject the reserved zstd codec, with exactly CFDViz::ZstdRejectionMessage
 *     and never a fallback codec (section 7).
 *  2. Verify uncompressedBytes == values * components * sizeof(dataType)
 *     BEFORE allocating (sections 4.4.4 / 6.3.2). This is the primary defence
 *     against a hostile size field, and it is worthless if it runs after the
 *     allocation it is supposed to guard.
 *  3. Verify the span lies inside the file (rule 1.5).
 *  4. Verify the payload CRC over the COMPRESSED bytes as stored. Because the
 *     CRC covers the stored bytes, corruption is caught without decompressing -
 *     which matters, since handing corrupt bytes to an inflater is how a reader
 *     turns a data error into a crash.
 *  5. Only then decompress.
 *
 * Nothing here touches a UObject or the game thread.
 */
namespace CFDViz
{
	/** Everything the decoder needs about one stored payload. */
	struct FPayloadSpec
	{
		/** Absolute byte offset of the stored (compressed) payload. */
		int64 Offset = 0;

		/** Stored length in bytes. For codec None this must equal UncompressedBytes. */
		int64 CompressedBytes = 0;

		/** Declared decoded length, checked against the derived size before allocating. */
		int64 UncompressedBytes = 0;

		/** CRC-32C over the stored (still compressed) bytes. */
		uint32 PayloadCrc32C = 0;

		/** Values in this payload - voxels for a CVF brick, entities for a CVA. */
		int64 ValueCount = 0;

		/** Components interleaved per value. */
		int32 ComponentCount = 1;

		ECFDVizDataType DataType = ECFDVizDataType::Float32;
		ECFDVizCodec Codec = ECFDVizCodec::Zlib;

		/** Included in error messages so a failure names the brick, e.g. "brick 12". */
		FString Context;
	};

	/**
	 * Read, verify and decode one payload into OutBytes.
	 *
	 * OutBytes is resized to exactly UncompressedBytes on success and left
	 * untouched on failure, so a caller cannot mistake a partially written buffer
	 * for data.
	 *
	 * @param bVerifyCrc Pass false ONLY from a validator that wants to report
	 *                   corruption rather than stop at it (section 10 allows a
	 *                   case to keep loading around a bad brick). Normal reads
	 *                   leave it true; a reader must never silently substitute
	 *                   zeros for a brick that failed its CRC (section 4.4.8).
	 * @param MaxAllocationBytes Refuse to allocate more than this. A cap is not
	 *                   redundant with the size check above: a *self-consistent*
	 *                   header can still declare a petabyte, and the two checks
	 *                   catch different lies.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult DecodePayload(
		const ICFDVizByteSource& Source,
		const FPayloadSpec& Spec,
		TArray<uint8>& OutBytes,
		bool bVerifyCrc = true,
		int64 MaxAllocationBytes = 1LL << 32);

	/**
	 * CRC-32C over a stored span, without decoding it.
	 *
	 * Streams in fixed chunks, so validating a large case does not require the
	 * whole payload to be resident. Returns false when the span is out of bounds
	 * or a read failed.
	 */
	FLOWVIZRUNTIME_API bool ComputeStoredCrc(
		const ICFDVizByteSource& Source,
		int64 Offset,
		int64 Count,
		uint32& OutCrc);

	/**
	 * Widen one stored value to double, for statistics and for value queries.
	 *
	 * float16, float32 and uint8 all convert to double exactly, so no precision
	 * is invented and none is lost. NaN is preserved rather than coerced
	 * (rule 1.7).
	 *
	 * @param Index Element index, NOT a byte offset.
	 * @return false when the index or the data type would read outside Bytes.
	 */
	FLOWVIZRUNTIME_API bool TryReadValueAsDouble(
		TArrayView<const uint8> Bytes,
		int64 Index,
		ECFDVizDataType DataType,
		double& OutValue);

	/**
	 * Raw stored bit pattern of one value, as the exact integer that is on disk:
	 * 8 bits for uint8, 16 for float16, 32 for float32, 64 for float64.
	 *
	 * This is what the section 9 cross-language bridge compares - `bits`, never
	 * `value` - because a bit pattern is immune to the text-formatting and
	 * rounding differences that make a float comparison between two languages
	 * unreliable. It is also the only way to assert that a NaN payload survived
	 * unchanged, since NaN != NaN.
	 */
	FLOWVIZRUNTIME_API bool TryReadValueBits(
		TArrayView<const uint8> Bytes,
		int64 Index,
		ECFDVizDataType DataType,
		uint64& OutBits);
}
