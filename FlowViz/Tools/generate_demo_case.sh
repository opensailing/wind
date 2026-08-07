#!/bin/bash
# Generate the film-tier demo case (renderer overhaul P4).
#
# 168x84x36 x 40 frames, ~400 MB -- deliberately NOT committed (gitignored);
# run this once and point the app at the result:
#   open Packaged/Mac/FlowViz.app --args -case="$PWD/Samples/MockCylinderWakeHiRes.cfdviz"
set -euo pipefail
cd "$(dirname "$0")/cfdviz"
PYTHONPATH=src python3 -m cfdviz generate-mock --high-res \
    --output ../../Samples/MockCylinderWakeHiRes.cfdviz
PYTHONPATH=src python3 -m cfdviz validate ../../Samples/MockCylinderWakeHiRes.cfdviz
echo "Hi-res demo case ready: Samples/MockCylinderWakeHiRes.cfdviz"
