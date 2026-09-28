#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 Tools/run_packaged_suite.py --suite ScientificAcceptance.Residuals.PublishedFullLog --count 1 --name residuals --captures ResidualAudit --residual-log "${1:?Provide the original published flowTorch log.pimpleFoam path}"
