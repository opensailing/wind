#include "StudioBoundaries.h"
#include "StudioModel.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags BoundaryFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FStudioBoundaryCondition Boundary(const FGuid& Target,EStudioBoundaryType Type)
{FStudioBoundaryCondition B;B.TargetId=Target;B.Type=Type;B.Name=StudioBoundaries::TypeName(Type);return B;}
FString BoundaryJSON(const TSharedPtr<FJsonObject>& Json)
{FString Text;FJsonSerializer::Serialize(Json.ToSharedRef(),TJsonWriterFactory<>::Create(&Text));return Text;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioBoundaryCoverageTest,"Studio.Boundaries.CoverageAndExplicitCapabilities",BoundaryFlags)
bool FStudioBoundaryCoverageTest::RunTest(const FString&)
{
    FStudioCaseDraft Case;Case.Setup.BackendId=TEXT("declared-test-capabilities");
    auto Coverage=StudioBoundaries::Analyze(Case);TestEqual(TEXT("Six domain targets"),Coverage.Targets,6);TestEqual(TEXT("No invented assignments"),Coverage.Configured,0);
    TestFalse(TEXT("Incomplete coverage"),Coverage.Complete());TestFalse(TEXT("Unknown solver capabilities"),Coverage.bCapabilitiesKnown);
    FString Error;const auto FaceIds=Case.Domain.Faces;for(const auto& Face:FaceIds)TestTrue(TEXT("Assign explicit wall"),StudioBoundaries::Set(Case,Boundary(Face,EStudioBoundaryType::NoSlip),false,Error));
    Coverage=StudioBoundaries::Analyze(Case);TestTrue(TEXT("All faces configured"),Coverage.Complete());TestFalse(TEXT("Coverage alone never proves compatibility"),Coverage.Compatible());
    auto Inlet=Case.Boundaries[0];Inlet.Type=EStudioBoundaryType::VelocityInlet;
    TestTrue(TEXT("Incomplete draft is saveable"),StudioBoundaries::Set(Case,Inlet,false,Error));
    Coverage=StudioBoundaries::Analyze(Case);TestEqual(TEXT("Missing velocity is not configured"),Coverage.Configured,5);
    TestTrue(TEXT("Missing value names owning target"),Coverage.Issues.ContainsByPredicate([&](const auto& I){return I.Target==Inlet.TargetId&&I.Kind==EStudioBoundaryIssue::Incomplete;}));
    Inlet.Velocity=FVector(2,0,0);StudioBoundaries::Set(Case,Inlet,false,Error);
    FStudioBoundaryCapabilities Caps;Caps.BackendId=Case.Setup.BackendId;Caps.Types={EStudioBoundaryType::NoSlip};
    Coverage=StudioBoundaries::Analyze(Case,&Caps);TestTrue(TEXT("Configuration coverage can be complete"),Coverage.Complete());TestFalse(TEXT("Unsupported inlet prevents compatibility"),Coverage.Compatible());
    Caps.Types.Add(EStudioBoundaryType::VelocityInlet);Coverage=StudioBoundaries::Analyze(Case,&Caps);
    TestTrue(TEXT("Only explicit matching capabilities establish compatibility"),Coverage.Compatible());
    Caps.BackendId=TEXT("other");TestFalse(TEXT("Wrong backend cannot attest compatibility"),StudioBoundaries::Analyze(Case,&Caps).Compatible());
    Case.Boundaries[0].Temperature=300.;Coverage=StudioBoundaries::Analyze(Case);
    TestTrue(TEXT("Thermal requirement disclosed"),Coverage.Issues.ContainsByPredicate([](const auto& I){return I.Kind==EStudioBoundaryIssue::Unsupported;}));
    auto Duplicate=Case.Boundaries[0];Duplicate.Id=FGuid::NewGuid();Case.Boundaries.Add(Duplicate);
    TestFalse(TEXT("Conflicting assignments cannot be complete"),StudioBoundaries::Analyze(Case).Complete());
    Case.Boundaries.Pop();Case.Boundaries[0].TargetId=FGuid::NewGuid();Coverage=StudioBoundaries::Analyze(Case);
    TestTrue(TEXT("Invalidated patch mapping is visible"),Coverage.Issues.ContainsByPredicate([&](const auto& I){return I.Target==Case.Boundaries[0].TargetId&&I.Kind==EStudioBoundaryIssue::Conflict;}));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioBoundaryPairsTest,"Studio.Boundaries.PeriodicTransactionsAndFrozenRuns",BoundaryFlags)
bool FStudioBoundaryPairsTest::RunTest(const FString&)
{
    FStudioModel Model(FPaths::ProjectSavedDir()/TEXT("Automation/Boundaries")/FGuid::NewGuid().ToString());Model.Pause();
    const auto Camera=Model.Project.Camera;const auto Frame=Model.SelectedFrame;const auto Intent=Model.RenderIntentRevision;
    Model.EditCase(TEXT("Fixture adapter"),[](auto& C){C.Setup.BackendId=TEXT("test-control-harness");});
    Model.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Before boundary edits"),Model.Project.Draft,EStudioRunOrigin::ControlHarness));
    const FString Frozen=StudioCaseIO::Serialize(*Model.Project.Runs.Last().GetConfiguration());
    auto Pair=Boundary(Model.Project.Draft.Domain.Faces[0],EStudioBoundaryType::Periodic);Pair.PairedTargetId=Model.Project.Draft.Domain.Faces[1];
    TestTrue(TEXT("Pair is assigned atomically"),Model.UpdateBoundary(Pair));TestEqual(TEXT("Both reciprocal records exist"),Model.Project.Draft.Boundaries.Num(),2);
    const auto* Partner=Model.Project.Draft.Boundaries.FindByPredicate([&](const auto& B){return B.TargetId==Pair.PairedTargetId;});
    TestTrue(TEXT("Partner references original target"),Partner&&Partner->PairedTargetId==Pair.TargetId);const FGuid PartnerId=Partner?Partner->Id:FGuid();
    Pair.Name=TEXT("Streamwise periodic");TestTrue(TEXT("Rename keeps reciprocal assignment"),Model.UpdateBoundary(Pair));
    Partner=Model.Project.Draft.Boundaries.FindByPredicate([&](const auto& B){return B.TargetId==Pair.PairedTargetId;});TestTrue(TEXT("Partner identity retained"),Partner&&Partner->Id==PartnerId);
    auto Wall=Pair;Wall.Type=EStudioBoundaryType::NoSlip;Wall.PairedTargetId.Invalidate();
    const FString Paired=StudioCaseIO::Serialize(Model.Project.Draft);
    TestFalse(TEXT("Unpair requires explicit operation"),Model.UpdateBoundary(Wall));TestEqual(TEXT("Rejected change preserves pair"),StudioCaseIO::Serialize(Model.Project.Draft),Paired);
    TestTrue(TEXT("Explicit unpair and type change"),Model.UpdateBoundary(Wall,true));TestEqual(TEXT("Only edited wall remains"),Model.Project.Draft.Boundaries.Num(),1);
    TestTrue(TEXT("One undo restores both records"),Model.UndoCase());TestEqual(TEXT("Restored reciprocal pair"),Model.Project.Draft.Boundaries.Num(),2);
    TestTrue(TEXT("Remove either side clears pair"),Model.RemoveBoundary(Pair.PairedTargetId));TestTrue(TEXT("No orphan counterpart"),Model.Project.Draft.Boundaries.IsEmpty());
    TestTrue(TEXT("Undo restores both assignments"),Model.UndoCase());
    const FString BeforeBad=StudioCaseIO::Serialize(Model.Project.Draft);auto Bad=Pair;Bad.PairedTargetId=Model.Project.Draft.Domain.Faces[3];
    TestFalse(TEXT("Perpendicular periodic faces rejected"),Model.UpdateBoundary(Bad));TestEqual(TEXT("Invalid pairing transactional"),StudioCaseIO::Serialize(Model.Project.Draft),BeforeBad);
    TestTrue(TEXT("Flow camera unaffected"),StudioView::CameraEquals(Model.Project.Camera,Camera));TestEqual(TEXT("Frame unaffected"),Model.SelectedFrame,Frame);TestEqual(TEXT("No CFD requests"),Model.RenderIntentRevision,Intent);
    TestEqual(TEXT("Frozen run unaffected"),StudioCaseIO::Serialize(*Model.Project.Runs.Last().GetConfiguration()),Frozen);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioBoundaryPersistenceTest,"Studio.Boundaries.PairPersistenceAndMigration",BoundaryFlags)
bool FStudioBoundaryPersistenceTest::RunTest(const FString&)
{
    FStudioProject Project;FString Error;
    auto Pair=Boundary(Project.Draft.Domain.Faces[4],EStudioBoundaryType::Periodic);Pair.PairedTargetId=Project.Draft.Domain.Faces[5];
    TestTrue(TEXT("Create periodic fixture"),StudioBoundaries::Set(Project.Draft,Pair,false,Error));
    FStudioProject Read;const FString Text=StudioProjectIO::Serialize(Project);
    TestTrue(TEXT("Periodic pair reopens"),StudioProjectIO::Parse(Text,Read,Error));TestEqual(TEXT("All pair identities exact"),StudioCaseIO::Serialize(Read.Draft),StudioCaseIO::Serialize(Project.Draft));
    TSharedPtr<FJsonObject> Json;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json);
    Json->GetObjectField(TEXT("draft"))->GetArrayField(TEXT("boundaries"))[0]->AsObject()->RemoveField(TEXT("pairedTargetId"));
    TestFalse(TEXT("Current schema cannot silently drop pairing"),StudioProjectIO::Parse(BoundaryJSON(Json),Read,Error));
    Project.Draft.Boundaries.Reset();StudioBoundaries::Set(Project.Draft,Boundary(Project.Draft.Domain.Faces[0],EStudioBoundaryType::NoSlip),false,Error);
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(Project)),Json);Json->SetNumberField(TEXT("version"),16);
    Json->GetObjectField(TEXT("draft"))->GetArrayField(TEXT("boundaries"))[0]->AsObject()->RemoveField(TEXT("pairedTargetId"));
    TestTrue(TEXT("Legacy nonperiodic assignment migrates"),StudioProjectIO::Parse(BoundaryJSON(Json),Read,Error));
    TestFalse(TEXT("Legacy migration invents no partner"),Read.Draft.Boundaries[0].PairedTargetId.IsValid());
    auto Wall=Boundary(Project.Draft.Domain.Faces[1],EStudioBoundaryType::NoSlip);StudioBoundaries::Set(Project.Draft,Wall,false,Error);
    Pair=Project.Draft.Boundaries[0];Pair.Type=EStudioBoundaryType::Periodic;Pair.PairedTargetId=Project.Draft.Domain.Faces[1];
    const FString Before=StudioCaseIO::Serialize(Project.Draft);
    TestFalse(TEXT("Pair cannot overwrite another existing condition"),StudioBoundaries::Set(Project.Draft,Pair,false,Error));
    TestEqual(TEXT("Conflicting wall remains unchanged"),StudioCaseIO::Serialize(Project.Draft),Before);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioBoundaryDraftTest,"Studio.Boundaries.RetainedPhysicalDrafts",BoundaryFlags)
bool FStudioBoundaryDraftTest::RunTest(const FString&)
{
    FStudioCaseDraft Case;const auto Target=StudioBoundaries::Targets(Case)[0];
    FStudioBoundaryEdit Edit;Edit.Reset(Target,nullptr);const FGuid AssignmentId=Edit.Saved.Id;
    TestFalse(TEXT("Empty target begins clean"),Edit.IsDirty());TestTrue(TEXT("Empty target matches no assignment"),Edit.Matches(nullptr));
    Edit.Type=EStudioBoundaryType::VelocityInlet;FStudioBoundaryCondition Result;
    TestTrue(TEXT("Unknown complete vector is a saveable draft"),Edit.Build(Case,Result));
    TestFalse(TEXT("Unknown velocity is not invented"),Result.Velocity.IsSet());
    Edit.Velocity[0]=TEXT("1.2345678901234567");
    Result.Name=TEXT("Sentinel");
    TestFalse(TEXT("Partial vector rejected"),Edit.Build(Case,Result));TestEqual(TEXT("Missing Y is focused"),Edit.ErrorField,2);
    TestEqual(TEXT("Rejected draft cannot publish"),Result.Name,FString(TEXT("Sentinel")));
    TestEqual(TEXT("Rejected text retained"),Edit.Velocity[0],FString(TEXT("1.2345678901234567")));
    Edit.Velocity[1]=TEXT("0");Edit.Velocity[2]=TEXT("-2.5e-6");
    TestTrue(TEXT("Explicit finite components accepted"),Edit.Build(Case,Result));
    TestEqual(TEXT("Assignment identity stable"),Result.Id,AssignmentId);
    TestEqual(TEXT("Physical double precision retained"),Result.Velocity.GetValue().X,1.2345678901234567);
    Edit.Reset(Target,&Result);TestTrue(TEXT("Current assignment matches"),Edit.Matches(&Result));
    auto Changed=Result;Changed.Velocity=FVector(9,0,0);TestFalse(TEXT("External edit detected"),Edit.Matches(&Changed));
    TestFalse(TEXT("External removal detected"),Edit.Matches(nullptr));
    Edit.Velocity[2]=TEXT("NaN");TestFalse(TEXT("Nonfinite vector rejected"),Edit.Build(Case,Result));TestEqual(TEXT("Invalid Z named"),Edit.ErrorField,3);
    Edit.Type=EStudioBoundaryType::PressureOutlet;Edit.Pressure=TEXT("-101.23456789012345");
    TestTrue(TEXT("Switch to outlet ignores retained inactive velocity draft"),Edit.Build(Case,Result));
    TestFalse(TEXT("Outlet has no incompatible velocity"),Result.Velocity.IsSet());
    TestEqual(TEXT("Pressure sign and precision retained"),Result.Pressure.GetValue(),-101.23456789012345);
    TestEqual(TEXT("Inactive velocity text retained for switching back"),Edit.Velocity[2],FString(TEXT("NaN")));
    Edit.Temperature=TEXT("0");TestFalse(TEXT("Absolute temperature must exceed zero"),Edit.Build(Case,Result));
    Edit.Temperature=TEXT("300");TestTrue(TEXT("Known temperature accepted into draft"),Edit.Build(Case,Result));
    Edit.Type=EStudioBoundaryType::Periodic;TestTrue(TEXT("Opposite face is derived from stable target"),Edit.Build(Case,Result));
    TestEqual(TEXT("Correct partner"),Result.PairedTargetId,Case.Domain.Faces[1]);
    TestTrue(TEXT("Periodic clears incompatible published fields"),!Result.Velocity.IsSet()&&!Result.Pressure.IsSet()&&!Result.Temperature.IsSet());
    Case.Domain.Faces[0]=FGuid::NewGuid();TestFalse(TEXT("Replaced target cannot receive stale edits"),Edit.Build(Case,Result));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioBoundaryCatalogTest,"Studio.Boundaries.BoundedCatalogAndCoverage",BoundaryFlags)
bool FStudioBoundaryCatalogTest::RunTest(const FString&)
{
    // Catalog metadata fixture only: it is not a geometry source or CFD result.
    FStudioCaseDraft Case;FStudioGeometryAsset Asset;Asset.Name=TEXT("Wing");Asset.SourcePath=TEXT("catalog-fixture.obj");
    Asset.Format=TEXT("obj");Asset.SourceSHA256=FString::ChrN(64,TEXT('a'));
    for(int32 I=0;I<300;++I){FStudioSurfacePatch Patch;Patch.Name=FString::Printf(TEXT("Panel %03d"),I);Asset.Patches.Add(Patch);}
    Case.Geometry.Add(Asset);FString Error;
    TestTrue(TEXT("Large catalog fixture is structurally valid"),StudioCaseIO::Validate(Case,Error));
    const auto First=StudioBoundaries::TargetPage(Case,TEXT(""),0,1000000);
    TestEqual(TEXT("All targets counted"),First.Matches,306);TestEqual(TEXT("Requested oversize page is bounded"),First.Items.Num(),128);
    const auto Second=StudioBoundaries::TargetPage(Case,TEXT(""),128);
    if(!TestEqual(TEXT("Second page is full"),Second.Items.Num(),128))return false;
    TestEqual(TEXT("Second page preserves source ordering"),Second.Items[0].Id,Asset.Patches[122].Id);
    const auto Last=StudioBoundaries::TargetPage(Case,TEXT(""),256);
    if(!TestEqual(TEXT("Final page includes remaining targets"),Last.Items.Num(),50))return false;
    TestEqual(TEXT("Last source identity retained"),Last.Items.Last().Id,Asset.Patches.Last().Id);
    const auto Filtered=StudioBoundaries::TargetPage(Case,TEXT(" WING / panel 29 "),0,2);
    TestEqual(TEXT("Case-insensitive trimmed filter counts all matches"),Filtered.Matches,10);
    if(!TestEqual(TEXT("Filter count independent of requested page size"),Filtered.Items.Num(),2))return false;
    TestEqual(TEXT("Filter finds actual stable patch"),Filtered.Items[0].Id,Asset.Patches[290].Id);
    const auto Beyond=StudioBoundaries::TargetPage(Case,TEXT("Panel"),10000);
    TestTrue(TEXT("Offset beyond filtered end returns empty page"),Beyond.Items.IsEmpty());TestEqual(TEXT("Empty page retains exact filtered count"),Beyond.Matches,300);
    const auto CountOnly=StudioBoundaries::TargetPage(Case,TEXT("Panel"),-5,-1);
    TestTrue(TEXT("Nonpositive page size allocates no rows"),CountOnly.Items.IsEmpty());TestEqual(TEXT("Count-only query still counts matches"),CountOnly.Matches,300);
    FStudioBoundaryTarget Target;
    TestTrue(TEXT("Direct lookup resolves imported stable identity"),StudioBoundaries::FindTarget(Case,Asset.Patches.Last().Id,Target));
    TestEqual(TEXT("Direct lookup keeps original owner"),Target.GeometryId,Asset.Id);
    TestEqual(TEXT("Imported patch is not a domain face"),Target.DomainFace,INDEX_NONE);const FGuid Kept=Target.Id;
    TestFalse(TEXT("Foreign target is rejected"),StudioBoundaries::FindTarget(Case,FGuid::NewGuid(),Target));TestEqual(TEXT("Failed lookup leaves output intact"),Target.Id,Kept);
    TestTrue(TEXT("Domain lookup reports fixed physical side"),StudioBoundaries::FindTarget(Case,Case.Domain.Faces[4],Target));TestEqual(TEXT("Negative Z side"),Target.DomainFace,4);
    const auto Coverage=StudioBoundaries::Analyze(Case);
    TestEqual(TEXT("Every unassigned target contributes to coverage"),Coverage.Targets,306);TestEqual(TEXT("Every issue counted"),Coverage.IssueCount,306);
    TestEqual(TEXT("Every missing condition blocks completeness"),Coverage.BlockingIssueCount,306);
    TestEqual(TEXT("Detailed diagnostics have bounded storage"),Coverage.Issues.Num(),128);TestFalse(TEXT("Truncating issue details cannot make a case complete"),Coverage.Complete());
    return true;
}
#endif
