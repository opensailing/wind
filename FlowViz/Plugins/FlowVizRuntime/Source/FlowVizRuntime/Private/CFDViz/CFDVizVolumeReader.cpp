// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizVolumeReader.h"

#include "CFDViz/CFDVizCrc32C.h"
#include "CFDViz/CFDVizPayload.h"
#include "CFDVizByteCursor.h"

namespace
{
namespace CvfLayout
{
	/* ---------------------------------------------------------------------- */
	/* On-disk offsets - format section 4.1 (header) and 4.3 (directory entry)  */
	/*                                                                          */
	/* NAMESPACED DELIBERATELY. This module builds as a UE unity build: every    */
	/* reader .cpp is concatenated into one translation unit, so an anonymous    */
	/* namespace is NOT per-file here. CVF's `OffsetFlags` is 20 and CVM's is    */
	/* 16; sharing the enclosing scope silently gave one reader the other's      */
	/* layout, and whichever file the generated Module.*.cpp included first won. */
	/* The static_asserts below caught it, which is the only reason it was not   */
	/* a runtime misparse. Every new reader gets its own layout namespace.       */
	/*                                                                          */
	/* These constants ARE the format. They are named and used by the parser    */
	/* below so that a failure can report the byte offset of the field that     */
	/* actually failed, which is what section 10 requires of an error message.  */
	/* ---------------------------------------------------------------------- */
	constexpr int64 OffsetMagic = 0;
	constexpr int64 OffsetHeaderBytes = 8;
	constexpr int64 OffsetMajorVersion = 12;
	constexpr int64 OffsetMinorVersion = 14;
	constexpr int64 OffsetEndianMarker = 16;
	constexpr int64 OffsetFlags = 20;
	constexpr int64 OffsetFrameIndex = 24;
	constexpr int64 OffsetFieldNumericId = 28;
	constexpr int64 OffsetSimulationTime = 32;
	constexpr int64 OffsetDimensions = 40;
	constexpr int64 OffsetBrickSize = 52;
	constexpr int64 OffsetComponentCount = 58;
	constexpr int64 OffsetDataType = 59;
	constexpr int64 OffsetAssociation = 60;
	constexpr int64 OffsetCodec = 61;
	constexpr int64 OffsetReservedShort = 62;
	constexpr int64 OffsetBrickCount = 64;
	constexpr int64 OffsetDirectoryOffset = 72;
	constexpr int64 OffsetPayloadOffset = 80;
	constexpr int64 OffsetBackgroundValue = 88;
	constexpr int64 OffsetHeaderCrc = CFDViz::CvfHeaderCrcOffset; // 104
	constexpr int64 OffsetReservedTail = 108;
	constexpr int64 ReservedTailBytes = 20;

	constexpr int64 EntryOffsetBrickIndex = 0;
	constexpr int64 EntryOffsetValidSize = 12;
	constexpr int64 EntryOffsetFlags = 18;
	constexpr int64 EntryOffsetPayloadOffset = 20;
	constexpr int64 EntryOffsetCompressedBytes = 28;
	constexpr int64 EntryOffsetUncompressedBytes = 32;
	constexpr int64 EntryOffsetComponentMin = 36;
	constexpr int64 EntryOffsetComponentMax = 52;
	constexpr int64 EntryOffsetPayloadCrc = 68;
	constexpr int64 EntryOffsetReserved = 72;
	constexpr int64 EntryReservedBytes = 8;

	/**
	 * Directory entries read per source request.
	 *
	 * 256 entries is 20 KiB. Streaming rather than slurping matters because
	 * brickCount is attacker-controlled until it has been bounds-checked, and even
	 * once it is legitimate a large sparse volume can carry hundreds of thousands
	 * of entries; a single transient buffer for all of them would be a
	 * multi-megabyte allocation nobody asked for.
	 */
	constexpr int32 DirectoryEntriesPerChunk = 256;

	/*
	 * The offsets above are only useful if they are exact: each one is used twice,
	 * once to decode a field and once to report where a malformed file went wrong.
	 * These assertions pin the whole layout, so a typo in either role is a build
	 * error rather than an error message that points at the wrong field.
	 */
	static_assert(OffsetMagic == 0 && OffsetHeaderBytes == 8, "CVF header: magic[8] then headerBytes.");
	static_assert(OffsetMajorVersion == 12 && OffsetMinorVersion == 14 && OffsetEndianMarker == 16, "CVF header: version pair then the endian marker.");
	static_assert(OffsetFlags == 20 && OffsetFrameIndex == 24 && OffsetFieldNumericId == 28, "CVF header: flags, frameIndex, fieldNumericId.");
	static_assert(OffsetSimulationTime == 32 && OffsetDimensions == 40, "CVF header: simulationTime is a float64 before the dimensions.");
	static_assert(OffsetDimensions + 3 * 4 == OffsetBrickSize, "CVF header: dimensions are uint32[3].");
	static_assert(OffsetBrickSize + 3 * 2 == OffsetComponentCount, "CVF header: brickSize is uint16[3].");
	static_assert(OffsetComponentCount + 1 == OffsetDataType && OffsetDataType + 1 == OffsetAssociation && OffsetAssociation + 1 == OffsetCodec, "CVF header: four uint8 enum bytes in sequence.");
	static_assert(OffsetCodec + 1 == OffsetReservedShort && OffsetReservedShort + 2 == OffsetBrickCount, "CVF header: two reserved bytes before brickCount.");
	static_assert(OffsetBrickCount + 8 == OffsetDirectoryOffset && OffsetDirectoryOffset + 8 == OffsetPayloadOffset, "CVF header: three uint64 offsets in sequence.");
	static_assert(OffsetPayloadOffset + 8 == OffsetBackgroundValue, "CVF header: backgroundValue follows payloadOffset.");
	static_assert(OffsetBackgroundValue + CFDViz::MaxCvfComponentCount * 4 == OffsetHeaderCrc, "CVF header: backgroundValue is float32[4] immediately before headerCrc32c.");
	static_assert(OffsetHeaderCrc + CFDViz::HeaderCrcFieldBytes == OffsetReservedTail, "CVF header: the reserved tail starts right after the CRC field.");
	static_assert(OffsetReservedTail + ReservedTailBytes == CFDViz::CvfHeaderBytes, "The CVF header must be exactly 128 bytes with no gap after the reserved tail.");
	static_assert(CFDViz::HeaderCrcFieldBytes == 4, "The header CRC field is a uint32.");

	static_assert(EntryOffsetBrickIndex == 0 && EntryOffsetBrickIndex + 3 * 4 == EntryOffsetValidSize, "CVF entry: brickIndex is uint32[3].");
	static_assert(EntryOffsetValidSize + 3 * 2 == EntryOffsetFlags, "CVF entry: validSize is uint16[3] before flags.");
	static_assert(EntryOffsetFlags + 2 == EntryOffsetPayloadOffset, "CVF entry: flags is a uint16 before the payload offset.");
	static_assert(EntryOffsetPayloadOffset + 8 == EntryOffsetCompressedBytes, "CVF entry: the payload offset is a uint64.");
	static_assert(EntryOffsetCompressedBytes + 4 == EntryOffsetUncompressedBytes, "CVF entry: compressedBytes is a uint32.");
	static_assert(EntryOffsetUncompressedBytes + 4 == EntryOffsetComponentMin, "CVF entry: uncompressedBytes is a uint32.");
	static_assert(EntryOffsetComponentMin + CFDViz::MaxCvfComponentCount * 4 == EntryOffsetComponentMax, "CVF entry: componentMin is float32[4].");
	static_assert(EntryOffsetComponentMax + CFDViz::MaxCvfComponentCount * 4 == EntryOffsetPayloadCrc, "CVF entry: componentMax is float32[4].");
	static_assert(EntryOffsetPayloadCrc + 4 == EntryOffsetReserved, "CVF entry: payloadCrc32c is a uint32.");
	static_assert(EntryOffsetReserved + EntryReservedBytes == CFDViz::CvfDirectoryEntryBytes, "A CVF directory entry must be exactly 80 bytes with no gap after the reserved block.");
}	// namespace CvfLayout

	// Unqualified below, but now unambiguously CVF's: a sibling reader's
	// same-named constant no longer resolves here.
	using namespace CvfLayout;

	/** Every offset in a header error message points at a field, so the offsets are useful only if they are exact. */
	FString FormatBrickCoordinate(const FIntVector& Coordinate)
	{
		return FString::Printf(TEXT("(%d, %d, %d)"), Coordinate.X, Coordinate.Y, Coordinate.Z);
	}

	/**
	 * IEEE binary32 bit pattern to IEEE binary16 bit pattern, round to nearest,
	 * ties to even, PRESERVING NaN payload bits.
	 *
	 * Written out rather than delegated to FPlatformMath::StoreHalf because
	 * StoreHalf canonicalises every NaN to 0x7E00. The Python reference narrows
	 * backgroundValue with numpy's float32-to-float16 cast, which keeps the
	 * payload; if the two disagreed, the background bytes an absent brick
	 * evaluates to would differ between the implementations, and the section 9
	 * known_values bridge compares bit patterns. Format rule 1.7 says NaN is
	 * preserved bit-exactly, and "the exponent happened to be all ones" is not a
	 * licence to rewrite the significand.
	 *
	 * Verified against numpy's cast over 600k patterns including NaN payloads,
	 * subnormals, ties, and both overflow directions.
	 */
	uint16 Float32BitsToFloat16Bits(uint32 Word)
	{
		const uint32 Sign = (Word >> 16) & 0x8000u;
		const int32 RawExponent = static_cast<int32>((Word >> 23) & 0xFFu);
		const uint32 Mantissa = Word & 0x7FFFFFu;

		if (RawExponent == 0xFF)
		{
			if (Mantissa == 0)
			{
				return static_cast<uint16>(Sign | 0x7C00u); // +/-infinity
			}
			// Keep the top 13 significand bits. If they are all zero the value
			// would read back as infinity, so force the low bit: a NaN must stay a
			// NaN even when its payload does not survive the narrowing.
			uint32 HalfMantissa = Mantissa >> 13;
			if (HalfMantissa == 0)
			{
				HalfMantissa = 1;
			}
			return static_cast<uint16>(Sign | 0x7C00u | HalfMantissa);
		}

		const int32 Exponent = RawExponent - 127;
		if (Exponent > 15)
		{
			// Overflows binary16's range. This is only ever reached for a
			// backgroundValue the writer chose, and GetBackgroundVoxelBytes reports
			// it rather than storing the resulting infinity - silently turning 1e30
			// into inf is the clamping format rule 1.6 forbids.
			return static_cast<uint16>(Sign | 0x7C00u);
		}

		if (Exponent >= -14)
		{
			// Representable as a binary16 normal.
			int32 HalfExponent = Exponent + 15;
			uint32 HalfMantissa = Mantissa >> 13;
			const uint32 Discarded = Mantissa & 0x1FFFu;
			// Round half to even: round up on more than half, or on exactly half
			// when the retained bit is odd.
			if (Discarded > 0x1000u || (Discarded == 0x1000u && (HalfMantissa & 1u) != 0u))
			{
				++HalfMantissa;
				if (HalfMantissa == 0x400u)
				{
					// The significand carried out; renormalise.
					HalfMantissa = 0;
					++HalfExponent;
					if (HalfExponent >= 0x1F)
					{
						return static_cast<uint16>(Sign | 0x7C00u);
					}
				}
			}
			return static_cast<uint16>(Sign | (static_cast<uint32>(HalfExponent) << 10) | HalfMantissa);
		}

		if (Exponent < -25)
		{
			// Below half of the smallest subnormal; rounds to a signed zero.
			return static_cast<uint16>(Sign);
		}

		// Subnormal in binary16. Restore the implicit leading 1 and shift down to
		// the fixed 2^-24 grid, rounding half to even as above. Shift lands in
		// [14, 24] for the exponent range that reaches here, so neither the shift
		// nor the (1 << (Shift - 1)) tie constant can be undefined.
		const uint32 Full = Mantissa | 0x800000u;
		const int32 Shift = -1 - Exponent;
		uint32 HalfMantissa = Full >> Shift;
		const uint32 Discarded = Full & ((1u << Shift) - 1u);
		const uint32 Tie = 1u << (Shift - 1);
		if (Discarded > Tie || (Discarded == Tie && (HalfMantissa & 1u) != 0u))
		{
			// Carrying into bit 10 produces the smallest normal, which is exactly
			// the right answer - no special case needed.
			++HalfMantissa;
		}
		return static_cast<uint16>(Sign | HalfMantissa);
	}

	/** Reinterpret a float's bits without undefined behaviour, so NaN payloads survive. */
	uint32 FloatToBits(float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return Bits;
	}

	/** Store a 16-bit value little-endian, independent of host byte order. */
	void StoreUInt16LE(uint8* Destination, uint16 Value)
	{
		Destination[0] = static_cast<uint8>(Value & 0xFFu);
		Destination[1] = static_cast<uint8>((Value >> 8) & 0xFFu);
	}

	/** Store a 32-bit value little-endian, independent of host byte order. */
	void StoreUInt32LE(uint8* Destination, uint32 Value)
	{
		Destination[0] = static_cast<uint8>(Value & 0xFFu);
		Destination[1] = static_cast<uint8>((Value >> 8) & 0xFFu);
		Destination[2] = static_cast<uint8>((Value >> 16) & 0xFFu);
		Destination[3] = static_cast<uint8>((Value >> 24) & 0xFFu);
	}

	/**
	 * Tile Pattern across TotalBytes. TotalBytes is always a whole number of
	 * patterns - it is voxelCount * voxelBytes and the pattern is one voxel.
	 */
	void FillWithPattern(uint8* Destination, int64 TotalBytes, const uint8* Pattern, int64 PatternBytes)
	{
		if (Destination == nullptr || TotalBytes <= 0 || PatternBytes <= 0)
		{
			return;
		}

		// A background of zero - overwhelmingly the common case - makes every byte
		// of the pattern identical, and Memset beats the doubling copy below.
		bool bUniform = true;
		for (int64 Index = 1; Index < PatternBytes; ++Index)
		{
			if (Pattern[Index] != Pattern[0])
			{
				bUniform = false;
				break;
			}
		}
		if (bUniform)
		{
			FMemory::Memset(Destination, Pattern[0], static_cast<SIZE_T>(TotalBytes));
			return;
		}

		FMemory::Memcpy(Destination, Pattern, static_cast<SIZE_T>(FMath::Min(PatternBytes, TotalBytes)));
		int64 Filled = FMath::Min(PatternBytes, TotalBytes);
		while (Filled < TotalBytes)
		{
			const int64 Chunk = FMath::Min(Filled, TotalBytes - Filled);
			FMemory::Memcpy(Destination + Filled, Destination, static_cast<SIZE_T>(Chunk));
			Filled += Chunk;
		}
	}

	/** ceil(A / B) for positive B, without floating point. */
	int64 CeilDiv(int64 A, int64 B)
	{
		return (B > 0) ? ((A + B - 1) / B) : 0;
	}
}

/* -------------------------------------------------------------------------- */
/* FCFDVizVolumeHeader                                                          */
/* -------------------------------------------------------------------------- */

uint32 FCFDVizVolumeHeader::ComputeHeaderCrc(TArrayView<const uint8> Bytes)
{
	if (Bytes.Num() < CFDViz::CvfHeaderBytes)
	{
		return 0;
	}

	const uint8* const Data = Bytes.GetData();

	// Section 4.1: CRC-32C over all 128 bytes with [104, 108) treated as zero.
	// Fed in three chunks rather than by copying the header and blanking the
	// field, so no scratch buffer is needed. The zero bytes take PART in the
	// checksum - they are not skipped. Skipping them would produce a different,
	// self-consistent answer that no other implementation would agree with.
	const uint8 ZeroedCrcField[4] = { 0, 0, 0, 0 };

	uint32 Crc = CFDViz::Crc32C::Compute(Data, OffsetHeaderCrc);
	Crc = CFDViz::Crc32C::Compute(ZeroedCrcField, CFDViz::HeaderCrcFieldBytes, Crc);
	Crc = CFDViz::Crc32C::Compute(
		Data + OffsetHeaderCrc + CFDViz::HeaderCrcFieldBytes,
		CFDViz::CvfHeaderBytes - OffsetHeaderCrc - CFDViz::HeaderCrcFieldBytes,
		Crc);
	return Crc;
}

FCFDVizResult FCFDVizVolumeHeader::Parse(TArrayView<const uint8> Bytes, FCFDVizVolumeHeader& Out, bool bVerifyCrc)
{
	if (Bytes.Num() < CFDViz::CvfHeaderBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::FileTooSmall,
			FString::Printf(
				TEXT("file is %d bytes, shorter than the %lld-byte CVF header"),
				Bytes.Num(), static_cast<long long>(CFDViz::CvfHeaderBytes)),
			FString(),
			OffsetMagic);
	}

	// Every field is pulled through the cursor, byte by byte and little-endian.
	// Section 4 forbids memcpy'ing a native struct over these bytes: C++ padding
	// is compiler-dependent, so a struct that lines up under one toolchain
	// silently misparses under another instead of failing.
	CFDViz::FByteCursor Cursor(Bytes.GetData(), CFDViz::CvfHeaderBytes);

	// The rejection order below matches the Python reference exactly, so a file
	// that is wrong in several ways at once is diagnosed identically by both
	// readers rather than each blaming a different field.
	ANSICHAR Magic[8] = {};
	Cursor.ReadBytes(Magic, sizeof(Magic));
	if (FMemory::Memcmp(Magic, CFDViz::CvfMagic, sizeof(CFDViz::CvfMagic)) != 0)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::BadMagic,
			TEXT("bad magic; expected 'CFDVOL1'. This is not a CVF volume file."),
			FString(),
			OffsetMagic);
	}

	uint32 DeclaredHeaderBytes = 0;
	uint16 MajorVersionValue = 0;
	uint16 MinorVersionValue = 0;
	uint32 EndianMarkerValue = 0;
	uint32 FlagsValue = 0;
	Cursor.ReadUInt32(DeclaredHeaderBytes);
	Cursor.ReadUInt16(MajorVersionValue);
	Cursor.ReadUInt16(MinorVersionValue);
	Cursor.ReadUInt32(EndianMarkerValue);
	Cursor.ReadUInt32(FlagsValue);

	if (EndianMarkerValue != CFDViz::EndianMarker)
	{
		// Format rule 1.1: reject foreign byte order, never byte-swap it. A
		// swapped read would succeed and produce a field that is subtly wrong
		// everywhere, which is far worse than a refusal.
		return FCFDVizResult::Fail(
			ECFDVizError::UnsupportedEndianness,
			FString::Printf(
				TEXT("byte order not supported: endianMarker is 0x%08X, expected 0x%08X. CFDViz is little-endian only."),
				EndianMarkerValue, CFDViz::EndianMarker),
			FString(),
			OffsetEndianMarker);
	}

	if (!CFDViz::IsSupportedMajorVersion(MajorVersionValue))
	{
		// A newer minor is accepted (format rule 1.4); a different major is not.
		return FCFDVizResult::Fail(
			ECFDVizError::UnsupportedVersion,
			FString::Printf(
				TEXT("unsupported major version %u; this reader implements CVF %u.x"),
				static_cast<uint32>(MajorVersionValue), static_cast<uint32>(CFDViz::SupportedMajorVersion)),
			FString(),
			OffsetMajorVersion);
	}

	if (static_cast<int64>(DeclaredHeaderBytes) != CFDViz::CvfHeaderBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("headerBytes is %u, expected %lld"),
				DeclaredHeaderBytes, static_cast<long long>(CFDViz::CvfHeaderBytes)),
			FString(),
			OffsetHeaderBytes);
	}

	const uint32 UnknownFlags = FlagsValue & ~CFDViz::Cvf::KnownFlags;
	if (UnknownFlags != 0)
	{
		// Format rule 1.4 accepts a newer minor version only when every construct
		// in the file is understood. An unknown header bit could change how
		// payloads decode; reading on "as if the bit were clear" would silently
		// produce wrong data, so the file is refused instead.
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("unknown flag bits 0x%08X set; this file uses a CVF feature this reader does not implement (flags=0x%08X)"),
				UnknownFlags, FlagsValue),
			FString(),
			OffsetFlags);
	}

	uint32 FrameIndexValue = 0;
	uint32 FieldNumericIdValue = 0;
	double SimulationTimeValue = 0.0;
	Cursor.ReadUInt32(FrameIndexValue);
	Cursor.ReadUInt32(FieldNumericIdValue);
	Cursor.ReadDouble(SimulationTimeValue);

	uint32 DimX = 0;
	uint32 DimY = 0;
	uint32 DimZ = 0;
	Cursor.ReadUInt32(DimX);
	Cursor.ReadUInt32(DimY);
	Cursor.ReadUInt32(DimZ);

	uint16 BrickX = 0;
	uint16 BrickY = 0;
	uint16 BrickZ = 0;
	Cursor.ReadUInt16(BrickX);
	Cursor.ReadUInt16(BrickY);
	Cursor.ReadUInt16(BrickZ);

	uint8 ComponentCountValue = 0;
	uint8 DataTypeCode = 0;
	uint8 AssociationCode = 0;
	uint8 CodecCode = 0;
	Cursor.ReadUInt8(ComponentCountValue);
	Cursor.ReadUInt8(DataTypeCode);
	Cursor.ReadUInt8(AssociationCode);
	Cursor.ReadUInt8(CodecCode);

	bool bReservedShortZero = false;
	Cursor.ReadIsAllZero(2, bReservedShortZero);
	if (!bReservedShortZero)
	{
		// A reserved field the spec pins to zero is not an optional feature: a
		// non-zero value means the layout changed. Contrast `flags`, where an
		// unknown bit is also refused but for a different reason.
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			TEXT("reserved header bytes [62, 64) are not zero"),
			FString(),
			OffsetReservedShort);
	}

	uint64 BrickCountValue = 0;
	uint64 DirectoryOffsetValue = 0;
	uint64 PayloadOffsetValue = 0;
	Cursor.ReadUInt64(BrickCountValue);
	Cursor.ReadUInt64(DirectoryOffsetValue);
	Cursor.ReadUInt64(PayloadOffsetValue);

	float BackgroundValues[CFDViz::MaxCvfComponentCount] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (int32 Slot = 0; Slot < CFDViz::MaxCvfComponentCount; ++Slot)
	{
		// ReadFloat memcpy's from the assembled bits, so a NaN background arrives
		// with its payload intact (format rule 1.7).
		Cursor.ReadFloat(BackgroundValues[Slot]);
	}

	uint32 StoredCrc = 0;
	Cursor.ReadUInt32(StoredCrc);

	bool bReservedTailZero = false;
	Cursor.ReadIsAllZero(ReservedTailBytes, bReservedTailZero);

	// Individual Read* results are not checked above: the cursor spans exactly
	// CvfHeaderBytes, which the length check at the top guaranteed, and the
	// static_asserts pin the field widths to that total. This turns that
	// reasoning into a check. A short read leaves the cursor un-advanced, so a
	// field silently defaulting to zero shows up here as a position that is not
	// the full header - rather than as a header full of plausible zeros.
	if (Cursor.Tell() != CFDViz::CvfHeaderBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::FileTooSmall,
			FString::Printf(
				TEXT("CVF header parse consumed %lld of %lld bytes"),
				Cursor.Tell(), static_cast<long long>(CFDViz::CvfHeaderBytes)),
			FString(),
			Cursor.Tell());
	}

	if (!bReservedTailZero)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			TEXT("reserved header bytes [108, 128) are not zero"),
			FString(),
			OffsetReservedTail);
	}

	if (DimX < 1 || DimY < 1 || DimZ < 1)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("grid dimension (%u, %u, %u) has a component below 1; every dimension must be at least 1 (section 3.1)"),
				DimX, DimY, DimZ),
			FString(),
			OffsetDimensions);
	}
	if (static_cast<int64>(DimX) > CFDViz::MaxGridDimension
		|| static_cast<int64>(DimY) > CFDViz::MaxGridDimension
		|| static_cast<int64>(DimZ) > CFDViz::MaxGridDimension)
	{
		// The file may be perfectly legal; this build simply indexes with int32
		// and cannot address it. Saying so is more honest than overflowing an
		// extent and then bounds-checking against the wrapped value.
		return FCFDVizResult::Fail(
			ECFDVizError::AllocationTooLarge,
			FString::Printf(
				TEXT("grid dimension (%u, %u, %u) exceeds the %lld this reader can address per axis"),
				DimX, DimY, DimZ, static_cast<long long>(CFDViz::MaxGridDimension)),
			FString(),
			OffsetDimensions);
	}

	if (BrickX < 1 || BrickY < 1 || BrickZ < 1)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("brickSize (%u, %u, %u) has a component below 1; a brick must be at least one voxel on every axis"),
				static_cast<uint32>(BrickX), static_cast<uint32>(BrickY), static_cast<uint32>(BrickZ)),
			FString(),
			OffsetBrickSize);
	}

	if (ComponentCountValue < 1 || static_cast<int32>(ComponentCountValue) > CFDViz::MaxCvfComponentCount)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("componentCount %u is outside 1..%d; the header's float32[4] backgroundValue and the directory's float32[4] statistics cap a CVF at %d components"),
				static_cast<uint32>(ComponentCountValue), CFDViz::MaxCvfComponentCount, CFDViz::MaxCvfComponentCount),
			FString(),
			OffsetComponentCount);
	}

	ECFDVizDataType DataTypeValue = ECFDVizDataType::Float32;
	if (!TryDataTypeFromCode(DataTypeCode, DataTypeValue) || !IsDataTypeSupportedInCvf(DataTypeValue))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::UnsupportedDataType,
			FString::Printf(
				TEXT("unknown or unsupported dataType %u; CVF supports float16, float32 and uint8 (float64 field storage is not supported in 1.0, section 3.3)"),
				static_cast<uint32>(DataTypeCode)),
			FString(),
			OffsetDataType);
	}

	ECFDVizAssociation AssociationValue = ECFDVizAssociation::Cell;
	if (!TryAssociationFromCode(AssociationCode, AssociationValue))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::UnsupportedAssociation,
			FString::Printf(
				TEXT("unknown association %u; expected 0 (cell) or 1 (point). Associations such as mesh-vertex, mesh-element, integration-point, face and particle are reserved and rejected in 1.0 (section 3.2)."),
				static_cast<uint32>(AssociationCode)),
			FString(),
			OffsetAssociation);
	}

	ECFDVizCodec CodecValue = ECFDVizCodec::Zlib;
	const bool bKnownCodec = TryCodecFromCode(CodecCode, CodecValue);
	if (bKnownCodec && CodecValue == ECFDVizCodec::Zstd)
	{
		// Section 7 pins this user-facing wording to the character, and the Python
		// reference emits the identical string. Message carries it alone - no
		// prefix, no appended field context - because a caller may show it
		// verbatim. Never fall back to another codec: inflating zstd bytes as zlib
		// either fails obscurely or, worse, produces plausible data.
		return FCFDVizResult::Fail(
			ECFDVizError::UnsupportedCodec,
			FString(CFDViz::ZstdRejectionMessage));
	}
	if (!bKnownCodec)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::UnsupportedCodec,
			FString::Printf(
				TEXT("unknown codec ID %u; supported: none, lz4, zlib"),
				static_cast<uint32>(CodecCode)),
			FString(),
			OffsetCodec);
	}

	if (bVerifyCrc)
	{
		// Last, so that a file which is simply not a CVF reports *that* rather
		// than a checksum mismatch nobody can act on - but before any caller sees
		// a field, so no value is trusted until the header is proven intact.
		const uint32 ComputedCrc = ComputeHeaderCrc(Bytes);
		if (ComputedCrc != StoredCrc)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::HeaderCrcMismatch,
				FString::Printf(
					TEXT("header CRC-32C mismatch: stored 0x%08X, computed 0x%08X. The header is corrupt."),
					StoredCrc, ComputedCrc),
				FString(),
				OffsetHeaderCrc);
		}
	}

	FCFDVizVolumeHeader Parsed;
	Parsed.HeaderBytes = DeclaredHeaderBytes;
	Parsed.MajorVersion = MajorVersionValue;
	Parsed.MinorVersion = MinorVersionValue;
	Parsed.EndianMarker = EndianMarkerValue;
	Parsed.Flags = FlagsValue;
	Parsed.FrameIndex = FrameIndexValue;
	Parsed.FieldNumericId = FieldNumericIdValue;
	Parsed.SimulationTime = SimulationTimeValue;
	Parsed.Dimensions = FIntVector(static_cast<int32>(DimX), static_cast<int32>(DimY), static_cast<int32>(DimZ));
	Parsed.BrickSize = FIntVector(static_cast<int32>(BrickX), static_cast<int32>(BrickY), static_cast<int32>(BrickZ));
	Parsed.ComponentCount = static_cast<int32>(ComponentCountValue);
	Parsed.DataType = DataTypeValue;
	Parsed.Association = AssociationValue;
	Parsed.Codec = CodecValue;
	Parsed.BackgroundValue[0] = BackgroundValues[0];
	Parsed.BackgroundValue[1] = BackgroundValues[1];
	Parsed.BackgroundValue[2] = BackgroundValues[2];
	Parsed.BackgroundValue[3] = BackgroundValues[3];
	Parsed.HeaderCrc32C = StoredCrc;

	// brickCount, directoryOffset and payloadOffset are uint64 on disk and are
	// narrowed here. A value past int64 range cannot be represented, let alone
	// read, so it is refused rather than wrapped into a small positive number
	// that would sail through every later bounds check (format rule 1.5).
	if (BrickCountValue > static_cast<uint64>(MAX_int64))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(TEXT("brickCount %llu is not representable"), BrickCountValue),
			FString(),
			OffsetBrickCount);
	}
	if (DirectoryOffsetValue > static_cast<uint64>(MAX_int64))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(TEXT("directoryOffset %llu is not representable"), DirectoryOffsetValue),
			FString(),
			OffsetDirectoryOffset);
	}
	if (PayloadOffsetValue > static_cast<uint64>(MAX_int64))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(TEXT("payloadOffset %llu is not representable"), PayloadOffsetValue),
			FString(),
			OffsetPayloadOffset);
	}
	Parsed.BrickCount = static_cast<int64>(BrickCountValue);
	Parsed.DirectoryOffset = static_cast<int64>(DirectoryOffsetValue);
	Parsed.PayloadOffset = static_cast<int64>(PayloadOffsetValue);

	Out = Parsed;
	return FCFDVizResult::Ok();
}

FIntVector FCFDVizVolumeHeader::GetValueCounts() const
{
	if (Dimensions.X < 1 || Dimensions.Y < 1 || Dimensions.Z < 1)
	{
		// An unvalidated header yields a count no index can satisfy, rather than
		// a plausible wrong one.
		return FIntVector(0, 0, 0);
	}
	if (Association == ECFDVizAssociation::Point)
	{
		// Section 3.2: point values sit on cell corners, so there is one more per
		// axis. This +1 exists in exactly one place in this reader; getting it
		// wrong shifts the whole field by half a voxel, which renders plausibly.
		return FIntVector(Dimensions.X + 1, Dimensions.Y + 1, Dimensions.Z + 1);
	}
	return Dimensions;
}

int64 FCFDVizVolumeHeader::GetValueCount() const
{
	const FIntVector Counts = GetValueCounts();
	if (Counts.X < 1 || Counts.Y < 1 || Counts.Z < 1)
	{
		return INDEX_NONE;
	}

	int64 Total = 0;
	if (!CFDViz::TryMultiply(Counts.X, Counts.Y, Total) || !CFDViz::TryMultiply(Total, Counts.Z, Total))
	{
		return INDEX_NONE;
	}
	return Total;
}

int64 FCFDVizVolumeHeader::GetDenseVolumeBytes() const
{
	const int64 Values = GetValueCount();
	if (Values == INDEX_NONE)
	{
		return INDEX_NONE;
	}
	return CFDViz::ComputePayloadBytes(Values, ComponentCount, DataType);
}

FIntVector FCFDVizVolumeHeader::GetBrickCounts() const
{
	const FIntVector Counts = GetValueCounts();
	if (Counts.X < 1 || Counts.Y < 1 || Counts.Z < 1
		|| BrickSize.X < 1 || BrickSize.Y < 1 || BrickSize.Z < 1)
	{
		return FIntVector(0, 0, 0);
	}
	return FIntVector(
		static_cast<int32>(CeilDiv(Counts.X, BrickSize.X)),
		static_cast<int32>(CeilDiv(Counts.Y, BrickSize.Y)),
		static_cast<int32>(CeilDiv(Counts.Z, BrickSize.Z)));
}

int64 FCFDVizVolumeHeader::GetTotalBrickCount() const
{
	const FIntVector Counts = GetBrickCounts();
	if (Counts.X < 1 || Counts.Y < 1 || Counts.Z < 1)
	{
		return INDEX_NONE;
	}

	int64 Total = 0;
	if (!CFDViz::TryMultiply(Counts.X, Counts.Y, Total) || !CFDViz::TryMultiply(Total, Counts.Z, Total))
	{
		return INDEX_NONE;
	}
	return Total;
}

FIntVector FCFDVizVolumeHeader::GetValidSizeForBrick(const FIntVector& BrickCoordinate) const
{
	const FIntVector Grid = GetBrickCounts();
	if (BrickCoordinate.X < 0 || BrickCoordinate.Y < 0 || BrickCoordinate.Z < 0
		|| BrickCoordinate.X >= Grid.X || BrickCoordinate.Y >= Grid.Y || BrickCoordinate.Z >= Grid.Z)
	{
		return FIntVector(0, 0, 0);
	}

	// EDGE BRICKS ARE NOT PADDED (section 4.4.3). The last brick on an axis holds
	// exactly the remainder, so the size is clipped to what is left of the
	// extent. Treating it as a full brickSize cube reads past the decoded buffer
	// and shears every subsequent row, which looks like plausible turbulence.
	const FIntVector Counts = GetValueCounts();
	return FIntVector(
		FMath::Min(BrickSize.X, Counts.X - BrickCoordinate.X * BrickSize.X),
		FMath::Min(BrickSize.Y, Counts.Y - BrickCoordinate.Y * BrickSize.Y),
		FMath::Min(BrickSize.Z, Counts.Z - BrickCoordinate.Z * BrickSize.Z));
}

bool FCFDVizVolumeHeader::ContainsValue(int32 I, int32 J, int32 K) const
{
	const FIntVector Counts = GetValueCounts();
	return I >= 0 && J >= 0 && K >= 0 && I < Counts.X && J < Counts.Y && K < Counts.Z;
}

/* -------------------------------------------------------------------------- */
/* FCFDVizBrickEntry                                                            */
/* -------------------------------------------------------------------------- */

FCFDVizResult FCFDVizBrickEntry::Parse(TArrayView<const uint8> Bytes, FCFDVizBrickEntry& Out, int64 FileOffset)
{
	if (Bytes.Num() < CFDViz::CvfDirectoryEntryBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::FileTooSmall,
			FString::Printf(
				TEXT("brick directory entry is %d bytes, shorter than the %lld-byte entry"),
				Bytes.Num(), static_cast<long long>(CFDViz::CvfDirectoryEntryBytes)),
			FString(),
			FileOffset);
	}

	CFDViz::FByteCursor Cursor(Bytes.GetData(), CFDViz::CvfDirectoryEntryBytes);

	uint32 IndexX = 0;
	uint32 IndexY = 0;
	uint32 IndexZ = 0;
	Cursor.ReadUInt32(IndexX);
	Cursor.ReadUInt32(IndexY);
	Cursor.ReadUInt32(IndexZ);

	uint16 ValidX = 0;
	uint16 ValidY = 0;
	uint16 ValidZ = 0;
	uint16 FlagsValue = 0;
	Cursor.ReadUInt16(ValidX);
	Cursor.ReadUInt16(ValidY);
	Cursor.ReadUInt16(ValidZ);
	Cursor.ReadUInt16(FlagsValue);

	uint64 PayloadOffsetValue = 0;
	uint32 CompressedBytesValue = 0;
	uint32 UncompressedBytesValue = 0;
	Cursor.ReadUInt64(PayloadOffsetValue);
	Cursor.ReadUInt32(CompressedBytesValue);
	Cursor.ReadUInt32(UncompressedBytesValue);

	float MinValues[CFDViz::MaxCvfComponentCount] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float MaxValues[CFDViz::MaxCvfComponentCount] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (int32 Slot = 0; Slot < CFDViz::MaxCvfComponentCount; ++Slot)
	{
		Cursor.ReadFloat(MinValues[Slot]);
	}
	for (int32 Slot = 0; Slot < CFDViz::MaxCvfComponentCount; ++Slot)
	{
		Cursor.ReadFloat(MaxValues[Slot]);
	}

	uint32 PayloadCrcValue = 0;
	Cursor.ReadUInt32(PayloadCrcValue);

	bool bReservedZero = false;
	Cursor.ReadIsAllZero(EntryReservedBytes, bReservedZero);

	// As in the header parser: the cursor spans exactly one entry, so this turns
	// "every field fits" from an argument into a check.
	if (Cursor.Tell() != CFDViz::CvfDirectoryEntryBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::FileTooSmall,
			FString::Printf(
				TEXT("CVF directory entry parse consumed %lld of %lld bytes"),
				Cursor.Tell(), static_cast<long long>(CFDViz::CvfDirectoryEntryBytes)),
			FString(),
			FileOffset + Cursor.Tell());
	}

	if (!bReservedZero)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			TEXT("reserved brick directory bytes [72, 80) are not zero"),
			FString(),
			FileOffset + EntryOffsetReserved);
	}

	if (FlagsValue != 0)
	{
		// No per-brick flag bits are defined in 1.0. An unknown one could change
		// how this payload decodes, so it is refused rather than ignored -
		// decoding anyway would yield plausible wrong voxels.
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("brick flags 0x%04X are set; this file uses a per-brick feature this reader does not implement"),
				static_cast<uint32>(FlagsValue)),
			FString(),
			FileOffset + EntryOffsetFlags);
	}

	if (PayloadOffsetValue > static_cast<uint64>(MAX_int64))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(TEXT("brick payload offset %llu is not representable"), PayloadOffsetValue),
			FString(),
			FileOffset + EntryOffsetPayloadOffset);
	}

	// validSize is NOT checked for a zero component here. It is checked against
	// the tiling by FCFDVizVolumeReader::ValidateBrickEntry, which knows the
	// header and so can say what the size should have been - and which reports
	// the same byte offset. A zero can never survive that equality, because the
	// expected remainder is always at least 1.

	if (static_cast<int64>(IndexX) > CFDViz::MaxGridDimension
		|| static_cast<int64>(IndexY) > CFDViz::MaxGridDimension
		|| static_cast<int64>(IndexZ) > CFDViz::MaxGridDimension)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::AllocationTooLarge,
			FString::Printf(
				TEXT("brick index (%u, %u, %u) exceeds the %lld this reader can address per axis"),
				IndexX, IndexY, IndexZ, static_cast<long long>(CFDViz::MaxGridDimension)),
			FString(),
			FileOffset + EntryOffsetBrickIndex);
	}

	FCFDVizBrickEntry Parsed;
	Parsed.BrickIndex = FIntVector(static_cast<int32>(IndexX), static_cast<int32>(IndexY), static_cast<int32>(IndexZ));
	Parsed.ValidSize = FIntVector(static_cast<int32>(ValidX), static_cast<int32>(ValidY), static_cast<int32>(ValidZ));
	Parsed.Flags = FlagsValue;
	Parsed.PayloadOffset = static_cast<int64>(PayloadOffsetValue);
	Parsed.CompressedBytes = static_cast<int64>(CompressedBytesValue);
	Parsed.UncompressedBytes = static_cast<int64>(UncompressedBytesValue);
	for (int32 Slot = 0; Slot < CFDViz::MaxCvfComponentCount; ++Slot)
	{
		Parsed.ComponentMin[Slot] = MinValues[Slot];
		Parsed.ComponentMax[Slot] = MaxValues[Slot];
	}
	Parsed.PayloadCrc32C = PayloadCrcValue;

	Out = Parsed;
	return FCFDVizResult::Ok();
}

bool FCFDVizBrickEntry::TryGetComponentRange(int32 Component, float& OutMin, float& OutMax) const
{
	if (Component < 0 || Component >= CFDViz::MaxCvfComponentCount)
	{
		return false;
	}

	const float Min = ComponentMin[Component];
	const float Max = ComponentMax[Component];

	// Section 4.4.7: a brick whose every value is NaN or masked stores
	// min = +inf, max = -inf. That is "no valid data", not a range - handing it
	// to a colour map produces an inverted, meaningless scale. The inverted
	// comparison also rejects a NaN in either slot, which no writer should emit
	// but which would otherwise propagate silently.
	if (!(Min <= Max))
	{
		return false;
	}

	OutMin = Min;
	OutMax = Max;
	return true;
}

/* -------------------------------------------------------------------------- */
/* FCFDVizVolumeReader - lifetime                                               */
/* -------------------------------------------------------------------------- */

FCFDVizVolumeReader::FCFDVizVolumeReader() = default;
FCFDVizVolumeReader::~FCFDVizVolumeReader() = default;

/*
 * The moves are written out rather than defaulted because Source is a raw
 * pointer that may alias OwnedSource. A defaulted move would transfer the
 * TUniquePtr and copy the pointer, leaving the moved-from reader still flagged
 * open with a Source aimed at an object the moved-to reader now owns - a read on
 * it would succeed today and dangle the moment the other reader is destroyed.
 * Rebinding and then clearing the source is what makes a moved-from reader
 * simply closed.
 */
FCFDVizVolumeReader::FCFDVizVolumeReader(FCFDVizVolumeReader&& Other)
{
	*this = MoveTemp(Other);
}

FCFDVizVolumeReader& FCFDVizVolumeReader::operator=(FCFDVizVolumeReader&& Other)
{
	if (this == &Other)
	{
		return *this;
	}

	// Whether Source aliased OwnedSource has to be decided before OwnedSource is
	// moved out from under it.
	const bool bSourceWasOwned = Other.Source != nullptr && Other.Source == Other.OwnedSource.Get();

	FilePath = MoveTemp(Other.FilePath);
	FileSize = Other.FileSize;
	bIsOpen = Other.bIsOpen;
	Header = Other.Header;
	Bricks = MoveTemp(Other.Bricks);
	BrickLookup = MoveTemp(Other.BrickLookup);
	OwnedSource = MoveTemp(Other.OwnedSource);
	DecodeScratch = MoveTemp(Other.DecodeScratch);

	// An owned source moved with the TUniquePtr, so re-derive the pointer; a
	// borrowed one belongs to the caller and is unaffected by the move.
	Source = bSourceWasOwned ? OwnedSource.Get() : Other.Source;

	Other.Source = nullptr;
	Other.FileSize = 0;
	Other.bIsOpen = false;
	Other.Header = FCFDVizVolumeHeader();
	Other.Bricks.Reset();
	Other.BrickLookup.Reset();
	Other.DecodeScratch.Reset();

	return *this;
}

void FCFDVizVolumeReader::Close()
{
	Source = nullptr;
	OwnedSource.Reset();
	FilePath.Reset();
	FileSize = 0;
	bIsOpen = false;
	Header = FCFDVizVolumeHeader();
	Bricks.Reset();
	BrickLookup.Reset();
	DecodeScratch.Reset();
}

FCFDVizResult FCFDVizVolumeReader::Fail(ECFDVizError Error, FString Message, int64 ByteOffset) const
{
	return FCFDVizResult::Fail(Error, MoveTemp(Message), FilePath, ByteOffset);
}

FCFDVizResult FCFDVizVolumeReader::RequireOpen() const
{
	if (!bIsOpen || Source == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::FileReadFailed,
			TEXT("the volume reader is not open; call Open() first"),
			FilePath);
	}
	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* FCFDVizVolumeReader - opening                                                */
/* -------------------------------------------------------------------------- */

FCFDVizResult FCFDVizVolumeReader::Open(const FString& Path, bool bVerifyHeaderCrc)
{
	Close();

	TUniquePtr<FCFDVizFileByteSource> FileSource = MakeUnique<FCFDVizFileByteSource>();
	if (!FileSource->Open(Path))
	{
		// FilePath is set first so the failure names the file even though nothing
		// was opened; Close() then clears it, so the reader is left closed rather
		// than half-open.
		FilePath = Path;
		FCFDVizResult Result = Fail(ECFDVizError::FileNotFound, TEXT("could not open the file for reading"));
		Close();
		return Result;
	}

	OwnedSource = MoveTemp(FileSource);
	Source = OwnedSource.Get();
	FilePath = Path;
	FileSize = Source->GetSize();

	FCFDVizResult Result = OpenInternal(bVerifyHeaderCrc);
	if (!Result.IsOk())
	{
		// A partly parsed reader is worse than a closed one: a caller that ignored
		// the result could read a header that was never validated.
		Close();
	}
	return Result;
}

FCFDVizResult FCFDVizVolumeReader::Open(const ICFDVizByteSource& InSource, bool bVerifyHeaderCrc)
{
	Close();

	Source = &InSource;
	FilePath = InSource.GetDisplayPath();
	FileSize = InSource.GetSize();

	FCFDVizResult Result = OpenInternal(bVerifyHeaderCrc);
	if (!Result.IsOk())
	{
		Close();
	}
	return Result;
}

FCFDVizResult FCFDVizVolumeReader::OpenInternal(bool bVerifyHeaderCrc)
{
	check(Source != nullptr);

	if (FileSize < CFDViz::CvfHeaderBytes)
	{
		return Fail(
			ECFDVizError::FileTooSmall,
			FString::Printf(
				TEXT("file is %lld bytes, shorter than the %lld-byte CVF header"),
				FileSize, static_cast<long long>(CFDViz::CvfHeaderBytes)),
			0);
	}

	uint8 HeaderBytes[CFDViz::CvfHeaderBytes] = {};
	if (!Source->Read(0, CFDViz::CvfHeaderBytes, HeaderBytes))
	{
		return Fail(ECFDVizError::FileReadFailed, TEXT("failed to read the 128-byte CVF header"), 0);
	}

	FCFDVizResult HeaderResult = FCFDVizVolumeHeader::Parse(
		TArrayView<const uint8>(HeaderBytes, CFDViz::CvfHeaderBytes), Header, bVerifyHeaderCrc);
	if (!HeaderResult.IsOk())
	{
		// Parse deliberately leaves FilePath empty - it does not know which file
		// the bytes came from. This is the one place that does.
		HeaderResult.FilePath = FilePath;
		return HeaderResult;
	}

	// payloadOffset is informational - every brick carries its own absolute
	// offset, and that is what actually gets bounds-checked - but it must still
	// describe a position in this file. A value inside the header or past the end
	// means the header is internally inconsistent, and the rest of it should not
	// be trusted on the strength of "we never read that field anyway". Note that
	// payloadOffset == FileSize is legal: a fully sparse volume has no payload
	// bytes at all. The Python reference does not check this field, so a header
	// this self-contradictory loads there and is refused here; the divergence is
	// loud, in the strict direction, and confined to files no writer produces.
	if (Header.PayloadOffset < CFDViz::CvfHeaderBytes)
	{
		return Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("payloadOffset %lld overlaps the %lld-byte header"),
				Header.PayloadOffset, static_cast<long long>(CFDViz::CvfHeaderBytes)),
			OffsetPayloadOffset);
	}
	// Contains with a zero length is exactly "0 <= offset <= size", checked
	// against the ACTUAL file size rather than anything the header claims.
	if (!Source->Contains(Header.PayloadOffset, 0))
	{
		return Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("payloadOffset %lld is past the end of the %lld byte file; it is truncated or the header is corrupt"),
				Header.PayloadOffset, FileSize),
			OffsetPayloadOffset);
	}

	// The dense size is computed here, before any read, purely so an
	// unrepresentable extent is reported at Open rather than surfacing later as a
	// mysterious failure from ReadDense.
	if (Header.GetValueCount() == INDEX_NONE)
	{
		const FIntVector Counts = Header.GetValueCounts();
		return Fail(
			ECFDVizError::AllocationTooLarge,
			FString::Printf(
				TEXT("value extent %s overflows this reader's addressable range"),
				*FormatBrickCoordinate(Counts)),
			OffsetDimensions);
	}

	FCFDVizResult DirectoryResult = ReadDirectory();
	if (!DirectoryResult.IsOk())
	{
		return DirectoryResult;
	}

	bIsOpen = true;
	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizVolumeReader::ReadDirectory()
{
	check(Source != nullptr);

	Bricks.Reset();
	BrickLookup.Reset();

	// Bound brickCount against the volume's own geometry BEFORE it is multiplied
	// into a byte length or an allocation (format rule 1.5).
	const int64 TotalBricks = Header.GetTotalBrickCount();
	if (TotalBricks == INDEX_NONE)
	{
		return Fail(
			ECFDVizError::AllocationTooLarge,
			TEXT("the brick grid implied by this header overflows"),
			OffsetBrickCount);
	}
	if (Header.BrickCount < 0 || Header.BrickCount > TotalBricks)
	{
		const FIntVector Grid = Header.GetBrickCounts();
		const FIntVector Counts = Header.GetValueCounts();
		return Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("brickCount %lld exceeds the %lld bricks a %s volume in %s bricks can hold; the directory is inconsistent with the header"),
				Header.BrickCount, TotalBricks, *FormatBrickCoordinate(Counts), *FormatBrickCoordinate(Grid)),
			OffsetBrickCount);
	}
	if (Header.BrickCount > MAX_int32)
	{
		// TArray is int32-indexed. A file this large is not necessarily malformed,
		// so say what the real limit is instead of calling it corrupt.
		return Fail(
			ECFDVizError::AllocationTooLarge,
			FString::Printf(
				TEXT("brickCount %lld exceeds the %d directory entries this reader can hold"),
				Header.BrickCount, MAX_int32),
			OffsetBrickCount);
	}

	if (Header.DirectoryOffset < CFDViz::CvfHeaderBytes)
	{
		return Fail(
			ECFDVizError::DirectoryOutOfBounds,
			FString::Printf(
				TEXT("directoryOffset %lld overlaps the %lld-byte header"),
				Header.DirectoryOffset, static_cast<long long>(CFDViz::CvfHeaderBytes)),
			OffsetDirectoryOffset);
	}

	int64 DirectoryBytes = 0;
	if (!CFDViz::TryMultiply(Header.BrickCount, CFDViz::CvfDirectoryEntryBytes, DirectoryBytes))
	{
		return Fail(
			ECFDVizError::DirectoryOutOfBounds,
			FString::Printf(TEXT("brick directory of %lld entries overflows"), Header.BrickCount),
			OffsetBrickCount);
	}
	// Checked against the ACTUAL file size, not against anything the header
	// claims. Contains() subtracts rather than adds, so a hostile offset near
	// 2^63 cannot wrap into a passing comparison.
	if (!Source->Contains(Header.DirectoryOffset, DirectoryBytes))
	{
		return Fail(
			ECFDVizError::DirectoryOutOfBounds,
			FString::Printf(
				TEXT("brick directory needs %lld bytes at offset %lld, but the file is only %lld bytes; it is truncated or the header is corrupt"),
				DirectoryBytes, Header.DirectoryOffset, FileSize),
			OffsetDirectoryOffset);
	}

	const int32 EntryCount = static_cast<int32>(Header.BrickCount);
	if (EntryCount == 0)
	{
		// A legitimate file: every brick equalled backgroundValue and all of them
		// were omitted (section 4.4.5). ReadDense still returns a full volume.
		return FCFDVizResult::Ok();
	}

	// Safe to reserve now, and only now: the span has been proven to exist in the
	// file, so brickCount is bounded by FileSize / 80.
	Bricks.Reserve(EntryCount);
	BrickLookup.Reserve(EntryCount);

	TArray<uint8> Chunk;
	Chunk.SetNumUninitialized(
		FMath::Min(DirectoryEntriesPerChunk, EntryCount) * static_cast<int32>(CFDViz::CvfDirectoryEntryBytes));

	int32 EntryIndex = 0;
	while (EntryIndex < EntryCount)
	{
		const int32 ThisChunk = FMath::Min(DirectoryEntriesPerChunk, EntryCount - EntryIndex);
		const int64 ChunkOffset = Header.DirectoryOffset + static_cast<int64>(EntryIndex) * CFDViz::CvfDirectoryEntryBytes;
		const int64 ChunkBytes = static_cast<int64>(ThisChunk) * CFDViz::CvfDirectoryEntryBytes;

		if (!Source->Read(ChunkOffset, ChunkBytes, Chunk.GetData()))
		{
			return Fail(
				ECFDVizError::FileReadFailed,
				FString::Printf(TEXT("failed to read %lld bytes of the brick directory"), ChunkBytes),
				ChunkOffset);
		}

		for (int32 Slot = 0; Slot < ThisChunk; ++Slot)
		{
			const int64 EntryOffset = ChunkOffset + static_cast<int64>(Slot) * CFDViz::CvfDirectoryEntryBytes;
			const TArrayView<const uint8> EntryBytes(
				Chunk.GetData() + static_cast<int64>(Slot) * CFDViz::CvfDirectoryEntryBytes,
				static_cast<int32>(CFDViz::CvfDirectoryEntryBytes));

			FCFDVizBrickEntry Entry;
			FCFDVizResult EntryResult = FCFDVizBrickEntry::Parse(EntryBytes, Entry, EntryOffset);
			if (!EntryResult.IsOk())
			{
				EntryResult.FilePath = FilePath;
				return EntryResult;
			}

			// ValidateBrickEntry consults BrickLookup for the duplicate check, so
			// it must run before this entry is added to it.
			FCFDVizResult ValidationResult = ValidateBrickEntry(Entry, EntryOffset);
			if (!ValidationResult.IsOk())
			{
				return ValidationResult;
			}

			BrickLookup.Add(Entry.BrickIndex, Bricks.Num());
			Bricks.Add(Entry);
		}

		EntryIndex += ThisChunk;
	}

	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizVolumeReader::ValidateBrickEntry(const FCFDVizBrickEntry& Entry, int64 EntryOffset) const
{
	const FIntVector Grid = Header.GetBrickCounts();
	if (Entry.BrickIndex.X >= Grid.X || Entry.BrickIndex.Y >= Grid.Y || Entry.BrickIndex.Z >= Grid.Z)
	{
		return Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("brick index %s is outside the %s brick grid of this volume"),
				*FormatBrickCoordinate(Entry.BrickIndex), *FormatBrickCoordinate(Grid)),
			EntryOffset + EntryOffsetBrickIndex);
	}

	// Two entries for one brick leaves which copy wins undefined - and, worse,
	// silently resolved differently by FindBrickByCoordinate (first wins) and by
	// ReadDense (last written wins), so ReadVoxel and ReadDense would disagree
	// about the same file.
	if (BrickLookup.Contains(Entry.BrickIndex))
	{
		return Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("duplicate directory entry for brick %s; which copy wins would be undefined"),
				*FormatBrickCoordinate(Entry.BrickIndex)),
			EntryOffset + EntryOffsetBrickIndex);
	}

	// EDGE BRICKS ARE NOT PADDED (section 4.4.3). The entry must declare exactly
	// the remainder its position implies. Checking it here means no later code
	// can be fooled into reconstructing an edge brick as a full cube - the bug
	// that shears a volume by one row and still renders.
	const FIntVector Expected = Header.GetValidSizeForBrick(Entry.BrickIndex);
	if (Entry.ValidSize != Expected)
	{
		return Fail(
			ECFDVizError::SizeMismatch,
			FString::Printf(
				TEXT("brick %s declares validSize %s but its position in a %s volume with %s bricks requires %s; edge bricks are clipped, never padded (section 4.4.3)"),
				*FormatBrickCoordinate(Entry.BrickIndex),
				*FormatBrickCoordinate(Entry.ValidSize),
				*FormatBrickCoordinate(Header.GetValueCounts()),
				*FormatBrickCoordinate(Header.BrickSize),
				*FormatBrickCoordinate(Expected)),
			EntryOffset + EntryOffsetValidSize);
	}

	// Section 4.4.4, and the reason this runs at Open: prove the declared decoded
	// size against the geometry BEFORE any allocation can be reached. This is the
	// primary defence against a hostile size field, and it is worthless if it
	// runs after the allocation it exists to guard. CFDViz::DecodePayload repeats
	// it, but a caller must not be able to reach a decode with an unproven size
	// in the first place.
	const int64 ExpectedBytes = Entry.ComputeExpectedUncompressedBytes(Header.ComponentCount, Header.DataType);
	if (ExpectedBytes == INDEX_NONE)
	{
		return Fail(
			ECFDVizError::SizeMismatch,
			FString::Printf(
				TEXT("brick %s payload size overflows: %s voxels x %d components x %s"),
				*FormatBrickCoordinate(Entry.BrickIndex),
				*FormatBrickCoordinate(Entry.ValidSize),
				Header.ComponentCount,
				DataTypeToString(Header.DataType)),
			EntryOffset + EntryOffsetUncompressedBytes);
	}
	if (Entry.UncompressedBytes != ExpectedBytes)
	{
		return Fail(
			ECFDVizError::SizeMismatch,
			FString::Printf(
				TEXT("brick %s declares uncompressedBytes %lld, but validSize %s x componentCount %d x sizeof(%s) = %lld"),
				*FormatBrickCoordinate(Entry.BrickIndex),
				Entry.UncompressedBytes,
				*FormatBrickCoordinate(Entry.ValidSize),
				Header.ComponentCount,
				DataTypeToString(Header.DataType),
				ExpectedBytes),
			EntryOffset + EntryOffsetUncompressedBytes);
	}

	if (Entry.PayloadOffset < CFDViz::CvfHeaderBytes && Entry.CompressedBytes > 0)
	{
		return Fail(
			ECFDVizError::PayloadOutOfBounds,
			FString::Printf(
				TEXT("brick %s payload offset %lld overlaps the %lld-byte header"),
				*FormatBrickCoordinate(Entry.BrickIndex),
				Entry.PayloadOffset,
				static_cast<long long>(CFDViz::CvfHeaderBytes)),
			EntryOffset + EntryOffsetPayloadOffset);
	}

	// Against the real file size, before any read (format rule 1.5).
	if (!Source->Contains(Entry.PayloadOffset, Entry.CompressedBytes))
	{
		return Fail(
			ECFDVizError::PayloadOutOfBounds,
			FString::Printf(
				TEXT("brick %s payload needs %lld bytes at offset %lld, but the file is only %lld bytes; it is truncated or the directory is corrupt"),
				*FormatBrickCoordinate(Entry.BrickIndex),
				Entry.CompressedBytes,
				Entry.PayloadOffset,
				FileSize),
			EntryOffset + EntryOffsetPayloadOffset);
	}

	// With no codec, the stored length and the decoded length are the same number
	// by definition. A file that says otherwise describes a transformation it did
	// not perform, and trusting either number would be a guess.
	if (Header.Codec == ECFDVizCodec::None && Entry.CompressedBytes != Entry.UncompressedBytes)
	{
		return Fail(
			ECFDVizError::SizeMismatch,
			FString::Printf(
				TEXT("brick %s uses codec 'none' but compressedBytes %lld != uncompressedBytes %lld"),
				*FormatBrickCoordinate(Entry.BrickIndex),
				Entry.CompressedBytes,
				Entry.UncompressedBytes),
			EntryOffset + EntryOffsetCompressedBytes);
	}

	return FCFDVizResult::Ok();
}

int32 FCFDVizVolumeReader::FindBrickByCoordinate(const FIntVector& BrickCoordinate) const
{
	const int32* Found = BrickLookup.Find(BrickCoordinate);
	// Absence is a legitimate answer, not an error: section 4.4.5 says an omitted
	// brick evaluates to backgroundValue everywhere.
	return (Found != nullptr) ? *Found : INDEX_NONE;
}

FCFDVizResult FCFDVizVolumeReader::ParseHeader(
	TArrayView<const uint8> HeaderBytes,
	const FString& DisplayPath,
	FCFDVizVolumeHeader& OutHeader,
	bool bVerifyCrc)
{
	FCFDVizResult Result = FCFDVizVolumeHeader::Parse(HeaderBytes, OutHeader, bVerifyCrc);
	if (!Result.IsOk())
	{
		Result.FilePath = DisplayPath;
	}
	return Result;
}

/* -------------------------------------------------------------------------- */
/* FCFDVizVolumeReader - decoding                                               */
/* -------------------------------------------------------------------------- */

FCFDVizResult FCFDVizVolumeReader::DecodeBrickInto(int32 BrickIndex, TArray<uint8>& Buffer, bool bVerifyCrc)
{
	Buffer.Reset();

	FCFDVizResult OpenResult = RequireOpen();
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	if (!Bricks.IsValidIndex(BrickIndex))
	{
		return Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("brick index %d is outside the %d entries in this directory"),
				BrickIndex, Bricks.Num()));
	}

	const FCFDVizBrickEntry& Entry = Bricks[BrickIndex];

	// The shared decoder, not a private copy. Section 6.3 makes CVA's payload
	// rules mirror CVF 4.4 exactly, and two decoders written to be identical will
	// drift. It enforces the normative order: reject zstd, prove the derived-size
	// equality, cap the allocation, bounds-check the span, CRC the STORED bytes,
	// and only then inflate. ValueCount is the brick's voxel count, so its
	// derived-size check is exactly the section 4.4.4 equality.
	CFDViz::FPayloadSpec Spec;
	Spec.Offset = Entry.PayloadOffset;
	Spec.CompressedBytes = Entry.CompressedBytes;
	Spec.UncompressedBytes = Entry.UncompressedBytes;
	Spec.PayloadCrc32C = Entry.PayloadCrc32C;
	Spec.ValueCount = Entry.GetVoxelCount();
	Spec.ComponentCount = Header.ComponentCount;
	Spec.DataType = Header.DataType;
	Spec.Codec = Header.Codec;
	Spec.Context = FString::Printf(
		TEXT("brick %d at %s"), BrickIndex, *FormatBrickCoordinate(Entry.BrickIndex));

	return CFDViz::DecodePayload(*Source, Spec, Buffer, bVerifyCrc, CFDViz::MaxReadableBytes);
}

FCFDVizResult FCFDVizVolumeReader::ReadBrick(int32 BrickIndex, TArray<uint8>& OutRaw, bool bVerifyCrc)
{
	return DecodeBrickInto(BrickIndex, OutRaw, bVerifyCrc);
}

FCFDVizResult FCFDVizVolumeReader::GetBackgroundVoxelBytes(TArray<uint8>& OutPattern) const
{
	OutPattern.Reset();

	const int32 ElementBytes = Header.GetElementBytes();
	if (ElementBytes <= 0 || Header.ComponentCount <= 0)
	{
		return Fail(
			ECFDVizError::UnsupportedDataType,
			FString::Printf(
				TEXT("cannot materialise backgroundValue for dataType %s with %d components"),
				DataTypeToString(Header.DataType), Header.ComponentCount));
	}

	const int64 VoxelBytes = Header.GetVoxelBytes();
	OutPattern.SetNumUninitialized(static_cast<int32>(VoxelBytes));
	uint8* const Destination = OutPattern.GetData();

	for (int32 Component = 0; Component < Header.ComponentCount; ++Component)
	{
		const float Value = Header.BackgroundValue[Component];
		uint8* const Slot = Destination + static_cast<int64>(Component) * ElementBytes;

		switch (Header.DataType)
		{
		case ECFDVizDataType::Float32:
			// A straight bit copy, so a NaN background keeps its payload.
			StoreUInt32LE(Slot, FloatToBits(Value));
			break;

		case ECFDVizDataType::Float16:
		{
			const uint32 Bits = FloatToBits(Value);
			const bool bFinite = ((Bits >> 23) & 0xFFu) != 0xFFu;
			const uint16 Half = Float32BitsToFloat16Bits(Bits);
			// A finite background that narrows to infinity has been silently
			// clamped, which format rule 1.6 forbids. Report it instead: the file
			// is describing a value it cannot store, and rendering it as infinity
			// would hide a writer bug behind plausible output. The Python
			// reference's _to_storage rejects exactly this case, with the same
			// finite-in / infinite-out test.
			if (bFinite && (Half & 0x7C00u) == 0x7C00u)
			{
				OutPattern.Reset();
				return Fail(
					ECFDVizError::InvalidHeader,
					FString::Printf(
						TEXT("backgroundValue component %d (%g) overflows float16 storage; CFDViz must not silently clamp values (rule 1.6)"),
						Component, Value),
					OffsetBackgroundValue + static_cast<int64>(Component) * 4);
			}
			StoreUInt16LE(Slot, Half);
			break;
		}

		case ECFDVizDataType::UInt8:
		{
			// An exact integer in range, or nothing. Clamping 300 to 255 is the
			// silent clamping rule 1.6 forbids, and the Python reference rejects
			// out-of-range and non-finite here too.
			//
			// The fractional test is stricter than the reference, which would
			// truncate 0.5 to 0 through numpy's cast. Truncation is the silent
			// quantisation rule 1.6 forbids just as much as clamping is, and a
			// fractional uint8 background is a writer bug either way, so it is
			// surfaced rather than rounded. The divergence is loud and confined to
			// a file no exporter produces - never a difference in decoded values.
			if (!FMath::IsFinite(Value) || Value < 0.0f || Value > 255.0f || FMath::Floor(Value) != Value)
			{
				OutPattern.Reset();
				return Fail(
					ECFDVizError::InvalidHeader,
					FString::Printf(
						TEXT("backgroundValue component %d (%g) is not an exact integer in 0..255 and cannot be stored as uint8 without clamping or rounding (rule 1.6)"),
						Component, Value),
					OffsetBackgroundValue + static_cast<int64>(Component) * 4);
			}
			*Slot = static_cast<uint8>(Value);
			break;
		}

		default:
			OutPattern.Reset();
			return Fail(
				ECFDVizError::UnsupportedDataType,
				FString::Printf(
					TEXT("dataType %s is not valid for CVF storage"),
					DataTypeToString(Header.DataType)),
				OffsetDataType);
		}
	}

	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizVolumeReader::ReadDense(TArray<uint8>& OutVolume, int64 MaxBytes, bool bVerifyCrc)
{
	OutVolume.Reset();

	FCFDVizResult OpenResult = RequireOpen();
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	const int64 DenseBytes = Header.GetDenseVolumeBytes();
	if (DenseBytes == INDEX_NONE)
	{
		return Fail(
			ECFDVizError::AllocationTooLarge,
			TEXT("the dense volume size implied by this header overflows"),
			OffsetDimensions);
	}

	// A self-consistent header can still declare a terabyte of empty space, and
	// because absent bricks cost no file bytes there is no file-size correlate to
	// catch it. This is the only guard on that path.
	const int64 Limit = FMath::Min(MaxBytes, CFDViz::MaxReadableBytes);
	if (DenseBytes > Limit)
	{
		return Fail(
			ECFDVizError::AllocationTooLarge,
			FString::Printf(
				TEXT("dense volume of %lld bytes exceeds the %lld byte limit; read this volume brick by brick instead"),
				DenseBytes, Limit),
			OffsetDimensions);
	}

	// Materialised before the allocation, so a background this file cannot
	// represent fails without having reserved gigabytes first.
	TArray<uint8> BackgroundPattern;
	FCFDVizResult BackgroundResult = GetBackgroundVoxelBytes(BackgroundPattern);
	if (!BackgroundResult.IsOk())
	{
		return BackgroundResult;
	}

	TArray<uint8> Volume;
	Volume.SetNumUninitialized(static_cast<int32>(DenseBytes));

	// Section 4.4.5: every brick absent from the directory evaluates to
	// backgroundValue. Prefilling means an absent brick needs no special case in
	// the loop below and, crucially, that no byte of the result is ever left
	// uninitialised - a partially filled buffer would be indistinguishable from
	// data.
	FillWithPattern(Volume.GetData(), DenseBytes, BackgroundPattern.GetData(), BackgroundPattern.Num());

	const FIntVector Counts = Header.GetValueCounts();
	const int64 VoxelBytes = Header.GetVoxelBytes();
	const int64 RowBytes = static_cast<int64>(Counts.X) * VoxelBytes;
	const int64 SliceBytes = RowBytes * static_cast<int64>(Counts.Y);

	for (int32 BrickIndex = 0; BrickIndex < Bricks.Num(); ++BrickIndex)
	{
		const FCFDVizBrickEntry& Entry = Bricks[BrickIndex];

		FCFDVizResult BrickResult = DecodeBrickInto(BrickIndex, DecodeScratch, bVerifyCrc);
		if (!BrickResult.IsOk())
		{
			// Nothing partially reconstructed escapes. Section 4.4.8 allows a
			// VALIDATOR to mark a brick unavailable and continue, but a reader must
			// not hand back a volume with an unexplained hole in it.
			return BrickResult;
		}

		const FIntVector Origin(
			Entry.BrickIndex.X * Header.BrickSize.X,
			Entry.BrickIndex.Y * Header.BrickSize.Y,
			Entry.BrickIndex.Z * Header.BrickSize.Z);
		const FIntVector Valid = Entry.ValidSize;

		// The memcpy loop below reads exactly UncompressedBytes from the decoded
		// buffer, and Open proved UncompressedBytes equals the size Valid implies.
		// This asserts the remaining link - that the decoder produced what it was
		// asked for - so the copy cannot run off the end of a short buffer if that
		// ever stops holding.
		if (DecodeScratch.Num() != Entry.UncompressedBytes)
		{
			return Fail(
				ECFDVizError::SizeMismatch,
				FString::Printf(
					TEXT("brick %d at %s decoded to %d bytes but its entry declares %lld"),
					BrickIndex, *FormatBrickCoordinate(Entry.BrickIndex),
					DecodeScratch.Num(), Entry.UncompressedBytes),
				Entry.PayloadOffset);
		}

		// Within a brick and within the volume alike: X fastest, then Y, then Z,
		// components interleaved per voxel (sections 4.4.1 and 4.4.2). So one row
		// of a brick is contiguous in both, and the copy is a run per (y, z).
		// Valid is the brick's OWN extent, never Header.BrickSize - that
		// distinction is the whole of the unpadded-edge-brick rule.
		const int64 BrickRowBytes = static_cast<int64>(Valid.X) * VoxelBytes;
		const uint8* const SourceBytes = DecodeScratch.GetData();
		uint8* const DestinationBytes = Volume.GetData();

		for (int32 Z = 0; Z < Valid.Z; ++Z)
		{
			for (int32 Y = 0; Y < Valid.Y; ++Y)
			{
				const int64 SourceOffset =
					((static_cast<int64>(Z) * Valid.Y + Y) * Valid.X) * VoxelBytes;
				const int64 DestinationOffset =
					static_cast<int64>(Origin.Z + Z) * SliceBytes
					+ static_cast<int64>(Origin.Y + Y) * RowBytes
					+ static_cast<int64>(Origin.X) * VoxelBytes;

				FMemory::Memcpy(
					DestinationBytes + DestinationOffset,
					SourceBytes + SourceOffset,
					static_cast<SIZE_T>(BrickRowBytes));
			}
		}
	}

	OutVolume = MoveTemp(Volume);
	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizVolumeReader::ReadVoxel(int32 I, int32 J, int32 K, TArray<uint8>& OutValue)
{
	OutValue.Reset();

	FCFDVizResult OpenResult = RequireOpen();
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	if (!Header.ContainsValue(I, J, K))
	{
		const FIntVector Counts = Header.GetValueCounts();
		return Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("voxel (%d, %d, %d) is outside the %s value extent of this %s-associated field"),
				I, J, K, *FormatBrickCoordinate(Counts), AssociationToString(Header.Association)));
	}

	const int64 VoxelBytes = Header.GetVoxelBytes();

	// Integer division, so this is the tiling of section 4.4 exactly - the same
	// arithmetic ReadDense walks in the other direction.
	const FIntVector BrickCoordinate(
		I / Header.BrickSize.X,
		J / Header.BrickSize.Y,
		K / Header.BrickSize.Z);

	const int32 BrickIndex = FindBrickByCoordinate(BrickCoordinate);
	if (BrickIndex == INDEX_NONE)
	{
		// Section 4.4.5: the brick was omitted, so every voxel in it - including
		// this one - is backgroundValue. Byte-identical to what ReadDense would
		// have left here, because both go through GetBackgroundVoxelBytes.
		return GetBackgroundVoxelBytes(OutValue);
	}

	FCFDVizResult BrickResult = DecodeBrickInto(BrickIndex, DecodeScratch, /*bVerifyCrc=*/true);
	if (!BrickResult.IsOk())
	{
		return BrickResult;
	}

	const FCFDVizBrickEntry& Entry = Bricks[BrickIndex];
	const FIntVector Local(
		I - Entry.BrickIndex.X * Header.BrickSize.X,
		J - Entry.BrickIndex.Y * Header.BrickSize.Y,
		K - Entry.BrickIndex.Z * Header.BrickSize.Z);

	// Open already proved ValidSize against the tiling, so this cannot fail on a
	// file that opened. It is here because the cost of being wrong is a read past
	// the end of a decoded buffer, and a bounds check is cheaper than the class of
	// bug it prevents.
	if (Local.X < 0 || Local.Y < 0 || Local.Z < 0
		|| Local.X >= Entry.ValidSize.X || Local.Y >= Entry.ValidSize.Y || Local.Z >= Entry.ValidSize.Z)
	{
		return Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("voxel (%d, %d, %d) maps to %s inside brick %s, which stores only %s voxels"),
				I, J, K,
				*FormatBrickCoordinate(Local),
				*FormatBrickCoordinate(Entry.BrickIndex),
				*FormatBrickCoordinate(Entry.ValidSize)));
	}

	// X fastest, then Y, then Z, components interleaved - the same indexing
	// ReadDense uses, over this brick's OWN valid extent rather than the nominal
	// brick size.
	const int64 ValueOffset =
		((static_cast<int64>(Local.Z) * Entry.ValidSize.Y + Local.Y) * Entry.ValidSize.X + Local.X) * VoxelBytes;

	if (ValueOffset < 0 || VoxelBytes > DecodeScratch.Num() - ValueOffset)
	{
		return Fail(
			ECFDVizError::SizeMismatch,
			FString::Printf(
				TEXT("voxel (%d, %d, %d) lies at byte %lld of a %d byte brick payload"),
				I, J, K, ValueOffset, DecodeScratch.Num()),
			Entry.PayloadOffset);
	}

	OutValue.SetNumUninitialized(static_cast<int32>(VoxelBytes));
	// A raw byte copy, so the stored bit pattern - NaN payloads included - reaches
	// the caller untouched. Nothing here widens float16 to float; that belongs in
	// the adapter layer.
	FMemory::Memcpy(OutValue.GetData(), DecodeScratch.GetData() + ValueOffset, static_cast<SIZE_T>(VoxelBytes));
	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* FCFDVizVolumeReader - integrity                                              */
/* -------------------------------------------------------------------------- */

FCFDVizResult FCFDVizVolumeReader::VerifyHeaderCrc()
{
	FCFDVizResult OpenResult = RequireOpen();
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	uint8 HeaderBytes[CFDViz::CvfHeaderBytes] = {};
	if (!Source->Read(0, CFDViz::CvfHeaderBytes, HeaderBytes))
	{
		return Fail(ECFDVizError::FileReadFailed, TEXT("failed to re-read the 128-byte CVF header"), 0);
	}

	const TArrayView<const uint8> View(HeaderBytes, CFDViz::CvfHeaderBytes);

	// Both sides of the comparison come from the bytes just read, not from the
	// cached Header. This is an integrity check on what is on disk right now: if
	// the file changed since Open, comparing a fresh checksum against a stale
	// stored value would report a mismatch in the wrong direction, and if Open
	// ran with bVerifyHeaderCrc false the cached value was never trusted in the
	// first place.
	CFDViz::FByteCursor Cursor(View);
	uint32 Stored = 0;
	if (!Cursor.Seek(OffsetHeaderCrc) || !Cursor.ReadUInt32(Stored))
	{
		return Fail(ECFDVizError::FileTooSmall, TEXT("the CVF header is too short to hold its CRC field"), OffsetHeaderCrc);
	}

	const uint32 Computed = FCFDVizVolumeHeader::ComputeHeaderCrc(View);
	if (Computed != Stored)
	{
		return Fail(
			ECFDVizError::HeaderCrcMismatch,
			FString::Printf(
				TEXT("header CRC-32C mismatch: stored 0x%08X, computed 0x%08X. The header is corrupt."),
				Stored, Computed),
			OffsetHeaderCrc);
	}
	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizVolumeReader::VerifyBrickCrc(int32 BrickIndex)
{
	FCFDVizResult OpenResult = RequireOpen();
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	if (!Bricks.IsValidIndex(BrickIndex))
	{
		return Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("brick index %d is outside the %d entries in this directory"),
				BrickIndex, Bricks.Num()));
	}

	const FCFDVizBrickEntry& Entry = Bricks[BrickIndex];

	// Section 4.3: the CRC covers the COMPRESSED bytes exactly as stored, so this
	// needs no decompression - and ComputeStoredCrc streams in 64 KiB chunks, so
	// it needs no whole-brick allocation either. That is what makes a full
	// integrity pass over a multi-gigabyte case affordable.
	uint32 Computed = 0;
	if (!CFDViz::ComputeStoredCrc(*Source, Entry.PayloadOffset, Entry.CompressedBytes, Computed))
	{
		return Fail(
			ECFDVizError::FileReadFailed,
			FString::Printf(
				TEXT("failed to read %lld stored bytes of brick %d at %s"),
				Entry.CompressedBytes, BrickIndex, *FormatBrickCoordinate(Entry.BrickIndex)),
			Entry.PayloadOffset);
	}

	if (Computed != Entry.PayloadCrc32C)
	{
		return Fail(
			ECFDVizError::PayloadCrcMismatch,
			FString::Printf(
				TEXT("payload CRC mismatch in brick %d at %s: expected 0x%08X, computed 0x%08X"),
				BrickIndex, *FormatBrickCoordinate(Entry.BrickIndex), Entry.PayloadCrc32C, Computed),
			Entry.PayloadOffset);
	}

	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizVolumeReader::VerifyAllCrcs()
{
	FCFDVizResult OpenResult = RequireOpen();
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	FCFDVizResult HeaderResult = VerifyHeaderCrc();
	if (!HeaderResult.IsOk())
	{
		return HeaderResult;
	}

	for (int32 BrickIndex = 0; BrickIndex < Bricks.Num(); ++BrickIndex)
	{
		FCFDVizResult BrickResult = VerifyBrickCrc(BrickIndex);
		if (!BrickResult.IsOk())
		{
			return BrickResult;
		}
	}

	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizVolumeReader::VerifyAllPayloadCrcs(TArray<int32>& OutFailedBricks)
{
	OutFailedBricks.Reset();

	FCFDVizResult OpenResult = RequireOpen();
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	// Section 4.4.8 lets a validator mark a failing brick unavailable and carry
	// on, provided the failure is surfaced. Collecting every bad brick rather
	// than stopping at the first is what makes that report possible - but the
	// return value is still a failure, so a caller that only checks IsOk() cannot
	// mistake a corrupt file for a clean one.
	for (int32 BrickIndex = 0; BrickIndex < Bricks.Num(); ++BrickIndex)
	{
		const FCFDVizBrickEntry& Entry = Bricks[BrickIndex];

		uint32 Computed = 0;
		if (!CFDViz::ComputeStoredCrc(*Source, Entry.PayloadOffset, Entry.CompressedBytes, Computed))
		{
			// A read failure is not a CRC failure: the file could not be examined
			// at all, so reporting "this brick is corrupt" would be a guess.
			return Fail(
				ECFDVizError::FileReadFailed,
				FString::Printf(
					TEXT("failed to read %lld stored bytes of brick %d at %s"),
					Entry.CompressedBytes, BrickIndex, *FormatBrickCoordinate(Entry.BrickIndex)),
				Entry.PayloadOffset);
		}

		if (Computed != Entry.PayloadCrc32C)
		{
			OutFailedBricks.Add(BrickIndex);
		}
	}

	if (OutFailedBricks.Num() > 0)
	{
		const int32 FirstFailed = OutFailedBricks[0];
		return Fail(
			ECFDVizError::PayloadCrcMismatch,
			FString::Printf(
				TEXT("%d of %d bricks failed their payload CRC; the first is brick %d at %s"),
				OutFailedBricks.Num(), Bricks.Num(), FirstFailed,
				*FormatBrickCoordinate(Bricks[FirstFailed].BrickIndex)),
			Bricks[FirstFailed].PayloadOffset);
	}

	return FCFDVizResult::Ok();
}
