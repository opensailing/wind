// Copyright FlowViz contributors. All Rights Reserved.

#include "Render/FlowVizVolumeTexture.h"

#include "CFDViz/CFDVizVolumeReader.h"
#include "Misc/ScopeLock.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "Runtime/Launch/Resources/Version.h"

/**
 * ADR 002 accepts a hand-written ray-marcher and names its cost: RHI resource
 * APIs churn between engine releases. The mitigation it names is that every
 * version-sensitive call lives in this file. This assert is the tripwire for
 * that promise - an engine upgrade must land here deliberately rather than
 * silently recompiling against changed semantics.
 *
 * When it fires: re-check FRHITextureCreateDesc::Create3D, the
 * FRHICommandListBase::CreateTexture / UpdateTexture3D signatures, and whether
 * UpdateTexture3D still interprets SourceRowPitch and SourceDepthPitch as BYTE
 * counts of the source buffer. Then move the version below.
 */
static_assert(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8,
	"FlowVizVolumeTexture.cpp is pinned to UE 5.8. Re-verify the RHI texture "
	"create/update API against the new engine, then update this assert. See "
	"the FlowVizVolumeRHI comment in FlowVizVolumeTexture.h.");

/* -------------------------------------------------------------------------- */
/* Local helpers                                                                */
/* -------------------------------------------------------------------------- */

/* NAMED namespace: unity build helpers must remain qualified. */
namespace FlowVizVolumeTextureLocal
{
	/** A failure whose message names the offending quantity and its value (engineering rule 12). */
	FCFDVizResult MakeFailure(ECFDVizError Error, FString Message)
	{
		return FCFDVizResult::Fail(Error, MoveTemp(Message));
	}

	/**
	 * Multiply into an int64, returning INDEX_NONE rather than wrapping.
	 *
	 * Every size in this file passes through here. A wrapped product turns a
	 * bounds check into a rubber stamp, and the sizes originate in a file this
	 * process did not write.
	 */
	int64 MultiplyChecked(int64 A, int64 B)
	{
		if (A < 0 || B < 0)
		{
			return INDEX_NONE;
		}
		int64 Product = 0;
		if (!CFDViz::TryMultiply(A, B, Product))
		{
			return INDEX_NONE;
		}
		return Product;
	}
}

/* -------------------------------------------------------------------------- */
/* Storage type -> texture format                                               */
/* -------------------------------------------------------------------------- */

FCFDVizResult FlowVizVolumeFormat::ChooseTextureFormat(
	ECFDVizDataType DataType,
	int32 SourceComponentCount,
	FFlowVizVolumeFormatChoice& OutChoice)
{
	OutChoice = FFlowVizVolumeFormatChoice();

	if (SourceComponentCount < 1 || SourceComponentCount > FlowVizVolume::MaxTextureComponents)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("component count %d is outside 1..%d"),
			SourceComponentCount, FlowVizVolume::MaxTextureComponents));
	}

	// Three components widen to four. No portable 3-channel 3D texture format
	// exists on any target this project ships to, and inventing one by packing
	// would change the stride the shader reads.
	const int32 TextureComponents = (SourceComponentCount == 3) ? 4 : SourceComponentCount;

	EPixelFormat Format = PF_Unknown;
	switch (DataType)
	{
	case ECFDVizDataType::Float16:
		Format = (TextureComponents == 1) ? PF_R16F
			: (TextureComponents == 2) ? PF_G16R16F
			: PF_FloatRGBA;
		break;

	case ECFDVizDataType::Float32:
		Format = (TextureComponents == 1) ? PF_R32_FLOAT
			: (TextureComponents == 2) ? PF_G32R32F
			: PF_A32B32G32R32F;
		break;

	case ECFDVizDataType::UInt8:
		// _UINT, never PF_R8 or PF_G8. Those are UNORM: the sampler would return
		// value/255 and a quantised field would arrive silently rescaled, which
		// is the normalisation format rule 1.6 forbids.
		Format = (TextureComponents == 1) ? PF_R8_UINT
			: (TextureComponents == 2) ? PF_R8G8_UINT
			: PF_R8G8B8A8_UINT;
		break;

	default:
		// float64 lands here. It is not a CVF storage type (section 3.3) and no
		// 3D texture format carries it; narrowing to float32 would be exactly
		// the silent quantisation rule 5 forbids.
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::UnsupportedDataType, FString::Printf(
			TEXT("data type %s has no 3D texture format"), DataTypeToString(DataType)));
	}

	OutChoice.PixelFormat = Format;
	OutChoice.TextureComponentCount = TextureComponents;
	OutChoice.bPadded = (TextureComponents > SourceComponentCount);
	return FCFDVizResult::Ok();
}

uint32 FlowVizVolumeFormat::GetPadElementBits(ECFDVizDataType DataType)
{
	switch (DataType)
	{
	case ECFDVizDataType::Float32:
		return 0x7FC00000u;   // quiet NaN, binary32
	case ECFDVizDataType::Float16:
		return 0x7E00u;       // quiet NaN, binary16
	default:
		return 0u;            // no NaN exists in an integer format
	}
}

/* -------------------------------------------------------------------------- */
/* Dense volume layout                                                          */
/* -------------------------------------------------------------------------- */

FCFDVizResult FFlowVizVolumeLayout::Make(
	const FIntVector& InExtent,
	int32 InSourceComponentCount,
	ECFDVizDataType InDataType,
	FFlowVizVolumeLayout& OutLayout,
	int64 MaxBytes)
{
	OutLayout = FFlowVizVolumeLayout();

	// Extent first: everything below multiplies by it, so a degenerate or
	// oversized axis must be rejected before any product is formed.
	if (InExtent.X < 1 || InExtent.Y < 1 || InExtent.Z < 1)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, FString::Printf(
			TEXT("volume extent (%d, %d, %d) has a non-positive axis"),
			InExtent.X, InExtent.Y, InExtent.Z));
	}
	if (InExtent.X > FlowVizVolume::MaxTextureDimension
		|| InExtent.Y > FlowVizVolume::MaxTextureDimension
		|| InExtent.Z > FlowVizVolume::MaxTextureDimension)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("volume extent (%d, %d, %d) exceeds the %d-voxel per-axis limit"),
			InExtent.X, InExtent.Y, InExtent.Z, FlowVizVolume::MaxTextureDimension));
	}

	FFlowVizVolumeFormatChoice Choice;
	const FCFDVizResult FormatResult =
		FlowVizVolumeFormat::ChooseTextureFormat(InDataType, InSourceComponentCount, Choice);
	if (!FormatResult.IsOk())
	{
		return FormatResult;
	}

	FFlowVizVolumeLayout Layout;
	Layout.Extent = InExtent;
	Layout.SourceComponentCount = InSourceComponentCount;
	Layout.TextureComponentCount = Choice.TextureComponentCount;
	Layout.DataType = InDataType;
	Layout.PixelFormat = Choice.PixelFormat;

	// The size check runs here, before any caller can allocate. A size check
	// that runs after the allocation it guards is decoration (rule 12).
	const int64 TextureBytes = Layout.GetTextureVolumeBytes();
	if (TextureBytes == INDEX_NONE)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("volume extent (%d, %d, %d) x %d channels overflows an int64 byte count"),
			InExtent.X, InExtent.Y, InExtent.Z, Layout.TextureComponentCount));
	}
	if (TextureBytes > MaxBytes)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("volume would need %lld bytes, over the %lld-byte limit"),
			TextureBytes, MaxBytes));
	}

	OutLayout = Layout;
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizVolumeLayout::MakeStatusLayout(
	const FIntVector& InExtent,
	FFlowVizVolumeLayout& OutLayout,
	int64 MaxBytes)
{
	// One uint8 channel per voxel. Going through Make means the status texture
	// gets the identical extent and overflow checks the field texture gets.
	return Make(InExtent, 1, ECFDVizDataType::UInt8, OutLayout, MaxBytes);
}

bool FFlowVizVolumeLayout::IsValid() const
{
	return Extent.X >= 1 && Extent.Y >= 1 && Extent.Z >= 1
		&& Extent.X <= FlowVizVolume::MaxTextureDimension
		&& Extent.Y <= FlowVizVolume::MaxTextureDimension
		&& Extent.Z <= FlowVizVolume::MaxTextureDimension
		&& SourceComponentCount >= 1
		&& SourceComponentCount <= FlowVizVolume::MaxTextureComponents
		&& TextureComponentCount >= SourceComponentCount
		&& TextureComponentCount <= FlowVizVolume::MaxTextureComponents
		&& PixelFormat != PF_Unknown
		&& GetElementBytes() > 0;
}

int64 FFlowVizVolumeLayout::GetVoxelCount() const
{
	if (Extent.X < 1 || Extent.Y < 1 || Extent.Z < 1)
	{
		return INDEX_NONE;
	}
	// X*Y as two int32s in an int64 cannot overflow; the third factor can.
	const int64 CountXY = static_cast<int64>(Extent.X) * static_cast<int64>(Extent.Y);
	return FlowVizVolumeTextureLocal::MultiplyChecked(CountXY, static_cast<int64>(Extent.Z));
}

int64 FFlowVizVolumeLayout::GetSourceRowPitch() const
{
	if (Extent.X < 1)
	{
		return INDEX_NONE;
	}
	return FlowVizVolumeTextureLocal::MultiplyChecked(static_cast<int64>(Extent.X), GetSourceVoxelBytes());
}

int64 FFlowVizVolumeLayout::GetSourceSlicePitch() const
{
	const int64 RowPitch = GetSourceRowPitch();
	if (RowPitch == INDEX_NONE || Extent.Y < 1)
	{
		return INDEX_NONE;
	}
	return FlowVizVolumeTextureLocal::MultiplyChecked(RowPitch, static_cast<int64>(Extent.Y));
}

int64 FFlowVizVolumeLayout::GetSourceVolumeBytes() const
{
	const int64 SlicePitch = GetSourceSlicePitch();
	if (SlicePitch == INDEX_NONE || Extent.Z < 1)
	{
		return INDEX_NONE;
	}
	return FlowVizVolumeTextureLocal::MultiplyChecked(SlicePitch, static_cast<int64>(Extent.Z));
}

int64 FFlowVizVolumeLayout::GetTextureRowPitch() const
{
	if (Extent.X < 1)
	{
		return INDEX_NONE;
	}
	return FlowVizVolumeTextureLocal::MultiplyChecked(static_cast<int64>(Extent.X), GetTextureVoxelBytes());
}

int64 FFlowVizVolumeLayout::GetTextureSlicePitch() const
{
	const int64 RowPitch = GetTextureRowPitch();
	if (RowPitch == INDEX_NONE || Extent.Y < 1)
	{
		return INDEX_NONE;
	}
	return FlowVizVolumeTextureLocal::MultiplyChecked(RowPitch, static_cast<int64>(Extent.Y));
}

int64 FFlowVizVolumeLayout::GetTextureVolumeBytes() const
{
	const int64 SlicePitch = GetTextureSlicePitch();
	if (SlicePitch == INDEX_NONE || Extent.Z < 1)
	{
		return INDEX_NONE;
	}
	return FlowVizVolumeTextureLocal::MultiplyChecked(SlicePitch, static_cast<int64>(Extent.Z));
}

int64 FFlowVizVolumeLayout::GetVoxelIndex(int32 I, int32 J, int32 K) const
{
	if (!Contains(I, J, K))
	{
		return INDEX_NONE;
	}
	// The volume already fits an int64 (Make checked it), so this cannot wrap.
	// X fastest, then Y, then Z - the CVF payload order (section 4.4.1).
	return static_cast<int64>(I)
		+ static_cast<int64>(Extent.X) * (static_cast<int64>(J) + static_cast<int64>(Extent.Y) * static_cast<int64>(K));
}

int64 FFlowVizVolumeLayout::GetSourceVoxelOffset(int32 I, int32 J, int32 K) const
{
	const int64 Index = GetVoxelIndex(I, J, K);
	if (Index == INDEX_NONE)
	{
		return INDEX_NONE;
	}
	return FlowVizVolumeTextureLocal::MultiplyChecked(Index, GetSourceVoxelBytes());
}

int64 FFlowVizVolumeLayout::GetTextureVoxelOffset(int32 I, int32 J, int32 K) const
{
	const int64 Index = GetVoxelIndex(I, J, K);
	if (Index == INDEX_NONE)
	{
		return INDEX_NONE;
	}
	return FlowVizVolumeTextureLocal::MultiplyChecked(Index, GetTextureVoxelBytes());
}

FString FFlowVizVolumeLayout::ToString() const
{
	return FString::Printf(
		TEXT("%dx%dx%d %s x%d (texture x%d, %s), %lld bytes"),
		Extent.X, Extent.Y, Extent.Z,
		DataTypeToString(DataType),
		SourceComponentCount,
		TextureComponentCount,
		GetPixelFormatString(PixelFormat),
		GetTextureVolumeBytes());
}

/* -------------------------------------------------------------------------- */
/* Brick placement                                                              */
/* -------------------------------------------------------------------------- */

FIntVector FlowVizVolumeBrick::GetBrickCounts(const FFlowVizVolumeLayout& Layout, const FIntVector& BrickSize)
{
	if (!Layout.IsValid() || BrickSize.X < 1 || BrickSize.Y < 1 || BrickSize.Z < 1)
	{
		return FIntVector(0, 0, 0);
	}
	return FIntVector(
		FMath::DivideAndRoundUp(Layout.Extent.X, BrickSize.X),
		FMath::DivideAndRoundUp(Layout.Extent.Y, BrickSize.Y),
		FMath::DivideAndRoundUp(Layout.Extent.Z, BrickSize.Z));
}

FCFDVizResult FlowVizVolumeBrick::MakePlacement(
	const FFlowVizVolumeLayout& Layout,
	const FIntVector& BrickCoordinate,
	const FIntVector& BrickSize,
	FFlowVizVolumeBrickPlacement& OutPlacement)
{
	OutPlacement = FFlowVizVolumeBrickPlacement();

	if (!Layout.IsValid())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("brick placement needs a valid volume layout"));
	}
	if (BrickSize.X < 1 || BrickSize.Y < 1 || BrickSize.Z < 1)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, FString::Printf(
			TEXT("brick size (%d, %d, %d) has a non-positive axis"),
			BrickSize.X, BrickSize.Y, BrickSize.Z));
	}

	const FIntVector Counts = GetBrickCounts(Layout, BrickSize);
	if (BrickCoordinate.X < 0 || BrickCoordinate.Y < 0 || BrickCoordinate.Z < 0
		|| BrickCoordinate.X >= Counts.X || BrickCoordinate.Y >= Counts.Y || BrickCoordinate.Z >= Counts.Z)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("brick coordinate (%d, %d, %d) is outside the %dx%dx%d tiling"),
			BrickCoordinate.X, BrickCoordinate.Y, BrickCoordinate.Z,
			Counts.X, Counts.Y, Counts.Z));
	}

	FFlowVizVolumeBrickPlacement Placement;
	Placement.DestOffset = FIntVector(
		BrickCoordinate.X * BrickSize.X,
		BrickCoordinate.Y * BrickSize.Y,
		BrickCoordinate.Z * BrickSize.Z);

	// EDGE BRICKS ARE NOT PADDED (section 4.4.3). The last brick on an axis
	// holds exactly the remainder; treating it as a full cube reads past the
	// decoded payload and shears every row after it.
	Placement.ValidSize = FIntVector(
		FMath::Min(BrickSize.X, Layout.Extent.X - Placement.DestOffset.X),
		FMath::Min(BrickSize.Y, Layout.Extent.Y - Placement.DestOffset.Y),
		FMath::Min(BrickSize.Z, Layout.Extent.Z - Placement.DestOffset.Z));

	// The brick's OWN strides, over its own valid size - not the volume's.
	const int64 VoxelBytes = Layout.GetSourceVoxelBytes();
	Placement.SourceRowPitch = FlowVizVolumeTextureLocal::MultiplyChecked(static_cast<int64>(Placement.ValidSize.X), VoxelBytes);
	Placement.SourceSlicePitch = FlowVizVolumeTextureLocal::MultiplyChecked(Placement.SourceRowPitch, static_cast<int64>(Placement.ValidSize.Y));
	Placement.SourceBytes = FlowVizVolumeTextureLocal::MultiplyChecked(Placement.SourceSlicePitch, static_cast<int64>(Placement.ValidSize.Z));

	if (Placement.SourceBytes == INDEX_NONE)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("brick (%d, %d, %d) byte count overflows"),
			BrickCoordinate.X, BrickCoordinate.Y, BrickCoordinate.Z));
	}

	OutPlacement = Placement;
	return FCFDVizResult::Ok();
}

FCFDVizResult FlowVizVolumeBrick::CopyBrickIntoDense(
	const FFlowVizVolumeLayout& Layout,
	const FFlowVizVolumeBrickPlacement& Placement,
	TArrayView<const uint8> BrickBytes,
	TArrayView<uint8> DenseBytes)
{
	if (!Layout.IsValid())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("brick copy needs a valid volume layout"));
	}

	// Both size checks run before the first write, so a rejected copy leaves the
	// destination untouched rather than partially filled - a partial copy would
	// leave a volume that looks complete.
	if (static_cast<int64>(BrickBytes.Num()) != Placement.SourceBytes)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("brick payload is %d bytes, expected %lld"),
			BrickBytes.Num(), Placement.SourceBytes));
	}

	const int64 DenseBytesNeeded = Layout.GetSourceVolumeBytes();
	if (DenseBytesNeeded == INDEX_NONE || static_cast<int64>(DenseBytes.Num()) < DenseBytesNeeded)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("dense buffer is %d bytes, needs at least %lld"),
			DenseBytes.Num(), DenseBytesNeeded));
	}

	const int64 VoxelBytes = Layout.GetSourceVoxelBytes();
	const int64 DenseRowPitch = Layout.GetSourceRowPitch();
	const int64 DenseSlicePitch = Layout.GetSourceSlicePitch();

	// Row by row. The brick's rows are contiguous within the brick and strided
	// within the volume, so this is the largest unit that is contiguous in both.
	for (int32 K = 0; K < Placement.ValidSize.Z; ++K)
	{
		for (int32 J = 0; J < Placement.ValidSize.Y; ++J)
		{
			const int64 SourceOffset =
				static_cast<int64>(K) * Placement.SourceSlicePitch
				+ static_cast<int64>(J) * Placement.SourceRowPitch;

			const int64 DestOffset =
				static_cast<int64>(Placement.DestOffset.Z + K) * DenseSlicePitch
				+ static_cast<int64>(Placement.DestOffset.Y + J) * DenseRowPitch
				+ static_cast<int64>(Placement.DestOffset.X) * VoxelBytes;

			FMemory::Memcpy(
				DenseBytes.GetData() + DestOffset,
				BrickBytes.GetData() + SourceOffset,
				static_cast<SIZE_T>(Placement.SourceRowPitch));
		}
	}

	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* Component widening                                                           */
/* -------------------------------------------------------------------------- */

FCFDVizResult FlowVizVolumeConvert::ExpandComponents(
	const FFlowVizVolumeLayout& Layout,
	TArrayView<const uint8> SourceBytes,
	TArray<uint8>& OutBytes)
{
	OutBytes.Empty();

	if (!Layout.IsValid())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("component widening needs a valid volume layout"));
	}

	const int64 SourceNeeded = Layout.GetSourceVolumeBytes();
	const int64 TextureNeeded = Layout.GetTextureVolumeBytes();
	if (SourceNeeded == INDEX_NONE || TextureNeeded == INDEX_NONE)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, TEXT("volume byte count overflows"));
	}
	if (static_cast<int64>(SourceBytes.Num()) != SourceNeeded)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("source volume is %d bytes, expected %lld"), SourceBytes.Num(), SourceNeeded));
	}
	if (TextureNeeded > FlowVizVolume::MaxUploadBytes)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("widened volume would need %lld bytes"), TextureNeeded));
	}

	OutBytes.SetNumUninitialized(static_cast<int32>(TextureNeeded));

	// No widening: a straight copy, so callers have one path.
	if (Layout.SourceComponentCount == Layout.TextureComponentCount)
	{
		FMemory::Memcpy(OutBytes.GetData(), SourceBytes.GetData(), static_cast<SIZE_T>(TextureNeeded));
		return FCFDVizResult::Ok();
	}

	const int32 ElementBytes = Layout.GetElementBytes();
	const int64 SourceVoxelBytes = Layout.GetSourceVoxelBytes();
	const int64 TextureVoxelBytes = Layout.GetTextureVoxelBytes();
	const int64 VoxelCount = Layout.GetVoxelCount();

	// The pad pattern, materialised once, little-endian. A quiet NaN so a shader
	// that forgets the ComponentCount gate produces a visibly broken result
	// rather than a plausible zero.
	const uint32 PadBits = FlowVizVolumeFormat::GetPadElementBits(Layout.DataType);
	uint8 PadPattern[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	for (int32 Byte = 0; Byte < ElementBytes && Byte < 4; ++Byte)
	{
		PadPattern[Byte] = static_cast<uint8>((PadBits >> (Byte * 8)) & 0xFFu);
	}

	const uint8* Source = SourceBytes.GetData();
	uint8* Dest = OutBytes.GetData();
	const int32 PadChannels = Layout.TextureComponentCount - Layout.SourceComponentCount;

	for (int64 Voxel = 0; Voxel < VoxelCount; ++Voxel)
	{
		// Stored components are copied byte for byte: no precision widening, no
		// scaling, no reordering. Only the added channel is synthesised.
		FMemory::Memcpy(
			Dest + Voxel * TextureVoxelBytes,
			Source + Voxel * SourceVoxelBytes,
			static_cast<SIZE_T>(SourceVoxelBytes));

		uint8* PadStart = Dest + Voxel * TextureVoxelBytes + SourceVoxelBytes;
		for (int32 Channel = 0; Channel < PadChannels; ++Channel)
		{
			FMemory::Memcpy(PadStart + Channel * ElementBytes, PadPattern, static_cast<SIZE_T>(ElementBytes));
		}
	}

	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* Per-voxel validity                                                           */
/* -------------------------------------------------------------------------- */

uint8 FlowVizVolumeStatus::ClassifyElement(const uint8* ElementBytes, ECFDVizDataType DataType)
{
	if (ElementBytes == nullptr)
	{
		return 0;
	}

	// Classification is on the STORED BITS. Going through a float conversion
	// would let a float16 NaN lose its payload on the round trip, and the answer
	// would then disagree with the bytes actually uploaded.
	switch (DataType)
	{
	case ECFDVizDataType::Float16:
	{
		const uint16 Bits = static_cast<uint16>(ElementBytes[0]) | (static_cast<uint16>(ElementBytes[1]) << 8);
		const uint16 Exponent = static_cast<uint16>((Bits >> 10) & 0x1Fu);
		const uint16 Mantissa = static_cast<uint16>(Bits & 0x3FFu);
		if (Exponent == 0x1Fu)
		{
			return (Mantissa != 0) ? FlowVizVoxelStatus::NaN : FlowVizVoxelStatus::Infinite;
		}
		return 0;
	}

	case ECFDVizDataType::Float32:
	{
		const uint32 Bits = static_cast<uint32>(ElementBytes[0])
			| (static_cast<uint32>(ElementBytes[1]) << 8)
			| (static_cast<uint32>(ElementBytes[2]) << 16)
			| (static_cast<uint32>(ElementBytes[3]) << 24);
		const uint32 Exponent = (Bits >> 23) & 0xFFu;
		const uint32 Mantissa = Bits & 0x7FFFFFu;
		if (Exponent == 0xFFu)
		{
			return (Mantissa != 0) ? FlowVizVoxelStatus::NaN : FlowVizVoxelStatus::Infinite;
		}
		return 0;
	}

	default:
		// An integer type has no NaN and no infinity, so every bit pattern is a
		// finite value - including 0xFF.
		return 0;
	}
}

FCFDVizResult FlowVizVolumeStatus::Build(
	const FFlowVizVolumeLayout& FieldLayout,
	const FFlowVizVolumeLayout& StatusLayout,
	const FFlowVizVolumeStatusSource& Source,
	TArray<uint8>& OutStatus)
{
	OutStatus.Empty();

	if (!FieldLayout.IsValid() || !StatusLayout.IsValid())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("status build needs two valid layouts"));
	}
	// A status volume built for the cell count while the field used the value
	// count differs by one voxel per axis; the resulting misalignment marks the
	// wrong cells invalid, which reads as noise in the data.
	if (!FieldLayout.HasSameExtent(StatusLayout))
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("status extent (%d, %d, %d) does not match the field's (%d, %d, %d)"),
			StatusLayout.Extent.X, StatusLayout.Extent.Y, StatusLayout.Extent.Z,
			FieldLayout.Extent.X, FieldLayout.Extent.Y, FieldLayout.Extent.Z));
	}

	const int64 VoxelCount = FieldLayout.GetVoxelCount();
	const int64 FieldNeeded = FieldLayout.GetSourceVolumeBytes();
	if (VoxelCount == INDEX_NONE || FieldNeeded == INDEX_NONE || VoxelCount > MAX_int32)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, TEXT("status volume byte count overflows"));
	}
	if (static_cast<int64>(Source.FieldBytes.Num()) != FieldNeeded)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("field volume is %d bytes, expected %lld (source stride)"),
			Source.FieldBytes.Num(), FieldNeeded));
	}

	// An empty mask means the case declared no grid.maskField, which is honest:
	// nothing rejected any cell. A mask of the WRONG length is a different thing
	// and must not be truncated - truncating would silently unmask the tail.
	const bool bHasMask = Source.MaskBytes.Num() > 0;
	if (bHasMask && static_cast<int64>(Source.MaskBytes.Num()) != VoxelCount)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("mask volume is %d bytes, expected %lld (one uint8 per voxel)"),
			Source.MaskBytes.Num(), VoxelCount));
	}

	OutStatus.SetNumUninitialized(static_cast<int32>(VoxelCount));

	const uint8* Field = Source.FieldBytes.GetData();
	const int32 ElementBytes = FieldLayout.GetElementBytes();
	const int32 ComponentCount = FieldLayout.SourceComponentCount;
	const int64 VoxelBytes = FieldLayout.GetSourceVoxelBytes();
	const ECFDVizDataType DataType = FieldLayout.DataType;

	for (int64 Voxel = 0; Voxel < VoxelCount; ++Voxel)
	{
		uint8 Status = 0;

		// A vector field is judged per voxel, not per component: a velocity with
		// one NaN component has neither a usable magnitude nor a usable
		// direction, so the whole voxel is marked.
		const uint8* VoxelBase = Field + Voxel * VoxelBytes;
		for (int32 Component = 0; Component < ComponentCount; ++Component)
		{
			Status |= ClassifyElement(VoxelBase + Component * ElementBytes, DataType);
		}

		if (bHasMask)
		{
			const bool bNonZero = Source.MaskBytes[static_cast<int32>(Voxel)] != 0;
			const bool bKeeps = Source.bNonZeroKeeps ? bNonZero : !bNonZero;
			if (!bKeeps)
			{
				Status |= FlowVizVoxelStatus::Masked;
			}
		}

		// Valid is set only when nothing objected. Written this way round rather
		// than as "start Valid and clear it" so that a component the loop above
		// forgot to examine leaves the voxel unmarked-invalid but ALSO
		// unmarked-valid, which fails closed.
		OutStatus[static_cast<int32>(Voxel)] = (Status == 0) ? FlowVizVoxelStatus::Valid : Status;
	}

	return FCFDVizResult::Ok();
}

FlowVizVolumeStatus::FCounts FlowVizVolumeStatus::CountStatus(TArrayView<const uint8> StatusBytes)
{
	FCounts Counts;
	Counts.Total = StatusBytes.Num();
	for (uint8 Status : StatusBytes)
	{
		if (Status == FlowVizVoxelStatus::Unknown)
		{
			++Counts.Unknown;
			continue;
		}
		// A voxel with several bits set is counted once per bit, because the
		// diagnostics panel reports masked and NaN separately and a voxel can
		// genuinely be both.
		Counts.Valid += ((Status & FlowVizVoxelStatus::Valid) != 0) ? 1 : 0;
		Counts.NaN += ((Status & FlowVizVoxelStatus::NaN) != 0) ? 1 : 0;
		Counts.Infinite += ((Status & FlowVizVoxelStatus::Infinite) != 0) ? 1 : 0;
		Counts.Masked += ((Status & FlowVizVoxelStatus::Masked) != 0) ? 1 : 0;
	}
	return Counts;
}

/* -------------------------------------------------------------------------- */
/* Physical <-> voxel <-> UVW                                                   */
/* -------------------------------------------------------------------------- */

bool FFlowVizVolumeTransform::IsValid() const
{
	if (!Grid.IsValid())
	{
		return false;
	}
	const FIntVector Counts = GetValueCounts();
	return Counts.X >= 1 && Counts.Y >= 1 && Counts.Z >= 1;
}

FVector FFlowVizVolumeTransform::PhysicalToVoxel(const FVector& SolverPosition) const
{
	const FVector Relative = (SolverPosition - Grid.Origin) / Grid.Spacing;

	// A cell value sits at the cell CENTRE, so cell centre i is at
	// (i + 0.5) * Spacing and must map back to exactly i. A point value sits ON
	// the grid point, so no offset. Omitting this shifts the entire field by
	// half a voxel - which renders plausibly and is wrong.
	if (Association == ECFDVizAssociation::Point)
	{
		return Relative;
	}
	return Relative - FVector(0.5);
}

FVector FFlowVizVolumeTransform::VoxelToPhysical(const FVector& VoxelCoordinate) const
{
	const FVector Offset = (Association == ECFDVizAssociation::Point)
		? VoxelCoordinate
		: VoxelCoordinate + FVector(0.5);
	return Grid.Origin + Offset * Grid.Spacing;
}

FVector FFlowVizVolumeTransform::PhysicalToTextureUVW(const FVector& SolverPosition) const
{
	const FIntVector Counts = GetValueCounts();
	if (Counts.X < 1 || Counts.Y < 1 || Counts.Z < 1)
	{
		return FVector::ZeroVector;
	}

	// (VoxelCoordinate + 0.5) / ValueCounts places voxel i at the CENTRE of
	// texel i, for either association. Per axis throughout - for the mock domain
	// the three divisors are 56, 28 and 6, and deriving one from another is
	// wrong there.
	const FVector Voxel = PhysicalToVoxel(SolverPosition);
	return FVector(
		(Voxel.X + 0.5) / static_cast<double>(Counts.X),
		(Voxel.Y + 0.5) / static_cast<double>(Counts.Y),
		(Voxel.Z + 0.5) / static_cast<double>(Counts.Z));
}

FVector FFlowVizVolumeTransform::TextureUVWToPhysical(const FVector& UVW) const
{
	const FIntVector Counts = GetValueCounts();
	if (Counts.X < 1 || Counts.Y < 1 || Counts.Z < 1)
	{
		return FVector::ZeroVector;
	}
	const FVector Voxel(
		UVW.X * static_cast<double>(Counts.X) - 0.5,
		UVW.Y * static_cast<double>(Counts.Y) - 0.5,
		UVW.Z * static_cast<double>(Counts.Z) - 0.5);
	return VoxelToPhysical(Voxel);
}

FMatrix FFlowVizVolumeTransform::GetLocalToUnrealTransform(double MetersToUnrealUnits) const
{
	// Local space is [0, PhysicalSize] with the grid's minimum corner at the
	// origin, so placing it means translating to the grid origin in SOLVER space
	// and then converting. Composing in that order keeps the axis and unit
	// conversion in exactly one implementation (rule 4, ADR 004).
	const FMatrix ToGridOrigin = FTranslationMatrix(Grid.Origin);
	return ToGridOrigin * MakeSolverToUnrealTransform(MetersToUnrealUnits);
}

double FFlowVizVolumeTransform::GetOriginNarrowingError() const
{
	// What float32 costs, per axis, reported rather than absorbed. Zero for a
	// small origin; metres for a kilometre-scale one.
	const double ErrorX = FMath::Abs(Grid.Origin.X - static_cast<double>(static_cast<float>(Grid.Origin.X)));
	const double ErrorY = FMath::Abs(Grid.Origin.Y - static_cast<double>(static_cast<float>(Grid.Origin.Y)));
	const double ErrorZ = FMath::Abs(Grid.Origin.Z - static_cast<double>(static_cast<float>(Grid.Origin.Z)));
	return FMath::Max3(ErrorX, ErrorY, ErrorZ);
}

FCFDVizResult FFlowVizVolumeTransform::MakeShaderParametersWithoutValueRange(
	const FFlowVizVolumeLayout& Layout,
	FFlowVizVolumeShaderParameters& OutParams) const
{
	// The degenerate domain, written explicitly and named at every call site.
	// See the header: this renders a uniform block, so it is only for parameter
	// blocks whose colours are never read.
	return MakeShaderParameters(Layout, FVector2D(0.0, 0.0), OutParams);
}

FCFDVizResult FFlowVizVolumeTransform::MakeShaderParameters(
	const FFlowVizVolumeLayout& Layout,
	const FVector2D& ValueRange,
	FFlowVizVolumeShaderParameters& OutParams) const
{
	if (!IsValid())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("shader parameters need a valid grid"));
	}
	if (!Layout.IsValid())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("shader parameters need a valid volume layout"));
	}

	// A texture sized from the cell count for a point-associated field is the
	// point/cell bug, and it renders as a plausibly-cropped volume. Rejected.
	const FIntVector Counts = GetValueCounts();
	if (Layout.Extent != Counts)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("texture extent (%d, %d, %d) does not match the %s-associated value counts (%d, %d, %d)"),
			Layout.Extent.X, Layout.Extent.Y, Layout.Extent.Z,
			AssociationToString(Association),
			Counts.X, Counts.Y, Counts.Z));
	}

	const FVector Spacing = Grid.Spacing;
	const FVector PhysicalSize = GetPhysicalSize();

	FFlowVizVolumeShaderParameters Params;

	// Row 0. The narrowing to float happens exactly once, here, and what it cost
	// travels with it so a caller knows GridOrigin must not be used for placement.
	Params.GridOrigin = FVector3f(
		static_cast<float>(Grid.Origin.X),
		static_cast<float>(Grid.Origin.Y),
		static_cast<float>(Grid.Origin.Z));
	Params.OriginNarrowingError = static_cast<float>(GetOriginNarrowingError());

	// Row 1.
	Params.PhysicalSize = FVector3f(
		static_cast<float>(PhysicalSize.X),
		static_cast<float>(PhysicalSize.Y),
		static_cast<float>(PhysicalSize.Z));
	Params.VoxelVolume = static_cast<float>(Spacing.X * Spacing.Y * Spacing.Z);

	// Rows 2 and 3. THREE INDEPENDENT NUMBERS. Min and max are over all three,
	// not over the first and last.
	Params.VoxelSpacing = FVector3f(
		static_cast<float>(Spacing.X),
		static_cast<float>(Spacing.Y),
		static_cast<float>(Spacing.Z));
	Params.MinVoxelSpacing = static_cast<float>(FMath::Min3(Spacing.X, Spacing.Y, Spacing.Z));
	Params.InvVoxelSpacing = FVector3f(
		static_cast<float>(1.0 / Spacing.X),
		static_cast<float>(1.0 / Spacing.Y),
		static_cast<float>(1.0 / Spacing.Z));
	Params.MaxVoxelSpacing = static_cast<float>(FMath::Max3(Spacing.X, Spacing.Y, Spacing.Z));

	// Rows 4 and 5. VALUE counts, so one larger per axis than the cell
	// dimensions for a point-associated field.
	Params.VolumeDimensions = Counts;
	Params.ComponentCount = Layout.SourceComponentCount;
	Params.InvVolumeDimensions = FVector3f(
		1.0f / static_cast<float>(Counts.X),
		1.0f / static_cast<float>(Counts.Y),
		1.0f / static_cast<float>(Counts.Z));
	Params.TextureComponentCount = Layout.TextureComponentCount;

	// Rows 6 and 7. UVW = LocalPos * UVWScale + UVWBias, where LocalPos is
	// [0, PhysicalSize].
	//
	// Cell:  LocalPos/Spacing is the continuous cell index, minus 0.5 is the
	//        voxel coordinate, plus 0.5 over the count is the texel centre - the
	//        two halves cancel, so the bias is ZERO and the scale is
	//        1/(Spacing*Count) = 1/PhysicalSize.
	// Point: nothing cancels, so the bias is half a texel over the value count,
	//        and the count is one larger per axis.
	const double ScaleX = 1.0 / (Spacing.X * static_cast<double>(Counts.X));
	const double ScaleY = 1.0 / (Spacing.Y * static_cast<double>(Counts.Y));
	const double ScaleZ = 1.0 / (Spacing.Z * static_cast<double>(Counts.Z));
	Params.UVWScale = FVector3f(
		static_cast<float>(ScaleX),
		static_cast<float>(ScaleY),
		static_cast<float>(ScaleZ));
	Params.AssociationCode = static_cast<uint32>(Association);

	if (Association == ECFDVizAssociation::Point)
	{
		Params.UVWBias = FVector3f(
			static_cast<float>(0.5 / static_cast<double>(Counts.X)),
			static_cast<float>(0.5 / static_cast<double>(Counts.Y)),
			static_cast<float>(0.5 / static_cast<double>(Counts.Z)));
	}
	else
	{
		Params.UVWBias = FVector3f::ZeroVector;
	}
	Params.DataTypeCode = static_cast<uint32>(Layout.DataType);

	// Row 8. A voxel must carry Valid to be sampled; anything in InvalidStatusMask
	// is "explicitly invalid" rather than "never written", so the shader can
	// colour masked and NaN differently from absent.
	Params.RequiredStatusMask = FlowVizVoxelStatus::Valid;
	Params.InvalidStatusMask =
		FlowVizVoxelStatus::NaN | FlowVizVoxelStatus::Infinite | FlowVizVoxelStatus::Masked;
	Params.bHasStatusTexture = 0;
	Params.bHasVectorTexture = 0;

	// Row 9. The colour domain is passed IN because a grid and a texture layout
	// do not carry one - only the manifest's per-field statistics do. Narrowed
	// to float here, alongside every other narrowing in this function.
	Params.ValueRangeMin = static_cast<float>(ValueRange.X);
	Params.ValueRangeMax = static_cast<float>(ValueRange.Y);

	// Non-finite rejection is meaningful for a float format and pointless for an
	// integer one, where no bit pattern is NaN.
	Params.bRejectNonFinite =
		(Layout.DataType == ECFDVizDataType::Float16 || Layout.DataType == ECFDVizDataType::Float32) ? 1u : 0u;
	Params.PadElementBits = FlowVizVolumeFormat::GetPadElementBits(Layout.DataType);

	OutParams = Params;
	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* Reader -> upload                                                             */
/* -------------------------------------------------------------------------- */

FCFDVizResult FlowVizVolumeBuild::BuildFieldBytes(
	FCFDVizVolumeReader& Reader,
	FFlowVizVolumeLayout& OutLayout,
	TArray<uint8>& OutBytes,
	int64 MaxBytes)
{
	OutLayout = FFlowVizVolumeLayout();
	OutBytes.Empty();

	if (!Reader.IsOpen())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("the volume reader is not open"));
	}

	const FCFDVizVolumeHeader& Header = Reader.GetHeader();

	// GetValueCounts applies the +1 for a point-associated field. Taking
	// Dimensions directly would size the texture one voxel short per axis, which
	// simply drops the last plane - a slightly cropped, perfectly plausible volume.
	const FIntVector Extent = Header.GetValueCounts();

	FFlowVizVolumeLayout Layout;
	const FCFDVizResult LayoutResult = FFlowVizVolumeLayout::Make(
		Extent, Header.ComponentCount, Header.DataType, Layout, MaxBytes);
	if (!LayoutResult.IsOk())
	{
		return LayoutResult;
	}

	// The budget is applied to the WIDENED size above and to the dense read
	// below. For a 3-component field the widened buffer is the larger of the
	// two, so checking only the read would let a case three-quarters of the
	// limit allocate past it.
	TArray<uint8> SourceBytes;
	const FCFDVizResult ReadResult = Reader.ReadDense(SourceBytes, MaxBytes);
	if (!ReadResult.IsOk())
	{
		return ReadResult;
	}

	const int64 SourceNeeded = Layout.GetSourceVolumeBytes();
	if (static_cast<int64>(SourceBytes.Num()) != SourceNeeded)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("the reader produced %d bytes, the layout expects %lld"),
			SourceBytes.Num(), SourceNeeded));
	}

	const FCFDVizResult ExpandResult =
		FlowVizVolumeConvert::ExpandComponents(Layout, SourceBytes, OutBytes);
	if (!ExpandResult.IsOk())
	{
		OutBytes.Empty();
		return ExpandResult;
	}

	OutLayout = Layout;
	return FCFDVizResult::Ok();
}

FCFDVizResult FlowVizVolumeBuild::BuildUpload(
	FCFDVizVolumeReader& FieldReader,
	FCFDVizVolumeReader* MaskReader,
	bool bAsVector,
	FFlowVizVolumeUpload& OutUpload,
	int64 MaxBytes)
{
	if (!FieldReader.IsOpen())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("the field reader is not open"));
	}

	if (FieldReader.GetHeader().Association != ECFDVizAssociation::Cell
		&& FieldReader.GetHeader().Association != ECFDVizAssociation::Point)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(
			ECFDVizError::InvalidHeader, TEXT("the field association is neither cell nor point"));
	}

	FFlowVizVolumeLayout FieldLayout;
	TArray<uint8> TextureBytes;
	const FCFDVizResult FieldResult =
		BuildFieldBytes(FieldReader, FieldLayout, TextureBytes, MaxBytes);
	if (!FieldResult.IsOk())
	{
		return FieldResult;
	}

	// The status build needs the SOURCE bytes, not the widened ones - the pad
	// channel is a synthesised NaN and classifying it would mark every voxel of
	// every 3-component field invalid.
	TArray<uint8> SourceBytes;
	const FCFDVizResult SourceResult = FieldReader.ReadDense(SourceBytes, MaxBytes);
	if (!SourceResult.IsOk())
	{
		return SourceResult;
	}

	TArray<uint8> MaskBytes;
	if (MaskReader != nullptr)
	{
		if (!MaskReader->IsOpen())
		{
			return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("the mask reader is not open"));
		}

		// A mask whose extent disagrees with the field's is REJECTED. Ignoring it
		// would silently downgrade a masked case to an unmasked one and render
		// solid geometry as fluid.
		const FIntVector MaskExtent = MaskReader->GetHeader().GetValueCounts();
		if (MaskExtent != FieldLayout.Extent)
		{
			return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
				TEXT("mask extent (%d, %d, %d) does not match the field's (%d, %d, %d)"),
				MaskExtent.X, MaskExtent.Y, MaskExtent.Z,
				FieldLayout.Extent.X, FieldLayout.Extent.Y, FieldLayout.Extent.Z));
		}
		if (MaskReader->GetHeader().ComponentCount != 1
			|| MaskReader->GetHeader().DataType != ECFDVizDataType::UInt8)
		{
			return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::UnsupportedDataType, FString::Printf(
				TEXT("the mask field must be a single uint8 component, got %d x %s"),
				MaskReader->GetHeader().ComponentCount,
				DataTypeToString(MaskReader->GetHeader().DataType)));
		}

		const FCFDVizResult MaskResult = MaskReader->ReadDense(MaskBytes, MaxBytes);
		if (!MaskResult.IsOk())
		{
			return MaskResult;
		}
	}

	FFlowVizVolumeLayout StatusLayout;
	const FCFDVizResult StatusLayoutResult =
		FFlowVizVolumeLayout::MakeStatusLayout(FieldLayout.Extent, StatusLayout, MaxBytes);
	if (!StatusLayoutResult.IsOk())
	{
		return StatusLayoutResult;
	}

	FFlowVizVolumeStatusSource StatusSource;
	StatusSource.FieldBytes = SourceBytes;
	StatusSource.MaskBytes = MaskBytes;

	TArray<uint8> StatusBytes;
	const FCFDVizResult StatusResult =
		FlowVizVolumeStatus::Build(FieldLayout, StatusLayout, StatusSource, StatusBytes);
	if (!StatusResult.IsOk())
	{
		return StatusResult;
	}

	// Built into a local and moved across only at the end, so a caller reusing
	// one payload object cannot upload a half-built frame after an error.
	FFlowVizVolumeUpload Upload;
	Upload.FrameIndex = static_cast<int32>(FieldReader.GetHeader().FrameIndex);
	Upload.Association = FieldReader.GetHeader().Association;
	Upload.SimulationTime = FieldReader.GetHeader().SimulationTime;
	if (bAsVector)
	{
		Upload.VectorLayout = FieldLayout;
		Upload.VectorBytes = MoveTemp(TextureBytes);
	}
	else
	{
		Upload.ScalarLayout = FieldLayout;
		Upload.ScalarBytes = MoveTemp(TextureBytes);
	}
	Upload.StatusLayout = StatusLayout;
	Upload.StatusBytes = MoveTemp(StatusBytes);

	const FCFDVizResult ValidateResult = Upload.Validate();
	if (!ValidateResult.IsOk())
	{
		return ValidateResult;
	}

	OutUpload = MoveTemp(Upload);
	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* One frame's upload payload                                                   */
/* -------------------------------------------------------------------------- */

FCFDVizResult FFlowVizVolumeUpload::Validate() const
{
	if (FrameIndex < 0)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::IndexOutOfRange, TEXT("upload payload has no frame index"));
	}
	if (Association != ECFDVizAssociation::Cell
		&& Association != ECFDVizAssociation::Point)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(
			ECFDVizError::InvalidHeader, TEXT("upload payload association is neither cell nor point"));
	}

	const bool bHasScalar = ScalarLayout.IsValid();
	const bool bHasVector = VectorLayout.IsValid();
	const bool bHasStatus = StatusLayout.IsValid();

	if (!bHasScalar && !bHasVector)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, TEXT("upload payload carries no field"));
	}

	// Each present buffer's length must equal its layout's TEXTURE byte count
	// exactly. Not "at least" - a longer buffer means the two disagree about the
	// stride, and the extra bytes would be silently ignored.
	struct FBuffer
	{
		const TCHAR* Name;
		const FFlowVizVolumeLayout* Layout;
		const TArray<uint8>* Bytes;
		bool bPresent;
	};
	const FBuffer Buffers[3] = {
		{ TEXT("scalar"), &ScalarLayout, &ScalarBytes, bHasScalar },
		{ TEXT("vector"), &VectorLayout, &VectorBytes, bHasVector },
		{ TEXT("status"), &StatusLayout, &StatusBytes, bHasStatus }
	};

	const FFlowVizVolumeLayout* Reference = nullptr;
	for (const FBuffer& Buffer : Buffers)
	{
		if (!Buffer.bPresent)
		{
			// An invalid layout with bytes attached is a caller that filled one
			// half of a pair; it must not pass as "absent".
			if (Buffer.Bytes->Num() != 0)
			{
				return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
					TEXT("the %s buffer holds %d bytes but its layout is not valid"),
					Buffer.Name, Buffer.Bytes->Num()));
			}
			continue;
		}

		const int64 Needed = Buffer.Layout->GetTextureVolumeBytes();
		if (Needed == INDEX_NONE || static_cast<int64>(Buffer.Bytes->Num()) != Needed)
		{
			return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
				TEXT("the %s buffer is %d bytes, its layout needs %lld"),
				Buffer.Name, Buffer.Bytes->Num(), Needed));
		}

		// One extent across every present layout. This is what catches a status
		// texture built for the cell count while the field used the value count:
		// the two differ by one voxel per axis and each is internally consistent.
		if (Reference == nullptr)
		{
			Reference = Buffer.Layout;
		}
		else if (!Reference->HasSameExtent(*Buffer.Layout))
		{
			return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
				TEXT("the %s layout's extent (%d, %d, %d) differs from (%d, %d, %d)"),
				Buffer.Name,
				Buffer.Layout->Extent.X, Buffer.Layout->Extent.Y, Buffer.Layout->Extent.Z,
				Reference->Extent.X, Reference->Extent.Y, Reference->Extent.Z));
		}
	}

	return FCFDVizResult::Ok();
}

int64 FFlowVizVolumeUpload::GetTotalBytes() const
{
	return static_cast<int64>(ScalarBytes.Num())
		+ static_cast<int64>(VectorBytes.Num())
		+ static_cast<int64>(StatusBytes.Num());
}

/* -------------------------------------------------------------------------- */
/* Buffer rotation                                                              */
/* -------------------------------------------------------------------------- */

int32 FlowVizVolumeRing::FindSlotForFrame(TArrayView<const FFlowVizVolumeSlotState> Slots, int32 FrameIndex)
{
	if (FrameIndex < 0)
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		// A slot mid-upload holds the PREVIOUS frame's voxels until the render
		// thread is done, so matching it would display the old frame under the
		// new frame's label.
		if (Slots[Index].FrameIndex == FrameIndex && !Slots[Index].bUploadInFlight)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

int32 FlowVizVolumeRing::FindMostRecentSlot(TArrayView<const FFlowVizVolumeSlotState> Slots)
{
	int32 Best = INDEX_NONE;
	uint64 BestSerial = 0;
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		// Same exclusion as FindSlotForFrame: a slot mid-upload holds voxels
		// that are being replaced under the sampler.
		if (Slots[Index].FrameIndex == INDEX_NONE || Slots[Index].bUploadInFlight)
		{
			continue;
		}
		if (Best == INDEX_NONE || Slots[Index].LastUseSerial > BestSerial)
		{
			Best = Index;
			BestSerial = Slots[Index].LastUseSerial;
		}
	}
	return Best;
}

int32 FlowVizVolumeRing::ChooseUploadSlot(
	TArrayView<const FFlowVizVolumeSlotState> Slots,
	int32 FrameIndex,
	int32 PinnedFrameA,
	int32 PinnedFrameB)
{
	if (FrameIndex < 0 || Slots.Num() == 0)
	{
		return INDEX_NONE;
	}

	// 1. A slot already holding this frame - re-uploading in place costs nothing
	//    and evicts nobody.
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (Slots[Index].FrameIndex == FrameIndex && !Slots[Index].bUploadInFlight)
		{
			return Index;
		}
	}

	// 2. Any never-filled slot, before evicting anything at all.
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (Slots[Index].FrameIndex == INDEX_NONE && !Slots[Index].bUploadInFlight)
		{
			return Index;
		}
	}

	// 3. The least recently used slot that is neither displayed nor uploading.
	//    Both displayed frames are read every frame during interpolation, so
	//    evicting either tears the visible image.
	int32 Best = INDEX_NONE;
	uint64 BestSerial = 0;
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		const FFlowVizVolumeSlotState& Slot = Slots[Index];
		if (Slot.bUploadInFlight)
		{
			continue;
		}
		const bool bPinned =
			(PinnedFrameA != INDEX_NONE && Slot.FrameIndex == PinnedFrameA)
			|| (PinnedFrameB != INDEX_NONE && Slot.FrameIndex == PinnedFrameB);
		if (bPinned)
		{
			continue;
		}
		if (Best == INDEX_NONE || Slot.LastUseSerial < BestSerial)
		{
			Best = Index;
			BestSerial = Slot.LastUseSerial;
		}
	}

	// INDEX_NONE is a real and expected answer for a two-buffer set
	// mid-interpolation. The caller must defer the prefetch, not evict a
	// displayed frame.
	return Best;
}

/* -------------------------------------------------------------------------- */
/* Version-isolated RHI wrapper                                                 */
/* -------------------------------------------------------------------------- */

FCFDVizResult FlowVizVolumeRHI::CheckDeviceSupport(const FFlowVizVolumeLayout& Layout)
{
	if (!Layout.IsValid())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("device support needs a valid volume layout"));
	}

	// No device to fail against, so there is nothing to report. GUsingNullRHI is
	// checked as well as GIsRHIInitialized because the null RHI DOES set
	// GIsRHIInitialized while leaving the pixel-format capability table empty -
	// judging a format against that table would declare every format
	// unsupported, and every logic test would fail for a reason that has nothing
	// to do with the logic.
	if (!GIsRHIInitialized || GUsingNullRHI)
	{
		return FCFDVizResult::Ok();
	}

	const int32 MaxDimension = GMaxVolumeTextureDimensions;
	if (MaxDimension > 0
		&& (Layout.Extent.X > MaxDimension || Layout.Extent.Y > MaxDimension || Layout.Extent.Z > MaxDimension))
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("volume extent (%d, %d, %d) exceeds this device's %d-voxel 3D texture limit"),
			Layout.Extent.X, Layout.Extent.Y, Layout.Extent.Z, MaxDimension));
	}

	if (!UE::PixelFormat::HasCapabilities(Layout.PixelFormat, EPixelFormatCapabilities::Texture3D))
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::UnsupportedDataType, FString::Printf(
			TEXT("this device cannot create %s as a 3D texture"),
			GetPixelFormatString(Layout.PixelFormat)));
	}

	return FCFDVizResult::Ok();
}

FTextureRHIRef FlowVizVolumeRHI::CreateVolumeTexture(
	FRHICommandListBase& RHICmdList,
	const FFlowVizVolumeLayout& Layout,
	const TCHAR* DebugName,
	FCFDVizResult& OutResult)
{
	OutResult = CheckDeviceSupport(Layout);
	if (!OutResult.IsOk())
	{
		// A malformed case must produce a message, not a crash.
		return FTextureRHIRef();
	}

	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create3D(
			DebugName != nullptr ? DebugName : TEXT("FlowVizVolume"),
			Layout.Extent,
			Layout.PixelFormat)
		.SetFlags(ETextureCreateFlags::ShaderResource);

	FTextureRHIRef Texture = RHICmdList.CreateTexture(Desc);
	if (!Texture.IsValid())
	{
		OutResult = FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("the RHI refused a %s 3D texture"), *Layout.ToString()));
	}
	return Texture;
}

FCFDVizResult FlowVizVolumeRHI::UpdateVolumeTexture(
	FRHICommandListBase& RHICmdList,
	FRHITexture* Texture,
	const FFlowVizVolumeLayout& Layout,
	TArrayView<const uint8> TextureBytes)
{
	if (Texture == nullptr)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("volume update needs a texture"));
	}
	if (!Layout.IsValid())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("volume update needs a valid layout"));
	}

	// Checked here rather than inside the RHI call: UpdateTexture3D has no idea
	// how long the source buffer is and will happily walk off the end.
	const int64 Needed = Layout.GetTextureVolumeBytes();
	if (Needed == INDEX_NONE || static_cast<int64>(TextureBytes.Num()) != Needed)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("upload buffer is %d bytes, the texture needs %lld"),
			TextureBytes.Num(), Needed));
	}

	const FUpdateTextureRegion3D Region(
		0, 0, 0,
		0, 0, 0,
		static_cast<uint32>(Layout.Extent.X),
		static_cast<uint32>(Layout.Extent.Y),
		static_cast<uint32>(Layout.Extent.Z));

	RHICmdList.UpdateTexture3D(
		Texture,
		/*MipIndex*/ 0,
		Region,
		static_cast<uint32>(Layout.GetTextureRowPitch()),
		static_cast<uint32>(Layout.GetTextureSlicePitch()),
		TextureBytes.GetData());

	return FCFDVizResult::Ok();
}

FCFDVizResult FlowVizVolumeRHI::UpdateVolumeTextureRegion(
	FRHICommandListBase& RHICmdList,
	FRHITexture* Texture,
	const FFlowVizVolumeLayout& Layout,
	const FFlowVizVolumeBrickPlacement& Placement,
	TArrayView<const uint8> RegionBytes)
{
	if (Texture == nullptr)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("region update needs a texture"));
	}
	if (!Layout.IsValid())
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("region update needs a valid layout"));
	}
	if (Placement.ValidSize.X < 1 || Placement.ValidSize.Y < 1 || Placement.ValidSize.Z < 1)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::IndexOutOfRange, TEXT("region update needs a non-degenerate placement"));
	}
	if (Placement.DestOffset.X + Placement.ValidSize.X > Layout.Extent.X
		|| Placement.DestOffset.Y + Placement.ValidSize.Y > Layout.Extent.Y
		|| Placement.DestOffset.Z + Placement.ValidSize.Z > Layout.Extent.Z)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::IndexOutOfRange, TEXT("region update reaches outside the texture"));
	}

	// The region's own strides, in TEXTURE stride and tightly packed - not the
	// whole volume's. Using the volume's would read the wrong rows.
	const int64 RegionRowPitch = FlowVizVolumeTextureLocal::MultiplyChecked(static_cast<int64>(Placement.ValidSize.X), Layout.GetTextureVoxelBytes());
	const int64 RegionSlicePitch = FlowVizVolumeTextureLocal::MultiplyChecked(RegionRowPitch, static_cast<int64>(Placement.ValidSize.Y));
	const int64 RegionBytesNeeded = FlowVizVolumeTextureLocal::MultiplyChecked(RegionSlicePitch, static_cast<int64>(Placement.ValidSize.Z));
	if (RegionBytesNeeded == INDEX_NONE || static_cast<int64>(RegionBytes.Num()) != RegionBytesNeeded)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::SizeMismatch, FString::Printf(
			TEXT("region buffer is %d bytes, expected %lld"),
			RegionBytes.Num(), RegionBytesNeeded));
	}

	const FUpdateTextureRegion3D Region(
		static_cast<uint32>(Placement.DestOffset.X),
		static_cast<uint32>(Placement.DestOffset.Y),
		static_cast<uint32>(Placement.DestOffset.Z),
		0, 0, 0,
		static_cast<uint32>(Placement.ValidSize.X),
		static_cast<uint32>(Placement.ValidSize.Y),
		static_cast<uint32>(Placement.ValidSize.Z));

	RHICmdList.UpdateTexture3D(
		Texture,
		/*MipIndex*/ 0,
		Region,
		static_cast<uint32>(RegionRowPitch),
		static_cast<uint32>(RegionSlicePitch),
		RegionBytes.GetData());

	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* The multi-buffered resource                                                  */
/* -------------------------------------------------------------------------- */

FFlowVizVolumeTextureSet::~FFlowVizVolumeTextureSet()
{
	// The owner is required to have flushed rendering commands; this is the last
	// line of defence, dropping the references on whatever thread runs the
	// destructor rather than leaking them.
	FScopeLock Lock(&SlotLock);
	Slots.Empty();
	SlotStates.Empty();
}

FCFDVizResult FFlowVizVolumeTextureSet::Initialize(int32 NumBuffers)
{
	if (NumBuffers < FlowVizVolume::MinBufferCount || NumBuffers > FlowVizVolume::MaxBufferCount)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("buffer count %d is outside %d..%d"),
			NumBuffers, FlowVizVolume::MinBufferCount, FlowVizVolume::MaxBufferCount));
	}

	FScopeLock Lock(&SlotLock);
	if (bReleaseInFlight)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(
			ECFDVizError::AllocationTooLarge,
			TEXT("the texture set cannot be reinitialised while its resources are being released"));
	}
	for (const FFlowVizVolumeSlotState& State : SlotStates)
	{
		if (State.bUploadInFlight)
		{
			return FlowVizVolumeTextureLocal::MakeFailure(
				ECFDVizError::AllocationTooLarge,
				TEXT("the texture set cannot be reinitialised while an upload owns a slot"));
		}
	}
	for (const FFlowVizVolumeSlotTextures& Slot : Slots)
	{
		if (Slot.ScalarTexture.IsValid()
			|| Slot.VectorTexture.IsValid()
			|| Slot.StatusTexture.IsValid())
		{
			return FlowVizVolumeTextureLocal::MakeFailure(
				ECFDVizError::InvalidHeader,
				TEXT("release the texture set's RHI resources before reinitialising it"));
		}
	}

	// No RHI work here, so this is safe from any thread and is what a test
	// without a device exercises. SlotLock keeps a queued upload from retaining an
	// index into arrays that are about to be reallocated.
	Slots.Empty();
	Slots.SetNum(NumBuffers);
	SlotStates.Empty();
	SlotStates.SetNum(NumBuffers);
	DisplayFrameA = INDEX_NONE;
	DisplayFrameB = INDEX_NONE;
	UploadedFieldLayout = FFlowVizVolumeLayout();
	UseSerial = 0;
	return FCFDVizResult::Ok();
}

int32 FFlowVizVolumeTextureSet::GetBufferCount() const
{
	FScopeLock Lock(&SlotLock);
	return Slots.Num();
}

TArray<FFlowVizVolumeSlotState> FFlowVizVolumeTextureSet::GetSlotStates() const
{
	FScopeLock Lock(&SlotLock);
	return SlotStates;
}

bool FFlowVizVolumeTextureSet::GetSlotTextures(
	int32 SlotIndex,
	FFlowVizVolumeSlotTextures& OutTextures) const
{
	FScopeLock Lock(&SlotLock);
	if (!Slots.IsValidIndex(SlotIndex))
	{
		OutTextures = FFlowVizVolumeSlotTextures();
		return false;
	}

	OutTextures = Slots[SlotIndex];
	return true;
}

void FFlowVizVolumeTextureSet::SetDisplayFrames(int32 FrameA, int32 FrameB)
{
	FScopeLock Lock(&SlotLock);
	DisplayFrameA = FrameA;
	DisplayFrameB = FrameB;
}

int32 FFlowVizVolumeTextureSet::GetDisplayFrameA() const
{
	FScopeLock Lock(&SlotLock);
	return DisplayFrameA;
}

int32 FFlowVizVolumeTextureSet::GetDisplayFrameB() const
{
	FScopeLock Lock(&SlotLock);
	return DisplayFrameB;
}

FFlowVizVolumeLayout FFlowVizVolumeTextureSet::GetUploadedFieldLayout() const
{
	FScopeLock Lock(&SlotLock);
	return UploadedFieldLayout;
}

int32 FFlowVizVolumeTextureSet::FindSlotForFrame(int32 FrameIndex) const
{
	FScopeLock Lock(&SlotLock);
	const int32 Slot = FlowVizVolumeRing::FindSlotForFrame(SlotStates, FrameIndex);
	// A slot can be bookkeeping-resident and still hold no content if its first
	// upload has not completed; such a slot must not be sampled.
	if (Slot != INDEX_NONE && Slots.IsValidIndex(Slot) && !Slots[Slot].bHasContent)
	{
		return INDEX_NONE;
	}
	return Slot;
}

int32 FFlowVizVolumeTextureSet::FindMostRecentResidentSlot() const
{
	FScopeLock Lock(&SlotLock);
	const int32 Slot = FlowVizVolumeRing::FindMostRecentSlot(SlotStates);
	// The same content guard as FindSlotForFrame: bookkeeping-resident with no
	// completed upload must not be sampled.
	if (Slot != INDEX_NONE && Slots.IsValidIndex(Slot) && !Slots[Slot].bHasContent)
	{
		return INDEX_NONE;
	}
	return Slot;
}

int32 FFlowVizVolumeTextureSet::PeekUploadSlot(int32 FrameIndex) const
{
	FScopeLock Lock(&SlotLock);
	return FlowVizVolumeRing::ChooseUploadSlot(SlotStates, FrameIndex, DisplayFrameA, DisplayFrameB);
}

void FFlowVizVolumeTextureSet::InvalidateResidency()
{
	FScopeLock Lock(&SlotLock);
	for (FFlowVizVolumeSlotState& State : SlotStates)
	{
		/*
		 * AN IN-FLIGHT SLOT KEEPS ITS RESERVATION. Its render command is already
		 * queued and cannot be recalled; clearing bUploadInFlight here would let
		 * ChooseUploadSlot hand the same slot to the next upload, and two
		 * commands would write one texture in an order neither controls.
		 *
		 * FrameIndex is cleared eitherway, so the stale frame is never found by
		 * FindSlotForFrame in the meantime. The in-flight upload deliberately does
		 * not re-stamp its frame when it lands, so the old case remains unreachable
		 * and the now-unpinned slot is the first thing the new case can evict.
		 */
		State.FrameIndex = INDEX_NONE;
	}

	// Nothing resident means nothing to pin. Leaving these set would protect
	// slots that no longer hold what they name from eviction.
	DisplayFrameA = INDEX_NONE;
	DisplayFrameB = INDEX_NONE;

	// The next upload records the new field's shape. Until then there is no
	// layout to describe, which is what TryMakeShaderParameters reads to decide
	// there is nothing to march.
	UploadedFieldLayout = FFlowVizVolumeLayout();
}

FCFDVizResult FFlowVizVolumeTextureSet::EnqueueUpload(FFlowVizVolumeUpload&& Upload)
{
	const FCFDVizResult ValidateResult = Upload.Validate();
	if (!ValidateResult.IsOk())
	{
		// Nothing is queued, so a bad payload never reaches the render thread.
		return ValidateResult;
	}

	FScopeLock Lock(&SlotLock);
	if (bReleaseInFlight)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(
			ECFDVizError::AllocationTooLarge,
			TEXT("the texture set is releasing its resources"));
	}
	if (Slots.Num() == 0)
	{
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::InvalidHeader, TEXT("the texture set has not been initialised"));
	}

	const int32 SlotIndex =
		FlowVizVolumeRing::ChooseUploadSlot(SlotStates, Upload.FrameIndex, DisplayFrameA, DisplayFrameB);
	if (SlotIndex == INDEX_NONE)
	{
		// "Retry after the display frames advance", not an error to surface.
		return FlowVizVolumeTextureLocal::MakeFailure(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("every one of the %d buffers is pinned or busy"), Slots.Num()));
	}

	/*
	 * THE FIELD'S SHAPE, recorded here rather than by the caller.
	 *
	 * Whichever texture the payload filled: Validate has already established
	 * that at least one of the two is present and that its bytes match its
	 * layout, so this cannot record a shape nothing was uploaded for.
	 *
	 * AFTER the slot is chosen, so a payload refused for want of a free slot
	 * leaves it alone -- that refusal means "retry once the display advances",
	 * and adopting the layout of a frame that was never queued would be a claim
	 * about voxels this set does not have.
	 */
	UploadedFieldLayout = Upload.VectorLayout.IsValid() ? Upload.VectorLayout : Upload.ScalarLayout;

	// Reservation and command insertion are one locked operation. ReleaseResources
	// takes the same lock, so its command cannot overtake an upload whose slot is
	// already marked busy.
	SlotStates[SlotIndex].bUploadInFlight = true;
	SlotStates[SlotIndex].FrameIndex = Upload.FrameIndex;
	SlotStates[SlotIndex].LastUseSerial = ++UseSerial;

	FFlowVizVolumeTextureSet* Self = this;
	FFlowVizVolumeUpload Payload = MoveTemp(Upload);
	ENQUEUE_RENDER_COMMAND(FlowVizVolumeUpload)(
		[Self, SlotIndex, Payload = MoveTemp(Payload)](FRHICommandListImmediate& RHICmdList) mutable
		{
			Self->UploadOnRenderThread(RHICmdList, SlotIndex, MoveTemp(Payload));
		});

	return FCFDVizResult::Ok();
}

void FFlowVizVolumeTextureSet::UploadOnRenderThread(
	FRHICommandListBase& RHICmdList,
	int32 SlotIndex,
	FFlowVizVolumeUpload&& Upload)
{
	check(IsInRenderingThread());
	FScopeLock Lock(&SlotLock);
	if (!Slots.IsValidIndex(SlotIndex))
	{
		return;
	}

	FFlowVizVolumeSlotTextures& Slot = Slots[SlotIndex];
	bool bAllOk = true;

	struct FTarget
	{
		FTextureRHIRef* Texture;
		FFlowVizVolumeLayout* SlotLayout;
		const FFlowVizVolumeLayout* PayloadLayout;
		const TArray<uint8>* Bytes;
		const TCHAR* DebugName;
	};
	const FTarget Targets[3] = {
		{ &Slot.ScalarTexture, &Slot.ScalarLayout, &Upload.ScalarLayout, &Upload.ScalarBytes, TEXT("FlowVizVolumeScalar") },
		{ &Slot.VectorTexture, &Slot.VectorLayout, &Upload.VectorLayout, &Upload.VectorBytes, TEXT("FlowVizVolumeVector") },
		{ &Slot.StatusTexture, &Slot.StatusLayout, &Upload.StatusLayout, &Upload.StatusBytes, TEXT("FlowVizVolumeStatus") }
	};

	for (const FTarget& Target : Targets)
	{
		if (!Target.PayloadLayout->IsValid())
		{
			continue;
		}

		// Created ONCE per slot and updated in place for the life of the case
		// (rule 2). A layout change is the only reason to release and recreate.
		const bool bNeedsCreate =
			!Target.Texture->IsValid()
			|| Target.SlotLayout->Extent != Target.PayloadLayout->Extent
			|| Target.SlotLayout->PixelFormat != Target.PayloadLayout->PixelFormat;

		if (bNeedsCreate)
		{
			FCFDVizResult CreateResult;
			*Target.Texture = FlowVizVolumeRHI::CreateVolumeTexture(
				RHICmdList, *Target.PayloadLayout, Target.DebugName, CreateResult);
			if (!CreateResult.IsOk())
			{
				CreateResult.LogIfFailed();
				bAllOk = false;
				continue;
			}
			*Target.SlotLayout = *Target.PayloadLayout;
		}

		const FCFDVizResult UpdateResult = FlowVizVolumeRHI::UpdateVolumeTexture(
			RHICmdList, Target.Texture->GetReference(), *Target.PayloadLayout, *Target.Bytes);
		if (!UpdateResult.IsOk())
		{
			UpdateResult.LogIfFailed();
			bAllOk = false;
		}
	}

	Slot.Association = Upload.Association;
	Slot.SimulationTime = Upload.SimulationTime;
	SlotStates[SlotIndex].bUploadInFlight = false;

	if (bAllOk)
	{
		Slot.bHasContent = true;
	}
	else
	{
		// A slot whose upload failed must not be displayed. Marking it
		// frameless means FindSlotForFrame never returns it, so the renderer
		// draws nothing there rather than the previous frame under this frame's
		// label - which would be a plausible, wrong image.
		SlotStates[SlotIndex].FrameIndex = INDEX_NONE;
	}
}

void FFlowVizVolumeTextureSet::ReleaseResources()
{
	FScopeLock Lock(&SlotLock);
	if (Slots.Num() == 0 || bReleaseInFlight)
	{
		return;
	}

	// Set before enqueueing and under the same lock EnqueueUpload takes. Every
	// upload that already owns a slot is therefore ahead of this command, and no
	// later upload can appear behind a release the caller expects to be final.
	bReleaseInFlight = true;
	FFlowVizVolumeTextureSet* Self = this;
	ENQUEUE_RENDER_COMMAND(FlowVizVolumeRelease)(
		[Self](FRHICommandListImmediate&)
		{
			check(IsInRenderingThread());
			FScopeLock RenderLock(&Self->SlotLock);
			for (FFlowVizVolumeSlotTextures& Slot : Self->Slots)
			{
				Slot.ScalarTexture.SafeRelease();
				Slot.VectorTexture.SafeRelease();
				Slot.StatusTexture.SafeRelease();
				Slot.ScalarLayout = FFlowVizVolumeLayout();
				Slot.VectorLayout = FFlowVizVolumeLayout();
				Slot.StatusLayout = FFlowVizVolumeLayout();
				Slot.Association = ECFDVizAssociation::Cell;
				Slot.SimulationTime = 0.0;
				Slot.bHasContent = false;
			}
			for (FFlowVizVolumeSlotState& State : Self->SlotStates)
			{
				State = FFlowVizVolumeSlotState();
			}
			Self->DisplayFrameA = INDEX_NONE;
			Self->DisplayFrameB = INDEX_NONE;
			Self->UploadedFieldLayout = FFlowVizVolumeLayout();
			Self->UseSerial = 0;
			Self->bReleaseInFlight = false;
		});
}
