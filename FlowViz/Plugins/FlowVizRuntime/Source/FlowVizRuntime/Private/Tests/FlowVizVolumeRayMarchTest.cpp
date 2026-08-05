// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Render/FlowVizVolumeTexture.h"

#include "GlobalShader.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RHIGlobals.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterMetadata.h"

/**
 * The ray-marching volume shader (ADR 002, plan.md section 9).
 *
 * WHAT CAN AND CANNOT BE CHECKED HERE, STATED UP FRONT. This file is split in
 * two, and the split is the honest boundary rather than a convenience:
 *
 *   FlowViz.Render.RayMarchShader        no device needed. The parameter block's
 *                                        layout, the defaults policy, the
 *                                        fail-closed status flag, the camera
 *                                        construction and the anisotropic step
 *                                        arithmetic - all pure C++.
 *   FlowViz.Render.RayMarchShaderDevice  needs RHI=1. That the shader COMPILED
 *                                        and is in the global shader map, which
 *                                        for a hand-written global shader is a
 *                                        runtime fact and not a build one
 *                                        (ADR 002 names this cost explicitly).
 *
 * THE ASSERTIONS BELOW ARE WRITTEN TO BE ABLE TO FAIL. Every one of them was
 * checked against a deliberately broken build before being left in place; where
 * a check would pass for a reason other than the one it names, it says so and
 * asserts the discriminating quantity instead. The failure this repo keeps
 * hitting is a criterion satisfied by something other than the property it
 * names - "N magenta pixels exist" passing because a legend swatch supplied
 * them - so a check here that could be satisfied by an empty frame, a cleared
 * buffer or a zeroed struct is not a check.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizVolumeRayMarchShaderTest,
	"FlowViz.Render.RayMarchShader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizVolumeRayMarchShaderDeviceTest,
	"FlowViz.Render.RayMarchShaderDevice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizRayMarchTestFixture
{
	/**
	 * The mock domain's grid, spelled out here rather than read from the code
	 * under test.
	 *
	 * THREE INDEPENDENT SPACINGS, NO TWO EQUAL AND NONE A MULTIPLE OF ANOTHER.
	 * 12m x 4m x 1m over 128 x 64 x 24 cells gives 0.09375, 0.0625, 0.0416667 -
	 * the numbers ADR 002 and FlowVizVolumeTexture.h both call out. A cubic
	 * fixture would pass against code that collapses spacing to a scalar, which
	 * is the single most likely way this shader is wrong on the primary dataset.
	 */
	FCFDVizGrid MakeMockGrid()
	{
		FCFDVizGrid Grid;
		Grid.Dimensions = FIntVector(128, 64, 24);
		Grid.Origin = FVector::ZeroVector;
		Grid.Spacing = FVector(12.0 / 128.0, 4.0 / 64.0, 1.0 / 24.0);
		return Grid;
	}

	/** Volume shader parameters for the mock domain, cell-associated, float16 x1. */
	bool MakeMockParameters(FFlowVizVolumeShaderParameters& OutParams)
	{
		FFlowVizVolumeTransform Transform;
		Transform.Grid = MakeMockGrid();
		Transform.Association = ECFDVizAssociation::Cell;

		FFlowVizVolumeLayout Layout;
		if (!FFlowVizVolumeLayout::Make(
				Transform.GetValueCounts(), 1, ECFDVizDataType::Float16, Layout).IsOk())
		{
			return false;
		}
		return Transform.MakeShaderParameters(Layout, OutParams).IsOk();
	}
}

bool FFlowVizVolumeRayMarchShaderTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizRayMarchTestFixture;

	/* == The constant buffer the shader reads ================================ */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a member of
	 * FFlowVizVolumeRayMarchParameters landing at a different byte offset from
	 * the same member of FFlowVizVolumeShaderParameters. That is the failure the
	 * task brief calls the trap - a shader reading spacing out of the dimensions
	 * slot renders a plausibly-shaped, wrong volume and passes a visual review.
	 *
	 * The static_asserts in the header pin the offsets at compile time, which is
	 * strictly better than a runtime check. What they CANNOT do is compare the
	 * two structs member by member by name, because offsetof on a
	 * SHADER_PARAMETER member and on a plain struct member are different
	 * expressions. This block does that comparison, and it fails at test time
	 * with a member name rather than at compile time with a byte number.
	 */
	{
		// Ten 16-byte rows, and the ray-march block must begin its own members
		// exactly where the volume block ends.
		TestEqual(TEXT("the volume parameter block is ten 16-byte rows"),
			static_cast<int32>(sizeof(FFlowVizVolumeShaderParameters)), 160);

		// Every member, by name, at the same offset in both structs. Spelled out
		// rather than looped, so a failure names the member that drifted.
		#define FLOWVIZ_TEST_SAME_OFFSET(Member) \
			TestEqual(TEXT("cbuffer offset agrees for " #Member), \
				static_cast<int32>(STRUCT_OFFSET(FFlowVizVolumeRayMarchParameters, Member)), \
				static_cast<int32>(offsetof(FFlowVizVolumeShaderParameters, Member)))

		FLOWVIZ_TEST_SAME_OFFSET(GridOrigin);
		FLOWVIZ_TEST_SAME_OFFSET(OriginNarrowingError);
		FLOWVIZ_TEST_SAME_OFFSET(PhysicalSize);
		FLOWVIZ_TEST_SAME_OFFSET(VoxelVolume);
		FLOWVIZ_TEST_SAME_OFFSET(VoxelSpacing);
		FLOWVIZ_TEST_SAME_OFFSET(MinVoxelSpacing);
		FLOWVIZ_TEST_SAME_OFFSET(InvVoxelSpacing);
		FLOWVIZ_TEST_SAME_OFFSET(MaxVoxelSpacing);
		FLOWVIZ_TEST_SAME_OFFSET(VolumeDimensions);
		FLOWVIZ_TEST_SAME_OFFSET(ComponentCount);
		FLOWVIZ_TEST_SAME_OFFSET(InvVolumeDimensions);
		FLOWVIZ_TEST_SAME_OFFSET(TextureComponentCount);
		FLOWVIZ_TEST_SAME_OFFSET(UVWScale);
		FLOWVIZ_TEST_SAME_OFFSET(AssociationCode);
		FLOWVIZ_TEST_SAME_OFFSET(UVWBias);
		FLOWVIZ_TEST_SAME_OFFSET(DataTypeCode);
		FLOWVIZ_TEST_SAME_OFFSET(RequiredStatusMask);
		FLOWVIZ_TEST_SAME_OFFSET(InvalidStatusMask);
		FLOWVIZ_TEST_SAME_OFFSET(bHasStatusTexture);
		FLOWVIZ_TEST_SAME_OFFSET(bHasVectorTexture);
		FLOWVIZ_TEST_SAME_OFFSET(ValueRangeMin);
		FLOWVIZ_TEST_SAME_OFFSET(ValueRangeMax);
		FLOWVIZ_TEST_SAME_OFFSET(bRejectNonFinite);
		FLOWVIZ_TEST_SAME_OFFSET(PadElementBits);

		#undef FLOWVIZ_TEST_SAME_OFFSET

		// And the HLSL side. The engine generates the shader-visible layout from
		// this metadata, so a member the metadata places elsewhere is a member
		// the SHADER reads from elsewhere - which no C++ offsetof can see.
		const FShaderParametersMetadata* Metadata =
			FFlowVizVolumeRayMarchParameters::FTypeInfo::GetStructMetadata();
		if (TestNotNull(TEXT("the parameter struct has shader metadata"), Metadata))
		{
			TArray<FString> Missing;
			TMap<FString, uint32> ByName;
			for (const FShaderParametersMetadata::FMember& Member : Metadata->GetMembers())
			{
				ByName.Add(FString(Member.GetName()), Member.GetOffset());
			}

			// The four members most likely to be confused with a neighbour, and
			// the two whose confusion is the documented failure mode.
			const TPair<const TCHAR*, int32> Pinned[] = {
				{ TEXT("GridOrigin"), 0 },
				{ TEXT("PhysicalSize"), 16 },
				{ TEXT("VoxelSpacing"), 32 },
				{ TEXT("MinVoxelSpacing"), 44 },
				{ TEXT("VolumeDimensions"), 64 },
				{ TEXT("UVWScale"), 96 },
				{ TEXT("UVWBias"), 112 },
				{ TEXT("bHasStatusTexture"), 136 },
			};
			for (const TPair<const TCHAR*, int32>& Entry : Pinned)
			{
				const uint32* Found = ByName.Find(FString(Entry.Key));
				if (TestNotNull(*FString::Printf(
						TEXT("the shader metadata declares %s"), Entry.Key), (const void*)Found))
				{
					TestEqual(*FString::Printf(
							TEXT("...and the SHADER reads %s from byte %d"), Entry.Key, Entry.Value),
						static_cast<int32>(*Found), Entry.Value);
				}
			}
		}
	}

	/* == Fail closed: no status texture means reject every voxel ============= */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: the flag that decides
	 * whether "we cannot tell" renders as "valid".
	 *
	 * FFlowVizVolumeTransform::MakeShaderParameters is a pure function of a
	 * layout. It cannot know what textures a caller will bind, so it writes
	 * bHasStatusTexture = 0 and bHasVectorTexture = 0 as PLACEHOLDERS. If the
	 * binding layer copies those through, the flags mean nothing. If instead it
	 * copied a 1 through, every voxel would read status from an unbound texture
	 * and the invalid-value policy would evaluate against garbage.
	 *
	 * So the assertion is not "the flag has some value". It is that the flag is
	 * a FUNCTION OF THE BOUND RESOURCE and of nothing else - checked by driving
	 * it from a non-zero starting state in both directions, so a
	 * "left it alone" bug and a "hardcoded it" bug both fail.
	 */
	{
		FFlowVizVolumeShaderParameters VolumeParams;
		if (!TestTrue(TEXT("the mock domain builds shader parameters"),
				MakeMockParameters(VolumeParams)))
		{
			return false;
		}

		// This is what the data path really produces. Pin it, because the whole
		// block below is reasoning about it.
		TestEqual(TEXT("MakeShaderParameters writes bHasStatusTexture=0 (a placeholder, "
					   "not a claim that no status texture exists)"),
			static_cast<int32>(VolumeParams.bHasStatusTexture), 0);

		FFlowVizVolumeRayMarchParameters RayParams;
		FlowVizRayMarch::FillDefaults(RayParams);

		// Poison both flags first. If FillFromVolumeParameters copied the
		// placeholder through, this 1 becomes a 0 and the next assertion fails;
		// if it correctly leaves them alone, the 1 survives. Starting from 0
		// would make both behaviours look identical - the classic criterion
		// satisfied by something other than the property it names.
		RayParams.bHasStatusTexture = 1u;
		RayParams.bHasVectorTexture = 1u;
		FlowVizRayMarch::FillFromVolumeParameters(VolumeParams, RayParams);

		TestEqual(TEXT("FillFromVolumeParameters does NOT launder the bHasStatusTexture "
					   "placeholder into the ray-march block"),
			static_cast<int32>(RayParams.bHasStatusTexture), 1);
		TestEqual(TEXT("...nor the bHasVectorTexture placeholder"),
			static_cast<int32>(RayParams.bHasVectorTexture), 1);

		// It did copy the rows it is supposed to copy. Without this the
		// assertions above would also pass for a function that copies nothing.
		TestEqual(TEXT("...but it DID copy the geometry rows (MinVoxelSpacing)"),
			RayParams.MinVoxelSpacing, VolumeParams.MinVoxelSpacing);
		TestEqual(TEXT("...and UVWBias, the half-voxel that must never be recomputed"),
			RayParams.UVWBias.Z, VolumeParams.UVWBias.Z);

		// Now the flag's real source. Null status texture -> 0, always.
		FRHITexture* const FakeField = reinterpret_cast<FRHITexture*>(0x1);
		FRHITexture* const FakeStatus = reinterpret_cast<FRHITexture*>(0x2);

		RayParams.bHasStatusTexture = 1u;
		TestTrue(TEXT("SetVolumeTextures accepts a field texture with no status texture"),
			FlowVizRayMarch::SetVolumeTextures(RayParams, FakeField, nullptr, false));
		TestEqual(TEXT("FAIL CLOSED: no status texture binds bHasStatusTexture=0, which the "
					   "shader reads as 'reject every voxel'. This is the flag that decides "
					   "whether 'cannot tell' renders as 'valid'."),
			static_cast<int32>(RayParams.bHasStatusTexture), 0);

		// And the other direction, so the check cannot be satisfied by a
		// function that hardcodes 0.
		RayParams.bHasStatusTexture = 0u;
		FlowVizRayMarch::SetVolumeTextures(RayParams, FakeField, FakeStatus, true);
		TestEqual(TEXT("a bound status texture binds bHasStatusTexture=1"),
			static_cast<int32>(RayParams.bHasStatusTexture), 1);
		TestEqual(TEXT("and bHasVectorTexture follows its own argument"),
			static_cast<int32>(RayParams.bHasVectorTexture), 1);

		// Nothing to march without a field texture, and saying so is the
		// difference between an empty image and an unreported failure.
		TestFalse(TEXT("SetVolumeTextures reports failure when there is no field texture"),
			FlowVizRayMarch::SetVolumeTextures(RayParams, nullptr, FakeStatus, false));

		// The masks the fail-closed policy is evaluated against. Status bit 0 is
		// Valid and Unknown is 0, so an all-zero status byte satisfies no
		// required bit - the shader's rejection is not merely a flag check.
		TestEqual(TEXT("RequiredStatusMask is Valid, so a zeroed status byte (Unknown) "
					   "satisfies nothing"),
			RayParams.RequiredStatusMask,
			static_cast<uint32>(FlowVizVoxelStatus::Valid));
		TestTrue(TEXT("InvalidStatusMask covers NaN, Infinite and Masked"),
			(RayParams.InvalidStatusMask & static_cast<uint32>(FlowVizVoxelStatus::NaN)) != 0
				&& (RayParams.InvalidStatusMask
					   & static_cast<uint32>(FlowVizVoxelStatus::Infinite)) != 0
				&& (RayParams.InvalidStatusMask
					   & static_cast<uint32>(FlowVizVoxelStatus::Masked)) != 0);
	}

	/* == Anisotropy, and the half-voxel the shader must not recompute ======== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: the shader's UVW mapping
	 * disagreeing with the CPU's.
	 *
	 * FlowVizVolumeRayMarch.usf reduces the whole local-space-to-texture mapping
	 * to one line - `LocalPos * UVWScale + UVWBias`. That line is only correct
	 * if UVWScale is genuinely per-axis and if UVWBias already carries the
	 * cell/point half-voxel. Both are true by construction in
	 * MakeShaderParameters, and both are exactly the sort of thing a later edit
	 * "simplifies" into a scalar or a recomputed +0.5.
	 *
	 * So this replicates the SHADER's arithmetic in C++ and compares it against
	 * FFlowVizVolumeTransform::PhysicalToTextureUVW, which is a separate
	 * implementation written from the grid definition rather than from the
	 * packed parameters. Two independent paths to the same texel is a
	 * differential test; asserting UVWScale equals 1/(Spacing*Counts) would only
	 * restate the code that produced it.
	 *
	 * The fixture's three spacings are 0.09375, 0.0625 and 0.0416667: no two
	 * equal, none a multiple of another. On a cubic fixture every assertion here
	 * passes against a scalar-spacing bug.
	 */
	{
		// The shader's line, transcribed. If FlowVizLocalToUVW in the .usf ever
		// stops matching this, the comment above it is the place to look.
		auto ShaderLocalToUVW = [](const FFlowVizVolumeShaderParameters& P, const FVector3f& Local)
		{
			return FVector3f(
				Local.X * P.UVWScale.X + P.UVWBias.X,
				Local.Y * P.UVWScale.Y + P.UVWBias.Y,
				Local.Z * P.UVWScale.Z + P.UVWBias.Z);
		};

		const FCFDVizGrid Grid = MakeMockGrid();

		// Anisotropy is real, and it is real in the fixture. If this fails the
		// rest of the block proves nothing.
		TestNotEqual(TEXT("the fixture's X and Y spacings differ"), Grid.Spacing.X, Grid.Spacing.Y);
		TestNotEqual(TEXT("the fixture's Y and Z spacings differ"), Grid.Spacing.Y, Grid.Spacing.Z);
		TestNotEqual(TEXT("the fixture's X and Z spacings differ"), Grid.Spacing.X, Grid.Spacing.Z);

		for (const ECFDVizAssociation Association :
			 { ECFDVizAssociation::Cell, ECFDVizAssociation::Point })
		{
			const bool bPoint = (Association == ECFDVizAssociation::Point);
			const TCHAR* const Label = bPoint ? TEXT("Point") : TEXT("Cell");

			FFlowVizVolumeTransform Transform;
			Transform.Grid = Grid;
			Transform.Association = Association;

			FFlowVizVolumeLayout Layout;
			if (!TestTrue(*FString::Printf(TEXT("[%s] layout builds"), Label),
					FFlowVizVolumeLayout::Make(
						Transform.GetValueCounts(), 1, ECFDVizDataType::Float16, Layout).IsOk()))
			{
				continue;
			}

			FFlowVizVolumeShaderParameters P;
			if (!TestTrue(*FString::Printf(TEXT("[%s] shader parameters build"), Label),
					Transform.MakeShaderParameters(Layout, P).IsOk()))
			{
				continue;
			}

			// Three corners and a centre, chosen so an axis swap moves the
			// answer. A single sample on the diagonal would not.
			const FIntVector Counts = Transform.GetValueCounts();
			const FVector3f PhysicalSize(
				static_cast<float>(Grid.Spacing.X * Grid.Dimensions.X),
				static_cast<float>(Grid.Spacing.Y * Grid.Dimensions.Y),
				static_cast<float>(Grid.Spacing.Z * Grid.Dimensions.Z));

			const FVector3f Probes[] = {
				FVector3f(0.0f, 0.0f, 0.0f),
				PhysicalSize,
				PhysicalSize * 0.5f,
				FVector3f(PhysicalSize.X * 0.25f, PhysicalSize.Y * 0.9f, PhysicalSize.Z * 0.1f),
			};

			for (const FVector3f& Local : Probes)
			{
				// The CPU's independent answer. Local space is offset from the
				// grid origin, which is zero for this fixture but spelled out so
				// the relationship is visible.
				const FVector Solver = FVector(Local) + Grid.Origin;
				const FVector Expected = Transform.PhysicalToTextureUVW(Solver);
				const FVector3f Actual = ShaderLocalToUVW(P, Local);

				TestNearlyEqual(*FString::Printf(
						TEXT("[%s] the shader's LocalPos*UVWScale+UVWBias agrees with "
							 "PhysicalToTextureUVW on U at local (%.4f, %.4f, %.4f)"),
						Label, Local.X, Local.Y, Local.Z),
					static_cast<double>(Actual.X), Expected.X, 1e-5);
				TestNearlyEqual(*FString::Printf(TEXT("[%s] ...and on V"), Label),
					static_cast<double>(Actual.Y), Expected.Y, 1e-5);
				TestNearlyEqual(*FString::Printf(TEXT("[%s] ...and on W"), Label),
					static_cast<double>(Actual.Z), Expected.Z, 1e-5);
			}

			// THE HALF-VOXEL, NAMED. Voxel i must land on texel i's CENTRE,
			// (i + 0.5)/N, for BOTH associations - that is what UVWBias exists
			// to arrange, and it differs between them by half a cell and one
			// texel per axis. Checked on the Z axis, whose count is smallest
			// (24 or 25), so a half-texel error is 2% of the range rather than
			// 0.4% and cannot hide inside the tolerance.
			const FVector3f VoxelZeroLocal = bPoint
				? FVector3f(0.0f, 0.0f, 0.0f)
				: FVector3f(
					  static_cast<float>(Grid.Spacing.X * 0.5),
					  static_cast<float>(Grid.Spacing.Y * 0.5),
					  static_cast<float>(Grid.Spacing.Z * 0.5));
			const FVector3f VoxelZeroUVW = ShaderLocalToUVW(P, VoxelZeroLocal);

			TestNearlyEqual(*FString::Printf(
					TEXT("[%s] voxel 0 lands on texel 0's CENTRE in W: 0.5/%d, not 0 and not "
						 "0.5/%d - the half-voxel UVWBias folds in and the shader must never "
						 "recompute"),
					Label, Counts.Z, Grid.Dimensions.Z),
				static_cast<double>(VoxelZeroUVW.Z), 0.5 / static_cast<double>(Counts.Z), 1e-5);
			TestNearlyEqual(*FString::Printf(TEXT("[%s] ...and in U: 0.5/%d"), Label, Counts.X),
				static_cast<double>(VoxelZeroUVW.X), 0.5 / static_cast<double>(Counts.X), 1e-5);

			// Cell and Point differ. Without this, a build that ignored
			// Association entirely would satisfy one of the two loops above and
			// the other would never be reached in a shape that disagreed.
			if (bPoint)
			{
				TestEqual(*FString::Printf(
						TEXT("[Point] the texture is one texel larger per axis than the cell count")),
					Counts, FIntVector(Grid.Dimensions.X + 1, Grid.Dimensions.Y + 1, Grid.Dimensions.Z + 1));
				TestTrue(TEXT("[Point] UVWBias is non-zero - the point association's half texel"),
					P.UVWBias.Z > 0.0f);
			}
			else
			{
				TestEqual(TEXT("[Cell] the texture matches the cell count"), Counts, Grid.Dimensions);
				TestEqual(TEXT("[Cell] UVWBias is zero - the half cell is already in the position"),
					P.UVWBias.Z, 0.0f);
			}

			// Per-axis, and derived from nothing. Equal scales on unequal
			// spacings is the scalar-spacing bug in its final form.
			TestNotEqual(*FString::Printf(TEXT("[%s] UVWScale X and Z are independent"), Label),
				P.UVWScale.X, P.UVWScale.Z);
		}
	}

	/* == The step is scaled by the THINNEST axis ============================= */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a step length taken from
	 * the wrong spacing.
	 *
	 * StepVoxels is in VOXEL units and the shader turns it into a world length
	 * with `StepVoxels * MinVoxelSpacing`. Using the mean or the max instead
	 * still renders - it just steps over thin-axis features, which on this
	 * domain is the 1m vertical extent resolved by 24 cells. That is aliasing
	 * that looks like smoothing, and no image comparison catches it.
	 */
	{
		FFlowVizVolumeShaderParameters P;
		if (TestTrue(TEXT("the mock domain builds shader parameters"), MakeMockParameters(P)))
		{
			const FVector Spacing = MakeMockGrid().Spacing;
			const double Thinnest = FMath::Min3(Spacing.X, Spacing.Y, Spacing.Z);
			const double Thickest = FMath::Max3(Spacing.X, Spacing.Y, Spacing.Z);
			const double Mean = (Spacing.X + Spacing.Y + Spacing.Z) / 3.0;

			TestNearlyEqual(TEXT("MinVoxelSpacing is the THINNEST axis (Z here, 1m/24)"),
				static_cast<double>(P.MinVoxelSpacing), Thinnest, 1e-6);
			TestNearlyEqual(TEXT("MaxVoxelSpacing is the thickest (X here, 12m/128)"),
				static_cast<double>(P.MaxVoxelSpacing), Thickest, 1e-6);

			// The discriminating comparisons. Without these, "MinVoxelSpacing is
			// some spacing" would pass for the mean or the max.
			TestTrue(TEXT("MinVoxelSpacing is not the mean of the three spacings"),
				FMath::Abs(static_cast<double>(P.MinVoxelSpacing) - Mean) > 1e-4);
			TestTrue(TEXT("MinVoxelSpacing is strictly less than MaxVoxelSpacing, so the two "
						  "cannot have been derived from one number"),
				P.MinVoxelSpacing < P.MaxVoxelSpacing);

			// A half-voxel step on the thinnest axis is ~2.08cm here. Stepping
			// by the thickest instead would be 4.7cm - it would skip texels.
			const double ExpectedStep = FlowVizRayMarch::DefaultStepVoxels * Thinnest;
			TestTrue(TEXT("a default half-voxel step is shorter than the thinnest voxel, so no "
						  "texel on the Z axis is stepped over"),
				ExpectedStep < Thinnest);
			TestTrue(TEXT("...and the same step taken against the thickest axis WOULD skip Z "
						  "texels, which is the bug this pins"),
				FlowVizRayMarch::DefaultStepVoxels * Thickest > Thinnest);

			// InvVoxelSpacing is the gradient's 1/h, per axis, and its own
			// number rather than a reciprocal recomputed in float.
			TestNearlyEqual(TEXT("InvVoxelSpacing.Z is 1/Spacing.Z (the gradient's per-axis 1/h)"),
				static_cast<double>(P.InvVoxelSpacing.Z), 1.0 / Spacing.Z, 1e-3);
			TestNotEqual(TEXT("InvVoxelSpacing X and Z are independent"),
				P.InvVoxelSpacing.X, P.InvVoxelSpacing.Z);
		}
	}

	/* == A widened 3-component field: .w is a quiet NaN, by design ============ */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: the shader gating its
	 * component loop on TextureComponentCount instead of ComponentCount.
	 *
	 * There is no 3-channel 3D texture format, so a vector field is stored in 4
	 * channels and the pad carries a quiet NaN on purpose - reading it must
	 * produce something obviously wrong rather than a plausible zero. A
	 * magnitude computed over TextureComponentCount channels would therefore be
	 * NaN for every voxel of every vector field, which under FlowVizIsNaN paints
	 * the whole volume magenta. Loud, not silent - but only because the pad is
	 * NaN. If the pad were zero the same bug would render a correct-looking
	 * magnitude that was quietly wrong on 3-vectors, and nobody would find it.
	 *
	 * So the assertion is that the two counts DIFFER for a 3-vector, that
	 * FLOWVIZ_PAD_IS_NAN is the declared policy, and that the shader gates on
	 * the source count.
	 */
	{
		FFlowVizVolumeTransform Transform;
		Transform.Grid = MakeMockGrid();
		Transform.Association = ECFDVizAssociation::Cell;

		FFlowVizVolumeLayout Layout;
		if (TestTrue(TEXT("a 3-component float32 layout builds"),
				FFlowVizVolumeLayout::Make(
					Transform.GetValueCounts(), 3, ECFDVizDataType::Float32, Layout).IsOk()))
		{
			FFlowVizVolumeShaderParameters P;
			if (TestTrue(TEXT("a 3-component field builds shader parameters"),
					Transform.MakeShaderParameters(Layout, P).IsOk()))
			{
				TestEqual(TEXT("ComponentCount is 3 - what the DATA has, and what the shader "
							   "must loop over"),
					P.ComponentCount, 3);
				TestEqual(TEXT("TextureComponentCount is 4 - what the TEXTURE has. Reading .w "
							   "of a widened field is a bug; the pad is a quiet NaN."),
					P.TextureComponentCount, 4);
				TestNotEqual(TEXT("the two counts differ, which is the entire hazard"),
					P.ComponentCount, P.TextureComponentCount);
				// PadElementBits is the BIT PATTERN in the pad channel, not its
				// width. 0x7FC00000 is a float32 quiet NaN. Asserting the exact
				// pattern is the point: 0 would be a plausible zero, and the
				// entire reason the pad is NaN is that it must not be.
				TestEqual(TEXT("the pad channel carries the float32 quiet-NaN bit pattern "
							   "0x7FC00000, so reading .w of a widened field is LOUDLY wrong "
							   "rather than plausibly zero"),
					P.PadElementBits, 0x7FC00000u);
				TestEqual(TEXT("...and it is what FlowVizVolumeFormat::GetPadElementBits says, "
							   "so the parameter block cannot drift from the uploader"),
					P.PadElementBits,
					FlowVizVolumeFormat::GetPadElementBits(ECFDVizDataType::Float32));

				// The same for float16, whose quiet NaN is a different pattern.
				// Without this, a hardcoded 0x7FC00000 would pass above.
				TestEqual(TEXT("a float16 field's pad is the float16 quiet NaN 0x7E00, not the "
							   "float32 pattern"),
					FlowVizVolumeFormat::GetPadElementBits(ECFDVizDataType::Float16), 0x7E00u);

				// And the honest exception. An integer format has no NaN, so
				// the pad is zero and the gate on ComponentCount is the ONLY
				// protection there. Documented in the header rather than faked,
				// and pinned here so it stays documented.
				TestEqual(TEXT("uint8 has no NaN to write, so its pad is 0 - for integer fields "
							   "the ComponentCount gate is the only protection, and this is why "
							   "the shader gates on it rather than relying on the pad"),
					FlowVizVolumeFormat::GetPadElementBits(ECFDVizDataType::UInt8), 0u);

				// Same field as a scalar: the counts agree, so a shader that
				// confused them would still pass every scalar test in this file.
				// This is why the vector case had to be constructed explicitly.
				FFlowVizVolumeLayout ScalarLayout;
				FFlowVizVolumeShaderParameters ScalarP;
				if (FFlowVizVolumeLayout::Make(
						Transform.GetValueCounts(), 1, ECFDVizDataType::Float32, ScalarLayout).IsOk()
					&& Transform.MakeShaderParameters(ScalarLayout, ScalarP).IsOk())
				{
					TestEqual(TEXT("for a SCALAR field the two counts agree, which is why a "
								   "scalar fixture cannot detect this confusion"),
						ScalarP.ComponentCount, ScalarP.TextureComponentCount);
				}
			}
		}

		// The widening happens ONLY at 3 components. A format chooser that
		// padded everything would make TextureComponentCount useless as a
		// signal, and the assertions above would still pass.
		for (const TPair<int32, int32> Expected :
			 { TPair<int32, int32>(1, 1), TPair<int32, int32>(2, 2),
			   TPair<int32, int32>(3, 4), TPair<int32, int32>(4, 4) })
		{
			FFlowVizVolumeFormatChoice Choice;
			if (FlowVizVolumeFormat::ChooseTextureFormat(
					ECFDVizDataType::Float32, Expected.Key, Choice).IsOk())
			{
				TestEqual(*FString::Printf(
						TEXT("a %d-component float32 field occupies %d texture channels"),
						Expected.Key, Expected.Value),
					Choice.TextureComponentCount, Expected.Value);
				TestEqual(*FString::Printf(
						TEXT("...and is marked padded only at 3 components (%d)"), Expected.Key),
					Choice.bPadded, Expected.Key == 3);
			}
		}
	}

	/* == The solver-to-Unreal transform mirrors Y ============================ */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a proxy box wound as if
	 * the transform preserved handedness.
	 *
	 * ADR 004's solver-to-Unreal transform has a NEGATIVE determinant. A box
	 * built for the un-mirrored case comes out inside-out: with back-face
	 * culling it renders NOTHING, and with front-face culling it renders the
	 * INSIDE of a box. Both look like "the ray-march shader did not run", which
	 * is the most expensive possible way to be wrong here - it sends you to
	 * debug the shader.
	 *
	 * This is a C++-side fact, and it is asserted here rather than only in the
	 * component's own test because this shader's caller is the thing that gets
	 * it wrong.
	 */
	{
		// Global scope, not namespace CFDViz - that namespace holds only the
		// unit constants. A known trap in CFDVizTypes.h.
		const FMatrix SolverToUnreal = MakeSolverToUnrealTransform();
		const double Determinant = SolverToUnreal.Determinant();

		TestTrue(TEXT("the solver-to-Unreal transform has a NEGATIVE determinant (Y is "
					  "mirrored), so a proxy box needs its winding REVERSED - a box wound the "
					  "other way renders as nothing, or as its own inside, and both look like "
					  "a shader that did not run"),
			Determinant < 0.0);
		TestTrue(TEXT("TransformReversesWinding agrees, and is the one place that answer lives"),
			TransformReversesWinding(SolverToUnreal));

		// The volume's own local-to-world inherits the mirror. Without this, the
		// check above would only pin the bare axis conversion and a
		// transform composed with a second flip would slip through.
		FFlowVizVolumeTransform Transform;
		Transform.Grid = MakeMockGrid();
		Transform.Association = ECFDVizAssociation::Cell;
		const FMatrix LocalToUnreal = Transform.GetLocalToUnrealTransform();

		TestTrue(TEXT("...and the VOLUME's local-to-Unreal transform reverses winding too, "
					  "which is the matrix the ray-march proxy actually uses"),
			TransformReversesWinding(LocalToUnreal));
	}

	/* == The Scientific profile's defaults ==================================== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a default that quietly
	 * changes what the image MEANS.
	 *
	 * Two of these are VISUAL_QA rules, not preferences. Lighting on by default
	 * would make apparent brightness a function of surface orientation, so a
	 * reader could no longer map colour to value (rule 1). Two invalid causes
	 * sharing a colour would make "invalid" unattributable (rule 4). Both render
	 * beautifully while being wrong.
	 */
	{
		FFlowVizVolumeRayMarchParameters P;
		FlowVizRayMarch::FillDefaults(P);

		TestEqual(TEXT("VISUAL_QA rule 1: lighting is OFF by default, so brightness cannot "
					   "stand in for value"),
			static_cast<int32>(P.bEnableLighting), 0);
		TestEqual(TEXT("jitter is OFF by default (ADR 002 names per-ray jitter as a temporal "
					   "shimmer risk); a caller that wants it asks"),
			static_cast<int32>(P.bEnableJitter), 0);
		TestEqual(TEXT("front-to-back alpha compositing is the default mode"),
			P.CompositeMode, static_cast<uint32>(EFlowVizCompositeMode::Alpha));
		TestEqual(TEXT("FillDefaults binds no textures, so a caller who forgot SetVolumeTextures "
					   "cannot render a plausible empty volume"),
			static_cast<int32>(P.bHasStatusTexture), 0);
		TestTrue(TEXT("MaxSteps is bounded, so no caller can ask the GPU for an unbounded loop"),
			P.MaxSteps >= 1u && P.MaxSteps <= FlowVizRayMarch::MaxStepsLimit);
		TestTrue(TEXT("the default crop box covers the whole domain"),
			P.CropBoxMin.IsNearlyZero() && P.CropBoxMax.Equals(FVector3f(1.0f, 1.0f, 1.0f)));
		TestEqual(TEXT("no clipping planes by default"), static_cast<int32>(P.NumClipPlanes), 0);

		// VISUAL_QA rule 4: five causes, five DISTINCT colours. Compared
		// pairwise, because "they are all set" passes when two are identical -
		// and two identical flag colours is exactly the unattributable-invalid
		// failure the rule forbids.
		const TPair<const TCHAR*, FLinearColor> Flags[] = {
			{ TEXT("NaN"), P.NaNColor },
			{ TEXT("Masked"), P.MaskedColor },
			{ TEXT("NoData"), P.NoDataColor },
			{ TEXT("UnderRange"), P.UnderRangeColor },
			{ TEXT("OverRange"), P.OverRangeColor },
		};
		for (int32 I = 0; I < UE_ARRAY_COUNT(Flags); ++I)
		{
			for (int32 J = I + 1; J < UE_ARRAY_COUNT(Flags); ++J)
			{
				TestFalse(*FString::Printf(
						TEXT("VISUAL_QA rule 4: the %s and %s colours are DISTINCT, so an "
							 "invalid voxel says WHY it is invalid"),
						Flags[I].Key, Flags[J].Key),
					Flags[I].Value.Equals(Flags[J].Value, 0.02f));
			}
		}

		// And none of them is the colormap's minimum. Viridis starts at a very
		// dark blue-purple, near-black; a flag colour that dark would read as
		// low data. This is the specific "never the colormap minimum" clause.
		for (const TPair<const TCHAR*, FLinearColor>& Flag : Flags)
		{
			if (Flag.Value.A <= 0.0f)
			{
				continue; // NoData is deliberately transparent: absent, not dark.
			}
			const float MaxChannel = FMath::Max3(Flag.Value.R, Flag.Value.G, Flag.Value.B);
			TestTrue(*FString::Printf(
					TEXT("the %s colour is not near-black, so it cannot be read as the "
						 "colormap's minimum (VISUAL_QA rule 4)"),
					Flag.Key),
				MaxChannel > 0.2f);
		}
	}

	/* == The camera: perspective AND orthographic ============================ */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a camera basis that is not
	 * orthonormal, and an ortho frustum that does not contain the domain.
	 *
	 * A non-orthonormal basis skews the image - a shear that reads as a
	 * perspective effect and is very hard to see. An ortho extent too small
	 * crops the domain silently; too large shrinks it. Neither errors.
	 */
	{
		FFlowVizVolumeShaderParameters VolumeParams;
		if (TestTrue(TEXT("the mock domain builds shader parameters"),
				MakeMockParameters(VolumeParams)))
		{
			const FIntPoint OutputSize(320, 240);

			for (const bool bOrthographic : { true, false })
			{
				FFlowVizVolumeRayMarchParameters P;
				FlowVizRayMarch::FillDefaults(P);
				FlowVizRayMarch::FillFromVolumeParameters(VolumeParams, P);
				FlowVizRayMarch::SetLookAtCamera(
					P, FVector3f(0.0f, 1.0f, -0.35f), OutputSize, bOrthographic);

				const TCHAR* const Label = bOrthographic ? TEXT("ortho") : TEXT("perspective");

				TestNearlyEqual(*FString::Printf(TEXT("[%s] Forward is unit length"), Label),
					P.RayCameraForward.Size(), 1.0f, 1e-4f);
				TestNearlyEqual(*FString::Printf(TEXT("[%s] Right is unit length"), Label),
					P.RayCameraRight.Size(), 1.0f, 1e-4f);
				TestNearlyEqual(*FString::Printf(TEXT("[%s] Up is unit length"), Label),
					P.RayCameraUp.Size(), 1.0f, 1e-4f);

				// Mutually perpendicular. Three unit vectors that are not
				// orthogonal produce a sheared image that still looks like a
				// render, so length alone is not enough.
				TestNearlyEqual(*FString::Printf(TEXT("[%s] Forward and Right are perpendicular"), Label),
					FVector3f::DotProduct(P.RayCameraForward, P.RayCameraRight), 0.0f, 1e-4f);
				TestNearlyEqual(*FString::Printf(TEXT("[%s] Forward and Up are perpendicular"), Label),
					FVector3f::DotProduct(P.RayCameraForward, P.RayCameraUp), 0.0f, 1e-4f);
				TestNearlyEqual(*FString::Printf(TEXT("[%s] Right and Up are perpendicular"), Label),
					FVector3f::DotProduct(P.RayCameraRight, P.RayCameraUp), 0.0f, 1e-4f);

				TestEqual(*FString::Printf(TEXT("[%s] the projection flag is set as asked"), Label),
					static_cast<int32>(P.bOrthographic), bOrthographic ? 1 : 0);
				TestEqual(*FString::Printf(TEXT("[%s] the output size is carried through"), Label),
					static_cast<int32>(P.OutputSizeX), OutputSize.X);

				// The eye is outside the domain and looking at it. A camera
				// placed inside renders a partial volume that looks like a
				// crop-box bug.
				const FVector3f Center = P.PhysicalSize * 0.5f;
				const float Radius = P.PhysicalSize.Size() * 0.5f;
				const FVector3f ToCenter = Center - P.RayCameraOrigin;

				TestTrue(*FString::Printf(
						TEXT("[%s] the eye is OUTSIDE the domain's bounding sphere"), Label),
					ToCenter.Size() > Radius);
				TestNearlyEqual(*FString::Printf(
						TEXT("[%s] ...and Forward points at the domain centre"), Label),
					FVector3f::DotProduct(ToCenter.GetSafeNormal(), P.RayCameraForward), 1.0f, 1e-3f);
			}

			// The ortho frustum must CONTAIN the bounding sphere - checked
			// against the domain's own size, not against the number the
			// function computed, so restating the implementation is not enough
			// to pass.
			FFlowVizVolumeRayMarchParameters P;
			FlowVizRayMarch::FillDefaults(P);
			FlowVizRayMarch::FillFromVolumeParameters(VolumeParams, P);
			FlowVizRayMarch::SetLookAtCamera(P, FVector3f(0.0f, 1.0f, 0.0f), OutputSize, true);

			const float DomainRadius = FVector3f(
				static_cast<float>(12.0), static_cast<float>(4.0), static_cast<float>(1.0)).Size() * 0.5f;

			TestTrue(TEXT("the ortho half-height covers the domain's bounding radius, so no part "
						  "of the volume is silently cropped out of frame"),
				P.OrthoHalfExtent.Y >= DomainRadius - 1e-3f);
			TestTrue(TEXT("...and the half-width follows the aspect ratio rather than the height"),
				P.OrthoHalfExtent.X > P.OrthoHalfExtent.Y);

			// A perspective camera's vertical FOV must follow the aspect too.
			FlowVizRayMarch::SetLookAtCamera(P, FVector3f(0.0f, 1.0f, 0.0f), OutputSize, false, 2.0f, 60.0f);
			TestTrue(TEXT("the perspective FOV is wider horizontally than vertically for a "
						  "landscape output"),
				P.TanHalfFov.X > P.TanHalfFov.Y);
			TestNearlyEqual(TEXT("...and the horizontal half-angle is the 60 degrees asked for"),
				static_cast<double>(P.TanHalfFov.X), FMath::Tan(FMath::DegreesToRadians(30.0)), 1e-4);
		}
	}

	/* == The uint/float permutation follows the STORED format ================ */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a uint texture read
	 * through a float declaration.
	 *
	 * Integer fields are stored in _UINT formats, never UNORM, so the sampler
	 * returns raw integers that the shader must convert explicitly. Reading a
	 * uint texture through a float Texture3D is undefined on Metal - it does not
	 * error, it reads as noise. The permutation is what keeps the declaration
	 * matched to the format, so the mapping from data type to permutation is the
	 * load-bearing part.
	 */
	{
		TestFalse(TEXT("Float32 uses the FLOAT permutation"),
			FFlowVizVolumeRayMarchCS::IsUintFieldFormat(ECFDVizDataType::Float32));
		TestFalse(TEXT("Float16 uses the FLOAT permutation"),
			FFlowVizVolumeRayMarchCS::IsUintFieldFormat(ECFDVizDataType::Float16));
		TestTrue(TEXT("UInt8 uses the UINT permutation - the texture is _UINT, not UNORM, so a "
					  "float declaration would read noise on Metal"),
			FFlowVizVolumeRayMarchCS::IsUintFieldFormat(ECFDVizDataType::UInt8));

		// Driven from the layout's chosen PIXEL FORMAT rather than from a
		// hand-written list, so the two cannot disagree. This is the assertion
		// that survives a new data type being added to ECFDVizDataType: the
		// three above would still pass while the new type read as noise.
		// Component counts 1 and 3 both, because the widened 3-component
		// formats are different enums from the scalar ones.
		int32 CoveredCases = 0;
		for (const ECFDVizDataType DataType :
			 { ECFDVizDataType::Float32, ECFDVizDataType::Float16, ECFDVizDataType::UInt8,
			   ECFDVizDataType::Float64 })
		{
			for (const int32 Components : { 1, 3 })
			{
				FFlowVizVolumeLayout Layout;
				if (!FFlowVizVolumeLayout::Make(
						FIntVector(8, 8, 8), Components, DataType, Layout).IsOk())
				{
					// Float64 is rejected by CVF 1.0 and never reaches a
					// texture. Not a gap: the type has no format to disagree with.
					continue;
				}
				++CoveredCases;

				const bool bFormatIsUint =
					(Layout.PixelFormat == PF_R8_UINT || Layout.PixelFormat == PF_R8G8_UINT
						|| Layout.PixelFormat == PF_R8G8B8A8_UINT
						|| Layout.PixelFormat == PF_R16_UINT || Layout.PixelFormat == PF_R32_UINT
						|| Layout.PixelFormat == PF_R32G32B32A32_UINT);

				TestEqual(*FString::Printf(
						TEXT("the permutation matches the chosen pixel format for data type %d "
							 "with %d components (format %d)"),
						static_cast<int32>(DataType), Components,
						static_cast<int32>(Layout.PixelFormat)),
					FFlowVizVolumeRayMarchCS::IsUintFieldFormat(DataType), bFormatIsUint);
			}
		}

		// The loop above proves nothing if every case was skipped - a `continue`
		// on all iterations passes silently. This is the guard against a check
		// satisfied by having run zero times.
		TestTrue(TEXT("the format/permutation loop actually examined some formats"),
			CoveredCases >= 6);
	}

	return true;
}

bool FFlowVizVolumeRayMarchShaderDeviceTest::RunTest(const FString& Parameters)
{
	// SKIP WITH A LOGGED REASON, never a silent pass. A shader that failed to
	// compile is indistinguishable from one that was never asked for, and a
	// green light that never ran is the one outcome this file must not produce.
	if (!GIsRHIInitialized || GUsingNullRHI)
	{
		AddWarning(TEXT(
			"SKIPPED: FlowViz.Render.RayMarchShaderDevice needs a real RHI device and this run has "
			"none (-nullrhi). NOTHING about shader compilation was verified. "
			"Re-run with: RHI=1 Tools/build_lock.sh ./Tools/run_tests.sh FlowViz.Render"));
		return true;
	}

	/* == The shader compiled ================================================= */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: a .usf that does not
	 * compile. ADR 002 accepts, as the price of a hand-written global shader,
	 * that shader errors surface at RUNTIME - so nothing in the build's
	 * "Result: Succeeded" says this shader exists. A shader that failed to
	 * compile renders nothing, which looks exactly like a feature that was never
	 * wired up. This is the only assertion in the project that can tell those
	 * two apart.
	 */
	{
		const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		if (!TestNotNull(TEXT("the global shader map exists"), ShaderMap))
		{
			return false;
		}

		// BOTH permutations. The uint one is not exercised by the float path at
		// all, and a compile error inside #if FLOWVIZ_FIELD_UINT would otherwise
		// stay invisible until a uint8 case was loaded.
		for (int32 UintPermutation = 0; UintPermutation <= 1; ++UintPermutation)
		{
			FFlowVizVolumeRayMarchCS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FFlowVizVolumeRayMarchCS::FFieldIsUint>(UintPermutation != 0);

			TShaderMapRef<FFlowVizVolumeRayMarchCS> ComputeShader(ShaderMap, PermutationVector);
			TestTrue(*FString::Printf(
					TEXT("FlowVizVolumeRayMarch.usf compiled for FLOWVIZ_FIELD_UINT=%d "
						 "(a failure here is a SHADER error; check the log for "
						 "'Failed to compile' above this line)"),
					UintPermutation),
				ComputeShader.IsValid());
		}
	}

	/* == The GPU reads the bytes the CPU wrote =============================== */
	/*
	 * WHAT THIS BLOCK CATCHES THAT NOTHING ELSE DOES: the shader's loose HLSL
	 * globals disagreeing with the C++ SHADER_PARAMETER block.
	 *
	 * Every static_assert in the header proves the two C++ structs agree with
	 * EACH OTHER. None of them can prove the .usf's declaration order matches,
	 * because the .usf is text compiled by a different toolchain. A member
	 * declared one slot out there gives a shader that reads spacing out of the
	 * dimensions slot - a wrong volume with plausible shape.
	 *
	 * FLOWVIZ_MODE_DIAGNOSTIC echoes all ten rows to pixels (0,0)..(9,0). The
	 * values are chosen so no two rows are equal and no component within a row
	 * is equal to another: a permuted read cannot coincidentally match.
	 *
	 * This block ALSO executes the shader, which is the only evidence in this
	 * file that anything renders at all.
	 */
	{
		using namespace FlowVizRayMarchTestFixture;

		FFlowVizVolumeShaderParameters VolumeParams;
		if (!TestTrue(TEXT("the mock domain builds shader parameters"),
				MakeMockParameters(VolumeParams)))
		{
			return false;
		}

		constexpr int32 Width = 16;
		constexpr int32 Height = 8;

		TArray<FLinearColor> Readback;
		bool bDispatched = false;

		ENQUEUE_RENDER_COMMAND(FlowVizRayMarchDiagnostic)(
			[&VolumeParams, &Readback, &bDispatched](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				// Float32 targets. OutValue.x carries a field value in solver
				// units; an 8- or 16-bit target would quantise it, which is the
				// silent rescaling the format rules forbid.
				const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
					FIntPoint(Width, Height),
					PF_A32B32G32R32F,
					FClearValueBinding::None,
					TexCreate_ShaderResource | TexCreate_UAV);

				FRDGTextureRef ColorTexture = GraphBuilder.CreateTexture(Desc, TEXT("FlowVizRayMarchColor"));
				FRDGTextureRef ValueTexture = GraphBuilder.CreateTexture(Desc, TEXT("FlowVizRayMarchValue"));

				FFlowVizVolumeRayMarchParameters* Params =
					GraphBuilder.AllocParameters<FFlowVizVolumeRayMarchParameters>();
				FlowVizRayMarch::FillDefaults(*Params);
				FlowVizRayMarch::FillFromVolumeParameters(VolumeParams, *Params);
				FlowVizRayMarch::SetLookAtCamera(*Params, FVector3f(0.0f, 1.0f, 0.0f),
					FIntPoint(Width, Height), true);
				Params->CompositeMode = static_cast<uint32>(EFlowVizCompositeMode::Diagnostic);

				// SHADER_USE_PARAMETER_STRUCT binds everything, so the textures
				// must be non-null even in a mode that never samples them.
				FTextureRHIRef Dummy3D;
				{
					const FRHITextureCreateDesc Create3D =
						FRHITextureCreateDesc::Create3D(TEXT("FlowVizRayMarchDummyField"))
							.SetExtent(1, 1)
							.SetDepth(1)
							.SetFormat(PF_R32_FLOAT)
							.SetFlags(ETextureCreateFlags::ShaderResource);
					Dummy3D = RHICmdList.CreateTexture(Create3D);
				}
				FTextureRHIRef DummyStatus;
				{
					const FRHITextureCreateDesc CreateStatus =
						FRHITextureCreateDesc::Create3D(TEXT("FlowVizRayMarchDummyStatus"))
							.SetExtent(1, 1)
							.SetDepth(1)
							.SetFormat(PF_R8_UINT)
							.SetFlags(ETextureCreateFlags::ShaderResource);
					DummyStatus = RHICmdList.CreateTexture(CreateStatus);
				}
				FTextureRHIRef DummyLut;
				{
					const FRHITextureCreateDesc CreateLut =
						FRHITextureCreateDesc::Create2D(TEXT("FlowVizRayMarchDummyLut"))
							.SetExtent(2, 1)
							.SetFormat(PF_FloatRGBA)
							.SetFlags(ETextureCreateFlags::ShaderResource);
					DummyLut = RHICmdList.CreateTexture(CreateLut);
				}

				FlowVizRayMarch::SetVolumeTextures(*Params, Dummy3D, DummyStatus, false);
				Params->TransferFunctionTexture = DummyLut;

				bDispatched = FlowVizRayMarch::AddRayMarchPass(
					GraphBuilder, GMaxRHIFeatureLevel, /*bFieldIsUint=*/false,
					Params, ColorTexture, ValueTexture);

				if (!bDispatched)
				{
					GraphBuilder.Execute();
					return;
				}

				FRHIGPUTextureReadback* GPUReadback =
					new FRHIGPUTextureReadback(TEXT("FlowVizRayMarchReadback"));
				AddEnqueueCopyPass(GraphBuilder, GPUReadback, ValueTexture);
				GraphBuilder.Execute();

				// The readback below is only meaningful once the GPU has
				// actually finished writing; reading earlier returns whatever
				// the staging buffer held, which is a plausible-looking lie.
				RHICmdList.SubmitAndBlockUntilGPUIdle();

				int32 RowPitch = 0;
				int32 BufferHeight = 0;
				void* Mapped = GPUReadback->Lock(RowPitch, &BufferHeight);
				if (Mapped != nullptr)
				{
					const FLinearColor* Pixels = static_cast<const FLinearColor*>(Mapped);
					Readback.SetNumUninitialized(Width);
					for (int32 X = 0; X < Width; ++X)
					{
						Readback[X] = Pixels[X];
					}
					GPUReadback->Unlock();
				}
				delete GPUReadback;
			});

		FlushRenderingCommands();

		if (!TestTrue(TEXT("the ray-march pass was added - false here means the shader is not "
						   "in the global shader map, i.e. it FAILED TO COMPILE"),
				bDispatched))
		{
			return false;
		}

		if (!TestEqual(TEXT("the diagnostic rows were read back from the GPU"),
				Readback.Num(), Width))
		{
			AddError(TEXT("The readback returned no pixels, so NOTHING about the GPU's view of "
						  "the constant buffer was verified. This is a harness failure, not a "
						  "pass."));
			return false;
		}

		// Row by row, against the CPU's own values. Every quantity here is
		// distinct from its neighbours in the fixture - three unequal spacings,
		// three unequal dimensions - so a member read one slot out cannot match.
		const auto CheckRow = [this, &Readback](
			int32 Index, const TCHAR* Name, const FVector4f& Expected)
		{
			const FLinearColor& Got = Readback[Index];
			TestNearlyEqual(*FString::Printf(TEXT("cbuffer row %d (%s): the GPU reads .x as the "
												  "CPU wrote it"), Index, Name),
				static_cast<double>(Got.R), static_cast<double>(Expected.X), 1e-3);
			TestNearlyEqual(*FString::Printf(TEXT("cbuffer row %d (%s): .y"), Index, Name),
				static_cast<double>(Got.G), static_cast<double>(Expected.Y), 1e-3);
			TestNearlyEqual(*FString::Printf(TEXT("cbuffer row %d (%s): .z"), Index, Name),
				static_cast<double>(Got.B), static_cast<double>(Expected.Z), 1e-3);
			TestNearlyEqual(*FString::Printf(TEXT("cbuffer row %d (%s): .w - the SCALAR sharing "
												  "the row, which is the slot a drifted member "
												  "lands in"), Index, Name),
				static_cast<double>(Got.A), static_cast<double>(Expected.W), 1e-3);
		};

		CheckRow(1, TEXT("PhysicalSize/VoxelVolume"),
			FVector4f(VolumeParams.PhysicalSize, VolumeParams.VoxelVolume));
		CheckRow(2, TEXT("VoxelSpacing/MinVoxelSpacing"),
			FVector4f(VolumeParams.VoxelSpacing, VolumeParams.MinVoxelSpacing));
		CheckRow(3, TEXT("InvVoxelSpacing/MaxVoxelSpacing"),
			FVector4f(VolumeParams.InvVoxelSpacing, VolumeParams.MaxVoxelSpacing));
		CheckRow(4, TEXT("VolumeDimensions/ComponentCount"),
			FVector4f(
				static_cast<float>(VolumeParams.VolumeDimensions.X),
				static_cast<float>(VolumeParams.VolumeDimensions.Y),
				static_cast<float>(VolumeParams.VolumeDimensions.Z),
				static_cast<float>(VolumeParams.ComponentCount)));
		CheckRow(6, TEXT("UVWScale/AssociationCode"),
			FVector4f(VolumeParams.UVWScale, static_cast<float>(VolumeParams.AssociationCode)));
		CheckRow(7, TEXT("UVWBias/DataTypeCode"),
			FVector4f(VolumeParams.UVWBias, static_cast<float>(VolumeParams.DataTypeCode)));

		// Row 8 carries the fail-closed flag, and it came from SetVolumeTextures
		// rather than from VolumeParams - which is the whole point of that
		// function. A dummy status texture WAS bound above, so the GPU must see 1.
		TestNearlyEqual(TEXT("cbuffer row 8: the GPU reads RequiredStatusMask as Valid"),
			static_cast<double>(Readback[8].R),
			static_cast<double>(VolumeParams.RequiredStatusMask), 1e-3);
		TestNearlyEqual(TEXT("cbuffer row 8: the GPU sees bHasStatusTexture=1, set by "
							 "SetVolumeTextures from the bound resource and NOT copied from the "
							 "data path's placeholder"),
			static_cast<double>(Readback[8].B), 1.0, 1e-3);

		// The distinctness the whole comparison rests on. Without it a permuted
		// read could match by coincidence and every CheckRow above would pass.
		TestTrue(TEXT("the fixture's rows are mutually distinct, so a member read one slot out "
					  "cannot match by coincidence"),
			!FMath::IsNearlyEqual(Readback[2].R, Readback[3].R, 1e-3f)
				&& !FMath::IsNearlyEqual(Readback[2].R, Readback[2].G, 1e-3f)
				&& !FMath::IsNearlyEqual(Readback[6].R, Readback[6].B, 1e-3f));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
