// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizSlicePanel.h"

#include "Misc/AutomationTest.h"
#include "UI/FlowVizSliceViewModel.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/**
 * The slice panel's controls must drive FFlowVizSliceViewModel.
 *
 * Every state change below is produced by pressing a real button, dragging a
 * real slider or committing a real text box - never by calling a view model
 * setter to make the next assertion true. A control wired to nothing fails at
 * the first assertion in its block.
 *
 * DIFFERENTIAL PROPERTY. Remove `.OnValueChanged(...)` from the position slider
 * in SFlowVizSlicePanel::Construct and "dragging the position slider moves the
 * slice" goes red. Remove the IsEnabled binding on the slab controls and the
 * rule 15 pair goes red on its FALSE assertion.
 */

// NAMED namespace: unity build. An anonymous namespace here would merge with
// every sibling test's and collide by ODR.
namespace FlowVizSlicePanelTest
{
	/** Three DIFFERENT extents, so an axis mix-up cannot pass by coincidence. */
	const FVector TestDomain(2.0, 4.0, 8.0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSlicePanelBindingTest,
	"FlowViz.UI.SlicePanel.ControlsDriveTheViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSlicePanelBindingTest::RunTest(const FString& Parameters)
{
	FFlowVizSliceViewModel Slice;
	if (!TestTrue(TEXT("the domain is accepted"),
			Slice.SetDomainSize(FlowVizSlicePanelTest::TestDomain).IsOk()))
	{
		return false;
	}
	Slice.CenterOnDomain();

	const TSharedRef<SFlowVizSlicePanel> Panel = SNew(SFlowVizSlicePanel).ViewModel(&Slice);

	/* == Axis preset buttons set the normal ================================== */
	{
		// Start on a DIFFERENT axis, so "it was already X" cannot make the
		// assertion pass without the click doing anything.
		Slice.SetAxisPreset(EFlowVizSliceAxis::Z);
		TestTrue(TEXT("precondition: the normal is Z"),
			Slice.GetNormal().Equals(FVector(0.0, 0.0, 1.0), 1e-6));

		const TSharedPtr<SButton> AxisX = Panel->GetAxisButton(EFlowVizSliceAxis::X);
		if (!TestTrue(TEXT("the panel built an X axis button"), AxisX.IsValid()))
		{
			return false;
		}

		AxisX->SimulateClick();

		TestTrue(
			*FString::Printf(TEXT("pressing the X preset sets the normal to X, not (%g,%g,%g); if "
								  "this fails the button is not wired to the slice view model"),
				Slice.GetNormal().X, Slice.GetNormal().Y, Slice.GetNormal().Z),
			Slice.GetNormal().Equals(FVector(1.0, 0.0, 0.0), 1e-6));

		// A SECOND, DIFFERENT AXIS. This distinguishes "the buttons are wired"
		// from "one button is wired three times".
		const TSharedPtr<SButton> AxisY = Panel->GetAxisButton(EFlowVizSliceAxis::Y);
		if (!TestTrue(TEXT("the panel built a Y axis button"), AxisY.IsValid()))
		{
			return false;
		}
		AxisY->SimulateClick();
		TestTrue(TEXT("pressing the Y preset sets the normal to Y"),
			Slice.GetNormal().Equals(FVector(0.0, 1.0, 0.0), 1e-6));
	}

	/* == Dragging the position slider moves the slice ======================== */
	{
		Slice.SetAxisPreset(EFlowVizSliceAxis::Z);
		Slice.CenterOnDomain();

		const TSharedPtr<SFlowVizScrubSlider> Position = Panel->GetPositionSlider();
		if (!TestTrue(TEXT("the panel built a position slider"), Position.IsValid()))
		{
			return false;
		}

		const double Before = Slice.GetNormalizedPosition();

		// A VALUE DIFFERENT FROM THE CURRENT ONE, and asserted so. SSlider's
		// CommitValue is `if (NewValue != OldValue)`, so committing the value the
		// slider already holds is a silent no-op and the delegate never fires -
		// which a test would misread as an unwired slider.
		constexpr float Target = 0.25f;
		if (!TestTrue(
				*FString::Printf(
					TEXT("the target %g differs from the current position %g, without which "
						 "CommitValue is a no-op and this check could not fail"),
					Target, Before),
				FMath::Abs(Before - static_cast<double>(Target)) > 1e-3))
		{
			return false;
		}

		// SimulateDrag, not SetValue: SetValue only assigns the value attribute
		// and never fires OnValueChanged, so a test built on it could not observe
		// the handler at all.
		Position->SimulateDrag(Target);

		TestTrue(
			*FString::Printf(TEXT("dragging the position slider moves the slice (%g -> %g, "
								  "wanted %g)"),
				Before, Slice.GetNormalizedPosition(), Target),
			FMath::IsNearlyEqual(Slice.GetNormalizedPosition(), static_cast<double>(Target), 1e-3));
	}

	/* == Thickness commits, and the slab controls follow it (rule 15) ======== */
	{
		Slice.SetThickness(0.0);
		TestFalse(TEXT("precondition: a zero-thickness slice is not a slab"), Slice.IsSlab());

		const TSharedPtr<SButton> AverageButton = Panel->GetSlabOpButton(EFlowVizSlabOp::Average);
		if (!TestTrue(TEXT("the panel built a slab-average button"), AverageButton.IsValid()))
		{
			return false;
		}

		// EVALUATE THE BOUND ATTRIBUTES, as a drawn frame would. Without this the
		// assertion below reads the constructor's cached `true` (SWidget.cpp:248)
		// rather than the predicate. See Tests/FlowVizSlateAttributePump.h.
		FlowVizSlateAttributePump::Pump(Panel);

		// The view model REFUSES an aggregation on a zero-thickness slice, so the
		// button must be dead. A live one is the nonfunctional control rule 15
		// forbids: pressed, refused, and nothing happens.
		TestFalse(
			TEXT("with no thickness the slab aggregation buttons are disabled (rule 15: "
				 "disabled and refused must be one fact)"),
			AverageButton->IsEnabled());

		const TSharedPtr<SFlowVizNumericEntry> ThicknessBox = Panel->GetThicknessBox();
		if (!TestTrue(TEXT("the panel built a thickness box"), ThicknessBox.IsValid()))
		{
			return false;
		}

		// SimulateCommit, not SetText: SetText never fires OnTextCommitted.
		ThicknessBox->SimulateCommit(FText::FromString(TEXT("0.5")));

		TestTrue(
			*FString::Printf(TEXT("committing a thickness reaches the view model (%g)"),
				Slice.GetThickness()),
			FMath::IsNearlyEqual(Slice.GetThickness(), 0.5, 1e-6));
		TestTrue(TEXT("a non-zero thickness makes it a slab"), Slice.IsSlab());

		FlowVizSlateAttributePump::Pump(Panel);

		// THE PAIR IS WHAT MAKES THIS A CHECK. The FALSE above and the TRUE here
		// differ only in what the view model says, so a button hard-wired to
		// either state fails one of them.
		TestTrue(TEXT("once there is thickness the slab aggregation buttons come alive"),
			AverageButton->IsEnabled());

		// And pressing one selects it, through the production delegate.
		const TSharedPtr<SButton> MaximumButton = Panel->GetSlabOpButton(EFlowVizSlabOp::Maximum);
		if (TestTrue(TEXT("the panel built a slab-maximum button"), MaximumButton.IsValid()))
		{
			MaximumButton->SimulateClick();
			TestTrue(TEXT("pressing the maximum button selects that aggregation"),
				Slice.GetSlabOp() == EFlowVizSlabOp::Maximum);
		}
	}

	/* == Visibility and interpolation toggles ================================ */
	{
		Slice.SetVisible(true);
		const TSharedPtr<SButton> Visible = Panel->GetVisibleButton();
		if (!TestTrue(TEXT("the panel built a visibility toggle"), Visible.IsValid()))
		{
			return false;
		}
		Visible->SimulateClick();
		TestFalse(TEXT("pressing the visibility toggle hides the slice"), Slice.IsVisible());
		Visible->SimulateClick();
		TestTrue(TEXT("pressing it again shows it, so it is a toggle"), Slice.IsVisible());

		// RULE 7'S CONTROL. Trilinear sampling produces values the solver never
		// stored, so the panel must both offer it and disclose it.
		Slice.SetTrilinear(false);
		const TSharedPtr<SButton> Trilinear = Panel->GetTrilinearButton();
		if (!TestTrue(TEXT("the panel built a trilinear toggle"), Trilinear.IsValid()))
		{
			return false;
		}
		Trilinear->SimulateClick();
		TestTrue(TEXT("pressing the trilinear toggle enables interpolated sampling"),
			Slice.IsTrilinear());

		// THE DISCLOSURE, and it must be a DIFFERENTIAL: an advisory that is
		// always visible discloses nothing, and one that is never visible is the
		// rule 7 failure. Both states are asserted.
		TestTrue(
			TEXT("interpolated sampling is VISIBLY identified (engineering rule 7); without "
				 "this the panel shows values the solver never produced as though stored"),
			Panel->IsInterpolationAdvisoryVisible());

		Trilinear->SimulateClick();
		TestFalse(TEXT("nearest sampling raises no advisory, so the advisory means something "
					   "when it does appear"),
			Panel->IsInterpolationAdvisoryVisible());
	}

	return true;
}

/* ========================================================================== */
/* Unbound                                                                     */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSlicePanelUnboundTest,
	"FlowViz.UI.SlicePanel.Unbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSlicePanelUnboundTest::RunTest(const FString& Parameters)
{
	// NO .ViewModel(). The workspace builds panels in this state before a case is
	// open, and it is the path that crashed a sibling panel with a SIGBUS when
	// SLATE_ARGUMENT left its pointer uninitialised.
	const TSharedRef<SFlowVizSlicePanel> Panel = SNew(SFlowVizSlicePanel);

	const TSharedPtr<SButton> AxisX = Panel->GetAxisButton(EFlowVizSliceAxis::X);
	if (!TestTrue(TEXT("an unbound panel still builds its controls"), AxisX.IsValid()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Panel);

	TestFalse(TEXT("with no view model the axis buttons are disabled (rule 15)"),
		AxisX->IsEnabled());

	// SimulateClick bypasses the enabled check, so this proves the HANDLER is
	// null-safe rather than merely unreachable.
	AxisX->SimulateClick();

	if (const TSharedPtr<SFlowVizScrubSlider> Position = Panel->GetPositionSlider())
	{
		Position->SimulateDrag(0.75f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
