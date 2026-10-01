#pragma once
#include "CoreMinimal.h"

/** Optional presentation movie alongside lossless PNG originals. One selected
 * original becomes one movie frame at a fixed playback rate, never a new CFD
 * sample or an implied physical-time rate. */
struct FStudioMovieOptions
{
    bool bEnabled=false;
    int32 FrameRate=20;
};

namespace StudioMovie
{
    bool Supported();
    bool Validate(const FStudioMovieOptions& Options,FIntPoint Size,FString& Error);
}

/** Worker-thread-only encoder. Owns native resources on that worker; never
 * call methods concurrently. Cancellation is observed through the callback,
 * including native backpressure/finalization. Destruction cancels unfinished
 * output. The caller owns private staging and atomic directory publication. */
class IStudioMovieEncoder
{
public:
    virtual ~IStudioMovieEncoder()=default;
    static TUniquePtr<IStudioMovieEncoder> Create();
    virtual bool Begin(const FString& Path,FIntPoint Size,int32 FrameRate,const FString& Description,FString& Error)=0;
    virtual bool Append(TConstArrayView<FColor> Pixels,TFunctionRef<bool()> Cancelled,FString& Error)=0;
    virtual bool Finish(TFunctionRef<bool()> Cancelled,FString& Error)=0;
};
