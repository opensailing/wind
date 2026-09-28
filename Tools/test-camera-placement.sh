#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite Studio.CameraPlacement. --count 5 --name camera-placement \
    --width "${1:-1320}" --height "${2:-740}" --captures CameraPlacement
