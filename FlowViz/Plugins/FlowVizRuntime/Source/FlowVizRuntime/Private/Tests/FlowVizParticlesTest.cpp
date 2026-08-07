// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"
#include "Flow/FlowVizParticles.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizParticlesTest
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
 * The tracer advector (renderer overhaul P5) -- the tested statement of the
 * math P8's GPU particles must reproduce: RK2 through the field, kill on
 * mask/exit/age, deterministic respawn.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizParticlesTest,
	"FlowViz.Flow.Particles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizParticlesTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizParticlesTest;

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

	FlowVizParticles::FAdvanceSettings Settings;
	Settings.DeltaSeconds = 0.01;
	Settings.MaxAge = 100.0;

	/* == Advection follows the flow ========================================= */
	{
		// One particle in free stream: U.x is strictly positive everywhere
		// (declared globalComponentMin[0] = 0.553), so every step must move it
		// STRICTLY downstream.
		TArray<FlowVizParticles::FParticle> Particles;
		FlowVizParticles::FParticle& Tracer = Particles.AddDefaulted_GetRef();
		Tracer.Position = FVector(1.0, 3.0, 0.5);
		Tracer.bAlive = true;

		double LastX = Tracer.Position.X;
		for (int32 Step = 0; Step < 50; ++Step)
		{
			if (FlowVizParticles::AdvanceParticles(
					Velocity, nullptr, 0.0, Mask, Settings, Particles) == 0)
			{
				break;
			}
			TestTrue(TEXT("the tracer moves strictly downstream each step"),
				Particles[0].Position.X > LastX);
			LastX = Particles[0].Position.X;
		}
		TestTrue(TEXT("fifty steps carried it visibly downstream"),
			LastX > 1.0 + 0.05);
		TestTrue(TEXT("and aged it"), Particles[0].Age > 0.4);
	}

	/* == The mask kills; the domain edge kills ============================== */
	{
		TArray<FlowVizParticles::FParticle> Particles;
		// Seeded just upstream of the cylinder (axis at (4,2), r=0.45), on the
		// stagnation line: the flow carries it INTO the mask.
		FlowVizParticles::FParticle& IntoMask = Particles.AddDefaulted_GetRef();
		IntoMask.Position = FVector(3.4, 2.0, 0.5);
		IntoMask.bAlive = true;
		// And one at the outlet edge, about to exit.
		FlowVizParticles::FParticle& Exiting = Particles.AddDefaulted_GetRef();
		Exiting.Position = FVector(11.99, 3.0, 0.5);
		Exiting.bAlive = true;

		int32 StepsUntilMaskKill = 0;
		for (int32 Step = 0; Step < 2000 && Particles[0].bAlive; ++Step)
		{
			FlowVizParticles::AdvanceParticles(
				Velocity, nullptr, 0.0, Mask, Settings, Particles);
			++StepsUntilMaskKill;
		}
		TestFalse(TEXT("the stagnation-line tracer DIES at the obstacle -- no flow "
					   "through the cylinder (rule 10 in particle form)"),
			Particles[0].bAlive);
		TestFalse(TEXT("the outlet tracer died by exiting the domain"),
			Particles[1].bAlive);
	}

	/* == Deterministic respawn ============================================== */
	{
		TArray<FlowVizParticles::FParticle> A, B;
		A.SetNum(64);
		B.SetNum(64);
		const FVector RakeStart(0.5, 0.5, 0.5);
		const FVector RakeEnd(0.5, 3.5, 0.5);
		FlowVizParticles::RespawnDead(RakeStart, RakeEnd, A);
		FlowVizParticles::RespawnDead(RakeStart, RakeEnd, B);

		bool bIdentical = true;
		bool bAllAlive = true;
		bool bOnRake = true;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			bIdentical &= A[Index].Position.Equals(B[Index].Position, 0.0);
			bAllAlive &= A[Index].bAlive;
			bOnRake &= FMath::IsNearlyEqual(A[Index].Position.X, 0.5, 1e-9)
				&& A[Index].Position.Y >= 0.5 - 1e-9
				&& A[Index].Position.Y <= 3.5 + 1e-9;
		}
		TestTrue(TEXT("respawn is DETERMINISTIC -- a capture rendered twice is the "
					  "same capture"), bIdentical);
		TestTrue(TEXT("every dead particle came back"), bAllAlive);
		TestTrue(TEXT("on the rake"), bOnRake);

		// Coverage: the golden-ratio sequence spreads across the rake rather
		// than clustering -- the spread must span most of the rake's extent.
		double MinY = 4.0, MaxY = 0.0;
		for (const FlowVizParticles::FParticle& Particle : A)
		{
			MinY = FMath::Min(MinY, Particle.Position.Y);
			MaxY = FMath::Max(MaxY, Particle.Position.Y);
		}
		TestTrue(TEXT("respawn covers the rake, not a corner of it"),
			MaxY - MinY > 2.5);
	}

	/* == Age-out ============================================================ */
	{
		TArray<FlowVizParticles::FParticle> Particles;
		FlowVizParticles::FParticle& Old = Particles.AddDefaulted_GetRef();
		Old.Position = FVector(1.0, 3.0, 0.5);
		Old.Age = 99.999;
		Old.bAlive = true;

		FlowVizParticles::FAdvanceSettings Aging = Settings;
		Aging.MaxAge = 100.0;
		Aging.DeltaSeconds = 0.01;
		FlowVizParticles::AdvanceParticles(Velocity, nullptr, 0.0, Mask, Aging, Particles);
		TestFalse(TEXT("a particle past MaxAge is killed for respawn"), Particles[0].bAlive);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
