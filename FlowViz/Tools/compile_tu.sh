#!/bin/bash
# Compile individual translation units using UBT's own per-TU flags, so one
# broken file in the tree cannot stop you verifying your own.
#
# WHEN TO USE THIS
#
# A fatal error in ONE file halts the whole UBT build after a single action.
# When a peer lands a TDD test whose header does not exist yet, or a mid-edit
# file that will not parse, `Build.sh` dies before it reaches anything of yours
# and you cannot tell your own state from theirs. This replays the response file
# UBT already generated for a single .cpp, so only that file is compiled.
#
#   ./Tools/compile_tu.sh FlowVizSession FlowVizSessionTest
#   ./Tools/compile_tu.sh --all-ui
#
# WHAT A CLEAN SWEEP HERE DOES NOT MEAN
#
# This checks COMPILATION. It does not check LINKAGE. Each TU compiles to
# /dev/null in isolation, so a declared-but-undefined FLOWVIZRUNTIME_API export
# -- a header whose .cpp was never written, which is exactly what blocked this
# module on 2026-08-05 -- is invisible to every unit here and appears only when
# the linker runs. Do not close a task on this. You still owe a real
# `Result: Succeeded` with a nonzero action count (see Docs/BUILD.md).
#
# WHY THERE IS A CONTROL
#
# The check is worth running only if it can come back red, and there are two
# quiet ways it cannot: a stale or missing .rsp (nothing gets compiled), and a
# result parser that mis-scores. Both look like success. So every run first
# injects a guaranteed error into a scratch copy and requires clang to reject
# it. If the control does not fail, the run is UNSCORED and no OK is printed.

set -uo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_SOURCE="${FLOWVIZ_ENGINE_SOURCE:-/Users/Shared/Epic Games/UE_5.8/Engine/Source}"
RSP_DIR="${PROJECT_DIR}/Plugins/FlowVizRuntime/Intermediate/Build/Mac/arm64/UnrealEditor/Development/FlowVizRuntime"
SRC_ROOT="${PROJECT_DIR}/Plugins/FlowVizRuntime/Source/FlowVizRuntime"

UI_UNITS=(
    FlowVizSession FlowVizSessionTest
    FlowVizClipViewModel FlowVizClipViewModelTest
    FlowVizProbeViewModel FlowVizProbeViewModelTest
    FlowVizSliceViewModel
    FlowVizTransferFunctionViewModel FlowVizTransferFunctionViewModelTest
    FlowVizTimelineViewModel FlowVizTimelineViewModelTest
)

# classify_tu_result <exit-code> <stderr-text> [corrupt-marker] -> ok|fail|unscored
#
# The third argument exists because of a measured bug: `n=$(grep -c error: f ||
# echo 0)` yields "0\n0", since grep -c PRINTS 0 and EXITS 1. Arithmetic on that
# string errors and silently takes the wrong branch -- it scored 11 clean
# compiles as FAIL, and with the comparison reversed would have produced 11
# false OKs. A count that is not a single integer is a broken measurement, not a
# result.
classify_tu_result() {
    local exit_code="$1" stderr_text="${2-}" corrupt="${3-}"
    if [[ -n "${corrupt}" ]]; then echo "unscored"; return; fi
    local errors
    errors=$(printf '%s' "${stderr_text}" | grep -c 'error:')
    errors=${errors:-0}
    [[ "${errors}" =~ ^[0-9]+$ ]] || { echo "unscored"; return; }
    if [[ "${exit_code}" -eq 0 ]]; then
        # clang exits 0 for warnings; a warning is not a failure or every
        # -Wunused turns the tree red.
        [[ "${errors}" -eq 0 ]] && echo "ok" || echo "unscored"
        return
    fi
    # Nonzero with no parseable diagnostic is a killed process, an OOM, or a
    # missing compiler -- never a compile failure we can attribute.
    [[ "${errors}" -gt 0 ]] && echo "fail" || echo "unscored"
}

# resolve_rsp <dir> <unit> -> path, or empty when UBT never generated one
resolve_rsp() {
    local dir="$1" unit="$2"
    local candidate="${dir}/${unit}.cpp.o.rsp"
    [[ -f "${candidate}" ]] && echo "${candidate}"
}

# classify_control_run <exit-code> <stderr-text> -> armed|broken
#
# The control must fail because the compiler REJECTED CODE, not because it could
# not find a file. A `file not found` proves clang ran; it does not prove clang
# can see into the translation unit under test, which is the property every OK
# below depends on.
classify_control_run() {
    local exit_code="$1" stderr_text="${2-}"
    [[ "${exit_code}" -eq 0 ]] && { echo "broken"; return; }
    if printf '%s' "${stderr_text}" | grep -q 'file not found'; then echo "broken"; return; fi
    if printf '%s' "${stderr_text}" | grep -q 'error:'; then echo "armed"; return; fi
    echo "broken"
}

[[ "${1-}" == "--source-only" ]] && return 0 2>/dev/null

# ---------------------------------------------------------------------------

units=()
if [[ "${1-}" == "--all-ui" ]]; then
    units=("${UI_UNITS[@]}")
else
    units=("$@")
fi
[[ ${#units[@]} -gt 0 ]] || {
    echo "usage: compile_tu.sh <UnitName>... | --all-ui"
    exit 2
}

CLANG="$(xcrun -f clang)" || { echo "no clang"; exit 1; }
cd "${ENGINE_SOURCE}" || { echo "no engine source at ${ENGINE_SOURCE}"; exit 1; }

# --- control: prove this harness can come back red -------------------------
CONTROL_UNIT="FlowVizSession"
CONTROL_SRC="${SRC_ROOT}/Private/UI/${CONTROL_UNIT}.cpp"
CONTROL_RSP="$(resolve_rsp "${RSP_DIR}" "${CONTROL_UNIT}")"
if [[ -z "${CONTROL_RSP}" || ! -f "${CONTROL_SRC}" ]]; then
    echo "UNSCORED: no control available (${CONTROL_UNIT}); run a full build once to generate .rsp files."
    exit 1
fi
SCRATCH="$(mktemp -d)"
trap 'rm -rf "${SCRATCH}"' EXIT
cp "${CONTROL_SRC}" "${SCRATCH}/ctl.cpp"
# A call to a member that certainly does not exist: rejected by the type
# checker, so it proves clang parsed this file's real contents.
printf '\nstatic void FlowVizCompileControl() { struct S {}; S s; s.NoSuchMemberXYZ(); }\n' >> "${SCRATCH}/ctl.cpp"
sed "s|${CONTROL_SRC}|${SCRATCH}/ctl.cpp|" "${CONTROL_RSP}" > "${SCRATCH}/ctl.rsp"
CONTROL_ERR="${SCRATCH}/ctl.err"
"${CLANG}" @"${SCRATCH}/ctl.rsp" -o /dev/null 2>"${CONTROL_ERR}"
CONTROL_EXIT=$?
if [[ "$(classify_control_run "${CONTROL_EXIT}" "$(cat "${CONTROL_ERR}")")" != "armed" ]]; then
    echo "UNSCORED: the control did not fail, so a clean result here would mean nothing."
    head -5 "${CONTROL_ERR}" | sed 's/^/    /'
    exit 1
fi
echo "control: clang rejected an injected error -- this check can come back red"
echo

# --- the real units ---------------------------------------------------------
ok=0; bad=0; unscored=0
for unit in "${units[@]}"; do
    rsp="$(resolve_rsp "${RSP_DIR}" "${unit}")"
    if [[ -z "${rsp}" ]]; then
        printf '  %-40s UNSCORED (no .rsp -- nothing was compiled)\n' "${unit}"
        unscored=$((unscored + 1))
        continue
    fi
    err="${SCRATCH}/${unit}.err"
    "${CLANG}" @"${rsp}" -o /dev/null 2>"${err}"
    rc=$?
    case "$(classify_tu_result "${rc}" "$(cat "${err}")")" in
        ok)   printf '  %-40s OK\n' "${unit}"; ok=$((ok + 1)) ;;
        fail) printf '  %-40s FAIL\n' "${unit}"
              grep -m3 'error:' "${err}" | sed 's/^/      /'
              bad=$((bad + 1)) ;;
        *)    printf '  %-40s UNSCORED (exit=%s, no attributable diagnostic)\n' "${unit}" "${rc}"
              unscored=$((unscored + 1)) ;;
    esac
done

printf '\ncompiled=%d failed=%d unscored=%d of %d\n' "${ok}" "${bad}" "${unscored}" "${#units[@]}"
echo "NOTE: this is COMPILATION only. Undefined exports surface at LINK time --"
echo "      you still owe a Build.sh 'Result: Succeeded' with a nonzero action count."
[[ "${bad}" -eq 0 && "${unscored}" -eq 0 ]]
