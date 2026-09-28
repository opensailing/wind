# Datasets

<!-- This file is intended to live at data_prep/README.md — the relative paths below
     assume that location. If you reuse it as the Zenodo/Figshare dataset description,
     replace the GIF paths with absolute raw.githubusercontent.com URLs (Zenodo does
     not resolve repo-relative paths). -->

Four transient CFD datasets accompany *From compression to discovery: representations whose structure reveals the governing equations* — three 2-D flows and one 3-D validation case, all simulated in ANSYS Fluent and exported as time-resolved velocity and pressure fields at every exported mesh point.

<table>
  <tr>
    <td align="center">
      <img src="../docs/assets/bfs.gif" alt="Backward-facing step"/><br/>
      <b>Backward-facing step</b><br/><sub>URANS (realizable k-ε), Re_h = 6,846</sub>
    </td>
    <td align="center">
      <img src="../docs/assets/cylinder_pair.gif" alt="Cylinder pair"/><br/>
      <b>Cylinder pair</b><br/><sub>URANS (SST k-ω), Re_D = 1×10⁵, gap = 1 D</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <img src="../docs/assets/naca0018.gif" alt="NACA0018 pitching wing"/><br/>
      <b>Pitching NACA0018</b><br/><sub>URANS (k-ω SST), f_α ≈ 0.80 Hz, κ ≈ 7.8×10⁻³</sub>
    </td>
    <td align="center">
      <img src="../docs/assets/cylinder3d_re300.gif" width="380" alt="3-D cylinder wake"/><br/>
      <b>Cylinder, 3-D</b><br/><sub>unsteady laminar, Re_D = 300</sub>
    </td>
  </tr>
</table>


## Summary

| Case | Solver | Re | Mesh nodes | Saved steps | Δt between saves | File size |
|---|---|---|---|---|---|---|
| Backward-facing step (`bfs`) | URANS, realizable k-ε | Re<sub>h</sub> = 6,846 | 20,890 | 8,000 | 0.02 s | 6.2 GB |
| Cylinder pair (`cylinder`) | URANS, SST k-ω | Re<sub>D</sub> = 1×10⁵ | ≈15,500 | 8,000 | 0.01 s | 7.5 GB |
| Pitching NACA0018 (`naca0018`) | URANS, k-ω SST | Re<sub>c</sub> ≈ 1.4×10⁵ | ≈19,000 | 8,000 | 2.5×10⁻³ s (every solver step) | 5.6 GB |
| Cylinder, 3-D (`cylinder_re300`) | unsteady laminar | Re<sub>D</sub> = 300 | 296,174 | 5,901 (first 2,000 used in the paper) | 1×10⁻² s (every solver step) | 32.6 GB |

Each dataset contains static pressure, the velocity components, the velocity magnitude, and the per-cell area/volume at every exported mesh point.

### Simulation setup (condensed from SI §S1)

- **Backward-facing step** — 2-D URANS with the realizable k-ε closure and scalable wall functions at Re<sub>h</sub> = 6,846 (air, U<sub>b</sub> = 0.5 m/s through a velocity inlet of height equal to the step height h = 0.2 m). Domain 7.5h × 2h, step 1.5h downstream of the inlet, outflow section 6h; the lower boundary (upstream floor, step face, downstream floor) is a no-slip wall, while the upper and downstream boundaries are constant-pressure outlets, so the upper boundary admits entrainment. SIMPLE pressure-velocity coupling, second-order upwind momentum/turbulence, second-order pressure interpolation. Snapshots are saved every 0.02 s; the 8,000 snapshots span 160 s ≈ 50 cycles of the reattachment flapping (f₁ ≈ 0.31 Hz, St<sub>h</sub> ≈ 0.12).
- **Cylinder pair** — 2-D URANS with the SST k-ω closure at Re<sub>D</sub> = 1×10⁵ (water, U<sub>∞</sub> = 0.5 m/s, D = 0.2 m); two identical cylinders side-by-side, gap G/D = 1 (centre-to-centre spacing 2D). Domain 4D upstream / 21D downstream / ±4D cross-stream; the lateral boundaries are no-slip walls, confining the pair in a plane channel at 25 % solid blockage; constant-pressure outlet; inlet turbulence intensity 5 %, eddy-viscosity ratio 10. SIMPLE coupling, second-order upwind momentum/turbulence, second-order pressure interpolation, first-order implicit time integration. Snapshots are saved every 0.01 s; the 8,000 snapshots span 80 s ≈ 55 cycles of the dominant shedding peak (0.69 Hz).
- **Pitching NACA0018** — 2-D URANS with the k-ω SST closure and no transition model; ideal-gas air at M = 0.074 and 300 K (U<sub>∞</sub> ≈ 25.7 m/s), giving Re<sub>c</sub> ≈ 1.4×10⁵ for chord c = 0.08 m. The effective angle of attack is prescribed by oscillating the far-field flow direction, α(t) = 15° + 10°·sin(ωt) with ω = 5 rad/s (f<sub>α</sub> = ω/2π ≈ 0.80 Hz), sweeping the incidence through [5°, 25°] at reduced frequency κ = ωc/(2U<sub>∞</sub>) ≈ 7.8×10⁻³. SIMPLEC coupling with second-order implicit stepping at Δt = 2.5×10⁻³ s. Snapshots are exported at every solver step (steps 1001–9000; the first 1,000 steps are discarded as initial transient), so the 8,000 snapshots span 20 s ≈ 16 pitching cycles.
- **Cylinder, 3-D (Re = 300)** — 3-D unsteady laminar simulation computed under a constrained budget as a higher-dimension stress test (water, D = 0.01 m, U<sub>∞</sub> = 0.0302 m/s). Domain 10D upstream / 25D downstream / ±10D cross-stream with an 8D span; symmetry lateral and spanwise boundaries, constant-pressure outlet, no-slip cylinder surface. PISO coupling, second-order upwind momentum, second-order pressure interpolation, second-order implicit stepping at Δt = 1×10⁻² s (= 0.030 D/U<sub>∞</sub>, snapshots at every step), at most 35 inner iterations per step. The first 2,000 frames (≈12.6 shedding periods) are used in the paper; the mesh-to-graph construction and preprocessing are shared with the 2-D cases.

Each 2-D case provides 8,000 statistically stationary snapshots, split **chronologically 4 : 1** into train/test so the test segment has no temporal overlap with training. Reconstruction metrics in the paper are averaged over 2,000 randomly sampled snapshots, and the validation split used for sparse regression is the final fifth of the training window.

## Download

Archived at Zenodo — 2-D cases: [10.5281/zenodo.20582405](https://doi.org/10.5281/zenodo.20582405); 3-D case: [10.5281/zenodo.20586598](https://doi.org/10.5281/zenodo.20586598). The archives include the CFD snapshot datasets, mesh coordinates, trained model weights, and the latent trajectories used for topology discovery and equation identification.

The code does not assume any particular location: each case YAML (`configs/flow2d/<case>.yaml`, 3-D under `configs/flow3d/`) points to the files explicitly:

```yaml
paths:
  data_path:  <path to the case .h5>
  csv_path:   <path to a node-coordinate CSV for this case>
  phase1_checkpoint: <path to the shared stage-① checkpoint>
```

A simple layout that works:

```
data/
├── BFS8000_re100.h5
├── cylinder8000_re100.h5
├── naca0018_8000_re100.h5
├── cylinder_3d_re300_ds.h5          # 3-D
└── coords/
    ├── bfs.csv
    ├── cylinder.csv
    └── naca0018.csv                 # any single per-step Fluent export works
```

## HDF5 format

Each case is a single HDF5 file written with `pandas.HDFStore` — **read it with `pandas.read_hdf`, not raw `h5py`**:

| Key | Shape | Content |
|---|---|---|
| `coordinates` | N × 2 | node coordinates (x, y) |
| `x_coords`, `y_coords` | N × 1 | the same coordinates as separate tables |
| `pressure` | N × T | static pressure |
| `velocity_u`, `velocity_v` | N × T | velocity components |
| `velocity_magnitude` | N × T | velocity magnitude (derivable from u, v; stored for convenience) |
| `cell_volume` | N × T | per-cell area (2-D) / volume (3-D); constant in time, stored per step |

N = mesh nodes, T = saved time steps; time columns are labelled `t_0001 … t_XXXX` by save index.

```python
import pandas as pd

f = "naca0018_8000_re100.h5"
coords = pd.read_hdf(f, "coordinates").values   # (N, 2)
u = pd.read_hdf(f, "velocity_u").values.T       # (T, N)
v = pd.read_hdf(f, "velocity_v").values.T       # (T, N)
p = pd.read_hdf(f, "pressure").values.T         # (T, N)
```

**How the model consumes this.** `(u, v, p)` form the three input channels per node; `coordinates` defines the k-NN graph; `cell_volume` provides the area weights of the reconstruction loss; and the four physics-supervision targets (velocity magnitude, vorticity, turbulent intensity, kinetic energy) are computed from these fields at data-loading time (`ImprovedPhysicsCalculator`, k = 5 neighbours).

## Raw Fluent exports

Each saved step is one ASCII CSV named `{PREFIX}-{index:04d}` (e.g. `naca0018-1001 … naca0018-9000`) containing one row per mesh node with columns

```
x-coordinate, y-coordinate, pressure, x-velocity, y-velocity, velocity-magnitude, cell-volume
```

(leading whitespace in headers is tolerated by the processor). For the NACA0018 case, exports correspond to solver steps 1001–9000 at every time step, the first 1,000 steps (2.5 s) having been discarded as initial transient. The per-case index ranges and save intervals are set in each case's configuration; the raw-export parameters are read from the parameter block in `fluent_data_processer.py` rather than edited by hand.

## Regenerating the HDF5 from raw exports

`data_prep/flow2d/fluent_data_processer.py` merges the per-step CSVs into one HDF5 file. It is configured by the parameter block at the top of the script rather than CLI flags:

```python
fluent_path      = "<folder with the per-step CSVs>"
output_path      = "<output folder>"        # "" = same as input
output_filename  = "naca0018_8000_re100.h5"
FILE_PREFIX      = "naca0018"
START_FILE_INDEX, END_FILE_INDEX = 1001, 9000
SAVE_COORDINATES = SAVE_PRESSURE = SAVE_VELOCITY_COMPONENTS = True
SAVE_VELOCITY_MAGNITUDE = SAVE_CELL_VOLUME = True
```

then, from the repository root:

```bash
python data_prep/flow2d/fluent_data_processer.py
```

The script reports any missing files, prints per-field shapes and value ranges, and re-reads the written file as a verification pass. Optional mesh sanity checks live alongside it: `check_grid.py`, `check_cell_area.py`, `check_cell_volume.py`.

**3-D case:** run `data_prep/flow3d/fluent_data_processor_3d.py` followed by `data_prep/flow3d/preprocess_physics_3d.py`, which precomputes the physics-supervision targets offline for the 3-D fields.

## Using the data for training

Point the case YAML at the files (see [Download](#download)) and launch the pipeline from the repository root as described in the [main README](../README.md). Trainer flags that act on the data: `--skip_transient` (drop additional leading steps), `--max_timesteps` (truncate the series), `--train_ratio` (chronological train/test split), `--normalization` (normalization scheme; the constants are saved per run as `normalization_params.*`).

## License & citation

Dataset DOIs: 2-D cases [10.5281/zenodo.20582405](https://doi.org/10.5281/zenodo.20582405), 3-D case [10.5281/zenodo.20586598](https://doi.org/10.5281/zenodo.20586598). If you use these datasets, please cite the paper (see the [main README](../README.md#citation)).

The datasets are released under CC BY 4.0. The accompanying code is released under the [MIT License](../LICENSE).