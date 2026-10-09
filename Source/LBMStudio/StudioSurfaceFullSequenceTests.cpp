#include "StudioScene.h"
#include "StudioSurfaceRenderData.h"
#include "StudioPlatformDiagnostics.h"
#include "StudioFileDialog.h"
#include "SStudioProbeProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "Serialization/Csv/CsvParser.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
struct FSurfaceSequenceAudit
{
    FString Error;
    int64 Values=0;
    double MaxRelativeError=0;
};
struct FSurfaceResourceSample
{
    double Seconds=0;
    uint64 Footprint=0;
    int64 Device=0;
};
}

/** Explicit full-data gate. Exhaustive pressure transport and ordinary-clock
 * viewport playback are separate phases; neither synthesizes missing frames. */
class FStudioSurfaceSequenceCommand final : public IAutomationLatentCommand
{
public:
    FStudioSurfaceSequenceCommand(FAutomationTestBase* InTest,FString InRecording,FString InSurface,bool InInspection=false)
        :Test(InTest),RecordingPath(MoveTemp(InRecording)),SurfacePath(MoveTemp(InSurface)),bInspection(InInspection){}
    ~FStudioSurfaceSequenceCommand()
    {if(Cancel)Cancel->store(true);if(Audit.IsValid())Audit.Wait();}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>300){Test->AddError(FString::Printf(TEXT("Full surface gate timed out in phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        switch(Phase)
        {
        case 0:
        {
            if(!Ready())return false;
            Root=FPaths::ProjectSavedDir()/(bInspection?TEXT("Automation/InspectionSequence"):TEXT("Automation/SurfaceSequence"));
            Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            Original=M.SnapshotProject();FString Error;
            Test->TestTrue(TEXT("Preserve preceding document"),StudioProjectIO::Save(Work/TEXT("original.lbms"),Original,Error));
            M.NewProject(TEXT("NACA 0018 full reconstructed surface"));
            Test->TestTrue(TEXT("Open full original recording"),M.RequestExternalRecording(RecordingPath));Phase=1;break;
        }
        case 1:
            if(!Ready())return false;
            if(!Test->TestEqual(TEXT("All original snapshots available"),M.Solver->FrameCount(),8000)||
                !Test->TestTrue(TEXT("Full source has an imported reference"),!M.Project.Recordings.IsEmpty()))return true;
            if(!Test->TestEqual(TEXT("Audited full recording descriptor"),M.Project.Recordings[0].MetadataSHA256,
                FString(TEXT("1ce4f9f4a7d71f060e60e62ecd0aa52de78e7f7d0328930ef0cdc952cd852a67"))))return true;
            Test->TestTrue(TEXT("Explicitly attach full-source reconstruction"),M.RequestReconstruction(SurfacePath));Phase=2;break;
        case 2:
        {
            if(!Ready())return false;
            const auto Surface=M.Solver->Reconstruction();
            if(!Test->TestTrue(TEXT("Full-source topology attached"),Surface.IsValid()))return true;
            Test->TestEqual(TEXT("Audited reconstruction descriptor"),Surface->MetadataSHA256,
                FString(TEXT("b9194cdcf7500163650c6a78d696aa8214a06988e245c4e5ea707fdcb511265d")));
            Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
            Audit=Async(EAsyncExecution::ThreadPool,[Solver=M.Solver,Cancel=Cancel]
            {
                FSurfaceSequenceAudit Result;
                for(int32 Ordinal=0;Ordinal<8000;++Ordinal)
                {
                    if(Cancel->load()){Result.Error=TEXT("Sequence audit cancelled");return Result;}
                    const auto Field=Solver->CaptureViewField(Ordinal,TEXT("pressure"),false,Cancel);
                    const auto Frame=Field->OriginalPoints();
                    if(!Field->IsValid()||!Frame||Frame->Ordinal!=Ordinal||Frame->Descriptor->Frames[Ordinal].Index!=Ordinal+1001)
                    {Result.Error=FString::Printf(TEXT("Original frame %d unavailable or mismatched"),Ordinal);return Result;}
                    const auto Data=StudioSurfaceRendering::Build(*Frame,*Field->Reconstruction(),TEXT("pressure"),{0,true,-10,10},Cancel);
                    if(!Data.Error.IsEmpty()){Result.Error=Data.Error;return Result;}
                    const auto* Values=Frame->FindValues(TEXT("pressure"));
                    if(!Values||Values->Num()!=18706||Data.Vertices.Num()!=18706||Data.Indices.Num()!=37188*3)
                    {Result.Error=TEXT("Full sequence lost source rows or verified triangles");return Result;}
                    for(int32 Row=0;Row<Values->Num();++Row)
                    {
                        const double Expected=((*Values)[Row]+10.)/20.;
                        const double Relative=FMath::Abs(Data.Scalars[Row]-Expected)/FMath::Max(1.,FMath::Abs(Expected));
                        Result.MaxRelativeError=FMath::Max(Result.MaxRelativeError,Relative);
                        if(!FMath::IsFinite(Relative)||Relative>1.e-7)
                        {Result.Error=TEXT("Full sequence scalar transport exceeded float precision bound");return Result;}
                        ++Result.Values;
                    }
                    const auto Cache=Solver->CacheStats();
                    if(Cache.ResidentBytes>Cache.BudgetBytes||StudioPointRecordings::LiveStats().AllocatedValueBytes>48LL*1024*1024)
                    {Result.Error=TEXT("Full sequence audit exceeded source memory budget");return Result;}
                }
                return Result;
            });
            Phase=3;break;
        }
        case 3:
        {
            if(!Audit.IsReady())return false;
            const auto Result=Audit.Get();Audit={};Cancel.Reset();
            if(!Test->TestTrue(*Result.Error,Result.Error.IsEmpty()))return true;
            Test->TestEqual(TEXT("Every original pressure sample transported"),Result.Values,int64(8000)*18706);
            Test->AddInfo(FString::Printf(TEXT("8000 original pressure frames: %lld scalar comparisons, max relative R32 error %.12g."),Result.Values,Result.MaxRelativeError));
            M.EditView(TEXT("Full reconstructed surface"),[](auto& S)
            {
                S.Display.ScalarField=TEXT("velocity_magnitude");S.Display.bVectors=false;S.Display.bMesh=false;
                S.Display.bReconstructedSurface=true;S.Display.bSourcePoints=true;
                S.Camera.Focus=FVector(.06,0,.015);S.Camera.Position=FVector(.16,.45,.16);
                S.Camera.Orientation=(S.Camera.Focus-S.Camera.Position).Rotation().Quaternion();
                S.Camera.OrbitDistance=(S.Camera.Focus-S.Camera.Position).Size();S.Camera.bOrthographic=false;
            });
            if(bInspection)
            {
                const auto B=M.Solver->Descriptor().DisplayBounds;
                FStudioSliceObject Slice;Slice.Name=TEXT("Full recording midplane");Slice.Origin=B.GetCenter();Slice.Opacity=.2;
                Test->TestTrue(TEXT("Author full-recording slice"),M.AddSlice(Slice));
                Slice.Id=FGuid::NewGuid();Slice.Name=TEXT("Full recording second plane");Slice.Opacity=.1;
                Test->TestTrue(TEXT("Author independently saved second slice"),M.AddSlice(Slice));
                FStudioRulerObject Ruler;Ruler.Name=TEXT("50 mm reference");Ruler.A=B.GetCenter();Ruler.B=Ruler.A+FVector(.03,0,.04);
                Ruler.Unit=TEXT("mm");Test->TestTrue(TEXT("Author known-distance ruler"),M.AddRuler(Ruler));RulerId=Ruler.Id;
                FStudioProbeObject Point;Point.Name=TEXT("Original pressure point 100");Point.Field=TEXT("pressure");Point.Method=EStudioProbeMethod::OriginalPoint;Point.PointId=100;
                Test->TestTrue(TEXT("Author exact-ID marker"),M.AddProbe(Point));
                FStudioProbeObject Line;Line.Kind=EStudioProbeKind::Line;Line.Name=TEXT("Full recording pressure profile");Line.Field=TEXT("pressure");Line.Samples=32;
                Line.A=B.Min+B.GetSize()*FVector(.2,.5,.75);Line.B=B.Min+B.GetSize()*FVector(.8,.5,.75);
                Test->TestTrue(TEXT("Author independent pressure line"),M.AddProbe(Line));LineId=Line.Id;
                ExpectedObjects=M.InspectionObjects;
            }
            M.PlaybackRate=4;M.bLoopPlayback=false;M.Scrub(0);Phase=4;break;
        }
        case 4:
            if(!Ready())return false;
            if(bInspection)
            {
                if(!bPanelOpened){Press(TEXT("InspectionTools"));bPanelOpened=true;return false;}
                if(!CurrentProfile())return false;
            }
            View=Scene->SavedCameraState();Source=M.Project.Dataset;Case=StudioCaseIO::Serialize(M.Project.Draft);
            FirstHash=Pixels();Capture(TEXT("first.png"));Scene->Snapshot(Root/TEXT("first-field.png"));
            Test->TestEqual(TEXT("First original step"),Scene->PresentedFrame().Index,1001);
            M.Run();PlaybackStarted=Now;LastPresented=1000;Phase=5;break;
        case 5:
        {
            if(bInspection)InspectPlayback(Now);
            if(Scene->HasPresentedFrame())
            {
                const auto F=Scene->PresentedFrame();
                if(F.Index!=LastPresented)
                {
                    Test->TestTrue(TEXT("Presented surface steps increase"),F.Index>LastPresented);
                    Test->TestEqual(TEXT("Presented source step/time agree"),F.Time,F.Index*.0025);
                    Test->TestEqual(TEXT("Presented source identity"),Scene->PresentedDatasetId(),Source);
                    LastPresented=F.Index;++PresentedFrames;
                }
            }
            if(Now-LastSample>=1)
            {
                LastSample=Now;Sample(Now-PlaybackStarted);
                const int32 Selected=M.SelectedFrame,Playback=M.PlaybackFrame;
                auto C=View;C.Position+=FVector(.025*FMath::Sin(Now-PlaybackStarted),0,.015*FMath::Cos(Now-PlaybackStarted));
                C.Orientation=(C.Focus-C.Position).Rotation().Quaternion();
                Scene->RestoreCamera(C,TEXT("Playback camera"));
                Test->TestEqual(TEXT("Camera retains selected source frame"),M.SelectedFrame,Selected);
                Test->TestEqual(TEXT("Camera retains playback cursor"),M.PlaybackFrame,Playback);
                Test->TestTrue(TEXT("Camera updates during source playback"),StudioView::CameraEquals(Scene->SavedCameraState(),C));
            }
            if(M.State!=EStudioRunState::Complete)return false;
            Test->TestEqual(TEXT("Natural playback reaches final ordinal"),M.PlaybackFrame,7999);
            Test->TestTrue(TEXT("Natural playback presents sustained evolution"),PresentedFrames>500);
            Test->TestTrue(TEXT("Natural playback runs for the full wall duration"),Now-PlaybackStarted>=95);
            Scene->RestoreCamera(View,TEXT("Restore comparison view"));Phase=6;break;
        }
        case 6:
        {
            if(!Ready())return false;
            if(bInspection)
            {
                const auto Profile=CurrentProfile();if(!Profile)return false;
                // Slate may refresh the enabled attribute after the new
                // profile is published. Use the control once it is ready.
                const auto Export=Control(TEXT("ExportProbeCSV"));if(!Export||!Export->IsEnabled())return false;
                Test->TestTrue(TEXT("Pressure profile stays live across the full recording"),ProfileFrames.Num()>100);
                Test->TestTrue(TEXT("Full playback has early and late pressure profiles"),EarliestProfile<1800&&LatestProfile>8200);
                Test->TestTrue(TEXT("Pressure exports occur repeatedly during playback"),ProbeExports>=6);
                Test->TestTrue(TEXT("Saved slice/probe/ruler coordinates survive playback"),M.InspectionObjects==ExpectedObjects);
                Test->TestTrue(TEXT("Known ruler measurement remains 50 mm"),FMath::IsNearlyEqual(StudioInspectionObjects::Measurement(*M.FindRuler(RulerId)).GetValue(),50.,1.e-10));
                FinalProfile=*Profile;FinalCSV=Root/TEXT("final-profile.csv");IFileManager::Get().Delete(*FinalCSV);
                StudioFileDialog::SetNextProbeCSVForAutomation(FinalCSV);Press(TEXT("ExportProbeCSV"));
            }
            Test->TestEqual(TEXT("Final original step"),Scene->PresentedFrame().Index,9000);
            Test->TestEqual(TEXT("Final original time"),Scene->PresentedFrame().Time,22.5);
            Test->TestNotEqual(TEXT("Original evolution changes field pixels at identical camera"),Pixels(),FirstHash);
            Capture(TEXT("complete.png"));Scene->Snapshot(Root/TEXT("last-field.png"));
            Test->TestEqual(TEXT("Playback and camera preserve case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
            Test->TestTrue(TEXT("Export final original values"),M.ExportField(Work/TEXT("displayed.csv")));
            FString CSV;FFileHelper::LoadFileToString(CSV,*(Work/TEXT("displayed.csv")));
            Test->TestTrue(TEXT("Export uses displayed step and time"),CSV.Contains(TEXT(",9000,\"t_9000\",22.5,")));
            CheckDrift();
            if(bInspection){Phase=12;break;}
            Test->TestTrue(TEXT("Save complete surface inspection"),M.SaveProject(Work/TEXT("surface.lbms")));Saved=M.SnapshotProject();
            Test->TestTrue(TEXT("Reopen complete surface inspection"),M.RequestProjectOpen(Work/TEXT("surface.lbms")));Phase=7;break;
        }
        case 7:
            if(!Ready())return false;
            if(bInspection)
            {
                if(M.SelectedInspectionObject!=LineId){M.SelectInspectionObject(LineId);return false;}
                if(!CurrentProfile())return false;
                Test->TestTrue(TEXT("Reopened full recording restores all inspection objects"),M.InspectionObjects==ExpectedObjects);
                Test->TestEqual(TEXT("Reopened profile retains independent pressure field"),CurrentProfile()->Snapshot.Field,FString(TEXT("pressure")));
            }
            Test->TestEqual(TEXT("Full surface document restores exactly"),StudioProjectIO::Serialize(M.SnapshotProject()),StudioProjectIO::Serialize(Saved));
            Test->TestEqual(TEXT("Reopen retains final GPU pixels"),Pixels(),LastHash);
            Capture(TEXT("reopened.png"));IdleStarted=Now;IdleCaptures=Scene->GetCaptureCount();Phase=8;break;
        case 8:
            if(Now-IdleStarted<3)return false;
            Test->TestEqual(TEXT("Idle surface submits no new captures"),Scene->GetCaptureCount(),IdleCaptures);
            M.EditView(TEXT("Original points"),[](auto& S){S.Display.bReconstructedSurface=false;});Phase=9;break;
        case 9:
            if(!Ready())return false;
            Test->TestEqual(TEXT("Point mode releases scalar texture ownership"),Scene->ResourceStats().ScalarTextureBytes,int64(0));
            M.UndoView();Phase=10;break;
        case 10:
            if(!Ready())return false;
            Test->TestEqual(TEXT("Representation undo restores exact surface pixels"),Pixels(),LastHash);
            Test->TestTrue(TEXT("Restore preceding source"),M.RequestProjectOpen(Work/TEXT("original.lbms")));Phase=11;break;
        case 11:
            if(!Ready())return false;
            if(bInspection&&(StudioPointRecordings::LiveStats().Readers||StudioPointRecordings::LiveStats().AllocatedValueBytes))return false;
            Test->TestEqual(TEXT("Original project restored"),M.Project.Id,Original.Id);
            Test->TestTrue(TEXT("Original exact camera restored"),StudioView::CameraEquals(M.Project.Camera,Original.Camera));
            Test->TestEqual(TEXT("Surface texture released after source replacement"),Scene->ResourceStats().ScalarTextureBytes,int64(0));
            Test->TestEqual(TEXT("Full source reader released"),StudioPointRecordings::LiveStats().Readers,0);
            Test->TestEqual(TEXT("Full source values released"),StudioPointRecordings::LiveStats().AllocatedValueBytes,int64(0));
            Test->TestTrue(TEXT("Save measured resource evidence"),FFileHelper::SaveStringToFile(
                TEXT("seconds,playback_ordinal,presented_step,cache_bytes,value_bytes,mesh_bytes,texture_bytes,workers,footprint_bytes,device_bytes\n")+Telemetry,*(Root/TEXT("resources.csv"))));
            Test->AddInfo(FString::Printf(TEXT("Natural 8000-frame surface playback: %d distinct displayed source steps, %d resource samples. This is full-sequence coverage, not an hour-long stability gate."),PresentedFrames,Samples.Num()));
            if(bInspection)
            {
                Test->AddInfo(FString::Printf(TEXT("Full recording inspection: %d distinct pressure profiles, source steps %d to %d, %d playback CSV exports."),ProfileFrames.Num(),EarliestProfile,LatestProfile,ProbeExports));
                Press(TEXT("CloseInspection"));
            }
            return true;
        case 12:
        {
            FString CSV;if(!FFileHelper::LoadFileToString(CSV,*FinalCSV))return false;
            FCsvParser Parser(CSV);const auto& Rows=Parser.GetRows();
            if(!Test->TestEqual(TEXT("Full-recording profile CSV has every sample"),Rows.Num(),33))return true;
            for(int32 I=1;I<Rows.Num();++I)
            {
                if(!Test->TestEqual(TEXT("Profile CSV retains every provenance column"),Rows[I].Num(),31))return true;
                Test->TestEqual(TEXT("Final profile CSV source step"),FCString::Atoi(Rows[I][13]),9000);
                Test->TestEqual(TEXT("Final profile CSV field stays pressure"),FString(Rows[I][15]),FString(TEXT("pressure")));
                const auto& Sample=FinalProfile.Snapshot.Samples[I-1];
                if(Sample.Value.IsSet())Test->TestEqual(TEXT("CSV retains displayed profile value exactly"),FCString::Atod(Rows[I][30]),Sample.Value.GetValue());
            }
            FinalProfile={};
            Test->TestTrue(TEXT("Save full recording with inspection"),M.SaveProject(Work/TEXT("inspection.lbms")));Saved=M.SnapshotProject();
            Test->TestTrue(TEXT("Reopen full recording inspection"),M.RequestProjectOpen(Work/TEXT("inspection.lbms")));Phase=7;break;
        }
        }
        return false;
    }
private:
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if(W->GetTag()==Tag)return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Find(Children->GetChildAt(I),Tag))return Found;
        return {};
    }
    TSharedPtr<SWidget> Control(FName Tag)
    {const auto W=GEngine->GameViewport->GetWindow();return W?Find(W.ToSharedRef(),Tag):nullptr;}
    void Press(FName Tag)
    {
        const auto W=Control(Tag);if(!Test->TestTrue(FString::Printf(TEXT("Inspection control %s is enabled"),*Tag.ToString()),W.IsValid()&&W->IsEnabled()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    TSharedPtr<const FStudioProbeProfile> CurrentProfile()
    {
        const auto Widget=Control(TEXT("InspectionProfile"));if(!Widget)return {};
        const auto Profile=StaticCastSharedPtr<SStudioProbeProfile>(Widget)->Profile.Get();if(!Profile)return {};
        const auto& R=Profile->Snapshot;const auto Identity=Scene->PresentedField()->Identity();
        if(!Test->TestTrue(TEXT("Actual profile and presented field are identified"),R.Identity.IsSet()&&Identity.IsSet()))return {};
        Test->TestEqual(TEXT("Actual profile matches presented source step"),R.Identity->Frame.Index,Identity->Frame.Index);
        Test->TestEqual(TEXT("Actual profile matches presented physical time"),R.Identity->Frame.Time,Identity->Frame.Time);
        Test->TestEqual(TEXT("Actual profile matches presented source"),R.Identity->Dataset,Identity->Dataset);
        Test->TestEqual(TEXT("Actual profile matches presented capture"),R.PresentationId,Scene->GetCaptureCount());
        Test->TestEqual(TEXT("Actual profile matches presented project"),R.ProjectId,Scene->PresentedProjectId());
        Test->TestEqual(TEXT("Actual profile samples pressure independently"),R.Field,FString(TEXT("pressure")));
        Test->TestEqual(TEXT("Actual profile retains source pressure unit"),R.Unit,FString(TEXT("Pa")));
        Test->TestEqual(TEXT("Viewport retains independent speed field"),Scene->PresentedScalar().Id,FString(TEXT("velocity_magnitude")));
        Test->TestTrue(TEXT("Actual pressure profile has supported physical samples"),Profile->ValidSamples>0&&R.Samples.Num()==32);
        return Profile;
    }
    void InspectPlayback(double Now)
    {
        const auto Profile=CurrentProfile();if(!Profile)return;
        const int32 Frame=Profile->Snapshot.Identity->Frame.Index;ProfileFrames.Add(Frame);EarliestProfile=FMath::Min(EarliestProfile,Frame);LatestProfile=FMath::Max(LatestProfile,Frame);
        if(Now-LastProbeExport<10)return;
        const auto Button=Control(TEXT("ExportProbeCSV"));if(!Button||!Button->IsEnabled())return;
        const FString Path=Root/FString::Printf(TEXT("playback-profile-%02d.csv"),ProbeExports);
        IFileManager::Get().Delete(*Path);StudioFileDialog::SetNextProbeCSVForAutomation(Path);Press(TEXT("ExportProbeCSV"));
        LastProbeExport=Now;++ProbeExports;
    }
    bool Ready() const{return !Scene->Model->IsRecordingLoadPending()&&!Scene->Model->IsProjectOpenPending()&&Scene->HasCurrentFrame();}
    uint32 Pixels()
    {
        TArray<FColor> P;
        Test->TestTrue(TEXT("Read actual surface pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(P));
        if(P.IsEmpty())return 0;
        int32 Nonuniform=0;for(auto& V:P){V.A=255;if(V!=P[0])++Nonuniform;}
        Test->TestTrue(TEXT("Full source GPU image is populated"),Nonuniform>P.Num()/100);
        return FCrc::MemCrc32(P.GetData(),P.Num()*sizeof(FColor));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> P;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture full-source workspace"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),P,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,P,PNG);
        Test->TestTrue(TEXT("Save full-source capture"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    void Sample(double Seconds)
    {
        const auto& M=*Scene->Model;const auto R=Scene->ResourceStats();const auto C=M.Solver->CacheStats();
        const auto P=StudioPointRecordings::LiveStats();const uint64 Footprint=FPlatformMemory::GetStats().UsedPhysical;
        const int64 Device=StudioPlatformDiagnostics::DeviceAllocatedBytes();Samples.Add({Seconds,Footprint,Device});
        Test->TestTrue(TEXT("One surface render worker"),R.Workers<=1);
        Test->TestTrue(TEXT("Full-source cache stays bounded"),C.ResidentBytes<=C.BudgetBytes);
        Test->TestTrue(TEXT("Full-source allocated values stay bounded"),P.AllocatedValueBytes<=48LL*1024*1024);
        Test->TestTrue(TEXT("Surface and named inspection geometry stay within their measured budget"),R.MeshBytes<=(bInspection?32LL:8LL)*1024*1024);
        Test->TestTrue(TEXT("Scalar texture stays present and within 1 MiB"),R.ScalarTextureBytes>0&&R.ScalarTextureBytes<=1024*1024);
        Test->TestTrue(TEXT("Process footprint within existing 6 GiB budget"),Footprint<=6ULL*1024*1024*1024);
        Test->TestTrue(TEXT("Measured Metal allocations within existing 2 GiB budget"),Device>0&&Device<=2LL*1024*1024*1024);
        Telemetry+=FString::Printf(TEXT("%.3f,%d,%d,%lld,%lld,%lld,%lld,%d,%llu,%lld\n"),Seconds,M.PlaybackFrame,
            Scene->PresentedFrame().Index,C.ResidentBytes,P.AllocatedValueBytes,R.MeshBytes,R.ScalarTextureBytes,R.Workers,Footprint,Device);
    }
    void CheckDrift()
    {
        Test->TestTrue(TEXT("Sustained playback resource sampling"),Samples.Num()>=90);
        double EarlyFoot=0,EarlyDevice=0,LateFoot=0,LateDevice=0;int32 Early=0,Late=0;
        for(const auto& S:Samples)
        {
            if(S.Seconds>=20&&S.Seconds<40){EarlyFoot+=S.Footprint;EarlyDevice+=S.Device;++Early;}
            if(S.Seconds>=80){LateFoot+=S.Footprint;LateDevice+=S.Device;++Late;}
        }
        if(!Test->TestTrue(TEXT("Warm and final resource windows covered"),Early>10&&Late>10))return;
        Test->TestTrue(TEXT("Warm footprint drift within existing 256 MiB tolerance"),LateFoot/Late-EarlyFoot/Early<=256.*1024*1024);
        Test->TestTrue(TEXT("Warm Metal drift within existing 128 MiB tolerance"),LateDevice/Late-EarlyDevice/Early<=128.*1024*1024);
        LastHash=Pixels();
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioProject Original,Saved;FStudioCameraState View;
    FString RecordingPath,SurfacePath,Root,Work,Source,Case,Telemetry;TFuture<FSurfaceSequenceAudit> Audit;FStudioLoadCancellation Cancel;
    TArray<FSurfaceResourceSample> Samples;int32 Phase=0,LastPresented=0,PresentedFrames=0;
    double Started=0,PlaybackStarted=0,LastSample=0,IdleStarted=0;uint64 IdleCaptures=0;uint32 FirstHash=0,LastHash=0;
    bool bInspection=false,bPanelOpened=false;FGuid LineId,RulerId;FStudioInspectionObjects ExpectedObjects;
    FStudioProbeProfile FinalProfile;FString FinalCSV;TSet<int32> ProfileFrames;
    int32 EarliestProfile=MAX_int32,LatestProfile=0,ProbeExports=0;double LastProbeExport=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceFullSequence,"ScientificAcceptance.Surface.FullSequence",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioSurfaceFullSequence::RunTest(const FString&)
{
    FString Recording,Surface;
    if(!FParse::Value(FCommandLine::Get(),TEXT("StudioPointRecording="),Recording)||
        !FParse::Value(FCommandLine::Get(),TEXT("StudioSurfaceReconstruction="),Surface))
    {AddError(TEXT("Supply the complete audited recording and explicit reconstruction paths."));return false;}
    ADD_LATENT_AUTOMATION_COMMAND(FStudioSurfaceSequenceCommand(this,Recording,Surface));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionFullSequence,"ScientificAcceptance.Inspection.FullSequence",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioInspectionFullSequence::RunTest(const FString&)
{
    FString Recording,Surface;
    if(!FParse::Value(FCommandLine::Get(),TEXT("StudioPointRecording="),Recording)||
        !FParse::Value(FCommandLine::Get(),TEXT("StudioSurfaceReconstruction="),Surface))
    {AddError(TEXT("Supply the complete audited recording and explicit reconstruction paths."));return false;}
    ADD_LATENT_AUTOMATION_COMMAND(FStudioSurfaceSequenceCommand(this,Recording,Surface,true));return true;
}
#endif
