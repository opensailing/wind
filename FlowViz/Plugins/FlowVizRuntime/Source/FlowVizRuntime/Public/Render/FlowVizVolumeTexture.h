// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizTypes.h"

// CoreMinimal.h forward-declares TArrayView but does not define it, and several
// entry points below take one by value.
#include "Containers/ArrayView.h"
#include "PixelFormat.h"
#include "RHIFwd.h"

/**
 * GPU volume data path - the RHI side of the ray-marching volume renderer
 * (plan.md section 9, ADR 002).
 *
 * WHAT THIS FILE IS FOR. A CVF frame arrives from FCFDVizVolumeReader as dense
 * bytes in the file's own storage type. This file turns those bytes into
 * persistent RHI 3D textures that a global shader can sample, and produces the
 * shader parameters that let the shader know where each voxel is in physical
 * space and which voxels it must refuse to sample. It does not ray-march and it
 * does not colour anything.
 *
 * THE SPLIT THAT MAKES THIS TESTABLE. Everything that can be wrong quietly -
 * voxel addressing, brick-to-dense placement, byte sizes, the physical <-> voxel
 * <-> UVW transform, the NaN/mask policy, the buffer-rotation policy - is a pure
 * function of plain data and is exercised by FlowVizVolumeTextureTest with no
 * RHI device present. Only three functions actually touch the RHI, and they live
 * behind FlowVizVolumeRHI below. What that leaves uncovered is stated on that
 * namespace, not glossed over.
 *
 * FOUR RULES THIS FILE EXISTS TO ENFORCE.
 *
 *  1. No value is converted. A float16 field is uploaded as IEEE binary16, a
 *     uint8 field as unsigned integers - never as a UNORM that the sampler would
 *     silently divide by 255. FlowVizVolumeFormat::ChooseTextureFormat picks
 *     integer formats for integer data for exactly this reason. The one
 *     transformation this file performs is widening a 3-component field to a
 *     4-channel texture, because no portable 3-channel 3D texture format exists;
 *     that adds a channel, it does not touch a stored component, and the fill
 *     pattern is a quiet NaN so a shader that wrongly reads it produces an
 *     obvious failure instead of a plausible zero.
 *
 *  2. Invalid is not zero. NaN, non-finite values and cells the mask field
 *     rejects are recorded in a separate status texture with distinct bit codes
 *     (FlowVizVoxelStatus). Status 0 means "nothing was ever written here", so a
 *     texture that failed to upload renders as absent rather than as a plausible
 *     field of zeroes. The field texture itself is never modified to mark
 *     invalidity - the stored bytes reach the GPU exactly as the file held them.
 *
 *  3. Voxels are not cubes. The shipped mock domain is 12 m x 4 m x 1 m over
 *     128 x 64 x 24 cells, so spacing is (0.09375, 0.0625, 0.0416667) - three
 *     different numbers. Spacing is a per-axis parameter everywhere in this file
 *     and in the shader parameter block. Code that collapses it to a scalar is
 *     wrong on the primary dataset.
 *
 *  4. Nothing here is allocated from a size the file chose. Every extent,
 *     component count and byte total is bounds-checked against
 *     FlowVizVolume::MaxTextureDimension and FlowVizVolume::MaxUploadBytes with
 *     overflow-safe arithmetic before a single byte is reserved, and a violation
 *     is an FCFDVizResult naming the offending quantity (engineering rule 12).
 *
 * THREADING. Everything except FlowVizVolumeRHI is a pure function callable from
 * any thread; the intended shape is that a worker thread decodes a frame and
 * builds an FFlowVizVolumeUpload, and only the handoff crosses to the render
 * thread (engineering rule 1 - no decompression or full-field work on the game
 * thread).
 */

class FCFDVizVolumeReader;

/* -------------------------------------------------------------------------- */
/* Policy limits                                                                */
/* -------------------------------------------------------------------------- */

namespace FlowVizVolume
{
	/**
	 * Largest edge, in voxels, this path will attempt on any axis.
	 *
	 * 2048 is the floor every D3D11-class and Metal-class device guarantees for a
	 * 3D texture, so a case that fits here loads everywhere the project ships.
	 * Devices commonly allow more; FlowVizVolumeRHI::CheckDeviceSupport additionally
	 * checks GMaxVolumeTextureDimensions at create time, which is the number that
	 * actually binds. This constant is the portable, testable-without-a-device cap
	 * so an oversized case is rejected with a message rather than by a driver.
	 */
	inline constexpr int32 MaxTextureDimension = 2048;

	/**
	 * Largest single upload buffer, in bytes. Matches CFDViz::MaxReadableBytes:
	 * TArray indexes with int32, so a byte array cannot exceed MAX_int32 elements
	 * regardless of available memory. Exceeding it is ECFDVizError::AllocationTooLarge -
	 * an honest "this build cannot address that", not an allocation failure.
	 */
	inline constexpr int64 MaxUploadBytes = MAX_int32;

	/**
	 * Channels a 3D texture may carry here. Capped at 4 by the CVF format itself,
	 * whose per-brick statistics and background value are float32[4]
	 * (CFDViz::MaxCvfComponentCount).
	 */
	inline constexpr int32 MaxTextureComponents = 4;

	/**
	 * Buffer counts for playback.
	 *
	 * Two is the minimum that lets frame A stay on screen while frame B uploads,
	 * and it is *not* enough to prefetch: with both buffers displayed during an
	 * interpolated frame, FlowVizVolumeRing::ChooseUploadSlot correctly returns
	 * INDEX_NONE because evicting either would tear the visible frame. Three is
	 * what makes "preload one ahead" possible, which is why plan.md section 8 asks
	 * for it. FlowVizVolumeRingTest asserts both halves of that statement.
	 */
	inline constexpr int32 MinBufferCount = 2;
	inline constexpr int32 RecommendedBufferCount = 3;
	inline constexpr int32 MaxBufferCount = 8;
}

/* -------------------------------------------------------------------------- */
/* Storage type -> texture format                                               */
/* -------------------------------------------------------------------------- */

/** Outcome of FlowVizVolumeFormat::ChooseTextureFormat. */
struct FFlowVizVolumeFormatChoice
{
	/** The RHI format. PF_Unknown when the choice failed. */
	EPixelFormat PixelFormat = PF_Unknown;

	/** Channels the texture carries. Equals the field's component count except for the 3 -> 4 widening. */
	int32 TextureComponentCount = 0;

	/** True when TextureComponentCount exceeds the field's component count, i.e. a pad channel exists. */
	bool bPadded = false;
};

namespace FlowVizVolumeFormat
{
	/**
	 * Pick the RHI format that carries this storage type WITHOUT converting it.
	 *
	 *     float16  1/2/3/4 -> R16F / G16R16F / FloatRGBA (RGBA16F, padded at 3)
	 *     float32  1/2/3/4 -> R32_FLOAT / G32R32F / A32B32G32R32F (padded at 3)
	 *     uint8    1/2/3/4 -> R8_UINT / R8G8_UINT / R8G8B8A8_UINT (padded at 3)
	 *
	 * uint8 maps to _UINT and never to PF_R8. PF_R8 is a UNORM format: the sampler
	 * would return value/255 and a quantised field would arrive at the shader
	 * silently rescaled, which is precisely the normalisation format rule 1.6 and
	 * engineering rule 5 forbid. _UINT delivers the stored integer.
	 *
	 * Three components have no portable 3D texture format on any target this
	 * project ships to, so they widen to four. The widening is the only data
	 * transformation in this file and it is additive - see GetPadElementBits.
	 *
	 * @return Ok, or UnsupportedDataType / IndexOutOfRange naming the rejected
	 *         combination. float64 is rejected here as it is in CVF (section 3.3).
	 */
	FLOWVIZRUNTIME_API FCFDVizResult ChooseTextureFormat(
		ECFDVizDataType DataType,
		int32 SourceComponentCount,
		FFlowVizVolumeFormatChoice& OutChoice);

	/**
	 * Bit pattern written into a pad channel, little-endian, for this storage type.
	 *
	 *     float32 -> 0x7FC00000   quiet NaN
	 *     float16 -> 0x7E00       quiet NaN
	 *     uint8   -> 0x00         no NaN exists in an integer format
	 *
	 * A pad channel is never read: the shader gates every fetch on
	 * FFlowVizVolumeShaderParameters::ComponentCount. NaN is chosen anyway so that
	 * a shader which forgets that gate produces a visibly broken result instead of
	 * a plausible zero, which is the failure that would otherwise ship. The uint8
	 * case cannot express that and is documented rather than faked.
	 */
	FLOWVIZRUNTIME_API uint32 GetPadElementBits(ECFDVizDataType DataType);
}

/* -------------------------------------------------------------------------- */
/* Dense volume layout                                                          */
/* -------------------------------------------------------------------------- */

/**
 * Byte layout of one dense volume, and every index and size derived from it.
 *
 * TWO STRIDES, DELIBERATELY NAMED APART. `Source*` describes the bytes a CVF
 * decode produces - the field's own component count. `Texture*` describes the
 * bytes the RHI texture holds - possibly one channel wider. They are equal
 * except for a 3-component field, and confusing them there produces a buffer
 * three quarters the size it should be, which reads as a shear rather than as a
 * crash. Every function below says which one it means.
 *
 * EXTENT IS A VALUE COUNT, NOT A CELL COUNT. For a point-associated field the
 * texture is one voxel larger per axis than the grid's cell dimensions; apply
 * that with FCFDVizGrid::ValueCounts or FFlowVizVolumeTransform::GetValueCounts,
 * never by hand at a call site.
 */
struct FFlowVizVolumeLayout
{
	/** Voxels along each axis - the texture's dimensions. Every component in 1..FlowVizVolume::MaxTextureDimension. */
	FIntVector Extent = FIntVector(0, 0, 0);

	/** Components the source bytes carry per voxel, interleaved (format section 4.4.2). 1..4. */
	int32 SourceComponentCount = 0;

	/** Channels the texture carries. Equals SourceComponentCount, or 4 when the source has 3. */
	int32 TextureComponentCount = 0;

	/** Storage type of one component. Never converted on the way to the GPU. */
	ECFDVizDataType DataType = ECFDVizDataType::Float32;

	/** RHI format chosen by FlowVizVolumeFormat::ChooseTextureFormat. */
	EPixelFormat PixelFormat = PF_Unknown;

	/**
	 * Build and bounds-check a layout.
	 *
	 * Checks, in this order: extent positive and within
	 * FlowVizVolume::MaxTextureDimension per axis; component count 1..4; data type
	 * legal for a CVF; a format exists; and the total texture byte count fits in
	 * MaxBytes and in an int64 without wrapping. The size check runs before any
	 * caller can allocate, which is engineering rule 12 - a size check that runs
	 * after the allocation it guards is decoration.
	 *
	 * @param MaxBytes Ceiling for the texture's byte total. Defaults to
	 *                 FlowVizVolume::MaxUploadBytes; pass less to impose a budget.
	 * @return Ok, or a failure naming the offending quantity and its value.
	 */
	FLOWVIZRUNTIME_API static FCFDVizResult Make(
		const FIntVector& InExtent,
		int32 InSourceComponentCount,
		ECFDVizDataType InDataType,
		FFlowVizVolumeLayout& OutLayout,
		int64 MaxBytes = FlowVizVolume::MaxUploadBytes);

	/**
	 * Layout of the per-voxel status texture for a volume of this extent:
	 * one uint8 channel, PF_R8_UINT, holding FlowVizVoxelStatus bits.
	 */
	FLOWVIZRUNTIME_API static FCFDVizResult MakeStatusLayout(
		const FIntVector& InExtent,
		FFlowVizVolumeLayout& OutLayout,
		int64 MaxBytes = FlowVizVolume::MaxUploadBytes);

	/** True for a layout that came out of a successful Make. A default-constructed layout is not valid. */
	FLOWVIZRUNTIME_API bool IsValid() const;

	/** Bytes per component. 0 for an unknown type, which then fails every size equality rather than becoming a bare stride. */
	int32 GetElementBytes() const
	{
		return SizeOfDataType(DataType);
	}

	/** Bytes for one voxel as the CVF decode produced it. */
	int64 GetSourceVoxelBytes() const
	{
		return static_cast<int64>(SourceComponentCount) * static_cast<int64>(GetElementBytes());
	}

	/** Bytes for one voxel in the texture. Larger than the source stride only for a widened 3-component field. */
	int64 GetTextureVoxelBytes() const
	{
		return static_cast<int64>(TextureComponentCount) * static_cast<int64>(GetElementBytes());
	}

	/** Voxels in the volume. INDEX_NONE on a degenerate extent or on overflow - treat that as a rejection, never as a count. */
	FLOWVIZRUNTIME_API int64 GetVoxelCount() const;

	/** Bytes of one X row, in source stride. INDEX_NONE on overflow. */
	FLOWVIZRUNTIME_API int64 GetSourceRowPitch() const;
	/** Bytes of one XY slice, in source stride. INDEX_NONE on overflow. */
	FLOWVIZRUNTIME_API int64 GetSourceSlicePitch() const;
	/** Bytes of the whole dense volume, in source stride - what FCFDVizVolumeReader::ReadDense yields. INDEX_NONE on overflow. */
	FLOWVIZRUNTIME_API int64 GetSourceVolumeBytes() const;

	/** Bytes of one X row as uploaded. This is UpdateTexture3D's SourceRowPitch. INDEX_NONE on overflow. */
	FLOWVIZRUNTIME_API int64 GetTextureRowPitch() const;
	/** Bytes of one XY slice as uploaded. This is UpdateTexture3D's SourceDepthPitch. INDEX_NONE on overflow. */
	FLOWVIZRUNTIME_API int64 GetTextureSlicePitch() const;
	/** Bytes of the whole texture. INDEX_NONE on overflow. */
	FLOWVIZRUNTIME_API int64 GetTextureVolumeBytes() const;

	/**
	 * Flatten (I,J,K) with X fastest, then Y, then Z - the CVF payload order
	 * (format section 4.4.1), so a decoded volume is walked linearly.
	 *
	 * @return The voxel index, or INDEX_NONE when the coordinate is outside the
	 *         extent. Out of range returns INDEX_NONE rather than wrapping, so a
	 *         bad index cannot become a valid-looking offset into other data.
	 */
	FLOWVIZRUNTIME_API int64 GetVoxelIndex(int32 I, int32 J, int32 K) const;

	/** Byte offset of voxel (I,J,K) in a source-stride buffer. INDEX_NONE when out of range. */
	FLOWVIZRUNTIME_API int64 GetSourceVoxelOffset(int32 I, int32 J, int32 K) const;

	/** Byte offset of voxel (I,J,K) in a texture-stride buffer. INDEX_NONE when out of range. */
	FLOWVIZRUNTIME_API int64 GetTextureVoxelOffset(int32 I, int32 J, int32 K) const;

	/** True when (I,J,K) addresses a voxel of this volume. */
	bool Contains(int32 I, int32 J, int32 K) const
	{
		return I >= 0 && J >= 0 && K >= 0 && I < Extent.X && J < Extent.Y && K < Extent.Z;
	}

	/** Same voxel grid, ignoring format and component count. Used to check that field and status textures agree. */
	bool HasSameExtent(const FFlowVizVolumeLayout& Other) const
	{
		return Extent == Other.Extent;
	}

	/** Diagnostic summary. Not a stable machine format. */
	FLOWVIZRUNTIME_API FString ToString() const;
};

/* -------------------------------------------------------------------------- */
/* Brick placement                                                              */
/* -------------------------------------------------------------------------- */

/** Where one CVF brick lands in the dense volume, and how its own bytes are strided. */
struct FFlowVizVolumeBrickPlacement
{
	/** First destination voxel: BrickCoordinate * BrickSize. */
	FIntVector DestOffset = FIntVector(0, 0, 0);

	/**
	 * Voxels this brick actually carries.
	 *
	 * min(BrickSize, Extent - DestOffset) per axis. EDGE BRICKS ARE NOT PADDED
	 * (format section 4.4.3): the last brick on an axis holds exactly the
	 * remainder. Treating it as a full BrickSize cube reads past the decoded
	 * buffer and shears every row after it, which renders as plausible turbulence.
	 */
	FIntVector ValidSize = FIntVector(0, 0, 0);

	/** Bytes of one X row within the brick, in source stride. */
	int64 SourceRowPitch = 0;
	/** Bytes of one XY slice within the brick, in source stride. */
	int64 SourceSlicePitch = 0;
	/** Bytes the brick's decoded payload must contain - the section 4.4.4 equality. */
	int64 SourceBytes = 0;
};

namespace FlowVizVolumeBrick
{
	/**
	 * Placement of the brick at BrickCoordinate, given the brick tiling BrickSize.
	 *
	 * @return Ok, or IndexOutOfRange when the coordinate lies outside the tiling,
	 *         or InvalidHeader for a non-positive brick size. Never returns a
	 *         padded ValidSize - see FFlowVizVolumeBrickPlacement::ValidSize.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult MakePlacement(
		const FFlowVizVolumeLayout& Layout,
		const FIntVector& BrickCoordinate,
		const FIntVector& BrickSize,
		FFlowVizVolumeBrickPlacement& OutPlacement);

	/** Bricks along each axis for this tiling: ceil(Extent / BrickSize). (0,0,0) on a degenerate input. */
	FLOWVIZRUNTIME_API FIntVector GetBrickCounts(const FFlowVizVolumeLayout& Layout, const FIntVector& BrickSize);

	/**
	 * Copy one decoded brick into its place in a dense source-stride buffer.
	 *
	 * Both buffers are in SOURCE stride - component widening happens afterwards,
	 * in FlowVizVolumeConvert::ExpandComponents, so this function never has to
	 * know about pad channels.
	 *
	 * @param BrickBytes Exactly Placement.SourceBytes bytes; anything else is
	 *                   SizeMismatch, checked before the first write.
	 * @param DenseBytes At least Layout.GetSourceVolumeBytes() bytes.
	 * @return Ok, or a failure naming the mismatch. On failure DenseBytes is
	 *         untouched - a partial copy would leave a volume that looks complete.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult CopyBrickIntoDense(
		const FFlowVizVolumeLayout& Layout,
		const FFlowVizVolumeBrickPlacement& Placement,
		TArrayView<const uint8> BrickBytes,
		TArrayView<uint8> DenseBytes);
}

/* -------------------------------------------------------------------------- */
/* Component widening                                                           */
/* -------------------------------------------------------------------------- */

namespace FlowVizVolumeConvert
{
	/**
	 * Widen a dense source-stride buffer to the texture's channel count.
	 *
	 * Stored components are copied byte for byte - no widening of precision, no
	 * scaling, no reordering. Only the added channel is synthesised, and it is
	 * filled with FlowVizVolumeFormat::GetPadElementBits.
	 *
	 * When no widening is needed this still produces a correct output buffer (a
	 * straight copy) so callers have one path; use
	 * Layout.SourceComponentCount == Layout.TextureComponentCount to skip it and
	 * upload the source buffer directly.
	 *
	 * @param OutBytes Resized to Layout.GetTextureVolumeBytes(). Emptied on
	 *                 failure, never left half-written.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult ExpandComponents(
		const FFlowVizVolumeLayout& Layout,
		TArrayView<const uint8> SourceBytes,
		TArray<uint8>& OutBytes);
}

/* -------------------------------------------------------------------------- */
/* Per-voxel validity                                                           */
/* -------------------------------------------------------------------------- */

/**
 * Status bits stored in the R8_UINT status texture, one byte per voxel.
 *
 * ZERO IS NOT "VALID". Zero is Unknown: no status was ever written for this
 * voxel. A texture that failed to upload, or a region never covered by a brick,
 * therefore reads as absent and the shader draws nothing there - rather than
 * reading as a plausible field of zeroes, which is the failure engineering rule
 * 10 exists to prevent. Validity is an explicit bit that something had to set.
 *
 * The bits are a diagnosis, not just a yes/no: the volume renderer's masked
 * colour, NaN colour and "no data" colour are different (plan.md section 10.3),
 * and the diagnostics panel reports masked and NaN counts separately, which is
 * impossible if the three collapse into one flag.
 */
namespace FlowVizVoxelStatus
{
	/** No status written. The zero value, deliberately: fail closed. */
	inline constexpr uint8 Unknown = 0;

	/** Every component finite, and the mask field did not reject this cell. The only bit that permits sampling. */
	inline constexpr uint8 Valid = 1 << 0;

	/** At least one component is NaN. The stored bit pattern is still in the field texture, unmodified. */
	inline constexpr uint8 NaN = 1 << 1;

	/** At least one component is +/-infinity. Distinct from NaN because +inf is how CVF spells "no valid data" (section 4.4.7). */
	inline constexpr uint8 Infinite = 1 << 2;

	/** The grid's mask field rejected this cell - solid geometry, an obstacle interior, an inactive region. */
	inline constexpr uint8 Masked = 1 << 3;

	/** Every bit this build defines. A byte with anything else set did not come from BuildVoxelStatus. */
	inline constexpr uint8 KnownBits = Valid | NaN | Infinite | Masked;
}

/** Inputs to FlowVizVolumeStatus::Build. */
struct FFlowVizVolumeStatusSource
{
	/**
	 * The field's dense bytes in SOURCE stride - exactly what
	 * FCFDVizVolumeReader::ReadDense produced. Read only; Build never writes to
	 * the field, so NaN payload bits reach the GPU bit-exact.
	 */
	TArrayView<const uint8> FieldBytes;

	/**
	 * The grid's mask field for the same frame: dense, uint8, one component, same
	 * extent. Empty when the case declares no `grid.maskField`, in which case no
	 * voxel is marked Masked.
	 */
	TArrayView<const uint8> MaskBytes;

	/**
	 * Mask polarity. True - the default and the only value the format defines -
	 * means a NON-ZERO mask value keeps the cell, matching `mask != 0` in the
	 * Python reference (Tools/cfdviz/src/cfdviz/case.py). Set false only for a
	 * case that documents the opposite convention; nothing in CFDViz 1.0 does.
	 */
	bool bNonZeroKeeps = true;
};

namespace FlowVizVolumeStatus
{
	/**
	 * Classify one stored component from its raw little-endian bytes.
	 *
	 * @return FlowVizVoxelStatus::NaN, ::Infinite, or 0 for a finite value.
	 *         Integer types are always finite. Reads exactly
	 *         SizeOfDataType(DataType) bytes; the caller guarantees they exist.
	 *
	 * Deliberately does not go through float conversion: a float16 NaN widened to
	 * float and back can lose its payload, and a classification that depends on
	 * that round trip would disagree with the bytes actually uploaded.
	 */
	FLOWVIZRUNTIME_API uint8 ClassifyElement(const uint8* ElementBytes, ECFDVizDataType DataType);

	/**
	 * Build the per-voxel status volume for one field.
	 *
	 * A voxel is Valid only if no component is NaN or infinite AND the mask field
	 * did not reject it. Otherwise the byte carries the reasons, one bit each, and
	 * Valid is clear. A vector field is judged per voxel, not per component: a
	 * velocity with one NaN component has no usable magnitude and no usable
	 * direction, so the whole voxel is marked - the same rule
	 * FCFDVizStatistics::AccumulateSample applies to magnitudes.
	 *
	 * @param StatusLayout Must have been built by
	 *                     FFlowVizVolumeLayout::MakeStatusLayout with the field's
	 *                     extent; a mismatch is rejected rather than truncated.
	 * @param OutStatus    Resized to the voxel count. Emptied on failure.
	 * @return Ok, or SizeMismatch naming which buffer disagreed with the layout.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult Build(
		const FFlowVizVolumeLayout& FieldLayout,
		const FFlowVizVolumeLayout& StatusLayout,
		const FFlowVizVolumeStatusSource& Source,
		TArray<uint8>& OutStatus);

	/** Voxels carrying each status bit. Cheap enough to run per upload, and it is what the diagnostics panel reports. */
	struct FCounts
	{
		int64 Total = 0;
		int64 Valid = 0;
		int64 NaN = 0;
		int64 Infinite = 0;
		int64 Masked = 0;
		int64 Unknown = 0;
	};

	/** Tally a status volume. A voxel with several bits set is counted once per bit, and once in Total. */
	FLOWVIZRUNTIME_API FCounts CountStatus(TArrayView<const uint8> StatusBytes);
}

/* -------------------------------------------------------------------------- */
/* Shader parameters                                                            */
/* -------------------------------------------------------------------------- */

/**
 * Everything the ray-marcher needs to know about where the volume is and which
 * voxels it may sample. Laid out for a direct memcpy into a constant buffer.
 *
 * HLSL PACKING IS THE REASON FOR THE ODD SHAPE. A constant-buffer member may not
 * straddle a 16-byte boundary, so this struct is built as explicit 16-byte rows
 * and every offset is pinned with a static_assert at the bottom of this file. In
 * a unity build, where a stray sibling constant can quietly change a value, an
 * unasserted offset is how a shader ends up reading spacing out of the dimension
 * slot - which renders as a plausibly-shaped, wrong volume. See Docs/BUILD.md.
 *
 * EVERYTHING HERE IS LOCAL TO THE VOLUME. Local space is
 * [0, PhysicalSize] in solver units with the grid's minimum corner at the
 * origin. World placement is the scene proxy's LocalToWorld, which must be
 * FFlowVizVolumeTransform::GetLocalToUnrealTransform - a double-precision
 * FMatrix. Keeping the large magnitudes out of this float block is what stops a
 * kilometre-scale case from losing metres of precision; GridOrigin below is
 * carried for diagnostics and probe display only, with its own error term.
 */
struct alignas(16) FFlowVizVolumeShaderParameters
{
	/* row 0 */
	/** Grid origin in solver units, NARROWED TO FLOAT. Diagnostics only - see the struct comment. */
	FVector3f GridOrigin = FVector3f::ZeroVector;
	/** Largest |Origin - (float)Origin| across the three axes, in solver units. Non-zero means GridOrigin is lossy and must not be used for placement. */
	float OriginNarrowingError = 0.0f;

	/* row 1 */
	/** Domain extent in solver units: Spacing * cell dimensions. The crop box and clip planes are expressed against this. */
	FVector3f PhysicalSize = FVector3f::ZeroVector;
	/** Sx*Sy*Sz. A per-voxel physical volume, needed by any integral over the field, and a value no cubic-voxel shortcut computes correctly. */
	float VoxelVolume = 0.0f;

	/* row 2 */
	/** Cell size per axis, solver units. THREE INDEPENDENT NUMBERS - the mock domain's are 0.09375, 0.0625, 0.0416667. */
	FVector3f VoxelSpacing = FVector3f::ZeroVector;
	/** min(Sx,Sy,Sz). The ray-march step in voxel units must be scaled by this or the thinnest axis is undersampled and aliases. */
	float MinVoxelSpacing = 0.0f;

	/* row 3 */
	/** 1 / VoxelSpacing, per axis. */
	FVector3f InvVoxelSpacing = FVector3f::ZeroVector;
	/** max(Sx,Sy,Sz). With MinVoxelSpacing this gives the anisotropy ratio, which gradient estimation needs. */
	float MaxVoxelSpacing = 0.0f;

	/* row 4 */
	/** Texture dimensions in voxels - VALUE counts, so one larger per axis than the cell dimensions for a point-associated field. */
	FIntVector VolumeDimensions = FIntVector(0, 0, 0);
	/** Stored components the field carries, 1..4. The shader must not read channels beyond this; the rest is pad. */
	int32 ComponentCount = 0;

	/* row 5 */
	/** 1 / VolumeDimensions, per axis - one texel in UVW. */
	FVector3f InvVolumeDimensions = FVector3f::ZeroVector;
	/** Channels the texture actually has. Greater than ComponentCount exactly when a 3-component field was widened to 4. */
	int32 TextureComponentCount = 0;

	/* row 6 */
	/** Local position -> texture UVW: UVW = LocalPos * UVWScale + UVWBias. Per axis, so anisotropy is carried, not assumed. */
	FVector3f UVWScale = FVector3f::ZeroVector;
	/** ECFDVizAssociation as a uint: 0 cell, 1 point. The half-voxel offset is already folded into UVWBias; this is for diagnostics. */
	uint32 AssociationCode = 0;

	/* row 7 */
	/** Companion of UVWScale. Zero for a cell-associated field; 0.5/VolumeDimensions for a point-associated one. */
	FVector3f UVWBias = FVector3f::ZeroVector;
	/** ECFDVizDataType as a uint. The shader needs it to know whether NaN can exist in the field texture at all. */
	uint32 DataTypeCode = 0;

	/* row 8 */
	/** Status bits a voxel must carry to be sampled. FlowVizVoxelStatus::Valid. A voxel failing this is rejected, never clamped to zero. */
	uint32 RequiredStatusMask = 0;
	/** Status bits that mean "explicitly invalid" rather than "never written", so the shader can colour masked and NaN differently from absent. */
	uint32 InvalidStatusMask = 0;
	/** 1 when a status texture is bound. 0 means the shader must reject every voxel - not that everything is valid. */
	uint32 bHasStatusTexture = 0;
	/** 1 when a vector texture is bound alongside the scalar one. */
	uint32 bHasVectorTexture = 0;

	/* row 9 */
	/** Transfer-function domain minimum, in solver units. For under-range colouring ONLY; values are never clamped into it. */
	float ValueRangeMin = 0.0f;
	/** Transfer-function domain maximum, same caveat. */
	float ValueRangeMax = 0.0f;
	/** 1 when the shader must additionally test isfinite() on the fetched value. Set for float formats; pointless for uint8, where it is 0. */
	uint32 bRejectNonFinite = 0;
	/** Bit pattern FlowVizVolumeFormat::GetPadElementBits wrote into the pad channel, so a shader assertion can check it. */
	uint32 PadElementBits = 0;
};

/* -------------------------------------------------------------------------- */
/* Physical <-> voxel <-> UVW                                                   */
/* -------------------------------------------------------------------------- */

/**
 * The one place the grid's physical geometry becomes GPU addressing.
 *
 * DOUBLE ON THE CPU, FLOAT ONLY AT THE END. Physical coordinates stay double
 * through every function here, matching FCFDVizGrid, because scientific domains
 * run from micrometre features to kilometre extents. The narrowing to float
 * happens exactly once, in MakeShaderParameters, and what it cost is reported by
 * GetOriginNarrowingError rather than being silently absorbed.
 *
 * THE HALF-VOXEL. A cell-associated value sits at the cell CENTRE, a
 * point-associated value sits ON the grid point. Both map to the centre of a
 * texel, but the physical position of texel 0 differs between them by half a
 * cell, and the point case also needs one extra texel per axis. Getting this
 * wrong shifts the entire field by half a voxel, which renders plausibly and is
 * wrong - the format spec calls it the single most common visualisation error.
 * It is applied here, once, and folded into UVWBias.
 */
struct FFlowVizVolumeTransform
{
	/** Cell dimensions, origin and spacing, in solver units and canonical CFDViz axes. */
	FCFDVizGrid Grid;

	/** Where this field's values sit relative to the grid. Decides the texture extent and the UVW bias. */
	ECFDVizAssociation Association = ECFDVizAssociation::Cell;

	/** Grid must satisfy FCFDVizGrid::IsValid and the value counts must be addressable. */
	FLOWVIZRUNTIME_API bool IsValid() const;

	/** Texture extent: cell dimensions, or one more per axis for a point-associated field. (0,0,0) when degenerate. */
	FIntVector GetValueCounts() const
	{
		return Grid.ValueCounts(Association);
	}

	/** Domain extent in solver units: Spacing * cell dimensions. The same box for either association. */
	FVector GetPhysicalSize() const
	{
		return FVector(
			static_cast<double>(Grid.Dimensions.X),
			static_cast<double>(Grid.Dimensions.Y),
			static_cast<double>(Grid.Dimensions.Z)) * Grid.Spacing;
	}

	/**
	 * Solver position -> continuous voxel coordinate, where an integer result
	 * lands exactly on a stored value.
	 *
	 * Cell:  (P - Origin)/Spacing - 0.5   so cell centre i maps to i
	 * Point: (P - Origin)/Spacing         so grid point i maps to i
	 *
	 * Not bounds checked; it extrapolates outside the grid on purpose, for ghost
	 * cells and neighbour lookups.
	 */
	FLOWVIZRUNTIME_API FVector PhysicalToVoxel(const FVector& SolverPosition) const;

	/** Exact inverse of PhysicalToVoxel. */
	FLOWVIZRUNTIME_API FVector VoxelToPhysical(const FVector& VoxelCoordinate) const;

	/**
	 * Solver position -> texture UVW, the coordinate a hardware sampler wants.
	 *
	 * UVW = (VoxelCoordinate + 0.5) / ValueCounts, which places voxel i at the
	 * centre of texel i for both associations. Per axis throughout: for the mock
	 * domain the three scale factors are 1/12, 1/4 and 1/1, and any code that
	 * derives one of them from another is wrong there.
	 */
	FLOWVIZRUNTIME_API FVector PhysicalToTextureUVW(const FVector& SolverPosition) const;

	/** Exact inverse of PhysicalToTextureUVW. */
	FLOWVIZRUNTIME_API FVector TextureUVWToPhysical(const FVector& UVW) const;

	/**
	 * Local-space -> Unreal world transform for the volume's scene proxy.
	 *
	 * Local space is [0, PhysicalSize] in solver units. This composes the
	 * translation to the grid origin with MakeSolverToUnrealTransform, so
	 * the axis and unit conversion has exactly one implementation
	 * (engineering rule 4, ADR 004).
	 *
	 * The result's determinant is NEGATIVE - solver-to-Unreal mirrors Y - so the
	 * proxy's box geometry needs its winding reversed. TransformReversesWinding
	 * answers that for any matrix; do not rediscover it per call site.
	 *
	 * @param MetersToUnrealUnits Derive from the manifest's `units.length`. Do not
	 *                            assume the case is in metres.
	 */
	FLOWVIZRUNTIME_API FMatrix GetLocalToUnrealTransform(
		double MetersToUnrealUnits = CFDViz::MetersToUnrealCentimeters) const;

	/**
	 * Largest error, in solver units, that narrowing the origin to float32 costs.
	 *
	 * Zero for a small origin, metres for a kilometre-scale one. Reported rather
	 * than hidden: it is what tells a caller that
	 * FFlowVizVolumeShaderParameters::GridOrigin must not be used for placement.
	 */
	FLOWVIZRUNTIME_API double GetOriginNarrowingError() const;

	/**
	 * Fill the shader parameter block.
	 *
	 * @param Layout   The field texture's layout. Its extent must equal
	 *                 GetValueCounts(); a mismatch is a rejection, because a
	 *                 texture sized from the cell count instead of the value count
	 *                 is the point/cell bug and it renders plausibly.
	 * @param ValueRange The transfer function's domain, in solver units, for the
	 *                 field this layout describes. See the overload below for why
	 *                 this is a parameter and not something derived here.
	 * @param OutParams Written only on success.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult MakeShaderParameters(
		const FFlowVizVolumeLayout& Layout,
		const FVector2D& ValueRange,
		FFlowVizVolumeShaderParameters& OutParams) const;

	/**
	 * As above, with a DELIBERATELY DEGENERATE colour domain of [0, 0].
	 *
	 * THIS OVERLOAD DOES NOT PRODUCE A RENDERABLE VOLUME, and that is the point
	 * of keeping it separate rather than defaulting the parameter. A grid and a
	 * texture layout do not carry a field's value range - only the manifest's
	 * statistics do - so this function cannot compute one, and the placeholder
	 * it writes has a specific, invisible consequence: the .usf normalises
	 * through `if (ValueRangeMax > ValueRangeMin)`, so with max == min the guard
	 * never fires, T stays 0 for every voxel, and the entire volume samples LUT
	 * entry 0. The range-classification blocks at .usf:367 and :813 are guarded
	 * on the same comparison, so no UNDER_RANGE or OVER_RANGE bit fires either.
	 * The result is a uniformly coloured block that nothing discloses.
	 *
	 * A DEFAULT ARGUMENT WOULD HAVE MADE THAT THE QUIET PATH. It was, until
	 * FlowViz.Scene.VolumeValueRange was written: the range was hardcoded to
	 * 0/0 here with no caller aware of it. Callers that genuinely have no range
	 * must now say so at the call site.
	 *
	 * Use it only for parameter blocks whose colours will not be read - layout
	 * and placement assertions, offset checks. Anything that reaches a pixel
	 * wants the overload above.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult MakeShaderParametersWithoutValueRange(
		const FFlowVizVolumeLayout& Layout,
		FFlowVizVolumeShaderParameters& OutParams) const;
};

/* -------------------------------------------------------------------------- */
/* One frame's upload payload                                                   */
/* -------------------------------------------------------------------------- */

/**
 * One complete frame, assembled off the render thread and handed over whole.
 *
 * COMPLETE IS THE POINT. plan.md section 8 requires that partially updated field
 * components are never displayed as a frame. Building the entire payload on a
 * worker thread and moving it across in one command is how that is guaranteed
 * structurally rather than by discipline: there is no intermediate state in which
 * the scalar texture holds frame N and the vector texture holds frame N-1.
 */
struct FFlowVizVolumeUpload
{
	/** Which stored frame this is. INDEX_NONE is not uploadable. */
	int32 FrameIndex = INDEX_NONE;

	/** Physical time in the manifest's time unit, carried through for the diagnostics panel. */
	double SimulationTime = 0.0;

	/** Scalar field layout and TEXTURE-stride bytes. Leave the layout invalid to skip the scalar texture. */
	FFlowVizVolumeLayout ScalarLayout;
	TArray<uint8> ScalarBytes;

	/** Vector field layout and TEXTURE-stride bytes. Separate resource from the scalar field, per plan.md section 9. */
	FFlowVizVolumeLayout VectorLayout;
	TArray<uint8> VectorBytes;

	/** Per-voxel FlowVizVoxelStatus bytes. Strongly recommended: without it the shader has no way to tell invalid from zero. */
	FFlowVizVolumeLayout StatusLayout;
	TArray<uint8> StatusBytes;

	/**
	 * Check the payload against its own layouts before anything is uploaded.
	 *
	 * Verifies: a frame index; at least one field present; each present buffer's
	 * length equals its layout's texture byte count exactly; and every present
	 * layout shares one extent. That last check is what catches a status texture
	 * built for the cell count while the field used the value count - the two
	 * differ by one voxel per axis and the resulting misalignment marks the wrong
	 * cells invalid, which looks like noise in the data.
	 *
	 * @return Ok, or a failure naming the buffer and both sizes.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult Validate() const;

	/** Total bytes this payload will hand to the GPU. 0 for an empty payload. */
	FLOWVIZRUNTIME_API int64 GetTotalBytes() const;
};

/* -------------------------------------------------------------------------- */
/* Buffer rotation                                                              */
/* -------------------------------------------------------------------------- */

/** What one buffer slot of FFlowVizVolumeTextureSet currently holds. */
struct FFlowVizVolumeSlotState
{
	/** Frame resident in this slot, or INDEX_NONE when the slot has never been filled. */
	int32 FrameIndex = INDEX_NONE;

	/** Monotonic counter stamped on every use. Larger is more recent; ties are broken by slot order. */
	uint64 LastUseSerial = 0;

	/** True between choosing this slot and the render thread finishing its upload. Such a slot is never chosen again or displayed. */
	bool bUploadInFlight = false;
};

namespace FlowVizVolumeRing
{
	/**
	 * Slot holding FrameIndex, or INDEX_NONE.
	 *
	 * A slot with an upload in flight does NOT match: its texture holds the
	 * previous frame's voxels until the render thread is done, and displaying it
	 * would show the old frame under the new frame's label.
	 */
	FLOWVIZRUNTIME_API int32 FindSlotForFrame(TArrayView<const FFlowVizVolumeSlotState> Slots, int32 FrameIndex);

	/**
	 * The most recently USED slot holding any frame at all (largest
	 * LastUseSerial; in-flight slots excluded). INDEX_NONE when nothing is
	 * resident. This is the HOLD-LAST-FRAME fallback: during playback the
	 * display frame's upload can lag the playhead, and a renderer that draws
	 * nothing for that gap strobes at upload latency -- verbatim "it blinks
	 * on and off over the timeline" from the app's first real user.
	 */
	FLOWVIZRUNTIME_API int32 FindMostRecentSlot(TArrayView<const FFlowVizVolumeSlotState> Slots);

	/**
	 * Choose the slot to upload FrameIndex into.
	 *
	 * In order: a slot already holding this frame; then any never-filled slot;
	 * then the least recently used slot that is neither displayed nor already
	 * uploading.
	 *
	 * @param PinnedFrameA,PinnedFrameB The frames currently on screen. During
	 *        interpolation both are read every frame, so evicting either tears the
	 *        visible image. Pass INDEX_NONE for none.
	 * @return The slot, or INDEX_NONE when every slot is pinned or busy. That is a
	 *         real and expected answer for a two-buffer set mid-interpolation, and
	 *         it is the concrete reason plan.md section 9 asks for three buffers -
	 *         the caller must defer the prefetch, not evict a displayed frame.
	 */
	FLOWVIZRUNTIME_API int32 ChooseUploadSlot(
		TArrayView<const FFlowVizVolumeSlotState> Slots,
		int32 FrameIndex,
		int32 PinnedFrameA,
		int32 PinnedFrameB);
}

/* -------------------------------------------------------------------------- */
/* Reader -> upload                                                             */
/* -------------------------------------------------------------------------- */

class FCFDVizVolumeReader;

namespace FlowVizVolumeBuild
{
	/**
	 * Decode one CVF and produce the bytes a texture upload wants.
	 *
	 * Reads the dense volume in the file's own storage type, then widens it to the
	 * texture's channel count if - and only if - the field has three components.
	 * No value is converted at any point.
	 *
	 * THE BUDGET IS CHECKED AGAINST THE WIDENED SIZE. A 3-component float16 field
	 * decodes to 6 bytes per voxel and uploads as 8, so a limit applied only to
	 * the read would pass a case that then allocates a third more than the caller
	 * allowed. MaxBytes bounds the larger of the two, before either is reserved.
	 *
	 * Worker-thread callable, and intended to be called there: it does file I/O
	 * and zlib decompression, neither of which may happen on the game thread
	 * (engineering rule 1).
	 *
	 * @param Reader   Must be open. A closed reader is refused, not read from.
	 * @param OutBytes Texture-stride bytes. Emptied on failure.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult BuildFieldBytes(
		FCFDVizVolumeReader& Reader,
		FFlowVizVolumeLayout& OutLayout,
		TArray<uint8>& OutBytes,
		int64 MaxBytes = FlowVizVolume::MaxUploadBytes);

	/**
	 * Assemble one complete frame: field bytes, status bytes, and the metadata
	 * from the CVF header.
	 *
	 * @param MaskReader The grid's mask field for the SAME frame, or null when the
	 *                   case declares none. Its extent must match the field's; a
	 *                   mismatch is rejected rather than truncated, because a
	 *                   truncated mask silently unmasks the tail of the volume.
	 * @param bAsVector  Whether this field belongs in the vector texture or the
	 *                   scalar one. The caller decides from the manifest, since a
	 *                   1-component field can be either and the component count
	 *                   alone cannot say.
	 * @param OutUpload  Overwritten. Left empty on failure.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult BuildUpload(
		FCFDVizVolumeReader& Reader,
		FCFDVizVolumeReader* MaskReader,
		bool bAsVector,
		FFlowVizVolumeUpload& OutUpload,
		int64 MaxBytes = FlowVizVolume::MaxUploadBytes);
}

/* -------------------------------------------------------------------------- */
/* Version-isolated RHI wrapper                                                 */
/* -------------------------------------------------------------------------- */

/**
 * THE ONLY CODE IN FLOWVIZ THAT CALLS A TEXTURE CREATE OR UPDATE API.
 *
 * ADR 002 accepts a hand-written global-shader ray-marcher and names its cost:
 * global shaders and RHI resource APIs churn between engine releases, and shader
 * problems surface at runtime rather than at build time. The mitigation it names
 * is this namespace. Everything version-sensitive - FRHITextureCreateDesc,
 * FUpdateTextureRegion3D, the command-list plumbing - is confined to
 * FlowVizVolumeTexture.cpp, so an engine upgrade is a diff against one file
 * rather than a hunt. Do not call RHICreateTexture or UpdateTexture3D anywhere
 * else; the isolation only holds if it is total.
 *
 * The .cpp carries a static_assert on ENGINE_MINOR_VERSION that fails on a newer
 * engine with instructions. That is intentional. An upgrade that silently
 * compiles against changed semantics is the failure mode being prevented.
 *
 * WHAT IS NOT COVERED BY TESTS. These four functions need a live RHI device, so
 * the automation suite's default -nullrhi run cannot exercise them; the
 * FlowViz.Render.VolumeTextureDevice test runs them only under RHI=1 and skips
 * with a logged reason when it cannot. Specifically unverified without a device:
 * that the driver accepts each chosen EPixelFormat as a 3D texture, that its row
 * and slice pitch interpretation matches ours, and that a sampled texel returns
 * the byte we uploaded. Everything that decides WHICH bytes go where is pure and
 * is covered.
 */
namespace FlowVizVolumeRHI
{
	/**
	 * Is this layout creatable on the device actually present?
	 *
	 * Checks GMaxVolumeTextureDimensions and the format's reported texture
	 * support, both of which are RHI globals unavailable to a pure test. The
	 * portable limits are already enforced by FFlowVizVolumeLayout::Make; this is
	 * the narrower, device-specific gate.
	 *
	 * @return Ok, or a failure naming the limit and the device's value. Returns Ok
	 *         when no RHI is initialised, because a null-RHI run has no device to
	 *         fail against and must not report a phantom incompatibility.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult CheckDeviceSupport(const FFlowVizVolumeLayout& Layout);

	/**
	 * Create one persistent 3D texture. Render thread only.
	 *
	 * Created ONCE per slot and then updated in place for the life of the case -
	 * engineering rule 2, no per-display-frame resource recreation. A layout
	 * change (a different case, or a different field shape) is the only reason to
	 * release and recreate.
	 *
	 * @return A null ref on failure, with the reason in OutResult. Never asserts on
	 *         a bad layout; a malformed case must produce a message, not a crash.
	 */
	FLOWVIZRUNTIME_API FTextureRHIRef CreateVolumeTexture(
		FRHICommandListBase& RHICmdList,
		const FFlowVizVolumeLayout& Layout,
		const TCHAR* DebugName,
		FCFDVizResult& OutResult);

	/**
	 * Overwrite the whole texture. Render thread only.
	 *
	 * @param TextureBytes Exactly Layout.GetTextureVolumeBytes() bytes, in texture
	 *                     stride. A short buffer is rejected before the RHI call
	 *                     rather than read past - UpdateTexture3D has no idea how
	 *                     long the source is and will happily walk off the end.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult UpdateVolumeTexture(
		FRHICommandListBase& RHICmdList,
		FRHITexture* Texture,
		const FFlowVizVolumeLayout& Layout,
		TArrayView<const uint8> TextureBytes);

	/**
	 * Overwrite one sub-box - a brick, or a region of interest. Render thread only.
	 *
	 * @param Placement    Destination box, from FlowVizVolumeBrick::MakePlacement.
	 * @param RegionBytes  Exactly ValidSize.X*Y*Z * Layout.GetTextureVoxelBytes()
	 *                     bytes, tightly packed in TEXTURE stride.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult UpdateVolumeTextureRegion(
		FRHICommandListBase& RHICmdList,
		FRHITexture* Texture,
		const FFlowVizVolumeLayout& Layout,
		const FFlowVizVolumeBrickPlacement& Placement,
		TArrayView<const uint8> RegionBytes);
}

/* -------------------------------------------------------------------------- */
/* The multi-buffered resource                                                  */
/* -------------------------------------------------------------------------- */

/** One buffer slot's textures. Created once; updated in place. */
struct FFlowVizVolumeSlotTextures
{
	FTextureRHIRef ScalarTexture;
	FTextureRHIRef VectorTexture;
	FTextureRHIRef StatusTexture;

	FFlowVizVolumeLayout ScalarLayout;
	FFlowVizVolumeLayout VectorLayout;
	FFlowVizVolumeLayout StatusLayout;

	/** Physical time of the resident frame, for the diagnostics panel. */
	double SimulationTime = 0.0;

	/** True once at least one upload has completed into this slot. */
	bool bHasContent = false;
};

/**
 * A double- or triple-buffered set of persistent volume textures.
 *
 * Holds N slots. Frame A and frame B - the two the shader interpolates between -
 * are pinned by SetDisplayFrames and are never overwritten while pinned. A
 * prefetch that cannot find a free slot is refused rather than served by evicting
 * a visible frame.
 *
 * LIFETIME. Slot textures are RHI resources and may only be created, updated or
 * released on the render thread. ReleaseResources enqueues that work; the owner
 * must flush rendering commands before destroying the set, exactly as any
 * FRenderResource owner must.
 */
class FLOWVIZRUNTIME_API FFlowVizVolumeTextureSet
{
public:
	FFlowVizVolumeTextureSet() = default;
	~FFlowVizVolumeTextureSet();

	// Owns RHI references; copying one would double-release them.
	FFlowVizVolumeTextureSet(const FFlowVizVolumeTextureSet&) = delete;
	FFlowVizVolumeTextureSet& operator=(const FFlowVizVolumeTextureSet&) = delete;

	/**
	 * Allocate the slot bookkeeping. No RHI work happens here, so this is safe
	 * from any thread and is what a test without a device exercises.
	 *
	 * @param NumBuffers FlowVizVolume::MinBufferCount..MaxBufferCount. Use
	 *                   RecommendedBufferCount unless memory is tight; see
	 *                   FlowVizVolumeRing::ChooseUploadSlot for what two costs you.
	 */
	FCFDVizResult Initialize(int32 NumBuffers);

	/** Slots configured, or 0 before Initialize. */
	int32 GetBufferCount() const;

	/** A stable bookkeeping snapshot for diagnostics and tests. */
	TArray<FFlowVizVolumeSlotState> GetSlotStates() const;

	/** Copy one slot's textures into OutTextures. False when the index is out of range. */
	bool GetSlotTextures(int32 SlotIndex, FFlowVizVolumeSlotTextures& OutTextures) const;

	/**
	 * Pin the frames being displayed. Pass INDEX_NONE for a slot that is not in
	 * use. Frames not resident are ignored - pinning is about protecting what IS
	 * resident, not about requesting a load.
	 */
	void SetDisplayFrames(int32 FrameA, int32 FrameB);

	int32 GetDisplayFrameA() const;
	int32 GetDisplayFrameB() const;

	/**
	 * Layout of the field texture this set has accepted uploads for, or an
	 * invalid layout before the first one.
	 *
	 * WHICH TEXTURE IT DESCRIBES depends on the field: a scalar field's bytes go
	 * to the scalar texture and a vector field's to the vector texture, and this
	 * follows whichever the payload filled. That is what a caller building
	 * shader parameters needs -- parameters describing an absent scalar texture
	 * would be worse than none.
	 *
	 * RECORDED ON THE GAME THREAD IN EnqueueUpload, not in the render command,
	 * so it is readable by the caller that just uploaded without waiting on the
	 * GPU. It describes what was ACCEPTED for upload rather than what has landed
	 * -- a validated payload's layout, since Validate has already rejected any
	 * payload whose bytes and layout disagree.
	 *
	 * WHY THE SET OWNS THIS. Two independent callers upload into a component's
	 * texture set -- UCFDVizVolumeComponent::UploadFrame for the headless
	 * capture path, and FFlowVizCasePlayer::DrainCompletedLoads for interactive
	 * playback -- and both build their payload with the same
	 * FlowVizVolumeBuild::BuildUpload. A layout cached beside only ONE of them is
	 * the shape of bug this accessor exists to prevent: the component used to
	 * hold this itself, so a case played back rather than captured uploaded its
	 * voxels correctly and still marched nothing, because the layout the shader
	 * parameters are built from was only ever filled by the capture path.
	 */
	FFlowVizVolumeLayout GetUploadedFieldLayout() const;

	/** Slot holding this frame and ready to sample, or INDEX_NONE. */
	int32 FindSlotForFrame(int32 FrameIndex) const;

	/** The most recently used slot ready to sample -- the hold-last-frame fallback. INDEX_NONE when nothing is resident. */
	int32 FindMostRecentResidentSlot() const;

	/** Which slot an upload of FrameIndex would use, without reserving it. INDEX_NONE when every slot is pinned or busy. */
	int32 PeekUploadSlot(int32 FrameIndex) const;

	/**
	 * Hand a complete frame to the render thread.
	 *
	 * Validates the payload, reserves a slot, and enqueues one render command that
	 * creates any missing textures and updates them in place. Returns before the
	 * upload happens - by design, since the caller is the game or a worker thread
	 * and must not block on the GPU.
	 *
	 * @return Ok once the work is queued. AllocationTooLarge when no slot is
	 *         available, which the caller should treat as "retry after the display
	 *         frames advance", not as an error to surface. Any payload problem is
	 *         reported from FFlowVizVolumeUpload::Validate and nothing is queued.
	 */
	FCFDVizResult EnqueueUpload(FFlowVizVolumeUpload&& Upload);

	/**
	 * Forget what is resident: every slot drops its frame, the display pins
	 * clear, and the uploaded layout goes invalid. The TEXTURES are left alone --
	 * they are re-created or updated in place by the next upload, and releasing
	 * them here would mean a render-thread round trip on every case change.
	 *
	 * CALL THIS WHEN THE MEANING OF A FRAME NUMBER CHANGES -- a different case,
	 * a different field, or an unload. Frame numbers are indices into whatever
	 * is currently bound, so a set that kept its bookkeeping across a rebind
	 * would answer FindSlotForFrame(3) with the PREVIOUS case's frame 3, and the
	 * shader would sample those voxels through the new case's transform. Both
	 * are real data, so the result is a plausible image of a case nobody loaded.
	 *
	 * An upload already in flight is not cancelled -- it cannot be. Its slot
	 * stays reserved so nothing else claims it, and lands as INDEX_NONE, so the
	 * stale frame it carries is never displayed.
	 */
	void InvalidateResidency();

	/** Enqueue release of every slot's textures. Safe to call when nothing was created. Flush rendering commands before destroying the set. */
	void ReleaseResources();

private:
	/** Render-thread body of EnqueueUpload. */
	void UploadOnRenderThread(FRHICommandListBase& RHICmdList, int32 SlotIndex, FFlowVizVolumeUpload&& Upload);

	/** Guards the slot arrays and every piece of bookkeeping derived from them. */
	mutable FCriticalSection SlotLock;

	TArray<FFlowVizVolumeSlotTextures> Slots;
	TArray<FFlowVizVolumeSlotState> SlotStates;

	int32 DisplayFrameA = INDEX_NONE;
	int32 DisplayFrameB = INDEX_NONE;

	/** See GetUploadedFieldLayout. Written by EnqueueUpload once the payload validates. */
	FFlowVizVolumeLayout UploadedFieldLayout;

	/** Stamped into FFlowVizVolumeSlotState::LastUseSerial. Monotonic; wrapping a uint64 is not a concern this side of the heat death. */
	uint64 UseSerial = 0;

	/** Prevents uploads or reinitialization from overtaking the queued release command. */
	bool bReleaseInFlight = false;
};

/* -------------------------------------------------------------------------- */
/* Constant-buffer layout assertions                                            */
/* -------------------------------------------------------------------------- */

/*
 * These pin FFlowVizVolumeShaderParameters against the HLSL constant buffer the
 * ray-marcher declares. They are not decoration: this module is a unity build,
 * so a sibling .cpp's constants share this translation unit, and the last time
 * offsets were left unasserted the result was a reader silently parsing with the
 * wrong ones (Docs/BUILD.md, commit 9d5d5ac). A struct whose members drift
 * produces a shader that reads spacing out of the dimensions slot - a wrong
 * volume that still renders.
 */
static_assert(sizeof(FVector3f) == 12, "FFlowVizVolumeShaderParameters assumes a packed 3-float vector.");
static_assert(sizeof(FIntVector) == 12, "FFlowVizVolumeShaderParameters assumes a packed 3-int vector.");

static_assert(offsetof(FFlowVizVolumeShaderParameters, GridOrigin) == 0, "cbuffer row 0");
static_assert(offsetof(FFlowVizVolumeShaderParameters, OriginNarrowingError) == 12, "cbuffer row 0");
static_assert(offsetof(FFlowVizVolumeShaderParameters, PhysicalSize) == 16, "cbuffer row 1");
static_assert(offsetof(FFlowVizVolumeShaderParameters, VoxelVolume) == 28, "cbuffer row 1");
static_assert(offsetof(FFlowVizVolumeShaderParameters, VoxelSpacing) == 32, "cbuffer row 2");
static_assert(offsetof(FFlowVizVolumeShaderParameters, MinVoxelSpacing) == 44, "cbuffer row 2");
static_assert(offsetof(FFlowVizVolumeShaderParameters, InvVoxelSpacing) == 48, "cbuffer row 3");
static_assert(offsetof(FFlowVizVolumeShaderParameters, MaxVoxelSpacing) == 60, "cbuffer row 3");
static_assert(offsetof(FFlowVizVolumeShaderParameters, VolumeDimensions) == 64, "cbuffer row 4");
static_assert(offsetof(FFlowVizVolumeShaderParameters, ComponentCount) == 76, "cbuffer row 4");
static_assert(offsetof(FFlowVizVolumeShaderParameters, InvVolumeDimensions) == 80, "cbuffer row 5");
static_assert(offsetof(FFlowVizVolumeShaderParameters, TextureComponentCount) == 92, "cbuffer row 5");
static_assert(offsetof(FFlowVizVolumeShaderParameters, UVWScale) == 96, "cbuffer row 6");
static_assert(offsetof(FFlowVizVolumeShaderParameters, AssociationCode) == 108, "cbuffer row 6");
static_assert(offsetof(FFlowVizVolumeShaderParameters, UVWBias) == 112, "cbuffer row 7");
static_assert(offsetof(FFlowVizVolumeShaderParameters, DataTypeCode) == 124, "cbuffer row 7");
static_assert(offsetof(FFlowVizVolumeShaderParameters, RequiredStatusMask) == 128, "cbuffer row 8");
static_assert(offsetof(FFlowVizVolumeShaderParameters, InvalidStatusMask) == 132, "cbuffer row 8");
static_assert(offsetof(FFlowVizVolumeShaderParameters, bHasStatusTexture) == 136, "cbuffer row 8");
static_assert(offsetof(FFlowVizVolumeShaderParameters, bHasVectorTexture) == 140, "cbuffer row 8");
static_assert(offsetof(FFlowVizVolumeShaderParameters, ValueRangeMin) == 144, "cbuffer row 9");
static_assert(offsetof(FFlowVizVolumeShaderParameters, ValueRangeMax) == 148, "cbuffer row 9");
static_assert(offsetof(FFlowVizVolumeShaderParameters, bRejectNonFinite) == 152, "cbuffer row 9");
static_assert(offsetof(FFlowVizVolumeShaderParameters, PadElementBits) == 156, "cbuffer row 9");
static_assert(sizeof(FFlowVizVolumeShaderParameters) == 160, "cbuffer total: ten 16-byte rows.");
