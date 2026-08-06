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

# RUN FROM A PRIVATE COPY. Bash does not read a script into memory: it reads
# lazily from an open fd, tracking a byte offset (`lsof` on a live campaign
# shows fd 255r on this file). Editing this file while a campaign runs shifts
# every offset past the insertion point, so the running shell resumes parsing
# mid-token and executes whatever text now sits at its saved offset.
#
# For most scripts that is a crash. Here it is worse and specifically so: this
# script copies over production source and applies deliberate defects. A
# corrupted resume can leave a mutant live in a shared checkout, apply one arm
# under another arm's name, or skip the restore entirely -- and each of those
# prints something that reads like a verdict. A campaign is a twenty-minute-per-
# arm process; "do not edit the file for the next four hours" is not a control,
# it is a hope. Tools/tests/test_mutate_selfcopy.sh reproduces the corruption on
# an unguarded stand-in as a control, then asserts this block is present and
# runs before anything destructive.
#
# The copy keeps the NAME mutate.sh (in a private dir) so `pgrep -f mutate.sh`
# still finds a running campaign -- that is how the edit-path refusal below, and
# a human, ask whether one is live.
#
# FLOWVIZ_MUTATE_ORIGIN carries the original Tools/ directory across the exec:
# PROJECT_DIR is derived from BASH_SOURCE, which after the re-exec points into
# the temp dir, and every path this script resolves -- verdict.sh, the repo root,
# the source file -- hangs off it.
if [[ -z "${FLOWVIZ_MUTATE_REEXEC:-}" ]]; then
    _origin_tools="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    _selfdir="$(mktemp -d -t flowviz_mutate_self)"
    if cat "${BASH_SOURCE[0]}" > "${_selfdir}/mutate.sh" 2>/dev/null; then
        chmod +x "${_selfdir}/mutate.sh"
        FLOWVIZ_MUTATE_REEXEC=1 \
        FLOWVIZ_MUTATE_ORIGIN="${_origin_tools}" \
        FLOWVIZ_MUTATE_SELFDIR="${_selfdir}" \
            exec "${_selfdir}/mutate.sh" "$@"
    fi
    # Copy failed (no space, read-only tmp). Fall through and run from the
    # checked-out file rather than refusing: an unprotected campaign is worth
    # more than no campaign, and the risk is only realised if someone edits it.
    rm -rf "${_selfdir}"
    echo "warning: could not create a private copy; edits to Tools/mutate.sh" >&2
    echo "         during this run WILL corrupt it." >&2
fi

# Remove the private copy on the way OUT, never right after the exec.
#
# Unlinking the image immediately after launching it KILLS the process on macOS
# -- measured, exit 137/SIGKILL -- because the kernel is still paging the image
# in. Unlinking one that is already running is safe. So the tidy-looking
# "delete it the instant we no longer need the path" is the one spelling that
# cannot work. The stand-in test measures all three cases (immediate rm, delayed
# rm, no rm) on the machine running the suite rather than trusting this note.
#
# This trap is REPLACED twice below, as arms are set up; each replacement
# re-adds this rm. A trap that only exists here would be silently dropped.
if [[ -n "${FLOWVIZ_MUTATE_SELFDIR:-}" ]]; then
    trap 'rm -rf "${FLOWVIZ_MUTATE_SELFDIR}"' EXIT
fi

SRC="${1:?usage: mutate.sh <source-file> <test-filter> <mutants-file>}"
FILTER="${2:?missing test filter}"
MUTANTS="${3:?missing mutants file}"

# After the re-exec BASH_SOURCE points into the private temp dir, so the origin
# passed through the exec is authoritative. Falling back to BASH_SOURCE covers
# the no-copy path above.
PROJECT_DIR="$(cd "${FLOWVIZ_MUTATE_ORIGIN:-$(dirname "${BASH_SOURCE[0]}")}/.." && pwd)"
REPO_ROOT="$(cd "${PROJECT_DIR}/.." && pwd)"
UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
BUILD_LOG="${BUILD_LOG:-/tmp/mutate_build.log}"
TEST_LOG="${TEST_LOG:-/tmp/mutate_test.log}"
SRC_BASE="$(basename "${SRC}")"

# WHERE THE EVIDENCE FOR EACH VERDICT IS KEPT.
#
# BUILD_LOG and TEST_LOG above are ONE PATH EACH, and the per-arm loop below
# redirects every arm's build and every arm's suite run into them. Each arm
# clobbers the last, so a finished ten-arm campaign leaves exactly one log: the
# tenth's.
#
# The verdicts are unaffected -- classify_* reads each log while it is still the
# current arm's. What is destroyed is the EVIDENCE, which is what a reader
# checks the verdicts against afterwards. Tools/mutants/session-workspace.README
# says so in its own words and asked for this fix:
#
#     WHICH ASSERTIONS KILLED WHICH ARM. DERIVED, NOT READ OFF A LOG - and that
#     is a defect in how this campaign was run rather than a property of the
#     results. ... A future campaign should point TEST_LOG at a per-arm path.
#
# Derivation is a claim about what a mutant SHOULD break, checked against
# nothing. Repo memory a-compound-mutant-scores-like-a-narrow-one is exactly
# what it cannot see: an arm named for three casualties that died on two, with
# the third riding along untested. Derivation reports all three, and reads as
# thorough.
#
# Observed live while this very fix was being written. The workspace-clock
# campaign's arm 5 came back UNSCORED with "no tests matched" -- a crashed
# editor, not a result -- and by the time the summary printed, arm 6 had already
# truncated the log holding the only account of it. The re-run had to start from
# nothing.
#
# ARCHIVED, NOT REDIRECTED. The active BUILD_LOG/TEST_LOG paths are left exactly
# as they were and each arm's logs are COPIED aside after it is scored. Pointing
# the classifiers at a moving target would put the log path inside the loop,
# where every existing test that pins BUILD_LOG/TEST_LOG stops describing the
# script that ships -- and those tests are the reason the classifier is
# trustworthy at all.
#
# Per campaign, not per checkout: mktemp -d, so two campaigns cannot land in one
# directory even if they start in the same second. Repo memory
# isolation-ends-at-the-shared-output-path.
MUTATE_LOG_DIR="${MUTATE_LOG_DIR:-$(mktemp -d -t mutate_logs)}"
mkdir -p "${MUTATE_LOG_DIR}"

# Copy this arm's logs somewhere the next arm will not overwrite.
#
# Named <index>-<arm name>-{build,test}.log. BOTH parts are load-bearing: the
# index preserves the order the summary prints in, and the name is what a reader
# holding a verdict line actually has in hand. Index alone would make finding an
# arm's log a counting exercise against the mutants file -- which is the
# derivation this exists to eliminate.
#
# Non-fatal by construction. A campaign that has scored an arm must not abort
# because it could not archive the paperwork; the trailing `true` keeps a full
# disk or a read-only /tmp from turning four good verdicts into none.
#
# The stem is derived in ONE place. The engine log has to be named before the
# suite runs (it is passed to run_tests.sh as LOG) while the build and test
# copies are named after, so two call sites need the same answer -- and two
# copies of the rule are two things to keep in step. A drift between them would
# put an arm's engine log under a name no reader would think to look for.
arm_log_stem() {  # arm_log_stem <index> <name> -> "<dir>/<index>-<slug>"
    local index="$1" name="$2" slug
    # Everything that is not alphanumeric becomes a dash, so an arm name
    # containing a slash cannot write outside the directory.
    slug="$(tr -c '[:alnum:]' '-' <<<"${name}" | cut -c1-60)"
    printf '%s/%s-%s' "${MUTATE_LOG_DIR}" "${index}" "${slug}"
}

archive_arm_logs() {
    local index="$1" name="$2" stem
    stem="$(arm_log_stem "${index}" "${name}")"
    cp "${BUILD_LOG}" "${stem}-build.log" 2>/dev/null || true
    cp "${TEST_LOG}" "${stem}-test.log" 2>/dev/null || true
    return 0
}

# Constraint 2: classification lives in its own file so it can be exercised
# against known-answer inputs without building anything. Sourced before the cd
# below, and by absolute path, because this script changes directory.
# shellcheck source=/dev/null
source "${PROJECT_DIR}/Tools/verdict.sh"

# check_harness_drift lives here, and the call at the end of this script had
# NEVER resolved without this line -- observed live 2026-08-06 as
# "line 623: check_harness_drift: command not found", printed after a
# five-arm campaign had already scored every arm.
#
# So the drift-provenance block -- the entire point of the self-copy work,
# which exists to say "these verdicts came from a harness that is no longer on
# disk" -- has been dead since it was written. It could not warn; it aborted.
#
# WHAT THE TEST SUITE SAW INSTEAD. test_mutate_selfcopy.sh sources
# mutation_window.sh itself before calling the function, so it was in scope for
# the test and absent here, and its check on THIS file was
# `grep -q 'check_harness_drift'` -- which asserts the call is written, not
# that it resolves. 26 green checks over a call that always died.
#
# Tools/tests/test_mutate_resolves_calls.sh runs a whole fixture campaign and
# fails if "command not found" appears anywhere in its output, which is the
# property grep cannot express.
# shellcheck source=/dev/null
source "${PROJECT_DIR}/Tools/mutation_window.sh"

cd "${REPO_ROOT}" || exit 2
[[ -f "${SRC}" ]] || { echo "error: no such source file: ${SRC}" >&2; exit 2; }

# BOTH path arguments are checked here, together, because both resolve against
# REPO_ROOT and only one of them used to say so.
#
# The mutants file was previously first touched at the parse, several hundred
# lines below -- past the baseline build AND the pristine suite. So the same
# typo (a path relative to FlowViz/ rather than to the repo root) cost two
# seconds on argument 1 and ten minutes on argument 3. Observed live
# 2026-08-06: a clip-seam campaign built and ran the whole suite, then aborted
# with FileNotFoundError on a mutants file it could have rejected instantly.
#
# The late abort was CORRECT -- it refused to score rather than reporting an
# empty run as a clean one -- so this is a cost fix, not a correctness fix.
# But the cost is not only the operator's time: between the cd and the parse
# this script writes the mutation marker and takes the global build lock, so a
# doomed invocation blocks every other agent's build to reach an error it
# already had the information to print.
#
# -r rather than -f: an unreadable file fails the parse just as surely as an
# absent one, and reports here rather than as a Python traceback.
#
# The message names the resolution root deliberately. "no such file" alone
# sends the reader off to confirm the file exists -- and it does exist, just
# not relative to the directory this script cd'd into. Naming the root is what
# turns the error into a diagnosis.
[[ -r "${MUTANTS}" ]] || {
    echo "error: no such mutants file: ${MUTANTS}" >&2
    echo "       relative paths resolve from ${REPO_ROOT}" >&2
    exit 2
}

# Is anyone else editing the file we are about to snapshot and repeatedly
# overwrite? Checked HERE, before the backup, because the backup is the thing
# that does the damage: every restore below is a whole-file `cp` from it, so a
# peer's edit landing between an arm and its restore is silently reverted.
#
# Observed live 2026-08-05: a campaign mutated FlowVizVolumeRayMarch.usf while
# another agent was landing a shader change in that same file. The lost work is
# the visible harm and the smaller one -- an arm's evidence is "the suite went
# red", and a peer's half-finished file goes red identically, so a KILLED that
# was really someone else's in-flight edit retires a gap instead of reporting
# it.
#
# Checked once, at start. By arm 2 the file is dirty BY DESIGN because this
# script made it so; re-checking would deadlock the loop.
_PORCELAIN="$(mktemp -t mutate_porcelain)"
git status --porcelain > "${_PORCELAIN}" 2>/dev/null || true
_SRC_REL="$(git ls-files --full-name "${SRC}" 2>/dev/null | head -1)"
[[ -n "${_SRC_REL}" ]] || _SRC_REL="${SRC}"
if [[ "$(check_foreign_edits "${_PORCELAIN}" "${_SRC_REL}")" == "foreign" ]]; then
    echo "ABORT: ${_SRC_REL} has uncommitted changes."
    echo
    echo "  This script snapshots that file and restores it with a whole-file"
    echo "  copy after every arm. Anything anyone lands in it meanwhile is"
    echo "  reverted with no error and no conflict marker -- and worse, their"
    echo "  half-finished file turns the suite red, which is indistinguishable"
    echo "  from a mutant being caught. Every arm would score a false KILLED."
    echo
    echo "  Commit or stash the change, or run the campaign in its own tree:"
    echo "    git worktree add ~/projects/wind-worktrees/<name> HEAD --detach"
    rm -f "${_PORCELAIN}"
    exit 2
fi
rm -f "${_PORCELAIN}"

BACKUP="$(mktemp -t mutate_backup)"
cp "${SRC}" "${BACKUP}"

# Constraint 4: restore on normal exit, on error, and on being killed.
#
# Declare the mutation window as well as restoring it. The restore protects THIS
# run's verdict; the marker protects everyone else's history. While a deliberate
# defect is live in a shared checkout, any other agent running `git add -A` can
# commit it under their own task's name, with an innocent message and a green
# suite beside it -- and unlike a corrupted verdict, nothing about that result
# looks wrong later. Tools/mutation_guard.sh (installed as .git/hooks/pre-commit)
# reads this file and refuses to commit while it exists.
#
# Under .git/ so that it survives the `git checkout -- .` and `git stash` that
# follow a mutation run, and so it can never itself be staged.
MARKER="$(git rev-parse --absolute-git-dir 2>/dev/null)/FLOWVIZ_MUTATION_ACTIVE"
#
# harness_sha fingerprints the copy this process is ACTUALLY executing, so the
# end of the run can tell whether Tools/mutate.sh was edited underneath it. The
# private copy above means such an edit no longer corrupts anything -- it means
# the campaign quietly finishes on the old code while the reader assumes the
# new. Recorded here, reported by report_harness_drift below.
HARNESS_SHA="$(shasum -a 256 < "${BASH_SOURCE[0]}" 2>/dev/null | cut -d' ' -f1)"
{
    echo "pid=$$"
    echo "source=${SRC}"
    echo "started=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "harness_sha=${HARNESS_SHA}"
} > "${MARKER}" 2>/dev/null || true

# The marker now stops builds as well as commits: Tools/mutation_window.sh,
# sourced by build_lock.sh and run_tests.sh, refuses to run in a checkout whose
# window is open. THIS campaign has to keep building, so it identifies itself by
# exporting the same pid it just declared. Only processes descended from here
# inherit it.
#
# Exported HERE, beside the declaration, so the two can never disagree -- and
# before the baseline build below, which is already inside the window and would
# otherwise refuse itself. Nothing about that deadlock would look like a guard
# working; it would look like the build breaking.
export FLOWVIZ_MUTATION_TOKEN="$$"

# Every exit path also drops the private copy this script is running from. It is
# folded into restore() rather than left as a separate trap because `trap ...
# EXIT` REPLACES the previous handler -- the self-copy trap installed at the top
# is gone the moment the line below runs. Putting the rm in the one function
# that all three later handlers call is what keeps it from being lost again.
drop_selfcopy() { [[ -n "${FLOWVIZ_MUTATE_SELFDIR:-}" ]] && rm -rf "${FLOWVIZ_MUTATE_SELFDIR}"; return 0; }
restore() { cp "${BACKUP}" "${SRC}"; rm -f "${MARKER}"; drop_selfcopy; }

# A SIGNAL ENDS THE CAMPAIGN. It used to restore and then carry on, because a
# trap handler resumes the script unless it exits -- this line was
# `trap restore EXIT INT TERM`, one handler for all three.
#
# Observed live 2026-08-05: a campaign was sent TERM so its mutant list could be
# corrected, and printed
#
#     SURVIVED  the classifier reports Dispatched no matter what
#
# for an arm that does not compile. The TERM arrived during that arm's build,
# restore() put the pristine file back, and the build and suite then ran against
# UNMUTATED SOURCE. A mutant that cannot build was reported as a coverage gap.
#
# That is the dangerous direction. A lost verdict announces itself; a
# manufactured one sends someone to write a test for a defect that is not there,
# and the arm it lands on is whichever happened to be building, so nothing about
# it reads as unusual afterwards.
#
# The quieter harm is the marker. mutation_guard.sh (the pre-commit hook) reads
# it to refuse a commit while a mutation is live, so clearing it with arms still
# to apply disarms that hook for the rest of the run -- precisely the window in
# which a live mutant can be committed.
#
# Tools/tests/test_mutate_signal.sh pins both spellings: it reproduces the old
# damage as a control, then asserts the new handler scores nothing further.
interrupted_notice() {
    echo >&2
    echo "INTERRUPTED: signalled mid-campaign. Verdicts printed before this" >&2
    echo "  line stand; nothing after it was scored, and the remaining arms" >&2
    echo "  were never run. Re-run to score them." >&2
}
on_signal() { restore; interrupted_notice; exit 143; }
trap restore EXIT
trap on_signal INT TERM

# Constraint 1: the exit status of Build.sh is meaningless; read the log.
# classify_build (verdict.sh) decides what the log says, and it is retried on
# `infrastructure` -- the UBT lock, or UnrealBuildTool falling over on the
# user-global state it keeps outside the worktree. Neither is a fact about the
# mutant, so neither is scored; they are waited out.
#
# build_lock.sh PREVENTS most of those rather than waiting them out. The retry
# above stays: the lock stops concurrent builds started through it, and cannot
# stop a build someone launches directly, so the classifier remains the
# backstop. Belt and braces, because the failure mode here is a mutant scored
# against another process's mess.
build() {
    local attempt
    for attempt in $(seq 1 40); do
        "${PROJECT_DIR}/Tools/build_lock.sh" \
            "${UE_ROOT}/Engine/Build/BatchFiles/Mac/Build.sh" \
            FlowVizEditor Mac Development \
            -project="${PROJECT_DIR}/FlowViz.uproject" >"${BUILD_LOG}" 2>&1
        # 75 = EX_TEMPFAIL from build_lock: the build NEVER RAN. The log holds
        # whatever the previous attempt left, so classifying it would score a
        # stale result against this mutant. Wait and retry instead.
        if [[ $? -eq 75 ]]; then
            sleep 15
            continue
        fi
        case "$(classify_build "${BUILD_LOG}")" in
            succeeded)      return 0 ;;
            # The build compiled NOTHING ("run 0 action(s)"), so the binary does
            # not contain this mutant and any test result would be about the
            # previous one. Force the source strictly newer and rebuild. If it
            # still compiles nothing, fall through to UNSCORED rather than score
            # a binary we know is stale.
            # -A takes [[hh]mm]SS and parses from the RIGHT, so the argument is
            # SECONDS unless it is long enough to reach the minutes and hours
            # fields: `01` is one second, `010000` is one hour. This read as
            # "+1 hour" and was "+1 second" -- see the comment at the touch in
            # build()'s tail for what that cost.
            noop)           touch -A 010000 "${SRC}"; sleep 1; continue ;;
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
    local status=$?
    # UnrealBuildTool rebuilds on MODIFICATION TIME, not content. `cp` from the
    # backup above can restore an mtime no newer than the existing .o, and the
    # write here happens within the same filesystem timestamp granularity, so
    # UBT can conclude "Target is up to date" and run 0 actions -- leaving the
    # tests to run against the previous, unmutated binary and score SURVIVED.
    # Touching into the future guarantees the source is strictly newer than any
    # object built from it.
    #
    # THE ARGUMENT IS NOT AN HOUR UNLESS IT IS SIX DIGITS. `man touch` gives
    # -A [-][[hh]mm]SS, parsed from the RIGHT, so `01` -- which is what stood
    # here -- advanced the mtime by ONE SECOND. Measured, not read:
    #
    #     -A 01     -> +1s        -A 0100 -> +60s        -A 010000 -> +3600s
    #
    # Live consequence 2026-08-06: a campaign spun for 25 minutes without
    # scoring an arm. Its source was 1683s behind the objects, the build ran 0
    # actions, `noop` above touched +1s and retried, and at ~40s per iteration
    # the source needed ~19 hours to overtake. The tree compiled perfectly the
    # whole time.
    #
    # The hang was the SAFE half. Here in build()'s tail the same literal runs
    # after every successful mutant build, so a build taking longer than a
    # second left the source older than its own objects -- and the NEXT arm
    # compiled nothing and was tested against the PREVIOUS arm's binary, which
    # scores SURVIVED. `noop` is the guard against exactly that, and this bug
    # sat inside the guard's remedy, so the protection and the hole were the
    # same line.
    #
    # Tools/tests/test_mtime_advance.sh pins the platform semantics and both
    # sites. Its assertions are about MAGNITUDE: "the mtime advanced" is true of
    # the bug.
    [[ ${status} -eq 0 ]] && touch -A 010000 "${SRC}"
    return ${status}
}

echo "=== waiting for a clean build before scoring anything ==="
BASELINE_OK=0
for attempt in $(seq 1 60); do
    if build; then echo "baseline builds (attempt ${attempt})"; BASELINE_OK=1; break; fi
    sleep 20
done
if [[ "${BASELINE_OK}" -ne 1 ]]; then
    echo "ABORT: the unmutated tree never built clean, so no verdict would mean anything."
    grep -aE "error:" "${BUILD_LOG}" | head -5
    exit 1
fi

# THE SUITE MUST BE GREEN, NOT MERELY COMPILABLE.
#
# This block used to be the loop above and nothing else, under a banner reading
# "waiting for a green baseline before scoring anything". It established that
# the tree COMPILES. The word "green" was doing work the code never did:
# run_tests.sh appeared exactly once in this script, inside the per-mutant loop.
#
# The gap forges reports rather than losing them. If any test matching FILTER is
# already failing, every arm inherits that failure -- and classify_test_run is
# right to call it `killed`, because tests really did run and one really did
# fail. What it cannot see is that the failure PREDATES the mutant. The campaign
# then reports every arm KILLED, which reads as a thoroughly covered file, and
# nothing in the output looks wrong.
#
# Live example, which is how this was found: FlowViz.Render.Wiring is a standing
# expected-red (task #26). Any campaign filtered on `FlowViz` or `FlowViz.Render`
# would have scored all-KILLED while testing nothing.
#
# This is the harness-level form of the identity control -- a KILLED means
# nothing unless the pristine tree is green under THAT filter.
echo "=== running the suite pristine: a KILLED means nothing without this ==="
BASELINE_TEST_LOG="${BASELINE_TEST_LOG:-$(mktemp -t mutate_baseline)}"
# The baseline gets its own engine log too, for the same reason the arms do --
# and more so. This is the identity control: every killed in the summary means
# "the suite was green here and red under the mutant", so when a campaign comes
# back all-KILLED the first thing to check is whether the pristine run was
# actually green. Leaving it on the shared per-checkout path meant arm 1
# overwrote the evidence for that before anyone could read it.
BASELINE_ENGINE_LOG="${MUTATE_LOG_DIR}/00-baseline-engine.log"
for _lock_attempt in $(seq 1 40); do
    LOG="${BASELINE_ENGINE_LOG}" "${PROJECT_DIR}/Tools/build_lock.sh" \
        "${PROJECT_DIR}/Tools/run_tests.sh" "${FILTER}" >"${BASELINE_TEST_LOG}" 2>&1
    BASELINE_EXIT=$?
    [[ "${BASELINE_EXIT}" -ne 75 ]] && break
    sleep 15
done

case "$(classify_baseline_run "${BASELINE_EXIT}" "${BASELINE_TEST_LOG}")" in
    green)
        echo "baseline green under '${FILTER}'"
        ;;
    red)
        echo "ABORT: '${FILTER}' is ALREADY RED before anything was mutated."
        echo
        echo "  Already failing:"
        baseline_failures "${BASELINE_TEST_LOG}" | sed 's/^/    /'
        echo
        echo "  Every arm would inherit these failures and score KILLED without"
        echo "  being tested -- an all-KILLED report that certifies nothing."
        echo "  Narrow FILTER to exclude them, or fix them first."
        echo "  Full output: ${BASELINE_TEST_LOG}"
        exit 1
        ;;
    *)
        echo "ABORT: the pristine suite produced no usable result under '${FILTER}'."
        echo
        echo "  No test run means no baseline, and no baseline means no arm can"
        echo "  be scored. Usual causes: the filter matched nothing, every"
        echo "  matched test skipped itself, or the editor died on startup."
        grep -aiE "no tests matched|assertion failed|critical error|SIGSEGV|failed to start" \
            "${BASELINE_TEST_LOG}" | head -3 | sed 's/^/    /'
        echo "  Full output: ${BASELINE_TEST_LOG}"
        exit 1
        ;;
esac

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
# SAME SPLIT AS THE TRAP ABOVE, and for the same reason. This pair REPLACES the
# earlier one -- a second `trap ... INT TERM` overrides the first -- so writing
# it as one handler for all three signals here would reinstate the defect for
# every arm in the loop below, which is the entire campaign. The split has to be
# repeated, not merely established once.
cleanup() { restore; rm -f "${RECORDS}"; }
on_signal() { cleanup; interrupted_notice; exit 143; }
trap cleanup EXIT
trap on_signal INT TERM

if ! python3 "${PROJECT_DIR}/Tools/parse_mutants.py" "${MUTANTS}" > "${RECORDS}"; then
    echo "ABORT: could not parse ${MUTANTS}; no verdict would mean anything."
    exit 2
fi

ARM_INDEX=0

while IFS=$'\t' read -r NAME_J FROM_J TO_J; do
    NAME=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]))' "${NAME_J}")
    FROM=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]))' "${FROM_J}")
    TO=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]))' "${TO_J}")

    # Zero-padded so `sort` and `ls` order the archive the way the summary
    # prints it. Incremented for EVERY record, including ones that go on to
    # SKIP, so an arm's index is its position in the mutants file rather than
    # its position among the arms that happened to run -- the second would
    # renumber every later arm whenever an earlier one went stale.
    ARM_INDEX=$((ARM_INDEX+1))
    ARM_TAG="$(printf '%02d' "${ARM_INDEX}")"

    cp "${BACKUP}" "${SRC}"
    apply_mutant "${FROM}" "${TO}"
    case $? in
        # No archive on these two paths, deliberately: nothing was built and
        # nothing was run, so BUILD_LOG and TEST_LOG still hold the PREVIOUS
        # arm's output. Copying them here would file one arm's evidence under
        # another arm's name, which is worse than having none.
        3) echo "SKIP      ${NAME} -- pattern not found; the mutant is stale"
           SKIPPED=$((SKIPPED+1)); continue ;;
        4) echo "SKIP      ${NAME} -- pattern matches more than once; too broad"
           SKIPPED=$((SKIPPED+1)); continue ;;
    esac

    if build; then
        # Constraint 2: the exit code decides, not a string in the output --
        # but only once classify_test_run has established that tests actually
        # ran. An engine that crashes on startup also exits non-zero.
        # Locked for the same reason as the build: a concurrent editor launch
        # contends on the same user-global UBT/trace state, and the resulting
        # noise is indistinguishable from a test result. A 75 here means the
        # suite never ran, so it must not reach classify_test_run -- that would
        # score a stale TEST_LOG against this mutant.
        #
        # Bounded, not a `while`. 75 is build_lock's EX_TEMPFAIL, but nothing
        # stops the engine from someday exiting 75 for its own reasons, and an
        # unbounded retry on that would hang the campaign forever with no
        # verdict. After the bound the 75 is passed through to
        # classify_test_run, which finds no `<n>/<m> passed.` summary in the log
        # and returns UNSCORED -- a refusal to conclude, which is the correct
        # answer for a run that never happened.
        # A per-arm engine log. run_tests.sh's default is derived from the
        # CHECKOUT path, so every arm of a campaign wrote the same file and
        # each summary's "Full log:" line pointed at whichever arm ran last.
        # TEST_LOG holds mutate.sh's own summary of the run; the engine detail
        # -- the stack, the assertion text, which test started -- lives in the
        # file run_tests.sh writes, and that was the copy being overwritten.
        # Attribution could only ever be DERIVED by matching timestamps.
        #
        # Exported rather than passed as an argument: run_tests.sh already
        # honours LOG, and build_lock.sh sits between us and it.
        ARM_ENGINE_LOG="$(arm_log_stem "${ARM_TAG}" "${NAME}")-engine.log"
        for _lock_attempt in $(seq 1 40); do
            LOG="${ARM_ENGINE_LOG}" "${PROJECT_DIR}/Tools/build_lock.sh" \
                "${PROJECT_DIR}/Tools/run_tests.sh" "${FILTER}" >"${TEST_LOG}" 2>&1
            TEST_EXIT=$?
            [[ "${TEST_EXIT}" -ne 75 ]] && break
            sleep 15
        done
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

    # ONE CALL, AFTER THE WHOLE if/elif/else, covering every path that got as
    # far as a build. Placing it inside each branch would mean a branch added
    # later silently ships without evidence -- and UNSCORED and INVALID are the
    # two verdicts whose logs are read most, because they are the ones that
    # have to be re-run.
    archive_arm_logs "${ARM_TAG}" "${NAME}"
done < "${RECORDS}"

cp "${BACKUP}" "${SRC}"
echo
echo "=== ${SRC_BASE} vs ${FILTER} ==="
echo "killed ${KILLED}  SURVIVED ${SURVIVED}  INVALID ${INVALID}  UNSCORED ${UNSCORED}  skipped ${SKIPPED}"
# Printed unconditionally, and printed HERE rather than at startup: a reader
# reaches for the logs when they see a verdict they want to check, and a path
# announced twenty minutes and one build earlier has scrolled away.
echo "per-arm logs: ${MUTATE_LOG_DIR}"

# Did Tools/mutate.sh change while this campaign was running? The private copy
# means such an edit could not corrupt anything -- and that is exactly why it
# has to be said out loud. The verdicts above are sound, but they describe a
# harness that is no longer the one in the checkout, and a reader comparing them
# against the current file would be comparing against code that never ran.
#
# Printed AFTER the summary, and never changes the exit status: the run was
# coherent. This is provenance, not a failure.
_drift="$(check_harness_drift "${MARKER}" "$(shasum -a 256 < "${PROJECT_DIR}/Tools/mutate.sh" 2>/dev/null | cut -d' ' -f1)")"
case "${_drift}" in
    drifted)
        echo
        echo "NOTE: Tools/mutate.sh was EDITED while this campaign ran."
        echo "  The verdicts above are sound -- this run executed from a private"
        echo "  copy taken at startup, so the edit could not reach it. But they"
        echo "  were produced by the PRE-EDIT harness. Reading them as evidence"
        echo "  about the file now on disk attributes them to code that never ran."
        echo "  Re-run to score against the current harness."
        ;;
    unknown)
        echo
        echo "NOTE: could not tell whether Tools/mutate.sh changed during this run."
        ;;
esac

[[ "${SURVIVED}" -eq 0 && "${UNSCORED}" -eq 0 ]] || exit 1
