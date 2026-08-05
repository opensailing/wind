# Verdict classification for mutate.sh. Sourced, not executed.
#
# Split out of mutate.sh so it can be tested against known-answer inputs
# (Tools/tests/test_verdict.sh) without building anything. The verdict is the
# only part of the harness that decides truth, and it has now been wrong twice:
#
#   1. It grepped the runner's output for "Result={Failed}". The engine prints
#      "Result={Fail}", so the test could never match and every mutant scored
#      SURVIVED -- including ones whose own logs said the test had failed.
#
#   2. It treated ANY non-zero exit from run_tests.sh as a kill. An editor that
#      crashes on startup (seen: "Assertion failed: bDirectoryExists" in
#      ShaderCore.cpp, then SIGSEGV) also exits non-zero, so a crash caused by
#      machine load was about to be recorded as "the suite caught this mutant."
#
# Those two defects fail in opposite directions, and the second is the
# dangerous one: a false SURVIVED asks for a test that already exists, while a
# false killed retires a check that was never exercised.
#
# So a kill is not "the runner was unhappy." It is the specific, positive
# observation that TESTS RAN AND AT LEAST ONE OF THEM FAILED. Anything else --
# a crash, a dead editor, a filter that matched nothing -- is UNSCORED, which
# is a refusal to draw a conclusion rather than a conclusion.

# classify_build <build-log> -> succeeded | infrastructure | failed
#
# "Did the build produce a binary, and if not, whose fault was it?"
#
# Separated from the caller's control flow for the same reason as
# classify_test_run: a build that fails for a reason unrelated to the mutant is
# not evidence about the mutant. UnrealBuildTool keeps state in
# ~/Library/Application Support, which is shared by every build on the machine
# regardless of worktree, so a concurrent build can abort an unrelated one
# through no fault of the code being tested. Scoring that would be the same
# error as scoring an engine crash as a kill, one stage earlier.
classify_build() {
    local log="$1"

    [[ -f "${log}" ]] || { echo "infrastructure"; return; }

    # The success line is authoritative and checked FIRST. Build.sh's exit code
    # is unreliable (it exits 0 on failure), and UBT sometimes crashes during
    # teardown after the binary is already linked -- that crash must not
    # discard a build that actually succeeded.
    #
    # BUT "Result: Succeeded" ALONE IS NOT ENOUGH FOR A MUTATION CAMPAIGN.
    # UnrealBuildTool decides what to rebuild from file modification times, and
    # mutate.sh installs each mutant with `cp` from a backup, which can leave an
    # mtime no newer than the object file already on disk. UBT then prints
    #
    #     Target is up to date
    #     Using Unreal Build Accelerator local executor to run 0 action(s)
    #     Result: Succeeded
    #
    # and the tests that follow run against the PREVIOUS, UNMUTATED binary. They
    # pass, and the mutant is recorded SURVIVED -- a demand for a test that
    # already exists and already works. Observed directly: `<=` -> `<` in
    # FitsInBudget scored SURVIVED with 0 actions, and killed with 6 failed
    # assertions once the file was actually recompiled.
    #
    # A build that compiled nothing is not evidence about the mutant, so it is
    # classified `noop` -- neither a pass nor a failure, but a signal to the
    # caller to force the rebuild and try again.
    if grep -aq "Result: Succeeded" "${log}"; then
        if grep -aqE "run 0 action\(s\)" "${log}"; then
            echo "noop"
            return
        fi
        echo "succeeded"
        return
    fi

    # Infrastructure, not code: the UBT lock, and UBT falling over on its own
    # global state. Deliberately narrow -- each pattern names a specific failure
    # in the build tool itself. A broad "Exception" match would swallow genuine
    # compile failures and silently drop mutants from scoring.
    if grep -aq "ConflictingInstance" "${log}" \
        || grep -aq "UnrealBuildTool/Trace.uba" "${log}" \
        || grep -aqE "Unhandled exception.*(EpicGames|UnrealBuildTool)" "${log}"; then
        echo "infrastructure"
        return
    fi

    echo "failed"
}

# classify_test_run <exit-code> <runner-output-file> -> killed | SURVIVED | UNSCORED
classify_test_run() {
    local exit_code="$1" log="$2"

    [[ -f "${log}" ]] || { echo "UNSCORED"; return; }

    # run_tests.sh prints one "  <Result>  <TestPath>" line per completed test
    # and a "<n>/<m> passed." summary. Both are absent when the editor died
    # before the automation controller reported anything.
    local ran
    ran=$(grep -acE '^[0-9]+/[0-9]+ passed\.' "${log}")

    if [[ "${ran}" -eq 0 ]]; then
        # No summary line: nothing was measured. Covers the crash, the dead
        # editor, and the filter that matched nothing.
        echo "UNSCORED"
        return
    fi

    # A summary exists, so tests genuinely executed. Now the exit code is
    # meaningful, because run_tests.sh only reaches its summary after the
    # engine returned and it counted results.
    if [[ "${exit_code}" -ne 0 ]]; then
        echo "killed"
    else
        echo "SURVIVED"
    fi
}

# =============================================================================
# Baseline: was the tree green BEFORE anything was mutated?
# =============================================================================
#
# classify_test_run above answers "did this arm's suite fail?" correctly, and
# that is not enough, because it cannot see WHEN the failure started. If a test
# matching the campaign's filter was already red, every arm inherits it: the
# summary line is real, the non-zero exit is real, and `killed` is the honest
# answer to the question classify_test_run was asked. The report then shows
# every arm KILLED, which reads as a thoroughly covered file.
#
# That is why this is a separate decision and not a flag on the one above. The
# question here is about the PRISTINE tree, asked once, before any mutant
# exists -- and a campaign that cannot establish it must not score anything.
#
# mutate.sh used to print "waiting for a green baseline before scoring
# anything" while calling only build(). It established that the tree COMPILES.
# The word "green" in that banner was doing work the code never did.

# classify_baseline_run <exit-code> <runner-output-file> -> green | red | UNSCORED
#
# green    the suite ran, everything passed, and something was actually verified
# red      the suite ran and something failed BEFORE any mutant existed
# UNSCORED nothing was measured -- crash, empty filter, or an all-skip filter
#
# UNSCORED and red are both refusals to proceed. They are distinguished because
# they need different messages: red names tests to fix, UNSCORED means the
# machine or the filter is wrong.
classify_baseline_run() {
    local exit_code="$1" log="$2"

    [[ -f "${log}" ]] || { echo "UNSCORED"; return; }

    # Same summary-line requirement as classify_test_run, for the same reason:
    # no summary means the automation controller never reported, so nothing was
    # measured. Covers the dead editor and the filter that matched nothing --
    # the latter matters here because zero-of-zero passing is vacuously true and
    # would otherwise baseline green, then score SURVIVED on every arm.
    local summary
    summary=$(grep -aoE '^[0-9]+/[0-9]+ passed\.' "${log}" | tail -1)
    if [[ -z "${summary}" ]]; then
        echo "UNSCORED"
        return
    fi

    if [[ "${exit_code}" -ne 0 ]]; then
        echo "red"
        return
    fi

    # A green run in which EVERY test skipped verified nothing. Those tests
    # report Result={Success} to the engine, so the summary reads clean -- the
    # count is accurate and the conclusion drawn from it would not be. Scoring
    # arms against such a filter yields SURVIVED across the board, demanding
    # tests that already exist.
    #
    # Compared against the run's own total rather than tested for the presence
    # of the word: one skipped GPU test among many real ones is ordinary and
    # must still baseline. Only "all of them" is disqualifying.
    local total skipped
    total="${summary%%/*}"
    skipped=$(awk '/^[0-9]+ skipped -- these reported success/ { print $1; exit }' "${log}")
    if [[ -n "${skipped}" && "${skipped}" -ge "${total}" ]]; then
        echo "UNSCORED"
        return
    fi

    echo "green"
}

# baseline_failures <runner-output-file> -> one test path per line
#
# Names for the abort message. "The baseline was not green" gives the reader
# nothing to act on and, worse, does not let them tell a standing failure from
# one they just introduced -- which is the exact confusion this whole guard
# exists to prevent. ALL failures are listed: reporting only the first invites
# a fix-and-retry loop that meets a second abort each time.
# Reads run_tests.sh's OWN per-test summary lines -- "  <Result>  <TestPath>",
# two-space indented -- rather than the engine's "Path={...}" records. Both
# appear in a real log and they are not equivalent: the engine emits Path={} in
# the failure-details block too, and on Started lines, so keying on it reports
# tests that did not fail. The indented summary is one line per completed test
# and is the format run_tests.sh is responsible for.
baseline_failures() {
    local log="$1"
    [[ -f "${log}" ]] || return 0
    awk '/^  Fail[[:space:]]+/ { print $2 }' "${log}" | sort -u
}

# =============================================================================
# Foreign edits: is somebody else working in the file we are about to mutate?
# =============================================================================
#
# mutate.sh snapshots the target file at campaign start and every restore is a
# whole-file `cp` from that snapshot. An edit a peer lands in that file between
# an arm and its restore is silently reverted -- no error, no conflict marker.
# Observed live: one campaign mutated FlowVizVolumeRayMarch.usf while another
# agent was landing a shader change in the same file.
#
# The lost work is the visible harm and the smaller one. The verdict is the
# real damage: an arm's evidence is "the suite went red", and a peer's mid-edit
# file -- uncompilable, or half-wired -- goes red identically. A KILLED that was
# really someone else's in-flight edit retires a gap instead of reporting it.
#
# Checked at campaign start only. By arm 2 the file is dirty BY DESIGN, because
# the campaign itself made it so; re-checking later would deadlock the loop.

# check_foreign_edits <porcelain-file> <target-path> -> clean | foreign
#
# porcelain-file holds `git status --porcelain` output. Passed in rather than
# shelled out so this is testable against known-answer inputs.
check_foreign_edits() {
    local porcelain="$1" target="$2"
    if [[ -n "$(foreign_edit_path "${porcelain}" "${target}")" ]]; then
        echo "foreign"
    else
        echo "clean"
    fi
}

# foreign_edit_path <porcelain-file> <target-path> -> the contended path, or ""
#
# Separate from the verdict so the refusal can name the file. Handing the
# reader "the tree is dirty" sends them to `git status` to work out what the
# harness meant; the point of refusing is to give them the reason.
foreign_edit_path() {
    local porcelain="$1" target="$2"
    [[ -f "${porcelain}" ]] || return 0

    # Columns 1-2 are the staged and worktree status; the path starts at 4.
    # BOTH columns count: a staged-but-uncommitted edit is just as clobberable
    # by a whole-file restore as an unstaged one, so keying on the worktree
    # column alone would miss it.
    #
    # Compared as a WHOLE PATH, never as a substring. A substring match would
    # refuse on FlowVizVolumeRayMarch.usf.bak, and that refusal would look
    # exactly as authoritative as a true one.
    awk -v target="${target}" '
        { path = substr($0, 4); if (path == target) { print path; exit } }
    ' "${porcelain}"
}
