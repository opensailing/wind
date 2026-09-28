#include "StudioScene.h"
#include "StudioSnapshot.h"
#include "StudioFileDialog.h"
#include "StudioPlatformDiagnostics.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "RHIResources.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Compares real recorded rendering, then checks ownership across repeated 4K
 * captures/async writes. No generated field and no long-session claim. */
class FStudioSnapshotRenderingCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioSnapshotRenderingCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>240){Test->AddError(FString::Printf(TEXT("Snapshot rendering timed out in phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||!Scene->HasCurrentFrame()||
            !StudioView::CameraEquals(Scene->PresentedCamera(),Scene->CameraState()))return false;
        switch(Phase)
        {
        case 0:
        {
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/SnapshotRendering");IFileManager::Get().MakeDirectory(*Root,true);
            FString Error;Original=M.SnapshotProject();
            if(!Test->TestTrue(TEXT("Preserve original project"),StudioProjectIO::Save(Root/TEXT("original.lbms"),Original,Error)))return true;
            Test->TestTrue(TEXT("Initialize resource evidence"),FFileHelper::SaveStringToFile(
                TEXT("stage,iteration,footprint_bytes,device_bytes,rhi_bytes,rhi_count,scene_targets,scene_captures,live_captures\n"),*(Root/TEXT("resources.csv"))));
            M.NewProject(TEXT("Snapshot projection acceptance"));M.Navigate(EStudioWorkspace::Solve);
            M.EditView(TEXT("Isolate recorded field"),[](auto& V){V.Display.bVectors=false;V.Display.bMesh=false;});
            Phase=1;break;
        }
        case 1:
            Scene->FitCamera();Phase=2;break;
        case 2:
            BuildViews(TEXT("airfoil"),false);Phase=3;break;
        case 3:
            VerifyView(Views[ViewIndex]);
            if(++ViewIndex<Views.Num()){Scene->RestoreCamera(Views[ViewIndex].Camera,TEXT("Snapshot projection check"));break;}
            if(!Test->TestTrue(TEXT("Open authentic 3D source"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json"))))return true;
            Phase=4;break;
        case 4:
            if(!Test->TestTrue(TEXT("Attach audited 3D reconstruction"),M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json"))))return true;
            Phase=5;break;
        case 5:
            M.EditView(TEXT("Isolate recorded volume"),[&](auto& V){V.Display.bVolume=true;V.Display.bVolumeIsosurface=false;
                V.Display.bCutPlane=false;V.Display.bSourcePoints=false;V.Display.bVectors=false;V.Display.bMesh=false;
                V.Display.VolumeOpacity=.41;V.Display.VolumeOpacityCurve=FVector(.1,.55,.9);V.Display.VolumeStepVoxels=.75;
                V.Display.VolumeClipMinimum=FVector(.1,.1,.1);V.Display.VolumeClipMaximum=FVector(.95,.95,.95);
                const auto Range=M.ActiveColorMapping();V.Display.bVolumeThreshold=true;
                V.Display.VolumeThresholdMinimum=Range.Minimum+(Range.Maximum-Range.Minimum)*.05;
                V.Display.VolumeThresholdMaximum=Range.Minimum+(Range.Maximum-Range.Minimum)*.95;});
            Scene->FitCamera();Phase=6;break;
        case 6:
            Test->TestTrue(TEXT("Real 3D volume texture is active"),Scene->ResourceStats().ScalarTextureBytes>0);
            Outside=Scene->SavedCameraState();Outside.bOrthographic=false;Outside.bDepthClipping=false;
            BuildViews(TEXT("volume"),true);Phase=7;break;
        case 7:
            VerifyView(Views[ViewIndex]);
            if(++ViewIndex<Views.Num()){Scene->RestoreCamera(Views[ViewIndex].Camera,TEXT("Snapshot projection check"));break;}
            Scene->RestoreCamera(Outside,TEXT("Repeated maximum-size snapshots"));Phase=8;break;
        case 8:
        {
            FStudioSnapshot S;S.Options.Size=FIntPoint(4096,4096);FString Error;
            const auto Field=Scene->PresentedField();const auto Camera=Scene->SavedCameraState();const uint64 Capture=Scene->GetCaptureCount();
            if(!Test->TestTrue(TEXT("Render annotated 4096-square snapshot"),Scene->CaptureSnapshot(S,nullptr,Error)))return true;
            Test->TestEqual(TEXT("Maximum output has every pixel"),S.Pixels.Num(),4096*4096);
            Test->TestTrue(TEXT("Large capture leaves field, camera and live capture unchanged"),Scene->PresentedField()==Field&&
                StudioView::CameraEquals(Scene->SavedCameraState(),Camera)&&Scene->GetCaptureCount()==Capture);
            Sample(TEXT("pixels-owned"));
            if(!Test->TestTrue(TEXT("Encode and atomically replace actual large PNG"),Export.Start(MoveTemp(S),Root/TEXT("volume-4096.png"))))return true;
            Phase=9;break;
        }
        case 9:
        {
            const auto Result=Export.Poll();if(!Result.IsSet())return false;
            if(!Test->TestTrue(TEXT("Large export completes successfully"),Result->bSuccess&&!Result->bCancelled))return true;
            ++Iteration;Sample(TEXT("export-complete"));
            if(Iteration==2||Iteration==8)
            {
                FlushRenderingCommands();CollectGarbage(RF_NoFlags,true);FlushRenderingCommands();
                SettledAt=Now;Phase=10;
            }
            else Phase=8;
            break;
        }
        case 10:
        {
            // Allow deferred Metal frees to age through several real frames.
            if(Now-SettledAt<.75)return false;
            const auto Current=Sample(Iteration==2?TEXT("warm-retained"):TEXT("final-retained"));
            Test->TestEqual(TEXT("Only the live render target survives collection"),Current.Targets,1);
            Test->TestEqual(TEXT("Only the live camera capture survives collection"),Current.Captures,1);
            if(Iteration==2){Warm=Current;Phase=8;break;}
            Test->TestTrue(TEXT("Repeated exports retain at most 192 MiB above warm process footprint"),Current.Footprint<=Warm.Footprint+192LL*1024*1024);
            Test->TestTrue(TEXT("Repeated exports retain at most 128 MiB above warm Metal allocations"),Current.Device>=0&&Current.Device<=Warm.Device+128LL*1024*1024);
            if(Warm.RHICount>0&&Current.RHICount>0)
                Test->TestTrue(TEXT("Repeated exports retain at most 128 MiB above warm tracked RHI resources"),Current.RHIBytes<=Warm.RHIBytes+128LL*1024*1024);
            else Test->AddInfo(TEXT("RHI tracking unavailable; native Metal allocations provide the GPU retention evidence."));
            Test->TestTrue(TEXT("Resource test restores original project"),M.RequestProjectOpen(Root/TEXT("original.lbms")));Phase=11;break;
        }
        case 11:
            Test->TestEqual(TEXT("Original document restored after exports"),M.Project.Id,Original.Id);return true;
        }
        return false;
    }
private:
    struct FView {FString Name;FStudioCameraState Camera;bool bPopulated=true;};
    struct FResources {int64 Footprint=0,Device=0,RHIBytes=0;int32 RHICount=0,Targets=0,Captures=0;};
    void BuildViews(const TCHAR* Prefix,bool bVolume)
    {
        Views.Reset();ViewIndex=0;const FString P=Prefix;auto Base=Scene->SavedCameraState();const auto Bounds=Scene->GetRenderedFlowBounds();
        Base.bOrthographic=false;Base.bDepthClipping=false;
        double Near=MAX_dbl,Far=0;const FVector Forward=Base.Orientation.GetForwardVector();
        for(int32 I=0;I<8;++I)
        {const FVector Point(I&1?Bounds.Max.X:Bounds.Min.X,I&2?Bounds.Max.Y:Bounds.Min.Y,I&4?Bounds.Max.Z:Bounds.Min.Z);
            const double Depth=FVector::DotProduct(Point-Base.Position,Forward);Near=FMath::Min(Near,Depth);Far=FMath::Max(Far,Depth);}
        Views.Add({P+TEXT("-perspective"),Base});auto Ortho=Base;Ortho.bOrthographic=true;
        Views.Add({P+TEXT("-orthographic"),Ortho});
        auto Clipped=Base;Clipped.bDepthClipping=true;Clipped.NearClipMeters=.0001;Clipped.FarClipMeters=(Near+Far)*.5;
        Views.Add({P+TEXT("-perspective-depth"),Clipped});Clipped.bOrthographic=true;
        Views.Add({P+TEXT("-orthographic-depth"),Clipped});
        Clipped.FarClipMeters=Near*.5;Views.Add({P+TEXT("-fully-clipped"),Clipped,false});
        if(bVolume)
        {
            auto Inside=Base;Inside.Position=Bounds.Min+Bounds.GetSize()*FVector(.65,.5,.55);Inside.Orientation=FQuat::Identity;
            Inside.Focus=Inside.Position+FVector(Inside.OrbitDistance,0,0);Inside.bDepthClipping=false;
            Views.Add({P+TEXT("-inside-perspective"),Inside});Inside.bOrthographic=true;Inside.OrthoWidth=Bounds.GetSize().Z*.75;
            Views.Add({P+TEXT("-inside-orthographic"),Inside});
        }
        Scene->RestoreCamera(Views[0].Camera,TEXT("Snapshot projection check"));
    }
    void VerifyView(const FView& View)
    {
        TArray<FColor> Before,After;const auto* Target=Scene->GetRenderTarget();const auto Camera=Scene->SavedCameraState();const uint64 Capture=Scene->GetCaptureCount();
        if(!Test->TestTrue(TEXT("Read live scientific pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Before)))return;
        FStudioSnapshot S;S.Options.Size=Scene->PresentedViewportSize();S.Options.bAnnotations=false;S.Options.bLegend=false;S.Options.bFrameInfo=false;
        FString Error;if(!Test->TestTrue(*View.Name,Scene->CaptureSnapshot(S,nullptr,Error)))return;
        Test->TestTrue(TEXT("Snapshot freezes all current display settings"),StudioView::DisplayEquals(S.DisplaySettings,static_cast<const FStudioViewSettings&>(*Scene->Model)));
        Test->TestTrue(TEXT("Snapshot has the exact rendered flow bounds"),S.FlowBounds==Scene->GetRenderedFlowBounds());
        Test->TestEqual(TEXT("Metadata distinguishes active 3D volume renderer from requested display"),S.bVolumeRendererActive,View.Name.StartsWith(TEXT("volume-")));
        Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(After);
        Test->TestTrue(TEXT("Projection export leaves the complete live image and camera untouched"),Before==After&&Scene->GetRenderTarget()==Target&&
            Scene->GetCaptureCount()==Capture&&StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
        if(!Test->TestEqual(TEXT("Native output pixel count matches"),S.Pixels.Num(),Before.Num()))return;
        double Difference=0;int32 Colored=0;
        for(int32 I=0;I<Before.Num();++I)
        {
            Difference+=Delta(Before[I],S.Pixels[I]);const auto& C=S.Pixels[I];
            if(FMath::Max3(C.R,C.G,C.B)>12&&FMath::Max3(C.R,C.G,C.B)-FMath::Min3(C.R,C.G,C.B)>4)++Colored;
        }
        const double Mean=Difference/(3.*Before.Num());
        Test->AddInfo(FString::Printf(TEXT("%s: native mean RGB error %.9g; colored pixels %d/%d"),*View.Name,Mean,Colored,Before.Num()));
        Test->TestTrue(TEXT("Native snapshot matches live scientific colors"),Mean<2.);
        Test->TestTrue(TEXT("Projection contains the expected scientific coverage"),View.bPopulated?Colored>Before.Num()/500:Colored==0);
        TArray64<uint8> PNG;Test->TestTrue(TEXT("Encode native projection evidence"),StudioSnapshot::Encode(S,PNG,Error));
        Test->TestTrue(TEXT("Write native projection evidence"),StudioFileDialog::WriteAtomicBytes(Root/(View.Name+TEXT(".png")),PNG,Error));
        Test->TestTrue(TEXT("Write live projection reference"),Scene->Snapshot(Root/(View.Name+TEXT("-live.png"))));
        // Remove exactly 64 pixels from each horizontal side. Every exported
        // pixel then has an independently known live pixel coordinate.
        S.Options.Size.X-=128;
        if(!Test->TestTrue(TEXT("Render integer-pixel centered crop"),Scene->CaptureSnapshot(S,nullptr,Error)))return;
        Difference=0;
        for(int32 Y=0;Y<S.Options.Size.Y;++Y)for(int32 X=0;X<S.Options.Size.X;++X)
            Difference+=Delta(S.Pixels[Y*S.Options.Size.X+X],Before[Y*S.SourceSize.X+X+64]);
        const double CropMean=Difference/(3.*S.Pixels.Num());
        Test->AddInfo(FString::Printf(TEXT("%s: centered crop mean RGB error %.9g"),*View.Name,CropMean));
        Test->TestTrue(TEXT("Crop preserves scientific coordinates without stretching"),CropMean<2.);
    }
    static int32 Delta(const FColor& A,const FColor& B)
    {return FMath::Abs(int32(A.R)-B.R)+FMath::Abs(int32(A.G)-B.G)+FMath::Abs(int32(A.B)-B.B);}
    FResources Sample(const TCHAR* Stage)
    {
        FResources R;R.Footprint=FPlatformMemory::GetStats().UsedPhysical;R.Device=StudioPlatformDiagnostics::DeviceAllocatedBytes();
#if RHI_ENABLE_RESOURCE_INFO
        TArray<TSharedPtr<FRHIResourceStats>> Stats;RHIGetTrackedResourceStats(Stats);R.RHICount=Stats.Num();for(const auto& S:Stats)R.RHIBytes+=S->SizeInBytes;
#endif
        for(TObjectIterator<UTextureRenderTarget2D> It;It;++It)if(It->GetOuter()==Scene.Get())++R.Targets;
        for(TObjectIterator<USceneCaptureComponent2D> It;It;++It)if(It->GetOuter()==Scene.Get())++R.Captures;
        const FString Line=FString::Printf(TEXT("%s,%d,%lld,%lld,%lld,%d,%d,%d,%llu\n"),Stage,Iteration,R.Footprint,R.Device,R.RHIBytes,R.RHICount,R.Targets,R.Captures,Scene->GetCaptureCount());
        Test->TestTrue(TEXT("Append snapshot resource sample"),FFileHelper::SaveStringToFile(Line,*(Root/TEXT("resources.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&IFileManager::Get(),FILEWRITE_Append));
        Test->AddInfo(Line.TrimEnd());return R;
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioProject Original;
    FStudioSnapshotExportTask Export;FStudioCameraState Outside;TArray<FView> Views;FResources Warm;
    FString Root;int32 Phase=0,ViewIndex=0,Iteration=0;double Started=0,SettledAt=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSnapshotRenderingTest,"Studio.SnapshotRendering.RecordedProjectionsAndResources",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioSnapshotRenderingTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioSnapshotRenderingCommand(this));return true;}
#endif
