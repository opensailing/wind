#!/bin/bash
# Does a clean checkout of this repository actually run?
#
# This test exists because it did not, and the failure was expensive to find.
#
# FlowVizRuntime.cpp maps /Plugin/FlowViz to the plugin's Shaders directory at
# module startup. AddShaderSourceDirectoryMapping does not tolerate a missing
# directory -- it fails a check, and on macOS that check turns into SIGSEGV
# while the module is still loading:
#
#     Assertion failed: FPaths::DirectoryExists(.../FlowVizRuntime/Shaders)
#     LogMac: === Critical error: ===
#     SIGSEGV: invalid attempt to access memory at address 0x3
#       ...AddShaderSourceDirectoryMapping...
#       ...FFlowVizRuntimeModule::StartupModule...
#
# Shaders/ existed in the tree it was written in, and was empty. Git does not
# track empty directories, so it existed in exactly one place on this machine
# and nowhere else -- not in a fresh clone, not in either mutation worktree.
# Three mutation campaigns died in it, and the harness of the day scored the
# resulting crash as `killed`, which is how a check that never ran gets
# recorded as verified.
#
# What makes this class of bug worth a permanent test is that it is invisible
# from the tree that has the directory. Every local build works. Only someone
# else's checkout breaks, which in this repo means CI, a new contributor, and
# every worktree-isolated mutation run.
#
# The check is therefore against git's index, not the filesystem: a path is
# present for other people only if git is carrying it.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "${REPO_ROOT}" || exit 2

PASS=0
FAIL=0

# require_tracked_dir <path> <why it must exist>
#
# Passes only if git has at least one file under the directory. A directory
# that exists locally but holds nothing tracked FAILS, because that is the
# precise shape of the defect: present here, absent everywhere else.
require_tracked_dir() {
    local dir="$1" why="$2"
    local count
    count=$(git ls-files -- "${dir}" | wc -l | tr -d ' ')
    if [[ "${count}" -gt 0 ]]; then
        echo "  ok    ${dir} survives a clean checkout (${count} tracked)"
        PASS=$((PASS+1))
    else
        echo "  FAIL  ${dir} is not in git, so a fresh clone will not have it."
        echo "        ${why}"
        if [[ -d "${dir}" ]]; then
            echo "        It exists in THIS tree, which is why local builds pass."
        fi
        FAIL=$((FAIL+1))
    fi
}

echo "checkout completeness:"

# Every directory below is one the engine requires to exist at startup, i.e.
# one whose absence is a crash rather than a degraded feature.
require_tracked_dir "FlowViz/Plugins/FlowVizRuntime/Shaders" \
    "FlowVizRuntime.cpp:23 maps it via AddShaderSourceDirectoryMapping, which asserts on a missing directory and takes the module down during StartupModule."

echo
echo "checkout tests: ${PASS} passed, ${FAIL} failed"
[[ "${FAIL}" -eq 0 ]]
