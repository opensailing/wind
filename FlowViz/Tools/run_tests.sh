#!/usr/bin/env bash
#
# Run FlowViz C++ automation tests headlessly and report a real pass/fail.
#
#   ./Tools/run_tests.sh                      # all FlowViz tests
#   ./Tools/run_tests.sh FlowViz.CFDViz       # a subtree
#   ./Tools/run_tests.sh FlowViz.CFDViz.Crc32C
#   RHI=1 ./Tools/run_tests.sh FlowViz.Render # tests that need a real RHI
#
# Notes on the flags, which are not obvious and cost real time to rediscover:
#
#   -abslog=<path>  REQUIRED to capture the engine log. Without it the useful
#                   output is swallowed and stdout shows only trace-daemon
#                   chatter, which looks exactly like a silent failure.
#   -notrace        Suppresses the UnrealTrace daemon noise on stdout.
#   -nullrhi        No GPU. Fast, and correct for logic tests. Omit it (RHI=1)
#                   for anything that renders.
#   -unattended -nopause -nosplash   No dialogs; required for CI/agent use.
#
# Exit code is the engine's: 0 = all requested tests passed.

set -uo pipefail

FILTER="${1:-FlowViz}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPROJECT="${PROJECT_DIR}/FlowViz.uproject"
UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
EDITOR_CMD="${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd"
LOG="${LOG:-/tmp/flowviz_tests.log}"

if [[ ! -x "${EDITOR_CMD}" ]]; then
    echo "error: UnrealEditor-Cmd not found at ${EDITOR_CMD}" >&2
    echo "       set UE_ROOT to your engine install" >&2
    exit 2
fi

RHI_FLAG="-nullrhi"
[[ "${RHI:-0}" == "1" ]] && RHI_FLAG=""

echo "Running automation tests matching '${FILTER}'..."

"${EDITOR_CMD}" "${UPROJECT}" \
    -ExecCmds="Automation RunTests ${FILTER}; Quit" \
    -unattended -nopause -nosplash -notrace ${RHI_FLAG} \
    -abslog="${LOG}" >/dev/null 2>&1
ENGINE_EXIT=$?

if [[ ! -f "${LOG}" ]]; then
    echo "error: no log produced at ${LOG}; the editor failed to start" >&2
    exit 3
fi

echo
grep -E "Test Completed\. Result=" "${LOG}" \
    | sed -E 's/.*Result=\{([^}]*)\} Name=\{([^}]*)\} Path=\{([^}]*)\}.*/  \1  \3/' \
    | sort -u

# Surface the actual assertion messages for anything that failed.
if grep -q "Result={Fail}" "${LOG}"; then
    echo
    echo "Failure details:"
    grep -E "LogAutomationController: (Error|Warning)|Expected|TestEqual|TestTrue" "${LOG}" \
        | sed 's/^/  /' | head -60
fi

echo
FOUND=$(grep -cE "Test Completed\. Result=" "${LOG}")
PASSED=$(grep -cE "Test Completed\. Result=\{Success\}" "${LOG}")

if [[ "${FOUND}" -eq 0 ]]; then
    # An empty filter match exits 0, which would otherwise read as success.
    echo "FAIL: no tests matched '${FILTER}'. Check the test path." >&2
    echo "Full log: ${LOG}"
    exit 4
fi

echo "${PASSED}/${FOUND} passed. Full log: ${LOG}"
[[ "${PASSED}" -eq "${FOUND}" && "${ENGINE_EXIT}" -eq 0 ]] || exit 1
