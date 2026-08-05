#!/bin/bash
# Known-answer tests for the two things mutate.sh must establish BEFORE it
# scores a single arm: that the suite was green to begin with, and that nobody
# else is editing the file about to be mutated.
#
# Both were missing, and both fail in the direction that looks like success.
#
# --- Defect B: the baseline never ran the suite ------------------------------
#
# mutate.sh printed "=== waiting for a green baseline before scoring anything
# ===" and then called only build(). run_tests.sh appeared at exactly one line
# in the whole script, inside the per-mutant loop. So the baseline established
# "it compiles" while the banner claimed something considerably stronger.
#
# The consequence is a forged report, not a missing one. If any test matching
# the campaign's filter is ALREADY failing, every arm inherits that failure.
# classify_test_run then sees a real "<n>/<m> passed." summary and a real
# non-zero exit and correctly answers `killed` -- the classifier is not wrong,
# tests ran and one failed. The defect is that the failure PREDATES the mutant.
# The campaign reports every arm KILLED, which reads as a thoroughly covered
# file, and nothing in the output looks off.
#
# This was live in this repo: FlowViz.Render.Wiring is a standing expected-red
# (task #26), so any campaign filtered on `FlowViz` or `FlowViz.Render` would
# have scored all-KILLED without testing anything.
#
# --- Defect A: whole-file restore clobbers a peer's concurrent edits ---------
#
# BACKUP is a whole-file snapshot taken at campaign start and every restore is
# `cp "${BACKUP}" "${SRC}"`. An edit a peer lands in that file between arm and
# restore is silently reverted to the start-of-campaign snapshot -- no error,
# no conflict marker. Observed live: one campaign mutated
# FlowVizVolumeRayMarch.usf while another agent was editing the same file.
#
# Again the dangerous direction is the verdict, not the lost work. An arm's
# evidence is "the suite went red". A peer's mid-edit shader -- uncompilable,
# or a half-wired parameter -- goes red identically, and a KILLED that was
# really someone else's in-flight edit retires a gap instead of reporting it.
#
# Both checks therefore live in verdict.sh with the other deciding code, and
# are exercised here against known-answer inputs without building anything.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "${SCRIPT_DIR}/../verdict.sh"

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

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# =============================================================================
# classify_baseline_run -- was the tree green BEFORE anything was mutated?
# =============================================================================

# --- a genuinely green baseline proceeds to scoring --------------------------
#
# THE CONTROL, and it is not a formality: a guard that refuses every baseline
# would satisfy every other case in this file while making the harness useless.
# It is listed first so a "refuse everything" implementation dies immediately.
cat > "${TMP}/green.log" <<'LOG'
Running automation tests matching 'FlowViz.Render.ReasonCodes'...

  Success  FlowViz.Render.ReasonCodes
  Success  FlowViz.Render.ReasonChannel

2/2 passed. Full log: /tmp/flowviz_tests_0dfae8c5.log
LOG
check "a green pristine suite is a usable baseline" "green" \
    "$(classify_baseline_run 0 "${TMP}/green.log")"

# --- the live case: a standing red poisons every arm -------------------------
#
# Real captured output. FlowViz.Render.Wiring is task #26's expected red. A
# campaign filtered this broadly would score every arm KILLED on this failure.
cat > "${TMP}/prered.log" <<'LOG'
Running automation tests matching 'FlowViz.Render'...

  Success  FlowViz.Render.RayMarchShader
  Success  FlowViz.Render.ReasonCodes
  Fail     FlowViz.Render.Wiring

Failure details:
  LogAutomationController: Error: Test Completed. Result={Fail} Name={Wiring} Path={FlowViz.Render.Wiring}

2/3 passed. Full log: /tmp/flowviz_tests_0dfae8c5.log
LOG
check "a suite already red is NOT a baseline" "red" \
    "$(classify_baseline_run 1 "${TMP}/prered.log")"

# --- the refusal must NAME the failing tests ---------------------------------
#
# Anti-vacuity. "baseline not green" tells the reader nothing they can act on,
# and specifically does not let them distinguish a pre-existing red from a
# problem they just introduced. The name is what makes the abort diagnosable,
# so it is asserted rather than assumed.
check "the refusal names the already-failing test" "FlowViz.Render.Wiring" \
    "$(baseline_failures "${TMP}/prered.log")"

# --- more than one pre-existing failure is fully reported --------------------
#
# Reporting only the first would let a reader fix one test, re-run, and meet a
# second abort they thought they had already dealt with.
cat > "${TMP}/tworeds.log" <<'LOG'
Running automation tests matching 'FlowViz'...

  Fail     FlowViz.Render.Wiring
  Fail     FlowViz.Scene.VolumeComponent

50/52 passed. Full log: /tmp/x.log
LOG
check "every pre-existing failure is named, not just the first" \
    "FlowViz.Render.Wiring FlowViz.Scene.VolumeComponent" \
    "$(baseline_failures "${TMP}/tworeds.log" | tr '\n' ' ' | sed 's/ *$//')"

# --- a baseline that never ran is not a green one ----------------------------
#
# The same distinction classify_test_run draws. An editor that died before the
# automation controller reported anything produces no summary line. Treating
# that as green would start a campaign on a machine that cannot run tests.
cat > "${TMP}/crashed.log" <<'LOG'
Running automation tests matching 'FlowViz.Render'...
Assertion failed: bDirectoryExists [File:ShaderCore.cpp]
LOG
check "a baseline run that produced no results is UNSCORED, not green" "UNSCORED" \
    "$(classify_baseline_run 1 "${TMP}/crashed.log")"

# --- a filter matching nothing is not a green baseline -----------------------
#
# run_tests.sh exits 4 with no summary line here. Zero tests passing out of
# zero is vacuously true and would otherwise start a campaign whose every arm
# runs no tests at all -- SURVIVED across the board, demanding tests that
# already exist.
cat > "${TMP}/nomatch.log" <<'LOG'
Running automation tests matching 'FlowViz.Render.Typo'...

FAIL: no tests matched 'FlowViz.Render.Typo'. Check the test path.
LOG
check "a filter that matched no tests is not a baseline" "UNSCORED" \
    "$(classify_baseline_run 4 "${TMP}/nomatch.log")"

# --- a green run whose tests all SKIPPED is not a baseline -------------------
#
# A test that skips itself still reports Success, so the summary reads clean.
# Scoring arms against a filter whose tests all skip yields SURVIVED for
# everything, for the same reason as the empty filter above.
cat > "${TMP}/skipped.log" <<'LOG'
Running automation tests matching 'FlowViz.Render.VolumeDevice'...

  Success  FlowViz.Render.VolumeDevice

1 skipped -- these reported success having verified nothing:
  FlowViz.Render.VolumeDevice
  (reasons are in the log; GPU tests need RHI=1)

1/1 passed. Full log: /tmp/x.log
LOG
check "a baseline whose tests all skipped verified nothing" "UNSCORED" \
    "$(classify_baseline_run 0 "${TMP}/skipped.log")"

# --- ...but a skip alongside real work is still a usable baseline ------------
#
# The control for the case above. Refusing any run containing the word SKIPPED
# would pass that check while blocking legitimate campaigns, since one skipped
# GPU test among many real ones is normal.
cat > "${TMP}/partial_skip.log" <<'LOG'
Running automation tests matching 'FlowViz.Render'...

  Success  FlowViz.Render.ReasonCodes
  Success  FlowViz.Render.VolumeDevice

1 skipped -- these reported success having verified nothing:
  FlowViz.Render.VolumeDevice
  (reasons are in the log; GPU tests need RHI=1)

2/2 passed. Full log: /tmp/x.log
LOG
check "a skip beside genuinely-run tests still baselines" "green" \
    "$(classify_baseline_run 0 "${TMP}/partial_skip.log")"

# =============================================================================
# check_foreign_edits -- is somebody else editing the file we are about to
# mutate?
# =============================================================================

# --- clean file, campaign proceeds -------------------------------------------
#
# The control. A guard that refuses every tree is not a guard, and this is the
# ordinary case: peers have unrelated files dirty all the time.
cat > "${TMP}/porcelain_clean.txt" <<'LOG'
?? FlowViz/Plugins/FlowVizRuntime/Source/FlowVizRuntime/Public/UI/
 M FlowViz/Docs/BACKLOG.md
LOG
check "an unrelated dirty file does not block a campaign" "clean" \
    "$(check_foreign_edits "${TMP}/porcelain_clean.txt" \
        "FlowViz/Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf")"

# --- the observed collision: the target file is dirty ------------------------
#
# The real event. A peer was mid-edit in FlowVizVolumeRayMarch.usf when a
# campaign took its backup of that same file.
cat > "${TMP}/porcelain_dirty.txt" <<'LOG'
 M FlowViz/Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf
 M FlowViz/Docs/BACKLOG.md
LOG
check "a peer's uncommitted edit in the target file blocks the campaign" "foreign" \
    "$(check_foreign_edits "${TMP}/porcelain_dirty.txt" \
        "FlowViz/Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf")"

# --- a path that merely CONTAINS the target name does not block --------------
#
# Anti-vacuity for the check above. A substring match would refuse on an
# unrelated file whose name embeds the target's, and the refusal would look
# exactly as authoritative as a true one.
cat > "${TMP}/porcelain_substr.txt" <<'LOG'
 M FlowViz/Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf.bak
 M FlowViz/Docs/OldFlowVizVolumeRayMarch.usf
LOG
check "a similarly-named file is not the target" "clean" \
    "$(check_foreign_edits "${TMP}/porcelain_substr.txt" \
        "FlowViz/Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf")"

# --- staged edits count too --------------------------------------------------
#
# Porcelain puts the staged flag in column 1 and the worktree flag in column 2.
# A staged-but-uncommitted edit is just as clobberable by a whole-file restore,
# so keying on column 2 alone would miss it.
cat > "${TMP}/porcelain_staged.txt" <<'LOG'
M  FlowViz/Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf
LOG
check "a STAGED edit in the target file also blocks" "foreign" \
    "$(check_foreign_edits "${TMP}/porcelain_staged.txt" \
        "FlowViz/Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf")"

# --- the refusal names the file ----------------------------------------------
#
# "the tree is dirty" sends the reader to `git status` to work out what the
# harness meant. The point of refusing is to hand them the reason.
check "the refusal identifies which file is contended" \
    "FlowViz/Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf" \
    "$(foreign_edit_path "${TMP}/porcelain_dirty.txt" \
        "FlowViz/Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf")"

echo
echo "baseline-guard tests: ${PASS} passed, ${FAIL} failed"
[[ "${FAIL}" -eq 0 ]]
