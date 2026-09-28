---
name: "LBM Solver Studio — Staged Recorded-Flow Controls"
description: "Accumulating scoped record of D05–D08 streamline/vector controls, V10/D01 viewport toolbar/triangle mesh display, and D11/S01 Solve inspector foundation within the incumbent Operate workspace."
colors:
  background: "#07121D"
  panel: "#101E2B"
  raised: "#182735"
  line: "#304151"
  text: "#E0E9F3"
  muted: "#99ACBF"
  cyan: "#00C8EC"
  blue: "#2980FF"
  green: "#43D981"
  amber: "#F0B74B"
  snapshot-amber: "#F5BD59"
  button-hover: "color(srgb-linear 0.035 0.10 0.16)"
  button-pressed: "color(srgb-linear 0.01 0.055 0.09)"
typography:
  title:
    fontFamily: "Unreal CoreStyle"
    fontSize: "12pt"
    fontWeight: 700
  body:
    fontFamily: "Unreal CoreStyle"
    fontSize: "10pt"
    fontWeight: 400
  section:
    fontFamily: "Unreal CoreStyle"
    fontSize: "10pt"
    fontWeight: 700
  label:
    fontFamily: "Unreal CoreStyle"
    fontSize: "9pt"
    fontWeight: 400
  seed-kind:
    fontFamily: "Unreal CoreStyle"
    fontSize: "11pt"
    fontWeight: 700
  micro:
    fontFamily: "Unreal CoreStyle"
    fontSize: "8pt"
    fontWeight: 400
rounded:
  button: "4px"
  input: "3px"
spacing:
  mode-gap: "4px"
  tight: "5px"
  label-gap: "6px"
  control-gap: "7px"
  field-gap: "8px"
  recovery-gap: "10px"
  key-gap: "12px"
  section-gap: "14px"
  menu-inset: "16px"
components:
  vector-settings-button:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.muted}"
    rounded: "{rounded.button}"
    padding: "3px 5px"
  vector-mode:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.text}"
    typography: "{typography.body}"
    rounded: "{rounded.button}"
    padding: "7px"
  vector-mode-selected:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.cyan}"
    typography: "{typography.body}"
    rounded: "{rounded.button}"
    padding: "7px"
  vector-number:
    backgroundColor: "{colors.background}"
    textColor: "{colors.text}"
    typography: "{typography.body}"
    rounded: "{rounded.input}"
    padding: "5px 7px"
    width: "130px"
  vector-menu:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text}"
    width: "320px"
    padding: "16px"
  vector-key:
    textColor: "{colors.text}"
    typography: "{typography.body}"
  vector-recovery:
    textColor: "{colors.amber}"
    typography: "{typography.label}"
  navigation-active:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.cyan}"
    typography: "{typography.label}"
    height: "45px"
    padding: "0px 14px"
  presented-status:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.green}"
    typography: "{typography.label}"
  streamline-settings-button:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.muted}"
    rounded: "{rounded.button}"
    padding: "3px 5px"
  streamline-seed-mode:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.text}"
    typography: "{typography.body}"
    rounded: "{rounded.button}"
    padding: "7px 6px"
  streamline-seed-mode-selected:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.cyan}"
    typography: "{typography.body}"
    rounded: "{rounded.button}"
    padding: "7px 6px"
  streamline-direction:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.text}"
    typography: "{typography.body}"
    rounded: "{rounded.button}"
    padding: "7px 5px"
  streamline-number:
    backgroundColor: "{colors.background}"
    textColor: "{colors.text}"
    typography: "{typography.body}"
    rounded: "{rounded.input}"
    padding: "5px 7px"
    width: "145px"
  streamline-menu:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text}"
    width: "350px"
    padding: "16px"
  seed-number:
    backgroundColor: "{colors.background}"
    textColor: "{colors.text}"
    typography: "{typography.body}"
    rounded: "{rounded.input}"
    padding: "5px 7px"
    width: "110px"
  seed-face:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.text}"
    typography: "{typography.label}"
    rounded: "{rounded.button}"
    padding: "7px 4px"
  seed-row-selected:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.cyan}"
    typography: "{typography.body}"
    rounded: "{rounded.button}"
    padding: "5px 6px"
  seed-inspector:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text}"
    padding: "10px"
  seed-aggregate:
    textColor: "{colors.text}"
    typography: "{typography.body}"
  snapshot-streamline-warning:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.snapshot-amber}"
  viewport-toolbar:
    backgroundColor: "{colors.panel}"
    padding: "5px"
  viewport-toolbar-button:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.text}"
    typography: "{typography.label}"
    rounded: "{rounded.button}"
    padding: "4px 8px"
  triangle-mesh-menu:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text}"
    width: "322px"
    padding: "14px"
  viewport-camera-menu:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text}"
    width: "340px"
    padding: "14px"
  solve-inspector:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text}"
    padding: "0px 10px"
    width: "322px"
  solve-inspector-category:
    textColor: "{colors.muted}"
    typography: "{typography.body}"
    padding: "12px 2px"
  solve-inspector-category-selected:
    textColor: "{colors.cyan}"
    typography: "{typography.body}"
    padding: "12px 2px"
---

# Design System: LBM Solver Studio — Staged Recorded-Flow Controls

## Overview

**Creative North Star: "Operate"**

The established “Operate” world is a dense native desktop workspace: blue-black surfaces, fine borders, compact controls, and a dominant CFD scene. D08 adds vector sampling and length controls; D05–D07 adds instantaneous recorded-velocity tracing, exact limits, and saved seed sets inside that world. Descriptive language and direction carry forward the user’s pinned appearance.

This is an accumulating scoped design record for `tmp/inspection-workspace-20260927`: D05–D08 streamline controls, saved seed editors and vectors, followed by V10/D01 viewport toolbar and actual triangle mesh display, and the D11/S01 Solve inspector category/ownership foundation. Each accepted feature retains its own evidence and limits. Main `DESIGN.md` remains the visual authority. Shared inspection infrastructure is described only where it serves these slices; other inspection features, the full application, and the wider renderer remain outside this record’s acceptance.

The six-line contract beside `SStudioWorkspace::VectorControls` makes density and length interpretable against the displayed CFD frame: sample limit, proportional or equal length, exact scale, and a current-frame key. Project/view history owns settings; captured geometry owns the key. Main `PRODUCT.md` and `DESIGN.md` were read as context only.

The D08 initial finish review and same-reviewer verdict live at `../implementation/vector-controls-finish-review.md` and `../implementation/vector-controls-finish-verdict.md`. The latter resolves **F1 only** with disposition **ship** for staged package `2751570ac6ad32c1877d431c68e33bd65b916e5acef234d766182cdfafcaaad8`. Its evidence is the 14 replacement captures in `.impeccable/review/vector-controls/` and two recorded five-suite workflows at 1320 × 740 and 1280 × 720, each with five successes, zero warnings/failures, exit 0, and no remaining owned processes. The prior D08 documentation pass inspected source, reports, and all 14 captures; it launched and tested nothing. The earlier 131-test model result predates the final UI-only correction and is not a fresh final-package model result.

The D05–D07 six-line contract immediately before `SStudioWorkspace::CanShowStreamlines` places one settings action beside Streamlines and owns saved seeds through the shared Inspection panel, scene placement, and view history. This is a local code-led extension in native macOS UE5 Slate using genuine published recorded fields. It retains unrestricted camera exploration and the sidebar as the sole persistent workspace navigator. No generated comp or new visual workshop applies.

The D05–D07 packet, initial review, and same-reviewer correction verdict are `../implementation/streamline-review-packet.md`, `../implementation/streamline-controls-finish-review.md`, and `../implementation/streamline-controls-finish-verdict.md`. The verdict is **ship for F1 and F2 only**, both resolved, on package `afedca302bc1061d9a7385ad6a1a99c0c997ae4766ba5582662d0fa266feee7c`. F1 adds visible derived-trace interpretation and frozen export work-limit text; F2 labels the shared aggregate “All seed sets:”. This correction verdict is not a fresh full-surface review. The D08 disposition and package above remain a separate historical scope.

Recorded verification is `tmp/debug/streamline-ui-verification-20260928.json`: 221 matching source/tool/config hashes and 12 clean packaged cases, comprising one Streamlines case and five FieldDisplay cases at each accepted size. All four manifests pin the corrected package, exit 0, and record no remaining owned processes. The model run reports 135 clean successes plus one success with Unreal’s bundled iOS `idevice_id` architecture-helper warning, zero failures/not-run. It precedes presentation-only formatting, header allocation, and provenance/annotation changes; backend/schema hashes are unchanged, and no fresh final-package model run is claimed.

This merge inspected relevant native source, recorded results, and all 26 captures at `.impeccable/review/streamlines-{1320x740,1280x720}-{settings,invalid,budget,saved-empty,inlet,line,line-draft,plane,points,original-unavailable,surface,volume,snapshot}.png`. Settings, invalid, and budget include an incidental settings-button hover tooltip; the F1/F2 scoring captures are unobstructed. Both snapshots intentionally export at 1280 × 720 without application chrome. This documenter launched no runtime and performed no build or test run.

Main integration, physical OS input, full accessibility, current integrated long-session acceptance, overall renderer/reference fidelity, and the rest of the full UI plan remain open. Protected main/schema-11 and scheduled-volume boundaries remain in force. Routed Slate events establish the supplied control workflow; the small authentic data fixtures do not establish full-source transport duration or extended stability. No browser/HTML detector applies to this native UI.

The V10/D01 direction contract immediately above `SStudioWorkspace::ViewToolbar` adds one compact owner for view direction, projection, topology and framing below the input hint. It inherits the dense blue-black Slate world, cyan selection and real recorded CFD. Views, Fit, and camera transform/lens/depth controls move to this owner; their previous visible copies are removed. The left Camera manager still manages saved views. The sidebar remains the sole workspace navigator, with no duplicate top workflow tabs, following the user’s explicit correction to the supplied reference.

`../implementation/viewport-toolbar-finish-review.md` gives **ship**, with no material fixes, for staged V10/D01 package `866d37a998d5242e080546f74e507a194b32d06d47cfc3f0f0f229d0c3419655`. The recorded verification at `../debug/viewport-toolbar-verification-20260928.json` pins 237 source/config/tool/material hashes and 24 captures. It records 139 successful model cases: 138 clean and one with the bundled `idevice_id` wrong-CPU helper warning. Five native manifests record 15 clean suites: one toolbar and five clipping suites per window size, plus three orientation suites at 1280 × 720. All owned native apps exited 0 with empty remaining-process inventories.

The finish reviewer opened all 24 `.impeccable/review/viewport-toolbar-{1320x740,1280x720}-{field,overlay,edges,derived-overlay,expanded,camera-settings,mesh-menu,no-topology,views,projection,mesh-snapshot,clipping-invalid}.png` captures and found them valid. The 22 window captures have the stated dimensions; the two mesh snapshots intentionally export at 960 × 540. `tmp/debug/viewport-toolbar-png-audit.json` independently records valid PNG chunk CRCs and frozen mesh metadata for both exports. This documentation pass read the implementation and recorded evidence and opened four representative captures; it ran no Unreal runtime, build or test.

V10/D01 acceptance covers routed Slate/native GPU behavior at those two sizes. It does not establish physical OS pointer/keyboard acceptance, long-session stability, full accessibility, the entire UI plan, or full studio/reference fidelity. Main integration waits for the existing volume/stability gate and fresh integration guards. The protected main `56a73127…` and scheduled-volume `516efec6…` executables remain unchanged by this staged work. At that V10/D01 verdict, D11/S01 inspector consolidation remained open; its later scoped foundation acceptance is recorded below. Full editable M3 authoring and M3–M5 completion remain open. Earlier inspection/camera acceptance remains in the canonical record with its original boundaries.

The D11/S01 six-line contract immediately above `SStudioWorkspace::CachedDisplayMenu` gives each Solve setting one visible owner without losing an unfinished edit. Setup, Physics, BCs and Display occupy the existing right inspector; the category tabs are local to Solve. The incumbent Operate palette, compact native macOS UE5 Slate controls, authentic recorded fields and sidebar-only workspace navigation remain the visual authority. This is a local code-led extension with no generated comp or new identity.

`../implementation/solve-inspector-finish-review.md` gives **ship**, with no material fixes, only for the staged D11/S01 category/ownership foundation, captured retained drafts, and supplied persistence/isolation evidence on client `92bfe98b417dfcb4c56a31d777b7b5b2cadb217314f281b2a631f58adf0fbc2e`, project schema 16. The packet is `../implementation/solve-inspector-review-packet.md`. Its early pending language and verification status predate the review; the finish review supplies the disposition. This acceptance adds to the prior D05–D08 and V10/D01 records without widening their verdicts.

`../debug/solve-inspector-verification-20260928.json` records 240 matching staged source/config/tool/material hashes and 14 clean final-client native cases in six manifests: one Inspector case at each of 1320 × 740 and 1280 × 720, plus five Colors, five FieldDisplay, one Streamlines and one ViewportToolbar case at 1280 × 720. All six report exit 0, zero warnings/errors/not-run cases and empty owned-process inventories. The model report records **140 clean successes** at `2026.09.28-05.51.31`, including `Studio.Inspector.SessionCategoryIsolation`, before the final retained-scroll UI correction and native-test formatting correction. Numerical model and persistence changes were unchanged afterward. This is not a fresh 140-case model run on the final client; the six native manifests cover that client.

The reviewer opened and accepted all 16 whole-window `.impeccable/review/solve-inspector-{1320x740,1280x720}-{display,setup,physics,boundaries,invalid,retained-draft,inspection,streamline-draft}.png` captures, eight at each stated size. They decode, match their recorded hashes and show the named states. This documentation pass opened eight representative captures spanning all eight states, validated all 16 capture hashes/dimensions and checked the source/evidence references. It ran no build, test, runtime, native control or context regeneration.

Four additional final-client regressions remain pending behind the protected volume run: `Studio.SurfaceControls.`, `Studio.VolumeControls.`, `Studio.Rendering.ExternalRecordingControlsAndBounds`, and `Studio.Rendering.JobControlsAndCamera`. Main integration still requires the existing volume/stability gate and fresh guards. Editable M3 Physics/BC authoring, physical OS input, full accessibility, extended integrated stability, full reference fidelity and the wider M1–M5 UI goal remain open. This documentation does not accept those workflows or the whole UI, and does not touch main production files or the protected volume workspace.

**Key Characteristics:**

- One settings action beside Vectors in the existing Display panel.
- Exact sample and scale inputs with reversible view edits.
- Cyan selection and focus, amber inline recovery, flat dark surfaces.
- Length meaning belongs to captured geometry and remains separate from scalar color.
- Sidebar-only workspace navigation remains the incumbent context.
- One Streamlines settings action controls exact physical dimensions, seed mode, direction, and bounded tracing.
- Saved seeds share the source-bound inspector, atomic scene placement, and reversible view history.
- Visible trace interpretation and work limits survive annotated export from frozen state.
- One bottom toolbar owns direction, projection, actual topology, framing and camera settings.
- Original, verified-derived and unavailable topology retain explicit meaning in the live legend and frozen export.
- Expand/Restore is a saved session layout preference; active inspection and placement remain available.
- Setup, Physics, BCs and Display have one local right-inspector owner with a session-only category preference.
- Retained vector/streamline drafts stay with their project/source/scalar identity; source physics and saved case summaries remain explicit.

## Colors

The frontmatter preserves the actual source sRGB hex values from `StudioUI`, plus the two authored linear button-state colors using CSS’s explicit linear color space. These are UI colors; scientific field palettes are separate.

### Primary

- **Cyan:** selected length-mode text, focused input outline, hover/pressed button outlines, and active workspace text.
- **Blue:** the scale slider handle and automatic inlet density handle.
- Streamline seed-mode and direction selections, chosen inlet-face text, selected seed rows, and saved seed markers reuse **Cyan**.

### Secondary

- **Amber:** wrapped numeric recovery, source limitations, live placement drafts, and derived-streamline provenance.
- **Snapshot amber:** the exported work-limit annotation uses this distinct authored color. Preserve the difference from live UI Amber.
- **Green:** the incumbent replay-state badge text, providing context alongside captured source/frame information.

### Neutral

- **Background / Panel / Raised:** input and application ground, local utility containers and popover, and resting controls respectively.
- **Line:** one-unit control outlines and slider track; **Muted:** labels, caveats, counts, and the compact panel key; **Text:** exact values, headings, and the popover key.
- **Button hover / Button pressed:** native linear blue surface states. They do not introduce a new palette direction.

**The Independent Encodings Rule.** Vector color follows the selected scalar; vector direction and proportional length follow velocity. Selecting Pressure does not turn arrow length into pressure.

Sidecar tonal ramps are generated preview metadata, not additional native colors or scientific palettes.

V10/D01 reuses Panel, Raised, Text, Muted and the shared cyan hover/focus outlines. Its mesh menu identifies the selected mode with literal “selected” text. The pale cyan-blue triangle edges use their own authored unlit material color, recorded in the sidecar, independently of the selected scalar palette; the retained scalar legend explicitly says that edges show topology.

Streamline tubes follow the captured viewport scalar mapping. The live popover’s work-limit recovery and “All seed sets:” readout use Text; the compact “limited” count uses Muted. The saved-seed density slider inherits CoreStyle and appears light gray in the captures; it does not use the automatic inlet slider’s authored Blue handle.

## Typography

The implementation requests Unreal CoreStyle Regular and Bold. No separate display or monospace family is configured for D08. The menu title uses the local title role; exact values, mode choices, row labels, and the menu key use body. “Arrow length” and “Display” use bold section text. Supporting copy, counts, recovery, and the compact key use label text. These compact sentence-case roles are extracted from this slice, not a replacement for the whole application’s type hierarchy.

The sidecar uses an explicitly approximate browser font fallback for portable component previews. Native capture typography remains authoritative.

D05–D07 reuses the title, body, section, and label roles. Seed-kind headings use the observed seed-kind role; the “Seeds” suffix in a shared list row uses micro. Snapshot annotations request CoreStyle Regular at `max(6, round(10 × s))`, where `s = clamp(min(outputWidth / 800, outputHeight / 500), 0.25, 4)`. This export scale is separate from fixed-size live control typography.

## Layout

Measurements describe Slate layout units at application scale; frontmatter px values are portable representations. The earlier D05–D08 floating Display overlay was 180 units wide with 11-unit padding; D11/S01 supersedes that placement with Display inside the 322-unit right inspector. Vectors retains one row: checkbox at the left, a compact settings button at the right. Its muted, wrapping length key sits 5 units below and collapses when vectors are hidden.

The vector settings popover requests 320 units of width, 16-unit padding, and a 450-unit maximum desired height with a scrolling body. Its order is title, optional recovery, Maximum samples, sampling explanation, Arrow length, two equal-width mode buttons, Scale, slider, key, arrow/sample/frame counts, and explanatory meaning. Exact fields occupy a 130-unit control column. The two mode buttons have a 4-unit gap. Title and major group separation use 14 units; numeric rows use 8; recovery and mode row use 10; the key-to-count spacing is 6, with 12 below the count.

Accepted native captures cover 1320 × 740 and 1280 × 720. Popup placement overlaps the surrounding workspace while keeping this local form and recovery readable. These dimensions are evidence sizes, not application breakpoints. The persistent navigation remains the sidebar; the incumbent rail switches between 172 and 52 units and its rows are 45 units high. D08 adds no navigation surface or mobile layout.

The two annotated snapshot captures are 1280 × 720; their filename prefixes identify the originating window run. Snapshot annotations keep the vector meaning below the scalar legend and source/frame/time in a separate bottom strip.

Streamlines uses a parallel checkbox/settings row in Display, with a Muted wrapping seed/trace summary 4 units below. Its popover requests 350 units of width, 16-unit padding, and a 530-unit maximum desired height; focus scrolls instantly into view with 8-unit navigation padding. The order is title, recorded velocity field, optional inline recovery, two seed-mode choices, automatic count/density or saved-seed action, direction, physical width/step/length, per-branch step cap, total work cap, captured summary, and interpretation. Exact fields occupy a 145-unit column. Choices share a 4-unit gap; count rows use 7 below, numeric rows 8, recovery/mode groups 10, title/saved-seed action/work cap 12, and the direction group 14.

Saved seeds use the existing right Inspection panel with 10-unit inset. Its shared list is capped at 138 units; notice and aggregate readout precede independently scrolling details, while Undo view / Redo view remain outside the detail scroll at the bottom. Seed numeric rows use a 110-unit field column and 5-unit bottom spacing. Inlet face pairs, A/B endpoints, Center/Span U/Span V, and selected-position navigation are local detail layouts. Captured partial rows at a scroll boundary are intentional scroll content.

For annotated streamlines with at least one captured segment, the snapshot bottom strip allocates separate source/frame/time, interpretation, and conditional work-limit rows. With export scale `s` defined under Typography, its margin is `12 × s`, row step `20 × s`, and footer height `(12 + 20 × rows) × s`. The scalar legend sits above this reserved strip. Frame-info and legend options remain independent of the streamline annotation option.

V10 centers its toolbar 12 units above the viewport bottom, below the input hint with a 6-unit gap. The flat Panel container has a 5-unit inset; controls have 4-unit gaps and 8 × 4 horizontal/vertical padding. Labels use the existing 9-point role; Fit and settings icons are 16 units. The Triangle mesh menu is 322 units wide with 14-unit padding. Viewport camera is 340 units wide, has 14-unit padding, and bounds its scrolling body to a 530-unit maximum desired height with instant focus scrolling. Its title is 12-point Bold and existing numeric rows remain compact.

Expand now hides the monitors and idle Solve inspector, including whichever of Setup, Physics, BCs or Display is selected. An active Inspection or camera-placement inspector remains available. The sidebar, viewport toolbar and timeline remain; Display controls return with the inspector on Restore. This supersedes the earlier V10/D01 layout claim that Display controls stayed visible while expanded. Restore returns the chosen category and working layout. This is an in-app layout preference saved in `StudioSession.json`, independent of case/project content and camera state; it is not OS fullscreen. The accepted V10/D01 window sizes remain evidence sizes, and its 960 × 540 annotated exports are separate from the earlier D05–D08 export dimensions.

D11/S01 uses the existing 322-unit inspector column and 7-unit gutter from the scene. The Panel container has 10-unit horizontal padding. Four equal-width category buttons use CoreStyle Regular 10-point labels, 2-unit horizontal and 12-unit vertical padding, Muted inactive text, Cyan active text and a 2-unit Cyan underline. Each category has its own retained scrolling body with instant focus scrolling and 12-unit navigation padding. The Display body adds 2-unit horizontal and 10-unit vertical padding. Section separators and compact label/value rows reuse the incumbent vocabulary; Setup’s lower content is deliberately reachable through its scroll boundary.

## Elevation & Depth

Native panels are flat. Tonal steps and one-unit control outlines provide containment; the vector extension defines no custom shadow or animated transition. The black scene ground and 3D field supply depth independently of the blue-black UI surfaces. Native popup/window behavior remains supplied by Slate.

Streamline controls use the same flat treatment and define no additional shadow or transition vocabulary. Scene seed labels use opaque Panel backing. Draft markers and lines have a dark outline behind Amber to separate them from the scientific field.

## Shapes

The Display panel, popover, key backing, and navigation row remain rectangular. Buttons and inputs use the incumbent gently curved corners specified in frontmatter and one-unit outlines. The settings glyph is the existing three horizontal lines with offset vertical marks, rendered at 14 units. Equal-width length-mode buttons remain ordinary compact controls.

## Components

### Vector settings action and length modes

The settings action uses the common Raised button style with Muted icon and no dropdown arrow. Its tooltip names sample count, length mode, and scale. The menu focuses its first enabled focusable descendant. “Proportional” and “Equal length” use Cyan text for the selected value; selection alone does not add a new border style. Hover/pressed outlines are Cyan, disabled surfaces use Background and Line, and native button keyboard focus is inherited from Slate.

### Exact numeric fields and recovery

Maximum samples accepts whole numbers from 1 through 4096. Scale accepts finite numbers from 0.2 through 3. Defaults are 384 samples, scale 1, and proportional length. Values render with up to eight significant digits in the exact editable field. Enter applies; uncommitted text and focus loss do not apply. Invalid, nonfinite, out-of-range, or fractional sample drafts remain visible while prior model values and geometry remain active. A valid edit or external view revision resynchronizes the displayed values, including focused fields.

Input interiors use Background and Text; the one-unit outline changes from Line at rest to Muted on hover and Cyan on focus. Recovery is one wrapped Amber message above the form. Samples names the whole-number range; Scale names its numeric range; both state “Previous setting retained.” Editing clears the displayed message; the latest rejected field supplies the shared recovery text.

**The Single Recovery Rule.** Keep rejected drafts visible and the previous applied value active. Show the complete wrapped recovery message inside the popover without a duplicate default error popup.

F1 removed the shared field’s default `Editor->SetError` calls while retaining its inline reporting and draft ownership. Both invalid captures retain `4097` and `nan`, show the latest Scale message, leave the slider unobscured, and preserve the prior 384-sample / 0.154 m key. Count rejection is also asserted in the routed workflow; the captures do not separately show a count-only error state.

### Scale slider and view ownership

The slider spans the same scale range as the exact field and uses Line for its track and Blue for its handle. Pointer/controller captures group continuous scale changes into one view edit. Sample count, scale, and length mode participate in reversible view changes and schema 14 project persistence as `vectorCount`, `vectorScale`, and `uniformVectors`. Earlier project versions retain the bounded default count and proportional mode; existing scale data remains validated. This is staged schema behavior, not an integrated release declaration.

The menu is bound to the project and source that opened it and disables its body if that context is replaced. Display settings do not change original recorded values, source/frame selection, or the camera merely by changing vector presentation.

### Sampling and captured-frame key

Original-point recordings choose evenly spaced original rows deterministically, capped by available rows; their vectors are not interpolated. The legacy field path chooses bounded evenly spaced sample positions on the 2D display plane. Solid, unavailable, invalid, zero-velocity, or uncolorable samples can produce fewer arrows than requested. The visible counts distinguish actual arrows from samples. Original sources without supplied velocity components disable the Vectors row and identify the missing components.

Proportional mode scales arrow length by velocity magnitude divided by the sampled frame’s maximum speed. Equal length preserves direction and explicitly says “direction only.” The reference length is 3.5% of the applicable domain extent multiplied by Scale. Color follows the selected scalar in either mode, including supplied speed when that is the chosen scalar; velocity components determine length and direction separately.

**The Captured Key Rule.** Show length, sampled maximum speed, arrow count, and frame identity from the geometry actually presented. Preserve that meaning in annotated export.

The panel key uses Muted label text; the menu repeats it in Text and follows it with arrow count, sample count, and captured frame. Proportional wording is “Arrow: … m” and “= … m/s (sample max)”; equal-length wording is “Arrow: … m” and “Equal length · direction only.” Explicit alternatives cover hidden vectors, missing velocity components, updating geometry, and no valid nonzero samples. A sampled maximum is not claimed as a whole-recording or whole-source maximum.

The original-point capture shows 96 arrows from 97 sampled rows. The original 3D fixture retains its source-dimensionality caveat and a velocity-based key. The pressure capture retains velocity length meaning alongside pressure color. Annotated snapshots use `CapturedVectors`, and metadata records sample count, glyph count, sampled maximum speed, reference length, and uniform-length mode. The supplied exports retain 257 samples/arrows, 0.25025 m reference length, and 88.4084636451024 m/s sampled maximum, displayed compactly as 0.2503 m and 88.41 m/s.

### Streamline settings and exact recovery

The Streamlines settings action uses the shared Raised button, Muted 14-unit settings glyph, and no dropdown arrow. The form identifies the single available field as “Flow field: recorded velocity (m/s)”. “Automatic inlet” and “Saved seed sets” select the seed source; “Forward”, “Backward”, and “Both” select velocity-trace direction. Cyan text identifies selection, with common native hover, pressed, disabled, and focus behavior. The settings body belongs to the project/source that opened it and disables when that context is replaced.

Let `L` be the longest side of the trace domain: attached 3D grid coverage when present, otherwise recording display bounds. Width, step, and maximum length are entered in meters but stored as fractions of `L`, so switching recordings rescales these distances. Width is the full tube diameter. Each direction has its own length and step limit; the total budget counts attempted steps across all seeds and branches.

| Setting | Accepted range | Default |
| --- | --- | --- |
| Automatic seed count | Whole 1–512 | 84 |
| Direction | Forward / Backward / Both | Forward |
| Width (m) | `0.000001 × L` through `0.02 × L` | `0.00175 × L` |
| Step (m) | `0.00001 × L` through `0.1 × L` | `0.00625 × L` |
| Max length (m) | `0.001 × L` through `100 × L` | `2 × L` |
| Max steps / branch | Whole 1–4096 | 512 |
| Total step budget | Whole 1–65536 | 32768 |

Streamline and seed exact fields request the shared field’s 17-digit mode, rendering the shortest decimal that round-trips to the applied double; the tooltip retains the full exact value. This differs from D08’s eight-significant-digit vector field display. A simple coordinate remains `-0.2`; pointer-derived coordinates legitimately retain longer values. Enter applies; focus loss does not apply a draft. Rejected nonfinite, fractional-integer, or out-of-range text remains visible with the prior setting and geometry retained. Valid changes and external view revisions synchronize focused fields. Recovery is one wrapped live Amber message with the range and “Previous setting retained.” No stored coordinate is rounded to shorten its field.

The recorded invalid state retains `513`, `3.5`, and `nan`; the accepted state uses 31 automatic seeds, Both, 0.0044 m diameter, 0.0176 m step, 0.66 m maximum length, 35 steps per branch, and 2048 total attempts. Setting the budget to 1 produces one trace and the direct recovery “Work limit reached. Increase the budget or reduce seeds/length.” Those are captured examples, not defaults or guaranteed trace counts.

### Saved seed sets and atomic placement

“Edit saved seed sets…” dismisses the popover and opens the shared Inspection panel. Its source-bound list owns name, visibility, duplicate/delete, detail editing, and view history. Adding a seed set switches to saved-seed mode in the same undo entry. Each set retains its stable ID and original dataset/metadata/payload identity; source changes cannot silently rebind it. Foreign-source, hidden-set, automatic-mode, and unsupported-set notices precede aggregate counts.

**The Aggregate Ownership Rule.** Prefix combined seed and trace counts with “All seed sets:” in a selected seed editor. Do not present those totals as selected-object counts.

F2 preserves true aggregate examples: a selected seven-seed inlet accompanies 23 seeds / 33 traces, a five-seed plane accompanies 14 / 22, and Point 2 of 2 accompanies 16 / 26. All labels fit at both recorded sizes. Empty saved mode states that no visible seed sets exist for the recording; the compact Display readout says “No supported traces”.

| Seed kind | Exact editor and placement behavior |
| --- | --- |
| Inlet face | Count/density and X/Y/Z minimum/maximum pairs. Seeds are inset 0.23% from the face. Y faces are unavailable for 2D; seeds remain on its original X/Z plane. Automatic mode uses the default X minimum inlet. |
| Line endpoints | Count/density and exact A/B scene-meter coordinates. Samples include endpoints; one seed uses the midpoint. Place in view commits after two clicks. |
| Rectangular plane | Count/density, Center, and full Span U / Span V vectors. Placement clicks center, U edge, V edge; edge distances become half spans. The deterministic grid distributes the requested count. |
| Selected positions | Exact X/Y/Z for Point n of N, Previous/Next, Pick another point, and Remove this point. Each chosen location is retained; controls enforce 1–512 positions. |

Counts are bounded at 512 per set, 4096 stored positions across sets, and 128 seed objects. Degenerate lines/planes, duplicate selected positions, and invalid geometry retain the previous object. Custom positions are neither snapped to nearby supported cells nor projected onto a 2D recording.

Place in view uses the source plane for 2D and a fixed camera-facing plane through the current point for 3D. Amber accepted draft markers and a Text preview marker distinguish an incomplete draft from saved Cyan/Muted seed geometry. Right-drag can look around without moving that plane; Escape or Cancel placement discards the draft. Saved coordinates change only after the final click, in one undo entry. The supplied line workflow explicitly checks retained coordinates after click one and complete placement undo/redo; plane and selected-position captures show their corresponding committed results. Source/object changes invalidate placement.

### Recorded tracing, interpretation, and frozen export

Streamlines trace one immutable instantaneous recorded velocity field with arc-length midpoint integration. They do not integrate time or represent temporal pathlines. Forward/backward branches share round-robin work; missing velocity/scalar, stagnation, solids, domain bounds, and complete-segment coverage end unsupported traces. Cancellation and source identity prevent obsolete work from replacing current results. Density, diameter, integration step, length, and budgets are visualization settings; they do not compute new solver output. Scalar color remains independent of velocity direction.

Original points without verified interpolation and recorded velocity disable the Streamlines row and produce no traces. A source may therefore support sampled vectors while streamlines remain unavailable. For attached reconstructions, F1 makes the live Display note explicitly identify “derived 2D triangle interpolation” or “derived 3D grid interpolation”, even with the scalar surface/volume hidden, and retains “Raw export retains every row.” A 2D source gains no spanwise field. The genuine 3D cylinder fixture establishes its supplied streamline state only.

**The Frozen Trace Meaning Rule.** Annotated export must identify instantaneous velocity streamlines, their derived interpolation when applicable, and captured work-limit exhaustion from the frozen snapshot identity and summary.

When annotations are enabled and `Snapshot.Streams.Segments > 0`, export shows “Instantaneous velocity streamlines” in Muted with the applicable derived interpolation. A captured exhausted budget adds “Work limit reached · tracing incomplete” in Snapshot amber. These lines remain controlled by annotations even when source/frame or scalar legend options are disabled; the accepted exports retain all three. The source/frame/time row uses Text, and the scalar legend retains captured scalar units and mapping.

Both supplied snapshots freeze cylinder frame 2200 at 22 s, 64 seeds, 128 traces, 2048 segments, derived-grid interpolation enum 3, reconstruction hash `25277f0fdcc3cd2196fe6efb147da1c24df4be332ac438d368fe948c234aefc8`, and `work_limit_reached: true`. PNG metadata also carries attempted steps, full tube diameter, seed mode, and the instantaneous midpoint method. Later live-view changes cannot alter these labels.

### Streamline persistence and evidence boundary

Staged project schema 15 stores complete `streamlineSettings`; inspection collection version 2 stores seed identity and exact geometry. Earlier projects retain the valid default streamline settings. The prior D08 schema-14 addition remains documented above as its historical change; this staged schema advance is not main integration. Settings and seed operations belong to reversible view history.

`StudioStreamlineRenderTests.cpp` records exact settings save/reopen, seed save/reopen for line/plane/selected positions, invalid recovery, undo/redo, placement, latest-frame replacement, true tube diameter, and scalar-colored geometry against recorded data. The inlet face case runs after that seed save/reopen; do not claim that particular saved file included an inlet. The supplied model evidence covers deterministic integration/geometry, whole-segment coverage, bounds, source isolation, migration, and shared history. These are recorded results, with the model timing qualification in Overview; no test was rerun for documentation.

Source authority is `StudioWorkspace.cpp/.h`, `StudioStreamlines.cpp/.h`, `StudioInspectionObjects.cpp/.h`, `StudioModelInspection.cpp`, `StudioScene.cpp/.h`, `StudioInspectionOverlay.cpp/.h`, `StudioSnapshot.cpp/.h`, `SStudioSnapshotOverlay.cpp`, `StudioProject.cpp/.h`, `StudioView.cpp`, and `StudioStreamlineRenderTests.cpp`, all under `Source/LBMStudio/`. The source contract, packet, review/verdict, verification JSON, and captures identified above bound this record to staged D05–D08.

### Native context and portable previews

The sidebar active row uses Raised and Cyan; inactive labels use Muted. The captured-frame strip retains source, frame, and time with the incumbent green replay badge. They are documented only as immediate context for D05–D08. `.impeccable/design.json` supplies self-contained HTML/CSS translations of these native appearances for the design panel. Those previews do not implement Slate validation, data sampling, history, project persistence, or native focus/input delivery.

Source authority is `Source/LBMStudio/StudioWorkspace.cpp` (`StudioUI`, `SValidatedViewNumber`, `VectorControls`, `VectorMenu`), `StudioFieldDisplay.*`, `StudioScene.cpp`, `StudioView.cpp`, `StudioProject.*`, `StudioSnapshot.cpp`, and `SStudioSnapshotOverlay.cpp`. The full reviewed source/capture packet is `../implementation/vector-controls-review-packet.json`; recorded verification is `../debug/vector-controls-verification-20260928.json` with final source inventory at `tmp/debug/vector-review-fix-20260928.json`.

### Viewport toolbar and camera ownership — V10

The strip presents Views, Perspective/Orthographic, Field/Mesh overlay/Wireframe, Fit, Expand/Restore, and the camera settings icon in that order. The mesh label becomes “No mesh” when an edge mode has been requested but the current presented frame has no usable topology. Views keeps the standard Faces, Edges and Corners actions; the orientation cube remains. The left Camera action is solely the saved-camera manager. Camera position, rotation, lens and depth clipping now live in the Viewport camera menu with Undo view / Redo view; the Setup inspector no longer repeats that camera section.

**The Single View Owner Rule.** Keep direction, projection, triangle display, Fit, expansion and camera settings together below the input hint; retain sidebar-only workspace navigation and the separate saved-camera manager.

Projection toggles only the saved camera’s orthographic flag, retaining exact position, rotation, focus, distance, field of view and orthographic width. The recorded native check uses FOV `48.123456789` and width `6.123456789`, rather than relying on the shorter visible number formatting. Standard directions retain focus/distance; Fit remains an explicit action with the F shortcut. Existing depth-clipping recovery retains invalid drafts and the previous applied range. Moving these controls does not change their shared view-history semantics or saved-camera collection ownership.

### Actual triangle mesh display — D01

The Triangle mesh menu offers “Field display” (0), “Mesh edges over field” (1), and “Mesh edges only” (2), followed by provenance/recovery and shared view history. These modes govern scalar/point fill and supplied triangle edges. Streamlines, vectors and independent inspection objects retain their own visibility. Edges-only hides fill only when a usable triangle mesh is actually available; it retains the selected scalar and its legend. The legacy `bMesh` setting remains the separate “Domain grid” control.

Original connectivity comes from the immutable recorded field. An explicitly attached, verified surface reconstruction supplies derived connectivity at original point positions. The recorded examples contain 10,216 original SU2 triangles and 37,188 derived triangles. Original 2D or 3D points without connectivity are never implicitly triangulated. Edge actions disable for missing topology or a mesh beyond the 131,072-face budget; the menu explains the reason, preserves field display and suggests a verified surface reconstruction when applicable. Over-budget, invalid or cancelled work publishes no partial mesh.

**The Explicit Topology Rule.** Show exact supplied or verified-derived connectivity with its provenance; never substitute a fabricated lattice, partial mesh, or implicit point triangulation.

Each supplied triangle reaches a dedicated procedural section at its exact node positions. Local barycentric opacity produces pixel-width edges using the native unlit, translucent, two-sided material with depth testing. This is not a global wireframe switch. Dense edges near the airfoil reflect the source connectivity and do not encode the active scalar.

Staged project schema 16 requires integer `meshStyle` in the range 0–2. Schema-15 migration defaults to Field display while retaining the independent domain-grid flag. Mesh mode persists through save/reopen and participates in shared view undo/redo without changing camera, source frame or case configuration. Expansion persists separately in the session and does not change the scientific document or field revision. Earlier schema-14/vector and schema-15/streamline changes above retain their historical scope.

### Mesh legend, frozen export and evidence boundary

The live scalar legend adds a wrapping Muted mesh note in a 166-unit column. It names “Original CFD mesh” or “Derived mesh”, the triangle count, and “Edges show topology”; edges-only adds “field fill hidden”. Unavailable topology instead shows the captured notice. These readouts follow the presented mesh summary.

With annotations enabled and captured mesh triangles present, the PNG footer repeats provenance, count, “edges show topology” and the conditional hidden-fill statement from the frozen snapshot. `triangle_mesh_display` metadata stores `triangles`, `derived`, `notice` and `field_fill_hidden`. Both accepted exports freeze 10,216 original triangles with `derived: false` and `field_fill_hidden: true`; the independent PNG audit confirms those values and chunk CRC validity. Later live display changes cannot rewrite frozen interpretation.

The source authority for this addition is `Source/LBMStudio/StudioWorkspace.cpp` (`ViewToolbar`, `MeshMenu`, `ViewportMenu`, `ColorLegend`, `Center`, `Settings`), `StudioMeshDisplay.*`, `StudioField.*`, `StudioRecordedSolver.cpp`, `StudioScene.*`, `StudioProject.*`, `StudioModel.*`, `StudioView.cpp`, `StudioSnapshot.*`, `SStudioSnapshotOverlay.*`, `StudioMeshDisplayTests.cpp`, `StudioViewportToolbarRenderTests.cpp`, `StudioCameraClippingRenderTests.cpp`, `StudioOrientationRenderTests.cpp`, and `Tools/create_mesh_material.py`. The finish review, verification manifest, capture set and PNG audit named in Overview bound this addition to V10/D01. Portable sidecar previews show appearance only and do not implement rendering, native interaction, history or persistence.

### Solve inspector owners — D11/S01

**The Single Solve Owner Rule.** Keep Setup, Physics, BCs and Display inside one right inspector; category changes must preserve the scientific document, frame, camera and render revision. The sidebar remains the sole persistent workspace navigator.

| Category | Owned content and limit |
| --- | --- |
| Setup | Replay/control-harness toolbar choice; recording selection and applicable reconstruction attachment; source sample/frame/duration disclosures, replay speed/loop, source link and playback status. Reconstruction display toggles belong to Display. |
| Physics | Read-only source conditions and coordinates, separately labeled saved-case backend, collision, turbulence and thermal settings. Missing source conditions say “Not supplied” or “See source reference”; saved choices do not alter recorded physics. |
| BCs | Read-only saved-case condition count, six domain-face assignments and imported surface-patch assignments/empty state. Recording boundary conditions are explicitly unavailable from this adapter. |
| Display | The single visible scalar selector and palette/range action, domain grid, applicable cut-plane/flow-layer controls, source-point/surface/volume presentation, and adjacent streamline/vector settings. The viewport toolbar retains camera and triangle-topology actions. |

Physics and BCs are a readable foundation, not completed editable authoring forms; full M3 editing remains open. The captured BGK and velocity-inlet values are explicit saved-case fixture choices, not newly computed CFD or supplied recording conditions. Category buttons dismiss open menus, select the category and save the session without changing scientific state. The captured workflow confirms exactly one visible scalar selector in Display, none in Setup, and one recording selector in Setup.

Camera placement takes precedence in the shared right column, then active Inspection, then the selected Solve category. Active Inspection replaces the category strip; Close restores the chosen category. This ownership uses the existing shared inspector rather than adding a second persistent panel. Expansion retains active Inspection/placement, with the idle-category behavior specified under Layout.

### Retained editor drafts and identity

Vector and streamline popovers retain the same editor objects when dismissed and reopened after category changes. Rejected text and its single wrapped Amber recovery remain visible while the previous applied setting stays active. The reviewed examples preserve vector Scale `nan` and fractional seed count `1.5`; repairing Scale to `1.625` applies on Enter, survives exact project save/reopen, and follows shared undo/redo. Focus loss does not apply a draft. Valid edits and external view revisions synchronize the vector/streamline fields, including focused fields.

**The Draft Identity Rule.** Retain unfinished display-editor text only within its project, solver instance and selected scalar; discard cached forms when any of those identities changes.

`CachedDisplayMenu` caches Color, Volume, Vector and Streamline editors. A project-ID, solver-instance or scalar-ID change clears the cache; workspace tick releases obsolete closures even before another editor opens. Color and Volume additionally rebuild after a render-intent revision because their older forms own local range drafts. Vector and Streamline editors handle revisions within their validated fields. Retained form scrolling drops a queued focus target when it no longer belongs to the reopened form, keeping the restored editor usable. These rules describe transient UI ownership, not serialization of unfinished text into the scientific project.

### Inspector preference and evidence boundary

`InspectorTab` defaults to Display (3), with Setup (0), Physics (1), and BCs (2). `StudioSession.json` stores `inspectorTab`; only finite whole numbers from 0 through 3 are accepted. Missing or malformed values retain the Display default. It is a session-only preference, independent of the schema-16 scientific project and the shared view-history values. `Studio.Inspector.SessionCategoryIsolation` covers all four fresh-reader choices, malformed values, unchanged serialized scientific document and unchanged render revision, subject to the model timing qualification in Overview.

Source authority is `Source/LBMStudio/StudioWorkspace.cpp` (the six-line contract, `CachedDisplayMenu`, `Settings`, `DisplayTools`, `SRetainedFormScrollBox`, workspace tick and inspector switcher), `StudioWorkspace.h`, `StudioModel.cpp/.h`, `StudioInspectorTests.cpp`, and `StudioInspectorRenderTests.cpp`. The packet, finish review, verification, six native manifests and 16 captures named in Overview bound D11/S01 to this staged foundation. Current recording/reconstruction/job regressions and main integration remain pending. Portable previews express appearance only; they do not supply Slate behavior, scientific validation, persistence or runtime evidence.

## Do's and Don'ts

### Do:

- Do keep sample limits, length mode, exact scale, and the current-frame key together in the vector popover.
- Do identify original-row sampling, zero or missing samples, and equal-length direction-only meaning.
- Do retain selected-scalar coloring independently of velocity direction and length.
- Do preserve rejected drafts, Enter-to-apply behavior, and synchronization after valid edits or view restore.
- Do keep this record and its acceptance claims scoped to staged D05–D08, V10/D01 and the D11/S01 foundation, retaining each review’s finding and package boundaries.
- Do retain exact seed coordinates, physical units, full tube diameter, and atomic placement history.
- Do label aggregate counts “All seed sets:” and preserve instantaneous/derived/work-limit meaning in frozen annotated exports.
- Do retain a single bottom view-control owner and keep saved-camera management distinct from the active viewport camera.
- Do preserve original/derived topology, triangle count and hidden-fill meaning in the presented legend and frozen PNG metadata.
- Do retain active inspection and placement when expanding, and persist expansion separately from the case and camera.

- Do preserve session-only inspector selection and source-bound draft ownership while category changes leave scientific state intact.
- Do label Physics and BCs as read-only source/saved-case summaries until editable M3 authoring is implemented and accepted.

### Don't:

- Don’t add a second persistent workspace navigation path.
- Don’t label requested sample limits as guaranteed arrow counts or sampled maxima as whole-source maxima.
- Don’t infer missing velocity components, interpolate original-point vectors, or imply spanwise flow from a 2D source.
- Don’t introduce decorative panel gradients, shadows, or a second clipped validation surface.
- Don’t treat these captures, preview snippets, the D08 F1 verdict, or the D05–D07 F1/F2 verdict as main integration, physical OS input, long-session stability, or full-app acceptance.
- Don’t fabricate interpolation for original points, snap unsupported custom seeds, or describe instantaneous traces as temporal pathlines.
- Don’t merge the live UI and snapshot Amber tokens or imply that a seed count guarantees the same number of traces.
- Don’t replace original points with implicit triangles, show a partial over-budget mesh, or confuse the domain grid with CFD connectivity.
- Don’t treat edges-only as a global wireframe mode or allow it to hide independently controlled streamlines, vectors or inspection objects.
- Don’t treat V10/D01’s ship verdict as main integration, physical OS input, stability, D11/S01, M3–M5 or whole-studio/reference acceptance.
- Don’t treat D11/S01’s foundation verdict as acceptance of editable M3 authoring, the four queued regressions, main integration or the wider UI goal.
- Don’t restore a floating Display duplicate, persist unfinished editor text as scientific state, or keep Display controls visible after the idle inspector is expanded away.
