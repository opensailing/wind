// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizColorMaps.h"

namespace CFDViz::ColorMaps
{
	namespace
	{
		// GENERATED from Tools/cfdviz/src/cfdviz/colormaps.py - do not hand-edit.
		// Regenerate both sides together; a divergence here makes an Unreal render
		// disagree with a Python reference figure of the same data.

		// viridis  (perceptually uniform)
		static const FCFDVizColorStop ViridisStops[] = {
			{ 0.000f, 0.267004f, 0.004874f, 0.329415f },
			{ 0.125f, 0.282623f, 0.140926f, 0.457517f },
			{ 0.250f, 0.253935f, 0.265254f, 0.529983f },
			{ 0.375f, 0.206756f, 0.371758f, 0.553117f },
			{ 0.500f, 0.163625f, 0.471133f, 0.558148f },
			{ 0.625f, 0.127568f, 0.566949f, 0.550556f },
			{ 0.750f, 0.134692f, 0.658636f, 0.517649f },
			{ 0.875f, 0.266941f, 0.748751f, 0.440573f },
			{ 1.000f, 0.993248f, 0.906157f, 0.143936f },
		};

		// plasma  (perceptually uniform)
		static const FCFDVizColorStop PlasmaStops[] = {
			{ 0.000f, 0.050383f, 0.029803f, 0.527975f },
			{ 0.125f, 0.253935f, 0.014979f, 0.617331f },
			{ 0.250f, 0.417642f, 0.000564f, 0.658390f },
			{ 0.375f, 0.562738f, 0.051545f, 0.641509f },
			{ 0.500f, 0.692840f, 0.165141f, 0.564522f },
			{ 0.625f, 0.798216f, 0.280197f, 0.469538f },
			{ 0.750f, 0.881443f, 0.392529f, 0.383229f },
			{ 0.875f, 0.949217f, 0.517763f, 0.295662f },
			{ 1.000f, 0.940015f, 0.975158f, 0.131326f },
		};

		// inferno  (perceptually uniform)
		static const FCFDVizColorStop InfernoStops[] = {
			{ 0.000f, 0.001462f, 0.000466f, 0.013866f },
			{ 0.125f, 0.087411f, 0.044556f, 0.224813f },
			{ 0.250f, 0.229739f, 0.036590f, 0.360847f },
			{ 0.375f, 0.374915f, 0.081348f, 0.412415f },
			{ 0.500f, 0.517933f, 0.132268f, 0.408558f },
			{ 0.625f, 0.665859f, 0.198401f, 0.370587f },
			{ 0.750f, 0.798216f, 0.280197f, 0.469538f },
			{ 0.875f, 0.930513f, 0.411474f, 0.145367f },
			{ 1.000f, 0.988362f, 0.998364f, 0.644924f },
		};

		// magma  (perceptually uniform)
		static const FCFDVizColorStop MagmaStops[] = {
			{ 0.000f, 0.001462f, 0.000466f, 0.013866f },
			{ 0.125f, 0.078815f, 0.054184f, 0.211667f },
			{ 0.250f, 0.207677f, 0.063327f, 0.379497f },
			{ 0.375f, 0.345164f, 0.106815f, 0.453077f },
			{ 0.500f, 0.482930f, 0.146968f, 0.472039f },
			{ 0.625f, 0.629101f, 0.192902f, 0.457755f },
			{ 0.750f, 0.775059f, 0.257322f, 0.406990f },
			{ 0.875f, 0.912966f, 0.381636f, 0.359630f },
			{ 1.000f, 0.987053f, 0.991438f, 0.749504f },
		};

		// turbo
		static const FCFDVizColorStop TurboStops[] = {
			{ 0.000f, 0.189950f, 0.071760f, 0.232170f },
			{ 0.125f, 0.251070f, 0.252370f, 0.633740f },
			{ 0.250f, 0.276280f, 0.421180f, 0.891230f },
			{ 0.375f, 0.258620f, 0.579580f, 0.998760f },
			{ 0.500f, 0.158440f, 0.735510f, 0.923050f },
			{ 0.625f, 0.092670f, 0.865540f, 0.710000f },
			{ 0.750f, 0.196590f, 0.949010f, 0.476050f },
			{ 0.875f, 0.527950f, 0.987270f, 0.235730f },
			{ 1.000f, 0.479600f, 0.015830f, 0.010550f },
		};

		// coolwarm  (diverging)
		static const FCFDVizColorStop CoolWarmStops[] = {
			{ 0.000f, 0.229806f, 0.298718f, 0.753683f },
			{ 0.125f, 0.365375f, 0.450915f, 0.859997f },
			{ 0.250f, 0.508937f, 0.588019f, 0.936277f },
			{ 0.375f, 0.653372f, 0.700669f, 0.977678f },
			{ 0.500f, 0.865003f, 0.865003f, 0.865003f },
			{ 0.625f, 0.938156f, 0.680509f, 0.615520f },
			{ 0.750f, 0.930204f, 0.520737f, 0.416470f },
			{ 0.875f, 0.867254f, 0.354740f, 0.259232f },
			{ 1.000f, 0.705673f, 0.015556f, 0.150233f },
		};

		// blue-white-red  (diverging)
		static const FCFDVizColorStop BlueWhiteRedStops[] = {
			{ 0.000f, 0.000000f, 0.000000f, 1.000000f },
			{ 0.500f, 1.000000f, 1.000000f, 1.000000f },
			{ 1.000f, 1.000000f, 0.000000f, 0.000000f },
		};

		// grayscale  (perceptually uniform)
		static const FCFDVizColorStop GrayscaleStops[] = {
			{ 0.000f, 0.000000f, 0.000000f, 0.000000f },
			{ 1.000f, 1.000000f, 1.000000f, 1.000000f },
		};

		/** Stable names. These MUST match the Python dict keys exactly. */
		static const FName MapNames[] = {
			FName(TEXT("viridis")),
			FName(TEXT("plasma")),
			FName(TEXT("inferno")),
			FName(TEXT("magma")),
			FName(TEXT("turbo")),
			FName(TEXT("coolwarm")),
			FName(TEXT("blue-white-red")),
			FName(TEXT("grayscale")),
		};

		static_assert(UE_ARRAY_COUNT(MapNames) == static_cast<int32>(ECFDVizColorMap::Count),
			"MapNames must have one entry per ECFDVizColorMap value.");
	}

	TArrayView<const FCFDVizColorStop> GetStops(ECFDVizColorMap Map)
	{
		switch (Map)
		{
		case ECFDVizColorMap::Viridis:		return MakeArrayView(ViridisStops);
		case ECFDVizColorMap::Plasma:		return MakeArrayView(PlasmaStops);
		case ECFDVizColorMap::Inferno:		return MakeArrayView(InfernoStops);
		case ECFDVizColorMap::Magma:		return MakeArrayView(MagmaStops);
		case ECFDVizColorMap::Turbo:		return MakeArrayView(TurboStops);
		case ECFDVizColorMap::CoolWarm:		return MakeArrayView(CoolWarmStops);
		case ECFDVizColorMap::BlueWhiteRed:	return MakeArrayView(BlueWhiteRedStops);
		case ECFDVizColorMap::Grayscale:	return MakeArrayView(GrayscaleStops);
		default:							return MakeArrayView(ViridisStops);
		}
	}

	FName GetName(ECFDVizColorMap Map)
	{
		const int32 Index = static_cast<int32>(Map);
		return (Index >= 0 && Index < UE_ARRAY_COUNT(MapNames)) ? MapNames[Index] : MapNames[0];
	}

	bool TryParse(FName Name, ECFDVizColorMap& OutMap)
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(MapNames); ++Index)
		{
			if (MapNames[Index] == Name)
			{
				OutMap = static_cast<ECFDVizColorMap>(Index);
				return true;
			}
		}
		return false;
	}

	bool IsPerceptuallyUniform(ECFDVizColorMap Map)
	{
		switch (Map)
		{
		case ECFDVizColorMap::Viridis:
		case ECFDVizColorMap::Plasma:
		case ECFDVizColorMap::Inferno:
		case ECFDVizColorMap::Magma:
		case ECFDVizColorMap::Grayscale:
			return true;
		default:
			return false;
		}
	}

	bool IsDiverging(ECFDVizColorMap Map)
	{
		return Map == ECFDVizColorMap::CoolWarm || Map == ECFDVizColorMap::BlueWhiteRed;
	}

	FLinearColor Sample(ECFDVizColorMap Map, float Position)
	{
		const TArrayView<const FCFDVizColorStop> Stops = GetStops(Map);
		check(Stops.Num() > 0);

		// Clamped here; under/over-range colors are a display decision made by
		// the caller, not a property of the colormap itself.
		Position = FMath::Clamp(Position, 0.0f, 1.0f);

		// Endpoints exactly, so a full-range LUT reproduces the true ends.
		if (Position <= Stops[0].Position)
		{
			return FLinearColor(Stops[0].R, Stops[0].G, Stops[0].B, 1.0f);
		}

		const FCFDVizColorStop& Last = Stops[Stops.Num() - 1];
		if (Position >= Last.Position)
		{
			return FLinearColor(Last.R, Last.G, Last.B, 1.0f);
		}

		for (int32 Index = 0; Index < Stops.Num() - 1; ++Index)
		{
			const FCFDVizColorStop& A = Stops[Index];
			const FCFDVizColorStop& B = Stops[Index + 1];
			if (Position >= A.Position && Position <= B.Position)
			{
				const float Span = B.Position - A.Position;
				const float Alpha = (Span <= 0.0f) ? 0.0f : (Position - A.Position) / Span;
				return FLinearColor(
					FMath::Lerp(A.R, B.R, Alpha),
					FMath::Lerp(A.G, B.G, Alpha),
					FMath::Lerp(A.B, B.B, Alpha),
					1.0f);
			}
		}

		return FLinearColor(Last.R, Last.G, Last.B, 1.0f);
	}

	void BuildLut(ECFDVizColorMap Map, TArray<FLinearColor>& OutLut, int32 Size, bool bReverse, int32 Bands)
	{
		if (Size <= 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("BuildLut: invalid size %d; using 256."), Size);
			Size = 256;
		}

		OutLut.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			// Pixel-center sampling. Must match the Python side, or the two
			// renders disagree by half a texel.
			float Position = (static_cast<float>(Index) + 0.5f) / static_cast<float>(Size);

			if (Bands > 0)
			{
				// Snap to the band center so each band shows its representative
				// color rather than the color at its lower edge.
				const int32 Band = FMath::Min(Bands - 1, static_cast<int32>(Position * Bands));
				Position = (static_cast<float>(Band) + 0.5f) / static_cast<float>(Bands);
			}

			if (bReverse)
			{
				Position = 1.0f - Position;
			}

			OutLut[Index] = Sample(Map, Position);
		}
	}
}
