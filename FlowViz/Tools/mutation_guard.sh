#!/usr/bin/env bash
#
# pre-commit hook: refuse to commit while a mutation window is open.
#
# Install: cp Tools/mutation_guard.sh .git/hooks/pre-commit && chmod +x that
# Tested by: Tools/tests/test_mutation_guard.sh
#
# THE FAILURE THIS PREVENTS. Several agents share one checkout. A mutation run
# deliberately breaks a production file, builds, scores, and restores it. During
# that window every OTHER agent in the tree is one `git add -A` away from
# committing the deliberate defect under their own task's name -- with an
# innocent commit message on top, and a green suite beside it if their build
# predates the restore. Nothing about the result looks wrong afterward, which is
# what makes it worse than the verdict corruption a re-run would catch.
#
# WHY A MARKER FILE AND NOT A DIFF HEURISTIC. The tempting design is to inspect
# the staged diff and reject anything that "looks like" a deleted guard. That
# cannot work: a deliberate mutation and a legitimate deletion are the same
# bytes. The distinguishing fact is not in the diff at all -- it is that a
# mutation run is IN PROGRESS. So the runner declares the window and this hook
# reads the declaration. It fails closed: if the marker is there, nobody
# commits.
#
# WHY UNDER .git/. The marker must survive the sweeping `git checkout -- .` and
# `git stash` that follow a mutation run, and must never itself be committable.
# Anything in the working tree can be wiped by exactly the cleanup that the
# mutation run performs on its way out.
#
# WHY IT OUTLIVES THE RUNNER. The dangerous case is the ABANDONED run -- the
# process that died between applying the defect and restoring it. A lock held by
# a live process would release on that death and protect nobody at the moment
# protection matters most. A file persists until someone clears it deliberately.

set -uo pipefail

GIT_DIR_PATH="$(git rev-parse --git-dir 2>/dev/null)" || exit 0
MARKER="${GIT_DIR_PATH}/FLOWVIZ_MUTATION_ACTIVE"

[[ -e "${MARKER}" ]] || exit 0

# Everything below goes to stderr so it survives `git commit -q` and shows up
# even when the caller redirected stdout.
{
    echo
    echo "COMMIT REFUSED: a mutation window is open in this checkout."
    echo
    echo "  marker: ${MARKER}"
    echo
    # The pid is stripped deliberately. Every agent Bash call runs in a fresh
    # shell, so the recorded pid is dead by the declaring agent's NEXT tool
    # call -- live campaign or abandoned one, `ps -p` says "dead" either way.
    # Printing it beside the `rm` instruction below hands the reader a liveness
    # test that is always false, and the action it unlocks strips protection
    # from a mutation that is still running. That inference was made, correctly
    # reasoned from a false signal, on 2026-08-05.
    if [[ -s "${MARKER}" ]]; then
        declared="$(sed -E 's/(^|[[:space:]])pid=[0-9]+[[:space:]]*/\1/g' "${MARKER}" \
                    | sed -E 's/^[[:space:]]+|[[:space:]]+$//g' | grep -v '^$')"
        if [[ -n "${declared}" ]]; then
            echo "  declared by:"
            printf '%s\n' "${declared}" | sed 's/^/    /'
            echo
        fi
    fi
    echo "A mutation run has deliberately broken a production file somewhere in"
    echo "this tree. Committing now can capture that defect under YOUR task's"
    echo "name, with a passing suite beside it and nothing to show it was not"
    echo "yours. Staging by explicit path does not make this safe: the point is"
    echo "that the tree is knowingly lying right now."
    echo
    echo "If you are NOT running the mutation:"
    echo "  wait for it to finish, or ask its owner to move to a worktree"
    echo "  (git worktree add ../wind-mutate-<topic> HEAD --detach)."
    echo
    echo "A dead process is NOT evidence the run was abandoned. Every agent"
    echo "shell command runs in a fresh shell, so the declaring process is"
    echo "already gone by its own next command -- 'ps' reports it dead whether"
    echo "the campaign is live or not, and so distinguishes nothing. What"
    echo "establishes abandonment is the TREE: an unexplained production edit"
    echo "with nobody working on it, and a marker whose declared purpose"
    echo "matches no running task."
    echo
    echo "  git diff                    # what is actually modified, and why"
    echo "  git diff --stat HEAD        # is any of it unexplained?"
    echo
    echo "Ask the owner named above before concluding they are gone. Only once"
    echo "the tree is clean of the mutation:"
    echo "  rm ${MARKER}"
    echo
    echo "Do not delete the marker merely to get your commit through. It is the"
    echo "only thing standing between someone else's sabotage and your history."
    echo
} >&2

exit 1
