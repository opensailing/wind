// Copyright FlowViz contributors. All Rights Reserved.

#include "../CFDViz/CFDVizByteCursor.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Direct test for the bounds-checked byte cursor.
 *
 * Every reader exercises this class, so it is not *uncovered* - but that
 * indirect coverage feeds it almost entirely VALID input, and its entire job is
 * what happens on invalid input. It is the mechanism behind non-negotiable rule
 * 12 ("all package offsets, dimensions, counts, and uncompressed sizes must be
 * bounds-checked before allocation"), and rule 12 is exactly as strong as this
 * file is.
 *
 * Two ideas govern the assertions:
 *
 * 1. **A check that a wrong implementation also passes is not a check.** The
 *    overflow-safe `CanRead` and the naive `Offset + Count <= Size` agree on
 *    every ordinary input. They disagree only when the addition wraps. So the
 *    hostile cases below use counts near INT64_MAX, chosen so the naive form
 *    computes a negative number and returns true.
 *
 * 2. **Rejection must be inert.** A failed read that has already advanced the
 *    cursor, or a failed Seek that has already moved it, converts a detected
 *    error into an undetected misparse further along. Every failure case here
 *    asserts the cursor did not move.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizByteCursorTest,
	"FlowViz.CFDViz.ByteCursor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace
{
	/**
	 * Sixteen bytes whose every multi-byte reading is distinct and asymmetric.
	 *
	 * No byte is zero (so a zero-fill bug cannot pass), no two adjacent bytes
	 * are equal (so a byte-order flip always changes the value), and the
	 * sequence is not a palindrome at any width.
	 */
	const uint8 Fixture[16] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
	};
}

bool FCFDVizByteCursorTest::RunTest(const FString& Parameters)
{
	using CFDViz::FByteCursor;

	const TArrayView<const uint8> Bytes(Fixture, 16);

	/* --------------------------------------------------------------------- */
	/* Little-endian assembly, byte by byte                                    */
	/* --------------------------------------------------------------------- */
	//
	// The values are written out in full rather than computed, so that a test
	// that "assembles the same way the code does" cannot agree with a broken
	// implementation. A host-order load on this arm64 machine would produce
	// these same numbers - which is the point of the big-endian companion
	// assertion below: the same bytes read the OTHER way must NOT match.
	{
		FByteCursor Cursor(Bytes);

		uint8 U8 = 0;
		TestTrue(TEXT("uint8 reads"), Cursor.ReadUInt8(U8));
		TestEqual(TEXT("uint8 value"), U8, static_cast<uint8>(0x01));
		TestEqual(TEXT("uint8 advanced by 1"), Cursor.Tell(), static_cast<int64>(1));

		Cursor.Seek(0);
		uint16 U16 = 0;
		TestTrue(TEXT("uint16 reads"), Cursor.ReadUInt16(U16));
		TestEqual(TEXT("uint16 is little-endian"), U16, static_cast<uint16>(0x0201));
		TestNotEqual(TEXT("uint16 is NOT big-endian"), U16, static_cast<uint16>(0x0102));
		TestEqual(TEXT("uint16 advanced by 2"), Cursor.Tell(), static_cast<int64>(2));

		Cursor.Seek(0);
		uint32 U32 = 0;
		TestTrue(TEXT("uint32 reads"), Cursor.ReadUInt32(U32));
		TestEqual(TEXT("uint32 is little-endian"), U32, static_cast<uint32>(0x04030201));
		TestNotEqual(TEXT("uint32 is NOT big-endian"), U32, static_cast<uint32>(0x01020304));
		TestEqual(TEXT("uint32 advanced by 4"), Cursor.Tell(), static_cast<int64>(4));

		Cursor.Seek(8);
		uint64 U64 = 0;
		TestTrue(TEXT("uint64 reads"), Cursor.ReadUInt64(U64));
		// Reading from offset 8 (0x11..0x88) rather than 0, so the top byte is
		// 0x88 - a value with the high bit set, which catches a shift done on a
		// signed type or a loop that stops at 7 bits.
		TestEqual(TEXT("uint64 is little-endian"), U64, static_cast<uint64>(0x8877665544332211ull));
		TestNotEqual(TEXT("uint64 is NOT big-endian"), U64, static_cast<uint64>(0x1122334455667788ull));
		TestEqual(TEXT("uint64 advanced by 8"), Cursor.Tell(), static_cast<int64>(16));
	}

	/* --------------------------------------------------------------------- */
	/* Reads stop at the end, and a refused read does not move the cursor      */
	/* --------------------------------------------------------------------- */
	//
	// Each width is tested one byte short of what it needs. A cursor that
	// advanced before checking would read past the span; a cursor that advanced
	// *after* failing would leave the position wrong for the next read, which is
	// how a rejected field turns into a misparse of the following one.
	{
		uint16 U16 = 0;
		uint32 U32 = 0;
		uint64 U64 = 0;

		FByteCursor C2(Bytes);
		TestTrue(TEXT("seek to 1 short of a uint16"), C2.Seek(15));
		TestFalse(TEXT("uint16 refuses with 1 byte left"), C2.ReadUInt16(U16));
		TestEqual(TEXT("refused uint16 left the cursor alone"), C2.Tell(), static_cast<int64>(15));

		FByteCursor C4(Bytes);
		TestTrue(TEXT("seek to 3 short of a uint32"), C4.Seek(13));
		TestFalse(TEXT("uint32 refuses with 3 bytes left"), C4.ReadUInt32(U32));
		TestEqual(TEXT("refused uint32 left the cursor alone"), C4.Tell(), static_cast<int64>(13));

		FByteCursor C8(Bytes);
		TestTrue(TEXT("seek to 7 short of a uint64"), C8.Seek(9));
		TestFalse(TEXT("uint64 refuses with 7 bytes left"), C8.ReadUInt64(U64));
		TestEqual(TEXT("refused uint64 left the cursor alone"), C8.Tell(), static_cast<int64>(9));

		// At the very end, with nothing left at all.
		FByteCursor CEnd(Bytes);
		TestTrue(TEXT("seek to the end is legal"), CEnd.Seek(16));
		TestEqual(TEXT("nothing remains"), CEnd.Remaining(), static_cast<int64>(0));
		uint8 U8 = 0;
		TestFalse(TEXT("uint8 refuses at the end"), CEnd.ReadUInt8(U8));
		TestEqual(TEXT("refused uint8 left the cursor alone"), CEnd.Tell(), static_cast<int64>(16));
	}

	/* --------------------------------------------------------------------- */
	/* Seek rejects out-of-range offsets without moving                        */
	/* --------------------------------------------------------------------- */
	{
		FByteCursor Cursor(Bytes);
		TestTrue(TEXT("seek into the span"), Cursor.Seek(7));

		TestFalse(TEXT("seek rejects a negative offset"), Cursor.Seek(-1));
		TestEqual(TEXT("rejected negative seek did not move"), Cursor.Tell(), static_cast<int64>(7));

		TestFalse(TEXT("seek rejects one past the end"), Cursor.Seek(17));
		TestEqual(TEXT("rejected past-end seek did not move"), Cursor.Tell(), static_cast<int64>(7));

		TestFalse(TEXT("seek rejects INT64_MIN"), Cursor.Seek(TNumericLimits<int64>::Min()));
		TestEqual(TEXT("rejected INT64_MIN seek did not move"), Cursor.Tell(), static_cast<int64>(7));

		TestFalse(TEXT("seek rejects INT64_MAX"), Cursor.Seek(TNumericLimits<int64>::Max()));
		TestEqual(TEXT("rejected INT64_MAX seek did not move"), Cursor.Tell(), static_cast<int64>(7));

		// Seeking exactly to the end IS legal - it is the natural end state of a
		// complete parse, and rejecting it would make a correct reader look
		// broken. This is the boundary an off-by-one gets wrong in either
		// direction, so both sides of it are asserted.
		TestTrue(TEXT("seek to exactly the end is accepted"), Cursor.Seek(16));
		TestEqual(TEXT("accepted end seek moved"), Cursor.Tell(), static_cast<int64>(16));
	}

	/* --------------------------------------------------------------------- */
	/* CanRead survives hostile arithmetic                                     */
	/* --------------------------------------------------------------------- */
	//
	// This is the reason the class exists in this form. The naive formulation
	//
	//     Offset + Count <= Size
	//
	// agrees with the real one on every input above. The counts below are chosen
	// so that the naive addition OVERFLOWS to a negative number and the naive
	// check returns true - handing a caller permission to allocate and read a
	// span that does not exist. A test that omitted these would pass against
	// both implementations and prove nothing.
	{
		FByteCursor Cursor(Bytes);
		const int64 Max = TNumericLimits<int64>::Max();

		TestTrue(TEXT("ordinary in-range read is allowed"), Cursor.CanRead(0, 16));
		TestTrue(TEXT("zero-length read at the end is allowed"), Cursor.CanRead(16, 0));
		TestTrue(TEXT("in-range read at an offset is allowed"), Cursor.CanRead(8, 8));

		TestFalse(TEXT("one byte past the end is refused"), Cursor.CanRead(0, 17));
		TestFalse(TEXT("one byte past the end at an offset is refused"), Cursor.CanRead(8, 9));
		TestFalse(TEXT("a negative offset is refused"), Cursor.CanRead(-1, 1));
		TestFalse(TEXT("a negative count is refused"), Cursor.CanRead(0, -1));
		TestFalse(TEXT("an offset past the end is refused"), Cursor.CanRead(17, 0));

		// The overflow cases. Offset + Count wraps for each of these.
		TestFalse(TEXT("INT64_MAX count at offset 0 is refused"), Cursor.CanRead(0, Max));
		TestFalse(TEXT("INT64_MAX count at a nonzero offset is refused"), Cursor.CanRead(8, Max));
		TestFalse(TEXT("a count that wraps to exactly 0 is refused"), Cursor.CanRead(1, Max));
		TestFalse(TEXT("INT64_MAX offset with a large count is refused"), Cursor.CanRead(Max, Max));
		TestFalse(TEXT("a 2^62 count is refused"), Cursor.CanRead(0, int64(1) << 62));

		// A 2^63-magnitude value as a header would produce it: the format's
		// uint64 length fields are narrowed to int64, so a hostile 0x8000...
		// arrives here as INT64_MIN. It must be refused as a negative count,
		// not wrapped into something plausible.
		TestFalse(TEXT("INT64_MIN count is refused as negative"),
			Cursor.CanRead(0, TNumericLimits<int64>::Min()));
	}

	/* --------------------------------------------------------------------- */
	/* ReadBytes                                                               */
	/* --------------------------------------------------------------------- */
	{
		FByteCursor Cursor(Bytes);
		uint8 Buffer[8] = {};

		TestTrue(TEXT("seek before a bulk read"), Cursor.Seek(8));
		TestTrue(TEXT("bulk read of the last 8 bytes"), Cursor.ReadBytes(Buffer, 8));
		TestEqual(TEXT("bulk read copied the first byte"), Buffer[0], static_cast<uint8>(0x11));
		TestEqual(TEXT("bulk read copied the last byte"), Buffer[7], static_cast<uint8>(0x88));
		TestEqual(TEXT("bulk read advanced to the end"), Cursor.Tell(), static_cast<int64>(16));

		TestFalse(TEXT("bulk read past the end is refused"), Cursor.ReadBytes(Buffer, 1));
		TestEqual(TEXT("refused bulk read left the cursor alone"), Cursor.Tell(), static_cast<int64>(16));

		Cursor.Seek(0);
		TestFalse(TEXT("bulk read of a negative count is refused"), Cursor.ReadBytes(Buffer, -1));
		TestEqual(TEXT("refused negative bulk read left the cursor alone"), Cursor.Tell(), static_cast<int64>(0));

		TestFalse(TEXT("bulk read of INT64_MAX is refused"),
			Cursor.ReadBytes(Buffer, TNumericLimits<int64>::Max()));
		TestEqual(TEXT("refused hostile bulk read left the cursor alone"), Cursor.Tell(), static_cast<int64>(0));

		// A zero-length read is legal and must not move the cursor.
		TestTrue(TEXT("a zero-length bulk read succeeds"), Cursor.ReadBytes(Buffer, 0));
		TestEqual(TEXT("a zero-length bulk read did not move the cursor"), Cursor.Tell(), static_cast<int64>(0));
	}

	/* --------------------------------------------------------------------- */
	/* ReadIsAllZero - the reserved-field check                                */
	/* --------------------------------------------------------------------- */
	//
	// Format rule: a non-zero reserved field means the file uses a construct
	// this reader does not implement, and must be rejected rather than ignored.
	// The distinction that matters is between the RETURN value (did the read
	// happen) and the OUT value (were the bytes zero) - a reader that conflates
	// them accepts a truncated file as "all zero".
	{
		const uint8 Zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		const uint8 LastNonZero[8] = { 0, 0, 0, 0, 0, 0, 0, 0x01 };
		const uint8 FirstNonZero[8] = { 0x01, 0, 0, 0, 0, 0, 0, 0 };

		bool bAllZero = false;

		FByteCursor ZeroCursor(TArrayView<const uint8>(Zeros, 8));
		TestTrue(TEXT("all-zero read succeeds"), ZeroCursor.ReadIsAllZero(8, bAllZero));
		TestTrue(TEXT("all-zero reports zero"), bAllZero);
		TestEqual(TEXT("all-zero consumed the field"), ZeroCursor.Tell(), static_cast<int64>(8));

		// The last byte is the one a loop with an off-by-one never inspects.
		FByteCursor LastCursor(TArrayView<const uint8>(LastNonZero, 8));
		bAllZero = true;
		TestTrue(TEXT("read of a field with a non-zero LAST byte succeeds"),
			LastCursor.ReadIsAllZero(8, bAllZero));
		TestFalse(TEXT("a non-zero last byte is detected"), bAllZero);
		TestEqual(TEXT("a rejected reserved field is still consumed"),
			LastCursor.Tell(), static_cast<int64>(8));

		// The first byte exercises the early-out path.
		FByteCursor FirstCursor(TArrayView<const uint8>(FirstNonZero, 8));
		bAllZero = true;
		TestTrue(TEXT("read of a field with a non-zero FIRST byte succeeds"),
			FirstCursor.ReadIsAllZero(8, bAllZero));
		TestFalse(TEXT("a non-zero first byte is detected"), bAllZero);
		TestEqual(TEXT("an early-out still consumes the whole field"),
			FirstCursor.Tell(), static_cast<int64>(8));

		// Truncated: the read must FAIL, which is different from reporting
		// "not all zero". A caller distinguishes "this file is short" from
		// "this file uses a reserved feature" by exactly this return value.
		FByteCursor ShortCursor(TArrayView<const uint8>(Zeros, 4));
		bAllZero = true;
		TestFalse(TEXT("a reserved field past the end is refused"),
			ShortCursor.ReadIsAllZero(8, bAllZero));
		TestEqual(TEXT("a refused reserved read left the cursor alone"),
			ShortCursor.Tell(), static_cast<int64>(0));

		FByteCursor HostileCursor(TArrayView<const uint8>(Zeros, 8));
		TestFalse(TEXT("a reserved field of INT64_MAX is refused"),
			HostileCursor.ReadIsAllZero(TNumericLimits<int64>::Max(), bAllZero));
		TestFalse(TEXT("a reserved field of a negative count is refused"),
			HostileCursor.ReadIsAllZero(-1, bAllZero));
		TestEqual(TEXT("refused hostile reserved reads left the cursor alone"),
			HostileCursor.Tell(), static_cast<int64>(0));
	}

	/* --------------------------------------------------------------------- */
	/* Floats: bit patterns survive, including NaN payloads                    */
	/* --------------------------------------------------------------------- */
	//
	// Format rule 1.7: NaN is legal data and MUST be preserved exactly. That is
	// stronger than "is a NaN" - a signalling NaN quieted in transit, or a
	// payload dropped, changes the bits while still satisfying IsNaN. So these
	// compare the BITS, not the values. (Comparing values would be useless
	// anyway: NaN != NaN.)
	{
		// A signalling NaN with a distinctive payload. Chosen over a plain
		// quiet NaN because sNaN is what an x87-style load-and-store quiets,
		// and the payload nibbles are asymmetric so a byte swap is visible.
		const uint32 SignallingNaNBits = 0x7F812345u;
		const uint64 DoubleNaNBits     = 0x7FF0000123456789ull;

		uint8 FloatBytes[4];
		for (int32 Index = 0; Index < 4; ++Index)
		{
			FloatBytes[Index] = static_cast<uint8>((SignallingNaNBits >> (Index * 8)) & 0xFFu);
		}

		float ReadValue = 0.0f;
		FByteCursor FloatCursor(TArrayView<const uint8>(FloatBytes, 4));
		TestTrue(TEXT("float reads"), FloatCursor.ReadFloat(ReadValue));

		uint32 RoundTripped = 0;
		FMemory::Memcpy(&RoundTripped, &ReadValue, sizeof(RoundTripped));
		TestEqual(TEXT("a signalling NaN payload survives ReadFloat bit-exactly"),
			RoundTripped, SignallingNaNBits);
		TestTrue(TEXT("and it is still a NaN"), FMath::IsNaN(ReadValue));

		uint8 DoubleBytes[8];
		for (int32 Index = 0; Index < 8; ++Index)
		{
			DoubleBytes[Index] = static_cast<uint8>((DoubleNaNBits >> (Index * 8)) & 0xFFu);
		}

		double ReadDoubleValue = 0.0;
		FByteCursor DoubleCursor(TArrayView<const uint8>(DoubleBytes, 8));
		TestTrue(TEXT("double reads"), DoubleCursor.ReadDouble(ReadDoubleValue));

		uint64 DoubleRoundTripped = 0;
		FMemory::Memcpy(&DoubleRoundTripped, &ReadDoubleValue, sizeof(DoubleRoundTripped));
		TestEqual(TEXT("a NaN payload survives ReadDouble bit-exactly"),
			DoubleRoundTripped, DoubleNaNBits);
		TestTrue(TEXT("and it is still a NaN"), FMath::IsNaN(ReadDoubleValue));

		// Negative zero: distinguishable from +0.0 only in its bits, so it
		// catches an implementation that round-trips through a comparison or
		// normalizes on the way through. -0.0 == 0.0 is true, so this assertion
		// has to be on the bit pattern to mean anything.
		const uint8 NegativeZeroBytes[4] = { 0x00, 0x00, 0x00, 0x80 };
		float NegativeZero = 1.0f;
		FByteCursor NegZeroCursor(TArrayView<const uint8>(NegativeZeroBytes, 4));
		TestTrue(TEXT("negative zero reads"), NegZeroCursor.ReadFloat(NegativeZero));
		uint32 NegativeZeroBits = 0;
		FMemory::Memcpy(&NegativeZeroBits, &NegativeZero, sizeof(NegativeZeroBits));
		TestEqual(TEXT("negative zero keeps its sign bit"), NegativeZeroBits, 0x80000000u);
		TestTrue(TEXT("negative zero still compares equal to positive zero"),
			NegativeZero == 0.0f);

		// The same case for ReadDouble, and it is not redundant. Every other
		// double fixture here is 0x7FF0000123456789 -- a NaN whose sign bit is
		// already CLEAR. On that input "clear the sign bit" is the identity
		// map, so no assertion over it can distinguish the two, exactly the way
		// a one-component array makes an index transposition unobservable.
		// A mutation run scored the float sign bit killed and the double sign
		// bit SURVIVED, and the asymmetry was in the fixtures, not the code.
		const uint8 NegativeZeroDoubleBytes[8] = { 0, 0, 0, 0, 0, 0, 0, 0x80 };
		double NegativeZeroDouble = 1.0;
		FByteCursor NegZeroDoubleCursor(TArrayView<const uint8>(NegativeZeroDoubleBytes, 8));
		TestTrue(TEXT("negative zero double reads"),
			NegZeroDoubleCursor.ReadDouble(NegativeZeroDouble));
		uint64 NegativeZeroDoubleBits = 0;
		FMemory::Memcpy(&NegativeZeroDoubleBits, &NegativeZeroDouble,
			sizeof(NegativeZeroDoubleBits));
		TestEqual(TEXT("negative zero double keeps its sign bit"),
			NegativeZeroDoubleBits, 0x8000000000000000ull);
		TestTrue(TEXT("negative zero double still compares equal to positive zero"),
			NegativeZeroDouble == 0.0);

		// An ordinary finite value, so the float path is not tested exclusively
		// on exotic bit patterns. 1.0f is 0x3F800000.
		const uint8 OneBytes[4] = { 0x00, 0x00, 0x80, 0x3F };
		float One = 0.0f;
		FByteCursor OneCursor(TArrayView<const uint8>(OneBytes, 4));
		TestTrue(TEXT("an ordinary float reads"), OneCursor.ReadFloat(One));
		TestEqual(TEXT("an ordinary float has its expected value"), One, 1.0f);

		// Truncated float and double reads must refuse and not move.
		float Unused = 0.0f;
		FByteCursor ShortFloat(TArrayView<const uint8>(Fixture, 3));
		TestFalse(TEXT("a float with 3 bytes available is refused"), ShortFloat.ReadFloat(Unused));
		TestEqual(TEXT("a refused float left the cursor alone"), ShortFloat.Tell(), static_cast<int64>(0));

		double UnusedDouble = 0.0;
		FByteCursor ShortDouble(TArrayView<const uint8>(Fixture, 7));
		TestFalse(TEXT("a double with 7 bytes available is refused"), ShortDouble.ReadDouble(UnusedDouble));
		TestEqual(TEXT("a refused double left the cursor alone"), ShortDouble.Tell(), static_cast<int64>(0));
	}

	/* --------------------------------------------------------------------- */
	/* Degenerate construction                                                 */
	/* --------------------------------------------------------------------- */
	//
	// A default-constructed cursor has a null pointer. Nothing may dereference
	// it, and every accessor must report an empty span rather than a negative
	// remaining count - a negative Remaining() compared against a read width
	// would let a read through.
	{
		FByteCursor Empty;
		TestEqual(TEXT("a default cursor is empty"), Empty.Num(), static_cast<int64>(0));
		TestEqual(TEXT("a default cursor is at 0"), Empty.Tell(), static_cast<int64>(0));
		TestEqual(TEXT("a default cursor has nothing remaining"), Empty.Remaining(), static_cast<int64>(0));

		uint8 U8 = 0;
		TestFalse(TEXT("a default cursor refuses a read"), Empty.ReadUInt8(U8));
		TestFalse(TEXT("a default cursor refuses CanRead of 1"), Empty.CanRead(0, 1));
		TestTrue(TEXT("a default cursor allows a zero-length read at 0"), Empty.CanRead(0, 0));
		TestTrue(TEXT("a default cursor can seek to 0"), Empty.Seek(0));
		TestFalse(TEXT("a default cursor cannot seek to 1"), Empty.Seek(1));

		// A negative size passed to the pointer constructor is clamped to 0
		// rather than stored. Stored negative, Remaining() would go positive
		// after a seek and admit a read.
		FByteCursor Negative(Fixture, -1);
		TestEqual(TEXT("a negative size is clamped to zero"), Negative.Num(), static_cast<int64>(0));
		TestEqual(TEXT("a negative-size cursor has nothing remaining"),
			Negative.Remaining(), static_cast<int64>(0));
		TestFalse(TEXT("a negative-size cursor refuses a read"), Negative.ReadUInt8(U8));
		TestFalse(TEXT("a negative-size cursor refuses CanRead of 1"), Negative.CanRead(0, 1));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
