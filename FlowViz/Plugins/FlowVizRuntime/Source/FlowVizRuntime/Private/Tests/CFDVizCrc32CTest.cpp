// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizCrc32C.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * CRC-32C conformance.
 *
 * These vectors are the contract between this reader and the Python writer in
 * Tools/cfdviz. If this test fails, the two implementations disagree and every
 * file written by one is unreadable by the other - so it is a build-blocking
 * failure, not a cosmetic one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizCrc32CTest,
	"FlowViz.CFDViz.Crc32C",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizCrc32CTest::RunTest(const FString& Parameters)
{
	using namespace CFDViz;

	// The canonical CRC-32C check value. Asserted identically by the Python
	// reference implementation's import-time self_check().
	TestEqual(TEXT("check value for \"123456789\""),
		Crc32C::Compute("123456789", 9), Crc32C::CheckValue);

	TestTrue(TEXT("SelfCheck() passes"), Crc32C::SelfCheck());

	// Empty input returns the seed unchanged, so an empty payload contributes
	// nothing to a streaming CRC.
	TestEqual(TEXT("empty buffer with default seed"), Crc32C::Compute(nullptr, 0), 0u);
	TestEqual(TEXT("empty buffer preserves running CRC"),
		Crc32C::Compute(nullptr, 0, 0xDEADBEEFu), 0xDEADBEEFu);

	// Streaming in chunks must equal the one-shot result. Brick payloads are
	// checksummed incrementally while being read, so this property is load-bearing.
	{
		const ANSICHAR* Full = "123456789";
		const uint32 OneShot = Crc32C::Compute(Full, 9);

		uint32 Streamed = Crc32C::Compute(Full, 4);          // "1234"
		Streamed = Crc32C::Compute(Full + 4, 5, Streamed);   // "56789"
		TestEqual(TEXT("chunked equals one-shot (4+5)"), Streamed, OneShot);

		// Byte-at-a-time, the degenerate case.
		uint32 PerByte = 0;
		for (int32 Index = 0; Index < 9; ++Index)
		{
			PerByte = Crc32C::Compute(Full + Index, 1, PerByte);
		}
		TestEqual(TEXT("byte-at-a-time equals one-shot"), PerByte, OneShot);
	}

	// A single-bit change must change the CRC - the point of the checksum.
	{
		const uint8 Original[] = { 0x00, 0x01, 0x02, 0x03 };
		const uint8 Flipped[]  = { 0x00, 0x01, 0x02, 0x02 };
		TestNotEqual(TEXT("single-bit corruption is detected"),
			Crc32C::Compute(Original, 4), Crc32C::Compute(Flipped, 4));
	}

	// This is NOT zlib's CRC-32. Guards against someone "fixing" the polynomial.
	// zlib's crc32(b"123456789") is 0xCBF43926.
	TestNotEqual(TEXT("distinct from zlib CRC-32"), Crc32C::CheckValue, 0xCBF43926u);

	// TArrayView overload agrees with the pointer overload.
	{
		const TArray<uint8> Bytes = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
		TestEqual(TEXT("TArrayView overload matches"),
			Crc32C::Compute(TArrayView<const uint8>(Bytes)), Crc32C::CheckValue);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
