// Copyright FlowViz contributors. All Rights Reserved.

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "UI/SFlowVizPipelinePanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizPipelinePanelTest
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

	struct FAnnounceCounter
	{
		int32 Count = 0;
		FSimpleDelegate MakeDelegate()
		{
			return FSimpleDelegate::CreateLambda([this]() { ++Count; });
		}
	};

	struct FOpenRecorder
	{
		TArray<FString> Paths;
		FFlowVizOpenCaseRequested MakeDelegate()
		{
			return FFlowVizOpenCaseRequested::CreateLambda(
				[this](const FString& Path) { Paths.Add(Path); });
		}
	};
}

/**
 * The pipeline panel (#83 / Milestone F): the case's fields as rows, click
 * to display. It is the plan's "pipeline tree" scoped to what the runtime
 * actually has -- one case, its volume fields -- rather than a tree control
 * with one branch pretending otherwise.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizPipelinePanelTest,
	"FlowViz.UI.PipelinePanel.FieldsDriveTheModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizPipelinePanelTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizPipelinePanelTest;

	FFlowVizWorkspaceModel Model;
	const FCFDVizResult OpenResult = Model.OpenCase(GetSampleCaseDir());
	if (!OpenResult.IsOk())
	{
		AddError(FString::Printf(TEXT("could not open the sample case: %s"),
			*OpenResult.ToString()));
		return false;
	}

	FAnnounceCounter Counter;
	const TSharedRef<SFlowVizPipelinePanel> Panel = SNew(SFlowVizPipelinePanel)
		.Model(&Model)
		.OnFieldChanged(Counter.MakeDelegate());
	Panel->RefreshFields();

	/* == One row per volume field, mask excluded ============================= */
	{
		const TArray<FName> Fields = Model.GetVolumeFieldIds();
		TestTrue(TEXT("CONTROL: the sample offers several fields"), Fields.Num() >= 5);
		TestEqual(TEXT("the panel built one row per field"),
			Panel->GetFieldRowCount(), Fields.Num());
		TestFalse(TEXT("the mask field has no row -- it renders as a solid block and the "
					   "field enumeration already excludes it"),
			Fields.Contains(FName(TEXT("validMask"))));
	}

	/* == Clicking a row switches the DISPLAYED field ========================= */
	{
		const TArray<FName> Fields = Model.GetVolumeFieldIds();
		// The model opens on the first field; pick a DIFFERENT one so the click
		// is a change.
		const FName Target(TEXT("pressure"));
		TestTrue(TEXT("CONTROL: pressure is offered"), Fields.Contains(Target));
		const int32 TargetRow = Fields.IndexOfByKey(Target);

		const TSharedPtr<SButton> Row = Panel->GetFieldButton(TargetRow);
		if (!TestTrue(TEXT("the pressure row has a button"), Row.IsValid()))
		{
			return false;
		}

		const int32 Before = Counter.Count;
		Row->SimulateClick();

		TestEqual(TEXT("clicking a field row displays that field -- SetField's first "
					   "widget caller; until now only session load switched fields"),
			Model.GetCase() != nullptr ? Model.TransferFunction.GetFieldId() : NAME_None,
			Target);
		TestEqual(TEXT("and announces, so the workspace can re-push"), Counter.Count, Before + 1);

		// Re-clicking the selected field is a no-op: SetField re-opens the case,
		// which resets playback -- an expensive reset nobody asked for.
		Row->SimulateClick();
		TestEqual(TEXT("re-clicking the displayed field does not announce"),
			Counter.Count, Before + 1);
	}

	/* == The open-case row announces, and only with a real path ============= */
	{
		FOpenRecorder Recorder;
		const TSharedRef<SFlowVizPipelinePanel> OpenPanel =
			SNew(SFlowVizPipelinePanel)
				.OnOpenCaseRequested(Recorder.MakeDelegate());

		// NO MODEL on purpose: opening is how a case ARRIVES, so the row must
		// work on the empty workspace the packaged app starts with.
		if (!TestTrue(TEXT("the open row exists without a case"),
				OpenPanel->GetOpenPathBox().IsValid() && OpenPanel->GetOpenButton().IsValid()))
		{
			return false;
		}

		OpenPanel->GetOpenButton()->SimulateClick();
		TestEqual(TEXT("CONTROL: an empty path does not announce -- a stray Enter is "
					   "not a request to open nothing"),
			Recorder.Paths.Num(), 0);

		OpenPanel->GetOpenPathBox()->SetText(
			FText::FromString(TEXT("  /some/case.cfdviz  ")));
		OpenPanel->GetOpenButton()->SimulateClick();
		if (!TestEqual(TEXT("a committed path announces once"), Recorder.Paths.Num(), 1))
		{
			return false;
		}
		TestEqual(TEXT("trimmed -- pasted paths arrive with whitespace"),
			Recorder.Paths[0], FString(TEXT("/some/case.cfdviz")));
	}

	/* == The mode toggles drive the model (renderer overhaul P6) ============ */
	{
		FAnnounceCounter ToggleCounter;
		const TSharedRef<SFlowVizPipelinePanel> TogglePanel =
			SNew(SFlowVizPipelinePanel)
				.Model(&Model)
				.OnFieldChanged(ToggleCounter.MakeDelegate());

		// The defaults ARE the P6 picture: surfaces on, volume off.
		TestTrue(TEXT("obstacle defaults visible"), Model.IsObstacleVisible());
		TestTrue(TEXT("iso defaults enabled"), Model.IsIsoSurfaceEnabled());
		TestTrue(TEXT("streamlines default enabled"), Model.AreStreamlinesEnabled());
		TestFalse(TEXT("the VOLUME defaults OFF -- the fog is opt-in now"),
			Model.IsVolumeVisible());

		const bool ExpectedActive[SFlowVizPipelinePanel::ModeToggleCount] = {
			Model.IsObstacleVisible(),
			Model.Slice.IsVisible(),
			Model.IsIsoSurfaceEnabled(),
			Model.AreStreamlinesEnabled(),
			Model.AreParticlesEnabled(),
			Model.IsVolumeVisible()
		};
		for (int32 Index = 0; Index < SFlowVizPipelinePanel::ModeToggleCount; ++Index)
		{
			const TSharedPtr<SButton> Toggle = TogglePanel->GetModeToggleButton(Index);
			TestTrue(FString::Printf(TEXT("toggle %d exists"), Index), Toggle.IsValid());
			if (Toggle.IsValid())
			{
				TestTrue(FString::Printf(TEXT("bound toggle %d is enabled"), Index),
					Toggle->IsEnabled());
				const FSlateBrush* ExpectedBrush = ExpectedActive[Index]
					? &FlowVizWorkspaceStyle::GetSelectedToolButtonStyle().Normal
					: &FlowVizWorkspaceStyle::GetToolButtonStyle().Normal;
				TestTrue(FString::Printf(TEXT("toggle %d visibly reflects its model state"), Index),
					Toggle->GetBorderImage() == ExpectedBrush);
			}
		}

		// Volume toggle (index 5 since Particles joined at 4): click flips the
		// model and announces once.
		const int32 Before = ToggleCounter.Count;
		TogglePanel->GetModeToggleButton(5)->SimulateClick();
		TestTrue(TEXT("clicking Volume turns the fog on"), Model.IsVolumeVisible());
		TestTrue(TEXT("the active Volume toggle visibly selects"),
			TogglePanel->GetModeToggleButton(5)->GetBorderImage()
				== &FlowVizWorkspaceStyle::GetSelectedToolButtonStyle().Normal);
		TestEqual(TEXT("and announces once, so the workspace applies visibility"),
			ToggleCounter.Count, Before + 1);
		TogglePanel->GetModeToggleButton(5)->SimulateClick();
		TestFalse(TEXT("clicking again turns it back off"), Model.IsVolumeVisible());
		TestTrue(TEXT("the inactive Volume toggle visibly deselects"),
			TogglePanel->GetModeToggleButton(5)->GetBorderImage()
				== &FlowVizWorkspaceStyle::GetToolButtonStyle().Normal);

		// Particles toggle: off by default (the CPU path is opt-in until the
		// Niagara upgrade), click arms the population.
		TestFalse(TEXT("particles default off"), Model.AreParticlesEnabled());
		TogglePanel->GetModeToggleButton(4)->SimulateClick();
		TestTrue(TEXT("clicking Particles enables them"), Model.AreParticlesEnabled());
		TestTrue(TEXT("and seeds a population"), Model.GetParticles().Num() > 0);
		TogglePanel->GetModeToggleButton(4)->SimulateClick();
		TestFalse(TEXT("clicking again disables"), Model.AreParticlesEnabled());

		// Obstacle toggle drives its flag too.
		TogglePanel->GetModeToggleButton(0)->SimulateClick();
		TestFalse(TEXT("clicking Obstacle hides it"), Model.IsObstacleVisible());
		TogglePanel->GetModeToggleButton(0)->SimulateClick();
		TestTrue(TEXT("and shows it again"), Model.IsObstacleVisible());
	}

	/* == Unbound: builds, disabled, null-safe ================================ */
	{
		const TSharedRef<SFlowVizPipelinePanel> Unbound = SNew(SFlowVizPipelinePanel);
		Unbound->RefreshFields();
		TestEqual(TEXT("an unbound panel has no rows"), Unbound->GetFieldRowCount(), 0);
		for (int32 Index = 0; Index < SFlowVizPipelinePanel::ModeToggleCount; ++Index)
		{
			const TSharedPtr<SButton> Toggle = Unbound->GetModeToggleButton(Index);
			if (TestTrue(FString::Printf(TEXT("unbound toggle %d still exists"), Index),
					Toggle.IsValid()))
			{
				TestFalse(FString::Printf(TEXT("unbound toggle %d is visibly disabled"), Index),
					Toggle->IsEnabled());
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
