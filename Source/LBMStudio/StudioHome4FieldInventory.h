#pragma once
#include "CoreMinimal.h"
#include "StudioRecording.h"
class FJsonObject;
/** Source-only, pointwise diagnostics. Never consumes editable case parameters. */
namespace StudioHome4FieldInventory
{
    constexpr int32 MaximumFields=128;
    bool Augment(TMap<FString,TArray<double>>& Fields,TMap<FString,FString>& Units,
        TMap<FString,FString>& Expressions,TSet<FString>& Derived,TMap<FString,FString>& Validity,
        const FJsonObject* OriginalRunSpec,TOptional<double> OriginalDxMeters,
        const FStudioLoadCancellation& Cancel,FString& Error);
    bool Advanced(const FString& Id);
    FString Group(const FString& Id);
}
