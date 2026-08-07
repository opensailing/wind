#!/bin/bash
# What does a mutation campaign do when it is killed mid-arm?
#
# It kept scoring. mutate.sh:192 was
#
#     restore() { cp "${BACKUP}" "${SRC}"; rm -f "${MARKER}"; }
#     trap restore EXIT INT TERM
#
# which restores the pristine file and clears the marker -- and then RETURNS,
# because a trap handler resumes the script unless it exits. The loop carries
# on to the next arm.
#
# Observed live, 2026-08-05: a dstat campaign was sent TERM so its mutant list
# could be corrected. It printed
#
#     SURVIVED  the classifier reports Dispatched no matter what -- the fail-OPEN direction
#
# for an arm that does not compile at all. The TERM landed in that arm's build
# window; restore() put the pristine file back; the build then succeeded and the
# suite passed -- because it was testing UNMUTATED SOURCE. A mutant that cannot
# build scored as a coverage gap.
#
# That is the shape this repo's memory keeps finding: not a lost result, but a
# manufactured one. A false SURVIVED sends someone to write a test for a defect
# that is not there, and the arm it lands on is whichever was building when the
# signal arrived, so nothing about it looks unusual afterwards.
#
# The second harm is quieter and worse. The marker is
# .git/FLOWVIZ_MUTATION_ACTIVE, which Tools/mutation_guard.sh (the pre-commit
# hook) reads to refuse a commit while a mutation is live. Deleting it while the
# loop still has arms to apply DISARMS that hook for the rest of the campaign --
# during exactly the window when a live mutant can be committed.
#
# So a signal must end the run: restore, say plainly that the campaign was
# interrupted and that no further verdict is trustworthy, and exit non-zero.
#
# These tests drive mutate.sh's trap shape through a STAND-IN loop rather than
# the real script, because the real one needs Unreal, a 15-minute build per arm
# and a lock. The stand-in reproduces the mechanism under test and nothing else:
# same trap, same restore, same per-arm cp/apply/score sequence.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MUTATE="${SCRIPT_DIR}/../mutate.sh"

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

# --- the mechanism, in a stand-in campaign ----------------------------------
#
# TRAP_BODY is substituted in, so the two spellings can be run against the same
# loop. Everything else is identical between them.
make_campaign() {
    local out="$1" trap_body="$2"
    cat > "${out}" <<EOF
#!/bin/bash
SRC="\${1}"; BACKUP="\${2}"; MARKER="\${3}"; SCORES="\${4}"
${trap_body}
for arm in 1 2 3; do
    cp "\${BACKUP}" "\${SRC}"
    echo "MUTANT-\${arm}" > "\${SRC}"
    sleep 2
    # A verdict is scored against whatever the file holds -- which is the whole
    # point: if a restore raced the build, this records PRISTINE.
    echo "arm\${arm}=\$(cat "\${SRC}")" >> "\${SCORES}"
done
EOF
    chmod +x "${out}"
}

# Run a campaign, TERM it during arm 1's window, report what it scored.
run_and_signal() {
    local script="$1"
    local src="${TMP}/src.$$" backup="${TMP}/backup.$$" marker="${TMP}/marker.$$"
    local scores="${TMP}/scores.$$"
    : > "${scores}"
    echo "PRISTINE" > "${src}"
    echo "PRISTINE" > "${backup}"
    echo "live" > "${marker}"

    "${script}" "${src}" "${backup}" "${marker}" "${scores}" >/dev/null 2>&1 &
    local pid=$!
    sleep 1
    kill -TERM "${pid}" 2>/dev/null
    wait "${pid}" 2>/dev/null
    SIGNAL_EXIT=$?
    SCORED_ARMS=$(wc -l < "${scores}" | tr -d ' ')
    # `grep -c || echo 0` prints "0\n0" when grep matches nothing: grep already
    # printed its own 0, and its exit 1 then fires the fallback too. The check
    # comparing against "0" failed on a correct result. Count the lines instead,
    # which emits exactly one number whether or not anything matched.
    SCORED_PRISTINE=$(grep "PRISTINE" "${scores}" 2>/dev/null | wc -l | tr -d ' ')
    MARKER_STATE=$([[ -f "${marker}" ]] && echo present || echo gone)
    rm -f "${src}" "${backup}" "${marker}" "${scores}"
}

# --- the OLD spelling, which must exhibit the defect -------------------------
#
# THE CONTROL. Without it a green run proves only that the new handler works in
# some sense; it cannot show that the check is capable of failing, and a check
# that cannot fail is what this repo's memory warns about most often. This runs
# the exact trap line mutate.sh shipped and asserts the damage is REPRODUCED.
make_campaign "${TMP}/old.sh" \
    'restore() { cp "${BACKUP}" "${SRC}"; rm -f "${MARKER}"; }
trap restore EXIT INT TERM'

run_and_signal "${TMP}/old.sh"
check "CONTROL: the shipped trap keeps scoring after TERM" "3" "${SCORED_ARMS}"
check "CONTROL: and scores an arm against PRISTINE source" "1" "${SCORED_PRISTINE}"
check "CONTROL: and clears the pre-commit marker early" "gone" "${MARKER_STATE}"

# --- the NEW spelling -------------------------------------------------------
#
# Restores, and ENDS. The EXIT handler still restores on a normal finish; only
# the signal path exits.
make_campaign "${TMP}/new.sh" \
    'restore() { cp "${BACKUP}" "${SRC}"; rm -f "${MARKER}"; }
on_signal() {
    restore
    echo "INTERRUPTED: campaign signalled mid-arm. Verdicts already printed" >&2
    echo "  stand; nothing after this point was scored." >&2
    exit 143
}
trap restore EXIT
trap on_signal INT TERM'

run_and_signal "${TMP}/new.sh"
check "a signalled campaign scores NO further arms" "0" "${SCORED_ARMS}"
check "so no arm is scored against pristine source" "0" "${SCORED_PRISTINE}"
check "and it exits non-zero rather than reporting a clean run" "143" "${SIGNAL_EXIT}"

# --- the new spelling must still restore on a NORMAL finish -----------------
#
# The obvious way to pass the tests above is to stop restoring on EXIT. That
# would leave a live mutant in the tree of every campaign that ends normally --
# a far worse defect than the one being fixed, and one the tests above cannot
# see because they always signal.
NORMAL_SRC="${TMP}/normal_src"
NORMAL_BACKUP="${TMP}/normal_backup"
NORMAL_MARKER="${TMP}/normal_marker"
echo "PRISTINE" > "${NORMAL_SRC}"
echo "PRISTINE" > "${NORMAL_BACKUP}"
echo "live" > "${NORMAL_MARKER}"
"${TMP}/new.sh" "${NORMAL_SRC}" "${NORMAL_BACKUP}" "${NORMAL_MARKER}" \
    "${TMP}/normal_scores" >/dev/null 2>&1
check "an uninterrupted campaign still restores the file" \
    "PRISTINE" "$(cat "${NORMAL_SRC}")"
check "an uninterrupted campaign still clears the marker" \
    "gone" "$([[ -f "${NORMAL_MARKER}" ]] && echo present || echo gone)"
check "and it scores every arm" "3" "$(wc -l < "${TMP}/normal_scores" | tr -d ' ')"

# --- and the real script must carry the fixed shape -------------------------
#
# The checks above exercise a stand-in. This is what ties them to the file that
# actually runs campaigns: without it, mutate.sh could keep the old trap
# forever while this suite reported six passes.
check "mutate.sh no longer traps INT/TERM to a handler that returns" \
    "0" "$(grep -cE '^trap (restore|cleanup) EXIT INT TERM' "${MUTATE}")"

# BOTH trap sites, not one. mutate.sh installs a second pair once RECORDS
# exists, and a later `trap ... INT TERM` REPLACES the earlier one -- so a fix
# applied to the first site alone is undone before the scoring loop starts, and
# the campaign runs its entire length with the defect back in place. Counting
# rather than existence-checking is what makes that visible: this is the repo's
# "a finding names an instance" rule, applied to the file that reports every
# other finding.
EXIT_TRAPS=$(grep -cE '^trap (restore|cleanup) EXIT$' "${MUTATE}")
SIGNAL_TRAPS=$(grep -c '^trap on_signal INT TERM' "${MUTATE}")
check "every EXIT trap in mutate.sh has a signal trap beside it" \
    "${EXIT_TRAPS}" "${SIGNAL_TRAPS}"
check "and there are two of them -- both trap sites are covered" \
    "2" "${SIGNAL_TRAPS}"

echo
echo "mutate.sh signal handling: ${PASS} passed, ${FAIL} failed"
[[ "${FAIL}" -eq 0 ]]
