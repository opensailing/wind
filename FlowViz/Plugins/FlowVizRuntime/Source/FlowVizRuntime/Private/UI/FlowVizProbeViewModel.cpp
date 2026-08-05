// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizProbeViewModel.h"

#include "CFDViz/CFDVizPayload.h"
#include "CFDViz/CFDVizVolumeReader.h"

/**
 * See FlowVizProbeViewModel.h, and ADR 004 section 8, which is the design.
 *
 * THE Y-MIRROR APPEARS NOWHERE IN THIS FILE. Every conversion between Unreal
 * space and solver space goes through MakeUnrealToSolverTransform or
 * MakeSolverToUnrealTransform. A hand-written (x, -y, z) here would be a
 * second implementation of the mirror, and the failure mode is a probe that
 * reports a real number from the mirrored cell - which no range check catches,
 * because the number is perfectly plausible.
 */

namespace
{
	bool IsFiniteVector(const FVector& V)
	{
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
	}
}

/* ========================================================================== */
/* Unit scale                                                                  */
/* ========================================================================== */

FCFDVizResult FFlowVizProbeViewModel::SetMetersToUnrealUnits(double Scale)
{
	if (!FMath::IsFinite(Scale) || Scale <= 0.0)
	{
		// MakeUnrealToSolverTransform returns the IDENTITY for a degenerate scale,
		// which silently drops the mirror - so every probe placed by clicking would
		// land on the Y-mirrored cell and report a plausible wrong value. Refused
		// here rather than allowed to reach that fallback.
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(
				TEXT("A length scale must be finite and positive; got %g. It comes from the "
					 "manifest's units.length and must not be assumed to be metres."),
				Scale));
	}
	MetersToUnrealUnits = Scale;
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Placement                                                                   */
/* ========================================================================== */

FGuid FFlowVizProbeViewModel::AddProbeAtSolverPosition(const FVector& SolverPosition, const FString& Name)
{
	if (!IsFiniteVector(SolverPosition))
	{
		// An invalid GUID is the refusal. A probe at a NaN position samples nothing
		// and would sit in the list looking like a placed probe.
		return FGuid();
	}

	FFlowVizProbe Probe;
	Probe.Id = FGuid::NewGuid();
	Probe.SolverPosition = SolverPosition;
	Probe.Name = Name.IsEmpty()
		? FString::Printf(TEXT("Probe %d"), Probes.Num() + 1)
		: Name;
	Probes.Add(MoveTemp(Probe));
	return Probes.Last().Id;
}

FGuid FFlowVizProbeViewModel::AddProbeAtUnrealPosition(const FVector& UnrealPosition, const FString& Name)
{
	if (!IsFiniteVector(UnrealPosition))
	{
		return FGuid();
	}
	// THE SHARED ADAPTER, NOT A LOCAL FORMULA. This is the one line that makes a
	// clicked position mean the same thing as a typed one.
	const FMatrix UnrealToSolver = MakeUnrealToSolverTransform(MetersToUnrealUnits);
	return AddProbeAtSolverPosition(UnrealToSolver.TransformPosition(UnrealPosition), Name);
}

FCFDVizResult FFlowVizProbeViewModel::RestoreProbe(
	const FGuid& Id, const FVector& SolverPosition, const FString& Name)
{
	if (!Id.IsValid())
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("A restored probe needs a valid id; an invalid one cannot be looked up "
				 "afterwards, so the probe would be unreachable by every mutator here"));
	}
	if (!IsFiniteVector(SolverPosition))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("A probe position must be finite; got (%g, %g, %g)"),
				SolverPosition.X, SolverPosition.Y, SolverPosition.Z));
	}
	if (FindProbe(Id) != nullptr)
	{
		// REFUSED, NOT OVERWRITTEN AND NOT DUPLICATED. Two probes under one id
		// makes every lookup below return whichever is earlier in the array, so a
		// rename or a drag would silently act on the wrong probe.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("A probe with id %s is already present; ids key chart series and "
					 "must be unique"),
				*Id.ToString(EGuidFormats::DigitsWithHyphens)));
	}

	FFlowVizProbe Probe;
	Probe.Id = Id;
	Probe.SolverPosition = SolverPosition;
	Probe.Name = Name.IsEmpty()
		? FString::Printf(TEXT("Probe %d"), Probes.Num() + 1)
		: Name;
	Probes.Add(MoveTemp(Probe));
	return FCFDVizResult::Ok();
}

bool FFlowVizProbeViewModel::MoveProbeToSolverPosition(const FGuid& Id, const FVector& SolverPosition)
{
	if (!IsFiniteVector(SolverPosition))
	{
		return false;
	}
	FFlowVizProbe* Probe = FindProbeMutable(Id);
	if (Probe == nullptr)
	{
		return false;
	}

	Probe->SolverPosition = SolverPosition;
	// THE READING IS DROPPED, NOT KEPT. A stale value beside a new position is the
	// worst outcome available here: it is a real measurement, correctly formatted,
	// attributed to the wrong point. Clearing it makes the readout blank until a
	// new sample arrives, which is honest.
	Probe->LastReading = FFlowVizProbeReading();
	return true;
}

bool FFlowVizProbeViewModel::MoveProbeToUnrealPosition(const FGuid& Id, const FVector& UnrealPosition)
{
	if (!IsFiniteVector(UnrealPosition))
	{
		return false;
	}
	const FMatrix UnrealToSolver = MakeUnrealToSolverTransform(MetersToUnrealUnits);
	return MoveProbeToSolverPosition(Id, UnrealToSolver.TransformPosition(UnrealPosition));
}

bool FFlowVizProbeViewModel::RemoveProbe(const FGuid& Id)
{
	return Probes.RemoveAll([&Id](const FFlowVizProbe& Probe) { return Probe.Id == Id; }) > 0;
}

void FFlowVizProbeViewModel::RemoveAllProbes()
{
	Probes.Reset();
}

bool FFlowVizProbeViewModel::RenameProbe(const FGuid& Id, const FString& NewName)
{
	FFlowVizProbe* Probe = FindProbeMutable(Id);
	if (Probe == nullptr)
	{
		return false;
	}
	Probe->Name = NewName;
	return true;
}

bool FFlowVizProbeViewModel::SetProbeVisible(const FGuid& Id, bool bVisible)
{
	FFlowVizProbe* Probe = FindProbeMutable(Id);
	if (Probe == nullptr)
	{
		return false;
	}
	// A visibility toggle, not a delete: the position and the last reading survive.
	Probe->bVisible = bVisible;
	return true;
}

const FFlowVizProbe* FFlowVizProbeViewModel::FindProbe(const FGuid& Id) const
{
	return Probes.FindByPredicate([&Id](const FFlowVizProbe& Probe) { return Probe.Id == Id; });
}

FFlowVizProbe* FFlowVizProbeViewModel::FindProbeMutable(const FGuid& Id)
{
	return Probes.FindByPredicate([&Id](const FFlowVizProbe& Probe) { return Probe.Id == Id; });
}

bool FFlowVizProbeViewModel::TryGetProbeUnrealPosition(const FGuid& Id, FVector& OutUnrealPosition) const
{
	const FFlowVizProbe* Probe = FindProbe(Id);
	if (Probe == nullptr)
	{
		return false;
	}
	// The exact inverse of the placement conversion, through the same adapter -
	// so placing by click and drawing the marker round-trip to the same point.
	OutUnrealPosition =
		SolverToUnrealPosition(Probe->SolverPosition, MetersToUnrealUnits);
	return true;
}

bool FFlowVizProbeViewModel::SetProbeReading(const FGuid& Id, const FFlowVizProbeReading& Reading)
{
	FFlowVizProbe* Probe = FindProbeMutable(Id);
	if (Probe == nullptr)
	{
		return false;
	}
	Probe->LastReading = Reading;
	return true;
}

/* ========================================================================== */
/* Line probe                                                                  */
/* ========================================================================== */

FCFDVizResult FFlowVizProbeViewModel::SetLineProbe(const FVector& SolverStart, const FVector& SolverEnd)
{
	if (!IsFiniteVector(SolverStart) || !IsFiniteVector(SolverEnd))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A line probe's endpoints must be finite"));
	}
	if (FVector::DistSquared(SolverStart, SolverEnd) <= UE_SMALL_NUMBER)
	{
		// A zero-length line samples the same voxel N times and plots a flat line
		// against a zero-width axis - which reads as "the field is constant here"
		// rather than as a degenerate probe.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("A line probe needs two distinct endpoints"));
	}

	LineStart = SolverStart;
	LineEnd = SolverEnd;
	bHasLine = true;
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizProbeViewModel::SetLineSampleCount(int32 Samples)
{
	if (Samples < FlowVizProbe::MinLineSamples)
	{
		// One sample is a point probe wearing a line's controls: the plot would
		// have a single point and an axis that means nothing.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("A line probe needs at least %d samples so both endpoints are sampled; got %d"),
				FlowVizProbe::MinLineSamples, Samples));
	}
	if (Samples > FlowVizProbe::MaxLineSamples)
	{
		// Bounded because each sample is a brick decode. An unbounded typed count
		// would hang the worker rather than report a limit.
		return FCFDVizResult::Fail(
			ECFDVizError::AllocationTooLarge,
			FString::Printf(
				TEXT("A line probe is limited to %d samples; got %d"),
				FlowVizProbe::MaxLineSamples, Samples));
	}
	LineSamples = Samples;
	return FCFDVizResult::Ok();
}

void FFlowVizProbeViewModel::SetLineAxisMode(EFlowVizLineProbeAxis Mode)
{
	LineAxis = Mode;
}

FCFDVizResult FFlowVizProbeViewModel::GetLineSamplePositions(TArray<FVector>& OutPositions) const
{
	OutPositions.Reset();
	if (!bHasLine)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("No line probe has been placed"));
	}

	OutPositions.Reserve(LineSamples);
	// N SAMPLES OVER N-1 INTERVALS. Dividing by N would leave the far endpoint
	// unsampled, which shows up as a line plot that stops just short of a wall -
	// easy to read as a boundary effect in the data rather than an off-by-one.
	const double Divisor = static_cast<double>(LineSamples - 1);
	for (int32 Index = 0; Index < LineSamples; ++Index)
	{
		const double T = static_cast<double>(Index) / Divisor;
		// The lerp form a + t*(b-a) is exact at t=0 and t=1, so sample 0 is
		// bit-exactly the start and sample N-1 bit-exactly the end.
		OutPositions.Add(LineStart + T * (LineEnd - LineStart));
	}
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizProbeViewModel::GetLineSampleAxisValues(TArray<double>& OutValues) const
{
	OutValues.Reset();
	if (!bHasLine)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("No line probe has been placed"));
	}

	const double Length = FVector::Dist(LineStart, LineEnd);
	OutValues.Reserve(LineSamples);
	const double Divisor = static_cast<double>(LineSamples - 1);
	for (int32 Index = 0; Index < LineSamples; ++Index)
	{
		const double T = static_cast<double>(Index) / Divisor;
		// Distance is in SOLVER units - the axis a user reads off in metres - and
		// NormalizedDistance is what makes two lines of different lengths
		// comparable on one chart.
		OutValues.Add(LineAxis == EFlowVizLineProbeAxis::NormalizedDistance ? T : T * Length);
	}
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Sampling the stored field                                                   */
/* ========================================================================== */

FCFDVizResult FlowVizProbe::SampleStoredField(
	const FCFDVizCase& Case,
	FName FieldId,
	int32 FrameIndex,
	const FVector& SolverPosition,
	FFlowVizProbeReading& OutReading)
{
	// Overwritten, never partially filled: a caller that ignores the return must
	// not find last call's components beside this call's failure.
	OutReading = FFlowVizProbeReading();

	const FCFDVizField* Field = Case.FindField(FieldId);
	if (Field == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("The case declares no field '%s'"), *FieldId.ToString()));
	}

	const FCFDVizGridDescriptor* Grid = Case.FindGridForField(*Field);
	if (Grid == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(
				TEXT("Field '%s' names grid '%s', which the case does not declare"),
				*FieldId.ToString(), *Field->GridId.ToString()));
	}

	if (!Case.Timeline.Times.IsValidIndex(FrameIndex))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("Frame %d is outside the case's %d frames"),
				FrameIndex, Case.Timeline.Times.Num()));
	}

	OutReading.FrameIndex = FrameIndex;
	OutReading.Time = Case.Timeline.Times[FrameIndex];

	if (!IsFiniteVector(SolverPosition))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A probe position must be finite"));
	}

	/*
	 * POSITION TO VOXEL INDEX.
	 *
	 * For a CELL field, value (i,j,k) sits at Origin + Spacing*(i+0.5), so the
	 * cell containing P is floor((P - Origin) / Spacing) - no half-cell term,
	 * because the half-cell is in the value's POSITION, not in the containment
	 * test. For a POINT field, values sit ON the lattice, so the nearest one is
	 * round((P - Origin) / Spacing).
	 *
	 * Getting this backwards shifts every probe by half a cell, which for the
	 * shipped sample is ~0.1 m - enough to read the neighbouring cell near a
	 * gradient and produce a number that is real, plausible, and from the wrong
	 * place.
	 */
	const FCFDVizGrid& Geometry = Grid->Geometry;
	const FVector Relative = (SolverPosition - Geometry.Origin) / Geometry.Spacing;

	FIntVector Voxel;
	if (Field->Association == ECFDVizAssociation::Point)
	{
		Voxel = FIntVector(
			static_cast<int32>(FMath::RoundToDouble(Relative.X)),
			static_cast<int32>(FMath::RoundToDouble(Relative.Y)),
			static_cast<int32>(FMath::RoundToDouble(Relative.Z)));
	}
	else
	{
		Voxel = FIntVector(
			static_cast<int32>(FMath::FloorToDouble(Relative.X)),
			static_cast<int32>(FMath::FloorToDouble(Relative.Y)),
			static_cast<int32>(FMath::FloorToDouble(Relative.Z)));
	}

	if (!Geometry.Contains(Voxel.X, Voxel.Y, Voxel.Z, Field->Association))
	{
		// OUTSIDE IS AN ANSWER, NOT AN ERROR. Ok with bHasValue and bInsideDomain
		// both false, so a readout says "outside the domain" rather than showing a
		// failure the user cannot act on - or worse, a zero.
		OutReading.bInsideDomain = false;
		OutReading.bHasValue = false;
		return FCFDVizResult::Ok();
	}

	OutReading.bInsideDomain = true;
	OutReading.Voxel = Voxel;
	// The centre of the voxel actually read, which is generally NOT the requested
	// position. Reported so a readout can name the cell rather than implying a
	// point measurement was taken where the user clicked.
	OutReading.SampledSolverPosition =
		Geometry.ValuePosition(Voxel.X, Voxel.Y, Voxel.Z, Field->Association);

	FString FramePath;
	const FCFDVizResult Resolved = Case.ResolveFieldFramePath(*Field, FrameIndex, FramePath);
	if (!Resolved.IsOk())
	{
		OutReading.Status = Resolved;
		return Resolved;
	}

	// DISK I/O AND DECOMPRESSION - which is why this is a free function a worker
	// calls, not a method on a game-thread view model (engineering rule 1).
	FCFDVizVolumeReader Reader;
	const FCFDVizResult Opened = Reader.Open(FramePath);
	if (!Opened.IsOk())
	{
		OutReading.Status = Opened;
		return Opened;
	}

	TArray<uint8> VoxelBytes;
	const FCFDVizResult Read = Reader.ReadVoxel(Voxel.X, Voxel.Y, Voxel.Z, VoxelBytes);
	if (!Read.IsOk())
	{
		OutReading.Status = Read;
		return Read;
	}

	const int32 Components = FMath::Max(1, Field->ComponentCount);
	OutReading.Components.Reserve(Components);
	for (int32 Index = 0; Index < Components; ++Index)
	{
		double Value = 0.0;
		if (!CFDViz::TryReadValueAsDouble(VoxelBytes, Index, Field->DataType, Value))
		{
			OutReading.Components.Reset();
			OutReading.bHasValue = false;
			OutReading.Status = FCFDVizResult::Fail(
				ECFDVizError::SizeMismatch,
				FString::Printf(
					TEXT("Voxel payload is too short for component %d of %d"), Index, Components),
				FramePath);
			return OutReading.Status;
		}
		// NaN IS PRESERVED (format rule 1.7). Coercing it to zero here would turn
		// "no data" into a measurement of zero, which is rule 10's exact failure.
		OutReading.Components.Add(Value);
	}

	// Magnitude over the DECLARED component count, never over a pad channel.
	double SumSquares = 0.0;
	for (const double Value : OutReading.Components)
	{
		SumSquares += Value * Value;
	}
	// sqrt of a NaN sum is NaN, which is what we want: an invalid component must
	// not be able to produce a finite-looking magnitude.
	OutReading.Magnitude = FMath::Sqrt(SumSquares);

	OutReading.bHasValue = true;
	return FCFDVizResult::Ok();
}
