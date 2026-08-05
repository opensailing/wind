// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizTypes.h"
#include "CFDViz/CFDVizManifest.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The canonical-to-Unreal conversion adapter (ADR 004).
 *
 * WHY THIS TEST IS SHAPED THE WAY IT IS. Canonical and Unreal differ only in
 * the sign of Y and a scale factor. Both are Z-up and X-forward. So a broken
 * conversion does not produce a garbled render - it produces a plausible render
 * that is mirrored, and a mirrored vortex street still looks like a vortex
 * street. Nothing on screen reports this.
 *
 * That rules out the obvious test. A round trip solver -> Unreal -> solver
 * returns the input whether the Y-flip is present in BOTH directions or absent
 * from BOTH; it is exactly as green against an adapter that has forgotten
 * handedness entirely. Round trips are still asserted below, but only as a
 * consistency check between the two matrices - never as evidence that the
 * mirror happens. Every conversion gets at least one ABSOLUTE assertion against
 * a hand-computed value.
 *
 * Fixtures are asymmetric in all three axes and use non-zero Y throughout. A
 * point like (1,1,1) survives any axis transposition, and a point on Y=0
 * survives a missing Y-flip - which is precisely the fixture that gets written
 * by accident when the interesting geometry is centred on the origin plane.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizCoordinateTest,
	"FlowViz.CFDViz.Coordinates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizCoordinateTest::RunTest(const FString& Parameters)
{
	// Asymmetric in every component, all non-zero, no two equal, and no two
	// related by a sign flip - so a transposition, a dropped axis or a wrong
	// mirror all produce a different answer.
	const FVector SolverPoint(2.0, 3.0, 5.0);

	/* -- Positions: absolute values, not just a round trip ------------------ */
	{
		const FVector Unreal = SolverToUnrealPosition(SolverPoint);

		// Hand-computed against the default scale of 100 cm/m.
		TestEqual(TEXT("X scales and keeps its sign"), Unreal.X, 200.0);
		TestEqual(TEXT("Y scales and FLIPS - the whole point of the adapter"), Unreal.Y, -300.0);
		TestEqual(TEXT("Z scales and keeps its sign"), Unreal.Z, 500.0);

		// Stated separately because "Y is negative" is the assertion that fails
		// when someone removes the mirror, and it should fail by name.
		TestTrue(TEXT("a positive solver Y becomes a negative Unreal Y"), Unreal.Y < 0.0);

		// A non-default scale must actually be used. Passing 1.0 and getting
		// centimetres back would mean the parameter is decorative.
		const FVector Unscaled = SolverToUnrealPosition(SolverPoint, 1.0);
		TestEqual(TEXT("scale 1 leaves magnitudes alone"), Unscaled.X, 2.0);
		TestEqual(TEXT("and still mirrors Y"), Unscaled.Y, -3.0);

		// A millimetre case: 0.001 m/unit * 100 cm/m = 0.1. This is the number
		// that is wrong by 1000x if someone defaults the unit lookup.
		const FVector Millimetres = SolverToUnrealPosition(SolverPoint, 0.1);
		TestEqual(TEXT("a millimetre-scale case converts by 0.1"), Millimetres.X, 0.2);
	}

	/* -- The matrix must agree with the per-point function ------------------ */
	{
		// Two implementations of one transform (CFDVizTypes.cpp writes the
		// per-point version out longhand for speed). If they ever disagree,
		// meshes and glyphs land in different places.
		const FMatrix Forward = MakeSolverToUnrealTransform();
		const FVector ViaMatrix = Forward.TransformPosition(SolverPoint);
		const FVector ViaFunction = SolverToUnrealPosition(SolverPoint);

		TestEqual(TEXT("matrix and per-point conversion agree"), ViaMatrix, ViaFunction);

		// The reflection itself, stated as the determinant. This is what
		// TransformReversesWinding keys off.
		TestTrue(TEXT("the transform has negative determinant"), Forward.Determinant() < 0.0);
		TestTrue(TEXT("TransformReversesWinding agrees"), TransformReversesWinding(Forward));

		// Control: a matrix that does NOT reverse winding must report false, or
		// the function is a constant and proves nothing about the one above.
		TestFalse(TEXT("identity does not reverse winding"),
			TransformReversesWinding(FMatrix::Identity));
		TestFalse(TEXT("a pure uniform scale does not reverse winding"),
			TransformReversesWinding(FScaleMatrix(3.0)));
		// Two mirrors cancel: determinant is positive again.
		TestFalse(TEXT("mirroring twice does not reverse winding"),
			TransformReversesWinding(FScaleMatrix(FVector(-1.0, -1.0, 1.0))));
	}

	/* -- Inverse: absolute, then round trip as a consistency check ---------- */
	{
		const FMatrix Inverse = MakeUnrealToSolverTransform();

		// Absolute first. An Unreal point with negative Y came from a POSITIVE
		// solver Y, and 500 cm is 5 m.
		const FVector Back = Inverse.TransformPosition(FVector(200.0, -300.0, 500.0));
		TestEqual(TEXT("inverse recovers X in solver units"), Back.X, 2.0);
		TestEqual(TEXT("inverse unflips Y"), Back.Y, 3.0);
		TestEqual(TEXT("inverse recovers Z"), Back.Z, 5.0);

		// Only now the round trip - and only as a check that the two matrices
		// are mutually consistent. On its own this passes with no mirror at all.
		const FVector RoundTripped = Inverse.TransformPosition(SolverToUnrealPosition(SolverPoint));
		TestTrue(TEXT("round trip returns the original point"),
			RoundTripped.Equals(SolverPoint, UE_DOUBLE_KINDA_SMALL_NUMBER));

		// Same at a non-default scale, where a hardcoded 100 would show up.
		const FMatrix InverseMm = MakeUnrealToSolverTransform(0.1);
		const FVector BackMm = InverseMm.TransformPosition(SolverToUnrealPosition(SolverPoint, 0.1));
		TestTrue(TEXT("round trip holds at a millimetre scale"),
			BackMm.Equals(SolverPoint, UE_DOUBLE_KINDA_SMALL_NUMBER));

		// A degenerate scale returns the identity rather than infinities. The
		// caller is expected to have validated units.length; this only ensures
		// the failure is inert instead of poisoning positions with NaN.
		AddExpectedError(TEXT("MakeUnrealToSolverTransform: invalid length scale"),
			EAutomationExpectedErrorFlags::Contains, 0);
		TestTrue(TEXT("a zero scale yields the identity, not infinities"),
			MakeUnrealToSolverTransform(0.0).Equals(FMatrix::Identity));
		TestTrue(TEXT("a non-finite scale yields the identity"),
			MakeUnrealToSolverTransform(std::numeric_limits<double>::quiet_NaN())
				.Equals(FMatrix::Identity));
	}

	/* -- True vectors vs pseudovectors -------------------------------------- */
	{
		const FVector3f Solver(2.0f, 3.0f, 5.0f);

		const FVector3f Direction = SolverToUnrealDirection(Solver);
		TestEqual(TEXT("a true vector mirrors Y"), Direction, FVector3f(2.0f, -3.0f, 5.0f));

		// Rule 4: a velocity in m/s stays in m/s. If a length scale ever leaks
		// in here, every arrow glyph becomes 100x too long and the readout lies.
		TestEqual(TEXT("and is NOT scaled to centimetres"), Direction.X, 2.0f);

		const FVector3f Pseudo = SolverToUnrealPseudoVector(Solver);
		TestEqual(TEXT("a pseudovector picks up det(M) = -1"), Pseudo, FVector3f(-2.0f, 3.0f, -5.0f));

		// THE assertion of this block. Vorticity transformed as a true vector
		// spins the wrong way, and the result still looks like plausible flow -
		// so the two must be provably different, not merely both "reasonable".
		TestNotEqual(TEXT("pseudovector differs from true vector"), Pseudo, Direction);
		TestEqual(TEXT("specifically, it is the true-vector result negated"),
			Pseudo, FVector3f(-Direction.X, -Direction.Y, -Direction.Z));

		// Magnitude is preserved by both, which is exactly why a magnitude-only
		// test would be worthless here: it passes under the identity too.
		TestTrue(TEXT("both preserve magnitude"),
			FMath::IsNearlyEqual(Pseudo.Size(), Direction.Size(), UE_KINDA_SMALL_NUMBER));

		// A cross product is where the sign difference comes from. Compute the
		// curl-like quantity in solver space, then convert as a pseudovector;
		// that must equal the cross product of the two converted true vectors.
		// This is ADR 004 section 4 - derive before converting - as arithmetic.
		{
			const FVector3f A(1.0f, 2.0f, 4.0f);
			const FVector3f B(3.0f, -5.0f, 7.0f);
			const FVector3f CrossInSolver = FVector3f::CrossProduct(A, B);

			const FVector3f ConvertedCross = SolverToUnrealPseudoVector(CrossInSolver);
			const FVector3f CrossOfConverted = FVector3f::CrossProduct(
				SolverToUnrealDirection(A), SolverToUnrealDirection(B));

			TestTrue(TEXT("converting a cross product as a pseudovector matches "
				"crossing the converted vectors"),
				ConvertedCross.Equals(CrossOfConverted, UE_KINDA_SMALL_NUMBER));

			// And the naive alternative genuinely differs, so the assertion
			// above is discriminating rather than trivially true.
			TestFalse(TEXT("treating it as a true vector does not"),
				SolverToUnrealDirection(CrossInSolver).Equals(CrossOfConverted, UE_KINDA_SMALL_NUMBER));
		}
	}

	/* -- Length units: a closed table, with no silent default --------------- */
	{
		FCFDVizUnits Units;
		double Meters = -1.0;

		Units.Length = TEXT("m");
		TestTrue(TEXT("\"m\" resolves"), Units.TryGetLengthInMeters(Meters));
		TestEqual(TEXT("to 1"), Meters, 1.0);

		Units.Length = TEXT("mm");
		TestTrue(TEXT("\"mm\" resolves"), Units.TryGetLengthInMeters(Meters));
		TestEqual(TEXT("to 0.001"), Meters, 0.001);

		Units.Length = TEXT("ft");
		TestTrue(TEXT("\"ft\" resolves"), Units.TryGetLengthInMeters(Meters));
		TestEqual(TEXT("to 0.3048"), Meters, 0.3048);

		// The load-bearing case. An unrecognised unit must NOT come back as 1.0:
		// assuming metres would place a millimetre-scale case a thousand times
		// too large, and nothing on screen would say so.
		{
			const double Sentinel = -12345.0;
			double Untouched = Sentinel;
			Units.Length = TEXT("furlong");
			TestFalse(TEXT("an unknown unit is refused"), Units.TryGetLengthInMeters(Untouched));
			TestEqual(TEXT("and the output is left untouched, not defaulted to 1"),
				Untouched, Sentinel);

			// Empty is the case that arrives from a manifest missing the field.
			Untouched = Sentinel;
			Units.Length = FString();
			TestFalse(TEXT("an empty unit is refused"), Units.TryGetLengthInMeters(Untouched));
			TestEqual(TEXT("and is likewise not defaulted"), Untouched, Sentinel);
		}
	}

	/* -- sourceToCanonical is TRANSPOSED, not copied ------------------------ */
	{
		// Translation in the last COLUMN (flat 3, 7, 11), the mathematical
		// column-vector convention the manifest uses.
		const double RowMajor[16] = {
			1.0, 0.0, 0.0, 7.0,
			0.0, 1.0, 0.0, 11.0,
			0.0, 0.0, 1.0, 13.0,
			0.0, 0.0, 0.0, 1.0
		};

		FMatrix Matrix;
		if (TestTrue(TEXT("a well-formed 16-element array converts"),
			TryMakeMatrixFromRowMajorArray(RowMajor, Matrix)))
		{
			// FMatrix is row-vector: translation belongs in the last ROW.
			TestEqual(TEXT("translation X lands in the last row"), Matrix.M[3][0], 7.0);
			TestEqual(TEXT("translation Y"), Matrix.M[3][1], 11.0);
			TestEqual(TEXT("translation Z"), Matrix.M[3][2], 13.0);

			// A straight memcpy would leave them here. Distinct prime-ish values
			// above mean a partial or diagonal-only transpose still fails.
			TestEqual(TEXT("and is not left in the source column"), Matrix.M[0][3], 0.0);
			TestEqual(TEXT("nor is Y"), Matrix.M[1][3], 0.0);
			TestEqual(TEXT("nor is Z"), Matrix.M[2][3], 0.0);

			// An asymmetric off-diagonal, so the transpose is checked somewhere
			// other than the translation column. Identity rotation is symmetric
			// and would hide a half-done transpose.
			const double Asymmetric[16] = {
				1.0, 2.0, 0.0, 0.0,
				0.0, 1.0, 0.0, 0.0,
				0.0, 0.0, 1.0, 0.0,
				0.0, 0.0, 0.0, 1.0
			};
			FMatrix Rotated;
			if (TestTrue(TEXT("an asymmetric matrix converts"),
				TryMakeMatrixFromRowMajorArray(Asymmetric, Rotated)))
			{
				TestEqual(TEXT("the off-diagonal element is transposed"), Rotated.M[1][0], 2.0);
				TestEqual(TEXT("and is not left where the manifest wrote it"), Rotated.M[0][1], 0.0);
			}
		}

		// Rejections. A non-finite element would poison every position it
		// touched and surface much later as missing geometry.
		{
			FMatrix Ignored = FMatrix::Identity;

			const double TooShort[8] = { 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0 };
			TestFalse(TEXT("a short array is refused"),
				TryMakeMatrixFromRowMajorArray(TooShort, Ignored));

			double WithNaN[16] = {
				1.0, 0.0, 0.0, 0.0,
				0.0, 1.0, 0.0, 0.0,
				0.0, 0.0, 1.0, 0.0,
				0.0, 0.0, 0.0, 1.0
			};
			WithNaN[5] = std::numeric_limits<double>::quiet_NaN();
			TestFalse(TEXT("a NaN element is refused"),
				TryMakeMatrixFromRowMajorArray(WithNaN, Ignored));

			WithNaN[5] = std::numeric_limits<double>::infinity();
			TestFalse(TEXT("an infinite element is refused"),
				TryMakeMatrixFromRowMajorArray(WithNaN, Ignored));

			// The output is untouched on refusal, so a caller that ignores the
			// return value gets the identity rather than a half-built matrix.
			TestTrue(TEXT("the output matrix is not modified on refusal"),
				Ignored.Equals(FMatrix::Identity));
		}
	}

	/* -- The frame vocabulary ----------------------------------------------- */
	{
		const FCFDVizCoordinateSystem Canonical = FCFDVizCoordinateSystem::Canonical();
		const FCFDVizCoordinateSystem Unreal = FCFDVizCoordinateSystem::Unreal();

		TestTrue(TEXT("the canonical frame is canonical"), Canonical.IsCanonical());
		TestFalse(TEXT("Unreal's frame is NOT canonical"), Unreal.IsCanonical());
		TestTrue(TEXT("the two frames compare unequal"), Canonical != Unreal);

		// They differ in chirality ONLY. If a future edit changes up or forward
		// as well, the single-axis mirror in the adapter becomes wrong, and this
		// is the assertion that should catch it.
		TestEqual(TEXT("both are Z-up"), Unreal.UpAxis, Canonical.UpAxis);
		TestEqual(TEXT("both are X-forward"), Unreal.ForwardAxis, Canonical.ForwardAxis);
		TestNotEqual(TEXT("and they differ in handedness alone"),
			Unreal.Handedness, Canonical.Handedness);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
