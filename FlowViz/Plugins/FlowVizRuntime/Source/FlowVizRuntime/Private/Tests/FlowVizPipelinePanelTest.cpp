// Copyright FlowViz contributors. All Rights Reserved.

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UI/FlowVizWorkspaceModel.h"
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

	/* == Unbound: builds, disabled, null-safe ================================ */
	{
		const TSharedRef<SFlowVizPipelinePanel> Unbound = SNew(SFlowVizPipelinePanel);
		Unbound->RefreshFields();
		TestEqual(TEXT("an unbound panel has no rows"), Unbound->GetFieldRowCount(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
