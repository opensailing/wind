#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite Studio.SavedComparisonUI. --count 1 --name saved-comparison --captures SavedComparisonUI "$@"
