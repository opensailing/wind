// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizVolumeComponent.h"

#include "CFDViz/CFDVizVolumeReader.h"
#include "DynamicMeshBuilder.h"
#include "Engine/Engine.h"
#include "FlowVizRuntime.h"
/*
 * MaterialDomain.h is included for MD_Surface at the default-material fallback
 * below. It is NOT redundant with Materials/Material.h: this module is a unity
 * build, so MD_Surface resolved for a long time only because some OTHER .cpp in
 * the same blob pulled this header in. Compiling this file alone (-singlefile)
 * failed with "use of undeclared identifier 'MD_Surface'; did you mean
 * 'TLM_Surface'?" - and the typo-correction is the dangerous part, since
 * TLM_Surface is a valid enumerator of a DIFFERENT enum. Do not drop this
 * include because a full-module build still succeeds without it.
 */
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MeshBuilderOneFrameResources.h"
#include "MeshElementCollector.h"
#include "Misc/Paths.h"
#include "PrimitiveDrawingUtils.h"
#include "PrimitiveSceneProxy.h"
#include "RenderingThread.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "ShaderCore.h"

/* -------------------------------------------------------------------------- */
/* Box geometry                                                                 */
/* -------------------------------------------------------------------------- */

namespace FlowVizVolumeBoxLayout
{
	/*
	 * A NAMED namespace, not an anonymous one. FlowVizRuntime is a unity build:
	 * every .cpp in the module is #included into one translation unit, so an
	 * anonymous namespace is NOT file-local and a constant named `FaceAxes` here
	 * would collide with a sibling's. That collision already shipped once in this
	 * repo and silently gave a reader the wrong byte offsets (Docs/BUILD.md,
	 * commit 9d5d5ac).
	 */

	/** Face order: -X, +X, -Y, +Y, -Z, +Z. Pinned because the test asserts against exactly this order. */
	struct FFaceDefinition
	{
		/** Outward normal. */
		FVector3f Normal;

		/** The four corners as 0/1 selectors per axis, counter-clockwise seen from OUTSIDE the box. */
		FVector3f Corners[4];
	};

	inline constexpr int32 FaceCount = 6;

	static const FFaceDefinition Faces[FaceCount] = {
		// -X face, looking along +X from outside (from -X toward the box).
		{{-1.0f, 0.0f, 0.0f},
			{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}}},
		// +X face.
		{{1.0f, 0.0f, 0.0f},
			{{1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 1.0f}}},
		// -Y face.
		{{0.0f, -1.0f, 0.0f},
			{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}},
		// +Y face.
		{{0.0f, 1.0f, 0.0f},
			{{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 0.0f}}},
		// -Z face.
		{{0.0f, 0.0f, -1.0f},
			{{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}},
		// +Z face.
		{{0.0f, 0.0f, 1.0f},
			{{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}}},
	};

	static_assert(FaceCount * 4 == FlowVizVolumeBox::VertexCount,
		"Four vertices per face: each face carries its own normal rather than an averaged corner.");
	static_assert(FaceCount * 2 == FlowVizVolumeBox::TriangleCount, "Two triangles per face.");
	static_assert(FlowVizVolumeBox::TriangleCount * 3 == FlowVizVolumeBox::IndexCount, "Three indices per triangle.");
}

void FlowVizVolumeBox::MakeBoxGeometry(
	const FVector& LocalSize,
	bool bReverseWinding,
	FFlowVizVolumeBoxGeometry& OutGeometry)
{
	OutGeometry.Positions.Reset();
	OutGeometry.Normals.Reset();
	OutGeometry.LocalUVWs.Reset();
	OutGeometry.Indices.Reset();

	// A degenerate axis yields NO geometry rather than a hull with zero-area
	// faces. Zero-area triangles rasterize as nothing, which is indistinguishable
	// on screen from a failed load and sends the reader to the wrong file.
	const bool bSizeIsDrawable =
		LocalSize.X > 0.0 && LocalSize.Y > 0.0 && LocalSize.Z > 0.0
		&& FMath::IsFinite(LocalSize.X) && FMath::IsFinite(LocalSize.Y) && FMath::IsFinite(LocalSize.Z);
	if (!bSizeIsDrawable)
	{
		return;
	}

	OutGeometry.Positions.Reserve(VertexCount);
	OutGeometry.Normals.Reserve(VertexCount);
	OutGeometry.LocalUVWs.Reserve(VertexCount);
	OutGeometry.Indices.Reserve(IndexCount);

	const FVector3f Size(LocalSize);

	for (int32 FaceIndex = 0; FaceIndex < FlowVizVolumeBoxLayout::FaceCount; ++FaceIndex)
	{
		const FlowVizVolumeBoxLayout::FFaceDefinition& Face = FlowVizVolumeBoxLayout::Faces[FaceIndex];
		const int32 BaseVertex = OutGeometry.Positions.Num();

		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			// The selector IS the local UVW: local space is [0, PhysicalSize]
			// with the minimum corner at the origin, so a 0/1 selector per axis
			// is already the normalised entry coordinate the ray-marcher wants.
			const FVector3f Selector = Face.Corners[Corner];
			OutGeometry.Positions.Add(Selector * Size);
			OutGeometry.Normals.Add(Face.Normal);
			OutGeometry.LocalUVWs.Add(Selector);
		}

		// The corners are authored counter-clockwise seen from outside, so
		// (0,1,2) and (0,2,3) are outward-facing in a space that does NOT mirror.
		//
		// Under a mirroring transform - which every CFDViz placement matrix is,
		// because solver -> Unreal negates Y (ADR 004 section 3) - a triangle's
		// cross product picks up det(M) = -1 and points INWARD. Swapping two
		// indices per triangle is the compensation, applied here and only here.
		const int32 Order[6] = {0, 1, 2, 0, 2, 3};
		const int32 ReversedOrder[6] = {0, 2, 1, 0, 3, 2};
		const int32* const Chosen = bReverseWinding ? ReversedOrder : Order;

		for (int32 Slot = 0; Slot < 6; ++Slot)
		{
			OutGeometry.Indices.Add(static_cast<uint32>(BaseVertex + Chosen[Slot]));
		}
	}

	check(OutGeometry.Positions.Num() == VertexCount);
	check(OutGeometry.Indices.Num() == IndexCount);
}

/* -------------------------------------------------------------------------- */
/* The ray-march seam                                                           */
/* -------------------------------------------------------------------------- */

namespace FlowVizVolumeRayMarchState
{
	/*
	 * A single global, because the ray-marcher is a global shader with no
	 * per-instance state. Written from the game thread before the render thread
	 * has work, and read from the render thread; the pointer is the only shared
	 * state and it is not written while rendering is in flight.
	 */
	static IFlowVizVolumeRayMarchDispatcher* GDispatcher = nullptr;
}

void FlowVizVolumeRayMarch::SetDispatcher(IFlowVizVolumeRayMarchDispatcher* Dispatcher)
{
	FlowVizVolumeRayMarchState::GDispatcher = Dispatcher;
}

IFlowVizVolumeRayMarchDispatcher* FlowVizVolumeRayMarch::GetDispatcher()
{
	return FlowVizVolumeRayMarchState::GDispatcher;
}

FFlowVizVolumeRayMarchContext FlowVizVolumeRayMarch::MakeDispatchContext(
	const FSceneView* View,
	const FMatrix& LocalToWorld,
	const FFlowVizVolumeProxyDynamicData& DynamicData,
	const FFlowVizVolumeSlotTextures* SlotATextures,
	const FFlowVizVolumeSlotTextures* SlotBTextures)
{
	FFlowVizVolumeRayMarchContext Context;
	Context.View = View;
	Context.LocalToWorld = LocalToWorld;
	Context.Parameters = DynamicData.Parameters;

	/*
	 * THE ASSIGNMENT THAT DID NOT EXIST. Without it Context.RenderSettings is
	 * default-constructed and the dispatcher's mutation-verified seam faithfully
	 * applies nothing: Alpha compositing, lighting off, sixteen parameters at
	 * FillDefaults' constants, no matter what a panel or console command had set.
	 *
	 * The first real headless capture rendered exactly that, and its agent
	 * reported the frame as "unlit front-to-back alpha compositing -- the only
	 * thing it can currently be". That was accurate, and this line's absence was
	 * the reason.
	 */
	Context.RenderSettings = DynamicData.RenderSettings;

	/*
	 * THE SAME ASSIGNMENT, one channel over. Its absence had a longer reach: the
	 * clip view model had no production caller anywhere, so FillDefaults'
	 * NumClipPlanes = 0 was what every frame ever rendered used, and the shader's
	 * clip loop ran zero times. SFlowVizClipPanel drew an advisory strip saying
	 * the panel it belonged to did nothing.
	 *
	 * Copied by value, on the render thread, out of the marshalled payload -- the
	 * component's own Clip is game-thread state and must not be read here.
	 */
	Context.Clip = DynamicData.Clip;

	Context.SlotA = SlotATextures;
	Context.SlotB = SlotBTextures;
	Context.Alpha = Context.SlotB != nullptr ? DynamicData.FrameSelection.Alpha : 0.0f;

	// Falling back to frame A alone is the right picture - a stored frame is the
	// only honest thing to draw when half the blend is missing - but the fallback
	// is PIXEL-IDENTICAL to a genuine single-frame display, so silence here would
	// let a held frame pass as measured data at that timestep. Report it and let
	// the marcher or an overlay disclose it (VISUAL_QA section 1 rule 5).
	Context.bInterpolationDegraded = FlowVizVolumeRayMarch::IsInterpolationDegraded(
		DynamicData.FrameSelection, /*bSlotBResident=*/Context.SlotB != nullptr);

	return Context;
}

FFlowVizDispatchStatus FlowVizVolumeRayMarch::ClassifyDispatch(
	const IFlowVizVolumeRayMarchDispatcher* Dispatcher,
	const FFlowVizVolumeProxyDynamicData& DynamicData,
	const FFlowVizVolumeTextureSet* TextureSet,
	const FFlowVizVolumeSlotTextures* SlotATextures)
{
	FFlowVizDispatchStatus Status;

	/*
	 * ORDERED BY WHAT HAS TO BE FIXED FIRST, not by how the old nested ifs
	 * happened to be written. A volume with no dispatcher AND no upload is
	 * reported as NoDispatcher: waiting for the upload is pointless while
	 * nothing would march it.
	 *
	 * NOTHING IS DEREFERENCED HERE. All three pointers are compared to null and
	 * nothing else, so a caller may pass a slot whose RHI textures have not been
	 * created. A test relies on that: it passes stack-allocated stand-ins that
	 * would crash rather than pass if this ever started reading through them.
	 */
	if (Dispatcher == nullptr)
	{
		Status.Reason = EFlowVizDispatchReason::NoDispatcher;
	}
	else if (!DynamicData.bHasParameters)
	{
		Status.Reason = EFlowVizDispatchReason::NoParameters;
	}
	else if (TextureSet == nullptr)
	{
		Status.Reason = EFlowVizDispatchReason::NoTextureSet;
	}
	else if (SlotATextures == nullptr)
	{
		Status.Reason = EFlowVizDispatchReason::FrameNotResident;
	}
	else
	{
		Status.Reason = EFlowVizDispatchReason::Dispatched;
	}

	return Status;
}

FFlowVizDispatchStatus FlowVizVolumeRayMarch::ClassifyDispatchAndPublish(
	const TSharedRef<FFlowVizDispatchStatusChannel, ESPMode::ThreadSafe>& Channel,
	const IFlowVizVolumeRayMarchDispatcher* Dispatcher,
	const FFlowVizVolumeProxyDynamicData& DynamicData,
	const FFlowVizVolumeTextureSet* TextureSet,
	const FFlowVizVolumeSlotTextures* SlotATextures)
{
	const FFlowVizDispatchStatus Status =
		ClassifyDispatch(Dispatcher, DynamicData, TextureSet, SlotATextures);

	/*
	 * UNCONDITIONAL, AND THAT IS THE ENTIRE POINT.
	 *
	 * Publishing only when Status.ShouldDispatch() leaves a volume that marched
	 * once and then stopped reading healthy forever -- a regression that
	 * presents as a green. That mutation survived the whole suite before this
	 * function existed, because the store was a separate statement in the proxy
	 * that a condition could be wrapped around. Here there is nothing to wrap:
	 * the classification and the report are one operation, and the reason
	 * handed back to the gate is the same value that was published.
	 *
	 * Plain store rather than compare-exchange: the newest frame's answer is
	 * the right one.
	 */
	Channel->Reason.Store(Status.Reason);

	return Status;
}

FString FlowVizVolumeRayMarch::DescribeDispatchReason(EFlowVizDispatchReason Reason)
{
	/*
	 * EACH STRING NAMES THE FIX, not the symptom. "The volume did not render" is
	 * what the reader already knows -- they are looking at a box. What they
	 * cannot see is which of five unrelated things to go and do.
	 *
	 * A SWITCH WITH NO default:, deliberately. Adding a reason without a
	 * description then fails to compile, rather than silently printing the
	 * fallback for a state nobody has words for -- and two reasons that print
	 * alike are two nobody can distinguish afterwards, which is the original
	 * defect wearing a message. FlowViz.Scene.DispatchStatus asserts they are
	 * all distinct.
	 */
	switch (Reason)
	{
	case EFlowVizDispatchReason::NeverRendered:
		return TEXT("no frame has reached the render thread yet - the volume has not been drawn even once");
	case EFlowVizDispatchReason::Dispatched:
		return TEXT("dispatched - the ray-march ran for this volume");
	case EFlowVizDispatchReason::NoDispatcher:
		return TEXT("NO RAY-MARCHER IS INSTALLED - nothing is marching any volume; check module startup registered one");
	case EFlowVizDispatchReason::NoParameters:
		return TEXT("the shader parameters are not built - no case is bound, or no frame has been uploaded, so the field's layout is unknown");
	case EFlowVizDispatchReason::NoTextureSet:
		return TEXT("the component has no texture set - a construction fault, not something that resolves on its own");
	case EFlowVizDispatchReason::FrameNotResident:
		return TEXT("the display frame's upload has not landed on the render thread yet - transient, resolves as the upload completes");
	}

	// Unreachable while the switch above is exhaustive. Returned rather than
	// checkf'd so a diagnostic can never be the thing that takes down a capture.
	return TEXT("unrecognised dispatch reason");
}

/* -------------------------------------------------------------------------- */
/* Marshalled game -> render state                                              */
/* -------------------------------------------------------------------------- */

/*
 * FFlowVizVolumeProxyDynamicData IS DECLARED IN THE PUBLIC HEADER, not here.
 *
 * It used to live in this file, private to this translation unit, and that is
 * how RenderSettings came to have no production writer at all: the only code
 * that could build the payload was unreachable from any test, so the only way
 * to ask what it contained was to render a frame -- and a defaulted composite
 * mode renders a perfectly plausible picture. See the type's comment.
 *
 * Everything the proxy needs is still BY VALUE. No pointer into the component
 * appears in it, and none may be added: a scene proxy outlives some component
 * operations and is destroyed on the render thread, so a pointer back into a
 * game-thread UObject is a use-after-free waiting for a garbage collection at
 * the wrong moment.
 *
 * The texture set is the one exception, and it is not an exception to the rule:
 * FFlowVizVolumeTextureSet is not a UObject, it is owned for the component's
 * whole life, and its RHI resources are released through the render thread
 * before the component is collected (see UCFDVizVolumeComponent::BeginDestroy
 * and IsReadyForFinishDestroy). The proxy holds it as a raw pointer because the
 * alternative - copying RHI references per frame - would defeat the persistent
 * texture requirement in plan.md section 9. It is held by the PROXY, not
 * marshalled in the payload.
 */

/* -------------------------------------------------------------------------- */
/* The scene proxy                                                              */
/* -------------------------------------------------------------------------- */

/**
 * The volume's representation on the render thread.
 *
 * WHAT IT DRAWS. The volume's bounding box as a triangle hull, with its winding
 * already corrected for the placement mirror, and then it hands the ray-marcher
 * that hull's view context. The box is the ray-march domain, not decoration: the
 * marcher runs per pixel covered by the hull.
 *
 * THE CAMERA CAN BE INSIDE THE BOX, AND THAT IS THE NORMAL CASE. A scientist
 * inspecting a wake flies the camera into the domain. A hull drawn with ordinary
 * back-face culling disappears the instant the near plane crosses the front
 * face, and what disappears is the entire volume - which reads as a catastrophic
 * failure rather than as a camera being where it is allowed to be.
 *
 * HOW THIS PROXY HANDLES IT: the mesh batch sets bDisableBackfaceCulling, so
 * BOTH faces of the hull rasterize. From outside, the front faces are nearer and
 * win the depth test; from inside, the front faces are behind the camera and the
 * BACK faces cover the screen, so the hull still produces fragments over exactly
 * the volume's screen footprint. The alternative - flipping the cull mode based
 * on whether the camera is inside - has a discontinuity exactly at the boundary
 * and pops a frame when the camera crosses a face.
 *
 * Two-sided rasterization is why the winding correction still matters even
 * though nothing is culled: the ray-marcher needs to know which side of the hull
 * a fragment is on to choose its ray entry point, and it reads that from the
 * facing, which is only meaningful if the winding is right.
 */
class FFlowVizVolumeSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static SIZE_T UniquePointer;
		return reinterpret_cast<SIZE_T>(&UniquePointer);
	}

	explicit FFlowVizVolumeSceneProxy(const UCFDVizVolumeComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, PhysicalSize(Component->GetPhysicalSize())
		, TextureSet(&const_cast<UCFDVizVolumeComponent*>(Component)->GetTextureSet())
		, BoundingBoxColor(Component->BoundingBoxColor)
		, bDrawBoundingBox(Component->bDrawBoundingBox)
		, DispatchStatusChannel(Component->GetDispatchStatusChannel())
	{
		bWillEverBeLit = false;

		// The winding correction, decided ONCE, here, from the matrix that will
		// actually place this volume - not from a constant, and not rediscovered
		// per call site (ADR 004 section 3).
		const FMatrix LocalToUnreal = Component->GetVolumeLocalToUnrealMatrix();
		bReverseWinding = TransformReversesWinding(LocalToUnreal);

		FlowVizVolumeBox::MakeBoxGeometry(PhysicalSize, bReverseWinding, BoxGeometry);

		/*
		 * Seeded from the component so the very first frame after the proxy is
		 * created is not blank while it waits for a dynamic-data push.
		 *
		 * THE PARAMETERS ARE PART OF THAT SEED. Until this line existed only
		 * FrameSelection was copied, so a freshly created proxy carried
		 * bHasParameters == false and the dispatch gate refused it -- the frame
		 * WAS blank, in exactly the way the comment above promised it would not
		 * be, and the hull rendered on its own.
		 *
		 * That is not a theoretical window. A commandlet capture never ticks:
		 * MarkRenderDynamicDataDirty queues an end-of-frame update, and a script
		 * that spawns an actor and captures immediately gets its proxy built and
		 * photographed inside one flush. The first real headless capture logged
		 * "bHasParameters=NO ... slotATextures=resident" -- the textures had
		 * landed and the parameters had not, because nothing had pushed them.
		 *
		 * TryMakeShaderParameters is const and cheap, and returning false here
		 * simply leaves the proxy in the state it used to always be in, so the
		 * dynamic-data push remains the authority; this only removes the gap
		 * before the first one arrives.
		 *
		 * BUILT BY THE SAME FUNCTION THE PER-FRAME PUSH USES, so the seed and the
		 * push cannot fill different fields. They already had, twice: this seed
		 * copied only FrameSelection until 37d2ae7, and neither writer ever set
		 * RenderSettings, so every frame composited with a default-constructed
		 * view model no matter which path filled it.
		 */
		DynamicData = Component->MakeProxyDynamicData();
	}

	/** Render thread. Replaces the marshalled copy wholesale, so there is no intermediate half-updated state. */
	void SetDynamicData_RenderThread(FFlowVizVolumeProxyDynamicData&& InData)
	{
		check(IsInRenderingThread());
		DynamicData = MoveTemp(InData);
	}

	/**
	 * Did anything actually ray-march this volume?
	 *
	 * Reported rather than assumed because "no marcher is registered" and "the
	 * marcher ran and drew nothing" are the same black screen with nothing in
	 * common as fixes. VISUAL_QA section 3 rule 6 forbids calling an unrendered
	 * feature working.
	 *
	 * THIS DOES NOT MAKE THAT CHECKABLE TODAY, which is what the last line used
	 * to claim. FFlowVizVolumeSceneProxy is private to this .cpp, so this
	 * accessor has no caller and cannot acquire one from outside: the answer is
	 * computed correctly and is unreachable. Exposing it means marshalling the
	 * flag back to the component (render thread writes, game thread reads, so
	 * an atomic), not merely calling this.
	 *
	 * Kept, rather than deleted, because the write at the dispatch site is the
	 * only record that a march occurred and deleting it would remove the thing
	 * a reader needs. Treat it as a stub with a correct value, not a facility.
	 */
	bool WasRayMarchDispatched() const
	{
		return bRayMarchDispatched;
	}

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override
	{
		if (BoxGeometry.Positions.Num() == 0)
		{
			return;
		}

		const FMatrix& LocalToWorld = GetLocalToWorld();

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if ((VisibilityMap & (1 << ViewIndex)) == 0)
			{
				continue;
			}

			const FSceneView* View = Views[ViewIndex];

			if (bDrawBoundingBox)
			{
				// The domain outline. Drawn from the same geometry the marcher
				// uses, so an outline that appears in the wrong place is
				// evidence about placement rather than about a second code path.
				FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
				const FBox LocalBox(FVector::ZeroVector, PhysicalSize);
				DrawWireBox(PDI, LocalToWorld, LocalBox, BoundingBoxColor, SDPG_World);
			}

			// The ray-march hull.
			const bool bIsWireframeView = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
			if (!bIsWireframeView)
			{
				BuildHullMeshBatch(LocalToWorld, ViewIndex, Collector);
			}

			// The ray-march itself. Everything handed over is either a copy or a
			// resource the component owns for its whole life; no game-thread
			// pointer crosses.
			/*
			 * WHY THE DISPATCH DID NOT HAPPEN, SAID OUT LOUD.
			 *
			 * Four things must hold before a march is issued, and until this
			 * block existed all four failed the same way: silently, leaving the
			 * proxy's opaque hull on screen. A headless capture of that frame is
			 * a picture of a box, and it is not distinguishable by eye from a
			 * correctly marched volume that happens to be dense. The first real
			 * capture run failed exactly here and the log could not say which
			 * condition was responsible.
			 *
			 * ONE CLASSIFIER, NOT A GATE AND A SEPARATE LOG CONDITION. Those
			 * used to be two hand-written De Morgan duals of each other -- the
			 * nested ifs decided whether to march, an independent four-term
			 * disjunction decided what to say about it, and nothing kept them
			 * dual. Editing either would have made this log describe a frame
			 * that did not happen, which is worse than no log: it is evidence
			 * pointing away from the defect, and in a headless capture it is the
			 * only evidence there is. Now the value that gates the march IS the
			 * value that gets reported.
			 *
			 * Logged ONCE per proxy rather than per frame - at 60fps per view
			 * this would otherwise bury the log - and at Warning, because a
			 * volume that is in the scene and not marching is a defect every
			 * time, never an expected state.
			 */
			IFlowVizVolumeRayMarchDispatcher* Dispatcher = FlowVizVolumeRayMarch::GetDispatcher();
			const int32 SlotAIndex = (TextureSet != nullptr)
				? TextureSet->FindSlotForFrame(DynamicData.FrameSelection.FrameA)
				: INDEX_NONE;
			const FFlowVizVolumeSlotTextures* SlotATextures = (TextureSet != nullptr)
				? TextureSet->GetSlotTextures(SlotAIndex)
				: nullptr;

			// CLASSIFY AND PUBLISH IN ONE CALL, never as two statements.
			//
			// This used to be ClassifyDispatch followed by a separate store into
			// the channel, and two mutations of that store survived the entire
			// suite: deleting it, and wrapping it in `if (Status.ShouldDispatch())`
			// so that only successes were reported. The second is the one that
			// matters -- a volume that marched once and then stopped would go on
			// reading healthy, turning a regression into a green.
			//
			// There is no separate store here to delete or to make conditional.
			// Publishing IS what this call does, so the reason that gates the
			// march below is necessarily the reason that reached the diagnostic.
			const FFlowVizDispatchStatus Status = FlowVizVolumeRayMarch::ClassifyDispatchAndPublish(
				DispatchStatusChannel, Dispatcher, DynamicData, TextureSet, SlotATextures);

			if (!Status.ShouldDispatch() && !bLoggedDispatchBlocker)
			{
				bLoggedDispatchBlocker = true;
				UE_LOG(LogFlowViz, Warning,
					TEXT("Volume ray-march SKIPPED and the hull is all this frame contains: %s ")
					TEXT("(frameA=%d slotA=%d)"),
					*FlowVizVolumeRayMarch::DescribeDispatchReason(Status.Reason),
					DynamicData.FrameSelection.FrameA,
					SlotAIndex);
			}

			if (Status.ShouldDispatch())
			{
				// Dispatcher, TextureSet and SlotATextures are all non-null here
				// BY CLASSIFICATION, not by a second set of checks - re-testing
				// them would recreate exactly the duplicate rule this replaced.
				const int32 SlotB = TextureSet->FindSlotForFrame(DynamicData.FrameSelection.FrameB);

				// Assembled by a named function, not inline, so the assignments
				// are reachable from a test. Inline, dropping any one of them was
				// invisible: FlowViz.Scene.ProxySettings proved the payload
				// carried the settings and the whole 90-test suite stayed green
				// with the context assignment deleted. See
				// FlowVizVolumeRayMarch::MakeDispatchContext.
				const FFlowVizVolumeRayMarchContext Context =
					FlowVizVolumeRayMarch::MakeDispatchContext(
						View, LocalToWorld, DynamicData,
						SlotATextures, TextureSet->GetSlotTextures(SlotB));

				Dispatcher->DispatchVolumeRayMarch(Context);
				bRayMarchDispatched = true;
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Relevance;
		Relevance.bDrawRelevance = IsShown(View);
		Relevance.bDynamicRelevance = true;
		Relevance.bShadowRelevance = false;

		// A ray-marched volume composites against what is behind it, so it is
		// translucent regardless of how opaque the transfer function happens to
		// make it this frame.
		Relevance.bSeparateTranslucency = Relevance.bNormalTranslucency = true;
		Relevance.bRenderInMainPass = ShouldRenderInMainPass();
		Relevance.bUsesLightingChannels = false;
		Relevance.bVelocityRelevance = false;
		return Relevance;
	}

	/**
	 * The volume never occludes anything: its hull is a loose bound on a
	 * semi-transparent field, and letting it write occlusion would cull geometry
	 * that is genuinely visible through mostly-empty flow.
	 */
	virtual bool CanBeOccluded() const override
	{
		return false;
	}

	virtual uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this) + GetAllocatedSize();
	}

private:
	/** One two-sided hull batch. See the class comment for why nothing is culled. */
	void BuildHullMeshBatch(const FMatrix& LocalToWorld, int32 ViewIndex, FMeshElementCollector& Collector) const
	{
		FDynamicMeshBuilder MeshBuilder(Collector.GetFeatureLevel(), /*NumTexCoords*/ 1);

		for (int32 VertexIndex = 0; VertexIndex < BoxGeometry.Positions.Num(); ++VertexIndex)
		{
			const FVector3f& LocalUVW = BoxGeometry.LocalUVWs[VertexIndex];

			FDynamicMeshVertex Vertex;
			Vertex.Position = BoxGeometry.Positions[VertexIndex];
			Vertex.TextureCoordinate[0] = FVector2f(LocalUVW.X, LocalUVW.Y);
			Vertex.SetTangents(
				FVector3f(1.0f, 0.0f, 0.0f),
				FVector3f(0.0f, 1.0f, 0.0f),
				BoxGeometry.Normals[VertexIndex]);
			Vertex.Color = FColor::White;
			MeshBuilder.AddVertex(Vertex);
		}

		for (int32 Triangle = 0; Triangle < FlowVizVolumeBox::TriangleCount; ++Triangle)
		{
			MeshBuilder.AddTriangle(
				static_cast<int32>(BoxGeometry.Indices[Triangle * 3 + 0]),
				static_cast<int32>(BoxGeometry.Indices[Triangle * 3 + 1]),
				static_cast<int32>(BoxGeometry.Indices[Triangle * 3 + 2]));
		}

		const FMaterialRenderProxy* MaterialProxy =
			GEngine->DebugMeshMaterial != nullptr
				? GEngine->DebugMeshMaterial->GetRenderProxy()
				: UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();

		/*
		 * GetMesh, NOT GetMeshElement. THIS IS A CRASH FIX, NOT A STYLE CHANGE.
		 *
		 * FDynamicMeshBuilder::GetMeshElement initialises its RHI resources
		 * through `FRHICommandListImmediate::Get()`, which opens with
		 * `check(IsInRenderingThread())` (RHICommandList.h:5310). Since UE 5.x
		 * gathers dynamic mesh elements on WORKER threads by default
		 * (r.Visibility.DynamicMeshElements.Parallel, on unless the RHI cannot
		 * support it), GetDynamicMeshElements does not run on the rendering
		 * thread, and that assertion fires. It took down the editor on the
		 * first headless capture, in FDynamicMeshElementContext::
		 * GatherDynamicMeshElementsForPrimitive on a task worker.
		 *
		 * GetMesh does the same work but takes its command list from
		 * `Collector.GetRHICommandList()` - the collector's own list, which is
		 * valid on whichever thread is doing the gather. That is the supported
		 * path for a proxy, and it allocates the one-frame resources and the
		 * FMeshBatch internally.
		 */
		FDynamicMeshBuilderSettings Settings;
		// Nothing is culled: the hull is a loose bound on a semi-transparent
		// field and the camera may be inside it.
		Settings.bDisableBackfaceCulling = true;
		Settings.bReceivesDecals = false;
		Settings.CastShadow = false;
		Settings.bUseSelectionOutline = false;
		Settings.bCanApplyViewModeOverrides = false;

		/*
		 * THE WINDING CORRECTION STAYS IN THE GEOMETRY. WHY THE SECOND ONE
		 * BELOW IS NOT A DOUBLE CORRECTION.
		 *
		 * GetMesh sets `Mesh.ReverseCulling = LocalToWorld.Determinant() < 0`
		 * internally and, unlike the GetMeshElement path, gives the caller no
		 * batch to amend afterwards - it calls Collector.AddMesh itself. For
		 * this volume the determinant is ALWAYS negative (the solver -> Unreal
		 * Y mirror), so that flag is always set, on top of the reversal
		 * MakeBoxGeometry already baked into the index buffer. That reads like
		 * the double conversion ADR 004 section 2 warns about, and an earlier
		 * revision of this comment claimed it was one and proposed dropping the
		 * geometry reversal to compensate. THAT WOULD HAVE BEEN A REAL BUG.
		 *
		 * ReverseCulling has exactly one consumer in the renderer, and it is
		 * reached only after a two-sided test that we fail on purpose:
		 *
		 *   ComputeMeshOverrideSettings (MeshPassProcessor.cpp:1846) maps
		 *   Mesh.bDisableBackfaceCulling -> EDrawingPolicyOverrideFlags::TwoSided,
		 *   and ComputeMeshCullMode (:1867) returns
		 *     bMeshRenderTwoSided ? CM_None : (bReverseCullMode ? CM_CCW : CM_CW)
		 *
		 * bDisableBackfaceCulling is set true twenty lines above, so the cull
		 * mode is CM_None and ReverseCulling is never consulted. There is one
		 * correction, not two, and it is the one in the index buffer.
		 *
		 * THE COUPLING IS LOAD-BEARING: this is only true while the hull is
		 * two-sided. Anyone who sets bDisableBackfaceCulling = false re-arms
		 * ReverseCulling and DOES get two reversals, which cancel - an
		 * inside-out hull that renders as nothing and looks exactly like a
		 * volume that failed to load. Change that flag and you must neutralise
		 * the geometry reversal in the same edit.
		 */
		MeshBuilder.GetMesh(
			LocalToWorld,
			MaterialProxy,
			SDPG_World,
			Settings,
			/*DrawOffset*/ nullptr,
			ViewIndex,
			Collector);
	}

	/** Domain extent in solver units. The hull and the wire box are both built from this. */
	FVector PhysicalSize;

	/** Local-space hull, winding already corrected for the placement mirror. */
	FFlowVizVolumeBoxGeometry BoxGeometry;

	/** Not a UObject and owned by the component for its whole life - see FFlowVizVolumeProxyDynamicData's comment. */
	FFlowVizVolumeTextureSet* TextureSet = nullptr;

	/** Replaced wholesale by SetDynamicData_RenderThread. */
	FFlowVizVolumeProxyDynamicData DynamicData;

	FLinearColor BoundingBoxColor;
	bool bDrawBoundingBox = true;

	/** True when the placement matrix mirrors, which for a CFDViz case it always does. Kept for diagnostics. */
	bool bReverseWinding = false;

	/**
	 * Where this proxy publishes what each frame decided.
	 *
	 * A SHARED REFERENCE, NOT A POINTER TO THE COMPONENT. The marshalling rule
	 * at the top of this file forbids a game-thread UObject pointer living in
	 * the proxy: a proxy outlives some component operations and is destroyed on
	 * the render thread, so that pointer is a use-after-free waiting for a
	 * collection at the wrong moment. Both sides hold the channel; whichever
	 * dies second releases it, and a proxy that outlives its component writes
	 * into memory it still owns.
	 */
	TSharedRef<FFlowVizDispatchStatusChannel, ESPMode::ThreadSafe> DispatchStatusChannel;

	/** Mutable because GetDynamicMeshElements is const; this is a diagnostic, not render state. */
	mutable bool bRayMarchDispatched = false;

	/** One warning per proxy, not one per frame per view. See the dispatch gate. */
	mutable bool bLoggedDispatchBlocker = false;
};

/* -------------------------------------------------------------------------- */
/* The component                                                                */
/* -------------------------------------------------------------------------- */

UCFDVizVolumeComponent::UCFDVizVolumeComponent()
	: DispatchStatusChannel(MakeShared<FFlowVizDispatchStatusChannel, ESPMode::ThreadSafe>())
{
	PrimaryComponentTick.bCanEverTick = false;

	// A volume is never static: its textures change every playback frame.
	Mobility = EComponentMobility::Movable;

	// Data, not geometry. Collision would let a user click a box that is not a
	// surface, and shadows from a loose hull would be shadows of nothing.
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	CastShadow = false;
	bCastDynamicShadow = false;
	bAffectDynamicIndirectLighting = false;

	TextureSet = MakeUnique<FFlowVizVolumeTextureSet>();
	TextureSet->Initialize(FlowVizVolume::RecommendedBufferCount);
}

FCFDVizResult UCFDVizVolumeComponent::LoadCase(const FString& CaseDirectory, FName FieldId)
{
	// Built into a local and committed only on success. A component that drops to
	// a half-loaded binding on failure would render a case that is partly the old
	// one and partly nothing.
	FFlowVizVolumeCaseBinding NewBinding;

	const FString ManifestPath =
		FPaths::GetCleanFilename(CaseDirectory).EndsWith(TEXT(".json"))
			? CaseDirectory
			: FPaths::Combine(CaseDirectory, TEXT("manifest.json"));

	const FCFDVizResult LoadResult = FCFDVizCase::LoadFromFile(ManifestPath, NewBinding.Case);
	if (!LoadResult.IsOk())
	{
		return LoadResult;
	}

	const FCFDVizResult CodecResult = NewBinding.Case.CheckCodecSupport();
	if (!CodecResult.IsOk())
	{
		return CodecResult;
	}

	// Field selection. A mask is a 0/1 volume; binding it by default would
	// render a solid block and read as a broken transfer function rather than as
	// the wrong field being displayed.
	const FCFDVizField* Field = nullptr;
	if (!FieldId.IsNone())
	{
		Field = NewBinding.Case.FindField(FieldId);
		if (Field == nullptr)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::InvalidManifest,
				FString::Printf(TEXT("case declares no field '%s'"), *FieldId.ToString()),
				ManifestPath);
		}
	}
	else
	{
		for (const FCFDVizField& Candidate : NewBinding.Case.Fields)
		{
			const FCFDVizGridDescriptor* CandidateGrid = NewBinding.Case.FindGridForField(Candidate);
			const bool bIsMask =
				(CandidateGrid != nullptr && CandidateGrid->MaskFieldId == Candidate.Id)
				|| Candidate.Semantic.Equals(TEXT("mask"), ESearchCase::IgnoreCase);
			if (!bIsMask)
			{
				Field = &Candidate;
				break;
			}
		}

		if (Field == nullptr)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::InvalidManifest,
				TEXT("case declares no field that is not a mask; nothing to display"),
				ManifestPath);
		}
	}

	const FCFDVizGridDescriptor* Grid = NewBinding.Case.FindGridForField(*Field);
	if (Grid == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(TEXT("field '%s' names grid '%s', which the case does not declare"),
				*Field->Id.ToString(), *Field->GridId.ToString()),
			ManifestPath);
	}

	NewBinding.FieldId = Field->Id;
	NewBinding.Transform.Grid = Grid->Geometry;
	NewBinding.Transform.Association = Field->Association;

	// The length scale comes from the manifest, and an unrecognised unit is an
	// error rather than a silent assumption of metres (ADR 004 section 5).
	// Defaulting a millimetre case to metres would place it a thousand times too
	// large, with nothing on screen to say so.
	double MetersPerUnit = 0.0;
	if (!NewBinding.Case.Units.TryGetLengthInMeters(MetersPerUnit))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(
				TEXT("units.length '%s' is not a length this build recognises; refusing to guess a scale"),
				*NewBinding.Case.Units.Length),
			ManifestPath);
	}
	NewBinding.MetersToUnrealUnits = MetersPerUnit * CFDViz::MetersToUnrealCentimeters;

	if (!NewBinding.Transform.IsValid())
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(TEXT("grid '%s' is not a usable volume geometry"), *Grid->Id.ToString()),
			ManifestPath);
	}

	NewBinding.bIsValid = true;
	CaseBinding = MoveTemp(NewBinding);
	UploadedScalarLayout = FFlowVizVolumeLayout();

	// Bounds and placement both changed, so the proxy is rebuilt rather than
	// updated: its hull geometry is baked at construction.
	MarkRenderStateDirty();
	UpdateBounds();

	UE_LOG(LogFlowViz, Log,
		TEXT("UCFDVizVolumeComponent: loaded '%s', displaying field '%s' over %d x %d x %d cells."),
		*CaseBinding.Case.Metadata.Name,
		*CaseBinding.FieldId.ToString(),
		CaseBinding.Transform.Grid.Dimensions.X,
		CaseBinding.Transform.Grid.Dimensions.Y,
		CaseBinding.Transform.Grid.Dimensions.Z);

	return FCFDVizResult::Ok();
}

void UCFDVizVolumeComponent::ClearCase()
{
	CaseBinding = FFlowVizVolumeCaseBinding();
	UploadedScalarLayout = FFlowVizVolumeLayout();
	MarkRenderStateDirty();
	UpdateBounds();
}

bool UCFDVizVolumeComponent::HasRenderableVolume() const
{
	if (!CaseBinding.bIsValid || !CaseBinding.Transform.IsValid())
	{
		return false;
	}

	const FVector Size = CaseBinding.Transform.GetPhysicalSize();
	return Size.X > 0.0 && Size.Y > 0.0 && Size.Z > 0.0;
}

FMatrix UCFDVizVolumeComponent::GetVolumeLocalToUnrealMatrix() const
{
	if (!CaseBinding.bIsValid)
	{
		return FMatrix::Identity;
	}

	// The ONLY placement source. Double precision, mirror included, grid-origin
	// translation included. Never FFlowVizVolumeShaderParameters::GridOrigin,
	// which is float-narrowed and says so via OriginNarrowingError.
	return CaseBinding.Transform.GetLocalToUnrealTransform(CaseBinding.MetersToUnrealUnits);
}

FVector UCFDVizVolumeComponent::GetPhysicalSize() const
{
	if (!CaseBinding.bIsValid)
	{
		return FVector::ZeroVector;
	}
	return CaseBinding.Transform.GetPhysicalSize();
}

FMatrix UCFDVizVolumeComponent::GetRenderMatrix() const
{
	// Volume placement first, then the component's own world transform, so an
	// actor moves the whole case like any other actor. CalcBounds composes these
	// two in the same order; if the two ever disagree, the volume is culled
	// against a box that is not where it is drawn.
	return GetVolumeLocalToUnrealMatrix() * GetComponentTransform().ToMatrixWithScale();
}

FBoxSphereBounds UCFDVizVolumeComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!HasRenderableVolume())
	{
		// A point at the component's location, not a zero-extent box at the
		// world origin: an empty component must not drag a scene's bounds toward
		// the origin.
		return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.0);
	}

	// THE ACTUAL PHYSICAL EXTENT: spacing times cell count, per axis. Not a unit
	// cube scaled by the actor transform, which is right only for a cubic domain
	// at unit scale and is wrong on the shipped mock case, whose voxels are
	// 0.09375 x 0.0625 x 0.0416667.
	const FVector Size = CaseBinding.Transform.GetPhysicalSize();
	const FBox LocalBox(FVector::ZeroVector, Size);

	// The same composition GetRenderMatrix performs, in the same order.
	return FBoxSphereBounds(LocalBox)
		.TransformBy(GetVolumeLocalToUnrealMatrix())
		.TransformBy(LocalToWorld);
}

void UCFDVizVolumeComponent::SetFrameSource(TSharedPtr<IFlowVizVolumeFrameSource> InFrameSource)
{
	FrameSource = MoveTemp(InFrameSource);
	MarkRenderDynamicDataDirty();
}

FFlowVizVolumeFrameSelection UCFDVizVolumeComponent::GetFrameSelection() const
{
	if (FrameSource.IsValid())
	{
		return FrameSource->GetFrameSelection();
	}

	// No player attached is a display POLICY - hold the first frame - not a stub
	// pretending to be a player. A component with a case bound and no player
	// shows frame 0, which is a defensible thing to see and is distinguishable
	// from an unloaded component, which shows nothing.
	FFlowVizVolumeFrameSelection Selection;
	Selection.FrameA = HasRenderableVolume() ? 0 : INDEX_NONE;
	return Selection;
}

void UCFDVizVolumeComponent::PublishDisplayFrames()
{
	if (!TextureSet.IsValid())
	{
		return;
	}

	const FFlowVizVolumeFrameSelection Selection = GetFrameSelection();

	// Both frames, not just A. During a blend the shader reads B every bit as
	// often as A, so pinning only A leaves the second half of every interpolated
	// frame evictable - which is exactly the frame a prefetch is most likely to
	// take, since it is the one nearest the playhead's direction of travel.
	TextureSet->SetDisplayFrames(Selection.FrameA, Selection.FrameB);
}

FVector2D UCFDVizVolumeComponent::GetDisplayValueRange() const
{
	// The fallback is the unit domain, NOT [0,0]. A zero-width domain makes the
	// .usf's `if (ValueRangeMax > ValueRangeMin)` guard never fire, so every
	// voxel reads LUT entry 0 and the volume renders as one flat colour with no
	// reason bit raised - an invisible failure. The unit domain at least colours
	// [0,1] honestly and makes an out-of-range field visibly saturated, which is
	// a picture a viewer can question. Neither is "the right colours"; one of
	// them can be noticed.
	const FVector2D UnitDomain(0.0, 1.0);

	if (!CaseBinding.bIsValid)
	{
		return UnitDomain;
	}

	const FCFDVizField* Field = CaseBinding.Case.FindField(CaseBinding.FieldId);
	if (Field == nullptr)
	{
		return UnitDomain;
	}

	FCFDVizStatistics Statistics;
	if (!Field->Statistics.TryMakeStatistics(Field->ComponentCount, Statistics))
	{
		// ABSENT IS NOT ZERO. A field whose manifest omits statistics has no
		// declared range, and inventing [0, max-of-something] would present a
		// guess as a measurement (FCFDVizFieldStatistics's own comment).
		return UnitDomain;
	}

	// THE MAGNITUDE RANGE, BECAUSE MAGNITUDE IS WHAT IS COLOURED.
	// FlowVizRayMarch::FillDefaults sets ComponentMode = Magnitude, and the
	// .usf's FlowVizExtractScalar under that mode returns sqrt of the sum of
	// squares - which for a one-component field is |x|, not x. Using the
	// component range instead would map a signed field like `pressure`
	// (component [-62.94, 27.69], magnitude [~0, 62.94]) entirely into the upper
	// half of its domain: a smooth, plausible, wrong picture with nothing on
	// screen to say so. The two ranges coincide for a non-negative field, which
	// is exactly why a fixture of only non-negative fields cannot catch this.
	double Min = 0.0;
	double Max = 0.0;
	if (Statistics.TryGetMagnitudeRange(Min, Max) && Max > Min)
	{
		return FVector2D(Min, Max);
	}

	// A declared range that is degenerate (min == max, a genuinely constant
	// field) is not usable as a domain: dividing by its width is what the guard
	// in the .usf refuses to do. The unit domain is the honest fallback.
	return UnitDomain;
}

bool UCFDVizVolumeComponent::TryMakeShaderParameters(FFlowVizVolumeShaderParameters& OutParams) const
{
	if (!HasRenderableVolume() || !UploadedScalarLayout.IsValid())
	{
		return false;
	}
	return CaseBinding.Transform
		.MakeShaderParameters(UploadedScalarLayout, GetDisplayValueRange(), OutParams)
		.IsOk();
}

FCFDVizResult UCFDVizVolumeComponent::UploadFrame(int32 FrameIndex)
{
	if (!HasRenderableVolume())
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidManifest, TEXT("no case is bound"));
	}

	if (FrameIndex < 0 || FrameIndex >= CaseBinding.Case.GetFrameCount())
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("frame %d is outside the timeline's %d frames"),
				FrameIndex, CaseBinding.Case.GetFrameCount()));
	}

	const FCFDVizField* Field = CaseBinding.Case.FindField(CaseBinding.FieldId);
	if (Field == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(TEXT("bound field '%s' is no longer in the case"), *CaseBinding.FieldId.ToString()));
	}

	FString FieldPath;
	const FCFDVizResult PathResult =
		CaseBinding.Case.ResolveFieldFramePath(*Field, FrameIndex, FieldPath);
	if (!PathResult.IsOk())
	{
		return PathResult;
	}

	FCFDVizVolumeReader Reader;
	const FCFDVizResult OpenResult = Reader.Open(FieldPath);
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	// The grid's mask, when it declares one. Passed to BuildUpload so masked
	// cells reach the status texture rather than being silently rendered as
	// data (plan.md section 4 rule 10).
	const FCFDVizGridDescriptor* Grid = CaseBinding.Case.FindGridForField(*Field);
	FCFDVizVolumeReader MaskReader;
	FCFDVizVolumeReader* MaskReaderPtr = nullptr;
	if (Grid != nullptr && Grid->HasMaskField())
	{
		FString MaskPath;
		if (CaseBinding.Case.ResolveFieldFramePath(Grid->MaskFieldId, FrameIndex, MaskPath).IsOk()
			&& MaskReader.Open(MaskPath).IsOk())
		{
			MaskReaderPtr = &MaskReader;
		}
	}

	const bool bAsVector = Field->ComponentCount > 1;

	FFlowVizVolumeUpload Upload;
	const FCFDVizResult BuildResult =
		FlowVizVolumeBuild::BuildUpload(Reader, MaskReaderPtr, bAsVector, Upload);
	if (!BuildResult.IsOk())
	{
		return BuildResult;
	}

	// The layout the shader parameter block is built from. Taken from whichever
	// texture this field actually landed in, so a vector field does not produce
	// parameters describing an absent scalar texture.
	UploadedScalarLayout = bAsVector ? Upload.VectorLayout : Upload.ScalarLayout;

	const FCFDVizResult UploadResult = TextureSet->EnqueueUpload(MoveTemp(Upload));
	if (!UploadResult.IsOk())
	{
		return UploadResult;
	}

	MarkRenderDynamicDataDirty();
	return FCFDVizResult::Ok();
}

FPrimitiveSceneProxy* UCFDVizVolumeComponent::CreateSceneProxy()
{
	// No case, no proxy. A proxy with nothing in it would put a primitive in the
	// render scene that draws nothing, and the scene-proxy count diagnostic
	// would then report health that is not there - the exact failure
	// FlowVizCaptureLibrary.h describes.
	if (!HasRenderableVolume())
	{
		return nullptr;
	}

	return new FFlowVizVolumeSceneProxy(this);
}

void UCFDVizVolumeComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	// MUST MIRROR BuildHullMeshBatch's choice, including its fallback. A list
	// that names only DebugMeshMaterial would still drop the batch on a
	// configuration where GEngine->DebugMeshMaterial is null and the hull falls
	// back to the default surface material.
	if (bGetDebugMaterials)
	{
		if (GEngine != nullptr && GEngine->DebugMeshMaterial != nullptr)
		{
			OutMaterials.Add(GEngine->DebugMeshMaterial);
		}
		else
		{
			OutMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
		}
	}
}

FFlowVizDispatchStatus UCFDVizVolumeComponent::GetLastDispatchStatus() const
{
	FFlowVizDispatchStatus Status;
	Status.Reason = DispatchStatusChannel->Reason.Load();
	return Status;
}

void UCFDVizVolumeComponent::ReportDispatchStatus_RenderThread(FFlowVizDispatchStatus Status)
{
	// Plain store, not a compare-exchange: the newest frame's answer is the
	// right one. A set-only latch would report a stale success forever after a
	// single good frame, turning a regression into a green.
	DispatchStatusChannel->Reason.Store(Status.Reason);
}

FFlowVizVolumeProxyDynamicData UCFDVizVolumeComponent::MakeProxyDynamicData() const
{
	FFlowVizVolumeProxyDynamicData Data;
	Data.FrameSelection = GetFrameSelection();
	Data.bHasParameters = TryMakeShaderParameters(Data.Parameters);

	// The channel that had no writer. Copied, never referenced: this crosses to
	// the render thread and the component may be edited or collected meanwhile.
	Data.RenderSettings = RenderSettings;

	// The other channel that had no writer, and copied for the same reason: the
	// view model owns a plane array, so a reference here would have the render
	// thread reading a TArray the game thread can reallocate mid-frame.
	Data.Clip = Clip;

	return Data;
}

void UCFDVizVolumeComponent::SetRenderSettings(const FFlowVizRenderSettingsViewModel& InSettings)
{
	RenderSettings = InSettings;

	// Nothing here changes geometry or bounds, so the proxy does not need
	// rebuilding -- only its marshalled copy needs replacing. MarkRenderStateDirty
	// would recreate the proxy and re-seed the hull to change a composite mode.
	MarkRenderDynamicDataDirty();
}

void UCFDVizVolumeComponent::SetClip(const FFlowVizClipViewModel& InClip)
{
	Clip = InClip;

	// Same reasoning as SetRenderSettings: clipping happens in the shader
	// against the full volume, so the hull, the bounds and the uploaded textures
	// are all unaffected. Only the marshalled copy needs replacing.
	MarkRenderDynamicDataDirty();
}

void UCFDVizVolumeComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();

	if (SceneProxy == nullptr)
	{
		return;
	}

	// Pin before publishing. The selection about to reach the render thread is
	// the one the proxy will sample, so the pin must be in place before the
	// proxy can act on it - not a tick later, which is a window a prefetch can
	// fit inside.
	PublishDisplayFrames();

	// Built on the game thread, moved to the render thread BY VALUE. The proxy
	// never reads the component. Same builder the proxy's constructor seed uses.
	FFlowVizVolumeProxyDynamicData Data = MakeProxyDynamicData();

	FFlowVizVolumeSceneProxy* VolumeProxy = static_cast<FFlowVizVolumeSceneProxy*>(SceneProxy);
	ENQUEUE_RENDER_COMMAND(FlowVizVolumeUpdateDynamicData)(
		[VolumeProxy, Data = MoveTemp(Data)](FRHICommandListImmediate&) mutable
		{
			VolumeProxy->SetDynamicData_RenderThread(MoveTemp(Data));
		});
}

void UCFDVizVolumeComponent::OnUnregister()
{
	Super::OnUnregister();
}

void UCFDVizVolumeComponent::BeginDestroy()
{
	Super::BeginDestroy();

	// RHI resources may only be released on the render thread. Enqueue that here
	// and let IsReadyForFinishDestroy hold the object alive until it has run;
	// releasing in the destructor would free the texture references while the
	// render thread might still be sampling them.
	if (TextureSet.IsValid() && !bResourcesReleased)
	{
		TextureSet->ReleaseResources();
		bResourcesReleased = true;

		// The fence is what makes the release ORDERED with respect to this
		// object's destruction. Without it, BeginDestroy would queue the release
		// and FinishDestroy would run the destructor immediately after, freeing
		// the texture set out from under a render command that has not executed
		// yet. Nothing about that is deterministic - it depends on how far behind
		// the render thread happens to be - so it would present as an occasional
		// crash on level teardown.
		ReleaseResourcesFence.BeginFence();
	}
}

bool UCFDVizVolumeComponent::IsReadyForFinishDestroy()
{
	if (!Super::IsReadyForFinishDestroy())
	{
		return false;
	}

	// Not a flush: returning false here asks the garbage collector to come back
	// later, which is the non-blocking way to wait. Calling
	// FlushRenderingCommands from a GC callback stalls the game thread on the
	// render thread for as many components as are being collected.
	return !bResourcesReleased || ReleaseResourcesFence.IsFenceComplete();
}
