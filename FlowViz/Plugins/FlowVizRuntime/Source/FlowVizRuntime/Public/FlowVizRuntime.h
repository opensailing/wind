// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Logging/LogMacros.h"

/** Primary log category for all FlowViz runtime systems. */
FLOWVIZRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogFlowViz, Log, All);

/**
 * FlowViz runtime module.
 *
 * Loaded at PostConfigInit so shader source mappings are registered before any
 * global shader is compiled.
 */
class FFlowVizRuntimeModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface
};
