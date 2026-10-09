#include "StudioProbeHistory.h"
#include "StudioModel.h"
#include "StudioPointRecording.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto HistoryProbeFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString HistoryFixture(const TCHAR* Name){return FPaths::ProjectContentDir()/TEXT("Samples")/Name;}
TSharedPtr<FJsonObject> HistoryJSON(const FString& Path)
{
    FString Text;TSharedPtr<FJsonObject> Out;
    if(FFileHelper::LoadFileToString(Text,*Path))FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Out);
    return Out;
}
FStudioProbeHistoryRequest HistoryRequest(TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Source,const FString& Scalar=TEXT("pressure"))
{
    FStudioProbeHistoryRequest R;R.ProjectId=FGuid::NewGuid();R.Source=Source;R.Probe.Name=TEXT("Recorded probe history");
    const auto& D=Source->Descriptor();R.Probe.Source={D.Id,D.MetadataSHA256,D.PayloadSHA256};R.Scalar=Scalar;
    R.FirstOrdinal=0;R.LastOrdinal=Source->FrameCount()-1;return R;
}
TOptional<FStudioProbeHistoryResult> FinishHistory(FStudioProbeHistoryTask& Task)
{
    const double Deadline=FPlatformTime::Seconds()+15.;
    do {if(auto Result=Task.Poll())return Result;FPlatformProcess::SleepNoStats(.001f);}while(FPlatformTime::Seconds()<Deadline);
    return {};
}
// Fault injection changes identity/read outcomes only. All scalar values still
// come from the published CFD fixtures, never an invented numerical recording.
class FHistoryIdentityField final : public IStudioField
{
public:
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Inner;
    FStudioFieldIdentity Changed;
    bool IsValid() const override{return Inner->IsValid();}
    TOptional<FStudioFieldIdentity> Identity() const override{return Changed;}
    TOptional<FStudioScalarDescriptor> Scalar(const FString& Id) const override{return Inner->Scalar(Id);}
    bool Sample(const FVector& P,FStudioFieldValue& Out) const override{return Inner->Sample(P,Out);}
    bool SampleScalar(const FVector& P,const FString& Id,double& Out) const override{return Inner->SampleScalar(P,Id,Out);}
    bool IsSolid(const FVector& P) const override{return Inner->IsSolid(P);}
    const TArray<FVector2D>& Boundary() const override{return Inner->Boundary();}
    const TArray<FIntVector>& BoundaryTriangles() const override{return Inner->BoundaryTriangles();}
    TSharedPtr<const FStudioPointFrame,ESPMode::ThreadSafe> OriginalPoints() const override{return Inner->OriginalPoints();}
};
class FHistoryReadFixture final : public IStudioSolver
{
public:
    explicit FHistoryReadFixture(TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> In):Inner(In),Meta(In->Descriptor()){}
    TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Inner;
    FStudioRecordingDescriptor Meta;
    int32 FailAt=INDEX_NONE,ChangeAt=INDEX_NONE;
    TFunction<void(FStudioFieldIdentity&)> Change;
    TFunction<void(int32)> OnRead;
    int32 FrameCount() const override{return Meta.Frames.Num();}
    FStudioFrame EvaluateFrame(int32 I) const override{return Meta.Frames[I];}
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32 I) const override{return Inner->CaptureField(I);}
    FStudioFieldReadResult ReadScalarFrame(int32 I,const FString& S,const FStudioLoadCancellation& C={}) const override
    {
        if(OnRead)OnRead(I);
        if(I==FailAt)return {{},TEXT("Injected read failure")};
        auto Read=Inner->ReadScalarFrame(I,S,C);
        if(I==ChangeAt&&Read.Field&&Read.Field->Identity().IsSet())
        {
            auto Field=MakeShared<FHistoryIdentityField,ESPMode::ThreadSafe>();Field->Inner=Read.Field;
            Field->Changed=*Read.Field->Identity();Change(Field->Changed);Read.Field=Field;
        }
        return Read;
    }
    bool ExportField(int32,const FString&) const override{return false;}
    FString LoadError() const override{return Inner->LoadError();}
    const FStudioRecordingDescriptor& Descriptor() const override{return Meta;}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeHistoryOriginal,"Studio.ProbeHistory.ExactOriginalPointsAndTimes",HistoryProbeFlags)
bool FProbeHistoryOriginal::RunTest(const FString&)
{
    const auto Root=HistoryFixture(TEXT("NACA0018_ReaderFixture"));const auto Source=StudioRecordings::Import(Root/TEXT("recording.json"),0,{});
    const auto Expected=HistoryJSON(Root/TEXT("expected.json"));
    if(!TestTrue(TEXT("Published original fixture and independent expectations load"),Source.Source.IsValid()&&Expected.IsValid()))return false;
    int32 Checked=0;
    for(const auto& Point:Expected->GetArrayField(TEXT("points")))for(const auto& Scalar:Source.Source->Descriptor().Scalars)
    {
        auto R=HistoryRequest(Source.Source,Scalar.Id);R.Probe.Method=EStudioProbeMethod::OriginalPoint;
        R.Probe.PointId=int64(Point->AsObject()->GetNumberField(TEXT("id")));R.Probe.Field=Scalar.Id;
        std::atomic<int32> Progress{0};const auto H=StudioProbeHistory::Evaluate(R,{},&Progress);
        if(!TestTrue(TEXT("Complete identified original-point history"),H.Status==EStudioProbeHistoryStatus::Complete&&H.Frames.Num()==3&&H.Matches(R)))return false;
        TestEqual(TEXT("Progress counts original frames"),Progress.load(),3);
        for(const auto& E:Expected->GetArrayField(TEXT("samples")))
        {
            const auto O=E->AsObject();if(O->GetStringField(TEXT("field"))!=Scalar.Id||O->GetNumberField(TEXT("point"))!=Point->AsObject()->GetNumberField(TEXT("row")))continue;
            const int32 I=int32(O->GetNumberField(TEXT("frame")));const auto& F=H.Frames[I];const auto& S=F.Samples[0];
            if(!TestTrue(TEXT("Original point has a value and both coordinate systems"),S.Value.IsSet()&&S.SourcePosition.IsSet()&&S.ScenePosition.IsSet()))return false;
            TestEqual(TEXT("Every value matches independent original-HDF5 extraction"),*S.Value,O->GetNumberField(TEXT("value")));
            TestEqual(TEXT("Original source step preserved"),F.Frame.Index,Source.Source->Descriptor().Frames[I].Index);
            TestEqual(TEXT("Sparse original source time preserved"),F.Frame.Time,Source.Source->Descriptor().Frames[I].Time);
            TestEqual(TEXT("Frame ordinal retained separately"),F.Ordinal,I);
            TestEqual(TEXT("Original point ID retained"),*S.PointId,*R.Probe.PointId);
            TestEqual(TEXT("Source units preserved"),H.Unit,Scalar.Unit);++Checked;
        }
        auto Changed=R;Changed.ProjectId=FGuid::NewGuid();TestFalse(TEXT("History cannot publish into another project"),H.Matches(Changed));
        Changed=R;Changed.Probe.A.X+=1;TestFalse(TEXT("Same-ID probe edits invalidate old values"),H.Matches(Changed));
        Changed=R;Changed.FirstOrdinal=1;TestFalse(TEXT("Range changes invalidate the history"),H.Matches(Changed));
    }
    TestEqual(TEXT("All independent source rows, fields and frames checked"),Checked,135);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeHistorySurface,"Studio.ProbeHistory.SurfaceLineAndSpatialGaps",HistoryProbeFlags)
bool FProbeHistorySurface::RunTest(const FString&)
{
    const auto Raw=StudioRecordings::Import(HistoryFixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),0,{});
    if(!TestTrue(*Raw.Error,Raw.Reference.IsSet()))return false;
    const auto Source=StudioRecordings::ImportReconstruction(*Raw.Reference,HistoryFixture(TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),0,{});
    const auto Expected=HistoryJSON(HistoryFixture(TEXT("NACA0018_SurfaceFixture"))/TEXT("expected.json"));
    if(!TestTrue(*Source.Error,Source.Source.IsValid()&&Expected.IsValid()))return false;
    int32 Checked=0;
    for(const auto& Q:Expected->GetArrayField(TEXT("queries")))for(const auto& Scalar:Source.Source->Descriptor().Scalars)
    {
        auto R=HistoryRequest(Source.Source,Scalar.Id);const auto XY=Q->AsObject()->GetArrayField(TEXT("position"));R.Probe.A=FVector(XY[0]->AsNumber(),0,XY[1]->AsNumber());
        const auto H=StudioProbeHistory::Evaluate(R);
        if(!TestTrue(TEXT("Reconstructed interpolation produces a complete history"),H.Status==EStudioProbeHistoryStatus::Complete&&H.Identity.IsSet()))return false;
        TestEqual(TEXT("Reconstruction has separate attribution"),H.Identity->ReconstructionSHA256,Expected->GetStringField(TEXT("reconstructionSHA256")));
        for(const auto& E:Q->AsObject()->GetArrayField(TEXT("expected")))
        {
            const auto V=E->AsObject();if(V->GetStringField(TEXT("field"))!=Scalar.Id)continue;
            const auto& Sample=H.Frames[int32(V->GetNumberField(TEXT("frame")))].Samples[0];
            if(!TestTrue(TEXT("Covered position has an interpolated value"),Sample.Value.IsSet()))return false;
            TestTrue(TEXT("History agrees with independent source interpolation"),FMath::Abs(*Sample.Value-V->GetNumberField(TEXT("value")))<1.e-9);++Checked;
        }
    }
    TestTrue(TEXT("Independent queries cover all fields and frames"),Checked>=45);
    auto R=HistoryRequest(Source.Source);R.Probe.Kind=EStudioProbeKind::Line;R.Probe.Samples=17;
    R.Probe.A=FVector(.13010210394,0,.094347722828);R.Probe.B=FVector(10,0,10);
    const auto Line=StudioProbeHistory::Evaluate(R);
    if(!TestTrue(TEXT("Line history includes every position and frame"),Line.Frames.Num()==3&&Line.Frames[0].Samples.Num()==17))return false;
    for(const auto& F:Line.Frames)
    {
        TestTrue(TEXT("Covered endpoint stays valued"),F.Samples[0].Value.IsSet());
        TestTrue(TEXT("Outside endpoint stays an explicit gap"),!F.Samples.Last().Value.IsSet()&&F.Samples.Last().Status==EStudioProbeSampleStatus::OutsideCoverage);
        TestTrue(TEXT("Exact line distance survives"),FMath::IsNearlyEqual(F.Samples.Last().DistanceAlongLineMeters,(R.Probe.B-R.Probe.A).Size(),1.e-12));
    }
    R.Probe.A.Y=R.Probe.B.Y=.01;const auto OffPlane=StudioProbeHistory::Evaluate(R);
    TestTrue(TEXT("Off-plane gaps never copy a 2D field through span"),OffPlane.Status==EStudioProbeHistoryStatus::Complete&&OffPlane.Frames[0].Samples[0].Status==EStudioProbeSampleStatus::OffPlane&&!OffPlane.Frames[0].Samples[0].Value.IsSet());
    R=HistoryRequest(Raw.Source);const auto NoMesh=StudioProbeHistory::Evaluate(R);
    TestTrue(TEXT("Raw point source never invents interpolation"),NoMesh.Frames.Num()==3&&NoMesh.Frames[0].Samples[0].Status==EStudioProbeSampleStatus::NoInterpolation);
    R.Probe.Method=EStudioProbeMethod::OriginalPoint;R.Probe.PointId=MAX_int64;const auto Missing=StudioProbeHistory::Evaluate(R);
    TestTrue(TEXT("Missing point ID never selects a nearest row"),Missing.Frames.Num()==3&&Missing.Frames[0].Samples[0].Status==EStudioProbeSampleStatus::MissingPoint&&!Missing.Frames[0].Samples[0].Value.IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeHistoryLegacy,"Studio.ProbeHistory.CompletePublishedLegacySequence",HistoryProbeFlags)
bool FProbeHistoryLegacy::RunTest(const FString&)
{
    const auto Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();auto R=HistoryRequest(Source);
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*(HistoryFixture(TEXT("MeshGraphNets_Airfoil"))/TEXT("flow.bin"))));
    if(!TestTrue(TEXT("Published source payload available"),Reader.IsValid()&&Source->FrameCount()==601))return false;
    int32 Magic,Version,Nodes,Triangles,Boundary,Frames;*Reader<<Magic<<Version<<Nodes<<Triangles<<Boundary<<Frames;
    double X,Y;*Reader<<X<<Y;R.Probe.A=FVector(X,0,Y)+Source->Descriptor().SourceOffset;
    const auto H=StudioProbeHistory::Evaluate(R);
    if(!TestTrue(TEXT("All 601 original snapshots retained"),H.Status==EStudioProbeHistoryStatus::Complete&&H.Frames.Num()==601))return false;
    const int64 Start=24LL+16LL*Nodes+12LL*Triangles+4LL*Boundary;
    for(int32 I=0;I<601;++I)
    {
        Reader->Seek(Start+int64(I)*(12LL+16LL*Nodes));int32 Step;double Time;float U,V,P,D;*Reader<<Step<<Time<<U<<V<<P<<D;
        const auto& F=H.Frames[I];
        if(!TestTrue(TEXT("Mesh vertex remains covered for every source frame"),F.Samples[0].Value.IsSet()))return false;
        TestTrue(TEXT("History matches independently decoded original-node pressure"),FMath::IsNearlyEqual(*F.Samples[0].Value,double(P),1.e-6));
        TestEqual(TEXT("Original payload step retained"),F.Frame.Index,Step);TestEqual(TEXT("Original payload time retained"),F.Frame.Time,Time);
    }
    TestFalse(TEXT("Independent payload decoder had no errors"),Reader->IsError());
    const auto Cache=Source->CacheStats();TestTrue(TEXT("Full history does not retain all CFD frames"),Cache.ResidentBytes<=Cache.BudgetBytes&&Cache.ResidentFrames<601);
    TestTrue(TEXT("Analysis does not mutate viewport errors"),Source->LoadError().IsEmpty());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeHistoryVolume,"Studio.ProbeHistory.OriginalThreeDimensionalSequence",HistoryProbeFlags)
bool FProbeHistoryVolume::RunTest(const FString&)
{
    const auto Root=HistoryFixture(TEXT("Cylinder3D_ReaderFixture"));const auto Source=StudioRecordings::Import(Root/TEXT("recording.json"),0,{});
    if(!TestTrue(*Source.Error,Source.Source.IsValid()))return false;
    auto R=HistoryRequest(Source.Source);R.Probe.Method=EStudioProbeMethod::OriginalPoint;
    const auto Meta=HistoryJSON(Root/TEXT("recording.json"));
    // Read the original declared arrays directly, independently of field sampling.
    if(!TestTrue(TEXT("Original 3D descriptor available"),Meta.IsValid()))return false;
    TUniquePtr<FArchive> IDs(IFileManager::Get().CreateFileReader(*(Root/Meta->GetObjectField(TEXT("pointIds"))->GetStringField(TEXT("path")))));
    TUniquePtr<FArchive> Pressure(IFileManager::Get().CreateFileReader(*(Root/TEXT("pressure.f64"))));
    if(!TestTrue(TEXT("Original 3D arrays open"),IDs.IsValid()&&Pressure.IsValid()))return false;
    const int32 Rows=int32(Meta->GetNumberField(TEXT("pointCount")));int64 Id;*IDs<<Id;R.Probe.PointId=Id;
    const auto H=StudioProbeHistory::Evaluate(R);
    if(!TestTrue(TEXT("All original 3D times sampled"),H.Status==EStudioProbeHistoryStatus::Complete&&H.Frames.Num()==3&&H.Identity->SpatialDimensions==3))return false;
    for(int32 I=0;I<3;++I)
    {
        Pressure->Seek(int64(I)*Rows*sizeof(double));double Expected;*Pressure<<Expected;
        TestEqual(TEXT("3D history retains original pressure exactly"),H.Frames[I].Samples[0].Value.GetValue(),Expected);
        const auto& S=H.Frames[I].Samples[0];TestEqual(TEXT("Three source axes retain scene mapping"),*S.ScenePosition,FVector(S.SourcePosition->X,S.SourcePosition->Z,S.SourcePosition->Y));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeHistoryReject,"Studio.ProbeHistory.RejectMismatchesAndPartialReads",HistoryProbeFlags)
bool FProbeHistoryReject::RunTest(const FString&)
{
    const auto Raw=StudioRecordings::Import(HistoryFixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),0,{});
    if(!TestTrue(*Raw.Error,Raw.Source.IsValid()))return false;
    auto Source=MakeShared<FHistoryReadFixture,ESPMode::ThreadSafe>(Raw.Source);auto R=HistoryRequest(Source);
    R.Probe.Method=EStudioProbeMethod::OriginalPoint;R.Probe.PointId=0;Source->ChangeAt=1;
    const TArray<TFunction<void(FStudioFieldIdentity&)>> Changes={
        [](auto& I){++I.Ordinal;},[](auto& I){++I.Frame.Index;},[](auto& I){I.Frame.Time+=1;},
        [](auto& I){I.Dataset=TEXT("wrong");},[](auto& I){I.MetadataSHA256=FString::ChrN(64,'0');},
        [](auto& I){I.ReconstructionSHA256=FString::ChrN(64,'a');},[](auto& I){I.SourceOffset.X+=1;},
        [](auto& I){I.SpatialDimensions=3;},[](auto& I){I.Interpolation=EStudioFieldInterpolation::ReconstructedGrid;}};
    for(const auto& Change:Changes)
    {
        Source->Change=Change;const auto H=StudioProbeHistory::Evaluate(R);
        TestTrue(TEXT("Mismatched second frame discards the entire partial result"),H.Status==EStudioProbeHistoryStatus::IdentityMismatch&&H.Frames.IsEmpty()&&H.FailedOrdinal==1);
    }
    Source->ChangeAt=INDEX_NONE;Source->FailAt=1;auto Failed=StudioProbeHistory::Evaluate(R);
    TestTrue(TEXT("Failed IO never masquerades as a spatial gap or complete prefix"),Failed.Status==EStudioProbeHistoryStatus::ReadFailed&&Failed.Frames.IsEmpty()&&Failed.FailedOrdinal==1);
    Source->FailAt=INDEX_NONE;Source->Meta.Frames[1].Time=Source->Meta.Frames[0].Time;
    TestTrue(TEXT("Non-increasing source times rejected"),StudioProbeHistory::Evaluate(R).Status==EStudioProbeHistoryStatus::IdentityMismatch);
    Source->Meta=Raw.Source->Descriptor();R.Probe.Source.Dataset=TEXT("wrong");TestTrue(TEXT("Foreign probe source rejected"),StudioProbeHistory::Evaluate(R).Status==EStudioProbeHistoryStatus::InvalidRequest);
    R=HistoryRequest(Source);R.Scalar=TEXT("absent");TestTrue(TEXT("Unavailable field never falls back"),StudioProbeHistory::Evaluate(R).Status==EStudioProbeHistoryStatus::InvalidRequest);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeHistoryBounds,"Studio.ProbeHistory.BoundsCancellationAndOwnedWorker",HistoryProbeFlags)
bool FProbeHistoryBounds::RunTest(const FString&)
{
    const auto Raw=StudioRecordings::Import(HistoryFixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),0,{});
    if(!TestTrue(*Raw.Error,Raw.Source.IsValid()))return false;
    auto Source=MakeShared<FHistoryReadFixture,ESPMode::ThreadSafe>(Raw.Source);auto R=HistoryRequest(Source);FString Error;
    R.LastOrdinal=3;TestFalse(TEXT("Past-end range rejected before reading"),StudioProbeHistory::Validate(R,Error));
    R.LastOrdinal=MAX_int32;TestFalse(TEXT("Overflow-sized ordinal rejected"),StudioProbeHistory::Validate(R,Error));
    Source->Meta.Frames.SetNum(StudioProbeHistory::MaxFrames+1);R.LastOrdinal=StudioProbeHistory::MaxFrames;
    TestFalse(TEXT("Frame-count bound explicit"),StudioProbeHistory::Validate(R,Error));
    R.LastOrdinal=1000;R.Probe.Kind=EStudioProbeKind::Line;R.Probe.B=FVector(1,0,0);R.Probe.Samples=1024;
    TestFalse(TEXT("Frame times line-sample budget enforced without decimation"),StudioProbeHistory::Validate(R,Error));
    Source->Meta=Raw.Source->Descriptor();R=HistoryRequest(Source);R.Probe.Method=EStudioProbeMethod::OriginalPoint;R.Probe.PointId=0;
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);Source->OnRead=[Cancel](int32 I){if(I==1)*Cancel=true;};
    const auto Partial=StudioProbeHistory::Evaluate(R,Cancel);TestTrue(TEXT("Cancellation releases partial values"),Partial.Status==EStudioProbeHistoryStatus::Cancelled&&Partial.Frames.IsEmpty());
    Source->OnRead={};FStudioProbeHistoryTask Task;
    if(!TestTrue(TEXT("One explicit worker starts"),Task.Start(R,Error)))return false;
    TestFalse(TEXT("Repeated start cannot queue unbounded work"),Task.Start(R,Error));Task.Cancel();
    const auto Stopped=FinishHistory(Task);if(!TestTrue(TEXT("Cancelled worker drains without publication"),Stopped.IsSet()&&Stopped->Status==EStudioProbeHistoryStatus::Cancelled&&Stopped->Frames.IsEmpty()))return false;
    TestTrue(TEXT("Explicit retry starts after cancellation"),Task.Start(R,Error));const auto Done=FinishHistory(Task);
    TestTrue(TEXT("Retry finishes all original frames"),Done.IsSet()&&Done->Matches(R)&&Task.CompletedFrames()==3&&Task.TotalFrames()==3);
    TestTrue(TEXT("Third request can be joined at shutdown"),Task.Start(R,Error));Task.Shutdown();
    TestFalse(TEXT("No worker remains after shutdown"),Task.IsBusy());TestFalse(TEXT("Shutdown cannot restart"),Task.Start(R,Error));return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeHistoryReadIsolation,"Studio.ProbeHistory.ReaderFailureIsolationAndArrayBudget",HistoryProbeFlags)
bool FProbeHistoryReadIsolation::RunTest(const FString&)
{
    FStudioPointReadOptions Options;Options.CacheBytes=0;Options.LiveArrayBytes=18706LL*sizeof(double);
    const auto Open=StudioPointRecordings::Open(HistoryFixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),Options);
    if(!TestTrue(*Open.Error,Open.Recording.IsValid()))return false;
    const auto Source=MakeShared<FPointRecordedSolver,ESPMode::ThreadSafe>(Open.Recording.ToSharedRef());
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Held=Source->CaptureViewField(0,TEXT("velocity_u"),false);
    auto R=HistoryRequest(Source);R.Probe.Method=EStudioProbeMethod::OriginalPoint;R.Probe.PointId=0;
    const auto Failed=StudioProbeHistory::Evaluate(R);
    TestTrue(TEXT("Pinned array budget fails history explicitly"),Failed.Status==EStudioProbeHistoryStatus::ReadFailed&&Failed.Frames.IsEmpty());
    TestTrue(TEXT("History failure leaves displayed solver usable"),Source->LoadError().IsEmpty()&&Held->IsValid());Held.Reset();
    const auto H=StudioProbeHistory::Evaluate(R);
    TestTrue(TEXT("Sequential history fits one scalar array budget"),H.Status==EStudioProbeHistoryStatus::Complete);
    TestEqual(TEXT("History retains copied samples, no source arrays"),Open.Recording->Stats().LiveArrayBytes,int64(0));
    TestTrue(TEXT("Source's hard live-array limit respected"),Open.Recording->Stats().PeakLiveArrayBytes<=Options.LiveArrayBytes);
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/probe-history-read-isolation");IFileManager::Get().MakeDirectory(*Root,true);
    for(const TCHAR* Name:{TEXT("recording.json"),TEXT("flow.bin")})
        if(!TestEqual(TEXT("Copy original legacy fixture for failure isolation"),IFileManager::Get().Copy(*(Root/Name),*(HistoryFixture(TEXT("MeshGraphNets_Airfoil"))/Name)),COPY_OK))return false;
    const auto Legacy=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>(Root/TEXT("flow.bin"));
    if(!TestTrue(*Legacy->LoadError(),Legacy->LoadError().IsEmpty()))return false;
    const FString Path=Root/TEXT("flow.bin"),HeldPath=Root/TEXT("flow.saved.bin");
    if(!TestTrue(TEXT("Temporarily hide copied source"),IFileManager::Get().Move(*HeldPath,*Path)))return false;
    const auto Read=Legacy->ReadScalarFrame(0,TEXT("pressure"));
    TestTrue(TEXT("Legacy analysis reports its own read failure"),!Read.Field&&!Read.Error.IsEmpty()&&Legacy->LoadError().IsEmpty());
    Legacy->CaptureField(0);const FString ViewError=Legacy->LoadError();
    TestFalse(TEXT("Viewport read still owns its error channel"),ViewError.IsEmpty());
    if(!TestTrue(TEXT("Restore copied source"),IFileManager::Get().Move(*Path,*HeldPath)))return false;
    const auto Recovered=Legacy->ReadScalarFrame(0,TEXT("pressure"));
    TestTrue(TEXT("Analysis retry can recover without clearing a viewport error"),Recovered.Field.IsValid()&&Recovered.Error.IsEmpty()&&Legacy->LoadError()==ViewError);
    IFileManager::Get().DeleteDirectory(*Root,false,true);return true;
}
#endif
