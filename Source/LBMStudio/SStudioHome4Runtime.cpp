#include "SStudioHome4Runtime.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "StudioFileDialog.h"
#include "StudioHome4JSON.h"
#include "Async/Async.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#define UI UI_HOME4_RUNTIME_IMPORT
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioHome4RuntimeUIPrivate
{
    FString Number(const TOptional<double>& N, const TCHAR* Unit = TEXT(""))
    { return N ? FString::Printf(TEXT("%.6g %s"), *N, Unit) : TEXT("unknown"); }
    const TCHAR* Backend(EStudioHome4Backend B)
    { return B == EStudioHome4Backend::Metal ? TEXT("Metal") : B == EStudioHome4Backend::CUDA ? TEXT("CUDA") : B == EStudioHome4Backend::PyTorch ? TEXT("PyTorch fallback") : TEXT("unknown"); }
    bool Integer(const FString& S, int64& N)
    {
        const FString V = S.TrimStartAndEnd(); if (V.IsEmpty() || V.Len() > 13) return false; N = 0;
        for (TCHAR C : V) { if (C < '0' || C > '9' || N > (1000000000000LL - (C - '0')) / 10) return false; N = N * 10 + C - '0'; }
        return N > 0;
    }
}
void SStudioHome4Runtime::Construct(const FArguments& A)
{
    using namespace StudioUI; Model = A._Model; Runtime = A._Runtime ? A._Runtime : MakeShared<FStudioHome4RuntimeSession>(A._Model);
    OwnerDraft = FPlatformProcess::UserName(); SizeDraft.SetNum(4);SyncOutputDraft();
    auto Rows = SNew(SVerticalBox);
    auto Button = [&](const TCHAR* Name, FName Tag, TFunction<void()> Work)
    { return SNew(SButton).Tag(Tag).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7, 5)).OnClicked_Lambda([Work] { Work(); return FReply::Handled(); })[Label(Name, 9)]; };
    auto Input = [&](const TCHAR* Caption, FName Tag, FString* Value)
    { Rows->AddSlot().AutoHeight().Padding(0, 3)[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(Caption, 8, Muted)]
        + SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).Tag(Tag).Style(&InputStyle()).Font(Font(9)).Text_Lambda([Value] { return FText::FromString(*Value); }).OnTextChanged_Lambda([Value](const FText& V) { *Value = V.ToString(); })]]; };
    Rows->AddSlot().AutoHeight()[Label(TEXT("Targets, queue and original science"), 13, Text, true)];
    Rows->AddSlot().AutoHeight().Padding(0, 4)[SNew(STextBlock).Tag(TEXT("Home4RuntimeSummary")).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(Summary()); })];
    Rows->AddSlot().AutoHeight()[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)[Button(TEXT("Local development response"), TEXT("Home4RuntimeLocal"), [this] { SelectDevelopmentTarget(TEXT("local"), EStudioHome4Backend::Metal); })]
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)[Button(TEXT("Remote development response"), TEXT("Home4RuntimeRemote"), [this] { SelectDevelopmentTarget(TEXT("gpulab"), EStudioHome4Backend::CUDA); })]
        + SHorizontalBox::Slot().FillWidth(1)[Button(TEXT("Fallback development response"), TEXT("Home4RuntimeFallback"), [this] { SelectDevelopmentTarget(TEXT("local"), EStudioHome4Backend::PyTorch); })]];
    Rows->AddSlot().AutoHeight().Padding(0, 4)[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)[Button(TEXT("Import original target status…"), TEXT("Home4RuntimeImportStatus"), [this] { Import(false); })]
        + SHorizontalBox::Slot().FillWidth(1)[Button(TEXT("Confirm this fallback"), TEXT("Home4RuntimeConfirmFallback"), [this] { const auto B = Runtime->BackendVerification(); Notice = B && Runtime->ConfirmFallback(B->Id) ? TEXT("Fallback confirmed for this exact response. Changed backend response requires new confirmation.") : TEXT("Select an effective fallback response first."); })]];
    Rows->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("Home4RuntimeCancelImport")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda([this]{return IsImporting();}).OnClicked_Lambda([this]{CancelImport();return FReply::Handled();})[Label(TEXT("Cancel original status/history read"),9)]];
    Input(TEXT("Queue owner"), TEXT("Home4RuntimeOwner"), &OwnerDraft);
    Rows->AddSlot().AutoHeight().Padding(0, 4)[Button(TEXT("Queue immutable current request"), TEXT("Home4RuntimeSubmit"), [this] { SubmitCurrent(); })];
    Input(TEXT("Step N (development counter)"), TEXT("Home4RuntimeStepCount"), &StepDraft);
    Input(TEXT("Run to explicit t*"), TEXT("Home4RuntimeTargetTime"), &TargetDraft);
    Rows->AddSlot().AutoHeight()[SAssignNew(Jobs, SVerticalBox)];
    Rows->AddSlot().AutoHeight().Padding(0, 12, 0, 4)[Label(TEXT("Live original log"), 11, Text, true)];
    Input(TEXT("Original run GUID"), TEXT("Home4RuntimeOriginalRun"), &RunDraft);
    Input(TEXT("Original JSONL path"), TEXT("Home4RuntimeLogPath"), &LogPathDraft);
    Rows->AddSlot().AutoHeight().Padding(0, 4)[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)[Button(TEXT("Choose original log…"), TEXT("Home4RuntimeChooseLog"), [this] { FString P; if (StudioFileDialog::DataFile(false, TEXT("Attach original growing HOME4 JSONL"), TEXT(""), TEXT("jsonl"), P)) LogPathDraft = P; })]
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)[Button(TEXT("Attach live tail"), TEXT("Home4RuntimeAttachLog"), [this] { AttachLog(); })]
        + SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Detach"), TEXT("Home4RuntimeDetachLog"), [this] { Runtime->DetachLiveLog(); })]];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4RuntimeTailStatus")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(Runtime->LiveStatus()); })];
    Rows->AddSlot().AutoHeight().Padding(0, 6)[SNew(STextBlock).Tag(TEXT("Home4RuntimeLogLines")).Font(Font(8)).ColorAndOpacity(Text).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(FString::Join(Runtime->LogLines(), TEXT("\n"))); })];
    Rows->AddSlot().AutoHeight().Padding(0, 12, 0, 4)[Label(TEXT("Planned output disk forecast"), 11, Text, true)];
    const TCHAR* Kinds[] = {TEXT("Trace bytes/output"), TEXT("Slice bytes/output"), TEXT("Viz bytes/output"), TEXT("Restart bytes/output")};
    for (int32 I = 0; I < 4; ++I) Input(Kinds[I], FName(*FString::Printf(TEXT("Home4RuntimeOutputSize%d"), I)), &SizeDraft[I]);
    Input(TEXT("Original estimate source / allocation basis"),TEXT("Home4RuntimeEstimateSource"),&EstimateSourceDraft);
    Input(TEXT("Explicit compression/header/storage assumption"),TEXT("Home4RuntimeEstimateAssumption"),&EstimateAssumptionDraft);
    Rows->AddSlot().AutoHeight()[Button(TEXT("Save attributed output estimates to this request"),TEXT("Home4RuntimeApplyEstimates"),[this]{ApplyOutputEstimates();})];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4RuntimeDiskForecast")).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true).Text_Lambda([this]
    {
        const auto M = Model.Pin(); if (!M || !M->Project.Draft.Home4) return FText::FromString(TEXT("Apply a run specification to forecast its outputs."));
        const auto Layout=FStudioHome4RuntimeSession::ConfiguredOutputLayouts(*M->Project.Draft.Home4);
        const auto R = Runtime->ResourceStatus(); const auto F = FStudioHome4RuntimeSession::Forecast(*M->Project.Draft.Home4, Layout, R && R->DiskFreeBytes ? TOptional<uint64>(uint64(*R->DiskFreeBytes)) : TOptional<uint64>());
        FString S; const TCHAR* Names[] = {TEXT("Trace"), TEXT("Slice"), TEXT("Viz"), TEXT("Restart")};
        for (int32 I = 0; I < 4; ++I) S += FString(Names[I]) + TEXT(" · count ") + (F.Counts[I] ? LexToString(*F.Counts[I]) : TEXT("unknown")) + TEXT(" · bytes ") + (F.Bytes[I] ? LexToString(*F.Bytes[I]) : TEXT("unknown")) + TEXT(" · ") + F.Reasons[I] + TEXT("\n");
        S += TEXT("Full planned total: ") + (F.TotalBytes ? LexToString(*F.TotalBytes) + TEXT(" bytes") : TEXT("unknown"));
        if (R) S += TEXT(" · destination free ") + StudioHome4RuntimeUIPrivate::Number(R->DiskFreeBytes, TEXT("bytes"));
        if (F.bExceedsSuppliedFreeSpace) S += TEXT(" · EXCEEDS supplied destination space");
        return FText::FromString(S);
    })];
    Rows->AddSlot().AutoHeight().Padding(0, 12, 0, 4)[Label(TEXT("Original completed-run throughput history"), 11, Text, true)];
    Rows->AddSlot().AutoHeight()[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)[Button(TEXT("Import measured history…"), TEXT("Home4RuntimeImportHistory"), [this] { Import(true); })]
        + SHorizontalBox::Slot().FillWidth(1)[Button(TEXT("Export measured history…"), TEXT("Home4RuntimeExportHistory"), [this] { ExportHistory(); })]];
    Rows->AddSlot().AutoHeight()[SAssignNew(Costs, SVerticalBox)];
    Rows->AddSlot().AutoHeight().Padding(0, 7)[SNew(STextBlock).Tag(TEXT("Home4RuntimeNotice")).Font(Font(9)).ColorAndOpacity(Amber).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(Notice); })];
    ChildSlot[SNew(SBorder).BorderImage(&PanelBrush).Padding(10)[Rows]]; Refresh();
}
void SStudioHome4Runtime::Tick(const FGeometry& G, double Now, float Delta)
{ SCompoundWidget::Tick(G, Now, Delta); Runtime->Scope();SyncOutputDraft();PollImport();Refresh(); }
bool SStudioHome4Runtime::SelectDevelopmentTarget(const FString& Target, EStudioHome4Backend Backend)
{
    FStudioHome4BackendVerification B; B.Id = FGuid::NewGuid(); B.Target = Target; B.Host = Target == TEXT("local") ? TEXT("local development endpoint") : TEXT("gpulab development endpoint");
    B.Device = Target == TEXT("local") ? TEXT("development-local-device") : TEXT("development-remote-device"); B.Source = TEXT("Explicit development backend protocol response; no numerical extension loaded");
    B.EffectiveBackend = Backend; B.ExtensionImported = Backend != EStudioHome4Backend::PyTorch; B.bDevelopmentResponse = true; B.ObservedAt = FPlatformTime::Seconds();
    const bool OK = Runtime->SetBackendVerification(B, Notice); if (OK) Notice = TEXT("Development target response selected. No hardware measurements, extension import or solver execution claimed."); return OK;
}
bool SStudioHome4Runtime::SubmitCurrent()
{
    const auto M = Model.Pin(); if (!M || !M->Project.Draft.Home4) { Notice = TEXT("Apply a HOME4 run specification before queueing."); return false; }
    const auto Id = FGuid::NewGuid(); const bool OK = Runtime->Submit(*M->Project.Draft.Home4, Id, OwnerDraft, FPlatformTime::Seconds(), Notice);
    if (OK) { RunDraft = Id.ToString(); Notice = TEXT("Current request frozen and queued. Development completion never establishes numerical validation."); QueueSignature = MAX_uint64; Refresh(); } return OK;
}
FString SStudioHome4Runtime::Summary() const
{
    using namespace StudioHome4RuntimeUIPrivate; const auto B = Runtime->BackendVerification();
    if (!B) return TEXT("No reviewed target/backend response. Choose explicit development protocol or import original target status. HOME4 engine execution is stubbed.");
    FString S = (B->bDevelopmentResponse ? TEXT("DEVELOPMENT response · ") : TEXT("IMPORTED original target response · ")) + FString(Backend(B->EffectiveBackend)) + TEXT(" · ") + B->Target + TEXT(" / ") + B->Host + TEXT(" / ") + B->Device + TEXT("\nSource: ") + B->Source;
    if (const auto R = Runtime->ResourceStatus())
        S += FString::Printf(TEXT("\nOriginal status age %.3g s; %s. Missing ownership/utilization never certifies a free device.\nGPU "),FMath::Max(0.,FPlatformTime::Seconds()-R->ObservedAt),FPlatformTime::Seconds()-R->ObservedAt>5?TEXT("stale snapshot — refresh the source response"):TEXT("recent supplied snapshot")) + Number(R->UtilizationPercent, TEXT("%")) + TEXT(" · memory ") + Number(R->MemoryUsedBytes, TEXT("bytes")) + TEXT(" / ") + Number(R->MemoryTotalBytes, TEXT("bytes")) +
            TEXT(" · power ") + Number(R->PowerWatts, TEXT("W")) + TEXT(" · temperature ") + Number(R->TemperatureC, TEXT("C")) + TEXT(" · clock ") + Number(R->ClockMHz, TEXT("MHz")) + TEXT("\nOwner ") + (R->Owner.IsEmpty() ? TEXT("unknown") : R->Owner) + TEXT(" · job ") + (R->JobId.IsEmpty() ? TEXT("unknown") : R->JobId) + TEXT(" · disk free ") + Number(R->DiskFreeBytes, TEXT("bytes"));
    return S;
}
void SStudioHome4Runtime::Refresh()
{
    using namespace StudioUI; uint64 Signature = 0; for (const auto& J : Runtime->QueueJobs()) Signature += J.Sequence + GetTypeHash(J.RunId);
    if (Signature != QueueSignature)
    {
        QueueSignature = Signature; Jobs->ClearChildren();
        for (const auto& J : Runtime->QueueJobs())
        {
            const auto Id = J.RunId; auto Row = SNew(SVerticalBox);
            Row->AddSlot().AutoHeight().Padding(0, 5)[SNew(STextBlock).Tag(FName(*(TEXT("Home4QueueJob.") + Id.ToString()))).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true)
                .Text_Lambda([this,Id]
                {
                    const auto* Job=Runtime->QueueJobs().FindByPredicate([&](const auto& V){return V.RunId==Id;});if(!Job)return FText::GetEmpty();
                    const double End=Job->State==EStudioHome4QueueState::Completed||Job->State==EStudioHome4QueueState::Cancelled||Job->State==EStudioHome4QueueState::Failed||Job->State==EStudioHome4QueueState::Stopped?Job->LastTransitionAt:FPlatformTime::Seconds();
                    return FText::FromString(FStudioHome4RuntimeSession::StateName(Job->State)+TEXT(" · ")+Job->Target+TEXT(" / ")+Job->Host+TEXT(" / ")+Job->Device+TEXT(" · owner ")+Job->Owner+
                        TEXT(" · planned run ")+Id.ToString()+FString::Printf(TEXT(" · elapsed %.3g s\n"),FMath::Max(0.,End-Job->SubmittedAt))+Job->Notice+TEXT("\nScientific result: ")+(Job->bScientificResultsAttached?TEXT("identified original evidence attached"):TEXT("pending original evidence")));
                })];
            auto Actions = SNew(SHorizontalBox);
            auto Add = [&](const TCHAR* Name, const FString& Key, TFunction<void()> Work)
            { Actions->AddSlot().AutoWidth().Padding(0, 0, 4, 0)[SNew(SButton).Tag(FName(*(Key + Id.ToString()))).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([Work] { Work(); return FReply::Handled(); })[Label(Name, 8)]]; };
            for (const auto& C : TArray<TPair<const TCHAR*, EStudioJobCommand>>{{TEXT("Pause"), EStudioJobCommand::Pause}, {TEXT("Resume"), EStudioJobCommand::Resume}, {TEXT("Stop"), EStudioJobCommand::Stop}, {TEXT("Reconnect"), EStudioJobCommand::Reconnect}})
                Add(C.Key, FString(TEXT("Home4Queue")) + C.Key + TEXT("."), [this, Id, Cmd = C.Value] { Runtime->Command(Id, Cmd, FPlatformTime::Seconds(), Notice); });
            Add(TEXT("Step N"), TEXT("Home4QueueStep."), [this, Id] { int64 N = 0; if (!StudioHome4RuntimeUIPrivate::Integer(StepDraft, N)) Notice = TEXT("Enter a positive bounded integer step count."); else Runtime->Command(Id, EStudioJobCommand::Step, FPlatformTime::Seconds(), Notice, N); });
            Add(TEXT("Run to t*"), TEXT("Home4QueueTarget."), [this, Id] { double V = 0; if (!LexTryParseString(V, *TargetDraft) || !FMath::IsFinite(V) || V <= 0) Notice = TEXT("Enter an explicit finite positive target."); else Runtime->Command(Id, EStudioJobCommand::RunToDimensionless, FPlatformTime::Seconds(), Notice, 1, V); });
            Add(TEXT("Finish protocol"), TEXT("Home4QueueFinish."), [this, Id] { Runtime->CompleteDevelopment(Id, FPlatformTime::Seconds(), Notice); });
            Add(TEXT("Disconnect protocol"), TEXT("Home4QueueDisconnect."), [this, Id] { Runtime->DisconnectDevelopment(Id, FPlatformTime::Seconds(), Notice); });
            Add(TEXT("Fail protocol"), TEXT("Home4QueueFail."), [this, Id] { Runtime->FailDevelopment(Id, FPlatformTime::Seconds(), TEXT("Explicit user development failure event"), Notice); });
            Add(TEXT("Select"), TEXT("Home4QueueSelect."), [this, Id] { Runtime->SelectJob(Id); });
            Add(TEXT("Cancel"), TEXT("Home4QueueCancel."), [this, Id] { Runtime->Cancel(Id, FPlatformTime::Seconds(), Notice); });
            Row->AddSlot().AutoHeight()[Actions]; Jobs->AddSlot().AutoHeight()[Row];
        }
    }
    const auto M = Model.Pin();const auto B=Runtime->BackendVerification();
    const uint64 Revision=HashCombineFast(GetTypeHash(M?M->Project.Draft.Revision:0),GetTypeHash(B?B->Id:FGuid()));
    const uint64 CurrentCostSignature=HashCombineFast(GetTypeHash(Runtime->PerformanceRevision()),GetTypeHash(Revision));
    if(CurrentCostSignature!=CostSignature)
    {
        CostSignature=CurrentCostSignature; Costs->ClearChildren();
        for (const auto& R : Runtime->PerformanceRecords())
        {
            const auto Seconds = M && M->Project.Draft.Home4 ? Runtime->EstimatedSeconds(*M->Project.Draft.Home4, R) : TOptional<double>();
            Costs->AddSlot().AutoHeight().Padding(0, 4)[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true).Text(FText::FromString(R.Host + TEXT(" / ") + R.Device + TEXT(" · ") + R.RecipeId + FString::Printf(TEXT(" · %.6g measured MLUPS\nCurrent request estimate "), R.MLUPS()) + StudioHome4RuntimeUIPrivate::Number(Seconds, TEXT("s")) + TEXT(" · retabulation policy ")+(R.RetabulationPolicy.IsEmpty()?TEXT("unknown"):R.RetabulationPolicy)+TEXT(" · body mode ")+(R.OriginalRunSpec&&!R.OriginalRunSpec->Geometry.BodyMotion.IsEmpty()?R.OriginalRunSpec->Geometry.BodyMotion:TEXT("original specification unknown"))+TEXT(" · original run ") + R.RunId.ToString() + TEXT("\n") + R.Source + TEXT(" · SHA256 ") + R.SourceSHA256))];
        }
    }
}
void SStudioHome4Runtime::SyncOutputDraft()
{
    const auto M=Model.Pin();if(!M)return;const FGuid P=M->Project.Id,C=M->Project.Draft.Id;
    const FString Config=M->Project.Draft.Home4?StudioHome4Config::Serialize(*M->Project.Draft.Home4):FString();
    if(P==ScopedProjectId&&C==ScopedCaseId&&Config==ShownOutputConfig)return;
    if(P!=ScopedProjectId||C!=ScopedCaseId){RunDraft.Empty();LogPathDraft.Empty();TargetDraft.Empty();Notice=TEXT("Project/case changed; target/log attachment requires original source identity.");QueueSignature=MAX_uint64;CostSignature=MAX_uint64;}
    ScopedProjectId=P;ScopedCaseId=C;ShownOutputConfig=Config;
    const auto* Performance=M->Project.Draft.Home4?&M->Project.Draft.Home4->Performance:nullptr;
    for(int32 I=0;I<4;++I)SizeDraft[I]=Performance&&Performance->OutputByteEstimates.Num()==4&&Performance->OutputByteEstimates[I]>0?LexToString(Performance->OutputByteEstimates[I]):FString();
    EstimateSourceDraft=Performance?Performance->OutputEstimateSource:FString();EstimateAssumptionDraft=Performance?Performance->OutputEstimateAssumption:FString();
}
bool SStudioHome4Runtime::ApplyOutputEstimates()
{
    const auto M=Model.Pin();if(!M||!M->Project.Draft.Home4){Notice=TEXT("Apply a HOME4 request before storing output estimates.");return false;}
    auto Spec=*M->Project.Draft.Home4;Spec.Performance.OutputByteEstimates.Reset();
    for(const auto& Text:SizeDraft)
    {
        double Value=0;if(!Text.TrimStartAndEnd().IsEmpty()&&(!LexTryParseString(Value,*Text)||!FMath::IsFinite(Value)||Value<0||Value>1.e12))
        {Notice=TEXT("Output estimates must be explicit finite bytes from 0 to 1e12; zero or empty remains unknown.");return false;}
        Spec.Performance.OutputByteEstimates.Add(Value);
    }
    Spec.Performance.OutputEstimateSource=EstimateSourceDraft;Spec.Performance.OutputEstimateAssumption=EstimateAssumptionDraft;
    if(!StudioHome4Config::Validate(Spec,Notice))return false;
    M->Project.Draft.Home4=MoveTemp(Spec);++M->Project.Draft.Revision;M->bDirty=true;ShownOutputConfig.Empty();SyncOutputDraft();
    Notice=TEXT("Attributed output sizes and explicit storage assumptions saved with this request. Missing channel sizes remain unknown.");return true;
}
void SStudioHome4Runtime::AttachLog()
{
    FGuid Run; if (!FGuid::Parse(RunDraft, Run) || !Run.IsValid()) { Notice = TEXT("Supply the original run GUID before live attachment."); return; }
    Runtime->AttachLiveLog(LogPathDraft, Run, Notice);
}
SStudioHome4Runtime::FOriginalImport SStudioHome4Runtime::ReadOriginal(const FString& Path,bool History,FGuid Project,FGuid Case,const TFunction<void()>& BeforeVerify)
{
    FOriginalImport R;R.Path=Path;R.ProjectId=Project;R.CaseId=Case;R.bHistory=History;FStudioFileAccess Access(Path);
    const int64 Size=IFileManager::Get().FileSize(*Path);const auto Stamp=IFileManager::Get().GetTimeStamp(*Path);
    if(Size<=0||Size>1024*1024){R.Error=TEXT("Original target/history response must be between 1 byte and 1 MiB.");return R;}
    auto Read=[&](TArray<uint8>& B)
    {
        TUniquePtr<FArchive> F(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));if(!F||F->TotalSize()!=Size)return false;
        B.SetNumUninitialized(int32(Size));F->Serialize(B.GetData(),Size);const bool Good=!F->IsError();const bool Closed=F->Close();F.Reset();return Good&&Closed;
    };
    TArray<uint8> Bytes,Checked;
    if(!Read(Bytes)||!StudioHome4JSON::UTF8(Bytes.GetData(),Bytes.Num())){R.Error=TEXT("Original response could not be read as strict UTF-8.");return R;}
    const int32 Offset=Bytes.Num()>=3&&Bytes[0]==0xef&&Bytes[1]==0xbb&&Bytes[2]==0xbf?3:0;
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+Offset),Bytes.Num()-Offset);R.JSON=FString(Converted.Length(),Converted.Get());
    if(BeforeVerify)BeforeVerify();
    if(!Read(Checked)||Checked!=Bytes||IFileManager::Get().FileSize(*Path)!=Size||IFileManager::Get().GetTimeStamp(*Path)!=Stamp)
    {R.JSON.Empty();R.Error=TEXT("Original response changed during import; prior target/history retained.");return R;}
    uint8 Digest[32];unsigned int Count=0;
    if(EVP_Digest(Bytes.GetData(),Bytes.Num(),Digest,&Count,EVP_sha256(),nullptr)!=1||Count!=32)
    {R.JSON.Empty();R.Error=TEXT("Original response could not be hashed.");return R;}
    R.SHA256=BytesToHex(Digest,Count).ToLower();return R;
}
bool SStudioHome4Runtime::ImportPath(const FString& Path,bool History)
{
    if(IsImporting()){Notice=TEXT("An original target/history import is already running.");return false;}
    if(Path.IsEmpty()){Notice=TEXT("Import cancelled; prior target/history retained.");return false;}
    const auto M=Model.Pin();if(!M)return false;const FGuid Project=M->Project.Id,Case=M->Project.Draft.Id;
    TFunction<void()> BeforeVerify;
#if WITH_DEV_AUTOMATION_TESTS
    BeforeVerify=BeforeImportVerify;
#endif
    bCancelImport=false;PendingImport=Async(EAsyncExecution::ThreadPool,[Path,History,Project,Case,BeforeVerify]{return ReadOriginal(Path,History,Project,Case,BeforeVerify);});
    Notice=TEXT("Reading identified original target/history response in the background; prior data retained.");return true;
}
void SStudioHome4Runtime::PollImport()
{
    if(!PendingImport.IsValid()||!PendingImport.IsReady())return;
    auto R=MoveTemp(PendingImport.GetMutable());PendingImport={};const auto M=Model.Pin();
    if(bCancelImport){Notice=TEXT("Original response import cancelled; prior target/history retained.");bCancelImport=false;return;}
    if(!M||M->Project.Id!=R.ProjectId||M->Project.Draft.Id!=R.CaseId){Notice=TEXT("Project/case changed while importing; original response was not attached.");return;}
    if(!R.Error.IsEmpty()){Notice=R.Error;return;}
    const bool Good=R.bHistory?Runtime->ParsePerformanceHistory(R.JSON,Notice):Runtime->ImportTargetStatus(R.JSON,FPlatformTime::Seconds(),Notice);
    if(Good){Notice=TEXT("Identified original response imported from ")+R.Path+TEXT(" · SHA256 ")+R.SHA256+TEXT(". Missing measurements remain unknown.");CostSignature=MAX_uint64;Refresh();}
}
void SStudioHome4Runtime::Import(bool History)
{
    FString Path;
    if(StudioFileDialog::DataFile(false,History?TEXT("Import original completed-run measured history"):TEXT("Import original target/backend status response"),TEXT(""),TEXT("json"),Path))ImportPath(Path,History);
}
void SStudioHome4Runtime::ExportHistory()
{
    FString Path; if (!StudioFileDialog::DataFile(true, TEXT("Export original measured hardware/workload history"), TEXT("home4-measured-history.json"), TEXT("json"), Path)) return;
    if (StudioFileDialog::WriteAtomic(Path, Runtime->SerializePerformanceHistory(), Notice)) Notice = TEXT("Measured history saved with original run/source identities.");
}
