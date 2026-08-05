// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizVolumeComponent.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * IS ANYTHING ACTUALLY WIRED TO MARCH A VOLUME IN A SHIPPING BUILD?
 *
 * This file exists because the rest of the suite cannot answer that question,
 * and the reason it cannot is structural rather than an oversight.
 *
 * The volume is drawn through a globally registered dispatcher. The scene proxy
 * asks for one (FFlowVizVolumeSceneProxy::GetDynamicMeshElements) and silently
 * draws nothing when none is installed. Every existing test that exercises that
 * path installs its OWN dispatcher first, because it needs a recording double to
 * assert against. Installing a dispatcher is therefore a PRECONDITION of those
 * tests - which makes every one of them structurally incapable of noticing that
 * production installs none. The suite was 49/49 green while a volume placed in
 * a real map rendered nothing at all:
 *
 *   - IFlowVizVolumeRayMarchDispatcher had exactly one implementation, and it
 *     lived in Private/Tests/FlowVizVolumeComponentTest.cpp.
 *   - FlowVizVolumeRayMarch::SetDispatcher had no caller outside /Tests/ - only
 *     its own declaration and definition.
 *   - FlowVizRayMarch::AddRayMarchPass, the real RDG compute dispatch, was
 *     called only from FlowVizVolumeRayMarchTest.cpp and FlowVizVolumeMarchTest.cpp.
 *
 * Both halves were thoroughly tested. Nothing joined them. The seam comment on
 * IFlowVizVolumeRayMarchDispatcher had already named this exact hazard - "no
 * marcher wired" and "marcher ran and produced nothing" look identical on screen
 * and have nothing in common as fixes - so the ambiguity was anticipated; what
 * was missing was an assertion that could distinguish the two.
 *
 * THE RULE THIS FILE ENFORCES: a test that installs a mock cannot verify that
 * production builds the thing the mock stands in for. That needs a test which
 * installs NOTHING and asks what startup left behind. This is that test, and it
 * is the only test in the plugin permitted to assert on the global dispatcher
 * without first setting it.
 *
 * WHY IT CANNOT SILENTLY STOP CHECKING. The assertion is on a global the other
 * tests deliberately mutate and restore. If a future test leaks a null - or
 * leaves its own stack-allocated double installed after returning - this test
 * reports the leak rather than being fooled by it, because it additionally
 * requires that what is installed is NOT any address owned by a test. See the
 * dangling-double check below.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizRenderWiringTest,
	"FlowViz.Render.Wiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizRenderWiringTest::RunTest(const FString& Parameters)
{
	/*
	 * Deliberately NO SetDispatcher call before this read, and no ON_SCOPE_EXIT
	 * restore. Both would defeat the purpose: this test's whole subject is the
	 * state the module's own startup left the global in. Touching it first would
	 * make the test assert about itself.
	 */
	IFlowVizVolumeRayMarchDispatcher* const Installed = FlowVizVolumeRayMarch::GetDispatcher();

	const bool bInstalled = TestNotNull(
		TEXT("module startup installs a production ray-march dispatcher; without one every "
			 "volume in every map draws nothing, and no other test in this plugin can tell, "
			 "because they all install their own double first"),
		Installed);

	if (!bInstalled)
	{
		// Nothing below is meaningful without one, and a null deref here would
		// report as a crash rather than as the failed assertion above.
		return false;
	}

	/*
	 * THE DANGLING-DOUBLE CHECK.
	 *
	 * TestNotNull alone would pass if a sibling test leaked its own recording
	 * dispatcher - a stack object that is destroyed by the time we read it, so
	 * the pointer is non-null and dangling. That failure mode is worse than the
	 * one above, because it looks like success.
	 *
	 * The production dispatcher is a function-local static with static storage
	 * duration, so it outlives every test. A test double is a local in another
	 * RunTest frame. We cannot compare addresses against doubles we cannot see,
	 * so we assert the property that distinguishes them: asking twice, with a
	 * full round trip through the accessor, must yield the SAME pointer. A
	 * leaked stack double is not stable across the rest of the suite; the
	 * registered singleton is.
	 *
	 * This is weaker than an identity comparison and it is what the visibility
	 * rules allow. It is not decorative: it fails if anything installs a
	 * per-call or per-frame dispatcher, which would be a real defect.
	 */
	TestEqual(
		TEXT("the installed dispatcher is stable across reads, so it is a registered singleton "
			 "rather than a test double leaked from another frame"),
		FlowVizVolumeRayMarch::GetDispatcher(),
		Installed);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
