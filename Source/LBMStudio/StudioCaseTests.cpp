#include "StudioCase.h"
#include "StudioModel.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    FStudioCaseDraft AuthoredCase()
    {
        FStudioCaseDraft D; D.Name=TEXT("Wing setup");
        FStudioMaterial Fluid; Fluid.Name=TEXT("Test fluid"); Fluid.Density=1.2; Fluid.KinematicViscosity=1.5e-5;
        D.Materials.Add(Fluid); D.Domain.FluidMaterialId=Fluid.Id;
        FStudioGeometryAsset Wing; Wing.Name=TEXT("Wing section");
        Wing.SourcePath=FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()/TEXT("tmp/debug/case-tests/assets/wing.stl"));
        Wing.SourceSHA256=FString::ChrN(64,TEXT('a')); Wing.MetersPerSourceUnit=.001;
        Wing.Translation=FVector(.2,-.1,.03); Wing.Rotation=FRotator(12,34,56).Quaternion();
        FStudioSurfacePatch Patch; Patch.Name=TEXT("Wing wall"); Wing.Patches.Add(Patch); D.Geometry.Add(Wing);
        FStudioBoundaryCondition Inlet; Inlet.Name=TEXT("Inlet"); Inlet.TargetId=D.Domain.Faces[0];
        Inlet.Type=EStudioBoundaryType::VelocityInlet; Inlet.Velocity=FVector(1,0,0); D.Boundaries.Add(Inlet);
        FStudioBoundaryCondition Wall; Wall.Name=TEXT("Wing wall"); Wall.TargetId=Patch.Id;
        Wall.Type=EStudioBoundaryType::NoSlip; D.Boundaries.Add(Wall);
        D.Setup.BackendId=TEXT("test-control-harness"); D.Setup.InletVelocity=FVector(1,0,0);
        D.Setup.ReferenceLength=.5; D.Setup.OutletPressure=0.; D.Setup.TimeStep=.00001;
        return D;
    }
    FString JSONText(const TSharedRef<FJsonObject>& O)
    { FString Text; FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text)); return Text; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCasePersistence,"Studio.Case.ReferencesAndFrozenRuns",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioCasePersistence::RunTest(const FString&)
{
    FStudioProject P; P.Draft=AuthoredCase(); P.SelectedFrame=315;
    P.Runs.Add(FStudioRunRecord::Capture(TEXT("Configuration snapshot"),P.Draft,EStudioRunOrigin::ControlHarness));
    const auto Snapshot=P.Runs.Last();
    P.Draft.Setup.InletVelocity=FVector(9,0,0);
    P.Draft.Materials[0].Density=5.; P.Draft.Geometry[0].Patches[0].Name=TEXT("Edited wall");
    TestEqual(TEXT("Frozen inlet is a deep copy"),Snapshot.GetConfiguration()->Setup.InletVelocity.GetValue(),FVector(1,0,0));
    TestEqual(TEXT("Frozen material remains unchanged"),Snapshot.GetConfiguration()->Materials[0].Density.GetValue(),1.2);
    TestEqual(TEXT("Frozen nested patch remains unchanged"),Snapshot.GetConfiguration()->Geometry[0].Patches[0].Name,FString(TEXT("Wing wall")));
    FStudioProject Loaded; FString Error;
    TestTrue(TEXT("Authored project and runs round trip"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Loaded,Error));
    TestEqual(TEXT("Draft values and all references preserved"),StudioCaseIO::Serialize(Loaded.Draft),StudioCaseIO::Serialize(P.Draft));
    TestEqual(TEXT("Run count preserved"),Loaded.Runs.Num(),2);
    if(Loaded.Runs.Num()==2)
    {
        TestNull(TEXT("Published CFD has no invented setup"),Loaded.Runs[0].GetConfiguration());
        TestEqual(TEXT("Frozen run identity"),Loaded.Runs[1].GetId(),Snapshot.GetId());
        TestTrue(TEXT("Harness origin is explicit"),Loaded.Runs[1].GetOrigin()==EStudioRunOrigin::ControlHarness);
        TestEqual(TEXT("Captured configuration is unchanged on disk"),StudioCaseIO::Serialize(*Loaded.Runs[1].GetConfiguration()),StudioCaseIO::Serialize(*Snapshot.GetConfiguration()));
    }
    TestFalse(TEXT("Unknown Reynolds number remains unset"),Loaded.Draft.Setup.ReynoldsNumber.IsSet());
    TestEqual(TEXT("Recording selection remains independent"),Loaded.SelectedFrame,315);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCaseValidation,"Studio.Case.RejectCorruptReferences",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioCaseValidation::RunTest(const FString&)
{
    const auto Valid=AuthoredCase(); FString Error; FStudioCaseDraft D=Valid;
    TestTrue(TEXT("Incomplete draft is saveable without numerical approval"),StudioCaseIO::Validate(FStudioCaseDraft(),Error));
    D.Materials.Reset(); TestFalse(TEXT("Deleted referenced material rejected"),StudioCaseIO::Validate(D,Error));
    D=Valid; D.Geometry.Reset(); TestFalse(TEXT("Deleted referenced patch rejected"),StudioCaseIO::Validate(D,Error));
    D=Valid; auto Duplicate=D.Boundaries[0]; Duplicate.Id=FGuid::NewGuid(); D.Boundaries.Add(Duplicate);
    TestFalse(TEXT("Two assignments to one target rejected"),StudioCaseIO::Validate(D,Error));
    D=Valid; D.Domain.Faces[1]=D.Domain.Faces[0]; TestFalse(TEXT("Repeated face identity rejected"),StudioCaseIO::Validate(D,Error));
    D=Valid; D.Domain.Min=D.Domain.Max; TestFalse(TEXT("Empty domain rejected"),StudioCaseIO::Validate(D,Error));
    D=Valid; D.Setup.TimeStep=-1.; TestFalse(TEXT("Negative time step rejected"),StudioCaseIO::Validate(D,Error));
    D=Valid; D.Geometry[0].Scale.X=0; TestFalse(TEXT("Collapsed geometry transform rejected"),StudioCaseIO::Validate(D,Error));
    D=Valid; D.Geometry[0].Rotation=FQuat(0,0,0,0); TestFalse(TEXT("Invalid asset orientation rejected"),StudioCaseIO::Validate(D,Error));
    D=Valid; D.Materials[0].Id=D.Id; TestFalse(TEXT("Cross-kind identity collision rejected"),StudioCaseIO::Validate(D,Error));
    auto Malformed=StudioCaseIO::ToJSON(Valid);
    Malformed->GetObjectField(TEXT("setup"))->SetArrayField(TEXT("latticeResolution"),{MakeShared<FJsonValueNumber>(256.5),MakeShared<FJsonValueNumber>(128),MakeShared<FJsonValueNumber>(128)});
    FStudioCaseDraft Kept=Valid; const FString Before=StudioCaseIO::Serialize(Kept);
    TestFalse(TEXT("Fractional lattice dimension rejected"),StudioCaseIO::FromJSON(Malformed,Kept,Error));
    TestEqual(TEXT("Rejected parse preserves destination"),StudioCaseIO::Serialize(Kept),Before);
    auto Run=FStudioRunRecord::Capture(TEXT("Harness"),Valid,EStudioRunOrigin::ControlHarness).ToJSON();
    Run->SetStringField(TEXT("backendId"),TEXT("wrong-backend")); FStudioRunRecord R;
    TestFalse(TEXT("Run and case backend mismatch rejected"),FStudioRunRecord::FromJSON(Run,R,Error));
    TestFalse(TEXT("Invalid record gives an actionable error"),Error.IsEmpty());
    Run->SetStringField(TEXT("origin"),TEXT("recording")); Run->SetStringField(TEXT("backendId"),TEXT("published-recording"));
    Run->SetStringField(TEXT("datasetId"),TEXT("fixture"));
    TestFalse(TEXT("Recording cannot claim authored configuration"),FStudioRunRecord::FromJSON(Run,R,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCaseMigration,"Studio.Case.Version3Migration",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioCaseMigration::RunTest(const FString&)
{
    FStudioProject Old; Old.Name=TEXT("Older project"); Old.SelectedFrame=417;
    Old.Camera.Position=FVector(2,4,6); Old.View.PlaybackRate=.5;
    TSharedPtr<FJsonObject> JSON;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(Old)),JSON);
    JSON->SetNumberField(TEXT("version"),3); JSON->RemoveField(TEXT("draft")); JSON->RemoveField(TEXT("runs"));
    FStudioProject Migrated; FString Error;
    TestTrue(TEXT("Version3 camera project migrates"),StudioProjectIO::Parse(JSONText(JSON.ToSharedRef()),Migrated,Error));
    TestEqual(TEXT("Existing project ID survives migration"),Migrated.Id,Old.Id);
    TestEqual(TEXT("Existing camera survives migration"),Migrated.Camera.Position,Old.Camera.Position);
    TestEqual(TEXT("Existing selection survives migration"),Migrated.SelectedFrame,417);
    TestTrue(TEXT("Migration creates a separate empty case"),Migrated.Draft.Geometry.IsEmpty()&&Migrated.Draft.Materials.IsEmpty());
    TestFalse(TEXT("Migration does not infer recording inlet settings"),Migrated.Draft.Setup.InletVelocity.IsSet());
    TestNull(TEXT("Imported recording keeps unknown configuration"),Migrated.Runs[0].GetConfiguration());
    FStudioProject Reloaded;
    TestTrue(TEXT("Migrated version4 can be saved and read"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Migrated),Reloaded,Error));
    TestEqual(TEXT("New case identity persists after migration"),Reloaded.Draft.Id,Migrated.Draft.Id);
    TestEqual(TEXT("Domain face identity persists"),Reloaded.Draft.Domain.Faces[3],Migrated.Draft.Domain.Faces[3]);
    const FString Before=StudioProjectIO::Serialize(Reloaded);
    JSON->SetNumberField(TEXT("version"),4);
    TestFalse(TEXT("Version4 requires its case data"),StudioProjectIO::Parse(JSONText(JSON.ToSharedRef()),Reloaded,Error));
    TestEqual(TEXT("Malformed replacement is transactional"),StudioProjectIO::Serialize(Reloaded),Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCaseUndo,"Studio.Case.AtomicUndoIndependentOfReplay",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioCaseUndo::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/case-tests")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir); const auto Draft=AuthoredCase();
    TestTrue(TEXT("Initial authoring transaction"),M.EditCase(TEXT("Import setup"),[&Draft](auto& D) { const FGuid Id=D.Id; D=Draft; D.Id=Id; }));
    M.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Harness settings"),M.Project.Draft,EStudioRunOrigin::ControlHarness));
    const FString Frozen=StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration());
    TestTrue(TEXT("Case persists before edits"),M.SaveProject(Dir/TEXT("case.lbms")));
    M.Run(); M.Scrub(.5); M.Project.Camera.Position=FVector(4,5,6);
    const int32 RenderRevision=M.Revision;
    TestTrue(TEXT("Unit of undo captures a multi-field edit"),M.EditCase(TEXT("Set inlet"),[](auto& D) { D.Setup.InletVelocity=FVector(2,0,0); D.Boundaries[0].Velocity=FVector(2,0,0); }));
    TestEqual(TEXT("Undo action is named"),M.UndoCaseLabel(),FString(TEXT("Set inlet")));
    const int64 Revision=M.Project.Draft.Revision;
    TestTrue(TEXT("Undo successful edit"),M.UndoCase());
    TestEqual(TEXT("Undo restores inlet"),M.Project.Draft.Setup.InletVelocity.GetValue(),FVector(1,0,0));
    TestEqual(TEXT("Undo restores related boundary"),M.Project.Draft.Boundaries[0].Velocity.GetValue(),FVector(1,0,0));
    TestTrue(TEXT("Revision is monotonic through undo"),M.Project.Draft.Revision>Revision);
    TestEqual(TEXT("Undo does not rewind selected frame"),M.SelectedFrame,300);
    TestTrue(TEXT("Undo does not stop replay"),M.State==EStudioRunState::Running);
    TestEqual(TEXT("Undo does not touch camera"),M.Project.Camera.Position,FVector(4,5,6));
    TestEqual(TEXT("Case edit cannot request a CFD rebuild"),M.Revision,RenderRevision);
    TestTrue(TEXT("Redo replays the authoring edit"),M.RedoCase());
    TestEqual(TEXT("Redo restores authored inlet"),M.Project.Draft.Setup.InletVelocity.GetValue(),FVector(2,0,0));
    const FString Before=StudioCaseIO::Serialize(M.Project.Draft);
    TestFalse(TEXT("Invalid transaction rejected"),M.EditCase(TEXT("Remove assigned fluid"),[](auto& D) { D.Materials.Reset(); }));
    TestEqual(TEXT("Invalid edit leaves draft untouched"),StudioCaseIO::Serialize(M.Project.Draft),Before);
    TestTrue(TEXT("Undo after invalid edit still targets last valid edit"),M.UndoCase());
    TestTrue(TEXT("No-op is accepted"),M.EditCase(TEXT("No changes"),[](auto&) {}));
    TestTrue(TEXT("No-op preserves redo"),M.CanRedoCase());
    TestTrue(TEXT("New branch edit accepted"),M.EditCase(TEXT("Rename case"),[](auto& D) { D.Name=TEXT("New branch"); }));
    TestFalse(TEXT("New edit discards stale redo"),M.CanRedoCase());
    TestEqual(TEXT("Undo and edits leave submitted configuration unchanged"),StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration()),Frozen);
    for(int32 I=0;I<70;++I) TestTrue(TEXT("Bounded history accepts later edits"),M.EditCase(TEXT("Change steps"),[I](auto& D) { D.Setup.MaxSteps=1000+I; }));
    int32 Count=0; while(M.UndoCase()) ++Count;
    TestEqual(TEXT("Undo retains at most 64 transactions"),Count,64);
    TestTrue(TEXT("Saved case reopens"),M.LoadProject(Dir/TEXT("case.lbms")));
    TestFalse(TEXT("Open clears prior project undo"),M.CanUndoCase());
    TestFalse(TEXT("Open clears prior project redo"),M.CanRedoCase());
    TestEqual(TEXT("Saved draft inlet independent of later history"),M.Project.Draft.Setup.InletVelocity.GetValue(),FVector(1,0,0));
    M.EditCase(TEXT("Rename"),[](auto& D) { D.Name=TEXT("Rename"); }); M.NewProject(TEXT("New"));
    TestFalse(TEXT("New project cannot undo another project's edits"),M.CanUndoCase());
    return true;
}
#endif
