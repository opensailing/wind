// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CFDViz/CFDVizColorMaps.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CFDViz/CFDVizTypes.h"

#include "FlowVizCaseActor.generated.h"

class UCFDVizVolumeComponent;
struct FFlowVizMeshPayload;

/**
 * One CFDViz case in a level (plan.md section 5.E).
 *
 * The actor is deliberately thin. It exists to give a case a transform, a
 * selectable thing in the outliner, and a place for the representations
 * (volume now; slice, boundary mesh, glyphs, streamlines and probes later) to
 * hang off one shared root. Every decision that can be wrong silently -
 * placement, extent, winding - lives in UCFDVizVolumeComponent, where it is
 * tested. Putting any of it here would give it a second implementation.
 *
 * THE ROOT IS A PLAIN SCENE COMPONENT, NOT THE VOLUME. Moving the actor must
 * move every representation together, and a volume-rooted actor would make the
 * volume's own placement matrix part of the actor transform - so a future slice
 * component would inherit the grid-origin translation and the Y mirror twice.
 * ADR 004 section 2 is explicit that a second conversion site is how a value
 * gets converted twice, and a double mirror is the identity and therefore
 * invisible.
 *
 * THREADING. Public mutation is game-thread only. Boundary files are decoded on
 * workers from copied requests; their payloads return through a generation gate
 * on the game thread, so an older case can never overwrite a newer one.
 */
UCLASS(ClassGroup = (FlowViz), meta = (DisplayName = "CFDViz Case"))
class FLOWVIZRUNTIME_API ACFDVizCaseActor : public AActor
{
	GENERATED_BODY()

public:
	ACFDVizCaseActor();

	/**
	 * Load a `.cfdviz` directory and display one of its volume fields.
	 *
	 * Forwards to UCFDVizVolumeComponent::LoadCase, which keeps the previous
	 * binding on failure - so a bad path leaves whatever was already on screen
	 * rather than blanking it.
	 *
	 * @param CaseDirectory Path to the `.cfdviz` directory or its manifest.json.
	 * @param FieldId       Field to display; NAME_None picks the first non-mask field.
	 */
	FCFDVizResult LoadCase(const FString& CaseDirectory, FName FieldId = NAME_None);

	/**
	 * Blueprint-facing form. Returns false and writes the reason to
	 * OutError - the same string FCFDVizResult::ToString produces, which names
	 * the file and byte offset where one applies.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowViz",
		meta = (DisplayName = "Load CFDViz Case"))
	bool LoadCaseFromPath(const FString& CaseDirectory, FString& OutError);

	/** The volume representation. Never null. */
	class UCFDVizFlowComponent* GetFlowComponent() const
	{
		return FlowComponent;
	}

	/** The obstacle's opaque lit surface (renderer overhaul P2). */
	class UCFDVizSurfaceMeshComponent* GetObstacleComponent() const
	{
		return ObstacleComponent;
	}

	/** The cut plane's surface (renderer overhaul P3). Its own component: per-frame rebuilds never dirty the obstacle. */
	class UCFDVizSurfaceMeshComponent* GetCutPlaneComponent() const
	{
		return CutPlaneComponent;
	}

	/** The Q iso-surface (renderer overhaul P4). Also its own component, for the same isolation. */
	class UCFDVizSurfaceMeshComponent* GetIsoSurfaceComponent() const
	{
		return IsoSurfaceComponent;
	}

	UCFDVizVolumeComponent* GetVolumeComponent() const
	{
		return VolumeComponent;
	}

	/**
	 * The case directory to load when the actor begins play.
	 *
	 * Loaded on BeginPlay rather than in the constructor: the constructor runs on
	 * the class default object during module load, where file I/O would happen
	 * once per editor start against a path that may not exist yet.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "FlowViz")
	FString CaseDirectory;

	/** Field to display. Leave as None for the first non-mask field. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "FlowViz")
	FName FieldId;

	virtual void BeginPlay() override;

	UPROPERTY()
	TObjectPtr<class UCFDVizFlowComponent> FlowComponent;

	UPROPERTY()
	TObjectPtr<class UCFDVizSurfaceMeshComponent> ObstacleComponent;

	UPROPERTY()
	TObjectPtr<class UCFDVizSurfaceMeshComponent> CutPlaneComponent;

	UPROPERTY()
	TObjectPtr<class UCFDVizSurfaceMeshComponent> IsoSurfaceComponent;

	/** Build every declared mesh's patches on a worker and apply on the game thread. */
	void LoadBoundaryMeshes();

public:
	/**
	 * Swap the surface materials to the profile's set (renderer overhaul P7):
	 * Presentation = PBR obstacle + Fresnel colormap; Scientific = the flat
	 * pair. Materials only -- geometry, sections and visibility untouched.
	 */
	void ApplyProfileMaterials(bool bPresentation);

	/**
	 * Record the colormap used by newly built surface payloads and rebind the
	 * active profile's plain vertex-colour material. Surface colours are authored
	 * into FFlowVizMeshSection::Colors before upload; no MID or LUT texture is
	 * created on this path.
	 */
	void SetSurfaceColorMap(ECFDVizColorMap Map);

private:
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SurfaceMaterialScientific;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SurfaceMaterialPresentation;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> ColormapMaterialScientific;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> ColormapMaterialPresentation;

	ECFDVizColorMap CurrentColorMap = ECFDVizColorMap::Viridis;
	bool bCurrentProfilePresentation = false;

	/** Monotonic game-thread token for boundary build supersession. */
	uint64 BoundaryBuildGeneration = 0;

	/** Apply only when the worker still describes the actor's current case. */
	void ApplyBoundaryPayload(uint64 BuildGeneration, const FFlowVizMeshPayload& Payload);

	friend class FFlowVizBoundaryBuildSupersessionTest;

private:
	/** The volume representation, attached to the root rather than being it - see the class comment. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "FlowViz",
		meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCFDVizVolumeComponent> VolumeComponent;
};
