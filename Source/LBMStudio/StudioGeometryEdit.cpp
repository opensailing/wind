#include "StudioGeometryEdit.h"
#include "StudioColor.h"
#include <charconv>

namespace StudioGeometryEditPrivate
{
FString Exact(double Value)
{
    ANSICHAR Buffer[128];
    const auto Result = std::to_chars(Buffer, Buffer + UE_ARRAY_COUNT(Buffer) - 1, Value);
    if (Result.ec == std::errc())
    {
        *Result.ptr = '\0';
        return FString(UTF8_TO_TCHAR(Buffer));
    }
    return FString::Printf(TEXT("%.17g"), Value);
}
}

void FStudioGeometryEdit::Reset(const FStudioGeometryAsset& Asset)
{
    Saved = Asset;
    Name = Asset.Name;
    Error.Empty();
    ErrorField = INDEX_NONE;
    const FRotator Rotation = Asset.Rotation.Rotator();
    const double Angles[] = {Rotation.Roll, Rotation.Pitch, Rotation.Yaw};
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        Values[Axis] = StudioGeometryEditPrivate::Exact(Asset.Translation[Axis]);
        Values[Axis + 3] = StudioGeometryEditPrivate::Exact(Angles[Axis]);
        Values[Axis + 6] = StudioGeometryEditPrivate::Exact(Asset.Scale[Axis]);
    }
}

bool FStudioGeometryEdit::IsDirty() const
{
    FStudioGeometryEdit Original;
    Original.Reset(Saved);
    if (Name != Original.Name) return true;
    for (int32 Index = 0; Index < 9; ++Index)
        if (Values[Index] != Original.Values[Index]) return true;
    return false;
}

bool FStudioGeometryEdit::Matches(const FStudioGeometryAsset& Asset) const
{
    if (Asset.Id != Saved.Id || Asset.Name != Saved.Name || Asset.Format != Saved.Format ||
        Asset.SourceSHA256 != Saved.SourceSHA256 || Asset.MetersPerSourceUnit != Saved.MetersPerSourceUnit ||
        Asset.Translation != Saved.Translation || Asset.Rotation != Saved.Rotation || Asset.Scale != Saved.Scale ||
        Asset.Patches.Num() != Saved.Patches.Num()) return false;
    for (int32 Index = 0; Index < Asset.Patches.Num(); ++Index)
        if (Asset.Patches[Index].Id != Saved.Patches[Index].Id || Asset.Patches[Index].Name != Saved.Patches[Index].Name)
            return false;
    return true;
}

bool FStudioGeometryEdit::Build(FStudioGeometryAsset& Out)
{
    Error.Empty();
    ErrorField = INDEX_NONE;
    const auto Fail = [this](int32 Field, const FString& Message)
    {
        ErrorField = Field;
        Error = Message;
        return false;
    };
    if (Name.TrimStartAndEnd().IsEmpty() || Name.Len() > 120)
        return Fail(0, TEXT("Enter an object name of 1–120 characters."));

    double Parsed[9];
    const TCHAR* Labels[] = {TEXT("Position X"), TEXT("Position Y"), TEXT("Position Z"),
        TEXT("Roll"), TEXT("Pitch"), TEXT("Yaw"), TEXT("Scale X"), TEXT("Scale Y"), TEXT("Scale Z")};
    for (int32 Index = 0; Index < 9; ++Index)
    {
        if (!StudioColor::ParseNumber(Values[Index], Parsed[Index]))
            return Fail(Index + 1, FString(Labels[Index]) + TEXT(": enter a finite number."));
        const double Limit = Index >= 3 && Index < 6 ? 360000. : 1.e8;
        if (FMath::Abs(Parsed[Index]) > Limit || (Index >= 6 && Parsed[Index] <= 0))
            return Fail(Index + 1, FString(Labels[Index]) + (Index >= 6 ?
                TEXT(": enter a positive factor no larger than 1e8.") : Index >= 3 ?
                TEXT(": enter degrees between −360000 and 360000.") :
                TEXT(": enter meters between −1e8 and 1e8.")));
    }

    FStudioGeometryAsset Candidate = Saved;
    Candidate.Name = Name.TrimStartAndEnd();
    Candidate.Translation = FVector(Parsed[0], Parsed[1], Parsed[2]);
    Candidate.Scale = FVector(Parsed[6], Parsed[7], Parsed[8]);
    const FRotator Original = Saved.Rotation.Rotator();
    // Comparing numbers also preserves the original quaternion when only the
    // formatting of an Euler field changed, including at a gimbal singularity.
    if (Parsed[3] != Original.Roll || Parsed[4] != Original.Pitch || Parsed[5] != Original.Yaw)
        Candidate.Rotation = FRotator(Parsed[4], Parsed[5], Parsed[3]).Quaternion().GetNormalized();
    Out = MoveTemp(Candidate);
    return true;
}
