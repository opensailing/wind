#include "StudioModel.h"
#include "StudioMaterials.h"

namespace
{
bool CanEditMaterials(FStudioModel& Model)
{
    if(Model.IsProjectOpenPending()||Model.IsRecordingLoadPending())
    {Model.Notice=TEXT("Wait for the project or recording to finish opening before editing materials.");return false;}
    return true;
}
FString NewMaterialName(const FStudioCaseDraft& Case,const FString& Base)
{
    auto Used=[&](const FString& Name){return Case.Materials.ContainsByPredicate([&](const auto& M){return M.Name.Equals(Name,ESearchCase::IgnoreCase);});};
    if(!Used(Base))return Base;
    for(int32 I=2;I<=258;++I)
    {const FString Name=Base.Left(112)+FString::Printf(TEXT(" %d"),I);if(!Used(Name))return Name;}
    return Base.Left(100)+TEXT(" copy");
}
}
bool FStudioModel::AddMaterial(bool bSolid,FGuid& OutId)
{
    if(!CanEditMaterials(*this))return false;
    if(Project.Draft.Materials.Num()>=256){Notice=TEXT("A case can contain at most 256 materials.");return false;}
    FStudioMaterial New;New.bSolid=bSolid;New.Name=NewMaterialName(Project.Draft,bSolid?TEXT("Solid"):TEXT("Fluid"));
    if(!EditCase(TEXT("Add material"),[New](auto& D){D.Materials.Add(New);}))return false;
    OutId=New.Id;return true;
}
bool FStudioModel::UpdateMaterial(const FStudioMaterial& Material)
{
    if(!CanEditMaterials(*this))return false;
    if(!Project.Draft.Materials.ContainsByPredicate([&](const auto& M){return M.Id==Material.Id;}))
    {Notice=TEXT("The selected material no longer exists in this case.");return false;}
    if(Material.bSolid&&Project.Draft.Domain.FluidMaterialId==Material.Id)
    {Notice=TEXT("Unassign this material from the fluid domain before changing it to a solid.");return false;}
    return EditCase(TEXT("Edit material"),[Material](auto& D)
    {if(auto* Current=D.Materials.FindByPredicate([&](const auto& M){return M.Id==Material.Id;}))*Current=Material;});
}
bool FStudioModel::DuplicateMaterial(const FGuid& Id,FGuid& OutId)
{
    if(!CanEditMaterials(*this))return false;
    const auto* Source=Project.Draft.Materials.FindByPredicate([Id](const auto& M){return M.Id==Id;});
    if(!Source){Notice=TEXT("Select an existing material to duplicate.");return false;}
    if(Project.Draft.Materials.Num()>=256){Notice=TEXT("A case can contain at most 256 materials.");return false;}
    FStudioMaterial New=*Source;New.Id=FGuid::NewGuid();New.Name=NewMaterialName(Project.Draft,Source->Name.Left(112)+TEXT(" copy"));
    if(!EditCase(TEXT("Duplicate material"),[New](auto& D){D.Materials.Add(New);}))return false;
    OutId=New.Id;return true;
}
bool FStudioModel::DeleteMaterial(const FGuid& Id,bool bUnassign)
{
    if(!CanEditMaterials(*this))return false;
    FStudioCaseDraft Candidate=Project.Draft;FString Error;
    if(!StudioMaterials::Remove(Candidate,Id,bUnassign,Error)){Notice=Error;return false;}
    return EditCase(bUnassign?TEXT("Unassign and delete material"):TEXT("Delete material"),[Candidate](auto& D){D=Candidate;});
}
bool FStudioModel::AssignDomainMaterial(const FGuid& Id)
{
    if(!CanEditMaterials(*this))return false;
    if(Id.IsValid())
    {
        const auto* Material=Project.Draft.Materials.FindByPredicate([Id](const auto& M){return M.Id==Id;});
        if(!Material||Material->bSolid){Notice=TEXT("Choose an existing fluid material for the domain.");return false;}
    }
    return EditCase(TEXT("Assign domain fluid"),[Id](auto& D){D.Domain.FluidMaterialId=Id;});
}
bool FStudioModel::AssignGeometryMaterial(const FGuid& GeometryId,const FGuid& MaterialId)
{
    if(!CanEditMaterials(*this))return false;
    if(!Project.Draft.Geometry.ContainsByPredicate([GeometryId](const auto& G){return G.Id==GeometryId;}))
    {Notice=TEXT("The selected geometry no longer exists in this case.");return false;}
    if(MaterialId.IsValid()&&!Project.Draft.Materials.ContainsByPredicate([MaterialId](const auto& M){return M.Id==MaterialId;}))
    {Notice=TEXT("Choose an existing material for this geometry.");return false;}
    return EditCase(TEXT("Assign geometry material"),[GeometryId,MaterialId](auto& D)
    {if(auto* Geometry=D.Geometry.FindByPredicate([GeometryId](const auto& G){return G.Id==GeometryId;}))Geometry->MaterialId=MaterialId;});
}
