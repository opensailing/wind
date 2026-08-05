// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Layout/ChildrenBase.h"
#include "Widgets/SWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Evaluate every bound Slate attribute in a widget tree, the way a live frame
 * does, so a headless test can read what a user would see.
 *
 * WHY THIS IS NEEDED, AND WHY IT IS NOT A FUDGE.
 *
 * `SWidget::IsEnabled()` (SWidget.h:998) does not call your predicate. It
 * returns `EnabledStateAttribute.Get()`, and `TSlateAttribute::Get()`
 * (SlateAttributeBase.inl:240) returns a CACHED value. Binding does not fill
 * that cache: `UE_SLATE_WITH_ATTRIBUTE_INITIALIZATION_ON_BIND` is 0
 * (SlateAttribute.h:17), so the default bind action is
 * `ESlateAttributeBindAction::None` and `AssignBinding`
 * (SlateAttributeBase.inl:532) skips `ProtectedUpdateNow`. The cache therefore
 * still holds the value from the widget's constructor - for enablement, the
 * literal `true` at SWidget.cpp:248.
 *
 * In a running application that is invisible, because something pumps the
 * attributes before anything is drawn: `SWidget::SlatePrepass` calls
 * `FSlateAttributeMetaData::UpdateAllAttributes` (SWidget.cpp:710), and
 * thereafter `FSlateInvalidationRoot::ProcessInvalidation` does it every frame
 * (SlateInvalidationRoot.cpp:1113-1126). A headless automation test runs
 * neither, so it reads the constructor's default and calls it the widget's
 * state.
 *
 * THAT IS A TRAP THAT POINTS THE WRONG WAY. The stale value is `true`, so an
 * assertion of the form `TestTrue(Button->IsEnabled())` PASSES on a widget with
 * no `.IsEnabled()` binding at all, on a widget bound to a predicate that
 * returns false, and on a widget whose binding was deleted - it cannot fail,
 * which by the project's own standard makes it not a check. The mirror
 * assertion, `TestFalse(...)`, fails on correct code. Both readings are wrong
 * and only one of them is loud.
 *
 * So this pumps the tree exactly as the frame loop does, using the engine's own
 * entry point rather than reaching into the attribute system, and it walks
 * `GetAllChildren` because that is the set the invalidation system walks - the
 * controls under test are grandchildren of the panel, not direct children.
 *
 * WHAT IT DOES NOT DO. It does not make a weak assertion strong. After pumping,
 * `TestTrue(IsEnabled())` still passes on an unbound widget, because `true` is
 * also the correct answer for one. The assertions that carry weight are the
 * ones that require a specific FALSE, and the pairs that require a widget to
 * change from one to the other when only the view model was touched.
 */
namespace FlowVizSlateAttributePump
{
	/** Evaluate every bound attribute on this widget and, recursively, its children. */
	inline void Pump(SWidget& Root)
	{
		Root.UpdateAllAttributes();

		if (FChildren* Children = Root.GetAllChildren())
		{
			Children->ForEachWidget([](SWidget& Child) { Pump(Child); });
		}
	}

	/**
	 * TEMPLATES, NOT `TSharedRef<SWidget>`/`TSharedPtr<SWidget>` OVERLOADS.
	 *
	 * A `TSharedRef<SFlowVizTransportBar>` converts to BOTH of those by equally
	 * good user-defined conversions, so a pair of concrete overloads is ambiguous
	 * at every call site that passes a derived widget - which is every call site
	 * here. Templated on the pointee, each call is an exact match and the
	 * ambiguity disappears.
	 */
	template<typename WidgetType>
	inline void Pump(const TSharedRef<WidgetType>& Root)
	{
		Pump(static_cast<SWidget&>(Root.Get()));
	}

	template<typename WidgetType>
	inline void Pump(const TSharedPtr<WidgetType>& Root)
	{
		if (Root.IsValid())
		{
			Pump(static_cast<SWidget&>(*Root));
		}
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
