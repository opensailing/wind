// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "Scene/FlowVizMeshPayload.h"

#include "FlowVizSurfaceMeshComponent.generated.h"

/**
 * The surface path's applier (renderer overhaul P2; architecture brief
 * section 8, "Surface path").
 *
 * A THIN WRAPPER over UProceduralMeshComponent, deliberately: PMC meshes
 * are real scene geometry -- Lumen-lit, shadow-casting, z-writing, path-
 * traceable -- which is the entire argument for the surface path over the
 * proxy-drawn hull it replaces. This class only maps payloads to sections
 * and keeps section identity (patch ids, names, visibility defaults) that
 * raw PMC section indices lose.
 *
 * THREE INSTANCES, NOT ONE. ACFDVizCaseActor owns one of these each for the
 * obstacle, the cut plane, and the iso surface, because CreateMeshSection
 * dirties the whole component's proxy: a per-frame iso rebuild on the same
 * component as the static obstacle would recreate the obstacle's render
 * state sixty times a second for nothing.
 *
 * PAYLOADS ARRIVE BUILT. Workers build FFlowVizMeshPayload (pure builders,
 * tested headless); this class applies on the game thread. SetSurfaceData
 * is explicit, never per-tick -- the same contract as UCFDVizFlowComponent.
 *
 * TICKING. UProceduralMeshComponent already defaults bCanEverTick to false, and
 * this subclass does not override that state. Setting the same flag again here
 * would not change behavior; the regression test pins the inherited default so
 * an engine change cannot silently turn three surface components into tickers.
 */
UCLASS(ClassGroup = (FlowViz), meta = (BlueprintSpawnableComponent))
class FLOWVIZRUNTIME_API UCFDVizSurfaceMeshComponent : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	UCFDVizSurfaceMeshComponent(const FObjectInitializer& ObjectInitializer);

	/** Replace every section with the payload's. defaultVisible is honoured per section. */
	void SetSurfaceData(const FFlowVizMeshPayload& Payload);

	/** Remove every section and forget its identity. */
	void ClearSurfaceData();

	int32 GetSectionCount() const { return SectionInfos.Num(); }

	/** The payload's SectionId for a section index, or MAX_uint32 out of range. */
	uint32 GetSectionId(int32 SectionIndex) const;
	FString GetSectionName(int32 SectionIndex) const;

	bool IsSectionVisible(int32 SectionIndex) const;
	void SetSectionVisible(int32 SectionIndex, bool bVisible);

private:
	struct FSectionInfo
	{
		uint32 SectionId = 0;
		FString Name;
		bool bVisible = true;
	};
	TArray<FSectionInfo> SectionInfos;
};
