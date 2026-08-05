// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"

/**
 * See FlowVizWorkspaceStyle.h for why the tokens are centralised.
 *
 * HOW THE PALETTE WAS CHOSEN. The surfaces are a near-neutral dark ramp with a
 * slight cool cast - enough that the UI does not look like dead grey, little
 * enough that FlowViz.UI.WorkspaceStyle.Tokens' neutrality assertion holds and
 * a viridis or coolwarm ramp beside it is not perceptually shifted. The steps
 * between background, panel and raised well are luminance steps rather than hue
 * steps, so the hierarchy survives being viewed in greyscale or by a
 * colour-blind reader.
 *
 * THE ACCENTS ARE THE ONLY CHROMATIC ELEMENTS AND EACH MEANS ONE THING:
 *
 *   Accent   - cyan. Interaction: focus, selection, the playhead. NEVER data.
 *   Advisory - amber. Engineering rule 7 and rule 8: this frame is interpolated,
 *              this range is per-frame. The user is being told something about
 *              the FIDELITY of what they are looking at.
 *   Warning  - rose. A refusal or a stale hold: what is on screen is not what
 *              was asked for.
 *
 * Cyan and amber are near-complementary, so the two states are separable at a
 * glance and remain separable under the common red-green deficiencies - which
 * a green/red pairing would not be.
 */

namespace FlowVizWorkspaceStyleLocal
{
	/* --- Surfaces. A luminance ramp, not a hue ramp. ----------------------- */

	const FLinearColor Background = FLinearColor(0.0175f, 0.0195f, 0.0225f, 1.0f);
	const FLinearColor Panel = FLinearColor(0.0345f, 0.0375f, 0.0425f, 1.0f);
	const FLinearColor PanelRaised = FLinearColor(0.0585f, 0.0625f, 0.0695f, 1.0f);
	const FLinearColor Border = FLinearColor(0.105f, 0.112f, 0.125f, 1.0f);

	/* --- Text -------------------------------------------------------------- */

	// Not pure white. A 1.0 white on a near-black panel is a ~19:1 ratio that
	// haloes on an LCD and is fatiguing over a long session; 0.86 keeps AAA with
	// room to spare while reading as calm.
	const FLinearColor TextPrimary = FLinearColor(0.86f, 0.875f, 0.90f, 1.0f);
	const FLinearColor TextSecondary = FLinearColor(0.52f, 0.545f, 0.585f, 1.0f);
	const FLinearColor TextDisabled = FLinearColor(0.28f, 0.295f, 0.325f, 1.0f);

	/* --- Accents ----------------------------------------------------------- */

	const FLinearColor Accent = FLinearColor(0.145f, 0.675f, 0.785f, 1.0f);
	const FLinearColor Advisory = FLinearColor(0.885f, 0.615f, 0.155f, 1.0f);
	const FLinearColor Warning = FLinearColor(0.855f, 0.315f, 0.355f, 1.0f);

	/* --- Metrics ----------------------------------------------------------- */

	constexpr float Unit = 4.0f;
	constexpr float CornerRadius = 4.0f;
}

const FLinearColor& FlowVizWorkspaceStyle::GetBackgroundColor()
{
	return FlowVizWorkspaceStyleLocal::Background;
}

const FLinearColor& FlowVizWorkspaceStyle::GetPanelColor()
{
	return FlowVizWorkspaceStyleLocal::Panel;
}

const FLinearColor& FlowVizWorkspaceStyle::GetPanelRaisedColor()
{
	return FlowVizWorkspaceStyleLocal::PanelRaised;
}

const FLinearColor& FlowVizWorkspaceStyle::GetBorderColor()
{
	return FlowVizWorkspaceStyleLocal::Border;
}

const FLinearColor& FlowVizWorkspaceStyle::GetTextPrimaryColor()
{
	return FlowVizWorkspaceStyleLocal::TextPrimary;
}

const FLinearColor& FlowVizWorkspaceStyle::GetTextSecondaryColor()
{
	return FlowVizWorkspaceStyleLocal::TextSecondary;
}

const FLinearColor& FlowVizWorkspaceStyle::GetTextDisabledColor()
{
	return FlowVizWorkspaceStyleLocal::TextDisabled;
}

const FLinearColor& FlowVizWorkspaceStyle::GetAccentColor()
{
	return FlowVizWorkspaceStyleLocal::Accent;
}

const FLinearColor& FlowVizWorkspaceStyle::GetAdvisoryColor()
{
	return FlowVizWorkspaceStyleLocal::Advisory;
}

const FLinearColor& FlowVizWorkspaceStyle::GetWarningColor()
{
	return FlowVizWorkspaceStyleLocal::Warning;
}

float FlowVizWorkspaceStyle::GetUnit()
{
	return FlowVizWorkspaceStyleLocal::Unit;
}

float FlowVizWorkspaceStyle::GetCornerRadius()
{
	return FlowVizWorkspaceStyleLocal::CornerRadius;
}

/* ========================================================================== */
/* Type                                                                        */
/* ========================================================================== */

FSlateFontInfo FlowVizWorkspaceStyle::GetHeadingFont()
{
	// "Bold" and "Regular" are typeface names guaranteed by the engine's own
	// Roboto set, so this resolves headlessly and in a packaged build without
	// needing a font asset from Content.
	return FCoreStyle::GetDefaultFontStyle("Bold", 11);
}

FSlateFontInfo FlowVizWorkspaceStyle::GetLabelFont()
{
	return FCoreStyle::GetDefaultFontStyle("Regular", 9);
}

FSlateFontInfo FlowVizWorkspaceStyle::GetNumericFont()
{
	// Mono, for the reason given in the header: a proportional digit set makes a
	// live readout jitter horizontally and defeats decimal alignment between two
	// probes. Roboto Mono ships with the engine.
	return FCoreStyle::GetDefaultFontStyle("Mono", 9);
}

FSlateFontInfo FlowVizWorkspaceStyle::GetCaptionFont()
{
	return FCoreStyle::GetDefaultFontStyle("Bold", 7);
}

/* ========================================================================== */
/* Brushes                                                                     */
/* ========================================================================== */

const FSlateBrush* FlowVizWorkspaceStyle::GetPanelBrush()
{
	// Function-local statics rather than globals: brush construction touches
	// Slate types whose static initialisation order relative to this module is
	// not guaranteed, and a brush built too early is a crash at load rather than
	// a visual defect. They are also singletons, which the token test asserts -
	// a fresh brush per call would allocate on every paint.
	static const FSlateRoundedBoxBrush Brush(
		FlowVizWorkspaceStyleLocal::Panel, FlowVizWorkspaceStyleLocal::CornerRadius);
	return &Brush;
}

const FSlateBrush* FlowVizWorkspaceStyle::GetRaisedBrush()
{
	static const FSlateRoundedBoxBrush Brush(
		FlowVizWorkspaceStyleLocal::PanelRaised, FlowVizWorkspaceStyleLocal::CornerRadius);
	return &Brush;
}

const FSlateBrush* FlowVizWorkspaceStyle::GetFlatBrush()
{
	static const FSlateColorBrush Brush(FLinearColor::White);
	return &Brush;
}

const FButtonStyle& FlowVizWorkspaceStyle::GetToolButtonStyle()
{
	static const FButtonStyle Style = []()
	{
		FButtonStyle Result;
		// Hover and press are LUMINANCE steps of the same neutral, not a hue
		// change. A button that shifts hue on hover competes with the data for
		// attention every time the pointer crosses it.
		Result.SetNormal(FSlateRoundedBoxBrush(
			FlowVizWorkspaceStyleLocal::PanelRaised, FlowVizWorkspaceStyleLocal::CornerRadius));
		Result.SetHovered(FSlateRoundedBoxBrush(
			FLinearColor(0.098f, 0.104f, 0.115f, 1.0f), FlowVizWorkspaceStyleLocal::CornerRadius));
		Result.SetPressed(FSlateRoundedBoxBrush(
			FLinearColor(0.135f, 0.143f, 0.158f, 1.0f), FlowVizWorkspaceStyleLocal::CornerRadius));
		Result.SetDisabled(FSlateRoundedBoxBrush(
			FLinearColor(0.042f, 0.045f, 0.050f, 1.0f), FlowVizWorkspaceStyleLocal::CornerRadius));
		Result.SetNormalForeground(FSlateColor(FlowVizWorkspaceStyleLocal::TextPrimary));
		Result.SetHoveredForeground(FSlateColor(FLinearColor::White));
		Result.SetPressedForeground(FSlateColor(FLinearColor::White));
		// A disabled control is DIM, not hidden. Rule 15 permits a marked-
		// unavailable control; what it forbids is one that looks live and is not.
		Result.SetDisabledForeground(FSlateColor(FlowVizWorkspaceStyleLocal::TextDisabled));
		Result.SetNormalPadding(FMargin(8.0f, 4.0f));
		Result.SetPressedPadding(FMargin(8.0f, 4.0f));
		return Result;
	}();
	return Style;
}

const FButtonStyle& FlowVizWorkspaceStyle::GetSelectedToolButtonStyle()
{
	static const FButtonStyle Style = []()
	{
		FButtonStyle Result = GetToolButtonStyle();
		// SELECTION IS THE ONE PLACE CHROME MAY BE CHROMATIC. A selected preset
		// must be identifiable without reading it, and a luminance-only "selected"
		// state is indistinguishable from "hovered" at a glance.
		const FLinearColor SelectedFill(0.055f, 0.185f, 0.215f, 1.0f);
		Result.SetNormal(FSlateRoundedBoxBrush(
			SelectedFill, FlowVizWorkspaceStyleLocal::CornerRadius));
		Result.SetHovered(FSlateRoundedBoxBrush(
			FLinearColor(0.075f, 0.235f, 0.270f, 1.0f), FlowVizWorkspaceStyleLocal::CornerRadius));
		Result.SetPressed(FSlateRoundedBoxBrush(
			FLinearColor(0.095f, 0.285f, 0.325f, 1.0f), FlowVizWorkspaceStyleLocal::CornerRadius));
		Result.SetNormalForeground(FSlateColor(FlowVizWorkspaceStyleLocal::Accent));
		return Result;
	}();
	return Style;
}
