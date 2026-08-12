# FlowViz

A scientific CFD visualizer built on Unreal Engine 5: it opens `.cfdviz` cases
(a bricked, compressed volume format with a JSON manifest), ray-marches the
fields on the GPU, and gives an operator the controls a ParaView user expects —
transfer functions, slices, clipping, iso-surfaces, glyphs, streamlines, probes
and charts — with every rendering choice disclosed rather than silently
defaulted.

The CFD **solver** is out of scope: representative cases are produced by
external solvers such as FluidX3D or OpenFOAM and visualized here. The bundled
analytic generator exists only for deterministic format and renderer tests.

## Requirements

| Component | Tested version |
| --- | --- |
| Unreal Engine | 5.8.1 at `/Users/Shared/Epic Games/UE_5.8` |
| macOS | Darwin 25.x, Apple Silicon |
| Python | 3.12+ with `numpy` (for the data tools only) |

See [Docs/BUILD.md](Docs/BUILD.md) for the full reference environment and
troubleshooting; the commands below are the short path.

## Generate or install the representative sample

The primary demo and performance fixture is
`Samples/FluidX3DSphereWake.cfdviz`: a genuine 192×96×96, 40-frame, fully 3D
sphere wake computed by FluidX3D. Generate it in an isolated clone of the pinned
solver revision:

```bash
Tools/generate_fluidx3d_sample.sh
```

That command requires an existing FluidX3D Git checkout. By default it uses the
sibling path `../FluidX3D`; otherwise pass `--source /absolute/path/to/FluidX3D`.
It builds the solver in an isolated clone, converts its velocity/flags sequences,
validates known values, qualifies the case as representative external-solver
data, and creates a checksum-pinned release archive with the required altered-
source bundle. If that archive already exists locally, install and re-qualify it
with:

```bash
Tools/download_demo_case.sh \
    --archive Samples/FluidX3DSphereWake.cfdviz.release.tar.gz
```

The descriptor's public URL is intentionally unset pending FluidX3D license/use
review, so a fresh checkout cannot download this case automatically. See
[Tools/fluidx3d/README.md](Tools/fluidx3d/README.md).

`Samples/MockCylinderWake.cfdviz` remains committed as a small deterministic
correctness fixture. It is an analytic construction, not CFD solver output:

```bash
cd Tools/cfdviz
PYTHONPATH=src python3 -m cfdviz generate-mock --output ../../Samples/MockCylinderWake.cfdviz --low-res
PYTHONPATH=src python3 -m cfdviz validate ../../Samples/MockCylinderWake.cfdviz
```

Python-side tests:

```bash
cd Tools/cfdviz && python3 -m pytest tests -q
```

## Build

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
    FlowVizEditor Mac Development -project="$PWD/FlowViz.uproject"
```

Success is the literal line `Result: Succeeded` — **the exit code lies** (a
known UBT wrapper behaviour, documented in Docs/BUILD.md).

## Launch

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
    "$PWD/FlowViz.uproject"
```

In the editor: Window → FlowViz to open the workspace tab, then
`FlowViz.LoadCase "Samples/MockCylinderWake.cfdviz" U` in the console. The
console commands (`FlowViz.LoadCase`, `FlowViz.ReloadCase`,
`FlowViz.ShowDiagnostics`, `FlowViz.Benchmark`, …) are documented in
[Docs/UI_CONTROLS.md](Docs/UI_CONTROLS.md) §2.

## Run the tests

C++ automation tests (builds are NOT implied — build first):

```bash
Tools/build_lock.sh Tools/run_tests.sh FlowViz          # everything
Tools/build_lock.sh Tools/run_tests.sh FlowViz.UI       # the workspace suites
RHI=1 Tools/build_lock.sh Tools/run_tests.sh FlowViz    # includes GPU tests
```

Harness self-tests and tree checks (the sweep that guards the guards):

```bash
Tools/run_harness_tests.sh
```

## Package

Generate or install the representative case before packaging. `DefaultGame.ini`
stages `Samples/` as NonUFS content, so UAT includes the case before it signs the
application bundle:

```bash
Tools/generate_fluidx3d_sample.sh
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
    -project="$PWD/FlowViz.uproject" -platform=Mac \
    -clientconfig=Development -build -cook -stage -pak -package \
    -archive -archivedirectory="$PWD/Packaged"
```

The archived app lands in `Packaged/Mac/` with the representative case already
inside its signed resources. Smoke-test it headless without modifying the bundle
after signing:

```bash
Packaged/Mac/FlowViz.app/Contents/MacOS/FlowViz \
    -ExecCmds="FlowViz.LoadCase \"$PWD/Packaged/Mac/FlowViz.app/Contents/UE/FlowViz/Samples/FluidX3DSphereWake.cfdviz\" U, FlowViz.DumpCase, Quit" \
    -unattended -nullrhi -nosplash
```

Note the COMMAS between ExecCmds commands: semicolons are not separators
there and leak into the previous command's arguments (`U;` is not a field).
The app's log lands in
`~/Library/Containers/com.YourCompany.FlowViz/Data/Library/Logs/FlowViz/` —
the packaged app is sandboxed, so it is not beside the project's Saved/.

## Where things are

| | |
| --- | --- |
| Format spec | [Docs/CFDVIZ_FORMAT.md](Docs/CFDVIZ_FORMAT.md) |
| Architecture | [Docs/ARCHITECTURE.md](Docs/ARCHITECTURE.md) |
| Controls reference (measured, not aspirational) | [Docs/UI_CONTROLS.md](Docs/UI_CONTROLS.md) |
| ParaView-parity status table | [Docs/OPENFOAM_PARAVIEW_PARITY.md](Docs/OPENFOAM_PARAVIEW_PARITY.md) |
| Performance protocol and numbers | [Docs/PERFORMANCE.md](Docs/PERFORMANCE.md) |
| FluidX3D representative-case recipe and license boundary | [Tools/fluidx3d/README.md](Tools/fluidx3d/README.md) |
| Visual QA rules | [Docs/VISUAL_QA.md](Docs/VISUAL_QA.md) |
| Known gaps, honestly stated | [Docs/BACKLOG.md](Docs/BACKLOG.md) |
