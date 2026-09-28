#include "StudioSurfaceRenderData.h"
#include "StudioScene.h"
#include "EngineUtils.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/GameViewportClient.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Crc.h"
#include "HAL/FileManager.h"
#include "Math/Vector2DHalf.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FStudioRecordingLoadResult SurfaceRenderFixture()
{
    auto Raw=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    return Raw.Reference.IsSet()?StudioRecordings::ImportReconstruction(*Raw.Reference,
        FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture/reconstruction.json"),0,{}):Raw;
}
constexpr auto SurfaceModelFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioScalarTransport,"Studio.SurfaceRendering.SourceScalarTransport",SurfaceModelFlags)
bool FStudioScalarTransport::RunTest(const FString&)
{
    const auto Loaded=SurfaceRenderFixture();if(!TestTrue(*Loaded.Error,Loaded.Source.IsValid()))return false;
    const auto Field=Loaded.Source->CaptureViewField(0,TEXT("pressure"),false);
    const auto Frame=Field->OriginalPoints();const auto* Values=Frame->FindValues(TEXT("pressure"));
    const auto Mapping=FStudioColorMapping{0,true,-10,10};
    const auto Data=StudioSurfaceRendering::Build(*Frame,*Field->Reconstruction(),TEXT("pressure"),Mapping);
    if(!TestTrue(*Data.Error,Data.Error.IsEmpty()))return false;
    TestEqual(TEXT("One GPU vertex per unchanged source row"),Data.Vertices.Num(),Values->Num());
    TestEqual(TEXT("Every verified fluid triangle retained"),Data.Indices.Num(),37188*3);
    int32 Outside=0;double MaxRelativeError=0;
    for(int32 I=0;I<Values->Num();++I)
    {
        const double Expected=((*Values)[I]+10.)/20.;
        MaxRelativeError=FMath::Max(MaxRelativeError,FMath::Abs(Data.Scalars[I]-Expected)/FMath::Max(1.,FMath::Abs(Expected)));
        if(Data.Scalars[I]<0||Data.Scalars[I]>1)++Outside;
        const auto& UV=Data.TextureCoordinates[I];
        // Half UV storage must address exactly this source row, including row
        // transitions and the last texel. No neighbouring scalar can bleed in.
        const FVector2DHalf Encoded(UV);const FVector2D Half=Encoded;
        if(Half!=UV||FMath::FloorToInt(Half.X*Data.TextureSize.X)!=I%Data.TextureSize.X||
            FMath::FloorToInt(Half.Y*Data.TextureSize.Y)!=I/Data.TextureSize.X)
        {AddError(TEXT("Half UV coordinates changed source-row identity"));return false;}
    }
    TestTrue(TEXT("Out-of-range source scalars stay unclamped before interpolation"),Outside>1000);
    TestTrue(TEXT("R32 transport has bounded relative float error"),MaxRelativeError<1.e-7);
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestTrue(TEXT("Cancelled upload owns no scalar buffers"),StudioSurfaceRendering::Build(*Frame,*Field->Reconstruction(),TEXT("pressure"),Mapping,Cancel).Scalars.IsEmpty());
    TestFalse(TEXT("Missing scientific field rejected"),StudioSurfaceRendering::Build(*Frame,*Field->Reconstruction(),TEXT("density"),Mapping).Error.IsEmpty());
    auto Wrong=*Field->Reconstruction();
    const auto OtherGeometry=MakeShared<FStudioPointGeometry,ESPMode::ThreadSafe>(*Frame->Geometry);
    const auto OtherSurface=FStudioPlanarSurface::Create(OtherGeometry,Field->Reconstruction()->Surface->Triangles());
    if(!TestTrue(*OtherSurface.Error,OtherSurface.Surface.IsValid()))return false;
    Wrong.Surface=OtherSurface.Surface;
    TestFalse(TEXT("Unbound geometry copy rejected"),StudioSurfaceRendering::Build(*Frame,Wrong,TEXT("pressure"),Mapping).Error.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceViewState,"Studio.SurfaceRendering.ViewPersistenceAndMigration",SurfaceModelFlags)
bool FStudioSurfaceViewState::RunTest(const FString&)
{
    FStudioProject P;P.View.bReconstructedSurface=false;FStudioProject Read;FString Error;
    TestTrue(TEXT("Version 10 surface choice reads"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Read,Error));
    TestFalse(TEXT("Original-point choice survives reopening"),Read.View.bReconstructedSurface);
    TSharedPtr<FJsonObject> Root;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),Root);
    Root->GetObjectField(TEXT("view"))->RemoveField(TEXT("reconstructedSurface"));
    auto Encode=[&]{FString S;FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&S));return S;};
    TestFalse(TEXT("Version 10 refuses missing display state"),StudioProjectIO::Parse(Encode(),Read,Error));
    Root->SetNumberField(TEXT("version"),9);
    TestTrue(TEXT("Version 9 migrates"),StudioProjectIO::Parse(Encode(),Read,Error));
    TestTrue(TEXT("Explicitly attached reconstruction defaults to surface display"),Read.View.bReconstructedSurface);
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/SurfaceViewHistory")/FGuid::NewGuid().ToString());
    const auto Before=M.InspectionState();const int32 Frame=M.SelectedFrame;
    TestTrue(TEXT("Representation is a view edit"),M.EditView(TEXT("Original points"),[](auto& S){S.Display.bReconstructedSurface=false;}));
    TestFalse(TEXT("Representation choice changes"),M.bReconstructedSurface);
    TestTrue(TEXT("Representation edit is undoable"),M.UndoView());
    TestTrue(TEXT("Undo restores exact inspection state"),M.InspectionState().Equals(Before));
    TestTrue(TEXT("Representation edit is redoable"),M.RedoView());
    TestFalse(TEXT("Redo restores original-point choice"),M.bReconstructedSurface);
    TestEqual(TEXT("Representation leaves source frame unchanged"),M.SelectedFrame,Frame);
    return true;
}

class FStudioSurfacePixelsCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioSurfacePixelsCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Start)Start=Now;
        if(Test->HasAnyErrors()||Now-Start>45){if(!Test->HasAnyErrors())Test->AddError(TEXT("Surface rendering acceptance timed out"));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        if(Phase==0)
        {
            const auto Loaded=SurfaceRenderFixture();if(!Test->TestTrue(*Loaded.Error,Loaded.Source.IsValid()))return true;
            M.NewProject(TEXT("Reconstructed scalar surface"));auto P=M.SnapshotProject();
            P.Dataset=Loaded.Source->Descriptor().Id;P.Recordings={*Loaded.Reference};P.SelectedFrame=0;
            // This readback measures the surface alone. Recorded streamlines
            // now work on reconstructed point data and can cover a sample pixel
            // with their own segment color, rather than the underlying scalar.
            P.View.bReconstructedSurface=true;P.View.bVectors=false;P.View.bStreamlines=false;P.View.bMesh=false;
            P.View.ScalarField=TEXT("pressure");
            const auto B=Loaded.Source->Descriptor().DisplayBounds;
            P.Camera.Focus=B.GetCenter();P.Camera.Focus.Y=0;
            P.Camera.Position=P.Camera.Focus+FVector(0,1,0);P.Camera.Orientation=FRotator(0,-90,0).Quaternion();
            P.Camera.bOrthographic=true;P.Camera.OrthoWidth=B.GetSize().X*1.1;P.Camera.OrbitDistance=1;
            const FString Path=FPaths::ProjectSavedDir()/TEXT("Automation/surface-pixels.lbms");FString Error;
            if(!Test->TestTrue(TEXT("Save original-source reconstruction test project"),StudioProjectIO::Save(Path,P,Error))||
                !Test->TestTrue(TEXT("Load real source and attached topology"),M.LoadProject(Path)))return true;
            M.Navigate(EStudioWorkspace::Solve);Phase=1;return false;
        }
        if(!Scene->HasCurrentFrame())return false;
        if(Phase>=1&&Phase<=3)
        {
            VerifyPixels();
            const FString Path=FPaths::ProjectSavedDir()/TEXT("Automation/SurfacePixels")/FString::Printf(TEXT("palette-%d.png"),Phase-1);
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
            Test->TestTrue(TEXT("Save actual scalar surface pixels"),Scene->Snapshot(Path));
            if(Phase<3)
            {
                const int32 Palette=Phase;
                M.EditView(TEXT("Surface palette"),[&](auto& S)
                {FStudioScalarStyle Style;Style.Dataset=M.Project.Dataset;Style.Field=TEXT("pressure");Style.Palette=Palette;Style.bManualRange=true;Style.Minimum=-10;Style.Maximum=10;S.Display.ScalarStyles={Style};});
                ++Phase;return false;
            }
            BeforeFrameHash=LastPixelHash;M.Scrub(1.);Phase=4;return false;
        }
        if(Phase==4)
        {
            VerifyPixels();
            Test->TestNotEqual(TEXT("Changing the original frame updates actual scalar pixels"),LastPixelHash,BeforeFrameHash);
            Test->TestEqual(TEXT("Last original source frame is presented"),Scene->PresentedFrame().Index,M.Solver->Descriptor().Frames.Last().Index);
            M.EditView(TEXT("Original points"),[](auto& S){S.Display.bReconstructedSurface=false;});Phase=5;return false;
        }
        if(Phase==5)
        {
            Test->TestEqual(TEXT("Point mode releases scalar texture ownership"),Scene->ResourceStats().ScalarTextureBytes,int64(0));
            Test->TestTrue(TEXT("Original-point geometry returns"),Scene->ResourceStats().Vertices>18706*18);
            M.UndoView();Phase=6;return false;
        }
        Test->TestTrue(TEXT("Undo restores surface texture"),Scene->ResourceStats().ScalarTextureBytes>0);
        return true;
    }
private:
    void VerifyPixels()
    {
        Test->TestEqual(TEXT("Surface pixel audit has no streamline overlay"),Scene->PresentedStreams().Segments,0);
        const auto& M=*Scene->Model;const auto Field=M.Solver->CaptureViewField(M.SelectedFrame,TEXT("pressure"),false);
        const auto Points=Field->OriginalPoints();const auto* Values=Points->FindValues(TEXT("pressure"));
        const auto Surface=Field->Reconstruction();
        TArray<FColor> Pixels;FReadSurfaceDataFlags Flags(RCM_UNorm);Flags.SetLinearToGamma(false);
        if(!Test->TestTrue(TEXT("Read actual surface GPU pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags)))return;
        LastPixelHash=FCrc::MemCrc32(Pixels.GetData(),Pixels.Num()*sizeof(FColor));
        const int32 W=Scene->GetRenderTarget()->SizeX,H=Scene->GetRenderTarget()->SizeY;
        const auto Camera=Scene->PresentedCamera();const double Width=Camera.OrthoWidth,Height=Width*H/W;
        int32 Compared=0,Wrong=0,Holes=0;int32 MaximumError=0;FString First;
        for(int32 Y=H/10;Y<H*9/10;Y+=FMath::Max(1,H/17))for(int32 X=W/10;X<W*9/10;X+=FMath::Max(1,W/29))
        {
            const FVector2D P(Camera.Focus.X+((X+.5)/W-.5)*Width,Camera.Focus.Z+(.5-(Y+.5)/H)*Height);
            bool Found=false;double ExpectedValue=0;
            for(const auto& T:Surface->Surface->Triangles())
            {
                const auto& VA=Points->Geometry->Positions[T.X];const auto& VB=Points->Geometry->Positions[T.Y];const auto& VC=Points->Geometry->Positions[T.Z];
                const FVector2D A(VA.X,VA.Y),B(VB.X,VB.Y),C(VC.X,VC.Y);
                const double Det=(B.Y-C.Y)*(A.X-C.X)+(C.X-B.X)*(A.Y-C.Y);
                const double U=((B.Y-C.Y)*(P.X-C.X)+(C.X-B.X)*(P.Y-C.Y))/Det;
                const double V=((C.Y-A.Y)*(P.X-C.X)+(A.X-C.X)*(P.Y-C.Y))/Det;
                const double Z=1-U-V;
                if(U<0||V<0||Z<0)continue;
                Found=true;if(FMath::Min3(U,V,Z)<.08)break;
                ExpectedValue=U*(*Values)[T.X]+V*(*Values)[T.Y]+Z*(*Values)[T.Z];
                // The PF_B8G8R8A8 viewport explicitly uses sRGB output. ReadPixels
                // preserves those encoded bytes; encode the linear palette once.
                const FColor Expected=StudioColor::Map(ExpectedValue,Scene->PresentedColorMapping()).ToFColor(true);
                const auto Actual=Pixels[Y*W+X];
                const int32 Error=FMath::Max3(FMath::Abs(int32(Actual.R)-Expected.R),FMath::Abs(int32(Actual.G)-Expected.G),FMath::Abs(int32(Actual.B)-Expected.B));
                MaximumError=FMath::Max(MaximumError,Error);++Compared;
                if(Error>4){++Wrong;if(First.IsEmpty())First=FString::Printf(TEXT("pixel=%d,%d scalar=%.9g actual=%s expected=%s"),X,Y,ExpectedValue,*Actual.ToString(),*Expected.ToString());}
                break;
            }
            if(!Found&&P.X>.015&&P.X<.075&&FMath::Abs(P.Y)<.002)
            {++Holes;const auto Actual=Pixels[Y*W+X];Test->TestTrue(TEXT("Inferred solid is not filled by scalar triangles"),Actual.R<10&&Actual.G<10&&Actual.B<10);}
        }
        // Probe the known solid explicitly: a coarse sampling grid can miss
        // this thin section at another window size. Confirm the actual pixel
        // center is inside the independently supplied boundary polygon first.
        const int32 HoleX=FMath::FloorToInt(((.04-Camera.Focus.X)/Width+.5)*W);
        const int32 HoleY=FMath::FloorToInt((.5+Camera.Focus.Z/Height)*H);
        const FVector2D HoleP(Camera.Focus.X+((HoleX+.5)/W-.5)*Width,Camera.Focus.Z+(.5-(HoleY+.5)/H)*Height);
        bool bInside=false;
        for(int32 I=0,J=Surface->Boundary.Num()-1;I<Surface->Boundary.Num();J=I++)
        {
            const auto& A=Surface->Boundary[I];const auto& B=Surface->Boundary[J];
            if((A.Y>HoleP.Y)!=(B.Y>HoleP.Y)&&HoleP.X<(B.X-A.X)*(HoleP.Y-A.Y)/(B.Y-A.Y)+A.X)bInside=!bInside;
        }
        if(Test->TestTrue(TEXT("Targeted pixel lies inside the inferred solid"),bInside&&HoleX>=0&&HoleX<W&&HoleY>=0&&HoleY<H))
        {
            const auto Actual=Pixels[HoleY*W+HoleX];++Holes;
            Test->TestTrue(TEXT("Targeted solid pixel contains no scalar field"),Actual.R<10&&Actual.G<10&&Actual.B<10);
        }
        Test->AddInfo(FString::Printf(TEXT("Scalar GPU palette %d frame %d: %d independent samples, %d mismatch, max byte error %d, %d hole samples; %s"),Scene->PresentedColorMapping().Palette,M.SelectedFrame,Compared,Wrong,MaximumError,Holes,*First));
        Test->TestTrue(TEXT("Independent triangle-interior samples cover rendered surface"),Compared>50);
        Test->TestEqual(TEXT("Pixels interpolate scalar before applying palette and clamp"),Wrong,0);
        Test->TestTrue(TEXT("Empty solid is covered by GPU pixel assertions"),Holes>0);
        Test->TestTrue(TEXT("Bounded reusable scalar texture"),Scene->ResourceStats().ScalarTextureBytes>0&&Scene->ResourceStats().ScalarTextureBytes<2*1024*1024);
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;double Start=0;int32 Phase=0;uint32 LastPixelHash=0,BeforeFrameHash=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfacePixels,"Studio.SurfaceRendering.OriginalDataPixels",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioSurfacePixels::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioSurfacePixelsCommand(this));return true;}
#endif
