#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite ScientificAcceptance.PerformanceUI.MeasurePauseCameraAndRestore --count 1 --name performance --captures PerformanceUI "$@"
