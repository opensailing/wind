#include "StudioDomain.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags DomainFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString DomainDirectory()
{const FString Dir=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/DomainModel")/FGuid::NewGuid().ToString());IFileManager::Get().MakeDirectory(*Dir,true);return Dir;}
bool DomainFixture(const FString& Dir,FStudioGeometryAsset& Asset)
{
    const FString Path=Dir/TEXT("bounds-fixture.obj");
    // Deliberate geometric test fixture. It contains no scientific output.
    if(!FFileHelper::SaveStringToFile(TEXT("# Domain geometry test fixture; no CFD\nv 0 0 0\nv 2 0 0\nv 0 1 0\nv 0 0 3\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n"),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return false;
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const auto Source=StudioMeshImport::Read(Path,Cancel);FStudioMeshImportOptions Options;Options.Name=TEXT("Bounds fixture");Options.MetersPerUnit=1.;
    FString Error;return StudioMeshImport::MakeAsset(Source,Options,Asset,Error);
}
FString DomainJSON(const TSharedPtr<FJsonObject>& Json)
{FString Text;FJsonSerializer::Serialize(Json.ToSharedRef(),TJsonWriterFactory<>::Create(&Text));return Text;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioDomainDraftTest,"Studio.Domain.ExactBoundsPaddingAndRejectedDrafts",DomainFlags)
bool FStudioDomainDraftTest::RunTest(const FString&)
{
    FStudioDomain Domain;Domain.Min=FVector(-1.2345678901234567,-2,-3);Domain.Max=FVector(4,5,6);
    FStudioDomainEdit Draft;Draft.Reset(Domain);FStudioDomain Built;
    TestFalse(TEXT("Initial draft is clean"),Draft.IsDirty());
    Draft.FaceNames[0]=TEXT("Inlet");TestTrue(TEXT("Rename builds"),Draft.Build(Built));
    TestEqual(TEXT("Renaming keeps exact bounds"),Built.Min,Domain.Min);
    TestTrue(TEXT("Face identities retained"),Built.Faces==Domain.Faces);
    const FVector Kept=Built.Min;
    for(const TCHAR* Invalid:{TEXT("nan"),TEXT("inf"),TEXT("1e9"),TEXT("-1e9"),TEXT("4m"),TEXT("")})
    {Draft.Minimum[0]=Invalid;TestFalse(TEXT("Invalid bounds rejected"),Draft.Build(Built));TestEqual(TEXT("Rejected output unchanged"),Built.Min,Kept);TestEqual(TEXT("Invalid text retained"),Draft.Minimum[0],FString(Invalid));}
    Draft.Reset(Domain);Draft.Maximum[0]=Draft.Minimum[0];TestFalse(TEXT("Zero dimension rejected"),Draft.Build(Built));
    Draft.Reset(Domain);Draft.FaceNames[1]=TEXT(" -x ");TestFalse(TEXT("Face names unique ignoring case and outer whitespace"),Draft.Build(Built));
    Draft.Reset(Domain);const FBox Planar(FVector(0,0,0),FVector(2,1,0));
    TestFalse(TEXT("Planar geometry needs explicitly entered padding"),Draft.Fit(Planar));
    Draft.Padding[0]=TEXT("1");Draft.Padding[1]=TEXT("2");Draft.Padding[4]=TEXT("0.5");Draft.Padding[5]=TEXT("1.5");
    TestTrue(TEXT("Asymmetric physical padding fits"),Draft.Fit(Planar));TestTrue(TEXT("Fit draft validates"),Draft.Build(Built));
    TestEqual(TEXT("Padded minimum is physical meters"),Built.Min,FVector(-1,0,-.5));
    TestEqual(TEXT("Padded maximum is physical meters"),Built.Max,FVector(4,1,1.5));
    TestTrue(TEXT("Planar geometry contained in padded 3D box"),StudioDomain::Contains(Built,Planar));
    TestFalse(TEXT("Outside object reported"),StudioDomain::Contains(Built,FBox(FVector(3,0,0),FVector(5,1,1))));
    Draft.Padding[2]=TEXT("-0.1");const FString OldMin=Draft.Minimum[0];
    TestFalse(TEXT("Negative padding rejected"),Draft.Fit(Planar));TestEqual(TEXT("Rejected fit retains bounds draft"),Draft.Minimum[0],OldMin);
    auto FluidChange=Domain;FluidChange.FluidMaterialId=FGuid::NewGuid();Draft.Reset(Domain);
    TestTrue(TEXT("Material assignment does not conflict with bounds editor"),Draft.Matches(FluidChange));
    FluidChange.Max.X+=1;TestFalse(TEXT("External bounds change conflicts"),Draft.Matches(FluidChange));
    Draft.Reset(Domain);Draft.SetDimension(0,TEXT("10"));
    TestTrue(TEXT("Dimension edit builds"),Draft.Build(Built));TestEqual(TEXT("Dimension edit anchors exact minimum"),Built.Min,Domain.Min);
    TestTrue(TEXT("Entered X length controls maximum"),FMath::Abs(Built.Max.X-Built.Min.X-10.)<1.e-12);
    Draft.SetDimension(0,TEXT("nan"));TestFalse(TEXT("Invalid dimension remains unapplied"),Draft.Build(Built));
    TestEqual(TEXT("Invalid dimension identifies its own input"),Draft.ErrorField,18);TestTrue(TEXT("Invalid dimension is a pending draft"),Draft.IsDirty());
    Draft.SetCoordinate(0,TEXT("1"));TestTrue(TEXT("Explicit bound edit resolves its dimension draft"),Draft.Build(Built));
    Draft.SetDimension(0,TEXT("200000000"));TestFalse(TEXT("Dimension cannot overflow coordinate range"),Draft.Build(Built));
    TestEqual(TEXT("Overflow focuses dimension"),Draft.ErrorField,18);
    Draft.Reset(Domain);const FString ExactSize=Draft.Dimensions[0];Draft.SetDimension(0,ExactSize);
    TestTrue(TEXT("Re-entering exact size builds"),Draft.Build(Built));TestEqual(TEXT("No drift from unchanged dimension text"),Built.Max,Domain.Max);
    TestFalse(TEXT("Unchanged dimensions leave draft clean"),Draft.IsDirty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioDomainGeometryTest,"Studio.Domain.VerifiedAllObjectBoundsAndCancellation",DomainFlags)
bool FStudioDomainGeometryTest::RunTest(const FString&)
{
    const FString Dir=DomainDirectory();FStudioGeometryAsset First;
    if(!TestTrue(TEXT("Explicit geometry fixture parsed"),DomainFixture(Dir,First)))return false;
    FStudioGeometryAsset Second=First;Second.Id=FGuid::NewGuid();Second.Name=TEXT("Transformed object");
    Second.Translation=FVector(4,-2,1);Second.Scale=FVector(2,3,4);for(auto& Patch:Second.Patches)Patch.Id=FGuid::NewGuid();
    FStudioCaseDraft Case;Case.Geometry={First,Second};auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    auto Result=StudioDomain::InspectGeometry(Case,Cancel);
    TestTrue(TEXT("Both original files verified"),Result.Complete());TestEqual(TEXT("Every object checked"),Result.Objects.Num(),2);
    TestEqual(TEXT("All-object minimum"),Result.Bounds.Min,FVector(0,-2,0));TestEqual(TEXT("All-object maximum"),Result.Bounds.Max,FVector(8,1,13));
    TestTrue(TEXT("Combined preview uses original transformed triangles"),Result.Preview&&Result.Preview->Indices.Num()==24);
    TestEqual(TEXT("Each original preview triangle retains its patch identity"),Result.TriangleTargets.Num(),8);
    if(Result.TriangleTargets.Num()==8)
    {
        for(int32 I=0;I<4;++I)TestEqual(TEXT("First object's original surface ID"),Result.TriangleTargets[I],First.Patches[0].Id);
        for(int32 I=4;I<8;++I)TestEqual(TEXT("Transformed object's independent surface ID"),Result.TriangleTargets[I],Second.Patches[0].Id);
    }
    const auto* PatchBounds=Result.PatchBounds.Find(Second.Patches[0].Id);
    TestTrue(TEXT("Patch bounds follow original triangles and case transform"),PatchBounds&&PatchBounds->Min==FVector(4,-2,1)&&PatchBounds->Max==FVector(8,1,13));
    const FString Key=StudioDomain::GeometryKey(Case);Case.Geometry[0].MaterialId=FGuid::NewGuid();
    TestEqual(TEXT("Material assignment does not invalidate source geometry"),StudioDomain::GeometryKey(Case),Key);
    Case.Geometry[1].SourceSHA256=FString::ChrN(64,TEXT('0'));Result=StudioDomain::InspectGeometry(Case,Cancel);
    TestFalse(TEXT("Changed source disables all-object fit"),Result.Complete());TestFalse(TEXT("Invalid object gives a recovery explanation"),Result.Objects[1].Error.IsEmpty());
    TestEqual(TEXT("Verified partial bounds exclude failed object"),Result.Bounds.Max,FVector(2,1,3));
    Case.Geometry[1]=Second;Case.Geometry[1].Patches[0].Name=TEXT("Altered mapping");Result=StudioDomain::InspectGeometry(Case,Cancel);
    TestFalse(TEXT("Patch mapping mismatch also rejected"),Result.Complete());
    *Cancel=true;Result=StudioDomain::InspectGeometry(Case,Cancel);TestTrue(TEXT("Cancellation observed before file read"),Result.bCancelled);
    TestFalse(TEXT("Cancelled result cannot fit"),Result.Complete());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioDomainPersistenceTest,"Studio.Domain.TransactionsFrozenRunsAndSchemaMigration",DomainFlags)
bool FStudioDomainPersistenceTest::RunTest(const FString&)
{
    FStudioModel M(DomainDirectory());M.Pause();const auto Camera=M.Project.Camera;const auto Frame=M.SelectedFrame;const auto Intent=M.RenderIntentRevision;
    FGuid Fluid;TestTrue(TEXT("Create fluid assignment fixture"),M.AddMaterial(false,Fluid));TestTrue(TEXT("Assign fluid"),M.AssignDomainMaterial(Fluid));
    M.EditCase(TEXT("Harness identity"),[](auto& Case){Case.Setup.BackendId=TEXT("test-control-harness");});
    M.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Frozen domain"),M.Project.Draft,EStudioRunOrigin::ControlHarness));
    const FString Frozen=StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration());
    const auto Original=M.Project.Draft.Domain;auto Edited=Original;Edited.Min.X=-5;Edited.FaceNames[0]=TEXT("Inlet");Edited.FluidMaterialId.Invalidate();
    TestTrue(TEXT("Domain edit is transactional"),M.UpdateDomain(Edited));TestEqual(TEXT("Current fluid assignment preserved"),M.Project.Draft.Domain.FluidMaterialId,Fluid);
    TestEqual(TEXT("One undo restores bounds and names"),M.UndoCase(),true);TestEqual(TEXT("Bounds restored"),M.Project.Draft.Domain.Min,Original.Min);
    TestEqual(TEXT("Face name restored"),M.Project.Draft.Domain.FaceNames[0],Original.FaceNames[0]);TestTrue(TEXT("Redo applies full edit"),M.RedoCase());
    auto Invalid=M.Project.Draft.Domain;Invalid.Faces[0]=FGuid::NewGuid();const FString Before=StudioCaseIO::Serialize(M.Project.Draft);
    TestFalse(TEXT("Resizing cannot replace boundary face identities"),M.UpdateDomain(Invalid));
    Invalid=M.Project.Draft.Domain;Invalid.Max=Invalid.Min;TestFalse(TEXT("Invalid volume rejected"),M.UpdateDomain(Invalid));
    TestEqual(TEXT("Failed edits preserve case"),StudioCaseIO::Serialize(M.Project.Draft),Before);
    TestEqual(TEXT("Frozen run never changes"),StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration()),Frozen);
    TestTrue(TEXT("Flow camera unchanged"),StudioView::CameraEquals(Camera,M.Project.Camera));TestEqual(TEXT("Flow frame unchanged"),Frame,M.SelectedFrame);TestEqual(TEXT("No CFD requested"),Intent,M.RenderIntentRevision);
    FStudioProject Loaded;FString Error;const FString Document=StudioProjectIO::Serialize(M.SnapshotProject());
    TestTrue(TEXT("Schema 17 round trip"),StudioProjectIO::Parse(Document,Loaded,Error));
    TestEqual(TEXT("Exact case round trip"),StudioCaseIO::Serialize(Loaded.Draft),Before);
    TSharedPtr<FJsonObject> Json;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document),Json);
    Json->GetObjectField(TEXT("draft"))->GetObjectField(TEXT("domain"))->RemoveField(TEXT("faceNames"));
    TestFalse(TEXT("Current schema requires face names"),StudioProjectIO::Parse(DomainJSON(Json),Loaded,Error));
    Json->SetNumberField(TEXT("version"),16);TestTrue(TEXT("Legacy schema migrates axis names"),StudioProjectIO::Parse(DomainJSON(Json),Loaded,Error));
    TestEqual(TEXT("Legacy negative X label"),Loaded.Draft.Domain.FaceNames[0],FString(TEXT("-X")));
    TestTrue(TEXT("Migration preserves existing face IDs"),Loaded.Draft.Domain.Faces==Original.Faces);
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document),Json);
    auto Config=Json->GetArrayField(TEXT("runs")).Last()->AsObject()->GetObjectField(TEXT("configuration"));
    Config->GetObjectField(TEXT("domain"))->RemoveField(TEXT("faceNames"));
    TestFalse(TEXT("Current frozen run also requires face names"),StudioProjectIO::Parse(DomainJSON(Json),Loaded,Error));
    Json->SetNumberField(TEXT("version"),16);TestTrue(TEXT("Legacy frozen run migrates without changing IDs"),StudioProjectIO::Parse(DomainJSON(Json),Loaded,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioDomainFaceDragTest,"Studio.Domain.FaceDraggingPhysicalCoordinates",DomainFlags)
bool FStudioDomainFaceDragTest::RunTest(const FString&)
{
    FStudioDomain Domain;Domain.Min=FVector(8,-1,-2);Domain.Max=FVector(12,1,2);
    FStudioCameraState Camera;Camera.Position=FVector::ZeroVector;Camera.Orientation=FQuat::Identity;
    Camera.Focus=FVector(10,0,0);Camera.OrbitDistance=10;Camera.FieldOfView=90;Camera.OrthoWidth=20;
    const FVector2D Size(800,600);
    for(bool Ortho:{false,true})
    {
        Camera.bOrthographic=Ortho;StudioDomain::FFaceDrag Start;FStudioDomain Changed;
        TestTrue(TEXT("Start positive Y face"),StudioDomain::BeginFaceDrag(Domain,3,Camera,Size,FVector2D(440,300),Start));
        TestTrue(TEXT("One meter outward drag"),StudioDomain::DragFace(Start,FVector2D(480,300),Changed));
        TestTrue(TEXT("Physical maximum follows one meter"),Changed.Max.Equals(FVector(12,2,2),1.e-8));
        TestEqual(TEXT("Other side stays exact"),Changed.Min,Domain.Min);TestTrue(TEXT("Stable face IDs"),Changed.Faces==Domain.Faces);
        const FVector Kept=Changed.Max;
        TestFalse(TEXT("Cannot drag through opposite face"),StudioDomain::DragFace(Start,FVector2D(359,300),Changed));
        TestEqual(TEXT("Rejected movement leaves result intact"),Changed.Max,Kept);
        TestTrue(TEXT("Start negative Y face"),StudioDomain::BeginFaceDrag(Domain,2,Camera,Size,FVector2D(360,300),Start));
        TestTrue(TEXT("Negative face moves outward"),StudioDomain::DragFace(Start,FVector2D(320,300),Changed));
        TestTrue(TEXT("Negative minimum follows movement"),Changed.Min.Equals(FVector(8,-2,-2),1.e-8));
        TestTrue(TEXT("Start positive Z face"),StudioDomain::BeginFaceDrag(Domain,5,Camera,Size,FVector2D(400,220),Start));
        TestTrue(TEXT("Upward screen movement increases physical Z"),StudioDomain::DragFace(Start,FVector2D(400,180),Changed));
        TestTrue(TEXT("Positive Z reaches three meters"),FMath::Abs(Changed.Max.Z-3.)<1.e-8);
        TestFalse(TEXT("End-on face has no usable screen direction"),StudioDomain::BeginFaceDrag(Domain,0,Camera,Size,FVector2D(400,300),Start));
        TestFalse(TEXT("Invalid face rejected"),StudioDomain::BeginFaceDrag(Domain,6,Camera,Size,FVector2D(400,300),Start));
    }
    Camera.bOrthographic=false;Camera.Position=FVector(4,5,7);Camera.Orientation=(FVector(10,0,0)-Camera.Position).Rotation().Quaternion();
    FVector2D Begin,End;const FVector Center=StudioDomain::FaceCenter(Domain,3);
    TestTrue(TEXT("Arbitrary camera projects initial face"),StudioCameraPlacement::Project(Camera,Size,Center,Begin,16./9.));
    TestTrue(TEXT("Arbitrary camera projects target face"),StudioCameraPlacement::Project(Camera,Size,Center+FVector(0,.75,0),End,16./9.));
    StudioDomain::FFaceDrag Start;FStudioDomain Changed;
    TestTrue(TEXT("Begin with actual captured aspect"),StudioDomain::BeginFaceDrag(Domain,3,Camera,Size,Begin,Start,16./9.));
    TestTrue(TEXT("Resize from arbitrary perspective"),StudioDomain::DragFace(Start,End,Changed));
    TestTrue(TEXT("Physical result is independent of observer"),FMath::Abs(Changed.Max.Y-1.75)<1.e-8);
    return true;
}

class FStudioDomainReadCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioDomainReadCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        if(Start==0)Start=FPlatformTime::Seconds();
        if(FPlatformTime::Seconds()-Start>15){Test->AddError(TEXT("Domain background check timed out"));return true;}
        if(Test->HasAnyErrors())return true;
        if(!Model)
        {
            const FString Dir=DomainDirectory();FStudioGeometryAsset Asset;if(!Test->TestTrue(TEXT("Async fixture created"),DomainFixture(Dir,Asset)))return true;
            Model=MakeShared<FStudioModel>(Dir);Model->Pause();Model->EditCase(TEXT("Geometry fixture"),[Asset](auto& Case){Case.Geometry.Add(Asset);});
            Test->TestTrue(TEXT("Domain is a sidebar route"),Model->Navigate(EStudioWorkspace::Domain));
            Test->TestTrue(TEXT("Entering starts a background check"),Model->IsReadingDomainGeometry());
            Model->Navigate(EStudioWorkspace::Solve);Model->NewProject(TEXT("Replacement domain"));
            Model->EditCase(TEXT("Replacement geometry fixture"),[Asset](auto& Case){Case.Geometry.Add(Asset);});Phase=1;return false;
        }
        Model->Tick(.01);
        if(Model->IsReadingDomainGeometry())return false;
        if(Phase==1)
        {
            Test->TestFalse(TEXT("Old-project result cannot publish"),Model->DomainGeometry.IsValid());
            Test->TestTrue(TEXT("Fresh project check starts"),Model->RequestDomainGeometry());Phase=2;return false;
        }
        if(Phase==2)
        {
            if(!Test->TestTrue(TEXT("Current result publishes"),Model->DomainGeometry&&Model->DomainGeometry->Complete()))return true;
            const auto Read=Model->DomainGeometry;auto Domain=Model->Project.Draft.Domain;Domain.Max=FVector(5,5,5);
            Test->TestTrue(TEXT("Bounds edit applies"),Model->UpdateDomain(Domain));Test->TestTrue(TEXT("Bounds edit keeps verified mesh cache"),Read==Model->DomainGeometry);
            Test->TestFalse(TEXT("Bounds edit does not reread files"),Model->IsReadingDomainGeometry());
            Model->RequestDomainGeometry();Model->CancelDomainGeometry();Phase=3;return false;
        }
        Test->TestFalse(TEXT("Cancelled check does not publish"),Model->DomainGeometry.IsValid());return true;
    }
private:
    FAutomationTestBase* Test;TSharedPtr<FStudioModel> Model;double Start=0;int32 Phase=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioDomainReadTest,"Studio.Domain.BackgroundReadIdentityAndCancellation",DomainFlags)
bool FStudioDomainReadTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioDomainReadCommand(this));return true;}
#endif
