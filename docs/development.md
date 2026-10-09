# Development workflow

Start with the [README quickstart](../README.md) and [macOS setup](./setup.md). The maintained `Tools/` entry points own Unreal process serialization, logs, and cleanup. Read the relevant script before adding a new build or launch command.

## Source and generated files

- Project sources: `Source/LBMStudio/`, `Config/`, and `Tools/`.
- Supplied sample inputs and assets: `Content/Samples/`, plus tracked `.uasset` files.
- Package resources: `Build/Mac/Resources/`.
- Generated output: `Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/`, `Packaged/`, `tmp/`, and staged `Content/ThirdParty/Home4CAD/` are ignored.

Sample imports and material generation can change tracked assets. Review `git diff` after build or validation. Keep scratch work under `tmp/`. Maintain `Tools/headless-tests.json` alongside changes to headless test names; it is the source of truth for the catalog.

## Safe Unreal execution

Build, test, package, and launch commands share `Tools/runtime_lane.py` and must run serially. Let the active owner finish or close the app normally. Never remove the lock or kill unrelated Unreal or crash-reporter processes. Keep source unchanged during validation so recorded fingerprints remain valid.

Use `Tools/build.sh` for the Editor target and material generation. Unreal material scripts run in `UnrealEditor-Cmd` with Unreal's bundled Python; do not run them with host Python. The launchers do not build missing executables. Build before first launch or package.

## Headless validation

Stage the CAD runtime before running the complete headless catalog:

```sh
python3 Tools/validate.py --suite Studio.FlowConditions.  # Compile, then matching catalog tests
python3 Tools/validate.py                                 # Compile, then full headless catalog
python3 Tools/validate.py --no-build --suite Studio.FlowConditions. # Existing module only
python3 Tools/validate_render.py                           # Compile, then Metal renderer checks
python3 Tools/validate_render.py --no-build                # Existing module only
```

Choose a suite prefix from `Tools/headless-tests.json`. The default headless validator uses NullRHI and runs the cataloged tests; the render validator exercises the production renderer on Metal without a window. Both write timestamped run directories under `tmp/debug/`, including `summary.json` and logs. A zero exit status and `passed: true` indicate success. Inspect the newest summary and logs for details. `--no-build` tests an existing module and does not compile current source.

These checks are focused verification, not complete product acceptance. Native dialogs, visual appearance, physical input, packaged-app stability, and long-session behavior need their own targeted workflows. HOME4 solver behavior remains stubbed; tests use clearly identified fixtures.

## Packaging

The package sequence is:

```sh
Tools/build.sh
Tools/package.sh
Tools/run.sh --packaged
```

The package script refreshes staged CAD, builds the Game target with `-NoUBA`, runs Unreal's separate `ApplePostBuildSync` step, then cooks, stages, packages, and verifies the app. Preserve those post-build steps. `Tools/package.sh` produces a local Development package. For distribution, follow [Apple's Developer ID guidance](https://developer.apple.com/developer-id/): sign nested code and the app, enable hardened runtime, retain document-access entitlements, remove `com.apple.security.get-task-allow`, notarize, and staple. Check the result with `python3 Tools/verify_macos_package.py Packaged/Mac/LBMStudio.app --distribution`, then verify startup on the recipient's Mac.

For packaged Development builds, preserve the `-LLM` startup workaround. Build 56057345.0.222 may relaunch itself to add that flag and hit macOS `forbidden-sandbox-reinit`; supplying `-LLM` directly avoids that restart. The maintained launcher and startup implementation handle the flag without a second image execution. Keep existing `-LLM` handling in build and validation commands.

## Further reference

See the [implementation reference](./reference.md) for controls, data formats, architecture and historical verification notes. Its `tmp/` reports are local artifacts; they are not included in a fresh clone.
