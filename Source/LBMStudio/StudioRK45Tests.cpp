#include "StudioStreamlines.h"
#include "StudioModel.h"
#include "StudioVolume.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioRK45TestPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
enum class Flow {Constant,Circle,Helix,Mixed};
// Artificial differential equations only; these are never installed CFD data.
class Field final:public IStudioField
{
public:
    Flow Kind=Flow::Constant;FVector Origin=FVector::ZeroVector;double Scale=1;int32 Dimensions=2;
    bool bGap=false,bZero=false,bMissingVelocity=false,bMissingScalar=false;
    mutable int32 Velocities=0,Scalars=0,Supports=0;
    int32 CancelVelocity=0,CancelScalar=0,CancelSupport=0;
    FStudioLoadCancellation Cancellation;
    bool IsValid() const override{return true;}
    TOptional<FStudioFieldIdentity> Identity() const override
    {FStudioFieldIdentity I;I.Dataset=TEXT("RK45_ArtificialMathematics");I.MetadataSHA256=FString::ChrN(64,'a');I.SpatialDimensions=Dimensions;
        I.Interpolation=EStudioFieldInterpolation::SourceTriangles;I.SourceOffset=Origin;I.Ordinal=17;I.Frame={1234,2.468};return I;}
    bool Sample(const FVector& P,FStudioFieldValue& Out) const override
    {return SampleVelocity(P,Out.Velocity)&&SampleScalar(P,TEXT("pressure"),Out.Pressure);}
    bool SampleVelocity(const FVector& P,FVector& Out) const override
    {
        ++Velocities;if(Cancellation&&Velocities==CancelVelocity)Cancellation->store(true);
        if(bMissingVelocity)return false;if(bZero){Out=FVector::ZeroVector;return true;}
        const FVector Q=(P-Origin)/Scale;
        const bool Curved=Kind==Flow::Circle||Kind==Flow::Helix||(Kind==Flow::Mixed&&Q.X<2);
        Out=Curved?FVector(-Q.Z,Kind==Flow::Helix?.5:0,Q.X):FVector(.6,0,.8);return true;
    }
    bool SampleScalar(const FVector& P,const FString& Id,double& Out) const override
    {
        ++Scalars;if(Cancellation&&Scalars==CancelScalar)Cancellation->store(true);
        if(bMissingScalar||Id!=TEXT("pressure"))return false;
        const FVector Q=(P-Origin)/Scale;Out=2*Q.X-3*Q.Z+Q.Y;return true;
    }
    bool SupportsSegment(const FVector& A,const FVector& B,const FStudioLoadCancellation& C) const override
    {
        ++Supports;if(Cancellation&&Supports==CancelSupport)Cancellation->store(true);
        if(C&&C->load())return false;
        const double X=(A.X-Origin.X)/Scale,Y=(B.X-Origin.X)/Scale;
        return !bGap||FMath::Max(X,Y)<.045||FMath::Min(X,Y)>.046;
    }
    bool IsSolid(const FVector&) const override{return false;}
    const TArray<FVector2D>& Boundary() const override{static const TArray<FVector2D> Empty;return Empty;}
    const TArray<FIntVector>& BoundaryTriangles() const override{static const TArray<FIntVector> Empty;return Empty;}
    FBox Bounds() const{return FBox(Origin+Scale*FVector(-5,-5,-5),Origin+Scale*FVector(5,5,5));}
};
FStudioSeedObject Seed(const IStudioField& F,const TArray<FVector>& Points)
{FStudioSeedObject S;S.Name=TEXT("Numerical test seeds");S.Kind=EStudioSeedKind::Points;S.Points=Points;const auto I=*F.Identity();S.Source={I.Dataset,I.MetadataSHA256,I.PayloadSHA256};return S;}
FStudioStreamlineSettings Tight()
{FStudioStreamlineSettings S;S.StepFraction=.1;S.MaximumLength=.1;S.MaximumSteps=4096;S.WorkBudget=16000;
    S.AbsoluteToleranceFraction=1.e-10;S.RelativeTolerance=1.e-10;return S;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRK45Constant,"Studio.Streamlines.RK45.ConstantFlowAndLimits",StudioRK45TestPrivate::Flags)
bool FStudioRK45Constant::RunTest(const FString&)
{
    using namespace StudioRK45TestPrivate;Field F;const auto Seeds=Seed(F,{FVector::ZeroVector});auto S=Tight();S.Direction=EStudioStreamDirection::Both;
    S.StepFraction=.013;S.MaximumLength=.071;FStudioStreamlineOutput O;FString Error;
    if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(F,F.Bounds(),{Seeds},S,TEXT("pressure"),O,Error)))return false;
    TestTrue(TEXT("New settings use Dormand-Prince"),S.Method==EStudioStreamMethod::DormandPrince45&&O.Method==S.Method);
    TestEqual(TEXT("No rejection for constant vector"),O.RejectedAttempts,0);TestEqual(TEXT("Both branches"),O.Paths.Num(),2);
    for(const auto& P:O.Paths)
    {
        TestTrue(TEXT("Exact normalized direction and fractional final arc"),P.PositionsMeters.Last().Equals((P.bBackward?-1.:1.)*.71*FVector(.6,0,.8),1.e-12));
        TestTrue(TEXT("Arc limit exact"),FMath::Abs(P.IntegrationLengthMeters-.71)<1.e-12&&P.End==EStudioStreamEnd::LengthLimit);
        TestTrue(TEXT("Rendered length obeys limit"),P.LengthMeters<=.71+1.e-12);
        for(int32 I=0;I<P.PositionsMeters.Num();++I)TestEqual(TEXT("Original scalar at accepted endpoint"),P.Scalars[I],2*P.PositionsMeters[I].X-3*P.PositionsMeters[I].Z);
    }
    TestEqual(TEXT("Velocity calls accounted"),O.VelocityEvaluations,F.Velocities);TestEqual(TEXT("Scalar calls accounted"),O.ScalarEvaluations,F.Scalars);TestEqual(TEXT("Support calls accounted"),O.SupportEvaluations,F.Supports);
    TestEqual(TEXT("Seven stage evaluations per attempt plus seed"),O.VelocityEvaluations,1+7*O.Attempts);
    S.WorkBudget=3;TestTrue(TEXT("Tiny budget publishes bounded partial result"),StudioStreamlines::Build(F,F.Bounds(),{Seeds},S,TEXT("pressure"),O,Error));
    TestTrue(TEXT("Round-robin attempts and geometry"),O.Attempts==3&&O.Segments==3&&O.bBudgetExhausted&&O.Paths[0].Attempts==2&&O.Paths[1].Attempts==1);
    for(const auto& P:O.Paths)TestTrue(TEXT("Every active branch ends work limit"),P.End==EStudioStreamEnd::WorkLimit);
    S.WorkBudget=100;S.MaximumSteps=2;TestTrue(TEXT("Branch cap succeeds"),StudioStreamlines::Build(F,F.Bounds(),{Seeds},S,TEXT("pressure"),O,Error));
    for(const auto& P:O.Paths)TestTrue(TEXT("Accepted segment cap"),P.PositionsMeters.Num()==3&&P.End==EStudioStreamEnd::StepLimit);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRK45Analytic,"Studio.Streamlines.RK45.AnalyticCircleAndHelix",StudioRK45TestPrivate::Flags)
bool FStudioRK45Analytic::RunTest(const FString&)
{
    using namespace StudioRK45TestPrivate;Field F;F.Kind=Flow::Circle;auto S=Tight();FStudioStreamlineOutput O;FString Error;
    const auto Seeds=Seed(F,{FVector(1,0,0)});
    S.AbsoluteToleranceFraction=1.e-5;S.RelativeTolerance=1.e-3;
    if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(F,F.Bounds(),{Seeds},S,TEXT("pressure"),O,Error)))return false;
    const FVector Exact(FMath::Cos(1.),0,FMath::Sin(1.));const double Loose=(O.Paths[0].PositionsMeters.Last()-Exact).Size();
    S=Tight();if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(F,F.Bounds(),{Seeds},S,TEXT("pressure"),O,Error)))return false;
    const double Fine=(O.Paths[0].PositionsMeters.Last()-Exact).Size();
    TestTrue(TEXT("Tight tolerance improves independent exact circle"),Fine<Loose*.05&&Fine<1.e-7);
    TestTrue(TEXT("Coarse trial rejects then adapts"),O.RejectedAttempts>0&&O.Segments>1);
    TestTrue(TEXT("Circle ends at exact integrated arc"),FMath::Abs(O.Paths[0].IntegrationLengthMeters-1.)<1.e-12);
    for(const auto& P:O.Paths[0].PositionsMeters)TestTrue(TEXT("Original exact circle radius retained numerically"),FMath::Abs(P.Size()-1.)<1.e-7);
    F.Origin=FVector(1000000,2000000,-3000000);F.Scale=2;
    const auto Moved=Seed(F,{F.Origin+FVector(2,0,0)});
    if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(F,F.Bounds(),{Moved},S,TEXT("pressure"),O,Error)))return false;
    TestTrue(TEXT("Tolerance uses translated/scaled domain coordinates"),((O.Paths[0].PositionsMeters.Last()-F.Origin)/F.Scale-Exact).Size()<2.e-7);
    F.Origin=FVector::ZeroVector;F.Scale=1;F.Kind=Flow::Helix;F.Dimensions=3;S.Direction=EStudioStreamDirection::Both;
    const auto Helix=Seed(F,{FVector(1,0,0)});
    if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(F,F.Bounds(),{Helix},S,TEXT("pressure"),O,Error)))return false;
    for(const auto& P:O.Paths)
    {
        const double Arc=(P.bBackward?-1.:1.)*P.IntegrationLengthMeters,Angle=Arc/FMath::Sqrt(1.25);
        const FVector H(FMath::Cos(Angle),.5*Angle,FMath::Sin(Angle));
        TestTrue(TEXT("Independent exact 3D helix all axes"),(P.PositionsMeters.Last()-H).Size()<1.e-7);
    }
    TestEqual(TEXT("Instantaneous source frame unchanged"),O.Identity->Frame.Index,1234);TestEqual(TEXT("Frame time never integrates"),O.Identity->Frame.Time,2.468);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRK45Budget,"Studio.Streamlines.RK45.AdaptiveBudgetAndAccuracyFloor",StudioRK45TestPrivate::Flags)
bool FStudioRK45Budget::RunTest(const FString&)
{
    using namespace StudioRK45TestPrivate;Field F;F.Kind=Flow::Mixed;auto S=Tight();S.StepFraction=.05;S.WorkBudget=2;
    const auto Seeds=Seed(F,{FVector(.1,0,0),FVector(3,0,0)});FStudioStreamlineOutput O;FString Error;
    if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(F,F.Bounds(),{Seeds},S,TEXT("pressure"),O,Error)))return false;
    if(!TestEqual(TEXT("Both numerical branches exist"),O.Paths.Num(),2))return false;
    TestTrue(TEXT("Rejected curved trial counts without a vertex"),O.Paths[0].Attempts==1&&O.Paths[0].RejectedAttempts==1&&O.Paths[0].PositionsMeters.Num()==1);
    TestTrue(TEXT("Straight branch gets its turn after rejection"),O.Paths[1].Attempts==1&&O.Paths[1].PositionsMeters.Num()==2);
    TestTrue(TEXT("Rejected work exhausts aggregate budget"),O.Attempts==2&&O.RejectedAttempts==1&&O.Segments==1&&O.bBudgetExhausted);
    TestTrue(TEXT("Stage calls strictly bounded"),O.VelocityEvaluations<=O.SeedCount+7*O.Attempts&&O.SupportEvaluations<=7*O.Attempts);
    F.Kind=Flow::Circle;S.StepFraction=.001;S.MinimumStepFraction=.001;S.RelativeTolerance=1.e-12;S.AbsoluteToleranceFraction=1.e-12;S.WorkBudget=100;
    const auto Tiny=Seed(F,{FVector(.001,0,0)});
    if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(F,F.Bounds(),{Tiny},S,TEXT("pressure"),O,Error)))return false;
    TestTrue(TEXT("No inaccurate acceptance at step floor"),O.Segments==0&&O.Paths[0].End==EStudioStreamEnd::AccuracyLimit&&O.Paths[0].RejectedAttempts==1&&O.Attempts==1);
    F.Kind=Flow::Constant;F.bGap=true;S=Tight();const auto Gap=Seed(F,{FVector::ZeroVector});
    TestTrue(TEXT("Narrow coverage gap handled"),StudioStreamlines::Build(F,F.Bounds(),{Gap},S,TEXT("pressure"),O,Error));
    TestTrue(TEXT("Valid endpoints cannot bridge unsampled gap"),O.Segments==0&&O.Paths[0].End==EStudioStreamEnd::CoverageUnavailable);
    F.bGap=false;F.bZero=true;TestTrue(TEXT("Exact stagnation terminates"),StudioStreamlines::Build(F,F.Bounds(),{Gap},S,TEXT("pressure"),O,Error)&&O.Paths[0].End==EStudioStreamEnd::Stagnation&&O.Segments==0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRK45Transactions,"Studio.Streamlines.RK45.TransactionalMigrationAndCancellation",StudioRK45TestPrivate::Flags)
bool FStudioRK45Transactions::RunTest(const FString&)
{
    using namespace StudioRK45TestPrivate;auto S=Tight();auto JSON=StudioStreamlines::ToJSON(S);FStudioStreamlineSettings Read;
    TestTrue(TEXT("RK45 settings round-trip exactly"),StudioStreamlines::FromJSON(JSON,Read)&&Read==S);
    JSON->RemoveField(TEXT("integration"));TestTrue(TEXT("Missing additive object migrates legacy midpoint"),StudioStreamlines::FromJSON(JSON,Read)&&Read.Method==EStudioStreamMethod::Midpoint);
    const auto Kept=Read;
    for(int32 Mode=0;Mode<5;++Mode)
    {
        JSON=StudioStreamlines::ToJSON(S);auto I=JSON->GetObjectField(TEXT("integration"));
        if(Mode==0)I->SetStringField(TEXT("method"),TEXT("unknown"));
        if(Mode==1)I->RemoveField(TEXT("relativeTolerance"));
        if(Mode==2)I->SetNumberField(TEXT("absoluteToleranceFraction"),-1);
        if(Mode==3)I->SetStringField(TEXT("minimumStepFraction"),TEXT("0.001"));
        if(Mode==4)I->SetNumberField(TEXT("minimumStepFraction"),.5);
        TestFalse(TEXT("Malformed integration rejected"),StudioStreamlines::FromJSON(JSON,Read));TestTrue(TEXT("Settings remain transactional"),Read==Kept);
    }
    Field Baseline;const auto Seeds=Seed(Baseline,{FVector::ZeroVector});FStudioStreamlineOutput O;FString Error;
    if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(Baseline,Baseline.Bounds(),{Seeds},S,TEXT("pressure"),O,Error)))return false;
    const auto Prior=O;
    for(int32 Stage=1;Stage<=7;++Stage)
    {
        Field F;F.Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);F.CancelVelocity=1+Stage;
        TestFalse(TEXT("Every RK stage honors cancellation"),StudioStreamlines::Build(F,F.Bounds(),{Seeds},S,TEXT("pressure"),O,Error,F.Cancellation));
        TestTrue(TEXT("Cancelled trial preserves complete previous geometry"),O.Attempts==Prior.Attempts&&O.Paths[0].PositionsMeters==Prior.Paths[0].PositionsMeters);
    }
    for(bool Scalar:{false,true})
    {
        Field F;F.Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);if(Scalar)F.CancelScalar=2;else F.CancelSupport=3;
        TestFalse(TEXT("Scalar and support cancellation honor transaction"),StudioStreamlines::Build(F,F.Bounds(),{Seeds},S,TEXT("pressure"),O,Error,F.Cancellation));
        TestTrue(TEXT("Previous scientific values retained"),O.Paths[0].Scalars==Prior.Paths[0].Scalars);
    }
    {
        Field F;F.Kind=Flow::Circle;F.Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);F.CancelVelocity=9;
        const auto Curved=Seed(F,{FVector(1,0,0)});
        TestFalse(TEXT("Cancellation while rejected trial waits to retry is atomic"),StudioStreamlines::Build(F,F.Bounds(),{Curved},S,TEXT("pressure"),O,Error,F.Cancellation));
        TestTrue(TEXT("Pending rejected trial cannot alter prior output"),O.Paths[0].PositionsMeters==Prior.Paths[0].PositionsMeters);
    }
    Field Foreign;auto Wrong=Seeds;Wrong.Source.MetadataSHA256=FString::ChrN(64,'f');
    TestTrue(TEXT("Foreign seed is explicit unavailable geometry"),StudioStreamlines::Build(Foreign,Foreign.Bounds(),{Wrong},S,TEXT("pressure"),O,Error)&&O.Paths.IsEmpty()&&O.Notices.Contains(Wrong.Id));
    S.RelativeTolerance=std::numeric_limits<double>::quiet_NaN();const auto Before=O;
    TestFalse(TEXT("Nonfinite error tolerance never launches"),StudioStreamlines::Build(Foreign,Foreign.Bounds(),{Seeds},S,TEXT("pressure"),O,Error));
    TestTrue(TEXT("Invalid call preserves prior output"),O.Paths.Num()==Before.Paths.Num()&&O.Notices.Num()==Before.Notices.Num());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRK45Recorded,"Studio.Streamlines.RK45.RecordedCoverageAndIdentity",StudioRK45TestPrivate::Flags)
bool FStudioRK45Recorded::RunTest(const FString&)
{
    using namespace StudioRK45TestPrivate;
    const auto Point=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Point.Error,Point.Reference.IsSet()))return false;
    const auto Surface=StudioRecordings::ImportReconstruction(*Point.Reference,FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture/reconstruction.json"),0,{});
    const auto VolumePoint=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Surface.Error,Surface.Source.IsValid())||!TestTrue(*VolumePoint.Error,VolumePoint.Reference.IsSet()))return false;
    const auto Volume=StudioRecordings::ImportReconstruction(*VolumePoint.Reference,FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json"),0,{});
    if(!TestTrue(*Volume.Error,Volume.Source.IsValid()))return false;
    TArray<TSharedRef<IStudioSolver,ESPMode::ThreadSafe>> Sources{MakeShared<FRecordedSolver,ESPMode::ThreadSafe>(),Surface.Source.ToSharedRef(),Volume.Source.ToSharedRef()};
    for(const auto& Solver:Sources)
    {
        const auto F=Solver->CaptureViewField(0,TEXT("pressure"),true);if(!TestTrue(TEXT("Immutable recorded velocity/scalar available"),F->IsValid()))return false;
        const auto I=*F->Identity();const FBox Bounds=StudioStreamlines::DomainBounds(*F,Solver->Descriptor().DisplayBounds);
        TArray<FVector> Positions;FString Error;if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::AutomaticSeeds(*F,Bounds,24,EStudioStreamDirection::Forward,Positions,Error)))return false;
        TestTrue(TEXT("Actual recording supports deterministic seeds"),Positions.Num()>4);
        const auto Seeds=Seed(*F,Positions);FStudioStreamlineSettings S;S.MaximumSteps=20;S.WorkBudget=2000;S.StepFraction=.001;
        FStudioStreamlineOutput O;if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(*F,Bounds,{Seeds},S,TEXT("pressure"),O,Error)))return false;
        TestTrue(TEXT("Substantial actual recorded traces"),O.Segments>30);
        TestTrue(TEXT("Exact snapshot source identity preserved"),O.Identity->Dataset==I.Dataset&&O.Identity->MetadataSHA256==I.MetadataSHA256&&O.Identity->PayloadSHA256==I.PayloadSHA256&&
            O.Identity->ReconstructionSHA256==I.ReconstructionSHA256&&O.Identity->Ordinal==I.Ordinal&&O.Identity->Frame.Time==I.Frame.Time&&O.Identity->SourceOffset==I.SourceOffset);
        for(const auto& P:O.Paths)for(int32 N=0;N<P.PositionsMeters.Num();++N)
        {
            double Value;if(!TestTrue(TEXT("Every emitted scalar is supported recorded data"),F->SampleScalar(P.PositionsMeters[N],TEXT("pressure"),Value)))return false;
            TestEqual(TEXT("Raw recorded value unchanged"),P.Scalars[N],Value);
            if(N){TestTrue(TEXT("Complete segment support retained"),F->SupportsSegment(P.PositionsMeters[N-1],P.PositionsMeters[N]));
                for(double T:{.25,.5,.75})TestTrue(TEXT("Interior has actual recorded scalar"),F->SampleScalar(FMath::Lerp(P.PositionsMeters[N-1],P.PositionsMeters[N],T),TEXT("pressure"),Value));}
        }
        const auto Later=Solver->CaptureViewField(2,TEXT("pressure"),true);FStudioStreamlineOutput Next;
        if(!TestTrue(TEXT("Valid source operation"),StudioStreamlines::Build(*Later,Bounds,{Seeds},S,TEXT("pressure"),Next,Error)))return false;
        TestEqual(TEXT("Separate frame retains its own identity"),Next.Identity->Ordinal,2);
        bool Changed=false;for(int32 N=0;N<FMath::Min(O.Paths.Num(),Next.Paths.Num());++N)
            Changed|=O.Paths[N].PositionsMeters!=Next.Paths[N].PositionsMeters||O.Paths[N].Scalars!=Next.Paths[N].Scalars;
        TestTrue(TEXT("Actual changing data changes geometry or values"),Changed);
    }
    return true;
}
#endif
