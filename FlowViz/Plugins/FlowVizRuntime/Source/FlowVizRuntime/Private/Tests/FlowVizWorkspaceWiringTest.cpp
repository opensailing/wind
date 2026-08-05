// Copyright FlowViz contributors. All Rights Reserved.

#include "Framework/Docking/TabManager.h"
#include "Misc/AutomationTest.h"
#include "UI/FlowVizWorkspaceTab.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * CAN A USER ACTUALLY OPEN THIS UI?
 *
 * Modelled directly on FlowVizRenderWiringTest, and for the same structural
 * reason. That file exists because 49 rendering tests were green while a volume
 * in a real map drew nothing: every one of them installed its own dispatcher as
 * a precondition, so not one of them could observe that production installed
 * none. The fix was a test that installs NOTHING and asks what startup left
 * behind.
 *
 * The UI has the identical shape of hole. FlowVizTransportBarTest and
 * FlowVizTransferFunctionPanelTest both `SNew` their panel, which is a
 * precondition of asserting anything about it - so both would stay green if
 * nothing in the shipping application ever constructed those widgets. The
 * panels would be complete, tested, and unreachable: the same "two finished
 * halves with nothing between them" that the render layer shipped.
 *
 * "Defined" is not "built". This test therefore constructs nothing itself. It
 * asks the global tab manager - the object a real user's menu click goes
 * through - whether module startup registered a spawner for the workspace, then
 * invokes that spawner and requires it to actually produce a populated tab. A
 * registration that returned an empty SDockTab would satisfy a mere
 * "HasTabSpawner" check while presenting the user with a blank panel, so the
 * content is asserted too.
 *
 * WHY THE SPAWNER IS EXERCISED RATHER THAN JUST COUNTED. Registering a spawner
 * whose callback crashes, or returns a tab with no widget, is a defect that
 * looks exactly like success from the outside. Calling it here is the only way
 * the assertion can distinguish "a menu entry exists" from "clicking the menu
 * entry gives you the workspace".
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceWiringTest,
	"FlowViz.UI.Workspace.Wiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceWiringTest::RunTest(const FString& Parameters)
{
	/*
	 * Deliberately NO registration call before this read. This test's whole
	 * subject is the state module startup left the global tab manager in;
	 * registering first would make it assert about itself - the precise mistake
	 * that let the render layer ship unwired.
	 */
	const TSharedRef<FGlobalTabmanager> TabManager = FGlobalTabmanager::Get();

	if (!TestTrue(
			TEXT("module startup registers a spawner for the FlowViz workspace tab; without one "
				 "every panel in this plugin is unreachable from the running application, and no "
				 "other UI test can tell, because they all SNew their own widget first"),
			TabManager->HasTabSpawner(FlowVizWorkspaceTab::TabId)))
	{
		return false;
	}

	// A REGISTRATION IS NOT A WORKING TAB. Invoke it the way a menu click does.
	const TSharedPtr<SDockTab> Tab = TabManager->TryInvokeTab(FTabId(FlowVizWorkspaceTab::TabId));

	if (!TestTrue(
			TEXT("invoking the workspace tab spawner produces a tab, so the registration is not "
				 "a menu entry that fails when clicked"),
			Tab.IsValid()))
	{
		return false;
	}

	/*
	 * AND THE TAB MUST CONTAIN SOMETHING. SDockTab's content defaults to
	 * SNullWidget, so a spawner that built its tab and forgot to fill it returns
	 * a perfectly valid, entirely blank tab - which passes every assertion above.
	 */
	const TSharedRef<SWidget> Content = Tab->GetContent();
	TestFalse(
		TEXT("the spawned workspace tab has real content rather than the empty default, so the "
			 "user gets the workspace rather than a blank panel"),
		Content == SNullWidget::NullWidget);

	// Leave the global no dirtier than we found it: this tab is a side effect of
	// the test, not something the user asked for.
	Tab->RequestCloseTab();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
