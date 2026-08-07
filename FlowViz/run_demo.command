#!/bin/bash
# FlowViz demo: double-click me, or run from a terminal.
#
# vorticityMagnitude, not U: the velocity field is ~uniform free-stream at the
# domain boundary, so from outside it reads as a featureless bright slab -- the
# wake structure is INSIDE. Vorticity is near zero in free stream and high in
# the vortex street, so the shedding is visible from the first frame.
cd "$(dirname "$0")"
open Packaged/Mac/FlowViz.app --args \
    -case="$PWD/Packaged/Mac/FlowViz.app/Contents/UE/FlowViz/Samples/MockCylinderWake.cfdviz" \
    -field=vorticityMagnitude
