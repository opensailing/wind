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

# --summarize <log> reports on an existing log without launching the editor.
# It is the seam Tools/tests/test_run_tests_summary.sh drives, so the reporting
# logic can be tested over fixture logs instead of a ~40s engine run.
SUMMARIZE_ONLY=""
if [[ "${1:-}" == "--summarize" ]]; then
    SUMMARIZE_ONLY=1
    LOG="${2:?--summarize needs a log path}"
    shift 2 || true
fi

FILTER="${1:-FlowViz}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPROJECT="${PROJECT_DIR}/FlowViz.uproject"
UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
EDITOR_CMD="${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd"

# PER-CHECKOUT, NOT SHARED. This used to default to a fixed
# /tmp/flowviz_tests.log. Nine worktrees exist so concurrent agents cannot
# corrupt each other's verdicts, and every one of them wrote its detail log to
# that one path -- handing back the isolation at the last step.
#
# Observed 2026-08-05: a run finished at 14:33:23 and the file at that path was
# dated 14:33:30, holding a different run's editor startup and zero test
# records, because a peer overwrote it between the run and the read. The failure
# direction is what makes it dangerous: an unrelated log sitting at a
# known-good path reads as evidence about YOUR run, so reconciling a suspicious
# total against it yields a confident wrong answer.
#
# Derived from PROJECT_DIR so it is stable across invocations from the same
# checkout (--summarize must still find the log a previous run wrote) and
# distinct between checkouts. An explicit LOG always wins.
_checkout_tag="$(printf '%s' "${PROJECT_DIR}" | shasum | cut -c1-8)"
LOG="${LOG:-/tmp/flowviz_tests_${_checkout_tag}.log}"

# Report where the log would go, without launching an engine. The tests assert
# on this rather than recomputing the formula, so a duplicated expression cannot
# pass while the real default stays shared.
if [[ "${1:-}" == "--print-log-path" ]]; then
    printf '%s\n' "${LOG}"
    exit 0
fi

if [[ -n "${SUMMARIZE_ONLY}" ]]; then
    # Reporting only: no engine, and no engine exit code to fold in.
    ENGINE_EXIT=0
else
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
fi

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

# --- tests that passed without verifying anything ----------------------------
#
# A test that skips itself -- no GPU, no fixture, no network -- still reports
# Result={Success} to the engine, so it lands in PASSED above. The count is
# accurate and the sentence "47/47 passed" is still misleading, because three
# of those 47 had checked nothing. Report them by name.
#
# Attribution is by the most recent "Test Started", NOT the nearest Completed:
# a test emits its skip message from inside its own body, so the marker lands
# after that test's Completed line and immediately before the next test's
# Started line. Keying on Completed blames the wrong test every time.
SKIPPED_NAMES="$(awk '
    /Test Started\. .*Path=\{/ {
        match($0, /Path=\{[^}]*\}/)
        current = substr($0, RSTART + 6, RLENGTH - 7)
    }
    /SKIPPED/ && current != "" && !seen[current]++ { print current }
' "${LOG}")"

SKIPPED=0
[[ -n "${SKIPPED_NAMES}" ]] && SKIPPED="$(printf '%s\n' "${SKIPPED_NAMES}" | wc -l | tr -d ' ')"

if [[ "${SKIPPED}" -gt 0 ]]; then
    echo "${SKIPPED} skipped -- these reported success having verified nothing:"
    printf '%s\n' "${SKIPPED_NAMES}" | sed 's/^/  /'
    echo "  (reasons are in the log; GPU tests need RHI=1)"
    echo
fi

echo "${PASSED}/${FOUND} passed. Full log: ${LOG}"

# A skip is not a failure. Turning the suite red for it would get the signal
# suppressed the first time someone ran without a GPU, which is the opposite
# of the point.
[[ "${PASSED}" -eq "${FOUND}" && "${ENGINE_EXIT}" -eq 0 ]] || exit 1
