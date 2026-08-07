#!/usr/bin/env bash
#
# Refuse to build or test in a checkout with an open mutation window.
#
# Sourced by Tools/build_lock.sh and Tools/run_tests.sh. Not executable on its
# own -- it defines check_mutation_window() and refuse_mutation_window().
# Tested by: Tools/tests/test_mutation_window.sh
#
# THE FAILURE THIS PREVENTS, which I caused myself on 2026-08-06.
#
# Tools/mutate.sh breaks a production file, builds, scores, restores, repeats.
# It already declares that window in .git/FLOWVIZ_MUTATION_ACTIVE so the
# pre-commit hook can refuse to COMMIT the live defect. But nothing refused to
# BUILD against it. I moved a campaign into its own worktree -- correctly
# isolating it from every peer -- and then, from the same shell, ran my own
# build and suite in that same worktree while an arm was applied. Both results
# were void, in both directions at once:
#
#   - my "5/5 passed" may have compiled against the mutated file, so a green
#     that proves nothing reads exactly like a green that proves something
#   - the campaign's verdicts were corrupted, because my build overwrote the
#     binary it was about to score -- and the corrupt direction is a false
#     KILLED, which retires a coverage gap instead of reporting it
#
# WHY THE COMMIT HOOK WAS NOT ENOUGH. It fires at the last step, after the
# damage is done and only if you happen to commit. The build is where the two
# runs actually collide. A guard that only catches you on the way out is a
# record of the accident, not a prevention of it.
#
# WHY A TOKEN AND NOT "IS THE PID ALIVE". The campaign itself must keep
# building -- that is its whole job -- so the guard needs to distinguish the
# owner from everyone else. Liveness cannot do it: every agent shell command
# runs in a fresh process, so a recorded pid reads as dead whether the campaign
# is running or abandoned (this repo already made that inference once, from a
# correct reading of a signal that distinguishes nothing). mutate.sh instead
# EXPORTS its pid as FLOWVIZ_MUTATION_TOKEN into the builds it spawns; only a
# process descended from the declaring campaign inherits it.
#
# The token is compared for EXACT equality against the marker's pid, so a stale
# token exported in some long-lived shell cannot open a later campaign's
# window, and a token that merely contains the pid as a substring is refused.
#
# FAILS CLOSED. A marker that exists but whose pid cannot be read means the
# owner is unknown, and unknown must never be more permissive than known-other.
# The only "clear" verdicts are: no marker at all, or an exact token match.

# check_mutation_window <marker-path> <token>
#   prints "clear" or "blocked"
check_mutation_window() {
    local marker="$1" token="${2:-}"

    [[ -e "${marker}" ]] || { echo "clear"; return 0; }

    local declared_pid=""
    declared_pid="$(sed -n -E 's/^pid=([0-9]+)[[:space:]]*$/\1/p' "${marker}" 2>/dev/null | head -1)"

    if [[ -n "${token}" && -n "${declared_pid}" && "${token}" == "${declared_pid}" ]]; then
        echo "clear"
        return 0
    fi

    echo "blocked"
}

# check_harness_drift <marker-path> <current-sha>
#   prints "same" | "drifted" | "unknown"
#
# A campaign runs from a private copy of mutate.sh (see the re-exec at the top
# of that script), so editing the checked-out file mid-run can no longer corrupt
# it. That fix trades a loud failure for a quiet one: the campaign now sails on
# using the OLD code, and prints verdicts that get attributed to the FIXED
# harness. Nothing in that output looks wrong afterwards.
#
# So the campaign records the fingerprint of the copy it is actually executing,
# and compares it against the checked-out file when it finishes. Drift does not
# invalidate the verdicts -- the code that produced them ran coherently -- but
# it does mean they describe a harness that no longer exists on disk, which is
# the one thing a reader would otherwise assume.
#
# UNKNOWN IS NOT "SAME". A marker with no recorded fingerprint (an older
# campaign), a missing marker, or an empty computed sha (shasum failed, file
# gone) all mean the comparison did not happen. Reporting those as "same" is the
# fail-open shape this repo has been bitten by before -- a missing summary read
# as a pass. Two empty strings are equal and prove nothing.
check_harness_drift() {
    local marker="$1" current="${2:-}"

    [[ -e "${marker}" ]] || { echo "unknown"; return 0; }
    [[ -n "${current}" ]] || { echo "unknown"; return 0; }

    local recorded=""
    recorded="$(sed -n -E 's/^harness_sha=([0-9a-f]+)[[:space:]]*$/\1/p' "${marker}" 2>/dev/null | head -1)"
    [[ -n "${recorded}" ]] || { echo "unknown"; return 0; }

    [[ "${recorded}" == "${current}" ]] && echo "same" || echo "drifted"
}

# marker_path_for <dir>
#   The marker for the checkout that CONTAINS <dir> -- resolved with `git -C`,
#   never from the caller's cwd. `git rev-parse --git-dir` answers about
#   wherever the shell happens to be standing, so a guard that used the bare
#   form would consult one checkout's marker while the build compiled another's
#   files. This repo has already shipped that bug once, in a mutation fence.
marker_path_for() {
    local dir="$1" gitdir=""
    gitdir="$(git -C "${dir}" rev-parse --absolute-git-dir 2>/dev/null)" || return 1
    [[ -n "${gitdir}" ]] || return 1
    printf '%s/FLOWVIZ_MUTATION_ACTIVE\n' "${gitdir}"
}

# refuse_mutation_window <marker-path> <what-was-refused>
#   Prints the refusal to stderr. Does not exit -- the caller chooses the code.
refuse_mutation_window() {
    local marker="$1" what="${2:-this run}"
    {
        echo
        echo "REFUSED: ${what} would run inside an open mutation window."
        echo
        echo "  marker: ${marker}"
        if [[ -s "${marker}" ]]; then
            local declared
            declared="$(sed -E 's/(^|[[:space:]])pid=[0-9]+[[:space:]]*/\1/g' "${marker}" \
                        | sed -E 's/^[[:space:]]+|[[:space:]]+$//g' | grep -v '^$')"
            if [[ -n "${declared}" ]]; then
                echo "  declared by:"
                printf '%s\n' "${declared}" | sed 's/^/    /'
            fi
        fi
        echo
        echo "A mutation campaign has deliberately broken a production file in this"
        echo "checkout. Building or testing here now voids BOTH results:"
        echo
        echo "  - yours, because it may compile against the mutated file -- a green"
        echo "    that proves nothing is indistinguishable from one that does"
        echo "  - the campaign's, because your build overwrites the binary it is"
        echo "    about to score, and that error's direction is a false KILLED"
        echo
        echo "Run your build in a different worktree:"
        echo "  git worktree add ~/projects/wind-worktrees/<name> HEAD --detach"
        echo
        echo "A dead pid is NOT evidence the campaign was abandoned -- every agent"
        echo "shell runs in a fresh process, so the declaring pid reads as dead"
        echo "either way. What establishes abandonment is the TREE: an unexplained"
        echo "production edit with nobody working on it."
        echo
        echo "  git diff    # what is actually modified, and why"
        echo
        echo "Do not delete the marker to get your build through. Clearing it while"
        echo "an arm is applied also disarms the pre-commit hook, which is the only"
        echo "thing keeping the live defect out of history."
        echo
    } >&2
}
