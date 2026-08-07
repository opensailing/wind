#!/usr/bin/env bash
#
# Run every test in Tools/tests and report a total that means something.
#
#   Tools/run_harness_tests.sh
#   FLOWVIZ_HARNESS_SKIP='test_slow.sh test_other.sh' Tools/run_harness_tests.sh
#
# Tested by: Tools/tests/test_run_harness_tests.sh
#
# WHY THIS EXISTS. Tools/tests held fourteen test files and nothing called any
# of them. Each was run by hand once, on the day it was written, and never
# again -- so the harness that decides every mutation verdict in this project
# was itself unverified from the moment its author moved on. A test with no
# caller is not coverage; it is a file that once passed.
#
# WHAT THIS RUNNER REFUSES TO CALL A PASS. Every failure this repo has actually
# shipped in a reporting layer has the same shape: something did not run, and
# the report could not tell that from success.
#
#   - "0 exports, all defined" scanned as clean while the linker found four
#     errors in those same files. An empty match set passes every check.
#   - a suite reported 49/49 twice, from two different suites, one of which
#     never touched the renderer.
#   - five of six mutation arms "killed" by a harness that exited 4 and tested
#     nothing.
#
# So exit 0 alone is NOT a pass here. A test must print a summary line saying
# how many checks it ran, and that number must be greater than zero. A script
# that exits 0 having verified nothing -- body commented out, fixture missing,
# early return -- is reported NOTRUN and fails the run, because by exit code it
# is identical to one that verified everything.
#
# NOTRUN IS ALSO DISTINCT FROM FAIL, in the other direction: a missing
# interpreter, an unreadable file, a crash before the first check. "Ran and
# found a defect" and "never ran" are different facts and want different
# actions, and collapsing them is how a broken harness gets read as a broken
# implementation. See the repo memory note nonzero-exit-can-mean-never-ran.
#
# THE SUMMARY DIALECTS. These files were written over weeks and print their
# totals four different ways:
#
#     verdict tests: 12 passed, 0 failed
#     mutation_guard.sh: 9 checks, all passed
#     run_tests.sh summary: 8/8 checks passed
#     3 of 5 arms did not. See above.
#
# All four are parsed. The alternative -- rewriting fourteen working files to
# one format as a precondition for running them -- is how a runner ends up
# never landing. But a file whose summary this cannot parse is NOTRUN, never a
# silent pass: the whole point is that unparseable must not mean fine.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTS_DIR="${FLOWVIZ_HARNESS_TESTS_DIR:-${HERE}/tests}"
SKIP="${FLOWVIZ_HARNESS_SKIP:-}"

# Extract "how many checks did this test run" from its output.
#
# Prints the count, or nothing if no summary was recognised. Deliberately reads
# the LAST matching line: several of these files print per-section totals on the
# way through, and the run-level total is the one at the end.
count_checks() {
    local out="$1" n=""

    # "verdict tests: 12 passed, 0 failed"  /  "12 passed, 0 failed"
    n="$(grep -oE '[0-9]+ passed, [0-9]+ failed' <<<"${out}" | tail -1)"
    if [[ -n "${n}" ]]; then
        local p f
        p="$(grep -oE '^[0-9]+' <<<"${n}")"
        f="$(grep -oE '[0-9]+ failed' <<<"${n}" | grep -oE '^[0-9]+')"
        printf '%d\n' "$((p + f))"
        return 0
    fi

    # "mutation_guard.sh: 9 checks, all passed"  /  "... 2 FAILED"
    n="$(grep -oE '[0-9]+ checks, ' <<<"${out}" | tail -1 | grep -oE '^[0-9]+')"
    if [[ -n "${n}" ]]; then printf '%d\n' "${n}"; return 0; fi

    # "run_tests.sh summary: 8/8 checks passed"  /  "3 of 8 checks FAILED"
    n="$(grep -oE '[0-9]+/[0-9]+ checks passed' <<<"${out}" | tail -1 | grep -oE '/[0-9]+' | tr -d /)"
    if [[ -n "${n}" ]]; then printf '%d\n' "${n}"; return 0; fi
    n="$(grep -oE '[0-9]+ of [0-9]+ checks FAILED' <<<"${out}" | tail -1 | grep -oE 'of [0-9]+' | grep -oE '[0-9]+')"
    if [[ -n "${n}" ]]; then printf '%d\n' "${n}"; return 0; fi

    # march_differential.sh: "all 5 arms ..." / "3 of 5 arms did not."
    n="$(grep -oE '[0-9]+ (of [0-9]+ )?arms' <<<"${out}" | tail -1 | grep -oE '[0-9]+$')"
    if [[ -n "${n}" ]]; then printf '%d\n' "${n}"; return 0; fi

    return 1
}

# Discover tests. Both languages: test_equivalent_mutants.py is a real test in
# this directory, and a runner that globbed only *.sh would drop it with
# nothing in the report to say so.
#
# Not `mapfile`: it is bash 4, and macOS /bin/bash is 3.2.57. This file's
# `#!/usr/bin/env bash` happens to find Homebrew's 5.3.9 today, so it worked --
# on a box without Homebrew bash, FOUND came back empty and the runner reported
# "no tests found" for a directory full of tests. Discovery is the one step
# whose failure mode is a silent, plausible-looking zero.
FOUND=()
while IFS= read -r f; do
    [[ -n "${f}" ]] && FOUND+=("${f}")
done < <(
    find "${TESTS_DIR}" -maxdepth 1 -type f \( -name 'test_*.sh' -o -name 'test_*.py' \) 2>/dev/null | sort
)

if [[ "${#FOUND[@]}" -eq 0 ]]; then
    # NOT exit 0. "0 tests, 0 failures" reads as green in every summary format
    # there is, and a runner pointed at the wrong directory -- a rename, a
    # moved checkout, a typo in an override -- produces exactly this.
    echo "FAIL: no tests found in ${TESTS_DIR}" >&2
    echo "      A run that discovers nothing is a broken runner, not a clean sweep." >&2
    exit 1
fi

passed=0
failed=0
notrun=0
skipped=0
total_checks=0
failed_names=()
notrun_names=()

echo "Tools/tests (${#FOUND[@]} files):"
echo

for path in "${FOUND[@]}"; do
    name="$(basename "${path}")"

    if [[ -n "${SKIP}" ]] && grep -qw -- "${name}" <<<"${SKIP}"; then
        # Named, never silent. Silent truncation reads as "covered everything".
        echo "SKIP  ${name}  (excluded by FLOWVIZ_HARNESS_SKIP)"
        skipped=$((skipped + 1))
        continue
    fi

    # Run through an explicit interpreter rather than relying on the execute
    # bit: `chmod -x` is a real and completely silent way to lose a test, and
    # a file that stops running with nothing in the report is the failure this
    # runner exists to prevent.
    case "${name}" in
        *.py) cmd=(python3 "${path}") ;;
        *)    cmd=(bash "${path}") ;;
    esac

    out="$("${cmd[@]}" 2>&1)"
    status=$?

    if ! checks="$(count_checks "${out}")"; then
        # No recognisable summary. Cannot conclude it passed, whatever it
        # exited -- and MUST NOT default to pass, because "unparseable" is what
        # a test looks like after its body stops running.
        echo "NOTRUN ${name}  (exit ${status}, no check summary in output)"
        notrun=$((notrun + 1))
        notrun_names+=("${name}")
        continue
    fi

    if [[ "${checks}" -eq 0 ]]; then
        echo "NOTRUN ${name}  (summary reports 0 checks)"
        notrun=$((notrun + 1))
        notrun_names+=("${name}")
        continue
    fi

    total_checks=$((total_checks + checks))

    if [[ "${status}" -eq 0 ]]; then
        echo "ok    ${name}  (${checks} checks)"
        passed=$((passed + 1))
    else
        echo "FAIL  ${name}  (${checks} checks, exit ${status})"
        failed=$((failed + 1))
        failed_names+=("${name}")
    fi
done

# Detail for the failures, after the roster, so the roster stays readable. Every
# test is run before any output is analysed: stopping at the first failure hides
# every later one, and turns fixing the harness into a serial guessing game.
if [[ "${failed}" -gt 0 || "${notrun}" -gt 0 ]]; then
    echo
    for n in "${failed_names[@]:-}"; do
        [[ -n "${n}" ]] || continue
        echo "--- ${n} (FAILED) ---"
        case "${n}" in
            *.py) python3 "${TESTS_DIR}/${n}" 2>&1 | grep -iE 'fail|error' | head -20 | sed 's/^/  /' ;;
            *)    bash "${TESTS_DIR}/${n}" 2>&1 | grep -iE 'fail|error' | head -20 | sed 's/^/  /' ;;
        esac
    done
    for n in "${notrun_names[@]:-}"; do
        [[ -n "${n}" ]] || continue
        echo "--- ${n} (DID NOT RUN) ---"
        echo "  This test produced no check summary, so nothing can be concluded"
        echo "  from its exit code. It is not a pass. Run it directly:"
        echo "    ${TESTS_DIR}/${n}"
    done
fi

# --- tree-level checks -------------------------------------------------------
#
# Not tests of a script's behaviour but of the tree the scripts live in, so they
# run once here rather than as files in tests/. Each had tests and no scheduled
# caller, which is the condition this runner exists to end: a checker with no
# caller is not coverage either.
#
# check_frozen_params.sh is the case in point. It had tests, and it sat at
# UNSCORED for an unknown length of time while UI_CONTROLS.md went on citing the
# number it used to produce. Nothing was red. Nothing was measuring.
#
# check_uncalled_setters.sh is the same lesson from the other side: the
# frozen-params check went GREEN while 13 render controls were still welded,
# because the defect had moved one level up (a writer whose setters nobody
# calls) -- so the sweep runs both questions, and a third guard with tests and
# no slot here should be treated as suspect by default.
#
# The portability check is scoped to the directory being swept, so the runner's
# own fixtures are checked too -- a fixture that cannot run under the shebang it
# names is as broken as a real script, and finding it here is how it gets found
# at all. The frozen-params check is scoped to the module instead; see below.
# EXIT 2 IS NOT A PASS. This tested `rc -eq 1` for failure and let every other
# code through as clean -- so exit 2, which these checkers return when they
# cannot establish an answer at all, printed the checker's own "UNSCORED ... No
# verdict" text and still exited the sweep 0.
#
# The checkers were right; their caller collapsed a three-valued exit to two.
# That is the same defect from a new angle each time it appears: a pipeline did
# the collapsing in one case (`checker | tail; echo $?` reported tail's 0), a
# caller does it here. UNSCORED is the SAFE direction, which is precisely why it
# goes unnoticed -- nothing turns red, the answer just quietly stops existing
# while the number it produced stays on the page.
run_tree_check() {  # run_tree_check <label> <script> [args...]
    local label="$1" script="$2"; shift 2
    if [[ ! -x "${script}" ]]; then
        # Not silent. A missing checker is indistinguishable from a passing one
        # in any report that only prints failures.
        echo "${label}: MISSING -- ${script} is not executable."
        echo "  A guard that cannot run is not a guard. Refusing to report a clean sweep."
        return 1
    fi

    local out rc
    out="$("${script}" "$@" 2>&1)"
    rc=$?

    case "${rc}" in
        0)
            echo "${label}: ${out}"
            return 0
            ;;
        1)
            echo "${label}: FAILED"
            printf '%s\n' "${out}" | sed 's/^/  /'
            return 1
            ;;
        *)
            echo "${label}: UNSCORED (exit ${rc}) -- the check could not establish an answer."
            printf '%s\n' "${out}" | sed 's/^/  /'
            echo "  This is NOT a pass. A scan that stopped answering reports the same"
            echo "  silence as a clean tree, and whatever number it last produced stays"
            echo "  quoted in the docs as though it were still being measured."
            return 1
            ;;
    esac
}

tree_checks_failed=0
echo
# The floor is a scan-quality guard for the real tree; the runner's own fixture
# directories are legitimately tiny, so it is lowered rather than allowed to
# report UNSCORED on a directory we can see the size of.
MIN_PLAUSIBLE_SCRIPTS=1 run_tree_check "shell portability" \
    "${HERE}/check_shell_portability.sh" "${TESTS_DIR}" \
    || tree_checks_failed=1

# Runs against the MODULE, not TESTS_DIR: what it guards is whether a shipped
# build can reach a shader parameter, which no fixture directory can answer. It
# resolves its own default module path relative to cwd, so the run is anchored
# at the FlowViz root beside Tools/ rather than wherever the sweep was invoked.
echo
(
    cd "${HERE}/.." 2>/dev/null || exit 1
    run_tree_check "frozen shader parameters" "${HERE}/check_frozen_params.sh"
) || tree_checks_failed=1

# The question one hop up, and the reason both run: the frozen-params check
# went green on 2026-08-06 while 13 render controls were still welded, because
# the defect had moved from "no writer" to "a writer whose setters nobody
# calls" -- exactly past that checker's edge. Same module anchoring.
echo
(
    cd "${HERE}/.." 2>/dev/null || exit 1
    run_tree_check "uncalled view model setters" "${HERE}/check_uncalled_setters.sh"
) || tree_checks_failed=1

echo
# The check total, not just the file total. Thirteen files each verifying
# nothing is still "13/13 files", and that sentence is the one that misleads.
printf '%d passed, %d failed, %d did not run' "${passed}" "${failed}" "${notrun}"
[[ "${skipped}" -gt 0 ]] && printf ', %d skipped' "${skipped}"
printf '  --  %d checks total\n' "${total_checks}"

[[ "${failed}" -eq 0 && "${notrun}" -eq 0 && "${tree_checks_failed}" -eq 0 ]] || exit 1
exit 0
