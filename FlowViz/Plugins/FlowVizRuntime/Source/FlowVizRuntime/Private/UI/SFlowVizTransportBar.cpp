// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizTransportBar.h"

#include "CFDViz/CFDVizManifest.h"
#include "Playback/FlowVizCasePlayer.h"
#include "Styling/CoreStyle.h"
#include "UI/FlowVizTimelineViewModel.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlowVizTransportBar"

// NAMED namespace, not anonymous: this module is a unity build, so anonymous
// namespaces from sibling .cpp files merge into one and same-named helpers
// collide. See the same note on every other UI .cpp here.
namespace FlowVizTransportBarLocal
{
	/**
	 * Transport glyphs.
	 *
	 * TEXT GLYPHS RATHER THAN ICON TEXTURES, DELIBERATELY. An icon set means a
	 * Content-side asset, which cannot be constructed in a headless automation
	 * test - so the binding tests could not build the widget at all. These are the
	 * standard Unicode transport symbols, present in the engine's Roboto set, and
	 * they scale with DPI for free.
	 */
	const FText PlayGlyph = FText::FromString(TEXT("▶"));   // BLACK RIGHT-POINTING TRIANGLE
	const FText PauseGlyph = FText::FromString(TEXT("⏸"));  // DOUBLE VERTICAL BAR
	const FText FirstGlyph = FText::FromString(TEXT("⏮"));  // BLACK LEFT-POINTING DOUBLE TRIANGLE WITH BAR
	const FText LastGlyph = FText::FromString(TEXT("⏭"));   // BLACK RIGHT-POINTING DOUBLE TRIANGLE WITH BAR
	const FText StepBackGlyph = FText::FromString(TEXT("◀"));
	const FText StepForwardGlyph = FText::FromString(TEXT("▶"));

	/** Height of the transport strip, in style units. */
	constexpr float BarHeightUnits = 9.0f;

	/**
	 * Format a physical time for the readout.
	 *
	 * FIXED DECIMALS, NOT A "SMART" FORMAT. A format that drops trailing zeros
	 * changes the string's width as playback runs, so the readout jitters and the
	 * eye tracks the motion instead of the number - the same reason the numeric
	 * font is monospaced.
	 */
	FText FormatTime(double Time, const FString& Unit)
	{
		const FString Text = FString::Printf(TEXT("%.3f %s"), Time, *Unit);
		return FText::FromString(Text);
	}
}

void SFlowVizTransportBar::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._TimelineViewModel;

	const float U = FlowVizWorkspaceStyle::GetUnit();

	/**
	 * A small helper so the five transport buttons are declared once rather than
	 * five near-identical blocks. Each captures `this` only to reach the bound
	 * predicate and handler - no state is copied in.
	 */
	auto MakeTransportButton =
		[this, U](const FText& Glyph, const FText& Tooltip, FOnClicked OnClicked,
			TAttribute<bool> IsEnabled, float FontScale) -> TSharedRef<SButton>
	{
		FSlateFontInfo Font = FlowVizWorkspaceStyle::GetLabelFont();
		Font.Size = FMath::RoundToInt(Font.Size * FontScale);

		return SNew(SButton)
			.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
			.OnClicked(OnClicked)
			// RULE 15 IS ENFORCED HERE, and it is bound rather than assigned: the
			// predicate is re-read every paint, so a case closing under the widget
			// disables the button without anything having to notify it.
			.IsEnabled(IsEnabled)
			.ToolTipText(Tooltip)
			.ContentPadding(FMargin(2.0f * U, 1.0f * U))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
					.Text(Glyph)
					.Font(Font)
					.ColorAndOpacity(FSlateColor::UseForeground())
			];
	};

	const TAttribute<bool> PlayEnabled =
		TAttribute<bool>::CreateSP(this, &SFlowVizTransportBar::IsPlayEnabled);
	const TAttribute<bool> StepEnabled =
		TAttribute<bool>::CreateSP(this, &SFlowVizTransportBar::IsStepEnabled);
	const TAttribute<bool> ScrubEnabled =
		TAttribute<bool>::CreateSP(this, &SFlowVizTransportBar::IsScrubEnabled);

	FirstFrameButton = MakeTransportButton(
		FlowVizTransportBarLocal::FirstGlyph,
		LOCTEXT("FirstFrameTip", "Jump to the first stored frame"),
		FOnClicked::CreateSP(this, &SFlowVizTransportBar::OnFirstFrameClicked),
		StepEnabled, 1.0f);

	StepBackwardButton = MakeTransportButton(
		FlowVizTransportBarLocal::StepBackGlyph,
		LOCTEXT("StepBackTip", "Step back one stored frame (pauses playback)"),
		FOnClicked::CreateSP(this, &SFlowVizTransportBar::OnStepBackwardClicked),
		StepEnabled, 1.0f);

	PlayPauseButton = MakeTransportButton(
		// This fixed glyph is immediately replaced below by a BOUND one. The label
		// must follow the view model's play state, because playback can stop on its
		// own at the end of a non-looping case and a fixed glyph would then lie.
		FlowVizTransportBarLocal::PlayGlyph,
		LOCTEXT("PlayPauseTip", "Play or pause"),
		FOnClicked::CreateSP(this, &SFlowVizTransportBar::OnPlayPauseClicked),
		PlayEnabled, 1.35f);

	StepForwardButton = MakeTransportButton(
		FlowVizTransportBarLocal::StepForwardGlyph,
		LOCTEXT("StepForwardTip", "Step forward one stored frame (pauses playback)"),
		FOnClicked::CreateSP(this, &SFlowVizTransportBar::OnStepForwardClicked),
		StepEnabled, 1.0f);

	LastFrameButton = MakeTransportButton(
		FlowVizTransportBarLocal::LastGlyph,
		LOCTEXT("LastFrameTip", "Jump to the last stored frame"),
		FOnClicked::CreateSP(this, &SFlowVizTransportBar::OnLastFrameClicked),
		StepEnabled, 1.0f);

	// The play/pause button's label has to be rebuilt with a bound attribute,
	// since the helper above takes a fixed FText. Done by replacing its content
	// rather than by widening the helper for one caller.
	{
		FSlateFontInfo PlayFont = FlowVizWorkspaceStyle::GetLabelFont();
		PlayFont.Size = FMath::RoundToInt(PlayFont.Size * 1.35f);

		PlayPauseButton->SetContent(
			SNew(STextBlock)
				.Text(TAttribute<FText>::CreateSP(this, &SFlowVizTransportBar::GetPlayPauseLabel))
				.Font(PlayFont)
				.ColorAndOpacity(FSlateColor::UseForeground()));
	}

	/* --- The options cluster (#75) ----------------------------------------- */
	/*
	 * Loop, playback mode, speed and interpolation. Five view model setters
	 * with no production caller until these controls existed -- the welded
	 * defaults defect (#74), one panel over. Labels name the STATE IN FORCE
	 * rather than the action, matching the slice panel's toggles: a button
	 * reading "Loop" while the case pingpongs is a lie about which control the
	 * user is holding.
	 */

	const TAttribute<bool> OptionsEnabled = TAttribute<bool>::CreateLambda(
		[this]() { return ViewModel != nullptr && ViewModel->HasFrames(); });

	auto MakeOptionButton = [this, U, &OptionsEnabled](TAttribute<FText> Label,
							   const FText& Tooltip, FOnClicked OnClicked) -> TSharedRef<SButton>
	{
		return SNew(SButton)
			.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
			.OnClicked(OnClicked)
			.IsEnabled(OptionsEnabled)
			.ToolTipText(Tooltip)
			.ContentPadding(FMargin(1.5f * U, 0.75f * U))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
					.Text(Label)
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			];
	};

	LoopModeButton = MakeOptionButton(
		TAttribute<FText>::CreateSP(this, &SFlowVizTransportBar::GetLoopModeLabel),
		LOCTEXT("LoopModeTip",
			"How playback treats the last frame: loop to the first, ping-pong back, or stop. "
			"Click to cycle."),
		FOnClicked::CreateSP(this, &SFlowVizTransportBar::OnLoopModeClicked));

	PlaybackModeButton = MakeOptionButton(
		TAttribute<FText>::CreateSP(this, &SFlowVizTransportBar::GetPlaybackModeLabel),
		LOCTEXT("PlaybackModeTip",
			"Sequence shows every stored frame in order; real-time advances by the case's own "
			"physical clock, skipping or holding frames as needed."),
		FOnClicked::CreateSP(this, &SFlowVizTransportBar::OnPlaybackModeClicked));

	InterpolationButton = MakeOptionButton(
		TAttribute<FText>::CreateSP(this, &SFlowVizTransportBar::GetInterpolationLabel),
		LOCTEXT("InterpolationTip",
			"Blend between stored frames, or hold each one. Blending is smooth and shows "
			"values the solver never computed; the badge discloses which is on screen."),
		FOnClicked::CreateSP(this, &SFlowVizTransportBar::OnInterpolationClicked));

	TSharedRef<SHorizontalBox> SpeedCluster = SNew(SHorizontalBox);
	for (int32 Index = 0; Index < FlowVizPlayback::NumSpeedPresets; ++Index)
	{
		TSharedPtr<SButton> PresetButton;
		SAssignNew(PresetButton, SButton)
			.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
			.OnClicked(
				FOnClicked::CreateSP(this, &SFlowVizTransportBar::OnSpeedPresetClicked, Index))
			.IsEnabled(OptionsEnabled)
			.ToolTipText(LOCTEXT("SpeedPresetTip", "Playback speed multiplier."))
			.ContentPadding(FMargin(1.0f * U, 0.75f * U))
			[
				SNew(STextBlock)
					.Text(FText::FromString(
						FString::Printf(TEXT("%gx"), FlowVizPlayback::SpeedPresets[Index])))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			];
		SpeedPresetButtons.Add(PresetButton);
		SpeedCluster->AddSlot()
			.AutoWidth()
			.Padding(FMargin(Index == 0 ? 0.0f : 0.25f * U, 0.0f, 0.0f, 0.0f))
			[
				PresetButton.ToSharedRef()
			];
	}

	SAssignNew(CustomSpeedBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(this, &SFlowVizTransportBar::GetCustomSpeedText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizTransportBar::OnCustomSpeedCommitted))
		.IsEnabled(OptionsEnabled)
		.ToolTipText(LOCTEXT("CustomSpeedTip",
			"Any speed multiplier. Zero is refused - a playing case at speed zero is "
			"indistinguishable from a paused one on screen and differs in every diagnostic."));

	ChildSlot
	[
		SNew(SBorder)
			.BorderImage(FlowVizWorkspaceStyle::GetFlatBrush())
			.BorderBackgroundColor(FlowVizWorkspaceStyle::GetPanelColor())
			.Padding(FMargin(2.0f * U, 1.5f * U))
			.VAlign(VAlign_Center)
		[
			SNew(SHorizontalBox)

			/* --- Transport cluster ---------------------------------------- */

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
			[
				FirstFrameButton.ToSharedRef()
			]
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
			[
				StepBackwardButton.ToSharedRef()
			]
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
			[
				PlayPauseButton.ToSharedRef()
			]
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
			[
				StepForwardButton.ToSharedRef()
			]
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 2.0f * U, 0.0f))
			[
				LastFrameButton.ToSharedRef()
			]

			/* --- Frame counter -------------------------------------------- */

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 2.0f * U, 0.0f))
			[
				SNew(STextBlock)
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizTransportBar::GetFrameCounterText))
					// Monospaced: see FlowVizWorkspaceStyle::GetNumericFont.
					.Font(FlowVizWorkspaceStyle::GetNumericFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
			]

			/* --- Scrubber. Takes all remaining width. --------------------- */

			+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 2.0f * U, 0.0f))
			[
				SAssignNew(ScrubSlider, SFlowVizScrubSlider)
					// BOUND, not assigned. Playback moves the playhead on its own,
					// and a session load can move it too; a slider holding its own
					// float would sit still while the case ran.
					.Value(TAttribute<float>::CreateSP(
						this, &SFlowVizTransportBar::GetScrubValue))
					.OnValueChanged(FOnFloatValueChanged::CreateSP(
						this, &SFlowVizTransportBar::OnScrubValueChanged))
					.IsEnabled(ScrubEnabled)
					.SliderBarColor(FSlateColor(FlowVizWorkspaceStyle::GetPanelRaisedColor()))
					.SliderHandleColor(FSlateColor(FlowVizWorkspaceStyle::GetAccentColor()))
					.ToolTipText(LOCTEXT("ScrubTip", "Scrub through physical time"))
			]

			/* --- Time readout --------------------------------------------- */

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 2.0f * U, 0.0f))
			[
				SNew(STextBlock)
					.Text(TAttribute<FText>::CreateSP(this, &SFlowVizTransportBar::GetTimeText))
					.Font(FlowVizWorkspaceStyle::GetNumericFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextPrimaryColor()))
			]

			/* --- Fidelity badge. Engineering rule 7's disclosure. ---------- */

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
			[
				SNew(SBorder)
					.BorderImage(FlowVizWorkspaceStyle::GetRaisedBrush())
					.Padding(FMargin(1.5f * U, 0.5f * U))
					.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
						.Text(TAttribute<FText>::CreateSP(
							this, &SFlowVizTransportBar::GetBadgeText))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						// The COLOUR is bound too, so an interpolated frame is amber
						// and a stale hold is rose without the widget caching which
						// it is.
						.ColorAndOpacity(TAttribute<FSlateColor>::CreateSP(
							this, &SFlowVizTransportBar::GetBadgeColor))
						.ToolTipText(LOCTEXT("BadgeTip",
							"What is actually on screen: an exact stored frame, a blend of two "
							"frames, or an older frame held while the requested one loads."))
				]
			]
		]
	];

	/*
	 * THE OPTIONS ROW, BELOW THE TRANSPORT ROW. Wrapped after the fact rather
	 * than restructuring the declarative block above: the transport row's slots
	 * are position-sensitive (tests reach them by getter, not index) and the
	 * whole bar is one bordered strip either way.
	 */
	{
		const TSharedRef<SWidget> TransportRow = ChildSlot.GetWidget();
		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight() [ TransportRow ]
			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 0.5f * U, 0.0f, 0.0f))
			[
				SNew(SBorder)
					.BorderImage(FlowVizWorkspaceStyle::GetFlatBrush())
					.BorderBackgroundColor(FlowVizWorkspaceStyle::GetPanelColor())
					.Padding(FMargin(2.0f * U, 1.0f * U))
					.VAlign(VAlign_Center)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
					[
						LoopModeButton.ToSharedRef()
					]
					+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.0f, 0.0f, 2.0f * U, 0.0f))
					[
						PlaybackModeButton.ToSharedRef()
					]
					+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
					[
						SpeedCluster
					]
					+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.0f, 0.0f, 2.0f * U, 0.0f))
					[
						SNew(SBox).MinDesiredWidth(14.0f * U) [ CustomSpeedBox.ToSharedRef() ]
					]
					+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
					[
						InterpolationButton.ToSharedRef()
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f) [ SNew(SBox) ]
				]
			]
		];
	}
}

/* ========================================================================== */
/* Bound reads                                                                 */
/* ========================================================================== */

bool SFlowVizTransportBar::IsPlayEnabled() const
{
	// Every read here answers "no" on a null view model rather than asserting.
	// The workspace builds this bar before a case exists, and a bar that crashed
	// at startup would be worse than one that is briefly inert.
	return ViewModel != nullptr && ViewModel->CanPlay();
}

bool SFlowVizTransportBar::IsStepEnabled() const
{
	return ViewModel != nullptr && ViewModel->CanStep();
}

bool SFlowVizTransportBar::IsScrubEnabled() const
{
	return ViewModel != nullptr && ViewModel->CanScrub();
}

float SFlowVizTransportBar::GetScrubValue() const
{
	if (ViewModel == nullptr)
	{
		return 0.0f;
	}
	return static_cast<float>(ViewModel->GetNormalizedTime());
}

FText SFlowVizTransportBar::GetPlayPauseLabel() const
{
	const bool bPlaying = ViewModel != nullptr && ViewModel->IsPlaying();
	return bPlaying ? FlowVizTransportBarLocal::PauseGlyph : FlowVizTransportBarLocal::PlayGlyph;
}

FText SFlowVizTransportBar::GetFrameCounterText() const
{
	if (ViewModel == nullptr || !ViewModel->HasFrames())
	{
		return LOCTEXT("NoFrames", "-- / --");
	}

	const int32 Displayed = ViewModel->GetDisplayedFrame();
	const int32 Count = ViewModel->GetFrameCount();

	if (Displayed == INDEX_NONE)
	{
		// Nothing complete is resident. Naming a frame here would claim the user
		// is looking at data that has not finished decoding.
		return FText::FromString(FString::Printf(TEXT("-- / %d"), Count));
	}

	// One-based for display, zero-based internally. Users count frames from 1.
	return FText::FromString(FString::Printf(TEXT("%d / %d"), Displayed + 1, Count));
}

FText SFlowVizTransportBar::GetTimeText() const
{
	if (ViewModel == nullptr || !ViewModel->HasFrames())
	{
		return LOCTEXT("NoTime", "--");
	}

	FString Unit = TEXT("s");
	if (const FFlowVizCasePlayer* Player = ViewModel->GetPlayer())
	{
		if (const FCFDVizCase* Case = Player->GetCase())
		{
			if (!Case->Units.Time.IsEmpty())
			{
				// The CASE's time unit, never an assumed "s". A case in
				// milliseconds labelled seconds is a three-orders-of-magnitude
				// misreading that looks entirely plausible (engineering rule 4).
				Unit = Case->Units.Time;
			}
		}
	}

	// GetDisplayedTime, not GetPhysicalTime: during a stall the playhead has moved
	// on but the pixels have not, and the readout must describe the PIXELS. The
	// badge says HeldStale at the same moment, so the two agree.
	return FlowVizTransportBarLocal::FormatTime(ViewModel->GetDisplayedTime(), Unit);
}

FText SFlowVizTransportBar::GetBadgeText() const
{
	const EFlowVizFrameBadge Badge =
		ViewModel != nullptr ? ViewModel->GetBadge() : EFlowVizFrameBadge::NoCase;

	switch (Badge)
	{
	case EFlowVizFrameBadge::NoCase:
		return LOCTEXT("BadgeNoCase", "NO CASE");
	case EFlowVizFrameBadge::NoData:
		return LOCTEXT("BadgeNoData", "LOADING");
	case EFlowVizFrameBadge::Exact:
		return LOCTEXT("BadgeExact", "EXACT");
	case EFlowVizFrameBadge::Interpolated:
		// THE WORD, not a symbol. Rule 7 requires interpolation to be VISIBLY
		// IDENTIFIED; a coloured dot that the user must learn is not identification.
		return LOCTEXT("BadgeInterpolated", "INTERPOLATED");
	case EFlowVizFrameBadge::HeldStale:
		return LOCTEXT("BadgeHeld", "HELD (STALE)");
	default:
		// A new enumerator must not silently render as blank - a blank badge reads
		// as "nothing to disclose", which is the failure rule 7 forbids.
		return LOCTEXT("BadgeUnknown", "UNKNOWN");
	}
}

FSlateColor SFlowVizTransportBar::GetBadgeColor() const
{
	const EFlowVizFrameBadge Badge =
		ViewModel != nullptr ? ViewModel->GetBadge() : EFlowVizFrameBadge::NoCase;

	switch (Badge)
	{
	case EFlowVizFrameBadge::Exact:
		// Exact data is the unremarkable case and gets no accent at all. Colouring
		// it too would make the advisory states less salient by comparison.
		return FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor());
	case EFlowVizFrameBadge::Interpolated:
		return FSlateColor(FlowVizWorkspaceStyle::GetAdvisoryColor());
	case EFlowVizFrameBadge::HeldStale:
		// Stronger than interpolated: the displayed TIME is not the requested one.
		return FSlateColor(FlowVizWorkspaceStyle::GetWarningColor());
	case EFlowVizFrameBadge::NoData:
		return FSlateColor(FlowVizWorkspaceStyle::GetAdvisoryColor());
	case EFlowVizFrameBadge::NoCase:
	default:
		return FSlateColor(FlowVizWorkspaceStyle::GetTextDisabledColor());
	}
}

/* ========================================================================== */
/* Actions                                                                     */
/* ========================================================================== */

/* --- The options cluster's handlers and labels (#75) ---------------------- */

FReply SFlowVizTransportBar::OnLoopModeClicked()
{
	if (ViewModel != nullptr)
	{
		// CYCLE, not toggle: three states, one button. The order is the enum's
		// own, so the label (which names the state in force) and the cycle agree.
		EFlowVizLoopMode Next = EFlowVizLoopMode::Loop;
		switch (ViewModel->GetLoopMode())
		{
			case EFlowVizLoopMode::Loop: Next = EFlowVizLoopMode::PingPong; break;
			case EFlowVizLoopMode::PingPong: Next = EFlowVizLoopMode::Once; break;
			case EFlowVizLoopMode::Once: Next = EFlowVizLoopMode::Loop; break;
		}
		ViewModel->SetLoopMode(Next);
	}
	return FReply::Handled();
}

FReply SFlowVizTransportBar::OnPlaybackModeClicked()
{
	if (ViewModel != nullptr)
	{
		const EFlowVizPlaybackMode Next =
			ViewModel->GetPlaybackMode() == EFlowVizPlaybackMode::Sequence
				? EFlowVizPlaybackMode::RealTime
				: EFlowVizPlaybackMode::Sequence;
		ViewModel->SetPlaybackMode(Next);
	}
	return FReply::Handled();
}

FReply SFlowVizTransportBar::OnSpeedPresetClicked(int32 PresetIndex)
{
	if (ViewModel != nullptr)
	{
		// The refusal (out-of-range index) keeps the prior speed; nothing to
		// report from a click that a bounds-checked button list cannot produce.
		ViewModel->SetSpeedPresetIndex(PresetIndex);
	}
	return FReply::Handled();
}

void SFlowVizTransportBar::OnCustomSpeedCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	if (ViewModel == nullptr)
	{
		return;
	}
	double Parsed = 0.0;
	if (!LexTryParseString(Parsed, *NewText.ToString()))
	{
		// UNPARSEABLE IS IGNORED, NOT COERCED: the bound attribute redisplays
		// the real speed on the next paint, so the box visibly snaps back.
		return;
	}
	// Zero and non-finite are refused by the MODEL (SetCustomSpeed), keeping
	// the prior speed -- same one-refusal-path rule as every numeric entry.
	ViewModel->SetCustomSpeed(Parsed);
}

FReply SFlowVizTransportBar::OnInterpolationClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->SetInterpolationEnabled(!ViewModel->IsInterpolationEnabled());
	}
	return FReply::Handled();
}

FText SFlowVizTransportBar::GetLoopModeLabel() const
{
	if (ViewModel == nullptr)
	{
		return LOCTEXT("LoopUnavailable", "Loop");
	}
	switch (ViewModel->GetLoopMode())
	{
		case EFlowVizLoopMode::PingPong: return LOCTEXT("LoopPingPong", "Ping-pong");
		case EFlowVizLoopMode::Once: return LOCTEXT("LoopOnce", "Play once");
		default: return LOCTEXT("LoopLoop", "Loop");
	}
}

FText SFlowVizTransportBar::GetPlaybackModeLabel() const
{
	if (ViewModel == nullptr)
	{
		return LOCTEXT("ModeUnavailable", "Mode");
	}
	return ViewModel->GetPlaybackMode() == EFlowVizPlaybackMode::RealTime
		? LOCTEXT("ModeRealTime", "Real-time")
		: LOCTEXT("ModeSequence", "Sequence");
}

FText SFlowVizTransportBar::GetInterpolationLabel() const
{
	if (ViewModel == nullptr)
	{
		return LOCTEXT("InterpUnavailable", "Blend");
	}
	return ViewModel->IsInterpolationEnabled() ? LOCTEXT("InterpOn", "Blended")
											   : LOCTEXT("InterpOff", "Stored frames");
}

FText SFlowVizTransportBar::GetCustomSpeedText() const
{
	if (ViewModel == nullptr)
	{
		return FText::GetEmpty();
	}
	return FText::FromString(FString::Printf(TEXT("%g"), ViewModel->GetSpeed()));
}

FReply SFlowVizTransportBar::OnPlayPauseClicked()
{
	if (ViewModel != nullptr)
	{
		// TogglePlayPause, not a local bIsPlaying flipped by hand. The view model
		// already knows which way the toggle goes, and a widget-side flag would
		// invert itself the first time playback ended on its own.
		ViewModel->TogglePlayPause();
	}
	return FReply::Handled();
}

FReply SFlowVizTransportBar::OnStepForwardClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->StepForward();
	}
	return FReply::Handled();
}

FReply SFlowVizTransportBar::OnStepBackwardClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->StepBackward();
	}
	return FReply::Handled();
}

FReply SFlowVizTransportBar::OnFirstFrameClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->GoToFirstFrame();
	}
	return FReply::Handled();
}

FReply SFlowVizTransportBar::OnLastFrameClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->GoToLastFrame();
	}
	return FReply::Handled();
}

void SFlowVizTransportBar::OnScrubValueChanged(float NewValue)
{
	if (ViewModel != nullptr)
	{
		ViewModel->ScrubToNormalized(static_cast<double>(NewValue));
	}
}

TSharedPtr<SButton> SFlowVizTransportBar::GetSpeedPresetButton(int32 Index) const
{
	return SpeedPresetButtons.IsValidIndex(Index) ? SpeedPresetButtons[Index] : nullptr;
}

#undef LOCTEXT_NAMESPACE
