// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizProbeViewModel.h"

#include "CFDViz/CFDVizManifest.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Numeric probes (plan.md 10.11; ADR 004 section 8; engineering rules 4 and 10).
 *
 * THIS FILE HAS REAL GROUND TRUTH, WHICH MOST VIEW-MODEL TESTS DO NOT. The
 * shipped sample carries known_values.json, produced by the Python writer and
 * asserted independently of this code. So the probe is not checked against
 * itself: a solver-space position goes in, and the value that comes out is
 * compared against a number that was computed by another program in another
 * language. Those literals appear below, with their voxel coordinates, so a
 * reader can find them in known_values.json.
 *
 *   U        frame 0  voxel [28,14,3]  comp 0 =  3.99609375   (0x43FE)
 *   U        frame 0  voxel [28,14,3]  comp 1 =  0.52392578125
 *   U        frame 0  voxel [28,14,3]  comp 2 = -0.1397705078125
 *   U        frame 0  voxel [55,27,5]  comp 0 =  5.87890625   (0x45E1)
 *   U        frame 0  voxel [17,11,0]  comp 0 =  NaN          (0x7E00)
 *   pressure frame 0  voxel [28,14,3]  comp 0 = 19.984375     (0x4CFF)
 *
 * THE GRID, WHICH IS WHY THE POSITIONS BELOW ARE WHAT THEY ARE. 56 x 28 x 6
 * cells, origin at zero, spacing (12/56, 4/28, 1/6). All three fields are CELL
 * associated, so value (i,j,k) sits at (i+0.5)*spacing. Voxel [28,14,3] is
 * therefore centred at (6.107142857, 2.071428571, 0.583333333) m.
 *
 * THE MIRROR IS THE THING MOST WORTH TESTING. A probe placed by clicking arrives
 * in Unreal centimetres and must come back through MakeUnrealToSolverTransform.
 * If the Y negation were dropped or applied twice, the probe would read a
 * DIFFERENT CELL and report a real, plausible number from it. No range check can
 * catch that, so the test below places one probe in solver space and the
 * mirrored one in Unreal space and requires them to read the same voxel - and
 * requires a naive unmirrored placement to read a DIFFERENT one, so the check
 * can actually fail.
 *
 * WHAT HAS NO CONSUMER, AND IS THEREFORE NOT TESTED HERE. Plotting a probe over
 * time, CSV export and interactive dragging have no widget, no export path and
 * no gizmo in this plugin. They are not modelled and not asserted; a test of a
 * "plot over time" toggle with nothing to plot would be a test of a bool.
 */

namespace FlowVizProbeViewModelTest
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

	bool LoadSampleCase(FAutomationTestBase& Test, FCFDVizCase& OutCase)
	{
		const FString CaseDir = GetSampleCaseDir();
		const FString ManifestPath = FPaths::Combine(CaseDir, TEXT("manifest.json"));
		if (CaseDir.IsEmpty() || !FPaths::FileExists(ManifestPath))
		{
			Test.AddError(FString::Printf(
				TEXT("the sample case is required for this test and is missing at '%s'"),
				*ManifestPath));
			return false;
		}
		const FCFDVizResult Load = FCFDVizCase::LoadFromFile(ManifestPath, OutCase);
		if (!Load.IsOk())
		{
			Test.AddError(FString::Printf(
				TEXT("failed to load the sample manifest: %s"), *Load.ToString()));
			return false;
		}
		return true;
	}

	/** Centre of a CELL value on the sample's grid, in solver metres. */
	FVector CellCentre(int32 I, int32 J, int32 K)
	{
		return FVector(
			(static_cast<double>(I) + 0.5) * (12.0 / 56.0),
			(static_cast<double>(J) + 0.5) * (4.0 / 28.0),
			(static_cast<double>(K) + 0.5) * (1.0 / 6.0));
	}
}

/* ========================================================================== */
/* Sampling: the values are checked against the Python writer's ground truth   */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProbeSampleTest,
	"FlowViz.UI.ProbeViewModel.Sample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProbeSampleTest::RunTest(const FString& Parameters)
{
	FCFDVizCase Case;
	if (!FlowVizProbeViewModelTest::LoadSampleCase(*this, Case))
	{
		return false;
	}

	/* == A vector field, every component, against known_values.json ========== */
	{
		FFlowVizProbeReading Reading;
		const FCFDVizResult Result = FlowVizProbe::SampleStoredField(
			Case, FName(TEXT("U")), 0,
			FlowVizProbeViewModelTest::CellCentre(28, 14, 3), Reading);

		TestTrue(TEXT("sampling U at an interior cell succeeds"), Result.IsOk());
		TestTrue(TEXT("the sample is inside the domain"), Reading.bInsideDomain);
		TestTrue(TEXT("and it has a value"), Reading.bHasValue);

		// THE VOXEL IS NAMED, so a readout can say WHICH cell it came from rather
		// than implying a point measurement at the clicked position.
		TestEqual(TEXT("the reading names the voxel it came from"),
			Reading.Voxel, FIntVector(28, 14, 3));

		TestEqual(TEXT("all three components of a 3-vector are read"),
			Reading.Components.Num(), 3);
		if (Reading.Components.Num() == 3)
		{
			// EXACT EQUALITY IS CORRECT HERE. These are float16 values widened to
			// double, which is exact; a tolerance would hide an off-by-one-voxel
			// read that happened to land nearby.
			TestEqual(TEXT("component 0 matches known_values.json exactly"),
				Reading.Components[0], 3.99609375);
			TestEqual(TEXT("component 1 matches known_values.json exactly"),
				Reading.Components[1], 0.52392578125);
			TestEqual(TEXT("component 2 matches known_values.json exactly - a "
						   "component-order bug would swap this with component 0"),
				Reading.Components[2], -0.1397705078125);
		}

		// The frame's physical time, not its index, is what a readout shows.
		TestEqual(TEXT("the reading carries the frame index"), Reading.FrameIndex, 0);
		TestEqual(TEXT("and the frame's physical time"), Reading.Time, 0.0);

		// The SAMPLED position is the voxel centre, which is generally not the
		// requested one. Reported so the UI can be honest about nearest-voxel.
		TestTrue(TEXT("the sampled position is the voxel's centre"),
			Reading.SampledSolverPosition.Equals(
				FlowVizProbeViewModelTest::CellCentre(28, 14, 3), 1.0e-9));
	}

	/* == A position anywhere inside a cell reads that cell =================== */
	{
		// NOT THE CENTRE, DELIBERATELY. A user clicks somewhere in a cell, not on
		// its centre. Offsetting by 40% of the spacing stays inside voxel
		// [28,14,3]; an implementation that added a spurious half-cell before
		// flooring would fall into the neighbour and return a different, entirely
		// plausible number.
		const FVector Spacing(12.0 / 56.0, 4.0 / 28.0, 1.0 / 6.0);
		const FVector OffCentre =
			FlowVizProbeViewModelTest::CellCentre(28, 14, 3) + Spacing * 0.4;

		FFlowVizProbeReading Reading;
		TestTrue(TEXT("sampling off-centre within a cell succeeds"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0, OffCentre, Reading).IsOk());
		TestEqual(TEXT("a position 40% off centre still reads its own cell - a "
					   "spurious half-cell offset would read the neighbour"),
			Reading.Voxel, FIntVector(28, 14, 3));
		if (Reading.Components.Num() == 3)
		{
			TestEqual(TEXT("and returns that cell's value"),
				Reading.Components[0], 3.99609375);
		}
	}

	/* == The partial edge brick, which is where tiling arithmetic breaks ===== */
	{
		FFlowVizProbeReading Reading;
		TestTrue(TEXT("sampling the partial edge brick succeeds"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0,
				FlowVizProbeViewModelTest::CellCentre(55, 27, 5), Reading).IsOk());
		TestTrue(TEXT("the last cell of the domain is inside it"), Reading.bInsideDomain);
		// The grid is 56 x 28 x 6 with 32 x 32 x 8 bricks, so [55,27,5] is in a
		// brick that is partial on all three axes - the case a stride bug hits.
		if (Reading.Components.Num() == 3)
		{
			TestEqual(TEXT("the partial edge brick reads correctly"),
				Reading.Components[0], 5.87890625);
		}
	}

	/* == NaN IS PRESERVED, not coerced (format rule 1.7, engineering rule 10) = */
	{
		FFlowVizProbeReading Reading;
		TestTrue(TEXT("sampling a masked cell succeeds"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0,
				FlowVizProbeViewModelTest::CellCentre(17, 11, 0), Reading).IsOk());
		TestTrue(TEXT("a masked cell is still inside the domain"), Reading.bInsideDomain);
		TestTrue(TEXT("and the read succeeded"), Reading.bHasValue);

		if (Reading.Components.Num() == 3)
		{
			/*
			 * COERCING NaN TO ZERO WOULD TURN "no data" INTO "a measurement of
			 * zero" - which for a velocity field is a perfectly plausible reading
			 * inside a solid body, and therefore invisible.
			 */
			TestTrue(TEXT("a NaN component arrives as NaN, not as zero"),
				FMath::IsNaN(Reading.Components[0]));
			TestFalse(TEXT("and is not silently finite"),
				FMath::IsFinite(Reading.Components[0]));
		}

		// A magnitude built from a NaN component must not come out finite, or an
		// invalid sample would colour like a real one.
		TestTrue(TEXT("the magnitude of a NaN vector is NaN, so an invalid sample "
					  "cannot masquerade as a finite one"),
			FMath::IsNaN(Reading.Magnitude));
	}

	/* == A scalar field: one component, and the magnitude is its size ======== */
	{
		FFlowVizProbeReading Reading;
		TestTrue(TEXT("sampling pressure succeeds"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("pressure")), 0,
				FlowVizProbeViewModelTest::CellCentre(28, 14, 3), Reading).IsOk());
		TestEqual(TEXT("a scalar field yields exactly one component"),
			Reading.Components.Num(), 1);
		if (Reading.Components.Num() == 1)
		{
			TestEqual(TEXT("pressure matches known_values.json exactly"),
				Reading.Components[0], 19.984375);
		}
		TestEqual(TEXT("a scalar's magnitude is its absolute value"),
			Reading.Magnitude, 19.984375);
	}

	/* == The magnitude is the Euclidean norm over the declared components ==== */
	{
		FFlowVizProbeReading Reading;
		TestTrue(TEXT("sampling U succeeds"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0,
				FlowVizProbeViewModelTest::CellCentre(28, 14, 3), Reading).IsOk());

		// Computed from the three ground-truth components, so this fails if the
		// magnitude were taken over the first component alone or over a pad.
		const double Expected = FMath::Sqrt(
			3.99609375 * 3.99609375
			+ 0.52392578125 * 0.52392578125
			+ 0.1397705078125 * 0.1397705078125);
		TestTrue(TEXT("the magnitude is the norm over ALL declared components"),
			FMath::IsNearlyEqual(Reading.Magnitude, Expected, 1.0e-12));
		TestTrue(TEXT("and is therefore larger than any single component"),
			Reading.Magnitude > Reading.Components[0]);
	}

	/* == A later frame reads different data ================================= */
	{
		FFlowVizProbeReading Frame0;
		FFlowVizProbeReading Frame19;
		TestTrue(TEXT("sampling frame 0 succeeds"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0,
				FlowVizProbeViewModelTest::CellCentre(28, 14, 3), Frame0).IsOk());
		TestTrue(TEXT("sampling frame 19 succeeds"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 19,
				FlowVizProbeViewModelTest::CellCentre(28, 14, 3), Frame19).IsOk());

		// GROUND TRUTH FOR FRAME 19, so "the frame index reaches the path" is
		// checked against a value rather than against frame 0 being different.
		if (Frame19.Components.Num() == 3)
		{
			TestEqual(TEXT("frame 19 matches known_values.json exactly"),
				Frame19.Components[0], 4.03515625);
		}
		TestNotEqual(TEXT("the frame index actually selects a different frame's file"),
			Frame19.Components[0], Frame0.Components[0]);
		TestEqual(TEXT("and the reading carries frame 19's physical time"),
			Frame19.Time, 0.95);
	}

	/* == OUTSIDE IS AN ANSWER, NOT AN ERROR ================================= */
	{
		FFlowVizProbeReading Reading;
		// Well past the 12 x 4 x 1 domain.
		const FCFDVizResult Result = FlowVizProbe::SampleStoredField(
			Case, FName(TEXT("U")), 0, FVector(100.0, 100.0, 100.0), Reading);

		// Ok, DELIBERATELY. "Outside the domain" is something a readout displays;
		// a failure would put an error dialog in front of a user who moved a
		// probe past the edge, which is an ordinary thing to do.
		TestTrue(TEXT("a position outside the domain is not an error"), Result.IsOk());
		TestFalse(TEXT("but it is reported as outside"), Reading.bInsideDomain);
		TestFalse(TEXT("and carries no value"), Reading.bHasValue);
		// NOT ZERO-FILLED (rule 10). An empty component list cannot be formatted
		// as "0.0" by a careless readout.
		TestEqual(TEXT("an outside reading is empty, never zero-filled"),
			Reading.Components.Num(), 0);

		// Just below the origin, which is the off-by-one that a floor-vs-round
		// mistake produces.
		FFlowVizProbeReading JustBelow;
		TestTrue(TEXT("a position just below the origin is not an error"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0, FVector(-0.001, 0.5, 0.5), JustBelow).IsOk());
		TestFalse(TEXT("a position just below the origin is outside - flooring a "
					   "negative must not land on voxel 0"),
			JustBelow.bInsideDomain);

		// Just past the far face, symmetrically.
		FFlowVizProbeReading JustAbove;
		TestTrue(TEXT("a position just past the far face is not an error"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0, FVector(12.001, 2.0, 0.5), JustAbove).IsOk());
		TestFalse(TEXT("a position just past the far face is outside"),
			JustAbove.bInsideDomain);

		// And the cell immediately inside that face IS sampled, so the bound is
		// checked in both directions and the "outside" check can actually fail.
		FFlowVizProbeReading LastCell;
		TestTrue(TEXT("the last cell is sampled"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0,
				FlowVizProbeViewModelTest::CellCentre(55, 14, 3), LastCell).IsOk());
		TestTrue(TEXT("the cell just inside the far face IS inside"),
			LastCell.bInsideDomain);
	}

	/* == Real failures are failures ========================================= */
	{
		FFlowVizProbeReading Reading;
		TestFalse(TEXT("an unknown field is a failure, not an empty reading"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("noSuchField")), 0,
				FlowVizProbeViewModelTest::CellCentre(28, 14, 3), Reading).IsOk());

		TestFalse(TEXT("a frame past the end of the timeline is a failure"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 9999,
				FlowVizProbeViewModelTest::CellCentre(28, 14, 3), Reading).IsOk());
		TestFalse(TEXT("a negative frame is a failure"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), -1,
				FlowVizProbeViewModelTest::CellCentre(28, 14, 3), Reading).IsOk());

		// A NaN position is a caller bug, not a point outside the domain.
		TestFalse(TEXT("a non-finite position is a failure"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0, FVector(NAN, 0.0, 0.0), Reading).IsOk());
		TestFalse(TEXT("and yields no value"), Reading.bHasValue);
	}

	return true;
}

/* ========================================================================== */
/* Placement: the Y-mirror, and the one adapter that may apply it             */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProbePlacementTest,
	"FlowViz.UI.ProbeViewModel.Placement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProbePlacementTest::RunTest(const FString& Parameters)
{
	FCFDVizCase Case;
	if (!FlowVizProbeViewModelTest::LoadSampleCase(*this, Case))
	{
		return false;
	}

	/*
	 * THE MIRROR, CHECKED AGAINST THE STORED FIELD RATHER THAN AGAINST ITSELF.
	 *
	 * A round trip through MakeUnrealToSolverTransform and back would agree with
	 * itself even if the mirror were dropped from both. So the assertion is that
	 * a probe placed by CLICKING - in Unreal centimetres, with y already negated
	 * by the conversion the renderer applied - reads the SAME GROUND-TRUTH VALUE
	 * as one placed by typing solver coordinates.
	 *
	 * AND THE CONTROL, so the check can actually fail. This fixture's domain lies
	 * wholly in positive y ([0, 4] m, origin at zero), so dropping the negation
	 * sends the position to negative y and OUT of the domain entirely - the
	 * unmirrored read comes back bInsideDomain = false. That is what the control
	 * below asserts, and it is the real behaviour rather than a different voxel.
	 *
	 * BE CLEAR ABOUT HOW STRONG THIS IS. On this fixture a dropped mirror is
	 * caught loudly, because "outside the domain" is unmistakable. On a domain
	 * STRADDLING y = 0 the same bug would land in a real cell and report a
	 * plausible wrong value, and this test would not distinguish it. No such
	 * fixture exists in the tree; if one is ever added, the sharper assertion
	 * belongs here.
	 */
	{
		const FVector SolverPosition = FlowVizProbeViewModelTest::CellCentre(28, 14, 3);
		const FVector UnrealPosition =
			SolverToUnrealPosition(SolverPosition, CFDViz::MetersToUnrealCentimeters);

		// Sanity: the conversion really did negate y and scale by 100, so the
		// "different voxel" claim below is about a real difference.
		TestTrue(TEXT("the Unreal position is the mirrored, scaled solver one"),
			FMath::IsNearlyEqual(UnrealPosition.Y, -SolverPosition.Y * 100.0, 1.0e-9));

		FFlowVizProbeViewModel ViewModel;
		const FGuid Typed = ViewModel.AddProbeAtSolverPosition(SolverPosition, TEXT("typed"));
		const FGuid Clicked = ViewModel.AddProbeAtUnrealPosition(UnrealPosition, TEXT("clicked"));
		TestTrue(TEXT("the typed probe was placed"), Typed.IsValid());
		TestTrue(TEXT("the clicked probe was placed"), Clicked.IsValid());

		const FFlowVizProbe* ClickedProbe = ViewModel.FindProbe(Clicked);
		if (ClickedProbe == nullptr)
		{
			AddError(TEXT("the clicked probe was not stored"));
			return false;
		}

		// RULE 4: the STATE is solver units. A view model storing centimetres
		// would report positions 100x wrong on a metre case and 100000x wrong on
		// a millimetre one, while still looking like a position.
		TestTrue(TEXT("a clicked probe's stored position is in SOLVER units, not centimetres"),
			ClickedProbe->SolverPosition.Equals(SolverPosition, 1.0e-9));

		FFlowVizProbeReading ClickedReading;
		TestTrue(TEXT("sampling at the clicked probe succeeds"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0, ClickedProbe->SolverPosition, ClickedReading).IsOk());
		TestEqual(TEXT("a probe placed by clicking reads the voxel under the click"),
			ClickedReading.Voxel, FIntVector(28, 14, 3));
		if (ClickedReading.Components.Num() == 3)
		{
			TestEqual(TEXT("and reads the ground-truth value from it"),
				ClickedReading.Components[0], 3.99609375);
		}

		/*
		 * THE CONTROL THAT MAKES THE ABOVE A CHECK RATHER THAN A COINCIDENCE.
		 * Scale but do not mirror, and the position leaves the domain entirely on
		 * this fixture - see the note at the top of this block for the case where
		 * it would not.
		 */
		const FVector Unmirrored = UnrealPosition / CFDViz::MetersToUnrealCentimeters;
		TestTrue(TEXT("the unmirrored position really is the mirrored one's y, negated"),
			FMath::IsNearlyEqual(Unmirrored.Y, -SolverPosition.Y, 1.0e-9));

		FFlowVizProbeReading UnmirroredReading;
		TestTrue(TEXT("sampling the unmirrored position is not an error"),
			FlowVizProbe::SampleStoredField(
				Case, FName(TEXT("U")), 0, Unmirrored, UnmirroredReading).IsOk());
		TestFalse(TEXT("a placement that skipped the mirror falls outside the domain "
					   "on this fixture - which is what makes the mirrored assertion "
					   "above a real check rather than a coincidence"),
			UnmirroredReading.bInsideDomain);
		TestNotEqual(TEXT("and reads a different voxel from the mirrored placement"),
			UnmirroredReading.Voxel, ClickedReading.Voxel);
	}

	/* == Drawing a probe is the exact inverse of placing it ================== */
	{
		FFlowVizProbeViewModel ViewModel;
		const FVector SolverPosition = FlowVizProbeViewModelTest::CellCentre(28, 14, 3);
		const FGuid Id = ViewModel.AddProbeAtSolverPosition(SolverPosition);

		FVector Drawn;
		TestTrue(TEXT("the probe's Unreal position is available"),
			ViewModel.TryGetProbeUnrealPosition(Id, Drawn));
		// If the marker were drawn without the mirror, it would appear at the
		// mirrored location while reading the correct cell - a probe whose label
		// and position disagree.
		TestTrue(TEXT("the marker is drawn where the click was, mirror included"),
			Drawn.Equals(
				SolverToUnrealPosition(SolverPosition, CFDViz::MetersToUnrealCentimeters),
				1.0e-6));
		TestTrue(TEXT("and the mirror is visible in the drawn y"), Drawn.Y < 0.0);
	}

	/* == A non-metre case scales, and still reads the right cell ============= */
	{
		// The scale is not assumed: a case in centimetres has units.length = "cm",
		// so one solver unit is one Unreal unit.
		FFlowVizProbeViewModel ViewModel;
		TestTrue(TEXT("a centimetre-scaled case is accepted"),
			ViewModel.SetMetersToUnrealUnits(1.0).IsOk());

		const FVector SolverPosition = FlowVizProbeViewModelTest::CellCentre(28, 14, 3);
		const FVector UnrealPosition = SolverToUnrealPosition(SolverPosition, 1.0);
		const FGuid Id = ViewModel.AddProbeAtUnrealPosition(UnrealPosition);

		const FFlowVizProbe* Probe = ViewModel.FindProbe(Id);
		if (Probe == nullptr)
		{
			AddError(TEXT("the probe was not stored"));
			return false;
		}
		TestTrue(TEXT("the case's own length scale is used, not an assumed 100"),
			Probe->SolverPosition.Equals(SolverPosition, 1.0e-9));

		// A degenerate scale would make MakeUnrealToSolverTransform fall back to
		// the identity, silently dropping the mirror. Refused before it can.
		TestFalse(TEXT("a zero scale is refused before it can reach the identity fallback"),
			ViewModel.SetMetersToUnrealUnits(0.0).IsOk());
		TestFalse(TEXT("a negative scale is refused"),
			ViewModel.SetMetersToUnrealUnits(-100.0).IsOk());
		TestFalse(TEXT("a non-finite scale is refused"),
			ViewModel.SetMetersToUnrealUnits(NAN).IsOk());
		TestEqual(TEXT("and the good scale survived every refusal"),
			ViewModel.GetMetersToUnrealUnits(), 1.0);
	}

	/* == The probe list, and what moving a probe does to its reading ========= */
	{
		FFlowVizProbeViewModel ViewModel;
		const FGuid First = ViewModel.AddProbeAtSolverPosition(FVector(1.0, 1.0, 0.5), TEXT("A"));
		const FGuid Second = ViewModel.AddProbeAtSolverPosition(FVector(2.0, 1.0, 0.5), TEXT("B"));
		TestEqual(TEXT("both probes are in the list"), ViewModel.GetProbeCount(), 2);
		TestNotEqual(TEXT("probe ids are distinct, so a chart can key series by them"),
			First, Second);

		// Give the first probe a reading, as a worker would.
		FFlowVizProbeReading Reading;
		Reading.bHasValue = true;
		Reading.bInsideDomain = true;
		Reading.Components.Add(42.0);
		Reading.Magnitude = 42.0;
		TestTrue(TEXT("a worker's reading is stored"), ViewModel.SetProbeReading(First, Reading));

		const FFlowVizProbe* Probe = ViewModel.FindProbe(First);
		if (Probe == nullptr)
		{
			AddError(TEXT("the probe was not stored"));
			return false;
		}
		TestTrue(TEXT("the reading is on the probe"), Probe->LastReading.bHasValue);

		/*
		 * MOVING A PROBE DROPS ITS READING. A stale value beside a new position
		 * is the worst outcome available: a real measurement, correctly
		 * formatted, attributed to the wrong point. A blank readout until the
		 * next sample is honest.
		 */
		TestTrue(TEXT("moving the probe succeeds"),
			ViewModel.MoveProbeToSolverPosition(First, FVector(5.0, 1.0, 0.5)));
		Probe = ViewModel.FindProbe(First);
		TestFalse(TEXT("moving a probe drops its reading rather than attributing an "
					   "old measurement to a new position"),
			Probe->LastReading.bHasValue);
		TestEqual(TEXT("and the stale components are gone, not left to be formatted"),
			Probe->LastReading.Components.Num(), 0);

		// Hiding is not deleting.
		TestTrue(TEXT("hiding succeeds"), ViewModel.SetProbeVisible(Second, false));
		TestEqual(TEXT("a hidden probe stays in the list"), ViewModel.GetProbeCount(), 2);
		TestFalse(TEXT("and is marked hidden"), ViewModel.FindProbe(Second)->bVisible);

		TestTrue(TEXT("renaming succeeds"), ViewModel.RenameProbe(Second, TEXT("Renamed")));
		TestEqual(TEXT("the name took"), ViewModel.FindProbe(Second)->Name, FString(TEXT("Renamed")));

		TestTrue(TEXT("removing succeeds"), ViewModel.RemoveProbe(Second));
		TestEqual(TEXT("the probe is gone"), ViewModel.GetProbeCount(), 1);
		TestFalse(TEXT("removing it twice fails"), ViewModel.RemoveProbe(Second));
		TestEqual(TEXT("and did not remove anything else"), ViewModel.GetProbeCount(), 1);

		// Operations on an unknown id fail rather than affecting an arbitrary probe.
		const FGuid Unknown = FGuid::NewGuid();
		FVector UnusedPosition = FVector::ZeroVector;
		TestFalse(TEXT("moving an unknown probe fails"),
			ViewModel.MoveProbeToSolverPosition(Unknown, FVector::ZeroVector));
		TestFalse(TEXT("renaming an unknown probe fails"),
			ViewModel.RenameProbe(Unknown, TEXT("x")));
		TestFalse(TEXT("reading an unknown probe's position fails"),
			ViewModel.TryGetProbeUnrealPosition(Unknown, UnusedPosition));

		ViewModel.RemoveAllProbes();
		TestEqual(TEXT("clearing removes everything"), ViewModel.GetProbeCount(), 0);
	}

	/* == A non-finite placement is refused ================================== */
	{
		FFlowVizProbeViewModel ViewModel;
		// An invalid GUID is the refusal. A NaN-positioned probe samples nothing
		// and would sit in the list looking placed.
		TestFalse(TEXT("a non-finite solver placement is refused"),
			ViewModel.AddProbeAtSolverPosition(FVector(NAN, 0.0, 0.0)).IsValid());
		TestFalse(TEXT("a non-finite Unreal placement is refused"),
			ViewModel.AddProbeAtUnrealPosition(FVector(0.0, NAN, 0.0)).IsValid());
		TestEqual(TEXT("and nothing was added"), ViewModel.GetProbeCount(), 0);
	}

	return true;
}

/* ========================================================================== */
/* The line probe                                                             */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProbeLineTest,
	"FlowViz.UI.ProbeViewModel.Line",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProbeLineTest::RunTest(const FString& Parameters)
{
	FCFDVizCase Case;
	if (!FlowVizProbeViewModelTest::LoadSampleCase(*this, Case))
	{
		return false;
	}

	/* == BOTH ENDPOINTS ARE SAMPLED ========================================= */
	{
		FFlowVizProbeViewModel ViewModel;
		const FVector Start(1.0, 2.0, 0.5);
		const FVector End(11.0, 2.0, 0.5);
		TestTrue(TEXT("a line is accepted"), ViewModel.SetLineProbe(Start, End).IsOk());
		TestTrue(TEXT("and is reported present"), ViewModel.HasLineProbe());
		TestTrue(TEXT("a sample count is accepted"), ViewModel.SetLineSampleCount(11).IsOk());

		TArray<FVector> Positions;
		TestTrue(TEXT("the sample positions are available"),
			ViewModel.GetLineSamplePositions(Positions).IsOk());
		TestEqual(TEXT("there are exactly as many samples as were asked for"),
			Positions.Num(), 11);

		/*
		 * N SAMPLES OVER N-1 INTERVALS. Dividing by N would put the last sample
		 * at 10/11 of the way along - a line plot that stops just short of the
		 * far wall, which reads as a boundary effect in the DATA rather than as
		 * an off-by-one in the sampler.
		 */
		TestTrue(TEXT("sample 0 is exactly the start point"),
			Positions[0].Equals(Start, 1.0e-12));
		TestTrue(TEXT("the last sample is exactly the end point - dividing by N "
					  "instead of N-1 would stop short of it"),
			Positions.Last().Equals(End, 1.0e-12));
		TestTrue(TEXT("the middle sample is the midpoint"),
			Positions[5].Equals(FVector(6.0, 2.0, 0.5), 1.0e-12));
	}

	/* == The samples read the field, and the values vary along the line ====== */
	{
		FFlowVizProbeViewModel ViewModel;
		// Along the wake centreline, which is where the sample case actually has
		// structure - a line through a uniform region would pass under a sampler
		// that returned the same voxel every time.
		TestTrue(TEXT("a line is accepted"),
			ViewModel.SetLineProbe(FVector(0.5, 2.0, 0.5), FVector(11.5, 2.0, 0.5)).IsOk());
		TestTrue(TEXT("a sample count is accepted"), ViewModel.SetLineSampleCount(24).IsOk());

		TArray<FVector> Positions;
		TestTrue(TEXT("the sample positions are available"),
			ViewModel.GetLineSamplePositions(Positions).IsOk());

		TSet<FIntVector> DistinctVoxels;
		TSet<double> DistinctValues;
		int32 InsideCount = 0;
		for (const FVector& Position : Positions)
		{
			FFlowVizProbeReading Reading;
			if (!FlowVizProbe::SampleStoredField(
					Case, FName(TEXT("U")), 0, Position, Reading).IsOk())
			{
				AddError(TEXT("a line sample failed"));
				return false;
			}
			if (Reading.bHasValue)
			{
				++InsideCount;
				DistinctVoxels.Add(Reading.Voxel);
				DistinctValues.Add(Reading.Components[0]);
			}
		}

		TestEqual(TEXT("every sample along an interior line is inside the domain"),
			InsideCount, Positions.Num());

		/*
		 * A DEGENERATE FIXTURE WOULD DEFEAT THIS TEST. If every sample landed in
		 * the same voxel, or the field were constant along this line, a sampler
		 * that ignored its position entirely would pass. Both are asserted
		 * against: the samples must span many voxels AND read many values.
		 */
		TestTrue(TEXT("the samples span many voxels, so a position-ignoring sampler fails"),
			DistinctVoxels.Num() > 10);
		TestTrue(TEXT("and read many distinct values, so a constant-returning sampler fails"),
			DistinctValues.Num() > 10);
	}

	/* == The axis modes ===================================================== */
	{
		FFlowVizProbeViewModel ViewModel;
		// Length 10 in x, so the distance axis has an obvious expected value.
		TestTrue(TEXT("a line is accepted"),
			ViewModel.SetLineProbe(FVector(1.0, 2.0, 0.5), FVector(11.0, 2.0, 0.5)).IsOk());
		TestTrue(TEXT("a sample count is accepted"), ViewModel.SetLineSampleCount(11).IsOk());

		ViewModel.SetLineAxisMode(EFlowVizLineProbeAxis::Distance);
		TArray<double> Distances;
		TestTrue(TEXT("the axis values are available"),
			ViewModel.GetLineSampleAxisValues(Distances).IsOk());
		TestEqual(TEXT("there is one axis value per sample"), Distances.Num(), 11);
		TestTrue(TEXT("the distance axis starts at zero"),
			FMath::IsNearlyZero(Distances[0], 1.0e-12));
		// SOLVER UNITS, which is what a user reads off a chart labelled in metres.
		// A centimetre axis would read 1000 here and look like a different case.
		TestTrue(TEXT("the distance axis ends at the line's length in SOLVER units"),
			FMath::IsNearlyEqual(Distances.Last(), 10.0, 1.0e-12));
		TestTrue(TEXT("and is linear in between"),
			FMath::IsNearlyEqual(Distances[5], 5.0, 1.0e-12));

		ViewModel.SetLineAxisMode(EFlowVizLineProbeAxis::NormalizedDistance);
		TArray<double> Normalized;
		TestTrue(TEXT("the normalized axis values are available"),
			ViewModel.GetLineSampleAxisValues(Normalized).IsOk());
		TestTrue(TEXT("the normalized axis starts at 0"),
			FMath::IsNearlyZero(Normalized[0], 1.0e-12));
		// This is what makes two lines of different lengths comparable on one
		// chart, so it must be 1 regardless of the line's length.
		TestTrue(TEXT("the normalized axis ends at 1 regardless of length"),
			FMath::IsNearlyEqual(Normalized.Last(), 1.0, 1.0e-12));
		TestTrue(TEXT("and the two modes genuinely differ"),
			!FMath::IsNearlyEqual(Normalized.Last(), Distances.Last(), 1.0e-6));
	}

	/* == Refusals =========================================================== */
	{
		FFlowVizProbeViewModel ViewModel;

		TArray<FVector> Positions;
		TArray<double> AxisValues;
		// Asking for samples before a line exists is a failure, not an empty list
		// that a chart would draw as a flat zero series.
		TestFalse(TEXT("sample positions are refused before a line is placed"),
			ViewModel.GetLineSamplePositions(Positions).IsOk());
		TestFalse(TEXT("axis values are refused before a line is placed"),
			ViewModel.GetLineSampleAxisValues(AxisValues).IsOk());

		// A ZERO-LENGTH LINE samples the same voxel N times and plots a flat line
		// against a zero-width axis, which reads as "the field is constant here".
		TestFalse(TEXT("a zero-length line is refused"),
			ViewModel.SetLineProbe(FVector(1.0, 2.0, 0.5), FVector(1.0, 2.0, 0.5)).IsOk());
		TestFalse(TEXT("a non-finite endpoint is refused"),
			ViewModel.SetLineProbe(FVector(NAN, 2.0, 0.5), FVector(11.0, 2.0, 0.5)).IsOk());
		TestFalse(TEXT("and no line was placed"), ViewModel.HasLineProbe());

		TestTrue(TEXT("a good line is accepted"),
			ViewModel.SetLineProbe(FVector(1.0, 2.0, 0.5), FVector(11.0, 2.0, 0.5)).IsOk());
		TestTrue(TEXT("a good count is accepted"), ViewModel.SetLineSampleCount(16).IsOk());

		TestFalse(TEXT("one sample is refused - it is a point probe wearing a "
					   "line's controls"),
			ViewModel.SetLineSampleCount(1).IsOk());
		TestFalse(TEXT("zero samples are refused"), ViewModel.SetLineSampleCount(0).IsOk());
		// Bounded because each sample is a brick decode: an unbounded typed count
		// would hang the worker rather than report a limit.
		TestFalse(TEXT("a count past the ceiling is refused rather than left to "
					   "hang the worker on brick decodes"),
			ViewModel.SetLineSampleCount(FlowVizProbe::MaxLineSamples + 1).IsOk());
		TestEqual(TEXT("and the good count survived every refusal"),
			ViewModel.GetLineSampleCount(), 16);

		TestTrue(TEXT("exactly two samples is legal - both endpoints, nothing between"),
			ViewModel.SetLineSampleCount(FlowVizProbe::MinLineSamples).IsOk());
		TArray<FVector> Pair;
		TestTrue(TEXT("a two-sample line yields its endpoints"),
			ViewModel.GetLineSamplePositions(Pair).IsOk());
		TestEqual(TEXT("exactly two samples"), Pair.Num(), 2);
		TestTrue(TEXT("the first is the start"), Pair[0].Equals(FVector(1.0, 2.0, 0.5), 1.0e-12));
		TestTrue(TEXT("the second is the end"), Pair[1].Equals(FVector(11.0, 2.0, 0.5), 1.0e-12));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
