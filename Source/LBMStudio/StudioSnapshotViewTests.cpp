#include "StudioSnapshotSource.h"
#include "StudioComparison.h"
#include "StudioPointRecording.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto SnapshotTestFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString SnapshotFixture(const TCHAR* Name)
{return FPaths::ProjectContentDir()/TEXT("Samples")/Name;}
TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> FreezeSnapshot(const IStudioSolver& Source,int32 Ordinal,FString& Error)
{
    const FString Scalar=TEXT("pressure");const auto Read=Source.ReadScalarFrame(Ordinal,Scalar);
    const auto* S=Source.Descriptor().Scalars.FindByPredicate([&](const auto& F){return F.Id==Scalar;});
    if(!S||!Read.Field){Error=Read.Error;return {};}
    return FStudioSnapshotSource::Create(Source,Ordinal,*S,Read.Field,Error);
}
struct FSnapshotTestFiles
{
    FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/SnapshotViews")/FGuid::NewGuid().ToString();
    ~FSnapshotTestFiles(){IFileManager::Get().DeleteDirectory(*Root,false,true);}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotOriginal,"Studio.SnapshotView.ExactOriginalScalarAndFrame",SnapshotTestFlags)
bool FSnapshotOriginal::RunTest(const FString&)
{
    const auto Source=StudioRecordings::Import(SnapshotFixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),1,{}).Source;
    if(!TestTrue(TEXT("Original CFD loads"),Source.IsValid()))return false;
    FString Error;const auto Frozen=FreezeSnapshot(*Source,1,Error);
    if(!TestTrue(*Error,Frozen.IsValid()))return false;
    const auto Read=Frozen->ReadScalarFrame(1,TEXT("pressure"));
    const auto Points=Read.Field->OriginalPoints();
    TestTrue(TEXT("Original array is retained with its exact source step and time"),Points.IsValid()&&
        Read.Field->Identity()->Ordinal==1&&Read.Field->Identity()->Frame.Index==5001&&Read.Field->Identity()->Frame.Time==12.5025);
    TestEqual(TEXT("Original timeline ordinals are not renumbered"),Frozen->FrameCount(),3);
    TestEqual(TEXT("Metadata retains last source step"),Frozen->EvaluateFrame(2).Index,9000);
    TestTrue(TEXT("Capture returns the pinned original field"),&Frozen->CaptureViewField(1,TEXT("pressure"),false).Get()==Read.Field.Get());
    TestTrue(TEXT("Previous and next frames never fall back to the pinned frame"),
        !Frozen->CaptureField(0)->IsValid()&&!Frozen->CaptureField(2)->IsValid());
    TestTrue(TEXT("Another scalar is explicitly unavailable"),!Frozen->ReadScalarFrame(1,TEXT("velocity_x")).Field&&
        !Frozen->CaptureViewField(1,TEXT("velocity_x"),false)->IsValid());
    TestTrue(TEXT("A scalar-only snapshot never supplies invented vectors"),!Frozen->CaptureViewField(1,TEXT("pressure"),true)->IsValid());
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestTrue(TEXT("Cancellation returns no snapshot"),!Frozen->ReadScalarFrame(1,TEXT("pressure"),Cancel).Field&&
        !Frozen->CaptureViewField(1,TEXT("pressure"),false,Cancel)->IsValid());
    TestTrue(TEXT("Rejected requests do not poison the retained field"),Frozen->LoadError().IsEmpty()&&Frozen->ReadScalarFrame(1,TEXT("pressure")).Field==Read.Field);
    auto S=*Source->Descriptor().Scalars.FindByPredicate([](const auto& F){return F.Id==TEXT("pressure");});
    TestTrue(TEXT("Wrong original ordinal rejected before publication"),!FStudioSnapshotSource::Create(*Source,0,S,Read.Field,Error));
    S.Maximum+=1;
    TestTrue(TEXT("Caller cannot relabel the original supplied range"),!FStudioSnapshotSource::Create(*Source,1,S,Read.Field,Error));
    const auto* Unloaded=Source->Descriptor().Scalars.FindByPredicate([](const auto& F){return F.Id!=TEXT("pressure");});
    if(Unloaded)TestTrue(TEXT("Metadata alone cannot claim an unloaded point array"),!FStudioSnapshotSource::Create(*Source,1,*Unloaded,Read.Field,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotCameras,"Studio.SnapshotView.IndependentCameraAndPlayback",SnapshotTestFlags)
bool FSnapshotCameras::RunTest(const FString&)
{
    FSnapshotTestFiles Files;FStudioModel Solve(Files.Root/TEXT("solve"));Solve.ReviewRecordedFrame(420);
    const FString Original=StudioProjectIO::Serialize(Solve.SnapshotProject());const auto Source=Solve.Solver;
    const auto State=Solve.State;FString Error;
    const auto Frozen=FreezeSnapshot(*Source,420,Error);
    if(!TestTrue(*Error,Frozen.IsValid()))return false;
    const auto Cache=Source->CacheStats();
    FStudioModel A(Frozen.ToSharedRef()),B(Frozen.ToSharedRef());
    TestTrue(TEXT("Independent transient models use no new recording readers"),A.IsSnapshotView()&&B.IsSnapshotView()&&A.Solver==Frozen&&B.Solver==Frozen);
    TestTrue(TEXT("Snapshot cameras start fitted to the source bounds"),StudioView::IsValid(A.InspectionState()));
    TestTrue(TEXT("Renderer selects the exact scalar and original ordinal"),A.SelectedFrame==420&&A.PlaybackFrame==420&&
        A.DisplayFrame().Time==Frozen->EvaluateFrame(420).Time&&A.ActiveScalar().Id==TEXT("pressure")&&!A.bVectors&&!A.bStreamlines);
    const auto BeforeB=B.InspectionState();auto Camera=A.Project.Camera;Camera.Position+=FVector(.3,.2,.1);Camera.Focus+=FVector(.3,.2,.1);
    TestTrue(TEXT("Comparison camera can be placed independently"),A.EditCamera(TEXT("Move comparison A"),Camera));
    TestTrue(TEXT("Other comparison camera retained"),B.InspectionState().Equals(BeforeB));
    TestTrue(TEXT("Independent camera undo remains available"),A.UndoView()&&A.InspectionState().Equals(BeforeB));
    TestTrue(TEXT("Display edits remain usable"),A.SetScalarStyle(0,true,-100,100));
    TestTrue(TEXT("Changing field requires a new verified snapshot"),!A.EditView(TEXT("Unavailable field"),[](auto& V){V.Display.ScalarField=TEXT("velocity_magnitude");}));
    TestTrue(TEXT("No vector loading through a snapshot view"),!A.EditView(TEXT("Vectors"),[](auto& V){V.Display.bVectors=true;}));
    A.Run();A.Pause();A.Step();A.Stop();A.Reset();A.Scrub(0);A.ReturnToLive();
    // Exercise the autosave boundary and a long-lived paused view without sleeping.
    for(int32 I=0;I<1801;++I){A.Tick(1);B.Tick(1);}
    TestTrue(TEXT("Thirty minutes of view ticks cannot advance the original frame"),A.SelectedFrame==420&&A.PlaybackFrame==420&&A.State==EStudioRunState::Ready);
    TestTrue(TEXT("A frozen view has no unsaved project prompt"),!A.HasUnsavedChanges()&&!A.bDirty);
    TestTrue(TEXT("Solve camera, settings, cursor and state retained"),Original==StudioProjectIO::Serialize(Solve.SnapshotProject())&&Solve.State==State&&Solve.Solver==Source);
    TestEqual(TEXT("Camera and view ticks perform no additional source frame loads"),Source->CacheStats().Loads,Cache.Loads);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotPersistence,"Studio.SnapshotView.ProjectAndSessionIsolation",SnapshotTestFlags)
bool FSnapshotPersistence::RunTest(const FString&)
{
    FSnapshotTestFiles Files;FStudioModel Solve(Files.Root);FString Error;
    const FString ProjectPath=Files.Root/TEXT("case.lbms");
    if(!TestTrue(TEXT("Isolated saved case exists"),Solve.SaveProject(ProjectPath)))return false;
    auto Moved=Solve.Project.Camera;Moved.Position+=FVector(.3,.2,.1);Moved.Focus+=FVector(.3,.2,.1);
    Solve.EditCamera(TEXT("Unsaved source camera"),Moved);
    Solve.WriteRecovery();
    const TArray<FString> Paths={ProjectPath,Files.Root/TEXT("StudioSession.json"),Files.Root/TEXT("Recovery/StudioRecovery.lbms")};
    TArray<FString> Before;
    for(const auto& Path:Paths){FString Text;if(!TestTrue(TEXT("Persistence sentinel readable"),FFileHelper::LoadFileToString(Text,*Path)))return false;Before.Add(Text);}
    const auto Frozen=FreezeSnapshot(*Solve.Solver,20,Error);if(!TestTrue(*Error,Frozen.IsValid()))return false;
    FStudioModel View(Frozen.ToSharedRef());View.ProjectPath=ProjectPath;View.PendingRecovery=Paths[2];
    auto Camera=View.Project.Camera;Camera.Position.X+=.1;View.EditCamera(TEXT("Compare camera"),Camera);
    TestTrue(TEXT("Snapshot cannot overwrite an active project"),!View.SaveProject(ProjectPath));
    TestTrue(TEXT("Snapshot cannot create a project duplicate"),!View.DuplicateProject(Files.Root/TEXT("copy.lbms"),TEXT("copy")));
    TestTrue(TEXT("Snapshot cannot alter a saved project's favorites"),!View.SetProjectFavorite(ProjectPath,true));
    TestTrue(TEXT("Snapshot cannot load or asynchronously replace a project"),!View.LoadProject(ProjectPath)&&!View.RequestProjectOpen(ProjectPath));
    TestTrue(TEXT("Snapshot cannot replace its recording"),!View.RequestRecording(TEXT("MeshGraphNets_Airfoil_test010")));
    TestTrue(TEXT("Snapshot cannot start the control harness"),!View.SetControlHarness(true)&&!View.CanControl(EStudioJobCommand::Submit)&&!View.Control(EStudioJobCommand::Submit));
    bool bEdited=false;
    TestTrue(TEXT("Snapshot rejects case edits"),!View.EditCase(TEXT("Case edit"),[&](auto&){bEdited=true;})&&!bEdited);
    View.WriteRecovery();View.DiscardRecovery();View.SaveSession();View.OpenSession();View.Tick(1801);
    TestTrue(TEXT("Snapshot cannot restore recovery into a comparison"),!View.RestoreRecovery());
    for(int32 I=0;I<Paths.Num();++I){FString Text;TestTrue(TEXT("Original persisted bytes survive inspection"),FFileHelper::LoadFileToString(Text,*Paths[I])&&Text==Before[I]);}
    TestTrue(TEXT("No snapshot project was created"),!IFileManager::Get().FileExists(*(Files.Root/TEXT("copy.lbms"))));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotTopology,"Studio.SnapshotView.OriginalSurfaceAndVolumeTopology",SnapshotTestFlags)
bool FSnapshotTopology::RunTest(const FString&)
{
    for(const bool bVolume:{false,true})
    {
        const auto Raw=StudioRecordings::Import(SnapshotFixture(bVolume?TEXT("Cylinder3D_ReaderFixture"):TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),0,{});
        if(!TestTrue(TEXT("Original point reference loads"),Raw.Source&&Raw.Reference.IsSet()))return false;
        const auto Reconstructed=StudioRecordings::ImportReconstruction(*Raw.Reference,
            SnapshotFixture(bVolume?TEXT("Cylinder3D_VolumeFixture"):TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),0,{});
        if(!TestTrue(*Reconstructed.Error,Reconstructed.Source.IsValid()))return false;
        FString Error;const auto Frozen=FreezeSnapshot(*Reconstructed.Source,2,Error);
        if(!TestTrue(*Error,Frozen.IsValid()))return false;
        FStudioModel View(Frozen.ToSharedRef());const auto Field=Frozen->CaptureField(2);const auto Identity=Field->Identity();
        TestTrue(TEXT("Existing renderer receives the original points"),Field->OriginalPoints().IsValid());
        TestTrue(TEXT("Reconstruction identity remains explicit"),Identity.IsSet()&&!Identity->ReconstructionSHA256.IsEmpty()&&
            Identity->Interpolation==(bVolume?EStudioFieldInterpolation::ReconstructedGrid:EStudioFieldInterpolation::ReconstructedTriangles));
        TestTrue(TEXT("Existing surface or volume mapping is retained directly"),Frozen->Reconstruction()==Reconstructed.Source->Reconstruction()&&
            Frozen->VolumeReconstruction()==Reconstructed.Source->VolumeReconstruction());
        TestTrue(TEXT("Snapshot view enables the verified topology"),bVolume?View.bVolume:View.bReconstructedSurface);
        TestTrue(TEXT("Camera valid for actual 2D or 3D bounds"),StudioView::IsValid(View.InspectionState()));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotOwnership,"Studio.SnapshotView.PairedPublicationAndRelease",SnapshotTestFlags)
bool FSnapshotOwnership::RunTest(const FString&)
{
    const auto Before=StudioRecordings::LiveStats();
    TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> SourceLifetime;
    TWeakPtr<const IStudioField,ESPMode::ThreadSafe> FieldLifetime;
    TSharedPtr<FStudioModel> View;
    {
        const auto Source=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>();SourceLifetime=Source;
        FStudioComparisonRequest R;R.ProjectId=FGuid::NewGuid();R.Primary=Source;R.Secondary=Source;
        R.PrimaryOrdinal=420;R.Scalar=TEXT("pressure");R.Alignment.Mode=EStudioTimeAlignment::RecordedTime;
        auto Result=StudioComparison::Evaluate(R);
        if(!TestTrue(TEXT("Paired read publishes two usable adapters"),Result.Matches(R)&&Result.Primary.Snapshot&&Result.Secondary.Snapshot))return false;
        FieldLifetime=Result.Primary.Field;View=MakeShared<FStudioModel>(Result.Primary.Snapshot.ToSharedRef());
        Result.Secondary.Snapshot.Reset();TestTrue(TEXT("Incomplete renderer pair is never current"),!Result.Matches(R));
    }
    TestTrue(TEXT("Renderer adapter does not own the original solver object"),!SourceLifetime.IsValid());
    TestTrue(TEXT("Pinned authentic field survives its reader owner"),FieldLifetime.IsValid()&&View->Solver->CaptureViewField(420,TEXT("pressure"),false)->IsValid());
    View.Reset();
    TestTrue(TEXT("Closing the view releases its snapshot"),!FieldLifetime.IsValid());
    const auto After=StudioRecordings::LiveStats();
    TestTrue(TEXT("No original CFD buffers leaked"),After.Readers==Before.Readers&&After.Frames==Before.Frames&&After.FrameBytes==Before.FrameBytes);
    return true;
}
#endif
