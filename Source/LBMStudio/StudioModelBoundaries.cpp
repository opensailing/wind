#include "StudioModel.h"
#include "StudioBoundaries.h"

bool FStudioModel::UpdateBoundary(const FStudioBoundaryCondition& Boundary,bool bUnpairExisting)
{
    if(IsProjectOpenPending()||IsRecordingLoadPending())
    {Notice=TEXT("Wait for the project or recording to finish opening before editing boundaries.");return false;}
    auto Candidate=Project.Draft;FString Error;
    if(!StudioBoundaries::Set(Candidate,Boundary,bUnpairExisting,Error)){Notice=Error;return false;}
    return EditCase(Boundary.Type==EStudioBoundaryType::Periodic?TEXT("Assign periodic pair"):bUnpairExisting?TEXT("Unpair and edit boundary"):TEXT("Edit boundary"),[Candidate](auto& Case){Case=Candidate;});
}
bool FStudioModel::RemoveBoundary(const FGuid& Target)
{
    if(IsProjectOpenPending()||IsRecordingLoadPending())
    {Notice=TEXT("Wait for the project or recording to finish opening before editing boundaries.");return false;}
    auto Candidate=Project.Draft;FString Error;
    if(!StudioBoundaries::Remove(Candidate,Target,Error)){Notice=Error;return false;}
    return EditCase(TEXT("Remove boundary assignment"),[Candidate](auto& Case){Case=Candidate;});
}
