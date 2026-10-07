#include "SStudioHome4Reports.h"
#include "SStudioHome4Monitors.h"
#include "StudioHome4Runtime.h"
#include "StudioHome4Validation.h"
#include "StudioHome4SpatialDiagnostics.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "StudioFileDialog.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

void SStudioHome4Reports::Construct(const FArguments& A)
{
    using namespace StudioUI;Model=A._Model;Runtime=A._Runtime;Validation=A._Validation;Spatial=A._Spatial;Monitors=A._Monitors;Scope();
    auto Rows=SNew(SVerticalBox);
    auto Input=[&](const TCHAR* Caption,FName Tag,FString* Value)
    {Rows->AddSlot().AutoHeight().Padding(0,3)[SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[Label(Caption,8,Muted)]
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).Tag(Tag).Style(&InputStyle()).Font(Font(9)).Text_Lambda([Value]{return FText::FromString(*Value);}).OnTextChanged_Lambda([Value](const FText& V){*Value=V.ToString();})]];};
    auto Button=[&](const TCHAR* Caption,FName Tag,TFunction<void()> Action)
    {Rows->AddSlot().AutoHeight().Padding(0,4)[SNew(SButton).Tag(Tag).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([Action]{Action();return FReply::Handled();})[Label(Caption,9)]];};
    Rows->AddSlot().AutoHeight()[Label(TEXT("Original science reports and figure runs"),12,Text,true)];
    Rows->AddSlot().AutoHeight().Padding(0,5)[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true).Text(FText::FromString(TEXT("Reports retain exact supplied original evidence, source identities, numeric tables, SVG/TikZ figures and adjacent JSON sidecars. A current request has no transferred numerical gate. Imported replay remains historical; a live log is an explicitly labeled captured prefix.")))];
    Button(TEXT("Queue current request as a separate _viz figure run"),TEXT("Home4ReportFigureRun"),[this]{QueueFigureRun();});
    Input(TEXT("New report folder (never overwrites)"),TEXT("Home4ReportFolder"),&FolderDraft);
    Input(TEXT("Body ID (empty: original aggregate)"),TEXT("Home4ReportBody"),&BodyDraft);
    Input(TEXT("Phase (empty: original aggregate)"),TEXT("Home4ReportPhase"),&PhaseDraft);
    Input(TEXT("Level"),TEXT("Home4ReportLevel"),&LevelDraft);
    Input(TEXT("Optional exact source axis label"),TEXT("Home4ReportWindowAxis"),&AxisDraft);
    Input(TEXT("Optional source window start"),TEXT("Home4ReportWindowStart"),&StartDraft);
    Input(TEXT("Optional source window end"),TEXT("Home4ReportWindowEnd"),&EndDraft);
    Rows->AddSlot().AutoHeight()[SNew(SCheckBox).Tag(TEXT("Home4ReportPublication")).IsChecked_Lambda([this]{return bFigurePublication?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([this](ECheckBoxState V){bFigurePublication=V==ECheckBoxState::Checked;})[Label(TEXT("Publish figure run only after original coefficient agreement"),9)]];
    Button(TEXT("Import independently identified published coefficient evidence…"),TEXT("Home4ReportImportPublished"),[this]{ImportPublished();});
    Input(TEXT("Publication absolute tolerance (explicit)"),TEXT("Home4ReportAbsolute"),&AbsoluteDraft);
    Input(TEXT("Publication relative tolerance (explicit)"),TEXT("Home4ReportRelative"),&RelativeDraft);
    Button(TEXT("Export original science report bundle…"),TEXT("Home4ReportExport"),[this]{ExportDialog();});
    Rows->AddSlot().AutoHeight().Padding(0,6)[SNew(STextBlock).Tag(TEXT("Home4ReportCompletionStatus")).Font(Font(9)).ColorAndOpacity(Amber).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Status);})];
    ChildSlot[SNew(SBorder).BorderImage(&PanelBrush).Padding(10)[Rows]];
}
void SStudioHome4Reports::Scope()
{
    const auto M=Model.Pin();const FGuid P=M?M->Project.Id:FGuid(),C=M?M->Project.Draft.Id:FGuid();
    if(ProjectId==P&&CaseId==C)return;ProjectId=P;CaseId=C;Published.Reset();
    Status=TEXT("Select original science/reference evidence for this project/case; missing source inputs stay unavailable.");
}
bool SStudioHome4Reports::QueueFigureRun()
{
    Scope();const auto M=Model.Pin();if(!M||!M->Project.Draft.Home4||!Runtime){Status=TEXT("Apply a HOME4 request and review a shared runtime target first.");return false;}
    FStudioHome4Spec Figure;if(!StudioHome4Reports::FigureRun(*M->Project.Draft.Home4,Figure,Status))return false;
    if(!Runtime->Submit(Figure,FGuid::NewGuid(),FPlatformProcess::UserName(),FPlatformTime::Seconds(),Status))return false;
    Status=TEXT("Separate immutable _viz development request queued. Publication requires independently attached original coefficients; no numerical result created.");return true;
}
bool SStudioHome4Reports::ExportTo(const FString& Parent,const FString& Folder)
{
    Scope();const auto M=Model.Pin();if(!M||!M->Project.Draft.Home4){Status=TEXT("Apply a HOME4 request before reporting.");return false;}
    FStudioHome4ReportInputs Inputs;Inputs.Validation=Validation.Get();Inputs.BodyId=BodyDraft;Inputs.Phase=PhaseDraft;Inputs.Display=M->UnitDisplay;
    const FString Level=LevelDraft.TrimStartAndEnd();if(Level.IsEmpty()||Level.Len()>2){Status=TEXT("Level must be a bounded nonnegative integer.");return false;}
    for(TCHAR C:Level)if(C<'0'||C>'9'){Status=TEXT("Level must be a bounded nonnegative integer.");return false;}
    Inputs.Level=FCString::Atoi(*Level);Inputs.WindowAxis=AxisDraft;
    if(!StartDraft.TrimStartAndEnd().IsEmpty()||!EndDraft.TrimStartAndEnd().IsEmpty())
    {
        double Start=0,End=0;if(!LexTryParseString(Start,*StartDraft)||!LexTryParseString(End,*EndDraft)){Status=TEXT("Both report window bounds must be explicit finite numbers.");return false;}
        Inputs.WindowStart=Start;Inputs.WindowEnd=End;
    }
    Inputs.bFigurePublication=bFigurePublication;Inputs.PublishedEvidence=Published.Get();
    if(bFigurePublication)
    {
        double Absolute=0,Relative=0;
        if(!LexTryParseString(Absolute,*AbsoluteDraft)||!LexTryParseString(Relative,*RelativeDraft)){Status=TEXT("Publication requires explicit finite absolute and relative tolerances.");return false;}
        Inputs.PublicationAbsoluteTolerance=Absolute;Inputs.PublicationRelativeTolerance=Relative;
    }
    const FStudioHome4TelemetryStream* Stream=nullptr;TOptional<FStudioHome4TelemetryProvenance> Provenance;
    const auto Panel=Monitors.Pin();
    if(Panel){Stream=Panel->DisplayedTelemetry();Provenance=Panel->ReportProvenance();Inputs.OriginalTelemetryBytes=&Panel->ReportOriginalBytes();}
    else if(Runtime){Stream=Runtime->ScienceStream().Get();Provenance=Runtime->ScienceProvenance();Inputs.OriginalTelemetryBytes=&Runtime->CapturedScienceBytes();}
    if(Spatial)Spatial->Poll();
    const auto SpatialEvidence=Spatial?Spatial->Evidence():TSharedPtr<const FStudioHome4SpatialEvidence,ESPMode::ThreadSafe>();
    const auto Evidence=Validation?Validation->Evidence:TSharedPtr<const FStudioHome4ReferenceEvidence>();FString Out;
    if(!StudioHome4Reports::Export(Parent,Folder,M->SnapshotProject(),Stream,Out,Status,Evidence.Get(),SpatialEvidence.Get(),Provenance?&*Provenance:nullptr,&Inputs))return false;
    Status=TEXT("Report published atomically to ")+Out;return true;
}
void SStudioHome4Reports::ExportDialog()
{FString Parent;if(StudioFileDialog::ExportFolder(Parent))ExportTo(Parent,FolderDraft);}
void SStudioHome4Reports::ImportPublished()
{
    Scope();const auto M=Model.Pin();if(!M||!M->Project.Draft.Home4){Status=TEXT("Apply a recipe before selecting published evidence.");return;}
    FString Path;if(!StudioFileDialog::DataFile(false,TEXT("Import original published coefficient reference"),TEXT(""),TEXT("json"),Path))return;
    FStudioHome4ReferenceExpectation Expected;Expected.RecipeId=M->Project.Draft.Home4->RecipeId;FStudioHome4ReferenceEvidence E;
    if(!StudioHome4Validation::Load(Path,Expected,E,Status))return;
    E.AttachedProjectId=ProjectId;E.AttachedCaseId=CaseId;
    Published=MakeShared<FStudioHome4ReferenceEvidence>(MoveTemp(E));Status=TEXT("Original published coefficient evidence selected; comparison remains pending explicit tolerances and current original figure evidence.");
}
