// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

#include "Render/FlowVizTransferFunction.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Scene/FlowVizVolumeComponent.h"

/**
 * THE SEAM BETWEEN A VOLUME IN A MAP AND THE RAY-MARCHER.
 *
 * FlowVizVolumeRayMarchShader could march a volume and FlowVizVolumeComponent
 * could ask for one, and nothing joined them: IFlowVizVolumeRayMarchDispatcher
 * had exactly one implementation and it lived in a test file, SetDispatcher had
 * no caller outside /Tests/, and AddRayMarchPass was reached only from two test
 * files. A volume placed in a real map rendered nothing while the suite was
 * green, because every rendering test installed its own dispatcher first and so
 * was structurally incapable of noticing that production installed none. This
 * file is the production implementation those tests were standing in for.
 *
 * WHY IT NEEDS TWO HALVES. FPrimitiveSceneProxy::GetDynamicMeshElements has no
 * FRDGBuilder - it collects mesh batches, it does not build a render graph - so
 * the dispatcher CANNOT call AddRayMarchPass from the call site that knows which
 * volumes are visible. The dispatcher therefore RECORDS a request per visible
 * volume per view, and an FSceneViewExtensionBase drains those requests in
 * PostRenderView_RenderThread, which does have a builder.
 *
 * WHY PostRenderView AND NOT PreRenderView. PreRenderView_RenderThread is
 * invoked from the PostStaticMeshUpdate callback (SceneRendering.cpp), which
 * runs BEFORE visibility and before GatherDynamicMeshElements - so it fires
 * before the queue it would be draining has been filled. PostRenderView runs
 * inside FSceneRenderer::OnRenderFinish, once per extension per view, with a
 * live FRDGBuilder and without needing any Renderer Internal/ header.
 */

/**
 * A camera reduced to what the marcher's cbuffer actually carries, still in
 * WORLD space.
 *
 * Separated from FSceneView so the world -> local mapping can be tested without
 * a scene. Constructing an FSceneView in an automation test needs a world, a
 * viewport and an RHI; the mapping it would be exercising is eight lines of
 * linear algebra. FlowVizVolumeViewCameraTest drives those eight lines directly.
 */
struct FFlowVizVolumeViewCamera
{
	/** Eye position, world space. */
	FVector WorldOrigin = FVector::ZeroVector;

	/** Orthonormal in world space, as a real view's basis always is. */
	FVector WorldForward = FVector::ForwardVector;
	FVector WorldRight = FVector::RightVector;
	FVector WorldUp = FVector::UpVector;

	/**
	 * tan(halfFov) per axis for a perspective view; half the frustum width and
	 * height in world units for an orthographic one.
	 *
	 * ONE FIELD FOR TWO MEANINGS because the .usf multiplies it by the same
	 * screen coordinate in both branches, and two fields would let a caller set
	 * the one the active branch does not read - which renders a plausible image
	 * at the wrong scale with nothing to point at.
	 */
	FVector2f HalfExtentOrTanFov = FVector2f(1.0f, 1.0f);

	/** False selects the .usf's orthographic branch. */
	bool bPerspective = true;
};

namespace FlowVizVolumeRayMarchProduction
{
	/**
	 * Read a real scene view into the world-space camera the marcher needs.
	 *
	 * GetTanHalfFov returns (1,1) for an orthographic projection - "no concept of
	 * FOV" - so the orthographic half extent is taken from ClipToView instead.
	 * Using GetTanHalfFov for both would frame every orthographic volume at a
	 * fixed, wrong scale.
	 */
	FFlowVizVolumeViewCamera MakeViewCamera(const FSceneView& View);

	/**
	 * Write the camera rows of the cbuffer, mapped into the volume's LOCAL space.
	 *
	 * THE BASIS IS TRANSFORMED, NEVER RE-NORMALISED. WorldToLocal is applied to
	 * each axis as a vector and the result is used at whatever length it comes
	 * out. Normalising each axis independently looks correct - unit length,
	 * plausible direction - and is wrong under a non-uniform scale, because the
	 * local basis is genuinely no longer orthonormal there and rescaling the axes
	 * unequally shears the field of view. It also discards the sign a mirrored
	 * placement carries, which renders the volume inside-out. The .usf normalises
	 * the assembled RAY, which only reparameterises t and leaves the ray's
	 * geometry alone; normalising the BASIS changes the geometry.
	 *
	 * World placement stays in double precision here and never reaches the GPU:
	 * a kilometre-scale case would lose metres to float32 if the marcher worked
	 * in world space.
	 */
	void SetViewCamera(
		FFlowVizVolumeRayMarchParameters& OutParameters,
		const FFlowVizVolumeViewCamera& Camera,
		const FMatrix& LocalToWorld,
		const FIntPoint& OutputSize);

	/** One volume, one view, recorded during GetDynamicMeshElements and marched later. */
	struct FRequest
	{
		/** Identity only - never dereferenced after recording. Matched against the view being drained. */
		const FSceneView* View = nullptr;

		/** Fully assembled except for the two UAVs, which AddRayMarchPass creates. */
		FFlowVizVolumeRayMarchParameters Parameters;

		/**
		 * Holds the slot textures alive from record to drain. The raw pointers in
		 * Parameters are borrowed from a texture set owned by a component, and a
		 * component can be destroyed between the two points.
		 */
		FTextureRHIRef FieldTexture;
		FTextureRHIRef StatusTexture;

		/** Which shader permutation - a uint texture read through a float declaration is noise on Metal, not an error. */
		bool bFieldIsUint = false;

		/** An interpolated frame whose second half is not resident. Disclosed, not silently dropped. */
		bool bInterpolationDegraded = false;

		/** Where in the view family's texture the result belongs. */
		FIntRect ViewRect;
	};

	/**
	 * The production dispatcher.
	 *
	 * DispatchVolumeRayMarch is const because the interface is - it is called
	 * from a const proxy method - so the queue is mutable. It is also called from
	 * GetDynamicMeshElements, which the renderer may run across several tasks, so
	 * the queue is guarded.
	 */
	class FDispatcher final : public IFlowVizVolumeRayMarchDispatcher
	{
	public:
		virtual ~FDispatcher() override = default;

		//~ Begin IFlowVizVolumeRayMarchDispatcher
		virtual void DispatchVolumeRayMarch(const FFlowVizVolumeRayMarchContext& Context) const override;
		//~ End IFlowVizVolumeRayMarchDispatcher

		/** Marches and composites every request recorded for this view, and removes them. */
		void DrainView(FRDGBuilder& GraphBuilder, const FSceneView& View) const;

		/**
		 * Drop requests belonging to no view in this family.
		 *
		 * Without this the queue grows without bound whenever a request is
		 * recorded and never drained - which is what happens if the view
		 * extension failed to register, or if a view was culled after its
		 * elements were gathered.
		 */
		void DropRequestsOutsideFamily(const FSceneViewFamily& ViewFamily) const;

		/** Render thread. Releases the LUT texture and empties the queue. */
		void ReleaseResources() const;

		/** How many requests are waiting. Diagnostics only. */
		int32 NumPendingRequests() const;

		/**
		 * The parameters of a pending request, as the dispatcher actually built
		 * them. Returns false if Index is out of range.
		 *
		 * WHY THIS EXISTS. Without it a test can only reach the parameter
		 * assembly by REBUILDING it -- calling FillDefaults, then the settings,
		 * in the same order the dispatcher does. That mirror passes whether or
		 * not the dispatcher makes those calls at all: deleting
		 * `Context.RenderSettings.ApplyToRayMarchParameters` from
		 * DispatchVolumeRayMarch left the whole suite green at 81/81, measured,
		 * while the shipped renderer went back to one selectable composite mode.
		 *
		 * Reading the queue is the difference between "the view model can
		 * produce IsoSurface" -- already true, already covered -- and "asking
		 * the scene for IsoSurface produces a request that says IsoSurface",
		 * which is the only version of the question the defect could fail.
		 */
		bool PeekRequestParameters(int32 Index, FFlowVizVolumeRayMarchParameters& OutParameters) const;

	private:
		mutable FCriticalSection RequestLock;
		mutable TArray<FRequest> PendingRequests;

		/**
		 * The colour map every production volume is rendered through, rebuilt only
		 * when the value range changes. No component owns a transfer function yet
		 * (plan.md section 10.3), and SHADER_USE_PARAMETER_STRUCT binds every
		 * declared resource - so without one here the dispatch is invalid, not
		 * merely unstyled.
		 */
		mutable FFlowVizTransferFunctionResource TransferFunction;
	};

	/**
	 * The installed dispatcher.
	 *
	 * A function-local static: static storage duration, so it outlives every
	 * test and every module reload within a process, and the pointer the module
	 * hands to SetDispatcher never dangles.
	 */
	FDispatcher& GetProductionDispatcher();

	/** Drains the dispatcher's queue where an FRDGBuilder exists. */
	class FViewExtension final : public FSceneViewExtensionBase
	{
	public:
		explicit FViewExtension(const FAutoRegister& AutoRegister)
			: FSceneViewExtensionBase(AutoRegister)
		{
		}

		//~ Begin ISceneViewExtension
		virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
		virtual void PostRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override;
		//~ End ISceneViewExtension
	};

	/**
	 * Install the dispatcher and arrange for the view extension to exist.
	 *
	 * Called from module startup. The dispatcher pointer is installed
	 * immediately - it is a plain global assignment and is safe before GEngine
	 * exists - but the view extension is deferred to OnPostEngineInit, because
	 * FSceneViewExtensions::RegisterExtension is guarded by ensure(GEngine) and
	 * this plugin loads at PostConfigInit, before GEngine is constructed.
	 */
	void Register();

	/** Uninstall. Safe to call when Register never ran. */
	void Unregister();
}
