#include "StudioHome4ReferenceSources.h"
#include "StudioHome4JSON.h"
#include "StudioHome4Couette.h"
#include "StudioFileDialog.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Paths.h"
#define UI UI_HOME4_REFERENCE_SOURCES
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI
namespace StudioHome4ReferenceSourcesPrivate
{
    constexpr int32 Max=4*1024*1024;
    FString JSON(const TSharedRef<FJsonObject>& O)
    {FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
    bool Hash(const TArray<uint8>& Bytes,FString& Out)
    {uint8 D[32];unsigned int N=0;if(EVP_Digest(Bytes.GetData(),Bytes.Num(),D,&N,EVP_sha256(),nullptr)!=1||N!=32)return false;Out=BytesToHex(D,N).ToLower();return true;}
}
bool StudioHome4ReferenceSources::Parse(const FString& JSON,const FString& Recipe,FStudioHome4SeriesSource& Out,FString& Error)
{
    using namespace StudioHome4ReferenceSourcesPrivate;const FTCHARToUTF8 Bytes(*JSON);TSharedPtr<FJsonObject> O;
    if(Bytes.Length()<=0||Bytes.Length()>Max||!StudioHome4JSON::Preflight(JSON)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON),O)||!O)
    {Error=TEXT("Independent original series must be strict UTF-8 JSON within 4 MiB.");return false;}
    FString Schema,Kind,Source,Epoch;double Version=0;
    if(!O->TryGetStringField(TEXT("schema"),Schema)||Schema!=TEXT("LBMStudio.Home4Series")||!O->TryGetNumberField(TEXT("version"),Version)||Version!=1||
        !O->TryGetStringField(TEXT("kind"),Kind)||(Kind!=TEXT("actual")&&Kind!=TEXT("reference"))||!O->TryGetStringField(TEXT("source_id"),Source)||Source.TrimStartAndEnd().IsEmpty()||Source.Len()>2048||
        !O->TryGetStringField(TEXT("epoch"),Epoch)||Epoch.TrimStartAndEnd().IsEmpty()||Epoch.Len()>256)
    {Error=TEXT("Original series needs schema/version, actual/reference kind, original source identity and explicit epoch.");return false;}
    for(TCHAR C:Source)if(C<32||C==127){Error=TEXT("Original source identity contains an invalid control character.");return false;}
    for(TCHAR C:Epoch)if(C<32||C==127){Error=TEXT("Original source epoch contains an invalid control character.");return false;}
    const auto Input=O->TryGetField(TEXT("series"));
    if(!Input||Input->Type!=EJson::Array||Input->AsArray().IsEmpty()||Input->AsArray().Num()>16){Error=TEXT("Supply 1–16 original named series.");return false;}
    TOptional<double> AnalyticTorque;FString AnalyticUnit;
    const auto Analytic=O->TryGetField(TEXT("analytic_couette"));
    if(Analytic&&Analytic->Type!=EJson::Null)
    {
        FStudioHome4CouetteInputs C;double Torque=0;
        if(Kind!=TEXT("reference")||Recipe!=TEXT("couette-spin")||Analytic->Type!=EJson::Object||!StudioHome4Couette::Parse(Analytic->AsObject(),C,Error)||!StudioHome4Couette::Torque(C,Torque,AnalyticUnit,Error))
        {if(Error.IsEmpty())Error=TEXT("Analytic Couette is available only as an identified Couette reference source with explicit original inputs.");return false;}
        AnalyticTorque=Torque;
    }
    TArray<TSharedPtr<FJsonValue>> Series;
    for(const auto& V:Input->AsArray())
    {
        if(V->Type!=EJson::Object){Error=TEXT("Original series must be objects.");return false;}
        const auto S=V->AsObject();auto Values=S->TryGetField(TEXT("values"));
        if(AnalyticTorque)
        {
            FString Id,Unit;const auto X=S->TryGetField(TEXT("x"));
            if(Values||!S->TryGetStringField(TEXT("id"),Id)||Id!=TEXT("torque")||!S->TryGetStringField(TEXT("unit"),Unit)||Unit!=AnalyticUnit||!X||X->Type!=EJson::Array||X->AsArray().IsEmpty()||X->AsArray().Num()>100000)
            {Error=TEXT("Analytic reference needs torque series with explicit original x samples and exact computed torque unit; values must be absent to avoid replacing supplied measurements.");return false;}
            TArray<TSharedPtr<FJsonValue>> Generated;for(int32 I=0;I<X->AsArray().Num();++I)Generated.Add(MakeShared<FJsonValueNumber>(*AnalyticTorque));
            Values=MakeShared<FJsonValueArray>(Generated);
        }
        if(!Values||Values->Type!=EJson::Array){Error=TEXT("Original series needs an explicit values array.");return false;}
        auto N=MakeShared<FJsonObject>();N->Values=S->Values;N->RemoveField(TEXT("values"));N->SetField(TEXT("actual"),Values);N->SetField(TEXT("reference"),Values);
        const auto SuppliedEpoch=S->TryGetField(TEXT("epoch"));
        if(SuppliedEpoch&&(SuppliedEpoch->Type!=EJson::String||SuppliedEpoch->AsString()!=Epoch)){Error=TEXT("Series epoch must match the explicitly declared source epoch.");return false;}
        N->SetStringField(TEXT("epoch"),Epoch);
        N->SetNumberField(TEXT("absolute_tolerance"),0);N->SetNumberField(TEXT("relative_tolerance"),0);Series.Add(MakeShared<FJsonValueObject>(N));
    }
    auto Normal=MakeShared<FJsonObject>();Normal->Values=O->Values;Normal->SetStringField(TEXT("schema"),TEXT("LBMStudio.Home4Reference"));
    if(AnalyticTorque)Normal->SetStringField(TEXT("reference_method"),StudioHome4Couette::FormulaDescription()+TEXT(" Source: ")+StudioHome4Couette::CitationURL());
    Normal->SetStringField(TEXT("actual_source"),Source);Normal->SetStringField(TEXT("reference_source"),Source);Normal->SetArrayField(TEXT("series"),Series);
    FStudioHome4ReferenceExpectation Expected;Expected.RecipeId=Recipe;FStudioHome4SeriesSource Candidate;Candidate.bReference=Kind==TEXT("reference");Candidate.Epoch=Epoch;
    if(!StudioHome4Validation::Parse(StudioHome4ReferenceSourcesPrivate::JSON(Normal),Expected,Candidate.Data,Error))return false;
    Candidate.Data.OriginalBytes.Reset();Candidate.Data.OriginalBytes.Append(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());if(!Hash(Candidate.Data.OriginalBytes,Candidate.Data.SourceSHA256)){Error=TEXT("Original source hash could not be calculated.");return false;}
    Out=MoveTemp(Candidate);Error.Empty();return true;
}
bool StudioHome4ReferenceSources::Load(const FString& Path,const FString& Recipe,FStudioHome4SeriesSource& Out,FString& Error)
{
    using namespace StudioHome4ReferenceSourcesPrivate;FStudioFileAccess Access(Path);
    const int64 Size=IFileManager::Get().FileSize(*Path);const auto Stamp=IFileManager::Get().GetTimeStamp(*Path);
    if(Size<=0||Size>Max){Error=TEXT("Independent original series must be between 1 byte and 4 MiB.");return false;}
    auto Read=[&](TArray<uint8>& B)
    {
        TUniquePtr<FArchive> F(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));if(!F||F->TotalSize()!=Size)return false;
        B.SetNumUninitialized(int32(Size));F->Serialize(B.GetData(),Size);const bool Good=!F->IsError();const bool Closed=F->Close();F.Reset();return Good&&Closed;
    };
    TArray<uint8> B,Checked;
    if(!Read(B)||!StudioHome4JSON::UTF8(B.GetData(),B.Num())){Error=TEXT("Original independent source could not be read as UTF-8.");return false;}
    const int32 Offset=B.Num()>=3&&B[0]==0xef&&B[1]==0xbb&&B[2]==0xbf?3:0;
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(B.GetData()+Offset),B.Num()-Offset);FStudioHome4SeriesSource Candidate;
    if(!Parse(FString(Text.Length(),Text.Get()),Recipe,Candidate,Error))return false;
    if(!Read(Checked)||Checked!=B||IFileManager::Get().FileSize(*Path)!=Size||IFileManager::Get().GetTimeStamp(*Path)!=Stamp)
    {Error=TEXT("Original independent source changed during import; prior evidence retained.");return false;}
    Candidate.Data.OriginalBytes=MoveTemp(B);if(!Hash(Candidate.Data.OriginalBytes,Candidate.Data.SourceSHA256)){Error=TEXT("Original source hash could not be calculated.");return false;}Candidate.Data.SourcePath=FPaths::ConvertRelativePathToFull(Path);Out=MoveTemp(Candidate);Error.Empty();return true;
}
bool StudioHome4ReferenceSources::AlignExact(const FStudioHome4SeriesSource& Actual,const FStudioHome4SeriesSource& Reference,double Absolute,double Relative,
    TOptional<double> Start,TOptional<double> End,FStudioHome4ReferenceEvidence& Out,FString& Error)
{
    if(Actual.bReference||!Reference.bReference||Actual.Data.RunId==Reference.Data.RunId||Actual.Data.RecipeId!=Reference.Data.RecipeId||Actual.Epoch!=Reference.Epoch||
        !FMath::IsFinite(Absolute)||Absolute<0||!FMath::IsFinite(Relative)||Relative<0||Start.IsSet()!=End.IsSet()||(Start&&(!FMath::IsFinite(*Start)||!FMath::IsFinite(*End)||*Start>=*End)))
    {Error=TEXT("Exact alignment requires original actual/reference sources, identical recipe/declared epoch, explicit tolerances and paired increasing finite window bounds.");return false;}
    for(const auto* Source:{&Actual,&Reference})
    {
        const auto& B=Source->Data.OriginalBytes;FString Hash;FStudioHome4SeriesSource Parsed;
        if(B.IsEmpty()||B.Num()>StudioHome4ReferenceSourcesPrivate::Max||!StudioHome4ReferenceSourcesPrivate::Hash(B,Hash)||Hash!=Source->Data.SourceSHA256||!StudioHome4JSON::UTF8(B.GetData(),B.Num()))
        {Error=TEXT("Independent original source bytes/hash are absent or changed.");return false;}
        const int32 Offset=B.Num()>=3&&B[0]==0xef&&B[1]==0xbb&&B[2]==0xbf?3:0;const FUTF8ToTCHAR T(reinterpret_cast<const ANSICHAR*>(B.GetData()+Offset),B.Num()-Offset);
        if(!Parse(FString(T.Length(),T.Get()),Source->Data.RecipeId,Parsed,Error)||Parsed.bReference!=Source->bReference||Parsed.Epoch!=Source->Epoch||StudioHome4Validation::SerializeEvidence(Parsed.Data)!=StudioHome4Validation::SerializeEvidence(Source->Data))
        {Error=TEXT("Independent source measurements no longer match their exact original bytes.");return false;}
    }
    FStudioHome4ReferenceEvidence E;E.RecipeId=Actual.Data.RecipeId;E.RunId=Actual.Data.RunId;E.OriginalRunSpec=Actual.Data.OriginalRunSpec;E.ActualSource=Actual.Data.ActualSource;E.ReferenceSource=Reference.Data.ReferenceSource;
    E.bReferenceOwnerVerified=Reference.Data.bReferenceOwnerVerified;E.ReferenceCitation=Reference.Data.ReferenceCitation;E.ReferenceSHA256=Reference.Data.ReferenceSHA256;E.ReferenceMethod=Reference.Data.ReferenceMethod;
    for(const auto& A:Actual.Data.Series)
    {
        const auto* R=Reference.Data.Series.FindByPredicate([&](const auto& V){return V.Id==A.Id;});if(!R)continue;
        if(A.Unit!=R->Unit||A.AbscissaUnit!=R->AbscissaUnit||A.AbscissaName!=R->AbscissaName){Error=TEXT("Original named metric/value/time conventions differ; explicit conversion is required before alignment.");return false;}
        if(A.MotionMode!=R->MotionMode||A.Normalization!=R->Normalization||A.SamplingConvention!=R->SamplingConvention)
        {Error=TEXT("Original metric motion/normalization/sampling conventions differ; their equivalence cannot be inferred. Extraction methods/windows remain independently identified in each original source.");return false;}
        FStudioHome4ReferenceSeries S=A;S.Abscissae.Reset();S.Actual.Reset();S.Reference.Reset();S.AbscissaEpoch=Actual.Epoch;S.AbsoluteTolerance=Absolute;S.RelativeTolerance=Relative;
        int32 J=0;
        for(int32 I=0;I<A.Abscissae.Num();++I)
        {
            const double X=A.Abscissae[I];if(Start&&(X<*Start||X>*End))continue;
            while(J<R->Abscissae.Num()&&R->Abscissae[J]<X)++J;
            if(J<R->Abscissae.Num()&&R->Abscissae[J]==X){S.Abscissae.Add(X);S.Actual.Add(A.Actual[I]);S.Reference.Add(R->Actual[J]);}
        }
        if(S.Abscissae.IsEmpty()){Error=TEXT("No exactly matched original samples exist in the selected window.");return false;}
        E.Series.Add(MoveTemp(S));
    }
    if(E.Series.IsEmpty()){Error=TEXT("Original actual/reference sources have no common named metrics.");return false;}
    FStudioHome4ReferenceExpectation Expected;Expected.RecipeId=E.RecipeId;
    if(!StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expected,E,Error))return false;
    E.bComposedAlignment=true;E.AlignmentPolicy=TEXT("exact_samples; no interpolation; identical declared epoch: ")+Actual.Epoch;
    E.ActualOriginalBytes=Actual.Data.OriginalBytes;E.ReferenceOriginalBytes=Reference.Data.OriginalBytes;
    E.ActualOriginalPath=Actual.Data.SourcePath;E.ReferenceOriginalPath=Reference.Data.SourcePath;E.ActualOriginalSHA256=Actual.Data.SourceSHA256;E.ReferenceOriginalSHA256=Reference.Data.SourceSHA256;
    E.AlignmentWindowStart=Start;E.AlignmentWindowEnd=End;
    const FString Document=StudioHome4Validation::SerializeEvidence(E);const FTCHARToUTF8 Bytes(*Document);E.OriginalBytes.Reset();E.OriginalBytes.Append(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());
    if(!StudioHome4ReferenceSourcesPrivate::Hash(E.OriginalBytes,E.SourceSHA256)){Error=TEXT("Aligned comparison hash could not be calculated.");return false;}
    Out=MoveTemp(E);Error.Empty();return true;
}
bool StudioHome4ReferenceSources::VerifyComposed(const FStudioHome4ReferenceEvidence& E,FString& Error)
{
    using namespace StudioHome4ReferenceSourcesPrivate;
    if(!E.bComposedAlignment)return true;
    FString ActualSHA,ReferenceSHA;
    if(E.ActualOriginalBytes.IsEmpty()||E.ReferenceOriginalBytes.IsEmpty()||E.ActualOriginalBytes.Num()>Max||E.ReferenceOriginalBytes.Num()>Max||!Hash(E.ActualOriginalBytes,ActualSHA)||!Hash(E.ReferenceOriginalBytes,ReferenceSHA)||
        ActualSHA!=E.ActualOriginalSHA256||ReferenceSHA!=E.ReferenceOriginalSHA256||!StudioHome4JSON::UTF8(E.ActualOriginalBytes.GetData(),E.ActualOriginalBytes.Num())||!StudioHome4JSON::UTF8(E.ReferenceOriginalBytes.GetData(),E.ReferenceOriginalBytes.Num()))
    {Error=TEXT("Composed comparison original source bytes/hashes are absent or changed.");return false;}
    auto Decode=[](const TArray<uint8>& B)
    {const int32 Offset=B.Num()>=3&&B[0]==0xef&&B[1]==0xbb&&B[2]==0xbf?3:0;const FUTF8ToTCHAR T(reinterpret_cast<const ANSICHAR*>(B.GetData()+Offset),B.Num()-Offset);return FString(T.Length(),T.Get());};
    FStudioHome4SeriesSource Actual,Reference;FStudioHome4ReferenceEvidence Rebuilt;
    if(E.Series.IsEmpty()||!Parse(Decode(E.ActualOriginalBytes),E.RecipeId,Actual,Error)||!Parse(Decode(E.ReferenceOriginalBytes),E.RecipeId,Reference,Error))return false;
    Actual.Data.OriginalBytes=E.ActualOriginalBytes;Reference.Data.OriginalBytes=E.ReferenceOriginalBytes;
    Actual.Data.SourcePath=E.ActualOriginalPath;Reference.Data.SourcePath=E.ReferenceOriginalPath;
    Actual.Data.SourceSHA256=ActualSHA;Reference.Data.SourceSHA256=ReferenceSHA;
    if(!AlignExact(Actual,Reference,E.Series[0].AbsoluteTolerance,E.Series[0].RelativeTolerance,E.AlignmentWindowStart,E.AlignmentWindowEnd,Rebuilt,Error)||
        StudioHome4Validation::SerializeEvidence(Rebuilt)!=StudioHome4Validation::SerializeEvidence(E))
    {Error=TEXT("Composed comparison no longer reproduces from exact original sources and declared alignment.");return false;}
    Error.Empty();return true;
}
