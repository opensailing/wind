# ADR 002 — Runtime volume rendering path

- **Status:** Accepted
- **Date:** 2026-08-04
- **Affects:** `Plugins/FlowVizRuntime/Shaders/`,
  `Plugins/FlowVizRuntime/Source/FlowVizRuntime/{Public,Private}/Render/`,
  `Docs/OPENFOAM_PARAVIEW_PARITY.md` §2

## Context

FlowViz must display an animated scalar volume that is simultaneously

1. **quantitatively honest** — a scientist reads values off it, and `plan.md`
   §4 forbids silently normalizing, quantizing, clamping, smoothing or
   discarding field values; and
2. **visually excellent** — the stated bar is that a critic comparing a
   FlowViz Presentation render blind against a film or AAA reference cannot
   say the reference is clearly better.

Those pull in different directions, which is why `VISUAL_QA.md` defines two
profiles with different bars. The renderer has to serve both from one data
path.

Three candidate paths exist in UE 5.8. We checked what the installed engine
actually provides rather than reasoning from documentation.

### Candidate A — Sparse Volume Texture (SVT)

Present and real in 5.8:

```
Engine/Source/Runtime/Engine/Classes/SparseVolumeTexture/SparseVolumeTexture.h
Engine/Source/Runtime/Engine/Public/SparseVolumeTexture/SparseVolumeTextureData.h
Engine/Source/Runtime/Engine/Public/SparseVolumeTexture/ISparseVolumeTextureStreamingManager.h
Engine/Source/Runtime/Engine/Public/Materials/MaterialExpressionSparseVolumeTextureSample.h
```

OpenVDB is genuinely available on this platform — not merely licensed:

```
$ file Engine/Source/ThirdParty/OpenVDB/Deploy/openvdb-13.0.0/Mac/lib/libopenvdb.a
Mach-O universal binary with 2 architectures: [x86_64] [arm64]
```

So the SVT path is not blocked by a missing dependency, which is the failure
mode ADR 005 hit with zstd.

Its problem is different: SVT is an **imported asset** pipeline. It expects
data to arrive as a cooked `USparseVolumeTexture`, streamed by the engine's
own streaming manager. `plan.md` §9 is explicit on this point — *"Do not make
imported Unreal assets the only way to load data"* and *"Do not require VDB
conversion for ordinary runtime playback."* A remote solver writing CVF frames
that must round-trip through a VDB conversion and an asset import before
anything appears on screen is not a runtime path; it is an offline pipeline
wearing one.

SVT also gives us the engine's sampling and compression policy, not ours. Its
formats are lossy-by-design for the cinematic case. That is precisely the
"silently quantize" behaviour §4 rule 5 forbids for the Scientific profile,
and we would not be able to state what happened to a value between the solver
and the screen.

### Candidate B — Proxy-volume material driven by dynamic 3D textures

A box mesh with a material that ray-marches a `Texture3D` sampled through
material expressions.

Workable and the cheapest thing to stand up. But the required feature list in
§9 includes early ray termination, configurable step count, per-ray jitter,
central-difference gradients, multiple clipping planes, crop box, and explicit
NaN/masked/out-of-range rejection. Expressing loop control and early-out in
the material graph is possible but fights the graph the entire way, and the
resulting node mess is unreviewable — which matters because this is the file a
harsh critic's findings will land in repeatedly.

### Candidate C — Custom global shader + scene proxy

A `FGlobalShader` ray-marcher over persistent RHI 3D textures, driven by a
scene proxy, with the volume uploaded on the render thread.

Costs the most up front. Buys complete control of sampling, compositing,
precision and invalid-value policy — the things both profiles are graded on.

## Decision

**Candidate C: a custom global-shader ray-marcher over persistent RHI 3D
textures, for both profiles.**

One renderer serves both. The profiles differ in *parameters and
post-processing*, not in code path:

| | Scientific | Presentation |
| --- | --- | --- |
| Compositing | front-to-back alpha, MIP, MinIP, average, iso | front-to-back alpha with scattering |
| Lighting | neutral / unlit — lighting must not modulate apparent scalar value | gradient lighting, self-shadowing, multiple scattering |
| Post | none that alters apparent values | bloom, DoF, tonemapping as required |
| Invalid data | rendered distinctly, never as zero or colormap minimum | same |

Sharing the sampler is deliberate: if the two profiles read the volume through
different code, a Presentation render can disagree with the Scientific render
about what the data *is*, and only one of them is checkable.

**SVT is kept as an optional, feature-flagged Presentation-only path**, exactly
as §9 permits. It is never the runtime renderer and never required for
playback.

## Consequences

- We own the ray-march loop, so §9's feature list is implementable directly
  rather than negotiated with a material graph.
- We own precision. Field values reach the shader in their stored type; any
  conversion is ours and is documented. This is what makes rule 5 ("do not
  silently normalize, quantize, clamp, smooth or discard") enforceable rather
  than aspirational.
- We own the invalid-value path. NaN, masked cells and out-of-range values are
  rejected explicitly in the shader, satisfying rule 10, instead of arriving
  as whatever the engine's sampler decided.
- **Cost:** global shaders must be maintained across engine upgrades, and
  shader compilation errors surface as runtime failures rather than build
  failures. Mitigated by keeping RHI texture creation/update behind the
  version-isolated wrapper (`Render/FlowVizVolumeTexture.*`) so a version bump
  touches one file.
- **Cost:** more code before anything appears on screen than Candidate B.
  Accepted; Candidate B's ceiling is below the stated visual bar.
- Anisotropic voxel spacing is a first-class shader parameter, not an
  assumption. The default mock domain is 12 m × 4 m × 1 m over 128 × 64 × 24
  cells, so voxels are genuinely non-cubic and any code assuming otherwise is
  wrong on the primary dataset.

## Open questions

- Multiple scattering in the Presentation profile is the single biggest
  contributor to "film-grade" per `VISUAL_QA.md` §2, and is not yet designed.
  Single-scattering-only smoke reads flat and dead, which is a hard failure
  against the stated bar.
- Temporal stability under camera motion is unproven. A still frame that looks
  perfect and shimmers in motion has failed, and per-ray jitter — which we need
  against banding — is a common cause of exactly that.
