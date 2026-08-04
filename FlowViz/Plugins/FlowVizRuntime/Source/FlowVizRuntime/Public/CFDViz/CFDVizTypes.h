// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// CoreMinimal.h forward-declares TArrayView but does not define it, and
// FCFDVizStatistics::AccumulateSample indexes one inline, which needs the
// complete type.
#include "Containers/ArrayView.h"

#include <limits>

/**
 * Shared type vocabulary for every CFDViz 1.0 reader.
 *
 * The manifest parser, the CVF brick reader, the CVM mesh reader and the
 * visualisation adapter all speak these types, so a disagreement here becomes a
 * disagreement between the Unreal reader and the Python reference - the one
 * failure this project's two-implementation design exists to prevent. The
 * normative contract is `Docs/CFDVIZ_FORMAT.md`; section references below point
 * at the clause each declaration encodes.
 *
 * Nothing in this header allocates, touches a UObject, or touches a UWorld:
 * everything is callable from a worker thread, because the readers built on it
 * must be (engineering rule 1 - no game-thread file I/O or decompression).
 *
 * No JSON type appears here. Json and JsonUtilities are PRIVATE dependencies of
 * this module, so a public header that mentioned them would not compile for
 * dependent modules.
 */

/* -------------------------------------------------------------------------- */
/* Enumerations - the numeric values are on-disk format, not implementation     */
/* detail. Changing one silently reinterprets every file ever written.          */
/* -------------------------------------------------------------------------- */

/**
 * Storage precision of a stored value (format section 4.2 / 6.2).
 *
 * The numeric values are the bytes actually written into CVF byte 59 and CVA
 * byte 49. They start at 1, not 0 - there is deliberately no zero data type, so
 * a zero-filled or truncated header cannot be mistaken for a valid float16
 * field.
 *
 * `Float64` exists here because CVA offers it and CVF does not, and section 6.2
 * requires that "1 and 2 match CVF section 4.2 so a shared C++ enum needs no
 * translation". Value 3 is uint8 in CVF and is *reserved* in CVA; value 4 is
 * float64 in CVA and is *not supported* in CVF, where float64 storage is
 * excluded from 1.0 (section 3.3). Use IsDataTypeSupportedInCvf /
 * IsDataTypeSupportedInCva rather than assuming every enumerator is legal in the
 * container you are reading.
 */
enum class ECFDVizDataType : uint8
{
	/** IEEE 754 binary16. */
	Float16 = 1,
	/** IEEE 754 binary32. */
	Float32 = 2,
	/** Unsigned 8-bit. CVF only; the quantisation is a property of the format and is declared in the manifest. */
	UInt8 = 3,
	/** IEEE 754 binary64. CVA only; CVF 1.0 rejects it. */
	Float64 = 4
};

/**
 * What a stored value is attached to (format section 3.2 / 4.2).
 *
 * CVA reuses the same two codes with mesh-flavoured names - 0 is element-like,
 * 1 is nodal/point-like - so a shared enum needs no translation there either.
 *
 * The half-cell offset between these two is called out by the spec as "the
 * single most common source of visualisation error". FCFDVizGrid below is the
 * one place that offset is implemented; do not re-derive it at a call site.
 */
enum class ECFDVizAssociation : uint8
{
	/** nx*ny*nz values, each at a cell centre. CVA: mesh-element. */
	Cell = 0,
	/** (nx+1)*(ny+1)*(nz+1) values, each at a grid point. CVA: mesh-vertex. */
	Point = 1
};

/**
 * Payload compression (format section 4.2 and section 7 / ADR 005).
 *
 * Zstd is reserved-but-unimplemented rather than deleted: reserving the ID means
 * a future 1.x writer can add it without renumbering, and files it writes are
 * rejected by a 1.0 reader with an actionable message instead of decoding to
 * garbage. A 1.0 reader MUST reject Zstd with exactly
 * CFDViz::ZstdRejectionMessage and MUST NOT fall back to another codec.
 */
enum class ECFDVizCodec : uint8
{
	/** Stored verbatim. Legal for tiny or already-incompressible payloads. */
	None = 0,
	/** RESERVED and unimplemented in 1.0. Reject, never substitute. */
	Zstd = 1,
	/** Decoded via NAME_LZ4. Permitted; the Python writer emits it only with the optional lz4 package. */
	LZ4 = 2,
	/** Shipping default. RFC 1950 zlib container - 2-byte header plus trailing Adler-32, NOT raw DEFLATE. */
	Zlib = 3
};

/**
 * Why a read failed.
 *
 * Errors are returned, never thrown (UE builds without exceptions) and never
 * asserted in shipping (engineering rule 4). The enum is deliberately specific:
 * "the file is bad" is not actionable, "the brick directory claims 4 GB of
 * entries past the end of a 12 MB file" is.
 */
enum class ECFDVizError : uint8
{
	/** Success. A default-constructed FCFDVizResult holds this. */
	None = 0,

	/** The path does not exist, or resolved outside the case root. */
	FileNotFound,
	/** The file exists but could not be read - permissions, a device error, a truncated network volume. */
	FileReadFailed,
	/** The file is shorter than the fixed header it claims to be. */
	FileTooSmall,

	/** The leading magic bytes are not this container's. */
	BadMagic,
	/** majorVersion is not 1 (format rule 1.4). */
	UnsupportedVersion,
	/** endianMarker != 0x01020304. Rejected rather than byte-swapped (format rule 1.1). */
	UnsupportedEndianness,
	/** headerBytes wrong, a reserved field non-zero, or an internally inconsistent header. */
	InvalidHeader,
	/** CRC-32C over the header, with the CRC field zeroed, does not match. */
	HeaderCrcMismatch,
	/** CRC-32C over the stored (still compressed) payload bytes does not match. */
	PayloadCrcMismatch,

	/** directoryOffset plus brickCount*80 does not fit inside the file. */
	DirectoryOutOfBounds,
	/** A payload offset plus its compressed length does not fit inside the file. */
	PayloadOutOfBounds,
	/** uncompressedBytes disagrees with valid extent * components * sizeof(dataType) (section 4.4.4 / 6.3.2). */
	SizeMismatch,

	/** codec is Zstd, or a value not in ECFDVizCodec. */
	UnsupportedCodec,
	/** The codec ran but did not produce the declared byte count, or reported failure. */
	DecompressionFailed,
	/** dataType is not legal for this container (e.g. float64 in a CVF). */
	UnsupportedDataType,
	/** association is not cell or point; the reserved 1.0 associations land here (section 3.2). */
	UnsupportedAssociation,

	/** manifest.json failed schema validation or one of the section 3.1 invariants. */
	InvalidManifest,
	/** A manifest path was absolute, had a drive letter, a backslash, a control character, or a ".." segment (rule 1.3). */
	PathTraversal,

	/** A frame, field, brick, vertex or triangle index was outside its declared range. */
	IndexOutOfRange,
	/** A size derived from file contents overflowed, or exceeded what this reader will allocate. */
	AllocationTooLarge
};

/* -------------------------------------------------------------------------- */
/* Enum helpers                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Bytes occupied by one component of this data type.
 *
 * Returns 0 for a value outside the enum. That is intentional and safe: every
 * caller multiplies this into an expected byte count and compares against the
 * stored uncompressedBytes, so an unknown type produces 0 != stored and the
 * payload is rejected before anything is allocated. It must never be used as a
 * stride without that comparison.
 */
FLOWVIZRUNTIME_API int32 SizeOfDataType(ECFDVizDataType DataType);

/** True if this data type may appear in a CVF (float16, float32, uint8 - section 3.3). */
FLOWVIZRUNTIME_API bool IsDataTypeSupportedInCvf(ECFDVizDataType DataType);

/** True if this data type may appear in a CVA (float16, float32, float64 - section 6.2). */
FLOWVIZRUNTIME_API bool IsDataTypeSupportedInCva(ECFDVizDataType DataType);

/**
 * Map a codec to the FName FCompression expects.
 *
 * Zlib -> NAME_Zlib, LZ4 -> NAME_LZ4. None and Zstd return NAME_None, because
 * neither goes through FCompression: None is a straight copy and Zstd must be
 * rejected outright with CFDViz::ZstdRejectionMessage. Callers must therefore
 * handle those two before asking for a format name, not treat NAME_None as a
 * decodable format.
 */
FLOWVIZRUNTIME_API FName CodecToFName(ECFDVizCodec Codec);

/** True if this build can actually decode the codec. False for Zstd and for anything outside the enum. */
FLOWVIZRUNTIME_API bool IsCodecSupported(ECFDVizCodec Codec);

/**
 * Names below match the Python reference and the manifest schema *exactly*
 * (lowercase: "float16", "cell", "zlib"), so the manifest parser can compare
 * against them directly instead of maintaining a second spelling of the same
 * enum. Returned pointers are static literals and outlive any caller.
 */
FLOWVIZRUNTIME_API const TCHAR* DataTypeToString(ECFDVizDataType DataType);
FLOWVIZRUNTIME_API const TCHAR* AssociationToString(ECFDVizAssociation Association);
FLOWVIZRUNTIME_API const TCHAR* CodecToString(ECFDVizCodec Codec);

/** Stable identifier for logs and test failure messages, e.g. "PayloadCrcMismatch". Not user-facing prose. */
FLOWVIZRUNTIME_API const TCHAR* CFDVizErrorToString(ECFDVizError Error);

/**
 * Parse the manifest spellings back to the enum. Comparison is case-sensitive,
 * matching the JSON schema's closed enums; an unrecognised string is a rejected
 * required enum (format rule 1.4), not a value to guess at.
 *
 * TryParseDataType accepts "float64" so a manifest can be diagnosed precisely -
 * the caller still has to reject it for CVF storage via IsDataTypeSupportedInCvf.
 */
FLOWVIZRUNTIME_API bool TryParseDataType(const FString& Name, ECFDVizDataType& OutDataType);
FLOWVIZRUNTIME_API bool TryParseAssociation(const FString& Name, ECFDVizAssociation& OutAssociation);
FLOWVIZRUNTIME_API bool TryParseCodec(const FString& Name, ECFDVizCodec& OutCodec);

/**
 * Validate a raw byte read straight out of a binary header.
 *
 * These exist so no reader ever does `static_cast<ECFDVizDataType>(Byte)`. A
 * cast of an out-of-range byte produces an enum value no switch handles, which
 * is exactly the silent-misbehaviour path the spec forbids; these return false
 * instead and leave the out parameter untouched.
 */
FLOWVIZRUNTIME_API bool TryDataTypeFromCode(uint8 Code, ECFDVizDataType& OutDataType);
FLOWVIZRUNTIME_API bool TryAssociationFromCode(uint8 Code, ECFDVizAssociation& OutAssociation);
FLOWVIZRUNTIME_API bool TryCodecFromCode(uint8 Code, ECFDVizCodec& OutCodec);

/* -------------------------------------------------------------------------- */
/* Format constants                                                             */
/* -------------------------------------------------------------------------- */

namespace CFDViz
{
	/** Read as anything else, the file is big-endian (or not a CFDViz file) and is rejected, never swapped. */
	inline constexpr uint32 EndianMarker = 0x01020304u;

	/** Supported container version. A newer minor is accepted; a different major is not (format rule 1.4). */
	inline constexpr uint16 SupportedMajorVersion = 1;
	inline constexpr uint16 SupportedMinorVersion = 0;

	/** `CFDVOL1\0` - trailing NUL is part of the 8-byte magic, not a terminator. */
	inline constexpr ANSICHAR CvfMagic[8] = { 'C', 'F', 'D', 'V', 'O', 'L', '1', '\0' };
	/** `CFDMESH1` - eight significant characters, no NUL. */
	inline constexpr ANSICHAR CvmMagic[8] = { 'C', 'F', 'D', 'M', 'E', 'S', 'H', '1' };
	/** `CFDARR1\0` - trailing NUL is part of the magic. */
	inline constexpr ANSICHAR CvaMagic[8] = { 'C', 'F', 'D', 'A', 'R', 'R', '1', '\0' };

	/** Fixed header sizes (sections 4.1, 5.1, 6.1). Headers are parsed field by field; never memcpy a struct over them, because C++ padding is compiler-dependent. */
	inline constexpr int64 CvfHeaderBytes = 128;
	inline constexpr int64 CvmHeaderBytes = 96;
	inline constexpr int64 CvaHeaderBytes = 96;

	/** One CVF brick directory entry (section 4.3). */
	inline constexpr int64 CvfDirectoryEntryBytes = 80;

	/**
	 * Byte range of the headerCrc32c field, which is zeroed while computing the
	 * header CRC over the whole header. CVM and CVA share offset 80 on purpose
	 * (section 6.1), so one routine can validate either.
	 */
	inline constexpr int64 CvfHeaderCrcOffset = 104;
	inline constexpr int64 CvmHeaderCrcOffset = 80;
	inline constexpr int64 CvaHeaderCrcOffset = 80;
	inline constexpr int64 HeaderCrcFieldBytes = 4;

	/** Per-brick statistics arrays and backgroundValue are float32[4], so a CVF field carries at most 4 components. */
	inline constexpr int32 MaxCvfComponentCount = 4;

	/** Longest relative path a manifest may declare; mirrors the schema's maxLength so both readers agree. */
	inline constexpr int32 MaxRelativePathLength = 1024;

	/**
	 * The exact user-facing string section 7 requires when zstd data is met.
	 *
	 * It is a constant rather than a literal at each call site because the spec
	 * mandates the wording, and the Python side asserts the identical string.
	 */
	inline constexpr const TCHAR* ZstdRejectionMessage =
		TEXT("CVF codec 'zstd' is reserved but not supported in CFDViz 1.0. Re-export this case with codec 'zlib'.");

	/** True if a container major version can be read by this build (format rule 1.4). */
	inline bool IsSupportedMajorVersion(uint32 MajorVersion)
	{
		return MajorVersion == SupportedMajorVersion;
	}

	/**
	 * Lexical path-traversal check, format rule 1.3.
	 *
	 * Rejects an empty path, a leading '/', a Windows drive letter, any
	 * backslash (a separator on Windows, so "..\x" would otherwise escape), any
	 * control character, any segment exactly equal to "..", and anything longer
	 * than MaxRelativePathLength. A single "." segment is *not* rejected,
	 * because the schema pattern does not reject it and the two must agree.
	 *
	 * This runs on the string before any filesystem call, which is the point:
	 * resolving first and checking afterwards can be defeated by symlinks.
	 */
	FLOWVIZRUNTIME_API bool IsSafeRelativePath(const FString& RelativePath);

	/**
	 * Multiply with overflow detection.
	 *
	 * @return true and the product, or false when the result would not fit in an
	 *         int64. Sizes here come from untrusted files, and a wrapped product
	 *         turns a bounds check into a rubber stamp.
	 */
	FLOWVIZRUNTIME_API bool TryMultiply(int64 A, int64 B, int64& OutProduct);

	/**
	 * Expected uncompressed byte count for a payload: values * components * sizeof(type).
	 *
	 * This is the equality that sections 4.4.4 and 6.3.2 require a reader to
	 * verify *before allocating*, and it is the primary defence against a
	 * hostile size field. Returns INDEX_NONE on overflow, on a non-positive
	 * component count, or on an unknown data type - all of which then fail the
	 * comparison against the stored uncompressedBytes.
	 */
	FLOWVIZRUNTIME_API int64 ComputePayloadBytes(int64 ValueCount, int32 ComponentCount, ECFDVizDataType DataType);
}

/* -------------------------------------------------------------------------- */
/* Result                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * Outcome of a read, carrying enough context to fix the problem.
 *
 * Section 10 requires that a malformed case produce "a specific, actionable
 * error message identifying the offending file and byte offset". That is why
 * FilePath and ByteOffset are fields rather than something a caller is trusted
 * to remember and prepend.
 */
struct FCFDVizResult
{
	/** What went wrong. None means success. */
	ECFDVizError Error = ECFDVizError::None;

	/** Human-readable specifics, e.g. "payload CRC mismatch in brick 12". No path and no offset - ToString adds those. */
	FString Message;

	/** The offending file. Case-relative where the case root is known, otherwise absolute. Empty when no single file is to blame. */
	FString FilePath;

	/** Byte offset within FilePath, or INDEX_NONE when the failure is not tied to one offset. */
	int64 ByteOffset = INDEX_NONE;

	FCFDVizResult() = default;

	/** Success. */
	static FCFDVizResult Ok()
	{
		return FCFDVizResult();
	}

	/**
	 * Failure.
	 *
	 * @param InError      Category, for programmatic handling.
	 * @param InMessage    Specifics. Falls back to CFDVizErrorToString when empty.
	 * @param InPath       Offending file, if one file is to blame.
	 * @param InByteOffset Offset within that file, or INDEX_NONE.
	 */
	static FCFDVizResult Fail(
		ECFDVizError InError,
		FString InMessage,
		FString InPath = FString(),
		int64 InByteOffset = INDEX_NONE)
	{
		FCFDVizResult Result;
		Result.Error = InError;
		Result.Message = MoveTemp(InMessage);
		Result.FilePath = MoveTemp(InPath);
		Result.ByteOffset = InByteOffset;
		return Result;
	}

	bool IsOk() const
	{
		return Error == ECFDVizError::None;
	}

	/**
	 * One line a user can act on, e.g.
	 * "case/frame_0007/U.cvf: payload CRC mismatch in brick 12 at byte 8192".
	 *
	 * Each part is omitted when it is not known, so a manifest-level failure
	 * with no byte offset still reads as a sentence.
	 */
	FLOWVIZRUNTIME_API FString ToString() const;

	/**
	 * Log via LogFlowViz at Error verbosity when this result is a failure.
	 *
	 * A no-op on success, so it is safe to call unconditionally after any read.
	 * It only logs - it never swallows the failure, which still has to be
	 * propagated by the caller.
	 */
	FLOWVIZRUNTIME_API void LogIfFailed() const;
};

/* -------------------------------------------------------------------------- */
/* Grid                                                                         */
/* -------------------------------------------------------------------------- */

/**
 * A uniform rectilinear grid, in solver units and solver axes.
 *
 * CELL VERSUS POINT - read this before indexing anything.
 *
 * `Dimensions` is always the *cell* count (nx, ny, nz), exactly as the manifest
 * declares it. How many values a field stores on this grid depends on its
 * association, and the two differ by one per axis:
 *
 *     Cell   nx * ny * nz values.               Value (i,j,k) sits at the cell
 *                                               CENTRE: Origin + Spacing*(i+0.5, j+0.5, k+0.5)
 *     Point  (nx+1)*(ny+1)*(nz+1) values.       Value (i,j,k) sits ON the grid
 *                                               point: Origin + Spacing*(i, j, k)
 *
 * The half-cell offset is the single most common visualisation bug in this
 * format - it renders as data shifted by half a voxel, which looks plausible
 * and is wrong. Never index a field with Dimensions directly; call
 * ValueCounts(Association) and let it add the +1. Never place a value with
 * Origin + Spacing*idx unless the association really is Point; use CellCenter
 * and PointPosition, which cannot be confused for one another.
 *
 * ORDER. X varies fastest, then Y, then Z (section 4.4.1), matching the CVF
 * payload layout, so a decoded brick can be walked linearly.
 *
 * UNITS AND AXES. Origin and Spacing are in the manifest's `units.length` and in
 * the canonical CFDViz frame - right-handed, Z-up, X-forward. They are NOT
 * Unreal centimetres and NOT Unreal's left-handed frame. Converting belongs in
 * the visualisation adapter, which calls MakeSolverToUnrealTransform below;
 * readers keep solver units (engineering rule 4). Unreal centimetres must never
 * be written back into a stored field.
 *
 * PRECISION. Physical coordinates are double, not float: scientific domains run
 * from micrometre features to kilometre extents, and a float32 origin quietly
 * loses metres of precision at kilometre magnitudes.
 */
struct FCFDVizGrid
{
	/** Cell counts (nx, ny, nz). Every component must be >= 1 (section 3.1). */
	FIntVector Dimensions = FIntVector(0, 0, 0);

	/** Position of grid point (0,0,0) - the minimum corner of cell (0,0,0), not its centre. Solver units. */
	FVector Origin = FVector::ZeroVector;

	/** Cell size along each axis. Every component must be > 0 (section 3.1). Solver units. */
	FVector Spacing = FVector::OneVector;

	/** Section 3.1: dimensions >= 1 per axis, spacing > 0 per axis, and both finite. */
	bool IsValid() const
	{
		return Dimensions.X >= 1 && Dimensions.Y >= 1 && Dimensions.Z >= 1
			&& Spacing.X > 0.0 && Spacing.Y > 0.0 && Spacing.Z > 0.0
			&& FMath::IsFinite(Spacing.X) && FMath::IsFinite(Spacing.Y) && FMath::IsFinite(Spacing.Z)
			&& FMath::IsFinite(Origin.X) && FMath::IsFinite(Origin.Y) && FMath::IsFinite(Origin.Z);
	}

	/**
	 * Values along each axis for this association: (nx,ny,nz) for Cell,
	 * (nx+1,ny+1,nz+1) for Point. This is the only place the +1 is applied.
	 *
	 * Returns (0,0,0) - a count no index can satisfy - for a non-positive
	 * dimension, and for a dimension so large that the Point +1 would overflow
	 * int32. Dimensions come from an untrusted manifest, and a wrapped count
	 * would turn every bounds check below into a rubber stamp. Every consumer
	 * already handles a zero count as "no values", so the degenerate answer
	 * propagates safely rather than needing a second error channel here.
	 */
	FIntVector ValueCounts(ECFDVizAssociation Association) const
	{
		if (Dimensions.X <= 0 || Dimensions.Y <= 0 || Dimensions.Z <= 0)
		{
			return FIntVector(0, 0, 0);
		}

		if (Association != ECFDVizAssociation::Point)
		{
			return Dimensions;
		}

		constexpr int32 MaxDimension = TNumericLimits<int32>::Max() - 1;
		if (Dimensions.X > MaxDimension || Dimensions.Y > MaxDimension || Dimensions.Z > MaxDimension)
		{
			return FIntVector(0, 0, 0);
		}
		return FIntVector(Dimensions.X + 1, Dimensions.Y + 1, Dimensions.Z + 1);
	}

	/**
	 * Total stored values for this association.
	 *
	 * @return The product, or INDEX_NONE if the grid is degenerate or the product
	 *         would overflow int64. A negative return must be treated as a
	 *         rejection (ECFDVizError::AllocationTooLarge), never coerced to a size.
	 */
	int64 ValueCount(ECFDVizAssociation Association) const
	{
		const FIntVector Counts = ValueCounts(Association);
		if (Counts.X <= 0 || Counts.Y <= 0 || Counts.Z <= 0)
		{
			return INDEX_NONE;
		}

		// int32 * int32 in int64 cannot overflow; the third factor can, so it is checked.
		const int64 CountXY = static_cast<int64>(Counts.X) * static_cast<int64>(Counts.Y);
		if (CountXY > TNumericLimits<int64>::Max() / static_cast<int64>(Counts.Z))
		{
			return INDEX_NONE;
		}
		return CountXY * static_cast<int64>(Counts.Z);
	}

	/** True if (i,j,k) addresses a stored value for this association. */
	bool Contains(int32 I, int32 J, int32 K, ECFDVizAssociation Association) const
	{
		const FIntVector Counts = ValueCounts(Association);
		return I >= 0 && J >= 0 && K >= 0 && I < Counts.X && J < Counts.Y && K < Counts.Z;
	}

	/**
	 * Centre of cell (i,j,k): Origin + Spacing * (i+0.5, j+0.5, k+0.5).
	 *
	 * Pure arithmetic - it is not bounds checked and will happily extrapolate
	 * outside the grid, which is deliberate so ghost cells and neighbour lookups
	 * work. Call Contains first when the index came from a file.
	 */
	FVector CellCenter(int32 I, int32 J, int32 K) const
	{
		return Origin + (FVector(static_cast<double>(I), static_cast<double>(J), static_cast<double>(K)) + FVector(0.5)) * Spacing;
	}

	/**
	 * Grid point (i,j,k): Origin + Spacing * (i, j, k). No half-cell offset.
	 *
	 * Not bounds checked, for the same reason as CellCenter.
	 */
	FVector PointPosition(int32 I, int32 J, int32 K) const
	{
		return Origin + FVector(static_cast<double>(I), static_cast<double>(J), static_cast<double>(K)) * Spacing;
	}

	/**
	 * Position of value (i,j,k) for whichever association the field uses.
	 *
	 * Prefer this when the association is data-driven; it removes the chance of
	 * pairing a Point field with CellCenter.
	 */
	FVector ValuePosition(int32 I, int32 J, int32 K, ECFDVizAssociation Association) const
	{
		return Association == ECFDVizAssociation::Point ? PointPosition(I, J, K) : CellCenter(I, J, K);
	}

	/**
	 * Flatten (i,j,k) with X fastest, then Y, then Z (section 4.4.1).
	 *
	 * @return The linear index, or INDEX_NONE when (i,j,k) is outside the value
	 *         extent for this association, or when the extent itself is too large
	 *         to address in an int64. Out of range returns INDEX_NONE rather than
	 *         wrapping, so a bad index from a file cannot become a valid-looking
	 *         offset into someone else's data.
	 */
	int64 LinearIndex(int32 I, int32 J, int32 K, ECFDVizAssociation Association) const
	{
		const FIntVector Counts = ValueCounts(Association);
		if (I < 0 || J < 0 || K < 0 || I >= Counts.X || J >= Counts.Y || K >= Counts.Z)
		{
			return INDEX_NONE;
		}

		// ValueCount performs the same nx*ny*nz overflow check, so a grid whose
		// total does not fit in an int64 is rejected here too. Without it, three
		// individually in-range indices could still combine into a wrapped,
		// plausible-looking offset.
		if (ValueCount(Association) == INDEX_NONE)
		{
			return INDEX_NONE;
		}

		return static_cast<int64>(I)
			+ static_cast<int64>(Counts.X) * (static_cast<int64>(J) + static_cast<int64>(Counts.Y) * static_cast<int64>(K));
	}

	/** Inverse of LinearIndex. Returns false, leaving OutIndex untouched, when Linear is out of range. */
	bool TryUnflattenIndex(int64 Linear, ECFDVizAssociation Association, FIntVector& OutIndex) const
	{
		const int64 Total = ValueCount(Association);
		if (Linear < 0 || Total <= 0 || Linear >= Total)
		{
			return false;
		}

		const FIntVector Counts = ValueCounts(Association);
		const int64 SliceStride = static_cast<int64>(Counts.X) * static_cast<int64>(Counts.Y);
		OutIndex = FIntVector(
			static_cast<int32>(Linear % Counts.X),
			static_cast<int32>((Linear / Counts.X) % Counts.Y),
			static_cast<int32>(Linear / SliceStride));
		return true;
	}

	/**
	 * Axis-aligned extent of the domain, in solver units:
	 * [Origin, Origin + Spacing * Dimensions].
	 *
	 * The same box for either association, which is the point - cell centres sit
	 * half a cell inside it and grid points sit exactly on its corners, so both
	 * describe the identical physical volume. Returns an invalid FBox
	 * (IsValid == 0) for a degenerate grid, so a caller cannot silently frame a
	 * camera on a zero-size domain.
	 */
	FBox WorldBounds() const
	{
		if (!IsValid())
		{
			return FBox(ForceInit);
		}
		const FVector Extent = FVector(
			static_cast<double>(Dimensions.X),
			static_cast<double>(Dimensions.Y),
			static_cast<double>(Dimensions.Z)) * Spacing;
		return FBox(Origin, Origin + Extent);
	}

	bool operator==(const FCFDVizGrid& Other) const
	{
		return Dimensions == Other.Dimensions && Origin == Other.Origin && Spacing == Other.Spacing;
	}

	bool operator!=(const FCFDVizGrid& Other) const
	{
		return !(*this == Other);
	}
};

/* -------------------------------------------------------------------------- */
/* Statistics                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Per-component and magnitude ranges over a field, with honest validity.
 *
 * Statistics ignore NaN and ignore cells rejected by the mask field (format
 * rules 1.7 and 4.4.7). A component with no valid samples at all records
 * min = +inf and max = -inf, which is the writer's mandated sentinel for "no
 * valid data" - NOT a range. Building a colour map from +inf..-inf produces an
 * inverted, meaningless scale, so every consumer must go through
 * TryGetComponentRange / TryGetMagnitudeRange instead of reading the arrays
 * directly. That is what bValid is for.
 *
 * Accumulation is double regardless of the storage type. float16, float32 and
 * uint8 all convert to double exactly, so no precision is invented and none is
 * lost, and summing millions of float32 samples in float32 would not be
 * reproducible against the Python reference.
 */
struct FCFDVizStatistics
{
	/** IEEE +infinity, the mandated "no valid data" sentinel for a minimum. */
	static constexpr double PositiveInfinity = std::numeric_limits<double>::infinity();
	/** IEEE -infinity, the mandated "no valid data" sentinel for a maximum. */
	static constexpr double NegativeInfinity = -std::numeric_limits<double>::infinity();

	/** Minimum per component, in field.components order. +inf where that component has no valid sample. */
	TArray<double> ComponentMin;

	/** Maximum per component, same order. -inf where that component has no valid sample. */
	TArray<double> ComponentMax;

	/**
	 * Range of the Euclidean magnitude over samples where EVERY component was
	 * valid. A partially-NaN vector has no meaningful magnitude - NaN propagates
	 * through the square root - so such samples are excluded rather than having
	 * their NaN components treated as zero.
	 */
	double MagnitudeMin = PositiveInfinity;
	double MagnitudeMax = NegativeInfinity;

	/** Count of NaN *component values* seen. Not a count of samples: a 3-vector with two NaNs contributes 2. */
	int64 NaNCount = 0;

	/** Count of non-NaN component values seen. NaNCount + ValidCount == samples * components. */
	int64 ValidCount = 0;

	/**
	 * True once at least one non-NaN component value has been seen.
	 *
	 * False means every stored value was NaN or masked, and the ranges are the
	 * +inf/-inf sentinel. Individual components can still be empty while this is
	 * true, so this is a cheap early-out, not a substitute for
	 * TryGetComponentRange.
	 *
	 * Code that populates this struct from manifest JSON must set it explicitly;
	 * AccumulateSample maintains it automatically.
	 */
	bool bValid = false;

	/** Zeroed counts and +inf/-inf sentinels for ComponentCount components. */
	static FCFDVizStatistics MakeEmpty(int32 ComponentCount)
	{
		FCFDVizStatistics Stats;
		if (ComponentCount > 0)
		{
			Stats.ComponentMin.Init(PositiveInfinity, ComponentCount);
			Stats.ComponentMax.Init(NegativeInfinity, ComponentCount);
		}
		return Stats;
	}

	int32 GetComponentCount() const
	{
		return ComponentMin.Num();
	}

	/**
	 * Fold one sample - all components of one voxel - into the running statistics.
	 *
	 * Values must hold exactly GetComponentCount() entries; a mismatched sample
	 * is ignored rather than partially applied, because a half-counted sample
	 * would corrupt ValidCount and hence the mean any caller derives from it.
	 *
	 * Masked samples are the caller's business: skip the call entirely for a
	 * cell the mask field rejects. NaN handling is here because it must be
	 * identical everywhere - values are never coerced, only counted and skipped.
	 */
	void AccumulateSample(TArrayView<const double> Values)
	{
		const int32 ComponentCount = GetComponentCount();
		if (Values.Num() != ComponentCount || ComponentCount <= 0)
		{
			return;
		}

		bool bSampleFullyValid = true;
		double MagnitudeSquared = 0.0;

		for (int32 Component = 0; Component < ComponentCount; ++Component)
		{
			const double Value = Values[Component];
			if (FMath::IsNaN(Value))
			{
				++NaNCount;
				bSampleFullyValid = false;
				continue;
			}

			++ValidCount;
			bValid = true;
			ComponentMin[Component] = FMath::Min(ComponentMin[Component], Value);
			ComponentMax[Component] = FMath::Max(ComponentMax[Component], Value);
			MagnitudeSquared += Value * Value;
		}

		if (bSampleFullyValid)
		{
			const double Magnitude = FMath::Sqrt(MagnitudeSquared);
			MagnitudeMin = FMath::Min(MagnitudeMin, Magnitude);
			MagnitudeMax = FMath::Max(MagnitudeMax, Magnitude);
		}
	}

	/**
	 * Range of one component.
	 *
	 * @return false - leaving the out parameters untouched - when the index is out
	 *         of range or that component saw no valid data. A false return means
	 *         "do not build a colour range", not "use zero".
	 */
	bool TryGetComponentRange(int32 Component, double& OutMin, double& OutMax) const
	{
		if (!ComponentMin.IsValidIndex(Component) || !ComponentMax.IsValidIndex(Component))
		{
			return false;
		}
		const double Min = ComponentMin[Component];
		const double Max = ComponentMax[Component];
		if (!(Min <= Max) || !FMath::IsFinite(Min) || !FMath::IsFinite(Max))
		{
			return false;
		}
		OutMin = Min;
		OutMax = Max;
		return true;
	}

	/** As TryGetComponentRange, for the magnitude range. */
	bool TryGetMagnitudeRange(double& OutMin, double& OutMax) const
	{
		if (!(MagnitudeMin <= MagnitudeMax) || !FMath::IsFinite(MagnitudeMin) || !FMath::IsFinite(MagnitudeMax))
		{
			return false;
		}
		OutMin = MagnitudeMin;
		OutMax = MagnitudeMax;
		return true;
	}

	/**
	 * Combine another block into this one, e.g. to build a global range from
	 * per-frame ranges. Merging frame by frame gives the same answer as one pass
	 * over every frame, because min/max and the counts are all associative.
	 *
	 * @return false, changing nothing, if the component counts differ.
	 */
	bool Merge(const FCFDVizStatistics& Other)
	{
		if (Other.GetComponentCount() != GetComponentCount())
		{
			return false;
		}

		for (int32 Component = 0; Component < GetComponentCount(); ++Component)
		{
			ComponentMin[Component] = FMath::Min(ComponentMin[Component], Other.ComponentMin[Component]);
			ComponentMax[Component] = FMath::Max(ComponentMax[Component], Other.ComponentMax[Component]);
		}

		MagnitudeMin = FMath::Min(MagnitudeMin, Other.MagnitudeMin);
		MagnitudeMax = FMath::Max(MagnitudeMax, Other.MagnitudeMax);
		NaNCount += Other.NaNCount;
		ValidCount += Other.ValidCount;
		bValid = bValid || Other.bValid;
		return true;
	}

	/** Diagnostic summary for logs and the validation report. Not a stable machine format. */
	FLOWVIZRUNTIME_API FString ToString() const;
};

/* -------------------------------------------------------------------------- */
/* Coordinate systems                                                           */
/* -------------------------------------------------------------------------- */

/** Chirality of a coordinate frame. Canonical CFDViz is right-handed; Unreal is left-handed. */
enum class ECFDVizCoordinateHandedness : uint8
{
	RightHanded = 0,
	LeftHanded = 1
};

/** A principal axis, for naming the up and forward directions of a frame. */
enum class ECFDVizAxis : uint8
{
	X = 0,
	Y = 1,
	Z = 2
};

/**
 * A coordinate frame, so a reader can *assert* the case is canonical instead of
 * assuming it.
 *
 * The canonical CFDViz frame (section 2) is fixed: right-handed, Z-up,
 * X-forward. The manifest's `coordinates` block restates it, and a manifest that
 * declares anything else is rejected rather than silently reinterpreted.
 */
struct FCFDVizCoordinateSystem
{
	ECFDVizCoordinateHandedness Handedness = ECFDVizCoordinateHandedness::RightHanded;
	ECFDVizAxis UpAxis = ECFDVizAxis::Z;
	ECFDVizAxis ForwardAxis = ECFDVizAxis::X;

	/** Right-handed, Z-up, X-forward - the only frame CFDViz 1.0 stores data in. */
	static FCFDVizCoordinateSystem Canonical()
	{
		return FCFDVizCoordinateSystem();
	}

	/** Left-handed, Z-up, X-forward - what Unreal renders in. Same up and forward as canonical; only chirality differs. */
	static FCFDVizCoordinateSystem Unreal()
	{
		FCFDVizCoordinateSystem System;
		System.Handedness = ECFDVizCoordinateHandedness::LeftHanded;
		System.UpAxis = ECFDVizAxis::Z;
		System.ForwardAxis = ECFDVizAxis::X;
		return System;
	}

	bool IsCanonical() const
	{
		return Handedness == ECFDVizCoordinateHandedness::RightHanded
			&& UpAxis == ECFDVizAxis::Z
			&& ForwardAxis == ECFDVizAxis::X;
	}

	bool operator==(const FCFDVizCoordinateSystem& Other) const
	{
		return Handedness == Other.Handedness && UpAxis == Other.UpAxis && ForwardAxis == Other.ForwardAxis;
	}

	bool operator!=(const FCFDVizCoordinateSystem& Other) const
	{
		return !(*this == Other);
	}
};

/**
 * Solver-to-Unreal conversion.
 *
 * READERS MUST NOT CALL THESE. Stored fields keep solver units and solver axes;
 * unit and axis conversion belongs in exactly one visualisation adapter
 * (engineering rule 4, format section 2, ADR 004). Unreal centimetres must never
 * be written back into a stored scientific field. They live in this header only
 * because the adapter and its tests both need them and there must be one
 * implementation.
 *
 * THE CONVERSION. Canonical CFDViz is right-handed, Z-up, X-forward. Unreal is
 * left-handed, Z-up, X-forward. Up and forward already agree, so the entire
 * difference is chirality, and the minimal correct change is to negate Y:
 *
 *     Unreal.X =  Solver.X
 *     Unreal.Y = -Solver.Y
 *     Unreal.Z =  Solver.Z
 *
 * Three consequences follow from that negation, and each is a real bug if
 * ignored:
 *
 *  1. TRIANGLE WINDING FLIPS. CVM triangles are counter-clockwise seen from
 *     outside (section 5.3). A mirror reverses that, so the mesh adapter must
 *     swap two indices per triangle or every surface renders inside-out with
 *     backwards normals. TransformReversesWinding answers this for any matrix.
 *  2. PSEUDOVECTORS NEGATE. Vorticity, angular velocity and anything else built
 *     from a cross product transforms with an extra factor of det(M) = -1. Use
 *     SolverToUnrealPseudoVector, not SolverToUnrealDirection, or vortex cores
 *     will spin the wrong way.
 *  3. SCALE APPLIES TO POSITIONS ONLY. A position in metres becomes centimetres;
 *     a velocity in m/s stays in m/s. SolverToUnrealDirection therefore mirrors
 *     without scaling - see its note.
 */
namespace CFDViz
{
	/** Unreal's world unit is the centimetre, so SI metres scale by 100. */
	inline constexpr double MetersToUnrealCentimeters = 100.0;
}

/**
 * Transform from canonical solver coordinates to Unreal world space.
 *
 * @param MetersToUnrealUnits Length scale, e.g. 100 for solver metres to Unreal
 *                            centimetres. Derive it from the manifest's
 *                            `units.length`; do not assume the case is in metres.
 * @return A UE row-vector matrix (apply as `Position * M`, or via
 *         TransformPosition). Its determinant is negative by construction - see
 *         the namespace comment above for what that implies.
 */
FLOWVIZRUNTIME_API FMatrix MakeSolverToUnrealTransform(double MetersToUnrealUnits = CFDViz::MetersToUnrealCentimeters);

/**
 * The exact inverse of MakeSolverToUnrealTransform.
 *
 * Written out rather than obtained from FMatrix::Inverse so a degenerate scale
 * cannot be quietly replaced by Inverse's identity fallback. A zero or
 * non-finite scale returns the identity and is a caller bug; validate
 * `units.length` when parsing the manifest instead of relying on this.
 */
FLOWVIZRUNTIME_API FMatrix MakeUnrealToSolverTransform(double MetersToUnrealUnits = CFDViz::MetersToUnrealCentimeters);

/** Convenience for a single position. Equivalent to MakeSolverToUnrealTransform(Scale).TransformPosition(P). */
FLOWVIZRUNTIME_API FVector SolverToUnrealPosition(const FVector& SolverPosition, double MetersToUnrealUnits = CFDViz::MetersToUnrealCentimeters);

/**
 * Mirror a true vector - velocity, displacement, a gradient - into Unreal axes.
 *
 * Deliberately applies NO unit scale. A velocity read from a CVF is in the
 * solver's units and stays there (engineering rule 4); whether an arrow glyph is
 * drawn 1 cm or 1 m long is a display decision for the adapter, made once and
 * visibly, not smuggled in through a coordinate conversion.
 */
FLOWVIZRUNTIME_API FVector3f SolverToUnrealDirection(const FVector3f& SolverVector);

/**
 * Mirror a pseudovector - vorticity, angular velocity, any cross product - into
 * Unreal axes.
 *
 * Picks up the extra det(M) = -1 relative to a true vector, giving
 * (-x, y, -z) where SolverToUnrealDirection gives (x, -y, z). Using the wrong
 * one reverses every vortex's sense of rotation, which is easy to miss because
 * the result still looks like plausible flow.
 */
FLOWVIZRUNTIME_API FVector3f SolverToUnrealPseudoVector(const FVector3f& SolverVector);

/**
 * True if applying Transform reverses triangle winding, i.e. its determinant is
 * negative. A mesh transformed by such a matrix needs two of every triangle's
 * three indices swapped to stay counter-clockwise from outside.
 */
FLOWVIZRUNTIME_API bool TransformReversesWinding(const FMatrix& Transform);

/**
 * Convert the manifest's `coordinates.sourceToCanonical` into an FMatrix.
 *
 * The manifest stores 16 numbers row-major, row 0 first, in the usual
 * mathematical convention where a transform acts on a column vector and the
 * translation sits in the last COLUMN (flat indices 3, 7, 11). Unreal's FMatrix
 * uses the row-vector convention, where the translation sits in the last ROW
 * (M[3][0..2]). The two are transposes of each other, so this function
 * transposes - and that transpose is the whole reason it exists rather than
 * being a memcpy. Copying the 16 values straight across produces a matrix that
 * looks right for pure rotations and silently misplaces every translation.
 *
 * @return false, leaving OutMatrix untouched, unless exactly 16 finite values
 *         were supplied.
 */
FLOWVIZRUNTIME_API bool TryMakeMatrixFromRowMajorArray(TArrayView<const double> RowMajor16, FMatrix& OutMatrix);
