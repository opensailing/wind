#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite Studio.CommandUI. --count 1 --name commands --width "${1:-1320}" --height "${2:-740}" --captures CommandUI --timeout 150
