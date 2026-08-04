# Visual quality bar and review protocol

This project has two visual targets, and they are not the same target. Judging
one by the other's standard produces bad work in both directions.

| Profile | Question it answers | Optimized for |
| --- | --- | --- |
| **Scientific** | "What is the data?" | Quantitative honesty |
| **Presentation** | "What does the flow look like?" | Cinematic beauty |

The Presentation profile must never be the default for quantitative work
(engineering rule; `plan.md` §9). The Scientific profile must never be dismissed
as "the ugly one" — a legible, honest scientific render is a design achievement,
not a fallback.

Both are held to a high bar. They are held to *different* bars.

---

## 1. The Scientific profile bar

The reference is ParaView, Tecplot, EnSight, and figures in *Journal of Fluid
Mechanics*. The bar is: **a reviewer could publish this figure without
apologising for it.**

### Hard requirements — a failure here is a bug, not a preference

1. **Color is data.** Pseudocolored surfaces use neutral or unlit shading.
   Lighting must not modulate the apparent scalar value. If a surface looks
   brighter because a light is nearer, the render is lying.
2. **The scale is legible.** Every pseudocolored view carries a scalar legend
   with the field name, units, and numeric range.
3. **The range is stable and stated.** Global range by default. A per-frame
   auto range must be visibly labelled, because a color that changes meaning
   between frames fabricates apparent physics.
4. **Invalid data is visibly invalid.** NaN, masked cells, and out-of-range
   values are rendered distinctly — never silently as zero, never as the
   colormap's minimum.
5. **Interpolation is disclosed.** A temporally interpolated frame says so
   on screen.
6. **Perceptually uniform colormaps by default** (viridis, cividis, or an
   equivalent). Rainbow/jet must not be the default: it manufactures visual
   edges where the data is smooth and hides real structure where it is steep.
   Offer it, labelled, for users who need it for comparison with legacy figures.
7. **Diverging data uses a diverging map centred on the meaningful zero.**
8. **No post-processing that alters apparent values.** No bloom on a
   pseudocolored surface, no auto-exposure, no motion blur, no depth of field,
   no chromatic aberration, no film grain, no vignette.
9. **Geometry reads correctly.** No z-fighting, no shadow acne, no visible
   aliasing on iso-surfaces, no cracks at brick boundaries.

### Quality requirements — these separate "correct" from "excellent"

10. Iso-surfaces are smooth and free of stair-stepping and banding.
11. Volume rendering shows no ring artifacts, no visible step banding, no
    posterization in the transfer function.
12. Text is crisp at native resolution; no blurry or clipped UI labels.
13. Anisotropic voxel spacing renders with correct proportions.
14. Silhouettes are clean at high zoom — no polygonal facets on curved surfaces.

---

## 2. The Presentation profile bar

The reference is film and AAA games: Houdini/Mantra and Arnold fluid renders,
the water and smoke in *Avatar: The Way of Water*, and real-time volumetrics in
recent AAA titles.

The user's stated bar is explicit: **a critic comparing a FlowViz render
side-by-side and blind against a film or AAA reference should not be able to
say the reference is clearly better.**

### What actually creates that impression, in rough order of impact

1. **Light transport.** Multiple scattering in the volume, not just absorption.
   Single-scattering-only smoke reads as flat and dead — this is the single
   biggest tell of a "technical" render.
2. **Density detail at multiple scales.** Real fluid has structure from the
   domain scale down to the pixel. Smooth blobby density is the second biggest
   tell.
3. **Silhouette and edge quality.** Wispy, detailed, high-contrast edges.
   Soft-edged blobs read as low quality regardless of shading.
4. **Value range and tone.** Real renders carry a wide dynamic range with
   genuine highlights and deep shadows. Flat mid-grey reads as amateur.
5. **Self-shadowing** within the volume, giving depth and form.
6. **Grounded context.** Contact shadows, environmental reflection, a believable
   background. A volume floating in flat grey never looks film-grade.
7. **Temporal stability.** No flickering, swimming, or crawling under motion.
   A still frame that looks perfect and shimmers in motion has failed.
8. **Motion quality.** Correct motion blur, believable advection, no popping
   between frames.
9. **Camera craft.** Deliberate framing, appropriate depth of field, a focal
   length that suits the subject.

### Hard failures in this profile

- Visible step banding or ray-march artifacts
- Aliased or crawling edges
- Obvious tiling or repetition in noise/detail
- Temporal flicker
- Clipping to pure white or crushing to pure black across large areas
- Popping when frames change

---

## 3. Review protocol

The reviewer is an adversarial critic. Their job is to find what is wrong, not
to confirm that work is done.

### Rules

1. **Blind comparison.** The critic is shown the FlowViz render and a
   film/AAA/publication reference **without being told which is which**, and
   must state which is better and why. Labelling them first destroys the test.
2. **The default verdict is REJECT.** "Looks good" is not a passing verdict.
   A pass requires stating specifically what makes it good.
3. **Findings must be actionable and specific.** "Banding visible in the
   density falloff on the upper-left of the plume, roughly 8-pixel period,
   suggests too few ray-march steps" is a finding. "Looks a bit off" is noise.
4. **Judge against the right profile.** Never fail a Scientific render for
   lacking bloom. Never pass a Presentation render because it is
   quantitatively accurate.
5. **Motion is reviewed in motion.** Any effect involving time is reviewed as
   an image sequence, never as a single frame.
6. **Report honestly.** If a capture is black, or the feature is not actually
   implemented, say that. Never describe an unrendered feature as passing.

### Verdicts

| Verdict | Meaning |
| --- | --- |
| `REJECT` | Hard failures present. Fix and resubmit. |
| `WEAK` | No hard failures, but the reference clearly wins the blind test. |
| `PASS` | No hard failures; blind comparison is genuinely a coin flip. |
| `EXCEEDS` | The critic preferred the FlowViz render blind. |

The loop continues until `PASS` or better. `WEAK` is not a passing grade — it
is the most common self-deception in this kind of work, because "no specific
thing is wrong" feels like success while the result is still visibly inferior.

### Required output format

```
PROFILE:  Scientific | Presentation
VERDICT:  REJECT | WEAK | PASS | EXCEEDS
BLIND:    which image was preferred, and why, before identities were revealed

HARD FAILURES
  - <specific, located, actionable>

QUALITY GAPS
  - <specific, located, actionable, ranked by visual impact>

WHAT WORKS
  - <specific — this prevents regressions in later iterations>
```

---

## 4. Capture requirements

### The acceptance test is differential, not a threshold

A capture is fit for review only if **removing the subject changes the image.**

```
1. Capture the scene.
2. Delete (or hide) the geometry under review.
3. Capture again with identical camera, resolution, and settings.
4. The two images MUST differ. If they are identical, the capture never
   contained the subject and the review is void.
```

**`Tools/capture/capture_frame.py` now performs this automatically.** Every shot
is rendered twice — once normally, once with `PrimitiveRenderMode` suppressing
all primitives — and PASS requires the two to differ. You do not have to run the
differential by hand; you do have to believe the FAIL when you get one. The
criterion lives in `Tools/capture/verdict.py` and its tests (`tests/`) feed it
known-empty frames and require rejection, so the check is itself checked.

This replaces an earlier criterion — `max > 0` and `unique > 10` over the RGB
array — which was **wrong and actively harmful**, and the reason it is spelled
out here is that it cost real time and produced a false report of success.

A scene containing SkyAtmosphere or VolumetricCloud renders a rich, plausible
gradient **without a single primitive being drawn**. That easily clears both
thresholds. The check therefore certified an empty sky as a working capture:
`max=182 unique=1672 mean=88.660` before deleting every mesh in the level, and
byte-identical numbers after. No threshold on a single image can tell content
from background, because the background alone satisfies any of them.

Note what a differential test rules out that an absolute one cannot: an unlit,
black, or wrongly-shaded mesh still **occludes** what is behind it, so the image
still changes. A byte-identical result means the geometry is genuinely absent
from the render, not merely mis-shaded — which points at the scene, not the
lighting.

Corollaries worth holding onto:

- **A metric that cannot fail is not a check.** Before trusting any automated
  pass criterion, construct the input that *should* fail it and confirm it does.
- **Look at the image.** This was caught by opening the PNG, not by reading
  numbers. Every number was consistent with success.
### Other capture requirements

- Renders are compared at **identical resolution and framing**. A resolution
  mismatch invalidates a blind comparison.
- Prefer **native resolution** captures. Upscaled images hide aliasing, which
  is exactly what the review is looking for.
- Any capture used in a review must record the profile, the map, the camera
  transform, and the resolution, so a finding can be reproduced.

See `Tools/capture/README.md` for the working capture path on this platform.
