#include "StudioFieldSequence.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioSequenceTests
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FString Fixture(const TCHAR* Name){return FPaths::ProjectContentDir()/TEXT("Samples")/Name/TEXT("recording.json");}
FString Root(){return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/FieldSequence"));}
struct FDirectory
{
    FString Path=Root()/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FDirectory(){IFileManager::Get().MakeDirectory(*Path,true);}
    ~FDirectory(){IFileManager::Get().DeleteDirectory(*Path,false,true);}
};
TArray<FString> Entries(const FString& Path)
{TArray<FString> Out;IFileManager::Get().FindFiles(Out,*(Path/TEXT("*")),true,true);Out.Sort();return Out;}
class FObservedSource final:public IStudioSolver
{
public:
    explicit FObservedSource(TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> In):Source(MoveTemp(In)),Meta(Source->Descriptor()){}
    int32 FrameCount() const override{return Meta.Frames.Num();}
    FStudioFrame EvaluateFrame(int32 N) const override{return Meta.Frames[N];}
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32 N) const override{return Source->CaptureField(N);}
    FStudioFieldReadResult ReadScalarFrame(int32 N,const FString& S,const FStudioLoadCancellation& C) const override
    {
        ReleasedBeforeNext&=!Previous.IsValid();++Reads;
        if(N==FailedOrdinal)return {{},TEXT("Injected read failure of an authentic source")};
        auto R=Source->ReadScalarFrame(N==WrongOrdinal?N+1:N,S,C);Previous=R.Field;return R;
    }
    bool ExportField(int32,const FString&) const override{return false;}
    FString LoadError() const override{return Source->LoadError();}
    const FStudioRecordingDescriptor& Descriptor() const override{return Meta;}
    TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Source;FStudioRecordingDescriptor Meta;
    int32 FailedOrdinal=INDEX_NONE,WrongOrdinal=INDEX_NONE;
    mutable int32 Reads=0;mutable bool ReleasedBeforeNext=true;
    mutable TWeakPtr<const IStudioField,ESPMode::ThreadSafe> Previous;
};
TOptional<FStudioFieldSequenceResult> Await(FStudioFieldSequenceTask& Task)
{
    const double Deadline=FPlatformTime::Seconds()+15.;
    while(FPlatformTime::Seconds()<Deadline)
    {auto R=Task.Poll();if(R)return R;FPlatformProcess::SleepNoStats(.001f);}
    return {};
}
struct FGate
{
    FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
    FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
    ~FGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceValidation,"Studio.FieldSequence.ValidationBeforeReads",StudioSequenceTests::Flags)
bool FSequenceValidation::RunTest(const FString&)
{
    using namespace StudioSequenceTests;
    auto Source=MakeShared<FObservedSource,ESPMode::ThreadSafe>(MakeShared<FRecordedSolver,ESPMode::ThreadSafe>());
    FStudioFieldSequenceRequest R{Source,0,2,{TEXT("pressure")}};FString Error;FDirectory Directory;
    TestTrue(*Error,StudioFieldSequence::Validate(R,Error));
    auto Bad=R;
    for(const auto Range:{FIntPoint(-1,2),FIntPoint(2,1),FIntPoint(0,601),FIntPoint(0,MAX_int32)})
    {Bad=R;Bad.FirstOrdinal=Range.X;Bad.LastOrdinal=Range.Y;TestFalse(TEXT("Invalid inclusive range"),StudioFieldSequence::Validate(Bad,Error));}
    for(const TArray<FString>& Fields:{TArray<FString>{},TArray<FString>{TEXT("pressure"),TEXT("pressure")},TArray<FString>{TEXT("temperature")}})
    {Bad=R;Bad.Scalars=Fields;TestFalse(TEXT("Invalid arrays"),StudioFieldSequence::Validate(Bad,Error));}
    Bad=R;Bad.Coordinates=EStudioExportCoordinates(255);TestFalse(TEXT("Invalid coordinate convention"),StudioFieldSequence::Validate(Bad,Error));
    const auto Original=Source->Meta;
    Source->Meta.Frames[1].Time=Source->Meta.Frames[0].Time;
    auto Result=StudioFieldSequence::Write(R,Directory.Path);
    TestTrue(TEXT("Invalid timestamps reject before bytes or reads"),!Result.bSuccess&&Source->Reads==0&&Entries(Directory.Path).IsEmpty());
    Source->Meta=Original;Source->Meta.PayloadSHA256=TEXT("invalid");
    TestFalse(TEXT("Unverified source"),StudioFieldSequence::Validate(R,Error));Source->Meta=Original;
    auto C=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    Result=StudioFieldSequence::Write(R,Directory.Path,C);
    TestTrue(TEXT("Pre-cancel never reads or writes"),Result.bCancelled&&!Result.bSuccess&&Source->Reads==0&&Entries(Directory.Path).IsEmpty());
    FStudioFieldSequenceTask Task;
    for(const auto* Name:{TEXT("../escape"),TEXT("."),TEXT(".hidden"),TEXT("sub/name"),TEXT("sub\\name"),TEXT("a:b"),TEXT("")})
        TestFalse(TEXT("Destination name is one new visible directory"),Task.Start(R,Directory.Path,Name,Error));
    TestFalse(TEXT("Parent must be absolute"),Task.Start(R,TEXT("relative"),TEXT("export"),Error));
    TestTrue(TEXT("Rejected starts do not create directories"),Entries(Directory.Path).IsEmpty());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceReference,"Studio.FieldSequence.PublishedSequencesAndBoundedOwnership",StudioSequenceTests::Flags)
bool FSequenceReference::RunTest(const FString&)
{
    using namespace StudioSequenceTests;
    const TCHAR* Names[]={TEXT("MeshGraphNets_Airfoil"),TEXT("NACA0018_ReaderFixture"),TEXT("Cylinder3D_ReaderFixture")};
    for(int32 K=0;K<3;++K)
    {
        FStudioRecordingLoadResult Loaded;
        if(K==0)Loaded.Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();
        else Loaded=StudioRecordings::Import(Fixture(Names[K]),0,{});
        if(!TestTrue(*Loaded.Error,Loaded.Source.IsValid()))return false;
        auto Source=MakeShared<FObservedSource,ESPMode::ThreadSafe>(Loaded.Source);
        FStudioFieldSequenceRequest R{Source,K==0?419:0,K==0?421:2,{TEXT("pressure")}};
        if(K==0){R.Scalars.Add(TEXT("density"));R.Coordinates=EStudioExportCoordinates::Scene;}
        if(K==2)R.Scalars.Add(TEXT("velocity_w"));
        const FString Output=Root()/Names[K];IFileManager::Get().DeleteDirectory(*Output,false,true);IFileManager::Get().MakeDirectory(*Output,true);
        int32 Frames=0;int64 Done=0;bool Monotonic=true;
        const auto Result=StudioFieldSequence::Write(R,Output,{},[&](int32 F,int64 D,int64 T)
        {Monotonic&=F>=Frames&&F<=3&&D>=0&&D<=T&&(F!=Frames||D>=Done);Frames=F;Done=D;});
        TestTrue(*Result.Error,Result.bSuccess);TestEqual(TEXT("All inclusive source frames exported"),Result.CompletedFrames,3);
        TestTrue(TEXT("Frame resources released before the next read and completion"),Source->ReleasedBeforeNext&&!Source->Previous.IsValid());
        TestTrue(TEXT("One read per source frame; source status unaffected"),Source->Reads==3&&Source->LoadError().IsEmpty());
        TestTrue(TEXT("Original endpoint identities"),Result.FirstIdentity&&Result.LastIdentity&&Result.FirstIdentity->Ordinal==R.FirstOrdinal&&Result.LastIdentity->Ordinal==R.LastOrdinal);
        TestTrue(TEXT("Monotonic progress reaches the last frame"),Monotonic&&Frames==3&&Done==0);
        TestEqual(TEXT("Collection plus three frames"),Entries(Output).Num(),4);
        const auto Stats=Loaded.Source->CacheStats();TestTrue(TEXT("Reader cache remains within budget"),Stats.ResidentBytes<=Stats.BudgetBytes);
    }
    AddInfo(TEXT("Independent sequence audit directory: ")+Root());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFailures,"Studio.FieldSequence.LateFailureIdentityAndCancellation",StudioSequenceTests::Flags)
bool FSequenceFailures::RunTest(const FString&)
{
    using namespace StudioSequenceTests;
    auto Source=MakeShared<FObservedSource,ESPMode::ThreadSafe>(MakeShared<FRecordedSolver,ESPMode::ThreadSafe>());
    FStudioFieldSequenceRequest R{Source,419,421,{TEXT("pressure")}};
    for(int32 Fault=0;Fault<3;++Fault)
    {
        FDirectory D;Source->Reads=0;Source->FailedOrdinal=Fault==0?420:INDEX_NONE;Source->WrongOrdinal=Fault==1?420:INDEX_NONE;
        const auto Original=Source->Meta;if(Fault==2)Source->Meta.Scalars.FindByPredicate([](const auto& S){return S.Id==TEXT("pressure");})->Unit=TEXT("changed");
        const auto Result=StudioFieldSequence::Write(R,D.Path);
        TestTrue(TEXT("Read or scientific identity failure prevents completion"),!Result.bSuccess&&!Result.bCancelled&&!Result.Error.IsEmpty());
        TestEqual(TEXT("Exact failure ordinal"),Result.FailedOrdinal,Fault==2?419:420);
        TestEqual(TEXT("Completed count stops at failure"),Result.CompletedFrames,Fault==2?0:1);
        FString XML;FFileHelper::LoadFileToString(XML,*(D.Path/TEXT("flow.pvd")));
        TestFalse(TEXT("Partial collection never has success terminator"),XML.EndsWith(TEXT("</VTKFile>\n")));Source->Meta=Original;
    }
    Source->FailedOrdinal=Source->WrongOrdinal=INDEX_NONE;Source->Reads=0;
    FDirectory D;auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const auto Result=StudioFieldSequence::Write(R,D.Path,Cancel,[&](int32 Frames,int64,int64){if(Frames==1)Cancel->store(true);});
    TestTrue(TEXT("Cancellation between frames prevents the next read"),Result.bCancelled&&!Result.bSuccess&&Result.CompletedFrames==1&&Source->Reads==1);
    TestTrue(TEXT("Independent errors and cancellation leave viewport status clear"),Source->LoadError().IsEmpty());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencePublishing,"Studio.FieldSequence.ExclusivePublicationCancellationAndShutdown",StudioSequenceTests::Flags)
bool FSequencePublishing::RunTest(const FString&)
{
    using namespace StudioSequenceTests;
    FDirectory D;FString Error;
    auto Source=MakeShared<FObservedSource,ESPMode::ThreadSafe>(MakeShared<FRecordedSolver,ESPMode::ThreadSafe>());
    FStudioFieldSequenceRequest R{Source,419,421,{TEXT("pressure")}};FStudioFieldSequenceTask Task;
    auto Gate=MakeShared<FGate,ESPMode::ThreadSafe>();
    Task.BeforePublishForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(10000);};
    TestTrue(*Error,Task.Start(R,D.Path,TEXT("cancelled"),Error));
    TestTrue(TEXT("All frames staged before publication"),Gate->Reached->Wait(10000));
    const auto P=Task.Progress();TestTrue(TEXT("Progress reaches total before publication"),P.CompletedFrames==3&&P.TotalFrames==3&&P.State==EStudioFieldExportState::Writing);
    TestFalse(TEXT("No queued sequence"),Task.Start(R,D.Path,TEXT("second"),Error));
    TestTrue(TEXT("Cancel wins the publication boundary"),Task.Cancel());TestFalse(TEXT("Cancel accepted once"),Task.Cancel());
    Gate->Release->Trigger();auto Result=Await(Task);
    TestTrue(TEXT("Cancellation removes all owned staging and never publishes"),Result&&Result->bCancelled&&!Result->bSuccess&&Entries(D.Path).IsEmpty());
    if(Task.IsBusy())return false;
    Source->FailedOrdinal=420;TestTrue(*Error,Task.Start(R,D.Path,TEXT("failure"),Error));Result=Await(Task);
    TestTrue(TEXT("Late read failure discards earlier frames and collection"),Result&&!Result->bSuccess&&Result->FailedOrdinal==420&&Entries(D.Path).IsEmpty());Source->FailedOrdinal=INDEX_NONE;
    if(Task.IsBusy())return false;
    Gate=MakeShared<FGate,ESPMode::ThreadSafe>();Task.BeforePublishForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(10000);};
    TestTrue(*Error,Task.Start(R,D.Path,TEXT("occupied"),Error));TestTrue(TEXT("Reach raced destination"),Gate->Reached->Wait(10000));
    IFileManager::Get().MakeDirectory(*(D.Path/TEXT("occupied")),true);
    FFileHelper::SaveStringToFile(TEXT("preserve"),*(D.Path/TEXT("occupied/user.txt")));
    Gate->Release->Trigger();Result=Await(Task);FString Text;
    FFileHelper::LoadFileToString(Text,*(D.Path/TEXT("occupied/user.txt")));
    TestTrue(TEXT("Destination created during work remains untouched"),Result&&!Result->bSuccess&&!Result->bCancelled&&Text==TEXT("preserve")&&Entries(D.Path).Num()==1);
    if(Task.IsBusy())return false;
    FFileHelper::SaveStringToFile(TEXT("file"),*(D.Path/TEXT("occupied-file")));
    const int32 ReadsBeforeExisting=Source->Reads;
    TestTrue(*Error,Task.Start(R,D.Path,TEXT("occupied-file"),Error));Result=Await(Task);
    FFileHelper::LoadFileToString(Text,*(D.Path/TEXT("occupied-file")));
    TestTrue(TEXT("Existing regular file rejected before expensive reads"),Result&&!Result->bSuccess&&Text==TEXT("file")&&Entries(D.Path).Num()==2&&Source->Reads==ReadsBeforeExisting);
    if(Task.IsBusy())return false;
    TestTrue(*Error,Task.Start(R,D.Path,TEXT("complete"),Error));Result=Await(Task);
    TestTrue(TEXT("Retry publishes complete relative collection"),Result&&Result->bSuccess&&Result->CompletedFrames==3&&Entries(D.Path/TEXT("complete")).Num()==4&&Entries(D.Path).Num()==3);
    TestFalse(TEXT("Published folder cannot report cancelled"),Task.Cancel());
    if(Task.IsBusy())return false;
    TestTrue(*Error,Task.Start(R,D.Path/TEXT("missing"),TEXT("export"),Error));Result=Await(Task);
    TestTrue(TEXT("Missing parent gives useful error without creating it"),Result&&!Result->bSuccess&&!Result->Error.IsEmpty()&&Entries(D.Path).Num()==3);
    FStudioFieldSequenceTask Closing;TSharedPtr<FObservedSource,ESPMode::ThreadSafe> Owned=MakeShared<FObservedSource,ESPMode::ThreadSafe>(Source->Source);
    TWeakPtr<const IStudioSolver,ESPMode::ThreadSafe> Weak=Owned;R.Source=Owned;Owned.Reset();
    TestTrue(*Error,Closing.Start(MoveTemp(R),D.Path,TEXT("shutdown"),Error));Closing.Shutdown();Closing.Shutdown();
    TestTrue(TEXT("Shutdown joins worker and releases source"),!Closing.IsBusy()&&!Weak.IsValid()&&!Closing.Poll());
    TestFalse(TEXT("Shutdown rejects new work"),Closing.Start({Source,0,0,{TEXT("pressure")}},D.Path,TEXT("after"),Error));
    TestTrue(TEXT("Shutdown has no hidden staging directory"),!Entries(D.Path).ContainsByPredicate([](const auto& Name){return Name.StartsWith(TEXT(".lbm-export-"));}));
    return true;
}
#endif
