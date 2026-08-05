#!/usr/bin/env bash
# Differential verification for FlowViz.Render.VolumeMarch.
#
# A test that has only ever passed is not evidence. This applies one mutation at
# a time to the SHADER (not the test), runs the march test, and requires each
# named assertion to go red. A mutation that leaves the suite green means the
# assertion it targets is vacuous and the test is decoration.
#
# Mutating the .usf rather than the .cpp is deliberate: shaders compile at
# runtime, so each arm costs a test run rather than a C++ rebuild. It also
# targets the code under test rather than the harness.
#
# ATTRIBUTION IS THE POINT. A shader that fails to COMPILE is UNSCORED, never
# killed - a compile error and a caught bug both show up as "not green", and
# scoring them alike inflates the campaign. Each arm therefore checks for shader
# compile errors before reading the verdict.
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1

USF="Plugins/FlowVizRuntime/Shaders/FlowVizVolumeRayMarch.usf"
BACKUP="$(mktemp -t flowviz_usf_backup)"
cp "${USF}" "${BACKUP}"

# Restore on ANY exit, including a signal. A mutant left in a shared tree makes
# every later run in this repo report someone else's sabotage as their own bug.
restore() {
    cp "${BACKUP}" "${USF}"
    rm -f "${BACKUP}"
}
trap restore EXIT INT TERM

failures=0
arms=0

# run_arm <name> <expect: KILL|SURVIVE> <sed-expression> <assertion-substring>
run_arm() {
    local name="$1" expect="$2" expr="$3" needle="$4"
    arms=$((arms + 1))

    cp "${BACKUP}" "${USF}"
    if ! perl -0pi -e "${expr}" "${USF}"; then
        echo "  UNSCORED  ${name}: the mutation could not be applied"
        failures=$((failures + 1))
        return
    fi
    if cmp -s "${BACKUP}" "${USF}"; then
        echo "  UNSCORED  ${name}: the mutation matched nothing - the file is unchanged, so this"
        echo "            arm tested the ORIGINAL shader and its verdict is meaningless"
        failures=$((failures + 1))
        return
    fi

    local log="/tmp/flowviz_march_arm.log"
    RHI=1 ./Tools/build_lock.sh ./Tools/run_tests.sh FlowViz.Render.VolumeMarch > "${log}" 2>&1
    local status=$?

    # ATTRIBUTION, BEFORE ANY VERDICT.
    #
    # A shader compile error is checked FIRST because it is the more precise
    # cause: it also produces a bad exit code, and reporting "the filter matched
    # nothing" when the real story is "the mutated HLSL is malformed" sends the
    # next reader to the wrong file.
    if grep -aqE "Failed to compile|Shader compile error" /tmp/flowviz_tests.log 2>/dev/null; then
        echo "  UNSCORED  ${name}: the SHADER FAILED TO COMPILE, so the test never exercised the"
        echo "            mutated logic. A malformed mutant is INVALID, not killed."
        failures=$((failures + 1))
        return
    fi

    # These exit codes are NOT failures of the code under test, and scoring them
    # as kills is what inflated this harness's first run to a meaningless 5/6:
    #   3  the editor never started
    #   4  the filter matched no tests - the mutant never ran
    #   75 the build lock timed out
    # Each looks exactly like "the test went red" if you only read $? != 0.
    case ${status} in
        3)
            echo "  UNSCORED  ${name}: the editor failed to start. The mutant never ran."
            failures=$((failures + 1)); return ;;
        4)
            echo "  UNSCORED  ${name}: the filter matched NO TESTS, so nothing was executed."
            echo "            A non-zero exit here means 'never ran', not 'caught the bug'."
            failures=$((failures + 1)); return ;;
        75)
            echo "  UNSCORED  ${name}: the build lock timed out; the arm never ran."
            failures=$((failures + 1)); return ;;
    esac

    if grep -aq "SKIPPED" /tmp/flowviz_tests.log 2>/dev/null; then
        echo "  UNSCORED  ${name}: the test skipped itself (no GPU). Nothing was verified."
        failures=$((failures + 1))
        return
    fi
    # The test must have actually completed, in either direction.
    if ! grep -aq "Test Completed\. Result=" /tmp/flowviz_tests.log 2>/dev/null; then
        echo "  UNSCORED  ${name}: no test completion in the log at all."
        failures=$((failures + 1))
        return
    fi

    local verdict="SURVIVED"
    [[ ${status} -ne 0 ]] && verdict="KILLED"

    if [[ "${verdict}" == "KILLED" && "${expect}" == "KILL" ]]; then
        # Killed is not enough: it must be killed BY THE ASSERTION THAT NAMES
        # THIS BUG. A mutation that trips an unrelated check still leaves the
        # intended assertion unproven.
        if grep -aq "${needle}" /tmp/flowviz_tests.log; then
            echo "  ok        ${name}: KILLED by the assertion that names it"
        else
            echo "  FAIL      ${name}: KILLED, but NOT by the expected assertion"
            echo "            expected a failure mentioning: ${needle}"
            failures=$((failures + 1))
        fi
    elif [[ "${verdict}" == "SURVIVED" && "${expect}" == "SURVIVE" ]]; then
        echo "  ok        ${name}: SURVIVED as expected (control)"
    elif [[ "${expect}" == "KILL" ]]; then
        echo "  FAIL      ${name}: SURVIVED. The assertion it targets is VACUOUS -"
        echo "            the shader is wrong in this way and the suite is green."
        failures=$((failures + 1))
    else
        echo "  FAIL      ${name}: KILLED, but this arm must SURVIVE. The test is"
        echo "            sensitive to something it should not constrain."
        failures=$((failures + 1))
    fi
}

echo "Differential arms for FlowViz.Render.VolumeMarch"
echo

# 1. THE HEADLINE BUG. Step scaled by the largest voxel spacing instead of the
#    smallest: undersamples the thin axis, aliases, and still renders a
#    plausible image that no colour assertion can catch.
run_arm "step-by-max-spacing" KILL \
    's/const float StepLength = max\(StepVoxels, 1e-6f\) \* MinVoxelSpacing;/const float StepLength = max(StepVoxels, 1e-6f) * MaxVoxelSpacing;/' \
    "steps, matching the SMALLEST voxel spacing"

# 2. The crop box ignored. Renders the whole domain, which looks correct.
run_arm "crop-box-ignored" KILL \
    's/const float3 BoxMin = saturate\(min\(CropBoxMin, CropBoxMax\)\) \* PhysicalSize;/const float3 BoxMin = float3(0.0f, 0.0f, 0.0f);/' \
    "raises the minimum by exactly 8 voxels"

# 3. The masked CAUSE is lost. Dropping the REASON_MASKED bit does NOT change
#    the composited value: the status falls through to REASON_UNKNOWN, which
#    also refuses the voxel, so the minimum is identical. What changes is that
#    the renderer can no longer say WHY there is a hole - masked geometry would
#    draw as empty space instead of as the grey that names it (VISUAL_QA rule 4).
#    The expected needle is therefore the reason-bit assertion, not the value
#    one; predicting the value assertion here was wrong about the shader, and
#    the arm is what corrected it.
run_arm "masked-cause-lost" KILL \
    's/if \(\(Status & FLOWVIZ_STATUS_MASKED\) != 0u\)   \{ Reason \|= FLOWVIZ_REASON_MASKED; \}/if (false) { Reason |= FLOWVIZ_REASON_MASKED; }/' \
    "REPORTS having crossed masked voxels"

# 3b. The status gate genuinely opened: masked voxels sampled as if valid. THIS
#     is the arm that must move the number, and it is a different bug from 3.
run_arm "masked-sampled-as-valid" KILL \
    's/if \(\(Status & RequiredStatusMask\) == RequiredStatusMask && RequiredStatusMask != 0u\)/if (true)/' \
    "raises the minimum by exactly"

# 4. VISUAL_QA rule 1 broken: the lighting term reaches the reported scalar.
run_arm "lighting-leaks-into-value" KILL \
    's/\t\t\t\t\t\tReportedValue = Sample\.Value;/\t\t\t\t\t\tReportedValue = Rgb.r > 0.0f ? Sample.Value * (0.5f + Rgb.r) : Sample.Value;/' \
    "BIT-IDENTICAL with lighting on and off"

# 5. Average returns the SUM rather than the mean.
run_arm "average-is-sum" KILL \
    's/else                                            \{ ReportedValue = SumValue \/ float\(ValidCount\); \}/else                                            { ReportedValue = SumValue; }/' \
    "Average is the MEAN of the samples"

# 6. The iso-surface snapped to the sample lattice. The crossing is what keeps
#    the surface off the voxel grid; without it the surface stair-steps, which
#    is the most visible artefact this renderer can produce - and it still
#    renders a complete, smoothly-lit, entirely plausible surface.
#    The mutation ALSO reports the sample rather than the iso value, because
#    THit alone only moves the shading position: the reported scalar is set to
#    IsoValue unconditionally, so a THit-only mutant would leave the value
#    assertion green and be caught by lighting instead. The needle is the
#    non-integer assertion, which is the one that names lattice snapping.
run_arm "iso-snaps-to-lattice" KILL \
    's/const float THit = PrevT \+ Alpha \* \(T - PrevT\);/const float THit = T;/;s/\t\t\t\t\t\tReportedValue = IsoValue;/\t\t\t\t\t\tReportedValue = Sample.Value;/' \
    "the reported value is NOT an integer"

# 6b. The iso crossing test fires on samples that do NOT bracket the surface.
#     An iso value above the entire field must find nothing; a renderer that
#     accepts any pair paints a surface through empty data.
run_arm "iso-hits-without-bracket" KILL \
    's/if \(bHavePrev && \(Signed == 0\.0f \|\| \(PrevSigned \* Signed\) < 0\.0f\)\)/if (bHavePrev)/' \
    "finds NO surface"

# 7. THE GRADIENT AXIS SWAP. X and Z exchanged in the central difference. This
#    lights the volume from the wrong side and looks completely reasonable -
#    no assertion about value, alpha, steps or status can see it, and neither
#    can an eye that has not been shown the correct image beside it. Here the
#    X:Y ratio moves from 0.25 to 16.
run_arm "gradient-axis-swapped" KILL \
    's/\t\t\(XP\.Value - XN\.Value\) \* 0\.5f \* InvVoxelSpacing\.x,\n\t\t\(YP\.Value - YN\.Value\) \* 0\.5f \* InvVoxelSpacing\.y,\n\t\t\(ZP\.Value - ZN\.Value\) \* 0\.5f \* InvVoxelSpacing\.z\);/\t\t(ZP.Value - ZN.Value) * 0.5f * InvVoxelSpacing.z,\n\t\t(YP.Value - YN.Value) * 0.5f * InvVoxelSpacing.y,\n\t\t(XP.Value - XN.Value) * 0.5f * InvVoxelSpacing.x);/' \
    "matching the analytic 8:32"

# 7b. One spacing for all three axes. The gradient is then wrong by up to 4x per
#     axis on this fixture and every shaded normal tilts - the anisotropy rule
#     again, but in the lighting rather than the step.
run_arm "gradient-one-spacing" KILL \
    's/\t\t\(YP\.Value - YN\.Value\) \* 0\.5f \* InvVoxelSpacing\.y,/\t\t(YP.Value - YN.Value) * 0.5f * InvVoxelSpacing.x,/' \
    "matching the analytic 8:32"

# 8. Clip planes ignored entirely. Renders the whole domain, which is a picture
#    that looks right unless you know a plane was requested.
run_arm "clip-planes-ignored" KILL \
    's/\t\tbHit = FlowVizApplyClipPlanes\(Origin, Dir, TMin, TMax\);/\t\tbHit = true;/' \
    "raises the minimum by exactly 8 voxels"

# 8b. The second plane REPLACES the first instead of intersecting with it. With
#     one plane the image is identical, so only a two-plane test can see this.
# This arm SURVIVED its first run, and the survival was correct: the assertion
# it targeted ("STILL in effect under the second") could not distinguish max()
# from assignment, because on that config TMin is written once with a value that
# already exceeds the box entry. The needle now points at the tight-then-loose
# pair, where the two orders give 8 voxels and 4 voxels respectively.
run_arm "clip-planes-replace" KILL \
    's/\t\t\tTMin = max\(TMin, THit\);   \/\/ entering the kept half-space/\t\t\tTMin = THit;/' \
    "OVERWROTE TMin"

# The same bug on the OTHER accumulator, which no arm reached before.
run_arm "clip-planes-replace-max" KILL \
    's/\t\t\tTMax = min\(TMax, THit\);   \/\/ leaving it/\t\t\tTMax = THit;/' \
    "not 12"

# 9. THE IDENTITY CONTROL, and the most important arm here.
#
#    A change that alters the source text but provably not the rendered result:
#    a comment, plus a statement that assigns a variable its own value. If this
#    "kills", the harness is reporting noise - a stale binary, a flaky device,
#    an assertion keyed on something incidental - and EVERY KILLED VERDICT ABOVE
#    IT IS SUSPECT. This arm is what makes the other five mean anything.
#
#    It is written as a real edit inside the marching loop rather than a
#    top-of-file comment, so it also proves the shader is genuinely recompiled
#    per arm: if the mutant text were being ignored, a no-op edit and a
#    behavioural one would be indistinguishable and this file would be theatre.
run_arm "identity-control" SURVIVE \
    's/\t\tfloat MaxValue = -3\.402823466e\+38f;/\t\t\/* FLOWVIZ IDENTITY CONTROL: no behaviour change *\/\n\t\tfloat MaxValue = -3.402823466e+38f;\n\t\tMaxValue = MaxValue;/' \
    ""

echo
if [[ ${failures} -eq 0 ]]; then
    echo "${arms}/${arms} arms behaved as required."
else
    echo "${failures} of ${arms} arms did not. See above."
fi
exit $(( failures > 0 ? 1 : 0 ))
