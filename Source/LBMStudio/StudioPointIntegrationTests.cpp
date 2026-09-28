#include "StudioModel.h"
#include "StudioPointRecording.h"
#include "StudioAssetPaths.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
struct FPointCaseFiles
{
    FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/PointIntegration")/FGuid::NewGuid().ToString());
    ~FPointCaseFiles(){IFileManager::Get().DeleteDirectory(*Root,false,true);}
    bool Copy(const FString& Name)
    {
        const FString From=FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture"),To=Root/Name;
        IFileManager::Get().MakeDirectory(*To,true);
        for(const TCHAR* File:{TEXT("recording.json"),TEXT("provenance.json"),TEXT("ATTRIBUTION.txt"),TEXT("coordinates.f64"),TEXT("point-ids.i64"),
            TEXT("velocity_u.f64"),TEXT("velocity_v.f64"),TEXT("velocity_magnitude.f64"),TEXT("pressure.f64"),TEXT("cell_volume.f64")})
            if(IFileManager::Get().Copy(*(To/File),*(From/File))!=COPY_OK)return false;
        return true;
    }
};
bool Finish(FStudioModel& M)
{
    const double Deadline=FPlatformTime::Seconds()+20;
    while((M.IsRecordingLoadPending()||M.IsProjectOpenPending())&&FPlatformTime::Seconds()<Deadline)
    {M.Tick(0);FPlatformProcess::Sleep(.001f);}
    return !M.IsRecordingLoadPending()&&!M.IsProjectOpenPending();
}
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointAdapter,"Studio.PointIntegration.OptionalFieldsAndExport",Flags)
bool FStudioPointAdapter::RunTest(const FString&)
{
    FPointCaseFiles Files;if(!TestTrue(TEXT("Copy original CFD test snapshots"),Files.Copy(TEXT("source"))))return false;
    const auto Opened=StudioRecordings::Import(Files.Root/TEXT("source/recording.json"),1,{});
    if(!TestTrue(*Opened.Error,Opened.Source.IsValid()))return false;
    TestEqual(TEXT("Point-format reference"),Opened.Reference->Format,FString(TEXT("point_v3")));
    TestTrue(TEXT("No fictional monolithic payload hash"),Opened.Reference->PayloadSHA256.IsEmpty());
    TestTrue(TEXT("Descriptor identifies source points"),Opened.Source->Descriptor().bSourcePoints);
    TestTrue(TEXT("Original velocity components available"),Opened.Source->Descriptor().bPointVelocity);
    TestEqual(TEXT("Source range retained"),Opened.Source->Descriptor().Scalars[2].Minimum,-616.4060669);
    TestEqual(TEXT("Actual original source step"),Opened.Source->EvaluateFrame(1).Index,5001);
    const auto Field=Opened.Source->CaptureViewField(1,TEXT("pressure"),false);
    const auto Points=Field->OriginalPoints();
    if(!TestTrue(TEXT("Exact source points are exposed"),Points.IsValid()))return false;
    TestNotNull(TEXT("Chosen pressure array"),Points->FindValues(TEXT("pressure")));
    TestNull(TEXT("Unrequested velocity not loaded for scalar-only view"),Points->FindValues(TEXT("velocity_u")));
    TestNull(TEXT("Missing density never invented"),Points->FindValues(TEXT("density")));
    FStudioFieldValue V;TestFalse(TEXT("Source points do not impersonate an interpolation mesh"),Field->Sample(FVector::ZeroVector,V));
    TestTrue(TEXT("No invented solid boundary"),Field->Boundary().IsEmpty());
    const FString CSV=Files.Root/TEXT("original-points.csv");
    TestTrue(TEXT("Export original values"),Opened.Source->ExportField(1,CSV));
    FString Text;TestTrue(TEXT("Read exported CSV"),FFileHelper::LoadFileToString(Text,*CSV));
    FString Header,Body;Text.Split(TEXT("\n"),&Header,&Body);
    TestTrue(TEXT("Export names unit uncertainty"),Header.Contains(TEXT("cell_volume [unspecified]")));
    TestFalse(TEXT("No density column"),Header.Contains(TEXT("density")));
    TestFalse(TEXT("No invented third coordinate"),Header.Contains(TEXT("source_z")));
    TestTrue(TEXT("Interpretation hash accompanies values"),Body.Contains(Opened.Reference->MetadataSHA256));
    TArray<FString> Rows;Text.ParseIntoArrayLines(Rows);TestEqual(TEXT("Every original point exported"),Rows.Num(),18707);
    TestFalse(TEXT("Invalid frame produces no export"),Opened.Source->ExportField(3,Files.Root/TEXT("invalid.csv")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointProject,"Studio.PointIntegration.ProjectSelectionReopenAndRepair",Flags)
bool FStudioPointProject::RunTest(const FString&)
{
    FPointCaseFiles Files;
    if(!TestTrue(TEXT("Original and exact relocated copies"),Files.Copy(TEXT("source"))&&Files.Copy(TEXT("moved"))))return false;
    FStudioModel M(Files.Root/TEXT("session"));M.Project.Camera.Position=FVector(7,8,9);M.Scrub(.4);
    const auto Camera=M.Project.Camera;const FString Draft=StudioCaseIO::Serialize(M.Project.Draft);
    const auto Before=StudioProjectIO::Serialize(M.SnapshotProject());
    TestTrue(TEXT("Point import starts"),M.RequestExternalRecording(Files.Root/TEXT("source/recording.json")));
    M.CancelRecording();TestTrue(TEXT("Cancelled point verifier drains"),Finish(M));
    TestEqual(TEXT("Cancellation retains entire document"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestTrue(TEXT("Point import retries"),M.RequestExternalRecording(Files.Root/TEXT("source/recording.json")));
    TestTrue(TEXT("Point import finishes"),Finish(M));
    if(!TestTrue(*M.Notice,M.Solver->Descriptor().bSourcePoints))return false;
    const FString Id=M.Project.Dataset;
    TestTrue(TEXT("Camera independent from source change"),StudioView::CameraEquals(Camera,M.Project.Camera));
    TestEqual(TEXT("Case is unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Draft);
    TestEqual(TEXT("Supplied default scalar"),M.ActiveScalar().Label,FString(TEXT("Exported speed")));
    M.EditView(TEXT("Pressure"),[](auto& S){S.Display.ScalarField=TEXT("pressure");S.Display.PointSize=1.5;S.Display.bSourcePoints=false;});
    TestEqual(TEXT("Selected field uses its own units"),M.ActiveScalar().Unit,FString(TEXT("Pa")));
    TestTrue(TEXT("Color/point display edit is undoable"),M.UndoView());
    TestEqual(TEXT("Undo restores source default"),M.ActiveScalar().Id,FString(TEXT("velocity_magnitude")));
    TestTrue(TEXT("Color/point display edit redo"),M.RedoView());M.Scrub(.5);
    const FString File=Files.Root/TEXT("project/case.lbms");TestTrue(TEXT("Save point source"),M.SaveProject(File));
    FString JSON,Error;FStudioProject Stored;FFileHelper::LoadFileToString(JSON,*File);
    TestTrue(TEXT("Point project parses"),StudioProjectIO::Parse(JSON,Stored,Error));
    TestTrue(TEXT("Descriptor location is portable"),FPaths::IsRelative(Stored.Recordings[0].Path));
    TestEqual(TEXT("Scalar field persisted"),Stored.View.ScalarField,FString(TEXT("pressure")));
    TestTrue(TEXT("Save As into another directory"),M.SaveProject(Files.Root/TEXT("elsewhere/case.lbms")));
    FStudioProject Rebased;TestTrue(TEXT("Rebased project resolves"),StudioProjectIO::Load(M.ProjectPath,Rebased,Error));
    TestTrue(TEXT("Same descriptor after Save As"),FPaths::IsSamePath(Rebased.Recordings[0].Path,Files.Root/TEXT("source/recording.json")));
    TestTrue(TEXT("Switch to existing source"),M.RequestRecording(TEXT("MeshGraphNets_Airfoil_test009")));TestTrue(TEXT("Old reader remains usable"),Finish(M));
    TestEqual(TEXT("Available pressure selection is retained"),M.ActiveScalar().Id,FString(TEXT("pressure")));
    M.EditView(TEXT("Unavailable field"),[](auto& S){S.Display.ScalarField=TEXT("missing_field");});
    TestEqual(TEXT("Unsupported selection uses actual available scalar"),M.ActiveScalar().Id,FString(TEXT("velocity_magnitude")));
    TestTrue(TEXT("Open saved point project"),M.RequestProjectOpen(File));TestTrue(TEXT("Point reopen finishes"),Finish(M));
    TestEqual(TEXT("Source restored"),M.Project.Dataset,Id);TestEqual(TEXT("Frame ordinal restored"),M.SelectedFrame,1);
    TestEqual(TEXT("Scalar restored"),M.ActiveScalar().Id,FString(TEXT("pressure")));
    TestEqual(TEXT("Point size restored"),M.PointSize,1.5);TestFalse(TEXT("Point visibility restored"),M.bSourcePoints);
    TestTrue(TEXT("Camera restored exactly"),StudioView::CameraEquals(M.Project.Camera,Camera));
    TestTrue(TEXT("Remove only owned source folder"),IFileManager::Get().DeleteDirectory(*(Files.Root/TEXT("source")),false,true));
    const auto Saved=StudioProjectIO::Serialize(M.SnapshotProject());
    TestTrue(TEXT("Missing source open starts"),M.RequestProjectOpen(File));TestTrue(TEXT("Missing source detection finishes"),Finish(M));
    TestTrue(TEXT("Repair offered"),M.RecordingRepair.IsSet());
    TestEqual(TEXT("Missing source keeps current project"),StudioProjectIO::Serialize(M.SnapshotProject()),Saved);
    TestTrue(TEXT("Repair matching descriptor and members"),M.RetryRecordingRepair(Files.Root/TEXT("moved/recording.json")));
    TestTrue(TEXT("Repair finishes"),Finish(M));TestFalse(TEXT("Repair clears prompt"),M.RecordingRepair.IsSet());
    TestTrue(TEXT("Verified new location applied"),FPaths::IsSamePath(M.Project.Recordings[0].Path,Files.Root/TEXT("moved/recording.json")));
    TestEqual(TEXT("Repair keeps scalar selection"),M.ActiveScalar().Id,FString(TEXT("pressure")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPointSchema,"Studio.PointIntegration.SchemaMigrationAndPinnedIdentity",Flags)
bool FStudioPointSchema::RunTest(const FString&)
{
    FStudioProject P,Out;FString Error;
    FStudioRecordingReference R;R.Id=TEXT("example");R.Title=TEXT("Point recording");R.Path=TEXT("../points/recording.json");
    R.MetadataSHA256=FString::ChrN(64,'a');R.Format=TEXT("point_v3");P.Recordings.Add(R);
    TestTrue(TEXT("V7 point reference round trip"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Out,Error));
    TestEqual(TEXT("Transitive member identity"),Out.Recordings[0].MetadataSHA256,R.MetadataSHA256);
    auto Bad=P;Bad.Recordings[0].Format=TEXT("unknown");
    TestFalse(TEXT("Unknown recording interpretation rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Bad),Out,Error));
    Bad=P;Bad.Recordings[0].Path=TEXT("../points/flow.bin");
    TestFalse(TEXT("Point reference cannot pose as monolithic file"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Bad),Out,Error));
    Bad=P;Bad.Recordings[0].PayloadSHA256=FString::ChrN(64,'b');
    TestFalse(TEXT("Unexpected point payload identity cannot silently disappear on save"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Bad),Out,Error));
    TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(FStudioProject())),O);
    O->SetNumberField(TEXT("version"),6);auto View=O->GetObjectField(TEXT("view"));
    for(const TCHAR* Key:{TEXT("scalarField"),TEXT("pointSize"),TEXT("sourcePoints")})View->RemoveField(Key);
    FString Text;FJsonSerializer::Serialize(O.ToSharedRef(),TJsonWriterFactory<>::Create(&Text));
    TestTrue(TEXT("V6 view migrates without new fields"),StudioProjectIO::Parse(Text,Out,Error));
    TestTrue(TEXT("Migration selects source default"),Out.View.ScalarField.IsEmpty());TestEqual(TEXT("Initial point size"),Out.View.PointSize,1.);
    FPointCaseFiles Files;if(!TestTrue(TEXT("Copy fixture"),Files.Copy(TEXT("source"))))return false;
    const auto Imported=StudioRecordings::Import(Files.Root/TEXT("source/recording.json"),0,{});
    if(!TestTrue(TEXT("Original imports"),Imported.Source.IsValid()))return false;
    TArray<uint8> Data;const FString Member=Files.Root/TEXT("source/pressure.f64");FFileHelper::LoadFileToArray(Data,*Member);
    Data[0]^=1;FFileHelper::SaveArrayToFile(Data,*Member);
    TestFalse(TEXT("All members pinned even when metadata unchanged"),StudioRecordings::Open(Imported.Reference->Id,{*Imported.Reference},0,{}).Source.IsValid());
    return true;
}
#endif
