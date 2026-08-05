// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceModel.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The workspace model: does opening a case actually configure every panel's
 * view model (plan.md section 5F)?
 *
 * WHAT MAKES THIS WORTH TESTING SEPARATELY FROM THE VIEW MODELS. Each view
 * model is already tested in isolation, and each would pass its own suite while
 * being left completely unconfigured by the thing that opens a case. The defect
 * this file is aimed at is the one the plugin has already shipped once, in the
 * render layer: two well-tested halves and nothing joining them.
 *
 * Concretely, the failure mode is a domain size that reaches the clip view
 * model but not the slice one. Both view models default to a unit domain, so
 * the slice panel's slider still works - it is simply calibrated for a 1x1x1
 * box instead of the case's real extent. Nothing errors; the slice just lands
 * in the wrong place. That is invisible to FlowViz.UI.SliceViewModel.*, which
 * sets its own domain as a precondition.
 *
 * SO THE ASSERTIONS BELOW ARE DELIBERATELY ABOUT PROPAGATION, not about the
 * behaviour of any one view model. They check that after OpenCase:
 *
 *  1. The timeline is bound to THIS workspace's player - not left null, and not
 *     bound to some other player.
 *  2. The transfer function is bound to the field that was actually opened.
 *  3. Every view model that has a domain has the SAME domain, and it is the
 *     case's real physical size rather than the unit default.
 *  4. Closing unbinds, so a stale case cannot be driven.
 */

// NAMED namespace: FlowVizRuntime is a unity build, so anonymous namespaces from
// every .cpp merge and same-named helpers collide across test files.
namespace FlowVizWorkspaceModelTest
{
	FString GetSampleCaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceModelTest,
	"FlowViz.UI.WorkspaceModel.OpenCase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceModelTest::RunTest(const FString& Parameters)
{
	const FString CaseDir = FlowVizWorkspaceModelTest::GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		// A SKIP IS A FAILURE HERE. Silently returning true would report Success
		// for a test that verified nothing, which is exactly the "green total
		// hiding a skip" this project has been bitten by.
		AddError(FString::Printf(
			TEXT("the sample case is required for this test and was not found at '%s'"), *CaseDir));
		return false;
	}

	/* == Before opening, nothing is bound =================================== */
	{
		FFlowVizWorkspaceModel Workspace;
		TestFalse(TEXT("a fresh workspace has no case open"), Workspace.IsCaseOpen());
		TestNull(TEXT("a fresh workspace has no case"), Workspace.GetCase());
		TestFalse(
			TEXT("a fresh workspace's transfer function is unbound"),
			Workspace.TransferFunction.IsBound());
		TestEqual(
			TEXT("a fresh workspace has no volume fields to list"),
			Workspace.GetVolumeFieldIds().Num(),
			0);
	}

	/* == Opening binds every view model ===================================== */
	{
		FFlowVizWorkspaceModel Workspace;
		const FCFDVizResult Result = Workspace.OpenCase(CaseDir);
		if (!TestTrue(
				*FString::Printf(TEXT("opening the sample case succeeds: %s"), *Result.ToString()),
				Result.IsOk()))
		{
			return false;
		}

		TestTrue(TEXT("the case is open"), Workspace.IsCaseOpen());
		TestNotNull(TEXT("the open case is reachable"), Workspace.GetCase());

		// 1. THE TIMELINE IS BOUND TO THIS WORKSPACE'S PLAYER. Asserting the exact
		// pointer, not merely IsBound(): a workspace that bound the timeline to
		// some other player would be "bound" and would drive the wrong case.
		TestTrue(TEXT("the timeline view model is bound"), Workspace.Timeline.IsBound());
		TestEqual(
			TEXT("the timeline is bound to THIS workspace's player, not another"),
			Workspace.Timeline.GetPlayer(),
			&Workspace.Player);

		// The sample case is multi-frame, so the transport must actually be usable.
		// Without this, a workspace that opened a case but left the player closed
		// would still pass the binding assertion above.
		TestTrue(
			TEXT("the bound timeline reports frames, so the transport is operable"),
			Workspace.Timeline.HasFrames());
		TestTrue(
			TEXT("the sample case has more than one frame, so play is enabled"),
			Workspace.Timeline.CanPlay());

		// 2. THE TRANSFER FUNCTION IS BOUND TO THE FIELD THAT WAS OPENED.
		TestTrue(
			TEXT("the transfer function is bound to a field"),
			Workspace.TransferFunction.IsBound());
		TestEqual(
			TEXT("the transfer function is bound to the same field the player is playing"),
			Workspace.TransferFunction.GetFieldId(),
			Workspace.Player.GetFieldId());

		// 3. THE DOMAIN REACHED BOTH VIEW MODELS THAT HAVE ONE, AND IT IS REAL.
		//
		// This is the assertion the whole file exists for. Both view models default
		// to FVector::OneVector, so a propagation failure leaves a WORKING slider
		// calibrated to a 1x1x1 box - no error anywhere.
		TestTrue(TEXT("the clip view model has a domain"), Workspace.Clip.HasDomain());
		TestTrue(TEXT("the slice view model has a domain"), Workspace.Slice.HasDomain());

		const FVector ClipDomain = Workspace.Clip.GetDomainSize();
		const FVector SliceDomain = Workspace.Slice.GetDomainSize();

		TestTrue(
			*FString::Printf(
				TEXT("the clip and slice view models share one domain; clip (%g, %g, %g) vs "
					 "slice (%g, %g, %g)"),
				ClipDomain.X, ClipDomain.Y, ClipDomain.Z,
				SliceDomain.X, SliceDomain.Y, SliceDomain.Z),
			ClipDomain.Equals(SliceDomain, UE_KINDA_SMALL_NUMBER));

		// NOT THE DEFAULT. A domain of exactly (1,1,1) is what an unconfigured view
		// model reports, so a test that only compared the two to each other would
		// pass on a workspace that set neither. The sample case is 2 m x 1 m x 0.5 m
		// and cannot be the unit cube.
		TestFalse(
			*FString::Printf(
				TEXT("the domain is the case's real size, not the unconfigured unit default; "
					 "got (%g, %g, %g)"),
				ClipDomain.X, ClipDomain.Y, ClipDomain.Z),
			ClipDomain.Equals(FVector::OneVector, UE_KINDA_SMALL_NUMBER));

		TestTrue(
			TEXT("the domain is positive on every axis"),
			ClipDomain.X > 0.0 && ClipDomain.Y > 0.0 && ClipDomain.Z > 0.0);

		// The probe view model's unit scale must come from the case's units.length
		// rather than being left at the default, or every probe placed by clicking
		// reports a position from the wrong cell on a non-metre case.
		TestTrue(
			TEXT("the probe view model has a positive unit scale"),
			Workspace.Probes.GetMetersToUnrealUnits() > 0.0);

		/* == Field enumeration is real ====================================== */
		const TArray<FName> Fields = Workspace.GetVolumeFieldIds();
		TestTrue(
			*FString::Printf(
				TEXT("the open case lists at least one volume field; listed %d"), Fields.Num()),
			Fields.Num() > 0);
		TestTrue(
			TEXT("the field the player opened is among the listed fields"),
			Fields.Contains(Workspace.Player.GetFieldId()));

		/* == Closing unbinds ================================================ */
		Workspace.CloseCase();
		TestFalse(TEXT("closing leaves no case open"), Workspace.IsCaseOpen());
		TestFalse(
			TEXT("closing unbinds the transfer function, so no stale range survives"),
			Workspace.TransferFunction.IsBound());
		TestFalse(
			TEXT("closing leaves the timeline with no frames to drive"),
			Workspace.Timeline.HasFrames());
	}

	/* == Switching fields re-binds the colouring ============================ */
	{
		FFlowVizWorkspaceModel Workspace;
		if (!Workspace.OpenCase(CaseDir).IsOk())
		{
			AddError(TEXT("could not open the sample case for the field-switch check"));
			return false;
		}

		const TArray<FName> Fields = Workspace.GetVolumeFieldIds();
		// Find a field that is NOT the one already bound, so the switch is a real
		// change rather than a no-op that would pass trivially.
		FName Other = NAME_None;
		for (const FName Candidate : Fields)
		{
			if (Candidate != Workspace.TransferFunction.GetFieldId())
			{
				Other = Candidate;
				break;
			}
		}

		if (Other.IsNone())
		{
			// Report rather than skip: if the sample ever drops to one field this
			// check stops meaning anything and someone must notice.
			AddError(TEXT("the sample case declares only one volume field, so the field-switch "
						  "assertion cannot distinguish a working switch from a no-op"));
			return false;
		}

		const FCFDVizResult SwitchResult = Workspace.SetField(Other);
		TestTrue(
			*FString::Printf(TEXT("switching to field '%s' succeeds: %s"),
				*Other.ToString(), *SwitchResult.ToString()),
			SwitchResult.IsOk());
		TestEqual(
			TEXT("the transfer function follows the field switch, so colouring is not left on "
				 "the previous field's range"),
			Workspace.TransferFunction.GetFieldId(),
			Other);
		TestEqual(
			TEXT("the player follows the field switch too"),
			Workspace.Player.GetFieldId(),
			Other);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
