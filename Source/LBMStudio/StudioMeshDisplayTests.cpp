#include "StudioMeshDisplay.h"
#include "StudioModel.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioSnapshot.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto MeshTestFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString MeshJSON(const TSharedRef<FJsonObject>& O)
{FString Text;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));return Text;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioOriginalMesh,"Studio.MeshDisplay.OriginalTopologyAndCancellation",MeshTestFlags)
bool FStudioOriginalMesh::RunTest(const FString&)
{
    FRecordedSolver Solver;
    const auto Field=Solver.CaptureViewField(0,TEXT("pressure"),true);
    if(!TestTrue(TEXT("Authentic SU2 recording loaded"),Field->IsValid()))return false;
    const auto Mesh=StudioMeshDisplay::Build(*Field);
    // Independently read the original connectivity and node array. The render
    // extractor is not used to construct the expected geometry.
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*(FPaths::ProjectContentDir()/TEXT("Samples/MeshGraphNets_Airfoil/flow.bin"))));
    if(!TestTrue(TEXT("Original recording available"),bool(Reader)))return false;
    int32 Magic,Version,Nodes,Triangles,Boundary,Frames;
    *Reader<<Magic<<Version<<Nodes<<Triangles<<Boundary<<Frames;
    if(!TestTrue(TEXT("Known source format"),Magic==0x53553246&&Version==2&&Nodes>0&&Triangles>0))return false;
    TArray<FVector2D> Source;Source.SetNum(Nodes);for(auto& P:Source)*Reader<<P.X<<P.Y;
    TestEqual(TEXT("Every original triangle is retained"),Mesh.TriangleCount,Triangles);
    TestEqual(TEXT("Each face has independent barycentric vertices"),Mesh.PositionsMeters.Num(),Triangles*3);
    TestFalse(TEXT("Source mesh is not labelled derived"),Mesh.bDerived);
    const FVector Offset=Solver.Descriptor().SourceOffset;
    for(int32 Face=0;Face<Triangles;++Face)
    {
        FIntVector T;*Reader<<T.X<<T.Y<<T.Z;
        for(int32 I=0;I<3;++I)
        {
            const FVector Expected(Source[T[I]].X+Offset.X,Offset.Y,Source[T[I]].Y+Offset.Z);
            if(!TestEqual(TEXT("Exact original positions, connectivity and scene-axis mapping"),Mesh.PositionsMeters[Face*3+I],Expected)||
                !TestEqual(TEXT("Independent triangle index"),Mesh.Indices[Face*3+I],Face*3+I))return false;
        }
    }
    const auto Later=StudioMeshDisplay::Build(*Solver.CaptureViewField(600,TEXT("density"),true));
    TestTrue(TEXT("Frame/scalar selection cannot deform topology"),Later.PositionsMeters==Mesh.PositionsMeters&&Later.Indices==Mesh.Indices);
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    const auto Stopped=StudioMeshDisplay::Build(*Field,Cancel);
    TestTrue(TEXT("Cancelled mesh publishes no partial geometry"),Stopped.PositionsMeters.IsEmpty()&&Stopped.Indices.IsEmpty()&&!Stopped.TriangleCount);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioDerivedMesh,"Studio.MeshDisplay.VerifiedDerivedTopologyAndAbsentConnectivity",MeshTestFlags)
bool FStudioDerivedMesh::RunTest(const FString&)
{
    const auto Point=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Point.Error,Point.Reference.IsSet()&&Point.Source.IsValid()))return false;
    const auto Raw=StudioMeshDisplay::Build(*Point.Source->CaptureViewField(0,TEXT("pressure"),true));
    TestTrue(TEXT("Original point rows are not implicitly connected"),Raw.TriangleCount==0&&Raw.Notice.Contains(TEXT("No triangle mesh supplied")));
    const auto Derived=StudioRecordings::ImportReconstruction(*Point.Reference,FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture/reconstruction.json"),0,{});
    if(!TestTrue(*Derived.Error,Derived.Source.IsValid()))return false;
    const auto Field=Derived.Source->CaptureViewField(0,TEXT("pressure"),true);
    const auto Mesh=StudioMeshDisplay::Build(*Field);const auto Surface=Derived.Source->Reconstruction()->Surface;
    TestTrue(TEXT("Derived topology is identified explicitly"),Mesh.bDerived&&Mesh.Notice.Contains(TEXT("Derived")));
    TestEqual(TEXT("Verified face count"),Mesh.TriangleCount,37188);
    const auto& Positions=Surface->Geometry().Positions;const auto& Faces=Surface->Triangles();
    for(int32 Face=0;Face<Faces.Num();++Face)for(int32 I=0;I<3;++I)
    {
        const auto P=Positions[Faces[Face][I]];
        if(!TestEqual(TEXT("Derived mesh retains exact original coordinates and verified faces"),Mesh.PositionsMeters[Face*3+I],FVector(P.X,P.Z,P.Y)))return false;
    }
    const auto Volume=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Volume.Error,Volume.Source.IsValid()))return false;
    TestEqual(TEXT("3D points do not manufacture a surface mesh"),Volume.Source->CaptureViewField(0,TEXT("pressure"),true)->MeshTriangleCount(),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMeshPersistence,"Studio.MeshDisplay.PersistenceHistoryAndLayoutIsolation",MeshTestFlags)
bool FStudioMeshPersistence::RunTest(const FString&)
{
    FStudioProject P,Loaded;P.View.MeshStyle=2;P.View.bMesh=true;FString Error;
    TestTrue(*Error,StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Loaded,Error));
    TestEqual(TEXT("Mesh mode round trips"),Loaded.View.MeshStyle,2);
    TSharedPtr<FJsonObject> JSON;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),JSON);
    auto View=JSON->GetObjectField(TEXT("view"));
    for(double Bad:{-1.,3.,1.5})
    {
        View->SetNumberField(TEXT("meshStyle"),Bad);
        TestFalse(TEXT("Unknown/fractional mesh mode rejected"),StudioProjectIO::Parse(MeshJSON(JSON.ToSharedRef()),Loaded,Error));
        TestEqual(TEXT("Invalid document leaves previous project intact"),Loaded.View.MeshStyle,2);
    }
    View->RemoveField(TEXT("meshStyle"));
    TestFalse(TEXT("Current schema requires explicit mesh mode"),StudioProjectIO::Parse(MeshJSON(JSON.ToSharedRef()),Loaded,Error));
    JSON->SetNumberField(TEXT("version"),15);
    TestTrue(TEXT("Previous schema migrates"),StudioProjectIO::Parse(MeshJSON(JSON.ToSharedRef()),Loaded,Error));
    TestTrue(TEXT("Legacy floor grid remains distinct from triangle mesh"),Loaded.View.MeshStyle==0&&Loaded.View.bMesh);
    const FString Dir=FPaths::ProjectSavedDir()/TEXT("Automation/MeshDisplay")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir);const auto Before=M.InspectionState();const int32 Frame=M.SelectedFrame;
    M.EditView(TEXT("Mesh display"),[](auto& S){S.Display.MeshStyle=1;});
    TestTrue(TEXT("Mesh display participates in shared undo"),M.UndoView()&&M.MeshStyle==Before.Display.MeshStyle);
    TestTrue(TEXT("Redo restores overlay"),M.RedoView()&&M.MeshStyle==1);
    TestTrue(TEXT("Mesh mode retains camera and replay"),StudioView::CameraEquals(M.Project.Camera,Before.Camera)&&M.SelectedFrame==Frame);
    const FString Document=StudioProjectIO::Serialize(M.SnapshotProject());const int32 Revision=M.Revision;
    M.bViewportExpanded=true;M.SaveSession();FStudioModel Reopened(Dir);Reopened.OpenSession();
    TestTrue(TEXT("Expanded viewport preference persists"),Reopened.bViewportExpanded);
    TestTrue(TEXT("Layout never edits scientific document or regenerates field"),Document==StudioProjectIO::Serialize(M.SnapshotProject())&&Revision==M.Revision);
    return true;
}
#endif
