// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class FFlowVizFieldMask;
class FFlowVizFieldSampler;
class UTextureRenderTargetVolume;

/**
 * The field-to-Niagara bridge (renderer overhaul P8; architecture brief's
 * "Flow path").
 *
 * Niagara's stock volume-texture data interface reads render targets, not
 * the raw RHI textures the ray-marcher owns -- so the feed fills a
 * UTextureRenderTargetVolume (RGBA16F: xyz = solver velocity, w = fluid
 * mask 0/1) from the CPU-decoded sampler the workspace already builds per
 * frame. GPU particles then advect by sampling it, with the mask channel
 * as their kill switch -- the same contract FlowVizParticles::
 * AdvanceParticles implements on the CPU, which remains the tested oracle.
 *
 * PACKING IS PURE AND TESTED. PackVelocityMask produces the texel array;
 * the GPU upload is a thin consumer. Determinism (P8 rule): the feed
 * carries no time of its own -- Niagara's sim is driven by a user
 * parameter the case player's clock sets.
 */
namespace FlowVizNiagaraFeed
{
	/**
	 * Validate a volume grid before any flattened count or index is formed.
	 * Exposed as the narrow arithmetic seam for overflow tests.
	 */
	FLOWVIZRUNTIME_API bool GetPackedVoxelCount(
		const FIntVector& Counts,
		int32& OutVoxelCount);

	/**
	 * One texel per voxel, X-fastest: xyz velocity (solver units), w = 1 for
	 * fluid, 0 for masked -- so a sampler in the sim can kill on w < 0.5
	 * exactly where the CPU oracle kills on IsMaskedAt.
	 *
	 * @return false for an unbuilt or non-vector sampler.
	 */
	FLOWVIZRUNTIME_API bool PackVelocityMask(
		const FFlowVizFieldSampler& Sampler,
		const FFlowVizFieldMask& Mask,
		TArray<FFloat16Color>& OutTexels,
		FIntVector& OutCounts);

	/**
	 * Create (or resize) the render target and upload the packed texels.
	 * Game thread; the upload itself is enqueued.
	 *
	 * The strong pointer is part of the contract, not a convenience: a transient-
	 * package Outer does not keep its contents alive, and the feed is designed to
	 * reuse this target across frames. Keeping it rooted also covers the interval
	 * between enqueueing the upload and the render thread consuming the resource.
	 * Reset the pointer when the owning system shuts down.
	 *
	 * @param InOutTarget Rooted while this strong pointer owns it; reused when
	 *                    dimensions match, recreated otherwise.
	 * @return false when packing failed; the target is untouched then.
	 */
	FLOWVIZRUNTIME_API bool UploadToRenderTarget(
		const FFlowVizFieldSampler& Sampler,
		const FFlowVizFieldMask& Mask,
		UObject* Outer,
		TStrongObjectPtr<UTextureRenderTargetVolume>& InOutTarget);
}
