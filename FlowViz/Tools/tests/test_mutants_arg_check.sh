#!/bin/bash
# mutate.sh must reject an unreadable mutants file BEFORE it builds anything.
#
# --- what happened ------------------------------------------------------------
#
# Observed live 2026-08-06. A clip-seam campaign was launched with the mutants
# file given relative to FlowViz/ rather than to the repo root:
#
#     ./Tools/mutate.sh <src> FlowViz Tools/mutants/clip-seam-dispatcher.txt
#
# mutate.sh does `cd "${REPO_ROOT}"` (mutate.sh:192), so that path needed the
# same FlowViz/ prefix the source argument had. It ran the full baseline build
# AND the full pristine suite -- about ten minutes -- and only then reached the
# parse and aborted:
#
#     FileNotFoundError: ... 'Tools/mutants/clip-seam-dispatcher.txt'
#     ABORT: could not parse ...; no verdict would mean anything.
#
# --- why this is worth a test rather than a fixed typo ------------------------
#
# The abort is CORRECT and this defect cannot forge a verdict: it fails closed
# and loudly, which is why it is filed apart from the campaign it interrupted.
# What it does is make one class of typo cost 200x more on argument 3 than on
# argument 1, and it does so in the one script whose whole job is to be run
# repeatedly with slightly different paths.
#
# The asymmetry is the finding. SRC is validated one line after the cd:
#
#     [[ -f "${SRC}" ]] || { echo "error: no such source file: ${SRC}" ...
#
# so mutate.sh ALREADY knows that a relative argument resolved against
# REPO_ROOT is a thing users get wrong. MUTANTS gets the identical treatment
# nowhere, and is first touched at the parse, past the build and the suite.
#
# There is a second cost that is not just time. Between the cd and the parse,
# mutate.sh writes the mutation marker (line 244) and takes the global build
# lock. A doomed invocation therefore blocks every other agent's build for the
# duration, and leaves a window open, to reach an error it could have printed
# instantly.
#
# --- what this test asserts, and the trap it avoids ---------------------------
#
# Asserting "exit 2" alone would PASS AGAINST THE DEFECT -- today's late parse
# exits 2 as well. The assertion that separates fixed from broken is that no
# build was ATTEMPTED, so the test stubs build_lock.sh with a recorder and
# checks the recorder stayed empty.
#
# A recorder that stays empty is exactly what a stub that never runs looks
# like, so an empty recorder is only evidence if it could have been non-empty.
# The CONTROL below drives the same fixture with a mutants file that DOES
# exist and requires the recorder to fire. Without it this file would pass with
# the stub misnamed, the fixture misbuilt, or mutate.sh dying at the cd.

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

echo "mutate.sh mutants-file argument check:"

# --- fixture ------------------------------------------------------------------
#
# A throwaway git repo with the layout mutate.sh derives its paths from:
# PROJECT_DIR is the dir above Tools/, REPO_ROOT the dir above that. The real
# tree is never touched -- a campaign may be running in it, and this test
# deliberately provokes the marker-writing path.
FIXTURE="$(mktemp -d -t mutants_arg_check)"
cleanup() { rm -rf "${FIXTURE}"; }
trap cleanup EXIT

mkdir -p "${FIXTURE}/Proj/Tools/mutants"
mkdir -p "${FIXTURE}/Proj/Src"

# git identity is set locally: a machine with no global user.email cannot
# commit, and the fixture would fail for a reason having nothing to do with
# the behaviour under test.
git -C "${FIXTURE}" init --quiet 2>/dev/null
git -C "${FIXTURE}" config user.email "test@example.invalid"
git -C "${FIXTURE}" config user.name "mutants arg check"

printf 'int Target() { return 1; }\n' > "${FIXTURE}/Proj/Src/Target.cpp"

# The real scripts mutate.sh sources by absolute path. Copied rather than
# stubbed: their behaviour is not what is under test, and a stub of
# verdict.sh could silently diverge from the classifier being shipped.
cp "${TOOLS}/verdict.sh" "${FIXTURE}/Proj/Tools/verdict.sh"
cp "${TOOLS}/parse_mutants.py" "${FIXTURE}/Proj/Tools/parse_mutants.py"
cp "${TOOLS}/mutation_window.sh" "${FIXTURE}/Proj/Tools/mutation_window.sh" 2>/dev/null || true

# THE RECORDER. Everything expensive in mutate.sh -- the baseline build and the
# baseline suite -- goes through build_lock.sh. Appending one line per
# invocation turns "did it build?" into a file that either has content or does
# not.
#
# It must also SUCCEED, not merely record. A stub that just exits non-zero
# leaves an empty BUILD_LOG, which classify_build reads as `infrastructure`
# and mutate.sh then retries 60 times at 20s intervals -- the fixture hangs
# instead of reaching the parse. So the stub writes the minimum each
# classifier demands to call the run clean:
#
#   classify_build       -- "Result: Succeeded" plus a NON-ZERO action count,
#                           because "run 0 action(s)" classifies as `noop` and
#                           loops just as long.
#   classify_baseline_run-- an "<n>/<m> passed." summary line and exit 0. A
#                           missing summary is UNSCORED, and a run whose tests
#                           all skipped is UNSCORED too, so the counts here are
#                           deliberately of real, non-skipped tests.
#
# Writing to whichever log the caller passed (mutate.sh redirects our stdout to
# BUILD_LOG for builds and to the baseline log for the suite) means one stub
# serves both without having to know which phase it is in.
cat > "${FIXTURE}/Proj/Tools/build_lock.sh" <<'STUB'
#!/bin/bash
echo "invoked: $*" >> "${RECORDER}"
# Satisfies classify_build (succeeded, non-zero actions) and
# classify_baseline_run (a real summary line) at once.
echo "Using Unreal Build Accelerator local executor to run 3 action(s)"
echo "Result: Succeeded"
echo "2/2 passed."
exit 0
STUB
chmod +x "${FIXTURE}/Proj/Tools/build_lock.sh"

cp "${MUTATE}" "${FIXTURE}/Proj/Tools/mutate.sh"
chmod +x "${FIXTURE}/Proj/Tools/mutate.sh"

# A real, parseable mutants file for the control arm.
cat > "${FIXTURE}/Proj/Tools/mutants/real.txt" <<'MUTANTS'
--- a control arm that exists
- int Target() { return 1; }
+ int Target() { return 2; }
MUTANTS

git -C "${FIXTURE}" add -A >/dev/null 2>&1
git -C "${FIXTURE}" commit -m "fixture" --quiet --no-verify 2>/dev/null

# Paths are given relative to REPO_ROOT (the fixture root), matching how the
# real campaign is invoked from the repo root.
SRC_ARG="Proj/Src/Target.cpp"

run_fixture() {
    local mutants_arg="$1" recorder="$2"
    : > "${recorder}"
    (
        cd "${FIXTURE}" || exit 99
        RECORDER="${recorder}" \
        BUILD_LOG="${FIXTURE}/build.log" \
        TEST_LOG="${FIXTURE}/test.log" \
        BASELINE_TEST_LOG="${FIXTURE}/baseline.log" \
        timeout 120 ./Proj/Tools/mutate.sh "${SRC_ARG}" SomeFilter "${mutants_arg}" \
            > "${FIXTURE}/out.log" 2>&1
        echo $? > "${FIXTURE}/exit"
    )
    cat "${FIXTURE}/exit" 2>/dev/null || echo "no-exit"
}

# --- THE CONTROL: the recorder can fire ---------------------------------------
#
# Runs first. If a real mutants file does NOT reach build_lock.sh, then the
# "no build attempted" assertion below is vacuous and this whole file is
# measuring nothing.
CONTROL_REC="${FIXTURE}/control_recorder"
CONTROL_EXIT="$(run_fixture "Proj/Tools/mutants/real.txt" "${CONTROL_REC}")"

check "CONTROL: a readable mutants file reaches the build" "yes" \
    "$([[ -s "${CONTROL_REC}" ]] && echo yes || echo no)"

if [[ ! -s "${CONTROL_REC}" ]]; then
    echo
    echo "  The control did not build, so the real assertion cannot distinguish"
    echo "  'rejected early' from 'never ran'. Fixture output:"
    sed 's/^/    /' "${FIXTURE}/out.log" 2>/dev/null | head -25
    echo "  (control exit ${CONTROL_EXIT})"
    echo
    echo "  ${PASS} passed, $((FAIL+1)) failed."
    exit 1
fi

# --- THE ASSERTION: a missing mutants file is rejected before any of that -----
MISSING_REC="${FIXTURE}/missing_recorder"
MISSING_EXIT="$(run_fixture "Tools/mutants/does-not-exist.txt" "${MISSING_REC}")"

check "a missing mutants file exits 2" "2" "${MISSING_EXIT}"

check "a missing mutants file attempts NO build" "yes" \
    "$([[ -s "${MISSING_REC}" ]] && echo no || echo yes)"

# The message has to name the path AND where it was resolved from. "no such
# file" alone sends the reader to check whether the file exists -- it does,
# just not relative to the root mutate.sh cd'd into. That is the entire trap.
check "the error names the offending path" "yes" \
    "$(grep -q 'does-not-exist.txt' "${FIXTURE}/out.log" && echo yes || echo no)"

check "the error says where relative paths resolve from" "yes" \
    "$(grep -qi 'resolve' "${FIXTURE}/out.log" && echo yes || echo no)"

# --- the window must not be left open on the early exit ----------------------
#
# The check lands before the marker is written, so a rejected invocation should
# leave no window. If the check were ever moved after line 244, this fails --
# and an abandoned marker blocks every subsequent build in that checkout, which
# presents as the build being broken rather than as a guard doing its job.
FIXTURE_GITDIR="$(git -C "${FIXTURE}" rev-parse --absolute-git-dir 2>/dev/null)"
check "a rejected invocation leaves no mutation window open" "yes" \
    "$([[ -e "${FIXTURE_GITDIR}/FLOWVIZ_MUTATION_ACTIVE" ]] && echo no || echo yes)"

# --- the same guard, on the argument that already had one --------------------
#
# Present so the pair cannot drift: if someone tightens MUTANTS and loosens SRC,
# or the cd moves above both, this catches it.
SRC_REC="${FIXTURE}/src_recorder"
SRC_MISSING_EXIT="$( : > "${SRC_REC}"; (
    cd "${FIXTURE}" || exit 99
    RECORDER="${SRC_REC}" BUILD_LOG="${FIXTURE}/b2.log" TEST_LOG="${FIXTURE}/t2.log" \
    BASELINE_TEST_LOG="${FIXTURE}/bl2.log" \
    timeout 120 ./Proj/Tools/mutate.sh "Proj/Src/NoSuchFile.cpp" SomeFilter \
        "Proj/Tools/mutants/real.txt" > "${FIXTURE}/out2.log" 2>&1
    echo $?
) )"

check "a missing SOURCE file still exits 2" "2" "${SRC_MISSING_EXIT}"
check "a missing SOURCE file attempts NO build" "yes" \
    "$([[ -s "${SRC_REC}" ]] && echo no || echo yes)"

echo
echo "  ${PASS} passed, ${FAIL} failed."
[[ "${FAIL}" -eq 0 ]]
