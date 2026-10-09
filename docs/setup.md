# macOS setup

This guide prepares an Apple Silicon Mac for LBM Solver Studio with Unreal Engine 5.8.1. Start with the [README quickstart](../README.md) for clone, stage, build, and run commands. Windows is listed in the project descriptor, but the maintained scripts target macOS ARM64. HOME4's solver remains stubbed; no solver repository or GPU cluster is needed.

## Install prerequisites

- **Unreal Engine 5.8.1** with Mac platform support, installed separately from this repository. The default path is `/Users/Shared/Epic Games/UE_5.8`. See [Epic's download page](https://www.unrealengine.com/download).
- **Full Xcode** compatible with macOS and UE 5.8. Epic's [Mac requirements](https://dev.epicgames.com/documentation/en-us/unreal-engine/macos-development-requirements-for-unreal-engine) recommend Xcode 26.1.1 and exclude 26.4. Command Line Tools alone are insufficient.
- **Metal Toolchain**, installed in Xcode Components or with `xcodebuild -downloadComponent MetalToolchain`. See [Apple's component guide](https://developer.apple.com/documentation/xcode/downloading-and-installing-additional-xcode-components).
- **Python 3.11+, Git, and ripgrep (`rg`)** on `PATH`. Base build and validation use the Python standard library.
- **FreeCAD 1.1.0 ARM64 / Python 3.11** for STEP/IGES preparation, the full headless suite, and packaging. Install and stage it below.

Select the full Xcode installation, finish first launch, and install Metal:

```sh
sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer
sudo xcodebuild -runFirstLaunch
xcodebuild -downloadComponent MetalToolchain
```

Complete any Xcode license prompt. If Xcode has a different name or location, update the selected path. See [Apple's Xcode selection guidance](https://developer.apple.com/library/archive/technotes/tn2339/_index.html).

### Xcode 27

UE 5.8.1 has been built on the reference machine with macOS 27 and Xcode 27.0 after a **local engine-only** edit to `Engine/Config/Apple/Apple_SDK.json`, raising `MaxVersion` from `26.9.0` to `27.9.0`. This bypasses the SDK version check; it does not establish Epic support for Xcode 27. Prefer Epic's compatible Xcode for a new setup. If reproducing that machine, back up the engine file and change only `MaxVersion`. Do not add a project SDK override: with an installed engine it can redirect precompiled Mac modules into missing project binary paths.

## Configure and preflight

Clone the repository using [README step 2](../README.md#2-clone-and-build). Run the remaining commands here from the repository root before continuing with the build.

From a terminal, set `UE_ENGINE_PATH` to the directory **containing** `Engine/` (not `Engine/` or `UnrealEditor.app`). The scripts default to the path below if it is unset. Re-export it in each terminal or persist it in your shell profile.

```sh
export UE_ENGINE_PATH="/Users/Shared/Epic Games/UE_5.8"
uname -m                              # arm64
xcode-select -p                       # full Xcode developer directory
xcodebuild -version
xcrun --sdk macosx metal -v            # Metal compiler runs
python3 -c 'import sys; print(sys.version); assert sys.version_info >= (3, 11)'
rg --version
cat "$UE_ENGINE_PATH/Engine/Build/Build.version"
test -x "$UE_ENGINE_PATH/Engine/Build/BatchFiles/Mac/Build.sh"
test -x "$UE_ENGINE_PATH/Engine/Binaries/Mac/UnrealEditor-Cmd"
test -x "$UE_ENGINE_PATH/Engine/Binaries/ThirdParty/DotNet/10.0/mac-arm64/dotnet"
```

Keep UE 5.8.1 for the initial build. Unreal's bundled .NET and Python serve its build and material tools; no system .NET or `pip install unreal` is needed. A generated Xcode workspace is unnecessary for the command-line workflow.

## Install and stage FreeCAD

Use the pinned [FreeCAD 1.1.0 macOS ARM64 / Python 3.11 DMG](https://github.com/FreeCAD/FreeCAD/releases/download/1.1.0/FreeCAD_1.1.0-macOS-arm64-py311.dmg). Its SHA-256 is listed in the official [release checksum file](https://github.com/FreeCAD/FreeCAD/releases/download/1.1.0/FreeCAD_1.1.0-macOS-arm64-py311.dmg-SHA256.txt):

```sh
mkdir -p tmp/setup
curl --fail --location --retry 3 \
  'https://github.com/FreeCAD/FreeCAD/releases/download/1.1.0/FreeCAD_1.1.0-macOS-arm64-py311.dmg' \
  --output tmp/setup/FreeCAD_1.1.0-macOS-arm64-py311.dmg
printf '%s\n' '52b069f86471ccf4fdd535c42cd9b74b9a8079a7abfd0f51ff19b0a30c6d795b  tmp/setup/FreeCAD_1.1.0-macOS-arm64-py311.dmg' | shasum -a 256 -c -
```

After the checksum passes, mount the DMG and copy `FreeCAD.app` to `/Applications`. Then stage the relocatable runtime:

```sh
export HOME4_FREECAD_RESOURCES="/Applications/FreeCAD.app/Contents/Resources"
python3 Tools/stage_home4_cad_runtime.py Content/ThirdParty/Home4CAD
```

If FreeCAD is elsewhere, point `HOME4_FREECAD_RESOURCES` at its `Contents/Resources` directory. FreeCAD's GUI need not stay open. Staging checks versions, builds, checksums, native binaries, and an isolated CAD operation. On first use it downloads upstream licenses and pinned archives. The generated runtime is Git-ignored. Use `--refresh` after helper or dependency updates to replace a recognized runtime transactionally. Do not weaken the checks to accept a different FreeCAD build.

## Dependencies by workflow

Base builds and validation need no host `pip` packages. Optional scientific audits use Pillow, NumPy, VTK, and `ffmpeg`/`ffprobe`; importing original HDF5 additionally uses `h5py`, and the Python HOME4 archive utility uses NumPy. Install these only for the workflow that needs them, preferably in an isolated environment:

```sh
python3 -m venv tmp/venv-audit
```

Unreal material scripts run inside `UnrealEditor-Cmd`, not host Python. For missing CAD interpreter or dependency errors, repeat staging with the pinned installer and correct Resources path; inspect `Content/ThirdParty/Home4CAD/runtime-manifest.json`.

## Troubleshooting

- **Engine path rejected or executable missing:** set `UE_ENGINE_PATH` to the parent of `Engine/`; check `Engine/Build/Build.version` and UE 5.8.1.
- **Invalid Mac SDK/Xcode range:** inspect `xcode-select -p`, `xcodebuild -version`, and the selected engine's `Apple_SDK.json`. Use a compatible full Xcode; the Xcode 27 procedure above is a reference-machine exception.
- **Metal compiler missing:** finish Xcode first launch, install Metal, and rerun `xcrun --sdk macosx metal -v`.
- **`No module named unreal`, material missing, or Editor module missing:** run `Tools/build.sh`; inspect relevant logs in `tmp/debug/`.
- **`Unreal execution is in use`:** wait for the active build, test, or app to finish. Do not remove the runtime lock or terminate unrelated Unreal processes.

`Tools/build.sh` does **not** stage CAD. The full headless suite and packaging require the staged runtime. See the [development guide](./development.md) for validation and package commands.
