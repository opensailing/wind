# LBM Solver Studio

A native CFD workstation built with **Unreal Engine 5**. Explore flow fields in 3D, position cameras, compare recordings, and export data or video.

**Current status:** the frontend works with published CFD recordings. The custom **HOME4 lattice Boltzmann solver is stubbed**; you do not need it to develop the app.

## Get running

### 1. Install the prerequisites

The maintained workflow targets **Apple Silicon macOS**.

| Tool | Version / requirement |
| --- | --- |
| Unreal Engine | **5.8.1**, installed through the Epic Games Launcher |
| Xcode | Full Xcode compatible with UE 5.8, plus the **Metal Toolchain** |
| Command-line tools | **Python 3.11+**, Git and `ripgrep` (`rg`) |
| FreeCAD | Pinned **1.1.0 ARM64 / Python 3.11** build for CAD, full tests and packaging |

**New machine? Use [Mac setup](docs/setup.md) to install these tools.** It includes the exact FreeCAD download and the Xcode 27 compatibility adjustment used by the reference machine.

### 2. Clone and build

```sh
git clone https://github.com/opensailing/wind.git
cd wind
export UE_ENGINE_PATH="/Users/Shared/Epic Games/UE_5.8"

python3 Tools/stage_home4_cad_runtime.py Content/ThirdParty/Home4CAD
Tools/build.sh
```

`UE_ENGINE_PATH` points to the folder **containing `Engine/`**. CAD staging defaults to `/Applications/FreeCAD.app/Contents/Resources`.

The build verifies the bundled samples, compiles C++, and prepares materials. Success prints **`Build and runtime materials ready.`** Logs are in `tmp/debug/`.

### 3. Launch

```sh
Tools/run.sh
```

The app opens in Unreal's standalone mode with a bundled airfoil recording. Orbit with left-drag, fly with right mouse + WASD, and press **F** to fit the domain. **F1** opens in-app help.

## Develop and test

Close the app before building or testing. Run Unreal commands **one at a time**.

| Task | Command |
| --- | --- |
| Build and run all headless tests | `python3 Tools/validate.py` |
| Check one feature | `python3 Tools/validate.py --suite Studio.FlowConditions.` |
| Check rendering on Metal, without a window | `python3 Tools/validate_render.py` |
| Build a Mac app bundle | `Tools/package.sh` |
| Run the packaged app | `Tools/run.sh --packaged` |

Validation needs no screenshots or desktop control. Look for **`passed: true`** in the new `tmp/debug/headless-*/summary.json` or `tmp/debug/windowless-render-*/summary.json`.

Packaging produces **`Packaged/Mac/LBMStudio.app`**. It includes the runtime dependencies; recipients do not need Unreal or FreeCAD. Distribution signing and notarization are a separate step—see [packaging](docs/development.md#packaging).

## Working with an AI agent

- Read [setup](docs/setup.md) and [development](docs/development.md) before changing build commands. Use the scripts in `Tools/`.
- Use headless tests for routine checks. Add tests to the [headless catalog](Tools/headless-tests.json); use Metal validation for renderer changes.
- Keep Unreal jobs serial. Let the launcher manage its processes; leave unrelated apps and crash reporters alone.
- Keep HOME4 stubbed and preserve CFD provenance. Use published recordings or explicitly labeled test fixtures.
- Make focused commits. Put scratch work in `tmp/`; review regenerated assets before committing.

## Find your way around

| Path | Contents |
| --- | --- |
| [`Source/LBMStudio/`](Source/LBMStudio/) | C++ application, Slate UI, rendering and tests |
| [`Config/`](Config/) | Unreal project settings |
| [`Tools/`](Tools/) | Build, launch, validation, packaging and data import scripts |
| [`Content/Samples/`](Content/Samples/) | Bundled recordings, fixtures and source attribution |
| [`feedback/`](feedback/) | Product requirements supplied by users |

For controls, data formats and architecture, see the [implementation reference](docs/reference.md). Historical test reports there describe specific checkpoints; run the checks above for your current checkout.
