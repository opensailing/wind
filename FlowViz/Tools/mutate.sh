#!/usr/bin/env bash
#
# Mutation-verify a test file against the source it claims to cover.
#
#   Tools/mutate.sh <source-file> <test-filter> <mutants-file>
#
# A test that passes against a deliberately broken implementation is not
# testing that implementation. This script breaks the source one edit at a
# time and reports, per edit, whether the suite noticed.
#
#   killed    the suite failed -- the behaviour is genuinely covered
#   SURVIVED  the suite passed anyway -- there is no real check here
#   INVALID   the mutant did not compile, attributed to THIS file
#   UNSCORED  the build broke somewhere else; nothing can be concluded
#
# The mutants file is a series of records separated by a line of "%%":
#
#     name of the mutant
#     --
#     text to find (verbatim, may span lines)
#     --
#     text to replace it with (verbatim, may span lines)
#     %%
#
# Both halves are matched and inserted literally. Earlier versions of this
# script passed them through perl s{}{}, which silently mangled any mutant
# whose replacement contained a brace -- the s{} delimiter -- and reported the
# resulting garbage as "did not compile". Substitution is done in python with
# str.replace so no character is special.
#
# ---------------------------------------------------------------------------
# Five constraints, every one of them learned by getting a wrong answer first
# and believing it. Constraint 2 has now been learned twice, from opposite
# directions, which is why the parts of this script that decide anything --
# classification and parsing -- are the parts under test.
#
#  1. Build.sh EXITS 0 WHEN IT FAILS. Only the printed "Result:" line is
#     authoritative. Never trust $? from a UE build.
#
#  2. A KILL REQUIRES EVIDENCE THAT TESTS RAN AND ONE FAILED -- not merely an
#     unhappy exit code, and never a grep for a word. This constraint has been
#     violated in both directions:
#
#       - A previous script tested for "Result={Failed}"; the engine prints
#         "Result={Fail}". That grep could not match under any circumstance, so
#         the script reported SURVIVED for every mutant it ever scored,
#         including one whose own captured log said the test had failed.
#
#       - Its replacement read ANY non-zero exit as a kill. An editor that dies
#         on startup exits non-zero too ("Assertion failed: bDirectoryExists"
#         in ShaderCore.cpp, then SIGSEGV, under heavy machine load), so a
#         crash that never reached the test was about to be recorded as proof
#         the suite catches the mutant.
#
#     The two failures are opposite and unequal: a false SURVIVED asks for a
#     test that already exists, while a false killed retires a check that was
#     never exercised. Classification therefore lives in Tools/verdict.sh, is
#     tested against known-answer inputs by Tools/tests/test_verdict.sh, and
#     requires the runner's "<n>/<m> passed." summary before it will read the
#     exit code at all. No summary means UNSCORED -- a refusal to conclude.
#
#  3. BUILD FAILURES MUST BE ATTRIBUTED. This tree is a unity build shared by
#     several agents: every .cpp in the module compiles as one translation
#     unit, so a sibling's half-saved file breaks *our* build. The old script
#     tried to separate the two by restoring and rebuilding -- but if the
#     sibling finished saving in between, the clean build went green and our
#     mutant was blamed for their breakage. That is how a valid mutant was
#     recorded INVALID. Errors are instead attributed by filename, and
#     anything not traceable to the mutated file is UNSCORED, never INVALID.
#
#  4. THE SOURCE IS RESTORED ON EVERY EXIT PATH, via trap. A mutant left in a
#     shared tree is not just a lost result: other agents build that tree, and
#     one was found still mutated long after its run had ended.
#
#  5. THIS CAMPAIGN'S MUTANT LIST BELONGS TO THIS CAMPAIGN. Parsing wrote the
#     records to a fixed /tmp filename and the scoring loop read from that same
#     fixed name, so two campaigns running at once shared one list. A payload
#     campaign was observed scoring a GPU texture mutant it had never been
#     given, while its own second mutant silently disappeared. That it surfaced
#     as a harmless SKIP was luck: the foreign pattern did not match the file
#     being mutated. One that DID match would have been applied, built, scored
#     and reported under another mutant's name -- a verdict that looks exactly
#     like a result and is about something else entirely. The records now go to
#     a mktemp file, and parsing lives in Tools/parse_mutants.py where
#     Tools/tests/test_mutant_isolation.sh runs two parses concurrently and
#     checks neither sees the other's mutants.
#
# ---------------------------------------------------------------------------
# RUN THIS IN A GIT WORKTREE, AND NOT ONE UNDER /tmp.
#
#     mkdir -p ~/projects/wind-worktrees
#     git worktree add ~/projects/wind-worktrees/<name> HEAD --detach
#
# Isolation matters because concurrent campaigns in one tree corrupt each
# other's verdicts in both directions, and the dangerous direction is a false
# *killed*: it records a check as verified when something else broke the build.
# Builds still serialize on UBT's global lock, which is only wall-clock.
#
# The /tmp restriction is a separate, macOS-specific trap. /tmp is a symlink to
# /private/tmp, and Unreal Build Accelerator caches one spelling then looks up
# the other, so it never writes the shared PCH:
#
#     UbaSessionServer - Refusing to register create-for-write '/tmp/...gch': dir not populated
#     UbaSessionServer - Failed to get file information for /private/tmp/...gch.tmp
#
# Every compile then fails with `unable to read PCH file`, and the build ends
# `Result: Failed (OtherCompilationError)`. This is worth recognising on sight:
# it is dozens of error: lines that mention no source file you touched, and it
# looks exactly like a real compile failure. Constraint 3 keeps it from being
# scored as INVALID -- no error names the mutated file, so it comes back
# UNSCORED -- but a correct refusal to conclude is still not a verdict.
#
set -uo pipefail

SRC="${1:?usage: mutate.sh <source-file> <test-filter> <mutants-file>}"
FILTER="${2:?missing test filter}"
MUTANTS="${3:?missing mutants file}"

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "${PROJECT_DIR}/.." && pwd)"
UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
BUILD_LOG="${BUILD_LOG:-/tmp/mutate_build.log}"
TEST_LOG="${TEST_LOG:-/tmp/mutate_test.log}"
SRC_BASE="$(basename "${SRC}")"

# Constraint 2: classification lives in its own file so it can be exercised
# against known-answer inputs without building anything. Sourced before the cd
# below, and by absolute path, because this script changes directory.
# shellcheck source=/dev/null
source "${PROJECT_DIR}/Tools/verdict.sh"

cd "${REPO_ROOT}" || exit 2
[[ -f "${SRC}" ]] || { echo "error: no such source file: ${SRC}" >&2; exit 2; }

BACKUP="$(mktemp -t mutate_backup)"
cp "${SRC}" "${BACKUP}"

# Constraint 4: restore on normal exit, on error, and on being killed.
restore() { cp "${BACKUP}" "${SRC}"; }
trap restore EXIT INT TERM

# Constraint 1: the exit status of Build.sh is meaningless; read the log.
# classify_build (verdict.sh) decides what the log says, and it is retried on
# `infrastructure` -- the UBT lock, or UnrealBuildTool falling over on the
# user-global state it keeps outside the worktree. Neither is a fact about the
# mutant, so neither is scored; they are waited out.
build() {
    local attempt
    for attempt in $(seq 1 40); do
        "${UE_ROOT}/Engine/Build/BatchFiles/Mac/Build.sh" \
            FlowVizEditor Mac Development \
            -project="${PROJECT_DIR}/FlowViz.uproject" >"${BUILD_LOG}" 2>&1
        case "$(classify_build "${BUILD_LOG}")" in
            succeeded)      return 0 ;;
            infrastructure) sleep 15; continue ;;
            *)              return 1 ;;
        esac
    done
    # Out of attempts and still not green. Returning 1 hands the decision to
    # the caller, which finds no error naming the mutated file and records
    # UNSCORED -- the right answer for a machine that would not build.
    return 1
}

# Constraint 3: did the compiler blame the file we mutated, or someone else's?
mutant_is_to_blame() {
    grep -aE "error:" "${BUILD_LOG}" | grep -aq "${SRC_BASE}"
}

apply_mutant() {
    FROM="$1" TO="$2" TARGET="${SRC}" python3 - <<'PY'
import os, sys
target, frm, to = os.environ['TARGET'], os.environ['FROM'], os.environ['TO']
text = open(target, encoding='utf-8').read()
count = text.count(frm)
if count == 0:
    sys.exit(3)          # pattern absent -- the mutant is stale
if count > 1:
    sys.exit(4)          # ambiguous -- would mutate more than intended
open(target, 'w', encoding='utf-8').write(text.replace(frm, to))
PY
}

echo "=== waiting for a green baseline before scoring anything ==="
BASELINE_OK=0
for attempt in $(seq 1 60); do
    if build; then echo "baseline green (attempt ${attempt})"; BASELINE_OK=1; break; fi
    sleep 20
done
if [[ "${BASELINE_OK}" -ne 1 ]]; then
    echo "ABORT: the unmutated tree never built clean, so no verdict would mean anything."
    grep -aE "error:" "${BUILD_LOG}" | head -5
    exit 1
fi

KILLED=0; SURVIVED=0; INVALID=0; UNSCORED=0; SKIPPED=0

# Constraint 5: this campaign's mutant list belongs to this campaign.
#
# Parsing used to write to /tmp/mutate_records.tsv -- a fixed name -- and the
# loop below read from that same fixed name. Two campaigns at once shared one
# file, and a payload campaign was seen scoring a GPU texture mutant it had
# never been given while its own second mutant silently vanished. It showed up
# as a harmless SKIP only because the foreign pattern happened not to match;
# one that matched would have been applied, built, scored, and reported under
# the wrong name in the wrong campaign.
#
# So the records go to a per-run temporary file, and parsing lives in
# parse_mutants.py where Tools/tests/test_mutant_isolation.sh exercises it
# concurrently. RECORDS is created by mktemp, so two campaigns cannot collide
# even if they start in the same second.
RECORDS="$(mktemp -t mutate_records)"
cleanup() { restore; rm -f "${RECORDS}"; }
trap cleanup EXIT INT TERM

if ! python3 "${PROJECT_DIR}/Tools/parse_mutants.py" "${MUTANTS}" > "${RECORDS}"; then
    echo "ABORT: could not parse ${MUTANTS}; no verdict would mean anything."
    exit 2
fi

while IFS=$'\t' read -r NAME_J FROM_J TO_J; do
    NAME=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]))' "${NAME_J}")
    FROM=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]))' "${FROM_J}")
    TO=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]))' "${TO_J}")

    cp "${BACKUP}" "${SRC}"
    apply_mutant "${FROM}" "${TO}"
    case $? in
        3) echo "SKIP      ${NAME} -- pattern not found; the mutant is stale"
           SKIPPED=$((SKIPPED+1)); continue ;;
        4) echo "SKIP      ${NAME} -- pattern matches more than once; too broad"
           SKIPPED=$((SKIPPED+1)); continue ;;
    esac

    if build; then
        # Constraint 2: the exit code decides, not a string in the output --
        # but only once classify_test_run has established that tests actually
        # ran. An engine that crashes on startup also exits non-zero.
        "${PROJECT_DIR}/Tools/run_tests.sh" "${FILTER}" >"${TEST_LOG}" 2>&1
        TEST_EXIT=$?
        case "$(classify_test_run "${TEST_EXIT}" "${TEST_LOG}")" in
            killed)
                echo "killed    ${NAME}"
                KILLED=$((KILLED+1)) ;;
            SURVIVED)
                echo "SURVIVED  ${NAME}   <-- the suite does NOT catch this"
                SURVIVED=$((SURVIVED+1)) ;;
            *)
                echo "UNSCORED  ${NAME} -- the test run produced no results; no conclusion drawn"
                grep -aiE "assertion failed|critical error|SIGSEGV|no tests matched|failed to start" \
                    "${TEST_LOG}" | head -2 | sed 's/^/            /'
                UNSCORED=$((UNSCORED+1)) ;;
        esac
    elif mutant_is_to_blame; then
        echo "INVALID   ${NAME} -- does not compile (error names ${SRC_BASE})"
        grep -aE "error:" "${BUILD_LOG}" | grep -a "${SRC_BASE}" | head -2 | sed 's/^/            /'
        INVALID=$((INVALID+1))
    else
        echo "UNSCORED  ${NAME} -- build broke elsewhere; no conclusion drawn"
        grep -aE "error:" "${BUILD_LOG}" | head -2 | sed 's/^/            /'
        UNSCORED=$((UNSCORED+1))
    fi
done < "${RECORDS}"

cp "${BACKUP}" "${SRC}"
echo
echo "=== ${SRC_BASE} vs ${FILTER} ==="
echo "killed ${KILLED}  SURVIVED ${SURVIVED}  INVALID ${INVALID}  UNSCORED ${UNSCORED}  skipped ${SKIPPED}"
[[ "${SURVIVED}" -eq 0 && "${UNSCORED}" -eq 0 ]] || exit 1
