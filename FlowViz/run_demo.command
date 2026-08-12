#!/bin/bash
# FlowViz representative demo: double-click me, or run from a terminal.
#
# This opens the genuine 3D FluidX3D sphere wake. The simulation was computed
# externally; FlowViz only imports and renders its time-varying velocity field.
set -euo pipefail

cd "$(dirname "$0")"
APP="$PWD/Packaged/Mac/FlowViz.app"
PACKAGED_CASE="$APP/Contents/UE/FlowViz/Samples/FluidX3DSphereWake.cfdviz"
SOURCE_CASE="$PWD/Samples/FluidX3DSphereWake.cfdviz"

if [[ ! -d "$APP" ]]; then
    printf 'FlowViz is not packaged yet: %s\n' "$APP" >&2
    printf 'Build the application using the Package command in README.md.\n' >&2
    exit 1
fi

if [[ -f "$SOURCE_CASE/manifest.json" ]]; then
    CASE="$SOURCE_CASE"
elif [[ -f "$PACKAGED_CASE/manifest.json" ]]; then
    CASE="$PACKAGED_CASE"
else
    printf 'The representative FluidX3D demo is not installed.\n' >&2
    printf 'Generate it with: Tools/generate_fluidx3d_sample.sh\n' >&2
    printf 'Or install a local release with: Tools/download_demo_case.sh --archive Samples/FluidX3DSphereWake.cfdviz.release.tar.gz\n' >&2
    exit 1
fi

open "$APP" --args -case="$CASE" -field=U
