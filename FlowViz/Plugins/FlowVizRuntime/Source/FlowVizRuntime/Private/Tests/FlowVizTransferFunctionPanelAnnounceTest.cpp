// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizTransferFunctionPanel.h"

#include "Misc/AutomationTest.h"
#include "UI/FlowVizTransferFunctionViewModel.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build. An anonymous one here would merge with every
 * sibling test's rather than being file-local (#37).
 */
namespace FlowVizTransferFunctionPanelAnnounceTest
{
	/**
	 * A counting subscriber.
	 *
	 * The panel's channel to the renderer is a FSimpleDelegate, so there is
	 * nothing to observe but the fact of a call -- which is exactly the fact
	 * that was missing from all seven handlers.
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
 * EVERY EDIT IN "COLOR & OPACITY" MUST REACH THE RENDERER.
 *
 * WHERE THIS CAME FROM. Not from a mutation campaign -- from mapping the chain
 * for #48. SFlowVizClipPanel has carried SLATE_EVENT(FSimpleDelegate,
 * OnClipChanged) since it was written, and fires NotifyClipChanged() from every
 * mutating handler. SFlowVizTransferFunctionPanel had NO delegate of any kind:
 * grepping its header for Delegate, SLATE_EVENT, Notify or Broadcast returned
 * nothing at all.
 *
 * So the failure here is not a caller that was forgotten among several. It is
 * the whole channel: every colormap button, every range control and the opacity
 * slider edited a view model that nothing downstream read. Seven handlers, zero
 * announcements.
 *
 * WHY NOBODY NOTICED, AND WHY THE EXISTING PANEL TEST CANNOT SEE IT.
 * FlowVizTransferFunctionPanelTest.cpp presses these same buttons and asserts
 * the VIEW MODEL changed -- correctly; that is its subject. But the view model
 * changes identically whether or not anything is announced, so those assertions
 * cannot distinguish a wired panel from an inert one. The ramp strip makes it
 * worse: it PAINTS from the same view model the buttons write, so the panel
 * visibly responds to every click. The colours on the strip move; the colours
 * in the volume do not.
 *
 * THE REFUSAL DIRECTION IS ASSERTED TOO. Five of the six setters return
 * FCFDVizResult and can refuse. Announcing a refused edit would push a
 * byte-identical transfer function to the render thread and rebuild the LUT for
 * a frame that cannot have changed. SetReverseColorMap returns void and cannot
 * refuse, so its handler announces unconditionally -- that asymmetry is
 * deliberate, and both halves are asserted below so a future edit that
 * "normalises" the handlers has to argue with a test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlowVizTransferFunctionPanelAnnounceTest,
	"FlowViz.UI.TransferFunctionPanel.Announce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlowVizTransferFunctionPanelAnnounceTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizTransferFunctionPanelAnnounceTest;

	FFlowVizTransferFunctionViewModel Model;
	FAnnounceCounter Announced;

	const TSharedRef<SFlowVizTransferFunctionPanel> Panel =
		SNew(SFlowVizTransferFunctionPanel)
			.ViewModel(&Model)
			.OnTransferFunctionChanged(Announced.MakeDelegate());

	/*
	 * THE IDENTITY CONTROL. Constructing a panel must announce NOTHING. A
	 * Construct that fired the delegate once -- to "push the initial state" --
	 * would make every +1 assertion below pass on an off-by-one, and would push
	 * a default transfer function over whatever a session load had just
	 * restored.
	 */
	if (!TestEqual(TEXT("CONTROL: constructing the panel announces nothing"),
			Announced.Count, 0))
	{
		return false;
	}

	/* == Colormap: the single most visible control in the workspace ========== */

	{
		// INFERNO, not the default. Selecting the map that is already active
		// would leave the view model unchanged, and then an announcement and a
		// no-op would be indistinguishable in the render.
		const TSharedPtr<SButton> Map = Panel->GetColorMapButton(ECFDVizColorMap::Inferno);
		if (!TestTrue(TEXT("CONTROL: the panel built a button for Inferno"), Map.IsValid()))
		{
			return false;
		}

		const int32 Before = Announced.Count;
		Map->SimulateClick();

		TestEqual(TEXT("CONTROL: the click really did change the selected map, so the "
					   "assertion below is about announcing and not about the click"),
			static_cast<int32>(Model.GetColorMap()), static_cast<int32>(ECFDVizColorMap::Inferno));

		TestEqual(TEXT("choosing a colormap announces, so the LUT is rebuilt and the volume "
					   "stops rendering through viridis while the picker says Inferno"),
			Announced.Count, Before + 1);
	}

	/* == Reverse: the one handler that announces unconditionally ============= */

	{
		const TSharedPtr<SButton> Reverse = Panel->GetReverseButton();
		if (!TestTrue(TEXT("CONTROL: the panel built a reverse button"), Reverse.IsValid()))
		{
			return false;
		}

		const int32 Before = Announced.Count;
		const bool bWasReversed = Model.IsColorMapReversed();
		Reverse->SimulateClick();

		TestNotEqual(TEXT("CONTROL: the click really did flip the reverse flag"),
			Model.IsColorMapReversed(), bWasReversed);

		TestEqual(TEXT("reversing announces, so high and low swap ends in the volume and not "
					   "only on the ramp strip"),
			Announced.Count, Before + 1);
	}

	/* == Opacity: the control that changes density, not colour ============== */

	{
		const TSharedPtr<SFlowVizOpacitySlider> Slider = Panel->GetOpacitySlider();
		if (!TestTrue(TEXT("CONTROL: the panel built an opacity slider"), Slider.IsValid()))
		{
			return false;
		}

		const int32 Before = Announced.Count;

		// 0.375, deliberately not 1.0, for TWO reasons. The multiplier defaults
		// to 1, so asking for 1 would be satisfied by a panel that announced
		// nothing and a renderer that never read the value -- and SSlider's
		// CommitValue is a no-op when the value is unchanged, so it would not
		// even reach the handler.
		Slider->SimulateDrag(0.375f);

		TestEqual(TEXT("CONTROL: the drag really did move the multiplier"),
			Model.GetOpacityMultiplier(), 0.375f);

		TestEqual(TEXT("moving the opacity slider announces, so the volume actually thins "
					   "rather than the number alone changing"),
			Announced.Count, Before + 1);
	}

	/* == Manual range: committed through the box a user types into ========== */

	{
		const TSharedPtr<SFlowVizNumericEntry> MaxBox = Panel->GetRangeMaxBox();
		if (!TestTrue(TEXT("CONTROL: the panel built a range-max box"), MaxBox.IsValid()))
		{
			return false;
		}

		const int32 Before = Announced.Count;
		MaxBox->SimulateCommit(FText::FromString(TEXT("11.75")));

		TestEqual(TEXT("CONTROL: the commit really did move the range maximum"),
			Model.GetRangeMax(), 11.75f);

		TestEqual(TEXT("committing a range bound announces, so the colours rescale in the "
					   "volume and the readout does not disagree with the picture"),
			Announced.Count, Before + 1);
	}

	/* == And a REFUSED edit announces nothing =============================== */

	/*
	 * THE OTHER DIRECTION, reached the way a user reaches it: type a minimum
	 * ABOVE the maximum. SetManualRange refuses max <= min outright -- an
	 * inverted domain would silently reverse the colormap, which is a different
	 * control the user did not touch.
	 *
	 * Without this arm, a handler that announced unconditionally would pass
	 * every assertion above. That handler would rebuild the LUT on each
	 * rejected keystroke, so the assertion is about work as much as correctness.
	 */
	{
		const TSharedPtr<SFlowVizNumericEntry> MinBox = Panel->GetRangeMinBox();
		if (!TestTrue(TEXT("CONTROL: the panel built a range-min box"), MinBox.IsValid()))
		{
			return false;
		}

		const float MaxBefore = Model.GetRangeMax();
		const float MinBefore = Model.GetRangeMin();
		const int32 Before = Announced.Count;

		// Above the 11.75 committed just above, so the view model must refuse.
		MinBox->SimulateCommit(FText::FromString(TEXT("99.5")));

		if (!TestEqual(TEXT("CONTROL: the refusal really was refused -- the minimum did not "
						    "move, without which the assertion below could not fail"),
				Model.GetRangeMin(), MinBefore))
		{
			return false;
		}
		TestEqual(TEXT("CONTROL: and the maximum is untouched too"),
			Model.GetRangeMax(), MaxBefore);

		TestEqual(TEXT("a REFUSED range edit announces nothing, so a rejected keystroke does "
					   "not rebuild the LUT for a frame that cannot have changed"),
			Announced.Count, Before);
	}

	/* == An unsubscribed panel is legal and inert =========================== */

	/*
	 * The panel is constructed before the workspace has a volume to push into,
	 * so an unbound delegate has to be a no-op rather than a crash. Asserted
	 * because ExecuteIfBound is what makes it true, and a future edit to
	 * Execute() would compile, pass every arm above, and crash the editor the
	 * first time anyone opened the workspace standalone.
	 */
	{
		FFlowVizTransferFunctionViewModel LoneModel;
		const TSharedRef<SFlowVizTransferFunctionPanel> LonePanel =
			SNew(SFlowVizTransferFunctionPanel).ViewModel(&LoneModel);

		const TSharedPtr<SButton> Map = LonePanel->GetColorMapButton(ECFDVizColorMap::Magma);
		if (!TestTrue(TEXT("CONTROL: the unsubscribed panel built its buttons"), Map.IsValid()))
		{
			return false;
		}

		Map->SimulateClick();

		TestEqual(TEXT("an unsubscribed panel still edits its model, and announcing to nobody "
					   "is inert rather than fatal"),
			static_cast<int32>(LoneModel.GetColorMap()), static_cast<int32>(ECFDVizColorMap::Magma));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
