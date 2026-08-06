#!/bin/bash
# Does a finished campaign leave enough behind to say WHICH ASSERTION killed
# which arm?
#
# It did not. mutate.sh set
#
#     BUILD_LOG="${BUILD_LOG:-/tmp/mutate_build.log}"
#     TEST_LOG="${TEST_LOG:-/tmp/mutate_test.log}"
#
# -- one path each -- and the per-arm loop redirected every arm's build and
# every arm's suite run into them. Each arm clobbered the last. When a ten-arm
# campaign finished, exactly one log survived: the tenth.
#
# WHY THAT MATTERS MORE THAN IT SOUNDS. The verdict itself is unaffected;
# classify_test_run reads the log while it is still the current arm's. What is
# lost is the EVIDENCE, and the evidence is what a reader checks the verdict
# against afterwards. Tools/mutants/session-workspace.README says so in its own
# words, and asked for this fix:
#
#     WHICH ASSERTIONS KILLED WHICH ARM. DERIVED, NOT READ OFF A LOG - and that
#     is a defect in how this campaign was run rather than a property of the
#     results. ... A future campaign should point TEST_LOG at a per-arm path.
#
# Derivation is a claim about what a mutant SHOULD break, checked against
# nothing. Repo memory a-compound-mutant-scores-like-a-narrow-one is precisely
# the case it cannot see: an arm named for three casualties that actually died
# on two, with the third riding along untested. Derivation reports all three and
# reads as thorough.
#
# The same argument applies to BUILD_LOG. An INVALID arm's verdict rests on
# compiler errors naming the mutated file; by the time the campaign prints its
# summary those errors have been overwritten by a later arm's build, so the one
# line of evidence for the one verdict that blames the ARM rather than the code
# is gone.
#
# WHY THIS DRIVES THE REAL mutate.sh. The property is not "bash can write to two
# different filenames" -- that needs no test. It is "the shipped script routes
# each arm's output somewhere that the next arm does not overwrite", and the
# only way to check that is to run the shipped script. So the fixture stubs the
# expensive parts (build_lock.sh, which is how both the build and the suite are
# invoked) and runs the actual file. Testing the pattern on a stand-in while the
# real script keeps one path is the mocking-a-seam failure this repo has
# already paid for once.
#
# THE CONTROL IS THE WHOLE TEST, twice over:
#
#   1. A multi-arm campaign must actually REACH every arm. If the fixture dies
#      after arm one, "each arm has its own log" is satisfied by a single file
#      and the assertion measures nothing. So the arm count is asserted first,
#      from the campaign's own summary line, and this file bails loudly if it
#      is wrong rather than proceeding to a vacuous pass.
#
#   2. The logs must have CONTENT. An implementation that creates three empty
#      per-arm files passes any check that only counts them -- and an empty log
#      is exactly as useless for attribution as an overwritten one. Repo memory
#      an-empty-match-set-passes-every-check.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$(cd "${HERE}/.." && pwd)"
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

FIXTURE="$(mktemp -d)"
trap 'rm -rf "${FIXTURE}"' EXIT

# --- a miniature repo, shaped exactly like the real one ----------------------
#
# mutate.sh resolves PROJECT_DIR as the parent of Tools/ and REPO_ROOT as its
# parent, then cds to REPO_ROOT. It also writes a mutation-window marker into
# the git dir, so the fixture has to be a real repository.
mkdir -p "${FIXTURE}/Proj/Tools/mutants" "${FIXTURE}/Proj/Src"
git -C "${FIXTURE}" init --quiet 2>/dev/null
git -C "${FIXTURE}" config user.email "fixture@example.com" 2>/dev/null
git -C "${FIXTURE}" config user.name "Fixture" 2>/dev/null

cat > "${FIXTURE}/Proj/Src/Target.cpp" <<'SRC'
int First() { return 1; }
int Second() { return 2; }
int Third() { return 3; }
SRC

# The real helpers, copied rather than stubbed: their behaviour is not what is
# under test and a divergent stub would quietly change the classification.
cp "${TOOLS}/verdict.sh" "${FIXTURE}/Proj/Tools/verdict.sh"
cp "${TOOLS}/parse_mutants.py" "${FIXTURE}/Proj/Tools/parse_mutants.py"
cp "${TOOLS}/mutation_window.sh" "${FIXTURE}/Proj/Tools/mutation_window.sh" 2>/dev/null || true

# THE STUB. Every expensive thing mutate.sh does -- baseline build, baseline
# suite, per-arm build, per-arm suite -- goes through build_lock.sh, and
# mutate.sh redirects its stdout to whichever log that phase owns. So one stub
# serves all four phases without knowing which it is in, and whatever it prints
# lands in the log under test.
#
# It must SUCCEED as well as print, or the fixture never reaches the arms:
#   classify_build        needs "Result: Succeeded" AND a non-zero action count
#                         ("run 0 action(s)" classifies as noop and retries 60x).
#   classify_baseline_run needs a real "<n>/<m> passed." summary and exit 0.
#
# The marker line carries a nonce so a log's CONTENT can be traced to the run
# that wrote it -- a stale file left by an earlier fixture would otherwise be
# indistinguishable from one this run produced. Repo memory
# shared-tree-results-need-a-timestamp.
cat > "${FIXTURE}/Proj/Tools/build_lock.sh" <<'STUB'
#!/bin/bash
echo "invoked: $*" >> "${RECORDER}"
echo "FIXTURE_NONCE ${FIXTURE_NONCE}"
echo "Using Unreal Build Accelerator local executor to run 3 action(s)"
echo "Result: Succeeded"
echo "2/2 passed."
exit 0
STUB
chmod +x "${FIXTURE}/Proj/Tools/build_lock.sh"

cp "${MUTATE}" "${FIXTURE}/Proj/Tools/mutate.sh"
chmod +x "${FIXTURE}/Proj/Tools/mutate.sh"

# THREE arms, not one. One arm cannot show an overwrite: the defect is that arm
# N+1 lands on arm N's file, so the fixture needs at least two, and three makes
# an off-by-one in any per-arm naming scheme visible.
#
# Each mutates a DIFFERENT line so no arm can be skipped as stale, and the names
# are distinct words so a per-arm path derived from the name is checkable.
cat > "${FIXTURE}/Proj/Tools/mutants/three.txt" <<'MUTANTS'
alpha arm
--
int First() { return 1; }
--
int First() { return 11; }
%%
beta arm
--
int Second() { return 2; }
--
int Second() { return 22; }
%%
gamma arm
--
int Third() { return 3; }
--
int Third() { return 33; }
MUTANTS

git -C "${FIXTURE}" add -A >/dev/null 2>&1
git -C "${FIXTURE}" commit -m "fixture" --quiet --no-verify 2>/dev/null

NONCE="perarm-$$"
RECORDER="${FIXTURE}/recorder"
: > "${RECORDER}"

# LOG_DIR is where per-arm output is expected to land. Passed explicitly so the
# fixture never writes into /tmp shared with a live campaign -- repo memory
# isolation-ends-at-the-shared-output-path, which is the same defect one level
# up from the one under test.
LOG_DIR="${FIXTURE}/logs"

(
    cd "${FIXTURE}" || exit 99
    RECORDER="${RECORDER}" \
    FIXTURE_NONCE="${NONCE}" \
    MUTATE_LOG_DIR="${LOG_DIR}" \
    BUILD_LOG="${FIXTURE}/build.log" \
    TEST_LOG="${FIXTURE}/test.log" \
    BASELINE_TEST_LOG="${FIXTURE}/baseline.log" \
    timeout 180 ./Proj/Tools/mutate.sh "Proj/Src/Target.cpp" SomeFilter \
        "Proj/Tools/mutants/three.txt" > "${FIXTURE}/out.log" 2>&1
    echo $? > "${FIXTURE}/exit"
)
CAMPAIGN_EXIT="$(cat "${FIXTURE}/exit" 2>/dev/null || echo no-exit)"

# =============================================================================
# CONTROL 1 -- did all three arms actually run?
# =============================================================================
#
# Read off the campaign's own summary rather than counted from files, because
# counting files is what the real assertion does and a control must not share
# its failure mode. Every arm here mutates a compiling file that the stub
# reports as passing, so all three should score SURVIVED.
SUMMARY="$(grep -aE 'killed [0-9]+  SURVIVED [0-9]+' "${FIXTURE}/out.log" | tail -1)"
SCORED="$(awk '{s=0; for(i=1;i<=NF;i++) if ($i ~ /^[0-9]+$/) s+=$i; print s}' <<<"${SUMMARY}")"

check "CONTROL: the campaign scored all three arms" "3" "${SCORED:-0}"

if [[ "${SCORED:-0}" -ne 3 ]]; then
    echo
    echo "  The fixture did not reach three arms, so 'each arm keeps its own log'"
    echo "  would be satisfied by a single file and this test would be measuring"
    echo "  nothing. Campaign exit ${CAMPAIGN_EXIT}. Output:"
    sed 's/^/    /' "${FIXTURE}/out.log" 2>/dev/null | tail -30
    echo
    echo "  ${PASS} passed, $((FAIL+1)) failed."
    exit 1
fi

# =============================================================================
# CONTROL 2 -- the stub really did write into the logs
# =============================================================================
#
# If the stub's output never reaches any file, then "the logs have distinct
# content" is a statement about two empty files and passes for the wrong
# reason.
check "CONTROL: the stub was invoked more than once" "yes" \
    "$([[ "$(wc -l < "${RECORDER}" | tr -d ' ')" -gt 1 ]] && echo yes || echo no)"

# =============================================================================
# THE ASSERTIONS
# =============================================================================

# --- one test log per arm, and they survive to the end of the campaign -------
mapfile -t ARM_TEST_LOGS < <(
    find "${LOG_DIR}" -type f -name '*test*' 2>/dev/null | sort
)
check "each arm leaves its own test log" "3" "${#ARM_TEST_LOGS[@]}"

# --- one build log per arm ---------------------------------------------------
#
# Asserted separately, not folded into the count above. An implementation that
# split TEST_LOG and left BUILD_LOG shared would satisfy a combined count while
# leaving every INVALID verdict unevidenced.
mapfile -t ARM_BUILD_LOGS < <(
    find "${LOG_DIR}" -type f -name '*build*' 2>/dev/null | sort
)
check "each arm leaves its own build log" "3" "${#ARM_BUILD_LOGS[@]}"

# --- the logs are not empty --------------------------------------------------
NONEMPTY=0
for log in "${ARM_TEST_LOGS[@]:-}"; do
    [[ -s "${log}" ]] && NONEMPTY=$((NONEMPTY+1))
done
check "every per-arm test log has content" "3" "${NONEMPTY}"

# --- the content came from THIS run ------------------------------------------
#
# A per-arm path that a previous campaign already populated would pass the
# count and the non-empty checks while containing another run's evidence.
WITH_NONCE=0
for log in "${ARM_TEST_LOGS[@]:-}"; do
    grep -q "${NONCE}" "${log}" 2>/dev/null && WITH_NONCE=$((WITH_NONCE+1))
done
check "every per-arm log was written by this run" "3" "${WITH_NONCE}"

# --- an arm's log is findable from its NAME ----------------------------------
#
# The point of the exercise is a reader with a verdict line in hand -- "SURVIVED
# beta arm" -- being able to open that arm's log. Numeric-only filenames make
# that a counting exercise against the mutants file, which is the derivation
# this fix exists to eliminate. So the arm name must appear in the path.
NAMED=0
for word in alpha beta gamma; do
    if find "${LOG_DIR}" -type f -name "*${word}*" 2>/dev/null | grep -q .; then
        NAMED=$((NAMED+1))
    fi
done
check "each arm's log is findable by its name" "3" "${NAMED}"

# --- the campaign SAYS where it put them -------------------------------------
#
# A directory nobody is told about is not evidence. The summary must name it,
# or the next README author derives attribution exactly as before.
check "the campaign reports where the per-arm logs are" "yes" \
    "$(grep -qa "${LOG_DIR}" "${FIXTURE}/out.log" && echo yes || echo no)"

echo
echo "  ${PASS} passed, ${FAIL} failed."
[[ "${FAIL}" -eq 0 ]]
