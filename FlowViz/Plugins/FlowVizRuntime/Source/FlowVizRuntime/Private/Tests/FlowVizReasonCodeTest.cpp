// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Render/FlowVizVolumeTexture.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

/**
 * The C++ enums and the .usf's #defines are the same numbers on two sides of a
 * boundary that no compiler crosses.
 *
 * WHAT THIS FILE CATCHES THAT NOTHING ELSE DOES: a renumbering. Every other
 * agreement in this plugin is pinned by a static_assert - the cbuffer offsets in
 * FlowVizVolumeRayMarchShader.h, the ten rows in FlowVizVolumeTexture.h - because
 * both sides are C++. These are not. FLOWVIZ_REASON_OVER_RANGE is text in a file
 * compiled by a different toolchain at runtime, and EFlowVizInvalidReason::OverRange
 * is a C++ enumerator; nothing in the build compares them. Change either one and
 * the plugin still compiles, the shader still compiles, every existing test still
 * passes, and OutValue.z starts meaning something else.
 *
 * SO THIS TEST PARSES THE SHADER SOURCE. It does not restate the values in a
 * third place - a hand-copied table would be a third thing to drift, and would
 * agree with a wrong .usf as happily as with a right one. It reads the same file
 * the shader compiler reads, through the same virtual path mapping
 * (/Plugin/FlowViz, set up in FlowVizRuntime.cpp), and compares the number it
 * finds there against the C++ constant itself. An edit to EITHER side fails it.
 *
 * WHAT IT STILL CANNOT DO, STATED PLAINLY: it proves the two tables agree, not
 * that the shader USES its table to mean what the enum says. A .usf that defined
 * FLOWVIZ_REASON_NAN as 8 and then OR-ed it in for a masked voxel passes here.
 * That second half is FlowViz.Render.VolumeMarch's: it renders NaN, infinite,
 * unknown, under-range and over-range fixtures on a real GPU and decodes
 * OutValue.z with the shipped FlowVizRayMarch::DecodeReason. The two together are
 * the pin; neither alone is.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizReasonCodeTest,
	"FlowViz.Render.ReasonCodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizReasonCodeFixture
{
	/** The shader this test parses, by the virtual path the compiler resolves. */
	static const TCHAR* const ShaderVirtualPath = TEXT("/Plugin/FlowViz/FlowVizVolumeRayMarch.usf");

	/** One `#define NAME VALUE` found in the shader, with the line it came from. */
	struct FShaderDefine
	{
		FString ValueText;
		int32 LineNumber = 0;
	};

	/**
	 * Every #define in a .usf, by name.
	 *
	 * Deliberately dumb: it does not evaluate expressions, follow #if, or resolve
	 * one define through another. Every constant this file pins is a literal, and
	 * a parser that quietly coped with something more would also quietly cope with
	 * a define it got wrong. Anything it cannot read is reported by name below
	 * rather than defaulting to zero - a table of expected values that all parsed
	 * to zero would compare zeros against zeros and pass.
	 */
	static TMap<FString, FShaderDefine> ParseDefines(const FString& Source)
	{
		TMap<FString, FShaderDefine> Defines;

		TArray<FString> Lines;
		Source.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			FString Line = Lines[Index].TrimStartAndEnd();
			if (!Line.StartsWith(TEXT("#define ")))
			{
				continue;
			}
			Line.MidInline(8, MAX_int32, EAllowShrinking::No);
			Line.TrimStartInline();

			// Name runs to the first space; a function-like macro would keep its
			// parenthesis here and simply never match an expected name.
			int32 Space = INDEX_NONE;
			if (!Line.FindChar(TEXT(' '), Space) && !Line.FindChar(TEXT('\t'), Space))
			{
				continue;
			}
			const FString Name = Line.Left(Space);

			FString Rest = Line.Mid(Space).TrimStart();

			// Stop at a trailing comment: several of these carry one.
			int32 CommentStart = INDEX_NONE;
			if (Rest.FindChar(TEXT('/'), CommentStart))
			{
				Rest.LeftInline(CommentStart, EAllowShrinking::No);
			}

			FShaderDefine Define;
			Define.ValueText = Rest.TrimEnd();
			Define.LineNumber = Index + 1;
			Defines.Add(Name, Define);
		}

		return Defines;
	}

	/**
	 * A HLSL integer literal, with the `u` suffix half of these carry.
	 *
	 * Returns false rather than 0 for anything it does not fully understand. See
	 * ParseDefines for why that distinction is load-bearing.
	 */
	static bool TryParseUint(const FString& Text, uint32& OutValue)
	{
		if (Text.IsEmpty())
		{
			return false;
		}

		FString Digits = Text;
		if (Digits.EndsWith(TEXT("u"), ESearchCase::IgnoreCase))
		{
			Digits.LeftChopInline(1, EAllowShrinking::No);
		}
		if (Digits.IsEmpty())
		{
			return false;
		}

		uint64 Value = 0;
		for (int32 Index = 0; Index < Digits.Len(); ++Index)
		{
			const TCHAR Character = Digits[Index];
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
			Value = Value * 10u + static_cast<uint64>(Character - TEXT('0'));
			if (Value > static_cast<uint64>(MAX_uint32))
			{
				return false;
			}
		}

		OutValue = static_cast<uint32>(Value);
		return true;
	}

	/** One pinned pair: a shader define, and the C++ constant it must equal. */
	struct FPinnedCode
	{
		const TCHAR* DefineName;
		uint32 CppValue;
		const TCHAR* CppName;
	};

	/**
	 * The prefixes this test claims to cover completely.
	 *
	 * Any define starting with one of these that is NOT in the table below is a
	 * failure, not a skip. Without that direction the test would pass on a shader
	 * that grew a FLOWVIZ_REASON_CLIPPED with no C++ enumerator behind it - a bit
	 * the renderer can set and no caller can name.
	 */
	static const TCHAR* const CoveredPrefixes[] = {
		TEXT("FLOWVIZ_REASON_"),
		TEXT("FLOWVIZ_STATUS_"),
		TEXT("FLOWVIZ_MODE_"),
		TEXT("FLOWVIZ_COMPONENT_"),
	};
}

bool FFlowVizReasonCodeTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizReasonCodeFixture;

	/* == The shader source, through the compiler's own path mapping =========== */

	const FString ShaderPath = GetShaderSourceFilePath(ShaderVirtualPath);
	if (!TestFalse(*FString::Printf(
				TEXT("the virtual path %s resolves to a file on disk. Empty here means the "
					 "/Plugin/FlowViz mapping FlowVizRuntime.cpp installs is gone, and NOTHING "
					 "about the reason codes was compared"),
				ShaderVirtualPath),
			ShaderPath.IsEmpty()))
	{
		return false;
	}

	FString Source;
	if (!TestTrue(*FString::Printf(TEXT("the shader source at %s was read"), *ShaderPath),
			FFileHelper::LoadFileToString(Source, *ShaderPath)))
	{
		return false;
	}

	const TMap<FString, FShaderDefine> Defines = ParseDefines(Source);

	// --- THE CONTROL, FIRST ---
	// An empty or near-empty parse would make every "expected == found" loop below
	// iterate over nothing and report success. The file has well over a dozen
	// defines; a handful means the parser broke, not that the shader shrank.
	if (!TestTrue(*FString::Printf(
				TEXT("CONTROL: the parser found %d defines in the shader. A parser that read "
					 "none - or a few - would make every comparison below vacuous while still "
					 "reporting success"),
				Defines.Num()),
			Defines.Num() >= 15))
	{
		return false;
	}

	/* == The table ============================================================ */
	/*
	 * The C++ side is read from the enum ITSELF, not transcribed. That is what
	 * makes an edit to the enum fail this test: there is no literal here to keep
	 * in step.
	 */
	const FPinnedCode Pinned[] = {
		{ TEXT("FLOWVIZ_REASON_NONE"),
		  static_cast<uint32>(EFlowVizInvalidReason::None),       TEXT("EFlowVizInvalidReason::None") },
		{ TEXT("FLOWVIZ_REASON_VALID"),
		  static_cast<uint32>(EFlowVizInvalidReason::Valid),      TEXT("EFlowVizInvalidReason::Valid") },
		{ TEXT("FLOWVIZ_REASON_UNKNOWN"),
		  static_cast<uint32>(EFlowVizInvalidReason::Unknown),    TEXT("EFlowVizInvalidReason::Unknown") },
		{ TEXT("FLOWVIZ_REASON_MASKED"),
		  static_cast<uint32>(EFlowVizInvalidReason::Masked),     TEXT("EFlowVizInvalidReason::Masked") },
		{ TEXT("FLOWVIZ_REASON_NAN"),
		  static_cast<uint32>(EFlowVizInvalidReason::NaN),        TEXT("EFlowVizInvalidReason::NaN") },
		{ TEXT("FLOWVIZ_REASON_INFINITE"),
		  static_cast<uint32>(EFlowVizInvalidReason::Infinite),   TEXT("EFlowVizInvalidReason::Infinite") },
		{ TEXT("FLOWVIZ_REASON_UNDER_RANGE"),
		  static_cast<uint32>(EFlowVizInvalidReason::UnderRange), TEXT("EFlowVizInvalidReason::UnderRange") },
		{ TEXT("FLOWVIZ_REASON_OVER_RANGE"),
		  static_cast<uint32>(EFlowVizInvalidReason::OverRange),  TEXT("EFlowVizInvalidReason::OverRange") },

		// The status byte the reason codes are derived FROM. Same boundary, same
		// failure mode: FlowVizStatusToReason reads these to write those.
		{ TEXT("FLOWVIZ_STATUS_VALID"),
		  static_cast<uint32>(FlowVizVoxelStatus::Valid),    TEXT("FlowVizVoxelStatus::Valid") },
		{ TEXT("FLOWVIZ_STATUS_NAN"),
		  static_cast<uint32>(FlowVizVoxelStatus::NaN),      TEXT("FlowVizVoxelStatus::NaN") },
		{ TEXT("FLOWVIZ_STATUS_INFINITE"),
		  static_cast<uint32>(FlowVizVoxelStatus::Infinite), TEXT("FlowVizVoxelStatus::Infinite") },
		{ TEXT("FLOWVIZ_STATUS_MASKED"),
		  static_cast<uint32>(FlowVizVoxelStatus::Masked),   TEXT("FlowVizVoxelStatus::Masked") },

		// The composite mode the CPU writes into the cbuffer and the shader
		// branches on. A renumber here does not fail, it renders the wrong mode.
		{ TEXT("FLOWVIZ_MODE_ALPHA"),
		  static_cast<uint32>(EFlowVizCompositeMode::Alpha),      TEXT("EFlowVizCompositeMode::Alpha") },
		{ TEXT("FLOWVIZ_MODE_MAXIMUM"),
		  static_cast<uint32>(EFlowVizCompositeMode::Maximum),    TEXT("EFlowVizCompositeMode::Maximum") },
		{ TEXT("FLOWVIZ_MODE_MINIMUM"),
		  static_cast<uint32>(EFlowVizCompositeMode::Minimum),    TEXT("EFlowVizCompositeMode::Minimum") },
		{ TEXT("FLOWVIZ_MODE_AVERAGE"),
		  static_cast<uint32>(EFlowVizCompositeMode::Average),    TEXT("EFlowVizCompositeMode::Average") },
		{ TEXT("FLOWVIZ_MODE_ISOSURFACE"),
		  static_cast<uint32>(EFlowVizCompositeMode::IsoSurface), TEXT("EFlowVizCompositeMode::IsoSurface") },
		{ TEXT("FLOWVIZ_MODE_DIAGNOSTIC"),
		  static_cast<uint32>(EFlowVizCompositeMode::Diagnostic), TEXT("EFlowVizCompositeMode::Diagnostic") },

		// Only the magnitude selector is a named define; the others are an index
		// the shader clamps. Magnitude is the one that must not collide with one.
		{ TEXT("FLOWVIZ_COMPONENT_MAGNITUDE"),
		  static_cast<uint32>(EFlowVizComponentMode::Magnitude),  TEXT("EFlowVizComponentMode::Magnitude") },

		// Not a prefix group, so it is pinned individually: the shader's clip
		// plane array and the C++ SHADER_PARAMETER_ARRAY must be the same length.
		{ TEXT("FLOWVIZ_MAX_CLIP_PLANES"),
		  static_cast<uint32>(FlowVizRayMarch::MaxClipPlanes),    TEXT("FlowVizRayMarch::MaxClipPlanes") },
	};

	for (const FPinnedCode& Pin : Pinned)
	{
		const FShaderDefine* Found = Defines.Find(Pin.DefineName);
		if (!TestNotNull(*FString::Printf(
					TEXT("the shader defines %s. Missing means it was renamed or deleted while %s "
						 "still exists in C++, and the two sides no longer describe the same thing"),
					Pin.DefineName, Pin.CppName),
				Found))
		{
			continue;
		}

		uint32 ShaderValue = 0;
		if (!TestTrue(*FString::Printf(
					TEXT("%s's value '%s' (line %d) parses as an integer literal. An unreadable "
						 "value is a failure, not a zero: a table of expected values that all "
						 "quietly became zero would compare zeros and pass"),
					Pin.DefineName, *Found->ValueText, Found->LineNumber),
				TryParseUint(Found->ValueText, ShaderValue)))
		{
			continue;
		}

		TestEqual(*FString::Printf(
				TEXT("%s (%u, .usf line %d) equals %s (%u). These are the same number on two "
					 "sides of a boundary no compiler crosses: renumber either and the plugin "
					 "builds, the shader builds, and OutValue.z silently starts meaning "
					 "something else"),
				Pin.DefineName, ShaderValue, Found->LineNumber, Pin.CppName, Pin.CppValue),
			ShaderValue, Pin.CppValue);
	}

	/* == And nothing in those groups is unaccounted for ======================= */
	/*
	 * The other direction. Without it a shader that grew a new FLOWVIZ_REASON_*
	 * with no C++ enumerator behind it passes: the renderer could set a bit that
	 * DecodeReason turns into a number no caller can name.
	 */
	{
		TArray<FString> Unpinned;
		for (const TPair<FString, FShaderDefine>& Entry : Defines)
		{
			bool bCovered = false;
			for (const TCHAR* Prefix : CoveredPrefixes)
			{
				if (Entry.Key.StartsWith(Prefix))
				{
					bCovered = true;
					break;
				}
			}
			if (!bCovered)
			{
				continue;
			}

			bool bPinned = false;
			for (const FPinnedCode& Pin : Pinned)
			{
				if (Entry.Key == Pin.DefineName)
				{
					bPinned = true;
					break;
				}
			}
			if (!bPinned)
			{
				Unpinned.Add(FString::Printf(TEXT("%s (line %d)"), *Entry.Key, Entry.Value.LineNumber));
			}
		}

		TestTrue(*FString::Printf(
				TEXT("every FLOWVIZ_REASON_/STATUS_/MODE_/COMPONENT_ define in the shader is "
					 "pinned to a C++ constant. Unpinned: [%s]. A new define here with no "
					 "enumerator behind it is a bit the renderer can set and no caller can name"),
				*FString::Join(Unpinned, TEXT(", "))),
			Unpinned.Num() == 0);
	}

	/* == The C++ side cannot grow a bit the shader has never heard of ========= */
	/*
	 * The remaining direction, and the one a text parser cannot reach: a NINTH
	 * enumerator added to EFlowVizInvalidReason. C++ enums are not enumerable, so
	 * the header carries FlowVizRayMarch::KnownInvalidReasons - the OR of every
	 * enumerator - and this compares it against the OR of the eight this file
	 * pins. Adding an enumerator to the enum without adding it here (and, via the
	 * loop above, to the shader) fails.
	 */
	{
		uint32 PinnedReasonUnion = 0;
		for (const FPinnedCode& Pin : Pinned)
		{
			if (FString(Pin.DefineName).StartsWith(TEXT("FLOWVIZ_REASON_")))
			{
				PinnedReasonUnion |= Pin.CppValue;
			}
		}

		TestEqual(*FString::Printf(
				TEXT("the eight reason bits pinned above are exactly the ones "
					 "FlowVizRayMarch::KnownInvalidReasons declares (0x%02x vs 0x%02x). A new "
					 "enumerator that reached the header but not the .usf lands here"),
				PinnedReasonUnion,
				static_cast<uint32>(FlowVizRayMarch::KnownInvalidReasons)),
			PinnedReasonUnion, static_cast<uint32>(FlowVizRayMarch::KnownInvalidReasons));

		// The bits are FLAGS: eight distinct powers of two plus zero, so a ray can
		// report "masked AND over range" in one number. Two enumerators sharing a
		// value would still satisfy every equality above while making the two
		// causes indistinguishable in OutValue.z - which is the whole point of the
		// channel.
		uint32 OredBits = 0;
		int32 DistinctBits = 0;
		for (const FPinnedCode& Pin : Pinned)
		{
			if (!FString(Pin.DefineName).StartsWith(TEXT("FLOWVIZ_REASON_")) || Pin.CppValue == 0)
			{
				continue;
			}
			OredBits |= Pin.CppValue;
			++DistinctBits;
			TestEqual(*FString::Printf(
					TEXT("%s (0x%02x) is a single bit, so reasons compose"), Pin.CppName, Pin.CppValue),
				static_cast<int32>(FMath::CountBits(Pin.CppValue)), 1);
		}
		TestEqual(*FString::Printf(
				TEXT("the seven non-zero reason bits are mutually distinct (union 0x%02x over %d "
					 "enumerators). A collision would let two causes read as one"),
				OredBits, DistinctBits),
			static_cast<int32>(FMath::CountBits(OredBits)), DistinctBits);
	}

	/* == The decoder the renderer ships ======================================= */
	/*
	 * DecodeReason is what a caller uses to read OutValue.z, and OutValue.z is a
	 * FLOAT: the shader writes float(ReasonBits). The +0.5 truncation inside it is
	 * the round trip's only fragile step, so it is exercised here on every bit and
	 * on the combinations the renderer actually produces - not just on integers
	 * that happen to be exact.
	 */
	{
		for (const FPinnedCode& Pin : Pinned)
		{
			if (!FString(Pin.DefineName).StartsWith(TEXT("FLOWVIZ_REASON_")))
			{
				continue;
			}
			const EFlowVizInvalidReason Decoded =
				FlowVizRayMarch::DecodeReason(static_cast<float>(Pin.CppValue));
			TestEqual(*FString::Printf(
					TEXT("DecodeReason(%.1f) returns %s"), static_cast<double>(Pin.CppValue), Pin.CppName),
				static_cast<uint32>(Decoded), Pin.CppValue);
		}

		// A ray that crossed masked voxels and ALSO found valid data over range
		// reports three bits at once. The decoder must not lose any of them.
		const EFlowVizInvalidReason Combined = FlowVizRayMarch::DecodeReason(
			static_cast<float>(
				static_cast<uint32>(EFlowVizInvalidReason::Valid)
				| static_cast<uint32>(EFlowVizInvalidReason::Masked)
				| static_cast<uint32>(EFlowVizInvalidReason::OverRange)));
		TestTrue(TEXT("DecodeReason keeps every bit of a composed reason (valid + masked + over "
					  "range), which is what a real ray reports"),
			EnumHasAllFlags(Combined, EFlowVizInvalidReason::Valid | EFlowVizInvalidReason::Masked
				| EFlowVizInvalidReason::OverRange)
				&& !EnumHasAnyFlags(Combined, EFlowVizInvalidReason::NaN));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
