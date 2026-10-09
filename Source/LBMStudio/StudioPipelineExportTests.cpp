#include "StudioPipelineExport.h"
#include "StudioFieldExportTask.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioPipelineExportTestsPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString Root(){return FPaths::ProjectSavedDir()/TEXT("Automation/PipelineExport");}
FStudioPipelineOperation Op(EStudioPipelineOperation Kind,const TCHAR* Name)
{FStudioPipelineOperation O;O.Kind=Kind;O.Name=Name;return O;}
FStudioPipelinePrepareRequest Fixture(bool Volume=false,bool Reconstruction=true,bool Installed=false)
{
    FStudioPipelinePrepareRequest R;R.ProjectId=FGuid::NewGuid();R.Revision=17;
    const FString Samples=FPaths::ProjectContentDir()/TEXT("Samples");
    if(Installed)R.Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();
    else
    {
        auto Source=StudioRecordings::Import(Samples/(Volume?TEXT("Cylinder3D_ReaderFixture"):TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),1,{});
        if(!Source.Source||!Source.Reference)return R;
        if(Reconstruction)Source=StudioRecordings::ImportReconstruction(*Source.Reference,Samples/(Volume?TEXT("Cylinder3D_VolumeFixture"):TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),1,{});
        R.Source=Source.Source;R.Recipe.Source.Reference=Source.Reference;
    }
    if(!R.Source)return R;const auto Field=R.Source->ReadScalarFrame(1,TEXT("pressure"));if(!Field.Field)return R;
    R.Recipe.Name=TEXT("Wing \"analysis\" · α");R.Recipe.Source.Title=R.Source->Descriptor().Title;R.Recipe.Source.Identity=*Field.Field->Identity();
    auto Select=Op(EStudioPipelineOperation::Field,TEXT("Pressure"));Select.Field=TEXT("pressure");Select.Unit=TEXT("Pa");R.Recipe.Operations={Select};return R;
}
bool SaveCase(const FString& Name,const FStudioPipelineEvaluationResult& E,FAutomationTestBase& Test)
{
    if(!Test.TestTrue(*E.Error,E.Output.IsValid()))return false;
    IFileManager::Get().MakeDirectory(*Root(),true);const auto& O=*E.Output;
    // Direct binary copies are serialization oracles, never new flow fields.
    auto Save=[&](const TCHAR* Suffix,const void* Data,int64 Bytes)
    {return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(static_cast<const uint8*>(Data),Bytes),*(Root()/(Name+Suffix)));};
    TArray<double> Vertices;TArray<int64> IDs;
    for(const auto& V:O.Vertices){Vertices.Append({V.PositionMeters.X,V.PositionMeters.Y,V.PositionMeters.Z,V.Scalar});IDs.Append({V.OriginalRow,V.OriginalPointId});}
    if(!Test.TestTrue(TEXT("Retain exact evaluated geometry for independent reader"),Save(TEXT("-vertices.f64"),Vertices.GetData(),Vertices.Num()*8LL)&&
        Save(TEXT("-identity.i64"),IDs.GetData(),IDs.Num()*8LL)&&Save(TEXT("-triangles.i32"),O.Triangles.GetData(),O.Triangles.Num()*12LL)&&Save(TEXT("-lines.i32"),O.Lines.GetData(),O.Lines.Num()*8LL)))return false;
    auto Truth=MakeShared<FJsonObject>();Truth->SetArrayField(TEXT("recipes"),StudioPipelines::ToJSON({E.Prepared.Recipe}));
    Truth->SetStringField(TEXT("field"),E.Prepared.Field->SelectedScalar().Id);Truth->SetStringField(TEXT("method"),O.Method);Truth->SetNumberField(TEXT("kind"),int32(O.Kind));
    TArray<TSharedPtr<FJsonValue>> Rows;
    if(O.Probe)for(const auto& V:O.Probe->Samples)
    {
        auto J=MakeShared<FJsonObject>();if(V.ScenePosition){TArray<TSharedPtr<FJsonValue>> XYZ;for(int32 K=0;K<3;++K)XYZ.Add(MakeShared<FJsonValueNumber>((*V.ScenePosition)[K]));J->SetArrayField(TEXT("position"),XYZ);}
        if(V.Value)J->SetNumberField(TEXT("value"),*V.Value);if(V.PointId)J->SetStringField(TEXT("point_id"),LexToString(*V.PointId));
        J->SetNumberField(TEXT("distance"),V.DistanceAlongLineMeters);J->SetNumberField(TEXT("status"),int32(V.Status));Rows.Add(MakeShared<FJsonValueObject>(J));
    }
    Truth->SetArrayField(TEXT("probe"),Rows);FString JSON;FJsonSerializer::Serialize(Truth,TJsonWriterFactory<>::Create(&JSON));
    if(!Test.TestTrue(TEXT("Retain evaluated recipe/table oracle"),FFileHelper::SaveStringToFile(JSON,*(Root()/(Name+TEXT(".json"))),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))return false;
    for(const auto Coordinates:{EStudioExportCoordinates::Source,EStudioExportCoordinates::Scene})for(const auto Format:{EStudioFieldExportFormat::VTK,EStudioFieldExportFormat::CSV})
    {
        if(O.Probe&&Format==EStudioFieldExportFormat::VTK)continue;
        const FString Path=Root()/(Name+(Coordinates==EStudioExportCoordinates::Source?TEXT("-source"):TEXT("-scene"))+(Format==EStudioFieldExportFormat::VTK?TEXT(".vtp"):TEXT(".csv")));
        TUniquePtr<FArchive> A(IFileManager::Get().CreateFileWriter(*Path));if(!Test.TestTrue(TEXT("Open export archive"),A.IsValid()))return false;
        int64 Done=0,Total=0;bool Monotonic=true;
        const auto Result=StudioPipelineExport::Write({E,Coordinates,Format},*A,{},[&](int64 N,int64 End){Monotonic&=N>Done&&N<=End&&(!Total||Total==End);Done=N;Total=End;});
        const bool Closed=A->Close()&&!A->IsError();
        if(!Test.TestTrue(*Result.Error,Result.bSuccess&&Closed&&Monotonic&&Done==Total&&Total>0&&Result.PipelineId==E.Prepared.Recipe.Id))return false;
    }
    return true;
}
TOptional<FStudioFieldExportResult> Await(FStudioFieldExportTask& T)
{
    const double End=FPlatformTime::Seconds()+10.;
    do{if(auto R=T.Poll())return R;FPlatformProcess::SleepNoStats(.001f);}while(FPlatformTime::Seconds()<End);return {};
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineExportFiles,"Studio.PipelineExport.PublishedGeometryAndProbeFiles",StudioPipelineExportTestsPrivate::Flags)
bool FPipelineExportFiles::RunTest(const FString&)
{
    using namespace StudioPipelineExportTestsPrivate;
    auto R=Fixture();if(!TestTrue(TEXT("Authentic surface opens"),R.Source.IsValid()))return false;
    auto Clip=Op(EStudioPipelineOperation::ClipBox,TEXT("Wing window"));Clip.A=FVector(-.1,-.01,-.1);Clip.B=FVector(.4,.01,.1);R.Recipe.Operations.Add(Clip);
    if(!SaveCase(TEXT("surface"),StudioPipelineEvaluation::Evaluate(R),*this))return false;
    auto M=Op(EStudioPipelineOperation::Magnitude,TEXT("Speed α"));M.Field=TEXT("derived.speed");M.Unit=TEXT("m/s");M.Components={TEXT("velocity_u"),TEXT("velocity_v")};
    R.Recipe.Operations.Insert(M,1);if(!SaveCase(TEXT("magnitude"),StudioPipelineEvaluation::Evaluate(R),*this))return false;
    R.Recipe.Operations.RemoveAt(1);auto Contour=Op(EStudioPipelineOperation::Contour,TEXT("Pressure line"));Contour.Value=-20;R.Recipe.Operations.Add(Contour);
    if(!SaveCase(TEXT("lines"),StudioPipelineEvaluation::Evaluate(R),*this))return false;
    R=Fixture(false,false);R.Recipe.Operations.Add(Clip);if(!SaveCase(TEXT("points"),StudioPipelineEvaluation::Evaluate(R),*this))return false;
    Clip.Id=FGuid::NewGuid();Clip.Name=TEXT("Empty window");Clip.A=FVector(5,5,5);Clip.B=FVector(6,6,6);R.Recipe.Operations.Add(Clip);
    if(!SaveCase(TEXT("empty"),StudioPipelineEvaluation::Evaluate(R),*this))return false;
    R=Fixture(false,true,true);if(!SaveCase(TEXT("translated"),StudioPipelineEvaluation::Evaluate(R),*this))return false;
    R=Fixture();auto Probe=Op(EStudioPipelineOperation::Probe,TEXT("Line with missing coverage"));Probe.bLine=true;Probe.A=FVector(-.6,0,.02);Probe.B=FVector(.5,0,.02);Probe.Samples=17;R.Recipe.Operations.Add(Probe);
    auto E=StudioPipelineEvaluation::Evaluate(R);if(!SaveCase(TEXT("probe"),E,*this))return false;
    TestTrue(TEXT("Probe fixture contains values and missing samples"),E.Output->Probe->Samples.ContainsByPredicate([](const auto& V){return V.Value.IsSet();})&&E.Output->Probe->Samples.ContainsByPredicate([](const auto& V){return !V.Value.IsSet();}));
    R.Recipe.Operations.Last().A.Y=.2;R.Recipe.Operations.Last().B.Y=.2;
    if(!SaveCase(TEXT("probe-off-plane"),StudioPipelineEvaluation::Evaluate(R),*this))return false;
    R=Fixture(true);auto Slice=Op(EStudioPipelineOperation::Slice,TEXT("Oblique plane"));Slice.A=FVector(.04,.04,0);Slice.B=FVector(1,2,3).GetSafeNormal();R.Recipe.Operations.Add(Slice);
    if(!SaveCase(TEXT("volume-slice"),StudioPipelineEvaluation::Evaluate(R),*this))return false;
    R.Recipe.Operations.RemoveAt(1);Contour.Value=-.15;R.Recipe.Operations.Add(Contour);Clip.Id=FGuid::NewGuid();Clip.Name=TEXT("Wake window");Clip.A=FVector(.005,.015,-.03);Clip.B=FVector(.12,.065,.03);R.Recipe.Operations.Add(Clip);
    if(!SaveCase(TEXT("volume-contour"),StudioPipelineEvaluation::Evaluate(R),*this))return false;
    AddInfo(TEXT("Independent VTK/CSV readback: ")+FPaths::ConvertRelativePathToFull(Root()));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineExportSafety,"Studio.PipelineExport.ValidationCancellationAndBoundedWrites",StudioPipelineExportTestsPrivate::Flags)
bool FPipelineExportSafety::RunTest(const FString&)
{
    using namespace StudioPipelineExportTestsPrivate;auto R=Fixture();FStudioPipelineExportRequest Request;Request.Evaluation=StudioPipelineEvaluation::Evaluate(R);
    if(!TestTrue(*Request.Evaluation.Error,Request.Evaluation.Output.IsValid()))return false;
    for(const auto Format:{EStudioFieldExportFormat::VTK,EStudioFieldExportFormat::CSV})
    {
        Request.Format=Format;auto C=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);FBufferArchive Pre;
        TestTrue(TEXT("Precancel writes nothing"),StudioPipelineExport::Write(Request,Pre,C).bCancelled&&Pre.IsEmpty());
        C->store(false);FBufferArchive Mid;const auto Cancelled=StudioPipelineExport::Write(Request,Mid,C,[C](int64 N,int64){if(N>=256)C->store(true);});
        TestTrue(TEXT("Mid-write cancellation is explicit"),Cancelled.bCancelled&&!Cancelled.bSuccess);
        class FFailing final:public FArchive
        {public:FFailing(){SetIsSaving(true);}void Serialize(void*,int64 N)override{Largest=FMath::Max(Largest,N);Bytes+=N;if(Bytes>70000)SetError();}int64 Bytes=0,Largest=0;};
        FFailing Failure;const auto Failed=StudioPipelineExport::Write(Request,Failure);
        TestTrue(TEXT("Failed disk writes return error in bounded chunks"),!Failed.bSuccess&&!Failed.Error.IsEmpty()&&Failure.Largest<40000);
    }
    Request.Format=EStudioFieldExportFormat::VTK;
    for(int32 Fault=0;Fault<6;++Fault)
    {
        auto Bad=Request;auto Output=MakeShared<FStudioPipelineOutput,ESPMode::ThreadSafe>(*Bad.Evaluation.Output);Bad.Evaluation.Output=Output;
        if(Fault==0)Bad.Evaluation.Prepared.Recipe.Source.Identity.Ordinal++;
        if(Fault==1)Output->Triangles[0].X=Output->Vertices.Num();
        if(Fault==2)Output->Vertices[0].Scalar=std::numeric_limits<double>::quiet_NaN();
        if(Fault==3)Output->Vertices[0].OriginalRow=MAX_int32;
        if(Fault==4)Output->Range=FVector2D(-1,1);
        if(Fault==5)Output->Kind=EStudioPipelineOutputKind::OriginalPoints;
        FBufferArchive A;const auto Result=StudioPipelineExport::Write(Bad,A);
        TestTrue(TEXT("Inconsistent identity, topology, values, rows and ranges rejected before bytes"),!Result.bSuccess&&!Result.Error.IsEmpty()&&A.IsEmpty());
    }
    auto Probe=Op(EStudioPipelineOperation::Probe,TEXT("Probe"));Probe.A=FVector(.2,.2,.02);R.Recipe.Operations.Add(Probe);Request.Evaluation=StudioPipelineEvaluation::Evaluate(R);
    FBufferArchive NoVTK;TestTrue(TEXT("Probe VTK directs to gap-preserving CSV"),StudioPipelineExport::Write(Request,NoVTK).Error.Contains(TEXT("CSV"))&&NoVTK.IsEmpty());
    Request.Format=EStudioFieldExportFormat::CSV;auto Broken=MakeShared<FStudioPipelineOutput,ESPMode::ThreadSafe>(*Request.Evaluation.Output);
    Broken->Probe->Samples[0].Status=EStudioProbeSampleStatus::Value;Request.Evaluation.Output=Broken;FBufferArchive NoCSV;
    TestTrue(TEXT("Missing value cannot claim value status"),!StudioPipelineExport::Write(Request,NoCSV).bSuccess&&NoCSV.IsEmpty());return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineExportPublish,"Studio.PipelineExport.AtomicPublishSnapshotAndRelease",StudioPipelineExportTestsPrivate::Flags)
bool FPipelineExportPublish::RunTest(const FString&)
{
    using namespace StudioPipelineExportTestsPrivate;
    const FString Directory=Root()/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Directory,true);
    ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Directory,false,true);};
    const FString Path=Directory/TEXT("existing.vtp"),Original=TEXT("previous valid destination");FFileHelper::SaveStringToFile(Original,*Path);
    auto Untouched=[&]{FString S;return FFileHelper::LoadFileToString(S,*Path)&&S==Original;};
    auto Staging=[] {TArray<FString> Paths;IFileManager::Get().FindFiles(Paths,*(FPaths::ProjectSavedDir()/TEXT("ExportStaging/*.partial")),true,false);Paths.Sort();return Paths;};const auto Before=Staging();
    auto Input=Fixture();FStudioPipelineExportRequest Request;Request.Evaluation=StudioPipelineEvaluation::Evaluate(Input);
    if(!TestTrue(*Request.Evaluation.Error,Request.Evaluation.Output.IsValid()))return false;
    struct FGate{FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);~FGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}};
    auto Gate=MakeShared<FGate,ESPMode::ThreadSafe>();FStudioFieldExportTask Task;
    Task.BeforePublishForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(10000);};
    TestTrue(TEXT("Start frozen pipeline write"),Task.Start(Request,Path));TestTrue(TEXT("Wait at publication boundary"),Gate->Reached->Wait(10000));
    TestFalse(TEXT("No queued pipeline write"),Task.Start(Request,Directory/TEXT("queued.vtp")));
    TestTrue(TEXT("Completed encoding reports monotonic endpoint"),Task.Progress().Total>0&&Task.Progress().Completed==Task.Progress().Total);
    TestTrue(TEXT("Cancel before publishing"),Task.Cancel());Gate->Release->Trigger();auto Done=Await(Task);
    TestTrue(TEXT("Cancel preserves existing file and cleans stage"),Done&&Done->bCancelled&&!Done->bSuccess&&Untouched()&&Staging()==Before);
    TestTrue(TEXT("Invalid destination starts independent task"),Task.Start(Request,Path/TEXT("child.vtp")));Done=Await(Task);
    TestTrue(TEXT("Failure preserves destination and cleans stage"),Done&&!Done->bSuccess&&!Done->Error.IsEmpty()&&Untouched()&&Staging()==Before);
    const auto Id=Request.Evaluation.Prepared.Recipe.Id;const auto Name=Request.Evaluation.Prepared.Recipe.Name;
    TestTrue(TEXT("Retry output"),Task.Start(Request,Path));Request.Evaluation.Prepared.Recipe.Name=TEXT("Later edit");Input.Source->ReadScalarFrame(2,TEXT("pressure"));Done=Await(Task);
    TestTrue(TEXT("Published identity is frozen despite source and recipe edits"),Done&&Done->bSuccess&&Done->Identity.Ordinal==1&&Done->PipelineId==Id&&Done->PipelineName==Name);
    TestFalse(TEXT("Published output cannot be cancelled"),Task.Cancel());
    auto Evaluation=StudioPipelineEvaluation::Evaluate(Input);TWeakPtr<const FStudioPipelineOutput,ESPMode::ThreadSafe> Weak=Evaluation.Output;
    FStudioFieldExportTask Closing;FStudioPipelineExportRequest Close{MoveTemp(Evaluation)};
    TestTrue(TEXT("Start output with sole retained owner"),Closing.Start(MoveTemp(Close),Directory/TEXT("closing.vtp")));Closing.Shutdown();Closing.Shutdown();
    TestTrue(TEXT("Shutdown drains and releases evaluated output"),!Closing.IsBusy()&&!Weak.IsValid()&&!Closing.Poll()&&Staging()==Before);
    return true;
}
#endif
