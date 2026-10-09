#include "SStudioHome4Lineage.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "Widgets/Input/SButton.h"
#include "StudioHome4Recipes.h"
#include "StudioHome4Lineage.h"
#include "StudioHome4Session.h"
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
    Model=Args._Model;Session=Args._Session?Args._Session:MakeShared<FStudioHome4Session>(Model);OnAuthoring=Args._OnAuthoring;Telemetry=Args._Telemetry;Evidence=Args._Evidence;OnFields=Args._OnFields;
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
    const auto Nodes=StudioHome4Lineage::Tree(Model->Project);
    if(Model->Project.Draft.Home4)
    {
        const auto& Current=*Model->Project.Draft.Home4;Rows->AddSlot().AutoHeight().Padding(0,4,0,14)[LineageText(TEXT("Current request · ")+Current.RecipeId+TEXT(" · branch ")+(Current.BranchId.IsEmpty()?TEXT("root"):Current.BranchId),10,StudioUI::Cyan)];
        if(!Current.ParentRunId.IsEmpty())Rows->AddSlot().AutoHeight()[LineageText(TEXT("Parent ")+Current.ParentRunId+TEXT(" · SHA256 ")+Current.ParentSpecSHA256,9)];
    }
    for(const auto& Node:Nodes)
    {
        const auto* Run=&Model->Project.Runs[Node.RunIndex];
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
        Add(TEXT("Source run: ")+(SourceId.IsEmpty()?TEXT("not supplied"):SourceId));
        if(Node.Parent!=INDEX_NONE)Add(TEXT("Child of ")+Model->Project.Runs[Node.Parent].GetName()+TEXT(" · immutable parent request verified"),9,StudioUI::Cyan);
        if(!Node.Issue.IsEmpty())Add(Node.Issue,9,StudioUI::Amber);
        for(const auto& Change:Node.Changes)Add(Change,9,StudioUI::Text);
        if(Config&&Config->Home4)Add(TEXT("Requested tag: ")+Config->Home4->Run.Tag+TEXT(" · backend: ")+StudioHome4Config::ToJSON(*Config->Home4)->GetObjectField(TEXT("run"))->GetStringField(TEXT("backend")));
        const auto Ref=Model->Project.Recordings.FindByPredicate([&](const auto& V){return V.Id==Run->GetDatasetId();});
        if(Ref){Add(TEXT("Recording: ")+Ref->Path);Add(TEXT("SHA256: ")+Ref->MetadataSHA256);}
        if(Provenance)
        {
            Add(TEXT("Original archive manifest: ")+Provenance->ManifestPath);Add(TEXT("Manifest SHA256: ")+Provenance->ManifestSHA256);
            for(const auto& Path:Provenance->OriginalArchives)Add(TEXT("Original snapshot: ")+Path);
            if(!Provenance->OriginalTag.IsEmpty())Add(TEXT("Original tag: ")+Provenance->OriginalTag);
            if(!Provenance->OriginalBackend.IsEmpty())Add(TEXT("Original backend: ")+Provenance->OriginalBackend);
        }
        const FStudioHome4Sample* Sample=nullptr;
        if(HasId&&Stream&&!Stream->History().IsEmpty()&&Stream->History().Last().Source.RunId==OriginalRun)Sample=&Stream->History().Last();
        if(Sample)
        {
            const auto Rate=FStudioHome4Diagnostics::Performance(*Sample).MLUPSInstant;
            Add(TEXT("Measured backend: ")+(Sample->Backend.IsEmpty()?TEXT("unavailable"):Sample->Backend)+TEXT(" · MLUPS: ")+
                (Rate?FString::Printf(TEXT("%.6g"),*Rate):Sample->ReportedMLUPSInstant?FString::Printf(TEXT("%.6g (reported)"),*Sample->ReportedMLUPSInstant):TEXT("unavailable")));
        }
        else if(Provenance&&Provenance->MeasuredMLUPS)Add(FString::Printf(TEXT("Original measured MLUPS: %.6g · "),*Provenance->MeasuredMLUPS)+Provenance->MeasurementSource+TEXT(" · SHA ")+Provenance->MeasurementSHA256);
        else Add(TEXT("Measured backend / throughput unavailable for this run."));
        if(Reference&&HasId&&Reference->RunId==OriginalRun&&Reference->RecipeId==Recipe&&
            Reference->AttachedProjectId==Model->Project.Id&&Reference->AttachedCaseId==Model->Project.Draft.Id)Add(Reference->GateStatus(),9,StudioUI::Amber);
        else if(Provenance&&!Provenance->GateStatus.IsEmpty())Add(Provenance->GateStatus+TEXT(" · original evidence SHA ")+Provenance->GateSourceSHA256,9,StudioUI::Amber);
        else Add(TEXT("Reference gate not evaluated for this run."));
        const FStudioHome4Spec* Snapshot=Config&&Config->Home4?&Config->Home4.GetValue():Provenance&&Provenance->OriginalRunSpec?&Provenance->OriginalRunSpec.GetValue():nullptr;
        if(Snapshot)Card->AddSlot().AutoHeight().Padding(0,6)[SNew(SButton).Tag(FName(*(TEXT("Home4Lineage.derive.")+Run->GetId().ToString()))).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this,Copy=*Snapshot,Id=Run->GetId().ToString()]
        {FString Error;if(Session->DeriveFrom(Copy,Id,Error)){Session->Status=TEXT("Branch retained from immutable parent. Edit and Apply to save the next request.");OnAuthoring.ExecuteIfBound();}else Session->Status=Error;return FReply::Handled();})[LineageText(TEXT("Derive next-run branch"),10,StudioUI::Cyan)]];
        if(Recording)Card->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(8,5)).OnClicked_Lambda([this,Id=Run->GetDatasetId()]
        {if(Model->RequestRecording(Id))OnFields.ExecuteIfBound();return FReply::Handled();})[LineageText(TEXT("Inspect recorded fields"),10,StudioUI::Cyan)]];
        Rows->AddSlot().AutoHeight().Padding(FMath::Min(Node.Depth,24)*18,0,0,10)[SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(12)[Card]];
    }
    if(Nodes.IsEmpty())Rows->AddSlot().AutoHeight()[LineageText(TEXT("Import an original recording or capture a run request to add its identity here."))];
}
