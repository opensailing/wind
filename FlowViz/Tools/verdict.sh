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
    if grep -aq "Result: Succeeded" "${log}"; then
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
