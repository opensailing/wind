#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite Studio.GeometryEditUI. --count 1 --name geometry-edit \
    --width "${1:-1320}" --height "${2:-740}" --captures GeometryEdit --timeout 150
