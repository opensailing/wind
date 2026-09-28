#include "StudioModel.h"
#include "Async/Async.h"
#include "StudioBoundaries.h"

void FStudioModel::InvalidateDomainGeometry()
{
    InvalidateLatticePreview();
    const FString Key=StudioDomain::GeometryKey(Project.Draft);
    // Bounds, names and material assignments never reread mesh files.
    ++DomainPreviewRevision;
    if(DomainProject==Project.Id&&DomainGeometryKey==Key)return;
    if(DomainCancellation)*DomainCancellation=true;
    DomainProject=Project.Id;DomainGeometryKey=Key;DomainGeometry.Reset();
    DomainNotice=TEXT("Case geometry has not been checked.");
    bReloadDomainGeometry=Workspace==EStudioWorkspace::Domain||Workspace==EStudioWorkspace::BoundaryConditions||Workspace==EStudioWorkspace::Meshing;
}
bool FStudioModel::RequestDomainGeometry()
{
    if(IsProjectOpenPending()||IsRecordingLoadPending())
    {DomainNotice=TEXT("Wait for the project or recording to finish opening.");return false;}
    if(PendingDomainGeometry.IsValid())
    {DomainNotice=TEXT("A geometry check is already running. Cancel it before checking again.");return false;}
    InvalidateDomainGeometry();bReloadDomainGeometry=false;
    InvalidateLatticePreview(true);
    DomainCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    ReadingDomainProject=Project.Id;ReadingDomainKey=DomainGeometryKey;
    const auto Case=Project.Draft;
    PendingDomainGeometry=Async(EAsyncExecution::ThreadPool,[Case,Cancel=DomainCancellation.ToSharedRef()]
    {return StudioDomain::InspectGeometry(Case,Cancel);});
    DomainGeometry.Reset();++DomainPreviewRevision;
    DomainNotice=TEXT("Checking original geometry and transformed bounds…");return true;
}
void FStudioModel::CancelDomainGeometry()
{
    InvalidateLatticePreview(true);
    if(DomainCancellation)*DomainCancellation=true;
    DomainGeometry.Reset();bReloadDomainGeometry=false;++DomainPreviewRevision;
    DomainNotice=TEXT("Geometry check cancelled. Check again before fitting the domain.");
}
void FStudioModel::PollDomainGeometry()
{
    if(PendingDomainGeometry.IsValid()&&PendingDomainGeometry.IsReady())
    {
        auto Result=PendingDomainGeometry.Get();PendingDomainGeometry={};
        const bool Current=!DomainCancellation->load()&&!Result.bCancelled&&ReadingDomainProject==Project.Id&&
            ReadingDomainKey==DomainGeometryKey&&Result.Key==DomainGeometryKey;
        DomainCancellation.Reset();
        if(Current)
        {
            DomainGeometry=MakeShared<FStudioDomainGeometry,ESPMode::ThreadSafe>(MoveTemp(Result));++DomainPreviewRevision;
            DomainNotice=DomainGeometry->Objects.IsEmpty()?TEXT("Import case geometry to check containment and fit the domain."):
                DomainGeometry->Complete()?TEXT("Source check complete. Aggregate geometry bounds verified for all case objects."):
                TEXT("Some geometry could not be verified. Resolve the listed files before fitting.");
        }
    }
    if(bReloadDomainGeometry&&!PendingDomainGeometry.IsValid()&&(Workspace==EStudioWorkspace::Domain||Workspace==EStudioWorkspace::BoundaryConditions||Workspace==EStudioWorkspace::Meshing)&&
        !IsProjectOpenPending()&&!IsRecordingLoadPending())RequestDomainGeometry();
}
bool FStudioModel::UpdateDomain(const FStudioDomain& Domain)
{
    if(IsProjectOpenPending()||IsRecordingLoadPending())
    {Notice=TEXT("Wait for the project or recording to finish opening before editing the domain.");return false;}
    if(Domain.Id!=Project.Draft.Domain.Id||Domain.Faces!=Project.Draft.Domain.Faces)
    {Notice=TEXT("Domain edits must retain the domain and face identities.");return false;}
    return EditCase(TEXT("Edit domain"),[Domain](auto& Case)
    {const FGuid Fluid=Case.Domain.FluidMaterialId;Case.Domain=Domain;Case.Domain.FluidMaterialId=Fluid;});
}
void FStudioModel::SelectDomainFace(int32 Index)
{
    if(Index<0||Index>=6||Index==SelectedDomainFace)return;
    SelectedDomainFace=Index;++DomainPreviewRevision;
}
bool FStudioModel::SelectBoundaryTarget(const FGuid& Id)
{
    FStudioBoundaryTarget Target;if(!StudioBoundaries::FindTarget(Project.Draft,Id,Target))return false;
    if(SelectedBoundaryTarget!=Id){SelectedBoundaryTarget=Id;++DomainPreviewRevision;}
    return true;
}
