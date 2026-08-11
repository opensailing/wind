// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizTypes.h"

// CoreMinimal.h does not guarantee either of these, and both appear by value in
// the structs below: TOptional is how "the manifest did not say" is kept
// distinguishable from "the manifest said the default", and FDateTime is the
// parsed form of case.createdUtc.
#include "Misc/DateTime.h"
#include "Misc/Optional.h"

/**
 * manifest.json - the CFDViz 1.1 case description (format section 3).
 *
 * The manifest is the only file that says what a case contains. Everything else
 * - which .cvf holds which field at which frame, what a value means physically,
 * which cells are masked - is derived from it, so a parser that guesses at a
 * missing property produces a case that renders plausibly and is wrong.
 * Accordingly: a missing REQUIRED property is an error naming that property, and
 * every OPTIONAL property has a default documented at its declaration rather
 * than invented at a call site.
 *
 * NO JSON TYPE APPEARS IN THIS HEADER. Json and JsonUtilities are PRIVATE
 * dependencies of FlowVizRuntime, so a public header naming FJsonObject would
 * not compile for any module that depends on this one. All JSON handling lives
 * in CFDVizManifest.cpp.
 *
 * NO UOBJECTS. These are plain structs held by value. Manifest parsing is file
 * I/O and must therefore be callable from a worker thread (engineering rule 1),
 * which rules out UObject allocation, UWorld access and any hop to the game
 * thread inside the read path.
 *
 * SOLVER UNITS THROUGHOUT. Origin, spacing, times and statistics are in the
 * units the manifest declares, in the canonical right-handed Z-up frame. Nothing
 * here is converted to Unreal centimetres; that happens in exactly one
 * visualisation adapter (format section 2, ADR 004).
 *
 * VERSION POLICY (format rule 1.4), implemented in ParseFromString:
 *   - major != 1                 -> rejected, naming the version found.
 *   - newer minor                -> ACCEPTED as long as every required
 *                                   construct understood by this 1.1 reader
 *                                   is present.
 *   - unknown object properties  -> ignored silently, at every level.
 *   - unknown value of a REQUIRED enum (dataType, association, codec, grid type,
 *     storage type, handedness, upAxis, forwardAxis) -> rejected, never guessed.
 */

/* -------------------------------------------------------------------------- */
/* Manifest-only enumerations                                                   */
/*                                                                              */
/* Unlike the enums in CFDVizTypes.h these are not on-disk byte values - they    */
/* are closed JSON enums, so their numeric values are implementation detail and  */
/* only the spellings in the .cpp are normative.                                 */
/* -------------------------------------------------------------------------- */

/** `grid.type`. CFDViz 1.x defines exactly one grid type; anything else is rejected, not ignored. */
enum class ECFDVizGridType : uint8
{
	/** Uniform Cartesian: constant spacing per axis, axis-aligned, described entirely by dimensions/origin/spacing. */
	UniformCartesian = 0
};

/** `field.storage.type`. CFDViz 1.x stores volume fields as CVF bricked volumes only. */
enum class ECFDVizStorageType : uint8
{
	BrickedVolume = 0
};

/** `timeline.defaultInterpolation` and `field.temporalInterpolation`. */
enum class ECFDVizInterpolation : uint8
{
	/** Show the nearest stored frame. Only ever displays data that exists on disk. */
	Nearest = 0,
	/** Blend the two bracketing frames. The UI must mark such a frame as interpolated. */
	Linear = 1
};

/** `field.display.defaultRangeMode` - where a colour range comes from before the user touches it. */
enum class ECFDVizRangeMode : uint8
{
	/** field.statistics global range: stable across the whole animation, so structures do not pulse as the range rescales. */
	Global = 0,
	/** The current frame's own range: maximises contrast per frame, at the cost of frame-to-frame comparability. */
	Frame = 1,
	/** field.display.recommendedRange. */
	Manual = 2
};

/** `field.phase.representation` - how an interface scalar is interpreted. */
enum class ECFDVizPhaseRepresentation : uint8
{
	/** A bounded phase fraction, conventionally in [0, 1]. */
	VolumeFraction = 0,
	/** A signed distance or level-set field. */
	SignedDistance = 1
};

/** `field.phase.inside` - which side of the interface is the primary phase. */
enum class ECFDVizPhaseInside : uint8
{
	/** Values greater than `interfaceValue` belong to the primary phase. */
	GreaterThanInterface = 0,
	/** Values less than `interfaceValue` belong to the primary phase. */
	LessThanInterface = 1
};

/* -------------------------------------------------------------------------- */
/* Leaf structures                                                              */
/* -------------------------------------------------------------------------- */

/**
 * `case.solver` - which code produced the data. Provenance only; never affects decoding.
 */
struct FCFDVizSolverInfo
{
	FString Name;
	FString Version;

	/** Numerical method, e.g. "finite-volume", "LBM D3Q19". Free-form. */
	FString Method;

	/** Source revision of the solver executable or source tree. Empty when unavailable. */
	FString Commit;

	/** Human-readable run/model configuration, e.g. "transient-LES". */
	FString Configuration;
};

/**
 * `case` - identity of the dataset (required: `id`, `name`).
 */
struct FCFDVizCaseMetadata
{
	/** Stable case identifier, usually a UUID. known_values.json echoes this as `caseId`, so the bridge test can prove it read the case it meant to. */
	FString Id;

	/** Human-readable title. Required and non-empty. */
	FString Name;

	FString Description;

	/**
	 * `createdUtc` as written, an ISO 8601 UTC string.
	 *
	 * Kept as text rather than an FDateTime because ISO 8601 admits spellings
	 * FDateTime::ParseIso8601 does not accept, and losing the string would turn a
	 * cosmetic parse gap into a case that will not load. Use TryGetCreatedUtc
	 * when a real timestamp is needed.
	 */
	FString CreatedUtcText;

	/**
	 * Data-quality classification surfaced in the UI, e.g. "visualization-demo".
	 *
	 * This is how a viewer tells a user that synthetic demonstration data is not
	 * validation-grade CFD. Free-form, so an unrecognised value is displayed
	 * rather than rejected - but it must be displayed, not dropped.
	 */
	FString Quality;

	FCFDVizSolverInfo Solver;

	TArray<FString> Tags;

	/** @return false, leaving OutDateTime untouched, when createdUtc is absent or is not a spelling FDateTime understands. */
	FLOWVIZRUNTIME_API bool TryGetCreatedUtc(FDateTime& OutDateTime) const;
};

/**
 * `units` - the units every stored value is in (required: `length`, `time`).
 *
 * Format rule 1.6: values are stored in these units and are never normalized,
 * quantized, clamped or rescaled by a reader or writer. These strings are what
 * makes a number physically meaningful; a viewer that shows "2.4" without them
 * is showing a number, not a measurement.
 */
struct FCFDVizUnits
{
	/** Length unit of origin, spacing and mesh positions. SI metres ("m") preferred. Required. */
	FString Length;

	/** Time unit of timeline.times. Required. */
	FString Time;

	FString Mass;
	FString Temperature;
	FString Angle;

	/**
	 * Metres per unit of `Length`, for the visualisation adapter's length scale.
	 *
	 * @return false - leaving OutMetersPerUnit untouched - for any spelling this
	 *         table does not contain. That is deliberate: MakeSolverToUnrealTransform
	 *         needs a real scale, and silently assuming metres for an unrecognised
	 *         unit would place a millimetre-scale case a thousand times too large
	 *         with nothing on screen to say so. A false return means "ask, or
	 *         refuse", not "assume 1".
	 */
	FLOWVIZRUNTIME_API bool TryGetLengthInMeters(double& OutMetersPerUnit) const;
};

/**
 * `coordinates` - an assertion that the case really is in the canonical frame.
 */
struct FCFDVizCoordinates
{
	/**
	 * Parsed from `handedness`/`upAxis`/`forwardAxis`, which are closed enums
	 * admitting only "right"/"Z"/"X". Any other value is rejected at parse time,
	 * so this is always canonical in a successfully loaded case - the field
	 * exists so a reader can *assert* that rather than assume it.
	 */
	FCFDVizCoordinateSystem System;

	/**
	 * `coordinates.origin`. Provenance: where the canonical frame sits relative to
	 * the source dataset.
	 *
	 * CFDViz 1.x gives this NO decoding meaning. `grid.origin` alone positions
	 * cells; adding this to a grid position double-offsets the entire volume,
	 * which looks like a plausible translation and is wrong. Defaults to zero.
	 */
	FVector Origin = FVector::ZeroVector;

	/**
	 * `sourceToCanonical`, already transposed into Unreal's row-vector convention
	 * by TryMakeMatrixFromRowMajorArray. Unset when absent or JSON null, which
	 * both mean identity.
	 */
	TOptional<FMatrix> SourceToCanonical;

	/** `crs` when given as a string - reserved georeferencing metadata. Empty when absent, null, or given as an object. */
	FString Crs;
};

/**
 * `timeline.sampling` - how stored snapshots relate to the external solver.
 *
 * Optional and additive in CFDViz 1.1. These values describe cadence and a
 * temporal-adequacy measurement; they never advance a simulation in FlowViz.
 */
struct FCFDVizTimelineSampling
{
	/** Physical time advanced by one source solver step, in `units.time`. Must be finite and > 0. */
	double SourceTimeStep = 0.0;

	/** Number of source solver steps between adjacent stored snapshots. Must be >= 1. */
	int64 StoredStepStride = 0;

	/** Maximum important-feature displacement between stored snapshots, measured in grid cells. Finite and >= 0. */
	double MaxFeatureDisplacementCells = 0.0;
};

/**
 * `timeline` - the stored frames (required: `frameCount`, `times`).
 */
struct FCFDVizTimeline
{
	/** Number of stored frames. Equals Times.Num(), and Steps.Num() when steps are present (section 3.1). May be 0. */
	int32 FrameCount = 0;

	/** Physical time of each frame in `units.time`. Strictly increasing and finite (section 3.1). */
	TArray<double> Times;

	/** Optional solver step number per frame, for provenance and UI display. Empty when absent. */
	TArray<int64> Steps;

	/**
	 * Default temporal interpolation.
	 *
	 * DEFAULTS TO Nearest when the manifest omits it. The schema declares no
	 * default, so this reader picks the one that cannot fabricate data: Nearest
	 * only ever shows a frame that exists on disk. Defaulting to Linear would
	 * make an unannotated manifest silently display frames the solver never
	 * produced.
	 */
	ECFDVizInterpolation DefaultInterpolation = ECFDVizInterpolation::Nearest;

	/** External-solver sampling cadence and adequacy evidence. Unset when the manifest omits `timeline.sampling`. */
	TOptional<FCFDVizTimelineSampling> Sampling;

	bool HasSteps() const
	{
		return Steps.Num() > 0;
	}

	/** @return false, leaving OutTime untouched, when FrameIndex is outside [0, Times.Num()). */
	bool TryGetTime(int32 FrameIndex, double& OutTime) const
	{
		if (!Times.IsValidIndex(FrameIndex))
		{
			return false;
		}
		OutTime = Times[FrameIndex];
		return true;
	}

	/** @return false when steps are absent or FrameIndex is out of range. */
	bool TryGetStep(int32 FrameIndex, int64& OutStep) const
	{
		if (!Steps.IsValidIndex(FrameIndex))
		{
			return false;
		}
		OutStep = Steps[FrameIndex];
		return true;
	}

	/**
	 * Section 3.1's monotonicity invariant.
	 *
	 * Non-finite entries are rejected explicitly rather than left to the `>`
	 * comparison: every comparison against NaN is false, so a single-element
	 * `[NaN]` timeline would otherwise pass a pairwise-only check, and the schema
	 * states NaN is not a legal time.
	 */
	FLOWVIZRUNTIME_API bool IsStrictlyIncreasing() const;
};

/**
 * `grid` - one uniform Cartesian grid (required: `id`, `type`, `dimensions`, `origin`, `spacing`).
 */
struct FCFDVizGridDescriptor
{
	/** Unique within `grids`. Referenced by `field.grid`. */
	FName Id;

	FString Name;

	ECFDVizGridType Type = ECFDVizGridType::UniformCartesian;

	/** Dimensions (always CELL counts), origin and spacing. See FCFDVizGrid for the cell-versus-point rules. */
	FCFDVizGrid Geometry;

	/**
	 * `maskField` - id of a field whose values reject cells from statistics and
	 * rendering. NAME_None when absent. Must name a declared field (section 3.1).
	 */
	FName MaskFieldId;

	bool HasMaskField() const
	{
		return !MaskFieldId.IsNone();
	}
};

/**
 * `field.storage` - where and how a field's per-frame files are stored
 * (required: `type`, `codec`, `pathPattern`).
 */
struct FCFDVizFieldStorage
{
	ECFDVizStorageType Type = ECFDVizStorageType::BrickedVolume;

	/**
	 * Advisory brick edge lengths. Unset when absent.
	 *
	 * The CVF header is authoritative (section 4.1) - this exists for tooling and
	 * for pre-sizing caches. A reader must never decode a brick using this value
	 * in preference to the one in the file it actually opened.
	 */
	TOptional<FIntVector> BrickSize;

	/**
	 * Payload codec. Defaults to Zlib only in the sense that a default-constructed
	 * struct holds it; `codec` is REQUIRED, so a parsed field always carries what
	 * the manifest said.
	 *
	 * Zstd parses successfully here on purpose. Section 7 requires a 1.0 reader to
	 * reject zstd with one exact message, which it cannot do if parsing has
	 * already failed with "unknown codec". See FCFDVizCase::CheckCodecSupport.
	 */
	ECFDVizCodec Codec = ECFDVizCodec::Zlib;

	/** Codec level recorded for provenance. Unset when absent. Does not affect decoding - the compressed stream is self-describing. */
	TOptional<int32> Level;

	/**
	 * Per-frame path relative to the case root, with a Python `str.format`
	 * placeholder for the frame index, e.g. "frames/{frame:06d}/U.cvf".
	 *
	 * Passes the section 1.3 traversal check both as written and after
	 * substitution. Expand it with FCFDVizCase::FormatFramePath, or better,
	 * resolve it with FCFDVizCase::ResolveFieldFramePath, which applies the
	 * traversal check and combines with the case root.
	 */
	FString PathPattern;
};

/**
 * `field.statistics` - declared ranges over all stored frames.
 *
 * Every member is optional and independently so, hence the explicit flags rather
 * than TOptional-wrapped arrays: a manifest may declare component ranges and no
 * magnitude range, or neither.
 *
 * NaN values and cells rejected by the mask field are excluded (format rules 1.7
 * and 4.4.7). A field with no valid data anywhere would need +inf/-inf, which
 * standard JSON cannot express, so such a field omits statistics instead - which
 * is why "absent" must never be read as "zero".
 *
 * READ THE FLAGS, NOT THE VALUES. A half-declared statistic - one component array
 * without its partner, or a minimum without a maximum - is kept exactly as
 * written rather than dropped, because discarding a declared number is the silent
 * normalisation format rule 1.6 forbids, and rejecting it would make this reader
 * refuse a manifest the schema and the Python reference both accept. The flags
 * are the only thing that says a range is usable.
 */
struct FCFDVizFieldStatistics
{
	/**
	 * Per component, in `field.components` order, exactly as declared.
	 *
	 * Either array may be non-empty while bHasComponentRange is false, when the
	 * manifest declared only one of the pair or the two lengths disagree. Do not
	 * pair them up without checking the flag.
	 */
	TArray<double> GlobalComponentMin;
	TArray<double> GlobalComponentMax;

	/** True when BOTH component arrays were declared and have equal length. Only then do the two arrays describe ranges. */
	bool bHasComponentRange = false;

	/** Zero when not declared - which is a placeholder, not a measurement. Gate on bHasMagnitudeRange. */
	double GlobalMagnitudeMin = 0.0;
	double GlobalMagnitudeMax = 0.0;

	/** True when BOTH magnitude bounds were declared. */
	bool bHasMagnitudeRange = false;

	bool IsEmpty() const
	{
		return !bHasComponentRange && !bHasMagnitudeRange;
	}

	/**
	 * Convert the declared ranges into the shared statistics type.
	 *
	 * NaNCount and ValidCount are left at 0 because the manifest does not record
	 * them. A caller must therefore not derive a mean or a NaN fraction from the
	 * result - it carries ranges only.
	 *
	 * @return false, leaving OutStatistics untouched, when no component range was
	 *         declared or its length disagrees with ComponentCount.
	 */
	FLOWVIZRUNTIME_API bool TryMakeStatistics(int32 ComponentCount, FCFDVizStatistics& OutStatistics) const;
};

/**
 * `field.display` / `derivedField.display` - presentation hints.
 *
 * Format rule 1.6: a reader MUST NOT let any of these alter a stored value.
 * They choose how numbers are shown, never what the numbers are.
 */
struct FCFDVizFieldDisplay
{
	/**
	 * "magnitude" (CFDViz::MagnitudeComponentName) or one of `field.components`.
	 * Empty when absent, meaning the consumer chooses - component 0 for a scalar,
	 * magnitude for a vector, is the usual choice.
	 */
	FString DefaultComponent;

	/**
	 * Colormap name, matching the Python keys (see CFDVizColorMaps.h). Empty when absent.
	 *
	 * Kept as a string rather than parsed to ECFDVizColorMap here so an
	 * unrecognised or newly-added colormap name is a display fallback, not a
	 * refusal to load the case.
	 */
	FString DefaultColorMap;

	/** Defaults to Global when absent: a range that is stable across the animation, so a structure does not appear to pulse as the range rescales per frame. */
	ECFDVizRangeMode DefaultRangeMode = ECFDVizRangeMode::Global;

	/** [min, max] in the field's own unit. Unset when absent. X is min, Y is max. */
	TOptional<FVector2D> RecommendedRange;

	/** Piecewise-linear opacity transfer function as (value, opacity) pairs; X is the value in the field's own unit, Y is opacity in [0,1]. Empty when absent. */
	TArray<FVector2D> OpacityPoints;
};

/**
 * `field.phase` - interpretation of a scalar free-surface/interface field.
 *
 * Optional in CFDViz 1.1. It is metadata over stored floating-point values; it
 * does not change decoding, clamp values, or generate topology.
 */
struct FCFDVizPhaseInterpretation
{
	ECFDVizPhaseRepresentation Representation = ECFDVizPhaseRepresentation::VolumeFraction;

	/** Phase on the declared `Inside` side, e.g. "water". Empty when unnamed. */
	FString PrimaryPhase;

	/** Opposite phase, e.g. "air". Empty when unnamed. */
	FString SecondaryPhase;

	/** Iso-value of the interface. Must be finite; volume fractions additionally require [0, 1]. */
	double InterfaceValue = 0.5;

	ECFDVizPhaseInside Inside = ECFDVizPhaseInside::GreaterThanInterface;
};

/**
 * `field` - one stored volume field (required: `numericId`, `id`, `components`,
 * `componentCount`, `dataType`, `association`, `grid`, `storage`).
 */
struct FCFDVizField
{
	/**
	 * Unique numeric id, written into the CVF header as uint32 so a .cvf can be
	 * tied back to its manifest entry without trusting its filename.
	 *
	 * uint32, NOT int32. The schema's range is 0..4294967295 and the CVF header
	 * field is uint32 (section 4.1); an int32 here would reject - or worse, wrap -
	 * a numericId the Python writer accepts, and the two implementations would
	 * disagree about a file they both consider valid.
	 */
	uint32 NumericId = 0;

	/** Unique within `fields`. Referenced by `grid.maskField` and by known_values.json samples. */
	FName Id;

	FString Name;
	FString Description;

	/** Physical meaning hint, e.g. "velocity", "pressure", "vorticity", "mask". Free-form; drives glyph and pseudovector choices in the adapter. */
	FString Semantic;

	/** Shape hint, e.g. "scalar", "vector". ComponentCount is authoritative; this is a label. */
	FString Kind;

	/** Component names in storage order. Components are interleaved per voxel (section 4.4.2). */
	TArray<FString> Components;

	/** Equals Components.Num() (section 3.1). 1..4 - the CVF header and directory carry float32[4] background/min/max slots. */
	int32 ComponentCount = 0;

	/** Storage precision. Restricted to float16/float32/uint8 for a CVF; float64 is rejected at parse time (section 3.3). */
	ECFDVizDataType DataType = ECFDVizDataType::Float32;

	/** Cell or point. The half-cell offset between them is the format's most common visualisation bug; let FCFDVizGrid apply it. */
	ECFDVizAssociation Association = ECFDVizAssociation::Cell;

	/** Id of the grid this field is sampled on. Must name a declared grid (section 3.1). */
	FName GridId;

	/** Solver unit of the stored values, e.g. "m/s". Empty when absent - which means unknown, not dimensionless. */
	FString Unit;

	/** `solverPrecision` - provenance only, the precision the solver computed in. Does not affect storage or decoding. Unset when absent. */
	TOptional<ECFDVizDataType> SolverPrecision;

	/** Unset when absent, in which case the timeline's default applies - see ResolveTemporalInterpolation. */
	TOptional<ECFDVizInterpolation> TemporalInterpolation;

	/** Free-surface/interface interpretation. Unset for ordinary scalar and vector fields. */
	TOptional<FCFDVizPhaseInterpretation> Phase;

	FCFDVizFieldStorage Storage;
	FCFDVizFieldStatistics Statistics;
	FCFDVizFieldDisplay Display;

	/**
	 * Index of a component by name, case-sensitively.
	 *
	 * @return INDEX_NONE if there is no such component. Case-sensitive because
	 *         `display.defaultComponent` is compared against these same strings on
	 *         the Python side, and a case-insensitive match here would accept a
	 *         manifest the reference implementation rejects.
	 */
	FLOWVIZRUNTIME_API int32 FindComponentIndex(const FString& ComponentName) const;

	/** This field's `temporalInterpolation` when declared, otherwise the timeline default. */
	ECFDVizInterpolation ResolveTemporalInterpolation(ECFDVizInterpolation TimelineDefault) const
	{
		return TemporalInterpolation.Get(TimelineDefault);
	}
};

/**
 * `mesh.patches[]` - one named boundary patch (required: `id`, `name`).
 *
 * `Id` matches the CVM `patchIds` array, which stores one uint32 per TRIANGLE
 * (section 5.3) - not per vertex.
 */
struct FCFDVizBoundaryPatch
{
	/** uint32 to match the CVM patchIds element type exactly. Unique within its mesh. */
	uint32 Id = 0;

	/** Required, non-empty. The shipped sample case must contain inlet, outlet, sideWalls and cylinderWall (section 5.3). */
	FString Name;

	/** Boundary condition class, e.g. "wall", "inlet", "outlet", "symmetry". Free-form, so an unknown value is displayed rather than rejected. */
	FString Type;

	/** Suggested display colour. LINEAR RGB, not sRGB - the manifest carries linear values and converting here would shift every patch colour. */
	FLinearColor Color = FLinearColor::White;

	/** False when `color` was absent, so a consumer can apply its own palette instead of a fabricated white. */
	bool bHasColor = false;

	/** Defaults to true when absent: a declared boundary is part of the geometry, and hiding it by default would make a case look like it is missing surfaces. */
	bool bDefaultVisible = true;

	/** Defaults to 1 (opaque) when absent. */
	float Opacity = 1.0f;
};

/**
 * `mesh` - one CVM triangle mesh (required: `id`, `path`).
 */
struct FCFDVizMesh
{
	FName Id;
	FString Name;

	/** Path to the .cvm relative to the case root. Passes the section 1.3 traversal check. Resolve with FCFDVizCase::ResolveMeshPath. */
	FString Path;

	/** Role hint, e.g. "boundary", "obstacle", "structure", "decoration". Not a closed enum, so an unknown value is ignored rather than rejected. */
	FString Role;

	/**
	 * True when the geometry does not change across frames.
	 *
	 * DEFAULTS TO true when absent, because in 1.x a mesh declares a single
	 * `path` with no frame placeholder - so its geometry cannot vary by frame and
	 * defaulting to false would describe something the format cannot express.
	 */
	bool bStatic = true;

	/** Optional placement into canonical coordinates, already transposed into Unreal's row-vector convention. Unset means identity. */
	TOptional<FMatrix> Transform;

	/** Named boundary patches. Patch ids are unique within this mesh. */
	TArray<FCFDVizBoundaryPatch> Patches;

	/** @return nullptr when no patch has that id. The pointer is into Patches and is invalidated by any change to it. */
	FLOWVIZRUNTIME_API const FCFDVizBoundaryPatch* FindPatch(uint32 PatchId) const;
};

/**
 * `derivedFields[]` - a field computed on demand from stored fields, never
 * written to disk (required: `id`, `expression`).
 */
struct FCFDVizDerivedField
{
	FName Id;
	FString Name;

	/** Evaluation expression over stored field ids, e.g. "mag(U)". Not parsed here - evaluation is the sampler's job. */
	FString Expression;

	FString Unit;

	/** Component names, when the expression's result shape is declared. Empty when absent. */
	TArray<FString> Components;

	/** 0 when absent. When both this and Components are declared they must agree. */
	int32 ComponentCount = 0;

	FCFDVizFieldDisplay Display;
};

/**
 * `structures[]` - association between a CVM mesh and mesh-associated (CVA)
 * results (required: `id`, `mesh`).
 *
 * Reserved for FEA in 1.x: the format specifies and round-trip tests CVA, but no
 * 1.x UI renders every result type, and 1.x sample cases declare this empty. It
 * is parsed and validated anyway so a case that does declare one is not silently
 * half-read.
 */
struct FCFDVizStructure
{
	FName Id;
	FString Name;

	/** Id of a declared mesh. */
	FName MeshId;

	/** Ids of CVA-backed results. Empty when absent. Not resolved against `fields`, which holds CVF volume fields only. */
	FString DisplacementField;
	FString VelocityField;
	FString StressField;
	FString StrainField;

	/** e.g. "raw", "smoothed". */
	TArray<FString> Variants;

	/** Relative path to a vertex-to-node map. Empty when absent; passes the section 1.3 traversal check when present. */
	FString VertexToNodeMap;
};

/**
 * `qualityMetrics` - recomputable evidence that a dataset is representative.
 *
 * Optional in CFDViz 1.1. These declarations are validated for shape and
 * cross-references, but a converter/acceptance harness must recompute them from
 * payloads rather than trusting the manifest as proof by itself.
 */
struct FCFDVizQualityMetrics
{
	/** Grid on which all spatial metrics were evaluated. */
	FName GridId;

	/** Number of active/valid cells used by the measurement. */
	int64 ActiveCellCount = 0;

	/** Extent of active data per axis, bounded by the declared grid dimensions. */
	FIntVector ActiveDimensions = FIntVector::ZeroValue;

	/** Number of ActiveDimensions components greater than one. */
	int32 EffectiveSpatialDimensions = 0;

	/** Three-component velocity field on GridId used for the velocity metrics. */
	FName VelocityFieldId;

	/** RMS velocity component values in velocity-field component order. */
	FVector VelocityComponentRms = FVector::ZeroVector;

	/** RMS gradient in the spanwise direction; finite and non-negative. */
	double SpanwiseGradientRms = 0.0;

	/** Number of payload frames evaluated; must equal timeline.frameCount. */
	int32 TemporalFrameCount = 0;
};

/**
 * `provenance` - free-form origin record. Never load-bearing for decoding.
 */
struct FCFDVizProvenance
{
	/** Origin class, e.g. "external-solver". Empty when not declared. */
	FString SourceType;
	FString GeneratorCommand;
	FString GeneratorVersion;
	FString SourceCase;
	FString SourceRevision;
	FString ExportCommand;
	TArray<FString> Notes;
};

/* -------------------------------------------------------------------------- */
/* Manifest-level constants                                                     */
/* -------------------------------------------------------------------------- */

namespace CFDViz
{
	/** `format` must be exactly this string. Any other value is not a CFDViz case. */
	inline constexpr const TCHAR* FormatMarker = TEXT("CFDViz");

	/** The reserved value of `display.defaultComponent` meaning "the Euclidean magnitude", as opposed to a name in `field.components`. */
	inline constexpr const TCHAR* MagnitudeComponentName = TEXT("magnitude");

	/**
	 * Largest manifest.json this reader will read into memory.
	 *
	 * Format rule 1.5 forbids allocating a buffer sized from untrusted input
	 * without first proving it consistent with the file on disk. A manifest has no
	 * internal length field to check, so the check is against this ceiling: even a
	 * hundred-thousand-frame timeline is a couple of megabytes of JSON, so 64 MiB
	 * is generous for legitimate data and still bounds a hostile one.
	 */
	inline constexpr int64 MaxManifestBytes = 64ll * 1024ll * 1024ll;

	/** Longest field width a `{frame:0Nd}` placeholder may request. A path cannot need more, and it bounds the string a hostile pattern can make this reader build. */
	inline constexpr int32 MaxFramePlaceholderWidth = 32;
}

/* -------------------------------------------------------------------------- */
/* The case                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * A parsed, validated manifest.json plus the case root it was found in.
 *
 * A successfully returned FCFDVizCase has already passed every section 3.1
 * invariant - ParseFromString runs Validate before returning, so there is no way
 * to obtain a parsed-but-unvalidated case by forgetting a call.
 *
 * CHECKS PERFORMED BEYOND THE LITERAL SECTION 3.1 LIST. These are the same class
 * of defect as the listed invariants and are enumerated here so the Python
 * reference can mirror them exactly - a check on one side only produces a case
 * that loads in one implementation and not the other, which is precisely what
 * the two-implementation design exists to catch:
 *   - `format` is exactly "CFDViz".
 *   - grid, mesh, derived-field and structure ids are each unique within their array.
 *   - patch ids are unique within their mesh.
 *   - `structure.mesh` names a declared mesh.
 *   - `derivedField.componentCount` agrees with its `components`, when both are declared.
 *   - `mesh.path` and `structure.vertexToNodeMap` pass the traversal check, as
 *     `field.storage.pathPattern` explicitly must.
 *   - `storage.pathPattern` expands successfully for the first and last frame,
 *     and each expansion still passes the traversal check.
 *   - grid dimensions fit in int32 (see ValidateGridDimensions in the .cpp for
 *     why this reader's limit is stricter than the schema's uint32).
 *
 * DELIBERATELY NOT CHECKED HERE: `storage.codec == zstd`. Section 3.1 does not
 * list it, so a zstd manifest is a VALID manifest that this build cannot decode -
 * a different thing from a malformed one. Call CheckCodecSupport to surface it
 * at load time with the exact wording section 7 mandates, rather than discovering
 * it brick by brick.
 */
struct FCFDVizCase
{
	/* --- Provenance of this object, not of the data --------------------------- */

	/** Absolute path of the manifest.json this was parsed from. Empty when parsed from a string with no context path. */
	FString ManifestPath;

	/** Absolute path of the directory containing the manifest - the case root every relative path is resolved against. Empty when unknown, in which case path resolution fails with a specific error rather than guessing at the current working directory. */
	FString CaseRootDir;

	/* --- Manifest contents ---------------------------------------------------- */

	/**
	 * `format`, exactly as written. Must be CFDViz::FormatMarker.
	 *
	 * Kept as a field rather than being checked and discarded at parse time so
	 * Validate() can enforce it on a case assembled programmatically too - a test
	 * or a future writer that forgets the marker would otherwise produce something
	 * this reader accepts and every other CFDViz implementation refuses.
	 */
	FString Format;

	/** `version` exactly as written, including any prerelease or build suffix. */
	FString FormatVersion;

	/** Parsed MAJOR.MINOR.PATCH. VersionMajor is always CFDViz::SupportedMajorVersion in a loaded case; VersionMinor may exceed SupportedMinorVersion (format rule 1.4). */
	int32 VersionMajor = 0;
	int32 VersionMinor = 0;
	int32 VersionPatch = 0;

	FCFDVizCaseMetadata Metadata;
	FCFDVizUnits Units;
	FCFDVizCoordinates Coordinates;
	FCFDVizTimeline Timeline;

	TArray<FCFDVizGridDescriptor> Grids;
	TArray<FCFDVizField> Fields;
	TArray<FCFDVizMesh> Meshes;
	TArray<FCFDVizDerivedField> DerivedFields;
	TArray<FCFDVizStructure> Structures;

	/** Representative-data evidence. Unset when the additive CFDViz 1.1 block is absent. */
	TOptional<FCFDVizQualityMetrics> QualityMetrics;

	FCFDVizProvenance Provenance;

	/* --- Loading -------------------------------------------------------------- */

	/**
	 * Read, parse and validate a manifest.json.
	 *
	 * Callable from a worker thread, and must be called from one for anything
	 * user-facing (engineering rule 1). It allocates no UObject and touches no
	 * UWorld. The file size is checked against CFDViz::MaxManifestBytes before the
	 * contents are read, so a hostile path cannot make this reader allocate an
	 * arbitrary buffer (format rule 1.5).
	 *
	 * OutCase is left untouched on failure, so a caller that ignores the result
	 * cannot end up rendering a half-populated case.
	 *
	 * @param ManifestPath Path to manifest.json. Its containing directory becomes CaseRootDir.
	 */
	static FLOWVIZRUNTIME_API FCFDVizResult LoadFromFile(const FString& ManifestPath, FCFDVizCase& OutCase);

	/**
	 * Parse and validate manifest text that is already in memory.
	 *
	 * Validation is not optional here: this runs every section 3.1 invariant
	 * before returning, so a successful return always means a usable case.
	 *
	 * @param Json        The manifest text. UTF-8 source is decoded by the caller;
	 *                    FFileHelper handles the BOM if one is present.
	 * @param ContextPath The path this text came from, used for error messages and
	 *                    to derive CaseRootDir (its containing directory). Pass
	 *                    empty for a synthetic manifest in a test - the case then
	 *                    parses and validates, and path resolution fails with
	 *                    "case root is unknown" rather than resolving against the
	 *                    process working directory.
	 */
	static FLOWVIZRUNTIME_API FCFDVizResult ParseFromString(const FString& Json, const FString& ContextPath, FCFDVizCase& OutCase);

	/**
	 * Every section 3.1 invariant, plus the extras listed in this struct's comment.
	 *
	 * Public and idempotent so a case assembled programmatically - by a test, or
	 * by a future writer - can be checked with the same code that checks a parsed
	 * one. Returns the FIRST failure, naming the offending property with a JSON
	 * path such as "fields[2].componentCount".
	 */
	FLOWVIZRUNTIME_API FCFDVizResult Validate() const;

	/**
	 * Whether every field's declared codec can actually be decoded by this build.
	 *
	 * Returns ECFDVizError::UnsupportedCodec with exactly
	 * CFDViz::ZstdRejectionMessage for a zstd field (format section 7). Separate
	 * from Validate because a zstd manifest is well-formed - it is this reader
	 * that cannot decode it - and because the mandated wording is about the data,
	 * not about the manifest being invalid.
	 *
	 * Call it after loading. Without it the first symptom is a decode failure deep
	 * in a brick loop, long after the point where the message could name the field.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult CheckCodecSupport() const;

	/* --- Lookups -------------------------------------------------------------- */

	/**
	 * Returned pointers are into the arrays above and are invalidated by any
	 * change to them. Lookup is a linear scan: a case declares a handful of
	 * fields, and an index map would add copy cost and a staleness failure mode to
	 * save nothing measurable.
	 *
	 * @return nullptr when nothing matches. Never a default-constructed object -
	 *         "no such field" must not be indistinguishable from "a field with no data".
	 */
	FLOWVIZRUNTIME_API const FCFDVizField* FindField(FName Id) const;
	FLOWVIZRUNTIME_API const FCFDVizField* FindFieldByNumericId(uint32 NumericId) const;
	FLOWVIZRUNTIME_API const FCFDVizGridDescriptor* FindGrid(FName Id) const;
	FLOWVIZRUNTIME_API const FCFDVizMesh* FindMesh(FName Id) const;
	FLOWVIZRUNTIME_API const FCFDVizDerivedField* FindDerivedField(FName Id) const;

	/** The grid a field is sampled on. nullptr if the field is unknown or its grid does not resolve - impossible in a validated case, but this is called on data from disk. */
	FLOWVIZRUNTIME_API const FCFDVizGridDescriptor* FindGridForField(const FCFDVizField& Field) const;

	/** Number of stored frames. */
	int32 GetFrameCount() const
	{
		return Timeline.FrameCount;
	}

	/* --- Paths ---------------------------------------------------------------- */

	/**
	 * The lexical path-traversal check of format rule 1.3.
	 *
	 * Rejects an empty path, a leading '/', a Windows drive letter, any backslash,
	 * any control character, any segment exactly equal to "..", and anything
	 * longer than CFDViz::MaxRelativePathLength. Every path taken from a manifest
	 * MUST pass this before being combined with the case root - it is the defence
	 * against a case crafted to read arbitrary files.
	 *
	 * LEXICAL ONLY: no filesystem call, by design. Resolving first and checking
	 * afterwards can be defeated by a symlink planted inside the case directory.
	 *
	 * This forwards to CFDViz::IsSafeRelativePath, which mirrors the schema's
	 * `relativePath` pattern. It is exposed here as well because this is the name
	 * the manifest API is documented under; there is one implementation, not two.
	 */
	static FLOWVIZRUNTIME_API bool IsSafeRelativePath(const FString& Path);

	/**
	 * Expand a `pathPattern` for one frame, exactly as Python's `str.format` would.
	 *
	 * Both implementations must produce the same filename or the Unreal reader
	 * opens files the Python writer never wrote. The supported subset is
	 * `{{`/`}}` for literal braces and `{frame}` with an optional format spec of
	 * `[0][width][d]` - so `{frame:06d}` gives "000042" and `{frame}` gives "42".
	 * Any other field name or spec is REJECTED rather than approximated, because a
	 * silently mis-expanded path fails later as a confusing "file not found".
	 *
	 * @param FrameIndex Must be >= 0; the caller normally bounds-checks it against
	 *                   frameCount first via ResolveFieldFramePath.
	 */
	static FLOWVIZRUNTIME_API FCFDVizResult FormatFramePath(const FString& PathPattern, int32 FrameIndex, FString& OutRelativePath);

	/**
	 * Combine a case-relative path with the case root, applying the traversal check.
	 *
	 * Fails with ECFDVizError::PathTraversal for an unsafe path and with
	 * ECFDVizError::InvalidManifest when CaseRootDir is unknown. After combining it
	 * re-checks that the result is still inside the root - redundant given the
	 * lexical check, and kept as the layer that would catch a future change to
	 * either half.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult ResolveRelativePath(const FString& RelativePath, FString& OutAbsolutePath) const;

	/**
	 * Absolute path of one field's file at one frame.
	 *
	 * Bounds-checks FrameIndex against frameCount (ECFDVizError::IndexOutOfRange),
	 * expands the pattern, and applies the traversal check to the expansion. Does
	 * NOT touch the filesystem - a missing file is the CVF reader's error to
	 * report, at the point where it can say what it failed to open.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult ResolveFieldFramePath(const FCFDVizField& Field, int32 FrameIndex, FString& OutAbsolutePath) const;

	/** As above, looking the field up by id first. Fails with ECFDVizError::IndexOutOfRange when no field has that id. */
	FLOWVIZRUNTIME_API FCFDVizResult ResolveFieldFramePath(FName FieldId, int32 FrameIndex, FString& OutAbsolutePath) const;

	/** Absolute path of a mesh's .cvm. Fails with ECFDVizError::IndexOutOfRange when no mesh has that id. */
	FLOWVIZRUNTIME_API FCFDVizResult ResolveMeshPath(FName MeshId, FString& OutAbsolutePath) const;
};
