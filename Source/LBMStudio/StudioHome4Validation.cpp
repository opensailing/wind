#include "StudioHome4Validation.h"
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
    bool Preflight(const FString& JSON)
    {
        const auto Reader = TJsonReaderFactory<>::Create(JSON);
        EJsonNotation Token;
        TArray<bool> Containers;
        TArray<TSet<FString>> Keys;
        while (Reader->ReadNext(Token))
        {
            if (Token == EJsonNotation::Error) return false;
            if (Token == EJsonNotation::Number && !FMath::IsFinite(Reader->GetValueAsNumber())) return false;
            if (Token != EJsonNotation::ObjectEnd && Token != EJsonNotation::ArrayEnd && !Containers.IsEmpty() && Containers.Last())
            {
                const FString Key = Reader->GetIdentifier();
                if (Keys.Last().Contains(Key)) return false;
                Keys.Last().Add(Key);
            }
            if (Token == EJsonNotation::ObjectStart || Token == EJsonNotation::ArrayStart)
            {
                if (Containers.Num() >= 32) return false;
                const bool Object = Token == EJsonNotation::ObjectStart;
                Containers.Add(Object); if (Object) Keys.Add(TSet<FString>());
            }
            else if (Token == EJsonNotation::ObjectEnd || Token == EJsonNotation::ArrayEnd)
            {
                if (Containers.IsEmpty()) return false;
                if (Containers.Last()) Keys.Pop(EAllowShrinking::No);
                Containers.Pop(EAllowShrinking::No);
            }
        }
        return Containers.IsEmpty();
    }
    bool UTF8(const uint8* Bytes, int32 Size)
    {
        for(int32 I=0;I<Size;)
        {
            const uint8 C=Bytes[I++];if(C<0x80){if(!C)return false;continue;}
            int32 N=0;uint32 Code=0,Minimum=0;
            if(C>=0xc2&&C<=0xdf){N=1;Code=C&31;Minimum=0x80;}
            else if(C>=0xe0&&C<=0xef){N=2;Code=C&15;Minimum=0x800;}
            else if(C>=0xf0&&C<=0xf4){N=3;Code=C&7;Minimum=0x10000;}else return false;
            if(Size-I<N)return false;
            for(int32 J=0;J<N;++J){const uint8 Next=Bytes[I++];if((Next&0xc0)!=0x80)return false;Code=(Code<<6)|(Next&63);}
            if(Code<Minimum||Code>0x10ffff||(Code>=0xd800&&Code<=0xdfff))return false;
        }
        return true;
    }
    TSharedPtr<FJsonValue> Field(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
    { return O ? O->TryGetField(Key) : nullptr; }
    bool Text(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FString& Out, int32 Max = 256)
    {
        const auto V = Field(O, Key);
        if (!V || V->Type != EJson::String) return false;
        Out = V->AsString();
        if (Out.TrimStartAndEnd().IsEmpty() || Out.Len() > Max) return false;
        for (TCHAR C : Out) if (C < 32) return false;
        return true;
    }
    bool Number(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double& Out, bool bNonnegative = false)
    {
        const auto V = Field(O, Key);
        return V && V->Type == EJson::Number && V->TryGetNumber(Out) && FMath::IsFinite(Out) && (!bNonnegative || Out >= 0);
    }
    bool Guid(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FGuid& Out)
    { FString S; return Text(O, Key, S, 64) && FGuid::Parse(S, Out) && Out.IsValid(); }
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

FString FStudioHome4ReferenceEvidence::GateStatus() const
{
    if (Series.IsEmpty()) return TEXT("not_evaluated");
    for (const auto& S : Series) if (!S.Gate.bEvaluated) return TEXT("not_evaluated");
    for (const auto& S : Series) if (!S.Gate.bPassed) return TEXT("failed");
    return TEXT("passed");
}
bool StudioHome4Validation::Parse(const FString& JSON, const FStudioHome4ReferenceExpectation& Expected,
    FStudioHome4ReferenceEvidence& Out, FString& Error)
{
    using namespace StudioHome4ValidationPrivate;
    const FTCHARToUTF8 Bytes(*JSON);
    auto Fail = [&](const TCHAR* Reason) { Error = Reason; return false; };
    if (Bytes.Length() <= 0 || Bytes.Length() > MaxBytes || !Preflight(JSON))
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
                R.Refinement <= 0 || !Number(Value->AsObject(), TEXT("value"), R.Value) || RunIds.Contains(R.RunId))
                return Fail(TEXT("Observed-order runs require unique identities and finite scalar values/refinements."));
            RunIds.Add(R.RunId); E.OrderRuns.Add(R);
        }
        const double A = E.OrderRuns[1].Refinement / E.OrderRuns[0].Refinement, B = E.OrderRuns[2].Refinement / E.OrderRuns[1].Refinement;
        if (!FMath::IsFinite(A) || !FMath::IsFinite(B) || A <= 1 || !FMath::IsNearlyEqual(A, B, 1e-12 * FMath::Max(A, B)))
            return Fail(TEXT("Observed order requires an increasing constant refinement ratio."));
        E.ObservedOrder = StudioHome4Recipes::ObservedOrder(E.OrderRuns[0].Value, E.OrderRuns[1].Value, E.OrderRuns[2].Value, A);
    }
    if (!SHA256(Bytes.Get(), Bytes.Length(), E.SourceSHA256)) return Fail(TEXT("Could not hash the original reference evidence."));
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
    if(File->IsError()||!UTF8(Bytes.GetData(),Bytes.Num())){Error=TEXT("Original reference must be valid UTF-8 JSON.");return false;}
    const int32 Offset=Bytes.Num()>=3&&Bytes[0]==0xef&&Bytes[1]==0xbb&&Bytes[2]==0xbf?3:0;
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+Offset),Bytes.Num()-Offset);
    const FString JSON(Converted.Length(),Converted.Get());
    FStudioHome4ReferenceEvidence Candidate;
    if (!Parse(JSON, Expected, Candidate, Error)) return false;
    if (!SHA256(Bytes.GetData(), Bytes.Num(), Candidate.SourceSHA256)) { Error = TEXT("Could not verify original evidence identity."); return false; }
    if (IFileManager::Get().FileSize(*Path) != Size || IFileManager::Get().GetTimeStamp(*Path) != Timestamp)
    { Error = TEXT("Original reference evidence changed while reading."); return false; }
    Candidate.SourcePath = FPaths::ConvertRelativePathToFull(Path); Out = MoveTemp(Candidate); return true;
}
TSharedRef<FJsonObject> StudioHome4Validation::EvidenceMetadata(const FStudioHome4ReferenceEvidence& E)
{
    auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("recipe_id"), E.RecipeId); O->SetStringField(TEXT("run_id"), E.RunId.ToString());
    O->SetStringField(TEXT("actual_source"), E.ActualSource); O->SetStringField(TEXT("reference_source"), E.ReferenceSource);
    O->SetStringField(TEXT("original_path"), E.SourcePath); O->SetStringField(TEXT("original_sha256"), E.SourceSHA256);
    O->SetStringField(TEXT("gate_status"), E.GateStatus());
    TArray<TSharedPtr<FJsonValue>> Series;
    for (const auto& S : E.Series)
    {
        auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("id"), S.Id); Item->SetStringField(TEXT("name"), S.Name);
        Item->SetStringField(TEXT("unit"), S.Unit); Item->SetStringField(TEXT("x_name"), S.AbscissaName); Item->SetStringField(TEXT("x_unit"), S.AbscissaUnit);
        Item->SetNumberField(TEXT("points"), S.Actual.Num()); Item->SetNumberField(TEXT("absolute_tolerance"), S.AbsoluteTolerance); Item->SetNumberField(TEXT("relative_tolerance"), S.RelativeTolerance);
        Item->SetStringField(TEXT("gate_status"), !S.Gate.bEvaluated ? TEXT("not_evaluated") : S.Gate.bPassed ? TEXT("passed") : TEXT("failed"));
        Item->SetStringField(TEXT("reason"), S.Gate.Reason);
        if (S.Gate.MaximumAbsoluteError) Item->SetNumberField(TEXT("max_absolute_error"), *S.Gate.MaximumAbsoluteError);
        if (S.Gate.RelativeL2Error) Item->SetNumberField(TEXT("relative_l2_error"), *S.Gate.RelativeL2Error);
        Series.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("series"), Series);
    if (E.ObservedOrder) { O->SetNumberField(TEXT("observed_order"), *E.ObservedOrder); O->SetStringField(TEXT("order_metric"), E.OrderMetric); }
    return O;
}
FString StudioHome4Validation::SerializeEvidence(const FStudioHome4ReferenceEvidence& E)
{
    using namespace StudioHome4ValidationPrivate;
    auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("schema"), TEXT("LBMStudio.Home4Reference")); O->SetNumberField(TEXT("version"), 1);
    O->SetStringField(TEXT("recipe_id"), E.RecipeId); O->SetStringField(TEXT("run_id"), E.RunId.ToString());
    O->SetStringField(TEXT("actual_source"), E.ActualSource); O->SetStringField(TEXT("reference_source"), E.ReferenceSource);
    TArray<TSharedPtr<FJsonValue>> Series;
    for (const auto& S : E.Series)
    {
        auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("id"), S.Id); Item->SetStringField(TEXT("name"), S.Name);
        Item->SetStringField(TEXT("x_name"), S.AbscissaName); Item->SetStringField(TEXT("x_unit"), S.AbscissaUnit); Item->SetStringField(TEXT("unit"), S.Unit);
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
        { auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("run_id"), R.RunId.ToString()); Item->SetNumberField(TEXT("refinement"), R.Refinement); Item->SetNumberField(TEXT("value"), R.Value); Runs.Add(MakeShared<FJsonValueObject>(Item)); }
        Order->SetArrayField(TEXT("runs"), Runs); O->SetObjectField(TEXT("order"), Order);
    }
    FString JSON; FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&JSON)); return JSON;
}
bool StudioHome4Validation::ExportEvidence(const FString& Parent, const FString& Folder, const FStudioHome4ReferenceEvidence& E,
    FString& OutPath, FString& Error)
{
    using namespace StudioHome4ValidationPrivate;
    if (E.SourceSHA256.Len() != 64 || E.GateStatus() == TEXT("not_evaluated")) { Error = TEXT("Identified imported reference evidence is required."); return false; }
    return Publish(Parent, Folder, [&E](const FString& Stage, FString& Failure)
    {
        if (!WriteJSON(Stage / TEXT("evidence.json"), EvidenceMetadata(E), Failure)) return false;
        if (!FFileHelper::SaveStringToFile(SerializeEvidence(E), *(Stage / TEXT("aligned_reference.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        { Failure = TEXT("Could not write aligned reference measurements."); return false; }
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
