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
    if [[ -s "${MARKER}" ]]; then
        echo "  declared by:"
        sed 's/^/    /' "${MARKER}"
        echo
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
    echo "If the run was abandoned and left this behind, verify the tree is"
    echo "clean of its mutation FIRST, then clear the window:"
    echo "  git diff        # confirm no unexplained production edits remain"
    echo "  rm ${MARKER}"
    echo
    echo "Do not delete the marker merely to get your commit through. It is the"
    echo "only thing standing between someone else's sabotage and your history."
    echo
} >&2

exit 1
