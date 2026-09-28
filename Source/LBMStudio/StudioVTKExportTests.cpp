#include "StudioVTKExport.h"
#include "StudioModel.h"
#include "StudioPointRecording.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/BufferArchive.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioVTKTests
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FString Fixture(const TCHAR* Name){return FPaths::ProjectContentDir()/TEXT("Samples")/Name;}
FString Output(){return FPaths::ProjectSavedDir()/TEXT("Automation/VTKExport");}
FStudioVTKExportResult Save(const FStudioVTKExportRequest& R,const TCHAR* Name)
{
    IFileManager::Get().MakeDirectory(*Output(),true);
    TUniquePtr<FArchive> A(IFileManager::Get().CreateFileWriter(*(Output()/Name)));
    if(!A){FStudioVTKExportResult Error;Error.Error=TEXT("Test staging file could not open.");return Error;}
    auto Result=StudioVTKExport::Write(R,*A);
    if(!A->Close()||A->IsError()){Result.bSuccess=false;Result.Error=TEXT("Test staging file could not close.");}
    return Result;
}
class FFailingVTKArchive final:public FArchive
{
public:
    FFailingVTKArchive(){SetIsSaving(true);}
    void Serialize(void*,int64 Count) override {Largest=FMath::Max(Largest,Count);Written+=Count;if(Written>100000)SetError();}
    int64 Written=0,Largest=0;
};
/** Fault injection wraps a real original snapshot; no manufactured CFD is accepted. */
class FFaultField final:public IStudioField
{
public:
    FFaultField(TSharedPtr<const IStudioField,ESPMode::ThreadSafe> In,int32 InFault,bool bAfterRead=false)
        :Original(MoveTemp(In)),Fault(InFault),Loaded(bAfterRead){}
    bool IsValid() const override{return Original->IsValid();}
    TOptional<FStudioFieldIdentity> Identity() const override
    {auto I=Original->Identity();if(Loaded&&Fault==1)++I->Ordinal;return I;}
    TOptional<FStudioScalarDescriptor> Scalar(const FString& Id) const override
    {auto S=Original->Scalar(Id);if(Loaded&&Fault==2&&S)S->Unit=TEXT("changed unit");return S;}
    FString ScalarExpression(const FString& Id) const override{return Original->ScalarExpression(Id);}
    int32 OriginalPointCount() const override{return Original->OriginalPointCount();}
    bool OriginalPoint(int32 N,int64& Id,FVector& P) const override
    {const bool Good=Original->OriginalPoint(N,Id,P);if(Loaded&&Fault==3&&N==7)++Id;return Good;}
    bool OriginalScalar(int32 N,const FString& Id,double& V) const override
    {if(!Loaded||Fault==4&&N==7)return false;return Original->OriginalScalar(N,Id,V);}
    int32 OriginalTriangleCount() const override{return Original->OriginalTriangleCount();}
    bool OriginalTriangle(int32 N,FIntVector& T) const override
    {const bool Good=Original->OriginalTriangle(N,T);if(Fault==5&&N==7)T.X=OriginalPointCount();return Good;}
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> LoadScalarSnapshot(const FString& Id,const FStudioLoadCancellation& C,FString& Error) const override
    {return MakeShared<FFaultField,ESPMode::ThreadSafe>(Original,Fault,true);}
    bool Sample(const FVector&,FStudioFieldValue&) const override{return false;}
    bool IsSolid(const FVector& P) const override{return Original->IsSolid(P);}
    const TArray<FVector2D>& Boundary() const override{return Original->Boundary();}
    const TArray<FIntVector>& BoundaryTriangles() const override{return Original->BoundaryTriangles();}
private:
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Original;int32 Fault;bool Loaded;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVTKOriginalRows,"Studio.VTKExport.ExactOriginalRowsAndConnectivity",StudioVTKTests::Flags)
bool FVTKOriginalRows::RunTest(const FString&)
{
    using namespace StudioVTKTests;
    FRecordedSolver Source;const auto Read=Source.ReadScalarFrame(420,TEXT("pressure"));
    if(!TestTrue(*Read.Error,Read.Field.IsValid()))return false;
    TUniquePtr<FArchive> Raw(IFileManager::Get().CreateFileReader(*(Fixture(TEXT("MeshGraphNets_Airfoil"))/TEXT("flow.bin"))));
    if(!TestTrue(TEXT("Published mesh payload opens"),Raw.IsValid()))return false;
    int32 Magic,Version,Points,Triangles,Boundary,Frames;*Raw<<Magic<<Version<<Points<<Triangles<<Boundary<<Frames;
    TestEqual(TEXT("Original point count"),Read.Field->OriginalPointCount(),Points);
    TestEqual(TEXT("Original triangle count"),Read.Field->OriginalTriangleCount(),Triangles);
    bool Coordinates=true,Connectivity=true,Values=true;
    for(int32 I=0;I<Points;++I)
    {
        FVector2D P;*Raw<<P.X<<P.Y;int64 Id;FVector Original;
        Coordinates&=Read.Field->OriginalPoint(I,Id,Original)&&Id==I&&Original==FVector(P.X,P.Y,0);
    }
    for(int32 I=0;I<Triangles;++I)
    {FIntVector T,Original;*Raw<<T.X<<T.Y<<T.Z;Connectivity&=Read.Field->OriginalTriangle(I,Original)&&T==Original;}
    Raw->Seek(24LL+16LL*Points+12LL*Triangles+4LL*Boundary+420LL*(12LL+16LL*Points)+12);
    const TCHAR* Ids[]={TEXT("velocity_x"),TEXT("velocity_y"),TEXT("pressure"),TEXT("density")};
    for(int32 I=0;I<Points;++I)
    {
        float V[4];Raw->Serialize(V,sizeof(V));
        for(int32 K=0;K<4;++K){double Actual;Values&=Read.Field->OriginalScalar(I,Ids[K],Actual)&&Actual==double(V[K]);}
        double Speed;Values&=Read.Field->OriginalScalar(I,TEXT("velocity_magnitude"),Speed)&&Speed==FVector2D(V[0],V[1]).Size();
    }
    TestTrue(TEXT("Every coordinate retains exact original double before display translation"),Coordinates);
    TestTrue(TEXT("Every triangle retains original row order and connectivity"),Connectivity);
    TestTrue(TEXT("All source tuples and explicitly derived magnitudes are exact"),Values&&!Raw->IsError());
    int64 Id;FVector P;double V;
    TestFalse(TEXT("No out-of-range point"),Read.Field->OriginalPoint(Points,Id,P));
    TestFalse(TEXT("No invented optional field"),Read.Field->OriginalScalar(0,TEXT("temperature"),V));
    const auto Loads=Source.CacheStats().Loads;FString Error;
    const auto Pinned=Read.Field->LoadScalarSnapshot(TEXT("density"),{},Error);
    TestTrue(TEXT("Legacy additional fields reuse the pinned original frame"),Pinned&&Pinned->Identity()->Ordinal==420&&Source.CacheStats().Loads==Loads);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVTKReferenceFiles,"Studio.VTKExport.PublishedReferenceFiles",StudioVTKTests::Flags)
bool FVTKReferenceFiles::RunTest(const FString&)
{
    using namespace StudioVTKTests;
    FRecordedSolver Source;const auto Read=Source.ReadScalarFrame(420,TEXT("pressure"));if(!Read.Field)return false;
    FStudioVTKExportRequest R{Read.Field,{TEXT("pressure"),TEXT("velocity_x"),TEXT("velocity_y"),TEXT("density"),TEXT("velocity_magnitude")}};
    auto A=Save(R,TEXT("airfoil-source.vtp"));TestTrue(*A.Error,A.bSuccess);
    R.Coordinates=EStudioExportCoordinates::Scene;auto B=Save(R,TEXT("airfoil-scene.vtp"));TestTrue(*B.Error,B.bSuccess);
    for(const auto Name:{TEXT("NACA0018_ReaderFixture"),TEXT("Cylinder3D_ReaderFixture")})
    {
        const auto Loaded=StudioRecordings::Import(Fixture(Name)/TEXT("recording.json"),2,{});
        if(!TestTrue(*Loaded.Error,Loaded.Source.IsValid()))return false;
        const auto Field=Loaded.Source->ReadScalarFrame(2,TEXT("pressure"));if(!TestTrue(*Field.Error,Field.Field.IsValid()))return false;
        R={Field.Field,{}};for(const auto& S:Loaded.Source->Descriptor().Scalars)R.Scalars.Add(S.Id);
        const bool Volume=Loaded.Source->Descriptor().SpatialDimensions==3;
        const auto Result=Save(R,Volume?TEXT("cylinder-points.vtp"):TEXT("naca-points.vtp"));TestTrue(*Result.Error,Result.bSuccess);
        TestEqual(TEXT("No connectivity invented for source points"),Result.Triangles,0);
        if(Volume)
        {
            const auto Reconstructed=StudioRecordings::ImportReconstruction(*Loaded.Reference,Fixture(TEXT("Cylinder3D_VolumeFixture"))/TEXT("reconstruction.json"),2,{});
            if(!TestTrue(*Reconstructed.Error,Reconstructed.Source.IsValid()))return false;
            const auto View=Reconstructed.Source->ReadScalarFrame(2,TEXT("pressure"));R={View.Field,{TEXT("pressure")}};
            const auto Original=Save(R,TEXT("cylinder-from-view.vtp"));TestTrue(*Original.Error,Original.bSuccess);
            TestTrue(TEXT("Reconstructed view exports original rows and records display reconstruction separately"),Original.Triangles==0&&Original.Identity.Interpolation==EStudioFieldInterpolation::ReconstructedGrid);
        }
    }
    AddInfo(TEXT("Independent VTK audit input: ")+FPaths::ConvertRelativePathToFull(Output()));return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVTKFailureSafety,"Studio.VTKExport.CancellationFaultsAndBoundedWrites",StudioVTKTests::Flags)
bool FVTKFailureSafety::RunTest(const FString&)
{
    using namespace StudioVTKTests;
    FRecordedSolver Source;auto Read=Source.ReadScalarFrame(420,TEXT("pressure"));if(!Read.Field)return false;
    FStudioVTKExportRequest R{Read.Field,{TEXT("pressure")}};
    FBufferArchive PreCancelled;auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestTrue(TEXT("Pre-cancel emits no staging bytes"),StudioVTKExport::Write(R,PreCancelled,Cancel).bCancelled&&PreCancelled.IsEmpty());
    Cancel->store(false);FBufferArchive MidCancelled;int64 Last=0,LastTotal=0;bool Monotonic=true;
    auto Result=StudioVTKExport::Write(R,MidCancelled,Cancel,[&](int64 Done,int64 Total)
    {Monotonic&=Done>Last&&Done<=Total&&(!LastTotal||Total==LastTotal);Last=Done;LastTotal=Total;if(Done>=1024)Cancel->store(true);});
    TestTrue(TEXT("Progress is monotonic and cancellation stops before success"),Monotonic&&Result.bCancelled&&!Result.bSuccess&&Last<LastTotal);
    FFailingVTKArchive Failure;Result=StudioVTKExport::Write(R,Failure);
    TestTrue(TEXT("Disk error explicit and encoded chunks bounded"),!Result.bSuccess&&!Result.Error.IsEmpty()&&Failure.Written>100000&&Failure.Largest<40000);
    for(const TArray<FString>& Selection:{TArray<FString>{},TArray<FString>{TEXT("density"),TEXT("density")},TArray<FString>{TEXT("temperature")}})
    {FBufferArchive Invalid;R.Scalars=Selection;Result=StudioVTKExport::Write(R,Invalid);TestTrue(TEXT("Invalid fields rejected before output"),!Result.bSuccess&&Invalid.IsEmpty());}
    R.Scalars={TEXT("pressure")};
    for(int32 Fault=1;Fault<=5;++Fault)
    {FBufferArchive Invalid;R.Field=MakeShared<FFaultField,ESPMode::ThreadSafe>(Read.Field,Fault);Result=StudioVTKExport::Write(R,Invalid);
        TestTrue(TEXT("Identity, unit, point order, missing value and connectivity faults fail explicitly"),!Result.bSuccess&&!Result.Error.IsEmpty());}
    TestTrue(TEXT("Independent export failures leave source load status clear"),Source.LoadError().IsEmpty());
    return true;
}
#endif
