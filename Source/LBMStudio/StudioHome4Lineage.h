#pragma once
#include "StudioProject.h"
struct FStudioHome4LineageNode
{
    int32 RunIndex=INDEX_NONE,Parent=INDEX_NONE,Depth=0;
    FString Identity,Issue;
    TArray<FString> Changes;
};
namespace StudioHome4Lineage
{
    /** Every retained run appears once. Cyclic/ambiguous/dangling/hash-mismatched edges are surfaced, never guessed. */
    TArray<FStudioHome4LineageNode> Tree(const FStudioProject& Project);
    TArray<FString> Changes(const FStudioHome4Spec& Before,const FStudioHome4Spec& After);
}
