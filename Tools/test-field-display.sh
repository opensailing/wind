#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite Studio.FieldDisplay. --count 5 --name field-display \
    --width "${1:-1320}" --height "${2:-740}" --captures FieldDisplay
