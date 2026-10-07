#include "StudioHome4Validation.h"
#include "StudioHome4JSON.h"
#include "StudioHome4RecipeGates.h"
#include "StudioHome4ReferenceSources.h"
#include "StudioFileDialog.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#define UI UI_HOME4_VALIDATION
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioHome4ValidationPrivate
{
    constexpr int64 MaxBytes = 8LL * 1024 * 1024;
    constexpr int32 MaxPoints = 100000, MaxTotalPoints = 200000;
    bool SHA256(const void* Bytes, int64 Size, FString& Hash)
    {
        EVP_MD_CTX* Context = EVP_MD_CTX_new();
        if (!Context) return false;
        uint8 Digest[32]; unsigned int Count = 0;
        const bool Valid = EVP_DigestInit_ex(Context, EVP_sha256(), nullptr) == 1 &&
            EVP_DigestUpdate(Context, Bytes, Size) == 1 && EVP_DigestFinal_ex(Context, Digest, &Count) == 1 && Count == 32;
        EVP_MD_CTX_free(Context);
        if (Valid) Hash = BytesToHex(Digest, Count).ToLower();
        return Valid;
    }
    TSharedPtr<FJsonValue> Field(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
    { return O ? O->TryGetField(Key) : nullptr; }
    bool Text(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FString& Out, int32 Max = 256)
    {
        const auto V = Field(O, Key);
        if (!V || V->Type != EJson::String) return false;
        Out = V->AsString();
        if (Out.TrimStartAndEnd().IsEmpty() || Out.Len() > Max) return false;
        for (TCHAR C : Out) if (C < 32||C==127) return false;
        return true;
    }
    bool Number(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double& Out, bool bNonnegative = false)
    {
        const auto V = Field(O, Key);
        return V && V->Type == EJson::Number && V->TryGetNumber(Out) && FMath::IsFinite(Out) && (!bNonnegative || Out >= 0);
    }
    bool Guid(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FGuid& Out)
    { FString S; return Text(O, Key, S, 64) && FGuid::Parse(S, Out) && Out.IsValid(); }
    bool Extraction(const TSharedPtr<FJsonObject>& Run,FStudioHome4ScalarExtraction& Out)
    {
        const auto V=Field(Run,TEXT("extraction"));if(!V||V->Type==EJson::Null)return true;
        if(V->Type!=EJson::Object)return false;const auto O=V->AsObject();
        const TSet<FString> Known={TEXT("window_start"),TEXT("window_end"),TEXT("abscissa_unit"),TEXT("epoch"),TEXT("method"),TEXT("source"),TEXT("source_sha256"),TEXT("epoch_confirmed_from_original")};
        for(const auto& E:O->Values)if(!Known.Contains(FString(*E.Key)))return false;
        auto OptionalNumber=[&](const TCHAR* K,TOptional<double>& N)
        {const auto F=Field(O,K);if(!F||F->Type==EJson::Null)return true;double D=0;if(!Number(O,K,D))return false;N=D;return true;};
        auto OptionalText=[&](const TCHAR* K,FString& S,int32 Max)
        {const auto F=Field(O,K);return !F||F->Type==EJson::Null||Text(O,K,S,Max);};
        if(!OptionalNumber(TEXT("window_start"),Out.WindowStart)||!OptionalNumber(TEXT("window_end"),Out.WindowEnd)||
            !OptionalText(TEXT("abscissa_unit"),Out.AbscissaUnit,96)||!OptionalText(TEXT("epoch"),Out.Epoch,256)||
            !OptionalText(TEXT("method"),Out.Method,256)||!OptionalText(TEXT("source"),Out.Source,2048)||
            !OptionalText(TEXT("source_sha256"),Out.SourceSHA256,64))return false;
        const auto EpochConfirmed=Field(O,TEXT("epoch_confirmed_from_original"));
        if(EpochConfirmed&&EpochConfirmed->Type!=EJson::Null&&(EpochConfirmed->Type!=EJson::Boolean||!EpochConfirmed->TryGetBool(Out.bEpochConfirmedFromOriginal)))return false;
        if(Out.bEpochConfirmedFromOriginal&&Out.Epoch.IsEmpty())return false;
        if(Out.WindowStart.IsSet()!=Out.WindowEnd.IsSet()||(Out.WindowStart&&(*Out.WindowEnd<=*Out.WindowStart||Out.AbscissaUnit.IsEmpty())))return false;
        if(!Out.SourceSHA256.IsEmpty())
        {
            if(Out.SourceSHA256.Len()!=64||Out.Source.IsEmpty())return false;
            for(TCHAR C:Out.SourceSHA256)if(!((C>='0'&&C<='9')||(C>='a'&&C<='f')||(C>='A'&&C<='F')))return false;
        }
        return true;
    }
    bool Array(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, TArray<double>& Out)
    {
        const auto V = Field(O, Key);
        if (!V || V->Type != EJson::Array || V->AsArray().IsEmpty() || V->AsArray().Num() > MaxPoints) return false;
        for (const auto& Item : V->AsArray())
        {
            double N = 0;
            if (Item->Type != EJson::Number || !Item->TryGetNumber(N) || !FMath::IsFinite(N)) return false;
            Out.Add(N);
        }
        return true;
    }
    TArray<TSharedPtr<FJsonValue>> JSONNumbers(const TArray<double>& Values)
    { TArray<TSharedPtr<FJsonValue>> Out; for (double V : Values) Out.Add(MakeShared<FJsonValueNumber>(V)); return Out; }
    bool FolderValid(const FString& Folder)
    {
        if (Folder.IsEmpty() || Folder.Len() > 100 || Folder == TEXT(".") || Folder == TEXT("..")) return false;
        for (TCHAR C : Folder) if (C < 32 || C == '/' || C == '\\' || C == ':') return false;
        return true;
    }
    bool Publish(const FString& Parent, const FString& Folder, TFunction<bool(const FString&, FString&)> Write,
        FString& OutPath, FString& Error)
    {
        if (!FolderValid(Folder)) { Error = TEXT("Use a new bundle folder name without path separators."); return false; }
        const FString Destination = Parent / Folder;
        if (IFileManager::Get().DirectoryExists(*Destination) || IFileManager::Get().FileExists(*Destination))
        { Error = TEXT("The bundle already exists; choose a new name to preserve original results."); return false; }
        FStudioFileAccess Access(Parent); FString Stage;
        if (!StudioFileDialog::CreateExportStage(Parent, Stage, Error)) return false;
        if (!Write(Stage, Error) || !StudioFileDialog::PublishExportDirectory(Stage, Destination, Error))
        { IFileManager::Get().DeleteDirectory(*Stage, false, true); return false; }
        OutPath = Destination; Error.Empty(); return true;
    }
    bool WriteJSON(const FString& Path, const TSharedRef<FJsonObject>& O, FString& Error)
    {
        FString JSON; FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&JSON));
        if (FFileHelper::SaveStringToFile(JSON, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return true;
        Error = TEXT("Could not write the evidence bundle."); return false;
    }
}

FString FStudioHome4ReferenceEvidence::ComparisonStatus() const
{
    if (Series.IsEmpty()) return TEXT("not_evaluated");
    for (const auto& S : Series) if (!S.Gate.bEvaluated) return TEXT("not_evaluated");
    for (const auto& S : Series) if (!S.Gate.bPassed) return TEXT("failed");
    return TEXT("passed");
}
FString FStudioHome4ReferenceEvidence::RecipeCoverage() const
{ return StudioHome4RecipeGates::Evaluate(*this).Status; }
FString FStudioHome4ReferenceEvidence::RecipeGateStatus() const
{
    const auto C=StudioHome4RecipeGates::Evaluate(*this);
    if(C.Status!=TEXT("complete")) return TEXT("not_evaluated");
    for(const auto& Id:C.Required)
    {
        const auto* S=Series.FindByPredicate([&](const auto& Item){return Item.Id==Id;});
        if(!S||!S->Gate.bEvaluated) return TEXT("not_evaluated");
        if(!S->Gate.bPassed) return TEXT("failed");
    }
    return TEXT("passed");
}
FString FStudioHome4ReferenceEvidence::GateStatus() const
{ return TEXT("Supplied-series comparisons: ")+ComparisonStatus()+TEXT(" · recipe gate ")+RecipeGateStatus()+TEXT(" (coverage ")+RecipeCoverage()+TEXT(")"); }
bool StudioHome4Validation::Parse(const FString& JSON, const FStudioHome4ReferenceExpectation& Expected,
    FStudioHome4ReferenceEvidence& Out, FString& Error)
{
    using namespace StudioHome4ValidationPrivate;
    const FTCHARToUTF8 Bytes(*JSON);
    auto Fail = [&](const TCHAR* Reason) { Error = Reason; return false; };
    if (Bytes.Length() <= 0 || Bytes.Length() > MaxBytes || !StudioHome4JSON::Preflight(JSON))
        return Fail(TEXT("Reference JSON is malformed, duplicated, non-finite, too deep or exceeds 8 MiB."));
    TSharedPtr<FJsonObject> O;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON), O) || !O) return Fail(TEXT("Reference evidence must be a JSON object."));
    FStudioHome4ReferenceEvidence E; FString Schema; double Version = 0;
    if (!Text(O, TEXT("schema"), Schema) || Schema != TEXT("LBMStudio.Home4Reference") ||
        !Number(O, TEXT("version"), Version) || Version != 1 || !Text(O, TEXT("recipe_id"), E.RecipeId) ||
        !Guid(O, TEXT("run_id"), E.RunId) || !Text(O, TEXT("actual_source"), E.ActualSource, 2048) ||
        !Text(O, TEXT("reference_source"), E.ReferenceSource, 2048)) return Fail(TEXT("Reference schema/version or recipe/run/source identity is invalid."));
    if (Expected.RecipeId.IsEmpty() || E.RecipeId != Expected.RecipeId ||
        (Expected.RunId && E.RunId != *Expected.RunId) ||
        (!Expected.ActualSource.IsEmpty() && E.ActualSource != Expected.ActualSource) ||
        (!Expected.ReferenceSource.IsEmpty() && E.ReferenceSource != Expected.ReferenceSource))
        return Fail(TEXT("Reference evidence does not match the selected recipe, run or explicit source identity."));
    const auto OriginalSpec=Field(O,TEXT("original_run_spec"));
    if(OriginalSpec&&OriginalSpec->Type!=EJson::Null)
    {
        FStudioHome4Spec Spec;
        if(OriginalSpec->Type!=EJson::Object||!StudioHome4Config::FromJSON(OriginalSpec->AsObject(),Spec,Error)||Spec.RecipeId!=E.RecipeId)
            return Fail(TEXT("Original run specification is invalid or belongs to another recipe."));
        E.OriginalRunSpec=MoveTemp(Spec);
    }
    const auto ReferenceMethod=Field(O,TEXT("reference_method"));
    if(ReferenceMethod&&ReferenceMethod->Type!=EJson::Null&&!Text(O,TEXT("reference_method"),E.ReferenceMethod,2048))return Fail(TEXT("Reference method must be an explicit bounded description."));
    const auto Verification=Field(O,TEXT("reference_verification"));
    if(Verification&&Verification->Type!=EJson::Null)
    {
        if(Verification->Type!=EJson::Object) return Fail(TEXT("Reference verification must be an explicit source object."));
        const auto V=Verification->AsObject(); const auto Flag=Field(V,TEXT("owner_verified"));
        if(!Flag||Flag->Type!=EJson::Boolean||!Flag->TryGetBool(E.bReferenceOwnerVerified)||
            !Text(V,TEXT("citation"),E.ReferenceCitation,2048)||!Text(V,TEXT("sha256"),E.ReferenceSHA256,64)||E.ReferenceSHA256.Len()!=64)
            return Fail(TEXT("Reference verification requires owner_verified, original citation and SHA256."));
        for(TCHAR C:E.ReferenceSHA256) if(!FChar::IsHexDigit(C)) return Fail(TEXT("Reference source SHA256 must be hexadecimal."));
    }
    const auto Series = Field(O, TEXT("series"));
    if (!Series || Series->Type != EJson::Array || Series->AsArray().IsEmpty() || Series->AsArray().Num() > 16)
        return Fail(TEXT("Supply 1–16 named aligned measurement/reference series."));
    TSet<FString> Ids; int32 Points = 0;
    for (const auto& V : Series->AsArray())
    {
        if (V->Type != EJson::Object) return Fail(TEXT("Each reference series must be an object."));
        const auto Item = V->AsObject(); FStudioHome4ReferenceSeries S;
        if (!Text(Item, TEXT("id"), S.Id) || !Text(Item, TEXT("name"), S.Name) ||
            !Text(Item, TEXT("x_name"), S.AbscissaName) || !Text(Item, TEXT("x_unit"), S.AbscissaUnit) ||
            !Text(Item, TEXT("unit"), S.Unit) || !Array(Item, TEXT("x"), S.Abscissae) ||
            !Array(Item, TEXT("actual"), S.Actual) || !Array(Item, TEXT("reference"), S.Reference) ||
            !Number(Item, TEXT("absolute_tolerance"), S.AbsoluteTolerance, true) ||
            !Number(Item, TEXT("relative_tolerance"), S.RelativeTolerance, true))
            return Fail(TEXT("Every series requires exact names/units, finite aligned arrays and explicit nonnegative tolerances."));
        const auto Epoch=Field(Item,TEXT("epoch"));
        if(Epoch&&Epoch->Type!=EJson::Null&&!Text(Item,TEXT("epoch"),S.AbscissaEpoch,256))return Fail(TEXT("Optional original series epoch must be an explicit bounded identity."));
        const auto Context=Field(Item,TEXT("metric_context"));
        if(Context&&Context->Type!=EJson::Null)
        {
            if(Context->Type!=EJson::Object)return Fail(TEXT("Original metric context must be an object."));
            const auto C=Context->AsObject();const TSet<FString> Known={TEXT("motion_mode"),TEXT("normalization"),TEXT("sampling_convention"),TEXT("extraction_method"),TEXT("window_start"),TEXT("window_end"),TEXT("window_unit"),TEXT("window_epoch")};
            for(const auto& Pair:C->Values)if(!Known.Contains(FString(*Pair.Key)))return Fail(TEXT("Unknown original metric qualification field."));
            auto OptionalText=[&](const TCHAR* K,FString& Value){const auto F=Field(C,K);return !F||F->Type==EJson::Null||Text(C,K,Value,1024);};
            if(!OptionalText(TEXT("motion_mode"),S.MotionMode)||!OptionalText(TEXT("normalization"),S.Normalization)||!OptionalText(TEXT("sampling_convention"),S.SamplingConvention)||!OptionalText(TEXT("extraction_method"),S.ExtractionMethod)||!OptionalText(TEXT("window_unit"),S.ExtractionWindowUnit)||!OptionalText(TEXT("window_epoch"),S.ExtractionEpoch))return Fail(TEXT("Metric qualification needs explicit bounded original text."));
            for(bool Start:{true,false})
            {const TCHAR* K=Start?TEXT("window_start"):TEXT("window_end");const auto F=Field(C,K);if(F&&F->Type!=EJson::Null){double N=0;if(!Number(C,K,N))return Fail(TEXT("Original metric extraction bounds must be finite."));(Start?S.ExtractionWindowStart:S.ExtractionWindowEnd)=N;}}
            if(S.ExtractionWindowStart.IsSet()!=S.ExtractionWindowEnd.IsSet()||(S.ExtractionWindowStart&&(*S.ExtractionWindowEnd<=*S.ExtractionWindowStart||S.ExtractionEpoch.IsEmpty()||S.ExtractionWindowUnit.IsEmpty()||S.ExtractionMethod.IsEmpty())))return Fail(TEXT("Original metric extraction requires paired increasing bounds and explicit original window units, epoch and method."));
        }
        if (Ids.Contains(S.Id) || S.Abscissae.Num() != S.Actual.Num() || S.Actual.Num() != S.Reference.Num())
            return Fail(TEXT("Series identities must be unique and x/actual/reference arrays must align exactly."));
        Ids.Add(S.Id); Points += S.Actual.Num();
        if (Points > MaxTotalPoints) return Fail(TEXT("Reference evidence exceeds its 200,000 aligned-point budget."));
        for (int32 I = 1; I < S.Abscissae.Num(); ++I) if (S.Abscissae[I] <= S.Abscissae[I - 1])
            return Fail(TEXT("Explicit sample abscissae must strictly increase; no ordering or interpolation is inferred."));
        for(double Reference:S.Reference)if(!FMath::IsFinite(S.AbsoluteTolerance+S.RelativeTolerance*FMath::Abs(Reference)))
            return Fail(TEXT("Tolerance arithmetic exceeds the finite numeric range."));
        S.Gate = StudioHome4Recipes::Compare(S.Actual, S.Reference, S.AbsoluteTolerance, S.RelativeTolerance, E.ReferenceSource);
        if (!S.Gate.bEvaluated || (S.Gate.RelativeL2Error&&!FMath::IsFinite(*S.Gate.RelativeL2Error))) return Fail(TEXT("Reference error calculation exceeds the finite numeric range."));
        E.Series.Add(MoveTemp(S));
    }
    const auto Order = Field(O, TEXT("order"));
    if (Order && Order->Type != EJson::Null)
    {
        if (Order->Type != EJson::Object) return Fail(TEXT("Observed-order evidence must be an object."));
        const auto Item = Order->AsObject();
        if (!Text(Item, TEXT("metric"), E.OrderMetric) || !Text(Item, TEXT("unit"), E.OrderUnit))
            return Fail(TEXT("Observed order needs an explicitly named scalar metric and unit."));
        bool Matched = false;
        for (const auto& S : E.Series) if (S.Id == E.OrderMetric && S.Unit == E.OrderUnit) Matched = true;
        if (!Matched) return Fail(TEXT("Observed-order scalar metric/unit must match an imported named series."));
        const auto Runs = Field(Item, TEXT("runs"));
        if (!Runs || Runs->Type != EJson::Array || Runs->AsArray().Num() != 3) return Fail(TEXT("Observed order requires exactly three explicitly identified scalar runs."));
        TSet<FGuid> RunIds;
        for (const auto& Value : Runs->AsArray())
        {
            if (Value->Type != EJson::Object) return Fail(TEXT("Observed-order runs must be objects."));
            FStudioHome4ScalarRun R;
            if (!Guid(Value->AsObject(), TEXT("run_id"), R.RunId) || !Number(Value->AsObject(), TEXT("refinement"), R.Refinement) ||
                R.Refinement <= 0 || !Number(Value->AsObject(), TEXT("value"), R.Value) || RunIds.Contains(R.RunId)||!Extraction(Value->AsObject(),R.Extraction))
                return Fail(TEXT("Observed-order runs require unique identities, finite scalars and valid explicit extraction metadata."));
            RunIds.Add(R.RunId); E.OrderRuns.Add(R);
        }
        const double A = E.OrderRuns[1].Refinement / E.OrderRuns[0].Refinement, B = E.OrderRuns[2].Refinement / E.OrderRuns[1].Refinement;
        if (!FMath::IsFinite(A) || !FMath::IsFinite(B) || A <= 1 || !FMath::IsNearlyEqual(A, B, 1e-12 * FMath::Max(A, B)))
            return Fail(TEXT("Observed order requires an increasing constant refinement ratio."));
        E.ObservedOrder = StudioHome4Recipes::ObservedOrder(E.OrderRuns[0].Value, E.OrderRuns[1].Value, E.OrderRuns[2].Value, A);
    }
    const auto Alignment=Field(O,TEXT("alignment"));
    if(Alignment&&Alignment->Type!=EJson::Null)
    {
        if(Alignment->Type!=EJson::Object)return Fail(TEXT("Alignment provenance must be an object."));
        const auto A=Alignment->AsObject();
        if(!Text(A,TEXT("policy"),E.AlignmentPolicy,2048)||!Text(A,TEXT("actual_sha256"),E.ActualOriginalSHA256,64)||!Text(A,TEXT("reference_sha256"),E.ReferenceOriginalSHA256,64)||E.ActualOriginalSHA256.Len()!=64||E.ReferenceOriginalSHA256.Len()!=64)
            return Fail(TEXT("Alignment provenance requires exact original source hashes and explicit policy."));
        for(TCHAR C:E.ActualOriginalSHA256+E.ReferenceOriginalSHA256)if(!FChar::IsHexDigit(C))return Fail(TEXT("Alignment source hashes must be hexadecimal."));
        const auto ActualPath=Field(A,TEXT("actual_path")),ReferencePath=Field(A,TEXT("reference_path"));
        if(ActualPath&&ActualPath->Type!=EJson::Null){if(ActualPath->Type!=EJson::String)return Fail(TEXT("Actual original path must be a string."));E.ActualOriginalPath=ActualPath->AsString();}
        if(ReferencePath&&ReferencePath->Type!=EJson::Null){if(ReferencePath->Type!=EJson::String)return Fail(TEXT("Reference original path must be a string."));E.ReferenceOriginalPath=ReferencePath->AsString();}
        for(TCHAR C:E.ActualOriginalPath+E.ReferenceOriginalPath)if(C<32||C==127)return Fail(TEXT("Original source paths contain control characters."));
        if(E.ActualOriginalPath.Len()>4096||E.ReferenceOriginalPath.Len()>4096)return Fail(TEXT("Alignment source path exceeds its identity budget."));
        double Start=0,End=0;const auto S=Field(A,TEXT("window_start")),T=Field(A,TEXT("window_end"));
        if(S&&S->Type!=EJson::Null){if(!Number(A,TEXT("window_start"),Start))return Fail(TEXT("Invalid alignment window start."));E.AlignmentWindowStart=Start;}
        if(T&&T->Type!=EJson::Null){if(!Number(A,TEXT("window_end"),End))return Fail(TEXT("Invalid alignment window end."));E.AlignmentWindowEnd=End;}
        if(E.AlignmentWindowStart.IsSet()!=E.AlignmentWindowEnd.IsSet()||(E.AlignmentWindowStart&&*E.AlignmentWindowEnd<=*E.AlignmentWindowStart))return Fail(TEXT("Alignment window bounds must be paired and increasing."));
        E.bComposedAlignment=true;
    }
    if (!SHA256(Bytes.Get(), Bytes.Length(), E.SourceSHA256)) return Fail(TEXT("Could not hash the original reference evidence."));
    E.OriginalBytes.Append(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());
    Out = MoveTemp(E); Error.Empty(); return true;
}
bool StudioHome4Validation::Load(const FString& Path, const FStudioHome4ReferenceExpectation& Expected,
    FStudioHome4ReferenceEvidence& Out, FString& Error)
{
    using namespace StudioHome4ValidationPrivate;
    FStudioFileAccess Access(Path);
    const int64 Size = IFileManager::Get().FileSize(*Path); const auto Timestamp = IFileManager::Get().GetTimeStamp(*Path);
    if (Size <= 0 || Size > MaxBytes) { Error = TEXT("Original reference file must contain between 1 byte and 8 MiB."); return false; }
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if(!File||File->TotalSize()!=Size){Error=TEXT("Original reference evidence is missing or changed.");return false;}
    TArray<uint8> Bytes;Bytes.SetNumUninitialized(int32(Size));File->Serialize(Bytes.GetData(),Size);
    if(File->IsError()||!StudioHome4JSON::UTF8(Bytes.GetData(),Bytes.Num())){Error=TEXT("Original reference must be valid UTF-8 JSON.");return false;}
    if(!File->Close()){Error=TEXT("Original reference read failed while closing.");return false;}
    File.Reset();
    const int32 Offset=Bytes.Num()>=3&&Bytes[0]==0xef&&Bytes[1]==0xbb&&Bytes[2]==0xbf?3:0;
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+Offset),Bytes.Num()-Offset);
    const FString JSON(Converted.Length(),Converted.Get());
    FStudioHome4ReferenceEvidence Candidate;
    if (!Parse(JSON, Expected, Candidate, Error)) return false;
    if (!SHA256(Bytes.GetData(), Bytes.Num(), Candidate.SourceSHA256)) { Error = TEXT("Could not verify original evidence identity."); return false; }
    if (IFileManager::Get().FileSize(*Path) != Size || IFileManager::Get().GetTimeStamp(*Path) != Timestamp)
    { Error = TEXT("Original reference evidence changed while reading."); return false; }
    TUniquePtr<FArchive> Verify(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if(!Verify||Verify->TotalSize()!=Size){Error=TEXT("Original reference evidence changed while reading.");return false;}
    TArray<uint8> Checked;Checked.SetNumUninitialized(int32(Size));Verify->Serialize(Checked.GetData(),Size);
    const bool ReadError=Verify->IsError();const bool Closed=Verify->Close();Verify.Reset();
    if(ReadError||!Closed||Checked!=Bytes||IFileManager::Get().FileSize(*Path)!=Size||IFileManager::Get().GetTimeStamp(*Path)!=Timestamp)
    {Error=TEXT("Original reference evidence changed while reading; previous evidence retained.");return false;}
    if(Candidate.bComposedAlignment){Error=TEXT("A derived alignment document cannot replace its original independent sources. Select the actual and reference originals and align them explicitly.");return false;}
    Candidate.OriginalBytes=MoveTemp(Bytes);
    Candidate.SourcePath = FPaths::ConvertRelativePathToFull(Path); Out = MoveTemp(Candidate); return true;
}
TSharedRef<FJsonObject> StudioHome4Validation::MetricContext(const FStudioHome4ReferenceSeries& S)
{
    auto O=MakeShared<FJsonObject>();
    for(const auto& P:TArray<TPair<FString,FString>>{{TEXT("motion_mode"),S.MotionMode},{TEXT("normalization"),S.Normalization},{TEXT("sampling_convention"),S.SamplingConvention},{TEXT("extraction_method"),S.ExtractionMethod},{TEXT("window_unit"),S.ExtractionWindowUnit},{TEXT("window_epoch"),S.ExtractionEpoch}})
        if(!P.Value.IsEmpty())O->SetStringField(P.Key,P.Value);
    if(S.ExtractionWindowStart)O->SetNumberField(TEXT("window_start"),*S.ExtractionWindowStart);
    if(S.ExtractionWindowEnd)O->SetNumberField(TEXT("window_end"),*S.ExtractionWindowEnd);
    return O;
}
TSharedRef<FJsonObject> StudioHome4Validation::EvidenceMetadata(const FStudioHome4ReferenceEvidence& E)
{
    auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("recipe_id"), E.RecipeId); O->SetStringField(TEXT("run_id"), E.RunId.ToString());
    O->SetStringField(TEXT("actual_source"), E.ActualSource); O->SetStringField(TEXT("reference_source"), E.ReferenceSource);
    O->SetStringField(TEXT("original_path"), E.SourcePath); O->SetStringField(TEXT("original_sha256"), E.SourceSHA256);
    O->SetStringField(TEXT("comparison_status"), E.ComparisonStatus());
    O->SetStringField(TEXT("gate_status"), E.RecipeGateStatus());
    O->SetStringField(TEXT("recipe_coverage"), E.RecipeCoverage());
    O->SetStringField(TEXT("coverage_reason"), StudioHome4RecipeGates::Evaluate(E).Reason);
    O->SetBoolField(TEXT("reference_owner_verified"),E.bReferenceOwnerVerified);O->SetStringField(TEXT("reference_citation"),E.ReferenceCitation);O->SetStringField(TEXT("reference_sha256"),E.ReferenceSHA256);O->SetStringField(TEXT("reference_method"),E.ReferenceMethod);
    O->SetNumberField(TEXT("original_bytes"),E.OriginalBytes.Num());
    O->SetBoolField(TEXT("composed_from_independent_original_sources"),E.bComposedAlignment);
    if(E.bComposedAlignment)
    {
        auto A=MakeShared<FJsonObject>();A->SetStringField(TEXT("policy"),E.AlignmentPolicy);
        A->SetStringField(TEXT("actual_path"),E.ActualOriginalPath);A->SetStringField(TEXT("reference_path"),E.ReferenceOriginalPath);
        A->SetStringField(TEXT("actual_sha256"),E.ActualOriginalSHA256);A->SetStringField(TEXT("reference_sha256"),E.ReferenceOriginalSHA256);
        A->SetNumberField(TEXT("actual_original_bytes"),E.ActualOriginalBytes.Num());A->SetNumberField(TEXT("reference_original_bytes"),E.ReferenceOriginalBytes.Num());
        if(E.AlignmentWindowStart){A->SetNumberField(TEXT("window_start"),*E.AlignmentWindowStart);A->SetNumberField(TEXT("window_end"),*E.AlignmentWindowEnd);}
        O->SetObjectField(TEXT("alignment"),A);
    }
    if(E.OriginalRunSpec) O->SetObjectField(TEXT("original_run_spec"),StudioHome4Config::ToJSON(*E.OriginalRunSpec));
    else O->SetField(TEXT("original_run_spec"),MakeShared<FJsonValueNull>());
    TArray<TSharedPtr<FJsonValue>> Series;
    for (const auto& S : E.Series)
    {
        auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("id"), S.Id); Item->SetStringField(TEXT("name"), S.Name);
        Item->SetStringField(TEXT("unit"), S.Unit); Item->SetStringField(TEXT("x_name"), S.AbscissaName); Item->SetStringField(TEXT("x_unit"), S.AbscissaUnit);
        if(!S.AbscissaEpoch.IsEmpty())Item->SetStringField(TEXT("epoch"),S.AbscissaEpoch);
        Item->SetObjectField(TEXT("metric_context"),MetricContext(S));
        Item->SetNumberField(TEXT("points"), S.Actual.Num()); Item->SetNumberField(TEXT("absolute_tolerance"), S.AbsoluteTolerance); Item->SetNumberField(TEXT("relative_tolerance"), S.RelativeTolerance);
        Item->SetStringField(TEXT("comparison_status"), !S.Gate.bEvaluated ? TEXT("not_evaluated") : S.Gate.bPassed ? TEXT("passed") : TEXT("failed"));
        Item->SetStringField(TEXT("reason"), S.Gate.Reason);
        if (S.Gate.MaximumAbsoluteError) Item->SetNumberField(TEXT("max_absolute_error"), *S.Gate.MaximumAbsoluteError);
        if (S.Gate.RelativeL2Error) Item->SetNumberField(TEXT("relative_l2_error"), *S.Gate.RelativeL2Error);
        Series.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("series"), Series);
    TArray<TSharedPtr<FJsonValue>> Runs;
    for(const auto& R:E.OrderRuns)Runs.Add(MakeShared<FJsonValueObject>(ScalarRunMetadata(R)));
    O->SetArrayField(TEXT("order_runs"),Runs);O->SetStringField(TEXT("scalar_window_equivalence"),TEXT("not_inferred"));
    if (E.ObservedOrder) { O->SetNumberField(TEXT("observed_order"), *E.ObservedOrder); O->SetStringField(TEXT("order_metric"), E.OrderMetric); }
    return O;
}
bool StudioHome4Validation::VerifyOriginalBytes(const FStudioHome4ReferenceEvidence& E,FString& Error)
{
    using namespace StudioHome4ValidationPrivate;FString Hash;
    if(E.OriginalBytes.IsEmpty()||E.OriginalBytes.Num()>MaxBytes||!StudioHome4JSON::UTF8(E.OriginalBytes.GetData(),E.OriginalBytes.Num())||
        !SHA256(E.OriginalBytes.GetData(),E.OriginalBytes.Num(),Hash)||Hash!=E.SourceSHA256)
    {Error=TEXT("Original evidence bytes are absent, invalid or differ from their retained source SHA256.");return false;}
    const int32 Offset=E.OriginalBytes.Num()>=3&&E.OriginalBytes[0]==0xef&&E.OriginalBytes[1]==0xbb&&E.OriginalBytes[2]==0xbf?3:0;
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(E.OriginalBytes.GetData()+Offset),E.OriginalBytes.Num()-Offset);
    FStudioHome4ReferenceEvidence Parsed;FStudioHome4ReferenceExpectation Expected;Expected.RecipeId=E.RecipeId;Expected.RunId=E.RunId;Expected.ActualSource=E.ActualSource;Expected.ReferenceSource=E.ReferenceSource;
    if(!Parse(FString(Converted.Length(),Converted.Get()),Expected,Parsed,Error)||SerializeEvidence(Parsed)!=SerializeEvidence(E))
    {Error=TEXT("Retained evidence no longer matches its exact original source bytes.");return false;}
    if(!StudioHome4ReferenceSources::VerifyComposed(E,Error))return false;
    Error.Empty();return true;
}
FString StudioHome4Validation::SerializeEvidence(const FStudioHome4ReferenceEvidence& E)
{
    using namespace StudioHome4ValidationPrivate;
    auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("schema"), TEXT("LBMStudio.Home4Reference")); O->SetNumberField(TEXT("version"), 1);
    O->SetStringField(TEXT("recipe_id"), E.RecipeId); O->SetStringField(TEXT("run_id"), E.RunId.ToString());
    O->SetStringField(TEXT("actual_source"), E.ActualSource); O->SetStringField(TEXT("reference_source"), E.ReferenceSource);
    if(E.OriginalRunSpec) O->SetObjectField(TEXT("original_run_spec"),StudioHome4Config::ToJSON(*E.OriginalRunSpec));
    if(!E.ReferenceMethod.IsEmpty())O->SetStringField(TEXT("reference_method"),E.ReferenceMethod);
    if(!E.ReferenceCitation.IsEmpty()||!E.ReferenceSHA256.IsEmpty())
    {
        auto V=MakeShared<FJsonObject>();V->SetBoolField(TEXT("owner_verified"),E.bReferenceOwnerVerified);
        V->SetStringField(TEXT("citation"),E.ReferenceCitation);V->SetStringField(TEXT("sha256"),E.ReferenceSHA256);O->SetObjectField(TEXT("reference_verification"),V);
    }
    if(E.bComposedAlignment)
    {
        auto A=MakeShared<FJsonObject>();A->SetStringField(TEXT("policy"),E.AlignmentPolicy);A->SetStringField(TEXT("actual_path"),E.ActualOriginalPath);A->SetStringField(TEXT("reference_path"),E.ReferenceOriginalPath);
        A->SetStringField(TEXT("actual_sha256"),E.ActualOriginalSHA256);A->SetStringField(TEXT("reference_sha256"),E.ReferenceOriginalSHA256);
        if(E.AlignmentWindowStart){A->SetNumberField(TEXT("window_start"),*E.AlignmentWindowStart);A->SetNumberField(TEXT("window_end"),*E.AlignmentWindowEnd);}
        O->SetObjectField(TEXT("alignment"),A);
    }
    TArray<TSharedPtr<FJsonValue>> Series;
    for (const auto& S : E.Series)
    {
        auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("id"), S.Id); Item->SetStringField(TEXT("name"), S.Name);
        Item->SetStringField(TEXT("x_name"), S.AbscissaName); Item->SetStringField(TEXT("x_unit"), S.AbscissaUnit); Item->SetStringField(TEXT("unit"), S.Unit);
        if(!S.AbscissaEpoch.IsEmpty())Item->SetStringField(TEXT("epoch"),S.AbscissaEpoch);
        Item->SetObjectField(TEXT("metric_context"),MetricContext(S));
        Item->SetArrayField(TEXT("x"), JSONNumbers(S.Abscissae)); Item->SetArrayField(TEXT("actual"), JSONNumbers(S.Actual)); Item->SetArrayField(TEXT("reference"), JSONNumbers(S.Reference));
        Item->SetNumberField(TEXT("absolute_tolerance"), S.AbsoluteTolerance); Item->SetNumberField(TEXT("relative_tolerance"), S.RelativeTolerance);
        Series.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("series"), Series);
    if (!E.OrderRuns.IsEmpty())
    {
        auto Order = MakeShared<FJsonObject>(); Order->SetStringField(TEXT("metric"), E.OrderMetric); Order->SetStringField(TEXT("unit"), E.OrderUnit);
        TArray<TSharedPtr<FJsonValue>> Runs;
        for (const auto& R : E.OrderRuns)
        { Runs.Add(MakeShared<FJsonValueObject>(ScalarRunMetadata(R))); }
        Order->SetArrayField(TEXT("runs"), Runs); O->SetObjectField(TEXT("order"), Order);
    }
    FString JSON; FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&JSON)); return JSON;
}
TSharedRef<FJsonObject> StudioHome4Validation::ScalarRunMetadata(const FStudioHome4ScalarRun& R)
{
    auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("run_id"),R.RunId.ToString());O->SetNumberField(TEXT("refinement"),R.Refinement);O->SetNumberField(TEXT("value"),R.Value);
    auto X=MakeShared<FJsonObject>();const auto& E=R.Extraction;
    if(E.WindowStart)X->SetNumberField(TEXT("window_start"),*E.WindowStart);else X->SetField(TEXT("window_start"),MakeShared<FJsonValueNull>());
    if(E.WindowEnd)X->SetNumberField(TEXT("window_end"),*E.WindowEnd);else X->SetField(TEXT("window_end"),MakeShared<FJsonValueNull>());
    auto String=[&](const TCHAR* K,const FString& V){if(V.IsEmpty())X->SetField(K,MakeShared<FJsonValueNull>());else X->SetStringField(K,V);};
    String(TEXT("abscissa_unit"),E.AbscissaUnit);String(TEXT("epoch"),E.Epoch);String(TEXT("method"),E.Method);String(TEXT("source"),E.Source);String(TEXT("source_sha256"),E.SourceSHA256);
    X->SetBoolField(TEXT("epoch_confirmed_from_original"),E.bEpochConfirmedFromOriginal);
    O->SetObjectField(TEXT("extraction"),X);return O;
}
FString StudioHome4Validation::ScalarRunDescription(const FStudioHome4ScalarRun& R)
{
    const auto& E=R.Extraction;auto Known=[](const FString& V){return V.IsEmpty()?FString(TEXT("unknown")):V;};
    const FString Window=E.WindowStart&&E.WindowEnd?FString::Printf(TEXT("%.17g to %.17g %s"),*E.WindowStart,*E.WindowEnd,*E.AbscissaUnit):FString(TEXT("unknown"));
    return FString::Printf(TEXT("Refinement %.17g · value %.17g · run %s\nExtraction window %s · epoch %s · method %s\nOriginal scalar source %s · SHA256 %s · epoch confirmation %s"),
        R.Refinement,R.Value,*R.RunId.ToString(),*Window,*Known(E.Epoch),*Known(E.Method),*Known(E.Source),*Known(E.SourceSHA256),E.bEpochConfirmedFromOriginal?TEXT("matched exact original epoch"):TEXT("owner declared; original epoch unknown"));
}
bool StudioHome4Validation::ExportEvidence(const FString& Parent, const FString& Folder, const FStudioHome4ReferenceEvidence& E,
    FString& OutPath, FString& Error)
{
    using namespace StudioHome4ValidationPrivate;
    if (E.SourceSHA256.Len() != 64 || E.ComparisonStatus() == TEXT("not_evaluated")) { Error = TEXT("Identified imported reference evidence is required."); return false; }
    if(!E.OriginalBytes.IsEmpty()&&!VerifyOriginalBytes(E,Error))return false;
    return Publish(Parent, Folder, [&E](const FString& Stage, FString& Failure)
    {
        if (!WriteJSON(Stage / TEXT("evidence.json"), EvidenceMetadata(E), Failure)) return false;
        if (!FFileHelper::SaveStringToFile(SerializeEvidence(E), *(Stage / TEXT("aligned_reference.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        { Failure = TEXT("Could not write aligned reference measurements."); return false; }
        if(!E.OriginalBytes.IsEmpty()&&!FFileHelper::SaveArrayToFile(E.OriginalBytes,*(Stage/TEXT("original_reference.json"))))
        {Failure=TEXT("Could not retain exact original reference bytes.");return false;}
        if(E.bComposedAlignment&&(!FFileHelper::SaveArrayToFile(E.ActualOriginalBytes,*(Stage/TEXT("original_actual_series.json")))||!FFileHelper::SaveArrayToFile(E.ReferenceOriginalBytes,*(Stage/TEXT("original_reference_series.json")))))
        {Failure=TEXT("Could not retain independent exact original sources.");return false;}
        return true;
    }, OutPath, Error);
}
bool StudioHome4Validation::ExportLadder(const FString& Parent, const FString& Folder, const TArray<FStudioHome4LadderRung>& Ladder,
    FString& OutPath, FString& Error)
{
    using namespace StudioHome4ValidationPrivate;
    if (Ladder.IsEmpty() || Ladder.Num() > 12) { Error = TEXT("Build an explicit bounded refinement ladder first."); return false; }
    TSet<FGuid> Runs; int32 Previous = 0;
    for (const auto& R : Ladder)
    {
        if (!R.PlannedRunId.IsValid() || Runs.Contains(R.PlannedRunId) || R.Refinement <= Previous || R.Refinement > 64 ||
            R.Spec.LineageId.IsEmpty() || R.Spec.LineageId != Ladder[0].Spec.LineageId || !StudioHome4Config::Validate(R.Spec, Error) ||
            (R.EstimatedSeconds && (!FMath::IsFinite(*R.EstimatedSeconds) || *R.EstimatedSeconds < 0)))
        { Error = TEXT("Ladder requires increasing factors, one lineage and valid unique planned run identities/specifications."); return false; }
        Runs.Add(R.PlannedRunId); Previous = R.Refinement;
    }
    return Publish(Parent, Folder, [&Ladder](const FString& Stage, FString& Failure)
    {
        auto Manifest = MakeShared<FJsonObject>(); Manifest->SetStringField(TEXT("schema"), TEXT("LBMStudio.Home4DevelopmentLadder")); Manifest->SetNumberField(TEXT("version"), 1);
        Manifest->SetBoolField(TEXT("launched"), false); Manifest->SetStringField(TEXT("lineage_id"), Ladder[0].Spec.LineageId);
        TArray<TSharedPtr<FJsonValue>> Queue;
        for (const auto& R : Ladder)
        {
            const FString File = FString::Printf(TEXT("run_spec_r%d.json"), R.Refinement);
            if (!WriteJSON(Stage / File, StudioHome4Config::ToJSON(R.Spec), Failure)) return false;
            auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("planned_run_id"), R.PlannedRunId.ToString()); Item->SetNumberField(TEXT("refinement"), R.Refinement);
            Item->SetStringField(TEXT("run_spec"), File); Item->SetStringField(TEXT("tag"), R.Spec.Run.Tag); Item->SetStringField(TEXT("cost_basis"), R.CostBasis);
            if (R.Cells) Item->SetStringField(TEXT("cells_exact"), LexToString(*R.Cells));
            if (R.AllocationBytes) Item->SetStringField(TEXT("allocation_bytes_exact"), LexToString(*R.AllocationBytes));
            if (R.EstimatedSeconds) Item->SetNumberField(TEXT("estimated_seconds"), *R.EstimatedSeconds);
            Queue.Add(MakeShared<FJsonValueObject>(Item));
        }
        Manifest->SetArrayField(TEXT("development_queue"), Queue); return WriteJSON(Stage / TEXT("ladder.json"), Manifest, Failure);
    }, OutPath, Error);
}
