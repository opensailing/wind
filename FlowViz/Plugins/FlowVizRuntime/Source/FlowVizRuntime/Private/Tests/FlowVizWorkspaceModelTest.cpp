// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceModel.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UI/FlowVizSession.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The workspace model: does opening a case actually configure every panel's
 * view model (plan.md section 5F)?
 *
 * WHAT MAKES THIS WORTH TESTING SEPARATELY FROM THE VIEW MODELS. Each view
 * model is already tested in isolation, and each would pass its own suite while
 * being left completely unconfigured by the thing that opens a case. The defect
 * this file is aimed at is the one the plugin has already shipped once, in the
 * render layer: two well-tested halves and nothing joining them.
 *
 * Concretely, the failure mode is a domain size that reaches the clip view
 * model but not the slice one. Both view models default to a unit domain, so
 * the slice panel's slider still works - it is simply calibrated for a 1x1x1
 * box instead of the case's real extent. Nothing errors; the slice just lands
 * in the wrong place. That is invisible to FlowViz.UI.SliceViewModel.*, which
 * sets its own domain as a precondition.
 *
 * SO THE ASSERTIONS BELOW ARE DELIBERATELY ABOUT PROPAGATION, not about the
 * behaviour of any one view model. They check that after OpenCase:
 *
 *  1. The timeline is bound to THIS workspace's player - not left null, and not
 *     bound to some other player.
 *  2. The transfer function is bound to the field that was actually opened.
 *  3. Every view model that has a domain has the SAME domain, and it is the
 *     case's real physical size rather than the unit default.
 *  4. Closing unbinds, so a stale case cannot be driven.
 */

// NAMED namespace: FlowVizRuntime is a unity build, so anonymous namespaces from
// every .cpp merge and same-named helpers collide across test files.
namespace FlowVizWorkspaceModelTest
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

	/* ====================================================================== */
	/* The session fixture                                                     */
	/* ====================================================================== */

	/**
	 * NOT THE FIELD A DEFAULT OPEN PICKS.
	 *
	 * OpenCase with NAME_None takes the first non-mask field, which for this
	 * sample is `U`. A LoadSession that dropped the session's field id on the
	 * floor and opened the case with NAME_None would therefore land on `U` and
	 * every colouring assertion below would still be comparing against a real
	 * bound field - it would simply be the wrong one. `vorticity` is chosen
	 * because it is NOT that default and still has 3 components, which
	 * SetComponent(Y) requires.
	 */
	const FName FixtureFieldId(TEXT("vorticity"));

	/**
	 * A FIXED id, because a probe's id is what a chart series is keyed by and a
	 * session that restored the position under a NEW id would look identical in
	 * every positional assertion. Deliberately different from the fixture guid in
	 * FlowVizSessionTest.cpp: two files asserting the same magic number makes a
	 * failure ambiguous about which fixture produced it.
	 */
	const FGuid FixtureProbeId(0xA1B2C3D4, 0xE5F60718, 0x293A4B5C, 0x6D7E8F90);

	/** The id of the probe the poison arm adds. Must be GONE after a load. */
	const FGuid PoisonProbeId(0x0F1E2D3C, 0x4B5A6978, 0x8796A5B4, 0xC3D2E1F0);

	/**
	 * The field the poison switches to. `U` is the sample's FIRST non-mask field,
	 * which is what an OpenCase with NAME_None lands on - so this is also the
	 * value a LoadSession that opened the case but dropped the session's field id
	 * would leave behind. It has 3 components, which the poison's
	 * SetComponent(Z) needs.
	 */
	const FName PoisonFieldId(TEXT("U"));

	/*
	 * Every value below is inside the sample's 12 m x 4 m x 1 m domain and is
	 * different from what OpenCase leaves the corresponding view model at. The
	 * defaults are asserted explicitly in
	 * FlowViz.UI.WorkspaceModel.SessionDefaults - that is the control which makes
	 * the round-trip assertions capable of failing.
	 */
	const FVector FixtureProbePosition(7.5, 1.25, 0.625);
	const FVector FixtureLineStart(1.0, 2.0, 0.5);
	const FVector FixtureLineEnd(11.0, 2.0, 0.5);
	constexpr int32 FixtureLineSamples = 48;

	const FVector FixtureCropMin(1.0, 0.5, 0.1);
	const FVector FixtureCropMax(10.0, 3.5, 0.9);

	/** NOT the domain centre (6, 2, 0.5): that is exactly where OpenCase's CenterOnDomain leaves it. */
	const FVector FixtureSliceOrigin(3.0, 1.25, 0.375);
	/** NOT +Z, which is the view model's default normal. */
	const FVector FixtureSliceNormal(1.0, 0.0, 0.0);
	constexpr double FixtureSliceThickness = 0.25;
	constexpr int32 FixtureSlabSamples = 6;

	constexpr double FixturePhysicalTime = 0.35;
	constexpr float FixtureRangeMin = -12.5f;
	constexpr float FixtureRangeMax = 33.25f;
	constexpr int32 FixtureColorBands = 7;
	constexpr float FixtureOpacityMultiplier = 0.6f;
	constexpr float FixtureOpacityFirstPosition = 0.25f;
	constexpr float FixtureOpacityFirstValue = 0.1f;
	constexpr float FixtureOpacitySecondPosition = 0.75f;
	constexpr float FixtureOpacitySecondValue = 0.9f;

	/**
	 * Slack for the JSON text round trip only.
	 *
	 * THE THRESHOLD IS CHECKED, not picked to be small-looking. The tightest gap
	 * any assertion here has to resolve is 0.1 - between the fixture slice
	 * origin's Z (0.375) and nothing nearer than 0.5 - so 1e-6 has five orders of
	 * magnitude of headroom over the smallest thing it must tell apart. A
	 * tolerance able to blur a fixture value into a default would make every
	 * comparison below a tautology.
	 */
	constexpr double Tolerance = 1.0e-6;

	/*
	 * TWO POINTS, CHOSEN TO SEPARATE THE TWO PLANES' STATES.
	 *
	 * The fixture's planes are +Y at d = -1.5 (ENABLED) and +Z at d = -0.25
	 * (DISABLED). Reading the planes' fields back would pass on a restore that
	 * got bEnabled wrong, because the field would still be there; what the
	 * renderer actually consumes is which points survive.
	 *
	 *   KeptPoint    y = 2.0  >= 1.5, so the enabled plane keeps it
	 *                z = 0.1  <  0.25, so the DISABLED plane would cut it if the
	 *                         restore re-enabled it. Kept => plane 2 came back off.
	 *
	 *   ClippedPoint y = 1.0  <  1.5, so the enabled plane cuts it. Clipped =>
	 *                         plane 1 came back, and came back ENABLED.
	 *
	 * So the pair fails on a dropped plane AND on a plane whose enabled state was
	 * inverted or defaulted - in either direction.
	 */
	const FVector KeptPoint(6.0, 2.0, 0.1);
	const FVector ClippedPoint(6.0, 1.0, 0.5);

	/**
	 * Where a test's session file is written. Removed by the test that creates
	 * it.
	 *
	 * PER TEST, not one directory shared by all three. Each test deletes its
	 * directory on the way out whether it passed or failed, so a shared one
	 * would let a failing test remove the file a later test was about to read -
	 * and that later test would then fail with FileNotFound, which reads as a
	 * defect in the load path rather than as fallout.
	 */
	FString GetSessionDir(const TCHAR* TestName)
	{
		return FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("FlowVizWorkspaceSessionTest"), TestName);
	}

	/**
	 * Drive every one of the five view models off its post-OpenCase value.
	 *
	 * EVERY SETTER IS CHECKED. A refused setter leaves the model at the default
	 * the round trip is supposed to be distinguished from, which turns the whole
	 * test into "a default round-trips to itself" - green, and about nothing.
	 * Repo memory: degenerate-data-defeats-assertions.
	 *
	 * @return False when any part of the fixture was refused. The caller must
	 *         abandon the test rather than assert against a half-built fixture.
	 */
	bool ConfigureFixture(FFlowVizWorkspaceModel& Workspace, FAutomationTestBase& Test)
	{
		bool bOk = true;
		auto Require = [&Test, &bOk](const TCHAR* What, const FCFDVizResult& Result)
		{
			if (!Result.IsOk())
			{
				Test.AddError(FString::Printf(
					TEXT("CONTROL: the fixture's %s was refused (%s), so the model is still at "
						 "its default and every round-trip assertion downstream would be "
						 "comparing one default against another"),
					What, *Result.ToString()));
				bOk = false;
			}
		};

		/* --- Player ------------------------------------------------------- */

		FFlowVizPlaybackSettings Playback;
		Playback.Mode = EFlowVizPlaybackMode::RealTime;
		Playback.LoopMode = EFlowVizLoopMode::PingPong;
		Playback.Speed = 2.0;
		Playback.SequenceFrameRate = 12.0;
		Playback.OutputFrameRate = 60.0;
		Playback.bInterpolate = false;
		Playback.PreloadAhead = 3;
		Playback.PreloadBehind = 2;
		Require(TEXT("playback settings"), Workspace.Player.SetSettings(Playback));

		// AFTER SetSettings, because Player::Open seeks to frame 0 and a settings
		// change re-selects frames around wherever the playhead is.
		Workspace.Player.SeekToTime(FixturePhysicalTime);

		/* --- Transfer function --------------------------------------------- */

		// Inferno, not the manifest's own default for this field (coolwarm), so a
		// restore that fell back to the field's declared colormap is distinguishable
		// from one that read the session.
		Require(TEXT("colour map"), Workspace.TransferFunction.SetColorMap(ECFDVizColorMap::Inferno));
		Workspace.TransferFunction.SetReverseColorMap(true);
		Require(TEXT("colour band count"), Workspace.TransferFunction.SetColorBands(FixtureColorBands));
		Require(TEXT("component choice"),
			Workspace.TransferFunction.SetComponent(EFlowVizComponentChoice::Y));

		FFlowVizOpacityCurve Curve;
		Curve.Points.Add(FFlowVizOpacityPoint(FixtureOpacityFirstPosition, FixtureOpacityFirstValue));
		Curve.Points.Add(FFlowVizOpacityPoint(FixtureOpacitySecondPosition, FixtureOpacitySecondValue));
		Require(TEXT("opacity curve"), Workspace.TransferFunction.SetOpacityCurve(Curve));
		Require(TEXT("opacity multiplier"),
			Workspace.TransferFunction.SetOpacityMultiplier(FixtureOpacityMultiplier));

		// Last of the transfer-function calls for readability only: NOTHING above
		// re-derives the range. SetColorMap says so explicitly, and SetComponent
		// does call ApplyRangeFromSource but returns early once the source is
		// Manual. That early return is the whole reason the order does not matter,
		// which makes it worth CHECKING rather than asserting in prose - if it ever
		// stopped returning early, the typed numbers would be quietly replaced by
		// the field's global statistics and every range assertion downstream would
		// be reading the manifest while looking like it read the session.
		Require(TEXT("manual colour range"),
			Workspace.TransferFunction.SetManualRange(FixtureRangeMin, FixtureRangeMax));

		/* --- Clip ---------------------------------------------------------- */

		FFlowVizClipPlane EnabledPlane;
		EnabledPlane.Normal = FVector(0.0, 1.0, 0.0);
		EnabledPlane.Distance = -1.5;
		EnabledPlane.bEnabled = true;
		Require(TEXT("enabled clip plane"), Workspace.Clip.AddPlane(EnabledPlane));

		FFlowVizClipPlane DisabledPlane;
		DisabledPlane.Normal = FVector(0.0, 0.0, 1.0);
		DisabledPlane.Distance = -0.25;
		DisabledPlane.bEnabled = false;
		Require(TEXT("disabled clip plane"), Workspace.Clip.AddPlane(DisabledPlane));

		Require(TEXT("crop box"), Workspace.Clip.SetCropBox(FixtureCropMin, FixtureCropMax));

		/* --- Slice ---------------------------------------------------------- */

		Require(TEXT("slice normal"), Workspace.Slice.SetNormal(FixtureSliceNormal));
		Require(TEXT("slice origin"), Workspace.Slice.SetOrigin(FixtureSliceOrigin));
		// THICKNESS FIRST: SetSlabOp and SetSlabSamples are both refused on a
		// zero-thickness slice.
		Require(TEXT("slice thickness"), Workspace.Slice.SetThickness(FixtureSliceThickness));
		Require(TEXT("slab operation"), Workspace.Slice.SetSlabOp(EFlowVizSlabOp::Maximum));
		Require(TEXT("slab sample count"), Workspace.Slice.SetSlabSamples(FixtureSlabSamples));
		Workspace.Slice.SetVisible(false);

		/* --- Probes --------------------------------------------------------- */

		Require(TEXT("probe"),
			Workspace.Probes.RestoreProbe(FixtureProbeId, FixtureProbePosition, TEXT("fixture probe")));
		if (!Workspace.Probes.SetProbeVisible(FixtureProbeId, false))
		{
			Test.AddError(TEXT("CONTROL: the fixture probe could not be hidden, so its visibility "
							   "is still the default true and the round trip could not tell a "
							   "restored flag from an untouched one"));
			bOk = false;
		}
		Require(TEXT("line probe"), Workspace.Probes.SetLineProbe(FixtureLineStart, FixtureLineEnd));
		Require(TEXT("line sample count"), Workspace.Probes.SetLineSampleCount(FixtureLineSamples));

		/* --- The fixture is only useful if it took ------------------------- */

		if (!Test.TestTrue(
				TEXT("CONTROL: the fixture's crop is genuinely ACTIVE before the save. "
					 "IsCropActive returns false for a crop equal to the full domain AND for a "
					 "clip model with no domain at all, so an inactive fixture would make "
					 "bHasCropBox false in the capture and the restored-crop assertion would "
					 "pass against a session that stored nothing"),
				Workspace.Clip.IsCropActive()))
		{
			bOk = false;
		}
		if (!Test.TestEqual(
				TEXT("CONTROL: exactly one of the fixture's two clip planes is enabled, so a "
					 "restore that enabled both is distinguishable from a correct one"),
				Workspace.Clip.GetEnabledPlaneCount(), 1))
		{
			bOk = false;
		}
		if (!Test.TestEqual(
				TEXT("CONTROL: the fixture's slab really is a slab; SetThickness forces the op "
					 "back to None when the thickness collapses"),
				Workspace.Slice.GetSlabOp(), EFlowVizSlabOp::Maximum))
		{
			bOk = false;
		}
		if (!Test.TestEqual(
				TEXT("CONTROL: the TYPED colour range is what the model holds before the save, "
					 "not a range re-derived from the field. This is the check behind the "
					 "ordering comment above: SetComponent calls ApplyRangeFromSource, which "
					 "returns early only while the source is Manual"),
				Workspace.TransferFunction.GetRangeMin(), FixtureRangeMin, UE_KINDA_SMALL_NUMBER))
		{
			bOk = false;
		}
		if (!Test.TestEqual(
				TEXT("CONTROL: and its maximum, for the same reason"),
				Workspace.TransferFunction.GetRangeMax(), FixtureRangeMax, UE_KINDA_SMALL_NUMBER))
		{
			bOk = false;
		}

		return bOk;
	}

	/**
	 * Overwrite all five view models with values that are NEITHER the fixture's
	 * NOR the ones OpenCase leaves behind.
	 *
	 * WITHOUT THIS, THE ROUND TRIP CANNOT FAIL. Saving a fixture and then
	 * asserting the fixture is still there is a tautology - the values never
	 * left. Poisoning first converts every assertion in VerifyRestoredPanels
	 * from "this value is present" into "the load PUT this value back", which is
	 * the only one of the two that a no-op LoadSession fails. Repo memory:
	 * a-defaults-helper-can-supply-the-observed-value.
	 *
	 * EVERY POISON DIFFERS FROM THE POST-OPEN DEFAULT AS WELL AS FROM THE
	 * FIXTURE, wherever the value has more than two states. That is not
	 * fastidiousness: LoadSession re-opens the case, which resets several of
	 * these on its own, so a poison equal to the default would leave a failure
	 * ambiguous between "the case was re-opened but the session never applied"
	 * and "nothing happened at all". Three distinct values make the observed one
	 * name which. The booleans (bInterpolate, reversed colormap, slice
	 * visibility) have only two states and so cannot carry that third value;
	 * they are still poisoned to the opposite of the fixture, which is all their
	 * assertions need in order to be able to fail.
	 *
	 * @return False when any part of the poison was refused - the caller must
	 *         abandon rather than assert against a half-poisoned model, since
	 *         the un-poisoned half would still be holding the fixture and would
	 *         pass on a LoadSession that did nothing.
	 */
	bool PoisonFixture(FFlowVizWorkspaceModel& Workspace, FAutomationTestBase& Test)
	{
		bool bOk = true;
		auto Require = [&Test, &bOk](const TCHAR* What, const FCFDVizResult& Result)
		{
			if (!Result.IsOk())
			{
				Test.AddError(FString::Printf(
					TEXT("CONTROL: the poison's %s was refused (%s), so that view model may still "
						 "hold the fixture's own value and the assertion for it after the reload "
						 "would pass without the session having restored anything"),
					What, *Result.ToString()));
				bOk = false;
			}
		};

		/* --- The field FIRST -------------------------------------------------
		 *
		 * SetField re-opens the player on the same case, which resets the
		 * playback settings to the case's defaults and the playhead to frame 0,
		 * and re-binds the transfer function, which resets the colormap,
		 * component, range and opacity curve. Poisoning either of those before
		 * this line would poison a state that is about to be thrown away.
		 */
		Require(TEXT("field switch"), Workspace.SetField(PoisonFieldId));

		/* --- Player ---------------------------------------------------------- */

		FFlowVizPlaybackSettings Poisoned;
		Poisoned.Mode = EFlowVizPlaybackMode::FixedFps;	  // fixture RealTime, default Sequence
		Poisoned.LoopMode = EFlowVizLoopMode::Once;		  // fixture PingPong,  default Loop
		Poisoned.Speed = 0.25;							  // fixture 2.0,       default 1.0
		Poisoned.SequenceFrameRate = 48.0;				  // fixture 12.0,      default 24.0
		Poisoned.OutputFrameRate = 15.0;				  // fixture 60.0,      default 30.0
		Poisoned.bInterpolate = true;					  // fixture false; two-state
		Poisoned.PreloadAhead = 5;						  // fixture 3,         default 1
		Poisoned.PreloadBehind = 6;						  // fixture 2,         default 1
		Require(TEXT("playback settings"), Workspace.Player.SetSettings(Poisoned));

		// 0.85 is a stored frame time, so the playhead lands on it exactly rather
		// than between two frames where the exact value would depend on the
		// interpolation setting the line above just changed.
		Workspace.Player.SeekToTime(0.85);

		/* --- Transfer function ------------------------------------------------ */

		// Turbo: not the fixture's Inferno, and not viridis, which is what
		// binding `U` above just adopted from the manifest.
		Require(TEXT("colour map"), Workspace.TransferFunction.SetColorMap(ECFDVizColorMap::Turbo));
		Workspace.TransferFunction.SetReverseColorMap(false);
		Require(TEXT("colour band count"), Workspace.TransferFunction.SetColorBands(3));
		// Z: not the fixture's Y, and not the Magnitude `U` declares as its
		// default component. Legal because `U` has three components.
		Require(TEXT("component choice"),
			Workspace.TransferFunction.SetComponent(EFlowVizComponentChoice::Z));

		FFlowVizOpacityCurve PoisonedCurve;
		PoisonedCurve.Points.Add(FFlowVizOpacityPoint(0.4f, 0.8f));
		PoisonedCurve.Points.Add(FFlowVizOpacityPoint(0.9f, 0.2f));
		Require(TEXT("opacity curve"), Workspace.TransferFunction.SetOpacityCurve(PoisonedCurve));
		Require(TEXT("opacity multiplier"),
			Workspace.TransferFunction.SetOpacityMultiplier(0.15f));

		// NO SetManualRange HERE, DELIBERATELY. The range source is itself one of
		// the things the round trip has to restore, and any manual range would
		// leave the source at Manual - which is exactly what VerifyRestoredPanels
		// asserts, so that assertion would pass without the session. BindField
		// has already put the source back to Global as part of the field switch
		// above; the control below is what confirms it, rather than trusting it.

		/* --- Clip -------------------------------------------------------------
		 *
		 * THREE PLANES, CHOSEN SO ALL FOUR CLIP ASSERTIONS CAN FAIL.
		 *
		 *   count 3        != the fixture's 2
		 *   enabled 2      != the fixture's 1
		 *   KeptPoint      is CLIPPED here (plane A cuts z < 0.3), and the
		 *                  fixture demands it be kept
		 *   ClippedPoint   is KEPT here (A keeps z = 0.5, B keeps y = 1), and
		 *                  the fixture demands it be cut
		 *
		 * The last two are the reason the poison is not simply "one plane
		 * somewhere else": the two probe points were picked to separate the
		 * fixture's two planes, and a poison that clipped everything would leave
		 * ClippedPoint's assertion true for the wrong reason.
		 */
		Workspace.Clip.RemoveAllPlanes();

		FFlowVizClipPlane PoisonPlaneA;
		PoisonPlaneA.Normal = FVector(0.0, 0.0, 1.0);
		PoisonPlaneA.Distance = -0.3;
		PoisonPlaneA.bEnabled = true;
		Require(TEXT("first poison clip plane"), Workspace.Clip.AddPlane(PoisonPlaneA));

		FFlowVizClipPlane PoisonPlaneB;
		PoisonPlaneB.Normal = FVector(0.0, -1.0, 0.0);
		PoisonPlaneB.Distance = 1.5;
		PoisonPlaneB.bEnabled = true;
		Require(TEXT("second poison clip plane"), Workspace.Clip.AddPlane(PoisonPlaneB));

		FFlowVizClipPlane PoisonPlaneC;
		PoisonPlaneC.Normal = FVector(1.0, 0.0, 0.0);
		PoisonPlaneC.Distance = -100.0;
		PoisonPlaneC.bEnabled = false;
		Require(TEXT("third poison clip plane"), Workspace.Clip.AddPlane(PoisonPlaneC));
		if (Workspace.Clip.GetPlaneCount() == 3)
		{
			Require(TEXT("third poison clip plane's disabled state"),
				Workspace.Clip.SetPlaneEnabled(2, false));
		}

		// Inside the domain and different from both the fixture's crop and the
		// full box ResetCropBox leaves behind.
		Require(TEXT("crop box"),
			Workspace.Clip.SetCropBox(FVector(2.0, 1.0, 0.2), FVector(5.0, 3.0, 0.8)));

		/* --- Slice ------------------------------------------------------------ */

		Require(TEXT("slice normal"), Workspace.Slice.SetNormal(FVector(0.0, 1.0, 0.0)));
		Require(TEXT("slice origin"), Workspace.Slice.SetOrigin(FVector(9.0, 3.0, 0.8)));
		Require(TEXT("slice thickness"), Workspace.Slice.SetThickness(0.75));
		Require(TEXT("slab operation"), Workspace.Slice.SetSlabOp(EFlowVizSlabOp::Minimum));
		Require(TEXT("slab sample count"), Workspace.Slice.SetSlabSamples(11));
		Workspace.Slice.SetVisible(true);

		/* --- Probes ----------------------------------------------------------- */

		Workspace.Probes.RemoveAllProbes();
		Require(TEXT("poison probe"),
			Workspace.Probes.RestoreProbe(
				PoisonProbeId, FVector(2.5, 3.0, 0.25), TEXT("poison probe")));
		// Left VISIBLE, which is the opposite of the fixture's hidden probe.
		Require(TEXT("line probe"),
			Workspace.Probes.SetLineProbe(FVector(0.5, 3.5, 0.9), FVector(11.5, 0.5, 0.1)));
		Require(TEXT("line sample count"), Workspace.Probes.SetLineSampleCount(9));

		/* == The poison is only useful if it took ============================== */

		/*
		 * PHRASED AS "DIFFERS FROM THE FIXTURE" RATHER THAN "EQUALS MY POISON".
		 *
		 * The property this helper owes its caller is not that any particular
		 * number is present - it is that NOTHING VerifyRestoredPanels will look
		 * at is already holding the value it demands. Asserting the poison's own
		 * numbers would be a second copy of the lines above and would still
		 * leave the real question unasked.
		 *
		 * The result-returning setters above are all checked, so these controls
		 * cover what those cannot: the void setters, and the composite state that
		 * no single setter owns.
		 */

		if (!Test.TestTrue(
				TEXT("CONTROL: the poison moved the FIELD off the fixture's, so the two "
					 "field-id assertions after the reload can fail"),
				Workspace.Player.GetFieldId() != FixtureFieldId
					&& Workspace.TransferFunction.GetFieldId() != FixtureFieldId))
		{
			bOk = false;
		}
		if (!Test.TestFalse(
				TEXT("CONTROL: the playhead is off the fixture's physical time. SeekToTime "
					 "returns void and is silently ignored on a closed player, so nothing "
					 "else would report a poison that did not happen"),
				FMath::IsNearlyEqual(
					Workspace.Player.GetPhysicalTime(), FixturePhysicalTime, Tolerance)))
		{
			bOk = false;
		}
		if (!Test.TestFalse(
				TEXT("CONTROL: the reversed-colormap flag is off the fixture's. "
					 "SetReverseColorMap returns void"),
				Workspace.TransferFunction.IsColorMapReversed()))
		{
			bOk = false;
		}
		// TestNotEqual over an explicit comparison rather than a direct call:
		// AutomationTest.h declares TestNotEqual only for the string types, so an
		// enum argument does not compile.
		if (!Test.TestTrue(
				TEXT("CONTROL: the range SOURCE is off Manual, which is what "
					 "VerifyRestoredPanels asserts. Nothing above sets it directly - it is "
					 "BindField, reached through the field switch, that puts it back to Global"),
				Workspace.TransferFunction.GetRangeSource() != EFlowVizRangeSource::Manual))
		{
			bOk = false;
		}
		if (!Test.TestFalse(
				TEXT("CONTROL: and the range numbers moved with it, so the two verbatim-range "
					 "assertions after the reload can fail"),
				FMath::IsNearlyEqual(
					Workspace.TransferFunction.GetRangeMin(), FixtureRangeMin, UE_KINDA_SMALL_NUMBER)))
		{
			bOk = false;
		}
		if (!Test.TestEqual(
				TEXT("CONTROL: three clip planes, so the restored COUNT of two can fail"),
				Workspace.Clip.GetPlaneCount(), 3))
		{
			bOk = false;
		}
		if (!Test.TestEqual(
				TEXT("CONTROL: two of them enabled, so the restored ENABLED count of one can fail"),
				Workspace.Clip.GetEnabledPlaneCount(), 2))
		{
			bOk = false;
		}
		if (!Test.TestFalse(
				TEXT("CONTROL: the point the fixture KEEPS is currently clipped, so that "
					 "assertion after the reload is answering about the restored planes"),
				Workspace.Clip.KeepsPoint(KeptPoint)))
		{
			bOk = false;
		}
		if (!Test.TestTrue(
				TEXT("CONTROL: and the point the fixture CLIPS is currently kept, so both "
					 "directions of the pair can fail rather than only one"),
				Workspace.Clip.KeepsPoint(ClippedPoint)))
		{
			bOk = false;
		}
		if (!Test.TestTrue(
				TEXT("CONTROL: the slice is VISIBLE, the opposite of the fixture's. SetVisible "
					 "returns void"),
				Workspace.Slice.IsVisible()))
		{
			bOk = false;
		}
		if (!Test.TestNull(
				TEXT("CONTROL: the fixture's probe is GONE, so the probe assertions after the "
					 "reload are about a probe the session restored rather than one that was "
					 "never removed"),
				Workspace.Probes.FindProbe(FixtureProbeId)))
		{
			bOk = false;
		}
		if (!Test.TestNotNull(
				TEXT("CONTROL: and the poison probe is present, so the assertion that it is "
					 "gone after the reload can fail"),
				Workspace.Probes.FindProbe(PoisonProbeId)))
		{
			bOk = false;
		}

		return bOk;
	}

	/**
	 * Assert the CASE half of the fixture: the right case, opened, on the right
	 * field.
	 *
	 * Separate from the panel half below because the two are restored by
	 * different code and one of them has a branch the other does not. A session
	 * whose case has MOVED restores every panel and no case at all - that is the
	 * whole point of plan.md section 14's relink - so the missing-case test
	 * asserts the panels with this half deliberately absent, and asserts in its
	 * place that the case did NOT come back. Folding both into one helper would
	 * have forced that test to either duplicate the panel assertions or assert
	 * nothing about them.
	 */
	void VerifyRestoredCase(
		const FFlowVizWorkspaceModel& Workspace, FAutomationTestBase& Test, const TCHAR* Context)
	{
		const FString Where = FString::Printf(TEXT(" (%s)"), Context);

		Test.TestTrue(*(TEXT("the session's case is open") + Where), Workspace.IsCaseOpen());
		Test.TestEqual(
			*(TEXT("the session's FIELD is bound, not the first non-mask field a default open "
				   "would have picked") + Where),
			Workspace.Player.GetFieldId(), FixtureFieldId);
		Test.TestEqual(
			*(TEXT("and the colouring is bound to that same field") + Where),
			Workspace.TransferFunction.GetFieldId(), FixtureFieldId);
	}

	/**
	 * Assert that every fixture value the five PANELS own is present.
	 *
	 * ON VALUES, NEVER ON THE RETURNED FCFDVizResult. FlowVizSession::
	 * ApplyToViewModels is best-effort: it applies what it can and reports only
	 * the FIRST failure, so an Ok is not evidence that anything was applied and a
	 * failure is not evidence that nothing was. What landed in the models is the
	 * only observable that answers the question.
	 */
	void VerifyRestoredPanels(
		const FFlowVizWorkspaceModel& Workspace, FAutomationTestBase& Test, const TCHAR* Context)
	{
		const FString Where = FString::Printf(TEXT(" (%s)"), Context);

		/* --- Player -------------------------------------------------------- */

		const FFlowVizPlaybackSettings& Playback = Workspace.Player.GetSettings();
		Test.TestEqual(*(TEXT("the playback mode survives") + Where),
			Playback.Mode, EFlowVizPlaybackMode::RealTime);
		Test.TestEqual(*(TEXT("the loop mode survives") + Where),
			Playback.LoopMode, EFlowVizLoopMode::PingPong);
		Test.TestEqual(*(TEXT("the speed survives") + Where), Playback.Speed, 2.0);
		Test.TestEqual(*(TEXT("the sequence frame rate survives") + Where),
			Playback.SequenceFrameRate, 12.0);
		Test.TestEqual(*(TEXT("the output frame rate survives") + Where),
			Playback.OutputFrameRate, 60.0);
		Test.TestFalse(
			*(TEXT("interpolation stays OFF, so a restore did not simply leave the case's own "
				   "default (this sample declares linear, i.e. true)") + Where),
			Playback.bInterpolate);
		Test.TestEqual(*(TEXT("the preload-ahead radius survives") + Where), Playback.PreloadAhead, 3);
		Test.TestEqual(*(TEXT("the preload-behind radius survives") + Where), Playback.PreloadBehind, 2);

		Test.TestEqual(
			*(TEXT("the playhead is back at the saved physical time rather than at frame 0, "
				   "where opening a case leaves it") + Where),
			Workspace.Player.GetPhysicalTime(), FixturePhysicalTime, Tolerance);

		/* --- Transfer function ---------------------------------------------- */

		Test.TestEqual(
			*(TEXT("the colour map survives, and is not the manifest default this field declares")
				+ Where),
			Workspace.TransferFunction.GetColorMap(), ECFDVizColorMap::Inferno);
		Test.TestTrue(*(TEXT("the reversed-map flag survives") + Where),
			Workspace.TransferFunction.IsColorMapReversed());
		Test.TestEqual(*(TEXT("the colour band count survives") + Where),
			Workspace.TransferFunction.GetColorBands(), FixtureColorBands);
		Test.TestEqual(
			*(TEXT("the component choice survives; SetComponent is refused on an unbound "
				   "transfer function, so this also proves the field was bound BEFORE the "
				   "session was applied") + Where),
			Workspace.TransferFunction.GetComponent(), EFlowVizComponentChoice::Y);
		Test.TestEqual(*(TEXT("the range SOURCE survives as Manual") + Where),
			Workspace.TransferFunction.GetRangeSource(), EFlowVizRangeSource::Manual);
		Test.TestEqual(*(TEXT("the manual range minimum survives verbatim") + Where),
			Workspace.TransferFunction.GetRangeMin(), FixtureRangeMin, UE_KINDA_SMALL_NUMBER);
		Test.TestEqual(*(TEXT("the manual range maximum survives verbatim") + Where),
			Workspace.TransferFunction.GetRangeMax(), FixtureRangeMax, UE_KINDA_SMALL_NUMBER);

		Test.TestEqual(
			*(TEXT("the opacity multiplier survives. The session folds it INTO the curve on "
				   "capture and unfolds it on apply, so a restore that skipped the unfold "
				   "leaves it at 1.0 and every pixel is more opaque than it was saved")
				+ Where),
			Workspace.TransferFunction.GetOpacityMultiplier(), FixtureOpacityMultiplier,
			UE_KINDA_SMALL_NUMBER);

		const FFlowVizOpacityCurve& RestoredCurve = Workspace.TransferFunction.GetOpacityCurve();
		if (Test.TestEqual(
				*(TEXT("the opacity curve has the fixture's two control points. The default ramp "
					   "BindField installs also has two, so the POSITIONS below are what "
					   "distinguish them - a count alone would pass on the default") + Where),
				RestoredCurve.Points.Num(), 2))
		{
			Test.TestEqual(*(TEXT("first opacity point position") + Where),
				RestoredCurve.Points[0].Position, FixtureOpacityFirstPosition, UE_KINDA_SMALL_NUMBER);
			Test.TestEqual(*(TEXT("first opacity point value") + Where),
				RestoredCurve.Points[0].Opacity, FixtureOpacityFirstValue, UE_KINDA_SMALL_NUMBER);
			Test.TestEqual(*(TEXT("second opacity point position") + Where),
				RestoredCurve.Points[1].Position, FixtureOpacitySecondPosition, UE_KINDA_SMALL_NUMBER);
			Test.TestEqual(*(TEXT("second opacity point value") + Where),
				RestoredCurve.Points[1].Opacity, FixtureOpacitySecondValue, UE_KINDA_SMALL_NUMBER);
		}
		Test.TestEqual(
			*(TEXT("the multiplier is NOT also left inside the restored curve, which would apply "
				   "it twice and halve every alpha again") + Where),
			RestoredCurve.OpacityMultiplier, 1.0f, UE_KINDA_SMALL_NUMBER);

		/* --- Clip ----------------------------------------------------------- */

		Test.TestEqual(*(TEXT("both clip planes come back") + Where),
			Workspace.Clip.GetPlaneCount(), 2);
		Test.TestEqual(
			*(TEXT("and exactly one of them is enabled, so the disabled plane came back "
				   "disabled rather than clipping the scene on reload") + Where),
			Workspace.Clip.GetEnabledPlaneCount(), 1);

		Test.TestTrue(
			*(TEXT("a point the ENABLED plane keeps and the DISABLED plane would have cut is "
				   "kept, which is the behaviour the renderer consumes rather than the field "
				   "value the struct carries") + Where),
			Workspace.Clip.KeepsPoint(KeptPoint));
		Test.TestFalse(
			*(TEXT("and a point on the far side of the ENABLED plane is cut, so the plane was "
				   "restored and restored switched on") + Where),
			Workspace.Clip.KeepsPoint(ClippedPoint));

		Test.TestTrue(
			*(TEXT("the crop is still ACTIVE. FFlowVizClipViewModel::SetDomainSize calls "
				   "ResetCropBox, and opening a case sets the domain - so a load that applied "
				   "the session BEFORE opening the case erases exactly this and nothing else "
				   "reports it") + Where),
			Workspace.Clip.IsCropActive());
		Test.TestTrue(
			*FString::Printf(
				TEXT("the crop minimum survives; got (%g, %g, %g)%s"),
				Workspace.Clip.GetCropMin().X, Workspace.Clip.GetCropMin().Y,
				Workspace.Clip.GetCropMin().Z, *Where),
			Workspace.Clip.GetCropMin().Equals(FixtureCropMin, Tolerance));
		Test.TestTrue(
			*FString::Printf(
				TEXT("the crop maximum survives; got (%g, %g, %g)%s"),
				Workspace.Clip.GetCropMax().X, Workspace.Clip.GetCropMax().Y,
				Workspace.Clip.GetCropMax().Z, *Where),
			Workspace.Clip.GetCropMax().Equals(FixtureCropMax, Tolerance));

		/* --- Slice ----------------------------------------------------------- */

		Test.TestTrue(
			*FString::Printf(
				TEXT("the slice origin survives and is NOT the domain centre CenterOnDomain "
					 "leaves it at when a case opens; got (%g, %g, %g)%s"),
				Workspace.Slice.GetOrigin().X, Workspace.Slice.GetOrigin().Y,
				Workspace.Slice.GetOrigin().Z, *Where),
			Workspace.Slice.GetOrigin().Equals(FixtureSliceOrigin, Tolerance));
		Test.TestTrue(
			*FString::Printf(
				TEXT("the slice normal survives and is not the default +Z; got (%g, %g, %g)%s"),
				Workspace.Slice.GetNormal().X, Workspace.Slice.GetNormal().Y,
				Workspace.Slice.GetNormal().Z, *Where),
			Workspace.Slice.GetNormal().Equals(FixtureSliceNormal, Tolerance));
		Test.TestEqual(*(TEXT("the slice thickness survives") + Where),
			Workspace.Slice.GetThickness(), FixtureSliceThickness, Tolerance);
		Test.TestEqual(*(TEXT("the slab operation survives") + Where),
			Workspace.Slice.GetSlabOp(), EFlowVizSlabOp::Maximum);
		Test.TestEqual(*(TEXT("the slab sample count survives") + Where),
			Workspace.Slice.GetSlabSamples(), FixtureSlabSamples);
		Test.TestFalse(
			*(TEXT("the slice comes back HIDDEN, so a hidden slice does not reappear over the "
				   "volume on reload") + Where),
			Workspace.Slice.IsVisible());

		/* --- Probes ----------------------------------------------------------- */

		Test.TestEqual(*(TEXT("the probe comes back, and only the one") + Where),
			Workspace.Probes.GetProbeCount(), 1);
		const FFlowVizProbe* Restored = Workspace.Probes.FindProbe(FixtureProbeId);
		if (Test.TestNotNull(
				*(TEXT("the probe is restored UNDER ITS SAVED ID. A new id would leave every "
					   "positional assertion below passing while the chart series it keys is "
					   "silently orphaned") + Where),
				Restored))
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("the probe's solver position survives; got (%g, %g, %g)%s"),
					Restored->SolverPosition.X, Restored->SolverPosition.Y,
					Restored->SolverPosition.Z, *Where),
				Restored->SolverPosition.Equals(FixtureProbePosition, Tolerance));
			Test.TestFalse(
				*(TEXT("the probe comes back HIDDEN. SetProbeVisible returns false for an "
					   "unknown id and reports nothing, so a probe restored under a fresh id "
					   "comes back visible with no error - this is the assertion that catches "
					   "it") + Where),
				Restored->bVisible);
		}

		Test.TestTrue(*(TEXT("the line probe comes back") + Where), Workspace.Probes.HasLineProbe());
		Test.TestTrue(
			*FString::Printf(TEXT("the line probe's start survives; got (%g, %g, %g)%s"),
				Workspace.Probes.GetLineStart().X, Workspace.Probes.GetLineStart().Y,
				Workspace.Probes.GetLineStart().Z, *Where),
			Workspace.Probes.GetLineStart().Equals(FixtureLineStart, Tolerance));
		Test.TestTrue(
			*FString::Printf(TEXT("the line probe's end survives; got (%g, %g, %g)%s"),
				Workspace.Probes.GetLineEnd().X, Workspace.Probes.GetLineEnd().Y,
				Workspace.Probes.GetLineEnd().Z, *Where),
			Workspace.Probes.GetLineEnd().Equals(FixtureLineEnd, Tolerance));
		Test.TestEqual(*(TEXT("the line probe's sample count survives") + Where),
			Workspace.Probes.GetLineSampleCount(), FixtureLineSamples);
	}
}

/* ========================================================================== */
/* The defaults control                                                        */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceModelSessionDefaultsTest,
	"FlowViz.UI.WorkspaceModel.SessionDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

/**
 * WHAT OpenCase ALONE LEAVES BEHIND.
 *
 * This test asserts nothing about sessions. It exists so the round trip's
 * assertions are provably capable of failing: every value below is one the
 * round-trip test demands be DIFFERENT, and if any of them ever drifted onto a
 * fixture value, the corresponding round-trip assertion would start passing
 * whether or not the session was applied - silently, and while still reading as
 * coverage.
 *
 * It is a separate test rather than a block inside the round trip so that a
 * drift reports as its own failure with its own name, instead of turning up as
 * a confusing pass.
 */
bool FFlowVizWorkspaceModelSessionDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceModelTest;

	const FString CaseDir = GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		AddError(FString::Printf(
			TEXT("the sample case is required for this test and was not found at '%s'"), *CaseDir));
		return false;
	}

	FFlowVizWorkspaceModel Workspace;
	const FCFDVizResult Opened = Workspace.OpenCase(CaseDir, FixtureFieldId);
	if (!TestTrue(
			*FString::Printf(TEXT("opening the sample case on '%s' succeeds: %s"),
				*FixtureFieldId.ToString(), *Opened.ToString()),
			Opened.IsOk()))
	{
		return false;
	}

	/* --- The field a DEFAULT open would have chosen ------------------------ */

	// Asserted here rather than assumed by the fixture's comment. If the sample
	// ever reorders its fields so that `vorticity` becomes the first non-mask
	// field, the round trip's "the session's FIELD is bound, not the first
	// non-mask field a default open would have picked" stops discriminating -
	// and nothing else would say so.
	{
		FFlowVizWorkspaceModel DefaultOpen;
		if (DefaultOpen.OpenCase(CaseDir).IsOk())
		{
			TestEqual(
				TEXT("a default open picks the FIRST NON-MASK field, which is the poison's field "
					 "and not the fixture's - this is what makes the round trip's field "
					 "assertion able to fail"),
				DefaultOpen.Player.GetFieldId(), PoisonFieldId);
			TestTrue(
				TEXT("and that default field is not the fixture's, so a LoadSession that opened "
					 "the case with NAME_None is distinguishable from one that used the saved "
					 "field id"),
				DefaultOpen.Player.GetFieldId() != FixtureFieldId);
		}
		else
		{
			AddError(TEXT("a default open of the sample case failed, so the field-discrimination "
						  "control could not run"));
		}
	}

	/* --- Player ------------------------------------------------------------ */

	const FFlowVizPlaybackSettings& Playback = Workspace.Player.GetSettings();
	TestEqual(TEXT("a freshly opened case plays in Sequence mode, not the fixture's RealTime"),
		Playback.Mode, EFlowVizPlaybackMode::Sequence);
	TestEqual(TEXT("and loops, rather than the fixture's PingPong"),
		Playback.LoopMode, EFlowVizLoopMode::Loop);
	TestEqual(TEXT("at speed 1.0, not the fixture's 2.0"), Playback.Speed, 1.0);
	TestEqual(TEXT("at 24 stored frames per second, not the fixture's 12"),
		Playback.SequenceFrameRate, 24.0);
	TestEqual(TEXT("with a 30 fps output rate, not the fixture's 60"),
		Playback.OutputFrameRate, 30.0);
	TestTrue(
		TEXT("WITH INTERPOLATION ON, because this sample declares linear. The fixture turns it "
			 "OFF for exactly this reason: a bInterpolate that came back true could otherwise "
			 "have come from the case rather than from the session"),
		Playback.bInterpolate);
	TestEqual(TEXT("preloading one frame ahead, not the fixture's 3"), Playback.PreloadAhead, 1);
	TestEqual(TEXT("and one behind, not the fixture's 2"), Playback.PreloadBehind, 1);

	TestEqual(
		TEXT("the playhead sits at t = 0 - Player::Open seeks to frame 0 - so the fixture's 0.35 "
			 "cannot be mistaken for an un-restored default"),
		Workspace.Player.GetPhysicalTime(), 0.0, Tolerance);

	/* --- Transfer function -------------------------------------------------- */

	TestEqual(
		TEXT("the colour map is the one THIS FIELD declares (coolwarm), not the fixture's "
			 "Inferno"),
		Workspace.TransferFunction.GetColorMap(), ECFDVizColorMap::CoolWarm);
	TestFalse(TEXT("the map is not reversed, which the fixture makes it"),
		Workspace.TransferFunction.IsColorMapReversed());
	TestEqual(TEXT("there is no banding, rather than the fixture's 7 bands"),
		Workspace.TransferFunction.GetColorBands(), 0);
	TestEqual(
		TEXT("the component is the manifest's declared Magnitude, not the fixture's Y"),
		Workspace.TransferFunction.GetComponent(), EFlowVizComponentChoice::Magnitude);
	TestEqual(
		TEXT("the range source is Global - rule 8's default - not the fixture's Manual"),
		Workspace.TransferFunction.GetRangeSource(), EFlowVizRangeSource::Global);

	// THE RANGE NUMBERS, CHECKED AGAINST THE MANIFEST RATHER THAN AGAINST
	// "not the fixture". A Global range on a DIVERGING map goes through
	// MakeDefaultDomain, which symmetrises it about zero; this field's magnitude
	// statistics are [0.00066, 16.332] and coolwarm is diverging, so the domain
	// lands on +/-16.332. The fixture's [-12.5, 33.25] is inside neither bound,
	// which is what makes both range assertions in the round trip discriminating.
	TestTrue(
		*FString::Printf(
			TEXT("the default range minimum comes from the field's statistics, symmetrised by "
				 "the diverging map; got %g, and the fixture's is %g"),
			Workspace.TransferFunction.GetRangeMin(), FixtureRangeMin),
		!FMath::IsNearlyEqual(
			Workspace.TransferFunction.GetRangeMin(), FixtureRangeMin, UE_KINDA_SMALL_NUMBER));
	TestTrue(
		*FString::Printf(
			TEXT("and the maximum likewise; got %g, and the fixture's is %g"),
			Workspace.TransferFunction.GetRangeMax(), FixtureRangeMax),
		!FMath::IsNearlyEqual(
			Workspace.TransferFunction.GetRangeMax(), FixtureRangeMax, UE_KINDA_SMALL_NUMBER));

	TestEqual(
		TEXT("the opacity multiplier is 1.0, not the fixture's 0.6 - so a restore that dropped "
			 "the unfold is distinguishable"),
		Workspace.TransferFunction.GetOpacityMultiplier(), 1.0f, UE_KINDA_SMALL_NUMBER);

	// THE DEFAULT CURVE HAS THREE POINTS SINCE P6: the ramp anchors alpha at
	// zero through the bottom quarter -- (0,0),(0.25,0),(1,1) -- so quiescent
	// fluid is fully invisible instead of a domain-filling haze. Still none of
	// the positions coincide with the fixture's (0.25 shares a POSITION with
	// the fixture's first point but carries a different value, which the round
	// trip's value assertions distinguish).
	const FFlowVizOpacityCurve& DefaultCurve = Workspace.TransferFunction.GetOpacityCurve();
	if (TestEqual(
			TEXT("the default opacity ramp has three control points -- the P6 "
				 "transparent-anchor ramp"),
			DefaultCurve.Points.Num(), 3))
	{
		TestEqual(TEXT("the default ramp starts at 0"),
			DefaultCurve.Points[0].Position, 0.0f, UE_KINDA_SMALL_NUMBER);
		TestEqual(TEXT("holds zero through the bottom quarter"),
			DefaultCurve.Points[1].Position, 0.25f, UE_KINDA_SMALL_NUMBER);
		TestEqual(TEXT("with zero opacity there"),
			DefaultCurve.Points[1].Opacity, 0.0f, UE_KINDA_SMALL_NUMBER);
		TestEqual(TEXT("and ends at 1"),
			DefaultCurve.Points[2].Position, 1.0f, UE_KINDA_SMALL_NUMBER);
	}

	/* --- Clip ---------------------------------------------------------------- */

	TestEqual(TEXT("a freshly opened case has no clip planes, not the fixture's two"),
		Workspace.Clip.GetPlaneCount(), 0);
	TestFalse(
		TEXT("and no active crop. SetDomainSize calls ResetCropBox, so the crop is the full "
			 "domain - which is what IsCropActive reports as inactive"),
		Workspace.Clip.IsCropActive());

	// Both probe points survive when nothing clips. Stated because the round
	// trip's KeptPoint assertion would pass in this state too: it is
	// ClippedPoint that carries the discrimination for the enabled plane, and
	// the poison is what makes KeptPoint's able to fail.
	TestTrue(
		TEXT("with no planes, the fixture's KEPT point is kept - so this assertion alone does "
			 "not prove a restore, which is why PoisonFixture clips it first"),
		Workspace.Clip.KeepsPoint(KeptPoint));
	TestTrue(
		TEXT("and so is the point the fixture CLIPS, which is the assertion that discriminates "
			 "against an un-restored clip model"),
		Workspace.Clip.KeepsPoint(ClippedPoint));

	/* --- Slice --------------------------------------------------------------- */

	// THE CENTRE, SPELLED OUT FROM THE MANIFEST rather than computed as
	// GetDomainSize() * 0.5. That expression is CenterOnDomain's own body reading
	// CenterOnDomain's own member, so it would hold whatever the model happened
	// to contain - including nothing. The manifest declares 56 x 28 x 6 cells at
	// (0.2142857..., 0.1428571..., 0.1666666...) m, which is a 12 x 4 x 1 m box,
	// so the centre is (6, 2, 0.5). Repo memory:
	// uninitialized-expectations-are-noise.
	//
	// If the sample's dimensions ever change, this fails - which is the point:
	// it is also the assertion that would catch the centre drifting onto the
	// fixture's (3, 1.25, 0.375) and quietly disarming the round trip.
	const FVector Centre(6.0, 2.0, 0.5);
	TestTrue(
		*FString::Printf(
			TEXT("the domain is the manifest's 12 x 4 x 1 m; got (%g, %g, %g)"),
			Workspace.Slice.GetDomainSize().X, Workspace.Slice.GetDomainSize().Y,
			Workspace.Slice.GetDomainSize().Z),
		Workspace.Slice.GetDomainSize().Equals(FVector(12.0, 4.0, 1.0), Tolerance));
	TestTrue(
		*FString::Printf(
			TEXT("an opened case centres the slice on it; got (%g, %g, %g), expected "
				 "(%g, %g, %g)"),
			Workspace.Slice.GetOrigin().X, Workspace.Slice.GetOrigin().Y,
			Workspace.Slice.GetOrigin().Z, Centre.X, Centre.Y, Centre.Z),
		Workspace.Slice.GetOrigin().Equals(Centre, Tolerance));
	TestFalse(
		TEXT("and that centre is not the fixture's slice origin, so the round trip's origin "
			 "assertion can fail"),
		Workspace.Slice.GetOrigin().Equals(FixtureSliceOrigin, Tolerance));

	TestTrue(
		*FString::Printf(
			TEXT("the default slice normal is +Z, not the fixture's +X; got (%g, %g, %g)"),
			Workspace.Slice.GetNormal().X, Workspace.Slice.GetNormal().Y,
			Workspace.Slice.GetNormal().Z),
		Workspace.Slice.GetNormal().Equals(FVector(0.0, 0.0, 1.0), Tolerance));
	TestEqual(TEXT("the default slice has zero thickness, not the fixture's 0.25"),
		Workspace.Slice.GetThickness(), 0.0, Tolerance);
	TestEqual(
		TEXT("so its slab op is None, not the fixture's Maximum - a zero-thickness slice "
			 "REFUSES an aggregation, which is why the fixture sets the thickness first"),
		Workspace.Slice.GetSlabOp(), EFlowVizSlabOp::None);
	TestEqual(TEXT("and it takes one sample, not the fixture's 6"),
		Workspace.Slice.GetSlabSamples(), 1);
	/*
	 * P3 SPLIT THE FLAG. Visible now means "the cut plane mesh is on screen"
	 * -- the default picture, so it defaults TRUE. The sliver hazard #77
	 * flipped the old flag for lives on the volume-slab switch, which still
	 * defaults false: no clip planes reach the volume on open.
	 */
	TestTrue(TEXT("a default slice is VISIBLE -- the cut plane is the default picture (P3)"),
		Workspace.Slice.IsVisible());
	TestFalse(TEXT("but the volume slab is OFF -- #77's sliver hazard lives on this flag now"),
		Workspace.Slice.IsVolumeSlabEnabled());

	/* --- Probes -------------------------------------------------------------- */

	TestEqual(TEXT("a freshly opened case has no probes, not the fixture's one"),
		Workspace.Probes.GetProbeCount(), 0);
	TestFalse(TEXT("and no line probe"), Workspace.Probes.HasLineProbe());
	TestEqual(
		TEXT("the default line sample count is 32, not the fixture's 48 - so that assertion "
			 "is not reading a default"),
		Workspace.Probes.GetLineSampleCount(), 32);

	return true;
}

/* ========================================================================== */
/* The round trip                                                              */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceModelSessionRoundTripTest,
	"FlowViz.UI.WorkspaceModel.SessionRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

/**
 * Configure -> save -> POISON -> load -> everything is back.
 *
 * THE POISON IS THE TEST. Without it this is "save a fixture, assert the
 * fixture is still there", which passes against a LoadSession whose body is
 * `return FCFDVizResult::Ok();`. The poison is what turns every assertion in
 * VerifyRestoredPanels into a claim about what the LOAD did.
 *
 * THE DOMAIN IS NOT RE-SET BETWEEN SAVE AND LOAD, and that is load-bearing.
 * FFlowVizClipViewModel::SetDomainSize calls ResetCropBox, and OpenCase sets
 * the domain - so a LoadSession that applied the session BEFORE re-opening the
 * case would restore the crop and then erase it, reporting Ok the whole way.
 * The crop assertion in VerifyRestoredPanels is what catches that ordering,
 * which is why this test loads into the SAME workspace rather than a fresh one:
 * a fresh workspace would have to open the case anyway and the ordering hazard
 * would never arise.
 */
bool FFlowVizWorkspaceModelSessionRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceModelTest;

	const FString CaseDir = GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		AddError(FString::Printf(
			TEXT("the sample case is required for this test and was not found at '%s'"), *CaseDir));
		return false;
	}

	const FString SessionDir = GetSessionDir(TEXT("RoundTrip"));
	const FString SessionPath = FPaths::Combine(
		SessionDir, FString::Printf(TEXT("roundtrip.%s"), FlowVizSession::GetFileExtension()));

	ON_SCOPE_EXIT
	{
		// Removed whichever way the test leaves, so a failure does not make the
		// NEXT run read a stale session and pass against a save that never
		// happened.
		IFileManager::Get().DeleteDirectory(*SessionDir, /*RequireExists=*/false, /*Tree=*/true);
	};

	FFlowVizWorkspaceModel Workspace;
	const FCFDVizResult Opened = Workspace.OpenCase(CaseDir, FixtureFieldId);
	if (!TestTrue(
			*FString::Printf(TEXT("opening the sample case succeeds: %s"), *Opened.ToString()),
			Opened.IsOk()))
	{
		return false;
	}

	if (!ConfigureFixture(Workspace, *this))
	{
		// ConfigureFixture has already reported which setter was refused. Going
		// on would compare defaults against defaults.
		return false;
	}

	/* == Save =============================================================== */

	const FCFDVizResult Saved = Workspace.SaveSession(SessionPath);
	if (!TestTrue(
			*FString::Printf(TEXT("saving the session succeeds: %s"), *Saved.ToString()),
			Saved.IsOk()))
	{
		return false;
	}
	if (!TestTrue(
			*FString::Printf(TEXT("the session file exists at '%s'"), *SessionPath),
			FPaths::FileExists(SessionPath)))
	{
		// A SaveSession that reported Ok without writing would make the load
		// below fail with FileNotFound, which reads as a load bug. Attributed
		// here instead.
		return false;
	}

	/* == Poison ============================================================= */

	if (!PoisonFixture(Workspace, *this))
	{
		return false;
	}

	/* == Load =============================================================== */

	const FCFDVizResult Loaded = Workspace.LoadSession(SessionPath);
	TestTrue(
		*FString::Printf(TEXT("loading the session succeeds: %s"), *Loaded.ToString()),
		Loaded.IsOk());

	// NOT `return false` ON A FAILED LOAD, deliberately. ApplyToViewModels is
	// best-effort and reports the FIRST failure after applying what it could, so
	// a non-Ok result still leaves a scene worth asserting about - and the
	// assertions below are what say WHICH part did not arrive. Abandoning here
	// would trade a precise report for a single opaque line.
	VerifyRestoredCase(Workspace, *this, TEXT("after reload"));
	VerifyRestoredPanels(Workspace, *this, TEXT("after reload"));

	/* == And the poison is gone ============================================= */

	TestNull(
		TEXT("the poison probe is GONE. ApplyToViewModels calls RemoveAllProbes before "
			 "restoring, so a probe the user deleted before saving does not come back; "
			 "without this, an implementation that merely ADDED the saved probes would pass "
			 "every assertion above"),
		Workspace.Probes.FindProbe(PoisonProbeId));

	return true;
}

/* ========================================================================== */
/* The case moved                                                              */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceModelSessionMissingCaseTest,
	"FlowViz.UI.WorkspaceModel.SessionMissingCase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

/**
 * The session's case has MOVED: everything else still comes back.
 *
 * plan.md section 14 requires that a session with a missing case "must allow the
 * user to relink". A relink is only worth offering if the user still has the
 * setup they would relink INTO - so this branch reports the missing case AND
 * applies all five panels, and the two halves of that are equally load-bearing.
 * Neither is checked by SessionRoundTrip, whose case is always present.
 *
 * BOTH FAILURE DIRECTIONS ARE ASSERTED, because they are opposite mistakes and
 * a test for one is silent about the other:
 *
 *   return early on a missing case  -> the panels never arrive, and the relink
 *                                      prompt has nothing behind it. Caught by
 *                                      VerifyRestoredPanels below.
 *   open the case anyway            -> the REPORT degrades to the reader's own
 *                                      "manifest not found", which names no
 *                                      relink and does not say the rest of the
 *                                      session survived. Caught by the message
 *                                      assertion below.
 *
 * WHY THE SECOND DIRECTION IS ABOUT THE MESSAGE AND NOT ABOUT LOST STATE. An
 * earlier version of this comment claimed OpenCase runs CloseCase before it can
 * fail on a bad path, so opening anyway would leave the workspace with no case.
 * THAT IS NOT WHAT OpenCase DOES: it parses into a local and validates the
 * codec, the field and the grid, and only reaches CloseCase once every gate has
 * passed (FlowVizWorkspaceModel.cpp, "Parsed into a local first"). A moved-away
 * path fails at the very first gate, so the open case survives either way and an
 * assertion on IsCaseOpen() cannot tell the two implementations apart.
 *
 * A mutation arm that dropped the bCaseFound guard SURVIVED this test for
 * exactly that reason (Tools/mutants/session-workspace-arm4.txt). The one thing
 * that does differ is the text the user is shown, and a relink prompt is built
 * from that text -- so it is asserted rather than assumed.
 *
 * THE STATE IS BUILT BY A REAL SAVE AND A REAL PARSE, then has its resolved path
 * repointed at a file that does not exist. Constructing an FFlowVizSessionState
 * by hand would let a field the capture actually writes go unset, and the test
 * would then be about a shape production never produces. The control below
 * asserts the parse found the case FIRST, so repointing it is a change rather
 * than a no-op.
 */
bool FFlowVizWorkspaceModelSessionMissingCaseTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceModelTest;

	const FString CaseDir = GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		AddError(FString::Printf(
			TEXT("the sample case is required for this test and was not found at '%s'"), *CaseDir));
		return false;
	}

	const FString SessionDir = GetSessionDir(TEXT("MissingCase"));
	const FString SessionPath = FPaths::Combine(
		SessionDir, FString::Printf(TEXT("moved.%s"), FlowVizSession::GetFileExtension()));

	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*SessionDir, /*RequireExists=*/false, /*Tree=*/true);
	};

	FFlowVizWorkspaceModel Workspace;
	const FCFDVizResult Opened = Workspace.OpenCase(CaseDir, FixtureFieldId);
	if (!TestTrue(
			*FString::Printf(TEXT("opening the sample case succeeds: %s"), *Opened.ToString()),
			Opened.IsOk()))
	{
		return false;
	}

	if (!ConfigureFixture(Workspace, *this))
	{
		return false;
	}

	const FCFDVizResult Saved = Workspace.SaveSession(SessionPath);
	if (!TestTrue(
			*FString::Printf(TEXT("saving the session succeeds: %s"), *Saved.ToString()),
			Saved.IsOk()))
	{
		return false;
	}

	/* == The case moves ===================================================== */

	FFlowVizSessionState State;
	const FCFDVizResult Parsed = FlowVizSession::LoadFromFile(SessionPath, State);
	if (!TestTrue(
			*FString::Printf(TEXT("parsing the session succeeds: %s"), *Parsed.ToString()),
			Parsed.IsOk()))
	{
		return false;
	}
	if (!TestTrue(
			TEXT("CONTROL: the parse FOUND the case, so clearing the flag below is a change "
				 "rather than a restatement of what the parse already produced"),
			State.bCaseFound))
	{
		return false;
	}

	const FString MovedAway = FPaths::Combine(SessionDir, TEXT("gone"), TEXT("manifest.json"));
	if (!TestFalse(
			*FString::Printf(
				TEXT("CONTROL: the path the case is moved to genuinely does not exist, so "
					 "OpenCase could not succeed on it even if it were called: '%s'"),
				*MovedAway),
			FPaths::FileExists(MovedAway)))
	{
		return false;
	}
	State.ResolvedCasePath = MovedAway;
	State.bCaseFound = false;

	/* == Poison ============================================================= */

	if (!PoisonFixture(Workspace, *this))
	{
		return false;
	}

	// Recorded AFTER the poison, which is what last set it. The assertion below
	// is that the load left it alone, so the value has to be read from the state
	// the load is handed rather than assumed from PoisonFieldId - if the poison's
	// field switch were ever refused, PoisonFixture reports it and this test has
	// already returned.
	const FName FieldBeforeLoad = Workspace.Player.GetFieldId();

	/* == Load =============================================================== */

	const FCFDVizResult Loaded = Workspace.LoadState(State);

	TestFalse(
		*FString::Printf(
			TEXT("the missing case is REPORTED rather than swallowed; got '%s'"),
			*Loaded.ToString()),
		Loaded.IsOk());
	TestEqual(
		TEXT("and reported as FileNotFound specifically, which is what a relink prompt is "
			 "keyed on - a generic failure would leave the caller unable to tell 'your case "
			 "moved' from 'your session is corrupt'"),
		Loaded.Error, ECFDVizError::FileNotFound);
	TestEqual(
		*FString::Printf(
			TEXT("naming the path that was looked for, so the prompt can say which file is "
				 "missing; got '%s'"),
			*Loaded.FilePath),
		Loaded.FilePath, MovedAway);

	/*
	 * THE MESSAGE IS THE PART THAT DISTINGUISHES THE TWO IMPLEMENTATIONS, and it
	 * is checked because the relink prompt is written from it. The error code and
	 * the path are IDENTICAL either way: an implementation that dropped the
	 * bCaseFound guard and called OpenCase on the moved-away path would get
	 * FileNotFound naming that same path back from the manifest reader. What it
	 * would NOT produce is a message saying the rest of the session survived and
	 * a relink is possible -- it would say "manifest not found", which reads as a
	 * failed open and gives the prompt nothing to offer.
	 *
	 * Asserted on a substring rather than the whole sentence: this is about the
	 * message coming from the branch that knows a relink is available, not about
	 * its exact wording, and pinning the wording would make every copy-edit a
	 * test failure.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("the message is the RELINK message, not the manifest reader's 'not found'. "
				 "Both carry FileNotFound and both name this path, so the text is the only "
				 "thing a relink prompt could be built from; got '%s'"),
			*Loaded.Message),
		Loaded.Message.Contains(TEXT("relink")));
	TestTrue(
		*FString::Printf(
			TEXT("and it says the rest of the session came back, which is what makes the "
				 "prompt worth showing rather than an error to dismiss; got '%s'"),
			*Loaded.Message),
		Loaded.Message.Contains(TEXT("everything else was applied")));

	/* == The case the user had open is UNTOUCHED ============================ */

	// NOT the assertion that separates the two implementations -- see the header
	// note. OpenCase validates into a local and reaches CloseCase only after
	// every gate passes, so a moved-away path leaves the open case alone whether
	// or not the guard above it exists. Kept because it pins that documented
	// property of OpenCase, which the relink flow depends on.
	TestTrue(
		TEXT("the case that was already open stays open, so the relink prompt is offered "
			 "against a scene that is still on screen"),
		Workspace.IsCaseOpen());
	TestEqual(
		TEXT("and on the field it was already on, NOT the session's. Re-binding the saved "
			 "field against a case the session did not open is how a scene comes back looking "
			 "restored while showing different data"),
		Workspace.Player.GetFieldId(), FieldBeforeLoad);

	/* == And every panel is back ============================================ */

	// The whole point of the branch: the setup the user would relink INTO
	// survived. NOT VerifyRestoredCase - the case is deliberately not restored
	// here, and that difference is the reason the two halves are separate
	// helpers.
	VerifyRestoredPanels(Workspace, *this, TEXT("after a load whose case had moved"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceModelTest,
	"FlowViz.UI.WorkspaceModel.OpenCase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceModelTest::RunTest(const FString& Parameters)
{
	const FString CaseDir = FlowVizWorkspaceModelTest::GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		// A SKIP IS A FAILURE HERE. Silently returning true would report Success
		// for a test that verified nothing, which is exactly the "green total
		// hiding a skip" this project has been bitten by.
		AddError(FString::Printf(
			TEXT("the sample case is required for this test and was not found at '%s'"), *CaseDir));
		return false;
	}

	/* == Before opening, nothing is bound =================================== */
	{
		FFlowVizWorkspaceModel Workspace;
		TestFalse(TEXT("a fresh workspace has no case open"), Workspace.IsCaseOpen());
		TestNull(TEXT("a fresh workspace has no case"), Workspace.GetCase());
		TestFalse(
			TEXT("a fresh workspace's transfer function is unbound"),
			Workspace.TransferFunction.IsBound());
		TestEqual(
			TEXT("a fresh workspace has no volume fields to list"),
			Workspace.GetVolumeFieldIds().Num(),
			0);
	}

	/* == Opening binds every view model ===================================== */
	{
		FFlowVizWorkspaceModel Workspace;
		const FCFDVizResult Result = Workspace.OpenCase(CaseDir);
		if (!TestTrue(
				*FString::Printf(TEXT("opening the sample case succeeds: %s"), *Result.ToString()),
				Result.IsOk()))
		{
			return false;
		}

		TestTrue(TEXT("the case is open"), Workspace.IsCaseOpen());
		TestNotNull(TEXT("the open case is reachable"), Workspace.GetCase());

		// 1. THE TIMELINE IS BOUND TO THIS WORKSPACE'S PLAYER. Asserting the exact
		// pointer, not merely IsBound(): a workspace that bound the timeline to
		// some other player would be "bound" and would drive the wrong case.
		TestTrue(TEXT("the timeline view model is bound"), Workspace.Timeline.IsBound());
		TestEqual(
			TEXT("the timeline is bound to THIS workspace's player, not another"),
			Workspace.Timeline.GetPlayer(),
			&Workspace.Player);

		// The sample case is multi-frame, so the transport must actually be usable.
		// Without this, a workspace that opened a case but left the player closed
		// would still pass the binding assertion above.
		TestTrue(
			TEXT("the bound timeline reports frames, so the transport is operable"),
			Workspace.Timeline.HasFrames());
		TestTrue(
			TEXT("the sample case has more than one frame, so play is enabled"),
			Workspace.Timeline.CanPlay());

		// 2. THE TRANSFER FUNCTION IS BOUND TO THE FIELD THAT WAS OPENED.
		TestTrue(
			TEXT("the transfer function is bound to a field"),
			Workspace.TransferFunction.IsBound());
		TestEqual(
			TEXT("the transfer function is bound to the same field the player is playing"),
			Workspace.TransferFunction.GetFieldId(),
			Workspace.Player.GetFieldId());

		// 3. THE DOMAIN REACHED BOTH VIEW MODELS THAT HAVE ONE, AND IT IS REAL.
		//
		// This is the assertion the whole file exists for. Both view models default
		// to FVector::OneVector, so a propagation failure leaves a WORKING slider
		// calibrated to a 1x1x1 box - no error anywhere.
		TestTrue(TEXT("the clip view model has a domain"), Workspace.Clip.HasDomain());
		TestTrue(TEXT("the slice view model has a domain"), Workspace.Slice.HasDomain());

		const FVector ClipDomain = Workspace.Clip.GetDomainSize();
		const FVector SliceDomain = Workspace.Slice.GetDomainSize();

		TestTrue(
			*FString::Printf(
				TEXT("the clip and slice view models share one domain; clip (%g, %g, %g) vs "
					 "slice (%g, %g, %g)"),
				ClipDomain.X, ClipDomain.Y, ClipDomain.Z,
				SliceDomain.X, SliceDomain.Y, SliceDomain.Z),
			ClipDomain.Equals(SliceDomain, UE_KINDA_SMALL_NUMBER));

		// NOT THE DEFAULT. A domain of exactly (1,1,1) is what an unconfigured view
		// model reports, so a test that only compared the two to each other would
		// pass on a workspace that set neither. The sample case is 12 m x 4 m x 1 m
		// and cannot be the unit cube.
		TestFalse(
			*FString::Printf(
				TEXT("the domain is the case's real size, not the unconfigured unit default; "
					 "got (%g, %g, %g)"),
				ClipDomain.X, ClipDomain.Y, ClipDomain.Z),
			ClipDomain.Equals(FVector::OneVector, UE_KINDA_SMALL_NUMBER));

		TestTrue(
			TEXT("the domain is positive on every axis"),
			ClipDomain.X > 0.0 && ClipDomain.Y > 0.0 && ClipDomain.Z > 0.0);

		// The probe view model's unit scale must come from the case's units.length
		// rather than being left at the default, or every probe placed by clicking
		// reports a position from the wrong cell on a non-metre case.
		TestTrue(
			TEXT("the probe view model has a positive unit scale"),
			Workspace.Probes.GetMetersToUnrealUnits() > 0.0);

		/* == Field enumeration is real ====================================== */
		const TArray<FName> Fields = Workspace.GetVolumeFieldIds();
		TestTrue(
			*FString::Printf(
				TEXT("the open case lists at least one volume field; listed %d"), Fields.Num()),
			Fields.Num() > 0);
		TestTrue(
			TEXT("the field the player opened is among the listed fields"),
			Fields.Contains(Workspace.Player.GetFieldId()));

		/* == Closing unbinds ================================================ */
		Workspace.CloseCase();
		TestFalse(TEXT("closing leaves no case open"), Workspace.IsCaseOpen());
		TestFalse(
			TEXT("closing unbinds the transfer function, so no stale range survives"),
			Workspace.TransferFunction.IsBound());
		TestFalse(
			TEXT("closing leaves the timeline with no frames to drive"),
			Workspace.Timeline.HasFrames());
	}

	/* == Switching fields re-binds the colouring ============================ */
	{
		FFlowVizWorkspaceModel Workspace;
		if (!Workspace.OpenCase(CaseDir).IsOk())
		{
			AddError(TEXT("could not open the sample case for the field-switch check"));
			return false;
		}

		const TArray<FName> Fields = Workspace.GetVolumeFieldIds();
		// Find a field that is NOT the one already bound, so the switch is a real
		// change rather than a no-op that would pass trivially.
		FName Other = NAME_None;
		for (const FName Candidate : Fields)
		{
			if (Candidate != Workspace.TransferFunction.GetFieldId())
			{
				Other = Candidate;
				break;
			}
		}

		if (Other.IsNone())
		{
			// Report rather than skip: if the sample ever drops to one field this
			// check stops meaning anything and someone must notice.
			AddError(TEXT("the sample case declares only one volume field, so the field-switch "
						  "assertion cannot distinguish a working switch from a no-op"));
			return false;
		}

		const FCFDVizResult SwitchResult = Workspace.SetField(Other);
		TestTrue(
			*FString::Printf(TEXT("switching to field '%s' succeeds: %s"),
				*Other.ToString(), *SwitchResult.ToString()),
			SwitchResult.IsOk());
		TestEqual(
			TEXT("the transfer function follows the field switch, so colouring is not left on "
				 "the previous field's range"),
			Workspace.TransferFunction.GetFieldId(),
			Other);
		TestEqual(
			TEXT("the player follows the field switch too"),
			Workspace.Player.GetFieldId(),
			Other);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
