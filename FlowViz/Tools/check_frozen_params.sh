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
# Measured 2026-08-05: 13 of 66 parameters were frozen this way, including
# CompositeMode (welded to Alpha) and bEnableLighting (welded to 0), which made
# four of the five modes required by OPENFOAM_PARAVIEW_PARITY.md unreachable and
# the entire gradient-lighting path dead. The suite was green throughout,
# because every test writes the parameter itself.
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
    | grep -vE '(BEGIN|END)_SHADER_PARAMETER' \
    | sed -E 's/.*,[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\)$/\1/' \
    | grep -E '^[A-Za-z_][A-Za-z0-9_]*$' \
    | sort -u > "${WORK}/declared.txt"

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

DEFAULTS_FILE="$(grep -rl "${DEFAULTS_FN}" "${MODULE_DIR}" --include='*.cpp' 2>/dev/null \
    | grep -v '/Tests/' | head -1)"

if [[ -z "${DEFAULTS_FILE}" ]]; then
    echo "check_frozen_params: UNSCORED" >&2
    echo "  no non-test .cpp under ${MODULE_DIR} defines ${DEFAULTS_FN}." >&2
    echo "  Without it there is nothing to measure 'frozen' against, and every" >&2
    echo "  parameter would appear to have a production writer." >&2
    exit 2
fi

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

find "${MODULE_DIR}" -name '*.cpp' -not -path '*/Tests/*' 2>/dev/null > "${WORK}/prod_files.txt"

: > "${WORK}/written.txt"
while IFS= read -r f; do
    [[ -n "${f}" ]] || continue
    awk -v fn="${DEFAULTS_FN}" '
        # Track whether we are inside the defaults function body.
        !in_fn && index($0, fn "(") { in_fn = 1; depth = 0; seen_brace = 0 }
        in_fn {
            n = gsub(/\{/, "{"); depth += n; if (n > 0) seen_brace = 1
            depth -= gsub(/\}/, "}")
            if (seen_brace && depth <= 0) { in_fn = 0 }
            next
        }
        { print }
    ' "${f}" 2>/dev/null \
        | grep -ohE '(\.|->)[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=([^=]|$)' \
        | sed -E 's/^(\.|->)([A-Za-z0-9_]+).*/\2/' >> "${WORK}/written.txt"
done < "${WORK}/prod_files.txt"
sort -u -o "${WORK}/written.txt" "${WORK}/written.txt"

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
