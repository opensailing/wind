// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Render/FlowVizNiagaraFeed.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizNiagaraFeedTest
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
}

/**
 * The Niagara feed's packing (renderer overhaul P8): velocity in RGB, the
 * mask in ALPHA -- the contract the GPU sim's kill switch and the CPU
 * oracle's IsMaskedAt must agree on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizNiagaraFeedTest,
	"FlowViz.Render.NiagaraFeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizNiagaraFeedTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizNiagaraFeedTest;

	using FRootedUploadSignature = bool (*)(
		const FFlowVizFieldSampler&,
		const FFlowVizFieldMask&,
		UObject*,
		TStrongObjectPtr<UTextureRenderTargetVolume>&);
	FRootedUploadSignature RootedUpload = &FlowVizNiagaraFeed::UploadToRenderTarget;
	TestTrue(TEXT("the upload API requires a rooted target owner"), RootedUpload != nullptr);

	FCFDVizCase Case;
	if (!TestTrue(TEXT("the sample case loads"),
			FCFDVizCase::LoadFromFile(GetSampleManifest(), Case).IsOk()))
	{
		return false;
	}
	FFlowVizFieldSampler Velocity;
	if (!TestTrue(TEXT("the U sampler builds"),
			Velocity.Build(Case, TEXT("U"), 0).IsOk()))
	{
		return false;
	}
	FFlowVizFieldMask Mask;
	Mask.Build(Velocity);

	TArray<FFloat16Color> Texels;
	FIntVector Counts;
	if (!TestTrue(TEXT("packing succeeds"),
			FlowVizNiagaraFeed::PackVelocityMask(Velocity, Mask, Texels, Counts)))
	{
		return false;
	}

	TestEqual(TEXT("the texel grid is the field grid"), Counts, FIntVector(56, 28, 6));
	TestEqual(TEXT("one texel per voxel"), Texels.Num(), 56 * 28 * 6);

	/* == The alpha channel IS the mask ====================================== */
	{
		int32 MaskedTexels = 0;
		int32 NaNTexels = 0;
		for (const FFloat16Color& Texel : Texels)
		{
			if (Texel.A.GetFloat() < 0.5f)
			{
				++MaskedTexels;
				// Masked texels carry ZERO velocity, never NaN: a NaN texel
				// poisons hardware filtering across the boundary.
				if (Texel.R.GetFloat() != 0.0f || Texel.G.GetFloat() != 0.0f
					|| Texel.B.GetFloat() != 0.0f)
				{
					++NaNTexels;
				}
			}
		}
		// The identity control: exactly the cylinder's 132 voxels.
		TestEqual(TEXT("exactly the mask's voxels have alpha 0"), MaskedTexels, 132);
		TestEqual(TEXT("and all of them carry zero velocity, not NaN or garbage"),
			NaNTexels, 0);
	}

	/* == Fluid texels carry the field ======================================= */
	{
		// Free-stream voxel (4, 21, 3): U.x strictly positive there.
		const int32 Index = (3 * 28 + 21) * 56 + 4;
		const FFloat16Color& Free = Texels[Index];
		TestTrue(TEXT("a free-stream texel is fluid (alpha 1)"), Free.A.GetFloat() > 0.5f);
		TestTrue(TEXT("with positive downstream velocity -- the declared strict "
					  "positivity of U.x"),
			Free.R.GetFloat() > 0.0f);

		// DIFFERENTIAL against the sampler: the texel is the voxel's value.
		TArray<double> Expected;
		Velocity.GetVoxelValue(FIntVector(4, 21, 3), Expected);
		TestTrue(TEXT("the texel matches the sampler's voxel to half precision"),
			FMath::Abs(Free.R.GetFloat() - Expected[0]) < 0.01
				&& FMath::Abs(Free.G.GetFloat() - Expected[1]) < 0.01
				&& FMath::Abs(Free.B.GetFloat() - Expected[2]) < 0.01);
	}

	/* == Refusals =========================================================== */
	{
		int32 OverflowedCount = 0;
		TestFalse(TEXT("a voxel product larger than int32 refuses before allocation"),
			FlowVizNiagaraFeed::GetPackedVoxelCount(
				FIntVector(1291, 1291, 1291), OverflowedCount));
		TestEqual(TEXT("a refused voxel count is reset"), OverflowedCount, 0);

		int32 OversizedAxisCount = 7;
		TestFalse(TEXT("a grid outside the volume texture axis limit refuses"),
			FlowVizNiagaraFeed::GetPackedVoxelCount(
				FIntVector(2049, 1, 1), OversizedAxisCount));
		TestEqual(TEXT("an axis-limit refusal also resets the count"), OversizedAxisCount, 0);

		FFlowVizFieldSampler Scalar;
		if (TestTrue(TEXT("a scalar sampler builds"),
				Scalar.Build(Case, TEXT("pressure"), 0).IsOk()))
		{
			TArray<FFloat16Color> Refused;
			FIntVector RefusedCounts;
			TestFalse(TEXT("a scalar field refuses -- velocity packing needs 3 "
						   "components"),
				FlowVizNiagaraFeed::PackVelocityMask(
					Scalar, Mask, Refused, RefusedCounts));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
