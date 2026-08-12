// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "RHIFwd.h"
#include "RenderGraphFwd.h"
#include "RenderGraphResources.h"
#include "ShaderParameterMacros.h"
#include "ShaderParameterStruct.h"

#include "Render/FlowVizVolumeTexture.h"

/**
 * The GPU consumer of the volume data path (ADR 002 Candidate C, plan.md
 * section 9). Shaders/FlowVizVolumeRayMarch.usf is the shader; this is the
 * declaration that binds it and the parameter block that feeds it.
 *
 * WHY A COMPUTE SHADER AND NOT A PIXEL SHADER. The pass writes two targets: an
 * image and a per-pixel quantitative record (OutValue). The second is what makes
 * the Scientific profile testable at all -- a render whose only output is colour
 * can be checked for "looks plausible" and nothing else, and plan.md section 4
 * forbids a path whose fidelity cannot be stated. A compute pass writes both to
 * UAVs with no render-target plumbing and runs identically off-screen, which is
 * what an automation test has.
 *
 * WHAT OutValue CARRIES, per pixel:
 *
 *   .x  the scalar the ray resolved to, BEFORE any lighting term
 *   .y  accumulated alpha
 *   .z  EFlowVizInvalidReason bits, OR-ed over the ray
 *   .w  steps the ray actually took
 *
 * Three properties of this renderer are asserted through it rather than by eye:
 *
 *   1. Lighting does not modulate apparent scalar value (VISUAL_QA rule 1).
 *      OutValue.x is written from the unlit sample, so a lit and an unlit render
 *      of the same frame must agree BIT FOR BIT in that channel.
 *   2. The step is scaled by MinVoxelSpacing, not by any other axis. OutValue.w
 *      is proportional to 1/MinVoxelSpacing, and on the mock domain the min and
 *      max spacings differ by 2.25x, so the wrong choice changes the number.
 *   3. Invalid voxels are refused for a NAMED reason. OutValue.z distinguishes
 *      NaN from masked from absent, so "this pixel is not data" and "this pixel
 *      is not data because the mask rejected it" are separate assertions.
 *
 * THE FAIL-CLOSED DEFAULT. FFlowVizVolumeShaderParameters::bHasStatusTexture is
 * 0 when no status texture is bound, and 0 means REJECT EVERY VOXEL -- not
 * "everything is valid" (see that member's comment). MakeShaderParameters
 * correctly leaves it 0; it is the BINDING layer, here, that must raise it, and
 * only when a texture is actually bound. FillFromVolumeParameters does that from
 * the bound resource, not from a caller-supplied flag, so the two cannot
 * disagree. Reading the flag backwards renders a complete, plausible image,
 * which is why FlowVizVolumeRayMarchTest asserts it in the rejecting direction.
 */

/** Compositing mode. Values must match the FLOWVIZ_MODE_* defines in the .usf. */
enum class EFlowVizCompositeMode : uint32
{
	/** Front-to-back alpha compositing. The default for a volume render. */
	Alpha = 0,
	/** Maximum-intensity projection. */
	Maximum = 1,
	/** Minimum-intensity projection. */
	Minimum = 2,
	/** Mean of the valid samples along the ray. Invalid samples are excluded from BOTH the sum and the count. */
	Average = 3,
	/** Ray-marched iso-surface, with a linear crossing between bracketing samples. */
	IsoSurface = 4,
	/**
	 * Echo the constant buffer back through the GPU, one row per pixel of scanline 0.
	 *
	 * This is the cbuffer drift detector. The static_asserts in
	 * FlowVizVolumeTexture.h pin the C++ offsets against each other, and the
	 * ones at the bottom of this file pin the SHADER_PARAMETER block against
	 * them - but only a round trip proves the shader reads the bytes the CPU
	 * wrote. A member in the wrong slot renders a plausible volume and fails
	 * here with a number.
	 */
	Diagnostic = 5,
};

/** Which component of the field is coloured. Must match the .usf. */
enum class EFlowVizComponentMode : uint32
{
	X = 0,
	Y = 1,
	Z = 2,
	W = 3,
	/** sqrt of the sum of squares over ComponentCount components - never over the pad channel. */
	Magnitude = 4,
};

/**
 * Why a sample was refused, or how it fell outside the transfer function.
 * OR-ed along the ray and reported in OutValue.z.
 *
 * NONE and UNKNOWN are different answers. NONE means the ray never entered the
 * volume. UNKNOWN means it did and found status 0 - "nothing was ever written
 * here" - which is the fail-closed reading of an absent status texture.
 */
enum class EFlowVizInvalidReason : uint32
{
	None = 0u,
	Valid = 1u << 0,
	Unknown = 1u << 1,
	Masked = 1u << 2,
	NaN = 1u << 3,
	Infinite = 1u << 4,
	UnderRange = 1u << 5,
	OverRange = 1u << 6,
};
ENUM_CLASS_FLAGS(EFlowVizInvalidReason);

namespace FlowVizRayMarch
{
	/** Compute threadgroup size. Mirrored into the shader environment as FLOWVIZ_THREADS_X/Y. */
	inline constexpr int32 ThreadGroupSizeX = 8;
	inline constexpr int32 ThreadGroupSizeY = 8;

	/** Clipping planes the shader declares. Matches FLOWVIZ_MAX_CLIP_PLANES. */
	inline constexpr int32 MaxClipPlanes = 6;

	/**
	 * Every bit EFlowVizInvalidReason defines, OR-ed together.
	 *
	 * A C++ enum is not enumerable, so a test that pins each enumerator against
	 * the .usf cannot notice a NINTH one being added - it would simply never look
	 * at it, and the new bit would reach OutValue.z with no #define behind it.
	 * This is the enumerable form: FlowViz.Render.ReasonCodes compares it against
	 * the union of the bits it pins, so an enumerator added here without a
	 * matching FLOWVIZ_REASON_* fails that test. Add a bit, add it here.
	 */
	inline constexpr EFlowVizInvalidReason KnownInvalidReasons =
		EFlowVizInvalidReason::Valid | EFlowVizInvalidReason::Unknown
		| EFlowVizInvalidReason::Masked | EFlowVizInvalidReason::NaN
		| EFlowVizInvalidReason::Infinite | EFlowVizInvalidReason::UnderRange
		| EFlowVizInvalidReason::OverRange;

	/**
	 * Default sample step, in VOXEL units.
	 *
	 * Half a voxel is Nyquist for a trilinearly filtered field: one sample per
	 * voxel aliases visibly on a thin feature. It is in voxel units - not
	 * physical units - so the same number is correct for a millimetre case and a
	 * kilometre one, and the shader multiplies it by MinVoxelSpacing.
	 */
	inline constexpr float DefaultStepVoxels = 0.5f;

	/**
	 * The step the opacity is normalised to.
	 *
	 * Front-to-back alpha depends on how often it is sampled, so without this
	 * correction halving the step doubles the apparent density and the image
	 * changes with a performance setting. Correcting to a fixed reference is
	 * what makes the render a function of the data rather than of the budget.
	 */
	inline constexpr float DefaultReferenceStepVoxels = 0.5f;

	/** Alpha at which a ray stops. Below 1 so a saturated ray costs nothing further. */
	inline constexpr float DefaultEarlyTerminationAlpha = 0.99f;

	/** Steps a ray may take before it gives up. Bounded so a degenerate camera cannot hang the GPU. */
	inline constexpr uint32 DefaultMaxSteps = 2048u;

	/** Ceiling on MaxSteps, enforced by FillDefaults. A caller cannot ask for an unbounded loop. */
	inline constexpr uint32 MaxStepsLimit = 8192u;
}

/**
 * Everything the ray-marcher needs.
 *
 * THE FIRST TEN ROWS ARE FFlowVizVolumeShaderParameters, MEMBER FOR MEMBER, IN
 * ORDER. They are spelled out rather than nested because SHADER_PARAMETER_STRUCT
 * would prefix every name in the generated HLSL and the .usf reads them at
 * global scope. The static_asserts at the bottom of this file pin each one
 * against the offset FlowVizVolumeTexture.h already pins, so a member added,
 * removed or reordered on either side fails to compile instead of rendering a
 * volume with spacing read out of the dimensions slot.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFlowVizVolumeRayMarchParameters, FLOWVIZRUNTIME_API)
	/* -- FFlowVizVolumeShaderParameters, rows 0..9 ------------------------- */
	SHADER_PARAMETER(FVector3f, GridOrigin)
	SHADER_PARAMETER(float, OriginNarrowingError)
	SHADER_PARAMETER(FVector3f, PhysicalSize)
	SHADER_PARAMETER(float, VoxelVolume)
	SHADER_PARAMETER(FVector3f, VoxelSpacing)
	SHADER_PARAMETER(float, MinVoxelSpacing)
	SHADER_PARAMETER(FVector3f, InvVoxelSpacing)
	SHADER_PARAMETER(float, MaxVoxelSpacing)
	SHADER_PARAMETER(FIntVector, VolumeDimensions)
	SHADER_PARAMETER(int32, ComponentCount)
	SHADER_PARAMETER(FVector3f, InvVolumeDimensions)
	SHADER_PARAMETER(int32, TextureComponentCount)
	SHADER_PARAMETER(FVector3f, UVWScale)
	SHADER_PARAMETER(uint32, AssociationCode)
	SHADER_PARAMETER(FVector3f, UVWBias)
	SHADER_PARAMETER(uint32, DataTypeCode)
	SHADER_PARAMETER(uint32, RequiredStatusMask)
	SHADER_PARAMETER(uint32, InvalidStatusMask)
	SHADER_PARAMETER(uint32, bHasStatusTexture)
	SHADER_PARAMETER(uint32, bHasVectorTexture)
	SHADER_PARAMETER(float, ValueRangeMin)
	SHADER_PARAMETER(float, ValueRangeMax)
	SHADER_PARAMETER(uint32, bRejectNonFinite)
	SHADER_PARAMETER(uint32, PadElementBits)

	/* -- Camera, in LOCAL volume space ------------------------------------ */
	/* World placement is the proxy's double-precision LocalToWorld and never
	   reaches the GPU; a kilometre-scale case would lose metres to float32. */
	SHADER_PARAMETER(FVector3f, RayCameraOrigin)
	SHADER_PARAMETER(float, RayCameraPad0)
	SHADER_PARAMETER(FVector3f, RayCameraForward)
	SHADER_PARAMETER(float, RayCameraPad1)
	SHADER_PARAMETER(FVector3f, RayCameraRight)
	SHADER_PARAMETER(float, RayCameraPad2)
	SHADER_PARAMETER(FVector3f, RayCameraUp)
	SHADER_PARAMETER(float, RayCameraPad3)
	/** tan(HalfFovX), tan(HalfFovY). Perspective only. */
	SHADER_PARAMETER(FVector2f, TanHalfFov)
	/** Half width and height of the ortho frustum, in solver units. Orthographic only. */
	SHADER_PARAMETER(FVector2f, OrthoHalfExtent)
	SHADER_PARAMETER(uint32, bOrthographic)
	SHADER_PARAMETER(uint32, OutputSizeX)
	SHADER_PARAMETER(uint32, OutputSizeY)
	SHADER_PARAMETER(uint32, JitterSeed)

	/* -- Marching --------------------------------------------------------- */
	/** Sample step in VOXEL units. The shader multiplies by MinVoxelSpacing. */
	SHADER_PARAMETER(float, StepVoxels)
	SHADER_PARAMETER(float, OpacityMultiplier)
	SHADER_PARAMETER(float, IsoValue)
	SHADER_PARAMETER(float, EarlyTerminationAlpha)
	/** Sub-step jitter, as a fraction of one step. Never changes the step COUNT. */
	SHADER_PARAMETER(float, JitterAmount)
	SHADER_PARAMETER(float, AmbientStrength)
	SHADER_PARAMETER(float, DiffuseStrength)
	SHADER_PARAMETER(float, ReferenceStepVoxels)
	SHADER_PARAMETER(uint32, MaxSteps)
	SHADER_PARAMETER(uint32, CompositeMode)
	SHADER_PARAMETER(uint32, ComponentMode)
	SHADER_PARAMETER(uint32, bEnableJitter)
	SHADER_PARAMETER(uint32, bEnableLighting)
	SHADER_PARAMETER(uint32, bFilterField)
	/** Reject a filtered sample whose trilinear footprint touches an invalid voxel. */
	SHADER_PARAMETER(uint32, bStrictStatusFilter)
	SHADER_PARAMETER(uint32, NumClipPlanes)

	/* -- Cropping, clipping, lighting ------------------------------------- */
	/** Normalised fractions of PhysicalSize, so a crop survives a spacing change. */
	SHADER_PARAMETER(FVector3f, CropBoxMin)
	SHADER_PARAMETER(float, CropPad0)
	SHADER_PARAMETER(FVector3f, CropBoxMax)
	SHADER_PARAMETER(float, CropPad1)
	SHADER_PARAMETER(FVector3f, LightDirection)
	SHADER_PARAMETER(float, LightPad0)
	/** dot(N, LocalPos) + D >= 0 is kept. Local space, solver units. */
	SHADER_PARAMETER_ARRAY(FVector4f, ClipPlanes, [FlowVizRayMarch::MaxClipPlanes])

	/* -- Invalid-value colours (plan.md 10.3, VISUAL_QA rule 4) ----------- */
	/* Distinct on purpose. Collapsing them is the failure rule 4 forbids. */
	SHADER_PARAMETER(FLinearColor, NaNColor)
	SHADER_PARAMETER(FLinearColor, MaskedColor)
	SHADER_PARAMETER(FLinearColor, NoDataColor)
	SHADER_PARAMETER(FLinearColor, UnderRangeColor)
	SHADER_PARAMETER(FLinearColor, OverRangeColor)

	/**
	 * Draw the colormap's END instead of the two range colours above.
	 *
	 * THE POLICY FOR THE TWO COLOURS DIRECTLY ABOVE, WHICH IS WHY IT LIVES HERE
	 * AND NOT IN THE MARCHING BLOCK. 0 - the default - is the protective reading
	 * of VISUAL_QA rule 4: an out-of-range value gets a flag colour, because
	 * drawing the colormap minimum would present it as the smallest real value in
	 * the field. 1 is the user's explicit opt-out, and it changes COLOUR ONLY.
	 *
	 * IT MUST NOT REACH THE CLASSIFICATION. OutValue.x and the FLOWVIZ_REASON_*
	 * bits are identical either way - see FlowVizTransferFunction.cpp:333, which
	 * states the same rule for the CPU path. A flag that also moved the reported
	 * number would turn a display choice into a quantitative lie, which is the
	 * exact failure the flag colours exist to prevent. FlowViz.Render.VolumeMarch
	 * asserts the invariance next to the colour difference, so "clamping did
	 * something" and "clamping did only what it is allowed to do" are separate
	 * assertions.
	 */
	SHADER_PARAMETER(uint32, bClampToRange)

	/* -- Resources -------------------------------------------------------- */
	SHADER_PARAMETER_TEXTURE(Texture3D, FieldTexture)
	SHADER_PARAMETER_TEXTURE(Texture3D<uint4>, StatusTexture)
	SHADER_PARAMETER_TEXTURE(Texture2D, TransferFunctionTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, FieldSampler)
	SHADER_PARAMETER_SAMPLER(SamplerState, TransferFunctionSampler)
	/*
	 * Opaque scene depth (renderer overhaul P6). When bHasSceneDepth, each
	 * ray's TMax is clamped at the opaque surface along it, so the volume
	 * composites BEHIND the obstacle/cut-plane/iso meshes. DepthToSolver is
	 * the scale from device-Z-derived view depth (Unreal units) to solver
	 * units along the ray.
	 */
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
	SHADER_PARAMETER(uint32, bHasSceneDepth)
	SHADER_PARAMETER(float, DepthToSolver)
	SHADER_PARAMETER(FVector4f, DeviceZToViewZ)
	SHADER_PARAMETER(FVector2f, ViewRectMin)

	/* -- Temporal field sampling, appended after the existing tail -------- */
	/** Raw A/B field interpolation alpha. Meaningful only while bBlendActive is 1. */
	SHADER_PARAMETER(float, BlendAlpha)
	/** One only when distinct, compatible A/B field and status resources are bound. */
	SHADER_PARAMETER(uint32, bBlendActive)

	/* Resource members live after the appended constants so they cannot move the
	 * pre-existing scene-depth value tail in the generated parameter layout. */
	SHADER_PARAMETER_TEXTURE(Texture3D, FieldTextureB)
	SHADER_PARAMETER_TEXTURE(Texture3D<uint4>, StatusTextureB)

	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutColor)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutValue)
END_SHADER_PARAMETER_STRUCT()

/**
 * The ray-marching compute shader.
 *
 * SHADER COMPILE ERRORS SURFACE AT RUNTIME, NOT AT BUILD TIME. ADR 002 names
 * that as the accepted cost of a hand-written global shader. A shader that
 * failed to compile renders nothing, which looks exactly like "the feature is
 * not wired up" - so FlowVizVolumeRayMarchTest asserts the shader is FOUND in
 * the global shader map before it asserts anything about pixels, and reports the
 * two failures differently.
 */
class FLOWVIZRUNTIME_API FFlowVizVolumeRayMarchCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFlowVizVolumeRayMarchCS);
	SHADER_USE_PARAMETER_STRUCT(FFlowVizVolumeRayMarchCS, FGlobalShader);

	using FParameters = FFlowVizVolumeRayMarchParameters;

	/**
	 * An integer field texture needs Texture3D<uint4>, a float one Texture3D<float4>,
	 * and the two cannot share a declaration. This is the permutation, not a
	 * runtime branch: a uint texture read through a float declaration is
	 * undefined on Metal and reads as noise, not as an error.
	 */
	class FFieldIsUint : SHADER_PERMUTATION_BOOL("FLOWVIZ_FIELD_UINT");
	using FPermutationDomain = TShaderPermutationDomain<FFieldIsUint>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment);

	/** True when this data type arrives in the shader as an integer texture. */
	static bool IsUintFieldFormat(ECFDVizDataType DataType);
};

namespace FlowVizRayMarch
{
	/**
	 * Set every field to a defensible Scientific-profile value.
	 *
	 * Specifically: lighting OFF (VISUAL_QA rule 1), jitter off, alpha
	 * compositing, no clip planes, the full crop box, and the five invalid
	 * colours set to five DISTINCT, saturated, out-of-colormap colours. Viridis
	 * spans dark blue-purple to yellow-green, so magenta, orange-red, grey and
	 * the two range colours cannot be mistaken for data.
	 *
	 * Resources are NOT set here - a defaults call that quietly bound a black
	 * texture would make "nothing is bound" render as a plausible empty volume.
	 */
	FLOWVIZRUNTIME_API void FillDefaults(FFlowVizVolumeRayMarchParameters& OutParameters);

	/**
	 * Copy the ten pinned rows out of the data path's block.
	 *
	 * bHasStatusTexture and bHasVectorTexture are DELIBERATELY NOT copied.
	 * MakeShaderParameters is a pure function of a layout and cannot know what
	 * is bound, so it writes 0 for both, and 0 means "reject every voxel". Only
	 * the binding layer knows, so SetVolumeTextures sets them from the resources
	 * it actually binds. Copying them here would launder a placeholder into a
	 * claim.
	 */
	FLOWVIZRUNTIME_API void FillFromVolumeParameters(
		const FFlowVizVolumeShaderParameters& Source,
		FFlowVizVolumeRayMarchParameters& OutParameters);

	/**
	 * Bind one display frame and mirror it into the always-bound B resource slots.
	 *
	 * @param StatusTexture Null is legal and sets bHasStatusTexture = 0, which
	 *                      makes the shader reject every voxel. A global uint
	 *                      volume fallback is still bound so shader validation
	 *                      never observes a null declared resource.
	 * @return false when FieldTexture is null - there is nothing to march.
	 */
	FLOWVIZRUNTIME_API bool SetVolumeTextures(
		FFlowVizVolumeRayMarchParameters& OutParameters,
		FRHITexture* FieldTexture,
		FRHITexture* StatusTexture,
		bool bHasVectorTexture = false,
		FRHITexture* FieldTextureB = nullptr,
		FRHITexture* StatusTextureB = nullptr,
		float BlendAlpha = 0.0f,
		bool bBlendActive = false);

	/**
	 * Place a camera looking at the volume's centre from a direction, in local space.
	 *
	 * @param Direction   From the eye toward the volume. Normalised internally.
	 * @param DistanceScale Multiplier on the domain's bounding radius.
	 * @param bOrthographic Orthographic frames the whole domain; perspective uses HorizontalFovDegrees.
	 */
	FLOWVIZRUNTIME_API void SetLookAtCamera(
		FFlowVizVolumeRayMarchParameters& OutParameters,
		const FVector3f& Direction,
		const FIntPoint& OutputSize,
		bool bOrthographic = true,
		float DistanceScale = 2.0f,
		float HorizontalFovDegrees = 60.0f);

	/** Threadgroups for an output of this size. */
	FLOWVIZRUNTIME_API FIntVector GetGroupCount(const FIntPoint& OutputSize);

	/**
	 * Add the ray-march pass to a render graph.
	 *
	 * @param OutColorTexture,OutValueTexture Must be PF_A32B32G32R32F and UAV
	 *        capable. Float32 is not a luxury: OutValue.x carries a field value
	 *        in solver units, which an 8- or 16-bit target would quantise -
	 *        exactly the silent quantisation plan.md section 4 rule 5 forbids.
	 * @return false when the shader is not in the global shader map, which is
	 *         what a shader that failed to compile looks like. Nothing is added
	 *         in that case; the caller must not treat an empty target as data.
	 */
	FLOWVIZRUNTIME_API bool AddRayMarchPass(
		FRDGBuilder& GraphBuilder,
		ERHIFeatureLevel::Type FeatureLevel,
		bool bFieldIsUint,
		FFlowVizVolumeRayMarchParameters* Parameters,
		FRDGTextureRef OutColorTexture,
		FRDGTextureRef OutValueTexture);

	/** Decode OutValue.z back to reason flags. */
	inline EFlowVizInvalidReason DecodeReason(float Encoded)
	{
		return static_cast<EFlowVizInvalidReason>(static_cast<uint32>(Encoded + 0.5f));
	}

	/**
	 * DeviceZ -> view-space depth, from the view's InvDeviceZToWorldZTransform.
	 *
	 * THE ENGINE'S EXACT FORMULA (ConvertFromDeviceZ, Common.ush:1376), and the
	 * C++ TWIN of the scene-depth conversion in FlowVizVolumeRayMarch.usf (the
	 * P6 clamp). It is BRANCHLESS ON PURPOSE, and any "simplification" into a
	 * perspective/ortho branch selected by T.w is the bug this replaced:
	 *
	 *   - perspective packs T = (0, 0, 1/B, A/B - fudge): the linear terms
	 *     vanish and the reciprocal term is 1/(DeviceZ*T.z - T.w) -- note
	 *     MINUS T.w. Writing + T.w flips the sign, and for a background pixel
	 *     (DeviceZ = 0 under reversed Z) the fudge-factor-sized T.w turns a
	 *     huge POSITIVE far depth into a huge NEGATIVE one, which clamps
	 *     TMax below TMin and vanishes the volume wherever nothing opaque
	 *     is behind it.
	 *   - ortho packs T = (1/A, -B/A + 1, 0, 1): the +1 folded into T.y and
	 *     the 1/(0 - 1) = -1 of the reciprocal term cancel, leaving the linear
	 *     mapping. Because T.w is 1 here, a branch on "T.w != 0 means
	 *     perspective" selects the wrong formula for EVERY ortho view and
	 *     returns a constant.
	 *
	 * The .usf cannot call this function and this function cannot call the
	 * .usf, so the two are kept in sync BY TEST: FlowViz.Render.DepthMath
	 * drives this twin with the InvDeviceZToWorldZTransform vectors real
	 * FSceneViews produce -- perspective and orthographic -- and checks known
	 * depths round-trip through the projection. Any edit to the shader's
	 * conversion must be mirrored here, where the maths is testable headless.
	 */
	inline float ConvertDeviceZToViewZ(const FVector4f& InvDeviceZToWorldZ, float DeviceZ)
	{
		const FVector4f& T = InvDeviceZToWorldZ;
		return DeviceZ * T.X + T.Y + 1.0f / (DeviceZ * T.Z - T.W);
	}
}

/* -------------------------------------------------------------------------- */
/* Constant-buffer layout assertions                                           */
/* -------------------------------------------------------------------------- */

/*
 * THE POINT OF THIS BLOCK. FlowVizVolumeTexture.h pins FFlowVizVolumeShaderParameters
 * at ten 16-byte rows. These pin the SHADER_PARAMETER block's first ten rows to
 * THE SAME OFFSETS, so the two structs cannot drift apart silently.
 *
 * A drift is not a crash and not a black screen. It is a shader reading spacing
 * out of the dimensions slot: a volume with plausible shape and wrong geometry,
 * which passes a visual review. The failure mode is precisely why the producing
 * struct is asserted, and an asserted producer feeding an unasserted consumer
 * buys nothing.
 *
 * Offsets are spelled as literals, not as offsetof of the other struct, so a
 * coordinated wrong edit to both files still fails.
 */
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, GridOrigin) == 0, "cbuffer row 0");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, OriginNarrowingError) == 12, "cbuffer row 0");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, PhysicalSize) == 16, "cbuffer row 1");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, VoxelVolume) == 28, "cbuffer row 1");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, VoxelSpacing) == 32, "cbuffer row 2");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, MinVoxelSpacing) == 44, "cbuffer row 2");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, InvVoxelSpacing) == 48, "cbuffer row 3");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, MaxVoxelSpacing) == 60, "cbuffer row 3");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, VolumeDimensions) == 64, "cbuffer row 4");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, ComponentCount) == 76, "cbuffer row 4");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, InvVolumeDimensions) == 80, "cbuffer row 5");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, TextureComponentCount) == 92, "cbuffer row 5");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, UVWScale) == 96, "cbuffer row 6");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, AssociationCode) == 108, "cbuffer row 6");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, UVWBias) == 112, "cbuffer row 7");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, DataTypeCode) == 124, "cbuffer row 7");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, RequiredStatusMask) == 128, "cbuffer row 8");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, InvalidStatusMask) == 132, "cbuffer row 8");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, bHasStatusTexture) == 136, "cbuffer row 8");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, bHasVectorTexture) == 140, "cbuffer row 8");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, ValueRangeMin) == 144, "cbuffer row 9");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, ValueRangeMax) == 148, "cbuffer row 9");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, bRejectNonFinite) == 152, "cbuffer row 9");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, PadElementBits) == 156, "cbuffer row 9");

/*
 * And the two structs agree on the SIZE of the shared prefix. Without this an
 * added member at the end of FFlowVizVolumeShaderParameters - past the last
 * pinned offset - would be missed by every assertion above.
 */
static_assert(
	STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, RayCameraOrigin) == sizeof(FFlowVizVolumeShaderParameters),
	"The ray-march parameters must begin their own members exactly where "
	"FFlowVizVolumeShaderParameters ends. A member added to either struct "
	"without updating the other lands here.");

/*
 * THE TAIL OF THE BLOCK, pinned for the reason the prefix is.
 *
 * The asserts above stop at offset 156 because that is where the SHARED prefix
 * ends - they exist to keep two structs from drifting apart. Everything after it
 * was unpinned, which is how bClampToRange could be appended with nothing
 * checking where it landed. These three pin the last colour, the new flag, and
 * the total, so a member inserted ANYWHERE between UnderRangeColor and the end
 * moves at least one of them.
 *
 * bClampToRange is a uint32 alone on row 34, which is deliberate and is why it
 * was appended rather than tucked into the padding after a FVector3f: the four
 * colours before it are 16-byte-aligned FLinearColors with no gaps to fill, so
 * any 4-byte member has to start a row regardless. Putting it last keeps every
 * offset in this file unchanged - the numbers below are the ones the compiler
 * computes today, not a renumbering.
 */
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, UnderRangeColor) == 512, "cbuffer row 32");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, OverRangeColor) == 528, "cbuffer row 33");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, bClampToRange) == 544, "cbuffer row 34");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, bHasSceneDepth) == 600, "cbuffer row 37");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, DepthToSolver) == 604, "cbuffer row 37");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, DeviceZToViewZ) == 608, "cbuffer row 38");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, ViewRectMin) == 624, "cbuffer row 39");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, BlendAlpha) == 632, "cbuffer row 39");
static_assert(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, bBlendActive) == 636, "cbuffer row 39");
