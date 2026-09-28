# LBM Solver Studio

A fresh Unreal Engine 5 Solve workspace for a custom lattice Boltzmann solver. The current backend replays **two published SU2 trajectories with 601 snapshots each** from DeepMind's published [MeshGraphNets Airfoil dataset](https://github.com/google-deepmind/deepmind-research/tree/master/meshgraphnets), test trajectories 009 and 010. It also imports the verified external NACA 0018 point recording: 8,000 original snapshots covering 19.9975 seconds of physical evolution. The custom lattice Boltzmann solver is not connected yet.

## Run

On the development Mac:

```bash
Tools/build.sh
Tools/run.sh
```

The first command verifies the pinned sample checksums, imports its original mesh and fields, compiles C++, and creates the runtime flow materials. The second opens the application in Unreal's standalone game mode. Set `UE_ENGINE_PATH` if the engine is installed elsewhere.

After packaging, `Tools/run.sh --packaged` opens the packaged application through the same launcher. The launcher prevents overlapping Studio/Unreal sessions, preserves arguments and exit status, and closes its own remaining children and crash reporters when the app exits. macOS reporters can detach into a separate process group; ownership is checked using the new reporter's exact project/PID/run-UUID argument and launch time. Existing unrelated reporters are retained. Session evidence is saved under `tmp/debug/launch-*.json`; crash report files are preserved. This launch guard applies to `Tools/run.sh`; Finder launches have separate application-level acceptance pending.

Verify launcher and reporter ownership without opening Unreal:

```sh
PYTHONPATH=Tools python3 -m unittest test_studio_processes test_run_studio test_run_packaged_suite test_stability_driver -v
```

## Development workflow

Commit each completed feature or fix as one logical change, including the tests and documentation needed to understand it. Run the relevant build and checks before committing, review the staged diff, and use a descriptive commit message. Keep unrelated changes in separate commits.

Builds, packaged apps, logs, temporary plans, captures and Python caches stay out of Git. Run Unreal builds and native tests serially through the scripts in `Tools/` so process ownership and cleanup remain controlled.

## Controls

| Action | Control |
| --- | --- |
| Orbit around the wing | Left-drag in the scene |
| Pan | Middle-drag |
| Zoom | Mouse wheel |
| Free flight | Hold right mouse, move with WASD and Q/E; Shift moves faster |
| Persistent free-flight mode | Click Free fly; click the scene to give it keyboard focus |
| Return to the domain | Fit button or F while the scene has focus |
| Exact camera placement | Right panel: X, Y, Z in meters; yaw, pitch, roll in degrees |
| Named camera views | Camera tool → enter a name → Save view; Activate / Update / Copy / Delete; rename inline |
| Standard view directions | Click a cube face/edge/corner, or Views → choose any of 26 signed directions; arrows and Enter/Space operate the focused cube |
| Undo camera/display changes | Camera menu or inspector → Undo view / Redo view; ⌘⌥Z / ⇧⌘⌥Z |
| Project files | Project menu → New/Open/Save/Save As; ⌘N, ⌘O, ⌘S, ⇧⌘S |
| Browse local projects | Projects in the sidebar; filter recent files, favorite, duplicate or remove from recents |
| Overview and recording access | Dashboard; Open Solve returns to the retained scene and camera |
| More viewport space | Collapse/expand at the bottom of the sidebar |
| Choose or import recordings | Right inspector → Recorded dataset → choose an included source or Import recording…; choose a folder containing `recording.json` and its referenced arrays (v3), or `flow.bin` plus `recording.json` (v2) |
| Field colors | Floating Display panel → settings beside the scalar → palette and source/custom range; Apply range commits bounds |
| Choose point-source fields | Floating Display panel → scalar selector; source points, point size and supplied vectors have independent controls |
| Attach a reconstructed surface | Recorded dataset → Surface reconstruction… → Import surface…; choose a verified reconstruction folder |
| Switch surface/points | Floating Display panel → Reconstructed surface; original-point controls return when it is unchecked |
| Repair a moved recording | Recorded dataset → Locate…, or Locate recording… after a project-open failure; original source hashes must match |
| Review recorded frames | Timeline; Follow replay selects the latest played snapshot |
| Choose toolbar behavior | Inspector → Toolbar controls → Replay or Control harness |
| Edit next-run requests | Solve inspector → Setup → Run parameters; Apply parameters / Revert edits, Undo case / Redo case |
| Exercise job control | Control harness → Run, Pause/Resume, Step, Stop; Checkpoint test and Reconnect in the inspector |
| Exercise failures/completion | Control harness → Test events → Finish control test, Simulate disconnect or Simulate failure |

In Replay mode, Run plays each bundled 601-snapshot recording at 20 snapshots/second: **30 seconds of playback** at 1×. Replay speed ranges from 0.25× to 4×; optional looping restarts the same recording. The timeline exposes every frame immediately. Pause retains the current frame; Step advances one recorded snapshot; Run after reviewing starts from that selection. Follow replay returns to the playback cursor. Camera and supported display controls remain interactive throughout. The 8,000-frame NACA source plays for 399.95 seconds at 1× (about 100 seconds at 4×); its 19.9975-second source duration is shown separately.

Selecting Control harness explicitly sets the case's backend to the deterministic development adapter. Run freezes the current case settings and creates a separate control-run record. The toolbar then operates that job; the inspector reports acknowledgements and offers reconnect after a simulated connection loss. Control Step does not advance the recorded field, and Checkpoint test creates no restart file. The viewport always retains its published source identity. Stop must be acknowledged before switching control mode, replacing the project or closing the window. Saving remains available. Reopened run history reports the last observed state; it never recreates or pretends to reconnect an old in-memory job.

**Setup → Run parameters** owns maximum solver steps, optional maximum physical time in seconds, output interval in solver steps, and a scheduled-checkpoint request with its retained interval. Step counts accept whole-number digits from 1 through 1,000,000,000,000; physical time accepts finite seconds above 0 and up to 1e12, or blank for no time limit. Checkpoint scheduling can be off while its interval remains editable and validated.

**Apply parameters** or Enter applies all valid requests together as one case edit; typing and focus loss leave the applied case unchanged. **Revert edits** restores applied values. Exact text survives inspector-category and sidebar navigation; a conflicting change to these parameters retains the draft and requires Revert. Unrelated case edits are preserved. Undo case / Redo case share the case history, and applied values save/reopen under the existing schema 17. Changing project or case identity resets the form. Save, project replacement, duplication and a new control-harness submission return to unresolved parameters; pending camera placement must be resolved before that redirect. An active run keeps its frozen settings, and published playback, source frame and camera remain independent.

These are stored requests for the next run. **No numerical backend is connected:** the control harness does not enforce these limits, write flow output or create scheduled restart files. Backend stopping, output selection and disk estimates, restart compatibility and the remaining S13–S15 work are still open.

View history retains up to 64 camera/display edits per open document. A drag, flight gesture, wheel burst or display-slider drag is one undo step; numeric fields commit on Enter or focus loss. Undo retains case edits, playback speed/loop settings and frame selection. Restoring a named camera is undoable. History resets when opening, creating, duplicating or recovering a project; the resulting view itself is saved. Orbit and free-look gestures use continuous camera-local rotation through the poles; Q/E flight follows the camera's local down/up directions, including its roll.

Color settings offer Spectrum, Blue–white–red and Grayscale. Range inputs accept finite decimal/scientific numbers; Apply range requires minimum below maximum. Source range restores the descriptor bounds. Values outside custom bounds use the endpoint colors; original values and CSV exports are unchanged. Settings persist separately for each source/field (up to 128 entries) and participate in view undo. The legend follows the mapping actually presented. A source/field change disables an already-open settings panel until it is reopened. Schema v8 stores these settings; older documents retain their source defaults.

An explicitly attached 2D reconstruction can display a continuous scalar surface through the original sample positions. The topology and inferred solid boundary are derived; recorded values stay unchanged and no spanwise field is added. Color mapping follows interpolation, including custom ranges. Surface reconstruction… offers matching-copy Locate and Remove; removal retains both source files and restores original points. Schema v10 retains the representation choice, while older documents default to a surface only when one is explicitly attached. Numerical GPU checks and routed controls have packaged evidence; the scoped two-size visual review is closed. Full 8,000-frame scalar transport and ordinary-clock playback/resource checks pass at both target sizes on package `ee5bac47…`, including normal executable and LaunchServices startup. Native folder access and a current hour-long stability gate remain open. See `tmp/implementation/surface-rendering-work-in-progress.md` and `tmp/implementation/surface-full-sequence-verification.md`.

The camera manager stores up to 128 uniquely named views. Activate restores the exact saved pose; Update replaces that saved pose with the current view; Copy duplicates its stored pose. Renaming and saved-camera edits retain playback, case and active view. Undo saved edit / Redo saved edit provide a separate 64-entry collection history. The collection survives save/reopen; its transient undo history resets with the document.

The orientation cube and Views menu change direction around the existing focus while retaining distance, FOV, projection and orthographic width. Top/bottom views have stable orientation. Direction changes participate in view history and save/reopen while playback continues. Three packaged orientation suites pass at both target sizes on `7db73c54…`; its selected-label/documentation corrections have a closed scoped review. Scene camera/frustum placement, editable clipping and physical OS input acceptance remain unfinished. Evidence: `tmp/implementation/orientation-verification.md`.

Projects use versioned `.lbms` documents in a location chosen through the native macOS file panel. They retain the source frame, visualization settings, exact camera transform/projection, named cameras, name and favorite state. The project menu lists recent files. Opening validates the entire document before replacing current state; New/Open/Close protect unsaved edits. Legacy `Saved/StudioProject.json` preferences can be imported and saved as a new document.

Interactive project opening, session reopen and recovery prepare the document, recording index and saved frame on a background worker. The footer reports the current stage and offers **Cancel opening**; the existing scene, camera and replay remain usable. An authoring edit, rename, saved-camera change or project replacement during loading prevents a late result from overwriting the newer work. Failed or cancelled opens preserve the current document and its recent-file entries. Successful opening restores the saved view and source frame together. The saved camera retains its original values instead of acquiring rounding changes from renderer conversion.

Unsaved changes are copied to `Saved/Recovery/StudioRecovery.lbms` every 30 seconds; the next launch offers Restore or Discard. Saves use macOS coordinated atomic replacement; the previous version is retained under `Saved/ProjectBackups`. Security-scoped bookmarks retain access to user-selected files across launches. These use [Apple's sandbox file-access APIs](https://developer.apple.com/documentation/security/accessing-files-from-the-macos-app-sandbox).

Current version 17 documents store a separate case draft: geometry references/transforms, domain faces, materials, boundary assignments and solver setup. Missing physical values stay unset. Run records preserve their origin and an immutable configuration snapshot when one exists; the published recording has no invented configuration. Job history stores the last observed state, elapsed wall time, acknowledgement counts and status. Older projects migrate without changing their saved camera or recording selection, and existing configuration snapshots gain no invented execution history. The model provides bounded, transactional case undo/redo (64 edits, 16 MB). Structural validation makes drafts safe to save; numerical readiness requires the custom solver's validation rules.

Projects provides a background-loaded recent-file list, name/path filtering, favorites, missing-file location, rename/save/save-as, and independent document duplication. Removing a recent entry leaves files untouched. New and Duplicate commit their destination before replacing the open document, so failed writes preserve current work. Dashboard shows the active recording, source-frame selection, saved run records and session activity. The sidebar is the sole workspace navigator; duplicate horizontal workflow tabs have been removed. Dashboard, Projects, Geometry, Domain, Materials, Boundary Conditions, Meshing and Solve share routing; navigation retains the camera and playback/review cursors. A hidden Solve viewport stops geometry updates and GPU captures while playback continues. Sidebar collapse is saved as an application preference. The remaining workspaces remain visibly unavailable while being implemented.

### Case authoring

- **Geometry:** select an imported object, edit its name, position, rotation and source-axis scale, then choose **Apply changes**. The preview shows the applied object while text is being edited.
- **Materials:** add fluid or solid records, enter known properties with unit conversion, and assign materials to the domain or geometry objects. Blank values remain unknown.
- **Domain:** edit bounds and dimensions, name stable faces, fit verified original geometry with padding, or drag face handles. The applied domain and amber draft outline are labeled separately. Source verification and applied-domain containment have separate results.
- **Boundary Conditions:** filter domain faces and imported patches, select original triangles in the preview, and author velocity, pressure, thermal or reciprocal periodic assignments. Coverage reports missing or inconsistent values; solver compatibility remains unverified.
- **Meshing:** edit lattice counts or derive counts from maximum spacing. Preview a bounded selection of original cells in a layer or the whole domain, with explicit surface/interior/exterior/unknown classifications. This is a geometric preview; backend preparation, refinement, quality and runtime memory rules remain unfinished.

Authoring cameras are independent of the retained Solve camera. Forms retain invalid or conflicting text across navigation; saving returns to unresolved forms for recovery. Applied changes participate in case undo/redo and persistence; existing run snapshots and recorded CFD remain independent.

Geometry references are resolved against their owning project folder. Save As and Duplicate write paths relative to the new destination while retaining the original asset and frozen run identities. Backups and recovery store resolved locations; case undo retains its source references across Save As. An unsaved case must use an absolute geometry source path until it has a project folder. Projects → Geometry files checks referenced files in the background, including sources used only by older runs. Locate accepts a file only when its SHA-256 matches; it updates matching draft/run and undo/redo locations without changing source identity. Cancel leaves references unchanged. Changed geometry requires a separate import. Verification describes the last completed check, not a permanent guarantee against later file edits.

Import Geometry opens a native STL/OBJ picker and the Geometry workspace. The preview has its own orbit/pan/zoom camera; the Solve camera and recorded boundary remain independent. Choose source units (meters, centimeters, millimeters or inches), source up/forward axes and an object name before committing. Import is one undoable case edit; save/reopen retains source hash, units, rotation and patch IDs. Selecting an existing object rereads and verifies its original source. Changed contents are rejected; use Locate for an unchanged original or import changed geometry separately.

To edit an existing object, select it in **Geometry** and wait for its original mesh to verify. Enter position X/Y/Z in case meters, Roll/Pitch/Yaw in degrees, and positive X/Y/Z scale factors. Scale acts along the original mesh axes before rotation; rotation is about the original source origin, followed by the position offset. Import units stay fixed. Exact editable values preserve double precision; long decimals can scroll within the field and appear in its tooltip. An unchanged rotation retains its stored quaternion exactly, including when only the Euler text formatting changes.

**Apply changes** or Enter commits the valid name and transform together as one undoable case edit; typing or moving focus leaves the applied mesh unchanged. **Revert edits** reloads the current applied values. Drafts, including invalid text, survive workspace and object switching within the open project. A conflicting object edit disables Apply until Revert. Saving or replacing the project returns to unresolved edits instead of saving the text. If the object was removed, **Discard removed object's edits** clears its retained draft and save guard without restoring the object; save redirection focuses that enabled action. Save/reopen preserves applied transforms, original source and patch identities, and material assignments. Recorded CFD, the Solve camera/frame and frozen run configurations remain independent.

The initial runtime importer supports ASCII/binary STL and polygon OBJ, including negative indices, vertex/texture/normal face notation, groups and planar concave polygons. Appearance materials are excluded; source normals do not override geometric preview shading. Files are limited to 64 MiB, 500,000 triangles, 1,500,000 vertices and 256 vertices per polygon. Source coordinates are retained. Boundary edges, nonmanifold edges, inconsistent winding and duplicate faces are reported without automatic repair; these diagnostics do not establish solver readiness. OBJ freeform/line/point geometry, weighted/colored vertices and nonplanar polygons require a polygon export or source repair. Large inputs parse on a worker, and obsolete/cancelled results cannot replace the current project. Only the selected case object is previewed in this initial Geometry workspace; general scene editing and meshing remain in progress.

Export CSV writes the selected snapshot's original nodes, derived velocity, pressure and density in source coordinates; Snapshot writes the viewport PNG. Both reveal `Saved/Exports` in Finder.

## CFD sample provenance

- **Source:** Pfaff, Fortunato, Sanchez-Gonzalez and Battaglia, *Learning Mesh-Based Simulation with Graph Networks*, ICLR 2021. The bundled data is SU2 ground truth from the official dataset, not model predictions.
- **Retained originals:** one complete, unmodified TFRecord from test trajectory 009, metadata, associated repository README and license. See [provenance and SHA-256 hashes](Content/Samples/MeshGraphNets_Airfoil/provenance.json). The repository has an Apache-2.0 license; the dataset download has no separate license statement.
- **Content:** 601 snapshots, 5,233 source nodes, 10,216 original triangles. No frames are generated or interpolated in time. Missing residual and force histories show “Not supplied.” Per-trajectory Mach and Reynolds settings were not published.
- **Time:** the distributed metadata specifies 0.0002 seconds between snapshots, giving 0.120 seconds of source elapsed time. The paper appendix lists a different interval (0.008 seconds). This app follows the file metadata and records that discrepancy; playback duration is independent. Absolute simulation start is unavailable.

`Tools/import_airfoil_sample.py` verifies source SHA-256 hashes and TFRecord CRC32C checksums before conversion. The paper defines the Airfoil state as momentum and density, and its visualization footnote divides momentum by density; accordingly, the source vector under the generic key `velocity` is divided by density. Pressure and density remain unchanged. Float32 storage preserves source precision. Original mesh connectivity is retained for barycentric spatial interpolation.

The exact source airfoil boundary and 2D field are extruded into a 3D view. Source `(x,y)` maps to scene `(X=x−0.5, Z=y)`; scene Y has no spanwise CFD variation. Each recording descriptor defines the viewing box; the bundled box is a crop of the larger computational domain. Fit frames that box while preserving orientation, and source switching retains the current camera. A saved slice outside new bounds remains explicit instead of being silently moved. Streamlines integrate the recorded velocity field. The default speed range is 0–400 m/s across all snapshots; per-source/field palette and range settings can override the display mapping. Camera and visualization controls do not change the CFD data. The prior three-frame SU2 tutorial sample is retained separately but is no longer the active or packaged fixture.

## Architecture

- `StudioModel`: solver interface, playback state, immutable frame identities and independent case edit history.
- `StudioProject` / `StudioCase`: versioned documents, stable object references, frozen run configurations, validation, migration, atomic saving and backups. Native Mac file access lives in `Mac/StudioFileDialog.mm`.
- `StudioRecordedSolver`: verified SU2 fixture loader, immutable field snapshots, spatial interpolation and original-node CSV export.
- `StudioPointRecording` / `StudioPointSolver`: checksum-pinned optional source arrays, bounded immutable snapshots, original-point rendering and original-coordinate CSV export; no inferred connectivity.
- `StudioJobs` / `StudioModelJobs`: independent command/event controller, deterministic control harness, explicit toolbar routing and persisted lifecycle summaries. Commands carry run and command identities; replies are ordered and correlated, settings freeze at submission, and acknowledgement/completion timeouts require reconnect before another launch. Stop can interrupt preparation. The harness simulates control acknowledgements only: it produces no CFD, physical time, scientific telemetry or restart files. Active jobs protect project replacement and close.
- `StudioScene`: Unreal scene capture, source-mesh airfoil extrusion, field planes, integrated streamlines and vectors. Geometry builds on one shared flow/preview worker using an immutable field snapshot. Explicit scrub/display/project/source changes cancel obsolete reads and geometry; publication checks project, source instance and render intent. Ordinary playback lets an in-flight frame complete so slow reads cannot starve presentation. Hidden/minimized work drains without GPU upload; camera captures remain independent. The flow texture redraws only when geometry, camera or viewport size changes; the covered game-world view is disabled.
- `StudioWorkspace`: native Slate shell, settings, camera controls, timeline and monitors.

For the legacy SU2 sources, translucent layers repeat the 2D field across the viewing span; this version does not contain a volume ray marcher. The production adapter can implement `IStudioSolver` and `IStudioField` independently of Slate, with live solver configuration added when its API is available.

## Verify and package

```bash
Tools/test.sh
Tools/package.sh
Tools/test-assets.sh       # Save As, duplication, backup/recovery and portable geometry paths in the packaged app
Tools/test-geometry.sh     # Packaged STL/OBJ parsing, diagnostics, import/undo/save/reopen and cancellation
Tools/test-geometry-edit.sh # Retained object edits, Apply/Revert, conflict/removal recovery and exact reopen
Tools/test-geometry-edit.sh 1280 720 # Geometry inspector at the minimum target size
Tools/test-project-loading.sh # Packaged asynchronous open/cancel, recovery, relocation and stale-result checks
Tools/test-recordings.sh   # Packaged external recording integrity, relocation, persistence and recovery
Tools/test-jobs.sh         # Packaged deterministic control lifecycle, timeout/reconnect and event integrity
Tools/test-run-settings.sh # Packaged retained Setup parameters, Apply/Revert, guards and frozen-run isolation
Tools/test-run-settings.sh 1280 720 # Run parameters at the minimum target size
Tools/test-histories.sh    # Published force values, integrity, cancellation and malformed samples
Tools/test-point-recordings.sh # Five reader suites plus three point/project integration suites
Tools/test-colors.sh       # Palette/range controls, original-value preservation and exact persistence
Tools/test-colors.sh 1280 720 # Color controls at the minimum target size
Tools/test-cameras.sh      # Camera collection model and routed GPU interaction/persistence
Tools/test-cameras.sh 1280 720 # Camera acceptance at minimum target size
Tools/test-render-requests.sh # Obsolete request cancellation and slow-reader playback progress
Tools/test-fields.sh       # Source scalars, planar velocities, interpolation and missing-data checks
Tools/test-surface-render.sh # Original scalars, reconstructed-surface GPU colors and v10 persistence
Tools/test-surface-full.sh /path/to/recording.json /path/to/reconstruction.json # Full original sequence and measured surface playback
Tools/test-render.sh       # Real GPU project loading, geometry returns, playback, camera and idle checks (about 80 seconds)
Tools/test-render.sh 2100  # Older mostly-idle scenario; does not replace mixed-use acceptance
Tools/test-stability.sh 30 # Short rehearsal of the mixed-use driver
Tools/test-stability.sh    # 20 min replay + 20 min camera/scrub/source changes + 20 min idle/minimized
```

Sixty-seven headless tests cover original SU2 node values and interpolation, playback lifecycle, immutable history/export, corrupt fixtures, project round trips, failed saves/opens, schema migration, recovery, case references/frozen run settings, bounded case and view undo, gesture grouping, unrestricted camera rotation, navigation, independent duplication, favorites, asynchronous recent-file loading, recording descriptors, cache eviction, integrity checks and transactional recording switching. Seven job-control suites cover frozen configuration, deterministic polling, command/state rules, stop during preparation, rejection, missing acknowledgements, reconnect races, stale/foreign replies and bounded event work/history, model command routing, saved lifecycle migration, and active-job protection during project replacement/close. Four geometry tests also run in the packaged app and cover source formats, concave triangulation, diagnostics, malformed input, units/orientation, import/undo/save/reopen, changed-source rejection and cancelled/stale work. Four project-loading suites cover asynchronous replacement, cancel/stale work, invalid source frames, recent-file relocation, recovery/legacy imports and interruptible recording preparation. Three external-recording suites use copies of published SU2 data to check original values and duration, full-file integrity including changed timestamps, identity conflicts, portable saves, duplicate/recovery, cancellation, stale work, and matching-only relocation. Six asset tests cover Save As, duplication, folder relocation, backup/recovery, streamed SHA-256, background verification, matching-only relink, cancellation/stale results and undo across document moves. Three camera model suites cover stable IDs, exact poses, isolation, bounded history, validation and project lifetime. Three history-reader suites cover independently checked published values, immutable identity, corrupt payloads, cancellation, size limits and malformed data. The additional camera GPU suite exercises save/rename/copy/update/delete/undo/redo/activate and exact reopen at both target sizes. The GPU suite exercises the actual job controls while moving the camera, pause/step/checkpoint/reconnect/stop, saved history, opening while moving the camera, the routed Cancel opening button, loaded camera/frame identity, imported mesh preview, its independent camera, returning to the original recorded view, orthographic zoom, routed Slate pointer/shortcut input, on-demand capture, background playback with no hidden captures, scene resumption and switching published recordings. Build and test evidence is written to `tmp/debug/`. The packaged app is placed under `Packaged/Mac/`.

Close the app before the GPU test. It launches the packaged app, moves its camera, loops the recording at 4×, reads back actual GPU pixels, and checks that an unchanged idle view submits no further 3D captures. Leave its controls untouched during the test. The app closes when the test finishes; its report is in `tmp/debug/render-automation/`.

The Geometry inspector's bounded review is closed by the **F1-only ship verdict** in `tmp/analysis/geometry-edit-20260928/finish-verdict.md`; the original `finish-review.md` remains its finding basis. Final package `dbc10846…` has two clean routed native Geometry cases at 1320 × 740 and 1280 × 720, with 16 captures (`f1-verification.json` in the same folder). The earlier four native cases and 164 successful model cases predate the final recovery copy/focus correction; one of those later model cases contains Unreal's `idevice_id` CPU-architecture helper warning. The separate core run passed all 164 model cases cleanly. This evidence does not establish full Geometry, solver readiness, physical OS input, broad accessibility, long-session stability or whole-product/reference acceptance.

The Setup run-parameter form has a bounded **ship** review with no material fixes in `tmp/analysis/run-settings-20260928/finish-review.md`. Its `ui-acceptance.json` pins package `c83d4a60…`, 16 captures and 10 clean native cases: one RunSettingsUI case at each target size, one Inspector regression and seven Jobs cases. The separate `core-acceptance.json` records 167 successful model cases before the UI work: 166 clean and one with Unreal's bundled `idevice_id` CPU-architecture helper warning. No final-package model rerun is claimed. Earlier native timing failures remain in `ui-acceptance.json`. This evidence covers the form, guarded submission and isolation; it does not close S13–S15, physical OS input, full accessibility, long-session stability or whole-renderer/reference acceptance.

The dedicated mixed-use runner records the exact binary/source hashes, process inventory and ten-second resource samples in a timestamped `tmp/debug/stability-*` directory. It measures macOS physical footprint, cached and pinned frame allocations, reader/worker counts, retained mesh buffers, render-target size and Metal device allocations. Engine RHI object counts may be unavailable on Metal; zero is recorded as unavailable, never as zero GPU use. Short rehearsals validate the test driver and are not hour-long acceptance. The idle phase spends half its time visible and half actually minimized, including background playback; minimized views retain their texture and stop flow builds/captures. The runner restores and reopens the saved inspection state and cleans up only processes it launched. The baseline archived at `tmp/debug/stability-20260927T075811Z/` passed one clean 3,602.227-second suite on binary `fc888c07…`: 40 source switches, 598 scrub/display actions, no idle/minimized captures, exact saved-state restoration, bounded resources and clean shutdown. This evidence applies to that build and machine, does not establish a fix for the preceding system Metal fault, and does not replace the two-hour release gate.

## Reference environment

Unreal 5.8.1, Apple Silicon, macOS 27, Xcode 27. The Apple Metal Toolchain must be installed (`xcodebuild -downloadComponent MetalToolchain`). On this machine UE's `Engine/Config/Apple/Apple_SDK.json` maximum was raised from 26.9.0 to 27.9.0 to accept Xcode 27. This is a local compatibility adjustment, not an assertion of Epic's support. Do not use a per-project SDK override with the installed engine: it can redirect precompiled Mac platform modules into missing project binary paths.

The previous project is preserved locally in `tmp/legacy-recovery-2026-09-26/` and is excluded from builds and version control.

## Recording data

The installed-recordings catalog and versioned descriptors identify each source, coordinate transform, units and frame timestamps. The reader retains mesh metadata and streams immutable frames through an 8 MiB LRU cache. Geometry workers perform frame reads off the UI thread. Active snapshots may retain frames beyond cache ownership; mesh/index and worker memory are additional. Source SHA256/TFRecord checks are verified during import; runtime verifies mesh and individual frame checksums.

The model now supports external `flow.bin` + `recording.json` pairs. Version 8 projects pin SHA-256 hashes of both legacy files; v3 references pin the descriptor and all member hashes it declares, preserve portable paths across Save As/Duplicate, and keep absolute recovery/backup locations. Background import and matching-content relocation preserve the current document on failure or cancellation. Opening a project can verify a replacement recording before committing the loaded document. The Recorded datasets menu lists imported and included sources and offers Import recording…, Locate… and cancellation. A failed project open offers Locate recording… while retaining the current project. Native macOS folder selection grants access to the metadata/payload pair; the routed GPU test injects its folder choice, so actual native picker/bookmark acceptance remains pending. Imported sources retain an explicit imported origin. Display geometry, slice limits and Fit use each descriptor’s bounds; broader field/topology formats remain unfinished.

The recording selector loads a candidate asynchronously and retains the current view until validation succeeds. Cancel interrupts metadata/index construction and chunked frame preparation, discards the pending replacement and drains its one bounded worker. Immutable solver/field/cache snapshots use thread-safe shared ownership across loading and rendering. Projects reopen their saved recording; unavailable data leaves the existing project intact. The viewport reports the source and frame actually presented, while the timeline selects the requested snapshot. Snapshot/CSV actions wait for a matching view by refusing a still-pending frame with an explanatory message.

Both current trajectories cover 0.12 seconds of source time using the distributed metadata. They are independent recordings, not consecutive pieces of a longer simulation. Neither contains residual/force histories or genuine spanwise flow. The external NACA source below supplies longer physical evolution; genuine 3D field support remains unfinished.

## Published scientific histories — integration in progress

`Content/Samples/NaluWind_NACA0021_Re270k_AoA30` retains 6,967 original Nalu-Wind force/moment samples from the [NLR dataset](https://data.nlr.gov/submissions/311), spanning solver time 0.4004–3.1868 s. Its original file is byte-identical to the authors' NACA 0021, 240×257×121 benchmark output. Provenance records the source archive/member hashes, paper, exact benchmark revision, full source notice and coefficient normalization. This is an independent run with no spatial field; it does not supply the SU2 recordings' missing histories or extend their animation.

`python3 Tools/import_nalu_history.py` verifies the originals and emits a separate `history.json`/`history.csv` pair. Original numeric text is retained; CL/CD use the authors' pressure-plus-viscous force calculation and reference denominator of 6,000 N. Four Python tests check every coefficient with independent decimal arithmetic, source preservation, malformed input and failed-import behavior: `python3 -m unittest discover -s Tools -p 'test_import_nalu_history.py' -v`.

`StudioHistory` adds an immutable, bounded reader with explicit units/derivations, independent run identity, hash verification and cancellation. Its three suites passed in the packaged application with no warnings or failures: `tmp/debug/histories-20260927T090205926457Z/`. Run `Tools/test-histories.sh` to verify. Chart controls, saved history selection and run references remain unfinished.

### External OpenFOAM residual extraction

`Tools/extract_openfoam_residuals.py` preserves a completed OpenFOAM log, every reported linear-solve record and its original source line. A separate timestep CSV selects the first initial and last final residual for each requested field, with explicit solve counts. Values and time text are copied from the source; missing fields, malformed numbers, repeated times and incomplete logs fail without publishing a partial result.

```sh
python3 Tools/extract_openfoam_residuals.py /path/to/log.pimpleFoam /path/to/new-output --fields Ux Uy p
python3 -m unittest discover -s Tools -p 'test_extract_openfoam_residuals.py' -v
```

The extraction is an external analysis directory and is not registered as an app sample. Its manifest records source/output hashes and selection rules. Native history import and chart integration remain pending. Acceptance against the original flowTorch cylinder log checked all 291,444 solve records and 20,000 source times independently; evidence is `tmp/analysis/flowtorch/residual-extraction-independent-verification.json`. Redistribution permission for that external dataset remains unverified, so its log and extracted values are not bundled in `Content/Samples`.


## Longer NACA 0018 recording — original-point playback

`Tools/import_naca0018.py` converts the original NACA 0018 Fluent member from [Zenodo record 20582405](https://zenodo.org/records/20582405), with CC-BY-4.0 attribution, into the recording v3 layout. It requires the exact verified original HDF5, pinned author setup README and original record metadata, plus an unused output directory. Python dependencies are NumPy and h5py; they are offline conversion tools, not application dependencies.

The verified original contains 18,706 points and 8,000 snapshots over 19.9975 s of physical evolution. Conversion preserves every float64 field value, original point ID and step label; timestamps use the author's 0.0025 s interval. It stores separately named optional arrays and distinguishes exported speed from the norm of the supplied velocity components. It does not invent density, connectivity, boundary patches, spanwise flow or monitor histories. Unspecified cell-volume units remain unspecified.

The full 4.79 GB external conversion was compared with the original through an independent pandas reader: 748,240,000 field values matched exactly and all 32,000 dynamic component-frame checksums passed. Evidence is under `tmp/analysis/zenodo-airfoil/`.

`StudioPointRecording` now reads v3 source points and optional fields in native C++. It preserves original point IDs, float64 values, scalar/vector meanings and timestamps; verifies all member SHA-256 hashes and per-frame checksums; supports cancellable serialized reads; and returns immutable snapshots. Scalar budgets include cached, pinned and in-flight value storage. Geometry and metadata have separate limits; these counters do not represent total process/GPU memory.

Five packaged reader suites and three project/point integration suites pass (`bash Tools/test-point-recordings.sh`). The explicit full-data gate also streamed all 8,000 original frames, checked 32,000 dynamic checksums and matched 135 independently read HDF5 values. Its report is `tmp/debug/point-recording-full-20260927T093840657550Z/`; the test used an identical copy inside the app sandbox and establishes reader behavior only. `Content/Samples/NACA0018_ReaderFixture` contains three unchanged source snapshots for routine tests, is absent from the installed dataset catalog, and is explicitly not a playback fixture. Regenerate it with `Tools/extract_naca_reader_fixture.py` using the audited originals; its independent golden reader additionally requires pandas/PyTables.

Project v8 references, import/relink, source selection and original-point rendering are implemented. The floating Display panel selects velocity X/Y, pressure, exported speed and exported cell volume with source-specific units/ranges. Point glyphs and supplied vectors can be hidden independently. Vectors use sampled original rows; their tooltip describes the length normalization. The NACA source is shown as original 2D points, with no invented wing boundary, mesh or spanwise field. Slice/streamline tools remain unavailable for this source until validated spatial sampling exists. CSV preserves original point IDs, source coordinates, step/time and all supplied fields.

Packaged ordinary-clock playback traversed the complete recording at 1320×740 and 1280×720 while moving the camera; displayed/exported identity, exact save/reopen and idle capture suppression passed. The renderer may skip intermediate frames to keep up with presentation; the reader gate separately checks every frame. The scoped UI finish review returned ship. Evidence and exact build boundaries: `tmp/implementation/point-recording-integration-verification.md`. Native external-folder access across relaunch remains an acceptance gap. The point-source package `fcf7f330…` subsequently passed a clean 3,608.966-second mixed-use suite: 12 full playback loops, 40 source switches, bounded resources, exact saved-view restoration and no owned processes left running. See `tmp/implementation/point-stability-verification.md`. This is a pinned-package baseline; later palette/range edits and the final two-hour release gate have separate acceptance.

To run the extended mixed-use check with the full audited source, supply its existing descriptor path:

```bash
Tools/test-stability.sh 45 --point-recording /absolute/path/to/recording.json   # Driver rehearsal
Tools/test-stability.sh 1200 --point-recording /absolute/path/to/recording.json # 60-minute gate
```

The runner validates the full descriptor hash, records the source mix and rejects missing point-source coverage. It performs sustained point playback, alternates both legacy sources and the point source, then tests visible idle/minimized playback and exact saved-view reopen. The app sandbox must already have access to the descriptor and members; a test copy inside the sandbox does not establish native picker permission acceptance.
