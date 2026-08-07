#!/bin/bash
# mutate.sh must push the mutated source's mtime far enough into the future to
# overtake object files already on disk.
#
# --- what happened ------------------------------------------------------------
#
# Observed live 2026-08-06. The clip-seam dispatcher campaign never scored a
# single arm. It sat at "waiting for a clean build" for 25+ minutes while the
# build log cycled every ~40 seconds: grow, print "Result: Succeeded", truncate,
# repeat. The successful build said:
#
#     Using Unreal Build Accelerator local executor to run 0 action(s)
#     Result: Succeeded
#
# classify_build calls that `noop`, correctly: UnrealBuildTool rebuilds on
# MODIFICATION TIME, the source was older than its objects, nothing recompiled,
# and the binary therefore does not contain the mutant. build() responds by
# forcing the source newer and retrying:
#
#     noop)  touch -A 01 "${SRC}"; sleep 1; continue ;;
#
# That line does not do what it reads as. From `man touch`:
#
#     touch [-A [-][[hh]mm]SS]
#
# -A parses its argument from the RIGHT as seconds, so `01` is ONE SECOND, not
# one hour. Measured on this machine:
#
#     -A 01      ->  +1 second
#     -A 0100    ->  +60 seconds
#     -A 010000  ->  +3600 seconds
#
# The observed source was 1683 seconds behind the objects. At +1s per ~40s
# iteration that is roughly 19 hours to catch up -- an infinite loop in
# practice, ending in an ABORT of a tree that compiles perfectly.
#
# --- the failure direction that matters ---------------------------------------
#
# The hang is the loud half. The quiet half is the second site, at the end of
# build(), which runs after every SUCCESSFUL mutant build so the NEXT arm's
# source is newer than the objects just produced. At +1 second, any build taking
# longer than a second leaves the source older than its own objects, so the next
# arm compiles nothing and is tested against the PREVIOUS arm's binary. That
# scores SURVIVED -- a demand for a test that already exists and already works.
#
# `noop` is the guard that normally catches a stale binary, and this bug is
# precisely in that guard's remedy. Both sites used the same literal, so nothing
# in the code looked inconsistent.
#
# --- why the obvious assertion would not have caught it ------------------------
#
# "The mtime advanced" is TRUE OF THE BUG: +1 second is an advance. Any test
# asserting only that direction passes against +1s and against +1h identically,
# and would have shipped this. The assertions below are about MAGNITUDE, with a
# threshold tied to the thing being overtaken -- an object file written while the
# build ran.
#
# (cf. verify-metrics-can-fail, and a-control-needs-a-known-nonzero-expectation.)

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

echo "mutate.sh source mtime advance:"

WORK="$(mktemp -d -t mtime_advance)"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

# How far ahead of "now" a touched file lands, in seconds. Negative means the
# past. Uses the same stat the harness would; python does the arithmetic so the
# result is a number rather than a formatted date.
advance_of() {
    python3 - "$1" <<'PY'
import os, sys, time
print(int(round(os.path.getmtime(sys.argv[1]) - time.time())))
PY
}

# --- THE PLATFORM FACT, MEASURED RATHER THAN ASSUMED --------------------------
#
# This is the part that was got wrong by reading. Pinning it here means a
# platform whose touch parses -A differently fails loudly in this file rather
# than silently in a campaign at 3am.
PROBE="${WORK}/probe"

touch "${PROBE}"; touch -A 01 "${PROBE}"
ONE="$(advance_of "${PROBE}")"
check "PLATFORM: -A 01 is one SECOND, not one hour" "yes" \
    "$([[ "${ONE}" -ge 0 && "${ONE}" -le 2 ]] && echo yes || echo no)"

touch "${PROBE}"; touch -A 010000 "${PROBE}"
HOUR="$(advance_of "${PROBE}")"
check "PLATFORM: -A 010000 is one hour" "yes" \
    "$([[ "${HOUR}" -ge 3590 && "${HOUR}" -le 3610 ]] && echo yes || echo no)"

# --- THE REGRESSION: no site may use the one-second form ----------------------
#
# Written against the shipped script rather than a copy, because the defect was
# a literal in the source and that is exactly what must not come back. Both
# occurrences were identical, so a check that tolerated "at least one good site"
# would have passed while the other stayed broken -- hence a count of BAD sites,
# required to be zero.
BAD_SITES="$(grep -c 'touch -A 0\{0,1\}1 ' "${MUTATE}" 2>/dev/null || true)"
check "no site advances the mtime by one second" "0" "${BAD_SITES:-0}"

# Every touch -A in the script must use a form that clears a minute. Anything
# shorter cannot overtake an object file written during a build.
SHORT_FORMS="$(grep -oE 'touch -A [0-9]+' "${MUTATE}" 2>/dev/null \
    | awk '{ v=$3; if (length(v) <= 2) c++ } END { print c+0 }')"
check "every touch -A argument is longer than SS" "0" "${SHORT_FORMS:-0}"

# The script must still touch in both places -- deleting the calls would pass
# the two checks above and reintroduce the stale-binary bug they exist to
# prevent.
TOUCH_SITES="$(grep -c 'touch -A' "${MUTATE}" 2>/dev/null || true)"
check "both touch sites are still present" "2" "${TOUCH_SITES:-0}"

# --- THE BEHAVIOUR: the advance actually overtakes a fresh object -------------
#
# The real question is not "what does -A mean" but "does the source end up newer
# than an object written moments ago". Reproduced concretely: an object file
# stamped NOW, a source file stamped in the past (as `cp` from a backup leaves
# it), then the script's own touch form applied.
#
# The threshold is 60 seconds rather than 0. Zero would pass against the bug on
# a fixture where the source happened to be only a second stale; 60 is the
# smallest advance that clears a build of ordinary length.
TOUCH_FORM="$(grep -oE 'touch -A [0-9]+' "${MUTATE}" | head -1 | awk '{print $3}')"

if [[ -z "${TOUCH_FORM}" ]]; then
    check "a touch form was found to exercise" "yes" "no"
else
    OBJ="${WORK}/fake.o"
    SRC="${WORK}/fake.cpp"
    touch "${OBJ}"                       # the object: written now
    touch -t 202001010000 "${SRC}"       # the source: stale, as cp would leave it

    STALE_BY="$(python3 - "${SRC}" "${OBJ}" <<'PY'
import os, sys
print(int(round(os.path.getmtime(sys.argv[1]) - os.path.getmtime(sys.argv[2]))))
PY
)"
    # CONTROL: the fixture must start in the broken state, or "the source is
    # newer" below proves nothing about the touch.
    check "CONTROL: the source starts OLDER than the object" "yes" \
        "$([[ "${STALE_BY}" -lt 0 ]] && echo yes || echo no)"

    touch -A "${TOUCH_FORM}" "${SRC}"

    # A single touch on a years-stale file cannot overtake anything, which is
    # the real-world shape (cp restores an old mtime). What the harness relies
    # on is that the touch is applied to a file whose mtime is already near now,
    # so measure that case too: restamp to now, then apply the form.
    touch "${SRC}"
    touch -A "${TOUCH_FORM}" "${SRC}"

    AHEAD="$(python3 - "${SRC}" "${OBJ}" <<'PY'
import os, sys
print(int(round(os.path.getmtime(sys.argv[1]) - os.path.getmtime(sys.argv[2]))))
PY
)"
    check "the touched source clears the object by over a minute" "yes" \
        "$([[ "${AHEAD}" -ge 60 ]] && echo yes || echo no)"
fi

echo
echo "  ${PASS} passed, ${FAIL} failed."
[[ "${FAIL}" -eq 0 ]]
