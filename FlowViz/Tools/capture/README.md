# Headless frame capture

Renders a FlowViz map from an arbitrary camera and writes a PNG, with no editor
window and no user interaction. Verified on macOS 15 / Apple M4 / Metal
(METAL_SM6) against Unreal Engine 5.8.1.

## Working command

```sh
cd FlowViz/Tools/capture
./capture_frame.py --map /Game/FlowViz/Maps/L_CapTest \
                   --output /tmp/shot.png \
                   --location -400 0 150 \
                   --rotation -15 0 0 \
                   --resolution 1280 720
```

Exit code is 0 only when every requested PNG exists and contains real shaded
content. Rotation is `[pitch, yaw, roll]` in degrees, matching the editor's
Details panel. Prefer `--look-at X Y Z` over `--rotation` — see "A bright PNG is
not a correct PNG" below.

### Verified measurements

Command above, `L_CapTest`, 1280x720, measured with the external checker:

```
min 0  max 219  mean 100.668  unique 218   (bottom-half mean 114.459)
```

Visually confirmed: sphere, checkered floor, cast shadow, sky. A 4-shot batch
over two maps produced 4/4 PASS.

Note `unique` differs by tool and both numbers appear in this repo. The external
one-liner's `len(np.unique(a))` counts distinct **byte values** (max 256); the
in-engine number counts distinct **RGB triples** (3995 for the same image).
Don't compare them to each other.

### Timing

| | Wall clock |
|---|---|
| Single shot, 1280x720, cold launch | **26s** |
| 4-shot batch, 2 maps | 21.2s total (**5.3s/shot** amortized) |

Engine startup and map load dominate; an individual capture is well under a
second. **Batch whenever you have more than one shot** — 20 separate
invocations take ~9 minutes, one batch of 20 takes ~1. Shots are grouped by map
internally so each map loads at most once per launch.

```sh
./capture_frame.py --batch shots.json
```

`shots.json` is a JSON list of shot objects; only `map` and `output` are
required. Schema is documented at the top of `capture_frame.py`, and `capture()`
is importable if you would rather not shell out.

First run after a shader-cache change costs several extra minutes for shader
compilation. That is a one-time cost per configuration, not per capture.

## Verification

Never trust a log line. An earlier iteration of this tool logged "exported" and
wrote no file at all — `ExportRenderTarget` is a silent no-op when the render
target is null. The shipped tool therefore reads pixels back **inside** the
engine and re-checks the file on disk afterwards, and this is the external check
to run on the result:

```sh
python3 -c "
from PIL import Image; import numpy as np
a=np.array(Image.open('/tmp/shot.png').convert('RGB'))
print('min',a.min(),'max',a.max(),'mean',round(float(a.mean()),3),'unique',len(np.unique(a)))
"
```

Those numbers are context, not a verdict. **PASS requires the frame to differ
from the same view with every primitive suppressed** — the tool renders that
reference automatically for each shot and compares. A frame that matches it
contains no geometry, however bright it is.

An earlier version of this tool passed on `max > 0 && unique > 10` and certified
captures that were byte-identical to the level with every mesh deleted. The
criterion now lives in `verdict.py` with tests that feed it known-bad frames and
require rejection.

### A bright PNG is not a correct PNG

**"Capture works" does not mean "this camera shows geometry."** This is the
single most important caveat here, and it produced repeated false positives
during development.

A SkyAtmosphere renders a bright, richly dithered full-screen gradient using no
primitives whatsoever. A camera that misses the geometry entirely yields
`max=219, unique=3017` — comfortably passing every brightness and variety check
while containing nothing but sky. Several such images were initially recorded as
successes and only caught by looking at them.

Defences, in order of strength:

1. **Look at the image.** Nothing else is conclusive.
2. **Differential test** — capture, remove the geometry, capture again, require
   the images to DIFFER. Measured 577,579 of 921,600 pixels changed. This is the
   acceptance criterion, not the absolute threshold. See `Docs/VISUAL_QA.md`.
3. **Bottom-half mean.** Sky occupies the top of a typical shot, so a healthy
   floor shows up as a bottom-half mean near 114 versus ~1.6 when unlit.
4. **`--look-at X Y Z`** instead of hand-picked pitch/yaw, which removes the
   most common cause of the failure.
5. **Proxy count** is logged per map. `0` means the render scene is genuinely
   empty and no camera or material change will help; `-1` means "could not
   tell" and is deliberately distinct from `0`.

## What failed, and why

Five independent causes, each of which alone produces a black or misleading
frame. All are fixed in the shipped path; they are recorded because each is
invisible from the outside and expensive to rediscover.

1. **Missing `-AllowCommandletRendering`.** Without it the RHI never
   initializes in a commandlet and every capture is silently black. The script
   always passes it.

2. **Python cannot spawn a scene capture.** Every Blueprint spawn entry point
   (`SpawnActorFromClass`, `BeginDeferredActorSpawnFromClass`,
   `AddComponentByClass`) is marked `BlueprintInternalUseOnly` and so is not
   exposed to Python, and `RegisterComponent` is not a `UFUNCTION`. A
   `SceneCaptureComponent2D` constructed directly in Python lands in
   `/Engine/Transient` with no world, captures black, and **reports success** —
   indistinguishable from a broken GPU path. Fixed by
   `UFlowVizCaptureLibrary::SpawnSceneCapture2D` in C++.

3. **Materials load lazily and render BLACK until resolved** — they do not fall
   back to a visible default. The result is a correct sky above a pitch-black
   floor, which reads as a broken renderer and sends you debugging the capture
   path instead of the content. `UFlowVizCaptureLibrary::ResolveMaterials`
   forces them; measured on `L_CapTest`, bottom-half mean **1.63 → 114.51**,
   same world and camera, back to back in one process.

4. **Editor HUD text composited into the captured pixels.**
   `r.SkyAtmosphere.EditorNotifications` defaults to `1`, and on any map with a
   skydome *mesh* using a sky material the renderer draws
   "YOUR SCENE CONTAINS A SKYDOME MESH WITH A SKY MATERIAL..." straight into
   scene colour during the base pass — into the render target, not a separate
   editor layer, so it survives into the PNG.
   `/Engine/Maps/Templates/Template_Default` trips this. Insidiously, the text
   *adds* brightness and colours, so it moves max/unique/mean **up** and makes a
   capture look healthier: that shot read `max=200 unique=3713` with the overlay
   and `max=148 unique=649` once disabled. The worker now turns it off after
   every map load.

5. **Content bugs in `L_CapTest`**, each individually sufficient to blacken the
   frame: no SkyAtmosphere; the DirectionalLight pitched **+45**, i.e. pointing
   *up*, which a SkyAtmosphere renders as night; and ExponentialHeightFog
   density `0.02`, which over a 1000-unit scene is solid haze that hides
   everything while passing brightness checks. The map is fixed and saved. The
   worker warns about all three, plus non-Movable light mobility — this project
   runs `r.AllowStaticLighting=False`, so static and stationary lights
   contribute nothing without a built lighting pass.

## Files

| | |
|---|---|
| `capture_frame.py` | CLI and importable driver; runs outside Unreal |
| `_capture_worker.py` | runs *inside* Unreal via the pythonscript commandlet |
| `../../Plugins/FlowVizRuntime/.../FlowVizCaptureLibrary.h` | C++ helpers Python cannot do itself |

Tests: `RHI=1 ./Tools/run_tests.sh FlowViz.Capture` (2/2 passing).

## Gotchas

- `print()` does **not** reach the log from the pythonscript commandlet. Use
  `unreal.log_warning()`.
- The render target must be `RTF_RGBA8_SRGB`. Float formats silently take the
  EXR/HDR path and you get no PNG.
- `unreal.Rotator` takes `(roll, pitch, yaw)`, not pitch-first. The tools accept
  the conventional `[pitch, yaw, roll]` and reorder internally.
- The capture is issued several times before readback; one capture is not
  reliably enough, as temporal effects settle over a few frames and the first is
  frequently blank.
- `capture_source` defaults to `FINAL_COLOR_LDR` (lit and tonemapped). Set it
  explicitly if you change it — omitting it silently captures with the default
  source, which made a floor read black and falsely implicated fog during
  development. `BASE_COLOR` and `SCENE_DEPTH` do not survive the 8-bit PNG path
  in a useful form and are for diagnostics only.
