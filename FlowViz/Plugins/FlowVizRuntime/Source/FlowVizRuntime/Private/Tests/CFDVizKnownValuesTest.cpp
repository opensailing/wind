// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizArrayReader.h"
#include "CFDViz/CFDVizByteSource.h"
#include "CFDViz/CFDVizCrc32C.h"
#include "CFDViz/CFDVizManifest.h"
#include "CFDViz/CFDVizMeshReader.h"
#include "CFDViz/CFDVizVolumeReader.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE CROSS-LANGUAGE BRIDGE (format section 9).
 *
 * WHY THIS FILE EXISTS AND WHAT IT PROVES THAT THE OTHERS DO NOT.
 *
 * Every other reader test in this directory builds its fixture bytes in C++ and
 * reads them back with C++. That proves the reader is self-consistent, which is
 * necessary and is not the property the format needs. A reader and a writer that
 * share the same misunderstanding - the wrong brick order, a transposed axis, a
 * float16 rounding mode - agree perfectly with each other and disagree with the
 * specification.
 *
 * This test is the only place in the repository where a disagreement between the
 * Python implementation and the Unreal implementation can surface. It opens
 * `Samples/MockCylinderWake.cfdviz`, which was written by
 * `python -m cfdviz generate-mock`, and checks the values Unreal decodes against
 * `known_values.json`, whose numbers Python computed and recorded. Neither
 * implementation was written by reading the other; both were written from
 * `Docs/CFDVIZ_FORMAT.md`. That is what makes agreement evidence rather than a
 * tautology.
 *
 * TWO DESIGN CHOICES THAT DECIDE WHETHER THIS TEST IS WORTH ANYTHING.
 *
 * 1. **Comparison is on BITS, not on values within a tolerance.** Every sample
 *    in known_values.json carries an exact `bits` string. A tolerance-based
 *    comparison of float16 data would pass while the two implementations round
 *    differently, which is exactly the class of bug this bridge exists to
 *    catch - and it would keep passing as the error accumulated.
 *
 * 2. **A missing or empty corpus FAILS.** A bridge test that silently passes
 *    when it finds no samples reports "the implementations agree" when what
 *    happened is that nothing was compared. The sample counts are asserted
 *    positively before any comparison runs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizKnownValuesTest,
	"FlowViz.CFDViz.KnownValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace CFDVizKnownValuesTest
{
	/** The committed low-resolution sample, relative to the plugin's base directory. */
	FString GetSampleCaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}

		// BaseDir is <Project>/Plugins/FlowVizRuntime; the samples live beside
		// Plugins/, not inside the plugin.
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"));
	}

	/**
	 * Parse a "0x1A2B" string into an integer.
	 *
	 * Written out rather than using FParse or FCString::Strtoi64 so that a
	 * malformed string is a hard failure instead of silently becoming 0 - a
	 * bridge whose expected values all quietly parse to zero would compare
	 * decoded zeros against expected zeros and pass.
	 */
	bool TryParseHexBits(const FString& Text, uint64& OutValue)
	{
		if (!Text.StartsWith(TEXT("0x"), ESearchCase::IgnoreCase) || Text.Len() <= 2)
		{
			return false;
		}

		uint64 Value = 0;
		for (int32 Index = 2; Index < Text.Len(); ++Index)
		{
			const TCHAR Character = Text[Index];
			uint64 Digit = 0;
			if (Character >= TEXT('0') && Character <= TEXT('9'))
			{
				Digit = static_cast<uint64>(Character - TEXT('0'));
			}
			else if (Character >= TEXT('a') && Character <= TEXT('f'))
			{
				Digit = static_cast<uint64>(Character - TEXT('a')) + 10;
			}
			else if (Character >= TEXT('A') && Character <= TEXT('F'))
			{
				Digit = static_cast<uint64>(Character - TEXT('A')) + 10;
			}
			else
			{
				return false;
			}
			Value = (Value << 4) | Digit;
		}

		OutValue = Value;
		return true;
	}

	/** Assemble the little-endian integer stored at Component within a voxel's bytes. */
	bool TryGetComponentBits(
		const TArray<uint8>& VoxelBytes, int32 Component, int32 ElementBytes, uint64& OutBits)
	{
		const int64 Offset = static_cast<int64>(Component) * ElementBytes;
		if (Offset < 0 || Offset + ElementBytes > VoxelBytes.Num())
		{
			return false;
		}

		uint64 Bits = 0;
		for (int32 Index = ElementBytes - 1; Index >= 0; --Index)
		{
			Bits = (Bits << 8) | static_cast<uint64>(VoxelBytes[Offset + Index]);
		}
		OutBits = Bits;
		return true;
	}
}

bool FCFDVizKnownValuesTest::RunTest(const FString& Parameters)
{
	using namespace CFDVizKnownValuesTest;

	const FString CaseDir = GetSampleCaseDir();
	if (!TestFalse(TEXT("the plugin base directory resolved"), CaseDir.IsEmpty()))
	{
		return false;
	}

	const FString ManifestPath = FPaths::Combine(CaseDir, TEXT("manifest.json"));
	const FString KnownValuesPath = FPaths::Combine(CaseDir, TEXT("known_values.json"));

	// The sample is committed, so absence is a real failure and not a reason to
	// skip. A skipped bridge reads identically to a passing one in CI output.
	if (!TestTrue(TEXT("the committed sample case has a manifest"),
			FPaths::FileExists(ManifestPath)))
	{
		AddError(FString::Printf(
			TEXT("Expected the committed sample at %s. Regenerate with: "
				 "python -m cfdviz generate-mock --low-res --output %s"),
			*ManifestPath, *CaseDir));
		return false;
	}
	if (!TestTrue(TEXT("the committed sample case has known_values.json"),
			FPaths::FileExists(KnownValuesPath)))
	{
		return false;
	}

	/* --------------------------------------------------------------------- */
	/* Load both sides                                                         */
	/* --------------------------------------------------------------------- */

	FCFDVizCase Case;
	const FCFDVizResult LoadResult = FCFDVizCase::LoadFromFile(ManifestPath, Case);
	if (!TestTrue(FString::Printf(TEXT("Unreal parses the Python-written manifest: %s"),
			*LoadResult.ToString()), LoadResult.IsOk()))
	{
		return false;
	}

	FString KnownValuesJson;
	if (!TestTrue(TEXT("known_values.json is readable"),
			FFileHelper::LoadFileToString(KnownValuesJson, *KnownValuesPath)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(KnownValuesJson);
	if (!TestTrue(TEXT("known_values.json is valid JSON"),
			FJsonSerializer::Deserialize(JsonReader, Root) && Root.IsValid()))
	{
		return false;
	}

	/* --------------------------------------------------------------------- */
	/* The two implementations must be talking about the same case             */
	/* --------------------------------------------------------------------- */
	//
	// Without this, a stale known_values.json from a different case would be
	// compared against fresh data and the mismatches would look like reader bugs.
	{
		FString CaseId;
		TestTrue(TEXT("known_values declares a caseId"), Root->TryGetStringField(TEXT("caseId"), CaseId));
		TestEqual(TEXT("known_values describes the manifest's case"),
			CaseId, Case.Metadata.Id);

		FString FormatVersion;
		TestTrue(TEXT("known_values declares a formatVersion"),
			Root->TryGetStringField(TEXT("formatVersion"), FormatVersion));
		TestTrue(TEXT("known_values is a 1.x format"), FormatVersion.StartsWith(TEXT("1.")));

		// The CRC-32C check value. Both implementations must be using
		// Castagnoli, not the ordinary CRC-32 - they differ only in the
		// polynomial, so every OTHER assertion in this file would pass with the
		// wrong one right up until a payload CRC was verified.
		FString CrcCheck;
		if (Root->TryGetStringField(TEXT("crc32cCheck"), CrcCheck))
		{
			uint64 Expected = 0;
			TestTrue(TEXT("the CRC check value parses"), TryParseHexBits(CrcCheck, Expected));
			const uint8 CheckInput[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
			const uint32 Actual = CFDViz::Crc32C::Compute(CheckInput, 9);
			TestEqual(TEXT("Unreal's CRC-32C agrees with Python's check value"),
				static_cast<uint64>(Actual), Expected);
		}
	}

	/* --------------------------------------------------------------------- */
	/* Volume samples                                                          */
	/* --------------------------------------------------------------------- */

	const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
	if (!TestTrue(TEXT("known_values has a samples array"),
			Root->TryGetArrayField(TEXT("samples"), Samples) && Samples != nullptr))
	{
		return false;
	}

	// An empty corpus must fail. This is the assertion that stops the whole file
	// from degenerating into an unopposed pass.
	if (!TestTrue(TEXT("there is at least one field sample to compare"), Samples->Num() > 0))
	{
		return false;
	}

	// Opening one reader per (field, frame) rather than per sample: the samples
	// are grouped, and reopening would turn this into a file-handle benchmark.
	FString OpenPath;
	FCFDVizVolumeReader Reader;
	int32 ComparedFieldSamples = 0;

	for (const TSharedPtr<FJsonValue>& Entry : *Samples)
	{
		const TSharedPtr<FJsonObject>* Sample = nullptr;
		if (!Entry.IsValid() || !Entry->TryGetObject(Sample) || Sample == nullptr)
		{
			AddError(TEXT("a samples[] entry is not an object"));
			continue;
		}

		FString FieldId;
		int32 Frame = 0;
		int32 Component = 0;
		FString ExpectedBitsText;
		const TArray<TSharedPtr<FJsonValue>>* Voxel = nullptr;

		if (!(*Sample)->TryGetStringField(TEXT("field"), FieldId)
			|| !(*Sample)->TryGetNumberField(TEXT("frame"), Frame)
			|| !(*Sample)->TryGetStringField(TEXT("bits"), ExpectedBitsText)
			|| !(*Sample)->TryGetArrayField(TEXT("voxel"), Voxel)
			|| Voxel == nullptr || Voxel->Num() != 3)
		{
			AddError(TEXT("a samples[] entry is missing field/frame/bits/voxel"));
			continue;
		}
		(*Sample)->TryGetNumberField(TEXT("component"), Component);

		uint64 ExpectedBits = 0;
		if (!TestTrue(FString::Printf(TEXT("sample bits parse: %s"), *ExpectedBitsText),
				TryParseHexBits(ExpectedBitsText, ExpectedBits)))
		{
			continue;
		}

		const FCFDVizField* Field = Case.FindField(FName(*FieldId));
		if (!TestNotNull(*FString::Printf(TEXT("manifest declares field '%s'"), *FieldId), Field))
		{
			continue;
		}

		FString FramePath;
		const FCFDVizResult PathResult = Case.ResolveFieldFramePath(*Field, Frame, FramePath);
		if (!TestTrue(*FString::Printf(TEXT("resolve %s frame %d: %s"),
				*FieldId, Frame, *PathResult.ToString()), PathResult.IsOk()))
		{
			continue;
		}

		if (FramePath != OpenPath)
		{
			// bVerifyHeaderCrc stays true: the header CRC is itself a
			// cross-implementation check, since Python computed it.
			const FCFDVizResult OpenResult = Reader.Open(FramePath, /*bVerifyHeaderCrc=*/true);
			if (!TestTrue(*FString::Printf(TEXT("open %s: %s"),
					*FPaths::GetCleanFilename(FramePath), *OpenResult.ToString()), OpenResult.IsOk()))
			{
				OpenPath.Reset();
				continue;
			}
			OpenPath = FramePath;
		}

		const int32 I = static_cast<int32>((*Voxel)[0]->AsNumber());
		const int32 J = static_cast<int32>((*Voxel)[1]->AsNumber());
		const int32 K = static_cast<int32>((*Voxel)[2]->AsNumber());

		TArray<uint8> VoxelBytes;
		const FCFDVizResult VoxelResult = Reader.ReadVoxel(I, J, K, VoxelBytes);
		if (!TestTrue(*FString::Printf(TEXT("read %s[%d] voxel (%d,%d,%d): %s"),
				*FieldId, Frame, I, J, K, *VoxelResult.ToString()), VoxelResult.IsOk()))
		{
			continue;
		}

		const int32 ElementBytes = Reader.GetHeader().GetElementBytes();
		uint64 ActualBits = 0;
		if (!TestTrue(*FString::Printf(TEXT("component %d is inside the voxel of %s"),
				Component, *FieldId),
				TryGetComponentBits(VoxelBytes, Component, ElementBytes, ActualBits)))
		{
			continue;
		}

		// The load-bearing comparison. Bits, not values: two implementations
		// that round float16 differently produce numbers that pass any
		// reasonable tolerance and different bits.
		TestEqual(*FString::Printf(
				TEXT("%s frame %d voxel (%d,%d,%d) component %d matches Python bit-for-bit"),
				*FieldId, Frame, I, J, K, Component),
			ActualBits, ExpectedBits);

		++ComparedFieldSamples;
	}

	TestEqual(TEXT("every field sample was actually compared"),
		ComparedFieldSamples, Samples->Num());

	/* --------------------------------------------------------------------- */
	/* Mesh samples                                                            */
	/* --------------------------------------------------------------------- */
	//
	// Positions are float32 and compared as bits for the same reason. Triangle
	// indices and patch IDs are integers, where a bit comparison and a value
	// comparison coincide - they are here because a winding-order or
	// per-triangle/per-vertex mix-up shows up in the indices long before it
	// shows up in a rendered image.

	// Required, not optional. Wrapping this section in a "if the array is
	// present" test would let a known_values.json without meshSamples delete
	// thirty comparisons and still report a pass - the same unopposed-pass
	// failure the field section above is written to avoid.
	const TArray<TSharedPtr<FJsonValue>>* MeshSamples = nullptr;
	if (TestTrue(TEXT("known_values has a meshSamples array"),
			Root->TryGetArrayField(TEXT("meshSamples"), MeshSamples) && MeshSamples != nullptr)
		&& TestTrue(TEXT("there is at least one mesh sample to compare"), MeshSamples->Num() > 0))
	{
		FString OpenMeshId;
		FCFDVizMeshReader MeshReader;
		int32 ComparedMeshSamples = 0;

		for (const TSharedPtr<FJsonValue>& Entry : *MeshSamples)
		{
			const TSharedPtr<FJsonObject>* Sample = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Sample) || Sample == nullptr)
			{
				AddError(TEXT("a meshSamples[] entry is not an object"));
				continue;
			}

			FString Kind;
			FString MeshId;
			if (!(*Sample)->TryGetStringField(TEXT("kind"), Kind)
				|| !(*Sample)->TryGetStringField(TEXT("mesh"), MeshId))
			{
				AddError(TEXT("a meshSamples[] entry is missing kind/mesh"));
				continue;
			}

			if (MeshId != OpenMeshId)
			{
				FString MeshPath;
				const FCFDVizResult MeshPathResult = Case.ResolveMeshPath(FName(*MeshId), MeshPath);
				if (!TestTrue(*FString::Printf(TEXT("resolve mesh '%s': %s"),
						*MeshId, *MeshPathResult.ToString()), MeshPathResult.IsOk()))
				{
					OpenMeshId.Reset();
					continue;
				}

				const FCFDVizResult MeshOpen = MeshReader.LoadFromFile(MeshPath);
				if (!TestTrue(*FString::Printf(TEXT("open mesh '%s': %s"),
						*MeshId, *MeshOpen.ToString()), MeshOpen.IsOk()))
				{
					OpenMeshId.Reset();
					continue;
				}
				OpenMeshId = MeshId;
			}

			if (Kind == TEXT("position"))
			{
				int32 Vertex = 0;
				int32 Component = 0;
				FString ExpectedBitsText;
				if (!(*Sample)->TryGetNumberField(TEXT("vertex"), Vertex)
					|| !(*Sample)->TryGetStringField(TEXT("bits"), ExpectedBitsText))
				{
					AddError(TEXT("a position meshSample is missing vertex/bits"));
					continue;
				}
				(*Sample)->TryGetNumberField(TEXT("component"), Component);

				uint64 ExpectedBits = 0;
				if (!TestTrue(TEXT("mesh position bits parse"),
						TryParseHexBits(ExpectedBitsText, ExpectedBits)))
				{
					continue;
				}

				const TArray<FVector3f>& Positions = MeshReader.GetPositions();
				if (!TestTrue(*FString::Printf(TEXT("mesh '%s' has vertex %d"), *MeshId, Vertex),
						Positions.IsValidIndex(Vertex)))
				{
					continue;
				}
				if (!TestTrue(TEXT("position component is 0..2"), Component >= 0 && Component <= 2))
				{
					continue;
				}

				const float Value = Positions[Vertex][Component];
				uint32 ActualBits = 0;
				FMemory::Memcpy(&ActualBits, &Value, sizeof(ActualBits));

				TestEqual(*FString::Printf(
						TEXT("mesh '%s' vertex %d component %d matches Python bit-for-bit"),
						*MeshId, Vertex, Component),
					static_cast<uint64>(ActualBits), ExpectedBits);
				++ComparedMeshSamples;
			}
			else if (Kind == TEXT("triangle"))
			{
				int32 Triangle = 0;
				const TArray<TSharedPtr<FJsonValue>>* Indices = nullptr;
				if (!(*Sample)->TryGetNumberField(TEXT("triangle"), Triangle)
					|| !(*Sample)->TryGetArrayField(TEXT("indices"), Indices)
					|| Indices == nullptr || Indices->Num() != 3)
				{
					AddError(TEXT("a triangle meshSample is missing triangle/indices"));
					continue;
				}

				const TArray<uint32>& MeshIndices = MeshReader.GetIndices();
				const int32 Base = Triangle * 3;
				if (!TestTrue(*FString::Printf(TEXT("mesh '%s' has triangle %d"), *MeshId, Triangle),
						MeshIndices.IsValidIndex(Base + 2)))
				{
					continue;
				}

				// Compared in order, not as a set: a reversed winding is a set
				// match and a visible lighting failure.
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint32 Expected = static_cast<uint32>((*Indices)[Corner]->AsNumber());
					TestEqual(*FString::Printf(
							TEXT("mesh '%s' triangle %d corner %d matches Python (order significant)"),
							*MeshId, Triangle, Corner),
						MeshIndices[Base + Corner], Expected);
				}
				++ComparedMeshSamples;
			}
			else if (Kind == TEXT("patchId"))
			{
				int32 Triangle = 0;
				int32 Expected = 0;
				if (!(*Sample)->TryGetNumberField(TEXT("triangle"), Triangle)
					|| !(*Sample)->TryGetNumberField(TEXT("value"), Expected))
				{
					AddError(TEXT("a patchId meshSample is missing triangle/value"));
					continue;
				}

				// Patch IDs are per TRIANGLE, not per vertex. Indexing this
				// array by vertex is the mistake the format's comment warns
				// about, and it produces plausible-looking data.
				const TArray<uint32>& PatchIds = MeshReader.GetPatchIds();
				if (!TestTrue(*FString::Printf(TEXT("mesh '%s' has a patch ID for triangle %d"),
						*MeshId, Triangle), PatchIds.IsValidIndex(Triangle)))
				{
					continue;
				}

				TestEqual(*FString::Printf(TEXT("mesh '%s' triangle %d patch ID matches Python"),
						*MeshId, Triangle),
					PatchIds[Triangle], static_cast<uint32>(Expected));
				++ComparedMeshSamples;
			}
			else
			{
				AddError(FString::Printf(TEXT("unknown meshSample kind '%s'"), *Kind));
			}
		}

		TestEqual(TEXT("every mesh sample was actually compared"),
			ComparedMeshSamples, MeshSamples->Num());
	}

	/* --------------------------------------------------------------------- */
	/* Mesh-associated array samples (.cva)                                    */
	/* --------------------------------------------------------------------- */
	//
	// This section exists because its absence was the one real hole in the
	// bridge. CFDVizArrayReaderTest.cpp verifies the CVA reader against
	// fixtures CFDVizArrayReaderTest.cpp wrote, which establishes that the
	// reader is self-consistent and nothing whatever about whether it agrees
	// with the Python implementation. Every other format is checked here
	// against bytes Python produced; CVA was not, because the generator emitted
	// no .cva and arraySamples was an empty list. An empty list compares
	// nothing and passes.
	//
	// Required, not optional, for the same reason as meshSamples above: making
	// it conditional would let a regenerated known_values.json with an empty
	// arraySamples silently delete this whole section and still report green.

	const TArray<TSharedPtr<FJsonValue>>* ArraySamples = nullptr;
	if (TestTrue(TEXT("known_values has an arraySamples array"),
			Root->TryGetArrayField(TEXT("arraySamples"), ArraySamples) && ArraySamples != nullptr)
		&& TestTrue(TEXT("there is at least one array sample to compare"), ArraySamples->Num() > 0))
	{
		FString OpenArrayPath;
		FCFDVizArrayData ArrayData;
		int32 ComparedArraySamples = 0;

		for (const TSharedPtr<FJsonValue>& Entry : *ArraySamples)
		{
			const TSharedPtr<FJsonObject>* Sample = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Sample) || Sample == nullptr)
			{
				AddError(TEXT("an arraySamples[] entry is not an object"));
				continue;
			}

			FString Kind;
			FString RelativePath;
			if (!(*Sample)->TryGetStringField(TEXT("kind"), Kind)
				|| !(*Sample)->TryGetStringField(TEXT("path"), RelativePath))
			{
				AddError(TEXT("an arraySamples[] entry is missing kind/path"));
				continue;
			}

			// Samples arrive grouped by path, so this reopens only on change.
			if (RelativePath != OpenArrayPath)
			{
				const FString ArrayPath = FPaths::Combine(CaseDir, RelativePath);
				FCFDVizFileByteSource ArraySource;
				if (!TestTrue(*FString::Printf(TEXT("open array '%s'"), *RelativePath),
						ArraySource.Open(ArrayPath)))
				{
					OpenArrayPath.Reset();
					continue;
				}

				const FCFDVizResult ArrayRead = FCFDVizArrayReader::Read(ArraySource, ArrayData);
				if (!TestTrue(*FString::Printf(TEXT("read array '%s': %s"),
						*RelativePath, *ArrayRead.ToString()), ArrayRead.IsOk()))
				{
					OpenArrayPath.Reset();
					continue;
				}
				OpenArrayPath = RelativePath;
			}

			// The frame the file declares must be the frame Python sampled. A
			// reader that ignored frameIndex would otherwise answer every
			// question about frame 7 with frame 0's data and pass, because the
			// payload comparison alone cannot tell which frame it read.
			int32 ExpectedFrame = 0;
			if ((*Sample)->TryGetNumberField(TEXT("frame"), ExpectedFrame))
			{
				TestEqual(*FString::Printf(TEXT("array '%s' declares frame %d"),
						*RelativePath, ExpectedFrame),
					static_cast<int32>(ArrayData.Header.FrameIndex), ExpectedFrame);
			}

			if (Kind == TEXT("value"))
			{
				int32 EntityIndex = 0;
				int32 Component = 0;
				FString ExpectedBitsText;
				if (!(*Sample)->TryGetNumberField(TEXT("entity"), EntityIndex)
					|| !(*Sample)->TryGetNumberField(TEXT("component"), Component)
					|| !(*Sample)->TryGetStringField(TEXT("bits"), ExpectedBitsText))
				{
					AddError(TEXT("a value arraySample is missing entity/component/bits"));
					continue;
				}

				uint64 ExpectedBits = 0;
				if (!TestTrue(*FString::Printf(TEXT("array '%s' entity %d expected bits parse"),
						*RelativePath, EntityIndex),
						TryParseHexBits(ExpectedBitsText, ExpectedBits)))
				{
					continue;
				}

				// Bits, not values. The obstacle array deliberately carries a
				// NaN, and NaN != NaN: a value comparison would report a
				// mismatch on a correct reader and, worse, a reader that folded
				// NaN to 0.0 would compare 0.0 == 0.0 against a bridge that had
				// also been read as 0.0 and pass. Only the bit pattern
				// distinguishes 0x7FC00000 from 0x00000000.
				uint64 ActualBits = 0;
				if (!TestTrue(*FString::Printf(
						TEXT("array '%s' has entity %d component %d"),
						*RelativePath, EntityIndex, Component),
						ArrayData.TryGetValueBits(EntityIndex, Component, ActualBits)))
				{
					continue;
				}

				TestEqual(*FString::Printf(
						TEXT("array '%s' entity %d component %d matches Python bit-for-bit"),
						*RelativePath, EntityIndex, Component),
					ActualBits, ExpectedBits);
				++ComparedArraySamples;
			}
			else if (Kind == TEXT("statistics"))
			{
				int32 Component = 0;
				double ExpectedValidCount = 0.0;
				if (!(*Sample)->TryGetNumberField(TEXT("component"), Component)
					|| !(*Sample)->TryGetNumberField(TEXT("validCount"), ExpectedValidCount))
				{
					AddError(TEXT("a statistics arraySample is missing component/validCount"));
					continue;
				}

				if (!TestTrue(*FString::Printf(TEXT("array '%s' carries frame statistics"),
						*RelativePath), ArrayData.FrameStatistics.IsSet()))
				{
					continue;
				}

				const FCFDVizArrayStatistics& Statistics = ArrayData.FrameStatistics.GetValue();
				if (!Statistics.ValidCount.IsValidIndex(Component))
				{
					AddError(FString::Printf(TEXT("array '%s' has no validCount for component %d"),
						*RelativePath, Component));
					continue;
				}

				// validCount is why a statistics sample exists at all. It is the
				// only field distinguishing a reader which EXCLUDES non-finite
				// values from one which folds them to zero and counts them - a
				// difference invisible in the payload that changes every mean
				// the UI displays.
				TestEqual(*FString::Printf(
						TEXT("array '%s' component %d validCount matches Python"),
						*RelativePath, Component),
					static_cast<int64>(Statistics.ValidCount[Component]),
					static_cast<int64>(ExpectedValidCount));

				// The three moments are compared too, and this is not padding:
				// a mutation campaign swapped ParseStatisticsSection's maximum
				// and mean reads and the test above passed anyway. The four
				// statistics sections are the same size and all full of
				// plausible doubles, so reading them in the wrong order yields
				// a reader that works, reports no error, and scales every
				// colour map in the UI by the wrong number - exactly the defect
				// CFDVizArrayReader.cpp's own comment warns about.
				//
				// Bits again, not values, for the reason given above: these are
				// doubles that can legitimately be non-finite, and only the bit
				// pattern separates a preserved NaN from a folded zero.
				const auto CompareMoment =
					[&](const TCHAR* Field, const TArray<double>& Values)
				{
					const TSharedPtr<FJsonObject>* Moment = nullptr;
					FString ExpectedText;
					if (!(*Sample)->TryGetObjectField(Field, Moment) || Moment == nullptr
						|| !(*Moment)->TryGetStringField(TEXT("bits"), ExpectedText))
					{
						AddError(FString::Printf(
							TEXT("array '%s' statistics sample has no %s.bits"),
							*RelativePath, Field));
						return false;
					}

					uint64 Expected = 0;
					if (!TestTrue(*FString::Printf(TEXT("array '%s' %s bits parse"),
							*RelativePath, Field),
							TryParseHexBits(ExpectedText, Expected)))
					{
						return false;
					}

					if (!Values.IsValidIndex(Component))
					{
						AddError(FString::Printf(TEXT("array '%s' has no %s for component %d"),
							*RelativePath, Field, Component));
						return false;
					}

					uint64 Actual = 0;
					const double Value = Values[Component];
					FMemory::Memcpy(&Actual, &Value, sizeof(Actual));
					TestEqual(*FString::Printf(
							TEXT("array '%s' component %d %s matches Python bit-for-bit"),
							*RelativePath, Component, Field),
						Actual, Expected);
					return true;
				};

				// All three are evaluated - no short-circuit - so one missing
				// field does not hide the other two.
				const bool bMinimum = CompareMoment(TEXT("minimum"), Statistics.Minimum);
				const bool bMaximum = CompareMoment(TEXT("maximum"), Statistics.Maximum);
				const bool bMean = CompareMoment(TEXT("mean"), Statistics.Mean);
				if (!(bMinimum && bMaximum && bMean))
				{
					continue;
				}

				++ComparedArraySamples;
			}
			else
			{
				AddError(FString::Printf(TEXT("unknown arraySample kind '%s'"), *Kind));
			}
		}

		// Every declared sample must have been compared, not merely iterated. A
		// `continue` above skips a comparison, and without this the test would
		// report a pass having checked nothing.
		TestEqual(TEXT("every array sample was actually compared"),
			ComparedArraySamples, ArraySamples->Num());
	}

	/* --------------------------------------------------------------------- */
	/* Declared mesh geometry counts                                           */
	/* --------------------------------------------------------------------- */
	//
	// known_values records the counts Python wrote. A reader that misparses the
	// vertex/triangle counts but happens to read vertex 0 correctly passes every
	// sample above.

	const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
	if (TestTrue(TEXT("known_values has a meshes array"),
			Root->TryGetArrayField(TEXT("meshes"), Meshes) && Meshes != nullptr)
		&& TestTrue(TEXT("at least one mesh is declared"), Meshes->Num() > 0))
	{
		int32 ComparedMeshes = 0;

		for (const TSharedPtr<FJsonValue>& Entry : *Meshes)
		{
			const TSharedPtr<FJsonObject>* MeshInfo = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(MeshInfo) || MeshInfo == nullptr)
			{
				AddError(TEXT("a meshes[] entry is not an object"));
				continue;
			}

			FString MeshId;
			int32 VertexCount = 0;
			int32 TriangleCount = 0;
			if (!(*MeshInfo)->TryGetStringField(TEXT("id"), MeshId)
				|| !(*MeshInfo)->TryGetNumberField(TEXT("vertexCount"), VertexCount)
				|| !(*MeshInfo)->TryGetNumberField(TEXT("triangleCount"), TriangleCount))
			{
				AddError(TEXT("a meshes[] entry is missing id/vertexCount/triangleCount"));
				continue;
			}

			FString MeshPath;
			if (!Case.ResolveMeshPath(FName(*MeshId), MeshPath).IsOk())
			{
				AddError(FString::Printf(TEXT("could not resolve mesh '%s'"), *MeshId));
				continue;
			}

			FCFDVizMeshReader CountReader;
			const FCFDVizResult MeshOpen = CountReader.LoadFromFile(MeshPath);
			if (!TestTrue(*FString::Printf(TEXT("open mesh '%s' for counts: %s"),
					*MeshId, *MeshOpen.ToString()), MeshOpen.IsOk()))
			{
				continue;
			}

			TestEqual(*FString::Printf(TEXT("mesh '%s' vertex count matches Python"), *MeshId),
				CountReader.GetVertexCount(), VertexCount);
			TestEqual(*FString::Printf(TEXT("mesh '%s' triangle count matches Python"), *MeshId),
				CountReader.GetTriangleCount(), TriangleCount);
			++ComparedMeshes;
		}

		TestEqual(TEXT("every declared mesh had its counts compared"),
			ComparedMeshes, Meshes->Num());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
