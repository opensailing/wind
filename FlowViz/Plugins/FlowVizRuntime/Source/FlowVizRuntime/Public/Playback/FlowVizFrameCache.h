// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizTypes.h"

// CoreMinimal.h forward-declares TArrayView but does not define it, and
// GetEntries below returns one by value.
#include "Containers/ArrayView.h"

/**
 * The residency budget for playback - plan.md section 8, "Caching".
 *
 * WHAT THIS IS AND IS NOT. This is accounting, not storage. It holds no voxels
 * and no RHI resources; it decides WHICH frames are allowed to be resident and
 * WHICH must go, and it answers that question with no GPU present so the policy
 * can be tested exhaustively. The bytes themselves live in the decode task's
 * FFlowVizVolumeUpload (CPU) and in FFlowVizVolumeTextureSet's slots (GPU); this
 * class is what stops either from growing without bound.
 *
 * TWO BUDGETS, NOT ONE. plan.md asks for "configurable CPU and GPU memory
 * budgets", and they are genuinely independent: a 3-component float16 field
 * decodes to 6 bytes per voxel on the CPU and uploads as 8 (the 3 -> 4 channel
 * widening in FlowVizVolumeTexture.h), and a case with a status texture pays for
 * it on the GPU only. A single budget derived from one of the two silently
 * overcommits the other.
 *
 * THE THREE RULES THAT MAKE EVICTION SAFE.
 *
 *  1. A PINNED FRAME IS NEVER EVICTED. Frames A and B are being sampled by the
 *     shader every display frame. Evicting either tears the visible image, which
 *     is the same failure FlowVizVolumeRing::ChooseUploadSlot refuses at the slot
 *     level. Admit() returns AllocationTooLarge rather than evicting a pinned
 *     frame - the caller must retry once the display frames advance, exactly as
 *     FFlowVizVolumeTextureSet::EnqueueUpload documents for its own refusal.
 *
 *  2. EVICTION IS PLANNED BEFORE IT IS COMMITTED. A frame that cannot fit even
 *     after evicting everything evictable must leave the cache UNCHANGED. The
 *     obvious loop - evict until it fits, then check - empties the cache and then
 *     fails, so an oversized frame costs every resident frame and buys nothing.
 *     Admit() computes the full eviction set first and applies it only if the
 *     admission will then succeed.
 *
 *  3. PRESENT IS NOT DISPLAYABLE. An entry admitted for a frame still being
 *     decoded holds its budget (that is the point - the reservation is what stops
 *     four concurrent decodes from overcommitting) but is NOT resident: it has no
 *     complete set of field components yet. IsResident() requires bComplete, and
 *     it is what the display path must ask. Contains() is for budget bookkeeping
 *     only. Conflating them is precisely how a partially updated frame reaches
 *     the screen, which plan.md section 8 forbids.
 *
 * THREADING. Not internally synchronised, and deliberately so: every call comes
 * from the player's Tick on the game thread, where the decisions are made. The
 * decode work it schedules is what runs on a worker.
 */

namespace FlowVizCache
{
	/**
	 * Defaults, chosen against the shipped full-resolution case rather than
	 * round numbers: 128 x 64 x 24 cells x 4 channels x 2 bytes is 1.5 MiB per
	 * frame for the widened velocity texture, so 512 MiB holds a few hundred
	 * frames and 256 MiB of GPU budget comfortably exceeds the 3 slots
	 * FlowVizVolume::RecommendedBufferCount actually allocates. They are starting
	 * points, not limits - both are settable per case.
	 */
	inline constexpr int64 DefaultCpuBudgetBytes = 512ll * 1024ll * 1024ll;
	inline constexpr int64 DefaultGpuBudgetBytes = 256ll * 1024ll * 1024ll;

	/** A zero or negative budget admits nothing and would stall playback silently, so it is rejected at Initialize. */
	inline constexpr int64 MinBudgetBytes = 1;
}

/** One frame's reservation. Byte counts are what the caller declared, never measured here. */
struct FFlowVizFrameCacheEntry
{
	/** The stored frame this reservation is for. Unique within the cache. */
	int32 FrameIndex = INDEX_NONE;

	/** Decoded bytes held on the CPU for this frame. */
	int64 CpuBytes = 0;

	/** Texture bytes this frame occupies on the GPU. Differs from CpuBytes for a widened 3-component field. */
	int64 GpuBytes = 0;

	/** Monotonic counter stamped by Admit and Touch. Larger is more recent; the smallest is evicted first. */
	uint64 LastUseSerial = 0;

	/** Currently frame A or frame B. Never evicted while set - see rule 1. */
	bool bPinned = false;

	/**
	 * Every field component for this frame has arrived. Only a complete frame may
	 * be displayed; an incomplete one still costs its budget.
	 */
	bool bComplete = false;
};

/** Cache occupancy, for the diagnostics panel. */
struct FFlowVizFrameCacheStats
{
	int32 EntryCount = 0;
	int32 CompleteCount = 0;
	int32 PinnedCount = 0;
	int64 CpuBytes = 0;
	int64 GpuBytes = 0;
	int64 CpuBudgetBytes = 0;
	int64 GpuBudgetBytes = 0;

	/** Cumulative counters since Initialize. Hits and misses are counted by the player, not here. */
	int64 EvictionCount = 0;
	int64 AdmitCount = 0;
	int64 RefusalCount = 0;
};

/**
 * LRU residency accounting for decoded frames, under independent CPU and GPU
 * byte budgets.
 */
class FLOWVIZRUNTIME_API FFlowVizFrameCache
{
public:
	/**
	 * Set the budgets and drop every entry.
	 *
	 * @return Ok, or IndexOutOfRange naming the budget that was below
	 *         FlowVizCache::MinBudgetBytes. A cache that admits nothing is not a
	 *         small cache, it is a stalled player, so it is refused here rather
	 *         than discovered as a playback that never advances.
	 */
	FCFDVizResult Initialize(
		int64 InCpuBudgetBytes = FlowVizCache::DefaultCpuBudgetBytes,
		int64 InGpuBudgetBytes = FlowVizCache::DefaultGpuBudgetBytes);

	/**
	 * Change the budgets in place, evicting whatever no longer fits.
	 *
	 * @param OutEvicted Appended with every frame dropped, so the caller can cancel
	 *                   an in-flight decode for an incomplete one.
	 * @return Ok, or IndexOutOfRange for a budget below the minimum, in which case
	 *         nothing changes. Ok even when the surviving set still exceeds the new
	 *         budget because everything over it is pinned - shrinking a budget must
	 *         not tear the visible frame either.
	 */
	FCFDVizResult SetBudgets(int64 InCpuBudgetBytes, int64 InGpuBudgetBytes, TArray<int32>& OutEvicted);

	/**
	 * Reserve room for one frame, evicting least-recently-used unpinned entries as
	 * needed.
	 *
	 * Re-admitting a frame already present updates its byte counts and touches it,
	 * which is the path a re-decode after a cancelled load takes.
	 *
	 * @param OutEvicted Appended with every frame dropped to make room. Empty when
	 *                   the frame fitted, and - by rule 2 - empty on failure too.
	 * @return Ok, or AllocationTooLarge when the frame cannot fit even with every
	 *         unpinned entry gone. Treat that exactly as
	 *         FFlowVizVolumeTextureSet::EnqueueUpload's own AllocationTooLarge:
	 *         retry after the display frames advance, not an error to surface.
	 *         IndexOutOfRange for a negative frame index or a negative byte count.
	 */
	FCFDVizResult Admit(int32 FrameIndex, int64 InCpuBytes, int64 InGpuBytes, TArray<int32>& OutEvicted);

	/** Stamp this frame as most recently used. @return false when it is not present. */
	bool Touch(int32 FrameIndex);

	/**
	 * Pin frames A and B against eviction, unpinning everything else. Pass
	 * INDEX_NONE for either. A frame that is not present is ignored - pinning
	 * protects what IS resident and never requests a load, matching
	 * FFlowVizVolumeTextureSet::SetDisplayFrames.
	 */
	void SetPinnedFrames(int32 FrameA, int32 FrameB);

	/** Mark a frame's decode finished, making it displayable. @return false when it is not present. */
	bool MarkComplete(int32 FrameIndex);

	/** Present at all, complete or not. This is the budget question. */
	bool Contains(int32 FrameIndex) const;

	/** Present AND complete - the only frames that may be displayed. This is the display question. See rule 3. */
	bool IsResident(int32 FrameIndex) const;

	/** True when this frame is pinned as A or B. */
	bool IsPinned(int32 FrameIndex) const;

	/** Drop one frame. @return false when it was not present. Removes it even when pinned - the caller has said it is gone. */
	bool Remove(int32 FrameIndex);

	/** Drop everything. Budgets and cumulative counters survive. */
	void Reset();

	/** Read-only entries in no particular order, for diagnostics and tests. */
	TArrayView<const FFlowVizFrameCacheEntry> GetEntries() const
	{
		return Entries;
	}

	/** The entry for a frame, or nullptr. Invalidated by any mutation. */
	const FFlowVizFrameCacheEntry* FindEntry(int32 FrameIndex) const;

	FFlowVizFrameCacheStats GetStats() const;

	int64 GetCpuBytes() const { return CpuBytes; }
	int64 GetGpuBytes() const { return GpuBytes; }
	int64 GetCpuBudgetBytes() const { return CpuBudgetBytes; }
	int64 GetGpuBudgetBytes() const { return GpuBudgetBytes; }
	int32 Num() const { return Entries.Num(); }

	/**
	 * The frame Admit would evict first, or INDEX_NONE when nothing is evictable.
	 *
	 * Exposed because "least recently used" is the one property of this class a
	 * test can check without also trusting the byte accounting.
	 */
	int32 PeekEvictionCandidate() const;

private:
	int32 FindIndex(int32 FrameIndex) const;

	TArray<FFlowVizFrameCacheEntry> Entries;

	int64 CpuBudgetBytes = FlowVizCache::DefaultCpuBudgetBytes;
	int64 GpuBudgetBytes = FlowVizCache::DefaultGpuBudgetBytes;

	int64 CpuBytes = 0;
	int64 GpuBytes = 0;

	/** Stamped into FFlowVizFrameCacheEntry::LastUseSerial. Monotonic. */
	uint64 UseSerial = 0;

	int64 EvictionCount = 0;
	int64 AdmitCount = 0;
	int64 RefusalCount = 0;
};
