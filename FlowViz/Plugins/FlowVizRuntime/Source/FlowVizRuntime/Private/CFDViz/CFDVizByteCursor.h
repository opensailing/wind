// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Bounds-checked little-endian cursor over a byte span.
 *
 * Private to the readers. It exists because format section 4 requires that
 * serialization be "manual, field by field" and that an implementation MUST NOT
 * memcpy a native struct over a header: C++ inserts padding wherever it likes,
 * so a struct that happens to match on Clang/arm64 will not match on another
 * compiler, and the file would be silently misparsed rather than rejected.
 *
 * Every integer is assembled byte by byte from the little-endian stream, so the
 * result is identical on a big-endian host. That is not hypothetical
 * defensiveness - it is what makes the endian marker check meaningful. If the
 * reader used host-order loads, a big-endian build would misread the marker
 * itself and could not report "byte order not supported".
 *
 * Floats go through FMemory::Memcpy from the assembled integer rather than a
 * pointer cast, which is the only way to reinterpret bits without undefined
 * behaviour, and is what preserves a NaN payload bit-exactly (format rule 1.7).
 *
 * Nothing here allocates or touches a UObject; it is callable from a worker
 * thread, as every CFDViz reader must be.
 */
namespace CFDViz
{
	class FByteCursor
	{
	public:
		FByteCursor() = default;

		explicit FByteCursor(TArrayView<const uint8> InBytes)
			: Data(InBytes.GetData())
			, Size(InBytes.Num())
		{
		}

		FByteCursor(const uint8* InData, int64 InSize)
			: Data(InData)
			, Size(InSize > 0 ? InSize : 0)
		{
		}

		/** Current read position. */
		int64 Tell() const { return Position; }

		/** Total bytes available. */
		int64 Num() const { return Size; }

		/** Bytes between the cursor and the end. Never negative. */
		int64 Remaining() const { return Size - Position; }

		/**
		 * Move the cursor. Returns false, leaving the cursor untouched, for a
		 * negative offset or one past the end - so a corrupt offset field cannot
		 * park the cursor somewhere a later read would treat as valid.
		 */
		bool Seek(int64 Offset)
		{
			if (Offset < 0 || Offset > Size)
			{
				return false;
			}
			Position = Offset;
			return true;
		}

		/** True if Count bytes can be read from Offset without leaving the span. */
		bool CanRead(int64 Offset, int64 Count) const
		{
			// Count is subtracted from the remaining length rather than added to
			// Offset, because Offset + Count is exactly the addition that overflows
			// when a hostile header claims a 2^63 length.
			return Offset >= 0 && Count >= 0 && Offset <= Size && Count <= Size - Offset;
		}

		bool ReadUInt8(uint8& Out)
		{
			if (Remaining() < 1)
			{
				return false;
			}
			Out = Data[Position++];
			return true;
		}

		bool ReadUInt16(uint16& Out)
		{
			if (Remaining() < 2)
			{
				return false;
			}
			Out = static_cast<uint16>(
				static_cast<uint16>(Data[Position])
				| (static_cast<uint16>(Data[Position + 1]) << 8));
			Position += 2;
			return true;
		}

		bool ReadUInt32(uint32& Out)
		{
			if (Remaining() < 4)
			{
				return false;
			}
			Out = static_cast<uint32>(Data[Position])
				| (static_cast<uint32>(Data[Position + 1]) << 8)
				| (static_cast<uint32>(Data[Position + 2]) << 16)
				| (static_cast<uint32>(Data[Position + 3]) << 24);
			Position += 4;
			return true;
		}

		bool ReadUInt64(uint64& Out)
		{
			if (Remaining() < 8)
			{
				return false;
			}
			uint64 Value = 0;
			for (int32 Index = 7; Index >= 0; --Index)
			{
				Value = (Value << 8) | static_cast<uint64>(Data[Position + Index]);
			}
			Position += 8;
			Out = Value;
			return true;
		}

		/** IEEE 754 binary32, bit pattern preserved (so NaN survives). */
		bool ReadFloat(float& Out)
		{
			uint32 Bits = 0;
			if (!ReadUInt32(Bits))
			{
				return false;
			}
			float Value = 0.0f;
			FMemory::Memcpy(&Value, &Bits, sizeof(Value));
			Out = Value;
			return true;
		}

		/** IEEE 754 binary64, bit pattern preserved. */
		bool ReadDouble(double& Out)
		{
			uint64 Bits = 0;
			if (!ReadUInt64(Bits))
			{
				return false;
			}
			double Value = 0.0;
			FMemory::Memcpy(&Value, &Bits, sizeof(Value));
			Out = Value;
			return true;
		}

		/** Copy Count raw bytes out. */
		bool ReadBytes(void* OutBuffer, int64 Count)
		{
			if (Count < 0 || Remaining() < Count)
			{
				return false;
			}
			if (Count > 0)
			{
				FMemory::Memcpy(OutBuffer, Data + Position, static_cast<SIZE_T>(Count));
			}
			Position += Count;
			return true;
		}

		/**
		 * Consume Count bytes and report whether every one of them was zero.
		 *
		 * Used for the `reserved = 0` fields the format declares. A non-zero
		 * reserved field means the file uses something this reader does not
		 * implement, and is rejected rather than ignored - ignoring it is how a
		 * future extension gets silently misread as 1.0 data.
		 */
		bool ReadIsAllZero(int64 Count, bool& bOutAllZero)
		{
			if (Count < 0 || Remaining() < Count)
			{
				return false;
			}
			bool bAllZero = true;
			for (int64 Index = 0; Index < Count; ++Index)
			{
				if (Data[Position + Index] != 0)
				{
					bAllZero = false;
					break;
				}
			}
			Position += Count;
			bOutAllZero = bAllZero;
			return true;
		}

	private:
		const uint8* Data = nullptr;
		int64 Size = 0;
		int64 Position = 0;
	};
}
