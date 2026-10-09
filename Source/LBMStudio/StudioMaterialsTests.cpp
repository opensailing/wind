#include "StudioMaterials.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags MaterialFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FStudioGeometryAsset MaterialGeometry()
{
    FStudioGeometryAsset Asset;Asset.Name=TEXT("Material assignment fixture");
    Asset.SourcePath=FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()/TEXT("tmp/debug/materials-tests/wing.stl"));
    Asset.SourceSHA256=FString::ChrN(64,TEXT('a'));return Asset;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMaterialDraftValues,"Studio.Materials.ExactUnitsUnknownAndRejectedDrafts",MaterialFlags)
bool FStudioMaterialDraftValues::RunTest(const FString&)
{
    FStudioMaterial Material;Material.Name=TEXT("Authored fluid");Material.Density=1.23456789012345;Material.KinematicViscosity=1.567890123456789e-5;
    FStudioMaterialEdit Draft;Draft.Reset(Material);FStudioMaterial Built;
    TestFalse(TEXT("Initial draft is clean"),Draft.IsDirty());
    TestTrue(TEXT("Density unit conversion"),Draft.ChangeUnit(EStudioMaterialProperty::Density,1));
    TestTrue(TEXT("Viscosity unit conversion"),Draft.ChangeUnit(EStudioMaterialProperty::KinematicViscosity,1));
    TestFalse(TEXT("Presentation unit changes do not dirty material"),Draft.IsDirty());
    Draft.Name=TEXT("Renamed fluid");TestTrue(TEXT("Rename builds without rounding properties"),Draft.Build(Built));
    TestEqual(TEXT("Stored density stays bit-exact"),Built.Density.GetValue(),Material.Density.GetValue());
    TestEqual(TEXT("Stored viscosity stays bit-exact"),Built.KinematicViscosity.GetValue(),Material.KinematicViscosity.GetValue());
    TestFalse(TEXT("Unknown conductivity remains unknown"),Built.ThermalConductivity.IsSet());
    for(int32 I=0;I<20;++I)
    {TestTrue(TEXT("Density display unit returns"),Draft.ChangeUnit(EStudioMaterialProperty::Density,I%2));}
    TestTrue(TEXT("Repeated display conversions build"),Draft.Build(Built));
    TestEqual(TEXT("Repeated conversions never change saved density"),Built.Density.GetValue(),Material.Density.GetValue());
    Draft.Values[0]=TEXT("0.0012");Draft.Units[0]=1;
    Draft.Values[1]=TEXT("15");Draft.Units[1]=1;
    TestTrue(TEXT("Authored units convert to SI"),Draft.Build(Built));
    TestTrue(TEXT("g/cm3 conversion"),FMath::Abs(Built.Density.GetValue()-1.2)<1.e-14);
    TestTrue(TEXT("mm2/s conversion"),FMath::Abs(Built.KinematicViscosity.GetValue()-15.e-6)<1.e-19);
    const auto Kept=Built;
    for(const TCHAR* Text:{TEXT("nan"),TEXT("inf"),TEXT("-1"),TEXT("0"),TEXT("1e99"),TEXT("12x")})
    {
        Draft.Values[0]=Text;
        TestFalse(TEXT("Invalid property rejected"),Draft.Build(Built));
        TestEqual(TEXT("Rejected build leaves output unchanged"),Built.Density.GetValue(),Kept.Density.GetValue());
        TestEqual(TEXT("Rejected text is retained"),Draft.Values[0],FString(Text));
        TestEqual(TEXT("Rejected property is identified"),Draft.ErrorProperty,0);
        TestFalse(TEXT("Invalid text cannot be silently converted"),Draft.ChangeUnit(EStudioMaterialProperty::Density,0));
        TestEqual(TEXT("Rejected conversion retains previous unit"),Draft.Units[0],1);
    }
    Draft.Values[0]=TEXT(" ");TestTrue(TEXT("Blank clears optional property"),Draft.Build(Built));
    TestFalse(TEXT("Cleared density is unknown, not zero"),Built.Density.IsSet());
    Draft.Reset(Material);FStudioMaterial Changed=Material;Changed.Density=2.;
    TestFalse(TEXT("External edit invalidates the saved draft base"),Draft.Matches(Changed));
    TestTrue(TEXT("Unrelated case changes do not invalidate material"),Draft.Matches(Material));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMaterialAssignments,"Studio.Materials.TransactionalAssignmentsAndFrozenRuns",MaterialFlags)
bool FStudioMaterialAssignments::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectSavedDir()/TEXT("Automation/MaterialsModel")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir);M.Pause();const auto Camera=M.Project.Camera;const int32 Frame=M.SelectedFrame;const auto RenderRevision=M.RenderIntentRevision;
    FGuid Fluid,Solid;
    TestTrue(TEXT("Add unknown fluid"),M.AddMaterial(false,Fluid));
    TestTrue(TEXT("Add unknown solid"),M.AddMaterial(true,Solid));
    TestFalse(TEXT("Fluid properties are not invented"),M.Project.Draft.Materials[0].Density.IsSet());
    auto Geometry=MaterialGeometry();
    TestTrue(TEXT("Add explicit case object fixture"),M.EditCase(TEXT("Assignment fixture"),[Geometry](auto& Case){Case.Geometry.Add(Geometry);}));
    TestTrue(TEXT("Assign fluid domain"),M.AssignDomainMaterial(Fluid));
    TestTrue(TEXT("Assign solid object"),M.AssignGeometryMaterial(Geometry.Id,Solid));
    const FString BeforeInvalid=StudioCaseIO::Serialize(M.Project.Draft);
    TestFalse(TEXT("Solid cannot replace domain fluid"),M.AssignDomainMaterial(Solid));
    TestFalse(TEXT("Unknown material cannot be assigned"),M.AssignGeometryMaterial(Geometry.Id,FGuid::NewGuid()));
    TestEqual(TEXT("Rejected assignments are transactional"),StudioCaseIO::Serialize(M.Project.Draft),BeforeInvalid);
    // A captured harness configuration carries its explicit adapter identifier.
    TestTrue(TEXT("Select harness contract for frozen fixture"),M.EditCase(TEXT("Harness fixture"),[](auto& Case){Case.Setup.BackendId=TEXT("test-control-harness");}));
    M.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Frozen material configuration"),M.Project.Draft,EStudioRunOrigin::ControlHarness));
    const FString Frozen=StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration());
    FStudioMaterial Update=M.Project.Draft.Materials[0];Update.Name=TEXT("Known fluid");Update.Density=1.2;Update.KinematicViscosity=1.5e-5;
    TestTrue(TEXT("Apply entered properties"),M.UpdateMaterial(Update));
    TestFalse(TEXT("Assigned fluid cannot become solid implicitly"),[&]{auto Invalid=Update;Invalid.bSolid=true;return M.UpdateMaterial(Invalid);}());
    FGuid Copy;TestTrue(TEXT("Duplicate exact material"),M.DuplicateMaterial(Fluid,Copy));
    TestEqual(TEXT("Duplicate receives no assignment"),StudioMaterials::AssignmentCount(M.Project.Draft,Copy),0);
    TestTrue(TEXT("Delete unassigned duplicate"),M.DeleteMaterial(Copy,false));
    TestFalse(TEXT("Assigned deletion requires explicit unassignment"),M.DeleteMaterial(Fluid,false));
    TestTrue(TEXT("Atomic unassign and delete"),M.DeleteMaterial(Fluid,true));
    TestFalse(TEXT("Domain reference cleared"),M.Project.Draft.Domain.FluidMaterialId.IsValid());
    TestTrue(TEXT("Deletion is one undo operation"),M.UndoCase());
    TestEqual(TEXT("Undo restores the stable assignment"),M.Project.Draft.Domain.FluidMaterialId,Fluid);
    const auto* Restored=M.Project.Draft.Materials.FindByPredicate([Fluid](const auto& Item){return Item.Id==Fluid;});
    TestTrue(TEXT("Undo restores exact properties"),Restored&&Restored->Density.IsSet()&&Restored->Density.GetValue()==1.2);
    TestTrue(TEXT("Material project saves"),M.SaveProject(Dir/TEXT("materials.lbms")));
    TestEqual(TEXT("No new CFD requested"),M.RenderIntentRevision,RenderRevision);
    TestEqual(TEXT("Source frame unaffected"),M.SelectedFrame,Frame);
    TestTrue(TEXT("Camera unaffected"),StudioView::CameraEquals(M.Project.Camera,Camera));
    TestEqual(TEXT("Existing run material configuration is unchanged"),StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration()),Frozen);
    const FString Saved=StudioCaseIO::Serialize(M.Project.Draft);
    TestTrue(TEXT("Material project reopens"),M.LoadProject(Dir/TEXT("materials.lbms")));
    TestEqual(TEXT("Properties, IDs and assignments reopen exactly"),StudioCaseIO::Serialize(M.Project.Draft),Saved);
    TestFalse(TEXT("Old project case history is cleared"),M.CanUndoCase());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMaterialLimits,"Studio.Materials.LimitsAndMissingIdentity",MaterialFlags)
bool FStudioMaterialLimits::RunTest(const FString&)
{
    FStudioCaseDraft Case;FString Error;FGuid Missing=FGuid::NewGuid();
    const FString Before=StudioCaseIO::Serialize(Case);
    TestFalse(TEXT("Missing deletion preserves case"),StudioMaterials::Remove(Case,Missing,true,Error));
    TestEqual(TEXT("No unrelated state mutation"),StudioCaseIO::Serialize(Case),Before);
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/MaterialLimits")/FGuid::NewGuid().ToString());
    TestTrue(TEXT("Materials workspace is a real route"),M.Navigate(EStudioWorkspace::Materials));
    TestTrue(TEXT("Fill document material limit"),M.EditCase(TEXT("Material limit fixture"),[](auto& D){for(int32 I=0;I<256;++I){FStudioMaterial Material;Material.Name=FString::Printf(TEXT("Material %d"),I);D.Materials.Add(Material);}}));
    FGuid Kept=Missing;
    TestFalse(TEXT("Add cannot exceed bounded material count"),M.AddMaterial(false,Kept));
    TestEqual(TEXT("Failed add does not publish identity"),Kept,Missing);
    TestFalse(TEXT("Duplicate cannot exceed bounded material count"),M.DuplicateMaterial(M.Project.Draft.Materials[0].Id,Kept));
    TestEqual(TEXT("Material count stays bounded"),M.Project.Draft.Materials.Num(),256);
    return true;
}
#endif
