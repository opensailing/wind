#include "StudioScene.h"
#include "StudioSnapshotSource.h"
#include "StudioPipelineRenderData.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Renderer acceptance only. No Post-Processing controls or physical-input
 * acceptance is implied by these programmatically placed independent scenes. */
class FStudioPipelineSceneCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioPipelineSceneCommand(FAutomationTestBase* In):Test(In){}
    ~FStudioPipelineSceneCommand(){Task.Shutdown();if(Scene.IsValid())Scene->Destroy();}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>180){Test->AddError(FString::Printf(TEXT("Pipeline renderer timed out at case %d phase %d"),Kind,Phase));return true;}
        if(!Solve.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)if(It->Model&&!It->Model->IsSnapshotView())Solve=*It;
        if(!Solve.IsValid()||!Solve->Model||GFrameCounter<Changed+5)return false;
        auto& M=*Solve->Model;
        switch(Phase)
        {
        case 0:
            if(!Solve->HasCurrentFrame()||M.IsRecordingLoadPending()||M.IsProjectOpenPending())return false;
            Baseline=StudioProjectIO::Serialize(M.SnapshotProject());BaselineSource=M.Solver;BaselineState=M.State;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/PipelineRendering");IFileManager::Get().MakeDirectory(*Root,true);
            IFileManager::Get().Delete(*(Root/TEXT("solve-after.json")),false,true);
            FFileHelper::SaveStringToFile(Baseline,*(Root/TEXT("solve-before.json")));
            StartCase();Next(1);break;
        case 1:
        {
            auto Result=Task.Poll();if(!Result.IsSet())return false;
            if(!Test->TestTrue(*Result->Error,Result->Output.IsValid()))return true;
            FString Error;auto Snapshot=FStudioSnapshotSource::CreatePipeline(*Result,Error);if(!Test->TestTrue(*Error,Snapshot.IsValid()))return true;
            auto* S=Solve->GetWorld()->SpawnActor<AStudioScene>();if(!Test->TestNotNull(TEXT("Independent scene created"),S))return true;
            Scene=S;*Visible=true;S->SetViewVisibility([Gate=Visible]{return *Gate;});
            Test->TestTrue(TEXT("Scene accepts only a paired evaluated pipeline"),S->InitializePipeline(Snapshot.ToSharedRef()));
            Test->TestFalse(TEXT("Scene cannot be initialized twice"),S->InitializePipeline(Snapshot.ToSharedRef()));
            const auto Size=GEngine->GameViewport->Viewport->GetSizeXY();S->ResizeViewport(Size.X,Size.Y);
            if(Kind!=2&&Kind!=6)
            {
                auto Camera=S->SavedCameraState();Camera.Focus=FVector(.075,0,0);Camera.Position=Camera.Focus+FVector(0,1,0);
                Camera.Orientation=FRotator(0,-90,0).Quaternion();Camera.OrbitDistance=1;Camera.bOrthographic=true;Camera.OrthoWidth=.58;
                S->RestoreCamera(Camera,TEXT("Pipeline acceptance camera"));
            }
            if(Kind==0)S->Model->SetScalarStyle(0,true,24,28);
            Next(2);break;
        }
        case 2:
        {
            if(Kind==6)
            {
                if(Scene->bBuilding||Scene->Model->Notice.IsEmpty())return false;
                Test->TestTrue(TEXT("Oversized authentic point result reports its limit and publishes no mesh"),!Scene->HasCurrentFrame()&&
                    Scene->ResourceStats().Vertices==0&&Scene->Model->Notice.Contains(TEXT("50,000"))&&Scene->PipelineOutput()->Vertices.Num()>50000);
                Retire();Next(7);break;
            }
            if(!Ready())return false;
            auto* S=Scene.Get();const auto& O=*S->PipelineOutput();const auto Stats=S->ResourceStats();
            const int64 Expected=Kind==0||Kind==2?O.Triangles.Num()*3:Kind==1?O.Lines.Num()*24:Kind==3?O.Vertices.Num()*18:0;
            Test->TestEqual(TEXT("Only complete evaluated geometry is submitted to Metal"),Stats.Indices,Expected);
            Test->TestEqual(TEXT("No source geometry, extrusion, vector or domain section is added"),Stats.Sections,Expected>0?1:0);
            Test->TestTrue(TEXT("Mesh and scalar textures remain within display budgets"),Stats.Vertices<=StudioPipelineRendering::MaxVertices&&Stats.MeshBytes<128LL*1024*1024&&Stats.ScalarTextureBytes<=4*1024*1024);
            Test->TestTrue(TEXT("Presented field has the exact recorded source frame"),S->PresentedField()->Identity()->Ordinal==1&&S->PresentedFrame().Index==S->Model->Solver->EvaluateFrame(1).Index);
            Test->TestTrue(TEXT("Derived array provenance is visible to the scene"),Kind!=0||(S->PresentedScalar().Id==TEXT("derived.speed")&&S->PresentedScalar().Origin==TEXT("pipeline-derived")));
            const auto* Capture=S->FindComponentByClass<USceneCaptureComponent2D>();
            Test->TestTrue(TEXT("Pipeline capture cannot include Solve scene primitives"),Capture&&Capture->PrimitiveRenderMode==ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList);
            if(Capture)for(const auto& P:Capture->ShowOnlyComponents)Test->TestTrue(TEXT("Capture primitive belongs to its independent scene"),P.IsValid()&&P->GetOwner()==S);
            if(Kind==0)VerifySurfacePixels();
            PixelHash=ReadPixels();if(Kind<4)Test->TestTrue(TEXT("Real numerical output makes visible pixels"),Colored>20);else Test->TestEqual(TEXT("Empty domain or table clears every old primitive"),Colored,0);
            Test->TestTrue(TEXT("Save native renderer evidence"),S->Snapshot(Root/FString::Printf(TEXT("output-%d.png"),Kind)));
            if(Kind==2){S->FitCamera();Next(3);break;}
            S->Orbit(22,-11);Next(3);break;
        }
        case 3:
            if(!Ready())return false;
            if(Kind<4)Test->TestNotEqual(TEXT("Independent camera changes the rendered perspective"),ReadPixels(),PixelHash);
            Scene->SetCameraMode(true);Scene->Fly(FVector(.03,.02,0),.05);Scene->Look(3,-2);Next(4);break;
        case 4:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Free camera remains available on evaluated output"),Scene->SavedCameraState().bFreeCamera);
            Test->TestTrue(TEXT("Save arbitrary camera evidence"),Scene->Snapshot(Root/FString::Printf(TEXT("camera-%d.png"),Kind)));
            Captures=Scene->GetCaptureCount();*Visible=false;Next(5);break;
        case 5:
            Test->TestTrue(TEXT("Hidden view submits no capture or geometry work"),Scene->GetCaptureCount()==Captures&&Scene->ResourceStats().Workers==0&&!Scene->HasCurrentFrame());
            *Visible=true;Next(6);break;
        case 6:
            if(!Ready())return false;
            Test->TestEqual(TEXT("Showing unchanged view reuses its captured frame"),Scene->GetCaptureCount(),Captures);
            Test->TestTrue(TEXT("Pipeline rendering leaves Solve source and run state unchanged"),BaselineSource==M.Solver&&BaselineState==M.State);
            {
                const auto Current=StudioProjectIO::Serialize(M.SnapshotProject());
                FFileHelper::SaveStringToFile(Current,*(Root/TEXT("solve-after.json")));
                Test->TestTrue(TEXT("Pipeline rendering leaves Solve case, frame, camera and display unchanged"),Baseline==Current);
            }
            Retire();Next(7);break;
        case 7:
            Test->TestTrue(TEXT("Destroy releases pinned outputs and view model before GC"),!OldField.IsValid()&&!OldOutput.IsValid()&&!OldModel.IsValid()&&Retired&&!Retired->Model&&!Retired->GetRenderTarget());
            Test->TestTrue(TEXT("Destroy clears geometry and drains workers"),Retired->ResourceStats().Vertices==0&&Retired->ResourceStats().Workers==0&&Retired->ResourceStats().ScalarTextureBytes==0);
            Retired.Reset();
            if(++Kind==7)return true;
            StartCase();Next(1);break;
        }
        return Test->HasAnyErrors();
    }
private:
    void Next(int32 P){Phase=P;Changed=GFrameCounter;}
    bool Ready() const {return Scene.IsValid()&&Scene->HasCurrentFrame()&&StudioView::CameraEquals(Scene->PresentedCamera(),Scene->SavedCameraState());}
    void Retire()
    {
        OldField=Scene->Model->Solver->CaptureField(1);OldOutput=Scene->PipelineOutput();OldModel=Scene->Model;
        Retired.Reset(Scene.Get());Scene->Destroy();Scene.Reset();
    }
    void StartCase()
    {
        const bool Volume=Kind==2||Kind==6;const FString Samples=FPaths::ProjectContentDir()/TEXT("Samples");
        auto Source=StudioRecordings::Import(Samples/(Volume?TEXT("Cylinder3D_ReaderFixture"):TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),1,{});
        if(!Test->TestTrue(*Source.Error,Source.Source&&Source.Reference.IsSet()))return;
        if(Kind!=3&&Kind!=6)Source=StudioRecordings::ImportReconstruction(*Source.Reference,Samples/(Volume?TEXT("Cylinder3D_VolumeFixture"):TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),1,{});
        if(!Test->TestTrue(*Source.Error,Source.Source&&Source.Reference.IsSet()))return;
        FStudioPipelinePrepareRequest R;R.ProjectId=FGuid::NewGuid();R.Revision=1;R.Source=Source.Source;R.Recipe.Name=TEXT("Native pipeline rendering");
        R.Recipe.Source.Title=Source.Source->Descriptor().Title;R.Recipe.Source.Reference=Source.Reference;
        const auto Field=Source.Source->ReadScalarFrame(1,TEXT("pressure")).Field;if(!Test->TestTrue(TEXT("Exact source field available"),Field&&Field->Identity().IsSet()))return;
        R.Recipe.Source.Identity=*Field->Identity();FStudioPipelineOperation Selection;Selection.Name=TEXT("Pressure");Selection.Field=TEXT("pressure");Selection.Unit=TEXT("Pa");
        if(Kind==0){Selection.Kind=EStudioPipelineOperation::Magnitude;Selection.Name=TEXT("Velocity norm");Selection.Field=TEXT("derived.speed");Selection.Unit=TEXT("m/s");Selection.Components={TEXT("velocity_u"),TEXT("velocity_v")};}
        R.Recipe.Operations.Add(Selection);
        if(Kind==1||Kind==2){FStudioPipelineOperation Contour;Contour.Kind=EStudioPipelineOperation::Contour;Contour.Name=TEXT("Contour");Contour.Value=Kind==1?-20:-.15;R.Recipe.Operations.Add(Contour);}
        if(Kind!=5&&Kind!=6)
        {
            FStudioPipelineOperation Clip;Clip.Kind=EStudioPipelineOperation::ClipBox;Clip.Name=TEXT("Clip");
            Clip.A=Volume?FVector(.005,.015,-.03):FVector(-.15,-.01,-.15);Clip.B=Volume?FVector(.12,.065,.03):FVector(.3,.01,.15);
            if(Kind==4){Clip.A=FVector(5,5,5);Clip.B=FVector(6,6,6);}R.Recipe.Operations.Add(Clip);
        }
        if(Kind==5){FStudioPipelineOperation Probe;Probe.Kind=EStudioPipelineOperation::Probe;Probe.Name=TEXT("Probe");Probe.A=FVector(.2,0,.02);R.Recipe.Operations.Add(Probe);}
        FString Error;Test->TestTrue(*Error,Task.Start(MoveTemp(R),Error));
    }
    uint32 ReadPixels()
    {
        TArray<FColor> Pixels;FReadSurfaceDataFlags Flags(RCM_UNorm);Flags.SetLinearToGamma(false);
        if(!Test->TestTrue(TEXT("Read actual Metal pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags))||Pixels.IsEmpty())return 0;
        Colored=0;const FColor Background=Pixels[0];for(const auto& P:Pixels)if(FMath::Abs(int32(P.R)-Background.R)>8||FMath::Abs(int32(P.G)-Background.G)>8||FMath::Abs(int32(P.B)-Background.B)>8)++Colored;
        return FCrc::MemCrc32(Pixels.GetData(),Pixels.Num()*sizeof(FColor));
    }
    void VerifySurfacePixels()
    {
        const auto& O=*Scene->PipelineOutput();TArray<FColor> Pixels;FReadSurfaceDataFlags Flags(RCM_UNorm);Flags.SetLinearToGamma(false);
        if(!Test->TestTrue(TEXT("Read scalar surface pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags)))return;
        const int32 W=Scene->GetRenderTarget()->SizeX,H=Scene->GetRenderTarget()->SizeY;const auto Camera=Scene->PresentedCamera();
        const double Width=Camera.OrthoWidth,Height=Width*H/W;int32 Compared=0,Wrong=0,Outside=0,MaxError=0;
        for(int32 Y=H/10;Y<H*9/10;Y+=FMath::Max(1,H/23))for(int32 X=W/10;X<W*9/10;X+=FMath::Max(1,W/31))
        {
            const FVector2D P(Camera.Focus.X+((X+.5)/W-.5)*Width,Camera.Focus.Z+(.5-(Y+.5)/H)*Height);
            if(P.X<-.151||P.X>.301||P.Y<-.151||P.Y>.151)
            {++Outside;Test->TestTrue(TEXT("Clipped-away pixels never fall back to original source geometry"),Pixels[Y*W+X]==Pixels[0]);continue;}
            for(const auto& T:O.Triangles)
            {
                const auto& A=O.Vertices[T.X];const auto& B=O.Vertices[T.Y];const auto& C=O.Vertices[T.Z];
                const double Det=(B.PositionMeters.Z-C.PositionMeters.Z)*(A.PositionMeters.X-C.PositionMeters.X)+(C.PositionMeters.X-B.PositionMeters.X)*(A.PositionMeters.Z-C.PositionMeters.Z);
                if(FMath::Abs(Det)<1.e-25)continue;
                const double U=((B.PositionMeters.Z-C.PositionMeters.Z)*(P.X-C.PositionMeters.X)+(C.PositionMeters.X-B.PositionMeters.X)*(P.Y-C.PositionMeters.Z))/Det;
                const double V=((C.PositionMeters.Z-A.PositionMeters.Z)*(P.X-C.PositionMeters.X)+(A.PositionMeters.X-C.PositionMeters.X)*(P.Y-C.PositionMeters.Z))/Det;
                const double Z=1-U-V;if(U<0||V<0||Z<0)continue;if(FMath::Min3(U,V,Z)<.08)break;
                const FColor Expected=StudioColor::Map(U*A.Scalar+V*B.Scalar+Z*C.Scalar,Scene->PresentedColorMapping()).ToFColor(true);
                const FColor Actual=Pixels[Y*W+X];const int32 Error=FMath::Max3(FMath::Abs(int32(Actual.R)-Expected.R),FMath::Abs(int32(Actual.G)-Expected.G),FMath::Abs(int32(Actual.B)-Expected.B));
                MaxError=FMath::Max(MaxError,Error);++Compared;if(Error>4)++Wrong;break;
            }
        }
        Test->AddInfo(FString::Printf(TEXT("Derived surface GPU readback: %d samples, %d mismatch, max byte error %d, %d clipped-away pixels."),Compared,Wrong,MaxError,Outside));
        Test->TestTrue(TEXT("Readback covers the surface and clipped area"),Compared>50&&Outside>10);
        Test->TestEqual(TEXT("Metal interpolates the evaluated scalar before palette and clamping"),Wrong,0);
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Solve,Scene;TStrongObjectPtr<AStudioScene> Retired;
    TSharedRef<bool> Visible=MakeShared<bool>(true);FStudioPipelineEvaluationTask Task;
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> BaselineSource;
    TWeakPtr<const IStudioField,ESPMode::ThreadSafe> OldField;TWeakPtr<const FStudioPipelineOutput,ESPMode::ThreadSafe> OldOutput;TWeakPtr<FStudioModel> OldModel;
    FString Root,Baseline;EStudioRunState BaselineState=EStudioRunState::Ready;
    double Started=0;int32 Kind=0,Phase=0,Colored=0;uint64 Changed=0,Captures=0;uint32 PixelHash=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPipelineScene,"Studio.PipelineScene.GeometryCameraAndLifetime",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioPipelineScene::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioPipelineSceneCommand(this));return true;}
#endif
