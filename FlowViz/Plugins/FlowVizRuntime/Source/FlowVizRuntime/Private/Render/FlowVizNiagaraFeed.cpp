// Copyright FlowViz contributors. All Rights Reserved.

#include "Render/FlowVizNiagaraFeed.h"

#include "Engine/TextureRenderTargetVolume.h"
#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Render/FlowVizVolumeTexture.h"
#include "TextureResource.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizNiagaraFeedLocal
{
	int64 VoxelIndex(const FIntVector& Counts, int32 X, int32 Y, int32 Z)
	{
		return (static_cast<int64>(Z) * Counts.Y + Y) * Counts.X + X;
	}
}

bool FlowVizNiagaraFeed::GetPackedVoxelCount(
	const FIntVector& Counts,
	int32& OutVoxelCount)
{
	OutVoxelCount = 0;
	if (Counts.X <= 0 || Counts.Y <= 0 || Counts.Z <= 0
		|| Counts.X > FlowVizVolume::MaxTextureDimension
		|| Counts.Y > FlowVizVolume::MaxTextureDimension
		|| Counts.Z > FlowVizVolume::MaxTextureDimension)
	{
		return false;
	}

	const int64 VoxelCount = static_cast<int64>(Counts.X) * Counts.Y * Counts.Z;
	if (VoxelCount > MAX_int32)
	{
		return false;
	}

	OutVoxelCount = static_cast<int32>(VoxelCount);
	return true;
}

bool FlowVizNiagaraFeed::PackVelocityMask(
	const FFlowVizFieldSampler& Sampler,
	const FFlowVizFieldMask& Mask,
	TArray<FFloat16Color>& OutTexels,
	FIntVector& OutCounts)
{
	using namespace FlowVizNiagaraFeedLocal;

	OutTexels.Reset();
	OutCounts = FIntVector::ZeroValue;

	if (!Sampler.IsBuilt() || Sampler.GetComponentCount() != 3)
	{
		return false;
	}

	OutCounts = Sampler.GetValueCounts();
	int32 VoxelCount = 0;
	if (!GetPackedVoxelCount(OutCounts, VoxelCount))
	{
		OutCounts = FIntVector::ZeroValue;
		return false;
	}
	OutTexels.SetNumUninitialized(VoxelCount);

	TArray<double> Value;
	for (int32 Z = 0; Z < OutCounts.Z; ++Z)
	{
		for (int32 Y = 0; Y < OutCounts.Y; ++Y)
		{
			for (int32 X = 0; X < OutCounts.X; ++X)
			{
				const FIntVector Voxel(X, Y, Z);
				const int64 Index = VoxelIndex(OutCounts, X, Y, Z);
				check(Index >= 0 && Index < OutTexels.Num());
				FFloat16Color& Texel = OutTexels[static_cast<int32>(Index)];
				if (Mask.IsVoxelMasked(Voxel) || !Sampler.GetVoxelValue(Voxel, Value))
				{
					/*
					 * MASKED = ZERO VELOCITY + ZERO ALPHA. The velocity must
					 * be a real number (a NaN texel poisons hardware
					 * filtering across the boundary), and zero is the one
					 * honest choice: a particle that reads a blended
					 * fluid/masked texel slows toward the wall instead of
					 * accelerating through it. The ALPHA channel is the
					 * authoritative kill switch, matching the CPU oracle's
					 * IsMaskedAt.
					 */
					Texel = FFloat16Color(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
					continue;
				}
				Texel = FFloat16Color(FLinearColor(
					static_cast<float>(Value[0]),
					static_cast<float>(Value[1]),
					static_cast<float>(Value[2]),
					1.0f));
			}
		}
	}
	return true;
}

bool FlowVizNiagaraFeed::UploadToRenderTarget(
	const FFlowVizFieldSampler& Sampler,
	const FFlowVizFieldMask& Mask,
	UObject* Outer,
	TStrongObjectPtr<UTextureRenderTargetVolume>& InOutTarget)
{
	TArray<FFloat16Color> Texels;
	FIntVector Counts;
	if (!PackVelocityMask(Sampler, Mask, Texels, Counts))
	{
		return false;
	}

	const uint64 RowPitch64 = static_cast<uint64>(Counts.X) * sizeof(FFloat16Color);
	const uint64 SlicePitch64 = RowPitch64 * static_cast<uint64>(Counts.Y);
	if (RowPitch64 > MAX_uint32 || SlicePitch64 > MAX_uint32)
	{
		return false;
	}
	const uint32 RowPitch = static_cast<uint32>(RowPitch64);
	const uint32 SlicePitch = static_cast<uint32>(SlicePitch64);

	UTextureRenderTargetVolume* Target = InOutTarget.Get();
	if (Target == nullptr
		|| Target->SizeX != Counts.X
		|| Target->SizeY != Counts.Y
		|| Target->SizeZ != Counts.Z)
	{
		InOutTarget.Reset(NewObject<UTextureRenderTargetVolume>(
			Outer != nullptr ? Outer : GetTransientPackage()));
		Target = InOutTarget.Get();
		if (Target == nullptr)
		{
			return false;
		}
		Target->bCanCreateUAV = false;
		Target->OverrideFormat = PF_FloatRGBA;
		Target->ClearColor = FLinearColor(0, 0, 0, 0);
		Target->Init(Counts.X, Counts.Y, Counts.Z, PF_FloatRGBA);
		Target->UpdateResourceImmediate(true);
	}

	FTextureRenderTargetResource* Resource =
		Target->GameThread_GetRenderTargetResource();
	if (Resource == nullptr)
	{
		return false;
	}

	// One UpdateTexture3D on the render thread; the texel buffer rides the
	// lambda by move so nothing is shared.
	ENQUEUE_RENDER_COMMAND(FlowVizNiagaraFeedUpload)(
		[Resource, Counts, RowPitch, SlicePitch,
			Texels = MoveTemp(Texels)](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Texture = Resource->GetRenderTargetTexture();
			if (Texture == nullptr)
			{
				return;
			}
			const FUpdateTextureRegion3D Region(
				0, 0, 0, 0, 0, 0, Counts.X, Counts.Y, Counts.Z);
			RHICmdList.UpdateTexture3D(
				Texture, 0, Region, RowPitch, SlicePitch,
				reinterpret_cast<const uint8*>(Texels.GetData()));
		});
	return true;
}
