// Copyright FlowViz contributors. All Rights Reserved.

#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizConsoleCommands.h"
#include "UI/FlowVizDiagnostics.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/FlowVizWorkspaceRegistry.h"
#include "UI/SFlowVizDiagnosticsOverlay.h"
#include "UI/SFlowVizWorkspace.h"
#include "Widgets/Text/STextBlock.h"

// The bound-attribute assertions read what a live frame would read, not the
// value the widget's constructor left in the cache. See the header.
#include "FlowVizSlateAttributePump.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local. That collision already shipped once in this repo (#37).
 */
namespace FlowVizDiagnosticsOverlayTest
{
	/** The committed low-resolution sample, beside Plugins/ rather than inside the plugin. */
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

	/**
	 * A SCALAR FIELD, NAMED. OpenCase's "first non-mask field" rule lands on `U`,
	 * a three-component vector; leaving it to default would exercise a different
	 * path from the one this file reads about.
	 */
	const FName SampleField(TEXT("speed"));

	/** Restores the engine timing globals this file drives. */
	struct FEngineTimingScope
	{
		double SavedDelta;

		FEngineTimingScope()
			: SavedDelta(FApp::GetDeltaTime())
		{
		}

		~FEngineTimingScope()
		{
			FApp::SetDeltaTime(SavedDelta);
		}
	};
}

/**
 * DOES THE OVERLAY RE-READ, OR DID IT SNAPSHOT ITSELF AT CONSTRUCTION?
 *
 * That is the entire subject of this file, and it is the only defect this
 * widget can have that its own numbers are not already tested for.
 * FlowViz.UI.Diagnostics covers Collect and Format directly -- twenty-one rows,
 * each driven to a known value. What no test of those functions can observe is a
 * widget that calls them ONCE, in Construct, and displays the result forever.
 *
 * WHY THAT BUG IS INVISIBLE TO EVERY OTHER CHECK. It produces correct text. The
 * rows are right, the formatting is right, the numbers are the ones Collect
 * genuinely returned -- for the workspace as it stood before a case was loaded.
 * A screenshot shows "FPS 0.0" and "(no case open)", which is also exactly what
 * a CORRECT overlay shows on an idle session. So the review reads fine, the
 * screenshot reads fine, and the panel is dead.
 *
 * The assertion therefore has the shape rule 7 asks for: drive the model into a
 * state whose formatted output DIFFERS, read the widget again, and require the
 * two reads to disagree. A frozen widget returns the same string twice and
 * fails. Both halves are asserted -- a first read that already contained the
 * post-change text would make the inequality pass for the wrong reason, so the
 * before-state is pinned too.
 *
 * THE TEXT IS READ THROUGH THE BOUND STextBlock, not through GetDisplayText
 * alone. GetDisplayText is a plain const member; calling it recomputes by
 * definition, so an assertion that only called it would pass against a widget
 * that never bound the attribute at all -- the exact un-falsifiable shape this
 * repo keeps finding. Reading the block's text is reading what a user sees.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizDiagnosticsOverlayLiveTest,
	"FlowViz.UI.DiagnosticsOverlay.Live",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizDiagnosticsOverlayLiveTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizDiagnosticsOverlayTest;

	FEngineTimingScope TimingScope;

	FFlowVizWorkspaceModel Model;

	const TSharedRef<SFlowVizDiagnosticsOverlay> Overlay =
		SNew(SFlowVizDiagnosticsOverlay).Model(&Model);

	const TSharedPtr<STextBlock> Block = Overlay->GetTextBlock();
	if (!TestTrue(TEXT("CONTROL: the overlay built a text block to display through"),
			Block.IsValid()))
	{
		return false;
	}

	/*
	 * WHAT THE USER WOULD SEE, evaluated the way a frame evaluates it. Without
	 * the pump, STextBlock::GetText returns the value cached at construction --
	 * see FlowVizSlateAttributePump.h. That cache is what makes an unbound
	 * widget indistinguishable from a bound one, which is precisely the
	 * distinction this test exists to draw.
	 */
	auto DisplayedText = [&Overlay, &Block]() -> FString
	{
		FlowVizSlateAttributePump::Pump(Overlay);
		return Block->GetText().ToString();
	};

	/* == Before: no case, and a pinned frame rate ============================ */

	// 30 fps exactly, so the reciprocal is exact in binary and the row below is
	// a specific string rather than a rounded one.
	FApp::SetDeltaTime(1.0 / 30.0);

	const FString Before = DisplayedText();

	/*
	 * THE BEFORE-STATE IS PINNED, not merely captured. If this first read
	 * already said "60.0" or already described an open case, the inequality at
	 * the end would pass for a reason unrelated to the widget re-reading
	 * anything -- and a test whose pass condition can be met by accident is not
	 * a check.
	 */
	if (!TestTrue(
			FString::Printf(TEXT("CONTROL: with no case open the overlay says so rather than "
								 "printing a screen of zeros. Displayed: '%s'"),
				*Before),
			Before.Contains(TEXT("(no case open)"))))
	{
		return false;
	}

	if (!TestTrue(
			FString::Printf(TEXT("CONTROL: the overlay reports the 30 fps this test pinned, so "
								 "the reads below start from a known frame rate. Displayed: '%s'"),
				*Before),
			Before.Contains(TEXT("FPS               30.0"))))
	{
		return false;
	}

	/* == The change: open a case and move the frame rate ===================== */

	const FString CaseDir = GetSampleCaseDir();
	if (!TestTrue(TEXT("CONTROL: the committed sample case opens"),
			Model.OpenCase(CaseDir, SampleField).IsOk()))
	{
		return false;
	}

	FApp::SetDeltaTime(1.0 / 60.0);

	const FString After = DisplayedText();

	/* == The assertion ======================================================= */

	/*
	 * THE WHOLE POINT. A Construct-time snapshot returns Before here, byte for
	 * byte, having observed neither the case nor the frame rate.
	 */
	TestNotEqual(
		TEXT("the overlay RE-READS: opening a case and changing the frame delta changes what is "
			 "on screen. A widget that called Collect once in Construct displays plausible, "
			 "correctly-formatted, permanently frozen numbers -- and an idle session's screenshot "
			 "of that bug is identical to a working overlay's"),
		After, Before);

	/*
	 * AND IT RE-READS BOTH SUBJECTS, asserted separately.
	 *
	 * TestNotEqual alone is satisfied by a widget that noticed the case and
	 * ignored the timing, or the reverse -- one changed row makes the whole
	 * string differ. These two rows come from different sources (the workspace
	 * model, and an engine global), so a widget that caches one and not the
	 * other is a real and separable defect.
	 */
	TestFalse(
		FString::Printf(TEXT("the case-open state is re-read: the '(no case open)' line is gone "
							 "now that a case IS open. Displayed: '%s'"),
			*After),
		After.Contains(TEXT("(no case open)")));

	TestTrue(
		FString::Printf(TEXT("the engine timing is re-read: the FPS row followed the delta from "
							 "30 to 60. Displayed: '%s'"),
			*After),
		After.Contains(TEXT("FPS               60.0")));

	/* == And it is the SAME formatter the console command prints ============= */

	/*
	 * NOT A REIMPLEMENTATION CHECK -- an equality check. If the overlay ever
	 * grows its own formatting, the two readouts of one session drift, and the
	 * drift looks like a measurement disagreeing with itself. Collected fresh
	 * here for the same model, so any difference is the widget's doing.
	 */
	TestEqual(
		TEXT("the overlay displays exactly what FlowVizDiagnostics::Format produces, so it and "
			 "FlowViz.ShowDiagnostics cannot disagree about the same session"),
		DisplayedText(),
		FlowVizDiagnostics::Format(FlowVizDiagnostics::Collect(Model, nullptr)));

	Model.CloseCase();
	return true;
}

/**
 * A NULL MODEL IS A LEGAL, INERT STATE, and it must not print zeros.
 *
 * The workspace constructs its children in an order this widget does not choose,
 * and every other panel in this plugin takes a null view model as ordinary. The
 * hazard specific to THIS widget is rule 10: a diagnostics readout whose model
 * is missing has no measurements at all, and "FPS 0.0 / 0 frames / 0% hit rate"
 * is not a neutral placeholder -- it is a set of measurement claims, all false,
 * and it looks exactly like a real session performing catastrophically.
 *
 * SEPARATE FROM THE LIVE TEST because that one asserts a null model never
 * reaches it. This one supplies one deliberately, which the Live test's
 * CONTROL assertions would refuse.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizDiagnosticsOverlayUnboundTest,
	"FlowViz.UI.DiagnosticsOverlay.Unbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizDiagnosticsOverlayUnboundTest::RunTest(const FString& Parameters)
{
	/*
	 * NO .Model() AT ALL, which is the case that matters: SLATE_ARGUMENT expands
	 * to a bare member with no initializer, so an omitted pointer holds
	 * indeterminate memory unless the SLATE_BEGIN_ARGS initializer list sets it.
	 * A widget missing that initializer passes every null check and crashes on
	 * the first dereference -- which is what took down
	 * FlowViz.UI.TransferFunctionPanel.Unbound before its header grew the line.
	 */
	const TSharedRef<SFlowVizDiagnosticsOverlay> Overlay = SNew(SFlowVizDiagnosticsOverlay);

	const TSharedPtr<STextBlock> Block = Overlay->GetTextBlock();
	if (!TestTrue(TEXT("CONTROL: an unbound overlay still builds its text block"), Block.IsValid()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Overlay);
	const FString Displayed = Block->GetText().ToString();

	TestTrue(
		FString::Printf(TEXT("an unbound overlay SAYS it has no workspace. Displayed: '%s'"),
			*Displayed),
		Displayed.Contains(TEXT("(no workspace)")));

	/*
	 * AND PRINTS NO MEASUREMENT. Rule 10: a zero here is a claim, not a blank.
	 * Asserted on the FPS row specifically because it is the row a reader looks
	 * at first and the one whose zero is most readily believed.
	 */
	TestFalse(
		FString::Printf(TEXT("an unbound overlay prints no frame rate at all, rather than 0.0 -- "
							 "which would read as a session running at zero fps. Displayed: '%s'"),
			*Displayed),
		Displayed.Contains(TEXT("FPS ")));

	/*
	 * HIT-TEST INVISIBLE. The overlay sits on top of the viewport region; at
	 * Slate's default visibility it accepts hit tests and swallows every click
	 * and drag meant for the volume underneath. The symptom is "the viewport
	 * stopped responding to the mouse", which reads as a broken viewport rather
	 * than as a text panel in front of it -- so nothing would look at this
	 * widget.
	 */
	TestEqual(
		TEXT("the overlay never takes a hit test, so clicks and drags reach the viewport it "
			 "covers rather than being swallowed by a text panel"),
		Overlay->GetVisibility(), EVisibility::HitTestInvisible);

	return true;
}

/**
 * IS THE OVERLAY REACHABLE FROM THE RUNNING APPLICATION?
 *
 * The two tests above both `SNew` the widget, which is a precondition of
 * asserting anything about it -- so both stay green if nothing in the shipping
 * application ever constructs one. The overlay would be complete, tested, and
 * unreachable: the same "two finished halves with nothing between them" the
 * render layer shipped, where 49 tests were green while a volume in a real map
 * drew nothing because every one of them installed its own dispatcher.
 *
 * So this test constructs a WORKSPACE -- the object the tab spawner builds and
 * hands to Slate -- and asks whether the workspace built an overlay. It never
 * constructs an overlay itself.
 *
 * AND IT ASSERTS THE TOGGLE, because "built" is not "reachable". A widget that
 * is constructed but permanently collapsed has exactly the same symptom as one
 * that was never constructed: FlowViz.ShowDiagnostics prints to the log, the
 * overlay sits in the tree where a HasOverlay check finds it, and the user sees
 * nothing on screen no matter what they type. Both states are asserted -- hidden
 * before, shown after -- because an overlay that is ALWAYS visible would satisfy
 * a shown-after check alone while covering the viewport permanently.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizDiagnosticsOverlayWiringTest,
	"FlowViz.UI.DiagnosticsOverlay.Wiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizDiagnosticsOverlayWiringTest::RunTest(const FString& Parameters)
{
	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);

	const TSharedPtr<SFlowVizDiagnosticsOverlay> Overlay = Workspace->GetDiagnosticsOverlay();

	if (!TestTrue(
			TEXT("the workspace BUILDS a diagnostics overlay; without this the widget is "
				 "complete, tested and unreachable from the running application, and no other "
				 "overlay test can tell because they both SNew their own"),
			Overlay.IsValid()))
	{
		return false;
	}

	/*
	 * THE SAME MODEL, NOT A SECOND ONE. An overlay handed its own model would
	 * display a workspace the user has never seen -- numbers that are internally
	 * consistent and about nothing. Driving the workspace's model and reading the
	 * overlay is the only way to tell the two apart.
	 */
	const FString CaseDir = FlowVizDiagnosticsOverlayTest::GetSampleCaseDir();
	if (!TestTrue(TEXT("CONTROL: the committed sample opens in the WORKSPACE's model"),
			Workspace->GetModel()
				.OpenCase(CaseDir, FlowVizDiagnosticsOverlayTest::SampleField)
				.IsOk()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Overlay);
	const FString Displayed = Overlay->GetTextBlock()->GetText().ToString();

	/*
	 * ASSERTED POSITIVELY, on a string only THIS case produces. The obvious form
	 * -- TestFalse on "(no case open)" -- is satisfied by an overlay wired to no
	 * model at all, because that one prints "(no workspace)" and the substring is
	 * absent for the wrong reason. Naming the field the workspace opened means
	 * only a model this test drove can make it pass.
	 */
	TestTrue(
		FString::Printf(
			TEXT("the overlay reads the WORKSPACE's model, not one of its own and not none: the "
				 "case opened on the workspace names its field here. Displayed: '%s'"),
			*Displayed),
		Displayed.Contains(FlowVizDiagnosticsOverlayTest::SampleField.ToString()));

	/* == And the volume reaches it, so the component rows are not permanent zeros == */

	/*
	 * THE SECOND HALF OF THE SAME WIRING. Most diagnostics rows come from the
	 * model, which the overlay was handed at construction; two of them --
	 * resolution and ray step -- live on the VOLUME COMPONENT, and the overlay
	 * only learns about that through SFlowVizWorkspace::SetVolume pushing it
	 * across. Without that push the overlay is correct about everything it can
	 * see and silently zero about the rest.
	 *
	 * AND ZERO IS A CLAIM HERE, not a blank: FlowVizDiagnostics documents a zero
	 * resolution as "no upload has happened", so an overlay that never learned
	 * about the volume reports a true-sounding fact about a volume that is
	 * uploading fine. Nothing else in this suite can catch it -- Live and Unbound
	 * both build their own overlay and neither has a workspace to push from.
	 *
	 * READ ON THE RAY STEP ROW rather than the resolution row. Resolution comes
	 * from the uploaded texture layout, which is zero in a headless session even
	 * when the push works, so an assertion there could not distinguish a missing
	 * push from an absent GPU upload. The ray step comes from the component's
	 * render settings, which are populated by its constructor: 0.500 voxels /
	 * 2048 steps with a volume, 0.000 / 0 without one.
	 */
	UCFDVizVolumeComponent* Volume = NewObject<UCFDVizVolumeComponent>();
	if (!TestNotNull(TEXT("CONTROL: a bare volume component was built"), Volume))
	{
		Workspace->GetModel().CloseCase();
		return false;
	}
	Volume->AddToRoot();
	ON_SCOPE_EXIT { Volume->RemoveFromRoot(); };

	const FString BeforeVolume = [&Overlay]()
	{
		FlowVizSlateAttributePump::Pump(Overlay);
		return Overlay->GetTextBlock()->GetText().ToString();
	}();

	if (!TestTrue(
			FString::Printf(
				TEXT("CONTROL: with no volume bound the ray step row reads zero, so the assertion "
					 "below can tell a pushed volume from an unpushed one. Displayed: '%s'"),
				*BeforeVolume),
			BeforeVolume.Contains(TEXT("Ray step          0.000 voxels, max 0 steps"))))
	{
		Workspace->GetModel().CloseCase();
		return false;
	}

	Workspace->SetVolume(Volume);

	FlowVizSlateAttributePump::Pump(Overlay);
	const FString AfterVolume = Overlay->GetTextBlock()->GetText().ToString();

	TestFalse(
		FString::Printf(
			TEXT("binding a volume to the WORKSPACE reaches the overlay, so the rows that live on "
				 "the component stop reading zero -- and a zero resolution is documented as 'no "
				 "upload has happened', which is a false claim about a volume that is fine. "
				 "Displayed: '%s'"),
			*AfterVolume),
		AfterVolume.Contains(TEXT("Ray step          0.000 voxels, max 0 steps")));

	Workspace->SetVolume(nullptr);

	/* == The toggle: hidden by default, shown on command ===================== */

	/*
	 * HIDDEN FIRST, and asserted before anything is typed. An overlay that ships
	 * visible covers the picture the user came to look at, and it would satisfy
	 * the shown-after assertion below without any toggle existing at all.
	 */
	if (!TestFalse(
			TEXT("the overlay is HIDDEN until asked for; one that ships visible covers the "
				 "viewport it describes, and would pass the shown-after check below with no "
				 "toggle implemented at all"),
			Workspace->IsDiagnosticsOverlayShown()))
	{
		Workspace->GetModel().CloseCase();
		return false;
	}

	/*
	 * AND HIDDEN IN THE TREE, not merely in the bool -- a mutation survivor
	 * taught this one. The flag above is initialised false and SetVisibility has
	 * not run yet, so it stays false no matter what visibility the CONTAINER was
	 * constructed with: a workspace that builds the slot visible ships an overlay
	 * covering the viewport from the moment the tab opens, and every assertion in
	 * this test still passes. The tree reads below happen only AFTER the first
	 * toggle, which is exactly the state that mutant left untouched.
	 *
	 * FALSIFIABLE BY CONSTRUCTION: the same read, on the same widget, returns
	 * visible three lines further down.
	 */
	FlowVizSlateAttributePump::Pump(Workspace);

	TestFalse(
		TEXT("and hidden IN THE BUILT TREE, not just in the flag: the shown state is false by "
			 "initialisation, so a container constructed visible ships an overlay over the "
			 "viewport with the bool still reading hidden"),
		Overlay->GetParentWidget().IsValid()
			&& Overlay->GetParentWidget()->GetVisibility().IsVisible());

	Workspace->SetDiagnosticsOverlayShown(true);

	TestTrue(
		TEXT("asking for the overlay shows it, so FlowViz.ShowDiagnostics puts something on "
			 "screen rather than only in the log"),
		Workspace->IsDiagnosticsOverlayShown());

	/*
	 * AND THE SLOT FOLLOWS. IsDiagnosticsOverlayShown could report a bool nothing
	 * acts on -- which is the shape #65 had, where Play set a flag no clock read.
	 * Read through the widget tree, pumped, so this is the visibility a frame
	 * would use rather than the one the constructor cached.
	 */
	FlowVizSlateAttributePump::Pump(Workspace);

	TestTrue(
		TEXT("the overlay's slot is actually visible in the built tree, so the shown state is "
			 "not a bool nothing acts on"),
		Overlay->GetParentWidget().IsValid()
			&& Overlay->GetParentWidget()->GetVisibility().IsVisible());

	Workspace->SetDiagnosticsOverlayShown(false);
	FlowVizSlateAttributePump::Pump(Workspace);

	TestFalse(
		TEXT("and it hides again, so the toggle is a toggle rather than a one-way switch"),
		Overlay->GetParentWidget().IsValid()
			&& Overlay->GetParentWidget()->GetVisibility().IsVisible());

	/* == And the command a user actually types drives it ====================== */

	/*
	 * THE NAME SAYS "SHOW". plan.md section 17 lists FlowViz.ShowDiagnostics
	 * among the console commands and asks for an on-screen overlay in the same
	 * breath; a command called Show that only writes to the output log leaves
	 * the overlay with no way to be turned on at all -- built, wired, and
	 * unreachable by exactly one step further out than the gap this test's
	 * first half closes.
	 *
	 * Executed through the console manager rather than by calling the exec
	 * function, so this asserts about the command a user can type.
	 */
	IConsoleObject* Object =
		IConsoleManager::Get().FindConsoleObject(FlowVizConsoleCommands::ShowDiagnosticsName);
	IConsoleCommand* Command = Object != nullptr ? Object->AsCommand() : nullptr;

	if (!TestNotNull(TEXT("CONTROL: FlowViz.ShowDiagnostics is registered, so there is a command "
						  "to execute"),
			Command))
	{
		Workspace->GetModel().CloseCase();
		return false;
	}

	/*
	 * CONTROL: the command acts on THIS workspace. It resolves its target
	 * through the registry, and a stale entry from an earlier test's workspace
	 * would make the assertion below report on an object this test never
	 * touched -- passing or failing for reasons that have nothing to do with
	 * the wiring.
	 */
	if (!TestEqual(TEXT("CONTROL: the registry resolves to the workspace this test built"),
			FlowVizWorkspaceRegistry::GetActiveWorkspace(), TSharedPtr<SFlowVizWorkspace>(Workspace)))
	{
		Workspace->GetModel().CloseCase();
		return false;
	}

	FStringOutputDevice Capture;
	Command->Execute(TArray<FString>(), nullptr, Capture);

	TestTrue(
		TEXT("FlowViz.ShowDiagnostics puts the overlay ON SCREEN, not only in the log -- a "
			 "command named Show that only writes text leaves the overlay with no way to be "
			 "turned on"),
		Workspace->IsDiagnosticsOverlayShown());

	/*
	 * AND IT TOGGLES. A show-only command means the only way to dismiss the
	 * overlay is to close the tab, which is why every engine stat command is a
	 * toggle. Asserted as a second execute rather than assumed from the first.
	 */
	Command->Execute(TArray<FString>(), nullptr, Capture);

	TestFalse(
		TEXT("typing it again dismisses the overlay, so the only way to get rid of it is not "
			 "closing the workspace"),
		Workspace->IsDiagnosticsOverlayShown());

	Workspace->GetModel().CloseCase();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
