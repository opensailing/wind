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

	/**
	 * CaptureToPNG plus the DoD 15 footer: case name, field, physical time,
	 * value range and a colour-bar legend, burned into the pixels before the
	 * PNG is written (FlowVizAnnotate::BurnFooter). The annotation reads the
	 * BOUND CASE ACTOR's own state -- its case name, field id, displayed time
	 * and transfer function -- so the caption cannot disagree with the image
	 * the way caller-supplied strings could.
	 *
	 * @return true only if an ANNOTATED png was written: a capture whose image
	 *         is too small for the footer fails rather than silently writing
	 *         an unannotated file under the annotated name.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static bool CaptureAnnotatedPNG(
		const UObject* WorldContextObject,
		class ACFDVizCaseActor* CaseActor,
		const FString& OutputPath,
		FVector Location,
		FRotator Rotation,
		int32 Width = 1280,
		int32 Height = 720,
		float FOV = 90.0f);

private:
	/**
	 * The shared readback-to-PNG pipeline behind both capture entry points.
	 * AnnotationRequest is a FlowVizCaptureLocal::FCaptureAnnotationRequest* or
	 * null; void* because that type lives in the .cpp's named namespace (unity
	 * build) and this header must not drag the annotation header into every
	 * includer.
	 */
	static bool CapturePipeline(
		const UObject* WorldContextObject,
		const FString& OutputPath,
		FVector Location,
		FRotator Rotation,
		int32 Width,
		int32 Height,
		float FOV,
		const void* AnnotationRequest);

public:

	/**
	 * Put a CFDViz case in the world, loaded, uploaded, and ready to march.
	 *
	 * Python cannot assemble this itself, and the reasons are the same class as
	 * SpawnSceneCapture2D's: every Blueprint spawn entry point is
	 * `BlueprintInternalUseOnly`, and `UCFDVizVolumeComponent::UploadFrame` is
	 * not a UFUNCTION. A case actor placed without an upload has a scene proxy
	 * and no shader parameters, so it draws its hull and dispatches NOTHING -
	 * which looks exactly like a broken ray-marcher.
	 *
	 * THE FIELD MUST BE A SCALAR, AND THIS FUNCTION REJECTS ONE THAT IS NOT.
	 * `LoadCase(NAME_None)` picks the first non-mask field, which in the shipped
	 * sample is `U` - a 3-component vector. A vector field's bytes land in the
	 * upload's VectorLayout, leaving `Slot.ScalarTexture` null, and
	 * `DispatchVolumeRayMarch` early-returns on `!Request.FieldTexture.IsValid()`.
	 * The volume then renders its hull and nothing else, silently: a picture
	 * indistinguishable from a broken marcher, produced by a correct one.
	 *
	 * An earlier draft of this comment only WARNED about that, which would have
	 * moved the trap from the renderer into the docstring. The check is here
	 * instead, and it fails loudly with the component count in OutError. Scalar
	 * fields in the sample: `speed`, `pressure`, `vorticityMagnitude`,
	 * `qCriterion`, `passiveScalar`.
	 *
	 * @param CaseDirectory Path to the `.cfdviz` directory or its manifest.json.
	 * @param FieldId       Field to display. Must name a 1-component field, and
	 *                      must be given: NAME_None is refused rather than
	 *                      defaulted, because the default is a vector.
	 * @param FrameIndex    Frame to upload. Without an upload there are no
	 *                      shader parameters and no dispatch.
	 * @param bDrawBoundingBox Whether the debug wireframe box is drawn. Note
	 *                      this does NOT suppress the proxy's solid hull, which
	 *                      is drawn unconditionally - see
	 *                      SetVolumeRayMarcherEnabled for what actually isolates
	 *                      the marcher.
	 * @param OutError      The full diagnostic on failure, naming the file.
	 * @return The spawned actor, or nullptr. A non-null return means the case
	 *         loaded AND a frame was uploaded; a partial success returns null.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture",
		meta = (WorldContext = "WorldContextObject"))
	static class ACFDVizCaseActor* SpawnCaseActor(
		const UObject* WorldContextObject,
		const FString& CaseDirectory,
		FName FieldId,
		FVector Location,
		FRotator Rotation,
		FString& OutError,
		int32 FrameIndex = 0,
		bool bDrawBoundingBox = false);

	/**
	 * Install or uninstall the production ray-march dispatcher.
	 *
	 * THIS IS THE ONLY SINGLE-VARIABLE CONTROL THE VOLUME HAS, and it exists
	 * because the obvious ones do not work. The scene proxy draws a wireframe
	 * box, a solid `GEngine->DebugMeshMaterial` hull, and the ray-march dispatch
	 * from one `GetDynamicMeshElements`. A primitive-suppressed reference capture
	 * removes all three at once, so "the frame differs from the reference" goes
	 * true the moment the hull rasterizes - which it does unconditionally, with
	 * no flag guarding it. That criterion cannot distinguish a working marcher
	 * from a dead one, and since the hull is an opaque box, the frame it
	 * certifies even looks like a rendered volume. `bDrawBoundingBox` does not
	 * help: only the wireframe is behind it.
	 *
	 * Toggling the dispatcher holds the proxy, the hull, the box, the camera and
	 * the lighting all fixed and moves exactly one thing. Any pixel that differs
	 * between the two captures came from the ray-marcher and from nothing else.
	 *
	 * Flushes rendering commands before returning, so a capture issued on the
	 * next line observes the change rather than racing it.
	 *
	 * @param bEnabled true reinstalls the production dispatcher; false uninstalls.
	 * @return Whether a dispatcher is installed after the call, so a caller can
	 *         assert the toggle took effect rather than assuming it.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture")
	static bool SetVolumeRayMarcherEnabled(bool bEnabled);

	/** Whether a ray-march dispatcher is currently installed. */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture")
	static bool IsVolumeRayMarcherEnabled();

	/**
	 * Choose how a placed volume composites, from script.
	 *
	 * THE LAST LINK IN A CHAIN THAT WAS VERIFIED EVERYWHERE ELSE. Below this
	 * point every hop is mutation-verified: component to payload
	 * (FlowViz.Scene.ProxySettings), payload to context
	 * (FlowViz.Scene.DispatchContext), context to shader parameters
	 * (FlowViz.Render.SettingsSeam). Above it there was nothing at all --
	 * `UCFDVizVolumeComponent::SetRenderSettings` had exactly one caller in the
	 * module and it was a test. So the five composite modes that #39 unfroze
	 * were reachable from C++ and from nowhere a capture script could stand, and
	 * every shipped frame still composited Alpha.
	 *
	 * AN int32 BECAUSE EFlowVizCompositeMode IS NOT A UENUM, deliberately: its
	 * values are pinned to the FLOWVIZ_MODE_* defines in the .usf, and a UENUM
	 * would be a second place for them to drift. The value is validated against
	 * the enum here rather than passed through -- an unrecognised mode falls to a
	 * default branch in the shader and renders as a mode nobody selected, which
	 * reads as a broken shader rather than as a rejected input.
	 *
	 * MERGES INTO THE VOLUME'S EXISTING SETTINGS rather than replacing them, so
	 * selecting a mode does not silently reset lighting, steps or jitter. The
	 * two setters are independent and FlowViz.Capture.RenderSettings asserts
	 * that each survives the other.
	 *
	 * @param CaseActor The placed case whose volume to configure. Null is
	 *                  refused and logged, not dereferenced.
	 * @param CompositeMode A value of EFlowVizCompositeMode: 0 Alpha, 1 Maximum,
	 *                  2 Minimum, 3 Average, 4 IsoSurface, 5 Diagnostic.
	 * @param IsoValue  The iso-surface threshold in field units. Read only in
	 *                  IsoSurface mode, but applied regardless so that switching
	 *                  into that mode later does not need a second call.
	 * @return false when the actor is null, has no volume, or the mode is not a
	 *         member of the enum. The volume's previous settings are KEPT on
	 *         refusal -- a control given a bad number must not become a control
	 *         that does nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture")
	static bool SetVolumeCompositeMode(
		class ACFDVizCaseActor* CaseActor,
		int32 CompositeMode,
		float IsoValue = 0.0f);

	/**
	 * Turn gradient lighting on or off for a placed volume.
	 *
	 * SEPARATE FROM THE MODE ON PURPOSE. VISUAL_QA rule 1 forbids lighting from
	 * modulating apparent scalar value, so the Scientific profile renders unlit
	 * and that default must survive a mode change. Folding both into one "apply
	 * settings" call would make every mode selection also a decision about
	 * lighting, silently.
	 *
	 * @return false when the actor is null or has no volume component.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture")
	static bool SetVolumeLightingEnabled(class ACFDVizCaseActor* CaseActor, bool bEnabled);

	/**
	 * Add a clipping plane to a placed volume, in the volume's local solver units.
	 *
	 * THE ENTRY POINT THE CLIP VIEW MODEL NEVER HAD. Every caller of
	 * FFlowVizClipViewModel::ApplyToRayMarchParameters was a test, so what
	 * shipped was FillDefaults' NumClipPlanes = 0 and the shader's clip loop ran
	 * zero times on every frame ever rendered. SFlowVizClipPanel disclosed that
	 * in an advisory strip.
	 *
	 * THE DOMAIN IS TAKEN FROM THE VOLUME, not from the caller. The crop box
	 * reaches the shader normalised by the domain size, so a domain that
	 * disagreed with the loaded field would clip at a plausible wrong place --
	 * and an image that is wrong but not obviously wrong is the failure this
	 * codebase keeps paying for. A volume with no field yet has no domain, and
	 * this refuses rather than guessing one.
	 *
	 * @param Normal   Plane normal in local space. Need not be unit length;
	 *                 must not be zero.
	 * @param Distance Signed distance along the normal. The kept half-space is
	 *                 dot(P, Normal) >= Distance.
	 * @return false when the actor is null, has no volume component, has no
	 *         field loaded, the normal is degenerate, or the volume already
	 *         holds the maximum number of planes.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture")
	static bool AddVolumeClipPlane(
		class ACFDVizCaseActor* CaseActor,
		FVector Normal,
		double Distance);

	/**
	 * Remove every clipping plane and reset the crop box on a placed volume.
	 *
	 * Separate from AddVolumeClipPlane so a capture script can return a volume to
	 * unclipped without knowing how many planes it accumulated -- and so the
	 * "off" direction has its own entry point rather than being expressed as an
	 * absence of calls, which nothing can assert on.
	 *
	 * @return false when the actor is null or has no volume component.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Capture")
	static bool ClearVolumeClipping(class ACFDVizCaseActor* CaseActor);
};
