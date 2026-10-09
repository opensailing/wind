#include "StudioPipeline.h"
#include "StudioSavedFieldView.h"
#include "Serialization/JsonSerializer.h"

namespace StudioPipelinePrivate
{
bool Text(const FString& V,int32 Max)
{
    if(V.IsEmpty()||V.Len()>Max||V!=V.TrimStartAndEnd())return false;
    for(TCHAR C:V)if(FChar::IsControl(C))return false;
    return true;
}
bool FieldId(const FString& V)
{
    if(!Text(V,128))return false;
    for(TCHAR C:V)if(!FChar::IsAlnum(C)&&C!='_'&&C!='-'&&C!='.')return false;
    return true;
}
bool Point(const FVector& V){return !V.ContainsNaN()&&V.GetAbsMax()<=1.e8;}
bool KnownUnit(const FString& V)
{return Text(V,64)&&!V.Equals(TEXT("unknown"),ESearchCase::IgnoreCase)&&!V.Equals(TEXT("unspecified"),ESearchCase::IgnoreCase);}
bool Parameters(const FStudioPipelineOperation& O)
{
    if(!O.Id.IsValid()||!Text(O.Name,120))return false;
    switch(O.Kind)
    {
    case EStudioPipelineOperation::Field:return FieldId(O.Field)&&Text(O.Unit,64);
    case EStudioPipelineOperation::Magnitude:
    {
        if(!FieldId(O.Field)||!O.Field.StartsWith(TEXT("derived."))||O.Field.Len()<=8||!KnownUnit(O.Unit)||O.Components.Num()<2||O.Components.Num()>3)return false;
        TSet<FString> Seen;for(const auto& S:O.Components){if(!FieldId(S)||Seen.Contains(S)||S==O.Field)return false;Seen.Add(S);}return true;
    }
    case EStudioPipelineOperation::ClipBox:return Point(O.A)&&Point(O.B)&&O.A.X<O.B.X&&O.A.Y<O.B.Y&&O.A.Z<O.B.Z;
    case EStudioPipelineOperation::Slice:return Point(O.A)&&Point(O.B)&&FMath::Abs(O.B.SizeSquared()-1.)<=1.e-10;
    case EStudioPipelineOperation::Contour:return FMath::IsFinite(O.Value);
    case EStudioPipelineOperation::Probe:return Point(O.A)&&Point(O.B)&&O.Samples>=2&&O.Samples<=StudioPipelines::MaxProbeSamples&&(!O.bLine||O.A!=O.B);
    }
    return false;
}
TArray<TSharedPtr<FJsonValue>> VectorJSON(const FVector& V)
{return {MakeShared<FJsonValueNumber>(V.X),MakeShared<FJsonValueNumber>(V.Y),MakeShared<FJsonValueNumber>(V.Z)};}
bool ReadVector(const TSharedPtr<FJsonObject>& O,const TCHAR* Key,FVector& V)
{
    const TArray<TSharedPtr<FJsonValue>>* A=nullptr;if(!O->TryGetArrayField(Key,A)||A->Num()!=3)return false;
    for(int32 I=0;I<3;++I)if(!(*A)[I]||!(*A)[I]->TryGetNumber(V[I]))return false;
    return Point(V);
}
bool Integer(const TSharedPtr<FJsonObject>& O,const TCHAR* Key,int32& V,int32 Max)
{
    double D;if(!O->TryGetNumberField(Key,D)||!FMath::IsFinite(D)||D<0||D>Max||D!=FMath::FloorToDouble(D))return false;
    V=int32(D);return true;
}
TSharedRef<FJsonObject> OperationJSON(const FStudioPipelineOperation& P)
{
    auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("id"),P.Id.ToString());O->SetStringField(TEXT("name"),P.Name);
    O->SetNumberField(TEXT("kind"),int32(P.Kind));O->SetBoolField(TEXT("enabled"),P.bEnabled);
    if(P.Kind==EStudioPipelineOperation::Field||P.Kind==EStudioPipelineOperation::Magnitude)
    {O->SetStringField(TEXT("field"),P.Field);O->SetStringField(TEXT("unit"),P.Unit);}
    if(P.Kind==EStudioPipelineOperation::Magnitude)
    {TArray<TSharedPtr<FJsonValue>> A;for(const auto& C:P.Components)A.Add(MakeShared<FJsonValueString>(C));O->SetArrayField(TEXT("components"),A);}
    if(P.Kind==EStudioPipelineOperation::ClipBox||P.Kind==EStudioPipelineOperation::Slice||P.Kind==EStudioPipelineOperation::Probe)
    {O->SetArrayField(TEXT("a"),VectorJSON(P.A));O->SetArrayField(TEXT("b"),VectorJSON(P.B));}
    if(P.Kind==EStudioPipelineOperation::Contour)O->SetNumberField(TEXT("value"),P.Value);
    if(P.Kind==EStudioPipelineOperation::Probe){O->SetBoolField(TEXT("line"),P.bLine);O->SetNumberField(TEXT("samples"),P.Samples);}
    return O;
}
bool ReadOperation(const TSharedPtr<FJsonObject>& O,FStudioPipelineOperation& P)
{
    FString Id;int32 Kind;
    if(!O||!O->TryGetStringField(TEXT("id"),Id)||!FGuid::Parse(Id,P.Id)||!O->TryGetStringField(TEXT("name"),P.Name)||
        !Integer(O,TEXT("kind"),Kind,5)||!O->TryGetBoolField(TEXT("enabled"),P.bEnabled))return false;
    P.Kind=EStudioPipelineOperation(Kind);
    if((P.Kind==EStudioPipelineOperation::Field||P.Kind==EStudioPipelineOperation::Magnitude)&&
        (!O->TryGetStringField(TEXT("field"),P.Field)||!O->TryGetStringField(TEXT("unit"),P.Unit)))return false;
    if(P.Kind==EStudioPipelineOperation::Magnitude)
    {
        const TArray<TSharedPtr<FJsonValue>>* A=nullptr;if(!O->TryGetArrayField(TEXT("components"),A)||A->Num()<2||A->Num()>3)return false;
        for(const auto& V:*A){FString S;if(!V||!V->TryGetString(S))return false;P.Components.Add(S);}
    }
    if((P.Kind==EStudioPipelineOperation::ClipBox||P.Kind==EStudioPipelineOperation::Slice||P.Kind==EStudioPipelineOperation::Probe)&&
        (!ReadVector(O,TEXT("a"),P.A)||!ReadVector(O,TEXT("b"),P.B)))return false;
    if(P.Kind==EStudioPipelineOperation::Contour&&!O->TryGetNumberField(TEXT("value"),P.Value))return false;
    if(P.Kind==EStudioPipelineOperation::Probe&&(!O->TryGetBoolField(TEXT("line"),P.bLine)||!Integer(O,TEXT("samples"),P.Samples,StudioPipelines::MaxProbeSamples)))return false;
    return Parameters(P);
}
FString CollectionText(const TArray<FStudioSavedPipeline>& V)
{
    auto O=MakeShared<FJsonObject>();O->SetArrayField(TEXT("pipelines"),StudioPipelines::ToJSON(V));FString S;
    FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;
}
}

bool StudioPipelines::IsValid(const FStudioSavedPipeline& P,FString& Error)
{
    using namespace StudioPipelinePrivate;
    if(!P.Id.IsValid()||!Text(P.Name,120)||!StudioSavedFieldViews::IsValid(P.Source))
    {Error=TEXT("A pipeline needs a name, exact original frame/source and valid independent camera.");return false;}
    if(P.Operations.IsEmpty()||P.Operations.Num()>MaxOperations)
    {Error=TEXT("A pipeline needs 1–32 ordered operations.");return false;}
    TSet<FGuid> Ids;TSet<FString> Names,Defined,DerivedNames;
    for(const auto& O:P.Operations)if(O.Kind==EStudioPipelineOperation::Magnitude)DerivedNames.Add(O.Field);
    bool Scalar=false,Slice=false,Contour=false,Probe=false;
    for(const auto& O:P.Operations)
    {
        auto Fail=[&](const TCHAR* Why){Error=O.Name+TEXT(": ")+Why;return false;};
        if(!Parameters(O))return Fail(TEXT("Invalid operation parameters. Keep finite coordinates, a unit slice normal and valid field/unit values."));
        if(Ids.Contains(O.Id)||Names.Contains(O.Name.ToLower()))return Fail(TEXT("Operation IDs and names must be distinct."));
        Ids.Add(O.Id);Names.Add(O.Name.ToLower());if(!O.bEnabled)continue;
        if(Probe)return Fail(TEXT("A probe produces the final sample table. Move this operation before the probe or disable it."));
        if(O.Kind==EStudioPipelineOperation::Field||O.Kind==EStudioPipelineOperation::Magnitude)
        {
            if(Contour)return Fail(TEXT("Select or derive the scalar before contour extraction."));
            if(O.Kind==EStudioPipelineOperation::Field&&DerivedNames.Contains(O.Field)&&!Defined.Contains(O.Field))
                return Fail(TEXT("Enable and place the derived field before selecting it."));
            if(O.Kind==EStudioPipelineOperation::Magnitude)
            {
                if(Defined.Contains(O.Field))return Fail(TEXT("Derived output IDs must be distinct."));
                for(const auto& C:O.Components)if(DerivedNames.Contains(C)&&!Defined.Contains(C))return Fail(TEXT("A component depends on a later or disabled derived field."));
                Defined.Add(O.Field);
            }
            Scalar=true;continue;
        }
        if(!Scalar)return Fail(TEXT("Select a scalar field or derive a magnitude before this operation."));
        if(O.Kind==EStudioPipelineOperation::Slice)
        {
            if(Slice||Contour)return Fail(TEXT("Only one slice is supported, before any contour."));
            if(P.Source.Identity.SpatialDimensions==2&&(O.B.X!=0.||O.B.Z!=0.||FMath::Abs(O.B.Y)!=1.||O.A.Y!=P.Source.Identity.SourceOffset.Y))
                return Fail(TEXT("A 2D field slice must lie on its original scene X/Z plane."));
            Slice=true;
        }
        if(O.Kind==EStudioPipelineOperation::Contour)
        {if(Contour)return Fail(TEXT("Only one contour extraction is supported."));Contour=true;}
        if(O.Kind==EStudioPipelineOperation::Probe)
        {if(Contour)return Fail(TEXT("Probe the field or slice before extracting contour geometry."));Probe=true;}
    }
    if(!Scalar){Error=TEXT("Enable a scalar field or magnitude operation.");return false;}
    Error.Empty();return true;
}
bool StudioPipelines::IsValid(const TArray<FStudioSavedPipeline>& P,FString& Error)
{
    if(P.Num()>MaxEntries){Error=TEXT("A project supports 64 saved pipelines. Remove one before saving another.");return false;}
    TSet<FGuid> Ids;TSet<FString> Names;
    for(const auto& V:P)
    {
        if(!IsValid(V,Error))return false;
        if(Ids.Contains(V.Id)||Names.Contains(V.Name.ToLower())){Error=TEXT("Pipeline IDs and names must be distinct.");return false;}
        Ids.Add(V.Id);Names.Add(V.Name.ToLower());
    }
    Error.Empty();return true;
}
bool StudioPipelines::Equals(const FStudioSavedPipeline& A,const FStudioSavedPipeline& B)
{return Equals(TArray<FStudioSavedPipeline>{A},TArray<FStudioSavedPipeline>{B});}
bool StudioPipelines::Equals(const TArray<FStudioSavedPipeline>& A,const TArray<FStudioSavedPipeline>& B)
{return StudioPipelinePrivate::CollectionText(A)==StudioPipelinePrivate::CollectionText(B);}
int64 StudioPipelines::StoredBytes(const TArray<FStudioSavedPipeline>& P)
{return int64(StudioPipelinePrivate::CollectionText(P).Len())*sizeof(TCHAR);}
TArray<TSharedPtr<FJsonValue>> StudioPipelines::ToJSON(const TArray<FStudioSavedPipeline>& P)
{
    TArray<TSharedPtr<FJsonValue>> A;
    for(const auto& V:P)
    {
        auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("id"),V.Id.ToString());O->SetStringField(TEXT("name"),V.Name);
        O->SetObjectField(TEXT("source"),StudioSavedFieldViews::ToJSON(V.Source));TArray<TSharedPtr<FJsonValue>> Ops;
        for(const auto& Op:V.Operations)Ops.Add(MakeShared<FJsonValueObject>(StudioPipelinePrivate::OperationJSON(Op)));
        O->SetArrayField(TEXT("operations"),Ops);A.Add(MakeShared<FJsonValueObject>(O));
    }
    return A;
}
bool StudioPipelines::FromJSON(const TArray<TSharedPtr<FJsonValue>>& JSON,TArray<FStudioSavedPipeline>& Out,FString& Error)
{
    Error=TEXT("Invalid saved pipelines. Current project kept.");if(JSON.Num()>MaxEntries)return false;
    TArray<FStudioSavedPipeline> Candidate;
    for(const auto& V:JSON)
    {
        const TSharedPtr<FJsonObject>* O=nullptr,*Source=nullptr;const TArray<TSharedPtr<FJsonValue>>* Ops=nullptr;FString Id;FStudioSavedPipeline P;
        if(!V||!V->TryGetObject(O)||!(*O)->TryGetStringField(TEXT("id"),Id)||!FGuid::Parse(Id,P.Id)||!(*O)->TryGetStringField(TEXT("name"),P.Name)||
            !(*O)->TryGetObjectField(TEXT("source"),Source)||!StudioSavedFieldViews::FromJSON(*Source,P.Source)||
            !(*O)->TryGetArrayField(TEXT("operations"),Ops)||Ops->IsEmpty()||Ops->Num()>MaxOperations)return false;
        for(const auto& Value:*Ops)
        {const TSharedPtr<FJsonObject>* Op=nullptr;FStudioPipelineOperation Operation;if(!Value||!Value->TryGetObject(Op)||!StudioPipelinePrivate::ReadOperation(*Op,Operation))return false;P.Operations.Add(MoveTemp(Operation));}
        Candidate.Add(MoveTemp(P));
    }
    if(!IsValid(Candidate,Error))return false;Out=MoveTemp(Candidate);Error.Empty();return true;
}
bool StudioPipelines::Compile(const FStudioSavedPipeline& P,const FStudioRecordingDescriptor& D,TArray<FStudioPipelineStage>& Out,FString& Error)
{
    if(!IsValid(P,Error))return false;
    const auto& I=P.Source.Identity;
    if(I.Dataset!=D.Id||I.MetadataSHA256!=D.MetadataSHA256||I.PayloadSHA256!=D.PayloadSHA256||I.SpatialDimensions!=D.SpatialDimensions||
        I.SourceOffset!=D.SourceOffset||!D.Frames.IsValidIndex(I.Ordinal)||I.Frame.Index!=D.Frames[I.Ordinal].Index||I.Frame.Time!=D.Frames[I.Ordinal].Time)
    {Error=TEXT("Pipeline source or original frame differs from this recording. Reopen its pinned source.");return false;}
    TMap<FString,FString> Units;for(const auto& S:D.Scalars)
    {
        if(Units.Contains(S.Id)){Error=TEXT("The source has duplicate scalar IDs.");return false;}
        Units.Add(S.Id,S.Unit);
    }
    TArray<FStudioPipelineStage> Stages;FString Field,Unit;int32 Dimensions=I.SpatialDimensions;
    for(const auto& O:P.Operations)
    {
        if(!O.bEnabled)continue;
        auto Fail=[&](const TCHAR* Why){Error=O.Name+TEXT(": ")+Why;return false;};
        if(O.Kind==EStudioPipelineOperation::Field)
        {
            const auto* U=Units.Find(O.Field);if(!U||*U!=O.Unit)return Fail(TEXT("The selected array or its pinned unit is unavailable."));
            Field=O.Field;Unit=O.Unit;
        }
        if(O.Kind==EStudioPipelineOperation::Magnitude)
        {
            if(Units.Contains(O.Field))return Fail(TEXT("The derived output would replace an existing array. Choose a new ID."));
            for(const auto& C:O.Components){const auto* U=Units.Find(C);if(!U||*U!=O.Unit)return Fail(TEXT("Magnitude requires available components with exactly the same known unit."));}
            Field=O.Field;Unit=O.Unit;Units.Add(Field,Unit);
        }
        if(O.Kind==EStudioPipelineOperation::Slice||O.Kind==EStudioPipelineOperation::Contour||O.Kind==EStudioPipelineOperation::Probe)
        {
            if(I.Interpolation==EStudioFieldInterpolation::None)return Fail(TEXT("This operation needs verified interpolation. Attach a reconstruction or use original-point field/clip operations."));
        }
        if(O.Kind==EStudioPipelineOperation::Slice)Dimensions=2;
        if(O.Kind==EStudioPipelineOperation::Contour)--Dimensions;
        if(O.Kind==EStudioPipelineOperation::Probe)Dimensions=0;
        Stages.Add({O.Id,O.Kind,Field,Unit,Dimensions});
    }
    Out=MoveTemp(Stages);Error.Empty();return true;
}
bool StudioPipelines::Move(FStudioSavedPipeline& P,const FGuid& Id,int32 To,FString& Error)
{
    const int32 From=P.Operations.IndexOfByPredicate([Id](const auto& O){return O.Id==Id;});
    if(From==INDEX_NONE||!P.Operations.IsValidIndex(To)){Error=TEXT("Choose an existing operation and a position inside this pipeline.");return false;}
    auto Candidate=P;auto Item=Candidate.Operations[From];Candidate.Operations.RemoveAt(From);Candidate.Operations.Insert(MoveTemp(Item),To);
    if(!IsValid(Candidate,Error))return false;P=MoveTemp(Candidate);return true;
}
bool StudioPipelines::SetEnabled(FStudioSavedPipeline& P,const FGuid& Id,bool Enabled,FString& Error)
{
    auto Candidate=P;auto* Item=Candidate.Operations.FindByPredicate([Id](const auto& O){return O.Id==Id;});
    if(!Item){Error=TEXT("This operation is no longer in the pipeline.");return false;}Item->bEnabled=Enabled;
    if(!IsValid(Candidate,Error))return false;P=MoveTemp(Candidate);return true;
}
