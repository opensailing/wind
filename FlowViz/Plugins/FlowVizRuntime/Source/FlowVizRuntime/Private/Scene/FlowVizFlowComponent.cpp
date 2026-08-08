// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizFlowComponent.h"

#include "CFDViz/CFDVizColorMaps.h"
#include "CFDViz/CFDVizTypes.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LineBatchComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

UCFDVizFlowComponent::UCFDVizFlowComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	GlyphMesh = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Glyphs"));
	GlyphMesh->SetupAttachment(this);
	GlyphMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GlyphMesh->SetCastShadow(false);

	/*
	 * THE ENGINE CONE, not a content-side arrow asset. An asset would need
	 * cooking and could not be constructed headless; the basic-shape cone
	 * ships with the engine, points along +Z at unit scale, and reads as a
	 * direction glyph at a distance. MakeGlyphTransforms bakes the +Z-to-+X
	 * reorientation so the transform maths stays "arrow along +X" throughout.
	 */
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(
		TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (ConeFinder.Succeeded())
	{
		GlyphMesh->SetStaticMesh(ConeFinder.Object);
	}

	LineBatch = CreateDefaultSubobject<ULineBatchComponent>(TEXT("Streamlines"));
	LineBatch->SetupAttachment(this);

	// Tracer sprites (P8's CPU path): the engine sphere at a small uniform
	// scale. Same headless-constructible reasoning as the cone.
	ParticleMesh = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Particles"));
	ParticleMesh->SetupAttachment(this);
	ParticleMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ParticleMesh->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereFinder.Succeeded())
	{
		ParticleMesh->SetStaticMesh(SphereFinder.Object);
	}
}

void UCFDVizFlowComponent::SetFlowData(
	const TArray<FFlowVizGlyph>& Glyphs,
	const TArray<FFlowVizStreamline>& Streamlines,
	double MetersToUnrealUnits)
{
	ClearFlowData();

	/* --- Glyphs ------------------------------------------------------------ */

	TArray<FTransform> Transforms;
	double MaxMagnitude = 0.0;
	MakeGlyphTransforms(Glyphs, MetersToUnrealUnits, Transforms, MaxMagnitude);
	if (Transforms.Num() > 0)
	{
		GlyphMesh->AddInstances(Transforms, /*bShouldReturnIndices*/ false,
			/*bWorldSpace*/ false);
	}

	/* --- Streamlines ------------------------------------------------------- */

	TArray<FStreamlineBatch> Batches;
	MakeStreamlineBatches(Streamlines, MetersToUnrealUnits, Batches);
	for (const FStreamlineBatch& Batch : Batches)
	{
		for (int32 Index = 0; Index + 1 < Batch.Points.Num(); ++Index)
		{
			// Lifetime 0 with bPersistent: drawn until the next ClearFlowData,
			// which is exactly the rebuild cadence.
			LineBatch->DrawLine(Batch.Points[Index], Batch.Points[Index + 1],
				Batch.Colors[Index], SDPG_World, /*Thickness*/ 1.5f, /*LifeTime*/ 0.0f);
		}
	}
}

void UCFDVizFlowComponent::ClearFlowData()
{
	GlyphMesh->ClearInstances();
	LineBatch->Flush();
	ParticleMesh->ClearInstances();
}

void UCFDVizFlowComponent::SetParticlePositions(
	TArrayView<const FVector> SolverPositions, double MetersToUnrealUnits)
{
	ParticleMesh->ClearInstances();
	if (SolverPositions.Num() == 0)
	{
		return;
	}
	TArray<FTransform> Transforms;
	Transforms.Reserve(SolverPositions.Num());
	// 3 uu radius: visible at domain scale, small enough to read as a tracer.
	const FVector Scale(0.06);
	for (const FVector& Solver : SolverPositions)
	{
		Transforms.Add(FTransform(FQuat::Identity,
			SolverToUnrealPosition(Solver, MetersToUnrealUnits), Scale));
	}
	ParticleMesh->AddInstances(Transforms, /*bShouldReturnIndices*/ false,
		/*bWorldSpace*/ false);
}

int32 UCFDVizFlowComponent::GetParticleInstanceCount() const
{
	return ParticleMesh->GetInstanceCount();
}

int32 UCFDVizFlowComponent::GetGlyphInstanceCount() const
{
	return GlyphMesh->GetInstanceCount();
}

void UCFDVizFlowComponent::MakeGlyphTransforms(
	const TArray<FFlowVizGlyph>& Glyphs,
	double MetersToUnrealUnits,
	TArray<FTransform>& OutTransforms,
	double& OutMaxMagnitude)
{
	OutTransforms.Reset();
	OutMaxMagnitude = 0.0;

	for (const FFlowVizGlyph& Glyph : Glyphs)
	{
		OutMaxMagnitude = FMath::Max(OutMaxMagnitude, Glyph.Magnitude);
	}
	if (OutMaxMagnitude <= 0.0)
	{
		return;
	}

	OutTransforms.Reserve(Glyphs.Num());
	for (const FFlowVizGlyph& Glyph : Glyphs)
	{
		/*
		 * DIRECTION through the direction adapter, POSITION through the
		 * position adapter -- they mirror differently (the Y flip scales
		 * positions but must only flip directions), and using one for both is
		 * exactly the vortex-spin bug CFDVizTypes' header warns about.
		 */
		const FVector UnrealPosition =
			SolverToUnrealPosition(Glyph.Position, MetersToUnrealUnits);
		const FVector UnrealDirection =
			FVector(SolverToUnrealDirection(FVector3f(Glyph.Direction)));

		const double Length =
			MaxGlyphLength * (Glyph.Magnitude / OutMaxMagnitude);

		/*
		 * The engine cone points +Z at unit scale (100 uu tall). The rotation
		 * takes +Z onto the flow direction; the scale takes 100 uu onto
		 * Length, with the radial axes thinned to a fixed fraction so a long
		 * arrow is a longer arrow, not a fatter one.
		 */
		const FQuat Rotation = FQuat::FindBetweenNormals(FVector::ZAxisVector, UnrealDirection);
		const FVector Scale(
			Length / 100.0 * 0.35, Length / 100.0 * 0.35, Length / 100.0);

		OutTransforms.Add(FTransform(Rotation, UnrealPosition, Scale));
	}
}

void UCFDVizFlowComponent::MakeStreamlineBatches(
	const TArray<FFlowVizStreamline>& Streamlines,
	double MetersToUnrealUnits,
	TArray<FStreamlineBatch>& OutBatches)
{
	OutBatches.Reset();

	double MaxMagnitude = 0.0;
	for (const FFlowVizStreamline& Line : Streamlines)
	{
		for (const double Magnitude : Line.Magnitudes)
		{
			MaxMagnitude = FMath::Max(MaxMagnitude, Magnitude);
		}
	}
	if (MaxMagnitude <= 0.0)
	{
		return;
	}

	for (const FFlowVizStreamline& Line : Streamlines)
	{
		if (Line.Points.Num() < 2)
		{
			// An empty line (a seed outside the domain) stays empty -- rake
			// order is index-stable, and there is nothing to draw.
			continue;
		}

		FStreamlineBatch& Batch = OutBatches.AddDefaulted_GetRef();
		Batch.Points.Reserve(Line.Points.Num());
		Batch.Colors.Reserve(Line.Points.Num());
		for (int32 Index = 0; Index < Line.Points.Num(); ++Index)
		{
			Batch.Points.Add(
				SolverToUnrealPosition(Line.Points[Index], MetersToUnrealUnits));
			// The default colormap over the batch's own maximum: the same
			// relative normalisation the glyphs use, so "bright" means the
			// same thing across both.
			const float Position =
				static_cast<float>(Line.Magnitudes[Index] / MaxMagnitude);
			Batch.Colors.Add(CFDViz::ColorMaps::Sample(CFDViz::ColorMaps::Default, Position));
		}
	}
}
