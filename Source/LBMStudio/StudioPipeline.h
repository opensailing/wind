#pragma once
#include "StudioProject.h"

/** Metadata for each enabled operation after ordered validation. No field IO,
 * geometry or invented samples occur while compiling a saved recipe. */
struct FStudioPipelineStage
{
    FGuid OperationId;
    EStudioPipelineOperation Kind=EStudioPipelineOperation::Field;
    FString Field,Unit;
    int32 OutputDimensions=0;
};

namespace StudioPipelines
{
    constexpr int32 MaxEntries=64;
    constexpr int32 MaxOperations=32;
    constexpr int32 MaxProbeSamples=1024;
    bool IsValid(const FStudioSavedPipeline& Pipeline,FString& Error);
    bool IsValid(const TArray<FStudioSavedPipeline>& Pipelines,FString& Error);
    bool Equals(const FStudioSavedPipeline& A,const FStudioSavedPipeline& B);
    bool Equals(const TArray<FStudioSavedPipeline>& A,const TArray<FStudioSavedPipeline>& B);
    TArray<TSharedPtr<FJsonValue>> ToJSON(const TArray<FStudioSavedPipeline>& Pipelines);
    bool FromJSON(const TArray<TSharedPtr<FJsonValue>>& JSON,TArray<FStudioSavedPipeline>& Out,FString& Error);
    int64 StoredBytes(const TArray<FStudioSavedPipeline>& Pipelines);
    /** Validate the saved original frame and field/unit dependencies against
     * its descriptor, including disabled/forward derived dependencies. Previous
     * Out remains intact on failure; actual arrays are verified on evaluation. */
    bool Compile(const FStudioSavedPipeline& Pipeline,const FStudioRecordingDescriptor& Descriptor,
        TArray<FStudioPipelineStage>& Out,FString& Error);
    /** Transactional reorder/toggle by stable operation ID. Invalid order keeps
     * the entire previous recipe, including disabled entries and exact values. */
    bool Move(FStudioSavedPipeline& Pipeline,const FGuid& OperationId,int32 ToIndex,FString& Error);
    bool SetEnabled(FStudioSavedPipeline& Pipeline,const FGuid& OperationId,bool bEnabled,FString& Error);
}
