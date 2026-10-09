#include "StudioRunSettings.h"
#include "StudioColor.h"
#include <charconv>

namespace StudioRunSettingsPrivate
{
constexpr int64 MaxCount = 1000000000000LL;
FString Exact(double Value)
{
    ANSICHAR Buffer[128];
    const auto Result = std::to_chars(Buffer, Buffer + UE_ARRAY_COUNT(Buffer) - 1, Value);
    if (Result.ec != std::errc()) return FString::Printf(TEXT("%.17g"), Value);
    *Result.ptr = '\0';
    return FString(UTF8_TO_TCHAR(Buffer));
}
bool Count(const FString& Text, int64& Out)
{
    const FString Digits = Text.TrimStartAndEnd();
    if (Digits.IsEmpty() || Digits.Len() > 13) return false;
    int64 Value = 0;
    for (TCHAR Digit : Digits)
    {
        if (Digit < TEXT('0') || Digit > TEXT('9')) return false;
        Value = Value * 10 + Digit - TEXT('0');
        if (Value > MaxCount) return false;
    }
    if (Value < 1) return false;
    Out = Value;
    return true;
}
}

void FStudioRunSettingsEdit::Reset(const FStudioCaseDraft& Case)
{
    CaseId = Case.Id; Saved = Case.Setup;
    Values[MaxSteps] = LexToString(Saved.MaxSteps);
    Values[MaxPhysicalTime] = Saved.MaxPhysicalTime ? StudioRunSettingsPrivate::Exact(*Saved.MaxPhysicalTime) : FString();
    Values[OutputInterval] = LexToString(Saved.OutputInterval);
    Values[CheckpointInterval] = LexToString(Saved.CheckpointInterval);
    bCheckpoints = Saved.bCheckpoints;
    Error.Empty(); ErrorField = INDEX_NONE;
}

bool FStudioRunSettingsEdit::IsDirty() const
{
    return bCheckpoints != Saved.bCheckpoints || Values[MaxSteps] != LexToString(Saved.MaxSteps) ||
        Values[MaxPhysicalTime] != (Saved.MaxPhysicalTime ? StudioRunSettingsPrivate::Exact(*Saved.MaxPhysicalTime) : FString()) ||
        Values[OutputInterval] != LexToString(Saved.OutputInterval) ||
        Values[CheckpointInterval] != LexToString(Saved.CheckpointInterval);
}

bool FStudioRunSettingsEdit::Matches(const FStudioCaseDraft& Case) const
{
    const auto& Setup = Case.Setup;
    return CaseId == Case.Id && Saved.MaxSteps == Setup.MaxSteps && Saved.MaxPhysicalTime == Setup.MaxPhysicalTime &&
        Saved.OutputInterval == Setup.OutputInterval && Saved.bCheckpoints == Setup.bCheckpoints &&
        Saved.CheckpointInterval == Setup.CheckpointInterval;
}

bool FStudioRunSettingsEdit::Build(FStudioCaseSetup& Out)
{
    Error.Empty(); ErrorField = INDEX_NONE;
    const auto Fail = [this](EField Field, const FString& Message)
    { ErrorField = Field; Error = Message; return false; };
    FStudioCaseSetup Candidate = Saved;
    if (!StudioRunSettingsPrivate::Count(Values[MaxSteps], Candidate.MaxSteps))
        return Fail(MaxSteps, TEXT("Maximum steps: enter whole-number digits from 1 to 1000000000000."));
    if (Values[MaxPhysicalTime].TrimStartAndEnd().IsEmpty()) Candidate.MaxPhysicalTime.Reset();
    else
    {
        double Seconds;
        if (!StudioColor::ParseNumber(Values[MaxPhysicalTime], Seconds) || Seconds <= 0 || Seconds > 1.e12)
            return Fail(MaxPhysicalTime, TEXT("Maximum physical time: enter seconds above 0 and no larger than 1e12, or leave blank for no time limit."));
        Candidate.MaxPhysicalTime = Seconds;
    }
    if (!StudioRunSettingsPrivate::Count(Values[OutputInterval], Candidate.OutputInterval))
        return Fail(OutputInterval, TEXT("Output interval: enter whole-number solver steps from 1 to 1000000000000."));
    if (!StudioRunSettingsPrivate::Count(Values[CheckpointInterval], Candidate.CheckpointInterval))
        return Fail(CheckpointInterval, TEXT("Checkpoint interval: enter whole-number solver steps from 1 to 1000000000000."));
    Candidate.bCheckpoints = bCheckpoints;
    Out = MoveTemp(Candidate);
    return true;
}

void FStudioRunSettingsEdit::CopySettings(const FStudioCaseSetup& From, FStudioCaseSetup& To)
{
    To.MaxSteps = From.MaxSteps;
    To.MaxPhysicalTime = From.MaxPhysicalTime;
    To.OutputInterval = From.OutputInterval;
    To.bCheckpoints = From.bCheckpoints;
    To.CheckpointInterval = From.CheckpointInterval;
}
