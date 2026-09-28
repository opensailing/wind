# LBM Solver Studio

<!-- impeccable:product-schema 1 -->

## Platform
Desktop macOS initially; portable Unreal code, Windows verification deferred.

## Stack
User confirmed Unreal Engine 5. Native C++ and Slate UI. Real 3D scene rendering.

## Product Purpose
A UI and CFD visualization interface for a custom lattice Boltzmann solver. The first deliverable is the Solve workspace, with a recorded CFD adapter behind a replaceable solver interface.

## Confirmed Behavior
- Wing section inside a 3D viewing domain; velocity coloring, slices, streamlines and vector arrows. The current source is 2D and its display extrusion is explicit.
- Orbit and unrestricted free-flight cameras. Camera and visualization controls remain live during run, pause and recorded-frame review.
- Replay mode runs, pauses, stops and steps through actual published snapshots. Control harness mode tests job commands independently, saves frozen settings and last-observed lifecycle, and clearly reports that no CFD is computed. Source physics is read only until the custom solver is connected.
- Scrub supplied frames and follow playback.
- Close match to the user-provided dark, dense solver workspace screenshot.
- First verified deliverable is a standalone macOS application.

## Evidence
Two independent SU2 ground-truth trajectories from DeepMind MeshGraphNets, test recordings 009 and 010, are bundled with original data, metadata, checksums and provenance. Each contains 601 snapshots covering 0.120 seconds of source time; they are separate 2D recordings, not consecutive segments or 3D data. Default playback of either recording lasts 30 seconds, with speed control, looping and immediate full-recording scrubbing. Missing residual and force history is visibly unavailable. No LBM run or measured GPU workload is represented.

The external NACA 0018 Fluent recording adds 8,000 original snapshots over 19.9975 seconds of physical evolution. Project/source integration, scalar selection, original-point/vector display and full playback have packaged evidence at two window sizes. Its source supplies points without a mesh or solid boundary. An explicitly attached, verified reconstruction now supports a continuous 2D scalar surface while preserving the original point values; the derived topology and inferred solid boundary are labeled. Surface/point controls, import/locate/reopen/repair/removal and independent GPU scalar checks have packaged evidence at both sizes. The reconstruction-control review is closed at its two-size scope. Full-sequence surface transport, playback and bounded resources pass at both target sizes on package `ee5bac47…`; native folder access and current hour-long stability remain open. Slices/streamlines for this source, genuine spanwise fields and full reference fidelity remain unfinished. The earlier original-point field-control review remains closed at its scope.

## Open Decisions
The production solver transport and API will be specified when available. Initial STL/OBJ geometry import and verified external 2D recording selection/relocation exist. Full geometry editing, domain editing, boundary-condition authoring, broader scientific formats and other workspaces remain in progress. Native recording-folder access across relaunch remains unaccepted. A pinned point-source package passed a 60-minute mixed-use baseline; later builds and the final two-hour release gate have separate acceptance. Scalar palettes/ranges and exact v8 persistence have a closed scoped review and packaged tests at both target sizes.
