# FluidX3D sphere-wake sample recipe

This directory is the exact altered-source recipe used to generate
`Samples/FluidX3DSphereWake.cfdviz`. The simulation runs in FluidX3D; FlowViz
only converts and renders the resulting velocity and flag volumes.

## Pinned solver

- Origin: <https://github.com/ProjectPhysX/FluidX3D>
- Revision: `024e48c23256a31346cf458fba76deae4aca7869`
- Configuration: D3Q19, SRT, FP16S, equilibrium inlet/outlet boundaries,
  headless (benchmark and graphics modes disabled)
- Lattice: `192 x 96 x 96`
- Obstacle: sphere, diameter 24 cells, centered at `(48, 48, 48)`
- Inlet speed: `0.06` lattice cells/step
- Reynolds number: 200
- Warm-up: 6,000 solver steps
- Stored sequence: 40 snapshots, steps 6000 through 6390 at stride 10
- Exported fields: velocity `u` and lattice `flags`, both with SI conversion off

A small deterministic cross-flow perturbation breaks exact lattice symmetry.
It does not prescribe the wake: the stored temporal flow is the result of the
three-dimensional external LBM solve.

## Reproduce

From the FlowViz project directory:

```bash
Tools/generate_fluidx3d_sample.sh \
  --source /absolute/path/to/FluidX3D \
  --work-dir "$HOME/projects/flowviz-fluidx3d-sphere-wake"
```

The script clones with `--no-local` into an isolated checkout, detaches at the
pinned revision, copies `sphere_wake_setup.cpp`, applies `defines.patch`, builds,
runs, converts, validates, qualifies, benchmarks, and creates a release archive.
It never builds in or writes to the source checkout, including when that checkout
contains unrelated uncommitted work.

The generated release includes the complete altered FluidX3D source tree and its
unchanged `LICENSE.md`, not only these two diffs. That is required if results from
an altered FluidX3D version are published.

## License and distribution boundary

FluidX3D's custom license permits public research, education, and personal use,
but prohibits commercial and military use. It prohibits training AI models on
the original or altered source code, requires altered source code to accompany
published binaries, data, or results generated from that altered source, and
asks scientific publications arising from FluidX3D to cite its listed articles.
Read the pinned `LICENSE.md` before using or distributing the source, case, or
archive. FlowViz's downloader must not be pointed at a public artifact until
those use, citation, and source-distribution conditions have been reviewed for
the intended release.
