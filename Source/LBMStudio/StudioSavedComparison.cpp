#include "StudioSavedComparison.h"
#include "StudioSavedFieldView.h"
#include "StudioModel.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace StudioSavedComparisonPrivate
{
bool Cancelled(const FStudioLoadCancellation& C){return C&&C->load(std::memory_order_relaxed);}
bool TextValid(const FString& Text,int32 Max)
{
    if(Text.IsEmpty()||Text.Len()>Max||Text.TrimStartAndEnd()!=Text)return false;
    for(TCHAR C:Text)if(C<32||C==127)return false;
    return true;
}
bool Integer(const TSharedPtr<FJsonObject>& O,const TCHAR* Key,int32& V,int32 Max)
{
    double D;if(!O->TryGetNumberField(Key,D)||!FMath::IsFinite(D)||D<0||D>Max||D!=FMath::FloorToDouble(D))return false;
    V=int32(D);return true;
}
TSharedRef<FJsonObject> SavedJSON(const FStudioSavedComparison& S)
{
    auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("id"),S.Id.ToString());O->SetStringField(TEXT("name"),S.Name);
    O->SetStringField(TEXT("scalar"),S.Scalar);O->SetStringField(TEXT("unit"),S.Unit);O->SetBoolField(TEXT("sharedRange"),S.bSharedRange);
    O->SetNumberField(TEXT("alignment"),int32(S.Alignment.Mode));O->SetNumberField(TEXT("matching"),int32(S.Alignment.Match));
    O->SetNumberField(TEXT("offsetSeconds"),S.Alignment.SecondaryOffsetSeconds);O->SetNumberField(TEXT("toleranceSeconds"),S.Alignment.MaximumMismatchSeconds);
    O->SetObjectField(TEXT("primary"),StudioSavedFieldViews::ToJSON(S.Primary));O->SetObjectField(TEXT("secondary"),StudioSavedFieldViews::ToJSON(S.Secondary));return O;
}
FString CollectionText(const TArray<FStudioSavedComparison>& Saved)
{
    auto O=MakeShared<FJsonObject>();O->SetArrayField(TEXT("comparisons"),StudioSavedComparisons::ToJSON(Saved));
    FString Text;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));return Text;
}
void Discard(FStudioComparisonRestoreResult& R)
{R.Pair.Reset();R.PrimarySource.Reset();R.SecondarySource.Reset();}
}

bool StudioSavedComparisons::IsValid(const FStudioSavedComparison& S,FString& Error)
{
    using namespace StudioSavedComparisonPrivate;
    if(!S.Id.IsValid()||!TextValid(S.Name,120))
    {Error=TEXT("Use a saved comparison name of 1–120 characters on one line.");return false;}
    if(!TextValid(S.Scalar,128)||!TextValid(S.Unit,64)||S.Unit.Equals(TEXT("unknown"),ESearchCase::IgnoreCase)||
        S.Unit.Equals(TEXT("unspecified"),ESearchCase::IgnoreCase)||!StudioComparison::IsValidAlignment(S.Alignment)||!StudioSavedFieldViews::IsValid(S.Primary)||!StudioSavedFieldViews::IsValid(S.Secondary))
    {Error=TEXT("Saved comparison contains invalid source, frame, scalar, alignment or camera values. Previous settings kept.");return false;}
    Error.Empty();return true;
}
bool StudioSavedComparisons::IsValid(const TArray<FStudioSavedComparison>& Saved,FString& Error)
{
    if(Saved.Num()>MaxEntries){Error=TEXT("This project supports 64 saved comparisons. Remove one before saving another.");return false;}
    TSet<FGuid> Ids;TSet<FString> Names;
    for(const auto& S:Saved)
    {
        if(!IsValid(S,Error))return false;
        if(Ids.Contains(S.Id)){Error=TEXT("The saved comparison list contains duplicate entries. Reopen a valid project copy.");return false;}
        if(Names.Contains(S.Name.ToLower())){Error=TEXT("A comparison already has that name. Choose a different name.");return false;}
        Ids.Add(S.Id);Names.Add(S.Name.ToLower());
    }
    Error.Empty();return true;
}
bool StudioSavedComparisons::Equals(const FStudioSavedComparison& A,const FStudioSavedComparison& B)
{return Equals(TArray<FStudioSavedComparison>{A},TArray<FStudioSavedComparison>{B});}
bool StudioSavedComparisons::Equals(const TArray<FStudioSavedComparison>& A,const TArray<FStudioSavedComparison>& B)
{return StudioSavedComparisonPrivate::CollectionText(A)==StudioSavedComparisonPrivate::CollectionText(B);}
int64 StudioSavedComparisons::StoredBytes(const TArray<FStudioSavedComparison>& Saved)
{return int64(StudioSavedComparisonPrivate::CollectionText(Saved).Len())*sizeof(TCHAR);}
TArray<TSharedPtr<FJsonValue>> StudioSavedComparisons::ToJSON(const TArray<FStudioSavedComparison>& Saved)
{
    TArray<TSharedPtr<FJsonValue>> A;for(const auto& S:Saved)A.Add(MakeShared<FJsonValueObject>(StudioSavedComparisonPrivate::SavedJSON(S)));return A;
}
bool StudioSavedComparisons::FromJSON(const TArray<TSharedPtr<FJsonValue>>& Array,TArray<FStudioSavedComparison>& Out,FString& Error)
{
    using namespace StudioSavedComparisonPrivate;
    Error=TEXT("Saved comparison settings are invalid. Current project kept.");if(Array.Num()>MaxEntries)return false;
    TArray<FStudioSavedComparison> Candidate;
    for(const auto& V:Array)
    {
        const TSharedPtr<FJsonObject>* O=nullptr,*A=nullptr,*B=nullptr;FStudioSavedComparison S;FString Id;int32 Mode=0,Match=0;
        if(!V||!V->TryGetObject(O)||!(*O)->TryGetStringField(TEXT("id"),Id)||!FGuid::Parse(Id,S.Id)||!(*O)->TryGetStringField(TEXT("name"),S.Name)||
            !(*O)->TryGetStringField(TEXT("scalar"),S.Scalar)||!(*O)->TryGetStringField(TEXT("unit"),S.Unit)||!(*O)->TryGetBoolField(TEXT("sharedRange"),S.bSharedRange)||
            !Integer(*O,TEXT("alignment"),Mode,3)||!Integer(*O,TEXT("matching"),Match,1)||!(*O)->TryGetNumberField(TEXT("offsetSeconds"),S.Alignment.SecondaryOffsetSeconds)||
            !(*O)->TryGetNumberField(TEXT("toleranceSeconds"),S.Alignment.MaximumMismatchSeconds)||!(*O)->TryGetObjectField(TEXT("primary"),A)||
            !(*O)->TryGetObjectField(TEXT("secondary"),B)||!StudioSavedFieldViews::FromJSON(*A,S.Primary)||!StudioSavedFieldViews::FromJSON(*B,S.Secondary))return false;
        S.Alignment.Mode=EStudioTimeAlignment(Mode);S.Alignment.Match=EStudioTimeMatch(Match);Candidate.Add(MoveTemp(S));
    }
    if(!IsValid(Candidate,Error))return false;Out=MoveTemp(Candidate);Error.Empty();return true;
}
bool StudioSavedComparisons::Create(const FString& Name,const FStudioComparisonResult& Pair,
    const FStudioCameraState& A,const FStudioCameraState& B,bool bSharedRange,const TArray<FStudioRecordingReference>& References,
    FStudioSavedComparison& Out,FString& Error)
{
    FStudioComparisonRequest R{Pair.ProjectId,Pair.PrimarySource.Pin(),Pair.SecondarySource.Pin(),Pair.Frames.PrimaryOrdinal,Pair.Scalar,Pair.Alignment};
    if(!Pair.Matches(R)){Error=TEXT("Compare the original frames before saving this comparison.");return false;}
    FStudioSavedComparison S;S.Name=Name.TrimStartAndEnd();S.Scalar=Pair.Scalar;S.Unit=Pair.Primary.Scalar.Unit;S.Alignment=Pair.Alignment;S.bSharedRange=bSharedRange;
    S.Primary.Title=Pair.Primary.Title;S.Secondary.Title=Pair.Secondary.Title;S.Primary.Identity=Pair.Primary.Identity;S.Secondary.Identity=Pair.Secondary.Identity;
    S.Primary.Camera=A;S.Secondary.Camera=B;
    for(auto* Side:{&S.Primary,&S.Secondary})
    {
        if(const auto* Ref=References.FindByPredicate([&](const auto& V){return StudioSavedFieldViews::ReferenceMatches(V,Side->Identity);}))Side->Reference=*Ref;
        else if(StudioRecordings::PathForId(Side->Identity.Dataset).IsEmpty())
        {Error=TEXT("The comparison's exact source reference is unavailable. Select the recording and compare its frames again before saving.");return false;}
    }
    if(!IsValid(S,Error))return false;Out=MoveTemp(S);return true;
}
FStudioComparisonRestoreResult StudioSavedComparisons::Restore(const FGuid& ProjectId,const FStudioSavedComparison& Saved,
    const TArray<FStudioRecordingReference>& References,const FStudioLoadCancellation& C)
{
    using namespace StudioSavedComparisonPrivate;
    FStudioComparisonRestoreResult R;R.ProjectId=ProjectId;R.Saved=Saved;
    auto Fail=[&](const FString& Error){Discard(R);R.bCancelled=Cancelled(C);R.Error=R.bCancelled?TEXT("Opening comparison cancelled. Current comparison kept."):Error;return MoveTemp(R);};
    FString Error;if(Cancelled(C))return Fail({});
    if(!ProjectId.IsValid()||!IsValid(Saved,Error))return Fail(Error.IsEmpty()?TEXT("This comparison has no project identity."):Error);
    auto A=StudioRecordings::Open(Saved.Primary.Identity.Dataset,StudioSavedFieldViews::ResolvedReference(Saved.Primary,References),Saved.Primary.Identity.Ordinal,C);
    if(!A.Source)return Fail(TEXT("Recording A: ")+A.Error+TEXT(" Locate the original source in Results and open the comparison again."));
    R.PrimarySource=MoveTemp(A.Source);if(Cancelled(C))return Fail({});
    auto B=StudioRecordings::Open(Saved.Secondary.Identity.Dataset,StudioSavedFieldViews::ResolvedReference(Saved.Secondary,References),Saved.Secondary.Identity.Ordinal,C);
    if(!B.Source)return Fail(TEXT("Recording B: ")+B.Error+TEXT(" Locate the original source in Results and open the comparison again."));
    R.SecondarySource=MoveTemp(B.Source);
    FStudioComparisonRequest Request{ProjectId,R.PrimarySource,R.SecondarySource,Saved.Primary.Identity.Ordinal,Saved.Scalar,Saved.Alignment};
    auto Pair=StudioComparison::Evaluate(Request,C);
    if(!Pair.Matches(Request))return Fail(Pair.Frames.Error);
    if(!StudioSavedFieldViews::SameIdentity(Saved.Primary.Identity,Pair.Primary.Identity)||!StudioSavedFieldViews::SameIdentity(Saved.Secondary.Identity,Pair.Secondary.Identity)||
        Saved.Unit!=Pair.Primary.Scalar.Unit||Saved.Unit!=Pair.Secondary.Scalar.Unit)
        return Fail(TEXT("The original source, reconstruction or frame differs from the saved comparison. Restore its original data, or make and save a new comparison."));
    if(Cancelled(C))return Fail({});R.Pair=MoveTemp(Pair);R.Error.Empty();return R;
}
bool FStudioComparisonRestoreResult::Matches(const FGuid& Project,const FStudioSavedComparison& Current) const
{
    if(bCancelled||!Error.IsEmpty()||Project!=ProjectId||!StudioSavedComparisons::Equals(Saved,Current)||!Pair.IsSet())return false;
    return Pair->Matches({ProjectId,PrimarySource,SecondarySource,Saved.Primary.Identity.Ordinal,Saved.Scalar,Saved.Alignment});
}
FStudioComparisonRestoreTask::~FStudioComparisonRestoreTask(){Shutdown();}
bool FStudioComparisonRestoreTask::Start(FGuid Project,FStudioSavedComparison Saved,TArray<FStudioRecordingReference> References,FString& Error)
{
    if(bShutdown||Pending.IsValid()){Error=TEXT("Wait for the comparison read to finish or cancel it before opening another.");return false;}
    if(!Project.IsValid()||!StudioSavedComparisons::IsValid(Saved,Error))return false;
    Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Pending=Async(EAsyncExecution::ThreadPool,[Project,Saved=MoveTemp(Saved),References=MoveTemp(References),Cancel=Cancellation]
        {return StudioSavedComparisons::Restore(Project,Saved,References,Cancel);});Error.Empty();return true;
}
void FStudioComparisonRestoreTask::Cancel(){if(Cancellation)Cancellation->store(true,std::memory_order_relaxed);}
void FStudioComparisonRestoreTask::Shutdown(){bShutdown=true;Cancel();if(Pending.IsValid()){Pending.Wait();Pending=TFuture<FStudioComparisonRestoreResult>();}Cancellation.Reset();}
TOptional<FStudioComparisonRestoreResult> FStudioComparisonRestoreTask::Poll()
{
    if(!Pending.IsValid()||!Pending.IsReady())return {};auto R=Pending.Consume();
    if(StudioSavedComparisonPrivate::Cancelled(Cancellation)){StudioSavedComparisonPrivate::Discard(R);R.bCancelled=true;R.Error=TEXT("Opening comparison cancelled. Current comparison kept.");}
    Cancellation.Reset();return R;
}
