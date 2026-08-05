// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizManifest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * manifest.json parsing (format section 3).
 *
 * EVERY JSON TYPE IS CONFINED TO THIS FILE. Json and JsonUtilities are PRIVATE
 * dependencies of FlowVizRuntime, so an FJsonObject in the public header would
 * not compile for any module depending on this one.
 *
 * The parse is strict about what the format defines and permissive about what it
 * does not: a required property that is missing or of the wrong type is an error
 * naming that property, an unknown property is ignored at every level (format
 * rule 1.4), and an optional property gets the default documented at its
 * declaration in the header rather than one invented here.
 */

namespace
{
	/* ---------------------------------------------------------------------- */
	/* Error helpers                                                           */
	/* ---------------------------------------------------------------------- */

	/** A manifest-level failure. Path is filled in by the caller that knows it. */
	FCFDVizResult ManifestError(FString Message)
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidManifest, MoveTemp(Message));
	}

	/* ---------------------------------------------------------------------- */
	/* Typed property access                                                   */
	/*                                                                         */
	/* Each Require* names the JSON path it failed on, e.g.                     */
	/* "fields[2].componentCount", because "invalid manifest" alone is not      */
	/* something a user can act on.                                            */
	/* ---------------------------------------------------------------------- */

	bool TryGetField(const TSharedPtr<FJsonObject>& Object, const FString& Name,
		TSharedPtr<FJsonValue>& OutValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
		// JSON null is treated as absent throughout: the schema's optional
		// properties may be written as null, and both spellings mean "not
		// declared" rather than "declared as nothing".
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return false;
		}
		OutValue = Value;
		return true;
	}

	FCFDVizResult RequireObject(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		const FString& Path, TSharedPtr<FJsonObject>& OutObject)
	{
		TSharedPtr<FJsonValue> Value;
		if (!TryGetField(Parent, Name, Value))
		{
			return ManifestError(FString::Printf(TEXT("%s is required but missing"), *Path));
		}
		const TSharedPtr<FJsonObject>* AsObject = nullptr;
		if (!Value->TryGetObject(AsObject) || AsObject == nullptr || !AsObject->IsValid())
		{
			return ManifestError(FString::Printf(TEXT("%s must be an object"), *Path));
		}
		OutObject = *AsObject;
		return FCFDVizResult::Ok();
	}

	FCFDVizResult RequireString(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		const FString& Path, FString& OutString)
	{
		TSharedPtr<FJsonValue> Value;
		if (!TryGetField(Parent, Name, Value))
		{
			return ManifestError(FString::Printf(TEXT("%s is required but missing"), *Path));
		}
		if (Value->Type != EJson::String || !Value->TryGetString(OutString))
		{
			return ManifestError(FString::Printf(TEXT("%s must be a string"), *Path));
		}
		return FCFDVizResult::Ok();
	}

	/** Optional string. Leaves OutString untouched when absent. */
	void ReadOptionalString(const TSharedPtr<FJsonObject>& Parent, const FString& Name, FString& OutString)
	{
		TSharedPtr<FJsonValue> Value;
		if (TryGetField(Parent, Name, Value) && Value->Type == EJson::String)
		{
			Value->TryGetString(OutString);
		}
	}

	void ReadOptionalBool(const TSharedPtr<FJsonObject>& Parent, const FString& Name, bool& OutBool)
	{
		TSharedPtr<FJsonValue> Value;
		if (TryGetField(Parent, Name, Value) && Value->Type == EJson::Boolean)
		{
			OutBool = Value->AsBool();
		}
	}

	/**
	 * A JSON number that must be an exact integer.
	 *
	 * JSON has one numeric type, so 3.5 arrives where an integer is required
	 * looking perfectly well-formed. Truncating it would silently accept a
	 * manifest the schema rejects, so a non-integral value is refused. Booleans
	 * are refused too - JSON true is not 1 here, and Unreal's TryGetNumber would
	 * not coerce it anyway, but the explicit type check documents the intent.
	 */
	FCFDVizResult RequireInteger(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		const FString& Path, int64& OutValue)
	{
		TSharedPtr<FJsonValue> Value;
		if (!TryGetField(Parent, Name, Value))
		{
			return ManifestError(FString::Printf(TEXT("%s is required but missing"), *Path));
		}
		if (Value->Type != EJson::Number)
		{
			return ManifestError(FString::Printf(TEXT("%s must be a number"), *Path));
		}

		const double Number = Value->AsNumber();
		if (!FMath::IsFinite(Number) || Number != FMath::TruncToDouble(Number))
		{
			return ManifestError(FString::Printf(TEXT("%s must be a whole number, not %f"), *Path, Number));
		}
		// Beyond 2^53 a double no longer represents consecutive integers, so a
		// value past it cannot be trusted to be the number that was written.
		if (FMath::Abs(Number) > 9007199254740992.0)
		{
			return ManifestError(FString::Printf(TEXT("%s is too large to represent exactly"), *Path));
		}

		OutValue = static_cast<int64>(Number);
		return FCFDVizResult::Ok();
	}

	FCFDVizResult RequireNumber(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		const FString& Path, double& OutValue)
	{
		TSharedPtr<FJsonValue> Value;
		if (!TryGetField(Parent, Name, Value))
		{
			return ManifestError(FString::Printf(TEXT("%s is required but missing"), *Path));
		}
		if (Value->Type != EJson::Number)
		{
			return ManifestError(FString::Printf(TEXT("%s must be a number"), *Path));
		}
		OutValue = Value->AsNumber();
		return FCFDVizResult::Ok();
	}

	FCFDVizResult RequireArray(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		const FString& Path, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
	{
		TSharedPtr<FJsonValue> Value;
		if (!TryGetField(Parent, Name, Value))
		{
			return ManifestError(FString::Printf(TEXT("%s is required but missing"), *Path));
		}
		if (!Value->TryGetArray(OutArray) || OutArray == nullptr)
		{
			return ManifestError(FString::Printf(TEXT("%s must be an array"), *Path));
		}
		return FCFDVizResult::Ok();
	}

	/** Optional array; leaves OutArray null when absent. */
	bool TryGetArray(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		const TArray<TSharedPtr<FJsonValue>>*& OutArray)
	{
		TSharedPtr<FJsonValue> Value;
		if (!TryGetField(Parent, Name, Value))
		{
			return false;
		}
		return Value->TryGetArray(OutArray) && OutArray != nullptr;
	}

	/** Exactly `Count` finite numbers - the shape of dimensions, origin and spacing. */
	FCFDVizResult RequireNumberArray(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		const FString& Path, int32 Count, TArray<double>& OutValues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		FCFDVizResult Result = RequireArray(Parent, Name, Path, Array);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (Array->Num() != Count)
		{
			return ManifestError(FString::Printf(TEXT("%s must have %d entries, not %d"),
				*Path, Count, Array->Num()));
		}

		OutValues.Reset(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const TSharedPtr<FJsonValue>& Entry = (*Array)[Index];
			if (!Entry.IsValid() || Entry->Type != EJson::Number)
			{
				return ManifestError(FString::Printf(TEXT("%s[%d] must be a number"), *Path, Index));
			}
			OutValues.Add(Entry->AsNumber());
		}
		return FCFDVizResult::Ok();
	}

	FCFDVizResult RequireStringArray(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		const FString& Path, TArray<FString>& OutValues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		FCFDVizResult Result = RequireArray(Parent, Name, Path, Array);
		if (!Result.IsOk())
		{
			return Result;
		}

		OutValues.Reset(Array->Num());
		for (int32 Index = 0; Index < Array->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Entry = (*Array)[Index];
			FString Text;
			if (!Entry.IsValid() || Entry->Type != EJson::String || !Entry->TryGetString(Text))
			{
				return ManifestError(FString::Printf(TEXT("%s[%d] must be a string"), *Path, Index));
			}
			OutValues.Add(MoveTemp(Text));
		}
		return FCFDVizResult::Ok();
	}

	/** Optional string array; absent leaves the output empty. */
	void ReadOptionalStringArray(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		TArray<FString>& OutValues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!TryGetArray(Parent, Name, Array))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			FString Text;
			if (Entry.IsValid() && Entry->Type == EJson::String && Entry->TryGetString(Text))
			{
				OutValues.Add(MoveTemp(Text));
			}
		}
	}

	/* ---------------------------------------------------------------------- */
	/* Closed enums                                                            */
	/*                                                                         */
	/* Case-sensitive, matching the schema and the Python reference. Accepting  */
	/* "Cell" here would let a manifest through that the reference rejects.     */
	/* ---------------------------------------------------------------------- */

	bool TryParseInterpolation(const FString& Name, ECFDVizInterpolation& Out)
	{
		if (Name.Equals(TEXT("nearest"), ESearchCase::CaseSensitive)) { Out = ECFDVizInterpolation::Nearest; return true; }
		if (Name.Equals(TEXT("linear"), ESearchCase::CaseSensitive)) { Out = ECFDVizInterpolation::Linear; return true; }
		return false;
	}

	bool TryParseRangeMode(const FString& Name, ECFDVizRangeMode& Out)
	{
		if (Name.Equals(TEXT("global"), ESearchCase::CaseSensitive)) { Out = ECFDVizRangeMode::Global; return true; }
		if (Name.Equals(TEXT("frame"), ESearchCase::CaseSensitive)) { Out = ECFDVizRangeMode::Frame; return true; }
		if (Name.Equals(TEXT("manual"), ESearchCase::CaseSensitive)) { Out = ECFDVizRangeMode::Manual; return true; }
		return false;
	}

	/** The reserved associations of section 3.2, named so the error can say "reserved". */
	bool IsReservedAssociation(const FString& Name)
	{
		return Name.Equals(TEXT("mesh-vertex"), ESearchCase::CaseSensitive)
			|| Name.Equals(TEXT("mesh-element"), ESearchCase::CaseSensitive)
			|| Name.Equals(TEXT("integration-point"), ESearchCase::CaseSensitive)
			|| Name.Equals(TEXT("face"), ESearchCase::CaseSensitive)
			|| Name.Equals(TEXT("particle"), ESearchCase::CaseSensitive);
	}

	/* ---------------------------------------------------------------------- */
	/* Version                                                                 */
	/* ---------------------------------------------------------------------- */

	/**
	 * MAJOR.MINOR.PATCH with an optional prerelease/build suffix, mirroring the
	 * Python reference's _VERSION_RE.
	 */
	bool TryParseSemanticVersion(const FString& Text, int32& OutMajor, int32& OutMinor, int32& OutPatch)
	{
		// Cut any "-prerelease" or "+build" suffix before splitting on '.'.
		FString Core = Text;
		int32 SuffixIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Core.Len(); ++Index)
		{
			if (Core[Index] == TEXT('-') || Core[Index] == TEXT('+'))
			{
				SuffixIndex = Index;
				break;
			}
		}
		if (SuffixIndex != INDEX_NONE)
		{
			Core = Core.Left(SuffixIndex);
		}

		TArray<FString> Parts;
		Core.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty=*/false);
		if (Parts.Num() != 3)
		{
			return false;
		}

		int32 Parsed[3] = { 0, 0, 0 };
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FString& Part = Parts[Index];
			if (Part.IsEmpty())
			{
				return false;
			}
			// IsNumeric would accept "1.5" and "-1"; each component must be
			// digits only.
			for (int32 Char = 0; Char < Part.Len(); ++Char)
			{
				if (!FChar::IsDigit(Part[Char]))
				{
					return false;
				}
			}
			Parsed[Index] = FCString::Atoi(*Part);
		}

		OutMajor = Parsed[0];
		OutMinor = Parsed[1];
		OutPatch = Parsed[2];
		return true;
	}

	/* ---------------------------------------------------------------------- */
	/* Sub-object parsers                                                      */
	/* ---------------------------------------------------------------------- */

	FCFDVizResult ParseDisplay(const TSharedPtr<FJsonObject>& Parent, const FString& Path,
		FCFDVizFieldDisplay& Out)
	{
		TSharedPtr<FJsonValue> Value;
		if (!TryGetField(Parent, TEXT("display"), Value))
		{
			return FCFDVizResult::Ok();
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
		{
			return ManifestError(FString::Printf(TEXT("%s.display must be an object"), *Path));
		}

		ReadOptionalString(*Object, TEXT("defaultComponent"), Out.DefaultComponent);
		ReadOptionalString(*Object, TEXT("defaultColorMap"), Out.DefaultColorMap);

		FString RangeMode;
		ReadOptionalString(*Object, TEXT("defaultRangeMode"), RangeMode);
		if (!RangeMode.IsEmpty() && !TryParseRangeMode(RangeMode, Out.DefaultRangeMode))
		{
			return ManifestError(FString::Printf(
				TEXT("%s.display.defaultRangeMode is \"%s\"; expected global, frame or manual"),
				*Path, *RangeMode));
		}

		const TArray<TSharedPtr<FJsonValue>>* Range = nullptr;
		if (TryGetArray(*Object, TEXT("recommendedRange"), Range))
		{
			if (Range->Num() != 2)
			{
				return ManifestError(FString::Printf(
					TEXT("%s.display.recommendedRange must be [min, max]"), *Path));
			}
			if ((*Range)[0]->Type != EJson::Number || (*Range)[1]->Type != EJson::Number)
			{
				return ManifestError(FString::Printf(
					TEXT("%s.display.recommendedRange entries must be numbers"), *Path));
			}
			Out.RecommendedRange = FVector2D((*Range)[0]->AsNumber(), (*Range)[1]->AsNumber());
		}

		const TArray<TSharedPtr<FJsonValue>>* Opacity = nullptr;
		if (TryGetArray(*Object, TEXT("opacityPoints"), Opacity))
		{
			for (int32 Index = 0; Index < Opacity->Num(); ++Index)
			{
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				if (!(*Opacity)[Index].IsValid() || !(*Opacity)[Index]->TryGetArray(Pair)
					|| Pair == nullptr || Pair->Num() != 2
					|| (*Pair)[0]->Type != EJson::Number || (*Pair)[1]->Type != EJson::Number)
				{
					return ManifestError(FString::Printf(
						TEXT("%s.display.opacityPoints[%d] must be [value, opacity]"), *Path, Index));
				}
				Out.OpacityPoints.Add(FVector2D((*Pair)[0]->AsNumber(), (*Pair)[1]->AsNumber()));
			}
		}

		return FCFDVizResult::Ok();
	}

	/**
	 * `field.statistics`.
	 *
	 * A half-declared statistic is KEPT as written and flagged unusable, rather
	 * than dropped or rejected: discarding a declared number would be the silent
	 * normalisation rule 1.6 forbids, and rejecting it would refuse a manifest
	 * the schema and the Python reference both accept.
	 */
	FCFDVizResult ParseStatistics(const TSharedPtr<FJsonObject>& Parent, const FString& Path,
		FCFDVizFieldStatistics& Out)
	{
		TSharedPtr<FJsonValue> Value;
		if (!TryGetField(Parent, TEXT("statistics"), Value))
		{
			return FCFDVizResult::Ok();
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
		{
			return ManifestError(FString::Printf(TEXT("%s.statistics must be an object"), *Path));
		}

		auto ReadDoubleArray = [&](const TCHAR* Name, TArray<double>& Target) -> FCFDVizResult
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!TryGetArray(*Object, Name, Array))
			{
				return FCFDVizResult::Ok();
			}
			for (int32 Index = 0; Index < Array->Num(); ++Index)
			{
				if (!(*Array)[Index].IsValid() || (*Array)[Index]->Type != EJson::Number)
				{
					return ManifestError(FString::Printf(
						TEXT("%s.statistics.%s[%d] must be a number"), *Path, Name, Index));
				}
				Target.Add((*Array)[Index]->AsNumber());
			}
			return FCFDVizResult::Ok();
		};

		FCFDVizResult Result = ReadDoubleArray(TEXT("globalComponentMin"), Out.GlobalComponentMin);
		if (!Result.IsOk())
		{
			return Result;
		}
		Result = ReadDoubleArray(TEXT("globalComponentMax"), Out.GlobalComponentMax);
		if (!Result.IsOk())
		{
			return Result;
		}

		// Only a matched pair of equal length constitutes a range.
		Out.bHasComponentRange = Out.GlobalComponentMin.Num() > 0
			&& Out.GlobalComponentMin.Num() == Out.GlobalComponentMax.Num();

		TSharedPtr<FJsonValue> MagMin;
		TSharedPtr<FJsonValue> MagMax;
		const bool bHasMin = TryGetField(*Object, TEXT("globalMagnitudeMin"), MagMin)
			&& MagMin->Type == EJson::Number;
		const bool bHasMax = TryGetField(*Object, TEXT("globalMagnitudeMax"), MagMax)
			&& MagMax->Type == EJson::Number;
		if (bHasMin)
		{
			Out.GlobalMagnitudeMin = MagMin->AsNumber();
		}
		if (bHasMax)
		{
			Out.GlobalMagnitudeMax = MagMax->AsNumber();
		}
		Out.bHasMagnitudeRange = bHasMin && bHasMax;

		return FCFDVizResult::Ok();
	}

	/** `sourceToCanonical` / `mesh.transform`: 16 row-major numbers, transposed on the way in. */
	FCFDVizResult ParseMatrix(const TSharedPtr<FJsonObject>& Parent, const FString& Name,
		const FString& Path, TOptional<FMatrix>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!TryGetArray(Parent, Name, Array))
		{
			return FCFDVizResult::Ok();
		}
		if (Array->Num() != 16)
		{
			return ManifestError(FString::Printf(
				TEXT("%s.%s must have 16 entries, not %d"), *Path, *Name, Array->Num()));
		}

		TArray<double> Values;
		Values.Reserve(16);
		for (int32 Index = 0; Index < 16; ++Index)
		{
			if (!(*Array)[Index].IsValid() || (*Array)[Index]->Type != EJson::Number)
			{
				return ManifestError(FString::Printf(TEXT("%s.%s[%d] must be a number"), *Path, *Name, Index));
			}
			Values.Add((*Array)[Index]->AsNumber());
		}

		// TryMakeMatrixFromRowMajorArray does the transpose into Unreal's
		// row-vector convention. Copying the 16 values straight across would
		// look right for pure rotations and silently misplace every translation.
		FMatrix Matrix;
		if (!TryMakeMatrixFromRowMajorArray(Values, Matrix))
		{
			return ManifestError(FString::Printf(
				TEXT("%s.%s must be 16 finite numbers"), *Path, *Name));
		}
		Out = Matrix;
		return FCFDVizResult::Ok();
	}
}

/* -------------------------------------------------------------------------- */
/* Leaf helpers                                                                 */
/* -------------------------------------------------------------------------- */

bool FCFDVizCaseMetadata::TryGetCreatedUtc(FDateTime& OutDateTime) const
{
	if (CreatedUtcText.IsEmpty())
	{
		return false;
	}
	// The raw text is kept regardless, so a spelling FDateTime does not accept
	// stays a cosmetic gap rather than a load failure.
	return FDateTime::ParseIso8601(*CreatedUtcText, OutDateTime);
}

bool FCFDVizUnits::TryGetLengthInMeters(double& OutMetersPerUnit) const
{
	struct FEntry
	{
		const TCHAR* Name;
		double MetersPerUnit;
	};
	// Deliberately a closed table. An unrecognised unit returns false so the
	// caller asks or refuses; assuming metres would place a millimetre-scale
	// case a thousand times too large with nothing on screen to say so.
	static const FEntry Table[] = {
		{ TEXT("m"), 1.0 },
		{ TEXT("meter"), 1.0 },
		{ TEXT("metre"), 1.0 },
		{ TEXT("meters"), 1.0 },
		{ TEXT("metres"), 1.0 },
		{ TEXT("cm"), 0.01 },
		{ TEXT("centimeter"), 0.01 },
		{ TEXT("centimetre"), 0.01 },
		{ TEXT("mm"), 0.001 },
		{ TEXT("millimeter"), 0.001 },
		{ TEXT("millimetre"), 0.001 },
		{ TEXT("km"), 1000.0 },
		{ TEXT("kilometer"), 1000.0 },
		{ TEXT("kilometre"), 1000.0 },
		{ TEXT("in"), 0.0254 },
		{ TEXT("inch"), 0.0254 },
		{ TEXT("ft"), 0.3048 },
		{ TEXT("foot"), 0.3048 },
		{ TEXT("feet"), 0.3048 },
	};

	for (const FEntry& Entry : Table)
	{
		if (Length.Equals(Entry.Name, ESearchCase::CaseSensitive))
		{
			OutMetersPerUnit = Entry.MetersPerUnit;
			return true;
		}
	}
	return false;
}

bool FCFDVizTimeline::IsStrictlyIncreasing() const
{
	// NaN is rejected explicitly rather than left to the `>` comparison: every
	// comparison against NaN is false, so a single-element [NaN] timeline would
	// pass a pairwise-only check.
	for (const double Time : Times)
	{
		if (FMath::IsNaN(Time) || !FMath::IsFinite(Time))
		{
			return false;
		}
	}
	for (int32 Index = 1; Index < Times.Num(); ++Index)
	{
		if (!(Times[Index] > Times[Index - 1]))
		{
			return false;
		}
	}
	return true;
}

bool FCFDVizFieldStatistics::TryMakeStatistics(int32 ComponentCount, FCFDVizStatistics& OutStatistics) const
{
	if (!bHasComponentRange || ComponentCount <= 0 || GlobalComponentMin.Num() != ComponentCount)
	{
		return false;
	}

	FCFDVizStatistics Result;
	Result.ComponentMin = GlobalComponentMin;
	Result.ComponentMax = GlobalComponentMax;
	if (bHasMagnitudeRange)
	{
		Result.MagnitudeMin = GlobalMagnitudeMin;
		Result.MagnitudeMax = GlobalMagnitudeMax;
	}
	// NaNCount and ValidCount stay 0: the manifest does not record them, and
	// inventing a count would let a caller derive a mean or a NaN fraction that
	// the data never supported.
	//
	// bValid must be set EXPLICITLY - FCFDVizStatistics documents that a struct
	// filled from manifest JSON has to do this, because only AccumulateSample
	// maintains the flag automatically. Left false, every caller that early-outs
	// on it would discard a range the manifest did declare.
	Result.bValid = true;

	OutStatistics = MoveTemp(Result);
	return true;
}

int32 FCFDVizField::FindComponentIndex(const FString& ComponentName) const
{
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		// Case-sensitive: display.defaultComponent is compared against these
		// same strings on the Python side.
		if (Components[Index].Equals(ComponentName, ESearchCase::CaseSensitive))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

const FCFDVizBoundaryPatch* FCFDVizMesh::FindPatch(uint32 PatchId) const
{
	for (const FCFDVizBoundaryPatch& Patch : Patches)
	{
		if (Patch.Id == PatchId)
		{
			return &Patch;
		}
	}
	return nullptr;
}

/* -------------------------------------------------------------------------- */
/* Parsing                                                                      */
/* -------------------------------------------------------------------------- */

namespace
{
	FCFDVizResult ParseCaseMetadata(const TSharedPtr<FJsonObject>& Root, FCFDVizCaseMetadata& Out)
	{
		TSharedPtr<FJsonObject> Object;
		FCFDVizResult Result = RequireObject(Root, TEXT("case"), TEXT("case"), Object);
		if (!Result.IsOk())
		{
			return Result;
		}

		Result = RequireString(Object, TEXT("id"), TEXT("case.id"), Out.Id);
		if (!Result.IsOk())
		{
			return Result;
		}
		Result = RequireString(Object, TEXT("name"), TEXT("case.name"), Out.Name);
		if (!Result.IsOk())
		{
			return Result;
		}

		ReadOptionalString(Object, TEXT("description"), Out.Description);
		ReadOptionalString(Object, TEXT("createdUtc"), Out.CreatedUtcText);
		ReadOptionalString(Object, TEXT("quality"), Out.Quality);
		ReadOptionalStringArray(Object, TEXT("tags"), Out.Tags);

		TSharedPtr<FJsonValue> SolverValue;
		if (TryGetField(Object, TEXT("solver"), SolverValue))
		{
			const TSharedPtr<FJsonObject>* Solver = nullptr;
			if (SolverValue->TryGetObject(Solver) && Solver != nullptr && Solver->IsValid())
			{
				ReadOptionalString(*Solver, TEXT("name"), Out.Solver.Name);
				ReadOptionalString(*Solver, TEXT("version"), Out.Solver.Version);
				ReadOptionalString(*Solver, TEXT("method"), Out.Solver.Method);
			}
		}

		return FCFDVizResult::Ok();
	}

	FCFDVizResult ParseUnits(const TSharedPtr<FJsonObject>& Root, FCFDVizUnits& Out)
	{
		TSharedPtr<FJsonObject> Object;
		FCFDVizResult Result = RequireObject(Root, TEXT("units"), TEXT("units"), Object);
		if (!Result.IsOk())
		{
			return Result;
		}

		Result = RequireString(Object, TEXT("length"), TEXT("units.length"), Out.Length);
		if (!Result.IsOk())
		{
			return Result;
		}
		Result = RequireString(Object, TEXT("time"), TEXT("units.time"), Out.Time);
		if (!Result.IsOk())
		{
			return Result;
		}

		ReadOptionalString(Object, TEXT("mass"), Out.Mass);
		ReadOptionalString(Object, TEXT("temperature"), Out.Temperature);
		ReadOptionalString(Object, TEXT("angle"), Out.Angle);
		return FCFDVizResult::Ok();
	}

	FCFDVizResult ParseCoordinates(const TSharedPtr<FJsonObject>& Root, FCFDVizCoordinates& Out)
	{
		TSharedPtr<FJsonObject> Object;
		FCFDVizResult Result = RequireObject(Root, TEXT("coordinates"), TEXT("coordinates"), Object);
		if (!Result.IsOk())
		{
			return Result;
		}

		// Closed enums admitting exactly one value each. A manifest declaring
		// anything else is rejected rather than silently reinterpreted - the
		// data is stored in the canonical frame and cannot be re-derived.
		FString Handedness;
		Result = RequireString(Object, TEXT("handedness"), TEXT("coordinates.handedness"), Handedness);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (!Handedness.Equals(TEXT("right"), ESearchCase::CaseSensitive))
		{
			return ManifestError(FString::Printf(
				TEXT("coordinates.handedness is \"%s\"; CFDViz 1.0 stores data right-handed only"),
				*Handedness));
		}

		FString UpAxis;
		Result = RequireString(Object, TEXT("upAxis"), TEXT("coordinates.upAxis"), UpAxis);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (!UpAxis.Equals(TEXT("Z"), ESearchCase::CaseSensitive))
		{
			return ManifestError(FString::Printf(
				TEXT("coordinates.upAxis is \"%s\"; CFDViz 1.0 is Z-up only"), *UpAxis));
		}

		FString ForwardAxis;
		Result = RequireString(Object, TEXT("forwardAxis"), TEXT("coordinates.forwardAxis"), ForwardAxis);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (!ForwardAxis.Equals(TEXT("X"), ESearchCase::CaseSensitive))
		{
			return ManifestError(FString::Printf(
				TEXT("coordinates.forwardAxis is \"%s\"; CFDViz 1.0 is X-forward only"), *ForwardAxis));
		}

		Out.System = FCFDVizCoordinateSystem::Canonical();

		const TArray<TSharedPtr<FJsonValue>>* Origin = nullptr;
		if (TryGetArray(Object, TEXT("origin"), Origin))
		{
			if (Origin->Num() != 3)
			{
				return ManifestError(TEXT("coordinates.origin must have 3 entries"));
			}
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if ((*Origin)[Index]->Type != EJson::Number)
				{
					return ManifestError(FString::Printf(TEXT("coordinates.origin[%d] must be a number"), Index));
				}
			}
			// Provenance only. Adding this to a grid position would
			// double-offset the whole volume - a plausible-looking translation
			// that is simply wrong.
			Out.Origin = FVector((*Origin)[0]->AsNumber(), (*Origin)[1]->AsNumber(), (*Origin)[2]->AsNumber());
		}

		Result = ParseMatrix(Object, TEXT("sourceToCanonical"), TEXT("coordinates"), Out.SourceToCanonical);
		if (!Result.IsOk())
		{
			return Result;
		}

		// `crs` is only read when given as a string; the schema also allows an
		// object, which carries no meaning in 1.0.
		TSharedPtr<FJsonValue> Crs;
		if (TryGetField(Object, TEXT("crs"), Crs) && Crs->Type == EJson::String)
		{
			Crs->TryGetString(Out.Crs);
		}

		return FCFDVizResult::Ok();
	}

	FCFDVizResult ParseTimeline(const TSharedPtr<FJsonObject>& Root, FCFDVizTimeline& Out)
	{
		TSharedPtr<FJsonObject> Object;
		FCFDVizResult Result = RequireObject(Root, TEXT("timeline"), TEXT("timeline"), Object);
		if (!Result.IsOk())
		{
			return Result;
		}

		int64 FrameCount = 0;
		Result = RequireInteger(Object, TEXT("frameCount"), TEXT("timeline.frameCount"), FrameCount);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (FrameCount < 0 || FrameCount > MAX_int32)
		{
			return ManifestError(FString::Printf(
				TEXT("timeline.frameCount is %lld, which is not a usable frame count"), FrameCount));
		}
		Out.FrameCount = static_cast<int32>(FrameCount);

		const TArray<TSharedPtr<FJsonValue>>* Times = nullptr;
		Result = RequireArray(Object, TEXT("times"), TEXT("timeline.times"), Times);
		if (!Result.IsOk())
		{
			return Result;
		}
		Out.Times.Reset(Times->Num());
		for (int32 Index = 0; Index < Times->Num(); ++Index)
		{
			if (!(*Times)[Index].IsValid() || (*Times)[Index]->Type != EJson::Number)
			{
				return ManifestError(FString::Printf(TEXT("timeline.times[%d] must be a number"), Index));
			}
			Out.Times.Add((*Times)[Index]->AsNumber());
		}

		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		if (TryGetArray(Object, TEXT("steps"), Steps))
		{
			Out.Steps.Reset(Steps->Num());
			for (int32 Index = 0; Index < Steps->Num(); ++Index)
			{
				if (!(*Steps)[Index].IsValid() || (*Steps)[Index]->Type != EJson::Number)
				{
					return ManifestError(FString::Printf(TEXT("timeline.steps[%d] must be a number"), Index));
				}
				// The schema types a step as an integer. Casting a fractional,
				// NaN or out-of-range double to int64 is undefined behaviour,
				// not a truncation, so the value is checked BEFORE the
				// conversion - and 2^53 is where a double stops representing
				// consecutive integers at all.
				const double Step = (*Steps)[Index]->AsNumber();
				if (!FMath::IsFinite(Step) || Step != FMath::TruncToDouble(Step)
					|| FMath::Abs(Step) > 9007199254740992.0)
				{
					return ManifestError(FString::Printf(
						TEXT("timeline.steps[%d] is %f; a solver step must be a whole number"), Index, Step));
				}
				Out.Steps.Add(static_cast<int64>(Step));
			}
		}

		FString Interpolation;
		ReadOptionalString(Object, TEXT("defaultInterpolation"), Interpolation);
		if (!Interpolation.IsEmpty() && !TryParseInterpolation(Interpolation, Out.DefaultInterpolation))
		{
			return ManifestError(FString::Printf(
				TEXT("timeline.defaultInterpolation is \"%s\"; expected nearest or linear"), *Interpolation));
		}

		return FCFDVizResult::Ok();
	}

	FCFDVizResult ParseGrid(const TSharedPtr<FJsonObject>& Object, int32 GridIndex, FCFDVizGridDescriptor& Out)
	{
		const FString Path = FString::Printf(TEXT("grids[%d]"), GridIndex);

		FString Id;
		FCFDVizResult Result = RequireString(Object, TEXT("id"), Path + TEXT(".id"), Id);
		if (!Result.IsOk())
		{
			return Result;
		}
		Out.Id = FName(*Id);

		ReadOptionalString(Object, TEXT("name"), Out.Name);

		FString Type;
		Result = RequireString(Object, TEXT("type"), Path + TEXT(".type"), Type);
		if (!Result.IsOk())
		{
			return Result;
		}
		// Rejected, not ignored: 1.0 can only position cells for a uniform
		// Cartesian grid, so any other type would be rendered with geometry the
		// manifest never described.
		if (!Type.Equals(TEXT("uniform-cartesian"), ESearchCase::CaseSensitive))
		{
			return ManifestError(FString::Printf(
				TEXT("%s.type is \"%s\"; CFDViz 1.0 defines only \"uniform-cartesian\""), *Path, *Type));
		}
		Out.Type = ECFDVizGridType::UniformCartesian;

		TArray<double> Dimensions;
		Result = RequireNumberArray(Object, TEXT("dimensions"), Path + TEXT(".dimensions"), 3, Dimensions);
		if (!Result.IsOk())
		{
			return Result;
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Value = Dimensions[Axis];
			if (Value != FMath::TruncToDouble(Value) || Value < 1.0)
			{
				return ManifestError(FString::Printf(
					TEXT("%s.dimensions[%d] is %g; every dimension must be a whole number of at least 1"),
					*Path, Axis, Value));
			}
			// Stricter than the schema's uint32 on purpose: FIntVector is int32,
			// and a dimension past int32 would wrap negative and defeat every
			// downstream bounds check.
			if (Value > static_cast<double>(MAX_int32))
			{
				return ManifestError(FString::Printf(
					TEXT("%s.dimensions[%d] is %g, beyond the %d this reader can address"),
					*Path, Axis, Value, MAX_int32));
			}
		}
		Out.Geometry.Dimensions = FIntVector(
			static_cast<int32>(Dimensions[0]),
			static_cast<int32>(Dimensions[1]),
			static_cast<int32>(Dimensions[2]));

		TArray<double> Origin;
		Result = RequireNumberArray(Object, TEXT("origin"), Path + TEXT(".origin"), 3, Origin);
		if (!Result.IsOk())
		{
			return Result;
		}
		Out.Geometry.Origin = FVector(Origin[0], Origin[1], Origin[2]);

		TArray<double> Spacing;
		Result = RequireNumberArray(Object, TEXT("spacing"), Path + TEXT(".spacing"), 3, Spacing);
		if (!Result.IsOk())
		{
			return Result;
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!(Spacing[Axis] > 0.0) || !FMath::IsFinite(Spacing[Axis]))
			{
				return ManifestError(FString::Printf(
					TEXT("%s.spacing[%d] is %g; every spacing component must be greater than 0"),
					*Path, Axis, Spacing[Axis]));
			}
		}
		Out.Geometry.Spacing = FVector(Spacing[0], Spacing[1], Spacing[2]);

		FString MaskField;
		ReadOptionalString(Object, TEXT("maskField"), MaskField);
		if (!MaskField.IsEmpty())
		{
			Out.MaskFieldId = FName(*MaskField);
		}

		return FCFDVizResult::Ok();
	}

	FCFDVizResult ParseFieldStorage(const TSharedPtr<FJsonObject>& Parent, const FString& Path,
		FCFDVizFieldStorage& Out)
	{
		TSharedPtr<FJsonObject> Object;
		FCFDVizResult Result = RequireObject(Parent, TEXT("storage"), Path + TEXT(".storage"), Object);
		if (!Result.IsOk())
		{
			return Result;
		}

		FString Type;
		Result = RequireString(Object, TEXT("type"), Path + TEXT(".storage.type"), Type);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (!Type.Equals(TEXT("bricked-volume"), ESearchCase::CaseSensitive))
		{
			return ManifestError(FString::Printf(
				TEXT("%s.storage.type is \"%s\"; CFDViz 1.0 stores volume fields as \"bricked-volume\""),
				*Path, *Type));
		}
		Out.Type = ECFDVizStorageType::BrickedVolume;

		FString Codec;
		Result = RequireString(Object, TEXT("codec"), Path + TEXT(".storage.codec"), Codec);
		if (!Result.IsOk())
		{
			return Result;
		}
		// zstd parses successfully here on purpose: section 7 requires a 1.0
		// reader to reject it with one exact message, which it cannot do if
		// parsing has already failed with "unknown codec". CheckCodecSupport is
		// what refuses it.
		if (!TryParseCodec(Codec, Out.Codec))
		{
			return ManifestError(FString::Printf(
				TEXT("%s.storage.codec is \"%s\"; expected none, lz4, zlib or zstd"), *Path, *Codec));
		}

		FString Pattern;
		Result = RequireString(Object, TEXT("pathPattern"), Path + TEXT(".storage.pathPattern"), Pattern);
		if (!Result.IsOk())
		{
			return Result;
		}
		// Checked as written here; Validate also checks each expansion, because
		// the pattern and its expansions are different strings.
		if (!CFDViz::IsSafeRelativePath(Pattern))
		{
			return FCFDVizResult::Fail(ECFDVizError::PathTraversal, FString::Printf(
				TEXT("%s.storage.pathPattern \"%s\" is not a safe case-relative path (section 1.3)"),
				*Path, *Pattern));
		}
		Out.PathPattern = MoveTemp(Pattern);

		const TArray<TSharedPtr<FJsonValue>>* BrickSize = nullptr;
		if (TryGetArray(Object, TEXT("brickSize"), BrickSize))
		{
			if (BrickSize->Num() != 3)
			{
				return ManifestError(FString::Printf(
					TEXT("%s.storage.brickSize must have 3 entries"), *Path));
			}
			int32 Dims[3] = { 0, 0, 0 };
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if ((*BrickSize)[Axis]->Type != EJson::Number)
				{
					return ManifestError(FString::Printf(
						TEXT("%s.storage.brickSize[%d] must be a number"), *Path, Axis));
				}
				const double Value = (*BrickSize)[Axis]->AsNumber();
				if (Value < 1.0 || Value != FMath::TruncToDouble(Value) || Value > 65535.0)
				{
					return ManifestError(FString::Printf(
						TEXT("%s.storage.brickSize[%d] is %g; brick edges are whole numbers in 1..65535"),
						*Path, Axis, Value));
				}
				Dims[Axis] = static_cast<int32>(Value);
			}
			// Advisory only. The CVF header is authoritative (section 4.1); a
			// reader must never decode a brick using this value in preference
			// to the one in the file it opened.
			Out.BrickSize = FIntVector(Dims[0], Dims[1], Dims[2]);
		}

		int64 Level = 0;
		TSharedPtr<FJsonValue> LevelValue;
		if (TryGetField(Object, TEXT("level"), LevelValue) && LevelValue->Type == EJson::Number)
		{
			Result = RequireInteger(Object, TEXT("level"), Path + TEXT(".storage.level"), Level);
			if (!Result.IsOk())
			{
				return Result;
			}
			Out.Level = static_cast<int32>(Level);
		}

		return FCFDVizResult::Ok();
	}

	FCFDVizResult ParseField(const TSharedPtr<FJsonObject>& Object, int32 FieldIndex, FCFDVizField& Out)
	{
		const FString Path = FString::Printf(TEXT("fields[%d]"), FieldIndex);

		int64 NumericId = 0;
		FCFDVizResult Result = RequireInteger(Object, TEXT("numericId"), Path + TEXT(".numericId"), NumericId);
		if (!Result.IsOk())
		{
			return Result;
		}
		// uint32, matching the CVF header field. An int32 here would reject - or
		// worse, wrap - a numericId the Python writer accepts.
		if (NumericId < 0 || NumericId > static_cast<int64>(MAX_uint32))
		{
			return ManifestError(FString::Printf(
				TEXT("%s.numericId is %lld; it must fit in a uint32"), *Path, NumericId));
		}
		Out.NumericId = static_cast<uint32>(NumericId);

		FString Id;
		Result = RequireString(Object, TEXT("id"), Path + TEXT(".id"), Id);
		if (!Result.IsOk())
		{
			return Result;
		}
		Out.Id = FName(*Id);

		ReadOptionalString(Object, TEXT("name"), Out.Name);
		ReadOptionalString(Object, TEXT("description"), Out.Description);
		ReadOptionalString(Object, TEXT("semantic"), Out.Semantic);
		ReadOptionalString(Object, TEXT("kind"), Out.Kind);
		ReadOptionalString(Object, TEXT("unit"), Out.Unit);

		Result = RequireStringArray(Object, TEXT("components"), Path + TEXT(".components"), Out.Components);
		if (!Result.IsOk())
		{
			return Result;
		}

		int64 ComponentCount = 0;
		Result = RequireInteger(Object, TEXT("componentCount"), Path + TEXT(".componentCount"), ComponentCount);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (ComponentCount < 1 || ComponentCount > CFDViz::MaxCvfComponentCount)
		{
			return ManifestError(FString::Printf(
				TEXT("%s.componentCount is %lld; a CVF field carries 1..%d components"),
				*Path, ComponentCount, CFDViz::MaxCvfComponentCount));
		}
		Out.ComponentCount = static_cast<int32>(ComponentCount);

		FString DataType;
		Result = RequireString(Object, TEXT("dataType"), Path + TEXT(".dataType"), DataType);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (!TryParseDataType(DataType, Out.DataType))
		{
			return ManifestError(FString::Printf(
				TEXT("%s.dataType is \"%s\"; expected float16, float32 or uint8"), *Path, *DataType));
		}
		// float64 parses so this message can be specific rather than "unknown
		// dataType" (section 3.3).
		if (!IsDataTypeSupportedInCvf(Out.DataType))
		{
			return ManifestError(FString::Printf(
				TEXT("%s.dataType is \"float64\"; float64 field storage is not supported in CFDViz 1.0 ")
				TEXT("(section 3.3) - record it in solverPrecision instead"), *Path));
		}

		FString Association;
		Result = RequireString(Object, TEXT("association"), Path + TEXT(".association"), Association);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (!TryParseAssociation(Association, Out.Association))
		{
			// Named as reserved rather than unknown, which is the more useful
			// diagnosis for a manifest written against a future version.
			if (IsReservedAssociation(Association))
			{
				return ManifestError(FString::Printf(
					TEXT("%s.association is \"%s\", which is reserved for a future version and must be ")
					TEXT("rejected in CFDViz 1.0 (section 3.2)"), *Path, *Association));
			}
			return ManifestError(FString::Printf(
				TEXT("%s.association is \"%s\"; expected cell or point"), *Path, *Association));
		}

		FString GridId;
		Result = RequireString(Object, TEXT("grid"), Path + TEXT(".grid"), GridId);
		if (!Result.IsOk())
		{
			return Result;
		}
		Out.GridId = FName(*GridId);

		FString SolverPrecision;
		ReadOptionalString(Object, TEXT("solverPrecision"), SolverPrecision);
		if (!SolverPrecision.IsEmpty())
		{
			ECFDVizDataType Precision = ECFDVizDataType::Float32;
			if (!TryParseDataType(SolverPrecision, Precision))
			{
				return ManifestError(FString::Printf(
					TEXT("%s.solverPrecision is \"%s\"; expected float32 or float64"),
					*Path, *SolverPrecision));
			}
			// Provenance only - unlike dataType, float64 is legal here.
			Out.SolverPrecision = Precision;
		}

		FString Interpolation;
		ReadOptionalString(Object, TEXT("temporalInterpolation"), Interpolation);
		if (!Interpolation.IsEmpty())
		{
			ECFDVizInterpolation Parsed = ECFDVizInterpolation::Nearest;
			if (!TryParseInterpolation(Interpolation, Parsed))
			{
				return ManifestError(FString::Printf(
					TEXT("%s.temporalInterpolation is \"%s\"; expected nearest or linear"),
					*Path, *Interpolation));
			}
			Out.TemporalInterpolation = Parsed;
		}

		Result = ParseFieldStorage(Object, Path, Out.Storage);
		if (!Result.IsOk())
		{
			return Result;
		}
		Result = ParseStatistics(Object, Path, Out.Statistics);
		if (!Result.IsOk())
		{
			return Result;
		}
		return ParseDisplay(Object, Path, Out.Display);
	}

	FCFDVizResult ParseMesh(const TSharedPtr<FJsonObject>& Object, int32 MeshIndex, FCFDVizMesh& Out)
	{
		const FString Path = FString::Printf(TEXT("meshes[%d]"), MeshIndex);

		FString Id;
		FCFDVizResult Result = RequireString(Object, TEXT("id"), Path + TEXT(".id"), Id);
		if (!Result.IsOk())
		{
			return Result;
		}
		Out.Id = FName(*Id);

		ReadOptionalString(Object, TEXT("name"), Out.Name);
		ReadOptionalString(Object, TEXT("role"), Out.Role);

		FString MeshPath;
		Result = RequireString(Object, TEXT("path"), Path + TEXT(".path"), MeshPath);
		if (!Result.IsOk())
		{
			return Result;
		}
		if (!CFDViz::IsSafeRelativePath(MeshPath))
		{
			return FCFDVizResult::Fail(ECFDVizError::PathTraversal, FString::Printf(
				TEXT("%s.path \"%s\" is not a safe case-relative path (section 1.3)"), *Path, *MeshPath));
		}
		Out.Path = MoveTemp(MeshPath);

		// Defaults to true: in 1.0 a mesh declares a single path with no frame
		// placeholder, so its geometry cannot vary by frame.
		ReadOptionalBool(Object, TEXT("static"), Out.bStatic);

		Result = ParseMatrix(Object, TEXT("transform"), Path, Out.Transform);
		if (!Result.IsOk())
		{
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>* Patches = nullptr;
		if (TryGetArray(Object, TEXT("patches"), Patches))
		{
			for (int32 Index = 0; Index < Patches->Num(); ++Index)
			{
				const FString PatchPath = FString::Printf(TEXT("%s.patches[%d]"), *Path, Index);
				const TSharedPtr<FJsonObject>* PatchObject = nullptr;
				if (!(*Patches)[Index].IsValid() || !(*Patches)[Index]->TryGetObject(PatchObject)
					|| PatchObject == nullptr || !PatchObject->IsValid())
				{
					return ManifestError(FString::Printf(TEXT("%s must be an object"), *PatchPath));
				}

				FCFDVizBoundaryPatch Patch;

				int64 PatchId = 0;
				Result = RequireInteger(*PatchObject, TEXT("id"), PatchPath + TEXT(".id"), PatchId);
				if (!Result.IsOk())
				{
					return Result;
				}
				// uint32 to match the CVM patchIds element type exactly.
				if (PatchId < 0 || PatchId > static_cast<int64>(MAX_uint32))
				{
					return ManifestError(FString::Printf(
						TEXT("%s.id is %lld; a patch id must fit in a uint32"), *PatchPath, PatchId));
				}
				Patch.Id = static_cast<uint32>(PatchId);

				Result = RequireString(*PatchObject, TEXT("name"), PatchPath + TEXT(".name"), Patch.Name);
				if (!Result.IsOk())
				{
					return Result;
				}
				ReadOptionalString(*PatchObject, TEXT("type"), Patch.Type);
				ReadOptionalBool(*PatchObject, TEXT("defaultVisible"), Patch.bDefaultVisible);

				const TArray<TSharedPtr<FJsonValue>>* Color = nullptr;
				if (TryGetArray(*PatchObject, TEXT("color"), Color))
				{
					if (Color->Num() < 3 || Color->Num() > 4)
					{
						return ManifestError(FString::Printf(
							TEXT("%s.color must be [r,g,b] or [r,g,b,a]"), *PatchPath));
					}
					double Channels[4] = { 0.0, 0.0, 0.0, 1.0 };
					for (int32 Channel = 0; Channel < Color->Num(); ++Channel)
					{
						if ((*Color)[Channel]->Type != EJson::Number)
						{
							return ManifestError(FString::Printf(
								TEXT("%s.color[%d] must be a number"), *PatchPath, Channel));
						}
						Channels[Channel] = (*Color)[Channel]->AsNumber();
					}
					// LINEAR RGB, not sRGB. Converting here would shift every
					// patch colour away from what the manifest declared.
					Patch.Color = FLinearColor(
						static_cast<float>(Channels[0]), static_cast<float>(Channels[1]),
						static_cast<float>(Channels[2]), static_cast<float>(Channels[3]));
					Patch.bHasColor = true;
				}

				TSharedPtr<FJsonValue> Opacity;
				if (TryGetField(*PatchObject, TEXT("opacity"), Opacity) && Opacity->Type == EJson::Number)
				{
					Patch.Opacity = static_cast<float>(Opacity->AsNumber());
				}

				Out.Patches.Add(MoveTemp(Patch));
			}
		}

		return FCFDVizResult::Ok();
	}

	FCFDVizResult ParseDerivedField(const TSharedPtr<FJsonObject>& Object, int32 Index, FCFDVizDerivedField& Out)
	{
		const FString Path = FString::Printf(TEXT("derivedFields[%d]"), Index);

		FString Id;
		FCFDVizResult Result = RequireString(Object, TEXT("id"), Path + TEXT(".id"), Id);
		if (!Result.IsOk())
		{
			return Result;
		}
		Out.Id = FName(*Id);

		// Not parsed here - evaluation is the sampler's job - but required, so a
		// derived field always says what it computes.
		Result = RequireString(Object, TEXT("expression"), Path + TEXT(".expression"), Out.Expression);
		if (!Result.IsOk())
		{
			return Result;
		}

		ReadOptionalString(Object, TEXT("name"), Out.Name);
		ReadOptionalString(Object, TEXT("unit"), Out.Unit);
		ReadOptionalStringArray(Object, TEXT("components"), Out.Components);

		TSharedPtr<FJsonValue> ComponentCount;
		if (TryGetField(Object, TEXT("componentCount"), ComponentCount)
			&& ComponentCount->Type == EJson::Number)
		{
			int64 Count = 0;
			Result = RequireInteger(Object, TEXT("componentCount"), Path + TEXT(".componentCount"), Count);
			if (!Result.IsOk())
			{
				return Result;
			}
			if (Count < 0 || Count > MAX_int32)
			{
				return ManifestError(FString::Printf(TEXT("%s.componentCount is %lld"), *Path, Count));
			}
			Out.ComponentCount = static_cast<int32>(Count);
		}

		return ParseDisplay(Object, Path, Out.Display);
	}

	FCFDVizResult ParseStructure(const TSharedPtr<FJsonObject>& Object, int32 Index, FCFDVizStructure& Out)
	{
		const FString Path = FString::Printf(TEXT("structures[%d]"), Index);

		FString Id;
		FCFDVizResult Result = RequireString(Object, TEXT("id"), Path + TEXT(".id"), Id);
		if (!Result.IsOk())
		{
			return Result;
		}
		Out.Id = FName(*Id);

		FString MeshId;
		Result = RequireString(Object, TEXT("mesh"), Path + TEXT(".mesh"), MeshId);
		if (!Result.IsOk())
		{
			return Result;
		}
		Out.MeshId = FName(*MeshId);

		ReadOptionalString(Object, TEXT("name"), Out.Name);
		ReadOptionalString(Object, TEXT("displacementField"), Out.DisplacementField);
		ReadOptionalString(Object, TEXT("velocityField"), Out.VelocityField);
		ReadOptionalString(Object, TEXT("stressField"), Out.StressField);
		ReadOptionalString(Object, TEXT("strainField"), Out.StrainField);
		ReadOptionalStringArray(Object, TEXT("variants"), Out.Variants);

		ReadOptionalString(Object, TEXT("vertexToNodeMap"), Out.VertexToNodeMap);
		if (!Out.VertexToNodeMap.IsEmpty() && !CFDViz::IsSafeRelativePath(Out.VertexToNodeMap))
		{
			return FCFDVizResult::Fail(ECFDVizError::PathTraversal, FString::Printf(
				TEXT("%s.vertexToNodeMap \"%s\" is not a safe case-relative path (section 1.3)"),
				*Path, *Out.VertexToNodeMap));
		}

		return FCFDVizResult::Ok();
	}
}

FCFDVizResult FCFDVizCase::ParseFromString(const FString& Json, const FString& ContextPath, FCFDVizCase& OutCase)
{
	// Built into a local and only moved out on success, so a caller that ignores
	// the result cannot end up rendering a half-populated case.
	FCFDVizCase Parsed;

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidManifest,
			FString::Printf(TEXT("manifest is not valid JSON: %s"), *Reader->GetErrorMessage()),
			ContextPath);
	}

	if (!ContextPath.IsEmpty())
	{
		Parsed.ManifestPath = ContextPath;
		// The case root is the manifest's DIRECTORY - every relative path in the
		// manifest resolves against it.
		Parsed.CaseRootDir = FPaths::GetPath(ContextPath);
	}

	// --- format marker ------------------------------------------------------
	FCFDVizResult Result = RequireString(Root, TEXT("format"), TEXT("format"), Parsed.Format);
	if (!Result.IsOk())
	{
		Result.FilePath = ContextPath;
		return Result;
	}
	if (!Parsed.Format.Equals(CFDViz::FormatMarker, ESearchCase::CaseSensitive))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidManifest, FString::Printf(
			TEXT("format is \"%s\", expected \"%s\"; this is not a CFDViz case"),
			*Parsed.Format, CFDViz::FormatMarker), ContextPath);
	}

	// --- version (format rule 1.4) ------------------------------------------
	Result = RequireString(Root, TEXT("version"), TEXT("version"), Parsed.FormatVersion);
	if (!Result.IsOk())
	{
		Result.FilePath = ContextPath;
		return Result;
	}
	if (!TryParseSemanticVersion(Parsed.FormatVersion, Parsed.VersionMajor, Parsed.VersionMinor, Parsed.VersionPatch))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidManifest, FString::Printf(
			TEXT("version \"%s\" is not a MAJOR.MINOR.PATCH semantic version"), *Parsed.FormatVersion),
			ContextPath);
	}
	// A newer MINOR is ACCEPTED - only the major is a compatibility break.
	// Treating any difference as fatal would make every 1.x case unreadable the
	// day 1.1 ships.
	if (Parsed.VersionMajor != static_cast<int32>(CFDViz::SupportedMajorVersion))
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedVersion, FString::Printf(
			TEXT("unsupported major version %d in version \"%s\"; this reader implements CFDViz %u.x"),
			Parsed.VersionMajor, *Parsed.FormatVersion, CFDViz::SupportedMajorVersion), ContextPath);
	}

	// --- required blocks ----------------------------------------------------
	Result = ParseCaseMetadata(Root, Parsed.Metadata);
	if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }

	Result = ParseUnits(Root, Parsed.Units);
	if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }

	Result = ParseCoordinates(Root, Parsed.Coordinates);
	if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }

	Result = ParseTimeline(Root, Parsed.Timeline);
	if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }

	// --- grids --------------------------------------------------------------
	const TArray<TSharedPtr<FJsonValue>>* Grids = nullptr;
	Result = RequireArray(Root, TEXT("grids"), TEXT("grids"), Grids);
	if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }

	for (int32 Index = 0; Index < Grids->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!(*Grids)[Index].IsValid() || !(*Grids)[Index]->TryGetObject(Object)
			|| Object == nullptr || !Object->IsValid())
		{
			return FCFDVizResult::Fail(ECFDVizError::InvalidManifest,
				FString::Printf(TEXT("grids[%d] must be an object"), Index), ContextPath);
		}
		FCFDVizGridDescriptor Grid;
		Result = ParseGrid(*Object, Index, Grid);
		if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }
		Parsed.Grids.Add(MoveTemp(Grid));
	}

	// --- fields -------------------------------------------------------------
	const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
	Result = RequireArray(Root, TEXT("fields"), TEXT("fields"), Fields);
	if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }

	for (int32 Index = 0; Index < Fields->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!(*Fields)[Index].IsValid() || !(*Fields)[Index]->TryGetObject(Object)
			|| Object == nullptr || !Object->IsValid())
		{
			return FCFDVizResult::Fail(ECFDVizError::InvalidManifest,
				FString::Printf(TEXT("fields[%d] must be an object"), Index), ContextPath);
		}
		FCFDVizField Field;
		Result = ParseField(*Object, Index, Field);
		if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }
		Parsed.Fields.Add(MoveTemp(Field));
	}

	// --- optional collections -----------------------------------------------
	const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
	if (TryGetArray(Root, TEXT("meshes"), Meshes))
	{
		for (int32 Index = 0; Index < Meshes->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*Meshes)[Index].IsValid() || !(*Meshes)[Index]->TryGetObject(Object)
				|| Object == nullptr || !Object->IsValid())
			{
				return FCFDVizResult::Fail(ECFDVizError::InvalidManifest,
					FString::Printf(TEXT("meshes[%d] must be an object"), Index), ContextPath);
			}
			FCFDVizMesh Mesh;
			Result = ParseMesh(*Object, Index, Mesh);
			if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }
			Parsed.Meshes.Add(MoveTemp(Mesh));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* DerivedFields = nullptr;
	if (TryGetArray(Root, TEXT("derivedFields"), DerivedFields))
	{
		for (int32 Index = 0; Index < DerivedFields->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*DerivedFields)[Index].IsValid() || !(*DerivedFields)[Index]->TryGetObject(Object)
				|| Object == nullptr || !Object->IsValid())
			{
				return FCFDVizResult::Fail(ECFDVizError::InvalidManifest,
					FString::Printf(TEXT("derivedFields[%d] must be an object"), Index), ContextPath);
			}
			FCFDVizDerivedField Derived;
			Result = ParseDerivedField(*Object, Index, Derived);
			if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }
			Parsed.DerivedFields.Add(MoveTemp(Derived));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Structures = nullptr;
	if (TryGetArray(Root, TEXT("structures"), Structures))
	{
		for (int32 Index = 0; Index < Structures->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*Structures)[Index].IsValid() || !(*Structures)[Index]->TryGetObject(Object)
				|| Object == nullptr || !Object->IsValid())
			{
				return FCFDVizResult::Fail(ECFDVizError::InvalidManifest,
					FString::Printf(TEXT("structures[%d] must be an object"), Index), ContextPath);
			}
			FCFDVizStructure Structure;
			Result = ParseStructure(*Object, Index, Structure);
			if (!Result.IsOk()) { Result.FilePath = ContextPath; return Result; }
			Parsed.Structures.Add(MoveTemp(Structure));
		}
	}

	TSharedPtr<FJsonValue> ProvenanceValue;
	if (TryGetField(Root, TEXT("provenance"), ProvenanceValue))
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (ProvenanceValue->TryGetObject(Object) && Object != nullptr && Object->IsValid())
		{
			ReadOptionalString(*Object, TEXT("generatorCommand"), Parsed.Provenance.GeneratorCommand);
			ReadOptionalString(*Object, TEXT("generatorVersion"), Parsed.Provenance.GeneratorVersion);
			ReadOptionalString(*Object, TEXT("sourceCase"), Parsed.Provenance.SourceCase);
			ReadOptionalStringArray(*Object, TEXT("notes"), Parsed.Provenance.Notes);
		}
	}

	// Validation is not optional: running it here is what makes a successful
	// return always mean a usable case, with no way to obtain a
	// parsed-but-unvalidated one by forgetting a call.
	Result = Parsed.Validate();
	if (!Result.IsOk())
	{
		Result.FilePath = ContextPath;
		return Result;
	}

	OutCase = MoveTemp(Parsed);
	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizCase::LoadFromFile(const FString& ManifestPath, FCFDVizCase& OutCase)
{
	// The size is checked BEFORE the contents are read, so a hostile path cannot
	// make this reader allocate an arbitrary buffer (format rule 1.5). A
	// manifest has no internal length field to check against, so the ceiling is
	// the check.
	const int64 FileSize = IFileManager::Get().FileSize(*ManifestPath);
	if (FileSize == INDEX_NONE)
	{
		return FCFDVizResult::Fail(ECFDVizError::FileNotFound,
			TEXT("manifest not found"), ManifestPath);
	}
	if (FileSize > CFDViz::MaxManifestBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("manifest is %lld bytes, beyond the %lld byte limit"),
			FileSize, CFDViz::MaxManifestBytes), ManifestPath);
	}

	// Read as BYTES, not through LoadFileToString. That helper silently skips a
	// UTF-8 BOM and will even decode UTF-16, so it would accept a manifest the
	// Python reference rejects by name - and the two implementations have to
	// agree on what is a legal file.
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *ManifestPath))
	{
		return FCFDVizResult::Fail(ECFDVizError::FileReadFailed,
			TEXT("manifest could not be read"), ManifestPath);
	}
	// Re-checked against what was actually read: the file may have grown between
	// the size query above and this read.
	if (Bytes.Num() > CFDViz::MaxManifestBytes)
	{
		return FCFDVizResult::Fail(ECFDVizError::AllocationTooLarge, FString::Printf(
			TEXT("manifest is %d bytes, beyond the %lld byte limit"),
			Bytes.Num(), CFDViz::MaxManifestBytes), ManifestPath);
	}

	// Section 1.2: UTF-8 without a BOM. Detected rather than stripped, because a
	// BOM means the writer is not producing what the format requires, and strict
	// parsers elsewhere in the toolchain will choke on it.
	if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidManifest,
			TEXT("manifest starts with a UTF-8 byte-order mark; CFDViz JSON is UTF-8 without a BOM (section 1.2)"),
			ManifestPath, /*ByteOffset=*/0);
	}
	// UTF-16 is not UTF-8 at all. Named here so the error says so, instead of
	// surfacing as "invalid JSON" on text that decoded to mojibake.
	if (Bytes.Num() >= 2
		&& ((Bytes[0] == 0xFF && Bytes[1] == 0xFE) || (Bytes[0] == 0xFE && Bytes[1] == 0xFF)))
	{
		return FCFDVizResult::Fail(ECFDVizError::InvalidManifest,
			TEXT("manifest starts with a UTF-16 byte-order mark; CFDViz JSON is UTF-8 without a BOM (section 1.2)"),
			ManifestPath, /*ByteOffset=*/0);
	}

	FString Text;
	FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());

	// Absolute, so CaseRootDir is a usable base no matter what the caller passed.
	const FString AbsolutePath = FPaths::ConvertRelativePathToFull(ManifestPath);
	return ParseFromString(Text, AbsolutePath, OutCase);
}

/* -------------------------------------------------------------------------- */
/* Validation - section 3.1                                                     */
/* -------------------------------------------------------------------------- */

namespace
{
	/**
	 * Grid dimensions must be ADDRESSABLE, not merely positive.
	 *
	 * Stricter than the schema on purpose. The schema types a dimension as a
	 * uint32, but FIntVector is int32 and a point-associated field needs
	 * (nx+1, ny+1, nz+1) values. A dimension near the uint32 ceiling would wrap
	 * negative on the way into that +1 and turn every downstream bounds check
	 * into a rubber stamp - the check would still run, it would just always pass.
	 *
	 * The PRODUCT is deliberately not capped. A grid large enough for its value
	 * count to exceed int32 is a legal manifest that the Python reference
	 * accepts, and FCFDVizGrid::ValueCount already reports an unrepresentable
	 * total as INDEX_NONE for the reader to refuse with a size-specific error.
	 * Capping it here would reject a case one implementation loads and the other
	 * does not - the exact divergence the two-implementation design exists to
	 * catch.
	 */
	FCFDVizResult ValidateGridDimensions(const FCFDVizGridDescriptor& Grid, int32 GridIndex)
	{
		// Asked through the helper every consumer actually calls, so a future
		// change to either side cannot leave a grid that validates here but reads
		// as zero-sized there. ValueCounts returns (0,0,0) rather than a wrapped
		// count when the +1 does not fit, and FCFDVizGrid::IsValid has already
		// rejected a non-positive dimension - so a zero here means exactly that
		// overflow.
		const FIntVector PointCounts = Grid.Geometry.ValueCounts(ECFDVizAssociation::Point);
		if (PointCounts.X <= 0 || PointCounts.Y <= 0 || PointCounts.Z <= 0)
		{
			const FIntVector& Dimensions = Grid.Geometry.Dimensions;
			return ManifestError(FString::Printf(
				TEXT("grids[%d] (\"%s\").dimensions is [%d, %d, %d]; one more than this overflows the int32 ")
				TEXT("a point-associated value count needs"),
				GridIndex, *Grid.Id.ToString(), Dimensions.X, Dimensions.Y, Dimensions.Z));
		}
		return FCFDVizResult::Ok();
	}
}

FCFDVizResult FCFDVizCase::Validate() const
{
	// Returns the FIRST failure, naming the offending property with a JSON path.
	if (!Format.Equals(CFDViz::FormatMarker, ESearchCase::CaseSensitive))
	{
		return ManifestError(FString::Printf(
			TEXT("format is \"%s\", expected \"%s\""), *Format, CFDViz::FormatMarker));
	}
	if (VersionMajor != static_cast<int32>(CFDViz::SupportedMajorVersion))
	{
		return FCFDVizResult::Fail(ECFDVizError::UnsupportedVersion, FString::Printf(
			TEXT("unsupported major version %d; this reader implements CFDViz %u.x"),
			VersionMajor, CFDViz::SupportedMajorVersion));
	}

	// --- timeline -----------------------------------------------------------
	if (Timeline.FrameCount != Timeline.Times.Num())
	{
		return ManifestError(FString::Printf(
			TEXT("timeline.frameCount is %d but timeline.times has %d entries"),
			Timeline.FrameCount, Timeline.Times.Num()));
	}
	if (Timeline.HasSteps() && Timeline.Steps.Num() != Timeline.FrameCount)
	{
		return ManifestError(FString::Printf(
			TEXT("timeline.frameCount is %d but timeline.steps has %d entries"),
			Timeline.FrameCount, Timeline.Steps.Num()));
	}
	if (!Timeline.IsStrictlyIncreasing())
	{
		return ManifestError(
			TEXT("timeline.times must be finite and strictly increasing; a repeated or NaN time is not a frame ordering"));
	}

	// --- grids --------------------------------------------------------------
	TSet<FName> GridIds;
	for (int32 Index = 0; Index < Grids.Num(); ++Index)
	{
		const FCFDVizGridDescriptor& Grid = Grids[Index];
		if (Grid.Id.IsNone())
		{
			return ManifestError(FString::Printf(TEXT("grids[%d].id is empty"), Index));
		}
		// A duplicate id means the manifest has two answers for one reference,
		// and which one a reader picked would decide what the user sees.
		if (GridIds.Contains(Grid.Id))
		{
			return ManifestError(FString::Printf(
				TEXT("duplicate grid id \"%s\""), *Grid.Id.ToString()));
		}
		GridIds.Add(Grid.Id);

		if (!Grid.Geometry.IsValid())
		{
			return ManifestError(FString::Printf(
				TEXT("grids[%d] (\"%s\") has invalid geometry: dimensions must be at least 1 per axis and spacing greater than 0"),
				Index, *Grid.Id.ToString()));
		}

		const FCFDVizResult GridResult = ValidateGridDimensions(Grid, Index);
		if (!GridResult.IsOk())
		{
			return GridResult;
		}
	}

	// --- fields -------------------------------------------------------------
	TSet<FName> FieldIds;
	TSet<uint32> NumericIds;
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		const FCFDVizField& Field = Fields[Index];
		const FString Label = Field.Id.IsNone()
			? FString::Printf(TEXT("fields[%d]"), Index)
			: FString::Printf(TEXT("field \"%s\""), *Field.Id.ToString());

		if (Field.Id.IsNone())
		{
			return ManifestError(FString::Printf(TEXT("fields[%d].id is empty"), Index));
		}
		if (FieldIds.Contains(Field.Id))
		{
			return ManifestError(FString::Printf(
				TEXT("duplicate field id \"%s\""), *Field.Id.ToString()));
		}
		FieldIds.Add(Field.Id);

		// A duplicate numericId would make it impossible to tie a .cvf back to
		// one manifest entry, which is the whole point of the field.
		if (NumericIds.Contains(Field.NumericId))
		{
			return ManifestError(FString::Printf(
				TEXT("duplicate field numericId %u"), Field.NumericId));
		}
		NumericIds.Add(Field.NumericId);

		if (Field.Components.Num() != Field.ComponentCount)
		{
			return ManifestError(FString::Printf(
				TEXT("%s: componentCount is %d but components has %d names"),
				*Label, Field.ComponentCount, Field.Components.Num()));
		}

		// Re-checked here even though ParseField already rejects an illegal
		// spelling. This function is public and documented as idempotent so a
		// case assembled programmatically - by a test, or by a future writer -
		// gets the same guarantees as a parsed one, and such a case never went
		// through the string enums at all. Section 3.1 lists both.
		if (!IsDataTypeSupportedInCvf(Field.DataType))
		{
			return ManifestError(FString::Printf(
				TEXT("%s: dataType %s cannot be stored in a CVF; section 3.1 allows float16, float32 or uint8"),
				*Label, DataTypeToString(Field.DataType)));
		}
		if (Field.Association != ECFDVizAssociation::Cell && Field.Association != ECFDVizAssociation::Point)
		{
			return ManifestError(FString::Printf(
				TEXT("%s: association %s is reserved for a future version; CFDViz 1.0 defines cell and point only ")
				TEXT("(section 3.2)"), *Label, AssociationToString(Field.Association)));
		}

		if (!GridIds.Contains(Field.GridId))
		{
			return ManifestError(FString::Printf(
				TEXT("%s: grid \"%s\" does not name a declared grid"),
				*Label, *Field.GridId.ToString()));
		}

		if (!CFDViz::IsSafeRelativePath(Field.Storage.PathPattern))
		{
			return FCFDVizResult::Fail(ECFDVizError::PathTraversal, FString::Printf(
				TEXT("%s: storage.pathPattern \"%s\" is not a safe case-relative path (section 1.3)"),
				*Label, *Field.Storage.PathPattern));
		}

		// The pattern and its EXPANSIONS are different strings, so both are
		// checked. First and last frame only: a pattern that expands safely at
		// both ends cannot misbehave between them, and checking every frame of a
		// hundred-thousand-frame case would cost more than it proves.
		if (Timeline.FrameCount > 0)
		{
			const int32 Probes[2] = { 0, Timeline.FrameCount - 1 };
			for (const int32 Frame : Probes)
			{
				FString Expanded;
				FCFDVizResult Result = FormatFramePath(Field.Storage.PathPattern, Frame, Expanded);
				if (!Result.IsOk())
				{
					return ManifestError(FString::Printf(
						TEXT("%s: storage.pathPattern \"%s\" does not expand for frame %d: %s"),
						*Label, *Field.Storage.PathPattern, Frame, *Result.Message));
				}
				if (!CFDViz::IsSafeRelativePath(Expanded))
				{
					return FCFDVizResult::Fail(ECFDVizError::PathTraversal, FString::Printf(
						TEXT("%s: storage.pathPattern expands to the unsafe path \"%s\""),
						*Label, *Expanded));
				}
			}
		}
	}

	// Checked AFTER the field loop, because a maskField may name any declared
	// field regardless of declaration order.
	for (int32 Index = 0; Index < Grids.Num(); ++Index)
	{
		const FCFDVizGridDescriptor& Grid = Grids[Index];
		if (Grid.HasMaskField() && !FieldIds.Contains(Grid.MaskFieldId))
		{
			return ManifestError(FString::Printf(
				TEXT("grid \"%s\": maskField \"%s\" does not name a declared field"),
				*Grid.Id.ToString(), *Grid.MaskFieldId.ToString()));
		}
	}

	// --- meshes -------------------------------------------------------------
	TSet<FName> MeshIds;
	for (int32 Index = 0; Index < Meshes.Num(); ++Index)
	{
		const FCFDVizMesh& Mesh = Meshes[Index];
		if (Mesh.Id.IsNone())
		{
			return ManifestError(FString::Printf(TEXT("meshes[%d].id is empty"), Index));
		}
		if (MeshIds.Contains(Mesh.Id))
		{
			return ManifestError(FString::Printf(TEXT("duplicate mesh id \"%s\""), *Mesh.Id.ToString()));
		}
		MeshIds.Add(Mesh.Id);

		if (!CFDViz::IsSafeRelativePath(Mesh.Path))
		{
			return FCFDVizResult::Fail(ECFDVizError::PathTraversal, FString::Printf(
				TEXT("mesh \"%s\": path \"%s\" is not a safe case-relative path (section 1.3)"),
				*Mesh.Id.ToString(), *Mesh.Path));
		}

		// Patch ids index the CVM patchIds array, so a duplicate would make a
		// triangle's patch ambiguous.
		TSet<uint32> PatchIds;
		for (const FCFDVizBoundaryPatch& Patch : Mesh.Patches)
		{
			if (PatchIds.Contains(Patch.Id))
			{
				return ManifestError(FString::Printf(
					TEXT("mesh \"%s\": duplicate patch id %u"), *Mesh.Id.ToString(), Patch.Id));
			}
			PatchIds.Add(Patch.Id);

			if (Patch.Name.IsEmpty())
			{
				return ManifestError(FString::Printf(
					TEXT("mesh \"%s\": patch %u has an empty name"), *Mesh.Id.ToString(), Patch.Id));
			}
		}
	}

	// --- derived fields -----------------------------------------------------
	TSet<FName> DerivedIds;
	for (int32 Index = 0; Index < DerivedFields.Num(); ++Index)
	{
		const FCFDVizDerivedField& Derived = DerivedFields[Index];
		if (Derived.Id.IsNone())
		{
			return ManifestError(FString::Printf(TEXT("derivedFields[%d].id is empty"), Index));
		}
		if (DerivedIds.Contains(Derived.Id))
		{
			return ManifestError(FString::Printf(
				TEXT("duplicate derived field id \"%s\""), *Derived.Id.ToString()));
		}
		DerivedIds.Add(Derived.Id);

		// Only when BOTH are declared - componentCount defaults to 0, which
		// means "not declared", not "zero components".
		if (Derived.ComponentCount > 0 && Derived.Components.Num() > 0
			&& Derived.ComponentCount != Derived.Components.Num())
		{
			return ManifestError(FString::Printf(
				TEXT("derived field \"%s\": componentCount is %d but components has %d names"),
				*Derived.Id.ToString(), Derived.ComponentCount, Derived.Components.Num()));
		}
	}

	// --- structures ---------------------------------------------------------
	TSet<FName> StructureIds;
	for (int32 Index = 0; Index < Structures.Num(); ++Index)
	{
		const FCFDVizStructure& Structure = Structures[Index];
		if (Structure.Id.IsNone())
		{
			return ManifestError(FString::Printf(TEXT("structures[%d].id is empty"), Index));
		}
		if (StructureIds.Contains(Structure.Id))
		{
			return ManifestError(FString::Printf(
				TEXT("duplicate structure id \"%s\""), *Structure.Id.ToString()));
		}
		StructureIds.Add(Structure.Id);

		if (!MeshIds.Contains(Structure.MeshId))
		{
			return ManifestError(FString::Printf(
				TEXT("structure \"%s\": mesh \"%s\" does not name a declared mesh"),
				*Structure.Id.ToString(), *Structure.MeshId.ToString()));
		}
	}

	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizCase::CheckCodecSupport() const
{
	for (const FCFDVizField& Field : Fields)
	{
		if (Field.Storage.Codec == ECFDVizCodec::Zstd)
		{
			// Section 7 pins this wording exactly, and the Python reference
			// emits the identical string - so the message carries it and
			// nothing else. The field is named through FilePath instead, which
			// keeps the mandated text intact while still telling a user which
			// field to re-encode.
			return FCFDVizResult::Fail(ECFDVizError::UnsupportedCodec,
				FString(CFDViz::ZstdRejectionMessage),
				FString::Printf(TEXT("field \"%s\""), *Field.Id.ToString()));
		}
		if (!IsCodecSupported(Field.Storage.Codec))
		{
			return FCFDVizResult::Fail(ECFDVizError::UnsupportedCodec, FString::Printf(
				TEXT("field \"%s\" declares codec %s, which this build cannot decode"),
				*Field.Id.ToString(), CodecToString(Field.Storage.Codec)));
		}
	}
	return FCFDVizResult::Ok();
}

/* -------------------------------------------------------------------------- */
/* Lookups                                                                      */
/* -------------------------------------------------------------------------- */

const FCFDVizField* FCFDVizCase::FindField(FName Id) const
{
	for (const FCFDVizField& Field : Fields)
	{
		if (Field.Id == Id)
		{
			return &Field;
		}
	}
	return nullptr;
}

const FCFDVizField* FCFDVizCase::FindFieldByNumericId(uint32 NumericId) const
{
	for (const FCFDVizField& Field : Fields)
	{
		if (Field.NumericId == NumericId)
		{
			return &Field;
		}
	}
	return nullptr;
}

const FCFDVizGridDescriptor* FCFDVizCase::FindGrid(FName Id) const
{
	for (const FCFDVizGridDescriptor& Grid : Grids)
	{
		if (Grid.Id == Id)
		{
			return &Grid;
		}
	}
	return nullptr;
}

const FCFDVizMesh* FCFDVizCase::FindMesh(FName Id) const
{
	for (const FCFDVizMesh& Mesh : Meshes)
	{
		if (Mesh.Id == Id)
		{
			return &Mesh;
		}
	}
	return nullptr;
}

const FCFDVizDerivedField* FCFDVizCase::FindDerivedField(FName Id) const
{
	for (const FCFDVizDerivedField& Derived : DerivedFields)
	{
		if (Derived.Id == Id)
		{
			return &Derived;
		}
	}
	return nullptr;
}

const FCFDVizGridDescriptor* FCFDVizCase::FindGridForField(const FCFDVizField& Field) const
{
	return FindGrid(Field.GridId);
}

/* -------------------------------------------------------------------------- */
/* Paths                                                                        */
/* -------------------------------------------------------------------------- */

bool FCFDVizCase::IsSafeRelativePath(const FString& Path)
{
	// One implementation, not two: this forwards to the shared checker so the
	// manifest API and the schema's `relativePath` pattern cannot drift apart.
	return CFDViz::IsSafeRelativePath(Path);
}

FCFDVizResult FCFDVizCase::FormatFramePath(const FString& PathPattern, int32 FrameIndex, FString& OutRelativePath)
{
	if (FrameIndex < 0)
	{
		return FCFDVizResult::Fail(ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("frame index %d is negative"), FrameIndex));
	}

	// A hand-written subset of Python's str.format: `{{`/`}}` for literal braces
	// and `{frame}` with an optional `[0][width][d]` spec. Anything else is
	// REJECTED rather than approximated - a silently mis-expanded path fails
	// later as a confusing "file not found", far from the manifest that caused it.
	FString Result;
	Result.Reserve(PathPattern.Len() + 16);

	int32 Index = 0;
	while (Index < PathPattern.Len())
	{
		const TCHAR Char = PathPattern[Index];

		if (Char == TEXT('}'))
		{
			// A closing brace is only legal doubled; a stray one is a typo that
			// would otherwise pass through into a filename.
			if (Index + 1 < PathPattern.Len() && PathPattern[Index + 1] == TEXT('}'))
			{
				Result.AppendChar(TEXT('}'));
				Index += 2;
				continue;
			}
			return ManifestError(FString::Printf(
				TEXT("pathPattern \"%s\" has an unmatched '}' at position %d"), *PathPattern, Index));
		}

		if (Char != TEXT('{'))
		{
			Result.AppendChar(Char);
			++Index;
			continue;
		}

		// Doubled '{' is a literal brace.
		if (Index + 1 < PathPattern.Len() && PathPattern[Index + 1] == TEXT('{'))
		{
			Result.AppendChar(TEXT('{'));
			Index += 2;
			continue;
		}

		const int32 Close = PathPattern.Find(TEXT("}"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, Index);
		if (Close == INDEX_NONE)
		{
			return ManifestError(FString::Printf(
				TEXT("pathPattern \"%s\" has an unclosed '{'"), *PathPattern));
		}

		const FString Placeholder = PathPattern.Mid(Index + 1, Close - Index - 1);
		FString FieldName = Placeholder;
		FString Spec;
		int32 ColonIndex = INDEX_NONE;
		if (Placeholder.FindChar(TEXT(':'), ColonIndex))
		{
			FieldName = Placeholder.Left(ColonIndex);
			Spec = Placeholder.Mid(ColonIndex + 1);
		}

		if (!FieldName.Equals(TEXT("frame"), ESearchCase::CaseSensitive))
		{
			return ManifestError(FString::Printf(
				TEXT("pathPattern \"%s\" uses the placeholder \"{%s}\"; only {frame} is supported"),
				*PathPattern, *FieldName));
		}

		// Parse `[0][width][d]`.
		bool bZeroPad = false;
		int32 Width = 0;
		{
			int32 SpecIndex = 0;
			if (SpecIndex < Spec.Len() && Spec[SpecIndex] == TEXT('0'))
			{
				bZeroPad = true;
				++SpecIndex;
			}
			int32 DigitStart = SpecIndex;
			while (SpecIndex < Spec.Len() && FChar::IsDigit(Spec[SpecIndex]))
			{
				++SpecIndex;
			}
			if (SpecIndex > DigitStart)
			{
				const FString WidthText = Spec.Mid(DigitStart, SpecIndex - DigitStart);
				Width = FCString::Atoi(*WidthText);
			}
			// A trailing 'd' is the only conversion this subset accepts.
			if (SpecIndex < Spec.Len() && Spec[SpecIndex] == TEXT('d'))
			{
				++SpecIndex;
			}
			if (SpecIndex != Spec.Len())
			{
				return ManifestError(FString::Printf(
					TEXT("pathPattern \"%s\" uses the unsupported format spec \"%s\"; expected [0][width][d]"),
					*PathPattern, *Spec));
			}
		}

		// Bounds the string a hostile pattern can make this reader build.
		if (Width > CFDViz::MaxFramePlaceholderWidth)
		{
			return ManifestError(FString::Printf(
				TEXT("pathPattern \"%s\" requests a field width of %d; the maximum is %d"),
				*PathPattern, Width, CFDViz::MaxFramePlaceholderWidth));
		}

		FString Number = FString::FromInt(FrameIndex);
		// Python pads but never TRUNCATES: a value wider than its field keeps
		// every digit, so frame 12345 in {frame:03d} is "12345", not "345".
		if (bZeroPad && Number.Len() < Width)
		{
			Number = FString::ChrN(Width - Number.Len(), TEXT('0')) + Number;
		}
		else if (!bZeroPad && Number.Len() < Width)
		{
			// A bare width right-aligns with spaces in Python. Spaces in a
			// filename are almost certainly not what a manifest meant, so this
			// is refused rather than silently producing " 42.cvf".
			return ManifestError(FString::Printf(
				TEXT("pathPattern \"%s\" requests a width with no zero-fill, which would pad the frame index with spaces"),
				*PathPattern));
		}

		Result.Append(Number);
		Index = Close + 1;
	}

	OutRelativePath = MoveTemp(Result);
	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizCase::ResolveRelativePath(const FString& RelativePath, FString& OutAbsolutePath) const
{
	// The lexical check runs FIRST, before any path is built or any filesystem
	// call is made. Resolving first and checking afterwards can be defeated by a
	// symlink planted inside the case directory.
	if (!CFDViz::IsSafeRelativePath(RelativePath))
	{
		return FCFDVizResult::Fail(ECFDVizError::PathTraversal, FString::Printf(
			TEXT("\"%s\" is not a safe case-relative path (section 1.3)"), *RelativePath), ManifestPath);
	}
	if (CaseRootDir.IsEmpty())
	{
		// Never falls back to the process working directory - that would resolve
		// to a different file depending on where the editor was launched from.
		return FCFDVizResult::Fail(ECFDVizError::InvalidManifest,
			TEXT("the case root is unknown, so a relative path cannot be resolved"), ManifestPath);
	}

	const FString Combined = FPaths::ConvertRelativePathToFull(FPaths::Combine(CaseRootDir, RelativePath));

	// Redundant given the lexical check, and kept as the layer that would catch a
	// future change to either half. IsUnderDirectory rather than StartsWith: a
	// bare prefix compare puts "/cases/run-evil" under "/cases/run", which is
	// exactly the containment bug this line exists to prevent.
	if (!FPaths::IsUnderDirectory(Combined, CaseRootDir))
	{
		return FCFDVizResult::Fail(ECFDVizError::PathTraversal, FString::Printf(
			TEXT("\"%s\" resolves outside the case root"), *RelativePath), ManifestPath);
	}

	OutAbsolutePath = Combined;
	return FCFDVizResult::Ok();
}

FCFDVizResult FCFDVizCase::ResolveFieldFramePath(const FCFDVizField& Field, int32 FrameIndex, FString& OutAbsolutePath) const
{
	// Bounds-checked against the timeline, so a caller cannot silently build a
	// path to a frame the case does not contain.
	if (FrameIndex < 0 || FrameIndex >= Timeline.FrameCount)
	{
		return FCFDVizResult::Fail(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("frame %d is outside the %d frames this case declares"),
			FrameIndex, Timeline.FrameCount), ManifestPath);
	}

	FString Relative;
	FCFDVizResult Result = FormatFramePath(Field.Storage.PathPattern, FrameIndex, Relative);
	if (!Result.IsOk())
	{
		Result.FilePath = ManifestPath;
		return Result;
	}

	// Does NOT touch the filesystem: a missing file is the CVF reader's error to
	// report, at the point where it can say what it failed to open.
	return ResolveRelativePath(Relative, OutAbsolutePath);
}

FCFDVizResult FCFDVizCase::ResolveFieldFramePath(FName FieldId, int32 FrameIndex, FString& OutAbsolutePath) const
{
	const FCFDVizField* Field = FindField(FieldId);
	if (Field == nullptr)
	{
		return FCFDVizResult::Fail(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("no field \"%s\" is declared in this case"), *FieldId.ToString()), ManifestPath);
	}
	return ResolveFieldFramePath(*Field, FrameIndex, OutAbsolutePath);
}

FCFDVizResult FCFDVizCase::ResolveMeshPath(FName MeshId, FString& OutAbsolutePath) const
{
	const FCFDVizMesh* Mesh = FindMesh(MeshId);
	if (Mesh == nullptr)
	{
		return FCFDVizResult::Fail(ECFDVizError::IndexOutOfRange, FString::Printf(
			TEXT("no mesh \"%s\" is declared in this case"), *MeshId.ToString()), ManifestPath);
	}
	return ResolveRelativePath(Mesh->Path, OutAbsolutePath);
}
