#include "StudioHome4Checkpoint.h"
#include "StudioHome4ArchivePrivate.h"
#include "StudioHome4Session.h"
#include "StudioFileDialog.h"
#include "StudioModel.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformFile.h"
#include "Misc/Paths.h"
#define UI UI_HOME4_CHECKPOINT
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioHome4CheckpointPrivate
{
    FString Identity(const FStudioHome4Spec& Spec)
    {
        const FTCHARToUTF8 Bytes(*StudioHome4Config::Serialize(Spec));
        uint8 Digest[32]; unsigned int Count = 0;
        if (EVP_Digest(Bytes.Get(), Bytes.Length(), Digest, &Count, EVP_sha256(), nullptr) != 1 || Count != 32) return {};
        return BytesToHex(Digest, Count).ToLower();
    }
    bool Strict(const FJsonObject& Object, std::initializer_list<const TCHAR*> Keys)
    {
        for (const auto& Pair : Object.Values)
        {
            bool bKnown = false; for (const auto* Key : Keys) if (Pair.Key == Key) bKnown = true;
            if (!bKnown) return false;
        }
        return true;
    }
    bool Dimensions(const FJsonObject& Object, const TCHAR* Key, FIntVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.TryGetArrayField(Key, Values) || Values->Num() != 3) return false;
        FIntVector Candidate;
        for (int32 I = 0; I < 3; ++I)
        {
            double N = 0; if (!(*Values)[I] || (*Values)[I]->Type != EJson::Number || !(*Values)[I]->TryGetNumber(N) ||
                !FMath::IsFinite(N) || N < 1 || N > 1048576 || N != FMath::FloorToDouble(N)) return false;
            Candidate[I] = int32(N);
        }
        Out = Candidate; return true;
    }
    bool Axes(const FString& Value)
    { return Value == TEXT("xyz") || Value == TEXT("xzy") || Value == TEXT("yxz") || Value == TEXT("yzx") || Value == TEXT("zxy") || Value == TEXT("zyx"); }
    bool Origin(const FJsonObject& Object, FVector& Out, const TCHAR* Key = TEXT("originXYZ"))
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.TryGetArrayField(Key, Values) || Values->Num() != 3) return false;
        FVector Candidate;
        for (int32 I = 0; I < 3; ++I)
        {
            double Value = 0;
            if (!(*Values)[I] || (*Values)[I]->Type != EJson::Number || !(*Values)[I]->TryGetNumber(Value) || !FMath::IsFinite(Value) || FMath::Abs(Value) > 1.e9) return false;
            Candidate[I] = Value;
        }
        Out = Candidate; return true;
    }
    bool Evaluate(const FJsonObject& Original, const FStudioHome4Spec& Next, FStudioHome4CheckpointInspection& Result)
    {
        auto Reject = [&](const FString& Error) { Result.State = EStudioHome4CheckpointState::Rejected; Result.Error = Error; return false; };
        auto Unavailable = [&](const FString& Error) { Result.State = EStudioHome4CheckpointState::Unavailable; Result.Error = Error; return false; };
        auto Mismatch = [&](const FString& Error) { Result.State = EStudioHome4CheckpointState::GridMismatch; Result.Error = Error; return false; };
        const TSharedPtr<FJsonObject>* Attestation = nullptr;
        if (!Original.HasField(TEXT("frontendRestartAttestation")))
            return Unavailable(TEXT("Original restart metadata unavailable. This archive has no source-declared frontend restart attestation; a viz snapshot cannot be used as a checkpoint."));
        if (!Original.TryGetObjectField(TEXT("frontendRestartAttestation"), Attestation) || !*Attestation)
            return Reject(TEXT("Original frontend restart attestation is invalid."));
        auto DeclaresViz = [](const FJsonObject& Object)
        {
            FString Kind;
            if (!Object.TryGetStringField(TEXT("kind"), Kind)) return false;
            return Kind == TEXT("viz") || Kind == TEXT("viz_snapshot") || Kind == TEXT("snapshot") || Kind == TEXT("slice") ||
                Kind == TEXT("home4_structured_source") || Kind == TEXT("home4_structured_slice");
        };
        const TSharedPtr<FJsonObject>* SourceArchive = nullptr;
        if (DeclaresViz(Original) || (Original.TryGetObjectField(TEXT("archive"), SourceArchive) && *SourceArchive && DeclaresViz(**SourceArchive)))
            return Reject(TEXT("Original source declares a visualization snapshot or slice; it cannot be a restart checkpoint."));
        const auto& A = **Attestation;
        double Version = 0, LevelCount = 0; FString Kind;
        if (!Strict(A, {TEXT("version"), TEXT("kind"), TEXT("axisOrder"), TEXT("dimensionsXYZ"), TEXT("componentAxis"), TEXT("requiredMembers"), TEXT("scalarMembers"), TEXT("tensorMembers"), TEXT("levelCount"), TEXT("patches")}) ||
            !A.TryGetNumberField(TEXT("version"), Version) || Version != 1 || !A.TryGetStringField(TEXT("kind"), Kind) || Kind != TEXT("checkpoint"))
            return Reject(TEXT("Only the explicit frontend checkpoint attestation v1 is supported. Viz output is not restart state."));
        if (!A.TryGetNumberField(TEXT("levelCount"), LevelCount) || !FMath::IsFinite(LevelCount) || LevelCount < 1 || LevelCount > 16 || LevelCount != FMath::FloorToDouble(LevelCount))
            return Unavailable(TEXT("Original checkpoint level count unavailable. Declare the complete root and refinement topology."));
        Result.LevelCount = int32(LevelCount);
        TSet<FString> Seen; int64 TotalNodes = 0;
        TArray<int64> NodesByLevel; NodesByLevel.SetNumZeroed(Result.LevelCount);
        auto StateGrid = [&](const FJsonObject& Group, int32 Level, FStudioHome4CheckpointPatch& Evidence)
        {
            FString ComponentAxis;
            if (!Group.TryGetStringField(TEXT("axisOrder"), Evidence.AxisOrder) || !Axes(Evidence.AxisOrder) ||
                !Dimensions(Group, TEXT("dimensionsXYZ"), Evidence.DimensionsXYZ) ||
                !Group.TryGetStringField(TEXT("componentAxis"), ComponentAxis) || ComponentAxis != TEXT("last"))
                return Reject(TEXT("Every checkpoint grid needs explicit XYZ dimensions, array axes and final component axis."));
            const auto& Grid = Evidence.DimensionsXYZ;
            const int64 Nodes = int64(Grid.X) * Grid.Y * Grid.Z;
            if (Nodes > StudioHome4Archives::MaximumSourceNodes || TotalNodes > StudioHome4Archives::MaximumSourceNodes - Nodes)
                return Reject(TEXT("Original checkpoint root and patch grids exceed the native total two million node inspection bound."));
            TotalNodes += Nodes; NodesByLevel[Level] += Nodes;
            const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
            if (!Group.TryGetArrayField(TEXT("requiredMembers"), Required) || Required->IsEmpty() || Required->Num() > 126)
                return Reject(TEXT("The original producer must declare the full required restart member list for every grid."));
            TSet<FString> GroupKeys;
            for (const auto& Value : *Required)
            {
                FString Name;
                if (!Value || Value->Type != EJson::String || !Value->TryGetString(Name) || !StudioHome4ArchivePrivate::CleanText(Name, 120) ||
                    Name == TEXT("run_spec") || Name == TEXT("iteration") || Seen.Contains(Name.ToLower()))
                    return Reject(TEXT("Original required restart keys must be globally unique grid members, excluding metadata."));
                const auto* Member = Result.Members.FindByPredicate([&](const auto& M) { return M.Name == Name; });
                if (!Member || Member->DType.Len() < 3 || (Member->DType[1] != 'f' && Member->DType[1] != 'i' && Member->DType[1] != 'u' && Member->DType[1] != 'b'))
                    return Reject(TEXT("Missing or unsupported required restart member: ") + Name);
                if (Member->Shape.Num() != 3 && Member->Shape.Num() != 4)
                    return Reject(TEXT("Required restart member must retain its declared original grid: ") + Name);
                for (int32 I = 0; I < 3; ++I)
                { const int32 Axis = Evidence.AxisOrder[I] == 'x' ? 0 : Evidence.AxisOrder[I] == 'y' ? 1 : 2; if (Member->Shape[I] != Grid[Axis]) return Reject(TEXT("Required member has an incompatible original grid: ") + Name); }
                if (Member->Shape.Num() == 4 && (Member->Shape[3] < 1 || Member->Shape[3] > 64)) return Reject(TEXT("Required restart component count exceeds the supported bound: ") + Name);
                Seen.Add(Name.ToLower()); GroupKeys.Add(Name.ToLower()); Evidence.RequiredMembers.Add(Name); Result.RequiredMembers.Add(Name);
            }
            const TSharedPtr<FJsonObject>* Scalars = nullptr; const TSharedPtr<FJsonObject>* Tensors = nullptr;
            if (!Group.TryGetObjectField(TEXT("scalarMembers"), Scalars) || !*Scalars ||
                !Strict(**Scalars, {TEXT("rho"), TEXT("ux"), TEXT("uy"), TEXT("uz"), TEXT("phi"), TEXT("solid")}) ||
                !Group.TryGetObjectField(TEXT("tensorMembers"), Tensors) || !*Tensors ||
                !Strict(**Tensors, {TEXT("S"), TEXT("a3"), TEXT("Jphi"), TEXT("Pphi")}))
                return Reject(TEXT("Source-declared scalar and hydro/phase tensor families are required for every checkpoint grid."));
            TSet<FString> Roles;
            auto Role = [&](const FJsonObject& Object, const TCHAR* RoleName, int32 Components, bool bSolid = false)
            {
                FString Name; if (!Object.TryGetStringField(RoleName, Name) || !GroupKeys.Contains(Name.ToLower()) || Roles.Contains(Name.ToLower())) return false;
                const auto* Member = Result.Members.FindByPredicate([&](const auto& M) { return M.Name == Name; });
                if (!Member || (!bSolid && Member->DType[1] != 'f') || Member->Shape.Num() != (Components == 1 ? 3 : 4) ||
                    (Components > 1 && Member->Shape[3] != Components)) return false;
                Roles.Add(Name.ToLower()); return true;
            };
            for (const auto* Key : {TEXT("rho"), TEXT("ux"), TEXT("uy"), TEXT("uz"), TEXT("phi"), TEXT("solid")})
                if (!Role(**Scalars, Key, 1, FString(Key) == TEXT("solid"))) return Reject(FString(TEXT("Missing, reused or incorrectly shaped original scalar role: ")) + Key);
            for (const auto& Pair : TArray<TPair<const TCHAR*, int32>>{{TEXT("S"), 6}, {TEXT("a3"), 7}, {TEXT("Jphi"), 3}, {TEXT("Pphi"), 6}})
                if (!Role(**Tensors, Pair.Key, Pair.Value)) return Reject(FString(TEXT("Missing, reused or incorrectly shaped retained moment family: ")) + Pair.Key);
            return true;
        };
        FStudioHome4CheckpointPatch Root; Root.Id = TEXT("root");
        if (!StateGrid(A, 0, Root)) return false;
        Result.DimensionsXYZ = Root.DimensionsXYZ; Result.AxisOrder = Root.AxisOrder;
        const TSharedPtr<FJsonObject>* Archive = nullptr;
        if (Original.TryGetObjectField(TEXT("archive"), Archive) && *Archive && (*Archive)->HasField(TEXT("axisOrder")))
        { FString DeclaredAxes; if (!(*Archive)->TryGetStringField(TEXT("axisOrder"), DeclaredAxes) || DeclaredAxes != Root.AxisOrder) return Reject(TEXT("Original archive axes conflict with its restart attestation.")); }
        const TSharedPtr<FJsonObject>* Lattice = nullptr;
        if (Original.TryGetObjectField(TEXT("lattice"), Lattice) && *Lattice && (*Lattice)->HasField(TEXT("extents")) && !(*Lattice)->HasTypedField<EJson::Null>(TEXT("extents")))
        { FIntVector DeclaredGrid; if (!Dimensions(**Lattice, TEXT("extents"), DeclaredGrid) || DeclaredGrid != Root.DimensionsXYZ) return Reject(TEXT("Original lattice extents conflict with the restart attestation.")); }
        const TArray<TSharedPtr<FJsonValue>>* Patches = nullptr;
        if (!A.TryGetArrayField(TEXT("patches"), Patches) || Patches->Num() > 64)
            return Unavailable(TEXT("Original checkpoint patch descriptors unavailable; declare the complete patch list, including an empty list for root-only state."));
        TSet<FString> PatchIds;
        for (const auto& Value : *Patches)
        {
            if (!Value || Value->Type != EJson::Object) return Reject(TEXT("Original checkpoint patch descriptor must be an object."));
            const auto Patch = Value->AsObject(); FStudioHome4CheckpointPatch Evidence; double Level = 0;
            if (!Patch || !Strict(*Patch, {TEXT("id"), TEXT("bodyId"), TEXT("level"), TEXT("originXYZ"), TEXT("followBody"), TEXT("axisOrder"), TEXT("dimensionsXYZ"), TEXT("componentAxis"), TEXT("requiredMembers"), TEXT("scalarMembers"), TEXT("tensorMembers")}) ||
                !Patch->TryGetStringField(TEXT("id"), Evidence.Id) || !StudioHome4ArchivePrivate::CleanText(Evidence.Id, 120) || Evidence.Id == TEXT("root") || PatchIds.Contains(Evidence.Id) ||
                !Patch->TryGetStringField(TEXT("bodyId"), Evidence.BodyId) ||
                !Patch->TryGetNumberField(TEXT("level"), Level) || !FMath::IsFinite(Level) || Level < 0 || Level >= LevelCount || Level != FMath::FloorToDouble(Level) ||
                !Origin(*Patch, Evidence.OriginXYZ) || !Patch->TryGetBoolField(TEXT("followBody"), Evidence.bFollowBody))
                return Reject(TEXT("Original checkpoint patch identities, levels, origins and body-following policy must be explicit and unique."));
            // Static world patches explicitly declare an empty body identity.
            // Following patches still require a clean, nonempty original body.
            if ((Evidence.BodyId.IsEmpty() && Evidence.bFollowBody) ||
                (!Evidence.BodyId.IsEmpty() && !StudioHome4ArchivePrivate::CleanText(Evidence.BodyId, 120)))
                return Reject(TEXT("Original checkpoint following patches require a body identity; static patches may explicitly declare an empty bodyId."));
            Evidence.Level = int32(Level); PatchIds.Add(Evidence.Id);
            if (!StateGrid(*Patch, Evidence.Level, Evidence)) return false;
            Result.Patches.Add(MoveTemp(Evidence));
        }
        for (int32 Level = 0; Level < Result.LevelCount; ++Level)
            if (NodesByLevel[Level] == 0) return Unavailable(TEXT("Checkpoint state evidence is missing for a declared refinement level."));
        const TSharedPtr<FJsonObject>* OriginalMD = nullptr;
        if (Original.TryGetObjectField(TEXT("multidomain"), OriginalMD) && *OriginalMD)
        {
            if ((*OriginalMD)->HasField(TEXT("levels")) && !(*OriginalMD)->HasTypedField<EJson::Null>(TEXT("levels")))
            {
                double DeclaredLevels = 0;
                if (!(*OriginalMD)->TryGetNumberField(TEXT("levels"), DeclaredLevels) || DeclaredLevels != LevelCount)
                    return Reject(TEXT("Original multidomain level count conflicts with its checkpoint state evidence."));
            }
            if ((*OriginalMD)->HasField(TEXT("levelCells")) && !(*OriginalMD)->HasTypedField<EJson::Null>(TEXT("levelCells")))
            {
                const TArray<TSharedPtr<FJsonValue>>* Cells = nullptr;
                if (!(*OriginalMD)->TryGetArrayField(TEXT("levelCells"), Cells)) return Reject(TEXT("Original multidomain cell counts are invalid."));
                if (!Cells->IsEmpty())
                {
                    if (Cells->Num() != NodesByLevel.Num()) return Reject(TEXT("Original multidomain cell count list conflicts with checkpoint levels."));
                    for (int32 Level = 0; Level < Cells->Num(); ++Level)
                    {
                        double Count = 0;
                        if (!(*Cells)[Level] || (*Cells)[Level]->Type != EJson::Number || !(*Cells)[Level]->TryGetNumber(Count) || Count != double(NodesByLevel[Level]))
                            return Reject(TEXT("Original declared per-level cell count conflicts with its retained checkpoint grids."));
                    }
                }
            }
        }
        const TSharedPtr<FJsonObject>* OriginalAuthoring = nullptr;
        if (Original.TryGetObjectField(TEXT("authoring"), OriginalAuthoring) && *OriginalAuthoring && (*OriginalAuthoring)->HasField(TEXT("patches")))
        {
            const TArray<TSharedPtr<FJsonValue>>* Declared = nullptr;
            if (!(*OriginalAuthoring)->TryGetArrayField(TEXT("patches"), Declared) || Declared->Num() != Result.Patches.Num())
                return Reject(TEXT("Original authored patch set conflicts with its checkpoint state evidence."));
            TSet<FString> MatchedOriginal;
            for (const auto& Value : *Declared)
            {
                if (!Value || Value->Type != EJson::Object) return Reject(TEXT("Original authored patch descriptor is invalid."));
                const auto Object = Value->AsObject(); FString Id, BodyId; double Level = 0; bool bFollow = false; FVector Position; FIntVector Extents;
                if (!Object || !Object->TryGetStringField(TEXT("id"), Id) || MatchedOriginal.Contains(Id) || !Object->TryGetStringField(TEXT("bodyId"), BodyId) ||
                    !Object->TryGetNumberField(TEXT("level"), Level) || !Origin(*Object, Position, TEXT("origin")) || !Dimensions(*Object, TEXT("extents"), Extents) || !Object->TryGetBoolField(TEXT("followBody"), bFollow))
                    return Reject(TEXT("Original authored patch metadata is incomplete or invalid."));
                const auto* Evidence = Result.Patches.FindByPredicate([&](const auto& Patch) { return Patch.Id == Id; });
                if (!Evidence || Evidence->BodyId != BodyId || double(Evidence->Level) != Level || Evidence->OriginXYZ != Position || Evidence->DimensionsXYZ != Extents || Evidence->bFollowBody != bFollow)
                    return Reject(TEXT("Original authored patch descriptor conflicts with checkpoint attestation: ") + Id);
                MatchedOriginal.Add(Id);
            }
        }
        for (const auto& Member : Result.Members)
            if (!Seen.Contains(Member.Name.ToLower()) && Member.Name != TEXT("run_spec") && Member.Name != TEXT("iteration")) Result.UndeclaredMembers.Add(Member.Name);
        if (!Next.Lattice.Extents.IsSet()) return Unavailable(TEXT("Next-run XYZ grid is unspecified; checkpoint compatibility is unavailable."));
        if (*Next.Lattice.Extents != Root.DimensionsXYZ)
            return Mismatch(FString::Printf(TEXT("Grid mismatch: original checkpoint %d × %d × %d; next run %d × %d × %d. Warm start is blocked."), Root.DimensionsXYZ.X, Root.DimensionsXYZ.Y, Root.DimensionsXYZ.Z, Next.Lattice.Extents->X, Next.Lattice.Extents->Y, Next.Lattice.Extents->Z));
        const int64 NextLevels = Next.Multidomain.Levels.Get(1);
        if (NextLevels != Result.LevelCount) return Mismatch(TEXT("Original checkpoint refinement level count differs from the exact next-run request."));
        if (Next.Authoring.Patches.Num() != Result.Patches.Num()) return Mismatch(TEXT("Original checkpoint patch set differs from the exact next-run authored patch set; no patch origins or grids are inferred."));
        TSet<FString> Matched;
        for (const auto& Expected : Next.Authoring.Patches)
        {
            const auto* Actual = Result.Patches.FindByPredicate([&](const auto& Patch) { return Patch.Id == Expected.Id; });
            if (!Actual || Matched.Contains(Expected.Id) || Actual->BodyId != Expected.BodyId || Actual->Level != Expected.Level ||
                Actual->OriginXYZ != Expected.Origin || Actual->DimensionsXYZ != Expected.Extents || Actual->bFollowBody != Expected.bFollowBody)
                return Mismatch(TEXT("Original checkpoint patch identity, level, origin, grid or following-body policy differs from next run: ") + Expected.Id);
            Matched.Add(Expected.Id);
        }
        if (!Next.Multidomain.LevelCells.IsEmpty())
        {
            if (Next.Multidomain.LevelCells.Num() != NodesByLevel.Num()) return Mismatch(TEXT("Next-run declared level cell counts do not match the complete checkpoint level count."));
            for (int32 Level = 0; Level < NodesByLevel.Num(); ++Level)
                if (Next.Multidomain.LevelCells[Level] != NodesByLevel[Level]) return Mismatch(TEXT("Original checkpoint grid nodes differ from next-run declared cell count at level ") + LexToString(Level));
        }
        Result.State = EStudioHome4CheckpointState::GridCompatible; Result.Error.Empty(); return true;
    }
}

FStudioHome4CheckpointInspection StudioHome4Checkpoints::Inspect(const FString& Path, const FStudioHome4Spec& NextSpec, const FStudioLoadCancellation& Cancel)
{
    using namespace StudioHome4ArchivePrivate;
    FStudioHome4CheckpointInspection Result; Result.Source.Path = FPaths::ConvertRelativePathToFull(Path);
    auto Fail = [&](const FString& Error)
    { Result.bCancelled = Cancelled(Cancel); Result.State = Result.bCancelled ? EStudioHome4CheckpointState::Cancelled : EStudioHome4CheckpointState::Rejected; Result.Error = Error; return Result; };
    FStudioFileAccess Access(Result.Source.Path); const int64 Size = IFileManager::Get().FileSize(*Result.Source.Path);
    const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Result.Source.Path);
    FString Error;
    if (Size <= 0 || Size > StudioHome4Archives::MaximumArchiveBytes) return Fail(TEXT("Checkpoint exceeds the native 512 MiB compressed/expanded archive bound or is unavailable."));
    if (!Hash(Result.Source.Path, Result.Source.SHA256, Cancel, Error)) return Fail(Error);
    FNpzReader Reader;
    if (!Reader.Open(Result.Source.Path, Cancel, Error) || !Reader.ScanHeadersAndCRC(Error)) return Fail(Error);
    Result.Members = Reader.Members();
    const auto* SpecMember = Result.Members.FindByPredicate([](const auto& M) { return M.Name == TEXT("run_spec"); });
    TSharedPtr<FJsonObject> Original;
    if (SpecMember)
    {
        FNumericArray Spec;
        if (!Reader.Read(TEXT("run_spec"), Spec, Error, false) || !JSON(Spec.StringValue, Original, Error)) return Fail(Error);
        Result.OriginalRunSpecJSON = MoveTemp(Spec.StringValue);
    }
    if (const auto* StepHeader = Result.Members.FindByPredicate([](const auto& M) { return M.Name == TEXT("iteration"); }))
    {
        // Reject array-shaped metadata before Read can allocate a numeric payload.
        if (!StepHeader->Shape.IsEmpty() || StepHeader->Count != 1 || (StepHeader->DType[1] != 'i' && StepHeader->DType[1] != 'u'))
            return Fail(TEXT("Original checkpoint iteration must be scalar integer metadata."));
        FNumericArray Step;
        if (!Reader.Read(TEXT("iteration"), Step, Error) || Step.Values.Num() != 1 ||
            Step.Values[0] < 0 || Step.Values[0] > MAX_int32 || Step.Values[0] != FMath::FloorToDouble(Step.Values[0]))
            return Fail(TEXT("Original checkpoint iteration is not a bounded integer solver step."));
        Result.OriginalStep = int32(Step.Values[0]);
    }
    FString After;
    if (!Hash(Result.Source.Path, After, Cancel, Error)) return Fail(Error);
    if (After != Result.Source.SHA256 || Size != IFileManager::Get().FileSize(*Result.Source.Path) || Stamp != IFileManager::Get().GetTimeStamp(*Result.Source.Path))
        return Fail(TEXT("Checkpoint changed during bounded inspection; inspect the original again."));
    if (!Original)
    { Result.State = EStudioHome4CheckpointState::Unavailable; Result.Error = TEXT("Original checkpoint run_spec metadata unavailable. Warm start is blocked."); return Result; }
    StudioHome4CheckpointPrivate::Evaluate(*Original, NextSpec, Result); return Result;
}

FStudioHome4PreparedCheckpoint::FStudioHome4PreparedCheckpoint(FString InDirectory, FString InPreparedPath, FString InSpecIdentity, FStudioHome4CheckpointInspection InInspection)
    : Directory(MoveTemp(InDirectory)), Path(MoveTemp(InPreparedPath)), Identity(MoveTemp(InSpecIdentity)), Inspection(MoveTemp(InInspection)) {}
FStudioHome4PreparedCheckpoint::~FStudioHome4PreparedCheckpoint()
{
    if (!Directory.IsEmpty())
    {
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Path, false);
        IFileManager::Get().DeleteDirectory(*Directory, false, true);
    }
}

static FStudioHome4CheckpointPreparation PrepareCheckpoint(const FString& Path, const FStudioHome4Spec& Spec, const FStudioLoadCancellation& Cancel, TFunction<void()> BeforeOriginalVerify)
{
    using namespace StudioHome4ArchivePrivate;
    FStudioHome4CheckpointPreparation Result;
    Result.Inspection.Source.Path = FPaths::ConvertRelativePathToFull(Path);
    struct FPrivateStage
    {
        FString Directory;
        ~FPrivateStage() { if (!Directory.IsEmpty()) IFileManager::Get().DeleteDirectory(*Directory, false, true); }
    } Stage;
    FString& Directory = Stage.Directory; FString Error;
    auto Fail = [&](const FString& Reason)
    {
        Result.Inspection.bCancelled = Cancelled(Cancel);
        Result.Inspection.State = Result.Inspection.bCancelled ? EStudioHome4CheckpointState::Cancelled : EStudioHome4CheckpointState::Rejected;
        Result.Inspection.Error = Reason;
        return Result;
    };
    FStudioFileAccess Access(Result.Inspection.Source.Path);
    if (Cancelled(Cancel)) return Fail(TEXT("Checkpoint inspection cancelled."));
    const FString Parent = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()) / TEXT("Home4CheckpointPreflight");
    if (!IFileManager::Get().MakeDirectory(*Parent, true) || !StudioFileDialog::CreateExportStage(Parent, Directory, Error)) return Fail(TEXT("Could not reserve a private checkpoint preparation folder: ") + Error);
    const FString Copy = Directory / TEXT("checkpoint.npz");
    const int64 Size = IFileManager::Get().FileSize(*Result.Inspection.Source.Path);
    const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Result.Inspection.Source.Path);
    FString OriginalSHA;
    if (Size <= 0 || Size > StudioHome4Archives::MaximumArchiveBytes || !Hash(Result.Inspection.Source.Path, OriginalSHA, Cancel, Error)) return Fail(TEXT("Original checkpoint is inaccessible, changed or exceeds 512 MiB. ") + Error);
    {
        TUniquePtr<FArchive> In(IFileManager::Get().CreateFileReader(*Result.Inspection.Source.Path, FILEREAD_Silent));
        TUniquePtr<FArchive> Out(IFileManager::Get().CreateFileWriter(*Copy, FILEWRITE_NoReplaceExisting));
        if (!In || !Out || In->TotalSize() != Size) return Fail(TEXT("Could not open the bounded original checkpoint and its private copy."));
        uint8 Buffer[65536];
        for (int64 At = 0; At < Size;)
        {
            if (Cancelled(Cancel)) return Fail(TEXT("Checkpoint inspection cancelled."));
            const int64 Count = FMath::Min<int64>(sizeof(Buffer), Size - At); In->Serialize(Buffer, Count); Out->Serialize(Buffer, Count);
            if (In->IsError() || Out->IsError()) return Fail(TEXT("Checkpoint copy failed; check available disk space."));
            At += Count;
        }
        if (!Out->Close() || Out->IsError()) return Fail(TEXT("Could not close the private checkpoint copy."));
    }
    if (BeforeOriginalVerify) BeforeOriginalVerify();
    FString After;
    if (!Hash(Result.Inspection.Source.Path, After, Cancel, Error) || OriginalSHA != After || Size != IFileManager::Get().FileSize(*Result.Inspection.Source.Path) || Stamp != IFileManager::Get().GetTimeStamp(*Result.Inspection.Source.Path))
        return Fail(TEXT("Original checkpoint changed while prepared; inspect it again."));
    Result.Inspection = StudioHome4Checkpoints::Inspect(Copy, Spec, Cancel); Result.Inspection.Source.Path = FPaths::ConvertRelativePathToFull(Path);
    if (Result.Inspection.Source.SHA256 != OriginalSHA) return Fail(TEXT("Private checkpoint copy differs from the original SHA-256."));
    if (Cancelled(Cancel)) return Fail(TEXT("Checkpoint inspection cancelled."));
    if (Result.Inspection.State != EStudioHome4CheckpointState::GridCompatible) return Result;
    const FString SpecIdentity = StudioHome4CheckpointPrivate::Identity(Spec);
    if (SpecIdentity.IsEmpty()) return Fail(TEXT("Could not bind the checkpoint to the frozen next-run request."));
    if (!FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Copy, true)) return Fail(TEXT("Could not seal the privately prepared checkpoint bytes."));
    if (Cancelled(Cancel))
    { FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Copy, false); return Fail(TEXT("Checkpoint inspection cancelled.")); }
    Result.Lease = MakeShared<FStudioHome4PreparedCheckpoint, ESPMode::ThreadSafe>(Directory, Copy, SpecIdentity, Result.Inspection);
    Directory.Empty(); // Cleanup is now owned by the immutable lease.
    return Result;
}

FStudioHome4CheckpointPreparation StudioHome4Checkpoints::Prepare(const FString& Path, const FStudioHome4Spec& Spec, const FStudioLoadCancellation& Cancel)
{ return PrepareCheckpoint(Path, Spec, Cancel, {}); }
#if WITH_DEV_AUTOMATION_TESTS
FStudioHome4CheckpointPreparation StudioHome4Checkpoints::PrepareWithCopyBoundaryForAutomation(const FString& Path, const FStudioHome4Spec& Spec, const FStudioLoadCancellation& Cancel, TFunction<void()> BeforeOriginalVerify)
{ return PrepareCheckpoint(Path, Spec, Cancel, MoveTemp(BeforeOriginalVerify)); }
#endif

FStudioHome4CheckpointSession::FStudioHome4CheckpointSession(TSharedPtr<FStudioModel> InModel, TSharedPtr<FStudioHome4Session> InEditor)
    : Model(InModel), Editor(InEditor) { Scope(); Notice = TEXT("Choose the original checkpoint and inspect it for this next-run draft."); }
FStudioHome4CheckpointSession::~FStudioHome4CheckpointSession()
{ if (Cancellation) Cancellation->store(true, std::memory_order_relaxed); }
void FStudioHome4CheckpointSession::Invalidate(const FString& Reason)
{
    if (Cancellation) Cancellation->store(true, std::memory_order_relaxed);
    ++Generation; Prepared.Reset(); Inspected.Reset(); CurrentState = EStudioHome4CheckpointState::Unavailable; Notice = Reason;
}
void FStudioHome4CheckpointSession::Scope()
{
    const auto M = Model.Pin(); const auto E = Editor.Pin();
    const FGuid P = M ? M->Project.Id : FGuid(), C = M ? M->Project.Draft.Id : FGuid();
    FStudioHome4Spec Draft; FString Error, CurrentIdentity;
    if (E) { E->Refresh(); if (!E->HasConflict() && E->Build(Draft, Error)) CurrentIdentity = StudioHome4CheckpointPrivate::Identity(Draft); }
    if (P != ProjectId || C != CaseId || CurrentIdentity != DraftIdentity)
    {
        Invalidate(TEXT("Project, case or next-run draft changed. Inspect its original checkpoint again."));
        ProjectId = P; CaseId = C; DraftIdentity = MoveTemp(CurrentIdentity);
    }
}
bool FStudioHome4CheckpointSession::Start(const FString& Path, const FStudioHome4Spec& Spec, FString& Error)
{
    Scope();
    const FString Identity = StudioHome4CheckpointPrivate::Identity(Spec);
    if (Pending.IsValid()) { Error = TEXT("A checkpoint worker is still completing; cancel or wait for it."); return false; }
    if (!ProjectId.IsValid() || !CaseId.IsValid() || Identity.IsEmpty() || Identity != DraftIdentity || !StudioHome4ArchivePrivate::CleanText(Path, 4096) ||
        Spec.Run.InitState.IsEmpty() || FPaths::ConvertRelativePathToFull(Spec.Run.InitState) != FPaths::ConvertRelativePathToFull(Path))
    { Error = TEXT("Choose the original checkpoint path in this project's current valid next-run draft before inspection."); return false; }
    Invalidate(TEXT("Preparing a private checkpoint copy and streaming original headers/checksums…"));
    CurrentState = EStudioHome4CheckpointState::Inspecting; PendingGeneration = Generation;
    Cancellation = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    Pending = Async(EAsyncExecution::ThreadPool, [Path, Spec, Cancel = Cancellation] { return StudioHome4Checkpoints::Prepare(Path, Spec, Cancel); });
    Error.Empty(); return true;
}
void FStudioHome4CheckpointSession::Tick()
{
    Scope(); if (!Pending.IsValid() || !Pending.IsReady()) return;
    auto Result = Pending.Get(); Pending = {}; Cancellation.Reset();
    if (PendingGeneration != Generation) return;
    Inspected = MoveTemp(Result.Inspection); CurrentState = Inspected->State; Prepared = MoveTemp(Result.Lease);
    if (CurrentState == EStudioHome4CheckpointState::GridCompatible)
        Notice = FString::Printf(TEXT("Frontend grid compatible: %d × %d × %d · %d source-declared state members retained · %d levels, %d patches · SHA-256 %s. Driver numerical and STATE_KEYS validation remains unavailable."), Inspected->DimensionsXYZ->X, Inspected->DimensionsXYZ->Y, Inspected->DimensionsXYZ->Z, Inspected->RequiredMembers.Num(), Inspected->LevelCount, Inspected->Patches.Num(), *Inspected->Source.SHA256.Left(12));
    else Notice = Inspected->Error;
}
void FStudioHome4CheckpointSession::Cancel()
{
    Invalidate(TEXT("Checkpoint inspection cancelled. Warm start requires a current completed inspection.")); CurrentState = EStudioHome4CheckpointState::Cancelled;
}
FString FStudioHome4CheckpointSession::Status() { Scope(); return Notice; }
EStudioHome4CheckpointState FStudioHome4CheckpointSession::State() { Scope(); return CurrentState; }
const FStudioHome4CheckpointInspection* FStudioHome4CheckpointSession::Metadata() { Scope(); return Inspected.IsSet() ? &*Inspected : nullptr; }
bool FStudioHome4CheckpointSession::ValidateForSubmit(const FStudioHome4Spec& Spec, TSharedPtr<const FStudioHome4PreparedCheckpoint, ESPMode::ThreadSafe>& OutLease, FString& Error)
{
    Scope(); OutLease.Reset();
    if (Spec.Run.InitState.IsEmpty()) { Error.Empty(); return true; }
    const FString SpecIdentity = StudioHome4CheckpointPrivate::Identity(Spec);
    if (Pending.IsValid() || CurrentState != EStudioHome4CheckpointState::GridCompatible || !Prepared ||
        SpecIdentity.IsEmpty() || SpecIdentity != DraftIdentity || Prepared->SpecIdentity() != SpecIdentity ||
        FPaths::ConvertRelativePathToFull(Spec.Run.InitState) != Prepared->SourcePath())
    { Error = Notice.IsEmpty() ? TEXT("Warm start requires inspection of an original full checkpoint for this exact project, case and next-run draft.") : Notice; return false; }
    OutLease = Prepared; Error.Empty(); return true;
}
