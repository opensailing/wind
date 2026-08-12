// Copyright FlowViz contributors. All Rights Reserved.

#include "../Render/FlowVizVolumeRayMarchDispatcher.h"

#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "SceneView.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The production dispatcher is the first consumer of SlotB and Alpha.
 *
 * This test drives that consumer through its real queue rather than rebuilding
 * parameters beside it. NullRHI is sufficient: no pixel is sampled, but distinct
 * RHI resources and their descriptors still exist and make A/B retention and
 * compatibility falsifiable.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTemporalDispatcherTest,
	"FlowViz.Render.TemporalDispatcher",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizTemporalDispatcherFixture
{
	constexpr int32 ViewWidth = 8;
	constexpr int32 ViewHeight = 4;
	constexpr float InteriorAlpha = 0.25f;

	struct FHarness
	{
		FlowVizVolumeRayMarchProduction::FDispatcher Dispatcher;
		FFlowVizVolumeSlotTextures SlotA;
		FFlowVizVolumeSlotTextures SlotB;
		FTextureRHIRef WrongFormatField;
		FTextureRHIRef VectorFieldA;
		FTextureRHIRef VectorFieldB;
		FTextureRHIRef WrongExtentField;
		FTextureRHIRef WrongExtentStatus;
		FSceneViewInitOptions ViewInit;
		TUniquePtr<FSceneView> View;

		void CreateResources()
		{
			FHarness* Self = this;
			ENQUEUE_RENDER_COMMAND(FlowVizTemporalDispatcherCreateResources)(
				[Self](FRHICommandListImmediate& RHICmdList)
				{
					auto CreateVolume = [&RHICmdList](
						const TCHAR* Name,
						const FIntVector& Extent,
						EPixelFormat Format)
					{
						const FRHITextureCreateDesc Desc =
							FRHITextureCreateDesc::Create3D(Name)
								.SetExtent(Extent.X, Extent.Y)
								.SetDepth(Extent.Z)
								.SetFormat(Format)
								.SetFlags(ETextureCreateFlags::ShaderResource);
						return RHICmdList.CreateTexture(Desc);
					};

					const FIntVector Extent(2, 2, 2);
					const FIntVector WrongExtent(3, 2, 2);
					Self->SlotA.ScalarTexture = CreateVolume(
						TEXT("FlowVizTemporalDispatcherFieldA"), Extent, PF_R32_FLOAT);
					Self->SlotB.ScalarTexture = CreateVolume(
						TEXT("FlowVizTemporalDispatcherFieldB"), Extent, PF_R32_FLOAT);
					Self->SlotA.StatusTexture = CreateVolume(
						TEXT("FlowVizTemporalDispatcherStatusA"), Extent, PF_R8_UINT);
					Self->SlotB.StatusTexture = CreateVolume(
						TEXT("FlowVizTemporalDispatcherStatusB"), Extent, PF_R8_UINT);
					Self->WrongFormatField = CreateVolume(
						TEXT("FlowVizTemporalDispatcherWrongFormat"), Extent, PF_R16F);
					Self->VectorFieldA = CreateVolume(
						TEXT("FlowVizTemporalDispatcherVectorA"), Extent, PF_A32B32G32R32F);
					Self->VectorFieldB = CreateVolume(
						TEXT("FlowVizTemporalDispatcherVectorB"), Extent, PF_A32B32G32R32F);
					Self->WrongExtentField = CreateVolume(
						TEXT("FlowVizTemporalDispatcherWrongExtentField"), WrongExtent, PF_R32_FLOAT);
					Self->WrongExtentStatus = CreateVolume(
						TEXT("FlowVizTemporalDispatcherWrongExtentStatus"), WrongExtent, PF_R8_UINT);
				});
			FlushRenderingCommands();

			const FIntVector Extent(2, 2, 2);
			FFlowVizVolumeLayout::Make(
				Extent, 1, ECFDVizDataType::Float32, SlotA.ScalarLayout);
			FFlowVizVolumeLayout::MakeStatusLayout(Extent, SlotA.StatusLayout);
			SlotB.ScalarLayout = SlotA.ScalarLayout;
			SlotB.StatusLayout = SlotA.StatusLayout;

			ViewInit.SetViewRectangle(FIntRect(0, 0, ViewWidth, ViewHeight));
			ViewInit.ViewOrigin = FVector::ZeroVector;
			ViewInit.ViewRotationMatrix = FMatrix::Identity;
			ViewInit.ProjectionMatrix = FMatrix::Identity;
			View = MakeUnique<FSceneView>(ViewInit);
		}

		bool IsReady() const
		{
			return View.IsValid()
				&& SlotA.ScalarTexture.IsValid()
				&& SlotB.ScalarTexture.IsValid()
				&& SlotA.StatusTexture.IsValid()
				&& SlotB.StatusTexture.IsValid()
				&& WrongFormatField.IsValid()
				&& VectorFieldA.IsValid()
				&& VectorFieldB.IsValid()
				&& WrongExtentField.IsValid()
				&& WrongExtentStatus.IsValid()
				&& SlotA.ScalarLayout.IsValid()
				&& SlotA.StatusLayout.IsValid();
		}

		void ReleaseResources()
		{
			Dispatcher.ReleaseResources();
			SlotA.ScalarTexture.SafeRelease();
			SlotA.StatusTexture.SafeRelease();
			SlotB.ScalarTexture.SafeRelease();
			SlotB.StatusTexture.SafeRelease();
			WrongFormatField.SafeRelease();
			VectorFieldA.SafeRelease();
			VectorFieldB.SafeRelease();
			WrongExtentField.SafeRelease();
			WrongExtentStatus.SafeRelease();
			FlushRenderingCommands();
		}

		FFlowVizVolumeRayMarchContext MakeContext(
			const FFlowVizVolumeSlotTextures* InSlotB,
			float Alpha,
			bool bProducerReportedDegradation = false) const
		{
			FFlowVizVolumeRayMarchContext Context;
			Context.View = View.Get();
			Context.LocalToWorld = FMatrix::Identity;
			Context.SlotA = &SlotA;
			Context.SlotB = InSlotB;
			Context.Alpha = Alpha;
			Context.bInterpolationDegraded = bProducerReportedDegradation;
			Context.Parameters.VolumeDimensions = SlotA.ScalarLayout.Extent;
			Context.Parameters.ComponentCount = SlotA.ScalarLayout.SourceComponentCount;
			Context.Parameters.TextureComponentCount = SlotA.ScalarLayout.TextureComponentCount;
			Context.Parameters.DataTypeCode = static_cast<uint32>(SlotA.ScalarLayout.DataType);
			Context.Parameters.AssociationCode = static_cast<uint32>(ECFDVizAssociation::Cell);
			return Context;
		}

		bool Dispatch(
			const FFlowVizVolumeRayMarchContext& Context,
			FFlowVizVolumeRayMarchParameters& OutParameters)
		{
			const int32 Before = Dispatcher.NumPendingRequests();
			Dispatcher.DispatchVolumeRayMarch(Context);
			if (Dispatcher.NumPendingRequests() != Before + 1)
			{
				return false;
			}
			return Dispatcher.PeekRequestParameters(Before, OutParameters);
		}
	};
}

bool FFlowVizTemporalDispatcherTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizTemporalDispatcherFixture;

	FHarness Harness;
	Harness.CreateResources();
	if (!TestTrue(TEXT("the temporal dispatcher fixture created distinct A/B resources"),
			Harness.IsReady()))
	{
		Harness.ReleaseResources();
		return false;
	}

	/* A genuine single stored frame remains the A-only identity control. */
	{
		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("a single-frame request is queued"),
				Harness.Dispatch(Harness.MakeContext(nullptr, 0.0f), Actual)))
		{
			TestTrue(TEXT("single-frame A reaches the primary field slot"),
				static_cast<FRHITexture*>(Actual.FieldTexture)
					== Harness.SlotA.ScalarTexture.GetReference());
			TestTrue(TEXT("single-frame A is mirrored into B"),
				static_cast<FRHITexture*>(Actual.FieldTextureB)
					== Harness.SlotA.ScalarTexture.GetReference());
			TestEqual(TEXT("single-frame rendering does not activate blending"),
				static_cast<int32>(Actual.bBlendActive), 0);
			TestEqual(TEXT("single-frame rendering carries zero blend alpha"),
				Actual.BlendAlpha, 0.0f);
		}
	}

	/* The assertion V3 exists for: production must consume B and alpha. */
	{
		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("a compatible temporal request is queued"),
				Harness.Dispatch(Harness.MakeContext(&Harness.SlotB, InteriorAlpha), Actual)))
		{
			TestTrue(TEXT("production retains the distinct frame-B field resource"),
				static_cast<FRHITexture*>(Actual.FieldTextureB)
					== Harness.SlotB.ScalarTexture.GetReference());
			TestTrue(TEXT("production retains the nearest-frame-capable B status resource"),
				static_cast<FRHITexture*>(Actual.StatusTextureB)
					== Harness.SlotB.StatusTexture.GetReference());
			TestEqual(TEXT("a compatible temporal request activates shader blending"),
				static_cast<int32>(Actual.bBlendActive), 1);
			TestEqual(TEXT("the narrowed display alpha reaches the shader request"),
				Actual.BlendAlpha, InteriorAlpha);
			TestEqual(TEXT("a compatible pair does not start a degradation episode"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(0));
		}
	}

	/* Vector-only residency must reach production rather than falling through the empty scalar channel. */
	{
		FFlowVizVolumeSlotTextures VectorSlotA = Harness.SlotA;
		FFlowVizVolumeSlotTextures VectorSlotB = Harness.SlotB;
		VectorSlotA.ScalarTexture.SafeRelease();
		VectorSlotB.ScalarTexture.SafeRelease();
		VectorSlotA.ScalarLayout = FFlowVizVolumeLayout();
		VectorSlotB.ScalarLayout = FFlowVizVolumeLayout();
		VectorSlotA.VectorTexture = Harness.VectorFieldA;
		VectorSlotB.VectorTexture = Harness.VectorFieldB;
		FFlowVizVolumeLayout::Make(
			FIntVector(2, 2, 2), 3, ECFDVizDataType::Float32, VectorSlotA.VectorLayout);
		VectorSlotB.VectorLayout = VectorSlotA.VectorLayout;

		FFlowVizVolumeRayMarchContext Context = Harness.MakeContext(nullptr, InteriorAlpha);
		Context.SlotA = &VectorSlotA;
		Context.SlotB = &VectorSlotB;
		Context.Parameters.VolumeDimensions = VectorSlotA.VectorLayout.Extent;
		Context.Parameters.ComponentCount = VectorSlotA.VectorLayout.SourceComponentCount;
		Context.Parameters.TextureComponentCount = VectorSlotA.VectorLayout.TextureComponentCount;
		Context.Parameters.DataTypeCode = static_cast<uint32>(VectorSlotA.VectorLayout.DataType);

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("a compatible vector-only temporal request is queued"),
				Harness.Dispatch(Context, Actual)))
		{
			TestTrue(TEXT("production binds vector frame A as the primary field"),
				static_cast<FRHITexture*>(Actual.FieldTexture)
					== Harness.VectorFieldA.GetReference());
			TestTrue(TEXT("production retains the distinct vector frame-B field"),
				static_cast<FRHITexture*>(Actual.FieldTextureB)
					== Harness.VectorFieldB.GetReference());
			TestEqual(TEXT("a vector request declares the vector binding state"),
				static_cast<int32>(Actual.bHasVectorTexture), 1);
			TestEqual(TEXT("a compatible vector pair activates temporal blending"),
				static_cast<int32>(Actual.bBlendActive), 1);
			TestEqual(TEXT("the vector request retains display alpha"),
				Actual.BlendAlpha, InteriorAlpha);
		}
	}

	/* A resident prefetch must not turn an exact frame-A request into a blend. */
	{
		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("an alpha-zero request with resident B is queued"),
				Harness.Dispatch(Harness.MakeContext(&Harness.SlotB, 0.0f), Actual)))
		{
			TestTrue(TEXT("alpha zero preserves the A-only field binding"),
				static_cast<FRHITexture*>(Actual.FieldTextureB)
					== Harness.SlotA.ScalarTexture.GetReference());
			TestTrue(TEXT("alpha zero preserves the A-only status binding"),
				static_cast<FRHITexture*>(Actual.StatusTextureB)
					== Harness.SlotA.StatusTexture.GetReference());
			TestEqual(TEXT("alpha zero does not activate blending"),
				static_cast<int32>(Actual.bBlendActive), 0);
			TestEqual(TEXT("alpha zero remains exact"), Actual.BlendAlpha, 0.0f);
			TestEqual(TEXT("an unrequested blend is not reported as degradation"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(0));
		}
	}

	/* Cell and point fields cannot share one physical sample location. */
	{
		FFlowVizVolumeSlotTextures WrongAssociation = Harness.SlotB;
		WrongAssociation.Association = ECFDVizAssociation::Point;

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("an association-incompatible B request queues its A fallback"),
				Harness.Dispatch(Harness.MakeContext(&WrongAssociation, InteriorAlpha), Actual)))
		{
			TestTrue(TEXT("association-incompatible B falls back to A"),
				static_cast<FRHITexture*>(Actual.FieldTextureB)
					== Harness.SlotA.ScalarTexture.GetReference());
			TestEqual(TEXT("association-incompatible B disables blending"),
				static_cast<int32>(Actual.bBlendActive), 0);
			TestEqual(TEXT("association-incompatible B clears alpha"), Actual.BlendAlpha, 0.0f);
			TestEqual(TEXT("association incompatibility starts a disclosed episode"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(1));
		}

		FFlowVizVolumeRayMarchParameters Recovered;
		TestTrue(TEXT("association compatibility recovery retires the warning latch"),
			Harness.Dispatch(Harness.MakeContext(&Harness.SlotB, InteriorAlpha), Recovered));
	}

	/* Matching metadata is insufficient when the actual RHI texture disagrees. */
	{
		FFlowVizVolumeSlotTextures WrongFormat = Harness.SlotB;
		WrongFormat.ScalarTexture = Harness.WrongFormatField;

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("a wrong-format B request still queues its honest A fallback"),
				Harness.Dispatch(Harness.MakeContext(&WrongFormat, InteriorAlpha), Actual)))
		{
			TestTrue(TEXT("wrong-format B falls back to A in the B field slot"),
				static_cast<FRHITexture*>(Actual.FieldTextureB)
					== Harness.SlotA.ScalarTexture.GetReference());
			TestTrue(TEXT("wrong-format B also falls back to A status"),
				static_cast<FRHITexture*>(Actual.StatusTextureB)
					== Harness.SlotA.StatusTexture.GetReference());
			TestEqual(TEXT("wrong-format B disables blending"),
				static_cast<int32>(Actual.bBlendActive), 0);
			TestEqual(TEXT("wrong-format B clears alpha"), Actual.BlendAlpha, 0.0f);
			TestEqual(TEXT("wrong-format B starts a second disclosed degradation episode"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(2));
		}
	}

	/* Recovery retires the episode latch before an independent incompatibility. */
	{
		FFlowVizVolumeRayMarchParameters Recovered;
		TestTrue(TEXT("a compatible request recovers interpolation"),
			Harness.Dispatch(Harness.MakeContext(&Harness.SlotB, InteriorAlpha), Recovered));

		FFlowVizVolumeSlotTextures WrongExtent = Harness.SlotB;
		WrongExtent.ScalarTexture = Harness.WrongExtentField;
		WrongExtent.StatusTexture = Harness.WrongExtentStatus;
		WrongExtent.ScalarLayout.Extent = FIntVector(3, 2, 2);
		WrongExtent.StatusLayout.Extent = FIntVector(3, 2, 2);

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("a wrong-extent B request still queues its A fallback"),
				Harness.Dispatch(Harness.MakeContext(&WrongExtent, InteriorAlpha), Actual)))
		{
			TestTrue(TEXT("wrong-extent B falls back to A"),
				static_cast<FRHITexture*>(Actual.FieldTextureB)
					== Harness.SlotA.ScalarTexture.GetReference());
			TestEqual(TEXT("wrong-extent B disables blending"),
				static_cast<int32>(Actual.bBlendActive), 0);
			TestEqual(TEXT("a later independent incompatibility starts a third episode"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(3));
		}
	}

	/* Component/type metadata is part of compatibility, not just dimensions. */
	{
		FFlowVizVolumeRayMarchParameters Recovered;
		TestTrue(TEXT("a second compatible request recovers interpolation"),
			Harness.Dispatch(Harness.MakeContext(&Harness.SlotB, InteriorAlpha), Recovered));

		FFlowVizVolumeSlotTextures WrongComponents = Harness.SlotB;
		WrongComponents.ScalarLayout.SourceComponentCount = 2;
		WrongComponents.ScalarLayout.TextureComponentCount = 2;

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("a component-incompatible B request queues its A fallback"),
				Harness.Dispatch(Harness.MakeContext(&WrongComponents, InteriorAlpha), Actual)))
		{
			TestEqual(TEXT("component-incompatible B disables blending"),
				static_cast<int32>(Actual.bBlendActive), 0);
			TestEqual(TEXT("component-incompatible B clears alpha"), Actual.BlendAlpha, 0.0f);
			TestEqual(TEXT("component incompatibility is disclosed as a fourth episode"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(4));
		}
	}

	/* Data type is independent compatibility metadata even at one RHI format. */
	{
		FFlowVizVolumeRayMarchParameters Recovered;
		TestTrue(TEXT("component compatibility recovery retires the warning latch"),
			Harness.Dispatch(Harness.MakeContext(&Harness.SlotB, InteriorAlpha), Recovered));

		FFlowVizVolumeSlotTextures WrongDataType = Harness.SlotB;
		WrongDataType.ScalarLayout.DataType = ECFDVizDataType::UInt8;

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("a data-type-incompatible B request queues its A fallback"),
				Harness.Dispatch(Harness.MakeContext(&WrongDataType, InteriorAlpha), Actual)))
		{
			TestEqual(TEXT("data-type-incompatible B disables blending"),
				static_cast<int32>(Actual.bBlendActive), 0);
			TestEqual(TEXT("data-type-incompatible B clears alpha"), Actual.BlendAlpha, 0.0f);
			TestEqual(TEXT("data-type incompatibility is disclosed as a fifth episode"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(5));
		}
	}

	/* Status provenance must match: neither presence nor layout is blendable. */
	{
		FFlowVizVolumeRayMarchParameters Recovered;
		TestTrue(TEXT("data-type compatibility recovery retires the warning latch"),
			Harness.Dispatch(Harness.MakeContext(&Harness.SlotB, InteriorAlpha), Recovered));

		FFlowVizVolumeSlotTextures MissingStatus = Harness.SlotB;
		MissingStatus.StatusTexture.SafeRelease();
		MissingStatus.StatusLayout = FFlowVizVolumeLayout();

		FFlowVizVolumeRayMarchParameters MissingActual;
		if (TestTrue(TEXT("a status-presence mismatch queues its A fallback"),
				Harness.Dispatch(Harness.MakeContext(&MissingStatus, InteriorAlpha), MissingActual)))
		{
			TestTrue(TEXT("status-presence mismatch mirrors A status"),
				static_cast<FRHITexture*>(MissingActual.StatusTextureB)
					== Harness.SlotA.StatusTexture.GetReference());
			TestEqual(TEXT("status-presence mismatch disables blending"),
				static_cast<int32>(MissingActual.bBlendActive), 0);
			TestEqual(TEXT("status-presence mismatch is disclosed as a sixth episode"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(6));
		}

		FFlowVizVolumeRayMarchParameters PresenceRecovered;
		TestTrue(TEXT("status-presence recovery retires the warning latch"),
			Harness.Dispatch(Harness.MakeContext(&Harness.SlotB, InteriorAlpha), PresenceRecovered));

		FFlowVizVolumeSlotTextures WrongStatusLayout = Harness.SlotB;
		WrongStatusLayout.StatusLayout.DataType = ECFDVizDataType::Float32;
		WrongStatusLayout.StatusLayout.PixelFormat = PF_R32_FLOAT;

		FFlowVizVolumeRayMarchParameters LayoutActual;
		if (TestTrue(TEXT("a status-layout mismatch queues its A fallback"),
				Harness.Dispatch(Harness.MakeContext(&WrongStatusLayout, InteriorAlpha), LayoutActual)))
		{
			TestEqual(TEXT("status-layout mismatch disables blending"),
				static_cast<int32>(LayoutActual.bBlendActive), 0);
			TestEqual(TEXT("status-layout mismatch clears alpha"), LayoutActual.BlendAlpha, 0.0f);
			TestEqual(TEXT("status-layout mismatch is disclosed as a seventh episode"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(7));
		}
	}

	/* The producer's missing-B signal remains a valid fallback source. */
	{
		FFlowVizVolumeRayMarchParameters Recovered;
		TestTrue(TEXT("a final compatible request recovers interpolation"),
			Harness.Dispatch(Harness.MakeContext(&Harness.SlotB, InteriorAlpha), Recovered));

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("a producer-reported missing B queues frame A"),
				Harness.Dispatch(
					Harness.MakeContext(nullptr, InteriorAlpha, true), Actual)))
		{
			TestTrue(TEXT("missing B mirrors A into the B resource slot"),
				static_cast<FRHITexture*>(Actual.FieldTextureB)
					== Harness.SlotA.ScalarTexture.GetReference());
			TestEqual(TEXT("missing B disables blending"),
				static_cast<int32>(Actual.bBlendActive), 0);
			TestEqual(TEXT("missing B clears alpha"), Actual.BlendAlpha, 0.0f);
			TestEqual(TEXT("missing B starts an eighth disclosed episode"),
				Harness.Dispatcher.GetInterpolationDegradedEpisodeCount(), uint64(8));
		}
	}

	Harness.ReleaseResources();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
