// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "CFDViz/CFDVizManifest.h"
#include "RenderCommandFence.h"
#include "Render/FlowVizVolumeTexture.h"

#include "FlowVizVolumeComponent.generated.h"

/**
 * The scene-graph layer: what puts a CFD volume into an Unreal level
 * (plan.md section 5.E, ADR 002, ADR 004).
 *
 * WHAT THIS FILE OWNS. Placement, extent and the proxy that submits the volume
 * to the renderer. It owns an FFlowVizVolumeTextureSet and hands it whole frames.
 * It does NOT decode CVF bytes (Render/FlowVizVolumeTexture.h does), it does not
 * ray-march (Render/FlowVizVolumeRayMarchShader.* does), it does not colour
 * (Render/FlowVizTransferFunction.* does), and it does not choose frames
 * (Playback/FlowVizCasePlayer.* does). Those last three reach this component
 * through the two small interfaces declared below, which this file owns.
 *
 * THREE THINGS HERE ARE WRONG SILENTLY, WHICH IS WHY EACH HAS ITS OWN TEST.
 *
 *  1. **Placement.** The solver -> Unreal transform MIRRORS Y (ADR 004). A
 *     volume placed without the mirror is not garbled; it is a plausible,
 *     mirrored wake. Placement comes from
 *     FFlowVizVolumeTransform::GetLocalToUnrealTransform, a double-precision
 *     FMatrix, and from nowhere else. In particular NOT from
 *     FFlowVizVolumeShaderParameters::GridOrigin, which is float-narrowed and
 *     carries OriginNarrowingError to say so.
 *
 *  2. **Winding.** That same mirror gives the placement matrix a NEGATIVE
 *     determinant, so a box authored counter-clockwise-outward in local space
 *     comes out clockwise-outward in world space. Unfixed, the box's faces are
 *     backwards and the volume renders as empty or inside-out - which reads as
 *     "the data failed to load", not as "the winding is wrong", and sends the
 *     reader to the wrong file. FlowVizVolumeBox::MakeBoxGeometry applies the
 *     compensation, keyed off ::TransformReversesWinding so the rule has one
 *     implementation (ADR 004 section 3).
 *
 *  3. **Bounds.** CalcBounds decides culling. Bounds larger than the volume cost
 *     fill rate; bounds SMALLER than the volume make it vanish at oblique
 *     angles, intermittently, in a way that looks like a streaming bug. They are
 *     computed from FFlowVizVolumeTransform::GetPhysicalSize - spacing times
 *     cell count, per axis - and never from a cube scaled by the actor
 *     transform. The shipped mock domain's voxels are 0.09375 x 0.0625 x
 *     0.0416667, three different numbers, so any cubic-voxel shortcut is wrong
 *     there and the test asserts against exactly that case.
 *
 * THREADING. The component is a game-thread object. The scene proxy is a
 * render-thread object. Nothing crosses by pointer: game-thread state reaches
 * the proxy as FFlowVizVolumeProxyDynamicData, copied by value through
 * SendRenderDynamicData_Concurrent. The proxy never dereferences the component.
 */

class FCFDVizVolumeReader;

/* -------------------------------------------------------------------------- */
/* Box geometry - the proxy's ray-march hull                                    */
/* -------------------------------------------------------------------------- */

/**
 * The volume's bounding box as drawable geometry, in LOCAL space
 * ([0, PhysicalSize], solver units, minimum corner at the origin - the space
 * FFlowVizVolumeTransform::GetLocalToUnrealTransform maps to the world).
 *
 * Deliberately plain data with no render types in it, so the winding rule - the
 * part that is wrong silently - is a pure function a test can check without a
 * device, a scene or a proxy.
 */
struct FFlowVizVolumeBoxGeometry
{
	/** 24 positions: four per face, so each face carries its own normal rather than an averaged corner. */
	TArray<FVector3f> Positions;

	/** 24 normals, OUTWARD in local space. Parallel to Positions. */
	TArray<FVector3f> Normals;

	/** 24 texture coordinates - the position normalised into [0,1]^3, which is the ray-marcher's local entry coordinate. */
	TArray<FVector3f> LocalUVWs;

	/** 36 indices, 12 triangles. Winding per MakeBoxGeometry's bReverseWinding. */
	TArray<uint32> Indices;
};

namespace FlowVizVolumeBox
{
	inline constexpr int32 VertexCount = 24;
	inline constexpr int32 TriangleCount = 12;
	inline constexpr int32 IndexCount = 36;

	/**
	 * Build the volume's box hull.
	 *
	 * THE WHOLE POINT OF THE FLAG. Triangle winding is a chirality, and the
	 * solver -> Unreal transform is a reflection (ADR 004 section 3): a triangle
	 * whose cross product points outward in local space has a cross product
	 * pointing INWARD once transformed, because a cross product picks up
	 * det(M) = -1. Callers therefore pass
	 *
	 *     bReverseWinding = TransformReversesWinding(LocalToWorld)
	 *
	 * and get back geometry that is outward-facing IN WORLD SPACE. Passing false
	 * for a mirroring transform produces a box that is inside-out; it still
	 * renders, it is still box-shaped, and the volume inside it disappears.
	 *
	 * Do not additionally set FMeshBatch::ReverseCulling for the mirror. The
	 * compensation is here, once. (FDynamicMeshBuilder::GetMesh applies its own,
	 * from the same determinant, which is why this component builds its mesh
	 * batch through GetMeshElement and sets the flag explicitly - see
	 * FFlowVizVolumeSceneProxy::GetDynamicMeshElements.)
	 *
	 * @param LocalSize Physical extent in solver units - GetPhysicalSize(). A
	 *                  non-positive or non-finite axis yields empty arrays rather
	 *                  than a degenerate hull.
	 */
	FLOWVIZRUNTIME_API void MakeBoxGeometry(
		const FVector& LocalSize,
		bool bReverseWinding,
		FFlowVizVolumeBoxGeometry& OutGeometry);
}

/* -------------------------------------------------------------------------- */
/* Seams onto the in-flight peers                                               */
/* -------------------------------------------------------------------------- */

/** Which stored frames are on screen and how they blend. Frame B and Alpha describe a temporally interpolated display frame (plan.md section 8). */
struct FFlowVizVolumeFrameSelection
{
	/** Displayed frame. INDEX_NONE means nothing is selected and nothing should be drawn. */
	int32 FrameA = INDEX_NONE;

	/** The frame being blended toward, or INDEX_NONE for a non-interpolated display frame. */
	int32 FrameB = INDEX_NONE;

	/** Blend weight toward FrameB, [0,1]. Meaningless when FrameB is INDEX_NONE. */
	float Alpha = 0.0f;

	/**
	 * True when the displayed frame was SYNTHESIZED rather than stored - which
	 * VISUAL_QA section 1 rule 5 requires be disclosed on screen.
	 *
	 * The bounds are exclusive at BOTH ends, and the two frames must differ,
	 * because each excluded case is an exact landing on a stored frame:
	 * alpha 0 is frame A, alpha 1 is frame B, and blending a frame with itself
	 * is that frame at every alpha. None of the three synthesized anything, so
	 * disclosing interpolation for them would tell a scientist that a stored
	 * measurement is derived.
	 *
	 * The opposite error - failing to disclose a real blend - is the one the
	 * rule exists to prevent, and is why this is a predicate with tested
	 * boundaries rather than an `Alpha > 0` convenience. Neither direction is
	 * visible on screen; both are lies about the data's provenance.
	 */
	bool IsInterpolated() const
	{
		return FrameB != INDEX_NONE
			&& FrameB != FrameA
			&& Alpha > 0.0f
			&& Alpha < 1.0f;
	}
};

/**
 * SEAM 1: frame selection, implemented by Playback/FlowVizCasePlayer when it
 * lands.
 *
 * Declared here, and owned by this file, so the component can be built and
 * tested against a definition rather than blocked on a peer. The component holds
 * a shared pointer to one of these; when none is attached it holds frame 0,
 * which is a display policy, not a stub pretending to be a player.
 */
class IFlowVizVolumeFrameSource
{
public:
	virtual ~IFlowVizVolumeFrameSource() = default;

	/** Game thread. Called once per tick; must not block. */
	virtual FFlowVizVolumeFrameSelection GetFrameSelection() const = 0;
};

/**
 * Everything the ray-march pass needs for one volume in one view. Assembled on
 * the render thread from the proxy's own marshalled copy - no game-thread
 * pointer appears in it.
 */
struct FFlowVizVolumeRayMarchContext
{
	/** The view being rendered. Never null. */
	const FSceneView* View = nullptr;

	/** Local -> world for this volume, double precision, mirror included. The proxy's LocalToWorld. */
	FMatrix LocalToWorld = FMatrix::Identity;

	/** The cbuffer block: everything is LOCAL to the volume. See FFlowVizVolumeShaderParameters. */
	FFlowVizVolumeShaderParameters Parameters;

	/** Textures for display frame A. Never null when a dispatch is issued. */
	const FFlowVizVolumeSlotTextures* SlotA = nullptr;

	/** Textures for display frame B, or null for a non-interpolated display frame. */
	const FFlowVizVolumeSlotTextures* SlotB = nullptr;

	/**
	 * Blend weight toward SlotB. Zero when SlotB is null.
	 *
	 * WHOEVER CONSUMES THIS MUST BLEND AS `(1-t)*A + t*B`, NOT `A + t*(B-A)`.
	 * The two are algebraically identical and numerically are not. The second
	 * form rounds (B-A) before scaling it, so at t == 1 it returns B only when
	 * (B-A) happens to be representable - and for a field that crosses zero
	 * with a couple of decades of range, usually it is not:
	 *
	 *     A = -100,  B = 0.01,  t = 1  ->  0.0100021362   (should be 0.01)
	 *     A = -1000, B = 0.001, t = 1  ->  0.0009765625   (should be 0.001)
	 *
	 * Measured here: over 2e6 random pairs spanning +/-1e-4..1e4 with mixed
	 * signs, the A + t*(B-A) form misses the endpoint on 46.9% of pairs, while
	 * (1-t)*A + t*B is exact on 100% at both t == 0 and t == 1. An FMA does not
	 * rescue it - the subtraction is already rounded before the multiply-add -
	 * and when (B-A) overflows, the mad form yields NaN at t == 0 and Inf at
	 * t == 1 where the two-product form is still exact. The t == 0 NaN is the
	 * worse end: that is the NON-interpolated case, so a field with extreme
	 * outliers produces NaN voxels even when nobody is blending.
	 *
	 * THE TRADE IS REAL AND TWO-SIDED - this form is chosen on magnitude, not
	 * because it is free. Boundedness (staying within [min(A,B), max(A,B)]) and
	 * monotonicity (t increases => the result never goes backwards) are
	 * different properties. Both forms are bounded. Only the mad form is
	 * monotone; (1-t)*A + t*B can retreat as t advances:
	 *
	 *     A=1, B=2:        774,339 backwards steps in 5e7 increasing t
	 *     A=1, B=1+1ULP: 1,048,577
	 *     mad form:              0, on every pair tested
	 *
	 * The retreat is ALWAYS exactly 1 ULP - 1.19e-07 of the A..B span. One step
	 * of an 8-bit colormap is 3.9e-03 of the span and a 16-bit step is 1.5e-05,
	 * so the wobble is four to five orders of magnitude below anything that can
	 * change a displayed color. The mad form's endpoint error on A=-1000,
	 * B=0.001 is 2.3e-02 RELATIVE - five orders the other way.
	 *
	 * So: a 1-ULP retreat in a smooth interior is a rounding artifact that
	 * cannot reach a pixel. A wrong endpoint is a scalar the solver never
	 * produced, pseudocolored as measured data, at a timestep the UI may
	 * simultaneously report as un-interpolated. Only the second is a provenance
	 * lie (VISUAL_QA section 1 rule 1). That asymmetry is the justification; if
	 * someone argues for the mad form on monotonicity grounds, this is the
	 * answer - not a claim that it has no cost.
	 *
	 * MEASURING ANY OF THIS IS ITSELF A TRAP - a sweep reporting zero failures
	 * is not a result until its coverage is separately demonstrated. Between two
	 * agents this rule produced five confident zeros from broken fixtures:
	 * drawing A and B within a factor of two (Sterbenz makes (B-A) exact, so
	 * both forms pass by construction); walking consecutive floats from 0.0f,
	 * which after 3e6 steps reaches only t = 4.2e-39 and never leaves the
	 * subnormals - 0.28% of [0,1]; a uniform grid too coarse to resolve the
	 * retreat, clean at 3e6 samples and 774,339 failures at 5e7; a loop guard
	 * that broke after the first pair; and skipping duplicate t values while
	 * leaving the previous value stale, which silently drops every comparison.
	 * Verify the fixture can produce a failure before believing it found none.
	 */
	float Alpha = 0.0f;

	/**
	 * True when an interpolated frame was REQUESTED but could not be produced,
	 * because frame B's voxels were not resident when this dispatch was built.
	 *
	 * The display silently falls back to frame A alone. That fallback is correct
	 * as a rendering decision - a stored frame is the only honest thing to draw
	 * when the blend's second half is missing - but it is NOT correct to keep
	 * quiet about, and the two cases are indistinguishable on screen:
	 *
	 *   - a genuine non-interpolated frame, which is measured data, and
	 *   - a blend that degraded to its A end, which is what the user asked for
	 *     minus the part that did not arrive.
	 *
	 * VISUAL_QA section 1 rule 5 requires a synthesized frame disclose itself.
	 * The inverse - a frame the user believes is a blend but which is actually a
	 * single stored frame held while the cache catches up - misreports the
	 * playhead's position in time, so it is the same class of provenance lie.
	 * Surfaced here so the marcher, or an overlay above it, can say so rather
	 * than each call site having to re-derive it from a null SlotB.
	 */
	bool bInterpolationDegraded = false;
};

/**
 * SEAM 2: the ray-marcher, implemented by Render/FlowVizVolumeRayMarchShader
 * when it lands.
 *
 * A dispatcher is registered globally rather than per component because the
 * shader is a global shader with no per-instance state, and because the
 * component must not link against a file that does not exist yet.
 *
 * WHEN NOTHING IS REGISTERED THE VOLUME IS NOT DRAWN, and the proxy says so via
 * FFlowVizVolumeSceneProxy::WasRayMarchDispatched. That distinction is load
 * bearing: "no marcher wired" and "marcher ran and produced nothing" look
 * identical on screen and have nothing in common as fixes.
 */
class IFlowVizVolumeRayMarchDispatcher
{
public:
	virtual ~IFlowVizVolumeRayMarchDispatcher() = default;

	/** Render thread only. Called once per view per volume, from GetDynamicMeshElements. */
	virtual void DispatchVolumeRayMarch(const FFlowVizVolumeRayMarchContext& Context) const = 0;
};

namespace FlowVizVolumeRayMarch
{
	/**
	 * Did a requested blend lose its second half?
	 *
	 * A named primitive rather than an expression inlined at the dispatch site,
	 * because the dispatch site lives in GetDynamicMeshElements - render thread,
	 * needs an RHI and a scene - so a test that drove it would self-skip without
	 * RHI=1 and cover nothing (repo memory note green-totals-can-hide-skips).
	 * Testing the primitive directly is only meaningful if the primitive IS the
	 * production code; a copy of this rule living in the test file would pass
	 * while the shipping expression rotted underneath it.
	 */
	inline bool IsInterpolationDegraded(const FFlowVizVolumeFrameSelection& Selection, bool bSlotBResident)
	{
		return Selection.IsInterpolated() && !bSlotBResident;
	}

	/** Install the ray-march implementation. Render thread, or before the render thread has work. Pass null to uninstall. */
	FLOWVIZRUNTIME_API void SetDispatcher(IFlowVizVolumeRayMarchDispatcher* Dispatcher);

	/** The installed implementation, or null. Null means no volume is being ray-marched by anyone. */
	FLOWVIZRUNTIME_API IFlowVizVolumeRayMarchDispatcher* GetDispatcher();
}

/* -------------------------------------------------------------------------- */
/* The component                                                                */
/* -------------------------------------------------------------------------- */

/** What one case load produced. Kept whole so a failed reload cannot leave a half-configured component. */
struct FFlowVizVolumeCaseBinding
{
	/** The loaded manifest. */
	FCFDVizCase Case;

	/** The field being displayed. */
	FName FieldId;

	/** Grid geometry and association for that field - the source of placement and extent. */
	FFlowVizVolumeTransform Transform;

	/** Length scale resolved from `units.length`, NOT assumed to be metres (ADR 004 section 5). */
	double MetersToUnrealUnits = CFDViz::MetersToUnrealCentimeters;

	/** True once every member above has been filled by a successful load. */
	bool bIsValid = false;
};

/**
 * A CFDViz volume in the level.
 *
 * UPrimitiveComponent rather than a bare USceneComponent: a scene proxy is the
 * only way to submit custom geometry to the renderer, and UPrimitiveComponent is
 * the only USceneComponent that has one.
 */
UCLASS(ClassGroup = (FlowViz), meta = (BlueprintSpawnableComponent),
	HideCategories = (Physics, Collision, Navigation))
class FLOWVIZRUNTIME_API UCFDVizVolumeComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UCFDVizVolumeComponent();

	/* --- Case binding ------------------------------------------------------- */

	/**
	 * Load a `.cfdviz` directory and bind one field for display.
	 *
	 * Does file I/O and JSON parsing, so it is not cheap; it does NOT decode any
	 * volume payload, which is the expensive part and happens per frame on a
	 * worker (engineering rule 1).
	 *
	 * @param CaseDirectory Path to the `.cfdviz` directory, or directly to its
	 *                      manifest.json.
	 * @param FieldId       Field to display. NAME_None picks the first field that
	 *                      is not the grid's mask - a mask is a 0/1 volume and
	 *                      binding it by default would show a solid block and
	 *                      look like a broken transfer function.
	 * @return Ok, or a failure naming the file. On failure the component keeps
	 *         its previous binding rather than dropping to a half-loaded one.
	 */
	FCFDVizResult LoadCase(const FString& CaseDirectory, FName FieldId = NAME_None);

	/** Drop the case, release the textures and remove the volume from the scene. Safe when nothing is loaded. */
	void ClearCase();

	/** The bound case. Check bIsValid before reading anything else. */
	const FFlowVizVolumeCaseBinding& GetCaseBinding() const
	{
		return CaseBinding;
	}

	/** True when a case is bound AND its grid describes a drawable, positive-extent box. */
	bool HasRenderableVolume() const;

	/* --- Placement and extent ----------------------------------------------- */

	/**
	 * Local -> Unreal for the bound volume, EXCLUDING the component's own
	 * transform.
	 *
	 * This is FFlowVizVolumeTransform::GetLocalToUnrealTransform: the grid-origin
	 * translation composed with the mirroring unit conversion. Identity when no
	 * case is bound.
	 */
	FMatrix GetVolumeLocalToUnrealMatrix() const;

	/** Domain extent in solver units - spacing times cell count, per axis. Zero when no case is bound. */
	FVector GetPhysicalSize() const;

	/**
	 * The full local -> world matrix the proxy is given.
	 *
	 * GetVolumeLocalToUnrealMatrix() composed with the component's world
	 * transform, so an actor may be moved, rotated and scaled like any other and
	 * the volume follows. CalcBounds composes the same two matrices in the same
	 * order; if these two ever disagree the volume is culled against a box that
	 * is not where it is drawn.
	 */
	virtual FMatrix GetRenderMatrix() const override;

	/**
	 * Bounds from the volume's actual physical extent.
	 *
	 * The box [0, PhysicalSize] pushed through GetVolumeLocalToUnrealMatrix and
	 * then through LocalToWorld. FBoxSphereBounds::TransformBy takes the absolute
	 * value of each matrix row, so the Y mirror does not produce a negative
	 * extent - but it DOES move the origin, which is why the expected origin in
	 * the test has a negative Y for a domain that lives at positive solver Y.
	 */
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

	/* --- Playback ----------------------------------------------------------- */

	/** Attach the case player. Pass an empty pointer to detach, after which the component holds frame 0. */
	void SetFrameSource(TSharedPtr<IFlowVizVolumeFrameSource> InFrameSource);

	/** What is on screen this tick. Frame 0 when no frame source is attached and a case is bound. */
	FFlowVizVolumeFrameSelection GetFrameSelection() const;

	/**
	 * Pin the currently displayed frames in the texture set so eviction cannot
	 * take them.
	 *
	 * The proxy samples BOTH display frames every frame during a blend, but
	 * FFlowVizVolumeTextureSet's eviction policy only knows which frames are in
	 * use because this is called. Without it, ChooseUploadSlot may hand a
	 * prefetch the very slot the shader is about to read.
	 *
	 * That failure needs a prefetch in flight WHILE two frames are displayed, so
	 * it appears under scrubbing and not in a paused screenshot - and it presents
	 * as a torn or stale frame, which reads as a decode bug rather than an
	 * eviction bug. Called automatically whenever the selection is published to
	 * the render thread; exposed because the pin is part of what "these frames
	 * are on screen" means, not a private detail.
	 */
	void PublishDisplayFrames();

	/** The multi-buffered textures this component owns. Never null once constructed. */
	FFlowVizVolumeTextureSet& GetTextureSet()
	{
		return *TextureSet;
	}

	/**
	 * Decode one frame and hand it to the render thread.
	 *
	 * Synchronous and therefore NOT for the game thread in production - it does
	 * file I/O and decompression. Present as the smallest correct thing the
	 * component can do on its own until the case player's worker pipeline lands;
	 * the tests call it directly.
	 *
	 * @return Ok once queued. AllocationTooLarge when every buffer slot is pinned
	 *         or busy, which means "retry after the display frames advance" and
	 *         is not an error to surface (FFlowVizVolumeTextureSet::EnqueueUpload).
	 */
	FCFDVizResult UploadFrame(int32 FrameIndex);

	/* --- Diagnostics -------------------------------------------------------- */

	/** Draw the volume's bounding box as a wireframe. On by default while no ray-marcher is registered, since otherwise nothing at all appears. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FlowViz|Debug")
	bool bDrawBoundingBox = true;

	/** Wireframe colour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FlowViz|Debug")
	FLinearColor BoundingBoxColor = FLinearColor(0.15f, 0.75f, 1.0f, 1.0f);

	/* --- UPrimitiveComponent ------------------------------------------------ */

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual void SendRenderDynamicData_Concurrent() override;
	virtual void OnUnregister() override;
	virtual void BeginDestroy() override;
	virtual bool IsReadyForFinishDestroy() override;

private:
	/** Build the shader parameter block for the bound field's layout. Returns false when no case is bound or the layout is not yet known. */
	bool TryMakeShaderParameters(FFlowVizVolumeShaderParameters& OutParams) const;

	FFlowVizVolumeCaseBinding CaseBinding;

	/** Owns RHI references, so it is heap-allocated and released through the render thread before the component is collected. */
	TUniquePtr<FFlowVizVolumeTextureSet> TextureSet;

	TSharedPtr<IFlowVizVolumeFrameSource> FrameSource;

	/** Layout of the most recently uploaded scalar field, needed for the shader parameter block. Invalid before the first upload. */
	FFlowVizVolumeLayout UploadedScalarLayout;

	/** Set once ReleaseResources has been enqueued, so IsReadyForFinishDestroy can wait for the render thread exactly once. */
	bool bResourcesReleased = false;

	/** Signalled when the enqueued texture release has actually executed. Destroying the texture set before then frees RHI references the render thread may still hold. */
	FRenderCommandFence ReleaseResourcesFence;
};
