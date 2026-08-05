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

# --- build classification ---------------------------------------------------
#
# A build that did not produce a binary has two very different causes, and the
# harness must not conflate them. If the MUTANT does not compile, that is
# INVALID -- a fact about the mutant. If the BUILD TOOL fell over, that is a
# fact about this machine, and scoring it at all would attribute a local race
# to the code under test.
#
# mutate.sh already waits out one such case (ConflictingInstance, the UBT
# lock). The cases below are the ones it did not recognise.

# THE REGRESSION: UnrealBuildTool aborted on a user-global state file.
#
# Real captured output. Trace.uba lives in ~/Library/Application Support and is
# shared by every build on the machine regardless of worktree, so a concurrent
# build deleting it aborts an unrelated one. Nothing about the mutant is
# involved, and the mutated file is never named -- yet a harness that only asks
# "did the build succeed?" would fall through to a verdict.
cat > "${TMP}/uba.log" <<'LOG'
Building FlowVizEditor...
Unhandled exception. System.IO.FileNotFoundException: Could not find file '/Users/x/Library/Application Support/Epic/UnrealBuildTool/Trace.uba'.
   at EpicGames.Core.Log.BackupLogFile(FileReference outputFile)
   at UnrealBuildTool.UnrealBuildTool.Main(String[] ArgumentsArray)
Build.sh: line 35: 64851 Abort trap: 6           dotnet UnrealBuildTool.dll "$@"
LOG
check "a UBT crash is infrastructure" "infrastructure" \
    "$(classify_build "${TMP}/uba.log")"

# The UBT lock, which mutate.sh already retries. Classified the same way, so
# that retry logic has one definition to consult rather than its own grep.
cat > "${TMP}/lock.log" <<'LOG'
Building FlowVizEditor...
ERROR: Another instance of UnrealBuildTool is already running (ConflictingInstance). Waiting...
LOG
check "the UBT lock is infrastructure" "infrastructure" \
    "$(classify_build "${TMP}/lock.log")"

# A real compile error in the mutated file. This is the one case that is a
# fact about the mutant, and it must NOT be swallowed as infrastructure --
# doing so would silently drop every uncompilable mutant from the scoring.
cat > "${TMP}/badmutant.log" <<'LOG'
Building FlowVizEditor...
/repo/Private/CFDViz/CFDVizArrayReader.cpp:118:9: error: use of undeclared identifier 'ValueCount'
1 error generated.
LOG
check "a compile error in the mutant is not infrastructure" "failed" \
    "$(classify_build "${TMP}/badmutant.log")"

# A successful build. Included because a classifier that answered
# "infrastructure" for everything would pass every case above.
cat > "${TMP}/ok.log" <<'LOG'
Building FlowVizEditor...
Total execution time: 41.20 seconds
Result: Succeeded
LOG
check "a green build succeeds" "succeeded" "$(classify_build "${TMP}/ok.log")"

# An abort trap AFTER a successful result. The success line is authoritative
# (the binary exists); a crash in UBT's own teardown must not discard it.
cat > "${TMP}/okthencrash.log" <<'LOG'
Building FlowVizEditor...
Result: Succeeded
Abort trap: 6
LOG
check "a teardown crash after success is still a success" "succeeded" \
    "$(classify_build "${TMP}/okthencrash.log")"

# A build that COMPILED NOTHING but still reported success. UnrealBuildTool
# rebuilds on modification time, so a mutant installed with `cp` can leave the
# source no newer than its object file; UBT then skips the compile and the
# tests measure the PREVIOUS binary. Scoring that is a false SURVIVED, which
# asks for a test that already exists. Seen for real: `<=` -> `<` in
# FitsInBudget scored SURVIVED against 0 actions, then killed with 6 failed
# assertions once genuinely recompiled.
cat > "${TMP}/noop.log" <<'LOG'
Building FlowVizEditor...
Target is up to date
Using Unreal Build Accelerator local executor to run 0 action(s)
Result: Succeeded
LOG
check "a build that compiled nothing is not a scoreable success" "noop" \
    "$(classify_build "${TMP}/noop.log")"

# The control for the case above: a build that DID compile is a real success.
# Without this, a classifier that answered "noop" for every green build would
# pass the previous check.
cat > "${TMP}/didwork.log" <<'LOG'
Building FlowVizEditor...
Using Unreal Build Accelerator local executor to run 2 action(s)
Result: Succeeded
LOG
check "a build that compiled something succeeds" "succeeded" \
    "$(classify_build "${TMP}/didwork.log")"

echo
echo "verdict tests: ${PASS} passed, ${FAIL} failed"
[[ "${FAIL}" -eq 0 ]]
