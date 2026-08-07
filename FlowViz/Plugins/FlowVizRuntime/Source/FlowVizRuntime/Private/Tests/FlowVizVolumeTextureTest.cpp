// Copyright FlowViz contributors. All Rights Reserved.

#include "Render/FlowVizVolumeTexture.h"

#include "CFDViz/CFDVizByteSource.h"
#include "CFDViz/CFDVizManifest.h"
#include "CFDViz/CFDVizVolumeReader.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The GPU volume data path: dense CVF bytes -> RHI-shaped buffers and the shader
 * parameter block (ADR 002, plan.md section 9).
 *
 * WHY THESE ASSERTIONS AND NOT OTHERS. Every failure this layer can produce is
 * a failure that still renders. A wrong axis order renders a transposed volume;
 * a wrong stride renders a shear; a dropped half-voxel renders the field shifted
 * by half a cell; a mask read at the wrong extent marks the wrong cells invalid
 * and looks like noise in the data. None of them crash, and none of them are
 * visible without knowing what the answer should have been. So:
 *
 *  1. **Expected values are derived independently, not from this code.** The
 *     offsets, UVW coordinates and byte counts below were computed from the
 *     format spec and the committed sample's own manifest, by hand and in
 *     Python, before the implementation existed. A test that asserts
 *     GetTextureVoxelOffset agrees with GetVoxelIndex * GetTextureVoxelBytes is
 *     a restatement of the implementation and passes against any consistent
 *     mistake.
 *
 *  2. **The fixture is asymmetric on every axis.** The sample grid is 56x28x6
 *     with spacing 0.2142857, 0.1428571, 0.1666667 - three different extents and
 *     three different spacings, no two equal and none a multiple of another. A
 *     cube fixture passes against a transposed index, and a cubic-voxel fixture
 *     passes against code that collapses spacing to a scalar.
 *
 *  3. **The mask fixture is the real one.** MockCylinderWake's validMask has
 *     exactly 132 zero cells - the cylinder interior - and U has NaN in exactly
 *     those 132 voxels. That coincidence is what makes the status build
 *     checkable two independent ways: the NaN count must equal the masked count,
 *     and both must equal 132, a number that came out of the file rather than
 *     out of this code.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizVolumeTextureTest,
	"FlowViz.Render.VolumeTexture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace
{
	/** The committed low-resolution sample, which lives beside Plugins/, not inside the plugin. */
	FString GetVolumeSampleCaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"));
	}

	/** The sample's grid, spelled out here rather than read back from the manifest the code under test also reads. */
	FCFDVizGrid MakeSampleGrid()
	{
		FCFDVizGrid Grid;
		Grid.Dimensions = FIntVector(56, 28, 6);
		Grid.Origin = FVector::ZeroVector;
		Grid.Spacing = FVector(
			0.21428571428571427,
			0.14285714285714285,
			0.16666666666666666);
		return Grid;
	}

	/** Little-endian read of one float16 as raw bits, so nothing round-trips through a float. */
	uint16 ReadBits16(const TArray<uint8>& Bytes, int64 Offset)
	{
		return static_cast<uint16>(Bytes[Offset]) | (static_cast<uint16>(Bytes[Offset + 1]) << 8);
	}
}

bool FFlowVizVolumeTextureTest::RunTest(const FString& Parameters)
{
	/* == Format choice: integers must not become UNORMs ====================== */
	{
		FFlowVizVolumeFormatChoice Choice;

		// float16, the sample's storage type for every real field.
		TestTrue(TEXT("float16 x1 has a format"),
			FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::Float16, 1, Choice).IsOk());
		TestEqual(TEXT("float16 x1 -> PF_R16F"), static_cast<int32>(Choice.PixelFormat), static_cast<int32>(PF_R16F));
		TestEqual(TEXT("float16 x1 carries one channel"), Choice.TextureComponentCount, 1);
		TestFalse(TEXT("float16 x1 is not padded"), Choice.bPadded);

		TestTrue(TEXT("float16 x2 has a format"),
			FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::Float16, 2, Choice).IsOk());
		TestEqual(TEXT("float16 x2 -> PF_G16R16F"), static_cast<int32>(Choice.PixelFormat), static_cast<int32>(PF_G16R16F));

		// Three components is the widening case, and the sample's U field.
		TestTrue(TEXT("float16 x3 has a format"),
			FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::Float16, 3, Choice).IsOk());
		TestEqual(TEXT("float16 x3 -> PF_FloatRGBA"), static_cast<int32>(Choice.PixelFormat), static_cast<int32>(PF_FloatRGBA));
		TestEqual(TEXT("float16 x3 widens to FOUR channels"), Choice.TextureComponentCount, 4);
		TestTrue(TEXT("float16 x3 reports that it padded"), Choice.bPadded);

		TestTrue(TEXT("float32 x1 has a format"),
			FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::Float32, 1, Choice).IsOk());
		TestEqual(TEXT("float32 x1 -> PF_R32_FLOAT"), static_cast<int32>(Choice.PixelFormat), static_cast<int32>(PF_R32_FLOAT));

		TestTrue(TEXT("float32 x4 has a format"),
			FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::Float32, 4, Choice).IsOk());
		TestEqual(TEXT("float32 x4 -> PF_A32B32G32R32F"), static_cast<int32>(Choice.PixelFormat), static_cast<int32>(PF_A32B32G32R32F));

		// THE ASSERTION THIS BLOCK EXISTS FOR. PF_R8 is UNORM: a sampler returns
		// value/255. The sample's validMask stores 0 and 1, so through PF_R8 a
		// "1" arrives at the shader as 0.00392 - a plausible small number, and
		// every masked test downstream would still pass because 0 stays 0.
		TestTrue(TEXT("uint8 x1 has a format"),
			FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::UInt8, 1, Choice).IsOk());
		TestEqual(TEXT("uint8 x1 -> PF_R8_UINT, NOT the UNORM PF_R8"),
			static_cast<int32>(Choice.PixelFormat), static_cast<int32>(PF_R8_UINT));
		TestNotEqual(TEXT("uint8 never chooses a UNORM format"),
			static_cast<int32>(Choice.PixelFormat), static_cast<int32>(PF_R8));
		TestNotEqual(TEXT("uint8 never chooses PF_G8 either"),
			static_cast<int32>(Choice.PixelFormat), static_cast<int32>(PF_G8));

		TestTrue(TEXT("uint8 x3 has a format"),
			FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::UInt8, 3, Choice).IsOk());
		TestEqual(TEXT("uint8 x3 -> PF_R8G8B8A8_UINT"), static_cast<int32>(Choice.PixelFormat), static_cast<int32>(PF_R8G8B8A8_UINT));
		TestEqual(TEXT("uint8 x3 widens to four"), Choice.TextureComponentCount, 4);

		// float64 is not a CVF storage type (format section 3.3) and there is no
		// 3D texture format for it. Rejected, not silently narrowed to float32 -
		// narrowing would be the exact quantisation rule 5 forbids.
		FFlowVizVolumeFormatChoice Rejected;
		const FCFDVizResult Float64 = FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::Float64, 1, Rejected);
		TestFalse(TEXT("float64 has no texture format"), Float64.IsOk());
		TestEqual(TEXT("float64 is UnsupportedDataType"),
			static_cast<int32>(Float64.Error), static_cast<int32>(ECFDVizError::UnsupportedDataType));

		TestFalse(TEXT("zero components is rejected"),
			FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::Float16, 0, Rejected).IsOk());
		TestFalse(TEXT("five components is rejected"),
			FlowVizVolumeFormat::ChooseTextureFormat(ECFDVizDataType::Float16, 5, Rejected).IsOk());

		// Pad bit patterns, as absolute constants. A quiet NaN in each float
		// width; zero for uint8, where no NaN exists.
		TestEqual(TEXT("float32 pad is a quiet NaN"), FlowVizVolumeFormat::GetPadElementBits(ECFDVizDataType::Float32), 0x7FC00000u);
		TestEqual(TEXT("float16 pad is a quiet NaN"), FlowVizVolumeFormat::GetPadElementBits(ECFDVizDataType::Float16), 0x7E00u);
		TestEqual(TEXT("uint8 pad is zero"), FlowVizVolumeFormat::GetPadElementBits(ECFDVizDataType::UInt8), 0u);
	}

	/* == Layout: strides and offsets, against hand-computed numbers ========== */
	{
		// The sample's U field: 56x28x6, three float16 components.
		FFlowVizVolumeLayout Layout;
		const FCFDVizResult Made = FFlowVizVolumeLayout::Make(
			FIntVector(56, 28, 6), 3, ECFDVizDataType::Float16, Layout);
		TestTrue(TEXT("the sample's U layout is buildable"), Made.IsOk());
		TestTrue(TEXT("and reports itself valid"), Layout.IsValid());

		TestEqual(TEXT("source keeps three components"), Layout.SourceComponentCount, 3);
		TestEqual(TEXT("texture carries four"), Layout.TextureComponentCount, 4);
		TestEqual(TEXT("element is two bytes"), Layout.GetElementBytes(), 2);

		// SOURCE and TEXTURE strides differ here, which is the whole reason they
		// are named apart. 3*2 = 6 versus 4*2 = 8.
		TestEqual(TEXT("source voxel is 6 bytes"), Layout.GetSourceVoxelBytes(), (int64)6);
		TestEqual(TEXT("texture voxel is 8 bytes"), Layout.GetTextureVoxelBytes(), (int64)8);

		TestEqual(TEXT("voxel count is 56*28*6"), Layout.GetVoxelCount(), (int64)9408);

		// X is the fastest axis (format section 4.4.1). A row is X long, a slice
		// is X*Y. Swapping the axes here is the transposition bug and it renders.
		TestEqual(TEXT("source row pitch is 56*6"), Layout.GetSourceRowPitch(), (int64)336);
		TestEqual(TEXT("source slice pitch is 56*28*6"), Layout.GetSourceSlicePitch(), (int64)9408);
		TestEqual(TEXT("source volume is 56*28*6*6"), Layout.GetSourceVolumeBytes(), (int64)56448);

		TestEqual(TEXT("texture row pitch is 56*8"), Layout.GetTextureRowPitch(), (int64)448);
		TestEqual(TEXT("texture slice pitch is 56*28*8"), Layout.GetTextureSlicePitch(), (int64)12544);
		TestEqual(TEXT("texture volume is 56*28*6*8"), Layout.GetTextureVolumeBytes(), (int64)75264);

		// Hand-computed: 30 + 56*(14 + 28*3) = 30 + 56*98 = 5518.
		TestEqual(TEXT("voxel index (30,14,3) is 5518"), Layout.GetVoxelIndex(30, 14, 3), (int64)5518);
		TestEqual(TEXT("source offset (30,14,3) is 5518*6"), Layout.GetSourceVoxelOffset(30, 14, 3), (int64)33108);
		TestEqual(TEXT("texture offset (30,14,3) is 5518*8"), Layout.GetTextureVoxelOffset(30, 14, 3), (int64)44144);

		// Asymmetric probes: each of these lands somewhere different under a
		// transposed index, which a symmetric (n,n,n) probe would not.
		TestEqual(TEXT("voxel index (1,0,0) is 1"), Layout.GetVoxelIndex(1, 0, 0), (int64)1);
		TestEqual(TEXT("voxel index (0,1,0) is 56"), Layout.GetVoxelIndex(0, 1, 0), (int64)56);
		TestEqual(TEXT("voxel index (0,0,1) is 1568"), Layout.GetVoxelIndex(0, 0, 1), (int64)1568);
		TestEqual(TEXT("last voxel (55,27,5) is 9407"), Layout.GetVoxelIndex(55, 27, 5), (int64)9407);

		// Out of range is INDEX_NONE, never a wrapped-but-plausible offset. Each
		// of these is in range on the OTHER two axes, so a check that tests only
		// the product would let them through.
		TestEqual(TEXT("X past the end is rejected"), Layout.GetVoxelIndex(56, 0, 0), (int64)INDEX_NONE);
		TestEqual(TEXT("Y past the end is rejected"), Layout.GetVoxelIndex(0, 28, 0), (int64)INDEX_NONE);
		TestEqual(TEXT("Z past the end is rejected"), Layout.GetVoxelIndex(0, 0, 6), (int64)INDEX_NONE);
		TestEqual(TEXT("a negative index is rejected"), Layout.GetVoxelIndex(-1, 0, 0), (int64)INDEX_NONE);
		TestEqual(TEXT("out of range source offset is rejected"), Layout.GetSourceVoxelOffset(56, 0, 0), (int64)INDEX_NONE);

		// A single-component layout has equal strides, which is the case where
		// confusing the two strides costs nothing and therefore hides.
		FFlowVizVolumeLayout Scalar;
		TestTrue(TEXT("a scalar float16 layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(56, 28, 6), 1, ECFDVizDataType::Float16, Scalar).IsOk());
		TestEqual(TEXT("scalar does not widen"), Scalar.TextureComponentCount, 1);
		TestEqual(TEXT("scalar volume is 56*28*6*2"), Scalar.GetTextureVolumeBytes(), (int64)18816);
		TestTrue(TEXT("scalar and vector layouts share an extent"), Scalar.HasSameExtent(Layout));

		// The status layout: one uint8 per voxel, whatever the field carries.
		FFlowVizVolumeLayout Status;
		TestTrue(TEXT("a status layout builds"),
			FFlowVizVolumeLayout::MakeStatusLayout(FIntVector(56, 28, 6), Status).IsOk());
		TestEqual(TEXT("status is PF_R8_UINT"), static_cast<int32>(Status.PixelFormat), static_cast<int32>(PF_R8_UINT));
		TestEqual(TEXT("status is one byte per voxel"), Status.GetTextureVolumeBytes(), (int64)9408);
		TestTrue(TEXT("status shares the field's extent"), Status.HasSameExtent(Layout));

		// Rejections. Each names one quantity; a degenerate or oversized extent
		// must not reach an allocation.
		FFlowVizVolumeLayout Bad;
		TestFalse(TEXT("a zero extent is rejected"),
			FFlowVizVolumeLayout::Make(FIntVector(0, 28, 6), 1, ECFDVizDataType::Float16, Bad).IsOk());
		TestFalse(TEXT("a negative extent is rejected"),
			FFlowVizVolumeLayout::Make(FIntVector(56, -1, 6), 1, ECFDVizDataType::Float16, Bad).IsOk());
		TestFalse(TEXT("an extent past MaxTextureDimension is rejected"),
			FFlowVizVolumeLayout::Make(FIntVector(FlowVizVolume::MaxTextureDimension + 1, 4, 4), 1, ECFDVizDataType::Float16, Bad).IsOk());
		TestTrue(TEXT("an extent exactly at MaxTextureDimension is accepted"),
			FFlowVizVolumeLayout::Make(FIntVector(FlowVizVolume::MaxTextureDimension, 4, 4), 1, ECFDVizDataType::UInt8, Bad).IsOk());
		TestFalse(TEXT("a default-constructed layout is not valid"), FFlowVizVolumeLayout().IsValid());

		// The MaxBytes budget must bite BEFORE anything allocates. 75263 is one
		// byte under the sample U field's texture size.
		FFlowVizVolumeLayout Budgeted;
		const FCFDVizResult TooBig = FFlowVizVolumeLayout::Make(
			FIntVector(56, 28, 6), 3, ECFDVizDataType::Float16, Budgeted, 75263);
		TestFalse(TEXT("a budget one byte short rejects the layout"), TooBig.IsOk());
		TestEqual(TEXT("and reports AllocationTooLarge"),
			static_cast<int32>(TooBig.Error), static_cast<int32>(ECFDVizError::AllocationTooLarge));
		TestTrue(TEXT("a budget of exactly the texture size is accepted"),
			FFlowVizVolumeLayout::Make(FIntVector(56, 28, 6), 3, ECFDVizDataType::Float16, Budgeted, 75264).IsOk());
	}

	/* == Brick placement: edge bricks are NOT padded ========================= */
	{
		FFlowVizVolumeLayout Layout;
		TestTrue(TEXT("layout for brick tests"),
			FFlowVizVolumeLayout::Make(FIntVector(56, 28, 6), 1, ECFDVizDataType::UInt8, Layout).IsOk());

		// The sample's tiling: 32x32x8 over 56x28x6, so ceil gives 2x1x1 bricks
		// and EVERY brick is an edge brick on at least one axis.
		const FIntVector BrickSize(32, 32, 8);
		const FIntVector Counts = FlowVizVolumeBrick::GetBrickCounts(Layout, BrickSize);
		TestEqual(TEXT("ceil(56/32) is 2 bricks on X"), Counts.X, 2);
		TestEqual(TEXT("ceil(28/32) is 1 brick on Y"), Counts.Y, 1);
		TestEqual(TEXT("ceil(6/8) is 1 brick on Z"), Counts.Z, 1);

		FFlowVizVolumeBrickPlacement First;
		TestTrue(TEXT("brick (0,0,0) places"),
			FlowVizVolumeBrick::MakePlacement(Layout, FIntVector(0, 0, 0), BrickSize, First).IsOk());
		TestEqual(TEXT("brick 0 starts at the origin"), First.DestOffset, FIntVector(0, 0, 0));
		// 32 on X because the volume is wider than one brick; 28 and 6 because
		// the volume is SHORTER than a brick on Y and Z. These came out of the
		// real file's directory, not out of this code.
		TestEqual(TEXT("brick 0 valid size is (32,28,6), clipped on Y and Z"), First.ValidSize, FIntVector(32, 28, 6));
		TestEqual(TEXT("brick 0 is 32*28*6 bytes"), First.SourceBytes, (int64)5376);
		TestEqual(TEXT("brick 0 row pitch is its OWN width, 32"), First.SourceRowPitch, (int64)32);
		TestEqual(TEXT("brick 0 slice pitch is 32*28"), First.SourceSlicePitch, (int64)896);

		FFlowVizVolumeBrickPlacement Second;
		TestTrue(TEXT("brick (1,0,0) places"),
			FlowVizVolumeBrick::MakePlacement(Layout, FIntVector(1, 0, 0), BrickSize, Second).IsOk());
		TestEqual(TEXT("brick 1 starts at x=32"), Second.DestOffset, FIntVector(32, 0, 0));
		// 56 - 32 = 24, NOT 32. Treating this as a full brick reads 8 columns
		// past the decoded payload and shears every row after it.
		TestEqual(TEXT("brick 1 valid size is (24,28,6) - the remainder, not a full brick"),
			Second.ValidSize, FIntVector(24, 28, 6));
		TestEqual(TEXT("brick 1 is 24*28*6 bytes"), Second.SourceBytes, (int64)4032);
		TestEqual(TEXT("brick 1 row pitch is 24, not 32"), Second.SourceRowPitch, (int64)24);

		TestFalse(TEXT("a brick outside the tiling is rejected"),
			FlowVizVolumeBrick::MakePlacement(Layout, FIntVector(2, 0, 0), BrickSize, Second).IsOk());
		TestFalse(TEXT("a zero brick size is rejected"),
			FlowVizVolumeBrick::MakePlacement(Layout, FIntVector(0, 0, 0), FIntVector(0, 8, 8), Second).IsOk());

		/* -- Copying a brick into the dense volume -------------------------- */
		//
		// A synthetic brick whose every byte is its own destination index, so a
		// misplaced byte is detectable at every position rather than only at a
		// boundary. A constant-filled brick would pass against any offset.
		FFlowVizVolumeLayout Small;
		TestTrue(TEXT("a small layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(5, 3, 2), 1, ECFDVizDataType::UInt8, Small).IsOk());

		const FIntVector SmallBrick(4, 2, 2);
		FFlowVizVolumeBrickPlacement Edge;
		TestTrue(TEXT("the edge brick of the small volume places"),
			FlowVizVolumeBrick::MakePlacement(Small, FIntVector(1, 0, 0), SmallBrick, Edge).IsOk());
		// 5 - 4 = 1 on X; 3 - 0 = 3 clipped to the brick's 2 on Y; 2 on Z.
		TestEqual(TEXT("the small edge brick is 1x2x2"), Edge.ValidSize, FIntVector(1, 2, 2));
		TestEqual(TEXT("and holds 4 bytes"), Edge.SourceBytes, (int64)4);

		TArray<uint8> BrickBytes;
		BrickBytes.SetNumUninitialized(4);
		BrickBytes[0] = 0xA0; BrickBytes[1] = 0xA1; BrickBytes[2] = 0xA2; BrickBytes[3] = 0xA3;

		TArray<uint8> Dense;
		Dense.Init(0, 30);
		TestTrue(TEXT("the edge brick copies in"),
			FlowVizVolumeBrick::CopyBrickIntoDense(Small, Edge, BrickBytes, Dense).IsOk());

		// Destinations computed by hand from X-fastest ordering over a 5x3x2
		// volume: (4,0,0)=4, (4,1,0)=9, (4,0,1)=19, (4,1,1)=24.
		TestEqual(TEXT("brick byte 0 lands at dense 4"), (int32)Dense[4], 0xA0);
		TestEqual(TEXT("brick byte 1 lands at dense 9"), (int32)Dense[9], 0xA1);
		TestEqual(TEXT("brick byte 2 lands at dense 19"), (int32)Dense[19], 0xA2);
		TestEqual(TEXT("brick byte 3 lands at dense 24"), (int32)Dense[24], 0xA3);

		// And nothing else moved - a copy that wrote a whole brick-shaped block
		// would smear into neighbours that this counts.
		int32 NonZero = 0;
		for (uint8 Byte : Dense)
		{
			NonZero += (Byte != 0) ? 1 : 0;
		}
		TestEqual(TEXT("exactly four bytes were written"), NonZero, 4);

		// A short brick buffer is refused before the first write.
		TArray<uint8> Short;
		Short.Init(0xFF, 3);
		TArray<uint8> Untouched;
		Untouched.Init(0, 30);
		const FCFDVizResult ShortResult = FlowVizVolumeBrick::CopyBrickIntoDense(Small, Edge, Short, Untouched);
		TestFalse(TEXT("a short brick buffer is rejected"), ShortResult.IsOk());
		TestEqual(TEXT("as SizeMismatch"),
			static_cast<int32>(ShortResult.Error), static_cast<int32>(ECFDVizError::SizeMismatch));
		bool bAllZero = true;
		for (uint8 Byte : Untouched)
		{
			bAllZero = bAllZero && (Byte == 0);
		}
		TestTrue(TEXT("and the destination was not partially written"), bAllZero);
	}

	/* == Component widening: stored bytes untouched, pad is NaN ============== */
	{
		FFlowVizVolumeLayout Layout;
		TestTrue(TEXT("a 3-component float16 layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(2, 1, 1), 3, ECFDVizDataType::Float16, Layout).IsOk());

		// Two voxels, six distinct float16 bit patterns. Distinct so a reordering
		// or a dropped component is visible; a buffer of equal values is not.
		const uint16 Source[6] = { 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006 };
		TArray<uint8> SourceBytes;
		SourceBytes.SetNumUninitialized(12);
		for (int32 Index = 0; Index < 6; ++Index)
		{
			SourceBytes[Index * 2 + 0] = static_cast<uint8>(Source[Index] & 0xFF);
			SourceBytes[Index * 2 + 1] = static_cast<uint8>(Source[Index] >> 8);
		}

		TArray<uint8> Widened;
		// Guarded: the sixteen reads below are at fixed offsets into this buffer,
		// so a failure here (or a short buffer from a bad pitch) would trip
		// TArray's bounds assert and kill the editor rather than fail the test.
		const bool bWidened = TestTrue(TEXT("widening succeeds"),
			FlowVizVolumeConvert::ExpandComponents(Layout, SourceBytes, Widened).IsOk());
		if (!bWidened
			|| !TestEqual(TEXT("the widened buffer is 2 voxels * 4 channels * 2 bytes"), Widened.Num(), 16))
		{
			return false;
		}

		// Every stored component survives, in order, bit for bit.
		TestEqual(TEXT("voxel 0 component 0 survives"), ReadBits16(Widened, 0), (uint16)0x0001);
		TestEqual(TEXT("voxel 0 component 1 survives"), ReadBits16(Widened, 2), (uint16)0x0002);
		TestEqual(TEXT("voxel 0 component 2 survives"), ReadBits16(Widened, 4), (uint16)0x0003);
		TestEqual(TEXT("voxel 1 component 0 survives"), ReadBits16(Widened, 8), (uint16)0x0004);
		TestEqual(TEXT("voxel 1 component 1 survives"), ReadBits16(Widened, 10), (uint16)0x0005);
		TestEqual(TEXT("voxel 1 component 2 survives"), ReadBits16(Widened, 12), (uint16)0x0006);

		// The pad channel is a quiet NaN, not zero. Zero is what a shader that
		// forgets the ComponentCount gate would read as a plausible value.
		TestEqual(TEXT("voxel 0 pad channel is a quiet NaN"), ReadBits16(Widened, 6), (uint16)0x7E00);
		TestEqual(TEXT("voxel 1 pad channel is a quiet NaN"), ReadBits16(Widened, 14), (uint16)0x7E00);

		// A short source is refused and leaves nothing behind.
		TArray<uint8> ShortSource;
		ShortSource.Init(0, 11);
		TArray<uint8> Empty;
		TestFalse(TEXT("a short source is rejected"),
			FlowVizVolumeConvert::ExpandComponents(Layout, ShortSource, Empty).IsOk());
		TestEqual(TEXT("and the output is emptied, not half written"), Empty.Num(), 0);

		// The no-widening path still produces a correct buffer.
		FFlowVizVolumeLayout Scalar;
		TestTrue(TEXT("a scalar layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(2, 1, 1), 1, ECFDVizDataType::UInt8, Scalar).IsOk());
		TArray<uint8> Pair;
		Pair.Add(0x11);
		Pair.Add(0x22);
		TArray<uint8> Copied;
		TestTrue(TEXT("the no-widening path succeeds"),
			FlowVizVolumeConvert::ExpandComponents(Scalar, Pair, Copied).IsOk());
		TestEqual(TEXT("and copies exactly"), Copied.Num(), 2);
		TestEqual(TEXT("byte 0"), (int32)Copied[0], 0x11);
		TestEqual(TEXT("byte 1"), (int32)Copied[1], 0x22);
	}

	/* == Element classification, on raw bits ================================ */
	{
		// float16: quiet NaN, signalling NaN, +inf, -inf, a finite value, and
		// the largest finite float16 (0x7BFF) which is one bit below +inf and is
		// what a >= comparison on the exponent alone would wrongly call infinite.
		const uint8 QuietNaN16[2] = { 0x00, 0x7E };
		const uint8 SignalNaN16[2] = { 0x01, 0x7C };
		const uint8 PosInf16[2] = { 0x00, 0x7C };
		const uint8 NegInf16[2] = { 0x00, 0xFC };
		const uint8 MaxFinite16[2] = { 0xFF, 0x7B };
		const uint8 One16[2] = { 0x00, 0x3C };

		TestEqual(TEXT("float16 quiet NaN classifies as NaN"),
			FlowVizVolumeStatus::ClassifyElement(QuietNaN16, ECFDVizDataType::Float16), FlowVizVoxelStatus::NaN);
		TestEqual(TEXT("float16 signalling NaN classifies as NaN"),
			FlowVizVolumeStatus::ClassifyElement(SignalNaN16, ECFDVizDataType::Float16), FlowVizVoxelStatus::NaN);
		TestEqual(TEXT("float16 +inf classifies as Infinite, not NaN"),
			FlowVizVolumeStatus::ClassifyElement(PosInf16, ECFDVizDataType::Float16), FlowVizVoxelStatus::Infinite);
		TestEqual(TEXT("float16 -inf classifies as Infinite"),
			FlowVizVolumeStatus::ClassifyElement(NegInf16, ECFDVizDataType::Float16), FlowVizVoxelStatus::Infinite);
		TestEqual(TEXT("the largest finite float16 is finite"),
			FlowVizVolumeStatus::ClassifyElement(MaxFinite16, ECFDVizDataType::Float16), (uint8)0);
		TestEqual(TEXT("float16 1.0 is finite"),
			FlowVizVolumeStatus::ClassifyElement(One16, ECFDVizDataType::Float16), (uint8)0);

		const uint8 QuietNaN32[4] = { 0x00, 0x00, 0xC0, 0x7F };
		const uint8 PosInf32[4] = { 0x00, 0x00, 0x80, 0x7F };
		const uint8 MaxFinite32[4] = { 0xFF, 0xFF, 0x7F, 0x7F };
		TestEqual(TEXT("float32 quiet NaN classifies as NaN"),
			FlowVizVolumeStatus::ClassifyElement(QuietNaN32, ECFDVizDataType::Float32), FlowVizVoxelStatus::NaN);
		TestEqual(TEXT("float32 +inf classifies as Infinite"),
			FlowVizVolumeStatus::ClassifyElement(PosInf32, ECFDVizDataType::Float32), FlowVizVoxelStatus::Infinite);
		TestEqual(TEXT("the largest finite float32 is finite"),
			FlowVizVolumeStatus::ClassifyElement(MaxFinite32, ECFDVizDataType::Float32), (uint8)0);

		// An integer has no NaN and no infinity. 0xFF must not be mistaken for
		// one by a classifier that looks only at the high bits.
		const uint8 IntMax[1] = { 0xFF };
		TestEqual(TEXT("uint8 0xFF is finite - integers have no NaN"),
			FlowVizVolumeStatus::ClassifyElement(IntMax, ECFDVizDataType::UInt8), (uint8)0);
	}

	/* == Status bits: zero is Unknown, not Valid ============================ */
	{
		// The one property the whole invalid-value policy rests on. If Valid were
		// 0, an unwritten or failed-upload texture would read as fully valid and
		// the shader would draw a plausible field of zeroes.
		TestEqual(TEXT("Unknown is zero"), FlowVizVoxelStatus::Unknown, (uint8)0);
		TestNotEqual(TEXT("Valid is NOT zero"), FlowVizVoxelStatus::Valid, (uint8)0);
		TestTrue(TEXT("every named bit is distinct"),
			(FlowVizVoxelStatus::Valid & FlowVizVoxelStatus::NaN) == 0
				&& (FlowVizVoxelStatus::Valid & FlowVizVoxelStatus::Infinite) == 0
				&& (FlowVizVoxelStatus::Valid & FlowVizVoxelStatus::Masked) == 0
				&& (FlowVizVoxelStatus::NaN & FlowVizVoxelStatus::Infinite) == 0
				&& (FlowVizVoxelStatus::NaN & FlowVizVoxelStatus::Masked) == 0
				&& (FlowVizVoxelStatus::Infinite & FlowVizVoxelStatus::Masked) == 0);
		TestEqual(TEXT("KnownBits is the union of the four"),
			FlowVizVoxelStatus::KnownBits,
			(uint8)(FlowVizVoxelStatus::Valid | FlowVizVoxelStatus::NaN | FlowVizVoxelStatus::Infinite | FlowVizVoxelStatus::Masked));
	}

	/* == Status build, on a hand-built 3-component field ==================== */
	{
		// Four voxels, three float16 components each. Voxel 0 finite and kept;
		// voxel 1 has ONE NaN component out of three - the case that must mark
		// the whole voxel, since a velocity with one NaN has neither a usable
		// magnitude nor a usable direction; voxel 2 finite but masked off; voxel
		// 3 finite and kept.
		FFlowVizVolumeLayout Field;
		TestTrue(TEXT("a 4-voxel vector layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(4, 1, 1), 3, ECFDVizDataType::Float16, Field).IsOk());
		FFlowVizVolumeLayout Status;
		TestTrue(TEXT("its status layout builds"),
			FFlowVizVolumeLayout::MakeStatusLayout(FIntVector(4, 1, 1), Status).IsOk());

		const uint16 Bits[12] = {
			0x3C00, 0x3C00, 0x3C00,   // voxel 0: 1,1,1
			0x3C00, 0x7E00, 0x3C00,   // voxel 1: middle component NaN
			0x3C00, 0x3C00, 0x3C00,   // voxel 2: finite, but masked below
			0x3C00, 0x7C00, 0x3C00    // voxel 3: middle component +inf
		};
		TArray<uint8> FieldBytes;
		FieldBytes.SetNumUninitialized(24);
		for (int32 Index = 0; Index < 12; ++Index)
		{
			FieldBytes[Index * 2 + 0] = static_cast<uint8>(Bits[Index] & 0xFF);
			FieldBytes[Index * 2 + 1] = static_cast<uint8>(Bits[Index] >> 8);
		}

		TArray<uint8> MaskBytes;
		MaskBytes.Add(1);
		MaskBytes.Add(1);
		MaskBytes.Add(0);   // voxel 2 rejected
		MaskBytes.Add(1);

		FFlowVizVolumeStatusSource Source;
		Source.FieldBytes = FieldBytes;
		Source.MaskBytes = MaskBytes;

		TArray<uint8> Out;
		TestTrue(TEXT("status builds"), FlowVizVolumeStatus::Build(Field, Status, Source, Out).IsOk());
		TestEqual(TEXT("one status byte per voxel"), Out.Num(), 4);

		TestEqual(TEXT("voxel 0 is Valid and nothing else"), Out[0], FlowVizVoxelStatus::Valid);
		TestEqual(TEXT("voxel 1 is NaN, and NOT Valid"), Out[1], FlowVizVoxelStatus::NaN);
		TestEqual(TEXT("voxel 2 is Masked, and NOT Valid"), Out[2], FlowVizVoxelStatus::Masked);
		TestEqual(TEXT("voxel 3 is Infinite, and NOT Valid"), Out[3], FlowVizVoxelStatus::Infinite);

		// Stated separately: these are the assertions that fail when someone
		// makes Valid the default and clears it on error, which is the natural
		// but wrong way to write this loop.
		TestTrue(TEXT("a partially-NaN vector is not Valid"), (Out[1] & FlowVizVoxelStatus::Valid) == 0);
		TestTrue(TEXT("a masked voxel is not Valid"), (Out[2] & FlowVizVoxelStatus::Valid) == 0);

		// The field bytes must NOT have been touched: the stored NaN payload has
		// to reach the GPU bit-exact.
		TestEqual(TEXT("the field's NaN bits are unmodified"), ReadBits16(FieldBytes, 8), (uint16)0x7E00);

		const FlowVizVolumeStatus::FCounts Counts = FlowVizVolumeStatus::CountStatus(Out);
		TestEqual(TEXT("4 voxels counted"), Counts.Total, (int64)4);
		TestEqual(TEXT("1 valid"), Counts.Valid, (int64)1);
		TestEqual(TEXT("1 NaN"), Counts.NaN, (int64)1);
		TestEqual(TEXT("1 infinite"), Counts.Infinite, (int64)1);
		TestEqual(TEXT("1 masked"), Counts.Masked, (int64)1);

		// No mask at all means nothing is Masked - not that everything is.
		FFlowVizVolumeStatusSource NoMask;
		NoMask.FieldBytes = FieldBytes;
		TArray<uint8> Unmasked;
		TestTrue(TEXT("status builds without a mask"),
			FlowVizVolumeStatus::Build(Field, Status, NoMask, Unmasked).IsOk());
		TestEqual(TEXT("voxel 2 is now Valid, since nothing rejected it"), Unmasked[2], FlowVizVoxelStatus::Valid);

		// A mask of the wrong length is rejected, not truncated. Truncating would
		// silently unmask the tail of the volume.
		FFlowVizVolumeStatusSource ShortMask;
		ShortMask.FieldBytes = FieldBytes;
		TArray<uint8> ThreeBytes;
		ThreeBytes.Init(1, 3);
		ShortMask.MaskBytes = ThreeBytes;
		TArray<uint8> Rejected;
		TestFalse(TEXT("a short mask is rejected"),
			FlowVizVolumeStatus::Build(Field, Status, ShortMask, Rejected).IsOk());
		TestEqual(TEXT("and the output is emptied"), Rejected.Num(), 0);

		// A status layout for a different extent is rejected, which is the
		// cell-count-versus-value-count bug expressed as a size check.
		FFlowVizVolumeLayout WrongStatus;
		TestTrue(TEXT("a 5-voxel status layout builds"),
			FFlowVizVolumeLayout::MakeStatusLayout(FIntVector(5, 1, 1), WrongStatus).IsOk());
		TArray<uint8> Mismatched;
		TestFalse(TEXT("a status layout of the wrong extent is rejected"),
			FlowVizVolumeStatus::Build(Field, WrongStatus, Source, Mismatched).IsOk());
	}

	/* == Transform: the half-voxel, and three independent spacings =========== */
	{
		FFlowVizVolumeTransform Cell;
		Cell.Grid = MakeSampleGrid();
		Cell.Association = ECFDVizAssociation::Cell;
		TestTrue(TEXT("the sample transform is valid"), Cell.IsValid());

		TestEqual(TEXT("a cell field's extent is the cell count"), Cell.GetValueCounts(), FIntVector(56, 28, 6));

		// 56 * 0.2142857... = 12, 28 * 0.1428571... = 4, 6 * 0.1666667... = 1.
		// The ADR's "12 m x 4 m x 1 m" domain, at this sample's resolution.
		const FVector Size = Cell.GetPhysicalSize();
		TestTrue(TEXT("physical size X is 12 m"), FMath::IsNearlyEqual(Size.X, 12.0, 1e-9));
		TestTrue(TEXT("physical size Y is 4 m"), FMath::IsNearlyEqual(Size.Y, 4.0, 1e-9));
		TestTrue(TEXT("physical size Z is 1 m"), FMath::IsNearlyEqual(Size.Z, 1.0, 1e-9));

		// THE HALF-VOXEL, ASSERTED AS AN ABSOLUTE POSITION. Cell centre (30,14,3)
		// is at Origin + Spacing*(30.5, 14.5, 3.5), and PhysicalToVoxel must map
		// it back to exactly (30,14,3). Code that omits the -0.5 returns
		// (30.5,14.5,3.5) - which is a legal-looking voxel coordinate and renders
		// the whole field shifted half a cell.
		const FVector CellCentre = Cell.Grid.CellCenter(30, 14, 3);
		TestTrue(TEXT("cell centre X is 30.5 * spacing"),
			FMath::IsNearlyEqual(CellCentre.X, 6.5357142857142856, 1e-12));
		const FVector Voxel = Cell.PhysicalToVoxel(CellCentre);
		TestTrue(TEXT("a cell centre maps to an INTEGER voxel coordinate on X"),
			FMath::IsNearlyEqual(Voxel.X, 30.0, 1e-9));
		TestTrue(TEXT("...on Y"), FMath::IsNearlyEqual(Voxel.Y, 14.0, 1e-9));
		TestTrue(TEXT("...and on Z"), FMath::IsNearlyEqual(Voxel.Z, 3.0, 1e-9));

		// The grid ORIGIN is not a cell centre; it is half a cell below one. So
		// it must map to (-0.5,-0.5,-0.5) for a cell field. This is the
		// assertion that fails when the half-voxel is applied with the wrong
		// sign - which a cell-centre round trip alone would not catch.
		const FVector OriginVoxel = Cell.PhysicalToVoxel(Cell.Grid.Origin);
		TestTrue(TEXT("the grid origin is half a cell BELOW voxel 0"),
			FMath::IsNearlyEqual(OriginVoxel.X, -0.5, 1e-12));

		// UVW: voxel i lands at the centre of texel i. (30+0.5)/56.
		const FVector UVW = Cell.PhysicalToTextureUVW(CellCentre);
		TestTrue(TEXT("UVW X is (30.5)/56"), FMath::IsNearlyEqual(UVW.X, 0.5446428571428571, 1e-12));
		TestTrue(TEXT("UVW Y is (14.5)/28"), FMath::IsNearlyEqual(UVW.Y, 0.5178571428571429, 1e-12));
		TestTrue(TEXT("UVW Z is (3.5)/6"), FMath::IsNearlyEqual(UVW.Z, 0.5833333333333334, 1e-12));

		// The first and last texel centres, which pin the ends of the mapping.
		const FVector FirstUVW = Cell.PhysicalToTextureUVW(Cell.Grid.CellCenter(0, 0, 0));
		TestTrue(TEXT("voxel 0 is at UVW 0.5/56, not 0"), FMath::IsNearlyEqual(FirstUVW.X, 0.5 / 56.0, 1e-12));
		const FVector LastUVW = Cell.PhysicalToTextureUVW(Cell.Grid.CellCenter(55, 27, 5));
		TestTrue(TEXT("the last voxel is at UVW 55.5/56, not 1"), FMath::IsNearlyEqual(LastUVW.X, 55.5 / 56.0, 1e-12));

		// Inverses. Asserted as inverses only - the absolute values above are
		// what prove the mapping is right; a round trip alone would pass with the
		// half-voxel missing from both directions.
		const FVector BackFromUVW = Cell.TextureUVWToPhysical(UVW);
		TestTrue(TEXT("UVW inverts"), BackFromUVW.Equals(CellCentre, 1e-9));
		TestTrue(TEXT("voxel inverts"), Cell.VoxelToPhysical(Voxel).Equals(CellCentre, 1e-9));

		/* -- A point-associated field is one voxel larger per axis ---------- */
		FFlowVizVolumeTransform Point;
		Point.Grid = MakeSampleGrid();
		Point.Association = ECFDVizAssociation::Point;
		TestEqual(TEXT("a point field's extent is cells + 1 per axis"),
			Point.GetValueCounts(), FIntVector(57, 29, 7));
		TestTrue(TEXT("but the physical size is unchanged"),
			Point.GetPhysicalSize().Equals(Cell.GetPhysicalSize(), 1e-12));

		// A point value sits ON the grid point, so there is no half-cell offset
		// on the way in - grid point 0 IS the origin and maps to voxel 0.
		const FVector PointVoxel = Point.PhysicalToVoxel(Point.Grid.Origin);
		TestTrue(TEXT("a point field maps the origin to voxel 0, not -0.5"),
			FMath::IsNearlyEqual(PointVoxel.X, 0.0, 1e-12));
		// ...but it still lands at the CENTRE of texel 0, over 57 texels.
		const FVector PointUVW = Point.PhysicalToTextureUVW(Point.Grid.Origin);
		TestTrue(TEXT("and at UVW 0.5/57"), FMath::IsNearlyEqual(PointUVW.X, 0.5 / 57.0, 1e-12));

		/* -- The shader parameter block ------------------------------------- */
		FFlowVizVolumeLayout Layout;
		TestTrue(TEXT("the U layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(56, 28, 6), 3, ECFDVizDataType::Float16, Layout).IsOk());

		FFlowVizVolumeShaderParameters Params;
		TestTrue(TEXT("shader parameters build"), Cell.MakeShaderParametersWithoutValueRange(Layout, Params).IsOk());

		TestEqual(TEXT("dimensions are the value counts"), Params.VolumeDimensions, FIntVector(56, 28, 6));
		TestEqual(TEXT("ComponentCount is the field's 3, not the texture's 4"), Params.ComponentCount, 3);
		TestEqual(TEXT("TextureComponentCount is 4"), Params.TextureComponentCount, 4);

		// THREE DIFFERENT SPACINGS. Any code that collapses these to a scalar
		// produces a volume that is 1.5x too wide on one axis and 0.78x on
		// another - which still looks like a wake.
		TestTrue(TEXT("spacing X"), FMath::IsNearlyEqual((double)Params.VoxelSpacing.X, 0.21428571428571427, 1e-7));
		TestTrue(TEXT("spacing Y"), FMath::IsNearlyEqual((double)Params.VoxelSpacing.Y, 0.14285714285714285, 1e-7));
		TestTrue(TEXT("spacing Z"), FMath::IsNearlyEqual((double)Params.VoxelSpacing.Z, 0.16666666666666666, 1e-7));
		TestTrue(TEXT("the three spacings are genuinely different"),
			Params.VoxelSpacing.X != Params.VoxelSpacing.Y
				&& Params.VoxelSpacing.Y != Params.VoxelSpacing.Z
				&& Params.VoxelSpacing.X != Params.VoxelSpacing.Z);

		// Min is Y and max is X here - NOT the first and last components, which
		// is what a min/max that forgot to compare all three would produce.
		TestTrue(TEXT("MinVoxelSpacing is the Y spacing"),
			FMath::IsNearlyEqual((double)Params.MinVoxelSpacing, 0.14285714285714285, 1e-7));
		TestTrue(TEXT("MaxVoxelSpacing is the X spacing"),
			FMath::IsNearlyEqual((double)Params.MaxVoxelSpacing, 0.21428571428571427, 1e-7));

		TestTrue(TEXT("VoxelVolume is Sx*Sy*Sz"),
			FMath::IsNearlyEqual((double)Params.VoxelVolume, 0.0051020408163265293, 1e-9));
		TestTrue(TEXT("InvVoxelSpacing X is 1/0.214..."),
			FMath::IsNearlyEqual((double)Params.InvVoxelSpacing.X, 4.666666666666667, 1e-5));
		TestTrue(TEXT("InvVoxelSpacing Y is 7"),
			FMath::IsNearlyEqual((double)Params.InvVoxelSpacing.Y, 7.0, 1e-5));

		// UVWScale = 1 / PhysicalSize, per axis: 1/12, 1/4, 1/1.
		TestTrue(TEXT("UVWScale X is 1/12"), FMath::IsNearlyEqual((double)Params.UVWScale.X, 1.0 / 12.0, 1e-7));
		TestTrue(TEXT("UVWScale Y is 1/4"), FMath::IsNearlyEqual((double)Params.UVWScale.Y, 0.25, 1e-7));
		TestTrue(TEXT("UVWScale Z is 1/1"), FMath::IsNearlyEqual((double)Params.UVWScale.Z, 1.0, 1e-7));

		// A cell field's UVW bias is ZERO: local position 0 is the minimum
		// corner, which is already texel 0's left edge.
		TestTrue(TEXT("a cell field has no UVW bias"),
			FMath::IsNearlyEqual((double)Params.UVWBias.X, 0.0, 1e-9));
		TestEqual(TEXT("AssociationCode 0 is cell"), Params.AssociationCode, 0u);
		TestEqual(TEXT("DataTypeCode is the float16 enumerator"),
			Params.DataTypeCode, (uint32)ECFDVizDataType::Float16);

		// Rejecting non-finite values is meaningful for a float format and is
		// meaningless for an integer one.
		TestEqual(TEXT("a float field asks the shader to reject non-finite values"), Params.bRejectNonFinite, 1u);
		TestEqual(TEXT("the pad pattern is reported to the shader"), Params.PadElementBits, 0x7E00u);
		TestEqual(TEXT("the required mask is Valid"), Params.RequiredStatusMask, (uint32)FlowVizVoxelStatus::Valid);

		// The parameter block's own UVW must agree with the transform's - they
		// are two implementations of one mapping, and the shader uses the former.
		const FVector Local = CellCentre - Cell.Grid.Origin;
		const double ShaderU = (double)Params.UVWScale.X * Local.X + (double)Params.UVWBias.X;
		TestTrue(TEXT("the shader's UVW mapping agrees with PhysicalToTextureUVW"),
			FMath::IsNearlyEqual(ShaderU, UVW.X, 1e-6));

		// A point field's bias is half a texel, and its scale is over 57 not 56.
		FFlowVizVolumeLayout PointLayout;
		TestTrue(TEXT("a point layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(57, 29, 7), 1, ECFDVizDataType::Float32, PointLayout).IsOk());
		FFlowVizVolumeShaderParameters PointParams;
		TestTrue(TEXT("point shader parameters build"), Point.MakeShaderParametersWithoutValueRange(PointLayout, PointParams).IsOk());
		TestEqual(TEXT("AssociationCode 1 is point"), PointParams.AssociationCode, 1u);
		TestTrue(TEXT("a point field's UVW bias is half a texel"),
			FMath::IsNearlyEqual((double)PointParams.UVWBias.X, 0.5 / 57.0, 1e-7));
		TestTrue(TEXT("and its scale is over 57 texels of 0.2142857 each"),
			FMath::IsNearlyEqual((double)PointParams.UVWScale.X, 0.081871345029239775, 1e-7));

		// A layout sized from the CELL count for a POINT field is the classic
		// bug. It must be rejected, not quietly rendered one plane short.
		FFlowVizVolumeShaderParameters Wrong;
		TestFalse(TEXT("a point transform with a cell-sized layout is REJECTED"),
			Point.MakeShaderParametersWithoutValueRange(Layout, Wrong).IsOk());

		/* -- Origin narrowing is reported, not hidden ----------------------- */
		TestTrue(TEXT("a zero origin narrows exactly"),
			FMath::IsNearlyEqual(Cell.GetOriginNarrowingError(), 0.0, 1e-15));
		TestTrue(TEXT("and the parameter block says so"),
			FMath::IsNearlyEqual((double)Params.OriginNarrowingError, 0.0f, 1e-9));

		FFlowVizVolumeTransform Far;
		Far.Grid = MakeSampleGrid();
		Far.Grid.Origin = FVector(1234567.89, 0.0, 0.0);
		Far.Association = ECFDVizAssociation::Cell;
		// float32 holds 1234567.89 as 1234567.875: an error of 0.015 solver
		// units - centimetres of misplacement, silently, if this went unreported.
		TestTrue(TEXT("a kilometre-scale origin reports its narrowing error"),
			FMath::IsNearlyEqual(Far.GetOriginNarrowingError(), 0.014999999897554517, 1e-9));
		FFlowVizVolumeShaderParameters FarParams;
		TestTrue(TEXT("far shader parameters build"), Far.MakeShaderParametersWithoutValueRange(Layout, FarParams).IsOk());
		TestTrue(TEXT("and the error reaches the shader block"), FarParams.OriginNarrowingError > 0.0f);

		/* -- Local-to-Unreal placement -------------------------------------- */
		const FMatrix ToUnreal = Cell.GetLocalToUnrealTransform();
		// Local (0,0,0) is the grid's minimum corner, which sits at the solver
		// origin, so it converts to the Unreal origin.
		TestTrue(TEXT("the local origin lands at the world origin"),
			ToUnreal.TransformPosition(FVector::ZeroVector).Equals(FVector::ZeroVector, 1e-6));
		// Local (12,4,1) m -> (1200, -400, 100) cm. Y NEGATES: canonical CFDViz
		// is right-handed and Unreal is left-handed (ADR 004).
		const FVector FarCorner = ToUnreal.TransformPosition(FVector(12.0, 4.0, 1.0));
		TestTrue(TEXT("X scales to centimetres"), FMath::IsNearlyEqual(FarCorner.X, 1200.0, 1e-6));
		TestTrue(TEXT("Y scales AND MIRRORS"), FMath::IsNearlyEqual(FarCorner.Y, -400.0, 1e-6));
		TestTrue(TEXT("Z scales to centimetres"), FMath::IsNearlyEqual(FarCorner.Z, 100.0, 1e-6));
		TestTrue(TEXT("so the transform reverses winding"), TransformReversesWinding(ToUnreal));

		// A non-metre case must use its own scale. 1000x wrong is the failure
		// when a millimetre case is assumed to be metres.
		const FMatrix Millimetres = Cell.GetLocalToUnrealTransform(0.1);
		TestTrue(TEXT("a millimetre case scales by 0.1"),
			FMath::IsNearlyEqual(Millimetres.TransformPosition(FVector(12.0, 0.0, 0.0)).X, 1.2, 1e-9));

		// A degenerate grid produces no parameters rather than a plausible block.
		FFlowVizVolumeTransform Degenerate;
		Degenerate.Grid.Dimensions = FIntVector(0, 0, 0);
		TestFalse(TEXT("a degenerate transform is not valid"), Degenerate.IsValid());
		FFlowVizVolumeShaderParameters Nothing;
		TestFalse(TEXT("and produces no shader parameters"),
			Degenerate.MakeShaderParametersWithoutValueRange(Layout, Nothing).IsOk());
	}

	/* == Upload payload validation ========================================== */
	{
		FFlowVizVolumeLayout Scalar;
		TestTrue(TEXT("a scalar layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(4, 2, 2), 1, ECFDVizDataType::Float16, Scalar).IsOk());
		FFlowVizVolumeLayout Status;
		TestTrue(TEXT("a status layout builds"),
			FFlowVizVolumeLayout::MakeStatusLayout(FIntVector(4, 2, 2), Status).IsOk());

		FFlowVizVolumeUpload Upload;
		Upload.FrameIndex = 7;
		Upload.SimulationTime = 0.35;
		Upload.ScalarLayout = Scalar;
		Upload.ScalarBytes.Init(0, 32);
		Upload.StatusLayout = Status;
		Upload.StatusBytes.Init(FlowVizVoxelStatus::Valid, 16);

		TestTrue(TEXT("a complete payload validates"), Upload.Validate().IsOk());
		TestEqual(TEXT("total bytes is 32 + 16"), Upload.GetTotalBytes(), (int64)48);

		FFlowVizVolumeUpload NoFrame = MoveTemp(Upload);
		NoFrame.FrameIndex = INDEX_NONE;
		TestFalse(TEXT("a payload with no frame index is rejected"), NoFrame.Validate().IsOk());
		NoFrame.FrameIndex = 7;

		// A buffer one byte short of its layout. This is the check that catches a
		// texture built from a cell count while the field used the value count.
		NoFrame.ScalarBytes.SetNum(31);
		const FCFDVizResult ShortResult = NoFrame.Validate();
		TestFalse(TEXT("a short scalar buffer is rejected"), ShortResult.IsOk());
		TestEqual(TEXT("as SizeMismatch"),
			static_cast<int32>(ShortResult.Error), static_cast<int32>(ECFDVizError::SizeMismatch));
		NoFrame.ScalarBytes.Init(0, 32);
		TestTrue(TEXT("restoring the length makes it valid again"), NoFrame.Validate().IsOk());

		// A status texture built for a DIFFERENT extent. Its byte count is
		// self-consistent, so only the shared-extent check catches it - and the
		// misalignment it would cause marks the wrong cells invalid, which reads
		// as noise in the data rather than as a bug.
		FFlowVizVolumeLayout WrongStatus;
		TestTrue(TEXT("a 5x2x2 status layout builds"),
			FFlowVizVolumeLayout::MakeStatusLayout(FIntVector(5, 2, 2), WrongStatus).IsOk());
		NoFrame.StatusLayout = WrongStatus;
		NoFrame.StatusBytes.Init(FlowVizVoxelStatus::Valid, 20);
		TestFalse(TEXT("a status texture of a different extent is rejected"), NoFrame.Validate().IsOk());

		// An empty payload has nothing to show and must not be uploaded.
		FFlowVizVolumeUpload Empty;
		Empty.FrameIndex = 0;
		TestFalse(TEXT("a payload with no field at all is rejected"), Empty.Validate().IsOk());
		TestEqual(TEXT("and totals zero bytes"), Empty.GetTotalBytes(), (int64)0);

		/*
		 * THE HALF-FILLED PAIR, which is the second branch of "absent".
		 *
		 * Validate treats a buffer as PRESENT only when its layout is valid, and
		 * skips it otherwise. Skipping is right for a genuinely absent field -
		 * the vector slot on a scalar-only payload is the normal case, asserted
		 * above. But the implementation's own comment says why the skip cannot
		 * be unconditional: "an invalid layout with bytes attached is a caller
		 * that filled one half of a pair; it must not pass as 'absent'."
		 *
		 * Only the absent half had a test. The two are indistinguishable from
		 * inside Validate unless the byte count is examined.
		 *
		 * WHAT HAPPENS IF THIS GUARD GOES, traced rather than assumed, because a
		 * comment claiming downstream behaviour nobody checked is the same
		 * species of false receipt this audit exists to find. Validate is the
		 * gate in EnqueueUpload (FlowVizVolumeTexture.cpp, the first statement);
		 * pass it and the payload reaches UploadOnRenderThread, whose per-target
		 * loop begins `if (!Target.PayloadLayout->IsValid()) { continue; }`. An
		 * orphaned VectorBytes therefore has no valid layout, is SKIPPED, leaves
		 * bAllOk true, and the slot is marked bHasContent = true. The frame
		 * displays scalar-only, as complete, and no branch on that path logs
		 * anything. So the guard under test is the ONLY thing standing between a
		 * caller's forgotten layout and a silently dropped vector field.
		 *
		 * Both directions, because a rejection keyed on "bytes present" alone
		 * would break the legitimate absent case that the assertions above rely
		 * on.
		 */
		FFlowVizVolumeUpload HalfPair;
		HalfPair.FrameIndex = 3;
		HalfPair.ScalarLayout = Scalar;
		HalfPair.ScalarBytes.Init(0, 32);
		HalfPair.StatusLayout = Status;
		HalfPair.StatusBytes.Init(FlowVizVoxelStatus::Valid, 16);
		TestTrue(TEXT("the control payload validates before the vector half is added"),
			HalfPair.Validate().IsOk());

		// Bytes without a layout. VectorLayout stays default-constructed, which
		// is exactly what a caller who forgot to set it would leave behind.
		HalfPair.VectorBytes.Init(0, 96);
		const FCFDVizResult HalfResult = HalfPair.Validate();
		TestFalse(TEXT("a buffer with bytes but no valid layout is REJECTED, not silently "
					   "treated as absent - otherwise a dropped vector field uploads and "
					   "displays as a scalar-only frame with nothing reporting the loss"),
			HalfResult.IsOk());
		TestEqual(TEXT("and it is a SizeMismatch, naming the disagreement"),
			static_cast<int32>(HalfResult.Error), static_cast<int32>(ECFDVizError::SizeMismatch));
		TestTrue(TEXT("and the message names the vector buffer, not the scalar one that is fine"),
			HalfResult.Message.Contains(TEXT("vector")));

		// The other direction: clearing the bytes restores a genuinely absent
		// field. Without this the check above would also pass against a Validate
		// that rejected every payload with an invalid layout, which would
		// outlaw the scalar-only case the renderer depends on.
		HalfPair.VectorBytes.Reset();
		TestTrue(TEXT("clearing the orphaned bytes makes the field genuinely absent again, "
					  "so a scalar-only payload is still legal"),
			HalfPair.Validate().IsOk());
	}

	/* == Buffer rotation ==================================================== */
	{
		// Three slots, two of them holding the frames on screen. This is the
		// configuration plan.md section 9 asks for and the one where the "do not
		// evict a displayed frame" rule has to be enforced by something other
		// than luck.
		TArray<FFlowVizVolumeSlotState> Slots;
		Slots.SetNum(3);
		Slots[0].FrameIndex = 10; Slots[0].LastUseSerial = 100;
		Slots[1].FrameIndex = 11; Slots[1].LastUseSerial = 101;
		Slots[2].FrameIndex = 9;  Slots[2].LastUseSerial = 50;

		TestEqual(TEXT("frame 11 is found in slot 1"), FlowVizVolumeRing::FindSlotForFrame(Slots, 11), 1);
		TestEqual(TEXT("an absent frame is INDEX_NONE"), FlowVizVolumeRing::FindSlotForFrame(Slots, 99), INDEX_NONE);

		// A slot mid-upload holds the OLD frame's voxels. Matching it would show
		// frame N-1 under frame N's label.
		Slots[1].bUploadInFlight = true;
		TestEqual(TEXT("a slot with an upload in flight does not match"),
			FlowVizVolumeRing::FindSlotForFrame(Slots, 11), INDEX_NONE);
		Slots[1].bUploadInFlight = false;

		/* -- The hold-last-frame fallback (#88): the strobe fix. ------------- */

		// The most recently USED resident slot, which is what the display holds
		// through an upload gap. Slot 1 (serial 101) beats slot 0 (100).
		TestEqual(TEXT("the most recent resident slot is the highest serial"),
			FlowVizVolumeRing::FindMostRecentSlot(Slots), 1);

		// Mid-upload slots are excluded for the same reason as FindSlotForFrame:
		// falling back to torn voxels would trade a blink for garbage.
		Slots[1].bUploadInFlight = true;
		TestEqual(TEXT("the fallback never picks a slot mid-upload"),
			FlowVizVolumeRing::FindMostRecentSlot(Slots), 0);
		Slots[1].bUploadInFlight = false;

		// An empty ring has nothing to hold: INDEX_NONE, not a crash and not
		// slot 0's never-filled state.
		TArray<FFlowVizVolumeSlotState> Empty;
		Empty.SetNum(2);
		TestEqual(TEXT("an empty ring yields INDEX_NONE -- there is no last frame "
					   "before the first"),
			FlowVizVolumeRing::FindMostRecentSlot(Empty), INDEX_NONE);

		// Prefetching frame 12 while 10 and 11 are displayed: slot 2 is the only
		// legal answer - it is neither pinned nor the most recently used.
		TestEqual(TEXT("the prefetch evicts the LRU unpinned slot"),
			FlowVizVolumeRing::ChooseUploadSlot(Slots, 12, 10, 11), 2);

		// A frame already resident reuses its own slot rather than a second one.
		TestEqual(TEXT("an already-resident frame reuses its slot"),
			FlowVizVolumeRing::ChooseUploadSlot(Slots, 10, 10, 11), 0);

		// A never-filled slot beats evicting anything.
		TArray<FFlowVizVolumeSlotState> WithEmpty;
		WithEmpty.SetNum(3);
		WithEmpty[0].FrameIndex = 10; WithEmpty[0].LastUseSerial = 100;
		WithEmpty[2].FrameIndex = 11; WithEmpty[2].LastUseSerial = 101;
		TestEqual(TEXT("an empty slot is preferred over an eviction"),
			FlowVizVolumeRing::ChooseUploadSlot(WithEmpty, 12, 10, 11), 1);

		// THE TWO-BUFFER ANSWER. Both slots hold displayed frames, so there is
		// nowhere to put a prefetch, and the honest answer is INDEX_NONE - not
		// "evict the older one", which would tear the visible interpolation.
		// This is the concrete cost of two buffers that ADR 002 and plan.md
		// section 9 cite for asking for three.
		TArray<FFlowVizVolumeSlotState> TwoSlots;
		TwoSlots.SetNum(2);
		TwoSlots[0].FrameIndex = 10; TwoSlots[0].LastUseSerial = 100;
		TwoSlots[1].FrameIndex = 11; TwoSlots[1].LastUseSerial = 101;
		// INDEX_NONE is an unscoped enumerator, not an int32, so it is cast at
		// every Test* comparison in this file. Without the cast the compiler
		// cannot pick between the int32/int64/template overloads and the build
		// fails - which is a compile error for the whole unity module, not just
		// for this test.
		TestEqual(TEXT("two buffers mid-interpolation CANNOT prefetch"),
			FlowVizVolumeRing::ChooseUploadSlot(TwoSlots, 12, 10, 11), static_cast<int32>(INDEX_NONE));
		// ...and three can. Stated adjacently because the pair is the argument.
		TestNotEqual(TEXT("but three buffers can"),
			FlowVizVolumeRing::ChooseUploadSlot(Slots, 12, 10, 11), static_cast<int32>(INDEX_NONE));

		// Only one frame displayed frees the other slot again.
		TestEqual(TEXT("with one pinned frame, the other slot is available"),
			FlowVizVolumeRing::ChooseUploadSlot(TwoSlots, 12, 11, INDEX_NONE), 0);

		// Every slot busy uploading is also INDEX_NONE.
		TArray<FFlowVizVolumeSlotState> AllBusy;
		AllBusy.SetNum(2);
		AllBusy[0].bUploadInFlight = true;
		AllBusy[1].bUploadInFlight = true;
		TestEqual(TEXT("every slot uploading means no slot"),
			FlowVizVolumeRing::ChooseUploadSlot(AllBusy, 12, INDEX_NONE, INDEX_NONE), INDEX_NONE);
	}

	/* == The texture set's bookkeeping, without a device ===================== */
	{
		FFlowVizVolumeTextureSet Set;
		TestFalse(TEXT("one buffer is below the minimum"), Set.Initialize(1).IsOk());
		TestFalse(TEXT("nine buffers is above the maximum"), Set.Initialize(9).IsOk());
		TestTrue(TEXT("three buffers initialises"), Set.Initialize(FlowVizVolume::RecommendedBufferCount).IsOk());
		TestEqual(TEXT("and reports three"), Set.GetBufferCount(), 3);
		TestEqual(TEXT("with three slot states"), Set.GetSlotStates().Num(), 3);
		TestEqual(TEXT("no frame is resident yet"), Set.FindSlotForFrame(0), INDEX_NONE);

		Set.SetDisplayFrames(4, 5);
		TestEqual(TEXT("display frame A is remembered"), Set.GetDisplayFrameA(), 4);
		TestEqual(TEXT("display frame B is remembered"), Set.GetDisplayFrameB(), 5);

		// With everything empty, a peek finds a slot without reserving it - so
		// two peeks in a row return the same one.
		const int32 Peeked = Set.PeekUploadSlot(6);
		TestTrue(TEXT("a peek finds a free slot"), Peeked != INDEX_NONE);
		TestEqual(TEXT("and peeking does not reserve it"), Set.PeekUploadSlot(6), Peeked);
		TestEqual(TEXT("slot bookkeeping is unchanged by a peek"),
			Set.GetSlotStates()[Peeked].FrameIndex, INDEX_NONE);

		TestEqual(TEXT("an out-of-range slot has no textures"), Set.GetSlotTextures(3), (const FFlowVizVolumeSlotTextures*)nullptr);
		TestNotEqual(TEXT("an in-range slot does"), Set.GetSlotTextures(0), (const FFlowVizVolumeSlotTextures*)nullptr);
	}

	/* == The real file: reader -> upload ==================================== */
	{
		const FString CaseDir = GetVolumeSampleCaseDir();
		if (!TestTrue(TEXT("the sample case directory resolves"), !CaseDir.IsEmpty()))
		{
			return false;
		}

		const FString UPath = FPaths::Combine(CaseDir, TEXT("frames"), TEXT("000000"), TEXT("U.cvf"));
		const FString MaskPath = FPaths::Combine(CaseDir, TEXT("frames"), TEXT("000000"), TEXT("validMask.cvf"));
		const FString PressurePath = FPaths::Combine(CaseDir, TEXT("frames"), TEXT("000000"), TEXT("pressure.cvf"));

		FCFDVizVolumeReader UReader;
		if (!TestTrue(TEXT("U.cvf opens"), UReader.Open(UPath).IsOk()))
		{
			return false;
		}

		// The header agrees with the manifest - if it did not, every expected
		// value below would be measuring the wrong file.
		TestEqual(TEXT("U is 56x28x6 cells"), UReader.GetHeader().Dimensions, FIntVector(56, 28, 6));
		TestEqual(TEXT("U has three components"), UReader.GetHeader().ComponentCount, 3);
		TestEqual(TEXT("U is float16"),
			static_cast<int32>(UReader.GetHeader().DataType), static_cast<int32>(ECFDVizDataType::Float16));

		FFlowVizVolumeLayout ULayout;
		TArray<uint8> UBytes;
		// Guarded for the same reason as the BuildUpload call below: the spot
		// values that follow index UBytes at fixed offsets up to 75260, and an
		// empty buffer would trip TArray's bounds assert and take the whole
		// editor down before any verdict is reported.
		if (!TestTrue(TEXT("U builds into texture bytes"),
			FlowVizVolumeBuild::BuildFieldBytes(UReader, ULayout, UBytes).IsOk()))
		{
			return false;
		}

		// 56*28*6 voxels * 4 channels * 2 bytes. NOT 3 channels: a 3-component
		// field has no portable 3D format and must widen.
		//
		// GUARDED: this is the size the spot values below assume. A layout bug
		// that shrinks the buffer (a row pitch computed from the source stride,
		// say) still returns Ok and still fills what it allocated, so the reads
		// at 44144..75260 would run off the end and trip TArray's bounds assert -
		// killing the editor instead of failing the test.
		if (!TestEqual(TEXT("the widened U buffer is 75264 bytes"), UBytes.Num(), 75264))
		{
			return false;
		}
		TestEqual(TEXT("the layout agrees"), ULayout.GetTextureVolumeBytes(), (int64)75264);
		TestEqual(TEXT("U's extent is the cell count, since it is cell-associated"),
			ULayout.Extent, FIntVector(56, 28, 6));

		// SPOT VALUES FROM THE FILE, decoded independently in Python before this
		// code existed. Voxel (30,14,3) at texture offset 5518*8 = 44144.
		TestEqual(TEXT("U(30,14,3) component 0 bits"), ReadBits16(UBytes, 44144), (uint16)0x4421);
		TestEqual(TEXT("U(30,14,3) component 1 bits"), ReadBits16(UBytes, 44146), (uint16)0x3835);
		TestEqual(TEXT("U(30,14,3) component 2 bits"), ReadBits16(UBytes, 44148), (uint16)0xB079);
		TestEqual(TEXT("U(30,14,3) pad channel is the quiet NaN"), ReadBits16(UBytes, 44150), (uint16)0x7E00);

		// Voxel (40,14,3): 40 + 56*(14+28*3) = 5528, offset 44224. It lives in
		// the SECOND brick (x >= 32), so this is the assertion that fails if the
		// edge brick is misplaced or read as a full 32-wide cube.
		TestEqual(TEXT("U(40,14,3) c0 - a voxel from the SECOND brick"), ReadBits16(UBytes, 44224), (uint16)0x446A);
		TestEqual(TEXT("U(40,14,3) c1"), ReadBits16(UBytes, 44226), (uint16)0x3592);
		TestEqual(TEXT("U(40,14,3) c2"), ReadBits16(UBytes, 44228), (uint16)0xB079);

		// The first and last voxels, which pin both ends of the buffer.
		TestEqual(TEXT("U(0,0,0) c0"), ReadBits16(UBytes, 0), (uint16)0x4773);
		TestEqual(TEXT("U(0,0,0) c1"), ReadBits16(UBytes, 2), (uint16)0xADBF);
		TestEqual(TEXT("U(0,0,0) c2"), ReadBits16(UBytes, 4), (uint16)0x0000);
		// (55,27,5) = voxel 9407, texture offset 75256.
		TestEqual(TEXT("U(55,27,5) c0 - the last voxel"), ReadBits16(UBytes, 75256), (uint16)0x45E1);
		TestEqual(TEXT("U(55,27,5) c1"), ReadBits16(UBytes, 75258), (uint16)0xACA0);
		TestEqual(TEXT("U(55,27,5) c2"), ReadBits16(UBytes, 75260), (uint16)0x95D6);

		/* -- Status against the real mask ------------------------------------ */
		FCFDVizVolumeReader MaskReader;
		if (!TestTrue(TEXT("validMask.cvf opens"), MaskReader.Open(MaskPath).IsOk()))
		{
			return false;
		}
		TestEqual(TEXT("the mask is uint8"),
			static_cast<int32>(MaskReader.GetHeader().DataType), static_cast<int32>(ECFDVizDataType::UInt8));

		FFlowVizVolumeUpload Upload;
		// GUARDED, not a bare TestTrue. Everything below indexes Upload's buffers
		// at fixed offsets; if the assembly fails those buffers are EMPTY and the
		// indexing trips TArray's bounds assert, which kills the editor before it
		// reports a verdict. A crash is not a test failure - the runner prints
		// "no tests matched" and a mutation run reading that would score the
		// mutant as inconclusive rather than killed.
		if (!TestTrue(TEXT("a full frame assembles from the two readers"),
			FlowVizVolumeBuild::BuildUpload(UReader, &MaskReader, /*bAsVector*/ true, Upload).IsOk()))
		{
			return false;
		}
		TestTrue(TEXT("and the assembled payload validates"), Upload.Validate().IsOk());
		TestEqual(TEXT("the frame index came from the CVF header"), Upload.FrameIndex, 0);
		TestEqual(TEXT("U landed in the VECTOR slot"), Upload.VectorBytes.Num(), 75264);
		TestEqual(TEXT("and not in the scalar slot"), Upload.ScalarBytes.Num(), 0);
		TestEqual(TEXT("the status texture is one byte per voxel"), Upload.StatusBytes.Num(), 9408);

		// THE NUMBER THAT CAME OUT OF THE FILE. MockCylinderWake's cylinder
		// interior is exactly 132 cells, the mask rejects exactly those, and U
		// stores NaN in exactly those. Two independent facts about the same 132
		// voxels, so a status build that dropped either the mask or the NaN scan
		// would still be caught by the other count.
		const FlowVizVolumeStatus::FCounts Counts = FlowVizVolumeStatus::CountStatus(Upload.StatusBytes);
		TestEqual(TEXT("9408 voxels were classified"), Counts.Total, (int64)9408);
		TestEqual(TEXT("exactly 132 voxels are masked - the cylinder interior"), Counts.Masked, (int64)132);
		TestEqual(TEXT("and exactly 132 carry NaN"), Counts.NaN, (int64)132);
		TestEqual(TEXT("so 9408 - 132 are valid"), Counts.Valid, (int64)9276);
		TestEqual(TEXT("nothing is infinite"), Counts.Infinite, (int64)0);
		TestEqual(TEXT("and nothing is Unknown - every voxel was written"), Counts.Unknown, (int64)0);

		// The masked voxels are in the right PLACES, not merely the right count.
		// A status volume that was correct in aggregate but transposed would pass
		// every count above. (17,11,0) = 633 and (19,16,5) = 8755 are the first
		// and last masked cells, from an independent decode.
		TestEqual(TEXT("voxel (17,11,0) is masked - the first rejected cell"),
			Upload.StatusBytes[633] & FlowVizVoxelStatus::Masked, (uint8)FlowVizVoxelStatus::Masked);
		TestEqual(TEXT("voxel (19,16,5) is masked - the last rejected cell"),
			Upload.StatusBytes[8755] & FlowVizVoxelStatus::Masked, (uint8)FlowVizVoxelStatus::Masked);
		TestEqual(TEXT("voxel 0 is NOT masked"),
			Upload.StatusBytes[0] & FlowVizVoxelStatus::Masked, (uint8)0);
		TestEqual(TEXT("voxel (30,14,3) is valid"),
			Upload.StatusBytes[5518], (uint8)FlowVizVoxelStatus::Valid);

		// And the field's NaN bits reached the texture buffer unmodified - the
		// status texture records invalidity, the field is never rewritten.
		TestEqual(TEXT("the masked voxel still holds its stored NaN bits"),
			ReadBits16(UBytes, 633 * 8), (uint16)0x7E00);

		/* -- A scalar field takes the scalar path ---------------------------- */
		FCFDVizVolumeReader PressureReader;
		if (TestTrue(TEXT("pressure.cvf opens"), PressureReader.Open(PressurePath).IsOk()))
		{
			FFlowVizVolumeUpload ScalarUpload;
			TestTrue(TEXT("pressure assembles"),
				FlowVizVolumeBuild::BuildUpload(PressureReader, &MaskReader, /*bAsVector*/ false, ScalarUpload).IsOk());
			// One component, so no widening: 9408 * 2 bytes.
			TestEqual(TEXT("pressure is 18816 bytes, unwidened"), ScalarUpload.ScalarBytes.Num(), 18816);
			TestEqual(TEXT("and nothing is in the vector slot"), ScalarUpload.VectorBytes.Num(), 0);
			// Voxel 5518 at offset 11036, decoded independently.
			TestEqual(TEXT("pressure(30,14,3) bits"), ReadBits16(ScalarUpload.ScalarBytes, 11036), (uint16)0x4CDD);

			const FlowVizVolumeStatus::FCounts PressureCounts =
				FlowVizVolumeStatus::CountStatus(ScalarUpload.StatusBytes);
			TestEqual(TEXT("pressure has the same 132 masked cells"), PressureCounts.Masked, (int64)132);
			TestEqual(TEXT("and the same 132 NaN cells"), PressureCounts.NaN, (int64)132);
		}

		/* -- A budget refuses before it allocates ---------------------------- */
		FFlowVizVolumeLayout Budgeted;
		TArray<uint8> BudgetedBytes;
		const FCFDVizResult OverBudget =
			FlowVizVolumeBuild::BuildFieldBytes(UReader, Budgeted, BudgetedBytes, 75263);
		TestFalse(TEXT("a budget one byte under the widened size refuses"), OverBudget.IsOk());
		TestEqual(TEXT("and the buffer is empty"), BudgetedBytes.Num(), 0);
		// The SOURCE read is only 56448 bytes, so a budget between the two sizes
		// must still refuse - a check applied only to the read would let it pass
		// and then allocate 75264.
		TArray<uint8> BetweenBytes;
		TestFalse(TEXT("a budget between the source and texture sizes still refuses"),
			FlowVizVolumeBuild::BuildFieldBytes(UReader, Budgeted, BetweenBytes, 60000).IsOk());

		/* -- A closed reader is refused, not read from ----------------------- */
		FCFDVizVolumeReader Closed;
		FFlowVizVolumeLayout ClosedLayout;
		TArray<uint8> ClosedBytes;
		TestFalse(TEXT("an unopened reader is refused"),
			FlowVizVolumeBuild::BuildFieldBytes(Closed, ClosedLayout, ClosedBytes).IsOk());
	}

	/* == A POINT-associated field sizes the texture with the +1 ============== */
	{
		// EVERY committed sample field is cell-associated, so BuildFieldBytes was
		// only ever exercised on the branch where GetValueCounts() == Dimensions.
		// Taking Dimensions directly instead of GetValueCounts() therefore changed
		// nothing any existing assertion could see - it survived mutation. On a
		// point-associated field it silently drops the last plane on every axis:
		// a slightly cropped volume that still renders, still uploads, and is
		// wrong by exactly the half-cell shift the format calls the single most
		// common visualisation error.
		//
		// Hand-built rather than loaded: no committed .cvf is point-associated,
		// so there is nothing on disk that can distinguish the two.
		// From `write_cvf(values=v, brick_size=(3,2,4), association='point',
		//                 dtype='float32', codec=0, dimensions=(2,1,3))`, where
		// v[i,j,k] = i + 3j + 6k over the 3x2x4 POINT extent.
		//
		// Cell dimensions are (2,1,3); point values are (3,2,4) = 24 values, one
		// float32 component = 96 bytes. A reader that used Dimensions would build
		// a (2,1,3) extent and 24 bytes.
		static const uint8 PointCvfBytes[] = {
			0x43, 0x46, 0x44, 0x56, 0x4F, 0x4C, 0x31, 0x00, 0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
			0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x04, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x29, 0xFC, 0x08, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00,
			0x04, 0x00, 0x00, 0x00, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00,
			0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,
			0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0xB8, 0x41, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,
			0x00, 0x00, 0x80, 0xFF, 0xB0, 0x04, 0x82, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x40, 0x40,
			0x00, 0x00, 0x80, 0x40, 0x00, 0x00, 0xA0, 0x40, 0x00, 0x00, 0xC0, 0x40, 0x00, 0x00, 0xE0, 0x40,
			0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x10, 0x41, 0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x30, 0x41,
			0x00, 0x00, 0x40, 0x41, 0x00, 0x00, 0x50, 0x41, 0x00, 0x00, 0x60, 0x41, 0x00, 0x00, 0x70, 0x41,
			0x00, 0x00, 0x80, 0x41, 0x00, 0x00, 0x88, 0x41, 0x00, 0x00, 0x90, 0x41, 0x00, 0x00, 0x98, 0x41,
			0x00, 0x00, 0xA0, 0x41, 0x00, 0x00, 0xA8, 0x41, 0x00, 0x00, 0xB0, 0x41, 0x00, 0x00, 0xB8, 0x41,
		};

		const FCFDVizMemoryByteSource PointSource(
			TArrayView<const uint8>(PointCvfBytes, UE_ARRAY_COUNT(PointCvfBytes)), TEXT("point.cvf"));
		FCFDVizVolumeReader PointReader;
		if (TestTrue(TEXT("a point-associated .cvf opens"), PointReader.Open(PointSource).IsOk()))
		{
			TestTrue(TEXT("the fixture really is point-associated"),
				PointReader.GetHeader().Association == ECFDVizAssociation::Point);
			// Independently: cell dims (2,1,3) + 1 per axis.
			TestEqual(TEXT("its value counts carry the +1"),
				PointReader.GetHeader().GetValueCounts(), FIntVector(3, 2, 4));

			FFlowVizVolumeLayout PointLayout;
			TArray<uint8> PointBytes;
			if (TestTrue(TEXT("a point field assembles"),
				FlowVizVolumeBuild::BuildFieldBytes(PointReader, PointLayout, PointBytes).IsOk()))
			{
				// THE ASSERTION THE SURVIVING MUTANT NEEDED. Using Dimensions
				// would give (2,1,3) here, and every check below would move with
				// it - so the extent is asserted against the hand-derived (3,2,4),
				// not against anything the implementation computed.
				TestEqual(TEXT("the texture extent is the VALUE count, not the cell count"),
					PointLayout.Extent, FIntVector(3, 2, 4));
				// 3*2*4 values * 1 component * 4 bytes. The cell-count mistake
				// yields 24.
				TestEqual(TEXT("...so the volume is 96 bytes, not 24"),
					PointBytes.Num(), 96);
				TestEqual(TEXT("...and the layout agrees"),
					PointLayout.GetTextureVolumeBytes(), (int64)96);

				// Spot-check the payload actually reached the buffer in X-fastest
				// order: v[i,j,k] = i + 3j + 6k, so the LAST value (2,1,3) is 23.
				// A cropped extent would never contain this voxel at all.
				const int64 LastOffset = PointLayout.GetTextureVoxelOffset(2, 1, 3);
				TestEqual(TEXT("the last point value sits at offset 92"), LastOffset, (int64)92);
				float LastValue = 0.0f;
				FMemory::Memcpy(&LastValue, PointBytes.GetData() + LastOffset, sizeof(float));
				TestEqual(TEXT("...and holds 23.0, the corner the cell count would have dropped"),
					LastValue, 23.0f);
			}
		}
	}

	/* == Device support, without a device ==================================== */
	{
		// Under -nullrhi there is no device to be incompatible with, and
		// reporting a phantom incompatibility would make every logic test fail
		// for a reason that has nothing to do with the logic.
		FFlowVizVolumeLayout Layout;
		TestTrue(TEXT("a layout builds"),
			FFlowVizVolumeLayout::Make(FIntVector(56, 28, 6), 1, ECFDVizDataType::Float16, Layout).IsOk());
		const FCFDVizResult Support = FlowVizVolumeRHI::CheckDeviceSupport(Layout);
		if (!GIsRHIInitialized)
		{
			TestTrue(TEXT("with no RHI, device support is not a failure"), Support.IsOk());
		}
		else
		{
			TestTrue(TEXT("a 56x28x6 float16 volume is supported on this device"), Support.IsOk());
		}

		// An invalid layout is refused whether or not a device exists.
		TestFalse(TEXT("an invalid layout has no device support"),
			FlowVizVolumeRHI::CheckDeviceSupport(FFlowVizVolumeLayout()).IsOk());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
