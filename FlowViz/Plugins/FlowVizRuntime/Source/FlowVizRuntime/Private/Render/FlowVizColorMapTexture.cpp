// Copyright FlowViz contributors. All Rights Reserved.

#include "Render/FlowVizColorMapTexture.h"

#include "Engine/Texture2D.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizColorMapTextureLocal
{
	/** Explicit RGBA byte order -- see CreateLutTexture's format note. */
	struct FRgba8
	{
		uint8 R = 0;
		uint8 G = 0;
		uint8 B = 0;
		uint8 A = 255;
	};
}

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

		/*
		 * sRGB-ENCODED, BY THE CONVENTION FLIP THE COMPILER FORCED: a Linear
		 * Color sampler rejects any default texture whose asset is sRGB --
		 * including every engine builtin -- so the material could never
		 * compile against a default. Color sampler + sRGB texture instead:
		 * ToFColor(true) encodes, the GPU decodes, and the sampled value is
		 * the authority's linear color either way. The parity test reproduces
		 * THIS arithmetic.
		 */
		OutBytes[Index] = Linear.ToFColor(/*bSRGB*/ true);
	}
}

UTexture2D* FlowVizColorMapTexture::CreateLutTexture(ECFDVizColorMap Map)
{
	using namespace FlowVizColorMapTextureLocal;

	TArray<FColor> Bytes;
	BuildLutBytes(Map, Bytes);

	/*
	 * EXPLICIT RGBA, NOT FColor-memcpy-into-BGRA. The first colored capture
	 * came out with viridis's yellow reading CYAN and its purple reading
	 * MAGENTA -- the R<->B swap signature: Metal's transient-texture path did
	 * not honour PF_B8G8R8A8's byte order. Writing each channel by name into
	 * an RGBA-format texture removes the ambiguity on every backend.
	 */
	TArray<FRgba8> Rgba;
	Rgba.SetNumUninitialized(Bytes.Num());
	for (int32 Index = 0; Index < Bytes.Num(); ++Index)
	{
		Rgba[Index].R = Bytes[Index].R;
		Rgba[Index].G = Bytes[Index].G;
		Rgba[Index].B = Bytes[Index].B;
		Rgba[Index].A = Bytes[Index].A;
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(LutWidth, 1, PF_R8G8B8A8);
	if (Texture == nullptr)
	{
		return nullptr;
	}

	// sRGB, CLAMPED, NO MIPS. See BuildLutBytes: the bytes are sRGB-encoded
	// and the hardware decode returns the authority's linear values. Clamping
	// is the shader-side twin of Sample's [0,1] clamp; a mip chain would
	// blend adjacent buckets at distance.
	Texture->SRGB = true;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->CompressionSettings = TC_EditorIcon;   // uncompressed BGRA8
#if WITH_EDITORONLY_DATA
	// Editor-only field (the packaged build has no mip GENERATION to disable;
	// CreateTransient already made a single-mip texture there).
	Texture->MipGenSettings = TMGS_NoMipmaps;
#endif

	void* Data = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Rgba.GetData(), Rgba.Num() * sizeof(FRgba8));
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}
