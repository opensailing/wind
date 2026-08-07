// Copyright FlowViz contributors. All Rights Reserved.

#include "Render/FlowVizColorMapTexture.h"

#include "Engine/Texture2D.h"

void FlowVizColorMapTexture::BuildLutBytes(ECFDVizColorMap Map, TArray<FColor>& OutBytes)
{
	OutBytes.SetNumUninitialized(LutWidth);
	for (int32 Index = 0; Index < LutWidth; ++Index)
	{
		// Texel centres: the material's sampler reads (i + 0.5) / W, so the
		// bytes are built at the same positions or the parity test would
		// compare different sample points and "pass" only by tolerance.
		const float Position = (Index + 0.5f) / LutWidth;
		const FLinearColor Linear = CFDViz::ColorMaps::Sample(Map, Position);

		// QuantizeRound, not the truncating ToFColor: a LUT is 256 buckets of
		// the authority, and truncation would bias every bucket down half a
		// step -- visible as a dimmer ramp than the Slate legend's.
		OutBytes[Index] = Linear.QuantizeRound();
	}
}

UTexture2D* FlowVizColorMapTexture::CreateLutTexture(ECFDVizColorMap Map)
{
	TArray<FColor> Bytes;
	BuildLutBytes(Map, Bytes);

	UTexture2D* Texture = UTexture2D::CreateTransient(LutWidth, 1, PF_B8G8R8A8);
	if (Texture == nullptr)
	{
		return nullptr;
	}

	// LINEAR, CLAMPED, NO MIPS. The LUT is linear color (sRGB would re-encode
	// the authority's values); clamping is the shader-side twin of Sample's
	// [0,1] clamp; a mip chain would blend adjacent buckets at distance.
	Texture->SRGB = false;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->CompressionSettings = TC_EditorIcon;   // uncompressed BGRA8
	Texture->MipGenSettings = TMGS_NoMipmaps;

	void* Data = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Bytes.GetData(), Bytes.Num() * sizeof(FColor));
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}
