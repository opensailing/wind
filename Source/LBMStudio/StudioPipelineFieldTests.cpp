#include "StudioPipelineField.h"
#include "StudioSavedFieldView.h"
#include "StudioPointRecording.h"
#include "StudioSliceRendering.h"
#include "StudioProbeSampling.h"
#include "StudioVolume.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"
#include <cmath>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioPipelineFieldTestsPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString Fixture(const TCHAR* Name){return FPaths::ProjectContentDir()/TEXT("Samples")/Name;}
FString Output(){return FPaths::ProjectSavedDir()/TEXT("Automation/PipelineFields");}
FStudioPipelineOperation Op(EStudioPipelineOperation Kind,const TCHAR* Name)
{FStudioPipelineOperation O;O.Kind=Kind;O.Name=Name;return O;}
struct FFixture
{
    FStudioPipelinePrepareRequest Request;
    FString Error;
    bool Load(bool Volume=false,bool Reconstructed=true)
    {
        const FString Folder=Volume?TEXT("Cylinder3D_ReaderFixture"):TEXT("NACA0018_ReaderFixture");
        auto R=StudioRecordings::Import(Fixture(*Folder)/TEXT("recording.json"),1,{});
        if(!R.Source||!R.Reference.IsSet()){Error=R.Error;return false;}
        if(Reconstructed)R=StudioRecordings::ImportReconstruction(*R.Reference,Fixture(Volume?TEXT("Cylinder3D_VolumeFixture"):TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),1,{});
        if(!R.Source||!R.Reference.IsSet()){Error=R.Error;return false;}
        auto Read=R.Source->ReadScalarFrame(1,TEXT("velocity_u"));
        if(!Read.Field||!Read.Field->Identity().IsSet()){Error=Read.Error;return false;}
        Request.ProjectId=FGuid::NewGuid();Request.Revision=17;Request.Source=R.Source;
        auto& P=Request.Recipe;P.Name=TEXT("Published magnitude");P.Source.Title=R.Source->Descriptor().Title;
        P.Source.Identity=*Read.Field->Identity();P.Source.Reference=R.Reference;
        auto M=Op(EStudioPipelineOperation::Magnitude,TEXT("Speed"));M.Field=TEXT("derived.speed");M.Unit=TEXT("m/s");M.Components={TEXT("velocity_u"),TEXT("velocity_v")};
        if(Volume)M.Components.Add(TEXT("velocity_w"));P.Operations={M};return true;
    }
};
// Fault injection changes metadata/read selection only. Every returned field
// still consists of unmodified original published CFD rows.
class FReadFault final : public IStudioSolver
{
public:
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Inner;
    FStudioRecordingDescriptor Meta;
    bool bWrongFrame=false;
    mutable int32 Reads=0;
    explicit FReadFault(TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source):Inner(Source),Meta(Source->Descriptor()){}
    int32 FrameCount() const override {return Inner->FrameCount();}
    FStudioFrame EvaluateFrame(int32 I) const override {return Inner->EvaluateFrame(I);}
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32 I) const override {return Inner->CaptureField(I);}
    FStudioFieldReadResult ReadScalarFrame(int32 I,const FString& Id,const FStudioLoadCancellation& C={}) const override
    {++Reads;return Inner->ReadScalarFrame(bWrongFrame?I+1:I,Id,C);}
    bool ExportField(int32,const FString&) const override {return false;}
    FString LoadError() const override {return Inner->LoadError();}
    const FStudioRecordingDescriptor& Descriptor() const override {return Meta;}
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Reconstruction() const override {return Inner->Reconstruction();}
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> VolumeReconstruction() const override {return Inner->VolumeReconstruction();}
};
TOptional<FStudioPipelinePrepared> Finish(FStudioPipelinePrepareTask& Task)
{
    const double End=FPlatformTime::Seconds()+20;
    do{if(auto R=Task.Poll())return R;FPlatformProcess::SleepNoStats(.001f);}while(FPlatformTime::Seconds()<End);return {};
}
bool SaveCSV(const FString& Name,const FString& Text)
{IFileManager::Get().MakeDirectory(*Output(),true);return FFileHelper::SaveStringToFile(Text,*(Output()/Name),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);}
bool SaveCase(const FString& Name,const FString& Text,const FStudioPipelineField& Field)
{
    const auto I=Field.Identity();auto O=MakeShared<FJsonObject>();
    O->SetStringField(TEXT("dataset"),I->Dataset);O->SetStringField(TEXT("sourceSHA256"),I->MetadataSHA256);
    O->SetStringField(TEXT("reconstructionSHA256"),I->ReconstructionSHA256);O->SetNumberField(TEXT("ordinal"),I->Ordinal);
    O->SetNumberField(TEXT("step"),I->Frame.Index);O->SetNumberField(TEXT("timeSeconds"),I->Frame.Time);
    O->SetStringField(TEXT("field"),Field.SelectedScalar().Id);O->SetStringField(TEXT("unit"),Field.SelectedScalar().Unit);
    O->SetStringField(TEXT("expression"),Field.ScalarExpression(Field.SelectedScalar().Id));
    FString JSON;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&JSON));return SaveCSV(Name,Text)&&SaveCSV(Name+TEXT(".json"),JSON);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineOriginalFields,"Studio.PipelineFields.ExactOriginalRowsAndDerivedGraph",StudioPipelineFieldTestsPrivate::Flags)
bool FPipelineOriginalFields::RunTest(const FString&)
{
    using namespace StudioPipelineFieldTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published NACA frame available"),F.Load()))return false;
    auto R=StudioPipelineFields::Prepare(F.Request);
    if(!TestTrue(*R.Error,R.Matches(F.Request.ProjectId,F.Request.Revision,F.Request.Recipe)))return false;
    const auto& P=*R.Field;const auto U=F.Request.Source->ReadScalarFrame(1,TEXT("velocity_u")).Field;
    const auto V=F.Request.Source->ReadScalarFrame(1,TEXT("velocity_v")).Field;
    FString CSV=TEXT("row,id,x,y,z,u,v,magnitude\n");
    for(int32 Row=0;Row<P.OriginalPointCount();++Row)
    {
        int64 Id,OriginalId;FVector Position,OriginalPosition;double A,B,M;
        if(!TestTrue(TEXT("All original rows remain available"),P.OriginalPoint(Row,Id,Position)&&U->OriginalPoint(Row,OriginalId,OriginalPosition)&&
            U->OriginalScalar(Row,TEXT("velocity_u"),A)&&V->OriginalScalar(Row,TEXT("velocity_v"),B)&&P.OriginalScalar(Row,TEXT("derived.speed"),M)))return false;
        if(Id!=OriginalId||Position!=OriginalPosition||M!=std::hypot(std::hypot(0.,A),B)){AddError(TEXT("Original row identity or derived value changed."));return false;}
        CSV+=FString::Printf(TEXT("%d,%lld,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g\n"),Row,Id,Position.X,Position.Y,Position.Z,A,B,M);
    }
    TestTrue(TEXT("Export original rows for independent source readback"),SaveCase(TEXT("naca-original.csv"),CSV,P));
    TestEqual(TEXT("Inputs retain only two original scalar arrays"),P.InputValueBytes(),int64(P.OriginalPointCount())*16);
    TestEqual(TEXT("Derived scalar is labeled"),P.SelectedScalar().Origin,FString(TEXT("pipeline-derived")));
    TestTrue(TEXT("Expression states interpolation order"),P.ScalarExpression(TEXT("derived.speed")).Contains(TEXT("interpolated before magnitude")));
    auto Q=F.Request;auto Next=Op(EStudioPipelineOperation::Magnitude,TEXT("Norm of prior output and U"));
    Next.Field=TEXT("derived.combined");Next.Unit=TEXT("m/s");Next.Components={TEXT("derived.speed"),TEXT("velocity_u")};Q.Recipe.Operations.Add(Next);
    const auto Combined=StudioPipelineFields::Prepare(Q);if(!TestTrue(*Combined.Error,Combined.Field.IsValid()))return false;
    double A,B;P.OriginalScalar(37,TEXT("derived.speed"),A);U->OriginalScalar(37,TEXT("velocity_u"),B);double Actual;
    TestTrue(TEXT("Derived dependency chain retains numerical meaning"),Combined.Field->OriginalScalar(37,TEXT("derived.combined"),Actual)&&Actual==std::hypot(A,B));
    TestEqual(TEXT("Reusing a component does not duplicate its array ownership"),Combined.Field->InputValueBytes(),P.InputValueBytes());
    auto Pressure=Op(EStudioPipelineOperation::Field,TEXT("Pressure"));Pressure.Field=TEXT("pressure");Pressure.Unit=TEXT("Pa");Q.Recipe.Operations.Add(Pressure);
    const auto Changed=StudioPipelineFields::Prepare(Q);TestTrue(TEXT("Later field selection changes final scalar"),Changed.Field&&Changed.Field->SelectedScalar().Id==TEXT("pressure"));
    TestEqual(TEXT("Earlier prepared result is immutable"),P.SelectedScalar().Id,FString(TEXT("derived.speed")));
    FFixture Raw;if(!TestTrue(TEXT("Original point-only source available"),Raw.Load(false,false)))return false;
    auto RawResult=StudioPipelineFields::Prepare(Raw.Request);TestTrue(TEXT("Magnitude works without manufactured connectivity"),RawResult.Field&&RawResult.Field->Identity()->Interpolation==EStudioFieldInterpolation::None);
    double Missing=17;TestFalse(TEXT("No interpolation invented for point-only output"),RawResult.Field->SampleScalar(FVector(.2,0,.02),TEXT("derived.speed"),Missing));
    TestEqual(TEXT("Unavailable scalar never becomes zero"),Missing,17.);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineSpatialQueries,"Studio.PipelineFields.ClipSliceAndProbeQueries",StudioPipelineFieldTestsPrivate::Flags)
bool FPipelineSpatialQueries::RunTest(const FString&)
{
    using namespace StudioPipelineFieldTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published NACA interpolation available"),F.Load()))return false;
    const auto Raw=StudioPipelineFields::Prepare(F.Request);if(!TestTrue(*Raw.Error,Raw.Field.IsValid()))return false;
    auto Clip=Op(EStudioPipelineOperation::ClipBox,TEXT("Wake box"));Clip.A=FVector(.15,-.1,-.1);Clip.B=FVector(.4,.1,.1);
    F.Request.Recipe.Operations.Add(Clip);
    auto Slice=Op(EStudioPipelineOperation::Slice,TEXT("Original plane"));Slice.A=F.Request.Recipe.Source.Identity.SourceOffset;Slice.B=FVector::RightVector;
    F.Request.Recipe.Operations.Add(Slice);
    auto R=StudioPipelineFields::Prepare(F.Request);if(!TestTrue(*R.Error,R.Field.IsValid()))return false;
    const FVector Inside(.25,0,.02),Outside(.5,0,.02);double Value=91;
    TestTrue(TEXT("Intersection permits supported wake sample"),R.Field->SampleScalar(Inside,TEXT("derived.speed"),Value));
    TestFalse(TEXT("Clip excludes otherwise supported source location"),R.Field->SampleScalar(Outside,TEXT("derived.speed"),Value));
    TestFalse(TEXT("2D query rejects extrusion"),R.Field->SampleScalar(Inside+FVector(0,.001,0),TEXT("derived.speed"),Value));
    TestFalse(TEXT("Continuous support cannot cross clip"),R.Field->SupportsSegment(Inside,Outside));
    TestTrue(TEXT("Valid segment retains source coverage"),R.Field->SupportsSegment(Inside,Inside+FVector(.01,0,0)));
    int32 Excluded=0;
    for(int32 Row=0;Row<R.Field->OriginalPointCount();++Row)
    {
        int64 Id;FVector P;R.Field->OriginalPoint(Row,Id,P);P=FVector(P.X,P.Z,P.Y);
        if(!R.Field->Includes(P)){++Excluded;double A,B;if(!Raw.Field->OriginalScalar(Row,TEXT("derived.speed"),A)||!R.Field->OriginalScalar(Row,TEXT("derived.speed"),B)||A!=B){AddError(TEXT("Clipping changed original rows."));return false;}}
    }
    TestTrue(TEXT("Substantial clipped original rows remain intact"),Excluded>1000);
    FStudioProbeRequest Probe;Probe.ProjectId=F.Request.ProjectId;Probe.PresentationId=1;Probe.Field=R.Field;Probe.DisplayedScalar=TEXT("derived.speed");
    Probe.Probe.Name=TEXT("Wake probe");Probe.Probe.Source={R.Field->Identity()->Dataset,R.Field->Identity()->MetadataSHA256,R.Field->Identity()->PayloadSHA256};
    Probe.Probe.A=FVector(.1,0,.02);Probe.Probe.B=FVector(.45,0,.02);Probe.Probe.Samples=71;Probe.Probe.Kind=EStudioProbeKind::Line;
    const auto Samples=StudioProbeSampling::Evaluate(Probe);TestTrue(TEXT("Existing immutable probe sampler accepts derived field"),Samples.Status==EStudioProbeStatus::Ready);
    FString CSV=TEXT("x,y,z,available,value\n");int32 Available=0,Missing=0;
    for(const auto& S:Samples.Samples)
    {const FVector P=*S.ScenePosition;if(S.Value.IsSet())++Available;else ++Missing;CSV+=FString::Printf(TEXT("%.17g,%.17g,%.17g,%d,"),P.X,P.Y,P.Z,int32(S.Value.IsSet()));if(S.Value.IsSet())CSV+=FString::Printf(TEXT("%.17g"),*S.Value);CSV+=TEXT("\n");}
    TestTrue(TEXT("Inside values and clipped missing samples are explicit"),Available>10&&Missing>10);
    TestTrue(TEXT("Export queries for independent interpolation audit"),SaveCase(TEXT("naca-probes.csv"),CSV,*R.Field));
    FStudioSliceObject Plane;Plane.Name=TEXT("Wake slice");Plane.Source=Probe.Probe.Source;Plane.Origin=Slice.A;Plane.Normal=Slice.B;
    const auto Mesh=StudioSliceRendering::Build(*R.Field,R.Field->DomainBounds(),{Plane},TEXT("derived.speed"));
    TestTrue(TEXT("Existing slice sampler accepts derived scalar graph"),Mesh.Indices.Num()>1000);
    for(int32 I:Mesh.Indices)if(!R.Field->Includes(Mesh.PositionsMeters[I])){AddError(TEXT("Slice escaped the pipeline domain."));return false;}
    auto Empty=Clip;Empty.Id=FGuid::NewGuid();Empty.Name=TEXT("Disjoint clip");Empty.A=FVector(4,4,4);Empty.B=FVector(5,5,5);F.Request.Recipe.Operations.Add(Empty);
    auto None=StudioPipelineFields::Prepare(F.Request);TestTrue(TEXT("Disjoint clips produce an explicit valid empty domain"),None.Field&&None.Field->HasEmptyDomain());
    TestFalse(TEXT("Empty domain returns no samples"),None.Field->SampleScalar(Inside,TEXT("derived.speed"),Value));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineVolumeQueries,"Studio.PipelineFields.OriginalThreeDimensionalMagnitude",StudioPipelineFieldTestsPrivate::Flags)
bool FPipelineVolumeQueries::RunTest(const FString&)
{
    using namespace StudioPipelineFieldTestsPrivate;
    FFixture F;if(!TestTrue(*F.Error,F.Load(true)))return false;
    auto R=StudioPipelineFields::Prepare(F.Request);if(!TestTrue(*R.Error,R.Field.IsValid()))return false;
    TestTrue(TEXT("Genuine source dimension and reconstruction retained"),R.Field->Identity()->SpatialDimensions==3&&R.Field->VolumeReconstruction()==F.Request.Source->VolumeReconstruction());
    auto Slice=Op(EStudioPipelineOperation::Slice,TEXT("Oblique plane"));Slice.A=F.Request.Source->Descriptor().DisplayBounds.GetCenter();Slice.B=FVector(1,2,3).GetSafeNormal();
    F.Request.Recipe.Operations.Add(Slice);auto S=StudioPipelineFields::Prepare(F.Request);if(!TestTrue(*S.Error,S.Field.IsValid()))return false;
    TestTrue(TEXT("Original3D values remain readable after slicing"),S.Field->OriginalPointCount()==R.Field->OriginalPointCount());
    TestFalse(TEXT("Off-slice sample is unavailable"),S.Field->Includes(Slice.A+Slice.B*.001));
    const auto& Bounds=R.Field->DomainBounds();FString CSV=TEXT("x,y,z,available,value\n");int32 Count=0;
    for(int32 I=1;I<16;++I)for(int32 J=1;J<12;++J)for(int32 K=1;K<9;++K)
    {
        const FVector P=Bounds.Min+Bounds.GetSize()*FVector(I/16.,J/12.,K/9.);double V;const bool Valid=R.Field->SampleScalar(P,TEXT("derived.speed"),V);
        CSV+=FString::Printf(TEXT("%.17g,%.17g,%.17g,%d,"),P.X,P.Y,P.Z,int32(Valid));if(Valid){++Count;CSV+=FString::Printf(TEXT("%.17g"),V);}CSV+=TEXT("\n");
    }
    TestTrue(TEXT("Substantial authentic3D supported samples"),Count>100);TestTrue(TEXT("Export3D derived queries"),SaveCase(TEXT("cylinder-probes.csv"),CSV,*R.Field));
    auto Volume=R.Field->VolumeReconstruction();TestTrue(TEXT("Volume topology remains the verified original reconstruction"),Volume&&Volume->MetadataSHA256==R.Field->Identity()->ReconstructionSHA256);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineFieldContracts,"Studio.PipelineFields.SourceContractsAndOriginalTopology",StudioPipelineFieldTestsPrivate::Flags)
bool FPipelineFieldContracts::RunTest(const FString&)
{
    using namespace StudioPipelineFieldTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published source available"),F.Load()))return false;
    auto Fault=MakeShared<FReadFault,ESPMode::ThreadSafe>(F.Request.Source);F.Request.Source=Fault;
    Fault->bWrongFrame=true;auto R=StudioPipelineFields::Prepare(F.Request);
    TestTrue(TEXT("Original adjacent frame cannot replace requested frame"),!R.Error.IsEmpty()&&!R.Field&&!R.Source&&R.Stages.IsEmpty());
    Fault->bWrongFrame=false;Fault->Meta.Scalars[0].Label+=TEXT(" changed");R=StudioPipelineFields::Prepare(F.Request);
    TestTrue(TEXT("Actual scalar metadata must match source declaration"),!R.Error.IsEmpty()&&!R.Field&&!R.Source);
    Fault->Meta=Fault->Inner->Descriptor();Fault->Meta.NodeCount=StudioPipelineFields::MaxSourcePoints+1;Fault->Reads=0;
    R=StudioPipelineFields::Prepare(F.Request);TestTrue(TEXT("Oversized source rejected before any scalar allocation"),!R.Error.IsEmpty()&&!R.Field&&Fault->Reads==0);
    TestTrue(TEXT("Analysis failures do not change source playback error"),Fault->Inner->LoadError().IsEmpty());
    FStudioPipelinePrepareRequest Legacy;Legacy.ProjectId=FGuid::NewGuid();Legacy.Revision=1;
    Legacy.Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();auto Read=Legacy.Source->ReadScalarFrame(1,TEXT("pressure"));
    if(!TestTrue(TEXT("Published SU2 original mesh available"),Read.Field&&Read.Field->Identity().IsSet()))return false;
    Legacy.Recipe.Name=TEXT("Original SU2 pressure");Legacy.Recipe.Source.Title=Legacy.Source->Descriptor().Title;Legacy.Recipe.Source.Identity=*Read.Field->Identity();
    auto Selection=Op(EStudioPipelineOperation::Field,TEXT("Pressure"));Selection.Field=TEXT("pressure");Selection.Unit=TEXT("Pa");Legacy.Recipe.Operations={Selection};
    const auto Prepared=StudioPipelineFields::Prepare(Legacy);if(!TestTrue(*Prepared.Error,Prepared.Field.IsValid()))return false;
    TestEqual(TEXT("Original mesh count retained"),Prepared.Field->OriginalTriangleCount(),Read.Field->OriginalTriangleCount());
    for(int32 I=0;I<Prepared.Field->OriginalTriangleCount();++I)
    {
        FIntVector A,B;if(!Prepared.Field->OriginalTriangle(I,A)||!Read.Field->OriginalTriangle(I,B)||A!=B){AddError(TEXT("Original topology changed."));return false;}
    }
    TestTrue(TEXT("Source connectivity is not replaced by reconstruction"),Prepared.Field->OriginalTriangleCount()>0&&!Prepared.Field->Reconstruction());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineWorker,"Studio.PipelineFields.PinnedRestoreCancellationAndLifetime",StudioPipelineFieldTestsPrivate::Flags)
bool FPipelineWorker::RunTest(const FString&)
{
    using namespace StudioPipelineFieldTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published field available"),F.Load()))return false;
    F.Request.Source.Reset();const auto R=StudioPipelineFields::Prepare(F.Request);
    if(!TestTrue(*R.Error,R.Matches(F.Request.ProjectId,F.Request.Revision,F.Request.Recipe)))return false;
    TestFalse(TEXT("Different project cannot adopt completion"),R.Matches(FGuid::NewGuid(),F.Request.Revision,F.Request.Recipe));
    TestFalse(TEXT("Different revision cannot adopt completion"),R.Matches(F.Request.ProjectId,F.Request.Revision+1,F.Request.Recipe));
    auto Changed=F.Request.Recipe;Changed.Source.Camera.Position.X+=1;TestFalse(TEXT("Recipe edit cannot adopt stale completion"),R.Matches(F.Request.ProjectId,F.Request.Revision,Changed));
    auto Bad=F.Request;Bad.Recipe.Source.Identity.Frame.Time+=.01;auto Failed=StudioPipelineFields::Prepare(Bad);
    TestTrue(TEXT("Changed original time publishes no partial ownership"),!Failed.Error.IsEmpty()&&!Failed.Field&&!Failed.Source&&Failed.Stages.IsEmpty());
    Bad=F.Request;Bad.Recipe.Source.Reference->Path=Output()/TEXT("missing.json");Failed=StudioPipelineFields::Prepare(Bad);
    TestTrue(TEXT("Missing pinned source cannot substitute current source"),!Failed.Error.IsEmpty()&&!Failed.Field&&!Failed.Source);
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);Failed=StudioPipelineFields::Prepare(F.Request,Cancel);
    TestTrue(TEXT("Early cancellation publishes nothing"),Failed.bCancelled&&!Failed.Field&&!Failed.Source);
    FStudioPipelinePrepareTask Task;FString Error;TestTrue(TEXT("Start one worker"),Task.Start(F.Request,Error));
    TestFalse(TEXT("No unbounded queued worker"),Task.Start(F.Request,Error));Task.Cancel();auto Result=Finish(Task);
    if(!TestTrue(TEXT("Cancelled worker drains"),Result.IsSet()))return false;
    TestTrue(TEXT("Cancellation discards partial sources and fields"),Result->bCancelled&&!Result->Field&&!Result->Source&&!Task.IsBusy());
    TestTrue(TEXT("Reusable after cancellation"),Task.Start(F.Request,Error));Result=Finish(Task);
    if(!TestTrue(TEXT("Completed worker matches current recipe"),Result.IsSet()&&Result->Matches(F.Request.ProjectId,F.Request.Revision,F.Request.Recipe)))return false;
    TWeakPtr<const FStudioPipelineField,ESPMode::ThreadSafe> Weak=Result->Field;
    TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> Source=Result->Source;Result.Reset();
    TestFalse(TEXT("Polled task retains no field"),Weak.IsValid());TestFalse(TEXT("Polled task retains no source"),Source.IsValid());
    TestTrue(TEXT("Start shutdown case"),Task.Start(F.Request,Error));Task.Shutdown();
    TestFalse(TEXT("Shutdown joins task"),Task.IsBusy());TestFalse(TEXT("Shutdown is final"),Task.Start(F.Request,Error));
    return true;
}
#endif
