// Copyright FlowViz contributors. All Rights Reserved.

#include "Render/FlowVizNiagaraFeed.h"

#include "Engine/TextureRenderTargetVolume.h"
#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "TextureResource.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizNiagaraFeedLocal
{
	int32 VoxelIndex(const FIntVector& Counts, int32 X, int32 Y, int32 Z)
	{
		return (Z * Counts.Y + Y) * Counts.X + X;
	}
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
	OutTexels.SetNumUninitialized(OutCounts.X * OutCounts.Y * OutCounts.Z);

	TArray<double> Value;
	for (int32 Z = 0; Z < OutCounts.Z; ++Z)
	{
		for (int32 Y = 0; Y < OutCounts.Y; ++Y)
		{
			for (int32 X = 0; X < OutCounts.X; ++X)
			{
				const FIntVector Voxel(X, Y, Z);
				FFloat16Color& Texel = OutTexels[VoxelIndex(OutCounts, X, Y, Z)];
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
	UTextureRenderTargetVolume*& InOutTarget)
{
	TArray<FFloat16Color> Texels;
	FIntVector Counts;
	if (!PackVelocityMask(Sampler, Mask, Texels, Counts))
	{
		return false;
	}

	if (InOutTarget == nullptr
		|| InOutTarget->SizeX != Counts.X
		|| InOutTarget->SizeY != Counts.Y
		|| InOutTarget->SizeZ != Counts.Z)
	{
		InOutTarget = NewObject<UTextureRenderTargetVolume>(
			Outer != nullptr ? Outer : GetTransientPackage());
		InOutTarget->bCanCreateUAV = false;
		InOutTarget->OverrideFormat = PF_FloatRGBA;
		InOutTarget->ClearColor = FLinearColor(0, 0, 0, 0);
		InOutTarget->Init(Counts.X, Counts.Y, Counts.Z, PF_FloatRGBA);
		InOutTarget->UpdateResourceImmediate(true);
	}

	FTextureRenderTargetResource* Resource =
		InOutTarget->GameThread_GetRenderTargetResource();
	if (Resource == nullptr)
	{
		return false;
	}

	// One UpdateTexture3D on the render thread; the texel buffer rides the
	// lambda by move so nothing is shared.
	ENQUEUE_RENDER_COMMAND(FlowVizNiagaraFeedUpload)(
		[Resource, Counts, Texels = MoveTemp(Texels)](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Texture = Resource->GetRenderTargetTexture();
			if (Texture == nullptr)
			{
				return;
			}
			const FUpdateTextureRegion3D Region(
				0, 0, 0, 0, 0, 0, Counts.X, Counts.Y, Counts.Z);
			const uint32 RowPitch = Counts.X * sizeof(FFloat16Color);
			const uint32 SlicePitch = RowPitch * Counts.Y;
			RHICmdList.UpdateTexture3D(
				Texture, 0, Region, RowPitch, SlicePitch,
				reinterpret_cast<const uint8*>(Texels.GetData()));
		});
	return true;
}
