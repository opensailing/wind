// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizFieldSampler.h"

#include "CFDViz/CFDVizPayload.h"
#include "CFDViz/CFDVizVolumeReader.h"

FCFDVizResult FFlowVizFieldSampler::Build(
	const FCFDVizCase& Case, FName FieldId, int32 FrameIndex)
{
	// Reset FIRST: a failed rebuild must leave an empty sampler, not last
	// frame's field answering this frame's questions.
	bBuilt = false;
	Values.Reset();

	const FCFDVizField* Field = Case.FindField(FieldId);
	if (Field == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("The case declares no field '%s'"), *FieldId.ToString()));
	}
	const FCFDVizGridDescriptor* GridDescriptor = Case.FindGridForField(*Field);
	if (GridDescriptor == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(TEXT("Field '%s' names grid '%s', which the case does not declare"),
				*FieldId.ToString(), *Field->GridId.ToString()));
	}

	FString FramePath;
	const FCFDVizResult Resolved = Case.ResolveFieldFramePath(*Field, FrameIndex, FramePath);
	if (!Resolved.IsOk())
	{
		return Resolved;
	}

	// DISK I/O AND ZLIB -- the reason Build is documented worker-only.
	FCFDVizVolumeReader Reader;
	const FCFDVizResult Opened = Reader.Open(FramePath);
	if (!Opened.IsOk())
	{
		return Opened;
	}

	TArray<uint8> Dense;
	const FCFDVizResult Read = Reader.ReadDense(Dense);
	if (!Read.IsOk())
	{
		return Read;
	}

	/*
	 * THE CVF HEADER IS AUTHORITATIVE (format section 4.1), and the loop below
	 * reinterprets the decoded bytes with a data type and component count - so
	 * those must be the HEADER's, cross-checked against the manifest rather
	 * than trusted from it. A manifest claiming 1 component against a
	 * 3-component file reads the interleaved x,y,z stream as scalars; one
	 * claiming uint8 against float16 reads one byte of every half; one
	 * claiming the other association shifts every sample half a cell. All
	 * three stay in bounds, so nothing crashes and the values render as
	 * plausible flow - the silent failure this refusal exists to prevent.
	 * Fail closed with the module's error convention instead.
	 */
	const FCFDVizVolumeHeader& Header = Reader.GetHeader();
	if (Header.ComponentCount != Field->ComponentCount)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(
				TEXT("field '%s' declares %d components but the CVF header carries %d; ")
				TEXT("the header is authoritative and the manifest is wrong"),
				*FieldId.ToString(), Field->ComponentCount, Header.ComponentCount),
			FramePath);
	}
	if (Header.DataType != Field->DataType)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(
				TEXT("field '%s' declares data type %s but the CVF header carries %s; ")
				TEXT("the header is authoritative and the manifest is wrong"),
				*FieldId.ToString(), DataTypeToString(Field->DataType),
				DataTypeToString(Header.DataType)),
			FramePath);
	}
	if (Header.Association != Field->Association)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(
				TEXT("field '%s' declares a %s association but the CVF header carries %s; ")
				TEXT("the header is authoritative and the manifest is wrong"),
				*FieldId.ToString(),
				Field->Association == ECFDVizAssociation::Cell ? TEXT("cell") : TEXT("point"),
				Header.Association == ECFDVizAssociation::Cell ? TEXT("cell") : TEXT("point")),
			FramePath);
	}

	ComponentCount = FMath::Max(1, Header.ComponentCount);
	ValueCounts = Header.GetValueCounts();
	Association = Header.Association;
	Grid = GridDescriptor->Geometry;

	const int64 ValueTotal =
		static_cast<int64>(ValueCounts.X) * ValueCounts.Y * ValueCounts.Z;

	/*
	 * WIDENED ONCE, AT BUILD. Sampling reads eight voxels per call; converting
	 * float16/float32 payload bytes on every read would put the conversion in
	 * the integrator's inner loop. Doubles cost 2-4x the payload's memory for
	 * one frame of one field, which is bounded and paid once.
	 */
	Values.SetNumUninitialized(ValueTotal * ComponentCount);
	for (int64 Index = 0; Index < ValueTotal * ComponentCount; ++Index)
	{
		double Value = 0.0;
		// The HEADER's data type - equal to the manifest's by the cross-check
		// above, and the header is the one that describes these bytes.
		if (!CFDViz::TryReadValueAsDouble(Dense, Index, Header.DataType, Value))
		{
			Values.Reset();
			return FCFDVizResult::Fail(
				ECFDVizError::SizeMismatch,
				FString::Printf(TEXT("value %lld reads outside the decoded payload"),
					static_cast<long long>(Index)));
		}
		// NaN is STORED, not filtered: the sample-time footprint check is what
		// rejects it, so a NaN pocket poisons exactly the samples that touch
		// it rather than the whole frame.
		Values[Index] = Value;
	}

	bBuilt = true;
	return FCFDVizResult::Ok();
}

bool FFlowVizFieldSampler::Sample(
	const FVector& SolverPosition, TArray<double>& OutValue) const
{
	if (!bBuilt)
	{
		return false;
	}

	/*
	 * POSITION TO LATTICE COORDINATES. Values sit at ValuePosition(i,j,k):
	 * cell values at Origin + Spacing*(i+0.5), point values at
	 * Origin + Spacing*i. Subtracting the half-cell for cell association puts
	 * both on the same integer lattice, and the trilinear weights fall out of
	 * the fractional part. Same convention as FlowVizProbe::SampleStoredField;
	 * getting it backwards shifts everything half a cell.
	 */
	const FVector Half =
		Association == ECFDVizAssociation::Cell ? FVector(0.5, 0.5, 0.5) : FVector::ZeroVector;
	const FVector Lattice = (SolverPosition - Grid.Origin) / Grid.Spacing - Half;

	// The interpolation footprint is [floor, floor+1] per axis. CLAMPED to the
	// value lattice at the boundary: a sample in the outer half-cell uses the
	// edge value rather than reading off the array. OUTSIDE the domain
	// entirely -- beyond the value extent -- is refused.
	const FIntVector MaxIndex(ValueCounts.X - 1, ValueCounts.Y - 1, ValueCounts.Z - 1);

	if (Lattice.X < -0.5 || Lattice.Y < -0.5 || Lattice.Z < -0.5
		|| Lattice.X > MaxIndex.X + 0.5 || Lattice.Y > MaxIndex.Y + 0.5
		|| Lattice.Z > MaxIndex.Z + 0.5)
	{
		return false;
	}

	const auto Axis = [](double Coordinate, int32 Max, int32& OutLow, double& OutFraction)
	{
		const double Clamped = FMath::Clamp(Coordinate, 0.0, static_cast<double>(Max));
		const double Floor = FMath::FloorToDouble(Clamped);
		OutLow = FMath::Min(static_cast<int32>(Floor), Max - 1 >= 0 ? Max - 1 : 0);
		if (Max == 0)
		{
			OutLow = 0;
			OutFraction = 0.0;
			return;
		}
		OutFraction = Clamped - OutLow;
	};

	int32 I0, J0, K0;
	double FX, FY, FZ;
	Axis(Lattice.X, MaxIndex.X, I0, FX);
	Axis(Lattice.Y, MaxIndex.Y, J0, FY);
	Axis(Lattice.Z, MaxIndex.Z, K0, FZ);

	const int32 I1 = FMath::Min(I0 + 1, MaxIndex.X);
	const int32 J1 = FMath::Min(J0 + 1, MaxIndex.Y);
	const int32 K1 = FMath::Min(K0 + 1, MaxIndex.Z);

	const auto ValueAt = [this](int32 I, int32 J, int32 K, int32 Component) -> double
	{
		const int64 VoxelIndex = (static_cast<int64>(K) * ValueCounts.Y + J) * ValueCounts.X + I;
		return Values[VoxelIndex * ComponentCount + Component];
	};

	OutValue.SetNum(ComponentCount);
	for (int32 Component = 0; Component < ComponentCount; ++Component)
	{
		const double C000 = ValueAt(I0, J0, K0, Component);
		const double C100 = ValueAt(I1, J0, K0, Component);
		const double C010 = ValueAt(I0, J1, K0, Component);
		const double C110 = ValueAt(I1, J1, K0, Component);
		const double C001 = ValueAt(I0, J0, K1, Component);
		const double C101 = ValueAt(I1, J0, K1, Component);
		const double C011 = ValueAt(I0, J1, K1, Component);
		const double C111 = ValueAt(I1, J1, K1, Component);

		// ANY non-finite corner rejects the sample (rule 10): blending a NaN
		// yields NaN, and blending around it would fabricate a value at
		// exactly the places the solver declined to provide one.
		if (!FMath::IsFinite(C000) || !FMath::IsFinite(C100) || !FMath::IsFinite(C010)
			|| !FMath::IsFinite(C110) || !FMath::IsFinite(C001) || !FMath::IsFinite(C101)
			|| !FMath::IsFinite(C011) || !FMath::IsFinite(C111))
		{
			OutValue.Reset();
			return false;
		}

		const double C00 = FMath::Lerp(C000, C100, FX);
		const double C10 = FMath::Lerp(C010, C110, FX);
		const double C01 = FMath::Lerp(C001, C101, FX);
		const double C11 = FMath::Lerp(C011, C111, FX);
		const double C0 = FMath::Lerp(C00, C10, FY);
		const double C1 = FMath::Lerp(C01, C11, FY);
		OutValue[Component] = FMath::Lerp(C0, C1, FZ);
	}
	return true;
}

bool FFlowVizFieldSampler::GetVoxelValue(const FIntVector& Voxel, TArray<double>& OutValue) const
{
	if (!bBuilt
		|| Voxel.X < 0 || Voxel.X >= ValueCounts.X
		|| Voxel.Y < 0 || Voxel.Y >= ValueCounts.Y
		|| Voxel.Z < 0 || Voxel.Z >= ValueCounts.Z)
	{
		return false;
	}

	const int32 VoxelIndex =
		(Voxel.Z * ValueCounts.Y + Voxel.Y) * ValueCounts.X + Voxel.X;
	OutValue.SetNumUninitialized(ComponentCount);
	for (int32 Component = 0; Component < ComponentCount; ++Component)
	{
		OutValue[Component] = Values[VoxelIndex * ComponentCount + Component];
	}
	return true;
}

bool FFlowVizFieldSampler::SampleVector(
	const FVector& SolverPosition, FVector& OutVector) const
{
	if (ComponentCount < 3)
	{
		return false;
	}
	TArray<double> Components;
	if (!Sample(SolverPosition, Components))
	{
		return false;
	}
	OutVector = FVector(Components[0], Components[1], Components[2]);
	return true;
}
