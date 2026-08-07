// Copyright FlowViz contributors. All Rights Reserved.

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UI/FlowVizWorkspaceModel.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizWorkspaceSamplingTest
{
	FString GetSampleCaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"));
	}

	/**
	 * The same interior cell FlowViz.UI.ProbeViewModel.Sample reads, whose value
	 * is pinned against known_values.json -- so "the probe shows a number" below
	 * is anchored to a number that is independently known to be real.
	 */
	FVector KnownCellCentre()
	{
		// Domain 12 x 4 x 1 m over 56 x 28 x 6 cells; cell centre = (i + 0.5) * spacing.
		const FVector Spacing(12.0 / 56.0, 4.0 / 28.0, 1.0 / 6.0);
		return FVector(28.5 * Spacing.X, 14.5 * Spacing.Y, 3.5 * Spacing.Z);
	}
}

/**
 * THE SAMPLING SERVICE: probes get readings, and the transfer function gets a
 * per-frame range (#75's last two setters).
 *
 * SetProbeReading was "the seam an async readback would call", and nothing
 * did: probes could be placed, named, hidden and persisted, and could never
 * display a value. SetCurrentFrameRange was why the "Per frame" range source
 * button was PERMANENTLY disabled -- its enablement predicate is
 * HasCurrentFrameRange(), and nothing ever measured one.
 *
 * The service samples on a WORKER (disk I/O and zlib -- engineering rule 1
 * forbids either on the game thread) and delivers on the game thread through
 * the two setters. The range is measured over the frame's decoded voxels,
 * under the SAME component selection the transfer function displays and
 * skipping masked and non-finite values -- so the measured range is the range
 * of exactly what the colour scale colours, not a bound derived from
 * per-component statistics that can overshoot a magnitude.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceSamplingTest,
	"FlowViz.UI.WorkspaceModel.Sampling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceSamplingTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceSamplingTest;

	const FString CaseDir = GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		AddError(FString::Printf(TEXT("sample case not found at '%s'"), *CaseDir));
		return false;
	}

	FFlowVizWorkspaceModel Model;
	const FCFDVizResult OpenResult = Model.OpenCase(CaseDir);
	if (!OpenResult.IsOk())
	{
		AddError(FString::Printf(TEXT("could not open the sample case: %s"), *OpenResult.ToString()));
		return false;
	}

	/* == THE DIFFERENTIAL, FIRST: per-frame is refused before a measurement == */
	{
		TestFalse(TEXT("CONTROL: no frame range has been measured yet"),
			Model.TransferFunction.HasCurrentFrameRange());
		TestFalse(
			TEXT("CONTROL: selecting the per-frame range source is REFUSED before a "
				 "measurement exists -- this is the permanently-disabled button, asserted"),
			Model.TransferFunction.SetRangeSource(EFlowVizRangeSource::CurrentFrame).IsOk());
	}

	/* == A probe, placed before the sample runs ============================== */
	const FGuid ProbeId =
		Model.Probes.AddProbeAtSolverPosition(KnownCellCentre(), TEXT("wake centre"));
	if (!TestTrue(TEXT("CONTROL: the probe was placed"), ProbeId.IsValid()))
	{
		return false;
	}
	{
		const FFlowVizProbe* Probe = Model.Probes.FindProbe(ProbeId);
		TestTrue(TEXT("CONTROL: the fresh probe has NO reading -- rule 10: an unsampled "
					  "probe must not read as zero"),
			Probe != nullptr && !Probe->LastReading.bHasValue);
	}

	/* == Request, wait, drain ================================================ */

	Model.RequestSampleUpdate();

	if (!TestTrue(TEXT("the sample completes within the timeout"),
			Model.WaitForPendingSamples(60.0)))
	{
		return false;
	}

	TestTrue(TEXT("draining applies results"), Model.DrainSampleResults());

	/* == The probe now holds a reading ======================================= */
	{
		const FFlowVizProbe* Probe = Model.Probes.FindProbe(ProbeId);
		if (!TestNotNull(TEXT("the probe still exists"), Probe))
		{
			return false;
		}
		const FFlowVizProbeReading& Reading = Probe->LastReading;
		TestTrue(TEXT("the probe has a value"), Reading.bHasValue);
		TestTrue(TEXT("from inside the domain"), Reading.bInsideDomain);
		TestEqual(TEXT("at frame 0, the frame on display"), Reading.FrameIndex, 0);
		TestEqual(TEXT("naming the voxel it came from"),
			Reading.Voxel, FIntVector(28, 14, 3));
		// U's components at this cell are pinned by known_values.json in the
		// probe sample test; here it is enough that the magnitude is finite and
		// nonzero -- the wake centre is not a stagnant cell.
		TestTrue(TEXT("with a finite, nonzero magnitude"),
			FMath::IsFinite(Reading.Magnitude) && Reading.Magnitude > 0.0);
	}

	/* == The per-frame range is measured and the source becomes selectable === */
	{
		if (!TestTrue(TEXT("a frame range was measured"),
				Model.TransferFunction.HasCurrentFrameRange()))
		{
			return false;
		}

		TestTrue(TEXT("the per-frame range source is now ACCEPTED -- the differential "
					  "against the refusal asserted above"),
			Model.TransferFunction.SetRangeSource(EFlowVizRangeSource::CurrentFrame).IsOk());

		const float FrameMin = Model.TransferFunction.GetRangeMin();
		const float FrameMax = Model.TransferFunction.GetRangeMax();
		TestTrue(TEXT("the measured range is a range"), FrameMax > FrameMin);

		/*
		 * ONE FRAME'S RANGE IS WITHIN THE GLOBAL RANGE. The manifest declares
		 * U's global magnitude range over ALL frames; frame 0's cannot exceed
		 * it. An instrument reading outside its own global bound is measuring
		 * something else (a-control-needs-a-known-nonzero-expectation).
		 */
		TestTrue(TEXT("the frame range sits within the field's declared global range"),
			FrameMin >= 0.0f && FrameMax <= 62.9375f + 1.0e-3f);
	}

	/* == The line series rides the same drain (#85) ========================== */
	{
		TestFalse(TEXT("CONTROL: no line series before a line probe exists"),
			Model.GetLineSeries().bHasRange);

		TestTrue(TEXT("CONTROL: a line probe is accepted"),
			Model.Probes.SetLineProbe(
				FVector(1.0, 3.0, 0.5), FVector(11.0, 3.0, 0.5)).IsOk());

		Model.RequestSampleUpdate();
		if (!TestTrue(TEXT("the line sample completes"), Model.WaitForPendingSamples(60.0)))
		{
			return false;
		}
		TestTrue(TEXT("draining applies the series"), Model.DrainSampleResults());

		const FFlowVizChartSeries& Series = Model.GetLineSeries();
		TestTrue(TEXT("the series has a range"), Series.bHasRange);
		TestEqual(TEXT("with the line's own sample count"),
			Series.Points.Num(), Model.Probes.GetLineSampleCount());
		TestTrue(TEXT("inside the field's declared global bound -- the numbers under the "
					  "plot are the displayed field's"),
			Series.MinY >= 0.9 && Series.MaxY <= 13.5);
	}

	/* == A workspace with no case refuses politely =========================== */
	{
		FFlowVizWorkspaceModel Empty;
		Empty.RequestSampleUpdate();
		TestTrue(TEXT("a request with no case open is a no-op, not a crash"),
			Empty.WaitForPendingSamples(1.0));
		TestFalse(TEXT("and drains nothing"), Empty.DrainSampleResults());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
