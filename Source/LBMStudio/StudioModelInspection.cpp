#include "StudioModel.h"

namespace StudioInspectionModelPrivate
{
FStudioInspectionObject* Find(FStudioInspectionObjects& Objects,const FGuid& Id)
{
    if(auto* O=Objects.Slices.FindByPredicate([&](const auto& S){return S.Id==Id;}))return O;
    if(auto* O=Objects.Probes.FindByPredicate([&](const auto& S){return S.Id==Id;}))return O;
    if(auto* O=Objects.Rulers.FindByPredicate([&](const auto& S){return S.Id==Id;}))return O;
    return Objects.Seeds.FindByPredicate([&](const auto& S){return S.Id==Id;});
}
bool HasName(const FStudioInspectionObjects& Objects,const FString& Name)
{
    auto Matches=[&](const auto& O){return O.Name.Equals(Name,ESearchCase::IgnoreCase);};
    return Objects.Slices.ContainsByPredicate(Matches)||Objects.Probes.ContainsByPredicate(Matches)||Objects.Rulers.ContainsByPredicate(Matches)||Objects.Seeds.ContainsByPredicate(Matches);
}
}

FStudioInspectionSource FStudioModel::InspectionSource() const
{
    if(!Solver)return {};
    const auto& D=Solver->Descriptor();return {D.Id,D.MetadataSHA256,D.PayloadSHA256};
}
const FStudioSliceObject* FStudioModel::FindSlice(const FGuid& Id) const
{return InspectionObjects.Slices.FindByPredicate([&](const auto& S){return S.Id==Id;});}
const FStudioProbeObject* FStudioModel::FindProbe(const FGuid& Id) const
{return InspectionObjects.Probes.FindByPredicate([&](const auto& S){return S.Id==Id;});}
const FStudioRulerObject* FStudioModel::FindRuler(const FGuid& Id) const
{return InspectionObjects.Rulers.FindByPredicate([&](const auto& S){return S.Id==Id;});}
const FStudioSeedObject* FStudioModel::FindSeed(const FGuid& Id) const
{return InspectionObjects.Seeds.FindByPredicate([&](const auto& S){return S.Id==Id;});}
const FStudioInspectionObject* FStudioModel::FindInspectionObject(const FGuid& Id) const
{
    if(const auto* O=FindSlice(Id))return O;
    if(const auto* O=FindProbe(Id))return O;
    if(const auto* O=FindRuler(Id))return O;
    return FindSeed(Id);
}
bool FStudioModel::InspectionMessage(const FString& Message,bool bError)
{InspectionNotice=Message;bInspectionError=bError;Notice=Message;return !bError;}
FString FStudioModel::UniqueInspectionName(const FString& Base) const
{
    const FString Stem=Base.Left(110).TrimStartAndEnd();
    if(!StudioInspectionModelPrivate::HasName(InspectionObjects,Stem))return Stem;
    for(int32 I=2;I<=4*StudioInspectionObjects::MaxObjectsPerKind+1;++I)
    {
        const FString Name=Stem+FString::Printf(TEXT(" %d"),I);
        if(!StudioInspectionModelPrivate::HasName(InspectionObjects,Name))return Name;
    }
    return FString();
}
bool FStudioModel::PrepareInspectionObject(FStudioInspectionObject& Object,const TCHAR* BaseName)
{
    if(IsProjectOpenPending()||IsRecordingLoadPending())
        return InspectionMessage(TEXT("Finish or cancel loading before placing inspection objects."),true);
    const auto Source=InspectionSource();
    if(!StudioInspectionObjects::IsValid(Source))return InspectionMessage(TEXT("Open a verified recording before placing inspection objects."),true);
    if(Object.Source.Dataset.IsEmpty()&&Object.Source.MetadataSHA256.IsEmpty()&&Object.Source.PayloadSHA256.IsEmpty())Object.Source=Source;
    if(!(Object.Source==Source))return InspectionMessage(TEXT("This object belongs to a different recording."),true);
    Object.Name=Object.Name.TrimStartAndEnd();if(Object.Name.IsEmpty())Object.Name=UniqueInspectionName(BaseName);
    return true;
}
bool FStudioModel::CommitInspectionObjects(const FString& Label,FStudioInspectionObjects Objects,bool bContinueGesture,bool bUseSavedSeeds)
{
    if(IsProjectOpenPending())return InspectionMessage(TEXT("Finish or cancel project opening before editing inspection objects."),true);
    FString Error;if(!StudioInspectionObjects::IsValid(Objects,Error))return InspectionMessage(Error,true);
    // An existing object cannot be relabelled as belonging to another source.
    auto SameBindings=[&](const auto& List)
    {
        for(const auto& O:List)if(const auto* Before=FindInspectionObject(O.Id);Before&&!(Before->Source==O.Source))return false;
        return true;
    };
    if(!SameBindings(Objects.Slices)||!SameBindings(Objects.Probes)||!SameBindings(Objects.Rulers)||!SameBindings(Objects.Seeds))
        return InspectionMessage(TEXT("An inspection object's original source cannot be changed."),true);
    if(Objects==InspectionObjects)return InspectionMessage(TEXT("Inspection object is already up to date."));
    if(!bContinueGesture)EndViewEdit();
    if(!EditView(Label,[&](auto& View){View.Display.InspectionObjects=MoveTemp(Objects);
        if(bUseSavedSeeds)View.Display.StreamlineSettings.bAutomaticSeeds=false;}))return InspectionMessage(Notice,true);
    return InspectionMessage(Label);
}
bool FStudioModel::AddSlice(FStudioSliceObject Slice)
{
    if(!PrepareInspectionObject(Slice,TEXT("Slice")))return false;
    const auto Id=Slice.Id;auto Objects=InspectionObjects;Objects.Slices.Add(MoveTemp(Slice));
    if(!CommitInspectionObjects(TEXT("Added slice"),MoveTemp(Objects)))return false;
    return SelectInspectionObject(Id);
}
bool FStudioModel::AddProbe(FStudioProbeObject Probe)
{
    if(!PrepareInspectionObject(Probe,Probe.Kind==EStudioProbeKind::Line?TEXT("Line probe"):TEXT("Point probe")))return false;
    const auto Id=Probe.Id;auto Objects=InspectionObjects;Objects.Probes.Add(MoveTemp(Probe));
    if(!CommitInspectionObjects(TEXT("Added probe"),MoveTemp(Objects)))return false;
    return SelectInspectionObject(Id);
}
bool FStudioModel::AddRuler(FStudioRulerObject Ruler)
{
    if(!PrepareInspectionObject(Ruler,Ruler.Kind==EStudioRulerKind::Angle?TEXT("Angle"):TEXT("Ruler")))return false;
    const auto Id=Ruler.Id;auto Objects=InspectionObjects;Objects.Rulers.Add(MoveTemp(Ruler));
    if(!CommitInspectionObjects(TEXT("Added ruler"),MoveTemp(Objects)))return false;
    return SelectInspectionObject(Id);
}
bool FStudioModel::AddSeed(FStudioSeedObject Seed)
{
    if(!PrepareInspectionObject(Seed,TEXT("Streamline seeds")))return false;
    const auto Id=Seed.Id;auto Objects=InspectionObjects;Objects.Seeds.Add(MoveTemp(Seed));
    if(!CommitInspectionObjects(TEXT("Added streamline seeds"),MoveTemp(Objects),false,true))return false;
    return SelectInspectionObject(Id);
}
bool FStudioModel::EditSeed(const FGuid& Id,TFunctionRef<void(FStudioSeedObject&)> Edit)
{
    auto Objects=InspectionObjects;auto* O=Objects.Seeds.FindByPredicate([&](const auto& S){return S.Id==Id;});
    if(!O)return InspectionMessage(TEXT("This seed set is no longer in the project."),true);
    if(!(O->Source==InspectionSource()))return InspectionMessage(TEXT("Open this seed set's recording before editing it."),true);
    Edit(*O);if(O->Id!=Id)return InspectionMessage(TEXT("Seed identity cannot be changed."),true);
    return CommitInspectionObjects(TEXT("Edited streamline seeds"),MoveTemp(Objects),true);
}
bool FStudioModel::EditSlice(const FGuid& Id,TFunctionRef<void(FStudioSliceObject&)> Edit)
{
    auto Objects=InspectionObjects;auto* O=Objects.Slices.FindByPredicate([&](const auto& S){return S.Id==Id;});
    if(!O)return InspectionMessage(TEXT("This slice is no longer in the project."),true);
    if(!(O->Source==InspectionSource()))return InspectionMessage(TEXT("Open this slice's recording before moving it."),true);
    Edit(*O);if(O->Id!=Id)return InspectionMessage(TEXT("Slice identity cannot be changed."),true);
    return CommitInspectionObjects(TEXT("Edited slice"),MoveTemp(Objects),true);
}
bool FStudioModel::EditProbe(const FGuid& Id,TFunctionRef<void(FStudioProbeObject&)> Edit)
{
    auto Objects=InspectionObjects;auto* O=Objects.Probes.FindByPredicate([&](const auto& S){return S.Id==Id;});
    if(!O)return InspectionMessage(TEXT("This probe is no longer in the project."),true);
    if(!(O->Source==InspectionSource()))return InspectionMessage(TEXT("Open this probe's recording before moving it."),true);
    Edit(*O);if(O->Id!=Id)return InspectionMessage(TEXT("Probe identity cannot be changed."),true);
    return CommitInspectionObjects(TEXT("Edited probe"),MoveTemp(Objects),true);
}
bool FStudioModel::EditRuler(const FGuid& Id,TFunctionRef<void(FStudioRulerObject&)> Edit)
{
    auto Objects=InspectionObjects;auto* O=Objects.Rulers.FindByPredicate([&](const auto& S){return S.Id==Id;});
    if(!O)return InspectionMessage(TEXT("This ruler is no longer in the project."),true);
    if(!(O->Source==InspectionSource()))return InspectionMessage(TEXT("Open this ruler's recording before moving it."),true);
    Edit(*O);if(O->Id!=Id)return InspectionMessage(TEXT("Ruler identity cannot be changed."),true);
    return CommitInspectionObjects(TEXT("Edited ruler"),MoveTemp(Objects),true);
}
bool FStudioModel::RenameInspectionObject(const FGuid& Id,const FString& Name)
{
    auto Objects=InspectionObjects;auto* O=StudioInspectionModelPrivate::Find(Objects,Id);
    if(!O)return InspectionMessage(TEXT("This inspection object is no longer in the project."),true);
    O->Name=Name.TrimStartAndEnd();return CommitInspectionObjects(TEXT("Renamed inspection object"),MoveTemp(Objects));
}
bool FStudioModel::DuplicateInspectionObject(const FGuid& Id)
{
    const auto* Original=FindInspectionObject(Id);
    if(!Original)return InspectionMessage(TEXT("This inspection object is no longer in the project."),true);
    const FGuid NewId=FGuid::NewGuid();const FString Name=UniqueInspectionName(Original->Name.Left(105)+TEXT(" copy"));
    auto Objects=InspectionObjects;
    auto Copy=[&](auto& List)
    {
        auto* Found=List.FindByPredicate([&](const auto& S){return S.Id==Id;});if(!Found)return false;
        auto Value=*Found;Value.Id=NewId;Value.Name=Name;List.Add(MoveTemp(Value));return true;
    };
    if(!(Copy(Objects.Slices)||Copy(Objects.Probes)||Copy(Objects.Rulers)||Copy(Objects.Seeds)))return false;
    if(!CommitInspectionObjects(TEXT("Duplicated inspection object"),MoveTemp(Objects)))return false;
    return SelectInspectionObject(NewId);
}
bool FStudioModel::DeleteInspectionObject(const FGuid& Id)
{
    if(!FindInspectionObject(Id))return InspectionMessage(TEXT("This inspection object is no longer in the project."),true);
    auto Objects=InspectionObjects;auto Match=[&](const auto& O){return O.Id==Id;};
    Objects.Slices.RemoveAll(Match);Objects.Probes.RemoveAll(Match);Objects.Rulers.RemoveAll(Match);Objects.Seeds.RemoveAll(Match);
    return CommitInspectionObjects(TEXT("Deleted inspection object"),MoveTemp(Objects));
}
bool FStudioModel::SetInspectionObjectVisible(const FGuid& Id,bool bVisible)
{
    auto Objects=InspectionObjects;auto* O=StudioInspectionModelPrivate::Find(Objects,Id);
    if(!O)return InspectionMessage(TEXT("This inspection object is no longer in the project."),true);
    O->bVisible=bVisible;return CommitInspectionObjects(bVisible?TEXT("Show inspection object"):TEXT("Hide inspection object"),MoveTemp(Objects));
}
bool FStudioModel::SelectInspectionObject(const FGuid& Id)
{
    if(Id.IsValid()&&!FindInspectionObject(Id))return InspectionMessage(TEXT("This inspection object is no longer in the project."),true);
    if(SelectedInspectionObject!=Id){SelectedInspectionObject=Id;++InspectionSelectionRevision;}
    return true;
}
