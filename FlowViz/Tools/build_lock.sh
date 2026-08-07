#!/usr/bin/env bash
#
# Serialize UnrealBuildTool invocations across concurrent agents.
#
#   Tools/build_lock.sh ./Tools/run_tests.sh FlowViz.Render
#   Tools/build_lock.sh "$UE/Engine/Build/BatchFiles/Mac/Build.sh" FlowVizEditor ...
#
# Runs the given command holding an exclusive lock, so two agents never build at
# the same moment. Everything else -- editing, reading, git -- runs unserialized;
# only the build/test step needs this.
#
# WHY THIS EXISTS. Two concurrent builds in this project have already produced a
# result shaped exactly like a test verdict but caused by the build system:
#
#   1. UnrealBuildTool aborted because ~/Library/Application Support/Epic/
#      UnrealBuildTool/Trace.uba -- a USER-GLOBAL file, shared across worktrees --
#      was deleted by a concurrent build mid-run. That is why a per-worktree lock
#      is not sufficient and this lock is global to the machine.
#   2. Two campaigns sharing a fixed /tmp state path scored each other's mutants.
#
# Both were misattribution: output that looks like "the code under test failed"
# when the truth is "the build tripped over another build". See the repo memory
# note verdict-harness-must-attribute-failures.
#
# NO flock ON macOS. The BSD userland has no flock(1), so this uses mkdir, which
# is atomic on every POSIX filesystem: exactly one of N racing mkdir calls
# succeeds. A lock file created with `>` or `test -e` would not be -- both have a
# window between the check and the create.
#
# The lock is released by a trap on EXIT/INT/TERM, and a lock whose owning PID is
# gone is treated as stale and broken, so a killed agent cannot wedge the queue.

set -uo pipefail

LOCK_DIR="${FLOWVIZ_BUILD_LOCK:-/tmp/flowviz-build.lock}"
# Long enough for a cold full build; short enough that a wedged lock clears.
TIMEOUT_SECONDS="${FLOWVIZ_BUILD_LOCK_TIMEOUT:-3600}"
POLL_SECONDS=5

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

# Refuse to build in a checkout with a live mutation campaign, unless we ARE
# that campaign. Checked before the lock is taken: a refusal should not first
# queue behind someone else's hour-long build.
#
# The checkout is resolved from THIS SCRIPT's location, not the caller's cwd --
# `git rev-parse` without -C answers about wherever the shell is standing, and
# a guard that consulted one checkout's marker while the build compiled
# another's files would protect nothing. See Tools/mutation_window.sh.
_LOCK_TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "${_LOCK_TOOLS}/mutation_window.sh"
if _MARKER="$(marker_path_for "${_LOCK_TOOLS}")"; then
    if [[ "$(check_mutation_window "${_MARKER}" "${FLOWVIZ_MUTATION_TOKEN:-}")" == "blocked" ]]; then
        refuse_mutation_window "${_MARKER}" "this build"
        # 76 = EX_PROTOCOL, and deliberately NOT the 75 (EX_TEMPFAIL) this
        # script returns for a lock timeout. mutate.sh RETRIES on 75; an open
        # window is permanent until the campaign ends, so reusing 75 would turn
        # a refusal into a retry loop that eventually reports something else.
        exit 76
    fi
fi

acquired=0

release_lock() {
    # Only remove a lock we actually hold. Without this check, an instance that
    # timed out waiting would delete the running builder's lock on its way out.
    if [[ "${acquired}" == "1" ]]; then
        rm -rf "${LOCK_DIR}"
    fi
}
trap release_lock EXIT INT TERM

waited=0
while true; do
    if mkdir "${LOCK_DIR}" 2>/dev/null; then
        acquired=1
        echo "$$" > "${LOCK_DIR}/pid"
        date -u +%Y-%m-%dT%H:%M:%SZ > "${LOCK_DIR}/acquired-at"
        break
    fi

    # Held. If the holder is gone, the lock is stale -- break it rather than
    # blocking every later build until a human notices.
    holder="$(cat "${LOCK_DIR}/pid" 2>/dev/null || echo "")"
    if [[ -n "${holder}" ]] && ! kill -0 "${holder}" 2>/dev/null; then
        echo "build_lock: holder pid ${holder} is gone; breaking stale lock" >&2
        rm -rf "${LOCK_DIR}"
        continue
    fi

    if [[ "${waited}" -ge "${TIMEOUT_SECONDS}" ]]; then
        echo "build_lock: timed out after ${TIMEOUT_SECONDS}s waiting for ${LOCK_DIR}" >&2
        echo "build_lock: held by pid ${holder:-unknown} since $(cat "${LOCK_DIR}/acquired-at" 2>/dev/null || echo unknown)" >&2
        # 75 = EX_TEMPFAIL. Distinct from the wrapped command's own codes, so a
        # caller can tell "never ran" from "ran and failed" -- scoring a
        # lock timeout as a test failure is the exact misattribution this
        # script exists to prevent.
        exit 75
    fi

    if [[ "${waited}" -eq 0 ]]; then
        echo "build_lock: waiting for build lock held by pid ${holder:-unknown}..." >&2
    fi
    sleep "${POLL_SECONDS}"
    waited=$((waited + POLL_SECONDS))
done

"$@"
exit $?
