#include "StudioVolumeComponent.h"
#include "Engine/VolumeTexture.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TextureResource.h"
#include "RHICommandList.h"

UStudioVolumeComponent::UStudioVolumeComponent(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryComponentTick.bCanEverTick=false;
    SetCollisionEnabled(ECollisionEnabled::NoCollision);SetCastShadow(false);
    bUseAsyncCooking=false;
}
bool UStudioVolumeComponent::Present(const FStudioVolumeRenderData& Data,const FStudioColorMapping& Mapping,
    const FStudioViewSettings& Settings,FString& Error)
{
    check(IsInGameThread());
    const auto D=Data.Dimensions;
    if(!Data.Error.IsEmpty()||D.GetMin()<2||Data.Texels.Num()!=int64(D.X)*D.Y*D.Z||Data.Texels.Num()>StudioVolumes::MaximumVoxels||(!Data.OpacityTexels.IsEmpty()&&Data.OpacityTexels.Num()!=Data.Texels.Num()))
    {ClearVolume();Error=Data.Error.IsEmpty()?TEXT("Invalid volume upload."):Data.Error;return false;}
    if(!Material)
    {
        auto* Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Studio/M_FlowVolume.M_FlowVolume"));
        if(Base)Material=UMaterialInstanceDynamic::Create(Base,this);
        if(!Material){Error=TEXT("Volume ray-marching material is missing. Rebuild application materials.");return false;}
    }
    if(!Texture||Texture->GetSizeX()!=D.X||Texture->GetSizeY()!=D.Y||Texture->GetSizeZ()!=D.Z)
    {
        Texture=UVolumeTexture::CreateTransient(D.X,D.Y,D.Z,PF_G32R32F);
        if(!Texture){ClearVolume();Error=TEXT("Volume texture allocation failed.");return false;}
        Texture->SRGB=false;Texture->Filter=TF_Bilinear;Texture->NeverStream=true;Texture->AddressMode=TA_Clamp;
        Texture->UpdateResource();
        Material->SetTextureParameterValue(TEXT("VolumeScalars"),Texture);
    }
    auto Upload=MakeShared<TArray<FVector2f>,ESPMode::ThreadSafe>(Data.Texels);
    FTextureResource* Resource=Texture->GetResource();
    if(!Resource){ClearVolume();Error=TEXT("Volume GPU resource is unavailable.");return false;}
    // Resource creation, this upload and a later resource release are queued in
    // order on the render thread. The immutable copy owns bytes until consumed.
    ENQUEUE_RENDER_COMMAND(StudioVolumeUpload)([Resource,Upload,D](FRHICommandListImmediate& RHICmdList)
    {
        if(Resource->TextureRHI)
        {
            const FUpdateTextureRegion3D Region(0,0,0,0,0,0,D.X,D.Y,D.Z);
            RHICmdList.UpdateTexture3D(Resource->TextureRHI,0,Region,D.X*sizeof(FVector2f),D.X*D.Y*sizeof(FVector2f),
                reinterpret_cast<const uint8*>(Upload->GetData()));
        }
    });
    if(!Data.OpacityTexels.IsEmpty())
    {
        if(!OpacityTexture||OpacityTexture->GetSizeX()!=D.X||OpacityTexture->GetSizeY()!=D.Y||OpacityTexture->GetSizeZ()!=D.Z)
        {
            if(OpacityTexture)OpacityTexture->ReleaseResource();
            OpacityTexture=UVolumeTexture::CreateTransient(D.X,D.Y,D.Z,PF_R32_FLOAT);
            if(!OpacityTexture){ClearVolume();Error=TEXT("Vorticity opacity allocation failed.");return false;}
            OpacityTexture->SRGB=false;OpacityTexture->Filter=TF_Nearest;OpacityTexture->NeverStream=true;OpacityTexture->AddressMode=TA_Clamp;
            OpacityTexture->UpdateResource();
        }
        auto Alpha=MakeShared<TArray<float>,ESPMode::ThreadSafe>(Data.OpacityTexels);FTextureResource* AlphaResource=OpacityTexture->GetResource();
        if(!AlphaResource){ClearVolume();Error=TEXT("Vorticity opacity resource unavailable.");return false;}
        ENQUEUE_RENDER_COMMAND(StudioVorticityUpload)([AlphaResource,Alpha,D](FRHICommandListImmediate& RHICmdList)
        {
            if(AlphaResource->TextureRHI)RHICmdList.UpdateTexture3D(AlphaResource->TextureRHI,0,FUpdateTextureRegion3D(0,0,0,0,0,0,D.X,D.Y,D.Z),D.X*sizeof(float),D.X*D.Y*sizeof(float),reinterpret_cast<const uint8*>(Alpha->GetData()));
        });
        Material->SetTextureParameterValue(TEXT("OpacityScalars"),OpacityTexture);Material->SetScalarParameterValue(TEXT("IndependentOpacity"),1);
    }
    else
    {
        Material->SetScalarParameterValue(TEXT("IndependentOpacity"),0);Material->SetTextureParameterValue(TEXT("OpacityScalars"),Texture);
        if(OpacityTexture)OpacityTexture->ReleaseResource();OpacityTexture=nullptr;
    }
    const FVector Lo=Data.SourceBounds.Min,Hi=Data.SourceBounds.Max;
    const FBox SceneBounds(FVector(Lo.X,Lo.Z,Lo.Y)*100.,FVector(Hi.X,Hi.Z,Hi.Y)*100.);
    Bounds=SceneBounds;
    SetMaterial(0,Material);
    auto Vector=[&](const TCHAR* Name,const FVector& V){Material->SetVectorParameterValue(Name,FLinearColor(V.X,V.Y,V.Z,0));};
    Vector(TEXT("VolumeMinimum"),Bounds.Min);Vector(TEXT("VolumeSize"),Bounds.GetSize());
    Vector(TEXT("GridDimensions"),FVector(D));
    Vector(TEXT("SourceMinimum"),Data.SourceBounds.Min);Vector(TEXT("SourceSize"),Data.SourceBounds.GetSize());
    Vector(TEXT("Cylinder"),FVector(Data.CylinderCenter.X,Data.CylinderCenter.Y,Data.CylinderRadius));
    Vector(TEXT("ClipMinimum"),Settings.VolumeClipMinimum);Vector(TEXT("ClipMaximum"),Settings.VolumeClipMaximum);
    Vector(TEXT("OpacityCurve"),Settings.VolumeOpacityCurve);
    Material->SetScalarParameterValue(TEXT("Opacity"),Settings.VolumeOpacity);
    Material->SetScalarParameterValue(TEXT("Palette"),Mapping.Palette);
    Material->SetVectorParameterValue(TEXT("LowColor"),Mapping.LowColor);
    Material->SetVectorParameterValue(TEXT("MiddleColor"),Mapping.MiddleColor);
    Material->SetVectorParameterValue(TEXT("HighColor"),Mapping.HighColor);
    Material->SetScalarParameterValue(TEXT("StepVoxels"),Settings.VolumeStepVoxels);
    const double Span=Mapping.Maximum-Mapping.Minimum;
    // Physical thresholds stay fixed when palette bounds or frame extrema change.
    const auto Normalize=[&](double Value){return Span>0?FMath::Clamp((Value-Mapping.Minimum)/Span,-1.e30,1.e30):
        Value<Mapping.Minimum?-1.e30:Value>Mapping.Maximum?1.e30:.5;};
    Material->SetScalarParameterValue(TEXT("ThresholdMinimum"),Normalize(Settings.VolumeThresholdMinimum));
    Material->SetScalarParameterValue(TEXT("ThresholdMaximum"),Normalize(Settings.VolumeThresholdMaximum));
    Material->SetScalarParameterValue(TEXT("ThresholdEnabled"),Settings.bVolumeThreshold);
    SetVisibility(true);return true;
}
void UStudioVolumeComponent::CameraChanged(const FStudioCameraState& Camera,FIntPoint Viewport,double DefaultNearCentimeters)
{
    if(!Material||!Texture||Viewport.X<=0||Viewport.Y<=0)return;
    const FVector P=Camera.Position*100.,D=Camera.Orientation.GetForwardVector();
    Material->SetVectorParameterValue(TEXT("RenderCameraPosition"),FLinearColor(P.X,P.Y,P.Z,0));
    Material->SetVectorParameterValue(TEXT("RenderCameraForward"),FLinearColor(D.X,D.Y,D.Z,0));
    Material->SetScalarParameterValue(TEXT("Orthographic"),Camera.bOrthographic);
    const double Near=Camera.bDepthClipping?Camera.NearClipMeters*100.:Camera.bOrthographic?0:DefaultNearCentimeters;
    const double Far=Camera.bDepthClipping?Camera.FarClipMeters*100.:1.e20;
    Material->SetScalarParameterValue(TEXT("CameraNearDepth"),Near);
    Material->SetScalarParameterValue(TEXT("CameraFarDepth"),Far);
    // A view-facing proxy survives near/far clipping even when the volume's
    // exit faces lie outside the camera frustum or the camera is inside it.
    const double Distance=FMath::Min(Near+FMath::Max(.1,Near*.1),(Near+Far)*.5);
    const double HalfWidth=Camera.bOrthographic?Camera.OrthoWidth*50.:
        FMath::Tan(FMath::DegreesToRadians(Camera.FieldOfView*.5))*Distance;
    const double HalfHeight=HalfWidth*Viewport.Y/Viewport.X;
    const FVector Center=P+D*Distance,R=Camera.Orientation.GetRightVector()*HalfWidth*1.001,
        U=Camera.Orientation.GetUpVector()*HalfHeight*1.001;
    const TArray<FVector> Vertices={Center-R-U,Center+R-U,Center+R+U,Center-R+U};
    const TArray<int32> Indices={0,2,1,0,3,2};
    const TArray<FVector> Normals={-D,-D,-D,-D};
    const TArray<FVector2D> UVs={FVector2D(0,0),FVector2D(1,0),FVector2D(1,1),FVector2D(0,1)};
    CreateMeshSection_LinearColor(0,Vertices,Indices,Normals,UVs,{}, {},false,false);
    SetMaterial(0,Material);
}
void UStudioVolumeComponent::ClearVolume(bool bReleaseResources)
{
    ClearAllMeshSections();SetVisibility(false);Bounds=FBox(ForceInit);
    if(Material){Material->SetTextureParameterValue(TEXT("VolumeScalars"),nullptr);Material->SetTextureParameterValue(TEXT("OpacityScalars"),nullptr);Material->SetScalarParameterValue(TEXT("IndependentOpacity"),0);}
    if(OpacityTexture)OpacityTexture->ReleaseResource();OpacityTexture=nullptr;
    if(bReleaseResources&&Texture)Texture->ReleaseResource();
    Texture=nullptr;
}
int64 UStudioVolumeComponent::TextureBytes() const
{ return (Texture?int64(Texture->GetSizeX())*Texture->GetSizeY()*Texture->GetSizeZ()*sizeof(FVector2f):0)+(OpacityTexture?int64(OpacityTexture->GetSizeX())*OpacityTexture->GetSizeY()*OpacityTexture->GetSizeZ()*sizeof(float):0); }
