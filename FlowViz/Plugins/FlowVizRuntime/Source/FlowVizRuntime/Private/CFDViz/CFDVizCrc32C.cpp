// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizCrc32C.h"
#include "FlowVizRuntime.h"

namespace CFDViz::Crc32C
{
	namespace
	{
		/**
		 * Byte-at-a-time lookup table, built at compile time.
		 *
		 * Generated rather than pasted so the polynomial constant in the header is
		 * the single source of truth - a pasted table can silently disagree with
		 * the documented parameters.
		 */
		struct FTable
		{
			uint32 Entries[256];

			constexpr FTable()
				: Entries()
			{
				for (uint32 Byte = 0; Byte < 256; ++Byte)
				{
					uint32 Crc = Byte;
					for (int32 Bit = 0; Bit < 8; ++Bit)
					{
						// Reflected algorithm: shift right, xor polynomial when the low bit is set.
						Crc = (Crc >> 1) ^ ((Crc & 1u) ? PolyReflected : 0u);
					}
					Entries[Byte] = Crc;
				}
			}
		};

		inline constexpr FTable Table{};
	}

	uint32 Compute(const void* Data, int64 Size, uint32 PreviousCrc)
	{
		if (Size <= 0)
		{
			return PreviousCrc;
		}
		check(Data != nullptr);

		uint32 Crc = PreviousCrc ^ 0xFFFFFFFFu; // undo the final XOR of any previous chunk

		const uint8* Bytes = static_cast<const uint8*>(Data);
		for (int64 Index = 0; Index < Size; ++Index)
		{
			Crc = Table.Entries[(Crc ^ Bytes[Index]) & 0xFFu] ^ (Crc >> 8);
		}

		return Crc ^ 0xFFFFFFFFu;
	}

	uint32 Compute(TArrayView<const uint8> Data, uint32 PreviousCrc)
	{
		return Compute(Data.GetData(), Data.Num(), PreviousCrc);
	}

	bool SelfCheck()
	{
		const ANSICHAR* Vector = "123456789";
		const uint32 Actual = Compute(Vector, 9);
		if (Actual != CheckValue)
		{
			UE_LOG(LogFlowViz, Error,
				TEXT("CRC-32C self-check FAILED: got 0x%08X, expected 0x%08X. ")
				TEXT("This implementation is incorrect and must not be used to validate files."),
				Actual, CheckValue);
			return false;
		}
		return true;
	}
}
