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
 * OutValue.z, on the GPU, one cause at a time.
 *
 * WHAT THIS FILE CATCHES THAT NOTHING ELSE DOES: a reason bit the renderer
 * promises and never sets, or sets for the wrong cause.
 *
 * FlowVizVolumeRayMarchShader.h states the promise plainly - OutValue.z
 * "distinguishes NaN from masked from absent, so 'this pixel is not data' and
 * 'this pixel is not data because the mask rejected it' are separate
 * assertions". Before this file existed, three of the eight reason values had
 * ever been asserted anywhere: VALID, MASKED and NONE. UNKNOWN, NaN, INFINITE,
 * UNDER_RANGE and OVER_RANGE had zero assertions between them. The channel that
 * makes the renderer's failure modes nameable was five-eighths unverified.
 *
 * WHY COLOUR CANNOT COVER FOR IT, which is the reason this file renders instead
 * of reasoning:
 *
 *   - NaN and INFINITE both return NaNColor (FlowVizVolumeRayMarch.usf:420-421).
 *     Two different causes, one magenta pixel. No colour assertion can tell them
 *     apart, ever. OutValue.z is the only discriminator that exists, so the pair
 *     below renders both and asserts the colours MATCH while the reasons DIFFER.
 *   - UNKNOWN returns NoDataColor, whose alpha is zero, so an all-unknown ray
 *     and a ray that never entered the volume are both a black pixel. Again the
 *     only difference is in .z, and again it is asserted here.
 *
 * EVERY ASSERTION READS .z THROUGH FlowVizRayMarch::DecodeReason, the decoder
 * the plugin ships to its callers. That decoder had ZERO call sites anywhere in
 * the plugin, tests included, until these tests called it - so the function every
 * consumer of this channel depends on had never once been executed. A decoder
 * nothing calls cannot be observed to be wrong. Reading .z here with a
 * hand-written mask would have left it that way.
 *
 * WHAT THIS FILE DOES NOT DO: check that the C++ enumerators are the numbers the
 * .usf writes. That is FlowViz.Render.ReasonCodes, which parses the shader text.
 * The two are halves of one pin - this one proves the shader SETS the right
 * cause, that one proves both sides AGREE on what the cause is called. A
 * renumbering of both sides at once passes here and fails there; a shader that
 * flags an infinity as NaN passes there and fails here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizReasonChannelTest,
	"FlowViz.Render.ReasonChannel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizReasonFixture
{
	/** 16 x 8 x 4 cells, matching FlowVizVolumeMarchTest so the framing is a known quantity. */
	static const FIntVector Extent(16, 8, 4);

	/** Distinct, non-unit, exact in binary - so no assertion here is a tolerance in disguise. */
	static const FVector Spacing(0.125, 0.5, 0.25);

	constexpr int32 OutputW = 32;
	constexpr int32 OutputH = 32;

	/** A value names the voxel it came from, so a reported scalar is checkable. */
	static float FieldValue(int32 I, int32 J, int32 K)
	{
		return static_cast<float>(I + 16 * J + 128 * K);
	}

	constexpr float FieldMax = 511.0f;

	/**
	 * NaN and +infinity BY BIT PATTERN, not by arithmetic.
	 *
	 * 0.0f/0.0f and 1.0f/0.0f are undefined behaviour the optimiser is entitled
	 * to fold, and -ffast-math is entitled to assume they never happen. A fixture
	 * whose "NaN" quietly compiled to a zero would make every assertion below
	 * vacuous in the most convincing possible way: the render would succeed and
	 * report VALID, and the test would be measuring nothing. The bit patterns are
	 * IEEE-754's own and cannot be constant-folded into something finite; the
	 * control block asserts they survived to the array.
	 */
	static float QuietNaN()
	{
		const uint32 Bits = 0x7FC00000u;
		float Value;
		FMemory::Memcpy(&Value, &Bits, sizeof(float));
		return Value;
	}

	static float PositiveInfinity()
	{
		const uint32 Bits = 0x7F800000u;
		float Value;
		FMemory::Memcpy(&Value, &Bits, sizeof(float));
		return Value;
	}

	/** Which field texture a config marches. */
	enum class EFieldChoice
	{
		/** Finite, decodable: f = i + 16j + 128k. */
		Normal,
		/** Every voxel a quiet NaN, with a VALID status byte - so only the shader's own check can catch it. */
		AllNaN,
		/** Every voxel +infinity, likewise. */
		AllInfinite,
	};

	/** Which status texture a config binds. */
	enum class EStatusChoice
	{
		AllValid,
		/** Status byte 0 everywhere: "nothing was ever written here". */
		AllUnknown,
		/** Status flagged NaN, over a field that is perfectly finite. */
		AllNaNFlagged,
		/** Status flagged infinite, likewise. */
		AllInfiniteFlagged,
	};

	struct FReasonConfig
	{
		const TCHAR* Name = nullptr;
		EFlowVizCompositeMode Mode = EFlowVizCompositeMode::Minimum;
		EFieldChoice Field = EFieldChoice::Normal;
		EStatusChoice Status = EStatusChoice::AllValid;

		/*
		 * The transfer function's domain.
		 *
		 * Defaulted to the whole field, exactly as FlowVizVolumeMarchTest does and
		 * for the same reason: nothing is out of range, so UNDER_RANGE and
		 * OVER_RANGE stay out of every comparison that is not about them. The two
		 * range configs opt into a narrower domain, because a range bit is
		 * unobservable on a domain that covers every sample.
		 */
		bool bNarrowRange = false;
		float RangeMin = 0.0f;
		float RangeMax = FieldMax;
	};

	struct FReasonResult
	{
		TArray<FLinearColor> Value;
		TArray<FLinearColor> Color;
		bool bDispatched = false;

		const FLinearColor& ValueAt(int32 X, int32 Y) const { return Value[Y * OutputW + X]; }
		const FLinearColor& ColorAt(int32 X, int32 Y) const { return Color[Y * OutputW + X]; }
	};

	static float ReportedValue(const FLinearColor& Pixel) { return Pixel.R; }
	static int32 StepCount(const FLinearColor& Pixel) { return FMath::RoundToInt(Pixel.A); }

	/**
	 * The reason, decoded by the SHIPPED decoder. See the file comment: this is
	 * the function every caller of this channel uses, and nothing called it.
	 */
	static EFlowVizInvalidReason ReasonOf(const FLinearColor& Pixel)
	{
		return FlowVizRayMarch::DecodeReason(Pixel.B);
	}

	static bool HasReason(const FLinearColor& Pixel, EFlowVizInvalidReason Reason)
	{
		return EnumHasAnyFlags(ReasonOf(Pixel), Reason);
	}

	/** The bits, for a failure message that prints what was actually found. */
	static uint32 RawReason(const FLinearColor& Pixel)
	{
		return static_cast<uint32>(ReasonOf(Pixel));
	}

	static float RgbDistance(const FLinearColor& A, const FLinearColor& B)
	{
		return FMath::Sqrt(FMath::Square(A.R - B.R) + FMath::Square(A.G - B.G)
			+ FMath::Square(A.B - B.B));
	}
}

bool FFlowVizReasonChannelTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizReasonFixture;

	// SKIP WITH A LOGGED REASON, never a silent pass. Tools/run_tests.sh scrapes
	// this marker and reports the test by name rather than counting it as green.
	if (!GIsRHIInitialized || GUsingNullRHI)
	{
		AddWarning(TEXT(
			"SKIPPED: FlowViz.Render.ReasonChannel needs a real RHI device and this run has none "
			"(-nullrhi). NOTHING about the OutValue.z reason channel was verified - not the NaN "
			"and INFINITE distinction (which no colour assertion can make), not UNKNOWN, and "
			"neither range bit. "
			"Re-run with: RHI=1 Tools/build_lock.sh ./Tools/run_tests.sh FlowViz.Render"));
		return true;
	}

	/* == The fixture ========================================================= */

	FCFDVizGrid Grid;
	Grid.Dimensions = Extent;
	Grid.Origin = FVector::ZeroVector;
	Grid.Spacing = Spacing;

	FFlowVizVolumeTransform Transform;
	Transform.Grid = Grid;
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

	// Three field volumes: one finite, one entirely NaN, one entirely infinite.
	// Written through the layout's own offsets, so a padded or reordered layout is
	// followed rather than second-guessed.
	TArray<uint8> NormalFieldBytes;
	TArray<uint8> NaNFieldBytes;
	TArray<uint8> InfiniteFieldBytes;
	NormalFieldBytes.SetNumZeroed(static_cast<int32>(FieldLayout.GetTextureVolumeBytes()));
	NaNFieldBytes.SetNumZeroed(static_cast<int32>(FieldLayout.GetTextureVolumeBytes()));
	InfiniteFieldBytes.SetNumZeroed(static_cast<int32>(FieldLayout.GetTextureVolumeBytes()));

	const float NaNValue = QuietNaN();
	const float InfValue = PositiveInfinity();

	// Four status volumes, one per cause the status byte can name.
	TArray<uint8> ValidStatusBytes;
	TArray<uint8> UnknownStatusBytes;
	TArray<uint8> NaNStatusBytes;
	TArray<uint8> InfiniteStatusBytes;
	ValidStatusBytes.SetNumZeroed(static_cast<int32>(StatusLayout.GetTextureVolumeBytes()));
	UnknownStatusBytes.SetNumZeroed(static_cast<int32>(StatusLayout.GetTextureVolumeBytes()));
	NaNStatusBytes.SetNumZeroed(static_cast<int32>(StatusLayout.GetTextureVolumeBytes()));
	InfiniteStatusBytes.SetNumZeroed(static_cast<int32>(StatusLayout.GetTextureVolumeBytes()));

	for (int32 K = 0; K < Extent.Z; ++K)
	{
		for (int32 J = 0; J < Extent.Y; ++J)
		{
			for (int32 I = 0; I < Extent.X; ++I)
			{
				const int64 FieldOffset = FieldLayout.GetTextureVoxelOffset(I, J, K);
				const float Normal = FieldValue(I, J, K);
				FMemory::Memcpy(NormalFieldBytes.GetData() + FieldOffset, &Normal, sizeof(float));
				FMemory::Memcpy(NaNFieldBytes.GetData() + FieldOffset, &NaNValue, sizeof(float));
				FMemory::Memcpy(InfiniteFieldBytes.GetData() + FieldOffset, &InfValue, sizeof(float));

				const int32 StatusOffset =
					static_cast<int32>(StatusLayout.GetTextureVoxelOffset(I, J, K));
				ValidStatusBytes[StatusOffset] = FlowVizVoxelStatus::Valid;
				UnknownStatusBytes[StatusOffset] = FlowVizVoxelStatus::Unknown;
				NaNStatusBytes[StatusOffset] = FlowVizVoxelStatus::NaN;
				InfiniteStatusBytes[StatusOffset] = FlowVizVoxelStatus::Infinite;
			}
		}
	}

	/* == The fixture is not degenerate ======================================= */
	/*
	 * WHAT THIS BLOCK CATCHES: a fixture on which the GPU assertions cannot fail.
	 * Each property they rely on is checked here first, on the CPU, where a
	 * failure names the fixture rather than the shader.
	 */
	{
		// The single most dangerous degeneracy available to this file: a "NaN"
		// that is really a zero renders as ordinary VALID data, and every
		// assertion about NaN below would be describing a finite number.
		float RoundTripNaN = 0.0f;
		float RoundTripInf = 0.0f;
		FMemory::Memcpy(&RoundTripNaN,
			NaNFieldBytes.GetData() + FieldLayout.GetTextureVoxelOffset(3, 2, 1), sizeof(float));
		FMemory::Memcpy(&RoundTripInf,
			InfiniteFieldBytes.GetData() + FieldLayout.GetTextureVoxelOffset(3, 2, 1), sizeof(float));

		if (!TestTrue(TEXT("CONTROL: the NaN field really holds a NaN after the round trip through "
						   "the byte array. A quiet zero here would render as ordinary valid data "
						   "and every NaN assertion below would be describing a finite number"),
				FMath::IsNaN(RoundTripNaN)))
		{
			return false;
		}

		if (!TestTrue(TEXT("CONTROL: the infinite field really holds an infinity, and it is NOT a "
						   "NaN - the two causes must be distinguishable in the FIXTURE before "
						   "they can be distinguished in the render"),
				!FMath::IsFinite(RoundTripInf) && !FMath::IsNaN(RoundTripInf)))
		{
			return false;
		}

		// The shader's non-finite check is gated on this flag. With it zero, a NaN
		// field would sail through as valid data and the NaN configs would report
		// VALID - a red whose cause is the fixture, not the shader.
		if (!TestEqual(TEXT("CONTROL: the float32 layout asks the shader to reject non-finite "
							"values (bRejectNonFinite). Zero here would disable the branch the NaN "
							"and INFINITE configs exist to exercise"),
				static_cast<int32>(VolumeParams.bRejectNonFinite), 1))
		{
			return false;
		}

		// The status path and the value path are different code. A status byte
		// that already carried Valid would let the NaN-flagged config pass through
		// FlowVizStatusToReason's early return and never reach the flag.
		TestTrue(TEXT("CONTROL: the NaN and Infinite status bytes do NOT carry the Valid bit, so "
					  "the shader's status gate cannot short-circuit past them"),
			(FlowVizVoxelStatus::NaN & FlowVizVoxelStatus::Valid) == 0
				&& (FlowVizVoxelStatus::Infinite & FlowVizVoxelStatus::Valid) == 0);

		TestTrue(TEXT("CONTROL: the four status bytes this fixture writes are mutually distinct, "
					  "so a render that reported the wrong one is a different number"),
			FlowVizVoxelStatus::Unknown != FlowVizVoxelStatus::Valid
				&& FlowVizVoxelStatus::NaN != FlowVizVoxelStatus::Infinite
				&& FlowVizVoxelStatus::NaN != FlowVizVoxelStatus::Valid);
	}

	/* == The render ========================================================== */

	TArray<FReasonConfig> PassOneConfigs;
	TArray<FReasonResult> PassOneResults;
	TArray<FReasonConfig> PassTwoConfigs;
	TArray<FReasonResult> PassTwoResults;

	FString SetupError;

	// One render command, parameterised by which config list to run, so pass two
	// goes through byte-for-byte the same setup, upload, dispatch and readback as
	// pass one. A second bespoke path could differ in a way that made the two
	// passes incomparable - and pass two's expectations are stated relative to a
	// number pass one measured.
	const auto RunPass = [&](const TArray<FReasonConfig>& Configs, TArray<FReasonResult>& Results)
	{
	ENQUEUE_RENDER_COMMAND(FlowVizReasonChannel)(
		[&](FRHICommandListImmediate& RHICmdList)
		{
			FCFDVizResult Created = FCFDVizResult::Ok();

			FTextureRHIRef NormalField = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, FieldLayout, TEXT("FlowVizReasonFieldNormal"), Created);
			FTextureRHIRef NaNField = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, FieldLayout, TEXT("FlowVizReasonFieldNaN"), Created);
			FTextureRHIRef InfiniteField = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, FieldLayout, TEXT("FlowVizReasonFieldInf"), Created);
			if (!NormalField.IsValid() || !NaNField.IsValid() || !InfiniteField.IsValid())
			{
				SetupError = TEXT("field texture: ") + Created.Message;
				return;
			}

			FTextureRHIRef ValidStatus = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, StatusLayout, TEXT("FlowVizReasonStatusValid"), Created);
			FTextureRHIRef UnknownStatus = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, StatusLayout, TEXT("FlowVizReasonStatusUnknown"), Created);
			FTextureRHIRef NaNStatus = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, StatusLayout, TEXT("FlowVizReasonStatusNaN"), Created);
			FTextureRHIRef InfiniteStatus = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, StatusLayout, TEXT("FlowVizReasonStatusInf"), Created);
			if (!ValidStatus.IsValid() || !UnknownStatus.IsValid() || !NaNStatus.IsValid()
				|| !InfiniteStatus.IsValid())
			{
				SetupError = TEXT("status texture: ") + Created.Message;
				return;
			}

			// The production upload path, not a bespoke test one, so a bug in
			// stride or bounds is a bug this test can see.
			const FCFDVizResult Uploads[] = {
				FlowVizVolumeRHI::UpdateVolumeTexture(RHICmdList, NormalField, FieldLayout, NormalFieldBytes),
				FlowVizVolumeRHI::UpdateVolumeTexture(RHICmdList, NaNField, FieldLayout, NaNFieldBytes),
				FlowVizVolumeRHI::UpdateVolumeTexture(RHICmdList, InfiniteField, FieldLayout, InfiniteFieldBytes),
				FlowVizVolumeRHI::UpdateVolumeTexture(RHICmdList, ValidStatus, StatusLayout, ValidStatusBytes),
				FlowVizVolumeRHI::UpdateVolumeTexture(RHICmdList, UnknownStatus, StatusLayout, UnknownStatusBytes),
				FlowVizVolumeRHI::UpdateVolumeTexture(RHICmdList, NaNStatus, StatusLayout, NaNStatusBytes),
				FlowVizVolumeRHI::UpdateVolumeTexture(RHICmdList, InfiniteStatus, StatusLayout, InfiniteStatusBytes),
			};
			for (const FCFDVizResult& Upload : Uploads)
			{
				if (!Upload.IsOk())
				{
					SetupError = TEXT("upload: ") + Upload.Message;
					return;
				}
			}

			// A 16-entry LUT with constant non-zero alpha, so an in-range pixel is
			// a colour and not a hole.
			constexpr int32 LutWidth = 16;
			FTextureRHIRef LutTexture;
			{
				const FRHITextureCreateDesc LutDesc =
					FRHITextureCreateDesc::Create2D(TEXT("FlowVizReasonLut"))
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

			for (int32 ConfigIndex = 0; ConfigIndex < Configs.Num(); ++ConfigIndex)
			{
				const FReasonConfig& Config = Configs[ConfigIndex];
				FReasonResult& Result = Results[ConfigIndex];

				FRDGBuilder GraphBuilder(RHICmdList);

				const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
					FIntPoint(OutputW, OutputH),
					PF_A32B32G32R32F,
					FClearValueBinding::None,
					TexCreate_ShaderResource | TexCreate_UAV);

				FRDGTextureRef ColorTexture = GraphBuilder.CreateTexture(Desc, TEXT("ReasonColor"));
				FRDGTextureRef ValueTexture = GraphBuilder.CreateTexture(Desc, TEXT("ReasonValue"));

				FFlowVizVolumeRayMarchParameters* Params =
					GraphBuilder.AllocParameters<FFlowVizVolumeRayMarchParameters>();
				FlowVizRayMarch::FillDefaults(*Params);
				FlowVizRayMarch::FillFromVolumeParameters(VolumeParams, *Params);

				// Looking down +X, the axis the field varies along.
				FlowVizRayMarch::SetLookAtCamera(*Params, FVector3f(1.0f, 0.0f, 0.0f),
					FIntPoint(OutputW, OutputH), /*bOrthographic=*/true,
					/*DistanceScale=*/1.0f, /*HorizontalFovDegrees=*/60.0f);

				Params->CompositeMode = static_cast<uint32>(Config.Mode);

				// NEAREST, not filtered: a filtered fetch blends neighbours, and a
				// blend of a NaN with a finite value is a NaN at a voxel the
				// fixture did not mark - which would make "which voxel caused
				// this" unanswerable.
				Params->bFilterField = 0;

				// The X component, not the magnitude: sqrt(x*x) is an extra
				// operation between the stored bit pattern and the value under
				// test, and this file is about that exact bit pattern.
				Params->ComponentMode = static_cast<uint32>(EFlowVizComponentMode::X);

				Params->ValueRangeMin = Config.bNarrowRange ? Config.RangeMin : 0.0f;
				Params->ValueRangeMax = Config.bNarrowRange ? Config.RangeMax : FieldMax;

				FRHITexture* Field = NormalField;
				if (Config.Field == EFieldChoice::AllNaN)          { Field = NaNField; }
				else if (Config.Field == EFieldChoice::AllInfinite) { Field = InfiniteField; }

				FRHITexture* Status = ValidStatus;
				if (Config.Status == EStatusChoice::AllUnknown)           { Status = UnknownStatus; }
				else if (Config.Status == EStatusChoice::AllNaNFlagged)   { Status = NaNStatus; }
				else if (Config.Status == EStatusChoice::AllInfiniteFlagged) { Status = InfiniteStatus; }

				FlowVizRayMarch::SetVolumeTextures(*Params, Field, Status, false);
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
					new FRHIGPUTextureReadback(TEXT("ReasonValueReadback"));
				FRHIGPUTextureReadback* ColorReadback =
					new FRHIGPUTextureReadback(TEXT("ReasonColorReadback"));
				AddEnqueueCopyPass(GraphBuilder, ValueReadback, ValueTexture);
				AddEnqueueCopyPass(GraphBuilder, ColorReadback, ColorTexture);
				GraphBuilder.Execute();

				// Reading before the GPU has finished returns whatever the staging
				// buffer held, which is a plausible-looking lie.
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

	/* == Pass one: the causes that need no measured range ==================== */

	enum { CfgValid = 0, CfgValueNaN = 1, CfgValueInf = 2,
		   CfgStatusNaN = 3, CfgStatusInf = 4, CfgUnknown = 5 };
	{
		const FReasonConfig Configs[] = {
			// The baseline every other row is contrasted against.
			{ TEXT("Valid"),     EFlowVizCompositeMode::Minimum,
			  EFieldChoice::Normal,      EStatusChoice::AllValid },
			// The shader's OWN non-finite check: the status byte says VALID, so
			// only FlowVizIsNaN and the magnitude test can catch these.
			{ TEXT("ValueNaN"),  EFlowVizCompositeMode::Minimum,
			  EFieldChoice::AllNaN,      EStatusChoice::AllValid },
			{ TEXT("ValueInf"),  EFlowVizCompositeMode::Minimum,
			  EFieldChoice::AllInfinite, EStatusChoice::AllValid },
			// The STATUS path: a perfectly finite field whose status volume says
			// the producer already found it non-finite. Different code, same bits.
			{ TEXT("StatusNaN"), EFlowVizCompositeMode::Minimum,
			  EFieldChoice::Normal,      EStatusChoice::AllNaNFlagged },
			{ TEXT("StatusInf"), EFlowVizCompositeMode::Minimum,
			  EFieldChoice::Normal,      EStatusChoice::AllInfiniteFlagged },
			// Status 0: "nothing was ever written here", which is absent, not zero.
			{ TEXT("Unknown"),   EFlowVizCompositeMode::Minimum,
			  EFieldChoice::Normal,      EStatusChoice::AllUnknown },
		};
		PassOneConfigs.Append(Configs, UE_ARRAY_COUNT(Configs));
	}

	PassOneResults.SetNum(PassOneConfigs.Num());
	RunPass(PassOneConfigs, PassOneResults);

	if (!SetupError.IsEmpty())
	{
		AddError(FString::Printf(
			TEXT("The fixture volumes could not be created or uploaded, so NOTHING about the "
				 "reason channel was verified. This is a harness failure, not a pass: %s"),
			*SetupError));
		return false;
	}

	for (int32 Index = 0; Index < PassOneConfigs.Num(); ++Index)
	{
		if (!TestTrue(*FString::Printf(
					TEXT("config '%s' dispatched - false here means the shader is not in the "
						 "global shader map, i.e. it FAILED TO COMPILE"),
					PassOneConfigs[Index].Name),
				PassOneResults[Index].bDispatched)
			|| !TestEqual(*FString::Printf(TEXT("config '%s' read back a full image"),
					PassOneConfigs[Index].Name),
					PassOneResults[Index].Value.Num(), OutputW * OutputH))
		{
			AddError(TEXT("The readback returned no pixels. Nothing below was verified."));
			return false;
		}
	}

	const int32 CentreX = OutputW / 2;
	const int32 CentreY = OutputH / 2;

	/* == The baseline ray is VALID and nothing else =========================== */
	/*
	 * Read this first. Every assertion below is "the render reports cause X",
	 * and each is worth nothing unless a correct render reports NO cause. If the
	 * baseline already carried NaN, UNKNOWN or a range bit, then finding that bit
	 * in a later render would say nothing about the fixture that produced it.
	 */
	const FLinearColor& ValidPixel = PassOneResults[CfgValid].ValueAt(CentreX, CentreY);
	{
		if (!TestTrue(TEXT("the centre ray marched the volume, so the per-pixel assertions below "
						   "are made against a ray that actually crossed it"),
				StepCount(ValidPixel) > 0))
		{
			return false;
		}

		if (!TestEqual(*FString::Printf(
					TEXT("CONTROL: a fully valid, fully in-range ray reports EXACTLY "
						 "EFlowVizInvalidReason::Valid and no other bit (got 0x%02x). Any extra "
						 "bit here would appear in every render below and make 'this config "
						 "raised cause X' unattributable to that config"),
					RawReason(ValidPixel)),
				RawReason(ValidPixel), static_cast<uint32>(EFlowVizInvalidReason::Valid)))
		{
			return false;
		}

		// The other end of the channel, and the one the header calls out: NONE
		// means the ray never entered. The corner is outside the domain.
		const FLinearColor& Corner = PassOneResults[CfgValid].ValueAt(0, 0);
		TestEqual(TEXT("CONTROL: a corner ray misses the domain and takes zero steps"),
			StepCount(Corner), 0);
		TestEqual(*FString::Printf(
				TEXT("...and reports EFlowVizInvalidReason::None (got 0x%02x)"), RawReason(Corner)),
			RawReason(Corner), static_cast<uint32>(EFlowVizInvalidReason::None));
	}

	/* == NaN and INFINITE, which no colour assertion can separate ============ */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE CAN: an infinity reported as a
	 * NaN, or the reverse.
	 *
	 * FlowVizVolumeRayMarch.usf:420-421 returns NaNColor for BOTH. That is a
	 * deliberate display choice - both are "not a number you can plot" - and it
	 * means the rendered picture contains no information about which one it was.
	 * The colours are asserted EQUAL below precisely to establish that, and then
	 * the reasons are asserted DIFFERENT. If a future change gave infinities their
	 * own colour the first assertion would fail and this comment would be wrong,
	 * which is the intended way to find out.
	 *
	 * CVF section 4.4.7 is why the distinction is worth a channel: +inf is how the
	 * format spells "no valid data", so an infinity is frequently a DELIBERATE
	 * absence marker while a NaN is usually a solver that diverged. Collapsing
	 * them turns "this region was masked by the producer" into "your simulation
	 * blew up".
	 */
	{
		const FLinearColor& ValueNaN = PassOneResults[CfgValueNaN].ValueAt(CentreX, CentreY);
		const FLinearColor& ValueInf = PassOneResults[CfgValueInf].ValueAt(CentreX, CentreY);
		const FLinearColor& StatusNaN = PassOneResults[CfgStatusNaN].ValueAt(CentreX, CentreY);
		const FLinearColor& StatusInf = PassOneResults[CfgStatusInf].ValueAt(CentreX, CentreY);

		const FLinearColor& ValueNaNColor = PassOneResults[CfgValueNaN].ColorAt(CentreX, CentreY);
		const FLinearColor& ValueInfColor = PassOneResults[CfgValueInf].ColorAt(CentreX, CentreY);

		// --- THE CONTROL, FIRST ---
		// All four rays must have MARCHED. A ray that never entered reports NONE,
		// which contains none of the bits below and would fail them for a reason
		// that has nothing to do with non-finite detection.
		if (!TestTrue(*FString::Printf(
					TEXT("CONTROL: all four non-finite rays crossed the volume (%d, %d, %d, %d "
						 "steps). A ray that never entered reports NONE and would fail every "
						 "assertion below with a misleading cause"),
					StepCount(ValueNaN), StepCount(ValueInf),
					StepCount(StatusNaN), StepCount(StatusInf)),
				StepCount(ValueNaN) > 0 && StepCount(ValueInf) > 0
					&& StepCount(StatusNaN) > 0 && StepCount(StatusInf) > 0))
		{
			return false;
		}

		/* -- The shader's own check, on the VALUE -------------------------- */

		TestTrue(*FString::Printf(
				TEXT("a field of NaNs with VALID status bytes is reported as "
					 "EFlowVizInvalidReason::NaN (got 0x%02x). The status volume says these "
					 "voxels are fine, so only the shader's own FlowVizIsNaN can catch them - "
					 "which is the case a filtered fetch straddling a bad voxel produces"),
				RawReason(ValueNaN)),
			HasReason(ValueNaN, EFlowVizInvalidReason::NaN));

		TestFalse(*FString::Printf(
				TEXT("...and NOT as INFINITE (0x%02x). This is the half of the pair no colour "
					 "assertion can make, because both causes draw NaNColor"),
				RawReason(ValueNaN)),
			HasReason(ValueNaN, EFlowVizInvalidReason::Infinite));

		TestTrue(*FString::Printf(
				TEXT("a field of +infinities is reported as EFlowVizInvalidReason::Infinite "
					 "(got 0x%02x)"), RawReason(ValueInf)),
			HasReason(ValueInf, EFlowVizInvalidReason::Infinite));

		TestFalse(*FString::Printf(
				TEXT("...and NOT as NaN (0x%02x). Stated separately from the assertion above: a "
					 "shader that routed every non-finite value to one bit would satisfy either "
					 "one alone"),
				RawReason(ValueInf)),
			HasReason(ValueInf, EFlowVizInvalidReason::NaN));

		/* -- THE POINT OF THE WHOLE FILE ----------------------------------- */

		TestTrue(*FString::Printf(
				TEXT("the NaN render and the INFINITE render are THE SAME COLOUR "
					 "(%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f) - both NaNColor, by design. This is "
					 "asserted, not assumed: it is what proves the reason channel is the only "
					 "place the two causes are distinguishable"),
				ValueNaNColor.R, ValueNaNColor.G, ValueNaNColor.B,
				ValueInfColor.R, ValueInfColor.G, ValueInfColor.B),
			RgbDistance(ValueNaNColor, ValueInfColor) < 1e-4f);

		TestNotEqual(*FString::Printf(
				TEXT("...while their REASONS differ (0x%02x vs 0x%02x). Identical pixels, "
					 "different causes, and OutValue.z is the sole discriminator - which is why "
					 "leaving it unasserted left the renderer unable to be wrong in public"),
				RawReason(ValueNaN), RawReason(ValueInf)),
			RawReason(ValueNaN), RawReason(ValueInf));

		/* -- The STATUS path, which is different code ----------------------- */
		/*
		 * FlowVizStatusToReason maps a status BYTE to a reason; the block above
		 * exercised FlowVizSampleVolume's own inspection of the VALUE. Either can
		 * be wrong alone: a producer-flagged infinity over a finite field reaches
		 * only this path.
		 */
		TestTrue(*FString::Printf(
				TEXT("a status volume flagged NaN over a FINITE field reports "
					 "EFlowVizInvalidReason::NaN (0x%02x) and not INFINITE - the status path, "
					 "which the value path above cannot reach"),
				RawReason(StatusNaN)),
			HasReason(StatusNaN, EFlowVizInvalidReason::NaN)
				&& !HasReason(StatusNaN, EFlowVizInvalidReason::Infinite));

		TestTrue(*FString::Printf(
				TEXT("a status volume flagged INFINITE over a FINITE field reports "
					 "EFlowVizInvalidReason::Infinite (0x%02x) and not NaN. CVF 4.4.7 spells "
					 "'no valid data' as +inf, so this is the bit that says 'the producer "
					 "excluded this region' rather than 'the solver diverged'"),
				RawReason(StatusInf)),
			HasReason(StatusInf, EFlowVizInvalidReason::Infinite)
				&& !HasReason(StatusInf, EFlowVizInvalidReason::NaN));

		// Neither non-finite ray may claim to have found usable data. VALID and
		// NaN on the same pixel would mean the composite sampled a NaN.
		TestFalse(*FString::Printf(
				TEXT("an all-NaN ray does NOT also report VALID (0x%02x) - a NaN that reached "
					 "the composite would poison the reported scalar"),
				RawReason(ValueNaN)),
			HasReason(ValueNaN, EFlowVizInvalidReason::Valid));

		TestFalse(*FString::Printf(TEXT("...nor does an all-infinite ray (0x%02x)"),
				RawReason(ValueInf)),
			HasReason(ValueInf, EFlowVizInvalidReason::Valid));
	}

	/* == UNKNOWN is not NONE, and both are black ============================= */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: "the ray found nothing"
	 * confused with "the ray went nowhere".
	 *
	 * The header states the distinction as the reason UNKNOWN exists: NONE means
	 * the ray never entered the volume; UNKNOWN means it did, and every voxel it
	 * met had status 0 - the fail-closed reading of a volume nobody wrote. Both
	 * render BLACK, because NoDataColor's alpha is zero and an empty pixel is also
	 * black, so the picture cannot tell them apart and .z is again the only
	 * discriminator. A user staring at a black region needs to know whether their
	 * data is missing or their camera is pointed at nothing.
	 */
	{
		const FLinearColor& Unknown = PassOneResults[CfgUnknown].ValueAt(CentreX, CentreY);
		const FLinearColor& Corner = PassOneResults[CfgUnknown].ValueAt(0, 0);
		const FLinearColor& UnknownColor = PassOneResults[CfgUnknown].ColorAt(CentreX, CentreY);
		const FLinearColor& CornerColor = PassOneResults[CfgUnknown].ColorAt(0, 0);

		// --- THE CONTROL, FIRST ---
		// The whole block turns on this ray having entered the volume. If it did
		// not, UNKNOWN and NONE are not being distinguished - they are both NONE.
		if (!TestTrue(*FString::Printf(
					TEXT("CONTROL: the all-unknown ray ENTERED the volume and took %d steps, "
						 "while the corner ray took %d. Without that difference this block "
						 "compares a ray to itself"),
					StepCount(Unknown), StepCount(Corner)),
				StepCount(Unknown) > 0 && StepCount(Corner) == 0))
		{
			return false;
		}

		TestEqual(*FString::Printf(
				TEXT("a ray that crosses a volume of status-0 voxels reports EXACTLY "
					 "EFlowVizInvalidReason::Unknown (got 0x%02x). Status 0 is 'nothing was ever "
					 "written here', which is absent - not zero, and not valid"),
				RawReason(Unknown)),
			RawReason(Unknown), static_cast<uint32>(EFlowVizInvalidReason::Unknown));

		TestFalse(*FString::Printf(
				TEXT("...and in particular does NOT report VALID (0x%02x). An absent status "
					 "texture read as 'everything is fine' is the fail-OPEN bug the whole status "
					 "gate exists to prevent"),
				RawReason(Unknown)),
			HasReason(Unknown, EFlowVizInvalidReason::Valid));

		TestNotEqual(*FString::Printf(
				TEXT("UNKNOWN (0x%02x) and NONE (0x%02x) are DIFFERENT answers: 'entered and "
					 "found nothing' versus 'never entered'"),
				RawReason(Unknown), RawReason(Corner)),
			RawReason(Unknown), RawReason(Corner));

		TestTrue(*FString::Printf(
				TEXT("...even though both pixels are the SAME BLACK (%.4f,%.4f,%.4f vs "
					 "%.4f,%.4f,%.4f). NoDataColor has zero alpha, so an all-unknown ray and an "
					 "empty one are the same picture and only .z separates them"),
				UnknownColor.R, UnknownColor.G, UnknownColor.B,
				CornerColor.R, CornerColor.G, CornerColor.B),
			RgbDistance(UnknownColor, CornerColor) < 1e-4f);
	}

	/* == Pass two: the range bits ============================================ */
	/*
	 * These need a domain that EXCLUDES part of the centre ray, and the ray's
	 * values are a property of the framing. The row constant is read back from
	 * pass one rather than recomputed here: a test that derived the row from the
	 * camera parameters would restate the projection code it is checking and would
	 * agree with it even when both were wrong.
	 *
	 * The narrowings are ONE-SIDED ON PURPOSE. A domain narrowed at both ends
	 * would put samples below it AND above it on the same ray, so .z would carry
	 * UNDER_RANGE and OVER_RANGE together and neither assertion could tell which
	 * end produced which bit. Each config below excludes exactly one end.
	 */
	const float CentreRowMin = ReportedValue(ValidPixel);

	enum { CfgUnder = 0, CfgOver = 1 };
	{
		const FReasonConfig Configs[] = {
			// Domain starts 4 above the row's first voxel and runs far past its
			// last, so samples fall UNDER it and none can fall over.
			{ TEXT("UnderRange"), EFlowVizCompositeMode::Minimum,
			  EFieldChoice::Normal, EStatusChoice::AllValid,
			  true, CentreRowMin + 4.0f, CentreRowMin + 1000.0f },
			// The mirror image: starts far below and ends 4 short of the last
			// voxel, so samples fall OVER it and none can fall under.
			{ TEXT("OverRange"),  EFlowVizCompositeMode::Maximum,
			  EFieldChoice::Normal, EStatusChoice::AllValid,
			  true, CentreRowMin - 1000.0f, CentreRowMin + 11.0f },
		};
		PassTwoConfigs.Append(Configs, UE_ARRAY_COUNT(Configs));
	}

	PassTwoResults.SetNum(PassTwoConfigs.Num());
	RunPass(PassTwoConfigs, PassTwoResults);

	if (!SetupError.IsEmpty())
	{
		AddError(FString::Printf(TEXT("The second pass could not be set up, so NEITHER range bit "
									  "was verified: %s"), *SetupError));
		return false;
	}

	for (int32 Index = 0; Index < PassTwoConfigs.Num(); ++Index)
	{
		if (!TestTrue(*FString::Printf(TEXT("second-pass config '%s' dispatched and read back"),
				PassTwoConfigs[Index].Name),
				PassTwoResults[Index].bDispatched
					&& PassTwoResults[Index].Value.Num() == OutputW * OutputH))
		{
			AddError(TEXT("Neither range bit was verified."));
			return false;
		}
	}

	/* == UNDER_RANGE and OVER_RANGE are opposite ends ======================== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a range comparison written
	 * backwards, and a range bit that never fires.
	 *
	 * Backwards is the interesting one. A shader that raised OVER_RANGE for a
	 * sample below the domain still colours the pixel a flag colour, still refuses
	 * to pretend it is in range, and still produces a picture a reviewer would
	 * accept - it just names the wrong end. VISUAL_QA rule 4 gives the two ends
	 * different colours precisely so a reader can tell "your scale starts too
	 * high" from "your scale ends too low"; getting them backwards inverts that
	 * advice. Each direction below is asserted to be PRESENT and its opposite to
	 * be ABSENT, because either alone is satisfied by a shader that sets both.
	 */
	{
		const FLinearColor& Under = PassTwoResults[CfgUnder].ValueAt(CentreX, CentreY);
		const FLinearColor& Over = PassTwoResults[CfgOver].ValueAt(CentreX, CentreY);

		// --- THE CONTROLS, FIRST, AND THERE ARE TWO ---
		//
		// The first says the rays marched. The second says the narrowing actually
		// excluded the resolved sample - without it, a domain that quietly stayed
		// full would put every sample in range, no bit would be raised, and the
		// failure would name the shader when the fixture was at fault.
		if (!TestTrue(*FString::Printf(
					TEXT("CONTROL: both range rays crossed the volume (%d and %d steps)"),
					StepCount(Under), StepCount(Over)),
				StepCount(Under) > 0 && StepCount(Over) > 0))
		{
			return false;
		}

		if (!TestTrue(*FString::Printf(
					TEXT("CONTROL: the narrowed domains really exclude the resolved samples - "
						 "Minimum resolved %.1f against a domain starting at %.1f, and Maximum "
						 "resolved %.1f against one ending at %.1f. A domain that still covered "
						 "them would raise no bit at all and blame the shader for the fixture"),
					ReportedValue(Under), CentreRowMin + 4.0f,
					ReportedValue(Over), CentreRowMin + 11.0f),
				ReportedValue(Under) < CentreRowMin + 4.0f
					&& ReportedValue(Over) > CentreRowMin + 11.0f))
		{
			return false;
		}

		// And the differential that makes the bits attributable to the narrowing:
		// the SAME ray, on the SAME data, reported neither bit under the full
		// domain of pass one.
		if (!TestFalse(*FString::Printf(
					TEXT("CONTROL: under the FULL domain the same ray reported neither range bit "
						 "(0x%02x). This is what makes the bits below a property of the narrowing "
						 "rather than something set on every ray"),
					RawReason(ValidPixel)),
				HasReason(ValidPixel,
					EFlowVizInvalidReason::UnderRange | EFlowVizInvalidReason::OverRange)))
		{
			return false;
		}

		TestTrue(*FString::Printf(
				TEXT("a sample BELOW the transfer function's domain reports "
					 "EFlowVizInvalidReason::UnderRange (got 0x%02x)"),
				RawReason(Under)),
			HasReason(Under, EFlowVizInvalidReason::UnderRange));

		TestFalse(*FString::Printf(
				TEXT("...and NOT OverRange (0x%02x). A comparison written backwards still flags "
					 "the pixel and still renders plausibly - it just tells the reader to fix "
					 "the opposite end of their scale"),
				RawReason(Under)),
			HasReason(Under, EFlowVizInvalidReason::OverRange));

		TestTrue(*FString::Printf(
				TEXT("a sample ABOVE the domain reports EFlowVizInvalidReason::OverRange (got "
					 "0x%02x)"), RawReason(Over)),
			HasReason(Over, EFlowVizInvalidReason::OverRange));

		TestFalse(*FString::Printf(TEXT("...and NOT UnderRange (0x%02x)"), RawReason(Over)),
			HasReason(Over, EFlowVizInvalidReason::UnderRange));

		// Out of range is NOT a rejection: the value stands and the sample is
		// still data (VISUAL_QA rule 4). A shader that treated out-of-range as
		// invalid would drop the bit AND the number, which is the quantitative
		// lie the flag colours exist to prevent.
		TestTrue(*FString::Printf(
				TEXT("an out-of-range ray is still VALID (0x%02x): out of range is a statement "
					 "about the COLORMAP, not a refusal of the data, so the scalar stands"),
				RawReason(Under)),
			HasReason(Under, EFlowVizInvalidReason::Valid)
				&& HasReason(Over, EFlowVizInvalidReason::Valid));
	}

	/* == Every bit the enum declares has now been seen ======================= */
	/*
	 * The completeness check, and the reason it is worth a block: a reason bit
	 * that no test ever OBSERVES is indistinguishable from one the shader never
	 * sets. Five of these eight were in exactly that state before this file.
	 * Collecting the union here means adding an enumerator to
	 * EFlowVizInvalidReason without rendering it fails - the bit would be missing
	 * from the union and named in the message.
	 */
	{
		uint32 SeenUnion = static_cast<uint32>(EFlowVizInvalidReason::None);
		const auto Observe = [&SeenUnion](const FLinearColor& Pixel)
		{
			SeenUnion |= RawReason(Pixel);
		};

		Observe(PassOneResults[CfgValid].ValueAt(CentreX, CentreY));
		Observe(PassOneResults[CfgValueNaN].ValueAt(CentreX, CentreY));
		Observe(PassOneResults[CfgValueInf].ValueAt(CentreX, CentreY));
		Observe(PassOneResults[CfgStatusNaN].ValueAt(CentreX, CentreY));
		Observe(PassOneResults[CfgStatusInf].ValueAt(CentreX, CentreY));
		Observe(PassOneResults[CfgUnknown].ValueAt(CentreX, CentreY));
		Observe(PassTwoResults[CfgUnder].ValueAt(CentreX, CentreY));
		Observe(PassTwoResults[CfgOver].ValueAt(CentreX, CentreY));

		// Masked is the one declared bit this file does not render, and it is
		// excluded DELIBERATELY rather than by omission: FlowViz.Render.VolumeMarch
		// already marches a masked region and asserts the bit, and re-rendering it
		// here would be a second fixture that can drift from that one. Every other
		// declared bit must appear in the union above.
		//
		// Subtracting it is what keeps this check honest. The alternative - a
		// hand-written expected constant - would have to be edited whenever the
		// enum grew, and the edit that silences the failure is the same edit that
		// hides a newly-unrendered bit.
		const uint32 Expected = static_cast<uint32>(FlowVizRayMarch::KnownInvalidReasons)
			& ~static_cast<uint32>(EFlowVizInvalidReason::Masked);

		TestEqual(*FString::Printf(
				TEXT("every bit EFlowVizInvalidReason declares - except Masked, which "
					 "FlowViz.Render.VolumeMarch owns - was actually OBSERVED coming out of the "
					 "GPU across the renders above (union 0x%02x, expected 0x%02x). A bit no "
					 "test has ever seen is indistinguishable from one the shader never sets, "
					 "which is what five of these eight were. If this fails because a NEW "
					 "enumerator was added, the fix is to render it, not to widen the mask"),
				SeenUnion, Expected),
			SeenUnion, Expected);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
