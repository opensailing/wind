// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDVizColorMaps.generated.h"

/**
 * Colormaps shared with the Python tools.
 *
 * The control points here are generated from
 * `Tools/cfdviz/src/cfdviz/colormaps.py`, which is the single source of truth.
 * A Python-generated reference figure and an Unreal render of the same field
 * must produce the same colors, so neither side re-types the tables.
 *
 * Sampling convention (must match the Python side exactly):
 *
 * - Linear interpolation between control points **in sRGB space**. Not
 *   perceptually ideal, but it is what matplotlib, ParaView, and GPU texture
 *   filtering all do - so it is what makes the three agree. A "better"
 *   interpolation here would make our figures disagree with every reference
 *   tool.
 * - LUT sampling uses **pixel centers**: entry `i` of an `N`-entry LUT samples
 *   `t = (i + 0.5) / N`. Using `i / (N - 1)` shifts the map by half a texel,
 *   which shows up as a subtle but real hue shift against reference figures.
 */

/** A single colormap control point. Channels are sRGB in [0,1]. */
struct FCFDVizColorStop
{
	float Position;
	float R;
	float G;
	float B;
};

UENUM(BlueprintType)
enum class ECFDVizColorMap : uint8
{
	/** Default. Perceptually uniform, colorblind-safe, legible in greyscale. */
	Viridis		UMETA(DisplayName = "Viridis"),
	Plasma		UMETA(DisplayName = "Plasma"),
	Inferno		UMETA(DisplayName = "Inferno"),
	Magma		UMETA(DisplayName = "Magma"),
	/** Rainbow replacement. Offered for legacy comparison; never the default. */
	Turbo		UMETA(DisplayName = "Turbo"),
	/** Diverging. Center on the meaningful zero, not the data midpoint. */
	CoolWarm	UMETA(DisplayName = "Cool to Warm"),
	/** Diverging. */
	BlueWhiteRed UMETA(DisplayName = "Blue-White-Red"),
	Grayscale	UMETA(DisplayName = "Grayscale"),

	Count		UMETA(Hidden)
};

namespace CFDViz::ColorMaps
{
	/**
	 * The default for every scalar field.
	 *
	 * Never default to a rainbow map: rainbows create visual edges where the
	 * data is smooth and flatten real structure where it is steep, which is
	 * actively misleading in a scientific figure.
	 */
	inline constexpr ECFDVizColorMap Default = ECFDVizColorMap::Viridis;

	/** Control points for a map, ascending in Position over [0,1]. */
	FLOWVIZRUNTIME_API TArrayView<const FCFDVizColorStop> GetStops(ECFDVizColorMap Map);

	/** Stable lowercase name, matching the Python keys exactly (e.g. "blue-white-red"). */
	FLOWVIZRUNTIME_API FName GetName(ECFDVizColorMap Map);

	/** Parse a Python-side name back to the enum. Returns false if unknown. */
	FLOWVIZRUNTIME_API bool TryParse(FName Name, ECFDVizColorMap& OutMap);

	/**
	 * Whether this map can be used without visually fabricating structure.
	 *
	 * The UI must mark anything outside this set, so a user who selects Turbo
	 * knows what they selected.
	 */
	FLOWVIZRUNTIME_API bool IsPerceptuallyUniform(ECFDVizColorMap Map);

	/**
	 * Whether this map is intended for signed data about a meaningful zero.
	 *
	 * Diverging maps should default to a range centered on zero rather than on
	 * the data midpoint, or the neutral color lands somewhere meaningless.
	 */
	FLOWVIZRUNTIME_API bool IsDiverging(ECFDVizColorMap Map);

	/** Sample a map at Position, clamped to [0,1]. Returns linear sRGB in [0,1]. */
	FLOWVIZRUNTIME_API FLinearColor Sample(ECFDVizColorMap Map, float Position);

	/**
	 * Fill a lookup table for upload as a 1D texture.
	 *
	 * @param Map       Which colormap.
	 * @param OutLut    Receives Size entries. Alpha is set to 1; opacity is a
	 *                  separate transfer function and must not be baked in here.
	 * @param Size      Entry count. 256 matches an 8-bit texture exactly.
	 * @param bReverse  Flip the map end-for-end.
	 * @param Bands     If > 0, quantize into this many discrete bands. Banding
	 *                  is a legitimate scientific display choice, so it happens
	 *                  here rather than as a posterize post-process that would
	 *                  also corrupt the UI and legend.
	 */
	FLOWVIZRUNTIME_API void BuildLut(
		ECFDVizColorMap Map,
		TArray<FLinearColor>& OutLut,
		int32 Size = 256,
		bool bReverse = false,
		int32 Bands = 0);
}
