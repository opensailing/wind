#!/bin/bash
# Find helpers that collide when UBT folds several .cpp files into one unity blob.
#
# WHY THIS EXISTS
#
# Two anonymous namespaces in the SAME translation unit are the SAME namespace.
# UBT concatenates several .cpp files into Module.<Name>.N.cpp, so two files that
# each define `IsFiniteVector` in `namespace { }` are a redefinition error the
# moment they land in the same blob.
#
# WHAT MAKES IT A TRAP RATHER THAN A TYPO
#
# Adaptive unity compiles RECENTLY EDITED files standalone. So the agent that
# writes the duplicate builds clean, commits, and the tree breaks for the NEXT
# person -- whose own changes are unrelated. Measured 2026-08-05: commit 8b27be9
# built green for its author and left HEAD at `Result: Failed` with 3
# redefinition errors, blocking two other agents mid-task.
#
# Which files share a blob is decided by UBT's bin-packing, and it CHANGES as
# files are added. A duplicate that does not collide today collides tomorrow with
# no edit to either file. So the check is "is this name defined in more than one
# file", not "does it collide right now".
#
# THE FIX THIS ARGUES FOR
#
# A per-file named namespace (`namespace FlowVizClipViewModelLocal`), which the
# module already uses at FlowVizVolumeRayMarchDispatcher.cpp:25. That makes the
# collision impossible by construction instead of fixing instances that recur.
#
# ANTI-VACUITY
#
# A scanner that extracts nothing reports "no collisions" in exactly the words it
# uses for a clean tree. So this prints the symbol count and FAILS on an
# implausibly small one: an empty match set must never read as a pass.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="${1:-${SCRIPT_DIR}/../Plugins/FlowVizRuntime/Source/FlowVizRuntime/Private}"

# Below this MANY EXTRACTABLE SYMBOLS IN TOTAL, assume the extractor is broken
# rather than the tree unusually clean.
#
# The floor is deliberately applied to the total, not to the at-risk subset.
# Those are different questions and conflating them makes the check unable to
# report the one outcome we are working toward: a fully qualified tree has ZERO
# at-risk symbols, and calling that UNSCORED forever would mean the fix could
# never be confirmed. So the total answers "does the extractor work" and the
# at-risk subset answers "does this tree collide".
MIN_PLAUSIBLE_SYMBOLS="${MIN_PLAUSIBLE_SYMBOLS:-40}"

if [[ ! -d "${MODULE_DIR}" ]]; then
    echo "check_unity_collisions: no such directory: ${MODULE_DIR}" >&2
    exit 2
fi

# Not `mapfile`: macOS ships bash 3.2, where it does not exist. It failed here
# loudly rather than silently, but only because the plausibility floor below
# caught the empty result -- an unguarded scan would have reported a clean tree.
# Every .cpp with a namespace of ANY form, including named ones that are never
# re-exported. Those contribute no risk, but they are what makes a correctly
# qualified tree measurable: discovery that only matched anonymous blocks and
# `using` lines would hand the extractor nothing once the fix lands, and the
# health control below would then read the goal state as a broken scan.
FILES=()
while IFS= read -r f; do
    [[ -n "${f}" ]] && FILES+=("${f}")
done < <(grep -rlE '^namespace([[:space:]]|$)|^using namespace ' \
    --include='*.cpp' "${MODULE_DIR}" 2>/dev/null | sort -u)

if [[ "${#FILES[@]}" -eq 0 ]]; then
    echo "check_unity_collisions: found no namespaces of any form under ${MODULE_DIR}." >&2
    echo "  That is more likely a broken scan than a clean tree. UNSCORED." >&2
    exit 2
fi

SYMS="$(perl -0777 -ne '
my $file = $ARGV;
# Anonymous namespaces, and named ones that are re-exported with a file-scope
# `using namespace X;`. The rename to <File>Local fixes the DECLARATION, but a
# `using` drags every name back into the unity TU at the point it appears, so two
# files whose helpers share a name collide again -- with the namespace sitting
# right there in the source looking like it prevents exactly that. Checking only
# anonymous blocks would have called the fixed tree clean while leaving the
# hazard reachable.
#
# Every symbol is emitted with a RISK FLAG: "risk" if it lands in the enclosing
# TU (anonymous block, or named-and-re-exported), "safe" if it stays behind a
# qualifier. The total answers "is the extractor working"; the risk subset
# answers "does this tree collide". Folding those two into one number would make
# a correctly qualified tree -- zero at-risk symbols -- indistinguishable from a
# broken scan, so the fix we are working toward could never be confirmed.
my %reexported = map { $_ => 1 } ($_ =~ /^using\s+namespace\s+([A-Za-z_][A-Za-z0-9_]*)\s*;/mg);
while (/^namespace\s+([A-Za-z_][A-Za-z0-9_]*)\s*\n\s*\{/mg) {
  my $ns = $1;
  my $risk = $reexported{$ns} ? "risk" : "safe";
  my $start = pos($_);
  my $depth = 1; my $i = $start;
  while ($depth > 0 && $i < length($_)) {
    my $c = substr($_, $i, 1);
    $depth++ if $c eq "{";
    $depth-- if $c eq "}";
    $i++;
  }
  my $body = substr($_, $start, $i - $start);
  while ($body =~ /^\t?(?:const\s+)?[A-Za-z_][A-Za-z0-9_:<>\*\&\s]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(/mg) {
    print "$1\t$file\t$risk\n";
  }
  while ($body =~ /^\t?const\s+TCHAR\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*=/mg) {
    print "$1\t$file\t$risk\n";
  }
}
pos($_) = 0;
while (/^namespace\s*\n\s*\{/mg) {
  my $start = pos($_);
  my $depth = 1; my $i = $start;
  while ($depth > 0 && $i < length($_)) {
    my $c = substr($_, $i, 1);
    $depth++ if $c eq "{";
    $depth-- if $c eq "}";
    $i++;
  }
  my $body = substr($_, $start, $i - $start);
  # An anonymous namespace is always at risk: there is no qualifier that could
  # keep it out of the enclosing TU.
  # Function definitions at the top level of the block.
  while ($body =~ /^\t?(?:const\s+)?[A-Za-z_][A-Za-z0-9_:<>\*\&\s]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(/mg) {
    print "$1\t$file\trisk\n";
  }
  # File-scope constants, which collide just as loudly.
  while ($body =~ /^\t?const\s+TCHAR\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*=/mg) {
    print "$1\t$file\trisk\n";
  }
}
' "${FILES[@]}" 2>/dev/null | sort -u)"

SYM_COUNT=$(printf '%s' "${SYMS}" | grep -c . )
SYM_COUNT=${SYM_COUNT:-0}

# The at-risk subset: symbols that actually land in the enclosing unity TU.
RISKY="$(printf '%s\n' "${SYMS}" | awk -F'\t' '$3 == "risk" { print $1 "\t" $2 }')"
RISK_COUNT=$(printf '%s' "${RISKY}" | grep -c . )
RISK_COUNT=${RISK_COUNT:-0}

printf 'scanned %d file(s), extracted %d symbol(s), %d of them exposed to the unity TU\n' \
    "${#FILES[@]}" "${SYM_COUNT}" "${RISK_COUNT}"

# The health control. An empty (or near-empty) EXTRACTION passes every per-symbol
# test below, so it is UNSCORED rather than a clean bill of health. Note this is
# the TOTAL, not the at-risk count: zero at-risk symbols is the goal state, and
# must be reportable as a pass.
if [[ "${SYM_COUNT}" -lt "${MIN_PLAUSIBLE_SYMBOLS}" ]]; then
    echo >&2
    echo "check_unity_collisions: only ${SYM_COUNT} symbols extracted, below the" >&2
    echo "  plausibility floor of ${MIN_PLAUSIBLE_SYMBOLS}. The extractor is probably not matching" >&2
    echo "  this file's style. Refusing to report a pass. UNSCORED." >&2
    exit 2
fi

DUPES="$(printf '%s\n' "${RISKY}" | cut -f1 | sort | uniq -d)"

if [[ -z "${DUPES}" ]]; then
    echo "no cross-file duplicates: every name reaching the unity TU is unique to its file"
    exit 0
fi

echo
echo "UNITY-BUILD COLLISION: these names reach the enclosing translation unit"
echo "from more than one .cpp -- via an anonymous namespace, or a named one that"
echo "a file-scope 'using namespace' re-exports. Any two that UBT packs into the"
echo "same Module.*.cpp collide: a redefinition in the anonymous case, an"
echo "ambiguous call in the re-exported case. The packing changes as files are"
echo "added, so a pair that builds today can break with no edit to either."
echo
while IFS= read -r sym; do
    [[ -n "${sym}" ]] || continue
    echo "  ${sym}"
    printf '%s\n' "${RISKY}" | awk -F'\t' -v s="${sym}" '$1 == s { print "      " $2 }'
done <<< "${DUPES}"
echo
echo "Fix: put the helpers in a per-file named namespace and QUALIFY the call"
echo "sites (FooLocal::Helper(...)), as"
echo "Private/Render/FlowVizVolumeRayMarchDispatcher.cpp:25 does. Do NOT add"
echo "'using namespace FooLocal;' -- that re-exports every name and puts the"
echo "collision straight back, as an ambiguity instead of a redefinition."
exit 1
