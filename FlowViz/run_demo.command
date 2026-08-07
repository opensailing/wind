#!/bin/bash
# FlowViz demo: double-click me, or run from a terminal.
cd "$(dirname "$0")"
open Packaged/Mac/FlowViz.app --args \
    -case="$PWD/Packaged/Mac/FlowViz.app/Contents/UE/FlowViz/Samples/MockCylinderWake.cfdviz" \
    -field=U
