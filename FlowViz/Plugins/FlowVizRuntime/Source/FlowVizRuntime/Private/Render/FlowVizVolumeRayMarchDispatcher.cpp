// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizVolumeRayMarchDispatcher.h"

#include "FlowVizRuntime.h"

#include "CommonRenderResources.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeLock.h"
#include "PixelFormat.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "ScreenPass.h"

/*
 * NAMED NAMESPACE, NOT ANONYMOUS. FlowVizRuntime is a unity build, so an
 * anonymous namespace here is not file-local: it merges with every other
 * anonymous namespace in the same unity blob and a name collision becomes an ODR
 * violation that only appears in some build configurations. That collision
 * already shipped once in this repo.
 */
namespace FlowVizVolumeRayMarchDispatcherLocal
{
	/*
	 * THE LUT DOMAIN CONSTANTS ARE GONE, and the reasoning they carried is now
	 * enforced by the type rather than by a pair of values.
	 *
	 * They existed because the LUT is a colour map, not a scale: the shader
	 * normalises the sample by the cbuffer's own ValueRangeMin/Max before
	 * indexing (FlowVizVolumeRayMarch.usf line 415), so a fixed [0,1] LUT domain
	 * colours correctly for any field range, and building per volume would
	 * rebuild the table whenever two volumes with different ranges were visible.
	 *
	 * That is still true. What changed is that FRequest now carries the transfer
	 * function itself instead of a map plus a range, and BuildLut reads no range
	 * at all -- so there is no longer a domain to pass, correctly or otherwise.
	 * A constant that only a comment still mentions is a warning at best and a
	 * misleading answer to "what domain does this build over?" at worst.
	 */

	/**
	 * PREMULTIPLIED-OVER.
	 *
	 * The .usf accumulates front-to-back as
	 *   Color.rgb += (1 - Color.a) * Alpha * Rgb;
	 *   Color.a   += (1 - Color.a) * Alpha;
	 * so OutColor.rgb already carries its own alpha. Compositing it with the
	 * ordinary SrcAlpha/InvSrcAlpha blend would multiply by alpha a second time
	 * and darken every semi-transparent voxel - a plausible image, uniformly
	 * wrong, of exactly the kind VISUAL_QA rule 1 exists to catch.
	 */
	FRHIBlendState* GetPremultipliedOverBlendState()
	{
		return TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
	}
}

FFlowVizVolumeViewCamera FlowVizVolumeRayMarchProduction::MakeViewCamera(const FSceneView& View)
{
	FFlowVizVolumeViewCamera Camera;

	Camera.WorldOrigin = View.ViewMatrices.GetViewOrigin();
	Camera.WorldForward = View.GetViewDirection();
	Camera.WorldRight = View.GetViewRight();
	Camera.WorldUp = View.GetViewUp();
	Camera.bPerspective = View.IsPerspectiveProjection();

	if (Camera.bPerspective)
	{
		Camera.HalfExtentOrTanFov = View.ViewMatrices.GetTanHalfFov();
	}
	else
	{
		// GetTanHalfFov returns (1,1) for an orthographic projection - the
		// engine's own comment says there is "no concept of FOV" there - so
		// reading it for both branches would frame every orthographic volume at
		// a fixed, wrong scale with no other symptom. The half extent is the
		// inverse of the projection's X/Y scale, in world units.
		const FMatrix& ViewToClip = View.ViewMatrices.GetViewToClip();
		const double ScaleX = ViewToClip.M[0][0];
		const double ScaleY = ViewToClip.M[1][1];
		Camera.HalfExtentOrTanFov = FVector2f(
			static_cast<float>(FMath::IsNearlyZero(ScaleX) ? 1.0 : 1.0 / ScaleX),
			static_cast<float>(FMath::IsNearlyZero(ScaleY) ? 1.0 : 1.0 / ScaleY));
	}

	return Camera;
}

void FlowVizVolumeRayMarchProduction::SetViewCamera(
	FFlowVizVolumeRayMarchParameters& OutParameters,
	const FFlowVizVolumeViewCamera& Camera,
	const FMatrix& LocalToWorld,
	const FIntPoint& OutputSize)
{
	const FMatrix WorldToLocal = LocalToWorld.Inverse();

	// TransformPosition for the eye, TransformVector for the axes: a translation
	// applies to one and not the other, and using the wrong one puts the camera
	// at the origin of the volume for every non-identity placement.
	const FVector LocalOrigin = WorldToLocal.TransformPosition(Camera.WorldOrigin);
	const FVector LocalForward = WorldToLocal.TransformVector(Camera.WorldForward);
	const FVector LocalRight = WorldToLocal.TransformVector(Camera.WorldRight);
	const FVector LocalUp = WorldToLocal.TransformVector(Camera.WorldUp);

	// NOT NORMALISED. See the header: under a non-uniform or mirrored placement
	// the local basis is genuinely not orthonormal, and forcing it to unit length
	// shears the field of view and loses the mirror.
	OutParameters.RayCameraOrigin = FVector3f(LocalOrigin);
	OutParameters.RayCameraForward = FVector3f(LocalForward);
	OutParameters.RayCameraRight = FVector3f(LocalRight);
	OutParameters.RayCameraUp = FVector3f(LocalUp);
	OutParameters.RayCameraPad0 = 0.0f;
	OutParameters.RayCameraPad1 = 0.0f;
	OutParameters.RayCameraPad2 = 0.0f;
	OutParameters.RayCameraPad3 = 0.0f;

	OutParameters.bOrthographic = Camera.bPerspective ? 0u : 1u;

	// Both rows are written in both branches. The .usf reads only the one its
	// branch selects, but leaving the other holding a previous frame's value
	// makes a later change of projection mode read stale geometry.
	if (Camera.bPerspective)
	{
		OutParameters.TanHalfFov = Camera.HalfExtentOrTanFov;
		OutParameters.OrthoHalfExtent = FVector2f::ZeroVector;
	}
	else
	{
		OutParameters.TanHalfFov = FVector2f::ZeroVector;
		OutParameters.OrthoHalfExtent = Camera.HalfExtentOrTanFov;
	}

	OutParameters.OutputSizeX = static_cast<uint32>(FMath::Max(OutputSize.X, 1));
	OutParameters.OutputSizeY = static_cast<uint32>(FMath::Max(OutputSize.Y, 1));
}

void FlowVizVolumeRayMarchProduction::FDispatcher::DispatchVolumeRayMarch(
	const FFlowVizVolumeRayMarchContext& Context) const
{
	if (Context.View == nullptr || Context.SlotA == nullptr)
	{
		return;
	}

	const FIntRect ViewRect = Context.View->UnscaledViewRect;
	if (ViewRect.Width() <= 0 || ViewRect.Height() <= 0)
	{
		return;
	}

	FRequest Request;
	Request.View = Context.View;
	Request.ViewRect = ViewRect;
	Request.bInterpolationDegraded = Context.bInterpolationDegraded;

	// Held by reference for the life of the request. The raw pointers written
	// into Parameters below are borrowed from a texture set a component owns,
	// and a component can be destroyed between recording and draining.
	Request.FieldTexture = Context.SlotA->ScalarTexture;
	Request.StatusTexture = Context.SlotA->StatusTexture;
	if (!Request.FieldTexture.IsValid())
	{
		return;
	}

	FlowVizRayMarch::FillDefaults(Request.Parameters);
	FlowVizRayMarch::FillFromVolumeParameters(Context.Parameters, Request.Parameters);

	// AFTER the defaults, BEFORE the textures and the camera.
	//
	// After, because the settings are what makes the defaults selectable rather
	// than welded -- applied first, FillDefaults would overwrite every one of
	// them and this call would be decoration. Before the texture and camera
	// rows, because ApplyToRayMarchParameters writes field-by-field and never
	// assigns the struct, so the ordering is a statement of intent rather than a
	// requirement; putting it here keeps it next to the call it modifies.
	//
	// This is the call that makes the view model reachable. Without it the class
	// still compiled, still passed FlowViz.UI.RenderSettings, and still left the
	// shipped renderer with one selectable composite mode -- a writer nobody
	// reaches leaves the parameter exactly as frozen as before.
	Context.RenderSettings.ApplyToRayMarchParameters(Request.Parameters);

	// THE CLIP SEAM, and the same story one channel over. FFlowVizClipViewModel
	// validates every plane before writing any, zeroes the unused tail, and
	// normalises the crop box -- all covered by FlowViz.UI.ClipViewModel, all
	// passing, and until this line every one of its callers was a test. What
	// shipped was FillDefaults' NumClipPlanes = 0, so the shader's
	// `min(NumClipPlanes, FLOWVIZ_MAX_CLIP_PLANES)` loop ran zero times on every
	// frame ever rendered.
	//
	// THE RESULT IS DELIBERATELY IGNORED, and that is not the usual shrug. The
	// call fails in exactly one way a frame can hit -- no domain set yet, before
	// the first upload -- and it writes NOTHING when it fails, so ignoring it
	// leaves the defaults intact and the volume renders unclipped. The
	// alternative, returning early, would drop the whole frame because a clip
	// box is not configured. A degenerate PLANE cannot get here: SetPlane and
	// AddPlane reject one on the way in, and the loop above re-validates.
	//
	// Placed after the settings for the same reason they come after FillDefaults:
	// these writers are field-by-field and never assign the struct, but the
	// ordering states which layer wins, and clipping is the more specific choice.
	Context.Clip.ApplyToRayMarchParameters(Request.Parameters);

	// THE COLOUR SEAM, and the third and last of these. Same story as the two
	// above -- every caller of ApplyToRayMarchParameters was a test, and what
	// shipped was FillDefaults -- with one difference that matters: this view
	// model also owns the LUT, and the LUT is built elsewhere (DrainView). So
	// this call carries only HALF the transfer function, and wiring it without
	// the other half would stretch the right colours over the wrong domain.
	// Request.TransferFunction below is that other half.
	//
	// THE RESULT IS IGNORED FOR A DIFFERENT REASON THAN THE CLIP'S. The clip
	// call fails on an unconfigured domain, which is a state a real frame hits.
	// This one fails only when the current choices do not Validate, and it
	// writes nothing when it does -- so ignoring it leaves the defaults and the
	// volume renders in viridis over [0,1] rather than dropping the frame.
	Context.TransferFunction.ApplyToRayMarchParameters(Request.Parameters);

	// CARRIED ON THE REQUEST, NOT READ AT DRAIN TIME. DrainView runs on the
	// render thread against a queue that may hold requests from several volumes;
	// Context is a game-thread value that is gone by then. Recording the choice
	// here is what lets the LUT follow the picker.
	//
	// THE VIEW MODEL'S OWN OBJECT, taken whole. See FRequest::TransferFunction:
	// the reverse toggle, the banding control and the opacity curve all live in
	// here and all change what BuildLut produces.
	Request.TransferFunction = Context.TransferFunction.GetTransferFunction();

	if (!FlowVizRayMarch::SetVolumeTextures(
			Request.Parameters,
			Request.FieldTexture.GetReference(),
			Request.StatusTexture.GetReference(),
			/*bHasVectorTexture=*/false))
	{
		return;
	}

	SetViewCamera(Request.Parameters, MakeViewCamera(*Context.View), Context.LocalToWorld, ViewRect.Size());

	Request.bFieldIsUint =
		FFlowVizVolumeRayMarchCS::IsUintFieldFormat(Context.SlotA->ScalarLayout.DataType);

	// DISCLOSED, NOT DROPPED. A frame whose second half is not resident is
	// rendered from frame A alone, which is a defensible fallback and a silent
	// lie if nobody says so. There is no per-pixel channel for it - OutValue is
	// fully allocated - so it is logged, at Warning, once per dispatch.
	if (Request.bInterpolationDegraded)
	{
		UE_LOG(LogFlowViz, Warning,
			TEXT("Volume ray-march: interpolation degraded - the second display frame is not resident, ")
			TEXT("so this view is rendered from frame A alone and is NOT the interpolated frame that was requested."));
	}

	{
		FScopeLock Lock(&RequestLock);
		PendingRequests.Add(MoveTemp(Request));
	}
}

void FlowVizVolumeRayMarchProduction::FDispatcher::DrainView(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View) const
{
	TArray<FRequest> Requests;
	{
		FScopeLock Lock(&RequestLock);
		for (int32 Index = PendingRequests.Num() - 1; Index >= 0; --Index)
		{
			if (PendingRequests[Index].View == &View)
			{
				Requests.Add(MoveTemp(PendingRequests[Index]));
				PendingRequests.RemoveAtSwap(Index, EAllowShrinking::No);
			}
		}
	}

	if (Requests.Num() == 0)
	{
		return;
	}

	// A LUT MUST EXIST BEFORE ANY DISPATCH. SHADER_USE_PARAMETER_STRUCT binds
	// every declared resource, so a null TransferFunctionTexture is an invalid
	// dispatch, not an unstyled one. Update is a no-op once the resident
	// function already matches.
	//
	// BUILT FROM THE REQUEST, NOT FROM A CONSTANT. This used to read
	// `MakeDefault(CFDViz::ColorMaps::Default, LutDomainMin, LutDomainMax)`,
	// which meant every volume ever rendered was coloured through viridis no
	// matter what the transfer-function panel said. The cbuffer half of that
	// seam is wired in DispatchVolumeRayMarch; this is the other half, and
	// wiring only one of them leaves half the panel inert.
	//
	// PASSED WHOLE, NOT REBUILT BY MakeDefault. MakeDefault constructs a FRESH
	// default and sets two fields on it, so routing the request through it would
	// silently discard bReverseColorMap, ColorBands and the opacity curve -- the
	// three inputs BuildLut reads besides the map itself.
	//
	// ONE LUT, MANY REQUESTS. The resource is per-dispatcher and the queue can
	// hold requests from several volumes, so the last one drained wins. That is
	// the pre-existing shape -- there was only ever one table -- and it is
	// honest for the single-volume case the product has today (plan.md 10.3).
	// A second volume with a different map would be mis-coloured, which is why
	// the choice is carried per-request: the day a component owns its own
	// resource, the value is already in the right place.
	//
	// Update() is a no-op when the resident function already Equals this one, so
	// an idle viewport rebuilds nothing.
	const FRequest& LutRequest = Requests[0];
	TransferFunction.Update(LutRequest.TransferFunction);

	FRHITexture* const LutTexture = TransferFunction.GetLutTexture();
	if (LutTexture == nullptr)
	{
		// No LUT means no colour map. Rendering anyway would produce an image
		// whose colours mean nothing, which is worse than an absent volume
		// because it looks like data.
		UE_LOG(LogFlowViz, Error,
			TEXT("Volume ray-march skipped for %d request(s): the transfer-function LUT texture does not exist, ")
			TEXT("so any colours produced would be meaningless."),
			Requests.Num());
		return;
	}

	FRDGTextureRef SceneOutput = TryCreateViewFamilyTexture(GraphBuilder, *View.Family);
	if (SceneOutput == nullptr)
	{
		UE_LOG(LogFlowViz, Warning,
			TEXT("Volume ray-march produced %d result(s) with no view-family texture to composite into."),
			Requests.Num());
		return;
	}

	const ERHIFeatureLevel::Type FeatureLevel = View.GetFeatureLevel();
	FGlobalShaderMap* const ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
	TShaderMapRef<FCopyRectPS> PixelShader(ShaderMap);
	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("Volume ray-march cannot composite: the engine's screen-pass copy shaders are missing from the global shader map."));
		return;
	}

	for (FRequest& Request : Requests)
	{
		const FIntPoint OutputSize = Request.ViewRect.Size();

		// PF_A32B32G32R32F IS NOT A LUXURY. OutValue.x carries a field value in
		// solver units; an 8- or 16-bit target would quantise it silently, which
		// is exactly the failure plan.md section 4 rule 5 forbids.
		const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
			OutputSize,
			PF_A32B32G32R32F,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV);

		FRDGTextureRef OutColor = GraphBuilder.CreateTexture(Desc, TEXT("FlowVizVolumeRayMarch.Color"));
		FRDGTextureRef OutValue = GraphBuilder.CreateTexture(Desc, TEXT("FlowVizVolumeRayMarch.Value"));

		// The marcher writes every pixel it is dispatched over, but a refused
		// dispatch leaves the texture holding whatever the transient allocator
		// last had there. Clearing first makes "nothing was marched" read as
		// transparent rather than as another pass's leftovers.
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutColor)), FVector4(0.0, 0.0, 0.0, 0.0));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutValue)), FVector4(0.0, 0.0, 0.0, 0.0));

		FFlowVizVolumeRayMarchParameters* Parameters =
			GraphBuilder.AllocParameters<FFlowVizVolumeRayMarchParameters>();
		*Parameters = Request.Parameters;
		Parameters->TransferFunctionTexture = LutTexture;

		if (!FlowVizRayMarch::AddRayMarchPass(
				GraphBuilder, FeatureLevel, Request.bFieldIsUint, Parameters, OutColor, OutValue))
		{
			// AN EMPTY TARGET IS NOT DATA. AddRayMarchPass returns false when the
			// shader is missing from the global shader map - what a shader that
			// failed to compile looks like at runtime - and nothing was added, so
			// compositing here would paint a cleared texture over the scene and
			// call it a render.
			UE_LOG(LogFlowViz, Error,
				TEXT("Volume ray-march pass was refused (the ray-march shader is not in the global shader map). ")
				TEXT("Nothing is composited; the volume is absent rather than blank."));
			continue;
		}

		FCopyRectPS::FParameters* CompositeParameters =
			GraphBuilder.AllocParameters<FCopyRectPS::FParameters>();
		CompositeParameters->InputTexture = OutColor;
		CompositeParameters->InputSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		CompositeParameters->RenderTargets[0] =
			FRenderTargetBinding(SceneOutput, ERenderTargetLoadAction::ELoad);

		// AddDrawTexturePass is deliberately NOT used: it hardware-copies when
		// the formats and sizes match and otherwise blends opaque, either of
		// which would erase the scene behind the volume instead of compositing
		// over it.
		AddDrawScreenPass(
			GraphBuilder,
			RDG_EVENT_NAME("FlowVizVolumeComposite %dx%d", OutputSize.X, OutputSize.Y),
			FScreenPassViewInfo(View),
			FScreenPassTextureViewport(SceneOutput, Request.ViewRect),
			FScreenPassTextureViewport(OutColor),
			VertexShader,
			PixelShader,
			FlowVizVolumeRayMarchDispatcherLocal::GetPremultipliedOverBlendState(),
			CompositeParameters);
	}
}

void FlowVizVolumeRayMarchProduction::FDispatcher::DropRequestsOutsideFamily(
	const FSceneViewFamily& ViewFamily) const
{
	FScopeLock Lock(&RequestLock);
	for (int32 Index = PendingRequests.Num() - 1; Index >= 0; --Index)
	{
		if (!ViewFamily.Views.Contains(PendingRequests[Index].View))
		{
			PendingRequests.RemoveAtSwap(Index, EAllowShrinking::No);
		}
	}
}

void FlowVizVolumeRayMarchProduction::FDispatcher::ReleaseResources() const
{
	{
		FScopeLock Lock(&RequestLock);
		PendingRequests.Empty();
	}
	TransferFunction.ReleaseResources();
}

int32 FlowVizVolumeRayMarchProduction::FDispatcher::NumPendingRequests() const
{
	FScopeLock Lock(&RequestLock);
	return PendingRequests.Num();
}

bool FlowVizVolumeRayMarchProduction::FDispatcher::PeekRequestParameters(
	int32 Index,
	FFlowVizVolumeRayMarchParameters& OutParameters) const
{
	FScopeLock Lock(&RequestLock);
	if (!PendingRequests.IsValidIndex(Index))
	{
		return false;
	}

	// A copy under the lock. Returning a reference would hand out a pointer into
	// an array the render thread may reallocate on the next dispatch.
	OutParameters = PendingRequests[Index].Parameters;
	return true;
}

FlowVizVolumeRayMarchProduction::FDispatcher& FlowVizVolumeRayMarchProduction::GetProductionDispatcher()
{
	// FUNCTION-LOCAL STATIC, so it has static storage duration and outlives every
	// test, every map load and every view family. The pointer handed to
	// SetDispatcher can therefore never dangle, and GetDispatcher stays a plain
	// read of a global - a lazy-init inside the getter would make the wiring test
	// pass while module startup still wired nothing.
	static FDispatcher Dispatcher;
	return Dispatcher;
}

void FlowVizVolumeRayMarchProduction::FViewExtension::PostRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneViewFamily& InViewFamily)
{
	// Runs immediately before the per-view loop below, so anything recorded for a
	// view that is not in this family was recorded for a view that will never be
	// drained. Without this the queue grows for the life of the process.
	GetProductionDispatcher().DropRequestsOutsideFamily(InViewFamily);
}

void FlowVizVolumeRayMarchProduction::FViewExtension::PostRenderView_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView)
{
	GetProductionDispatcher().DrainView(GraphBuilder, InView);
}

namespace FlowVizVolumeRayMarchDispatcherLocal
{
	/** Kept alive for the module's lifetime; the registry holds only a weak pointer. */
	static TSharedPtr<FlowVizVolumeRayMarchProduction::FViewExtension, ESPMode::ThreadSafe> GViewExtension;

	static FDelegateHandle GPostEngineInitHandle;

	void CreateViewExtension()
	{
		if (!GViewExtension.IsValid())
		{
			GViewExtension =
				FSceneViewExtensions::NewExtension<FlowVizVolumeRayMarchProduction::FViewExtension>();
		}
	}
}

void FlowVizVolumeRayMarchProduction::Register()
{
	// INSTALLED IMMEDIATELY, not deferred. This is a plain global assignment and
	// is safe before GEngine exists; it is also the whole point of the wiring
	// test, which asks what MODULE STARTUP left behind.
	FlowVizVolumeRayMarch::SetDispatcher(&GetProductionDispatcher());

	// The view extension cannot be created here. FSceneViewExtensions::
	// RegisterExtension is guarded by ensure(GEngine), and this plugin loads at
	// PostConfigInit - before GEngine is constructed. Creating it here would
	// trip the ensure and register nothing, leaving a dispatcher that records
	// requests nobody ever drains.
	if (GEngine != nullptr)
	{
		FlowVizVolumeRayMarchDispatcherLocal::CreateViewExtension();
	}
	else if (!FlowVizVolumeRayMarchDispatcherLocal::GPostEngineInitHandle.IsValid())
	{
		FlowVizVolumeRayMarchDispatcherLocal::GPostEngineInitHandle =
			FCoreDelegates::GetOnPostEngineInit().AddStatic(
				&FlowVizVolumeRayMarchDispatcherLocal::CreateViewExtension);
	}
}

void FlowVizVolumeRayMarchProduction::Unregister()
{
	if (FlowVizVolumeRayMarchDispatcherLocal::GPostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(
			FlowVizVolumeRayMarchDispatcherLocal::GPostEngineInitHandle);
		FlowVizVolumeRayMarchDispatcherLocal::GPostEngineInitHandle.Reset();
	}

	// Uninstall BEFORE tearing anything down, so no proxy can record a request
	// into a dispatcher whose resources are being released underneath it.
	if (FlowVizVolumeRayMarch::GetDispatcher() == &GetProductionDispatcher())
	{
		FlowVizVolumeRayMarch::SetDispatcher(nullptr);
	}

	FlowVizVolumeRayMarchDispatcherLocal::GViewExtension.Reset();

	// RHI resources may only be released on the render thread, and the queue may
	// hold texture references the render thread is still reading.
	ENQUEUE_RENDER_COMMAND(FlowVizReleaseRayMarchDispatcher)(
		[](FRHICommandListImmediate&)
		{
			GetProductionDispatcher().ReleaseResources();
		});
	FlushRenderingCommands();
}
