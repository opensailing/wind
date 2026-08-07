#!/usr/bin/env bash
#
# Tests for the summary that Tools/run_tests.sh prints.
#
# Run: FlowViz/Tools/tests/test_run_tests_summary.sh
#
# Why this file exists
# --------------------
# `run_tests.sh FlowViz` printed "47/47 passed" on a run in which three tests
# had skipped themselves for want of a GPU, having verified nothing at all. The
# engine reports a self-skipped test as Result={Success}, so the count was
# accurate and the sentence was still misleading: it is the shape of a green
# number standing in front of work that never happened.
#
# The skip messages themselves were honest -- each names what went unverified
# and how to re-run it. They were just buried in a 2000-line log that nobody
# reads when the last line says everything passed.
#
# What is actually under test
# ---------------------------
# `run_tests.sh --summarize <log>` reads a log and prints the summary. That
# seam exists so these tests can drive the reporting logic over fixture logs
# instead of launching the editor, which takes ~40s and needs an engine.
#
# The load-bearing part is the CONTROL at the bottom. A skip-counter that
# always reported 0 would pass every "no skips here" assertion in this file.
# So the fixtures come in pairs -- same log, skip lines present and absent --
# and the test asserts the two produce DIFFERENT output. A counter that cannot
# distinguish them fails, whatever number it happens to print.
#
# See the repo memory note verify-metrics-can-fail.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_TESTS="${HERE}/../run_tests.sh"

failures=0
checks=0

check() {
    local name="$1" expected="$2" actual="$3"
    checks=$((checks + 1))
    if [[ "${expected}" == "${actual}" ]]; then
        echo "  ok    ${name}"
    else
        failures=$((failures + 1))
        echo "  FAIL  ${name}"
        echo "        expected: ${expected}"
        echo "        actual:   ${actual}"
    fi
}

check_contains() {
    local name="$1" needle="$2" haystack="$3"
    checks=$((checks + 1))
    if [[ "${haystack}" == *"${needle}"* ]]; then
        echo "  ok    ${name}"
    else
        failures=$((failures + 1))
        echo "  FAIL  ${name}"
        echo "        expected to contain: ${needle}"
        echo "        actual:              ${haystack}"
    fi
}

check_lacks() {
    local name="$1" needle="$2" haystack="$3"
    checks=$((checks + 1))
    if [[ "${haystack}" != *"${needle}"* ]]; then
        echo "  ok    ${name}"
    else
        failures=$((failures + 1))
        echo "  FAIL  ${name}"
        echo "        expected NOT to contain: ${needle}"
        echo "        actual:                  ${haystack}"
    fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# --- fixtures ----------------------------------------------------------------
#
# Real log lines, copied from an actual run. The engine's format matters here:
# a self-skipped test still reports Result={Success}, and the SKIPPED marker
# lands AFTER that line, before the next test starts. Attribution therefore
# has to track the most recent "Test Started", not the nearest Completed.

started() {
    printf '[2026.08.05-10.08.28:276][711]LogAutomationController: Display: Test Started. Name={%s} Path={%s}\n' \
        "${1##*.}" "$1"
}
completed() {
    printf '[2026.08.05-10.08.28:300][714]LogAutomationController: Display: Test Completed. Result={%s} Name={%s} Path={%s}\n' \
        "$2" "${1##*.}" "$1"
}
skip_marker() {
    printf '[2026.08.05-10.08.28:300][714]LogAutomationController: Warning: SKIPPED: %s needs a real RHI device and this run has none (-nullrhi). NOTHING was verified.\n' \
        "$1"
}

# WITH skips: two ordinary passes and two tests that skipped themselves.
WITH="${WORK}/with_skips.log"
{
    started    FlowViz.CFDViz.Crc32C
    completed  FlowViz.CFDViz.Crc32C Success
    started    FlowViz.Render.RayMarchShaderDevice
    completed  FlowViz.Render.RayMarchShaderDevice Success
    skip_marker FlowViz.Render.RayMarchShaderDevice
    started    FlowViz.Render.VolumeTexture
    completed  FlowViz.Render.VolumeTexture Success
    started    FlowViz.Render.VolumeDevice
    completed  FlowViz.Render.VolumeDevice Success
    skip_marker FlowViz.Render.VolumeDevice
} > "${WITH}"

# WITHOUT skips: byte-for-byte the same log, minus the two SKIPPED lines. This
# is the same four tests all genuinely passing -- what an RHI=1 run looks like.
WITHOUT="${WORK}/without_skips.log"
grep -v "SKIPPED" "${WITH}" > "${WITHOUT}"

echo "run_tests.sh --summarize:"

# --- the counts themselves ---------------------------------------------------

with_out="$("${RUN_TESTS}" --summarize "${WITH}" 2>&1)"
without_out="$("${RUN_TESTS}" --summarize "${WITHOUT}" 2>&1)"

# The summary also prints a line per test that ran, so every test's name
# appears in the full output regardless. The attribution assertions below must
# look at the SKIP REPORT only, or they would pass on the listing alone.
#
# The report is the "N skipped" header plus the indented names beneath it,
# which do NOT themselves contain the word "skip" -- so take everything from
# the header to the blank line that ends the block.
with_skips="$(printf '%s\n' "${with_out}" | awk '/skipped --/{f=1} f&&NF==0{f=0} f')"

check_contains "a log with skips reports how many skipped" \
    "2 skipped" "${with_out}"

check_contains "the pass count still counts every test the engine ran" \
    "4/4 passed" "${with_out}"

# Naming them is the point. "2 skipped" tells you something was not verified;
# it does not tell you WHICH GPU path is unproven, and that is the fact a
# reader needs to decide whether the green line covers what they care about.
check_contains "each skipped test is named in the skip report" \
    "FlowViz.Render.RayMarchShaderDevice" "${with_skips}"
check_contains "including the second one" \
    "FlowViz.Render.VolumeDevice" "${with_skips}"

# Attribution has to be right, not just the count. The skip marker for
# RayMarchShaderDevice sits after its Completed line, adjacent to the next
# test's Started line -- an off-by-one in either direction blames a test that
# ran perfectly well.
check_lacks "a test that did not skip is not named as skipped" \
    "FlowViz.CFDViz.Crc32C" "${with_skips}"
check_lacks "nor is the test whose Started line follows a skip marker" \
    "FlowViz.Render.VolumeTexture" "${with_skips}"

# --- a clean run says so, and says it differently -----------------------------

check_lacks "a log with no skips does not mention skipping" \
    "skipped" "${without_out}"

check_contains "and still reports the passes" \
    "4/4 passed" "${without_out}"

# --- THE CONTROL -------------------------------------------------------------
#
# Everything above is satisfiable by a summarizer that guesses. The two
# fixtures differ ONLY in the presence of the SKIPPED lines, so if the
# summarizer's output is identical across them it is not reading those lines
# at all, and every assertion above is an accident. This check is what makes
# the rest of the file evidence.

checks=$((checks + 1))
if [[ "${with_out}" != "${without_out}" ]]; then
    echo "  ok    CONTROL: the summary distinguishes a skipped run from a clean one"
else
    failures=$((failures + 1))
    echo "  FAIL  CONTROL: identical summary for logs that differ only in SKIPPED lines"
    echo "        The skip check is not reading the log. Every assertion above is vacuous."
fi

# --- a truncated session must not report a full green -------------------------
#
# THE DEFECT THIS SECTION EXISTS FOR, observed 2026-08-06. The engine crashed
# on the render thread partway through a run. 51 tests had completed, all of
# them passing; the remaining 55 never started. run_tests.sh printed
#
#     51/51 passed
#
# and exited 0, because FOUND and PASSED were both grep counts over Completed
# lines in the SAME log. The denominator moves with the numerator, so a session
# that dies at test 51 is arithmetically indistinguishable from a suite of 51.
#
# The engine already logs the answer before running anything:
#
#     LogAutomationCommandLine: Display: Found 106 automation tests based on 'FlowViz'
#
# followed by one indented line per test name. That is the engine's own filter
# result -- reading it is not a reimplementation of its matching rules, which
# is what makes it trustworthy as a denominator.

enumerated() {
    printf "[2026.08.06-06.34.43:037][407]LogAutomationCommandLine: Display: Found %s automation tests based on '%s'\n" \
        "$1" "$2"
    shift 2
    for name in "$@"; do
        printf '[2026.08.06-06.34.43:038][407]LogAutomationCommandLine: Display: \t%s\n' "${name}"
    done
}

# Four enumerated, two completed: the session died after Crc32C.
TRUNCATED="${WORK}/truncated.log"
{
    enumerated 4 FlowViz \
        FlowViz.CFDViz.ColorMaps FlowViz.CFDViz.Crc32C \
        FlowViz.Render.VolumeTexture FlowViz.UI.Workspace.Bind
    started    FlowViz.CFDViz.ColorMaps
    completed  FlowViz.CFDViz.ColorMaps Success
    started    FlowViz.CFDViz.Crc32C
    completed  FlowViz.CFDViz.Crc32C Success
} > "${TRUNCATED}"

# The SAME four tests, all of which ran. Byte-identical enumeration block.
COMPLETE="${WORK}/complete.log"
{
    enumerated 4 FlowViz \
        FlowViz.CFDViz.ColorMaps FlowViz.CFDViz.Crc32C \
        FlowViz.Render.VolumeTexture FlowViz.UI.Workspace.Bind
    started    FlowViz.CFDViz.ColorMaps
    completed  FlowViz.CFDViz.ColorMaps Success
    started    FlowViz.CFDViz.Crc32C
    completed  FlowViz.CFDViz.Crc32C Success
    started    FlowViz.Render.VolumeTexture
    completed  FlowViz.Render.VolumeTexture Success
    started    FlowViz.UI.Workspace.Bind
    completed  FlowViz.UI.Workspace.Bind Success
} > "${COMPLETE}"

trunc_out="$("${RUN_TESTS}" --summarize "${TRUNCATED}" 2>&1)"
trunc_rc=$?
complete_out="$("${RUN_TESTS}" --summarize "${COMPLETE}" 2>&1)"
complete_rc=$?

check "a truncated run exits non-zero" "1" "${trunc_rc}"
check "a complete run still exits 0"   "0" "${complete_rc}"

# The count must be against what the ENGINE found, not against what ran.
# "2/2 passed" is the exact sentence the defect printed.
check_contains "the denominator is the enumerated total, not the completed count" \
    "2/4" "${trunc_out}"
check_lacks "and never restates the completed count as the total" \
    "2/2" "${trunc_out}"

# Naming them, for the same reason skips are named: a bare "2 of 4" does not
# say whether a tail was cut or a subtree was skipped, and those have
# different causes. The names are what tell you which.
check_contains "a test that never ran is named" \
    "FlowViz.Render.VolumeTexture" "${trunc_out}"
check_contains "including the last one" \
    "FlowViz.UI.Workspace.Bind" "${trunc_out}"
check_lacks "a test that DID run is not named as missing" \
    "FlowViz.CFDViz.Crc32C" "$(printf '%s\n' "${trunc_out}" | awk '/never ran/{f=1} f&&NF==0{f=0} f')"

# A complete run must stay quiet. A "did not run" report that fires on every
# run would be turned off within a day, which is how the skip report nearly
# died too.
check_lacks "a complete run reports nothing missing" \
    "never ran" "${complete_out}"
check_contains "and reports the full count" \
    "4/4 passed" "${complete_out}"

# --- THE CONTROL for the truncation check ------------------------------------
#
# Every assertion above is satisfied by a summarizer that never reads the
# enumeration at all and simply complains whenever it sees fewer than four
# Completed lines.
#
# Comparing trunc_out against complete_out would NOT catch that: those two
# fixtures also differ in their test records, so their summaries differ either
# way. That comparison passes vacuously -- it is the shape of a pass criterion
# that cannot fail (repo memory: verify-metrics-can-fail).
#
# So the control varies ONLY the thing under test. Same two completed tests,
# same everything, with the enumeration block deleted. A summarizer reading it
# must say something different about these two logs; one that ignores it
# cannot tell them apart.
TRUNC_NO_ENUM="${WORK}/truncated_no_enumeration.log"
grep -v "automation tests based on\|Display: 	FlowViz" "${TRUNCATED}" > "${TRUNC_NO_ENUM}"

trunc_no_enum_out="$("${RUN_TESTS}" --summarize "${TRUNC_NO_ENUM}" 2>&1)"

checks=$((checks + 1))
if [[ "${trunc_out}" != "${trunc_no_enum_out}" ]]; then
    echo "  ok    CONTROL: the enumeration block is what changes the verdict"
else
    failures=$((failures + 1))
    echo "  FAIL  CONTROL: identical summary with and without the enumeration block"
    echo "        The enumerated total is not being read. Every truncation assertion above"
    echo "        is an accident of the fixture having exactly two completed tests."
fi

# The second control, for the opposite error. A summarizer that treats the
# enumeration line as the denominator ALWAYS would report "0/4" for a log that
# has no enumeration at all -- every pre-2026-08-06 log, and every log from
# `--summarize` over a fixture that predates this section. Those must keep
# working off the completed count.
NO_ENUM="${WORK}/no_enumeration.log"
{
    started    FlowViz.CFDViz.Crc32C
    completed  FlowViz.CFDViz.Crc32C Success
} > "${NO_ENUM}"

no_enum_out="$("${RUN_TESTS}" --summarize "${NO_ENUM}" 2>&1)"
no_enum_rc=$?
check "a log with no enumeration line falls back to the completed count" \
    "1/1" "$(printf '%s\n' "${no_enum_out}" | grep -oE '[0-9]+/[0-9]+' | head -1)"
check "and does not fail for want of a denominator" "0" "${no_enum_rc}"

# --- exit status -------------------------------------------------------------
#
# A skip is not a failure. It must not turn the build red, or the useful
# signal gets suppressed the first time someone runs the suite without a GPU.

"${RUN_TESTS}" --summarize "${WITH}" >/dev/null 2>&1
check "a skipped test does not make the summary exit nonzero" "0" "$?"

FAILING="${WORK}/failing.log"
{
    started    FlowViz.CFDViz.Crc32C
    completed  FlowViz.CFDViz.Crc32C Fail
    started    FlowViz.Render.VolumeDevice
    completed  FlowViz.Render.VolumeDevice Success
    skip_marker FlowViz.Render.VolumeDevice
} > "${FAILING}"

fail_out="$("${RUN_TESTS}" --summarize "${FAILING}" 2>&1)"
fail_status=$?
check "a real failure still exits nonzero even when something also skipped" \
    "1" "${fail_status}"
check_contains "and the skip is still reported alongside it" \
    "1 skipped" "${fail_out}"

# An empty log must not read as a clean sweep -- the existing guard for
# "no tests matched" is load-bearing and the new code path must not bypass it.
: > "${WORK}/empty.log"
"${RUN_TESTS}" --summarize "${WORK}/empty.log" >/dev/null 2>&1
check "an empty log is an error, not a pass" "4" "$?"

# --- a crash is not a filter typo --------------------------------------------
#
# THE DEFECT THIS SECTION EXISTS FOR, observed 2026-08-06 while re-running an
# UNSCORED mutant. A ClockSeam arm removed FTSTicker::RemoveTicker from the
# workspace destructor, so the handler kept ticking a freed model. The engine
# took a SIGSEGV inside FFlowVizCasePlayer::Tick, between the test's Started
# line and its Completed line. run_tests.sh printed:
#
#     FAIL: no tests matched 'FlowViz.UI.Workspace.ClockSeam'. Check the test path.
#
# The filter was correct. The test had STARTED. The message sent a reader off to
# audit a test path that was never wrong, while the actual evidence -- a null
# dereference with a full stack -- sat unmentioned in the same log.
#
# The crash detection added by the truncation work above already exists, at the
# "FOUND BUT NEVER RAN" block. It is UNREACHABLE here: that block runs after the
# RAN==0 early exit, and a session that dies before its first test completes has
# RAN==0 by construction. So the earlier fix closed the branch where SOME tests
# completed and left the branch where NONE did (repo memory:
# audit-the-second-branch-of-every-documented-hazard).
#
# Three states currently produce byte-identical output and exit 4:
#
#     the filter genuinely matched nothing   (the message is correct)
#     the engine crashed during a test       (the message is a lie)
#     the engine died before any test ran    (the message is a lie)
#
# Only the first should say "check the test path".

critical_error() {
    printf '[2026.08.06-15.08.23:619][594]LogMac: === Critical error: ===\n'
    printf 'SIGSEGV: invalid attempt to access memory at address 0x0\n'
    printf '\n'
    printf '[2026.08.06-15.08.23:619][594]LogMac: 0x51f51d8c libUnrealEditor-FlowVizRuntime.dylib!FFlowVizCasePlayer::Tick(double)   [UnknownFile])\n'
    printf '0x06169bc0 libUnrealEditor-Core.dylib!FTSTicker::Tick(float)   [UnknownFile])\n'
}

# The real shape: enumerated, started, then died. No Completed line at all.
#
# NAMED WITHOUT THE WORD "crash", deliberately. The summary echoes "Full log:
# <path>", so a fixture called crashed_mid_test.log makes every assertion
# looking for "crash" pass on the FILENAME. The first red run of this section
# did exactly that -- "says the engine crashed" went green against
# "FAIL: no tests matched" because the path was in the same output. A search
# pattern is a pass criterion (repo memory: a-search-pattern-is-a-pass-criterion).
CRASHED="${WORK}/died_mid_test.log"
{
    enumerated 1 FlowViz.UI.Workspace.ClockSeam FlowViz.UI.Workspace.ClockSeam
    started    FlowViz.UI.Workspace.ClockSeam
    critical_error
} > "${CRASHED}"

# A genuine typo: the engine enumerated nothing and nothing ever started.
# This is the control that keeps the fix honest -- a "fix" that simply deletes
# the misleading sentence would pass every assertion above it and fail here.
NO_MATCH="${WORK}/no_match.log"
{
    printf "[2026.08.06-06.34.43:037][407]LogAutomationCommandLine: Display: Found 0 automation tests based on 'FlowViz.Typo.NotAThing'\n"
} > "${NO_MATCH}"

crashed_out="$("${RUN_TESTS}" --summarize "${CRASHED}" 2>&1)"
crashed_rc=$?
no_match_out="$("${RUN_TESTS}" --summarize "${NO_MATCH}" 2>&1)"
no_match_rc=$?

check_lacks "a crashed session is NOT reported as an unmatched filter" \
    "no tests matched" "${crashed_out}"
check_lacks "and does not tell the reader to go check a correct test path" \
    "Check the test path" "${crashed_out}"

# What it must say instead. The stack is the whole reason the log is worth
# opening, and naming the test says WHERE it died -- the two facts that turn
# "something went wrong" into a diagnosis.
check_contains "a crashed session says the engine crashed" \
    "crash" "$(printf '%s' "${crashed_out}" | tr 'A-Z' 'a-z')"
check_contains "and shows the signal" \
    "SIGSEGV" "${crashed_out}"
check_contains "and names the test that was running when it died" \
    "FlowViz.UI.Workspace.ClockSeam" "${crashed_out}"

# A crash is not a pass. It must stay non-zero, and it must NOT reuse exit 4 --
# mutate.sh classifies on the output, but a distinct code lets any caller tell
# an unrunnable machine from a wrong filter without parsing prose.
checks=$((checks + 1))
if [[ "${crashed_rc}" -ne 0 && "${crashed_rc}" -ne 4 ]]; then
    echo "  ok    a crash exits non-zero with a code distinct from the filter error"
else
    failures=$((failures + 1))
    echo "  FAIL  a crash exits non-zero with a code distinct from the filter error"
    echo "        expected: not 0 and not 4"
    echo "        actual:   ${crashed_rc}"
fi

# THE CONTROL. A genuine empty match must be unchanged -- same message, same
# exit 4. Without this, deleting the sentence outright passes everything above.
check_contains "CONTROL: a genuinely unmatched filter still says so" \
    "no tests matched" "${no_match_out}"
check_contains "CONTROL: and still points at the test path as the thing to check" \
    "Check the test path" "${no_match_out}"
check "CONTROL: and still exits 4" "4" "${no_match_rc}"

# THE SECOND CONTROL, for the vacuous-pass direction. Everything above is
# satisfiable by a summarizer that ignores the log and keys off, say, whether
# the file has more than one line. These two fixtures both have RAN==0; they
# differ in whether a crash occurred. If the summary cannot distinguish them,
# it is not reading the crash and the assertions above are accidents.
#
# COMPARED WITH THE "Full log:" LINE STRIPPED. The two fixtures live at
# different paths, which the summary echoes, so the raw outputs differ no matter
# what the summarizer does -- a comparison that cannot fail is not a control
# (repo memory: verify-metrics-can-fail). Removing the one line that is
# guaranteed to differ leaves only the diagnosis to compare.
strip_log_path() { printf '%s\n' "$1" | grep -v "^Full log:"; }

checks=$((checks + 1))
if [[ "$(strip_log_path "${crashed_out}")" != "$(strip_log_path "${no_match_out}")" ]]; then
    echo "  ok    CONTROL: the summary distinguishes a crash from an empty filter match"
else
    failures=$((failures + 1))
    echo "  FAIL  CONTROL: identical summary for a crashed run and an unmatched filter"
    echo "        Both have zero completed tests. If the output is the same, the crash"
    echo "        is not being read and every assertion above is vacuous."
fi

# A session that died BEFORE any test started is the third state. It is not a
# filter typo either: the engine enumerated four tests, so the filter matched.
DIED_EARLY="${WORK}/died_before_starting.log"
{
    enumerated 4 FlowViz \
        FlowViz.CFDViz.ColorMaps FlowViz.CFDViz.Crc32C \
        FlowViz.Render.VolumeTexture FlowViz.UI.Workspace.Bind
    critical_error
} > "${DIED_EARLY}"

died_early_out="$("${RUN_TESTS}" --summarize "${DIED_EARLY}" 2>&1)"

check_lacks "a session that died before its first test is not a filter error either" \
    "no tests matched" "${died_early_out}"

# "4" alone would match a timestamp, a path fragment, or the enumeration line
# echoed back -- the count has to be attached to a claim about what ran.
check_contains "and it reports none-of-N ran rather than none-found" \
    "0/4" "${died_early_out}"

# --- the default log path must not be shared between checkouts ---------------
#
# LOG defaulted to a fixed /tmp/flowviz_tests.log. Nine worktrees exist to keep
# concurrent agents from corrupting each other's verdicts, and every one of them
# wrote its detail log to that single path -- so the isolation the worktrees buy
# was given straight back at the last step.
#
# Hit 2026-08-05: a run finished at 14:33:23 and the log at that path was dated
# 14:33:30, containing a different run's editor startup and ZERO test records.
# The console summary said 49/50 and the file said nothing, because a peer had
# overwritten it between the run and the read.
#
# The danger is not the confusion, it is the direction of the error: an
# unrelated log at a known-good path reads as evidence about YOUR run. Someone
# reconciling a suspicious total against it gets a confident wrong answer. Same
# family as the stale-dylib trap -- a real file, truthfully read, about the
# wrong run.
#
# So the default must be per-checkout. Asserted by running two copies of the
# script from different fake project directories and requiring the paths differ.

default_log_for() {
    # Ask the script itself where it would write, with no LOG set, without
    # launching an engine. Printed by the script rather than recomputed here:
    # a duplicated formula would pass while the real default stayed shared.
    ( unset LOG; "$1" --print-log-path 2>/dev/null )
}

wt_a="${WORK}/checkout-a/Tools"
wt_b="${WORK}/checkout-b/Tools"
mkdir -p "${wt_a}" "${wt_b}"
cp "${RUN_TESTS}" "${wt_a}/run_tests.sh"
cp "${RUN_TESTS}" "${wt_b}/run_tests.sh"

path_a="$(default_log_for "${wt_a}/run_tests.sh")"
path_b="$(default_log_for "${wt_b}/run_tests.sh")"

if [[ -z "${path_a}" || -z "${path_b}" ]]; then
    check "run_tests.sh reports its default log path (--print-log-path)" \
        "non-empty" "empty"
else
    if [[ "${path_a}" == "${path_b}" ]]; then
        shared="shared"
    else
        shared="distinct"
    fi
    check "two checkouts get DIFFERENT default log paths" "distinct" "${shared}"
    [[ "${shared}" == "shared" ]] && echo "        both resolved to: ${path_a}"

    # The control. "Distinct" is also satisfied by a random path per INVOCATION,
    # which would break --summarize and lose the log between run and read. The
    # same checkout must resolve to the same place twice.
    path_a2="$(default_log_for "${wt_a}/run_tests.sh")"
    if [[ "${path_a}" == "${path_a2}" ]]; then
        stability="stable"
    else
        stability="varies per run"
    fi
    check "but one checkout resolves to the SAME path every time" \
        "stable" "${stability}"

    # An explicit LOG must still win, or the mutation harness and every caller
    # that pins a path silently writes somewhere else.
    explicit="$( LOG="${WORK}/explicit.log" "${wt_a}/run_tests.sh" --print-log-path 2>/dev/null )"
    check "an explicitly set LOG still wins over the default" \
        "${WORK}/explicit.log" "${explicit}"
fi

echo
if [[ "${failures}" -eq 0 ]]; then
    echo "run_tests.sh summary: ${checks}/${checks} checks passed"
    exit 0
fi
echo "run_tests.sh summary: ${failures} of ${checks} checks FAILED"
exit 1
