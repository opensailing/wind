// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceModel.h"

#include "CFDViz/CFDVizManifest.h"
#include "Misc/Paths.h"
#include "Render/FlowVizVolumeTexture.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizSession.h"

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

/* ========================================================================== */
/* Sessions                                                                    */
/* ========================================================================== */

FCFDVizResult FFlowVizWorkspaceModel::SaveSession(const FString& FilePath) const
{
	FFlowVizSessionState State;

	/*
	 * THE PLAYER IS PASSED EVEN WHEN NO CASE IS OPEN, and that is deliberate
	 * rather than sloppy. CaptureFromViewModels reads Settings and PhysicalTime
	 * unconditionally but only reaches for the field id and case path when
	 * IsOpen(), so a workspace with no case yields a session that carries the
	 * colour and playback setup with an EMPTY case path. Refusing to save that
	 * would make "set up my colours, then pick a case" unexpressible.
	 */
	FlowVizSession::CaptureFromViewModels(
		&Player, &TransferFunction, &Clip, &Slice, &Probes, &RenderSettings, State);

	return FlowVizSession::SaveToFile(State, FilePath);
}

FCFDVizResult FFlowVizWorkspaceModel::LoadSession(const FString& FilePath)
{
	FFlowVizSessionState State;
	const FCFDVizResult Read = FlowVizSession::LoadFromFile(FilePath, State);
	if (!Read.IsOk())
	{
		// A read failure is total - there is no state to apply. Distinct from a
		// MISSING CASE, which LoadFromFile reports as Ok with bCaseFound false
		// and which LoadState handles below.
		return Read;
	}

	return LoadState(State);
}

FCFDVizResult FFlowVizWorkspaceModel::LoadState(const FFlowVizSessionState& State)
{
	FCFDVizResult FirstFailure = FCFDVizResult::Ok();
	auto Record = [&FirstFailure](const FCFDVizResult& Result)
	{
		if (!Result.IsOk() && FirstFailure.IsOk())
		{
			FirstFailure = Result;
		}
	};

	/* --- The case, FIRST ---------------------------------------------------- */

	/*
	 * READ THE HEADER'S NOTE ON ORDER BEFORE CHANGING ANYTHING HERE. Opening
	 * resets playback settings, seeks to frame 0, re-binds the transfer function
	 * and RESETS THE CROP BOX. Every one of those would silently undo an apply
	 * that had already run, and none of them reports an error while doing it.
	 */
	if (!State.ResolvedCasePath.IsEmpty())
	{
		if (State.bCaseFound)
		{
			// The session's OWN field, not NAME_None. Passing NAME_None would open
			// the first non-mask field and then let ApplyToViewModels colour it
			// with the saved field's transfer function - a scene that looks
			// restored and is showing different data.
			Record(OpenCase(State.ResolvedCasePath, State.FieldId));
		}
		else
		{
			/*
			 * REPORTED, BUT THE REST STILL APPLIES. plan.md section 14 requires a
			 * relink to be possible, and a relink is only worth offering if the
			 * user still has the setup they would relink INTO. So the colour map,
			 * clip planes, slice and probes are applied against whatever case is
			 * currently open - possibly none - and the caller learns the case was
			 * missing from this result.
			 */
			Record(FCFDVizResult::Fail(
				ECFDVizError::FileNotFound,
				TEXT("the session's case was not found; everything else was applied, and the "
					 "session can be relinked"),
				State.ResolvedCasePath));
		}
	}

	/* --- Then everything else ----------------------------------------------- */

	// Best-effort by contract: this reports the FIRST failure after applying
	// what it could, so its result is recorded rather than returned.
	Record(FlowVizSession::ApplyToViewModels(
		State, &Player, &TransferFunction, &Clip, &Slice, &Probes, &RenderSettings));

	return FirstFailure;
}

/* ========================================================================== */
/* The channel to the renderer                                                 */
/* ========================================================================== */

bool FFlowVizWorkspaceModel::PushClipToVolume(
	const FFlowVizClipViewModel& Source, UCFDVizVolumeComponent* Volume)
{
	if (Volume == nullptr)
	{
		return false;
	}

	// THE VOLUME'S OWN EXTENT, re-read every time. See the header: a domain
	// carried from the source model is right until a second case is loaded into
	// the same actor, and then it clips at a plausible wrong place rather than
	// failing.
	const FVector PhysicalSize = Volume->GetPhysicalSize();
	if (PhysicalSize.X <= 0.0 || PhysicalSize.Y <= 0.0 || PhysicalSize.Z <= 0.0)
	{
		// NO CASE BOUND, so there is no extent to normalise the crop against.
		// Refused rather than defaulted: a unit domain would make every crop
		// fraction wrong by the case's real size, and SetDomainSize would refuse a
		// zero axis anyway - this way the caller learns nothing happened instead
		// of half-happening.
		return false;
	}

	// COPIED, NOT ALIASED. The component keeps its own model and publishes it to
	// the render thread; handing it a reference to the UI's model would put a
	// game-thread-mutated object behind a render-thread read.
	FFlowVizClipViewModel Pushed = Source;

	/*
	 * THE ORDER HERE IS LOAD-BEARING, AND THE OBVIOUS ORDER IS WRONG.
	 *
	 * SetDomainSize calls ResetCropBox - deliberately, because a crop authored
	 * for a 10 m domain means something entirely different in a 0.1 m one. So
	 * "set the domain, then copy the planes" silently discards the crop the user
	 * dragged, on EVERY push, and this pushes on every edit: drag a crop, touch
	 * any other control, watch the crop snap back to full with nothing reporting
	 * an error.
	 *
	 * So the crop is captured BEFORE the domain is imposed and restored after,
	 * and only when the source had a domain of its own to have authored it
	 * against. A crop from a model with no domain is not a crop, it is a default.
	 */
	const bool bSourceHadDomain = Source.HasDomain();
	const bool bSourceWasCropped = bSourceHadDomain && Source.IsCropActive();
	const FVector SourceCropMin = Source.GetCropMin();
	const FVector SourceCropMax = Source.GetCropMax();

	if (!Pushed.SetDomainSize(PhysicalSize).IsOk())
	{
		// Already screened by the positive-extent check above, so this is the
		// "something changed underneath us" branch rather than an expected one.
		// Refused whole: a model with the wrong domain clips at the wrong place.
		return false;
	}

	if (bSourceWasCropped)
	{
		// A crop authored against a DIFFERENT domain than the volume's is refused
		// by SetCropBox only if it is degenerate, not if it is merely out of
		// scale. Restoring it unchanged is still right: the numbers are in solver
		// units, which is what the crop boxes in the panel are labelled in, and
		// SetCropBox deliberately does not clamp into the domain.
		Pushed.SetCropBox(SourceCropMin, SourceCropMax);
	}

	// REPLACED, NOT MERGED. The component's clip state is the UI's, wholesale:
	// planes removed in the panel must disappear from the render, and an
	// append-only push would leave the last plane clipping forever.
	Volume->SetClip(Pushed);
	return true;
}

bool FFlowVizWorkspaceModel::PushTransferFunctionToVolume(
	const FFlowVizTransferFunctionViewModel& Source, UCFDVizVolumeComponent* Volume)
{
	if (Volume == nullptr)
	{
		return false;
	}

	// NO EXTENT GUARD, DELIBERATELY. See the header: colours map values, not
	// positions, so there is nothing to normalise against the volume's geometry
	// and no reason to refuse a volume with no case bound yet.
	//
	// COPIED, NOT ALIASED, for the same reason as the clip push: the component
	// republishes this to the render thread, and the view model owns an opacity
	// curve with an array behind it.
	Volume->SetTransferFunction(Source);
	return true;
}

bool FFlowVizWorkspaceModel::PushRenderSettingsToVolume(
	const FFlowVizRenderSettingsViewModel& Source, UCFDVizVolumeComponent* Volume)
{
	if (Volume == nullptr)
	{
		return false;
	}

	// NO EXTENT GUARD, for the reason the header gives: render settings are
	// value state, meaningful with no case open. COPIED, NOT ALIASED --
	// SetRenderSettings takes a const& and stores by value, so the component's
	// copy crosses to the render thread while the UI keeps mutating this one.
	Volume->SetRenderSettings(Source);
	return true;
}
