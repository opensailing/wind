// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FlowVizCaptureLibrary.generated.h"

/**
 * Helpers for headless/offline frame capture.
 *
 * These exist to answer one question that a black PNG cannot: **is the render
 * scene empty, or is the GPU path broken?** Those two failures look identical
 * from the outside and have nothing in common as fixes, so guessing between
 * them wastes large amounts of time.
 *
 * A hard-won lesson is encoded here. A capture that contains only sky and fog
 * can pass a naive "is it black?" check while containing no geometry at all -
 * SkyAtmosphere and VolumetricCloud render as a full-screen gradient without
 * involving a single primitive. The correct acceptance test is therefore
 * differential, not absolute: capture, remove the geometry, capture again, and
 * require the two images to DIFFER. See `Docs/VISUAL_QA.md` section 4.
 *
 * Note that a commandlet must be run with `-AllowCommandletRendering`, or the
 * renderer is disabled outright and nothing renders at all.
 */
UCLASS()
class FLOWVIZRUNTIME_API UFlowVizCaptureLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Flush deferred render-state updates so primitives reach the render scene.
	 *
	 * Runs `SendAllEndOfFrameUpdates` followed by a rendering-command flush, so
	 * that on return the render thread has caught up and a capture issued
	 * immediately afterwards observes the current world state.
	 *
	 * Safe to call redundantly; it is a no-op when nothing is dirty.
	 *
	 * This is necessary but has **not** been shown to be sufficient in a
	 * commandlet - calling it did not by itself make geometry render there.
	 * Treat it as removing one variable, not as a fix.
	 *
	 * @param WorldContextObject Any object in the world to flush.
	 * @return true if a world was resolved and flushed.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static bool FlushSceneUpdates(const UObject* WorldContextObject);

	/**
	 * How many primitive proxies the world's render scene actually holds.
	 *
	 * This queries `FSceneInterface` - the structure the renderer draws from -
	 * rather than counting components, and that distinction is the whole point.
	 * An earlier version of this function iterated UPrimitiveComponents with a
	 * non-null SceneProxy pointer, which does not fall when actors are
	 * destroyed, because destroyed actors survive until the next garbage
	 * collection with their pointers stale. It reported a healthy count for a
	 * scene that provably rendered nothing.
	 *
	 * The value is only trustworthy because it moves in both directions;
	 * `FlowViz.Capture.SceneProxyCount` asserts exactly that.
	 *
	 * Zero here means the scene is genuinely empty and no camera or material
	 * change will help. A healthy count alongside a black frame means the
	 * problem is downstream in the GPU or capture path.
	 *
	 * @return Proxy count, or INDEX_NONE if no world or no render scene could be
	 *         resolved. INDEX_NONE is deliberately distinct from 0: "cannot
	 *         answer" must not be reported as "the scene is empty".
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static int32 GetSceneProxyCount(const UObject* WorldContextObject);

	/**
	 * Log one line per primitive in the world, with the fact that matters.
	 *
	 * Registered-but-proxyless components are listed explicitly, because that
	 * combination is the signature of the render-state flush not having
	 * happened, and it is invisible in any aggregate count.
	 *
	 * @return Number of components reported, or INDEX_NONE if no world resolved.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static int32 LogPrimitiveBreakdown(const UObject* WorldContextObject);

	/**
	 * Spawn a scene-capture camera that is actually part of the world.
	 *
	 * Python cannot do this itself. Every Blueprint spawn entry point is marked
	 * `BlueprintInternalUseOnly` and so is not exposed, and a
	 * `SceneCaptureComponent2D` constructed directly in Python is created in
	 * `/Engine/Transient` with no world - `RegisterComponent` is not a UFUNCTION,
	 * so it can never be attached to one. That orphaned component captures a
	 * black frame and reports no error, which is the most expensive failure mode
	 * in this whole area because it is indistinguishable from a broken GPU path.
	 *
	 * The returned actor is registered and ready to capture on demand;
	 * `bCaptureEveryFrame` is deliberately off so that results depend on the
	 * caller's explicit `CaptureScene`, not on tick timing.
	 *
	 * @return The spawned actor, or nullptr if no world could be resolved.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static class ASceneCapture2D* SpawnSceneCapture2D(
		const UObject* WorldContextObject,
		FVector Location,
		FRotator Rotation,
		float FOV = 90.0f);

	/**
	 * Force every material in the world to finish loading.
	 *
	 * Materials load lazily, and a primitive whose material has not resolved yet
	 * renders BLACK - it does not fall back to a visible default. The result is
	 * an unlit surface under a correct sky, which looks like a broken renderer
	 * and sends you debugging the capture path instead of the content.
	 *
	 * Measured on L_CapTest: without this the lower half of the frame reads
	 * mean=1.63; with it, mean=114.51. Same world, same camera, same frame
	 * count, back to back in one process.
	 *
	 * `CaptureToPNG` calls this itself. It is exposed because any caller driving
	 * a SceneCaptureComponent2D directly needs it too.
	 *
	 * @return Number of materials resolved, or INDEX_NONE if no world resolved.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static int32 ResolveMaterials(const UObject* WorldContextObject);

	/**
	 * Render one frame from an arbitrary viewpoint and write it to a PNG.
	 *
	 * This is the whole capture path in a single call, because splitting it
	 * across the Python boundary is what made earlier attempts fail silently:
	 * a null render target makes `ExportRenderTarget` a no-op that still logs
	 * success, so "the log said it exported" proved nothing and no file existed.
	 * Here the target cannot be null, and the return value reflects whether a
	 * file was actually written.
	 *
	 * The capture is issued several times before being read. One capture is not
	 * reliably enough: temporal effects and streaming settle over a few frames,
	 * and the first frame is frequently blank.
	 *
	 * A `true` return still does not mean the image contains geometry - a scene
	 * with a SkyAtmosphere renders a convincing gradient with no primitives at
	 * all. Verify differentially; see the class comment.
	 *
	 * @param OutputPath Absolute path of the .png to write. Directories created.
	 * @return true only if a PNG was written to disk.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static bool CaptureToPNG(
		const UObject* WorldContextObject,
		const FString& OutputPath,
		FVector Location,
		FRotator Rotation,
		int32 Width = 1280,
		int32 Height = 720,
		float FOV = 90.0f);
};
