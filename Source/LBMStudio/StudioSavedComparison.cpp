#include "StudioSavedComparison.h"
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
bool HashValid(const FString& Hash)
{
    if(Hash.Len()!=64)return false;
    for(TCHAR C:Hash)if(!FChar::IsHexDigit(C))return false;
    return true;
}
bool IdentityEqual(const FStudioFieldIdentity& A,const FStudioFieldIdentity& B)
{
    return A.Dataset==B.Dataset&&A.MetadataSHA256==B.MetadataSHA256&&A.PayloadSHA256==B.PayloadSHA256&&
        A.ReconstructionSHA256==B.ReconstructionSHA256&&A.Ordinal==B.Ordinal&&A.Frame.Index==B.Frame.Index&&
        A.Frame.Time==B.Frame.Time&&A.SpatialDimensions==B.SpatialDimensions&&A.SourceOffset==B.SourceOffset&&A.Interpolation==B.Interpolation;
}
bool ReferenceMatches(const FStudioRecordingReference& R,const FStudioFieldIdentity& I)
{
    return StudioRecordings::IsValidReference(R)&&R.Id==I.Dataset&&R.MetadataSHA256==I.MetadataSHA256&&
        R.PayloadSHA256==I.PayloadSHA256&&(R.Reconstruction.IsSet()?R.Reconstruction->MetadataSHA256:FString())==I.ReconstructionSHA256;
}
bool SideValid(const FStudioSavedComparisonSide& S)
{
    const auto& I=S.Identity;
    if(!TextValid(S.Title,256)||!TextValid(I.Dataset,256)||!HashValid(I.MetadataSHA256)||
        (!I.PayloadSHA256.IsEmpty()&&!HashValid(I.PayloadSHA256))||(!I.ReconstructionSHA256.IsEmpty()&&!HashValid(I.ReconstructionSHA256))||
        I.Ordinal<0||I.Ordinal>=StudioComparison::MaxTimelineFrames||I.Frame.Index<0||!FMath::IsFinite(I.Frame.Time)||I.Frame.Time<0||
        (I.SpatialDimensions!=2&&I.SpatialDimensions!=3)||I.SourceOffset.ContainsNaN()||I.SourceOffset.GetAbsMax()>1.e8||
        uint8(I.Interpolation)>uint8(EStudioFieldInterpolation::ReconstructedGrid))return false;
    const bool Reconstructed=I.Interpolation==EStudioFieldInterpolation::ReconstructedTriangles||I.Interpolation==EStudioFieldInterpolation::ReconstructedGrid;
    if(Reconstructed!=!I.ReconstructionSHA256.IsEmpty())return false;
    if(S.Reference.IsSet()&&!ReferenceMatches(*S.Reference,I))return false;
    FStudioInspectionState View;View.Camera=S.Camera;return StudioView::IsValid(View);
}
TSharedRef<FJsonObject> ReferenceJSON(const FStudioRecordingReference& R)
{
    auto O=MakeShared<FJsonObject>();
    O->SetStringField(TEXT("id"),R.Id);O->SetStringField(TEXT("title"),R.Title);O->SetStringField(TEXT("path"),R.Path);
    O->SetStringField(TEXT("format"),R.Format);O->SetStringField(TEXT("metadataSHA256"),R.MetadataSHA256);O->SetStringField(TEXT("payloadSHA256"),R.PayloadSHA256);
    if(R.Reconstruction.IsSet())
    {auto V=MakeShared<FJsonObject>();V->SetStringField(TEXT("path"),R.Reconstruction->Path);V->SetStringField(TEXT("metadataSHA256"),R.Reconstruction->MetadataSHA256);O->SetObjectField(TEXT("reconstruction"),V);}
    else O->SetField(TEXT("reconstruction"),MakeShared<FJsonValueNull>());
    return O;
}
bool ReadReference(const TSharedPtr<FJsonObject>& O,FStudioRecordingReference& R)
{
    if(!O||!O->TryGetStringField(TEXT("id"),R.Id)||!O->TryGetStringField(TEXT("title"),R.Title)||!O->TryGetStringField(TEXT("path"),R.Path)||
        !O->TryGetStringField(TEXT("format"),R.Format)||!O->TryGetStringField(TEXT("metadataSHA256"),R.MetadataSHA256)||
        !O->TryGetStringField(TEXT("payloadSHA256"),R.PayloadSHA256))return false;
    const auto V=O->TryGetField(TEXT("reconstruction"));if(!V)return false;
    if(!V->IsNull())
    {
        const TSharedPtr<FJsonObject>* P=nullptr;FStudioReconstructionReference Ref;
        if(!V->TryGetObject(P)||!(*P)->TryGetStringField(TEXT("path"),Ref.Path)||!(*P)->TryGetStringField(TEXT("metadataSHA256"),Ref.MetadataSHA256))return false;
        R.Reconstruction=MoveTemp(Ref);
    }
    return StudioRecordings::IsValidReference(R);
}
bool Integer(const TSharedPtr<FJsonObject>& O,const TCHAR* Key,int32& V,int32 Max)
{
    double D;if(!O->TryGetNumberField(Key,D)||!FMath::IsFinite(D)||D<0||D>Max||D!=FMath::FloorToDouble(D))return false;
    V=int32(D);return true;
}
TSharedRef<FJsonObject> SideJSON(const FStudioSavedComparisonSide& S)
{
    auto O=MakeShared<FJsonObject>();const auto& I=S.Identity;
    O->SetStringField(TEXT("title"),S.Title);O->SetStringField(TEXT("dataset"),I.Dataset);
    O->SetStringField(TEXT("metadataSHA256"),I.MetadataSHA256);O->SetStringField(TEXT("payloadSHA256"),I.PayloadSHA256);
    O->SetStringField(TEXT("reconstructionSHA256"),I.ReconstructionSHA256);O->SetNumberField(TEXT("ordinal"),I.Ordinal);
    O->SetNumberField(TEXT("step"),I.Frame.Index);O->SetNumberField(TEXT("timeSeconds"),I.Frame.Time);
    O->SetNumberField(TEXT("dimensions"),I.SpatialDimensions);O->SetNumberField(TEXT("interpolation"),int32(I.Interpolation));
    TArray<TSharedPtr<FJsonValue>> Offset;for(int32 A=0;A<3;++A)Offset.Add(MakeShared<FJsonValueNumber>(I.SourceOffset[A]));
    O->SetArrayField(TEXT("sourceOffsetMeters"),Offset);O->SetObjectField(TEXT("camera"),StudioProjectIO::CameraToJSON(S.Camera));
    if(S.Reference.IsSet())O->SetObjectField(TEXT("reference"),ReferenceJSON(*S.Reference));
    else O->SetField(TEXT("reference"),MakeShared<FJsonValueNull>());
    return O;
}
bool ReadSide(const TSharedPtr<FJsonObject>& O,FStudioSavedComparisonSide& S)
{
    auto& I=S.Identity;int32 Interpolation=0;const TArray<TSharedPtr<FJsonValue>>* Offset=nullptr;const TSharedPtr<FJsonObject>* Camera=nullptr;
    if(!O||!O->TryGetStringField(TEXT("title"),S.Title)||!O->TryGetStringField(TEXT("dataset"),I.Dataset)||
        !O->TryGetStringField(TEXT("metadataSHA256"),I.MetadataSHA256)||!O->TryGetStringField(TEXT("payloadSHA256"),I.PayloadSHA256)||
        !O->TryGetStringField(TEXT("reconstructionSHA256"),I.ReconstructionSHA256)||!Integer(O,TEXT("ordinal"),I.Ordinal,StudioComparison::MaxTimelineFrames-1)||
        !Integer(O,TEXT("step"),I.Frame.Index,MAX_int32)||!O->TryGetNumberField(TEXT("timeSeconds"),I.Frame.Time)||
        !Integer(O,TEXT("dimensions"),I.SpatialDimensions,3)||!Integer(O,TEXT("interpolation"),Interpolation,3)||
        !O->TryGetArrayField(TEXT("sourceOffsetMeters"),Offset)||Offset->Num()!=3||!O->TryGetObjectField(TEXT("camera"),Camera)||
        !StudioProjectIO::CameraFromJSON(*Camera,S.Camera))return false;
    I.Interpolation=EStudioFieldInterpolation(Interpolation);
    for(int32 A=0;A<3;++A)if(!(*Offset)[A]||!(*Offset)[A]->TryGetNumber(I.SourceOffset[A]))return false;
    const auto Ref=O->TryGetField(TEXT("reference"));if(!Ref)return false;
    if(!Ref->IsNull())
    {const TSharedPtr<FJsonObject>* R=nullptr;FStudioRecordingReference Reference;if(!Ref->TryGetObject(R)||!ReadReference(*R,Reference))return false;S.Reference=MoveTemp(Reference);}
    return SideValid(S);
}
TSharedRef<FJsonObject> SavedJSON(const FStudioSavedComparison& S)
{
    auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("id"),S.Id.ToString());O->SetStringField(TEXT("name"),S.Name);
    O->SetStringField(TEXT("scalar"),S.Scalar);O->SetStringField(TEXT("unit"),S.Unit);O->SetBoolField(TEXT("sharedRange"),S.bSharedRange);
    O->SetNumberField(TEXT("alignment"),int32(S.Alignment.Mode));O->SetNumberField(TEXT("matching"),int32(S.Alignment.Match));
    O->SetNumberField(TEXT("offsetSeconds"),S.Alignment.SecondaryOffsetSeconds);O->SetNumberField(TEXT("toleranceSeconds"),S.Alignment.MaximumMismatchSeconds);
    O->SetObjectField(TEXT("primary"),SideJSON(S.Primary));O->SetObjectField(TEXT("secondary"),SideJSON(S.Secondary));return O;
}
FString CollectionText(const TArray<FStudioSavedComparison>& Saved)
{
    auto O=MakeShared<FJsonObject>();O->SetArrayField(TEXT("comparisons"),StudioSavedComparisons::ToJSON(Saved));
    FString Text;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));return Text;
}
TArray<FStudioRecordingReference> ResolvedReference(const FStudioSavedComparisonSide& Side,const TArray<FStudioRecordingReference>& Current)
{
    // Installed sources remain installed even if an unrelated external entry
    // later happens to reuse that ID. Every result still checks its saved hash.
    if(!Side.Reference.IsSet())return {};
    auto Ref=*Side.Reference;
    if(const auto* New=Current.FindByPredicate([&](const auto& R){return R.Id==Ref.Id&&R.Format==Ref.Format&&
        R.MetadataSHA256==Ref.MetadataSHA256&&R.PayloadSHA256==Ref.PayloadSHA256;}))
    {
        Ref.Path=New->Path;
        if(Ref.Reconstruction.IsSet()&&New->Reconstruction.IsSet()&&Ref.Reconstruction->MetadataSHA256==New->Reconstruction->MetadataSHA256)
            Ref.Reconstruction->Path=New->Reconstruction->Path;
    }
    return {MoveTemp(Ref)};
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
        S.Unit.Equals(TEXT("unspecified"),ESearchCase::IgnoreCase)||!StudioComparison::IsValidAlignment(S.Alignment)||!SideValid(S.Primary)||!SideValid(S.Secondary))
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
            !(*O)->TryGetObjectField(TEXT("secondary"),B)||!ReadSide(*A,S.Primary)||!ReadSide(*B,S.Secondary))return false;
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
        if(const auto* Ref=References.FindByPredicate([&](const auto& V){return StudioSavedComparisonPrivate::ReferenceMatches(V,Side->Identity);}))Side->Reference=*Ref;
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
    auto A=StudioRecordings::Open(Saved.Primary.Identity.Dataset,ResolvedReference(Saved.Primary,References),Saved.Primary.Identity.Ordinal,C);
    if(!A.Source)return Fail(TEXT("Recording A: ")+A.Error+TEXT(" Locate the original source in Results and open the comparison again."));
    R.PrimarySource=MoveTemp(A.Source);if(Cancelled(C))return Fail({});
    auto B=StudioRecordings::Open(Saved.Secondary.Identity.Dataset,ResolvedReference(Saved.Secondary,References),Saved.Secondary.Identity.Ordinal,C);
    if(!B.Source)return Fail(TEXT("Recording B: ")+B.Error+TEXT(" Locate the original source in Results and open the comparison again."));
    R.SecondarySource=MoveTemp(B.Source);
    FStudioComparisonRequest Request{ProjectId,R.PrimarySource,R.SecondarySource,Saved.Primary.Identity.Ordinal,Saved.Scalar,Saved.Alignment};
    auto Pair=StudioComparison::Evaluate(Request,C);
    if(!Pair.Matches(Request))return Fail(Pair.Frames.Error);
    if(!IdentityEqual(Saved.Primary.Identity,Pair.Primary.Identity)||!IdentityEqual(Saved.Secondary.Identity,Pair.Secondary.Identity)||
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
