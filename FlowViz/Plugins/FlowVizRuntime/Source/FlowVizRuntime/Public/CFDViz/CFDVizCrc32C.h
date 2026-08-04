// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * CRC-32C (Castagnoli) - the checksum used by every CFDViz binary format.
 *
 * This is NOT FCrc::MemCrc32 (Unreal's own variant) and NOT the CRC-32 used by
 * zip/zlib (polynomial 0x04C11DB7). Substituting either produces files that one
 * implementation writes and the other rejects, so the check value below is
 * asserted by automation tests on this side and by the import-time self-check in
 * Tools/cfdviz/src/cfdviz/crc32c.py on the Python side.
 *
 * Parameters (see Docs/CFDVIZ_FORMAT.md section 8):
 *
 *     polynomial (reflected)  0x82F63B78
 *     initial value           0xFFFFFFFF
 *     input/output reflected  yes
 *     final XOR               0xFFFFFFFF
 */
namespace CFDViz::Crc32C
{
	/** Reflected Castagnoli polynomial. */
	inline constexpr uint32 PolyReflected = 0x82F63B78u;

	/**
	 * Standard check value: Compute("123456789", 9).
	 *
	 * Any implementation that fails this is wrong, and every file it has written
	 * is unreadable by a conforming reader.
	 */
	inline constexpr uint32 CheckValue = 0xE3069283u;

	/**
	 * Compute CRC-32C over a buffer.
	 *
	 * @param Data      Bytes to checksum. May be null only when Size is 0.
	 * @param Size      Number of bytes.
	 * @param PreviousCrc Running CRC from a previous call, for streaming. Pass
	 *                    the previous return value directly; pre/post
	 *                    conditioning is handled internally, exactly matching the
	 *                    Python reference's `crc` parameter.
	 * @return The CRC-32C as an unsigned 32-bit integer.
	 */
	FLOWVIZRUNTIME_API uint32 Compute(const void* Data, int64 Size, uint32 PreviousCrc = 0);

	/** Convenience overload for a contiguous byte view. */
	FLOWVIZRUNTIME_API uint32 Compute(TArrayView<const uint8> Data, uint32 PreviousCrc = 0);

	/**
	 * Return true if this implementation reproduces CheckValue.
	 *
	 * Called by the runtime module at startup so a miscompiled or mistakenly
	 * edited table fails loudly and immediately, rather than silently accepting
	 * corrupt data or rejecting valid data.
	 */
	FLOWVIZRUNTIME_API bool SelfCheck();
}
