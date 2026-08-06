#!/bin/bash
# Find bash-4-only constructs in scripts that will be run by bash 3.2.
#
# WHY THIS EXISTS
#
# macOS ships /bin/bash 3.2.57 and always will -- bash 4 went GPLv3 and Apple
# froze at the last GPLv2 release. `mapfile`, `readarray`, `declare -A` and
# `${x^^}` all arrived in bash 4. A script whose shebang is `#!/bin/bash`
# therefore cannot use them on this machine.
#
# WHAT MAKES IT A TRAP RATHER THAN A TYPO
#
# run_harness_tests.sh runs every test as `cmd=(bash "${path}")` -- deliberately,
# so that a `chmod -x` cannot silently drop a test from the sweep. But `bash` on
# PATH here is Homebrew's 5.3.9, so the runner ALSO overrides every test's
# shebang. A test can depend on bash 4, pass in every sweep, and die the moment
# a human runs it directly to debug it:
#
#     $ ./test_mutate_per_arm_logs.sh
#     line 258: mapfile: command not found
#     line 261: ARM_TEST_LOGS: unbound variable
#
# `#!/usr/bin/env bash` is not a fix. It finds whatever bash is first on PATH,
# which is 5.3.9 here and 3.2.57 on any box without Homebrew bash -- CI, a fresh
# checkout, a login shell with a trimmed PATH. The guaranteed floor on macOS is
# 3.2, so this checker applies the same rule to both shebangs.
#
# WHY A CHECKER AND NOT FOUR EDITS
#
# The hazard was already documented in three separate comments, one of which
# records that it once made an entire test file pass VACUOUSLY. It recurred
# anyway, four more times, because prose that describes a hazard does not
# prevent it. Nothing rejected the construct, so every new script started the
# trap fresh.
#
# TELLING CODE FROM COMMENTARY
#
# Those three warnings all name `mapfile` outright, and this file names every
# construct it hunts. A checker that grepped for the word would flag the prose
# warning against the word, the tree could never read clean, and the fix could
# never be confirmed. So this strips comments, single-quoted strings and
# heredoc bodies before matching, and requires builtins to appear in COMMAND
# position rather than anywhere on the line.
#
# ANTI-VACUITY
#
# A scan that discovers no files reports "portable" in the same words as a
# genuinely portable tree. So the file count is printed and floored: an
# implausibly small scan is UNSCORED, not clean.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${1:-${SCRIPT_DIR}}"

# Below this many DISCOVERED SHELL SCRIPTS, assume the scan is broken rather
# than the tree unusually small. Tools/ holds ~29; a run that finds a handful
# has almost certainly been pointed at the wrong path.
MIN_PLAUSIBLE_SCRIPTS="${MIN_PLAUSIBLE_SCRIPTS:-20}"

if [[ ! -d "${ROOT}" ]]; then
    echo "check_shell_portability: no such directory: ${ROOT}" >&2
    exit 2
fi

ROOT="$(cd "${ROOT}" && pwd)"

# The scanner. Written to a temp file inside a quoted heredoc rather than
# inlined: a heredoc body is skipped by the scan below, so this checker can
# name every construct it hunts without flagging itself.
SCANNER="$(mktemp)"
trap 'rm -f "${SCANNER}"' EXIT

cat > "${SCANNER}" <<'PERL_SCANNER'
# Emit "<line>\t<construct>" for every bash-4-only construct in the file.
#
# State is carried ACROSS lines: a multi-line single-quoted string (an embedded
# awk or perl program, which this harness is full of) must stay inert for its
# whole length. Resetting per line would treat lines 2..n of such a block as
# live code -- and those blocks are exactly where builtin names appear as text.

my $state = 0;          # 0 normal, 1 in single quotes, 2 in double quotes
my $heredoc = undef;    # terminator word while inside a heredoc body
my $heredoc_dash = 0;   # <<- strips leading tabs from the terminator
my $lineno = 0;

# Strip inert text from a line, preserving offsets so nothing shifts.
#   keep_dq=0 -> double-quoted content removed too (for command-position hunts)
#   keep_dq=1 -> double-quoted content kept ("${X^^}" is real code)
sub strip {
    my ($s, $keep_dq) = @_;
    my $out = "";
    my $i = 0;
    my $n = length($s);
    while ($i < $n) {
        my $c = substr($s, $i, 1);
        if ($state == 0) {
            if ($c eq "\\") { $out .= "  "; $i += 2; next; }
            if ($c eq "\x27") { $state = 1; $out .= " "; $i++; next; }
            if ($c eq "\"")   { $state = 2; $out .= ($keep_dq ? "\"" : " "); $i++; next; }
            if ($c eq "#") {
                # A comment only where a word could start. This is what keeps
                # ${#ARR[@]}, ${VAR#prefix} and $# from eating the line.
                my $prev = $i > 0 ? substr($s, $i - 1, 1) : "";
                last if ($i == 0 || $prev =~ /\s/);
                $out .= $c; $i++; next;
            }
            $out .= $c; $i++; next;
        }
        elsif ($state == 1) {
            $state = 0 if $c eq "\x27";
            $out .= " "; $i++; next;
        }
        else {
            if ($c eq "\\") { $out .= "  "; $i += 2; next; }
            if ($c eq "\"") { $state = 0; $out .= ($keep_dq ? "\"" : " "); $i++; next; }
            $out .= ($keep_dq ? $c : " "); $i++; next;
        }
    }
    return $out;
}

while (my $line = <>) {
    $lineno++;
    $line =~ s/\r?\n$//;

    # Inside a heredoc body nothing is code. Fixture scripts are written with
    # heredocs, so their contents are data no matter what they say.
    if (defined $heredoc) {
        my $t = $line;
        $t =~ s/^\t+// if $heredoc_dash;
        $heredoc = undef if $t eq $heredoc;
        next;
    }

    # A heredoc opener must be found on text that still HAS its quotes:
    # `<<'EOF'` and `<<"EOF"` are the common forms, and the strip below turns
    # the terminator into blanks. Detected here, applied after the hunts, so a
    # violation on the same line is still reported.
    my $opener = $line;
    $opener =~ s/^([^#\x27"]*)#.*$/$1/;   # only a comment before any quote
    my $starts_heredoc = undef;
    my $starts_dash = 0;
    if ($opener =~ /<<(-?)\s*(["\x27]?)([A-Za-z_][A-Za-z0-9_]*)\2/ && $opener !~ /<<</) {
        $starts_dash = ($1 eq "-") ? 1 : 0;
        $starts_heredoc = $3;
    }

    my $saved = $state;
    my $nq = strip($line, 0);
    $state = $saved;
    my $sq = strip($line, 1);

    # A quoted heredoc terminator leaves the quote state balanced; strip() saw
    # only one of the pair on this line, so correct it rather than carrying a
    # phantom open quote into the body.
    $state = 0 if defined $starts_heredoc;

    # --- command-position builtins -------------------------------------------
    #
    # `mapfile_compat() { ... }` is not a call to mapfile, and a label reading
    # "declare -A is caught" is not a declaration. Anchoring to command
    # position is what separates them.
    my $CMD = qr/(?:^|[;&|(){]|\bthen\b|\bdo\b|\belse\b|\belif\b)\s*/;

    print "$lineno\tmapfile\n"    if $nq =~ /$CMD mapfile\b/x;
    print "$lineno\treadarray\n"  if $nq =~ /$CMD readarray\b/x;
    print "$lineno\tdeclare -A (associative array)\n"
        if $nq =~ /$CMD (?:declare|typeset|local)\s+-[a-zA-Z]*A/x;

    # `|&` is bash 4 shorthand for 2>&1 |. Not `||`, and not a `|` followed by
    # a background `&`, which cannot occur.
    print "$lineno\t|& pipe\n" if $nq =~ /[^|]\|&/ || $nq =~ /^\|&/;

    # --- case-modifying expansions -------------------------------------------
    #
    # Matched with double quotes KEPT: "${X^^}" is the idiomatic form, and a
    # literal ${x^^} cannot appear in a double-quoted string anyway -- it would
    # expand. In single quotes it is text, and already stripped.
    print "$lineno\t\${x^^} case expansion\n"
        if $sq =~ /\$\{[A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]*\])?(?:\^\^?|,,?)/;

    # Applied last so a violation on the opener line is still reported. `<<<`
    # is a herestring and `< <(` is process substitution; neither opens a body.
    if (defined $starts_heredoc) {
        $heredoc_dash = $starts_dash;
        $heredoc = $starts_heredoc;
    }
}
PERL_SCANNER

# --- discovery ----------------------------------------------------------------
#
# By shebang, not by extension. A shell script with no .sh suffix is still a
# shell script, and this is the discovery step whose failure mode is "found
# nothing, reported clean" -- so it is deliberately the broad one.
#
# Not `mapfile`, for the reason this file exists.
SCRIPTS=()
while IFS= read -r f; do
    [[ -n "${f}" ]] || continue
    head -1 "${f}" 2>/dev/null | grep -qE '^#!.*[/ ](bash|sh)([[:space:]]|$)' \
        && SCRIPTS+=("${f}")
done < <(find "${ROOT}" -type f ! -path '*/.git/*' 2>/dev/null | sort)

COUNT="${#SCRIPTS[@]}"

VIOLATIONS=0
REPORT=""
for f in "${SCRIPTS[@]:-}"; do
    [[ -n "${f}" ]] || continue
    rel="${f#${ROOT}/}"
    while IFS=$'\t' read -r lineno construct; do
        [[ -n "${lineno}" ]] || continue
        REPORT="${REPORT}  ${rel}:${lineno}: ${construct}"$'\n'
        VIOLATIONS=$((VIOLATIONS + 1))
    done < <(perl "${SCANNER}" "${f}" 2>/dev/null)
done

# Violations outrank the floor. A tree that is both too small to judge AND
# demonstrably broken has one answer that is certainly true, and burying it
# under UNSCORED would hide a real finding behind a scan-quality complaint.
if [[ "${VIOLATIONS}" -gt 0 ]]; then
    # The scan size is reported on BOTH verdict paths. A count that only
    # appears when the tree is clean cannot be used to tell a clean tree from
    # a scan that found almost nothing.
    echo "NOT PORTABLE: ${VIOLATIONS} bash-4-only construct(s); scanned ${COUNT} script(s) under ${ROOT}"
    printf '%s' "${REPORT}"
    echo
    echo "  macOS /bin/bash is 3.2.57. These run under Homebrew bash and die under the"
    echo "  shebang the file itself names. Replace array reads with:"
    echo
    echo "      ARR=()"
    echo "      while IFS= read -r x; do [[ -n \"\${x}\" ]] && ARR+=(\"\${x}\"); done < <(...)"
    echo
    exit 1
fi

if [[ "${COUNT}" -lt "${MIN_PLAUSIBLE_SCRIPTS}" ]]; then
    echo "UNSCORED: scanned ${COUNT} script(s) under ${ROOT}, below the floor of ${MIN_PLAUSIBLE_SCRIPTS}." >&2
    echo "  That is more likely a broken scan than a small tree. No verdict." >&2
    exit 2
fi

echo "scanned ${COUNT} script(s): portable to bash 3.2."
exit 0
