// Copyright FlowViz contributors. All Rights Reserved.

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Playback/FlowVizCasePlayer.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/SFlowVizTransportBar.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizTransportOptionsTest
{
	FString GetSampleCaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"));
	}
}

/**
 * The transport bar's OPTION controls drive the timeline view model.
 *
 * The five transport buttons and the scrubber were wired from the start; the
 * options were not. FFlowVizTimelineViewModel carried SetLoopMode,
 * SetPlaybackMode, SetSpeedPresetIndex, SetCustomSpeed and
 * SetInterpolationEnabled with NO production caller anywhere (#75, found by
 * check_uncalled_setters.sh) -- so loop, speed and interpolation were welded
 * to the player's defaults exactly the way the render settings were welded to
 * the view model's (#74). Same defect, one panel over.
 *
 * Every change below is produced by pressing a real button; asserting through
 * the view model's getters, which read the PLAYER's settings -- so a button
 * wired to a widget-side cache rather than the model fails here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransportOptionsBindingTest,
	"FlowViz.UI.TransportBar.OptionsDriveTheViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransportOptionsBindingTest::RunTest(const FString& Parameters)
{
	const FString CaseDir = FlowVizTransportOptionsTest::GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		AddError(FString::Printf(
			TEXT("the sample case is required to make the options live; not found at '%s'"),
			*CaseDir));
		return false;
	}

	FFlowVizWorkspaceModel Workspace;
	const FCFDVizResult OpenResult = Workspace.OpenCase(CaseDir);
	if (!OpenResult.IsOk())
	{
		AddError(FString::Printf(TEXT("could not open the sample case: %s"), *OpenResult.ToString()));
		return false;
	}

	const TSharedRef<SFlowVizTransportBar> Bar = SNew(SFlowVizTransportBar)
		.TimelineViewModel(&Workspace.Timeline);

	/* == Loop mode cycles through all three states =========================== */
	{
		const TSharedPtr<SButton> LoopButton = Bar->GetLoopModeButton();
		if (!TestTrue(TEXT("the bar built a loop mode button"), LoopButton.IsValid()))
		{
			return false;
		}

		// The default is Loop, so the first click is a CHANGE -- "it was already
		// Loop" cannot make an assertion pass without the click doing anything.
		TestEqual(TEXT("precondition: the loop mode is Loop"),
			Workspace.Timeline.GetLoopMode(), EFlowVizLoopMode::Loop);

		LoopButton->SimulateClick();
		TestEqual(TEXT("one click advances Loop to PingPong"),
			Workspace.Timeline.GetLoopMode(), EFlowVizLoopMode::PingPong);

		LoopButton->SimulateClick();
		TestEqual(TEXT("a second click advances PingPong to Once"),
			Workspace.Timeline.GetLoopMode(), EFlowVizLoopMode::Once);

		LoopButton->SimulateClick();
		TestEqual(TEXT("a third click wraps back to Loop -- the cycle covers every state, so no "
					   "mode is unreachable from the button"),
			Workspace.Timeline.GetLoopMode(), EFlowVizLoopMode::Loop);
	}

	/* == Playback mode toggles Sequence <-> RealTime ========================= */
	{
		const TSharedPtr<SButton> ModeButton = Bar->GetPlaybackModeButton();
		if (!TestTrue(TEXT("the bar built a playback mode button"), ModeButton.IsValid()))
		{
			return false;
		}

		TestEqual(TEXT("precondition: the playback mode is Sequence"),
			Workspace.Timeline.GetPlaybackMode(), EFlowVizPlaybackMode::Sequence);

		ModeButton->SimulateClick();
		TestEqual(TEXT("clicking the mode button switches to RealTime"),
			Workspace.Timeline.GetPlaybackMode(), EFlowVizPlaybackMode::RealTime);

		ModeButton->SimulateClick();
		TestEqual(TEXT("clicking again returns to Sequence"),
			Workspace.Timeline.GetPlaybackMode(), EFlowVizPlaybackMode::Sequence);
	}

	/* == Speed preset buttons select the preset ============================== */
	{
		// 1.0 is preset index 3; pick a DIFFERENT one so the click is a change.
		const int32 TwoXIndex = 4;
		TestTrue(TEXT("CONTROL: the chosen preset is not the current speed"),
			!FMath::IsNearlyEqual(
				FlowVizPlayback::SpeedPresets[TwoXIndex], Workspace.Timeline.GetSpeed(), 1.0e-9));

		const TSharedPtr<SButton> TwoXButton = Bar->GetSpeedPresetButton(TwoXIndex);
		if (!TestTrue(TEXT("the bar built a button for the 2x preset"), TwoXButton.IsValid()))
		{
			return false;
		}
		TwoXButton->SimulateClick();
		TestEqual(TEXT("clicking a speed preset applies that speed"),
			Workspace.Timeline.GetSpeed(), FlowVizPlayback::SpeedPresets[TwoXIndex]);
		TestEqual(TEXT("and the preset index reports it as selected"),
			Workspace.Timeline.GetSpeedPresetIndex(), TwoXIndex);

		// EVERY preset must have a button; a bar offering four of seven would be
		// the render panel's five-of-six-modes defect wearing playback clothes.
		for (int32 Index = 0; Index < FlowVizPlayback::NumSpeedPresets; ++Index)
		{
			const TSharedPtr<SButton> PresetButton = Bar->GetSpeedPresetButton(Index);
			if (!TestTrue(FString::Printf(TEXT("a button exists for preset %d"), Index),
					PresetButton.IsValid()))
			{
				continue;
			}
			PresetButton->SimulateClick();
			TestEqual(FString::Printf(TEXT("clicking preset %d applies it"), Index),
				Workspace.Timeline.GetSpeed(), FlowVizPlayback::SpeedPresets[Index]);
		}
	}

	/* == A typed custom speed, and its refusal path ========================== */
	{
		const TSharedPtr<SFlowVizNumericEntry> SpeedBox = Bar->GetCustomSpeedBox();
		if (!TestTrue(TEXT("the bar built a custom speed box"), SpeedBox.IsValid()))
		{
			return false;
		}

		// 3.0 is deliberately NOT a preset, so this exercises SetCustomSpeed
		// rather than the preset path, and the preset index must report none.
		SpeedBox->SimulateCommit(FText::FromString(TEXT("3.0")));
		TestEqual(TEXT("committing a custom speed applies it"),
			Workspace.Timeline.GetSpeed(), 3.0);
		TestEqual(TEXT("and no preset button claims it (rule 15: a lit preset for a hand-typed "
					   "speed claims a state the control is not in)"),
			Workspace.Timeline.GetSpeedPresetIndex(), INDEX_NONE);

		// ZERO IS REFUSED, keeping the prior speed: a paused player and a playing
		// one at speed zero look identical on screen and differ in every
		// diagnostic.
		SpeedBox->SimulateCommit(FText::FromString(TEXT("0")));
		TestEqual(TEXT("a zero speed is refused and the previous speed kept"),
			Workspace.Timeline.GetSpeed(), 3.0);

		SpeedBox->SimulateCommit(FText::FromString(TEXT("abc")));
		TestEqual(TEXT("unparseable text is ignored, not coerced"),
			Workspace.Timeline.GetSpeed(), 3.0);
	}

	/* == Interpolation toggle ================================================ */
	{
		const TSharedPtr<SButton> InterpButton = Bar->GetInterpolationButton();
		if (!TestTrue(TEXT("the bar built an interpolation toggle"), InterpButton.IsValid()))
		{
			return false;
		}

		TestTrue(TEXT("precondition: interpolation is on (the player's default)"),
			Workspace.Timeline.IsInterpolationEnabled());

		InterpButton->SimulateClick();
		TestFalse(TEXT("clicking the toggle turns interpolation off -- stored frames only, "
					   "which is what a voxel-accurate review wants"),
			Workspace.Timeline.IsInterpolationEnabled());

		InterpButton->SimulateClick();
		TestTrue(TEXT("clicking again turns it back on"),
			Workspace.Timeline.IsInterpolationEnabled());
	}

	return true;
}

/**
 * Unbound: the options are disabled and their handlers null-safe, matching the
 * transport buttons' own unbound contract.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransportOptionsUnboundTest,
	"FlowViz.UI.TransportBar.OptionsUnbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransportOptionsUnboundTest::RunTest(const FString& Parameters)
{
	const TSharedRef<SFlowVizTransportBar> Bar = SNew(SFlowVizTransportBar);

	const TSharedPtr<SButton> LoopButton = Bar->GetLoopModeButton();
	if (!TestTrue(TEXT("an unbound bar still builds the loop button"), LoopButton.IsValid()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Bar);

	TestFalse(TEXT("with no view model the loop button is disabled (rule 15)"),
		LoopButton->IsEnabled());

	// SimulateClick bypasses the enabled check: proves the handlers are
	// null-safe, not merely unreachable.
	LoopButton->SimulateClick();
	if (const TSharedPtr<SButton> ModeButton = Bar->GetPlaybackModeButton())
	{
		ModeButton->SimulateClick();
	}
	if (const TSharedPtr<SButton> Preset = Bar->GetSpeedPresetButton(0))
	{
		Preset->SimulateClick();
	}
	if (const TSharedPtr<SButton> Interp = Bar->GetInterpolationButton())
	{
		Interp->SimulateClick();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
