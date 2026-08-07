// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceRegistry.h"

#include "UI/SFlowVizWorkspace.h"

namespace FlowVizWorkspaceRegistry
{
	namespace
	{
		/**
		 * Newest LAST, which is what makes GetActiveWorkspace a reverse scan.
		 *
		 * Function-local rather than a namespace-scope global so its construction
		 * happens on first use. This module loads at PostConfigInit, and a
		 * namespace-scope TArray would be constructed during static
		 * initialisation in an order no one controls.
		 */
		TArray<TWeakPtr<SFlowVizWorkspace>>& GetEntries()
		{
			static TArray<TWeakPtr<SFlowVizWorkspace>> Entries;
			return Entries;
		}

		/** Drop entries whose widget has been destroyed. Called on every read. */
		void Compact()
		{
			GetEntries().RemoveAll(
				[](const TWeakPtr<SFlowVizWorkspace>& Entry) { return !Entry.IsValid(); });
		}
	}

	void Register(SFlowVizWorkspace& Workspace)
	{
		/*
		 * AsShared() REQUIRES A LIVE SHARED REFERENCE, which is why this is called
		 * from Construct and not from the constructor.
		 *
		 * SCompoundWidget derives from TSharedFromThis, but the weak referencer it
		 * carries is only populated once the widget is owned by a TSharedRef --
		 * and during the C++ constructor body it is not. SNew constructs, then
		 * wraps, then calls Construct, so Construct is the earliest point where
		 * AsShared() is legal. Calling it a frame earlier is a check() failure,
		 * not a silent null.
		 *
		 * DoesSharedInstanceExist is asked anyway rather than assumed: a
		 * hypothetical caller that invokes Construct on a stack-allocated widget
		 * would otherwise take down the editor from inside a registry.
		 */
		if (!Workspace.DoesSharedInstanceExist())
		{
			return;
		}

		Compact();

		TWeakPtr<SFlowVizWorkspace> Weak = StaticCastSharedRef<SFlowVizWorkspace>(Workspace.AsShared());

		// IDEMPOTENT. Construct is called once per widget by SNew, but a caller
		// that invoked it twice would otherwise get two entries for one workspace
		// and a "how many are open" answer that is wrong by one.
		for (const TWeakPtr<SFlowVizWorkspace>& Entry : GetEntries())
		{
			if (Entry.HasSameObject(&Workspace))
			{
				return;
			}
		}

		GetEntries().Add(MoveTemp(Weak));
	}

	void Unregister(SFlowVizWorkspace& Workspace)
	{
		/*
		 * BY ADDRESS, NOT BY AsShared().
		 *
		 * This runs from ~SFlowVizWorkspace, at which point the shared reference
		 * controller has already released the object -- AsShared() would fail its
		 * check and the weak entry has already gone null, so matching on the
		 * pointer is the only identification left. HasSameObject compares the
		 * stored address without resurrecting anything, which is exactly the
		 * operation this needs.
		 */
		GetEntries().RemoveAll(
			[&Workspace](const TWeakPtr<SFlowVizWorkspace>& Entry)
			{ return !Entry.IsValid() || Entry.HasSameObject(&Workspace); });
	}

	TArray<TSharedPtr<SFlowVizWorkspace>> GetLiveWorkspaces()
	{
		Compact();

		TArray<TSharedPtr<SFlowVizWorkspace>> Live;
		Live.Reserve(GetEntries().Num());
		for (const TWeakPtr<SFlowVizWorkspace>& Entry : GetEntries())
		{
			if (TSharedPtr<SFlowVizWorkspace> Pinned = Entry.Pin())
			{
				Live.Add(MoveTemp(Pinned));
			}
		}
		return Live;
	}

	TSharedPtr<SFlowVizWorkspace> GetActiveWorkspace()
	{
		Compact();

		// REVERSE, because the newest workspace is appended last and the header
		// explains why the newest is the one a typed command means.
		const TArray<TWeakPtr<SFlowVizWorkspace>>& Entries = GetEntries();
		for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
		{
			if (TSharedPtr<SFlowVizWorkspace> Pinned = Entries[Index].Pin())
			{
				return Pinned;
			}
		}
		return nullptr;
	}
}
