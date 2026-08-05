// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizArrayReader.h"

#include "CFDViz/CFDVizCrc32C.h"
#include "CFDVizByteCursor.h"

namespace CFDViz
{
	bool IsValidCvaComponentCount(int32 ComponentCount)
	{
		// 1 scalar, 3 vector, 6 symmetric tensor, 9 full tensor (section 6).
		// Anything else - 2, 4, 16 - means the writer and this reader disagree
		// about what the array is, so it is refused rather than guessed at.
		return ComponentCount == 1 || ComponentCount == 3 || ComponentCount == 6 || ComponentCount == 9;
	}

	TArrayView<const TCHAR* const> GetCvaComponentNames(int32 ComponentCount)
	{
		static const TCHAR* const ScalarNames[] = { TEXT("Value") };
		static const TCHAR* const VectorNames[] = { TEXT("X"), TEXT("Y"), TEXT("Z") };

		// THE NORMATIVE SYMMETRIC-TENSOR ORDER (section 6.3).
		//
		// XX, YY, ZZ, XY, YZ, XZ - and NOT the other common Voigt convention
		// XX, YY, ZZ, YZ, XZ, XY. The spec singles this out as "the classic way
		// for two solvers to silently disagree": under the wrong order the shear
		// components are permuted, every number still looks physically
		// plausible, and nothing downstream can detect it. This array is the one
		// place the order is written down on this side of the bridge.
		static const TCHAR* const SymmetricNames[] = {
			TEXT("XX"), TEXT("YY"), TEXT("ZZ"), TEXT("XY"), TEXT("YZ"), TEXT("XZ")
		};

		// Full tensors are row-major.
		static const TCHAR* const FullNames[] = {
			TEXT("XX"), TEXT("XY"), TEXT("XZ"),
			TEXT("YX"), TEXT("YY"), TEXT("YZ"),
			TEXT("ZX"), TEXT("ZY"), TEXT("ZZ")
		};

		switch (ComponentCount)
		{
		case 1: return TArrayView<const TCHAR* const>(ScalarNames, 1);
		case 3: return TArrayView<const TCHAR* const>(VectorNames, 3);
		case 6: return TArrayView<const TCHAR* const>(SymmetricNames, 6);
		case 9: return TArrayView<const TCHAR* const>(FullNames, 9);
		default: return TArrayView<const TCHAR* const>();
		}
	}
}

namespace
{
	/**
	 * Verify the header CRC.
	 *
	 * Section 6.1: CRC-32C over [0,96) with bytes [80,84) ZEROED - not skipped.
	 * Zeroing keeps the checksummed length equal to the header length, so the
	 * CRC still covers the field's position. The copy is 96 bytes on the stack;
	 * mutating the caller's buffer to compute a checksum would be a nasty
	 * surprise for a caller that also wanted the raw bytes.
	 */
	bool VerifyCvaHeaderCrc(TArrayView<const uint8> HeaderBytes, uint32 StoredCrc)
	{
		uint8 Scratch[CFDViz::CvaHeaderBytes];
		FMemory::Memcpy(Scratch, HeaderBytes.GetData(), CFDViz::CvaHeaderBytes);
		FMemory::Memzero(Scratch + CFDViz::CvaHeaderCrcOffset, CFDViz::HeaderCrcFieldBytes);
		return CFDViz::Crc32C::Compute(Scratch, CFDViz::CvaHeaderBytes) == StoredCrc;
	}

	/** Parse one statistics section (section 6.4) from an already-bounds-checked span. */
	bool ParseStatisticsSection(
		TArrayView<const uint8> Bytes,
		int32 ComponentCount,
		FCFDVizArrayStatistics& Out)
	{
		CFDViz::FByteCursor Cursor(Bytes);

		uint64 ValueCount = 0;
		if (!Cursor.ReadUInt64(ValueCount))
		{
			return false;
		}
		Out.ValueCount = static_cast<int64>(ValueCount);

		// Four contiguous arrays, each ComponentCount long: minimum, maximum,
		// mean, then validCount. Read strictly in that order - the sections are
		// the same size and full of plausible doubles, so a swap of maximum and
		// mean would produce a working reader with quietly wrong colour scales.
		Out.Minimum.SetNumUninitialized(ComponentCount);
		Out.Maximum.SetNumUninitialized(ComponentCount);
		Out.Mean.SetNumUninitialized(ComponentCount);
		Out.ValidCount.SetNumUninitialized(ComponentCount);

		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			if (!Cursor.ReadDouble(Out.Minimum[Index]))
			{
				return false;
			}
		}
		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			if (!Cursor.ReadDouble(Out.Maximum[Index]))
			{
				return false;
			}
		}
		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			if (!Cursor.ReadDouble(Out.Mean[Index]))
			{
				return false;
			}
		}
		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			if (!Cursor.ReadUInt64(Out.ValidCount[Index]))
			{
				return false;
			}
		}
		return true;
	}
}

FCFDVizResult FCFDVizArrayReader::ParseHeader(
	TArrayView<const uint8> HeaderBytes,
	const FString& DisplayPath,
	FCFDVizArrayHeader& OutHeader)
{
	if (HeaderBytes.Num() < CFDViz::CvaHeaderBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::FileTooSmall,
			FString::Printf(TEXT("a CVA header is %lld bytes but only %d were supplied"),
				CFDViz::CvaHeaderBytes, HeaderBytes.Num()),
			DisplayPath, 0);
	}

	CFDViz::FByteCursor Cursor(HeaderBytes.Left(CFDViz::CvaHeaderBytes));

	// --- magic @0
	ANSICHAR Magic[8] = {};
	if (!Cursor.ReadBytes(Magic, 8))
	{
		return FCFDVizResult::Fail(ECFDVizError::FileTooSmall, TEXT("truncated magic"), DisplayPath, 0);
	}
	if (FMemory::Memcmp(Magic, CFDViz::CvaMagic, 8) != 0)
	{
		return FCFDVizResult::Fail(ECFDVizError::BadMagic,
			TEXT("not a CVA file: magic is not \"CFDARR1\\0\""), DisplayPath, 0);
	}

	// --- version @8/@10, endian marker @12.
	//
	// NOTE THE ORDER, which differs from CVF: CVA puts major/minor at 8/10 and
	// the endian marker at 12, where CVF puts headerBytes at 8 and the marker at
	// 16. Reusing CVF's field order here yields a header full of plausible
	// nonsense rather than an error, so the two parsers stay separate.
	if (!Cursor.ReadUInt16(OutHeader.MajorVersion) || !Cursor.ReadUInt16(OutHeader.MinorVersion))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated version"), DisplayPath, 8);
	}

	uint32 EndianMarker = 0;
	if (!Cursor.ReadUInt32(EndianMarker))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated endian marker"), DisplayPath, 12);
	}
	// Rejected, never byte-swapped (section 6.1 / 4.1). Swapping would produce
	// numbers, and confidently wrong numbers are worse than a refusal to load.
	if (EndianMarker != CFDViz::EndianMarker)
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedEndianness,
			FString::Printf(TEXT("byte order not supported: endian marker is 0x%08X, expected 0x%08X"),
				EndianMarker, CFDViz::EndianMarker),
			DisplayPath, 12);
	}

	// The major version gate comes after the marker: a foreign-endian file would
	// report a garbage version, and "byte order not supported" is the more
	// actionable of the two messages. A newer MINOR version is accepted, because
	// minor versions are additive by definition (rule 1.4).
	if (!CFDViz::IsSupportedMajorVersion(OutHeader.MajorVersion))
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedVersion,
			FString::Printf(TEXT("CVA major version %u is not supported; this reader implements %u.x"),
				OutHeader.MajorVersion, CFDViz::SupportedMajorVersion),
			DisplayPath, 8);
	}

	if (!Cursor.ReadUInt32(OutHeader.Flags))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated flags"), DisplayPath, 16);
	}
	// An unknown flag bit means the file uses a feature this reader does not
	// implement. Ignoring it is how a future extension gets silently misread as
	// 1.0 data - the failure would surface as wrong values, not as an error.
	if ((OutHeader.Flags & ~CFDViz::CvaFlags::Known) != 0)
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
			FString::Printf(TEXT("unknown CVA flag bits 0x%08X"), OutHeader.Flags & ~CFDViz::CvaFlags::Known),
			DisplayPath, 16);
	}

	if (!Cursor.ReadUInt32(OutHeader.HeaderBytes))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated headerBytes"), DisplayPath, 20);
	}
	if (OutHeader.HeaderBytes != CFDViz::CvaHeaderBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
			FString::Printf(TEXT("headerBytes is %u, expected %lld"),
				OutHeader.HeaderBytes, CFDViz::CvaHeaderBytes),
			DisplayPath, 20);
	}

	if (!Cursor.ReadUInt32(OutHeader.FrameIndex)
		|| !Cursor.ReadUInt32(OutHeader.FieldNumericId)
		|| !Cursor.ReadDouble(OutHeader.SimulationTime))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated frame fields"), DisplayPath, 24);
	}

	uint64 ValueCount = 0;
	if (!Cursor.ReadUInt64(ValueCount))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated valueCount"), DisplayPath, 40);
	}
	// Clamped to int64's positive range before it is used in any arithmetic, so
	// a hostile 2^63 count cannot become a negative size downstream.
	if (ValueCount > static_cast<uint64>(MAX_int64))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
			FString::Printf(TEXT("valueCount %llu is impossibly large"), ValueCount), DisplayPath, 40);
	}
	OutHeader.ValueCount = static_cast<int64>(ValueCount);

	// --- the four packed enum bytes @48..@51, no padding between them.
	uint8 ComponentCount = 0;
	uint8 DataTypeCode = 0;
	uint8 AssociationCode = 0;
	uint8 CodecCode = 0;
	if (!Cursor.ReadUInt8(ComponentCount) || !Cursor.ReadUInt8(DataTypeCode)
		|| !Cursor.ReadUInt8(AssociationCode) || !Cursor.ReadUInt8(CodecCode))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated enum block"), DisplayPath, 48);
	}

	OutHeader.ComponentCount = static_cast<int32>(ComponentCount);
	if (!CFDViz::IsValidCvaComponentCount(OutHeader.ComponentCount))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
			FString::Printf(TEXT("componentCount %d is not 1 (scalar), 3 (vector), 6 (symmetric tensor) or 9 (full tensor)"),
				OutHeader.ComponentCount),
			DisplayPath, 48);
	}

	if (!TryDataTypeFromCode(DataTypeCode, OutHeader.DataType))
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedDataType,
			FString::Printf(TEXT("unknown dataType %u"), DataTypeCode), DisplayPath, 49);
	}
	// CVA and CVF share the enum but not its domain: uint8 (3) is reserved for
	// CVF and CVA does not offer it, while float64 (4) is CVA-only. Accepting
	// the wrong one would misread the payload stride by a factor of four or
	// eight and produce a full array of plausible garbage.
	if (!IsDataTypeSupportedInCva(OutHeader.DataType))
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedDataType,
			FString::Printf(TEXT("dataType %s is not offered by CVA (uint8 is reserved for CVF)"),
				DataTypeToString(OutHeader.DataType)),
			DisplayPath, 49);
	}

	if (!TryAssociationFromCode(AssociationCode, OutHeader.Association))
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedAssociation,
			FString::Printf(TEXT("unknown association %u"), AssociationCode), DisplayPath, 50);
	}
	if (!TryCodecFromCode(CodecCode, OutHeader.Codec))
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedCodec,
			FString::Printf(TEXT("unknown codec %u"), CodecCode), DisplayPath, 51);
	}

	if (!Cursor.ReadUInt32(OutHeader.PayloadCrc32C))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated payloadCrc32c"), DisplayPath, 52);
	}

	uint64 PayloadOffset = 0;
	uint64 CompressedBytes = 0;
	uint64 UncompressedBytes = 0;
	if (!Cursor.ReadUInt64(PayloadOffset) || !Cursor.ReadUInt64(CompressedBytes) || !Cursor.ReadUInt64(UncompressedBytes))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated payload block"), DisplayPath, 56);
	}
	if (PayloadOffset > static_cast<uint64>(MAX_int64)
		|| CompressedBytes > static_cast<uint64>(MAX_int64)
		|| UncompressedBytes > static_cast<uint64>(MAX_int64))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
			TEXT("payload offset or size is impossibly large"), DisplayPath, 56);
	}
	OutHeader.PayloadOffset = static_cast<int64>(PayloadOffset);
	OutHeader.CompressedBytes = static_cast<int64>(CompressedBytes);
	OutHeader.UncompressedBytes = static_cast<int64>(UncompressedBytes);

	if (!Cursor.ReadUInt32(OutHeader.HeaderCrc32C))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated headerCrc32c"), DisplayPath, 80);
	}

	// reserved @84 must be zero, same reasoning as the flag mask above.
	bool bReservedZero = false;
	if (!Cursor.ReadIsAllZero(4, bReservedZero))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated reserved field"), DisplayPath, 84);
	}
	if (!bReservedZero)
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
			TEXT("reserved bytes at offset 84 are not zero; this file uses a feature this reader does not implement"),
			DisplayPath, 84);
	}

	uint64 StatisticsOffset = 0;
	if (!Cursor.ReadUInt64(StatisticsOffset))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader, TEXT("truncated statisticsOffset"), DisplayPath, 88);
	}
	if (StatisticsOffset > static_cast<uint64>(MAX_int64))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
			TEXT("statisticsOffset is impossibly large"), DisplayPath, 88);
	}
	OutHeader.StatisticsOffset = static_cast<int64>(StatisticsOffset);

	// The CRC is verified last, after the structural checks, so a file that is
	// both corrupt and malformed reports the more specific problem. A stale CRC
	// on an otherwise-valid header still fails here.
	if (!VerifyCvaHeaderCrc(HeaderBytes.Left(CFDViz::CvaHeaderBytes), OutHeader.HeaderCrc32C))
	{
		return FCFDVizResult::Fail(ECFDVizError::HeaderCrcMismatch,
			FString::Printf(TEXT("header CRC mismatch: the file declares 0x%08X"), OutHeader.HeaderCrc32C),
			DisplayPath, CFDViz::CvaHeaderCrcOffset);
	}

	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizArrayReader::Read(
	const ICFDVizByteSource& Source,
	FCFDVizArrayData& OutArray,
	bool bVerifyCrc)
{
	const FString& Path = Source.GetDisplayPath();

	// Reset first: a caller that reuses one FCFDVizArrayData across files must
	// never end up with the previous file's payload beside this file's header.
	OutArray = FCFDVizArrayData();

	if (Source.GetSize() < CFDViz::CvaHeaderBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::FileTooSmall,
			FString::Printf(TEXT("file is %lld bytes; a CVA header alone is %lld"),
				Source.GetSize(), CFDViz::CvaHeaderBytes),
			Path, 0);
	}

	uint8 HeaderBytes[CFDViz::CvaHeaderBytes];
	if (!Source.Read(0, CFDViz::CvaHeaderBytes, HeaderBytes))
	{
		return FCFDVizResult::Fail(ECFDVizError::FileReadFailed, TEXT("could not read the CVA header"), Path, 0);
	}

	FCFDVizResult Result = ParseHeader(
		TArrayView<const uint8>(HeaderBytes, CFDViz::CvaHeaderBytes), Path, OutArray.Header);
	if (!Result.IsOk())
	{
		return Result;
	}

	const FCFDVizArrayHeader& Header = OutArray.Header;

	// --- statistics (section 6.4), before the payload so a truncated file
	// reports the first thing that is actually missing.
	if (Header.HasFrameStatistics() || Header.HasGlobalStatistics())
	{
		const int32 SectionCount = (Header.HasFrameStatistics() ? 1 : 0) + (Header.HasGlobalStatistics() ? 1 : 0);
		const int64 SectionBytes = FCFDVizArrayStatistics::GetSectionBytes(Header.ComponentCount);
		const int64 TotalBytes = SectionBytes * static_cast<int64>(SectionCount);

		if (Header.StatisticsOffset < CFDViz::CvaHeaderBytes)
		{
			return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
				FString::Printf(TEXT("statisticsOffset %lld overlaps the %lld byte header"),
					Header.StatisticsOffset, CFDViz::CvaHeaderBytes),
				Path, 88);
		}
		if (!Source.Contains(Header.StatisticsOffset, TotalBytes))
		{
			return FCFDVizResult::Fail(ECFDVizError::PayloadOutOfBounds,
				FString::Printf(TEXT("statistics need %lld bytes at offset %lld but the file is %lld bytes"),
					TotalBytes, Header.StatisticsOffset, Source.GetSize()),
				Path, 88);
		}

		TArray<uint8> StatsBytes;
		StatsBytes.SetNumUninitialized(static_cast<int32>(TotalBytes));
		if (!Source.Read(Header.StatisticsOffset, TotalBytes, StatsBytes.GetData()))
		{
			return FCFDVizResult::Fail(ECFDVizError::FileReadFailed,
				TEXT("could not read the statistics sections"), Path, Header.StatisticsOffset);
		}

		// Frame first, then global (section 6.4). With both flags set the two
		// sections are the same size and often similar in content, so reading
		// them in the wrong order swaps per-frame and case-global ranges without
		// any error - a viewer would just scale every frame slightly wrong.
		int64 SectionCursor = 0;
		if (Header.HasFrameStatistics())
		{
			FCFDVizArrayStatistics Frame;
			if (!ParseStatisticsSection(
					TArrayView<const uint8>(StatsBytes.GetData() + SectionCursor, SectionBytes),
					Header.ComponentCount, Frame))
			{
				return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
					TEXT("frame statistics section is malformed"), Path, Header.StatisticsOffset + SectionCursor);
			}
			// The section's own valueCount must agree with the header's.
			// Trusting a disagreement would scale a field by statistics computed
			// over some other number of entities.
			if (Frame.ValueCount != Header.ValueCount)
			{
				return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
					FString::Printf(TEXT("frame statistics cover %lld entities but the header declares valueCount %lld"),
						Frame.ValueCount, Header.ValueCount),
					Path, Header.StatisticsOffset + SectionCursor);
			}
			OutArray.FrameStatistics = MoveTemp(Frame);
			SectionCursor += SectionBytes;
		}
		if (Header.HasGlobalStatistics())
		{
			FCFDVizArrayStatistics Global;
			if (!ParseStatisticsSection(
					TArrayView<const uint8>(StatsBytes.GetData() + SectionCursor, SectionBytes),
					Header.ComponentCount, Global))
			{
				return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
					TEXT("global statistics section is malformed"), Path, Header.StatisticsOffset + SectionCursor);
			}
			// Global statistics deliberately are NOT checked against the
			// header's valueCount: they cover every frame in the case, so their
			// count is the sum across frames and is expected to be larger.
			OutArray.GlobalStatistics = MoveTemp(Global);
		}
	}

	// --- payload. The offset check is here rather than inside DecodePayload
	// because "overlaps the header" is a structural error specific to this
	// container, while the decoder's checks are about the payload itself.
	if (Header.PayloadOffset < CFDViz::CvaHeaderBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidHeader,
			FString::Printf(TEXT("payloadOffset %lld overlaps the %lld byte header"),
				Header.PayloadOffset, CFDViz::CvaHeaderBytes),
			Path, 56);
	}

	CFDViz::FPayloadSpec Spec;
	Spec.Offset = Header.PayloadOffset;
	Spec.CompressedBytes = Header.CompressedBytes;
	Spec.UncompressedBytes = Header.UncompressedBytes;
	Spec.PayloadCrc32C = Header.PayloadCrc32C;
	Spec.ValueCount = Header.ValueCount;
	Spec.ComponentCount = Header.ComponentCount;
	Spec.DataType = Header.DataType;
	Spec.Codec = Header.Codec;
	Spec.Context = TEXT("the array payload");

	// The shared decoder, the same one CVF bricks go through - section 6.3 says
	// the rules mirror 4.4 exactly, and two decoders meant to behave identically
	// eventually would not.
	Result = CFDViz::DecodePayload(Source, Spec, OutArray.Bytes, bVerifyCrc);
	if (!Result.IsOk())
	{
		// Nothing partial is handed back: a caller must not be able to mistake a
		// half-populated array for data.
		OutArray.Bytes.Reset();
		return Result;
	}

	return FCFDVizResult::Ok();
}
