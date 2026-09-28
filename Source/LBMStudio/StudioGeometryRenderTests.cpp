#include "StudioScene.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioGeometryRenderCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioGeometryRenderCommand(FAutomationTestBase* T):Test(T){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(Start==0)Start=Now;
        if(Now-Start>35){Test->AddError(TEXT("Geometry rendering acceptance timed out"));return true;}
        if(Test->HasAnyErrors())return true;
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;auto& M=*Scene->Model;
        switch(Phase)
        {
        case 0:
            M.Navigate(EStudioWorkspace::Solve);if(!Scene->HasCurrentFrame())return false;if(M.State==EStudioRunState::Running)M.Pause();
            Camera=M.Project.Camera;Frame=M.SelectedFrame;Dataset=M.Project.Dataset;Case=M.Project.Draft;
            Path=FPaths::ProjectSavedDir()/TEXT("Automation/import-preview.obj");IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
            FFileHelper::SaveStringToFile(TEXT("# Structural tetrahedron, no CFD fields\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n"),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Test->TestTrue(TEXT("Start real geometry parser"),M.RequestGeometryImport(Path));M.Navigate(EStudioWorkspace::Geometry);Phase=1;break;
        case 1:
            if(M.IsReadingGeometry())return false;
            Test->TestTrue(TEXT("Mesh loaded"),M.GeometrySource.IsValid());Phase=6;break;
        case 6:
            if(!Scene->HasGeometryPreview())return false;
            {
                TArray<FColor> Pixels;Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
                int32 Changed=0;for(const auto& P:Pixels)if(P!=Pixels[0])++Changed;
                Test->TestTrue(TEXT("Shape is visible before units are chosen"),Changed>Pixels.Num()/100);
                Test->TestEqual(TEXT("Unit selection still required"),M.ImportOptions.MetersPerUnit,0.);
                Test->TestFalse(TEXT("Unknown units cannot commit"),M.CommitGeometryImport());
                M.ImportOptions.MetersPerUnit=1;M.GeometryOptionsChanged();Phase=2;
            }break;
        case 2:
            if(!Scene->HasGeometryPreview())return false;
            {
                TArray<FColor> Pixels;Test->TestTrue(TEXT("Preview GPU readback succeeds"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels));
                int32 Changed=0;for(const auto& P:Pixels)if(P!=Pixels[0])++Changed;
                Test->TestTrue(TEXT("Imported mesh produces a populated render"),Changed>Pixels.Num()/100);
                Test->TestFalse(TEXT("Geometry preview cannot masquerade as recorded frame"),Scene->HasCurrentFrame());
                Scene->Orbit(40,20);Test->TestTrue(TEXT("Preview camera does not edit Solve camera"),StudioView::CameraEquals(M.Project.Camera,Camera));
                Test->TestTrue(TEXT("Save uses retained Solve camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
                Test->TestTrue(TEXT("Commit preview"),M.CommitGeometryImport());
                Test->TestTrue(TEXT("Preview gestures did not alter recorded frame"),M.SelectedFrame==Frame&&M.Project.Dataset==Dataset);
                Phase=3;PhaseTime=Now;
            }break;
        case 3:
            if(!Scene->HasGeometryPreview()||Now-PhaseTime<1)return false;
            Captures=Scene->GetCaptureCount();PhaseTime=Now;Phase=4;break;
        case 4:
            if(Now-PhaseTime<2)return false;
            Test->TestEqual(TEXT("Unchanged mesh preview stops captures"),Scene->GetCaptureCount(),Captures);
            M.Navigate(EStudioWorkspace::Solve);Phase=5;break;
        case 5:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Return restores exact Solve camera"),StudioView::CameraEquals(Scene->CameraState(),Camera));
            Test->TestEqual(TEXT("Return restores recording identity"),Scene->PresentedDatasetId(),Dataset);
            Test->TestEqual(TEXT("Return restores recorded frame"),M.SelectedFrame,Frame);
            Test->TestTrue(TEXT("Imported mesh remains undoable after rendering"),M.UndoCase());
            Test->TestEqual(TEXT("Undo restores original geometry count"),M.Project.Draft.Geometry.Num(),Case.Geometry.Num());
            return true;
        }
        return false;
    }
private:
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;double Start=0,PhaseTime=0;int32 Phase=0,Frame=0;uint64 Captures=0;
    FString Path,Dataset;FStudioCameraState Camera;FStudioCaseDraft Case;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioGeometryRenderTest,"Studio.Rendering.GeometryPreviewAndReturn",EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioGeometryRenderTest::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioGeometryRenderCommand(this));return true;}

// Identity alone does not prove that a recreated render proxy reached the GPU.
// Exercise the empty Geometry route, then compare the actual settled flow pixels.
class FStudioEmptyGeometryReturnCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioEmptyGeometryReturnCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()||Now-Started>30)
        {if(!Test->HasAnyErrors())Test->AddError(TEXT("Empty Geometry return timed out"));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        switch(Phase)
        {
        case 0:
            M.NewProject(TEXT("Empty geometry return"));M.Navigate(EStudioWorkspace::Solve);
            Phase=1;Changed=Now;break;
        case 1:
            if(!Scene->HasCurrentFrame()||Now-Changed<.5)return false;
            Camera=M.Project.Camera;Frame=M.SelectedFrame;Baseline=ReadPixels();
            Test->TestTrue(TEXT("Baseline contains visible flow"),Populated);
            Size=FIntPoint(Scene->GetRenderTarget()->SizeX,Scene->GetRenderTarget()->SizeY);
            M.Navigate(EStudioWorkspace::Projects);Phase=2;Changed=Now;break;
        case 2:
            if(Now-Changed<.15)return false;
            M.Navigate(EStudioWorkspace::Dashboard);Phase=3;Changed=Now;break;
        case 3:
            if(Now-Changed<.15)return false;
            M.CancelGeometryImport();M.Navigate(EStudioWorkspace::Geometry);Phase=4;Changed=Now;break;
        case 4:
            if(Now-Changed<6.)return false;
            Test->TestFalse(TEXT("No selected mesh in Geometry"),M.GeometrySource.IsValid());
            Test->TestFalse(TEXT("Empty Geometry cannot claim a source frame"),Scene->HasCurrentFrame());
            M.Navigate(EStudioWorkspace::Solve);Phase=5;Changed=Now;break;
        case 5:
            if(!Scene->HasCurrentFrame()||Now-Changed<.5)return false;
            {
                const uint32 Returned=ReadPixels();
                Test->TestTrue(TEXT("Returning from empty Geometry restores visible flow pixels"),Populated);
                Test->TestEqual(TEXT("Return restores viewport width"),Scene->GetRenderTarget()->SizeX,Size.X);
                Test->TestEqual(TEXT("Return restores viewport height"),Scene->GetRenderTarget()->SizeY,Size.Y);
                Test->TestEqual(TEXT("Return restores identical paused field pixels"),Returned,Baseline);
                Test->TestTrue(TEXT("Return preserves exact saved camera"),StudioView::CameraEquals(M.Project.Camera,Camera));
                Test->TestEqual(TEXT("Return preserves selected source frame"),M.SelectedFrame,Frame);
                const auto Stats=Scene->ResourceStats();
                AddInfo(FString::Printf(TEXT("Empty Geometry return %d: populated=%d, sections=%d, vertices=%lld, target=%dx%d, hash=%u/%u"),
                    Repeat,Populated,Stats.Sections,Stats.Vertices,Size.X,Size.Y,Returned,Baseline));
                if(++Repeat<3){M.Navigate(EStudioWorkspace::Geometry);Phase=4;Changed=Now;}
                else return true;
            }break;
        }
        return false;
    }
private:
    void AddInfo(const FString& Message){Test->AddInfo(Message);}
    uint32 ReadPixels()
    {
        TArray<FColor> Pixels;Populated=false;
        if(!Test->TestTrue(TEXT("Actual flow target readback succeeds"),
            Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels)&&!Pixels.IsEmpty()))return 0;
        int32 Different=0;
        for(auto& P:Pixels){P.A=255;if(P.R!=Pixels[0].R||P.G!=Pixels[0].G||P.B!=Pixels[0].B)++Different;}
        Populated=Different>Pixels.Num()/100;
        return FCrc::MemCrc32(Pixels.GetData(),Pixels.Num()*sizeof(FColor));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;
    double Started=0,Changed=0;int32 Phase=0,Repeat=0,Frame=0;uint32 Baseline=0;bool Populated=false;
    FIntPoint Size;FStudioCameraState Camera;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioEmptyGeometryReturn,"Studio.Rendering.EmptyGeometryReturnPixels",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioEmptyGeometryReturn::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioEmptyGeometryReturnCommand(this));return true;}
#endif
