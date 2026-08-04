// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizRuntime.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogFlowViz);

#define LOCTEXT_NAMESPACE "FFlowVizRuntimeModule"

void FFlowVizRuntimeModule::StartupModule()
{
	// Map /Plugin/FlowViz to the plugin's Shaders directory so global shaders and
	// material expressions can #include from a stable virtual path.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
	if (Plugin.IsValid())
	{
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/FlowViz"), ShaderDir);
		UE_LOG(LogFlowViz, Log, TEXT("Mapped virtual shader path /Plugin/FlowViz -> %s"), *ShaderDir);
	}
	else
	{
		UE_LOG(LogFlowViz, Error, TEXT("FlowVizRuntime plugin not found; shader path mapping skipped."));
	}
}

void FFlowVizRuntimeModule::ShutdownModule()
{
	ResetAllShaderSourceDirectoryMappings();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFlowVizRuntimeModule, FlowVizRuntime)
