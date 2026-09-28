#include "StudioModel.h"
#include "StudioLattice.h"
#include "Async/Async.h"

void FStudioModel::InvalidateLatticePreview(bool bForce)
{
    if(!LatticePreview&&!PendingLatticePreview.IsValid())return;
    if(!bForce&&LatticeProject==Project.Id&&LatticeKey==StudioLattice::PreviewKey(Project.Draft,LatticeSettings))return;
    if(LatticeCancellation)*LatticeCancellation=true;
    LatticePreview.Reset();LatticeProgress.Reset();++DomainPreviewRevision;
    LatticeNotice=TEXT("The case or geometry changed. Preview the applied lattice again.");
}
bool FStudioModel::RequestLatticePreview(const FStudioLatticePreviewSettings& Settings)
{
    if(IsProjectOpenPending()||IsRecordingLoadPending()||IsReadingDomainGeometry())
    {LatticeNotice=TEXT("Wait for the project and geometry checks to finish.");return false;}
    if(PendingLatticePreview.IsValid())
    {LatticeNotice=TEXT("The previous preview is still finishing. Wait before starting another.");return false;}
    FStudioLatticeLayout Layout;FStudioLatticeSamplePlan Plan;FString Error;
    if(!StudioLattice::Layout(Project.Draft.Domain,Project.Draft.Setup.LatticeResolution,Layout,Error)||!StudioLattice::SamplePlan(Layout,Settings,Plan,Error))
    {LatticeNotice=Error;return false;}
    if(!Project.Draft.Geometry.IsEmpty()&&(!DomainGeometry||DomainGeometry->Key!=StudioDomain::GeometryKey(Project.Draft)))
    {LatticeNotice=TEXT("Check original case geometry before previewing cells.");return false;}
    LatticeSettings=Settings;LatticeProject=Project.Id;LatticeKey=StudioLattice::PreviewKey(Project.Draft,Settings);
    LatticeCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const auto Progress=MakeShared<FStudioLatticePreviewProgress,ESPMode::ThreadSafe>();LatticeProgress=Progress;
    const auto Case=Project.Draft;const auto Geometry=DomainGeometry;
    PendingLatticePreview=Async(EAsyncExecution::ThreadPool,[Case,Geometry,Settings,Progress,Cancel=LatticeCancellation.ToSharedRef()]
    {return StudioLattice::Preview(Case,Geometry,Settings,Cancel,Progress);});
    LatticePreview.Reset();++DomainPreviewRevision;LatticeNotice=TEXT("Classifying original cells against case geometry…");return true;
}
void FStudioModel::CancelLatticePreview()
{
    if(LatticeCancellation)*LatticeCancellation=true;
    LatticePreview.Reset();LatticeProgress.Reset();++DomainPreviewRevision;
    LatticeNotice=PendingLatticePreview.IsValid()?TEXT("Cancelling preview. Waiting for the worker to finish…"):TEXT("Preview cleared.");
}
void FStudioModel::PollLatticePreview()
{
    if(!PendingLatticePreview.IsValid()||!PendingLatticePreview.IsReady())return;
    auto Result=PendingLatticePreview.Get();PendingLatticePreview={};
    const bool Current=!LatticeCancellation->load()&&!Result.bCancelled&&LatticeProject==Project.Id&&
        LatticeKey==Result.Key&&LatticeKey==StudioLattice::PreviewKey(Project.Draft,LatticeSettings);
    const bool Cancelled=LatticeCancellation->load()||Result.bCancelled;
    LatticeCancellation.Reset();LatticeProgress.Reset();
    if(Current)
    {
        if(Result.Complete())
        {
            LatticePreview=MakeShared<FStudioLatticePreview,ESPMode::ThreadSafe>(MoveTemp(Result));++DomainPreviewRevision;
            LatticeNotice=TEXT("Cell preview ready. Geometry occupancy is an authoring aid; backend preparation is still required.");
        }
        else LatticeNotice=Result.Error.IsEmpty()?TEXT("The cell preview did not complete."):Result.Error;
    }
    else if(Cancelled)LatticeNotice=TEXT("Preview cancelled. Ready to preview the current case.");
}

bool FStudioModel::UpdateLatticeResolution(const FIntVector& Resolution)
{
    if(IsProjectOpenPending()||IsRecordingLoadPending())
    {Notice=TEXT("Wait for the project or recording to finish opening before editing the lattice.");return false;}
    FStudioLatticeLayout Grid;FString Error;
    if(!StudioLattice::Layout(Project.Draft.Domain,Resolution,Grid,Error)){Notice=Error;return false;}
    return EditCase(TEXT("Edit lattice resolution"),[Resolution](auto& Case){Case.Setup.LatticeResolution=Resolution;});
}
