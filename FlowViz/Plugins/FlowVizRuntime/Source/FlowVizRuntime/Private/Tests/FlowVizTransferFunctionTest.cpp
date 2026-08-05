// Copyright FlowViz contributors. All Rights Reserved.

#include "Render/FlowVizTransferFunction.h"

#include "CFDViz/CFDVizColorMaps.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The transfer function: colour table + opacity curve -> an RGBA LUT, and a
 * solver value -> a LUT coordinate (plan.md section 10.3, VISUAL_QA rules 4, 6,
 * 7 and 11).
 *
 * WHY THESE ASSERTIONS AND NOT OTHERS. Every failure this layer can produce is a
 * failure that still renders a plausible image. A half-texel shift renders the
 * whole field in slightly wrong colours; a clamped out-of-range value renders as
 * the colormap's minimum and reads as real data at the bottom of the scale; a
 * diverging map centred on the data midpoint renders white at a value that means
 * nothing. None crash, and none are visible without knowing the answer already.
 * So:
 *
 *  1. EXPECTED VALUES COME FROM THE PYTHON SOURCE OF TRUTH, NOT FROM THIS CODE.
 *     Every number tagged GENERATED below was produced by running
 *     Tools/cfdviz/src/cfdviz/colormaps.py - `build_lut(name, size)` and
 *     `sample_colormap(name, t)` - and pasting the result. A test that asserted
 *     BuildLut agrees with Sample would be a restatement of the implementation
 *     and would pass against any self-consistent mistake, including a uniformly
 *     shifted one.
 *
 *  2. THE PIXEL-CENTRE PROBES ARE CHOSEN WHERE THE TWO CONVENTIONS DISAGREE.
 *     This is the subtle part. At the CENTRE of a colormap `(i+0.5)/N` and
 *     `i/(N-1)` differ by less than 1e-4 - below this test's tolerance - so a
 *     probe at entry 127 of 256 CANNOT distinguish them and a test built only
 *     from centre probes would pass against the bug it exists to catch. The two
 *     conventions diverge at the ENDS, and by more the smaller the LUT:
 *
 *         viridis N=64,  i=63:  correct 0.947854  wrong 0.993248   (0.0454 apart)
 *         viridis N=256, i=255: correct 0.981899  wrong 0.993248   (0.0113 apart)
 *         viridis N=64,  i=31:  correct 0.166321  wrong 0.166363   (0.00004 apart)
 *
 *     The probes below are the end entries, where the gap is 10x to 450x the
 *     tolerance. The `TfPixelCentreProbes` table carries BOTH values so the test
 *     asserts the gap is real before asserting which side of it we are on - a
 *     probe whose two conventions agree is not evidence and is failed as such.
 *     (Repo memory: a pass criterion that cannot fail is not a check.)
 *
 *  3. THE FIXTURE IS NOT SYMMETRIC. Viridis is monotone in no channel, has no
 *     symmetry about its midpoint, and its endpoints differ in every channel - so
 *     a reversed, mirrored or half-shifted LUT is distinguishable from a correct
 *     one at the probes used. Coolwarm is the diverging fixture and IS symmetric
 *     in R/B about 0.5, which is why the rule-7 assertions probe the domain
 *     mapping rather than the colour: for a symmetric map the wrong centre still
 *     produces a plausible colour, so the coordinate is what has to be checked.
 *     (Repo memory: degenerate data defeats assertions.)
 *
 *  4. THE OUT-OF-RANGE ASSERTIONS CHECK THE COORDINATE, NOT ONLY THE CLASS. A
 *     mapping that returned the right EFlowVizValueClass but a clamped
 *     coordinate would satisfy a class-only test while making it impossible for
 *     the shader to tell how far outside the value was. Both halves are asserted.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionTest,
	"FlowViz.Render.TransferFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

/**
 * The RHI half. Needs a real device, so it is a separate test that SKIPS WITH A
 * LOGGED REASON under -nullrhi rather than passing silently - a skipped check
 * that reports success is indistinguishable from a real one afterwards.
 *
 * Run it with:  RHI=1 Tools/build_lock.sh ./Tools/run_tests.sh FlowViz.Render
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionDeviceTest,
	"FlowViz.Render.TransferFunctionDevice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace
{
	/**
	 * Well below 8-bit quantisation (1/255 = 0.0039), so real divergence from the
	 * Python tables fails while float rounding does not.
	 */
	constexpr float TfTolerance = 1.0e-4f;

	/**
	 * Spelled out rather than using the <cmath> macros, which Unreal's build
	 * settings do not guarantee are available and which fast-math flags are
	 * entitled to fold away.
	 */
	const float NotANumber = std::numeric_limits<float>::quiet_NaN();
	const float PositiveInfinity = std::numeric_limits<float>::infinity();

	/** One LUT entry, with BOTH sampling conventions' answers. */
	struct FTfPixelCentreProbe
	{
		ECFDVizColorMap Map;
		int32 LutSize;
		int32 Index;

		/** GENERATED: colormaps.py build_lut(name, size)[index] - the (i+0.5)/N convention. */
		float CorrectR;
		float CorrectG;
		float CorrectB;

		/** GENERATED: colormaps.py sample_colormap(name, index/(size-1)) - the WRONG i/(N-1) convention. */
		float WrongR;
		float WrongG;
		float WrongB;
	};

	/**
	 * End entries, where the two conventions disagree by 10x-450x TfTolerance.
	 *
	 * Deliberately NOT centre entries: at i=31 of 64 the two conventions differ by
	 * 0.00004, which is under TfTolerance, so such a probe would pass either way.
	 * The test asserts that fact rather than assuming it.
	 */
	const FTfPixelCentreProbe TfPixelCentreProbes[] = {
		// viridis, N=64. Gap at the ends is ~0.045 - 450x TfTolerance.
		{ ECFDVizColorMap::Viridis, 64, 0,   0.267980f, 0.013377f, 0.337421f,   0.267004f, 0.004874f, 0.329415f },
		{ ECFDVizColorMap::Viridis, 64, 1,   0.269933f, 0.030384f, 0.353434f,   0.268987f, 0.022150f, 0.345682f },
		{ ECFDVizColorMap::Viridis, 64, 62,  0.857065f, 0.876643f, 0.199555f,   0.901019f, 0.886169f, 0.181604f },
		{ ECFDVizColorMap::Viridis, 64, 63,  0.947854f, 0.896319f, 0.162476f,   0.993248f, 0.906157f, 0.143936f },

		// viridis, N=256 - the shipped default width. Gap ~0.011 = 113x TfTolerance.
		{ ECFDVizColorMap::Viridis, 256, 0,   0.267248f, 0.007000f, 0.331417f,   0.267004f, 0.004874f, 0.329415f },
		{ ECFDVizColorMap::Viridis, 256, 255, 0.981899f, 0.903698f, 0.148571f,   0.993248f, 0.906157f, 0.143936f },

		// coolwarm, N=256 - the diverging fixture, checked at both ends.
		{ ECFDVizColorMap::CoolWarm, 256, 0,   0.231924f, 0.301096f, 0.755344f,   0.229806f, 0.298718f, 0.753683f },
		{ ECFDVizColorMap::CoolWarm, 256, 255, 0.708198f, 0.020856f, 0.151936f,   0.705673f, 0.015556f, 0.150233f },
	};

	/**
	 * The whole viridis LUT at N=8. GENERATED from build_lut("viridis", 8).
	 *
	 * A short table is included in full because at N=8 the half-texel error is
	 * enormous (entry 7 differs by 0.363 in R between conventions) and because a
	 * full table catches a shift of ANY size in ANY direction, not just at the
	 * probes someone thought to pick.
	 */
	const float TfViridis8[8][3] = {
		{ 0.274814f, 0.072900f, 0.393466f },
		{ 0.268279f, 0.203090f, 0.493750f },
		{ 0.230346f, 0.318506f, 0.541550f },
		{ 0.185190f, 0.421445f, 0.555632f },
		{ 0.145597f, 0.519041f, 0.554352f },
		{ 0.131130f, 0.612792f, 0.534102f },
		{ 0.200816f, 0.703694f, 0.479111f },
		{ 0.630095f, 0.827454f, 0.292255f },
	};

	/** Every shipped map, for the sweeps that must hold across all of them. */
	const ECFDVizColorMap TfAllMaps[] = {
		ECFDVizColorMap::Viridis,
		ECFDVizColorMap::Plasma,
		ECFDVizColorMap::Inferno,
		ECFDVizColorMap::Magma,
		ECFDVizColorMap::Turbo,
		ECFDVizColorMap::CoolWarm,
		ECFDVizColorMap::BlueWhiteRed,
		ECFDVizColorMap::Grayscale,
	};

	/** RGB-only L2 distance. Alpha is the opacity curve's business, not a colour's. */
	float TfRgbDistance(const FLinearColor& A, const FLinearColor& B)
	{
		return FMath::Sqrt(
			FMath::Square(A.R - B.R) + FMath::Square(A.G - B.G) + FMath::Square(A.B - B.B));
	}

	/** Read one float16 channel out of packed LUT bytes, little-endian, without a float round trip. */
	float TfReadPackedChannel(const TArray<uint8>& Bytes, int32 EntryIndex, int32 Channel)
	{
		const int32 Offset = EntryIndex * FlowVizTransferFunction::LutEntryBytes + Channel * 2;
		FFloat16 Half;
		Half.Encoded = static_cast<uint16>(Bytes[Offset]) | (static_cast<uint16>(Bytes[Offset + 1]) << 8);
		return Half.GetFloat();
	}
}

bool FFlowVizTransferFunctionTest::RunTest(const FString& Parameters)
{
	/* == The pixel-centre convention ========================================= */
	{
		// THE CONTROL. Before asserting which convention BuildLut used, prove the
		// two conventions actually disagree at every probe by more than the
		// tolerance. A probe where they agree could not fail and would be
		// decoration; asserting the gap turns "this test passes" into evidence.
		for (const FTfPixelCentreProbe& Probe : TfPixelCentreProbes)
		{
			const float Gap = FMath::Max3(
				FMath::Abs(Probe.CorrectR - Probe.WrongR),
				FMath::Abs(Probe.CorrectG - Probe.WrongG),
				FMath::Abs(Probe.CorrectB - Probe.WrongB));
			if (Gap <= TfTolerance * 10.0f)
			{
				AddError(FString::Printf(
					TEXT("Probe %s N=%d i=%d cannot distinguish (i+0.5)/N from i/(N-1): the two ")
					TEXT("conventions differ by only %.6f. A probe that cannot fail is not a check."),
					*CFDViz::ColorMaps::GetName(Probe.Map).ToString(),
					Probe.LutSize, Probe.Index, Gap));
			}
		}

		for (const FTfPixelCentreProbe& Probe : TfPixelCentreProbes)
		{
			FFlowVizTransferFunction Tf;
			Tf.ColorMap = Probe.Map;
			Tf.LutSize = Probe.LutSize;

			TArray<FLinearColor> Lut;
			if (!TestTrue(TEXT("the LUT builds"),
				FlowVizTransferFunction::BuildLut(Tf, Lut).IsOk()))
			{
				continue;
			}
			TestEqual(TEXT("LUT size honours LutSize"), Lut.Num(), Probe.LutSize);

			const FString Context = FString::Printf(TEXT("%s N=%d i=%d"),
				*CFDViz::ColorMaps::GetName(Probe.Map).ToString(), Probe.LutSize, Probe.Index);
			const FLinearColor& Entry = Lut[Probe.Index];

			// Against the Python source of truth, at pixel centres.
			TestNearlyEqual(*(Context + TEXT(" [R]")), Entry.R, Probe.CorrectR, TfTolerance);
			TestNearlyEqual(*(Context + TEXT(" [G]")), Entry.G, Probe.CorrectG, TfTolerance);
			TestNearlyEqual(*(Context + TEXT(" [B]")), Entry.B, Probe.CorrectB, TfTolerance);

			// And explicitly NOT the i/(N-1) answer. Stated separately so a
			// failure says "you used the endpoint convention" rather than just
			// "a number was wrong".
			const float DistanceToWrong = FMath::Max3(
				FMath::Abs(Entry.R - Probe.WrongR),
				FMath::Abs(Entry.G - Probe.WrongG),
				FMath::Abs(Entry.B - Probe.WrongB));
			if (DistanceToWrong <= TfTolerance)
			{
				AddError(FString::Printf(
					TEXT("%s matches the i/(N-1) convention. The LUT is shifted by half a texel, ")
					TEXT("which renders as a hue shift against every matplotlib/ParaView reference figure."),
					*Context));
			}
		}

		// The whole N=8 viridis table. Catches a shift of any size or direction,
		// not only at hand-picked probes.
		{
			FFlowVizTransferFunction Tf;
			Tf.ColorMap = ECFDVizColorMap::Viridis;
			Tf.LutSize = 8;

			TArray<FLinearColor> Lut;
			TestTrue(TEXT("an 8-entry LUT builds"), FlowVizTransferFunction::BuildLut(Tf, Lut).IsOk());
			if (Lut.Num() == 8)
			{
				for (int32 Index = 0; Index < 8; ++Index)
				{
					const FString Context = FString::Printf(TEXT("viridis N=8 i=%d"), Index);
					TestNearlyEqual(*(Context + TEXT(" [R]")), Lut[Index].R, TfViridis8[Index][0], TfTolerance);
					TestNearlyEqual(*(Context + TEXT(" [G]")), Lut[Index].G, TfViridis8[Index][1], TfTolerance);
					TestNearlyEqual(*(Context + TEXT(" [B]")), Lut[Index].B, TfViridis8[Index][2], TfTolerance);
				}
			}
		}
	}

	/* == SampleLut is the matching inverse =================================== */
	{
		// A LUT built at pixel centres and read back at pixel centres must return
		// its own entries exactly. If SampleLut used U*(N-1) instead of U*N-0.5,
		// this fails at every entry except the middle one.
		for (const int32 Size : { 8, 64, 256 })
		{
			FFlowVizTransferFunction Tf;
			Tf.ColorMap = ECFDVizColorMap::Viridis;
			Tf.LutSize = Size;

			TArray<FLinearColor> Lut;
			if (!FlowVizTransferFunction::BuildLut(Tf, Lut).IsOk())
			{
				AddError(FString::Printf(TEXT("a %d-entry LUT failed to build"), Size));
				continue;
			}

			float WorstError = 0.0f;
			int32 WorstIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Size; ++Index)
			{
				const float U = (static_cast<float>(Index) + 0.5f) / static_cast<float>(Size);
				const FLinearColor Sampled = FlowVizTransferFunction::SampleLut(Lut, U);
				const float Error = FMath::Max3(
					FMath::Abs(Sampled.R - Lut[Index].R),
					FMath::Abs(Sampled.G - Lut[Index].G),
					FMath::Abs(Sampled.B - Lut[Index].B));
				if (Error > WorstError)
				{
					WorstError = Error;
					WorstIndex = Index;
				}
			}
			TestTrue(
				*FString::Printf(
					TEXT("N=%d: sampling at (i+0.5)/N returns entry i (worst %.6f at i=%d)"),
					Size, WorstError, WorstIndex),
				WorstError <= TfTolerance);
		}

		// Outside [0,1] the end entries are held, not extrapolated or wrapped: an
		// out-of-range value is supposed to get a flag colour, and a LUT that
		// wrapped would hand it the OPPOSITE end of the colormap - the most
		// misleading possible answer.
		{
			FFlowVizTransferFunction Tf;
			TArray<FLinearColor> Lut;
			TestTrue(TEXT("a default LUT builds"), FlowVizTransferFunction::BuildLut(Tf, Lut).IsOk());
			if (Lut.Num() > 0)
			{
				TestTrue(TEXT("below 0 holds the first entry"),
					FlowVizTransferFunction::SampleLut(Lut, -3.0f).Equals(Lut[0], TfTolerance));
				TestTrue(TEXT("above 1 holds the last entry"),
					FlowVizTransferFunction::SampleLut(Lut, 4.0f).Equals(Lut[Lut.Num() - 1], TfTolerance));
			}
		}

		// An empty LUT yields black rather than reading out of bounds.
		{
			const FLinearColor Empty = FlowVizTransferFunction::SampleLut(TArrayView<const FLinearColor>(), 0.5f);
			TestEqual(TEXT("an empty LUT samples black"), Empty, FLinearColor(0, 0, 0, 0));
		}
	}

	/* == Opacity is a separate curve, never baked into the colour table ======= */
	{
		// The colour tables are shared with Python and compared against reference
		// figures. If an opacity edit perturbed RGB, every such comparison would
		// depend on the current opacity ramp.
		FFlowVizTransferFunction Opaque;
		Opaque.ColorMap = ECFDVizColorMap::Viridis;
		Opaque.Opacity = FFlowVizOpacityCurve::MakeConstant(1.0f);

		FFlowVizTransferFunction Ramped;
		Ramped.ColorMap = ECFDVizColorMap::Viridis;
		Ramped.Opacity = FFlowVizOpacityCurve::MakeLinearRamp();
		Ramped.Opacity.OpacityMultiplier = 0.25f;

		TArray<FLinearColor> OpaqueLut, RampedLut;
		TestTrue(TEXT("the opaque LUT builds"), FlowVizTransferFunction::BuildLut(Opaque, OpaqueLut).IsOk());
		TestTrue(TEXT("the ramped LUT builds"), FlowVizTransferFunction::BuildLut(Ramped, RampedLut).IsOk());

		if (OpaqueLut.Num() == RampedLut.Num() && OpaqueLut.Num() > 0)
		{
			float WorstRgbDrift = 0.0f;
			for (int32 Index = 0; Index < OpaqueLut.Num(); ++Index)
			{
				WorstRgbDrift = FMath::Max(WorstRgbDrift, TfRgbDistance(OpaqueLut[Index], RampedLut[Index]));
			}
			TestTrue(
				*FString::Printf(TEXT("changing the opacity curve leaves RGB untouched (worst drift %.6f)"),
					WorstRgbDrift),
				WorstRgbDrift <= TfTolerance);

			// ...and the alpha channel DID change, so the previous assertion is
			// about a real difference rather than two identical LUTs.
			float WorstAlphaDelta = 0.0f;
			for (int32 Index = 0; Index < OpaqueLut.Num(); ++Index)
			{
				WorstAlphaDelta = FMath::Max(WorstAlphaDelta,
					FMath::Abs(OpaqueLut[Index].A - RampedLut[Index].A));
			}
			TestTrue(
				*FString::Printf(TEXT("...while alpha genuinely differs (worst delta %.6f)"), WorstAlphaDelta),
				WorstAlphaDelta > 0.5f);

			// Alpha is NOT premultiplied: the ray-marcher needs the raw colour for
			// its own step-size correction. A premultiplied entry at low alpha
			// would have RGB near zero.
			const int32 Late = OpaqueLut.Num() - 1;
			TestTrue(TEXT("colour is not premultiplied by alpha"),
				TfRgbDistance(RampedLut[Late], OpaqueLut[Late]) <= TfTolerance);
		}

		// ColorMaps::BuildLut sets alpha to 1 and documents that opacity must not
		// be baked in. Assert that contract directly, so a change there is caught
		// here rather than by a wrong render.
		{
			TArray<FLinearColor> ColorOnly;
			CFDViz::ColorMaps::BuildLut(ECFDVizColorMap::Viridis, ColorOnly, 64);
			for (const FLinearColor& Entry : ColorOnly)
			{
				if (Entry.A != 1.0f)
				{
					AddError(TEXT("ColorMaps::BuildLut must leave alpha at 1; opacity is a separate curve."));
					break;
				}
			}
		}
	}

	/* == The opacity curve itself ============================================ */
	{
		// An EMPTY curve is fully opaque, not fully transparent. A default
		// transfer function that rendered nothing would read as a failed load.
		{
			FFlowVizOpacityCurve Empty;
			TestEqual(TEXT("an empty curve is opaque at 0"), Empty.Evaluate(0.0f), 1.0f);
			TestEqual(TEXT("an empty curve is opaque at 1"), Empty.Evaluate(1.0f), 1.0f);
			TestTrue(TEXT("an empty curve validates"), Empty.Validate().IsOk());
		}

		// A SINGLE point is a constant, not a ramp from zero.
		{
			FFlowVizOpacityCurve Single;
			Single.Points.Add(FFlowVizOpacityPoint(0.7f, 0.4f));
			TestNearlyEqual(TEXT("one point holds below itself"), Single.Evaluate(0.0f), 0.4f, TfTolerance);
			TestNearlyEqual(TEXT("one point holds at itself"), Single.Evaluate(0.7f), 0.4f, TfTolerance);
			TestNearlyEqual(TEXT("one point holds above itself"), Single.Evaluate(1.0f), 0.4f, TfTolerance);
		}

		// Linear interpolation between points, with the ends HELD rather than
		// extrapolated. Extrapolating produces opacities outside [0,1] that then
		// clamp, showing an invisible or solid region nobody asked for.
		{
			FFlowVizOpacityCurve Curve;
			Curve.Points.Add(FFlowVizOpacityPoint(0.25f, 0.0f));
			Curve.Points.Add(FFlowVizOpacityPoint(0.75f, 1.0f));

			TestNearlyEqual(TEXT("held below the first point"), Curve.Evaluate(0.0f), 0.0f, TfTolerance);
			TestNearlyEqual(TEXT("at the first point"), Curve.Evaluate(0.25f), 0.0f, TfTolerance);
			TestNearlyEqual(TEXT("midway is the midpoint"), Curve.Evaluate(0.50f), 0.5f, TfTolerance);
			TestNearlyEqual(TEXT("a quarter along"), Curve.Evaluate(0.375f), 0.25f, TfTolerance);
			TestNearlyEqual(TEXT("at the last point"), Curve.Evaluate(0.75f), 1.0f, TfTolerance);
			TestNearlyEqual(TEXT("held above the last point"), Curve.Evaluate(1.0f), 1.0f, TfTolerance);

			// Extrapolation would give -0.5 at t=0 and +1.5 at t=1. Assert the
			// held values are NOT those, so "held" is checked rather than assumed.
			TestTrue(TEXT("the curve is not extrapolated below"), Curve.Evaluate(0.0f) >= 0.0f);
			TestTrue(TEXT("the curve is not extrapolated above"), Curve.Evaluate(1.0f) <= 1.0f);
		}

		// Points need not be authored in order - an editor drags them past each
		// other. Unsorted input must give the same answer as sorted input.
		{
			FFlowVizOpacityCurve Sorted;
			Sorted.Points.Add(FFlowVizOpacityPoint(0.0f, 0.1f));
			Sorted.Points.Add(FFlowVizOpacityPoint(0.5f, 0.9f));
			Sorted.Points.Add(FFlowVizOpacityPoint(1.0f, 0.2f));

			FFlowVizOpacityCurve Shuffled;
			Shuffled.Points.Add(FFlowVizOpacityPoint(1.0f, 0.2f));
			Shuffled.Points.Add(FFlowVizOpacityPoint(0.0f, 0.1f));
			Shuffled.Points.Add(FFlowVizOpacityPoint(0.5f, 0.9f));

			for (const float T : { 0.0f, 0.2f, 0.5f, 0.8f, 1.0f })
			{
				TestNearlyEqual(
					*FString::Printf(TEXT("unsorted points evaluate identically at %.2f"), T),
					Shuffled.Evaluate(T), Sorted.Evaluate(T), TfTolerance);
			}

			// And the interpolated value is genuinely non-trivial at 0.25:
			// 0.1 + (0.9-0.1)*0.5 = 0.5.
			TestNearlyEqual(TEXT("interpolates between the first two points"),
				Sorted.Evaluate(0.25f), 0.5f, TfTolerance);
		}

		// The global multiplier scales the whole curve without editing it.
		{
			FFlowVizOpacityCurve Curve = FFlowVizOpacityCurve::MakeLinearRamp();
			const float Full = Curve.Evaluate(0.75f);
			Curve.OpacityMultiplier = 0.5f;
			TestNearlyEqual(TEXT("the multiplier halves the curve"),
				Curve.Evaluate(0.75f), Full * 0.5f, TfTolerance);
			Curve.OpacityMultiplier = 0.0f;
			TestNearlyEqual(TEXT("a zero multiplier makes it invisible"),
				Curve.Evaluate(0.75f), 0.0f, TfTolerance);
		}

		// Validation rejects what would corrupt a texture.
		{
			FFlowVizOpacityCurve Bad;
			Bad.Points.Add(FFlowVizOpacityPoint(1.5f, 0.5f));
			TestFalse(TEXT("a position above 1 is rejected"), Bad.Validate().IsOk());

			FFlowVizOpacityCurve NegativeOpacity;
			NegativeOpacity.Points.Add(FFlowVizOpacityPoint(0.5f, -0.1f));
			TestFalse(TEXT("a negative opacity is rejected"), NegativeOpacity.Validate().IsOk());

			FFlowVizOpacityCurve NotFinite;
			NotFinite.Points.Add(FFlowVizOpacityPoint(0.5f, NotANumber));
			TestFalse(TEXT("a NaN opacity is rejected"), NotFinite.Validate().IsOk());

			FFlowVizOpacityCurve BadMultiplier;
			BadMultiplier.OpacityMultiplier = 2.0f;
			TestFalse(TEXT("a multiplier above 1 is rejected"), BadMultiplier.Validate().IsOk());

			FFlowVizOpacityCurve TooMany;
			for (int32 Index = 0; Index < FlowVizTransferFunction::MaxOpacityPoints + 1; ++Index)
			{
				TooMany.Points.Add(FFlowVizOpacityPoint(0.5f, 0.5f));
			}
			TestFalse(TEXT("too many points is rejected"), TooMany.Validate().IsOk());
		}

		// A NaN query must not propagate into the alpha channel of a texture.
		{
			FFlowVizOpacityCurve Curve = FFlowVizOpacityCurve::MakeLinearRamp();
			TestTrue(TEXT("a NaN position yields a finite opacity"),
				FMath::IsFinite(Curve.Evaluate(NotANumber)));
		}
	}

	/* == The opacity curve reaches the LUT at matching coordinates ============ */
	{
		// A control point at t and a colour stop at t must land on the same entry.
		FFlowVizTransferFunction Tf;
		Tf.ColorMap = ECFDVizColorMap::Grayscale;
		Tf.LutSize = 256;
		Tf.Opacity.Points.Add(FFlowVizOpacityPoint(0.0f, 0.0f));
		Tf.Opacity.Points.Add(FFlowVizOpacityPoint(1.0f, 1.0f));

		TArray<FLinearColor> Lut;
		if (TestTrue(TEXT("a ramped LUT builds"), FlowVizTransferFunction::BuildLut(Tf, Lut).IsOk()))
		{
			// Alpha at entry i must equal the curve at the PIXEL CENTRE (i+0.5)/N,
			// not at i/(N-1). Entry 0 is 0.5/256 = 0.001953, not 0.
			TestNearlyEqual(TEXT("alpha[0] is the curve at 0.5/256"), Lut[0].A, 0.5f / 256.0f, TfTolerance);
			TestNearlyEqual(TEXT("alpha[255] is the curve at 255.5/256"), Lut[255].A, 255.5f / 256.0f, TfTolerance);

			// The i/(N-1) convention would give exactly 0 and exactly 1.
			TestTrue(TEXT("alpha[0] is not the i/(N-1) answer of 0"), Lut[0].A > TfTolerance);
			TestTrue(TEXT("alpha[255] is not the i/(N-1) answer of 1"), Lut[255].A < 1.0f - TfTolerance);

			// Grayscale RGB is the identity map, so the same check applies to
			// colour and cross-checks the two halves against each other.
			TestNearlyEqual(TEXT("grayscale R[0] is also at the pixel centre"),
				Lut[0].R, 0.5f / 256.0f, TfTolerance);
		}
	}

	/* == Values are NEVER clamped into the domain (VISUAL_QA rule 4) ========== */
	{
		FFlowVizTransferFunction Tf;
		Tf.ValueRangeMin = 10.0f;
		Tf.ValueRangeMax = 20.0f;

		// In range.
		{
			const FFlowVizMappedValue Mapped = Tf.MapValueToNormalized(15.0f);
			TestEqual(TEXT("15 in [10,20] is in range"), Mapped.ValueClass, EFlowVizValueClass::InRange);
			TestNearlyEqual(TEXT("...at coordinate 0.5"), Mapped.Normalized, 0.5f, TfTolerance);
		}

		// The endpoints are IN range, not out of it - an inclusive domain.
		{
			TestEqual(TEXT("the minimum is in range"),
				Tf.MapValueToNormalized(10.0f).ValueClass, EFlowVizValueClass::InRange);
			TestEqual(TEXT("the maximum is in range"),
				Tf.MapValueToNormalized(20.0f).ValueClass, EFlowVizValueClass::InRange);
			TestNearlyEqual(TEXT("the minimum maps to 0"),
				Tf.MapValueToNormalized(10.0f).Normalized, 0.0f, TfTolerance);
			TestNearlyEqual(TEXT("the maximum maps to 1"),
				Tf.MapValueToNormalized(20.0f).Normalized, 1.0f, TfTolerance);
		}

		// UNDER range: classified AND left below zero. A clamped coordinate would
		// make two very different values indistinguishable to the shader.
		{
			const FFlowVizMappedValue Near = Tf.MapValueToNormalized(9.0f);
			const FFlowVizMappedValue Far = Tf.MapValueToNormalized(-90.0f);

			TestEqual(TEXT("9 is under range"), Near.ValueClass, EFlowVizValueClass::UnderRange);
			TestEqual(TEXT("-90 is under range"), Far.ValueClass, EFlowVizValueClass::UnderRange);
			TestNearlyEqual(TEXT("9 maps to -0.1, not to 0"), Near.Normalized, -0.1f, TfTolerance);
			TestNearlyEqual(TEXT("-90 maps to -10, not to 0"), Far.Normalized, -10.0f, TfTolerance);
			TestTrue(TEXT("a barely-under and a far-under value stay distinguishable"),
				FMath::Abs(Near.Normalized - Far.Normalized) > 1.0f);
		}

		// OVER range: same, above 1.
		{
			const FFlowVizMappedValue Near = Tf.MapValueToNormalized(21.0f);
			const FFlowVizMappedValue Far = Tf.MapValueToNormalized(120.0f);

			TestEqual(TEXT("21 is over range"), Near.ValueClass, EFlowVizValueClass::OverRange);
			TestEqual(TEXT("120 is over range"), Far.ValueClass, EFlowVizValueClass::OverRange);
			TestNearlyEqual(TEXT("21 maps to 1.1, not to 1"), Near.Normalized, 1.1f, TfTolerance);
			TestNearlyEqual(TEXT("120 maps to 11, not to 1"), Far.Normalized, 11.0f, TfTolerance);
			TestTrue(TEXT("a barely-over and a far-over value stay distinguishable"),
				FMath::Abs(Near.Normalized - Far.Normalized) > 1.0f);
		}

		// NaN and infinity are Invalid, NOT out-of-range: +inf is how CVF spells
		// "no valid data", which is a different statement from "a large value".
		{
			TestEqual(TEXT("NaN is invalid"),
				Tf.MapValueToNormalized(NotANumber).ValueClass, EFlowVizValueClass::Invalid);
			TestEqual(TEXT("+inf is invalid, not over-range"),
				Tf.MapValueToNormalized(PositiveInfinity).ValueClass, EFlowVizValueClass::Invalid);
			TestEqual(TEXT("-inf is invalid, not under-range"),
				Tf.MapValueToNormalized(-PositiveInfinity).ValueClass, EFlowVizValueClass::Invalid);
		}

		// bClampToRange is a DISPLAY choice and must not erase the classification -
		// otherwise the renderer cannot know what it is hiding.
		{
			FFlowVizTransferFunction Clamped = Tf;
			Clamped.bClampToRange = true;
			const FFlowVizMappedValue Mapped = Clamped.MapValueToNormalized(-90.0f);
			TestEqual(TEXT("clamping does not erase the under-range class"),
				Mapped.ValueClass, EFlowVizValueClass::UnderRange);
			TestNearlyEqual(TEXT("...nor the coordinate"), Mapped.Normalized, -10.0f, TfTolerance);
		}

		// The inverse round-trips, including outside the domain.
		for (const float Value : { 10.0f, 12.5f, 20.0f, -5.0f, 55.0f })
		{
			const FFlowVizMappedValue Mapped = Tf.MapValueToNormalized(Value);
			TestNearlyEqual(
				*FString::Printf(TEXT("%.1f round-trips through the domain"), Value),
				Tf.MapNormalizedToValue(Mapped.Normalized), Value, 1.0e-3f);
		}

		// A negative domain, to catch code that assumed Min >= 0.
		{
			FFlowVizTransferFunction Negative;
			Negative.ValueRangeMin = -30.0f;
			Negative.ValueRangeMax = -10.0f;
			TestNearlyEqual(TEXT("a fully negative domain maps its midpoint to 0.5"),
				Negative.MapValueToNormalized(-20.0f).Normalized, 0.5f, TfTolerance);
			TestEqual(TEXT("...and -5 is over range, not under"),
				Negative.MapValueToNormalized(-5.0f).ValueClass, EFlowVizValueClass::OverRange);
		}
	}

	/* == EvaluateColor draws the flag colours, not the colormap ends ========== */
	{
		FFlowVizTransferFunction Tf = FFlowVizTransferFunction::MakeDefault(
			ECFDVizColorMap::Viridis, 0.0f, 100.0f);

		TArray<FLinearColor> Lut;
		if (TestTrue(TEXT("the default LUT builds"), FlowVizTransferFunction::BuildLut(Tf, Lut).IsOk()))
		{
			const FLinearColor Under = Tf.EvaluateColor(Lut, -50.0f);
			const FLinearColor Over = Tf.EvaluateColor(Lut, 500.0f);
			const FLinearColor Invalid = Tf.EvaluateColor(Lut, NotANumber);

			TestTrue(TEXT("an under-range value draws the under colour"),
				TfRgbDistance(Under, Tf.UnderRangeColor) <= TfTolerance);
			TestTrue(TEXT("an over-range value draws the over colour"),
				TfRgbDistance(Over, Tf.OverRangeColor) <= TfTolerance);
			TestTrue(TEXT("a NaN draws the NaN colour"),
				TfRgbDistance(Invalid, Tf.NaNColor) <= TfTolerance);

			// THE ASSERTION RULE 4 ACTUALLY NEEDS. Silently drawing the colormap's
			// minimum is the specific failure the rule forbids, so it is checked
			// as its own claim rather than implied by the one above.
			TestTrue(TEXT("an under-range value is NOT the colormap's minimum"),
				TfRgbDistance(Under, Lut[0]) > 0.2f);
			TestTrue(TEXT("an over-range value is NOT the colormap's maximum"),
				TfRgbDistance(Over, Lut[Lut.Num() - 1]) > 0.2f);

			// An in-range value does come from the LUT.
			const FLinearColor Mid = Tf.EvaluateColor(Lut, 50.0f);
			TestTrue(TEXT("an in-range value comes from the LUT"),
				TfRgbDistance(Mid, FlowVizTransferFunction::SampleLut(Lut, 0.5f)) <= TfTolerance);
		}
	}

	/* == The flag colours cannot be mistaken for data ========================= */
	{
		// If a flag colour collided with a colormap entry, a NaN would be
		// indistinguishable from a real value - rule 4 defeated by palette choice
		// rather than by logic. Checked against EVERY shipped map so that adding a
		// colormap that collides fails here instead of silently degrading the
		// invalid-data display.
		const FFlowVizTransferFunction Tf;
		const TPair<const TCHAR*, FLinearColor> Flags[] = {
			{ TEXT("under-range"), Tf.UnderRangeColor },
			{ TEXT("over-range"), Tf.OverRangeColor },
			{ TEXT("NaN"), Tf.NaNColor },
			{ TEXT("masked"), Tf.MaskedColor },
		};

		for (const TPair<const TCHAR*, FLinearColor>& Flag : Flags)
		{
			float Closest = TNumericLimits<float>::Max();
			ECFDVizColorMap ClosestMap = ECFDVizColorMap::Viridis;
			for (const ECFDVizColorMap Map : TfAllMaps)
			{
				TArray<FLinearColor> MapLut;
				CFDViz::ColorMaps::BuildLut(Map, MapLut, 256);
				for (const FLinearColor& Entry : MapLut)
				{
					const float Distance = TfRgbDistance(Flag.Value, Entry);
					if (Distance < Closest)
					{
						Closest = Distance;
						ClosestMap = Map;
					}
				}
			}
			// 0.25 in RGB L2 is a plainly different colour at any brightness; the
			// four defaults clear it (the tightest, cyan, sits at 0.298 from turbo).
			TestTrue(
				*FString::Printf(
					TEXT("the %s colour is distinguishable from every colormap entry (closest %.4f, in %s)"),
					Flag.Key, Closest, *CFDViz::ColorMaps::GetName(ClosestMap).ToString()),
				Closest > 0.25f);
		}

		// ...and from each other, since the diagnostics panel reports masked and
		// NaN counts separately and a user must be able to tell them apart.
		for (int32 A = 0; A < UE_ARRAY_COUNT(Flags); ++A)
		{
			for (int32 B = A + 1; B < UE_ARRAY_COUNT(Flags); ++B)
			{
				const float Distance = TfRgbDistance(Flags[A].Value, Flags[B].Value);
				TestTrue(
					*FString::Printf(TEXT("the %s and %s colours are distinct (%.4f apart)"),
						Flags[A].Key, Flags[B].Key, Distance),
					Distance > 0.25f);
			}
		}
	}

	/* == Diverging maps centre on zero (VISUAL_QA rule 7) ==================== */
	{
		// The asymmetric fixture is the point: [-2, 8] has midpoint 3, so
		// centring on the data midpoint and centring on zero give different
		// answers. A symmetric fixture like [-5, 5] would pass either way.
		for (const ECFDVizColorMap Map : { ECFDVizColorMap::CoolWarm, ECFDVizColorMap::BlueWhiteRed })
		{
			const FString Name = CFDViz::ColorMaps::GetName(Map).ToString();
			float Min = 0.0f;
			float Max = 0.0f;
			FFlowVizTransferFunction::MakeDefaultDomain(Map, -2.0f, 8.0f, Min, Max);

			TestNearlyEqual(*(Name + TEXT(": domain min is -8")), Min, -8.0f, TfTolerance);
			TestNearlyEqual(*(Name + TEXT(": domain max is +8")), Max, 8.0f, TfTolerance);

			// The claim that actually matters: zero lands on the map's neutral
			// coordinate. Asserted on the COORDINATE because coolwarm and
			// blue-white-red are symmetric in R/B about 0.5 - a wrongly centred
			// domain still yields a plausible colour, so a colour assertion here
			// would be weak evidence.
			const FFlowVizTransferFunction Tf = FFlowVizTransferFunction::MakeDefault(Map, -2.0f, 8.0f);
			TestNearlyEqual(*(Name + TEXT(": zero maps to the neutral coordinate 0.5")),
				Tf.MapValueToNormalized(0.0f).Normalized, 0.5f, TfTolerance);

			// The data-midpoint mistake would put zero at 0.2. State it so a
			// failure names the actual bug.
			TestTrue(*(Name + TEXT(": zero is NOT at the data midpoint's 0.2")),
				FMath::Abs(Tf.MapValueToNormalized(0.0f).Normalized - 0.2f) > 0.05f);

			// The whole data range still fits inside the widened domain - a
			// centring that cropped the data would trade one lie for another.
			TestTrue(*(Name + TEXT(": the data minimum is still in range")),
				Tf.MapValueToNormalized(-2.0f).IsInRange());
			TestTrue(*(Name + TEXT(": the data maximum is still in range")),
				Tf.MapValueToNormalized(8.0f).IsInRange());
		}

		// A one-sided range still centres: [2, 8] must become [-8, 8], because a
		// diverging map with no negative data still means its neutral is zero.
		{
			float Min = 0.0f;
			float Max = 0.0f;
			FFlowVizTransferFunction::MakeDefaultDomain(ECFDVizColorMap::CoolWarm, 2.0f, 8.0f, Min, Max);
			TestNearlyEqual(TEXT("a wholly positive range still centres on zero [min]"), Min, -8.0f, TfTolerance);
			TestNearlyEqual(TEXT("a wholly positive range still centres on zero [max]"), Max, 8.0f, TfTolerance);
		}

		// A SEQUENTIAL map keeps the data range. Re-centring viridis would throw
		// away half the dynamic range for no reason.
		for (const ECFDVizColorMap Map : { ECFDVizColorMap::Viridis, ECFDVizColorMap::Turbo, ECFDVizColorMap::Grayscale })
		{
			float Min = 0.0f;
			float Max = 0.0f;
			FFlowVizTransferFunction::MakeDefaultDomain(Map, -2.0f, 8.0f, Min, Max);
			const FString Name = CFDViz::ColorMaps::GetName(Map).ToString();
			TestNearlyEqual(*(Name + TEXT(": sequential keeps the data min")), Min, -2.0f, TfTolerance);
			TestNearlyEqual(*(Name + TEXT(": sequential keeps the data max")), Max, 8.0f, TfTolerance);
		}

		// The rule follows IsDiverging rather than a hard-coded list, so the two
		// cannot drift apart.
		for (const ECFDVizColorMap Map : TfAllMaps)
		{
			float Min = 0.0f;
			float Max = 0.0f;
			FFlowVizTransferFunction::MakeDefaultDomain(Map, -2.0f, 8.0f, Min, Max);
			const bool bCentred = FMath::IsNearlyEqual(Min, -Max, TfTolerance);
			TestEqual(
				*FString::Printf(TEXT("%s centres iff IsDiverging says so"),
					*CFDViz::ColorMaps::GetName(Map).ToString()),
				bCentred, CFDViz::ColorMaps::IsDiverging(Map));
		}

		// A degenerate range yields a usable unit domain, not a zero-width one
		// that would divide by zero in every mapping.
		{
			const FFlowVizTransferFunction Flat =
				FFlowVizTransferFunction::MakeDefault(ECFDVizColorMap::Viridis, 5.0f, 5.0f);
			TestTrue(TEXT("a zero-width data range yields a valid domain"), Flat.Validate().IsOk());
			TestTrue(TEXT("...with a positive width"), Flat.GetValueRange() > 0.0f);

			const FFlowVizTransferFunction Inverted =
				FFlowVizTransferFunction::MakeDefault(ECFDVizColorMap::Viridis, 9.0f, 1.0f);
			TestTrue(TEXT("an inverted data range yields a valid domain"), Inverted.Validate().IsOk());

			const FFlowVizTransferFunction NotFinite =
				FFlowVizTransferFunction::MakeDefault(ECFDVizColorMap::Viridis, NotANumber, 1.0f);
			TestTrue(TEXT("a non-finite data range yields a valid domain"), NotFinite.Validate().IsOk());
		}
	}

	/* == The default is perceptually uniform (VISUAL_QA rule 6) =============== */
	{
		const FFlowVizTransferFunction Default;
		TestEqual(TEXT("the default colormap is the shared default"),
			Default.ColorMap, CFDViz::ColorMaps::Default);
		TestTrue(TEXT("the default colormap is perceptually uniform"),
			CFDViz::ColorMaps::IsPerceptuallyUniform(Default.ColorMap));
		TestFalse(TEXT("the default is not a rainbow"),
			Default.ColorMap == ECFDVizColorMap::Turbo);
		TestFalse(TEXT("the default does not clamp out-of-range values away"),
			Default.bClampToRange);
		TestEqual(TEXT("the default is continuous, not banded"), Default.ColorBands, 0);
		TestTrue(TEXT("the default transfer function validates"), Default.Validate().IsOk());
	}

	/* == No posterization in the transfer function (VISUAL_QA rule 11) ======== */
	{
		// Two independent ways of saying "smooth", because either alone has a
		// blind spot: a distinct-value count misses a LUT with big jumps between
		// distinct entries, and a step-size bound misses a LUT that repeats
		// entries without ever jumping.
		for (const ECFDVizColorMap Map : TfAllMaps)
		{
			const FString Name = CFDViz::ColorMaps::GetName(Map).ToString();

			FFlowVizTransferFunction Tf;
			Tf.ColorMap = Map;
			Tf.LutSize = 256;

			TArray<FLinearColor> Lut;
			if (!FlowVizTransferFunction::BuildLut(Tf, Lut).IsOk())
			{
				AddError(*FString::Printf(TEXT("%s failed to build a LUT"), *Name));
				continue;
			}

			// (a) No adjacent pair jumps by more than 1/32 in any channel. The
			// measured worst across all eight shipped maps is turbo at 0.0304
			// (7.7/255); an 8-band quantisation jumps 0.4666, fifteen times that.
			float WorstStep = 0.0f;
			for (int32 Index = 1; Index < Lut.Num(); ++Index)
			{
				WorstStep = FMath::Max(WorstStep, FMath::Max3(
					FMath::Abs(Lut[Index].R - Lut[Index - 1].R),
					FMath::Abs(Lut[Index].G - Lut[Index - 1].G),
					FMath::Abs(Lut[Index].B - Lut[Index - 1].B)));
			}
			TestTrue(
				*FString::Printf(TEXT("%s: no visible step between adjacent LUT entries (worst %.4f)"),
					*Name, WorstStep),
				WorstStep < 0.04f);

			// (b) The float16 packing keeps all 256 entries distinct. An 8-bit
			// LUT would collapse viridis to 240 - sixteen adjacent pairs merged,
			// which is posterization introduced by the texture format rather than
			// by the colormap.
			TArray<uint8> Packed;
			if (TestTrue(*FString::Printf(TEXT("%s packs"), *Name),
				FlowVizTransferFunction::PackLutBytes(Lut, Packed).IsOk()))
			{
				TSet<uint64> Distinct;
				for (int32 Index = 0; Index < Lut.Num(); ++Index)
				{
					const uint64 Key =
						(static_cast<uint64>(FMath::RoundToInt(TfReadPackedChannel(Packed, Index, 0) * 65535.0f)) << 32)
						| (static_cast<uint64>(FMath::RoundToInt(TfReadPackedChannel(Packed, Index, 1) * 65535.0f)) << 16)
						| static_cast<uint64>(FMath::RoundToInt(TfReadPackedChannel(Packed, Index, 2) * 65535.0f));
					Distinct.Add(Key);
				}
				TestEqual(
					*FString::Printf(TEXT("%s: all 256 packed entries stay distinct"), *Name),
					Distinct.Num(), 256);
			}
		}

		// THE CONTROL. Prove the smoothness check can actually fail, by feeding it
		// the one input that is legitimately stepped. Without this the bound above
		// could be so loose that nothing fails it.
		{
			FFlowVizTransferFunction Banded;
			Banded.ColorMap = ECFDVizColorMap::Viridis;
			Banded.LutSize = 256;
			Banded.ColorBands = 8;

			TArray<FLinearColor> Lut;
			if (TestTrue(TEXT("a banded LUT builds"), FlowVizTransferFunction::BuildLut(Banded, Lut).IsOk()))
			{
				float WorstStep = 0.0f;
				int32 Transitions = 0;
				for (int32 Index = 1; Index < Lut.Num(); ++Index)
				{
					const float Step = FMath::Max3(
						FMath::Abs(Lut[Index].R - Lut[Index - 1].R),
						FMath::Abs(Lut[Index].G - Lut[Index - 1].G),
						FMath::Abs(Lut[Index].B - Lut[Index - 1].B));
					WorstStep = FMath::Max(WorstStep, Step);
					if (Step > TfTolerance)
					{
						++Transitions;
						TestEqual(TEXT("a band boundary falls on a multiple of 32"), Index % 32, 0);
					}
				}
				TestTrue(
					*FString::Printf(
						TEXT("the smoothness bound DOES reject a banded LUT (worst step %.4f)"), WorstStep),
					WorstStep >= 0.04f);
				TestEqual(TEXT("8 bands produce 7 transitions"), Transitions, 7);
			}
		}
	}

	/* == Packing ============================================================= */
	{
		FFlowVizTransferFunction Tf;
		Tf.ColorMap = ECFDVizColorMap::Viridis;
		Tf.LutSize = 64;
		Tf.Opacity = FFlowVizOpacityCurve::MakeLinearRamp();

		TArray<FLinearColor> Lut;
		TArray<uint8> Packed;
		TestTrue(TEXT("the LUT builds"), FlowVizTransferFunction::BuildLut(Tf, Lut).IsOk());
		TestTrue(TEXT("the LUT packs"), FlowVizTransferFunction::PackLutBytes(Lut, Packed).IsOk());

		TestEqual(TEXT("packed size is entries * 8 bytes"), Packed.Num(), 64 * 8);
		TestEqual(TEXT("GetPackedBytes agrees"),
			FlowVizTransferFunction::GetPackedBytes(64), static_cast<int64>(64 * 8));

		// Channel order is RGBA and the values survive the float16 round trip to
		// well within 8-bit precision. A swapped R/B would render a blue-green
		// map orange and is otherwise invisible in a pure-CPU test.
		for (const int32 Index : { 0, 1, 31, 63 })
		{
			const FString Context = FString::Printf(TEXT("packed entry %d"), Index);
			TestNearlyEqual(*(Context + TEXT(" [R]")), TfReadPackedChannel(Packed, Index, 0), Lut[Index].R, 1.0e-3f);
			TestNearlyEqual(*(Context + TEXT(" [G]")), TfReadPackedChannel(Packed, Index, 1), Lut[Index].G, 1.0e-3f);
			TestNearlyEqual(*(Context + TEXT(" [B]")), TfReadPackedChannel(Packed, Index, 2), Lut[Index].B, 1.0e-3f);
			TestNearlyEqual(*(Context + TEXT(" [A]")), TfReadPackedChannel(Packed, Index, 3), Lut[Index].A, 1.0e-3f);
		}

		// Viridis entry 0 is dark purple: R and B differ enough that an R/B swap
		// is detectable. Asserted so the channel-order check above is known to be
		// discriminating rather than trivially satisfied.
		TestTrue(TEXT("the packing fixture can detect an R/B swap"),
			FMath::Abs(Lut[0].R - Lut[0].B) > 0.05f);

		// A LUT outside the size bounds is refused rather than packed.
		{
			TArray<FLinearColor> TooSmall;
			TooSmall.Add(FLinearColor::White);
			TArray<uint8> Bytes;
			TestFalse(TEXT("a 1-entry LUT is refused"),
				FlowVizTransferFunction::PackLutBytes(TooSmall, Bytes).IsOk());
			TestEqual(TEXT("...and the output is emptied"), Bytes.Num(), 0);
			TestEqual(TEXT("GetPackedBytes refuses 1"),
				FlowVizTransferFunction::GetPackedBytes(1), static_cast<int64>(INDEX_NONE));
			TestEqual(TEXT("GetPackedBytes refuses an oversized LUT"),
				FlowVizTransferFunction::GetPackedBytes(FlowVizTransferFunction::MaxLutSize + 1),
				static_cast<int64>(INDEX_NONE));
		}
	}

	/* == Validation ========================================================== */
	{
		FFlowVizTransferFunction Tf;

		Tf = FFlowVizTransferFunction();
		Tf.LutSize = 1;
		TestFalse(TEXT("a 1-entry LUT size is rejected"), Tf.Validate().IsOk());

		Tf = FFlowVizTransferFunction();
		Tf.LutSize = FlowVizTransferFunction::MaxLutSize + 1;
		TestFalse(TEXT("an oversized LUT is rejected"), Tf.Validate().IsOk());

		Tf = FFlowVizTransferFunction();
		Tf.ValueRangeMin = 10.0f;
		Tf.ValueRangeMax = 10.0f;
		TestFalse(TEXT("a zero-width domain is rejected"), Tf.Validate().IsOk());

		Tf = FFlowVizTransferFunction();
		Tf.ValueRangeMin = 20.0f;
		Tf.ValueRangeMax = 10.0f;
		TestFalse(TEXT("an inverted domain is rejected"), Tf.Validate().IsOk());

		Tf = FFlowVizTransferFunction();
		Tf.ValueRangeMax = NotANumber;
		TestFalse(TEXT("a non-finite domain is rejected"), Tf.Validate().IsOk());

		Tf = FFlowVizTransferFunction();
		Tf.ColorBands = -1;
		TestFalse(TEXT("negative bands are rejected"), Tf.Validate().IsOk());

		Tf = FFlowVizTransferFunction();
		Tf.Opacity.OpacityMultiplier = -1.0f;
		TestFalse(TEXT("a bad opacity curve fails the whole transfer function"), Tf.Validate().IsOk());

		// A rejected transfer function must not produce a LUT: half a LUT would
		// upload and render as a plausible wrong image.
		{
			FFlowVizTransferFunction Bad;
			Bad.LutSize = 0;
			TArray<FLinearColor> Lut;
			Lut.Add(FLinearColor::White);
			TestFalse(TEXT("an invalid transfer function builds no LUT"),
				FlowVizTransferFunction::BuildLut(Bad, Lut).IsOk());
			TestEqual(TEXT("...and the output is emptied, not left stale"), Lut.Num(), 0);
		}
	}

	/* == Shader parameters =================================================== */
	{
		FFlowVizTransferFunction Tf;
		Tf.ValueRangeMin = -4.0f;
		Tf.ValueRangeMax = 12.0f;
		Tf.LutSize = 256;
		Tf.Opacity.OpacityMultiplier = 0.75f;
		Tf.bClampToRange = true;

		FFlowVizTransferFunctionShaderParameters Params;
		if (TestTrue(TEXT("shader parameters build"),
			FlowVizTransferFunction::MakeShaderParameters(Tf, Params).IsOk()))
		{
			TestEqual(TEXT("range min"), Params.ValueRangeMin, -4.0f);
			TestEqual(TEXT("range max"), Params.ValueRangeMax, 12.0f);
			TestNearlyEqual(TEXT("inverse range"), Params.InvValueRange, 1.0f / 16.0f, TfTolerance);
			TestEqual(TEXT("LUT size"), Params.LutSize, 256);
			TestEqual(TEXT("opacity multiplier"), Params.OpacityMultiplier, 0.75f);
			TestEqual(TEXT("clamp flag"), Params.bClampToRange, static_cast<uint32>(1));

			// The half-texel the shader needs to hit pixel centres. Getting this
			// wrong reintroduces the same half-texel shift on the GPU side.
			TestNearlyEqual(TEXT("half texel is 0.5/N"), Params.LutHalfTexel, 0.5f / 256.0f, TfTolerance);
			TestNearlyEqual(TEXT("coord scale is (N-1)/N"), Params.LutCoordScale, 255.0f / 256.0f, TfTolerance);

			// The four flag colours reach the shader, since they are what rule 4
			// is implemented with.
			TestEqual(TEXT("under colour reaches the shader"), Params.UnderRangeColor, Tf.UnderRangeColor);
			TestEqual(TEXT("over colour reaches the shader"), Params.OverRangeColor, Tf.OverRangeColor);
			TestEqual(TEXT("NaN colour reaches the shader"), Params.NaNColor, Tf.NaNColor);
			TestEqual(TEXT("masked colour reaches the shader"), Params.MaskedColor, Tf.MaskedColor);
		}

		// An invalid transfer function leaves the block untouched rather than
		// half-populated.
		{
			FFlowVizTransferFunction Bad;
			Bad.ValueRangeMax = Bad.ValueRangeMin;
			FFlowVizTransferFunctionShaderParameters Untouched;
			Untouched.LutSize = 4242;
			TestFalse(TEXT("an invalid transfer function makes no parameters"),
				FlowVizTransferFunction::MakeShaderParameters(Bad, Untouched).IsOk());
			TestEqual(TEXT("...and the block is untouched"), Untouched.LutSize, 4242);
		}
	}

	/* == The resource updates in place and does nothing when nothing changed == */
	{
		// Runs without an RHI: Update builds the LUT on the calling thread and
		// enqueues at most one command, and the counters below are CPU-side.
		FFlowVizTransferFunctionResource Resource;
		TestFalse(TEXT("a fresh resource has no content"), Resource.HasContent());
		TestEqual(TEXT("...and has built nothing"), Resource.GetBuildCount(), 0);

		FFlowVizTransferFunction Tf = FFlowVizTransferFunction::MakeDefault(
			ECFDVizColorMap::Viridis, 0.0f, 10.0f);

		TestTrue(TEXT("the first update succeeds"), Resource.Update(Tf).IsOk());
		TestTrue(TEXT("...and the resource has content"), Resource.HasContent());
		TestEqual(TEXT("...built once"), Resource.GetBuildCount(), 1);
		TestEqual(TEXT("...with the requested LUT size"), Resource.GetLut().Num(), Tf.LutSize);

		// ENGINEERING RULE 2. An identical update must not rebuild or re-upload.
		// Without the counter this is unobservable from outside, and a resource
		// that rebuilt every frame would pass every correctness assertion.
		TestTrue(TEXT("an identical update succeeds"), Resource.Update(Tf).IsOk());
		TestEqual(TEXT("...and rebuilds nothing"), Resource.GetBuildCount(), 1);

		// A real change does rebuild - so the previous assertion is about a
		// working no-op path, not about a resource that never rebuilds at all.
		Tf.Opacity.OpacityMultiplier = 0.5f;
		TestTrue(TEXT("a changed opacity updates"), Resource.Update(Tf).IsOk());
		TestEqual(TEXT("...and rebuilds once more"), Resource.GetBuildCount(), 2);

		// A domain change with an identical LUT still rebuilds (the shader
		// parameters changed), but must not recreate the texture.
		const int32 CreatesBefore = Resource.GetTextureCreateCount();
		Tf.ValueRangeMax = 20.0f;
		TestTrue(TEXT("a changed domain updates"), Resource.Update(Tf).IsOk());
		TestEqual(TEXT("...without recreating the texture"),
			Resource.GetTextureCreateCount(), CreatesBefore);

		// An invalid update changes NOTHING: the resident LUT keeps its contents
		// rather than being left half-updated.
		{
			const int32 BuildsBefore = Resource.GetBuildCount();
			const TArray<FLinearColor> Before(Resource.GetLut());

			FFlowVizTransferFunction Bad = Tf;
			Bad.LutSize = -1;
			TestFalse(TEXT("an invalid update is rejected"), Resource.Update(Bad).IsOk());
			TestEqual(TEXT("...and rebuilds nothing"), Resource.GetBuildCount(), BuildsBefore);
			TestEqual(TEXT("...and the resident LUT is unchanged"),
				Resource.GetLut().Num(), Before.Num());
			if (Resource.GetLut().Num() == Before.Num() && Before.Num() > 0)
			{
				TestTrue(TEXT("...byte for byte"),
					Resource.GetLut()[0].Equals(Before[0], TfTolerance));
			}
			TestTrue(TEXT("...and the resident transfer function is unchanged"),
				Resource.GetTransferFunction().Equals(Tf));
		}

		// Shader parameters come from the resident transfer function.
		{
			FFlowVizTransferFunctionShaderParameters Params;
			TestTrue(TEXT("the resource makes shader parameters"),
				Resource.MakeShaderParameters(Params).IsOk());
			TestEqual(TEXT("...for the resident domain"), Params.ValueRangeMax, 20.0f);
		}

		// An empty resource refuses rather than emitting a zeroed block that
		// would render a plausible wrong image.
		{
			FFlowVizTransferFunctionResource Fresh;
			FFlowVizTransferFunctionShaderParameters Params;
			TestFalse(TEXT("an empty resource makes no shader parameters"),
				Fresh.MakeShaderParameters(Params).IsOk());
		}

		Resource.ReleaseResources();
		FlushRenderingCommands();
	}

	/* == Device support, without a device ===================================== */
	{
		// Under -nullrhi there is no device to be incompatible with, and a phantom
		// incompatibility would fail every logic test for an unrelated reason.
		const FCFDVizResult Support = FlowVizTransferFunctionRHI::CheckDeviceSupport(256);
		if (!GIsRHIInitialized || GUsingNullRHI)
		{
			TestTrue(TEXT("with no device, support is not a failure"), Support.IsOk());
		}
		else
		{
			TestTrue(TEXT("a 256-entry LUT is supported on this device"), Support.IsOk());
		}

		// A bad size is refused whether or not a device exists.
		TestFalse(TEXT("a 1-entry LUT has no device support"),
			FlowVizTransferFunctionRHI::CheckDeviceSupport(1).IsOk());
		TestFalse(TEXT("an oversized LUT has no device support"),
			FlowVizTransferFunctionRHI::CheckDeviceSupport(
				FlowVizTransferFunction::MaxLutSize + 1).IsOk());
	}

	return true;
}

bool FFlowVizTransferFunctionDeviceTest::RunTest(const FString& Parameters)
{
	// SKIP WITH A LOGGED REASON, never a silent pass. A skipped check that
	// reports success is indistinguishable from a real one afterwards, which is
	// exactly the misattribution this repo's harness rules exist to refuse.
	if (!GIsRHIInitialized || GUsingNullRHI)
	{
		AddWarning(TEXT(
			"SKIPPED: FlowViz.Render.TransferFunctionDevice needs a real RHI device and this run has "
			"none (-nullrhi). Nothing about texture creation or upload was verified. "
			"Re-run with: RHI=1 Tools/build_lock.sh ./Tools/run_tests.sh FlowViz.Render"));
		return true;
	}

	FFlowVizTransferFunction Tf = FFlowVizTransferFunction::MakeDefault(
		ECFDVizColorMap::Viridis, 0.0f, 10.0f);
	Tf.Opacity = FFlowVizOpacityCurve::MakeLinearRamp();

	TArray<FLinearColor> Lut;
	if (!TestTrue(TEXT("the LUT builds"), FlowVizTransferFunction::BuildLut(Tf, Lut).IsOk()))
	{
		return false;
	}

	TArray<uint8> Packed;
	if (!TestTrue(TEXT("the LUT packs"), FlowVizTransferFunction::PackLutBytes(Lut, Packed).IsOk()))
	{
		return false;
	}

	TestTrue(TEXT("the device supports a 256-entry LUT texture"),
		FlowVizTransferFunctionRHI::CheckDeviceSupport(Tf.LutSize).IsOk());

	// Create and update on the render thread, which is the only thread these may
	// be called from.
	bool bCreated = false;
	bool bUpdated = false;
	FString CreateMessage;
	FString UpdateMessage;

	ENQUEUE_RENDER_COMMAND(FlowVizTransferFunctionDeviceTest)(
		[&bCreated, &bUpdated, &CreateMessage, &UpdateMessage, LutSize = Tf.LutSize, &Packed]
		(FRHICommandListImmediate& RHICmdList)
		{
			FCFDVizResult CreateResult;
			FTextureRHIRef Texture = FlowVizTransferFunctionRHI::CreateLutTexture(
				RHICmdList, LutSize, TEXT("FlowVizTransferFunctionTest"), CreateResult);
			bCreated = Texture.IsValid() && CreateResult.IsOk();
			CreateMessage = CreateResult.Message;
			if (!bCreated)
			{
				return;
			}

			const FCFDVizResult UpdateResult = FlowVizTransferFunctionRHI::UpdateLutTexture(
				RHICmdList, Texture.GetReference(), LutSize, Packed);
			bUpdated = UpdateResult.IsOk();
			UpdateMessage = UpdateResult.Message;

			// A short buffer must be refused BEFORE the RHI call, not read past.
			TArrayView<const uint8> Short(Packed.GetData(), Packed.Num() / 2);
			if (FlowVizTransferFunctionRHI::UpdateLutTexture(
					RHICmdList, Texture.GetReference(), LutSize, Short).IsOk())
			{
				UpdateMessage = TEXT("a short buffer was accepted");
				bUpdated = false;
			}

			Texture.SafeRelease();
		});
	FlushRenderingCommands();

	TestTrue(*FString::Printf(TEXT("the device created a %d x 1 PF_FloatRGBA LUT texture (%s)"),
		Tf.LutSize, *CreateMessage), bCreated);
	TestTrue(*FString::Printf(TEXT("the device accepted the LUT upload (%s)"), *UpdateMessage), bUpdated);

	// The resource's own create-once path, end to end.
	{
		FFlowVizTransferFunctionResource Resource;
		TestTrue(TEXT("the resource updates"), Resource.Update(Tf).IsOk());
		FlushRenderingCommands();
		TestNotNull(TEXT("...and holds a texture"), Resource.GetLutTexture());
		TestEqual(TEXT("...created once"), Resource.GetTextureCreateCount(), 1);

		// A changed curve at the same size must NOT recreate the texture -
		// engineering rule 2, no per-change resource recreation.
		FFlowVizTransferFunction Changed = Tf;
		Changed.Opacity.OpacityMultiplier = 0.3f;
		TestTrue(TEXT("a changed curve updates"), Resource.Update(Changed).IsOk());
		FlushRenderingCommands();
		TestEqual(TEXT("...without recreating the texture"), Resource.GetTextureCreateCount(), 1);

		// A changed SIZE is the one reason to recreate.
		FFlowVizTransferFunction Resized = Changed;
		Resized.LutSize = 512;
		TestTrue(TEXT("a resized LUT updates"), Resource.Update(Resized).IsOk());
		FlushRenderingCommands();
		TestEqual(TEXT("...and does recreate the texture"), Resource.GetTextureCreateCount(), 2);

		/*
		 * THE SECOND BRANCH OF THE NULL-TEXTURE EARLY RETURN.
		 *
		 * UploadOnRenderThread returns silently when LutTexture is invalid after
		 * the create block. That return has two causes and the code cannot tell
		 * them apart:
		 *
		 *   1. There is no device. The null RHI returns nothing from
		 *      CreateTexture, so silence is correct - logging every frame would
		 *      bury real output under a condition nobody can fix.
		 *   2. There IS a device and the create FAILED. That is a genuine GPU
		 *      failure, and the same early return swallows it.
		 *
		 * The comment on that return used to justify the silence by saying the
		 * CPU-side LUT "is what the legend and the probe readout sample."
		 * Neither a legend nor a probe readout exists anywhere in this plugin,
		 * so the justification was false and the silence covered case 2 as well
		 * as case 1 - a false all-clear buying quiet on a real failure path.
		 *
		 * We are on a real device here (this test skips under -nullrhi), so the
		 * texture must EXIST after every successful update. Asserting that is
		 * what makes case 2 observable: if a create silently fails on this
		 * device, GetLutTexture returns null and this goes red instead of the
		 * resource reporting Ok and rendering nothing.
		 *
		 * Checked after the RESIZE specifically, because the resize is the only
		 * path that releases a live texture and builds another. A create that
		 * fails there leaves the resource holding null while every count and
		 * every result code still reads exactly like success.
		 */
		TestNotNull(TEXT("after a resize on a real device the resource still holds a texture - "
						 "a null here is a silently swallowed GPU create failure, which the "
						 "counters and the FCFDVizResult both report as success"),
			Resource.GetLutTexture());

		Resource.ReleaseResources();
		FlushRenderingCommands();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
