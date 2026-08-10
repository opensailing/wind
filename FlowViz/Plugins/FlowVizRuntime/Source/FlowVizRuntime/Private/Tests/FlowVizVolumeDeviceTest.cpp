// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Render/FlowVizVolumeTexture.h"

#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RHIResources.h"
#include "RenderingThread.h"

/**
 * The half of Render/FlowVizVolumeTexture that needs a real device.
 *
 * FlowVizVolumeTextureTest covers every pure function without an RHI, which is
 * most of the file. What it cannot cover is whether this Metal driver actually
 * accepts the formats FlowVizVolumeFormat chooses and the strides
 * FFlowVizVolumeLayout computes - a create call that the driver rejects, or a
 * row pitch it interprets differently, would pass every logic assertion and
 * still produce a blank or sheared volume on screen.
 *
 * Run with:  RHI=1 ./FlowViz/Tools/run_tests.sh FlowViz.Render.VolumeDevice
 *
 * Under the default -nullrhi this test SKIPS WITH A LOGGED REASON rather than
 * passing. A test that silently passes when it did nothing is worse than one
 * that fails: it reports coverage that does not exist.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizVolumeDeviceTest,
	"FlowViz.Render.VolumeDevice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlowVizVolumeDeviceTest::RunTest(const FString& Parameters)
{
	if (!GIsRHIInitialized || GUsingNullRHI)
	{
		// Not a pass. The runner shows this line, so "0 device assertions ran"
		// is visible rather than inferred.
		AddInfo(TEXT("SKIPPED: no RHI device (running under -nullrhi). "
			"Re-run with RHI=1 ./FlowViz/Tools/run_tests.sh FlowViz.Render.VolumeDevice"));
		return true;
	}

	/* == Every format this module can choose must actually create ============ */
	{
		// The formats FlowVizVolumeFormat::ChooseTextureFormat can return. If the
		// device refuses one, the logic tests would still pass and the volume
		// would be blank at runtime - so the question is asked here, of the
		// driver, rather than assumed.
		const ECFDVizDataType DataTypes[3] = {
			ECFDVizDataType::Float16, ECFDVizDataType::Float32, ECFDVizDataType::UInt8
		};

		for (ECFDVizDataType DataType : DataTypes)
		{
			for (int32 Components = 1; Components <= 4; ++Components)
			{
				FFlowVizVolumeLayout Layout;
				const FCFDVizResult Made = FFlowVizVolumeLayout::Make(
					FIntVector(8, 4, 2), Components, DataType, Layout);
				if (!TestTrue(*FString::Printf(TEXT("%s x%d builds a layout"),
					DataTypeToString(DataType), Components), Made.IsOk()))
				{
					continue;
				}

				TestTrue(*FString::Printf(TEXT("this device supports %s x%d as %s"),
						DataTypeToString(DataType), Components,
						GetPixelFormatString(Layout.PixelFormat)),
					FlowVizVolumeRHI::CheckDeviceSupport(Layout).IsOk());
			}
		}
	}

	/* == Create and upload on the real device ================================ */
	{
		// NO TEXEL READBACK. Reading a 3D texture back on Metal needs a staging
		// copy this layer does not have, so this section proves the driver
		// ACCEPTS the create and the upload - not what the shader would sample.
		// The format policy is therefore asserted directly, against the enum,
		// rather than inferred from a round trip: a uint8 field must reach the
		// GPU as PF_R8_UINT, because the UNORM PF_R8 would deliver a stored 1
		// to the shader as 0.00392.
		FFlowVizVolumeLayout Layout;
		if (!TestTrue(TEXT("a uint8 layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(8, 4, 2), 1, ECFDVizDataType::UInt8, Layout).IsOk()))
		{
			return false;
		}

		// A distinct value per voxel, so a transposed or sheared upload cannot
		// coincide with the expected pattern.
		const int32 VoxelCount = static_cast<int32>(Layout.GetVoxelCount());
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(VoxelCount);
		for (int32 Index = 0; Index < VoxelCount; ++Index)
		{
			Bytes[Index] = static_cast<uint8>(Index & 0xFF);
		}

		// The format policy, on the layout the device is about to be handed.
		// Asserted here rather than deduced from a readback that does not exist.
		TestEqual(TEXT("a uint8 field is PF_R8_UINT on the device, never the UNORM PF_R8"),
			static_cast<int32>(Layout.PixelFormat), static_cast<int32>(PF_R8_UINT));

		FTextureRHIRef Texture;
		FCFDVizResult CreateResult;
		int32 ReadWidth = 0;
		int32 ReadHeight = 0;
		int32 ReadDepth = 0;
		EPixelFormat CreatedFormat = PF_Unknown;

		ENQUEUE_RENDER_COMMAND(FlowVizVolumeDeviceTest)(
			[&](FRHICommandListImmediate& RHICmdList)
			{
				Texture = FlowVizVolumeRHI::CreateVolumeTexture(
					RHICmdList, Layout, TEXT("FlowVizVolumeDeviceTest"), CreateResult);
				if (!Texture.IsValid())
				{
					return;
				}

				FlowVizVolumeRHI::UpdateVolumeTexture(RHICmdList, Texture.GetReference(), Layout, Bytes)
					.LogIfFailed();

				// GetSizeXYZ, not the GetSizeX/Y/Z trio: those are marked
				// deprecated in RHIResources.h and read ArraySize rather than
				// Depth for some dimensions.
				const FIntVector ActualSize = Texture->GetSizeXYZ();
				ReadWidth = ActualSize.X;
				ReadHeight = ActualSize.Y;
				ReadDepth = ActualSize.Z;
				CreatedFormat = Texture->GetFormat();
			});
		FlushRenderingCommands();

		TestTrue(TEXT("the device created the 3D texture"), CreateResult.IsOk());
		if (TestTrue(TEXT("and returned a valid texture"), Texture.IsValid()))
		{
			// The extent the driver actually allocated, which is what the shader
			// will sample - not the extent we asked for.
			TestEqual(TEXT("the texture is 8 voxels wide"), ReadWidth, 8);
			TestEqual(TEXT("4 tall"), ReadHeight, 4);
			TestEqual(TEXT("and 2 deep"), ReadDepth, 2);
			// The format the DRIVER allocated, read back off the resource - not
			// the one we asked for. A UNORM slipping through would be sampled as
			// value/255 by every shader and is invisible without this.
			TestEqual(TEXT("and the driver allocated it as PF_R8_UINT"),
				static_cast<int32>(CreatedFormat), static_cast<int32>(PF_R8_UINT));
		}

		// Released on the render thread, then waited for: leaving an RHI
		// reference alive past the test would be reported as a leak by a later,
		// unrelated test.
		ENQUEUE_RENDER_COMMAND(FlowVizVolumeDeviceTestRelease)(
			[&Texture](FRHICommandListImmediate&)
			{
				Texture.SafeRelease();
			});
		FlushRenderingCommands();
	}

	/* == A region update addresses only its own sub-box ====================== */
	{
		// UpdateVolumeTextureRegion is what a future streaming path will use to
		// upload one brick at a time. Its strides are the REGION's, not the
		// volume's, and nothing without a device can check that the driver
		// agrees.
		FFlowVizVolumeLayout Layout;
		if (!TestTrue(TEXT("a region-test layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(8, 4, 2), 1, ECFDVizDataType::UInt8, Layout).IsOk()))
		{
			return false;
		}

		FFlowVizVolumeBrickPlacement Placement;
		Placement.DestOffset = FIntVector(4, 0, 0);
		Placement.ValidSize = FIntVector(4, 4, 2);
		Placement.SourceRowPitch = 4;
		Placement.SourceSlicePitch = 16;
		Placement.SourceBytes = 32;

		TArray<uint8> RegionBytes;
		RegionBytes.Init(0xAB, 32);

		FTextureRHIRef Texture;
		FCFDVizResult CreateResult;
		FCFDVizResult RegionResult;

		ENQUEUE_RENDER_COMMAND(FlowVizVolumeRegionTest)(
			[&](FRHICommandListImmediate& RHICmdList)
			{
				Texture = FlowVizVolumeRHI::CreateVolumeTexture(
					RHICmdList, Layout, TEXT("FlowVizVolumeRegionTest"), CreateResult);
				if (!Texture.IsValid())
				{
					return;
				}

				TArray<uint8> Zeroes;
				Zeroes.Init(0, static_cast<int32>(Layout.GetTextureVolumeBytes()));
				FlowVizVolumeRHI::UpdateVolumeTexture(RHICmdList, Texture.GetReference(), Layout, Zeroes)
					.LogIfFailed();

				RegionResult = FlowVizVolumeRHI::UpdateVolumeTextureRegion(
					RHICmdList, Texture.GetReference(), Layout, Placement, RegionBytes);
			});
		FlushRenderingCommands();

		TestTrue(TEXT("the region update was accepted by the driver"), RegionResult.IsOk());

		// A region reaching past the texture must be refused by US, before the
		// driver's own check() turns it into a crash rather than a message.
		FFlowVizVolumeBrickPlacement TooBig = Placement;
		TooBig.DestOffset = FIntVector(6, 0, 0);
		TooBig.ValidSize = FIntVector(4, 4, 2);

		ENQUEUE_RENDER_COMMAND(FlowVizVolumeRegionOverrun)(
			[&](FRHICommandListImmediate& RHICmdList)
			{
				if (Texture.IsValid())
				{
					RegionResult = FlowVizVolumeRHI::UpdateVolumeTextureRegion(
						RHICmdList, Texture.GetReference(), Layout, TooBig, RegionBytes);
				}
			});
		FlushRenderingCommands();

		TestFalse(TEXT("a region reaching past the texture is refused, not passed to the driver"),
			RegionResult.IsOk());

		ENQUEUE_RENDER_COMMAND(FlowVizVolumeRegionRelease)(
			[&Texture](FRHICommandListImmediate&)
			{
				Texture.SafeRelease();
			});
		FlushRenderingCommands();
	}

	/* == The full path: one real frame onto the GPU =========================== */
	{
		// Three buffers, one payload, through EnqueueUpload - the path the
		// renderer will actually use. Nothing above exercises the slot
		// bookkeeping against real texture creation.
		FFlowVizVolumeLayout Layout;
		if (!TestTrue(TEXT("a set layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(8, 4, 2), 3, ECFDVizDataType::Float16, Layout).IsOk()))
		{
			return false;
		}

		FFlowVizVolumeLayout StatusLayout;
		TestTrue(TEXT("a status layout builds"),
			FFlowVizVolumeLayout::MakeStatusLayout(FIntVector(8, 4, 2), StatusLayout).IsOk());

		FFlowVizVolumeUpload Upload;
		Upload.FrameIndex = 7;
		Upload.SimulationTime = 0.25;
		Upload.VectorLayout = Layout;
		Upload.VectorBytes.Init(0, static_cast<int32>(Layout.GetTextureVolumeBytes()));
		Upload.StatusLayout = StatusLayout;
		Upload.StatusBytes.Init(FlowVizVoxelStatus::Valid, static_cast<int32>(StatusLayout.GetTextureVolumeBytes()));

		TestTrue(TEXT("the payload validates"), Upload.Validate().IsOk());

		FFlowVizVolumeTextureSet Set;
		TestTrue(TEXT("the set initialises"),
			Set.Initialize(FlowVizVolume::RecommendedBufferCount).IsOk());
		TestTrue(TEXT("the upload is accepted"), Set.EnqueueUpload(MoveTemp(Upload)).IsOk());
		TestFalse(TEXT("slot storage cannot be reinitialised while a render-thread upload owns it"),
			Set.Initialize(FlowVizVolume::RecommendedBufferCount).IsOk());

		// The upload happens on the render thread, so nothing above is true yet.
		FlushRenderingCommands();

		const int32 Slot = Set.FindSlotForFrame(7);
		if (TestTrue(TEXT("frame 7 is resident after the flush"), Slot != INDEX_NONE))
		{
			FFlowVizVolumeSlotTextures Textures;
			if (TestTrue(TEXT("the slot has textures"),
				Set.GetSlotTextures(Slot, Textures)))
			{
				TestTrue(TEXT("the vector texture was created on the device"),
					Textures.VectorTexture.IsValid());
				TestTrue(TEXT("the status texture was created on the device"),
					Textures.StatusTexture.IsValid());
				TestFalse(TEXT("and no scalar texture, since the payload had none"),
					Textures.ScalarTexture.IsValid());
				TestEqual(TEXT("the slot remembers the simulation time"),
					Textures.SimulationTime, 0.25);
			}
		}

		Set.ReleaseResources();
		FlushRenderingCommands();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
