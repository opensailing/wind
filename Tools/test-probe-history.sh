#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite Studio.ProbeMonitorUI. --count 1 --name probe-history --captures ProbeMonitorUI "$@"
