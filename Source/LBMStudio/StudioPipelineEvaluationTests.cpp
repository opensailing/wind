#include "StudioPipelineEvaluation.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioPipelineEvaluationTestsPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString Fixture(const TCHAR* Name){return FPaths::ProjectContentDir()/TEXT("Samples")/Name;}
FString Output(){return FPaths::ProjectSavedDir()/TEXT("Automation/PipelineGeometry");}
FStudioPipelineOperation Op(EStudioPipelineOperation Kind,const TCHAR* Name)
{FStudioPipelineOperation O;O.Kind=Kind;O.Name=Name;return O;}
struct FFixture
{
    FStudioPipelinePrepareRequest Request;
    FString Error;
    bool Load(bool Volume=false,bool Reconstruction=true)
    {
        auto R=StudioRecordings::Import(Fixture(Volume?TEXT("Cylinder3D_ReaderFixture"):TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),1,{});
        if(!R.Source||!R.Reference.IsSet()){Error=R.Error;return false;}
        if(Reconstruction)R=StudioRecordings::ImportReconstruction(*R.Reference,Fixture(Volume?TEXT("Cylinder3D_VolumeFixture"):TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),1,{});
        if(!R.Source||!R.Reference.IsSet()){Error=R.Error;return false;}
        auto Field=R.Source->ReadScalarFrame(1,TEXT("pressure")).Field;if(!Field||!Field->Identity().IsSet())return false;
        Request.ProjectId=FGuid::NewGuid();Request.Revision=2;Request.Source=R.Source;auto& P=Request.Recipe;
        P.Name=TEXT("Pressure geometry");P.Source.Title=R.Source->Descriptor().Title;P.Source.Identity=*Field->Identity();P.Source.Reference=R.Reference;
        auto S=Op(EStudioPipelineOperation::Field,TEXT("Pressure"));S.Field=TEXT("pressure");S.Unit=TEXT("Pa");P.Operations={S};return true;
    }
    void Magnitude(bool Volume=false)
    {
        auto M=Op(EStudioPipelineOperation::Magnitude,TEXT("Derived speed"));M.Field=TEXT("derived.speed");M.Unit=TEXT("m/s");M.Components={TEXT("velocity_u"),TEXT("velocity_v")};
        if(Volume)M.Components.Add(TEXT("velocity_w"));Request.Recipe.Operations={M};
    }
};
bool WriteCase(const TCHAR* Name,const FStudioPipelineEvaluationResult& R)
{
    if(!R.Output)return false;IFileManager::Get().MakeDirectory(*Output(),true);const auto& O=*R.Output;
    TArray<double> Vertices;TArray<int64> Identity;
    for(const auto& V:O.Vertices){Vertices.Append({V.PositionMeters.X,V.PositionMeters.Y,V.PositionMeters.Z,V.Scalar});Identity.Append({V.OriginalRow,V.OriginalPointId});}
    auto Save=[&](const FString& S,const void* Data,int64 Bytes)
    {return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(static_cast<const uint8*>(Data),Bytes),*(Output()/(FString(Name)+S)));};
    if(!Save(TEXT("-vertices.f64"),Vertices.GetData(),Vertices.Num()*sizeof(double))||!Save(TEXT("-identity.i64"),Identity.GetData(),Identity.Num()*sizeof(int64))||
        !Save(TEXT("-triangles.i32"),O.Triangles.GetData(),O.Triangles.Num()*sizeof(FIntVector))||!Save(TEXT("-lines.i32"),O.Lines.GetData(),O.Lines.Num()*sizeof(FIntPoint)))return false;
    auto JSON=MakeShared<FJsonObject>();const auto& I=R.Prepared.Recipe.Source.Identity;
    JSON->SetStringField(TEXT("source"),I.MetadataSHA256);JSON->SetStringField(TEXT("reconstruction"),I.ReconstructionSHA256);
    JSON->SetNumberField(TEXT("ordinal"),I.Ordinal);JSON->SetNumberField(TEXT("step"),I.Frame.Index);JSON->SetNumberField(TEXT("timeSeconds"),I.Frame.Time);
    JSON->SetStringField(TEXT("field"),R.Prepared.Field->SelectedScalar().Id);JSON->SetStringField(TEXT("unit"),R.Prepared.Field->SelectedScalar().Unit);
    JSON->SetStringField(TEXT("method"),O.Method);JSON->SetStringField(TEXT("byteOrder"),TEXT("little"));
    JSON->SetNumberField(TEXT("vertices"),O.Vertices.Num());JSON->SetNumberField(TEXT("triangles"),O.Triangles.Num());JSON->SetNumberField(TEXT("lines"),O.Lines.Num());
    JSON->SetArrayField(TEXT("recipes"),StudioPipelines::ToJSON({R.Prepared.Recipe}));FString Text;FJsonSerializer::Serialize(JSON,TJsonWriterFactory<>::Create(&Text));
    return FFileHelper::SaveStringToFile(Text,*(Output()/(FString(Name)+TEXT(".json"))),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
TOptional<FStudioPipelineEvaluationResult> Finish(FStudioPipelineEvaluationTask& Task)
{
    const double End=FPlatformTime::Seconds()+30;do{if(auto R=Task.Poll())return R;FPlatformProcess::SleepNoStats(.001f);}while(FPlatformTime::Seconds()<End);return {};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineSurfaceOutput,"Studio.PipelineEvaluation.OriginalPointsAndClippedSurfaces",StudioPipelineEvaluationTestsPrivate::Flags)
bool FPipelineSurfaceOutput::RunTest(const FString&)
{
    using namespace StudioPipelineEvaluationTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published NACA surface available"),F.Load()))return false;
    auto Clip=Op(EStudioPipelineOperation::ClipBox,TEXT("Wing region"));Clip.A=FVector(-.1,-.01,-.1);Clip.B=FVector(.4,.01,.1);F.Request.Recipe.Operations.Add(Clip);
    auto R=StudioPipelineEvaluation::Evaluate(F.Request);if(!TestTrue(*R.Error,R.Matches(F.Request.ProjectId,F.Request.Revision,F.Request.Recipe)))return false;
    TestTrue(TEXT("Nonempty reconstructed surface with explicit provenance"),R.Output->Kind==EStudioPipelineOutputKind::Surface&&R.Output->Triangles.Num()>1000&&R.Output->bDerivedGeometry);
    int32 Derived=0;
    for(const auto& V:R.Output->Vertices)
    {
        if(!TestTrue(TEXT("Every triangle vertex remains inside clip"),FBox(Clip.A,Clip.B).IsInsideOrOn(V.PositionMeters)))return false;
        if(V.OriginalRow==INDEX_NONE)++Derived;
        else{int64 Id;FVector P;double Scalar;R.Prepared.Field->OriginalPoint(V.OriginalRow,Id,P);R.Prepared.Field->OriginalScalar(V.OriginalRow,TEXT("pressure"),Scalar);
            if(Id!=V.OriginalPointId||FVector(P.X,P.Z,P.Y)!=V.PositionMeters||Scalar!=V.Scalar){AddError(TEXT("Original row metadata changed."));return false;}}
    }
    TestTrue(TEXT("Box intersections are derived vertices"),Derived>0);TestTrue(TEXT("Export surface for independent audit"),WriteCase(TEXT("naca-surface"),R));
    FFixture Raw;if(!TestTrue(TEXT("Original point source available"),Raw.Load(false,false)))return false;Raw.Request.Recipe.Operations.Add(Clip);
    auto Points=StudioPipelineEvaluation::Evaluate(Raw.Request);if(!TestTrue(*Points.Error,Points.Output.IsValid()))return false;
    TestTrue(TEXT("Point source stays a point source"),Points.Output->Kind==EStudioPipelineOutputKind::OriginalPoints&&!Points.Output->bDerivedGeometry&&Points.Output->Triangles.IsEmpty());
    for(const auto& V:Points.Output->Vertices)if(V.OriginalRow==INDEX_NONE){AddError(TEXT("Invented original row."));return false;}
    TestTrue(TEXT("Export exact clipped original rows"),WriteCase(TEXT("naca-points"),Points));
    Clip.Id=FGuid::NewGuid();Clip.Name=TEXT("Outside");Clip.A=FVector(5,5,5);Clip.B=FVector(6,6,6);Raw.Request.Recipe.Operations.Add(Clip);
    auto Empty=StudioPipelineEvaluation::Evaluate(Raw.Request);TestTrue(TEXT("Disjoint region is a successful empty result"),Empty.Output&&Empty.Output->IsEmpty()&&!Empty.Output->Range.IsSet()&&Empty.Error.IsEmpty());
    // The installed default source has original cells and a display translation.
    // Exercise that path as well as the external point/reconstruction readers.
    FStudioPipelinePrepareRequest Legacy;Legacy.ProjectId=FGuid::NewGuid();Legacy.Revision=3;
    Legacy.Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();const auto Field=Legacy.Source->ReadScalarFrame(1,TEXT("pressure")).Field;
    if(!TestTrue(TEXT("Installed published mesh available"),Field&&Field->Identity().IsSet()))return false;
    Legacy.Recipe.Name=TEXT("Original translated surface");Legacy.Recipe.Source.Title=Legacy.Source->Descriptor().Title;Legacy.Recipe.Source.Identity=*Field->Identity();
    Legacy.Recipe.Operations={F.Request.Recipe.Operations[0]};auto Region=Op(EStudioPipelineOperation::ClipBox,TEXT("Central region"));
    const FBox Bounds=Legacy.Source->Descriptor().DisplayBounds;Region.A=Bounds.Min+Bounds.GetSize()*.2;Region.B=Bounds.Max-Bounds.GetSize()*.2;Legacy.Recipe.Operations.Add(Region);
    auto Original=StudioPipelineEvaluation::Evaluate(Legacy);
    if(!TestTrue(*Original.Error,Original.Output.IsValid()))return false;
    TestTrue(TEXT("Installed source retains original triangle representation"),Original.Output->Triangles.Num()>0&&Original.Output->Method.Contains(TEXT("Original source triangles")));
    for(const auto& V:Original.Output->Vertices)if(V.OriginalRow!=INDEX_NONE)
    {
        int64 Id;FVector P;Field->OriginalPoint(V.OriginalRow,Id,P);
        if(V.PositionMeters!=FVector(P.X,P.Z,P.Y)+Field->Identity()->SourceOffset||V.OriginalPointId!=Id){AddError(TEXT("Original translated mesh identity changed."));return false;}
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineContourLines,"Studio.PipelineEvaluation.TriangleContoursAndOperationOrder",StudioPipelineEvaluationTestsPrivate::Flags)
bool FPipelineContourLines::RunTest(const FString&)
{
    using namespace StudioPipelineEvaluationTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published surface available"),F.Load()))return false;
    auto C=Op(EStudioPipelineOperation::Contour,TEXT("Pressure contour"));C.Value=-20;F.Request.Recipe.Operations.Add(C);
    auto Clip=Op(EStudioPipelineOperation::ClipBox,TEXT("Contour window"));Clip.A=FVector(-.15,-.01,-.15);Clip.B=FVector(.3,.01,.15);F.Request.Recipe.Operations.Add(Clip);
    auto R=StudioPipelineEvaluation::Evaluate(F.Request);if(!TestTrue(*R.Error,R.Output.IsValid()))return false;
    TestTrue(TEXT("2D scalar produces line geometry"),R.Output->Kind==EStudioPipelineOutputKind::ContourLines&&R.Output->Lines.Num()>100&&R.Output->Triangles.IsEmpty());
    for(const auto& V:R.Output->Vertices)if(V.Scalar!=C.Value||V.OriginalRow!=INDEX_NONE||!FBox(Clip.A,Clip.B).IsInsideOrOn(V.PositionMeters)){AddError(TEXT("Contour identity/value/domain invalid."));return false;}
    TestTrue(TEXT("Export pressure lines"),WriteCase(TEXT("naca-contour"),R));
    F.Magnitude();C.Value=25.5;F.Request.Recipe.Operations.Add(C);F.Request.Recipe.Operations.Add(Clip);
    R=StudioPipelineEvaluation::Evaluate(F.Request);if(!TestTrue(*R.Error,R.Output.IsValid()))return false;
    TestTrue(TEXT("Derived contour approximation is labeled"),R.Output->Method.Contains(TEXT("approximated"))&&R.Output->Lines.Num()>100);
    TestTrue(TEXT("Export magnitude contour lines"),WriteCase(TEXT("naca-magnitude-contour"),R));
    auto Slice=Op(EStudioPipelineOperation::Slice,TEXT("Source plane"));Slice.B=FVector::RightVector;
    auto Upstream=F.Request;Upstream.Recipe.Operations={F.Request.Recipe.Operations[0],Slice};
    const auto Input=StudioPipelineEvaluation::Evaluate(Upstream);
    TestTrue(TEXT("Export sampled input for independent slice-contour audit"),WriteCase(TEXT("naca-slice-input"),Input));
    F.Request.Recipe.Operations.Insert(Slice,1);R=StudioPipelineEvaluation::Evaluate(F.Request);
    if(!TestTrue(*R.Error,R.Output.IsValid()))return false;
    TestTrue(TEXT("Slice then contour has sampled line geometry"),R.Output->Lines.Num()>0);TestTrue(TEXT("Export slice contour order"),WriteCase(TEXT("naca-slice-contour"),R));
    F.Request.Recipe.Operations[2].Value=1.e8;R=StudioPipelineEvaluation::Evaluate(F.Request);
    TestTrue(TEXT("Outside-range contour produces empty success"),R.Output&&R.Output->IsEmpty()&&R.Error.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineVolumeOutput,"Studio.PipelineEvaluation.VolumeContoursAndObliqueSlices",StudioPipelineEvaluationTestsPrivate::Flags)
bool FPipelineVolumeOutput::RunTest(const FString&)
{
    using namespace StudioPipelineEvaluationTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Genuine3D published field available"),F.Load(true)))return false;
    auto C=Op(EStudioPipelineOperation::Contour,TEXT("Pressure isosurface"));C.Value=-.15;F.Request.Recipe.Operations.Add(C);
    auto Clip=Op(EStudioPipelineOperation::ClipBox,TEXT("Wake window"));Clip.A=FVector(.005,.015,-.03);Clip.B=FVector(.12,.065,.03);F.Request.Recipe.Operations.Add(Clip);
    auto R=StudioPipelineEvaluation::Evaluate(F.Request);if(!TestTrue(*R.Error,R.Output.IsValid()))return false;
    TestTrue(TEXT("Volume contour is a nonempty triangle surface"),R.Output->Kind==EStudioPipelineOutputKind::ContourSurface&&R.Output->Triangles.Num()>1000);
    TestTrue(TEXT("Explicit approximation method"),R.Output->Method.Contains(TEXT("tetrahedral"))&&R.Output->Method.Contains(TEXT("approximation")));
    for(const auto& V:R.Output->Vertices)if(V.Scalar!=C.Value||V.OriginalRow!=INDEX_NONE){AddError(TEXT("Volume contour scalar or provenance invalid."));return false;}
    TestTrue(TEXT("Output allocation remains bounded"),R.Output->AllocatedBytes()<=StudioPipelineEvaluation::MaxOutputBytes);
    TestTrue(TEXT("Export volume contour"),WriteCase(TEXT("cylinder-contour"),R));
    F.Magnitude(true);C.Value=.03;F.Request.Recipe.Operations.Add(C);F.Request.Recipe.Operations.Add(Clip);
    auto Speed=StudioPipelineEvaluation::Evaluate(F.Request);if(!TestTrue(*Speed.Error,Speed.Output.IsValid()))return false;
    TestTrue(TEXT("Magnitude has a supported volume contour"),Speed.Output->Triangles.Num()>1000);TestTrue(TEXT("Export magnitude isosurface"),WriteCase(TEXT("cylinder-magnitude-contour"),Speed));
    auto Slice=Op(EStudioPipelineOperation::Slice,TEXT("Oblique section"));Slice.A=FVector(.04,.04,0);Slice.B=FVector(1,2,3).GetSafeNormal();
    F.Request.Recipe.Operations={F.Request.Recipe.Operations[0],Slice,Clip};auto Plane=StudioPipelineEvaluation::Evaluate(F.Request);
    if(!TestTrue(*Plane.Error,Plane.Output.IsValid()))return false;
    TestTrue(TEXT("Oblique3D slice has supported triangles"),Plane.Output->Kind==EStudioPipelineOutputKind::Surface&&Plane.Output->Triangles.Num()>1000);
    for(const auto& V:Plane.Output->Vertices)if(FMath::Abs(FVector::DotProduct(V.PositionMeters-Slice.A,Slice.B))>1.e-9){AddError(TEXT("Slice vertex left requested plane."));return false;}
    TestTrue(TEXT("Export oblique derived slice"),WriteCase(TEXT("cylinder-slice"),Plane));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineProbeOutput,"Studio.PipelineEvaluation.ProbeTableAndEvaluationLifetime",StudioPipelineEvaluationTestsPrivate::Flags)
bool FPipelineProbeOutput::RunTest(const FString&)
{
    using namespace StudioPipelineEvaluationTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published frame available"),F.Load()))return false;
    auto Clip=Op(EStudioPipelineOperation::ClipBox,TEXT("Wake window"));Clip.A=FVector(.15,-.01,-.1);Clip.B=FVector(.4,.01,.1);F.Request.Recipe.Operations.Add(Clip);
    auto Probe=Op(EStudioPipelineOperation::Probe,TEXT("Wake samples"));Probe.bLine=true;Probe.A=FVector(.1,0,.02);Probe.B=FVector(.45,0,.02);Probe.Samples=71;F.Request.Recipe.Operations.Add(Probe);
    auto R=StudioPipelineEvaluation::Evaluate(F.Request);if(!TestTrue(*R.Error,R.Matches(F.Request.ProjectId,F.Request.Revision,F.Request.Recipe)))return false;
    TestTrue(TEXT("Final probe table preserves rows"),R.Output->Kind==EStudioPipelineOutputKind::ProbeTable&&R.Output->Probe.IsSet()&&R.Output->Probe->Samples.Num()==71);
    int32 Values=0,Missing=0;for(const auto& S:R.Output->Probe->Samples)if(S.Value.IsSet())++Values;else ++Missing;
    TestTrue(TEXT("Unsupported rows remain missing"),Values==51&&Missing==20);
    TestFalse(TEXT("Changed revision rejects output"),R.Matches(F.Request.ProjectId,F.Request.Revision+1,F.Request.Recipe));
    auto Changed=F.Request.Recipe;Changed.Operations.Last().A.X+=.01;TestFalse(TEXT("Edited probe rejects output"),R.Matches(F.Request.ProjectId,F.Request.Revision,Changed));
    auto C=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);auto Cancelled=StudioPipelineEvaluation::Evaluate(F.Request,C);
    TestTrue(TEXT("Cancelled evaluation owns no output"),Cancelled.bCancelled&&!Cancelled.Output&&!Cancelled.Prepared.Field&&!Cancelled.Prepared.Source);
    FStudioPipelineEvaluationTask Task;FString Error;TestTrue(TEXT("One evaluation starts"),Task.Start(F.Request,Error));TestFalse(TEXT("Second evaluation not queued"),Task.Start(F.Request,Error));
    Task.Cancel();auto Result=Finish(Task);if(!TestTrue(TEXT("Cancelled worker drains"),Result.IsSet()))return false;
    TestTrue(TEXT("Cancelled result publishes no geometry"),Result->bCancelled&&!Result->Output&&!Result->Prepared.Field&&!Task.IsBusy());
    TestTrue(TEXT("Task reusable after cancellation"),Task.Start(F.Request,Error));Result=Finish(Task);if(!TestTrue(TEXT("Successful worker output"),Result.IsSet()&&Result->Matches(F.Request.ProjectId,F.Request.Revision,F.Request.Recipe)))return false;
    TWeakPtr<const FStudioPipelineOutput,ESPMode::ThreadSafe> Weak=Result->Output;TWeakPtr<const FStudioPipelineField,ESPMode::ThreadSafe> Field=Result->Prepared.Field;Result.Reset();
    TestFalse(TEXT("Task releases output on poll"),Weak.IsValid());TestFalse(TEXT("Task releases field on poll"),Field.IsValid());
    TestTrue(TEXT("Start shutdown evaluation"),Task.Start(F.Request,Error));Task.Shutdown();TestFalse(TEXT("Shutdown drains evaluation"),Task.IsBusy());TestFalse(TEXT("Closed task rejects work"),Task.Start(F.Request,Error));
    return true;
}
#endif
