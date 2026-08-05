// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceStyle.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The design tokens (plan.md section 11).
 *
 * WHAT IS ASSERTABLE ABOUT A COLOUR AND WHAT IS NOT. "Is this palette
 * beautiful" is not a test. "Is this text legible on this background" is: it is
 * a contrast ratio with a published threshold, and a palette edit that drops a
 * readout below it is a real regression that nobody notices by looking, because
 * each individual change looks fine.
 *
 * These tests therefore assert the properties a redesign must PRESERVE:
 *
 *  1. Text meets WCAG contrast on the surface it is actually drawn on.
 *  2. The chrome stays low-chroma, so it does not shift the apparent colour of
 *     the data through simultaneous contrast. A scientific colormap sitting
 *     next to a saturated interface panel is measurably misread.
 *  3. The advisory and warning accents are DISTINGUISHABLE from each other and
 *     from the interactive accent - they encode different meanings (rule 7's
 *     interpolation disclosure versus a refusal), and two tokens that resolve
 *     to near-identical colours make that disclosure invisible.
 *  4. Spacing is a positive base unit, because every margin is a multiple of it
 *     and a zero would silently collapse the whole layout.
 *
 * WHY THE CONTRAST HELPER IS SPELLED OUT HERE. It is the WCAG 2.1 relative
 * luminance formula. Writing it in the test rather than calling a production
 * helper is deliberate: a shared implementation would let a bug in the formula
 * make both the production check and the test agree on a wrong answer.
 */

// NAMED namespace: FlowVizRuntime is a unity build, so anonymous namespaces from
// every .cpp merge and same-named helpers collide across test files.
namespace FlowVizWorkspaceStyleTest
{
	/** WCAG 2.1 relative luminance. Input is linear, so the sRGB decode step is already done. */
	double RelativeLuminance(const FLinearColor& Color)
	{
		// The coefficients are the WCAG ones. Clamped because a token authored
		// slightly out of gamut would otherwise produce a ratio above the
		// theoretical maximum and pass a threshold it should fail.
		const double R = FMath::Clamp(static_cast<double>(Color.R), 0.0, 1.0);
		const double G = FMath::Clamp(static_cast<double>(Color.G), 0.0, 1.0);
		const double B = FMath::Clamp(static_cast<double>(Color.B), 0.0, 1.0);
		return 0.2126 * R + 0.7152 * G + 0.0722 * B;
	}

	/** WCAG contrast ratio, 1.0 (identical) to 21.0 (black on white). */
	double ContrastRatio(const FLinearColor& A, const FLinearColor& B)
	{
		const double LA = RelativeLuminance(A);
		const double LB = RelativeLuminance(B);
		const double Lighter = FMath::Max(LA, LB);
		const double Darker = FMath::Min(LA, LB);
		return (Lighter + 0.05) / (Darker + 0.05);
	}

	/**
	 * Chroma as the spread between the largest and smallest channel.
	 *
	 * A cheap stand-in for saturation that needs no colour-space conversion. It
	 * is sufficient for the property under test - "is this chrome colour close
	 * to neutral" - and being arithmetic on the stored values it cannot drift
	 * from what the widget is actually built with.
	 */
	double ChannelSpread(const FLinearColor& Color)
	{
		const double MaxC = FMath::Max3(Color.R, Color.G, Color.B);
		const double MinC = FMath::Min3(Color.R, Color.G, Color.B);
		return MaxC - MinC;
	}

	/** Euclidean distance in linear RGB. Used only to assert two accents are not the same colour. */
	double ColorDistance(const FLinearColor& A, const FLinearColor& B)
	{
		return FMath::Sqrt(
			FMath::Square(static_cast<double>(A.R) - B.R)
			+ FMath::Square(static_cast<double>(A.G) - B.G)
			+ FMath::Square(static_cast<double>(A.B) - B.B));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceStyleTest,
	"FlowViz.UI.WorkspaceStyle.Tokens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceStyleTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceStyleTest;

	const FLinearColor Panel = FlowVizWorkspaceStyle::GetPanelColor();
	const FLinearColor Raised = FlowVizWorkspaceStyle::GetPanelRaisedColor();
	const FLinearColor Background = FlowVizWorkspaceStyle::GetBackgroundColor();
	const FLinearColor Primary = FlowVizWorkspaceStyle::GetTextPrimaryColor();
	const FLinearColor Secondary = FlowVizWorkspaceStyle::GetTextSecondaryColor();

	/* == Legibility ========================================================= */
	{
		// 7:1 is WCAG AAA for body text. Primary readouts are numbers a user is
		// expected to READ OFF and quote, so AAA rather than AA is the right bar.
		const double PrimaryOnPanel = ContrastRatio(Primary, Panel);
		TestTrue(
			*FString::Printf(
				TEXT("primary text on a panel meets WCAG AAA (7:1); measured %.2f:1"),
				PrimaryOnPanel),
			PrimaryOnPanel >= 7.0);

		// Primary text also appears on a raised well - a probe row, a readout box -
		// which is a DIFFERENT background. Checking only the panel would leave the
		// surface most numbers are actually drawn on unverified.
		const double PrimaryOnRaised = ContrastRatio(Primary, Raised);
		TestTrue(
			*FString::Printf(
				TEXT("primary text on a raised well meets WCAG AAA (7:1); measured %.2f:1"),
				PrimaryOnRaised),
			PrimaryOnRaised >= 7.0);

		// 4.5:1 is WCAG AA. Secondary text is labels and units, not values.
		const double SecondaryOnPanel = ContrastRatio(Secondary, Panel);
		TestTrue(
			*FString::Printf(
				TEXT("secondary text on a panel meets WCAG AA (4.5:1); measured %.2f:1"),
				SecondaryOnPanel),
			SecondaryOnPanel >= 4.5);
	}

	/* == The chrome must not compete with the data ========================== */
	{
		// The viewport surround must be darker than the panels, so a bright volume
		// reads as emissive rather than as a lighter patch on a lighter field.
		TestTrue(
			TEXT("the viewport background is darker than the panels around it"),
			RelativeLuminance(Background) < RelativeLuminance(Panel));

		// A raised well must be distinguishable from the panel it sits in, or the
		// grouping it exists to express is invisible.
		TestTrue(
			TEXT("a raised well is lighter than the panel it sits in"),
			RelativeLuminance(Raised) > RelativeLuminance(Panel));

		// SIMULTANEOUS CONTRAST. A saturated chrome colour beside a colormap shifts
		// the apparent hue of the data. 0.06 is tight enough to exclude a
		// recognisable hue while permitting the slight cool cast that keeps a dark
		// UI from looking dead.
		const double PanelSpread = ChannelSpread(Panel);
		TestTrue(
			*FString::Printf(
				TEXT("the panel fill is near-neutral so it cannot shift the apparent colour of a "
					 "colormap next to it; channel spread %.4f"),
				PanelSpread),
			PanelSpread <= 0.06);

		const double BackgroundSpread = ChannelSpread(Background);
		TestTrue(
			*FString::Printf(
				TEXT("the viewport background is near-neutral; channel spread %.4f"),
				BackgroundSpread),
			BackgroundSpread <= 0.06);
	}

	/* == The accents encode different things and must look different ======== */
	{
		const FLinearColor Accent = FlowVizWorkspaceStyle::GetAccentColor();
		const FLinearColor Advisory = FlowVizWorkspaceStyle::GetAdvisoryColor();
		const FLinearColor Warning = FlowVizWorkspaceStyle::GetWarningColor();

		// Rule 7's interpolation badge and a refusal are different messages. If the
		// two tokens resolve close together the disclosure stops disclosing.
		TestTrue(
			TEXT("the advisory and warning accents are visibly different colours"),
			ColorDistance(Advisory, Warning) > 0.1);
		TestTrue(
			TEXT("the interactive accent is visibly different from the advisory accent"),
			ColorDistance(Accent, Advisory) > 0.1);

		// An accent is a signal; a neutral one cannot signal. This is the opposite
		// assertion to the chrome check above, and having both means a palette that
		// flattened everything to grey would fail rather than quietly pass the
		// neutrality tests.
		TestTrue(
			TEXT("the advisory accent is actually chromatic, or it cannot draw the eye"),
			ChannelSpread(Advisory) > 0.15);
		TestTrue(
			TEXT("the interactive accent is actually chromatic"),
			ChannelSpread(Accent) > 0.15);

		// Badge text is drawn IN the accent colour on a panel, so the accent has its
		// own legibility requirement. 3:1 is the WCAG non-text/large-text bar.
		const double AdvisoryOnPanel = ContrastRatio(Advisory, Panel);
		TestTrue(
			*FString::Printf(
				TEXT("advisory badge text is legible on a panel (3:1); measured %.2f:1"),
				AdvisoryOnPanel),
			AdvisoryOnPanel >= 3.0);
	}

	/* == Metrics ============================================================ */
	{
		// Every margin in the workspace is a multiple of this, so a zero or a
		// negative would collapse or invert the entire layout.
		TestTrue(
			TEXT("the base spacing unit is positive"),
			FlowVizWorkspaceStyle::GetUnit() > 0.0f);
		TestTrue(
			TEXT("the corner radius is non-negative"),
			FlowVizWorkspaceStyle::GetCornerRadius() >= 0.0f);

		// A zero font size renders nothing at all, which looks like a missing widget
		// rather than a styling error.
		TestTrue(
			TEXT("the heading font has a positive size"),
			FlowVizWorkspaceStyle::GetHeadingFont().Size > 0);
		TestTrue(
			TEXT("the numeric font has a positive size"),
			FlowVizWorkspaceStyle::GetNumericFont().Size > 0);

		// Headings must outrank body text or the hierarchy is not expressed.
		TestTrue(
			TEXT("headings are larger than labels"),
			FlowVizWorkspaceStyle::GetHeadingFont().Size > FlowVizWorkspaceStyle::GetLabelFont().Size);
	}

	/* == Brushes are real ==================================================== */
	{
		// A null brush is not a styling bug, it is a crash the first time a panel
		// paints. Each accessor is a singleton, so this also pins that they are not
		// being rebuilt per call.
		const FSlateBrush* PanelBrush = FlowVizWorkspaceStyle::GetPanelBrush();
		TestNotNull(TEXT("the panel brush exists"), PanelBrush);
		TestNotNull(TEXT("the raised brush exists"), FlowVizWorkspaceStyle::GetRaisedBrush());
		TestNotNull(TEXT("the flat brush exists"), FlowVizWorkspaceStyle::GetFlatBrush());
		TestEqual(
			TEXT("the panel brush is a stable singleton rather than a fresh allocation per call"),
			FlowVizWorkspaceStyle::GetPanelBrush(),
			PanelBrush);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
