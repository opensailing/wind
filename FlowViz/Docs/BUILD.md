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

### `Build.sh` exits 0 even when the build FAILS

Verified on the reference machine (2026-08-04): a run that printed
`Result: Failed (ConflictingInstance)` still returned exit code `0`.

**Never branch on `$?`, and never chain with `&&`.** Redirect to a log and
grep the text:

```bash
LOG=/tmp/flowviz_build.log
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
    FlowVizEditor Mac Development \
    -project="$PWD/FlowViz/FlowViz.uproject" > "$LOG" 2>&1
grep -aq "Result: Succeeded" "$LOG" || { echo "build failed"; exit 1; }
```

This matters most for anything that *scores* a build. A mutation-testing
harness keyed on the exit code records every mutant as killed while proving
nothing — a result that reads as thorough coverage and is worthless.

Use `grep -a`. `Result: Failed (ConflictingInstance)` means another
UnrealBuildTool instance holds the global mutex — not your error; sleep and
retry. Its message contains `Global\UnrealBuildTool_Mutex_…`, and in zsh
`echo "$OUT"` turns that `\U` into a NUL byte, after which grep treats the
stream as binary and silently matches nothing. A retry loop piping through
`echo` therefore sees no conflict and proceeds as though the build ran.

### `Result: Succeeded` does not mean anything was compiled

A no-op build prints exactly the same line. Found 2026-08-05: a full-module
build returned exit 0, printed `Result: Succeeded`, and showed zero `error:`
lines, while the log read:

```
Target is up to date
Using Unreal Build Accelerator local executor to run 0 action(s)
```

It compiled nothing — a concurrent build had finished seconds earlier. If the
*point* of your build was to check that new or changed sources compile, that
run did not answer the question. Check the action count too:

```bash
grep -aq "Result: Succeeded" "$LOG" || { echo "build failed"; exit 1; }
grep -aq "run 0 action(s)" "$LOG" && echo "WARNING: compiled nothing"
```

### A git worktree under `/tmp` cannot be built

`/tmp` is a symlink to `private/tmp`. Unreal Build Accelerator refuses to
register writes through it (`dir not populated`) and truncates the object
file. **The error does not name the cause** — it surfaces at link as:

```
ld: LINKEDIT content 'symbol table' extends beyond end of segment
```

which reads as a corrupt toolchain or an engine bug and sends you debugging
the code instead of the path. It also produced repeated UNSCORED mutants in a
campaign before anyone suspected the directory.

Put worktrees under the home directory instead, e.g.
`~/projects/wind-worktrees/<name>`. Relocating fixes it with no other change.
If you ever see a LINKEDIT or symbol-table error, check `pwd -P` for
`/private/tmp` before investigating anything else.

### On clangd errors in your IDE

You will very likely see `'CoreMinimal.h' file not found` and a cascade of
`Unknown type name 'uint32'` errors in any editor using clangd. **These are not
real.** clangd has no compile database for a UBT project, so it cannot resolve
engine includes or the UE type prelude.

The authoritative gate is UnrealBuildTool. If `Build.sh` prints
`Result: Succeeded`, the code compiles. Do not "fix" clangd diagnostics by
changing correct UE code.

### An anonymous namespace is NOT per-file in this module

This is the trap most likely to bite anyone adding a reader, and it already
broke the build once (`9d5d5ac`).

FlowVizRuntime builds as a **unity build**: UBT generates
`Intermediate/.../Module.FlowVizRuntime.cpp`, which `#include`s every `.cpp` in
the module into a single translation unit. So this, in two different files:

```cpp
namespace   // "private to this file" -- it is not
{
    constexpr int64 OffsetFlags = 20;   // CVF, section 4.1
}
```

...collides with a sibling's `OffsetFlags = 16` (CVM). Whichever file the
generated include list names first wins, and the other reader silently parses
its own format using the wrong byte offsets. Not a crash — a wrong volume.

Give file-local format constants a **named namespace** (`namespace CvfLayout`)
or a distinct prefix (`CvmOffsetFlags`). Both are in use; either is fine.

**Assert your offsets at compile time.** The collision was caught only because
the CVF reader pins its layout:

```cpp
static_assert(OffsetFlags == 20 && OffsetFrameIndex == 24, "CVF header: ...");
```

which failed with `expression evaluates to '16 == 20'` and named the problem
outright. Readers without such assertions would have shipped a silent misparse.

To inspect what actually got concatenated:

```sh
grep include FlowViz/Plugins/FlowVizRuntime/Intermediate/Build/Mac/arm64/UnrealEditor/Development/FlowVizRuntime/Module.FlowVizRuntime.cpp
```

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

### `run_tests.sh` does NOT build. A new test file will not run.

It launches the editor against whatever binary is already on disk. Nothing in
its output distinguishes "your test passed" from "your test does not exist in
this binary" — the table simply omits it, and the `N/M passed` line is green
for the tests that *were* compiled in.

Observed 2026-08-05: a newly added `FlowVizVolumeMarchTest.cpp` produced
`6/6 passed` with the new test silently absent from the list. Build first, then
run:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
    FlowVizEditor Mac Development \
    -project="$PWD/FlowViz/FlowViz.uproject" > /tmp/b.log 2>&1
grep -aq "Result: Succeeded" /tmp/b.log && grep -aoE "run [0-9]+ action\(s\)" /tmp/b.log
```

**Check the test name you expect is in the output table.** A count is not a
roster; `7/7` and `6/6` look equally green.

**The second branch of this, which is worse: an EXISTING test running against
stale code.** The roster check above catches a new test that vanished. It cannot
catch a test that is present, runs, and reports on a binary built from source you
did not write — a changed assertion in a file that was already compiled in, or a
production fix that never reached the dylib. The name is in the table, the count
is green, and the result is about somebody else's code. In a shared checkout that
binary may be a peer's, and may contain their live mutant.

Two agents hit this on 2026-08-05, independently, both having already read the
warning above. It is not enough to know that `run_tests.sh` does not build; the
rebuild has to be an unconditional step, because the failure is silent and green
in both directions — it hides a change you broke AND a change you fixed.

**Wrapping it in `build_lock.sh` does not make it a build.** `build_lock.sh
./Tools/run_tests.sh …` reads like a build command and serializes like one. It
runs the editor.

```bash
# ALWAYS this order. The action count is the evidence, not "Result: Succeeded".
./Tools/build_lock.sh "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
    FlowVizEditor Mac Development -Project="$PWD/FlowViz.uproject" -WaitMutex
RHI=1 ./Tools/build_lock.sh ./Tools/run_tests.sh <filter>
```

**Zero compiled actions after a source edit means your edit did not reach the
binary.** That is the tell — it is how the stale-binary run was caught, when a
comment-only header change reported no recompiled units and an explicit build
then rebuilt nine. A `.usf` edit is the one exception: shaders compile at
runtime. The C++ half of a shader change is not exempt, and a half-rebuilt pair
is exactly how a flag appears to work.

### A non-zero exit does not mean a test failed

`run_tests.sh` uses distinct exit codes, and anything that *scores* a run must
tell them apart:

| code | meaning |
|-----:|---------|
| `1` | a test genuinely failed |
| `3` | the editor never started — no log was produced |
| `4` | the filter matched no tests — **nothing ran** |
| `75` | `build_lock.sh` timed out — the command never ran |

A harness keyed on `$? != 0` scores 3, 4 and 75 as caught bugs. Observed
2026-08-05: a mutation campaign reported 5 of 6 mutants killed while every arm
had actually exited 4, having tested nothing at all. The tell was the identity
control — a no-op mutation that must survive — coming back "killed."

**Always include a control arm that must survive.** Without one, a campaign
where nothing ran is indistinguishable from a campaign where everything was
caught. See `Tools/tests/march_differential.sh` for the shape: attribute
3/4/75 and shader-compile failure as UNSCORED *before* reading any verdict,
and require each kill to come from the assertion that names the bug rather
than from any failure at all.

### A green total can include tests that verified nothing

**Run the `RHI=1` arm before you believe a render claim.** The GPU tests skip
themselves under the default `-nullrhi`, and the engine records a self-skipped
test as `Result={Success}` — so they land in the pass count. On 2026-08-05 the
suite reported `47/47 passed` while three of those 47 had checked nothing
about the GPU at all.

The summary now names them, so this cannot hide any more:

```
3 skipped -- these reported success having verified nothing:
  FlowViz.Render.RayMarchShaderDevice
  FlowViz.Render.TransferFunctionDevice
  FlowViz.Render.VolumeDevice
  (reasons are in the log; GPU tests need RHI=1)

47/47 passed.
```

A skip does **not** fail the run. Turning the suite red for it would get the
signal suppressed the first time someone ran without a GPU, which is the
opposite of the point. But a skipped device test is an unanswered question,
not a passed one — treat the two arms as separate obligations.

`--summarize <log>` reports on an existing log without launching the editor.
It exists so the reporting logic is testable over fixture logs
(`Tools/tests/test_run_tests_summary.sh`) instead of a ~40s engine run.

### The detail log is per-checkout. Check its mtime before believing it.

`run_tests.sh` writes to `/tmp/flowviz_tests_<checkout-hash>.log`; run
`./Tools/run_tests.sh --print-log-path` to see yours. Setting `LOG` overrides it.

It used to be a fixed `/tmp/flowviz_tests.log` for every checkout. With nine
worktrees open — which exist precisely so concurrent agents cannot corrupt each
other — all nine wrote there, and the isolation was handed back at the last
step. Hit on 2026-08-05: a run finished at 14:33:23 and the file at that path was
dated 14:33:**30**, containing a different run's editor startup and zero test
records. The console summary said 49/50 and the log said nothing.

The console summary is printed by your own process and is always yours. The
detail log is a file on a shared filesystem. When they disagree, suspect the
file — and note that a *missing* log would have been safe, whereas an unrelated
one at a known-good path reads as evidence about your run. That log is exactly
what you open when a total looks wrong, so it turns a suspicion into a confident
wrong answer.

Before quoting a log for `MetalRHI`, `nullrhi`, a skip count, or an assertion's
text, check `stat -f '%Sm' <log>` against when your run ended. **A log newer than
the run that supposedly produced it belongs to a different run.** Prefer
capturing your own stdout (`> /tmp/my_run.log 2>&1`) for anything you will report
or score, and give each mutation arm its own file — a verdict read from a shared
path can be a false KILLED, which reports as coverage you do not have.

### Tests failing on a shared tree may be someone else's mutation

This project proves its assertions can fail by deliberately breaking the code
underneath them. While a break-verify-restore cycle is in flight the tree is
genuinely red, and from the outside that is indistinguishable from a real
defect — on 2026-08-05 a cbuffer assertion failed in a way that looked exactly
like a drifted struct member and was in fact a live falsification cycle.

Before filing a failure seen during a shared-tree run:

```bash
git status --short                       # unexpected modifications?
grep -rn "// BROKEN:" <source dirs>      # live sabotage markers
stat -f '%Sm %N' <the source> <the .dylib>   # was it edited mid-run?
```

A source file whose mtime matches the second your test reported the failure
was being edited while you compiled.

### Declare a mutation window before you break anything

A live mutation does not only corrupt other people's *verdicts*. It corrupts
their *history*: while a deliberate defect sits in the tree, any other agent
running `git add -A` or `git commit -a` commits it under their own task's name,
with an innocent message on top and a green suite beside it. A bad verdict gets
caught by a re-run. A mis-attributed commit does not — nothing about it looks
wrong afterward.

So declare the window. `Tools/mutate.sh` does this automatically and needs
nothing from you. Do it by hand only for a one-line manual break — the case that
skips the tooling and causes the trouble.

**Do not use a `trap` to clear it.** Each agent shell command runs in its own
shell, which exits the moment that command returns. A trap installed there fires
at the end of *that command*, so it clears the marker and reverts your mutation
before you ever get to the build. What happens next is the trap in both senses:
you see your edit undone, re-apply it in the following command without the marker
idiom that appeared to eat it, and now the defect is live with **no window
declared**. That is not hypothetical — it is how the shader mutants of
2026-08-05 came to be live in the shared tree while the guard said committing was
fine. A trap only works inside a single long-running script, which is why
`mutate.sh` can use one and you cannot.

Arm it in one command, clear it explicitly in a later one:

```bash
# 1. arm, then break the file (same command or a later one -- no trap)
MARKER="$(git rev-parse --git-dir)/FLOWVIZ_MUTATION_ACTIVE"
printf 'purpose=<what you are proving, and which task>\n' > "$MARKER"

# 2. ... build, run the suite, record the verdict, across as many commands
#    as it takes. The window stays open the whole time, which is correct.

# 3. restore FIRST, verify, and only then close the window
cp "$BACKUP" "$SRC"
git diff --stat            # must be empty for the mutated file
rm "$MARKER"
```

Restore before clearing, in that order. Clearing first reopens the tree to
committers while the defect is still in it, which is the exact failure the
marker exists to prevent.

Record a `purpose=` naming what you are proving. Do not bother recording a pid:
it belongs to a shell that is already dead by your next command, so it proves
nothing about whether the run is alive, and a reader who tests it with `ps` will
conclude a live campaign was abandoned. `mutation_guard.sh` strips pids from the
refusal for that reason.

`Tools/mutation_guard.sh`, installed as `.git/hooks/pre-commit`, refuses every
commit in the checkout while that file exists:

```bash
cp FlowViz/Tools/mutation_guard.sh .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit
```

The hook is a **copy**, so fixing the source does not update it — run that line
again after any change to `mutation_guard.sh`, and in every new clone and
worktree. `Tools/tests/test_mutation_guard.sh` checks the installed copy against
the source and prints the reinstall command when they diverge; it caught the
live hook running a version two commits behind while its own suite was green.

It lives under `.git/` so it survives the sweeping `git checkout -- .` a mutation
run ends with, and so it can never itself be staged. It is a file rather than a
process lock because the dangerous case is the **abandoned** run — the process
that died between applying the defect and restoring it. A lock would release on
that death, at the moment protection matters most. A worktree's marker blocks
only that worktree, which is the isolation you wanted.

**Do not delete the marker to get your commit through.** Verify the tree is clean
of the mutation first (`git diff`), then clear it.

### Never `reset --hard` or `checkout -- .` in a shared checkout

Both destroy the uncommitted work of every other agent in the tree, with no undo
— the reflog recovers commits, and their work was never committed. Unrecognised
modified files are the normal state of a shared checkout, not debris.

This happened on 2026-08-05: an agent smoke-tested the pre-commit hook above with
an `--allow-empty` commit followed by `reset --hard HEAD~1`, and destroyed three
of a peer's uncommitted files mid-task. The destructive step was the *cleanup*,
not the action being tested — an empty commit feels like it touches nothing,
which is what makes the paired reset feel free.

Smoke-test git behaviour in `mktemp -d` + `git init`. Hooks, aliases and filters
all behave identically there. If you must verify against the real repo, verify
only the path that gets REFUSED — the success path is the one that needs undoing.
For your own files, prefer `git stash push -- <explicit paths you own>` over
anything sweeping.

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

## Sample data

`FlowViz/Samples/MockCylinderWake.cfdviz` is **committed** (3.39 MiB), so a
fresh checkout has a case to load without running anything first. Regenerate it
byte-identically with:

```bash
PYTHONPATH=FlowViz/Tools/cfdviz/src python3 -m cfdviz generate-mock \
    --low-res --output FlowViz/Samples/MockCylinderWake.cfdviz
```

`--low-res` is the small preset that fits in source control. Omit it for the
full-resolution case (128 x 64 x 24, 90 frames), which is far too large to
commit. Every generation parameter is exposed as a flag — see `--help` — and
the output is deterministic: the same parameters always produce the same bytes,
including the case UUID.

The data is a closed-form analytic construction, **not a solved flow**. The
manifest says so, in those words, and that wording is asserted by a test.

Validate any case with:

```bash
PYTHONPATH=FlowViz/Tools/cfdviz/src python3 -m cfdviz validate <case>
```

which exits non-zero and names the offending file and byte offset on failure.

Two more commands help when a case looks wrong in the renderer but valid to the
validator:

```bash
# One field at one frame: a summary, one voxel, or a .npy in its stored dtype
python3 -m cfdviz extract <case> --frame 12 --field U --voxel 40 14 3

# Decode everything and report throughput, in DECODED bytes rather than
# file size, so the number is comparable across codecs
python3 -m cfdviz benchmark-read <case>
```

`extract --voxel` reports `masked` separately from the value, because a cell
inside the obstacle holding NaN and a cell whose value is genuinely NaN are
different bugs.

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

**Headless render capture is not yet working on this machine.** Captures write
correctly-sized PNGs that contain sky and fog but **no geometry**. This is an
open problem; `Tools/capture/` is usable plumbing, not a finished capture path.

Do not re-litigate the following. Each was proposed as the cause and then
disproven by experiment, and rediscovering them is expensive:

| Theory | How it was ruled out |
| --- | --- |
| Sidecar/AirPlay virtual display breaks the swapchain | `-RenderOffscreen` never presents to a swapchain and is also black |
| `DefaultEngine.ini` `MacTargetSettings` (`SF_METAL_SM6`) | Stock engine maps render in the *same* project with the *same* config. **Do not revert this config.** |
| Lighting is wrong / meshes are black | An unlit mesh still *occludes* the sky. Deleting all geometry produced a byte-identical image, so nothing was drawn at all |
| `SCENE_DEPTH` shows an empty scene | It reads `max=255 unique=2 mean=85.0` on working maps too — depth lands in the red channel and saturates in RGBA8. Useless as a diagnostic |

Two facts that are established and load-bearing:

- **`-AllowCommandletRendering` is mandatory.** Without it the renderer is
  disabled outright in a commandlet and nothing renders at all.
- **A capture that looks non-black proves nothing.** SkyAtmosphere and
  VolumetricCloud render a full-screen gradient with zero primitives involved.
  Verify differentially — see [VISUAL_QA.md](VISUAL_QA.md) §4.

To check whether the render scene is populated, independently of any capture:

```bash
RHI=1 ./FlowViz/Tools/run_tests.sh FlowViz.Capture
```

That test spawns a cube and asserts the scene's proxy count rises, then falls
when it is destroyed. Two traps it encodes, both of which produce a convincing
false zero:

- `FScene::GetPrimitiveSceneProxies()` reports the state as of the **last frame
  drawn**. Adds sit in a pending queue until a frame renders, so a commandlet
  that never drew one reports zero forever. Force the drain with
  `UpdateAllPrimitiveSceneInfos`, inside a `UE::RenderCommandPipe::FSyncScope`
  — without that scope it trips `Assertion failed: !IsReplaying()`.
- A world may hold an **`FNULLSceneInterface` stub** rather than a real
  `FScene`, which returns an empty proxy array unconditionally.
  `UWorld::AllocateScene` falls back to it unless `GIsClient &&
  FApp::CanEverRender() && !GUsingNullRHI`.
