// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizClipPanel.h"

#include "Misc/AutomationTest.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/**
 * The clip panel's controls must drive FFlowVizClipViewModel.
 *
 * NOTHING HERE CALLS A VIEW MODEL SETTER TO MAKE AN ASSERTION TRUE. Every state
 * change is produced by pressing a real button or committing a real text box, so
 * a control wired to nothing fails at the first assertion in its block. Where a
 * setter IS called it is to establish a PRECONDITION that makes the following
 * assertion able to fail - see the note on the enablement pair below.
 *
 * DIFFERENTIAL PROPERTY. Remove `.OnClicked(...)` from the preset buttons in
 * SFlowVizClipPanel::Construct and "pressing a preset adds a plane" goes red.
 * Remove the IsEnabled binding on the add row and the rule 15 pair below goes
 * red on its FALSE assertion.
 */

// NAMED namespace: unity build. Every .cpp in this module compiles into one
// translation unit, so an anonymous namespace here would collide by ODR with the
// anonymous namespace of every sibling test.
namespace FlowVizClipPanelTest
{
	/** A domain with three DIFFERENT extents, deliberately. */
	const FVector TestDomain(2.0, 4.0, 8.0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipPanelBindingTest,
	"FlowViz.UI.ClipPanel.ControlsDriveTheViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizClipPanelBindingTest::RunTest(const FString& Parameters)
{
	FFlowVizClipViewModel Clip;
	if (!TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipPanelTest::TestDomain).IsOk()))
	{
		return false;
	}

	const TSharedRef<SFlowVizClipPanel> Panel = SNew(SFlowVizClipPanel).ViewModel(&Clip);

	/* == Pressing a preset button adds a plane facing that way ================ */
	{
		TestEqual(TEXT("precondition: no planes yet"), Clip.GetPlaneCount(), 0);

		const TSharedPtr<SButton> KeepPlusX =
			Panel->GetPresetButton(EFlowVizClipPreset::KeepPlusX);
		if (!TestTrue(TEXT("the panel built a +X preset button"), KeepPlusX.IsValid()))
		{
			return false;
		}

		KeepPlusX->SimulateClick();

		if (!TestEqual(
				TEXT("pressing a preset adds a plane; if this fails the button is not wired to "
					 "the clip view model"),
				Clip.GetPlaneCount(), 1))
		{
			return false;
		}

		// AND IT IS THE PLANE THAT BUTTON NAMES. A panel that wired every preset
		// to the same call would pass the count assertion above while offering six
		// buttons that do one thing.
		const FFlowVizClipPlane* Plane = Clip.FindPlane(0);
		if (!TestTrue(TEXT("the added plane is readable"), Plane != nullptr))
		{
			return false;
		}
		TestTrue(
			*FString::Printf(TEXT("the +X preset faces +X, not (%g,%g,%g)"),
				Plane->Normal.X, Plane->Normal.Y, Plane->Normal.Z),
			Plane->Normal.Equals(FVector(1.0, 0.0, 0.0), 1e-6));

		// A SECOND, DIFFERENT PRESET. This is what distinguishes "the buttons are
		// wired" from "one button is wired six times".
		const TSharedPtr<SButton> KeepMinusZ =
			Panel->GetPresetButton(EFlowVizClipPreset::KeepMinusZ);
		if (!TestTrue(TEXT("the panel built a -Z preset button"), KeepMinusZ.IsValid()))
		{
			return false;
		}
		KeepMinusZ->SimulateClick();

		TestEqual(TEXT("the second preset adds a second plane"), Clip.GetPlaneCount(), 2);
		if (const FFlowVizClipPlane* Second = Clip.FindPlane(1))
		{
			TestTrue(
				*FString::Printf(TEXT("the -Z preset faces -Z, not (%g,%g,%g)"),
					Second->Normal.X, Second->Normal.Y, Second->Normal.Z),
				Second->Normal.Equals(FVector(0.0, 0.0, -1.0), 1e-6));
		}
	}

	/* == The enable toggle hides a plane without deleting it ================== */
	{
		const int32 CountBefore = Clip.GetPlaneCount();
		TestEqual(TEXT("precondition: both planes are enabled"),
			Clip.GetEnabledPlaneCount(), CountBefore);

		const TSharedPtr<SButton> Toggle = Panel->GetPlaneEnableButton(0);
		if (!TestTrue(TEXT("the panel built an enable toggle for plane 0"), Toggle.IsValid()))
		{
			return false;
		}

		Toggle->SimulateClick();

		TestEqual(
			TEXT("toggling disables the plane, so the shader stops receiving it"),
			Clip.GetEnabledPlaneCount(), CountBefore - 1);

		// THE DISTINCTION THIS PANEL EXISTS TO PRESERVE: hidden is not deleted.
		// A toggle implemented as RemovePlane would pass the assertion above and
		// silently destroy the user's plane.
		TestEqual(TEXT("the plane is retained, not removed"), Clip.GetPlaneCount(), CountBefore);

		Toggle->SimulateClick();
		TestEqual(TEXT("toggling again re-enables it, so it is a toggle"),
			Clip.GetEnabledPlaneCount(), CountBefore);
	}

	/* == Invert flips BOTH the normal and the distance ======================== */
	{
		const FFlowVizClipPlane* Before = Clip.FindPlane(0);
		if (!TestTrue(TEXT("plane 0 is readable"), Before != nullptr))
		{
			return false;
		}
		const FVector NormalBefore = Before->Normal;
		const double DistanceBefore = Before->Distance;

		// A NON-ZERO DISTANCE IS REQUIRED FOR THIS TO MEAN ANYTHING. Negating zero
		// gives zero, so a panel that flipped only the normal would be
		// indistinguishable from a correct one if D were 0. The preset places the
		// plane through the domain centre, so D is -1.0 here; asserted rather than
		// assumed.
		if (!TestTrue(
				*FString::Printf(
					TEXT("the plane's distance is non-zero (%g), without which the distance half "
						 "of this assertion could not fail"),
					DistanceBefore),
				FMath::Abs(DistanceBefore) > 1e-6))
		{
			return false;
		}

		const TSharedPtr<SButton> Invert = Panel->GetPlaneInvertButton(0);
		if (!TestTrue(TEXT("the panel built an invert button for plane 0"), Invert.IsValid()))
		{
			return false;
		}

		Invert->SimulateClick();

		const FFlowVizClipPlane* After = Clip.FindPlane(0);
		if (!TestTrue(TEXT("plane 0 survives inversion"), After != nullptr))
		{
			return false;
		}
		TestTrue(TEXT("inverting negates the normal"),
			After->Normal.Equals(-NormalBefore, 1e-6));
		TestTrue(
			*FString::Printf(
				TEXT("inverting negates the DISTANCE too (%g -> %g); negating only the normal "
					 "moves the plane instead of flipping which side is kept"),
				DistanceBefore, After->Distance),
			FMath::IsNearlyEqual(After->Distance, -DistanceBefore, 1e-6));
	}

	/* == The remove button removes ============================================ */
	{
		const int32 CountBefore = Clip.GetPlaneCount();
		const TSharedPtr<SButton> Remove = Panel->GetPlaneRemoveButton(0);
		if (!TestTrue(TEXT("the panel built a remove button for plane 0"), Remove.IsValid()))
		{
			return false;
		}

		Remove->SimulateClick();
		TestEqual(TEXT("pressing remove removes the plane"),
			Clip.GetPlaneCount(), CountBefore - 1);
	}

	/* == Rule 15: the add controls die at the shader's six-plane limit ======== */
	{
		Clip.RemoveAllPlanes();

		const TSharedPtr<SButton> AddButton =
			Panel->GetPresetButton(EFlowVizClipPreset::KeepPlusX);
		if (!TestTrue(TEXT("the +X preset button exists"), AddButton.IsValid()))
		{
			return false;
		}

		// EVALUATE THE BOUND ATTRIBUTES, as a drawn frame would. Without this the
		// assertions below read the constructor's cached `true` (SWidget.cpp:248)
		// rather than the predicate. See Tests/FlowVizSlateAttributePump.h.
		FlowVizSlateAttributePump::Pump(Panel);

		TestTrue(TEXT("with room for more planes the add control is live"),
			AddButton->IsEnabled());

		// Fill to the shader's limit THROUGH THE BUTTON, which also proves the
		// limit is reached by the same path a user takes.
		for (int32 Index = 0; Index < FlowVizRayMarch::MaxClipPlanes; ++Index)
		{
			AddButton->SimulateClick();
		}

		if (!TestEqual(TEXT("the panel added planes up to the shader's limit"),
				Clip.GetPlaneCount(), FlowVizRayMarch::MaxClipPlanes))
		{
			return false;
		}
		TestFalse(TEXT("precondition: the view model would now refuse another plane"),
			Clip.CanAddPlane());

		FlowVizSlateAttributePump::Pump(Panel);

		// THE PAIR IS WHAT MAKES THIS A CHECK. The TRUE above and the FALSE here
		// differ only in what the view model says, so a button hard-wired to either
		// state fails one of them. Rule 15: disabled and refused are one fact.
		TestFalse(
			TEXT("at the six-plane limit the add control is disabled (engineering rule 15); a "
				 "seventh press would be silently refused"),
			AddButton->IsEnabled());
	}

	/* == The crop box entry boxes commit to the view model ==================== */
	{
		Clip.ResetCropBox();
		TestFalse(TEXT("precondition: the crop is the whole domain"), Clip.IsCropActive());

		const TSharedPtr<SFlowVizNumericEntry> MaxX = Panel->GetCropMaxBox(0);
		if (!TestTrue(TEXT("the panel built a crop max-X box"), MaxX.IsValid()))
		{
			return false;
		}

		// SimulateCommit, not SetText: SetText never fires OnTextCommitted, so a
		// test built on it could not observe the handler at all.
		MaxX->SimulateCommit(FText::FromString(TEXT("1.5")));

		TestTrue(TEXT("committing a crop bound activates the crop"), Clip.IsCropActive());
		TestTrue(
			*FString::Printf(TEXT("the committed value reaches the view model (max.X = %g)"),
				Clip.GetCropMax().X),
			FMath::IsNearlyEqual(Clip.GetCropMax().X, 1.5, 1e-6));

		// AND THE OTHER AXES ARE UNTOUCHED. A handler that wrote the whole vector
		// from one box would pass the assertion above and quietly crop Y and Z.
		TestTrue(
			*FString::Printf(TEXT("committing X leaves Y at the domain extent (%g)"),
				Clip.GetCropMax().Y),
			FMath::IsNearlyEqual(Clip.GetCropMax().Y, FlowVizClipPanelTest::TestDomain.Y, 1e-6));
	}

	return true;
}

/* ========================================================================== */
/* Unbound                                                                     */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipPanelUnboundTest,
	"FlowViz.UI.ClipPanel.Unbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizClipPanelUnboundTest::RunTest(const FString& Parameters)
{
	// NO .ViewModel(). This is the state the workspace builds panels in before a
	// case is open, and it is the path that crashed a sibling panel with a SIGBUS
	// when SLATE_ARGUMENT left its pointer uninitialised.
	const TSharedRef<SFlowVizClipPanel> Panel = SNew(SFlowVizClipPanel);

	const TSharedPtr<SButton> Preset = Panel->GetPresetButton(EFlowVizClipPreset::KeepPlusX);
	if (!TestTrue(TEXT("an unbound panel still builds its controls"), Preset.IsValid()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Panel);

	TestFalse(TEXT("with no view model the preset buttons are disabled (rule 15)"),
		Preset->IsEnabled());

	// SimulateClick bypasses the enabled check, so this proves the HANDLER is
	// null-safe rather than merely unreachable.
	Preset->SimulateClick();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
