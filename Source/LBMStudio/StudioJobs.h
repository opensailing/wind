#pragma once
#include "CoreMinimal.h"
#include "StudioCase.h"
#include "StudioJobTelemetry.h"

// Job control has no access to recorded fields, camera state or playback time.
enum class EStudioJobState : uint8
{ Idle, Validating, Preparing, Queued, Running, Pausing, Paused, Stopping, Completed, Stopped, Failed, Disconnected };
enum class EStudioJobCommand : uint8 { Submit, Pause, Resume, Step, Stop, Checkpoint, Reconnect, RunToDimensionless };
enum class EStudioJobEventKind : uint8 { Accepted, State, Rejected, StepCompleted, CheckpointCompleted };

namespace StudioJobs
{
    FString StateName(EStudioJobState State);
    bool ParseState(const FString& Name,EStudioJobState& Out);
}

/** Last observed control state, never proof that a saved session is connected.
 * Counts describe acknowledged commands, not scientific steps or restart files. */
struct FStudioJobHistory
{
    FGuid RunId;
    EStudioJobState LastState = EStudioJobState::Idle;
    double ElapsedSeconds = 0;
    uint64 StepCommands = 0;
    uint64 CheckpointCommands = 0;
    FString Notice;
};

struct FStudioJobCapabilities
{
    FString BackendId;
    bool bControlHarness = false;
    bool bPause = false, bStep = false, bCheckpoint = false, bReconnect = false;
    bool bTelemetry = false;
    bool bStepN = false, bRunToDimensionless = false, bOutputSchedule = false;
    double AcknowledgementTimeout = 5., CompletionTimeout = 30.;
    double TelemetryStaleSeconds = 5.;
};
struct FStudioJobRequest
{
    FGuid RunId;
    uint64 CommandId = 0;
    EStudioJobCommand Command = EStudioJobCommand::Submit;
    // A deep snapshot for Submit only. The adapter receives its own value copy.
    TOptional<FStudioCaseDraft> Configuration;
    int64 StepCount = 1; // Requested development control steps, never a solver measurement.
    TOptional<int64> SimulatedTargetStep;
    TOptional<double> RequestedDimensionlessTime;
};
struct FStudioJobEvent
{
    FGuid RunId;
    uint64 Sequence = 0, CommandId = 0;
    EStudioJobEventKind Kind = EStudioJobEventKind::State;
    EStudioJobState State = EStudioJobState::Idle;
    FString Message;
    TOptional<int64> SimulatedSteps; // Authoritative harness counter, not CFD progress.
};

enum class EStudioJobScheduledOutputKind : uint8 { Trace, Slice, Visualization, Restart };
/** Development schedule expectation only: no output file or science value exists. */
struct FStudioJobScheduleNotice
{
    FGuid RunId;
    uint64 CommandId = 0;
    EStudioJobScheduledOutputKind Kind = EStudioJobScheduledOutputKind::Trace;
    int64 FirstStep = 0, LastStep = 0, Crossings = 0, Interval = 0;
};

/** Nonblocking owner-thread contract. Transports marshal worker events into Poll.
 * Poll appends at most MaxEvents; Send must never block on solver acknowledgement.
 * A command completes only on its corresponding authoritative state/result event. */
class IStudioJobAdapter
{
public:
    virtual ~IStudioJobAdapter() = default;
    virtual FStudioJobCapabilities Capabilities() const = 0;
    virtual void Send(const FStudioJobRequest& Request, double Now) = 0;
    virtual void Poll(double Now, int32 MaxEvents, TArray<FStudioJobEvent>& Out) = 0;
    // Optional independent stream: no command acknowledgements or log entries.
    // Owner-thread, nonblocking, at most MaxSamples complete measurements.
    virtual void PollTelemetry(double Now, int32 MaxSamples, TArray<FStudioJobMeasurement>& Out) {}
};

class FStudioJobController
{
public:
    explicit FStudioJobController(TUniquePtr<IStudioJobAdapter> InAdapter);
    bool Submit(const FString& Name, const FStudioCaseDraft& Draft, double Now);
    bool Command(EStudioJobCommand Command, double Now);
    bool StepN(int64 Count, double Now);
    bool RunToDimensionless(double Target, double Now);
    TOptional<int64> DimensionlessTargetStep(double Target) const;
    void Tick(double Now);
    bool Can(EStudioJobCommand Command) const;
    EStudioJobState State() const { return Current; }
    const FStudioJobCapabilities& Capabilities() const { return Caps; }
    const TOptional<FStudioRunRecord>& Run() const { return Record; }
    const FString& Notice() const { return Status; }
    const TArray<FStudioJobEvent>& Events() const { return History; }
    bool IsPending() const { return Pending.IsSet(); }
    uint64 PendingCommandId() const { return Pending.IsSet()?Pending->CommandId:0; }
    uint64 CompletedStepCommands() const { return Steps; }
    int64 SimulatedControlSteps() const { return SimulatedSteps; }
    const TArray<FStudioJobScheduleNotice>& ScheduledOutputs() const { return Schedule; }
    static constexpr int64 MaxSimulatedControlSteps = 1000000000000LL;
    uint64 CompletedCheckpointCommands() const { return Checkpoints; }
    FStudioJobTelemetryView Telemetry() const;
    const TArray<FStudioJobTelemetryRecord>& TelemetryHistory() const { return Measurements; }
    static bool IsTerminal(EStudioJobState State);
private:
    bool AdvanceClock(double Now);
    void Dispatch(FStudioJobRequest Request,double Now);
    bool Accept(const FStudioJobEvent& Event);
    bool AcceptTelemetry(const FStudioJobMeasurement& Sample);
    void BreakTelemetryRates();
    void AddScheduledOutputs(int64 Before, int64 After, uint64 CommandId);
    bool DispatchStepRange(EStudioJobCommand Command, int64 Count, double Now, TOptional<double> Target = {});
    TUniquePtr<IStudioJobAdapter> Adapter;
    FStudioJobCapabilities Caps;
    TOptional<FStudioRunRecord> Record;
    TOptional<FStudioJobRequest> Pending;
    TArray<FStudioJobEvent> History;
    TArray<FStudioJobTelemetryRecord> Measurements;
    TArray<FStudioJobScheduleNotice> Schedule;
    int64 SimulatedSteps = 0;
    TOptional<int64> StepHighWater;
    TOptional<double> PhysicalHighWater;
    EStudioJobState Current = EStudioJobState::Idle, BeforeCommand = EStudioJobState::Idle;
    uint64 NextCommand = 1, LastSequence = 0, Steps = 0, Checkpoints = 0;
    uint64 StateSequence = 0, RateAfterSequence = 0;
    double LastClock = -1, Deadline = 0;
    FString Status;
};

/** Deterministic development transport. Its events are control simulation only:
 * no fields, residuals, forces, physical time, restart files or resource metrics.
 * Callers inject monotonic seconds, so tests need neither sleep nor wall clocks. */
class FStudioControlHarness final : public IStudioJobAdapter
{
public:
    FStudioJobCapabilities Capabilities() const override;
    void Send(const FStudioJobRequest& Request,double Now) override;
    void Poll(double Now,int32 MaxEvents,TArray<FStudioJobEvent>& Out) override;
    // Deterministic fault controls used by tests and the future developer panel.
    void DropNextCommand() { bDropNext=true; }
    void RejectNextCommand() { bRejectNext=true; }
    void InjectFailure(double Now);
    void InjectDisconnect(double Now);
    void InjectCompletion(double Now);
private:
    struct FScheduled { double At=0; FStudioJobEvent Event; bool bQueryState=false; };
    void Queue(double At,uint64 Command,EStudioJobEventKind Kind,EStudioJobState State,const FString& Message,bool bQueryState=false);
    void DeliverUntil(double Now,int32 MaxEvents,TArray<FStudioJobEvent>* Out);
    FGuid ActiveRun;
    EStudioJobState Actual = EStudioJobState::Idle;
    TArray<FScheduled> QueueItems;
    uint64 Sequence = 0;
    int64 SimulatedSteps = 0;
    bool bDropNext=false,bRejectNext=false;
};
