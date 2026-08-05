#!/bin/bash
# Known-answer tests for compile_tu.sh -- the per-translation-unit compile check.
#
# WHY THIS EXISTS
#
# A fatal error in ONE file halts the whole UBT build after a single action. On
# 2026-08-05 an agent committed a TDD test including a header it had not written
# yet; the build died at action 1 and NOBODY could compile anything for an hour.
# Every other agent's work was unverifiable through no fault of its own.
#
# UBT has already written a response file per translation unit under
# Intermediate/Build/.../*.cpp.o.rsp holding that TU's exact flags. Replaying one
# through clang compiles that file ALONE, so a peer's broken file cannot mask
# your own state.
#
# WHAT THIS CANNOT DO, AND WHY THE SCRIPT SAYS SO OUT LOUD
#
# -o /dev/null on one TU checks COMPILATION, never LINKAGE. An undefined
# FLOWVIZRUNTIME_API symbol -- a declared-but-unimplemented export, which is
# exactly what blocked this module earlier the same day -- is invisible to every
# per-TU compile and appears only when the linker runs. A clean sweep here is
# NOT a substitute for `Result: Succeeded` with a nonzero action count.
#
# THE FAILURE THIS GUARDS AGAINST
#
# The check is only worth running if it can come back red. Two ways it silently
# cannot:
#   1. a missing/stale .rsp -- report UNSCORED, never "OK". A TU that was never
#      compiled must not score the same as one that compiled clean.
#   2. a broken result parser. Measured: `n=$(grep -c error: "$f" || echo 0)`
#      yields "0\n0" on zero matches, because grep -c PRINTS 0 and EXITS 1. Every
#      numeric test downstream then errors out and takes the else branch. That
#      scored 11 clean TUs as FAIL. Reversed, it would have been 11 false OKs.
#
# So compile_tu.sh runs a CONTROL before believing any pass: it injects a
# guaranteed error into a scratch copy and requires clang to reject it. If the
# control does not fail, the whole run is UNSCORED.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UNDER_TEST="${TOOLS_DIR}/compile_tu.sh"

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
# shellcheck source=/dev/null
source "${UNDER_TEST}" --source-only

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# --- classify_tu_result: the parser that scored 11 clean compiles as FAIL -----
#
# Anti-vacuity first. If classify_tu_result answered UNSCORED to everything it
# would pass every "must not say OK" case below, so a genuine clean compile has
# to come back OK or the rest of this file proves nothing.

check "clean compile is OK" \
    "ok" "$(classify_tu_result 0 "$(printf '')")"

check "compiler error is FAIL" \
    "fail" "$(classify_tu_result 1 "$(printf 'x.cpp:3:5: error: no member named Foo\n')")"

# The measured bug. grep -c prints 0 and exits 1, so a `|| echo 0` fallback
# yields TWO values; anything doing arithmetic on that errors and takes the
# wrong branch. Feeding the literal corrupt string must not score OK.
check "corrupt \"0\\n0\" error count does not score OK" \
    "unscored" "$(classify_tu_result 0 "$(printf '0\n0\n')" corrupt)"

# exit 0 with diagnostics on stderr: clang exits 0 for warnings. A warning is
# not a failure, or every -Wunused turns the tree red.
check "warnings with exit 0 stay OK" \
    "ok" "$(classify_tu_result 0 "$(printf 'x.cpp:9:1: warning: unused variable\n')")"

# The inverse of the above, and the one that matters: a nonzero exit with NO
# parseable error text is not a pass. That is how a killed process, an OOM, or a
# missing compiler looks.
check "nonzero exit with no error text is UNSCORED" \
    "unscored" "$(classify_tu_result 137 "$(printf '')")"

# --- resolve_rsp: a TU that was never compiled must not score like a clean one -

RSPDIR="${WORK}/rsp"
mkdir -p "${RSPDIR}"
printf -- '-c /some/File.cpp\n' > "${RSPDIR}/File.cpp.o.rsp"

check "an existing .rsp resolves" \
    "${RSPDIR}/File.cpp.o.rsp" "$(resolve_rsp "${RSPDIR}" File)"

check "a missing .rsp is empty, not a path" \
    "" "$(resolve_rsp "${RSPDIR}" NoSuchUnit)"

# CONTROL: resolve_rsp must not simply answer empty for everything -- that would
# make the missing-rsp case above pass for a broken implementation. Covered by
# the positive case above; this asserts the pair is genuinely discriminating.
check "resolve_rsp discriminates present from absent" \
    "different" \
    "$([[ "$(resolve_rsp "${RSPDIR}" File)" != "$(resolve_rsp "${RSPDIR}" NoSuchUnit)" ]] \
        && echo different || echo same)"

# --- classify_control_run: no control, no verdict ----------------------------
#
# The control injects a guaranteed error and REQUIRES clang to reject it. If the
# control compiles clean, the harness is not testing anything and every OK it
# produced is worthless.

check "control that fails to compile means the harness works" \
    "armed" "$(classify_control_run 1 "$(printf "x.cpp:9:1: error: no member named 'BogusXYZ'\n")")"

check "control that compiles CLEAN invalidates the run" \
    "broken" "$(classify_control_run 0 "$(printf '')")"

# A control that fails for the WRONG reason -- a missing header, say -- proves
# the compiler ran but not that it can see into the code under test. It must not
# arm the harness on a `file not found`.
check "control failing on a missing header does not arm the harness" \
    "broken" "$(classify_control_run 1 "$(printf "x.cpp:3:10: fatal error: 'Nope.h' file not found\n")")"

printf '\n%d passed, %d failed\n' "${PASS}" "${FAIL}"
[[ "${FAIL}" -eq 0 ]]
