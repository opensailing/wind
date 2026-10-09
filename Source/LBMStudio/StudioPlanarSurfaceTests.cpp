#include "StudioPlanarSurface.h"
#include "StudioAssets.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioModel.h"
#include "StudioAssetPaths.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto SurfaceFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
TSharedRef<FStudioPointGeometry, ESPMode::ThreadSafe> SurfaceRingPoints()
{
    auto P = MakeShared<FStudioPointGeometry, ESPMode::ThreadSafe>();
    P->Positions = { {-2,-2,0}, {2,-2,0}, {2,2,0}, {-2,2,0},
        {-.5,-.5,0}, {.5,-.5,0}, {.5,.5,0}, {-.5,.5,0} };
    return P;
}
TArray<FIntVector> SurfaceRingFaces()
{ return { {0,1,5}, {0,5,4}, {1,2,6}, {1,6,5}, {2,3,7}, {2,7,6}, {3,0,4}, {3,4,7} }; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceSamplingTest, "Studio.Surface.HolesAndSourceValues", SurfaceFlags)
bool FStudioSurfaceSamplingTest::RunTest(const FString&)
{
    const auto P = SurfaceRingPoints();
    const auto R = FStudioPlanarSurface::Create(P, SurfaceRingFaces());
    if (!TestTrue(*R.Error, R.Surface.IsValid())) return false;
    TArray<double> Values;
    for (const auto& Point : P->Positions) Values.Add(2 * Point.X - 3 * Point.Y + 7);
    for (int32 I = 0; I < P->Positions.Num(); ++I)
    {
        double Value;
        TestTrue(TEXT("Every original vertex is available"), R.Surface->Sample(FVector2D(P->Positions[I].X, P->Positions[I].Y), Values, Value));
        TestEqual(TEXT("Original node value is exact"), Value, Values[I]);
    }
    // An affine scalar is an interpolation identity, not a fabricated CFD fixture.
    for (const auto& Q : { FVector2D(1,.25), FVector2D(-1,-.25), FVector2D(.1,1), FVector2D(.1,-1), FVector2D(2,2) })
    {
        double Value;
        TestTrue(TEXT("Interior and outer boundary sample"), R.Surface->Sample(Q, Values, Value));
        TestTrue(TEXT("Barycentric interpolation preserves affine values"), FMath::IsNearlyEqual(Value, 2*Q.X-3*Q.Y+7, 1.e-12));
        FStudioSurfaceLocation L;
        TestTrue(TEXT("Sample identifies a source triangle"), R.Surface->Locate(Q,L));
        TestTrue(TEXT("Convex weights sum to one"), L.Weights.GetMin()>=0 && FMath::IsNearlyEqual(L.Weights.X+L.Weights.Y+L.Weights.Z,1.,1.e-14));
    }
    for (const auto& Q : { FVector2D(0,0), FVector2D(.49,.49), FVector2D(2.01,0), FVector2D(0,-2.01),
        FVector2D(std::numeric_limits<double>::quiet_NaN(),0) })
    {
        double Value = 0; FStudioSurfaceLocation L;
        TestFalse(TEXT("No interpolation through a solid or outside supplied triangles"), R.Surface->Sample(Q,Values,Value));
        TestFalse(TEXT("Missing sample never becomes a finite zero"), FMath::IsFinite(Value));
        TestFalse(TEXT("No invalid source triangle is returned"), R.Surface->Locate(Q,L));
        TestEqual(TEXT("Failed location resets previous identity"), L.Triangle, INDEX_NONE);
    }
    double Value;
    TestFalse(TEXT("Missing scalar rows are unavailable"), R.Surface->Sample(FVector2D(1,0),{},Value));
    Values[1]=std::numeric_limits<double>::infinity();
    TestFalse(TEXT("Nonfinite source value is unavailable"), R.Surface->Sample(FVector2D(1.5,-1),Values,Value));
    TestTrue(TEXT("Index allocation is reported"),R.Surface->IndexBytes()>0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceIndexTest, "Studio.Surface.BoundsCancellationAndIndex", SurfaceFlags)
bool FStudioSurfaceIndexTest::RunTest(const FString&)
{
    const auto P=SurfaceRingPoints();
    TestFalse(TEXT("Empty topology rejected"),FStudioPlanarSurface::Create(P,{}).Surface.IsValid());
    TestFalse(TEXT("Budget enforced before index allocation"),FStudioPlanarSurface::Create(P,SurfaceRingFaces(),1).Surface.IsValid());
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Cancelled index never publishes"),FStudioPlanarSurface::Create(P,SurfaceRingFaces(),1024*1024,Cancel).Surface.IsValid());
    for(const auto& Invalid : {FIntVector(-1,1,5), FIntVector(0,1,100), FIntVector(0,0,1), FIntVector(0,5,1)})
    {auto Faces=SurfaceRingFaces();Faces[0]=Invalid;TestFalse(TEXT("Invalid triangle rejected"),FStudioPlanarSurface::Create(P,MoveTemp(Faces)).Surface.IsValid());}
    auto NonPlanar=SurfaceRingPoints();NonPlanar->Positions[0].Z=.1;
    TestFalse(TEXT("No silent flattening of 3D source data"),FStudioPlanarSurface::Create(NonPlanar,SurfaceRingFaces()).Surface.IsValid());
    auto NonFinite=SurfaceRingPoints();NonFinite->Positions[0].X=std::numeric_limits<double>::infinity();
    TestFalse(TEXT("Nonfinite coordinates rejected"),FStudioPlanarSurface::Create(NonFinite,SurfaceRingFaces()).Surface.IsValid());

    // Exercise multiple index levels with actual holes between many independent
    // copies of the structural ring. Queries must not leak into another island.
    auto Many=MakeShared<FStudioPointGeometry,ESPMode::ThreadSafe>();TArray<FIntVector> Faces;TArray<double> Values;
    for(int32 I=0;I<256;++I)
    {
        const int32 Base=Many->Positions.Num();
        for(const auto& V:P->Positions){const auto W=V+FVector(I*5,0,0);Many->Positions.Add(W);Values.Add(W.X+2*W.Y);}
        for(const auto& T:SurfaceRingFaces())Faces.Add(T+FIntVector(Base));
    }
    const auto R=FStudioPlanarSurface::Create(Many,MoveTemp(Faces),1024*1024);
    if(!TestTrue(TEXT("Bounded hierarchy created"),R.Surface.IsValid()))return false;
    TestTrue(TEXT("Reported retained storage stays in budget"),R.Surface->IndexBytes()<=1024*1024);
    for(int32 I=0;I<256;++I)
    {
        double Value;const FVector2D Q(I*5+1,.25);
        TestTrue(TEXT("Indexed point finds the correct island"),R.Surface->Sample(Q,Values,Value));
        TestTrue(TEXT("Indexed value uses correct original rows"),FMath::IsNearlyEqual(Value,Q.X+2*Q.Y,1.e-10));
        TestFalse(TEXT("Indexed hole is excluded"),R.Surface->Sample(FVector2D(I*5,0),Values,Value));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceNacaTest, "Studio.Surface.OriginalNacaSamples", SurfaceFlags)
bool FStudioSurfaceNacaTest::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture");
    FString Text;TSharedPtr<FJsonObject> Expected;
    if(!TestTrue(TEXT("Read independently derived expected values"),FFileHelper::LoadFileToString(Text,*(Root/TEXT("expected.json")))&&
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Expected)&&Expected.IsValid()))return false;
    const auto Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);FString Hash,Error;
    if(!TestTrue(TEXT("Pinned reconstructed triangle bytes"),StudioAssets::HashFile(Root/TEXT("triangles.u32"),Cancellation,Hash,Error)&&
        Hash==Expected->GetStringField(TEXT("trianglesSHA256"))))return false;
    if(!TestTrue(TEXT("Pinned reconstruction interpretation"),StudioAssets::HashFile(Root/TEXT("reconstruction.json"),Cancellation,Hash,Error)&&
        Hash==Expected->GetStringField(TEXT("reconstructionSHA256"))))return false;
    const auto Open=StudioPointRecordings::Open(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"));
    if(!TestTrue(*Open.Error,Open.Recording.IsValid()))return false;
    const auto& D=Open.Recording->Descriptor();
    TestEqual(TEXT("Fixture uses the identical original point coordinates"),D.Coordinates.SHA256,Expected->GetStringField(TEXT("coordinatesSHA256")));
    TestEqual(TEXT("Fixture uses the identical original point IDs"),D.PointIds.SHA256,Expected->GetStringField(TEXT("pointIdsSHA256")));
    TArray<uint8> Bytes;if(!TestTrue(TEXT("Read small derived test topology"),FFileHelper::LoadFileToArray(Bytes,*(Root/TEXT("triangles.u32")))))return false;
    if(!TestEqual(TEXT("Expected source-derived topology size"),Bytes.Num(),37188*12))return false;
    TArray<FIntVector> Faces;Faces.Reserve(Bytes.Num()/12);
    auto U32=[&](int32 Offset){return uint32(Bytes[Offset])|(uint32(Bytes[Offset+1])<<8)|(uint32(Bytes[Offset+2])<<16)|(uint32(Bytes[Offset+3])<<24);};
    for(int32 I=0;I<Bytes.Num();I+=12)Faces.Add(FIntVector(int32(U32(I)),int32(U32(I+4)),int32(U32(I+8))));
    TArray<FString> Fields;for(const auto& F:D.Fields)Fields.Add(F.Id);
    TSharedPtr<const FStudioPlanarSurface,ESPMode::ThreadSafe> Surface;
    int64 NodeChecks=0;int32 InterpolatedChecks=0;double MaxNodeError=0,MaxInterpolationError=0;
    for(int32 Frame=0;Frame<3;++Frame)
    {
        const auto Read=Open.Recording->ReadFrame(Frame,Fields);
        if(!TestTrue(*Read.Error,Read.Frame.IsValid()))return false;
        if(!Surface)
        {
            const auto Built=FStudioPlanarSurface::Create(Read.Frame->Geometry.ToSharedRef(),MoveTemp(Faces));
            if(!TestTrue(*Built.Error,Built.Surface.IsValid()))return false;
            Surface=Built.Surface;
        }
        for(const auto& Id:Fields)
        {
            const auto* Values=Read.Frame->FindValues(Id);if(!TestNotNull(TEXT("Original scalar available"),Values))return false;
            for(int32 Row=0;Row<Read.Frame->Geometry->Positions.Num();++Row)
            {
                const auto& P=Read.Frame->Geometry->Positions[Row];double Actual;
                if(!Surface->Sample(FVector2D(P.X,P.Y),*Values,Actual))
                {AddError(FString::Printf(TEXT("Missing original node %d, field %s, frame %d"),Row,*Id,Frame));return false;}
                MaxNodeError=FMath::Max(MaxNodeError,FMath::Abs(Actual-(*Values)[Row]));++NodeChecks;
            }
        }
        for(const auto& Item:Expected->GetArrayField(TEXT("queries")))
        {
            const auto Q=Item->AsObject();const auto& Position=Q->GetArrayField(TEXT("position"));
            const FVector2D P(Position[0]->AsNumber(),Position[1]->AsNumber());
            for(const auto& Entry:Q->GetArrayField(TEXT("expected")))
            {
                const auto V=Entry->AsObject();if(int32(V->GetNumberField(TEXT("frame")))!=Frame)continue;
                const auto* Values=Read.Frame->FindValues(V->GetStringField(TEXT("field")));double Actual;
                if(!Values||!Surface->Sample(P,*Values,Actual)){AddError(TEXT("Independent NACA interpolation query was unavailable."));return false;}
                MaxInterpolationError=FMath::Max(MaxInterpolationError,FMath::Abs(Actual-V->GetNumberField(TEXT("value"))));++InterpolatedChecks;
            }
        }
    }
    TestEqual(TEXT("Every original node and scalar checked across three original frames"),NodeChecks,int64(18706*5*3));
    TestEqual(TEXT("Every original node value is retained exactly"),MaxNodeError,0.);
    TestEqual(TEXT("Independent original-HDF5 interpolation checks"),InterpolatedChecks,720);
    TestTrue(TEXT("Interpolated fields match independent expected values"),MaxInterpolationError<1.e-8);
    TestTrue(TEXT("Native spatial index uses less than 8 MiB"),Surface->IndexBytes()<8LL*1024*1024);
    FStudioSurfaceLocation L;
    TestFalse(TEXT("Airfoil interior supplies no reconstructed fluid values"),Surface->Locate(FVector2D(.04,0),L));
    TestFalse(TEXT("Outside original data has no extrapolation"),Surface->Locate(FVector2D(1,1),L));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceBindingTest,"Studio.Surface.SourceBindingAndIntegrity",SurfaceFlags)
bool FStudioSurfaceBindingTest::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture");
    const auto Open=StudioPointRecordings::Open(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"));
    if(!TestTrue(*Open.Error,Open.Recording.IsValid()))return false;
    const auto Frame=Open.Recording->ReadFrame(0,{Open.Recording->Descriptor().DefaultScalar});
    if(!TestTrue(*Frame.Error,Frame.Frame.IsValid()))return false;
    const auto Geometry=Frame.Frame->Geometry.ToSharedRef();const auto D=Open.Recording->Descriptor();
    const auto Loaded=StudioSurfaceReconstructions::Load(Root/TEXT("reconstruction.json"),D,Geometry);
    if(!TestTrue(*Loaded.Error,Loaded.Reconstruction.IsValid()))return false;
    const auto& R=*Loaded.Reconstruction;
    TestEqual(TEXT("Full reconstructed surface"),R.Surface->TriangleCount(),37188);
    TestEqual(TEXT("Inferred solid retains original 56-point loop"),R.BoundaryRows.Num(),56);
    TestTrue(TEXT("Method and limitations remain visible metadata"),!R.Method.IsEmpty()&&!R.BoundaryOrigin.IsEmpty()&&!R.Limitations.IsEmpty());
    const auto Reopened=StudioSurfaceReconstructions::Load(Root/TEXT("reconstruction.json"),D,Geometry,{},R.MetadataSHA256);
    TestTrue(TEXT("Exact pinned reconstruction reopens"),Reopened.Reconstruction.IsValid());
    TestFalse(TEXT("Changed interpretation cannot replace saved topology"),StudioSurfaceReconstructions::Load(Root/TEXT("reconstruction.json"),D,Geometry,{},FString::ChrN(64,'0')).Reconstruction.IsValid());
    for(int32 I=0;I<6;++I)
    {
        auto Other=D;
        switch(I)
        {
        case 0:Other.Id=TEXT("different_run");break;
        case 1:Other.MetadataSHA256=FString::ChrN(64,'0');break;
        case 2:Other.Coordinates.SHA256=FString::ChrN(64,'0');break;
        case 3:Other.PointIds.SHA256=FString::ChrN(64,'0');break;
        case 4:Other.Frames.Pop();break;
        case 5:Other.SpatialDimensions=3;break;
        }
        TestFalse(TEXT("Wrong run/descriptor/coordinates/point order/times/dimensions rejected"),
            StudioSurfaceReconstructions::Load(Root/TEXT("reconstruction.json"),Other,Geometry).Reconstruction.IsValid());
    }
    FStudioSurfaceLocation L;
    TestFalse(TEXT("Loaded reconstruction excludes airfoil interior"),R.Surface->Locate(FVector2D(.04,0),L));
    const auto* Values=Frame.Frame->FindValues(D.DefaultScalar);double Value;
    TestTrue(TEXT("Original scalar samples after validated load"),Values&&R.Surface->Sample(FVector2D(.12,.03),*Values,Value)&&FMath::IsFinite(Value));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceCorruptionTest,"Studio.Surface.MalformedTopologyAndCancellation",SurfaceFlags)
bool FStudioSurfaceCorruptionTest::RunTest(const FString&)
{
    struct FFiles
    {FString Path=FPaths::ProjectSavedDir()/TEXT("Automation/SurfaceLoad")/FGuid::NewGuid().ToString();
        ~FFiles(){IFileManager::Get().DeleteDirectory(*Path,false,true);}} Files;
    IFileManager::Get().MakeDirectory(*Files.Path,true);
    const FString Original=FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture");
    for(const TCHAR* Name:{TEXT("reconstruction.json"),TEXT("triangles.u32"),TEXT("solid-boundary.u32")})
        if(!TestTrue(TEXT("Copy isolated topology fixture"),IFileManager::Get().Copy(*(Files.Path/Name),*(Original/Name))==COPY_OK))return false;
    const auto Open=StudioPointRecordings::Open(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"));
    if(!TestTrue(*Open.Error,Open.Recording.IsValid()))return false;
    const auto Frame=Open.Recording->ReadFrame(0,{Open.Recording->Descriptor().DefaultScalar});
    if(!TestTrue(*Frame.Error,Frame.Frame.IsValid()))return false;
    const auto Geometry=Frame.Frame->Geometry.ToSharedRef();const auto& D=Open.Recording->Descriptor();
    auto Load=[&](const FStudioLoadCancellation& C=FStudioLoadCancellation())
    {return StudioSurfaceReconstructions::Load(Files.Path/TEXT("reconstruction.json"),D,Geometry,C);};
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Cancelled load publishes no partial surface"),Load(Cancel).Reconstruction.IsValid());
    FString Text;FFileHelper::LoadFileToString(Text,*(Files.Path/TEXT("reconstruction.json")));TSharedPtr<FJsonObject> Base;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Base))return false;
    auto Write=[&](const TSharedPtr<FJsonObject>& O)
    {FString JSON;FJsonSerializer::Serialize(O.ToSharedRef(),TJsonWriterFactory<>::Create(&JSON));return FFileHelper::SaveStringToFile(JSON,*(Files.Path/TEXT("reconstruction.json")));};
    TArray<uint8> Bytes;if(!FFileHelper::LoadFileToArray(Bytes,*(Files.Path/TEXT("triangles.u32"))))return false;
    const auto GoodBytes=Bytes;
    Bytes[0]^=1;FFileHelper::SaveArrayToFile(Bytes,*(Files.Path/TEXT("triangles.u32")));
    TestFalse(TEXT("Corrupt topology bytes rejected by content hash"),Load().Reconstruction.IsValid());
    const auto NotCancelled=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    auto Rehash=[&]()
    {
        FString Hash,Error;
        if(!StudioAssets::HashFile(Files.Path/TEXT("triangles.u32"),NotCancelled,Hash,Error))return false;
        Base->GetObjectField(TEXT("triangles"))->SetStringField(TEXT("sha256"),Hash);return Write(Base);
    };
    // A correct payload hash is not sufficient: indices must address this source.
    Bytes=GoodBytes;for(int32 I=0;I<4;++I)Bytes[I]=255;
    FFileHelper::SaveArrayToFile(Bytes,*(Files.Path/TEXT("triangles.u32")));if(!Rehash())return false;
    TestFalse(TEXT("Rehashed out-of-range source index rejected"),Load().Reconstruction.IsValid());
    TArray<uint8> Hole;FFileHelper::LoadFileToArray(Hole,*(Files.Path/TEXT("solid-boundary.u32")));
    if(!TestEqual(TEXT("Boundary fixture size"),Hole.Num(),56*4))return false;
    Bytes=GoodBytes;const int32 Pick[]={0,18,37};
    for(int32 V=0;V<3;++V)for(int32 B=0;B<4;++B)Bytes[V*4+B]=Hole[Pick[V]*4+B];
    FFileHelper::SaveArrayToFile(Bytes,*(Files.Path/TEXT("triangles.u32")));if(!Rehash())return false;
    const auto Filled=Load();
    TestFalse(TEXT("Rehashed fluid triangle filling the airfoil rejected"),Filled.Reconstruction.IsValid());
    TestTrue(TEXT("Solid failure identifies its cause"),Filled.Error.Contains(TEXT("solid")));
    FFileHelper::SaveArrayToFile(GoodBytes,*(Files.Path/TEXT("triangles.u32")));if(!Rehash())return false;
    auto Array=Base->GetObjectField(TEXT("triangles"));Array->SetStringField(TEXT("path"),TEXT("../triangles.u32"));Write(Base);
    TestFalse(TEXT("Index member cannot escape selected reconstruction folder"),Load().Reconstruction.IsValid());
    Array->SetStringField(TEXT("path"),TEXT("triangles.u32"));Write(Base);
    TestTrue(TEXT("A later valid load recovers after failures"),Load().Reconstruction.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceProjectTest,"Studio.Surface.ProjectBindingMigrationAndRelocation",SurfaceFlags)
bool FStudioSurfaceProjectTest::RunTest(const FString&)
{
    struct FFiles
    {FString Path=FPaths::ProjectSavedDir()/TEXT("Automation/SurfaceProject")/FGuid::NewGuid().ToString();
        ~FFiles(){IFileManager::Get().DeleteDirectory(*Path,false,true);}} Files;
    IFileManager::Get().MakeDirectory(*Files.Path,true);
    const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture"));
    auto Imported=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Imported.Error,Imported.Source.IsValid()&&Imported.Reference.IsSet()))return false;
    FString Hash,Error;const auto Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    if(!TestTrue(TEXT("Pin supplemental interpretation"),StudioAssets::HashFile(Root/TEXT("reconstruction.json"),Cancellation,Hash,Error)))return false;
    auto Reference=*Imported.Reference;Reference.Reconstruction=FStudioReconstructionReference{Root/TEXT("reconstruction.json"),Hash};
    FStudioProject P;P.Dataset=Reference.Id;P.Recordings={Reference};P.SelectedFrame=1;
    P.Camera.Position=FVector(4,5,6);P.AssetBaseDirectory=Files.Path;
    FStudioProject Reopened;
    if(!TestTrue(TEXT("Current project preserves explicit reconstruction"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Reopened,Error)))return false;
    TestTrue(TEXT("Reconstruction reference is durable"),Reopened.Recordings[0].Reconstruction.IsSet());
    TestEqual(TEXT("Exact interpretation hash persists"),Reopened.Recordings[0].Reconstruction->MetadataSHA256,Hash);
    TestEqual(TEXT("Camera remains independent"),Reopened.Camera.Position,P.Camera.Position);
    const FString Destination=Files.Path/TEXT("nested/project.lbms");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination),true);
    TestTrue(TEXT("Save As rebases original and reconstruction files"),StudioProjectIO::Save(Destination,P,Error));
    if(!TestTrue(TEXT("Load resolves both file families"),StudioProjectIO::Load(Destination,Reopened,Error)))return false;
    TestTrue(TEXT("Source path retains its meaning"),FPaths::IsSamePath(Reopened.Recordings[0].Path,Reference.Path));
    TestTrue(TEXT("Supplemental path retains its meaning"),FPaths::IsSamePath(Reopened.Recordings[0].Reconstruction->Path,Reference.Reconstruction->Path));
    const auto Loaded=StudioRecordings::Open(Reopened.Dataset,Reopened.Recordings,Reopened.SelectedFrame,{});
    if(!TestTrue(*Loaded.Error,Loaded.Source.IsValid()))return false;
    TestTrue(TEXT("Recording opens with its explicit validated surface"),Loaded.Source->Reconstruction().IsValid());
    TestTrue(TEXT("Open retains the saved reconstruction reference"),Loaded.Reference.IsSet()&&Loaded.Reference->Reconstruction.IsSet());
    const auto Field=Loaded.Source->CaptureField(1);const auto Points=Field->OriginalPoints();
    if(!TestTrue(TEXT("Original scientific snapshot stays available"),Points.IsValid()))return false;
    double Value;const auto* Values=Points->FindValues(TEXT("velocity_magnitude"));
    TestTrue(TEXT("Bound surface can sample the selected original frame"),Values&&Loaded.Source->Reconstruction()->Surface->Sample(FVector2D(.12,.03),*Values,Value));

    TSharedPtr<FJsonObject> JSON;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),JSON);
    auto Record=JSON->GetArrayField(TEXT("recordings"))[0]->AsObject();Record->RemoveField(TEXT("reconstruction"));
    auto AsText=[&](){FString Result;FJsonSerializer::Serialize(JSON.ToSharedRef(),TJsonWriterFactory<>::Create(&Result));return Result;};
    const FString Before=StudioProjectIO::Serialize(Reopened);
    TestFalse(TEXT("V9 requires explicit optional reconstruction state"),StudioProjectIO::Parse(AsText(),Reopened,Error));
    TestEqual(TEXT("Rejected migration leaves destination unchanged"),StudioProjectIO::Serialize(Reopened),Before);
    JSON->SetNumberField(TEXT("version"),8);
    if(TestTrue(TEXT("V8 migrates with original-point behavior"),StudioProjectIO::Parse(AsText(),Reopened,Error)))
        TestFalse(TEXT("Migration never invents a reconstruction"),Reopened.Recordings[0].Reconstruction.IsSet());
    auto Bad=P;Bad.Recordings[0].Reconstruction->MetadataSHA256=TEXT("unverified");
    TestFalse(TEXT("Invalid saved topology hash rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Bad),Reopened,Error));
    Bad=P;Bad.Recordings[0].Reconstruction->Path=Files.Path/TEXT("missing/reconstruction.json");
    const auto Failed=StudioRecordings::Open(Bad.Dataset,Bad.Recordings,1,{});
    TestFalse(TEXT("Missing topology cannot silently fall back to another interpretation"),Failed.Source.IsValid());
    TestTrue(TEXT("Failure leaves earlier immutable source usable"),Loaded.Source->CaptureField(1)->IsValid());
    auto Recovery=P;Recovery.RecoverySource=Files.Path/TEXT("original.lbms");FStudioProject Stored;
    TestTrue(TEXT("Recovery resolves reconstruction path"),StudioAssetPaths::ForStorage(Recovery,Files.Path/TEXT("recovery.lbms"),Stored,Error));
    TestFalse(TEXT("Recovery topology reference remains absolute"),FPaths::IsRelative(Stored.Recordings[0].Reconstruction->Path));
    return true;
}
#endif
