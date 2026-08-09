// Copyright FlowViz contributors. All Rights Reserved.

#include "Playback/FlowVizFrameCache.h"

/**
 * The three rules from the header, as code.
 *
 * The entry list is a flat TArray searched linearly. That is deliberate: the
 * cache holds tens of entries, not thousands, and every operation here already
 * walks the whole list to find an LRU minimum or to sum bytes. A TMap would add
 * a second structure to keep consistent with the array and buy nothing at this
 * size.
 */

namespace FlowVizFrameCacheDetail
{
	/**
	 * Would a set holding these totals be within budget?
	 *
	 * AT the budget is IN budget. A cache that evicts on `Used + New >= Budget`
	 * refuses the frame that exactly fills it, which makes a budget sized to hold
	 * exactly N frames hold N-1. Both spellings look right; only the boundary
	 * distinguishes them.
	 */
	FORCEINLINE bool FitsInBudget(int64 Used, int64 Budget)
	{
		return Used <= Budget;
	}
}

int32 FFlowVizFrameCache::FindIndex(int32 FrameIndex) const
{
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (Entries[Index].FrameIndex == FrameIndex)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

FCFDVizResult FFlowVizFrameCache::Initialize(int64 InCpuBudgetBytes, int64 InGpuBudgetBytes)
{
	if (InCpuBudgetBytes < FlowVizCache::MinBudgetBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("CPU budget %lld is below the minimum of %lld bytes; a cache that admits nothing "
					 "stalls playback rather than saving memory"),
				InCpuBudgetBytes, FlowVizCache::MinBudgetBytes));
	}
	if (InGpuBudgetBytes < FlowVizCache::MinBudgetBytes)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("GPU budget %lld is below the minimum of %lld bytes"),
				InGpuBudgetBytes, FlowVizCache::MinBudgetBytes));
	}

	Entries.Reset();
	CpuBudgetBytes = InCpuBudgetBytes;
	GpuBudgetBytes = InGpuBudgetBytes;
	CpuBytes = 0;
	GpuBytes = 0;
	UseSerial = 0;
	EvictionCount = 0;
	AdmitCount = 0;
	RefusalCount = 0;

	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizFrameCache::SetBudgets(int64 InCpuBudgetBytes, int64 InGpuBudgetBytes, TArray<int32>& OutEvicted)
{
	if (InCpuBudgetBytes < FlowVizCache::MinBudgetBytes || InGpuBudgetBytes < FlowVizCache::MinBudgetBytes)
	{
		// Nothing changes on refusal, including the budgets themselves.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("budgets (%lld CPU, %lld GPU) must both be at least %lld bytes"),
				InCpuBudgetBytes, InGpuBudgetBytes, FlowVizCache::MinBudgetBytes));
	}

	CpuBudgetBytes = InCpuBudgetBytes;
	GpuBudgetBytes = InGpuBudgetBytes;

	// Evict least-recently-used unpinned entries until the survivors fit. This
	// terminates: every iteration either removes an entry or finds nothing
	// evictable and stops.
	while (!FlowVizFrameCacheDetail::FitsInBudget(CpuBytes, CpuBudgetBytes)
		|| !FlowVizFrameCacheDetail::FitsInBudget(GpuBytes, GpuBudgetBytes))
	{
		const int32 Candidate = PeekEvictionCandidate();
		if (Candidate == INDEX_NONE)
		{
			// Everything left is pinned. Shrinking a budget must not tear the
			// visible frame, so this is Ok with the cache over budget; the excess
			// clears as soon as the display frames advance.
			break;
		}

		OutEvicted.Add(Candidate);
		Remove(Candidate);
		++EvictionCount;
	}

	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizFrameCache::Admit(int32 FrameIndex, int64 InCpuBytes, int64 InGpuBytes, TArray<int32>& OutEvicted)
{
	if (FrameIndex < 0)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("frame index %d is negative"), FrameIndex));
	}
	if (InCpuBytes < 0 || InGpuBytes < 0)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("frame %d declared negative byte counts (%lld CPU, %lld GPU)"),
				FrameIndex, InCpuBytes, InGpuBytes));
	}

	// A re-admit replaces the old reservation rather than adding to it. The
	// existing entry's bytes come out of the running totals first, so the fit
	// below is computed against the world after the replacement - not against a
	// world where this frame is charged twice.
	const int32 Existing = FindIndex(FrameIndex);
	int64 BaseCpu = CpuBytes;
	int64 BaseGpu = GpuBytes;
	if (Existing != INDEX_NONE)
	{
		BaseCpu -= Entries[Existing].CpuBytes;
		BaseGpu -= Entries[Existing].GpuBytes;
	}

	// RULE 2: PLAN, THEN COMMIT. Nothing below mutates the cache until the plan
	// is known to succeed. The plan walks a copy of the LRU order, so a failed
	// admission costs no resident frame.
	TArray<int32> Plan;
	int64 PlannedCpu = BaseCpu + InCpuBytes;
	int64 PlannedGpu = BaseGpu + InGpuBytes;

	if (!FlowVizFrameCacheDetail::FitsInBudget(PlannedCpu, CpuBudgetBytes)
		|| !FlowVizFrameCacheDetail::FitsInBudget(PlannedGpu, GpuBudgetBytes))
	{
		// Build the eviction order once: unpinned entries, excluding the frame
		// being admitted, oldest first.
		TArray<int32> Order;
		Order.Reserve(Entries.Num());
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			if (!Entries[Index].bPinned && Entries[Index].FrameIndex != FrameIndex)
			{
				Order.Add(Index);
			}
		}
		Order.Sort([this](int32 A, int32 B)
		{
			return Entries[A].LastUseSerial < Entries[B].LastUseSerial;
		});

		for (int32 OrderIndex = 0; OrderIndex < Order.Num(); ++OrderIndex)
		{
			if (FlowVizFrameCacheDetail::FitsInBudget(PlannedCpu, CpuBudgetBytes)
				&& FlowVizFrameCacheDetail::FitsInBudget(PlannedGpu, GpuBudgetBytes))
			{
				break;
			}

			const FFlowVizFrameCacheEntry& Victim = Entries[Order[OrderIndex]];
			PlannedCpu -= Victim.CpuBytes;
			PlannedGpu -= Victim.GpuBytes;
			Plan.Add(Victim.FrameIndex);
		}

		if (!FlowVizFrameCacheDetail::FitsInBudget(PlannedCpu, CpuBudgetBytes)
			|| !FlowVizFrameCacheDetail::FitsInBudget(PlannedGpu, GpuBudgetBytes))
		{
			// Even with every evictable frame gone it does not fit: either the
			// frame is larger than the whole budget, or the remainder is pinned as
			// A/B. AllocationTooLarge is the same signal
			// FFlowVizVolumeTextureSet::EnqueueUpload gives for a fully pinned slot
			// set - retry once the display frames advance, not an error to surface.
			++RefusalCount;
			return FCFDVizResult::Fail(
				ECFDVizError::AllocationTooLarge,
				FString::Printf(
					TEXT("frame %d needs %lld CPU / %lld GPU bytes and does not fit in the "
						 "%lld / %lld budget even after evicting every unpinned frame; retry once "
						 "the display frames advance"),
					FrameIndex, InCpuBytes, InGpuBytes, CpuBudgetBytes, GpuBudgetBytes));
		}
	}

	// The plan succeeds. Commit it.
	for (int32 Victim : Plan)
	{
		Remove(Victim);
		++EvictionCount;
	}
	OutEvicted.Append(Plan);

	++UseSerial;

	const int32 Slot = FindIndex(FrameIndex);
	if (Slot != INDEX_NONE)
	{
		FFlowVizFrameCacheEntry& Entry = Entries[Slot];
		CpuBytes -= Entry.CpuBytes;
		GpuBytes -= Entry.GpuBytes;
		Entry.CpuBytes = InCpuBytes;
		Entry.GpuBytes = InGpuBytes;
		Entry.LastUseSerial = UseSerial;
		// bComplete is NOT reset: a re-admit at the same size is a touch, and a
		// caller that is genuinely re-decoding calls Remove first. Resetting it
		// here would blank a displayed frame on a redundant admit.
		CpuBytes += InCpuBytes;
		GpuBytes += InGpuBytes;
	}
	else
	{
		FFlowVizFrameCacheEntry Entry;
		Entry.FrameIndex = FrameIndex;
		Entry.CpuBytes = InCpuBytes;
		Entry.GpuBytes = InGpuBytes;
		Entry.LastUseSerial = UseSerial;
		Entry.bPinned = false;
		Entry.bComplete = false;
		Entries.Add(Entry);

		CpuBytes += InCpuBytes;
		GpuBytes += InGpuBytes;
	}

	++AdmitCount;
	return FCFDVizResult::Ok();
}

bool FFlowVizFrameCache::Touch(int32 FrameIndex)
{
	const int32 Index = FindIndex(FrameIndex);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	++UseSerial;
	Entries[Index].LastUseSerial = UseSerial;
	return true;
}

void FFlowVizFrameCache::SetPinnedFrames(int32 FrameA, int32 FrameB)
{
	SetPinnedFrames(FrameA, FrameB, INDEX_NONE, INDEX_NONE);
}

void FFlowVizFrameCache::SetPinnedFrames(int32 FrameA, int32 FrameB, int32 FrameC, int32 FrameD)
{
	// Unconditionally rewrite every flag. Pinning is a statement about the
	// current pairs, so the previous ones must be released in the same call -
	// a version that only sets the new pins leaks a pin per seek and
	// eventually pins the whole cache.
	for (FFlowVizFrameCacheEntry& Entry : Entries)
	{
		Entry.bPinned = (Entry.FrameIndex == FrameA && FrameA != INDEX_NONE)
			|| (Entry.FrameIndex == FrameB && FrameB != INDEX_NONE)
			|| (Entry.FrameIndex == FrameC && FrameC != INDEX_NONE)
			|| (Entry.FrameIndex == FrameD && FrameD != INDEX_NONE);
	}
}

bool FFlowVizFrameCache::MarkComplete(int32 FrameIndex)
{
	const int32 Index = FindIndex(FrameIndex);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	Entries[Index].bComplete = true;
	return true;
}

bool FFlowVizFrameCache::Contains(int32 FrameIndex) const
{
	return FindIndex(FrameIndex) != INDEX_NONE;
}

bool FFlowVizFrameCache::IsResident(int32 FrameIndex) const
{
	const int32 Index = FindIndex(FrameIndex);
	return Index != INDEX_NONE && Entries[Index].bComplete;
}

bool FFlowVizFrameCache::IsPinned(int32 FrameIndex) const
{
	const int32 Index = FindIndex(FrameIndex);
	return Index != INDEX_NONE && Entries[Index].bPinned;
}

bool FFlowVizFrameCache::Remove(int32 FrameIndex)
{
	const int32 Index = FindIndex(FrameIndex);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	CpuBytes -= Entries[Index].CpuBytes;
	GpuBytes -= Entries[Index].GpuBytes;
	Entries.RemoveAt(Index);

	// The totals are a running sum, so a slipped decrement compounds silently
	// until the cache believes it holds memory it does not. Assert the invariant
	// here, where the only subtraction happens.
	check(CpuBytes >= 0);
	check(GpuBytes >= 0);

	return true;
}

void FFlowVizFrameCache::Reset()
{
	Entries.Reset();
	CpuBytes = 0;
	GpuBytes = 0;
}

const FFlowVizFrameCacheEntry* FFlowVizFrameCache::FindEntry(int32 FrameIndex) const
{
	const int32 Index = FindIndex(FrameIndex);
	return Index == INDEX_NONE ? nullptr : &Entries[Index];
}

FFlowVizFrameCacheStats FFlowVizFrameCache::GetStats() const
{
	FFlowVizFrameCacheStats Stats;
	Stats.EntryCount = Entries.Num();
	Stats.CpuBytes = CpuBytes;
	Stats.GpuBytes = GpuBytes;
	Stats.CpuBudgetBytes = CpuBudgetBytes;
	Stats.GpuBudgetBytes = GpuBudgetBytes;
	Stats.EvictionCount = EvictionCount;
	Stats.AdmitCount = AdmitCount;
	Stats.RefusalCount = RefusalCount;

	for (const FFlowVizFrameCacheEntry& Entry : Entries)
	{
		if (Entry.bComplete)
		{
			++Stats.CompleteCount;
		}
		if (Entry.bPinned)
		{
			++Stats.PinnedCount;
		}
	}

	return Stats;
}

int32 FFlowVizFrameCache::PeekEvictionCandidate() const
{
	int32 Best = INDEX_NONE;
	uint64 BestSerial = MAX_uint64;

	for (const FFlowVizFrameCacheEntry& Entry : Entries)
	{
		if (Entry.bPinned)
		{
			continue;
		}
		if (Entry.LastUseSerial < BestSerial)
		{
			BestSerial = Entry.LastUseSerial;
			Best = Entry.FrameIndex;
		}
	}

	return Best;
}
