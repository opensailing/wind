---
name: "LBM Solver Studio — Recorded-Flow Controls and Case Authoring"
description: "Accumulating scoped record of recorded-flow controls, the Solve inspector foundation, and Geometry transforms, Materials, Domain, Boundary Conditions and lattice-preview authoring in the incumbent Operate workspace."
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
  authoring-title:
    fontFamily: "Unreal CoreStyle"
    fontSize: "18pt"
    fontWeight: 700
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
  authoring-number:
    backgroundColor: "{colors.background}"
    textColor: "{colors.text}"
    typography: "{typography.body}"
    rounded: "{rounded.input}"
    padding: "5px 7px"
  material-row-selected:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.cyan}"
    typography: "{typography.body}"
    rounded: "{rounded.button}"
    padding: "9px 10px"
  authoring-layers:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.muted}"
    typography: "{typography.body}"
    padding: "5px 8px"
  authoring-inspector:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text}"
    width: "334px"
    padding: "14px"
  boundary-inspector:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text}"
    width: "310px"
  authoring-recovery:
    textColor: "{colors.amber}"
    typography: "{typography.body}"
  geometry-inspector:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text}"
    width: "304px"
    padding: "14px"
---

# Design System: LBM Solver Studio — Recorded-Flow Controls and Case Authoring

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

The 2026-09-28 authoring addition is a local code-led Operate extension for native macOS UE5.8 C++ Slate, scoped to Materials, Domain, Boundary Conditions and lattice-preview Meshing in `tmp/authoring-workspace-20260928`. Existing Solve tokens, component behavior and historical evidence above remain authoritative at their original scope. Earlier pending-integration statements describe those review checkpoints; they do not replace a current integration report. At the documentation checkpoint, the authoring source was installed in main but uncommitted pending production verification. The separate main-integration result below records the subsequent checks.

Authoring has one editable owner for each parameter: material properties and assignments in Materials, domain bounds/dimensions/face names in Domain, conditions in Boundary Conditions, and physical cell counts in Meshing. Solve's read-only source physics and saved-case summaries keep their existing roles. The sidebar is the sole persistent workspace navigator; local preview actions do not create a top workflow row. Applied case data, retained form text, recorded scientific fields, frozen run configurations and observing cameras stay distinct. Geometry fixtures and occupancy previews are authoring data; the genuine published CFD in Solve retains its scientific identity.

`../implementation/authoring-review-packet.md` and the full five-section `../implementation/authoring-finish-review.md` supply the authoring review. The appended correction verdict is **ship for F1–F3 only**, all resolved, on package `155679833b8b0522bce2192d543820c61bb747795ef05afc82d86189817768fe`, source delta `73fca1f932b435b08f7509314096509932f7def4017065edd5cb74d86f2fe6b7`. F1 unifies Domain renderability and layer labels; F2 separates verified aggregate geometry bounds from applied-domain containment; F3 restores full-opacity enabled hints. The original full review remains the finding basis; the correction verdict is not a new whole-surface review.

Artifact `tmp/implementation/authoring-source-verification.json` pins 45 changed source/tool files and schema 17. `tmp/implementation/authoring-native-verification.json` pins eight manifests, **12 clean native cases across 1320 × 740 and 1280 × 720**, and **34 valid whole-window captures** in `.impeccable/review/authoring/`, all on that one package. The finish reviewer opened all 34, checked hashes/dimensions, and found no obstructing tooltips; each native manifest reports passed, exit 0 and no errors or remaining owned processes. The verification files' verdict-pending status predates the appended ship verdict. The earlier **161 model cases predate the final UI/test changes and Domain source-status copy**, so they are not a final-package model result. The historical one-off Domain pointer-capture failure remains unexplained despite the subsequently passing complete matrix.

The authoring documentation pass read the source, review and recorded verification, opened nine representative captures, and checked all 34 capture hashes/dimensions, eight manifest hashes/results and 45 source pins. It ran no build, application, native interaction or test. Routed Slate/native GPU evidence does not establish physical OS input. Backend preparation, refinement/quality, remaining Geometry and Setup/Physics, full M3, connected solver validation, broad accessibility, long-session stability and overall reference fidelity remain incomplete. No new visual workshop, generated comp or HTML/CSS detector applies to this native extension.

**Main authoring integration — 2026-09-28.** Source commit `7336661` integrates the reviewed authoring controls with schema 17. `tmp/analysis/authoring-integration-20260928/production-acceptance.json` records 161 clean model cases and 18 native cases on package `24ae10c2c7b0dedc73e1eca44e2b0e456a637568aec6c75fa5597cfb7d22c205`: 12 authoring cases at both window sizes and six shared scene, inspection, camera, volume and job regressions at 1280 × 720. One inspection snapshot-menu test required an unchanged-package retry; its initial failure is preserved and no menu fix is claimed. The Domain handle test now activates the app before pointer input, and both size runs observed that activation before passing. The production interaction code is unchanged by that test adjustment. All accepted native runs exited 0 without remaining owned processes. An additional editor build with adaptive unity disabled passed. These checks establish the integrated authoring scope; physical OS input, extended stability, solver preparation and the remaining UI milestones retain their open status.

**Geometry object editing — 2026-09-28.** The six-line contract immediately above `SStudioWorkspace::GeometryObjectEditor` extends the existing Operate inspector with retained name, position, rotation and source-axis scale. Core transactions are committed in `874af51`; the reviewed UI and F1 correction are committed in `4ab3b88`. The object list, applied mesh and scrollable inspector retain their roles, with sidebar-only navigation and the existing native palette, type and focus treatment. This local extension introduces no replacement identity.

`tmp/analysis/geometry-edit-20260928/finish-review.md` is the original bounded finding basis; `finish-verdict.md` in that folder gives **ship for F1 only**, closing truthful removed-object recovery and focus. `f1-verification.json` pins final package `dbc1084618a3f6f8a2fa6f626f7b97b79cc1952bde43f8f0080e823073172435`, two clean Geometry inspector cases at 1320 × 740 and 1280 × 720, and 16 final captures under `.impeccable/review/geometry-edit-f1/{1320,1280}/`. Both cases exited 0 with no remaining owned processes. The earlier four native cases and 164 successful model cases in `ui-acceptance.json` predate the final UI state/copy/focus correction; one passing model case has Unreal's bundled `idevice_id` CPU-architecture helper warning. `core-acceptance.json` separately records 164 clean model cases before the UI work. No fresh final-package model run is claimed, and the correction changes no core model behavior.

The Geometry documentation pass read the native source and recorded evidence, opened four representative final captures including removal at both sizes, and checked all 16 capture hashes/dimensions, both manifest hashes/results and ten source pins. It ran no build, application or test. These structural geometry fixtures and routed Slate cases establish this inspector scope; full Geometry, connected solver readiness, physical OS input, broad accessibility, long-session stability, overall reference fidelity and whole-product completion remain open.

**Setup run parameters — 2026-09-28.** The six-line contract immediately above `SStudioWorkspace::RunSettingsControls` extends the native Operate inspector with exact retained requests for the next run. Core transactions are committed in `0b580d1`; the UI is committed in `cb35759`. Run parameters leads Setup within the existing right column, reusing compact CoreStyle type, blue-black fields, Cyan actions/focus and Amber recovery. The sidebar remains the sole persistent workspace navigator; the published flow and frozen active-run configuration retain their ownership. Project schema 17 is unchanged.

`tmp/analysis/run-settings-20260928/finish-review.md` gives **ship**, with no material fixes, for the bounded form and reviewed guards. `ui-acceptance.json` in that folder pins package `c83d4a60829eee3b6980f8455da5a4dd0d2d58f88b2439137ca6f511a7c08624`, four source/tool files and 10 clean native cases: one RunSettingsUI case at each of 1320 × 740 and 1280 × 720, one Inspector regression at 1280 × 720 and seven Jobs cases at 1320 × 740. All four manifests report exit 0, no errors and no remaining owned processes. The 16 captures under `.impeccable/review/run-settings/{1320,1280}/` cover applied defaults, retained draft, invalid count, save guard, applied parameters, conflict, active run and reopened parameters; the finish reviewer opened all 16. `core-acceptance.json` records 167 successful model cases before the UI work, 166 clean and one with Unreal's bundled `idevice_id` CPU-architecture helper warning. No final-package model rerun is claimed. The two earlier failed native timing runs remain recorded in `ui-acceptance.json`.

The run-parameter documentation pass read source, review and recorded evidence, opened four representative captures across both sizes, verified ten source pins and four manifest/report results, and checked all 16 PNG payloads and dimensions. It ran no application, build or test. This acceptance covers retained parameter authoring, guarded submission and frozen-run/recording isolation. Backend stopping, output selection and disk estimates, restart compatibility and full S13–S15 remain open, as do physical OS input, full accessibility, long-session stability, whole-renderer and overall reference fidelity. No numerical backend enforces these requests or produces scheduled output/restart files.

**Monitors — 2026-09-28.** The six-line contract beside `SStudioWorkspace::MonitorWorkspace` adds a large original-time chart and one source/series/provenance inspector to the incumbent native macOS UE5.8 Slate Operate workspace. Sidebar navigation, blue-black panels, compact CoreStyle type and cyan focus retain their existing roles. Model/settings commit `cb03c92`, same-project reopen fix `5f238f8`, original-row export `8adbfed` and UI commit `abe2fa3` implement this bounded published-history slice. Schema 18 adds saved monitor settings; expansion and the compact preview choice remain in-memory session state. `PRODUCT.md` is unchanged.

`tmp/analysis/monitors-20260928/finish-review.md` is the initial bounded finding basis; `finish-verdict.md` gives **ship for F1–F3 only**, all resolved in correction round one. F1 identifies the single compact trace with series/unit/position/count and a native picker, F2 visibly names Linear/Log, and F3 distinguishes original-time ticks under deep zoom. This verdict does not repeat the initial full scoped review or accept the whole studio.

`ui-acceptance.json` in that folder pins final package `274d5cb8f3592f50ee6eb006cefcccb5f1cdf587d6db81ad147dcab2c34e4e80`, 15 current source hashes, 30 final captures and three clean native cases: Monitors at 1320 × 740 and 1280 × 720, plus the shared Inspector regression at 1280 × 720. All three manifests report success, exit 0 and empty owned-process inventories. The earlier model report records 172 successful cases, 171 clean and one with Unreal's bundled `idevice_id` CPU-architecture helper warning; its timing precedes the final UI-only corrections. No final-package model rerun is claimed. Earlier native helper-focus failure evidence remains in the acceptance record.

The same reviewer opened all 30 final captures under `.impeccable/review/monitors/{1320,1280}/` and found them valid. This documentation pass read source and recorded evidence, opened four representative captures and verified all 30 PNG signatures, chunk CRCs, compressed payloads and dimensions, all 15 source pins and all three accepted native manifests. It ran no build, application or tests. Original-sample hover, tooltip appearance, native Save panels and lower provenance content are not established by these captures. Physical OS input, broad accessibility, installed residuals, external-history import/portable paths, probe histories, live metrics/log/performance scope and current long-session/release gates remain open. Prior source-reader/provenance evidence and all earlier design scopes retain their boundaries. The context helper's desktop-platform fallback does not make this a web surface; no HTML/CSS detector applies.

**Activity log — 2026-09-28.** M05 expands the existing Solve log card inside the native macOS UE5 Slate Operate world. Journal commit `df2373e`, frozen export commit `dff1cab` and viewer commit `6defb25` retain the existing palette, type, focus treatment and sidebar navigation. The viewer owns session observations and transient filters/expansion; project schema remains 18. `PRODUCT.md` and the established visual world are unchanged.

`tmp/analysis/activity-log-20260928/finish-review.md` gives **ship for the scoped M05 Activity log addition**, with no material fixes. Its `ui-acceptance.json` pins package `55b65787383448bbb9a0549b1b1075de9b1a08398189a8edc80531fb5e01b858`, 17 source hashes and 22 captures under `.impeccable/review/activity-log/{1320,1280}/`. Two clean `Studio.LogUI.` cases at 1320 × 740 and 1280 × 720 and a clean `Studio.MonitorUI.` regression at 1280 × 720 use that same package; all exited 0 with no remaining owned processes. The final editor model report records 176 clean passes. The finish reviewer opened all 22 captures after two self-visual rounds; this documentation pass opened four representative captures and verified source pins, PNG payloads/dimensions and all three native manifests without launching builds, tests or applications. Physical OS input, clipboard delivery, native save-panel interaction, tooltip appearance, live solver log transport and current long-session/release gates remain unverified. The full UI plan and whole-product acceptance remain open.

**Application command input — 2026-09-28.** M06 adds one command field below the expanded Activity log in the incumbent native Slate Operate workspace. Core commit `291c773` and UI commit `65b6dbc` retain the existing palette, type, focus treatment and sidebar navigation. Commands name their replay/control target, reuse application guards and keep local responses visible independently of journal filters or pause. Project schema remains 18.

`tmp/analysis/commands-20260928/finish-review.md` is the original full scoped review and finding basis; `finish-verdict.md` gives **ship for F1**, with no listed material finding remaining. F1 keeps oversized-input rejection active when history navigation leaves the shortened draft unchanged. `f1-acceptance.json` pins package `3635805f25edb7724f011df0d211e9c762d8627864f0947928054e2970649792`, 16 source hashes, 22 captures under `.impeccable/review/commands/{1320,1280}/`, and four clean native cases: CommandUI at 1320 × 740 and 1280 × 720, plus LogUI and RunSettingsUI at 1280 × 720. All exited 0 with no remaining owned processes. The latest model report records 180 clean passes; it precedes only the final native-test layout-wait adjustment, with application/model sources unchanged. The earlier pre-F1 model warning remains historical evidence.

This documentation pass read source, review and recorded evidence and verified all source/capture hashes, four native manifests and model totals; it performed no new visual review or app/build/test run. Routed Slate events and known-path saving bound this evidence. Physical OS input, native Save As, clipboard, tooltip appearance, broad accessibility, real backend transport, original crash diagnosis and current extended/release stability gates remain open. The full UI plan and whole-product/reference acceptance remain incomplete.

**Original residual histories — 2026-09-28.** M01 extends the existing Monitors chart/source inspector and Solve residual card in native macOS UE5 Slate. UI commit `a3aa6f3` preserves the user’s dense dark reference, existing tokens and sidebar-only workspace navigation. One Choose history menu owns installed force histories, the saved residual source, Import residual log… and Locate exact residual source…. Schema 19 saves each family’s source and chart settings independently; the active Monitors family is session state. On a new project session it prefers force when saved, otherwise saved residuals. Earlier pending-residual statements describe their historical checkpoints; this addition closes the original-log import/chart/repair UI gap.

`tmp/analysis/residual-ui-20260928/finish-review.md` gives **ship for the scoped M01 residual UI addition**, with no material fixes. Its `ui-acceptance.json` pins package `02da6f5b57abb94db21d3b9f790ea7e0a2bda9dc223e63aac13b6ecfd5ba3582`, 188 clean model cases and three clean native cases: residual UI at 1320 × 740 and 1280 × 720, and force Monitors at 1280 × 720. All three suite manifests report pass, exit 0, no errors and empty remaining owned-process inventories. The finish reviewer individually opened all 55 captures: 36 residual states, 15 force states and four native picker/save/relaunch observations. The verified external log has 20,000 source times and 291,444 linear solves; its redistribution grant remains unverified and its data is not bundled.

`native-picker-acceptance.json` in that folder records actual NSOpenPanel import and NSSavePanel project saving, followed by a new application process showing the same 20,000-sample source. Native Tab/Return and OS-panel accessibility controls supplied that interaction; mouse/chord activation was unreliable. A temporary launcher finalizer raised `TypeError` after application exit, so no launcher exit-code manifest exists for that sequence. Recorded normal application shutdown and the empty final process inventory remain narrower evidence. This documentation pass read native source, the contract, review, acceptance records and three manifests; it ran no application, build or tests and performed no new visual review. Broad physical pointer/keyboard reliability, tooltip appearance, accessibility, live backend transport, current long-session/release gates and the full plan remain unaccepted. The scoped verdict does not accept overall studio/reference fidelity.

**Solve performance inspector — 2026-09-28.** UI commit `0141b20` extends the existing right inspector in the native macOS UE5 Slate Operate workspace. The single **View performance** action in Control status keeps the flow visible and retains the case, recording, selected frame and camera. The original dense dark reference, incumbent palette/type and sidebar-only workspace navigation remain authoritative. The adapter telemetry contract in `c83c478` and the application sampler have separate measurement ownership; the control harness supplies no numerical solver telemetry. Project schema remains 19.

`tmp/analysis/performance-20260928/finish-review.md` gives **ship for the bounded Solve performance inspector and actual application measurements**, with no material fixes. Its `ui-acceptance.json` pins package `3388837fcd439dd6659d67949f0970a5002eb4045c7bd05d90ef7849ff4545a3` and 14 source/tool hashes. Recorded verification includes a successful editor build, **196 successful model cases** (195 clean and one with the unrelated bundled Unreal iOS `idevice_id` CPU-architecture warning), **12 passing runner tests**, and **two clean native cases** at 1280 × 720 and 1320 × 740. Both native cases exited 0 with no remaining owned processes, using the `explicit-tracking` startup profile and `-LLM`.

The finish reviewer opened all **14 final captures** under `tmp/debug/performance-final-{1280-20260928T164520809295Z,1320-20260928T164602840847Z}/captures/`: solve, live, paused, paused-camera, resumed, solver-unavailable and closed at each size. The native cases exercise End, Escape, focus/settings restoration and continued camera captures with frozen readings at a fixed recorded frame. Active replay advancing during a readings pause is source-reviewed only: the panel copies its display state while root collection continues independently. Home/PageUp/PageDown and the Close button's shared close handler are also source-reviewed. The earlier 1280 frame-isolation failure remains recorded; the later passes do not establish its cause or broad input reliability.

This documentation pass read the implementation and recorded evidence, verified the 14 source/tool pins and both native manifests/reports, and checked the model/runner totals. It launched no application, build or tests and performed no new visual review. The disposition excludes numerical solver transport and native acceptance of future current/stale/disconnected/final numeric solver states, GPU timing/utilization, physical OS-input reliability, full keyboard/accessibility acceptance, active-replay freeze testing, long-session/release gates, unrelated field/reference fidelity and full five-milestone plan completion. No HTML/browser detector applies; iOS and Android are outside this native desktop scope.

**Recorded probe histories — 2026-09-28.** M03 integration commit `3b53255` connects backend `e5e022a` to the existing Monitors source menu, chart, export and exact-frame return to Solve. This is a local native macOS UE5.8 Slate Operate extension. The original dense dark CFD reference, blue-black tokens, CoreStyle hierarchy, cyan focus and sidebar-only navigation remain authoritative; no new visual identity or system refresh applies. Histories and their chart choices are session state; saved probe definitions retain existing project persistence. Earlier pending-probe statements describe their historical checkpoints.

`tmp/analysis/probe-history-20260928/finish-review.md` is the original M03 review and finding basis: the reviewer opened all 35 supplied captures and required F1, distinguishable value-axis labels under zoom. `finish-verdict.md` gives **ship for the scored F1 fix only**, after individually opening five replacement captures. F1 adapts significant digits until adjacent ticks differ, measures the native label margin and keeps chart reduction aligned with the actual plot width. The force, residual and compact Solve regressions showed no visible regression from this correction. This is not a repeated whole-surface review or whole-application approval.

`f1-acceptance.json` pins package `0a9dc534c75c9b6b737a6edec115ea99aafb5ecd64e7367a10695f1a019bc563`, 14 source/tool hashes, **206 clean model cases** and **four clean native cases**: probe history at 1320 × 740 and 1280 × 720, then force and residual regressions at 1280 × 720. All exited 0 with empty owned-process inventories. Original `ui-acceptance.json` remains the full capture checkpoint before F1. `core-acceptance.json` separately retains the `e5e022a` backend audit: 203 successful cases, one with an unrelated Unreal helper warning, including original NACA, all 601 SU2 frames, a three-frame original 3D fixture, reconstruction/gap checks and bounded worker/reader failure isolation. Its source audits are not reclassified as new native UI evidence.

This documentation pass read implementation, direction contract and recorded reviews/evidence; it verified all 14 final source/tool pins, four native manifests and the final model totals. It ran no application, build or tests and performed no new visual review. Native input is routed Slate keyboard events with injected dialog paths; cancellation is exercised before worker publication. Physical OS input, native-picker interaction, long-running cancellation stress, long-session/release acceptance, unrelated Solve/reference fidelity and full-plan completion remain outside this record. No HTML/browser detector applies to this desktop scope.

**Results browser checkpoint — 2026-09-28.** Commit `6b28b69` adds the recording/run browser within the incumbent native Slate world; project schema remains 19. `tmp/analysis/results-20260928/finish-review.md` is the original finding basis, and `finish-verdict.md` gives **ship for F1/F2 only**: full-opacity search hints and corrected ownership documentation. The reviewer opened all 12 replacement captures; measured hint contrast is 14.36:1 in both modes at both sizes. `ui-acceptance.json` pins final package `7beb59e53e926a379cd907b49048503bc2415c6404dd04e9adc632567ea8f9a5`, 10 source/tool hashes, 209 successful model cases (208 clean, one existing Unreal iOS-helper warning) and four clean native cases: Results at 1320 × 740 and 1280 × 720, external-recording relocation and the Solve inspector. All native runs exited 0 with no remaining owned processes. Final Results capture roots are `tmp/debug/results-20260928T183116029436Z/captures/` and `tmp/debug/results-20260928T183126875785Z/captures/`.

This documentation pass inspected five representative final captures and verified the pinned source/capture hashes, dimensions and recorded reports without launching the app, builds or tests. Routed Slate input and injected folders bound this evidence. The verdict does not establish physical OS/native-picker access, current long-session/release acceptance, comparison/time alignment, pipelines, bulk export, whole-renderer/reference fidelity or full five-milestone completion. Comparison has its own later record below. Earlier review and package boundaries remain intact.

**Results comparison checkpoint — 2026-09-28.** The alignment core `7a95e5e194153c6d2e0955a0d8c8606d09f0b769`, immutable snapshot adapter `828480cdade7d32dcdef7782c54f9fb813c0d0c3` and visible UI `3ee6e809e8004108360da21ae4988e99ca57f720` are separate commits. `SStudioComparisonWorkspace.cpp` carries the local direction contract: explicit original-time correspondence, equal native A/B views, honest source identity and independent cameras within Results. The established blue-black panels, compact CoreStyle type, cyan focus, amber recovery and actual CFD scene depth remain the visual authority. No new primitives, browser implementation or visual world are introduced; at this historical session-only checkpoint project schema remained 19.

`tmp/analysis/comparison-ui-20260928/finish-review.md` is the original finding basis after 27 captures were opened. It required F1, actionable no-overlap recovery copy. `finish-verdict.md` gives **ship for F1 only** after reopening that correction at 1320 × 740 and 1280 × 720. This verdict covers the scored correction and retains the original comparison scope; it does not accept the whole surface or application.

`acceptance.json` pins 13 source/tool hashes and corrected package `e03a89fca94399c02fdc53077980757ae75e5b6e9db13b1ccec924516a6e2568`, which passed two clean comparison cases at those sizes. Before the copy-only correction, the model report recorded **220 successful cases** (218 clean; two with the existing Unreal `idevice_id` CPU-architecture helper warning), and package `872cecac4b7897b588a2e26e3b11f3d8561a6bfbb83fef403e358f2b25d9cff2` passed four clean native cases: comparison at both sizes and Results/Inspector regressions at 1280 × 720. Model and Results/Inspector evidence was not rerun on the corrected package. All six native runs exited 0 with empty remaining owned-process inventories. Final comparison captures are `.impeccable/review/comparison-ui/{1320,1280}/`; `results-entry.png` retains the earlier package's Results entry capture.

This documentation pass read native sources, the direction contract and recorded reviews/evidence, opened four representative captures, and verified all 13 source/tool hashes, 27 capture hashes/dimensions, six manifests and model totals. It launched no app, build or tests. Routed Slate input and original reader fixtures do not establish physical OS/native-picker access, full accessibility, full-source-duration playback, current 30-minute/RSS or release acceptance. At this historical checkpoint comparison saving/export remained open; saved configurations have a separate schema-20 record below. Comparison export, pipelines, bulk workflows and the five-milestone plan remain open; the disposal checks do not constitute a long-session memory gate. Historical evidence and token/snippet provenance remain intact.

**Saved comparisons — 2026-09-28.** Core `8d06373ddda0dcc5eb0316884ef27ac59eba5f23` and UI `482f71fb62215d90417532ffdb0abfa5b86ef377` extend the same Results comparison header with Save comparison… and Saved comparisons popovers. The direction contract, equal A/B scenes, incumbent blue-black palette, compact CoreStyle hierarchy, cyan focus, amber recovery and sole persistent sidebar remain authoritative. Schema 20 persists exact configurations; the live opened view and worker state remain session state. This addition supersedes the earlier session-only/persistence-open scope without widening the historical F1-only review.

`tmp/analysis/saved-comparisons-20260928/finish-review.md` gives **ship**, with no material fixes, for the saved-comparison extension at 1320 × 740 and 1280 × 720. Its `ui-acceptance.json` pins five source/tool hashes and 20 final captures under `.impeccable/review/saved-comparisons/{1320,1280}/`. Final package `a921f2fa956f4e9af99d61eb898cf7b4f2ba012a36ad51d27e348a7a479dc3c8` passed four clean native cases: saved comparisons at both sizes, then existing comparison and Results at 1280 × 720. All exited 0 with no owned processes remaining. The model report records **225 successful cases**, 224 clean and one with the existing Unreal `idevice_id` helper CPU warning, before a final native-test-only menu-timing change. Earlier failed native timing runs and the pre-presentation blank volume capture are superseded by this final settled evidence, not reclassified as passing.

The finish reviewer opened all 20 captures. This documentation pass opened five representative captures, read the source and review, and verified pinned hashes, dimensions, four native manifests/reports and model totals without launching an app, build or test. The volume capture uses the authentic Cylinder3D reader fixture with three original snapshots. Routed Slate input and injected folders do not establish physical OS/native-picker, full accessibility, full-duration 3D playback, current long-session/RSS, release or full-plan acceptance. Comparison export and bulk workflows remain open; prior source provenance, tokens, snippets and review boundaries are preserved.

**Original-frame VTK export — historical checkpoint, 2026-09-28.** UI commit `238610a` extends the single header Export menu in the incumbent native macOS UE5 Slate Operate workspace. The direction in `tmp/analysis/field-export-ui-20260928/direction.md` preserves the dense dark reference, existing palette/CoreStyle hierarchy, cyan selection, amber recovery and sole persistent sidebar. The original-frame task panel adds no visual identity or project schema.

`finish-review.md` in that folder gives **ship**, with no findings, at 1320 × 740 and 1280 × 720. `ui-acceptance.json` pins package `c6b0508d18ab26949e3623a8b06bed4930e795d273ee2a7ecb793d73908ca09a` and 11 source/tool hashes. Recorded evidence includes **229 successful model cases** (227 clean, two with existing Unreal helper warnings), **two clean native cases**, exit 0 with no owned processes remaining, and **20 captures opened by the finish reviewer**. The two existing VTK 9.7 audits each cover two exported files and 301,407 original point rows, with exact original coordinates, IDs, topology and selected scalar values and no reader warnings/errors. SU2 pressure/density stay at ordinal 420 in scene coordinates after the scene advances; the genuine cylinder point fixture exports all six arrays at ordinal 2 in source coordinates.

This documentation pass read the native implementation, direction, review and recorded evidence and verified the 11 source/tool pins. It performed no build, test, app launch or new visual review. Routed Slate keys and injected destinations establish the exercised form workflow; actual save-panel interaction/permissions, physical input, Finder reveal and full accessibility remain unproven. The progress capture uses a deterministic pre-publication barrier, not a performance measurement. The CSV entry is retained but this native case does not write CSV. Time-range/bulk and comparison exports, pipelines, full scientific rendering, current long-session/release gates and full-plan acceptance remain open. Earlier visual tokens, snippets, history and evidence boundaries retain their authority.

**Unified original-field Export — 2026-09-28.** Feature commit `f429747` extends the incumbent form to VTK XML/CSV and displayed/range/all original frames. `tmp/analysis/export-range-ui-20260928/direction.md` retains the native macOS Slate Operate world, established palette/CoreStyle roles, cyan selection, amber recovery and sole persistent sidebar. The header opens one form directly. Its fixed source header and action/status/reveal footer surround scrolling options; this is a local extension with no new visual identity or project schema. It supersedes the historical VTK-only panel and pending range/CSV descriptions above while preserving their original evidence boundaries.

The fresh `finish-review.md` in that folder gives **ship**, with no material findings, at 1320 × 740 and 1280 × 720. `ui-acceptance.json` pins package `a10a1abc4840f57918f9b38956277245d505de820c8f0c585d76be3fd81333ce` and 13 source/tool hashes. Recorded evidence includes **236 successful model cases** (235 clean and one with the existing Unreal `idevice_id` bad-CPU-type warning), **12 runner checks**, **two clean native cases**, exit 0 with no owned processes remaining, and **36 captures opened by the finish reviewer**. Each independent CSV/VTK audit checks eight field files and 2,078,451 original point rows with exact observed values, plus both sequence indices. The exercised outputs include frozen SU2 selection, CSV current/range and all three authentic cylinder reader-fixture frames in VTK.

This documentation pass read the implementation, direction, review and recorded evidence; verified all 13 source/tool pins, package/native pins, report totals, exported-file/index hashes and the dimensions of all 36 captures; and merged the current behavior into the existing docs. It performed no build, test, app/browser launch or new visual review. Routed Slate input and injected file/folder destinations do not establish physical input, actual native picker/permissions, Finder reveal or full accessibility. PVD XML and referenced VTP files were checked separately; actual ParaView collection playback remains unproven. The deterministic pre-publication progress barrier is not throughput evidence. Comparison exports, pipelines, image sequences/movie, full scientific rendering/reference fidelity, current long-session/resource/release gates and full-plan acceptance remain open. Existing tokens, preview snippets and historical acceptance remain intact.

**Native Post-Processing — 2026-09-28.** UI commit `edb2d2d` adds a local Operate workspace governed by PRODUCT.md, this design system, StudioTheme and the direction contract at the top of `SStudioPipelineWorkspace.cpp`. Sidebar-only navigation, compact CoreStyle controls, blue-black panels, cyan selection and amber recovery remain incumbent. A scrolling operation editor accompanies dominant scientific output; source identity, numerical method, Evaluate and root project Save retain explicit ownership. This supersedes earlier pending-pipeline-UI descriptions without extending their historical verdicts.

`tmp/analysis/pipeline-ui-20260928/finish-review.md` found only **PP-F01**, conflicting default scalar names. The correction updates an untouched default label with the selected scalar and preserves custom aliases; `finish-verdict.md` gives **SHIP for PP-F01 only**. Review used a fresh read-only CLI because the collaboration thread cap prevented spawning; its first provider-503 attempt was followed by one scoped retry. The verdict predates final test-only input pacing/diagnostics. Reviewed images are archived in `review-scored-captures`; this documentation pass opened all eight final run captures separately.

`acceptance.json` records **253 successful model cases** (250 clean, three existing Unreal `idevice_id` architecture warnings), with production sources unchanged afterward. The later test-only changes were built into package `70723c442b9cc405231c3525a911f944183f4607b4b47ef4b0f25d3701f5f5b8`, which passed the complete native workflow at 1320 × 740 and 1280 × 720, both exit 0 with no owned processes remaining. This pass verified all seven source pins, the current package and native reports, model totals and eight final PNG dimensions. Earlier intermittent menu assertions, including after pacing, remain unexplained; passing with immediate-open assertions establishes neither root cause nor fix. The PP-F01 verdict does not accept the full workspace, native gate or app; later native results retain their own scope. Physical OS input, native pickers, full accessibility, pipeline-output export, current integrated long-session/release gates and full-plan acceptance remain open. No source change, build, test or app/browser launch was performed for documentation.

**Evaluated pipeline Export — 2026-09-28.** UI commit `43543ed`, following presentation-only snapshots `4319cbf`, extends the single root Export owner. Post-Processing freezes completed evaluated output; other workspaces retain original Solve export. A pending task keeps its frozen context across workspace/project changes. `tmp/analysis/pipeline-export-ui-20260928/finish-review.md` gives **ship for the captured local menu extension only**, with no demonstrated material fixes. The fresh read-only Codex CLI reviewer inspected all 30 captures after the collaboration thread cap prevented a fresh spawn. The incumbent theme is implemented in `StudioWorkspace.cpp` with declarations in `StudioTheme.h`; there is no `StudioTheme.cpp`.

`acceptance.json` records **256 model successes** (253 clean, three existing helper warnings), predating final copy-only UI and native-test-only changes. Final package `355bd5188ad8cde4ae512e42966c04f8b3ee973907a4128a75df538c8fb4b79d` passed complete pipeline-export workflows at 1320 × 740 and 1280 × 720, both exit 0 with no owned processes remaining. The original-field regression passed earlier on `78122be1…`, before those changes. Final independent readback covers 10 files, 345,102 serialized vertices and 34 probe rows, with exact serialized numbers and maximum independent scalar-sampling error `2.842170943040401e-14`. The visual/source reviewer did not independently verify exported bytes, provenance, camera behavior or schema; those claims retain their separate source/readback evidence.

This documentation pass read the scoped implementation and evidence, verified ten source/tool pins, package/native records, model/readback totals and 30 PNG dimensions, and preserved the established tokens/snippets. It ran no app, build, test or new visual review. The new extension supersedes historical pending-pipeline-export descriptions without widening earlier core or PP-F01 reviews. Intermittent menu assertions remain unresolved; popup lookup/diagnostics and passing final workflows do not establish a root-cause fix. Physical input, native picker/permissions, Finder reveal, full accessibility, current integrated long-session/release and full-plan acceptance remain open.

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

- Materials, Domain, Boundary Conditions and physical lattice counts have separate retained case editors with explicit Apply/Revert and save guards.
- Preview labels describe applied state, renderable drafts, source verification and sampled occupancy without implying solver readiness.
- Geometry retains exact object drafts beside the applied mesh, with explicit Apply/Revert and removal-specific Discard recovery.
- Setup retains next-run stop/output/checkpoint requests with Apply/Revert, shared case history and visible backend limits.
- Monitors preserves independent published source/time/units, exact-row export and a named single-series force preview in Solve.
- Residuals retain original-log time/value lines, separate saved settings and a bounded three-trace Solve preview beside force history.
- Activity log keeps UTC observation/source identity, stable paused snapshots, recoverable clear and frozen-context export within Solve.
- Command input keeps exact replay/control targets, guarded sending, bounded session recall and local responses inside the expanded log.

- Performance keeps measured application activity, source-attributed flow work and unavailable solver telemetry together in one inspector with a frozen UTC view.
- Recorded probe histories keep original frame/scalar/position identity through explicit generation, gap-aware charts, frame links and frozen CSV export.

- Results comparison keeps equal original A/B views, explicit alignment, independent cameras and a local saved-configuration library within the sole sidebar navigation path.
- One root Export form keeps original frame/sequence or evaluated-output identity, format, coordinates and a cancellable task together with fixed source and action/status regions.
- Post-Processing keeps ordered operations, pinned original source/frame, explicit evaluation and independent output together within the sidebar-owned workspace.

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

Authoring reuses Cyan for selection and focus, Amber for recovery, draft geometry and outside/unknown containment, and the same Text/Muted hierarchy. Enabled optional-value and filter hints use Text at full hint opacity through the existing `SProjectFilterBox`; “Unknown”, “Optional · unknown” and “Filter by name” remain hints rather than stored values. The F3 reviewer measured six captured regions at **14.36:1** contrast (foreground RGB 223/232/243, background 15/25/34), above the 4.5:1 finding threshold. These are measured native pixels, not new palette tokens or a full accessibility certification. Disabled controls keep their separate native treatment.

Monitors reuses the native panel, text, action and recovery colors. The chart owns a separate six-color trace cycle, fine linear-color grid/labels and cyan hover/focus lines in `StudioMonitorChart.cpp`; these local chart colors do not redefine the application or scalar-field palette. Trace color follows plotted selection order. The compact force preview identifies its sole trace by name and unit; the residual preview uses the first three selected trace colors with named legends. Source constants are recorded in the sidecar.

## Typography

The implementation requests Unreal CoreStyle Regular and Bold. No separate display or monospace family is configured for D08. The menu title uses the local title role; exact values, mode choices, row labels, and the menu key use body. “Arrow length” and “Display” use bold section text. Supporting copy, counts, recovery, and the compact key use label text. These compact sentence-case roles are extracted from this slice, not a replacement for the whole application’s type hierarchy.

The sidecar uses an explicitly approximate browser font fallback for portable component previews. Native capture typography remains authoritative.

D05–D07 reuses the title, body, section, and label roles. Seed-kind headings use the observed seed-kind role; the “Seeds” suffix in a shared list row uses micro. Snapshot annotations request CoreStyle Regular at `max(6, round(10 × s))`, where `s = clamp(min(outputWidth / 800, outputHeight / 500), 0.25, 4)`. This export scale is separate from fixed-size live control typography.

Authoring uses the existing CoreStyle hierarchy and the recurring authoring-title role for Domain, Boundary Conditions and Meshing. Materials has an observed 20-point Bold workspace heading. Exact fields use 10-point type; source, unit, recovery and status text wrap within their owners. Round-trippable numeric strings remain intact in editable fields and value tooltips; horizontal field scrolling accommodates long values.

Monitors uses a 22-point Bold workspace title, 12-point Bold source/series/provenance headings, 10-point controls and wrapping 9-point explanations. The compact source, series/count and scale labels use 8-point type. Main time/value ticks use 9-point type, compact ticks use 8-point, and exact hover values retain 17 significant digits. Time-tick precision increases until adjacent labels differ; measured native text bounds the count before endpoint labels use separate rows at constrained widths. Shared value-axis labels now also increase significant digits until adjacent ticks differ, and their measured native width reserves the left margin in full and compact charts. Probe frame/range controls reuse 10-point type with 9-point wrapping interpretation and recovery; no new type role is introduced.

Activity log reuses CoreStyle with an 18-point Bold title, 10-point controls/messages/details and 9-point row context/counts/export status. Severity is written as Info, Warning or Error beside UTC time and source; color reinforces that text.

Command input reuses 10-point CoreStyle for the field, actions and wrapping response, with a bold 10-point label and muted 9-point keyboard/suggestion text. Result and Sent responses use Cyan; Cannot send and oversized-input recovery use Amber.

Performance reuses CoreStyle with a 15-point Bold title, 11-point Bold section headings, 9-point label/value rows and actions, and 8-point chart/source/status text. Muted labels align with Text values; Cyan traces use visible units and measured time spans.

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

Run parameters leads Setup in one aligned column: next-run context, maximum steps, optional physical seconds, output interval, checkpoint request and interval, Apply/Revert, inline recovery, case Undo/Redo, then backend qualification. Labels and fields use the existing 10-point role; wrapping context and recovery use 9-point type. Labels sit 5 units above inputs, fields have 10 units below, and the equal-width Apply/Revert actions share a 6-unit gap. The whole new form is visible at both reviewed sizes; later Setup sections remain within the retained category scroll container. These are evidence sizes, not new breakpoints.

Materials uses an 18-unit workspace inset, a 210-unit list, a flexible retained properties form and a 280-unit assignments column. List-to-form and form-to-assignments gaps are 16 and 18 units. Domain and Meshing place a flexible 3D preview beside one 334-unit inspector, separated by 6 units; inspector padding is 14 units with instant focus scrolling and 12-unit navigation padding. Their header uses 14 × 10 horizontal/vertical padding and a 6-unit scene gap. The containment or classification strip remains adjacent to its preview.

Boundary Conditions uses a 14-unit workspace inset, a 196-unit bounded target list, a flexible preview and one 310-unit inspector, with 12-unit gutters. Its Select/Orbit, Fit and projection actions are local preview controls. Lists and inspectors scroll independently; a clipped row at a scroll boundary is ordinary retained form content. The two accepted window sizes are evidence sizes, not new breakpoints. The authoring camera supports arbitrary observation independently of the saved Solve camera and recorded frame.

Geometry keeps its 190-unit object list with 12-unit inset, flexible applied-mesh preview and 304-unit inspector, separated by 6-unit gutters. The retained inspector has 14-unit padding, instant focus scrolling and 10-unit navigation padding. Object name precedes Position, Rotation and Scale groups; each group has three equal-width fields with 6-unit gaps, labels 4 units above inputs and 12 units below the group. The source-axis/origin explanation sits above the adjacent Apply/Revert actions. Wrapped recovery and source diagnostics remain in the same scroll container; removal recovery appears above the disabled retained form. The two reviewed window sizes are evidence sizes, not new layout breakpoints.

Monitors uses an 18-unit workspace inset with the title/actions above the source/sample count and export status. A flexible left chart has 12-unit panel padding; the right source inspector is 326 units wide, separated by a 14-unit gutter, with 14-unit padding and instant focus scrolling with 12-unit navigation padding. Expand / Restore collapses only this source inspector and retains chart controls and selections. This local in-memory expansion differs from the existing saved Solve expansion preference. The compact Solve force card keeps source identity, a single-series picker/count and adjacent Linear/Log label above the chart. The reviewed 1320 × 740 and 1280 × 720 windows are evidence sizes, not breakpoints.

M01 keeps this chart/inspector arrangement and gives Choose history a 440-unit menu with a 10-unit inset. Solve’s residual and force cards remain adjacent. Below an 820-unit workspace height, the monitor row is 225 units with loaded residual history and 195 units without it. At 820 units or above, it is 239 units. The residual card adds an 8-point source label, shown/selected count and scale above the plot, a wrapping trace legend, and an 8-point `i`/`f` definition below it. Full legends spell out first initial / last final; compact legends abbreviate them and ellipsize long names to their measured cell width.

M03 retains the Monitors chart and 326-unit source inspector. Choose history now bounds its existing 440-unit-wide menu to a 440-unit maximum desired height with a scrolling list. Saved probes appear under “Saved probes · current recording.” First/Last frame controls occupy 140-unit fields above Generate history; the exact selected-frame summary and Show frame in Solve action sit below the chart. Position choices and source provenance stay in the existing inspector scroll. Shared Y-label measurement can expand the chart’s left margin; chart reduction uses the remaining plot width. The two recorded desktop sizes establish this local layout, not new breakpoints.

Activity log uses the existing Solve content area with 18-unit horizontal and 16-unit vertical padding. Title, Pause view / Follow latest and Restore Solve lead; search/source/severity form one row, followed by project/run scope and clear/recovery/export actions. The virtualized list fills remaining height; selected details occupy a 108-unit read-only field above counts and export feedback. Source and severity menus are 164 and 170 units wide. Alternating Background/Panel rows use 10 × 7-unit padding, Raised hover/selection and a fine Cyan focus outline. The compact card shows the latest three visible entries with a separate Open log action. Accepted window sizes remain evidence sizes, not breakpoints or a new navigation surface.

The command block sits 12 units below the log list and optional selected details, above the counts/export feedback. A flexible field precedes Send and Commands… with 8-unit gaps; keyboard guidance stays beside the label. Suggestions wrap below the field, and responses scroll within a 100-unit maximum desired height. The menu requests 450 units of width and a 360-unit maximum desired height with a scrolling list. These local dimensions reuse the existing native layout and add no breakpoint or workspace route.

Performance occupies the existing 322-unit right inspector with a 12-unit inset. Its title/Close row and Live or Paused status/Pause or Resume row remain fixed above the scrolling Application, Flow rendering and Solver job sections. Charts are 76 units high; label/value rows have 3-unit vertical padding, and sections start with 12 units above and 5 below their heading. The active performance inspector remains visible when the Solve viewport is expanded. The reviewed 1280 × 720 and 1320 × 740 sizes establish this local layout only; they add no breakpoint or workspace route.

Results has a 20-unit inset, a 280-unit searchable catalog and flexible source/run details separated by 20 units. Catalog and details scroll independently with instant focus scrolling and 12-unit navigation padding. Recordings / Run history are local content categories; the sidebar remains the workspace navigator. CoreStyle titles, fine borders, Cyan selected labels/focus and full-opacity search hints reuse the incumbent theme. Metadata uses a 145-unit label column; the one-based frame field is 100 units wide. Both reviewed desktop sizes retain the same arrangement.

Comparison reuses the 20-unit Results inset with equal A/B source selectors and equal flexible scene columns separated by 16 units. Three equal columns group time alignment, frame matching and supplied scalar above a 96-unit Frame A field; conditional offset/tolerance fields are 116 units. Each scene keeps its source identity above and color legend/source-range/topology below. Source, camera and save/library menus use a 410-unit width, a 360-unit maximum desired height and scrolling content. Save comparison… and Saved comparisons sit beside Back to Results in the header; the library keeps name, A/B source/frame metadata and local actions together. CoreStyle title/body/metadata roles are 18-point bold, 10-point and 9-point; source titles use the incumbent 11-point bold role. The layout is retained at both evidenced sizes, which are acceptance windows rather than new breakpoints.

The shared root Export button directly opens a 430-unit form with a 14-unit inset and a 610-unit maximum desired height. Source/frame-or-sequence/topology stay fixed above a scrolling options body; destination/save, progress/cancel and wrapped status/recovery/reveal stay fixed below it. VTK/CSV format and displayed/range/all scope precede the scalar list, which has its own 125-unit maximum desired height. Two equal coordinate choices retain a 5-unit gap, and sequence scope exposes a new-folder name. Both scroll regions use instant focus scrolling. The incumbent 12-point title, 10-point source/body and 9-point metadata roles carry the hierarchy. Both accepted desktop sizes retain this composition; they establish acceptance windows rather than new breakpoints or persistent navigation. Evaluated-output context hides original range/array controls and instead shows the frozen scalar/method; probe output fixes the format to CSV. Unavailable context says No exportable evaluation with recovery and no frozen-result claim.

Post-Processing has a 16-unit inset and a 300-unit operation editor separated from flexible output by 14 units. The header keeps Evaluate and pending Cancel above saved/new/manage controls and local Undo/Redo. The editor scrolls independently; source/frame and ordered operations precede the parameter form. Source/scalar identity leads the output, while the color legend, numerical method and evaluated counts remain adjacent below. Probe tables scroll within the output column. CoreStyle uses an 18-point workspace title, 12-point output scalar, 11-point source/editor titles, 10-point controls and 9-point metadata. Popup contents use a 12-unit inset and 420-unit maximum height, with 350-unit default width, 410-unit saved/new menus, 380-unit scalar menus and 440-unit provenance. Both evidenced desktop sizes retain this layout; no new breakpoint or visual tokens are introduced.

## Elevation & Depth

Native panels are flat. Tonal steps and one-unit control outlines provide containment; the vector extension defines no custom shadow or animated transition. The black scene ground and 3D field supply depth independently of the blue-black UI surfaces. Native popup/window behavior remains supplied by Slate.

Streamline controls use the same flat treatment and define no additional shadow or transition vocabulary. Scene seed labels use opaque Panel backing. Draft markers and lines have a dark outline behind Amber to separate them from the scientific field.

Authoring adds no custom shadow or motion vocabulary. Applied geometry and selected faces provide scene depth; an Amber draft outline overlays the applied Domain only when renderable. Compact status backing, bounded lists and scrollable inspectors retain the same flat tonal layers.

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
| Setup | Next-run maximum steps/optional physical seconds, output interval and checkpoint request/interval with Apply/Revert and case history; replay/control-harness toolbar choice; read-only current recording and applicable reconstruction attachment; source sample/frame/duration disclosures, replay speed/loop and playback status. Recording selection, import, Locate and source references belong to Results. Reconstruction display toggles belong to Display. |
| Physics | Read-only source conditions and coordinates, separately labeled saved-case backend, collision, turbulence and thermal settings. Missing source conditions say “Not supplied” or “See source reference”; saved choices do not alter recorded physics. |
| BCs | Read-only saved-case condition count, six domain-face assignments and imported surface-patch assignments/empty state. Recording boundary conditions are explicitly unavailable from this adapter. |
| Display | The single visible scalar selector and palette/range action, domain grid, applicable cut-plane/flow-layer controls, source-point/surface/volume presentation, and adjacent streamline/vector settings. The viewport toolbar retains camera and triangle-topology actions. |

Physics and BCs are a readable foundation, not completed editable authoring forms; full M3 editing remains open. The captured BGK and velocity-inlet values are explicit saved-case fixture choices, not newly computed CFD or supplied recording conditions. Category buttons dismiss open menus, select the category and save the session without changing scientific state. The current workflow has exactly one visible scalar selector in Display, none in Setup, and no recording selector in Setup. Results owns the recording catalog; Dashboard summarizes the saved-run count.

Camera placement takes precedence in the shared right column, then active Inspection, then the selected Solve category. Active Inspection replaces the category strip; Close restores the chosen category. This ownership uses the existing shared inspector rather than adding a second persistent panel. Expansion retains active Inspection/placement, with the idle-category behavior specified under Layout.

### Results recording catalog and immutable runs

`SStudioResultsWorkspace.cpp/.h` owns the recording catalog, source details and run inspection; its six-line direction contract keeps original CFD distinct from saved control configurations. Recordings searches included/external names, IDs and paths. Run history filters saved records. Empty filtering explains the missing match while retaining the active details. Selection/import/Locate retains the current source until verification succeeds; the footer's Cancel recording action discards a pending replacement. Solve keeps read-only source information, replay and reconstruction settings; Dashboard keeps a run-count summary.

Recording details show original dimensions/connectivity, frame count, source-time range/note and supplied scalar ranges, units and origins. Source identity includes an HTTPS reference action, read-only copyable location/hashes, display translation and explicit reconstruction status. One-based Frame and the slider review original snapshots while retaining the independent playback cursor; source step/time remain adjacent. Inspect in Solve retains the camera and editable case. Follow playback restores the playback cursor.

Run history is immutable inspection: toolbar controls and field export retain their explicitly named active context. Published/imported records do not borrow case settings; Show recording explicitly opens their source. Configured runs show captured case revision, backend, geometry/material counts, lattice and time/step requests, with control-harness records labeled “no CFD output.” Last-observed lifecycle or its absence remains explicit. `StudioResultsRenderTests.cpp` and `bash Tools/test-results.sh --width 1320 --height 740` cover this bounded flow (also use 1280 × 720); the Overview acceptance record preserves its remaining limits.

### Results original-frame comparison

`SStudioComparisonWorkspace.cpp/.h` owns local **Compare recordings** and **Back to Results** actions. A starts from the active Solve recording/frame; B and the time alignment require explicit selection. Same supplied scalar IDs and known matching units govern the scalar menu. **Frame A** is a one-based ordinal with original step/time beside it. **Compare frames** explicitly submits the read, and invalidated settings clear the old views. Recorded timestamps compare numbers without establishing a shared clock; elapsed alignment subtracts each source's first timestamp; manual offset follows `B source time + offset = A source time`. Exact matching uses equal represented times; nearest matching uses an explicit tolerance, selects the earlier frame on a tie and stays inside B's aligned range. The notice preserves aligned A/B times and signed B − A seconds.

Each equal scene retains original frame, source step/time, dimension, physical-unit legend, actual source range and original-points/source-triangles/explicit-reconstruction meaning. **Shared color range** defaults to the union of source extrema; each-source mapping remains available, and legend labels follow the presented mapping. Fit and Camera belong to each side. Orbit, pan, right-drag free flight, numeric XYZ/pitch/yaw/roll and perspective/orthographic projection remain independent. Cameras survive frame/alignment changes and reset on successful source replacement for that side. Main Solve source, playback, camera, frame cursor, scalar, case and toolbar context remain separate; saved-collection edits only change that collection and project dirty state, while the footer names the Solve recording affected by playback/export.

One source-read, pair or saved-restore task runs at a time. Cancellation and identity changes discard stale/partial results; both scenes become visible together after both original frames are current. An ordinary comparison setting change clears the obsolete pair. Opening a saved comparison retains the current pair until both originals validate; failure or cancellation keeps it, and an initially empty view stays empty. The amber no-overlap notice retains its reason and instructs the user to choose a compatible Frame A, recording or time alignment, then select Compare frames again. Hidden Results stops comparison captures. Closing/replacing views drains workers and releases snapshot/model ownership, meshes, render targets and textures before garbage collection; UObject retirement remains deferred. `StudioSnapshotSource.cpp/.h` provides the immutable, original-frame renderer adapter without additional source IO or playback advancement.

**Save comparison…** uses the incumbent focused name field and local popup. It captures the two exact source/frame/scalar/unit/reconstruction identities, each side’s own external source/reconstruction references, alignment and matching settings, both independent cameras and shared/source range choice. The popup distinguishes saving the configuration into project state from toolbar Save writing the project file. Names are 1–120 characters on one line, unique without regard to case, with up to 64 entries. Rejected names retain the focused draft and amber recovery copy.

**Saved comparisons** shows each name with A/B source and original frame metadata, then Open, Update, Rename… and Delete, followed by Undo comparisons/Redo. Update freezes the current pair under the existing stable ID and name. Collection undo/redo and Delete leave the displayed pair alone. Open validates both sources, reconstructions, exact frames and units asynchronously before adopting either scene and restoring both cameras. Recovery directs the user to the original data or a new comparison; current matching recording references may repair moved paths but never replace the saved field identity. Schema 20 keeps these paths portable. The opened view, pending workers and collection edit history remain session state.

`StudioSavedComparison.cpp/.h` and `StudioModelComparisons.cpp` own immutable definitions, transactional restore and the separate bounded collection history. Snapshot models neither run the solver nor invent solver metrics. `StudioComparisonExport.cpp/.h` now provides a frozen original-pair export backend: VTK multiblock or paired CSV, explicit time/scalar/source/camera/range metadata, bounded staging and exclusive directory publication with cancellation. Root Export integration remains pending, so the visible comparison workspace continues to use the named Solve source for root export. Core `45f7337` has 259 successful model cases (three existing helper warning cases) and independent VTK/CSV verification of 16 bundles and 3,766,532 exact original point rows in `tmp/analysis/comparison-export-20260928/`; this establishes no new appearance, native-input or long-session acceptance. No temporal interpolation/extrapolation, field subtraction, unit conversion, inferred geometry registration or fabricated field data is provided. `StudioComparison.cpp/.h`, `StudioComparisonRenderTests.cpp`, `StudioScene.cpp` and `StudioWorkspace.cpp` ground alignment, lifecycle and native interaction behavior. Reproduce the saved-library case with `bash Tools/test-saved-comparisons.sh --width 1320 --height 740` or `--width 1280 --height 720`; `Tools/test-comparison.sh` retains the original comparison case at the same sizes. `StudioSavedComparisonRenderTests.cpp` covers the native save/library workflow; the Overview preserves package, input and stability limits.

### Post-Processing ordered operations and output ownership

The sidebar opens `SStudioPipelineWorkspace`. Saved/new/manage menus and collection Undo/Redo belong to that workspace; root project Save persists schema-21 recipes and their cameras. New pipelines pin the current Solve frame for its source or the first frame of another original recording. The one-based Frame input remains separate from original step/time. Source… exposes copyable source/reconstruction hashes, location, dimension and scene offset; Results retains import/Locate ownership. Toolbar playback keeps its named Solve source; root Export freezes evaluated output while Post-Processing is active and original Solve data elsewhere.

Numbered rows expose enable state, operation name, type and actual scalar ID/unit. An untouched default scalar name follows field changes; custom aliases are preserved. Reordering follows the validated field and geometry dependencies; a magnitude may follow a clip, and a point/line probe terminates the sequence. Interpolation-dependent controls are unavailable without suitable source topology or verified reconstruction. Parameters use scene meters and exact numeric drafts, with Apply parameters/Enter, Revert and explicit Evaluate. Invalid drafts retain recovery and block project saving/replacement; changes clear stale output. A single asynchronous evaluation exposes Cancel and rejects obsolete project/revision/recipe results. Pending work disables edits; source errors direct recovery to Results.

Scientific output retains original source/frame, scalar units/origin, numerical method, range and primitive counts. Derived magnitudes expose their expression; missing probe samples remain unavailable in tables with scene coordinates and distance. Fit and Camera belong to the pipeline; root Save retains that independent camera. Colors affects only the current evaluation and resets on reevaluation. Empty, loading and invalid states name the next action without inventing geometry. The shared root Export freezes completed evaluated output, including current recipe/name/camera presentation, with no project-schema change. Source authority is `SStudioPipelineWorkspace.cpp/.h`, its root integration in `StudioWorkspace.cpp/.h` and `StudioPipelineWorkspaceTests.cpp`; the Overview review and final native evidence retain their separate scopes. Existing preview snippets remain appearance references and supply no native pipeline behavior.

### Retained editor drafts and identity

Vector and streamline popovers retain the same editor objects when dismissed and reopened after category changes. Rejected text and its single wrapped Amber recovery remain visible while the previous applied setting stays active. The reviewed examples preserve vector Scale `nan` and fractional seed count `1.5`; repairing Scale to `1.625` applies on Enter, survives exact project save/reopen, and follows shared undo/redo. Focus loss does not apply a draft. Valid edits and external view revisions synchronize the vector/streamline fields, including focused fields.

**The Draft Identity Rule.** Retain unfinished display-editor text only within its project, solver instance and selected scalar; discard cached forms when any of those identities changes.

`CachedDisplayMenu` caches Color, Volume, Vector and Streamline editors. A project-ID, solver-instance or scalar-ID change clears the cache; workspace tick releases obsolete closures even before another editor opens. Color and Volume additionally rebuild after a render-intent revision because their older forms own local range drafts. Vector and Streamline editors handle revisions within their validated fields. Retained form scrolling drops a queued focus target when it no longer belongs to the reopened form, keeping the restored editor usable. These rules describe transient UI ownership, not serialization of unfinished text into the scientific project.

### Inspector preference and evidence boundary

`InspectorTab` defaults to Display (3), with Setup (0), Physics (1), and BCs (2). `StudioSession.json` stores `inspectorTab`; only finite whole numbers from 0 through 3 are accepted. Missing or malformed values retain the Display default. It is a session-only preference, independent of the schema-16 scientific project and the shared view-history values. `Studio.Inspector.SessionCategoryIsolation` covers all four fresh-reader choices, malformed values, unchanged serialized scientific document and unchanged render revision, subject to the model timing qualification in Overview.

Source authority is `Source/LBMStudio/StudioWorkspace.cpp` (the six-line contract, `CachedDisplayMenu`, `Settings`, `DisplayTools`, `SRetainedFormScrollBox`, workspace tick and inspector switcher), `StudioWorkspace.h`, `StudioModel.cpp/.h`, `StudioInspectorTests.cpp`, and `StudioInspectorRenderTests.cpp`. The packet, finish review, verification, six native manifests and 16 captures named in Overview bound D11/S01 to this staged foundation. Current recording/reconstruction/job regressions and main integration remain pending. Portable previews express appearance only; they do not supply Slate behavior, scientific validation, persistence or runtime evidence.

### Setup run parameters and retained requests

Setup owns maximum solver steps, optional maximum physical time in seconds, output interval in solver steps, the scheduled-checkpoint request and its interval. Counts accept exact whole-number digits from 1 through 1,000,000,000,000. Physical time accepts a finite value above 0 and no larger than 1e12 seconds, or blank for no limit. Loaded seconds use the shortest decimal that round-trips to the stored double; long values remain available through native field scrolling and full-value tooltips. Turning checkpoints off retains the interval, which stays editable and validated. The checkbox says “Request scheduled checkpoints”; its helper states the request or retained-off meaning.

Apply parameters or Enter validates all fields and copies only these five owned settings into one case transaction, preserving unrelated setup edits. Typing and focus loss leave applied data unchanged. A rejected value stays in the field with wrapped Amber recovery and focus on that field. Revert edits loads the latest applied settings. Apply is disabled for a clean or conflicting form; Revert remains available for dirty, invalid or conflicting state. Pending project or recording loading disables the form and its actions.

Retained text survives inspector-category and sidebar round trips within the project/case identity. An external edit to the owned settings preserves dirty text and disables Apply until Revert; unrelated case edits do not conflict. Clean fields synchronize after case Undo/Redo or another applied change. Changing project or case identity resets the form. Applied settings participate in shared case history and schema-17 persistence; unfinished text is transient UI state.

Save, project replacement, duplication and a new control-harness submission expose unresolved parameters in Setup and focus the form. Pending camera placement is preserved and must be resolved before that redirect. Existing harness resume and recorded playback retain their separate behavior. Editing next-run requests leaves the active run's submitted configuration, recording, selected source frame, render intent and observing camera unchanged. The form identifies active-run ownership and states that the disconnected numerical backend/control harness does not enforce limits, write flow output or create scheduled restart files. Backend stopping, disk estimates/output selection and restart compatibility remain unfinished S13–S15 work.

Source authority is `StudioRunSettings.h/.cpp`, `StudioModelRunSettings.cpp`, `StudioWorkspace.cpp` (`RunSettingsControls`, `RefreshRunSettings`, `EnsureRunSettingsResolved`, Header Run and save/replacement guards), `StudioWorkspace.h`, `StudioRunSettingsTests.cpp` and `StudioRunSettingsRenderTests.cpp`. `Tools/test-run-settings.sh [width height]` runs one packaged `Studio.RunSettingsUI.` case, defaulting to 1320 × 740. The package, full bounded form review, prior model timing, four manifests and 16 captures in Overview define this addition's evidence scope. New-run and save behavior have routed assertions; project-replacement wiring was reviewed in source. Reused portable input/action previews describe appearance only.

### Case authoring ownership and exact drafts

**The Applied Case Rule.** Show the applied case independently of retained text, and require Apply before draft values can change the case. Preserve the recording, saved Solve camera and frozen run configurations.

Materials, Domain, Boundary Conditions and Meshing use their existing sidebar routes. Their retained editors survive workspace/selection round trips within the current project. A rejected value remains visible with wrapped Amber recovery; a conflicting draft cannot silently overwrite a later case edit. Revert reloads applied data. Save guards route to unresolved forms for Apply/Revert; they do not silently serialize unfinished text. An applied case may still have unknown or incomplete scientific values and be saved without being declared ready to run. Applied edits use case undo/redo and exact project persistence; observing the authoring scene does not edit Solve's saved camera or scientific arrays.

Materials and Boundary optional inputs and the Boundary filter reuse `SProjectFilterBox` at full hint opacity, with the shared Background/Text field style and Line/Muted/Cyan normal/hover/focus outlines. Empty text remains unknown data. Exact number formatting preserves the shortest decimal that round-trips to the stored double; source bounds and velocity editors retain precision even when native field scrolling is needed. Full-opacity hints are the F3 presentation correction and do not alter parsing, stored values or disabled-state semantics.

### Materials list, properties and assignments

The left list owns Add fluid/Add solid, selection and case history. Selected names use Cyan; rows show applied type, assignment count and an asterisk for unapplied properties. The center owns name, type, density, kinematic viscosity, thermal conductivity and specific heat; the right owns Duplicate, Delete or Unassign and delete, and applied domain/geometry assignments. The empty state offers creation without inventing properties.

Optional properties accept blank for unknown or finite positive values within the document range. Density offers kg/m³ and g/cm³; viscosity offers m²/s and mm²/s; conductivity and specific heat use W/(m·K) and J/(kg·K). Display-unit changes preserve the saved SI value when text is unchanged. Apply properties or Enter commits a valid candidate; focus loss does not apply. Invalid and conflicting drafts remain in their material's editor, with Revert recovery. Assignments use applied properties and stay separate from unfinished property text. Unassign and delete clears current-case references in the same reversible edit; frozen run configurations keep their original material records. Solver-specific validation remains unavailable until an adapter supplies rules.

### Domain bounds, layers and containment

One inspector owns exact meter bounds, dimensions anchored at each minimum, six stable named faces, padding for the next fit, and Apply domain/Revert edits. Selecting a face highlights it in Cyan. Square viewport handles change a retained draft; release leaves it unapplied, while Escape/capture cancellation restores the prior draft. Fit bounds with padding produces draft bounds against complete verified original geometry. Planar geometry requires padding to enclose a 3D volume. Fit view frames the independent authoring camera and does not apply domain edits.

**The Rendered Layers Rule.** Derive both the viewport badge and inspector layer text from the same renderable-draft predicate used by the painter; never claim that invalid or conflicting text has an outline.

| Domain state | Visible layer wording and recovery |
| --- | --- |
| Applied, including outside geometry | “Applied domain”; no amber draft claim. |
| Valid dirty bounds, fit or active/released handle | “Applied domain + amber draft outline”; Apply domain is required to change the case. |
| Invalid retained text | “Applied domain · Invalid draft hidden”; keep text and the applied scene, with error recovery. |
| Conflict after another applied edit | “Applied domain · Conflicting draft hidden”; retain text and require Revert before applying. |
| No available scene preview | “Domain preview unavailable”. |

`FStudioDomainWorkspaceState::CanPreviewDraft`, `SDomainViewport` and the shared `Layers` reader implement F1. Their gate includes current project/workspace identity, loading state, absence of conflict and successful draft construction. Fit and resize notices use the same applied/draft distinction.

**The Containment Evidence Rule.** Report source/aggregate geometry verification separately from whether each object fits inside the applied domain.

Check geometry verifies original file identity, saved surface patches and transformed bounds. Its result explicitly names aggregate geometry bounds. Per-object “Inside the applied domain”, Amber “Outside the applied domain” and Amber unknown containment remain separate. Outside recovery says to enlarge Bounds or use Fit to geometry, then Apply domain; unknown recovery calls for Check geometry or source-file repair in Geometry. Containment is evaluated against applied bounds even while a valid draft outline is visible. Geometry omitted by preview limits still contributes verified original bounds to containment. Verification and enclosure do not attest topology quality, production meshing or solver readiness. Original preview geometry is bounded at 500,000 vertices and 500,000 triangles; it does not infer replacement surfaces.

### Boundary targets, conditions and atomic pairs

The bounded, filterable Faces and patches list uses stable domain-face and imported-patch identities, with 128 targets per page. Selection and original-triangle picking share that identity; the preview highlights the selected original target. Select/Orbit and camera/projection controls remain local to this workspace. No fallback patch geometry is fabricated for unavailable sources, and the list remains available for targets omitted from the bounded scene preview.

The inspector owns condition name/type, exact velocity in m/s, pressure in Pa, optional prescribed temperature in K, Apply condition and Revert edits. Supported document types are Unassigned, Velocity inlet, Pressure outlet, No-slip wall, Slip wall, Symmetry and Periodic pair. A velocity is either all three explicit components, including zeros, or entirely blank/unknown; partial vectors retain their text and show recovery. Thermal intent requires an enabled thermal model and a supporting solver. Visited draft forms remain bound to project/target identity; invalid or externally changed assignments retain text, and a removed target retains its unresolved draft until Revert discards it. Save guards prevent unresolved edits from being lost.

A periodic domain face pairs with its opposite face. Both reciprocal assignments apply atomically in one case edit; a new pair requires the opposite face to be unassigned. Removing a periodic assignment removes its partner. Changing to a different condition exposes Amber “Unpair and apply” with the partner-removal consequence beside it. Rejection leaves both applied assignments intact. Applied coverage shows configured/total targets and bounded missing/incomplete/conflict diagnostics, while “Solver compatibility” remains explicitly not verified. A complete configuration count is not solver validation. Dedicated captures establish inlet, partial-vector, selected-patch and periodic appearance; pressure, thermal and removed-target behavior is supported by source and recorded assertions rather than separate captures in this packet.

### Physical lattice counts and bounded occupancy preview

Meshing owns X/Y/Z cell counts, exact requested maximum spacing, Apply counts/Revert edits and one preview inspector. Counts divide the applied domain into physical cells. Calculate draft counts uses a ceiling per axis so derived spacing is no larger than requested, and leaves Apply counts as the commit point. Invalid or conflicting counts stay in the form while the scene uses the applied case; preview requests are unavailable until the count draft is resolved.

Preview region offers one original X/Y/Z cell layer or Whole, a layer index and a maximum displayed-cell budget. The UI bounds layer previews to 32,768 cells and whole-domain previews to 4,096. Deterministic sampling retains original cell indices and physical extents; it does not replace skipped cells with larger cells. The 3% display gap is visual spacing only. The badge identifies the applied lattice and region; counts distinguish displayed from candidate cells and “sampled” from “complete region”. Changing region/budget requires Preview cells to update the view.

Classification distinguishes Outside geometry, Inside closed geometry, Surface overlap and Unknown, with a text key alongside swatches. It uses verified original triangles; surface intersection and closed-surface evidence have separate meanings. Open, missing, omitted or ambiguous geometry retains unknown occupancy where classification cannot be established. A verified original bound does not substitute for omitted triangle geometry. Progress states name geometry indexing and cells classified; Cancel/Clear removes pending or presented preview, and project/domain/geometry identity prevents stale results from publishing.

The independent authoring camera, Fit view and perspective/orthographic control remain available for observation. Backend lattice rules, memory estimates, refinement and production cell flags are explicitly not supplied. This CPU geometric occupancy aid does not generate a production mesh, compute CFD, establish numerical compatibility or validate the custom LBM solver.

### Authoring source and evidence boundary

Source authority is `Source/LBMStudio/StudioWorkspace.cpp` (the Materials, Domain, Boundaries and Lattice contracts, retained forms, save guards and full-opacity hints), `StudioModelMaterials.cpp`, `StudioModelDomain.cpp`, `StudioModelBoundaries.cpp`, `StudioModelLattice.cpp`, `StudioMaterials.*`, `StudioDomain.*`, `StudioBoundaries.*`, `StudioBoundarySelection.*`, `StudioLattice.*`, `StudioLatticePreview.cpp` and `StudioScene.*`. The package, source delta, two verification files, review and 34 captures in Overview bound this addition. Existing Solve authority and prior review limits remain unchanged; the F1–F3 ship verdict and these portable appearance snippets do not establish fresh main integration or full M3 completion.

### Geometry object transforms and retained recovery

The existing right inspector owns Object name, Position (m), Rotation (°) and Scale · source axes. Position is a case-coordinate offset; Roll · X, Pitch · Y and Yaw · Z expose the object's rotation in degrees. Original coordinates are converted by the retained import units, scaled along original mesh axes, rotated about the original source origin, then translated. The pivot is not the mesh center. This editor changes name and transform only; source bytes/hash, import units, patch identities and material assignments retain their ownership.

Exact field formatting uses the shortest decimal that round-trips to the stored double. Long values remain in the native scrolling field and full-value tooltip. Rotation is stored as a quaternion: unchanged Euler values, including equivalent text formatting, preserve that quaternion exactly. Changed angles produce a normalized quaternion and may display a different equivalent Euler representation after Apply. Names require 1–120 characters and are trimmed when applied. Position accepts finite values within ±1e8 meters, rotation within ±360000 degrees, and scale finite positive factors up to 1e8; transformed bounds must also stay within the supported coordinate range. Invalid text stays visible with Amber recovery and focus on the invalid field.

Drafts remain with their project/object across workspace and object switching. The list adds an asterisk for dirty or conflicting forms; the viewport says “Applied object · unapplied edits not shown.” Apply changes or Enter commits the valid name and transform in one case undo step after the selected original mesh is verified; typing and focus loss leave applied data unchanged. Revert edits reloads current applied values. An external change to the edited object retains local text and disables Apply until Revert; byte-identical relocation and material assignment do not conflict. Save/replacement guards return to the unresolved form, including when another object's source is being read. Applied transforms persist exactly, with recording/frame, saved Solve camera and frozen run configuration isolation.

**The Available Recovery Rule.** When an object is removed, retain its unapplied text, state that no mesh is being previewed for it, and direct saving to the enabled Discard action. Never instruct the user to use disabled Apply/Revert controls.

“Discard removed object's edits” sits above the retained disabled form. Save redirection focuses that action; discarding clears the matching save guard without restoring the object. Existing-object conflicts keep their Revert recovery. Cyan focus/selection, Amber recovery and the existing Background/Text inputs carry these states without new colors, shadows or motion.

Source authority is `StudioGeometryEdit.h/.cpp`, `StudioModelGeometry.cpp`, the contract/editor/guards in `StudioWorkspace.cpp`, and `StudioGeometryEditRenderTests.cpp`. `Tools/test-geometry-edit.sh [width height]` runs the single packaged `Studio.GeometryEditUI.` case, defaulting to 1320 × 740. The final package, two cases and F1-only verdict in Overview bound this addition; native routed input does not prove physical OS input or complete sequential Tab/screen-reader behavior. Portable snippets describe appearance only.

### Published-history Monitors and shared Solve preview

**The Independent History Rule.** Keep each history's verified source, original solver time, units and normalization visible. An independent run never acquires the active field recording's time, physics or residuals.

The installed Nalu-Wind NACA 0021 history supplies 6,967 rows over 0.4004–3.1868 seconds. Its declared field association is empty. Source force components use N, moments use N m, and Y+ extrema and CL/CD use unit 1. The supplied coefficient derivations are CL = (Fpy + Fvy) / 6000 and CD = (Fpx + Fvx) / 6000, with density 1.2 kg/m³, freestream speed 50 m/s and area 4 m². The scrollable inspector retains time interpretation, sorted reference values, selected-column origin/expressions, limitations and Open published source. This force source supplies no residuals; the separate residual card accepts an original completed log as described below.

Choose history opens the installed catalog alongside the saved residual source, residual import/Locate actions and saved probes from the current recording. For the installed force family, loading verifies integrity asynchronously; Cancel loading retains the previous selection, and Remove clears its project reference while preserving source files. Probe generation and removal have the session ownership described below. Stale results cannot take ownership after project replacement, and same-project reopen invalidates the previous loaded history before verification. Missing or changed interpretation reports recovery instead of silently accepting another source. The initial CL/CD selection comes from declared columns. Each axis accepts one unit: unlike-unit checkboxes disable until Clear series or individual removal empties the selection. No-series, no-original-samples and no-positive-log-samples states remain explicit.

The full chart plots original time and selected values. Pixel buckets retain original first/minimum/maximum/last samples in temporal order; reduction does not smooth, interpolate or shift time. Log scale plots positive values with visible gaps at nonpositive samples and an explanatory note. Main labels name Value (unit) and linear/logarithmic scale. Hover uses the nearest original source row and lists exact time and trace values; the cyan cursor marks that row. The main chart supports wheel zoom around the pointer, left-drag pan, focused +/− zoom about the center, ←/→ pan by 10% of the current span, and Home / Fit time to restore the original extent. The compact chart shares data/settings and hover semantics; time navigation belongs to the full chart.

**The Named Compact Trace Rule.** In the force preview, draw only the named preview series; keep its ID, unit, position/count and Linear/Log scale visible while preserving the full project selection.

The native compact picker exposes all project-selected series and is enabled when more than one is selected. Examples are `CL (1) · 1 of 2` and `Fpy (N) · 2 of 6`. Selecting a preview series narrows a copy of chart settings only; it cannot replace the saved selection or hide those series in the full Monitors chart. If the local choice is absent from the current selection, the preview uses its first selected series. The scale tooltip explains nonpositive omission and broken traces. Adaptive original-time ticks remain distinct in full and compact views, including the reviewed 1.7900–1.7908-second window. Source identity and independent-history labeling remain above this compact representation.

Introduced in schema 18, the force `monitor` object saves `historyId`, `metadataSHA256`, `series`, `logY`, `manualTime`, `timeMinimum` and `timeMaximum`. Older documents start with no selected monitor source. Source identity and equal-unit membership are validated when data becomes available. Monitors expansion and the preview-series choice live in the workspace session only; neither is scientific project content or serialized to `StudioSession.json`. History/chart changes retain the recorded flow, camera and active control-run identity.

Export history freezes immutable history, selected series and time settings before the native destination panel opens. The UTF-8 CSV emits every original row in the inclusive selected window, with zero-based `source_sample`, `solver_time (s)` and selected series IDs. `%.17g` preserves round-trippable times and values. Comment records carry history ID/title/source URL, metadata/payload/original-source hashes, declared field association, time note, each column's label/unit/origin/expression and sorted normalization references. Log gaps and display reduction never alter exported values or row membership. A single bounded background task writes atomically, rejects output above 32 Mi-characters, and reports original-row count, frozen series and filename. Empty series/windows produce explicit errors; export feedback is accepted only for its owning project and loaded history. The header Export form separately owns original field data in VTK or CSV.

Source authority is `StudioMonitor.h/.cpp`, `StudioModelMonitor.cpp`, `StudioMonitorChart.h/.cpp`, `StudioMonitorExport.h/.cpp`, the monitor fields in `StudioProject.h/.cpp`, and `Monitors`, `MonitorWorkspace` and `RefreshMonitors` in `StudioWorkspace.cpp`. `Tools/test-monitors.sh [width height]` runs one packaged `Studio.MonitorUI.` case, default 1320 × 740. Recorded evidence and the F1–F3 verdict in Overview bound this addition. Existing portable snippets remain appearance previews; they supply no native chart, history, export or acceptance behavior.

### Original residual histories and independent previews

**The Original Residual Rule.** Keep the original time, first-initial/last-final selection, source lines and shown/selected trace count explicit. A residual plot does not establish convergence or acquire another run’s time or normalization.

Choose history offers installed force histories, the saved `Residuals · filename`, Import residual log… and Locate exact residual source…. Import and Locate use the native Choose Residual Log panel, which selects one file with any extension and remembers its security-scoped access; the reader validates content. The saved-source item remains available when that source is missing. The menu disables during either family’s load or project opening. Cancel loading, Remove, Clear series, scale, Fit time and Export history route to the active family.

Residuals use original `Time =` values with axis units `source units`, because the log declares no physical time unit. Values have unit `reported residual`. Each field offers first initial from its first solve and last final from its last solve in that time block. The initial selection uses first-initial series on a logarithmic axis. The inspector states that no convergence threshold is assumed and retains the original path, SHA-256, time interpretation, selected-column definitions and limitations. Inner solves remain in the original log; no absent value or normalization is invented.

The large chart plots all selected series, labels first initial / last final, and keeps the existing original-time navigation, extrema-preserving reduction and explicit log gaps. Source-backed hover text includes the zero-based original sample, exact time, one-based `Time line`, and each drawn value’s one-based `log line`. Source and values use the same verified original row; logarithmic omission changes only drawing. The shared chart implementation supplies this tooltip behavior; the scoped captures do not establish physical hover or tooltip appearance.

Solve displays the residual and force cards together. The residual card draws the first three selected series in selection order, or fewer when selected, and names its independent source, shown/selected count, Linear/Log scale and original time units. Its legend uses `i` and `f`, defined visibly as first initial and last final. The force card retains its single-series picker. Neither compact representation changes either family’s full selection. With no residual source, Solve says Not supplied and names import recovery; during loading it reports verification, and a saved unavailable source reports Source unavailable with the retained error/recovery notice.

Schema 19 stores the residual path and chart settings in a separate `residual` object beside the existing force `monitor` object. Each family retains its selected source, series, scale and automatic/manual time range across save/reopen; Save As, duplication and recovery preserve portable residual references. Version 18 projects start with no residual source. Active family, Monitors expansion and the force preview choice remain session state. On a new project session, the workspace selects force when a force reference exists, otherwise the saved residual family. Import/Locate failure and cancellation retain the previous residual selection and settings; Locate verifies the exact saved source/interpretation. A missing log leaves the rest of the project usable. Remove clears only the active family’s selection and retains source files.

Export history freezes the active family’s verified source and selected chart settings before the destination panel. Residual CSV adds `time_source_line` and one `.source_line` column per selected series, with one-based original-log numbering, the source path, source/interpretation hashes and the selection definition. It includes every original row in the inclusive selected window with round-trippable values, including zeros hidden on a log axis. Force export semantics remain as recorded above.

Source authority is `StudioResiduals.h/.cpp`, `StudioModelResidual.cpp`, `StudioProject.h/.cpp`, `StudioMonitorExport.cpp`, `StudioMonitorChart.cpp`, the active-family helpers/Monitors/MonitorWorkspace/RefreshMonitors in `StudioWorkspace.cpp`, and `Mac/StudioFileDialog.mm`. The M01 record in Overview bounds acceptance. The published acceptance log remains external, with no redistribution grant claimed; no residual source is installed by default.

### Recorded probe histories, exact frame links and frozen export

**The Recorded Probe Identity Rule.** Keep the selected scalar, original frame ordinal, source step/time, position and sampling status attached to every probe value. Rebuild explicitly after the project, recording, saved probe or frame range changes; never substitute missing values or publish a partial history.

Choose history lists saved point and line probes bound to the current original recording. Exact original-point probes retain the supplied point ID; spatial probes use the recording’s available interpolation with its source/reconstruction attribution. Selecting a probe freezes its fixed field, or the current scalar when its field is automatic, and exposes one-based inclusive First frame / Last frame controls. Selection alone performs no history read. Generate history validates the range and explicit limits of 100,000 frames and 1,000,000 position samples; an over-limit request requires a smaller range or fewer saved line samples and never silently decimates. A single owned worker reads one immutable scalar field at a time. Cancel loading signals cancellation, drains the worker and publishes no partial history. Read/identity failures discard partial results and show local recovery.

Every saved position is available in the series list; at most 16 positions are selected together. The shared linear/log chart, time-window controls, Fit time and Expand / Restore retain source time and units. Pixel reduction preserves extrema for display while immutable samples remain complete. NaN is only an internal chart gap marker for unavailable spatial samples; empty/no-positive views and sample statuses explain the absence, without invented values or temporal interpolation. Adaptive Y-tick precision and native label measurement keep distinct measured values legible under zoom.

Hover or focused ↑/↓ selects an exact sampled frame. The footer shows its one-based UI frame, original source step and physical time. Show frame in Solve, or Enter on the selected chart sample, scrubs to that original recording ordinal and selects the saved probe in Inspection while retaining the case and camera. History generation and chart inspection remain independent of playback; only the explicit frame link changes the reviewed frame.

Derived history, range, position selection and chart settings are session-only. Saved probe definitions already persist through the existing project schema. Project/source/probe changes invalidate the result, chart selection, frame link and export eligibility; range changes clear the plot and require Generate history. Remove clears derived selection/history and retains the saved probe. Workspace navigation and camera/playback changes do not invalidate a matching request.

Export history freezes immutable source, selected positions and time window before opening the destination panel. Probe CSV is long-form: one row per selected position per included original frame. It retains zero-based `frame_ordinal` and `sample_index`, `source_step`, `time_s`, line distance, scene/source coordinates, original point IDs, explicit statuses and round-trippable values. Missing values have blank cells. Comments record project/probe/source/reconstruction/scalar identity, scalar origin/units, source dimensions, sampling method and saved line endpoints. Log omissions and chart reduction never alter the exported values; the 32 Mi-character bound reports how to narrow the window or selection.

Source authority is `StudioProbeMonitor.h/.cpp`, `StudioProbeHistory.h/.cpp`, `StudioMonitorChart.h/.cpp`, `StudioMonitorExport.cpp` and the source/range/export/frame-link ownership in `StudioWorkspace.cpp`. `StudioProbeMonitorTests.cpp` and `StudioProbeMonitorRenderTests.cpp` establish the model and routed native cases. `Tools/test-probe-history.sh` runs one packaged `Studio.ProbeMonitorUI.` case; use `--width 1280 --height 720` for the minimum target size. The M03 and F1 evidence boundaries in Overview apply; this extension introduces no numerical live-solver metrics or new portable preview primitives.

### Original-field VTK/CSV export and task ownership

Outside Post-Processing, the single root **Export** button opens original Solve data in the shared **VTK XML** / **CSV table** form; the intermediate VTK menu and legacy synchronous CSV action are removed. Opening the idle form captures the presented original field for **Displayed frame** and names its source, one-based frozen frame, original step/time and original triangles or points. A point-only source explicitly says it has no source mesh. **Frame range** accepts one-based inclusive First frame / Last frame inputs; **All frames** selects the original recording. Sequence requests pin their source and preserve exact original ordinals, steps and times without interpolation. The fixed source header reports the count and step/time span while options scroll.

The displayed scalar is selected initially. Checkboxes and All/None select arrays with supplied units and declared derived labels; unknown units stay unknown. Empty selection disables saving and says **Select at least one scalar array.** Invalid fractional/out-of-range frame inputs and invalid folder names also disable saving with specific recovery. **Source XYZ** is the default. **Scene XZY** applies the display axis mapping plus offset to point coordinates in meters while scalar components retain their original source basis. Selection uses Cyan, helper text uses Muted, and errors use wrapping Amber text beside the recovery action. CSV explains its leading metadata comment; VTK explains original points and supplied mesh without display reconstruction or extrusion.

**Choose destination and save…** opens the native macOS file panel for one `.vtp` or `.csv`. Range/all exposes **New export folder name** and **Choose parent folder and export…**. The native folder picker selects a parent; the sequence publishes together as a new named directory through an exclusive atomic operation. An existing directory is never replaced, and its recovery asks for a new name. CSV sequences contain per-frame files plus `frames.csv`; VTK sequences contain per-frame VTP files plus relative `flow.pvd` references and original times. Filenames and index ordinals use zero-based original ordinals despite the one-based input labels.

The root owns one asynchronous task and no queue across popup dismissal. Saving pins the whole request before menus close or native destination selection begins, so camera and replay/review remain independent. The header says **Exporting…** while pending; reopening shows the same task, locks draft controls and exposes progress with **Cancel export** until publication begins. The fixed footer keeps status, recovery and **Show saved export in Finder** available as appropriate. Errors retain retry through the destination action; cancelled tasks restore editable controls. Reopening while idle refreshes the current-frame draft while retaining format, arrays, coordinates, scope, range and folder name for the same source. Saved feedback retains its own result identity. A new source/project clears idle draft/result ownership; a pending task retains its frozen context, and completion logs only to its owning project. This remains session state with no schema change.

VTK preserves original IDs, points, selected scalar values and supplied triangles; point-only sources receive vertex cells. CSV retains every original point row and begins with `# LBMStudioMetadataUTF8 ` plus source/array/coordinate JSON; table readers skip that comment. Both preserve source hashes, original ordinal/step/time, units, scalar origins/expressions and coordinate processing without reconstructed display grids, extrusion, clipping or resampling. Private staging is cleaned on every outcome. Limits remain 1–64 arrays, 4 million points, 8 million triangles and 512 MiB per frame, with at most 100,000 frames in a sequence. Additional scalar reads must match frozen identity and row order. Native single-file publication maps the completed file and does not establish constant RSS.

Source authority is `StudioFieldExportUI.h/.cpp`, `StudioFieldExportTask.h/.cpp`, `StudioFieldSequence.h/.cpp`, `StudioVTKExport.h/.cpp`, `StudioCSVExport.h/.cpp`, `StudioWorkspace.cpp::ExportMenu` and the native `StudioFileDialog` implementation. `StudioFieldExportRenderTests.cpp` and `Tools/test-field-export.sh --width 1320 --height 740` cover the bounded native workflow; also use 1280 × 720, serially. `Tools/verify_export_range_ui.py --exports <run>/captures --report <report.json>` independently checks eight UI field files and both sequence indices. The current and historical Overview records retain their separate acceptance boundaries; existing portable previews supply no native export behavior.

### Evaluated pipeline export through the shared owner

Post-Processing routes root Export through `SStudioPipelineWorkspace::ExportSnapshot`. A completed, applied and current evaluation freezes its output with the current recipe/name/camera, original frame/step/time, scalar units/origin/expression, numerical method and source/reconstruction hashes before native destination selection. Presentation-only copies preserve metadata without recomputing numerical values. Missing, busy, stale or unapplied results show **No exportable evaluation** and specific recovery; they do not fall through to Solve. A completed empty output remains a valid export.

Geometry offers **VTK XML** for evaluated points/lines/triangles or **CSV table** for evaluated vertex rows without connectivity. Probe tables require CSV, retaining all requested rows, blank unavailable values and status. **Scene XZY** is the pipeline default; **Source XYZ** remains available, with scalar meaning preserved. The established fixed source and save/status/recovery regions surround the scrolling options. Original-field scope/array controls are hidden for evaluated output; its scalar meaning and numerical method remain explicit.

One root task retains its own context when workspace/project changes and keeps **Cancel export** available until publication. Progress, retry and saved-export reveal use the existing shared controls. Idle snapshots release when menus close or the project changes; completed closed-menu tasks release retained ownership. Post-Processing's footer says that Export saves its evaluated output while toolbar playback uses Solve. There is no schema change. Source authority is `StudioFieldExportUI.cpp/.h`, `SStudioPipelineWorkspace.cpp/.h`, `StudioWorkspace.cpp::ExportMenu` and the pipeline writer/shared publisher. The local-menu review, final readback and earlier original-export regression retain the distinct boundaries in Overview.

### Activity log observations and frozen export

**The Observation Identity Rule.** Keep UTC application observation time, severity and source explicit. A log entry never acquires solver time, scientific-history meaning or an unrelated project/run identity.

Open log expands locally from the Solve activity card; Restore Solve returns keyboard focus to that action and retains the case, camera, selected frame and render intent. Expanded log hides flow rendering while playback remains independent. Source choices are Application, Playback and Control harness; severity choices are All severities, Warnings + errors and Errors only. A case-insensitive literal search is bounded to 256 characters. All session projects clears and disables Current control run; changing project resets filters, following and expansion.

Pause view freezes a bounded snapshot while collection continues, including when filters change. Follow latest resumes from the retained journal. Clear view hides sequences through the captured boundary without deleting journal entries; Show retained removes that boundary within the current snapshot. Visible/retained/dropped and paused new-event counts keep the limit visible. The journal retains 2,048 entries, 2,048-character messages and 256-character references, with explicit truncation. All log state is session only; no schema or durable-history claim is added.

Rows show a single-line message preview; selection exposes the complete retained message and ISO UTC/project/run/source context in a selectable, wrapping, read-only field. Export CSV… copies the visible filtered snapshot before the destination picker and performs one atomic background write. CSV columns are `observation_sequence`, `observed_at_utc`, `severity`, `source`, `project_id`, `run_id`, `source_reference`, `truncated` and `message`; quoting retains multiline content and sequence order survives wall-clock changes. Empty/busy export is disabled, clear/no-match states name recovery, and export feedback reports entry count and filename for its owning project.

Source authority is `StudioLog.h/.cpp`, `StudioLogExport.h/.cpp`, `StudioModelJobs.cpp`, the log card/`ActivityLogPanel`/`RefreshActivityLog`/`ExpandActivityLog` in `StudioWorkspace.cpp`, and the hidden-flow gate in `StudioScene.cpp`. `Tools/test-activity-log.sh [width height]` runs one packaged `Studio.LogUI.` case. The recorded scope in Overview bounds acceptance; existing portable snippets provide no native log behavior.

### Application command input and local responses

**The Explicit Command Target Rule.** Keep replay and control-harness command names explicit, revalidate mode/state/capabilities at every send, and distinguish a sent job request from its observed acknowledgement.

The registry exposes 19 exact, argument-free names through help, completion and Commands…. Menu selection and Tab/Shift+Tab fill text without execution; Enter or Send dispatches. Up/Down recalls at most 64 recognized commands, collapsing consecutive duplicates and excluding unsupported text. Case and ordinary spaces normalize; unsupported names, tabs/newlines and arguments are rejected without shell or engine-console fallback. The 256-character input limit shortens oversized input and blocks sending until an actual edit or replacement; unchanged recall retains the guard.

Success clears input; errors retain it with wrapped local recovery. Responses remain beside the field when journal pause/filters hide their copied application entries. Job responses say Sent and direct the user to status or following log entries. Project identity changes clear draft, suggestions and response while session recall persists; recalled commands must pass current validation. Pause/resume remain literal. Project saving uses the existing native workflow, view fit/undo/redo respect pending placement, and job submit shares the Run button's retained-parameter guard. These session controls add no schema or numerical-solver behavior.

Source authority is `StudioCommands.h/.cpp`, `SStudioCommandInput.h/.cpp`, shared styles declared in `StudioTheme.h` and the `StudioWorkspace.cpp` handlers. `Tools/test-commands.sh [width height]` runs the packaged `Studio.CommandUI.` case. Native implementation and recorded evidence define behavior; existing portable snippets retain their original scope.

### Solve performance readings and frozen inspection

**View performance** in Control status opens the existing right inspector and transfers focus after layout. Its action is disabled during camera or inspection placement. The header remains visible while application, flow and solver details scroll. Escape or Close restores the prior settings category and returns focus to View performance; Home/End and PageUp/PageDown navigate the focused inspector. Opening and closing retain authored case, source, selected frame and camera. The sidebar remains the sole workspace navigator.

The Application section reports this LBM Studio process: macOS `phys_footprint` in MiB (1,048,576 bytes), user-plus-system CPU usage with **100% per core**, mean root-workspace tick cadence in ms, and mean CPU update work in ms. Update work excludes the measurement read, Slate paint and GPU rendering. Platform reads occur at most once per second and retain **120 samples**. Invalid observations are rejected; a tick gap above two seconds resets rate baselines. Missing counters remain unavailable. Cyan cadence and footprint charts show their units and actual elapsed span within the latest 120 seconds; missing values and gaps above 2.5 seconds break the trace.

Flow rendering reports on-demand scene captures per second, the last CPU capture-submit duration and presented field-build duration in ms, active geometry-worker count, allocated procedural mesh buffers in MiB, and engine-reported render-target/scalar-texture allocations in MiB. The last field build retains its presented source and frame identity. A stationary scene may correctly show zero captures per second. CPU submit/build durations do not measure GPU execution; buffer/texture allocations do not establish total process memory or device residency. The render device is named and **GPU time / utilization: unavailable** remains explicit.

**Pause readings** copies the retained application history, solver telemetry view and source state, and labels the frozen display `Paused · HH:MM:SS UTC`. Camera interaction and collection remain independent; Resume readings returns to the current retained history, including measurements collected while the display was frozen. Pausing readings does not issue playback or job commands. The panel and its histories are session state; this feature adds no project schema.

Solver job values come only from the separate `StudioJobTelemetry` view, with producer/run/host/device/sample age attribution when supplied. Physical time, wall time and ETA use seconds, throughput uses steps per second, progress/utilization use percent, and resource memory uses MiB. Current, stale, disconnected and final states have explicit labels. In the available control-harness state, the source says **No solver measurements. Recorded playback is independent.** Absent metrics remain **Unavailable**, and stop-limit progress is **Indeterminate**. Playback percentage, requested case limits, command acknowledgements and application memory cannot become measured solver progress or resources. Future numeric solver states are wired and model-tested but are outside this native visual acceptance.

Source authority is `StudioPerformance.h/.cpp`, `SStudioPerformancePanel.h/.cpp`, the timing/resource hooks in `StudioScene.h/.cpp`, and the root sampler/inspector ownership in `StudioWorkspace.cpp`. `Tools/test-performance.sh [width height]` runs the packaged `ScientificAcceptance.PerformanceUI.MeasurePauseCameraAndRestore` case. The recorded evidence and input/replay limits in Overview bound its acceptance; existing portable snippets do not simulate native performance measurements.

### Flow overview and movable viewport panels

The Solve viewport retains the incumbent navy palette and native Slate controls. Flow overview fits the complete displayed domain, simplifies the airfoil view to its original-mesh slice plus integrated streamlines, and applies a clearly labeled custom scalar range. Original visible nodes determine the range with 15% padding and a deterministic 50,000-node sampling limit. Speed is derived after interpolation of original velocity components in the pixel shader, matching field probes. Source data, topology, exports, selected frame and the existing palette stay intact. The operation belongs to view history; saved cameras remain freely editable.

The airfoil slice uses its exact original 2D triangles, clipped to the display bounds, at 0.18 opacity; sources above 131,072 triangles retain bounded grid sampling. Streamline endpoint colors follow their actual samples. A camera-aligned navy backdrop and subdued domain edges establish depth without lighting scalar colors. Solve captures at twice its Slate dimensions, capped at 3840 × 2400; captures remain on demand. This is a presentation of published 2D CFD, not newly simulated spanwise flow or the mockup's illustrated wake.

**Display > Streamline settings > Direction markers** uses the existing checkbox treatment. Equal-size cones with six sides follow original velocity sampled along integrated traces, including backward traces; physical flow determines their orientation, and the sampled selected scalar determines their color. Size communicates direction only. Markers use equal arc-length spacing (7.5% of the longest domain side), with staggered seed rows, at most 32 candidates per path and 2,048 emitted markers overall. Flow overview enables them. The optional `directionMarkers` view property defaults to false in older documents and participates in shared view undo/redo. Toggling it retains the camera and selected frame.

Playback, Tools, Axes, Color scale and View are independent floating panels. Compact title handles drag; minus hides the body and plus restores it. Clicking raises the panel. Arrow keys move a focused handle by 10 Slate units, or 1 with Shift. Escape and capture loss cancel a tentative drag. The View body owns contextual Pan/Zoom/Fly/Inspect/placement hints. Tools presents Orbit, Pan, Zoom and Fly with labels at larger heights; below 820 Slate units of workspace height, the modes use two columns of icons with tooltips. Selected icons are Cyan and inactive icons Muted in both layouts, keeping the lower-left legend clear.

`StudioWorkspace.cpp`'s `ViewTools` and `SFlowViewport` own navigation. Left-drag latches the selected tool; middle-drag pan and right-drag look remain available. Pan translates camera position and focus together. Zoom drags up to zoom in and down to zoom out, changing orthographic width or perspective distance. Pan/Zoom selection is local and unpersisted; outside Fly it does not edit the camera. Entering or leaving Fly changes only the saved, undoable `bFreeCamera` flag, preserving pose, focus and display settings.

Positions use fractions of available panel travel and clamp to the viewport during resize, minimize and restore. Only committed positions enter `StudioSession.json`; these preferences stay outside project serialization and view history and do not invalidate CFD rendering. Display's Reset panels restores all default positions and open bodies. Inspection-label layout reserves actual panel rectangles, not the transparent floating layer.

Source authority: `StudioFlowPresentation`, `StudioStreamlines`, `SStudioFloatingLayer`, `StudioFloatingPanes`, `StudioScene`, the Solve center and streamline settings in `StudioWorkspace`, and `StudioModel` session serialization. `StudioFlowPresentationTests.cpp`, `StudioFlowPresentationRenderTests.cpp` and `StudioStreamlineTests.cpp` cover marker direction/scalar sampling, bounded geometry, persistence and older-document defaults. Model and packaged tests also cover range/source isolation, original-scalar transport, view undo/save, pointer minimize, keyboard restore, dragging and clamping, canceled drag persistence, reset, and camera/frame isolation. The supplied dataset determines the flow's shape and color distribution; screenshot fidelity does not authorize invented CFD values.

## Do's and Don'ts

### Do:

- Do preserve one root Export owner, explicit original/evaluated identity, unavailable-evaluation recovery and task-owned context with Cancel across workspace/project changes.
- Do retain the pipeline's actual field/unit, original source/frame and numerical method beside its ordered operations and output; separate Apply, Evaluate and project Save ownership.
- Do keep frozen frame or original sequence identity, array units/origins and the source-basis rule visible through export; retain fixed source/action regions, one pending task and explicit recovery.
- Do retain explicit A/B time interpretation, original frame identity, physical-unit legends and independent cameras when comparing recordings.
- Do keep sample limits, length mode, exact scale, and the current-frame key together in the vector popover.
- Do identify original-row sampling, zero or missing samples, and equal-length direction-only meaning.
- Do retain selected-scalar coloring independently of velocity direction and length.
- Do preserve rejected drafts, Enter-to-apply behavior, and synchronization after valid edits or view restore.
- Do keep this record and its acceptance claims scoped to D05–D08, V10/D01, the D11/S01 foundation, the authoring supplement, Geometry object editing, Setup run parameters, published-history Monitors, M01 residual histories, the M05 Activity log and M06 command-input additions, retaining each review’s finding and package boundaries.
- Do retain exact seed coordinates, physical units, full tube diameter, and atomic placement history.
- Do label aggregate counts “All seed sets:” and preserve instantaneous/derived/work-limit meaning in frozen annotated exports.
- Do retain a single bottom view-control owner and keep saved-camera management distinct from the active viewport camera.
- Do preserve original/derived topology, triangle count and hidden-fill meaning in the presented legend and frozen PNG metadata.
- Do retain active inspection and placement when expanding, and persist expansion separately from the case and camera.

- Do preserve session-only inspector selection and source-bound draft ownership while category changes leave scientific state intact.
- Do label Physics and BCs as read-only source/saved-case summaries until editable M3 authoring is implemented and accepted.

- Do keep blank authoring values unknown, exact SI storage intact, and Apply/Revert/save guards adjacent to their retained forms.
- Do make Domain layer labels follow renderability, with source-bound verification and applied-domain containment reported separately.
- Do preserve stable boundary identities, atomic periodic pairs and sampled original-cell meaning.
- Do preserve exact Geometry fields, original-axis scale, source-origin rotation and the applied mesh while drafts remain unresolved.
- Do give removed Geometry drafts an enabled Discard recovery and focus that action when saving redirects to it.
- Do retain exact next-run requests, explicit Apply/Revert, conflict recovery and shared case history while the active run keeps its submitted settings.

- Do preserve original history rows/time/units, one-unit axis selection, visible log gaps and frozen exact-row CSV provenance.
- Do name the compact force trace, unit, position/count and scale while retaining the complete project selection.
- Do keep residual and force sources/settings independent, with residual source lines, first-initial/last-final meaning and the compact shown/selected count visible.

- Do keep log observation/source context, paused snapshot identity, reversible clear and frozen CSV provenance visible within the existing Solve owner.

- Do keep command discovery and recall separate from sending, preserve recoverable input, and show job dispatch separately from observed state.

- Do keep performance units, measurement ownership, missing values and frozen UTC state explicit while the flow remains available in the existing Solve inspector.
- Do keep recorded probe generation explicit, source/frame/scalar identity intact and gaps visible through chart inspection, exact-frame return and frozen CSV export.

### Don't:

- Don’t present stale/unapplied pipeline output as frozen, fall through to Solve from an unavailable pipeline export, or infer popup reliability from diagnostic lookup changes and passing final runs.
- Don’t treat the PP-F01 label verdict or final passing native pair as resolution of intermittent menu failures, physical input/accessibility acceptance, pipeline-output export or release acceptance.
- Don’t substitute reconstruction, interpolation or resampling for original export rows, replace an existing sequence directory, or treat routed VTK/CSV controls and parsed PVD files as physical picker/Finder, full accessibility, ParaView playback or release acceptance.
- Don’t treat equal scalar IDs/units or aligned timestamps as shared geometry, a shared physical clock, a derived difference field or permission to substitute changed data when reopening a saved comparison.
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
- Don’t treat a visible draft, verified aggregate bounds or complete coverage count as an applied change, enclosed geometry or solver compatibility.
- Don’t substitute larger cells or invented triangles for omitted preview data, or equate geometric occupancy with production meshing.
- Don’t treat F1–F3 acceptance or the earlier 161 model cases as a fresh main build/model result, physical OS input, full M3, solver validation or whole-product approval.
- Don’t treat the Geometry F1-only verdict or its two final native cases as full Geometry, solver readiness, physical OS input, broad accessibility, long-session or whole-product acceptance.
- Don’t describe retained run requests or their scoped review as enforced stopping, generated flow/restart files, full S13–S15 or whole-product acceptance.
- Don’t synchronize independent Nalu-Wind history to SU2 playback, invent residuals, or export reduced/log-filtered rows as the original history.
- Don’t treat the Monitors F1–F3 correction verdict as physical hover/dialog acceptance, a final-package model rerun, current long-session/release acceptance or completion of live backend/history tooling.
- Don’t infer residual convergence, physical time units or redistribution rights, or treat the M01 ship verdict and native picker observations as broad physical-input, stability or full-plan acceptance.
- Don’t treat session observations or the M05 log verdict as scientific history, live solver transport, physical input/clipboard/dialog acceptance or long-session/full-plan completion.
- Don’t bypass command mode/state or shared save/run guards, or treat M06 acceptance as real backend transport, physical input or full-plan completion.
- Don’t treat application CPU timings or allocation bytes as GPU workload/residency, or extend the performance verdict to active-replay freeze testing, full keyboard/accessibility, long-session or full-plan acceptance.
- Don’t treat a generated probe history as persistent project data, fill spatial gaps, silently decimate a request, or extend the F1-only verdict to physical input, native pickers, long-session/release or full-plan acceptance.
