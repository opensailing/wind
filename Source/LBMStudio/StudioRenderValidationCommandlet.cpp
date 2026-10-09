#include "StudioRenderValidationCommandlet.h"
#include "StudioScene.h"
#include "StudioSnapshotSource.h"
#include "StudioSnapshot.h"
#include "StudioRecording.h"
#include "StudioVolume.h"
#include "StudioVolumeComponent.h"
#include "StudioPointRecording.h"
#include "StudioHome4Archive.h"
#include "Misc/Base64.h"
#include "ProceduralMeshComponent.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInterface.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/Crc.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "SStudioHelpPanel.h"
#include "SStudioNotifications.h"
#include "StudioWorkspace.h"
#include "GenericPlatform/GenericApplication.h"
#include "Interfaces/ISlateRHIRendererModule.h"
#include "Slate/WidgetRenderer.h"
#include "ImageUtils.h"
#include "StudioHome4SliceFixtures.inl"
#include "StudioHome4GPUFixtures.inl"
#endif

UStudioRenderValidationCommandlet::UStudioRenderValidationCommandlet()
{
    IsClient=true;IsServer=false;IsEditor=true;LogToConsole=true;ShowErrorCount=true;
}

#if WITH_EDITOR
namespace StudioWindowlessRendering
{
const FIntPoint Size(640,360);

class FRun final
{
public:
    TArray<FString> Errors;
    TArray<TSharedPtr<FJsonValue>> Cases;
    TArray<TSharedPtr<FJsonValue>> Checks;
    TArray<TSharedPtr<FJsonValue>> ReviewImages;
    TSharedPtr<FStudioModel> Model;
    AStudioScene* Scene=nullptr;
    UWorld* World=nullptr;
    bool bVisible=true;

    // Explicit design-review opt-in only. Routine renderer validation still
    // writes numerical JSON and no images. These are real offscreen Slate
    // renders, never desktop screenshots or native-window acceptance.
    void ReviewHelp(const FString& Directory,bool bNotifications=false)
    {
        const bool OwnSlate=!FSlateApplication::IsInitialized();
        if(OwnSlate)
            FSlateApplication::InitializeAsStandaloneApplication(
                FModuleManager::LoadModuleChecked<ISlateRHIRendererModule>(TEXT("SlateRHIRenderer")).CreateSlateRHIRenderer(),
                MakeShared<GenericApplication>(nullptr));
        ON_SCOPE_EXIT{FlushRenderingCommands();if(OwnSlate)FSlateApplication::Shutdown();};
        FSlateApplication::Get().GetRenderer()->LoadStyleResources(FCoreStyle::Get());
        auto* Renderer=new FWidgetRenderer(true,true);Renderer->SetApplyColorDeficiencyCorrection(false);
        ON_SCOPE_EXIT{BeginCleanup(Renderer);FlushRenderingCommands();};
        IFileManager::Get().MakeDirectory(*Directory,true);
        const auto CaptureWidget=[&](const FString& Name,const TSharedRef<SWidget>& Widget,FVector2D WidgetSize)
        {
            TStrongObjectPtr<UTextureRenderTarget2D> Target(NewObject<UTextureRenderTarget2D>());
            // Slate applies display gamma itself. A linear target prevents a
            // second hardware gamma conversion from washing out the navy UI.
            Target->InitCustomFormat(int32(WidgetSize.X),int32(WidgetSize.Y),PF_B8G8R8A8,true);
            Target->UpdateResourceImmediate(true);
            // Text auto-wrap and first-use brushes/font atlases settle after
            // the first paint. Root layout also resizes its flow target.
            for(int32 Paint=0;Paint<3;++Paint)
            {Renderer->DrawWidget(Target.Get(),Widget,WidgetSize,0);FlushRenderingCommands();Ready();}
            TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);
            const bool Read=Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            const FString Path=Directory/(Name+TEXT(".png"));TArray64<uint8> PNG;
            if(Read)FImageUtils::PNGCompressImageArray(int32(WidgetSize.X),int32(WidgetSize.Y),Pixels,PNG);
            if(Check((bNotifications?TEXT("notifications.review."):TEXT("help.review."))+Name,
                Read&&!PNG.IsEmpty()&&FFileHelper::SaveArrayToFile(PNG,*Path)))
                ReviewImages.Add(MakeShared<FJsonValueString>(Path));
        };
        if(bNotifications)
        {
            // UI automation messages are explicitly labeled fixtures; they
            // contain no scientific arrays, solver metrics or generated CFD.
            auto Panel=SNew(SStudioNotifications).Model(Model).OnClose_Lambda([]{}).Reveal([](uint64){return true;});
            CaptureWidget(TEXT("notifications-empty"),Panel,FVector2D(500,500));
            Model->AddLog(TEXT("UI automation fixture: imported geometry needs review. Original data retained."),EStudioLogSeverity::Warning);
            Model->AddLog(TEXT("UI automation fixture: project save failed. Choose a writable location and try again."),EStudioLogSeverity::Error);
            Panel=SNew(SStudioNotifications).Model(Model).OnClose_Lambda([]{}).Reveal([](uint64){return true;});
            CaptureWidget(TEXT("notifications-history"),Panel,FVector2D(500,500));
            Model->AddLog(TEXT("UI automation fixture: a new warning arrived while reading."),EStudioLogSeverity::Warning);
            CaptureWidget(TEXT("notifications-arrival"),Panel,FVector2D(500,500));
            Model->MarkNotificationsReadThrough(Model->Notifications().LastSequence());
            Panel=SNew(SStudioNotifications).Model(Model).OnClose_Lambda([]{}).Reveal([](uint64){return true;});
            CaptureWidget(TEXT("notifications-read"),Panel,FVector2D(500,500));
            Model->SetNotificationRead(Model->Notifications().LastSequence(),false);
        }
        else
        {
        const TCHAR* Names[]={TEXT("help-workspace"),TEXT("help-shortcuts"),TEXT("help-diagnostics"),TEXT("help-about")};
        for(int32 I=0;I<4;++I)
            CaptureWidget(Names[I],SNew(SStudioHelpPanel).Model(Model).Scene(Scene).Page(EStudioHelpPage(I))
                .OnResults_Lambda([]{}).OnClose_Lambda([]{}),FVector2D(540,500));
        }
        for(const auto WidgetSize:{FVector2D(1280,720),FVector2D(1320,740)})
            CaptureWidget(FString(bNotifications?TEXT("notifications"):TEXT("help"))+
                FString::Printf(TEXT("-header-%dx%d"),int32(WidgetSize.X),int32(WidgetSize.Y)),
                SNew(SStudioWorkspace).Model(Model).Scene(Scene),WidgetSize);
        Scene->ResizeViewport(Size.X,Size.Y,true);Ready();
    }

    bool Check(const FString& Name,bool Passed,const FString& Detail=FString())
    {
        auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Name);
        Row->SetBoolField(TEXT("passed"),Passed);Row->SetStringField(TEXT("detail"),Detail);
        Checks.Add(MakeShared<FJsonValueObject>(Row));
        if(!Passed){Errors.Add(Name+TEXT(": ")+Detail);UE_LOG(LogTemp,Error,TEXT("%s: %s"),*Name,*Detail);}
        return Passed;
    }
    void Pump()
    {
        ENQUEUE_RENDER_COMMAND(StudioValidationBeginFrame)([](FRHICommandListImmediate&)
        {++GFrameNumberRenderThread;});
        ++GFrameCounter;
        World->SendAllEndOfFrameUpdates();Scene->Tick(0);
        ENQUEUE_RENDER_COMMAND(StudioValidationEndFrame)([](FRHICommandListImmediate& RHICmdList){RHICmdList.EndFrame();});
        FlushRenderingCommands();
    }
    bool Ready()
    {
        const double Deadline=FPlatformTime::Seconds()+120;
        do
        {
            Pump();
            if(Scene->HasCurrentFrame()&&StudioView::CameraEquals(Scene->PresentedCamera(),Scene->CameraState()))return true;
            if(!Scene->bBuilding&&!Model->Notice.IsEmpty())return Check(TEXT("scene.ready"),false,Model->Notice);
            FPlatformProcess::Sleep(.005f);
        }while(FPlatformTime::Seconds()<Deadline&&!IsEngineExitRequested());
        return Check(TEXT("scene.ready"),false,TEXT("Original field or camera did not finish rendering within 120 seconds."));
    }
    TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> Freeze(const TSharedPtr<IStudioSolver,ESPMode::ThreadSafe>& Source,int32 Ordinal,const FString& Scalar=FString(),bool bVelocity=false)
    {
        FString Error;
        auto Frozen=Async(EAsyncExecution::ThreadPool,[&]
        {return FStudioSnapshotSource::CreateView(*Source,Ordinal,Scalar.IsEmpty()?Source->Descriptor().DefaultScalar:Scalar,bVelocity,{},Error);}).Get();
        Check(TEXT("source.freeze"),Frozen.IsValid(),Error);return Frozen;
    }
    bool StartScene(const TSharedPtr<IStudioSolver,ESPMode::ThreadSafe>& Source,int32 Ordinal,const FString& Scalar=FString(),bool bVelocity=false)
    {
        auto Frozen=Freeze(Source,Ordinal,Scalar,bVelocity);if(!Frozen)return false;
        Model=MakeShared<FStudioModel>(Frozen.ToSharedRef());
        Model->bVectors=false;Model->bStreamlines=false;Model->bCutPlane=false;
        Model->bMesh=false;Model->bSourcePoints=false;
        Scene=World->SpawnActor<AStudioScene>();
        if(!Check(TEXT("scene.created"),Scene!=nullptr))return false;
        // No GameMode is assigned to this world: the app workspace and native
        // game viewport must never be instantiated by the validation command.
        Scene->DispatchBeginPlay();Scene->SetViewVisibility([this]{return bVisible;});
        Scene->Initialize(Model.ToSharedRef());Scene->ResizeViewport(Size.X,Size.Y,true);
        FAssetCompilingManager::Get().FinishAllCompilation();
        return Ready();
    }
    bool ChangeFrame(const TSharedPtr<IStudioSolver,ESPMode::ThreadSafe>& Source,int32 Ordinal)
    {
        auto Frozen=Freeze(Source,Ordinal);if(!Frozen)return false;
        Model->Solver=Frozen;Model->SelectedFrame=Model->PlaybackFrame=Ordinal;Model->DisplayChanged();return Ready();
    }
    // Retain the explicit artificial originals and generated headers for independent
    // Python checks. These archives are render controls, never CFD sample evidence.
    void Home4Slices(const FString& Directory)
    {
        const FString Folder=Directory/TEXT("home4-fixtures");
        IFileManager::Get().MakeDirectory(*Folder,true);
        for(const auto& Pair:TArray<TPair<const TCHAR*,const TCHAR*>>{
            {TEXT("xy"),StudioHome4SliceFixtures::xy},{TEXT("xz"),StudioHome4SliceFixtures::xz},
            {TEXT("yz"),StudioHome4SliceFixtures::yz},{TEXT("mask"),StudioHome4GPUFixtures::MaskXY}})
        {
            const FString Key=Pair.Key,Original=Folder/(Key+TEXT(".npz"));TArray<uint8> Bytes;
            if(!Check(TEXT("home4.")+Key+TEXT(".original"),FBase64::Decode(Pair.Value,Bytes)&&FFileHelper::SaveArrayToFile(Bytes,*Original)))continue;
            const auto Converted=Async(EAsyncExecution::ThreadPool,[&]
            {
                FStudioHome4ArchiveRequest Request;Request.Sources={StudioHome4Archives::Inspect(Original).Source};
                Request.OutputParent=Folder;Request.FolderName=Key;Request.SourceURI=TEXT("urn:artificial-home4-gpu-slice");
                Request.Attribution=TEXT("Artificial affine GPU regression fixture; not CFD output or an installed sample.");
                Request.Mapping.AxisOrder=Key==TEXT("mask")?TEXT("xy"):Key;Request.Mapping.MetadataOrder=TEXT("xyz");
                Request.Mapping.CoordinateUnits=TEXT("lattice");Request.Mapping.VelocityUnits=TEXT("lattice");
                Request.Mapping.DxMeters=.1;Request.Mapping.DtSeconds=.02;
                return StudioHome4Archives::Convert(Request);
            }).Get();
            if(!Check(TEXT("home4.")+Key+TEXT(".convert"),Converted.bSuccess,Converted.Error))continue;
            const auto Open=Async(EAsyncExecution::ThreadPool,[&]{return StudioRecordings::Import(Converted.RecordingJSON,0,{});}).Get();
            if(!Check(TEXT("home4.")+Key+TEXT(".source"),Open.Source.IsValid(),Open.Error))continue;
            const bool Mask=Key==TEXT("mask");
            for(int32 Pass=0;Pass<(Mask?4:1);++Pass)
            {
                const bool Rejected=Mask&&(Pass%2==1),Glyph=Mask&&Pass>=2;
                const FString Scalar=Mask?(Rejected?TEXT("log10_tau_margin"):TEXT("mask_control")):TEXT("ux");
                const FString Name=Mask?FString(TEXT("home4.mask."))+(Glyph?TEXT("glyph."):TEXT("surface."))+(Rejected?TEXT("masked"):TEXT("control")):TEXT("home4.slice.")+Key;
                if(!StartScene(Open.Source,0,Scalar,Glyph))continue;
                Check(Name+TEXT(".controls_applied"),Model->EditView(TEXT("Artificial source slice controls"),[&](auto& S)
                {
                    S.Display.bVolume=true;S.Display.bCutPlane=true;S.Display.bSourcePoints=false;
                    S.Display.bVectors=false;S.Display.VectorCount=Mask?81:30;S.Display.bUniformVectors=true;
                    S.Display.VectorScale=.25;S.Display.bHome4AirMask=false;
                }),TEXT("Valid source-slice view settings must be accepted."));
                // The isolated model is a frozen comparison snapshot, whose
                // interactive EditView intentionally forbids enabling vectors.
                // Its source was explicitly prepared with velocity for this
                // renderer contract; configure that layer directly here.
                Model->SetScalarStyle(3,true,-1.,1.,{FLinearColor(1,0,1),FLinearColor(1,0,1),FLinearColor(1,0,1)});
                const auto V=Open.Source->VolumeReconstruction();
                if(!Check(Name+TEXT(".topology"),V&&V->OriginalGrid&&V->OriginalGrid->bPlanar)){Retire();continue;}
                const int32 SourceAxis=V->OriginalGrid->PlaneAxis(),Axis=SourceAxis==1?2:SourceAxis==2?1:0;
                const auto B=V->SourceBounds;const FVector Center(B.GetCenter().X,B.GetCenter().Z,B.GetCenter().Y);
                FVector Normal=FVector::ZeroVector;Normal[Axis]=1;
                auto Camera=Scene->CameraState();Camera.Focus=Center;Camera.OrbitDistance=2;
                Camera.Position=Center+Normal*2;Camera.Orientation=(-Normal).Rotation().Quaternion();Camera.bOrthographic=true;
                const FBox SceneBounds(FVector(B.Min.X,B.Min.Z,B.Min.Y),FVector(B.Max.X,B.Max.Z,B.Max.Y));
                const FVector Right=Camera.Orientation.GetRightVector(),Up=Camera.Orientation.GetUpVector();
                const FVector Extent=SceneBounds.GetSize();
                const double Horizontal=FMath::Abs(Right.X)*Extent.X+FMath::Abs(Right.Y)*Extent.Y+FMath::Abs(Right.Z)*Extent.Z;
                const double Vertical=FMath::Abs(Up.X)*Extent.X+FMath::Abs(Up.Y)*Extent.Y+FMath::Abs(Up.Z)*Extent.Z;
                Camera.OrthoWidth=FMath::Max(Horizontal,Vertical*Size.X/Size.Y)*1.25;
                Scene->RestoreCamera(Camera,TEXT("Original source plane GPU camera"));
                // Finish validated camera/style edits before the snapshot-only
                // vector override; those edits reject interactive vector layers.
                Model->bVectors=Glyph;Model->DisplayChanged();
                if(!Ready()){Retire();continue;}
                Record(Name,0,false);
                auto Row=Cases.Last()->AsObject();
                const auto Field=Scene->PresentedField();const auto Identity=Field->Identity();
                const auto Points=Field->OriginalPoints();auto* Component=Scene->FindComponentByClass<UStudioVolumeComponent>();
                auto* Mesh=Cast<UProceduralMeshComponent>(Scene->GetRootComponent());
                const auto* Surface=Mesh?Mesh->GetProcMeshSection(2):nullptr;
                const auto* Glyphs=Mesh?Mesh->GetProcMeshSection(4):nullptr;
                const int32 Triangles=Surface?Surface->ProcIndexBuffer.Num()/3:0;
                const int32 ExpectedTriangles=Mask?(Rejected?64:128):40,ExpectedGlyphs=Glyph?(Rejected?45:81):0;
                bool Plane=Points&&Points->Geometry&&Points->Geometry->Positions.Num()==(Mask?81:30);
                if(Plane)for(const auto& P:Points->Geometry->Positions)Plane&=FMath::IsNearlyEqual(P[SourceAxis],B.Min[SourceAxis],1.e-12);
                if(Surface)for(const auto& P:Surface->ProcVertexBuffer)Plane&=FMath::IsNearlyEqual(double(P.Position[Axis])/100.,B.Min[SourceAxis],1.e-7);
                const bool Native=Identity&&Identity->Interpolation==EStudioFieldInterpolation::SourceSlice&&Plane;
                const bool NoVolume=Component&&Component->TextureBytes()==0&&!Component->IsVisible();
                const bool Geometry=Triangles==ExpectedTriangles&&Scene->PresentedVectors().GlyphCount==ExpectedGlyphs&&
                    (!Glyph||(Glyphs&&Glyphs->ProcIndexBuffer.Num()>0));
                double Sample=0;const FVector Invalid=Mask?FVector(.3,.5,.8):FVector(.225,.4,.55),Valid=Mask?FVector(.8,.5,.8):FVector(.375,.4,.55);
                const bool InvalidSample=Field->SampleScalar(Invalid,Scalar,Sample),ValidSample=Field->SampleScalar(Valid,Scalar,Sample);
                const bool Sampling=!Mask||(InvalidSample==!Rejected&&ValidSample);
                TArray<FColor> Pixels;const bool Read=Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
                for(auto& P:Pixels)P.A=255;
                const FString PixelPath=Directory/(Name+TEXT(".bgra"));
                const bool Saved=Read&&Pixels.Num()==Size.X*Size.Y&&FFileHelper::SaveArrayToFile(
                    TArrayView64<const uint8>(reinterpret_cast<const uint8*>(Pixels.GetData()),Pixels.Num()*sizeof(FColor)),*PixelPath);
                Row->SetStringField(TEXT("interpolation"),Native?TEXT("SourceSlice"):TEXT("unexpected"));
                Row->SetNumberField(TEXT("original_nodes"),Points&&Points->Geometry?Points->Geometry->Positions.Num():0);
                Row->SetBoolField(TEXT("original_plane_only"),Plane);Row->SetNumberField(TEXT("volume_texture_bytes"),Component?Component->TextureBytes():-1);
                Row->SetNumberField(TEXT("surface_triangles"),Triangles);Row->SetNumberField(TEXT("glyphs"),Scene->PresentedVectors().GlyphCount);
                Row->SetStringField(TEXT("scalar"),Scalar);Row->SetBoolField(TEXT("invalid_sample_available"),InvalidSample);Row->SetBoolField(TEXT("valid_sample_available"),ValidSample);
                Row->SetNumberField(TEXT("ortho_width"),Camera.OrthoWidth);
                auto Vector=[](const FVector& P){return TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(P.X),MakeShared<FJsonValueNumber>(P.Y),MakeShared<FJsonValueNumber>(P.Z)};};
                Row->SetArrayField(TEXT("camera_focus"),Vector(Center));Row->SetArrayField(TEXT("camera_right"),Vector(Right));Row->SetArrayField(TEXT("camera_up"),Vector(Up));
                Row->SetBoolField(TEXT("passed"),Row->GetBoolField(TEXT("passed"))&&Native&&NoVolume&&Geometry&&Sampling&&Saved);
                Check(Name+TEXT(".source_slice"),Native,TEXT("Unchanged nodes on the original fixed source plane, including rendered vertices."));
                Check(Name+TEXT(".no_volume"),NoVolume,TEXT("SourceSlice must allocate no 3D volume texture."));
                Check(Name+TEXT(".mask_geometry"),Geometry,TEXT("Exact nonempty source-cell and original vector glyph counts."));
                Check(Name+TEXT(".sampling"),Sampling,TEXT("Invalid diagnostic samples unavailable; valid region retained."));
                Check(Name+TEXT(".readback"),Saved,TEXT("Raw GPU target bytes retained for independent numerical checks."));
                Retire();
            }
        }
    }
    void CheckIndependentOpacity()
    {
        // Constant transfer weights isolate the GPU input contract. They are display-test
        // controls over the existing recorded field, never published CFD measurements.
        auto* Component=Scene->FindComponentByClass<UStudioVolumeComponent>();const auto Field=Scene->PresentedField();
        if(!Check(TEXT("volume.independent_opacity.component"),Component&&Field&&Field->OriginalPoints()&&Field->VolumeReconstruction()))return;
        auto Grid=StudioVolumes::Build(*Field->OriginalPoints(),*Field->VolumeReconstruction(),Scene->PresentedScalar().Id,Scene->PresentedColorMapping());
        if(!Check(TEXT("volume.independent_opacity.grid"),Grid.Error.IsEmpty(),Grid.Error))return;
        const auto Camera=Scene->CameraState();const auto Identity=Field->Identity();const int64 Before=Component->TextureBytes();
        TArray<uint32> Hashes;FString Error;
        for(const float Weight:{0.f,1.f})
        {
            Grid.OpacityTexels.Init(Weight,Grid.Texels.Num());
            if(!Check(TEXT("volume.independent_opacity.upload"),Component->Present(Grid,Scene->PresentedColorMapping(),*Model,Error),Error))return;
            FlushRenderingCommands();
            FStudioSnapshot Snapshot;Snapshot.Options={Size,false,false,false};
            if(!Check(TEXT("volume.independent_opacity.capture"),Scene->CaptureSnapshot(Snapshot,nullptr,Error),Error))return;
            Hashes.Add(FCrc::MemCrc32(Snapshot.Pixels.GetData(),Snapshot.Pixels.Num()*sizeof(FColor)));
            Check(TEXT("volume.independent_opacity.identity"),Snapshot.Identity.Ordinal==Identity->Ordinal&&StudioView::CameraEquals(Camera,Snapshot.Camera));
        }
        Check(TEXT("volume.independent_opacity.changes_pixels"),Hashes.Num()==2&&Hashes[0]!=Hashes[1]);
        Check(TEXT("volume.independent_opacity.accounted"),Component->TextureBytes()==Before+int64(Grid.Texels.Num())*sizeof(float));
        Grid.OpacityTexels.Reset();Component->Present(Grid,Scene->PresentedColorMapping(),*Model,Error);FlushRenderingCommands();
        Check(TEXT("volume.independent_opacity.released"),Component->TextureBytes()==Before);
    }
    void Retire()
    {
        if(Scene){Scene->Destroy();Scene=nullptr;}Model.Reset();FlushRenderingCommands();
    }
    uint32 Record(const FString& Name,int32 Ordinal,bool bRequireVolume,bool bSnapshot=false)
    {
        if(!Ready())return 0;
        TArray<FColor> Pixels;FStudioSnapshot Snapshot;FString Error;
        bool Read=false;
        if(bSnapshot)
        {
            Snapshot.Options={Size,false,false,false};Read=Scene->CaptureSnapshot(Snapshot,nullptr,Error);
            Pixels=MoveTemp(Snapshot.Pixels);
        }
        else Read=Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        const auto Field=Scene->PresentedField();const auto Identity=Field?Field->Identity():TOptional<FStudioFieldIdentity>();
        const auto Frame=Model->Solver->EvaluateFrame(Ordinal);
        const bool Exact=Identity&&Identity->Ordinal==Ordinal&&Identity->Frame.Index==Frame.Index&&Identity->Frame.Time==Frame.Time&&
            Identity->Dataset==Model->Solver->Descriptor().Id&&Identity->MetadataSHA256==Model->Solver->Descriptor().MetadataSHA256;
        int32 Colored=0;
        for(auto& P:Pixels){P.A=255;if(FMath::Max3(P.R,P.G,P.B)>12&&FMath::Max3(P.R,P.G,P.B)-FMath::Min3(P.R,P.G,P.B)>4)++Colored;}
        const auto Stats=Scene->ResourceStats();
        const bool Bounded=Stats.Workers<=1&&Stats.MeshBytes<128LL*1024*1024&&Stats.ScalarTextureBytes<=17LL*1024*1024;
        const bool Volume=!bRequireVolume||Stats.ScalarTextureBytes>0;
        bool Passed=Read&&Pixels.Num()==Size.X*Size.Y&&Colored>Pixels.Num()/200&&Exact&&Bounded&&Volume;
        double SnapshotDifference=-1;
        if(bSnapshot)Passed&=Snapshot.Identity.Ordinal==Ordinal&&Snapshot.Identity.Frame.Index==Frame.Index&&
            StudioView::CameraEquals(Snapshot.Camera,Scene->PresentedCamera())&&Snapshot.bVolumeRendererActive;
        if(bSnapshot)
        {
            TArray<FColor> Anchor;
            if(Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Anchor)&&Anchor.Num()==Pixels.Num()&&!Pixels.IsEmpty())
            {
                int64 Difference=0;
                for(int32 I=0;I<Pixels.Num();++I)Difference+=FMath::Abs(int32(Anchor[I].R)-Pixels[I].R)+
                    FMath::Abs(int32(Anchor[I].G)-Pixels[I].G)+FMath::Abs(int32(Anchor[I].B)-Pixels[I].B);
                SnapshotDifference=double(Difference)/(Pixels.Num()*3.);
            }
            Passed&=SnapshotDifference>=0&&SnapshotDifference<2;
        }
        const uint32 CRC=Pixels.IsEmpty()?0:FCrc::MemCrc32(Pixels.GetData(),Pixels.Num()*sizeof(FColor));
        auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),Passed);
        Row->SetNumberField(TEXT("width"),Size.X);Row->SetNumberField(TEXT("height"),Size.Y);
        Row->SetNumberField(TEXT("colored_pixels"),Colored);Row->SetStringField(TEXT("pixel_crc32"),FString::Printf(TEXT("%08x"),CRC));
        Row->SetBoolField(TEXT("frame_matches_source"),Exact);Row->SetBoolField(TEXT("volume_required"),bRequireVolume);
        Row->SetNumberField(TEXT("mesh_bytes"),Stats.MeshBytes);Row->SetNumberField(TEXT("scalar_texture_bytes"),Stats.ScalarTextureBytes);
        if(Identity)
        {
            Row->SetStringField(TEXT("dataset"),Identity->Dataset);Row->SetStringField(TEXT("metadata_sha256"),Identity->MetadataSHA256);
            Row->SetStringField(TEXT("payload_sha256"),Identity->PayloadSHA256);Row->SetStringField(TEXT("reconstruction_sha256"),Identity->ReconstructionSHA256);
            Row->SetNumberField(TEXT("ordinal"),Identity->Ordinal);Row->SetNumberField(TEXT("original_step"),Identity->Frame.Index);
            Row->SetNumberField(TEXT("original_time"),Identity->Frame.Time);Row->SetNumberField(TEXT("spatial_dimensions"),Identity->SpatialDimensions);
        }
        // Store numerical camera/field/export identity alongside pixel metrics.
        if(bSnapshot)
        {
            Row->SetStringField(TEXT("snapshot_metadata"),StudioSnapshot::Metadata(Snapshot));
            Row->SetNumberField(TEXT("snapshot_mean_rgb_error"),SnapshotDifference);
        }
        Row->SetStringField(TEXT("camera"),Scene->PresentedCamera().Position.ToString());
        Row->SetBoolField(TEXT("orthographic"),Scene->PresentedCamera().bOrthographic);
        Cases.Add(MakeShared<FJsonValueObject>(Row));
        Check(Name,Passed,Error.IsEmpty()?TEXT("Original identity, visible pixels and bounded production rendering."):Error);
        return CRC;
    }
};
}
#endif

int32 UStudioRenderValidationCommandlet::Main(const FString& Params)
{
#if !WITH_EDITOR
    UE_LOG(LogTemp,Error,TEXT("Renderer validation requires the editor commandlet build."));return 1;
#else
    using namespace StudioWindowlessRendering;
    FString Output;
    if(!FParse::Value(*Params,TEXT("StudioRenderReport="),Output)||Output.IsEmpty())
    {UE_LOG(LogTemp,Error,TEXT("Supply -StudioRenderReport=<absolute JSON path>."));return 1;}
    FRun Run;const double Started=FPlatformTime::Seconds();
    FString HelpReview;FParse::Value(*Params,TEXT("StudioHelpReview="),HelpReview);
    FString NotificationsReview;FParse::Value(*Params,TEXT("StudioNotificationsReview="),NotificationsReview);
    const FString RHI=GDynamicRHI?GDynamicRHI->GetName():TEXT("unavailable");
    const bool Windowless=IsRunningCommandlet()&&FApp::CanEverRender()&&GEngine&&GEngine->GameViewport==nullptr&&
        (!FSlateApplication::IsInitialized()||FSlateApplication::Get().GetTopLevelWindows().IsEmpty());
    Run.Check(TEXT("runtime.windowless"),Windowless,TEXT("No game viewport or top-level Slate windows; commandlet rendering enabled."));
    Run.Check(TEXT("runtime.gpu"),GDynamicRHI&&!GUsingNullRHI,RHI);
    if(Windowless&&!GUsingNullRHI)
    {
        const TStrongObjectPtr<UWorld> World(UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,false));
        Run.World=World.Get();
        UWorld* PreviousWorld=GWorld;GWorld=World.Get();
        auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);Context.SetCurrentWorld(World.Get());
        ON_SCOPE_EXIT
        {
            Run.Retire();GEngine->DestroyWorldContext(World.Get());
            World->DestroyWorld(false);GWorld=PreviousWorld;FlushRenderingCommands();
        };
        World->InitializeActorsForPlay(FURL());World->BeginPlay();
        const auto Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
        auto Wing=Async(EAsyncExecution::ThreadPool,[&]
        {return StudioRecordings::Import(StudioRecordings::PathForId(TEXT("MeshGraphNets_Airfoil_test009")),0,Cancellation);}).Get();
        if(Run.Check(TEXT("source.wing"),Wing.Source.IsValid(),Wing.Error)&&Run.StartScene(Wing.Source,0))
        {
            const FString CaseBefore=StudioCaseIO::Serialize(Run.Model->Project.Draft);
            const auto CameraBefore=Run.Scene->CameraState();const uint32 First=Run.Record(TEXT("wing.first"),0,false);
            if(!HelpReview.IsEmpty())Run.ReviewHelp(HelpReview);
            if(!NotificationsReview.IsEmpty())Run.ReviewHelp(NotificationsReview,true);
            const int32 Last=Wing.Source->FrameCount()-1;
            if(Run.ChangeFrame(Wing.Source,Last))
            {
                const uint32 Final=Run.Record(TEXT("wing.last"),Last,false);
                Run.Check(TEXT("wing.evolution"),First!=Final,TEXT("Different original times produce different GPU pixels."));
                Run.Check(TEXT("wing.frame_keeps_camera"),StudioView::CameraEquals(CameraBefore,Run.Scene->CameraState()));
                Run.Scene->Orbit(35,12);const uint32 Moved=Run.Record(TEXT("wing.camera"),Last,false);
                Run.Check(TEXT("wing.camera_independent"),Moved!=Final&&Run.Model->SelectedFrame==Last&&
                    StudioCaseIO::Serialize(Run.Model->Project.Draft)==CaseBefore);
            }
        }
        Run.Retire();
        // VolumeComponent loads this asset when the first completed grid is
        // applied. A commandlet does not tick the editor shader manager. Pin
        // and finish it before any grid can publish an otherwise-ready frame.
        const TStrongObjectPtr<UMaterialInterface> VolumeMaterial(LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Game/Studio/M_FlowVolume.M_FlowVolume")));
        Run.Check(TEXT("assets.volume_material"),VolumeMaterial.IsValid());
        FAssetCompilingManager::Get().FinishAllCompilation();
        const FString Recording=FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json");
        const FString Mapping=FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json");
        auto Volume=Async(EAsyncExecution::ThreadPool,[&]
        {
            auto Source=StudioRecordings::Import(Recording,0,Cancellation);
            if(Source.Reference)return StudioRecordings::ImportReconstruction(*Source.Reference,Mapping,0,Cancellation);
            return Source;
        }).Get();
        if(Run.Check(TEXT("source.volume"),Volume.Source&&Volume.Source->VolumeReconstruction().IsValid(),Volume.Error)&&Run.StartScene(Volume.Source,0))
        {
            const auto Outside=Run.Scene->CameraState();const uint32 First=Run.Record(TEXT("volume.first"),0,true);
            const int32 Last=Volume.Source->FrameCount()-1;
            if(Run.ChangeFrame(Volume.Source,Last))
            {
                const uint32 Final=Run.Record(TEXT("volume.last"),Last,true);
                Run.Check(TEXT("volume.evolution"),First!=Final);
                const auto B=Volume.Source->Descriptor().DisplayBounds;auto Camera=Outside;
                Camera.Position=B.Min+B.GetSize()*FVector(.65,.5,.55);Camera.Orientation=FQuat::Identity;
                Camera.Focus=Camera.Position+FVector(1,0,0)*Camera.OrbitDistance;
                Run.Scene->RestoreCamera(Camera,TEXT("Windowless camera inside volume"));
                Run.Record(TEXT("volume.inside_perspective"),Last,true);
                Camera.bOrthographic=true;Camera.OrthoWidth=B.GetSize().Z*.75;
                Run.Scene->RestoreCamera(Camera,TEXT("Windowless orthographic camera"));
                Run.Record(TEXT("volume.inside_orthographic"),Last,true);
                Run.Scene->RestoreCamera(Outside,TEXT("Restore outside camera"));
                Run.Model->EditView(TEXT("Clip volume"),[](auto& S){S.Display.VolumeClipMinimum.X=.5;S.Display.VolumeOpacity=.8;});
                const uint32 Clipped=Run.Record(TEXT("volume.clipped"),Last,true);
                Run.Check(TEXT("volume.clip_changes_pixels"),Clipped!=Final);
                const auto Range=Run.Model->ActiveColorMapping();
                Run.Model->EditView(TEXT("Isosurface"),[&](auto& S){S.Display.bVolume=false;S.Display.bVolumeIsosurface=true;
                    S.Display.VolumeIsovalue=(Range.Minimum+Range.Maximum)*.5;});
                Run.Record(TEXT("volume.isosurface"),Last,false);
                Run.Model->EditView(TEXT("Restore original view"),[](auto& S){S.Display.bVolume=true;S.Display.bVolumeIsosurface=false;
                    S.Display.VolumeClipMinimum=FVector::ZeroVector;S.Display.VolumeOpacity=FStudioViewSettings().VolumeOpacity;});
                const uint32 Restored=Run.Record(TEXT("volume.restored"),Last,true);
                Run.Check(TEXT("volume.restored_pixels"),Restored==Final);
                Run.Record(TEXT("snapshot.original_frame"),Last,true,true);
                const uint64 Captures=Run.Scene->GetCaptureCount();
                const auto Inspection=Run.Model->InspectionState();
                for(int32 I=0;I<10;++I)Run.Pump();
                Run.bVisible=false;for(int32 I=0;I<10;++I)Run.Pump();Run.bVisible=true;
                Run.Check(TEXT("idle.no_captures"),Run.Scene->GetCaptureCount()==Captures&&Run.Model->InspectionState().Equals(Inspection));
                Run.CheckIndependentOpacity();
            }
        }
        Run.Retire();
        Run.Home4Slices(FPaths::GetPath(Output));
    }
    Run.Check(TEXT("runtime.still_windowless"),GEngine->GameViewport==nullptr&&
        (!FSlateApplication::IsInitialized()||FSlateApplication::Get().GetTopLevelWindows().IsEmpty()));
    auto Report=MakeShared<FJsonObject>();Report->SetNumberField(TEXT("version"),1);Report->SetBoolField(TEXT("passed"),Run.Errors.IsEmpty());
    Report->SetStringField(TEXT("mode"),TEXT("windowless-gpu-commandlet"));Report->SetStringField(TEXT("rhi"),RHI);
    Report->SetBoolField(TEXT("windowless"),Windowless);Report->SetNumberField(TEXT("screenshots"),Run.ReviewImages.Num());
    Report->SetArrayField(TEXT("review_images"),Run.ReviewImages);
    Report->SetNumberField(TEXT("elapsed_seconds"),FPlatformTime::Seconds()-Started);
    Report->SetArrayField(TEXT("cases"),Run.Cases);Report->SetArrayField(TEXT("checks"),Run.Checks);
    TArray<TSharedPtr<FJsonValue>> Errors;for(const auto& Error:Run.Errors)Errors.Add(MakeShared<FJsonValueString>(Error));
    Report->SetArrayField(TEXT("errors"),Errors);
    FString JSON;FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&JSON));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output),true);
    if(!FFileHelper::SaveStringToFile(JSON,*Output,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 1;
    return Run.Errors.IsEmpty()?0:1;
#endif
}
