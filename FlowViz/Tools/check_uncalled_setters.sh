#!/bin/bash
# Find view model setters that no production caller can reach.
#
# WHAT THIS ANSWERS, and why check_frozen_params.sh cannot
#
# The frozen-params checker asks "does anything outside FillDefaults write this
# shader parameter". On 2026-08-06 it went green because all 16 frozen
# parameters gained a writer: FFlowVizRenderSettingsViewModel. But that writer
# copies its own member fields, and 14 of its 17 setters had no production
# caller -- so 13 render controls were still welded to one value. The freeze
# had moved exactly one level up, past the edge of what that checker measures.
# A green guard bounds the defect it encodes, not the class.
#
# So this checker asks the counterpart question, one hop up the chain: for
# every public Set* method on every FlowViz*ViewModel, does any production
# file call it? A setter only tests call is a control the shipped build cannot
# operate, whatever the parameter-level scan says.
#
# WHAT COUNTS AS A CALLER. Not Private/Tests/ -- a test that calls the setter
# itself cannot discover that nothing else does. Not the view model's own
# files -- SetCompositeModeByValue calling SetCompositeMode proves internal
# plumbing, not reachability. And not a same-named method on some other type:
# a call counts only from a file that also mentions the view model's class,
# the same collision gate the frozen-params checker needed for
# OutHeader.ComponentCount.
#
# EXIT CODES
#   0  every view model setter has a production caller (allowlisted ones named)
#   1  at least one uncalled setter, each named on its own line
#   2  UNSCORED -- the scan could not establish either answer. Never conflate
#      this with 0: a scan that harvests nothing reports "no uncalled setters"
#      in the same words it uses for a fully wired tree.
#
# Usage: check_uncalled_setters.sh [module-dir]

set -uo pipefail

MODULE_DIR="${1:-Plugins/FlowVizRuntime/Source/FlowVizRuntime}"

# Plausibility floors. The real module has 6 view models and ~58 setters; a
# harvest far below that means the extraction failed, and an empty extraction
# would otherwise report every setter as fine. Overridable for fixtures and for
# probing that the floor itself works.
MIN_CLASSES="${MIN_CLASSES:-2}"
MIN_SETTERS="${MIN_SETTERS:-5}"

if [[ ! -d "${MODULE_DIR}" ]]; then
    echo "check_uncalled_setters: no such directory: ${MODULE_DIR}" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# --- 1. Every view model type and its public setters --------------------------
#
# The harvest walks each FlowViz*ViewModel.h and records "TypeName Setter"
# pairs. Method declarations only: a line like `void SetFoo(int32 InValue);` or
# `bool SetBar(float In);`. Comment lines are stripped first for the same
# reason the frozen checker strips them -- an example call in a doc comment is
# fully visible to grep and invisible to the compiler.
#
# `class` OR `struct`. FFlowVizRenderSettingsViewModel -- the type that
# MOTIVATED this checker -- is a struct, and a class-only harvest silently
# dropped it. The checker still exited 1 on other findings, so the incomplete
# census read exactly like a complete one; only the count (27, not 41) said
# otherwise, and only if you already knew the right answer.
#
# ONE TYPE PER HEADER (head -1), which is the module's convention. A second
# view model type in the same header would be attributed to the first -- if
# that convention breaks, this harvest must grow per-line tracking.

find "${MODULE_DIR}" -name 'FlowViz*ViewModel.h' -path '*/Public/*' 2>/dev/null \
    | sort > "${WORK}/vm_headers.txt"

: > "${WORK}/pairs.txt"
while IFS= read -r h; do
    [[ -n "${h}" ]] || continue
    cls="$(grep -oE '(class|struct)[[:space:]]+([A-Z_]*API[[:space:]]+)?FFlowViz[A-Za-z0-9]*ViewModel' "${h}" \
        | sed -E 's/.*[[:space:]](F[A-Za-z0-9]*ViewModel)$/\1/' | head -1)"
    [[ -n "${cls}" ]] || continue
    grep -vE '^[[:space:]]*(//|\*|/\*)' "${h}" \
        | grep -oE '^[[:space:]]*(virtual[[:space:]]+)?[A-Za-z_][A-Za-z0-9_:<>&]*[[:space:]]+Set[A-Z][A-Za-z0-9]*[[:space:]]*\(' \
        | grep -oE 'Set[A-Z][A-Za-z0-9]*' \
        | while IFS= read -r s; do printf '%s %s\n' "${cls}" "${s}"; done \
        >> "${WORK}/pairs.txt"
done < "${WORK}/vm_headers.txt"

sort -u -o "${WORK}/pairs.txt" "${WORK}/pairs.txt"

CLASS_COUNT="$(cut -d' ' -f1 "${WORK}/pairs.txt" | sort -u | wc -l | tr -d ' ')"
SETTER_COUNT="$(wc -l < "${WORK}/pairs.txt" | tr -d ' ')"

if [[ "${CLASS_COUNT}" -lt "${MIN_CLASSES}" || "${SETTER_COUNT}" -lt "${MIN_SETTERS}" ]]; then
    echo "check_uncalled_setters: UNSCORED" >&2
    echo "  harvested ${CLASS_COUNT} view model class(es) and ${SETTER_COUNT} setter(s) from" >&2
    echo "  ${MODULE_DIR}, below the plausibility floor of ${MIN_CLASSES} classes /" >&2
    echo "  ${MIN_SETTERS} setters. Either the headers use a declaration form this script" >&2
    echo "  does not parse, or the path is wrong. An empty harvest would otherwise" >&2
    echo "  report every setter as having a caller." >&2
    exit 2
fi

# --- 2. Production files that may hold callers --------------------------------
#
# Production = not Private/Tests/, and not the view model's own .h/.cpp. The
# own-file exclusion is per-class, applied in the loop below: FooViewModel.cpp
# is a legitimate caller of BarViewModel's setters.

find "${MODULE_DIR}" \( -name '*.cpp' -o -name '*.h' \) -not -path '*/Tests/*' 2>/dev/null \
    | sort > "${WORK}/prod_files.txt"

# --- 3. For each setter: any production caller? -------------------------------
#
# A caller is `.SetFoo(` or `->SetFoo(` on a NON-COMMENT line, in a file that
# also mentions the type name. `&Class::SetFoo` (delegate binding) counts too.
# The type-mention gate is what stops OutHeader.SetMaxSteps(...) on some
# unrelated type from laundering the view model's SetMaxSteps -- the same
# false-clean direction the frozen checker had to close.
#
# THE MENTION MAY LIVE IN THE COMPANION HEADER. Slate panels name the view
# model type in their .h (the SLATE_ARGUMENT and the member pointer) and make
# every call in their .cpp through that member, which never repeats the type.
# Measured on the real module: SFlowVizSlicePanel.cpp calls six setters,
# mentions the class zero times, and a per-file gate reported all six uncalled.
# So a .cpp is credited with its same-path .h's mentions. Only the companion
# (same directory, same stem) -- crediting any includer would reopen the
# collision hole one directory at a time.

mentions_type() {  # mentions_type <file> <type> -> 0 if file or companion .h mentions it
    local f="$1" t="$2"
    grep -qF "${t}" "${f}" 2>/dev/null && return 0
    case "${f}" in
        *.cpp)
            # Same directory first, then the Unreal Private/ -> Public/ mirror:
            # SFooPanel.cpp sits in Private/UI, its header in Public/UI.
            local hdr="${f%.cpp}.h"
            [[ -f "${hdr}" ]] && grep -qF "${t}" "${hdr}" 2>/dev/null && return 0
            case "${f}" in
                */Private/*)
                    hdr="${f/\/Private\///Public/}"
                    hdr="${hdr%.cpp}.h"
                    [[ -f "${hdr}" ]] && grep -qF "${t}" "${hdr}" 2>/dev/null && return 0
                    ;;
            esac
            ;;
    esac
    return 1
}

# ONE PASS PER CLASS, NOT PER SETTER. The gated file set depends only on the
# class, so it is computed once and the candidate files' non-comment lines are
# concatenated; each setter is then a single grep over that corpus. The naive
# per-setter x per-file loop took ~20s on the real module, which matters
# because the sweep's own tests invoke the sweep -- and the checker -- several
# times over.
: > "${WORK}/uncalled.txt"
cut -d' ' -f1 "${WORK}/pairs.txt" | sort -u > "${WORK}/classes.txt"
while IFS= read -r cls; do
    [[ -n "${cls}" ]] || continue
    base="${cls#F}"   # FFlowVizAlphaViewModel -> FlowVizAlphaViewModel

    : > "${WORK}/corpus.txt"
    while IFS= read -r f; do
        [[ -n "${f}" ]] || continue
        case "${f}" in
            */"${base}".h|*/"${base}".cpp) continue ;;
        esac
        mentions_type "${f}" "${cls}" || continue
        grep -vE '^[[:space:]]*(//|\*|/\*)' "${f}" 2>/dev/null >> "${WORK}/corpus.txt"
    done < "${WORK}/prod_files.txt"

    while IFS=' ' read -r pcls setter; do
        [[ "${pcls}" == "${cls}" && -n "${setter}" ]] || continue
        if ! grep -qE "((\.|->)${setter}[[:space:]]*\(|&${cls}::${setter}\b)" \
            "${WORK}/corpus.txt" 2>/dev/null; then
            printf '%s %s\n' "${base}" "${setter}" >> "${WORK}/uncalled.txt"
        fi
    done < "${WORK}/pairs.txt"
done < "${WORK}/classes.txt"
sort -o "${WORK}/uncalled.txt" "${WORK}/uncalled.txt"

# --- 4. The allowlist ---------------------------------------------------------
#
# Format: <ViewModelName>::<Setter>:<reason>, e.g.
#   FlowVizProbeViewModel::SetProbeReading:written by the async readback, tracked as 2g
#
# An allowlist is also the obvious way to defeat this checker, so it is held to
# the same rules as frozen_params_allow.txt: no reason means UNSCORED, an entry
# whose setter IS called means UNSCORED (a stale entry pre-silences whatever
# stops being called under that name next), and an entry naming a setter the
# harvest never saw means UNSCORED (it is either a typo or a form the scan
# cannot see -- both mean the exemption is not being measured).

ALLOW_FILE="${MODULE_DIR}/uncalled_setters_allow.txt"
: > "${WORK}/allowed.txt"
if [[ -f "${ALLOW_FILE}" ]]; then
    while IFS= read -r line; do
        line="${line%%$'\r'}"
        [[ -z "${line}" || "${line}" =~ ^[[:space:]]*# ]] && continue
        # <Class>::<Setter>:<reason> -- split on the double colon first, so the
        # single-colon reason separator cannot be confused with it.
        if [[ "${line}" != *::* ]]; then
            echo "check_uncalled_setters: UNSCORED" >&2
            echo "  allowlist entry '${line}' is not of the form Class::Setter:reason." >&2
            exit 2
        fi
        cls_part="${line%%::*}"
        rest="${line#*::}"
        if [[ "${rest}" != *:* ]]; then
            echo "check_uncalled_setters: UNSCORED" >&2
            echo "  allowlist entry '${line}' has no reason after the setter name." >&2
            echo "  The reason is what a reviewer reads to decide the exemption is" >&2
            echo "  honest; without one this file is just a mute button." >&2
            exit 2
        fi
        set_part="${rest%%:*}"
        reason="${rest#*:}"
        if [[ -z "${reason// /}" ]]; then
            echo "check_uncalled_setters: UNSCORED" >&2
            echo "  allowlist entry '${line}' has an empty reason." >&2
            exit 2
        fi
        # Findings and this file both use the F-less class name; pairs.txt keeps
        # the harvested form. Accept either spelling of the class in the entry.
        # Strip the type prefix only when it is actually there: a blind ${x#F}
        # would eat the first letter of the F-less form itself.
        case "${cls_part}" in
            FF*) base_cls="${cls_part#F}" ;;
            *)   base_cls="${cls_part}" ;;
        esac
        if ! grep -qE "^F?${base_cls} ${set_part}\$" "${WORK}/pairs.txt"; then
            echo "check_uncalled_setters: UNSCORED" >&2
            echo "  allowlist names '${cls_part}::${set_part}', which the harvest did not" >&2
            echo "  find. Either a typo or a declaration form the scan cannot see --" >&2
            echo "  both mean this exemption is not being measured." >&2
            exit 2
        fi
        if ! grep -qxF "${base_cls} ${set_part}" "${WORK}/uncalled.txt"; then
            echo "check_uncalled_setters: UNSCORED" >&2
            echo "  allowlist names '${cls_part}::${set_part}', which HAS a production" >&2
            echo "  caller. A stale entry pre-silences whatever stops being called" >&2
            echo "  under that name next; remove it." >&2
            exit 2
        fi
        printf '%s %s\n' "${base_cls}" "${set_part}" >> "${WORK}/allowed.txt"
    done < "${ALLOW_FILE}"
fi
sort -u -o "${WORK}/allowed.txt" "${WORK}/allowed.txt"

if [[ -s "${WORK}/allowed.txt" ]]; then
    grep -vxFf "${WORK}/allowed.txt" "${WORK}/uncalled.txt" > "${WORK}/findings.txt" || true
    ALLOWED_COUNT="$(wc -l < "${WORK}/allowed.txt" | tr -d ' ')"
else
    cp "${WORK}/uncalled.txt" "${WORK}/findings.txt"
    ALLOWED_COUNT=0
fi

# --- 5. Verdict ---------------------------------------------------------------

if [[ -s "${WORK}/findings.txt" ]]; then
    FOUND="$(wc -l < "${WORK}/findings.txt" | tr -d ' ')"
    echo "check_uncalled_setters: UNCALLED SETTER(S) -- ${FOUND} of ${SETTER_COUNT}"
    echo
    echo "No production code calls these. Whatever default the view model's member"
    echo "initialiser holds is the only value a shipped build can render with --"
    echo "the parameter-level scan reports them as written, because it asks about"
    echo "the hop below this one. Calls from Private/Tests/ are deliberately not"
    echo "counted: a test that calls the setter itself cannot reveal that nothing"
    echo "else does."
    echo
    awk '{ printf "  %s::%s\n", $1, $2 }' "${WORK}/findings.txt"
    exit 1
fi

echo "check_uncalled_setters: every view model setter has a production caller."
echo "  ${SETTER_COUNT} setters across ${CLASS_COUNT} view models, all reachable from"
echo "  production code (${ALLOWED_COUNT} allowlisted as deliberately uncalled, each"
echo "  verified still uncalled)."
exit 0
