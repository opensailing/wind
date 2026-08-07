// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "UI/FlowVizTransferFunctionViewModel.h"
#include "UI/SFlowVizTransferFunctionPanel.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizTransferFunctionDisclosureTest
{
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
 * The clamp toggle and the disclosure palette drive the view model.
 *
 * SetClampToRange and the four disclosure colour setters had NO production
 * caller (#75): #29 carried bClampToRange all the way to the shader, and no
 * widget could flip it -- the exact welded-control shape of #74, in the panel
 * that already existed. The disclosure palette is VISUAL_QA rule 4's own
 * surface: each colour names WHY a voxel is not showing data, and each swatch
 * set must keep the five distinguishable, which is why the options are a fixed
 * palette per slot rather than a picker.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionDisclosureTest,
	"FlowViz.UI.TransferFunctionPanel.DisclosureControls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransferFunctionDisclosureTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizTransferFunctionDisclosureTest;

	FFlowVizTransferFunctionViewModel Model;
	FAnnounceCounter Counter;

	const TSharedRef<SFlowVizTransferFunctionPanel> Panel =
		SNew(SFlowVizTransferFunctionPanel)
			.ViewModel(&Model)
			.OnTransferFunctionChanged(Counter.MakeDelegate());

	/* == The clamp toggle ==================================================== */
	{
		const TSharedPtr<SButton> ClampButton = Panel->GetClampButton();
		if (!TestTrue(TEXT("the panel built a clamp toggle"), ClampButton.IsValid()))
		{
			return false;
		}

		TestFalse(TEXT("precondition: clamp is off, so out-of-range voxels take the "
					   "under/over colours"),
			Model.IsClampToRange());

		const int32 Before = Counter.Count;
		ClampButton->SimulateClick();
		TestTrue(TEXT("clicking the clamp toggle turns clamping on"), Model.IsClampToRange());
		TestEqual(TEXT("and announces, so the renderer follows"), Counter.Count, Before + 1);

		ClampButton->SimulateClick();
		TestFalse(TEXT("clicking again turns it off"), Model.IsClampToRange());
	}

	/* == The disclosure swatches ============================================= */
	{
		// Each of the four slots has its own swatch row; picking a swatch changes
		// THAT colour and no other. The panel offers a fixed set per slot, and
		// index 0 must not be the shipped default for the slot under test --
		// asserted below rather than assumed, so a palette edit cannot silently
		// vacuate the test (a-control-needs-a-known-nonzero-expectation).
		struct FSlotProbe
		{
			const TCHAR* Name;
			int32 Slot;
			TFunction<FLinearColor()> Get;
		};
		const FSlotProbe Probes[] = {
			{ TEXT("NaN"), SFlowVizTransferFunctionPanel::DisclosureSlotNaN,
				[&] { return Model.GetTransferFunction().NaNColor; } },
			{ TEXT("masked"), SFlowVizTransferFunctionPanel::DisclosureSlotMasked,
				[&] { return Model.GetTransferFunction().MaskedColor; } },
			{ TEXT("under-range"), SFlowVizTransferFunctionPanel::DisclosureSlotUnderRange,
				[&] { return Model.GetTransferFunction().UnderRangeColor; } },
			{ TEXT("over-range"), SFlowVizTransferFunctionPanel::DisclosureSlotOverRange,
				[&] { return Model.GetTransferFunction().OverRangeColor; } },
		};

		for (const FSlotProbe& Probe : Probes)
		{
			const FLinearColor Before = Probe.Get();

			// Find an alternative that differs from the current colour.
			int32 DifferentIndex = INDEX_NONE;
			const int32 SwatchCount =
				SFlowVizTransferFunctionPanel::GetDisclosureSwatchCount(Probe.Slot);
			if (!TestTrue(
					FString::Printf(TEXT("the %s slot offers at least two swatches"), Probe.Name),
					SwatchCount >= 2))
			{
				continue;
			}
			for (int32 Index = 0; Index < SwatchCount; ++Index)
			{
				if (!SFlowVizTransferFunctionPanel::GetDisclosureSwatchColor(Probe.Slot, Index)
						 .Equals(Before))
				{
					DifferentIndex = Index;
					break;
				}
			}
			if (!TestTrue(FString::Printf(
					TEXT("CONTROL: some %s swatch differs from the shipped default"), Probe.Name),
					DifferentIndex != INDEX_NONE))
			{
				continue;
			}

			const TSharedPtr<SButton> Swatch =
				Panel->GetDisclosureSwatchButton(Probe.Slot, DifferentIndex);
			if (!TestTrue(FString::Printf(TEXT("the %s swatch button exists"), Probe.Name),
					Swatch.IsValid()))
			{
				continue;
			}

			const int32 CountBefore = Counter.Count;
			Swatch->SimulateClick();
			TestTrue(FString::Printf(TEXT("clicking a %s swatch changes that colour"), Probe.Name),
				Probe.Get().Equals(SFlowVizTransferFunctionPanel::GetDisclosureSwatchColor(
					Probe.Slot, DifferentIndex)));
			TestEqual(FString::Printf(TEXT("and announces once for %s"), Probe.Name),
				Counter.Count, CountBefore + 1);

			// Re-clicking the now-selected swatch is a no-op and must not push a
			// byte-identical function to the render thread.
			Swatch->SimulateClick();
			TestEqual(FString::Printf(
					TEXT("re-clicking the selected %s swatch does not announce"), Probe.Name),
				Counter.Count, CountBefore + 1);
		}

		/* -- Rule 4: after any selection, the five disclosure colours stay
		      pairwise distinguishable. The palettes are constructed to make
		      collisions impossible; this asserts the CONSTRUCTION, so a palette
		      edit that lets NaN equal masked fails here rather than in a
		      screenshot review. ----------------------------------------------- */
		const FFlowVizTransferFunction& Function = Model.GetTransferFunction();
		const FLinearColor Four[] = {
			Function.NaNColor, Function.MaskedColor, Function.UnderRangeColor,
			Function.OverRangeColor
		};
		for (int32 A = 0; A < 4; ++A)
		{
			for (int32 B = A + 1; B < 4; ++B)
			{
				TestFalse(FString::Printf(
					TEXT("disclosure colours %d and %d remain distinguishable (rule 4)"), A, B),
					Four[A].Equals(Four[B]));
			}
		}
	}

	return true;
}

/**
 * Unbound: the new controls are disabled and null-safe, like the rest of the
 * panel.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionDisclosureUnboundTest,
	"FlowViz.UI.TransferFunctionPanel.DisclosureUnbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransferFunctionDisclosureUnboundTest::RunTest(const FString& Parameters)
{
	const TSharedRef<SFlowVizTransferFunctionPanel> Panel = SNew(SFlowVizTransferFunctionPanel);

	const TSharedPtr<SButton> ClampButton = Panel->GetClampButton();
	if (!TestTrue(TEXT("an unbound panel still builds the clamp toggle"), ClampButton.IsValid()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Panel);
	TestFalse(TEXT("with no view model the clamp toggle is disabled (rule 15)"),
		ClampButton->IsEnabled());

	ClampButton->SimulateClick();
	if (const TSharedPtr<SButton> Swatch = Panel->GetDisclosureSwatchButton(
			SFlowVizTransferFunctionPanel::DisclosureSlotNaN, 0))
	{
		Swatch->SimulateClick();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
