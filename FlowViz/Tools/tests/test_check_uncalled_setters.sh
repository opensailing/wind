#!/bin/bash
# Known-answer tests for check_uncalled_setters.sh.
#
# WHY THIS EXISTS
#
# check_frozen_params.sh went green on 2026-08-06 because all 16 frozen shader
# parameters gained a production writer: FFlowVizRenderSettingsViewModel. But
# that writer copies its own member fields, and 14 of the view model's 17
# setters had no production caller -- so 13 render controls were still welded
# to a single value. The freeze moved one level up, past the guard's edge, and
# the guard said clean because "is there a writer outside FillDefaults" is the
# only question it asks.
#
# This checker asks the counterpart question: does each public setter on a view
# model have a production caller? Without it, the next control added freezes
# the same way and nothing says so.
#
# THE FAILURE MODES THESE CASES ARE SHAPED AGAINST, each one measured on this
# repo rather than imagined:
#
#  - A test that calls the setter itself cannot discover that nothing else
#    calls it. Private/Tests/ must not count. (The render-settings suite was
#    green on every setter the whole time.)
#  - A view model calling its OWN setter proves nothing about reachability;
#    SetCompositeModeByValue calls SetCompositeMode internally. Own-file calls
#    must not count.
#  - Name collisions launder: `.SetMaxSteps(` on some unrelated object must not
#    count as a caller of the view model's SetMaxSteps. Same defect the frozen
#    checker had with OutHeader.ComponentCount, same fix: a call counts only
#    from a file that mentions the view model's class.
#  - A commented-out call still matches a source-scanning grep
#    (an-anchor-matched-by-name-matches-comments). Line comments must not count.
#  - An allowlist is the obvious way to defeat the checker, so entries without
#    a reason, entries naming setters that do not exist, and entries whose
#    setter IS called must all refuse to score rather than silence anything.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UNDER_TEST="${TOOLS_DIR}/check_uncalled_setters.sh"

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

[[ -f "${UNDER_TEST}" ]] || { echo "no ${UNDER_TEST}"; echo '0 passed, 1 failed'; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# --- fixture builders ---------------------------------------------------------
#
# A fixture is a miniature of the real module: Public/UI holds
# FlowViz<Name>ViewModel.h headers, Private/UI their own .cpp files, and
# Private/Render production code that may or may not call the setters.
#
# Every tree gets a second view model (Beta) whose setters are all called, so
# each fixture clears the plausibility floors (2 classes, 5 setters) without
# the case under test having to carry them.

new_tree() {  # new_tree <dir>
    local d="$1"
    mkdir -p "${d}/Public/UI" "${d}/Private/UI" "${d}/Private/Render" "${d}/Private/Tests"
    vm "${d}" Beta SetOne SetTwo SetThree SetFour
    caller "${d}" BetaDriver.cpp Beta SetOne SetTwo SetThree SetFour
}

vm() {  # vm <dir> <Name> <setter>...
    local d="$1" name="$2"; shift 2
    local f="${d}/Public/UI/FlowViz${name}ViewModel.h" s
    printf 'class FLOWVIZRUNTIME_API FFlowViz%sViewModel\n{\npublic:\n' "${name}" > "${f}"
    for s in "$@"; do
        printf '\tvoid %s(int32 InValue);\n' "${s}" >> "${f}"
    done
    printf '};\n' >> "${f}"
}

caller() {  # caller <dir> <file> <Name> <setter>...
    local d="$1" file="$2" name="$3"; shift 3
    local f="${d}/Private/Render/${file}" s
    printf 'void FDriver::Drive(FFlowViz%sViewModel& VM)\n{\n' "${name}" > "${f}"
    for s in "$@"; do
        printf '\tVM.%s(1);\n' "${s}" >> "${f}"
    done
    printf '}\n' >> "${f}"
}

# Calls the setter on an object of some OTHER type, in a file that never
# mentions the view model's class. A caller scan keyed on the method name alone
# counts this and reports clean -- the laundering direction.
collision_caller() {  # collision_caller <dir> <file> <setter>...
    local d="$1" file="$2"; shift 2
    local f="${d}/Private/Render/${file}" s
    printf 'void FOther::Drive(FSomethingUnrelated& Header)\n{\n' > "${f}"
    for s in "$@"; do
        printf '\tHeader.%s(1);\n' "${s}" >> "${f}"
    done
    printf '}\n' >> "${f}"
}

test_caller() {  # test_caller <dir> <Name> <setter>...
    local d="$1" name="$2"; shift 2
    local f="${d}/Private/Tests/ViewModelTest.cpp" s
    printf 'bool FTest::RunTest(const FString&)\n{\n\tFFlowViz%sViewModel VM;\n' "${name}" > "${f}"
    for s in "$@"; do
        printf '\tVM.%s(1);\n' "${s}" >> "${f}"
    done
    printf '\treturn true;\n}\n' >> "${f}"
}

# The view model's own .cpp calling its own setter through this->. Matches the
# caller regex AND mentions the class, so only the own-file exclusion stops it.
self_caller() {  # self_caller <dir> <Name> <setter>
    local d="$1" name="$2" s="$3"
    printf 'void FFlowViz%sViewModel::ApplyPreset()\n{\n\tthis->%s(1);\n}\n' \
        "${name}" "${s}" > "${d}/Private/UI/FlowViz${name}ViewModel.cpp"
}

# A file that mentions the class and contains the call ONLY inside a comment.
comment_caller() {  # comment_caller <dir> <Name> <setter>
    local d="$1" name="$2" s="$3"
    {
        printf '// Explains how FFlowViz%sViewModel would be driven:\n' "${name}"
        printf '// VM.%s(1);\n' "${s}"
        printf 'void FDoc::Nothing()\n{\n}\n'
    } > "${d}/Private/Render/Commentary.cpp"
}

run_on() {  # run_on <dir> -> "<exit>:<clean|findings|unscored|unknown>"
    local out rc
    out="$("${UNDER_TEST}" "$1" 2>&1)"
    rc=$?
    local kind="unknown"
    printf '%s' "${out}" | grep -q 'UNCALLED SETTER' && kind="findings"
    printf '%s' "${out}" | grep -q 'every view model setter has a production caller' && kind="clean"
    printf '%s' "${out}" | grep -q 'UNSCORED' && kind="unscored"
    printf '%s:%s' "${rc}" "${kind}"
}

# --- ANTI-VACUITY: the checker must be able to say BOTH things ----------------
#
# Two trees differing only in whether Driver.cpp calls the setter. A checker
# stuck on either answer fails one of them.

D="${WORK}/uncalled"; new_tree "${D}"
vm "${D}" Alpha SetFrozen SetLive
caller "${D}" AlphaDriver.cpp Alpha SetLive
check "a setter no production file calls is a FINDING" \
    "1:findings" "$(run_on "${D}")"

OUT="$("${UNDER_TEST}" "${D}" 2>&1)" || true
check "the finding names the instance, not just a count" \
    "yes" "$(printf '%s' "${OUT}" | grep -q 'FlowVizAlphaViewModel::SetFrozen' && echo yes || echo no)"

D="${WORK}/called"; new_tree "${D}"
vm "${D}" Alpha SetFrozen SetLive
caller "${D}" AlphaDriver.cpp Alpha SetFrozen SetLive
check "CONTROL: the same tree with the caller added is CLEAN" \
    "0:clean" "$(run_on "${D}")"

# --- Callers that must NOT count ----------------------------------------------

D="${WORK}/test_only"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
test_caller "${D}" Alpha SetFrozen
check "a setter called ONLY from Private/Tests/ is still a FINDING" \
    "1:findings" "$(run_on "${D}")"

D="${WORK}/self_only"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
self_caller "${D}" Alpha SetFrozen
check "a view model calling its OWN setter is still a FINDING" \
    "1:findings" "$(run_on "${D}")"

D="${WORK}/collision"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
collision_caller "${D}" Unrelated.cpp SetFrozen
check "a same-named call in a file that never mentions the class is still a FINDING" \
    "1:findings" "$(run_on "${D}")"

D="${WORK}/commented"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
comment_caller "${D}" Alpha SetFrozen
check "a call that exists only in a comment is still a FINDING" \
    "1:findings" "$(run_on "${D}")"

# --- Declaration and caller forms measured on the REAL module -----------------
#
# Both of these are defects the checker had on its first real run, kept as
# regressions.
#
# FFlowVizRenderSettingsViewModel -- the view model that MOTIVATED this checker
# -- is declared `struct`, not `class`. A harvest that only matches `class`
# silently drops it, and its 14 uncalled setters vanish from the report while
# the checker still exits 1 on other findings. An incomplete census that still
# fails reads exactly like a complete one.

struct_vm() {  # struct_vm <dir> <Name> <setter>...
    local d="$1" name="$2"; shift 2
    local f="${d}/Public/UI/FlowViz${name}ViewModel.h" s
    printf 'struct FLOWVIZRUNTIME_API FFlowViz%sViewModel\n{\npublic:\n' "${name}" > "${f}"
    for s in "$@"; do
        printf '\tvoid %s(int32 InValue);\n' "${s}" >> "${f}"
    done
    printf '};\n' >> "${f}"
}

D="${WORK}/struct_decl"; new_tree "${D}"
struct_vm "${D}" Alpha SetFrozen
check "a view model declared 'struct' is harvested; its uncalled setter is a FINDING" \
    "1:findings" "$(run_on "${D}")"

OUT="$("${UNDER_TEST}" "${D}" 2>&1)" || true
check "  and the struct's setter is named in the report" \
    "yes" "$(printf '%s' "${OUT}" | grep -q 'FlowVizAlphaViewModel::SetFrozen' && echo yes || echo no)"

# Slate panels name the view model type in their HEADER (the SLATE_ARGUMENT and
# the member pointer) and make the calls in their .cpp through that member. A
# per-file class-mention gate discards every one of those real callers: measured
# on the real module, SFlowVizSlicePanel.cpp calls SetAxisPreset and five
# siblings, mentions the class zero times, and all six reported as uncalled.
# The companion header at the same path mentions it three times.

split_caller() {  # split_caller <dir> <Name> <setter>
    local d="$1" name="$2" s="$3"
    printf 'class SPanel\n{\n\tFFlowViz%sViewModel* ViewModel = nullptr;\n};\n' "${name}" \
        > "${d}/Private/Render/Panel.h"
    printf '#include "Panel.h"\nvoid SPanel::OnClicked()\n{\n\tViewModel->%s(1);\n}\n' "${s}" \
        > "${d}/Private/Render/Panel.cpp"
}

D="${WORK}/split_mention"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
split_caller "${D}" Alpha SetFrozen
check "a call in a .cpp whose companion header names the class is a CALLER (clean)" \
    "0:clean" "$(run_on "${D}")"

# The same split across the Unreal Private/Public convention: the panel's .cpp
# lives in Private/UI and its header in Public/UI. Same stem, sibling roots.
# Measured on the real module -- every S*Panel is laid out this way, so without
# this rule the six slice-panel setters above stay falsely uncalled even after
# the same-directory companion rule lands.

D="${WORK}/split_private_public"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
printf 'class SPanel\n{\n\tFFlowVizAlphaViewModel* ViewModel = nullptr;\n};\n' \
    > "${D}/Public/UI/SPanel.h"
printf '#include "UI/SPanel.h"\nvoid SPanel::OnClicked()\n{\n\tViewModel->SetFrozen(1);\n}\n' \
    > "${D}/Private/UI/SPanel.cpp"
check "a Private/UI .cpp whose Public/UI header names the class is a CALLER (clean)" \
    "0:clean" "$(run_on "${D}")"

# CONTROL: the companion-header rule must not reopen collision laundering. The
# .cpp calls the setter on an unrelated object AND has a companion header, but
# neither file mentions the view model class.
D="${WORK}/split_collision"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
printf 'class FOther\n{\n\tint32 X = 0;\n};\n' > "${D}/Private/Render/Panel.h"
printf '#include "Panel.h"\nvoid FOther::Drive(FSomethingUnrelated& H)\n{\n\tH.SetFrozen(1);\n}\n' \
    > "${D}/Private/Render/Panel.cpp"
check "  CONTROL: a companion header that does NOT name the class still gates the call out" \
    "1:findings" "$(run_on "${D}")"

# --- The allowlist must not be a free silence ---------------------------------

D="${WORK}/allowed"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
printf 'FlowVizAlphaViewModel::SetFrozen:tracked as backlog item 2f; panel lands with it\n' \
    > "${D}/uncalled_setters_allow.txt"
check "an allowlisted uncalled setter scores CLEAN" \
    "0:clean" "$(run_on "${D}")"

D="${WORK}/allow_no_reason"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
printf 'FlowVizAlphaViewModel::SetFrozen\n' > "${D}/uncalled_setters_allow.txt"
check "an allowlist entry with no reason is UNSCORED" \
    "2:unscored" "$(run_on "${D}")"

D="${WORK}/allow_stale"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
caller "${D}" AlphaDriver.cpp Alpha SetFrozen
printf 'FlowVizAlphaViewModel::SetFrozen:was uncalled once\n' > "${D}/uncalled_setters_allow.txt"
check "an allowlist entry whose setter IS called is UNSCORED (stale pre-silence)" \
    "2:unscored" "$(run_on "${D}")"

D="${WORK}/allow_unknown"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
caller "${D}" AlphaDriver.cpp Alpha SetFrozen
printf 'FlowVizAlphaViewModel::SetImaginary:never existed\n' > "${D}/uncalled_setters_allow.txt"
check "an allowlist entry naming a setter the scan did not find is UNSCORED" \
    "2:unscored" "$(run_on "${D}")"

# --- The scan must refuse to score what it could not extract ------------------

D="${WORK}/no_vms"
mkdir -p "${D}/Public/UI" "${D}/Private/Render"
check "a module with no view model headers is UNSCORED, not clean" \
    "2:unscored" "$(run_on "${D}")"

D="${WORK}/one_vm"
mkdir -p "${D}/Public/UI" "${D}/Private/UI" "${D}/Private/Render" "${D}/Private/Tests"
vm "${D}" Alpha SetOne SetTwo SetThree SetFour SetFive
caller "${D}" AlphaDriver.cpp Alpha SetOne SetTwo SetThree SetFour SetFive
check "one view model where the floor expects two is UNSCORED (harvest failure reads as absence)" \
    "2:unscored" "$(run_on "${D}")"

D="${WORK}/floor"; new_tree "${D}"
vm "${D}" Alpha SetFrozen
caller "${D}" AlphaDriver.cpp Alpha SetFrozen
OUT="$(MIN_SETTERS=500 "${UNDER_TEST}" "${D}" 2>&1)"
RC=$?
check "a setter count below the plausibility floor is UNSCORED" \
    "2:yes" "${RC}:$(printf '%s' "${OUT}" | grep -q 'UNSCORED' && echo yes || echo no)"

printf '\n%d passed, %d failed\n' "${PASS}" "${FAIL}"
[[ "${FAIL}" -eq 0 ]]
