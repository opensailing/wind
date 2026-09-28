#include "StudioMeshImport.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    FStudioMeshImportResult ParseMesh(const FString& Text,const FString& Format=TEXT("obj"))
    {
        const FTCHARToUTF8 Data(*Text);auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
        return StudioMeshImport::Parse(MakeArrayView(reinterpret_cast<const uint8*>(Data.Get()),Data.Length()),Format,Cancel);
    }
    FString MeshTestDirectory()
    {
#if WITH_EDITOR
        const FString Base=FPaths::ProjectDir()/TEXT("tmp/debug/mesh-import-tests");
#else
        const FString Base=FPaths::ProjectSavedDir()/TEXT("Automation/MeshImport");
#endif
        const FString Root=FPaths::ConvertRelativePathToFull(Base/FGuid::NewGuid().ToString());IFileManager::Get().MakeDirectory(*Root,true);return Root;
    }
    bool DrainMesh(FStudioModel& Model)
    {
        const double Deadline=FPlatformTime::Seconds()+10.;
        do {Model.Tick(0);if(!Model.IsReadingGeometry())return true;FPlatformProcess::Sleep(.001f);}while(FPlatformTime::Seconds()<Deadline);
        return false;
    }
    const FString TriangleOBJ=TEXT("v 0 0 0\nv 1000 0 0\nv 0 500 0\ng Wing\nf 1 2 3\n");
    void WriteMesh(const FString& Path,const FString& Content=TriangleOBJ)
    {FFileHelper::SaveStringToFile(Content,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMeshFormats,"Studio.Geometry.SourceFormatsAndTriangulation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioMeshFormats::RunTest(const FString&)
{
    const auto OBJ=ParseMesh(TEXT("# Concave polygon, known area 3\nv 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\ng wing\nf -5 -4 -3 -2 -1\n"));
    TestTrue(TEXT("Concave negative-index OBJ parses"),OBJ.IsValid());if(!OBJ.IsValid())return false;
    TestEqual(TEXT("Ear clipping produces n-2 triangles"),OBJ.Mesh->Indices.Num(),9);
    double Area=0;for(int32 I=0;I<9;I+=3){const auto& M=*OBJ.Mesh;const auto A=M.Positions[M.Indices[I]],B=M.Positions[M.Indices[I+1]],C=M.Positions[M.Indices[I+2]];Area+=FVector::CrossProduct(B-A,C-A).Z*.5;}
    TestEqual(TEXT("Original polygon area and winding retained"),Area,3.);
    TestEqual(TEXT("Unused patch removed"),OBJ.Mesh->PatchNames.Num(),1);TestEqual(TEXT("Group retained"),OBJ.Mesh->PatchNames[0],FString(TEXT("wing")));
    const auto ASCII=ParseMesh(TEXT("solid wing\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid wing\n"),TEXT("stl"));
    TestTrue(TEXT("ASCII STL parses"),ASCII.IsValid());if(!ASCII.IsValid())return false;
    TestEqual(TEXT("Known source coordinate"),ASCII.Mesh->Positions[1],FVector(1,0,0));TestEqual(TEXT("Open triangle boundary reported"),ASCII.Mesh->BoundaryEdges,3);
    TArray<uint8> Binary;Binary.Init(0,134);FMemory::Memcpy(Binary.GetData(),"solid binary",12);Binary[80]=1;
    const float Values[12]={0,0,1,0,0,0,1,0,0,0,1,0};FMemory::Memcpy(Binary.GetData()+84,Values,48);
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);const auto STL=StudioMeshImport::Parse(Binary,TEXT("stl"),Cancel);
    TestTrue(TEXT("Binary STL with solid header is detected by size/count"),STL.IsValid());if(!STL.IsValid())return false;
    TestEqual(TEXT("ASCII and binary vertices agree"),STL.Mesh->Positions,ASCII.Mesh->Positions);
    TestEqual(TEXT("Original bytes have distinct identities"),STL.SHA256==ASCII.SHA256,false);
    TestTrue(TEXT("OBJ normal/texture index notation"),ParseMesh(TEXT("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\nf 1/1/1 2/1/1 3/1/1")).IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMeshFailures,"Studio.Geometry.DiagnosticsAndRejectedInput",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioMeshFailures::RunTest(const FString&)
{
    for(const auto& Text:TArray<FString>{TEXT("v nan 0 0"),TEXT("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 2 3"),TEXT("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4"),TEXT("v 0 0 0\nv 1 0 0\nv 2 0 0\nf 1 2 3"),TEXT("v 0 0 0\nv 1 0 0\nv 1 1 1\nv 0 1 0\nf 1 2 3 4"),TEXT("curv 0 1 2")})
    {const auto R=ParseMesh(Text);TestFalse(TEXT("Malformed or unsupported source is rejected"),R.IsValid());TestFalse(TEXT("Failure has actionable detail"),R.Error.IsEmpty());}
    const auto Dup=ParseMesh(TriangleOBJ+TEXT("f 1 2 3\n"));TestTrue(TEXT("Duplicate faces remain inspectable"),Dup.IsValid());if(Dup.IsValid()){TestEqual(TEXT("Duplicate face diagnosed"),Dup.Mesh->DuplicateFaces,1);TestEqual(TEXT("Same-direction paired edges diagnosed"),Dup.Mesh->InconsistentEdges,3);}
    const auto Nonmanifold=ParseMesh(TriangleOBJ+TEXT("v 0 -1 0\nv 0 0 1\nf 2 1 4\nf 1 2 5\n"));if(TestTrue(TEXT("Nonmanifold source inspectable"),Nonmanifold.IsValid()))TestEqual(TEXT("Shared-by-three edge diagnosed"),Nonmanifold.Mesh->NonmanifoldEdges,1);
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);TArray<uint8> Bytes{1,2,3};const auto Stopped=StudioMeshImport::Parse(Bytes,TEXT("obj"),Cancel);
    TestTrue(TEXT("Cancelled parse explicitly reported"),Stopped.bCancelled);TestFalse(TEXT("Cancelled source never usable"),Stopped.IsValid());
    TestFalse(TEXT("Wrong format rejected"),ParseMesh(TriangleOBJ,TEXT("step")).IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMeshCommit,"Studio.Geometry.ImportUndoSaveReopen",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioMeshCommit::RunTest(const FString&)
{
    const FString Root=MeshTestDirectory(),Path=Root/TEXT("wing.obj"),ProjectPath=Root/TEXT("case.lbms");WriteMesh(Path);
    FStudioModel M(Root/TEXT("session"));M.Scrub(.5);const auto Camera=M.Project.Camera;const int32 Frame=M.SelectedFrame;const FString Dataset=M.Project.Dataset;const int32 Revision=M.Revision;
    const FString Before=StudioCaseIO::Serialize(M.Project.Draft);
    TestTrue(TEXT("Start asynchronous import"),M.RequestGeometryImport(Path));TestEqual(TEXT("Read never commits the case"),StudioCaseIO::Serialize(M.Project.Draft),Before);
    TestTrue(TEXT("Worker finishes"),DrainMesh(M));TestTrue(TEXT("Source preview ready"),M.GeometrySource.IsValid());
    TestFalse(TEXT("Unknown source units block commit"),M.CommitGeometryImport());
    M.ImportOptions.MetersPerUnit=.001;M.ImportOptions.UpAxis=1;M.ImportOptions.ForwardAxis=0;
    FStudioGeometryAsset Preview;FString Error;TestTrue(TEXT("Preview transform valid"),M.GeometryAssetForPreview(Preview,Error));
    TestTrue(TEXT("Source +Y maps to world +Z"),Preview.Rotation.RotateVector(FVector(0,1,0)).Equals(FVector(0,0,1),1.e-9));
    const FBox Bounds=StudioMeshImport::TransformedBounds(*M.GeometrySource.Mesh,Preview);TestTrue(TEXT("Known millimeter dimensions become meters"),Bounds.GetSize().Equals(FVector(1,0,.5),1.e-9));
    TestTrue(TEXT("Commit succeeds"),M.CommitGeometryImport());TestEqual(TEXT("One mesh added"),M.Project.Draft.Geometry.Num(),1);const auto Added=M.Project.Draft.Geometry[0];
    TestEqual(TEXT("Recording remains selected"),M.Project.Dataset,Dataset);TestEqual(TEXT("Frame remains selected"),M.SelectedFrame,Frame);TestEqual(TEXT("Field revision unchanged"),M.Revision,Revision);TestTrue(TEXT("Solve camera unchanged"),StudioView::CameraEquals(Camera,M.Project.Camera));
    TestTrue(TEXT("Undo import"),M.UndoCase());TestEqual(TEXT("Undo removes object"),M.Project.Draft.Geometry.Num(),0);TestTrue(TEXT("Redo import"),M.RedoCase());TestEqual(TEXT("Redo preserves patch IDs"),M.Project.Draft.Geometry[0].Patches[0].Id,Added.Patches[0].Id);
    TestTrue(TEXT("Save imported case"),M.SaveProject(ProjectPath));TestTrue(TEXT("Reopen imported case"),M.LoadProject(ProjectPath));
    TestTrue(TEXT("Select saved geometry"),M.SelectGeometry(Added.Id));TestTrue(TEXT("Reload finishes"),DrainMesh(M));TestTrue(TEXT("Original source reverified"),M.GeometrySource.IsValid());TestEqual(TEXT("Saved scale retained"),M.Project.Draft.Geometry[0].MetersPerSourceUnit,.001);
    WriteMesh(Path,TriangleOBJ+TEXT("# changed original\n"));TestTrue(TEXT("Select changed file"),M.SelectGeometry(Added.Id));TestTrue(TEXT("Changed source read drains"),DrainMesh(M));TestFalse(TEXT("Changed bytes never presented under saved identity"),M.GeometrySource.IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMeshStale,"Studio.Geometry.CancelAndStaleCompletion",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioMeshStale::RunTest(const FString&)
{
    const FString Root=MeshTestDirectory(),Path=Root/TEXT("wing.obj");WriteMesh(Path);FStudioModel M(Root/TEXT("session"));
    const FString Before=StudioCaseIO::Serialize(M.Project.Draft);
    TestTrue(TEXT("Request preview"),M.RequestGeometryImport(Path));M.CancelGeometryImport();TestTrue(TEXT("Cancelled work drains"),DrainMesh(M));TestFalse(TEXT("Cancelled work cannot appear"),M.GeometrySource.IsValid());TestEqual(TEXT("Cancel preserves case"),StudioCaseIO::Serialize(M.Project.Draft),Before);
    TestTrue(TEXT("Request before edit"),M.RequestGeometryImport(Path));TestTrue(TEXT("Independent case edit"),M.EditCase(TEXT("Rename case"),[](auto& D){D.Name=TEXT("Edited");}));DrainMesh(M);TestFalse(TEXT("Case edit invalidates old preview"),M.GeometrySource.IsValid());
    TestTrue(TEXT("Request before new project"),M.RequestGeometryImport(Path));M.NewProject(TEXT("New project"));DrainMesh(M);TestFalse(TEXT("Project switch invalidates old preview"),M.GeometrySource.IsValid());TestEqual(TEXT("New case has no imported mesh"),M.Project.Draft.Geometry.Num(),0);
    return true;
}
#endif
