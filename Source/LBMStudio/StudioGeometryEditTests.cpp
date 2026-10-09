#include "StudioGeometryEdit.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioGeometryEditTests
{
constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
FString Directory()
{
    const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/GeometryEdit") / FGuid::NewGuid().ToString());
    IFileManager::Get().MakeDirectory(*Path, true);
    return Path;
}
bool Fixture(const FString& Root, FStudioMeshImportResult& Source, FStudioGeometryAsset& Asset)
{
    const FString Path = Root / TEXT("original.obj");
    // Structural geometry only. No generated scientific fields.
    if (!FFileHelper::SaveStringToFile(TEXT("# Original transform fixture\nv 0 0 0\nv 1000 0 0\nv 0 500 0\nv 0 0 250\ng Wing\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n"),
        *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
    Source = StudioMeshImport::Read(Path, MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false));
    FStudioMeshImportOptions Options;
    Options.Name = TEXT("Wing fixture"); Options.MetersPerUnit = .001;
    FString Error;
    return StudioMeshImport::MakeAsset(Source, Options, Asset, Error);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioGeometryExactEdit, "Studio.GeometryEdit.ExactDraftsAndPhysicalTransform", StudioGeometryEditTests::Flags)
bool FStudioGeometryExactEdit::RunTest(const FString&)
{
    FStudioGeometryAsset Asset;
    Asset.Name = TEXT("Original wing"); Asset.MetersPerSourceUnit = .001;
    Asset.Translation = FVector(.12345678901234567, -2.123456789012345, 3.);
    Asset.Rotation = FRotator(90., 23.456789012345, 17.).Quaternion();
    Asset.Scale = FVector(1.12345678901234, 2., .5);
    FStudioGeometryEdit Edit; Edit.Reset(Asset);
    TestFalse(TEXT("A loaded form is clean"), Edit.IsDirty());
    Edit.Name = TEXT("Renamed wing");
    FStudioGeometryAsset Built;
    TestTrue(TEXT("A rename builds"), Edit.Build(Built));
    TestTrue(TEXT("A rename preserves the stored quaternion bit for bit"), Built.Rotation == Asset.Rotation);
    TestTrue(TEXT("Unedited physical translation and scale stay exact"), Built.Translation == Asset.Translation && Built.Scale == Asset.Scale);
    for (int32 I = 0; I < 12; ++I)
    {
        Edit.Reset(Built); Edit.Name += TEXT("x");
        TestTrue(TEXT("Repeated edit/reopen does not drift at an Euler singularity"), Edit.Build(Built) && Built.Rotation == Asset.Rotation);
    }

    Asset.Rotation = FQuat::Identity; Edit.Reset(Asset);
    Edit.Values[0] = TEXT("10"); Edit.Values[1] = TEXT("20"); Edit.Values[2] = TEXT("30");
    Edit.Values[3] = TEXT("0"); Edit.Values[4] = TEXT("0"); Edit.Values[5] = TEXT("90");
    Edit.Values[6] = TEXT("2"); Edit.Values[7] = TEXT("3"); Edit.Values[8] = TEXT("4");
    TestTrue(TEXT("Explicit physical transform builds"), Edit.Build(Built));
    const FVector PhysicalX = StudioMeshImport::TransformPosition(FVector(1000, 0, 0), Built);
    const FVector PhysicalY = StudioMeshImport::TransformPosition(FVector(0, 1000, 0), Built);
    AddInfo(FString::Printf(TEXT("Known 90-degree transform: X=(%.17g,%.17g,%.17g) Y=(%.17g,%.17g,%.17g)"),
        PhysicalX.X, PhysicalX.Y, PhysicalX.Z, PhysicalY.X, PhysicalY.Y, PhysicalY.Z));
    // UE's rotator-to-quaternion conversion differs from the ideal reference
    // by < 3e-10 m here. Stored, unedited coordinates are checked exactly above.
    TestTrue(TEXT("Source units and scale precede rotation and physical translation"),
        PhysicalX.Equals(FVector(10, 22, 30), 1.e-9));
    TestTrue(TEXT("Independent source-axis Y scale is retained"),
        PhysicalY.Equals(FVector(7, 20, 30), 1.e-9));
    TestEqual(TEXT("Source unit conversion stays unchanged"), Built.MetersPerSourceUnit, Asset.MetersPerSourceUnit);
    const auto Kept = Built;
    for (const TCHAR* Invalid : {TEXT(""), TEXT("nan"), TEXT("inf"), TEXT("12m"), TEXT("1e100")})
    {
        Edit.Values[0] = Invalid;
        TestFalse(TEXT("Invalid physical coordinate is rejected"), Edit.Build(Built));
        TestEqual(TEXT("Rejected text is retained"), Edit.Values[0], FString(Invalid));
        TestTrue(TEXT("Rejected output remains unchanged"), Built.Translation == Kept.Translation && Built.Rotation == Kept.Rotation);
        TestEqual(TEXT("Coordinate error identifies its field"), Edit.ErrorField, 1);
    }
    for (const TCHAR* Invalid : {TEXT("0"), TEXT("-1"), TEXT("100000001")})
    {
        Edit.Reset(Asset); Edit.Values[8] = Invalid;
        TestFalse(TEXT("Degenerate, mirrored and excessive scale are rejected"), Edit.Build(Built));
        TestEqual(TEXT("Scale error identifies its field"), Edit.ErrorField, 9);
    }
    Edit.Reset(Asset); Edit.Values[5] = TEXT("360001");
    TestFalse(TEXT("Unbounded angle input is rejected"), Edit.Build(Built));
    Edit.Reset(Asset); Edit.Name = TEXT(" ");
    TestFalse(TEXT("An empty name remains unapplied"), Edit.Build(Built));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioGeometryEditTransaction, "Studio.GeometryEdit.TransactionsPersistenceAndIsolation", StudioGeometryEditTests::Flags)
bool FStudioGeometryEditTransaction::RunTest(const FString&)
{
    const FString Root = StudioGeometryEditTests::Directory();
    FStudioMeshImportResult Source; FStudioGeometryAsset Asset;
    if (!TestTrue(TEXT("Read actual original geometry"), StudioGeometryEditTests::Fixture(Root, Source, Asset))) return false;
    FStudioModel Model(Root); Model.Pause();
    const auto Camera = Model.Project.Camera; const auto Solver = Model.Solver;
    const auto Frame = Model.SelectedFrame; const auto RenderRevision = Model.RenderIntentRevision;
    TestTrue(TEXT("Install a structural fixture"), Model.EditCase(TEXT("Fixture"), [&Asset](auto& Case)
    { Case.Geometry.Add(Asset); Case.Setup.BackendId = TEXT("test-control-harness"); }));
    Model.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Frozen wing"), Model.Project.Draft, EStudioRunOrigin::ControlHarness));
    const FString Frozen = StudioCaseIO::Serialize(*Model.Project.Runs.Last().GetConfiguration());
    const FString Before = StudioCaseIO::Serialize(Model.Project.Draft);
    const int64 BeforeRevision = Model.Project.Draft.Revision;
    FStudioGeometryEdit Edit; Edit.Reset(Asset);
    Edit.Name = TEXT("Moved wing"); Edit.Values[0] = TEXT("2.123456789012345"); Edit.Values[5] = TEXT("30"); Edit.Values[7] = TEXT("1.25");
    Model.SelectedGeometry = Asset.Id; Model.GeometrySource = Source;
    TestTrue(TEXT("Apply a name and transform atomically"), Model.UpdateGeometry(Edit));
    const auto Applied = Model.Project.Draft.Geometry[0];
    TestEqual(TEXT("Typed coordinate is exact"), Applied.Translation.X, 2.123456789012345);
    TestEqual(TEXT("Stable object ID survives"), Applied.Id, Asset.Id);
    TestEqual(TEXT("Stable patch ID survives"), Applied.Patches[0].Id, Asset.Patches[0].Id);
    TestEqual(TEXT("Original source hash survives"), Applied.SourceSHA256, Asset.SourceSHA256);
    TestTrue(TEXT("Verified immutable source is reused"), Model.GeometrySource.Mesh == Source.Mesh);
    TestEqual(TEXT("Original mesh coordinate is not transformed in place"), Source.Mesh->Positions[1], FVector(1000, 0, 0));
    TestEqual(TEXT("Frozen configuration is unchanged"), StudioCaseIO::Serialize(*Model.Project.Runs.Last().GetConfiguration()), Frozen);
    TestTrue(TEXT("Recording object and Solve camera are independent"), Model.Solver == Solver && StudioView::CameraEquals(Model.Project.Camera, Camera));
    TestEqual(TEXT("Source frame stays selected"), Model.SelectedFrame, Frame);
    TestEqual(TEXT("No scientific render request is made"), Model.RenderIntentRevision, RenderRevision);
    TestTrue(TEXT("One undo restores the complete name/transform"), Model.UndoCase());
    auto Restored = Model.Project.Draft; Restored.Revision = BeforeRevision;
    // History increments revisions; compare the content at the original revision.
    TestEqual(TEXT("Undo restores the complete original case"), StudioCaseIO::Serialize(Restored), Before);
    TestTrue(TEXT("Redo restores the complete edit"), Model.RedoCase());
    FStudioProject Loaded; FString Error;
    TestTrue(TEXT("Edited case serializes and reopens"), StudioProjectIO::Parse(StudioProjectIO::Serialize(Model.SnapshotProject()), Loaded, Error));
    TestTrue(TEXT("Reopened transform is exact"), Loaded.Draft.Geometry[0].Translation == Applied.Translation && Loaded.Draft.Geometry[0].Rotation == Applied.Rotation && Loaded.Draft.Geometry[0].Scale == Applied.Scale);
    TestEqual(TEXT("Reopened name is exact"), Loaded.Draft.Geometry[0].Name, Applied.Name);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioGeometryEditGuards, "Studio.GeometryEdit.ConflictsSourcesAndBounds", StudioGeometryEditTests::Flags)
bool FStudioGeometryEditGuards::RunTest(const FString&)
{
    const FString Root = StudioGeometryEditTests::Directory();
    FStudioMeshImportResult Source; FStudioGeometryAsset Asset;
    if (!TestTrue(TEXT("Read original mesh"), StudioGeometryEditTests::Fixture(Root, Source, Asset))) return false;
    FStudioModel Model(Root); Model.Pause();
    Model.EditCase(TEXT("Fixture"), [&Asset](auto& Case){Case.Geometry.Add(Asset);});
    FStudioGeometryEdit Edit; Edit.Reset(Asset); Edit.Values[0] = TEXT("3");
    TestFalse(TEXT("Unverified selection cannot apply"), Model.UpdateGeometry(Edit));
    Model.SelectedGeometry = Asset.Id; Model.GeometrySource = Source;
    FStudioGeometryEdit Stale = Edit;
    Model.EditCase(TEXT("Concurrent transform"), [](auto& Case){Case.Geometry[0].Translation.Y = 7.;});
    Model.GeometrySource = Source;
    const FString BeforeConflict = StudioCaseIO::Serialize(Model.Project.Draft);
    TestFalse(TEXT("A stale form cannot overwrite an external transform"), Model.UpdateGeometry(Stale));
    TestEqual(TEXT("Conflict preserves the applied case"), StudioCaseIO::Serialize(Model.Project.Draft), BeforeConflict);

    Edit.Reset(Model.Project.Draft.Geometry[0]); Edit.Values[0] = TEXT("100000000");
    TestFalse(TEXT("Transformed original bounds must also be in range"), Model.UpdateGeometry(Edit));
    TestEqual(TEXT("Bounds rejection is atomic"), StudioCaseIO::Serialize(Model.Project.Draft), BeforeConflict);
    Edit.Reset(Model.Project.Draft.Geometry[0]); Edit.Values[0] = TEXT("3");
    Model.GeometrySource.SHA256 = FString::ChrN(64, TEXT('0'));
    TestFalse(TEXT("Mismatched source verification cannot apply"), Model.UpdateGeometry(Edit));
    Model.GeometrySource = Source;
    FGuid Solid; Model.AddMaterial(true, Solid); Model.AssignGeometryMaterial(Asset.Id, Solid);
    Model.GeometrySource = Source;
    TestTrue(TEXT("A material assignment does not invalidate the geometry form"), Edit.Matches(Model.Project.Draft.Geometry[0]));
    TestTrue(TEXT("Apply retains a newer material assignment"), Model.UpdateGeometry(Edit));
    TestEqual(TEXT("Material remains assigned"), Model.Project.Draft.Geometry[0].MaterialId, Solid);
    auto Relocated = Model.Project.Draft.Geometry[0]; Edit.Reset(Relocated);
    Relocated.SourcePath = Root / TEXT("identical-relocated.obj");
    TestTrue(TEXT("A byte-identical relocation is not a transform conflict"), Edit.Matches(Relocated));
    Relocated.Patches[0].Id = FGuid::NewGuid();
    TestFalse(TEXT("A changed patch identity is a conflict"), Edit.Matches(Relocated));
    return true;
}
#endif
