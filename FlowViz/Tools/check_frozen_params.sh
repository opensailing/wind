#!/bin/bash
# Find shader parameters that no production caller can set.
#
# WHAT THIS ANSWERS, and what it does not
#
# A parameter written ONLY by the defaults function is frozen: whatever constant
# that function assigns is the only value a shipped build can ever render with.
# The shader may implement six composite modes and handle all six correctly --
# this checker is about whether anything can SELECT them.
#
# Measured 2026-08-05: 16 of 74 parameters were frozen this way, including
# CompositeMode (welded to Alpha) and bEnableLighting (welded to 0), which made
# four of the five modes required by OPENFOAM_PARAVIEW_PARITY.md unreachable and
# the entire gradient-lighting path dead. The suite was green throughout,
# because every test writes the parameter itself.
#
# The denominator was 73 until the extractor was fixed to parse
# SHADER_PARAMETER_ARRAY: one row had been dropped silently, so the count
# described this script's vocabulary rather than the header. Hence the UNSCORED
# on any row it cannot parse -- see step 1.
#
# THAT is why writes from Private/Tests/ do not count here. A test that supplies
# the input cannot discover that nothing else supplies it, so counting test
# writes would make this checker certify the exact defect it exists to find.
#
# EXIT CODES
#   0  every declared parameter has a production writer besides the defaults
#   1  at least one frozen parameter, each named on its own line
#   2  UNSCORED -- the scan could not establish either answer. Never conflate
#      this with 0: a checker that extracts nothing reports "no frozen
#      parameters" in the same words it uses for a clean tree.
#
# Usage: check_frozen_params.sh [module-dir]

set -uo pipefail

MODULE_DIR="${1:-Plugins/FlowVizRuntime/Source/FlowVizRuntime}"

# The defaults function, and the file it lives in. A write inside this file is
# what "frozen" is measured against.
DEFAULTS_FN="FillDefaults"

# Plausibility floors. Below these the extraction is not believable and the
# answer is UNSCORED rather than a pass. The real module declares 66 parameters;
# 5 is low enough for a small fixture and high enough that a parse failure
# (which yields 0) cannot slip through as clean.
MIN_PARAMS=5

if [[ ! -d "${MODULE_DIR}" ]]; then
    echo "check_frozen_params: no such directory: ${MODULE_DIR}" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# --- 1. Every declared parameter ----------------------------------------------
#
# SHADER_PARAMETER(type, Name) and SHADER_PARAMETER_ARRAY etc. Take the last
# comma-separated field before the close paren. Nested template types like
# TShaderResourceParameterTypeInfo<FVector3f> contain commas, so anchor on the
# final one.

# BEGIN_/END_SHADER_PARAMETER_STRUCT are excluded by the leading-boundary match:
# `BEGIN_SHADER_PARAMETER_STRUCT(FParams, FLOWVIZRUNTIME_API)` has the same
# trailing-field shape as a real row, so without this the API macro is harvested
# as a parameter -- and since nothing ever assigns to a macro, it reports frozen
# in perpetuity. Measured on the real header, which is exactly this form.
grep -rhoE '(^|[^A-Z_])SHADER_PARAMETER[A-Z_]*\([^)]*\)' "${MODULE_DIR}" --include='*.h' 2>/dev/null \
    | grep -vE '(BEGIN|END)_SHADER_PARAMETER' > "${WORK}/rows.txt"

# SHADER_PARAMETER_ARRAY carries a THIRD field -- (FVector4f, ClipPlanes,
# [MaxClipPlanes]) -- so the trailing-field rule lands on the bracket instead of
# the name. Its own rule, applied first.
sed -E 's/.*SHADER_PARAMETER_ARRAY\([^,]*,[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*,.*/\1/; t
         s/.*,[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\)$/\1/' \
    "${WORK}/rows.txt" > "${WORK}/names_raw.txt"

grep -E '^[A-Za-z_][A-Za-z0-9_]*$' "${WORK}/names_raw.txt" | sort -u > "${WORK}/declared.txt"

# A row that yielded no identifier is a declaration form this script has no rule
# for. It must be REPORTED, never discarded.
#
# Found 2026-08-05 by counting: the checker said "16 of 73" while the header
# holds 74 rows. The missing one was SHADER_PARAMETER_ARRAY, dropped by the
# name-shape filter without a word -- so the number read as a full census while
# one parameter had never been examined at all. That is the same defect this
# checker exists to find, in the checker. Silence about what a scan could not
# parse is indistinguishable from a scan that found nothing wrong.
grep -vE '^[A-Za-z_][A-Za-z0-9_]*$' "${WORK}/names_raw.txt" > "${WORK}/unparsed.txt"
if [[ -s "${WORK}/unparsed.txt" ]]; then
    echo "check_frozen_params: UNSCORED" >&2
    echo "  $(wc -l < "${WORK}/unparsed.txt" | tr -d ' ') SHADER_PARAMETER row(s) matched but yielded no" >&2
    echo "  parameter name. This script has no rule for their declaration form, so" >&2
    echo "  it cannot say whether they are frozen -- and dropping them would report" >&2
    echo "  a census of everything it happens to understand as a census of the code." >&2
    echo >&2
    sed 's/^/  /' "${WORK}/unparsed.txt" >&2
    exit 2
fi

PARAM_COUNT=$(wc -l < "${WORK}/declared.txt" | tr -d ' ')

if [[ "${PARAM_COUNT}" -lt "${MIN_PARAMS}" ]]; then
    echo "check_frozen_params: UNSCORED" >&2
    echo "  only ${PARAM_COUNT} shader parameters extracted from ${MODULE_DIR}," >&2
    echo "  below the plausibility floor of ${MIN_PARAMS}. Either the header uses a" >&2
    echo "  declaration form this script does not parse, or the path is wrong." >&2
    echo "  An empty extraction would otherwise report every parameter as fine." >&2
    exit 2
fi

# --- 2. Locate the defaults function ------------------------------------------
#
# Its file is excluded from the production-writer scan below. If it cannot be
# found, every parameter would look like it has no defaults-only writer and the
# tree would read as clean -- the exact vacuous pass this guards against.

# MATCH THE DEFINITION, NOT THE NAME. This was `grep -rl "${DEFAULTS_FN}"`,
# which matches any file CONTAINING the string. Measured on this module: four
# non-test files did -- the definer, two call sites, and one whose only matches
# were COMMENTS naming the function to explain a clamp. `head -1` then picked
# whichever the directory walk reached first, and six identical runs returned
# two different files.
#
# When it picked a non-definer the real defaults file was scanned as ordinary
# production code, every welded constant counted as a production write, and the
# checker printed "every declared parameter has a production writer" -- exit 0,
# on the tree with 16 frozen parameters. A comment was enough to disarm it.
#
# A definition starts at column 0 with a return type. A call site is indented
# or follows `=`; a comment line starts with `//` or `*`.
# Verified against both definition forms (`void FlowVizRayMarch::FillDefaults(`
# and a bare `void FillDefaults(`) and against six things it must NOT match:
# an indented call, `// FillDefaults ...`, ` * FillDefaults(`, a call on the
# right of `=`, and the substring names MyFillDefaults / FillDefaultsHelper.
DEFN_RE='^[A-Za-z_][A-Za-z0-9_:<>,&*[:space:]]*[^A-Za-z0-9_]'"${DEFAULTS_FN}"'[[:space:]]*\('

find "${MODULE_DIR}" -name '*.cpp' -not -path '*/Tests/*' 2>/dev/null \
    | sort > "${WORK}/all_prod.txt"

: > "${WORK}/defn_files.txt"
while IFS= read -r f; do
    [[ -n "${f}" ]] || continue
    if grep -qE "${DEFN_RE}" "${f}" 2>/dev/null; then
        printf '%s\n' "${f}" >> "${WORK}/defn_files.txt"
    fi
done < "${WORK}/all_prod.txt"

DEFN_COUNT="$(wc -l < "${WORK}/defn_files.txt" | tr -d ' ')"

if [[ "${DEFN_COUNT}" -eq 0 ]]; then
    echo "check_frozen_params: UNSCORED" >&2
    echo "  no non-test .cpp under ${MODULE_DIR} defines ${DEFAULTS_FN}." >&2
    echo "  Without it there is nothing to measure 'frozen' against, and every" >&2
    echo "  parameter would appear to have a production writer." >&2
    exit 2
fi

# Ambiguity is UNSCORED, never a pick. With two definitions, excluding one
# leaves the other scanned as production, so its welded constants count as
# real writes -- the same false clean, arrived at a different way.
if [[ "${DEFN_COUNT}" -gt 1 ]]; then
    echo "check_frozen_params: UNSCORED" >&2
    echo "  ${DEFN_COUNT} non-test .cpp files define ${DEFAULTS_FN}:" >&2
    echo >&2
    sed 's/^/    /' "${WORK}/defn_files.txt" >&2
    echo >&2
    echo "  'Frozen' is measured against exactly one defaults function. Choosing" >&2
    echo "  one would leave the other's welded constants counting as production" >&2
    echo "  writes, which is the false clean this checker exists to prevent." >&2
    exit 2
fi

DEFAULTS_FILE="$(cat "${WORK}/defn_files.txt")"

# --- 3. Every production write ------------------------------------------------
#
# Production = not Private/Tests/, and not inside the defaults FUNCTION BODY.
#
# The body, not the file. The defaults function shares FlowVizVolumeRayMarchShader.cpp
# with four other genuine writers -- FillFromVolumeParameters, SetVolumeTextures,
# SetLookAtCamera, AddRayMarchPass. Excluding the file buried all four and
# reported bRejectNonFinite (written by FillFromVolumeParameters, confirmed by
# hand) as frozen. Body extents are found by brace depth from the function's
# opening line.
#
# Both `X.Name =` and `X->Name =`. A `==` comparison is not a write. The value
# may sit on the following line, so the character after `=` is allowed to be
# end-of-line -- two such writes exist in FlowVizVolumeTexture.cpp and were
# missed by requiring a non-`=` character there.

cp "${WORK}/all_prod.txt" "${WORK}/prod_files.txt"

: > "${WORK}/written.txt"
while IFS= read -r f; do
    [[ -n "${f}" ]] || continue
    # Only the definer has a body to exclude. This was applied to EVERY file,
    # keyed on `index($0, fn "(")` -- which matches a CALL site too. A call
    # line carries no brace, so seen_brace stayed 0, depth never returned to 0,
    # and every following line was discarded as "inside the defaults body". In
    # FlowVizVolumeRayMarchDispatcher.cpp that silently ate the 11 lines after
    # `FlowVizRayMarch::FillDefaults(Request.Parameters);`. They happened to
    # contain no writes, so the count survived by luck rather than by design.
    if [[ "${f}" != "${DEFAULTS_FILE}" ]]; then
        grep -ohE '(\.|->)[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?[[:space:]]*=([^=]|$)' "${f}" 2>/dev/null \
            | sed -E 's/^(\.|->)([A-Za-z0-9_]+).*/\2/' >> "${WORK}/written.txt"
        continue
    fi
    # The definition's LINE NUMBER, found by grep, not re-matched inside awk.
    # awk's ERE rejected the `\(` in DEFN_RE outright ("illegal primary in
    # regular expression"); the rule then never fired, the defaults body was
    # never excluded, and all ten of its welded writes counted as production --
    # a checker that had stopped measuring anything while still exiting 0.
    # 2>/dev/null on the awk had been swallowing the error message.
    DEFN_LINE="$(grep -nE "${DEFN_RE}" "${f}" | head -1 | cut -d: -f1)"
    if [[ -z "${DEFN_LINE}" ]]; then
        echo "check_frozen_params: UNSCORED" >&2
        echo "  ${f} was selected as the defaults file but no definition line" >&2
        echo "  could be located in it. Without body extents its welded writes" >&2
        echo "  would count as production writes." >&2
        exit 2
    fi
    awk -v defn="${DEFN_LINE}" '
        # Track whether we are inside the defaults function body. Anchored on the
        # DEFINITION line, so a recursive or self-referential call cannot start
        # a second phantom body.
        !in_fn && NR == defn { in_fn = 1; depth = 0; seen_brace = 0 }
        in_fn {
            n = gsub(/\{/, "{"); depth += n; if (n > 0) seen_brace = 1
            depth -= gsub(/\}/, "}")
            if (seen_brace && depth <= 0) { in_fn = 0 }
            next
        }
        { print }
    ' "${f}" 2>/dev/null \
        | grep -ohE '(\.|->)[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?[[:space:]]*=([^=]|$)' \
        | sed -E 's/^(\.|->)([A-Za-z0-9_]+).*/\2/' >> "${WORK}/written.txt"
done < "${WORK}/prod_files.txt"
sort -u -o "${WORK}/written.txt" "${WORK}/written.txt"

# --- 3b. A writer nobody references is not a fix ------------------------------
#
# Found 2026-08-05 while fixing the defect this script reports. Adding a view
# model that wrote all 16 frozen parameters -- real production file, outside
# Private/Tests/ -- flipped this checker to "every declared parameter has a
# production writer" while NOTHING had changed about reachability: the dispatcher
# still called only FillDefaults, and the new function's only caller in the whole
# tree was its own test. A shipped build still rendered one composite mode.
#
# That green was worse than the red it replaced, because it tells the next reader
# the problem is solved. So: a file whose writes are counted must itself be
# mentioned by some other production file.
#
# THIS IS DELIBERATELY WEAKER THAN CALL-GRAPH REACHABILITY and says so. A source
# scan cannot see whether the call actually runs, so an included-but-never-called
# writer still passes. It catches the case that actually occurred -- a writer with
# no production mention at all -- and the UNSCORED wording claims nothing more.

: > "${WORK}/orphans.txt"
while IFS= read -r f; do
    [[ -n "${f}" ]] || continue
    [[ "${f}" == "${DEFAULTS_FILE}" ]] && continue

    # Does this file write any declared parameter at all? If not it is not a
    # writer and its reachability is irrelevant.
    # DEFAULTS_FILE is already skipped above, so no body exclusion is needed
    # here. The earlier version ran the same call-site-triggered awk over every
    # file, which meant a file whose only writes followed a call to the defaults
    # function looked like it wrote nothing and was silently exempted from the
    # reachability question entirely.
    if ! grep -qE '(\.|->)[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?[[:space:]]*=([^=]|$)' "${f}" 2>/dev/null; then
        continue
    fi

    # Is it mentioned by a THIRD production file -- one that is neither itself
    # nor its own paired header?
    #
    # The pairing exclusion is the whole rule. A writer's own header always
    # names it, so "is this basename mentioned by any other file?" is true for
    # every .cpp that has a header, and the check could never fail. Measured:
    # with the self-pair counted, the real module reported clean while
    # FlowVizRenderSettingsViewModel.cpp had no consumer at all -- the rule was
    # a tautology that returned exactly the answer being hoped for.
    base="$(basename "${f}" .cpp)"
    if ! grep -rl "${base}" "${MODULE_DIR}" --include='*.cpp' --include='*.h' 2>/dev/null \
        | grep -v '/Tests/' \
        | grep -v "/${base}\.cpp$" \
        | grep -qv "/${base}\.h$"; then
        printf '%s\n' "${f}" >> "${WORK}/orphans.txt"
    fi
done < "${WORK}/prod_files.txt"

if [[ -s "${WORK}/orphans.txt" ]]; then
    echo "check_frozen_params: UNSCORED" >&2
    echo "  These production files write shader parameters, and no other" >&2
    echo "  production file mentions them:" >&2
    echo >&2
    sed 's/^/    /' "${WORK}/orphans.txt" >&2
    echo >&2
    echo "  Their writes are not counted, because a writer nobody reaches leaves" >&2
    echo "  the parameter exactly as frozen as before -- and reporting it as a" >&2
    echo "  production writer would answer 'can a user select this?' with 'a" >&2
    echo "  function exists that could'. Wire the writer into the dispatch path," >&2
    echo "  or delete it." >&2
    echo >&2
    echo "  (This checks MENTION, not that the call executes. It cannot detect a" >&2
    echo "  writer that is included and never called.)" >&2
    exit 2
fi

# --- 4. Frozen = declared, and never written outside the defaults -------------

comm -23 "${WORK}/declared.txt" "${WORK}/written.txt" > "${WORK}/frozen_raw.txt"

# --- 5. The allowlist ---------------------------------------------------------
#
# Some parameters are meant to be constant: explicit cbuffer padding, and static
# sampler states. Listing them every run trains the reader to skim past the ones
# that matter.
#
# An allowlist is also the obvious way to defeat this checker, so it is
# constrained two ways. Every entry must carry a reason after a colon -- that
# text is what a reviewer reads to judge the exemption. And an entry naming a
# parameter that is NOT frozen is an error, not a shrug: it means either the
# parameter was wired up and its exemption outlived it (pre-silencing whatever
# freezes under that name next), or the name never applied at all.

ALLOW_FILE="${MODULE_DIR}/frozen_params_allow.txt"
: > "${WORK}/allowed.txt"

if [[ -f "${ALLOW_FILE}" ]]; then
    while IFS= read -r line || [[ -n "${line}" ]]; do
        # Blank lines and # comments are not entries.
        [[ -z "${line}" || "${line}" =~ ^[[:space:]]*# ]] && continue

        name="${line%%:*}"
        reason="${line#*:}"
        name="$(printf '%s' "${name}" | tr -d '[:space:]')"

        if [[ "${line}" != *:* || -z "$(printf '%s' "${reason}" | tr -d '[:space:]')" ]]; then
            echo "check_frozen_params: UNSCORED" >&2
            echo "  allowlist entry '${line}' has no reason after the colon." >&2
            echo "  The reason is the mechanism: it is what a reviewer reads to decide" >&2
            echo "  the exemption is honest. An entry without one is a silenced check." >&2
            exit 2
        fi

        if ! grep -qx "${name}" "${WORK}/frozen_raw.txt"; then
            echo "check_frozen_params: UNSCORED" >&2
            echo "  allowlist names '${name}', which is NOT frozen." >&2
            echo "  Either it acquired a production writer and this entry outlived it --" >&2
            echo "  leaving the next parameter to freeze under that name pre-silenced --" >&2
            echo "  or the name never matched a parameter. Remove it." >&2
            exit 2
        fi

        printf '%s\n' "${name}" >> "${WORK}/allowed.txt"
    done < "${ALLOW_FILE}"
fi

sort -u -o "${WORK}/allowed.txt" "${WORK}/allowed.txt"
comm -23 "${WORK}/frozen_raw.txt" "${WORK}/allowed.txt" > "${WORK}/frozen.txt"
FROZEN_COUNT=$(wc -l < "${WORK}/frozen.txt" | tr -d ' ')
ALLOWED_COUNT=$(wc -l < "${WORK}/allowed.txt" | tr -d ' ')

if [[ "${FROZEN_COUNT}" -eq 0 ]]; then
    echo "check_frozen_params: every declared parameter has a production writer."
    echo "  ${PARAM_COUNT} parameters declared, all reachable from outside ${DEFAULTS_FN}"
    echo "  (${ALLOWED_COUNT} allowlisted as deliberately constant, each verified still frozen)."
    exit 0
fi

echo "check_frozen_params: FROZEN PARAMETER(S) -- ${FROZEN_COUNT} of ${PARAM_COUNT}" >&2
echo >&2
echo "These are declared and written ONLY by ${DEFAULTS_FN}. Whatever constant it" >&2
echo "assigns is the only value a shipped build can render with. Writes from" >&2
echo "Private/Tests/ are deliberately not counted: a test that sets the parameter" >&2
echo "itself cannot reveal that nothing else does." >&2
echo >&2
sed 's/^/  /' "${WORK}/frozen.txt" >&2
echo >&2
echo "Fix by applying a settings source after ${DEFAULTS_FN}, keeping its values as" >&2
echo "the defaults -- they are usually deliberate. See Docs/BACKLOG.md item 2e." >&2
exit 1
