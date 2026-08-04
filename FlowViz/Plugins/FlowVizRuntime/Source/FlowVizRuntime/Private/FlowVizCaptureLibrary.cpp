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
#include "RHI.h"
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

	// RTF_RGBA8_SRGB is required, not preferred: ExportRenderTarget and the
	// image wrappers only produce a PNG for 8-bit formats, and float formats are
	// silently routed to the EXR/HDR path instead - producing "success" and no
	// .png file, which is precisely the original symptom in this project.
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
