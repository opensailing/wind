#include "StudioModel.h"
#include "StudioPointRecording.h"
#include "StudioSurfaceReconstruction.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto FieldFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FString PointFixture(){return FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json");}
FString SurfaceFixture(){return FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture/reconstruction.json");}
FStudioRecordingLoadResult OpenSurface()
{
    const auto Source=StudioRecordings::Import(PointFixture(),0,{});
    return Source.Reference.IsSet()?StudioRecordings::ImportReconstruction(*Source.Reference,SurfaceFixture(),0,{}):Source;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFieldOriginalSurface,"Studio.Fields.OriginalSurfaceSamples",FieldFlags)
bool FStudioFieldOriginalSurface::RunTest(const FString&)
{
    const auto Loaded=OpenSurface();
    if(!TestTrue(*Loaded.Error,Loaded.Source.IsValid()))return false;
    FString Text;TSharedPtr<FJsonObject> Expected;
    if(!TestTrue(TEXT("Read independent original-HDF5 expectations"),
        FFileHelper::LoadFileToString(Text,*(FPaths::GetPath(SurfaceFixture())/TEXT("expected.json")))&&
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Expected)&&Expected.IsValid()))return false;
    int64 Nodes=0;int32 Queries=0,Vectors=0;
    double MaxNodeError=0,MaxQueryError=0,MaxVectorError=0;
    for(int32 Frame=0;Frame<3;++Frame)
    {
        for(const auto& Scalar:Loaded.Source->Descriptor().Scalars)
        {
            const auto Snapshot=Loaded.Source->CaptureViewField(Frame,Scalar.Id,true);
            if(!TestTrue(TEXT("Snapshot retains verified reconstruction"),Snapshot->IsValid()&&
                Snapshot->Reconstruction()==Loaded.Source->Reconstruction()))return false;
            const auto Points=Snapshot->OriginalPoints();
            const auto* Values=Points->FindValues(Scalar.Id);
            if(!TestNotNull(TEXT("Selected original array is loaded"),Values))return false;
            for(int32 Row=0;Row<Points->Geometry->Positions.Num();++Row)
            {
                const auto& P=Points->Geometry->Positions[Row];double Actual;
                if(!Snapshot->SampleScalar(FVector(P.X,0,P.Y),Scalar.Id,Actual))
                {AddError(FString::Printf(TEXT("Original node unavailable: %d, %s, frame %d"),Row,*Scalar.Id,Frame));return false;}
                MaxNodeError=FMath::Max(MaxNodeError,FMath::Abs(Actual-(*Values)[Row]));++Nodes;
            }
            for(const auto& Item:Expected->GetArrayField(TEXT("queries")))
            {
                const auto Q=Item->AsObject();const auto& XY=Q->GetArrayField(TEXT("position"));
                const FVector P(XY[0]->AsNumber(),0,XY[1]->AsNumber());
                double U=0,V=0;bool HasU=false,HasV=false;
                for(const auto& Entry:Q->GetArrayField(TEXT("expected")))
                {
                    const auto Value=Entry->AsObject();if(int32(Value->GetNumberField(TEXT("frame")))!=Frame)continue;
                    const FString Id=Value->GetStringField(TEXT("field"));
                    const double Reference=Value->GetNumberField(TEXT("value"));
                    if(Id==TEXT("velocity_u")){U=Reference;HasU=true;}
                    if(Id==TEXT("velocity_v")){V=Reference;HasV=true;}
                    if(Id!=Scalar.Id)continue;
                    double Actual;
                    if(!Snapshot->SampleScalar(P,Id,Actual)){AddError(TEXT("Independent field query unavailable"));return false;}
                    MaxQueryError=FMath::Max(MaxQueryError,FMath::Abs(Actual-Reference));++Queries;
                }
                // One vector query per independent location/frame, even when
                // pressure is the displayed scalar. Component roles own meaning.
                if(Scalar.Id==TEXT("pressure")&&HasU&&HasV)
                {
                    FVector Velocity;
                    if(!Snapshot->SampleVelocity(P,Velocity)){AddError(TEXT("Independent vector query unavailable"));return false;}
                    MaxVectorError=FMath::Max(MaxVectorError,(Velocity-FVector(U,0,V)).Size());++Vectors;
                }
            }
        }
    }
    TestEqual(TEXT("Every original node/field/frame checked through the public query API"),Nodes,int64(18706*5*3));
    TestEqual(TEXT("Source-node values remain exact"),MaxNodeError,0.);
    TestEqual(TEXT("All independent interpolation values checked"),Queries,720);
    TestEqual(TEXT("Independent vector queries checked"),Vectors,144);
    TestTrue(TEXT("Scalar queries match original-HDF5 interpolation"),MaxQueryError<1.e-8);
    TestTrue(TEXT("Source XY vectors map correctly to scene XZ"),MaxVectorError<1.e-8);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFieldMissingSurface,"Studio.Fields.MissingDataAndImmutableSnapshots",FieldFlags)
bool FStudioFieldMissingSurface::RunTest(const FString&)
{
    auto Loaded=OpenSurface();
    if(!TestTrue(*Loaded.Error,Loaded.Source.IsValid()))return false;
    const auto Pressure=Loaded.Source->CaptureViewField(0,TEXT("pressure"),false);
    const FVector P(.12,0,.03);double Value,First;
    TestTrue(TEXT("Selected pressure available without velocity"),Pressure->SampleScalar(P,TEXT("pressure"),First));
    for(const auto& Id:{TEXT("density"),TEXT("unknown"),TEXT("velocity_magnitude"),TEXT("velocity_u")})
    {
        Value=123;
        TestFalse(TEXT("Absent or unrequested scalar has no value"),Pressure->SampleScalar(P,Id,Value));
        TestFalse(TEXT("Unavailable scalar cannot be mistaken for zero or the preceding value"),FMath::IsFinite(Value));
    }
    FVector Velocity(1,2,3);
    TestFalse(TEXT("Unrequested velocity components are unavailable"),Pressure->SampleVelocity(P,Velocity));
    TestTrue(TEXT("Unavailable vector is explicitly nonfinite"),Velocity.ContainsNaN());
    const auto Full=Loaded.Source->CaptureViewField(0,TEXT("velocity_magnitude"),true);
    TestTrue(TEXT("Explicitly requested components are available"),Full->SampleVelocity(P,Velocity));
    TestTrue(TEXT("Supplied exported speed is available separately"),Full->SampleScalar(P,TEXT("velocity_magnitude"),Value));
    TestFalse(TEXT("Exported source speed is never replaced with component magnitude"),FMath::IsNearlyEqual(Value,Velocity.Size(),1.e-5));
    for(const auto& Outside:{FVector(.04,0,0),FVector(1,0,1),FVector(P.X,1.e-12,P.Z),
        FVector(P.X,-.5,P.Z),FVector(std::numeric_limits<double>::quiet_NaN(),0,P.Z)})
    {
        Value=123;Velocity=FVector(1,2,3);
        TestFalse(TEXT("No scalar inside the hole, outside coverage, off plane or nonfinite"),Full->SampleScalar(Outside,TEXT("velocity_magnitude"),Value));
        TestFalse(TEXT("Missing spatial scalar is nonfinite"),FMath::IsFinite(Value));
        TestFalse(TEXT("No vector inside the hole, outside coverage, off plane or nonfinite"),Full->SampleVelocity(Outside,Velocity));
        TestTrue(TEXT("Missing spatial vector is nonfinite"),Velocity.ContainsNaN());
    }
    const auto Later=Loaded.Source->CaptureViewField(2,TEXT("pressure"),false);
    TestTrue(TEXT("Later pressure is independently available"),Later->SampleScalar(P,TEXT("pressure"),Value));
    TestFalse(TEXT("Original frames contain different pressure"),FMath::IsNearlyEqual(First,Value,1.e-8));
    Loaded.Source.Reset(); // Simulate project/source removal while a query retains a snapshot.
    TestTrue(TEXT("Retained snapshot remains readable after source removal"),Pressure->SampleScalar(P,TEXT("pressure"),Value));
    TestEqual(TEXT("Later reads/removal cannot mutate pinned values"),Value,First);
    const auto Raw=StudioRecordings::Import(PointFixture(),0,{});
    if(!TestTrue(*Raw.Error,Raw.Source.IsValid()))return false;
    const auto Points=Raw.Source->CaptureViewField(0,TEXT("pressure"),true);
    TestFalse(TEXT("Original-point mode advertises no reconstruction"),Points->Reconstruction().IsValid());
    TestFalse(TEXT("Removing topology removes scalar interpolation"),Points->SampleScalar(P,TEXT("pressure"),Value));
    TestFalse(TEXT("Removing topology removes velocity interpolation"),Points->SampleVelocity(P,Velocity));
    FStudioFieldValue Complete;
    TestFalse(TEXT("Optional fields never impersonate a complete legacy tuple"),Full->Sample(P,Complete));
    const auto Invalid=Raw.Source->CaptureViewField(-1,TEXT("pressure"),true);
    TestFalse(TEXT("Invalid frame cannot provide a scalar"),Invalid->SampleScalar(P,TEXT("pressure"),Value));
    TestFalse(TEXT("Invalid frame cannot provide velocity"),Invalid->SampleVelocity(P,Velocity));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFieldLegacy,"Studio.Fields.LegacySourceQueries",FieldFlags)
bool FStudioFieldLegacy::RunTest(const FString&)
{
    FRecordedSolver Solver;const auto Field=Solver.CaptureField(0);
    const FVector P(.03250676393508911,0,.1194048523902893);
    double Value;FVector Velocity;
    TestTrue(TEXT("Original legacy velocity available"),Field->SampleVelocity(P,Velocity));
    TestTrue(TEXT("Original legacy pressure available"),Field->SampleScalar(P,TEXT("pressure"),Value));
    TestTrue(TEXT("Independently decoded original pressure retained"),FMath::IsNearlyEqual(Value,98699.78125,1.e-5));
    TestTrue(TEXT("Original legacy density available"),Field->SampleScalar(P,TEXT("density"),Value));
    TestTrue(TEXT("Independently decoded original density retained"),FMath::IsNearlyEqual(Value,1.2020570039749146,1.e-8));
    TestTrue(TEXT("Legacy derived speed available"),Field->SampleScalar(P,TEXT("velocity_magnitude"),Value));
    TestEqual(TEXT("Legacy speed uses supplied component norm"),Value,Velocity.Size());
    TestTrue(TEXT("Source X component available"),Field->SampleScalar(P,TEXT("velocity_x"),Value));
    TestEqual(TEXT("Source X component follows scene X"),Value,Velocity.X);
    TestTrue(TEXT("Source Y component available"),Field->SampleScalar(P,TEXT("velocity_y"),Value));
    TestEqual(TEXT("Source Y component follows scene Z"),Value,Velocity.Z);
    TestFalse(TEXT("No third source component is invented"),Field->SampleScalar(P,TEXT("velocity_z"),Value));
    TestFalse(TEXT("Unknown component is nonfinite"),FMath::IsFinite(Value));
    for(const auto& Outside:{FVector(0,0,0),FVector(3,0,0)})
    {
        TestFalse(TEXT("Missing legacy scalar remains unavailable"),Field->SampleScalar(Outside,TEXT("pressure"),Value));
        TestFalse(TEXT("Missing legacy scalar has no finite fallback"),FMath::IsFinite(Value));
        TestFalse(TEXT("Missing legacy vector remains unavailable"),Field->SampleVelocity(Outside,Velocity));
        TestTrue(TEXT("Missing legacy vector has no zero fallback"),Velocity.ContainsNaN());
    }
    return true;
}
#endif
