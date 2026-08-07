// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CFDViz/CFDVizColorMaps.h"
#include "CoreMinimal.h"

class UTexture2D;

/**
 * The colormap LUT as a texture (renderer overhaul P3; architecture brief's
 * "one color authority").
 *
 * CFDViz::ColorMaps REMAINS THE AUTHORITY: this is a derived artifact --
 * 256x1 BGRA8, sRGB off (the LUT is linear color), sampled by the surface
 * materials with the scalar the payloads bake into UV0.x. The Slate legend
 * and the capture burn-in draw from the same CPU LUT, so viewport, legend
 * and export cannot disagree; a byte-level parity test pins THIS bridge, the
 * only one that crosses to the GPU.
 */
namespace FlowVizColorMapTexture
{
	constexpr int32 LutWidth = 256;

	/** The authority's samples as BGRA8 bytes, exactly as the texture stores them. */
	FLOWVIZRUNTIME_API void BuildLutBytes(ECFDVizColorMap Map, TArray<FColor>& OutBytes);

	/** A transient 256x1 texture of BuildLutBytes. Caller owns rooting it. */
	FLOWVIZRUNTIME_API UTexture2D* CreateLutTexture(ECFDVizColorMap Map);
}
