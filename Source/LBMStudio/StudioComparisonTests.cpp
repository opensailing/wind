#include "StudioComparison.h"
#include "StudioModel.h"
#include "StudioPointRecording.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <cmath>
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto ComparisonTestFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString ComparisonFixture(const TCHAR* Name)
{return FPaths::ProjectContentDir()/TEXT("Samples")/Name;}
TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> ComparisonPointSource(const TCHAR* Name=TEXT("NACA0018_ReaderFixture"))
{return StudioRecordings::Import(ComparisonFixture(Name)/TEXT("recording.json"),0,{}).Source;}
FStudioComparisonRequest ComparisonRequest(TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> A,
    TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> B)
{
    FStudioComparisonRequest R;R.ProjectId=FGuid::NewGuid();R.Primary=A;R.Secondary=B;
    R.PrimaryOrdinal=0;R.Scalar=TEXT("pressure");R.Alignment.Mode=EStudioTimeAlignment::RecordedTime;return R;
}
TOptional<FStudioComparisonResult> FinishComparison(FStudioComparisonTask& Task)
{
    const double Deadline=FPlatformTime::Seconds()+15;
    do {if(auto Result=Task.Poll())return Result;FPlatformProcess::SleepNoStats(.001f);}while(FPlatformTime::Seconds()<Deadline);
    return {};
}
// Fault injection changes only identity, metadata or IO outcomes. Numerical
// arrays remain the authors' published CFD, never a fabricated flow fixture.
class FComparisonChangedField final : public IStudioField
{
public:
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Inner;
    FStudioFieldIdentity Changed;
    bool IsValid() const override{return Inner->IsValid();}
    TOptional<FStudioFieldIdentity> Identity() const override{return Changed;}
    TOptional<FStudioScalarDescriptor> Scalar(const FString& Id) const override{return Inner->Scalar(Id);}
    bool Sample(const FVector& P,FStudioFieldValue& V) const override{return Inner->Sample(P,V);}
    bool IsSolid(const FVector& P) const override{return Inner->IsSolid(P);}
    const TArray<FVector2D>& Boundary() const override{return Inner->Boundary();}
    const TArray<FIntVector>& BoundaryTriangles() const override{return Inner->BoundaryTriangles();}
};
class FComparisonReadSource final : public IStudioSolver
{
public:
    explicit FComparisonReadSource(TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Source):Inner(Source),Meta(Source->Descriptor()){}
    TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Inner;
    FStudioRecordingDescriptor Meta;
    bool bFail=false;
    TFunction<void(FStudioFieldIdentity&)> Change;
    TFunction<void()> OnRead;
    mutable std::atomic<int32> Reads{0};
    int32 FrameCount() const override{return Meta.Frames.Num();}
    FStudioFrame EvaluateFrame(int32 I) const override{return Meta.Frames[I];}
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32 I) const override{return Inner->CaptureField(I);}
    FStudioFieldReadResult ReadScalarFrame(int32 I,const FString& S,const FStudioLoadCancellation& C={}) const override
    {
        ++Reads;if(OnRead)OnRead();if(bFail)return {{},TEXT("Injected comparison read failure")};
        auto Read=Inner->ReadScalarFrame(I,S,C);
        if(Change&&Read.Field&&Read.Field->Identity().IsSet())
        {auto Field=MakeShared<FComparisonChangedField,ESPMode::ThreadSafe>();Field->Inner=Read.Field;Field->Changed=*Read.Field->Identity();Change(Field->Changed);Read.Field=Field;}
        return Read;
    }
    bool ExportField(int32,const FString&) const override{return false;}
    FString LoadError() const override{return Inner->LoadError();}
    const FStudioRecordingDescriptor& Descriptor() const override{return Meta;}
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Reconstruction() const override{return Inner->Reconstruction();}
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> VolumeReconstruction() const override{return Inner->VolumeReconstruction();}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComparisonPublishedTimes,"Studio.Comparison.IndependentPublishedTimelines",ComparisonTestFlags)
bool FComparisonPublishedTimes::RunTest(const FString&)
{
    const auto A=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();
    const auto B=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>(ComparisonFixture(TEXT("MeshGraphNets_Airfoil_test010"))/TEXT("flow.bin"));
    if(!TestTrue(TEXT("Two original published trajectories load"),A->LoadError().IsEmpty()&&B->LoadError().IsEmpty()&&A->Descriptor().Id!=B->Descriptor().Id))return false;
    auto R=ComparisonRequest(A,B);
    for(int32 I=0;I<601;++I)
    {
        const auto Pair=StudioComparison::Align(A->Descriptor(),B->Descriptor(),I,R.Alignment);
        if(!TestTrue(TEXT("All original timestamps match without interpolation"),Pair.Status==EStudioComparisonStatus::Ready&&Pair.PrimaryOrdinal==I&&Pair.SecondaryOrdinal==I&&Pair.MismatchSeconds==0))return false;
        TestEqual(TEXT("Matching retains original physical time"),Pair.SecondaryFrame.Time,B->Descriptor().Frames[I].Time);
    }
    R.PrimaryOrdinal=420;const auto Pair=StudioComparison::Evaluate(R);
    TestTrue(TEXT("Both independent original fields published"),Pair.Matches(R)&&Pair.Primary.Field&&Pair.Secondary.Field&&
        Pair.Primary.Identity.Dataset!=Pair.Secondary.Identity.Dataset&&Pair.Primary.Identity.Ordinal==420&&Pair.Secondary.Identity.Ordinal==420);
    TestEqual(TEXT("Independent field ranges retained"),Pair.Secondary.Scalar.Maximum,B->Descriptor().Scalars.FindByPredicate([](const auto& S){return S.Id==TEXT("pressure");})->Maximum);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComparisonTimePolicy,"Studio.Comparison.ExplicitAlignmentAndBounds",ComparisonTestFlags)
bool FComparisonTimePolicy::RunTest(const FString&)
{
    const auto A=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();const auto B=ComparisonPointSource();
    if(!TestTrue(TEXT("Real nonuniform point timeline loads"),B.IsValid()))return false;
    FStudioComparisonAlignment S;
    TestTrue(TEXT("No implicit alignment mode"),StudioComparison::Align(A->Descriptor(),B->Descriptor(),0,S).Status==EStudioComparisonStatus::InvalidRequest);
    S.Mode=EStudioTimeAlignment::RecordedTime;S.Match=EStudioTimeMatch::Nearest;S.MaximumMismatchSeconds=100;
    TestTrue(TEXT("Tolerance cannot extrapolate outside source overlap"),StudioComparison::Align(A->Descriptor(),B->Descriptor(),0,S).Status==EStudioComparisonStatus::NoMatch);
    S.Mode=EStudioTimeAlignment::ElapsedFromStart;S.Match=EStudioTimeMatch::Exact;S.MaximumMismatchSeconds=0;
    auto Pair=StudioComparison::Align(A->Descriptor(),B->Descriptor(),0,S);
    TestTrue(TEXT("Elapsed alignment preserves nonzero original time and step"),Pair.Status==EStudioComparisonStatus::Ready&&Pair.SecondaryFrame.Time==2.5025&&Pair.SecondaryFrame.Index==1001&&Pair.SecondaryAlignedTime==0);
    TestTrue(TEXT("Elapsed matching does not compare relative frame indices"),StudioComparison::Align(A->Descriptor(),B->Descriptor(),420,S).Status==EStudioComparisonStatus::NoMatch);
    S.Mode=EStudioTimeAlignment::ManualOffset;S.SecondaryOffsetSeconds=-2.5025;S.Match=EStudioTimeMatch::Nearest;S.MaximumMismatchSeconds=.1;
    Pair=StudioComparison::Align(A->Descriptor(),B->Descriptor(),420,S);
    TestTrue(TEXT("Explicit offset and tolerance identify actual nearest source snapshot"),Pair.Status==EStudioComparisonStatus::Ready&&Pair.SecondaryOrdinal==0&&Pair.SecondaryFrame.Time==2.5025&&Pair.MismatchSeconds==-.084);
    S.MaximumMismatchSeconds=.01;TestTrue(TEXT("Nearest difference above tolerance rejected"),StudioComparison::Align(A->Descriptor(),B->Descriptor(),420,S).Status==EStudioComparisonStatus::NoMatch);
    // An unchanged subset of original timestamps provides an exact nearest tie.
    auto Subset=A->Descriptor();Subset.Frames={A->Descriptor().Frames[0],A->Descriptor().Frames[2]};
    S={EStudioTimeAlignment::RecordedTime,EStudioTimeMatch::Nearest,0,.0002};
    Pair=StudioComparison::Align(A->Descriptor(),Subset,1,S);
    TestTrue(TEXT("Exact tie chooses earlier original step, with signed mismatch"),Pair.Status==EStudioComparisonStatus::Ready&&Pair.SecondaryFrame.Index==0&&Pair.MismatchSeconds==-.0002);
    S.MaximumMismatchSeconds=std::nextafter(.0002,0.);TestTrue(TEXT("Tolerance boundary has no hidden epsilon"),StudioComparison::Align(A->Descriptor(),Subset,1,S).Status==EStudioComparisonStatus::NoMatch);
    S.MaximumMismatchSeconds=.1;S.Match=EStudioTimeMatch::Exact;
    TestTrue(TEXT("Exact mode refuses hidden nearest tolerance"),StudioComparison::Align(A->Descriptor(),Subset,1,S).Status==EStudioComparisonStatus::InvalidRequest);
    S.MaximumMismatchSeconds=0;S.Mode=EStudioTimeAlignment::ManualOffset;S.SecondaryOffsetSeconds=1.e300;
    TestTrue(TEXT("Offset which collapses source precision rejected"),StudioComparison::Align(A->Descriptor(),Subset,1,S).Status==EStudioComparisonStatus::InvalidTimeline);
    S={EStudioTimeAlignment::RecordedTime,EStudioTimeMatch::Exact,0,0};
    for(const double Invalid:{-1.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
    {auto Bad=Subset;Bad.Frames[1].Time=Invalid;TestTrue(TEXT("Invalid source time rejected"),StudioComparison::Align(A->Descriptor(),Bad,0,S).Status==EStudioComparisonStatus::InvalidTimeline);}
    Subset.Frames[1].Time=Subset.Frames[0].Time;TestTrue(TEXT("Duplicate source times rejected"),StudioComparison::Align(A->Descriptor(),Subset,0,S).Status==EStudioComparisonStatus::InvalidTimeline);
    TestTrue(TEXT("Source step cannot masquerade as ordinal"),StudioComparison::Align(B->Descriptor(),B->Descriptor(),5001,S).Status==EStudioComparisonStatus::InvalidRequest);
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestTrue(TEXT("Metadata scan cancellation is explicit"),StudioComparison::Align(A->Descriptor(),B->Descriptor(),0,S,Cancel).Status==EStudioComparisonStatus::Cancelled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComparisonOriginalArrays,"Studio.Comparison.OriginalArraysAndTopology",ComparisonTestFlags)
bool FComparisonOriginalArrays::RunTest(const FString&)
{
    const auto A=ComparisonPointSource(),B=ComparisonPointSource();
    if(!TestTrue(TEXT("Two independent original readers load"),A.IsValid()&&B.IsValid()))return false;
    FString JSON;TSharedPtr<FJsonObject> Expected;
    if(!TestTrue(TEXT("Independent HDF5 values load"),FFileHelper::LoadFileToString(JSON,*(ComparisonFixture(TEXT("NACA0018_ReaderFixture"))/TEXT("expected.json")))&&
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON),Expected)))return false;
    auto R=ComparisonRequest(A,B);int32 Checked=0;
    for(const auto& S:A->Descriptor().Scalars)if(S.Unit!=TEXT("unspecified"))for(int32 I=0;I<3;++I)
    {
        R.Scalar=S.Id;R.PrimaryOrdinal=I;const auto Pair=StudioComparison::Evaluate(R);
        if(!TestTrue(TEXT("Exact original point snapshots publish"),Pair.Matches(R)&&Pair.Primary.Field->OriginalPoints()&&Pair.Secondary.Field->OriginalPoints()))return false;
        const auto* First=Pair.Primary.Field->OriginalPoints()->FindValues(S.Id);const auto* Second=Pair.Secondary.Field->OriginalPoints()->FindValues(S.Id);
        for(const auto& Entry:Expected->GetArrayField(TEXT("samples")))
        {
            const auto V=Entry->AsObject();if(V->GetStringField(TEXT("field"))!=S.Id||V->GetNumberField(TEXT("frame"))!=I)continue;
            const int32 Row=int32(V->GetNumberField(TEXT("point")));const double Value=V->GetNumberField(TEXT("value"));
            TestEqual(TEXT("First retains independent original array value"),(*First)[Row],Value);
            TestEqual(TEXT("Second retains independent original array value"),(*Second)[Row],Value);++Checked;
        }
    }
    TestEqual(TEXT("All independently extracted values with explicit units compared"),Checked,108);
    auto Raw=StudioRecordings::Import(ComparisonFixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),0,{});
    if(!TestTrue(TEXT("Point reference available"),Raw.Reference.IsSet()))return false;
    const auto Surface=StudioRecordings::ImportReconstruction(*Raw.Reference,ComparisonFixture(TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),0,{});
    if(!TestTrue(*Surface.Error,Surface.Source.IsValid()))return false;
    R=ComparisonRequest(A,Surface.Source);const auto Pair=StudioComparison::Evaluate(R);
    TestTrue(TEXT("Original points and derived surface keep distinct topology identities"),Pair.Matches(R)&&
        Pair.Primary.Identity.Interpolation==EStudioFieldInterpolation::None&&Pair.Secondary.Identity.Interpolation==EStudioFieldInterpolation::ReconstructedTriangles&&
        !Pair.Secondary.Identity.ReconstructionSHA256.IsEmpty()&&Pair.Secondary.Field->Reconstruction()==Surface.Source->Reconstruction());
    const auto Volume=ComparisonPointSource(TEXT("Cylinder3D_ReaderFixture"));
    if(!TestTrue(TEXT("Actual 3D recording loads"),Volume.IsValid()))return false;
    R=ComparisonRequest(Volume,Volume);R.PrimaryOrdinal=2;const auto ThreeD=StudioComparison::Evaluate(R);
    TestTrue(TEXT("3D frame identity survives pairing"),ThreeD.Matches(R)&&ThreeD.Primary.Identity.SpatialDimensions==3&&ThreeD.Secondary.Identity.Ordinal==2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComparisonIdentity,"Studio.Comparison.ScalarUnitsAndIdentityGuards",ComparisonTestFlags)
bool FComparisonIdentity::RunTest(const FString&)
{
    const auto Raw=ComparisonPointSource();if(!TestTrue(TEXT("Published reader loads"),Raw.IsValid()))return false;
    const auto Other=MakeShared<FComparisonReadSource,ESPMode::ThreadSafe>(Raw);auto R=ComparisonRequest(Raw,Other);
    for(const FString Unit:{TEXT("kPa"),TEXT(""),TEXT("unspecified")})
    {
        Other->Meta.Scalars.FindByPredicate([](const auto& S){return S.Id==TEXT("pressure");})->Unit=Unit;
        TestTrue(TEXT("Incompatible or unknown unit fails before any field read"),StudioComparison::Evaluate(R).Frames.Status==EStudioComparisonStatus::InvalidRequest&&Other->Reads.load()==0);
    }
    Other->Meta=Raw->Descriptor();R.Scalar=TEXT("absent");TestTrue(TEXT("No scalar fallback"),StudioComparison::Evaluate(R).Frames.Status==EStudioComparisonStatus::InvalidRequest);R.Scalar=TEXT("pressure");
    const TArray<TFunction<void(FStudioFieldIdentity&)>> Changes={
        [](auto& I){++I.Ordinal;},[](auto& I){++I.Frame.Index;},[](auto& I){I.Frame.Time+=1;},
        [](auto& I){I.Dataset=TEXT("wrong");},[](auto& I){I.MetadataSHA256=TEXT("wrong");},[](auto& I){I.PayloadSHA256=TEXT("wrong");},
        [](auto& I){I.ReconstructionSHA256=TEXT("wrong");},[](auto& I){I.SourceOffset.X+=1;},[](auto& I){I.SpatialDimensions=3;},
        [](auto& I){I.Interpolation=EStudioFieldInterpolation::SourceTriangles;}};
    for(const auto& Change:Changes)
    {
        Other->Change=Change;const auto Pair=StudioComparison::Evaluate(R);
        TestTrue(TEXT("Wrong second snapshot discards both fields"),Pair.Frames.Status==EStudioComparisonStatus::IdentityMismatch&&!Pair.Primary.Field&&!Pair.Secondary.Field&&!Pair.Matches(R));
    }
    Other->Change={};const auto Pair=StudioComparison::Evaluate(R);
    if(!TestTrue(TEXT("Unmodified pair publishes"),Pair.Matches(R)))return false;
    auto Changed=R;Changed.ProjectId=FGuid::NewGuid();TestFalse(TEXT("Foreign project cannot publish"),Pair.Matches(Changed));
    Changed=R;Changed.PrimaryOrdinal=1;TestFalse(TEXT("A later scrub invalidates old pair"),Pair.Matches(Changed));
    Changed=R;Changed.Alignment.Mode=EStudioTimeAlignment::ElapsedFromStart;TestFalse(TEXT("Policy edit invalidates old pair"),Pair.Matches(Changed));
    Changed=R;Changed.Secondary=Raw;TestFalse(TEXT("Reader replacement invalidates old pair"),Pair.Matches(Changed));
    Changed=R;Changed.Scalar=TEXT("velocity_u");TestFalse(TEXT("Field edit invalidates old pair"),Pair.Matches(Changed));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComparisonIsolation,"Studio.Comparison.ReadFailureAndPlaybackIsolation",ComparisonTestFlags)
bool FComparisonIsolation::RunTest(const FString&)
{
    FStudioModel Model(FPaths::ProjectDir()/TEXT("tmp/debug/comparison-tests")/FGuid::NewGuid().ToString());
    Model.ReviewRecordedFrame(37);Model.Project.Camera.Position=FVector(7,8,9);const auto Before=StudioProjectIO::Serialize(Model.SnapshotProject());
    const auto Other=MakeShared<FComparisonReadSource,ESPMode::ThreadSafe>(Model.Solver);auto R=ComparisonRequest(Model.Solver,Other);R.PrimaryOrdinal=420;
    Other->bFail=true;const auto Failed=StudioComparison::Evaluate(R);
    TestTrue(TEXT("IO failure publishes no partial pair"),Failed.Frames.Status==EStudioComparisonStatus::ReadFailed&&!Failed.Primary.Field&&!Failed.Secondary.Field);
    TestTrue(TEXT("Failure stays in analysis channel"),Model.Solver->LoadError().IsEmpty()&&Other->LoadError().IsEmpty());
    TestEqual(TEXT("Comparison never moves camera, cursors or editable case"),StudioProjectIO::Serialize(Model.SnapshotProject()),Before);
    Other->bFail=false;const auto Pair=StudioComparison::Evaluate(R);TestTrue(TEXT("Explicit retry succeeds"),Pair.Matches(R));
    TestEqual(TEXT("Successful comparison also preserves playback and authoring"),StudioProjectIO::Serialize(Model.SnapshotProject()),Before);
    auto C=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);Other->OnRead=[C]{C->store(true);};
    const auto Cancelled=StudioComparison::Evaluate(R,C);
    TestTrue(TEXT("Cancellation during second read discards first original field"),Cancelled.Frames.Status==EStudioComparisonStatus::Cancelled&&!Cancelled.Primary.Field&&!Cancelled.Secondary.Field);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComparisonWorker,"Studio.Comparison.BoundedWorkerAndRelease",ComparisonTestFlags)
bool FComparisonWorker::RunTest(const FString&)
{
    FStudioPointReadOptions Options;Options.CacheBytes=0;Options.LiveArrayBytes=2*18706LL*sizeof(double);
    const auto Open=StudioPointRecordings::Open(ComparisonFixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),Options);
    if(!TestTrue(*Open.Error,Open.Recording.IsValid()))return false;
    auto Source=MakeShared<FPointRecordedSolver,ESPMode::ThreadSafe>(Open.Recording.ToSharedRef());
    auto R=ComparisonRequest(Source,Source);FStudioComparisonTask Task;FString Error;
    auto Unset=R;Unset.Alignment.Mode=EStudioTimeAlignment::Unset;TestFalse(TEXT("Worker refuses implicit alignment"),Task.Start(Unset,Error));
    if(!TestTrue(TEXT("Paired read starts"),Task.Start(R,Error)))return false;
    TestFalse(TEXT("No unbounded queue of scrubs"),Task.Start(R,Error));Task.Cancel();
    auto Result=FinishComparison(Task);TestTrue(TEXT("Cancel drains worker and retains no pair"),Result.IsSet()&&Result->Frames.Status==EStudioComparisonStatus::Cancelled&&!Result->Primary.Field&&!Result->Secondary.Field);
    if(!TestTrue(TEXT("Retry starts after drain"),Task.Start(R,Error)))return false;
    Result=FinishComparison(Task);if(!TestTrue(TEXT("Completed pair has current request identity"),Result.IsSet()&&Result->Matches(R)))return false;
    TestTrue(TEXT("Retained pair respects source live-array cap"),Open.Recording->Stats().PeakLiveArrayBytes<=Options.LiveArrayBytes);
    Result.Reset();TestEqual(TEXT("Releasing comparison releases original arrays"),Open.Recording->Stats().LiveArrayBytes,int64(0));
    if(!TestTrue(TEXT("Final request starts"),Task.Start(R,Error)))return false;
    Task.Shutdown();TestFalse(TEXT("Shutdown joins worker"),Task.IsBusy());TestFalse(TEXT("Shutdown cannot restart"),Task.Start(R,Error));
    TestEqual(TEXT("Shutdown releases unpublished arrays"),Open.Recording->Stats().LiveArrayBytes,int64(0));
    return true;
}
#endif
