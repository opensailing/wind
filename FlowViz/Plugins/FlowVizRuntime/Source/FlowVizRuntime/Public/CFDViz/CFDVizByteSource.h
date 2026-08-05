// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

class IFileHandle;

/**
 * Random-access byte source for the CFDViz readers.
 *
 * The readers are written against this rather than against a path or a
 * pre-slurped TArray<uint8> for two reasons, both load-bearing:
 *
 *  1. RANDOM ACCESS IS THE POINT OF CVF. The brick directory exists so a viewer
 *     can decode one brick, or a region of interest, without touching the rest
 *     of a multi-gigabyte volume (format section 4). A reader that read whole
 *     files would make the directory decorative.
 *  2. TESTS MUST ASSERT BYTES, NOT ROUND-TRIPS. A round-trip through one
 *     implementation passes even when the reader and the writer share the same
 *     wrong idea of the layout. FCFDVizMemoryByteSource lets a test feed in a
 *     hand-built byte array - or bytes the Python reference actually wrote - and
 *     assert what the reader makes of them.
 *
 * Implementations must be usable from a worker thread (engineering rule 1: no
 * game-thread file I/O). They need not be safe to use from several threads at
 * once; give each thread its own source.
 */
class FLOWVIZRUNTIME_API ICFDVizByteSource
{
public:
	virtual ~ICFDVizByteSource() = default;

	/** Total readable size in bytes, or 0 when the source is not open. */
	virtual int64 GetSize() const = 0;

	/**
	 * Copy Count bytes starting at Offset into OutBuffer.
	 *
	 * @return false without touching OutBuffer when the span is not entirely
	 *         inside the source, or when the underlying read failed. Callers must
	 *         treat false as a hard error; a short read is never reported as
	 *         success with fewer bytes, because a caller that then parsed the
	 *         uninitialised tail would be parsing stack garbage.
	 */
	virtual bool Read(int64 Offset, int64 Count, void* OutBuffer) const = 0;

	/** Name for error messages - a file path, or a synthetic label for memory. */
	virtual const FString& GetDisplayPath() const = 0;

	/** True when Count bytes starting at Offset lie entirely inside the source. */
	bool Contains(int64 Offset, int64 Count) const
	{
		// Count is compared against the remaining length rather than added to
		// Offset: Offset + Count is exactly the sum that overflows when a corrupt
		// header claims a length near 2^63, which would turn this check into a
		// rubber stamp (format rule 1.5).
		const int64 Size = GetSize();
		return Offset >= 0 && Count >= 0 && Offset <= Size && Count <= Size - Offset;
	}
};

/**
 * A byte source over memory the caller owns.
 *
 * Does NOT copy and does NOT take ownership: the referenced bytes must outlive
 * this object. That is deliberate - it is the zero-allocation path used for
 * hand-built test vectors and for buffers already resident.
 */
class FLOWVIZRUNTIME_API FCFDVizMemoryByteSource final : public ICFDVizByteSource
{
public:
	explicit FCFDVizMemoryByteSource(TArrayView<const uint8> InBytes, FString InDisplayPath = TEXT("<memory>"))
		: Bytes(InBytes)
		, DisplayPath(MoveTemp(InDisplayPath))
	{
	}

	virtual int64 GetSize() const override
	{
		return Bytes.Num();
	}

	virtual bool Read(int64 Offset, int64 Count, void* OutBuffer) const override
	{
		if (!Contains(Offset, Count))
		{
			return false;
		}
		if (Count > 0)
		{
			FMemory::Memcpy(OutBuffer, Bytes.GetData() + Offset, static_cast<SIZE_T>(Count));
		}
		return true;
	}

	virtual const FString& GetDisplayPath() const override
	{
		return DisplayPath;
	}

private:
	TArrayView<const uint8> Bytes;
	FString DisplayPath;
};

/**
 * A byte source over a file, opened for reading and seeked per request.
 *
 * Safe to construct and use on a worker thread, which is required: decompressing
 * or even reading a CFDViz case on the game thread hitches the renderer
 * (engineering rule 1).
 */
class FLOWVIZRUNTIME_API FCFDVizFileByteSource final : public ICFDVizByteSource
{
public:
	FCFDVizFileByteSource() = default;
	~FCFDVizFileByteSource();

	FCFDVizFileByteSource(const FCFDVizFileByteSource&) = delete;
	FCFDVizFileByteSource& operator=(const FCFDVizFileByteSource&) = delete;

	/** Open a file for reading. Returns false when it does not exist or cannot be opened. */
	bool Open(const FString& InPath);

	/** True when a file is currently open. */
	bool IsOpen() const
	{
		return Handle != nullptr;
	}

	void Close();

	virtual int64 GetSize() const override
	{
		return FileSize;
	}

	virtual bool Read(int64 Offset, int64 Count, void* OutBuffer) const override;

	virtual const FString& GetDisplayPath() const override
	{
		return DisplayPath;
	}

private:
	IFileHandle* Handle = nullptr;
	int64 FileSize = 0;
	FString DisplayPath;
};
