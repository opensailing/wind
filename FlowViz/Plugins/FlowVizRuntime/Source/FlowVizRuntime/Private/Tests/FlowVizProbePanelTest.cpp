// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizProbePanel.h"

#include "Misc/AutomationTest.h"
#include "UI/FlowVizProbeViewModel.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/**
 * The probe list's controls must drive FFlowVizProbeViewModel.
 *
 * Every state change below is produced by pressing a real button or committing a
 * real text box, never by calling a view model setter to make the next assertion
 * true. A control wired to nothing fails at the first assertion in its block.
 *
 * THE ROW INDEX IS THE INTERESTING PART. Probe rows are built in a loop, so the
 * classic defect is a handler that captured the loop variable by reference, or
 * one that always acts on row 0. Both are checked by removing the MIDDLE probe
 * and asserting the survivors are the outer two - a panel that removed the wrong
 * row still leaves the count correct.
 */

// NAMED namespace: unity build. An anonymous namespace here would merge with
// every sibling test's and collide by ODR.
namespace FlowVizProbePanelTest
{
	/** Three DIFFERENT components, so an axis mix-up cannot pass by coincidence. */
	const FVector FirstPosition(1.0, 2.0, 3.0);

	/** A SECOND, DIFFERENT position - what distinguishes "wired" from "hard-coded". */
	const FVector SecondPosition(-4.0, 5.5, 6.25);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProbePanelBindingTest,
	"FlowViz.UI.ProbePanel.ControlsDriveTheViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProbePanelBindingTest::RunTest(const FString& Parameters)
{
	FFlowVizProbeViewModel Probes;

	const TSharedRef<SFlowVizProbePanel> Panel = SNew(SFlowVizProbePanel).ViewModel(&Probes);

	/* == Placing a probe by numeric XYZ ====================================== */
	{
		TestEqual(TEXT("precondition: no probes yet"), Probes.GetProbeCount(), 0);

		const TSharedPtr<SFlowVizNumericEntry> X = Panel->GetPlacementBox(0);
		const TSharedPtr<SFlowVizNumericEntry> Y = Panel->GetPlacementBox(1);
		const TSharedPtr<SFlowVizNumericEntry> Z = Panel->GetPlacementBox(2);
		const TSharedPtr<SButton> Add = Panel->GetAddProbeButton();
		if (!TestTrue(TEXT("the panel built three placement fields and an add button"),
				X.IsValid() && Y.IsValid() && Z.IsValid() && Add.IsValid()))
		{
			return false;
		}

		// SimulateCommit, not SetText: SetText never fires OnTextCommitted, so a
		// test built on it could not observe the handler at all.
		X->SimulateCommit(FText::FromString(TEXT("1")));
		Y->SimulateCommit(FText::FromString(TEXT("2")));
		Z->SimulateCommit(FText::FromString(TEXT("3")));
		Add->SimulateClick();

		if (!TestEqual(TEXT("pressing Add places a probe"), Probes.GetProbeCount(), 1))
		{
			return false;
		}
		TestTrue(
			*FString::Printf(TEXT("the probe lands at the typed SOLVER position, not (%g,%g,%g); "
								  "three different components mean an axis swap fails here"),
				Probes.GetProbes()[0].SolverPosition.X, Probes.GetProbes()[0].SolverPosition.Y,
				Probes.GetProbes()[0].SolverPosition.Z),
			Probes.GetProbes()[0].SolverPosition.Equals(
				FlowVizProbePanelTest::FirstPosition, 1e-6));

		// A SECOND probe at a DIFFERENT position. Without this, a panel that
		// ignored the fields and always placed at (1,2,3) would pass.
		X->SimulateCommit(FText::FromString(TEXT("-4")));
		Y->SimulateCommit(FText::FromString(TEXT("5.5")));
		Z->SimulateCommit(FText::FromString(TEXT("6.25")));
		Add->SimulateClick();

		if (!TestEqual(TEXT("a second Add places a second probe"), Probes.GetProbeCount(), 2))
		{
			return false;
		}
		TestTrue(TEXT("the second probe reads the fields afresh rather than repeating the first"),
			Probes.GetProbes()[1].SolverPosition.Equals(
				FlowVizProbePanelTest::SecondPosition, 1e-6));
	}

	/* == Rename ============================================================== */
	{
		const TSharedPtr<SFlowVizNumericEntry> NameBox = Panel->GetProbeNameBox(0);
		if (!TestTrue(TEXT("the panel built a name field on row 0"), NameBox.IsValid()))
		{
			return false;
		}

		const FString Before = Probes.GetProbes()[0].Name;
		NameBox->SimulateCommit(FText::FromString(TEXT("Inlet centre")));

		TestNotEqual(
			TEXT("the new name differs from the old one, without which this check could not "
				 "fail"),
			Before, FString(TEXT("Inlet centre")));
		TestEqual(TEXT("committing a name renames that probe"),
			Probes.GetProbes()[0].Name, FString(TEXT("Inlet centre")));

		// AND IT RENAMED THE RIGHT ONE. A handler acting on row 0 regardless of
		// which row was edited would pass the assertion above.
		TestNotEqual(TEXT("renaming row 0 left row 1's name alone"),
			Probes.GetProbes()[1].Name, FString(TEXT("Inlet centre")));
	}

	/* == Visibility is a toggle, not a delete ================================ */
	{
		const TSharedPtr<SButton> Visible = Panel->GetProbeVisibleButton(1);
		if (!TestTrue(TEXT("the panel built a visibility toggle on row 1"), Visible.IsValid()))
		{
			return false;
		}

		const int32 CountBefore = Probes.GetProbeCount();
		TestTrue(TEXT("precondition: row 1 starts visible"), Probes.GetProbes()[1].bVisible);

		Visible->SimulateClick();

		TestFalse(TEXT("pressing the toggle hides that probe"), Probes.GetProbes()[1].bVisible);
		// A TOGGLE IMPLEMENTED AS RemoveProbe would satisfy nothing above but
		// would silently destroy the user's probe and its history. The count is
		// what catches it.
		TestEqual(TEXT("hiding a probe does not delete it - the count is unchanged"),
			Probes.GetProbeCount(), CountBefore);
		TestTrue(TEXT("and row 0 was not hidden along with it"), Probes.GetProbes()[0].bVisible);

		Visible->SimulateClick();
		TestTrue(TEXT("pressing it again shows the probe, so it is a toggle"),
			Probes.GetProbes()[1].bVisible);
	}

	/* == Removing the MIDDLE row removes that row ============================ */
	{
		// A third probe, so there IS a middle. With two probes, "removed the wrong
		// one" and "removed the right one" are distinguishable only by identity;
		// with three, the surviving pair names the bug directly.
		const TSharedPtr<SFlowVizNumericEntry> X = Panel->GetPlacementBox(0);
		const TSharedPtr<SFlowVizNumericEntry> Y = Panel->GetPlacementBox(1);
		const TSharedPtr<SFlowVizNumericEntry> Z = Panel->GetPlacementBox(2);
		X->SimulateCommit(FText::FromString(TEXT("9")));
		Y->SimulateCommit(FText::FromString(TEXT("9")));
		Z->SimulateCommit(FText::FromString(TEXT("9")));
		Panel->GetAddProbeButton()->SimulateClick();

		if (!TestEqual(TEXT("there are three probes to choose between"),
				Probes.GetProbeCount(), 3))
		{
			return false;
		}

		const FGuid KeepFirst = Probes.GetProbes()[0].Id;
		const FGuid RemoveMiddle = Probes.GetProbes()[1].Id;
		const FGuid KeepLast = Probes.GetProbes()[2].Id;

		const TSharedPtr<SButton> Remove = Panel->GetProbeRemoveButton(1);
		if (!TestTrue(TEXT("the panel built a remove button on row 1"), Remove.IsValid()))
		{
			return false;
		}

		Remove->SimulateClick();

		TestEqual(TEXT("removing a row removes exactly one probe"), Probes.GetProbeCount(), 2);
		// THE IDENTITIES, not the count. A handler that captured the loop index by
		// reference - so every row removes the last - keeps the count correct and
		// deletes the wrong probe.
		TestTrue(TEXT("the FIRST probe survived"), Probes.FindProbe(KeepFirst) != nullptr);
		TestTrue(TEXT("the LAST probe survived"), Probes.FindProbe(KeepLast) != nullptr);
		TestTrue(TEXT("the MIDDLE probe - the one whose button was pressed - is gone"),
			Probes.FindProbe(RemoveMiddle) == nullptr);
	}

	/* == Rule 15: Clear is dead when there is nothing to clear =============== */
	{
		const TSharedPtr<SButton> Clear = Panel->GetRemoveAllButton();
		if (!TestTrue(TEXT("the panel built a clear button"), Clear.IsValid()))
		{
			return false;
		}

		// EVALUATE THE BOUND ATTRIBUTES, as a drawn frame would. Without this the
		// assertion reads the constructor's cached `true` rather than the
		// predicate. See Tests/FlowVizSlateAttributePump.h.
		FlowVizSlateAttributePump::Pump(Panel);
		TestTrue(TEXT("with probes present, Clear is live"), Clear->IsEnabled());

		Clear->SimulateClick();
		TestEqual(TEXT("Clear removes every probe"), Probes.GetProbeCount(), 0);

		FlowVizSlateAttributePump::Pump(Panel);
		// THE PAIR IS WHAT MAKES THIS A CHECK. The TRUE above and the FALSE here
		// differ only in what the view model says, so a button hard-wired to
		// either state fails one of them.
		TestFalse(TEXT("with no probes, Clear is dead (rule 15: a button that would do "
					   "nothing must not look pressable)"),
			Clear->IsEnabled());
	}

	/* == The line probe ====================================================== */
	{
		TestFalse(TEXT("precondition: no line probe yet"), Probes.HasLineProbe());

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Panel->GetLineStartBox(Axis)->SimulateCommit(FText::FromString(TEXT("0")));
		}
		// A DIFFERENT value per axis on the end point, so a handler that wrote one
		// component to all three is caught.
		Panel->GetLineEndBox(0)->SimulateCommit(FText::FromString(TEXT("2")));
		Panel->GetLineEndBox(1)->SimulateCommit(FText::FromString(TEXT("4")));
		Panel->GetLineEndBox(2)->SimulateCommit(FText::FromString(TEXT("8")));

		const TSharedPtr<SButton> SetLine = Panel->GetSetLineButton();
		if (!TestTrue(TEXT("the panel built a set-line button"), SetLine.IsValid()))
		{
			return false;
		}
		SetLine->SimulateClick();

		if (!TestTrue(TEXT("pressing it defines the line probe"), Probes.HasLineProbe()))
		{
			return false;
		}
		TestTrue(TEXT("the line runs to the typed end point, each axis its own value"),
			Probes.GetLineEnd().Equals(FVector(2.0, 4.0, 8.0), 1e-6));
		TestTrue(TEXT("and starts at the typed start point"),
			Probes.GetLineStart().Equals(FVector::ZeroVector, 1e-6));

		const int32 SamplesBefore = Probes.GetLineSampleCount();
		const TSharedPtr<SFlowVizNumericEntry> SamplesBox = Panel->GetLineSamplesBox();
		if (!TestTrue(TEXT("the panel built a sample-count field"), SamplesBox.IsValid()))
		{
			return false;
		}

		// A COUNT DIFFERENT FROM THE CURRENT ONE, asserted so - otherwise a
		// no-op handler would be indistinguishable from a working one.
		constexpr int32 TargetSamples = 17;
		if (!TestTrue(
				*FString::Printf(TEXT("the target %d differs from the current %d, without which "
									  "this check could not fail"),
					TargetSamples, SamplesBefore),
				TargetSamples != SamplesBefore))
		{
			return false;
		}

		SamplesBox->SimulateCommit(FText::FromString(TEXT("17")));
		TestEqual(TEXT("committing a sample count reaches the view model"),
			Probes.GetLineSampleCount(), TargetSamples);
	}

	return true;
}

/* ========================================================================== */
/* Rule 10: an unsampled probe must not read as zero                           */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProbePanelNoReadingTest,
	"FlowViz.UI.ProbePanel.UnsampledProbeDoesNotReadAsZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProbePanelNoReadingTest::RunTest(const FString& Parameters)
{
	FFlowVizProbeViewModel Probes;
	const TSharedRef<SFlowVizProbePanel> Panel = SNew(SFlowVizProbePanel).ViewModel(&Probes);

	const FGuid Id = Probes.AddProbeAtSolverPosition(FVector(1.0, 2.0, 3.0), TEXT("P"));
	if (!TestTrue(TEXT("a probe was placed"), Id.IsValid()))
	{
		return false;
	}

	// A FRESH PROBE HAS NO READING: bHasValue is false and Components is empty.
	TestFalse(TEXT("precondition: the probe has not been sampled"),
		Probes.GetProbes()[0].LastReading.bHasValue);

	const FText Unsampled = Panel->GetProbeValueText(0);

	// THE DEFECT THIS EXISTS TO CATCH. FFlowVizProbeReading::Magnitude is 0.0 by
	// default, so a readout that formats it unconditionally prints "0" - a
	// plausible physical value, in a field where zero velocity is a real and
	// interesting result. The user cannot tell it from a measurement.
	TestFalse(
		TEXT("an unsampled probe does not display a numeric zero (engineering rule 10: "
			 "missing data is never zero-filled)"),
		Unsampled.ToString().Contains(TEXT("0")));
	TestFalse(TEXT("and it says something rather than nothing"), Unsampled.IsEmpty());

	// NOW GIVE IT A READING, and check the readout changes. Without this the
	// assertion above would also pass on a panel that displays a constant string
	// and can never show a value at all.
	FFlowVizProbeReading Reading;
	Reading.bHasValue = true;
	Reading.bInsideDomain = true;
	Reading.Components = {3.0, 4.0};
	Reading.Magnitude = 5.0;
	Reading.Voxel = FIntVector(7, 8, 9);
	Reading.FrameIndex = 2;
	TestTrue(TEXT("the reading was stored"), Probes.SetProbeReading(Id, Reading));

	const FText Sampled = Panel->GetProbeValueText(0);
	TestNotEqual(TEXT("a sampled probe reads differently from an unsampled one"),
		Sampled.ToString(), Unsampled.ToString());
	TestTrue(
		*FString::Printf(TEXT("the readout shows the magnitude (got '%s')"), *Sampled.ToString()),
		Sampled.ToString().Contains(TEXT("5")));

	return true;
}

/* ========================================================================== */
/* Unbound                                                                     */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProbePanelUnboundTest,
	"FlowViz.UI.ProbePanel.Unbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProbePanelUnboundTest::RunTest(const FString& Parameters)
{
	// NO .ViewModel(). The workspace builds panels in this state before a case is
	// open, and it is the path that crashed a sibling panel with a SIGBUS when
	// SLATE_ARGUMENT left its pointer uninitialised.
	const TSharedRef<SFlowVizProbePanel> Panel = SNew(SFlowVizProbePanel);

	const TSharedPtr<SButton> Add = Panel->GetAddProbeButton();
	if (!TestTrue(TEXT("an unbound panel still builds its controls"), Add.IsValid()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Panel);
	TestFalse(TEXT("with no view model the add button is disabled (rule 15)"), Add->IsEnabled());

	// SimulateClick bypasses the enabled check, so this proves the HANDLER is
	// null-safe rather than merely unreachable.
	Add->SimulateClick();
	if (const TSharedPtr<SButton> Clear = Panel->GetRemoveAllButton())
	{
		Clear->SimulateClick();
	}
	if (const TSharedPtr<SFlowVizNumericEntry> X = Panel->GetPlacementBox(0))
	{
		X->SimulateCommit(FText::FromString(TEXT("1")));
	}

	// And the row accessors report "no such row" rather than indexing an array.
	TestFalse(TEXT("an unbound panel has no probe rows"),
		Panel->GetProbeRemoveButton(0).IsValid());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
