#pragma once
#include "StudioCase.h"

struct FStudioBoundaryTarget
{
    FGuid Id,GeometryId;
    FString Name;
    int32 DomainFace=INDEX_NONE;
};
struct FStudioBoundaryTargetPage
{
    TArray<FStudioBoundaryTarget> Items;
    int32 Matches=0;
};
struct FStudioBoundaryCapabilities
{
    FString BackendId;
    TSet<EStudioBoundaryType> Types;
    bool bTemperature=false;
};
enum class EStudioBoundaryIssue : uint8 { Missing, Incomplete, Conflict, Unsupported };
struct FStudioBoundaryDiagnostic
{
    FGuid Target;
    EStudioBoundaryIssue Kind=EStudioBoundaryIssue::Missing;
    FString Message;
};
struct FStudioBoundaryCoverage
{
    int32 Targets=0,Configured=0;
    int32 IssueCount=0,BlockingIssueCount=0;
    bool bCapabilitiesKnown=false;
    // A bounded sample; counts include every issue even when details are omitted.
    TArray<FStudioBoundaryDiagnostic> Issues;
    bool Complete() const { return Targets>0&&Targets==Configured&&BlockingIssueCount==0; }
    bool Compatible() const { return Complete()&&bCapabilitiesKnown&&IssueCount==0; }
};

/** Unapplied text remains local to a stable face/patch and survives navigation. */
struct FStudioBoundaryEdit
{
    FStudioBoundaryCondition Saved;
    bool bHadAssignment=false;
    FString Name,Velocity[3],Pressure,Temperature;
    EStudioBoundaryType Type=EStudioBoundaryType::Unassigned;
    FString Error;
    int32 ErrorField=INDEX_NONE; // 0 name, 1–3 velocity, 4 pressure, 5 temperature.
    void Reset(const FStudioBoundaryTarget& Target,const FStudioBoundaryCondition* Assignment);
    bool Matches(const FStudioBoundaryCondition* Assignment) const;
    bool IsDirty() const;
    bool Build(const FStudioCaseDraft& Case,FStudioBoundaryCondition& Out);
};
namespace StudioBoundaries
{
    FString TypeName(EStudioBoundaryType Type);
    /** Find one stable target without allocating the complete patch catalog. */
    bool FindTarget(const FStudioCaseDraft& Case,const FGuid& Id,FStudioBoundaryTarget& Out);
    /** Filter all names, but allocate at most 128 visible rows. Offset counts matches. */
    FStudioBoundaryTargetPage TargetPage(const FStudioCaseDraft& Case,const FString& Filter,int32 Offset=0,int32 Limit=128);
    TArray<FStudioBoundaryTarget> Targets(const FStudioCaseDraft& Case);
    FStudioBoundaryCoverage Analyze(const FStudioCaseDraft& Case,const FStudioBoundaryCapabilities* Capabilities=nullptr);
    /** Applies to a copy first. Periodic partner changes are one case edit. */
    bool Set(FStudioCaseDraft& Case,const FStudioBoundaryCondition& Boundary,bool bUnpairExisting,FString& Error);
    bool Remove(FStudioCaseDraft& Case,const FGuid& Target,FString& Error);
}
