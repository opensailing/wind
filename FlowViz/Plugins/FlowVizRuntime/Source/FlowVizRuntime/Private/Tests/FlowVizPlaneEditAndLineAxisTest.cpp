// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "UI/FlowVizClipViewModel.h"
#include "UI/FlowVizProbeViewModel.h"
#include "UI/SFlowVizClipPanel.h"
#include "UI/SFlowVizProbePanel.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizPlaneEditAndLineAxisTest
{
	/** Three DIFFERENT extents, matching the sibling clip tests. */
	const FVector TestDomain(2.0, 4.0, 8.0);

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
 * A plane's DISTANCE is editable in place (#75: SetPlane had no production
 * caller). Planes could be added, hidden, inverted and deleted -- but not
 * MOVED: repositioning one meant delete-and-re-add, which loses the enabled
 * state and the row's identity. The distance box closes that.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipPlaneEditTest,
	"FlowViz.UI.ClipPanel.PlaneDistanceIsEditable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizClipPlaneEditTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizPlaneEditAndLineAxisTest;

	FFlowVizClipViewModel Clip;
	if (!TestTrue(TEXT("the domain is accepted"), Clip.SetDomainSize(TestDomain).IsOk()))
	{
		return false;
	}

	FAnnounceCounter Counter;
	const TSharedRef<SFlowVizClipPanel> Panel = SNew(SFlowVizClipPanel)
		.ViewModel(&Clip)
		.OnClipChanged(Counter.MakeDelegate());

	// A plane to edit, added through the panel so the row exists.
	const TSharedPtr<SButton> KeepPlusZ = Panel->GetPresetButton(EFlowVizClipPreset::KeepPlusZ);
	if (!TestTrue(TEXT("CONTROL: the +Z preset button exists"), KeepPlusZ.IsValid()))
	{
		return false;
	}
	KeepPlusZ->SimulateClick();
	if (!TestEqual(TEXT("CONTROL: the preset added a plane"), Clip.GetPlaneCount(), 1))
	{
		return false;
	}

	const TSharedPtr<SFlowVizNumericEntry> DistanceBox = Panel->GetPlaneDistanceBox(0);
	if (!TestTrue(TEXT("the plane row built a distance box"), DistanceBox.IsValid()))
	{
		return false;
	}

	/* == An accepted edit moves the plane and announces ====================== */
	{
		// -0.375 is not a value any preset produces (they sit at the domain
		// edges), so the assertion cannot pass on a plane that did not move.
		const int32 Before = Counter.Count;
		DistanceBox->SimulateCommit(FText::FromString(TEXT("-0.375")));

		const FFlowVizClipPlane* Plane = Clip.FindPlane(0);
		if (!TestNotNull(TEXT("the plane still exists"), Plane))
		{
			return false;
		}
		TestTrue(TEXT("committing a distance moves the plane in place"),
			FMath::IsNearlyEqual(Plane->Distance, -0.375, 1.0e-9));
		TestEqual(TEXT("and announces, so the renderer follows"), Counter.Count, Before + 1);

		// THE NORMAL AND THE ENABLED STATE SURVIVE the edit -- the whole point
		// of in-place editing over delete-and-re-add.
		TestTrue(TEXT("the normal is untouched"),
			Plane->Normal.Equals(FVector(0.0, 0.0, 1.0), 1.0e-9));
		TestTrue(TEXT("the plane is still enabled"), Plane->bEnabled);
	}

	/* == Refusals: garbage and non-finite do not announce ==================== */
	{
		const int32 Before = Counter.Count;
		DistanceBox->SimulateCommit(FText::FromString(TEXT("abc")));
		const FFlowVizClipPlane* Plane = Clip.FindPlane(0);
		TestTrue(TEXT("unparseable text leaves the distance unchanged"),
			Plane != nullptr && FMath::IsNearlyEqual(Plane->Distance, -0.375, 1.0e-9));
		TestEqual(TEXT("and does not announce"), Counter.Count, Before);

		DistanceBox->SimulateCommit(FText::FromString(TEXT("-0.375")));
		TestEqual(TEXT("re-committing the held value does not announce"), Counter.Count, Before);
	}

	return true;
}

/**
 * The line probe's X axis is selectable (#75: SetLineAxisMode had no
 * production caller). Distance and normalized-distance produce different
 * x-coordinates for the same samples; normalized is what makes two lines of
 * different lengths comparable, and nothing could select it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProbeLineAxisTest,
	"FlowViz.UI.ProbePanel.LineAxisIsSelectable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProbeLineAxisTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizPlaneEditAndLineAxisTest;

	FFlowVizProbeViewModel Probes;
	const TSharedRef<SFlowVizProbePanel> Panel = SNew(SFlowVizProbePanel).ViewModel(&Probes);

	const TSharedPtr<SButton> AxisButton = Panel->GetLineAxisButton();
	if (!TestTrue(TEXT("the panel built a line axis toggle"), AxisButton.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("precondition: the axis mode is Distance"),
		Probes.GetLineAxisMode(), EFlowVizLineProbeAxis::Distance);

	AxisButton->SimulateClick();
	TestEqual(TEXT("clicking the toggle selects normalized distance"),
		Probes.GetLineAxisMode(), EFlowVizLineProbeAxis::NormalizedDistance);

	AxisButton->SimulateClick();
	TestEqual(TEXT("clicking again returns to solver-unit distance"),
		Probes.GetLineAxisMode(), EFlowVizLineProbeAxis::Distance);

	/* == The mode actually changes the axis values =========================== */
	{
		// A 10-unit line: under Distance the last sample reads 10, under
		// Normalized it reads 1. The values differ, so a toggle wired to a
		// widget-side bool rather than the model fails the second read.
		TestTrue(TEXT("CONTROL: a line probe is accepted"),
			Probes.SetLineProbe(FVector(1.0, 2.0, 0.5), FVector(11.0, 2.0, 0.5)).IsOk());

		TArray<double> AxisValues;
		TestTrue(TEXT("CONTROL: axis values are produced"),
			Probes.GetLineSampleAxisValues(AxisValues).IsOk());
		TestTrue(TEXT("under Distance the far endpoint reads the line's length"),
			AxisValues.Num() > 0 && FMath::IsNearlyEqual(AxisValues.Last(), 10.0, 1.0e-9));

		AxisButton->SimulateClick();
		AxisValues.Reset();
		TestTrue(TEXT("axis values are produced after the toggle"),
			Probes.GetLineSampleAxisValues(AxisValues).IsOk());
		TestTrue(TEXT("under NormalizedDistance the far endpoint reads 1"),
			AxisValues.Num() > 0 && FMath::IsNearlyEqual(AxisValues.Last(), 1.0, 1.0e-9));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
