#!/usr/bin/env bash
#
# Tests for Tools/mutation_window.sh and the two callers that enforce it.
#
# Run: FlowViz/Tools/tests/test_mutation_window.sh
#
# WHAT IS BEING PROTECTED. A mutation campaign deliberately breaks a production
# file in a checkout, builds, scores, and restores it. Any build or test run in
# that same checkout during the window is void in BOTH directions:
#
#   - the run's own result may have compiled against the mutated file
#   - the campaign's verdicts are corrupted, because the run clobbered the
#     binary the campaign was about to score
#
# Observed 2026-08-06, self-inflicted: I isolated a campaign into its own
# worktree so no peer could reach it, then ran my own build and suite in that
# same worktree from the same shell. The pre-commit guard caught the commit
# attempt; nothing caught the build. The guard was the last line, not the first.
#
# WHY EVERY CHECK BELOW HAS A CONTROL. "The command did not run" is satisfied
# just as well by a script that is simply broken, or by a stub that never ran
# for an unrelated reason. Absence is not evidence. So each refusal check is
# paired with the SAME invocation minus the marker, which must run the command
# -- if the control also reports "did not run", the refusal check proves
# nothing and this file says so. See the repo memory notes
# verify-metrics-can-fail and a-control-needs-a-known-nonzero-expectation.
#
# WHY THE END-TO-END CHECKS EXIST alongside the pure-function ones. Testing
# check_mutation_window() in isolation says the classifier is right; it says
# nothing about whether build_lock.sh or run_tests.sh ever calls it. That is
# the mocking-a-seam-hides-that-nothing-builds-it failure, already paid for in
# this repo. The end-to-end checks copy the real scripts into a throwaway git
# repo and drive them for real.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$(cd "${HERE}/.." && pwd)"

# shellcheck source=/dev/null
source "${TOOLS}/mutation_window.sh"

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

echo "mutation_window.sh:"

# --- the classifier ----------------------------------------------------------

check "no marker at all is clear" "clear" \
    "$(check_mutation_window "${WORK}/absent" "")"

MARKER="${WORK}/marker"
{
    echo "pid=4242"
    echo "source=Private/Scene/FlowVizVolumeComponent.cpp"
    echo "started=2026-08-06T02:02:35Z"
} > "${MARKER}"

check "a marker with no token is BLOCKED" "blocked" \
    "$(check_mutation_window "${MARKER}" "")"

check "the campaign that declared the window may build" "clear" \
    "$(check_mutation_window "${MARKER}" "4242")"

# A token left over in the environment from an earlier campaign must not open
# the window for a later one. This is the difference between "am I the owner"
# and "have I ever been an owner" -- an exported variable survives longer than
# the run that set it, and a shell that inherited one would sail straight
# through the guard.
check "a token from a DIFFERENT campaign is blocked" "blocked" \
    "$(check_mutation_window "${MARKER}" "9999")"

check "a token that merely contains the pid is blocked" "blocked" \
    "$(check_mutation_window "${MARKER}" "14242")"

# Fail closed. A marker whose pid line is missing or unreadable means the
# window's owner is unknown, and "unknown owner" must not be more permissive
# than "known other owner".
printf 'source=something\n' > "${WORK}/marker_nopid"
check "a marker with no pid line blocks even a token holder" "blocked" \
    "$(check_mutation_window "${WORK}/marker_nopid" "4242")"

: > "${WORK}/marker_empty"
check "an empty marker file still blocks" "blocked" \
    "$(check_mutation_window "${WORK}/marker_empty" "4242")"

# --- end to end: build_lock.sh -----------------------------------------------
#
# A throwaway git repo with the real scripts copied in, so PROJECT_DIR resolves
# to it and the marker lands in its .git. Nothing here touches the FlowViz tree.

make_repo() {
    local root="$1"
    mkdir -p "${root}/Proj/Tools"
    cp "${TOOLS}/build_lock.sh" "${TOOLS}/run_tests.sh" \
       "${TOOLS}/mutation_window.sh" "${root}/Proj/Tools/"
    git -C "${root}" init -q 2>/dev/null
    git -C "${root}" config user.email t@t >/dev/null 2>&1
    git -C "${root}" config user.name t >/dev/null 2>&1
    # A commit so the repo is not in a half-initialised state.
    : > "${root}/seed"
    git -C "${root}" add -A >/dev/null 2>&1
    git -C "${root}" commit -qm seed >/dev/null 2>&1
}

declare_window() { printf 'pid=%s\nsource=x.cpp\nstarted=now\n' "$1" > "$2/.git/FLOWVIZ_MUTATION_ACTIVE"; }

REPO="${WORK}/repo"
make_repo "${REPO}"
LOCKED="${REPO}/Proj/Tools/build_lock.sh"

# CONTROL FIRST, deliberately. If the wrapped command does not run with the
# tree clean, every "did not run" below is meaningless and the reader should
# see that before reading the refusals.
rm -f "${REPO}/ran"
FLOWVIZ_BUILD_LOCK="${WORK}/bl1" "${LOCKED}" \
    sh -c "touch '${REPO}/ran'" >/dev/null 2>&1
control_exit=$?
check "CONTROL: with no window open, build_lock RUNS the command" "present" \
    "$([[ -e "${REPO}/ran" ]] && echo present || echo absent)"
check "CONTROL: ...and exits 0" "0" "${control_exit}"

declare_window 4242 "${REPO}"

rm -f "${REPO}/ran"
FLOWVIZ_BUILD_LOCK="${WORK}/bl2" "${LOCKED}" \
    sh -c "touch '${REPO}/ran'" >/dev/null 2>&1
blocked_exit=$?
check "with a window open, build_lock does NOT run the command" "absent" \
    "$([[ -e "${REPO}/ran" ]] && echo present || echo absent)"

# NOT 75. build_lock already returns 75 (EX_TEMPFAIL) for a lock timeout, and
# mutate.sh RETRIES on 75 -- so reusing it here would turn a permanent refusal
# into a retry loop that eventually gives up and reports something else.
check "...and refuses with 76, distinct from the 75 that means 'retry'" "76" \
    "${blocked_exit}"

rm -f "${REPO}/ran"
FLOWVIZ_MUTATION_TOKEN=4242 FLOWVIZ_BUILD_LOCK="${WORK}/bl3" "${LOCKED}" \
    sh -c "touch '${REPO}/ran'" >/dev/null 2>&1
owner_exit=$?
check "the campaign's own builds still run (guard does not deadlock mutate.sh)" \
    "present" "$([[ -e "${REPO}/ran" ]] && echo present || echo absent)"
check "...and exit 0" "0" "${owner_exit}"

rm -f "${REPO}/ran"
FLOWVIZ_MUTATION_TOKEN=9999 FLOWVIZ_BUILD_LOCK="${WORK}/bl4" "${LOCKED}" \
    sh -c "touch '${REPO}/ran'" >/dev/null 2>&1
check "a stale token does not open someone else's window" "absent" \
    "$([[ -e "${REPO}/ran" ]] && echo present || echo absent)"

# The refusal must say which checkout and which source, or the reader cannot
# tell their own campaign from a peer's abandoned one.
msg="$(FLOWVIZ_BUILD_LOCK="${WORK}/bl5" "${LOCKED}" true 2>&1 >/dev/null)"
check "the refusal names the mutated source" "yes" \
    "$(grep -q "x.cpp" <<<"${msg}" && echo yes || echo no)"
check "the refusal names the marker path" "yes" \
    "$(grep -q "FLOWVIZ_MUTATION_ACTIVE" <<<"${msg}" && echo yes || echo no)"

# --- end to end: a LINKED WORKTREE, which is where this actually happens -----
#
# Every check above uses a plain `git init` repo, where the git dir is simply
# .git. A linked worktree's is .git/worktrees/<name>/, and the accident this
# guard exists to prevent happened in one -- as does every campaign, since
# mutate.sh's own header instructs you to run it in a worktree. A marker path
# spelled `<toplevel>/.git/...` would satisfy all 21 checks above and protect
# nothing where it counts.
#
# The failure this pins is not hypothetical in this repo: a mutation fence
# already shipped once using a cwd-dependent `git rev-parse --git-dir`, which
# resolved against the wrong tree.

MAIN="${WORK}/mainrepo"
make_repo "${MAIN}"
WT="${WORK}/linked"
git -C "${MAIN}" worktree add -q --detach "${WT}" >/dev/null 2>&1
wt_ok=$?
if [[ "${wt_ok}" -ne 0 || ! -d "${WT}/Proj/Tools" ]]; then
    failures=$((failures + 1))
    echo "  FAIL  could not create a linked worktree; the linked-worktree checks did NOT run"
else
    WT_GITDIR="$(git -C "${WT}" rev-parse --absolute-git-dir)"
    check "a linked worktree's git dir is NOT its toplevel/.git" "differs" \
        "$([[ "${WT_GITDIR}" == "${WT}/.git" ]] && echo same || echo differs)"

    WT_LOCK="${WT}/Proj/Tools/build_lock.sh"

    # CONTROL first, again: the linked worktree builds fine with no window.
    rm -f "${WT}/ran"
    FLOWVIZ_BUILD_LOCK="${WORK}/wl1" "${WT_LOCK}" \
        sh -c "touch '${WT}/ran'" >/dev/null 2>&1
    check "CONTROL: linked worktree with no window RUNS the command" "present" \
        "$([[ -e "${WT}/ran" ]] && echo present || echo absent)"

    printf 'pid=4242\nsource=x.cpp\nstarted=now\n' > "${WT_GITDIR}/FLOWVIZ_MUTATION_ACTIVE"

    rm -f "${WT}/ran"
    FLOWVIZ_BUILD_LOCK="${WORK}/wl2" "${WT_LOCK}" \
        sh -c "touch '${WT}/ran'" >/dev/null 2>&1
    wt_exit=$?
    check "a window in a LINKED WORKTREE blocks its build" "absent" \
        "$([[ -e "${WT}/ran" ]] && echo present || echo absent)"
    check "...with 76" "76" "${wt_exit}"

    # A window in the worktree must NOT block the main checkout, and vice
    # versa. Nine worktrees exist precisely so campaigns do not stop each
    # other; a guard that read one shared marker would serialize all of them
    # and get itself disabled the first time it did.
    rm -f "${MAIN}/ran"
    FLOWVIZ_BUILD_LOCK="${WORK}/wl3" "${MAIN}/Proj/Tools/build_lock.sh" \
        sh -c "touch '${MAIN}/ran'" >/dev/null 2>&1
    check "...and does NOT block the main checkout" "present" \
        "$([[ -e "${MAIN}/ran" ]] && echo present || echo absent)"

    rm -f "${WT}/ran"
    FLOWVIZ_MUTATION_TOKEN=4242 FLOWVIZ_BUILD_LOCK="${WORK}/wl4" "${WT_LOCK}" \
        sh -c "touch '${WT}/ran'" >/dev/null 2>&1
    check "the worktree campaign's own builds still run" "present" \
        "$([[ -e "${WT}/ran" ]] && echo present || echo absent)"

    # The guard resolves the checkout from the SCRIPT's location, not the
    # caller's cwd. Invoked from outside the worktree entirely, it must still
    # find that worktree's marker -- this is the exact bug the fence shipped.
    rm -f "${WT}/ran"
    ( cd "${WORK}" && FLOWVIZ_BUILD_LOCK="${WORK}/wl5" "${WT_LOCK}" \
        sh -c "touch '${WT}/ran'" >/dev/null 2>&1 )
    check "the window is found when invoked from an unrelated cwd" "absent" \
        "$([[ -e "${WT}/ran" ]] && echo present || echo absent)"
fi

# --- end to end: run_tests.sh ------------------------------------------------
#
# run_tests.sh launches the editor directly; it does not go through
# build_lock.sh, so guarding only the lock would leave a bare `run_tests.sh`
# free to run inside the window. A stub editor records whether it was reached.

STUB_UE="${WORK}/fakeue"
mkdir -p "${STUB_UE}/Engine/Binaries/Mac"
cat > "${STUB_UE}/Engine/Binaries/Mac/UnrealEditor-Cmd" <<STUB
#!/usr/bin/env bash
touch "${WORK}/editor_ran"
# Write a log at the -abslog path so run_tests.sh gets past its own checks.
for a in "\$@"; do
    case "\$a" in
        -abslog=*) printf 'Test Completed. Result={Success} Name={x} Path={FlowViz.Stub}\n' > "\${a#-abslog=}" ;;
    esac
done
exit 0
STUB
chmod +x "${STUB_UE}/Engine/Binaries/Mac/UnrealEditor-Cmd"

RT="${REPO}/Proj/Tools/run_tests.sh"

# CONTROL: no window -> the editor IS reached. Without this the refusal check
# below would pass even if the stub were simply never wired up correctly.
CLEAN="${WORK}/repo_clean"
make_repo "${CLEAN}"
rm -f "${WORK}/editor_ran"
UE_ROOT="${STUB_UE}" LOG="${WORK}/rt_clean.log" \
    "${CLEAN}/Proj/Tools/run_tests.sh" FlowViz.Stub >/dev/null 2>&1
check "CONTROL: with no window open, run_tests REACHES the editor" "present" \
    "$([[ -e "${WORK}/editor_ran" ]] && echo present || echo absent)"

rm -f "${WORK}/editor_ran"
UE_ROOT="${STUB_UE}" LOG="${WORK}/rt_blocked.log" \
    "${RT}" FlowViz.Stub >/dev/null 2>&1
rt_exit=$?
check "with a window open, run_tests does NOT reach the editor" "absent" \
    "$([[ -e "${WORK}/editor_ran" ]] && echo present || echo absent)"
check "...and refuses with 76" "76" "${rt_exit}"

rm -f "${WORK}/editor_ran"
FLOWVIZ_MUTATION_TOKEN=4242 UE_ROOT="${STUB_UE}" LOG="${WORK}/rt_owner.log" \
    "${RT}" FlowViz.Stub >/dev/null 2>&1
check "the campaign's own test runs still reach the editor" "present" \
    "$([[ -e "${WORK}/editor_ran" ]] && echo present || echo absent)"

# --summarize reads a log that already exists and launches nothing, so it is
# safe inside the window -- and refusing it would make the window harder to
# diagnose from outside, which is the opposite of the point.
rm -f "${WORK}/editor_ran"
UE_ROOT="${STUB_UE}" "${RT}" --summarize "${WORK}/rt_clean.log" FlowViz.Stub \
    >/dev/null 2>&1
check "--summarize is allowed inside the window (it launches nothing)" "absent" \
    "$([[ -e "${WORK}/editor_ran" ]] && echo present || echo absent)"

echo
echo "mutation_window tests: $((checks - failures)) passed, ${failures} failed"
exit $(( failures > 0 ? 1 : 0 ))
