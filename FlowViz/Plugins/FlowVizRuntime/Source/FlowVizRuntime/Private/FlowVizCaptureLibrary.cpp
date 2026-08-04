// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizCaptureLibrary.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FlowVizRuntime.h"
#include "RenderingThread.h"

bool UFlowVizCaptureLibrary::FlushSceneUpdates(const UObject* WorldContextObject)
{
	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;

	if (World == nullptr)
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("FlushSceneUpdates: could not resolve a world from the supplied context object."));
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

int32 UFlowVizCaptureLibrary::GetRegisteredPrimitiveCount(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;

	if (World == nullptr)
	{
		UE_LOG(LogFlowViz, Error,
			TEXT("GetRegisteredPrimitiveCount: could not resolve a world from the supplied context object."));
		return INDEX_NONE;
	}

	int32 Count = 0;
	for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
	{
		const UPrimitiveComponent* Component = *It;
		if (Component->GetWorld() == World && Component->IsRegistered() && Component->SceneProxy != nullptr)
		{
			++Count;
		}
	}

	UE_LOG(LogFlowViz, Log, TEXT("Registered primitives with scene proxies: %d"), Count);
	return Count;
}
