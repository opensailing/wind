#pragma once

#include "Commandlets/Commandlet.h"
#include "StudioRenderValidationCommandlet.generated.h"

/** Unattended GPU regression using the production flow scene and attributed
 * original recordings. Owns an isolated world; never creates a game viewport,
 * changes a user's project, sends desktop input or saves screenshots. */
UCLASS()
class UStudioRenderValidationCommandlet final : public UCommandlet
{
    GENERATED_BODY()
public:
    UStudioRenderValidationCommandlet();
    virtual int32 Main(const FString& Params) override;
};
