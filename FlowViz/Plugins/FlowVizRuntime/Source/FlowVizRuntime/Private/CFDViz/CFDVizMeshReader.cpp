// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizMeshReader.h"

#include "CFDViz/CFDVizCrc32C.h"
#include "FlowVizRuntime.h"

#include "HAL/FileManager.h"
#include "Serialization/Archive.h"
#include "Templates/UniquePtr.h"

/**
 * Bulk arrays are copied straight out of the file bytes rather than decoded
 * element by element, which is only correct because the stored byte order and
 * the host byte order agree. The fixed header fields below are decoded manually
 * and are endian-independent; these arrays are not, so the assumption is made a
 * compile-time error rather than a runtime surprise on a future target.
 */
static_assert(PLATFORM_LITTLE_ENDIAN, "CFDViz stores little-endian data; a big-endian target needs per-element byte swaps in the array readers below.");

/**
 * Positions, normals and the double-precision copy are read directly into their
 * final containers, so those containers must have exactly the file's layout with
 * no padding. UE guarantees this - the same assumption underpins every vertex
 * buffer upload in the engine - but stating it here means a change to the vector
 * types breaks the build instead of silently misreading every mesh.
 */
static_assert(sizeof(FVector3f) == 12, "FVector3f must be three tightly packed floats to be read directly from file bytes.");
static_assert(sizeof(FVector) == 24, "FVector must be three tightly packed doubles to be read directly from file bytes.");

namespace
{
	/* ---------------------------------------------------------------------- */
	/* Header field offsets (format section 5.1)                               */
	/*                                                                          */
	/* Named constants rather than literals at the parse site, because they are  */
	/* used twice: once to decode the field and once to report the byte offset a  */
	/* malformed file went wrong at, which section 10 requires. A literal typed   */
	/* correctly in one place and wrongly in the other produces an error message  */
	/* that points at the wrong field.                                           */
	/* ---------------------------------------------------------------------- */
	constexpr int64 CvmOffsetMagic = 0;
	constexpr int64 CvmOffsetMajorVersion = 8;
	constexpr int64 CvmOffsetMinorVersion = 10;
	constexpr int64 CvmOffsetEndianMarker = 12;
	constexpr int64 CvmOffsetFlags = 16;
	constexpr int64 CvmOffsetHeaderBytes = 20;
	constexpr int64 CvmOffsetVertexCount = 24;
	constexpr int64 CvmOffsetTriangleCount = 32;
	constexpr int64 CvmOffsetPositionsOffset = 40;
	constexpr int64 CvmOffsetNormalsOffset = 48;
	constexpr int64 CvmOffsetIndicesOffset = 56;
	constexpr int64 CvmOffsetPatchIdsOffset = 64;
	constexpr int64 CvmOffsetNodeIdsOffset = 72;
	constexpr int64 CvmOffsetHeaderCrc = CFDViz::CvmHeaderCrcOffset; // 80
	constexpr int64 CvmOffsetReserved = 84;
	constexpr int64 CvmReservedBytes = 12;

	static_assert(CvmOffsetReserved + CvmReservedBytes == CFDViz::CvmHeaderBytes, "The CVM header must be exactly 96 bytes with no gap after the reserved block.");
	static_assert(CFDViz::HeaderCrcFieldBytes == 4, "The header CRC field is a uint32.");

	/** Longest list of patch IDs quoted back in an error, so a mesh with thousands does not produce an unreadable message. */
	constexpr int32 MaxPatchIdsInMessage = 16;

	/* ---------------------------------------------------------------------- */
	/* Little-endian scalar decode                                             */
	/*                                                                          */
	/* Assembled from bytes rather than memcpy'd, so these are correct on any     */
	/* host and never assume the buffer is aligned. The spec calls for manual      */
	/* field-by-field serialization precisely because a struct memcpy would carry  */
	/* compiler-chosen padding into the format.                                    */
	/* ---------------------------------------------------------------------- */
	uint16 ReadUInt16LE(const uint8* Base, int64 Offset)
	{
		return static_cast<uint16>(
			static_cast<uint16>(Base[Offset]) |
			(static_cast<uint16>(Base[Offset + 1]) << 8));
	}

	uint32 ReadUInt32LE(const uint8* Base, int64 Offset)
	{
		return static_cast<uint32>(Base[Offset])
			| (static_cast<uint32>(Base[Offset + 1]) << 8)
			| (static_cast<uint32>(Base[Offset + 2]) << 16)
			| (static_cast<uint32>(Base[Offset + 3]) << 24);
	}

	uint64 ReadUInt64LE(const uint8* Base, int64 Offset)
	{
		uint64 Value = 0;
		for (int64 Byte = 7; Byte >= 0; --Byte)
		{
			Value = (Value << 8) | static_cast<uint64>(Base[Offset + Byte]);
		}
		return Value;
	}

	/** Format a uint64 for a message. Wrapped so the printf cast is written once. */
	FString UInt64ToString(uint64 Value)
	{
		return FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(Value));
	}

	/**
	 * Narrow a stored uint64 to the int64 the rest of the reader does arithmetic
	 * in. Returns false for a value beyond int64, which no real file can contain
	 * and which would otherwise become a negative offset that compares as "well
	 * inside the file".
	 */
	bool TryToInt64(uint64 Value, int64& OutValue)
	{
		if (Value > static_cast<uint64>(TNumericLimits<int64>::Max()))
		{
			return false;
		}
		OutValue = static_cast<int64>(Value);
		return true;
	}

	/**
	 * Prove an array's byte span exists inside the real file before anything is
	 * allocated for it (format rule 1.5, engineering rule 2).
	 *
	 * This is the whole defence against a hostile header: every allocation below
	 * is sized from a count that has already been shown to be backed by real bytes
	 * on disk, so a header claiming four billion vertices fails here rather than
	 * in the allocator.
	 *
	 * @param ArrayName         Names the array in the error, e.g. "nodeIds".
	 * @param HeaderFieldOffset Offset of the *offset field* in the header, so the
	 *                          error points at the field to inspect.
	 * @param FileSize          The size of the file as it actually is, never a size
	 *                          the file claims for itself.
	 */
	FCFDVizResult ValidateArraySpan(
		const TCHAR* ArrayName,
		uint64 StoredOffset,
		int64 HeaderFieldOffset,
		int64 ElementCount,
		int64 BytesPerElement,
		int64 FileSize,
		const FString& Path,
		int64& OutOffset,
		int64& OutByteCount)
	{
		OutOffset = 0;
		OutByteCount = 0;

		int64 ByteCount = 0;
		if (!CFDViz::TryMultiply(ElementCount, BytesPerElement, ByteCount))
		{
			return FCFDVizResult::Fail(
				ECFDVizError::AllocationTooLarge,
				FString::Printf(
					TEXT("%s length overflows: %lld elements of %lld bytes cannot be addressed"),
					ArrayName, static_cast<long long>(ElementCount), static_cast<long long>(BytesPerElement)),
				Path,
				HeaderFieldOffset);
		}

		int64 Offset = 0;
		if (!TryToInt64(StoredOffset, Offset))
		{
			return FCFDVizResult::Fail(
				ECFDVizError::PayloadOutOfBounds,
				FString::Printf(
					TEXT("%s offset %s is beyond any addressable file position; the header is corrupt"),
					ArrayName, *UInt64ToString(StoredOffset)),
				Path,
				HeaderFieldOffset);
		}

		// An empty array is allowed to sit at offset 0 - it occupies no bytes, so
		// it cannot overlap anything. A non-empty one inside the header would mean
		// the array and the header describe the same bytes.
		if (ByteCount > 0 && Offset < CFDViz::CvmHeaderBytes)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::InvalidHeader,
				FString::Printf(
					TEXT("%s offset %lld overlaps the %lld-byte header"),
					ArrayName, static_cast<long long>(Offset), static_cast<long long>(CFDViz::CvmHeaderBytes)),
				Path,
				HeaderFieldOffset);
		}

		// Written as a subtraction rather than Offset + ByteCount <= FileSize so
		// the sum can never overflow into a value that passes the test.
		if (Offset > FileSize || ByteCount > FileSize - Offset)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::PayloadOutOfBounds,
				FString::Printf(
					TEXT("%s needs %lld bytes at offset %lld, but the file is only %lld bytes; it is truncated or the header is corrupt"),
					ArrayName,
					static_cast<long long>(ByteCount),
					static_cast<long long>(Offset),
					static_cast<long long>(FileSize)),
				Path,
				HeaderFieldOffset);
		}

		OutOffset = Offset;
		OutByteCount = ByteCount;
		return FCFDVizResult::Ok();
	}
}

/* -------------------------------------------------------------------------- */
/* Byte sources                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Random-access bytes behind a load.
 *
 * The abstraction earns its keep by making LoadFromFile and LoadFromMemory run
 * the identical validation path: a test can build a deliberately malformed mesh
 * in memory and be certain it exercised the same bounds checks a file would,
 * rather than a parallel copy of them that could drift.
 */
class FCFDVizMeshByteSource
{
public:
	virtual ~FCFDVizMeshByteSource() = default;

	/** Size of the underlying data as it actually is. Never a size taken from the data itself. */
	virtual int64 GetSize() const = 0;

	/**
	 * Copy Size bytes from Offset into Dest.
	 *
	 * Re-checks its own bounds even though every caller has already validated the
	 * span, because this is the last point before a write into a raw pointer and
	 * a defence that only exists at the call site is one refactor from not
	 * existing.
	 *
	 * @return false on an out-of-range request or an I/O failure.
	 */
	virtual bool Read(int64 Offset, void* Dest, int64 Size) = 0;
};

namespace
{
	/** Byte source over a caller-owned buffer. Does not copy the buffer, so it must outlive the load. */
	class FCFDVizMeshMemorySource final : public FCFDVizMeshByteSource
	{
	public:
		explicit FCFDVizMeshMemorySource(TArrayView<const uint8> InBytes)
			: Bytes(InBytes)
		{
		}

		virtual int64 GetSize() const override
		{
			return Bytes.Num();
		}

		virtual bool Read(int64 Offset, void* Dest, int64 Size) override
		{
			if (Size <= 0)
			{
				return true;
			}
			if (Dest == nullptr || Offset < 0 || Offset > Bytes.Num() || Size > Bytes.Num() - Offset)
			{
				return false;
			}
			FMemory::Memcpy(Dest, Bytes.GetData() + Offset, static_cast<SIZE_T>(Size));
			return true;
		}

	private:
		TArrayView<const uint8> Bytes;
	};

	/**
	 * Byte source over an open file.
	 *
	 * Seeks and reads only the spans the header describes rather than pulling the
	 * whole file into memory. For a mesh the difference is modest; the point is
	 * that the reader never holds a buffer whose size a header asked for.
	 */
	class FCFDVizMeshFileSource final : public FCFDVizMeshByteSource
	{
	public:
		explicit FCFDVizMeshFileSource(TUniquePtr<FArchive> InArchive)
			: Archive(MoveTemp(InArchive))
			, Size(Archive.IsValid() ? Archive->TotalSize() : 0)
		{
		}

		virtual int64 GetSize() const override
		{
			return Size;
		}

		virtual bool Read(int64 Offset, void* Dest, int64 ReadSize) override
		{
			if (ReadSize <= 0)
			{
				return true;
			}
			if (!Archive.IsValid() || Dest == nullptr || Offset < 0 || Offset > Size || ReadSize > Size - Offset)
			{
				return false;
			}
			Archive->Seek(Offset);
			Archive->Serialize(Dest, ReadSize);
			return !Archive->IsError();
		}

	private:
		TUniquePtr<FArchive> Archive;
		int64 Size = 0;
	};
}

/* -------------------------------------------------------------------------- */
/* FCFDVizMeshHeader                                                            */
/* -------------------------------------------------------------------------- */

uint32 FCFDVizMeshHeader::ComputeHeaderCrc(TArrayView<const uint8> HeaderBytes)
{
	if (HeaderBytes.Num() < CFDViz::CvmHeaderBytes)
	{
		return 0;
	}

	const uint8* const Bytes = HeaderBytes.GetData();

	// Section 5.1: the CRC covers all 96 bytes with [80, 84) treated as zero.
	// Fed to the CRC in three chunks rather than by copying the header and
	// blanking the field, so no scratch buffer is needed; CFDViz::Crc32C::Compute
	// handles its own pre/post conditioning when chained.
	const uint8 ZeroedCrcField[4] = { 0, 0, 0, 0 };

	uint32 Crc = CFDViz::Crc32C::Compute(Bytes, CvmOffsetHeaderCrc);
	Crc = CFDViz::Crc32C::Compute(ZeroedCrcField, CFDViz::HeaderCrcFieldBytes, Crc);
	Crc = CFDViz::Crc32C::Compute(
		Bytes + CvmOffsetHeaderCrc + CFDViz::HeaderCrcFieldBytes,
		CFDViz::CvmHeaderBytes - CvmOffsetHeaderCrc - CFDViz::HeaderCrcFieldBytes,
		Crc);
	return Crc;
}

FCFDVizResult FCFDVizMeshHeader::Parse(
	TArrayView<const uint8> HeaderBytes,
	FCFDVizMeshHeader& OutHeader,
	const FString& FilePath,
	bool bVerifyCrc)
{
	if (HeaderBytes.Num() < CFDViz::CvmHeaderBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::FileTooSmall,
			FString::Printf(
				TEXT("file is %d bytes, shorter than the %lld-byte CVM header"),
				HeaderBytes.Num(), static_cast<long long>(CFDViz::CvmHeaderBytes)),
			FilePath,
			CvmOffsetMagic);
	}

	const uint8* const Bytes = HeaderBytes.GetData();

	// The rejection order below deliberately matches the Python reference, so a
	// file that is wrong in several ways at once is diagnosed identically by both
	// readers rather than each blaming a different field.
	if (FMemory::Memcmp(Bytes, CFDViz::CvmMagic, sizeof(CFDViz::CvmMagic)) != 0)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::BadMagic,
			TEXT("bad magic; expected 'CFDMESH1'. This is not a CVM mesh file."),
			FilePath,
			CvmOffsetMagic);
	}

	const uint32 EndianMarker = ReadUInt32LE(Bytes, CvmOffsetEndianMarker);
	if (EndianMarker != CFDViz::EndianMarker)
	{
		// Format rule 1.1: reject foreign byte order, never byte-swap it. A
		// swapped read would succeed and produce geometry that is subtly wrong.
		return FCFDVizResult::Fail(
			ECFDVizError::UnsupportedEndianness,
			FString::Printf(
				TEXT("byte order not supported: endianMarker is 0x%08X, expected 0x%08X. CFDViz is little-endian only."),
				EndianMarker, CFDViz::EndianMarker),
			FilePath,
			CvmOffsetEndianMarker);
	}

	const uint16 MajorVersion = ReadUInt16LE(Bytes, CvmOffsetMajorVersion);
	const uint16 MinorVersion = ReadUInt16LE(Bytes, CvmOffsetMinorVersion);
	if (!CFDViz::IsSupportedMajorVersion(MajorVersion))
	{
		// A newer minor is accepted (format rule 1.4); a different major is not.
		return FCFDVizResult::Fail(
			ECFDVizError::UnsupportedVersion,
			FString::Printf(
				TEXT("unsupported major version %u; this reader implements CVM %u.x"),
				static_cast<uint32>(MajorVersion), static_cast<uint32>(CFDViz::SupportedMajorVersion)),
			FilePath,
			CvmOffsetMajorVersion);
	}

	const uint32 DeclaredHeaderBytes = ReadUInt32LE(Bytes, CvmOffsetHeaderBytes);
	if (static_cast<int64>(DeclaredHeaderBytes) != CFDViz::CvmHeaderBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("headerBytes is %u, expected %lld"),
				DeclaredHeaderBytes, static_cast<long long>(CFDViz::CvmHeaderBytes)),
			FilePath,
			CvmOffsetHeaderBytes);
	}

	const uint32 Flags = ReadUInt32LE(Bytes, CvmOffsetFlags);
	const uint32 UnknownFlags = Flags & ~CFDViz::Cvm::KnownFlags;
	if (UnknownFlags != 0)
	{
		// An unknown bit may change an array's layout or add an array this reader
		// does not know to skip. Reading on "as if the bit were clear" would
		// silently produce wrong geometry, so the file is rejected instead.
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidHeader,
			FString::Printf(
				TEXT("unknown flag bits 0x%08X set; this file uses a CVM feature this reader does not implement (flags=0x%08X)"),
				UnknownFlags, Flags),
			FilePath,
			CvmOffsetFlags);
	}

	for (int64 Index = 0; Index < CvmReservedBytes; ++Index)
	{
		if (Bytes[CvmOffsetReserved + Index] != 0)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::InvalidHeader,
				FString::Printf(
					TEXT("reserved header bytes [%lld, %lld) are not zero"),
					static_cast<long long>(CvmOffsetReserved), static_cast<long long>(CFDViz::CvmHeaderBytes)),
				FilePath,
				CvmOffsetReserved + Index);
		}
	}

	const uint32 StoredCrc = ReadUInt32LE(Bytes, CvmOffsetHeaderCrc);
	if (bVerifyCrc)
	{
		const uint32 ComputedCrc = ComputeHeaderCrc(HeaderBytes);
		if (ComputedCrc != StoredCrc)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::HeaderCrcMismatch,
				FString::Printf(
					TEXT("header CRC-32C mismatch: stored 0x%08X, computed 0x%08X. The header is corrupt."),
					StoredCrc, ComputedCrc),
				FilePath,
				CvmOffsetHeaderCrc);
		}
	}

	FCFDVizMeshHeader Parsed;
	Parsed.MajorVersion = MajorVersion;
	Parsed.MinorVersion = MinorVersion;
	Parsed.Flags = Flags;
	Parsed.VertexCount = ReadUInt64LE(Bytes, CvmOffsetVertexCount);
	Parsed.TriangleCount = ReadUInt64LE(Bytes, CvmOffsetTriangleCount);
	Parsed.PositionsOffset = ReadUInt64LE(Bytes, CvmOffsetPositionsOffset);
	Parsed.NormalsOffset = ReadUInt64LE(Bytes, CvmOffsetNormalsOffset);
	Parsed.IndicesOffset = ReadUInt64LE(Bytes, CvmOffsetIndicesOffset);
	Parsed.PatchIdsOffset = ReadUInt64LE(Bytes, CvmOffsetPatchIdsOffset);
	Parsed.NodeIdsOffset = ReadUInt64LE(Bytes, CvmOffsetNodeIdsOffset);
	Parsed.HeaderCrc32C = StoredCrc;

	// Section 5.2: an absent array's offset MUST be 0. A stale non-zero offset
	// behind a clear bit means the writer was buggy; honouring it later would
	// read whatever happens to sit at that offset and call it geometry.
	struct FOptionalArray
	{
		const TCHAR* Name;
		uint64 Offset;
		bool bPresent;
		int64 HeaderFieldOffset;
	};
	const FOptionalArray OptionalArrays[] = {
		{ TEXT("normalsOffset"), Parsed.NormalsOffset, Parsed.HasNormals(), CvmOffsetNormalsOffset },
		{ TEXT("patchIdsOffset"), Parsed.PatchIdsOffset, Parsed.HasPatchIds(), CvmOffsetPatchIdsOffset },
		{ TEXT("nodeIdsOffset"), Parsed.NodeIdsOffset, Parsed.HasNodeIds(), CvmOffsetNodeIdsOffset },
	};
	for (const FOptionalArray& Array : OptionalArrays)
	{
		if (!Array.bPresent && Array.Offset != 0)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::InvalidHeader,
				FString::Printf(
					TEXT("%s is %s but its presence flag is clear; the spec requires 0 for absent arrays"),
					Array.Name, *UInt64ToString(Array.Offset)),
				FilePath,
				Array.HeaderFieldOffset);
		}
	}

	OutHeader = Parsed;
	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* FCFDVizMeshReader - loading                                                  */
/* -------------------------------------------------------------------------- */

void FCFDVizMeshReader::Reset()
{
	Header = FCFDVizMeshHeader();
	SourcePath.Reset();
	Positions.Reset();
	Normals.Reset();
	Indices.Reset();
	PatchIds.Reset();
	NodeIds.Reset();
	DoublePositions.Reset();
	bLoaded = false;
	bHasNormals = false;
	bHasPatchIds = false;
	bHasNodeIds = false;
}

FCFDVizResult FCFDVizMeshReader::LoadFromFile(const FString& Path, const FCFDVizMeshLoadOptions& Options)
{
	Reset();

	IFileManager& FileManager = IFileManager::Get();

	// CreateFileReader returns null for both "no such file" and "cannot open it",
	// and those need different fixes - one is a broken manifest path, the other a
	// permissions or device problem - so they are distinguished before reporting.
	TUniquePtr<FArchive> Archive(FileManager.CreateFileReader(*Path));
	if (!Archive.IsValid())
	{
		const bool bExists = FileManager.FileExists(*Path);
		return FCFDVizResult::Fail(
			bExists ? ECFDVizError::FileReadFailed : ECFDVizError::FileNotFound,
			bExists ? TEXT("cannot open mesh for reading") : TEXT("mesh file does not exist"),
			Path);
	}

	FCFDVizMeshFileSource Source(MoveTemp(Archive));
	return LoadInternal(Source, Path, Options);
}

FCFDVizResult FCFDVizMeshReader::LoadFromMemory(
	TArrayView<const uint8> FileBytes,
	const FString& DiagnosticPath,
	const FCFDVizMeshLoadOptions& Options)
{
	Reset();

	FCFDVizMeshMemorySource Source(FileBytes);
	return LoadInternal(Source, DiagnosticPath, Options);
}

FCFDVizResult FCFDVizMeshReader::LoadInternal(
	FCFDVizMeshByteSource& Source,
	const FString& Path,
	const FCFDVizMeshLoadOptions& Options)
{
	Reset();

	const int64 FileSize = Source.GetSize();
	if (FileSize < CFDViz::CvmHeaderBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::FileTooSmall,
			FString::Printf(
				TEXT("file is %lld bytes, shorter than the %lld-byte CVM header"),
				static_cast<long long>(FileSize), static_cast<long long>(CFDViz::CvmHeaderBytes)),
			Path,
			0);
	}

	uint8 HeaderBytes[CFDViz::CvmHeaderBytes];
	if (!Source.Read(0, HeaderBytes, CFDViz::CvmHeaderBytes))
	{
		return FCFDVizResult::Fail(ECFDVizError::FileReadFailed, TEXT("failed to read the 96-byte CVM header"), Path, 0);
	}

	FCFDVizMeshHeader ParsedHeader;
	{
		const FCFDVizResult HeaderResult = FCFDVizMeshHeader::Parse(
			TArrayView<const uint8>(HeaderBytes, CFDViz::CvmHeaderBytes),
			ParsedHeader,
			Path,
			Options.bVerifyHeaderCrc);
		if (!HeaderResult.IsOk())
		{
			return HeaderResult;
		}
	}

	/* -- counts ---------------------------------------------------------- */

	int64 VertexCount = 0;
	int64 TriangleCount = 0;
	if (!TryToInt64(ParsedHeader.VertexCount, VertexCount))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::AllocationTooLarge,
			FString::Printf(TEXT("vertexCount %s is not addressable"), *UInt64ToString(ParsedHeader.VertexCount)),
			Path,
			CvmOffsetVertexCount);
	}
	if (!TryToInt64(ParsedHeader.TriangleCount, TriangleCount))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::AllocationTooLarge,
			FString::Printf(TEXT("triangleCount %s is not addressable"), *UInt64ToString(ParsedHeader.TriangleCount)),
			Path,
			CvmOffsetTriangleCount);
	}

	/* -- bounds checks, before any allocation ----------------------------- */

	// Every present array is validated, including ones this caller asked not to
	// load: a file whose nodeIds run off the end is truncated whether or not this
	// caller wanted nodeIds, and reporting that honestly is the point of the
	// validator. Only the copy below is conditional.
	int64 PositionsOffset = 0;
	int64 PositionsBytes = 0;
	int64 IndicesOffset = 0;
	int64 IndicesBytes = 0;
	int64 NormalsOffset = 0;
	int64 NormalsBytes = 0;
	int64 PatchIdsOffset = 0;
	int64 PatchIdsBytes = 0;
	int64 NodeIdsOffset = 0;
	int64 NodeIdsBytes = 0;

	{
		int64 PositionComponents = 0;
		if (!CFDViz::TryMultiply(VertexCount, CFDViz::Cvm::ComponentsPerVector, PositionComponents))
		{
			return FCFDVizResult::Fail(
				ECFDVizError::AllocationTooLarge,
				FString::Printf(TEXT("vertexCount %lld overflows when expanded to XYZ components"), static_cast<long long>(VertexCount)),
				Path,
				CvmOffsetVertexCount);
		}

		int64 IndexEntries = 0;
		if (!CFDViz::TryMultiply(TriangleCount, CFDViz::Cvm::IndicesPerTriangle, IndexEntries))
		{
			return FCFDVizResult::Fail(
				ECFDVizError::AllocationTooLarge,
				FString::Printf(TEXT("triangleCount %lld overflows when expanded to corner indices"), static_cast<long long>(TriangleCount)),
				Path,
				CvmOffsetTriangleCount);
		}

		FCFDVizResult SpanResult = ValidateArraySpan(
			TEXT("positions"), ParsedHeader.PositionsOffset, CvmOffsetPositionsOffset,
			PositionComponents, ParsedHeader.GetPositionComponentBytes(),
			FileSize, Path, PositionsOffset, PositionsBytes);
		if (!SpanResult.IsOk())
		{
			return SpanResult;
		}

		SpanResult = ValidateArraySpan(
			TEXT("indices"), ParsedHeader.IndicesOffset, CvmOffsetIndicesOffset,
			IndexEntries, CFDViz::Cvm::IndexBytes,
			FileSize, Path, IndicesOffset, IndicesBytes);
		if (!SpanResult.IsOk())
		{
			return SpanResult;
		}

		if (ParsedHeader.HasNormals())
		{
			SpanResult = ValidateArraySpan(
				TEXT("normals"), ParsedHeader.NormalsOffset, CvmOffsetNormalsOffset,
				PositionComponents, CFDViz::Cvm::NormalComponentBytes,
				FileSize, Path, NormalsOffset, NormalsBytes);
			if (!SpanResult.IsOk())
			{
				return SpanResult;
			}
		}

		if (ParsedHeader.HasPatchIds())
		{
			// One uint32 per TRIANGLE. Sizing this per vertex is the classic CVM
			// bug: on a closed mesh the two counts are close enough that the file
			// still loads and every patch lands on the wrong triangles.
			SpanResult = ValidateArraySpan(
				TEXT("patchIds"), ParsedHeader.PatchIdsOffset, CvmOffsetPatchIdsOffset,
				TriangleCount, CFDViz::Cvm::PatchIdBytes,
				FileSize, Path, PatchIdsOffset, PatchIdsBytes);
			if (!SpanResult.IsOk())
			{
				return SpanResult;
			}
		}

		if (ParsedHeader.HasNodeIds())
		{
			// One uint64 per VERTEX - the mirror image of patchIds above.
			SpanResult = ValidateArraySpan(
				TEXT("nodeIds"), ParsedHeader.NodeIdsOffset, CvmOffsetNodeIdsOffset,
				VertexCount, CFDViz::Cvm::NodeIdBytes,
				FileSize, Path, NodeIdsOffset, NodeIdsBytes);
			if (!SpanResult.IsOk())
			{
				return SpanResult;
			}
		}

		// TArray indexes with int32. The file-size checks above already make a
		// count this large impossible in any file small enough to exist, but the
		// limit is stated rather than assumed, so the failure is a named error
		// instead of a negative Num() deep inside an allocator.
		constexpr int64 MaxElements = static_cast<int64>(TNumericLimits<int32>::Max());
		if (VertexCount > MaxElements)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::AllocationTooLarge,
				FString::Printf(
					TEXT("vertexCount %lld exceeds the %lld vertices this reader can address"),
					static_cast<long long>(VertexCount), static_cast<long long>(MaxElements)),
				Path,
				CvmOffsetVertexCount);
		}
		if (IndexEntries > MaxElements)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::AllocationTooLarge,
				FString::Printf(
					TEXT("triangleCount %lld needs %lld corner indices, more than this reader can address"),
					static_cast<long long>(TriangleCount), static_cast<long long>(IndexEntries)),
				Path,
				CvmOffsetTriangleCount);
		}
	}

	/* -- reads ------------------------------------------------------------ */

	// Built into locals and moved into the object only once every check has
	// passed, so a rejected file can never leave a half-populated mesh behind for
	// a caller that ignored the returned result.
	const int32 VertexNum = static_cast<int32>(VertexCount);
	const int32 TriangleNum = static_cast<int32>(TriangleCount);
	const int32 IndexNum = TriangleNum * static_cast<int32>(CFDViz::Cvm::IndicesPerTriangle);

	TArray<FVector3f> LocalPositions;
	TArray<FVector> LocalDoublePositions;
	TArray<FVector3f> LocalNormals;
	TArray<uint32> LocalIndices;
	TArray<uint32> LocalPatchIds;
	TArray<uint64> LocalNodeIds;

	if (ParsedHeader.ArePositionsFloat64())
	{
		// PRECISION LOSS, and it is deliberate. Rendering wants FVector3f, so
		// float64 positions are narrowed on the way in: a domain measured in
		// kilometres keeps roughly centimetre resolution, which is invisible on
		// screen but matters to a probe or a measurement. Callers that do
		// arithmetic on coordinates set bRetainDoublePrecisionPositions and read
		// GetDoublePositions instead. Narrowing preserves NaN as NaN - values are
		// never coerced to zero (format rule 1.7) - though a NaN payload is not
		// preserved bit-for-bit across the width change, which is inherent to the
		// conversion and not something a reader can avoid.
		TArray<FVector> StoredPositions;
		StoredPositions.SetNumUninitialized(VertexNum);
		if (VertexNum > 0 && !Source.Read(PositionsOffset, StoredPositions.GetData(), PositionsBytes))
		{
			return FCFDVizResult::Fail(ECFDVizError::FileReadFailed, TEXT("failed to read the float64 positions array"), Path, PositionsOffset);
		}

		LocalPositions.SetNumUninitialized(VertexNum);
		for (int32 Vertex = 0; Vertex < VertexNum; ++Vertex)
		{
			const FVector& Stored = StoredPositions[Vertex];
			LocalPositions[Vertex] = FVector3f(
				static_cast<float>(Stored.X),
				static_cast<float>(Stored.Y),
				static_cast<float>(Stored.Z));
		}

		if (Options.bRetainDoublePrecisionPositions)
		{
			LocalDoublePositions = MoveTemp(StoredPositions);
		}
	}
	else
	{
		// float32 is already the render precision, so the file bytes land straight
		// in the final array - no conversion, and NaN survives bit-exactly.
		LocalPositions.SetNumUninitialized(VertexNum);
		if (VertexNum > 0 && !Source.Read(PositionsOffset, LocalPositions.GetData(), PositionsBytes))
		{
			return FCFDVizResult::Fail(ECFDVizError::FileReadFailed, TEXT("failed to read the positions array"), Path, PositionsOffset);
		}
	}

	LocalIndices.SetNumUninitialized(IndexNum);
	if (IndexNum > 0 && !Source.Read(IndicesOffset, LocalIndices.GetData(), IndicesBytes))
	{
		return FCFDVizResult::Fail(ECFDVizError::FileReadFailed, TEXT("failed to read the indices array"), Path, IndicesOffset);
	}

	// Section 5.3: every index must address an existing vertex. Checked here,
	// before any caller can index with it - which is where it would otherwise
	// crash or, worse, read a neighbouring array and render silently wrong
	// geometry. An out-of-range index is a rejected file, never a clamped index.
	{
		const uint32 VertexLimit = static_cast<uint32>(VertexNum);
		for (int32 Entry = 0; Entry < IndexNum; ++Entry)
		{
			if (LocalIndices[Entry] >= VertexLimit)
			{
				return FCFDVizResult::Fail(
					ECFDVizError::IndexOutOfRange,
					FString::Printf(
						TEXT("triangle index %u at triangle %d is >= vertexCount %d; the index buffer is corrupt"),
						LocalIndices[Entry],
						Entry / static_cast<int32>(CFDViz::Cvm::IndicesPerTriangle),
						VertexNum),
					Path,
					IndicesOffset + static_cast<int64>(Entry) * CFDViz::Cvm::IndexBytes);
			}
		}
	}

	const bool bLoadNormals = ParsedHeader.HasNormals() && Options.bLoadNormals;
	if (bLoadNormals)
	{
		LocalNormals.SetNumUninitialized(VertexNum);
		if (VertexNum > 0 && !Source.Read(NormalsOffset, LocalNormals.GetData(), NormalsBytes))
		{
			return FCFDVizResult::Fail(ECFDVizError::FileReadFailed, TEXT("failed to read the normals array"), Path, NormalsOffset);
		}
	}

	const bool bLoadPatchIds = ParsedHeader.HasPatchIds() && Options.bLoadPatchIds;
	if (bLoadPatchIds)
	{
		LocalPatchIds.SetNumUninitialized(TriangleNum);
		if (TriangleNum > 0 && !Source.Read(PatchIdsOffset, LocalPatchIds.GetData(), PatchIdsBytes))
		{
			return FCFDVizResult::Fail(ECFDVizError::FileReadFailed, TEXT("failed to read the patchIds array"), Path, PatchIdsOffset);
		}
	}

	const bool bLoadNodeIds = ParsedHeader.HasNodeIds() && Options.bLoadNodeIds;
	if (bLoadNodeIds)
	{
		LocalNodeIds.SetNumUninitialized(VertexNum);
		if (VertexNum > 0 && !Source.Read(NodeIdsOffset, LocalNodeIds.GetData(), NodeIdsBytes))
		{
			return FCFDVizResult::Fail(ECFDVizError::FileReadFailed, TEXT("failed to read the nodeIds array"), Path, NodeIdsOffset);
		}
	}

	/* -- commit ----------------------------------------------------------- */

	Header = ParsedHeader;
	SourcePath = Path;
	Positions = MoveTemp(LocalPositions);
	DoublePositions = MoveTemp(LocalDoublePositions);
	Normals = MoveTemp(LocalNormals);
	Indices = MoveTemp(LocalIndices);
	PatchIds = MoveTemp(LocalPatchIds);
	NodeIds = MoveTemp(LocalNodeIds);
	bHasNormals = bLoadNormals;
	bHasPatchIds = bLoadPatchIds;
	bHasNodeIds = bLoadNodeIds;
	bLoaded = true;

	UE_LOG(LogFlowViz, Verbose,
		TEXT("CVM loaded: %s - %d vertices, %d triangles%s%s%s%s"),
		*Path,
		VertexNum,
		TriangleNum,
		bHasNormals ? TEXT(", normals") : TEXT(""),
		bHasPatchIds ? TEXT(", patchIds") : TEXT(""),
		bHasNodeIds ? TEXT(", nodeIds") : TEXT(""),
		Header.ArePositionsFloat64() ? TEXT(", float64 positions narrowed to float32") : TEXT(""));

	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* FCFDVizMeshReader - element access                                           */
/* -------------------------------------------------------------------------- */

bool FCFDVizMeshReader::TryGetTriangle(int32 TriangleIndex, uint32& OutA, uint32& OutB, uint32& OutC) const
{
	if (TriangleIndex < 0 || TriangleIndex >= GetTriangleCount())
	{
		return false;
	}
	const int32 Base = TriangleIndex * static_cast<int32>(CFDViz::Cvm::IndicesPerTriangle);
	OutA = Indices[Base + 0];
	OutB = Indices[Base + 1];
	OutC = Indices[Base + 2];
	return true;
}

bool FCFDVizMeshReader::TryGetTrianglePatchId(int32 TriangleIndex, uint32& OutPatchId) const
{
	if (!PatchIds.IsValidIndex(TriangleIndex))
	{
		return false;
	}
	OutPatchId = PatchIds[TriangleIndex];
	return true;
}

bool FCFDVizMeshReader::TryGetVertexNodeId(int32 VertexIndex, uint64& OutNodeId) const
{
	if (!NodeIds.IsValidIndex(VertexIndex))
	{
		return false;
	}
	OutNodeId = NodeIds[VertexIndex];
	return true;
}

/* -------------------------------------------------------------------------- */
/* FCFDVizMeshReader - derived                                                  */
/* -------------------------------------------------------------------------- */

FBox3f FCFDVizMeshReader::ComputeBounds() const
{
	FBox3f Bounds(ForceInit);
	for (const FVector3f& Position : Positions)
	{
		// A single NaN or infinite coordinate would otherwise poison the whole
		// box and, through it, camera framing and culling. Skipping such a vertex
		// changes nothing about the stored positions, which GetPositions still
		// returns bit-exactly (format rule 1.7); it only declines to let a value
		// that bounds nothing define a bound. IsFinite is spelled out rather than
		// using ContainsNaN, whose name suggests it rejects only NaN when it also
		// rejects infinities - both of which must be excluded here.
		if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z))
		{
			continue;
		}
		Bounds += Position;
	}
	return Bounds;
}

void FCFDVizMeshReader::GetUniquePatchIds(TArray<uint32>& OutUniquePatchIds) const
{
	OutUniquePatchIds.Reset();
	if (PatchIds.IsEmpty())
	{
		return;
	}

	TSet<uint32> Seen;
	Seen.Reserve(PatchIds.Num());
	for (const uint32 PatchId : PatchIds)
	{
		bool bAlreadySeen = false;
		Seen.Add(PatchId, &bAlreadySeen);
		if (!bAlreadySeen)
		{
			OutUniquePatchIds.Add(PatchId);
		}
	}

	// Ascending, so a UI listing patches and a test asserting them agree without
	// either depending on hash order, which is not stable across runs.
	OutUniquePatchIds.Sort();
}

int32 FCFDVizMeshReader::CountTrianglesInPatch(uint32 PatchId) const
{
	int32 Count = 0;
	for (const uint32 Stored : PatchIds)
	{
		if (Stored == PatchId)
		{
			++Count;
		}
	}
	return Count;
}

FCFDVizResult FCFDVizMeshReader::BuildForPatch(
	uint32 PatchId,
	TArray<FVector3f>& OutPositions,
	TArray<uint32>& OutIndices) const
{
	return BuildForPatch(PatchId, OutPositions, OutIndices, nullptr, nullptr);
}

FCFDVizResult FCFDVizMeshReader::BuildForPatch(
	uint32 PatchId,
	TArray<FVector3f>& OutPositions,
	TArray<uint32>& OutIndices,
	TArray<FVector3f>* OutNormals,
	TArray<int32>* OutSourceVertexIndices) const
{
	// Cleared up front so a caller that ignores the result never renders whatever
	// happened to be in these arrays before the call.
	OutPositions.Reset();
	OutIndices.Reset();
	if (OutNormals != nullptr)
	{
		OutNormals->Reset();
	}
	if (OutSourceVertexIndices != nullptr)
	{
		OutSourceVertexIndices->Reset();
	}

	if (!bLoaded)
	{
		// There is no file behind an unloaded reader, so there is nothing more
		// specific to say than that.
		return FCFDVizResult::Fail(
			ECFDVizError::FileNotFound,
			TEXT("no mesh is loaded; LoadFromFile must succeed before a patch can be extracted"),
			SourcePath);
	}

	if (!bHasPatchIds)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("cannot extract patch %u: this mesh carries no patchIds array (flag bit 1 is clear, or patch IDs were not requested at load)"),
				PatchId),
			SourcePath,
			CvmOffsetPatchIdsOffset);
	}

	const int32 TriangleNum = GetTriangleCount();
	if (PatchIds.Num() != TriangleNum)
	{
		// Unreachable through a successful load, which sizes PatchIds from
		// triangleCount. Kept because the per-triangle/per-vertex confusion this
		// guards against is the format's easiest mistake, and a future change that
		// reintroduced it would otherwise index past the end of one of the arrays.
		return FCFDVizResult::Fail(
			ECFDVizError::SizeMismatch,
			FString::Printf(
				TEXT("patchIds has %d entries but the mesh has %d triangles; patchIds is one value per triangle"),
				PatchIds.Num(), TriangleNum),
			SourcePath,
			CvmOffsetPatchIdsOffset);
	}

	const int32 MatchingTriangles = CountTrianglesInPatch(PatchId);
	if (MatchingTriangles == 0)
	{
		// Failing rather than returning an empty mesh is deliberate: an empty
		// result is indistinguishable from a patch the user hid, so the caller
		// would draw nothing and believe the case was fine. The available IDs are
		// listed because "patch 7 is missing" is far less useful than knowing the
		// mesh actually offers 1, 2 and 3.
		TArray<uint32> Available;
		GetUniquePatchIds(Available);

		TArray<FString> Parts;
		Parts.Reserve(FMath::Min(Available.Num(), MaxPatchIdsInMessage));
		for (int32 Index = 0; Index < Available.Num() && Index < MaxPatchIdsInMessage; ++Index)
		{
			Parts.Add(FString::Printf(TEXT("%u"), Available[Index]));
		}
		FString AvailableText = FString::Join(Parts, TEXT(", "));
		if (Available.Num() > MaxPatchIdsInMessage)
		{
			AvailableText += FString::Printf(TEXT(", ... (%d in total)"), Available.Num());
		}

		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("no triangle in this mesh has patch ID %u; available patch IDs: %s"),
				PatchId,
				Available.IsEmpty() ? TEXT("none") : *AvailableText),
			SourcePath,
			CvmOffsetPatchIdsOffset);
	}

	const bool bEmitNormals = OutNormals != nullptr && bHasNormals && Normals.Num() == Positions.Num();

	// Renumber from zero, copying only the vertices the patch actually uses -
	// otherwise every extracted patch would carry the whole domain's vertex
	// buffer, which for a case with four patches is four full copies on the GPU.
	TArray<int32> Remap;
	Remap.Init(INDEX_NONE, Positions.Num());

	OutIndices.Reserve(MatchingTriangles * static_cast<int32>(CFDViz::Cvm::IndicesPerTriangle));

	for (int32 Triangle = 0; Triangle < TriangleNum; ++Triangle)
	{
		if (PatchIds[Triangle] != PatchId)
		{
			continue;
		}

		const int32 Base = Triangle * static_cast<int32>(CFDViz::Cvm::IndicesPerTriangle);
		for (int32 Corner = 0; Corner < CFDViz::Cvm::IndicesPerTriangle; ++Corner)
		{
			// Load proved every index is < vertexCount, so this cast and lookup
			// cannot go out of range; nothing here re-derives that guarantee.
			const int32 SourceVertex = static_cast<int32>(Indices[Base + Corner]);

			int32& Mapped = Remap[SourceVertex];
			if (Mapped == INDEX_NONE)
			{
				Mapped = OutPositions.Num();
				OutPositions.Add(Positions[SourceVertex]);
				if (bEmitNormals)
				{
					OutNormals->Add(Normals[SourceVertex]);
				}
				if (OutSourceVertexIndices != nullptr)
				{
					OutSourceVertexIndices->Add(SourceVertex);
				}
			}

			// Corner order is preserved, so the extracted mesh keeps the section
			// 5.3 counter-clockwise-from-outside winding. Reordering here would
			// turn every patch inside out.
			OutIndices.Add(static_cast<uint32>(Mapped));
		}
	}

	return FCFDVizResult::Ok();
}
