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
    # Refuse to run the suite in a checkout with a live mutation campaign,
    # unless we ARE that campaign. Guarding only build_lock.sh would leave this
    # script -- which launches the editor directly and is routinely run bare --
    # free to run inside the window. See Tools/mutation_window.sh.
    #
    # Deliberately inside the `if`: --summarize reads an existing log and
    # launches nothing, so it is safe in the window, and refusing it would make
    # an open window harder to diagnose from outside.
    # shellcheck source=/dev/null
    source "${PROJECT_DIR}/Tools/mutation_window.sh"
    if _MARKER="$(marker_path_for "${PROJECT_DIR}")"; then
        if [[ "$(check_mutation_window "${_MARKER}" "${FLOWVIZ_MUTATION_TOKEN:-}")" == "blocked" ]]; then
            refuse_mutation_window "${_MARKER}" "this test run"
            exit 76
        fi
    fi

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
RAN=$(grep -cE "Test Completed\. Result=" "${LOG}")
PASSED=$(grep -cE "Test Completed\. Result=\{Success\}" "${LOG}")

# --- what the engine FOUND, read before anything decides what happened -------
#
# THIS BLOCK USED TO SIT BELOW THE RAN==0 EXIT, and that placement was itself a
# defect. See the crash branch immediately after it: a session that dies before
# its first test completes has RAN==0, so everything below the early exit is
# unreachable for exactly the case that most needs it.
ENUMERATED_NAMES="$(awk '
    /automation tests based on/ { collecting = 1; next }
    collecting && /Display: \t/ {
        sub(/.*Display: \t/, "")
        sub(/[[:space:]]+$/, "")
        print
        next
    }
    collecting { collecting = 0 }
' "${LOG}")"

ENUMERATED_COUNT=0
[[ -n "${ENUMERATED_NAMES}" ]] && \
    ENUMERATED_COUNT="$(printf '%s\n' "${ENUMERATED_NAMES}" | sort -u | wc -l | tr -d ' ')"

# --- nothing completed: a crash, a dead editor, or a filter that matched none -
#
# These three used to print the same sentence and exit 4:
#
#     FAIL: no tests matched '<filter>'. Check the test path.
#
# Observed 2026-08-06: a mutation arm removed FTSTicker::RemoveTicker from the
# workspace destructor, the handler kept ticking a freed model, and the engine
# took a SIGSEGV inside FFlowVizCasePlayer::Tick between the test's Started line
# and its Completed line. This script reported an unmatched filter. The filter
# was correct, the test had started, and the stack was sitting in the same log
# it told the reader to ignore.
#
# The distinction matters because the actions differ: a wrong filter is a typo
# to fix, a crash is a defect to diagnose, and a mutation harness scoring the
# two the same way cannot tell an unrunnable machine from a mistyped path.
if [[ "${RAN}" -eq 0 ]]; then
    _CRASHED=""
    grep -q "Critical error" "${LOG}" && _CRASHED=1

    # The test that was running when the process died: the last Started with no
    # matching Completed. With RAN==0 that is simply the last Started line.
    _LAST_STARTED="$(grep -E "Test Started\. .*Path=\{" "${LOG}" \
        | sed -E 's/.*Path=\{([^}]*)\}.*/\1/' | tail -1)"

    if [[ -n "${_CRASHED}" ]]; then
        echo "CRASHED: the engine hit a critical error and no test completed." >&2
        if [[ -n "${_LAST_STARTED}" ]]; then
            echo "  died while running: ${_LAST_STARTED}" >&2
        else
            echo "  it died before any test started." >&2
        fi
        echo "  ${PASSED}/${ENUMERATED_COUNT} of the tests the engine found completed." >&2
        echo >&2
        grep -A 4 "Critical error" "${LOG}" | sed 's/^/    /' >&2
        echo >&2
        echo "  This is NOT a filter problem -- '${FILTER}' matched." >&2
        echo "Full log: ${LOG}"
        exit 5
    fi

    if [[ "${ENUMERATED_COUNT}" -gt 0 || -n "${_LAST_STARTED}" ]]; then
        # The filter matched and the engine still produced no result. Not a typo
        # and not an observed crash -- an editor that died without writing one.
        echo "NO RESULTS: '${FILTER}' matched ${ENUMERATED_COUNT} test(s) and none completed." >&2
        [[ -n "${_LAST_STARTED}" ]] && echo "  last started: ${_LAST_STARTED}" >&2
        echo "  ${PASSED}/${ENUMERATED_COUNT} completed. The session did not finish." >&2
        echo "Full log: ${LOG}"
        exit 5
    fi

    # Nothing enumerated, nothing started, no crash: the filter really is wrong.
    # An empty filter match exits 0, which would otherwise read as success.
    echo "FAIL: no tests matched '${FILTER}'. Check the test path." >&2
    echo "Full log: ${LOG}"
    exit 4
fi

# --- tests that were found but never ran -------------------------------------
#
# THE DENOMINATOR MUST NOT COME FROM THE SAME LINES AS THE NUMERATOR. This
# script used to count Completed lines for both, so a session that died partway
# through reported "51/51 passed" and exited 0 -- the count was arithmetically
# correct and completely wrong, because 55 further tests had been found and
# never started. Observed 2026-08-06: a SIGSEGV on the render thread took the
# editor down at test 51 of 106 and this script called it a clean green.
#
# The engine enumerates before it runs anything, and logs it:
#
#   LogAutomationCommandLine: Display: Found 106 automation tests based on 'FlowViz'
#   LogAutomationCommandLine: Display: 	FlowViz.CFDViz.ArrayReader.ComponentOrder
#   ...one indented line per test...
#
# Reading that list is not a reimplementation of the engine's filter semantics
# -- it IS the engine's answer, which is the only denominator that can disagree
# with the numerator. Anything derived from the test records themselves moves
# with them and can never detect a truncation.
#
# Absent on logs written before this was added, and on any log from a run that
# died before enumerating. Falling back to RAN there keeps --summarize working
# over old logs; it means those logs cannot detect truncation, which is the
# status quo and not a regression.
#
# ENUMERATED_NAMES is read above, before the RAN==0 exit, because the crash
# branch needs it too -- a session that dies before its first test completes
# never reaches this point.

MISSING_NAMES=""
if [[ -n "${ENUMERATED_NAMES}" ]]; then
    # Compare by name, not by count. A run that skipped one test and somehow
    # ran an extra would net to zero on counts alone, and the names are what
    # tell a reader whether a tail was cut or a subtree never started.
    _RAN_NAMES="$(mktemp -t flowviz_ran)"
    _ENUM_NAMES="$(mktemp -t flowviz_enum)"
    grep -E "Test Completed\. Result=" "${LOG}" \
        | sed -E 's/.*Path=\{([^}]*)\}.*/\1/' | sort -u > "${_RAN_NAMES}"
    printf '%s\n' "${ENUMERATED_NAMES}" | sort -u > "${_ENUM_NAMES}"
    MISSING_NAMES="$(comm -23 "${_ENUM_NAMES}" "${_RAN_NAMES}")"
    rm -f "${_RAN_NAMES}" "${_ENUM_NAMES}"
fi

FOUND="${RAN}"
if [[ -n "${ENUMERATED_NAMES}" ]]; then
    FOUND="$(printf '%s\n' "${ENUMERATED_NAMES}" | sort -u | wc -l | tr -d ' ')"
fi

if [[ -n "${MISSING_NAMES}" ]]; then
    _MISSING_COUNT="$(printf '%s\n' "${MISSING_NAMES}" | wc -l | tr -d ' ')"
    echo "${_MISSING_COUNT} FOUND BUT NEVER RAN -- the session did not finish:"
    printf '%s\n' "${MISSING_NAMES}" | sed 's/^/  /'
    echo
    # The overwhelmingly likely cause, and the one that reads as success.
    if grep -q "Critical error" "${LOG}"; then
        echo "  The engine hit a critical error. The stack is in the log:"
        grep -A 3 "Critical error" "${LOG}" | sed 's/^/    /' | head -6
        echo
    fi
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
#
# A TRUNCATION IS a failure, and FOUND is now the enumerated total, so a run
# that died partway through fails this comparison on the count alone. That is
# deliberate: the whole defect was that the two numbers could not disagree.
[[ "${PASSED}" -eq "${FOUND}" && "${ENGINE_EXIT}" -eq 0 ]] || exit 1
