#!/usr/bin/env bash
#
# Tests for Tools/mutation_guard.sh, the pre-commit hook that refuses to commit
# while a mutation window is open.
#
# Run: FlowViz/Tools/tests/test_mutation_guard.sh
#
# WHY THIS EXISTS. On 2026-08-05 a deliberately deleted production guard sat in
# the shared working tree while five agents worked in it. Any one of them
# running `git add -A` would have committed a real defect to main under their
# own task's name, with an innocent message on top and a green suite beside it.
# Verdict corruption gets caught by a re-run; a mis-attributed commit does not,
# because nothing about it looks wrong afterward.
#
# Warnings to peers are not a mechanism -- they depend on every future agent
# reading and remembering them. This is the mechanism.
#
# THE CONTROL IS THE POINT OF THIS FILE. A hook that rejected every commit
# would pass any test that only checks "the mutation-shaped commit was
# rejected". So each rejection case is paired with a clean-commit case that
# MUST still succeed. If the clean commit is refused, the guard is not
# distinguishing a mutation window from ordinary work and this file says so.
# See the repo memory note verify-metrics-can-fail.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUARD="${HERE}/../mutation_guard.sh"

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

if [[ ! -x "${GUARD}" ]]; then
    echo "mutation_guard.sh: FAIL - ${GUARD} does not exist or is not executable"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# A throwaway repo. The guard is tested against real `git commit` runs rather
# than by calling it directly, because "the hook rejects" and "the commit is
# actually prevented" are different claims and only the second one matters.
REPO="${WORK}/repo"
mkdir -p "${REPO}"
git init -q "${REPO}"
git -C "${REPO}" config user.email "test@flowviz.invalid"
git -C "${REPO}" config user.name "FlowViz Test"
mkdir -p "${REPO}/.git/hooks"
cp "${GUARD}" "${REPO}/.git/hooks/pre-commit"
chmod +x "${REPO}/.git/hooks/pre-commit"

printf 'int production(void) { return 1; }\n' > "${REPO}/prod.c"
git -C "${REPO}" add prod.c
git -C "${REPO}" commit -qm "baseline" 2>/dev/null

echo "mutation_guard.sh:"

# --- with no mutation window, ordinary work commits normally -----------------
#
# This is the control. Run FIRST, so a guard that simply refuses everything is
# caught before any of the rejection cases below can flatter it.

printf 'int production(void) { return 2; }\n' > "${REPO}/prod.c"
git -C "${REPO}" add prod.c
git -C "${REPO}" commit -qm "ordinary change" >/dev/null 2>&1
check "a normal commit succeeds when no mutation is active" "0" "$?"

subject="$(git -C "${REPO}" log -1 --format=%s)"
check "and it really landed" "ordinary change" "${subject}"

# --- with a mutation window open, the commit is refused ----------------------

MARKER="${REPO}/.git/FLOWVIZ_MUTATION_ACTIVE"
printf 'agent=test-agent\nfile=prod.c\n' > "${MARKER}"

before="$(git -C "${REPO}" rev-parse HEAD)"
printf 'int production(void) { return 3; }\n' > "${REPO}/prod.c"
git -C "${REPO}" add prod.c
git -C "${REPO}" commit -qm "would capture someone else's mutant" >/dev/null 2>&1
check "a commit is REFUSED while a mutation window is open" "1" "$(( $? == 0 ? 0 : 1 ))"

after="$(git -C "${REPO}" rev-parse HEAD)"
check "and HEAD did not move, so nothing was captured" "${before}" "${after}"

# The refusal has to say what to do. A hook that fails with a bare exit code
# gets disabled by the next agent who hits it and cannot tell why.
msg="$(git -C "${REPO}" commit -qm "again" 2>&1)"
case "${msg}" in
    *FLOWVIZ_MUTATION_ACTIVE*) named_marker="yes" ;;
    *)                         named_marker="no"  ;;
esac
check "and the refusal names the marker file so it can be cleared" "yes" "${named_marker}"

# --- clearing the window restores normal service -----------------------------
#
# The second half of the control. Without this, every assertion above would
# also pass against a hook that refuses forever once triggered -- which would
# block the repo and be discovered only by whoever it blocked.

rm -f "${MARKER}"
git -C "${REPO}" commit -qm "after the window closed" >/dev/null 2>&1
check "a commit succeeds again once the window is closed" "0" "$?"

subject="$(git -C "${REPO}" log -1 --format=%s)"
check "and that commit landed too" "after the window closed" "${subject}"

# --- the window survives an interrupted mutation run -------------------------
#
# A marker that only blocks while a script is alive protects nothing: the
# dangerous case IS the abandoned mutation, where the runner died between
# applying the defect and restoring it. The marker is a file precisely so that
# it outlives the process that made it, so assert that directly.

printf 'agent=dead-runner\n' > "${MARKER}"
( exit 0 )  # the notional mutation runner exits without cleaning up
git -C "${REPO}" commit -qm "after an abandoned run" --allow-empty >/dev/null 2>&1
check "an abandoned mutation window still blocks commits" "1" "$(( $? == 0 ? 0 : 1 ))"
rm -f "${MARKER}"

# --- the guard is not fooled by a marker in the working tree -----------------
#
# The marker lives under .git/ so it can never be staged, committed, or wiped
# by `git checkout -- .`. A marker that lived in the working tree could be
# deleted by exactly the kind of sweeping restore that follows a mutation run.

printf 'agent=x\n' > "${REPO}/FLOWVIZ_MUTATION_ACTIVE"
git -C "${REPO}" commit -qm "working-tree marker is not the mechanism" --allow-empty >/dev/null 2>&1
check "a marker in the working tree does not block (only .git/ counts)" "0" "$?"
rm -f "${REPO}/FLOWVIZ_MUTATION_ACTIVE"

echo
if [[ ${failures} -eq 0 ]]; then
    echo "mutation_guard.sh: ${checks} checks, all passed"
    exit 0
fi
echo "mutation_guard.sh: ${checks} checks, ${failures} FAILED"
exit 1
