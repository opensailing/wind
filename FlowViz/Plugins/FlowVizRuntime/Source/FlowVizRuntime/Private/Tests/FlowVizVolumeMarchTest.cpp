// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Render/FlowVizVolumeTexture.h"

#include "GlobalShader.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RHIGlobals.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

/**
 * The ray-march loop, over a volume that has data in it.
 *
 * WHY THIS FILE EXISTS SEPARATELY FROM FlowVizVolumeRayMarchTest.cpp. That file
 * proves the pass dispatches, the shader compiled on Metal, and the GPU reads
 * back the constant-buffer bytes the CPU wrote. It marches a 1x1x1 dummy
 * texture, so it says nothing about the loop. Compositing, the anisotropic step
 * rule, the crop box and the status gate all compile, all dispatch, and until
 * this file existed had never run over a populated volume. A loop that steps by
 * the largest voxel spacing instead of the smallest, or composites back to
 * front, still produces a picture - and some of those pictures look entirely
 * reasonable.
 *
 * THE FIELD IS ITS OWN VOXEL INDEX: f(i,j,k) = i + 16j + 128k. That single
 * choice is what makes every expectation below a closed form rather than a
 * judgement about an image:
 *
 *   - A sample's value DECODES to the voxel it was read from, so "the shader
 *     sampled a voxel" and "the shader sampled the RIGHT voxel" are different
 *     assertions and this file can make the second one.
 *   - Along a ray travelling in +X at fixed (j,k) the values are i + C for a
 *     constant C = 16j + 128k. So max - min == 15 EXACTLY, for every ray that
 *     crosses the domain, with no dependence on where the ray entered. The
 *     assertion needs no camera arithmetic and therefore does not restate the
 *     implementation it is checking.
 *   - The field VARIES ALONG THE VIEW DIRECTION. A field that is constant along
 *     the ray makes Maximum, Minimum and Average return the same number, which
 *     would make three composite modes an identity map on this fixture and let
 *     every mutation among them survive a green suite. The control below asserts
 *     the three are mutually distinct BEFORE anything else is believed.
 *
 * BECAUSE THE FIELD IS LINEAR, ITS GRADIENT IS A CONSTANT: (8, 32, 512) in
 * local units, everywhere in the domain. That is what makes lighting DIRECTION
 * checkable here. The three components are distinct powers of two in the ratio
 * 1 : 4 : 64, so a swapped axis in the gradient - which lights the volume from
 * the wrong side and looks entirely reasonable - moves a measured ratio by at
 * least 4x. See the gradient block for how three renders recover the normal.
 *
 * THE GEOMETRY IS DELIBERATELY ANISOTROPIC AND DELIBERATELY NOT UNIT. Spacing
 * (0.125, 0.5, 0.25) over 16 x 8 x 4 cells gives physical size (2, 4, 1) and
 * UVW scale (0.5, 0.25, 1.0) - three distinct spacings, three distinct extents,
 * three distinct scales, none derivable from another. A cubic fixture, or one
 * whose UVW scale came out (1,1,1), would be an identity map on the transform
 * and would pass against code that collapses any of these to a scalar. Every
 * constant is a negative power of two, so the expected values are exact in
 * float32 and no assertion here is a tolerance in disguise.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizVolumeMarchTest,
	"FlowViz.Render.VolumeMarch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizMarchFixture
{
	/** 16 x 8 x 4 cells. Distinct per axis, so an axis swap changes the answer. */
	static const FIntVector Extent(16, 8, 4);

	/** Distinct, non-unit, exact in binary. See the file comment for why each of those matters. */
	static const FVector Spacing(0.125, 0.5, 0.25);

	/** Output is square so the ortho frustum is square and the aspect term cannot hide a swap. */
	constexpr int32 OutputW = 32;
	constexpr int32 OutputH = 32;

	/** The whole point: a value names the voxel it came from. */
	static float FieldValue(int32 I, int32 J, int32 K)
	{
		return static_cast<float>(I + 16 * J + 128 * K);
	}

	/** Largest value the field takes, so the transfer function can span it without clipping. */
	constexpr float FieldMax = 511.0f;

	/**
	 * Entries in the fixture LUT, and the formula that fills it.
	 *
	 * HOISTED OUT OF THE RENDER LAMBDA ON PURPOSE. The clamp block below asserts
	 * that a clamped out-of-range sample draws the colormap's END - which means it
	 * needs to know what the LUT's first and last entries ARE. Restating them as
	 * literals next to the assertion would let the fixture and the expectation
	 * drift apart, and a drifted expectation fails for a reason that has nothing
	 * to do with clamping. One formula, two callers.
	 *
	 * GREEN IS THE CONSTANT 0.5 AT EVERY ENTRY and must stay that way: the
	 * lighting block recovers NdotL by dividing the green channel by it.
	 */
	constexpr int32 LutWidth = 16;

	static FLinearColor LutEntry(int32 Index)
	{
		const float T = static_cast<float>(Index) / static_cast<float>(LutWidth - 1);
		return FLinearColor(T, 0.5f, 1.0f - T, 0.25f);
	}

	/**
	 * The two out-of-range flag colours, chosen FAR FROM BOTH ENDS OF THE LUT.
	 *
	 * The clamp assertion is a difference between two renders, so its resolution
	 * is the distance between the flag colour and the colormap end that replaces
	 * it. FillDefaults' cyan (0, 0.85, 1) sits 0.35 from LUT entry 0 (0, 0.5, 1)
	 * in the GREEN CHANNEL ALONE, and its orange-red 0.15 from entry 15 - a
	 * one-channel margin, which a partially-applied fix could squeeze under.
	 * Pure red against (0, 0.5, 1) and pure green against (1, 0.5, 0) separate in
	 * more than one channel and by more than 0.5, so the difference is a fact
	 * about clamping rather than about the tolerance.
	 *
	 * They are also distinct FROM EACH OTHER, so a shader that drew the under
	 * colour for an over-range sample is a different answer and not a near miss.
	 */
	static const FLinearColor UnderFlagColor(1.0f, 0.0f, 0.0f, 1.0f);
	static const FLinearColor OverFlagColor(0.0f, 1.0f, 0.0f, 1.0f);

	/** Voxels with I < this are marked Masked in the second status volume. */
	constexpr int32 MaskedBelowI = 4;

	static FCFDVizGrid MakeGrid()
	{
		FCFDVizGrid Grid;
		Grid.Dimensions = Extent;
		Grid.Origin = FVector::ZeroVector;
		Grid.Spacing = Spacing;
		return Grid;
	}

	/**
	 * THE ANALYTIC GRADIENT, which is what makes the lighting-direction block
	 * possible at all.
	 *
	 * f = i + 16j + 128k is LINEAR, so df/di is 1, 16 and 128 per voxel and the
	 * gradient in local units is the same vector EVERYWHERE in the domain:
	 *
	 *     grad f = (1/0.125, 16/0.5, 128/0.25) = (8, 32, 512)
	 *
	 * Constant means the expected normal does not depend on where the ray hit,
	 * so a lighting assertion does not have to first prove the hit position.
	 * The three components are distinct powers of two in the ratio 1 : 4 : 64,
	 * so an axis swap is not a subtle shading difference - it is a different
	 * number, and the ratios below separate every swap by at least 4x.
	 */
	constexpr float GradX = 8.0f;
	constexpr float GradY = 32.0f;
	constexpr float GradZ = 512.0f;

	/** Matches FillDefaults. Read back out of NdotL, so they must agree. */
	constexpr float Ambient = 0.35f;
	constexpr float Diffuse = 0.65f;

	/** What the test dispatches. One entry per row of the results array. */
	struct FMarchConfig
	{
		const TCHAR* Name = nullptr;
		EFlowVizCompositeMode Mode = EFlowVizCompositeMode::Maximum;
		bool bMaskedStatus = false;
		bool bLighting = false;
		float CropMinX = 0.0f;

		/*
		 * Second pass only. The iso value is stated RELATIVE to the centre ray's
		 * row constant, which the test reads back from the first pass rather
		 * than deriving from the camera. An absolute iso value would require
		 * this file to recompute which (j,k) row the centre pixel lands on,
		 * i.e. to restate the framing code it is supposed to be checking.
		 */
		bool  bIso = false;
		float IsoOffset = 0.0f;
		FVector3f LightDir = FVector3f(0.0f, 0.0f, 1.0f);
		int32 NumClip = 0;
		FVector4f Clip0 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
		FVector4f Clip1 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);

		/*
		 * THE CLAMP ARM'S NARROWED DOMAIN.
		 *
		 * Every config above renders with ValueRangeMin = 0, ValueRangeMax =
		 * FieldMax, which spans the whole field precisely so nothing is out of
		 * range and the two range reason bits stay out of every comparison. The
		 * clamp flag is only observable on a sample the domain does NOT cover, so
		 * these configs opt into a narrower one. Defaulted to the full span, so
		 * every pass-one and pass-two config already written renders byte for byte
		 * as it did before this field existed.
		 */
		bool  bNarrowRange = false;
		float RangeMin = 0.0f;
		float RangeMax = FieldMax;
		bool  bClampToRange = false;

		/*
		 * THE SCENE-DEPTH ARM (P6 clamp; findings F2/F3/F6). When bound, the
		 * shader clamps each ray's TMax at the opaque surface read from
		 * SceneDepthTexture -- and until this fixture existed, NO test had ever
		 * run that path on a GPU: every RHI test called AddRayMarchPass with a
		 * null depth (the pass's own 1x1 dummy, bHasSceneDepth=0), so the
		 * DeviceZ conversion, the CosTheta division and the texel addressing all
		 * compiled, dispatched, and had never executed against data.
		 *
		 * The fixture's depth texture is 64x64 -- LARGER than the 32x32 dispatch,
		 * as a screen-percentage-scaled scene texture is -- holding PlaneDeviceZ
		 * in the quadrant x>=32 && y>=32 and NearDeviceZ elsewhere. DepthRectMin
		 * points the dispatch into that quadrant, so a shader that ignored
		 * ViewRectMin reads NearDeviceZ (clamps every ray to nothing, zero
		 * steps) while correct addressing reads the plane and clamps at a known
		 * voxel. DeviceZToViewZ is packed ORTHO-SHAPED, (1, 1, 0, 1): under the
		 * engine's branchless formula ViewZ = z*1 + 1 + 1/(0 - 1) = z exactly,
		 * while the old `w != 0 selects perspective` branch returns the
		 * constant 1 -- so this arm also fails against the F2 bug on the GPU.
		 */
		bool bBindDepth = false;
		FVector2f DepthRectMin = FVector2f(0.0f, 0.0f);

		/*
		 * F3's GPU arm. SetLookAtCamera writes a UNIT forward; production's
		 * SetViewCamera deliberately does not (the local basis carries the
		 * placement scale, 0.01 under the standard metres-to-centimetres
		 * placement). Scaling ONLY the forward leaves the marched geometry
		 * untouched -- the shader normalises the ray direction -- but a CosTheta
		 * computed with the RAW forward scales by this factor and moves the
		 * opaque clamp by 1/scale. 100 here mirrors the real placement's cancel:
		 * with the normalize() fix the depth arm reports the same voxel as
		 * ForwardScale=1; without it OpaqueT shrinks 100x below TMin and the
		 * ray dies with zero steps.
		 */
		float ForwardScale = 1.0f;

		/*
		 * THE SHADOW HOLD. P7 added single-scatter self-shadowing to
		 * FlowVizApplyLighting: the diffuse term is attenuated by a coarse march
		 * toward the light, and that attenuation is DIRECTION-DEPENDENT. From the
		 * centre iso hit, a +Y shadow ray crosses three in-volume steps, a +X ray
		 * one and a +Z ray none, so the three axis-lit renders the gradient block
		 * compares stopped measuring NdotL and started measuring
		 * NdotL * 0.75^steps. Measured: X:Y moved from the analytic 0.25 to
		 * 0.4444 (= 0.25 * 0.75 / 0.75^3) and Y:Z from 0.0625 to 0.026367
		 * (= 0.0625 * 0.75^3) -- exactly the per-step shadow factor
		 * (1 - LutAlpha 0.25 * OpacityMultiplier 1), which is how the cause was
		 * attributed to shadowing rather than to the gradient.
		 *
		 * FlowVizShadowTransmittance multiplies by
		 * (1 - Shaded.a * OpacityMultiplier) per step, so OpacityMultiplier = 0
		 * forces the transmittance to exactly 1, while iso-mode compositing --
		 * which writes Color = (rgb, 1) directly -- never reads the multiplier.
		 * The gradient block HOLDS the shadow term this way; self-shadowing is
		 * P7's feature, not the property under test there.
		 */
		float OpacityMul = 1.0f;
	};

	/** A single dispatch's readback, both targets. */
	struct FMarchResult
	{
		TArray<FLinearColor> Value;
		TArray<FLinearColor> Color;
		bool bDispatched = false;

		const FLinearColor& At(const TArray<FLinearColor>& Image, int32 X, int32 Y) const
		{
			return Image[Y * OutputW + X];
		}
		const FLinearColor& ValueAt(int32 X, int32 Y) const { return At(Value, X, Y); }
		const FLinearColor& ColorAt(int32 X, int32 Y) const { return At(Color, X, Y); }
	};

	/**
	 * The diffuse multiplier the shader applied, recovered from a rendered pixel.
	 *
	 * The LUT's GREEN channel is the constant 0.5 at every entry, so green is
	 * 0.5 * (Ambient + Diffuse * NdotL) and NOTHING ELSE. Red and blue vary with
	 * the LUT index and would confound a change in shading with a change in the
	 * sampled value; green cannot. This is what lets the block below read NdotL
	 * off an image without inverting the transfer function.
	 */
	static float ShadeMultiplier(const FLinearColor& Pixel) { return Pixel.G / 0.5f; }

	/** The NdotL that produced a given multiplier. Inverse of Ambient + Diffuse * NdotL. */
	static float RecoveredNdotL(const FLinearColor& Pixel)
	{
		return (ShadeMultiplier(Pixel) - Ambient) / Diffuse;
	}

	/** OutValue channel meanings, named rather than spelled .R/.G/.B/.A at every use. */
	static float ReportedValue(const FLinearColor& Pixel) { return Pixel.R; }
	static float ReportedAlpha(const FLinearColor& Pixel) { return Pixel.G; }

	/**
	 * The reason bits, decoded by THE SHIPPED DECODER.
	 *
	 * This calls FlowVizRayMarch::DecodeReason rather than rounding the float
	 * here, and that is the point. Until it did, DecodeReason had ZERO call sites
	 * in the entire plugin - not one, tests included - while this file read
	 * OutValue.z through a local helper and hand-written masks (& 1u, & 4u). So
	 * two things were true at once: the shipped decoder was never executed by
	 * anything, and the assertions never mentioned EFlowVizInvalidReason at all.
	 *
	 * A decoder nothing calls cannot be observed to be wrong. The +0.5 truncation
	 * inside it could round the wrong way, or the enum could be renumbered, and
	 * every test here would still pass while every SHIPPING caller read the wrong
	 * cause off the same pixel. Routing the assertions through it makes the
	 * decoder part of what is under test instead of a promise about it.
	 *
	 * FlowViz.Render.ReasonCodes pins the other half: that these C++ enumerators
	 * are the numbers the .usf writes.
	 */
	static EFlowVizInvalidReason ReasonOf(const FLinearColor& Pixel)
	{
		return FlowVizRayMarch::DecodeReason(Pixel.B);
	}

	/** True when the ray reported this cause. A named enumerator, never a bare mask. */
	static bool HasReason(const FLinearColor& Pixel, EFlowVizInvalidReason Reason)
	{
		return EnumHasAnyFlags(ReasonOf(Pixel), Reason);
	}

	/** The raw bits, for a message that prints what was actually found. */
	static uint32 ReasonBits(const FLinearColor& Pixel)
	{
		return static_cast<uint32>(ReasonOf(Pixel));
	}

	static int32 StepCount(const FLinearColor& Pixel) { return FMath::RoundToInt(Pixel.A); }
}

bool FFlowVizVolumeMarchTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizMarchFixture;

	// SKIP WITH A LOGGED REASON, never a silent pass. A green total that
	// includes a test which verified nothing is the failure this repo keeps
	// hitting; Tools/run_tests.sh scrapes this marker and reports it by name.
	if (!GIsRHIInitialized || GUsingNullRHI)
	{
		AddWarning(TEXT(
			"SKIPPED: FlowViz.Render.VolumeMarch needs a real RHI device and this run has none "
			"(-nullrhi). NOTHING about the marching loop was verified - not the step rule, not "
			"compositing, not the crop box, not the status gate. "
			"Re-run with: RHI=1 Tools/build_lock.sh ./Tools/run_tests.sh FlowViz.Render"));
		return true;
	}

	/* == The fixture ========================================================= */

	FFlowVizVolumeTransform Transform;
	Transform.Grid = MakeGrid();
	Transform.Association = ECFDVizAssociation::Cell;

	FFlowVizVolumeLayout FieldLayout;
	if (!TestTrue(TEXT("the fixture's field layout is accepted"),
			FFlowVizVolumeLayout::Make(Extent, 1, ECFDVizDataType::Float32, FieldLayout).IsOk()))
	{
		return false;
	}

	FFlowVizVolumeLayout StatusLayout;
	if (!TestTrue(TEXT("the fixture's status layout is accepted"),
			FFlowVizVolumeLayout::MakeStatusLayout(Extent, StatusLayout).IsOk()))
	{
		return false;
	}

	FFlowVizVolumeShaderParameters VolumeParams;
	if (!TestTrue(TEXT("the fixture builds shader parameters"),
			Transform.MakeShaderParametersWithoutValueRange(FieldLayout, VolumeParams).IsOk()))
	{
		return false;
	}

	// Field bytes, written through the layout's own offsets rather than a
	// hand-rolled i + W*(j + H*k): if the layout pads or reorders, the fixture
	// follows it, and a mismatch between the two is the bug this cannot mask.
	TArray<uint8> FieldBytes;
	FieldBytes.SetNumZeroed(static_cast<int32>(FieldLayout.GetTextureVolumeBytes()));
	for (int32 K = 0; K < Extent.Z; ++K)
	{
		for (int32 J = 0; J < Extent.Y; ++J)
		{
			for (int32 I = 0; I < Extent.X; ++I)
			{
				const float Value = FieldValue(I, J, K);
				const int64 Offset = FieldLayout.GetTextureVoxelOffset(I, J, K);
				FMemory::Memcpy(FieldBytes.GetData() + Offset, &Value, sizeof(float));
			}
		}
	}

	TArray<uint8> ValidStatusBytes;
	TArray<uint8> MaskedStatusBytes;
	ValidStatusBytes.SetNumZeroed(static_cast<int32>(StatusLayout.GetTextureVolumeBytes()));
	MaskedStatusBytes.SetNumZeroed(static_cast<int32>(StatusLayout.GetTextureVolumeBytes()));
	for (int32 K = 0; K < Extent.Z; ++K)
	{
		for (int32 J = 0; J < Extent.Y; ++J)
		{
			for (int32 I = 0; I < Extent.X; ++I)
			{
				const int64 Offset = StatusLayout.GetTextureVoxelOffset(I, J, K);
				ValidStatusBytes[static_cast<int32>(Offset)] = FlowVizVoxelStatus::Valid;
				MaskedStatusBytes[static_cast<int32>(Offset)] =
					(I < MaskedBelowI) ? FlowVizVoxelStatus::Masked : FlowVizVoxelStatus::Valid;
			}
		}
	}

	/* == The fixture is not degenerate ======================================= */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a fixture on which the
	 * assertions below cannot fail.
	 *
	 * A survivor can mean the mutant is an identity map on the data, not that
	 * the code is right. Every property the GPU assertions rely on is checked
	 * here first, on the CPU, where a failure names the fixture rather than the
	 * shader.
	 */
	{
		TestTrue(TEXT("the three voxel spacings are mutually distinct, so code that collapses "
					  "spacing to a scalar cannot pass"),
			Spacing.X != Spacing.Y && Spacing.Y != Spacing.Z && Spacing.X != Spacing.Z);

		TestTrue(TEXT("the three UVW scales are mutually distinct AND none is 1.0 - a unit scale "
					  "would make a dropped UVWScale an identity map"),
			VolumeParams.UVWScale.X != VolumeParams.UVWScale.Y
				&& VolumeParams.UVWScale.Y != VolumeParams.UVWScale.Z
				&& !FMath::IsNearlyEqual(VolumeParams.UVWScale.X, 1.0f)
				&& !FMath::IsNearlyEqual(VolumeParams.UVWScale.Y, 1.0f));

		TestTrue(TEXT("min and max voxel spacing differ by 4x, so stepping by the wrong one "
					  "changes the step count by 4x rather than by rounding"),
			FMath::IsNearlyEqual(VolumeParams.MaxVoxelSpacing / VolumeParams.MinVoxelSpacing, 4.0f));

		// The invariant every GPU assertion below rests on, verified on the data
		// itself: along +X at fixed (j,k) the field spans exactly 15.
		bool bAllRowsSpan15 = true;
		for (int32 K = 0; K < Extent.Z && bAllRowsSpan15; ++K)
		{
			for (int32 J = 0; J < Extent.Y && bAllRowsSpan15; ++J)
			{
				bAllRowsSpan15 = FMath::IsNearlyEqual(
					FieldValue(Extent.X - 1, J, K) - FieldValue(0, J, K), 15.0f);
			}
		}
		TestTrue(TEXT("every X row of the fixture spans exactly 15, which is the closed form the "
					  "traversal assertions compare against"),
			bAllRowsSpan15);

		TestTrue(TEXT("distinct (j,k) rows carry distinct constants, so a ray that sampled the "
					  "wrong row reports a different number rather than the same one"),
			FieldValue(0, 1, 0) != FieldValue(0, 0, 0)
				&& FieldValue(0, 0, 1) != FieldValue(0, 0, 0)
				&& FieldValue(0, 0, 1) != FieldValue(0, 1, 0));
	}

	/* == The configurations to render ======================================== */

	const FMarchConfig Configs[] = {
		{ TEXT("Maximum"),      EFlowVizCompositeMode::Maximum },
		{ TEXT("Minimum"),      EFlowVizCompositeMode::Minimum },
		{ TEXT("Average"),      EFlowVizCompositeMode::Average },
		{ TEXT("MinimumCrop"),  EFlowVizCompositeMode::Minimum, false, false, 0.5f },
		{ TEXT("MinimumMask"),  EFlowVizCompositeMode::Minimum, true },
		{ TEXT("AlphaUnlit"),   EFlowVizCompositeMode::Alpha },
		{ TEXT("AlphaLit"),     EFlowVizCompositeMode::Alpha,   false, true },
	};
	constexpr int32 NumConfigs = UE_ARRAY_COUNT(Configs);

	enum { CfgMax = 0, CfgMin = 1, CfgAvg = 2, CfgCrop = 3, CfgMask = 4, CfgUnlit = 5, CfgLit = 6 };

	TArray<FMarchResult> Results;
	Results.SetNum(NumConfigs);

	FString SetupError;

	// Filled by pass one, consumed by pass two. See FMarchConfig::IsoOffset for
	// why the iso configs cannot be built until the first pass has run.
	TArray<FMarchConfig> SecondPassConfigs;
	TArray<FMarchResult> SecondPassResults;

	// One render command, parameterised by which config list to run, so pass two
	// goes through byte-for-byte the same setup, upload, dispatch and readback as
	// pass one. A second bespoke path here could differ from the first in a way
	// that made the iso results incomparable with the values they are stated
	// relative to.
	const auto RunPass = [&](const TArray<FMarchConfig>& PassConfigs, TArray<FMarchResult>& PassResults)
	{
	ENQUEUE_RENDER_COMMAND(FlowVizVolumeMarch)(
		[&](FRHICommandListImmediate& RHICmdList)
		{
			FCFDVizResult Created = FCFDVizResult::Ok();
			FTextureRHIRef FieldTexture = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, FieldLayout, TEXT("FlowVizMarchField"), Created);
			if (!FieldTexture.IsValid())
			{
				SetupError = TEXT("field texture: ") + Created.Message;
				return;
			}
			FTextureRHIRef ValidStatusTexture = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, StatusLayout, TEXT("FlowVizMarchStatusValid"), Created);
			FTextureRHIRef MaskedStatusTexture = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, StatusLayout, TEXT("FlowVizMarchStatusMasked"), Created);
			if (!ValidStatusTexture.IsValid() || !MaskedStatusTexture.IsValid())
			{
				SetupError = TEXT("status texture: ") + Created.Message;
				return;
			}

			// The production upload path, not a bespoke test one, so a bug in
			// stride or bounds is a bug this test can see.
			const FCFDVizResult FieldUploaded = FlowVizVolumeRHI::UpdateVolumeTexture(
				RHICmdList, FieldTexture, FieldLayout, FieldBytes);
			const FCFDVizResult ValidUploaded = FlowVizVolumeRHI::UpdateVolumeTexture(
				RHICmdList, ValidStatusTexture, StatusLayout, ValidStatusBytes);
			const FCFDVizResult MaskedUploaded = FlowVizVolumeRHI::UpdateVolumeTexture(
				RHICmdList, MaskedStatusTexture, StatusLayout, MaskedStatusBytes);
			if (!FieldUploaded.IsOk() || !ValidUploaded.IsOk() || !MaskedUploaded.IsOk())
			{
				SetupError = TEXT("upload: ") + FieldUploaded.Message + ValidUploaded.Message
					+ MaskedUploaded.Message;
				return;
			}

			// A 16-entry LUT with CONSTANT non-zero alpha. Constant because the
			// alpha arm below compares a lit and an unlit render, and an alpha
			// that varied with value would let a lighting change masquerade as
			// an opacity change. LutEntry is shared with the clamp block, which
			// asserts against the colormap's ENDS and must not restate them.
			FTextureRHIRef LutTexture;
			{
				const FRHITextureCreateDesc LutDesc =
					FRHITextureCreateDesc::Create2D(TEXT("FlowVizMarchLut"))
						.SetExtent(LutWidth, 1)
						.SetFormat(PF_FloatRGBA)
						.SetFlags(ETextureCreateFlags::ShaderResource);
				LutTexture = RHICmdList.CreateTexture(LutDesc);

				TArray<FFloat16Color> Lut;
				Lut.SetNum(LutWidth);
				for (int32 Index = 0; Index < LutWidth; ++Index)
				{
					Lut[Index] = FFloat16Color(LutEntry(Index));
				}
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, LutWidth, 1);
				RHICmdList.UpdateTexture2D(LutTexture, 0, Region,
					LutWidth * sizeof(FFloat16Color), reinterpret_cast<const uint8*>(Lut.GetData()));
			}

			// The scene-depth fixture is larger than the dispatch, as a scaled
			// renderer scene texture can be. Only its far quadrant contains the
			// opaque plane; the rest clamps before the volume. ViewRectMin is what
			// makes the two regions distinguishable to the shader.
			FTextureRHIRef SceneDepthTexture;
			{
				constexpr int32 DepthSize = 64;
				constexpr int32 DepthRectOffset = 32;
				const FRHITextureCreateDesc DepthDesc =
					FRHITextureCreateDesc::Create2D(TEXT("FlowVizMarchSceneDepth"))
						.SetExtent(DepthSize, DepthSize)
						.SetFormat(PF_R32_FLOAT)
						.SetFlags(ETextureCreateFlags::ShaderResource);
				SceneDepthTexture = RHICmdList.CreateTexture(DepthDesc);
				if (!SceneDepthTexture.IsValid())
				{
					SetupError = TEXT("scene depth texture creation failed");
					return;
				}

				const float PlaneDeviceZ = VolumeParams.PhysicalSize.Size() * 0.5f;
				TArray<float> Depth;
				Depth.SetNumUninitialized(DepthSize * DepthSize);
				for (int32 Y = 0; Y < DepthSize; ++Y)
				{
					for (int32 X = 0; X < DepthSize; ++X)
					{
						Depth[Y * DepthSize + X] =
							(X >= DepthRectOffset && Y >= DepthRectOffset)
								? PlaneDeviceZ
								: 0.0f;
					}
				}
				const FUpdateTextureRegion2D DepthRegion(0, 0, 0, 0, DepthSize, DepthSize);
				RHICmdList.UpdateTexture2D(
					SceneDepthTexture, 0, DepthRegion,
					DepthSize * sizeof(float),
					reinterpret_cast<const uint8*>(Depth.GetData()));
			}

			for (int32 ConfigIndex = 0; ConfigIndex < PassConfigs.Num(); ++ConfigIndex)
			{
				const FMarchConfig& Config = PassConfigs[ConfigIndex];
				FMarchResult& Result = PassResults[ConfigIndex];

				FRDGBuilder GraphBuilder(RHICmdList);

				// Float32 targets. OutValue.x carries a field value in solver
				// units; an 8- or 16-bit target would quantise it.
				const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
					FIntPoint(OutputW, OutputH),
					PF_A32B32G32R32F,
					FClearValueBinding::None,
					TexCreate_ShaderResource | TexCreate_UAV);

				FRDGTextureRef ColorTexture = GraphBuilder.CreateTexture(Desc, TEXT("MarchColor"));
				FRDGTextureRef ValueTexture = GraphBuilder.CreateTexture(Desc, TEXT("MarchValue"));

				FFlowVizVolumeRayMarchParameters* Params =
					GraphBuilder.AllocParameters<FFlowVizVolumeRayMarchParameters>();
				FlowVizRayMarch::FillDefaults(*Params);
				FlowVizRayMarch::FillFromVolumeParameters(VolumeParams, *Params);

				// JITTER OFF, EXPLICITLY. This fixture asserts exact values at
				// exact voxels; P6's jitter-on default would shift sample
				// positions sub-step and move the maxima the assertions name.
				// The default's own pin lives in the RayMarchShader test --
				// here it is the independent variable being HELD, not tested.
				Params->bEnableJitter = 0;

				// Looking down +X: the axis the field varies along, so the
				// composite modes see a changing value and are not an identity
				// map on this fixture.
				FlowVizRayMarch::SetLookAtCamera(*Params, FVector3f(1.0f, 0.0f, 0.0f),
					FIntPoint(OutputW, OutputH), /*bOrthographic=*/true,
					/*DistanceScale=*/1.0f, /*HorizontalFovDegrees=*/60.0f);
				Params->RayCameraForward *= Config.ForwardScale;

				if (Config.bBindDepth)
				{
					Params->SceneDepthTexture = RegisterExternalTexture(
						GraphBuilder, SceneDepthTexture.GetReference(), TEXT("FlowVizMarchSceneDepth"));
					Params->bHasSceneDepth = 1;
					Params->DeviceZToViewZ = FVector4f(1.0f, 1.0f, 0.0f, 1.0f);
					Params->DepthToSolver = 1.0f;
					Params->ViewRectMin = Config.DepthRectMin;
				}

				Params->CompositeMode = static_cast<uint32>(Config.Mode);
				Params->bEnableLighting = Config.bLighting ? 1u : 0u;
				// 1.0 for every pass-one config (identical to FillDefaults, so
				// nothing pre-existing changes). 0 only on the gradient configs,
				// which hold P7's self-shadow term -- see FMarchConfig::OpacityMul.
				Params->OpacityMultiplier = Config.OpacityMul;
				Params->CropBoxMin = FVector3f(Config.CropMinX, 0.0f, 0.0f);
				Params->CropBoxMax = FVector3f(1.0f, 1.0f, 1.0f);

				// Second-pass fields. Zero-valued for every pass-one config, so
				// the pass-one results are unchanged by this block existing.
				Params->IsoValue = Config.IsoOffset;
				Params->LightDirection = Config.LightDir.GetSafeNormal();
				Params->NumClipPlanes = static_cast<uint32>(Config.NumClip);
				Params->ClipPlanes[0] = Config.Clip0;
				Params->ClipPlanes[1] = Config.Clip1;

				// NEAREST, not filtered. A filtered fetch blends neighbouring
				// voxels, and then a sample no longer decodes to the voxel it
				// came from - which is the property every assertion here uses.
				Params->bFilterField = 0;

				// Spans the whole field by default, so nothing is under or over
				// range and those reason bits stay out of the comparison. Only the
				// clamp configs narrow it, because the clamp flag is unobservable
				// on a domain that covers every sample.
				Params->ValueRangeMin = Config.bNarrowRange ? Config.RangeMin : 0.0f;
				Params->ValueRangeMax = Config.bNarrowRange ? Config.RangeMax : FieldMax;
				Params->bClampToRange = Config.bClampToRange ? 1u : 0u;

				// The two flag colours the clamp block measures against. Set for
				// EVERY config, not only the clamped ones: a value set on one arm
				// of a differential pair and not the other would produce a colour
				// difference that had nothing to do with the flag under test.
				Params->UnderRangeColor = UnderFlagColor;
				Params->OverRangeColor = OverFlagColor;

				FlowVizRayMarch::SetVolumeTextures(*Params, FieldTexture,
					Config.bMaskedStatus ? MaskedStatusTexture : ValidStatusTexture, false);
				Params->TransferFunctionTexture = LutTexture;

				Result.bDispatched = FlowVizRayMarch::AddRayMarchPass(
					GraphBuilder, GMaxRHIFeatureLevel, /*bFieldIsUint=*/false,
					Params, ColorTexture, ValueTexture);
				if (!Result.bDispatched)
				{
					GraphBuilder.Execute();
					continue;
				}

				FRHIGPUTextureReadback* ValueReadback =
					new FRHIGPUTextureReadback(TEXT("MarchValueReadback"));
				FRHIGPUTextureReadback* ColorReadback =
					new FRHIGPUTextureReadback(TEXT("MarchColorReadback"));
				AddEnqueueCopyPass(GraphBuilder, ValueReadback, ValueTexture);
				AddEnqueueCopyPass(GraphBuilder, ColorReadback, ColorTexture);
				GraphBuilder.Execute();

				// Reading before the GPU has finished returns whatever the
				// staging buffer held, which is a plausible-looking lie.
				RHICmdList.SubmitAndBlockUntilGPUIdle();

				const auto CopyOut = [](FRHIGPUTextureReadback* Readback, TArray<FLinearColor>& Out)
				{
					int32 RowPitch = 0;
					int32 BufferHeight = 0;
					void* Mapped = Readback->Lock(RowPitch, &BufferHeight);
					if (Mapped != nullptr)
					{
						const FLinearColor* Pixels = static_cast<const FLinearColor*>(Mapped);
						Out.SetNumUninitialized(OutputW * OutputH);
						for (int32 Y = 0; Y < OutputH; ++Y)
						{
							for (int32 X = 0; X < OutputW; ++X)
							{
								// Row pitch is in PIXELS and is not the width.
								Out[Y * OutputW + X] = Pixels[Y * RowPitch + X];
							}
						}
						Readback->Unlock();
					}
				};
				CopyOut(ValueReadback, Result.Value);
				CopyOut(ColorReadback, Result.Color);
				delete ValueReadback;
				delete ColorReadback;
			}
		});

	FlushRenderingCommands();
	};

	{
		TArray<FMarchConfig> FirstPass;
		FirstPass.Append(Configs, NumConfigs);
		RunPass(FirstPass, Results);
	}

	if (!SetupError.IsEmpty())
	{
		AddError(FString::Printf(
			TEXT("The fixture volume could not be created or uploaded, so NOTHING about the "
				 "marching loop was verified. This is a harness failure, not a pass: %s"),
			*SetupError));
		return false;
	}

	for (int32 ConfigIndex = 0; ConfigIndex < NumConfigs; ++ConfigIndex)
	{
		if (!TestTrue(*FString::Printf(TEXT("config '%s' dispatched - false here means the "
										   "shader is not in the global shader map, i.e. it "
										   "FAILED TO COMPILE"), Configs[ConfigIndex].Name),
				Results[ConfigIndex].bDispatched))
		{
			return false;
		}
		if (!TestEqual(*FString::Printf(TEXT("config '%s' read back a full image"),
				Configs[ConfigIndex].Name),
				Results[ConfigIndex].Value.Num(), OutputW * OutputH))
		{
			AddError(TEXT("The readback returned no pixels. Nothing below was verified."));
			return false;
		}
	}

	/* == The image has structure ============================================= */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: an empty frame, and a
	 * frame where every ray hits.
	 *
	 * Read this first. "Minimum returns 0" is satisfied by a cleared buffer, so
	 * every value assertion below is worthless until something is known to have
	 * marched. The domain occupies part of the frustum and not all of it, so a
	 * correct render has both hitting and missing pixels; an all-hit image means
	 * the box intersection accepted rays it should have rejected, and an all-miss
	 * image means nothing rendered at all.
	 */
	int32 CentreX = OutputW / 2;
	int32 CentreY = OutputH / 2;
	{
		const FMarchResult& Max = Results[CfgMax];

		int32 HitPixels = 0;
		for (int32 Y = 0; Y < OutputH; ++Y)
		{
			for (int32 X = 0; X < OutputW; ++X)
			{
				if (StepCount(Max.ValueAt(X, Y)) > 0)
				{
					++HitPixels;
				}
			}
		}

		TestTrue(*FString::Printf(TEXT("some rays marched the volume (%d of %d pixels took at "
									   "least one step) - zero here means NOTHING rendered and "
									   "every assertion below would be vacuous"),
					HitPixels, OutputW * OutputH),
			HitPixels > 20);

		TestTrue(*FString::Printf(TEXT("...and some rays missed it (%d of %d hit) - an all-hit "
									   "image means the box intersection accepted rays outside "
									   "the domain"),
					HitPixels, OutputW * OutputH),
			HitPixels < OutputW * OutputH);

		if (!TestTrue(TEXT("the centre pixel is one of the hits, so the per-pixel assertions "
						   "below are made against a ray that actually crossed the volume"),
				StepCount(Max.ValueAt(CentreX, CentreY)) > 0))
		{
			return false;
		}

		TestTrue(TEXT("the centre ray reports REASON_VALID - it sampled data, rather than "
					  "crossing the box and finding every voxel refused"),
			HasReason(Max.ValueAt(CentreX, CentreY), EFlowVizInvalidReason::Valid));

		// The corner is outside the domain in both screen axes for this framing.
		const FLinearColor& Corner = Max.ValueAt(0, 0);
		TestEqual(TEXT("a corner ray misses the domain entirely and takes zero steps"),
			StepCount(Corner), 0);
		TestEqual(TEXT("...and reports REASON_NONE, which is 'the ray never entered' and is a "
					   "different answer from 'entered and found nothing'"),
			static_cast<int32>(ReasonBits(Corner)), 0);
	}

	/* == The ray crossed the whole domain ==================================== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a loop that stops early,
	 * starts late, or samples the wrong row.
	 *
	 * max - min == 15 is the closed form derived in the file comment. It holds
	 * for every crossing ray regardless of entry point, so this compares against
	 * arithmetic rather than against the camera code that produced the ray.
	 */
	{
		const float MaxValue = ReportedValue(Results[CfgMax].ValueAt(CentreX, CentreY));
		const float MinValue = ReportedValue(Results[CfgMin].ValueAt(CentreX, CentreY));
		const float AvgValue = ReportedValue(Results[CfgAvg].ValueAt(CentreX, CentreY));

		TestEqual(TEXT("the ray spans exactly 15 field units, i.e. it visited BOTH the first and "
					   "the last voxel along X. A short span means the loop stopped early; a "
					   "larger one means it sampled more than one row"),
			static_cast<double>(MaxValue - MinValue), 15.0, 1e-4);

		// The constant identifies the row. Decoding it proves the ray stayed in
		// one (j,k) row and that the row is a real one.
		const int32 RowConstant = FMath::RoundToInt(MinValue);
		TestEqual(TEXT("the ray's first voxel is X index 0 - the value decodes to i=0, so the "
					   "march began at the domain boundary rather than partway in"),
			RowConstant % 16, 0);

		const int32 DecodedJ = (RowConstant / 16) % 8;
		const int32 DecodedK = RowConstant / 128;
		TestTrue(*FString::Printf(TEXT("the reported value decodes to a real voxel row "
									   "(j=%d, k=%d) rather than to a blended or out-of-range "
									   "index"), DecodedJ, DecodedK),
			DecodedJ >= 0 && DecodedJ < Extent.Y && DecodedK >= 0 && DecodedK < Extent.Z);

		// --- THE CONTROL ---
		// If the three composite modes agree, the field does not vary along the
		// view direction and Maximum, Minimum and Average are the same function
		// on this fixture. Every mutation among them would survive. Nothing
		// above this line is worth anything without it.
		TestTrue(*FString::Printf(
				TEXT("CONTROL: Maximum (%.1f), Minimum (%.1f) and Average (%.1f) are mutually "
					 "DISTINCT. If they were equal the field would not vary along the ray, the "
					 "three modes would be an identity map on this fixture, and every assertion "
					 "distinguishing them would be vacuous"),
				MaxValue, MinValue, AvgValue),
			MaxValue != MinValue && AvgValue != MaxValue && AvgValue != MinValue);

		TestTrue(*FString::Printf(TEXT("Average lies strictly between Minimum and Maximum (%.3f)"),
					AvgValue),
			AvgValue > MinValue && AvgValue < MaxValue);

		// The mean of i over a ray that samples the 16 voxels near-uniformly is
		// 7.5. A mode returning the sum would give ~250, the first sample 0, the
		// last 15 - all excluded by this band.
		TestTrue(*FString::Printf(TEXT("Average is the MEAN of the samples, not their sum and "
									   "not an endpoint (offset from row constant: %.3f, "
									   "expected near 7.5)"), AvgValue - MinValue),
			FMath::Abs((AvgValue - MinValue) - 7.5f) < 1.5f);

		// Different rows must report different constants, or the image is one
		// value painted everywhere and "it resolved the volume" is unproven.
		TSet<int32> DistinctRows;
		for (int32 Y = 0; Y < OutputH; ++Y)
		{
			for (int32 X = 0; X < OutputW; ++X)
			{
				const FLinearColor& Pixel = Results[CfgMin].ValueAt(X, Y);
				if (StepCount(Pixel) > 0 && HasReason(Pixel, EFlowVizInvalidReason::Valid))
				{
					DistinctRows.Add(FMath::RoundToInt(ReportedValue(Pixel)));
				}
			}
		}
		TestTrue(*FString::Printf(TEXT("different pixels resolve different voxel rows (%d "
									   "distinct row constants). One distinct value would mean "
									   "the image is a flat fill that happens to be in range"),
					DistinctRows.Num()),
			DistinctRows.Num() > 1);
	}

	/* == The step obeys the anisotropy rule ================================== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a step scaled by the
	 * largest voxel spacing instead of the smallest.
	 *
	 * That bug renders a complete, plausible, smoothly-shaded image. It is
	 * invisible to any assertion about colour and to every existing test in this
	 * repo. Here it is a NUMBER: the domain is 2.0 across in X, the step is
	 * StepVoxels * MinVoxelSpacing = 0.5 * 0.125 = 0.0625, so a crossing ray
	 * takes 2.0/0.0625 + 1 = 33 steps. Scaled by the largest spacing instead the
	 * step is 0.25 and the ray takes 9. The fixture's 4x spacing ratio is what
	 * makes those two answers impossible to confuse.
	 *
	 * Maximum mode, deliberately: alpha compositing terminates early once the
	 * ray saturates, so its step count measures opacity rather than the step
	 * rule.
	 */
	{
		const int32 Steps = StepCount(Results[CfgMax].ValueAt(CentreX, CentreY));

		constexpr int32 ExpectedSteps = 33;  // 2.0 / (0.5 * 0.125) + 1
		constexpr int32 MaxSpacingSteps = 9; // 2.0 / (0.5 * 0.5)   + 1

		TestTrue(*FString::Printf(
				TEXT("the ray took %d steps, matching the SMALLEST voxel spacing (expected %d "
					 "+/-1). %d steps would mean the step was scaled by the LARGEST spacing, "
					 "which undersamples the thin axis and aliases while still producing a "
					 "plausible image"),
				Steps, ExpectedSteps, MaxSpacingSteps),
			FMath::Abs(Steps - ExpectedSteps) <= 1);

		// Stated separately so the failure message names the bug even if the
		// exact count drifts for an unrelated reason.
		TestTrue(*FString::Printf(TEXT("...and in particular took far more than the %d steps the "
									   "max-spacing bug produces"), MaxSpacingSteps),
			Steps > 2 * MaxSpacingSteps);
	}

	/* == The crop box removes domain, in domain fractions ==================== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a crop box applied in the
	 * wrong space, or not at all.
	 *
	 * CropBoxMin.x = 0.5 is half the domain, which is local x >= 1.0, which is
	 * voxel index 8. So the minimum rises by exactly 8 and the span falls from
	 * 15 to 7. A crop treated as a voxel index, a UVW coordinate or a world
	 * distance gives a different number here, and no crop at all gives 15.
	 */
	{
		const FLinearColor& Uncropped = Results[CfgMin].ValueAt(CentreX, CentreY);
		const FLinearColor& Cropped = Results[CfgCrop].ValueAt(CentreX, CentreY);

		if (TestTrue(TEXT("the cropped ray still marches - a crop that rejected every ray would "
						  "make the comparison below vacuous"),
				StepCount(Cropped) > 0))
		{
			TestEqual(TEXT("cropping to the far half of X raises the minimum by exactly 8 voxels. "
						   "A crop applied in voxel indices, UVW or world units lands elsewhere; "
						   "no crop at all leaves it unchanged"),
				static_cast<double>(ReportedValue(Cropped) - ReportedValue(Uncropped)), 8.0, 1e-4);

			TestEqual(TEXT("...and the maximum is untouched, so the crop removed the near half "
						   "rather than shrinking the ray from both ends"),
				static_cast<double>(ReportedValue(Results[CfgCrop].ValueAt(CentreX, CentreY))
					- ReportedValue(Results[CfgMax].ValueAt(CentreX, CentreY))),
				-7.0, 1e-4);
		}
	}

	/* == The status volume gates sampling ==================================== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: masked voxels contributing
	 * to the composite.
	 *
	 * The first four X voxels are Masked. A masked voxel is not absent: it must
	 * be excluded from the value while still being reported in the reason bits,
	 * so the picture can say WHY there is a hole. Excluding it from both, or
	 * from neither, are different bugs and this distinguishes them.
	 */
	{
		const FLinearColor& Unmasked = Results[CfgMin].ValueAt(CentreX, CentreY);
		const FLinearColor& Masked = Results[CfgMask].ValueAt(CentreX, CentreY);

		if (TestTrue(TEXT("the masked ray still marches and still finds valid data beyond the "
						  "masked slab"),
				StepCount(Masked) > 0 && HasReason(Masked, EFlowVizInvalidReason::Valid)))
		{
			TestEqual(*FString::Printf(
					TEXT("masking voxels X<%d raises the minimum by exactly %d - the masked "
						 "values are excluded from the composite rather than averaged in"),
					MaskedBelowI, MaskedBelowI),
				static_cast<double>(ReportedValue(Masked) - ReportedValue(Unmasked)),
				static_cast<double>(MaskedBelowI), 1e-4);

			TestTrue(TEXT("...and the ray REPORTS having crossed masked voxels (REASON_MASKED), "
						  "so the renderer can colour the cause rather than silently dropping it"),
				HasReason(Masked, EFlowVizInvalidReason::Masked));

			TestFalse(TEXT("...while the unmasked render of the same ray does NOT report masking, "
						   "which is what proves the bit above came from the status volume and "
						   "not from something set on every ray"),
				HasReason(Unmasked, EFlowVizInvalidReason::Masked));
		}
	}

	/* == Lighting does not modulate the reported value ======================= */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a shading term leaking
	 * into the scalar a scientific reading is taken from.
	 *
	 * VISUAL_QA rule 1. OutValue.x is written from the unlit sample, so a lit
	 * and an unlit render must agree on it BIT FOR BIT - not nearly, exactly.
	 * BOTH halves are asserted: if the colour did not change, lighting never ran
	 * and the invariance holds for the wrong reason.
	 */
	{
		const FLinearColor& UnlitValue = Results[CfgUnlit].ValueAt(CentreX, CentreY);
		const FLinearColor& LitValue = Results[CfgLit].ValueAt(CentreX, CentreY);
		const FLinearColor& UnlitColor = Results[CfgUnlit].ColorAt(CentreX, CentreY);
		const FLinearColor& LitColor = Results[CfgLit].ColorAt(CentreX, CentreY);

		if (TestTrue(TEXT("the alpha-composited ray accumulated opacity, so there is a shaded "
						  "colour to compare at all"),
				ReportedAlpha(UnlitValue) > 0.0f && StepCount(UnlitValue) > 0))
		{
			// --- THE CONTROL, FIRST ---
			TestTrue(*FString::Printf(
					TEXT("CONTROL: lighting CHANGED the colour (unlit rgb %.4f,%.4f,%.4f vs lit "
						 "%.4f,%.4f,%.4f). If these matched, lighting never ran and the "
						 "bit-identity assertion below would pass for the wrong reason"),
					UnlitColor.R, UnlitColor.G, UnlitColor.B,
					LitColor.R, LitColor.G, LitColor.B),
				UnlitColor.R != LitColor.R || UnlitColor.G != LitColor.G
					|| UnlitColor.B != LitColor.B);

			TestTrue(*FString::Printf(
					TEXT("the reported scalar is BIT-IDENTICAL with lighting on and off "
						 "(%.9g vs %.9g). A tolerance would not do: any difference at all means "
						 "a shading term reached the value a reading is taken from"),
					ReportedValue(UnlitValue), ReportedValue(LitValue)),
				ReportedValue(UnlitValue) == ReportedValue(LitValue));

			TestTrue(TEXT("...and so is the accumulated alpha, which lighting must not touch "
						  "either"),
				ReportedAlpha(UnlitValue) == ReportedAlpha(LitValue));
		}

		// Alpha compositing reports the FIRST valid sample, and along +X the
		// field increases, so that is also the minimum. A mode-dispatch bug that
		// ran the wrong branch shows up here as a mismatch between two
		// independently computed answers.
		TestEqual(TEXT("alpha compositing reports the first sample along the ray, which for a "
					   "field increasing along the view direction is the same value Minimum "
					   "reports"),
			static_cast<double>(ReportedValue(UnlitValue)),
			static_cast<double>(ReportedValue(Results[CfgMin].ValueAt(CentreX, CentreY))), 1e-4);
	}

	/* == Pass two: iso-surface, lighting direction, clip planes ============== */
	/*
	 * These need an iso value that lands INSIDE the centre ray's row, and the row
	 * constant is a property of the framing. Reading it back from pass one rather
	 * than recomputing it here is deliberate: a test that derived the row from the
	 * camera parameters would restate the projection code it is checking, and
	 * would agree with it even when both were wrong.
	 */

	const float CentreRowMin = ReportedValue(Results[CfgMin].ValueAt(CentreX, CentreY));

	// Halfway along the crossing, and deliberately NOT on a voxel value: x.5
	// cannot be hit by any sample, so a reported IsoValue can only have come from
	// the linear-crossing branch. Landing it on an integer would let a renderer
	// that snapped to the nearest sample produce the same answer.
	const float IsoTarget = CentreRowMin + 7.5f;

	{
		const FMarchConfig IsoConfigs[] = {
			// The surface exists on the centre ray, between voxels 7 and 8.
			{ TEXT("IsoHit"),   EFlowVizCompositeMode::IsoSurface, false, false, 0.0f,
			  true, IsoTarget },
			// Above every value in the domain, so no crossing exists anywhere.
			{ TEXT("IsoMiss"),  EFlowVizCompositeMode::IsoSurface, false, false, 0.0f,
			  true, FieldMax + 100.0f },
			// The same surface, lit from each axis in turn. Three renders that
			// differ ONLY in LightDirection, so a difference between them is a
			// property of the normal and of nothing else.
			{ TEXT("IsoLitX"),  EFlowVizCompositeMode::IsoSurface, false, true, 0.0f,
			  true, IsoTarget, FVector3f(1.0f, 0.0f, 0.0f) },
			{ TEXT("IsoLitY"),  EFlowVizCompositeMode::IsoSurface, false, true, 0.0f,
			  true, IsoTarget, FVector3f(0.0f, 1.0f, 0.0f) },
			{ TEXT("IsoLitZ"),  EFlowVizCompositeMode::IsoSurface, false, true, 0.0f,
			  true, IsoTarget, FVector3f(0.0f, 0.0f, 1.0f) },
			// Clip planes, in LOCAL units. dot(N, LocalPos) + D >= 0 is kept, so
			// (1,0,0,-1) keeps local x >= 1.0, which is voxel index 8 - the SAME
			// cut the crop box makes at 0.5, expressed in a different space. That
			// equivalence is the assertion.
			{ TEXT("ClipMin"),  EFlowVizCompositeMode::Minimum, false, false, 0.0f,
			  false, 0.0f, FVector3f(0.0f, 0.0f, 1.0f), 1,
			  FVector4f(1.0f, 0.0f, 0.0f, -1.0f) },
			// A second plane facing the other way: keep local x <= 1.5, i.e.
			// voxel 12 and below. Two planes together must intersect, not replace.
			{ TEXT("ClipBoth"), EFlowVizCompositeMode::Maximum, false, false, 0.0f,
			  false, 0.0f, FVector3f(0.0f, 0.0f, 1.0f), 2,
			  FVector4f(1.0f, 0.0f, 0.0f, -1.0f), FVector4f(-1.0f, 0.0f, 0.0f, 1.5f) },
			// TWO planes on the SAME side, tight first and LOOSE SECOND. This
			// ordering is the whole point: it is what makes the accumulating
			// max() load-bearing rather than decorative. See the assertion.
			{ TEXT("ClipMinPair"), EFlowVizCompositeMode::Minimum, false, false, 0.0f,
			  false, 0.0f, FVector3f(0.0f, 0.0f, 1.0f), 2,
			  FVector4f(1.0f, 0.0f, 0.0f, -1.0f), FVector4f(1.0f, 0.0f, 0.0f, -0.5f) },
			// The same shape on the far side, exercising the min() accumulator:
			// keep x <= 1.0, then the LOOSER x <= 1.5.
			{ TEXT("ClipMaxPair"), EFlowVizCompositeMode::Maximum, false, false, 0.0f,
			  false, 0.0f, FVector3f(0.0f, 0.0f, 1.0f), 2,
			  FVector4f(-1.0f, 0.0f, 0.0f, 1.0f), FVector4f(-1.0f, 0.0f, 0.0f, 1.5f) },
			// A plane whose kept half-space excludes the whole domain. The ray
			// must be rejected outright rather than clamped to an empty interval.
			{ TEXT("ClipAll"),  EFlowVizCompositeMode::Minimum, false, false, 0.0f,
			  false, 0.0f, FVector3f(0.0f, 0.0f, 1.0f), 1,
			  FVector4f(1.0f, 0.0f, 0.0f, -100.0f) },
		};
		SecondPassConfigs.Append(IsoConfigs, UE_ARRAY_COUNT(IsoConfigs));

		// The three axis-lit gradient renders hold P7's self-shadow term at
		// exactly 1 by zeroing the opacity the shadow march multiplies by --
		// nothing else in iso mode reads OpacityMultiplier, so this is a hold on
		// the confound, not a change to the property under test. See
		// FMarchConfig::OpacityMul for the measured misattribution it prevents.
		SecondPassConfigs[2].OpacityMul = 0.0f; // IsoLitX
		SecondPassConfigs[3].OpacityMul = 0.0f; // IsoLitY
		SecondPassConfigs[4].OpacityMul = 0.0f; // IsoLitZ
	}

	/*
	 * FOUR MORE CONFIGS FOR THE CLAMP FLAG: two values, each rendered with the
	 * flag off and on.
	 *
	 * The domain is narrowed to [RowMin + 4, RowMin + 11] - stated relative to the
	 * row constant read back from pass one, for the same reason IsoTarget is. The
	 * ray still spans RowMin .. RowMin + 15, so Minimum resolves BELOW the domain
	 * and Maximum ABOVE it, and one narrowing produces both an under-range and an
	 * over-range pixel. Two directions matter: a shader that honoured the flag on
	 * only one branch would be half fixed and look entirely correct on whichever
	 * half was tested.
	 *
	 * WHY THE PAIRS DIFFER ONLY IN THE FLAG. Same mode, same range, same colours,
	 * same camera, same textures. Anything that differs between a pair is
	 * therefore attributable to bClampToRange and to nothing else - which is what
	 * makes this a test of the RENDER rather than of the parameter block. The
	 * existing assertion in FlowVizTransferFunctionTest checks that the flag is
	 * SET; that passes identically whether or not any shader ever reads it.
	 */
	const float ClampRangeMin = CentreRowMin + 4.0f;
	const float ClampRangeMax = CentreRowMin + 11.0f;
	{
		FMarchConfig ClampConfigs[4];

		// Minimum resolves to the row's first voxel, which is 4 below the domain.
		ClampConfigs[0].Name = TEXT("ClampUnderOff");
		ClampConfigs[0].Mode = EFlowVizCompositeMode::Minimum;
		ClampConfigs[1].Name = TEXT("ClampUnderOn");
		ClampConfigs[1].Mode = EFlowVizCompositeMode::Minimum;

		// Maximum resolves to the row's last voxel, 4 above the domain.
		ClampConfigs[2].Name = TEXT("ClampOverOff");
		ClampConfigs[2].Mode = EFlowVizCompositeMode::Maximum;
		ClampConfigs[3].Name = TEXT("ClampOverOn");
		ClampConfigs[3].Mode = EFlowVizCompositeMode::Maximum;

		for (int32 Index = 0; Index < 4; ++Index)
		{
			ClampConfigs[Index].bNarrowRange = true;
			ClampConfigs[Index].RangeMin = ClampRangeMin;
			ClampConfigs[Index].RangeMax = ClampRangeMax;
			// Odd entries are the clamped arm of their pair.
			ClampConfigs[Index].bClampToRange = (Index % 2) == 1;
		}

		SecondPassConfigs.Append(ClampConfigs, UE_ARRAY_COUNT(ClampConfigs));
	}

	/*
	 * Two depth-clamped renders differ only in the length of RayCameraForward.
	 * Both address the 32,32 quadrant of the 64x64 depth texture. The first proves
	 * the scene-depth branch actually clamps a still-visible ray; the second proves
	 * the raw basis scale cannot change that clamp.
	 */
	{
		FMarchConfig DepthConfigs[2];
		DepthConfigs[0].Name = TEXT("DepthOffsetUnitForward");
		DepthConfigs[1].Name = TEXT("DepthOffsetScaledForward");
		for (FMarchConfig& DepthConfig : DepthConfigs)
		{
			DepthConfig.Mode = EFlowVizCompositeMode::Maximum;
			DepthConfig.bBindDepth = true;
			DepthConfig.DepthRectMin = FVector2f(32.0f, 32.0f);
		}
		DepthConfigs[1].ForwardScale = 100.0f;
		SecondPassConfigs.Append(DepthConfigs, UE_ARRAY_COUNT(DepthConfigs));
	}

	enum { IsoHit = 0, IsoMiss = 1, IsoLitX = 2, IsoLitY = 3, IsoLitZ = 4,
		   ClipMin = 5, ClipBoth = 6, ClipMinPair = 7, ClipMaxPair = 8, ClipAll = 9,
		   ClampUnderOff = 10, ClampUnderOn = 11, ClampOverOff = 12, ClampOverOn = 13,
		   DepthUnitForward = 14, DepthScaledForward = 15 };

	SecondPassResults.SetNum(SecondPassConfigs.Num());
	RunPass(SecondPassConfigs, SecondPassResults);

	if (!SetupError.IsEmpty())
	{
		AddError(FString::Printf(TEXT("The second pass could not be set up, so iso-surface, "
									  "lighting direction and clipping were NOT verified: %s"),
			*SetupError));
		return false;
	}

	for (int32 Index = 0; Index < SecondPassConfigs.Num(); ++Index)
	{
		if (!TestTrue(*FString::Printf(TEXT("second-pass config '%s' dispatched and read back"),
				SecondPassConfigs[Index].Name),
				SecondPassResults[Index].bDispatched
					&& SecondPassResults[Index].Value.Num() == OutputW * OutputH))
		{
			AddError(TEXT("Nothing in the iso-surface, gradient or clipping blocks was verified."));
			return false;
		}
	}

	/* == The iso-surface is found between samples ============================ */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: an iso-surface snapped to
	 * the sample lattice, and a miss reported as a hit.
	 *
	 * The linear-crossing branch is what keeps an iso-surface off the lattice and
	 * free of stair-stepping - the single most visible artefact this renderer can
	 * produce. It compiled and dispatched but had never run. IsoTarget is x.5, a
	 * value no sample takes, so a renderer that reported the nearest sample would
	 * report an integer here and this block would catch it.
	 */
	{
		const FLinearColor& Hit = SecondPassResults[IsoHit].ValueAt(CentreX, CentreY);
		const FLinearColor& Miss = SecondPassResults[IsoMiss].ValueAt(CentreX, CentreY);

		// --- THE CONTROL, FIRST ---
		// The whole block is vacuous if the surface was never crossed.
		if (TestTrue(*FString::Printf(
					TEXT("CONTROL: the iso ray marched and produced an opaque hit (alpha %.3f, "
						 "%d steps). Without a crossing, every assertion below would be comparing "
						 "two empty pixels"),
					ReportedAlpha(Hit), StepCount(Hit)),
				ReportedAlpha(Hit) > 0.0f && StepCount(Hit) > 0))
		{
			TestEqual(*FString::Printf(
					TEXT("the iso-surface reports the REQUESTED value %.4f, not the nearest "
						 "sample. The target is deliberately x.5, which no voxel takes, so an "
						 "integer here means the crossing was snapped to the sample lattice and "
						 "the surface would stair-step"),
					IsoTarget),
				static_cast<double>(ReportedValue(Hit)), static_cast<double>(IsoTarget), 1e-3);

			TestTrue(*FString::Printf(
					TEXT("...and the reported value is NOT an integer (%.4f), stated separately so "
						 "the failure names lattice snapping even if the target moves"),
					ReportedValue(Hit)),
				FMath::Abs(ReportedValue(Hit) - FMath::RoundToFloat(ReportedValue(Hit))) > 0.1f);

			// A crossing at 7.5 of 15 is halfway, so the ray must stop around
			// half its steps in. Running to the far wall means the break never
			// fired and the surface is the LAST crossing rather than the first -
			// which for a closed surface renders the back face.
			const int32 FullSteps = StepCount(Results[CfgMax].ValueAt(CentreX, CentreY));
			TestTrue(*FString::Printf(
					TEXT("the iso ray STOPPED at the surface (%d steps vs %d for a full crossing). "
						 "Marching to the far wall means the first crossing did not terminate the "
						 "loop, so a closed surface would show its back face"),
					StepCount(Hit), FullSteps),
				StepCount(Hit) < FullSteps);
		}

		// A miss is not a hit with alpha zero: it must report nothing found.
		TestTrue(*FString::Printf(
				TEXT("an iso value above the whole field finds NO surface (alpha %.3f, value "
					 "%.3f). A hit here would mean the crossing test fires on samples that do not "
					 "bracket the surface"),
				ReportedAlpha(Miss), ReportedValue(Miss)),
			ReportedAlpha(Miss) == 0.0f);

		TestTrue(*FString::Printf(
				TEXT("...and the missing ray still MARCHED (%d steps), which is what proves the "
					 "miss came from finding no crossing rather than from never entering the "
					 "volume"), StepCount(Miss)),
			StepCount(Miss) > 0);
	}

	/* == The gradient points where the field actually rises =================== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a normal that points the
	 * wrong way.
	 *
	 * The existing lighting test proves lighting RAN and did not corrupt the
	 * reported scalar. It says nothing about direction, and a volume lit from the
	 * wrong side looks entirely reasonable - it is only wrong next to the data.
	 * An axis swapped in FlowVizGradient, or one spacing used for all three axes,
	 * survives every other assertion in this file.
	 *
	 * HOW A DIRECTION BECOMES A NUMBER. The field is linear, so the gradient is
	 * the constant (8, 32, 512) everywhere - no dependence on the hit position,
	 * which is what makes this checkable without first proving where the ray hit.
	 * Three renders differ ONLY in LightDirection, along +X, +Y and +Z. For a unit
	 * light along an axis, NdotL is that component of the unit normal, so the
	 * three renders recover the normal's components directly and their RATIOS are
	 * the gradient's ratios: 8 : 32 : 512 = 1 : 4 : 64. Normalisation cancels, so
	 * this tests DIRECTION and is indifferent to the gradient's magnitude.
	 *
	 * Measured separations for the bugs this targets: X<->Z swap moves the X:Y
	 * ratio from 0.25 to 16, X<->Y to 4, and dividing all three axes by one
	 * spacing to 0.0625. The tightest is 4x. The tolerance below is 5%.
	 */
	{
		const FLinearColor& LitX = SecondPassResults[IsoLitX].ColorAt(CentreX, CentreY);
		const FLinearColor& LitY = SecondPassResults[IsoLitY].ColorAt(CentreX, CentreY);
		const FLinearColor& LitZ = SecondPassResults[IsoLitZ].ColorAt(CentreX, CentreY);

		const float NdotLX = RecoveredNdotL(LitX);
		const float NdotLY = RecoveredNdotL(LitY);
		const float NdotLZ = RecoveredNdotL(LitZ);

		// --- THE CONTROLS, FIRST, AND THERE ARE TWO ---
		//
		// A control that cannot fail is the same bug one level up. Both of these
		// are written to be capable of failing on this fixture: the first fails
		// if lighting is off or the surface was missed, the second fails if the
		// normal happens to be axis-aligned, which would make two of the three
		// renders identical and the ratios undefined.
		const bool bLitHit = ReportedAlpha(SecondPassResults[IsoLitX].ValueAt(CentreX, CentreY)) > 0.0f;

		if (TestTrue(TEXT("CONTROL: the lit iso renders produced an opaque surface, so there is a "
						  "shaded pixel to read a normal from"),
				bLitHit)
			&& TestTrue(*FString::Printf(
					TEXT("CONTROL: the three axis lights give DISTINCT shading (NdotL = %.6f, "
						 "%.6f, %.6f). If any two matched, the normal would be degenerate on this "
						 "fixture and the ratios below could not distinguish an axis swap"),
					NdotLX, NdotLY, NdotLZ),
				NdotLX != NdotLY && NdotLY != NdotLZ && NdotLX != NdotLZ))
		{
			// saturate() clamps NdotL to [0,1]. A clamped component carries no
			// direction information, so a ratio taken across one would be
			// comparing two constants and could not fail.
			//
			// THE BOUND IS 1.0 EXACTLY, NOT A BAND BELOW IT. The correct normal
			// here is (0.0155927, 0.0623707, 0.9979312): Z really is within
			// 0.00207 of unity, because the gradient is 64x steeper in Z than in
			// X and the surface is very nearly Z-facing. An earlier version of
			// this control demanded < 0.99 and rejected that correct value - it
			// was testing an arbitrary threshold rather than the property it
			// names. A saturate() clamp produces EXACTLY 1.0, so exactly 1.0 is
			// what distinguishes clamped from merely steep. The margin survives
			// the round trip: 0.00207 of NdotL is 6.7e-4 in the green channel,
			// four orders above float32 resolution at that magnitude.
			const bool bUnclamped =
				NdotLX > 0.0f && NdotLX < 1.0f && NdotLY > 0.0f && NdotLY < 1.0f
				&& NdotLZ > 0.0f && NdotLZ < 1.0f;

			if (TestTrue(*FString::Printf(
						TEXT("CONTROL: no recovered component is clamped by saturate() (%.6f, "
							 "%.6f, %.6f all strictly inside (0,1); a clamp reads exactly 1.0). A "
							 "clamped component is a constant, and a ratio of constants cannot "
							 "detect a swapped axis"),
						NdotLX, NdotLY, NdotLZ),
					bUnclamped))
			{
				// grad = (8, 32, 512): X:Y is 0.25, Y:Z is 0.0625.
				const float RatioXY = NdotLX / NdotLY;
				const float RatioYZ = NdotLY / NdotLZ;

				TestTrue(*FString::Printf(
						TEXT("the gradient's X:Y ratio is %.5f, matching the analytic 8:32 = 0.25. "
							 "An X<->Y swap gives 4.0 and an X<->Z swap 16.0; dividing every axis "
							 "by a single spacing gives 0.0625. Each is more than 4x from the "
							 "correct answer, so this is a direction check, not a tolerance"),
						RatioXY),
					FMath::Abs(RatioXY - 0.25f) < 0.0125f);

				TestTrue(*FString::Printf(
						TEXT("the gradient's Y:Z ratio is %.5f, matching the analytic 32:512 = "
							 "0.0625. A Y<->Z swap gives 16.0"),
						RatioYZ),
					FMath::Abs(RatioYZ - 0.0625f) < 0.00313f);

				// Direction, not just proportion: the field INCREASES along every
				// axis, so the surface normal (which points down-gradient) must
				// face the negative octant. All three NdotL positive under
				// positive-axis lights is what says so. A normal that was not
				// negated points the other way and lights the surface from
				// behind - a sign error that leaves the ratios above intact.
				TestTrue(*FString::Printf(
						TEXT("all three components of the normal have the same sign (NdotL %.4f, "
							 "%.4f, %.4f all positive). The field rises along every axis, so a "
							 "normal pointing down-gradient must face one octant; a dropped "
							 "negation reverses all three and lights the surface from behind "
							 "while leaving the ratios above unchanged"),
						NdotLX, NdotLY, NdotLZ),
					NdotLX > 0.0f && NdotLY > 0.0f && NdotLZ > 0.0f);

				// The Z component dominates by 64:1, so the shading is nearly
				// fully lit from +Z and nearly dark from +X. Stated as an
				// absolute so the failure names the anisotropy rather than a ratio.
				TestTrue(*FString::Printf(
						TEXT("lighting along the steepest axis (+Z, gradient %.0f) shades far "
							 "brighter than along the shallowest (+X, gradient %.0f): NdotL %.4f "
							 "vs %.4f. Equal shading would mean the gradient ignored per-axis "
							 "spacing"),
						GradZ, GradX, NdotLZ, NdotLX),
					NdotLZ > 4.0f * NdotLX);
			}
		}
	}

	/* == Clip planes cut in local units, and compose ========================== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a clip plane in the wrong
	 * space, a second plane that replaces the first, and an empty clip that
	 * renders anyway.
	 *
	 * The plane (1,0,0,-1) keeps local x >= 1.0. The crop box at 0.5 keeps the
	 * far half of a domain 2.0 across, which is ALSO local x >= 1.0. Two
	 * different parameters, two different spaces, one geometric cut - so the two
	 * renders must agree exactly. That equivalence is a stronger statement than
	 * either alone: a plane evaluated in UVW or world units still cuts something,
	 * and still looks like a clipped volume, but stops matching the crop box.
	 */
	{
		const FLinearColor& Clipped = SecondPassResults[ClipMin].ValueAt(CentreX, CentreY);
		const FLinearColor& Cropped = Results[CfgCrop].ValueAt(CentreX, CentreY);
		const FLinearColor& Uncropped = Results[CfgMin].ValueAt(CentreX, CentreY);

		if (TestTrue(*FString::Printf(
					TEXT("CONTROL: the clipped ray still marches (%d steps) and found valid data. "
						 "A clip that rejected everything would make the comparison below vacuous"),
					StepCount(Clipped)),
				StepCount(Clipped) > 0 && (ReasonBits(Clipped) & 1u) != 0u))
		{
			TestEqual(*FString::Printf(
					TEXT("a clip plane at local x >= 1.0 raises the minimum by exactly 8 voxels "
						 "(%.3f -> %.3f). A plane evaluated in UVW, voxel indices or world units "
						 "cuts somewhere else and still produces a plausible clipped image"),
					ReportedValue(Uncropped), ReportedValue(Clipped)),
				static_cast<double>(ReportedValue(Clipped) - ReportedValue(Uncropped)), 8.0, 1e-4);

			TestEqual(TEXT("...and lands on exactly the same voxel as the CROP BOX at 0.5, which "
						   "is the same cut expressed in domain fractions. Two parameters in two "
						   "spaces must agree on one piece of geometry"),
				static_cast<double>(ReportedValue(Clipped)),
				static_cast<double>(ReportedValue(Cropped)), 1e-4);
		}

		// Two planes must INTERSECT their half-spaces. A second plane that
		// overwrote TMin/TMax instead of narrowing them would leave the maximum
		// at the far wall, which is the uncropped answer.
		const FLinearColor& Both = SecondPassResults[ClipBoth].ValueAt(CentreX, CentreY);
		const float UnclippedMax = ReportedValue(Results[CfgMax].ValueAt(CentreX, CentreY));

		if (TestTrue(*FString::Printf(TEXT("CONTROL: the doubly-clipped ray still marches (%d "
										   "steps)"), StepCount(Both)),
				StepCount(Both) > 0 && HasReason(Both, EFlowVizInvalidReason::Valid)))
		{
			TestTrue(*FString::Printf(
					TEXT("a second plane keeping local x <= 1.5 lowers the maximum below the "
						 "unclipped %.1f (got %.1f). An unchanged maximum means the second plane "
						 "REPLACED the first rather than intersecting with it, so only one plane "
						 "would ever take effect"),
					UnclippedMax, ReportedValue(Both)),
				ReportedValue(Both) < UnclippedMax);

			TestTrue(*FString::Printf(
					TEXT("...and the first plane is STILL in effect under the second (minimum %.1f "
						 "is at or above the +8 voxel cut). This is the other order of the same "
						 "replacement bug"),
					ReportedValue(Both)),
				ReportedValue(Both) >= ReportedValue(Uncropped) + 8.0f - 1e-4f);
		}

		/*
		 * THE TIGHTER OF TWO PLANES ON ONE SIDE MUST WIN, WHICHEVER ORDER THEY ARRIVE
		 * IN. The two assertions above cannot establish this, and a differential run
		 * proved it: replacing `TMin = max(TMin, THit)` with `TMin = THit` SURVIVED
		 * them both. On the configs above each accumulator is written exactly once,
		 * and the value it writes already exceeds the box-entry TMin - so max() and
		 * plain assignment agree, and the mutant is an identity map. The assertion
		 * named the bug and could not see it.
		 *
		 * What is needed is a SECOND write to the SAME accumulator that would move
		 * it the WRONG WAY. ClipMinPair keeps x >= 1.0 and then x >= 0.5: both
		 * narrow TMin, and the looser one comes second. Intersecting keeps the
		 * tighter 1.0 (minimum +8); overwriting takes the later 0.5 (minimum +4).
		 * ClipMaxPair is the mirror image on the min() accumulator. Neither can be
		 * satisfied by an identity map, because the two orders give different
		 * numbers by construction.
		 */
		{
			const FLinearColor& MinPair = SecondPassResults[ClipMinPair].ValueAt(CentreX, CentreY);
			const FLinearColor& MaxPair = SecondPassResults[ClipMaxPair].ValueAt(CentreX, CentreY);

			if (TestTrue(*FString::Printf(
						TEXT("CONTROL: the tight-then-loose ray still marches (%d steps) and found "
							 "valid data"), StepCount(MinPair)),
					StepCount(MinPair) > 0 && HasReason(MinPair, EFlowVizInvalidReason::Valid)))
			{
				TestEqual(*FString::Printf(
						TEXT("two planes both keeping a MINIMUM x, tight (>=1.0) then LOOSE "
							 "(>=0.5), intersect to the tight one: the minimum rises by 8 voxels, "
							 "not 4. A second plane that OVERWROTE TMin instead of accumulating "
							 "max() would take the looser plane simply because it came last, and "
							 "every assertion above still passes - only the order distinguishes "
							 "them (%.3f -> %.3f)"),
						ReportedValue(Uncropped), ReportedValue(MinPair)),
					static_cast<double>(ReportedValue(MinPair) - ReportedValue(Uncropped)),
					8.0, 1e-4);

				TestEqual(TEXT("...and it agrees exactly with the SINGLE tight plane, which is the "
							   "same half-space. Adding a redundant looser plane must change "
							   "nothing at all"),
					static_cast<double>(ReportedValue(MinPair)),
					static_cast<double>(ReportedValue(Clipped)), 1e-4);
			}

			if (TestTrue(*FString::Printf(
						TEXT("CONTROL: the max-side pair still marches (%d steps) and found valid "
							 "data"), StepCount(MaxPair)),
					StepCount(MaxPair) > 0 && HasReason(MaxPair, EFlowVizInvalidReason::Valid)))
			{
				// The mirror image, on the other accumulator. Keeping x <= 1.0 caps
				// the maximum at voxel 8; the looser x <= 1.5 would cap it at 12.
				//
				// The boundary voxel is INCLUDED, symmetrically with the minimum
				// side: the crop box at 0.5 is the cut at local x = 1.0 and raises
				// the minimum TO voxel 8, so the opposing cut at the same plane must
				// leave the maximum AT voxel 8. An earlier version of this line
				// predicted 7, treating one boundary as exclusive and the other as
				// inclusive; the measured 8 is what the two sides agreeing requires.
				TestEqual(*FString::Printf(
						TEXT("two planes both keeping a MAXIMUM x, tight (<=1.0) then LOOSE "
							 "(<=1.5), intersect to the tight one: the maximum is 8 above the row "
							 "constant, not 12. This is the same replacement bug on the min() "
							 "accumulator, which the max() assertion above cannot reach (%.3f)"),
						ReportedValue(MaxPair)),
					static_cast<double>(ReportedValue(MaxPair) - ReportedValue(Uncropped)),
					8.0, 1e-4);
			}
		}

		// A plane that excludes the domain must reject the ray, not march an
		// empty interval. Zero steps AND no valid reason bit: "never entered" is
		// a different answer from "entered and found nothing", and the renderer
		// distinguishes them elsewhere.
		const FLinearColor& All = SecondPassResults[ClipAll].ValueAt(CentreX, CentreY);
		TestEqual(TEXT("a clip plane whose kept half-space excludes the whole domain rejects the "
					   "ray outright and takes zero steps"),
			StepCount(All), 0);
		TestEqual(TEXT("...and reports REASON_NONE, i.e. 'the ray never entered' rather than "
					   "'entered and found nothing usable'"),
			static_cast<int32>(ReasonBits(All)), 0);
	}

	/* == bClampToRange changes the PIXEL, and changes nothing else ============ */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a user-facing control that
	 * silently does nothing.
	 *
	 * bClampToRange was produced by MakeShaderParameters, pinned by a
	 * static_assert on its cbuffer offset, and asserted by
	 * FlowVizTransferFunctionTest - and no shader could read it. There was no
	 * SHADER_PARAMETER to carry it and `grep -i ClampToRange Shaders/` returned
	 * nothing, so the CPU preview honoured the flag and the GPU render did not.
	 * Turning clamping ON changed the preview and not the picture.
	 *
	 * WHY THE EXISTING ASSERTION COULD NOT SEE IT, which is the reusable part:
	 * `TestEqual(Params.bClampToRange, 1u)` tests the PRODUCER. It passes byte for
	 * byte on a build where the flag reaches the shader and on one where it is
	 * dropped on the floor, because in both the struct field is 1. A flag's
	 * coverage has to be a difference in what was DRAWN; anything upstream of the
	 * dispatch is satisfied by the defect.
	 *
	 * So this is differential and it is on pixels: the same out-of-range voxel,
	 * rendered twice, differing only in the flag.
	 *
	 *   flag off -> the flag colour       (VISUAL_QA rule 4's protection)
	 *   flag on  -> the colormap's end    (the user's explicit opt-out)
	 *
	 * AND THE INVARIANT THAT KEEPS THE OPT-OUT HONEST. Clamping is a DISPLAY
	 * choice applied AFTER classification - FlowVizTransferFunction.cpp:333 says
	 * so for the CPU path and the same must hold here. The reported scalar and the
	 * reason bits are asserted IDENTICAL across each pair. A flag that also moved
	 * the number, or that cleared the UNDER/OVER_RANGE bit, would hide the
	 * out-of-range condition instead of recolouring it - which is the quantitative
	 * lie the flag colours exist to prevent, arriving through the control meant to
	 * be an informed choice about them.
	 */
	{
		const FLinearColor& UnderOffValue = SecondPassResults[ClampUnderOff].ValueAt(CentreX, CentreY);
		const FLinearColor& UnderOnValue = SecondPassResults[ClampUnderOn].ValueAt(CentreX, CentreY);
		const FLinearColor& OverOffValue = SecondPassResults[ClampOverOff].ValueAt(CentreX, CentreY);
		const FLinearColor& OverOnValue = SecondPassResults[ClampOverOn].ValueAt(CentreX, CentreY);

		const FLinearColor& UnderOffColor = SecondPassResults[ClampUnderOff].ColorAt(CentreX, CentreY);
		const FLinearColor& UnderOnColor = SecondPassResults[ClampUnderOn].ColorAt(CentreX, CentreY);
		const FLinearColor& OverOffColor = SecondPassResults[ClampOverOff].ColorAt(CentreX, CentreY);
		const FLinearColor& OverOnColor = SecondPassResults[ClampOverOn].ColorAt(CentreX, CentreY);

		const auto RgbDistance = [](const FLinearColor& A, const FLinearColor& B)
		{
			return FMath::Sqrt(FMath::Square(A.R - B.R) + FMath::Square(A.G - B.G)
				+ FMath::Square(A.B - B.B));
		};

		/* -- THE CONTROLS, FIRST ------------------------------------------- */
		/*
		 * A comparison that finds nothing because it is BLIND looks exactly like a
		 * proof of correctness. Three things must be true before any difference
		 * below means anything, and each is stated so that it can fail:
		 *
		 *   1. The narrowed domain really put these samples out of range. If the
		 *      range did not take effect the samples are in-range, both arms draw
		 *      the same LUT colour, and "no difference" would be reported as a
		 *      pass by an assertion looking for equality - or as a failure with a
		 *      misleading cause by this one.
		 *   2. Both renders are non-black. Two black pixels are equal, and a pair
		 *      of empty images satisfies every equality assertion here.
		 *   3. The flag colour and the colormap end are far apart, so the pair CAN
		 *      differ measurably. This is a property of the fixture and is checked
		 *      on the CPU, where a failure names the fixture rather than the shader.
		 */
		const int32 UnderReason = static_cast<int32>(ReasonBits(UnderOffValue));
		const int32 OverReason = static_cast<int32>(ReasonBits(OverOffValue));

		// Both halves: the RESOLVED value - which is what picks the colour - lies
		// outside the domain, AND the shader agrees by raising the matching reason
		// bit. Checking only the bit would accept a render whose resolved value was
		// in range while some other sample along the ray was not; checking only the
		// value would not prove the shader classified it.
		const bool bRangeTookEffect =
			ReportedValue(UnderOffValue) < ClampRangeMin
			&& ReportedValue(OverOffValue) > ClampRangeMax
			// Named enumerators, not literals. A hand-written 32 agrees with the
			// shader only until someone renumbers one side, and a stale literal
			// here would silently start reading a DIFFERENT cause while still
			// passing. HasReason routes through FlowVizRayMarch::DecodeReason, the
			// decoder the plugin ships to its callers, and FlowViz.Render.ReasonCodes
			// pins those enumerators to the .usf's own #defines.
			&& HasReason(UnderOffValue, EFlowVizInvalidReason::UnderRange)
			&& HasReason(OverOffValue, EFlowVizInvalidReason::OverRange);

		if (!TestTrue(*FString::Printf(
					TEXT("CONTROL: narrowing the domain to [%.1f, %.1f] really put the centre ray's "
						 "Minimum (%.1f) UNDER it and its Maximum (%.1f) OVER it - reason bits %d and "
						 "%d carry UNDER_RANGE (32) and OVER_RANGE (64). Without this the samples are "
						 "in range, both arms draw the same LUT colour, and every comparison below "
						 "would be between two identical correct pixels"),
					ClampRangeMin, ClampRangeMax,
					ReportedValue(UnderOffValue), ReportedValue(OverOffValue),
					UnderReason, OverReason),
				bRangeTookEffect))
		{
			AddError(TEXT("The clamp fixture is degenerate: nothing was out of range, so the "
						  "clamp flag had nothing to change and NOTHING about it was verified."));
			return false;
		}

		const auto IsNonBlack = [](const FLinearColor& C)
		{
			return (C.R + C.G + C.B) > 0.01f;
		};

		if (!TestTrue(*FString::Printf(
					TEXT("CONTROL: all four clamp renders are NON-BLACK (unclamped under "
						 "%.3f,%.3f,%.3f; clamped under %.3f,%.3f,%.3f; unclamped over "
						 "%.3f,%.3f,%.3f; clamped over %.3f,%.3f,%.3f). Two black pixels are equal, "
						 "so an empty image would satisfy the invariance assertions below while "
						 "proving nothing"),
					UnderOffColor.R, UnderOffColor.G, UnderOffColor.B,
					UnderOnColor.R, UnderOnColor.G, UnderOnColor.B,
					OverOffColor.R, OverOffColor.G, OverOffColor.B,
					OverOnColor.R, OverOnColor.G, OverOnColor.B),
				IsNonBlack(UnderOffColor) && IsNonBlack(UnderOnColor)
					&& IsNonBlack(OverOffColor) && IsNonBlack(OverOnColor)))
		{
			return false;
		}

		const FLinearColor LutFirst = LutEntry(0);
		const FLinearColor LutLast = LutEntry(LutWidth - 1);

		// The fixture's own separation, on the CPU. The assertions below demand a
		// difference of 0.5; this says the fixture can actually produce one.
		TestTrue(*FString::Printf(
				TEXT("CONTROL: the fixture's flag colours are far from the colormap ends they "
					 "replace (under %.3f, over %.3f in RGB distance). A flag colour that sat near "
					 "the LUT end would make 'clamped' and 'unclamped' nearly the same pixel and "
					 "the difference assertions would be measuring the tolerance"),
				RgbDistance(UnderFlagColor, LutFirst), RgbDistance(OverFlagColor, LutLast)),
			RgbDistance(UnderFlagColor, LutFirst) > 0.5f
				&& RgbDistance(OverFlagColor, LutLast) > 0.5f);

		/* -- THE DEFECT: the two renders must DIFFER ----------------------- */

		TestTrue(*FString::Printf(
				TEXT("an UNDER-range pixel is coloured DIFFERENTLY with clamping on than off "
					 "(%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f; distance %.4f). Identical colours mean the "
					 "shader never received bClampToRange, so the control changes the CPU preview "
					 "and not the render - which is the whole defect, and is invisible to any "
					 "assertion that only checks the flag was SET"),
				UnderOffColor.R, UnderOffColor.G, UnderOffColor.B,
				UnderOnColor.R, UnderOnColor.G, UnderOnColor.B,
				RgbDistance(UnderOffColor, UnderOnColor)),
			RgbDistance(UnderOffColor, UnderOnColor) > 0.5f);

		TestTrue(*FString::Printf(
				TEXT("an OVER-range pixel likewise (%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f; distance "
					 "%.4f). Stated separately from the under-range case: a shader that guarded "
					 "only one of the two branches is half fixed and looks entirely correct on "
					 "whichever half was tested"),
				OverOffColor.R, OverOffColor.G, OverOffColor.B,
				OverOnColor.R, OverOnColor.G, OverOnColor.B,
				RgbDistance(OverOffColor, OverOnColor)),
			RgbDistance(OverOffColor, OverOnColor) > 0.5f);

		/* -- ...and differ in the RIGHT DIRECTION --------------------------- */
		/*
		 * "The two differ" alone would be satisfied by a flag that drew anything
		 * else at all - the NaN colour, black, the other end of the LUT. These name
		 * the four colours: unclamped is the flag colour, clamped is the colormap
		 * END, on both branches.
		 */
		TestTrue(*FString::Printf(
				TEXT("unclamped, an under-range pixel is the UNDER-RANGE FLAG COLOUR "
					 "(%.4f,%.4f,%.4f vs expected %.3f,%.3f,%.3f). This is VISUAL_QA rule 4's "
					 "protection and it must still be the default"),
				UnderOffColor.R, UnderOffColor.G, UnderOffColor.B,
				UnderFlagColor.R, UnderFlagColor.G, UnderFlagColor.B),
			RgbDistance(UnderOffColor, UnderFlagColor) < 0.01f);

		TestTrue(*FString::Printf(
				TEXT("CLAMPED, the same pixel is the colormap's FIRST entry (%.4f,%.4f,%.4f vs "
					 "expected %.3f,%.3f,%.3f) - the LUT end saturate() produces, not the flag "
					 "colour and not some third thing"),
				UnderOnColor.R, UnderOnColor.G, UnderOnColor.B,
				LutFirst.R, LutFirst.G, LutFirst.B),
			RgbDistance(UnderOnColor, LutFirst) < 0.01f);

		TestTrue(*FString::Printf(
				TEXT("unclamped, an over-range pixel is the OVER-RANGE FLAG COLOUR "
					 "(%.4f,%.4f,%.4f vs expected %.3f,%.3f,%.3f)"),
				OverOffColor.R, OverOffColor.G, OverOffColor.B,
				OverFlagColor.R, OverFlagColor.G, OverFlagColor.B),
			RgbDistance(OverOffColor, OverFlagColor) < 0.01f);

		TestTrue(*FString::Printf(
				TEXT("CLAMPED, the same pixel is the colormap's LAST entry (%.4f,%.4f,%.4f vs "
					 "expected %.3f,%.3f,%.3f). The under and over branches must reach OPPOSITE "
					 "ends; a shader that clamped both to the same end is a different bug that the "
					 "difference assertions above cannot see"),
				OverOnColor.R, OverOnColor.G, OverOnColor.B,
				LutLast.R, LutLast.G, LutLast.B),
			RgbDistance(OverOnColor, LutLast) < 0.01f);

		/* -- THE INVARIANT: clamping is a DISPLAY choice, and only that ----- */
		/*
		 * BIT EQUALITY, not a tolerance, for the same reason the lighting block
		 * uses it: any difference at all means the display flag reached the number
		 * a scientific reading is taken from.
		 */
		TestTrue(*FString::Printf(
				TEXT("the reported scalar is BIT-IDENTICAL with clamping on and off on the "
					 "under-range branch (%.9g vs %.9g). Clamping is a DISPLAY choice applied AFTER "
					 "classification - FlowVizTransferFunction.cpp:333 states the same rule for the "
					 "CPU path. A flag that also clamped the reported VALUE would turn an informed "
					 "opt-out into the quantitative lie the flag colours exist to prevent"),
				ReportedValue(UnderOffValue), ReportedValue(UnderOnValue)),
			ReportedValue(UnderOffValue) == ReportedValue(UnderOnValue));

		TestTrue(*FString::Printf(
				TEXT("...and on the over-range branch (%.9g vs %.9g)"),
				ReportedValue(OverOffValue), ReportedValue(OverOnValue)),
			ReportedValue(OverOffValue) == ReportedValue(OverOnValue));

		TestEqual(*FString::Printf(
				TEXT("the REASON BITS are identical with clamping on and off on the under-range "
					 "branch (%d vs %d). UNDER_RANGE must still be reported when clamping is on: "
					 "clamping recolours the disclosure, it does not withdraw it, and a shader that "
					 "cleared the bit would make the diagnostics panel agree that nothing was out "
					 "of range"),
				static_cast<int32>(ReasonBits(UnderOffValue)),
				static_cast<int32>(ReasonBits(UnderOnValue))),
			static_cast<int32>(ReasonBits(UnderOnValue)),
			static_cast<int32>(ReasonBits(UnderOffValue)));

		TestEqual(*FString::Printf(
				TEXT("...and on the over-range branch (%d vs %d)"),
				static_cast<int32>(ReasonBits(OverOffValue)),
				static_cast<int32>(ReasonBits(OverOnValue))),
			static_cast<int32>(ReasonBits(OverOnValue)),
			static_cast<int32>(ReasonBits(OverOffValue)));

		// The step count too: clamping must not change how far the ray marched.
		// A flag that altered traversal would change the sampled set, and then the
		// bit-equal value above would be a coincidence of this fixture.
		TestEqual(TEXT("...and the ray took the same number of steps either way, so clamping "
					   "changed the shading and not the traversal"),
			StepCount(UnderOnValue), StepCount(UnderOffValue));
	}

	/* == Scene depth addresses the scaled rect and clamps in ray units ========= */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: the P6 depth path
	 * compiling but sampling the wrong scene texel, converting ortho DeviceZ
	 * through the perspective branch, or dividing by a scale-bearing forward
	 * vector.
	 *
	 * The 64x64 depth texture has useful depth only in its far quadrant. The
	 * 32x32 dispatch reaches that quadrant solely through ViewRectMin=(32,32),
	 * mirroring a scaled renderer view inside a larger scene texture. The plane
	 * is halfway through the volume, so the correct result is neither zero steps
	 * nor a full crossing. Finally, scaling only RayCameraForward by 100 cannot
	 * change the ray geometry; equal results prove the depth conversion uses its
	 * direction rather than its inherited local-space length.
	 */
	{
		const FLinearColor& Full = Results[CfgMax].ValueAt(CentreX, CentreY);
		const FLinearColor& Unit =
			SecondPassResults[DepthUnitForward].ValueAt(CentreX, CentreY);
		const FLinearColor& Scaled =
			SecondPassResults[DepthScaledForward].ValueAt(CentreX, CentreY);

		const int32 FullSteps = StepCount(Full);
		const int32 UnitSteps = StepCount(Unit);
		const int32 ScaledSteps = StepCount(Scaled);

		if (TestTrue(*FString::Printf(
					TEXT("CONTROL: offset scene depth leaves a visible PARTIAL ray (%d steps, "
						 "reason bits %u). Zero means ViewRectMin was ignored, ortho DeviceZ used "
						 "the perspective formula, or the plane landed before volume entry"),
					UnitSteps, ReasonBits(Unit)),
			UnitSteps > 0 && HasReason(Unit, EFlowVizInvalidReason::Valid)))
		{
			constexpr int32 ExpectedHalfTraversalSteps = 17;
			TestTrue(*FString::Printf(
					TEXT("the depth plane halfway through X bounds traversal to %d steps "
						 "(expected %d +/-1), rather than the full ray's %d"),
					UnitSteps, ExpectedHalfTraversalSteps, FullSteps),
				FMath::Abs(UnitSteps - ExpectedHalfTraversalSteps) <= 1);

			TestTrue(*FString::Printf(
					TEXT("scene depth SHORTENS the ray (%d steps vs %d without depth). "
						 "Equality means the depth texture was bound but never affected traversal"),
					UnitSteps, FullSteps),
				UnitSteps < FullSteps);

			TestTrue(*FString::Printf(
					TEXT("the halfway plane lowers Maximum from %.3f to %.3f, so the shorter "
						 "step count also changed which data was composited"),
					ReportedValue(Full), ReportedValue(Unit)),
				ReportedValue(Unit) < ReportedValue(Full));
		}

		if (TestTrue(*FString::Printf(
					TEXT("CONTROL: the 100x-forward depth arm still marches (%d steps, reason "
						 "bits %u). Zero means CosTheta used the raw scale-bearing forward"),
					ScaledSteps, ReasonBits(Scaled)),
			ScaledSteps > 0 && HasReason(Scaled, EFlowVizInvalidReason::Valid)))
		{
			TestEqual(TEXT("normalizing RayCameraForward makes a 100x basis scale leave "
						 "the opaque clamp's step count unchanged"),
				ScaledSteps, UnitSteps);

			TestTrue(*FString::Printf(
					TEXT("...and leaves the reported scalar BIT-IDENTICAL (%.9g vs %.9g), "
						 "proving the scale changed neither geometry nor the sampled endpoint"),
					ReportedValue(Scaled), ReportedValue(Unit)),
				ReportedValue(Scaled) == ReportedValue(Unit));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
