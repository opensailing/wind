#!/bin/bash
# Known-answer tests for check_frozen_params.sh.
#
# WHY THIS EXISTS
#
# The checker's job is to answer "can any production caller set this shader
# parameter, or is it welded to whatever the defaults function writes". Measured
# 2026-08-05 on this repo, the answer for 13 of 66 parameters was "welded":
# `FlowVizRayMarch::FillDefaults` was the only non-test writer of CompositeMode,
# bEnableLighting, IsoValue and ten others, so a shipped build could render in
# exactly one of the six composite modes and could never light a gradient.
#
# THE FAILURE MODE THIS GUARD IS SHAPED AGAINST
#
# The suite was green on all six modes the whole time. Those tests write
# `Params->CompositeMode` directly, which is the correct way to test a shader
# and is structurally blind to whether anything else ever writes it. A test that
# supplies the input cannot discover that nothing else supplies it. So the
# checker deliberately does NOT count writes from Private/Tests/ -- and the
# cases below pin that, because a checker that counted them would have called
# the frozen tree clean.
#
# THE NEAR MISS IN METHOD, worth preserving because it nearly ended the audit:
# the first query asked "which parameters are never written?" and returned
# EMPTY, because the defaults function counts as a writer. Empty read as "all
# covered." The question had to become "written only by the defaults function"
# before the 13 appeared. The `all_written_but_frozen` case below is that exact
# tree: every parameter written, every parameter frozen. A checker built on the
# first question passes it. This one must not.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UNDER_TEST="${TOOLS_DIR}/check_frozen_params.sh"

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

# --- fixture builders ---------------------------------------------------------
#
# A fixture is a miniature of the real module: a Public/Render header declaring
# SHADER_PARAMETER rows, a Private/Render defaults function, optional other
# production writers, and optional tests that write the same names.

new_tree() {  # new_tree <dir>
    local d="$1"
    mkdir -p "${d}/Public/Render" "${d}/Private/Render" "${d}/Private/UI" "${d}/Private/Tests"
    printf 'BEGIN_SHADER_PARAMETER_STRUCT(FParams, )\n' > "${d}/Public/Render/Shader.h"
}

declare_params() {  # declare_params <dir> <name>...
    local d="$1"; shift
    local p
    for p in "$@"; do
        printf '\tSHADER_PARAMETER(uint32, %s)\n' "${p}" >> "${d}/Public/Render/Shader.h"
    done
}

# The defaults function. Its writes are what "frozen" means.
write_defaults() {  # write_defaults <dir> <name>...
    local d="$1"; shift
    local f="${d}/Private/Render/Shader.cpp" p
    printf 'void FlowVizRayMarch::FillDefaults(FParams& OutParameters)\n{\n' > "${f}"
    for p in "$@"; do
        printf '\tOutParameters.%s = 0;\n' "${p}" >> "${f}"
    done
    printf '}\n' >> "${f}"
}

# A real production writer, in a file that is not the defaults function.
write_production() {  # write_production <dir> <name>...
    local d="$1"; shift
    local f="${d}/Private/UI/ViewModel.cpp" p
    printf 'void FViewModel::Apply(FParams& OutParameters) const\n{\n' > "${f}"
    for p in "$@"; do
        printf '\tOutParameters.%s = Value;\n' "${p}" >> "${f}"
    done
    printf '}\n' >> "${f}"
}

# A test writer. Must NOT count -- this is the whole point of the guard.
write_test() {  # write_test <dir> <name>...
    local d="$1"; shift
    local f="${d}/Private/Tests/ShaderTest.cpp" p
    printf 'bool FTest::RunTest(const FString&)\n{\n' > "${f}"
    for p in "$@"; do
        printf '\tParams->%s = 3;\n' "${p}" >> "${f}"
    done
    printf '\treturn true;\n}\n' >> "${f}"
}

run_on() {  # run_on <dir> -> "<exit>:<clean|frozen|unscored>"
    local out rc
    out="$("${UNDER_TEST}" "$1" 2>&1)"
    rc=$?
    local kind="unknown"
    printf '%s' "${out}" | grep -q 'FROZEN PARAMETER' && kind="frozen"
    printf '%s' "${out}" | grep -q 'every declared parameter has a production writer' && kind="clean"
    printf '%s' "${out}" | grep -q 'UNSCORED' && kind="unscored"
    printf '%s:%s' "${rc}" "${kind}"
}

# --- ANTI-VACUITY: the checker must be able to say BOTH things ----------------
#
# These two trees differ only in whether ViewModel.cpp writes the parameter. A
# checker stuck on either answer fails one of them.

D="${WORK}/frozen"; new_tree "${D}"
declare_params  "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_defaults  "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_production "${D}" Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
check "a parameter only the defaults function writes is FROZEN" \
    "1:frozen" "$(run_on "${D}")"

D="${WORK}/clean"; new_tree "${D}"
declare_params  "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_defaults  "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_production "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
check "the same tree with one added production writer is CLEAN" \
    "0:clean" "$(run_on "${D}")"

# --- The defect that made the audit necessary ---------------------------------
#
# THE CASE THAT KILLS THE OBVIOUS IMPLEMENTATION. Every parameter is written,
# so "which parameters are never written?" returns empty and reads as a pass.
# Every parameter is ALSO frozen. This is the real tree as found.

D="${WORK}/all_written_but_frozen"; new_tree "${D}"
declare_params "${D}" CompositeMode bEnableLighting IsoValue Alpha Beta Gamma Delta Epsilon Zeta Eta
write_defaults "${D}" CompositeMode bEnableLighting IsoValue Alpha Beta Gamma Delta Epsilon Zeta Eta
check "a tree where EVERY parameter is written and every one is frozen is FROZEN, not clean" \
    "1:frozen" "$(run_on "${D}")"

# --- Test writers must not launder a frozen parameter -------------------------
#
# The suite was green on all six composite modes while this defect was live.
# If Private/Tests/ counted as production, this case would come back clean and
# the guard would certify the exact tree it exists to catch.

D="${WORK}/test_writer_only"; new_tree "${D}"
declare_params "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_defaults "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_production "${D}" Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_test     "${D}" CompositeMode
check "a parameter written ONLY by a test is still FROZEN" \
    "1:frozen" "$(run_on "${D}")"

# --- The report must name the instance, not just the count --------------------
#
# A finding names an instance: "13 frozen" sends the reader hunting, and the
# fix then gets applied to whichever one they find first.

D="${WORK}/names"; new_tree "${D}"
declare_params  "${D}" CompositeMode bEnableLighting IsoValue Alpha Beta Gamma Delta Epsilon Zeta Eta
write_defaults  "${D}" CompositeMode bEnableLighting IsoValue Alpha Beta Gamma Delta Epsilon Zeta Eta
write_production "${D}" Alpha Beta Gamma Delta Epsilon Zeta Eta
OUT="$("${UNDER_TEST}" "${D}" 2>&1)"
check "the report names every frozen parameter" \
    "3" "$(printf '%s\n' "${OUT}" | grep -cE '^  (CompositeMode|bEnableLighting|IsoValue)$')"

# --- Three defects found by running the checker on the real module ------------
#
# The first run reported 26 frozen of 74. Both numbers were wrong, and each
# error is a case below. This is the [[run-a-new-assertion-against-pristine-code-first]]
# shape: the checker went red on the real tree, which is what I wanted to see,
# and part of the red was the instrument rather than the code.

# A. BEGIN_SHADER_PARAMETER_STRUCT(Name, API) is not a parameter row. The
#    trailing-field rule harvested the API macro as a parameter, and since
#    nothing assigns to a macro it reported as frozen forever.
D="${WORK}/struct_header"; new_tree "${D}"
printf 'BEGIN_SHADER_PARAMETER_STRUCT(FParams, FLOWVIZRUNTIME_API)\n' > "${D}/Public/Render/Shader.h"
declare_params  "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_defaults  "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_production "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
check "the struct's own API macro is not counted as a parameter" \
    "0:clean" "$(run_on "${D}")"

# B. The defaults function shares its FILE with four other production writers
#    (FillFromVolumeParameters, SetVolumeTextures, SetLookAtCamera,
#    AddRayMarchPass). Excluding the whole file buried all four, which is how
#    bRejectNonFinite -- written by FillFromVolumeParameters, verified by hand --
#    was reported frozen. The exclusion must be the FUNCTION BODY, not the file.
D="${WORK}/sibling_fn"; new_tree "${D}"
declare_params "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
{
    printf 'void FlowVizRayMarch::FillDefaults(FParams& OutParameters)\n{\n'
    printf '\tOutParameters.CompositeMode = 0;\n\tOutParameters.Alpha = 0;\n'
    printf '\tOutParameters.Beta = 0;\n\tOutParameters.Gamma = 0;\n\tOutParameters.Delta = 0;\n'
    printf '\tOutParameters.Epsilon = 0;\n\tOutParameters.Zeta = 0;\n\tOutParameters.Eta = 0;\n'
    printf '\tOutParameters.Theta = 0;\n\tOutParameters.Iota = 0;\n}\n\n'
    # A sibling in the SAME FILE that genuinely varies its writes.
    printf 'void FlowVizRayMarch::FillFromVolumeParameters(const FSrc& Source, FParams& OutParameters)\n{\n'
    printf '\tOutParameters.Alpha = Source.Alpha;\n\tOutParameters.Beta = Source.Beta;\n'
    printf '\tOutParameters.Gamma = Source.Gamma;\n\tOutParameters.Delta = Source.Delta;\n'
    printf '\tOutParameters.Epsilon = Source.Epsilon;\n\tOutParameters.Zeta = Source.Zeta;\n'
    printf '\tOutParameters.Eta = Source.Eta;\n\tOutParameters.Theta = Source.Theta;\n'
    printf '\tOutParameters.Iota = Source.Iota;\n}\n'
} > "${D}/Private/Render/Shader.cpp"
OUT="$("${UNDER_TEST}" "${D}" 2>&1)"
check "a sibling function in the defaults FILE still counts as a production writer" \
    "1" "$(printf '%s\n' "${OUT}" | grep -cE '^  [A-Za-z]+$')"
check "  and the one frozen parameter is the right one" \
    "CompositeMode" "$(printf '%s\n' "${OUT}" | grep -E '^  [A-Za-z]+$' | tr -d ' ')"

# C. A write whose value sits on the next line -- `Params.X =\n\t Value;` -- has
#    nothing after the `=` for the [^=] guard to match, so it was missed. Two
#    such writes exist in FlowVizVolumeTexture.cpp.
D="${WORK}/multiline"; new_tree "${D}"
declare_params "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_defaults "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
{
    printf 'void FOther::Apply(FParams& Params) const\n{\n'
    for p in CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota; do
        printf '\tParams.%s =\n\t\tComputeSomething(Source);\n' "${p}"
    done
    printf '}\n'
} > "${D}/Private/UI/ViewModel.cpp"
check "a write whose value is on the following line still counts" \
    "0:clean" "$(run_on "${D}")"

# D. A comparison is not a write. Without this the guard is trivially satisfied
#    by any file that reads the parameter.
D="${WORK}/comparison"; new_tree "${D}"
declare_params "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_defaults "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
write_production "${D}" Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
printf 'void FReader::Check(const FParams& P)\n{\n\tif (P.CompositeMode == 4) { Log(); }\n}\n' \
    > "${D}/Private/UI/Reader.cpp"
check "a == comparison does not count as a write" \
    "1:frozen" "$(run_on "${D}")"

# --- The allowlist, and why it needs its own guard ----------------------------
#
# Some parameters are SUPPOSED to be constant: explicit padding floats that exist
# only to satisfy cbuffer alignment, and static sampler states. Reporting them
# forever would train the reader to skim a 21-line list for the 16 that matter,
# which is how the real ones get missed.
#
# But an allowlist is a place to hide things. Anyone can silence this checker by
# appending a name. So: entries must carry a reason, and an entry that is NOT
# actually frozen is an ERROR rather than a shrug -- that is the signature of a
# name added to dodge the check, or left behind after a parameter was wired up.

allow() {  # allow <dir> <name>:<reason>...
    local d="$1"; shift
    printf '%s\n' "$@" > "${d}/frozen_params_allow.txt"
}

D="${WORK}/allow_ok"; new_tree "${D}"
declare_params  "${D}" CropPad0 CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_defaults  "${D}" CropPad0 CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_production "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
allow "${D}" 'CropPad0:cbuffer alignment padding, never read by the shader'
check "an allowlisted padding member does not count as frozen" \
    "0:clean" "$(run_on "${D}")"

D="${WORK}/allow_hides_real"; new_tree "${D}"
declare_params  "${D}" CropPad0 CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_defaults  "${D}" CropPad0 CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_production "${D}" Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
allow "${D}" 'CropPad0:cbuffer alignment padding, never read by the shader'
check "allowlisting padding does NOT silence a real frozen control beside it" \
    "1:frozen" "$(run_on "${D}")"

# An entry with no reason is not an entry. The reason is the whole mechanism:
# it is what a reviewer reads to decide the exemption is honest.
D="${WORK}/allow_no_reason"; new_tree "${D}"
declare_params  "${D}" CropPad0 CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_defaults  "${D}" CropPad0 CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_production "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
allow "${D}" 'CropPad0'
check "an allowlist entry with no reason is UNSCORED, not honoured" \
    "2:unscored" "$(run_on "${D}")"

# A stale entry. The parameter got wired up and nobody removed its exemption --
# so the next parameter to freeze under that name is pre-silenced.
D="${WORK}/allow_stale"; new_tree "${D}"
declare_params  "${D}" CropPad0 CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_defaults  "${D}" CropPad0 CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_production "${D}" CropPad0 CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
allow "${D}" 'CropPad0:cbuffer alignment padding, never read by the shader'
check "an allowlisted parameter that is NOT frozen is a stale-entry error" \
    "2:unscored" "$(run_on "${D}")"

# --- An empty match set must not read as a pass -------------------------------
#
# The way this checker dies silently is a header whose SHADER_PARAMETER rows it
# cannot parse: zero parameters declared, zero frozen, "clean". Same shape as
# the bash-3.2 mapfile failure that made every unity test pass vacuously.

D="${WORK}/no_params"; new_tree "${D}"
printf 'struct FNotAShaderStruct { uint32 X; };\n' > "${D}/Public/Render/Shader.h"
write_defaults "${D}" Alpha
check "a header with no SHADER_PARAMETER rows is UNSCORED, not clean" \
    "2:unscored" "$(run_on "${D}")"

D="${WORK}/no_defaults"; new_tree "${D}"
declare_params "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota
rm -f "${D}/Private/Render/Shader.cpp"
check "a tree with no defaults function at all is UNSCORED, not clean" \
    "2:unscored" "$(run_on "${D}")"

D="${WORK}/too_few"; new_tree "${D}"
declare_params "${D}" Alpha Beta
write_defaults "${D}" Alpha Beta
write_production "${D}" Alpha Beta
check "an implausibly small extraction is UNSCORED, not clean" \
    "2:unscored" "$(run_on "${D}")"

check "a missing directory is an error, not a pass" \
    "2:unknown" "$(run_on "${WORK}/does_not_exist")"

# --- A row the extractor cannot parse must not vanish -------------------------
#
# Found 2026-08-05 by probing the checker's own output against the real header:
# it reported "16 of 73" while the header declares 74 rows. SHADER_PARAMETER_ARRAY
# carries a third field -- `(FVector4f, ClipPlanes, [MaxClipPlanes])` -- so the
# trailing-field pattern lands on the bracket and the name-shape filter dropped
# the line. Silently. The count measured the regex's vocabulary, not the code.
#
# That is the same defect class this checker exists to find: a number that reads
# as coverage while the thing it counts was never examined. Two requirements
# follow, and the second matters more than the first.

declare_array() {  # declare_array <dir> <name> <extent>
    local d="$1" name="$2" extent="$3"
    printf '\tSHADER_PARAMETER_ARRAY(FVector4f, %s, [%s])\n' \
        "${name}" "${extent}" >> "${d}/Public/Render/Shader.h"
}

# 1. The array parameter is a real parameter. Frozen when only the defaults
#    writes it -- ClipPlanes in the real module is written by the clip view model
#    via `OutParameters.ClipPlanes[Index] =`, which the writer scan must also see.
D="${WORK}/array_frozen"; new_tree "${D}"
declare_params "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
declare_array  "${D}" ClipPlanes 'FlowVizRayMarch::MaxClipPlanes'
# The array write must go INSIDE the defaults body. Appending it after
# write_defaults puts it past the closing brace, where the body tracker
# correctly reads it as a production writer -- the fixture would then be
# testing brace tracking and reporting on array extraction.
printf 'void FlowVizRayMarch::FillDefaults(FParams& OutParameters)\n{\n' \
    > "${D}/Private/Render/Shader.cpp"
for p in CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta; do
    printf '\tOutParameters.%s = 0;\n' "${p}" >> "${D}/Private/Render/Shader.cpp"
done
printf '\tOutParameters.ClipPlanes[Index] = FVector4f(0,0,0,0);\n}\n' \
    >> "${D}/Private/Render/Shader.cpp"
write_production "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
check "an array parameter written only by the defaults is frozen" \
    "1:frozen" "$(run_on "${D}")"

D="${WORK}/array_written"; new_tree "${D}"
declare_params  "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
declare_array   "${D}" ClipPlanes 'FlowVizRayMarch::MaxClipPlanes'
write_defaults  "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_production "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
printf '\tOutParameters.ClipPlanes[Written] = FVector4f(A,B,C,D);\n}\n' \
    >> "${D}/Private/UI/ViewModel.cpp"
check "a subscripted write counts as a production writer" \
    "0:clean" "$(run_on "${D}")"

# 2. THE ONE THAT GENERALISES. A declaration form nobody has thought of yet must
#    be REPORTED, not discarded. This fixture uses a shape the current extractor
#    has no rule for; the checker may not answer clean or frozen, because it
#    cannot know which. Without this, the next macro form to appear repeats
#    exactly the failure above and nothing says so.
D="${WORK}/unparseable_row"; new_tree "${D}"
declare_params "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
printf '\tSHADER_PARAMETER_STRUCT_INCLUDE(FSomeOther, %s)\n' '' \
    >> "${D}/Public/Render/Shader.h"
write_defaults  "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
write_production "${D}" CompositeMode Alpha Beta Gamma Delta Epsilon Zeta Eta Theta
check "a SHADER_PARAMETER row the extractor cannot parse is UNSCORED, not dropped" \
    "2:unscored" "$(run_on "${D}")"

printf '\n%d passed, %d failed\n' "${PASS}" "${FAIL}"
[[ "${FAIL}" -eq 0 ]]
