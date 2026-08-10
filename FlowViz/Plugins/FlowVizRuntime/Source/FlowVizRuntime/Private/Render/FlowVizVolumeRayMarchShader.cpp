// Copyright FlowViz contributors. All Rights Reserved.

#include "Render/FlowVizVolumeRayMarchShader.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "PixelFormat.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"

/*
 * IMPLEMENT_GLOBAL_SHADER's third argument is the VIRTUAL path. /Plugin/FlowViz
 * is mapped to the plugin's Shaders/ directory by FFlowVizRuntimeModule::StartupModule,
 * and FlowVizRuntime loads at PostConfigInit precisely so that mapping exists
 * before any global shader is compiled (Docs/BUILD.md). A module loading later
 * makes this fail in ways that are hard to attribute.
 */
IMPLEMENT_GLOBAL_SHADER(
	FFlowVizVolumeRayMarchCS,
	"/Plugin/FlowViz/FlowVizVolumeRayMarch.usf",
	"FlowVizVolumeRayMarchCS",
	SF_Compute);

bool FFlowVizVolumeRayMarchCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	// SM5 is the floor for a 3D texture plus a UAV write, and it is what Metal
	// reports on the reference machine.
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FFlowVizVolumeRayMarchCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters,
	FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

	OutEnvironment.SetDefine(TEXT("FLOWVIZ_THREADS_X"), FlowVizRayMarch::ThreadGroupSizeX);
	OutEnvironment.SetDefine(TEXT("FLOWVIZ_THREADS_Y"), FlowVizRayMarch::ThreadGroupSizeY);

	// Fast math is allowed to assume no NaN exists, which would let a compiler
	// fold FlowVizIsNaN's self-comparison to a constant false. The entire
	// invalid-value policy would then evaluate to "everything is valid" and
	// render a plausible, wrong image (VISUAL_QA rule 4).
	OutEnvironment.CompilerFlags.Add(CFLAG_NoFastMath);
}

bool FFlowVizVolumeRayMarchCS::IsUintFieldFormat(ECFDVizDataType DataType)
{
	// FlowVizVolumeFormat::ChooseTextureFormat maps every integer type to a
	// _UINT format and never to a UNORM, so the shader must read those through
	// a Texture3D<uint4> declaration. Float16 and Float32 arrive as float.
	switch (DataType)
	{
	case ECFDVizDataType::Float16:
	case ECFDVizDataType::Float32:
		return false;
	default:
		return true;
	}
}

/* -------------------------------------------------------------------------- */
/* Defaults                                                                     */
/* -------------------------------------------------------------------------- */

void FlowVizRayMarch::FillDefaults(FFlowVizVolumeRayMarchParameters& OutParameters)
{
	// The camera and the volume rows are left alone: they come from
	// SetLookAtCamera and FillFromVolumeParameters, and inventing a default
	// geometry here would let a caller who forgot one of those still render.
	OutParameters.StepVoxels = DefaultStepVoxels;
	OutParameters.ReferenceStepVoxels = DefaultReferenceStepVoxels;
	OutParameters.OpacityMultiplier = 1.0f;
	OutParameters.IsoValue = 0.0f;
	OutParameters.EarlyTerminationAlpha = DefaultEarlyTerminationAlpha;
	OutParameters.MaxSteps = DefaultMaxSteps;

	OutParameters.CompositeMode = static_cast<uint32>(EFlowVizCompositeMode::Alpha);
	OutParameters.ComponentMode = static_cast<uint32>(EFlowVizComponentMode::Magnitude);

	// Jitter trades banding for noise, and per-ray jitter is a known cause of
	// temporal shimmer (ADR 002's open question). Off by default; a caller that
	// wants it asks for it.
	// ON since P6: grain the temporal AA resolves beats banding (the research
	// doc's "jitter always on with TAA-friendly noise"). ADR 002's shimmer
	// concern was about UNRESOLVED noise; the fixed seed keeps stills stable.
	OutParameters.bEnableJitter = 1;
	OutParameters.JitterAmount = 1.0f;
	OutParameters.JitterSeed = 0;

	// VISUAL_QA rule 1: lighting must not modulate apparent scalar value. The
	// Scientific profile therefore renders unlit by default.
	OutParameters.bEnableLighting = 0;
	OutParameters.AmbientStrength = 0.35f;
	OutParameters.DiffuseStrength = 0.65f;
	OutParameters.LightDirection = FVector3f(-0.5f, -0.6f, -0.6f).GetSafeNormal();
	OutParameters.LightPad0 = 0.0f;

	OutParameters.bFilterField = 1;
	OutParameters.bStrictStatusFilter = 0;

	OutParameters.CropBoxMin = FVector3f::ZeroVector;
	OutParameters.CropBoxMax = FVector3f(1.0f, 1.0f, 1.0f);
	OutParameters.CropPad0 = 0.0f;
	OutParameters.CropPad1 = 0.0f;

	OutParameters.NumClipPlanes = 0;
	for (int32 Index = 0; Index < MaxClipPlanes; ++Index)
	{
		OutParameters.ClipPlanes[Index] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	}

	// FIVE DISTINCT COLOURS, NONE OF THEM IN VIRIDIS. VISUAL_QA rule 4 requires
	// invalid data to be visibly invalid and never the colormap's minimum;
	// viridis runs dark blue-purple to yellow-green, so magenta, orange-red and
	// mid-grey all read as "not data" at a glance and as different causes on
	// inspection. Collapsing any two of these is the failure the rule names.
	OutParameters.NaNColor = FLinearColor(1.0f, 0.0f, 1.0f, 1.0f);       // magenta
	OutParameters.MaskedColor = FLinearColor(0.35f, 0.35f, 0.35f, 1.0f); // neutral grey
	OutParameters.NoDataColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);    // absent: transparent
	OutParameters.UnderRangeColor = FLinearColor(0.0f, 0.85f, 1.0f, 1.0f);  // cyan
	OutParameters.OverRangeColor = FLinearColor(1.0f, 0.35f, 0.0f, 1.0f);   // orange-red

	// OFF, which is the PROTECTIVE reading and matches FFlowVizTransferFunction's
	// own default. The two colours above only do their job while this is 0: with
	// it on, an out-of-range value is drawn as the colormap's end, i.e. as the
	// most extreme REAL value in the field. That is a legitimate display choice
	// and it is the user's to make, so it is never the default a caller gets by
	// forgetting to set it.
	OutParameters.bClampToRange = 0;

	OutParameters.bOrthographic = 1;
	OutParameters.RayCameraPad0 = 0.0f;
	OutParameters.RayCameraPad1 = 0.0f;
	OutParameters.RayCameraPad2 = 0.0f;
	OutParameters.RayCameraPad3 = 0.0f;

	OutParameters.FieldSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	OutParameters.TransferFunctionSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// A caller that forgot SetVolumeTextures must not render a plausible empty
	// volume, so nothing is bound here and the flags stay fail-closed.
	OutParameters.bHasStatusTexture = 0;
	OutParameters.bHasVectorTexture = 0;

	// The P6 scene-depth block. BEGIN_SHADER_PARAMETER_STRUCT zero-initialises
	// only resource members, so without these four writes a direct caller of
	// AddRayMarchPass uploads stack garbage in the value fields. Harmless while
	// bHasSceneDepth guards every read -- but bHasSceneDepth ITSELF was one of
	// the garbage fields, and this codebase has been burned by exactly this
	// class (an identity control once demanded 2.5e31 because FillDefaults
	// never wrote the field it was reading). DrainView overwrites all four when
	// real depth is bound.
	OutParameters.bHasSceneDepth = 0;
	OutParameters.DepthToSolver = 1.0f;
	OutParameters.DeviceZToViewZ = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	OutParameters.ViewRectMin = FVector2f(0.0f, 0.0f);

	// Clamped in one place so a caller cannot ask the GPU for an unbounded loop.
	OutParameters.MaxSteps = FMath::Clamp(OutParameters.MaxSteps, 1u, MaxStepsLimit);
}

void FlowVizRayMarch::FillFromVolumeParameters(
	const FFlowVizVolumeShaderParameters& Source,
	FFlowVizVolumeRayMarchParameters& OutParameters)
{
	OutParameters.GridOrigin = Source.GridOrigin;
	OutParameters.OriginNarrowingError = Source.OriginNarrowingError;
	OutParameters.PhysicalSize = Source.PhysicalSize;
	OutParameters.VoxelVolume = Source.VoxelVolume;
	OutParameters.VoxelSpacing = Source.VoxelSpacing;
	OutParameters.MinVoxelSpacing = Source.MinVoxelSpacing;
	OutParameters.InvVoxelSpacing = Source.InvVoxelSpacing;
	OutParameters.MaxVoxelSpacing = Source.MaxVoxelSpacing;
	OutParameters.VolumeDimensions = Source.VolumeDimensions;
	OutParameters.ComponentCount = Source.ComponentCount;
	OutParameters.InvVolumeDimensions = Source.InvVolumeDimensions;
	OutParameters.TextureComponentCount = Source.TextureComponentCount;
	OutParameters.UVWScale = Source.UVWScale;
	OutParameters.AssociationCode = Source.AssociationCode;
	OutParameters.UVWBias = Source.UVWBias;
	OutParameters.DataTypeCode = Source.DataTypeCode;
	OutParameters.RequiredStatusMask = Source.RequiredStatusMask;
	OutParameters.InvalidStatusMask = Source.InvalidStatusMask;
	OutParameters.ValueRangeMin = Source.ValueRangeMin;
	OutParameters.ValueRangeMax = Source.ValueRangeMax;
	OutParameters.bRejectNonFinite = Source.bRejectNonFinite;
	OutParameters.PadElementBits = Source.PadElementBits;

	// bHasStatusTexture and bHasVectorTexture are NOT copied. Source is a pure
	// function of a layout and always writes 0 for both, which means "reject
	// every voxel"; only SetVolumeTextures knows what is really bound. Copying
	// the placeholder here would launder it into a claim about bindings.
}

bool FlowVizRayMarch::SetVolumeTextures(
	FFlowVizVolumeRayMarchParameters& OutParameters,
	FRHITexture* FieldTexture,
	FRHITexture* StatusTexture,
	bool bHasVectorTexture)
{
	OutParameters.FieldTexture = FieldTexture;
	OutParameters.StatusTexture = StatusTexture;
	OutParameters.bHasVectorTexture = bHasVectorTexture ? 1u : 0u;

	// FAIL CLOSED, AND FROM THE RESOURCE ITSELF. The flag cannot disagree with
	// what is bound, because it is derived from what is bound. A null status
	// texture leaves it 0, and the shader reads 0 as "reject every voxel" -
	// see the member comment in FlowVizVolumeTexture.h. Reading it the other way
	// renders a complete, plausible image, which is why it is asserted rather
	// than reviewed.
	OutParameters.bHasStatusTexture = (StatusTexture != nullptr) ? 1u : 0u;

	return FieldTexture != nullptr;
}

void FlowVizRayMarch::SetLookAtCamera(
	FFlowVizVolumeRayMarchParameters& OutParameters,
	const FVector3f& Direction,
	const FIntPoint& OutputSize,
	bool bOrthographic,
	float DistanceScale,
	float HorizontalFovDegrees)
{
	const FVector3f Size = OutParameters.PhysicalSize;
	const FVector3f Center = Size * 0.5f;
	const float Radius = FMath::Max(Size.Size() * 0.5f, UE_SMALL_NUMBER);

	FVector3f Forward = Direction.GetSafeNormal();
	if (Forward.IsNearlyZero())
	{
		Forward = FVector3f(0.0f, 0.0f, -1.0f);
	}

	// Any up vector not parallel to Forward will do; the choice only rotates the
	// image about the view axis.
	FVector3f Up = FVector3f(0.0f, 0.0f, 1.0f);
	if (FMath::Abs(FVector3f::DotProduct(Up, Forward)) > 0.99f)
	{
		Up = FVector3f(0.0f, 1.0f, 0.0f);
	}
	const FVector3f Right = FVector3f::CrossProduct(Forward, Up).GetSafeNormal();
	Up = FVector3f::CrossProduct(Right, Forward).GetSafeNormal();

	const float Distance = Radius * FMath::Max(DistanceScale, 1.0f);

	OutParameters.RayCameraOrigin = Center - Forward * Distance;
	OutParameters.RayCameraForward = Forward;
	OutParameters.RayCameraRight = Right;
	OutParameters.RayCameraUp = Up;
	OutParameters.bOrthographic = bOrthographic ? 1u : 0u;

	OutParameters.OutputSizeX = static_cast<uint32>(FMath::Max(OutputSize.X, 1));
	OutParameters.OutputSizeY = static_cast<uint32>(FMath::Max(OutputSize.Y, 1));

	const float AspectRatio =
		static_cast<float>(FMath::Max(OutputSize.X, 1)) / static_cast<float>(FMath::Max(OutputSize.Y, 1));

	// The ortho frustum frames the whole bounding sphere, so the volume is fully
	// visible from any direction without per-direction tuning.
	OutParameters.OrthoHalfExtent = FVector2f(Radius * AspectRatio, Radius);

	const float TanHalfX = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(HorizontalFovDegrees, 1.0f, 179.0f)) * 0.5f);
	OutParameters.TanHalfFov = FVector2f(TanHalfX, TanHalfX / FMath::Max(AspectRatio, UE_SMALL_NUMBER));
}

FIntVector FlowVizRayMarch::GetGroupCount(const FIntPoint& OutputSize)
{
	return FIntVector(
		FMath::DivideAndRoundUp(FMath::Max(OutputSize.X, 1), ThreadGroupSizeX),
		FMath::DivideAndRoundUp(FMath::Max(OutputSize.Y, 1), ThreadGroupSizeY),
		1);
}

bool FlowVizRayMarch::AddRayMarchPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	bool bFieldIsUint,
	FFlowVizVolumeRayMarchParameters* Parameters,
	FRDGTextureRef OutColorTexture,
	FRDGTextureRef OutValueTexture)
{
	if (Parameters == nullptr || OutColorTexture == nullptr || OutValueTexture == nullptr)
	{
		return false;
	}

	/*
	 * THE DEPTH FALLBACK LIVES HERE, NOT IN DrainView (P6 crash fix). Every
	 * declared RDG texture must be bound; DrainView supplies real depth or a
	 * dummy, but the DEVICE TEST calls this function directly and its null
	 * SceneDepthTexture executed as a render-thread SIGSEGV. The pass is the
	 * one place every caller flows through, so the guarantee belongs here.
	 */
	if (Parameters->SceneDepthTexture == nullptr)
	{
		// UAV + a clear pass: RDG validation rejects reading a texture nothing
		// wrote ("read dependency ... but it was never written to"), so the
		// dummy is cleared -- one GPU write of four bytes, once per pass.
		FRDGTextureRef DummyDepth = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_R32_FLOAT,
				FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("FlowVizVolumeRayMarch.DummyDepth"));
		AddClearUAVPass(GraphBuilder,
			GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DummyDepth)),
			FVector4(0.0, 0.0, 0.0, 0.0));
		Parameters->SceneDepthTexture = DummyDepth;
		Parameters->bHasSceneDepth = 0;
	}

	FFlowVizVolumeRayMarchCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FFlowVizVolumeRayMarchCS::FFieldIsUint>(bFieldIsUint);

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);
	if (ShaderMap == nullptr)
	{
		return false;
	}

	TShaderMapRef<FFlowVizVolumeRayMarchCS> ComputeShader(ShaderMap, PermutationVector);
	if (!ComputeShader.IsValid())
	{
		// This is what a shader that failed to COMPILE looks like at runtime -
		// ADR 002 accepts that cost. Refusing to add the pass leaves the targets
		// untouched, so the caller sees "nothing was rendered" rather than an
		// image it might mistake for data.
		return false;
	}

	Parameters->OutColor = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutColorTexture));
	Parameters->OutValue = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutValueTexture));

	const FIntPoint OutputSize(
		static_cast<int32>(Parameters->OutputSizeX),
		static_cast<int32>(Parameters->OutputSizeY));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FlowVizVolumeRayMarch %dx%d", OutputSize.X, OutputSize.Y),
		ComputeShader,
		Parameters,
		GetGroupCount(OutputSize));

	return true;
}
