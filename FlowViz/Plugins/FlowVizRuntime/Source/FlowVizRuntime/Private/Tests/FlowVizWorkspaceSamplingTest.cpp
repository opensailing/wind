// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizCrc32C.h"
#include "CFDViz/CFDVizVolumeReader.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
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

	/** Per-test scratch directory under Saved/, removed by the test that makes it. */
	FString GetScratchDir(const TCHAR* TestName)
	{
		return FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("FlowVizWorkspaceSamplingTest"), TestName);
	}

	/* --- A tiny hand-built case whose one field is UNIFORM at frame 0 -------- */

	void WriteU16(TArray<uint8>& Bytes, int32 Offset, uint16 Value)
	{
		Bytes[Offset + 0] = static_cast<uint8>(Value & 0xFF);
		Bytes[Offset + 1] = static_cast<uint8>((Value >> 8) & 0xFF);
	}

	void WriteU32(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (8 * Index)) & 0xFF);
		}
	}

	void WriteU64(TArray<uint8>& Bytes, int32 Offset, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (8 * Index)) & 0xFF);
		}
	}

	void WriteF32(TArray<uint8>& Bytes, int32 Offset, float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		WriteU32(Bytes, Offset, Bits);
	}

	/**
	 * Write a valid single-brick CVF holding a 4x3x2 cell-associated uint8
	 * scalar where EVERY voxel is the same value -- the "uniform frame 0" of
	 * the drain-return finding. codec none, CRCs computed with the runtime's
	 * own Crc32C so the reader accepts the file through its full validation.
	 */
	bool WriteUniformCvf(const FString& Path, uint8 UniformValue)
	{
		constexpr int32 HeaderBytes = 128;
		constexpr int32 EntryBytes = 80;
		constexpr int32 PayloadBytes = 4 * 3 * 2; // one uint8 component
		const int32 PayloadOffset = HeaderBytes + EntryBytes;

		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(PayloadOffset + PayloadBytes);

		// ---- header (format section 4.1) ----
		const ANSICHAR Magic[8] = { 'C', 'F', 'D', 'V', 'O', 'L', '1', '\0' };
		FMemory::Memcpy(Bytes.GetData(), Magic, 8);
		WriteU32(Bytes, 8, HeaderBytes);
		WriteU16(Bytes, 12, 1);							  // majorVersion
		WriteU16(Bytes, 14, 0);							  // minorVersion
		WriteU32(Bytes, 16, 0x01020304u);				  // endianMarker
		WriteU32(Bytes, 20, 0);							  // flags
		WriteU32(Bytes, 24, 0);							  // frameIndex
		WriteU32(Bytes, 28, 1);							  // fieldNumericId
		WriteU64(Bytes, 32, 0);							  // simulationTime 0.0
		WriteU32(Bytes, 40, 4);							  // dimX (cells)
		WriteU32(Bytes, 44, 3);							  // dimY
		WriteU32(Bytes, 48, 2);							  // dimZ
		WriteU16(Bytes, 52, 4);							  // brickX
		WriteU16(Bytes, 54, 4);							  // brickY
		WriteU16(Bytes, 56, 4);							  // brickZ
		Bytes[58] = 1;									  // componentCount
		Bytes[59] = 3;									  // dataType uint8
		Bytes[60] = 0;									  // association cell
		Bytes[61] = 0;									  // codec none
		WriteU64(Bytes, 64, 1);							  // brickCount
		WriteU64(Bytes, 72, HeaderBytes);				  // directoryOffset
		WriteU64(Bytes, 80, static_cast<uint64>(PayloadOffset));
		// backgroundValue float32[4] stays zero.
		WriteU32(Bytes, 104,
			FCFDVizVolumeHeader::ComputeHeaderCrc(
				TArrayView<const uint8>(Bytes.GetData(), HeaderBytes)));

		// ---- one directory entry (format section 4.3) ----
		const int32 E = HeaderBytes;
		// brickIndex (0,0,0) already zero.
		WriteU16(Bytes, E + 12, 4);						  // validSize
		WriteU16(Bytes, E + 14, 3);
		WriteU16(Bytes, E + 16, 2);
		WriteU16(Bytes, E + 18, 0);						  // entry flags
		WriteU64(Bytes, E + 20, static_cast<uint64>(PayloadOffset));
		WriteU32(Bytes, E + 28, PayloadBytes);			  // compressed
		WriteU32(Bytes, E + 32, PayloadBytes);			  // uncompressed
		// componentMin/Max: the value for component 0, the writer's inf/-inf
		// convention for the unused three.
		WriteF32(Bytes, E + 36, static_cast<float>(UniformValue));
		WriteF32(Bytes, E + 52, static_cast<float>(UniformValue));
		for (int32 Component = 1; Component < 4; ++Component)
		{
			WriteF32(Bytes, E + 36 + Component * 4, TNumericLimits<float>::Max() * 2.0f);
			WriteF32(Bytes, E + 52 + Component * 4, -TNumericLimits<float>::Max() * 2.0f);
		}

		// ---- payload: every voxel the same ----
		for (int32 Index = 0; Index < PayloadBytes; ++Index)
		{
			Bytes[PayloadOffset + Index] = UniformValue;
		}
		WriteU32(Bytes, E + 68,
			CFDViz::Crc32C::Compute(Bytes.GetData() + PayloadOffset, PayloadBytes));

		return FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	/**
	 * A complete on-disk case with ONE scalar field, uniform at its ONE frame,
	 * and no meshes, no U, no qCriterion. Opening it and sampling produces a
	 * result that carries ONLY a cut plane: no probes were placed, the uniform
	 * frame yields no range (max > min fails), no line probe exists, and the
	 * iso/streamline builders find no fields to ride.
	 */
	bool MakeUniformCase(const FString& CaseDir)
	{
		IFileManager& Files = IFileManager::Get();
		Files.DeleteDirectory(*CaseDir, /*RequireExists*/ false, /*Tree*/ true);

		const FString Manifest = TEXT(R"JSON(
{
  "format": "CFDViz",
  "version": "1.0.0",
  "case": {
    "id": "0f2b6d1c-8f5e-4c6a-9a41-0d7f3b2a5c11",
    "name": "Uniform drain fixture",
    "quality": "visualization-demo"
  },
  "units": { "length": "m", "time": "s" },
  "coordinates": { "handedness": "right", "upAxis": "Z", "forwardAxis": "X" },
  "timeline": { "frameCount": 1, "times": [0.0] },
  "grids": [
    {
      "id": "main",
      "type": "uniform-cartesian",
      "dimensions": [4, 3, 2],
      "origin": [0.0, 0.0, 0.0],
      "spacing": [0.1, 0.1, 0.1]
    }
  ],
  "fields": [
    {
      "numericId": 1,
      "id": "phi",
      "components": ["v"],
      "componentCount": 1,
      "dataType": "uint8",
      "association": "cell",
      "grid": "main",
      "unit": "1",
      "storage": {
        "type": "bricked-volume",
        "codec": "none",
        "brickSize": [4, 4, 4],
        "pathPattern": "frames/{frame:06d}/phi.cvf"
      }
    }
  ]
}
)JSON");

		if (!FFileHelper::SaveStringToFile(
				Manifest, *FPaths::Combine(CaseDir, TEXT("manifest.json"))))
		{
			return false;
		}
		return WriteUniformCvf(
			FPaths::Combine(CaseDir, TEXT("frames"), TEXT("000000"), TEXT("phi.cvf")),
			/*UniformValue*/ 7);
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

	/* == The cut plane rides the same drain (renderer overhaul P3) ========== */
	{
		// Visible slice + domain -> the worker builds the plane mesh and the
		// drain stores it with a fresh flag the workspace consumes once.
		Model.Slice.SetDomainSize(FVector(12.0, 4.0, 1.0));
		Model.Slice.SetAxisPreset(EFlowVizSliceAxis::Z);
		Model.Slice.CenterOnDomain();
		Model.Slice.SetVisible(true);

		Model.RequestSampleUpdate();
		Model.WaitForPendingSamples();
		if (!TestTrue(TEXT("the drain applied a result"), Model.DrainSampleResults()))
		{
			return false;
		}

		if (!TestTrue(TEXT("a fresh cut plane arrived with the drain"),
				Model.HasFreshCutPlane()))
		{
			return false;
		}
		const FFlowVizMeshPayload& Plane = Model.ConsumeCutPlane();
		TestFalse(TEXT("consuming clears the fresh flag -- one apply per build"),
			Model.HasFreshCutPlane());
		if (TestEqual(TEXT("one section"), Plane.Sections.Num(), 1))
		{
			TestTrue(TEXT("with real geometry"),
				Plane.Sections[0].Vertices.Num() > 1000);
			TestEqual(TEXT("and a scalar per vertex"),
				Plane.Sections[0].ScalarUVs.Num(), Plane.Sections[0].Vertices.Num());
		}
		TestTrue(TEXT("the echoed range is non-degenerate"),
			Model.GetCutPlaneRangeMax() > Model.GetCutPlaneRangeMin());

		// CONTROL: an INVISIBLE slice builds nothing -- the flag stays down.
		Model.Slice.SetVisible(false);
		Model.RequestSampleUpdate();
		Model.WaitForPendingSamples();
		Model.DrainSampleResults();
		TestFalse(TEXT("CONTROL: an invisible slice builds no cut plane"),
			Model.HasFreshCutPlane());
	}

	/* == The iso surface rides the same drain (renderer overhaul P4) ======== */
	{
		// The sample case carries qCriterion, so the default-enabled iso
		// pipeline builds a surface at P90 of positive Q, colored by |U|.
		Model.RequestSampleUpdate();
		Model.WaitForPendingSamples();
		Model.DrainSampleResults();

		if (!TestTrue(TEXT("a fresh iso surface arrived"), Model.HasFreshIsoSurface()))
		{
			return false;
		}
		const FFlowVizMeshPayload& Iso = Model.ConsumeIsoSurface();
		TestFalse(TEXT("consuming clears the fresh flag"), Model.HasFreshIsoSurface());
		if (TestEqual(TEXT("one section"), Iso.Sections.Num(), 1))
		{
			TestTrue(TEXT("with triangles -- the wake has vortices at P90"),
				Iso.Sections[0].Indices.Num() >= 3);
			TestEqual(TEXT("and a color-by-|U| scalar per vertex"),
				Iso.Sections[0].ScalarUVs.Num(), Iso.Sections[0].Vertices.Num());
		}
		TestTrue(TEXT("the iso value used is positive -- P90 of positive Q"),
			Model.GetLastIsoValueUsed() > 0.0);

		// The toggle gates the build: disabled means no fresh payload.
		Model.SetIsoSurfaceEnabled(false);
		Model.WaitForPendingSamples();
		Model.DrainSampleResults();
		TestFalse(TEXT("CONTROL: a disabled iso pipeline builds nothing"),
			Model.HasFreshIsoSurface());
		Model.SetIsoSurfaceEnabled(true);
		Model.WaitForPendingSamples();
		Model.DrainSampleResults();
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

/**
 * A GEOMETRY-ONLY RESULT IS STILL AN APPLIED RESULT.
 *
 * DrainSampleResults documents "@return True when anything was applied", and
 * the workspace gates EVERY mesh push on that return (SFlowVizWorkspace's
 * drain block). The streamline, velocity-sampler, iso and cut-plane branches
 * set their fresh flags without setting bApplied -- so a result that carries
 * ONLY geometry drains, marks the cut plane fresh, returns false, and the
 * mesh never reaches the actor. That result is not exotic: a case whose one
 * field is UNIFORM at frame 0 (no probes placed, max > min fails so no range,
 * no line series) produces exactly it, and the P3 "case opens showing the
 * z-mid cut plane" hero image never appears until something else changes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceSamplingGeometryOnlyDrainTest,
	"FlowViz.UI.WorkspaceModel.SamplingGeometryOnlyDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceSamplingGeometryOnlyDrainTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceSamplingTest;

	const FString CaseDir = GetScratchDir(TEXT("GeometryOnlyDrain"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*CaseDir, /*RequireExists*/ false, /*Tree*/ true);
	};
	if (!TestTrue(TEXT("CONTROL: the uniform fixture case was built on disk"),
			MakeUniformCase(CaseDir)))
	{
		return false;
	}

	FFlowVizWorkspaceModel Model;
	const FCFDVizResult OpenResult = Model.OpenCase(CaseDir);
	if (!TestTrue(*FString::Printf(TEXT("CONTROL: the uniform case opens (%s)"),
			*OpenResult.ToString()),
			OpenResult.IsOk()))
	{
		return false;
	}

	// CONTROLS establishing that the drained result will be geometry-only.
	TestEqual(TEXT("CONTROL: no probes are placed, so no probe reading can be applied"),
		Model.Probes.GetProbes().Num(), 0);
	TestTrue(TEXT("CONTROL: the slice opens visible with a domain (OpenCase's P3 default), "
				  "so the worker builds a cut plane"),
		Model.Slice.IsVisible() && Model.Slice.HasDomain());

	Model.RequestSampleUpdate();
	if (!TestTrue(TEXT("the sample completes"), Model.WaitForPendingSamples(60.0)))
	{
		return false;
	}

	const bool bApplied = Model.DrainSampleResults();

	// CONTROLS proving the result really was geometry-only AND non-empty.
	if (!TestTrue(TEXT("CONTROL: the drain stored a fresh cut plane -- something was applied"),
			Model.HasFreshCutPlane()))
	{
		return false;
	}
	TestFalse(TEXT("CONTROL: the uniform frame produced no per-frame range (max > min fails), "
				   "so nothing else in the result could have set the return"),
		Model.TransferFunction.HasCurrentFrameRange());
	TestFalse(TEXT("CONTROL: and no line series"), Model.GetLineSeries().bHasRange);

	/* THE FINDING. */
	TestTrue(TEXT("DrainSampleResults returns TRUE for a result that applied only a cut "
				  "plane -- the caller gates every mesh push on this return, so false "
				  "here is a hero image that never appears"),
		bApplied);

	return true;
}

/**
 * A REFUSED RESAMPLE IS DEFERRED, NOT DROPPED.
 *
 * RequestSampleUpdate refuses while one request is in flight -- correct, the
 * newest data wins -- but the refusal was silent AND the workspace advances
 * LastSampledFrame BEFORE calling, so the display state that asked is never
 * asked for again: scrub during a long flight and the probes, ranges and cut
 * plane permanently describe the flight's frame while the transport says the
 * new one. The fix records the refusal and re-issues from the drain, which
 * runs every tick.
 *
 * SHAPE: request 1 is issued against an empty probe list; a probe is added;
 * request 2 is issued while 1 is still in flight (the sample case's worker
 * builds iso + streamlines + cut plane, a window many orders of magnitude
 * wider than two adjacent game-thread calls) and is refused. After the drain
 * that applies request 1's result, the deferred request must run -- so a
 * second wait-and-drain must deliver the probe's reading.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceSamplingDeferredResampleTest,
	"FlowViz.UI.WorkspaceModel.SamplingDeferredResample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceSamplingDeferredResampleTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceSamplingTest;

	const FString CaseDir = GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		AddError(FString::Printf(TEXT("sample case not found at '%s'"), *CaseDir));
		return false;
	}

	FFlowVizWorkspaceModel Model;
	if (!TestTrue(TEXT("CONTROL: the sample case opens"), Model.OpenCase(CaseDir).IsOk()))
	{
		return false;
	}

	// Request 1: captured with NO probes, so its result cannot supply the
	// reading asserted at the end -- only the deferred re-issue can.
	Model.RequestSampleUpdate();

	const FGuid ProbeId =
		Model.Probes.AddProbeAtSolverPosition(KnownCellCentre(), TEXT("late probe"));
	if (!TestTrue(TEXT("CONTROL: the probe was placed"), ProbeId.IsValid()))
	{
		return false;
	}

	// Request 2, while 1 is in flight: refused. Today the refusal DROPS it.
	Model.RequestSampleUpdate();

	if (!TestTrue(TEXT("the in-flight sample completes"), Model.WaitForPendingSamples(60.0)))
	{
		return false;
	}

	// The drain that applies request 1's result is where the deferred request
	// must be re-issued -- production drains every tick, so this is the seam.
	Model.DrainSampleResults();

	if (!TestTrue(TEXT("any re-issued sample completes"), Model.WaitForPendingSamples(60.0)))
	{
		return false;
	}
	Model.DrainSampleResults();

	const FFlowVizProbe* Probe = Model.Probes.FindProbe(ProbeId);
	if (!TestNotNull(TEXT("CONTROL: the probe still exists"), Probe))
	{
		return false;
	}

	/* THE FINDING. */
	TestTrue(TEXT("a resample refused while one was in flight is re-issued by the drain, "
				  "so the probe added after the flight began still gets a reading -- "
				  "a dropped refusal leaves it blank forever"),
		Probe->LastReading.bHasValue);

	return true;
}

/**
 * A RESULT FROM A SUPERSEDED REQUEST IS DISCARDED, NOT APPLIED.
 *
 * FSampleQueue::FResult carried no generation tag, so the drain applied
 * everything unconditionally: SetField mid-flight let the old field's probe
 * readings and range land on the new binding ("colour pressure against a
 * velocity domain" -- the file's own words); LoadSession was worse (saved
 * GUIDs attach old-case readings to new-case probes). The fix stamps a
 * monotonically increasing generation into every request, bumps it on
 * OpenCase / CloseCase / SetField / LoadState, and discards mismatches at
 * the drain. CloseCase also clears the queued results outright.
 *
 * DETERMINISTIC BY CONSTRUCTION: the wait ensures the result is already
 * QUEUED before the generation-bumping call runs, so the discard decision
 * never races the worker.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceSamplingStaleGenerationTest,
	"FlowViz.UI.WorkspaceModel.SamplingStaleGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceSamplingStaleGenerationTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceSamplingTest;

	const FString CaseDir = GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		AddError(FString::Printf(TEXT("sample case not found at '%s'"), *CaseDir));
		return false;
	}

	/* == SetField mid-flight: the old field's readings and range must die ==== */
	{
		FFlowVizWorkspaceModel Model;
		if (!TestTrue(TEXT("CONTROL: the sample case opens"), Model.OpenCase(CaseDir).IsOk()))
		{
			return false;
		}

		const FGuid ProbeId =
			Model.Probes.AddProbeAtSolverPosition(KnownCellCentre(), TEXT("survivor"));
		if (!TestTrue(TEXT("CONTROL: the probe was placed"), ProbeId.IsValid()))
		{
			return false;
		}

		Model.RequestSampleUpdate();
		if (!TestTrue(TEXT("CONTROL: the sample completes BEFORE the field switch, so the "
						   "stale result is queued and the discard decision cannot race "
						   "the worker"),
				Model.WaitForPendingSamples(60.0)))
		{
			return false;
		}

		// The field switch: re-binds the transfer function (which resets its
		// per-frame range) and must invalidate the queued result.
		if (!TestTrue(TEXT("CONTROL: the field switch succeeds"),
				Model.SetField(FName(TEXT("pressure"))).IsOk()))
		{
			return false;
		}

		Model.DrainSampleResults();

		const FFlowVizProbe* Probe = Model.Probes.FindProbe(ProbeId);
		if (!TestNotNull(TEXT("CONTROL: the probe survived the field switch"), Probe))
		{
			return false;
		}

		/* THE FINDING, both halves. */
		TestFalse(TEXT("a probe reading sampled from the PREVIOUS field is discarded at the "
					   "drain rather than displayed under the new field's name"),
			Probe->LastReading.bHasValue);
		TestFalse(TEXT("and the previous field's frame range is discarded rather than "
					   "colouring pressure against a velocity domain"),
			Model.TransferFunction.HasCurrentFrameRange());
	}

	/* == Component switch supersedes and resamples ============================ */
	{
		FFlowVizWorkspaceModel Model;
		if (!TestTrue(TEXT("CONTROL: the sample case opens for the component arm"),
				Model.OpenCase(CaseDir).IsOk()))
		{
			return false;
		}

		Model.RequestSampleUpdate();
		if (!TestTrue(TEXT("CONTROL: the magnitude sample is queued before the component switch"),
				Model.WaitForPendingSamples(60.0)))
		{
			return false;
		}

		if (!TestTrue(TEXT("CONTROL: switching to X succeeds"),
				Model.TransferFunction.SetComponent(EFlowVizComponentChoice::X).IsOk()))
		{
			return false;
		}

		TestFalse(TEXT("the first drain rejects the queued Magnitude result rather than applying "
					   "its range to X"),
			Model.DrainSampleResults());
		TestFalse(TEXT("the stale Magnitude range did not land on X"),
			Model.TransferFunction.HasCurrentFrameRange());

		if (!TestTrue(TEXT("the component change reissues a sample for X"),
				Model.WaitForPendingSamples(60.0)))
		{
			return false;
		}
		TestTrue(TEXT("the replacement X sample applies"), Model.DrainSampleResults());
		TestTrue(TEXT("the replacement supplies X's current-frame range"),
			Model.TransferFunction.HasCurrentFrameRange());
	}

	/* == CloseCase clears the queue outright ================================= */
	{
		FFlowVizWorkspaceModel Model;
		if (!TestTrue(TEXT("CONTROL: the sample case opens"), Model.OpenCase(CaseDir).IsOk()))
		{
			return false;
		}

		Model.RequestSampleUpdate();
		if (!TestTrue(TEXT("CONTROL: the sample completes before the close, so a result is "
						   "genuinely queued when CloseCase runs"),
				Model.WaitForPendingSamples(60.0)))
		{
			return false;
		}

		Model.CloseCase();

		TestFalse(TEXT("a drain after CloseCase applies NOTHING -- the closed case's results "
					   "were cleared, not left for the next case to inherit"),
			Model.DrainSampleResults());
		TestFalse(TEXT("and no stale cut plane surfaces from the closed case"),
			Model.HasFreshCutPlane());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
