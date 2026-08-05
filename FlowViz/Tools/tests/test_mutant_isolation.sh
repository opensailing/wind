#!/bin/bash
# Can two mutation campaigns run at once without corrupting each other?
#
# They could not. mutate.sh parsed its mutant file into a FIXED path,
# /tmp/mutate_records.tsv, and then read the loop's input from that same fixed
# path. Two campaigns in flight therefore shared one file: whichever parsed
# last won, and both loops read its records.
#
# This was observed, not theorised. A campaign launched against
# CFDVizPayload.cpp with a two-mutant set reported:
#
#     SKIP      skip the sub-region RHI update -- pattern not found
#     === CFDVizPayload.cpp vs FlowViz.CFDViz.KnownValues ===
#     killed 1  SURVIVED 0  INVALID 0  UNSCORED 0  skipped 2
#
# "skip the sub-region RHI update" is a GPU texture-upload mutant. It is not in
# the payload set, and the payload set's own second mutant never ran.
#
# It surfaced as a harmless SKIP only by luck -- the foreign pattern did not
# appear in the file being mutated. A foreign pattern that DID match would have
# been applied, built, tested and scored, and the verdict would have been
# printed under the wrong mutant's name in the wrong campaign's report. That is
# the same class of defect as scoring a crash as a kill: a verdict that looks
# exactly like a result and is about something else entirely.
#
# The fix is that parsing writes to stdout and the loop consumes it directly,
# so there is no shared filename to collide on. These tests pin the parser's
# behaviour, which is what makes that possible.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARSE="${SCRIPT_DIR}/../parse_mutants.py"

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

# --- two different sets, parsed concurrently, stay separate -----------------
#
# The regression test. Both are parsed at the same time, exactly as two
# campaigns would, and each output must contain only its own mutants.
cat > "${TMP}/alpha.txt" <<'EOF'
alpha one
--
find_alpha_one
--
replace_alpha_one
%%
alpha two
--
find_alpha_two
--
replace_alpha_two
EOF

cat > "${TMP}/beta.txt" <<'EOF'
beta one
--
find_beta_one
--
replace_beta_one
EOF

python3 "${PARSE}" "${TMP}/alpha.txt" > "${TMP}/alpha.tsv" &
ALPHA_PID=$!
python3 "${PARSE}" "${TMP}/beta.txt" > "${TMP}/beta.tsv" &
BETA_PID=$!
wait "${ALPHA_PID}"; wait "${BETA_PID}"

check "concurrent parse: alpha keeps both its own mutants" "2" \
    "$(wc -l < "${TMP}/alpha.tsv" | tr -d ' ')"
check "concurrent parse: beta keeps its one mutant" "1" \
    "$(wc -l < "${TMP}/beta.tsv" | tr -d ' ')"
check "concurrent parse: no beta mutant leaked into alpha" "0" \
    "$(grep -c 'beta' "${TMP}/alpha.tsv")"
check "concurrent parse: no alpha mutant leaked into beta" "0" \
    "$(grep -c 'alpha' "${TMP}/beta.tsv")"

# --- indentation is preserved -----------------------------------------------
#
# Every real mutant is indented C++ whose leading tabs must match the source
# exactly. A parser that stripped them would make every mutant stale -- and a
# stale mutant SKIPs, which silently shrinks a campaign rather than failing it.
printf 'indented\n--\n\t\tif (A && B)\n--\n\t\tif (A)\n' > "${TMP}/indent.txt"
check "leading indentation survives parsing" '"\t\tif (A && B)"' \
    "$(python3 "${PARSE}" "${TMP}/indent.txt" | cut -f2)"

# --- multi-line patterns survive --------------------------------------------
#
# The statistics-swap mutants are ~10 lines each. A parser that split on the
# wrong boundary would silently truncate them to the first line, which would
# then match in several places and be rejected as ambiguous.
printf 'multiline\n--\nline one\nline two\n--\nline two\nline one\n' > "${TMP}/multi.txt"
check "a multi-line pattern keeps its newline" '"line one\nline two"' \
    "$(python3 "${PARSE}" "${TMP}/multi.txt" | cut -f2)"

# --- malformed input is rejected, not half-parsed ---------------------------
#
# A record missing its second '--' would otherwise be read as a two-field
# record and mutate a file into whatever the name happened to be.
printf 'broken\n--\nonly one separator\n' > "${TMP}/bad.txt"
python3 "${PARSE}" "${TMP}/bad.txt" > /dev/null 2>&1
check "a malformed record is an error, not a silent skip" "2" "$?"

# --- an empty file is an error ----------------------------------------------
#
# Zero mutants means zero verdicts, and a campaign that scores nothing must
# say so rather than print a clean 0/0 that reads like success.
: > "${TMP}/empty.txt"
python3 "${PARSE}" "${TMP}/empty.txt" > /dev/null 2>&1
check "an empty mutant file is an error" "2" "$?"

# --- a real mutant set round-trips ------------------------------------------
#
# Known-answer against a committed set, so the parser is checked against the
# files it will actually see rather than only against invented ones.
REAL="${SCRIPT_DIR}/../mutants/bridge-cva-arrayreader.txt"
if [[ -f "${REAL}" ]]; then
    check "the committed CVA mutant set parses to 5 records" "5" \
        "$(python3 "${PARSE}" "${REAL}" | wc -l | tr -d ' ')"
fi

echo
echo "mutant isolation tests: ${PASS} passed, ${FAIL} failed"
[[ "${FAIL}" -eq 0 ]]
