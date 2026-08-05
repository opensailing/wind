#!/bin/bash
# Known-answer tests for check_unity_collisions.sh.
#
# WHY THIS EXISTS
#
# The checker's job is to answer "will these files collide when UBT folds them
# into one unity blob". A checker that always answers "clean" passes every tree
# and is worse than nothing, because it is READ as evidence. So the cases below
# lead with the ones that must come back RED, and every green case is paired
# with a red one over nearly identical input.
#
# THE TWO REAL DEFECTS, both measured 2026-08-05 on this repo:
#
#   1. Anonymous namespaces. Two `namespace { }` blocks in the same TU are the
#      SAME namespace, so identically named helpers are a redefinition. Commit
#      8b27be9 shipped three such pairs and left HEAD unbuildable.
#
#   2. `using namespace <File>Local;`. The rename to a named namespace fixes (1),
#      but a file-scope `using` re-exports every name back into the enclosing TU.
#      Verified against clang: the result is `call to 'X' is ambiguous`, listing
#      both namespaces as candidates. The hazard survives in a form the source
#      code appears to deny -- the comment above the namespace names the exact
#      problem it no longer prevents.
#
# Case 2 is why this file exists at all. A checker that only understood case 1
# would have called the "fixed" tree clean.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UNDER_TEST="${TOOLS_DIR}/check_unity_collisions.sh"

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

# The checker refuses to report a pass on an implausibly small extraction, so
# these fixtures need enough padding symbols to clear the floor. Padding is
# per-file unique; only the symbols each case is about are shared.
pad() {  # pad <file> <tag> <count>
    local f="$1" tag="$2" n="$3" i
    for ((i = 0; i < n; i++)); do
        printf '\tbool %s_Pad%d(double V)\n\t{\n\t\treturn V == V;\n\t}\n\n' "${tag}" "${i}" >> "${f}"
    done
}

# anon_file <path> <tag> <shared-symbol-or-empty> <pad-count>
anon_file() {
    local f="$1" tag="$2" shared="$3" n="$4"
    printf '#include "X.h"\n\nnamespace\n{\n' > "${f}"
    [[ -n "${shared}" ]] && printf '\tbool %s(double V)\n\t{\n\t\treturn V == V;\n\t}\n\n' "${shared}" >> "${f}"
    pad "${f}" "${tag}" "${n}"
    printf '}\n\nvoid %s_Use() {}\n' "${tag}" >> "${f}"
}

# named_file <path> <tag> <shared-symbol-or-empty> <pad-count> <emit-using: yes|no>
named_file() {
    local f="$1" tag="$2" shared="$3" n="$4" emit_using="$5"
    printf '#include "X.h"\n\nnamespace %sLocal\n{\n' "${tag}" > "${f}"
    [[ -n "${shared}" ]] && printf '\tbool %s(double V)\n\t{\n\t\treturn V == V;\n\t}\n\n' "${shared}" >> "${f}"
    pad "${f}" "${tag}" "${n}"
    printf '}\n\n' >> "${f}"
    [[ "${emit_using}" == "yes" ]] && printf 'using namespace %sLocal;\n\n' "${tag}" >> "${f}"
    printf 'void %s_Use() {}\n' "${tag}" >> "${f}"
}

run_on() {  # run_on <dir> -> "<exit>:<clean|collision|unscored>"
    local out rc
    out="$("${UNDER_TEST}" "$1" 2>&1)"
    rc=$?
    local kind="unknown"
    printf '%s' "${out}" | grep -q 'UNITY-BUILD COLLISION' && kind="collision"
    printf '%s' "${out}" | grep -q 'no cross-file duplicates' && kind="clean"
    printf '%s' "${out}" | grep -q 'UNSCORED' && kind="unscored"
    printf '%s:%s' "${rc}" "${kind}"
}

# --- ANTI-VACUITY: the checker must be able to say BOTH things ----------------
#
# If it answered "collision" to everything it would pass every red case below,
# and if it answered "clean" to everything it would pass every green one. Each
# pair differs only in whether one symbol name is shared.

D="${WORK}/anon_collide"; mkdir -p "${D}"
anon_file "${D}/A.cpp" A IsFiniteVector 25
anon_file "${D}/B.cpp" B IsFiniteVector 25
check "anonymous namespaces sharing a helper name are a COLLISION" \
    "1:collision" "$(run_on "${D}")"

D="${WORK}/anon_clean"; mkdir -p "${D}"
anon_file "${D}/A.cpp" A "" 25
anon_file "${D}/B.cpp" B "" 25
check "anonymous namespaces with no shared name are CLEAN" \
    "0:clean" "$(run_on "${D}")"

# --- The defect the rename left behind ----------------------------------------
#
# Named namespaces do not collide by themselves. A file-scope `using namespace`
# re-exports them, and clang then reports the call as ambiguous. These two cases
# differ ONLY in the presence of that one line.

D="${WORK}/named_using"; mkdir -p "${D}"
named_file "${D}/A.cpp" A IsFiniteVector 25 yes
named_file "${D}/B.cpp" B IsFiniteVector 25 yes
check "named namespaces re-exported with 'using namespace' still COLLIDE" \
    "1:collision" "$(run_on "${D}")"

D="${WORK}/named_qualified"; mkdir -p "${D}"
named_file "${D}/A.cpp" A IsFiniteVector 25 no
named_file "${D}/B.cpp" B IsFiniteVector 25 no
check "named namespaces WITHOUT 'using' are clean even sharing a name" \
    "0:clean" "$(run_on "${D}")"

# --- An empty match set must not read as a pass -------------------------------
#
# The failure this guards: a scan that extracts nothing reports success in
# exactly the words it uses for a clean tree. Measured on this very script --
# `mapfile` does not exist in macOS's bash 3.2, so the file list came back empty
# and every per-symbol test passed vacuously. Only the plausibility floor caught
# it.

D="${WORK}/no_namespaces"; mkdir -p "${D}"
printf '#include "X.h"\n\nvoid Plain() {}\n' > "${D}/A.cpp"
check "a tree with no namespaces at all is UNSCORED, not clean" \
    "2:unscored" "$(run_on "${D}")"

D="${WORK}/too_few"; mkdir -p "${D}"
anon_file "${D}/A.cpp" A "" 2
check "an implausibly small extraction is UNSCORED, not clean" \
    "2:unscored" "$(run_on "${D}")"

check "a missing directory is an error, not a pass" \
    "2:unknown" "$(run_on "${WORK}/does_not_exist")"

# --- Three-way sharing, as the real tree had ----------------------------------

D="${WORK}/three_way"; mkdir -p "${D}"
anon_file "${D}/A.cpp" A IsFiniteVector 20
anon_file "${D}/B.cpp" B IsFiniteVector 20
anon_file "${D}/C.cpp" C IsFiniteVector 20
OUT="$("${UNDER_TEST}" "${D}" 2>&1)"
check "a name shared by three files names all three" \
    "3" "$(printf '%s' "${OUT}" | grep -c '\.cpp$')"

# --- Mixed forms: an anonymous helper and a re-exported named one -------------
#
# The real tree is mid-migration and holds both styles at once, so the two
# extractors have to agree with each other rather than each covering half.

D="${WORK}/mixed"; mkdir -p "${D}"
anon_file  "${D}/A.cpp" A IsFiniteVector 25
named_file "${D}/B.cpp" B IsFiniteVector 25 yes
check "an anonymous helper collides with a re-exported named one" \
    "1:collision" "$(run_on "${D}")"

printf '\n%d passed, %d failed\n' "${PASS}" "${FAIL}"
[[ "${FAIL}" -eq 0 ]]
