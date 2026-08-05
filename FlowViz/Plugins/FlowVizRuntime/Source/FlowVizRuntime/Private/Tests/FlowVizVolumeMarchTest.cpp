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

	/** What the test dispatches. One entry per row of the results array. */
	struct FMarchConfig
	{
		const TCHAR* Name = nullptr;
		EFlowVizCompositeMode Mode = EFlowVizCompositeMode::Maximum;
		bool bMaskedStatus = false;
		bool bLighting = false;
		float CropMinX = 0.0f;
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

	/** OutValue channel meanings, named rather than spelled .R/.G/.B/.A at every use. */
	static float ReportedValue(const FLinearColor& Pixel) { return Pixel.R; }
	static float ReportedAlpha(const FLinearColor& Pixel) { return Pixel.G; }
	static uint32 ReasonBits(const FLinearColor& Pixel)
	{
		return static_cast<uint32>(FMath::RoundToInt(Pixel.B));
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
			Transform.MakeShaderParameters(FieldLayout, VolumeParams).IsOk()))
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
			// an opacity change.
			constexpr int32 LutWidth = 16;
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
					const float T = static_cast<float>(Index) / static_cast<float>(LutWidth - 1);
					Lut[Index] = FFloat16Color(FLinearColor(T, 0.5f, 1.0f - T, 0.25f));
				}
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, LutWidth, 1);
				RHICmdList.UpdateTexture2D(LutTexture, 0, Region,
					LutWidth * sizeof(FFloat16Color), reinterpret_cast<const uint8*>(Lut.GetData()));
			}

			for (int32 ConfigIndex = 0; ConfigIndex < NumConfigs; ++ConfigIndex)
			{
				const FMarchConfig& Config = Configs[ConfigIndex];
				FMarchResult& Result = Results[ConfigIndex];

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

				// Looking down +X: the axis the field varies along, so the
				// composite modes see a changing value and are not an identity
				// map on this fixture.
				FlowVizRayMarch::SetLookAtCamera(*Params, FVector3f(1.0f, 0.0f, 0.0f),
					FIntPoint(OutputW, OutputH), /*bOrthographic=*/true,
					/*DistanceScale=*/1.0f, /*HorizontalFovDegrees=*/60.0f);

				Params->CompositeMode = static_cast<uint32>(Config.Mode);
				Params->bEnableLighting = Config.bLighting ? 1u : 0u;
				Params->CropBoxMin = FVector3f(Config.CropMinX, 0.0f, 0.0f);
				Params->CropBoxMax = FVector3f(1.0f, 1.0f, 1.0f);

				// NEAREST, not filtered. A filtered fetch blends neighbouring
				// voxels, and then a sample no longer decodes to the voxel it
				// came from - which is the property every assertion here uses.
				Params->bFilterField = 0;

				// Spans the whole field, so nothing is under or over range and
				// those reason bits stay out of the comparison.
				Params->ValueRangeMin = 0.0f;
				Params->ValueRangeMax = FieldMax;

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
			(ReasonBits(Max.ValueAt(CentreX, CentreY)) & 1u /* FLOWVIZ_REASON_VALID */) != 0u);

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
				if (StepCount(Pixel) > 0 && (ReasonBits(Pixel) & 1u) != 0u)
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
				StepCount(Masked) > 0 && (ReasonBits(Masked) & 1u) != 0u))
		{
			TestEqual(*FString::Printf(
					TEXT("masking voxels X<%d raises the minimum by exactly %d - the masked "
						 "values are excluded from the composite rather than averaged in"),
					MaskedBelowI, MaskedBelowI),
				static_cast<double>(ReportedValue(Masked) - ReportedValue(Unmasked)),
				static_cast<double>(MaskedBelowI), 1e-4);

			TestTrue(TEXT("...and the ray REPORTS having crossed masked voxels (REASON_MASKED), "
						  "so the renderer can colour the cause rather than silently dropping it"),
				(ReasonBits(Masked) & 4u /* FLOWVIZ_REASON_MASKED */) != 0u);

			TestFalse(TEXT("...while the unmasked render of the same ray does NOT report masking, "
						   "which is what proves the bit above came from the status volume and "
						   "not from something set on every ray"),
				(ReasonBits(Unmasked) & 4u) != 0u);
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

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
