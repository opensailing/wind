#include "StudioPointRecording.h"
#include "StudioVolume.h"
#include "StudioModel.h"
#include "StudioAssets.h"
#include "StudioColor.h"
#include "StudioStreamlines.h"
#include "StudioSnapshot.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4FieldsTestPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Values)
{TArray<TSharedPtr<FJsonValue>> A;for(double V:Values)A.Add(MakeShared<FJsonValueNumber>(V));return A;}
void Bits(TArray<uint8>& Bytes,uint64 V)
{for(int32 I=0;I<8;++I)Bytes.Add(uint8(V>>(8*I)));}
void Double(TArray<uint8>& Bytes,double V)
{uint64 B;FMemory::Memcpy(&B,&V,8);Bits(Bytes,B);}

// Artificial affine-grid mathematics only. This fixture is never an installed solver result.
struct Fixture
{
    FString Folder=FPaths::ProjectSavedDir()/TEXT("Automation/Home4Fields")/FGuid::NewGuid().ToString();
    TSharedRef<FJsonObject> Meta=MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Grid=MakeShared<FJsonObject>();
    bool bReady=true;
    Fixture()
    {
        bReady=IFileManager::Get().MakeDirectory(*Folder,true);
        const FString Provenance=Save(TEXT("provenance.json"),TEXT("{\"fixture\":\"artificial structural mathematics, not CFD\"}"));
        const FString Attribution=Save(TEXT("ATTRIBUTION.txt"),TEXT("Artificial structural fixture for native grid tests.\n"));
        const FString Manifest=Save(TEXT("source-manifest.json"),TEXT("{\"version\":1,\"kind\":\"home4_source_snapshot_manifest\",\"snapshots\":[]}"));
        Meta->SetNumberField(TEXT("version"),3);Meta->SetStringField(TEXT("kind"),TEXT("field_recording"));
        Meta->SetStringField(TEXT("id"),TEXT("HOME4_ArtificialStructuralFixture"));Meta->SetStringField(TEXT("title"),TEXT("Artificial grid mathematics"));
        Meta->SetStringField(TEXT("sourceURL"),TEXT("urn:artificial-structural-unit-fixture"));
        Meta->SetStringField(TEXT("coordinateUnit"),TEXT("m"));Meta->SetStringField(TEXT("timeUnit"),TEXT("s"));
        Meta->SetStringField(TEXT("timeOrigin"),TEXT("Artificial step * original 0.02 s/step"));
        Meta->SetNumberField(TEXT("spatialDimensions"),3);Meta->SetNumberField(TEXT("pointCount"),343);Meta->SetNumberField(TEXT("frameCount"),2);
        Meta->SetStringField(TEXT("defaultScalar"),TEXT("phi"));Meta->SetStringField(TEXT("provenanceSHA256"),Provenance);Meta->SetStringField(TEXT("attributionSHA256"),Attribution);
        auto Topology=MakeShared<FJsonObject>();Topology->SetStringField(TEXT("kind"),TEXT("points"));Topology->SetStringField(TEXT("origin"),TEXT("source"));
        Topology->SetField(TEXT("connectivity"),MakeShared<FJsonValueNull>());Meta->SetObjectField(TEXT("topology"),Topology);
        auto Bounds=MakeShared<FJsonObject>();Bounds->SetArrayField(TEXT("min"),Numbers({1,2,3}));Bounds->SetArrayField(TEXT("max"),Numbers({1+.2*6,2+.3*6,3+.4*6}));Meta->SetObjectField(TEXT("sourceBounds"),Bounds);
        TArray<uint8> Coordinates,Ids;
        for(int32 Z=0;Z<7;++Z)for(int32 Y=0;Y<7;++Y)for(int32 X=0;X<7;++X)
        {Double(Coordinates,1+.2*X);Double(Coordinates,2+.3*Y);Double(Coordinates,3+.4*Z);Bits(Ids,(1+2*X)+17*((2+2*Y)+17*(3+2*Z)));}
        auto C=Array(TEXT("coordinates.f64"),Coordinates,{343,3});Meta->SetObjectField(TEXT("coordinates"),C);
        auto I=MakeShared<FJsonObject>();I->SetStringField(TEXT("path"),TEXT("point-ids.i64"));I->SetStringField(TEXT("dtype"),TEXT("int64"));
        I->SetStringField(TEXT("byteOrder"),TEXT("little"));I->SetNumberField(TEXT("count"),343);I->SetStringField(TEXT("sha256"),SaveBytes(TEXT("point-ids.i64"),Ids));Meta->SetObjectField(TEXT("pointIds"),I);
        TArray<TSharedPtr<FJsonValue>> Frames;
        for(int32 Ordinal=0;Ordinal<2;++Ordinal)
        {auto F=MakeShared<FJsonObject>();F->SetNumberField(TEXT("index"),10+10*Ordinal);F->SetStringField(TEXT("label"),FString::Printf(TEXT("step_%d"),10+10*Ordinal));F->SetNumberField(TEXT("time"),.2+.2*Ordinal);Frames.Add(MakeShared<FJsonValueObject>(F));}
        Meta->SetArrayField(TEXT("frames"),Frames);
        TArray<TSharedPtr<FJsonValue>> Fields;
        for(const FString Id:{TEXT("phi"),TEXT("solid"),TEXT("derivative_valid"),TEXT("q"),TEXT("ux"),TEXT("uy"),TEXT("uz"),TEXT("solid_support"),TEXT("liquid_support")})
        {
            TArray<uint8> Data;double Min=MAX_dbl,Max=-MAX_dbl;
            for(int32 Ordinal=0;Ordinal<2;++Ordinal)for(int32 Z=0;Z<7;++Z)for(int32 Y=0;Y<7;++Y)for(int32 X=0;X<7;++X)
            {
                double V=0;
                if(Id==TEXT("phi"))V=Ordinal==0?(X==6&&Y==6&&Z==6?100.:double(X)/6.):(X==3&&Y==3&&Z==3?0.:1.);
                else if(Id==TEXT("solid"))V=Ordinal==1&&X==3&&Y==3&&Z==3?1.:0.;
                else if(Id==TEXT("derivative_valid"))V=Ordinal==0&&X==4&&Y==2&&Z==2?0.:1.;
                else if(Id==TEXT("solid_support"))V=Ordinal==1&&X==3&&Y==3&&Z==3?0.:1.;
                else if(Id==TEXT("liquid_support"))V=Ordinal==0?(X>=3?1.:0.):(X==3&&Y==3&&Z==3?0.:1.);
                else if(Id==TEXT("q"))V=9.; // Independent Q for rigid rotation u=(-3y,3x,0): Omega:Omega=18, S:S=0.
                else if(Id==TEXT("ux"))V=-3*(2+.3*Y);
                else if(Id==TEXT("uy"))V=3*(1+.2*X);
                Min=FMath::Min(Min,V);Max=FMath::Max(Max,V);Double(Data,V);
            }
            auto F=MakeShared<FJsonObject>();F->SetStringField(TEXT("id"),Id);F->SetStringField(TEXT("label"),Id);
            F->SetStringField(TEXT("unit"),Id==TEXT("q")?TEXT("1/s2"):Id.StartsWith(TEXT("u"))?TEXT("m/s"):TEXT("1"));
            const bool Derived=Id==TEXT("q")||Id==TEXT("derivative_valid")||Id.EndsWith(TEXT("_support"));
            F->SetStringField(TEXT("association"),TEXT("point"));F->SetStringField(TEXT("origin"),Derived?TEXT("derived"):TEXT("source"));
            if(Derived)F->SetStringField(TEXT("expression"),TEXT("Artificial exact mathematical fixture"));
            F->SetBoolField(TEXT("static"),false);F->SetArrayField(TEXT("range"),Numbers({Min,Max}));
            auto A=Array(Id+TEXT(".f64"),Data,{2,343});A->SetArrayField(TEXT("frameCRC32"),Numbers({double(FCrc::MemCrc32(Data.GetData(),343*8)),double(FCrc::MemCrc32(Data.GetData()+343*8,343*8))}));F->SetObjectField(TEXT("array"),A);
            if(Id==TEXT("q")){F->SetBoolField(TEXT("airMaskDefault"),true);F->SetStringField(TEXT("validityMask"),TEXT("derivative_valid"));}
            if(Id.StartsWith(TEXT("u"))){F->SetStringField(TEXT("vector"),TEXT("velocity"));F->SetStringField(TEXT("component"),Id.Right(1));}
            if(Id==TEXT("phi"))
            {auto R=MakeShared<FJsonObject>();R->SetNumberField(TEXT("minimum"),0);R->SetNumberField(TEXT("maximum"),1);R->SetStringField(TEXT("policy"),TEXT("first_frame_percentile_99"));R->SetNumberField(TEXT("clippedAbove"),1);F->SetObjectField(TEXT("displayRange"),R);}
            Fields.Add(MakeShared<FJsonValueObject>(F));
        }
        Meta->SetArrayField(TEXT("fields"),Fields);Meta->SetArrayField(TEXT("limitations"),{MakeShared<FJsonValueString>(TEXT("Artificial structural tests only; no CFD validation claim."))});
        Grid->SetNumberField(TEXT("version"),1);Grid->SetStringField(TEXT("kind"),TEXT("home4_structured_source"));Grid->SetStringField(TEXT("layout"),TEXT("x_fastest_node_grid"));
        Grid->SetArrayField(TEXT("dimensionsXYZ"),Numbers({7,7,7}));Grid->SetArrayField(TEXT("originalDimensionsXYZ"),Numbers({17,17,17}));
        Grid->SetArrayField(TEXT("cropMinimumXYZ"),Numbers({1,2,3}));Grid->SetArrayField(TEXT("cropMaximumXYZ"),Numbers({15,16,17}));Grid->SetNumberField(TEXT("previewStride"),2);
        Grid->SetArrayField(TEXT("originMeters"),Numbers({1,2,3}));Grid->SetArrayField(TEXT("spacingMeters"),Numbers({.2,.3,.4}));
        Grid->SetArrayField(TEXT("originalOriginXYZ"),Numbers({9,17,24}));Grid->SetArrayField(TEXT("originalSpacingXYZ"),Numbers({1,1.5,2}));
        Grid->SetStringField(TEXT("axisOrder"),TEXT("zyx"));Grid->SetStringField(TEXT("metadataOrder"),TEXT("xyz"));Grid->SetStringField(TEXT("coordinateUnits"),TEXT("lattice"));Grid->SetStringField(TEXT("velocityUnits"),TEXT("lattice"));
        Grid->SetStringField(TEXT("sourceRunId"),TEXT("artificial_run_1"));Grid->SetNumberField(TEXT("timeOriginSeconds"),0);
        Grid->SetStringField(TEXT("phaseField"),TEXT("phi"));Grid->SetStringField(TEXT("solidField"),TEXT("solid"));Grid->SetStringField(TEXT("derivativeValidityField"),TEXT("derivative_valid"));Grid->SetNumberField(TEXT("phiLiquidMin"),.5);
        Grid->SetStringField(TEXT("solidSupportField"),TEXT("solid_support"));Grid->SetStringField(TEXT("liquidSupportField"),TEXT("liquid_support"));
        auto Units=MakeShared<FJsonObject>();Units->SetNumberField(TEXT("dxMeters"),.1);Units->SetNumberField(TEXT("dtSeconds"),.02);Units->SetNumberField(TEXT("densityReferenceKgM3"),1000);Grid->SetObjectField(TEXT("units"),Units);
        auto Ref=MakeShared<FJsonObject>();Ref->SetNumberField(TEXT("lengthCells"),25);Ref->SetNumberField(TEXT("speedCellsPerStep"),.03);Ref->SetNumberField(TEXT("densityLattice"),1);Grid->SetObjectField(TEXT("reference"),Ref);
        auto M=MakeShared<FJsonObject>();M->SetStringField(TEXT("path"),TEXT("source-manifest.json"));M->SetStringField(TEXT("sha256"),Manifest);M->SetNumberField(TEXT("frameCount"),2);Grid->SetObjectField(TEXT("sourceManifest"),M);
        Meta->SetObjectField(TEXT("structuredGrid"),Grid);Publish();
    }
    ~Fixture(){IFileManager::Get().DeleteDirectory(*Folder,false,true);}
    FString Path() const{return Folder/TEXT("recording.json");}
    FString SaveBytes(const FString& Name,const TArray<uint8>& Data)
    {
        FString Hash,Error;bReady&=FFileHelper::SaveArrayToFile(Data,*(Folder/Name));
        bReady&=StudioAssets::HashFile(Folder/Name,MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false),Hash,Error);return Hash;
    }
    FString Save(const FString& Name,const FString& Text)
    {FTCHARToUTF8 Utf(*Text);TArray<uint8> Bytes;Bytes.Append(reinterpret_cast<const uint8*>(Utf.Get()),Utf.Length());return SaveBytes(Name,Bytes);}
    TSharedRef<FJsonObject> Array(const FString& Name,const TArray<uint8>& Data,std::initializer_list<double> Shape)
    {auto A=MakeShared<FJsonObject>();A->SetStringField(TEXT("path"),Name);A->SetStringField(TEXT("dtype"),TEXT("float64"));A->SetStringField(TEXT("byteOrder"),TEXT("little"));A->SetStringField(TEXT("sha256"),SaveBytes(Name,Data));A->SetNumberField(TEXT("byteLength"),Data.Num());A->SetArrayField(TEXT("shape"),Numbers(Shape));return A;}
    void Publish(){FString Text;bReady&=FJsonSerializer::Serialize(Meta,TJsonWriterFactory<>::Create(&Text));Save(TEXT("recording.json"),Text);}
    FStudioPointOpenResult Open(const FStudioPointReadOptions& Options={}) const
    {return Async(EAsyncExecution::ThreadPool,[this,Options]{return StudioPointRecordings::Open(Path(),Options);}).Get();}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4OriginalGrid,"Studio.Home4.Fields.OriginalGridIdentityAndUnits",StudioHome4FieldsTestPrivate::Flags)
bool FStudioHome4OriginalGrid::RunTest(const FString&)
{
    using namespace StudioHome4FieldsTestPrivate;Fixture F;if(!TestTrue(TEXT("Artificial fixture written"),F.bReady))return false;
    const auto R=F.Open();if(!TestTrue(*R.Error,R.Recording.IsValid()))return false;
    const auto& D=R.Recording->Descriptor();const auto S=D.StructuredGrid;
    if(!TestTrue(TEXT("Immutable source mapping present"),S.IsValid()))return false;
    TestEqual(TEXT("Original dimensions retained"),S->OriginalDimensions,FIntVector(17));
    FIntVector I;TestTrue(TEXT("Original row index available"),S->OriginalIndex(0,I));TestEqual(TEXT("Crop starts at original indices"),I,FIntVector(1,2,3));
    TestTrue(TEXT("Last row mapped"),S->OriginalIndex(342,I));TestEqual(TEXT("Stride maps last row"),I,FIntVector(13,14,15));
    const auto Context=S->UnitContext();TestEqual(TEXT("Original dx"),Context.Units.DxMeters.GetValue(),.1);TestEqual(TEXT("Original L"),Context.Reference.LengthCells.GetValue(),25.);
    TestEqual(TEXT("Original run identity"),S->SourceRunId,FString(TEXT("artificial_run_1")));
    const auto V=StudioVolumes::OriginalSource(D,R.Recording->Geometry());if(!TestTrue(*V.Error,V.Volume.IsValid()))return false;
    TestEqual(TEXT("Source mode labeled"),V.Volume->Title,FString(TEXT("Original structured grid")));
    TestEqual(TEXT("No cylinder invented"),V.Volume->CylinderRadius,0.);TestEqual(TEXT("Identity row"),V.Volume->Stencils[127].Rows[0],127);
    TestEqual(TEXT("Identity weight"),V.Volume->Stencils[127].Weights[0],1.);
    TestFalse(TEXT("No unavailable pressure"),D.FindField(TEXT("pressure"))!=nullptr);
    F.Grid->SetArrayField(TEXT("originalDimensionsXYZ"),Numbers({16000000,16000000,16000000}));F.Publish();
    TestFalse(TEXT("Overflowing original dimensions rejected before product"),F.Open().Recording.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4OriginalMasks,"Studio.Home4.Fields.FrameMasksAndExactSamples",StudioHome4FieldsTestPrivate::Flags)
bool FStudioHome4OriginalMasks::RunTest(const FString&)
{
    using namespace StudioHome4FieldsTestPrivate;Fixture F;const auto R=F.Open();if(!TestTrue(*R.Error,R.Recording.IsValid()))return false;
    const auto V=StudioVolumes::OriginalSource(R.Recording->Descriptor(),R.Recording->Geometry());if(!TestTrue(*V.Error,V.Volume.IsValid()))return false;
    const auto A=R.Recording->ReadFrame(0,{TEXT("q"),TEXT("ux"),TEXT("uy"),TEXT("uz")});const auto B=R.Recording->ReadFrame(1,{TEXT("q")});
    if(!TestTrue(*A.Error,A.Frame.IsValid())||!TestTrue(*B.Error,B.Frame.IsValid()))return false;
    TestNotNull(TEXT("Required phase loaded"),A.Frame->FindValues(TEXT("phi")));TestNotNull(TEXT("Required derivative mask loaded"),A.Frame->FindValues(TEXT("derivative_valid")));
    FString Error;const auto Q=StudioVolumes::SourceMask(*A.Frame,*V.Volume,TEXT("q"),false,Error);
    if(!TestTrue(*Error,!Q.IsEmpty()))return false;
    double Value;TestTrue(TEXT("Exact rigid-rotation Q sample"),StudioVolumes::SampleSource(*A.Frame,*V.Volume,Q,TEXT("q"),FVector(1+.2*4.2,2+.3*3.2,3+.4*3.2),Value));TestTrue(TEXT("Independent analytic Q"),FMath::Abs(Value-9.)<1.e-12);
    TestEqual(TEXT("Air halo excluded"),Q[3+7*(3+7*3)],uint8(0));TestEqual(TEXT("Derivative invalid node excluded"),Q[4+7*(2+7*2)],uint8(0));
    const auto Phi=StudioVolumes::SourceMask(*A.Frame,*V.Volume,TEXT("phi"),false,Error);
    TestEqual(TEXT("Phi retains air"),Phi[0],uint8(1));TestEqual(TEXT("Phi keeps interface"),Phi[3+7*(3+7*3)],uint8(1));
    const auto Second=StudioVolumes::SourceMask(*B.Frame,*V.Volume,TEXT("phi"),false,Error);
    TestEqual(TEXT("Second frame solid classification"),Second[3+7*(3+7*3)],uint8(2));
    TestEqual(TEXT("First mask remains immutable"),Phi[3+7*(3+7*3)],uint8(1));
    TestEqual(TEXT("Static topology unchanged across frames"),V.Volume->Classification[3+7*(3+7*3)],uint8(1));
    TestEqual(TEXT("Original solid retained"),(*B.Frame->FindValues(TEXT("solid")))[3+7*(3+7*3)],1.);
    const auto Obstacle=StudioVolumes::SourceMask(*B.Frame,*V.Volume,TEXT("solid"),false,Error);
    TestEqual(TEXT("Solid contour includes original obstacle nodes"),Obstacle[3+7*(3+7*3)],uint8(1));
    FStudioColorMapping SolidMapping;const auto SolidGrid=StudioVolumes::Build(*B.Frame,*V.Volume,TEXT("solid"),SolidMapping);
    TestTrue(TEXT("Source obstacle contour available"),!StudioVolumes::Isosurface(SolidGrid,.5).Indices.IsEmpty());
    const auto NoAir=StudioVolumes::SourceMask(*A.Frame,*V.Volume,TEXT("q"),false,Error,{},false);
    TestEqual(TEXT("Air display override permits original scientific values"),NoAir[2+7*(3+7*3)],uint8(1));
    TestEqual(TEXT("Override preserves derivative validity"),NoAir[4+7*(2+7*2)],uint8(0));
    TestFalse(TEXT("Region cannot bridge source solid"),StudioVolumes::SupportsSourceRegion(*V.Volume,Second,FBox(FVector(1.2,2.3,3.4),FVector(2,3.5,5))));
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestTrue(TEXT("Cancelled mask publishes nothing"),StudioVolumes::SourceMask(*A.Frame,*V.Volume,TEXT("q"),false,Error,Cancel).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4OriginalIso,"Studio.Home4.Fields.PhiIsosurfaceAndDisplayPolicy",StudioHome4FieldsTestPrivate::Flags)
bool FStudioHome4OriginalIso::RunTest(const FString&)
{
    using namespace StudioHome4FieldsTestPrivate;Fixture F;const auto R=F.Open();if(!TestTrue(*R.Error,R.Recording.IsValid()))return false;
    const auto V=StudioVolumes::OriginalSource(R.Recording->Descriptor(),R.Recording->Geometry());if(!TestTrue(*V.Error,V.Volume.IsValid()))return false;
    const auto A=R.Recording->ReadFrame(0,{TEXT("phi")});if(!TestTrue(*A.Error,A.Frame.IsValid()))return false;
    FStudioColorMapping Mapping;Mapping.Minimum=0;Mapping.Maximum=1;
    const auto Grid=StudioVolumes::Build(*A.Frame,*V.Volume,TEXT("phi"),Mapping);if(!TestTrue(*Grid.Error,Grid.Error.IsEmpty()))return false;
    const auto Iso=StudioVolumes::Isosurface(Grid,.5);if(!TestTrue(*Iso.Error,Iso.Error.IsEmpty()))return false;
    TestTrue(TEXT("Exact phi plane has triangles"),!Iso.Indices.IsEmpty());
    for(const auto& P:Iso.PositionsMeters)if(!TestTrue(TEXT("phi=0.5 lies on original x=1.6m plane"),FMath::Abs(P.X-1.6)<1.e-6))return false;
    auto Solver=MakeShared<FPointRecordedSolver,ESPMode::ThreadSafe>(R.Recording.ToSharedRef(),nullptr,V.Volume);
    const auto Scalar=Solver->Descriptor().Scalars.FindByPredicate([](const auto& S){return S.Id==TEXT("phi");});
    if(!TestNotNull(TEXT("Original scalar metadata"),Scalar))return false;
    TestEqual(TEXT("Full source maximum retained"),Scalar->Maximum,100.);
    const auto Default=StudioColor::Resolve(Solver->Descriptor().Id,*Scalar,{});TestEqual(TEXT("First-frame default held"),Default.Maximum,1.);
    FStudioScalarStyle Style;Style.Dataset=Solver->Descriptor().Id;Style.Field=TEXT("phi");Style.bManualRange=true;Style.Minimum=.2;Style.Maximum=.8;
    const auto Manual=StudioColor::Resolve(Solver->Descriptor().Id,*Scalar,{Style});TestEqual(TEXT("Explicit range wins"),Manual.Maximum,.8);
    TestEqual(TEXT("Clipped count provenance retained"),R.Recording->Descriptor().FindField(TEXT("phi"))->FirstFrameClippedAbove,int64(1));
    const auto Field=Solver->CaptureViewField(0,TEXT("phi"),false);TSharedPtr<FJsonObject> ImageSource;
    const auto JSON=StudioSnapshot::SourceMetadata(*Field,TEXT("phi"),Default,true);
    TestTrue(TEXT("Image source metadata parses"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON),ImageSource));
    if(ImageSource)
    {
        TestEqual(TEXT("Actual clipped source node count"),ImageSource->GetNumberField(TEXT("clipped_above")),1.);
        TestEqual(TEXT("Original crop retained in image"),ImageSource->GetArrayField(TEXT("crop_minimum_indices"))[0]->AsNumber(),1.);
        TestEqual(TEXT("Preview explicitly identified"),ImageSource->GetNumberField(TEXT("preview_stride")),2.);
        TestEqual(TEXT("Image source step conversion is not case unit map"),ImageSource->GetObjectField(TEXT("units"))->GetNumberField(TEXT("dtSeconds")),.02);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4OriginalIntegrity,"Studio.Home4.Fields.RejectChangedGridAndMasks",StudioHome4FieldsTestPrivate::Flags)
bool FStudioHome4OriginalIntegrity::RunTest(const FString&)
{
    using namespace StudioHome4FieldsTestPrivate;
    const TArray<TFunction<void(Fixture&)>> Mutations={
        [](Fixture& F){F.Grid->SetArrayField(TEXT("spacingMeters"),Numbers({.21,.3,.4}));F.Publish();},
        [](Fixture& F){F.Grid->SetStringField(TEXT("axisOrder"),TEXT("unknown"));F.Publish();},
        [](Fixture& F){F.Grid->SetStringField(TEXT("solidField"),TEXT("absent"));F.Publish();},
        [](Fixture& F){F.Grid->GetObjectField(TEXT("units"))->SetNumberField(TEXT("dxMeters"),0);F.Publish();},
        [](Fixture& F){F.Save(TEXT("source-manifest.json"),TEXT("changed"));},
        [](Fixture& F){auto U=F.Grid->GetObjectField(TEXT("sourceManifest"));U->SetStringField(TEXT("path"),TEXT("../outside.json"));F.Publish();}
    };
    for(const auto& Mutation:Mutations){Fixture F;Mutation(F);const auto R=F.Open();TestFalse(TEXT("Invalid source interpretation never publishes"),R.Recording.IsValid());TestFalse(TEXT("Rejection provides reason"),R.Error.IsEmpty());}
    Fixture F;FStudioPointReadOptions Small;Small.LiveArrayBytes=343*8;Small.CacheBytes=0;
    const auto R=F.Open(Small);if(!TestTrue(*R.Error,R.Recording.IsValid()))return false;
    TestFalse(TEXT("Dependencies honor explicit live-byte cap"),R.Recording->ReadFrame(0,{TEXT("q")}).Frame.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4LiquidStreams,"Studio.Streamlines.RK45.SourceMasksAndLiquidSeeds",StudioHome4FieldsTestPrivate::Flags)
bool FStudioHome4LiquidStreams::RunTest(const FString&)
{
    using namespace StudioHome4FieldsTestPrivate;Fixture F;const auto R=F.Open();if(!TestTrue(*R.Error,R.Recording.IsValid()))return false;
    const auto Grid=StudioVolumes::OriginalSource(R.Recording->Descriptor(),R.Recording->Geometry());if(!TestTrue(*Grid.Error,Grid.Volume.IsValid()))return false;
    auto Solver=MakeShared<FPointRecordedSolver,ESPMode::ThreadSafe>(R.Recording.ToSharedRef(),nullptr,Grid.Volume);
    const auto First=Solver->CaptureViewField(0,TEXT("phi"),true);const auto Second=Solver->CaptureViewField(1,TEXT("phi"),true);
    if(!TestTrue(TEXT("Source frames retain valid velocity"),First->IsValid()&&Second->IsValid()))return false;
    const FBox Bounds=StudioStreamlines::DomainBounds(*First,Solver->Descriptor().DisplayBounds);
    TArray<FVector> Seeds,Again;FString Error;
    if(!TestTrue(*Error,StudioStreamlines::AutomaticSeeds(*First,Bounds,12,EStudioStreamDirection::Forward,Seeds,Error)))return false;
    TestEqual(TEXT("Native grid seeds interior supported liquid cells"),Seeds.Num(),12);
    TestTrue(TEXT("Interior seeding ignores tracing direction deterministically"),StudioStreamlines::AutomaticSeeds(*First,Bounds,12,EStudioStreamDirection::Both,Again,Error)&&Seeds==Again);
    for(const FVector& P:Seeds)
    {FVector V;double Phi;TestTrue(TEXT("Every original seed has recorded liquid velocity and whole-cell support"),First->SampleVelocity(P,V)&&First->SampleScalar(P,TEXT("phi"),Phi)&&Phi>=.5&&First->SupportsSegment(P,P));}
    FStudioSeedObject Seed;Seed.Name=TEXT("Liquid seeds");Seed.Kind=EStudioSeedKind::Points;Seed.Points=Seeds;const auto Identity=*First->Identity();Seed.Source={Identity.Dataset,Identity.MetadataSHA256,Identity.PayloadSHA256};
    FStudioStreamlineSettings Settings;Settings.MaximumSteps=3;Settings.WorkBudget=128;
    FStudioStreamlineOutput Output;if(!TestTrue(*Error,StudioStreamlines::Build(*First,Bounds,{Seed},Settings,TEXT("phi"),Output,Error)))return false;
    TestTrue(TEXT("RK45 follows actual original-grid velocities"),Output.Segments>0&&Output.Method==EStudioStreamMethod::DormandPrince45);
    TestEqual(TEXT("Exact original frame bound"),Output.Identity->Ordinal,0);
    for(const auto& Path:Output.Paths)for(int32 I=1;I<Path.PositionsMeters.Num();++I)
        TestTrue(TEXT("Native trajectories cannot bridge source air or solids"),First->SupportsSegment(Path.PositionsMeters[I-1],Path.PositionsMeters[I]));
    const auto Kept=Seeds;const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Cancelled liquid seeding is atomic"),StudioStreamlines::AutomaticSeeds(*Second,Bounds,12,EStudioStreamDirection::Forward,Seeds,Error,Cancel));
    TestTrue(TEXT("Prior exact seeds preserved"),Seeds==Kept);
    const FVector SolidPoint(1+.2*3,3+.4*3,2+.3*3);
    TestTrue(TEXT("Second-frame source solid available"),Second->IsSolid(SolidPoint));
    TestFalse(TEXT("First-frame snapshot does not acquire later solid"),First->IsSolid(SolidPoint));
    return true;
}
#endif
