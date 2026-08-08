// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizStudioRig.h"

#include "Components/DirectionalLightComponent.h"
#include "Scene/FlowVizCaseActor.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizStudioRigLocal
{
	const FName KeyName(TEXT("FlowVizRigKey"));
	const FName FillName(TEXT("FlowVizRigFill"));
	const FName RimName(TEXT("FlowVizRigRim"));

	UDirectionalLightComponent* FindRigLight(const ACFDVizCaseActor& Actor, FName Name)
	{
		for (UActorComponent* Component : Actor.GetComponents())
		{
			if (Component != nullptr && Component->GetFName() == Name)
			{
				return Cast<UDirectionalLightComponent>(Component);
			}
		}
		return nullptr;
	}
}

void FlowVizStudioRig::Describe(
	const FVector& UnrealDomainSize, TArray<FLightDescription>& OutLights)
{
	OutLights.Reset();

	/*
	 * THE THREE-POINT GRAMMAR, in the domain's frame (+X downstream, -Y is
	 * the mirrored +Y, +Z up):
	 *
	 * KEY: from high on the -Y side, angled ACROSS the flow axis -- grazing
	 * light is what makes tube curvature read (research doc 7.1). Brightest,
	 * neutral-warm.
	 * FILL: from the +Y side, cooler and much dimmer -- lifts the shadows the
	 * key throws without flattening them.
	 * RIM: from behind the wake (downstream, +X) and above, warm -- the
	 * silhouette glow on tube edges seen from the standard 3/4 upstream view.
	 *
	 * Directions are normalized "pointing" vectors; directional lights ignore
	 * position, so the domain size only matters if this rig ever gains area
	 * lights -- the parameter stays so the design can scale then.
	 */
	FLightDescription& Key = OutLights.AddDefaulted_GetRef();
	Key.Name = FlowVizStudioRigLocal::KeyName;
	Key.Direction = FVector(0.35, 0.75, -0.56).GetSafeNormal();
	Key.Color = FLinearColor(1.0f, 0.96f, 0.90f);
	Key.IntensityLux = 10.0f;

	FLightDescription& Fill = OutLights.AddDefaulted_GetRef();
	Fill.Name = FlowVizStudioRigLocal::FillName;
	Fill.Direction = FVector(0.2, -0.8, -0.4).GetSafeNormal();
	Fill.Color = FLinearColor(0.75f, 0.85f, 1.0f);
	Fill.IntensityLux = 3.0f;

	FLightDescription& Rim = OutLights.AddDefaulted_GetRef();
	Rim.Name = FlowVizStudioRigLocal::RimName;
	Rim.Direction = FVector(-0.85, 0.1, -0.35).GetSafeNormal();
	Rim.Color = FLinearColor(1.0f, 0.85f, 0.70f);
	Rim.IntensityLux = 6.0f;
}

void FlowVizStudioRig::Apply(ACFDVizCaseActor& Actor)
{
	TArray<FLightDescription> Lights;
	Describe(FVector(1200.0, 400.0, 100.0), Lights);

	for (const FLightDescription& Description : Lights)
	{
		UDirectionalLightComponent* Light =
			FlowVizStudioRigLocal::FindRigLight(Actor, Description.Name);
		if (Light == nullptr)
		{
			Light = NewObject<UDirectionalLightComponent>(&Actor, Description.Name);
			Light->SetupAttachment(Actor.GetRootComponent());
			Light->RegisterComponent();
		}
		Light->SetWorldRotation(Description.Direction.Rotation());
		Light->SetLightColor(Description.Color);
		Light->SetIntensity(Description.IntensityLux);
		// The KEY carries the shadows; three shadowing directionals triple the
		// virtual-shadow-map cost for shadows nobody reads.
		Light->SetCastShadows(Description.Name == FlowVizStudioRigLocal::KeyName);
		// Atmosphere interaction off: the rig lights the DATA, not the sky.
		Light->SetAtmosphereSunLight(false);
	}
}

void FlowVizStudioRig::Remove(ACFDVizCaseActor& Actor)
{
	for (const FName Name : { FlowVizStudioRigLocal::KeyName,
		FlowVizStudioRigLocal::FillName, FlowVizStudioRigLocal::RimName })
	{
		if (UDirectionalLightComponent* Light =
				FlowVizStudioRigLocal::FindRigLight(Actor, Name))
		{
			Light->DestroyComponent();
		}
	}
}

bool FlowVizStudioRig::IsApplied(const ACFDVizCaseActor& Actor)
{
	return FlowVizStudioRigLocal::FindRigLight(Actor, FlowVizStudioRigLocal::KeyName)
		!= nullptr;
}
