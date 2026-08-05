// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizManifest.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * manifest.json conformance (format section 3).
 *
 * THE BASELINE MANIFEST BELOW IS THE PYTHON REFERENCE'S OWN TEST FIXTURE,
 * captured verbatim from `conftest.sample_manifest()`. That makes these tests a
 * genuine cross-implementation check rather than a restatement of this reader's
 * assumptions: every accept/reject expectation was first run through
 * `cfdviz.manifest.validate_manifest` and matches its verdict. A manifest the
 * two implementations disagree about is exactly the defect two implementations
 * exist to find, and it would show up here.
 *
 * THE MUTATION DISCIPLINE. Each rejection test takes the baseline - which is
 * asserted valid first - and breaks exactly ONE thing. That is what proves a
 * check can actually fail: a validator that returned "valid" unconditionally
 * would pass a suite built only from valid manifests, so every invariant gets a
 * paired mutant that must be refused for the stated reason.
 */

namespace
{
	/**
	 * The Python reference's sample_manifest(), byte for byte.
	 *
	 * It is deliberately not minimal - it carries a maskField cross-reference,
	 * a 3-component field, mixed codecs (zlib and none), optional `steps`, and a
	 * mesh with two patches, so the cross-reference and uniqueness checks have
	 * something real to resolve against.
	 */
	const TCHAR* const BaselineManifest = TEXT(R"JSON(
{
  "format": "CFDViz",
  "version": "1.0.0",
  "case": {
    "id": "d7f46da7-6bd8-4d4b-9410-32d1ea776328",
    "name": "Sample",
    "quality": "visualization-demo"
  },
  "units": { "length": "m", "time": "s" },
  "coordinates": { "handedness": "right", "upAxis": "Z", "forwardAxis": "X" },
  "timeline": {
    "frameCount": 2,
    "times": [0.0, 0.5],
    "steps": [0, 100]
  },
  "grids": [
    {
      "id": "main",
      "type": "uniform-cartesian",
      "dimensions": [4, 3, 2],
      "origin": [0.0, 0.0, 0.0],
      "spacing": [0.1, 0.1, 0.1],
      "maskField": "validMask"
    }
  ],
  "fields": [
    {
      "numericId": 1,
      "id": "pressure",
      "components": ["p"],
      "componentCount": 1,
      "dataType": "float32",
      "association": "cell",
      "grid": "main",
      "unit": "Pa",
      "storage": {
        "type": "bricked-volume",
        "codec": "zlib",
        "brickSize": [4, 4, 4],
        "pathPattern": "frames/{frame:06d}/pressure.cvf"
      }
    },
    {
      "numericId": 2,
      "id": "U",
      "components": ["x", "y", "z"],
      "componentCount": 3,
      "dataType": "float32",
      "association": "cell",
      "grid": "main",
      "unit": "m/s",
      "storage": {
        "type": "bricked-volume",
        "codec": "zlib",
        "brickSize": [4, 4, 4],
        "pathPattern": "frames/{frame:06d}/U.cvf"
      }
    },
    {
      "numericId": 3,
      "id": "validMask",
      "components": ["valid"],
      "componentCount": 1,
      "dataType": "uint8",
      "association": "cell",
      "grid": "main",
      "storage": {
        "type": "bricked-volume",
        "codec": "none",
        "brickSize": [4, 4, 4],
        "pathPattern": "frames/{frame:06d}/validMask.cvf"
      }
    }
  ],
  "meshes": [
    {
      "id": "obstacle",
      "name": "Obstacle",
      "path": "meshes/obstacle.cvm",
      "role": "obstacle",
      "static": true,
      "patches": [
        { "id": 10, "name": "windward", "type": "wall" },
        { "id": 20, "name": "leeward", "type": "wall" }
      ]
    }
  ]
}
)JSON");

	/** The baseline with one substring swapped - the whole mutation mechanism. */
	FString MutateManifest(const FString& From, const FString& To)
	{
		FString Text = BaselineManifest;
		check(Text.Contains(From));
		Text.ReplaceInline(*From, *To, ESearchCase::CaseSensitive);
		return Text;
	}
}

/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizManifestParseTest,
	"FlowViz.CFDViz.Manifest.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizManifestParseTest::RunTest(const FString& Parameters)
{
	FCFDVizCase Case;
	const FCFDVizResult Result = FCFDVizCase::ParseFromString(BaselineManifest, FString(), Case);

	if (!TestTrue(TEXT("the reference sample manifest parses"), Result.IsOk()))
	{
		AddError(FString::Printf(TEXT("parse failed: %s"), *Result.ToString()));
		return false;
	}

	// --- identity ---------------------------------------------------------
	TestEqual(TEXT("format marker"), Case.Format, FString(TEXT("CFDViz")));
	TestEqual(TEXT("version major"), Case.VersionMajor, 1);
	TestEqual(TEXT("version minor"), Case.VersionMinor, 0);
	TestEqual(TEXT("version patch"), Case.VersionPatch, 0);
	TestEqual(TEXT("case id"), Case.Metadata.Id, FString(TEXT("d7f46da7-6bd8-4d4b-9410-32d1ea776328")));
	TestEqual(TEXT("case name"), Case.Metadata.Name, FString(TEXT("Sample")));
	// Quality must survive parsing: it is how a UI tells a user that synthetic
	// demo data is not validation-grade CFD.
	TestEqual(TEXT("quality is carried through, not dropped"), Case.Metadata.Quality,
		FString(TEXT("visualization-demo")));

	// --- units ------------------------------------------------------------
	TestEqual(TEXT("length unit"), Case.Units.Length, FString(TEXT("m")));
	TestEqual(TEXT("time unit"), Case.Units.Time, FString(TEXT("s")));

	double MetersPerUnit = 0.0;
	TestTrue(TEXT("\"m\" resolves to a length scale"), Case.Units.TryGetLengthInMeters(MetersPerUnit));
	TestEqual(TEXT("and that scale is 1 metre per unit"), MetersPerUnit, 1.0);

	// --- coordinates ------------------------------------------------------
	TestTrue(TEXT("the declared frame is the canonical one"), Case.Coordinates.System.IsCanonical());
	TestFalse(TEXT("sourceToCanonical is unset when absent, meaning identity"),
		Case.Coordinates.SourceToCanonical.IsSet());

	// --- timeline ---------------------------------------------------------
	TestEqual(TEXT("frame count"), Case.Timeline.FrameCount, 2);
	TestEqual(TEXT("times parsed"), Case.Timeline.Times.Num(), 2);
	TestEqual(TEXT("times[0]"), Case.Timeline.Times[0], 0.0);
	TestEqual(TEXT("times[1]"), Case.Timeline.Times[1], 0.5);
	TestTrue(TEXT("steps are present"), Case.Timeline.HasSteps());

	int64 Step = -1;
	TestTrue(TEXT("step 1 readable"), Case.Timeline.TryGetStep(1, Step));
	TestEqual(TEXT("and it is 100"), Step, static_cast<int64>(100));
	TestTrue(TEXT("the timeline is strictly increasing"), Case.Timeline.IsStrictlyIncreasing());

	// Absent `defaultInterpolation` must default to Nearest, the only choice
	// that cannot display a frame the solver never produced.
	TestTrue(TEXT("absent defaultInterpolation defaults to Nearest, never Linear"),
		Case.Timeline.DefaultInterpolation == ECFDVizInterpolation::Nearest);

	// --- grid -------------------------------------------------------------
	if (TestEqual(TEXT("one grid"), Case.Grids.Num(), 1))
	{
		const FCFDVizGridDescriptor& Grid = Case.Grids[0];
		TestEqual(TEXT("grid id"), Grid.Id, FName(TEXT("main")));
		TestEqual(TEXT("grid dimensions are CELL counts"), Grid.Geometry.Dimensions, FIntVector(4, 3, 2));
		TestEqual(TEXT("grid spacing X"), Grid.Geometry.Spacing.X, 0.1);
		TestTrue(TEXT("grid geometry is self-consistent"), Grid.Geometry.IsValid());
		TestTrue(TEXT("the mask field is declared"), Grid.HasMaskField());
		TestEqual(TEXT("and it names validMask"), Grid.MaskFieldId, FName(TEXT("validMask")));
	}

	// --- fields -----------------------------------------------------------
	if (TestEqual(TEXT("three fields"), Case.Fields.Num(), 3))
	{
		const FCFDVizField* Pressure = Case.FindField(FName(TEXT("pressure")));
		if (TestNotNull(TEXT("pressure found by id"), Pressure))
		{
			TestEqual(TEXT("pressure numericId"), Pressure->NumericId, static_cast<uint32>(1));
			TestEqual(TEXT("pressure componentCount"), Pressure->ComponentCount, 1);
			TestTrue(TEXT("pressure dataType is float32"), Pressure->DataType == ECFDVizDataType::Float32);
			TestTrue(TEXT("pressure association is cell"), Pressure->Association == ECFDVizAssociation::Cell);
			TestEqual(TEXT("pressure unit"), Pressure->Unit, FString(TEXT("Pa")));
			TestTrue(TEXT("pressure codec is zlib"), Pressure->Storage.Codec == ECFDVizCodec::Zlib);
			TestTrue(TEXT("advisory brickSize is present"), Pressure->Storage.BrickSize.IsSet());
			TestEqual(TEXT("and it is 4^3"), Pressure->Storage.BrickSize.GetValue(), FIntVector(4, 4, 4));
			TestEqual(TEXT("pressure grid"), Pressure->GridId, FName(TEXT("main")));

			// An absent field-level temporalInterpolation must fall back to the
			// timeline default rather than to a hardcoded guess.
			TestFalse(TEXT("pressure declares no temporalInterpolation"),
				Pressure->TemporalInterpolation.IsSet());
			TestTrue(TEXT("so it resolves to the timeline default"),
				Pressure->ResolveTemporalInterpolation(Case.Timeline.DefaultInterpolation)
					== ECFDVizInterpolation::Nearest);
		}

		const FCFDVizField* Velocity = Case.FindField(FName(TEXT("U")));
		if (TestNotNull(TEXT("U found by id"), Velocity))
		{
			TestEqual(TEXT("U componentCount"), Velocity->ComponentCount, 3);
			TestEqual(TEXT("U component names parsed in order"), Velocity->Components.Num(), 3);
			TestEqual(TEXT("U component 0"), Velocity->Components[0], FString(TEXT("x")));
			TestEqual(TEXT("U component 2"), Velocity->Components[2], FString(TEXT("z")));
			// Case-sensitive: the Python side compares against these same
			// strings, and a looser match here would accept a manifest the
			// reference rejects.
			TestEqual(TEXT("component lookup by name"), Velocity->FindComponentIndex(TEXT("y")), 1);
			TestEqual(TEXT("component lookup is case-sensitive"),
				Velocity->FindComponentIndex(TEXT("Y")), static_cast<int32>(INDEX_NONE));
			TestEqual(TEXT("unknown component"), Velocity->FindComponentIndex(TEXT("w")),
				static_cast<int32>(INDEX_NONE));
		}

		const FCFDVizField* Mask = Case.FindField(FName(TEXT("validMask")));
		if (TestNotNull(TEXT("validMask found"), Mask))
		{
			TestTrue(TEXT("validMask dataType is uint8"), Mask->DataType == ECFDVizDataType::UInt8);
			TestTrue(TEXT("validMask codec is none"), Mask->Storage.Codec == ECFDVizCodec::None);
			// "unit absent" must mean unknown, not dimensionless-and-fine.
			TestTrue(TEXT("validMask has no declared unit"), Mask->Unit.IsEmpty());
		}

		// Numeric-id lookup exists so a .cvf can be tied to its manifest entry
		// without trusting a filename.
		const FCFDVizField* ById = Case.FindFieldByNumericId(2);
		if (TestNotNull(TEXT("field found by numericId"), ById))
		{
			TestEqual(TEXT("and it is U"), ById->Id, FName(TEXT("U")));
		}
		TestNull(TEXT("an undeclared numericId finds nothing"), Case.FindFieldByNumericId(999));
		TestNull(TEXT("an undeclared field id finds nothing"), Case.FindField(FName(TEXT("nope"))));

		// The grid a field is sampled on must resolve through the cross-reference.
		if (Pressure != nullptr)
		{
			const FCFDVizGridDescriptor* Grid = Case.FindGridForField(*Pressure);
			if (TestNotNull(TEXT("the field's grid resolves"), Grid))
			{
				TestEqual(TEXT("to main"), Grid->Id, FName(TEXT("main")));
			}
		}

		/*
		 * THE SECOND BRANCH OF THE LOOKUP CONTRACT, for the three lookups that
		 * had only their first.
		 *
		 * CFDVizManifest.h states it for all five: "@return nullptr when nothing
		 * matches. Never a default-constructed object - 'no such field' must not
		 * be indistinguishable from 'a field with no data'." Before this block,
		 * only FindField and FindFieldByNumericId had a TestNull behind that
		 * sentence. FindGrid and FindMesh had NO test at all, in either
		 * direction, and FindDerivedField had only its match case (line ~1157).
		 *
		 * The distinction is load bearing precisely because the failure is
		 * quiet: a lookup that returned a pointer to a static default would give
		 * the caller a grid with zero dimensions or a mesh with no path, and the
		 * renderer would draw an empty domain while believing the case was fine.
		 * That is the same shape as "a patch the user hid" in the mesh reader -
		 * absent data wearing the costume of present-but-empty data.
		 *
		 * Both directions per function, because a TestNull alone also passes
		 * against a function that has been broken to return nullptr always.
		 */
		const FCFDVizGridDescriptor* MainGrid = Case.FindGrid(FName(TEXT("main")));
		if (TestNotNull(TEXT("FindGrid resolves a declared grid id"), MainGrid))
		{
			TestEqual(TEXT("and it is the grid that was declared, not a default"),
				MainGrid->Geometry.Dimensions, FIntVector(4, 3, 2));
		}
		TestNull(TEXT("FindGrid returns nullptr for an undeclared grid id, never a "
					  "default-constructed grid that would render as an empty domain"),
			Case.FindGrid(FName(TEXT("nosuchgrid"))));

		const FCFDVizMesh* ObstacleMesh = Case.FindMesh(FName(TEXT("obstacle")));
		if (TestNotNull(TEXT("FindMesh resolves a declared mesh id"), ObstacleMesh))
		{
			TestEqual(TEXT("and it carries the declared path, not an empty one"),
				ObstacleMesh->Path, FString(TEXT("meshes/obstacle.cvm")));
		}
		TestNull(TEXT("FindMesh returns nullptr for an undeclared mesh id, never a "
					  "default-constructed mesh with an empty path"),
			Case.FindMesh(FName(TEXT("nosuchmesh"))));

		// FindGridForField forwards to FindGrid, so an unresolvable GridId must
		// come back null rather than as a grid the field was never sampled on.
		// Driven with a synthetic field because every field in this manifest
		// resolves - the failing input cannot be reached through the fixture.
		FCFDVizField Dangling;
		Dangling.Id = FName(TEXT("dangling"));
		Dangling.GridId = FName(TEXT("nosuchgrid"));
		TestNull(TEXT("a field naming a grid the case does not declare resolves to nullptr, "
					  "so a dangling cross-reference cannot pass as a valid sampling grid"),
			Case.FindGridForField(Dangling));
	}

	// --- mesh and patches --------------------------------------------------
	if (TestEqual(TEXT("one mesh"), Case.Meshes.Num(), 1))
	{
		const FCFDVizMesh& Mesh = Case.Meshes[0];
		TestEqual(TEXT("mesh id"), Mesh.Id, FName(TEXT("obstacle")));
		TestEqual(TEXT("mesh path"), Mesh.Path, FString(TEXT("meshes/obstacle.cvm")));
		TestTrue(TEXT("mesh is static"), Mesh.bStatic);
		TestEqual(TEXT("two patches"), Mesh.Patches.Num(), 2);

		// Patch ids are the values stored per TRIANGLE in the CVM patchIds
		// array, so they must survive as the exact uint32s written.
		const FCFDVizBoundaryPatch* Windward = Mesh.FindPatch(10);
		if (TestNotNull(TEXT("patch 10 found"), Windward))
		{
			TestEqual(TEXT("patch 10 name"), Windward->Name, FString(TEXT("windward")));
			TestEqual(TEXT("patch 10 type"), Windward->Type, FString(TEXT("wall")));
			// Absent colour must be reported as absent, so a consumer applies
			// its own palette instead of a fabricated white.
			TestFalse(TEXT("patch 10 declares no colour"), Windward->bHasColor);
			TestTrue(TEXT("and defaults to visible"), Windward->bDefaultVisible);
			TestEqual(TEXT("and to opaque"), Windward->Opacity, 1.0f);
		}
		TestNotNull(TEXT("patch 20 found"), Mesh.FindPatch(20));
		TestNull(TEXT("an undeclared patch id finds nothing"), Mesh.FindPatch(999));
	}

	// --- absent optional blocks -------------------------------------------
	TestEqual(TEXT("no derived fields declared"), Case.DerivedFields.Num(), 0);
	TestEqual(TEXT("no structures declared"), Case.Structures.Num(), 0);

	// A manifest with no zstd field must pass the codec gate.
	TestTrue(TEXT("every declared codec is decodable by this build"),
		Case.CheckCodecSupport().IsOk());

	return true;
}

/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizManifestRejectionTest,
	"FlowViz.CFDViz.Manifest.Rejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizManifestRejectionTest::RunTest(const FString& Parameters)
{
	// Every expectation below was first checked against the Python
	// validate_manifest(), so an accept/reject disagreement fails here.
	auto Parses = [this](const FString& Json, FCFDVizCase& OutCase) -> FCFDVizResult
	{
		return FCFDVizCase::ParseFromString(Json, FString(), OutCase);
	};

	FCFDVizCase Case;

	// The control. If this ever fails, every rejection below is meaningless.
	TestTrue(TEXT("the unmutated baseline is valid"), Parses(BaselineManifest, Case).IsOk());

	// --- format marker -----------------------------------------------------
	{
		const FString Json = MutateManifest(TEXT("\"format\": \"CFDViz\""), TEXT("\"format\": \"NotCFDViz\""));
		const FCFDVizResult Result = Parses(Json, Case);
		TestFalse(TEXT("a wrong format marker is rejected"), Result.IsOk());
	}

	// --- version policy (format rule 1.4) ----------------------------------
	{
		const FString Json = MutateManifest(TEXT("\"version\": \"1.0.0\""), TEXT("\"version\": \"2.0.0\""));
		const FCFDVizResult Result = Parses(Json, Case);
		TestFalse(TEXT("a different MAJOR version is rejected"), Result.IsOk());
		TestTrue(TEXT("and it is reported as a version problem"),
			Result.Error == ECFDVizError::UnsupportedVersion);
	}
	{
		// A newer MINOR must be ACCEPTED. This is the direction that is easy to
		// get wrong by treating any version difference as fatal, which would
		// make every 1.x case unreadable the day 1.1 ships.
		const FString Json = MutateManifest(TEXT("\"version\": \"1.0.0\""), TEXT("\"version\": \"1.7.0\""));
		FCFDVizCase Newer;
		const FCFDVizResult Result = Parses(Json, Newer);
		TestTrue(TEXT("a newer MINOR version is accepted, not rejected"), Result.IsOk());
		TestEqual(TEXT("and its minor is preserved"), Newer.VersionMinor, 7);
	}
	{
		const FString Json = MutateManifest(TEXT("\"version\": \"1.0.0\""), TEXT("\"version\": \"banana\""));
		TestFalse(TEXT("a non-semver version is rejected"), Parses(Json, Case).IsOk());
	}

	// Unknown properties are ignored at every level (format rule 1.4), so a 1.1
	// manifest carrying new keys still loads here.
	{
		const FString Json = MutateManifest(
			TEXT("\"units\": { \"length\": \"m\", \"time\": \"s\" }"),
			TEXT("\"units\": { \"length\": \"m\", \"time\": \"s\", \"somethingNew\": 5 }"));
		TestTrue(TEXT("an unknown property is ignored, not rejected"), Parses(Json, Case).IsOk());
	}

	// --- required properties ------------------------------------------------
	{
		const FString Json = MutateManifest(TEXT("\"length\": \"m\", \"time\": \"s\""), TEXT("\"length\": \"m\""));
		const FCFDVizResult Result = Parses(Json, Case);
		TestFalse(TEXT("a missing required units.time is rejected"), Result.IsOk());
		// The message must name the property, or a user cannot act on it.
		TestTrue(TEXT("and the message names the missing property"),
			Result.Message.Contains(TEXT("time")));
	}
	{
		const FString Json = MutateManifest(TEXT("\"id\": \"main\","), TEXT("\"name\": \"main\","));
		TestFalse(TEXT("a grid with no id is rejected"), Parses(Json, Case).IsOk());
	}

	// --- section 3.3: float64 is not CVF storage ---------------------------
	{
		const FString Json = MutateManifest(
			TEXT("\"dataType\": \"float32\",\n      \"association\": \"cell\",\n      \"grid\": \"main\",\n      \"unit\": \"Pa\""),
			TEXT("\"dataType\": \"float64\",\n      \"association\": \"cell\",\n      \"grid\": \"main\",\n      \"unit\": \"Pa\""));
		const FCFDVizResult Result = Parses(Json, Case);
		TestFalse(TEXT("float64 field storage is rejected (section 3.3)"), Result.IsOk());
		// float64 parses as a dataType precisely so this message can be
		// specific rather than "unknown dataType".
		TestTrue(TEXT("and the message explains float64 specifically"),
			Result.Message.Contains(TEXT("float64")));
	}

	// --- section 3.2: reserved associations ---------------------------------
	{
		const FString Json = MutateManifest(TEXT("\"association\": \"cell\",\n      \"grid\": \"main\",\n      \"unit\": \"Pa\""),
			TEXT("\"association\": \"mesh-vertex\",\n      \"grid\": \"main\",\n      \"unit\": \"Pa\""));
		const FCFDVizResult Result = Parses(Json, Case);
		TestFalse(TEXT("a reserved association is rejected in 1.0"), Result.IsOk());
		TestTrue(TEXT("and is described as reserved, not merely unknown"),
			Result.Message.Contains(TEXT("reserved")));
	}

	// --- section 3.1 invariants ---------------------------------------------
	{
		const FString Json = MutateManifest(TEXT("\"components\": [\"x\", \"y\", \"z\"],\n      \"componentCount\": 3"),
			TEXT("\"components\": [\"x\", \"y\", \"z\"],\n      \"componentCount\": 2"));
		const FCFDVizResult Result = Parses(Json, Case);
		TestFalse(TEXT("componentCount disagreeing with components is rejected"), Result.IsOk());
		TestTrue(TEXT("and the message names the field"), Result.Message.Contains(TEXT("U")));
	}
	{
		const FString Json = MutateManifest(TEXT("\"times\": [0.0, 0.5]"), TEXT("\"times\": [0.5, 0.0]"));
		TestFalse(TEXT("a non-increasing timeline is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"times\": [0.0, 0.5]"), TEXT("\"times\": [0.0, 0.0]"));
		TestFalse(TEXT("a repeated time is rejected: increasing means STRICTLY"),
			Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"frameCount\": 2"), TEXT("\"frameCount\": 5"));
		TestFalse(TEXT("frameCount disagreeing with times is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"steps\": [0, 100]"), TEXT("\"steps\": [0, 100, 200]"));
		TestFalse(TEXT("steps of the wrong length is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"grid\": \"main\",\n      \"unit\": \"Pa\""),
			TEXT("\"grid\": \"nope\",\n      \"unit\": \"Pa\""));
		const FCFDVizResult Result = Parses(Json, Case);
		TestFalse(TEXT("a field referencing an undeclared grid is rejected"), Result.IsOk());
		TestTrue(TEXT("and the message names the dangling reference"),
			Result.Message.Contains(TEXT("nope")));
	}
	{
		const FString Json = MutateManifest(TEXT("\"maskField\": \"validMask\""), TEXT("\"maskField\": \"nope\""));
		TestFalse(TEXT("a maskField naming no declared field is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"id\": \"U\","), TEXT("\"id\": \"pressure\","));
		const FCFDVizResult Result = Parses(Json, Case);
		TestFalse(TEXT("a duplicate field id is rejected"), Result.IsOk());
		TestTrue(TEXT("and is described as a duplicate"),
			Result.Message.Contains(TEXT("duplicate")) || Result.Message.Contains(TEXT("Duplicate")));
	}
	{
		const FString Json = MutateManifest(TEXT("\"numericId\": 2,"), TEXT("\"numericId\": 1,"));
		TestFalse(TEXT("a duplicate field numericId is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("{ \"id\": 20, \"name\": \"leeward\", \"type\": \"wall\" }"),
			TEXT("{ \"id\": 10, \"name\": \"leeward\", \"type\": \"wall\" }"));
		TestFalse(TEXT("a duplicate patch id within a mesh is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"dimensions\": [4, 3, 2]"), TEXT("\"dimensions\": [4, 0, 2]"));
		TestFalse(TEXT("a zero grid dimension is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"spacing\": [0.1, 0.1, 0.1]"), TEXT("\"spacing\": [0.1, 0.0, 0.1]"));
		TestFalse(TEXT("a zero grid spacing is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"spacing\": [0.1, 0.1, 0.1]"), TEXT("\"spacing\": [0.1, -0.1, 0.1]"));
		TestFalse(TEXT("a negative grid spacing is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"type\": \"uniform-cartesian\""), TEXT("\"type\": \"curvilinear\""));
		TestFalse(TEXT("an unknown grid type is rejected, not ignored"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"handedness\": \"right\""), TEXT("\"handedness\": \"left\""));
		TestFalse(TEXT("a non-canonical handedness is rejected"), Parses(Json, Case).IsOk());
	}
	{
		const FString Json = MutateManifest(TEXT("\"upAxis\": \"Z\""), TEXT("\"upAxis\": \"Y\""));
		TestFalse(TEXT("a non-canonical up axis is rejected"), Parses(Json, Case).IsOk());
	}

	// --- section 1.3: path traversal ----------------------------------------
	{
		const FString Json = MutateManifest(TEXT("\"pathPattern\": \"frames/{frame:06d}/pressure.cvf\""),
			TEXT("\"pathPattern\": \"../../etc/passwd\""));
		const FCFDVizResult Result = Parses(Json, Case);
		TestFalse(TEXT("a traversing pathPattern is rejected"), Result.IsOk());
		TestTrue(TEXT("and is reported as traversal"), Result.Error == ECFDVizError::PathTraversal);
	}
	{
		const FString Json = MutateManifest(TEXT("\"path\": \"meshes/obstacle.cvm\""),
			TEXT("\"path\": \"/etc/passwd\""));
		TestFalse(TEXT("an absolute mesh path is rejected"), Parses(Json, Case).IsOk());
	}

	// --- malformed input ----------------------------------------------------
	{
		FCFDVizCase Empty;
		TestFalse(TEXT("empty text is rejected"), Parses(TEXT(""), Empty).IsOk());
		TestFalse(TEXT("truncated JSON is rejected"), Parses(TEXT("{\"format\": \"CFDViz\""), Empty).IsOk());
		TestFalse(TEXT("a JSON array is not a manifest"), Parses(TEXT("[1,2,3]"), Empty).IsOk());
		TestFalse(TEXT("a JSON scalar is not a manifest"), Parses(TEXT("42"), Empty).IsOk());
	}

	// OutCase must be untouched on failure, so a caller that ignores the result
	// cannot end up rendering a half-populated case.
	{
		FCFDVizCase Loaded;
		TestTrue(TEXT("a good manifest loads"), Parses(BaselineManifest, Loaded).IsOk());
		const int32 FieldCountBefore = Loaded.Fields.Num();

		const FString Json = MutateManifest(TEXT("\"format\": \"CFDViz\""), TEXT("\"format\": \"Nope\""));
		TestFalse(TEXT("then a bad one fails"), Parses(Json, Loaded).IsOk());
		TestEqual(TEXT("and leaves the previously loaded case untouched"),
			Loaded.Fields.Num(), FieldCountBefore);
	}

	return true;
}

/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizManifestPathTest,
	"FlowViz.CFDViz.Manifest.Paths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizManifestPathTest::RunTest(const FString& Parameters)
{
	// --- the lexical traversal check (format rule 1.3) ---------------------
	// Each expectation matches cfdviz.manifest.is_safe_relative_path exactly;
	// the last two are the ones a naive implementation gets wrong.
	TestTrue(TEXT("a normal relative path is safe"),
		FCFDVizCase::IsSafeRelativePath(TEXT("frames/000000/U.cvf")));
	TestTrue(TEXT("a pattern with a placeholder is safe"),
		FCFDVizCase::IsSafeRelativePath(TEXT("frames/{frame:06d}/U.cvf")));
	TestFalse(TEXT("a leading slash is absolute"), FCFDVizCase::IsSafeRelativePath(TEXT("/etc/passwd")));
	TestFalse(TEXT("a drive letter is absolute on Windows"),
		FCFDVizCase::IsSafeRelativePath(TEXT("C:/Windows/system32")));
	TestFalse(TEXT("a backslash separates on Windows"),
		FCFDVizCase::IsSafeRelativePath(TEXT("..\\secrets")));
	TestFalse(TEXT("a leading .. segment escapes"), FCFDVizCase::IsSafeRelativePath(TEXT("../x")));
	TestFalse(TEXT("an interior .. segment escapes"), FCFDVizCase::IsSafeRelativePath(TEXT("a/../b")));
	TestFalse(TEXT("a trailing .. segment escapes"), FCFDVizCase::IsSafeRelativePath(TEXT("a/..")));
	TestFalse(TEXT("an empty path is not a path"), FCFDVizCase::IsSafeRelativePath(TEXT("")));
	TestFalse(TEXT("a control character is refused"),
		FCFDVizCase::IsSafeRelativePath(TEXT("a\tb/c.cvf")));

	// These two must be ACCEPTED. ".." is only dangerous as a whole segment;
	// rejecting the substring would refuse legitimate filenames that the Python
	// reference accepts, and the two readers would disagree about a valid case.
	TestTrue(TEXT("\"..hidden\" is an ordinary filename, not a traversal"),
		FCFDVizCase::IsSafeRelativePath(TEXT("..hidden/x")));
	TestTrue(TEXT("\"a..b\" is an ordinary filename"),
		FCFDVizCase::IsSafeRelativePath(TEXT("a..b/c.cvf")));

	// --- frame path expansion, matching Python str.format ------------------
	{
		FString Expanded;
		FCFDVizResult Result = FCFDVizCase::FormatFramePath(TEXT("frames/{frame:06d}/U.cvf"), 42, Expanded);
		TestTrue(TEXT("a zero-padded pattern expands"), Result.IsOk());
		TestEqual(TEXT("exactly as Python's str.format would"), Expanded,
			FString(TEXT("frames/000042/U.cvf")));

		Result = FCFDVizCase::FormatFramePath(TEXT("f/{frame}.cvf"), 7, Expanded);
		TestTrue(TEXT("a bare {frame} expands"), Result.IsOk());
		TestEqual(TEXT("without padding"), Expanded, FString(TEXT("f/7.cvf")));

		Result = FCFDVizCase::FormatFramePath(TEXT("f/{frame:03d}.cvf"), 12345, Expanded);
		TestTrue(TEXT("a value wider than its field expands"), Result.IsOk());
		TestEqual(TEXT("and is not truncated to the field width"), Expanded,
			FString(TEXT("f/12345.cvf")));

		// Literal braces, the `{{`/`}}` escape.
		Result = FCFDVizCase::FormatFramePath(TEXT("f/{{literal}}/{frame}.cvf"), 3, Expanded);
		TestTrue(TEXT("escaped braces expand"), Result.IsOk());
		TestEqual(TEXT("to single literal braces"), Expanded, FString(TEXT("f/{literal}/3.cvf")));

		// A pattern with no placeholder is legal - every frame would resolve to
		// the same file, which is the manifest's business, not the formatter's.
		Result = FCFDVizCase::FormatFramePath(TEXT("static.cvf"), 3, Expanded);
		TestTrue(TEXT("a pattern with no placeholder expands to itself"), Result.IsOk());
		TestEqual(TEXT("unchanged"), Expanded, FString(TEXT("static.cvf")));
	}
	{
		// Rejected rather than approximated: a silently mis-expanded path fails
		// later as a confusing "file not found".
		FString Expanded;
		TestFalse(TEXT("an unknown placeholder name is rejected"),
			FCFDVizCase::FormatFramePath(TEXT("f/{step}.cvf"), 1, Expanded).IsOk());
		TestFalse(TEXT("an unsupported format spec is rejected"),
			FCFDVizCase::FormatFramePath(TEXT("f/{frame:x}.cvf"), 1, Expanded).IsOk());
		TestFalse(TEXT("an unclosed brace is rejected"),
			FCFDVizCase::FormatFramePath(TEXT("f/{frame.cvf"), 1, Expanded).IsOk());
		TestFalse(TEXT("a stray closing brace is rejected"),
			FCFDVizCase::FormatFramePath(TEXT("f/frame}.cvf"), 1, Expanded).IsOk());
		TestFalse(TEXT("a negative frame index is rejected"),
			FCFDVizCase::FormatFramePath(TEXT("f/{frame}.cvf"), -1, Expanded).IsOk());
		// Bounds the string a hostile pattern can make this reader build.
		TestFalse(TEXT("an absurd field width is rejected"),
			FCFDVizCase::FormatFramePath(TEXT("f/{frame:0999d}.cvf"), 1, Expanded).IsOk());
	}

	// --- resolution against a case root -------------------------------------
	{
		FCFDVizCase Case;
		TestTrue(TEXT("baseline parses"), FCFDVizCase::ParseFromString(BaselineManifest, FString(), Case).IsOk());

		// With no context path the root is unknown, and resolution must fail
		// with a specific error rather than silently resolving against the
		// process working directory.
		FString Absolute;
		const FCFDVizResult Result = Case.ResolveRelativePath(TEXT("meshes/obstacle.cvm"), Absolute);
		TestFalse(TEXT("resolution fails when the case root is unknown"), Result.IsOk());
		TestTrue(TEXT("and says so rather than guessing at the working directory"),
			Result.Error == ECFDVizError::InvalidManifest);
	}
	{
		// Now with a context path, so CaseRootDir is known.
		const FString ContextPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CFDVizTest"), TEXT("manifest.json"));
		FCFDVizCase Case;
		if (TestTrue(TEXT("baseline parses with a context path"),
			FCFDVizCase::ParseFromString(BaselineManifest, ContextPath, Case).IsOk()))
		{
			TestFalse(TEXT("the case root is derived from the context path"), Case.CaseRootDir.IsEmpty());

			FString Absolute;
			TestTrue(TEXT("a mesh path resolves"),
				Case.ResolveMeshPath(FName(TEXT("obstacle")), Absolute).IsOk());
			TestTrue(TEXT("to a path under the case root"),
				Absolute.Contains(TEXT("obstacle.cvm")));

			// The per-frame path is the one that matters for loading data.
			TestTrue(TEXT("a field frame path resolves"),
				Case.ResolveFieldFramePath(FName(TEXT("U")), 1, Absolute).IsOk());
			TestTrue(TEXT("and carries the zero-padded frame index"),
				Absolute.Contains(TEXT("000001")));

			// Bounds-checked against frameCount, so a caller cannot silently
			// build a path to a frame the case does not contain.
			TestFalse(TEXT("a frame past the end is rejected"),
				Case.ResolveFieldFramePath(FName(TEXT("U")), 2, Absolute).IsOk());
			TestFalse(TEXT("a negative frame is rejected"),
				Case.ResolveFieldFramePath(FName(TEXT("U")), -1, Absolute).IsOk());
			TestFalse(TEXT("an unknown field id is rejected"),
				Case.ResolveFieldFramePath(FName(TEXT("nope")), 0, Absolute).IsOk());
			TestFalse(TEXT("an unknown mesh id is rejected"),
				Case.ResolveMeshPath(FName(TEXT("nope")), Absolute).IsOk());

			// The traversal check applies at resolution time too, not only at
			// parse time - this is the layer that would catch a future change
			// to either half.
			TestFalse(TEXT("resolution refuses a traversing path"),
				Case.ResolveRelativePath(TEXT("../../etc/passwd"), Absolute).IsOk());
		}
	}

	// --- containment: the SIBLING-PREFIX escape -----------------------------
	//
	// ResolveRelativePath ends in a containment test on the resolved absolute
	// path. The bug it exists to stop is not "../" - the lexical check above
	// already refuses every ".." segment - but a sibling directory whose name
	// begins with the case root's name. With root ".../CFDVizContainment/run",
	// the path ".../CFDVizContainment/run-evil/data.cvf" literally starts with
	// the root string, so a StartsWith prefix compare reports it contained.
	// Only a separator-aware test (FPaths::IsUnderDirectory) rejects it.
	//
	// The sibling name must share the root's EXACT prefix ("run" -> "run-evil").
	// A differently-named sibling is refused by StartsWith too and would prove
	// nothing about which test is in use.
	{
		const FString ContainmentDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CFDVizContainment"));
		const FString CaseRoot = FPaths::Combine(ContainmentDir, TEXT("run"));
		const FString ContextPath = FPaths::Combine(CaseRoot, TEXT("manifest.json"));

		FCFDVizCase Case;
		if (TestTrue(TEXT("baseline parses under the containment root"),
			FCFDVizCase::ParseFromString(BaselineManifest, ContextPath, Case).IsOk()))
		{
			// CONTROL. Without a path that must SUCCEED, an implementation that
			// rejected everything would look identical to a correct one.
			FString Legitimate;
			const FCFDVizResult LegitimateResult =
				Case.ResolveRelativePath(TEXT("fields/x.cvf"), Legitimate);
			if (!TestTrue(TEXT("a legitimate path under the case root still resolves"),
				LegitimateResult.IsOk()))
			{
				AddError(FString::Printf(TEXT("control case failed: %s"), *LegitimateResult.ToString()));
			}
			TestTrue(TEXT("and lands inside the case root"),
				FPaths::IsUnderDirectory(Legitimate, CaseRoot));

			// Build the sibling path by absolute construction rather than by a
			// relative string: every relative spelling of it needs a ".."
			// segment, which the lexical check rejects first, so the
			// containment line would never be reached and the test would pass
			// for the wrong reason.
			const FString SiblingRoot = CaseRoot + TEXT("-evil");
			const FString SiblingFile = FPaths::Combine(SiblingRoot, TEXT("data.cvf"));

			// The precise defect: a prefix compare says "contained", the
			// separator-aware test says "outside". Asserting both pins which
			// one the reader must behave like.
			TestTrue(TEXT("the sibling path does share the case root's string prefix"),
				SiblingFile.StartsWith(CaseRoot));
			TestFalse(TEXT("but it is NOT under the case root"),
				FPaths::IsUnderDirectory(SiblingFile, CaseRoot));

			// Through the reader's own entry point the only relative spelling of
			// the sibling is "../run-evil/data.cvf", and that is refused by the
			// LEXICAL check on the ".." segment, before the containment test is
			// reached. It is asserted here for the behaviour, not as evidence
			// about which containment test is in use - it is refused either way.
			FString Escaped;
			const FCFDVizResult SiblingResult =
				Case.ResolveRelativePath(TEXT("../run-evil/data.cvf"), Escaped);
			TestFalse(TEXT("the relative spelling of the sibling escape is refused"),
				SiblingResult.IsOk());
			TestTrue(TEXT("as a path traversal"),
				SiblingResult.Error == ECFDVizError::PathTraversal);
		}
	}

	return true;
}

/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizManifestCodecTest,
	"FlowViz.CFDViz.Manifest.CodecSupport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizManifestCodecTest::RunTest(const FString& Parameters)
{
	// Section 7: zstd is RESERVED. A zstd manifest is well-formed - it is this
	// build that cannot decode it - so it must PARSE and then be refused by
	// CheckCodecSupport with the exact mandated wording. Rejecting it at parse
	// time would be wrong in the other direction: the message would be "invalid
	// manifest", which misdescribes a valid file.
	const FString Json = MutateManifest(TEXT("\"codec\": \"zlib\",\n        \"brickSize\": [4, 4, 4],\n        \"pathPattern\": \"frames/{frame:06d}/pressure.cvf\""),
		TEXT("\"codec\": \"zstd\",\n        \"brickSize\": [4, 4, 4],\n        \"pathPattern\": \"frames/{frame:06d}/pressure.cvf\""));

	FCFDVizCase Case;
	const FCFDVizResult ParseResult = FCFDVizCase::ParseFromString(Json, FString(), Case);
	if (!TestTrue(TEXT("a zstd manifest is VALID and parses"), ParseResult.IsOk()))
	{
		AddError(FString::Printf(TEXT("parse failed: %s"), *ParseResult.ToString()));
		return false;
	}

	const FCFDVizField* Pressure = Case.FindField(FName(TEXT("pressure")));
	if (TestNotNull(TEXT("the zstd field parsed"), Pressure))
	{
		TestTrue(TEXT("and its codec is recorded as zstd"), Pressure->Storage.Codec == ECFDVizCodec::Zstd);
	}

	const FCFDVizResult CodecResult = Case.CheckCodecSupport();
	TestFalse(TEXT("but this build cannot decode it"), CodecResult.IsOk());
	TestTrue(TEXT("and it is reported as an unsupported codec"),
		CodecResult.Error == ECFDVizError::UnsupportedCodec);
	// The wording is normative (section 7) and is shared with the Python
	// reference, so it is compared literally rather than by substring.
	TestEqual(TEXT("with exactly the wording section 7 mandates"),
		CodecResult.Message, FString(CFDViz::ZstdRejectionMessage));
	// The message must name the field, or a user cannot tell which one to re-encode.
	TestTrue(TEXT("and the failure names the offending field"),
		CodecResult.FilePath.Contains(TEXT("pressure"))
			|| CodecResult.Message.Contains(TEXT("pressure")));

	// The unmutated baseline uses zlib and none, both decodable.
	FCFDVizCase Baseline;
	TestTrue(TEXT("baseline parses"), FCFDVizCase::ParseFromString(BaselineManifest, FString(), Baseline).IsOk());
	TestTrue(TEXT("and every codec it declares is supported"), Baseline.CheckCodecSupport().IsOk());

	return true;
}

/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizManifestFileTest,
	"FlowViz.CFDViz.Manifest.LoadFromFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizManifestFileTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CFDVizManifestTest"));
	const FString Path = FPaths::Combine(Dir, TEXT("manifest.json"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);

	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/true);
	};

	if (!TestTrue(TEXT("the fixture manifest is written"),
		FFileHelper::SaveStringToFile(FString(BaselineManifest), *Path)))
	{
		return false;
	}

	FCFDVizCase Case;
	const FCFDVizResult Result = FCFDVizCase::LoadFromFile(Path, Case);
	if (!TestTrue(TEXT("it loads from disk"), Result.IsOk()))
	{
		AddError(FString::Printf(TEXT("load failed: %s"), *Result.ToString()));
		return false;
	}

	TestEqual(TEXT("three fields"), Case.Fields.Num(), 3);
	TestFalse(TEXT("the manifest path is recorded"), Case.ManifestPath.IsEmpty());
	// The case root is what every relative path resolves against, so it must be
	// the manifest's DIRECTORY, not the manifest itself.
	TestFalse(TEXT("the case root is recorded"), Case.CaseRootDir.IsEmpty());
	TestFalse(TEXT("and it is the containing directory, not the file"),
		Case.CaseRootDir.EndsWith(TEXT("manifest.json")));

	// Paths must resolve against the real directory now that the root is known.
	FString Absolute;
	if (TestTrue(TEXT("a frame path resolves"),
		Case.ResolveFieldFramePath(FName(TEXT("pressure")), 0, Absolute).IsOk()))
	{
		TestTrue(TEXT("under the case root"), Absolute.StartsWith(Case.CaseRootDir));
		TestTrue(TEXT("with the expanded frame index"), Absolute.Contains(TEXT("000000")));
	}

	// A missing file must fail with FileNotFound rather than an empty case that
	// looks merely uninteresting.
	FCFDVizCase Missing;
	const FCFDVizResult MissingResult =
		FCFDVizCase::LoadFromFile(FPaths::Combine(Dir, TEXT("absent.json")), Missing);
	TestFalse(TEXT("a missing manifest fails"), MissingResult.IsOk());
	TestTrue(TEXT("and says the file was not found"),
		MissingResult.Error == ECFDVizError::FileNotFound);

	// Malformed JSON on disk must name the file, or the user cannot tell which
	// of a case's files is broken.
	const FString BadPath = FPaths::Combine(Dir, TEXT("bad.json"));
	if (TestTrue(TEXT("a malformed manifest is written"),
		FFileHelper::SaveStringToFile(FString(TEXT("{ not json")), *BadPath)))
	{
		FCFDVizCase Bad;
		const FCFDVizResult BadResult = FCFDVizCase::LoadFromFile(BadPath, Bad);
		TestFalse(TEXT("a malformed manifest fails"), BadResult.IsOk());
		TestFalse(TEXT("and the failure names the file"), BadResult.FilePath.IsEmpty());
	}

	// BYTE ORDER MARKS. Section 1.2 requires UTF-8 with no BOM, and the Python
	// reference rejects a BOM by name. A reader that quietly strips one - which
	// FFileHelper::LoadFileToString does, and which also decodes UTF-16 - would
	// accept a whole class of file the reference implementation refuses, and the
	// two readers would disagree about what a valid case is.
	//
	// The bytes are written directly rather than through SaveStringToFile,
	// because its AutoDetect encoding writes no BOM for pure-ASCII content: a
	// test built that way would assert nothing.
	{
		const FString Utf8Ascii = BaselineManifest;
		TArray<uint8> Ascii;
		Ascii.Reserve(Utf8Ascii.Len());
		for (int32 Index = 0; Index < Utf8Ascii.Len(); ++Index)
		{
			Ascii.Add(static_cast<uint8>(Utf8Ascii[Index]));
		}

		// The same bytes with NO BOM must load, so the rejections below are
		// attributable to the BOM alone and not to the fixture.
		const FString CleanPath = FPaths::Combine(Dir, TEXT("clean.json"));
		if (TestTrue(TEXT("a BOM-free manifest is written"),
			FFileHelper::SaveArrayToFile(Ascii, *CleanPath)))
		{
			FCFDVizCase Clean;
			const FCFDVizResult CleanResult = FCFDVizCase::LoadFromFile(CleanPath, Clean);
			if (!TestTrue(TEXT("the same bytes without a BOM load"), CleanResult.IsOk()))
			{
				AddError(FString::Printf(TEXT("control case failed: %s"), *CleanResult.ToString()));
			}
		}

		struct FBomCase
		{
			const TCHAR* Name;
			const TCHAR* FileName;
			TArray<uint8> Prefix;
		};
		const TArray<FBomCase> BomCases = {
			{ TEXT("UTF-8"), TEXT("bom_utf8.json"), { 0xEF, 0xBB, 0xBF } },
			{ TEXT("UTF-16 LE"), TEXT("bom_utf16le.json"), { 0xFF, 0xFE } },
			{ TEXT("UTF-16 BE"), TEXT("bom_utf16be.json"), { 0xFE, 0xFF } },
		};

		for (const FBomCase& BomCase : BomCases)
		{
			TArray<uint8> Bytes = BomCase.Prefix;
			Bytes.Append(Ascii);

			const FString BomPath = FPaths::Combine(Dir, BomCase.FileName);
			if (!TestTrue(TEXT("the BOM fixture is written"), FFileHelper::SaveArrayToFile(Bytes, *BomPath)))
			{
				continue;
			}

			FCFDVizCase BomCaseData;
			const FCFDVizResult BomResult = FCFDVizCase::LoadFromFile(BomPath, BomCaseData);
			TestFalse(FString::Printf(TEXT("a %s BOM is rejected"), BomCase.Name), BomResult.IsOk());
			// Offset 0 points at the BOM itself, which is the only thing wrong
			// with the file - a later offset would send the user hunting through
			// valid JSON.
			TestEqual(FString::Printf(TEXT("and the %s failure points at byte 0"), BomCase.Name),
				BomResult.ByteOffset, static_cast<int64>(0));
		}
	}

	return true;
}

/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizManifestStatisticsTest,
	"FlowViz.CFDViz.Manifest.Statistics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizManifestStatisticsTest::RunTest(const FString& Parameters)
{
	// A field carrying a full, well-formed statistics block.
	{
		const FString Json = MutateManifest(TEXT("\"unit\": \"m/s\","),
			TEXT("\"unit\": \"m/s\",\n      \"statistics\": {\n")
			TEXT("        \"globalComponentMin\": [-1.0, -2.0, -3.0],\n")
			TEXT("        \"globalComponentMax\": [1.0, 2.0, 3.0],\n")
			TEXT("        \"globalMagnitudeMin\": 0.0,\n")
			TEXT("        \"globalMagnitudeMax\": 3.75\n      },"));

		FCFDVizCase Case;
		const FCFDVizResult Result = FCFDVizCase::ParseFromString(Json, FString(), Case);
		if (!TestTrue(TEXT("a manifest with statistics parses"), Result.IsOk()))
		{
			AddError(FString::Printf(TEXT("parse failed: %s"), *Result.ToString()));
			return false;
		}

		const FCFDVizField* Velocity = Case.FindField(FName(TEXT("U")));
		if (TestNotNull(TEXT("U parsed"), Velocity))
		{
			const FCFDVizFieldStatistics& Stats = Velocity->Statistics;
			TestFalse(TEXT("the statistics block is not empty"), Stats.IsEmpty());
			TestTrue(TEXT("a component range was declared"), Stats.bHasComponentRange);
			TestTrue(TEXT("a magnitude range was declared"), Stats.bHasMagnitudeRange);
			TestEqual(TEXT("component minima parsed in order"), Stats.GlobalComponentMin.Num(), 3);
			TestEqual(TEXT("min[1]"), Stats.GlobalComponentMin[1], -2.0);
			TestEqual(TEXT("max[2]"), Stats.GlobalComponentMax[2], 3.0);
			TestEqual(TEXT("magnitude max"), Stats.GlobalMagnitudeMax, 3.75);

			FCFDVizStatistics Converted;
			TestTrue(TEXT("statistics convert to the shared type"),
				Stats.TryMakeStatistics(3, Converted));
			// bValid is what every consumer of FCFDVizStatistics branches on.
			// A converted statistic that leaves it false is silently discarded
			// by callers that early-out on it, so the range the manifest
			// declared never reaches the viewer - a wrong answer with no error.
			TestTrue(TEXT("and are flagged valid, or every bValid-guarded caller drops them"),
				Converted.bValid);
			TestEqual(TEXT("carrying the component minima"), Converted.ComponentMin.Num(), 3);
			TestEqual(TEXT("value preserved"), Converted.ComponentMin[0], -1.0);
			TestEqual(TEXT("magnitude minimum carried across"), Converted.MagnitudeMin, 0.0);
			TestEqual(TEXT("magnitude maximum carried across"), Converted.MagnitudeMax, 3.75);
			// The manifest does not record counts, so they must stay zero
			// rather than being invented - a caller must not derive a mean or a
			// NaN fraction from a manifest-sourced statistic.
			TestEqual(TEXT("NaN count is not invented"), Converted.NaNCount, static_cast<int64>(0));
			TestEqual(TEXT("valid count is not invented"), Converted.ValidCount, static_cast<int64>(0));

			// A component count that disagrees must refuse, not silently pad or
			// truncate the arrays.
			FCFDVizStatistics Wrong;
			// Pre-marked valid so the refusal is provably a refusal: if
			// TryMakeStatistics returns false it must also leave the output
			// untouched, or a caller that checks bValid instead of the return
			// value reads a statistic that was never built.
			Wrong.bValid = true;
			Wrong.ComponentMin.Add(99.0);
			TestFalse(TEXT("a disagreeing component count is refused"),
				Stats.TryMakeStatistics(2, Wrong));
			TestEqual(TEXT("and a refused conversion writes nothing"),
				Wrong.ComponentMin.Num(), 1);
			TestEqual(TEXT("leaving the caller's value intact"), Wrong.ComponentMin[0], 99.0);
		}
	}

	// A HALF-DECLARED MAGNITUDE range: a minimum with no maximum. Same rule as
	// the component arrays - kept verbatim, flagged unusable. Asserted
	// separately because bHasMagnitudeRange and bHasComponentRange are
	// independent flags, and a suite that only checks the component side passes
	// when the magnitude test is loosened from AND to OR.
	{
		const FString Json = MutateManifest(TEXT("\"unit\": \"m/s\","),
			TEXT("\"unit\": \"m/s\",\n      \"statistics\": {\n")
			TEXT("        \"globalComponentMin\": [-1.0, -2.0, -3.0],\n")
			TEXT("        \"globalComponentMax\": [1.0, 2.0, 3.0],\n")
			TEXT("        \"globalMagnitudeMin\": 0.5\n      },"));

		FCFDVizCase Case;
		if (TestTrue(TEXT("a half-declared magnitude range still parses"),
			FCFDVizCase::ParseFromString(Json, FString(), Case).IsOk()))
		{
			const FCFDVizField* Velocity = Case.FindField(FName(TEXT("U")));
			if (TestNotNull(TEXT("U parsed"), Velocity))
			{
				TestTrue(TEXT("the component range is still usable"),
					Velocity->Statistics.bHasComponentRange);
				TestFalse(TEXT("but a minimum with no maximum is not a magnitude range"),
					Velocity->Statistics.bHasMagnitudeRange);

				// The declared bound is kept; the ABSENT one must not read as a
				// number. Reading GlobalMagnitudeMax here would be reading a
				// default the manifest never stated.
				TestEqual(TEXT("the declared bound is kept verbatim"),
					Velocity->Statistics.GlobalMagnitudeMin, 0.5);

				FCFDVizStatistics Converted;
				TestTrue(TEXT("the component range still converts"),
					Velocity->Statistics.TryMakeStatistics(3, Converted));
				double MagMin = 0.0;
				double MagMax = 0.0;
				TestFalse(TEXT("without offering a magnitude range"),
					Converted.TryGetMagnitudeRange(MagMin, MagMax));
			}
		}
	}

	// A HALF-DECLARED statistic: a minimum with no maximum. It must be kept
	// exactly as written - discarding a declared number would be the silent
	// normalisation rule 1.6 forbids - but the flag must say it is not usable.
	{
		const FString Json = MutateManifest(TEXT("\"unit\": \"m/s\","),
			TEXT("\"unit\": \"m/s\",\n      \"statistics\": {\n")
			TEXT("        \"globalComponentMin\": [-1.0, -2.0, -3.0]\n      },"));

		FCFDVizCase Case;
		if (TestTrue(TEXT("a half-declared statistic still parses"),
			FCFDVizCase::ParseFromString(Json, FString(), Case).IsOk()))
		{
			const FCFDVizField* Velocity = Case.FindField(FName(TEXT("U")));
			if (TestNotNull(TEXT("U parsed"), Velocity))
			{
				TestEqual(TEXT("the declared minima are kept verbatim"),
					Velocity->Statistics.GlobalComponentMin.Num(), 3);
				TestFalse(TEXT("but the range is flagged unusable"),
					Velocity->Statistics.bHasComponentRange);

				FCFDVizStatistics Converted;
				TestFalse(TEXT("and it will not convert to a usable statistic"),
					Velocity->Statistics.TryMakeStatistics(3, Converted));
			}
		}
	}

	// Mismatched array lengths are the other half-declared shape.
	{
		const FString Json = MutateManifest(TEXT("\"unit\": \"m/s\","),
			TEXT("\"unit\": \"m/s\",\n      \"statistics\": {\n")
			TEXT("        \"globalComponentMin\": [-1.0, -2.0, -3.0],\n")
			TEXT("        \"globalComponentMax\": [1.0, 2.0]\n      },"));

		FCFDVizCase Case;
		if (TestTrue(TEXT("mismatched statistic lengths still parse"),
			FCFDVizCase::ParseFromString(Json, FString(), Case).IsOk()))
		{
			const FCFDVizField* Velocity = Case.FindField(FName(TEXT("U")));
			if (TestNotNull(TEXT("U parsed"), Velocity))
			{
				TestFalse(TEXT("but do not constitute a range"),
					Velocity->Statistics.bHasComponentRange);
			}
		}
	}

	// A field with NO statistics: absent must never read as zero. A field whose
	// values are entirely NaN omits statistics rather than writing +inf/-inf,
	// which JSON cannot express.
	{
		FCFDVizCase Case;
		TestTrue(TEXT("baseline parses"), FCFDVizCase::ParseFromString(BaselineManifest, FString(), Case).IsOk());
		const FCFDVizField* Pressure = Case.FindField(FName(TEXT("pressure")));
		if (TestNotNull(TEXT("pressure parsed"), Pressure))
		{
			TestTrue(TEXT("absent statistics are empty, not zero"), Pressure->Statistics.IsEmpty());
			TestFalse(TEXT("with no component range"), Pressure->Statistics.bHasComponentRange);
			TestFalse(TEXT("and no magnitude range"), Pressure->Statistics.bHasMagnitudeRange);

			FCFDVizStatistics Converted;
			TestFalse(TEXT("and nothing to convert"), Pressure->Statistics.TryMakeStatistics(1, Converted));
		}
	}

	return true;
}

/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizManifestOptionalTest,
	"FlowViz.CFDViz.Manifest.Optional",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FCFDVizManifestOptionalTest::RunTest(const FString& Parameters)
{
	// Optional blocks that the baseline omits: derived fields, structures,
	// provenance, display hints, and a sourceToCanonical matrix.
	const FString Json = MutateManifest(TEXT("\"meshes\": ["),
		TEXT("\"derivedFields\": [\n")
		TEXT("    { \"id\": \"speed\", \"expression\": \"mag(U)\", \"unit\": \"m/s\",\n")
		TEXT("      \"components\": [\"m\"], \"componentCount\": 1,\n")
		TEXT("      \"display\": { \"defaultColorMap\": \"viridis\", \"defaultRangeMode\": \"frame\",\n")
		TEXT("                    \"recommendedRange\": [0.0, 5.0] } }\n")
		TEXT("  ],\n")
		TEXT("  \"structures\": [\n")
		TEXT("    { \"id\": \"beam\", \"mesh\": \"obstacle\", \"displacementField\": \"disp\" }\n")
		TEXT("  ],\n")
		TEXT("  \"provenance\": { \"generatorCommand\": \"cfdviz build\", \"generatorVersion\": \"1.0.0\" },\n")
		TEXT("  \"meshes\": ["));

	FCFDVizCase Case;
	const FCFDVizResult Result = FCFDVizCase::ParseFromString(Json, FString(), Case);
	if (!TestTrue(TEXT("optional blocks parse"), Result.IsOk()))
	{
		AddError(FString::Printf(TEXT("parse failed: %s"), *Result.ToString()));
		return false;
	}

	// --- derived fields ----------------------------------------------------
	if (TestEqual(TEXT("one derived field"), Case.DerivedFields.Num(), 1))
	{
		const FCFDVizDerivedField* Speed = Case.FindDerivedField(FName(TEXT("speed")));
		if (TestNotNull(TEXT("speed found by id"), Speed))
		{
			// The expression is not parsed here - evaluation is the sampler's
			// job - but it must survive verbatim.
			TestEqual(TEXT("the expression is carried verbatim"), Speed->Expression,
				FString(TEXT("mag(U)")));
			TestEqual(TEXT("unit"), Speed->Unit, FString(TEXT("m/s")));
			TestEqual(TEXT("component count"), Speed->ComponentCount, 1);
			TestEqual(TEXT("colormap name is kept as a string"), Speed->Display.DefaultColorMap,
				FString(TEXT("viridis")));
			TestTrue(TEXT("range mode parsed"), Speed->Display.DefaultRangeMode == ECFDVizRangeMode::Frame);
			TestTrue(TEXT("recommended range present"), Speed->Display.RecommendedRange.IsSet());
			TestEqual(TEXT("range max"), Speed->Display.RecommendedRange.GetValue().Y, 5.0);
		}

		// The other half of the same sentence in CFDVizManifest.h. A derived
		// field is an EXPRESSION - a default-constructed one carries an empty
		// expression, and a sampler handed that would evaluate nothing and
		// report no error, which is the "field with no data" the header forbids
		// this from being confused with.
		TestNull(TEXT("an undeclared derived-field id finds nothing, never a derived field "
					  "with an empty expression"),
			Case.FindDerivedField(FName(TEXT("nosuchderived"))));
	}

	// --- structures ---------------------------------------------------------
	if (TestEqual(TEXT("one structure"), Case.Structures.Num(), 1))
	{
		TestEqual(TEXT("structure id"), Case.Structures[0].Id, FName(TEXT("beam")));
		TestEqual(TEXT("and it references the declared mesh"), Case.Structures[0].MeshId,
			FName(TEXT("obstacle")));
	}

	// --- provenance ---------------------------------------------------------
	TestEqual(TEXT("provenance command"), Case.Provenance.GeneratorCommand, FString(TEXT("cfdviz build")));

	// A structure naming an undeclared mesh must be rejected.
	{
		const FString Bad = Json.Replace(TEXT("\"mesh\": \"obstacle\""), TEXT("\"mesh\": \"nope\""),
			ESearchCase::CaseSensitive);
		FCFDVizCase BadCase;
		TestFalse(TEXT("a structure naming no declared mesh is rejected"),
			FCFDVizCase::ParseFromString(Bad, FString(), BadCase).IsOk());
	}

	// A derived field whose componentCount disagrees with its components.
	{
		const FString Bad = Json.Replace(TEXT("\"components\": [\"m\"], \"componentCount\": 1"),
			TEXT("\"components\": [\"m\"], \"componentCount\": 4"), ESearchCase::CaseSensitive);
		FCFDVizCase BadCase;
		TestFalse(TEXT("a derived field with a disagreeing componentCount is rejected"),
			FCFDVizCase::ParseFromString(Bad, FString(), BadCase).IsOk());
	}

	// --- sourceToCanonical, which must be TRANSPOSED into Unreal's convention -
	{
		// The manifest stores row-major with the translation in the last COLUMN
		// (flat indices 3, 7, 11). Unreal's FMatrix puts it in the last ROW. A
		// straight copy looks right for pure rotations and silently misplaces
		// every translation, so the translation is what this checks.
		const FString WithMatrix = MutateManifest(
			TEXT("\"coordinates\": { \"handedness\": \"right\", \"upAxis\": \"Z\", \"forwardAxis\": \"X\" }"),
			TEXT("\"coordinates\": { \"handedness\": \"right\", \"upAxis\": \"Z\", \"forwardAxis\": \"X\",\n")
			TEXT("    \"origin\": [1.0, 2.0, 3.0],\n")
			TEXT("    \"sourceToCanonical\": [1,0,0,10, 0,1,0,20, 0,0,1,30, 0,0,0,1] }"));

		FCFDVizCase Transformed;
		if (TestTrue(TEXT("a manifest with sourceToCanonical parses"),
			FCFDVizCase::ParseFromString(WithMatrix, FString(), Transformed).IsOk()))
		{
			// coordinates.origin is provenance only - it must NOT be folded
			// into grid positioning, which would double-offset the volume.
			TestEqual(TEXT("coordinates.origin is recorded"), Transformed.Coordinates.Origin,
				FVector(1.0, 2.0, 3.0));

			if (TestTrue(TEXT("the matrix is set"), Transformed.Coordinates.SourceToCanonical.IsSet()))
			{
				const FMatrix& M = Transformed.Coordinates.SourceToCanonical.GetValue();
				// Transposed: the translation now lives in the last ROW.
				TestEqual(TEXT("translation X moved to Unreal's row convention"), M.M[3][0], 10.0);
				TestEqual(TEXT("translation Y"), M.M[3][1], 20.0);
				TestEqual(TEXT("translation Z"), M.M[3][2], 30.0);
				// And is NOT left in the column where the manifest wrote it.
				TestEqual(TEXT("and is not left in the source column"), M.M[0][3], 0.0);
			}
		}
	}

	// A malformed matrix must be refused rather than partially applied.
	{
		const FString BadMatrix = MutateManifest(
			TEXT("\"coordinates\": { \"handedness\": \"right\", \"upAxis\": \"Z\", \"forwardAxis\": \"X\" }"),
			TEXT("\"coordinates\": { \"handedness\": \"right\", \"upAxis\": \"Z\", \"forwardAxis\": \"X\",\n")
			TEXT("    \"sourceToCanonical\": [1,0,0,0, 0,1,0,0] }"));

		FCFDVizCase BadCase;
		TestFalse(TEXT("a sourceToCanonical with the wrong element count is rejected"),
			FCFDVizCase::ParseFromString(BadMatrix, FString(), BadCase).IsOk());
	}

	// --- createdUtc ---------------------------------------------------------
	{
		const FString WithDate = MutateManifest(TEXT("\"quality\": \"visualization-demo\""),
			TEXT("\"quality\": \"visualization-demo\",\n    \"createdUtc\": \"2026-01-15T10:30:00Z\""));

		FCFDVizCase Dated;
		if (TestTrue(TEXT("a manifest with createdUtc parses"),
			FCFDVizCase::ParseFromString(WithDate, FString(), Dated).IsOk()))
		{
			// The raw text must be kept, so an ISO 8601 spelling FDateTime does
			// not accept stays a cosmetic gap rather than a load failure.
			TestEqual(TEXT("the raw text is preserved"), Dated.Metadata.CreatedUtcText,
				FString(TEXT("2026-01-15T10:30:00Z")));

			FDateTime Parsed;
			if (TestTrue(TEXT("and it parses to a timestamp"), Dated.Metadata.TryGetCreatedUtc(Parsed)))
			{
				TestEqual(TEXT("year"), Parsed.GetYear(), 2026);
				TestEqual(TEXT("month"), Parsed.GetMonth(), 1);
				TestEqual(TEXT("day"), Parsed.GetDay(), 15);
			}
		}
	}
	{
		// An unparseable date must not fail the load.
		const FString BadDate = MutateManifest(TEXT("\"quality\": \"visualization-demo\""),
			TEXT("\"quality\": \"visualization-demo\",\n    \"createdUtc\": \"last Tuesday\""));

		FCFDVizCase Dated;
		if (TestTrue(TEXT("an unparseable createdUtc does not fail the load"),
			FCFDVizCase::ParseFromString(BadDate, FString(), Dated).IsOk()))
		{
			TestEqual(TEXT("the text is still preserved"), Dated.Metadata.CreatedUtcText,
				FString(TEXT("last Tuesday")));
			FDateTime Parsed;
			TestFalse(TEXT("but it yields no timestamp"), Dated.Metadata.TryGetCreatedUtc(Parsed));
		}
	}

	// --- unit scale ---------------------------------------------------------
	{
		// An unrecognised length unit must REFUSE to produce a scale. Assuming
		// metres would place a millimetre-scale case a thousand times too large
		// with nothing on screen to say so.
		const FString Furlongs = MutateManifest(TEXT("\"length\": \"m\""), TEXT("\"length\": \"furlong\""));
		FCFDVizCase FurlongCase;
		if (TestTrue(TEXT("an unusual unit still parses"),
			FCFDVizCase::ParseFromString(Furlongs, FString(), FurlongCase).IsOk()))
		{
			double Scale = -1.0;
			TestFalse(TEXT("but an unrecognised length unit yields no scale"),
				FurlongCase.Units.TryGetLengthInMeters(Scale));
			TestEqual(TEXT("and leaves the output untouched rather than assuming 1"), Scale, -1.0);
		}
	}
	{
		const FString Millimetres = MutateManifest(TEXT("\"length\": \"m\""), TEXT("\"length\": \"mm\""));
		FCFDVizCase MillimetreCase;
		if (TestTrue(TEXT("millimetres parse"),
			FCFDVizCase::ParseFromString(Millimetres, FString(), MillimetreCase).IsOk()))
		{
			double Scale = 0.0;
			TestTrue(TEXT("mm resolves to a scale"), MillimetreCase.Units.TryGetLengthInMeters(Scale));
			TestEqual(TEXT("of one thousandth of a metre"), Scale, 0.001);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
