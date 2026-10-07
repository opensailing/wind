#pragma once
#include "StudioHome4Telemetry.h"
#include "StudioJobs.h"
#include "Async/Future.h"
#include <atomic>

class FStudioModel;
struct FStudioHome4ReferenceEvidence;
class FStudioHome4CheckpointSession;
class FStudioHome4PreparedCheckpoint;
enum class EStudioHome4QueueState : uint8 { Queued, Preparing, Running, Paused, Stopping, Stopped, Completed, Cancelled, Failed, Disconnected };
struct FStudioHome4BackendVerification
{
    FGuid Id;
    FString Target, Host, Device, Source;
    EStudioHome4Backend EffectiveBackend = EStudioHome4Backend::Unknown;
    TOptional<bool> ExtensionImported;
    bool bDevelopmentResponse = false;
    double ObservedAt = 0;
};
struct FStudioHome4ResourceStatus
{
    FString Target, Host, Device, Owner, JobId, Source;
    TOptional<double> UtilizationPercent, MemoryUsedBytes, MemoryTotalBytes, PowerWatts, TemperatureC, ClockMHz, DiskFreeBytes, PeakGBps;
    double ObservedAt = 0;
    bool bDevelopmentResponse = false;
};
struct FStudioHome4QueueJob
{
    FGuid RunId, ProjectId, CaseId;
    FGuid BackendVerificationId;
    EStudioHome4Backend EffectiveBackend = EStudioHome4Backend::Unknown;
    FStudioHome4Spec FrozenSpec;
    /** Frontend attestation only; the eventual numerical adapter must verify and
     * load PreparedPath on its worker. FrozenSpec retains the requested source. */
    TSharedPtr<const FStudioHome4PreparedCheckpoint, ESPMode::ThreadSafe> PreparedCheckpoint;
    FString Target, Host, Device, Owner, Notice;
    EStudioHome4QueueState State = EStudioHome4QueueState::Queued;
    double SubmittedAt = 0, StartedAt = 0, LastTransitionAt = 0;
    int64 AcknowledgedControlSteps = 0;
    uint64 Sequence = 0;
    bool bDevelopmentProtocol = true, bScientificResultsAttached = false;
};
struct FStudioHome4MeasuredRun
{
    FGuid RunId;
    FString Host, Device, RecipeId, Source, SourceSHA256, RetabulationPolicy;
    EStudioHome4Backend Backend = EStudioHome4Backend::Unknown;
    double NodeUpdates = 0, ElapsedSeconds = 0;
    TOptional<double> TransferredBytes;
    bool bCompletedOriginalRun = false;
    TOptional<FStudioHome4Spec> OriginalRunSpec;
    double MLUPS() const { return ElapsedSeconds > 0 ? NodeUpdates / ElapsedSeconds / 1.e6 : 0; }
};
struct FStudioHome4OutputLayout
{
    EStudioHome4OutputKind Kind = EStudioHome4OutputKind::Trace;
    TOptional<uint64> BytesPerOutput;
    FString Source, Assumption;
};
struct FStudioHome4DiskForecast
{
    TOptional<uint64> Counts[4], Bytes[4], TotalBytes;
    FString Reasons[4];
    bool bExceedsSuppliedFreeSpace = false;
};
struct FStudioHome4GuardOutcome
{
    FStudioHome4ActionRequest Request;
    FString Recovery, Stop, Locate;
    bool bStopAcknowledged = false, bRecoveryAcknowledged = false;
};

/** Shared owner-thread frontend runtime. The development queue acknowledges
 * control requests only; original JSONL/file/status/measurement inputs are separate.
 * Call Tick once per owning workspace. Getters clear stale project/case scope. */
class FStudioHome4RuntimeSession
{
public:
    explicit FStudioHome4RuntimeSession(TSharedPtr<FStudioModel> Model);
    ~FStudioHome4RuntimeSession();
    void Tick(double Now);
    void Scope();
    bool AttachLiveLog(const FString& Path, const FGuid& OriginalRun, FString& Error);
    void DetachLiveLog();
    TSharedPtr<FStudioHome4TelemetryStream> ScienceStream() { Scope(); return Stream; }
    TOptional<FStudioHome4TelemetryProvenance> ScienceProvenance();
    const TArray<uint8>& CapturedScienceBytes() { Scope(); return OriginalBytes; }
    FString LiveStatus() const { return TailStatus; }
    bool IsTailing() const { return !TailPath.IsEmpty(); }
    bool IsScienceStale(double Now) const { return LastScienceAt < 0 || Now - LastScienceAt > 5; }
    void SetDiagnosticPolicy(const FStudioHome4DiagnosticPolicy& Policy);
    void SetLocate(TFunction<void(const FStudioHome4ActionRequest&)> Callback) { Locate = MoveTemp(Callback); }
    const TArray<FStudioHome4GuardOutcome>& GuardOutcomes() { Scope(); return Guards; }
    const TArray<FString>& LogLines() { Scope(); return Lines; }

    bool SetBackendVerification(const FStudioHome4BackendVerification& Verification, FString& Error);
    bool ConfirmFallback(const FGuid& VerificationId);
    const TOptional<FStudioHome4BackendVerification>& BackendVerification() { Scope(); return Backend; }
    const TOptional<FStudioHome4ResourceStatus>& ResourceStatus() { Scope(); return Resources; }
    bool ImportTargetStatus(const FString& JSON, double Now, FString& Error);
    bool SetResourceStatus(const FStudioHome4ResourceStatus& Status, FString& Error);
    bool IsForeignDeviceBusy(const FString& Owner) const;
    void SetCheckpointSession(TSharedPtr<FStudioHome4CheckpointSession> Session) { Checkpoints = MoveTemp(Session); }
    TSharedPtr<FStudioHome4CheckpointSession> Checkpoint() const { return Checkpoints; }

    bool Submit(const FStudioHome4Spec& Spec, const FGuid& RunId, const FString& Owner, double Now, FString& Error);
    bool Command(const FGuid& RunId, EStudioJobCommand Command, double Now, FString& Error, int64 StepCount = 1, TOptional<double> TargetTime = {});
    bool CompleteDevelopment(const FGuid& RunId, double Now, FString& Error);
    bool DisconnectDevelopment(const FGuid& RunId, double Now, FString& Error);
    bool FailDevelopment(const FGuid& RunId, double Now, const FString& Reason, FString& Error);
    bool Cancel(const FGuid& RunId, double Now, FString& Error);
    const TArray<FStudioHome4QueueJob>& QueueJobs() { Scope(); return Jobs; }
    const FStudioHome4QueueJob* SelectedActiveJob();
    bool SelectJob(const FGuid& RunId);
    bool CanCommand(const FGuid& RunId, EStudioJobCommand Command);
    static FString StateName(EStudioHome4QueueState State);
    bool AttachOriginalResult(const FGuid& RunId, FString& Error);
    bool AttachOriginalEvidence(const FStudioHome4ReferenceEvidence& Evidence,FString& Error);

    bool RecordMeasuredRun(const FStudioHome4MeasuredRun& Run, FString& Error);
    const TArray<FStudioHome4MeasuredRun>& PerformanceRecords() const { return MeasuredRuns; }
    uint64 PerformanceRevision() const { return HistoryRevision; }
    FString SerializePerformanceHistory() const;
    bool ParsePerformanceHistory(const FString& JSON, FString& Error);
    TOptional<double> EstimatedSeconds(const FStudioHome4Spec& Spec, const FStudioHome4MeasuredRun& Basis) const;
    static TArray<FStudioHome4OutputLayout> ConfiguredOutputLayouts(const FStudioHome4Spec& Spec);
    static FStudioHome4DiskForecast Forecast(const FStudioHome4Spec& Spec, const TArray<FStudioHome4OutputLayout>& Layouts, TOptional<uint64> FreeBytes = {});
private:
    struct FTailRead
    {
        TArray<uint8> Bytes, Anchor;
        FString Error;
        int64 Offset = 0, Size = 0;
        uint64 Generation = 0;
        bool bReplaced = false;
    };
    static FTailRead ReadTail(const FString& Path, int64 Offset, const TArray<uint8>& Anchor, uint64 Generation, int64 HistoricalEnd, bool HistoricalFragment,
        const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& Cancel);
    void PollTail(double Now);
    void DispatchGuards(double Now);
    FStudioHome4QueueJob* FindJob(const FGuid& RunId);
    void Transition(FStudioHome4QueueJob& Job, EStudioHome4QueueState State, double Now, const FString& Notice);
    void SyncHistory(const FStudioHome4QueueJob& Job, double Now);
    TWeakPtr<FStudioModel> Model;
    FGuid ProjectId, CaseId, FallbackAcknowledged, SelectedRunId;
    TSharedPtr<FStudioHome4TelemetryStream> Stream;
    FStudioHome4DiagnosticPolicy Policy;
    FString TailPath, TailStatus;
    FGuid TailRun;
    int64 TailOffset = 0, HistoricalPrefixEnd = 0;
    bool bTailBlocked = false, bHistoricalFragment = false, bCaptureComplete = true;
    uint64 Generation = 0, LastGuardRecord = 0;
    double LastClock = -1, NextReadAt = 0, LastScienceAt = -1;
    TArray<uint8> Anchor, OriginalBytes, LogPending;
    TArray<FString> Lines;
    TFuture<FTailRead> Pending;
    TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Cancellation;
    TArray<FStudioHome4QueueJob> Jobs;
    TArray<FStudioHome4GuardOutcome> Guards;
    TOptional<FStudioHome4BackendVerification> Backend;
    TOptional<FStudioHome4ResourceStatus> Resources;
    TArray<FStudioHome4MeasuredRun> MeasuredRuns;
    uint64 HistoryRevision=0;
    TSharedPtr<FStudioHome4CheckpointSession> Checkpoints;
    TFunction<void(const FStudioHome4ActionRequest&)> Locate;
};
