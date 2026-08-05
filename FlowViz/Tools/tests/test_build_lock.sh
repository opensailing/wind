#!/usr/bin/env bash
#
# Tests for Tools/build_lock.sh.
#
# Run: FlowViz/Tools/tests/test_build_lock.sh
#
# The property under test -- "two builds never overlap" -- is the kind that a
# broken implementation passes trivially. A lock script that simply ran the
# command with no locking at all would satisfy any test that only checks the
# command's output and exit code. So the mutual-exclusion test below measures
# OVERLAP directly: each concurrent worker appends an enter and an exit marker,
# and the test asserts the markers strictly alternate. It is verified against a
# deliberately unlocked control that MUST fail it -- if the control passes, the
# test cannot distinguish locking from not locking and reports itself broken.
#
# That control is the point of this file. See the repo memory note
# verify-metrics-can-fail: a pass criterion that cannot fail is not a check.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCK_SCRIPT="${HERE}/../build_lock.sh"

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

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

echo "build_lock.sh:"

# --- the wrapped command runs, and its exit code is preserved ----------------
#
# A lock that swallows the command's status would turn every failing build into
# a passing one, which is worse than no lock at all.

out="$(FLOWVIZ_BUILD_LOCK="${WORK}/l1" "${LOCK_SCRIPT}" echo hello 2>/dev/null)"
check "the wrapped command's stdout passes through" "hello" "${out}"

FLOWVIZ_BUILD_LOCK="${WORK}/l2" "${LOCK_SCRIPT}" true >/dev/null 2>&1
check "exit 0 is preserved" "0" "$?"

FLOWVIZ_BUILD_LOCK="${WORK}/l3" "${LOCK_SCRIPT}" sh -c 'exit 17' >/dev/null 2>&1
check "a nonzero exit code is preserved exactly" "17" "$?"

# --- the lock is released, including on failure ------------------------------

FLOWVIZ_BUILD_LOCK="${WORK}/l4" "${LOCK_SCRIPT}" sh -c 'exit 3' >/dev/null 2>&1
check "the lock dir is gone after a FAILING command" "absent" \
    "$([[ -e "${WORK}/l4" ]] && echo present || echo absent)"

# --- mutual exclusion, measured as overlap -----------------------------------
#
# Each worker appends "enter N", sleeps, appends "exit N". Under a working lock
# the trace strictly alternates enter/exit. Any interleaving means two workers
# were inside at once.

run_workers() {
    # $1 = "locked" | "unlocked"; writes trace to $2
    local mode="$1" trace="$2"
    : > "${trace}"
    local pids=()
    for i in 1 2 3; do
        (
            body="printf 'enter %s\n' ${i} >> '${trace}'; sleep 0.3; printf 'exit %s\n' ${i} >> '${trace}'"
            if [[ "${mode}" == "locked" ]]; then
                FLOWVIZ_BUILD_LOCK="${trace}.lock" "${LOCK_SCRIPT}" sh -c "${body}" 2>/dev/null
            else
                sh -c "${body}"
            fi
        ) &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "${p}"; done
}

# An overlap exists iff some "enter" is immediately followed by another "enter".
has_overlap() {
    awk '
        /^enter/ { if (inside) { found = 1 } ; inside = 1 }
        /^exit/  { inside = 0 }
        END { print (found ? "overlap" : "serial") }
    ' "$1"
}

run_workers locked "${WORK}/trace_locked"
check "three concurrent workers never overlap under the lock" "serial" \
    "$(has_overlap "${WORK}/trace_locked")"

check "...and all three actually ran (not silently skipped)" "3" \
    "$(grep -c '^enter' "${WORK}/trace_locked")"

# THE CONTROL. Same workers, no lock. This MUST report overlap, or the check
# above is vacuous -- it would pass just as happily with the locking removed.
run_workers unlocked "${WORK}/trace_unlocked"
check "CONTROL: the same workers DO overlap without the lock" "overlap" \
    "$(has_overlap "${WORK}/trace_unlocked")"

# --- a stale lock from a dead process is broken, not waited on ---------------
#
# Without this, one killed agent wedges every subsequent build until a human
# notices. Pid 99999 is chosen to be almost certainly unused; if it happens to
# exist this test would hang, so it is bounded by a short timeout.

mkdir -p "${WORK}/stale"
echo "99999" > "${WORK}/stale/pid"
if kill -0 99999 2>/dev/null; then
    echo "  skip  stale-lock test: pid 99999 exists on this machine"
else
    stale_out="$(FLOWVIZ_BUILD_LOCK="${WORK}/stale" FLOWVIZ_BUILD_LOCK_TIMEOUT=30 \
        "${LOCK_SCRIPT}" echo recovered 2>/dev/null)"
    check "a lock held by a dead pid is broken and the command runs" \
        "recovered" "${stale_out}"
fi

# --- a live holder makes the waiter time out with EX_TEMPFAIL, not run -------
#
# The distinct exit code is what lets a caller tell "never ran" from "ran and
# failed". Scoring a lock timeout as a build failure is exactly the
# misattribution this whole mechanism exists to prevent.

mkdir -p "${WORK}/held"
sleep 30 &
holder_pid=$!
echo "${holder_pid}" > "${WORK}/held/pid"

FLOWVIZ_BUILD_LOCK="${WORK}/held" FLOWVIZ_BUILD_LOCK_TIMEOUT=1 \
    "${LOCK_SCRIPT}" sh -c 'echo SHOULD_NOT_RUN > "'"${WORK}"'/ran"' >/dev/null 2>&1
check "waiting on a LIVE holder times out with 75 (EX_TEMPFAIL)" "75" "$?"
check "...and the wrapped command did NOT run" "absent" \
    "$([[ -e "${WORK}/ran" ]] && echo present || echo absent)"
check "...and the live holder's lock was NOT deleted by the waiter" "present" \
    "$([[ -e "${WORK}/held" ]] && echo present || echo absent)"

kill "${holder_pid}" 2>/dev/null
wait "${holder_pid}" 2>/dev/null

echo
echo "build_lock tests: $((checks - failures)) passed, ${failures} failed"
exit $(( failures > 0 ? 1 : 0 ))
