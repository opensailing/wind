// Copyright FlowViz contributors. All Rights Reserved.

#include "Capture/FlowVizAnnotate.h"

namespace FlowVizAnnotateLocal
{
	/*
	 * A 5x7 bitmap font, ASCII 32..90 (space through Z, lowercase folded up by
	 * the drawer). Each glyph is five column bytes, seven low bits each,
	 * bit 0 = top row. Hand-derived from the classic 5x7 LCD set.
	 */
	constexpr int32 GlyphWidth = 5;
	constexpr int32 GlyphHeight = 7;
	constexpr int32 FirstGlyph = 32;
	constexpr int32 LastGlyph = 90;

	constexpr uint8 Font[LastGlyph - FirstGlyph + 1][GlyphWidth] = {
		{ 0x00, 0x00, 0x00, 0x00, 0x00 },  // ' '
		{ 0x00, 0x00, 0x5F, 0x00, 0x00 },  // '!'
		{ 0x00, 0x07, 0x00, 0x07, 0x00 },  // '"'
		{ 0x14, 0x7F, 0x14, 0x7F, 0x14 },  // '#'
		{ 0x24, 0x2A, 0x7F, 0x2A, 0x12 },  // '$'
		{ 0x23, 0x13, 0x08, 0x64, 0x62 },  // '%'
		{ 0x36, 0x49, 0x55, 0x22, 0x50 },  // '&'
		{ 0x00, 0x05, 0x03, 0x00, 0x00 },  // '\''
		{ 0x00, 0x1C, 0x22, 0x41, 0x00 },  // '('
		{ 0x00, 0x41, 0x22, 0x1C, 0x00 },  // ')'
		{ 0x14, 0x08, 0x3E, 0x08, 0x14 },  // '*'
		{ 0x08, 0x08, 0x3E, 0x08, 0x08 },  // '+'
		{ 0x00, 0x50, 0x30, 0x00, 0x00 },  // ','
		{ 0x08, 0x08, 0x08, 0x08, 0x08 },  // '-'
		{ 0x00, 0x60, 0x60, 0x00, 0x00 },  // '.'
		{ 0x20, 0x10, 0x08, 0x04, 0x02 },  // '/'
		{ 0x3E, 0x51, 0x49, 0x45, 0x3E },  // '0'
		{ 0x00, 0x42, 0x7F, 0x40, 0x00 },  // '1'
		{ 0x42, 0x61, 0x51, 0x49, 0x46 },  // '2'
		{ 0x21, 0x41, 0x45, 0x4B, 0x31 },  // '3'
		{ 0x18, 0x14, 0x12, 0x7F, 0x10 },  // '4'
		{ 0x27, 0x45, 0x45, 0x45, 0x39 },  // '5'
		{ 0x3C, 0x4A, 0x49, 0x49, 0x30 },  // '6'
		{ 0x01, 0x71, 0x09, 0x05, 0x03 },  // '7'
		{ 0x36, 0x49, 0x49, 0x49, 0x36 },  // '8'
		{ 0x06, 0x49, 0x49, 0x29, 0x1E },  // '9'
		{ 0x00, 0x36, 0x36, 0x00, 0x00 },  // ':'
		{ 0x00, 0x56, 0x36, 0x00, 0x00 },  // ';'
		{ 0x08, 0x14, 0x22, 0x41, 0x00 },  // '<'
		{ 0x14, 0x14, 0x14, 0x14, 0x14 },  // '='
		{ 0x00, 0x41, 0x22, 0x14, 0x08 },  // '>'
		{ 0x02, 0x01, 0x51, 0x09, 0x06 },  // '?'
		{ 0x32, 0x49, 0x79, 0x41, 0x3E },  // '@'
		{ 0x7E, 0x11, 0x11, 0x11, 0x7E },  // 'A'
		{ 0x7F, 0x49, 0x49, 0x49, 0x36 },  // 'B'
		{ 0x3E, 0x41, 0x41, 0x41, 0x22 },  // 'C'
		{ 0x7F, 0x41, 0x41, 0x22, 0x1C },  // 'D'
		{ 0x7F, 0x49, 0x49, 0x49, 0x41 },  // 'E'
		{ 0x7F, 0x09, 0x09, 0x09, 0x01 },  // 'F'
		{ 0x3E, 0x41, 0x49, 0x49, 0x7A },  // 'G'
		{ 0x7F, 0x08, 0x08, 0x08, 0x7F },  // 'H'
		{ 0x00, 0x41, 0x7F, 0x41, 0x00 },  // 'I'
		{ 0x20, 0x40, 0x41, 0x3F, 0x01 },  // 'J'
		{ 0x7F, 0x08, 0x14, 0x22, 0x41 },  // 'K'
		{ 0x7F, 0x40, 0x40, 0x40, 0x40 },  // 'L'
		{ 0x7F, 0x02, 0x0C, 0x02, 0x7F },  // 'M'
		{ 0x7F, 0x04, 0x08, 0x10, 0x7F },  // 'N'
		{ 0x3E, 0x41, 0x41, 0x41, 0x3E },  // 'O'
		{ 0x7F, 0x09, 0x09, 0x09, 0x06 },  // 'P'
		{ 0x3E, 0x41, 0x51, 0x21, 0x5E },  // 'Q'
		{ 0x7F, 0x09, 0x19, 0x29, 0x46 },  // 'R'
		{ 0x46, 0x49, 0x49, 0x49, 0x31 },  // 'S'
		{ 0x01, 0x01, 0x7F, 0x01, 0x01 },  // 'T'
		{ 0x3F, 0x40, 0x40, 0x40, 0x3F },  // 'U'
		{ 0x1F, 0x20, 0x40, 0x20, 0x1F },  // 'V'
		{ 0x3F, 0x40, 0x38, 0x40, 0x3F },  // 'W'
		{ 0x63, 0x14, 0x08, 0x14, 0x63 },  // 'X'
		{ 0x07, 0x08, 0x70, 0x08, 0x07 },  // 'Y'
		{ 0x61, 0x51, 0x49, 0x45, 0x43 },  // 'Z'
	};

	const FColor FooterBackground(18, 18, 22, 255);
	const FColor CaptionColor(235, 235, 235, 255);
}

void FlowVizAnnotate::DrawText(
	TArray<FColor>& Pixels,
	int32 Width,
	int32 Height,
	int32 X,
	int32 Y,
	const FString& Text,
	const FColor& Color,
	int32 Scale)
{
	using namespace FlowVizAnnotateLocal;

	Scale = FMath::Max(1, Scale);
	int32 PenX = X;
	for (const TCHAR Raw : Text)
	{
		TCHAR Character = FChar::ToUpper(Raw);
		if (Character < FirstGlyph || Character > LastGlyph)
		{
			Character = TEXT('?');
		}
		const uint8* Glyph = Font[Character - FirstGlyph];

		for (int32 Column = 0; Column < GlyphWidth; ++Column)
		{
			for (int32 Row = 0; Row < GlyphHeight; ++Row)
			{
				if ((Glyph[Column] & (1 << Row)) == 0)
				{
					continue;
				}
				for (int32 SY = 0; SY < Scale; ++SY)
				{
					for (int32 SX = 0; SX < Scale; ++SX)
					{
						const int32 PX = PenX + Column * Scale + SX;
						const int32 PY = Y + Row * Scale + SY;
						if (PX >= 0 && PX < Width && PY >= 0 && PY < Height)
						{
							Pixels[PY * Width + PX] = Color;
						}
					}
				}
			}
		}
		PenX += (GlyphWidth + 1) * Scale;
	}
}

bool FlowVizAnnotate::BurnFooter(
	TArray<FColor>& Pixels,
	int32 Width,
	int32 Height,
	const FFlowVizCaptureAnnotation& Annotation)
{
	using namespace FlowVizAnnotateLocal;

	if (Width < 320 || Height < FooterHeight * 2
		|| Pixels.Num() != Width * Height)
	{
		// Too small for a legible footer, or the buffer disagrees with the
		// stated size. Nothing is modified: half an annotation is worse than
		// none, and a partial write into a mis-sized buffer is a crash.
		return false;
	}

	const int32 FooterTop = Height - FooterHeight;

	/* -- The strip: opaque, so the caption never fights the render. -------- */
	for (int32 Y = FooterTop; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			Pixels[Y * Width + X] = FooterBackground;
		}
	}

	/* -- Caption: case, field, time -- the identity DoD 15 names. ---------- */
	const FString Caption = FString::Printf(TEXT("%s  |  %s  |  T = %.4g %s"),
		*Annotation.CaseName, *Annotation.FieldName, Annotation.Time, *Annotation.TimeUnit);
	DrawText(Pixels, Width, Height, 8, FooterTop + 8, Caption, CaptionColor, 2);

	/* -- The legend: the REAL colormap, sampled per column. ----------------- */
	const int32 LegendWidth = FMath::Min(Width / 3, 360);
	const int32 LegendHeight = 12;
	const int32 LegendLeft = Width - LegendWidth - 8;
	const int32 LegendTop = FooterTop + 8;

	for (int32 X = 0; X < LegendWidth; ++X)
	{
		// The lookup runs the strip left-to-right as min-to-max; reversal flips
		// the LOOKUP, matching the ramp widget and the shader (one definition
		// of "reversed" everywhere).
		const float Position = static_cast<float>(X) / (LegendWidth - 1);
		const float Lookup = Annotation.bReversed ? 1.0f - Position : Position;
		const FColor Sample =
			CFDViz::ColorMaps::Sample(Annotation.ColorMap, Lookup).ToFColor(/*sRGB*/ true);
		for (int32 Y = 0; Y < LegendHeight; ++Y)
		{
			Pixels[(LegendTop + Y) * Width + (LegendLeft + X)] = Sample;
		}
	}

	/* -- Range labels under the bar's ends. --------------------------------- */
	const FString MinLabel = FString::Printf(TEXT("%.4g"), Annotation.RangeMin);
	const FString MaxLabel = FString::Printf(TEXT("%.4g"), Annotation.RangeMax);
	DrawText(Pixels, Width, Height, LegendLeft, LegendTop + LegendHeight + 4,
		MinLabel, CaptionColor, 1);
	// Right-aligned: 6 pixels per scaled glyph column (5 + 1 spacing).
	DrawText(Pixels, Width, Height,
		LegendLeft + LegendWidth - MaxLabel.Len() * 6, LegendTop + LegendHeight + 4,
		MaxLabel, CaptionColor, 1);

	return true;
}
