// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FlowVizCaptureLibrary.generated.h"

/**
 * Helpers for headless/offline frame capture.
 *
 * These exist because of a specific, non-obvious failure mode. In a commandlet
 * (`-run=pythonscript`) the engine never runs a normal game tick, so
 * `UWorld::SendAllEndOfFrameUpdates()` is never called. Primitive components
 * mark their render state dirty when they register and rely on that end-of-frame
 * flush to create their scene proxies - so in a commandlet, no proxy is ever
 * created and the render scene stays empty.
 *
 * The symptom is deeply misleading: rendering "works" (the RHI initializes,
 * SceneCapture2D reports success, files are written) but every capture contains
 * only fog and atmosphere, and SCENE_DEPTH is uniformly at the far plane. It
 * looks like a GPU or platform failure. It is not - it is a missing tick.
 *
 * Call FlushSceneUpdates() after spawning or modifying actors and before
 * capturing, from Python:
 *
 *     unreal.FlowVizCaptureLibrary.flush_scene_updates(world)
 *     capture_component.capture_scene()
 *
 * Note that the commandlet must also be run with `-AllowCommandletRendering`,
 * or the renderer is disabled outright and nothing renders at all.
 */
UCLASS()
class FLOWVIZRUNTIME_API UFlowVizCaptureLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Flush deferred render-state updates so primitives actually reach the scene.
	 *
	 * Runs `SendAllEndOfFrameUpdates` followed by a rendering-command flush, so
	 * that on return the render thread has caught up and a capture issued
	 * immediately afterwards observes the current world state.
	 *
	 * Safe to call redundantly; it is a no-op when nothing is dirty.
	 *
	 * @param WorldContextObject Any object in the world to flush.
	 * @return true if a world was resolved and flushed.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static bool FlushSceneUpdates(const UObject* WorldContextObject);

	/**
	 * Report how many primitive components the world's render scene holds.
	 *
	 * Diagnostic for the failure above: a scene with actors present but zero
	 * registered primitives means the end-of-frame flush has not happened, which
	 * distinguishes "nothing was ticked" from "the GPU is broken" - two problems
	 * that otherwise look identical from a black PNG.
	 *
	 * @return The number of registered primitive components, or INDEX_NONE if no
	 *         world could be resolved.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static int32 GetRegisteredPrimitiveCount(const UObject* WorldContextObject);
};
