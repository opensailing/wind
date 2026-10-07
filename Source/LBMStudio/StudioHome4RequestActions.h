#pragma once
#include "StudioHome4Config.h"
namespace StudioHome4RequestActions
{
    bool Smoke(const FStudioHome4Spec&,int64 Steps,int64 Cadence,FStudioHome4Spec&,FString& Error);
    bool Figure(const FStudioHome4Spec&,FStudioHome4Spec&,FString& Error);
    bool Safeguards(const FStudioHome4Spec&,FStudioHome4Spec&,FString& Error);
    FString CompleteProtocol(const FStudioHome4Spec&);
}
