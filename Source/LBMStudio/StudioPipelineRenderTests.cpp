#include "StudioPipelineRenderData.h"
#include "StudioSnapshotSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioPipelineRenderTestsPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FStudioPipelineOperation Op(EStudioPipelineOperation Kind,const TCHAR* Name)
{FStudioPipelineOperation O;O.Kind=Kind;O.Name=Name;return O;}
bool Load(FStudioPipelinePrepareRequest& R,bool Volume=false,bool Reconstructed=true)
{
    const FString Root=FPaths::ProjectContentDir()/TEXT("Samples");
    auto Source=StudioRecordings::Import(Root/(Volume?TEXT("Cylinder3D_ReaderFixture"):TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),1,{});
    if(!Source.Source||!Source.Reference.IsSet())return false;
    if(Reconstructed)Source=StudioRecordings::ImportReconstruction(*Source.Reference,Root/(Volume?TEXT("Cylinder3D_VolumeFixture"):TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),1,{});
    if(!Source.Source||!Source.Reference.IsSet())return false;
    const auto Read=Source.Source->ReadScalarFrame(1,TEXT("pressure"));if(!Read.Field||!Read.Field->Identity().IsSet())return false;
    R.ProjectId=FGuid::NewGuid();R.Revision=1;R.Source=Source.Source;auto& P=R.Recipe;P.Name=TEXT("Pipeline render test");
    P.Source.Title=Source.Source->Descriptor().Title;P.Source.Identity=*Read.Field->Identity();P.Source.Reference=Source.Reference;
    auto Field=Op(EStudioPipelineOperation::Field,TEXT("Pressure"));Field.Field=TEXT("pressure");Field.Unit=TEXT("Pa");P.Operations={Field};return true;
}
FStudioColorMapping Mapping(const FStudioPipelineEvaluationResult& R)
{return StudioColor::Resolve(R.Prepared.Recipe.Source.Identity.Dataset,R.Prepared.Field->SelectedScalar(),{});}
bool NoMesh(const FStudioPipelineRenderData& R)
{return R.Surface.Vertices.IsEmpty()&&R.Surface.Indices.IsEmpty()&&R.Surface.Scalars.IsEmpty()&&R.Glyphs.Vertices.IsEmpty()&&R.Contour.Vertices.IsEmpty();}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineSnapshotAdapter,"Studio.PipelineRendering.DerivedSnapshotIdentityAndLifetime",StudioPipelineRenderTestsPrivate::Flags)
bool FPipelineSnapshotAdapter::RunTest(const FString&)
{
    using namespace StudioPipelineRenderTestsPrivate;
    TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> Reader;
    TWeakPtr<const FStudioPipelineOutput,ESPMode::ThreadSafe> Output;
    TWeakPtr<const IStudioField,ESPMode::ThreadSafe> Field;
    TSharedPtr<FStudioModel> View;FString Error;
    {
        FStudioPipelinePrepareRequest R;if(!TestTrue(TEXT("Published source available"),Load(R)))return false;
        auto M=Op(EStudioPipelineOperation::Magnitude,TEXT("Velocity norm"));M.Field=TEXT("derived.speed");M.Unit=TEXT("m/s");M.Components={TEXT("velocity_u"),TEXT("velocity_v")};R.Recipe.Operations={M};
        auto Result=StudioPipelineEvaluation::Evaluate(R);if(!TestTrue(*Result.Error,Result.Output.IsValid()))return false;
        auto Snapshot=FStudioSnapshotSource::CreatePipeline(Result,Error);if(!TestTrue(*Error,Snapshot.IsValid()))return false;
        Reader=R.Source;Output=Result.Output;Field=Result.Prepared.Field;
        TestTrue(TEXT("Adapter owns exactly the evaluated geometry and selected derived field"),Snapshot->PipelineOutput()==Result.Output&&
            Snapshot->ReadScalarFrame(1,TEXT("derived.speed")).Field==Result.Prepared.Field&&Snapshot->Descriptor().Scalars.Num()==1);
        const auto& S=Snapshot->Descriptor().Scalars[0];
        TestTrue(TEXT("Derived provenance is never relabeled as a supplied source array"),S.Origin==TEXT("pipeline-derived")&&S.Id==M.Field&&S.Unit==M.Unit&&S.Label==M.Name);
        TestTrue(TEXT("Original scalar, other frames and invented velocity are unavailable"),!Snapshot->ReadScalarFrame(1,TEXT("pressure")).Field&&
            !Snapshot->CaptureField(0)->IsValid()&&!Snapshot->CaptureViewField(1,M.Field,true)->IsValid());
        TestTrue(TEXT("Sparse source timeline survives adapter"),Snapshot->EvaluateFrame(1).Index==5001&&Snapshot->EvaluateFrame(1).Time==12.5025&&Snapshot->FrameCount()==3);
        View=MakeShared<FStudioModel>(Snapshot.ToSharedRef());
        const auto OriginalCamera=View->Project.Camera;auto Camera=OriginalCamera;Camera.Position.X+=.2;Camera.Focus.X+=.2;
        TestTrue(TEXT("Derived scalar works in independent view controls"),View->IsSnapshotView()&&View->ActiveScalar().Id==M.Field&&View->SetScalarStyle(2,true,24,28)&&View->EditCamera(TEXT("Place analysis camera"),Camera));
        View->Run();View->Step();View->Scrub(2);View->Tick(1801);
        TestTrue(TEXT("Independent view cannot advance or write the source project"),View->SelectedFrame==1&&!View->HasUnsavedChanges());
        auto Changed=Result;Changed.Prepared.Recipe.Operations[0].Components[0]=TEXT("pressure");
        TestFalse(TEXT("Edited operation list cannot masquerade as its previously built graph"),Changed.Matches(R.ProjectId,R.Revision,Changed.Prepared.Recipe));
        TestFalse(TEXT("Mismatched graph cannot become a renderer source"),FStudioSnapshotSource::CreatePipeline(Changed,Error).IsValid());
        Changed=Result;Changed.Prepared.Recipe.Source.Identity.Ordinal=0;TestFalse(TEXT("Edited source frame rejected"),FStudioSnapshotSource::CreatePipeline(Changed,Error).IsValid());
        Changed=Result;Changed.Output.Reset();TestFalse(TEXT("Incomplete output rejected"),FStudioSnapshotSource::CreatePipeline(Changed,Error).IsValid());
        Changed=Result;Changed.bCancelled=true;TestFalse(TEXT("Cancelled output rejected"),FStudioSnapshotSource::CreatePipeline(Changed,Error).IsValid());
    }
    TestTrue(TEXT("Renderer retains the numerical result without the source reader"),!Reader.IsValid()&&Output.IsValid()&&Field.IsValid());
    View.Reset();TestTrue(TEXT("Closing view releases all pipeline output ownership"),!Output.IsValid()&&!Field.IsValid());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineSurfaceRender,"Studio.PipelineRendering.ScalarInterpolationAndExactGeometry",StudioPipelineRenderTestsPrivate::Flags)
bool FPipelineSurfaceRender::RunTest(const FString&)
{
    using namespace StudioPipelineRenderTestsPrivate;
    FStudioPipelinePrepareRequest R;if(!TestTrue(TEXT("Published surface available"),Load(R)))return false;
    auto Clip=Op(EStudioPipelineOperation::ClipBox,TEXT("Window"));Clip.A=FVector(-.15,-.01,-.15);Clip.B=FVector(.3,.01,.15);R.Recipe.Operations.Add(Clip);
    const auto Result=StudioPipelineEvaluation::Evaluate(R);if(!TestTrue(*Result.Error,Result.Output.IsValid()))return false;
    auto Map=Mapping(Result);Map.Minimum=-10;Map.Maximum=10;Map.bManualRange=true;
    const auto Data=StudioPipelineRendering::Build(*Result.Output,Map);if(!TestTrue(*Data.Error,Data.Error.IsEmpty()))return false;
    TestTrue(TEXT("Only evaluated surface geometry is emitted"),Data.Glyphs.Vertices.IsEmpty()&&Data.Contour.Vertices.IsEmpty()&&Data.Surface.Indices.Num()==Result.Output->Triangles.Num()*3);
    bool Outside=false;
    for(int32 I=0;I<Result.Output->Triangles.Num();++I)
    {
        const auto& T=Result.Output->Triangles[I];const int32 Original[]={T.X,T.Y,T.Z};
        for(int32 K=0;K<3;++K)
        {
            const int32 J=Data.Surface.Indices[I*3+K];const auto& V=Result.Output->Vertices[Original[K]];const double N=(V.Scalar-Map.Minimum)/(Map.Maximum-Map.Minimum);Outside|=N<0||N>1;
            if(Data.Surface.Vertices[J]!=V.PositionMeters*100.||Data.Surface.Scalars[J]!=float(N))
            {AddError(TEXT("Rendered surface changed numerical geometry or clamped vertex scalars."));return false;}
            const FVector2D UV=Data.Surface.TextureCoordinates[J];const int32 W=Data.Surface.TextureSize.X,H=Data.Surface.TextureSize.Y;
            if(UV!=FVector2D((J%W+.5)/W,(J/W+.5)/H)){AddError(TEXT("Scalar texel identity is incorrect."));return false;}
        }
    }
    TestTrue(TEXT("Narrow ranges interpolate before clamping in the pixel shader"),Outside);
    TestTrue(TEXT("Scalar UV texel centers fit existing half-precision coordinates"),Data.Surface.TextureSize.X<=1024&&Data.Surface.TextureSize.Y<=1024);
    TestTrue(TEXT("Clipping bounds remain in meters, without a fake span"),Data.Bounds.Min.Y==0&&Data.Bounds.Max.Y==0&&Data.Bounds.Min.X>=Clip.A.X&&Data.Bounds.Max.X<=Clip.B.X);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineOtherRender,"Studio.PipelineRendering.PointsContoursAndProbeTable",StudioPipelineRenderTestsPrivate::Flags)
bool FPipelineOtherRender::RunTest(const FString&)
{
    using namespace StudioPipelineRenderTestsPrivate;
    for(int32 Kind=0;Kind<4;++Kind)
    {
        FStudioPipelinePrepareRequest R;if(!TestTrue(TEXT("Published original rows available"),Load(R,Kind==2,Kind!=0)))return false;
        if(Kind==1||Kind==2)
        {
            auto Contour=Op(EStudioPipelineOperation::Contour,TEXT("Isovalue"));Contour.Value=Kind==1?-20:-.15;R.Recipe.Operations.Add(Contour);
            if(Kind==2){auto Clip=Op(EStudioPipelineOperation::ClipBox,TEXT("Wake"));Clip.A=FVector(.005,.015,-.03);Clip.B=FVector(.12,.065,.03);R.Recipe.Operations.Add(Clip);}
        }
        if(Kind==3){auto Probe=Op(EStudioPipelineOperation::Probe,TEXT("Probe"));Probe.A=FVector(.2,0,.02);R.Recipe.Operations.Add(Probe);}
        const auto Result=StudioPipelineEvaluation::Evaluate(R);if(!TestTrue(*Result.Error,Result.Output.IsValid()))return false;
        const auto Map=Mapping(Result);const auto Data=StudioPipelineRendering::Build(*Result.Output,Map);if(!TestTrue(*Data.Error,Data.Error.IsEmpty()))return false;
        const auto& O=*Result.Output;
        if(Kind==0)
        {
            TestTrue(TEXT("Every original point has an arbitrary-camera glyph with no fabricated cells"),Data.Glyphs.Vertices.Num()==O.Vertices.Num()*18&&Data.Surface.Vertices.IsEmpty());
            for(int32 I=0;I<O.Vertices.Num();++I)
            {
                FVector Sum=FVector::ZeroVector;for(int32 J=0;J<18;++J)Sum+=Data.Glyphs.Vertices[I*18+J];
                if(!Sum.Equals(O.Vertices[I].PositionMeters*1800.,1.e-7)||Data.Glyphs.Colors[I*18]!=StudioColor::Map(O.Vertices[I].Scalar,Map))
                {AddError(TEXT("Point glyph moved or recolored an original row."));return false;}
            }
        }
        if(Kind==1)TestTrue(TEXT("All contour segments get constant-isovalue geometry"),Data.Glyphs.Indices.Num()==O.Lines.Num()*24&&Data.Surface.Vertices.IsEmpty());
        if(Kind==2)
        {
            TestTrue(TEXT("Contour surface retains all evaluated triangles"),Data.Contour.Indices.Num()==O.Triangles.Num()*3&&Data.Glyphs.Vertices.IsEmpty());
            for(int32 I=0;I<Data.Contour.Normals.Num();++I)
                if(Data.Contour.Normals[I].ContainsNaN()||!FMath::IsNearlyEqual(Data.Contour.Normals[I].Size(),1.,1.e-6)||Data.Contour.Colors[I]!=StudioColor::Map(-.15,Map))
                {AddError(FString::Printf(TEXT("Contour shading normal %s (length %.17g), color %s."),*Data.Contour.Normals[I].ToString(),Data.Contour.Normals[I].Size(),*Data.Contour.Colors[I].ToString()));return false;}
        }
        if(Kind==3)TestTrue(TEXT("Probe table stays a table without fabricated surface output"),NoMesh(Data)&&O.Probe.IsSet()&&O.Probe->Samples.Num()==1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineRenderFailure,"Studio.PipelineRendering.BoundsCancellationAndInvalidOutput",StudioPipelineRenderTestsPrivate::Flags)
bool FPipelineRenderFailure::RunTest(const FString&)
{
    using namespace StudioPipelineRenderTestsPrivate;
    FStudioPipelinePrepareRequest R;if(!TestTrue(TEXT("Published surface available"),Load(R)))return false;
    const auto Result=StudioPipelineEvaluation::Evaluate(R);if(!TestTrue(*Result.Error,Result.Output.IsValid()))return false;
    auto Map=Mapping(Result);auto Check=[&](const FStudioPipelineOutput& O,const FStudioColorMapping& M)
    {const auto D=StudioPipelineRendering::Build(O,M);return TestTrue(TEXT("Rejected render publishes no partial geometry"),!D.Error.IsEmpty()&&NoMesh(D));};
    auto Bad=*Result.Output;Bad.Triangles.Last().Z=Bad.Vertices.Num();Check(Bad,Map);
    Bad=*Result.Output;Bad.Vertices.Last().Scalar=std::numeric_limits<double>::quiet_NaN();Check(Bad,Map);
    Bad=*Result.Output;Bad.Vertices.Last().PositionMeters.X=std::numeric_limits<double>::infinity();Check(Bad,Map);
    Bad=*Result.Output;Bad.Kind=EStudioPipelineOutputKind::OriginalPoints;Check(Bad,Map);
    Bad=*Result.Output;Bad.Kind=EStudioPipelineOutputKind::OriginalPoints;Bad.Triangles.Reset();Bad.Vertices.SetNum(StudioPipelineRendering::MaxPoints+1);Check(Bad,Map);
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);const auto Cancelled=StudioPipelineRendering::Build(*Result.Output,Map,Cancel);
    TestTrue(TEXT("Cancelled render releases all buffers"),Cancelled.bCancelled&&NoMesh(Cancelled));
    Map.Maximum=Map.Minimum-1;Check(*Result.Output,Map);Map=Mapping(Result);Map.Minimum=0;Map.Maximum=1.e-300;Check(*Result.Output,Map);
    FStudioPipelineOutput Empty;Empty.Kind=EStudioPipelineOutputKind::Surface;const auto E=StudioPipelineRendering::Build(Empty,Mapping(Result));
    TestTrue(TEXT("Valid empty geometry clears the prior scene without an error"),E.Error.IsEmpty()&&!E.bCancelled&&NoMesh(E));
    return true;
}
#endif
