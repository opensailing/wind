// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceModel.h"

#include "CFDViz/CFDVizManifest.h"
#include "Misc/Paths.h"
#include "Render/FlowVizVolumeTexture.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizSession.h"

#include "CFDViz/CFDVizPayload.h"
#include "CFDViz/CFDVizVolumeReader.h"
#include "Flow/FlowVizChartSeries.h"
#include "Flow/FlowVizCutPlane.h"
#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"
#include "Tasks/Task.h"

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

	/*
	 * VISIBLE, Z-NORMAL, BY DEFAULT (renderer overhaul P3). The z-mid cut
	 * plane is the genre's default picture for this data -- the honest hero
	 * image on a quasi-2D case (research doc section 5 item 3) -- so a case
	 * OPENS showing it rather than hiding the one view everyone expects
	 * behind a toggle. The volume slab consumer keys off the same flag and
	 * composes only when the volume mode draws, so this costs the default
	 * picture nothing.
	 */
	Slice.SetAxisPreset(EFlowVizSliceAxis::Z);
	Slice.SetVisible(true);

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

	// The profile's owner (#83): before this, bPresentationMode round-tripped
	// through the JSON with nothing to read or write it.
	State.bPresentationMode = bPresentationMode;

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

	/*
	 * THE PROFILE FLAG, WITHOUT THE BUNDLE. SetPresentationMode would stomp
	 * the lighting and jitter the session just restored -- the saved settings
	 * are the user's tuned state, not the bundle's entry point. The flag is
	 * adopted; the settings speak for themselves.
	 */
	bPresentationMode = State.bPresentationMode;

	return FirstFailure;
}

void FFlowVizWorkspaceModel::SetPresentationMode(bool bInPresentation)
{
	bPresentationMode = bInPresentation;

	if (bInPresentation)
	{
		// The Presentation bundle: lit, jittered. The values are the view
		// model's own defaults for the terms -- only the toggles move, so a
		// user's tuned ambient survives a profile round trip.
		RenderSettings.SetLightingEnabled(true);
		RenderSettings.SetJitterEnabled(true);
	}
	else
	{
		// The Scientific bundle: VISUAL_QA rule 1 (lighting must not modulate
		// apparent scalar value) and ADR 002 (jitter trades banding for
		// shimmer). These SET rather than toggle: entering Scientific must
		// land on the quantitative-honesty state whatever was tuned before.
		RenderSettings.SetLightingEnabled(false);
		RenderSettings.SetJitterEnabled(false);
	}
}

/* ========================================================================== */
/* The sampling service (#75)                                                  */
/* ========================================================================== */

/**
 * Shared with the worker task, exactly the shape of FFlowVizCasePlayer's
 * FLoadQueue and for the same reason: the task must be able to outlive the
 * model (a workspace closing mid-sample is ordinary), so everything it touches
 * is copied or shared, never a pointer back in.
 */
struct FFlowVizWorkspaceModel::FSampleQueue
{
	struct FProbeResult
	{
		FGuid Id;
		FFlowVizProbeReading Reading;
	};

	struct FResult
	{
		TArray<FProbeResult> Probes;
		bool bHasFrameRange = false;
		float FrameMin = 0.0f;
		float FrameMax = 0.0f;

		/** The line probe's distance series (#85), when a line was set. */
		bool bHasLineSeries = false;
		FFlowVizChartSeries LineSeries;

		/** The cut plane's mesh (renderer overhaul P3), when the slice is visible. */
		bool bHasCutPlane = false;
		FFlowVizMeshSection CutPlane;
		double CutPlaneRangeMin = 0.0;
		double CutPlaneRangeMax = 0.0;
	};

	FCriticalSection Mutex;
	TArray<FResult> Results;
	int32 PendingCount = 0;

	void Push(FResult&& Result)
	{
		FScopeLock Lock(&Mutex);
		Results.Add(MoveTemp(Result));
		--PendingCount;
	}

	bool HasResults()
	{
		FScopeLock Lock(&Mutex);
		return Results.Num() > 0;
	}

	void Drain(TArray<FResult>& OutResults)
	{
		FScopeLock Lock(&Mutex);
		OutResults = MoveTemp(Results);
		Results.Reset();
	}

	int32 GetPendingCount()
	{
		FScopeLock Lock(&Mutex);
		return PendingCount;
	}
};

void FFlowVizWorkspaceModel::RequestSampleUpdate()
{
	if (!Player.IsOpen() || !SharedCase.IsValid())
	{
		return;
	}

	if (!SampleQueue.IsValid())
	{
		SampleQueue = MakeShared<FSampleQueue, ESPMode::ThreadSafe>();
	}

	{
		FScopeLock Lock(&SampleQueue->Mutex);
		if (SampleQueue->PendingCount > 0)
		{
			// One in flight is enough: the newest display state wins, and a
			// queue of stale requests would apply readings for frames the
			// display has already left.
			return;
		}
		++SampleQueue->PendingCount;
	}

	// EVERYTHING COPIED OR SHARED. The probe list, the frame, the field and the
	// component selection are all captured by value; the case rides a shared ref.
	TSharedPtr<FSampleQueue, ESPMode::ThreadSafe> Queue = SampleQueue;
	TSharedPtr<const FCFDVizCase> CaseRef = SharedCase;
	const FName SampleFieldId = Player.GetFieldId();
	// The DISPLAYED frame, not the playhead's target: readings must describe
	// what is on screen. INDEX_NONE (nothing resident yet) falls back to the
	// nearest wanted frame, which at open time is frame 0.
	const FFlowVizDisplaySelection& Display = Player.GetDisplay();
	const int32 FrameIndex = Display.FrameA != INDEX_NONE ? Display.FrameA : 0;
	const EFlowVizComponentChoice RangeComponent = TransferFunction.GetComponent();

	TArray<FSampleQueue::FProbeResult> Requests;
	for (const FFlowVizProbe& Probe : Probes.GetProbes())
	{
		if (Probe.bVisible)
		{
			FSampleQueue::FProbeResult& Request = Requests.AddDefaulted_GetRef();
			Request.Id = Probe.Id;
			Request.Reading.SampledSolverPosition = Probe.SolverPosition;
		}
	}

	// The line probe's geometry, copied by value for the worker (#85).
	const bool bHasLine = Probes.HasLineProbe();
	const FVector LineStart = Probes.GetLineStart();
	const FVector LineEnd = Probes.GetLineEnd();
	const int32 LineSamples = Probes.GetLineSampleCount();
	const EFlowVizLineProbeAxis LineAxis = Probes.GetLineAxisMode();

	// The cut plane's request (renderer overhaul P3): the slice view model's
	// plane, copied by value. Built only when the slice is visible AND has a
	// domain -- an invisible slice costs nothing.
	const bool bWantCutPlane = Slice.IsVisible() && Slice.HasDomain();
	FlowVizCutPlane::FCutPlaneRequest CutRequest;
	if (bWantCutPlane)
	{
		CutRequest.Origin = Slice.GetOrigin();
		CutRequest.Normal = Slice.GetNormal();
		CutRequest.DomainSize = Slice.GetDomainSize();
	}

	UE::Tasks::Launch(TEXT("FlowVizWorkspaceSample"),
		[Queue, CaseRef, SampleFieldId, FrameIndex, RangeComponent,
			bHasLine, LineStart, LineEnd, LineSamples, LineAxis,
			bWantCutPlane, CutRequest,
			Requests = MoveTemp(Requests)]() mutable
		{
			FSampleQueue::FResult Result;

			// DISK I/O AND ZLIB, ON A WORKER -- the whole reason this is a task
			// (engineering rule 1).
			for (FSampleQueue::FProbeResult& Request : Requests)
			{
				const FVector Position = Request.Reading.SampledSolverPosition;
				FlowVizProbe::SampleStoredField(
					*CaseRef, SampleFieldId, FrameIndex, Position, Request.Reading);
				Result.Probes.Add(MoveTemp(Request));
			}

			/*
			 * THE FRAME RANGE, measured over the frame's own decoded voxels
			 * under the SAME component selection the transfer function
			 * displays. Brick-directory statistics would avoid the decode, but
			 * they are per-component extremes: for Magnitude they can only
			 * bound, not measure, and a colour scale stretched to a bound it
			 * never reaches wastes its resolution. Skips non-finite values --
			 * the shader rejects them too, so the range covers exactly what is
			 * coloured.
			 */
			const FCFDVizField* Field = CaseRef->FindField(SampleFieldId);
			FString FramePath;
			if (Field != nullptr
				&& CaseRef->ResolveFieldFramePath(*Field, FrameIndex, FramePath).IsOk())
			{
				FCFDVizVolumeReader Reader;
				TArray<uint8> Dense;
				if (Reader.Open(FramePath).IsOk() && Reader.ReadDense(Dense).IsOk())
				{
					const int32 Components = FMath::Max(1, Field->ComponentCount);
					const FIntVector Extent = Reader.GetHeader().GetValueCounts();
					const int64 ValueCount =
						static_cast<int64>(Extent.X) * Extent.Y * Extent.Z;

					float MinSeen = TNumericLimits<float>::Max();
					float MaxSeen = TNumericLimits<float>::Lowest();

					for (int64 Value = 0; Value < ValueCount; ++Value)
					{
						double Scalar = 0.0;
						if (RangeComponent == EFlowVizComponentChoice::Magnitude
							&& Components > 1)
						{
							double SumSquares = 0.0;
							bool bAllFinite = true;
							for (int32 C = 0; C < Components; ++C)
							{
								double Part = 0.0;
								if (!CFDViz::TryReadValueAsDouble(Dense,
										Value * Components + C, Field->DataType, Part)
									|| !FMath::IsFinite(Part))
								{
									bAllFinite = false;
									break;
								}
								SumSquares += Part * Part;
							}
							if (!bAllFinite)
							{
								continue;
							}
							Scalar = FMath::Sqrt(SumSquares);
						}
						else
						{
							// X/Y/Z/W read that component; Magnitude of a scalar
							// field IS the component.
							int32 C = 0;
							switch (RangeComponent)
							{
								case EFlowVizComponentChoice::Y: C = 1; break;
								case EFlowVizComponentChoice::Z: C = 2; break;
								case EFlowVizComponentChoice::W: C = 3; break;
								default: C = 0; break;
							}
							if (C >= Components)
							{
								C = 0;
							}
							if (!CFDViz::TryReadValueAsDouble(Dense,
									Value * Components + C, Field->DataType, Scalar)
								|| !FMath::IsFinite(Scalar))
							{
								continue;
							}
						}
						const float AsFloat = static_cast<float>(Scalar);
						MinSeen = FMath::Min(MinSeen, AsFloat);
						MaxSeen = FMath::Max(MaxSeen, AsFloat);
					}

					// max > min is SetCurrentFrameRange's own precondition; a
					// constant frame yields an unusable range and stays
					// unreported rather than fabricating a width.
					if (MaxSeen > MinSeen)
					{
						Result.bHasFrameRange = true;
						Result.FrameMin = MinSeen;
						Result.FrameMax = MaxSeen;
					}
				}
			}

			/*
			 * THE LINE SERIES (#85), from a field sampler over the same frame.
			 * Built AFTER the probes and the range so a sampler build failure
			 * costs only the chart, not the readings. The chart samples the
			 * DISPLAYED field -- the numbers under the plot are the numbers on
			 * screen.
			 */
			if (bHasLine)
			{
				FFlowVizFieldSampler LineSampler;
				if (LineSampler.Build(*CaseRef, SampleFieldId, FrameIndex).IsOk())
				{
					Result.bHasLineSeries = FlowVizChart::BuildLineSeries(
						LineSampler, LineStart, LineEnd, LineSamples, LineAxis,
						Result.LineSeries);
				}
			}

			/*
			 * THE CUT PLANE (renderer overhaul P3), same worker, same frame,
			 * same displayed field -- the plane in the viewport is the data on
			 * the timeline, never a neighbouring frame's. Built after the
			 * probes so a sampler failure costs only the plane.
			 */
			if (bWantCutPlane)
			{
				FFlowVizFieldSampler PlaneSampler;
				if (PlaneSampler.Build(*CaseRef, SampleFieldId, FrameIndex).IsOk())
				{
					FFlowVizFieldMask PlaneMask;
					PlaneMask.Build(PlaneSampler);
					Result.bHasCutPlane = FlowVizCutPlane::BuildCutPlaneMesh(
						PlaneSampler, PlaneMask, CutRequest, Result.CutPlane,
						Result.CutPlaneRangeMin, Result.CutPlaneRangeMax);
				}
			}

			Queue->Push(MoveTemp(Result));
		});
}

bool FFlowVizWorkspaceModel::DrainSampleResults()
{
	if (!SampleQueue.IsValid() || !SampleQueue->HasResults())
	{
		return false;
	}

	TArray<FSampleQueue::FResult> Results;
	SampleQueue->Drain(Results);

	bool bApplied = false;
	for (FSampleQueue::FResult& Result : Results)
	{
		for (FSampleQueue::FProbeResult& Probe : Result.Probes)
		{
			// A probe deleted while the sample ran returns false here; that is
			// the answer, not an error.
			if (Probes.SetProbeReading(Probe.Id, Probe.Reading))
			{
				bApplied = true;
			}
		}
		if (Result.bHasFrameRange)
		{
			// SetCurrentFrangeRange refuses non-finite and empty ranges; the
			// worker guaranteed max > min, so a refusal here means the range
			// crossed a NaN boundary in transit -- refused, not clamped.
			if (TransferFunction.SetCurrentFrameRange(Result.FrameMin, Result.FrameMax).IsOk())
			{
				bApplied = true;
			}
		}
		if (Result.bHasCutPlane)
		{
			// Stored on the model like the line series: a measurement with no
			// view-model owner, applied by the workspace's tick to the actor's
			// cut-plane component.
			CutPlanePayload.Sections.Reset();
			CutPlanePayload.Sections.Add(MoveTemp(Result.CutPlane));
			CutPlaneRangeMin = Result.CutPlaneRangeMin;
			CutPlaneRangeMax = Result.CutPlaneRangeMax;
			bCutPlaneFresh = true;
		}

		if (Result.bHasLineSeries)
		{
			LineSeries = MoveTemp(Result.LineSeries);
			bApplied = true;
		}
	}
	return bApplied;
}

bool FFlowVizWorkspaceModel::WaitForPendingSamples(double TimeoutSeconds)
{
	if (!SampleQueue.IsValid())
	{
		return true;
	}
	const double StartTime = FPlatformTime::Seconds();
	while (SampleQueue->GetPendingCount() > 0)
	{
		if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			return false;
		}
		FPlatformProcess::Sleep(0.001f);
	}
	return true;
}

/* ========================================================================== */
/* The channel to the renderer                                                 */
/* ========================================================================== */

FFlowVizClipViewModel FFlowVizWorkspaceModel::ComposeClipWithSlice(
	const FFlowVizClipViewModel& Clip, const FFlowVizSliceViewModel& Slice)
{
	// The user's model rides whole: planes, crop, domain. COPIED -- the
	// composition must never write back into the model the panel edits.
	FFlowVizClipViewModel Composed = Clip;

	// BOTH switches: on screen, and explicitly slabbing the volume (see the
	// view model's two-consumers comment).
	if (!Slice.IsVisible() || !Slice.IsVolumeSlabEnabled() || !Slice.HasDomain())
	{
		return Composed;
	}

	TArray<FFlowVizClipPlane> SlabPlanes;
	if (Slice.MakeSlabPlanes(SlabPlanes) != 2)
	{
		return Composed;
	}

	// THE USER'S PLANES WIN THE BUDGET. AddPlane refuses past MaxClipPlanes;
	// adding the slab first would make the USER'S next plane the one refused,
	// with the refusal surfacing in a different panel than the cause. Checked
	// up front so the slab is all-or-nothing: one slab plane without its
	// opposite keeps half the domain, which reads as a broken clip rather than
	// a full slice budget.
	if (Composed.GetPlaneCount() + SlabPlanes.Num() > FlowVizRayMarch::MaxClipPlanes)
	{
		return Composed;
	}

	for (const FFlowVizClipPlane& Plane : SlabPlanes)
	{
		// Cannot fail after the budget check: MakeSlabPlanes normalises and a
		// slab from a valid slice is non-degenerate. The result is still read,
		// because "cannot fail" is a claim about today's code.
		if (!Composed.AddPlane(Plane).IsOk())
		{
			return Clip;
		}
	}
	return Composed;
}

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
