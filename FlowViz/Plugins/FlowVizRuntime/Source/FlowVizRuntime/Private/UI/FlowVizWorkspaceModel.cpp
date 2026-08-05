// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceModel.h"

#include "CFDViz/CFDVizManifest.h"
#include "Misc/Paths.h"
#include "Render/FlowVizVolumeTexture.h"

/**
 * See FlowVizWorkspaceModel.h.
 *
 * THE ONE JOB OF THIS FILE IS PROPAGATION, AND IT IS EASY TO GET SILENTLY
 * WRONG. Opening a case has to reach five view models. Four of the five report
 * an error if they are handed something bad - but a view model that is never
 * touched at all reports nothing, because its defaults are legal. The clip and
 * slice models both default to a unit domain, so forgetting to set one leaves a
 * fully functional slider calibrated for a 1x1x1 box. Nothing throws; the slice
 * simply lands somewhere else. FlowViz.UI.WorkspaceModel.OpenCase asserts the
 * domain is the case's real size rather than merely present, because "present"
 * is what an unconfigured model already reports.
 */

namespace FlowVizWorkspaceModelLocal
{
	/** Accept either a `.cfdviz` directory or the manifest.json inside it, matching UCFDVizVolumeComponent::LoadCase. */
	FString ResolveManifestPath(const FString& CaseDirectory)
	{
		return FPaths::GetCleanFilename(CaseDirectory).EndsWith(TEXT(".json"))
			? CaseDirectory
			: FPaths::Combine(CaseDirectory, TEXT("manifest.json"));
	}

	/**
	 * True when this field is a grid's declared mask, or calls itself one.
	 *
	 * THE SAME TEST UCFDVizVolumeComponent::LoadCase USES. A mask is a 0/1
	 * volume: displaying it by default renders a solid block, which reads as a
	 * broken transfer function rather than as the wrong field being shown.
	 */
	bool IsMaskField(const FCFDVizCase& Case, const FCFDVizField& Field)
	{
		const FCFDVizGridDescriptor* Grid = Case.FindGridForField(Field);
		return (Grid != nullptr && Grid->MaskFieldId == Field.Id)
			|| Field.Semantic.Equals(TEXT("mask"), ESearchCase::IgnoreCase);
	}
}

FFlowVizWorkspaceModel::FFlowVizWorkspaceModel() = default;

FFlowVizWorkspaceModel::~FFlowVizWorkspaceModel()
{
	// Unbind BEFORE the members start dying. Declaration order already makes the
	// timeline die before the player, but stating it here means a future
	// reordering of the struct is a no-op rather than a shutdown use-after-free.
	Timeline.Unbind();
}

bool FFlowVizWorkspaceModel::IsCaseOpen() const
{
	return Player.IsOpen();
}

const FCFDVizCase* FFlowVizWorkspaceModel::GetCase() const
{
	return Player.GetCase();
}

void FFlowVizWorkspaceModel::CloseCase()
{
	// Unbind the borrowed pointer first: Close() waits on outstanding decodes,
	// and a view model pointing at a player mid-teardown is a window where a
	// panel's attribute could read through it.
	Timeline.Unbind();
	Player.Close();
	TransferFunction.Unbind();
	Probes.RemoveAllProbes();
	Clip.RemoveAllPlanes();

	// Released AFTER Player.Close(), which waits on outstanding decodes - those
	// workers read the case, so dropping the last reference first would free it
	// underneath them.
	SharedCase.Reset();
}

TArray<FName> FFlowVizWorkspaceModel::GetVolumeFieldIds() const
{
	TArray<FName> Result;

	const FCFDVizCase* OpenCase = GetCase();
	if (OpenCase == nullptr)
	{
		return Result;
	}

	for (const FCFDVizField& Field : OpenCase->Fields)
	{
		// Masks are excluded for the reason in IsMaskField: offering one in a
		// field picker invites the user to select a field that renders as a solid
		// block. It is still loadable by name through SetField for diagnostics.
		if (!FlowVizWorkspaceModelLocal::IsMaskField(*OpenCase, Field))
		{
			Result.Add(Field.Id);
		}
	}

	return Result;
}

FCFDVizResult FFlowVizWorkspaceModel::OpenCase(const FString& CaseDirectory, FName FieldId)
{
	const FString ManifestPath = FlowVizWorkspaceModelLocal::ResolveManifestPath(CaseDirectory);

	// Parsed into a local first. A failure must leave the workspace on its
	// PREVIOUS case rather than on a half-built new one - a panel showing a mix
	// of two cases is worse than a panel showing a stale one, because only the
	// second is obvious.
	FCFDVizCase Parsed;
	const FCFDVizResult LoadResult = FCFDVizCase::LoadFromFile(ManifestPath, Parsed);
	if (!LoadResult.IsOk())
	{
		return LoadResult;
	}

	const FCFDVizResult CodecResult = Parsed.CheckCodecSupport();
	if (!CodecResult.IsOk())
	{
		// Refused HERE rather than at the first decode. A case whose codec this
		// build cannot read would otherwise open, bind every panel, and then show
		// an empty volume with the controls all live - rule 15's failure mode.
		return CodecResult;
	}

	/* --- Pick the field ---------------------------------------------------- */

	const FCFDVizField* Field = nullptr;
	if (!FieldId.IsNone())
	{
		Field = Parsed.FindField(FieldId);
		if (Field == nullptr)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::InvalidManifest,
				FString::Printf(TEXT("case declares no field '%s'"), *FieldId.ToString()),
				ManifestPath);
		}
	}
	else
	{
		for (const FCFDVizField& Candidate : Parsed.Fields)
		{
			if (!FlowVizWorkspaceModelLocal::IsMaskField(Parsed, Candidate))
			{
				Field = &Candidate;
				break;
			}
		}
		if (Field == nullptr)
		{
			return FCFDVizResult::Fail(
				ECFDVizError::InvalidManifest,
				TEXT("case declares no field that is not a mask; nothing to display"),
				ManifestPath);
		}
	}

	const FCFDVizGridDescriptor* Grid = Parsed.FindGridForField(*Field);
	if (Grid == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(TEXT("field '%s' names grid '%s', which the case does not declare"),
				*Field->Id.ToString(), *Field->GridId.ToString()),
			ManifestPath);
	}

	/* --- Commit ------------------------------------------------------------ */

	// Everything below this line has already been validated, so the workspace is
	// not left half-configured by a failure partway through.
	const FName ChosenField = Field->Id;

	CloseCase();

	const TSharedRef<const FCFDVizCase> Shared = MakeShared<const FCFDVizCase>(MoveTemp(Parsed));
	const FCFDVizResult OpenResult = Player.Open(Shared, ChosenField);
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	// Held so SetField can re-open the same case without re-reading the manifest.
	SharedCase = Shared;

	Timeline.BindPlayer(&Player);

	// THE DOMAIN, TO EVERY MODEL THAT HAS ONE. Derived once, from the shared
	// transform helper, rather than multiplied out per call site: two independent
	// spacing-times-dimensions expressions are two chances to differ, and a clip
	// box and a slice slider disagreeing about the domain is invisible until
	// someone measures.
	FFlowVizVolumeTransform Transform;
	Transform.Grid = Grid->Geometry;
	Transform.Association = Field->Association;
	const FVector PhysicalSize = Transform.GetPhysicalSize();

	// Reported rather than ignored: a degenerate domain would make every
	// normalised crop fraction a division by zero, and both view models refuse
	// it. Capturing the first failure means the caller learns why the clip panel
	// is inert instead of discovering it by clicking.
	FCFDVizResult FirstFailure = FCFDVizResult::Ok();

	const FCFDVizResult ClipDomainResult = Clip.SetDomainSize(PhysicalSize);
	if (!ClipDomainResult.IsOk() && FirstFailure.IsOk())
	{
		FirstFailure = ClipDomainResult;
	}

	const FCFDVizResult SliceDomainResult = Slice.SetDomainSize(PhysicalSize);
	if (!SliceDomainResult.IsOk() && FirstFailure.IsOk())
	{
		FirstFailure = SliceDomainResult;
	}

	// A fresh slice sits at the domain's centre rather than at the origin corner,
	// where it would be coplanar with a face and look like it had not appeared.
	Slice.CenterOnDomain();

	/* --- Unit scale for probes --------------------------------------------- */

	// units.length, NOT an assumption of metres. ADR 004 section 5: a
	// millimetre-scale case would otherwise report probe positions a thousand
	// times wrong while still looking like positions.
	double MetersPerUnit = 1.0;
	if (Shared->Units.TryGetLengthInMeters(MetersPerUnit))
	{
		const FCFDVizResult ScaleResult =
			Probes.SetMetersToUnrealUnits(MetersPerUnit * CFDViz::MetersToUnrealCentimeters);
		if (!ScaleResult.IsOk() && FirstFailure.IsOk())
		{
			FirstFailure = ScaleResult;
		}
	}
	else if (FirstFailure.IsOk())
	{
		// TryGetLengthInMeters returning false means "ask, or refuse" - never
		// "assume 1". Reported so the workspace can say so rather than probing at
		// a silently wrong scale.
		FirstFailure = FCFDVizResult::Fail(
			ECFDVizError::InvalidManifest,
			FString::Printf(
				TEXT("case declares length unit '%s', which this build has no metre scale for; "
					 "probe positions would be at an unknown scale"),
				*Shared->Units.Length),
			ManifestPath);
	}

	/* --- Colouring ---------------------------------------------------------- */

	const FCFDVizResult BindResult = TransferFunction.BindField(*Shared, ChosenField);
	if (!BindResult.IsOk() && FirstFailure.IsOk())
	{
		FirstFailure = BindResult;
	}

	return FirstFailure;
}

FCFDVizResult FFlowVizWorkspaceModel::SetField(FName FieldId)
{
	const FCFDVizCase* OpenCase = GetCase();
	if (OpenCase == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("no case is open, so there is no field to select"));
	}

	if (OpenCase->FindField(FieldId) == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("the open case declares no field '%s'"), *FieldId.ToString()));
	}

	// Re-open on the SAME shared case rather than re-parsing: the manifest has
	// not changed, and re-reading it would be game-thread file I/O (rule 1).
	if (!SharedCase.IsValid())
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("no parsed case is held, so the field cannot be re-bound"));
	}
	const TSharedRef<const FCFDVizCase> Shared = SharedCase.ToSharedRef();

	const FCFDVizResult OpenResult = Player.Open(Shared, FieldId);
	if (!OpenResult.IsOk())
	{
		return OpenResult;
	}

	// THE COLOURING MUST FOLLOW THE FIELD. Leaving the previous field's range in
	// place would colour pressure against a velocity domain: every pixel would be
	// clamped to an end of the map, which looks like saturated data rather than
	// like the wrong range.
	return TransferFunction.BindField(*Shared, FieldId);
}
