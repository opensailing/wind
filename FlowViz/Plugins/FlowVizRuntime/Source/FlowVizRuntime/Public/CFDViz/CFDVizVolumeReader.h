// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizByteSource.h"
#include "CFDViz/CFDVizTypes.h"

// CoreMinimal.h forward-declares TArrayView but does not define it, and the
// Parse entry points below take one by value.
#include "Containers/ArrayView.h"
#include "Templates/UniquePtr.h"

/**
 * CVF reader - one field at one frame, stored as a bricked volume (format
 * section 4).
 *
 * A .cvf is a fixed 128-byte header, a brick directory of 80-byte entries, and
 * the brick payloads. The directory is the whole point: it permits random brick
 * access, region-of-interest loading and empty-space skipping without decoding
 * the whole volume. That is why this reader keeps a seekable source open and
 * reads spans on demand instead of slurping the file - a single frame of a
 * production case is routinely larger than the working set anyone wants
 * resident, and a reader that loaded whole files would make the directory
 * decorative. Open(const FString&) goes through FCFDVizFileByteSource, which is
 * an IFileHandle from FPlatformFileManager underneath.
 *
 * THE FIELD OFFSETS BELOW ARE THE FORMAT. Nothing here memcpy's a native struct
 * over file bytes - C++ padding is compiler-dependent, so a struct that happens
 * to match on one toolchain would silently misparse on another (section 4).
 * Every field is assembled byte by byte, little-endian, via CFDViz::FByteCursor.
 *
 * THREAD SAFETY. Everything here is callable from a worker thread. Nothing here
 * allocates a UObject, touches a UWorld, or dispatches to the game thread - that
 * is engineering rule 1, no game-thread file I/O or decompression. What it is
 * NOT is internally synchronised: one reader owns one open file handle whose
 * seek position is shared state, plus a scratch decode buffer, so two threads
 * must not call into the same instance at once. Give each thread its own reader
 * over the same path; several handles on one file are fine.
 * FCFDVizVolumeHeader::Parse and FCFDVizBrickEntry::Parse are pure functions of
 * their input and are safe to call from anywhere.
 *
 * WHAT THIS READER WILL NOT DO. It never converts a stored value. A float16
 * field comes back as IEEE binary16 bytes, not floats. Widening during the read
 * would lose NaN payload bits and would be exactly the silent normalisation
 * format rules 1.6 and 1.7 forbid; widening for the GPU or for statistics
 * belongs in the adapter layer, which is also the only place solver-to-Unreal
 * coordinate conversion may happen (format section 2 / ADR 004). Nothing in this
 * file calls a SolverToUnreal* function.
 *
 * BYTES COME BACK LITTLE-ENDIAN, as stored. The reader itself is
 * endian-independent - every header field is assembled byte by byte, and the
 * background pattern it synthesises for absent bricks is written out
 * little-endian to match real payloads - but a consumer that reinterprets the
 * returned bytes as floats is assuming a little-endian host.
 */

/* -------------------------------------------------------------------------- */
/* Format constants and reader policy limits                                    */
/* -------------------------------------------------------------------------- */

namespace CFDViz
{
	/** CVF header flag bits (format section 4.1). These numeric values are on-disk format. */
	namespace Cvf
	{
		/**
		 * Bricks whose every voxel equals backgroundValue may be absent from the
		 * directory (section 4.4.5).
		 *
		 * The reader treats an absent brick as backgroundValue whether or not this
		 * bit is set, because section 4.4.5 defines the meaning of absence
		 * unconditionally. The bit is a promise from the writer that omission was
		 * deliberate, which a consumer can use to skip a background prefill when it
		 * is clear.
		 */
		inline constexpr uint32 FlagSparse = 1u << 0;

		/** Every flag bit this reader understands. Anything else is rejected, not ignored. */
		inline constexpr uint32 KnownFlags = FlagSparse;
	}

	/**
	 * Largest single buffer this reader will produce, in bytes.
	 *
	 * Not a format limit - TArray's. TArray indexes with int32, so a byte array
	 * cannot hold more than MAX_int32 elements however much memory exists. A CVF
	 * header can legitimately declare a dense volume far larger than that (a
	 * 1024^3 float32 vector field is 12 GB, and a file with no bricks at all
	 * still declares its full extent), so the ceiling has to surface as a
	 * specific ECFDVizError::AllocationTooLarge rather than as an allocation
	 * failure or a truncated read. Volumes past this point must be consumed brick
	 * by brick with ReadBrick, which is what the bricked layout is for.
	 */
	inline constexpr int64 MaxReadableBytes = MAX_int32;

	/**
	 * Largest grid dimension this reader will address, per axis.
	 *
	 * The format stores dimensions as uint32 and the schema permits the full
	 * range, but every index, extent and stride in this reader is int32-based
	 * (FIntVector, TArray), and a point-associated field adds one to each axis.
	 * Capping one below MAX_int32 means that +1 cannot overflow, so no bounds
	 * check downstream can be defeated by a wrapped extent - the exact failure
	 * format rule 1.5 exists to prevent. A file beyond this is rejected as
	 * AllocationTooLarge, which is honest: the file may be legal, this build
	 * simply cannot address it.
	 */
	inline constexpr int64 MaxGridDimension = static_cast<int64>(MAX_int32) - 1;
}

/* -------------------------------------------------------------------------- */
/* Header - format section 4.1                                                  */
/* -------------------------------------------------------------------------- */

/**
 * The fixed 128-byte CVF header (section 4.1).
 *
 * The in-memory layout below is deliberately unrelated to the on-disk layout;
 * the on-disk offsets live in the comments and in the parser.
 *
 * DIMENSIONS ARE CELL COUNTS, NOT VALUE COUNTS. The manifest schema says so in
 * as many words ("Cell counts [nx, ny, nz]. Stored as uint32 in the CVF
 * header"), so how many values the file actually stores depends on `Association`
 * (format section 3.2):
 *
 *     Cell   nx * ny * nz values
 *     Point  (nx+1) * (ny+1) * (nz+1) values
 *
 * That +1 is why `Association` is in the header at all. Getting it wrong shifts
 * an entire field by half a voxel - which renders plausibly and is wrong - or
 * overruns the brick tiling. Use GetValueCounts(); never tile or index with
 * Dimensions directly.
 */
struct FCFDVizVolumeHeader
{
	/** Offset 8. Must be 128. A different value means a different container layout, not a longer header to skip over. */
	uint32 HeaderBytes = 0;

	/** Offsets 12 / 14. Major must be 1; a newer minor is accepted (format rule 1.4). */
	uint16 MajorVersion = 0;
	uint16 MinorVersion = 0;

	/**
	 * Offset 16. Must read back as 0x01020304.
	 *
	 * Retained after validation rather than discarded so a diagnostic can show
	 * what was actually on disk. Anything else is rejected as foreign byte order
	 * and never byte-swapped (format rule 1.1).
	 */
	uint32 EndianMarker = 0;

	/**
	 * Offset 20. Only CFDViz::Cvf::KnownFlags may be set.
	 *
	 * A bit outside that mask is REJECTED, not ignored. Format rule 1.4 accepts a
	 * newer minor version only when every construct in the file is understood,
	 * and an unknown header bit could change how payloads decode; reading on "as
	 * if the bit were clear" would silently produce wrong data. The Python
	 * reference rejects the same bits with the same message, so the two
	 * implementations agree about which files are loadable.
	 */
	uint32 Flags = 0;

	/** Offset 24. Which frame this file belongs to. */
	uint32 FrameIndex = 0;

	/** Offset 28. Ties the file back to its manifest entry without relying on the filename. */
	uint32 FieldNumericId = 0;

	/** Offset 32. Physical time, in the manifest's time unit. */
	double SimulationTime = 0.0;

	/** Offsets 40/44/48. Cell counts; the association decides how many values that implies. Each in 1..CFDViz::MaxGridDimension. */
	FIntVector Dimensions = FIntVector(0, 0, 0);

	/** Offsets 52/54/56. Brick edge lengths in voxels, 1..65535. Bricks tile the VALUE extent, not the cell extent. */
	FIntVector BrickSize = FIntVector(0, 0, 0);

	/** Offset 58. 1..4; the header's float32[4] background and the directory's float32[4] statistics are what cap it at 4. */
	int32 ComponentCount = 0;

	/** Offset 59. float64 is rejected for CVF in 1.0 (format section 3.3). */
	ECFDVizDataType DataType = ECFDVizDataType::Float32;

	/** Offset 60. Decides the +1 per axis on the value extent. */
	ECFDVizAssociation Association = ECFDVizAssociation::Cell;

	/** Offset 61. Zstd is rejected at parse time with exactly the section 7 wording. */
	ECFDVizCodec Codec = ECFDVizCodec::Zlib;

	/** Offset 64. Bricks whose voxels all equal BackgroundValue MAY be omitted (section 4.4.5), so this can be fewer than the tiling implies - and may be 0. */
	int64 BrickCount = 0;

	/** Offset 72. Start of the brick directory. */
	int64 DirectoryOffset = 0;

	/** Offset 80. First payload byte. Informational: every directory entry carries its own absolute offset, and that is what the reader bounds-checks. */
	int64 PayloadOffset = 0;

	/**
	 * Offset 88, float32[4]. The value of every voxel in a brick omitted from the
	 * directory (section 4.4.5) - omitting uniform bricks is how empty space
	 * costs nothing. Only the first ComponentCount entries are meaningful.
	 *
	 * This is data, not metadata: it is materialised into payload bytes by
	 * ReadDense and ReadVoxel, so format rule 1.7 applies and a NaN background
	 * must survive. It is only ever moved by FMemory::Memcpy, never computed
	 * with, so its bit pattern is preserved exactly. See
	 * FCFDVizVolumeReader::GetBackgroundVoxelBytes for what happens when the
	 * storage type is narrower than float32.
	 */
	float BackgroundValue[CFDViz::MaxCvfComponentCount] = { 0.0f, 0.0f, 0.0f, 0.0f };

	/**
	 * Offset 104. CRC-32C over bytes [0, 128) with bytes [104, 108) - this field -
	 * treated as zero.
	 *
	 * The field is blanked, not skipped: those four zero bytes still take part in
	 * the checksum. Checksumming the stored CRC alongside the data it protects
	 * would make the check unsatisfiable, and skipping the bytes entirely would
	 * give a different, incompatible answer from the Python reference.
	 */
	uint32 HeaderCrc32C = 0;

	/**
	 * Parse and validate 128 header bytes.
	 *
	 * Validation order matches the Python reference exactly, so a file that is
	 * wrong in several ways at once is diagnosed identically by both readers
	 * rather than each blaming a different field: magic, byte order, major
	 * version, headerBytes, unknown flags, reserved bytes, dimensions, brick
	 * size, component count, data type, association, codec, and the header CRC
	 * last. Structural checks run before the CRC so that a file which is simply
	 * not a CVF reports *that*, rather than a checksum mismatch nobody can act
	 * on.
	 *
	 * @param Bytes      At least CFDViz::CvfHeaderBytes bytes; extra is ignored.
	 * @param Out        Written only on success.
	 * @param bVerifyCrc Leave true. False exists for the section 10 validator,
	 *                   which must *report* a bad header CRC alongside the rest of
	 *                   the file rather than stop at it.
	 * @return Ok, or a failure naming the field and its byte offset. The result
	 *         carries no file path - the caller knows which file it read and
	 *         stamps FCFDVizResult::FilePath itself.
	 *
	 *         The one exception is codec = zstd, which returns Message set to
	 *         exactly CFDViz::ZstdRejectionMessage and nothing else, because
	 *         section 7 pins that user-facing wording. Do not "improve" it, do
	 *         not append context to Message, and never fall back to another
	 *         codec.
	 */
	FLOWVIZRUNTIME_API static FCFDVizResult Parse(TArrayView<const uint8> Bytes, FCFDVizVolumeHeader& Out, bool bVerifyCrc = true);

	/** CRC-32C over 128 header bytes with [104, 108) blanked (section 4.1). Returns 0 for a short span. */
	FLOWVIZRUNTIME_API static uint32 ComputeHeaderCrc(TArrayView<const uint8> Bytes);

	/** True when the writer promised that background bricks may be omitted. Absence still means backgroundValue either way (section 4.4.5). */
	bool IsSparse() const
	{
		return (Flags & CFDViz::Cvf::FlagSparse) != 0;
	}

	/** Bytes per stored component. 0 for a data type this build does not know, which then fails every size equality below rather than becoming a bare stride. */
	int32 GetElementBytes() const
	{
		return SizeOfDataType(DataType);
	}

	/** Bytes for one complete voxel: ComponentCount * GetElementBytes(). The stride of the dense volume, and the size of a single ReadVoxel result. */
	int64 GetVoxelBytes() const
	{
		return static_cast<int64>(ComponentCount) * static_cast<int64>(GetElementBytes());
	}

	/**
	 * Values per axis for this field's association - (nx,ny,nz) for Cell,
	 * (nx+1,ny+1,nz+1) for Point. This is the only place the +1 is applied.
	 *
	 * Returns (0,0,0) for a header that has not been validated, so a caller that
	 * skipped Parse gets a count no index can satisfy rather than a plausible
	 * wrong one.
	 */
	FLOWVIZRUNTIME_API FIntVector GetValueCounts() const;

	/** Total stored values. INDEX_NONE on a degenerate header or on overflow; treat that as a rejection, never as a size. */
	FLOWVIZRUNTIME_API int64 GetValueCount() const;

	/** Dense volume size in bytes: values * components * sizeof(dataType). INDEX_NONE on overflow or an unknown type. The section 4.4.4 allocation guard, applied to the whole volume. */
	FLOWVIZRUNTIME_API int64 GetDenseVolumeBytes() const;

	/**
	 * Bricks along each axis: ceil(valueCount / brickSize) per axis.
	 *
	 * The directory may hold fewer entries than the product of these, because a
	 * uniform brick may be omitted (section 4.4.5). It must never hold more, and
	 * must never name a brick outside this range - FCFDVizVolumeReader::Open
	 * enforces both.
	 */
	FLOWVIZRUNTIME_API FIntVector GetBrickCounts() const;

	/** Bricks a fully dense file would carry. INDEX_NONE on overflow. The upper bound on BrickCount. */
	FLOWVIZRUNTIME_API int64 GetTotalBrickCount() const;

	/**
	 * Voxels the brick at this brick coordinate must contain along each axis.
	 *
	 * EDGE BRICKS ARE NOT PADDED (section 4.4.3): the last brick on an axis holds
	 * exactly the remainder, so this is min(brickSize, extent - index*brickSize)
	 * per axis. Reconstructing an edge brick as though it were a full brickSize
	 * cube is the classic CVF bug - it reads past the end of the decoded buffer
	 * and shears every row after it, which looks like plausible turbulence.
	 *
	 * @return (0,0,0) when the brick coordinate is outside the tiling.
	 */
	FLOWVIZRUNTIME_API FIntVector GetValidSizeForBrick(const FIntVector& BrickCoordinate) const;

	/** True when (I,J,K) addresses a stored value, i.e. lies inside GetValueCounts(). */
	FLOWVIZRUNTIME_API bool ContainsValue(int32 I, int32 J, int32 K) const;
};

/* -------------------------------------------------------------------------- */
/* Brick directory entry - format section 4.3                                   */
/* -------------------------------------------------------------------------- */

/**
 * One 80-byte brick directory entry (section 4.3).
 *
 * A brick that is present is described completely by its entry: where its
 * compressed bytes live, how many there are, what they expand to, and the CRC of
 * the bytes AS STORED. That last point is what makes integrity checkable without
 * decompressing anything.
 *
 * A brick that is absent from the directory is not an error: section 4.4.5 lets
 * a writer omit any brick whose voxels all equal backgroundValue, and such a
 * brick evaluates to backgroundValue everywhere.
 */
struct FCFDVizBrickEntry
{
	/** Offsets 0/4/8. Brick coordinate, in bricks - multiply by the header's BrickSize for the brick's first voxel. */
	FIntVector BrickIndex = FIntVector(0, 0, 0);

	/**
	 * Offsets 12/14/16. Voxels actually stored along each axis.
	 *
	 * Smaller than BrickSize at the far edge of the volume, because edge bricks
	 * are NOT padded (section 4.4.3). Assuming a full brick here reads past the
	 * payload and shears the data by a row.
	 */
	FIntVector ValidSize = FIntVector(0, 0, 0);

	/**
	 * Offset 18. No per-brick flag bits are defined in 1.0, so any bit set here
	 * is rejected - an unknown per-brick bit could change how the payload
	 * decodes, and decoding it anyway would produce plausible wrong voxels.
	 */
	uint16 Flags = 0;

	/** Offset 20. Absolute, from the start of the file. The CRC covers exactly CompressedBytes bytes here. */
	int64 PayloadOffset = 0;

	/** Offset 28. Stored length. For codec None this must equal UncompressedBytes. */
	int64 CompressedBytes = 0;

	/**
	 * Offset 32. Decoded length.
	 *
	 * Section 4.4.4 makes this normatively equal to
	 * validX * validY * validZ * componentCount * sizeof(dataType), and a reader
	 * MUST verify that equality BEFORE allocating. It is the allocation guard,
	 * not a sanity check: it is the primary defence against a hostile size field,
	 * and it is worthless if it runs after the allocation it is supposed to
	 * guard. FCFDVizVolumeReader::Open enforces it for every entry, at Open time,
	 * so no later call can reach an allocation with an unverified size.
	 */
	int64 UncompressedBytes = 0;

	/**
	 * Offsets 36 / 52, float32[4]. Per-component extremes over this brick,
	 * ignoring NaN and cells the mask field rejects.
	 *
	 * A brick whose every value is NaN or masked stores min = +inf and
	 * max = -inf (section 4.4.7). That is "no valid data", NOT a range - feeding
	 * it to a colour map produces an inverted, meaningless scale - so read it
	 * through TryGetComponentRange rather than directly.
	 */
	float ComponentMin[CFDViz::MaxCvfComponentCount] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float ComponentMax[CFDViz::MaxCvfComponentCount] = { 0.0f, 0.0f, 0.0f, 0.0f };

	/** Offset 68. CRC-32C over the COMPRESSED bytes as stored - not over the decoded voxels - so integrity is checkable without decoding. */
	uint32 PayloadCrc32C = 0;

	/**
	 * Parse one 80-byte directory entry.
	 *
	 * Only self-consistency is checked here: reserved bytes zero, flags zero,
	 * offsets representable. Everything that needs the header - the section 4.4.4
	 * size equality, the tiling, the file bounds - is checked by
	 * FCFDVizVolumeReader::Open, which is the only place that knows all three.
	 *
	 * @param Bytes      At least CFDViz::CvfDirectoryEntryBytes bytes; extra ignored.
	 * @param Out        Written only on success.
	 * @param FileOffset Offset of this entry in the file, used only so the byte
	 *                   offset in a failure points at the real file position.
	 */
	FLOWVIZRUNTIME_API static FCFDVizResult Parse(TArrayView<const uint8> Bytes, FCFDVizBrickEntry& Out, int64 FileOffset = 0);

	/** Voxels stored in this brick: validX * validY * validZ. Cannot overflow - three uint16 factors reach at most 2^48. */
	int64 GetVoxelCount() const
	{
		return static_cast<int64>(ValidSize.X) * static_cast<int64>(ValidSize.Y) * static_cast<int64>(ValidSize.Z);
	}

	/**
	 * The section 4.4.4 equality, computed in int64 so no hostile combination can
	 * wrap into agreement with the stored uint32.
	 *
	 * @return Expected uncompressed bytes, or INDEX_NONE on overflow or an
	 *         unknown data type - both of which then fail the comparison.
	 */
	int64 ComputeExpectedUncompressedBytes(int32 ComponentCount, ECFDVizDataType DataType) const
	{
		return CFDViz::ComputePayloadBytes(GetVoxelCount(), ComponentCount, DataType);
	}

	/**
	 * Range of one component over this brick.
	 *
	 * @return false, leaving the out parameters untouched, for an out-of-range
	 *         component or for the +inf / -inf "no valid data" sentinel of section
	 *         4.4.7. A false return means "do not build a range from this brick",
	 *         not "use zero".
	 */
	FLOWVIZRUNTIME_API bool TryGetComponentRange(int32 Component, float& OutMin, float& OutMax) const;
};

/* -------------------------------------------------------------------------- */
/* Reader                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * Random-access reader for one .cvf file.
 *
 * Two-phase on purpose. Open() reads the 128-byte header and the brick
 * directory, proves every offset and length against the real file size, and
 * stops - small, bounded, and enough to plan work, so it is the right call for
 * listing a case's metadata. Decoding then happens per brick (ReadBrick), per
 * voxel (ReadVoxel), or for the whole volume (ReadDense); VerifyAllCrcs checks
 * integrity without decompressing anything at all.
 *
 * ReadVoxel and ReadDense share the same tiling arithmetic and the same
 * background pattern by construction, not by convention. The cross-language
 * known_values bridge (format section 9) compares single voxels read through
 * ReadVoxel against the Python reference, so a ReadVoxel that disagreed with
 * ReadDense would make a case pass or fail depending on which path a caller
 * happened to take.
 *
 * See the file comment for thread safety: worker-thread safe, not internally
 * synchronised, one instance per thread.
 */
class FLOWVIZRUNTIME_API FCFDVizVolumeReader
{
public:
	FCFDVizVolumeReader();
	~FCFDVizVolumeReader();

	// The reader owns a file handle, so it is move-only. Declared here and
	// defined in the .cpp so the owned source's destructor is instantiated in
	// exactly one place.
	FCFDVizVolumeReader(const FCFDVizVolumeReader&) = delete;
	FCFDVizVolumeReader& operator=(const FCFDVizVolumeReader&) = delete;
	FCFDVizVolumeReader(FCFDVizVolumeReader&&);
	FCFDVizVolumeReader& operator=(FCFDVizVolumeReader&&);

	/**
	 * Open a .cvf, validating the header and the entire brick directory.
	 *
	 * Every offset and length is checked against the file's ACTUAL size before
	 * anything is allocated (format rule 1.5), and every brick's
	 * UncompressedBytes is checked against
	 * validX * validY * validZ * componentCount * sizeof(dataType) (section
	 * 4.4.4). Brick coordinates must lie inside the tiling and must be unique,
	 * and each brick's valid sizes must equal the exact remainder the tiling
	 * implies - a brick claiming a full brickSize cube at the edge of the volume
	 * is rejected here rather than overrunning a buffer later.
	 *
	 * No payload byte is read, so this stays cheap even for a multi-gigabyte
	 * frame.
	 *
	 * @param Path             Path to the .cvf.
	 * @param bVerifyHeaderCrc Leave true. False exists only for the section 10
	 *                         validator, which has to open a file with a corrupt
	 *                         header CRC in order to report it.
	 * @return Ok, or a failure naming the file and the offending byte offset.
	 *         Opening an already-open reader closes the previous file first; on
	 *         failure the reader is left closed, never half-open.
	 */
	FCFDVizResult Open(const FString& Path, bool bVerifyHeaderCrc = true);

	/**
	 * Open over a caller-supplied byte source.
	 *
	 * The source is BORROWED, not owned, and must outlive the reader. This is the
	 * entry point tests use with FCFDVizMemoryByteSource: a hand-built 128-byte
	 * header asserts what the parser makes of specific bytes, which a round-trip
	 * through this codebase's own writer cannot do - a round-trip passes even
	 * when reader and writer share the same wrong idea of the layout.
	 */
	FCFDVizResult Open(const ICFDVizByteSource& InSource, bool bVerifyHeaderCrc = true);

	/** Release the file handle and all parsed state. Safe to call when not open. */
	void Close();

	/** True between a successful Open and a Close. Every read below fails with InvalidHeader when this is false. */
	bool IsOpen() const
	{
		return bIsOpen;
	}

	/** The validated header. Only meaningful while IsOpen(). */
	const FCFDVizVolumeHeader& GetHeader() const
	{
		return Header;
	}

	/**
	 * The validated brick directory, in file order.
	 *
	 * Sparse by design: a brick made entirely of backgroundValue may be absent
	 * (section 4.4.5), so this is NOT indexed by brick coordinate. Use
	 * FindBrickByCoordinate to go from a coordinate to an index.
	 */
	const TArray<FCFDVizBrickEntry>& GetBricks() const
	{
		return Bricks;
	}

	/** Spelling used by the sibling CFDViz readers. Identical to GetBricks(). */
	const TArray<FCFDVizBrickEntry>& GetBrickEntries() const
	{
		return Bricks;
	}

	/** Entries present in the directory - not the number of bricks in the tiling, which is Header.GetTotalBrickCount(). */
	int32 GetBrickCount() const
	{
		return Bricks.Num();
	}

	/** Path passed to Open, or the source's display name. */
	const FString& GetFilePath() const
	{
		return FilePath;
	}

	/** Size of the file as measured at Open, in bytes. Every bounds check is against this, not against a size taken from the header. */
	int64 GetFileSize() const
	{
		return FileSize;
	}

	/**
	 * Find the directory slot for a brick coordinate.
	 *
	 * @return INDEX_NONE when that brick was omitted, which per section 4.4.5
	 *         means every voxel in it equals backgroundValue - a legitimate and
	 *         common answer, not an error.
	 */
	int32 FindBrickByCoordinate(const FIntVector& BrickCoordinate) const;

	/**
	 * Parse a header from exactly 128 bytes, without needing the rest of the
	 * file. A thin convenience over FCFDVizVolumeHeader::Parse that stamps
	 * DisplayPath into the result so the failure names a file.
	 */
	static FCFDVizResult ParseHeader(
		TArrayView<const uint8> HeaderBytes,
		const FString& DisplayPath,
		FCFDVizVolumeHeader& OutHeader,
		bool bVerifyCrc = true);

	/**
	 * Decode exactly one brick.
	 *
	 * @param BrickIndex Index into GetBricks(), NOT a brick coordinate.
	 * @param OutRaw     Resized to the entry's UncompressedBytes and filled with
	 *                   the decoded voxels: X fastest, then Y, then Z, components
	 *                   interleaved per voxel, in the file's own storage type. No
	 *                   value is converted, scaled or clamped, so a float16 field
	 *                   yields IEEE binary16 bytes and NaN payloads survive
	 *                   bit-exactly. Emptied on failure - never left holding
	 *                   partially decoded bytes that could pass for data.
	 * @param bVerifyCrc Verify the payload CRC over the compressed bytes BEFORE
	 *                   decoding. Handing corrupt bytes to an inflater is how a
	 *                   reader turns a data error into a crash, which is why the
	 *                   order matters. Section 4.4.8 permits a validator to mark a
	 *                   failing brick unavailable and carry on, which is the only
	 *                   reason this can be turned off - a reader MUST NOT silently
	 *                   substitute zeros, so turning it off means the caller has
	 *                   taken on reporting the corruption.
	 * @return Ok, or a failure naming the file, the brick and the byte offset.
	 */
	FCFDVizResult ReadBrick(int32 BrickIndex, TArray<uint8>& OutRaw, bool bVerifyCrc = true);

	/**
	 * Reconstruct the whole dense volume.
	 *
	 * Same layout as a brick, over the whole value extent: X fastest, then Y,
	 * then Z, components interleaved per voxel, storage type unchanged. Bricks
	 * absent from the directory are filled with backgroundValue (section 4.4.5)
	 * materialised into the storage type - see GetBackgroundVoxelBytes for what
	 * that means when the storage type is narrower than float32.
	 *
	 * @param OutVolume  Resized to Header.GetDenseVolumeBytes(). Emptied on
	 *                   failure - never partially filled, never zero-filled.
	 * @param MaxBytes   Refuse to allocate more than this, reporting
	 *                   ECFDVizError::AllocationTooLarge. Defaults to
	 *                   CFDViz::MaxReadableBytes, TArray's own ceiling; pass less
	 *                   to impose a budget. This cap is not redundant with the
	 *                   per-brick size check: a fully self-consistent header can
	 *                   still declare a terabyte of empty space, and because
	 *                   absent bricks cost no file bytes there is no file-size
	 *                   correlate to catch it. This is the only guard.
	 * @param bVerifyCrc As ReadBrick.
	 * @return Ok, or the first failure encountered.
	 */
	FCFDVizResult ReadDense(TArray<uint8>& OutVolume, int64 MaxBytes = CFDViz::MaxReadableBytes, bool bVerifyCrc = true);

	/**
	 * Read one voxel, decoding ONLY the brick that contains it.
	 *
	 * This is what the known_values cross-language bridge calls (format section
	 * 9), and it is byte-for-byte identical to the corresponding slice of
	 * ReadDense by construction: both go through GetValidSizeForBrick and
	 * GetBackgroundVoxelBytes.
	 *
	 * @param I,J,K    VALUE indices, in [0, extent) per axis. For a
	 *                 point-associated field the legal range is one larger per
	 *                 axis than the cell dimension - see the header's note on the
	 *                 +1.
	 * @param OutValue Resized to Header.GetVoxelBytes() and filled with the
	 *                 interleaved components of that voxel, unconverted. Emptied
	 *                 on failure.
	 * @return Ok, IndexOutOfRange for an index outside the extent, or a decode
	 *         failure. When the containing brick is absent the result is
	 *         backgroundValue.
	 */
	FCFDVizResult ReadVoxel(int32 I, int32 J, int32 K, TArray<uint8>& OutValue);

	/**
	 * Verify the header CRC and every brick's payload CRC WITHOUT decompressing.
	 *
	 * Payload CRCs are defined over the compressed bytes exactly as stored
	 * (section 4.3), so this streams the file in fixed-size chunks and never
	 * allocates a whole brick. It is the cheap integrity pass for the section 10
	 * validation report.
	 *
	 * @return Ok, or the FIRST failure, naming the brick and its byte offset. Use
	 *         VerifyAllPayloadCrcs when the report needs every bad brick rather
	 *         than the first, which is what section 4.4.8's "mark the brick
	 *         unavailable and continue" needs.
	 */
	FCFDVizResult VerifyAllCrcs();

	/**
	 * Verify every brick's stored CRC, collecting all failures instead of
	 * stopping at the first.
	 *
	 * @param OutFailedBricks Directory slots whose CRC did not match; emptied
	 *                        first, so it is empty on a clean file.
	 * @return PayloadCrcMismatch when any brick failed - so a caller that only
	 *         checks IsOk() still gets the right answer - or a read failure, or
	 *         Ok when every brick verified. The list carries the detail the
	 *         validation report needs to name each bad brick.
	 */
	FCFDVizResult VerifyAllPayloadCrcs(TArray<int32>& OutFailedBricks);

	/** Verify one brick's stored-payload CRC, without decompressing it. BrickIndex is an index into GetBricks(). */
	FCFDVizResult VerifyBrickCrc(int32 BrickIndex);

	/** Verify the header CRC by re-reading the 128 header bytes from the source. */
	FCFDVizResult VerifyHeaderCrc();

	/**
	 * backgroundValue materialised into exactly one voxel's worth of storage
	 * bytes, little-endian, as an absent brick's payload would have been stored.
	 *
	 * Exposed because a caller doing its own sparse traversal needs exactly these
	 * bytes to stay consistent with ReadDense.
	 *
	 * float32 storage is a straight bit copy. float16 storage rounds to nearest,
	 * ties to even, and PRESERVES NaN payload bits, matching numpy's float32 to
	 * float16 cast - which is what the Python reference performs, so the two
	 * implementations produce identical background bytes. Note that this is
	 * deliberately NOT FPlatformMath::StoreHalf, which canonicalises every NaN to
	 * 0x7E00 and would therefore disagree. uint8 storage requires the stored
	 * float to be an exact integer in [0, 255]; anything else is reported as
	 * InvalidHeader rather than rounded or clamped, because rounding it would be
	 * exactly the silent quantisation format rule 1.6 forbids - a fractional
	 * uint8 background is a writer bug, and truncating it would hide it.
	 *
	 * @param OutPattern Resized to Header.GetVoxelBytes(). Emptied on failure.
	 */
	FCFDVizResult GetBackgroundVoxelBytes(TArray<uint8>& OutPattern) const;

private:
	/** Shared tail of both Open overloads: parse the header, then the directory. */
	FCFDVizResult OpenInternal(bool bVerifyHeaderCrc);

	/** Parse and validate the directory, streamed in chunks so a large directory needs no large transient buffer. */
	FCFDVizResult ReadDirectory();

	/** Cross-check one entry against the header: tiling, valid sizes, the section 4.4.4 size equality, file bounds. */
	FCFDVizResult ValidateBrickEntry(const FCFDVizBrickEntry& Entry, int64 EntryOffset) const;

	/** Read, optionally CRC-check, and decode one brick into Buffer. Buffer is emptied on failure. */
	FCFDVizResult DecodeBrickInto(int32 BrickIndex, TArray<uint8>& Buffer, bool bVerifyCrc);

	/** Guard shared by every read entry point: the reader is open and the header is usable. */
	FCFDVizResult RequireOpen() const;

	/** A failure carrying this reader's path. */
	FCFDVizResult Fail(ECFDVizError Error, FString Message, int64 ByteOffset = INDEX_NONE) const;

	FString FilePath;
	int64 FileSize = 0;
	bool bIsOpen = false;

	FCFDVizVolumeHeader Header;
	TArray<FCFDVizBrickEntry> Bricks;

	/** Brick coordinate to index into Bricks. Built once at Open so ReadVoxel is a hash lookup rather than a scan of a sparse directory. */
	TMap<FIntVector, int32> BrickLookup;

	/**
	 * The source every read goes through. Points at OwnedSource for
	 * Open(const FString&) and at the caller's object for the borrowing overload,
	 * so there is exactly one read path either way.
	 */
	const ICFDVizByteSource* Source = nullptr;

	/** Set only by Open(const FString&). Holds the IFileHandle; reads are seek-then-read, so the handle is per-reader state. */
	TUniquePtr<FCFDVizFileByteSource> OwnedSource;

	/** Buffer ReadDense and ReadVoxel decode into, kept as a member so the brick loop does not build a fresh TArray per brick. */
	TArray<uint8> DecodeScratch;
};
