// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateTypes.h"

/**
 * The workspace's design tokens (plan.md section 11).
 *
 * WHY A TOKEN TABLE AND NOT LITERALS AT THE CALL SITES. Every colour and metric
 * a panel uses is named here once. Scattering FLinearColor(0.07f, ...) through
 * five panels is how a UI ends up with four slightly different greys that read
 * as rendering artefacts, and it makes a global change - a light profile,
 * plan.md section 11's "presentation profile" - a search-and-replace over
 * hundreds of call sites rather than an edit to one table.
 *
 * THE PALETTE IS DELIBERATELY NEUTRAL AND DESATURATED. This is a scientific
 * instrument: the DATA carries the colour, and the chrome must not compete with
 * it. A saturated interface hue sitting next to a viridis ramp shifts the
 * apparent colour of the ramp - simultaneous contrast - which in a tool whose
 * entire job is quantitative colour is not a matter of taste. Chrome is
 * therefore held at low chroma, and the one accent is reserved for interactive
 * affordances and never used to encode data.
 *
 * CONTRAST IS A REQUIREMENT, NOT A PREFERENCE. Primary text on the panel
 * background is above 12:1, secondary above 5:1, so a readout stays legible on
 * a projector in a lit room - which is where these figures actually get
 * presented.
 *
 * NOT A UMG/Slate STYLE ASSET. Those live in Content and cannot be constructed
 * headlessly, so a test could not read the values a widget was built with. This
 * is plain C++ data with no engine registration required.
 */
namespace FlowVizWorkspaceStyle
{
	/* --- Surfaces --------------------------------------------------------- */

	/** The viewport surround. Darkest, so a bright volume reads as emissive against it. */
	FLOWVIZRUNTIME_API const FLinearColor& GetBackgroundColor();

	/** A docked panel's fill. */
	FLOWVIZRUNTIME_API const FLinearColor& GetPanelColor();

	/** A raised group inside a panel - a plane row, a readout well. */
	FLOWVIZRUNTIME_API const FLinearColor& GetPanelRaisedColor();

	/** Hairline between surfaces. Low contrast on purpose: it separates, it does not decorate. */
	FLOWVIZRUNTIME_API const FLinearColor& GetBorderColor();

	/* --- Text ------------------------------------------------------------- */

	FLOWVIZRUNTIME_API const FLinearColor& GetTextPrimaryColor();
	FLOWVIZRUNTIME_API const FLinearColor& GetTextSecondaryColor();

	/** Disabled controls. Rule 15 hides or marks dead controls; this styles the marked ones. */
	FLOWVIZRUNTIME_API const FLinearColor& GetTextDisabledColor();

	/* --- Semantic accents ------------------------------------------------- */

	/**
	 * The single interactive accent. Interaction only - never a data colour.
	 *
	 * A UI that tints a control with a hue also used by a colormap invites the
	 * reading that the control is showing a value.
	 */
	FLOWVIZRUNTIME_API const FLinearColor& GetAccentColor();

	/**
	 * Advisory amber. This is engineering rule 7 and rule 8 wearing a colour:
	 * interpolated frames and per-frame colour ranges must be VISIBLY identified,
	 * and this is what identifies them.
	 */
	FLOWVIZRUNTIME_API const FLinearColor& GetAdvisoryColor();

	/** A held/stale frame or a refused edit. Stronger than advisory. */
	FLOWVIZRUNTIME_API const FLinearColor& GetWarningColor();

	/* --- Metrics ---------------------------------------------------------- */

	/** Base spacing unit. Every margin in the workspace is a multiple of it, so rhythm is consistent by construction. */
	FLOWVIZRUNTIME_API float GetUnit();

	/** Corner radius for panels and wells. */
	FLOWVIZRUNTIME_API float GetCornerRadius();

	/* --- Type ------------------------------------------------------------- */

	/** Panel and section headings. */
	FLOWVIZRUNTIME_API FSlateFontInfo GetHeadingFont();

	/** Control labels and body text. */
	FLOWVIZRUNTIME_API FSlateFontInfo GetLabelFont();

	/**
	 * NUMERIC READOUTS ARE MONOSPACED, AND THAT IS A CORRECTNESS DECISION.
	 *
	 * A proportional digit set changes width as a value animates, so a probe
	 * readout jitters horizontally while playback runs and the eye reads the
	 * motion as the number changing more than it is. Fixed-advance digits also
	 * let a column of readings align on the decimal point, which is what makes
	 * two probes comparable at a glance.
	 */
	FLOWVIZRUNTIME_API FSlateFontInfo GetNumericFont();

	/** Small caps-ish label for badges and units. */
	FLOWVIZRUNTIME_API FSlateFontInfo GetCaptionFont();

	/* --- Brushes ---------------------------------------------------------- */

	/** Rounded fill for a panel. */
	FLOWVIZRUNTIME_API const FSlateBrush* GetPanelBrush();

	/** Rounded fill for a raised well inside a panel. */
	FLOWVIZRUNTIME_API const FSlateBrush* GetRaisedBrush();

	/** Flat fill, no rounding - separators and gradient backdrops. */
	FLOWVIZRUNTIME_API const FSlateBrush* GetFlatBrush();

	/** Transport and tool buttons, styled to the token table rather than the editor's theme. */
	FLOWVIZRUNTIME_API const FButtonStyle& GetToolButtonStyle();

	/** A button that reads as selected while a predicate holds - a speed preset, an axis preset. */
	FLOWVIZRUNTIME_API const FButtonStyle& GetSelectedToolButtonStyle();
}
