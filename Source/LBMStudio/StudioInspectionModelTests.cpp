#include "StudioModel.h"
#include "StudioProbeSampling.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto InspectionModelFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString InspectionSession(){return FPaths::ProjectDir()/TEXT("tmp/debug/inspection-model")/FGuid::NewGuid().ToString();}
FString InspectionJSON(const TSharedRef<FJsonObject>& O)
{FString Text;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));return Text;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionProject,"Studio.Inspection.ProjectPersistenceAndMigration",InspectionModelFlags)
bool FStudioInspectionProject::RunTest(const FString&)
{
    const FString Dir=InspectionSession();FStudioModel M(Dir);
    FStudioSliceObject A;A.Name=TEXT("Wing plane");A.Origin=FVector(.12345678901234567,0,.03);
    FStudioSliceObject B;B.Name=TEXT("Cross plane");B.Normal=FVector(1,2,3).GetSafeNormal();B.Origin=FVector(.1,0,.02);
    FStudioProbeObject P;P.Name=TEXT("Pressure reference");P.Field=TEXT("pressure");P.A=FVector(.03250676393508911,0,.1194048523902893);
    FStudioRulerObject R;R.Name=TEXT("Known distance");R.A=FVector::ZeroVector;R.B=FVector(.03,0,.04);R.Unit=TEXT("mm");
    if(!TestTrue(TEXT("Create two independently oriented slices and source-bound probe/ruler"),M.AddSlice(A)&&M.AddSlice(B)&&M.AddProbe(P)&&M.AddRuler(R)))return false;
    const auto Expected=M.InspectionObjects;const auto Camera=M.Project.Camera;const auto ProjectId=M.Project.Id;
    const FString Path=Dir/TEXT("saved-inspection.lbms");
    TestTrue(TEXT("Save authored inspection"),M.SaveProject(Path));M.NewProject(TEXT("Different project"));
    TestTrue(TEXT("New project has independent objects"),M.InspectionObjects.Slices.IsEmpty()&&M.InspectionObjects.Probes.IsEmpty()&&M.InspectionObjects.Rulers.IsEmpty());
    if(!TestTrue(TEXT("Reopen saved inspection"),M.LoadProject(Path)))return false;
    TestTrue(TEXT("Every object and all physical coordinates restore exactly"),M.InspectionObjects==Expected);
    TestEqual(TEXT("Project identity survives"),M.Project.Id,ProjectId);
    TestTrue(TEXT("Camera survives object persistence"),StudioView::CameraEquals(M.Project.Camera,Camera));
    TestFalse(TEXT("Reopen starts with no stale undo"),M.CanUndoView());
    TestFalse(TEXT("Reopened document is clean"),M.HasUnsavedChanges());
    TestTrue(TEXT("Known physical distance after reopen"),FMath::IsNearlyEqual(StudioInspectionObjects::Measurement(*M.FindRuler(R.Id)).GetValue(),50.,1.e-12));
    FStudioProbeRequest Request;Request.ProjectId=M.Project.Id;Request.PresentationId=1;Request.Probe=*M.FindProbe(P.Id);
    Request.DisplayedScalar=TEXT("pressure");Request.Field=M.Solver->CaptureViewField(0,TEXT("pressure"),false);
    const auto Sample=StudioProbeSampling::Evaluate(Request);
    if(!TestTrue(TEXT("Reopened probe samples its original source"),Sample.Samples.Num()==1&&Sample.Samples[0].Value.IsSet()))return false;
    TestTrue(TEXT("Reopened pressure matches independent original decode"),FMath::IsNearlyEqual(Sample.Samples[0].Value.GetValue(),98699.78125,1.e-5));

    const FString Serialized=StudioProjectIO::Serialize(M.SnapshotProject());TSharedPtr<FJsonObject> JSON;
    TestTrue(TEXT("Read project JSON for migration cases"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Serialized),JSON));
    TestEqual(TEXT("Inspection schema version"),int32(JSON->GetNumberField(TEXT("version"))),FStudioProject::CurrentVersion);
    auto View=JSON->GetObjectField(TEXT("view"));View->RemoveField(TEXT("inspectionObjects"));
    FStudioProject Out=M.SnapshotProject();const auto Kept=Out;FString Error;
    TestFalse(TEXT("Current schema requires inspection state"),StudioProjectIO::Parse(InspectionJSON(JSON.ToSharedRef()),Out,Error));
    TestTrue(TEXT("Rejected project keeps all existing objects"),Out.View.InspectionObjects==Kept.View.InspectionObjects);
    JSON->SetNumberField(TEXT("version"),12);
    TestTrue(TEXT("Volume-only schema migrates with empty inspection objects"),StudioProjectIO::Parse(InspectionJSON(JSON.ToSharedRef()),Out,Error));
    TestTrue(TEXT("Migration creates no fictitious probes or measurements"),Out.View.InspectionObjects.Probes.IsEmpty()&&Out.View.InspectionObjects.Rulers.IsEmpty());
    TestEqual(TEXT("Volume controls survive migration"),Out.View.VolumeStepVoxels,M.VolumeStepVoxels);
    JSON->SetNumberField(TEXT("version"),11);
    TestTrue(TEXT("Pre-volume project also migrates"),StudioProjectIO::Parse(InspectionJSON(JSON.ToSharedRef()),Out,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionHistory,"Studio.Inspection.SharedHistoryAndRenderIsolation",InspectionModelFlags)
bool FStudioInspectionHistory::RunTest(const FString&)
{
    FStudioModel M(InspectionSession());FStudioSliceObject S;S.Name=TEXT("Slice");TestTrue(TEXT("Add slice"),M.AddSlice(S));
    const auto Start=*M.FindSlice(S.Id);const auto Camera=M.Project.Camera;
    const FString Case=StudioCaseIO::Serialize(M.Project.Draft);M.Run();M.Tick(.1);M.Scrub(.6);
    const int32 Frame=M.SelectedFrame,Replay=M.PlaybackFrame,BeforeRevision=M.Revision;
    M.BeginViewEdit(TEXT("Move slice"));
    for(int32 I=1;I<=50;++I)TestTrue(TEXT("Edit physical slice position"),M.EditSlice(S.Id,[I](auto& O){O.Origin.X=I*.001;}));
    M.EndViewEdit();const auto Moved=*M.FindSlice(S.Id);
    TestTrue(TEXT("Visible slice motion invalidates field geometry"),M.Revision>BeforeRevision);
    TestTrue(TEXT("One undo restores entire slice gesture"),M.UndoView()&&*M.FindSlice(S.Id)==Start);
    TestTrue(TEXT("One redo restores exact final slice"),M.RedoView()&&*M.FindSlice(S.Id)==Moved);
    const int32 Render=M.Revision;const auto Intent=M.RenderIntentRevision;
    FStudioProbeObject P;P.Name=TEXT("Probe");P.A=FVector(.1,0,.03);TestTrue(TEXT("Add point probe"),M.AddProbe(P));
    FStudioRulerObject R;R.Name=TEXT("Ruler");R.B=FVector(1,0,0);TestTrue(TEXT("Add ruler"),M.AddRuler(R));
    TestTrue(TEXT("Move probe"),M.EditProbe(P.Id,[](auto& O){O.A.Z=.04;}));
    TestTrue(TEXT("Change distance unit"),M.EditRuler(R.Id,[](auto& O){O.Unit=TEXT("mm");}));
    TestTrue(TEXT("Rename slice"),M.RenameInspectionObject(S.Id,TEXT("Renamed slice")));
    TestEqual(TEXT("Probe/ruler/label edits require no CFD geometry rebuild"),M.Revision,Render);
    TestEqual(TEXT("Overlay edits do not obsolete an in-flight field"),M.RenderIntentRevision,Intent);
    TestTrue(TEXT("Overlay edits are in shared undo"),M.UndoView());
    TestEqual(TEXT("Undo restores slice name"),M.FindSlice(S.Id)->Name,Moved.Name);
    TestEqual(TEXT("Object edits retain selected physical frame"),M.SelectedFrame,Frame);
    TestEqual(TEXT("Object edits retain replay cursor"),M.PlaybackFrame,Replay);
    TestTrue(TEXT("Object edits retain live playback"),M.State==EStudioRunState::Running&&M.bReviewing);
    TestTrue(TEXT("Object edits retain camera"),StudioView::CameraEquals(M.Project.Camera,Camera));
    TestEqual(TEXT("Object edits retain solver case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionLifecycle,"Studio.Inspection.ObjectLifecycleAndSourceGuards",InspectionModelFlags)
bool FStudioInspectionLifecycle::RunTest(const FString&)
{
    FStudioModel M(InspectionSession());FStudioSliceObject S;TestTrue(TEXT("Unnamed object gets a unique name and current source"),M.AddSlice(S));
    const auto Source=M.InspectionSource();TestTrue(TEXT("Created object is source-bound"),M.FindSlice(S.Id)->Source==Source);
    const auto Original=*M.FindSlice(S.Id);TestTrue(TEXT("Duplicate object"),M.DuplicateInspectionObject(S.Id));
    const auto CopyId=M.SelectedInspectionObject;const auto Copy=*M.FindSlice(CopyId);
    TestTrue(TEXT("Duplicate gets its own identity and name"),CopyId!=S.Id&&Copy.Name!=Original.Name);
    TestTrue(TEXT("Duplicate retains exact geometry and source"),Copy.Origin==Original.Origin&&Copy.Normal==Original.Normal&&Copy.Source==Original.Source);
    TestTrue(TEXT("Hide duplicate"),M.SetInspectionObjectVisible(CopyId,false));
    TestTrue(TEXT("Visibility has undo"),M.UndoView()&&M.FindSlice(CopyId)->bVisible);
    TestTrue(TEXT("Delete selected duplicate"),M.DeleteInspectionObject(CopyId));
    TestFalse(TEXT("Deleted selection is cleared"),M.SelectedInspectionObject.IsValid());
    TestTrue(TEXT("Deletion undo restores stable identity"),M.UndoView()&&M.FindSlice(CopyId)!=nullptr);
    const auto Before=M.InspectionObjects;
    TestFalse(TEXT("Names must be unique"),M.RenameInspectionObject(CopyId,Original.Name.ToUpper()));
    TestFalse(TEXT("Unknown object cannot be edited"),M.EditSlice(FGuid::NewGuid(),[](auto& O){O.Origin.X=2.;}));
    TestFalse(TEXT("Existing source cannot be reassigned"),M.EditSlice(S.Id,[](auto& O){O.Source.MetadataSHA256=FString::ChrN(64,'f');}));
    TestFalse(TEXT("Invalid normal is not normalized silently"),M.EditSlice(S.Id,[](auto& O){O.Normal=FVector::ZeroVector;}));
    TestFalse(TEXT("Stable ID cannot be replaced by an edit"),M.EditSlice(S.Id,[](auto& O){O.Id=FGuid::NewGuid();}));
    TestTrue(TEXT("Rejected edits retain all objects"),M.InspectionObjects==Before);
    auto Foreign=S;Foreign.Id=FGuid::NewGuid();Foreign.Source=Source;Foreign.Source.MetadataSHA256=FString::ChrN(64,'f');
    TestFalse(TEXT("New objects cannot impersonate another source"),M.AddSlice(Foreign));
    // Simulate the atomic source swap; frame transport is tested separately.
    const FString OtherPath=StudioRecordings::PathForId(TEXT("MeshGraphNets_Airfoil_test010"));
    if(!TestFalse(TEXT("Second independently recorded source is installed"),OtherPath.IsEmpty()))return false;
    auto SavedSource=M.Solver;M.Solver=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>(OtherPath);
    if(!TestFalse(TEXT("Fixture selects a different scientific source"),M.InspectionSource()==Source))return false;
    TestFalse(TEXT("Inactive-source object cannot be moved"),M.EditSlice(S.Id,[](auto& O){O.Origin.X=2.;}));
    TestTrue(TEXT("Inactive-source object can still be renamed"),M.RenameInspectionObject(S.Id,TEXT("Archived slice")));
    TestTrue(TEXT("Rename retains original scientific source"),M.FindSlice(S.Id)->Source==Source);
    M.Solver=SavedSource;M.NewProject(TEXT("New inspection"));
    TestTrue(TEXT("Project replacement clears objects and selection"),M.InspectionObjects.Slices.IsEmpty()&&!M.SelectedInspectionObject.IsValid());
    TestFalse(TEXT("Prior project objects cannot return through undo"),M.CanUndoView());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionHistoryBudget,"Studio.Inspection.BoundedObjectHistory",InspectionModelFlags)
bool FStudioInspectionHistoryBudget::RunTest(const FString&)
{
    // Geometry-only stress document; contains no synthetic CFD values.
    FStudioInspectionState State;
    const FStudioInspectionSource Source{FString::ChrN(256,'d'),FString::ChrN(64,'a'),FString::ChrN(64,'b')};
    for(int32 I=0;I<StudioInspectionObjects::MaxObjectsPerKind;++I)
    {
        const FString Suffix=FString::FromInt(I);
        FStudioSliceObject S;S.Source=Source;S.Name=FString::ChrN(110,'s')+Suffix;State.Display.InspectionObjects.Slices.Add(S);
        FStudioProbeObject P;P.Source=Source;P.Name=FString::ChrN(110,'p')+Suffix;P.Field=FString::ChrN(128,'f');State.Display.InspectionObjects.Probes.Add(P);
        FStudioRulerObject R;R.Source=Source;R.Name=FString::ChrN(110,'r')+Suffix;State.Display.InspectionObjects.Rulers.Add(R);
    }
    TestTrue(TEXT("Stress document obeys object limits"),StudioView::IsValid(State));
    FStudioViewHistory History;
    for(int32 I=1;I<=100;++I)
    {
        auto Next=State;Next.Camera.Position.X=I;
        History.Record(TEXT("Move camera with saved inspection"),State,Next);State=MoveTemp(Next);
        TestTrue(TEXT("Stored history stays within its memory budget"),History.StoredBytes()<=FStudioViewHistory::MaxStoredBytes);
    }
    int32 Count=0;FStudioInspectionState Restored;FString Label;
    while(History.Restore(false,State,Restored,Label)){State=MoveTemp(Restored);++Count;}
    TestTrue(TEXT("Large documents retain usable history within both bounds"),Count>1&&Count<FStudioViewHistory::MaxEntries);
    TestTrue(TEXT("Undo-to-redo transfer cannot increase the stored budget"),History.StoredBytes()<=FStudioViewHistory::MaxStoredBytes);
    History.Clear();TestEqual(TEXT("Clear releases all stored inspection states"),History.StoredBytes(),int64(0));
    return true;
}
#endif
