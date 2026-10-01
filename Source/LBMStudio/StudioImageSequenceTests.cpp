#include "StudioImageSequence.h"
#include "StudioImageSequenceRenderer.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioImageSequenceTests
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FString Root(){return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/ImageSequenceEncoder"));}
struct FDirectory
{
    FString Path=Root()/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    bool bRetain=false;
    FDirectory(){IFileManager::Get().MakeDirectory(*Path,true);}
    ~FDirectory(){if(!bRetain)IFileManager::Get().DeleteDirectory(*Path,false,true);}
};
TArray<FString> Entries(const FString& Path)
{TArray<FString> Out;IFileManager::Get().FindFiles(Out,*(Path/TEXT("*")),true,true);Out.Sort();return Out;}
FStudioImageSequenceRequest Request()
{
    FStudioImageSequenceRequest R;R.Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();
    R.FirstOrdinal=3;R.LastOrdinal=10;R.Stride=3;
    auto& V=R.View;V.Project=FGuid(1,2,3,4);V.SourceTitle=R.Source->Descriptor().Title;
    const auto Field=R.Source->ReadScalarFrame(0,TEXT("pressure"));
    if(!Field.Field||!Field.Field->Identity()||!Field.Field->Scalar(TEXT("pressure"))){R.Source.Reset();return R;}
    V.Identity=*Field.Field->Identity();V.Scalar=*Field.Field->Scalar(TEXT("pressure"));
    V.DisplaySettings.ScalarField=V.Scalar.Id;
    V.Mapping=StudioColor::Resolve(V.Identity.Dataset,V.Scalar,{});
    V.Camera.Position=FVector(.125,2.5,3.75);V.Options.Size=FIntPoint(64,64);V.SourceSize=FIntPoint(1600,900);
    V.Framing=StudioSnapshot::Frame(V.SourceSize,V.Options.Size);return R;
}
FStudioSnapshot Image(const FStudioImageSequenceRequest& R,int32 N)
{
    auto S=R.View;S.Identity.Ordinal=N;S.Identity.Frame=R.Source->Descriptor().Frames[N];S.Capture=N+1;
    // Test-only encoder pattern, NOT a rendered CFD image. Authentic source
    // identity exercises the writer contract; GPU fidelity is a separate gate.
    S.Pixels.Init(FColor(uint8(17+N),101,213,0),S.Options.Size.X*S.Options.Size.Y);return S;
}
template<typename Predicate> bool Wait(Predicate Ready)
{
    const double Deadline=FPlatformTime::Seconds()+10.;
    do {if(Ready())return true;FPlatformProcess::SleepNoStats(.001f);}while(FPlatformTime::Seconds()<Deadline);
    return false;
}
TOptional<int32> Take(FStudioImageSequenceTask& Task)
{TOptional<int32> N;Wait([&]{N=Task.TakeFrameRequest();return N.IsSet()||Task.Progress().Phase==EStudioImageSequencePhase::Complete;});return N;}
TOptional<FStudioImageSequenceResult> Await(FStudioImageSequenceTask& Task)
{TOptional<FStudioImageSequenceResult> R;Wait([&]{R=Task.Poll();return R.IsSet();});return R;}
struct FGate
{
    FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
    FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
    ~FGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageSequenceValidation,"Studio.ImageSequence.SelectionAndFrozenViewValidation",StudioImageSequenceTests::Flags)
bool FImageSequenceValidation::RunTest(const FString&)
{
    using namespace StudioImageSequenceTests;
    const auto R=Request();FString Error;
    if(!TestTrue(*Error,StudioImageSequence::Validate(R,Error)))return false;
    for(const auto Range:{FIntVector(-1,2,1),FIntVector(2,1,1),FIntVector(0,601,1),FIntVector(0,1,0),FIntVector(0,1,-1)})
    {
        auto Bad=R;Bad.FirstOrdinal=Range.X;Bad.LastOrdinal=Range.Y;Bad.Stride=Range.Z;
        TestFalse(TEXT("Invalid original selection rejected"),StudioImageSequence::Validate(Bad,Error));
    }
    auto Bad=R;Bad.View.Pixels.Add(FColor::Black);TestFalse(TEXT("Anchor must not retain an extra pixel buffer"),StudioImageSequence::Validate(Bad,Error));
    Bad=R;Bad.View.Identity.PayloadSHA256=TEXT("wrong");TestFalse(TEXT("Anchor source hash checked"),StudioImageSequence::Validate(Bad,Error));
    Bad=R;Bad.View.Framing.Minimum.X=.5;TestFalse(TEXT("Anchor crop checked"),StudioImageSequence::Validate(Bad,Error));
    Bad=R;Bad.View.Options.Size=FIntPoint(8192,8192);TestFalse(TEXT("Image dimensions bounded"),StudioImageSequence::Validate(Bad,Error));
    Bad=R;Bad.View.Scalar.Unit=TEXT("wrong");TestFalse(TEXT("Scalar meaning checked"),StudioImageSequence::Validate(Bad,Error));
    Bad=R;Bad.View.Camera.FieldOfView=0;TestFalse(TEXT("Camera validated"),StudioImageSequence::Validate(Bad,Error));
    auto S=Image(R,3);TestTrue(TEXT("Exact frozen image accepted"),StudioImageSequence::Matches(R,3,S,Error));
    S.Camera.Orientation=(FQuat(FVector::UpVector,4.e-11)*S.Camera.Orientation).GetNormalized();
    TestTrue(TEXT("Component rotation round-off accepted"),StudioImageSequence::Matches(R,3,S,Error));
    for(int32 Kind=0;Kind<10;++Kind)
    {
        S=Image(R,3);
        if(Kind==0)S.Identity.Frame.Time+=.001;
        if(Kind==1)S.Camera.Position.X+=.001;
        if(Kind==2)S.DisplaySettings.bVectors=!S.DisplaySettings.bVectors;
        if(Kind==3)S.Mapping.Maximum+=1;
        if(Kind==4)S.Options.bLegend=!S.Options.bLegend;
        if(Kind==5)S.Pixels.Pop();
        if(Kind==6)S.Identity.ReconstructionSHA256=TEXT("wrong");
        if(Kind==7)S.Project=FGuid::NewGuid();
        if(Kind==8)S.Projection.M[0][0]+=.001;
        if(Kind==9)S.Camera.Orientation=(FQuat(FVector::UpVector,1.e-7)*S.Camera.Orientation).GetNormalized();
        TestFalse(TEXT("Frame and view drift refused"),StudioImageSequence::Matches(R,3,S,Error));
    }
    S=Image(R,4);TestFalse(TEXT("Unselected original frame refused"),StudioImageSequence::Matches(R,4,S,Error));
    FDirectory D;FStudioImageSequenceTask Task;
    for(const auto* Name:{TEXT("../escape"),TEXT(".hidden"),TEXT("sub/name"),TEXT("sub\\name"),TEXT("a:b"),TEXT(" name"),TEXT("name "),TEXT("")})
        TestFalse(TEXT("New folder name validation"),Task.Start(R,D.Path,Name,Error));
    TestTrue(TEXT("Invalid starts create no staging"),Entries(D.Path).IsEmpty());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageSequenceWrite,"Studio.ImageSequence.PNGIndexAndBoundedHandoff",StudioImageSequenceTests::Flags)
bool FImageSequenceWrite::RunTest(const FString&)
{
    using namespace StudioImageSequenceTests;
    auto R=Request();const auto Frozen=R;FString Error;FDirectory D;FStudioImageSequenceTask Task;
    const FString Name=TEXT("sequence");
    if(!TestTrue(TEXT("Start bounded writer"),Task.Start(R,D.Path,Name,Error)))return false;
    TestFalse(TEXT("No second sequence while busy"),Task.Start(R,D.Path,TEXT("other"),Error));
    R.View.Camera.Position.X=999;R.FirstOrdinal=0;R.Stride=1;
    for(int32 N=Frozen.FirstOrdinal;N<=Frozen.LastOrdinal;N+=Frozen.Stride)
    {
        const auto Next=Take(Task);if(!TestTrue(TEXT("Only the selected original ordinal requested"),Next&&*Next==N))return false;
        TestFalse(TEXT("Handoff is consumed once"),Task.TakeFrameRequest().IsSet());
        TestFalse(TEXT("No partial folder published"),IFileManager::Get().DirectoryExists(*(D.Path/Name)));
        auto S=Image(Frozen,N);TestTrue(TEXT("Submit one image"),Task.Submit(MoveTemp(S),Error));
        TestTrue(TEXT("Pixel buffer moved into bounded handoff"),S.Pixels.IsEmpty());
        auto Duplicate=Image(Frozen,N);TestFalse(TEXT("No image queue"),Task.Submit(MoveTemp(Duplicate),Error));
    }
    const auto Result=Await(Task);
    if(!TestTrue(TEXT("All selected images publish together"),Result&&Result->bSuccess&&!Result->bCancelled&&Result->CompletedFrames==3&&Result->FailedOrdinal==INDEX_NONE))return false;
    const auto Files=Entries(Result->Path);
    TestTrue(TEXT("No frame interpolation or extra files"),Files==TArray<FString>{TEXT("frame_000003.png"),TEXT("frame_000006.png"),TEXT("frame_000009.png"),TEXT("frames.jsonl"),TEXT("sequence.json")});
    FString Manifest;FFileHelper::LoadFileToString(Manifest,*(Result->Path/TEXT("sequence.json")));
    TSharedPtr<FJsonObject> J;TestTrue(TEXT("Manifest JSON readable"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manifest),J));
    if(!J)return false;
    TestEqual(TEXT("Frozen camera independent from changed draft"),J->GetObjectField(TEXT("view"))->GetObjectField(TEXT("camera"))->GetArrayField(TEXT("position_meters"))[0]->AsNumber(),.125);
    TestEqual(TEXT("Requested range endpoint retained even when stride skips it"),J->GetNumberField(TEXT("last_ordinal_inclusive")),10.);
    FString Index;FFileHelper::LoadFileToString(Index,*(Result->Path/TEXT("frames.jsonl")));TArray<FString> Lines;Index.ParseIntoArrayLines(Lines);
    if(!TestEqual(TEXT("Index has one row per selected original"),Lines.Num(),3))return false;
    auto& Module=FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    for(int32 K=0;K<3;++K)
    {
        const int32 N=3+3*K;TSharedPtr<FJsonObject> Entry;
        if(!TestTrue(TEXT("Index row parses"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Lines[K]),Entry)))return false;
        TestEqual(TEXT("Original source step"),Entry->GetNumberField(TEXT("source_step")),double(Frozen.Source->Descriptor().Frames[N].Index));
        TestEqual(TEXT("Original physical time"),Entry->GetNumberField(TEXT("source_time_seconds")),Frozen.Source->Descriptor().Frames[N].Time);
        TArray64<uint8> PNG;FFileHelper::LoadFileToArray(PNG,*(Result->Path/Entry->GetStringField(TEXT("file"))));
        const auto Reader=Module.CreateImageWrapper(EImageFormat::PNG);TArray64<uint8> Raw;
        if(!TestTrue(TEXT("Each PNG independently decodes"),Reader->SetCompressed(PNG.GetData(),PNG.Num())&&Reader->GetRaw(ERGBFormat::RGBA,8,Raw)))return false;
        TestTrue(TEXT("Encoder pattern exact; no GPU fidelity claim"),Reader->GetWidth()==64&&Reader->GetHeight()==64&&Raw.Num()==64*64*4&&Raw[0]==17+N&&Raw[1]==101&&Raw[2]==213&&Raw[3]==255);
    }
    TestFalse(TEXT("Completed publication cannot cancel"),Task.Cancel());
    // Persist only explicitly named encoder artifacts for independent readback.
    D.bRetain=true;
    TestTrue(TEXT("Retain writer-only test artifact location"),FFileHelper::SaveStringToFile(Result->Path,*(Root()/TEXT("latest-pattern-sequence.txt")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageSequenceFailure,"Studio.ImageSequence.FailuresDiscardPartialStaging",StudioImageSequenceTests::Flags)
bool FImageSequenceFailure::RunTest(const FString&)
{
    using namespace StudioImageSequenceTests;
    const auto R=Request();FDirectory D;FString Error;FStudioImageSequenceTask Task;
    for(int32 Kind=0;Kind<3;++Kind)
    {
        TestTrue(TEXT("Start failure scenario"),Task.Start(R,D.Path,TEXT("failed"),Error));
        auto N=Take(Task);if(!N)return false;
        TestTrue(TEXT("Stage first valid frame"),Task.Submit(Image(R,*N),Error));
        N=Take(Task);if(!N)return false;
        if(Kind==0)TestTrue(TEXT("Renderer failure handed back"),Task.FailFrame(TEXT("Original frame read failed.")));
        if(Kind==1){auto Wrong=Image(R,*N);Wrong.Camera.Position.X+=1;Task.Submit(MoveTemp(Wrong),Error);}
        if(Kind==2){auto Wrong=Image(R,*N);Wrong.Identity.Frame.Time+=1;Task.Submit(MoveTemp(Wrong),Error);}
        auto Result=Await(Task);
        TestTrue(TEXT("Partial failure has exact ordinal and recovery"),Result&&!Result->bSuccess&&!Result->bCancelled&&Result->CompletedFrames==1&&Result->FailedOrdinal==6&&!Result->Error.IsEmpty());
        TestTrue(TEXT("Failed images and index are removed"),Entries(D.Path).IsEmpty());
    }
    IFileManager::Get().MakeDirectory(*(D.Path/TEXT("existing")),true);
    const FString Sentinel=D.Path/TEXT("existing/keep.txt");FFileHelper::SaveStringToFile(TEXT("keep"),*Sentinel);
    Task.Start(R,D.Path,TEXT("existing"),Error);auto Result=Await(Task);
    TestTrue(TEXT("Existing directory refused before requesting pixels"),Result&&!Result->bSuccess&&Result->CompletedFrames==0);
    FString Kept;TestTrue(TEXT("Existing file unchanged"),FFileHelper::LoadFileToString(Kept,*Sentinel)&&Kept==TEXT("keep"));
    Task.Start(R,Sentinel,TEXT("blocked"),Error);Result=Await(Task);
    TestTrue(TEXT("Disk path error returned without hanging producer"),Result&&!Result->bSuccess&&!Result->Error.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageSequenceLifecycle,"Studio.ImageSequence.CancellationPublicationRaceAndRelease",StudioImageSequenceTests::Flags)
bool FImageSequenceLifecycle::RunTest(const FString&)
{
    using namespace StudioImageSequenceTests;
    auto R=Request();FDirectory D;FString Error;FStudioImageSequenceTask Task;
    TestTrue(TEXT("Start cancellable handoff"),Task.Start(R,D.Path,TEXT("cancelled"),Error));
    if(!Take(Task))return false;
    TestTrue(TEXT("Cancel wakes waiting worker"),Task.Cancel());TestFalse(TEXT("Cancellation accepted once"),Task.Cancel());
    auto Result=Await(Task);TestTrue(TEXT("Waiting cancellation drains without producer"),Result&&Result->bCancelled&&!Result->bSuccess);
    TestTrue(TEXT("Waiting cancellation removes staging"),Entries(D.Path).IsEmpty());
    for(bool Collision:{false,true})
    {
        auto Gate=MakeShared<FGate,ESPMode::ThreadSafe>();Task.BeforePublishForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(10000);};
        TestTrue(TEXT("Start gated publication"),Task.Start(R,D.Path,TEXT("final"),Error));
        for(int32 N=R.FirstOrdinal;N<=R.LastOrdinal;N+=R.Stride){if(!Take(Task))return false;Task.Submit(Image(R,N),Error);}
        if(!TestTrue(TEXT("Worker reaches publication boundary"),Gate->Reached->Wait(5000)))return false;
        if(Collision)IFileManager::Get().MakeDirectory(*(D.Path/TEXT("final")),true);
        else TestTrue(TEXT("Cancel wins before publication"),Task.Cancel());
        Gate->Release->Trigger();Result=Await(Task);
        TestTrue(TEXT("No overwrite at publication race"),Result&&!Result->bSuccess&&(Collision?!Result->bCancelled:Result->bCancelled));
        TestTrue(TEXT("Only externally created destination can remain"),Entries(D.Path)==(Collision?TArray<FString>{TEXT("final")}:TArray<FString>{}));
    }
    TWeakPtr<const IStudioSolver,ESPMode::ThreadSafe> Weak=R.Source;
    {
        FStudioImageSequenceTask Waiting;Waiting.Start(R,D.Path,TEXT("shutdown"),Error);if(!Take(Waiting))return false;
        R.Source.Reset();TestTrue(TEXT("Pending task pins source"),Weak.IsValid());
        // Destructor must not depend on UI delivery of the requested image.
    }
    TestFalse(TEXT("Shutdown drains and releases the source"),Weak.IsValid());
    TestFalse(TEXT("Shutdown publishes no directory"),IFileManager::Get().DirectoryExists(*(D.Path/TEXT("shutdown"))));return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageSequencePreparation,"Studio.ImageSequence.IndependentVelocityAndProbePreparation",StudioImageSequenceTests::Flags)
bool FImageSequencePreparation::RunTest(const FString&)
{
    using namespace StudioImageSequenceTests;
    const auto Load=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Load.Error,Load.Source.IsValid()))return false;
    const auto& D=Load.Source->Descriptor();
    // Seed the live error deliberately; successful and failed independent reads
    // must not replace it. This source is isolated from the application model.
    Load.Source->CaptureViewField(-1,TEXT("pressure"),true);
    const FString LiveError=Load.Source->LoadError();if(!TestFalse(TEXT("Fixture live error established"),LiveError.IsEmpty()))return false;
    FString Error;const auto Adapter=FStudioSnapshotSource::CreateView(*Load.Source,1,TEXT("pressure"),true,{},Error);
    if(!TestTrue(*Error,Adapter.IsValid()))return false;
    const auto Field=Adapter->CaptureViewField(1,TEXT("pressure"),true);
    const auto Points=Field->OriginalPoints();if(!TestTrue(TEXT("Prepared immutable velocity frame"),Points.IsValid()))return false;
    int32 Components=0;
    for(const auto& F:Points->Descriptor->Fields)if(F.Vector==TEXT("velocity"))
    {++Components;TestTrue(TEXT("Original velocity array retained"),Points->FindValues(F.Id)!=nullptr);}
    TestEqual(TEXT("Original 2D velocity has two supplied components"),Components,2);
    TestFalse(TEXT("Adapter cannot read another frame"),Adapter->CaptureViewField(0,TEXT("pressure"),true)->IsValid());
    TestFalse(TEXT("No fallback to a different scalar"),FStudioSnapshotSource::CreateView(*Load.Source,1,TEXT("missing"),true,{},Error).IsValid());
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Independent preparation respects cancellation"),FStudioSnapshotSource::CreateView(*Load.Source,1,TEXT("pressure"),true,Cancel,Error).IsValid());
    TestEqual(TEXT("Independent reads preserve live error"),Load.Source->LoadError(),LiveError);
    FStudioImageSequenceRequest R;R.Source=Load.Source;R.FirstOrdinal=0;R.LastOrdinal=2;
    R.View.Project=FGuid::NewGuid();R.View.Scalar=*Field->Scalar(TEXT("pressure"));R.View.DisplaySettings.bVectors=true;
    FStudioProbeObject Probe;Probe.Name=TEXT("Original exported point");Probe.Source={D.Id,D.MetadataSHA256,D.PayloadSHA256};
    Probe.Method=EStudioProbeMethod::OriginalPoint;Probe.PointId=Points->Geometry->PointIds[17];
    R.View.Objects.Probes.Add(Probe);R.View.SelectedObject=Probe.Id;
    const auto Prepared=StudioImageSequence::Prepare(R,1,{});
    TestTrue(*Prepared.Error,Prepared.Snapshot.IsValid());
    const auto Position=Prepared.Markers.Position(R.View.Project,Probe);const auto Original=Points->Geometry->Positions[17];
    TestTrue(TEXT("Annotation uses exact original ID coordinates"),Position&&*Position==FVector(Original.X,Original.Z,Original.Y));
    TestTrue(TEXT("Selected probe sampled from this original frame"),Prepared.Probe&&Prepared.Probe->Samples.Num()==1&&
        Prepared.Probe->Samples[0].Value&&*Prepared.Probe->Samples[0].Value==(*Points->FindValues(TEXT("pressure")))[17]);
    TestEqual(TEXT("Probe preparation preserves live error"),Load.Source->LoadError(),LiveError);return true;
}
#endif
