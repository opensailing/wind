# Building and testing FlowViz

Everything below has been run on the reference machine and is reported as
observed, not as intended. Where something is unverified on a platform, it says
so.

## Reference environment

| Component | Version |
| --- | --- |
| Unreal Engine | 5.8.1 (`++UE5+Release-5.8`, CL 56057345) |
| Engine location | `/Users/Shared/Epic Games/UE_5.8` |
| macOS | 26.5.2 (Darwin 25.5.0) |
| Hardware | Apple M4, 10 GPU cores, 32 GB |
| Xcode | 26.5 (build 17F42) |
| Apple clang | 21.0.0 |
| Python | 3.14.0 |
| numpy | 1.26.4 |
| pytest | 9.0.2 |
| jsonschema | 4.26.0 |

Windows is a declared target platform in `FlowViz.uproject` but is **not yet
built or tested**. Do not assume it works.

## Project layout

The project is a C++ project with two plugins:

| Module | Type | Purpose |
| --- | --- | --- |
| `FlowVizApp` | Primary game module | Application entry point |
| `FlowVizRuntime` | Runtime plugin (`PostConfigInit`) | Format readers, data model, rendering |
| `FlowVizEditor` | Editor-only plugin | Editor tooling |

`FlowVizRuntime` loads at `PostConfigInit` so its virtual shader path mapping
(`/Plugin/FlowViz` → the plugin's `Shaders/` directory) is registered before any
global shader is compiled. Loading later makes shader compilation fail in ways
that are hard to attribute.

## Building

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
    FlowVizEditor Mac Development \
    -project="$PWD/FlowViz/FlowViz.uproject"
```

Success is the literal line `Result: Succeeded`. A clean incremental build of
the three modules takes roughly 15 seconds on the reference machine.

Targets are `FlowVizEditor` (editor) and `FlowViz` (game/packaged).

### On clangd errors in your IDE

You will very likely see `'CoreMinimal.h' file not found` and a cascade of
`Unknown type name 'uint32'` errors in any editor using clangd. **These are not
real.** clangd has no compile database for a UBT project, so it cannot resolve
engine includes or the UE type prelude.

The authoritative gate is UnrealBuildTool. If `Build.sh` prints
`Result: Succeeded`, the code compiles. Do not "fix" clangd diagnostics by
changing correct UE code.

## Running C++ tests

```bash
./FlowViz/Tools/run_tests.sh                      # all FlowViz tests
./FlowViz/Tools/run_tests.sh FlowViz.CFDViz       # a subtree
./FlowViz/Tools/run_tests.sh FlowViz.CFDViz.Crc32C
RHI=1 ./FlowViz/Tools/run_tests.sh FlowViz.Render # tests that need a GPU
```

Output is a per-test table plus an `N/M passed` line; the exit code is non-zero
on any failure.

Two flags in that script are worth knowing about, because rediscovering them
costs real time:

- **`-abslog=<path>` is required.** Without it the engine log is swallowed and
  stdout carries only UnrealTrace daemon chatter — which looks exactly like a
  silent crash. The interesting output goes to the log file, not the terminal.
- **`-nullrhi`** skips GPU initialization. Correct and fast for logic tests;
  set `RHI=1` to drop it for anything that renders.

The script treats a filter matching **zero** tests as a failure. The engine
exits 0 in that case, so a typo'd test path would otherwise read as success.

## Running Python tests

```bash
cd FlowViz/Tools/cfdviz
python3 -m pytest tests -q
```

`pyproject.toml` sets `pythonpath = ["src"]`, so no install step is needed to
run the suite. To use the CLI without installing:

```bash
PYTHONPATH=FlowViz/Tools/cfdviz/src python3 -m cfdviz --help
```

Or install it for real:

```bash
pip install -e "FlowViz/Tools/cfdviz[dev]"
```

The only required dependency is numpy. `lz4` and `jsonschema` are optional
extras and the code degrades gracefully without them. **zstandard is
deliberately not a dependency** — see
[ADR 005](ADR/005-compression-codec.md).

## Generating sample data

```bash
PYTHONPATH=FlowViz/Tools/cfdviz/src python3 -m cfdviz generate-mock \
    --low-res --output FlowViz/Samples/MockCylinderWake.cfdviz
```

`--low-res` produces the small case that is small enough for source control and
that the packaged application auto-loads. Omit it for the full-resolution case.

Validate any case with:

```bash
PYTHONPATH=FlowViz/Tools/cfdviz/src python3 -m cfdviz validate <case>
```

which exits non-zero and names the offending file and byte offset on failure.

## Cross-language format agreement

The Python writer and the Unreal reader must agree byte-for-byte. This is
enforced mechanically rather than by inspection:

1. The generator writes `known_values.json` into the case, recording exact
   IEEE-754 **bit patterns** read back *through the Python reader*.
2. The Unreal automation test `FlowViz.CFDViz.KnownValues` reads that file and
   asserts on the bit patterns via the C++ reader.

Point that test at a case with:

```bash
FLOWVIZ_TEST_CASE=/path/to/case.cfdviz ./FlowViz/Tools/run_tests.sh FlowViz.CFDViz.KnownValues
```

It falls back to `FlowViz/Samples/`. If no case is present the test logs a skip
rather than failing, so a fresh checkout is not blocked on generating data.

The same discipline applies to CRC-32C: `FlowViz.CFDViz.Crc32C` asserts the
check value `0xE3069283` that Python's `crc32c.self_check()` asserts at import.
CFDViz uses CRC-32**C** (Castagnoli, reflected polynomial `0x82F63B78`) — *not*
`FCrc::MemCrc32` and *not* zlib's CRC-32.

## Known platform issues

**Headless render capture on this machine.** The reference Mac's main display is
a Sidecar/AirPlay **virtual** device rather than a physical panel. Capture paths
that render through a real window swapchain (`-DumpMovie`, `HighResShot`)
produce correctly-sized but entirely black frames — verified all-zero pixels,
not a loading artifact. See `Tools/capture/README.md` for the working capture
path and its caveats.
