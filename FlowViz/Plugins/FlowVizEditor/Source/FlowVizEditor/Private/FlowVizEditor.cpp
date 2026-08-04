// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizEditor.h"

#include "FlowVizRuntime.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FFlowVizEditorModule"

void FFlowVizEditorModule::StartupModule()
{
	UE_LOG(LogFlowViz, Log, TEXT("FlowVizEditor module started."));
}

void FFlowVizEditorModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFlowVizEditorModule, FlowVizEditor)
