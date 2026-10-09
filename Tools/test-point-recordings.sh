#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite Studio.Point --count 8 --name point-recordings
