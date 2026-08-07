#!/bin/bash
# Every function mutate.sh CALLS must resolve when mutate.sh runs it.
#
# --- what happened ------------------------------------------------------------
#
# Observed live 2026-08-06, printed after a clip-seam campaign had scored all
# five of its arms:
#
#     .../flowviz_mutate_self.bX0OZIG8Qd/mutate.sh: line 623:
#     check_harness_drift: command not found
#
# check_harness_drift is defined in Tools/mutation_window.sh:85. mutate.sh
# sources Tools/verdict.sh (line 190) and nothing else. So the call at line 623
# had never once resolved -- the entire harness-drift provenance block, which
# exists to warn that verdicts came from a pre-edit copy of the harness, has
# never run since the day it was written.
#
# --- what I expected to be broken, and was not --------------------------------
#
# I filed this expecting the failed lookup to corrupt the exit code too. The
# last line of the script is
#
#     [[ "${SURVIVED}" -eq 0 && "${UNSCORED}" -eq 0 ]] || exit 1
#
# and the failed substitution lands immediately before it under `set -uo
# pipefail` with no -e, so the theory was that $? leaks through. The campaign
# that exposed the defect scored killed 4 / SURVIVED 1, wanted exit 1, and got
# exit 1 -- right answer, possibly wrong reason.
#
# THE ASSERTION BELOW DISPROVED THAT. "an all-killed campaign exits 0" passes
# against the UNFIXED script: the `[[ ]]` is its own command and its status is
# the script's, so the dead substitution before it changes nothing. The exit
# path was never in danger.
#
# Both directions are pinned here anyway. Not because they are failing, but
# because the reasoning that says they cannot fail is exactly the reasoning
# that has been wrong before in this file's subject matter, and a campaign's
# exit code is what a wrapper reads instead of the summary.
#
# So the defect is narrower than filed and entirely real: the drift-provenance
# block -- #45's whole purpose -- has never executed.
#
# --- why 26 green checks did not catch it -------------------------------------
#
# Tools/tests/test_mutate_selfcopy.sh covers check_harness_drift thoroughly and
# passes. It sources mutation_window.sh ITSELF at line 307, so the function is
# in scope for the test and absent in production. Its check on the real script
# is:
#
#     grep -q 'check_harness_drift' "${MUTATE}"
#
# which asserts the CALL is written, not that it resolves. And its end-to-end
# arm builds a throwaway campaign script that sources mutation_window.sh
# explicitly -- proving the mechanism works in a harness that does the one
# thing mutate.sh forgot to do. Repo memory calls this shape
# mocking-a-seam-hides-that-nothing-builds-it: every test installed its own
# dependency, so none could see that production installed none.
#
# So this file asserts the property that grep cannot: mutate.sh RUNS to
# completion with no unresolved command, and its exit code is decided by the
# verdict counts in BOTH directions.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$(cd "${SCRIPT_DIR}/.." && pwd)"
MUTATE="${TOOLS}/mutate.sh"

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

echo "mutate.sh resolves every function it calls:"

# --- fixture ------------------------------------------------------------------
#
# Same shape as test_mutants_arg_check.sh: a throwaway git repo laid out the way
# mutate.sh derives its paths (PROJECT_DIR above Tools/, REPO_ROOT above that).
# The real tree is never touched -- a campaign may be live in it, and this test
# drives the marker-writing path on purpose.
FIXTURE="$(mktemp -d -t mutate_resolves)"
cleanup() { rm -rf "${FIXTURE}"; }
trap cleanup EXIT

mkdir -p "${FIXTURE}/Proj/Tools/mutants"
mkdir -p "${FIXTURE}/Proj/Src"

git -C "${FIXTURE}" init --quiet 2>/dev/null
git -C "${FIXTURE}" config user.email "test@example.invalid"
git -C "${FIXTURE}" config user.name "mutate resolves calls"

printf 'int Target() { return 1; }\n' > "${FIXTURE}/Proj/Src/Target.cpp"

# The real dependencies, copied not stubbed. A stub of mutation_window.sh would
# define check_harness_drift and mask the exact defect under test.
cp "${TOOLS}/verdict.sh"         "${FIXTURE}/Proj/Tools/verdict.sh"
cp "${TOOLS}/parse_mutants.py"   "${FIXTURE}/Proj/Tools/parse_mutants.py"
cp "${TOOLS}/mutation_window.sh" "${FIXTURE}/Proj/Tools/mutation_window.sh"

# The build/test stub. Same reasoning as test_mutants_arg_check.sh: it must
# SUCCEED as well as record, or classify_build reads an empty log as
# `infrastructure` and mutate.sh retries 60 times at 20s intervals.
#
# TEST_RESULT is what makes both exit-code directions reachable. It is written
# by each arm below to decide whether the arm's suite passes (mutant SURVIVED)
# or fails (mutant killed).
cat > "${FIXTURE}/Proj/Tools/build_lock.sh" <<'STUB'
#!/bin/bash
echo "Using Unreal Build Accelerator local executor to run 3 action(s)"
echo "Result: Succeeded"

# PHASE BY EVIDENCE, NOT BY TIMING. The baseline runs against pristine
# source; an arm runs with the mutant APPLIED (return 2). Reading the target
# file is deterministic where the previous watcher -- polling the log every
# 0.2s and flipping a flag -- raced the instant stub: the arm's suite could
# consume the flag before the flip landed, scoring SURVIVED in a run that
# asked for a kill. The sweep caught it as a once-in-a-few-runs flake.
PHASE="baseline"
if grep -q 'return 2' "${TARGET_FILE}" 2>/dev/null; then
    PHASE="arm"
fi

if [[ "${PHASE}" == "arm" ]]; then
    # The drift edit, mid-campaign by construction: the first ARM invocation
    # is after the baseline and before the verdict. Once, marker-guarded.
    if [[ -n "${DRIFT_EDIT}" && ! -f "${DRIFT_EDIT}.done" ]]; then
        echo "# edited mid-campaign by test_mutate_resolves_calls.sh" >> "${DRIFT_EDIT}"
        touch "${DRIFT_EDIT}.done"
    fi
    if [[ "$(cat "${TEST_RESULT}" 2>/dev/null)" == "fail" ]]; then
        echo "1/2 passed."
        exit 1
    fi
fi
echo "2/2 passed."
exit 0
STUB
chmod +x "${FIXTURE}/Proj/Tools/build_lock.sh"

cp "${MUTATE}" "${FIXTURE}/Proj/Tools/mutate.sh"
chmod +x "${FIXTURE}/Proj/Tools/mutate.sh"

cat > "${FIXTURE}/Proj/Tools/mutants/one.txt" <<'MUTANTS'
the only arm
--
int Target() { return 1; }
--
int Target() { return 2; }
%%
MUTANTS

git -C "${FIXTURE}" add -A >/dev/null 2>&1
git -C "${FIXTURE}" commit -m "fixture" --quiet --no-verify 2>/dev/null

# BASELINE MUST PASS, ARM MAY NOT.
#
# mutate.sh runs the suite pristine first and refuses to score anything if that
# baseline is red (#33). The stub therefore has to answer "pass" during the
# baseline and only then switch. The arm's verdict is chosen by flipping
# TEST_RESULT between the two runs, which the fixture does by having the stub
# read a file mutate.sh knows nothing about.
#
# The flip is driven from a background watcher rather than inline, because
# mutate.sh does not offer a hook between the baseline and the first arm.
#
# The third argument edits Tools/mutate.sh ON DISK mid-campaign. That is the
# only way to reach the `drifted` branch, and it is safe for exactly the reason
# the branch exists to announce: the running process is executing a private
# copy, so the edit cannot corrupt it (#45).
run_campaign() {
    local arm_result="$1" outfile="$2" drift="${3:-no}"
    local flag="${FIXTURE}/test_result"

    # The stub decides phase from the TARGET FILE (mutant applied = arm), so
    # this flag only carries the ARM's verdict and can be set up front -- no
    # watcher, no race. See the stub's own comment for the flake this
    # replaced.
    echo "${arm_result}" > "${flag}"

    local drift_target=""
    if [[ "${drift}" == "drift" ]]; then
        drift_target="${FIXTURE}/Proj/Tools/mutate.sh"
        rm -f "${drift_target}.done"
    fi

    (
        cd "${FIXTURE}" || exit 99
        TEST_RESULT="${flag}" \
        TARGET_FILE="${FIXTURE}/Proj/Src/Target.cpp" \
        DRIFT_EDIT="${drift_target}" \
        BUILD_LOG="${FIXTURE}/build.log" \
        TEST_LOG="${FIXTURE}/test.log" \
        BASELINE_TEST_LOG="${FIXTURE}/baseline.log" \
        timeout 180 ./Proj/Tools/mutate.sh "Proj/Src/Target.cpp" SomeFilter \
            "Proj/Tools/mutants/one.txt" > "${outfile}" 2>&1
        echo $? > "${FIXTURE}/exit"
    )

    cat "${FIXTURE}/exit" 2>/dev/null || echo "no-exit"
}

# --- ARM A: the mutant is killed, so the campaign must exit 0 -----------------
#
# The direction the live run never exercised -- it had a survivor and wanted
# exit 1. This arm is what disproved the exit-code half of the report: it
# passes against the unfixed script. Kept as a regression pin, not as evidence
# of the defect.
KILLED_OUT="${FIXTURE}/killed.log"
KILLED_EXIT="$(run_campaign "fail" "${KILLED_OUT}")"

# CONTROL FIRST: if the campaign never reached its summary, every assertion
# below is about a run that did not happen.
check "CONTROL: the campaign reaches its verdict summary" "yes" \
    "$(grep -q 'killed 1' "${KILLED_OUT}" && echo yes || echo no)"

if ! grep -q 'killed 1' "${KILLED_OUT}" 2>/dev/null; then
    echo
    echo "  The fixture campaign did not score its arm, so nothing below is"
    echo "  measuring the exit path. Output:"
    sed 's/^/    /' "${KILLED_OUT}" 2>/dev/null | head -30
    echo "  (exit ${KILLED_EXIT})"
    echo
    echo "  ${PASS} passed, $((FAIL+1)) failed."
    exit 1
fi

# THE ASSERTION THE DEFECT FAILS. Any unresolved function anywhere in the run
# prints "command not found" -- naming no function in particular, so this keeps
# working when the next one is added.
check "no command in mutate.sh is unresolved at runtime" "yes" \
    "$(grep -q 'command not found' "${KILLED_OUT}" && echo no || echo yes)"

check "an all-killed campaign exits 0" "0" "${KILLED_EXIT}"

# SILENCE IS CORRECT HERE, AND THAT IS THE PROBLEM.
#
# I first asserted a provenance line on this arm and it failed against the FIXED
# script. The reasoning in the comment I wrote was wrong twice over: the marker
# is NOT gone by now (restore() clears it on EXIT, which is after this call),
# and the sha is NOT different (HARNESS_SHA fingerprints the private copy, which
# is byte-identical to the file it was copied from). So check_harness_drift
# returns `same` -- and the case has no `same` branch, by design: an unchanged
# harness has nothing to disclose.
#
# Repo memory run-a-new-assertion-against-pristine-code-first, hit for the
# second time in this one report. Both halves of what I filed were wrong; the
# defect itself was real.
#
# The consequence for THIS file: a `same` run cannot distinguish "the block ran
# and stayed quiet" from "the block died on a failed lookup". Both print
# nothing. Asserting on silence here would be asserting a tautology, so arm C
# below drives the one branch that has to speak.
check "an unchanged harness stays silent (no spurious drift notice)" "yes" \
    "$(grep -q 'NOTE: Tools/mutate.sh was EDITED' "${KILLED_OUT}" && echo no || echo yes)"

# --- ARM B: the mutant survives, so the campaign must exit 1 ------------------
#
# Present so the pair cannot drift: a "fix" that hardcodes exit 0 would pass
# arm A alone, and an exit code that cannot distinguish a survivor from a clean
# sweep is worse than none -- it reads as a pass.
SURVIVED_OUT="${FIXTURE}/survived.log"
SURVIVED_EXIT="$(run_campaign "pass" "${SURVIVED_OUT}")"

check "CONTROL: the survivor campaign also reaches its summary" "yes" \
    "$(grep -q 'SURVIVED 1' "${SURVIVED_OUT}" && echo yes || echo no)"

check "a campaign with a survivor exits 1" "1" "${SURVIVED_EXIT}"

check "no unresolved command in the survivor path either" "yes" \
    "$(grep -q 'command not found' "${SURVIVED_OUT}" && echo no || echo yes)"

# --- ARM C: the drift branch, the only one that has to speak ------------------
#
# THIS is the assertion the defect fails. Arms A and B prove no lookup dies
# mid-run; neither can prove the provenance block itself executes, because the
# `same` verdict they produce is silent and so is a block that never ran.
#
# Here the fixture appends a line to Tools/mutate.sh after the baseline goes
# green. The running process is on a private copy, so this changes nothing about
# the campaign -- which is precisely the situation #45 built the notice to
# disclose. check_harness_drift must now answer `drifted` and the NOTE must
# appear. With the source line removed it does not, and this check goes red.
#
# Verified against the unfixed script before the fix was kept: it fails there.
DRIFT_OUT="${FIXTURE}/drift.log"
DRIFT_EXIT="$(run_campaign "fail" "${DRIFT_OUT}" "drift")"

check "CONTROL: the drift campaign also reaches its summary" "yes" \
    "$(grep -q 'killed 1' "${DRIFT_OUT}" && echo yes || echo no)"

check "a harness edited mid-run is disclosed" "yes" \
    "$(grep -q 'NOTE: Tools/mutate.sh was EDITED' "${DRIFT_OUT}" && echo yes || echo no)"

# The notice is provenance, not a verdict: an all-killed campaign that happened
# to be edited underneath still exits 0. If this ever flips, the notice has
# started failing runs that were coherent.
check "the drift notice does not change the exit status" "0" "${DRIFT_EXIT}"

# --- the source line itself ---------------------------------------------------
#
# A static backstop for the specific dependency, so a refactor that drops the
# source line fails HERE with a clear name rather than only via the runtime
# assertions above. Deliberately NOT the whole test: grep is what missed this
# the first time.
check "mutate.sh sources mutation_window.sh" "yes" \
    "$(grep -q 'source .*Tools/mutation_window.sh' "${MUTATE}" && echo yes || echo no)"

echo
echo "  ${PASS} passed, ${FAIL} failed."
[[ "${FAIL}" -eq 0 ]]
