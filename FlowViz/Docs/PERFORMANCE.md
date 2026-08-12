# Performance

Required by `plan.md` §3 and §17.

**Nothing in this document is a measurement yet.** `plan.md` §17 closes with
"Record actual measured hardware and results in PERFORMANCE.md. Do not claim
performance that was not measured," and at the time of writing FlowViz has never
rendered a frame — there is no ray-march dispatch wired to a view, so there is
no frame time to report. See [`BACKLOG.md`](BACKLOG.md) item 2.

This file exists now, empty of results, on purpose. Writing the protocol *before*
there are numbers is the only point at which it can be written without knowing
which numbers it will bless. A results table added later to a file whose method
section was written alongside it is a much weaker artefact.

---

## 1. The targets, and what each one actually asserts

From `plan.md` §17. They are goals for the included sample, and the plan says
outright that they are "not reasons to fake results". Restated here as things
that can be *falsified*, because a target that cannot fail is not a target.

| # | Target | What would falsify it |
|---|---|---|
| T1 | Responsive timeline scrubbing | A scrub input whose visible response exceeds the stated budget, measured input-to-photon rather than by feel |
| T2 | No synchronous disk loading on the game thread | Any file read on the game thread, detected by instrumentation, not by inspection |
| T3 | No full texture-resource recreation per frame | RHI resource creation count > 0 in steady-state playback |
| T4 | ≥ 30 FPS with the sample volume plus a moderate streamline count | Median frame time > 33.3 ms over a stated window, on stated hardware |
| T5 | UI remains responsive while expensive filters recompute | Game-thread hitch beyond budget during a recompute |
| T6 | Memory use obeys configured cache budgets | Resident bytes exceeding `FlowViz.SetCpuCacheMB` / `SetGpuCacheMB` at any sample |

T2, T3 and T6 are the interesting ones: they are **absolute**, so a single
counterexample falsifies them and no averaging can hide it. T1, T4 and T5 are
statistical and need a stated window and percentile, not a single best run.

---

## 2. Reporting rules

These bind whoever fills in §4.

1. **State the hardware, every time.** A frame time without a machine attached
   to it is not a result. §3 records the only machine used so far.
2. **Report a distribution, not a best case.** Median and 95th percentile over a
   stated number of frames, plus the window length. A mean alone hides hitches,
   and hitches are what T1 and T5 are about.
3. **Discard the first N frames and say what N was.** Shader compilation, PSO
   creation and texture upload all land in early frames and none of them recur.
4. **A target with no measurement is `NOT MEASURED`, never blank and never
   inferred.** "Should be fine given the data size" is not a row in this table.
5. **Report the regression too.** If a change makes something slower, that
   number goes in this file. A performance document containing only improvements
   is an advertisement.
6. **Name the build configuration.** A Development-editor number and a Shipping
   number are different claims about different binaries; either is fine to
   report, mislabelling is not.

Rule 4 is the one that matters most here, because every row in §4 currently
depends on it.

---

## 3. Hardware

The only machine FlowViz has run on. Recorded now because it is verifiable now,
independent of whether any measurement exists.

| Property | Value |
|---|---|
| Model | Apple M4 |
| CPU | 10 physical cores, 10 logical |
| Memory | 32 GiB unified |
| GPU | Apple M4 integrated, unified memory (no discrete VRAM) |
| Graphics API | Metal (Metal 4 support reported) |
| OS | macOS 26.5.2 |
| Engine | Unreal Engine 5.8.1 |

Two consequences of unified memory that matter for reading any future result
from this machine:

- **The CPU and GPU cache budgets draw on the same physical pool.** On a
  discrete-GPU machine they are separate resources and exhausting one does not
  pressure the other. T6 can pass here and fail there, or the reverse.
- **Upload cost is not bus-transfer cost.** `FlowVizVolumeTexture`'s upload path
  does not pay PCIe transfer on this machine. A discrete GPU will, and a volume
  upload time measured here will understate it there.

Neither is a reason to avoid measuring on this machine. Both are reasons the
result must not be generalised past it.

---

## 4. Results

| Target | Result | Notes |
|---|---|---|
| T1 Timeline scrubbing | **NOT MEASURED** | No UI layer exists (`plan.md` §F) |
| T2 No sync disk I/O on game thread | **NOT MEASURED** | Playback path in progress |
| T3 No per-frame resource recreation | **NOT MEASURED** | Requires steady-state playback |
| T4 ≥ 30 FPS with sample + streamlines | **NOT MEASURED** | Nothing has been rendered |
| T5 UI responsive during recompute | **NOT MEASURED** | No UI layer, no filters |
| T6 Cache budgets obeyed | **NOT MEASURED** | `FFlowVizFrameCache` accounting exists; not yet exercised under load |

Six of six unmeasured. This table is the honest state of the project's
performance knowledge and should be read as such.

### The samples under test

`Samples/FluidX3DSphereWake.cfdviz` is the representative T4/performance
fixture. It contains a genuine external FluidX3D sphere-wake solve:

- 192×96×96 lattice cells and 1,762,319 active cells;
- 40 stored snapshots spanning solver steps 6000 through 6390;
- imported velocity and flags sequences;
- nonzero spanwise velocity and spanwise gradients;
- measured changes between every adjacent velocity frame;
- 406,364,515 bytes (387.54 MiB) in the converted case.

`cfdviz qualify-representative` pins these claims to the exact solver revision,
known-value bridge, minimum 3D extent, temporal cadence, and deterministic case
SHA-256. Performance results for the primary demo must name this case and its
hash. The read benchmark currently records conversion-tool decode throughput;
it is not a renderer frame-rate result, so T4 remains **NOT MEASURED** above.

`Samples/MockCylinderWake.cfdviz` is the 3.39 MiB, 56×28×6, 20-frame analytic
correctness fixture. **It is not a performance fixture, and a T4 result measured
against its 9,408 cells is invalid.** It remains committed only because format,
known-value, and renderer tests need a small deterministic case in every fresh
checkout.

---

## 5. Instrumentation status

`plan.md` §17 asks for a diagnostics overlay (24 counters), Unreal trace events,
a `LogFlowViz` category, and eight console commands.

| Item | Status |
|---|---|
| `LogFlowViz` log category | **Implemented** — declared and in use |
| Diagnostics overlay (24 counters) | **Not implemented** |
| Unreal trace events | **Not implemented** |
| `FlowViz.*` console commands (8) | **Not implemented** |

The overlay is a prerequisite for most of §4: the counters it names — cache hit
rate, pending I/O, decompression time, GPU upload time, volume sample count — are
the measurements. Building it is how T2, T3 and T6 become checkable at all,
since none of the three can be established by watching a frame rate.

One caution for whoever builds it: **an overlay counter is a claim, and a
counter that cannot be wrong is not a measurement.** A "cache hit rate" wired to
a hit counter that is never decremented reads 100% forever and looks like
success. Each counter needs a test that drives it to a known non-trivial value
and fails if it reports something else — the same standard the rest of this
project holds, for the same reason.
