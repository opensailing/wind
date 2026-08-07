// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "Flow/FlowVizFlowInspection.h"

#include "FlowVizFlowComponent.generated.h"

class UInstancedStaticMeshComponent;
class ULineBatchComponent;

/**
 * Glyphs and streamlines in the viewport (#86 / DoD 9 and 10's last hop).
 *
 * THE MATHS IS ELSEWHERE. BuildSliceGlyphs and BuildStreamlines are tested
 * against the analytic wake (FlowViz.Flow.*); this component's own logic is
 * the solver-to-Unreal transform of their output and the instancing, which
 * is why MakeGlyphTransforms and MakeStreamlineBatches are static and
 * headless-testable while the component itself is a thin applier.
 *
 * REBUILD IS EXPLICIT (SetFlowData), not per-tick: the geometry changes when
 * the displayed frame or the slice moves, and the workspace already owns
 * both signals. Building on a worker is the CALLER's job -- the samplers do
 * disk I/O; this component only receives finished arrays on the game thread.
 *
 * GLYPH SCALE. An arrow's length maps |v| linearly onto
 * [0, MaxGlyphLength * scale] against the batch's own maximum -- the same
 * relative-to-this-slice normalisation the glyph builder's magnitude floor
 * uses, so the longest arrow on ANY slice is the same size on screen and
 * the ratio between arrows is the data.
 */
UCLASS(ClassGroup = (FlowViz), meta = (BlueprintSpawnableComponent))
class FLOWVIZRUNTIME_API UCFDVizFlowComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UCFDVizFlowComponent();

	/**
	 * Replace the displayed glyphs and streamlines.
	 *
	 * @param Glyphs      SOLVER-space glyphs from BuildSliceGlyphs.
	 * @param Streamlines SOLVER-space lines from BuildStreamlines.
	 * @param MetersToUnrealUnits The case's length scale.
	 */
	void SetFlowData(
		const TArray<FFlowVizGlyph>& Glyphs,
		const TArray<FFlowVizStreamline>& Streamlines,
		double MetersToUnrealUnits);

	/** Drop everything drawn. */
	void ClearFlowData();

	/** Instances currently drawn -- the test seam for "the glyphs reached the scene". */
	int32 GetGlyphInstanceCount() const;

	/**
	 * One transform per glyph, in Unreal space: scaled unit-arrow along +X,
	 * rotated onto the (mirrored) direction, at the (scaled) position.
	 *
	 * @param OutMaxMagnitude The batch's own maximum |v|, the length scale's
	 *        denominator. Zero when Glyphs is empty.
	 */
	static void MakeGlyphTransforms(
		const TArray<FFlowVizGlyph>& Glyphs,
		double MetersToUnrealUnits,
		TArray<FTransform>& OutTransforms,
		double& OutMaxMagnitude);

	/**
	 * Streamline polylines in Unreal space, with a colour per segment from the
	 * local |v| against the batch maximum, sampled from the WORKSPACE's default
	 * colormap (viridis) -- the chart and the lines agree on what fast means.
	 */
	struct FStreamlineBatch
	{
		TArray<FVector> Points;
		TArray<FLinearColor> Colors;
	};
	static void MakeStreamlineBatches(
		const TArray<FFlowVizStreamline>& Streamlines,
		double MetersToUnrealUnits,
		TArray<FStreamlineBatch>& OutBatches);

	/** Unreal-units length of the longest glyph at scale 1. */
	static constexpr double MaxGlyphLength = 40.0;

private:
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> GlyphMesh;

	UPROPERTY()
	TObjectPtr<ULineBatchComponent> LineBatch;
};
