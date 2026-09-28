#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite Studio.Domain. --count 2 --name domain \
    --width "${1:-1320}" --height "${2:-740}" --captures Domain --timeout 150
