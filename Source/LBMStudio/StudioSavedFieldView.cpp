#include "StudioSavedFieldView.h"
#include "StudioView.h"

namespace StudioSavedFieldViews
{
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
bool SameIdentity(const FStudioFieldIdentity& A,const FStudioFieldIdentity& B)
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
bool IsValid(const FStudioSavedFieldView& S)
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
TSharedRef<FJsonObject> ToJSON(const FStudioSavedFieldView& S)
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
bool ReadInto(const TSharedPtr<FJsonObject>& O,FStudioSavedFieldView& S)
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
    return IsValid(S);
}
TArray<FStudioRecordingReference> ResolvedReference(const FStudioSavedFieldView& Side,const TArray<FStudioRecordingReference>& Current)
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
bool FromJSON(const TSharedPtr<FJsonObject>& JSON,FStudioSavedFieldView& Out)
{
    FStudioSavedFieldView Candidate;
    if(!ReadInto(JSON,Candidate))return false;
    Out=MoveTemp(Candidate);return true;
}
}
