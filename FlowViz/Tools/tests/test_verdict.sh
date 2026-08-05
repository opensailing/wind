#!/bin/bash
# Known-answer tests for mutate.sh's verdict function.
#
# The verdict is the one part of the harness that decides truth, and it has
# been wrong twice already -- once by grepping for a string the engine never
# prints, once by reading an engine crash as a kill. Both defects looked like
# results. This file pins the distinction with cases whose answers are known
# independently of the implementation.
#
# The cases are real captured runner output, not invented text, because the
# whole class of bug here is a mismatch between what the engine actually prints
# and what the harness expects it to print.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "${SCRIPT_DIR}/../verdict.sh"

PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [[ "${expected}" == "${actual}" ]]; then
        echo "  ok    ${name}"
        PASS=$((PASS+1))
    else
        echo "  FAIL  ${name}: expected '${expected}', got '${actual}'"
        FAIL=$((FAIL+1))
    fi
}

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# --- the mutant's test failed: a genuine kill -------------------------------
cat > "${TMP}/killed.log" <<'LOG'
Running automation tests matching 'FlowViz.CFDViz.ByteCursor'...

  Fail  FlowViz.CFDViz.ByteCursor

Failure details:
  LogAutomationController: Error: Expected 'CanRead rejects a wrapping count' to be true.

0/1 passed. Full log: /tmp/flowviz_tests.log
LOG
check "a failing test is a kill" "killed" "$(classify_test_run 1 "${TMP}/killed.log")"

# --- the mutant's test passed: a survivor -----------------------------------
cat > "${TMP}/survived.log" <<'LOG'
Running automation tests matching 'FlowViz.CFDViz.ByteCursor'...

  Success  FlowViz.CFDViz.ByteCursor

1/1 passed. Full log: /tmp/flowviz_tests.log
LOG
check "a passing test is a survivor" "SURVIVED" "$(classify_test_run 0 "${TMP}/survived.log")"

# --- THE REGRESSION: the engine crashed before any test ran -----------------
#
# Captured from a real run whose editor died in ShaderCore.cpp with
# "Assertion failed: bDirectoryExists" -- an engine startup failure that has
# nothing to do with the mutant. run_tests.sh correctly exits non-zero. The
# harness read that as "the suite caught the mutant" and recorded a kill for a
# check that was never exercised, which is the exact false-verified direction
# worktree isolation exists to prevent.
cat > "${TMP}/crashed.log" <<'LOG'
Running automation tests matching 'FlowViz.CFDViz.ByteCursor'...

FAIL: no tests matched 'FlowViz.CFDViz.ByteCursor'. Check the test path.
Full log: /tmp/flowviz_tests.log
LOG
check "an engine crash is NOT a kill" "UNSCORED" "$(classify_test_run 4 "${TMP}/crashed.log")"

# --- the editor never started at all ----------------------------------------
cat > "${TMP}/nostart.log" <<'LOG'
Running automation tests matching 'FlowViz.CFDViz.ByteCursor'...
error: no log produced at /tmp/flowviz_tests.log; the editor failed to start
LOG
check "a dead editor is NOT a kill" "UNSCORED" "$(classify_test_run 3 "${TMP}/nostart.log")"

# --- a partial run: some tests ran, one failed ------------------------------
#
# Distinct from the crash case: tests DID execute and one reported Fail, so the
# non-zero exit is attributable to the mutant even though the run was noisy.
cat > "${TMP}/partial.log" <<'LOG'
Running automation tests matching 'FlowViz.CFDViz.MeshReader'...

  Fail  FlowViz.CFDViz.MeshReader.Rejection
  Success  FlowViz.CFDViz.MeshReader.Layout

Failure details:
  LogAutomationController: Error: Expected 'headerBytes = 96 still loads' to be true.

1/2 passed. Full log: /tmp/flowviz_tests.log
LOG
check "a real failure among passes is a kill" "killed" "$(classify_test_run 1 "${TMP}/partial.log")"

# --- a run that reports zero tests found but exits 0 ------------------------
#
# Cannot happen through run_tests.sh today (it exits 4), but the classifier
# must not treat "no evidence" as "survived" if that ever changes: a filter
# typo would otherwise silently clear every mutant.
cat > "${TMP}/empty.log" <<'LOG'
Running automation tests matching 'FlowViz.CFDViz.Nonexistent'...
LOG
check "no tests found is never a survivor" "UNSCORED" "$(classify_test_run 0 "${TMP}/empty.log")"

echo
echo "verdict tests: ${PASS} passed, ${FAIL} failed"
[[ "${FAIL}" -eq 0 ]]
