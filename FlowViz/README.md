# FlowViz

A scientific CFD visualizer built on Unreal Engine 5: it opens `.cfdviz` cases
(a bricked, compressed volume format with a JSON manifest), ray-marches the
fields on the GPU, and gives an operator the controls a ParaView user expects —
transfer functions, slices, clipping, iso-surfaces, glyphs, streamlines, probes
and charts — with every rendering choice disclosed rather than silently
defaulted.

The CFD **solver** is out of scope: cases are produced elsewhere (or by the
bundled mock generator) and visualized here.

## Requirements

| Component | Tested version |
| --- | --- |
| Unreal Engine | 5.8.1 at `/Users/Shared/Epic Games/UE_5.8` |
| macOS | Darwin 25.x, Apple Silicon |
| Python | 3.12+ with `numpy` (for the data tools only) |

See [Docs/BUILD.md](Docs/BUILD.md) for the full reference environment and
troubleshooting; the commands below are the short path.

## Generate the sample case

A generated sample (`Samples/MockCylinderWake.cfdviz`, a synthetic cylinder
wake with an analytic vortex street) is **committed**, so this step is optional:

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
`FlowViz.LoadCase Samples/MockCylinderWake.cfdviz U` in the console. The
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

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
    -project="$PWD/FlowViz.uproject" -platform=Mac \
    -clientconfig=Development -build -cook -stage -pak -package \
    -archive -archivedirectory="$PWD/Packaged"
```

The archived app lands in `Packaged/Mac/`. Stage the sample beside the
packaged content and smoke-test it headless (verified on this machine — the
app opens the case with no manual importing):

```bash
cp -R Samples Packaged/Mac/FlowViz.app/Contents/UE/FlowViz/Samples
Packaged/Mac/FlowViz.app/Contents/MacOS/FlowViz     -ExecCmds="FlowViz.LoadCase $PWD/Packaged/Mac/FlowViz.app/Contents/UE/FlowViz/Samples/MockCylinderWake.cfdviz U, FlowViz.DumpCase, Quit"     -unattended -nullrhi -nosplash
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
| Visual QA rules | [Docs/VISUAL_QA.md](Docs/VISUAL_QA.md) |
| Known gaps, honestly stated | [Docs/BACKLOG.md](Docs/BACKLOG.md) |
