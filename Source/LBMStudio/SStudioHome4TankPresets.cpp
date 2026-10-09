#include "SStudioHome4TankPresets.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "StudioFileDialog.h"
#include "Async/Async.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"

void SStudioHome4TankPresets::Construct(const FArguments& A)
{
    Session=A._Session;Authoring=A._Authoring;ImportPath=A._ImportPath;ExportPath=A._ExportPath;check(Session.IsValid());SetCanTick(true);Scope();
    auto Rows=SNew(SVerticalBox);Rows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("TH01 original G / Q / P tank & zone definitions"),13,StudioUI::Text,true)];
    Rows->AddSlot().AutoHeight().Padding(0,6)[StudioUI::Label(TEXT("G/Q/P numeric definitions were not supplied with the design note. Import an identified original definition into each named slot; selecting an undefined name leaves the request unchanged. Applied provenance is retained in the project, while imported source slots remain in this workspace session."),9,StudioUI::Muted,true)];
    auto Names=SNew(SHorizontalBox);
    for(const FString Name:{TEXT("G"),TEXT("Q"),TEXT("P")})
    {
        Names->AddSlot().FillWidth(1).Padding(0,0,5,0)[SNew(SButton).Tag(FName(*(TEXT("Home4TankPreset.select.")+Name))).ButtonStyle(&StudioUI::ButtonStyle())
        .OnClicked_Lambda([this,Name]{Select(Name);return FReply::Handled();})[SNew(STextBlock).Font(StudioUI::Font(10)).Text(FText::FromString(Name)).ColorAndOpacity_Lambda([this,Name]{return FSlateColor(Selected==Name?StudioUI::Cyan:StudioUI::Text);})]];
    }
    Rows->AddSlot().AutoHeight()[Names];
    auto FileActions=SNew(SHorizontalBox),ReviewActions=SNew(SHorizontalBox);
    auto Action=[&](const TSharedRef<SHorizontalBox>& Row,const TCHAR* Text,const TCHAR* Tag,TFunction<void()> Fn){Row->AddSlot().AutoWidth().Padding(0,0,6,0)[SNew(SButton).Tag(FName(Tag)).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([Fn]{Fn();return FReply::Handled();})[StudioUI::Label(Text,9)]];};
    Action(FileActions,TEXT("Import original JSON…"),TEXT("Home4TankPreset.import"),[this]{Import();});Action(FileActions,TEXT("Cancel read"),TEXT("Home4TankPreset.cancel"),[this]{CancelImport();});Action(FileActions,TEXT("Export original JSON…"),TEXT("Home4TankPreset.export"),[this]{Export();});
    Action(ReviewActions,TEXT("Review against current draft"),TEXT("Home4TankPreset.review"),[this]{Review();});Action(ReviewActions,TEXT("Retain reviewed preset"),TEXT("Home4TankPreset.apply"),[this]{Apply();});
    Rows->AddSlot().AutoHeight().Padding(0,8,0,4)[FileActions];Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[ReviewActions];
    Rows->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(170)[SNew(SMultiLineEditableTextBox).Tag(TEXT("Home4TankPreset.details")).IsReadOnly(true).AutoWrapText(true).Font(StudioUI::Font(9)).Text_Lambda([this]{return FText::FromString(Details());})]];
    Rows->AddSlot().AutoHeight().Padding(0,6)[SNew(STextBlock).Tag(TEXT("Home4TankPreset.status")).Font(StudioUI::Font(9)).AutoWrapText(true).ColorAndOpacity(StudioUI::Amber).Text_Lambda([this]{return FText::FromString(Message);})];
    ChildSlot[SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(10)[Rows]];Select(Selected);
}
SStudioHome4TankPresets::~SStudioHome4TankPresets(){if(Cancellation)Cancellation->store(true);}
void SStudioHome4TankPresets::Scope()
{
    Session->Refresh();const auto M=Session->Owner();const FGuid P=M?M->Project.Id:FGuid(),C=M?M->Project.Draft.Id:FGuid();
    if(P==Project&&C==Case)return;Project=P;Case=C;if(Cancellation)Cancellation->store(true);Slots.Reset();ReviewBase.Empty();Message=TEXT("Project or case changed; original preset slots cleared.");
}
void SStudioHome4TankPresets::Tick(const FGeometry&,double,float){Scope();PollImport();}
void SStudioHome4TankPresets::Select(const FString& Name)
{
    Scope();if(IsImporting()){Message=TEXT("Wait for or cancel the current original preset read before changing slots.");return;}Selected=Name;ReviewBase.Empty();
    if(!Slots.Contains(Name)){Message=TEXT("Original ")+Name+TEXT(" definition is missing. No tank or zone values were inferred.");return;}Review();
}
void SStudioHome4TankPresets::Review()
{
    Scope();ReviewBase.Empty();const auto* P=Slots.Find(Selected);if(!P){Message=TEXT("Original ")+Selected+TEXT(" definition is missing; request retained.");return;}
    FStudioHome4Spec S,N;FString E;if(!Session->Build(S,E)||!StudioHome4TankPresets::Apply(*P,S,N,E)){Message=E;return;}
    ReviewBase=StudioHome4Authoring::Fingerprint(S);Message=TEXT("Original ")+Selected+TEXT(" reviewed against this exact draft. Retain copies its tank/zone fields and provenance. Apply configuration saves the result.");
}
void SStudioHome4TankPresets::Apply()
{
    Scope();const auto* P=Slots.Find(Selected);if(!P){Message=TEXT("Original ")+Selected+TEXT(" definition is missing; request retained.");return;}
    FStudioHome4Spec S,N;FString E;if(IsImporting()||!Session->Build(S,E)){Message=E.IsEmpty()?TEXT("Wait for the original preset read."):E;return;}
    if(ReviewBase.IsEmpty()||StudioHome4Authoring::Fingerprint(S)!=ReviewBase){Message=TEXT("Draft changed after preset review. Review against the current draft before retaining; request unchanged.");return;}
    if(!StudioHome4TankPresets::Apply(*P,S,N,E)){Message=E;return;}
    if(P->SourcePath.IsEmpty()){Message=TEXT("Reimport the identified original file before retaining its definition.");return;}
    ImportProject=Project;ImportCase=Case;ImportBase=ReviewBase;ImportName=Selected;ReviewedApply=MoveTemp(N);bApplying=true;Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Pending=Async(EAsyncExecution::ThreadPool,[Original=*P,Proposed=ReviewedApply,ProjectId=Project,CaseId=Case,C=Cancellation]
    {
        FResult R;R.bGood=StudioHome4TankPresets::Load(Original.SourcePath,R.Preset,R.Error,C);
        if(R.bGood&&(R.Preset.SourceSHA256!=Original.SourceSHA256||R.Preset.OriginalBytes!=Original.OriginalBytes)){R.bGood=false;R.Error=TEXT("Original preset changed since review; reimport it. Current request retained.");}
        if(R.bGood)
        {
            FStudioHome4AuthoringRequest Request;Request.ProjectId=ProjectId;Request.CaseId=CaseId;Request.Spec=Proposed;Request.SampleBudget=1024;
            const auto Preview=StudioHome4Authoring::Build(Request,C.ToSharedRef());
            if(!Preview.IsValid()){R.bGood=false;R.Error=TEXT("Preset geometric preparation failed; current request retained. ")+Preview.Error;}
            else if(Preview.Mesh&&(!Preview.Body.IsValid||Preview.Body.Min.ContainsNaN()||Preview.Body.Max.ContainsNaN()||!Preview.Tank.IsInsideOrOn(Preview.Body.Min)||!Preview.Tank.IsInsideOrOn(Preview.Body.Max)))
            {R.bGood=false;R.Error=TEXT("Preset tank would clip the complete prepared body; current request retained.");}
        }
        if(R.bGood)
        {
            FStudioHome4TankZonePreset Last;R.bGood=StudioHome4TankPresets::Load(Original.SourcePath,Last,R.Error,C);
            if(R.bGood&&(Last.SourceSHA256!=Original.SourceSHA256||Last.OriginalBytes!=Original.OriginalBytes)){R.bGood=false;R.Error=TEXT("Original preset changed during geometric preparation; current request retained.");}
        }
        return R;
    });Message=TEXT("Rechecking original bytes before retaining the reviewed preset; current request unchanged.");
}
bool SStudioHome4TankPresets::BeginImport(const FString& Path)
{
    Scope();if(IsImporting()){Message=TEXT("An original preset read is already active.");return false;}if(Path.IsEmpty()){Message=TEXT("Preset import cancelled; previous slot retained.");return false;}
    FStudioHome4Spec S;FString E;if(!Session->Build(S,E)||S.RecipeId!=TEXT("th01-hull")){Message=E.IsEmpty()?TEXT("Choose TH01 before importing its G/Q/P definitions."):E;return false;}
    ImportProject=Project;ImportCase=Case;ImportBase=StudioHome4Authoring::Fingerprint(S);ImportName=Selected;bApplying=false;Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    TFunction<void()> BeforeVerify;
#if WITH_DEV_AUTOMATION_TESTS
    BeforeVerify=BeforeImportVerify;
#endif
    Pending=Async(EAsyncExecution::ThreadPool,[Path,C=Cancellation,BeforeVerify]{FResult R;R.bGood=StudioHome4TankPresets::Load(Path,R.Preset,R.Error,C,BeforeVerify);return R;});Message=TEXT("Reading exact original preset; current draft and prior slot retained.");return true;
}
void SStudioHome4TankPresets::PollImport()
{
    Scope();if(!Pending.IsValid()||!Pending.IsReady())return;auto R=Pending.Get();Pending={};const bool Cancelled=Cancellation&&Cancellation->load(),Applying=bApplying;bApplying=false;Cancellation.Reset();
    FStudioHome4Spec S;FString E;if(Project!=ImportProject||Case!=ImportCase||!Session->Build(S,E)||StudioHome4Authoring::Fingerprint(S)!=ImportBase)
    {Message=TEXT("Preset import scope or draft changed; previous slot retained.");return;}if(Cancelled){Message=TEXT("Original preset read cancelled; previous slot retained.");return;}
    if(!R.bGood){Message=R.Error;return;}if(R.Preset.Name!=ImportName){Message=TEXT("Original preset name differs from selected slot; previous slot retained.");return;}
    if(Applying)
    {
        if(!Session->Replace(ReviewedApply,E)){Message=E;return;}ReviewBase.Empty();
        if(Authoring&&Authoring->Draft()==Session){Authoring->Cancel();if(!Authoring->Request()){Message=TEXT("Preset tank/zone request retained; geometric preparation: ")+Authoring->Status;return;}}
        Message=TEXT("Original ")+Selected+TEXT(" tank/zone values and provenance retained. Actual authored regions prepare in the shared geometric view; Apply configuration saves with undo.");Session->Status=Message;return;
    }
    Slots.Add(ImportName,MoveTemp(R.Preset));Review();
}
void SStudioHome4TankPresets::CancelImport(){if(Cancellation)Cancellation->store(true);Message=TEXT("Original preset cancellation requested; previous slot and draft retained.");}
void SStudioHome4TankPresets::Import()
{
    FString P;const bool Good=ImportPath?ImportPath(P):StudioFileDialog::DataFile(false,TEXT("Import original TH01 G/Q/P definition"),{},TEXT("json"),P);if(Good)BeginImport(P);else Message=TEXT("Preset import cancelled; previous slot retained.");
}
void SStudioHome4TankPresets::Export()
{
    Scope();const auto* P=Slots.Find(Selected);if(!P){Message=TEXT("Original ")+Selected+TEXT(" definition is missing; nothing exported.");return;}FString Path;
    const bool Good=ExportPath?ExportPath(Path):StudioFileDialog::DataFile(true,TEXT("Export exact original TH01 preset"),{},TEXT("json"),Path);if(!Good){Message=TEXT("Preset export cancelled; original source retained.");return;}
    FString E;Message=StudioHome4TankPresets::Export(*P,Path,E)?TEXT("Exact original preset bytes exported to a new destination."):E;
}
FString SStudioHome4TankPresets::Details()const
{
    FStudioHome4Spec S;FString E;FString T;
    if(Session->Build(S,E,true)&&!S.Authoring.TankZonePresetName.IsEmpty())
    {
        const auto* Applied=Slots.Find(S.Authoring.TankZonePresetName);
        T=TEXT("Draft provenance: ")+S.Authoring.TankZonePresetName+TEXT(" · ")+S.Authoring.TankZonePresetSourceId+TEXT("\nSHA256 ")+S.Authoring.TankZonePresetSourceSHA256+TEXT("\n")+
            (Applied?(StudioHome4TankPresets::MatchesApplied(S,*Applied)?TEXT("Current tank/zone payload matches imported original."):TEXT("Edited from the imported original preset; current values differ.")):TEXT("Original definition is not loaded in this workspace; exact current match is not asserted."))+TEXT("\n\n");
    }
    const auto* P=Slots.Find(Selected);return T+(P?StudioHome4TankPresets::Review(*P):TEXT("Original ")+Selected+TEXT(" definition missing. Required JSON: schema LBMStudio.Home4TankZonePreset, version 1, name G/Q/P, source_id and full run_spec with TH01 reference L, tank, waterline, zone units and explicit zones. No built-in numbers."));
}
