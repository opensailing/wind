#pragma once
#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "StudioVolume.h"
#include "StudioProject.h"
#include "StudioVolumeComponent.generated.h"

class UVolumeTexture;
class UMaterialInstanceDynamic;

/** Owns one bounded display texture. Scientific frames never live in UObjects. */
UCLASS()
class UStudioVolumeComponent : public UProceduralMeshComponent
{
    GENERATED_BODY()
public:
    UStudioVolumeComponent(const FObjectInitializer& ObjectInitializer);
    bool Present(const FStudioVolumeRenderData& Data, const FStudioColorMapping& Mapping,
        const FStudioViewSettings& Settings, FString& Error);
    void CameraChanged(const FStudioCameraState& Camera, FIntPoint Viewport, double DefaultNearCentimeters);
    /** Release GPU storage immediately for a retired inspection scene. */
    void ClearVolume(bool bReleaseResources=false);
    int64 TextureBytes() const;
private:
    UPROPERTY(Transient) TObjectPtr<UVolumeTexture> Texture;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> Material;
    FBox Bounds = FBox(ForceInit);
};
