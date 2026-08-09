// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizFieldSampler.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37). The class name also matters here - the
 * blob already holds FFlowVizFieldSamplerTest (FlowVizFlowInspectionTest.cpp),
 * so this file's test class carries the finding's name instead.
 */
namespace FlowVizFieldSamplerHeaderTest
{
	FString GetSampleManifest()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(
			ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"), TEXT("manifest.json"));
	}

	/** The mutable entry for a field, so an arm can make the manifest lie. */
	FCFDVizField* FindMutableField(FCFDVizCase& Case, FName Id)
	{
		for (FCFDVizField& Field : Case.Fields)
		{
			if (Field.Id == Id)
			{
				return &Field;
			}
		}
		return nullptr;
	}
}

/**
 * THE CVF HEADER IS AUTHORITATIVE (format section 4.1) - and the sampler used
 * to trust the MANIFEST's DataType/ComponentCount to reinterpret CVF-decoded
 * bytes, never cross-checking them against the header of the file it actually
 * opened. A disagreeing manifest yielded plausible-but-wrong values SILENTLY:
 * a component count of 1 against a 3-component file reads the interleaved
 * x,y,z stream as scalars, and a narrower data type reads half of every value.
 * Both render as flow. This test makes each lie fail closed instead.
 *
 * The arms below are chosen so the PRISTINE code passes them wrongly - i.e.
 * they were red because Build SUCCEEDED and produced values. A mismatch in the
 * widening direction (float32 against a float16 file) already failed by
 * accident, via the read-past-the-end guard; the narrowing direction is the
 * one that needed the check.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizFieldSamplerHeaderCrossCheckTest,
	"FlowViz.Flow.FieldSampler.HeaderCrossCheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizFieldSamplerHeaderCrossCheckTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizFieldSamplerHeaderTest;

	const FString ManifestPath = GetSampleManifest();
	if (ManifestPath.IsEmpty() || !FPaths::FileExists(ManifestPath))
	{
		AddInfo(FString::Printf(
			TEXT("SKIPPED: no sample case at '%s'. Generate one with "
				 "'python3 -m cfdviz generate-mock --low-res'."),
			*ManifestPath));
		return true;
	}

	/* == Control: an honest manifest still builds ============================ */
	// Without this, every arm below could "pass" against a cross-check that
	// rejects everything - a control needs its own threshold checked.
	{
		FCFDVizCase Case;
		const FCFDVizResult Load = FCFDVizCase::LoadFromFile(ManifestPath, Case);
		if (!TestTrue(FString::Printf(TEXT("the sample case loads (%s)"), *Load.ToString()),
				Load.IsOk()))
		{
			return false;
		}

		FFlowVizFieldSampler Sampler;
		const FCFDVizResult Built = Sampler.Build(Case, FName(TEXT("U")), 0);
		TestTrue(FString::Printf(TEXT("an honest manifest builds (%s)"), *Built.ToString()),
			Built.IsOk());
		TestTrue(TEXT("the honest build is usable"), Sampler.IsBuilt());
	}

	/* == A lying ComponentCount fails closed ================================= */
	// U is 3-component in the file. A manifest claiming 1 makes the interleaved
	// x,y,z stream read as one-third as many scalars - every index is in range,
	// so nothing crashes and the values are plausible. That is the silent arm.
	{
		FCFDVizCase Case;
		FCFDVizCase::LoadFromFile(ManifestPath, Case);
		FCFDVizField* Field = FindMutableField(Case, FName(TEXT("U")));
		if (!TestNotNull(TEXT("the sample declares U"), Field))
		{
			return false;
		}
		Field->ComponentCount = 1;

		FFlowVizFieldSampler Sampler;
		const FCFDVizResult Built = Sampler.Build(Case, FName(TEXT("U")), 0);
		TestFalse(TEXT("a manifest component count disagreeing with the CVF header is refused"),
			Built.IsOk());
		TestTrue(TEXT("the refusal names an invalid manifest"),
			Built.Error == ECFDVizError::InvalidManifest);
		TestFalse(TEXT("the failed build leaves an empty sampler"), Sampler.IsBuilt());

		TArray<double> Value;
		TestFalse(TEXT("the failed sampler refuses to sample"),
			Sampler.Sample(FVector(6.0, 2.0, 0.5), Value));
	}

	/* == A lying DataType fails closed ======================================= */
	// U is float16 in the file. A manifest claiming uint8 - a legal CVF type -
	// reads one byte of every half, in range everywhere, silently. This is the
	// narrowing direction the old read-past-the-end guard could not catch.
	{
		FCFDVizCase Case;
		FCFDVizCase::LoadFromFile(ManifestPath, Case);
		FCFDVizField* Field = FindMutableField(Case, FName(TEXT("U")));
		if (!TestNotNull(TEXT("the sample declares U"), Field))
		{
			return false;
		}
		Field->DataType = ECFDVizDataType::UInt8;

		FFlowVizFieldSampler Sampler;
		const FCFDVizResult Built = Sampler.Build(Case, FName(TEXT("U")), 0);
		TestFalse(TEXT("a manifest data type disagreeing with the CVF header is refused"),
			Built.IsOk());
		TestTrue(TEXT("the refusal names an invalid manifest"),
			Built.Error == ECFDVizError::InvalidManifest);
		TestFalse(TEXT("the failed build leaves an empty sampler"), Sampler.IsBuilt());
	}

	/* == A lying Association fails closed ==================================== */
	// The third sibling of the same defect: cell versus point decides both the
	// value extent and the half-cell sampling offset. The header carries it
	// (offset 60), so a manifest that disagrees is the same class of lie -
	// values shifted half a cell, rendered as plausible flow.
	{
		FCFDVizCase Case;
		FCFDVizCase::LoadFromFile(ManifestPath, Case);
		FCFDVizField* Field = FindMutableField(Case, FName(TEXT("U")));
		if (!TestNotNull(TEXT("the sample declares U"), Field))
		{
			return false;
		}
		Field->Association = ECFDVizAssociation::Point;

		FFlowVizFieldSampler Sampler;
		const FCFDVizResult Built = Sampler.Build(Case, FName(TEXT("U")), 0);
		TestFalse(TEXT("a manifest association disagreeing with the CVF header is refused"),
			Built.IsOk());
		TestTrue(TEXT("the refusal names an invalid manifest"),
			Built.Error == ECFDVizError::InvalidManifest);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
