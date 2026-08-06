#!/usr/bin/env bash
#
# Tests that Tools/mutate.sh cannot be corrupted by an edit to its own file
# while a campaign is running.
#
# Run: FlowViz/Tools/tests/test_mutate_selfcopy.sh
#
# THE HAZARD, confirmed live on 2026-08-05. Bash does not read a script into
# memory: it reads lazily from an open fd, tracking a BYTE OFFSET. `lsof` on a
# running campaign showed fd 255r held on Tools/mutate.sh. Editing that file
# shifts every offset past the insertion point, so the running shell resumes
# parsing mid-token and executes whatever text now sits at its saved offset.
#
# For most scripts that is a crash. For this one it is worse, and specifically
# so: mutate.sh `cp`s over production source files and applies deliberate
# defects. A corrupted resume can leave a mutant live in a shared checkout,
# apply an arm under another arm's name, or skip the restore entirely -- and
# every one of those outcomes prints something that looks like a verdict.
#
# WHY THE TESTS BELOW USE A STAND-IN SCRIPT. The property is a property of bash,
# not of mutate.sh: "a long-running script survives having its own file
# rewritten". Driving the real mutate.sh would need an engine, a build and
# twenty minutes per arm. So the mechanism is exercised on a small script that
# re-execs the same way, and then -- crucially -- a separate check asserts that
# the REAL mutate.sh actually contains that mechanism and runs it before it
# touches anything. Testing the pattern without checking the real file adopts it
# is the mocking-a-seam failure this repo has already paid for.
#
# THE CONTROL IS THE WHOLE TEST. A script that survives an edit proves nothing
# unless the same script WITHOUT the guard demonstrably does not. Bash's
# corruption depends on buffering and offsets, so a poorly-built control can
# survive by luck and silently turn the real check into a tautology. The control
# here is verified to actually break; if it does not, this file reports itself
# broken rather than passing.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$(cd "${HERE}/.." && pwd)"
MUTATE="${TOOLS}/mutate.sh"

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

echo "mutate.sh self-copy:"

# --- the mechanism, on a stand-in --------------------------------------------
#
# Each stand-in writes a marker at the very end. Reaching that line after its
# own file has been rewritten mid-run is the property under test.

write_standin() {
    # $1 = path, $2 = "guarded" | "bare"
    local path="$1" mode="$2"
    {
        printf '#!/usr/bin/env bash\n'
        if [[ "${mode}" == "guarded" ]]; then
            # The same shape mutate.sh uses: copy self to a temp path and
            # re-exec from there, once, guarded against a loop.
            cat <<'GUARD'
if [[ -z "${STANDIN_REEXEC:-}" ]]; then
    _self="$(mktemp -t standin)"
    cat "$0" > "${_self}"
    chmod +x "${_self}"
    STANDIN_REEXEC=1 exec "${_self}" "$@"
fi
GUARD
        fi
        printf 'printf "START\\n" >> "%s"\n' "$3"
        # Padding so the edit below lands in the middle of the region the shell
        # has not yet read. A short script is read in one gulp and cannot show
        # the defect at all -- a control that passes for that reason would make
        # the real check vacuous.
        for i in $(seq 1 400); do
            printf ': "padding line %d aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"\n' "${i}"
        done
        printf 'sleep 2\n'
        for i in $(seq 401 800); do
            printf ': "padding line %d aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"\n' "${i}"
        done
        printf 'printf "FINISHED\\n" >> "%s"\n' "$3"
    } > "${path}"
    chmod +x "${path}"
}

# Rewrite the script's file while it runs, the way an editor does: insert a
# large block near the top so every later byte offset shifts.
corrupt_in_place() {
    local path="$1"
    python3 - "${path}" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split('\n')
inject = ['echo "INJECTED LINE THAT SHIFTS EVERY LATER BYTE OFFSET %d"' % i
          for i in range(200)]
out = lines[:2] + inject + lines[2:]
open(p, 'w').write('\n'.join(out))
PY
}

run_and_corrupt() {
    # $1 = mode; prints the marker file's contents
    local mode="$1"
    local script="${WORK}/standin_${mode}.sh"
    local marker="${WORK}/marker_${mode}"
    : > "${marker}"
    write_standin "${script}" "${mode}" "${marker}"

    "${script}" >/dev/null 2>&1 &
    local pid=$!
    # Let it get past START and into the sleep, with its offset parked deep in
    # the file.
    sleep 1
    corrupt_in_place "${script}"
    wait "${pid}" 2>/dev/null
    tr '\n' ' ' < "${marker}"
}

# THE CONTROL, run first and deliberately reported first: an unguarded script
# must NOT reach its end after the edit. If this passes, bash is buffering
# differently than assumed on this machine and the guarded check below cannot
# distinguish anything -- so the control's failure is what gives the real check
# meaning.
bare_out="$(run_and_corrupt bare)"
check "CONTROL: an UNGUARDED script does not survive being edited mid-run" "no" \
    "$(grep -q 'FINISHED' <<<"${bare_out}" && echo yes || echo no)"
check "CONTROL: ...and it did start, so the run was real" "yes" \
    "$(grep -q 'START' <<<"${bare_out}" && echo yes || echo no)"

guarded_out="$(run_and_corrupt guarded)"
check "a script that re-execs from a private copy DOES survive" "yes" \
    "$(grep -q 'FINISHED' <<<"${guarded_out}" && echo yes || echo no)"

# --- the real mutate.sh actually adopts the mechanism -------------------------
#
# Everything above is about bash. This is about the file that matters. Without
# these checks the suite could be entirely green while mutate.sh never grew the
# guard -- the seam tested, nothing built on it.

check "mutate.sh re-execs from a private copy" "yes" \
    "$(grep -q 'FLOWVIZ_MUTATE_REEXEC' "${MUTATE}" && echo yes || echo no)"

# The re-exec must happen BEFORE the script does anything destructive.
# Re-execing after the backup, or after an arm is applied, protects the part of
# the run that is already over.
# Match the `exec` itself, not the env assignments beside it: they are set on
# their own continuation lines, so a pattern requiring both on one line finds
# nothing and reports "no re-exec" about a script that has one.
reexec_line="$(grep -n '^[[:space:]]*exec "' "${MUTATE}" | head -1 | cut -d: -f1)"
backup_line="$(grep -n '^cp "\${SRC}" "\${BACKUP}"' "${MUTATE}" | head -1 | cut -d: -f1)"
marker_line="$(grep -n '^MARKER=' "${MUTATE}" | head -1 | cut -d: -f1)"

check "the re-exec is found in mutate.sh" "yes" \
    "$([[ -n "${reexec_line}" ]] && echo yes || echo no)"
check "...and happens BEFORE the source is backed up" "yes" \
    "$([[ -n "${reexec_line}" && -n "${backup_line}" && "${reexec_line}" -lt "${backup_line}" ]] && echo yes || echo no)"
check "...and BEFORE the mutation marker is declared" "yes" \
    "$([[ -n "${reexec_line}" && -n "${marker_line}" && "${reexec_line}" -lt "${marker_line}" ]] && echo yes || echo no)"

# --- how the private copy may be cleaned up ----------------------------------
#
# The obvious cleanup -- unlink the copy immediately after exec'ing it, so no
# temp file can ever leak -- is WRONG ON THIS PLATFORM, and wrong in the
# direction that kills the campaign outright. Measured here on macOS 26.5.2 /
# arm64, three cases, same script:
#
#   A. rm immediately after launch              -> exit 137 (SIGKILL), no output
#   B. rm one second in, already running        -> exit 0, ran to completion
#   C. no rm at all (control)                   -> exit 0, ran to completion
#
# So the POSIX "an open fd keeps the inode alive" intuition does not cover the
# exec/page-in window: unlink the image before the kernel has finished mapping
# it and the process is killed, not merely orphaned. B and C together show the
# kill is caused by the TIMING and not by the unlink itself -- without C, a
# broken probe would look identical.
#
# These two checks re-measure that on whatever machine is running the suite
# rather than trusting the comment. If a future macOS closes the window, A
# starts passing and this check goes red -- which is the correct outcome: the
# constraint mutate.sh is built around would no longer be real, and someone
# should find out from a red test rather than from a dead campaign.
probe_rm() {
    # $1 = seconds to wait before rm ("0" = immediately)
    local delay="$1" out=""
    out="$(
        t="$(mktemp -t selfprobe)"
        printf '#!/usr/bin/env bash\nsleep 2\nprintf OK\n' > "${t}"
        chmod +x "${t}"
        "${t}" & p=$!
        [[ "${delay}" != "0" ]] && sleep "${delay}"
        rm -f "${t}"
        wait "${p}" 2>/dev/null
    )"
    [[ "${out}" == "OK" ]] && echo survived || echo killed
}
check "unlinking the private copy IMMEDIATELY after exec kills it" \
    "killed" "$(probe_rm 0)"
check "CONTROL: unlinking it once it is running is safe" \
    "survived" "$(probe_rm 1)"

# Therefore the copy must be removed on the way OUT, not on the way in. It must
# also actually be removed -- a campaign that leaks a copy of itself per run is
# a smaller problem than a dead campaign, but it is still one.
check "mutate.sh removes its private copy in a trap, not right after exec" "yes" \
    "$(grep -q 'rm -rf "\${FLOWVIZ_MUTATE_SELFDIR' "${MUTATE}" && echo yes || echo no)"

# --- what the re-exec must NOT disturb ---------------------------------------
#
# exec replaces the process image and KEEPS the pid. Both the mutation marker
# (pid=$$) and the build token (FLOWVIZ_MUTATION_TOKEN=$$) are that pid, and
# mutation_window.sh compares them for exact equality -- so if the re-exec ever
# became a fork instead of an exec, the campaign would declare one pid and hand
# its builds another, and every build it spawned would refuse itself. That
# deadlock would present as "the build is broken", not as "the guard is
# misconfigured", so it is pinned here.
pid_probe="$(
    s="${WORK}/pidprobe.sh"
    cat > "${s}" <<'PROBE'
#!/usr/bin/env bash
if [[ -z "${PROBE_REEXEC:-}" ]]; then
    d="$(mktemp -d -t probeself)"
    cp "$0" "${d}/pidprobe.sh"
    chmod +x "${d}/pidprobe.sh"
    PROBE_REEXEC=1 PROBE_BEFORE="$$" exec "${d}/pidprobe.sh" "$@"
fi
[[ "$$" == "${PROBE_BEFORE}" ]] && printf same || printf "differs(${PROBE_BEFORE}->$$)"
PROBE
    chmod +x "${s}"
    "${s}"
)"
check "\$\$ is preserved across the re-exec, so marker pid == build token" \
    "same" "${pid_probe}"

# The re-exec must not make a running campaign invisible to `pgrep -f mutate.sh`.
# That is how a human, and the edit-path refusal in mutation_guard.sh, ask "is a
# campaign live in this tree?" -- and a copy named mutate_self.XXXXX would answer
# no while one was running. A guard that cannot see the thing it guards against
# is the has-no-callers failure applied to a safety check.
#
# Observed BEHAVIOURALLY rather than by grepping for a path spelling: the real
# mutate.sh is run with no arguments under `bash -x`, so it performs its actual
# re-exec and then dies on the usage line two statements later, before it has
# read a source file, taken a backup, or declared a marker. What the trace shows
# is the path the real script really exec'd, not the path this test guessed it
# would.
exec_target="$(bash -x "${MUTATE}" 2>&1 >/dev/null | grep -E '^\+ exec ' | head -1 | sed -E 's/^\+ exec //; s/^"|"$//g')"
check "the re-exec target is observable in a trace of the real script" "yes" \
    "$([[ -n "${exec_target}" ]] && echo yes || echo no)"
check "the private copy is still named mutate.sh (pgrep -f mutate.sh keeps working)" "mutate.sh" \
    "$(basename "${exec_target:-none}")"
check "...and it lives outside the checkout, so an edit there cannot reach it" "yes" \
    "$([[ -n "${exec_target}" && "${exec_target}" != "${TOOLS}"/* ]] && echo yes || echo no)"

# Running it with no arguments must NOT have left a marker or a live mutation
# behind: the re-exec happens first, so a mistake in its placement would show up
# as a campaign that declared a window and then exited on the usage error --
# leaving every later build in this checkout refused with no campaign to blame.
_gitdir="$(git -C "${TOOLS}" rev-parse --absolute-git-dir 2>/dev/null || echo /nonexistent)"
check "a usage error after the re-exec leaves no mutation marker behind" "no" \
    "$([[ -e "${_gitdir}/FLOWVIZ_MUTATION_ACTIVE" ]] && echo yes || echo no)"

# And it must not leak a private copy per invocation.
check "the trace run cleaned up its private copy" "gone" \
    "$([[ -n "${exec_target}" && -e "${exec_target}" ]] && echo present || echo gone)"

# A campaign started from a RELATIVE path must still work after the re-exec:
# the copy lives in a temp dir, so anything resolved from $0's directory would
# resolve to the wrong place. mutate.sh derives PROJECT_DIR from BASH_SOURCE,
# so the re-exec must pass the original location through rather than let the
# copy's own path be used.
check "mutate.sh passes its original directory through the re-exec" "yes" \
    "$(grep -q 'FLOWVIZ_MUTATE_ORIGIN' "${MUTATE}" && echo yes || echo no)"

# --- harness drift: the hazard the fix above CREATES --------------------------
#
# Running from a private copy removes the corruption. It does not remove the
# reason someone edits mutate.sh mid-campaign -- and it makes the consequence
# QUIETER, which is the worse direction.
#
# Before: you fix a scoring bug in mutate.sh while a campaign runs, and the
# campaign dies or garbles. Loud, and you go and re-run it.
# After: you fix the same bug, the campaign sails on using the OLD code, and
# prints verdicts you then attribute to the fixed harness. Nothing about that
# output looks wrong. This repo has already paid for that shape once, from the
# other end -- a hook suite that ran 12/12 green against source while the
# installed copy was still the pre-fix version.
#
# So the campaign records a fingerprint of the script it is actually running and
# checks it at the end. The report is the campaign's own, not a hook's: there is
# no edit event to intercept, and a note in a doc is not a check.
source "${TOOLS}/mutation_window.sh"

drift_marker="${WORK}/drift_marker"
{ echo "pid=1234"; echo "source=Foo.cpp"; echo "harness_sha=abc123"; } > "${drift_marker}"

check "an unchanged harness reports no drift" "same" \
    "$(check_harness_drift "${drift_marker}" "abc123")"
check "an edited harness reports drift" "drifted" \
    "$(check_harness_drift "${drift_marker}" "def456")"

# A marker with no fingerprint at all -- written by a campaign that started
# before this field existed -- must not report "same". Unknown is not a clean
# bill of health; that is the same fail-open shape as reading a missing summary
# as a pass.
{ echo "pid=1234"; echo "source=Foo.cpp"; } > "${drift_marker}"
check "a marker with no recorded fingerprint reports unknown, not same" "unknown" \
    "$(check_harness_drift "${drift_marker}" "abc123")"
check "an absent marker reports unknown" "unknown" \
    "$(check_harness_drift "${WORK}/no_such_marker" "abc123")"
# An empty computed fingerprint means shasum failed or the file vanished. It
# must not silently equal an empty recorded one and report "same".
{ echo "pid=1234"; echo "harness_sha="; } > "${drift_marker}"
check "an empty fingerprint on both sides is unknown, not same" "unknown" \
    "$(check_harness_drift "${drift_marker}" "")"

# The real mutate.sh must record the fingerprint and check it.
check "mutate.sh records its harness fingerprint in the marker" "yes" \
    "$(grep -q 'harness_sha=' "${MUTATE}" && echo yes || echo no)"
check "...and reports drift when the campaign finishes" "yes" \
    "$(grep -q 'check_harness_drift' "${MUTATE}" && echo yes || echo no)"

# END-TO-END: edit the checked-out mutate.sh under a running campaign and
# confirm BOTH halves -- the run completes (no corruption) AND it says the file
# it ran from is no longer what is on disk. A guard that only proves the first
# half is how the quiet failure ships.
e2e="$(
    REPO="${WORK}/e2e"
    mkdir -p "${REPO}/FlowViz/Tools"
    cp "${TOOLS}/mutation_window.sh" "${REPO}/FlowViz/Tools/"
    S="${REPO}/FlowViz/Tools/mutate.sh"
    cat > "${S}" <<'CAMPAIGN'
#!/usr/bin/env bash
set -uo pipefail
if [[ -z "${FLOWVIZ_MUTATE_REEXEC:-}" ]]; then
    _o="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    _d="$(mktemp -d -t e2eself)"
    cat "${BASH_SOURCE[0]}" > "${_d}/mutate.sh"; chmod +x "${_d}/mutate.sh"
    FLOWVIZ_MUTATE_REEXEC=1 FLOWVIZ_MUTATE_ORIGIN="${_o}" FLOWVIZ_MUTATE_SELFDIR="${_d}" \
        exec "${_d}/mutate.sh" "$@"
fi
TOOLS="${FLOWVIZ_MUTATE_ORIGIN}"
source "${TOOLS}/mutation_window.sh"
SELF_SHA="$(shasum -a 256 < "$0" | cut -d' ' -f1)"
M="${TOOLS}/marker"
{ echo "pid=$$"; echo "harness_sha=${SELF_SHA}"; } > "${M}"
sleep 3
echo "COMPLETED"
echo "drift=$(check_harness_drift "${M}" "$(shasum -a 256 < "${TOOLS}/mutate.sh" | cut -d' ' -f1)")"
rm -rf "${FLOWVIZ_MUTATE_SELFDIR}"
CAMPAIGN
    chmod +x "${S}"
    "${S}" > "${WORK}/e2e_out" 2>&1 &
    p=$!
    sleep 1
    printf '\n# an edit landing mid-campaign\n' >> "${S}"
    wait "${p}" 2>/dev/null
    tr '\n' ' ' < "${WORK}/e2e_out"
)"
check "E2E: the campaign completes despite its file being edited mid-run" "yes" \
    "$(grep -q 'COMPLETED' <<<"${e2e}" && echo yes || echo no)"
check "E2E: ...and it reports that the on-disk harness drifted" "yes" \
    "$(grep -q 'drift=drifted' <<<"${e2e}" && echo yes || echo no)"

echo
echo "mutate self-copy tests: $((checks - failures)) passed, ${failures} failed"
exit $(( failures > 0 ? 1 : 0 ))
