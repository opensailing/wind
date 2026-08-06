#!/bin/bash
# check_shell_portability.sh must find bash-4-only builtins in a tree of scripts.
#
# --- what happened ------------------------------------------------------------
#
# macOS ships /bin/bash 3.2.57 (frozen at the last GPLv2 release). `mapfile`,
# `readarray`, `declare -A` and `${x^^}` all arrived in bash 4. A script with
# `#!/bin/bash` therefore CANNOT use them on this machine -- but the same file
# run as `bash script.sh` picks up /opt/homebrew/bin/bash 5.3.9 from PATH and
# works perfectly.
#
# run_harness_tests.sh invokes every test as `cmd=(bash "${path}")` (line 136),
# deliberately, so a `chmod -x` cannot silently drop a test. The side effect is
# that it ALSO overrides every test's shebang. So a test file can depend on
# bash 4, pass in every harness sweep, and die the moment anyone runs it
# directly -- which is what a person does when debugging one failing test.
#
# Measured 2026-08-06, test_mutate_per_arm_logs.sh:
#
#     $ ./test_mutate_per_arm_logs.sh
#     line 258: mapfile: command not found
#     line 261: ARM_TEST_LOGS: unbound variable
#
# --- why a checker and not four edits -----------------------------------------
#
# This hazard was already known. It is written down in three separate places:
#
#     check_unity_collisions.sh:57       "Not `mapfile`: macOS ships bash 3.2"
#     test_check_unity_collisions.sh:133 "...the file list came back empty"
#     test_check_frozen_params.sh:330    "the bash-3.2 mapfile failure that made
#                                         every unity test pass vacuously"
#
# Three comments, one of them recording that this exact bug once made an entire
# test file pass VACUOUSLY -- and the defect recurred anyway, four more times.
# Prose that describes a hazard does not prevent it. Nothing in the harness
# rejected the construct, so every new script started the trap fresh.
#
# --- the two failure directions are not equally loud --------------------------
#
# run_harness_tests.sh:96 uses mapfile to DISCOVER tests. Under bash 3.2 FOUND
# comes back empty -- and its "no tests found" branch exits 1 with an explicit
# message. That one fails closed.
#
# test_mutate_per_arm_logs.sh uses it to COUNT results. Under bash 3.2 the
# array is unbound, checks that should compare 3 against 3 compare 3 against
# nothing, and the file keeps going. That one fails open, which is the
# direction that ships.
#
# --- what this checker must NOT do --------------------------------------------
#
# The three comments above contain the literal word `mapfile`. A checker that
# grepped for the word would flag the very prose warning against it, the real
# tree could never read clean, and the fix could never be confirmed. Telling
# code from commentary is the whole job -- so the fixtures below assert both
# directions, and a comment mentioning a builtin is asserted CLEAN.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UNDER_TEST="${TOOLS_DIR}/check_shell_portability.sh"

PASS=0
FAIL=0

check() {  # check <label> <expected> <actual>
    if [[ "$2" == "$3" ]]; then
        printf '  ok   %s\n' "$1"
        PASS=$((PASS + 1))
    else
        printf '  FAIL %s\n         expected: %s\n         actual:   %s\n' "$1" "$2" "$3"
        FAIL=$((FAIL + 1))
    fi
}

[[ -f "${UNDER_TEST}" ]] || { echo "no ${UNDER_TEST}"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# The checker refuses to report a pass on an implausibly small scan, so fixture
# trees are run with the floor lowered. The floor itself is asserted separately
# below -- a threshold nothing tests is a threshold that can drift to zero.
run_on() {  # run_on <dir> [floor] -> "<exit>:<clean|violation|unscored|unknown>"
    local out rc
    out="$(MIN_PLAUSIBLE_SCRIPTS="${2:-1}" "${UNDER_TEST}" "$1" 2>&1)"
    rc=$?
    local kind="unknown"
    printf '%s' "${out}" | grep -q 'NOT PORTABLE'   && kind="violation"
    printf '%s' "${out}" | grep -q 'portable to bash 3.2' && kind="clean"
    printf '%s' "${out}" | grep -q 'UNSCORED'       && kind="unscored"
    printf '%s:%s' "${rc}" "${kind}"
}

# Padding so a fixture tree can clear a floor when the case is about the floor.
pad() {  # pad <dir> <count>
    local d="$1" n="$2" i
    for (( i = 0; i < n; i++ )); do
        printf '#!/bin/bash\necho pad%d\n' "${i}" > "${d}/pad${i}.sh"
    done
}

# =============================================================================
# ANTI-VACUITY: the checker must be able to say BOTH things
# =============================================================================
#
# Asserted before anything else. A checker that always says "clean" passes
# every clean fixture below it, and a checker that always says "violation"
# passes every dirty one. Neither is a check until both directions are shown.

D="${WORK}/portable"; mkdir -p "${D}"
cat > "${D}/fine.sh" <<'EOF'
#!/bin/bash
FILES=()
while IFS= read -r f; do
    [[ -n "${f}" ]] && FILES+=("${f}")
done < <(find . -name '*.cpp')
echo "${#FILES[@]}"
EOF
check "CONTROL: a bash-3.2-safe tree reads clean" \
    "0:clean" "$(run_on "${D}")"

D="${WORK}/mapfile"; mkdir -p "${D}"
cat > "${D}/bad.sh" <<'EOF'
#!/bin/bash
mapfile -t FILES < <(find . -name '*.cpp')
echo "${#FILES[@]}"
EOF
check "CONTROL: mapfile under a bash-3.2 shebang is a violation" \
    "1:violation" "$(run_on "${D}")"

# =============================================================================
# THE FALSE-POSITIVE DIRECTION -- prose about the hazard is not the hazard
# =============================================================================
#
# This is the case that decides whether the checker is usable at all. The real
# tree documents this trap in three comments that name `mapfile` outright. If
# those read as violations, the tree can never go green, and a checker that
# cannot report the goal state is one nobody will keep.

D="${WORK}/comment"; mkdir -p "${D}"
cat > "${D}/documented.sh" <<'EOF'
#!/bin/bash
# Not `mapfile`: macOS ships bash 3.2, where it does not exist.
# Nor readarray, and no declare -A either.
FILES=()
while IFS= read -r f; do   # mapfile would be shorter, and would not run here
    FILES+=("${f}")
done < <(find . -name '*.cpp')
EOF
check "a comment naming a bash-4 builtin is NOT a violation" \
    "0:clean" "$(run_on "${D}")"

# A trailing comment on a real command line, and a `#` that is NOT a comment.
D="${WORK}/hashes"; mkdir -p "${D}"
cat > "${D}/hashes.sh" <<'EOF'
#!/bin/bash
N="${#ARR[@]}"          # array length, not a comment marker
S="${VAR#prefix}"       # prefix strip
echo "count: $# args"   # mapfile mentioned only here
grep '#!/bin/bash' file
EOF
check "a # inside an expansion does not hide the rest of the line" \
    "0:clean" "$(run_on "${D}")"

# The mirror of the case above: a violation with a comment on the SAME line
# must still be caught. Stripping comments must not strip the code beside them.
D="${WORK}/trailing"; mkdir -p "${D}"
cat > "${D}/trailing.sh" <<'EOF'
#!/bin/bash
mapfile -t X < <(ls)    # collect the listing
EOF
check "a violation with a trailing comment is still a violation" \
    "1:violation" "$(run_on "${D}")"

# A name that merely CONTAINS a builtin's name is not that builtin.
D="${WORK}/lookalike"; mkdir -p "${D}"
cat > "${D}/lookalike.sh" <<'EOF'
#!/bin/bash
mapfile_compat() { while IFS= read -r l; do echo "${l}"; done; }
readarray_result=0
echo "${mapfile_compat_output:-}"
EOF
check "a function named mapfile_compat is not a call to mapfile" \
    "0:clean" "$(run_on "${D}")"

# =============================================================================
# COVERAGE -- every bash-4 construct, not just the one that bit us
# =============================================================================
#
# mapfile is what recurred four times, but a checker that only knows mapfile
# leaves the next author to discover declare -A the same way. Each is asserted
# on its own so a partial implementation cannot pass on the strength of one.

D="${WORK}/readarray"; mkdir -p "${D}"
printf '#!/bin/bash\nreadarray -t X < file\n' > "${D}/a.sh"
check "readarray is caught" "1:violation" "$(run_on "${D}")"

D="${WORK}/assoc"; mkdir -p "${D}"
printf '#!/bin/bash\ndeclare -A SEEN\nSEEN[x]=1\n' > "${D}/a.sh"
check "declare -A is caught" "1:violation" "$(run_on "${D}")"

D="${WORK}/assoc_local"; mkdir -p "${D}"
printf '#!/bin/bash\nf() { local -A M; M[k]=v; }\n' > "${D}/a.sh"
check "local -A is caught" "1:violation" "$(run_on "${D}")"

D="${WORK}/case_expand"; mkdir -p "${D}"
printf '#!/bin/bash\nX=abc\necho "${X^^}"\n' > "${D}/a.sh"
check 'a ${x^^} case expansion is caught' "1:violation" "$(run_on "${D}")"

D="${WORK}/pipe_both"; mkdir -p "${D}"
printf '#!/bin/bash\nmake |& tee log\n' > "${D}/a.sh"
check "a |& pipe is caught" "1:violation" "$(run_on "${D}")"

# =============================================================================
# THE SHEBANG IS NOT AN EXCUSE
# =============================================================================
#
# `#!/usr/bin/env bash` finds whatever bash comes first on PATH. On this
# machine that is Homebrew's 5.3.9, so these constructs work TODAY. They stop
# working on any box without Homebrew bash -- CI, a fresh checkout, a login
# shell with a trimmed PATH -- and they stop working silently, because
# `env bash` will happily fall through to /bin/bash 3.2.
#
# run_harness_tests.sh:96 is exactly this: mapfile under `#!/usr/bin/env bash`,
# working entirely by PATH accident. The guaranteed floor on macOS is 3.2, so
# the rule is the same for both shebangs.

D="${WORK}/env_shebang"; mkdir -p "${D}"
printf '#!/usr/bin/env bash\nmapfile -t X < file\n' > "${D}/a.sh"
check "env-bash does not exempt a file -- PATH is not a guarantee" \
    "1:violation" "$(run_on "${D}")"

# A non-bash script is genuinely not our business. sh scripts never had these
# builtins and python has no opinion about them.
D="${WORK}/notbash"; mkdir -p "${D}"
printf '#!/usr/bin/env python3\nmapfile = "declare -A"\n' > "${D}/a.py"
printf '#!/bin/bash\necho ok\n' > "${D}/real.sh"
check "a python file is not scanned for shell builtins" \
    "0:clean" "$(run_on "${D}")"

# =============================================================================
# REPORTING -- a violation must be findable, not just counted
# =============================================================================

D="${WORK}/report"; mkdir -p "${D}/sub"
printf '#!/bin/bash\necho fine\n' > "${D}/clean.sh"
printf '#!/bin/bash\n\n\nmapfile -t X < file\n' > "${D}/sub/dirty.sh"
REPORT="$(MIN_PLAUSIBLE_SCRIPTS=1 "${UNDER_TEST}" "${D}" 2>&1)"
check "the report names the offending file" "yes" \
    "$(grep -q 'dirty.sh' <<<"${REPORT}" && echo yes || echo no)"
check "the report gives the line number" "yes" \
    "$(grep -qE 'dirty\.sh:4|:4:' <<<"${REPORT}" && echo yes || echo no)"
check "the report names the construct" "yes" \
    "$(grep -q 'mapfile' <<<"${REPORT}" && echo yes || echo no)"
check "the clean file is not named as an offender" "no" \
    "$(grep -qE '^[^#]*clean\.sh:[0-9]' <<<"${REPORT}" && echo yes || echo no)"
check "subdirectories are scanned, not just the top level" "yes" \
    "$(grep -q 'sub/dirty.sh' <<<"${REPORT}" && echo yes || echo no)"

# =============================================================================
# AN EMPTY MATCH SET MUST NOT READ AS A PASS
# =============================================================================
#
# The way this checker dies silently is a discovery step that finds no files:
# zero scripts scanned, zero violations, "clean". Identical wording to a
# genuinely portable tree. A wrong path, a rename, or a find that errors out
# all produce it -- so the count is printed and floored.

D="${WORK}/empty"; mkdir -p "${D}"
check "a directory with no shell scripts is UNSCORED, not clean" \
    "2:unscored" "$(run_on "${D}")"

check "a missing directory is an error, not a pass" \
    "2:unknown" "$(run_on "${WORK}/does_not_exist")"

# The floor must be able to reject. Asserted with a REAL number rather than the
# lowered fixture default, because a threshold that is only ever run at 1 is a
# threshold that would pass at 0.
D="${WORK}/floor"; mkdir -p "${D}"
pad "${D}" 3
check "a scan far below the plausible floor is UNSCORED, not clean" \
    "2:unscored" "$(run_on "${D}" 25)"
check "the same tree above the floor reads clean" \
    "0:clean" "$(run_on "${D}" 3)"

# The floor must not be able to mask a violation: a tree that is BOTH too small
# and not portable must report the violation, not hide behind UNSCORED.
D="${WORK}/floor_and_dirty"; mkdir -p "${D}"
printf '#!/bin/bash\nmapfile -t X < file\n' > "${D}/a.sh"
check "a violation under a too-high floor is still reported" \
    "1:violation" "$(run_on "${D}" 25)"

# =============================================================================
# THE REAL TREE
# =============================================================================
#
# The point of the whole exercise. Run at the checker's own default floor -- if
# the default were nonsense, every fixture above would still pass, since they
# all override it.

REAL="$("${UNDER_TEST}" "${TOOLS_DIR}" 2>&1)"
REAL_RC=$?
check "Tools/ is portable to the bash the shebangs actually name" "0" "${REAL_RC}"
if [[ "${REAL_RC}" -ne 0 ]]; then
    printf '%s\n' "${REAL}" | sed 's/^/         | /'
fi

# The scan must be big enough to be meaningful, and this asserts the default
# floor is doing work on the real tree rather than being trivially cleared.
SCANNED="$(printf '%s' "${REAL}" | grep -oE 'scanned [0-9]+' | grep -oE '[0-9]+' | tail -1)"
check "the real scan reached a plausible number of scripts" "yes" \
    "$([[ -n "${SCANNED}" && "${SCANNED}" -ge 20 ]] && echo yes || echo no)"

# Every test in this directory must run under its OWN shebang, not only under
# the runner's `bash`. This is the assertion that would have caught the defect
# in the first place: the harness passes either way, a human does not.
DIRECT_FAILS=0
DIRECT_NAMES=""
for t in "${SCRIPT_DIR}"/test_*.sh; do
    shebang="$(head -1 "${t}")"
    interp="${shebang#\#!}"
    case "${interp}" in
        */env\ bash) interp="$(command -v bash)" ;;
        *)           interp="${interp%% *}" ;;
    esac
    [[ -x "${interp}" ]] || continue
    if ! "${interp}" -n "${t}" 2>/dev/null; then
        DIRECT_FAILS=$((DIRECT_FAILS + 1))
        DIRECT_NAMES="${DIRECT_NAMES} $(basename "${t}")"
    fi
done
check "every test parses under the interpreter its own shebang names" \
    "0" "${DIRECT_FAILS}"
[[ -n "${DIRECT_NAMES}" ]] && printf '         offenders:%s\n' "${DIRECT_NAMES}"

printf '\n%d passed, %d failed\n' "${PASS}" "${FAIL}"
[[ "${FAIL}" -eq 0 ]]
