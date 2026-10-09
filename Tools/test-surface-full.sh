#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $# -ne 2 ]]; then
    echo 'Usage: Tools/test-surface-full.sh /path/to/recording.json /path/to/reconstruction.json' >&2
    exit 2
fi
exec python3 Tools/run_packaged_suite.py --suite ScientificAcceptance.Surface.FullSequence \
    --count 1 --name surface-full-sequence --captures SurfaceSequence --timeout 360 \
    --startup-profile application-default --point-recording "$1" --surface-reconstruction "$2"
