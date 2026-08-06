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
#
# IT ALSO EMULATES run_tests.sh's ENGINE LOG, which is the second defect this
# file covers. The real run_tests.sh writes the engine's detail log to a path it
# derives from the CHECKOUT (not the run), truncating it via -abslog, and then
# prints "Full log: <path>" as the last line of its summary. So the stub does
# the same: one session record per invocation, written to ${LOG} if the caller
# set one and to a fixed shared path otherwise, plus the pointer line.
#
# Without this the fixture cannot see the defect at all -- the summaries would
# have no "Full log:" pointer to be dangling.
cat > "${FIXTURE}/Proj/Tools/build_lock.sh" <<'STUB'
#!/bin/bash
echo "invoked: $*" >> "${RECORDER}"
echo "FIXTURE_NONCE ${FIXTURE_NONCE}"
echo "Using Unreal Build Accelerator local executor to run 3 action(s)"
echo "Result: Succeeded"

# Only the suite phases behave like run_tests.sh; the build phases do not.
if [[ "$*" == *run_tests.sh* ]]; then
    SESSION=$(( $(cat "${SESSION_COUNTER}" 2>/dev/null || echo 0) + 1 ))
    echo "${SESSION}" > "${SESSION_COUNTER}"

    ENGINE_LOG="${LOG:-${SHARED_ENGINE_LOG}}"
    # Truncating, exactly like -abslog: a session REPLACES whatever was there.
    {
        echo "LogInit: Session CrashGUID nonce=${FIXTURE_NONCE}"
        echo "ENGINE_SESSION ${SESSION}"
    } > "${ENGINE_LOG}"

    # The session number goes to STDOUT as well as into the log. That is what
    # makes "this summary describes that log" a checkable claim: a reader with
    # an archived summary in hand can compare the session it reports against
    # the session the file it cites contains. Real run_tests.sh output carries
    # the same correspondence via the engine's own session banner.
    echo "ENGINE_SESSION ${SESSION}"
    echo "2/2 passed. Full log: ${ENGINE_LOG}"
else
    echo "2/2 passed."
fi
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

# Where the stub writes its engine log when the caller sets no LOG -- i.e. the
# shared per-checkout path the real run_tests.sh defaults to.
SHARED_ENGINE_LOG="${FIXTURE}/shared_engine.log"
SESSION_COUNTER="${FIXTURE}/session_counter"
: > "${SESSION_COUNTER}"

# LOG_DIR is where per-arm output is expected to land. Passed explicitly so the
# fixture never writes into /tmp shared with a live campaign -- repo memory
# isolation-ends-at-the-shared-output-path, which is the same defect one level
# up from the one under test.
LOG_DIR="${FIXTURE}/logs"

(
    cd "${FIXTURE}" || exit 99
    RECORDER="${RECORDER}" \
    FIXTURE_NONCE="${NONCE}" \
    SHARED_ENGINE_LOG="${SHARED_ENGINE_LOG}" \
    SESSION_COUNTER="${SESSION_COUNTER}" \
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
ARM_TEST_LOGS=()
while IFS= read -r f; do
    [[ -n "${f}" ]] && ARM_TEST_LOGS+=("${f}")
done < <(
    find "${LOG_DIR}" -type f -name '*test*' 2>/dev/null | sort
)
check "each arm leaves its own test log" "3" "${#ARM_TEST_LOGS[@]}"

# --- one build log per arm ---------------------------------------------------
#
# Asserted separately, not folded into the count above. An implementation that
# split TEST_LOG and left BUILD_LOG shared would satisfy a combined count while
# leaving every INVALID verdict unevidenced.
ARM_BUILD_LOGS=()
while IFS= read -r f; do
    [[ -n "${f}" ]] && ARM_BUILD_LOGS+=("${f}")
done < <(
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

# =============================================================================
# THE ENGINE DETAIL LOG -- the half the archive above does NOT cover
# =============================================================================
#
# The archived files above are run_tests.sh's SUMMARY. The summary's last line
# is a pointer:
#
#     Full log: /tmp/flowviz_tests_a9d81a30.log
#
# and run_tests.sh derives that path from the CHECKOUT, not the run:
#
#     _checkout_tag="$(printf '%s' "${PROJECT_DIR}" | shasum | cut -c1-8)"
#     LOG="${LOG:-/tmp/flowviz_tests_${_checkout_tag}.log}"
#
# Every arm of a campaign therefore names the SAME file, and the engine
# truncates it on each launch (-abslog). When the campaign ends, that file holds
# only the LAST arm's session, so every earlier arm's archived summary cites a
# file that no longer contains its evidence. A citation that resolves to
# somebody else's data is worse than a missing one.
#
# Observed 2026-08-06 on the workspace-clock re-run: three arms, all three
# summaries ending "Full log: /tmp/flowviz_tests_a9d81a30.log", one session in
# it (`grep -ac "LogInit: Session CrashGUID"` == 1), mtime matching arm 3. I
# opened it looking for arm 2, found Result={Success}, and briefly concluded
# arm 2 had passed. It was the identity control's session. Repo memory
# shared-tree-results-need-a-timestamp.

# --- THE CONTROL, asserted FIRST ---------------------------------------------
#
# The whole claim rests on "a shared path retains only the last session". If the
# fixture's stub does not actually overwrite, then every assertion below passes
# against a defect that was never reproduced -- the test would be describing a
# hazard the fixture cannot exhibit.
#
# So: demonstrate the failing behaviour directly, on the fixture's own stub,
# before asserting the fix. Two sessions written to one path must leave one.
CTRL_SHARED="${FIXTURE}/control_shared.log"
( LOG="${CTRL_SHARED}" SESSION_COUNTER="${FIXTURE}/ctrl_counter" \
  RECORDER="/dev/null" FIXTURE_NONCE="${NONCE}" \
  SHARED_ENGINE_LOG="${CTRL_SHARED}" \
  "${FIXTURE}/Proj/Tools/build_lock.sh" run_tests.sh Filter >/dev/null 2>&1 )
( LOG="${CTRL_SHARED}" SESSION_COUNTER="${FIXTURE}/ctrl_counter" \
  RECORDER="/dev/null" FIXTURE_NONCE="${NONCE}" \
  SHARED_ENGINE_LOG="${CTRL_SHARED}" \
  "${FIXTURE}/Proj/Tools/build_lock.sh" run_tests.sh Filter >/dev/null 2>&1 )
check "CONTROL: two sessions at one path leave exactly one" "1" \
    "$(grep -c "LogInit: Session CrashGUID" "${CTRL_SHARED}" 2>/dev/null || echo 0)"

# --- the shared default must no longer be what the arms use ------------------
#
# If mutate.sh sets a per-arm LOG, the stub never falls back to
# SHARED_ENGINE_LOG and that file is never created at all.
check "no arm wrote to the shared per-checkout engine log" "no" \
    "$([[ -s "${SHARED_ENGINE_LOG}" ]] && echo yes || echo no)"

# --- one engine log per arm, archived alongside the summary ------------------
#
# The BASELINE run leaves an engine log too, and it is not an arm: it is the
# identity control, the run that decides whether a killed means anything. So
# the arm assertions below count arm logs specifically rather than every file
# with "engine" in the name -- a bare count would move with the baseline and
# stop describing the arms.
ARM_ENGINE_LOGS=()
while IFS= read -r f; do
    [[ -n "${f}" ]] && ARM_ENGINE_LOGS+=("${f}")
done < <(
    find "${LOG_DIR}" -type f -name '*engine*' ! -name '*baseline*' 2>/dev/null | sort
)
check "each arm leaves its own engine detail log" "3" "${#ARM_ENGINE_LOGS[@]}"

# The control's own log is separate and asserted on its own. Arm 1 used to
# overwrite it before anyone could read it, which is the evidence you reach for
# first when a campaign returns all-KILLED.
BASELINE_ENGINE_LOGS=()
while IFS= read -r f; do
    [[ -n "${f}" ]] && BASELINE_ENGINE_LOGS+=("${f}")
done < <(
    find "${LOG_DIR}" -type f -name '*baseline*engine*' 2>/dev/null | sort
)
check "the baseline keeps its own engine log, not overwritten by arm 1" "1" \
    "${#BASELINE_ENGINE_LOGS[@]}"

# --- each holds exactly ONE session, and they are DIFFERENT sessions ---------
#
# Counting files is not enough: three copies of the same last-arm log would
# satisfy it. The stub stamps an incrementing ENGINE_SESSION, so distinct
# session numbers prove each arm's own run was captured rather than a snapshot
# of a shared file taken three times.
ONE_SESSION=0
for log in "${ARM_ENGINE_LOGS[@]:-}"; do
    [[ "$(grep -c "LogInit: Session CrashGUID" "${log}" 2>/dev/null || echo 0)" -eq 1 ]] \
        && ONE_SESSION=$((ONE_SESSION+1))
done
check "every per-arm engine log holds exactly one session" "3" "${ONE_SESSION}"

DISTINCT="$(cat "${ARM_ENGINE_LOGS[@]:-/dev/null}" 2>/dev/null \
    | grep -o "ENGINE_SESSION [0-9]*" | sort -u | wc -l | tr -d ' ')"
check "and the three arms captured three DIFFERENT sessions" "3" "${DISTINCT}"

# --- the summary's pointer must resolve to that arm's own log ----------------
#
# The actual reader experience: open arm 2's archived summary, follow its
# "Full log:" line, and land on arm 2's session. This is the assertion the
# whole section exists for -- the others are its preconditions.
RESOLVES=0
POINTERS=""
for summary in "${ARM_TEST_LOGS[@]:-}"; do
    pointer="$(grep -o "Full log: .*" "${summary}" 2>/dev/null | tail -1 | sed 's/^Full log: //')"
    [[ -n "${pointer}" && -s "${pointer}" ]] || continue
    POINTERS="${POINTERS}${pointer}"$'\n'
    # The session the SUMMARY reports, compared against the session the log it
    # cites actually contains. Read from the summary only -- an earlier version
    # fell back to reading the number out of the pointer when the summary had
    # none, which greps a file for a string taken from that same file and
    # cannot fail. It passed while all three arms shared one log.
    want="$(grep -o "ENGINE_SESSION [0-9]*" "${summary}" 2>/dev/null | tail -1)"
    [[ -n "${want}" ]] || continue
    grep -q "${want}" "${pointer}" 2>/dev/null && RESOLVES=$((RESOLVES+1))
done
check "each arm's summary points at a log holding that arm's session" "3" "${RESOLVES}"

# Three summaries citing ONE path satisfies the count above while being the
# exact defect this task exists to fix.
check "and the three summaries cite three DIFFERENT paths" "3" \
    "$(printf '%s' "${POINTERS}" | sort -u | grep -c . | tr -d ' ')"

echo
echo "  ${PASS} passed, ${FAIL} failed."
[[ "${FAIL}" -eq 0 ]]
