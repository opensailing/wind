// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CFDViz/CFDVizColorMaps.h"
#include "CoreMinimal.h"

/**
 * Screenshot annotation (#83 / Milestone F, DoD 15): the case name, field,
 * physical time, value range and a colour-bar legend, burned into the
 * captured pixels.
 *
 * PIXELS, NOT WIDGETS. The capture path reads a render target on a headless
 * RHI; there is no Slate surface over it to composite and no UMG to
 * screenshot. Burning into the pixel array is also what makes this testable:
 * the annotated image IS the output, and a test reads the same pixels a
 * journal reviewer would.
 *
 * THE FONT IS A 5x7 BITMAP, EMBEDDED. No engine font asset loads without a
 * renderer and no asset dependency survives packaging trimming; 96 glyphs of
 * 35 bits each is smaller than the code to load anything. Uppercase-folded:
 * a figure caption's annotation is labelling, not typography.
 *
 * THE LEGEND SAMPLES THE REAL COLORMAP TABLE -- CFDViz::ColorMaps::Sample,
 * the same table the LUT upload uses -- so the strip in the PNG and the
 * colours in the volume cannot come from different palettes.
 */
struct FFlowVizCaptureAnnotation
{
	FString CaseName;
	FString FieldName;

	/** Physical time and its unit, e.g. 0.24 and "s". */
	double Time = 0.0;
	FString TimeUnit = TEXT("s");

	/** The transfer function's domain, and the colormap the legend samples. */
	float RangeMin = 0.0f;
	float RangeMax = 1.0f;
	ECFDVizColorMap ColorMap = ECFDVizColorMap::Viridis;
	bool bReversed = false;
};

namespace FlowVizAnnotate
{
	/** Height of the burned footer strip, pixels. Exposed so tests can bound the affected region. */
	inline constexpr int32 FooterHeight = 48;

	/**
	 * Burn the annotation footer into the bottom of an image.
	 *
	 * The footer is an opaque dark strip: caption text on the left, the colour
	 * bar with its min/max labels on the right. Everything ABOVE the strip is
	 * untouched -- the annotation must never paint over data pixels.
	 *
	 * @param Pixels Row-major, Width * Height. Modified in place.
	 * @return false when the image is too small for the footer (< 2x its
	 *         height, or too narrow for the caption); nothing is modified then.
	 */
	FLOWVIZRUNTIME_API bool BurnFooter(
		TArray<FColor>& Pixels,
		int32 Width,
		int32 Height,
		const FFlowVizCaptureAnnotation& Annotation);

	/** Render one line of 5x7 text into the pixel array. Exposed for the font's own test. */
	FLOWVIZRUNTIME_API void DrawText(
		TArray<FColor>& Pixels,
		int32 Width,
		int32 Height,
		int32 X,
		int32 Y,
		const FString& Text,
		const FColor& Color,
		int32 Scale = 1);
}
