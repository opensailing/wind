#include "StudioBoundaries.h"
#include "StudioColor.h"
#include "StudioMaterials.h"

namespace
{
template<typename T> bool SameBoundaryValue(const TOptional<T>& A,const TOptional<T>& B)
{return A.IsSet()==B.IsSet()&&(!A.IsSet()||A.GetValue()==B.GetValue());}
FString BoundaryNumber(const TOptional<double>& Value)
{return Value.IsSet()?StudioMaterials::ExactNumber(Value.GetValue()):FString();}
void ForEachBoundaryTarget(const FStudioCaseDraft& Case,TFunctionRef<void(const FStudioBoundaryTarget&)> Visit)
{
    for(int32 I=0;I<Case.Domain.Faces.Num();++I)
        Visit({Case.Domain.Faces[I],FGuid(),Case.Domain.FaceNames.IsValidIndex(I)?Case.Domain.FaceNames[I]:FString::Printf(TEXT("Domain face %d"),I+1),I});
    for(const auto& Geometry:Case.Geometry)for(const auto& Patch:Geometry.Patches)
        Visit({Patch.Id,Geometry.Id,Geometry.Name+TEXT(" / ")+Patch.Name,INDEX_NONE});
}
}

void FStudioBoundaryEdit::Reset(const FStudioBoundaryTarget& Target,const FStudioBoundaryCondition* Assignment)
{
    bHadAssignment=Assignment!=nullptr;Saved=Assignment?*Assignment:FStudioBoundaryCondition();
    if(!Assignment){Saved.TargetId=Target.Id;Saved.Name=Target.Name.Left(120);}
    Name=Saved.Name;Type=Saved.Type;Pressure=BoundaryNumber(Saved.Pressure);Temperature=BoundaryNumber(Saved.Temperature);
    for(int32 I=0;I<3;++I)Velocity[I]=Saved.Velocity.IsSet()?StudioMaterials::ExactNumber(Saved.Velocity.GetValue()[I]):FString();
    Error.Empty();ErrorField=INDEX_NONE;
}
bool FStudioBoundaryEdit::Matches(const FStudioBoundaryCondition* Assignment) const
{
    if(!Assignment)return !bHadAssignment;
    return bHadAssignment&&Assignment->Id==Saved.Id&&Assignment->TargetId==Saved.TargetId&&Assignment->PairedTargetId==Saved.PairedTargetId&&
        Assignment->Name==Saved.Name&&Assignment->Type==Saved.Type&&SameBoundaryValue(Assignment->Velocity,Saved.Velocity)&&
        SameBoundaryValue(Assignment->Pressure,Saved.Pressure)&&SameBoundaryValue(Assignment->Temperature,Saved.Temperature);
}
bool FStudioBoundaryEdit::IsDirty() const
{
    if(Name!=Saved.Name||Type!=Saved.Type||Pressure!=BoundaryNumber(Saved.Pressure)||Temperature!=BoundaryNumber(Saved.Temperature))return true;
    for(int32 I=0;I<3;++I)if(Velocity[I]!=(Saved.Velocity.IsSet()?StudioMaterials::ExactNumber(Saved.Velocity.GetValue()[I]):FString()))return true;
    return false;
}
bool FStudioBoundaryEdit::Build(const FStudioCaseDraft& Case,FStudioBoundaryCondition& Out)
{
    Error.Empty();ErrorField=INDEX_NONE;
    auto Fail=[&](int32 Field,const FString& Message){ErrorField=Field;Error=Message;return false;};
    FStudioBoundaryTarget Target;
    if(!StudioBoundaries::FindTarget(Case,Saved.TargetId,Target))return Fail(INDEX_NONE,TEXT("This face or patch no longer exists. Choose a current target."));
    if(uint8(Type)>uint8(EStudioBoundaryType::Periodic))return Fail(INDEX_NONE,TEXT("Choose a supported boundary type."));
    const FString Clean=Name.TrimStartAndEnd();
    if(Clean.IsEmpty()||Name.Len()>120)return Fail(0,TEXT("Name: enter 1–120 characters."));
    FStudioBoundaryCondition Candidate=Saved;Candidate.Name=Clean;Candidate.Type=Type;
    Candidate.Velocity.Reset();Candidate.Pressure.Reset();Candidate.Temperature.Reset();Candidate.PairedTargetId.Invalidate();
    auto Scalar=[&](const FString& Text,const TOptional<double>& Previous,TOptional<double>& Value,int32 Index,bool Positive)
    {
        if(Text.TrimStartAndEnd().IsEmpty()){Value.Reset();return true;}
        if(Text==BoundaryNumber(Previous)){Value=Previous;return true;}
        double Number=0;
        if(!StudioColor::ParseNumber(Text,Number)||FMath::Abs(Number)>1.e12||(Positive&&Number<=0))
            return Fail(Index,Positive?TEXT("Temperature: enter a finite value above zero and at most 1e12 K, or leave blank."):
                TEXT("Pressure: enter a finite value between -1e12 and 1e12 Pa, or leave blank."));
        Value=Number;return true;
    };
    if(Type==EStudioBoundaryType::VelocityInlet)
    {
        int32 Supplied=0;for(const auto& Component:Velocity)if(!Component.TrimStartAndEnd().IsEmpty())++Supplied;
        if(Supplied&&Supplied!=3)
        {
            for(int32 I=0;I<3;++I)if(Velocity[I].TrimStartAndEnd().IsEmpty())return Fail(1+I,TEXT("Velocity: enter all three components, including explicit zeros, or leave all three blank."));
        }
        if(Supplied)
        {
            FVector Value;
            for(int32 I=0;I<3;++I)
            {
                if(Saved.Velocity.IsSet()&&Velocity[I]==StudioMaterials::ExactNumber(Saved.Velocity.GetValue()[I]))Value[I]=Saved.Velocity.GetValue()[I];
                else if(!StudioColor::ParseNumber(Velocity[I],Value[I])||FMath::Abs(Value[I])>1.e8)
                    return Fail(1+I,TEXT("Velocity: each component must be finite and between -1e8 and 1e8 m/s."));
            }
            Candidate.Velocity=Value;
        }
    }
    if(Type==EStudioBoundaryType::PressureOutlet&&!Scalar(Pressure,Saved.Pressure,Candidate.Pressure,4,false))return false;
    if(Type!=EStudioBoundaryType::Periodic&&Type!=EStudioBoundaryType::Unassigned&&
        !Scalar(Temperature,Saved.Temperature,Candidate.Temperature,5,true))return false;
    if(Type==EStudioBoundaryType::Periodic)
    {
        if(Target.DomainFace==INDEX_NONE||!Case.Domain.Faces.IsValidIndex(Target.DomainFace^1))
            return Fail(INDEX_NONE,TEXT("Periodic pairing is available for opposite domain faces. Imported-patch pairing needs a solver mapping."));
        Candidate.PairedTargetId=Case.Domain.Faces[Target.DomainFace^1];
    }
    Out=MoveTemp(Candidate);return true;
}

FString StudioBoundaries::TypeName(EStudioBoundaryType Type)
{
    switch(Type)
    {
    case EStudioBoundaryType::Unassigned:return TEXT("Unassigned");
    case EStudioBoundaryType::VelocityInlet:return TEXT("Velocity inlet");
    case EStudioBoundaryType::PressureOutlet:return TEXT("Pressure outlet");
    case EStudioBoundaryType::NoSlip:return TEXT("No-slip wall");
    case EStudioBoundaryType::Slip:return TEXT("Slip wall");
    case EStudioBoundaryType::Symmetry:return TEXT("Symmetry");
    case EStudioBoundaryType::Periodic:return TEXT("Periodic pair");
    }
    return TEXT("Unknown boundary type");
}
bool StudioBoundaries::FindTarget(const FStudioCaseDraft& Case,const FGuid& Id,FStudioBoundaryTarget& Out)
{
    if(!Id.IsValid())return false;
    const int32 Face=Case.Domain.Faces.IndexOfByKey(Id);
    if(Face!=INDEX_NONE)
    {
        Out={Id,FGuid(),Case.Domain.FaceNames.IsValidIndex(Face)?Case.Domain.FaceNames[Face]:FString::Printf(TEXT("Domain face %d"),Face+1),Face};
        return true;
    }
    for(const auto& Geometry:Case.Geometry)for(const auto& Patch:Geometry.Patches)if(Patch.Id==Id)
    {Out={Patch.Id,Geometry.Id,Geometry.Name+TEXT(" / ")+Patch.Name,INDEX_NONE};return true;}
    return false;
}
FStudioBoundaryTargetPage StudioBoundaries::TargetPage(const FStudioCaseDraft& Case,const FString& Filter,int32 Offset,int32 Limit)
{
    FStudioBoundaryTargetPage Out;Offset=FMath::Max(0,Offset);Limit=FMath::Clamp(Limit,0,128);
    const FString Query=Filter.TrimStartAndEnd();
    auto Add=[&](const FGuid& Id,const FGuid& Geometry,const FString& Name,int32 Face)
    {
        if(!Query.IsEmpty()&&!Name.Contains(Query,ESearchCase::IgnoreCase))return;
        const int32 Index=Out.Matches++;
        if(Index>=Offset&&Out.Items.Num()<Limit)Out.Items.Add({Id,Geometry,Name,Face});
    };
    for(int32 I=0;I<Case.Domain.Faces.Num();++I)
        Add(Case.Domain.Faces[I],FGuid(),Case.Domain.FaceNames.IsValidIndex(I)?Case.Domain.FaceNames[I]:FString::Printf(TEXT("Domain face %d"),I+1),I);
    for(const auto& Geometry:Case.Geometry)for(const auto& Patch:Geometry.Patches)
        Add(Patch.Id,Geometry.Id,Geometry.Name+TEXT(" / ")+Patch.Name,INDEX_NONE);
    return Out;
}
TArray<FStudioBoundaryTarget> StudioBoundaries::Targets(const FStudioCaseDraft& Case)
{
    TArray<FStudioBoundaryTarget> Out;
    for(int32 I=0;I<Case.Domain.Faces.Num();++I)
        Out.Add({Case.Domain.Faces[I],FGuid(),Case.Domain.FaceNames.IsValidIndex(I)?Case.Domain.FaceNames[I]:FString::Printf(TEXT("Domain face %d"),I+1),I});
    for(const auto& Geometry:Case.Geometry)for(const auto& Patch:Geometry.Patches)
        Out.Add({Patch.Id,Geometry.Id,Geometry.Name+TEXT(" / ")+Patch.Name,INDEX_NONE});
    return Out;
}
FStudioBoundaryCoverage StudioBoundaries::Analyze(const FStudioCaseDraft& Case,const FStudioBoundaryCapabilities* Capabilities)
{
    FStudioBoundaryCoverage Out;
    auto AddIssue=[&](const FGuid& Target,EStudioBoundaryIssue Kind,const FString& Message)
    {
        ++Out.IssueCount;if(Kind!=EStudioBoundaryIssue::Unsupported)++Out.BlockingIssueCount;
        if(Out.Issues.Num()<128)Out.Issues.Add({Target,Kind,Message});
    };
    Out.bCapabilitiesKnown=Capabilities&&!Capabilities->BackendId.IsEmpty()&&Capabilities->BackendId==Case.Setup.BackendId;
    FString StructuralError;
    if(!StudioCaseIO::Validate(Case,StructuralError))AddIssue(FGuid(),EStudioBoundaryIssue::Conflict,StructuralError);
    TMap<FGuid,const FStudioBoundaryCondition*> AssignedByTarget;TMap<FGuid,int32> AssignmentCounts;
    for(const auto& Boundary:Case.Boundaries){AssignedByTarget.FindOrAdd(Boundary.TargetId)=&Boundary;++AssignmentCounts.FindOrAdd(Boundary.TargetId);}
    TSet<FGuid> SeenTargets;
    ForEachBoundaryTarget(Case,[&](const FStudioBoundaryTarget& Target)
    {
        ++Out.Targets;
        const auto* Assigned=AssignedByTarget.FindRef(Target.Id);const int32 Count=AssignmentCounts.FindRef(Target.Id);
        if(Assigned)SeenTargets.Add(Target.Id);
        auto Issue=[&](EStudioBoundaryIssue Kind,const FString& Message){AddIssue(Target.Id,Kind,Message);};
        if(!Assigned||(Count==1&&Assigned->Type==EStudioBoundaryType::Unassigned))
        {Issue(EStudioBoundaryIssue::Missing,TEXT("Choose a boundary condition for ")+Target.Name+TEXT("."));return;}
        if(Count>1){Issue(EStudioBoundaryIssue::Conflict,TEXT("More than one condition targets ")+Target.Name+TEXT("."));return;}
        const auto& B=*Assigned;bool Configured=true;
        if(B.Type==EStudioBoundaryType::VelocityInlet&&!B.Velocity.IsSet())
        {Issue(EStudioBoundaryIssue::Incomplete,TEXT("Enter all three velocity components in m/s."));Configured=false;}
        if(B.Type==EStudioBoundaryType::PressureOutlet&&!B.Pressure.IsSet())
        {Issue(EStudioBoundaryIssue::Incomplete,TEXT("Enter the outlet pressure in Pa."));Configured=false;}
        if((B.Type!=EStudioBoundaryType::VelocityInlet&&B.Velocity.IsSet())||(B.Type!=EStudioBoundaryType::PressureOutlet&&B.Pressure.IsSet()))
        {Issue(EStudioBoundaryIssue::Conflict,TEXT("This type has incompatible velocity or pressure values. Clear the unused values."));Configured=false;}
        if(B.Type==EStudioBoundaryType::Periodic)
        {
            const auto* Partner=AssignedByTarget.FindRef(B.PairedTargetId);
            if(Target.DomainFace==INDEX_NONE||!Case.Domain.Faces.IsValidIndex(Target.DomainFace^1)||B.PairedTargetId!=Case.Domain.Faces[Target.DomainFace^1]||!Partner||
                Partner->Type!=EStudioBoundaryType::Periodic||Partner->PairedTargetId!=B.TargetId||B.Temperature.IsSet())
            {Issue(EStudioBoundaryIssue::Conflict,TEXT("Periodic conditions require a reciprocal pair on opposite domain faces."));Configured=false;}
        }
        else if(B.PairedTargetId.IsValid()){Issue(EStudioBoundaryIssue::Conflict,TEXT("A nonperiodic condition cannot retain a periodic partner."));Configured=false;}
        if(uint8(B.Type)>uint8(EStudioBoundaryType::Periodic)){Issue(EStudioBoundaryIssue::Unsupported,TEXT("Unknown boundary type."));Configured=false;}
        if(Configured)++Out.Configured;
        if(Out.bCapabilitiesKnown)
        {
            if(!Capabilities->Types.Contains(B.Type))Issue(EStudioBoundaryIssue::Unsupported,TEXT("The selected backend does not support ")+TypeName(B.Type)+TEXT("."));
            if(B.Temperature.IsSet()&&!Capabilities->bTemperature)Issue(EStudioBoundaryIssue::Unsupported,TEXT("The selected backend does not support prescribed boundary temperature."));
        }
        if(B.Temperature.IsSet()&&!Case.Setup.bThermal)
            Issue(EStudioBoundaryIssue::Unsupported,TEXT("Prescribed temperature requires an enabled thermal model and supporting backend."));
    });
    for(const auto& Boundary:Case.Boundaries)if(!SeenTargets.Contains(Boundary.TargetId))
        AddIssue(Boundary.TargetId,EStudioBoundaryIssue::Conflict,TEXT("The assigned patch no longer exists. Reassign this boundary before running."));
    return Out;
}
bool StudioBoundaries::Set(FStudioCaseDraft& Case,const FStudioBoundaryCondition& Boundary,bool bUnpairExisting,FString& Error)
{
    if(!StudioCaseIO::Validate(Case,Error))return false;
    FStudioCaseDraft Candidate=Case;FStudioBoundaryTarget Target;
    if(!FindTarget(Candidate,Boundary.TargetId,Target)){Error=TEXT("The selected face or surface patch no longer exists.");return false;}
    const auto* Previous=Candidate.Boundaries.FindByPredicate([&](const auto& B){return B.TargetId==Boundary.TargetId;});
    if(Previous&&Previous->Id!=Boundary.Id){Error=TEXT("The boundary assignment changed. Reload its current values before applying.");return false;}
    if(Previous&&Previous->Type==EStudioBoundaryType::Periodic&&Boundary.Type!=EStudioBoundaryType::Periodic)
    {
        if(!bUnpairExisting){Error=TEXT("Changing this condition also removes its periodic partner. Apply the explicit unpair action.");return false;}
        const FGuid Partner=Previous->PairedTargetId;
        Candidate.Boundaries.RemoveAll([&](const auto& B){return B.TargetId==Partner;});
    }
    if(Boundary.Type==EStudioBoundaryType::Periodic)
    {
        if(Target.DomainFace==INDEX_NONE||Boundary.PairedTargetId!=Candidate.Domain.Faces[Target.DomainFace^1])
        {Error=TEXT("Choose the opposite domain face as the periodic partner. Imported-patch mappings require a supporting solver adapter.");return false;}
        auto* Partner=Candidate.Boundaries.FindByPredicate([&](const auto& B){return B.TargetId==Boundary.PairedTargetId;});
        if(Partner&&(Partner->Type!=EStudioBoundaryType::Periodic||Partner->PairedTargetId!=Boundary.TargetId))
        {Error=TEXT("The opposite face already has a different assignment. Remove it before creating the periodic pair.");return false;}
        if(!Partner)
        {
            FStudioBoundaryCondition Other;Other.TargetId=Boundary.PairedTargetId;Other.PairedTargetId=Boundary.TargetId;Other.Type=EStudioBoundaryType::Periodic;
            Other.Name=Candidate.Domain.FaceNames[Target.DomainFace^1].Left(111)+TEXT(" periodic");Candidate.Boundaries.Add(MoveTemp(Other));
        }
    }
    if(auto* Existing=Candidate.Boundaries.FindByPredicate([&](const auto& B){return B.TargetId==Boundary.TargetId;}))*Existing=Boundary;
    else Candidate.Boundaries.Add(Boundary);
    if(!StudioCaseIO::Validate(Candidate,Error))return false;
    Case=MoveTemp(Candidate);Error.Empty();return true;
}
bool StudioBoundaries::Remove(FStudioCaseDraft& Case,const FGuid& Target,FString& Error)
{
    const auto* Existing=Case.Boundaries.FindByPredicate([&](const auto& B){return B.TargetId==Target;});
    if(!Existing){Error=TEXT("This face or patch has no boundary assignment to remove.");return false;}
    const FGuid Partner=Existing->Type==EStudioBoundaryType::Periodic?Existing->PairedTargetId:FGuid();
    FStudioCaseDraft Candidate=Case;
    Candidate.Boundaries.RemoveAll([&](const auto& B){return B.TargetId==Target||(Partner.IsValid()&&B.TargetId==Partner);});
    if(!StudioCaseIO::Validate(Candidate,Error))return false;
    Case=MoveTemp(Candidate);Error.Empty();return true;
}
