#include "SStudioHome4Lineage.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "Widgets/Input/SButton.h"
#include "StudioHome4Recipes.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
TSharedRef<SWidget> LineageText(const FString& Text,int32 Size=10,FLinearColor Color=StudioUI::Muted)
{return SNew(STextBlock).Text(FText::FromString(Text)).Font(StudioUI::Font(Size)).ColorAndOpacity(Color).AutoWrapText(true);}
}
void SStudioHome4Lineage::Construct(const FArguments& Args)
{
    Model=Args._Model;Telemetry=Args._Telemetry;Evidence=Args._Evidence;OnFields=Args._OnFields;
    ChildSlot[SAssignNew(Rows,SVerticalBox)];Refresh();SetCanTick(true);
}
void SStudioHome4Lineage::Tick(const FGeometry&,double,float)
{
    FString Next=Model->Project.Id.ToString()+TEXT("/")+LexToString(Model->Project.Runs.Num())+TEXT("/")+LexToString(Model->Project.Draft.Revision);
    if(const auto T=Telemetry?Telemetry():nullptr;T&&!T->History().IsEmpty())
        Next+=T->History().Last().Source.RunId.ToString()+LexToString(T->History().Last().RecordIndex);
    if(const auto E=Evidence?Evidence():nullptr)Next+=E->SourceSHA256;
    if(Next!=Signature){Signature=MoveTemp(Next);Refresh();}
}
void SStudioHome4Lineage::Refresh()
{
    Rows->ClearChildren();Rows->AddSlot().AutoHeight().Padding(0,8,0,10)[LineageText(TEXT("Recipe lineage & retained runs"),13,StudioUI::Text)];
    const auto Stream=Telemetry?Telemetry():nullptr;const auto Reference=Evidence?Evidence():nullptr;
    TArray<const FStudioRunRecord*> Runs;for(const auto& R:Model->Project.Runs)Runs.Add(&R);
    auto Lineage=[](const FStudioRunRecord& R)
    {if(R.GetProvenance())return R.GetProvenance()->LineageId;
        const auto C=R.GetConfiguration();return C&&C->Home4?C->Home4->LineageId:FString();};
    Runs.StableSort([&](const FStudioRunRecord& A,const FStudioRunRecord& B){return Lineage(A)<Lineage(B);});
    FString Group;
    for(const auto* Run:Runs)
    {
        const FString Parent=Lineage(*Run),Heading=Parent.IsEmpty()?TEXT("Unassigned lineage"):Parent;
        if(Group!=Heading){Group=Heading;Rows->AddSlot().AutoHeight().Padding(0,6,0,5)[LineageText(Group,10,StudioUI::Cyan)];}
        auto Card=SNew(SVerticalBox);auto Add=[&](const FString& Text,int32 Size=9,FLinearColor Color=StudioUI::Muted)
        {Card->AddSlot().AutoHeight().Padding(0,3)[LineageText(Text,Size,Color)];};
        Add(Run->GetName(),11,StudioUI::Text);
        const auto Provenance=Run->GetProvenance();const auto Config=Run->GetConfiguration();
        const bool Recording=Run->GetOrigin()==EStudioRunOrigin::ImportedRecording||Run->GetOrigin()==EStudioRunOrigin::PublishedRecording;
        const FString SourceId=Provenance?Provenance->RunId:FString();FGuid OriginalRun;
        const bool HasId=FGuid::Parse(SourceId,OriginalRun)&&OriginalRun.IsValid();
        const auto Recipe=Provenance?Provenance->RecipeId:Config&&Config->Home4?Config->Home4->RecipeId:FString();
        const auto Definition=StudioHome4Recipes::Find(Recipe);
        Add((Recording?TEXT("REPLAY · "):TEXT("CAPTURED REQUEST · "))+(Definition?Definition->Name:TEXT("Recipe unspecified")));
        Add(TEXT("Source run: ")+(HasId?SourceId:TEXT("not supplied")));
        if(Config&&Config->Home4)Add(TEXT("Requested tag: ")+Config->Home4->Run.Tag+TEXT(" · backend: ")+StudioHome4Config::ToJSON(*Config->Home4)->GetObjectField(TEXT("run"))->GetStringField(TEXT("backend")));
        const auto Ref=Model->Project.Recordings.FindByPredicate([&](const auto& V){return V.Id==Run->GetDatasetId();});
        if(Ref){Add(TEXT("Recording: ")+Ref->Path);Add(TEXT("SHA256: ")+Ref->MetadataSHA256);}
        if(Provenance){Add(TEXT("Original archive manifest: ")+Provenance->ManifestPath);Add(TEXT("Manifest SHA256: ")+Provenance->ManifestSHA256);}
        const FStudioHome4Sample* Sample=nullptr;
        if(HasId&&Stream&&!Stream->History().IsEmpty()&&Stream->History().Last().Source.RunId==OriginalRun)Sample=&Stream->History().Last();
        if(Sample)
        {
            const auto Rate=FStudioHome4Diagnostics::Performance(*Sample).MLUPSInstant;
            Add(TEXT("Measured backend: ")+(Sample->Backend.IsEmpty()?TEXT("unavailable"):Sample->Backend)+TEXT(" · MLUPS: ")+
                (Rate?FString::Printf(TEXT("%.6g"),*Rate):Sample->ReportedMLUPSInstant?FString::Printf(TEXT("%.6g (reported)"),*Sample->ReportedMLUPSInstant):TEXT("unavailable")));
        }
        else Add(TEXT("Measured backend / throughput unavailable for this run."));
        if(Reference&&HasId&&Reference->RunId==OriginalRun&&Reference->RecipeId==Recipe&&
            Reference->AttachedProjectId==Model->Project.Id&&Reference->AttachedCaseId==Model->Project.Draft.Id)Add(Reference->GateStatus(),9,StudioUI::Amber);
        else Add(TEXT("Reference gate not evaluated for this run."));
        if(Recording)Card->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(8,5)).OnClicked_Lambda([this,Id=Run->GetDatasetId()]
        {if(Model->RequestRecording(Id))OnFields.ExecuteIfBound();return FReply::Handled();})[LineageText(TEXT("Inspect recorded fields"),10,StudioUI::Cyan)]];
        Rows->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(12)[Card]];
    }
    if(Runs.IsEmpty())Rows->AddSlot().AutoHeight()[LineageText(TEXT("Import an original recording or capture a run request to add its identity here."))];
}
