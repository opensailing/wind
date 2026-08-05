// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizRuntime.h"

#include "CFDViz/CFDVizCrc32C.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Render/FlowVizVolumeRayMarchDispatcher.h"
#include "ShaderCore.h"
#include "UI/FlowVizWorkspaceTab.h"

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

	// Fail loudly at startup rather than silently mis-validating every CFDViz file.
	// CFDVizCrc32C::SelfCheck logs the specific mismatch on failure.
	ensureMsgf(CFDViz::Crc32C::SelfCheck(), TEXT("CFDViz CRC-32C self-check failed; see the log above."));

	// INSTALL THE RAY-MARCH DISPATCHER. Without this line the ray-marcher and the
	// volume component are two finished halves of a feature with nothing between
	// them: FFlowVizVolumeSceneProxy::GetDynamicMeshElements guards its dispatch
	// on FlowVizVolumeRayMarch::GetDispatcher() and draws nothing when it is
	// null, which is what a volume in a real map did while the suite was green.
	// Every rendering test installed its own dispatcher as a precondition, so no
	// test could observe that production installed none.
	FlowVizVolumeRayMarchProduction::Register();

	// REGISTER THE WORKSPACE TAB. The same argument as the line above, applied to
	// the UI: without this, every Slate panel in this plugin is a definition that
	// nothing in the running application ever constructs, and no panel test can
	// tell - each one SNew's its own widget as a precondition of asserting
	// anything about it. FlowViz.UI.Workspace.Wiring is the test that constructs
	// nothing and asks the global tab manager what startup left behind.
	FlowVizWorkspaceTab::Register();
}

void FFlowVizRuntimeModule::ShutdownModule()
{
	FlowVizWorkspaceTab::Unregister();
	FlowVizVolumeRayMarchProduction::Unregister();
	ResetAllShaderSourceDirectoryMappings();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFlowVizRuntimeModule, FlowVizRuntime)
