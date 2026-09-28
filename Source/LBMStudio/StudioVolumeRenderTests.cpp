#include "StudioScene.h"
#include "StudioVolume.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Uses only the separately converted, attributed original 3D subset. */
class FStudioVolumeViewportCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioVolumeViewportCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Start)Start=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Start>180){Test->AddError(FString::Printf(TEXT("Volume viewport timed out in phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        const bool Ready=!M.IsProjectOpenPending()&&!M.IsRecordingLoadPending()&&Scene->HasCurrentFrame();
        switch(Phase)
        {
        case 0:
        {
            if(!Ready)return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/VolumeViewport");IFileManager::Get().MakeDirectory(*Root,true);
            Original=M.SnapshotProject();FString Error;
            if(!Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Root/TEXT("original.lbms"),Original,Error)))return true;
            M.NewProject(TEXT("Authentic 3D volume acceptance"));
            if(!Test->TestTrue(TEXT("Import original 3D fixture"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json"))))return true;
            Phase=1;break;
        }
        case 1:
            if(!Ready)return false;
            Test->TestEqual(TEXT("Actual 3D source dimensions"),M.Solver->Descriptor().SpatialDimensions,3);
            Test->TestTrue(TEXT("Source recording includes genuine time evolution"),M.Frames.Last().Time-M.Frames[0].Time>10.);
            if(!Test->TestTrue(TEXT("Attach verified volume mapping"),M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json"))))return true;
            Phase=2;break;
        case 2:
            if(!Ready)return false;
            if(!Test->TestTrue(TEXT("Volume mapping attached"),M.Solver->VolumeReconstruction().IsValid()))return true;
            M.EditView(TEXT("Isolate volume rendering"),[](auto& S){S.Display.bVolume=true;S.Display.bVectors=false;S.Display.bCutPlane=false;S.Display.bMesh=false;S.Display.bVolumeIsosurface=false;});
            Scene->FitCamera();Phase=3;break;
        case 3:
            if(!Ready)return false;
            Outside=M.Project.Camera;
            FirstCRC=Pixels(TEXT("outside-first.png"));
            Test->TestTrue(TEXT("Volume owns a bounded GPU texture"),Scene->ResourceStats().ScalarTextureBytes>0&&Scene->ResourceStats().ScalarTextureBytes<=StudioVolumes::MaximumVoxels*8LL);
            M.Scrub(1);Phase=4;break;
        case 4:
            if(!Ready)return false;
            Test->TestTrue(TEXT("Original time change changes rendered pixels"),Pixels(TEXT("outside-last.png"))!=FirstCRC);
            Test->TestTrue(TEXT("Displayed source step matches requested frame"),Scene->PresentedFrame().Index==M.Frames.Last().Index);
            {
                const auto B=M.Solver->Descriptor().DisplayBounds;auto Camera=Outside;
                Camera.Position=B.Min+B.GetSize()*FVector(.65,.5,.55);Camera.Orientation=FQuat::Identity;
                Camera.Focus=Camera.Position+FVector(1,0,0)*Camera.OrbitDistance;
                Scene->RestoreCamera(Camera,TEXT("Inside volume"));
            }
            Phase=5;break;
        case 5:
            if(!Ready||!StudioView::CameraEquals(Scene->PresentedCamera(),Scene->CameraState()))return false;
            Pixels(TEXT("inside-perspective.png"));
            {
                auto Camera=M.Project.Camera;Camera.bOrthographic=true;Camera.OrthoWidth=M.Solver->Descriptor().DisplayBounds.GetSize().Z*.75;
                Scene->RestoreCamera(Camera,TEXT("Inside orthographic volume"));
            }
            Phase=6;break;
        case 6:
            if(!Ready||!Scene->PresentedCamera().bOrthographic)return false;
            Pixels(TEXT("inside-orthographic.png"));Scene->RestoreCamera(Outside,TEXT("Restore outside view"));
            M.EditView(TEXT("Clip and recolor volume"),[](auto& S){S.Display.VolumeClipMinimum.X=.5;S.Display.VolumeOpacity=.8;S.Display.VolumeOpacityCurve=FVector(1,.2,0);S.Display.VolumeStepVoxels=.5;});
            Phase=7;break;
        case 7:
        {
            if(!Ready)return false;
            Pixels(TEXT("clipped.png"));
            const auto Range=M.ActiveColorMapping();
            Test->TestTrue(TEXT("Apply custom field colors"),M.SetScalarStyle(3,Range.bManualRange,Range.Minimum,Range.Maximum,
                {FLinearColor(.02,.08,.4),FLinearColor(.8,.1,.03),FLinearColor(.95,.85,.1)}));
            Phase=16;break;
        }
        case 16:
        {
            if(!Ready)return false;
            Pixels(TEXT("custom-colors.png"));
            const auto Range=M.ActiveColorMapping();
            M.EditView(TEXT("Show isosurface"),[&](auto& S){S.Display.bVolume=false;S.Display.bVolumeIsosurface=true;S.Display.VolumeIsovalue=(Range.Minimum+Range.Maximum)*.5;});
            Phase=8;break;
        }
        case 8:
            if(!Ready)return false;
            Pixels(TEXT("isosurface.png"));
            M.EditView(TEXT("Restore volume"),[](auto& S){S.Display.bVolume=true;S.Display.bVolumeIsosurface=false;});
            Phase=9;break;
        case 9:
            if(!Ready)return false;
            Test->TestTrue(TEXT("Save volume view"),M.SaveProject(Root/TEXT("volume.lbms")));Saved=M.SnapshotProject();
            Test->TestTrue(TEXT("Reopen volume view"),M.RequestProjectOpen(Root/TEXT("volume.lbms")));Phase=10;break;
        case 10:
            if(!Ready)return false;
            Test->TestEqual(TEXT("Volume view restored exactly"),StudioProjectIO::Serialize(M.SnapshotProject()),StudioProjectIO::Serialize(Saved));
            Pixels(TEXT("reopened.png"));Idle=Now;IdleCaptures=Scene->GetCaptureCount();Phase=11;break;
        case 11:
            if(Now-Idle<2)return false;
            Test->TestEqual(TEXT("Idle volume produces no new captures"),Scene->GetCaptureCount(),IdleCaptures);
            {
                const auto B=M.Solver->Descriptor().DisplayBounds;const double Length=B.GetSize().X;
                auto Camera=Outside;Camera.bOrthographic=false;Camera.Orientation=FQuat::Identity;
                Camera.Position=B.GetCenter();Camera.Position.X=B.Min.X-Length;
                Camera.OrbitDistance=Length*1.5;Camera.Focus=Camera.Position+FVector(Length*1.5,0,0);
                Camera.bDepthClipping=true;Camera.NearClipMeters=Length*.001;Camera.FarClipMeters=Length*.5;
                Scene->RestoreCamera(Camera,TEXT("Clip whole volume behind far plane"));
            }
            Phase=14;break;
        case 14:
        {
            if(!Ready||!StudioView::CameraEquals(Scene->PresentedCamera(),Scene->CameraState()))return false;
            TArray<FColor> P;
            if(!Test->TestTrue(TEXT("Read camera-clipped volume"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(P))||P.IsEmpty())return true;
            int32 Variation=0;const FColor First=P[0];
            for(const FColor& Pixel:P)Variation=FMath::Max(Variation,FMath::Max3(FMath::Abs(int32(Pixel.R)-First.R),FMath::Abs(int32(Pixel.G)-First.G),FMath::Abs(int32(Pixel.B)-First.B)));
            Test->TestTrue(TEXT("Far plane before domain produces only uniform background"),Variation<=1);
            ClippedBackground=First;
            for(auto& Pixel:P)Pixel.A=255;
            ClippedCRC=FCrc::MemCrc32(P.GetData(),P.Num()*sizeof(FColor));
            Test->TestTrue(TEXT("Save completely clipped volume"),Scene->Snapshot(Root/TEXT("camera-before-volume.png")));
            auto Camera=Scene->CameraState();const double Length=M.Solver->Descriptor().DisplayBounds.GetSize().X;
            Camera.NearClipMeters=Length*1.25;Camera.FarClipMeters=Length*1.75;
            Scene->RestoreCamera(Camera,TEXT("Reveal bounded depth interval"));Phase=15;break;
        }
        case 15:
            if(!Ready||!StudioView::CameraEquals(Scene->PresentedCamera(),Scene->CameraState()))return false;
            Test->TestTrue(TEXT("Changing camera clip planes reveals original volume"),Pixels(TEXT("camera-depth-interval.png"))!=ClippedCRC);
            {
                TArray<FColor> P;Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(P);
                int32 Changed=0;
                for(const auto& C:P)if(FMath::Max3(FMath::Abs(int32(C.R)-ClippedBackground.R),FMath::Abs(int32(C.G)-ClippedBackground.G),FMath::Abs(int32(C.B)-ClippedBackground.B))>3)++Changed;
                Test->TestTrue(TEXT("Revealed volume occupies a substantial area beyond domain lines"),Changed>P.Num()/50);
            }
            {
                const auto B=M.Solver->Descriptor().DisplayBounds;const auto Target=Scene->GetRenderTarget();
                auto Camera=Outside;Camera.bDepthClipping=false;Camera.bOrthographic=true;Camera.Focus=B.GetCenter();
                Camera.OrbitDistance=B.GetSize().GetMax()*2;Camera.Position=Camera.Focus+FVector(0,Camera.OrbitDistance,0);
                Camera.Orientation=(Camera.Focus-Camera.Position).Rotation().Quaternion();
                Camera.OrthoWidth=FMath::Max(B.GetSize().X,B.GetSize().Z*double(Target->SizeX)/Target->SizeY)*1.1;
                Scene->RestoreCamera(Camera,TEXT("Reference midspan section"));
                M.Scrub(.5);M.SetScalarStyle(0,false,M.ActiveScalar().Minimum,M.ActiveScalar().Maximum);
                M.EditView(TEXT("Reference slice"),[&](auto& S){S.Display.bVolume=false;S.Display.bVolumeIsosurface=false;
                    S.Display.bSourcePoints=false;S.Display.bCutPlane=true;S.Display.SliceAxis=1;S.Display.SlicePosition=B.GetCenter().Y;
                    S.Display.VolumeClipMinimum=FVector::ZeroVector;S.Display.VolumeClipMaximum=FVector::OneVector;});
            }
            Phase=17;break;
        case 17:
            if(!Ready||!StudioView::CameraEquals(Scene->PresentedCamera(),Scene->CameraState()))return false;
            Pixels(TEXT("midspan-slice.png"));
            Test->TestTrue(TEXT("Export unchanged original values at the displayed slice time"),M.ExportField(Root/TEXT("midspan-original-points.csv")));
            Test->TestTrue(TEXT("Remove volume without changing source values"),M.RemoveReconstruction());Phase=12;break;
        case 12:
            if(!Ready)return false;
            Test->TestFalse(TEXT("Removal restores original-point source"),M.Solver->VolumeReconstruction().IsValid());
            Test->TestEqual(TEXT("Volume GPU texture released"),Scene->ResourceStats().ScalarTextureBytes,int64(0));
            Test->TestTrue(TEXT("Restore preceding project"),M.RequestProjectOpen(Root/TEXT("original.lbms")));Phase=13;break;
        case 13:
            if(!Ready)return false;
            Test->TestEqual(TEXT("Original document restored"),M.Project.Id,Original.Id);return true;
        }
        return false;
    }
private:
    uint32 Pixels(const TCHAR* Name)
    {
        TArray<FColor> Pixels;
        if(!Test->TestTrue(TEXT("Read actual volume render target"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels)))return 0;
        int32 Visible=0;for(auto& P:Pixels){P.A=255;if(FMath::Max3(P.R,P.G,P.B)>12&&FMath::Max3(P.R,P.G,P.B)-FMath::Min3(P.R,P.G,P.B)>4)++Visible;}
        Test->TestTrue(TEXT("View contains populated scientific rendering"),Visible>Pixels.Num()/200);
        Test->TestTrue(TEXT("Save volume evidence"),Scene->Snapshot(Root/Name));
        return FCrc::MemCrc32(Pixels.GetData(),Pixels.Num()*sizeof(FColor));
    }
    FAutomationTestBase* Test;
    TWeakObjectPtr<AStudioScene> Scene;
    FStudioProject Original,Saved;
    FStudioCameraState Outside;
    FColor ClippedBackground;
    FString Root;
    int32 Phase=0;double Start=0,Idle=0;uint32 FirstCRC=0,ClippedCRC=0;uint64 IdleCaptures=0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVolumeViewport,"Studio.VolumeRendering.AuthenticRecording",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioVolumeViewport::RunTest(const FString&)
{
    if(!IFileManager::Get().FileExists(*(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json"))))
    {AddError(TEXT("The authentic 3D fixture has not been acquired and converted. No synthetic data may substitute for this gate."));return false;}
    ADD_LATENT_AUTOMATION_COMMAND(FStudioVolumeViewportCommand(this));return true;
}
#endif
