#include "StudioHome4Runtime.h"
#include "StudioHome4Checkpoint.h"
#include "StudioHome4Validation.h"
#include "StudioHome4JSON.h"
#include "StudioModel.h"
#include "StudioFileDialog.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#define UI UI_HOME4_RUNTIME
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioHome4RuntimePrivate
{
    constexpr int32 MaxTailBytes = 65536, MaxOriginalBytes = 64 * 1024 * 1024;
    bool Clean(const FString& V, int32 Max = 256, bool Required = true)
    { if (V.Len() > Max || (Required && V.TrimStartAndEnd().IsEmpty())) return false; for (TCHAR C : V) if (C < 32 || C == 127) return false; return true; }
    bool Hash(const TArray<uint8>& Bytes, FString& Out)
    {
        uint8 Digest[32]; unsigned int Count = 0;
        if (EVP_Digest(Bytes.GetData(), Bytes.Num(), Digest, &Count, EVP_sha256(), nullptr) != 1 || Count != 32) return false;
        Out = BytesToHex(Digest, Count).ToLower(); return true;
    }
    bool SHA(const FString& V)
    { if (V.Len() != 64) return false; for (TCHAR C : V) if (!FChar::IsHexDigit(C)) return false; return true; }
    bool Time(double Now) { return FMath::IsFinite(Now) && Now >= 0; }
    bool Terminal(EStudioHome4QueueState S)
    { return S == EStudioHome4QueueState::Completed || S == EStudioHome4QueueState::Cancelled || S == EStudioHome4QueueState::Stopped || S == EStudioHome4QueueState::Failed; }
    bool KnownBackend(EStudioHome4Backend B)
    { return B == EStudioHome4Backend::Metal || B == EStudioHome4Backend::CUDA || B == EStudioHome4Backend::PyTorch; }
    FString BackendName(EStudioHome4Backend B)
    { return B == EStudioHome4Backend::Metal ? TEXT("metal") : B == EStudioHome4Backend::CUDA ? TEXT("cuda") : B == EStudioHome4Backend::PyTorch ? TEXT("pytorch") : TEXT("unknown"); }
    EStudioHome4Backend ParseBackend(const FString& V)
    { return V == TEXT("metal") ? EStudioHome4Backend::Metal : V == TEXT("cuda") ? EStudioHome4Backend::CUDA : V == TEXT("pytorch") ? EStudioHome4Backend::PyTorch : EStudioHome4Backend::Unknown; }
    bool Object(const FString& JSON, TSharedPtr<FJsonObject>& Out)
    { return JSON.Len() <= 1024 * 1024 && StudioHome4JSON::Preflight(JSON) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON), Out) && Out; }
    bool Text(const TSharedPtr<FJsonObject>& O, const TCHAR* K, FString& V, bool Required = true)
    { const auto F = O->TryGetField(K); if (!F || F->Type == EJson::Null) return !Required; return F->Type == EJson::String && F->TryGetString(V) && Clean(V, 2048, Required); }
    bool Number(const TSharedPtr<FJsonObject>& O, const TCHAR* K, TOptional<double>& V, double Minimum = 0, double Maximum = 1.e18)
    { const auto F = O->TryGetField(K); if (!F || F->Type == EJson::Null) return true; double N = 0; if (F->Type != EJson::Number || !F->TryGetNumber(N) || !FMath::IsFinite(N) || N < Minimum || N > Maximum) return false; V = N; return true; }
    FString JSON(const TSharedRef<FJsonObject>& O)
    { FString Out; FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&Out)); return Out; }
}

FStudioHome4RuntimeSession::FStudioHome4RuntimeSession(TSharedPtr<FStudioModel> InModel) : Model(InModel)
{ Scope(); }
FStudioHome4RuntimeSession::~FStudioHome4RuntimeSession() { DetachLiveLog(); }
void FStudioHome4RuntimeSession::Scope()
{
    const auto M = Model.Pin(); const FGuid P = M ? M->Project.Id : FGuid(), C = M ? M->Project.Draft.Id : FGuid();
    if (P == ProjectId && C == CaseId) return;
    DetachLiveLog(); Jobs.Reset(); Guards.Reset(); Lines.Reset(); Backend.Reset(); Resources.Reset(); FallbackAcknowledged.Invalidate(); SelectedRunId.Invalidate();
    ProjectId = P; CaseId = C; TailStatus = TEXT("Project/case changed; attach its original live log and target response.");
}
void FStudioHome4RuntimeSession::DetachLiveLog()
{
    if (Cancellation) Cancellation->store(true, std::memory_order_relaxed);
    Cancellation.Reset(); ++Generation; TailPath.Empty(); TailRun.Invalidate(); TailOffset = 0; HistoricalPrefixEnd = 0; bTailBlocked = false; bHistoricalFragment = false; Anchor.Reset(); OriginalBytes.Reset(); LogPending.Reset(); Stream.Reset();
    LastScienceAt = -1; LastGuardRecord = 0; bCaptureComplete = true; TailStatus = TEXT("Live source detached; retained imports remain historical replay.");
}
bool FStudioHome4RuntimeSession::AttachLiveLog(const FString& Path, const FGuid& OriginalRun, FString& Error)
{
    using namespace StudioHome4RuntimePrivate; Scope();
    if (!ProjectId.IsValid() || !CaseId.IsValid() || !OriginalRun.IsValid() || !Clean(Path, 4096))
    { Error = TEXT("Live log needs this project/case, a valid original run GUID and path."); return false; }
    const FString Full = FPaths::ConvertRelativePathToFull(Path);
    if (Full.Len() > 256) { Error = TEXT("Live source path exceeds the science source identity limit of 256 characters."); return false; }
    FStudioFileAccess Access(Full);
    const int64 ExistingBytes = IFileManager::Get().FileSize(*Full);
    if (ExistingBytes < 0) { Error = TEXT("Select an existing original live JSONL log."); return false; }
    DetachLiveLog(); TailPath = Full; TailRun = OriginalRun; Cancellation = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    HistoricalPrefixEnd = ExistingBytes;
    Stream = MakeShared<FStudioHome4TelemetryStream>(); Stream->BeginRun({OriginalRun, Full}); Stream->SetDiagnosticPolicy(Policy);
    NextReadAt = 0; TailStatus = TEXT("LIVE · bounded original log tail attached. Historical existing prefix is ingested without live guard dispatch."); Error.Empty(); return true;
}
void FStudioHome4RuntimeSession::SetDiagnosticPolicy(const FStudioHome4DiagnosticPolicy& InPolicy)
{ Policy = InPolicy; if (Stream) Stream->SetDiagnosticPolicy(Policy); }
FStudioHome4RuntimeSession::FTailRead FStudioHome4RuntimeSession::ReadTail(const FString& Path, int64 Offset, const TArray<uint8>& ExpectedAnchor,
    uint64 Epoch, int64 HistoricalEnd, bool HistoricalFragment, const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& Cancel)
{
    using namespace StudioHome4RuntimePrivate; FTailRead R; R.Generation = Epoch; R.Offset = Offset;
    if (!Cancel || Cancel->load(std::memory_order_relaxed)) return R;
    FStudioFileAccess Access(Path); TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!File) { R.Error = TEXT("Live log unavailable; reconnect or reattach the source."); return R; }
    R.Size = File->TotalSize(); if (R.Size < 0) { R.Error = TEXT("Live log size is unavailable."); return R; }
    const int32 AnchorCount = ExpectedAnchor.IsEmpty() ? int32(FMath::Min<int64>(R.Size, 4096)) : ExpectedAnchor.Num();
    R.Anchor.SetNumUninitialized(int32(FMath::Min<int64>(R.Size, AnchorCount)));
    if (!R.Anchor.IsEmpty()) File->Serialize(R.Anchor.GetData(), R.Anchor.Num());
    R.bReplaced = R.Size < Offset || (!ExpectedAnchor.IsEmpty() && R.Anchor != ExpectedAnchor);
    if (R.bReplaced) R.Offset = 0;
    const int64 End = R.Offset < HistoricalEnd ? FMath::Min(R.Size, HistoricalEnd) : R.Size;
    const int32 Count = int32(FMath::Min<int64>(MaxTailBytes, FMath::Max<int64>(0, End - R.Offset)));
    R.Bytes.SetNumUninitialized(Count); File->Seek(R.Offset); if (Count) File->Serialize(R.Bytes.GetData(), Count);
    if (HistoricalFragment && R.Offset >= HistoricalEnd)
        for (int32 I = 0; I < R.Bytes.Num(); ++I) if (R.Bytes[I] == '\n') { R.Bytes.SetNum(I + 1, EAllowShrinking::No); break; }
    if (File->IsError() || !File->Close()) { R.Bytes.Reset(); R.Error = TEXT("Live log read changed or failed; previous science retained."); }
    return R;
}
void FStudioHome4RuntimeSession::PollTail(double Now)
{
    using namespace StudioHome4RuntimePrivate;
    if (Pending.IsValid() && Pending.IsReady())
    {
        auto R = MoveTemp(Pending.GetMutable()); Pending = {};
        if (R.Generation != Generation || TailPath.IsEmpty()) return;
        if (!R.Error.IsEmpty()) { TailStatus = R.Error; NextReadAt = Now + 1; return; }
        if (R.bReplaced)
        {
            Stream->ResetTail(); LogPending.Reset(); bTailBlocked = true;
            TailStatus = TEXT("Original live file truncated/replaced; retained science and bytes preserved, partial line discarded. Explicitly reattach the verified original run to resume.");
            return;
        }
        if (Anchor.IsEmpty()) Anchor = R.Anchor;
        const auto Batch = Stream->AppendBytes(R.Bytes.GetData(), R.Bytes.Num());
        const int32 Offset = Batch.ConsumedBytes, Accepted = Batch.Accepted;
        const bool Invalid = Batch.Malformed > 0 || Batch.Oversized > 0 || Batch.Regressing > 0;
        for (int32 I = 0; I < Offset; ++I)
        {
            const uint8 Byte = R.Bytes[I];
            if (Byte == '\n')
            {
                if (!LogPending.IsEmpty() && StudioHome4JSON::UTF8(LogPending.GetData(), LogPending.Num()))
                {
                    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(LogPending.GetData()), LogPending.Num());
                    if (Lines.Num() >= 120) Lines.RemoveAt(0, 1, EAllowShrinking::No);
                    Lines.Add(FString(Text.Length(), Text.Get()));
                }
                LogPending.Reset();
            }
            else if (LogPending.Num() < 16384) LogPending.Add(Byte);
        }
        // At most one bounded read is parsed per owning tick; partial lines stay pending.
        TailOffset = R.Offset + Offset;
        if (bCaptureComplete && OriginalBytes.Num() <= MaxOriginalBytes - Offset) OriginalBytes.Append(R.Bytes.GetData(), Offset);
        else bCaptureComplete = false;
        if (Accepted > 0) LastScienceAt = Now;
        if (Invalid) TailStatus = TEXT("LIVE · malformed/oversized/regressing original records rejected; accepted science retained.");
        else if (Accepted > 0) TailStatus = TEXT("LIVE · original science appended. Control replies remain independent.");
        if (!bCaptureComplete) TailStatus += TEXT(" Original capture exceeds 64 MiB; retained bytes are a partial prefix, not the complete displayed source.");
        // Existing file prefix is historical when first attaching. It is displayed,
        // but cannot stop a current job. Only subsequently appended records dispatch.
        const bool Historical = R.Offset < HistoricalPrefixEnd || bHistoricalFragment;
        if (Historical && !Stream->ActionRequests().IsEmpty())
            LastGuardRecord = Stream->ActionRequests().Last().RecordIndex;
        if (R.Offset < HistoricalPrefixEnd && TailOffset == HistoricalPrefixEnd && HistoricalPrefixEnd > 0)
            bHistoricalFragment = Stream->BufferedBytes() > 0;
        else if (bHistoricalFragment && Offset > 0 && R.Bytes[Offset - 1] == '\n') bHistoricalFragment = false;
        DispatchGuards(Now); NextReadAt = Now + .1;
    }
    if (!TailPath.IsEmpty() && !bTailBlocked && !Pending.IsValid() && Now >= NextReadAt)
    {
        const FString Path = TailPath; const auto Prefix = Anchor; const int64 Offset = TailOffset, HistoricalEnd = HistoricalPrefixEnd; const bool Fragment = bHistoricalFragment; const uint64 Epoch = Generation; const auto Cancel = Cancellation;
        Pending = Async(EAsyncExecution::ThreadPool, [Path, Prefix, Offset, Epoch, HistoricalEnd, Fragment, Cancel] { return ReadTail(Path, Offset, Prefix, Epoch, HistoricalEnd, Fragment, Cancel); });
    }
}
void FStudioHome4RuntimeSession::Tick(double Now)
{
    using namespace StudioHome4RuntimePrivate; Scope(); if (!Time(Now) || Now < LastClock) return; LastClock = Now;
    PollTail(Now);
    for (auto& J : Jobs)
    {
        if (J.State == EStudioHome4QueueState::Queued)
        {
            const bool ForeignBusy = Resources && Resources->Target == J.Target && Resources->Host == J.Host && Resources->Device == J.Device &&
                Resources->UtilizationPercent && *Resources->UtilizationPercent > 0 && (Resources->Owner.IsEmpty() || Resources->Owner != J.Owner);
            const bool Busy = ForeignBusy || Jobs.ContainsByPredicate([&](const auto& Other)
            { return Other.RunId != J.RunId && Other.Target == J.Target && Other.Host == J.Host && Other.Device == J.Device && (Other.State == EStudioHome4QueueState::Preparing || Other.State == EStudioHome4QueueState::Running || Other.State == EStudioHome4QueueState::Paused || Other.State == EStudioHome4QueueState::Stopping); });
            if (!Busy) Transition(J, EStudioHome4QueueState::Preparing, Now, TEXT("Development launch request accepted; no solver process started."));
            else J.Notice = TEXT("Queued: selected device has an active or foreign tenant job.");
        }
        else if (J.State == EStudioHome4QueueState::Preparing && Now - J.LastTransitionAt >= .05)
        { J.StartedAt = Now; Transition(J, EStudioHome4QueueState::Running, Now, TEXT("Development protocol running; scientific result remains pending original evidence.")); }
        else if (J.State == EStudioHome4QueueState::Stopping && Now - J.LastTransitionAt >= .05)
            Transition(J, EStudioHome4QueueState::Stopped, Now, TEXT("Stop acknowledged by development protocol; no numerical solver or checkpoint file claimed."));
        SyncHistory(J, Now);
    }
    for (auto& G : Guards) if (!G.bStopAcknowledged)
        if (const auto* J = FindJob(G.Request.Source.RunId); J && (J->State == EStudioHome4QueueState::Stopped || J->State == EStudioHome4QueueState::Cancelled))
        { G.bStopAcknowledged = true; G.Stop = TEXT("Development stop acknowledged; recorded science remains original."); }
}
TOptional<FStudioHome4TelemetryProvenance> FStudioHome4RuntimeSession::ScienceProvenance()
{
    Scope(); if (!Stream || (!Stream->Latest() && Stream->OutputEvents().IsEmpty())) return {};
    FStudioHome4TelemetryProvenance P; P.StreamRunId = TailRun; P.OriginalRunId = TailRun; P.SourceId = TailPath; P.SourcePath = TailPath;
    StudioHome4RuntimePrivate::Hash(OriginalBytes, P.SourceSHA256); P.AttachedProjectId = ProjectId; P.AttachedCaseId = CaseId;
    P.bCapturedPrefix = true; P.CapturedByteCount = OriginalBytes.Num(); P.bCaptureCoversDisplayedData = bCaptureComplete; return P;
}
void FStudioHome4RuntimeSession::DispatchGuards(double Now)
{
    if (!Stream) return;
    for (const auto& A : Stream->ActionRequests())
    {
        if (A.RecordIndex <= LastGuardRecord) continue;
        LastGuardRecord = A.RecordIndex;
        FStudioHome4GuardOutcome O; O.Request = A;
        auto* J = FindJob(A.Source.RunId);
        if (!J || J->ProjectId != ProjectId || J->CaseId != CaseId || J->State == EStudioHome4QueueState::Completed || J->State == EStudioHome4QueueState::Cancelled || J->State == EStudioHome4QueueState::Stopped || J->State == EStudioHome4QueueState::Failed)
        { O.Stop = TEXT("No active source-matched owned job; no stop sent."); O.Recovery = TEXT("No active source-matched job; no recovery sent."); O.Locate = TEXT("Source not owned by an active job; automatic camera action suppressed."); }
        else
        {
            if (A.bCheckpointLastGoodState && A.LastGoodStep)
            { O.bRecoveryAcknowledged = true; O.Recovery = FString::Printf(TEXT("Development recovery request acknowledged for original last-good step %lld. No restart file generated; original solver capability remains unavailable."), *A.LastGoodStep); }
            else O.Recovery = TEXT("Recovery unavailable: no retained original last-good state. No checkpoint claim.");
            FString Error;
            if (Command(J->RunId, EStudioJobCommand::Stop, Now, Error)) O.Stop = TEXT("Stop requested; awaiting development acknowledgment.");
            else O.Stop = Error;
            if (A.bLocateCell && Locate) { Locate(A); O.Locate = TEXT("Source-bound original-cell camera/marker callback dispatched."); }
            else O.Locate = TEXT("Location or compatible owner callback unavailable; camera retained.");
        }
        if (Guards.Num() >= 64) Guards.RemoveAt(0, 1, EAllowShrinking::No);
        Guards.Add(MoveTemp(O));
    }
}
bool FStudioHome4RuntimeSession::SetBackendVerification(const FStudioHome4BackendVerification& V, FString& Error)
{
    using namespace StudioHome4RuntimePrivate; Scope();
    if (!V.Id.IsValid() || !Clean(V.Target) || !Clean(V.Host) || !Clean(V.Device) || !Clean(V.Source, 2048) || !KnownBackend(V.EffectiveBackend) || !Time(V.ObservedAt))
    { Error = TEXT("Backend response needs explicit identity, target/host/device, effective backend and original source."); return false; }
    if (!V.ExtensionImported || (V.EffectiveBackend != EStudioHome4Backend::PyTorch && !*V.ExtensionImported))
    { Error = TEXT("Effective extension backend requires an explicit successful extension import response; failed import must report fallback."); return false; }
    Backend = V; FallbackAcknowledged.Invalidate(); Error.Empty(); return true;
}
bool FStudioHome4RuntimeSession::ConfirmFallback(const FGuid& Id)
{ Scope(); if (!Backend || Backend->Id != Id || Backend->EffectiveBackend != EStudioHome4Backend::PyTorch) return false; FallbackAcknowledged = Id; return true; }
bool FStudioHome4RuntimeSession::SetResourceStatus(const FStudioHome4ResourceStatus& S, FString& Error)
{
    using namespace StudioHome4RuntimePrivate; Scope();
    if (!Clean(S.Target) || !Clean(S.Host) || !Clean(S.Device) || !Clean(S.Source, 2048) || !Clean(S.Owner, 256, false) || !Clean(S.JobId, 256, false) || !Time(S.ObservedAt))
    { Error = TEXT("Resource status requires explicit target, host, device, source and valid observation time."); return false; }
    const TOptional<double> Values[] = {S.UtilizationPercent, S.MemoryUsedBytes, S.MemoryTotalBytes, S.PowerWatts, S.TemperatureC, S.ClockMHz, S.DiskFreeBytes, S.PeakGBps};
    for (int32 I = 0; I < UE_ARRAY_COUNT(Values); ++I) if (Values[I] && (!FMath::IsFinite(*Values[I]) || *Values[I] < (I == 4 ? -273.15 : 0) || (I == 0 && *Values[I] > 100)))
    { Error = TEXT("Invalid original resource measurement; previous status retained."); return false; }
    if (S.MemoryUsedBytes && S.MemoryTotalBytes && *S.MemoryUsedBytes > *S.MemoryTotalBytes)
    { Error = TEXT("Resource used memory exceeds supplied total memory."); return false; }
    for (const auto& Bytes : {S.MemoryUsedBytes, S.MemoryTotalBytes, S.DiskFreeBytes})
        if (Bytes && (*Bytes > 9007199254740991. || FMath::FloorToDouble(*Bytes) != *Bytes))
        { Error = TEXT("Resource byte counts must be exact nonnegative integers within JSON's safe integer range."); return false; }
    Resources = S; Error.Empty(); return true;
}
bool FStudioHome4RuntimeSession::ImportTargetStatus(const FString& JSON, double Now, FString& Error)
{
    using namespace StudioHome4RuntimePrivate; TSharedPtr<FJsonObject> O;
    if (!Object(JSON, O)) { Error = TEXT("Target response must be strict bounded JSON."); return false; }
    FString Schema, Id, BackendText; FStudioHome4BackendVerification B; FStudioHome4ResourceStatus R;
    if (!Text(O, TEXT("schema"), Schema) || Schema != TEXT("LBMStudio.Home4TargetStatus") || !Text(O, TEXT("verification_id"), Id) || !FGuid::Parse(Id, B.Id) ||
        !Text(O, TEXT("target"), B.Target) || !Text(O, TEXT("host"), B.Host) || !Text(O, TEXT("device"), B.Device) || !Text(O, TEXT("source"), B.Source) || !Text(O, TEXT("effective_backend"), BackendText))
    { Error = TEXT("Target response must identify verification, endpoint, effective backend and original source."); return false; }
    bool Imported = false, Development = false;
    const auto E = O->TryGetField(TEXT("extension_imported")), D = O->TryGetField(TEXT("development_response"));
    if (!E || E->Type != EJson::Boolean || !E->TryGetBool(Imported) || !D || D->Type != EJson::Boolean || !D->TryGetBool(Development))
    { Error = TEXT("Target response needs explicit boolean extension_imported and development_response."); return false; }
    B.ExtensionImported = Imported; B.bDevelopmentResponse = Development; B.ObservedAt = Now; B.EffectiveBackend = ParseBackend(BackendText);
    R.Target = B.Target; R.Host = B.Host; R.Device = B.Device; R.Source = B.Source; R.bDevelopmentResponse = Development; R.ObservedAt = Now;
    if (!Text(O, TEXT("owner"), R.Owner, false) || !Text(O, TEXT("job_id"), R.JobId, false) ||
        !Number(O, TEXT("utilization_percent"), R.UtilizationPercent, 0, 100) || !Number(O, TEXT("memory_used_bytes"), R.MemoryUsedBytes) || !Number(O, TEXT("memory_total_bytes"), R.MemoryTotalBytes) ||
        !Number(O, TEXT("power_watts"), R.PowerWatts) || !Number(O, TEXT("temperature_c"), R.TemperatureC, -273.15, 1000) || !Number(O, TEXT("clock_mhz"), R.ClockMHz) ||
        !Number(O, TEXT("disk_free_bytes"), R.DiskFreeBytes) || !Number(O, TEXT("peak_gbps"), R.PeakGBps))
    { Error = TEXT("Invalid original resource response fields."); return false; }
    // Validate both records on a temporary owner before replacing either one.
    auto PreviousB = Backend; auto PreviousR = Resources; const auto PreviousAck = FallbackAcknowledged;
    if (!SetBackendVerification(B, Error) || !SetResourceStatus(R, Error))
    { Backend = MoveTemp(PreviousB); Resources = MoveTemp(PreviousR); FallbackAcknowledged = PreviousAck; return false; }
    return true;
}
bool FStudioHome4RuntimeSession::IsForeignDeviceBusy(const FString& Owner) const
{ return Backend && Resources && Resources->Target == Backend->Target && Resources->Host==Backend->Host && Resources->Device == Backend->Device && Resources->UtilizationPercent && *Resources->UtilizationPercent > 0 && (Resources->Owner.IsEmpty() || Resources->Owner != Owner); }
FStudioHome4QueueJob* FStudioHome4RuntimeSession::FindJob(const FGuid& Id)
{ return Jobs.FindByPredicate([&](const auto& J) { return J.RunId == Id; }); }
const FStudioHome4QueueJob* FStudioHome4RuntimeSession::SelectedActiveJob()
{
    Scope();
    if (const auto* J = FindJob(SelectedRunId); J && !StudioHome4RuntimePrivate::Terminal(J->State)) return J;
    for (int32 I = Jobs.Num() - 1; I >= 0; --I)
        if (!StudioHome4RuntimePrivate::Terminal(Jobs[I].State)) { SelectedRunId = Jobs[I].RunId; return &Jobs[I]; }
    return nullptr;
}
bool FStudioHome4RuntimeSession::SelectJob(const FGuid& Id)
{ Scope(); if (!FindJob(Id)) return false; SelectedRunId = Id; return true; }
bool FStudioHome4RuntimeSession::CanCommand(const FGuid& Id, EStudioJobCommand C)
{
    Scope(); const auto* J = FindJob(Id); if (!J || StudioHome4RuntimePrivate::Terminal(J->State)) return false;
    if (C == EStudioJobCommand::Pause) return J->State == EStudioHome4QueueState::Running;
    if (C == EStudioJobCommand::Resume || C == EStudioJobCommand::Step || C == EStudioJobCommand::RunToDimensionless) return J->State == EStudioHome4QueueState::Paused;
    if (C == EStudioJobCommand::Stop) return J->State != EStudioHome4QueueState::Stopping;
    if (C == EStudioJobCommand::Reconnect) return J->State == EStudioHome4QueueState::Disconnected;
    return C == EStudioJobCommand::Checkpoint && (J->State == EStudioHome4QueueState::Running || J->State == EStudioHome4QueueState::Paused);
}
FString FStudioHome4RuntimeSession::StateName(EStudioHome4QueueState State)
{
    const TCHAR* Names[] = {TEXT("Queued"), TEXT("Preparing"), TEXT("Running"), TEXT("Paused"), TEXT("Stopping"), TEXT("Stopped"), TEXT("Completed"), TEXT("Cancelled"), TEXT("Failed"), TEXT("Disconnected")};
    return uint8(State) < UE_ARRAY_COUNT(Names) ? Names[uint8(State)] : TEXT("Unknown");
}
void FStudioHome4RuntimeSession::Transition(FStudioHome4QueueJob& J, EStudioHome4QueueState S, double Now, const FString& Notice)
{ J.State = S; J.LastTransitionAt = Now; J.Notice = Notice; ++J.Sequence; SyncHistory(J, Now); }
void FStudioHome4RuntimeSession::SyncHistory(const FStudioHome4QueueJob& J, double Now)
{
    const auto M = Model.Pin(); if (!M) return;
    auto* H = M->Project.JobHistory.FindByPredicate([&](const auto& V) { return V.RunId == J.RunId; }); if (!H) return;
    switch (J.State)
    {
        case EStudioHome4QueueState::Queued: H->LastState = EStudioJobState::Queued; break;
        case EStudioHome4QueueState::Preparing: H->LastState = EStudioJobState::Preparing; break;
        case EStudioHome4QueueState::Running: H->LastState = EStudioJobState::Running; break;
        case EStudioHome4QueueState::Paused: H->LastState = EStudioJobState::Paused; break;
        case EStudioHome4QueueState::Stopping: H->LastState = EStudioJobState::Stopping; break;
        case EStudioHome4QueueState::Stopped: case EStudioHome4QueueState::Cancelled: H->LastState = EStudioJobState::Stopped; break;
        case EStudioHome4QueueState::Completed: H->LastState = EStudioJobState::Completed; break;
        case EStudioHome4QueueState::Failed: H->LastState = EStudioJobState::Failed; break;
        case EStudioHome4QueueState::Disconnected: H->LastState = EStudioJobState::Disconnected; break;
    }
    H->Notice = J.Notice;
    H->ElapsedSeconds = FMath::Max(0., (StudioHome4RuntimePrivate::Terminal(J.State) ? J.LastTransitionAt : Now) - J.SubmittedAt);
    M->bDirty = true;
}
bool FStudioHome4RuntimeSession::Submit(const FStudioHome4Spec& Spec, const FGuid& Id, const FString& Owner, double Now, FString& Error)
{
    using namespace StudioHome4RuntimePrivate; Scope();
    if (!ProjectId.IsValid() || !CaseId.IsValid() || !Id.IsValid() || FindJob(Id) || Jobs.Num() >= 64 || !Clean(Owner) || !Time(Now) || Now < LastClock || !StudioHome4Config::Validate(Spec, Error))
    { if (Error.IsEmpty()) Error = TEXT("Queue submission requires this scope, a unique run, owner, valid frozen spec and monotonic clock (maximum 64 jobs)."); return false; }
    if (!Backend || Backend->EffectiveBackend == EStudioHome4Backend::Unknown)
    { Error = TEXT("Review an explicit backend/target verification response before queueing."); return false; }
    if (!Spec.Run.Device.IsEmpty() && Spec.Run.Device != Backend->Device)
    { Error = TEXT("Requested device does not match reviewed backend response."); return false; }
    if (!Spec.Run.QueueTarget.IsEmpty() && Spec.Run.QueueTarget != Backend->Target)
    { Error = TEXT("Requested queue target does not match reviewed backend response."); return false; }
    if (Backend->EffectiveBackend == EStudioHome4Backend::PyTorch && FallbackAcknowledged != Backend->Id)
    { Error = TEXT("Effective PyTorch fallback requires explicit confirmation of this verification before queue submission."); return false; }
    TSharedPtr<const FStudioHome4PreparedCheckpoint, ESPMode::ThreadSafe> PreparedCheckpoint;
    if (!Spec.Run.InitState.IsEmpty())
    {
        if (!Checkpoints)
        { Error = TEXT("Inspect the identified original restart checkpoint for this exact frozen request before queueing."); return false; }
        if (!Checkpoints->ValidateForSubmit(Spec, PreparedCheckpoint, Error)) return false;
        if (!PreparedCheckpoint)
        { Error = TEXT("Checkpoint inspection did not retain a prepared original byte lease; request was not queued."); return false; }
    }
    const auto M = Model.Pin();
    if (!M || M->Project.Runs.Num() >= 256 || M->Project.JobHistory.Num() >= 256)
    { Error = TEXT("Project run history is unavailable or full; create a new project before submitting another request."); return false; }
    FStudioCaseDraft Frozen = M->Project.Draft; Frozen.Home4 = Spec;
    const FString RequestedBackend = StudioHome4RuntimePrivate::BackendName(Spec.Run.Backend);
    Frozen.Setup.BackendId = RequestedBackend;
    auto RecordJSON = FStudioRunRecord::Capture(M->Project.Name.Left(64) + TEXT(" · HOME4 development ") + Id.ToString(EGuidFormats::Short), Frozen, EStudioRunOrigin::ControlHarness).ToJSON();
    RecordJSON->SetStringField(TEXT("id"), Id.ToString());
    FStudioRunRecord Record;
    if (!FStudioRunRecord::FromJSON(RecordJSON, Record, Error)) return false;
    auto Candidate = M->SnapshotProject(); Candidate.Runs.Add(Record); FStudioJobHistory History; History.RunId = Id; Candidate.JobHistory.Add(History);
    if (StudioProjectIO::Serialize(Candidate).Len() > 4 * 1024 * 1024 - 8192)
    { Error = TEXT("Project run history reached its document budget; create a new project before submitting."); return false; }
    FStudioHome4QueueJob J; J.RunId = Id; J.ProjectId = ProjectId; J.CaseId = CaseId; J.FrozenSpec = Spec; J.Owner = Owner;
    J.PreparedCheckpoint = MoveTemp(PreparedCheckpoint);
    J.BackendVerificationId = Backend->Id; J.EffectiveBackend = Backend->EffectiveBackend; J.Target = Backend->Target; J.Host = Backend->Host; J.Device = Backend->Device;
    J.SubmittedAt = J.LastTransitionAt = Now; J.Notice = TEXT("Immutable development launch queued; original science result remains pending."); J.Sequence = 1;
    M->Project.Runs.Add(Record); M->Project.JobHistory.Add(History); ++M->CatalogRevision; M->bDirty = true;
    Jobs.Add(MoveTemp(J)); SelectedRunId = Id; SyncHistory(Jobs.Last(), Now); Error.Empty(); return true;
}
bool FStudioHome4RuntimeSession::Command(const FGuid& Id, EStudioJobCommand C, double Now, FString& Error, int64 Count, TOptional<double> Target)
{
    using namespace StudioHome4RuntimePrivate; Scope(); auto* J = FindJob(Id);
    if (!J || !Time(Now) || Now < LastClock || Terminal(J->State)) { Error = TEXT("Command has no active owned run or monotonic clock."); return false; }
    if (C == EStudioJobCommand::Pause && J->State == EStudioHome4QueueState::Running) Transition(*J, EStudioHome4QueueState::Paused, Now, TEXT("Development pause acknowledged."));
    else if (C == EStudioJobCommand::Resume && J->State == EStudioHome4QueueState::Paused) Transition(*J, EStudioHome4QueueState::Running, Now, TEXT("Development resume acknowledged."));
    else if (C == EStudioJobCommand::Stop && J->State != EStudioHome4QueueState::Stopping)
        Transition(*J, EStudioHome4QueueState::Stopping, Now, TEXT("Development stop request accepted; acknowledgment pending."));
    else if (C == EStudioJobCommand::Reconnect && J->State == EStudioHome4QueueState::Disconnected)
        Transition(*J, EStudioHome4QueueState::Paused, Now, TEXT("Development reconnect query acknowledged paused; original science must reconnect separately."));
    else if ((C == EStudioJobCommand::Step || C == EStudioJobCommand::RunToDimensionless) && J->State == EStudioHome4QueueState::Paused)
    {
        if (C == EStudioJobCommand::RunToDimensionless)
        {
            const auto V = Target ? StudioHome4Config::ConvertUnits(*Target, EStudioHome4Quantity::Time, EStudioHome4UnitDisplay::Nondimensional, EStudioHome4UnitDisplay::Lattice, J->FrozenSpec) : TOptional<double>();
            if (!V || *V <= double(J->AcknowledgedControlSteps) || *V > 1.e12) { Error = TEXT("Run-to requires explicit later t* and a known frozen reference time."); return false; }
            const double Rounded = FMath::RoundToDouble(*V);
            const double Reach = FMath::IsNearlyEqual(*V, Rounded, 1.e-12 * FMath::Max(1., FMath::Abs(*V))) ? Rounded : FMath::CeilToDouble(*V);
            Count = int64(Reach) - J->AcknowledgedControlSteps;
        }
        const int64 Limit = J->FrozenSpec.Run.Steps.Get(1000000000000LL);
        if (Count <= 0 || Count > Limit - J->AcknowledgedControlSteps) { Error = TEXT("Development range exceeds the frozen run's positive bounded step budget."); return false; }
        J->AcknowledgedControlSteps += Count; Transition(*J, J->State, Now, TEXT("Development control range acknowledged; this counter is not a numerical solver step measurement."));
    }
    else if (C == EStudioJobCommand::Checkpoint && (J->State == EStudioHome4QueueState::Running || J->State == EStudioHome4QueueState::Paused))
        Transition(*J, J->State, Now, TEXT("Development checkpoint request acknowledged; no restart state or file generated."));
    else { Error = TEXT("Command is unavailable in this owned queue state."); return false; }
    Error.Empty(); return true;
}
bool FStudioHome4RuntimeSession::CompleteDevelopment(const FGuid& Id, double Now, FString& Error)
{
    Scope(); auto* J = FindJob(Id);
    if (!J || (J->State != EStudioHome4QueueState::Running && J->State != EStudioHome4QueueState::Paused) || !StudioHome4RuntimePrivate::Time(Now) || Now < LastClock)
    { Error = TEXT("Finish requires an active development run."); return false; }
    Transition(*J, EStudioHome4QueueState::Completed, Now, TEXT("Development protocol completed. Original scientific result still pending; no CFD gate or throughput claimed.")); Error.Empty(); return true;
}
bool FStudioHome4RuntimeSession::DisconnectDevelopment(const FGuid& Id, double Now, FString& Error)
{
    Scope(); auto* J = FindJob(Id);
    if (!J || StudioHome4RuntimePrivate::Terminal(J->State) || J->State == EStudioHome4QueueState::Stopping || !StudioHome4RuntimePrivate::Time(Now) || Now < LastClock)
    { Error = TEXT("Disconnect requires an active owned development protocol."); return false; }
    Transition(*J, EStudioHome4QueueState::Disconnected, Now, TEXT("Explicit development transport disconnect; original science source remains independent.")); Error.Empty(); return true;
}
bool FStudioHome4RuntimeSession::FailDevelopment(const FGuid& Id, double Now, const FString& Reason, FString& Error)
{
    Scope(); auto* J = FindJob(Id);
    if (!J || StudioHome4RuntimePrivate::Terminal(J->State) || !StudioHome4RuntimePrivate::Clean(Reason, 2048) || !StudioHome4RuntimePrivate::Time(Now) || Now < LastClock)
    { Error = TEXT("Failure outcome requires an active owned protocol and explicit bounded reason."); return false; }
    Transition(*J, EStudioHome4QueueState::Failed, Now, TEXT("Explicit development failure: ") + Reason); Error.Empty(); return true;
}
bool FStudioHome4RuntimeSession::Cancel(const FGuid& Id, double Now, FString& Error)
{
    Scope(); auto* J = FindJob(Id); if (!J || StudioHome4RuntimePrivate::Terminal(J->State) || !StudioHome4RuntimePrivate::Time(Now) || Now < LastClock)
    { Error = TEXT("Cancel requires an active owned development run."); return false; }
    Transition(*J, EStudioHome4QueueState::Cancelled, Now, TEXT("Owned development request cancelled; no scientific completion implied.")); Error.Empty(); return true;
}
bool FStudioHome4RuntimeSession::AttachOriginalResult(const FGuid& Id, FString& Error)
{
    Scope(); auto* J = FindJob(Id); if (!J) { Error = TEXT("Original result run identity is not in this project's queue."); return false; }
    J->bScientificResultsAttached = true; J->Notice += TEXT(" Identified original result evidence attached independently."); ++J->Sequence; Error.Empty(); return true;
}
bool FStudioHome4RuntimeSession::AttachOriginalEvidence(const FStudioHome4ReferenceEvidence& E,FString& Error)
{
    Scope();const auto M=Model.Pin();auto* Job=FindJob(E.RunId);
    if(!M||!Job||E.AttachedProjectId!=ProjectId||E.AttachedCaseId!=CaseId||!E.OriginalRunSpec||
        StudioHome4Config::Serialize(*E.OriginalRunSpec)!=StudioHome4Config::Serialize(Job->FrozenSpec)||!StudioHome4Validation::VerifyOriginalBytes(E,Error))
    {if(Error.IsEmpty())Error=TEXT("Original evidence must match this scope, immutable queued specification and original source bytes.");return false;}
    auto* Record=M->Project.Runs.FindByPredicate([&](const auto& R){return R.GetId()==E.RunId;});
    if(!Record){Error=TEXT("Original run history is absent; no unrelated card was relabeled.");return false;}
    FStudioRecordedRunProvenance P=Record->GetProvenance().Get(FStudioRecordedRunProvenance());
    P.RunId=E.RunId.ToString();P.RecipeId=E.RecipeId;P.LineageId=E.OriginalRunSpec->LineageId;
    P.OriginalRunSpec=E.OriginalRunSpec;P.OriginalTag=E.OriginalRunSpec->Run.Tag;
    P.MeasurementSource=E.ActualSource;P.MeasurementSHA256=E.bComposedAlignment?E.ActualOriginalSHA256:E.SourceSHA256;
    P.GateStatus=E.RecipeGateStatus();P.GateSourceSHA256=E.SourceSHA256;
    P.ParentRunId=E.OriginalRunSpec->ParentRunId;P.ParentSpecSHA256=E.OriginalRunSpec->ParentSpecSHA256;
    const auto Updated=Record->WithProvenance(P);FStudioRunRecord Verified;
    if(!FStudioRunRecord::FromJSON(Updated.ToJSON(),Verified,Error))return false;
    auto Candidate=M->SnapshotProject();const int32 Index=Candidate.Runs.IndexOfByPredicate([&](const auto& R){return R.GetId()==E.RunId;});Candidate.Runs[Index]=Verified;
    const FTCHARToUTF8 ProjectBytes(*StudioProjectIO::Serialize(Candidate));
    if(ProjectBytes.Length()>4*1024*1024-8192){Error=TEXT("Original evidence provenance exceeds this project's bounded document budget; previous history retained.");return false;}
    *Record=MoveTemp(Verified);Job->bScientificResultsAttached=true;++Job->Sequence;++M->CatalogRevision;M->bDirty=true;Error.Empty();return true;
}
void FStudioHome4RuntimeSession::AttachMeasuredProvenance(const FStudioHome4MeasuredRun& R)
{
    Scope();const auto M=Model.Pin();if(!M||!R.OriginalRunSpec)return;
    auto* Record=M->Project.Runs.FindByPredicate([&](const auto& V){return V.GetId()==R.RunId;});if(!Record||Record->GetOrigin()==EStudioRunOrigin::PublishedRecording)return;
    const auto* Configuration=Record->GetConfiguration();const auto Existing=Record->GetProvenance();
    const FStudioHome4Spec* Original=Configuration&&Configuration->Home4?&*Configuration->Home4:Existing&&Existing->OriginalRunSpec?&*Existing->OriginalRunSpec:nullptr;
    if(!Original||StudioHome4Config::Serialize(*Original)!=StudioHome4Config::Serialize(*R.OriginalRunSpec))return;
    FStudioRecordedRunProvenance P=Existing.Get(FStudioRecordedRunProvenance());
    P.RunId=R.RunId.ToString();P.RecipeId=R.RecipeId;P.LineageId=R.OriginalRunSpec->LineageId;P.OriginalRunSpec=R.OriginalRunSpec;
    P.OriginalTag=R.OriginalRunSpec->Run.Tag;P.OriginalBackend=StudioHome4RuntimePrivate::BackendName(R.Backend);
    P.MeasuredMLUPS=R.MLUPS();P.MeasurementSource=R.Source;P.MeasurementSHA256=R.SourceSHA256;
    P.ParentRunId=R.OriginalRunSpec->ParentRunId;P.ParentSpecSHA256=R.OriginalRunSpec->ParentSpecSHA256;
    FStudioRunRecord Verified;FString Error;
    if(!FStudioRunRecord::FromJSON(Record->WithProvenance(P).ToJSON(),Verified,Error))return;
    auto Candidate=M->SnapshotProject();const int32 Index=Candidate.Runs.IndexOfByPredicate([&](const auto& V){return V.GetId()==R.RunId;});Candidate.Runs[Index]=Verified;
    const FTCHARToUTF8 ProjectBytes(*StudioProjectIO::Serialize(Candidate));
    if(ProjectBytes.Length()>4*1024*1024-8192)return;
    *Record=MoveTemp(Verified);++M->CatalogRevision;M->bDirty=true;
}
bool FStudioHome4RuntimeSession::RecordMeasuredRun(const FStudioHome4MeasuredRun& R, FString& Error)
{
    using namespace StudioHome4RuntimePrivate;
    if (!R.RunId.IsValid() || !R.bCompletedOriginalRun || !Clean(R.Host) || !Clean(R.Device) || !Clean(R.RecipeId) || !Clean(R.Source, 2048) || !SHA(R.SourceSHA256) || !Clean(R.RetabulationPolicy, 256, false) ||
        !KnownBackend(R.Backend) || !FMath::IsFinite(R.NodeUpdates) || R.NodeUpdates <= 0 || !FMath::IsFinite(R.ElapsedSeconds) || R.ElapsedSeconds <= 0 || !FMath::IsFinite(R.MLUPS()) ||
        (R.TransferredBytes && (!FMath::IsFinite(*R.TransferredBytes) || *R.TransferredBytes < 0)))
    { Error = TEXT("Measured history needs an identified completed original run, machine/workload source hash and finite actual work/time."); return false; }
    if(R.OriginalRunSpec)
    {
        if(!StudioHome4Config::Validate(*R.OriginalRunSpec,Error)||R.OriginalRunSpec->RecipeId!=R.RecipeId||R.OriginalRunSpec->Geometry.RetabulationPolicy!=R.RetabulationPolicy)
        {Error=TEXT("Measured history original specification/recipe/retabulation policy does not match its attributed workload.");return false;}
    }
    if (const auto* Existing = MeasuredRuns.FindByPredicate([&](const auto& V) { return V.RunId == R.RunId; }))
    { Error = Existing->SourceSHA256 == R.SourceSHA256 ? TEXT("Original measured run is already recorded.") : TEXT("Original run ID conflicts with a different measurement source hash."); return false; }
    auto Previous=MeasuredRuns;
    if (MeasuredRuns.Num() >= 128) MeasuredRuns.RemoveAt(0, 1, EAllowShrinking::No);
    MeasuredRuns.Add(R);
    const FTCHARToUTF8 Document(*SerializePerformanceHistory());
    if(!bParsingHistory&&Document.Length()>1024*1024){MeasuredRuns=MoveTemp(Previous);Error=TEXT("Original measured history exceeds its 1 MiB persisted source budget; prior history retained.");return false;}
    ++HistoryRevision;if(!bParsingHistory)AttachMeasuredProvenance(R);Error.Empty();return true;
}
FString FStudioHome4RuntimeSession::SerializePerformanceHistory() const
{
    using namespace StudioHome4RuntimePrivate; auto Root = MakeShared<FJsonObject>(); Root->SetStringField(TEXT("schema"), TEXT("LBMStudio.Home4MeasuredHistory")); TArray<TSharedPtr<FJsonValue>> Records;
    for (const auto& R : MeasuredRuns)
    {
        auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("run_id"), R.RunId.ToString()); O->SetStringField(TEXT("host"), R.Host); O->SetStringField(TEXT("device"), R.Device);
        O->SetStringField(TEXT("recipe_id"), R.RecipeId); O->SetStringField(TEXT("source"), R.Source); O->SetStringField(TEXT("source_sha256"), R.SourceSHA256); O->SetStringField(TEXT("retabulation_policy"), R.RetabulationPolicy);
        O->SetStringField(TEXT("backend"), BackendName(R.Backend)); O->SetNumberField(TEXT("node_updates"), R.NodeUpdates); O->SetNumberField(TEXT("elapsed_seconds"), R.ElapsedSeconds); O->SetBoolField(TEXT("completed_original_run"), true);
        if (R.TransferredBytes) O->SetNumberField(TEXT("transferred_bytes"), *R.TransferredBytes);
        if(R.OriginalRunSpec)O->SetObjectField(TEXT("original_run_spec"),StudioHome4Config::ToJSON(*R.OriginalRunSpec));
        Records.Add(MakeShared<FJsonValueObject>(O));
    }
    Root->SetArrayField(TEXT("records"), Records); return JSON(Root);
}
bool FStudioHome4RuntimeSession::ParsePerformanceHistory(const FString& JSONText, FString& Error)
{
    using namespace StudioHome4RuntimePrivate; TSharedPtr<FJsonObject> O; FString Schema;
    if (!Object(JSONText, O) || !Text(O, TEXT("schema"), Schema) || Schema != TEXT("LBMStudio.Home4MeasuredHistory")) { Error = TEXT("Supply original HOME4 measured-history JSON."); return false; }
    const auto A = O->TryGetField(TEXT("records")); if (!A || A->Type != EJson::Array || A->AsArray().Num() > 128) { Error = TEXT("Measured history supports at most 128 original runs."); return false; }
    TGuardValue<bool> ParsingGuard(bParsingHistory,true);const uint64 PreviousRevision=HistoryRevision;
    TArray<FStudioHome4MeasuredRun> Previous = MeasuredRuns; MeasuredRuns.Reset();
    for (const auto& V : A->AsArray())
    {
        if (V->Type != EJson::Object) { Error = TEXT("Measured history records must be objects."); MeasuredRuns = MoveTemp(Previous);HistoryRevision=PreviousRevision; return false; }
        const auto R = V->AsObject(); FStudioHome4MeasuredRun M; FString Id, B; TOptional<double> Updates, Seconds; bool Completed = false;
        const auto Flag = R->TryGetField(TEXT("completed_original_run"));
        if (!Text(R, TEXT("run_id"), Id) || !FGuid::Parse(Id, M.RunId) || !Text(R, TEXT("host"), M.Host) || !Text(R, TEXT("device"), M.Device) || !Text(R, TEXT("recipe_id"), M.RecipeId) ||
            !Text(R, TEXT("source"), M.Source) || !Text(R, TEXT("source_sha256"), M.SourceSHA256) || !Text(R, TEXT("retabulation_policy"), M.RetabulationPolicy, false) || !Text(R, TEXT("backend"), B) ||
            !Number(R, TEXT("node_updates"), Updates) || !Number(R, TEXT("elapsed_seconds"), Seconds) || !Number(R, TEXT("transferred_bytes"), M.TransferredBytes) || !Updates || !Seconds || !Flag || Flag->Type != EJson::Boolean || !Flag->TryGetBool(Completed))
        { Error = TEXT("Original measured history record is incomplete or malformed."); MeasuredRuns = MoveTemp(Previous);HistoryRevision=PreviousRevision; return false; }
        const auto Original=R->TryGetField(TEXT("original_run_spec"));
        if(Original&&Original->Type!=EJson::Null)
        {
            FStudioHome4Spec Spec;
            if(Original->Type!=EJson::Object||!StudioHome4Config::FromJSON(Original->AsObject(),Spec,Error)||Spec.RecipeId!=M.RecipeId)
            {Error=TEXT("Measured history original run specification is invalid or belongs to another workload.");MeasuredRuns=MoveTemp(Previous);HistoryRevision=PreviousRevision;return false;}
            M.OriginalRunSpec=MoveTemp(Spec);
        }
        M.NodeUpdates = *Updates; M.ElapsedSeconds = *Seconds; M.Backend = ParseBackend(B); M.bCompletedOriginalRun = Completed;
        if (!RecordMeasuredRun(M, Error)) { MeasuredRuns = MoveTemp(Previous);HistoryRevision=PreviousRevision; return false; }
    }
    for(const auto& R:MeasuredRuns)AttachMeasuredProvenance(R);
    HistoryRevision=PreviousRevision+1;Error.Empty(); return true;
}
TOptional<double> FStudioHome4RuntimeSession::EstimatedSeconds(const FStudioHome4Spec& Spec, const FStudioHome4MeasuredRun& Basis) const
{
    if(!Basis.OriginalRunSpec||Spec.Geometry.BodyMotion.IsEmpty()||Spec.Geometry.RetabulationPolicy.IsEmpty()||
        Basis.OriginalRunSpec->Geometry.BodyMotion!=Spec.Geometry.BodyMotion||Basis.OriginalRunSpec->Geometry.RetabulationPolicy!=Spec.Geometry.RetabulationPolicy||
        Basis.OriginalRunSpec->Geometry.RetabulateEvery!=Spec.Geometry.RetabulateEvery||Basis.OriginalRunSpec->Multidomain.Levels!=Spec.Multidomain.Levels||
        Basis.OriginalRunSpec->Authoring.Patches.Num()!=Spec.Authoring.Patches.Num()||Basis.OriginalRunSpec->Multidomain.SneqMode!=Spec.Multidomain.SneqMode)return {};
    if (!Backend || !Basis.bCompletedOriginalRun || Basis.RecipeId != Spec.RecipeId || Basis.MLUPS() <= 0 ||
        Basis.Host != Backend->Host || Basis.Device != Backend->Device || Basis.Backend != Backend->EffectiveBackend ||
        Basis.RetabulationPolicy != Spec.Geometry.RetabulationPolicy) return {};
    FStudioHome4Spec Estimate = Spec; Estimate.Performance.MeasuredMLUPS = Basis.MLUPS(); Estimate.Performance.MeasurementSource = Basis.Source; return StudioHome4Config::Derive(Estimate).EstimatedSeconds;
}
TArray<FStudioHome4OutputLayout> FStudioHome4RuntimeSession::ConfiguredOutputLayouts(const FStudioHome4Spec& Spec)
{
    TArray<FStudioHome4OutputLayout> Layouts;const auto& P=Spec.Performance;
    for(int32 I=0;I<4;++I)
    {
        FStudioHome4OutputLayout L;L.Kind=EStudioHome4OutputKind(I);
        if(P.OutputByteEstimates.Num()==4&&P.OutputByteEstimates[I]>0&&FMath::IsFinite(P.OutputByteEstimates[I])&&P.OutputByteEstimates[I]<=1.e12&&
            !P.OutputEstimateSource.TrimStartAndEnd().IsEmpty()&&!P.OutputEstimateAssumption.TrimStartAndEnd().IsEmpty())
        {
            L.BytesPerOutput=uint64(FMath::CeilToDouble(P.OutputByteEstimates[I]));L.Source=P.OutputEstimateSource;
            L.Assumption=P.OutputEstimateAssumption+TEXT("; fractional byte estimates conservatively rounded up");
        }
        Layouts.Add(MoveTemp(L));
    }
    return Layouts;
}
FStudioHome4DiskForecast FStudioHome4RuntimeSession::Forecast(const FStudioHome4Spec& Spec, const TArray<FStudioHome4OutputLayout>& Layouts, TOptional<uint64> Free)
{
    FStudioHome4DiskForecast F; const TOptional<int64> Intervals[] = {Spec.Run.MeasureEvery, Spec.Run.SaveEvery, Spec.Run.VizEvery, Spec.Run.RestartEvery}; bool Known = Spec.Run.Steps.IsSet(); uint64 Total = 0;
    for (int32 I = 0; I < 4; ++I)
    {
        if (!Spec.Run.Steps || *Spec.Run.Steps <= 0 || !Intervals[I] || *Intervals[I] <= 0) { F.Reasons[I] = TEXT("Step budget or independent cadence unspecified or invalid."); Known = false; continue; }
        F.Counts[I] = uint64(*Spec.Run.Steps / *Intervals[I]);
        const auto* L = Layouts.FindByPredicate([I](const auto& V) { return int32(V.Kind) == I; });
        if (!L || !L->BytesPerOutput || L->Source.TrimStartAndEnd().IsEmpty()) { F.Reasons[I] = TEXT("Output shape/dtype/member size or attributed user estimate unknown."); Known = false; continue; }
        if (*L->BytesPerOutput > 0 && *F.Counts[I] > MAX_uint64 / *L->BytesPerOutput) { F.Reasons[I] = TEXT("Forecast exceeds integer budget."); Known = false; continue; }
        F.Bytes[I] = *F.Counts[I] * *L->BytesPerOutput; F.Reasons[I] = L->Source + TEXT(" · ") + L->Assumption;
        if (Total > MAX_uint64 - *F.Bytes[I]) { Known = false; F.Bytes[I].Reset(); } else Total += *F.Bytes[I];
    }
    if (Known) { F.TotalBytes = Total; F.bExceedsSuppliedFreeSpace = Free && Total > *Free; } return F;
}
