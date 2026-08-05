// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizCaptureLibrary.h"

#include "AssetCompilingManager.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Engine.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "FlowVizRuntime.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Render/FlowVizVolumeRayMarchDispatcher.h"
#include "RHI.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "RenderingThread.h"
#include "SceneInterface.h"
#include "TextureResource.h"
#include "UObject/UObjectIterator.h"

namespace
{
	UWorld* ResolveWorld(const UObject* WorldContextObject, const TCHAR* Caller)
	{
		UWorld* World = GEngine
			? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
			: nullptr;

		if (World == nullptr)
		{
			UE_LOG(LogFlowViz, Error,
				TEXT("%s: could not resolve a world from the supplied context object."), Caller);
		}

		return World;
	}
}

bool UFlowVizCaptureLibrary::FlushSceneUpdates(const UObject* WorldContextObject)
{
	UWorld* World = ResolveWorld(WorldContextObject, TEXT("FlushSceneUpdates"));
	if (World == nullptr)
	{
		return false;
	}

	// Creates the scene proxies for any component whose render state is dirty.
	// In a normal game tick this happens every frame; in a commandlet it never
	// happens at all unless called explicitly.
	World->SendAllEndOfFrameUpdates();

	// Wait for the render thread to consume those commands, so a capture issued
	// on the next line sees the updated scene rather than racing it.
	FlushRenderingCommands();

	return true;
}

int32 UFlowVizCaptureLibrary::GetSceneProxyCount(const UObject* WorldContextObject)
{
	const UWorld* World = ResolveWorld(WorldContextObject, TEXT("GetSceneProxyCount"));
	if (World == nullptr)
	{
		return INDEX_NONE;
	}

	// No render scene at all - typical under -nullrhi. Reporting 0 here would be
	// a lie of exactly the kind this function exists to prevent, because "the
	// renderer is absent" and "the scene is empty" demand opposite responses.
	if (World->Scene == nullptr)
	{
		UE_LOG(LogFlowViz, Warning,
			TEXT("GetSceneProxyCount: world '%s' has no render scene (running with -nullrhi?)."),
			*World->GetName());
		return INDEX_NONE;
	}

	// A world can hold an FNULLSceneInterface stub instead of a real FScene:
	// UWorld::AllocateScene falls back to it whenever GIsClient is false,
	// FApp::CanEverRender() is false, or the null RHI is active. That stub
	// returns an empty proxy array unconditionally, so without this check a
	// commandlet would report a confident, permanent zero and send the reader
	// hunting for missing geometry that was never the problem.
	if (World->Scene->GetRenderScene() == nullptr)
	{
		UE_LOG(LogFlowViz, Warning,
			TEXT("GetSceneProxyCount: world '%s' has a null scene stub, not a real FScene. ")
			TEXT("GIsClient=%d CanEverRender=%d UsingNullRHI=%d. No primitive can render here, ")
			TEXT("whatever the world contains."),
			*World->GetName(), GIsClient ? 1 : 0, FApp::CanEverRender() ? 1 : 0,
			GUsingNullRHI ? 1 : 0);
		return INDEX_NONE;
	}

	// Ask the renderer's own structure. Counting components with a non-null
	// SceneProxy pointer instead would not fall when actors are destroyed, since
	// they persist until the next GC with that pointer stale.
	int32 Count = 0;
	{
		// Required before draining the queue. UpdateAllPrimitiveSceneInfos
		// asserts !IsReplaying(), so issuing it from inside a replayed command
		// crashes the render thread outright; the engine takes this same scope
		// in FScene::ApplyWorldOffset for the same reason.
		UE::RenderCommandPipe::FSyncScope SyncScope;

		ENQUEUE_RENDER_COMMAND(FlowVizCountSceneProxies)(
			[Scene = World->Scene, &Count](FRHICommandListImmediate& RHICmdList)
			{
				// Adds and removals sit in a pending queue that is normally
				// drained while rendering a frame. Without this the array
				// reports the state as of the last frame drawn - which in a
				// commandlet that has never drawn one is a permanent, and
				// entirely convincing, zero.
				Scene->UpdateAllPrimitiveSceneInfos(RHICmdList);

				Count = Scene->GetPrimitiveSceneProxies().Num();
			});
	}

	// Required for correctness, not just ordering: Count is a stack reference
	// captured by the render command, so it must not go out of scope first.
	FlushRenderingCommands();

	UE_LOG(LogFlowViz, Log, TEXT("Render scene holds %d primitive proxies."), Count);
	return Count;
}

int32 UFlowVizCaptureLibrary::LogPrimitiveBreakdown(const UObject* WorldContextObject)
{
	const UWorld* World = ResolveWorld(WorldContextObject, TEXT("LogPrimitiveBreakdown"));
	if (World == nullptr)
	{
		return INDEX_NONE;
	}

	int32 Total = 0;
	int32 Registered = 0;
	int32 RegisteredWithoutProxy = 0;

	for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
	{
		UPrimitiveComponent* Component = *It;

		// Objects awaiting destruction still answer GetWorld(); including them
		// is how the previous implementation produced a count that never fell.
		if (!IsValid(Component) || Component->GetWorld() != World)
		{
			continue;
		}

		++Total;

		const bool bRegistered = Component->IsRegistered();
		const bool bHasProxy = Component->SceneProxy != nullptr;

		if (bRegistered)
		{
			++Registered;
		}

		if (bRegistered && !bHasProxy)
		{
			++RegisteredWithoutProxy;

			// The interesting case, named individually: registered with no proxy
			// is the signature of a missing render-state flush.
			UE_LOG(LogFlowViz, Warning,
				TEXT("  %s (%s): registered, NO scene proxy - visible=%s"),
				*Component->GetName(),
				*Component->GetClass()->GetName(),
				Component->IsVisible() ? TEXT("true") : TEXT("false"));
		}
	}

	UE_LOG(LogFlowViz, Log,
		TEXT("Primitives in '%s': %d live, %d registered, %d registered without a proxy."),
		*World->GetName(), Total, Registered, RegisteredWithoutProxy);

	// The two numbers to compare. A large gap is the diagnosis; agreement means
	// the scene really is as empty as it looks and the cause is upstream.
	UE_LOG(LogFlowViz, Log,
		TEXT("Compare against the render scene: %d proxies."),
		GetSceneProxyCount(WorldContextObject));

	return Total;
}

ASceneCapture2D* UFlowVizCaptureLibrary::SpawnSceneCapture2D(
	const UObject* WorldContextObject,
	FVector Location,
	FRotator Rotation,
	float FOV)
{
	UWorld* World = ResolveWorld(WorldContextObject, TEXT("SpawnSceneCapture2D"));
	if (World == nullptr)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	// A capture camera is instrumentation, not level content; it must never
	// block on collision with whatever happens to be at the requested viewpoint.
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ASceneCapture2D* Capture =
		World->SpawnActor<ASceneCapture2D>(Location, Rotation, SpawnParams);

	if (Capture == nullptr)
	{
		UE_LOG(LogFlowViz, Error, TEXT("SpawnSceneCapture2D: SpawnActor returned null."));
		return nullptr;
	}

	USceneCaptureComponent2D* Component = Capture->GetCaptureComponent2D();
	if (Component == nullptr)
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("SpawnSceneCapture2D: spawned actor has no capture component."));
		return nullptr;
	}

	Component->FOVAngle = FOV;

	// Capture only when asked. Every-frame capture would make the result depend
	// on when it happens to be read, which is neither testable nor reproducible
	// - and in a commandlet, which may never tick, it would never fire at all.
	Component->bCaptureEveryFrame = false;
	Component->bCaptureOnMovement = false;

	// Final LDR colour is the only source that survives an 8-bit PNG intact.
	// The HDR sources need float formats and take the EXR path on export.
	Component->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

	// SpawnActor placed the actor, but the component's own transform is what the
	// renderer reads; set it explicitly rather than trusting attachment order.
	Component->SetWorldLocationAndRotation(Location, Rotation);

	return Capture;
}

int32 UFlowVizCaptureLibrary::ResolveMaterials(const UObject* WorldContextObject)
{
	const UWorld* World = ResolveWorld(WorldContextObject, TEXT("ResolveMaterials"));
	if (World == nullptr)
	{
		return INDEX_NONE;
	}

	int32 Resolved = 0;
	for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
	{
		UPrimitiveComponent* Primitive = *It;
		if (!IsValid(Primitive) || Primitive->GetWorld() != World)
		{
			continue;
		}

		const int32 NumMaterials = Primitive->GetNumMaterials();
		for (int32 Index = 0; Index < NumMaterials; ++Index)
		{
			if (UMaterialInterface* Material = Primitive->GetMaterial(Index))
			{
				// Resolving the render proxy is what actually forces the load;
				// fetching the pointer alone can be satisfied by a stub that
				// still renders black.
				Material->GetRenderProxy();
				++Resolved;
			}
		}
	}

	UE_LOG(LogFlowViz, Log, TEXT("Resolved %d materials."), Resolved);
	return Resolved;
}

bool UFlowVizCaptureLibrary::CaptureToPNG(
	const UObject* WorldContextObject,
	const FString& OutputPath,
	FVector Location,
	FRotator Rotation,
	int32 Width,
	int32 Height,
	float FOV)
{
	UWorld* World = ResolveWorld(WorldContextObject, TEXT("CaptureToPNG"));
	if (World == nullptr)
	{
		return false;
	}

	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("CaptureToPNG: invalid resolution %dx%d."), Width, Height);
		return false;
	}

	// Report the scene's state alongside every capture. A black PNG is
	// ambiguous on its own, and this is the number that disambiguates it.
	const int32 ProxyCount = GetSceneProxyCount(World);
	UE_LOG(LogFlowViz, Log,
		TEXT("CaptureToPNG: %dx%d from %s, render scene holds %d proxies."),
		Width, Height, *Location.ToCompactString(), ProxyCount);

	ASceneCapture2D* Capture = SpawnSceneCapture2D(World, Location, Rotation, FOV);
	if (Capture == nullptr)
	{
		return false;
	}

	// Cleaning up matters here: callers take many captures from one process, and
	// a leaked capture actor is a live camera rendering into a live target every
	// subsequent frame.
	ON_SCOPE_EXIT
	{
		Capture->Destroy();
	};

	USceneCaptureComponent2D* Component = Capture->GetCaptureComponent2D();

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(Capture);

	// 8-bit because ReadPixels below fills a TArray<FColor> (BGRA8) and the
	// FImageView handed to CompressImage declares ERawImageFormat::BGRA8. A
	// float target would make that declaration a lie about the bytes.
	//
	// THE EXR HAZARD DOES NOT APPLY TO THIS FUNCTION, despite what an earlier
	// version of this comment said. That hazard - float formats silently taking
	// the EXR/HDR path, producing "success" and no .png - belongs to
	// ExportRenderTarget, which decides the container from the target's format.
	// This function never calls it: it reads pixels back itself, passes
	// TEXT("png") to CompressImage explicitly, and then stats the file. The
	// container here is chosen by the literal above, not inferred from a format,
	// so no format could route it to EXR.
	//
	// The live instance of that hazard is Tools/capture/_capture_worker.py:252,
	// which DOES call export_render_target, 77 lines after choosing the format
	// at :175. It is guarded end to end rather than at the call: the worker
	// stats the file and verdict.py:113 fails an unwritten PNG, asserted by
	// tests/test_verdict.py::test_unwritten_file_is_not_a_pass. Keep that guard
	// if this comment tempts anyone to add a redundant one here.
	RenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8_SRGB;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->InitAutoFormat(Width, Height);
	RenderTarget->UpdateResourceImmediate(true);

	Component->TextureTarget = RenderTarget;

	ResolveMaterials(World);

	// Materials and shaders compiled on demand render as the default grey (or
	// nothing) until they are ready. Without this wait the first captures of a
	// session are a coin flip.
	FAssetCompilingManager::Get().FinishAllCompilation();

	// Push any pending render-state creation into the scene before drawing.
	FlushSceneUpdates(World);

	// Several captures, not one. The first frame is often blank while temporal
	// effects, streaming and auto-exposure settle.
	constexpr int32 WarmUpCaptures = 5;
	for (int32 Index = 0; Index < WarmUpCaptures; ++Index)
	{
		Component->CaptureScene();
		FlushRenderingCommands();
	}

	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (Resource == nullptr)
	{
		UE_LOG(LogFlowViz, Error, TEXT("CaptureToPNG: render target has no resource."));
		return false;
	}

	TArray<FColor> Pixels;
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
	ReadFlags.SetLinearToGamma(false);

	if (!Resource->ReadPixels(Pixels, ReadFlags))
	{
		UE_LOG(LogFlowViz, Error, TEXT("CaptureToPNG: ReadPixels failed."));
		return false;
	}

	if (Pixels.Num() != Width * Height)
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("CaptureToPNG: read %d pixels, expected %d."), Pixels.Num(), Width * Height);
		return false;
	}

	// PNG treats alpha as real. Scene captures routinely produce zero alpha,
	// which yields a fully transparent image that many tools then composite onto
	// black - reading as "the capture failed" when the colour data is fine.
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}

	const FString AbsolutePath = FPaths::ConvertRelativePathToFull(OutputPath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);

	TArray64<uint8> PngData;
	FImageView ImageView(Pixels.GetData(), Width, Height, ERawImageFormat::BGRA8);
	if (!FImageUtils::CompressImage(PngData, TEXT("png"), ImageView, /*Quality*/ 0))
	{
		UE_LOG(LogFlowViz, Error, TEXT("CaptureToPNG: PNG compression failed."));
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(PngData, *AbsolutePath))
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("CaptureToPNG: could not write '%s'."), *AbsolutePath);
		return false;
	}

	// Confirm from the filesystem rather than from having reached this line. The
	// original bug in this project logged success and wrote nothing, so the only
	// acceptable evidence is a file with a non-zero size.
	const int64 FileSize = IFileManager::Get().FileSize(*AbsolutePath);
	if (FileSize <= 0)
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("CaptureToPNG: '%s' is missing or empty after write."), *AbsolutePath);
		return false;
	}

	UE_LOG(LogFlowViz, Log,
		TEXT("CaptureToPNG: wrote '%s' (%lld bytes)."), *AbsolutePath, FileSize);
	return true;
}

ACFDVizCaseActor* UFlowVizCaptureLibrary::SpawnCaseActor(
	const UObject* WorldContextObject,
	const FString& CaseDirectory,
	FName FieldId,
	FVector Location,
	FRotator Rotation,
	FString& OutError,
	int32 FrameIndex,
	bool bDrawBoundingBox)
{
	OutError.Reset();

	UWorld* World = ResolveWorld(WorldContextObject, TEXT("SpawnCaseActor"));
	if (World == nullptr)
	{
		OutError = TEXT("could not resolve a world from the supplied context object");
		return nullptr;
	}

	/*
	 * REFUSED, NOT DEFAULTED. LoadCase(NAME_None) picks the manifest's first
	 * non-mask field, which in the shipped sample is the 3-component `U`. That
	 * load succeeds, the upload succeeds, and the volume then renders its hull
	 * and nothing else because the dispatcher early-returns on a null
	 * ScalarTexture. Defaulting here would make the most convenient call the
	 * one that produces an un-marchable volume.
	 */
	if (FieldId.IsNone())
	{
		OutError = TEXT("no field was named. This function will not pick one for you: the "
			"manifest's first field is typically a vector, whose bytes never reach the "
			"scalar texture the ray-marcher samples, so the volume would draw its hull "
			"and march nothing. Name a scalar field explicitly.");
		UE_LOG(LogFlowViz, Error, TEXT("SpawnCaseActor: %s"), *OutError);
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	// A case placed for a capture is instrumentation; it must never fail to
	// appear because something happens to occupy the requested transform.
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ACFDVizCaseActor* Actor =
		World->SpawnActor<ACFDVizCaseActor>(Location, Rotation, SpawnParams);

	if (Actor == nullptr)
	{
		OutError = TEXT("SpawnActor returned null");
		UE_LOG(LogFlowViz, Error, TEXT("SpawnCaseActor: %s."), *OutError);
		return nullptr;
	}

	/*
	 * Every failure below destroys the actor before returning. A half-configured
	 * case actor left in the world is worse than none: it has a scene proxy, so
	 * it draws, and it has no parameters, so it never dispatches - the exact
	 * pairing that makes a dead marcher photograph like a live one.
	 */
	bool bSucceeded = false;
	ON_SCOPE_EXIT
	{
		if (!bSucceeded && IsValid(Actor))
		{
			World->DestroyActor(Actor);
		}
	};

	UCFDVizVolumeComponent* Volume = Actor->GetVolumeComponent();
	if (Volume == nullptr)
	{
		OutError = TEXT("the spawned case actor has no volume component");
		UE_LOG(LogFlowViz, Error, TEXT("SpawnCaseActor: %s."), *OutError);
		return nullptr;
	}

	const FCFDVizResult LoadResult = Actor->LoadCase(CaseDirectory, FieldId);
	if (!LoadResult.IsOk())
	{
		OutError = FString::Printf(TEXT("could not load '%s' field '%s': %s"),
			*CaseDirectory, *FieldId.ToString(), *LoadResult.ToString());
		UE_LOG(LogFlowViz, Error, TEXT("SpawnCaseActor: %s"), *OutError);
		return nullptr;
	}

	/*
	 * THE SCALAR CHECK, AFTER THE LOAD BECAUSE THE MANIFEST IS WHAT KNOWS.
	 * A vector field loads and uploads without complaint; the failure appears
	 * only later, on the render thread, as a dispatch that early-returns. This
	 * is the last point at which it can be reported to a caller at all.
	 */
	const FCFDVizField* const Field =
		Volume->GetCaseBinding().Case.FindField(Volume->GetCaseBinding().FieldId);
	if (Field == nullptr)
	{
		OutError = FString::Printf(
			TEXT("field '%s' is not in the loaded manifest"), *FieldId.ToString());
		UE_LOG(LogFlowViz, Error, TEXT("SpawnCaseActor: %s."), *OutError);
		return nullptr;
	}

	if (Field->ComponentCount != 1)
	{
		OutError = FString::Printf(
			TEXT("field '%s' has %d components; the ray-marcher samples a scalar texture, so "
				 "a multi-component field's bytes land in the vector texture instead, leaving "
				 "Slot.ScalarTexture null and DispatchVolumeRayMarch early-returning. The "
				 "volume would draw its hull and march nothing"),
			*FieldId.ToString(), Field->ComponentCount);
		UE_LOG(LogFlowViz, Error, TEXT("SpawnCaseActor: %s."), *OutError);
		return nullptr;
	}

	/*
	 * THE STEP NOTHING IN PRODUCTION PERFORMS. ACFDVizCaseActor::BeginPlay
	 * loads and never uploads, and a component with no upload has no
	 * UploadedScalarLayout, so TryMakeShaderParameters returns false and the
	 * proxy's bHasParameters stays false. The proxy then draws its box and its
	 * hull and never touches the dispatcher. Without this line the actor looks
	 * completely healthy and marches nothing.
	 */
	const FCFDVizResult UploadResult = Volume->UploadFrame(FrameIndex);
	if (!UploadResult.IsOk())
	{
		OutError = FString::Printf(TEXT("could not upload frame %d of '%s': %s"),
			FrameIndex, *FieldId.ToString(), *UploadResult.ToString());
		UE_LOG(LogFlowViz, Error, TEXT("SpawnCaseActor: %s"), *OutError);
		return nullptr;
	}

	Volume->bDrawBoundingBox = bDrawBoundingBox;
	Volume->MarkRenderStateDirty();

	// Verified rather than assumed. This is the condition the proxy actually
	// gates its dispatch on, so checking it here is checking the thing that
	// matters instead of checking that the calls above returned Ok.
	FFlowVizVolumeShaderParameters Params;
	if (!Volume->TryMakeShaderParameters(Params))
	{
		OutError = FString::Printf(
			TEXT("'%s' loaded and uploaded but produces no shader parameters, so the scene "
				 "proxy's bHasParameters would be false and it would never call the "
				 "dispatcher"),
			*FieldId.ToString());
		UE_LOG(LogFlowViz, Error, TEXT("SpawnCaseActor: %s."), *OutError);
		return nullptr;
	}

	// Push the proxy and the dynamic data to the render thread now, so a capture
	// issued on the next line sees a volume rather than racing its creation. In
	// a commandlet nothing ticks, so this never happens on its own.
	FlushSceneUpdates(World);

	UE_LOG(LogFlowViz, Log,
		TEXT("SpawnCaseActor: '%s' field '%s' frame %d, colour domain [%f, %f], box=%d."),
		*CaseDirectory, *FieldId.ToString(), FrameIndex,
		Params.ValueRangeMin, Params.ValueRangeMax, bDrawBoundingBox ? 1 : 0);

	bSucceeded = true;
	return Actor;
}

bool UFlowVizCaptureLibrary::SetVolumeRayMarcherEnabled(bool bEnabled)
{
	if (bEnabled)
	{
		/*
		 * Re-installs THE PRODUCTION DISPATCHER, not a fresh one. Register() is
		 * idempotent with respect to the global - it is a plain assignment of
		 * the address of a function-local static - so calling it again after an
		 * uninstall restores exactly the pointer module startup left behind.
		 * Allocating a new dispatcher here would leak, and worse, would not be
		 * the object the view extension drains.
		 */
		FlowVizVolumeRayMarchProduction::Register();
	}
	else
	{
		FlowVizVolumeRayMarch::SetDispatcher(nullptr);
	}

	// The render thread may be midway through a frame that already read the old
	// value. Without this flush a capture issued immediately afterwards can
	// straddle the change, which would put pixels from both states in one image
	// and make the difference between them unattributable.
	FlushRenderingCommands();

	const bool bInstalled = IsVolumeRayMarcherEnabled();
	UE_LOG(LogFlowViz, Log,
		TEXT("SetVolumeRayMarcherEnabled(%d): dispatcher installed = %d."),
		bEnabled ? 1 : 0, bInstalled ? 1 : 0);

	// Reports what IS, not what was asked for. A caller asserting on the return
	// value is then asserting about the global rather than about its own input.
	return bInstalled;
}

bool UFlowVizCaptureLibrary::IsVolumeRayMarcherEnabled()
{
	return FlowVizVolumeRayMarch::GetDispatcher() != nullptr;
}

namespace
{
	/**
	 * The volume behind a case actor, or null with a diagnostic naming the caller.
	 *
	 * Shared by both setters so a null actor produces the same refusal from
	 * either, and so neither can be written to dereference first and check after.
	 */
	UCFDVizVolumeComponent* ResolveVolume(ACFDVizCaseActor* CaseActor, const TCHAR* Caller)
	{
		if (CaseActor == nullptr)
		{
			UE_LOG(LogFlowViz, Error, TEXT("%s: no case actor was supplied."), Caller);
			return nullptr;
		}

		UCFDVizVolumeComponent* Volume = CaseActor->GetVolumeComponent();
		if (Volume == nullptr)
		{
			UE_LOG(LogFlowViz, Error,
				TEXT("%s: the case actor has no volume component."), Caller);
			return nullptr;
		}

		return Volume;
	}
}

bool UFlowVizCaptureLibrary::SetVolumeCompositeMode(
	ACFDVizCaseActor* CaseActor, int32 CompositeMode, float IsoValue)
{
	UCFDVizVolumeComponent* Volume = ResolveVolume(CaseActor, TEXT("SetVolumeCompositeMode"));
	if (Volume == nullptr)
	{
		return false;
	}

	/*
	 * READ-MODIFY-WRITE, NOT A FRESH VIEW MODEL. Assigning a default-constructed
	 * one here would make every mode selection also a silent reset of lighting,
	 * steps, jitter and the rest -- the same class of defect as the dispatcher's
	 * settings source, which must mutate FillDefaults' output rather than replace
	 * it or the geometry and camera rows are erased. FlowViz.Capture.RenderSettings
	 * asserts that lighting survives a mode change and vice versa.
	 */
	FFlowVizRenderSettingsViewModel Settings = Volume->GetRenderSettings();

	/*
	 * VALIDATED, NOT CAST. static_cast<EFlowVizCompositeMode>(99) is a legal cast
	 * and an illegal mode: the .usf switches on this value and an unrecognised
	 * one falls through to a default branch, rendering as a mode nobody selected.
	 * SetCompositeModeByValue refuses it and keeps the previous mode, so a bad
	 * number leaves the volume rendering what it rendered before.
	 */
	if (CompositeMode < 0
		|| !Settings.SetCompositeModeByValue(static_cast<uint32>(CompositeMode)))
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("SetVolumeCompositeMode: %d is not a composite mode; the volume keeps mode %u. "
				 "Valid values: 0 Alpha, 1 Maximum, 2 Minimum, 3 Average, 4 IsoSurface, "
				 "5 Diagnostic."),
			CompositeMode, static_cast<uint32>(Volume->GetRenderSettings().GetCompositeMode()));
		return false;
	}

	// Applied whatever the mode, so switching into IsoSurface later does not need
	// a second call. Refused separately: a non-finite threshold finds no crossing
	// and renders an empty iso-surface, which looks like data out of range.
	if (!Settings.SetIsoValue(IsoValue))
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("SetVolumeCompositeMode: iso value %f is not finite; nothing was changed."),
			IsoValue);
		return false;
	}

	// THE CALL THAT DID NOT EXIST ANYWHERE IN PRODUCTION.
	Volume->SetRenderSettings(Settings);

	UE_LOG(LogFlowViz, Log,
		TEXT("SetVolumeCompositeMode: mode %d, iso %f applied to '%s'."),
		CompositeMode, IsoValue, *CaseActor->GetName());

	return true;
}

bool UFlowVizCaptureLibrary::SetVolumeLightingEnabled(ACFDVizCaseActor* CaseActor, bool bEnabled)
{
	UCFDVizVolumeComponent* Volume = ResolveVolume(CaseActor, TEXT("SetVolumeLightingEnabled"));
	if (Volume == nullptr)
	{
		return false;
	}

	FFlowVizRenderSettingsViewModel Settings = Volume->GetRenderSettings();
	Settings.SetLightingEnabled(bEnabled);
	Volume->SetRenderSettings(Settings);

	UE_LOG(LogFlowViz, Log, TEXT("SetVolumeLightingEnabled(%d) applied to '%s'."),
		bEnabled ? 1 : 0, *CaseActor->GetName());

	return true;
}
