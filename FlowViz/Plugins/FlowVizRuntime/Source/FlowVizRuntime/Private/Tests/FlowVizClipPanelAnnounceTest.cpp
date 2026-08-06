// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizClipPanel.h"

#include "Misc/AutomationTest.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "Widgets/Input/SButton.h"
// The advisory search downcasts to STextBlock to read its text, so the complete
// type is needed here rather than the forward declaration the panel header has.
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/*
 * NAMED namespace: unity build. An anonymous one here would merge with every
 * sibling test's rather than being file-local (#37).
 */
namespace FlowVizClipPanelAnnounceTest
{
	/** Three DIFFERENT extents, so an axis mix-up cannot pass by symmetry. */
	const FVector TestDomain(2.0, 4.0, 8.0);

	/**
	 * A counting subscriber.
	 *
	 * The panel's channel to the renderer is a FSimpleDelegate, so there is
	 * nothing to observe but the fact of a call - which is exactly the fact the
	 * mutants below delete.
	 */
	struct FAnnounceCounter
	{
		int32 Count = 0;

		FSimpleDelegate MakeDelegate()
		{
			return FSimpleDelegate::CreateLambda([this]() { ++Count; });
		}
	};
}

/**
 * EVERY EDIT THAT CHANGES THE IMAGE MUST BE ANNOUNCED, not just the one that
 * happens to be tested.
 *
 * WHERE THIS CAME FROM. Mutation campaign clip-wire-panel (2026-08-06, recorded
 * in Tools/mutants/clip-wire-panel.README) deleted the NotifyClipChanged() call
 * from the enable, invert and remove handlers one at a time. All three SURVIVED
 * a green suite. The arm next to them - the same deletion in the PRESET handler
 * - was killed.
 *
 * That pair is the whole diagnosis. A test did prove this panel can announce,
 * through exactly one control out of four, and that single green is what made
 * the other three look covered. The mechanism was verified once and every other
 * caller of it was assumed.
 *
 * WHY THE EXISTING TESTS CANNOT CATCH THIS, and why this file does not simply
 * extend them. FlowVizClipPanelTest.cpp presses these same three buttons and
 * asserts the VIEW MODEL changed - correctly, and that is its subject. But
 * deleting NotifyClipChanged() leaves the view model change completely intact:
 * the plane still hides, still flips, still disappears. Only the renderer never
 * hears. So a view-model assertion cannot fail on these mutants no matter how
 * thorough it is, and adding one would close the task without closing the gap.
 * Repo memory verify-metrics-can-fail.
 *
 * WHAT THIS FILE ASSERTS INSTEAD: the delegate fired. Nothing else distinguishes
 * a wired handler from an unwired one.
 *
 * A SECOND TRAP, WORTH STATING BECAUSE IT IS INVISIBLE. NotifyClipChanged uses
 * ExecuteIfBound, and an unsubscribed panel is a legal inert state. So pressing
 * a button on a panel with no .OnClipChanged(...) proves nothing at all - it
 * cannot crash and it cannot count. Every arm below subscribes first, and the
 * count is asserted to MOVE rather than merely to be non-zero.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipPanelAnnouncesEveryEditTest,
	"FlowViz.UI.ClipPanel.AnnouncesEveryEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizClipPanelAnnouncesEveryEditTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizClipPanelAnnounceTest;

	FFlowVizClipViewModel Clip;
	if (!TestTrue(TEXT("CONTROL: the domain is accepted, without which every edit below would "
					   "be refused and every announcement correctly absent"),
			Clip.SetDomainSize(TestDomain).IsOk()))
	{
		return false;
	}

	FAnnounceCounter Announced;

	const TSharedRef<SFlowVizClipPanel> Panel = SNew(SFlowVizClipPanel)
		.ViewModel(&Clip)
		.OnClipChanged(Announced.MakeDelegate());

	/* == The control arm: the channel exists at all =========================== */

	/*
	 * WITHOUT THIS, EVERY ASSERTION BELOW IS VACUOUS. A panel that dropped the
	 * OnClipChanged argument on the floor - the exact SLATE_EVENT defect this
	 * repo shipped once - would leave the counter at zero forever, and "the count
	 * did not move" is the failure mode all four arms below look for. They must
	 * be preceded by a demonstration that the count CAN move.
	 */
	{
		const TSharedPtr<SButton> Preset = Panel->GetPresetButton(EFlowVizClipPreset::KeepPlusX);
		if (!TestTrue(TEXT("CONTROL: the panel built a +X preset button"), Preset.IsValid()))
		{
			return false;
		}

		const int32 Before = Announced.Count;
		Preset->SimulateClick();

		if (!TestEqual(TEXT("CONTROL: adding a plane announces exactly once, so the subscription "
						    "is live and a count that fails to move below means the handler did "
						    "not announce - not that this test never subscribed"),
				Announced.Count, Before + 1))
		{
			return false;
		}
		if (!TestEqual(TEXT("CONTROL: and the plane really was added, so the rows below exist"),
				Clip.GetPlaneCount(), 1))
		{
			return false;
		}
	}

	/* == Enable: hiding a plane changes the image ============================= */

	{
		const TSharedPtr<SButton> Toggle = Panel->GetPlaneEnableButton(0);
		if (!TestTrue(TEXT("CONTROL: the panel built an enable toggle for plane 0"),
				Toggle.IsValid()))
		{
			return false;
		}

		const int32 Before = Announced.Count;
		Toggle->SimulateClick();

		/*
		 * THE PLANE COUNT IS UNCHANGED HERE, which is the whole reason this arm
		 * exists. A channel that announced only when the COUNT moved would pass
		 * every add and remove test in the suite and leave a hidden plane clipping
		 * the render forever. The shader is given the ENABLED planes.
		 */
		TestEqual(TEXT("CONTROL: hiding retains the plane, so this is the count-unchanged case"),
			Clip.GetPlaneCount(), 1);

		TestEqual(TEXT("hiding a plane announces, so the renderer stops receiving it - the shader "
					   "takes the ENABLED planes, so an unannounced hide keeps clipping"),
			Announced.Count, Before + 1);

		// Restore, so the arms below start from a plane that is actually enabled.
		Toggle->SimulateClick();
	}

	/* == Invert: the plane flips, and the image with it ======================= */

	{
		const TSharedPtr<SButton> Invert = Panel->GetPlaneInvertButton(0);
		if (!TestTrue(TEXT("CONTROL: the panel built an invert button for plane 0"),
				Invert.IsValid()))
		{
			return false;
		}

		const int32 Before = Announced.Count;
		Invert->SimulateClick();

		TestEqual(TEXT("inverting announces, so the side that is kept changes in the image and "
					   "not only in the panel"),
			Announced.Count, Before + 1);
	}

	/* == Remove: a deleted plane must stop clipping =========================== */

	{
		const TSharedPtr<SButton> Remove = Panel->GetPlaneRemoveButton(0);
		if (!TestTrue(TEXT("CONTROL: the panel built a remove button for plane 0"),
				Remove.IsValid()))
		{
			return false;
		}

		const int32 Before = Announced.Count;
		Remove->SimulateClick();

		TestEqual(TEXT("CONTROL: the plane really was removed"), Clip.GetPlaneCount(), 0);

		TestEqual(TEXT("removing announces, so a deleted plane stops clipping rather than "
					   "clipping forever with the panel showing none"),
			Announced.Count, Before + 1);
	}

	/* == And a REFUSED edit announces nothing ================================ */

	/*
	 * THE OTHER DIRECTION, and it is a real requirement rather than a symmetry
	 * for its own sake. Announcing an edit that was refused pushes an identical
	 * clip model to the render thread - a flush for a frame that cannot have
	 * changed. The handlers gate on .IsOk() precisely to avoid it.
	 *
	 * Driven to the shader's limit through the BUTTON, so the refusal is reached
	 * by the path a user takes rather than by calling the view model directly.
	 */
	{
		const TSharedPtr<SButton> Preset = Panel->GetPresetButton(EFlowVizClipPreset::KeepPlusX);
		if (!TestTrue(TEXT("CONTROL: the +X preset button exists"), Preset.IsValid()))
		{
			return false;
		}

		for (int32 Index = 0; Index < FlowVizRayMarch::MaxClipPlanes; ++Index)
		{
			Preset->SimulateClick();
		}

		if (!TestEqual(TEXT("CONTROL: the panel filled to the shader's plane limit"),
				Clip.GetPlaneCount(), FlowVizRayMarch::MaxClipPlanes))
		{
			return false;
		}
		if (!TestFalse(TEXT("CONTROL: the view model would now REFUSE another plane, without "
						    "which the assertion below could not fail"),
				Clip.CanAddPlane()))
		{
			return false;
		}

		/*
		 * SimulateClick BYPASSES THE ENABLED CHECK, deliberately. The button is
		 * disabled at the limit (rule 15, asserted in FlowVizClipPanelTest.cpp),
		 * so a test that respected enablement could never reach the handler's
		 * refusal branch - the guard would be unreachable and the assertion
		 * vacuous. Repo memory unreachable-code-cannot-be-differentially-tested.
		 */
		const int32 Before = Announced.Count;
		Preset->SimulateClick();

		TestEqual(TEXT("CONTROL: the refused press really added nothing"),
			Clip.GetPlaneCount(), FlowVizRayMarch::MaxClipPlanes);

		TestEqual(TEXT("a REFUSED edit announces nothing, so the render thread is not flushed "
					   "for a frame that cannot have changed"),
			Announced.Count, Before);
	}

	return true;
}

/* ========================================================================== */
/* The advisory the layout actually reads                                      */
/* ========================================================================== */

/**
 * IS THE ADVISORY EVER SHOWN, as opposed to correctly decided?
 *
 * WHERE THIS CAME FROM, and it is the more interesting half of the same
 * campaign. Two arms concern the same advisory:
 *
 *     killed    the advisory latches -- once retired it never returns
 *     SURVIVED  the advisory is never shown -- an inert control discloses nothing
 *
 * The second replaces the body of GetNotWiredAdvisoryVisibility with a bare
 * `return EVisibility::Collapsed`. It survived a green suite. The pair localises
 * the gap exactly: six assertions across FlowVizWorkspaceVolumeSeamTest.cpp call
 * IsNotWiredAdvisoryVisible() - an inline PREDICATE in the header - and not one
 * reads the visibility the strip is actually bound to. The suite checks the
 * value a user never sees and skips the one the layout consumes.
 *
 * That is #54's own defect one layer down. #54 made the advisory RETIRE
 * correctly when a volume binds; nothing checked it is ever DISPLAYED. A panel
 * that shows it never - the state where an inert control discloses nothing,
 * which is the entire reason the strip was written - passes today.
 *
 * WHY THIS READS THE WIDGET RATHER THAN THE ACCESSOR. GetNotWiredAdvisoryVisibility
 * is private, and making it public to test it would be testing the same
 * indirection one step further out. What a user sees is the BORDER's visibility,
 * after the frame loop has evaluated its bound attribute. So this walks to the
 * widget and asks it, exactly as the layout does.
 *
 * PUMPING IS NOT OPTIONAL HERE and the direction of the trap matters: an
 * unpumped widget returns its constructor's cached default, which for visibility
 * is Visible. So "TestEqual(..., Visible)" would pass on a widget with NO
 * binding at all. The assertion that carries the weight is the COLLAPSED one,
 * and the pair is what makes either mean anything. See
 * Tests/FlowVizSlateAttributePump.h.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipPanelAdvisoryIsShownTest,
	"FlowViz.UI.ClipPanel.AdvisoryIsShown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizClipPanelAdvisoryIsShownTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizClipPanelAnnounceTest;

	FFlowVizClipViewModel Clip;
	const TSharedRef<SFlowVizClipPanel> Panel = SNew(SFlowVizClipPanel).ViewModel(&Clip);

	/*
	 * FIND THE STRIP BY ITS TEXT, because the panel does not expose it and should
	 * not have to. What is being verified is that SOMETHING in the built tree
	 * carries the advisory and follows the binding; a widget the panel kept a
	 * handle to would be one more thing that can be correct while unparented.
	 *
	 * The text is the advisory's own accessor, so this cannot drift from it.
	 */
	const FText AdvisoryText = Panel->GetNotWiredAdvisoryText();
	if (!TestFalse(TEXT("CONTROL: the advisory has text to search for"), AdvisoryText.IsEmpty()))
	{
		return false;
	}

	/*
	 * STextBlock::GetText, NOT SWidget::GetAccessibleText. The accessible-text
	 * accessor is compiled out unless WITH_ACCESSIBILITY, which is off in most
	 * configurations - a test built on it would simply fail to compile here, and
	 * a test that compiled it out conditionally would silently verify nothing in
	 * the configuration that actually runs.
	 */
	TFunction<TSharedPtr<SWidget>(const TSharedRef<SWidget>&)> FindAdvisoryStrip;
	FindAdvisoryStrip = [&AdvisoryText, &FindAdvisoryStrip](
							const TSharedRef<SWidget>& Root) -> TSharedPtr<SWidget>
	{
		TSharedPtr<SWidget> Found;

		if (FChildren* Children = Root->GetAllChildren())
		{
			Children->ForEachWidget(
				[&](SWidget& Child)
				{
					if (Found.IsValid())
					{
						return;
					}

					// The TEXT BLOCK carries the string; the strip whose visibility
					// is bound is the ancestor it hangs from. Matching the text and
					// then taking that ancestor keeps this anchored to the real
					// widget rather than to a type name that matches three borders.
					if (Child.GetType() == TEXT("STextBlock")
						&& static_cast<STextBlock&>(Child).GetText().EqualTo(AdvisoryText))
					{
						Found = Root;
						return;
					}

					Found = FindAdvisoryStrip(Child.AsShared());
				});
		}

		return Found;
	};

	// Pump before searching: the text block's own Text is a bound attribute too.
	FlowVizSlateAttributePump::Pump(Panel);

	const TSharedPtr<SWidget> Strip = FindAdvisoryStrip(Panel);
	if (!TestValid(TEXT("CONTROL: the built panel actually contains a widget carrying the "
						"advisory text -- a strip that is never parented cannot be seen no "
						"matter what its visibility says"),
			Strip))
	{
		return false;
	}

	/* == Unbound: the claim is true, so the strip must be visible ============= */

	FlowVizSlateAttributePump::Pump(Panel);

	TestEqual(TEXT("with no volume bound the advisory strip is VISIBLE in the built tree, not "
				   "merely decided to be -- an inert control that discloses nothing is the "
				   "exact failure this strip was written to prevent"),
		Strip->GetVisibility(), EVisibility::Visible);

	/* == Bound: the claim is false, so the strip must go ====================== */

	Panel->SetVolumeBound(true);
	FlowVizSlateAttributePump::Pump(Panel);

	/*
	 * COLLAPSED, NOT MERELY "not Visible". Hidden would also stop the text being
	 * read and would leave a strip-sized hole in the layout - the code says so
	 * and this is where that statement is checked. Asserting the exact enumerator
	 * is also what makes this arm able to fail on a mutant that returns Hidden.
	 */
	TestEqual(TEXT("binding a volume COLLAPSES the strip rather than hiding it, so the panel "
				   "does not keep a strip-sized gap where the retired advisory was"),
		Strip->GetVisibility(), EVisibility::Collapsed);

	/* == And back, so it tracks rather than latching ========================== */

	Panel->SetVolumeBound(false);
	FlowVizSlateAttributePump::Pump(Panel);

	TestEqual(TEXT("unbinding restores the strip, so its visibility tracks the binding rather "
				   "than latching the first time it is cleared"),
		Strip->GetVisibility(), EVisibility::Visible);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
