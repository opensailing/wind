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

echo
if [[ "${failures}" -eq 0 ]]; then
    echo "run_tests.sh summary: ${checks}/${checks} checks passed"
    exit 0
fi
echo "run_tests.sh summary: ${failures} of ${checks} checks FAILED"
exit 1
