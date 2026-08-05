// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizClipViewModel.h"

#include "Misc/AutomationTest.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "UI/FlowVizSliceViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Clip planes, the crop box, and the slice plane (plan.md sections 10.4, 10.5, 5F).
 *
 * WHAT IS BEING CHECKED, AND WHAT WOULD BE POINTLESS TO CHECK. Setting a plane
 * and reading it back tests an assignment. Every assertion below is either
 * against the RAY-MARCH CONSTANT BUFFER - the thing the GPU actually reads, via
 * ApplyToRayMarchParameters - or against the sign of SignedDistance for a
 * concrete point, which a wrong convention cannot produce by accident.
 *
 * THE SIGN IS THE WHOLE RISK HERE. `dot(N, LocalPos) + D >= 0` keeps the
 * half-space the normal points INTO. Every plausible way to get this wrong -
 * keeping the other side, negating only N on invert, normalising N without
 * rescaling D - renders a volume that is clipped, looks deliberate, and shows
 * the wrong half. So the tests below name a point that must be KEPT and a point
 * that must be DROPPED for each plane, rather than asserting a stored number.
 *
 * THE SLICE IS CHECKED THROUGH THE CLIP PLANE ON PURPOSE. Nothing renders a
 * slice yet (there is no slice component in this plugin), so a test of the slice
 * view model against itself would be a round trip. MakeClipPlane() converts into
 * a convention whose consumer DOES exist, so the slice's orientation is checked
 * against a real one.
 */

namespace FlowVizClipViewModelTest
{
	/** The shipped sample's domain, in solver metres: 56*0.214.., 28*0.142.., 6*0.166.. */
	const FVector SampleDomain(12.0, 4.0, 1.0);

	/**
	 * True when a packed slot is INERT: the shader may consult it and the volume
	 * is unchanged.
	 *
	 * ALL FOUR COMPONENTS, NOT JUST XYZ, and the difference is not cosmetic. The
	 * shader keeps `dot(N, LocalPos) + D >= 0`. A slot of (0, 0, 0, W) has a zero
	 * normal, so that test collapses to the constant `W >= 0` - and with W < 0 it
	 * DROPS EVERY SAMPLE and the volume disappears entirely. FVector4f::
	 * IsNearlyZero3 ignores W and would call such a slot empty, so a helper built
	 * on it would report "the tail is clear" about a tail that blanks the render.
	 * IsNearlyZero tests all four. See the Consumer test for the fixture that
	 * tells the two apart.
	 *
	 * THE TWO SPELLINGS WERE RUN AGAINST EACH OTHER on the production FVector4f,
	 * not reasoned about: they agree on (0,0,0,0) and on (1,0,0,0), and disagree
	 * on exactly the rows where W alone is non-zero - IsNearlyZero3 calls
	 * (0,0,0,-1) empty, IsNearlyZero does not. So the (0,0,0,-1) assertion below
	 * genuinely fails if this line is changed to IsNearlyZero3; it is not a row
	 * that both spellings happen to pass.
	 *
	 * EXACT ZERO, NOT A TOLERANCE. ApplyToRayMarchParameters writes the unused
	 * tail as literal 0.0f, so anything else in a slot got there from somewhere
	 * and must be counted. IsNearlyZero's default KINDA_SMALL_NUMBER (1e-4) would
	 * swallow a real plane sitting closer than that to the origin.
	 */
	bool IsSlotInert(const FVector4f& Slot)
	{
		return Slot.IsNearlyZero(0.0f);
	}

	/** Count how many of the packed slots carry a plane that would affect the render. */
	int32 CountNonZeroPlanes(const FFlowVizVolumeRayMarchParameters& Parameters)
	{
		int32 Count = 0;
		for (int32 Index = 0; Index < FlowVizRayMarch::MaxClipPlanes; ++Index)
		{
			if (!IsSlotInert(Parameters.ClipPlanes[Index]))
			{
				++Count;
			}
		}
		return Count;
	}
}

/* ========================================================================== */
/* The convention: which half is kept                                         */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipViewModelConventionTest,
	"FlowViz.UI.ClipViewModel.Convention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizClipViewModelConventionTest::RunTest(const FString& Parameters)
{
	/* == SignedDistance is the shader's expression, not a paraphrase ========= */
	{
		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(1.0, 0.0, 0.0);
		Plane.Distance = -3.0;

		// The plane sits at x = 3. dot(N,P) + D at x = 5 is +2, at x = 1 is -2.
		TestEqual(TEXT("SignedDistance is dot(N,P) + D"),
			Plane.SignedDistance(FVector(5.0, 0.0, 0.0)), 2.0);
		TestEqual(TEXT("SignedDistance is negative on the discarded side"),
			Plane.SignedDistance(FVector(1.0, 0.0, 0.0)), -2.0);

		// KEEPS THE SIDE THE NORMAL POINTS INTO. If this were inverted, every
		// clip in the product would show the complementary half of the domain -
		// which looks like a deliberate clip of the wrong region, not like a bug.
		TestTrue(TEXT("+X normal keeps the high-x side"), Plane.Keeps(FVector(5.0, 0.0, 0.0)));
		TestFalse(TEXT("+X normal drops the low-x side"), Plane.Keeps(FVector(1.0, 0.0, 0.0)));

		// A point exactly ON the plane is kept, matching the shader's >=. A strict
		// > would drop a plane's worth of voxels at every axis-aligned preset,
		// which reads as a one-voxel seam rather than as a comparison operator.
		TestTrue(TEXT("a point on the plane is kept, matching the shader's >="),
			Plane.Keeps(FVector(3.0, 0.0, 0.0)));
	}

	/* == Invert negates BOTH N and D ======================================== */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the sample domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(1.0, 0.0, 0.0);
		Plane.Distance = -3.0;
		TestTrue(TEXT("the plane is accepted"), Clip.AddPlane(Plane).IsOk());

		const FVector High(5.0, 2.0, 0.5);
		const FVector Low(1.0, 2.0, 0.5);
		TestTrue(TEXT("before inverting, the high side is kept"), Clip.KeepsPoint(High));
		TestFalse(TEXT("before inverting, the low side is dropped"), Clip.KeepsPoint(Low));

		TestTrue(TEXT("inverting succeeds"), Clip.InvertPlane(0).IsOk());

		/*
		 * THIS IS THE ASSERTION THAT SEPARATES "invert" FROM "negate the normal".
		 *
		 * Negating N alone would give the plane x = -3, which is outside a domain
		 * spanning [0, 12] - so BOTH points would be kept and the volume would
		 * look unclipped. Asserting only that Low is now kept would pass under
		 * that bug. Asserting that High is now DROPPED is what fails it, because
		 * the mirrored plane keeps everything.
		 */
		TestTrue(TEXT("after inverting, the low side is kept"), Clip.KeepsPoint(Low));
		TestFalse(TEXT("after inverting, the high side is dropped - a normal-only "
					   "negation would keep both, since x = -3 is outside the domain"),
			Clip.KeepsPoint(High));

		// The plane did not move: the point ON it is still on it, still kept.
		TestTrue(TEXT("the inverted plane sits in the same place"),
			Clip.KeepsPoint(FVector(3.0, 2.0, 0.5)));
	}

	/* == An unnormalised normal is normalised, and D keeps its meaning ======= */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(4.0, 0.0, 0.0); // Length 4, deliberately.
		Plane.Distance = -3.0;
		TestTrue(TEXT("an unnormalised normal is accepted"), Clip.AddPlane(Plane).IsOk());

		const FFlowVizClipPlane* Stored = Clip.FindPlane(0);
		if (Stored == nullptr)
		{
			AddError(TEXT("the plane was not stored"));
			return false;
		}
		TestTrue(TEXT("the stored normal is unit length"),
			FMath::IsNearlyEqual(Stored->Normal.Size(), 1.0, 1.0e-9));

		// A REAL RISK, NOT A STYLE POINT. If Normalize() scaled N but left D, the
		// plane would move from x = 3 to x = 0.75 - a plane that is still inside
		// the domain and still clips something, so the render looks fine and the
		// numeric readout lies. The keep/drop pair below is what catches it.
		TestTrue(TEXT("normalising the normal does not move the plane: x=5 is still kept"),
			Clip.KeepsPoint(FVector(5.0, 2.0, 0.5)));
		TestFalse(TEXT("normalising the normal does not move the plane: x=1 is still "
					   "dropped, which a rescaled N with an unrescaled D would keep"),
			Clip.KeepsPoint(FVector(1.0, 2.0, 0.5)));
	}

	/* == Multiple planes INTERSECT, they do not union ======================= */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		FFlowVizClipPlane KeepAboveX;
		KeepAboveX.Normal = FVector(1.0, 0.0, 0.0);
		KeepAboveX.Distance = -3.0;
		FFlowVizClipPlane KeepBelowX;
		KeepBelowX.Normal = FVector(-1.0, 0.0, 0.0);
		KeepBelowX.Distance = 9.0; // Keeps x <= 9.
		TestTrue(TEXT("the first plane is accepted"), Clip.AddPlane(KeepAboveX).IsOk());
		TestTrue(TEXT("the second plane is accepted"), Clip.AddPlane(KeepBelowX).IsOk());

		TestTrue(TEXT("a point in the slab both planes keep is kept"),
			Clip.KeepsPoint(FVector(6.0, 2.0, 0.5)));
		// A UNION would keep these: each is kept by one plane and dropped by the
		// other. Intersection is what makes a two-plane slab mean a slab.
		TestFalse(TEXT("a point below the slab is dropped - a union would keep it"),
			Clip.KeepsPoint(FVector(1.0, 2.0, 0.5)));
		TestFalse(TEXT("a point above the slab is dropped - a union would keep it"),
			Clip.KeepsPoint(FVector(11.0, 2.0, 0.5)));
	}

	/* == A disabled plane stops clipping but is not deleted ================= */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(1.0, 0.0, 0.0);
		Plane.Distance = -3.0;
		TestTrue(TEXT("the plane is accepted"), Clip.AddPlane(Plane).IsOk());

		const FVector Low(1.0, 2.0, 0.5);
		TestFalse(TEXT("the enabled plane drops the low side"), Clip.KeepsPoint(Low));

		TestTrue(TEXT("disabling succeeds"), Clip.SetPlaneEnabled(0, false).IsOk());
		TestTrue(TEXT("a disabled plane clips nothing"), Clip.KeepsPoint(Low));
		TestEqual(TEXT("a disabled plane is retained in the list, not deleted"),
			Clip.GetPlaneCount(), 1);
		TestEqual(TEXT("but it does not count as enabled"), Clip.GetEnabledPlaneCount(), 0);

		TestTrue(TEXT("re-enabling succeeds"), Clip.SetPlaneEnabled(0, true).IsOk());
		TestFalse(TEXT("the re-enabled plane clips again, from where it was"),
			Clip.KeepsPoint(Low));
	}

	return true;
}

/* ========================================================================== */
/* Presets, limits, and refusals                                              */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipViewModelLimitsTest,
	"FlowViz.UI.ClipViewModel.Limits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizClipViewModelLimitsTest::RunTest(const FString& Parameters)
{
	/* == Every preset keeps the half it is named after ====================== */
	{
		// A SIGN TABLE IS SIX CHANCES TO GET ONE SIGN WRONG, and five right ones
		// make the sixth look like a data problem. All six are enumerated.
		const FVector Centre = FlowVizClipViewModelTest::SampleDomain * 0.5;

		struct FPresetCase
		{
			EFlowVizClipPreset Preset;
			FVector Kept;
			FVector Dropped;
			const TCHAR* Name;
		};
		const FPresetCase Cases[] = {
			{EFlowVizClipPreset::KeepPlusX, FVector(11.0, 2.0, 0.5), FVector(1.0, 2.0, 0.5), TEXT("KeepPlusX")},
			{EFlowVizClipPreset::KeepMinusX, FVector(1.0, 2.0, 0.5), FVector(11.0, 2.0, 0.5), TEXT("KeepMinusX")},
			{EFlowVizClipPreset::KeepPlusY, FVector(6.0, 3.5, 0.5), FVector(6.0, 0.5, 0.5), TEXT("KeepPlusY")},
			{EFlowVizClipPreset::KeepMinusY, FVector(6.0, 0.5, 0.5), FVector(6.0, 3.5, 0.5), TEXT("KeepMinusY")},
			{EFlowVizClipPreset::KeepPlusZ, FVector(6.0, 2.0, 0.9), FVector(6.0, 2.0, 0.1), TEXT("KeepPlusZ")},
			{EFlowVizClipPreset::KeepMinusZ, FVector(6.0, 2.0, 0.1), FVector(6.0, 2.0, 0.9), TEXT("KeepMinusZ")},
		};

		for (const FPresetCase& Case : Cases)
		{
			FFlowVizClipViewModel Clip;
			TestTrue(TEXT("the domain is accepted"),
				Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
			TestTrue(FString::Printf(TEXT("%s is accepted"), Case.Name),
				Clip.AddPresetPlane(Case.Preset).IsOk());

			TestTrue(FString::Printf(TEXT("%s keeps the side it names"), Case.Name),
				Clip.KeepsPoint(Case.Kept));
			TestFalse(FString::Printf(TEXT("%s drops the other side"), Case.Name),
				Clip.KeepsPoint(Case.Dropped));
			// Through the centre, so the centre is on the plane and therefore kept.
			TestTrue(FString::Printf(TEXT("%s passes through the domain centre"), Case.Name),
				Clip.KeepsPoint(Centre));
		}
	}

	/* == A preset with no domain is refused ================================= */
	{
		FFlowVizClipViewModel Clip;
		// Without a domain there is no centre. A preset that quietly used the
		// origin would put the plane on the domain's minimum CORNER, clipping the
		// entire volume away - which reads as a failed load.
		TestFalse(TEXT("a preset without a domain is refused"),
			Clip.AddPresetPlane(EFlowVizClipPreset::KeepPlusX).IsOk());
		TestEqual(TEXT("and nothing is added"), Clip.GetPlaneCount(), 0);
	}

	/* == The seventh plane is REFUSED, not dropped ========================== */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		for (int32 Index = 0; Index < FlowVizRayMarch::MaxClipPlanes; ++Index)
		{
			FFlowVizClipPlane Plane;
			Plane.Normal = FVector(1.0, 0.0, 0.0);
			Plane.Distance = -static_cast<double>(Index);
			TestTrue(FString::Printf(TEXT("plane %d fits"), Index), Clip.AddPlane(Plane).IsOk());
			TestEqual(FString::Printf(TEXT("plane %d was added"), Index),
				Clip.GetPlaneCount(), Index + 1);
		}

		TestFalse(TEXT("with the array full, CanAddPlane is false - which is what "
					   "greys out the add button rather than letting it fail"),
			Clip.CanAddPlane());

		FFlowVizClipPlane Seventh;
		Seventh.Normal = FVector(0.0, 1.0, 0.0);
		// REFUSED, NOT SILENTLY DROPPED. A dropped plane appears in the list, is
		// not in the shader, and clips nothing - a control that looks like it
		// worked (rule 15).
		TestFalse(TEXT("a seventh plane is refused"), Clip.AddPlane(Seventh).IsOk());
		TestEqual(TEXT("and the list still holds exactly MaxClipPlanes"),
			Clip.GetPlaneCount(), FlowVizRayMarch::MaxClipPlanes);
	}

	/* == Degenerate planes are refused ====================================== */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		FFlowVizClipPlane ZeroNormal;
		ZeroNormal.Normal = FVector::ZeroVector;
		TestFalse(TEXT("a zero-length normal is refused - it makes dot(N,P)+D a "
					   "constant, so the plane keeps everything or nothing"),
			Clip.AddPlane(ZeroNormal).IsOk());

		FFlowVizClipPlane NaNNormal;
		NaNNormal.Normal = FVector(NAN, 0.0, 0.0);
		TestFalse(TEXT("a NaN normal is refused"), Clip.AddPlane(NaNNormal).IsOk());

		FFlowVizClipPlane NaNDistance;
		NaNDistance.Normal = FVector(1.0, 0.0, 0.0);
		NaNDistance.Distance = NAN;
		// A NaN D makes every comparison false, so the volume vanishes entirely.
		TestFalse(TEXT("a NaN distance is refused"), Clip.AddPlane(NaNDistance).IsOk());

		TestEqual(TEXT("no degenerate plane was stored"), Clip.GetPlaneCount(), 0);
	}

	/* == A degenerate domain is refused ===================================== */
	{
		FFlowVizClipViewModel Clip;
		// Zero on any axis makes the crop normalisation a division by zero.
		TestFalse(TEXT("a zero-extent domain is refused"),
			Clip.SetDomainSize(FVector(12.0, 0.0, 1.0)).IsOk());
		TestFalse(TEXT("a negative-extent domain is refused"),
			Clip.SetDomainSize(FVector(12.0, -4.0, 1.0)).IsOk());
		TestFalse(TEXT("a non-finite domain is refused"),
			Clip.SetDomainSize(FVector(12.0, NAN, 1.0)).IsOk());
		TestFalse(TEXT("and none of those left a domain behind"), Clip.HasDomain());
	}

	return true;
}

/* ========================================================================== */
/* What the ray-marcher receives                                              */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipViewModelConsumerTest,
	"FlowViz.UI.ClipViewModel.Consumer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizClipViewModelConsumerTest::RunTest(const FString& Parameters)
{
	/* == A plane reaches the constant buffer in the shader's own form ======== */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(1.0, 0.0, 0.0);
		Plane.Distance = -3.0;
		TestTrue(TEXT("the plane is accepted"), Clip.AddPlane(Plane).IsOk());

		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		TestTrue(TEXT("applying to the ray-march block succeeds"),
			Clip.ApplyToRayMarchParameters(Params).IsOk());

		TestEqual(TEXT("the shader is told there is one plane"), Params.NumClipPlanes, 1u);

		/*
		 * THE PACKING IS xyz = N, w = D, WHICH IS WHAT MAKES dot(plane.xyz, P) +
		 * plane.w THE SAME TEST THE CPU RAN. Asserting the four floats is the
		 * only way to catch a swap - a w/x transposition still produces a plane,
		 * still clips, and shows the wrong region.
		 */
		TestEqual(TEXT("the packed normal x reaches the shader"), Params.ClipPlanes[0].X, 1.0f);
		TestEqual(TEXT("the packed normal y reaches the shader"), Params.ClipPlanes[0].Y, 0.0f);
		TestEqual(TEXT("the packed normal z reaches the shader"), Params.ClipPlanes[0].Z, 0.0f);
		TestEqual(TEXT("the packed distance reaches the shader in w"),
			Params.ClipPlanes[0].W, -3.0f);

		// The CPU predicate and the packed plane must agree, or a probe readout
		// would report "inside" for a voxel the render clipped away.
		const FVector Probe(5.0, 2.0, 0.5);
		const float Packed = Params.ClipPlanes[0].X * static_cast<float>(Probe.X)
			+ Params.ClipPlanes[0].Y * static_cast<float>(Probe.Y)
			+ Params.ClipPlanes[0].Z * static_cast<float>(Probe.Z)
			+ Params.ClipPlanes[0].W;
		TestEqual(TEXT("the packed plane and KeepsPoint agree on which side a point is"),
			Packed >= 0.0f, Clip.KeepsPoint(Probe));
	}

	/* == Disabled planes are omitted and the tail is ZEROED ================= */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		for (int32 Index = 0; Index < 3; ++Index)
		{
			FFlowVizClipPlane Plane;
			Plane.Normal = FVector(1.0, 0.0, 0.0);
			Plane.Distance = -1.0 - static_cast<double>(Index);
			TestTrue(TEXT("a plane is accepted"), Clip.AddPlane(Plane).IsOk());
		}

		FFlowVizVolumeRayMarchParameters Full;
		FlowVizRayMarch::FillDefaults(Full);
		TestTrue(TEXT("applying three planes succeeds"),
			Clip.ApplyToRayMarchParameters(Full).IsOk());
		TestEqual(TEXT("three planes reach the shader"), Full.NumClipPlanes, 3u);
		TestEqual(TEXT("and exactly three slots are non-zero"),
			FlowVizClipViewModelTest::CountNonZeroPlanes(Full), 3);

		// Disable the MIDDLE one, so a naive implementation that only lowered the
		// count would leave the wrong plane in slot 1.
		TestTrue(TEXT("disabling the middle plane succeeds"),
			Clip.SetPlaneEnabled(1, false).IsOk());

		FFlowVizVolumeRayMarchParameters Reduced;
		FlowVizRayMarch::FillDefaults(Reduced);
		TestTrue(TEXT("re-applying succeeds"),
			Clip.ApplyToRayMarchParameters(Reduced).IsOk());
		TestEqual(TEXT("two planes now reach the shader"), Reduced.NumClipPlanes, 2u);

		// The surviving planes are COMPACTED to the front, not left with a hole.
		TestEqual(TEXT("the enabled planes are compacted to the front: slot 0 is the first"),
			Reduced.ClipPlanes[0].W, -1.0f);
		TestEqual(TEXT("slot 1 is the THIRD plane, not the disabled second"),
			Reduced.ClipPlanes[1].W, -3.0f);

		/*
		 * THE ZEROED TAIL IS NOT TIDINESS. A stale plane sitting in slot 2 while
		 * NumClipPlanes says 2 is invisible - until some later frame raises the
		 * count and a plane the user removed reappears. That is a bug that
		 * manifests one interaction after its cause.
		 */
		TestEqual(TEXT("the unused tail is zeroed, so a later count raise cannot "
					   "resurrect a removed plane"),
			FlowVizClipViewModelTest::CountNonZeroPlanes(Reduced), 2);

		/*
		 * WHAT "ZEROED" HAS TO MEAN - the fixture that separates the two available
		 * zero-tests, asserted on the discriminator ITSELF.
		 *
		 * It is checked directly rather than by driving the view model, because
		 * AddPlane REFUSES a degenerate normal: there is no way to get (0,0,0,W)
		 * into a slot through the public API, so a test that went that route would
		 * be blocked by the guard and would prove nothing about the helper every
		 * tail assertion above depends on.
		 *
		 * A slot of (0, 0, 0, -1) makes the shader's `dot(N,P) + D >= 0` collapse
		 * to `-1 >= 0`, which is false for every sample: the volume vanishes. That
		 * is the single most destructive thing a "cleared" slot can contain, so a
		 * helper that called it clear would be reporting the opposite of the truth.
		 */
		TestTrue(TEXT("an all-zero slot is inert - the shader's test collapses to "
					  "0 >= 0, which keeps every sample"),
			FlowVizClipViewModelTest::IsSlotInert(FVector4f(0.0f, 0.0f, 0.0f, 0.0f)));
		TestFalse(TEXT("a slot with a zero normal but W = -1 is NOT inert: it drops "
					   "every sample and blanks the volume, so IsNearlyZero3 - which "
					   "ignores W - would be the wrong test here"),
			FlowVizClipViewModelTest::IsSlotInert(FVector4f(0.0f, 0.0f, 0.0f, -1.0f)));
		TestFalse(TEXT("and a real plane through the origin (W = 0) is counted, so "
					   "the helper cannot be satisfied by W alone"),
			FlowVizClipViewModelTest::IsSlotInert(FVector4f(1.0f, 0.0f, 0.0f, 0.0f)));
	}

	/* == The crop box is normalised on the way out, and only there =========== */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		// SOLVER UNITS IN. Half the domain in x, the middle half in y.
		TestTrue(TEXT("a crop box in solver units is accepted"),
			Clip.SetCropBox(FVector(3.0, 1.0, 0.0), FVector(9.0, 3.0, 1.0)).IsOk());

		// The stored value is what the user typed - not a fraction. A view model
		// that normalised on the way IN would show 0.25 on a numeric panel
		// labelled metres.
		TestEqual(TEXT("the crop is stored in the solver units it was typed in"),
			Clip.GetCropMin().X, 3.0);

		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		TestTrue(TEXT("applying succeeds"), Clip.ApplyToRayMarchParameters(Params).IsOk());

		// FRACTIONS OUT: 3/12, 1/4, 0/1 and 9/12, 3/4, 1/1.
		TestEqual(TEXT("crop min x normalises by the domain extent"), Params.CropBoxMin.X, 0.25f);
		TestEqual(TEXT("crop min y normalises by ITS OWN axis extent, not x's"),
			Params.CropBoxMin.Y, 0.25f);
		TestEqual(TEXT("crop min z normalises by its own axis extent"), Params.CropBoxMin.Z, 0.0f);
		TestEqual(TEXT("crop max x normalises by the domain extent"), Params.CropBoxMax.X, 0.75f);
		TestEqual(TEXT("crop max y normalises by its own axis extent"), Params.CropBoxMax.Y, 0.75f);
		TestEqual(TEXT("crop max z normalises by its own axis extent"), Params.CropBoxMax.Z, 1.0f);

		/*
		 * A PER-AXIS CHECK, DELIBERATELY. The sample domain is 12 x 4 x 1, so a
		 * normalisation that divided every axis by DomainSize.X would give y =
		 * 1/12 = 0.083 instead of 0.25 - a crop that is wrong only in y, on a
		 * domain that is much longer than it is tall. On a cube fixture that bug
		 * is invisible. This is why the fixture is not a cube.
		 */
	}

	/* == Reset restores the whole domain ==================================== */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestFalse(TEXT("a fresh view model has no active crop"), Clip.IsCropActive());

		TestTrue(TEXT("a crop is accepted"),
			Clip.SetCropBox(FVector(3.0, 1.0, 0.0), FVector(9.0, 3.0, 1.0)).IsOk());
		TestTrue(TEXT("that crop is active"), Clip.IsCropActive());

		Clip.ResetCropBox();
		TestFalse(TEXT("after reset the crop is inactive again"), Clip.IsCropActive());

		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		TestTrue(TEXT("applying succeeds"), Clip.ApplyToRayMarchParameters(Params).IsOk());
		TestEqual(TEXT("a reset crop reaches the shader as the full 0..1 box"),
			Params.CropBoxMin, FVector3f::ZeroVector);
		TestEqual(TEXT("a reset crop reaches the shader as the full 0..1 box"),
			Params.CropBoxMax, FVector3f::OneVector);
	}

	/* == An inverted crop is refused, and the previous one survives ========== */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestTrue(TEXT("a good crop is accepted"),
			Clip.SetCropBox(FVector(3.0, 1.0, 0.0), FVector(9.0, 3.0, 1.0)).IsOk());

		TestFalse(TEXT("an inverted crop is refused - it selects nothing, which "
					   "renders as an empty volume rather than as an error"),
			Clip.SetCropBox(FVector(9.0, 1.0, 0.0), FVector(3.0, 3.0, 1.0)).IsOk());
		TestEqual(TEXT("the refused crop did not overwrite the good one"),
			Clip.GetCropMin().X, 3.0);

		TestFalse(TEXT("a non-finite crop is refused"),
			Clip.SetCropBox(FVector(NAN, 1.0, 0.0), FVector(9.0, 3.0, 1.0)).IsOk());
		TestEqual(TEXT("the refused crop did not overwrite the good one"),
			Clip.GetCropMin().X, 3.0);
	}

	/* == A crop wider than the domain is LEGAL and is not clamped ============ */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		// Selecting everything is meaningful; clamping would silently rewrite
		// what the user typed into the numeric fields.
		TestTrue(TEXT("a crop wider than the domain is accepted"),
			Clip.SetCropBox(FVector(-1.0, -1.0, -1.0), FVector(13.0, 5.0, 2.0)).IsOk());
		TestEqual(TEXT("and is stored exactly as typed, not clamped to the domain"),
			Clip.GetCropMin().X, -1.0);
	}

	/* == Applying with no domain is refused, and writes NOTHING ============== */
	{
		FFlowVizClipViewModel Clip;

		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		Params.NumClipPlanes = 4u; // A sentinel the call must not disturb.

		TestFalse(TEXT("applying without a domain is refused - there is no extent "
					   "to normalise the crop by"),
			Clip.ApplyToRayMarchParameters(Params).IsOk());
		// VALIDATE-THEN-WRITE, not write-then-fail. A partial write would leave
		// the constant buffer describing a scene nobody asked for.
		TestEqual(TEXT("a refused apply leaves the parameter block untouched"),
			Params.NumClipPlanes, 4u);
	}

	return true;
}

/* ========================================================================== */
/* The slice, checked through the one consumer it has                         */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSliceViewModelPlaneTest,
	"FlowViz.UI.SliceViewModel.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSliceViewModelPlaneTest::RunTest(const FString& Parameters)
{
	/* == MakeClipPlane agrees with SignedDistance's sign ==================== */
	{
		FFlowVizSliceViewModel Slice;
		TestTrue(TEXT("the domain is accepted"),
			Slice.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestTrue(TEXT("an origin is accepted"), Slice.SetOrigin(FVector(6.0, 2.0, 0.5)).IsOk());
		TestTrue(TEXT("a +Z normal is accepted"), Slice.SetNormal(FVector(0.0, 0.0, 1.0)).IsOk());

		const FVector Above(6.0, 2.0, 0.9);
		const FVector Below(6.0, 2.0, 0.1);

		TestTrue(TEXT("SignedDistance is positive on the normal's side"),
			Slice.SignedDistance(Above) > 0.0);
		TestTrue(TEXT("SignedDistance is negative on the other side"),
			Slice.SignedDistance(Below) < 0.0);
		TestTrue(TEXT("SignedDistance is zero on the plane"),
			FMath::IsNearlyZero(Slice.SignedDistance(FVector(6.0, 2.0, 0.5))));

		/*
		 * THE CONVERSION IS THE POINT. Nothing renders a slice, so this is the
		 * only assertion in the file that can catch the slice's orientation being
		 * wrong: the emitted clip plane must keep the same half-space the slice's
		 * own SignedDistance calls positive. If MakeClipPlane forgot the negation
		 * in D = -dot(N, Origin), the plane would sit at the domain's origin
		 * instead of at the slice - still a plane, still through the volume.
		 */
		const FFlowVizClipPlane Emitted = Slice.MakeClipPlane();
		TestTrue(TEXT("the emitted plane is valid"), Emitted.IsValid());
		TestTrue(TEXT("the emitted clip plane keeps the slice's positive side"),
			Emitted.Keeps(Above));
		TestFalse(TEXT("the emitted clip plane drops the slice's negative side"),
			Emitted.Keeps(Below));
		TestTrue(TEXT("a point on the slice is kept, matching the shader's >="),
			Emitted.Keeps(FVector(6.0, 2.0, 0.5)));

		// The two expressions are the SAME number, not merely the same sign -
		// which is what pins D = -dot(N, Origin) rather than any offset that
		// happens to have the right sign at these two points.
		TestTrue(TEXT("the emitted plane's signed distance equals the slice's"),
			FMath::IsNearlyEqual(
				Emitted.SignedDistance(Above), Slice.SignedDistance(Above), 1.0e-9));
	}

	/* == The clip plane travels with the slice, and through a real consumer == */
	{
		FFlowVizSliceViewModel Slice;
		TestTrue(TEXT("the domain is accepted"),
			Slice.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestTrue(TEXT("a +X normal is accepted"), Slice.SetNormal(FVector(1.0, 0.0, 0.0)).IsOk());
		TestTrue(TEXT("an origin is accepted"), Slice.SetOrigin(FVector(3.0, 2.0, 0.5)).IsOk());

		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"),
			Clip.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestTrue(TEXT("the slice's plane is accepted by the clip view model"),
			Clip.AddPlane(Slice.MakeClipPlane()).IsOk());

		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		TestTrue(TEXT("it reaches the ray-march block"),
			Clip.ApplyToRayMarchParameters(Params).IsOk());

		// D = -dot(N, Origin) = -3 for a +X normal through x = 3. This is the
		// slice's position arriving at the GPU, in the GPU's own form.
		TestEqual(TEXT("the slice's position reaches the shader as w = -dot(N, Origin)"),
			Params.ClipPlanes[0].W, -3.0f);

		// Move the slice; the emitted plane must move with it.
		TestTrue(TEXT("moving the slice succeeds"),
			Slice.SetOrigin(FVector(9.0, 2.0, 0.5)).IsOk());
		FFlowVizClipViewModel Moved;
		TestTrue(TEXT("the domain is accepted"),
			Moved.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestTrue(TEXT("the moved plane is accepted"), Moved.AddPlane(Slice.MakeClipPlane()).IsOk());

		FFlowVizVolumeRayMarchParameters MovedParams;
		FlowVizRayMarch::FillDefaults(MovedParams);
		TestTrue(TEXT("it reaches the ray-march block"),
			Moved.ApplyToRayMarchParameters(MovedParams).IsOk());
		TestEqual(TEXT("the moved slice reaches the shader at its new position"),
			MovedParams.ClipPlanes[0].W, -9.0f);
	}

	/* == The normalized position uses the PROJECTED extent =================== */
	{
		FFlowVizSliceViewModel Slice;
		TestTrue(TEXT("the domain is accepted"),
			Slice.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestTrue(TEXT("a +X normal is accepted"), Slice.SetNormal(FVector(1.0, 0.0, 0.0)).IsOk());

		TestTrue(TEXT("mid-travel is accepted"), Slice.SetNormalizedPosition(0.5).IsOk());
		TestTrue(TEXT("half way along x is x = 6 on a 12 m domain"),
			FMath::IsNearlyEqual(Slice.GetOrigin().X, 6.0, 1.0e-9));
		TestTrue(TEXT("and it reads back as the fraction that was set"),
			FMath::IsNearlyEqual(Slice.GetNormalizedPosition(), 0.5, 1.0e-9));

		TestTrue(TEXT("the low end is accepted"), Slice.SetNormalizedPosition(0.0).IsOk());
		TestTrue(TEXT("0 puts the slice at the domain's low face"),
			FMath::IsNearlyEqual(Slice.GetOrigin().X, 0.0, 1.0e-9));
		TestTrue(TEXT("the high end is accepted"), Slice.SetNormalizedPosition(1.0).IsOk());
		TestTrue(TEXT("1 puts the slice at the domain's high face"),
			FMath::IsNearlyEqual(Slice.GetOrigin().X, 12.0, 1.0e-9));

		/*
		 * THE OBLIQUE CASE IS WHY THE EXTENT IS PROJECTED RATHER THAN AXIAL.
		 *
		 * For N = (1,1,0)/sqrt(2) on a 12 x 4 x 1 domain, the projected extent is
		 * (12 + 4)/sqrt(2) = 11.3137... Picking the DOMINANT AXIS instead would
		 * use 12, and the slider would run out of travel before reaching the far
		 * corner - the slice would stop moving partway across the box while the
		 * slider kept going, which reads as a stuck gizmo.
		 *
		 * The check below is on a quantity the dominant-axis form cannot produce:
		 * at fraction 1 the slice must pass through the domain's FAR CORNER.
		 */
		FFlowVizSliceViewModel Oblique;
		TestTrue(TEXT("the domain is accepted"),
			Oblique.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestTrue(TEXT("an oblique normal is accepted"),
			Oblique.SetNormal(FVector(1.0, 1.0, 0.0)).IsOk());

		TestTrue(TEXT("the low end is accepted"), Oblique.SetNormalizedPosition(0.0).IsOk());
		TestTrue(TEXT("at fraction 0 an oblique slice passes through the near corner"),
			FMath::IsNearlyZero(Oblique.SignedDistance(FVector(0.0, 0.0, 0.5)), 1.0e-9));

		TestTrue(TEXT("the high end is accepted"), Oblique.SetNormalizedPosition(1.0).IsOk());
		TestTrue(TEXT("at fraction 1 an oblique slice passes through the FAR corner - "
					  "a dominant-axis extent would stop short of it"),
			FMath::IsNearlyZero(Oblique.SignedDistance(FVector(12.0, 4.0, 0.5)), 1.0e-9));
	}

	/* == The slab controls are coupled to the thickness ===================== */
	{
		FFlowVizSliceViewModel Slice;
		TestTrue(TEXT("the domain is accepted"),
			Slice.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());

		TestFalse(TEXT("a fresh slice is not a slab"), Slice.IsSlab());
		TestEqual(TEXT("and its aggregation is None"), Slice.GetSlabOp(), EFlowVizSlabOp::None);

		// RULE 15: a sample count on a zero-thickness slice is a control that
		// does nothing, so it is refused rather than accepted and ignored.
		TestFalse(TEXT("a slab sample count is refused on a zero-thickness slice"),
			Slice.SetSlabSamples(8).IsOk());
		TestFalse(TEXT("an aggregation is refused on a zero-thickness slice"),
			Slice.SetSlabOp(EFlowVizSlabOp::Average).IsOk());

		TestTrue(TEXT("a thickness is accepted"), Slice.SetThickness(0.25).IsOk());
		TestTrue(TEXT("the slice is now a slab"), Slice.IsSlab());

		// GROWING FROM ZERO MUST LEAVE A USABLE SLAB. A thick slab still reporting
		// SlabOp::None and one sample would render exactly like the plane it just
		// stopped being - a control that appears inert.
		TestNotEqual(TEXT("growing a slab adopts a real aggregation rather than "
						  "leaving None, which would render identically to a plane"),
			Slice.GetSlabOp(), EFlowVizSlabOp::None);
		TestTrue(TEXT("and at least two samples, or the slab is sampled like a plane"),
			Slice.GetSlabSamples() >= 2);

		TestTrue(TEXT("a sample count is now accepted"), Slice.SetSlabSamples(8).IsOk());
		TestEqual(TEXT("and it took"), Slice.GetSlabSamples(), 8);
		TestFalse(TEXT("one sample is refused on a slab - it is a plane wearing a "
					   "slab's controls"),
			Slice.SetSlabSamples(1).IsOk());
		TestEqual(TEXT("the refused count did not overwrite the good one"),
			Slice.GetSlabSamples(), 8);

		TestTrue(TEXT("Minimum is accepted on a slab"),
			Slice.SetSlabOp(EFlowVizSlabOp::Minimum).IsOk());
		TestFalse(TEXT("clearing the aggregation to None is refused on a slab - the "
					   "slab would have no rule for collapsing its samples"),
			Slice.SetSlabOp(EFlowVizSlabOp::None).IsOk());

		// COLLAPSING BACK TO ZERO MUST CLEAN UP. Leaving Minimum and 8 samples on
		// a zero-thickness slice would leave two live-looking controls that
		// cannot affect anything.
		TestTrue(TEXT("collapsing the thickness is accepted"), Slice.SetThickness(0.0).IsOk());
		TestFalse(TEXT("the slice is a plane again"), Slice.IsSlab());
		TestEqual(TEXT("collapsing clears the aggregation"),
			Slice.GetSlabOp(), EFlowVizSlabOp::None);
		TestEqual(TEXT("and drops back to a single sample"), Slice.GetSlabSamples(), 1);
	}

	/* == Refusals leave the previous plane intact =========================== */
	{
		FFlowVizSliceViewModel Slice;
		TestTrue(TEXT("the domain is accepted"),
			Slice.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestTrue(TEXT("a normal is accepted"), Slice.SetNormal(FVector(1.0, 0.0, 0.0)).IsOk());
		TestTrue(TEXT("an origin is accepted"), Slice.SetOrigin(FVector(6.0, 2.0, 0.5)).IsOk());

		TestFalse(TEXT("a zero-length normal is refused"),
			Slice.SetNormal(FVector::ZeroVector).IsOk());
		TestEqual(TEXT("the refused normal did not overwrite the good one"),
			Slice.GetNormal(), FVector(1.0, 0.0, 0.0));

		TestFalse(TEXT("a non-finite normal is refused"),
			Slice.SetNormal(FVector(NAN, 0.0, 0.0)).IsOk());
		TestEqual(TEXT("the refused normal did not overwrite the good one"),
			Slice.GetNormal(), FVector(1.0, 0.0, 0.0));

		TestFalse(TEXT("a non-finite origin is refused"),
			Slice.SetOrigin(FVector(0.0, NAN, 0.0)).IsOk());
		TestEqual(TEXT("the refused origin did not overwrite the good one"),
			Slice.GetOrigin(), FVector(6.0, 2.0, 0.5));

		TestFalse(TEXT("a negative thickness is refused"), Slice.SetThickness(-1.0).IsOk());
		TestFalse(TEXT("a non-finite thickness is refused"), Slice.SetThickness(NAN).IsOk());

		// An origin OUTSIDE the domain is legal: an empty slice is a state, not an
		// error, and clamping would fight the user's numeric entry.
		TestTrue(TEXT("an origin outside the domain is accepted - an empty slice is "
					  "a state, not an error"),
			Slice.SetOrigin(FVector(20.0, 2.0, 0.5)).IsOk());
	}

	/* == Axis presets orient the plane, without moving the origin ============ */
	{
		FFlowVizSliceViewModel Slice;
		TestTrue(TEXT("the domain is accepted"),
			Slice.SetDomainSize(FlowVizClipViewModelTest::SampleDomain).IsOk());
		TestTrue(TEXT("an origin is accepted"), Slice.SetOrigin(FVector(6.0, 2.0, 0.5)).IsOk());

		TestTrue(TEXT("the X preset is accepted"),
			Slice.SetAxisPreset(EFlowVizSliceAxis::X).IsOk());
		TestEqual(TEXT("the X preset faces +X"), Slice.GetNormal(), FVector(1.0, 0.0, 0.0));
		TestEqual(TEXT("and leaves the origin where it was"),
			Slice.GetOrigin(), FVector(6.0, 2.0, 0.5));

		TestTrue(TEXT("the Y preset is accepted"),
			Slice.SetAxisPreset(EFlowVizSliceAxis::Y).IsOk());
		TestEqual(TEXT("the Y preset faces +Y"), Slice.GetNormal(), FVector(0.0, 1.0, 0.0));

		TestTrue(TEXT("the Z preset is accepted"),
			Slice.SetAxisPreset(EFlowVizSliceAxis::Z).IsOk());
		TestEqual(TEXT("the Z preset faces +Z"), Slice.GetNormal(), FVector(0.0, 0.0, 1.0));

		TestTrue(TEXT("centring is accepted"), Slice.CenterOnDomain().IsOk());
		TestEqual(TEXT("centring puts the origin at the domain centre"),
			Slice.GetOrigin(), FVector(6.0, 2.0, 0.5));
		TestEqual(TEXT("and leaves the normal alone"),
			Slice.GetNormal(), FVector(0.0, 0.0, 1.0));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
