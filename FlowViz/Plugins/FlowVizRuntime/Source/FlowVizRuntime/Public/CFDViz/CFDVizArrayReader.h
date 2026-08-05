// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizByteSource.h"
#include "CFDViz/CFDVizPayload.h"
#include "CFDViz/CFDVizTypes.h"

/**
 * CVA reader - mesh-associated arrays (format section 6).
 *
 * One .cva holds one array at one frame: a value per mesh vertex or per mesh
 * element of a companion .cvm, with 1, 3, 6 or 9 components. CVA is specified
 * and round-trip tested in 1.0 even though no 1.0 UI renders every result type -
 * the point is that the on-disk contract is frozen and provable now, so an FEA
 * result written today still reads when the renderer catches up.
 *
 * SHARED HEADER PREFIX. Bytes [0,24) mean exactly what they mean in a CVM header,
 * and headerCrc32c sits at [80,84) in both. Section 6.1 calls both choices
 * deliberate and says they "must not be tidied", so one routine can sniff and
 * validate either container. This reader depends on that and on nothing else
 * about CVM.
 *
 * TENSOR COMPONENT ORDER. A 6-component symmetric tensor is stored
 * XX, YY, ZZ, XY, YZ, XZ. The other common Voigt order (XX, YY, ZZ, YZ, XZ, XY)
 * is, in the spec's words, "the classic way for two solvers to silently
 * disagree" - the numbers all look plausible either way, so nothing catches it
 * downstream. GetComponentNames below is the single place that order is written.
 *
 * No UObject, no UWorld, no game-thread work.
 */

namespace CFDViz
{
	/** CVA flag bits (section 6.2). */
	namespace CvaFlags
	{
		inline constexpr uint32 FrameStatistics = 1u << 0;
		inline constexpr uint32 GlobalStatistics = 1u << 1;

		/** Any bit outside this mask means a feature this reader does not implement, so the file is rejected. */
		inline constexpr uint32 Known = FrameStatistics | GlobalStatistics;
	}

	/** 1 scalar, 3 vector, 6 symmetric tensor, 9 full tensor (section 6). */
	FLOWVIZRUNTIME_API bool IsValidCvaComponentCount(int32 ComponentCount);

	/**
	 * Normative component labels for a CVA component count.
	 *
	 * 6 returns the symmetric-tensor order XX, YY, ZZ, XY, YZ, XZ; 9 returns
	 * row-major XX, XY, XZ, YX, YY, YZ, ZX, ZY, ZZ. Returns an empty view for an
	 * unsupported count.
	 */
	FLOWVIZRUNTIME_API TArrayView<const TCHAR* const> GetCvaComponentNames(int32 ComponentCount);
}

/**
 * One statistics section (section 6.4), `8 + 32 * componentCount` bytes.
 *
 * Statistics are float64 regardless of the payload's storage type: they are
 * metadata, and widening keeps a float16 array's extremes exactly representable
 * for 32 bytes per component.
 *
 * A component with no valid samples stores Minimum = +inf and Maximum = -inf -
 * the mandated "no valid data" sentinel (section 4.4.7) - and Mean = NaN. Those
 * are sentinels, NOT a range; building a colour scale from +inf..-inf yields an
 * inverted, meaningless axis. Go through TryGetComponentRange.
 */
struct FCFDVizArrayStatistics
{
	/** Offset 0. Entities these statistics cover. */
	int64 ValueCount = 0;

	/** Offset 8. +inf where a component has no valid sample. */
	TArray<double> Minimum;
	/** Offset 8 + 8C. -inf where a component has no valid sample. */
	TArray<double> Maximum;
	/** Offset 8 + 16C. NaN where a component has no valid sample. */
	TArray<double> Mean;
	/** Offset 8 + 24C. Non-NaN entries per component, so a partly invalid field still reports honestly. */
	TArray<uint64> ValidCount;

	int32 GetComponentCount() const { return Minimum.Num(); }

	/** Serialized size for a component count. */
	static int64 GetSectionBytes(int32 ComponentCount)
	{
		return 8 + 32 * static_cast<int64>(ComponentCount);
	}

	/**
	 * Usable range of one component.
	 *
	 * @return false - leaving the outputs untouched - when the index is out of
	 *         range or that component holds the +inf/-inf "no valid data"
	 *         sentinel. False means "do not build a range", never "use zero".
	 */
	bool TryGetComponentRange(int32 Component, double& OutMin, double& OutMax) const
	{
		if (!Minimum.IsValidIndex(Component) || !Maximum.IsValidIndex(Component))
		{
			return false;
		}
		const double Min = Minimum[Component];
		const double Max = Maximum[Component];
		if (!(Min <= Max) || !FMath::IsFinite(Min) || !FMath::IsFinite(Max))
		{
			return false;
		}
		OutMin = Min;
		OutMax = Max;
		return true;
	}
};

/** The fixed 96-byte CVA header (section 6.1). */
struct FCFDVizArrayHeader
{
	/** Offsets 8 / 10. */
	uint16 MajorVersion = 0;
	uint16 MinorVersion = 0;
	/** Offset 16. */
	uint32 Flags = 0;
	/** Offset 20. Must be 96. */
	uint32 HeaderBytes = 0;
	/** Offset 24. */
	uint32 FrameIndex = 0;
	/** Offset 28. */
	uint32 FieldNumericId = 0;
	/** Offset 32. */
	double SimulationTime = 0.0;
	/** Offset 40. Vertices or elements, per Association. */
	int64 ValueCount = 0;
	/** Offset 48. 1, 3, 6 or 9. */
	int32 ComponentCount = 0;
	/** Offset 49. float16, float32 or float64 - NOT uint8, which is reserved here for CVF. */
	ECFDVizDataType DataType = ECFDVizDataType::Float32;
	/** Offset 50. 0 = mesh-element, 1 = mesh-vertex, matching CVF's element-0 / nodal-1 convention. */
	ECFDVizAssociation Association = ECFDVizAssociation::Cell;
	/** Offset 51. */
	ECFDVizCodec Codec = ECFDVizCodec::None;
	/** Offset 52. Over the COMPRESSED bytes as stored. */
	uint32 PayloadCrc32C = 0;
	/** Offset 56. */
	int64 PayloadOffset = 0;
	/** Offset 64. */
	int64 CompressedBytes = 0;
	/** Offset 72. Checked against valueCount * componentCount * sizeof(dataType) before allocating. */
	int64 UncompressedBytes = 0;
	/** Offset 80. CRC-32C over [0,96) with [80,84) zeroed - byte-for-byte CVM's rule. */
	uint32 HeaderCrc32C = 0;
	/** Offset 88. 0 when no statistics are stored. */
	int64 StatisticsOffset = 0;

	bool HasFrameStatistics() const { return (Flags & CFDViz::CvaFlags::FrameStatistics) != 0; }
	bool HasGlobalStatistics() const { return (Flags & CFDViz::CvaFlags::GlobalStatistics) != 0; }

	/** valueCount * componentCount * sizeof(dataType), or INDEX_NONE on overflow. */
	int64 GetExpectedUncompressedBytes() const
	{
		return CFDViz::ComputePayloadBytes(ValueCount, ComponentCount, DataType);
	}
};

/**
 * A decoded CVA array.
 *
 * Values keep their on-disk storage type in Bytes rather than being widened, so
 * a float16 payload hashes identically to what the writer emitted and a NaN
 * survives bit-exactly. Use TryGetValue to widen one value on demand.
 */
struct FCFDVizArrayData
{
	FCFDVizArrayHeader Header;

	/** Decoded payload, components interleaved per entity: v0.x, v0.y, v0.z, v1.x, ... */
	TArray<uint8> Bytes;

	/** Present when flag bit 0 is set. */
	TOptional<FCFDVizArrayStatistics> FrameStatistics;
	/** Present when flag bit 1 is set. */
	TOptional<FCFDVizArrayStatistics> GlobalStatistics;

	/** Element index of (entity, component), in elements not bytes. INDEX_NONE when out of range. */
	int64 GetElementIndex(int64 Entity, int32 Component) const
	{
		if (Entity < 0 || Entity >= Header.ValueCount || Component < 0 || Component >= Header.ComponentCount)
		{
			return INDEX_NONE;
		}
		return Entity * static_cast<int64>(Header.ComponentCount) + static_cast<int64>(Component);
	}

	/** One component of one entity, widened to double. NaN is preserved, not coerced. */
	bool TryGetValue(int64 Entity, int32 Component, double& OutValue) const
	{
		const int64 Index = GetElementIndex(Entity, Component);
		return Index != INDEX_NONE && CFDViz::TryReadValueAsDouble(Bytes, Index, Header.DataType, OutValue);
	}

	/** Raw stored bit pattern - what the section 9 bridge compares, and the only NaN-safe check. */
	bool TryGetValueBits(int64 Entity, int32 Component, uint64& OutBits) const
	{
		const int64 Index = GetElementIndex(Entity, Component);
		return Index != INDEX_NONE && CFDViz::TryReadValueBits(Bytes, Index, Header.DataType, OutBits);
	}
};

/** Reads a CVA file. */
class FLOWVIZRUNTIME_API FCFDVizArrayReader
{
public:
	/** Parse a header from exactly 96 bytes, without the rest of the file. */
	static FCFDVizResult ParseHeader(
		TArrayView<const uint8> HeaderBytes,
		const FString& DisplayPath,
		FCFDVizArrayHeader& OutHeader);

	/**
	 * Read header, payload and any statistics sections.
	 *
	 * @param bVerifyCrc false only for a validator reporting corruption rather
	 *                   than stopping at it. A normal read leaves it true.
	 */
	static FCFDVizResult Read(
		const ICFDVizByteSource& Source,
		FCFDVizArrayData& OutArray,
		bool bVerifyCrc = true);
};
