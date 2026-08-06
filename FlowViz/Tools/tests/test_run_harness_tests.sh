#!/usr/bin/env bash
#
# Tests for Tools/run_harness_tests.sh -- the runner for this directory.
#
# Run: FlowViz/Tools/tests/test_run_harness_tests.sh
#
# WHY THE RUNNER NEEDS ITS OWN TESTS. It is the thing that decides whether the
# other thirteen files passed, so every way it can be wrong is a way the whole
# harness reports green while proving nothing. Three of those ways have already
# happened in this repo, and all three are the same shape -- something did not
# run, and nothing said so:
#
#   - an empty match set passes every check ("0 exports, all defined")
#   - a green total hides a test that skipped itself
#   - a non-zero exit can mean "never ran", not "ran and failed"
#
# So the checks below are mostly about the runner's behaviour on tests that do
# NOT produce an honest result: a file that exits 0 having verified nothing, a
# missing interpreter, an empty directory. A runner that only handles pass and
# fail is a runner that reports pass for everything else.
#
# Every check has a control where required: the do-nothing test is paired with
# a real one in the same run, so "reported not-run" cannot be confused with
# "the runner reports not-run for everything".

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="${HERE}/../run_harness_tests.sh"

failures=0
checks=0

check() {
    local name="$1" expected="$2" actual="$3"
    checks=$((checks + 1))
    if [[ "${expected}" == "${actual}" ]]; then
        echo "  ok    ${name}"
    else
        failures=$((failures + 1))
        echo "  FAIL  ${name}"
        echo "        expected: ${expected}"
        echo "        actual:   ${actual}"
    fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# Fake tests in the several summary dialects this directory actually uses. The
# runner must read all of them; a runner that understood only its author's
# favourite spelling would report the rest unparseable, and the fix for that
# noise is invariably to make unparseable mean pass.
make_test() {
    local dir="$1" name="$2" body="$3"
    mkdir -p "${dir}"
    printf '#!/usr/bin/env bash\n%s\n' "${body}" > "${dir}/${name}"
    chmod +x "${dir}/${name}"
}

echo "run_harness_tests.sh:"

# --- the happy path, first, so every "did not run" below has a control -------

D="${WORK}/green"
make_test "${D}" test_a.sh 'echo "  ok    thing"; echo "a tests: 3 passed, 0 failed"; exit 0'
make_test "${D}" test_b.sh 'echo "b: 5 checks, all passed"; exit 0'
make_test "${D}" test_c.sh 'echo "c summary: 4/4 checks passed"; exit 0'

out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
green_exit=$?
check "CONTROL: three passing tests exit 0" "0" "${green_exit}"
check "CONTROL: ...and all three are named in the report" "3" \
    "$(grep -cE '^(ok|PASS) ' <<<"${out}")"

# The runner must report how many CHECKS ran, not just how many FILES did. A
# file count says nothing about whether the files verified anything: thirteen
# files each verifying nothing is still "13/13".
check "the total check count is summed across files, not just files counted" "yes" \
    "$(grep -qE '12 checks' <<<"${out}" && echo yes || echo no)"

# --- a failing test is reported and fails the run ----------------------------

D="${WORK}/red"
make_test "${D}" test_ok.sh 'echo "ok: 2 passed, 0 failed"; exit 0'
make_test "${D}" test_bad.sh 'echo "bad: 1 passed, 2 failed"; exit 1'

out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
red_exit=$?
check "one failing test fails the whole run" "1" "${red_exit}"
check "...and the failing file is named" "yes" \
    "$(grep -q 'test_bad.sh' <<<"${out}" && echo yes || echo no)"

# A runner that stops at the first failure hides every later one, so a single
# red file makes the rest of the harness invisible -- and the next person fixes
# one thing, re-runs, and finds another. Both must appear in one run.
D="${WORK}/red2"
make_test "${D}" test_bad1.sh 'echo "1 passed, 1 failed"; exit 1'
make_test "${D}" test_bad2.sh 'echo "1 passed, 1 failed"; exit 1'
make_test "${D}" test_good.sh 'echo "1 passed, 0 failed"; exit 0'
out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
check "every test runs even after one fails" "3" \
    "$(grep -cE '^(ok|PASS|FAIL|NOTRUN) ' <<<"${out}")"

# --- a test that exits 0 having verified NOTHING -----------------------------
#
# This is the case the whole file exists for. Such a script is
# indistinguishable from a passing one by exit code alone, and it is exactly
# what a test looks like after someone comments out its body, or when its
# fixture is missing and it returns early. It must NOT count as a pass.

D="${WORK}/silent"
make_test "${D}" test_real.sh 'echo "real: 2 passed, 0 failed"; exit 0'
make_test "${D}" test_silent.sh 'exit 0'

out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
silent_exit=$?
check "a test that exits 0 with no summary is NOT counted as passed" "yes" \
    "$(grep -qE '^NOTRUN .*test_silent' <<<"${out}" && echo yes || echo no)"
check "...and it fails the run rather than being ignored" "1" "${silent_exit}"
check "CONTROL: the real test in that same run still reports ok" "yes" \
    "$(grep -qE '^(ok|PASS) .*test_real' <<<"${out}" && echo yes || echo no)"

# A summary claiming zero checks is the same defect wearing a summary line.
D="${WORK}/zero"
make_test "${D}" test_zero.sh 'echo "zero tests: 0 passed, 0 failed"; exit 0'
out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
zero_exit=$?
check "a test reporting 0 checks is not a pass" "1" "${zero_exit}"

# --- could not run, as distinct from ran and failed --------------------------

D="${WORK}/norun"
make_test "${D}" test_fine.sh 'echo "fine: 1 passed, 0 failed"; exit 0'
mkdir -p "${D}"
printf '#!/nonexistent/interpreter\n' > "${D}/test_nointerp.sh"
chmod +x "${D}/test_nointerp.sh"

out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
norun_exit=$?
check "a test whose interpreter is missing is NOTRUN, not FAIL" "yes" \
    "$(grep -qE '^NOTRUN .*test_nointerp' <<<"${out}" && echo yes || echo no)"
check "...and still fails the run" "1" "${norun_exit}"

# A test file with no execute bit is a real and silent way to lose coverage:
# `chmod -x` and the file stops running with nothing to show for it. Run it
# through its shebang interpreter rather than skipping it.
D="${WORK}/noexec"
mkdir -p "${D}"
printf '#!/usr/bin/env bash\necho "noexec: 2 passed, 0 failed"\nexit 0\n' > "${D}/test_noexec.sh"
chmod -x "${D}/test_noexec.sh"
out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
noexec_exit=$?
check "a test file without the execute bit is still run" "0" "${noexec_exit}"
check "...and reported as a pass" "yes" \
    "$(grep -qE '^(ok|PASS) .*test_noexec' <<<"${out}" && echo yes || echo no)"

# --- an empty directory must FAIL, not report a clean sweep ------------------
#
# "0 tests, 0 failures" reads as green in every summary format there is. This
# repo has already shipped that reading once: "0 exports, all defined" scanned
# as clean while the linker was finding four errors in those same files.

D="${WORK}/empty"
mkdir -p "${D}"
out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
empty_exit=$?
check "an empty tests directory FAILS rather than reporting a clean sweep" "1" \
    "${empty_exit}"
check "...and says so in words" "yes" \
    "$(grep -qiE 'no tests' <<<"${out}" && echo yes || echo no)"

# A directory that exists but holds only non-test files is the same situation.
D="${WORK}/nomatch"
mkdir -p "${D}"
printf 'not a test\n' > "${D}/helper.sh"
out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
check "a directory with no test_* files also fails" "1" "$?"

# --- discovery covers python as well as bash ---------------------------------
#
# test_equivalent_mutants.py lives in this directory and is a real test. A
# runner that globbed only *.sh would drop it without a word -- and the report
# would look complete, because nothing names what was not discovered.

D="${WORK}/py"
make_test "${D}" test_sh.sh 'echo "sh: 1 passed, 0 failed"; exit 0'
mkdir -p "${D}"
cat > "${D}/test_py.py" <<'PY'
#!/usr/bin/env python3
print("py: 7 passed, 0 failed")
PY
out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
check "python tests are discovered too" "yes" \
    "$(grep -qE '^(ok|PASS) .*test_py.py' <<<"${out}" && echo yes || echo no)"

cat > "${D}/test_pyfail.py" <<'PY'
#!/usr/bin/env python3
import sys
print("pyfail: 1 passed, 3 failed")
sys.exit(1)
PY
out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" "${RUNNER}" 2>&1)"
check "a failing python test fails the run" "1" "$?"

# --- the runner names what it skipped ----------------------------------------
#
# Some tests need an engine or a long build. If any are excluded, the exclusion
# must be printed: silent truncation reads as "covered everything".

D="${WORK}/slow"
make_test "${D}" test_quick.sh 'echo "q: 1 passed, 0 failed"; exit 0'
make_test "${D}" test_slow_thing.sh 'echo "s: 1 passed, 0 failed"; exit 0'
out="$(FLOWVIZ_HARNESS_TESTS_DIR="${D}" FLOWVIZ_HARNESS_SKIP='test_slow_thing.sh' \
    "${RUNNER}" 2>&1)"
skip_exit=$?
check "an excluded test is reported by name, not silently dropped" "yes" \
    "$(grep -qE '^SKIP .*test_slow_thing' <<<"${out}" && echo yes || echo no)"
check "...and a run with exclusions still exits 0 when the rest pass" "0" \
    "${skip_exit}"
check "...and the summary states how many were skipped" "yes" \
    "$(grep -qiE '1 skipped' <<<"${out}" && echo yes || echo no)"

echo
echo "run_harness_tests tests: $((checks - failures)) passed, ${failures} failed"
exit $(( failures > 0 ? 1 : 0 ))
