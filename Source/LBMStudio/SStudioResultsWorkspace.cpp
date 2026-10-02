/*
THESIS: Choose original CFD and distinguish saved control runs from computed output.
OWN-WORLD: Existing dense blue-black Slate shell, fine boundaries, cyan selection and CoreStyle type.
STORY: Choose a recording, read its original dimensions/fields/time, inspect an exact frame in Solve.
FIRST VIEWPORT: A 280-unit searchable catalog sits beside source details and exact-frame controls; import belongs to this workspace.
FORM: Operate extension of the established master/detail authoring pattern. Sidebar remains the sole workspace navigator.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
#include "SStudioResultsWorkspace.h"
#include "SStudioComparisonWorkspace.h"
#include "StudioScene.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSlider.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Async/Async.h"

namespace
{
class SResultsFilterBox : public SEditableTextBox
{
public:
    void Construct(const FArguments& Args)
    {
        SEditableTextBox::Construct(Args);
        // The shared input foreground already supplies the muted search color.
        EditableText->SetHintTextOpacity(1.f);
    }
};
FString ResultOrigin(EStudioRunOrigin Origin)
{
    switch(Origin)
    {
    case EStudioRunOrigin::PublishedRecording:return TEXT("Published recording");
    case EStudioRunOrigin::ImportedRecording:return TEXT("Imported recording");
    case EStudioRunOrigin::ControlHarness:return TEXT("Control harness · no CFD output");
    default:return TEXT("Solver configuration · no attached CFD output");
    }
}
TSharedRef<SWidget> ResultText(const FString& Value,int32 Size=10,FLinearColor Color=StudioUI::Text,bool Bold=false)
{
    return SNew(STextBlock).Text(FText::FromString(Value)).Font(StudioUI::Font(Size,Bold))
        .ColorAndOpacity(Color).AutoWrapText(true);
}
TSharedRef<SWidget> ResultProperty(const FString& Caption,const FString& Value,FName Tag=NAME_None)
{
    auto Content=ResultText(Value,10);Content->SetTag(Tag);
    return SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(145)[ResultText(Caption,9,StudioUI::Muted)]]
        +SHorizontalBox::Slot().FillWidth(1)[Content];
}
TSharedRef<SButton> ResultButton(FName Tag,const FString& Caption,TFunction<void()> Action)
{
    return SNew(SButton).Tag(Tag).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(10,7))
        .OnClicked_Lambda([Action]{Action();return FReply::Handled();})[StudioUI::Label(Caption)];
}
}

bool SStudioResultsWorkspace::Available() const
{return !M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();}
bool SStudioResultsWorkspace::HasRecording() const
{return M->Solver&&M->Solver->FrameCount()>0&&M->Solver->Descriptor().Id==M->Project.Dataset;}

void SStudioResultsWorkspace::Construct(const FArguments& Args)
{
    using namespace StudioUI;
    M=Args._Model;Scene=Args._Scene;Inspect=Args._OnInspect;Import=Args._OnImport;Locate=Args._Locate;
    // The installed registry is small, but it is still file parsing and stays off Slate.
    InstalledTask=Async(EAsyncExecution::ThreadPool,[]{return StudioRecordings::Installed();});
    auto ImportButton=ResultButton(TEXT("ResultsImport"),TEXT("Import recording…"),[this]{SetRuns(false);Import.ExecuteIfBound();});
    ImportButton->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Available();}));
    auto Category=[this](bool Runs,const TCHAR* Caption,const TCHAR* Tag)
    {
        auto B=ResultButton(Tag,Caption,[this,Runs]{SetRuns(Runs);});
        B->SetContent(SNew(STextBlock).Font(Font(10)).Text(FText::FromString(Caption))
            .ColorAndOpacity_Lambda([this,Runs]{return bRuns==Runs?Cyan:Muted;}));return B;
    };
    auto CompareButton=ResultButton(TEXT("ResultsCompare"),TEXT("Compare recordings"),[this]{OpenComparison();});
    CompareButton->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Available()&&HasRecording()&&!InstalledTask.IsValid();}));
    Catalog=SNew(SBorder).BorderImage(&PanelBrush).Padding(20)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,16)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1)[SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight()[Label(TEXT("Results"),18,Text,true)]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,5)[ResultText(TEXT("Original recordings and saved run records"),10,Muted)]]
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)[CompareButton]
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ImportButton]]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,20,0)[SNew(SBox).WidthOverride(280)
                    [SNew(SVerticalBox)
                        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
                            +SHorizontalBox::Slot().FillWidth(1)[Category(false,TEXT("Recordings"),TEXT("ResultsRecordings"))]
                            +SHorizontalBox::Slot().FillWidth(1).Padding(4,0,0,0)[Category(true,TEXT("Run history"),TEXT("ResultsRuns"))]]
                        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SAssignNew(SearchBox,SResultsFilterBox).Tag(TEXT("ResultsSearch"))
                            .Style(&InputStyle()).Font(Font(10)).HintText_Lambda([this]{return FText::FromString(bRuns?TEXT("Filter run name or ID"):TEXT("Filter name, ID or path"));})
                            .OnTextChanged_Lambda([this](const FText& Value){Search=Value.ToString();bRowsDirty=true;})]
                        +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                            .NavigationScrollPadding(12.f)+SScrollBox::Slot()[SAssignNew(Rows,SVerticalBox)]]]]
                +SHorizontalBox::Slot().FillWidth(1)[SNew(SScrollBox).Tag(TEXT("ResultsDetailsScroll"))
                    .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12.f)
                    +SScrollBox::Slot()[SAssignNew(Details,SBox)]]]];
    ChildSlot[SAssignNew(Body,SBox)[Catalog.ToSharedRef()]];
}

void SStudioResultsWorkspace::OpenComparison()
{
    if(!Available()||!HasRecording()||!Scene.IsValid())return;
    TArray<FStudioRecordingEntry> Entries;
    for(const auto& R:M->Project.Recordings)Entries.Add({R.Id,R.Title,R.Path});
    for(const auto& R:Installed)if(!Entries.ContainsByPredicate([&](const auto& E){return E.Id==R.Id;}))Entries.Add(R);
    if(!Entries.ContainsByPredicate([this](const auto& E){return E.Id==M->Project.Dataset;}))Entries.Add({M->Project.Dataset,M->Solver->Descriptor().Title,{}});
    Comparison=SNew(SStudioComparisonWorkspace).Tag(TEXT("ComparisonWorkspace")).Model(M).World(Scene->GetWorld()).Recordings(Entries)
        .OnBack_Lambda([this]{CloseComparison();});
    Body->SetContent(Comparison.ToSharedRef());
    FSlateApplication::Get().SetKeyboardFocus(Comparison,EFocusCause::Navigation);
}
TOptional<FStudioComparisonExportRequest> SStudioResultsWorkspace::ExportComparisonSnapshot(FString& Error) const
{
    if(Comparison)return Comparison->ExportSnapshot(Error);
    Error=TEXT("Open a comparison and Compare frames before exporting both recordings.");return {};
}
void SStudioResultsWorkspace::CloseComparison()
{
    Body->SetContent(Catalog.ToSharedRef());Comparison.Reset();
    FSlateApplication::Get().SetKeyboardFocus(SearchBox,EFocusCause::Navigation);
}

void SStudioResultsWorkspace::SetRuns(bool Value)
{bRuns=Value;bRowsDirty=bDetailsDirty=true;}
bool SStudioResultsWorkspace::RevealNotification(const FGuid& Run,const FString& Recording)
{
    if(Run.IsValid())
    {if(!M->Project.Runs.ContainsByPredicate([&](const auto& Entry){return Entry.GetId()==Run;}))return false;}
    else if(Recording!=M->Project.Dataset)return false;
    if(Comparison)CloseComparison();
    ProjectId=M->Project.Id;RunId=Run;SearchBox->SetText(FText::GetEmpty());SetRuns(Run.IsValid());
    RefreshRows();Details->SetContent(bRuns?RunDetails():RecordingDetails());bDetailsDirty=false;
    FSlateApplication::Get().SetKeyboardFocus(SearchBox,EFocusCause::Navigation);return true;
}
void SStudioResultsWorkspace::OpenDataset(const FString& Id)
{
    if(!Available())return;
    SetRuns(false);SearchBox->SetText(FText::GetEmpty());
    if((Id!=M->Project.Dataset||!HasRecording()||!M->Solver->LoadError().IsEmpty())&&M->RequestRecording(Id))Opening=Id;
}
void SStudioResultsWorkspace::Tick(const FGeometry& Geometry,double Time,float Delta)
{
    SCompoundWidget::Tick(Geometry,Time,Delta);
    if(InstalledTask.IsValid()&&InstalledTask.IsReady())
    {Installed=InstalledTask.Get();InstalledTask={};bRowsDirty=bDetailsDirty=true;}
    if(ProjectId!=M->Project.Id)
    {if(Comparison)CloseComparison();ProjectId=M->Project.Id;RunId.Invalidate();Opening.Empty();bRuns=false;SearchBox->SetText(FText::GetEmpty());bRowsDirty=bDetailsDirty=true;}
    if(LastSource.Pin()!=M->Solver||LastCatalog!=M->CatalogRevision||LastRunCount!=M->Project.Runs.Num())
    {
        LastSource=M->Solver;LastCatalog=M->CatalogRevision;LastRunCount=M->Project.Runs.Num();
        bRowsDirty=bDetailsDirty=true;
    }
    if(!M->IsRecordingLoadPending()&&!Opening.IsEmpty()){Opening.Empty();bRowsDirty=true;}
    if(bRowsDirty)RefreshRows();
    if(bDetailsDirty){Details->SetContent(bRuns?RunDetails():RecordingDetails());bDetailsDirty=false;}
}

void SStudioResultsWorkspace::RefreshRows()
{
    using namespace StudioUI;
    const auto Focus=FSlateApplication::Get().GetKeyboardFocusedWidget();FName FocusTag;
    for(const auto& R:RowButtons)if(R.Value.Pin()==Focus){FocusTag=R.Key;break;}
    Rows->ClearChildren();RowButtons.Reset();int32 Shown=0;
    auto Add=[&](FName Tag,const FString& Title,const FString& Note,TFunction<void()> Action,TFunction<bool()> Selected,bool bSource,
        const FString& ExternalId=FString(),const FString& ExternalPath=FString())
    {
        auto B=ResultButton(Tag,Title,Action);
        B->SetContent(SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Font(10,true)).AutoWrapText(true).Text(FText::FromString(Title))
                .ColorAndOpacity_Lambda([Selected]{return Selected()?Cyan:Text;})]
            +SVerticalBox::Slot().AutoHeight().Padding(0,4)[ResultText(Note,9,Muted)]);
        if(bSource)B->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Available();}));
        RowButtons.Add(Tag,B);
        auto Row=SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[B];
        if(!ExternalId.IsEmpty())
        {
            const FName LocateTag(*(TEXT("ResultsLocate_")+ExternalId));
            auto Find=ResultButton(LocateTag,TEXT("Locate…"),[this,ExternalId,ExternalPath]{Locate(ExternalId,ExternalPath);});
            Find->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Available();}));
            Find->SetToolTipText(FText::FromString(TEXT("Locate an exact copy of this recording, even when its old folder is missing.")));
            RowButtons.Add(LocateTag,Find);Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4,0,0,0)[Find];
        }
        Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[Row];++Shown;
    };
    if(bRuns)
    {
        for(const auto& R:M->Project.Runs)
        {
            if(!Search.IsEmpty()&&!R.GetName().Contains(Search)&&!R.GetId().ToString().Contains(Search)&&!R.GetDatasetId().Contains(Search))continue;
            const FGuid Id=R.GetId();
            Add(FName(*(TEXT("ResultsRun_")+Id.ToString())),R.GetName(),ResultOrigin(R.GetOrigin()),
                [this,Id]{RunId=Id;bDetailsDirty=true;},[this,Id]{return RunId==Id;},false);
        }
    }
    else
    {
        TArray<FStudioRecordingEntry> Entries;
        for(const auto& R:M->Project.Recordings)Entries.Add({R.Id,R.Title,R.Path});
        for(const auto& R:Installed)if(!Entries.ContainsByPredicate([&](const auto& E){return E.Id==R.Id;}))Entries.Add(R);
        // A loaded source remains visible even when its installed registry disappeared.
        if(HasRecording()&&!Entries.ContainsByPredicate([this](const auto& E){return E.Id==M->Project.Dataset;}))
            Entries.Add({M->Project.Dataset,M->Solver->Descriptor().Title,{}});
        for(const auto& R:Entries)
        {
            if(!Search.IsEmpty()&&!R.Title.Contains(Search)&&!R.Id.Contains(Search)&&!R.Path.Contains(Search))continue;
            const bool External=M->Project.Recordings.ContainsByPredicate([&](const auto& E){return E.Id==R.Id;});
            const FString Note=R.Id==Opening?TEXT("Opening… current recording retained"):
                R.Id==M->Project.Dataset?TEXT("Current recording"):
                External?TEXT("External folder · checked on open"):TEXT("Included published recording");
            Add(FName(*(TEXT("ResultsDataset_")+R.Id)),R.Title,Note,[this,Id=R.Id]{OpenDataset(Id);},[this,Id=R.Id]{return M->Project.Dataset==Id;},true,
                External?R.Id:FString(),R.Path);
        }
    }
    if(!Shown)Rows->AddSlot().AutoHeight().Padding(0,8)[ResultText(!Search.IsEmpty()?TEXT("No results match this filter."):
        bRuns?TEXT("This project has no saved run records."):InstalledTask.IsValid()?TEXT("Loading recording catalog…"):TEXT("Import a recording folder to inspect its output."),10,Muted)];
    bRowsDirty=false;
    if(!FocusTag.IsNone())
    {
        if(const auto* B=RowButtons.Find(FocusTag))FSlateApplication::Get().SetKeyboardFocus(B->Pin(),EFocusCause::Navigation);
        else FSlateApplication::Get().SetKeyboardFocus(SearchBox,EFocusCause::Navigation);
    }
}

TSharedRef<SWidget> SStudioResultsWorkspace::RecordingDetails()
{
    using namespace StudioUI;
    if(!HasRecording())return ResultText(TEXT("No recording is loaded. Import a recording or select an available source."),11,Muted);
    const auto& D=M->Solver->Descriptor();auto Content=SNew(SVerticalBox);
    auto Heading=[&](const TCHAR* Name){Content->AddSlot().AutoHeight().Padding(0,18,0,8)[Label(Name,11,Text,true)];};
    auto Property=[&](const FString& Name,const FString& Value){Content->AddSlot().AutoHeight().Padding(0,0,0,8)[ResultProperty(Name,Value)];};
    Content->AddSlot().AutoHeight()[ResultText(D.Title,15,Text,true)];
    Content->AddSlot().AutoHeight().Padding(0,6,0,10)[ResultText(FString::Printf(TEXT("%dD %s · %d %s"),D.SpatialDimensions,
        D.bSourcePoints?TEXT("original points"):TEXT("triangle mesh"),D.NodeCount,D.bSourcePoints?TEXT("points"):TEXT("nodes")),10,Muted)];
    Property(TEXT("Original frames"),FString::Printf(TEXT("%d snapshots"),D.Frames.Num()));
    Property(TEXT("Connectivity"),D.bSourcePoints?TEXT("No cell connectivity supplied"):FString::Printf(TEXT("%d original triangles"),D.TriangleCount));
    Property(TEXT("Source time"),FString::Printf(TEXT("%.9g – %.9g s · duration %.9g s"),D.Frames[0].Time,D.Frames.Last().Time,D.Frames.Last().Time-D.Frames[0].Time));
    Content->AddSlot().AutoHeight().Padding(0,0,0,10)[ResultText(D.TimeNote,9,Muted)];
    Heading(TEXT("Frame review"));
    auto InspectButton=ResultButton(TEXT("ResultsInspect"),TEXT("Inspect in Solve"),[this]{Inspect.ExecuteIfBound();});
    InspectButton->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Available()&&HasRecording();}));
    Content->AddSlot().AutoHeight()[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)[Label(TEXT("Frame"),10,Muted)]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(100)[SNew(SNumericEntryBox<int32>).Tag(TEXT("ResultsFrame"))
            .Font(Font(10)).AllowSpin(true).MinValue(1).MaxValue(D.Frames.Num()).MinSliderValue(1).MaxSliderValue(D.Frames.Num())
            .Value_Lambda([this]()->TOptional<int32>{return M->SelectedFrame+1;})
            .IsEnabled_Lambda([this]{return Available();})
            .OnValueChanged_Lambda([this](int32 Value){M->ReviewRecordedFrame(Value-1);})
            .OnValueCommitted_Lambda([this](int32 Value,ETextCommit::Type){M->ReviewRecordedFrame(Value-1);})]]
        +SHorizontalBox::Slot().FillWidth(1).Padding(10,0).VAlign(VAlign_Center)[SNew(STextBlock).Tag(TEXT("ResultsFrameIdentity"))
            .Font(Font(10)).ColorAndOpacity(Text).AutoWrapText(true).Text_Lambda([this]{const auto& F=M->DisplayFrame();
                return FText::FromString(FString::Printf(TEXT("Source step %d · %.9g s"),F.Index,F.Time));})]
        +SHorizontalBox::Slot().AutoWidth()[InspectButton]];
    Content->AddSlot().AutoHeight().Padding(0,10)[SNew(SSlider).Tag(TEXT("ResultsTimeline"))
        .Value_Lambda([this]{return M->Frames.Num()<2?0.f:float(M->SelectedFrame)/float(M->Frames.Num()-1);})
        .IsEnabled_Lambda([this]{return Available()&&M->Frames.Num()>1;})
        .OnValueChanged_Lambda([this](float V){M->Scrub(V);})];
    Content->AddSlot().AutoHeight().Padding(0,0,0,4)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
            .Text_Lambda([this]{return FText::FromString(M->bReviewing?TEXT("Reviewing one original frame. Playback position is retained."):TEXT("Following recorded playback. Every original snapshot is available."));})]
        +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[SNew(SButton).Tag(TEXT("ResultsFollow")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,5))
            .IsEnabled_Lambda([this]{return Available()&&M->bReviewing;})
            .OnClicked_Lambda([this]{M->ReturnToLive();return FReply::Handled();})[Label(TEXT("Follow playback"),9)]]];
    Heading(TEXT("Recorded fields"));
    Content->AddSlot().AutoHeight().Padding(0,0,0,8)[ResultText(TEXT("Ranges and units supplied by the recording metadata."),9,Muted)];
    for(const auto& F:D.Scalars)Property(F.Label,FString::Printf(TEXT("%.9g – %.9g %s · %s"),F.Minimum,F.Maximum,*F.Unit,*F.Origin));
    Content->AddSlot().AutoHeight()[ResultText(D.FieldNote,9,Amber)];
    Heading(TEXT("Source and identity"));
    Property(TEXT("Dataset ID"),D.Id);Property(TEXT("Source"),D.SourceLabel);
    auto SourceButton=ResultButton(TEXT("ResultsSource"),TEXT("Open source reference"),[URL=D.SourceURL]{FPlatformProcess::LaunchURL(*URL,nullptr,nullptr);});
    SourceButton->SetEnabled(D.SourceURL.StartsWith(TEXT("https://")));SourceButton->SetToolTipText(FText::FromString(D.SourceURL));
    Content->AddSlot().AutoHeight().Padding(0,0,0,10)[SourceButton];
    const auto* Ref=M->Project.Recordings.FindByPredicate([&](const auto& R){return R.Id==D.Id;});
    FString Path=Ref?Ref->Path:FString();
    if(!Ref)if(const auto* Entry=Installed.FindByPredicate([&](const auto& R){return R.Id==D.Id;}))Path=Entry->Path;
    auto Copyable=[&](const TCHAR* Caption,const FString& Value)
    {
        Content->AddSlot().AutoHeight().Padding(0,0,0,4)[Label(Caption,9,Muted)];
        Content->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SEditableTextBox).Style(&InputStyle()).Font(Font(9)).IsReadOnly(true)
            .Text(FText::FromString(Value.IsEmpty()?TEXT("Not supplied"):Value)).ToolTipText(FText::FromString(Value))];
    };
    Copyable(TEXT("Recording location"),Path);
    Copyable(TEXT("Metadata SHA-256"),D.MetadataSHA256);
    if(!D.PayloadSHA256.IsEmpty())Copyable(TEXT("Payload SHA-256"),D.PayloadSHA256);
    else Content->AddSlot().AutoHeight().Padding(0,0,0,10)[ResultText(TEXT("Individual source arrays are identified by the hashes in this recording's metadata."),9,Muted)];
    Property(TEXT("Display translation"),FString::Printf(TEXT("X %.9g · Y %.9g · Z %.9g m"),D.SourceOffset.X,D.SourceOffset.Y,D.SourceOffset.Z));
    Property(TEXT("Reconstruction"),Ref&&Ref->Reconstruction.IsSet()?TEXT("Explicit derived topology attached; original values retained"):TEXT("No external reconstruction attached"));
    return Content;
}

TSharedRef<SWidget> SStudioResultsWorkspace::RunDetails()
{
    using namespace StudioUI;
    const auto* R=M->Project.Runs.FindByPredicate([this](const auto& Run){return Run.GetId()==RunId;});
    if(!R)return ResultText(TEXT("Select a run record to inspect its saved source or captured case settings."),11,Muted);
    auto Content=SNew(SVerticalBox);
    Content->AddSlot().AutoHeight()[ResultText(R->GetName(),15,Text,true)];
    Content->AddSlot().AutoHeight().Padding(0,8,0,16)[ResultText(ResultOrigin(R->GetOrigin()),10,Amber)];
    Content->AddSlot().AutoHeight().Padding(0,0,0,16)[SNew(STextBlock).Tag(TEXT("ResultsToolbarContext"))
        .Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true).Text_Lambda([this]
        {
            const FString Source=HasRecording()?M->Solver->Descriptor().Title:TEXT("no recording loaded");
            const FString Prefix=M->Project.bControlHarness?
                TEXT("Toolbar job controls use the current case. Field export uses "):
                TEXT("Toolbar playback and field export use ");
            return FText::FromString(Prefix+Source+TEXT(". Selecting a saved run only changes this inspection."));
        })];
    auto Property=[&](const TCHAR* Name,const FString& Value){Content->AddSlot().AutoHeight().Padding(0,0,0,10)
        [ResultProperty(Name,Value,FString(Name)==TEXT("Run ID")?FName(TEXT("ResultsRunIdentity")):NAME_None)];};
    Property(TEXT("Run ID"),R->GetId().ToString());
    Content->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(STextBlock).Tag(TEXT("ResultsRunStatus")).Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this,Id=R->GetId()]{const auto Status=M->RunStatus(Id);return FText::FromString(Status.IsEmpty()?TEXT("No job lifecycle is attached to this record."):Status);})];
    if(!R->GetDatasetId().IsEmpty())
    {
        Property(TEXT("Recording"),R->GetDatasetId());
        Content->AddSlot().AutoHeight().Padding(0,0,0,14)[ResultText(TEXT("Case settings were not supplied with this recording."),10,Muted)];
        auto Open=ResultButton(TEXT("ResultsRunRecording"),TEXT("Show recording"),[this,Id=R->GetDatasetId()]{OpenDataset(Id);});
        Open->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Available();}));
        Content->AddSlot().AutoHeight()[Open];
    }
    if(const auto* C=R->GetConfiguration())
    {
        Property(TEXT("Captured case"),C->Name);Property(TEXT("Case revision"),FString::Printf(TEXT("%lld"),C->Revision));
        Property(TEXT("Backend"),R->GetBackendId().IsEmpty()?TEXT("Not selected"):R->GetBackendId());
        Property(TEXT("Geometry / materials"),FString::Printf(TEXT("%d geometries · %d materials"),C->Geometry.Num(),C->Materials.Num()));
        Property(TEXT("Lattice request"),FString::Printf(TEXT("%d × %d × %d"),C->Setup.LatticeResolution.X,C->Setup.LatticeResolution.Y,C->Setup.LatticeResolution.Z));
        Property(TEXT("Step limit"),FString::Printf(TEXT("%lld"),C->Setup.MaxSteps));
        Property(TEXT("Physical time limit"),C->Setup.MaxPhysicalTime?FString::Printf(TEXT("%.9g s"),*C->Setup.MaxPhysicalTime):TEXT("Not specified"));
        Content->AddSlot().AutoHeight().Padding(0,8)[ResultText(TEXT("These settings are the immutable run snapshot. Editing the current case does not change them."),10,Muted)];
    }
    return Content;
}
