// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizTypes.h"
#include "FlowVizRuntime.h"

/* -------------------------------------------------------------------------- */
/* Enum helpers                                                                 */
/* -------------------------------------------------------------------------- */

int32 SizeOfDataType(ECFDVizDataType DataType)
{
	switch (DataType)
	{
	case ECFDVizDataType::Float16: return 2;
	case ECFDVizDataType::Float32: return 4;
	case ECFDVizDataType::UInt8:   return 1;
	case ECFDVizDataType::Float64: return 8;
	default:                       return 0;
	}
}

bool IsDataTypeSupportedInCvf(ECFDVizDataType DataType)
{
	// Section 3.3: float64 storage is excluded from 1.0 for GPU rendering. A
	// manifest may record the original solver precision, but not store in it.
	return DataType == ECFDVizDataType::Float16
		|| DataType == ECFDVizDataType::Float32
		|| DataType == ECFDVizDataType::UInt8;
}

bool IsDataTypeSupportedInCva(ECFDVizDataType DataType)
{
	// Section 6.2: CVA offers float64 and does not offer CVF's uint8.
	return DataType == ECFDVizDataType::Float16
		|| DataType == ECFDVizDataType::Float32
		|| DataType == ECFDVizDataType::Float64;
}

FName CodecToFName(ECFDVizCodec Codec)
{
	switch (Codec)
	{
	case ECFDVizCodec::Zlib: return FName(NAME_Zlib);
	case ECFDVizCodec::LZ4:  return FName(NAME_LZ4);

	// None is a straight copy and Zstd must be rejected outright, so neither has
	// an FCompression format. Returning NAME_None here rather than guessing means
	// a caller that forgot to handle them fails an obvious lookup instead of
	// decoding a payload with the wrong codec.
	default: return NAME_None;
	}
}

bool IsCodecSupported(ECFDVizCodec Codec)
{
	switch (Codec)
	{
	case ECFDVizCodec::None:
	case ECFDVizCodec::LZ4:
	case ECFDVizCodec::Zlib:
		return true;

	// Reserved but unimplemented in 1.0 (section 7 / ADR 005). Never falls back.
	case ECFDVizCodec::Zstd:
	default:
		return false;
	}
}

const TCHAR* DataTypeToString(ECFDVizDataType DataType)
{
	switch (DataType)
	{
	case ECFDVizDataType::Float16: return TEXT("float16");
	case ECFDVizDataType::Float32: return TEXT("float32");
	case ECFDVizDataType::UInt8:   return TEXT("uint8");
	case ECFDVizDataType::Float64: return TEXT("float64");
	default:                       return TEXT("<unknown>");
	}
}

const TCHAR* AssociationToString(ECFDVizAssociation Association)
{
	switch (Association)
	{
	case ECFDVizAssociation::Cell:  return TEXT("cell");
	case ECFDVizAssociation::Point: return TEXT("point");
	default:                        return TEXT("<unknown>");
	}
}

const TCHAR* CodecToString(ECFDVizCodec Codec)
{
	switch (Codec)
	{
	case ECFDVizCodec::None: return TEXT("none");
	case ECFDVizCodec::Zstd: return TEXT("zstd");
	case ECFDVizCodec::LZ4:  return TEXT("lz4");
	case ECFDVizCodec::Zlib: return TEXT("zlib");
	default:                 return TEXT("<unknown>");
	}
}

const TCHAR* CFDVizErrorToString(ECFDVizError Error)
{
	switch (Error)
	{
	case ECFDVizError::None:                    return TEXT("None");
	case ECFDVizError::FileNotFound:            return TEXT("FileNotFound");
	case ECFDVizError::FileReadFailed:          return TEXT("FileReadFailed");
	case ECFDVizError::FileTooSmall:            return TEXT("FileTooSmall");
	case ECFDVizError::BadMagic:                return TEXT("BadMagic");
	case ECFDVizError::UnsupportedVersion:      return TEXT("UnsupportedVersion");
	case ECFDVizError::UnsupportedEndianness:   return TEXT("UnsupportedEndianness");
	case ECFDVizError::InvalidHeader:           return TEXT("InvalidHeader");
	case ECFDVizError::HeaderCrcMismatch:       return TEXT("HeaderCrcMismatch");
	case ECFDVizError::PayloadCrcMismatch:      return TEXT("PayloadCrcMismatch");
	case ECFDVizError::DirectoryOutOfBounds:    return TEXT("DirectoryOutOfBounds");
	case ECFDVizError::PayloadOutOfBounds:      return TEXT("PayloadOutOfBounds");
	case ECFDVizError::SizeMismatch:            return TEXT("SizeMismatch");
	case ECFDVizError::UnsupportedCodec:        return TEXT("UnsupportedCodec");
	case ECFDVizError::DecompressionFailed:     return TEXT("DecompressionFailed");
	case ECFDVizError::UnsupportedDataType:     return TEXT("UnsupportedDataType");
	case ECFDVizError::UnsupportedAssociation:  return TEXT("UnsupportedAssociation");
	case ECFDVizError::InvalidManifest:         return TEXT("InvalidManifest");
	case ECFDVizError::PathTraversal:           return TEXT("PathTraversal");
	case ECFDVizError::IndexOutOfRange:         return TEXT("IndexOutOfRange");
	case ECFDVizError::AllocationTooLarge:      return TEXT("AllocationTooLarge");
	default:                                    return TEXT("<unknown>");
	}
}

bool TryParseDataType(const FString& Name, ECFDVizDataType& OutDataType)
{
	// Case-sensitive on purpose: the schema's dataType is a closed enum, and
	// accepting "Float32" here would let a manifest through that the Python
	// reference rejects.
	if (Name.Equals(TEXT("float16"), ESearchCase::CaseSensitive)) { OutDataType = ECFDVizDataType::Float16; return true; }
	if (Name.Equals(TEXT("float32"), ESearchCase::CaseSensitive)) { OutDataType = ECFDVizDataType::Float32; return true; }
	if (Name.Equals(TEXT("uint8"),   ESearchCase::CaseSensitive)) { OutDataType = ECFDVizDataType::UInt8;   return true; }

	// Recognised so the caller can say "float64 storage is not supported in 1.0"
	// instead of "unknown dataType". It still has to be rejected for CVF storage.
	if (Name.Equals(TEXT("float64"), ESearchCase::CaseSensitive)) { OutDataType = ECFDVizDataType::Float64; return true; }

	return false;
}

bool TryParseAssociation(const FString& Name, ECFDVizAssociation& OutAssociation)
{
	if (Name.Equals(TEXT("cell"),  ESearchCase::CaseSensitive)) { OutAssociation = ECFDVizAssociation::Cell;  return true; }
	if (Name.Equals(TEXT("point"), ESearchCase::CaseSensitive)) { OutAssociation = ECFDVizAssociation::Point; return true; }

	// mesh-vertex, mesh-element, integration-point, face and particle are
	// reserved for a future version and MUST be rejected in 1.0 (section 3.2),
	// so they deliberately fall through to false here.
	return false;
}

bool TryParseCodec(const FString& Name, ECFDVizCodec& OutCodec)
{
	if (Name.Equals(TEXT("none"), ESearchCase::CaseSensitive)) { OutCodec = ECFDVizCodec::None; return true; }
	if (Name.Equals(TEXT("lz4"),  ESearchCase::CaseSensitive)) { OutCodec = ECFDVizCodec::LZ4;  return true; }
	if (Name.Equals(TEXT("zlib"), ESearchCase::CaseSensitive)) { OutCodec = ECFDVizCodec::Zlib; return true; }

	// Parsed, not accepted: the caller must reject it with ZstdRejectionMessage,
	// which it cannot do if parsing reports "unknown codec" instead.
	if (Name.Equals(TEXT("zstd"), ESearchCase::CaseSensitive)) { OutCodec = ECFDVizCodec::Zstd; return true; }

	return false;
}

bool TryDataTypeFromCode(uint8 Code, ECFDVizDataType& OutDataType)
{
	switch (Code)
	{
	case 1: OutDataType = ECFDVizDataType::Float16; return true;
	case 2: OutDataType = ECFDVizDataType::Float32; return true;
	case 3: OutDataType = ECFDVizDataType::UInt8;   return true;
	case 4: OutDataType = ECFDVizDataType::Float64; return true;
	default: return false;
	}
}

bool TryAssociationFromCode(uint8 Code, ECFDVizAssociation& OutAssociation)
{
	switch (Code)
	{
	case 0: OutAssociation = ECFDVizAssociation::Cell;  return true;
	case 1: OutAssociation = ECFDVizAssociation::Point; return true;
	default: return false;
	}
}

bool TryCodecFromCode(uint8 Code, ECFDVizCodec& OutCodec)
{
	switch (Code)
	{
	case 0: OutCodec = ECFDVizCodec::None; return true;
	case 1: OutCodec = ECFDVizCodec::Zstd; return true;
	case 2: OutCodec = ECFDVizCodec::LZ4;  return true;
	case 3: OutCodec = ECFDVizCodec::Zlib; return true;
	default: return false;
	}
}

/* -------------------------------------------------------------------------- */
/* CFDViz namespace helpers                                                     */
/* -------------------------------------------------------------------------- */

namespace CFDViz
{
	bool IsSafeRelativePath(const FString& RelativePath)
	{
		// Mirrors the `relativePath` pattern in cfdviz-1.0.schema.json exactly.
		// The two must agree: a path this accepts and the schema rejects (or the
		// reverse) is a case that loads in one implementation and not the other.
		if (RelativePath.IsEmpty() || RelativePath.Len() > MaxRelativePathLength)
		{
			return false;
		}

		// Leading '/' is an absolute POSIX path.
		if (RelativePath[0] == TEXT('/'))
		{
			return false;
		}

		// A Windows drive prefix such as "C:". Only at position 0, matching the
		// schema's lookahead; a colon later in a name is not a drive letter.
		if (RelativePath.Len() >= 2 && RelativePath[1] == TEXT(':') && FChar::IsAlpha(RelativePath[0]))
		{
			return false;
		}

		for (int32 Index = 0; Index < RelativePath.Len(); ++Index)
		{
			const TCHAR Char = RelativePath[Index];

			// Backslash is a separator on Windows, so "..\\x" would escape the
			// case root on one platform while looking like a filename on another.
			if (Char == TEXT('\\'))
			{
				return false;
			}

			// Control characters, including an embedded NUL.
			if (Char < TEXT(' '))
			{
				return false;
			}
		}

		// Any segment that is exactly "..". Checked segment-wise rather than by
		// searching for the substring, so a legitimate file named "..config" or
		// "a..b" is not rejected - the schema does not reject those either.
		int32 SegmentStart = 0;
		for (int32 Index = 0; Index <= RelativePath.Len(); ++Index)
		{
			const bool bAtEnd = (Index == RelativePath.Len());
			if (!bAtEnd && RelativePath[Index] != TEXT('/'))
			{
				continue;
			}

			const int32 SegmentLength = Index - SegmentStart;
			if (SegmentLength == 2
				&& RelativePath[SegmentStart] == TEXT('.')
				&& RelativePath[SegmentStart + 1] == TEXT('.'))
			{
				return false;
			}
			SegmentStart = Index + 1;
		}

		return true;
	}

	bool TryMultiply(int64 A, int64 B, int64& OutProduct)
	{
		// Sizes here come from untrusted files. Negative inputs are rejected
		// outright rather than sign-analysed: no count, extent or byte length in
		// this format is ever negative, so a negative operand is already corrupt.
		if (A < 0 || B < 0)
		{
			return false;
		}
		if (A != 0 && B > TNumericLimits<int64>::Max() / A)
		{
			return false;
		}
		OutProduct = A * B;
		return true;
	}

	int64 ComputePayloadBytes(int64 ValueCount, int32 ComponentCount, ECFDVizDataType DataType)
	{
		const int32 ElementBytes = SizeOfDataType(DataType);
		if (ValueCount < 0 || ComponentCount <= 0 || ElementBytes <= 0)
		{
			return INDEX_NONE;
		}

		int64 Product = 0;
		if (!TryMultiply(ValueCount, static_cast<int64>(ComponentCount), Product))
		{
			return INDEX_NONE;
		}
		if (!TryMultiply(Product, static_cast<int64>(ElementBytes), Product))
		{
			return INDEX_NONE;
		}
		return Product;
	}
}

/* -------------------------------------------------------------------------- */
/* FCFDVizResult                                                                */
/* -------------------------------------------------------------------------- */

FString FCFDVizResult::ToString() const
{
	if (IsOk())
	{
		return TEXT("ok");
	}

	// Fall back to the enum name so an error raised without a message is still
	// identifiable, rather than rendering as an empty string.
	FString Body = Message.IsEmpty() ? FString(CFDVizErrorToString(Error)) : Message;

	if (!FilePath.IsEmpty())
	{
		Body = FilePath + TEXT(": ") + Body;
	}

	if (ByteOffset != INDEX_NONE)
	{
		Body += FString::Printf(TEXT(" at byte %lld"), static_cast<long long>(ByteOffset));
	}

	return Body;
}

void FCFDVizResult::LogIfFailed() const
{
	if (IsOk())
	{
		return;
	}

	UE_LOG(LogFlowViz, Error, TEXT("CFDViz [%s] %s"), CFDVizErrorToString(Error), *ToString());
}

/* -------------------------------------------------------------------------- */
/* FCFDVizStatistics                                                            */
/* -------------------------------------------------------------------------- */

FString FCFDVizStatistics::ToString() const
{
	if (!bValid)
	{
		return FString::Printf(
			TEXT("no valid data (%lld NaN of %lld values)"),
			static_cast<long long>(NaNCount),
			static_cast<long long>(NaNCount + ValidCount));
	}

	FString Result;
	for (int32 Component = 0; Component < GetComponentCount(); ++Component)
	{
		double Min = 0.0;
		double Max = 0.0;
		if (TryGetComponentRange(Component, Min, Max))
		{
			Result += FString::Printf(TEXT("c%d [%g, %g] "), Component, Min, Max);
		}
		else
		{
			// Surfaced rather than skipped: a component with no valid samples is
			// exactly the case a consumer must not build a colour range from.
			Result += FString::Printf(TEXT("c%d [no valid data] "), Component);
		}
	}

	double MagMin = 0.0;
	double MagMax = 0.0;
	if (TryGetMagnitudeRange(MagMin, MagMax))
	{
		Result += FString::Printf(TEXT("|v| [%g, %g] "), MagMin, MagMax);
	}

	Result += FString::Printf(
		TEXT("valid %lld, NaN %lld"),
		static_cast<long long>(ValidCount),
		static_cast<long long>(NaNCount));

	return Result;
}

/* -------------------------------------------------------------------------- */
/* Coordinate conversion - adapter layer only, never the readers                 */
/* -------------------------------------------------------------------------- */

FMatrix MakeSolverToUnrealTransform(double MetersToUnrealUnits)
{
	// Canonical (right-handed, Z-up, X-forward) to Unreal (left-handed, Z-up,
	// X-forward). Up and forward already match, so the only change is chirality,
	// and negating Y is the minimal correct way to make it: it leaves the
	// forward and up directions a user reasons about untouched.
	//
	// UE matrices are row-vector (Position * M), so the basis vectors are rows.
	const double Scale = MetersToUnrealUnits;
	return FMatrix(
		FVector(Scale, 0.0, 0.0),
		FVector(0.0, -Scale, 0.0),
		FVector(0.0, 0.0, Scale),
		FVector::ZeroVector);
}

FMatrix MakeUnrealToSolverTransform(double MetersToUnrealUnits)
{
	// Written out rather than obtained from FMatrix::Inverse, which substitutes
	// the identity for a near-singular matrix. Silently returning the identity
	// from a coordinate conversion would place data in the wrong units with no
	// indication that anything happened.
	if (MetersToUnrealUnits == 0.0 || !FMath::IsFinite(MetersToUnrealUnits))
	{
		UE_LOG(LogFlowViz, Error, TEXT("MakeUnrealToSolverTransform: invalid length scale %g; returning identity. Validate units.length when parsing the manifest."), MetersToUnrealUnits);
		return FMatrix::Identity;
	}

	const double InverseScale = 1.0 / MetersToUnrealUnits;
	return FMatrix(
		FVector(InverseScale, 0.0, 0.0),
		FVector(0.0, -InverseScale, 0.0),
		FVector(0.0, 0.0, InverseScale),
		FVector::ZeroVector);
}

FVector SolverToUnrealPosition(const FVector& SolverPosition, double MetersToUnrealUnits)
{
	// Written directly instead of building a matrix: this is called per vertex
	// and per glyph, and the arithmetic is the whole transform anyway.
	return FVector(
		SolverPosition.X * MetersToUnrealUnits,
		-SolverPosition.Y * MetersToUnrealUnits,
		SolverPosition.Z * MetersToUnrealUnits);
}

FVector3f SolverToUnrealDirection(const FVector3f& SolverVector)
{
	// No unit scale: a velocity stays in the solver's units (engineering rule 4).
	return FVector3f(SolverVector.X, -SolverVector.Y, SolverVector.Z);
}

FVector3f SolverToUnrealPseudoVector(const FVector3f& SolverVector)
{
	// A cross product picks up det(M) = -1 under the mirror, so the result is
	// the true-vector transform negated: (x, -y, z) * -1 = (-x, y, -z).
	return FVector3f(-SolverVector.X, SolverVector.Y, -SolverVector.Z);
}

bool TransformReversesWinding(const FMatrix& Transform)
{
	return Transform.Determinant() < 0.0;
}

bool TryMakeMatrixFromRowMajorArray(TArrayView<const double> RowMajor16, FMatrix& OutMatrix)
{
	if (RowMajor16.Num() != 16)
	{
		return false;
	}

	for (int32 Index = 0; Index < 16; ++Index)
	{
		// A NaN or infinity here would silently poison every position it touches
		// and show up much later as missing geometry, so it is rejected at the
		// boundary where the manifest is still nameable in the error.
		if (!FMath::IsFinite(RowMajor16[Index]))
		{
			return false;
		}
	}

	// Transpose. The manifest stores the mathematical column-vector convention
	// (p' = A*p, translation in the last column, flat indices 3/7/11); FMatrix
	// uses the row-vector convention (p' = p*M, translation in the last row).
	// A straight copy would look correct for pure rotations and misplace every
	// translation.
	FMatrix Result;
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Column = 0; Column < 4; ++Column)
		{
			Result.M[Column][Row] = RowMajor16[Row * 4 + Column];
		}
	}

	OutMatrix = Result;
	return true;
}
