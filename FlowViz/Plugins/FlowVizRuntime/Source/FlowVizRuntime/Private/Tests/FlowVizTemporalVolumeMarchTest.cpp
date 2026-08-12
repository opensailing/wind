// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Render/FlowVizTransferFunction.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Render/FlowVizVolumeTexture.h"

#include "GlobalShader.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RHIGlobals.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

#include <cmath>
#include <limits>

/**
 * Temporal field sampling through the real compute shader and RHI.
 *
 * Source-contract tests pin the intended HLSL expression, but only GPU readback
 * can prove that frame B, alpha, nearest-frame status, vector magnitude,
 * gradients, and shadow rays all consume the same temporal field. Every arm
 * below uses distinct non-degenerate 3D textures and reads OutValue/OutColor
 * through the production pass.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTemporalVolumeMarchTest,
	"FlowViz.Render.TemporalVolumeMarch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizTemporalMarchFixture
{
	static const FIntVector Extent(8, 4, 4);
	static const FVector Spacing(1.0, 1.0, 1.0);
	constexpr int32 OutputW = 8;
	constexpr int32 OutputH = 8;
	constexpr int32 CentreX = OutputW / 2;
	constexpr int32 CentreY = OutputH / 2;
	constexpr int32 LutWidth = 16;

	enum class ELutKind : uint8
	{
		Constant,
		AlphaRamp,
	};

	struct FRenderSpec
	{
		FString Name;
		FFlowVizVolumeLayout FieldLayout;
		FFlowVizVolumeLayout StatusLayout;
		FFlowVizVolumeShaderParameters VolumeParams;
		TArray<uint8> FieldA;
		TArray<uint8> FieldB;
		TArray<uint8> StatusA;
		TArray<uint8> StatusB;
		float BlendAlpha = 0.0f;
		bool bBlendActive = false;
		EFlowVizCompositeMode CompositeMode = EFlowVizCompositeMode::Minimum;
		EFlowVizComponentMode ComponentMode = EFlowVizComponentMode::X;
		FVector3f CameraForward = FVector3f(1.0f, 0.0f, 0.0f);
		bool bLighting = false;
		FVector3f LightDirection = FVector3f(1.0f, 0.0f, 0.0f);
		float AmbientStrength = 0.35f;
		float DiffuseStrength = 0.65f;
		float OpacityMultiplier = 1.0f;
		float StepVoxels = FlowVizRayMarch::DefaultStepVoxels;
		float IsoValue = 0.0f;
		float ValueRangeMin = -2000.0f;
		float ValueRangeMax = 2000.0f;
		ELutKind LutKind = ELutKind::Constant;
	};

	struct FRenderResult
	{
		FLinearColor Value = FLinearColor::Transparent;
		FLinearColor Color = FLinearColor::Transparent;
		bool bDispatched = false;
		bool bReadBack = false;
	};

	static uint32 ReasonBits(const FLinearColor& Pixel)
	{
		return static_cast<uint32>(FlowVizRayMarch::DecodeReason(Pixel.B));
	}

	static bool HasReason(const FLinearColor& Pixel, EFlowVizInvalidReason Reason)
	{
		return EnumHasAnyFlags(FlowVizRayMarch::DecodeReason(Pixel.B), Reason);
	}

	static int32 StepCount(const FLinearColor& Pixel)
	{
		return FMath::RoundToInt(Pixel.A);
	}

	static bool MakeSpec(const FString& Name, int32 ComponentCount, FRenderSpec& Out, FString& OutError)
	{
		Out = FRenderSpec();
		Out.Name = Name;

		FCFDVizGrid Grid;
		Grid.Dimensions = Extent;
		Grid.Origin = FVector::ZeroVector;
		Grid.Spacing = Spacing;

		FFlowVizVolumeTransform Transform;
		Transform.Grid = Grid;
		Transform.Association = ECFDVizAssociation::Cell;

		const FCFDVizResult FieldResult = FFlowVizVolumeLayout::Make(
			Extent, ComponentCount, ECFDVizDataType::Float32, Out.FieldLayout);
		const FCFDVizResult StatusResult =
			FFlowVizVolumeLayout::MakeStatusLayout(Extent, Out.StatusLayout);
		const FCFDVizResult ParamsResult = Transform.MakeShaderParametersWithoutValueRange(
			Out.FieldLayout, Out.VolumeParams);
		if (!FieldResult.IsOk() || !StatusResult.IsOk() || !ParamsResult.IsOk())
		{
			OutError = FString::Printf(TEXT("%s layout: %s%s%s"), *Name,
				*FieldResult.Message, *StatusResult.Message, *ParamsResult.Message);
			return false;
		}

		Out.FieldA.SetNumZeroed(static_cast<int32>(Out.FieldLayout.GetTextureVolumeBytes()));
		Out.FieldB.SetNumZeroed(static_cast<int32>(Out.FieldLayout.GetTextureVolumeBytes()));
		Out.StatusA.Init(FlowVizVoxelStatus::Valid,
			static_cast<int32>(Out.StatusLayout.GetTextureVolumeBytes()));
		Out.StatusB = Out.StatusA;
		return true;
	}

	template <typename F>
	static void FillScalar(const FFlowVizVolumeLayout& Layout, TArray<uint8>& Bytes, F&& ValueAt)
	{
		for (int32 K = 0; K < Extent.Z; ++K)
		{
			for (int32 J = 0; J < Extent.Y; ++J)
			{
				for (int32 I = 0; I < Extent.X; ++I)
				{
					const float Value = ValueAt(I, J, K);
					const int64 Offset = Layout.GetTextureVoxelOffset(I, J, K);
					FMemory::Memcpy(Bytes.GetData() + Offset, &Value, sizeof(float));
				}
			}
		}
	}

	template <typename F>
	static void FillVector(const FFlowVizVolumeLayout& Layout, TArray<uint8>& Bytes, F&& ValueAt)
	{
		for (int32 K = 0; K < Extent.Z; ++K)
		{
			for (int32 J = 0; J < Extent.Y; ++J)
			{
				for (int32 I = 0; I < Extent.X; ++I)
				{
					const FVector3f Value = ValueAt(I, J, K);
					const FVector4f Raw(Value.X, Value.Y, Value.Z,
						std::numeric_limits<float>::quiet_NaN());
					const int64 Offset = Layout.GetTextureVoxelOffset(I, J, K);
					FMemory::Memcpy(Bytes.GetData() + Offset, &Raw, sizeof(FVector4f));
				}
			}
		}
	}

	template <typename F>
	static void FillStatus(const FFlowVizVolumeLayout& Layout, TArray<uint8>& Bytes, F&& StatusAt)
	{
		for (int32 K = 0; K < Extent.Z; ++K)
		{
			for (int32 J = 0; J < Extent.Y; ++J)
			{
				for (int32 I = 0; I < Extent.X; ++I)
				{
					const int64 Offset = Layout.GetTextureVoxelOffset(I, J, K);
					Bytes[static_cast<int32>(Offset)] = StatusAt(I, J, K);
				}
			}
		}
	}
}

bool FFlowVizTemporalVolumeMarchTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizTemporalMarchFixture;

	if (!GIsRHIInitialized || GUsingNullRHI)
	{
		AddWarning(TEXT(
			"SKIPPED: FlowViz.Render.TemporalVolumeMarch needs a real RHI device and this run has none "
			"(-nullrhi). NOTHING about temporal pixels, nearest-frame status, vector-before-magnitude, "
			"gradient sampling, or shadow sampling was verified. Re-run with: "
			"RHI=1 Tools/build_lock.sh ./Tools/run_tests.sh FlowViz.Render.TemporalVolumeMarch"));
		return true;
	}

	TArray<FRenderSpec> Specs;
	FString SetupError;

	auto AddScalar = [&](const TCHAR* Name, auto&& A, auto&& B) -> int32
	{
		FRenderSpec Spec;
		if (!MakeSpec(Name, 1, Spec, SetupError))
		{
			return INDEX_NONE;
		}
		FillScalar(Spec.FieldLayout, Spec.FieldA, Forward<decltype(A)>(A));
		FillScalar(Spec.FieldLayout, Spec.FieldB, Forward<decltype(B)>(B));
		return Specs.Add(MoveTemp(Spec));
	};

	auto AddVector = [&](const TCHAR* Name, auto&& A, auto&& B) -> int32
	{
		FRenderSpec Spec;
		if (!MakeSpec(Name, 3, Spec, SetupError))
		{
			return INDEX_NONE;
		}
		FillVector(Spec.FieldLayout, Spec.FieldA, Forward<decltype(A)>(A));
		FillVector(Spec.FieldLayout, Spec.FieldB, Forward<decltype(B)>(B));
		Spec.ComponentMode = EFlowVizComponentMode::Magnitude;
		Spec.ValueRangeMin = 0.0f;
		Spec.ValueRangeMax = 10.0f;
		return Specs.Add(MoveTemp(Spec));
	};

	const auto Constant = [](float Value)
	{
		return [Value](int32, int32, int32) { return Value; };
	};
	const auto ConstantVector = [](const FVector3f& Value)
	{
		return [Value](int32, int32, int32) { return Value; };
	};

	const int32 AlphaZero = AddScalar(TEXT("AlphaZero"), Constant(-1000.0f), Constant(0.001f));
	const int32 AlphaOne = AddScalar(TEXT("AlphaOne"), Constant(-1000.0f), Constant(0.001f));
	const int32 Interior = AddScalar(TEXT("Interior"),
		Constant(1586.845947265625f), Constant(-5887.7373046875f));
	const int32 Identity = AddScalar(TEXT("Identity"), Constant(-37.25f), Constant(-37.25f));
	const int32 StatusBelow = AddScalar(TEXT("StatusBelow"), Constant(10.0f), Constant(20.0f));
	const int32 StatusAt = AddScalar(TEXT("StatusAt"), Constant(10.0f), Constant(20.0f));

	if (AlphaZero == INDEX_NONE || AlphaOne == INDEX_NONE || Interior == INDEX_NONE
		|| Identity == INDEX_NONE || StatusBelow == INDEX_NONE || StatusAt == INDEX_NONE)
	{
		AddError(SetupError);
		return false;
	}
	Specs[AlphaZero].bBlendActive = true;
	Specs[AlphaZero].BlendAlpha = 0.0f;
	Specs[AlphaOne].bBlendActive = true;
	Specs[AlphaOne].BlendAlpha = 1.0f;
	Specs[Interior].bBlendActive = true;
	Specs[Interior].BlendAlpha = 0.25f;
	Specs[Identity].bBlendActive = true;
	Specs[Identity].BlendAlpha = 0.375f;
	Specs[StatusBelow].bBlendActive = true;
	Specs[StatusBelow].BlendAlpha = std::nextafter(0.5f, 0.0f);
	Specs[StatusAt].bBlendActive = true;
	Specs[StatusAt].BlendAlpha = 0.5f;
	FillStatus(Specs[StatusBelow].StatusLayout, Specs[StatusBelow].StatusA,
		[](int32, int32, int32) { return FlowVizVoxelStatus::NaN; });
	FillStatus(Specs[StatusBelow].StatusLayout, Specs[StatusBelow].StatusB,
		[](int32, int32, int32) { return FlowVizVoxelStatus::Masked; });
	Specs[StatusAt].StatusA = Specs[StatusBelow].StatusA;
	Specs[StatusAt].StatusB = Specs[StatusBelow].StatusB;

	const int32 VectorA = AddVector(TEXT("VectorA"),
		ConstantVector(FVector3f(1.0f, 0.0f, 0.0f)),
		ConstantVector(FVector3f(-1.0f, 0.0f, 0.0f)));
	const int32 VectorB = AddVector(TEXT("VectorB"),
		ConstantVector(FVector3f(1.0f, 0.0f, 0.0f)),
		ConstantVector(FVector3f(-1.0f, 0.0f, 0.0f)));
	const int32 VectorBlend = AddVector(TEXT("VectorBlend"),
		ConstantVector(FVector3f(1.0f, 0.0f, 0.0f)),
		ConstantVector(FVector3f(-1.0f, 0.0f, 0.0f)));
	if (VectorA == INDEX_NONE || VectorB == INDEX_NONE || VectorBlend == INDEX_NONE)
	{
		AddError(SetupError);
		return false;
	}
	Specs[VectorA].bBlendActive = true;
	Specs[VectorA].BlendAlpha = 0.0f;
	Specs[VectorB].bBlendActive = true;
	Specs[VectorB].BlendAlpha = 1.0f;
	Specs[VectorBlend].bBlendActive = true;
	Specs[VectorBlend].BlendAlpha = 0.5f;

	const auto LinearX = [](int32 I, int32, int32) { return static_cast<float>(I); };
	const auto DoubleX = [](int32 I, int32, int32) { return 2.0f * static_cast<float>(I); };
	const auto OffsetLinearX = [](int32 I, int32, int32) { return 10.0f + static_cast<float>(I); };
	const auto OffsetDoubleX = [](int32 I, int32, int32) { return 20.0f + 2.0f * static_cast<float>(I); };
	const int32 TemporalUnlit = AddScalar(TEXT("TemporalUnlit"), OffsetLinearX, OffsetDoubleX);
	const int32 TemporalLit = AddScalar(TEXT("TemporalLit"), OffsetLinearX, OffsetDoubleX);
	if (TemporalUnlit == INDEX_NONE || TemporalLit == INDEX_NONE)
	{
		AddError(SetupError);
		return false;
	}
	for (const int32 SpecIndex : { TemporalUnlit, TemporalLit })
	{
		FRenderSpec& Spec = Specs[SpecIndex];
		Spec.bBlendActive = true;
		Spec.BlendAlpha = 0.25f;
		Spec.CompositeMode = EFlowVizCompositeMode::Alpha;
		Spec.ValueRangeMin = 0.0f;
		Spec.ValueRangeMax = 40.0f;
		Spec.OpacityMultiplier = 1.0f;
	}
	Specs[TemporalLit].bLighting = true;
	Specs[TemporalLit].LightDirection = FVector3f(0.0f, 1.0f, 0.0f);
	Specs[TemporalLit].DiffuseStrength = 0.0f;

	const auto LinearXPlusY = [](int32 I, int32 J, int32)
	{
		return static_cast<float>(I + 2 * J);
	};
	const int32 GradientA = AddScalar(TEXT("GradientA"), LinearX, LinearXPlusY);
	const int32 GradientBlend = AddScalar(TEXT("GradientBlend"), LinearX, LinearXPlusY);
	const int32 GradientB = AddScalar(TEXT("GradientB"), LinearX, LinearXPlusY);
	if (GradientA == INDEX_NONE || GradientBlend == INDEX_NONE || GradientB == INDEX_NONE)
	{
		AddError(SetupError);
		return false;
	}
	for (const int32 SpecIndex : { GradientA, GradientBlend, GradientB })
	{
		FRenderSpec& Spec = Specs[SpecIndex];
		Spec.CompositeMode = EFlowVizCompositeMode::IsoSurface;
		Spec.IsoValue = 5.0f;
		Spec.ValueRangeMin = 0.0f;
		Spec.ValueRangeMax = 14.0f;
		Spec.bLighting = true;
		Spec.LightDirection = FVector3f(0.0f, 1.0f, 0.0f);
		Spec.OpacityMultiplier = 0.0f;
	}
	Specs[GradientBlend].bBlendActive = true;
	Specs[GradientBlend].BlendAlpha = 0.5f;
	Specs[GradientB].bBlendActive = true;
	Specs[GradientB].BlendAlpha = 1.0f;

	const auto ShadowFieldA = [](int32 I, int32 J, int32)
	{
		return static_cast<float>(I + J);
	};
	const auto ShadowFieldB = [](int32 I, int32 J, int32)
	{
		return I < 3 ? 7.0f : static_cast<float>(I + J);
	};
	const int32 ShadowA = AddScalar(TEXT("ShadowA"), ShadowFieldA, ShadowFieldB);
	const int32 ShadowBlend = AddScalar(TEXT("ShadowBlend"), ShadowFieldA, ShadowFieldB);
	const int32 ShadowB = AddScalar(TEXT("ShadowB"), ShadowFieldA, ShadowFieldB);
	if (ShadowA == INDEX_NONE || ShadowBlend == INDEX_NONE || ShadowB == INDEX_NONE)
	{
		AddError(SetupError);
		return false;
	}
	for (const int32 SpecIndex : { ShadowA, ShadowBlend, ShadowB })
	{
		FRenderSpec& Spec = Specs[SpecIndex];
		Spec.CompositeMode = EFlowVizCompositeMode::IsoSurface;
		Spec.CameraForward = FVector3f(0.0f, 1.0f, 0.0f);
		Spec.IsoValue = 5.0f;
		Spec.ValueRangeMin = 0.0f;
		Spec.ValueRangeMax = 10.0f;
		Spec.bLighting = true;
		Spec.LightDirection = FVector3f(1.0f, 0.0f, 0.0f);
		Spec.OpacityMultiplier = 1.0f;
		Spec.LutKind = ELutKind::AlphaRamp;
	}
	Specs[ShadowBlend].bBlendActive = true;
	Specs[ShadowBlend].BlendAlpha = 0.5f;
	Specs[ShadowB].bBlendActive = true;
	Specs[ShadowB].BlendAlpha = 1.0f;

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	if (!TestNotNull(TEXT("the global shader map exists before temporal pixel dispatch"), ShaderMap))
	{
		return false;
	}
	for (int32 UintPermutation = 0; UintPermutation <= 1; ++UintPermutation)
	{
		FFlowVizVolumeRayMarchCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FFlowVizVolumeRayMarchCS::FFieldIsUint>(UintPermutation != 0);
		TShaderMapRef<FFlowVizVolumeRayMarchCS> ComputeShader(ShaderMap, PermutationVector);
		if (!TestTrue(*FString::Printf(
				TEXT("the temporal ray-march shader compiled for FLOWVIZ_FIELD_UINT=%d"),
				UintPermutation), ComputeShader.IsValid()))
		{
			return false;
		}
	}

	TArray<FLinearColor> LutEntries;
	LutEntries.SetNum(LutWidth);
	for (int32 Index = 0; Index < LutWidth; ++Index)
	{
		const float T = static_cast<float>(Index) / static_cast<float>(LutWidth - 1);
		LutEntries[Index] = FLinearColor(0.25f, 0.5f, 0.75f, T);
	}
	TArray<uint8> LutBytes;
	if (!TestTrue(TEXT("the temporal fixture LUT packs through the production format"),
			FlowVizTransferFunction::PackLutBytes(LutEntries, LutBytes).IsOk()))
	{
		return false;
	}

	TArray<FRenderResult> Results;
	Results.SetNum(Specs.Num());

	ENQUEUE_RENDER_COMMAND(FlowVizTemporalVolumeMarch)(
		[&](FRHICommandListImmediate& RHICmdList)
		{
			for (int32 SpecIndex = 0; SpecIndex < Specs.Num(); ++SpecIndex)
			{
				const FRenderSpec& Spec = Specs[SpecIndex];
				FRenderResult& Result = Results[SpecIndex];
				const auto CreateVolume = [&](const FFlowVizVolumeLayout& Layout,
					const TCHAR* DebugName, const TCHAR* Role, FTextureRHIRef& OutTexture)
				{
					FCFDVizResult Created = FCFDVizResult::Ok();
					OutTexture = FlowVizVolumeRHI::CreateVolumeTexture(
						RHICmdList, Layout, DebugName, Created);
					if (!OutTexture.IsValid() || !Created.IsOk())
					{
						SetupError = FString::Printf(TEXT("%s %s texture creation: %s"),
							*Spec.Name, Role, *Created.Message);
						return false;
					}
					return true;
				};

				FTextureRHIRef FieldA;
				FTextureRHIRef FieldB;
				FTextureRHIRef StatusA;
				FTextureRHIRef StatusB;
				if (!CreateVolume(Spec.FieldLayout, TEXT("FlowVizTemporalFieldA"), TEXT("field A"), FieldA)
					|| !CreateVolume(Spec.FieldLayout, TEXT("FlowVizTemporalFieldB"), TEXT("field B"), FieldB)
					|| !CreateVolume(Spec.StatusLayout, TEXT("FlowVizTemporalStatusA"), TEXT("status A"), StatusA)
					|| !CreateVolume(Spec.StatusLayout, TEXT("FlowVizTemporalStatusB"), TEXT("status B"), StatusB))
				{
					return;
				}

				const FCFDVizResult UploadedA = FlowVizVolumeRHI::UpdateVolumeTexture(
					RHICmdList, FieldA, Spec.FieldLayout, Spec.FieldA);
				const FCFDVizResult UploadedB = FlowVizVolumeRHI::UpdateVolumeTexture(
					RHICmdList, FieldB, Spec.FieldLayout, Spec.FieldB);
				const FCFDVizResult StatusUploadedA = FlowVizVolumeRHI::UpdateVolumeTexture(
					RHICmdList, StatusA, Spec.StatusLayout, Spec.StatusA);
				const FCFDVizResult StatusUploadedB = FlowVizVolumeRHI::UpdateVolumeTexture(
					RHICmdList, StatusB, Spec.StatusLayout, Spec.StatusB);
				if (!UploadedA.IsOk() || !UploadedB.IsOk()
					|| !StatusUploadedA.IsOk() || !StatusUploadedB.IsOk())
				{
					SetupError = Spec.Name + TEXT(" upload failed: ") + UploadedA.Message
						+ UploadedB.Message + StatusUploadedA.Message + StatusUploadedB.Message;
					return;
				}

				FCFDVizResult LutCreated = FCFDVizResult::Ok();
				FTextureRHIRef LutTexture = FlowVizTransferFunctionRHI::CreateLutTexture(
					RHICmdList, LutWidth, TEXT("FlowVizTemporalLut"), LutCreated);
				if (!LutTexture.IsValid() || !LutCreated.IsOk())
				{
					SetupError = Spec.Name + TEXT(" LUT creation: ") + LutCreated.Message;
					return;
				}

				TArray<uint8> SpecLutBytes = LutBytes;
				if (Spec.LutKind == ELutKind::Constant)
				{
					TArray<FLinearColor> ConstantLut;
					ConstantLut.Init(FLinearColor(0.25f, 0.5f, 0.75f, 0.25f), LutWidth);
					const FCFDVizResult Packed =
						FlowVizTransferFunction::PackLutBytes(ConstantLut, SpecLutBytes);
					if (!Packed.IsOk())
					{
						SetupError = Spec.Name + TEXT(" LUT packing: ") + Packed.Message;
						return;
					}
				}
				const FCFDVizResult LutUploaded = FlowVizTransferFunctionRHI::UpdateLutTexture(
					RHICmdList, LutTexture.GetReference(), LutWidth, SpecLutBytes);
				if (!LutUploaded.IsOk())
				{
					SetupError = Spec.Name + TEXT(" LUT upload: ") + LutUploaded.Message;
					return;
				}

				FRDGBuilder GraphBuilder(RHICmdList);
				const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
					FIntPoint(OutputW, OutputH), PF_A32B32G32R32F,
					FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV);
				FRDGTextureRef ColorTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("TemporalColor"));
				FRDGTextureRef ValueTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("TemporalValue"));

				FFlowVizVolumeRayMarchParameters* Params =
					GraphBuilder.AllocParameters<FFlowVizVolumeRayMarchParameters>();
				FlowVizRayMarch::FillDefaults(*Params);
				FlowVizRayMarch::FillFromVolumeParameters(Spec.VolumeParams, *Params);
				FlowVizRayMarch::SetLookAtCamera(*Params, Spec.CameraForward,
					FIntPoint(OutputW, OutputH), /*bOrthographic=*/true,
					/*DistanceScale=*/1.0f, /*HorizontalFovDegrees=*/60.0f);
				Params->bEnableJitter = 0;
				Params->bFilterField = 1;
				Params->CompositeMode = static_cast<uint32>(Spec.CompositeMode);
				Params->ComponentMode = static_cast<uint32>(Spec.ComponentMode);
				Params->bEnableLighting = Spec.bLighting ? 1u : 0u;
				Params->LightDirection = Spec.LightDirection;
				Params->AmbientStrength = Spec.AmbientStrength;
				Params->DiffuseStrength = Spec.DiffuseStrength;
				Params->OpacityMultiplier = Spec.OpacityMultiplier;
				Params->StepVoxels = Spec.StepVoxels;
				Params->IsoValue = Spec.IsoValue;
				Params->ValueRangeMin = Spec.ValueRangeMin;
				Params->ValueRangeMax = Spec.ValueRangeMax;
				if (!FlowVizRayMarch::SetVolumeTextures(*Params,
						FieldA, StatusA, /*bHasVectorTexture=*/false,
						FieldB, StatusB, Spec.BlendAlpha, Spec.bBlendActive))
				{
					SetupError = Spec.Name + TEXT(" SetVolumeTextures refused the fixture");
					return;
				}
				Params->TransferFunctionTexture = LutTexture;

				Result.bDispatched = FlowVizRayMarch::AddRayMarchPass(
					GraphBuilder, GMaxRHIFeatureLevel, /*bFieldIsUint=*/false,
					Params, ColorTexture, ValueTexture);
				if (!Result.bDispatched)
				{
					GraphBuilder.Execute();
					continue;
				}

				FRHIGPUTextureReadback ValueReadback(TEXT("TemporalValueReadback"));
				FRHIGPUTextureReadback ColorReadback(TEXT("TemporalColorReadback"));
				AddEnqueueCopyPass(GraphBuilder, &ValueReadback, ValueTexture);
				AddEnqueueCopyPass(GraphBuilder, &ColorReadback, ColorTexture);
				GraphBuilder.Execute();
				RHICmdList.SubmitAndBlockUntilGPUIdle();

				const auto ReadCentre = [](FRHIGPUTextureReadback& Readback, FLinearColor& OutPixel)
				{
					int32 RowPitch = 0;
					int32 BufferHeight = 0;
					void* Mapped = Readback.Lock(RowPitch, &BufferHeight);
					if (Mapped == nullptr)
					{
						return false;
					}
					const bool bContainsCentre = RowPitch > CentreX && BufferHeight > CentreY;
					if (bContainsCentre)
					{
						const FLinearColor* Pixels = static_cast<const FLinearColor*>(Mapped);
						OutPixel = Pixels[CentreY * RowPitch + CentreX];
					}
					Readback.Unlock();
					return bContainsCentre;
				};
				Result.bReadBack = ReadCentre(ValueReadback, Result.Value)
					&& ReadCentre(ColorReadback, Result.Color);
			}
		});
	FlushRenderingCommands();

	if (!SetupError.IsEmpty())
	{
		AddError(FString::Printf(TEXT("Temporal GPU fixture failed before assertions: %s"), *SetupError));
		return false;
	}
	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		if (!TestTrue(*FString::Printf(
				TEXT("temporal config '%s' dispatched; false means the shader was unavailable"),
				*Specs[Index].Name), Results[Index].bDispatched))
		{
			return false;
		}
		if (!TestTrue(*FString::Printf(TEXT("temporal config '%s' read back its centre pixel"),
				*Specs[Index].Name), Results[Index].bReadBack))
		{
			return false;
		}
	}

	auto ResultFor = [&](int32 SpecIndex) -> const FRenderResult&
	{
		return Results[SpecIndex];
	};

	/* Exact endpoint controls, including the wide-range B endpoint. */
	TestTrue(TEXT("alpha zero returns A bit-exactly"), ResultFor(AlphaZero).Value.R == -1000.0f);
	TestTrue(TEXT("alpha one returns the wide-range B endpoint bit-exactly"),
		ResultFor(AlphaOne).Value.R == 0.001f);
	TestTrue(*FString::Printf(
			TEXT("alpha 0.25 returns the two-product interior value (got %.9g)"),
			ResultFor(Interior).Value.R),
		ResultFor(Interior).Value.R == -281.79986572265625f);
	TestTrue(TEXT("B=A remains an identity under an interior alpha"),
		ResultFor(Identity).Value.R == -37.25f);

	/* Status is one nearest stored classification, with B selected at exact 0.5. */
	const FLinearColor& BelowStatus = ResultFor(StatusBelow).Value;
	const FLinearColor& AtStatus = ResultFor(StatusAt).Value;
	TestEqual(TEXT("the float immediately below 0.5 selects exactly NaN status A"),
		ReasonBits(BelowStatus), static_cast<uint32>(EFlowVizInvalidReason::NaN));
	TestEqual(TEXT("exact alpha 0.5 selects exactly masked status B, never A|B"),
		ReasonBits(AtStatus), static_cast<uint32>(EFlowVizInvalidReason::Masked));

	/* Blend raw vectors first: |0.5*(1,0,0)+0.5*(-1,0,0)| is zero, not one. */
	TestTrue(TEXT("vector A magnitude control is one"), ResultFor(VectorA).Value.R == 1.0f);
	TestTrue(TEXT("vector B magnitude control is one"), ResultFor(VectorB).Value.R == 1.0f);
	TestTrue(TEXT("opposing vectors cancel before magnitude extraction and remain a valid sample"),
		ResultFor(VectorBlend).Value.R == 0.0f
			&& ReasonBits(ResultFor(VectorBlend).Value)
				== static_cast<uint32>(EFlowVizInvalidReason::Valid)
			&& StepCount(ResultFor(VectorBlend).Value) > 0);

	/* Lighting changes colour but never the data-derived temporal scientific value. */
	const FRenderResult& Unlit = ResultFor(TemporalUnlit);
	const FRenderResult& Lit = ResultFor(TemporalLit);
	TestTrue(*FString::Printf(
			TEXT("the alpha-composited temporal ray reports the blended first sample (got %.9g)"),
			Unlit.Value.R),
		Unlit.Value.R == 12.5f);
	TestTrue(TEXT("the temporal lighting control returns finite colour"),
		FMath::IsFinite(Unlit.Color.R) && FMath::IsFinite(Unlit.Color.G)
			&& FMath::IsFinite(Unlit.Color.B) && FMath::IsFinite(Lit.Color.R)
			&& FMath::IsFinite(Lit.Color.G) && FMath::IsFinite(Lit.Color.B));
	TestTrue(TEXT("ambient-only lighting changes temporal colour"),
		Unlit.Color.R != Lit.Color.R || Unlit.Color.G != Lit.Color.G || Unlit.Color.B != Lit.Color.B);
	TestTrue(TEXT("lighting leaves temporally blended OutValue.x bit-identical"),
		Unlit.Value.R == Lit.Value.R);

	/* Gradient taps call the temporal primitive: B adds a Y gradient visible to a Y light. */
	const FRenderResult& GradAResult = ResultFor(GradientA);
	const FRenderResult& GradBlendResult = ResultFor(GradientBlend);
	const FRenderResult& GradBResult = ResultFor(GradientB);
	TestTrue(TEXT("gradient controls hit the same requested iso value"),
		GradAResult.Value.R == 5.0f && GradBlendResult.Value.R == 5.0f
			&& GradBResult.Value.R == 5.0f);
	TestTrue(TEXT("gradient controls return finite colour"),
		FMath::IsFinite(GradAResult.Color.G) && FMath::IsFinite(GradBlendResult.Color.G)
			&& FMath::IsFinite(GradBResult.Color.G));
	TestTrue(TEXT(
			"the half-step gradient is distinct from both endpoints, not frame B sampled outright"),
		GradBlendResult.Color.G > GradAResult.Color.G + 0.02f
			&& GradBlendResult.Color.G + 0.02f < GradBResult.Color.G);
	TestTrue(*FString::Printf(
			TEXT("the half-step iso hit lies between the endpoint hits "
				"(A steps %d, blend steps %d, B steps %d)"),
			StepCount(GradAResult.Value), StepCount(GradBlendResult.Value),
			StepCount(GradBResult.Value)),
		StepCount(GradBResult.Value) < StepCount(GradBlendResult.Value)
			&& StepCount(GradBlendResult.Value) < StepCount(GradAResult.Value));

	/* Shadow rays call the temporal primitive: only the remote, shadow-side B voxels differ. */
	const FRenderResult& ShadowAResult = ResultFor(ShadowA);
	const FRenderResult& ShadowBlendResult = ResultFor(ShadowBlend);
	const FRenderResult& ShadowBResult = ResultFor(ShadowB);
	TestTrue(TEXT("shadow controls retain the same primary iso hit"),
		ShadowAResult.Value.R == 5.0f && ShadowBlendResult.Value.R == 5.0f
			&& ShadowBResult.Value.R == 5.0f
			&& StepCount(ShadowAResult.Value) == StepCount(ShadowBlendResult.Value)
			&& StepCount(ShadowBlendResult.Value) == StepCount(ShadowBResult.Value));
	TestTrue(TEXT("shadow controls return finite colour"),
		FMath::IsFinite(ShadowAResult.Color.G) && FMath::IsFinite(ShadowBlendResult.Color.G)
			&& FMath::IsFinite(ShadowBResult.Color.G));
	TestTrue(*FString::Printf(
			TEXT("the half-step shadow is distinct from both endpoints, not frame B sampled "
				"outright (A %.9g, blend %.9g, B %.9g)"),
			ShadowAResult.Color.G, ShadowBlendResult.Color.G, ShadowBResult.Color.G),
		ShadowAResult.Color.G > ShadowBlendResult.Color.G + 0.01f
			&& ShadowBlendResult.Color.G > ShadowBResult.Color.G + 0.01f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
