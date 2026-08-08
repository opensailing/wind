// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizSurfaceMeshComponent.h"

UCFDVizSurfaceMeshComponent::UCFDVizSurfaceMeshComponent(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Real scene geometry: z-writing, shadow-casting -- the compositing rules
	// (research doc section 5 item 5) in component-default form. The volume is
	// the only translucent thing in the picture, and it is not this.
	SetCastShadow(true);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	bUseAsyncCooking = false;
}

void UCFDVizSurfaceMeshComponent::SetSurfaceData(const FFlowVizMeshPayload& Payload)
{
	ClearSurfaceData();

	SectionInfos.Reserve(Payload.Sections.Num());
	for (int32 SectionIndex = 0; SectionIndex < Payload.Sections.Num(); ++SectionIndex)
	{
		const FFlowVizMeshSection& Section = Payload.Sections[SectionIndex];

		/*
		 * THE SCALAR RIDES UV0.x, at float precision. Vertex COLOR is 8-bit
		 * -- baking a colormap coordinate there posterizes 256 values into
		 * visible bands on smooth fields (VISUAL_QA rule 11's shape). The
		 * material samples the LUT texture with this coordinate instead, so
		 * a colormap or range change never touches geometry.
		 */
		TArray<FVector2D> UV0;
		if (Section.ScalarUVs.Num() == Section.Vertices.Num())
		{
			UV0.Reserve(Section.ScalarUVs.Num());
			for (const float Scalar : Section.ScalarUVs)
			{
				UV0.Add(FVector2D(Scalar, 0.5));
			}
		}

		CreateMeshSection(
			SectionIndex, Section.Vertices, Section.Indices, Section.Normals, UV0,
			Section.Colors, /*Tangents*/ TArray<FProcMeshTangent>(),
			/*bCreateCollision*/ false);

		// The manifest's stated intent, applied rather than reported: an inlet
		// with defaultVisible=false arrives hidden instead of arriving as a
		// wall the user must diagnose.
		SetMeshSectionVisible(SectionIndex, Section.bDefaultVisible);

		FSectionInfo& Info = SectionInfos.AddDefaulted_GetRef();
		Info.SectionId = Section.SectionId;
		Info.Name = Section.Name;
		Info.bVisible = Section.bDefaultVisible;
	}
}

void UCFDVizSurfaceMeshComponent::ClearSurfaceData()
{
	ClearAllMeshSections();
	SectionInfos.Reset();
}

uint32 UCFDVizSurfaceMeshComponent::GetSectionId(int32 SectionIndex) const
{
	return SectionInfos.IsValidIndex(SectionIndex)
		? SectionInfos[SectionIndex].SectionId
		: MAX_uint32;
}

FString UCFDVizSurfaceMeshComponent::GetSectionName(int32 SectionIndex) const
{
	return SectionInfos.IsValidIndex(SectionIndex)
		? SectionInfos[SectionIndex].Name
		: FString();
}

bool UCFDVizSurfaceMeshComponent::IsSectionVisible(int32 SectionIndex) const
{
	return SectionInfos.IsValidIndex(SectionIndex) && SectionInfos[SectionIndex].bVisible;
}

void UCFDVizSurfaceMeshComponent::SetSectionVisible(int32 SectionIndex, bool bVisible)
{
	if (!SectionInfos.IsValidIndex(SectionIndex))
	{
		return;
	}
	SectionInfos[SectionIndex].bVisible = bVisible;
	SetMeshSectionVisible(SectionIndex, bVisible);
}
