// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizSession.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/**
 * `.cfdvizsession` serialisation (plan.md section 14). See the header.
 *
 * EVERY JSON TYPE IS CONFINED TO THIS FILE, for the same reason as in
 * CFDVizManifest.cpp: Json and JsonUtilities are PRIVATE dependencies of this
 * module, so an FJsonObject in the public header would not compile downstream.
 *
 * NUMBERS ARE WRITTEN AS DOUBLES AND READ AS DOUBLES. A probe position that went
 * through a float would move to the edge of its voxel and read a different -
 * entirely plausible - value on reload. FJsonValueNumber is double-backed, so
 * this costs nothing as long as nothing narrows on the way through.
 *
 * ENUMS ARE WRITTEN AS NAMES, NOT ORDINALS. An ordinal silently changes meaning
 * the day someone inserts a value into the middle of an enum, and the session
 * reopens with the wrong colormap and no error. A name that is no longer
 * recognised falls back to the documented default, which is visible.
 */

// Named rather than anonymous: see the note in FlowVizClipViewModel.cpp. Under a
// unity build these helpers share a translation unit with the sibling view
// models, and an anonymous namespace would collide with their same-named ones.
namespace FlowVizSessionLocal
{
	/* --- Key names, in one place ------------------------------------------ */

	const TCHAR* KeyFormat = TEXT("format");
	const TCHAR* KeyVersionMajor = TEXT("versionMajor");
	const TCHAR* KeyVersionMinor = TEXT("versionMinor");

	/* --- Enum names ------------------------------------------------------- */

	FString PlaybackModeToString(EFlowVizPlaybackMode Mode)
	{
		switch (Mode)
		{
			case EFlowVizPlaybackMode::RealTime: return TEXT("realTime");
			case EFlowVizPlaybackMode::FixedFps: return TEXT("fixedFps");
			case EFlowVizPlaybackMode::Sequence: return TEXT("sequence");
			default: return TEXT("sequence");
		}
	}

	EFlowVizPlaybackMode PlaybackModeFromString(const FString& Name)
	{
		if (Name == TEXT("realTime")) { return EFlowVizPlaybackMode::RealTime; }
		if (Name == TEXT("fixedFps")) { return EFlowVizPlaybackMode::FixedFps; }
		return EFlowVizPlaybackMode::Sequence;
	}

	FString LoopModeToString(EFlowVizLoopMode Mode)
	{
		switch (Mode)
		{
			case EFlowVizLoopMode::Once: return TEXT("once");
			case EFlowVizLoopMode::PingPong: return TEXT("pingPong");
			case EFlowVizLoopMode::Loop: return TEXT("loop");
			default: return TEXT("loop");
		}
	}

	EFlowVizLoopMode LoopModeFromString(const FString& Name)
	{
		if (Name == TEXT("once")) { return EFlowVizLoopMode::Once; }
		if (Name == TEXT("pingPong")) { return EFlowVizLoopMode::PingPong; }
		return EFlowVizLoopMode::Loop;
	}

	FString ComponentToString(EFlowVizComponentChoice Component)
	{
		// A SWITCH, NOT A CAST. A cast would keep compiling - and silently mean a
		// different component - if either enum were ever reordered.
		switch (Component)
		{
			case EFlowVizComponentChoice::X: return TEXT("x");
			case EFlowVizComponentChoice::Y: return TEXT("y");
			case EFlowVizComponentChoice::Z: return TEXT("z");
			case EFlowVizComponentChoice::W: return TEXT("w");
			case EFlowVizComponentChoice::Magnitude: return TEXT("magnitude");
			default: return TEXT("magnitude");
		}
	}

	EFlowVizComponentChoice ComponentFromString(const FString& Name)
	{
		if (Name == TEXT("x")) { return EFlowVizComponentChoice::X; }
		if (Name == TEXT("y")) { return EFlowVizComponentChoice::Y; }
		if (Name == TEXT("z")) { return EFlowVizComponentChoice::Z; }
		if (Name == TEXT("w")) { return EFlowVizComponentChoice::W; }
		return EFlowVizComponentChoice::Magnitude;
	}

	FString RangeSourceToString(EFlowVizRangeSource Source)
	{
		switch (Source)
		{
			case EFlowVizRangeSource::CurrentFrame: return TEXT("currentFrame");
			case EFlowVizRangeSource::Manual: return TEXT("manual");
			case EFlowVizRangeSource::Global: return TEXT("global");
			default: return TEXT("global");
		}
	}

	EFlowVizRangeSource RangeSourceFromString(const FString& Name)
	{
		if (Name == TEXT("currentFrame")) { return EFlowVizRangeSource::CurrentFrame; }
		if (Name == TEXT("manual")) { return EFlowVizRangeSource::Manual; }
		// GLOBAL IS THE FALLBACK, deliberately: rule 8 makes the animation-stable
		// range the default, so an unrecognised name lands on the honest one.
		return EFlowVizRangeSource::Global;
	}

	FString SlabOpToString(EFlowVizSlabOp Op)
	{
		switch (Op)
		{
			case EFlowVizSlabOp::Average: return TEXT("average");
			case EFlowVizSlabOp::Minimum: return TEXT("minimum");
			case EFlowVizSlabOp::Maximum: return TEXT("maximum");
			case EFlowVizSlabOp::None: return TEXT("none");
			default: return TEXT("none");
		}
	}

	EFlowVizSlabOp SlabOpFromString(const FString& Name)
	{
		if (Name == TEXT("average")) { return EFlowVizSlabOp::Average; }
		if (Name == TEXT("minimum")) { return EFlowVizSlabOp::Minimum; }
		if (Name == TEXT("maximum")) { return EFlowVizSlabOp::Maximum; }
		return EFlowVizSlabOp::None;
	}

	/* --- Vector helpers --------------------------------------------------- */

	TSharedRef<FJsonObject> MakeVector(const FVector& V)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		// Named components rather than an array, so a file is readable and a
		// transposed axis is visible in a diff.
		Object->SetNumberField(TEXT("x"), V.X);
		Object->SetNumberField(TEXT("y"), V.Y);
		Object->SetNumberField(TEXT("z"), V.Z);
		return Object;
	}

	bool TryReadVector(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key, FVector& OutVector)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Parent.IsValid() || !Parent->TryGetObjectField(Key, Object) || Object == nullptr)
		{
			return false;
		}
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!(*Object)->TryGetNumberField(TEXT("x"), X)
			|| !(*Object)->TryGetNumberField(TEXT("y"), Y)
			|| !(*Object)->TryGetNumberField(TEXT("z"), Z))
		{
			return false;
		}
		OutVector = FVector(X, Y, Z);
		return true;
	}

	/** Read a number into a double, leaving the target alone when the key is absent. */
	void ReadNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double& OutValue)
	{
		double Value = 0.0;
		if (Object.IsValid() && Object->TryGetNumberField(Key, Value))
		{
			OutValue = Value;
		}
	}

	void ReadFloat(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, float& OutValue)
	{
		double Value = 0.0;
		if (Object.IsValid() && Object->TryGetNumberField(Key, Value))
		{
			OutValue = static_cast<float>(Value);
		}
	}

	void ReadInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, int32& OutValue)
	{
		int32 Value = 0;
		if (Object.IsValid() && Object->TryGetNumberField(Key, Value))
		{
			OutValue = Value;
		}
	}

	void ReadBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, bool& OutValue)
	{
		bool Value = false;
		if (Object.IsValid() && Object->TryGetBoolField(Key, Value))
		{
			OutValue = Value;
		}
	}

	FString ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Key, Value);
		}
		return Value;
	}
}

using namespace FlowVizSessionLocal;

const TCHAR* FlowVizSession::GetFormatName()
{
	return TEXT("cfdviz-session");
}

const TCHAR* FlowVizSession::GetFileExtension()
{
	return TEXT("cfdvizsession");
}

/* ========================================================================== */
/* Save                                                                        */
/* ========================================================================== */

FCFDVizResult FlowVizSession::SaveToString(
	const FFlowVizSessionState& State,
	const FString& SessionDirectory,
	FString& OutJson)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(KeyFormat, GetFormatName());
	Root->SetNumberField(KeyVersionMajor, FormatVersionMajor);
	Root->SetNumberField(KeyVersionMinor, FormatVersionMinor);

	/* --- Case ------------------------------------------------------------- */

	/*
	 * RELATIVE WHEN POSSIBLE (plan.md section 14). An absolute path makes the
	 * session unopenable on any machine with a different layout - and unopenable
	 * in a way that presents as a MISSING CASE, so the user is asked to relink a
	 * file that is sitting right beside the session.
	 *
	 * MakePathRelativeTo fails when there is no relative route (a different
	 * volume, say), and the absolute path is then correct rather than a fallback.
	 */
	FString StoredCasePath = State.CasePath;
	if (!StoredCasePath.IsEmpty() && !SessionDirectory.IsEmpty())
	{
		FString Relative = FPaths::ConvertRelativePathToFull(StoredCasePath);
		FString Base = FPaths::ConvertRelativePathToFull(SessionDirectory);
		if (!Base.EndsWith(TEXT("/")))
		{
			Base += TEXT("/");
		}
		if (FPaths::MakePathRelativeTo(Relative, *Base))
		{
			StoredCasePath = Relative;
		}
	}
	Root->SetStringField(TEXT("case"), StoredCasePath);
	Root->SetStringField(TEXT("field"), State.FieldId.ToString());

	/* --- Playback --------------------------------------------------------- */

	Root->SetNumberField(TEXT("physicalTime"), State.PhysicalTime);
	{
		TSharedRef<FJsonObject> Playback = MakeShared<FJsonObject>();
		Playback->SetStringField(TEXT("mode"), PlaybackModeToString(State.Playback.Mode));
		Playback->SetStringField(TEXT("loop"), LoopModeToString(State.Playback.LoopMode));
		Playback->SetNumberField(TEXT("speed"), State.Playback.Speed);
		Playback->SetNumberField(TEXT("sequenceFrameRate"), State.Playback.SequenceFrameRate);
		Playback->SetNumberField(TEXT("outputFrameRate"), State.Playback.OutputFrameRate);
		Playback->SetBoolField(TEXT("interpolate"), State.Playback.bInterpolate);
		Playback->SetNumberField(TEXT("preloadAhead"), State.Playback.PreloadAhead);
		Playback->SetNumberField(TEXT("preloadBehind"), State.Playback.PreloadBehind);
		Root->SetObjectField(TEXT("playback"), Playback);
	}

	/* --- Colouring -------------------------------------------------------- */

	Root->SetStringField(TEXT("colorMap"), CFDViz::ColorMaps::GetName(State.ColorMap).ToString());
	Root->SetBoolField(TEXT("reverseColorMap"), State.bReverseColorMap);
	Root->SetNumberField(TEXT("colorBands"), State.ColorBands);
	Root->SetStringField(TEXT("component"), ComponentToString(State.Component));
	Root->SetStringField(TEXT("rangeSource"), RangeSourceToString(State.RangeSource));
	Root->SetNumberField(TEXT("rangeMin"), State.RangeMin);
	Root->SetNumberField(TEXT("rangeMax"), State.RangeMax);
	{
		TSharedRef<FJsonObject> Opacity = MakeShared<FJsonObject>();
		Opacity->SetNumberField(TEXT("multiplier"), State.Opacity.OpacityMultiplier);
		TArray<TSharedPtr<FJsonValue>> Points;
		for (const FFlowVizOpacityPoint& Point : State.Opacity.Points)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("position"), Point.Position);
			Entry->SetNumberField(TEXT("opacity"), Point.Opacity);
			Points.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Opacity->SetArrayField(TEXT("points"), Points);
		Root->SetObjectField(TEXT("opacity"), Opacity);
	}

	/* --- Clipping --------------------------------------------------------- */

	{
		TArray<TSharedPtr<FJsonValue>> Planes;
		for (const FFlowVizClipPlane& Plane : State.ClipPlanes)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetObjectField(TEXT("normal"), MakeVector(Plane.Normal));
			Entry->SetNumberField(TEXT("distance"), Plane.Distance);
			Entry->SetStringField(TEXT("label"), Plane.Label);
			// A DISABLED PLANE IS PERSISTED. Writing only enabled planes would
			// silently delete every hidden one on the next save.
			Entry->SetBoolField(TEXT("enabled"), Plane.bEnabled);
			Planes.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Root->SetArrayField(TEXT("clipPlanes"), Planes);
	}
	{
		TSharedRef<FJsonObject> Crop = MakeShared<FJsonObject>();
		Crop->SetBoolField(TEXT("active"), State.bHasCropBox);
		Crop->SetObjectField(TEXT("min"), MakeVector(State.CropMin));
		Crop->SetObjectField(TEXT("max"), MakeVector(State.CropMax));
		Root->SetObjectField(TEXT("cropBox"), Crop);
	}

	/* --- Slice ------------------------------------------------------------ */

	{
		TSharedRef<FJsonObject> Slice = MakeShared<FJsonObject>();
		Slice->SetBoolField(TEXT("active"), State.bHasSlice);
		Slice->SetObjectField(TEXT("origin"), MakeVector(State.SliceOrigin));
		Slice->SetObjectField(TEXT("normal"), MakeVector(State.SliceNormal));
		Slice->SetNumberField(TEXT("thickness"), State.SliceThickness);
		Slice->SetNumberField(TEXT("slabSamples"), State.SliceSlabSamples);
		Slice->SetStringField(TEXT("slabOp"), SlabOpToString(State.SliceSlabOp));
		Slice->SetBoolField(TEXT("visible"), State.bSliceVisible);
		Root->SetObjectField(TEXT("slice"), Slice);
	}

	/* --- Probes ----------------------------------------------------------- */

	{
		TArray<TSharedPtr<FJsonValue>> Probes;
		for (const FFlowVizProbe& Probe : State.Probes)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			// THE ID IS PERSISTED, NOT REGENERATED. A chart series is keyed by it,
			// so a fresh GUID on load would orphan every saved reference.
			Entry->SetStringField(TEXT("id"), Probe.Id.ToString(EGuidFormats::DigitsWithHyphens));
			Entry->SetStringField(TEXT("name"), Probe.Name);
			// SOLVER UNITS. Writing centimetres would make a session opened against
			// a differently scaled case probe a different cell.
			Entry->SetObjectField(TEXT("solverPosition"), MakeVector(Probe.SolverPosition));
			Entry->SetBoolField(TEXT("visible"), Probe.bVisible);
			// The last reading is NOT persisted: it is a measurement of a frame,
			// derivable by re-sampling, and a stale one restored beside a live
			// case would be a real number attributed to the wrong data.
			Probes.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Root->SetArrayField(TEXT("probes"), Probes);
	}
	{
		TSharedRef<FJsonObject> Line = MakeShared<FJsonObject>();
		Line->SetBoolField(TEXT("active"), State.bHasLineProbe);
		Line->SetObjectField(TEXT("start"), MakeVector(State.LineStart));
		Line->SetObjectField(TEXT("end"), MakeVector(State.LineEnd));
		Line->SetNumberField(TEXT("samples"), State.LineSamples);
		Root->SetObjectField(TEXT("lineProbe"), Line);
	}

	/* --- Workspace -------------------------------------------------------- */

	Root->SetBoolField(TEXT("presentationMode"), State.bPresentationMode);
	{
		TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
		Camera->SetBoolField(TEXT("active"), State.bHasCamera);
		Camera->SetObjectField(TEXT("location"), MakeVector(State.CameraLocation));
		TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
		Rotation->SetNumberField(TEXT("pitch"), State.CameraRotation.Pitch);
		Rotation->SetNumberField(TEXT("yaw"), State.CameraRotation.Yaw);
		Rotation->SetNumberField(TEXT("roll"), State.CameraRotation.Roll);
		Camera->SetObjectField(TEXT("rotation"), Rotation);
		Root->SetObjectField(TEXT("camera"), Camera);
	}
	{
		TSharedRef<FJsonObject> Panels = MakeShared<FJsonObject>();
		for (const TPair<FString, bool>& Panel : State.PanelVisibility)
		{
			// A HIDDEN PANEL IS WRITTEN AS false, not omitted: omitting it would
			// make it reappear on reload, which is the opposite of what was saved.
			Panels->SetBoolField(Panel.Key, Panel.Value);
		}
		Root->SetObjectField(TEXT("panelVisibility"), Panels);
	}
	{
		TArray<TSharedPtr<FJsonValue>> Annotations;
		for (const FString& Annotation : State.Annotations)
		{
			// Verbatim, in order - the order they are displayed in.
			Annotations.Add(MakeShared<FJsonValueString>(Annotation));
		}
		Root->SetArrayField(TEXT("annotations"), Annotations);
	}

	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		// OutJson is left untouched, per the header: a caller that ignores the
		// result must not write a truncated session over a good one.
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest, TEXT("failed to serialise the session"));
	}

	OutJson = MoveTemp(Serialized);
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Load                                                                        */
/* ========================================================================== */

FCFDVizResult FlowVizSession::LoadFromString(
	const FString& Json,
	const FString& SessionDirectory,
	FFlowVizSessionState& OutState)
{
	// Built into a local and only moved out on success, so a caller that ignores
	// the result cannot end up with a half-restored scene.
	FFlowVizSessionState Parsed;

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(TEXT("session is not valid JSON: %s"), *Reader->GetErrorMessage()));
	}

	const FString Format = ReadString(Root, KeyFormat);
	if (Format != GetFormatName())
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(
				TEXT("not a %s file: format is '%s'"), GetFormatName(), *Format));
	}

	int32 Major = FormatVersionMajor;
	ReadInt(Root, KeyVersionMajor, Major);
	if (Major > FormatVersionMajor)
	{
		/*
		 * A NEWER MAJOR IS REFUSED. A major bump means a key CHANGED MEANING, so
		 * reading it anyway restores a scene that is plausible and wrong - worse
		 * than refusing, because nothing looks broken and the user trusts it.
		 * A newer MINOR is accepted: its unknown keys are simply ignored below.
		 */
		return FCFDVizResult::Fail(
			ECFDVizError::UnsupportedVersion,
			FString::Printf(
				TEXT("session format version %d is newer than the supported %d; a major "
					 "version change means a key changed meaning, so it is not read"),
				Major, FormatVersionMajor));
	}

	/* --- Case ------------------------------------------------------------- */

	Parsed.CasePath = ReadString(Root, TEXT("case"));
	Parsed.FieldId = FName(*ReadString(Root, TEXT("field")));

	if (!Parsed.CasePath.IsEmpty())
	{
		if (FPaths::IsRelative(Parsed.CasePath) && !SessionDirectory.IsEmpty())
		{
			Parsed.ResolvedCasePath = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(SessionDirectory, Parsed.CasePath));
		}
		else
		{
			Parsed.ResolvedCasePath = FPaths::ConvertRelativePathToFull(Parsed.CasePath);
		}
		/*
		 * A MISSING CASE IS NOT A FAILURE (plan.md section 14: "Loading a session
		 * with a missing case must allow the user to relink"). The flag is what a
		 * relink prompt is keyed on, and ResolvedCasePath names what was looked
		 * for so the prompt can say so. Everything below still loads.
		 */
		Parsed.bCaseFound = FPaths::FileExists(Parsed.ResolvedCasePath);
	}

	/* --- Playback --------------------------------------------------------- */

	ReadNumber(Root, TEXT("physicalTime"), Parsed.PhysicalTime);
	{
		const TSharedPtr<FJsonObject>* Playback = nullptr;
		if (Root->TryGetObjectField(TEXT("playback"), Playback) && Playback != nullptr)
		{
			Parsed.Playback.Mode = PlaybackModeFromString(ReadString(*Playback, TEXT("mode")));
			Parsed.Playback.LoopMode = LoopModeFromString(ReadString(*Playback, TEXT("loop")));
			ReadNumber(*Playback, TEXT("speed"), Parsed.Playback.Speed);
			ReadNumber(*Playback, TEXT("sequenceFrameRate"), Parsed.Playback.SequenceFrameRate);
			ReadNumber(*Playback, TEXT("outputFrameRate"), Parsed.Playback.OutputFrameRate);
			ReadBool(*Playback, TEXT("interpolate"), Parsed.Playback.bInterpolate);
			ReadInt(*Playback, TEXT("preloadAhead"), Parsed.Playback.PreloadAhead);
			ReadInt(*Playback, TEXT("preloadBehind"), Parsed.Playback.PreloadBehind);
		}
	}

	/* --- Colouring -------------------------------------------------------- */

	{
		const FString ColorMapName = ReadString(Root, TEXT("colorMap"));
		ECFDVizColorMap ColorMap = CFDViz::ColorMaps::Default;
		// An unrecognised name falls back to the documented default rather than
		// failing the load: a session written by a build with an extra colormap
		// should still open, showing a map the user can see is not theirs.
		CFDViz::ColorMaps::TryParse(FName(*ColorMapName), ColorMap);
		Parsed.ColorMap = ColorMap;
	}
	ReadBool(Root, TEXT("reverseColorMap"), Parsed.bReverseColorMap);
	ReadInt(Root, TEXT("colorBands"), Parsed.ColorBands);
	Parsed.Component = ComponentFromString(ReadString(Root, TEXT("component")));
	Parsed.RangeSource = RangeSourceFromString(ReadString(Root, TEXT("rangeSource")));
	ReadFloat(Root, TEXT("rangeMin"), Parsed.RangeMin);
	ReadFloat(Root, TEXT("rangeMax"), Parsed.RangeMax);
	{
		const TSharedPtr<FJsonObject>* Opacity = nullptr;
		if (Root->TryGetObjectField(TEXT("opacity"), Opacity) && Opacity != nullptr)
		{
			ReadFloat(*Opacity, TEXT("multiplier"), Parsed.Opacity.OpacityMultiplier);
			const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
			if ((*Opacity)->TryGetArrayField(TEXT("points"), Points) && Points != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Points)
				{
					const TSharedPtr<FJsonObject>* Entry = nullptr;
					if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
					{
						continue;
					}
					FFlowVizOpacityPoint Point;
					ReadFloat(*Entry, TEXT("position"), Point.Position);
					ReadFloat(*Entry, TEXT("opacity"), Point.Opacity);
					Parsed.Opacity.Points.Add(Point);
				}
			}
		}
	}

	/* --- Clipping --------------------------------------------------------- */

	{
		const TArray<TSharedPtr<FJsonValue>>* Planes = nullptr;
		if (Root->TryGetArrayField(TEXT("clipPlanes"), Planes) && Planes != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Planes)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
				{
					continue;
				}
				FFlowVizClipPlane Plane;
				if (!TryReadVector(*Entry, TEXT("normal"), Plane.Normal))
				{
					// A plane with no normal is not a plane. Skipped rather than
					// restored as a default that would clip an arbitrary half.
					continue;
				}
				ReadNumber(*Entry, TEXT("distance"), Plane.Distance);
				Plane.Label = ReadString(*Entry, TEXT("label"));
				// Defaults to enabled when absent, matching the struct.
				Plane.bEnabled = true;
				ReadBool(*Entry, TEXT("enabled"), Plane.bEnabled);
				Parsed.ClipPlanes.Add(Plane);
			}
		}
	}
	{
		const TSharedPtr<FJsonObject>* Crop = nullptr;
		if (Root->TryGetObjectField(TEXT("cropBox"), Crop) && Crop != nullptr)
		{
			ReadBool(*Crop, TEXT("active"), Parsed.bHasCropBox);
			TryReadVector(*Crop, TEXT("min"), Parsed.CropMin);
			TryReadVector(*Crop, TEXT("max"), Parsed.CropMax);
		}
	}

	/* --- Slice ------------------------------------------------------------ */

	{
		const TSharedPtr<FJsonObject>* Slice = nullptr;
		if (Root->TryGetObjectField(TEXT("slice"), Slice) && Slice != nullptr)
		{
			ReadBool(*Slice, TEXT("active"), Parsed.bHasSlice);
			TryReadVector(*Slice, TEXT("origin"), Parsed.SliceOrigin);
			TryReadVector(*Slice, TEXT("normal"), Parsed.SliceNormal);
			ReadNumber(*Slice, TEXT("thickness"), Parsed.SliceThickness);
			ReadInt(*Slice, TEXT("slabSamples"), Parsed.SliceSlabSamples);
			Parsed.SliceSlabOp = SlabOpFromString(ReadString(*Slice, TEXT("slabOp")));
			ReadBool(*Slice, TEXT("visible"), Parsed.bSliceVisible);
		}
	}

	/* --- Probes ----------------------------------------------------------- */

	{
		const TArray<TSharedPtr<FJsonValue>>* Probes = nullptr;
		if (Root->TryGetArrayField(TEXT("probes"), Probes) && Probes != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Probes)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
				{
					continue;
				}
				FFlowVizProbe Probe;
				const FString IdText = ReadString(*Entry, TEXT("id"));
				if (!FGuid::Parse(IdText, Probe.Id))
				{
					// A NEW ID, NOT A REFUSAL: an unparseable id costs the probe's
					// chart-series linkage, which is worth less than the probe.
					Probe.Id = FGuid::NewGuid();
				}
				Probe.Name = ReadString(*Entry, TEXT("name"));
				if (!TryReadVector(*Entry, TEXT("solverPosition"), Probe.SolverPosition))
				{
					// A probe with no position samples nothing.
					continue;
				}
				Probe.bVisible = true;
				ReadBool(*Entry, TEXT("visible"), Probe.bVisible);
				Parsed.Probes.Add(Probe);
			}
		}
	}
	{
		const TSharedPtr<FJsonObject>* Line = nullptr;
		if (Root->TryGetObjectField(TEXT("lineProbe"), Line) && Line != nullptr)
		{
			ReadBool(*Line, TEXT("active"), Parsed.bHasLineProbe);
			TryReadVector(*Line, TEXT("start"), Parsed.LineStart);
			TryReadVector(*Line, TEXT("end"), Parsed.LineEnd);
			ReadInt(*Line, TEXT("samples"), Parsed.LineSamples);
		}
	}

	/* --- Workspace -------------------------------------------------------- */

	ReadBool(Root, TEXT("presentationMode"), Parsed.bPresentationMode);
	{
		const TSharedPtr<FJsonObject>* Camera = nullptr;
		if (Root->TryGetObjectField(TEXT("camera"), Camera) && Camera != nullptr)
		{
			ReadBool(*Camera, TEXT("active"), Parsed.bHasCamera);
			TryReadVector(*Camera, TEXT("location"), Parsed.CameraLocation);
			const TSharedPtr<FJsonObject>* Rotation = nullptr;
			if ((*Camera)->TryGetObjectField(TEXT("rotation"), Rotation) && Rotation != nullptr)
			{
				double Pitch = 0.0;
				double Yaw = 0.0;
				double Roll = 0.0;
				(*Rotation)->TryGetNumberField(TEXT("pitch"), Pitch);
				(*Rotation)->TryGetNumberField(TEXT("yaw"), Yaw);
				(*Rotation)->TryGetNumberField(TEXT("roll"), Roll);
				Parsed.CameraRotation = FRotator(Pitch, Yaw, Roll);
			}
		}
	}
	{
		const TSharedPtr<FJsonObject>* Panels = nullptr;
		if (Root->TryGetObjectField(TEXT("panelVisibility"), Panels) && Panels != nullptr)
		{
			/*
			 * `auto`, NOT TPair<FString, ...>.
			 *
			 * FJsonObject::Values is keyed by FJsonObject::FStringType, which is
			 * UE::FSharedString in UE 5.8 - FString only under
			 * UE_JSONOBJECT_LEGACY_STRING_KEYS, which JsonObject.h defines to 0
			 * and documents as slated for removal.
			 *
			 * NAMING FString HERE STILL COMPILES, which is exactly why it is worth
			 * a comment: FString is CONSTRUCTIBLE from the key type, so the loop
			 * variable would not bind to the map's element at all. It would bind to
			 * a temporary TPair materialised - with a fresh string allocation - on
			 * every iteration, silently, with no diagnostic. `auto` binds to the
			 * element itself. (Verified by compiling both spellings against this
			 * module's real flags.)
			 */
			for (const auto& Panel : (*Panels)->Values)
			{
				bool bVisible = false;
				if (Panel.Value.IsValid() && Panel.Value->TryGetBool(bVisible))
				{
					Parsed.PanelVisibility.Add(FString(Panel.Key), bVisible);
				}
			}
		}
	}
	{
		const TArray<TSharedPtr<FJsonValue>>* Annotations = nullptr;
		if (Root->TryGetArrayField(TEXT("annotations"), Annotations) && Annotations != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Annotations)
			{
				FString Text;
				if (Value.IsValid() && Value->TryGetString(Text))
				{
					Parsed.Annotations.Add(Text);
				}
			}
		}
	}

	OutState = MoveTemp(Parsed);
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Files                                                                       */
/* ========================================================================== */

FCFDVizResult FlowVizSession::SaveToFile(const FFlowVizSessionState& State, const FString& FilePath)
{
	FString Json;
	// The directory used for relative-path resolution is the file's OWN, so a
	// session saved beside its case stores a relative path without the caller
	// having to say so.
	const FCFDVizResult Saved = SaveToString(State, FPaths::GetPath(FilePath), Json);
	if (!Saved.IsOk())
	{
		return Saved;
	}

	if (!FFileHelper::SaveStringToFile(Json, *FilePath))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::FileReadFailed,
			TEXT("failed to write the session file"), FilePath);
	}
	return FCFDVizResult::Ok();
}

FCFDVizResult FlowVizSession::LoadFromFile(const FString& FilePath, FFlowVizSessionState& OutState)
{
	if (!FPaths::FileExists(FilePath))
	{
		/*
		 * A MISSING SESSION IS A DIFFERENT FAILURE FROM A MISSING CASE, and they
		 * get different treatment: this is an error the user must act on, while a
		 * missing case is a relink prompt with the rest of the scene intact.
		 * Reporting FileNotFound rather than a parse error is what lets a caller
		 * tell them apart.
		 */
		return FCFDVizResult::Fail(
			ECFDVizError::FileNotFound, TEXT("session file does not exist"), FilePath);
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *FilePath))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::FileReadFailed, TEXT("failed to read the session file"), FilePath);
	}

	return LoadFromString(Json, FPaths::GetPath(FilePath), OutState);
}

FCFDVizResult FlowVizSession::RelinkCase(FFlowVizSessionState& State, const FString& NewManifestPath)
{
	if (!FPaths::FileExists(NewManifestPath))
	{
		// STATE IS UNTOUCHED, so a mistaken relink does not lose the original
		// path and the user can simply try again.
		return FCFDVizResult::Fail(
			ECFDVizError::FileNotFound,
			TEXT("cannot relink: the chosen manifest does not exist"), NewManifestPath);
	}

	State.CasePath = NewManifestPath;
	State.ResolvedCasePath = FPaths::ConvertRelativePathToFull(NewManifestPath);
	State.bCaseFound = true;
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Capture and apply                                                           */
/* ========================================================================== */

void FlowVizSession::CaptureFromViewModels(
	const FFlowVizCasePlayer* Player,
	const FFlowVizTransferFunctionViewModel* TransferFunction,
	const FFlowVizClipViewModel* Clip,
	const FFlowVizSliceViewModel* Slice,
	const FFlowVizProbeViewModel* Probes,
	FFlowVizSessionState& OutState)
{
	/*
	 * EVERY ARGUMENT IS OPTIONAL, AND A NULL ONE CONTRIBUTES NOTHING. A workspace
	 * with no slice must not get a default slice materialising out of nowhere on
	 * the next reload - which is why the bHas* flags are only ever set from a
	 * live view model, never defaulted true.
	 */
	if (Player != nullptr)
	{
		OutState.PhysicalTime = Player->GetPhysicalTime();
		OutState.Playback = Player->GetSettings();
		if (Player->IsOpen())
		{
			OutState.FieldId = Player->GetFieldId();
			const FCFDVizCase* Case = Player->GetCase();
			if (Case != nullptr)
			{
				OutState.CasePath = Case->ManifestPath;
			}
		}
	}

	if (TransferFunction != nullptr)
	{
		OutState.ColorMap = TransferFunction->GetColorMap();
		OutState.bReverseColorMap = TransferFunction->IsColorMapReversed();
		OutState.ColorBands = TransferFunction->GetColorBands();
		OutState.Component = TransferFunction->GetComponent();
		OutState.RangeSource = TransferFunction->GetRangeSource();
		OutState.RangeMin = TransferFunction->GetRangeMin();
		OutState.RangeMax = TransferFunction->GetRangeMax();
		OutState.Opacity = TransferFunction->GetOpacityCurve();
		// The view model keeps the multiplier outside the curve (so a slider tick
		// does not rebuild the LUT); the session stores one opacity object, so it
		// is folded in here and unfolded on apply.
		OutState.Opacity.OpacityMultiplier = TransferFunction->GetOpacityMultiplier();
		if (TransferFunction->IsBound())
		{
			OutState.FieldId = TransferFunction->GetFieldId();
		}
	}

	if (Clip != nullptr)
	{
		OutState.ClipPlanes = Clip->GetPlanes();
		OutState.CropMin = Clip->GetCropMin();
		OutState.CropMax = Clip->GetCropMax();
		OutState.bHasCropBox = Clip->IsCropActive();
	}

	if (Slice != nullptr)
	{
		OutState.bHasSlice = true;
		OutState.SliceOrigin = Slice->GetOrigin();
		OutState.SliceNormal = Slice->GetNormal();
		OutState.SliceThickness = Slice->GetThickness();
		OutState.SliceSlabSamples = Slice->GetSlabSamples();
		OutState.SliceSlabOp = Slice->GetSlabOp();
		OutState.bSliceVisible = Slice->IsVisible();
	}

	if (Probes != nullptr)
	{
		OutState.Probes = Probes->GetProbes();
		OutState.bHasLineProbe = Probes->HasLineProbe();
		OutState.LineStart = Probes->GetLineStart();
		OutState.LineEnd = Probes->GetLineEnd();
		OutState.LineSamples = Probes->GetLineSampleCount();
	}
}

FCFDVizResult FlowVizSession::ApplyToViewModels(
	const FFlowVizSessionState& State,
	FFlowVizCasePlayer* Player,
	FFlowVizTransferFunctionViewModel* TransferFunction,
	FFlowVizClipViewModel* Clip,
	FFlowVizSliceViewModel* Slice,
	FFlowVizProbeViewModel* Probes)
{
	/*
	 * BEST EFFORT, WITH THE FIRST FAILURE REPORTED.
	 *
	 * A session whose clip planes are legal but whose playback speed is not
	 * restores the planes and reports the speed. Discarding a whole scene over
	 * one bad number is what makes a session file fragile - the user loses an
	 * afternoon's setup because one field was out of range. Reporting nothing
	 * would be worse in the other direction: a corrupt session would present as a
	 * scene that is almost right.
	 */
	FCFDVizResult FirstFailure = FCFDVizResult::Ok();
	auto Record = [&FirstFailure](const FCFDVizResult& Result)
	{
		if (!Result.IsOk() && FirstFailure.IsOk())
		{
			FirstFailure = Result;
		}
	};

	if (Player != nullptr)
	{
		Record(Player->SetSettings(State.Playback));
		if (Player->IsOpen())
		{
			Player->SeekToTime(State.PhysicalTime);
		}
	}

	if (TransferFunction != nullptr)
	{
		Record(TransferFunction->SetColorMap(State.ColorMap));
		TransferFunction->SetReverseColorMap(State.bReverseColorMap);
		Record(TransferFunction->SetColorBands(State.ColorBands));
		Record(TransferFunction->SetComponent(State.Component));

		FFlowVizOpacityCurve Curve = State.Opacity;
		const float Multiplier = Curve.OpacityMultiplier;
		// The view model owns the multiplier separately, so it is unfolded back
		// out of the curve rather than being applied twice.
		Curve.OpacityMultiplier = 1.0f;
		Record(TransferFunction->SetOpacityCurve(Curve));
		Record(TransferFunction->SetOpacityMultiplier(Multiplier));

		/*
		 * THE RANGE BEFORE THE SOURCE, DELIBERATELY. SetRangeSource(Manual) is
		 * what makes the typed values authoritative, and setting the source first
		 * would let a Global re-derivation overwrite them. Manual is also the only
		 * source whose numbers live in the session at all - Global and
		 * CurrentFrame are re-derived from the data on bind.
		 */
		if (State.RangeSource == EFlowVizRangeSource::Manual)
		{
			Record(TransferFunction->SetManualRange(State.RangeMin, State.RangeMax));
		}
		else
		{
			Record(TransferFunction->SetRangeSource(State.RangeSource));
		}
	}

	if (Clip != nullptr)
	{
		Clip->RemoveAllPlanes();
		for (const FFlowVizClipPlane& Plane : State.ClipPlanes)
		{
			/*
			 * THE ADD IS CHECKED BEFORE THE INDEX IS USED.
			 *
			 * AddPlane REFUSES a seventh plane and a degenerate normal. On a
			 * refusal the list did not grow, so `GetPlaneCount() - 1` still names
			 * the PREVIOUS plane - and a refused-but-disabled plane would switch
			 * off a legitimate neighbour that the user can see. That fault is
			 * invisible in the result code, because the disable would SUCCEED; the
			 * only symptom is a plane that quietly stops clipping on reload.
			 */
			const FCFDVizResult Added = Clip->AddPlane(Plane);
			if (!Added.IsOk())
			{
				Record(Added);
				continue;
			}
			if (!Plane.bEnabled)
			{
				// AddPlane admits an enabled plane; the disabled state is applied
				// afterwards so a hidden plane comes back hidden rather than
				// suddenly clipping the scene on reload. Safe now: the add above
				// succeeded, so the last index is this plane's.
				Record(Clip->SetPlaneEnabled(Clip->GetPlaneCount() - 1, false));
			}
		}
		if (State.bHasCropBox)
		{
			Record(Clip->SetCropBox(State.CropMin, State.CropMax));
		}
		else
		{
			Clip->ResetCropBox();
		}
	}

	if (Slice != nullptr && State.bHasSlice)
	{
		Record(Slice->SetNormal(State.SliceNormal));
		Record(Slice->SetOrigin(State.SliceOrigin));
		// THICKNESS BEFORE THE SLAB CONTROLS. Both SetSlabSamples and SetSlabOp
		// are refused on a zero-thickness slice, so setting them first would
		// report two spurious failures and silently drop the slab configuration.
		Record(Slice->SetThickness(State.SliceThickness));
		if (State.SliceThickness > 0.0)
		{
			Record(Slice->SetSlabOp(State.SliceSlabOp));
			Record(Slice->SetSlabSamples(State.SliceSlabSamples));
		}
		Slice->SetVisible(State.bSliceVisible);
	}

	if (Probes != nullptr)
	{
		Probes->RemoveAllProbes();
		for (const FFlowVizProbe& Probe : State.Probes)
		{
			/*
			 * RESTORED UNDER THE SAVED ID, IN ONE OPERATION.
			 *
			 * Adding and then re-identifying would be two ids for one probe, and
			 * every id-keyed call after it - the visibility line below included -
			 * would have to guess which one is live. Getting that wrong has no
			 * error path: SetProbeVisible simply returns false for an unknown id,
			 * so the probes come back at the right positions and INVISIBLE, with
			 * nothing logged.
			 */
			const FCFDVizResult Restored =
				Probes->RestoreProbe(Probe.Id, Probe.SolverPosition, Probe.Name);
			if (!Restored.IsOk())
			{
				Record(Restored);
				continue;
			}
			// Keyed off the SAVED id, which RestoreProbe has just guaranteed is the
			// one in the list.
			Probes->SetProbeVisible(Probe.Id, Probe.bVisible);
		}

		if (State.bHasLineProbe)
		{
			Record(Probes->SetLineProbe(State.LineStart, State.LineEnd));
			Record(Probes->SetLineSampleCount(State.LineSamples));
		}
	}

	return FirstFailure;
}
