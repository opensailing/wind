#include "StudioHome4SpatialDiagnostics.h"
#include "SStudioHome4SpatialDiagnostics.h"
#include "StudioModel.h"
#include "StudioHome4Session.h"
#include "StudioHome4Reports.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4SpatialTestFixtures
{
    // Artificial diagnostics fixtures are confined to automation, never application sources.
    FString Fixture(const FGuid& Run)
    {
        return FString::Printf(TEXT("{\"schema\":\"LBMStudio.Home4SpatialDiagnostics\",\"version\":1,\"run_id\":\"%s\",\"axis_order\":\"XYZ\",\"coordinate_unit\":\"original cells\",\"source_id\":\"artificial-spatial-test-source\","),*Run.ToString())+
            TEXT("\"patches\":[{\"id\":\"root\",\"level\":0,\"origin\":[-10,20,30],\"spacing\":[1,2,3],\"extents\":[4,3,2],\"cell_count\":24,\"masks\":[{\"kind\":\"active\",\"field_id\":\"active_mask\",\"count\":24}]},{\"id\":\"fine\",\"level\":2,\"origin\":[1,2,3],\"spacing\":[0.25,0.5,0.75],\"extents\":[8,6,4],\"cell_count\":192}],")
            TEXT("\"levels\":[{\"level\":0,\"nu\":{\"value\":0.01,\"unit\":\"cells2/step\"},\"sigma\":{\"value\":0.001,\"unit\":\"lattice\"},\"mobility\":{\"value\":0.02,\"unit\":\"cells2/step\"},\"gravity\":{\"value\":0.0001,\"unit\":\"cells/step2\"},\"tau\":{\"value\":0.53,\"unit\":\"1\"},\"substeps\":2,\"tau_floor\":false,\"root_phase_free\":true,\"even_wrap\":true,\"sneq_mode\":\"derived\",\"performance\":{\"elapsed_seconds\":2,\"node_updates\":4000000}},{\"level\":2,\"nu\":{\"value\":0.04,\"unit\":\"cells2/step\"}}],")
            TEXT("\"zones\":[{\"id\":\"beach\",\"kind\":\"beach\",\"min\":[0,0,0],\"max\":[4,5,6],\"profile\":{\"axis\":\"X\",\"coordinate_unit\":\"original cells\",\"value_unit\":\"1\",\"x\":[0,1,2],\"values\":[0,0.25,1]},\"level_strengths\":[{\"level\":2,\"value\":0.75,\"unit\":\"1\"}]}],")
            TEXT("\"geometry\":[{\"body_id\":\"hull\",\"cad_source\":\"original.step\",\"sdf_backend\":\"original-cpt\",\"tessellation_status\":\"complete\",\"cache_status\":\"hit\",\"cut_link_status\":\"measured\",\"cut_link_fraction\":0.25,\"tessellation_triangles\":120,\"cut_links\":20,\"bounds_min\":[0,0,0],\"bounds_max\":[1,2,3],\"equilibrium_heave\":{\"value\":1.2,\"unit\":\"mm\"},\"trim\":{\"value\":0.1,\"unit\":\"degrees\"}}]}");
    }
    TSharedRef<FJsonObject> Object(const FString& JSON)
    {TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON),O);return O.ToSharedRef();}
    FString JSON(const TSharedRef<FJsonObject>& O){FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SpatialParser,"Studio.Home4.Spatial.StrictOriginalAxesIdentityAndOptionalContracts",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SpatialParser::RunTest(const FString&)
{
    using namespace StudioHome4SpatialTestFixtures;const auto Run=FGuid::NewGuid();const auto Source=Fixture(Run);FStudioHome4SpatialEvidence E;FString Error;
    if(!TestTrue(TEXT("Explicit original diagnostics accepted"),StudioHome4SpatialDiagnostics::Parse(Source,Run,E,Error)))return false;
    TestEqual(TEXT("Original X origin preserved"),E.Patches[0].Origin->X,-10.);TestEqual(TEXT("Anisotropic source spacing preserved"),E.Patches[0].Spacing->Z,3.);
    TestEqual(TEXT("Patch bounds use supplied original node extents"),E.Patches[0].Bounds()->Max.Y,24.);TestEqual(TEXT("Original fine level retained"),E.Patches[1].Level,2);
    TestEqual(TEXT("Actual substeps retained without 2^d substitution"),E.Levels[0].Substeps.Get(-1),int64(2));TestFalse(TEXT("Absent fine tau not borrowed from root"),E.Levels[1].Tau.Value.IsSet());
    TestEqual(TEXT("Zone profile original shape retained"),E.Zones[0].ProfileValues[1],.25);TestEqual(TEXT("Original adapter cache status retained"),E.Geometry[0].CacheStatus,FString(TEXT("hit")));
    TestFalse(TEXT("Missing flotation detail stays unavailable"),E.Geometry[0].RunningHeave.Value.IsSet());TestEqual(TEXT("Original bytes source SHA256 retained"),E.SourceSHA256.Len(),64);
    const auto Kept=E.SourceSHA256;
    auto Bad=Object(Source);Bad->SetStringField(TEXT("axis_order"),TEXT("ZYX"));TestFalse(TEXT("Foreign axes are not reordered silently"),StudioHome4SpatialDiagnostics::Parse(JSON(Bad),Run,E,Error));
    Bad=Object(Source);Bad->GetArrayField(TEXT("patches"))[1]->AsObject()->SetStringField(TEXT("id"),TEXT("root"));TestFalse(TEXT("Duplicate patch IDs rejected"),StudioHome4SpatialDiagnostics::Parse(JSON(Bad),Run,E,Error));
    Bad=Object(Source);auto DuplicatedZones=Bad->GetArrayField(TEXT("zones"));const auto DuplicatedZone=DuplicatedZones[0];DuplicatedZones.Add(DuplicatedZone);Bad->SetArrayField(TEXT("zones"),DuplicatedZones);TestFalse(TEXT("Duplicate zone identities rejected"),StudioHome4SpatialDiagnostics::Parse(JSON(Bad),Run,E,Error));
    Bad=Object(Source);Bad->GetArrayField(TEXT("levels"))[1]->AsObject()->SetNumberField(TEXT("level"),0);TestFalse(TEXT("Duplicate level IDs rejected"),StudioHome4SpatialDiagnostics::Parse(JSON(Bad),Run,E,Error));
    Bad=Object(Source);Bad->GetArrayField(TEXT("patches"))[0]->AsObject()->SetStringField(TEXT("run_id"),FGuid::NewGuid().ToString());TestFalse(TEXT("Nested foreign run cannot contaminate patches"),StudioHome4SpatialDiagnostics::Parse(JSON(Bad),Run,E,Error));
    Bad=Object(Source);Bad->GetArrayField(TEXT("zones"))[0]->AsObject()->GetObjectField(TEXT("profile"))->SetArrayField(TEXT("x"),{MakeShared<FJsonValueNumber>(0),MakeShared<FJsonValueNumber>(0),MakeShared<FJsonValueNumber>(2)});TestFalse(TEXT("Regressing profile coordinate rejected"),StudioHome4SpatialDiagnostics::Parse(JSON(Bad),Run,E,Error));
    Bad=Object(Source);Bad->GetArrayField(TEXT("levels"))[0]->AsObject()->GetObjectField(TEXT("nu"))->RemoveField(TEXT("unit"));TestFalse(TEXT("No guessed measured quantity unit"),StudioHome4SpatialDiagnostics::Parse(JSON(Bad),Run,E,Error));
    Bad=Object(Source);Bad->SetStringField(TEXT("solver_state"),TEXT("running"));TestFalse(TEXT("Unknown version-one envelope field rejected"),StudioHome4SpatialDiagnostics::Parse(JSON(Bad),Run,E,Error));
    TestFalse(TEXT("Foreign expected run rejected"),StudioHome4SpatialDiagnostics::Parse(Source,FGuid::NewGuid(),E,Error));TestEqual(TEXT("Failures preserve exact prior provenance"),E.SourceSHA256,Kept);
    FString Nonfinite=Source;Nonfinite.ReplaceInline(TEXT("0.0001"),TEXT("1e999"));TestFalse(TEXT("Nonfinite original measurements rejected before tree construction"),StudioHome4SpatialDiagnostics::Parse(Nonfinite,Run,E,Error));
    TestFalse(TEXT("Duplicate decoded property rejected"),StudioHome4SpatialDiagnostics::Parse(TEXT("{\"vers\\u0069on\":1,")+Source.Mid(1),Run,E,Error));
    FString Deep=TEXT("{\"future\":")+FString::ChrN(40,TEXT('['))+TEXT("0")+FString::ChrN(40,TEXT(']'))+TEXT("}");TestFalse(TEXT("Deep malformed source bounded"),StudioHome4SpatialDiagnostics::Parse(Deep,{},E,Error));
    FStudioHome4SpatialLocation Location;TestTrue(TEXT("Fine patch location carries identity"),StudioHome4SpatialDiagnostics::Locate(E,TEXT("fine"),FIntVector(2,3,1),Location,Error));
    TestEqual(TEXT("Locate never maps fine patch to root"),Location.Level,2);TestTrue(TEXT("Locate retains exact original run"),Location.RunId==Run);TestEqual(TEXT("Locate exact patch-local source Z"),Location.OriginalCell.Z,1);
    TestFalse(TEXT("Out of bounds cell rejected"),StudioHome4SpatialDiagnostics::Locate(E,TEXT("fine"),FIntVector(8,0,0),Location,Error));
    const FString Empty=FString::Printf(TEXT("{\"schema\":\"LBMStudio.Home4SpatialDiagnostics\",\"version\":1,\"run_id\":\"%s\",\"axis_order\":\"XYZ\",\"coordinate_unit\":\"cells\",\"source_id\":\"artificial-empty-source\"}"),*Run.ToString());
    TestTrue(TEXT("Missing optional diagnostics remain honest"),StudioHome4SpatialDiagnostics::Parse(Empty,Run,E,Error)&&E.Patches.IsEmpty()&&E.Zones.IsEmpty()&&E.Levels.IsEmpty());return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SpatialFile,"Studio.Home4.Spatial.BoundedVerifiedOriginalImportAndRollback",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SpatialFile::RunTest(const FString&)
{
    using namespace StudioHome4SpatialTestFixtures;const auto Run=FGuid::NewGuid();const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-spatial-file-tests")/Run.ToString();IFileManager::Get().MakeDirectory(*Root,true);const FString Path=Root/TEXT("source.json");
    const auto Source=Fixture(Run);TestTrue(TEXT("Write original artificial file"),FFileHelper::SaveStringToFile(Source,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));FStudioHome4SpatialEvidence E;FString Error;
    TestTrue(TEXT("Bounded original file imported"),StudioHome4SpatialDiagnostics::Load(Path,Run,E,Error));const auto SHA=E.SourceSHA256;const auto Bytes=E.OriginalBytes;const auto Stamp=IFileManager::Get().GetTimeStamp(*Path);
    FString Rewrite=Source;Rewrite.ReplaceInline(TEXT("artificial-spatial-test-source"),TEXT("artificial-spatial-test-otherx"));
    TestEqual(TEXT("Rewrite fixture preserves byte count"),Rewrite.Len(),Source.Len());
    TestFalse(TEXT("Same-size preserved-timestamp rewrite detected"),StudioHome4SpatialDiagnostics::LoadWithVerificationForAutomation(Path,Run,E,Error,[&]{FFileHelper::SaveStringToFile(Rewrite,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);IFileManager::Get().SetTimeStamp(*Path,Stamp);}));
    TestEqual(TEXT("Changed source retains original SHA"),E.SourceSHA256,SHA);TestTrue(TEXT("Changed source retains exact original bytes"),E.OriginalBytes==Bytes);
    TArray<uint8> Bad{0xc0,0xaf};FFileHelper::SaveArrayToFile(Bad,*Path);TestFalse(TEXT("Invalid UTF8 cannot enter original evidence"),StudioHome4SpatialDiagnostics::Load(Path,{},E,Error));
    Bad.Init(uint8(' '),8*1024*1024+1);FFileHelper::SaveArrayToFile(Bad,*Path);TestFalse(TEXT("File allocation is bounded at 8 MiB"),StudioHome4SpatialDiagnostics::Load(Path,{},E,Error));
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);TestFalse(TEXT("Cancelled import cannot publish"),StudioHome4SpatialDiagnostics::Load(Path,{},E,Error,Cancel));TestEqual(TEXT("All failures preserve prior evidence"),E.SourceSHA256,SHA);
    IFileManager::Get().DeleteDirectory(*Root,false,true);return !HasAnyErrors();
}
namespace StudioHome4SpatialTestFixtures
{
    class FNativeWorkflow final:public IAutomationLatentCommand
    {
    public:
        explicit FNativeWorkflow(FAutomationTestBase& T):Test(T)
        {
            Run=FGuid::NewGuid();Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-spatial-native")/Run.ToString();IFileManager::Get().MakeDirectory(*Root,true);Path=Root/TEXT("original.json");Bad=Root/TEXT("bad.json");
            FFileHelper::SaveStringToFile(Fixture(Run),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);FFileHelper::SaveStringToFile(TEXT("{}"),*Bad,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Model=MakeShared<FStudioModel>(Root/TEXT("project"));Camera=Model->Project.Camera;Draft=StudioCaseIO::Serialize(Model->Project.Draft);Session=MakeShared<FStudioHome4SpatialSession>(Model);
            Panel=SNew(SStudioHome4SpatialDiagnostics).Model(Model).Session(Session).ExpectedRunId(Run).OnLocate_Lambda([this](const auto& L){Location=L;++Located;});
            UI=MakeUnique<FStudioHeadlessSlate>(Test,Panel.ToSharedRef(),FVector2D(700,950));Panel->BeginImportPath(Path);Started=FPlatformTime::Seconds();
        }
        ~FNativeWorkflow(){UI.Reset();Panel.Reset();Session.Reset();Model.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);}
        bool Update()override
        {
            Session->Poll();if(FPlatformTime::Seconds()-Started>20){Test.AddError(TEXT("Spatial import exceeded deadline."));return true;}if(Session->IsImporting())return false;
            UI->Layout();
            if(Stage==0)
            {
                if(!Test.TestTrue(TEXT("Native original source attached"),Session->Evidence().IsValid()))return true;
                Test.TestTrue(TEXT("Publication stamps current project attachment"),Session->Evidence()->AttachedProjectId.IsSet()&&*Session->Evidence()->AttachedProjectId==Model->Project.Id);
                Test.TestTrue(TEXT("Publication stamps current case attachment"),Session->Evidence()->AttachedCaseId.IsSet()&&*Session->Evidence()->AttachedCaseId==Model->Project.Draft.Id);
                Test.TestTrue(TEXT("Source run visible"),UI->Text(TEXT("Home4SpatialSource")).Contains(Run.ToString()));Test.TestTrue(TEXT("No science validation inferred"),UI->Text(TEXT("Home4SpatialSource")).Contains(TEXT("not evaluated")));
                if(!UI->Press(TEXT("Home4SpatialNextPatch"))||!UI->Press(TEXT("Home4SpatialPlane1"))||!UI->Press(TEXT("Home4SpatialLevel2")))return true;
                Test.TestTrue(TEXT("Fine patch explicitly selected"),UI->Text(TEXT("Home4SpatialPatch")).Contains(TEXT("fine · level 2")));Test.TestTrue(TEXT("Original missing level tau shown unavailable"),UI->Text(TEXT("Home4SpatialLevels")).Contains(TEXT("tau Unavailable")));
                if(!UI->Type(TEXT("Home4SpatialCell"),TEXT("2,3,1"))||!UI->Press(TEXT("Home4SpatialLocate")))return true;
                Test.TestEqual(TEXT("Explicit locator action routed once"),Located,1);Test.TestEqual(TEXT("Locator fine level preserved"),Location.Level,2);Test.TestTrue(TEXT("Locator original run identity exact"),Location.RunId==Run);
                if(!UI->Type(TEXT("Home4SpatialCell"),TEXT("8,0,0"))||!UI->Press(TEXT("Home4SpatialLocate")))return true;
                Test.TestEqual(TEXT("Invalid original indices do not route"),Located,1);
                Test.TestTrue(TEXT("Source profile samples are visible"),UI->Text(TEXT("Home4SpatialZone")).Contains(TEXT("3 original samples")));Preserved=Session->Evidence();Session->BeginImport(Bad,Run);Stage=1;return false;
            }
            if(Stage==1)
            {
                Test.TestTrue(TEXT("Bad import retains shared original evidence"),Session->Evidence()==Preserved);Test.TestTrue(TEXT("Bad import cause visible"),UI->Text(TEXT("Home4SpatialStatus")).Contains(TEXT("retained")));
                Test.TestTrue(TEXT("Read-only evidence never edits case"),StudioCaseIO::Serialize(Model->Project.Draft)==Draft);Test.TestTrue(TEXT("Failed import never changes camera"),StudioView::CameraEquals(Model->Project.Camera,Camera));
                const auto Other=SNew(SStudioHome4SpatialDiagnostics).Model(Model).Session(Session).View(EStudioHome4SpatialView::Zones);Test.TestTrue(TEXT("Other pages share the same source controller"),Other->SharedSession()==Session);
                Session->BeginImport(Path,Run);Session->Cancel();Stage=2;return false;
            }
            if(Stage==2)
            {
                Test.TestTrue(TEXT("Cancelled read retains prior shared evidence"),Session->Evidence()==Preserved);
                Session->BeginImport(Path,Run);Model->Project.Draft.Id=FGuid::NewGuid();
                Test.TestFalse(TEXT("Evidence getter rejects stale case before another Poll"),Session->Evidence().IsValid());Stage=3;return false;
            }
            Test.TestFalse(TEXT("Case change clears prior shared evidence"),Session->Evidence().IsValid());Test.TestTrue(TEXT("Pending old case evidence cannot attach"),Session->Status().Contains(TEXT("not attached")));return true;
        }
    private:
        FAutomationTestBase& Test;FGuid Run;FString Root,Path,Bad,Draft;double Started=0;int32 Stage=0,Located=0;FStudioCameraState Camera;
        FStudioHome4SpatialLocation Location;TSharedPtr<FStudioModel> Model;TSharedPtr<FStudioHome4SpatialSession> Session;TSharedPtr<SStudioHome4SpatialDiagnostics> Panel;TSharedPtr<const FStudioHome4SpatialEvidence,ESPMode::ThreadSafe> Preserved;TUniquePtr<FStudioHeadlessSlate> UI;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SpatialNative,"Studio.Home4.Spatial.NativeSharedSourceProfilesLocateAndScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SpatialNative::RunTest(const FString&)
{if(!TestTrue(TEXT("Slate initialized"),FSlateApplication::IsInitialized()))return false;ADD_LATENT_AUTOMATION_COMMAND(StudioHome4SpatialTestFixtures::FNativeWorkflow(*this));return true;}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SpatialReport,"Studio.Home4.Spatial.ReportOriginalBytesAndAttachmentScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SpatialReport::RunTest(const FString&)
{
    using namespace StudioHome4SpatialTestFixtures;
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-spatial-reports")/FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*Root,true);
    auto Model=MakeShared<FStudioModel>(Root/TEXT("model"));FStudioHome4Session Settings(Model);
    if(!TestTrue(TEXT("Apply artificial test recipe"),Settings.ApplyRecipe(TEXT("th01-hull"))))return false;
    const FGuid Run=FGuid::NewGuid();const FString Original=Root/TEXT("original.json");
    const FTCHARToUTF8 Source(*(Fixture(Run)+TEXT("\n")));TArray<uint8> Bytes{0xef,0xbb,0xbf};
    Bytes.Append(reinterpret_cast<const uint8*>(Source.Get()),Source.Length());
    TestTrue(TEXT("Write exact source with UTF8 BOM and trailing newline"),FFileHelper::SaveArrayToFile(Bytes,*Original));
    FStudioHome4SpatialEvidence E;FString Error,Destination;
    if(!TestTrue(TEXT("Load original retained evidence"),StudioHome4SpatialDiagnostics::Load(Original,Run,E,Error)))return false;
    TestFalse(TEXT("Unattached source cannot be exported into current case"),StudioHome4Reports::Export(Root,TEXT("unattached"),Model->SnapshotProject(),nullptr,Destination,Error,nullptr,&E));
    TestFalse(TEXT("Rejected attachment creates no report directory"),IFileManager::Get().DirectoryExists(*(Root/TEXT("unattached"))));
    E.AttachedProjectId=Model->Project.Id;E.AttachedCaseId=Model->Project.Draft.Id;
    TestTrue(TEXT("Scoped original spatial source exports atomically"),StudioHome4Reports::Export(Root,TEXT("original-report"),Model->SnapshotProject(),nullptr,Destination,Error,nullptr,&E));
    TArray<uint8> Exported;
    TestTrue(TEXT("Read original report source"),FFileHelper::LoadFileToArray(Exported,*(Destination/TEXT("spatial-diagnostics.json"))));
    TestTrue(TEXT("Report retains exact original BOM, newline and bytes"),Exported==Bytes);
    FString Metadata;FFileHelper::LoadFileToString(Metadata,*(Destination/TEXT("report.json")));
    const auto O=Object(Metadata);const auto P=O->GetObjectField(TEXT("imported_spatial_diagnostics"));
    TestEqual(TEXT("Report original run identified independently"),P->GetStringField(TEXT("run_id")),Run.ToString());
    TestEqual(TEXT("Report original source SHA256 identified"),P->GetStringField(TEXT("source_sha256")),E.SourceSHA256);
    TestEqual(TEXT("Report gate never inferred from diagnostics"),P->GetStringField(TEXT("gate_status")),FString(TEXT("not_evaluated")));
    TestEqual(TEXT("Report explicit project attachment"),P->GetStringField(TEXT("attached_project_id")),Model->Project.Id.ToString());
    auto WrongScope=E;WrongScope.AttachedCaseId=FGuid::NewGuid();
    TestFalse(TEXT("Foreign case cannot transfer report evidence"),StudioHome4Reports::Export(Root,TEXT("foreign-case"),Model->SnapshotProject(),nullptr,Destination,Error,nullptr,&WrongScope));
    WrongScope=E;WrongScope.AttachedProjectId=FGuid::NewGuid();
    TestFalse(TEXT("Foreign project cannot transfer report evidence"),StudioHome4Reports::Export(Root,TEXT("foreign-project"),Model->SnapshotProject(),nullptr,Destination,Error,nullptr,&WrongScope));
    auto Tampered=E;Tampered.OriginalBytes.Last()=uint8(' ');
    TestFalse(TEXT("Mutated retained bytes fail original hash verification"),StudioHome4Reports::Export(Root,TEXT("tampered-bytes"),Model->SnapshotProject(),nullptr,Destination,Error,nullptr,&Tampered));
    Tampered=E;Tampered.RunId=FGuid::NewGuid();
    TestFalse(TEXT("Mutated retained run fails original identity verification"),StudioHome4Reports::Export(Root,TEXT("tampered-run"),Model->SnapshotProject(),nullptr,Destination,Error,nullptr,&Tampered));
    Tampered=E;Tampered.SourceId=TEXT("foreign-source");
    TestFalse(TEXT("Mutated retained source fails original identity verification"),StudioHome4Reports::Export(Root,TEXT("tampered-source"),Model->SnapshotProject(),nullptr,Destination,Error,nullptr,&Tampered));
    TestFalse(TEXT("Published spatial report cannot be overwritten"),StudioHome4Reports::Export(Root,TEXT("original-report"),Model->SnapshotProject(),nullptr,Destination,Error,nullptr,&E));
    IFileManager::Get().DeleteDirectory(*Root,false,true);return !HasAnyErrors();
}
#endif
