#pragma once
#include "StudioModel.h"
#include "StudioPointRecording.h"

struct FStudioSourceVector
{
    FString Id,Unit;
    FString Components[3];
};
struct FStudioSourceVectorRows
{
    TArray<FVector> Values;
    TArray<uint8> Valid;
    FString Unit;
};
namespace StudioSourceVectors
{
    /** Only complete explicitly associated components with identical units. */
    TArray<FStudioSourceVector> Catalogue(const FStudioPointRecordingDescriptor& Descriptor);
    /** Worker-only read. Coordinates/components retain original row order; no resampling. */
    bool Read(const IStudioField& Field,const FString& VectorId,FStudioSourceVectorRows& Out,
        const FStudioLoadCancellation& Cancel,FString& Error,const TArray<int32>& SelectedRows={});
}
