#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 Tools/run_packaged_suite.py --suite Studio.SurfaceRendering. --count 3 --name surface-render --captures SurfacePixels --timeout 120
