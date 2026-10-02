#include "StudioHelp.h"
#include "StudioModel.h"
#include "StudioScene.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformProperties.h"
#include "RHI.h"
#include "Serialization/JsonSerializer.h"

FString StudioHelp::WorkspaceName(EStudioWorkspace Workspace)
{
    const TCHAR* Names[]={TEXT("Dashboard"),TEXT("Projects"),TEXT("Geometry"),TEXT("Domain"),TEXT("Materials"),
        TEXT("Boundary Conditions"),TEXT("Meshing"),TEXT("Solve"),TEXT("Monitors"),TEXT("Results"),TEXT("Post-Processing"),TEXT("Settings"),TEXT("Setup")};
    const int32 Index=int32(Workspace);return Index>=0&&Index<UE_ARRAY_COUNT(Names)?Names[Index]:TEXT("Workspace");
}
FString StudioHelp::Guidance(EStudioWorkspace Workspace,bool Harness)
{
    switch(Workspace)
    {
    case EStudioWorkspace::Dashboard:return TEXT("Open a recent project or create one from the header. The dashboard summarizes the active project and its saved runs. Missing files can be located from Projects.");
    case EStudioWorkspace::Projects:return TEXT("Filter recent projects by name or path, or show favorites. Open keeps the current project until its files are verified. Use Locate for a moved project; its saved source identities must still match.");
    case EStudioWorkspace::Geometry:return TEXT("Import STL or OBJ from the header, then choose units and orientation before committing the preview. Inspect diagnostics and select an object to edit its transform. Apply commits the draft; Revert retains the saved object. Source files stay unchanged.");
    case EStudioWorkspace::Domain:return TEXT("Set solver-space extents in meters, or pad around the imported geometry. Apply the draft and inspect named faces in the preview. Camera movement changes the preview perspective; it does not change the domain.");
    case EStudioWorkspace::Materials:return TEXT("Create a fluid or solid material and assign the domain fluid. Enter properties with their displayed units, then Apply. Flow conditions read viscosity from that assigned fluid. Missing properties remain unspecified.");
    case EStudioWorkspace::BoundaryConditions:return TEXT("Select a stable domain face or geometry patch, then assign a supported boundary type and its values. Inspect the highlighted target and validation messages before applying. Changed geometry can invalidate patch assignments.");
    case EStudioWorkspace::Meshing:return TEXT("Set lattice dimensions and inspect the cell spacing and memory estimate. Apply resolution, then build the bounded occupancy preview. Preview classification is preparation guidance; it is not a completed solver run.");
    case EStudioWorkspace::Solve:return Harness?
        TEXT("Run submits a frozen case to the control harness. Pause, Step and Checkpoint test acknowledgements; the harness computes no CFD. Move the camera at any time. The displayed field still belongs to its original recording. Use Snapshot beside the timeline for images or movies; Export in the header saves field data."):
        TEXT("Run plays the published recording; Pause, Step and the timeline inspect original snapshots. Move the camera at any time without changing playback or the case. Fit frames the displayed domain; Flow overview frames the wing and nearby wake. Use Snapshot beside the timeline for images or movies; Export in the header saves field data.");
    case EStudioWorkspace::Monitors:return TEXT("Choose a published force history, an original OpenFOAM residual log, or generate a history from a saved probe. Their physical times and source identities remain separate from field playback. Select a sample to inspect its original frame when that link is available. Export CSV preserves original rows.");
    case EStudioWorkspace::Results:return TEXT("Browse recordings and saved runs here. Recording details own original source provenance, units, topology, hashes and exact-frame inspection. Comparisons retain independent sources, time alignment and cameras. Toolbar playback continues to control the Solve recording.");
    case EStudioWorkspace::PostProcessing:return TEXT("Choose an original source and frame, then edit an ordered recipe. Apply commits recipe edits; Evaluate builds its output. Cancel or a failed evaluation retains the previous result. The header Export saves the current completed evaluation, with its original and derived identities.");
    case EStudioWorkspace::Settings:return TEXT("Application preferences are being implemented. Camera and display controls currently belong to Solve; material units belong to Materials. Help does not change those settings.");
    case EStudioWorkspace::Setup:return TEXT("Use Solve's Setup inspector for next-run flow and run requests. Apply retained edits before saving or submitting a new control run. These requests do not recalculate the published recording.");
    }
    return TEXT("Choose a workspace from the sidebar. Help describes the current workspace without changing the case or view.");
}
FString StudioHelp::ApplicationVersion()
{
    FString Version;
    if(GConfig)GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"),TEXT("ProjectVersion"),Version,GGameIni);
    return Version.IsEmpty()?TEXT("development"):Version;
}
FString StudioHelp::Diagnostics(const FStudioModel& M,const AStudioScene* Scene)
{
    auto Root=MakeShared<FJsonObject>();Root->SetStringField(TEXT("format"),TEXT("LBMStudio.Diagnostics"));Root->SetNumberField(TEXT("version"),1);
    Root->SetStringField(TEXT("observed_at_utc"),FDateTime::UtcNow().ToIso8601());
    Root->SetStringField(TEXT("application_version"),ApplicationVersion());Root->SetStringField(TEXT("unreal_version"),FEngineVersion::Current().ToString());
    Root->SetStringField(TEXT("platform"),FPlatformProperties::PlatformName());
    Root->SetStringField(TEXT("build_configuration"),LexToString(FApp::GetBuildConfiguration()));
    Root->SetStringField(TEXT("rhi"),GDynamicRHI?GDynamicRHI->GetName():TEXT("unavailable"));
    Root->SetStringField(TEXT("workspace"),WorkspaceName(M.Workspace));
    Root->SetStringField(TEXT("project_id"),M.Project.Id.ToString());Root->SetStringField(TEXT("project_name"),M.Project.Name);
    Root->SetStringField(TEXT("case_id"),M.Project.Draft.Id.ToString());Root->SetStringField(TEXT("case_revision"),LexToString(M.Project.Draft.Revision));
    Root->SetBoolField(TEXT("project_has_unsaved_changes"),M.HasUnsavedChanges());
    Root->SetStringField(TEXT("control_mode"),M.Project.bControlHarness?TEXT("control harness; no CFD computed"):TEXT("recording replay"));
    Root->SetStringField(TEXT("playback_state"),M.Solver?M.StatusText():TEXT("Data unavailable"));
    Root->SetNumberField(TEXT("playback_ordinal"),M.PlaybackFrame);Root->SetBoolField(TEXT("reviewing_original_frame"),M.bReviewing);
    Root->SetStringField(TEXT("job_state"),StudioJobs::StateName(M.Job().State()));
    Root->SetStringField(TEXT("notice"),M.Notice);
    if(M.Solver)
    {
        const auto& D=M.Solver->Descriptor();auto Source=MakeShared<FJsonObject>();
        Source->SetStringField(TEXT("dataset"),D.Id);Source->SetStringField(TEXT("title"),D.Title);
        Source->SetStringField(TEXT("source_url"),D.SourceURL);Source->SetStringField(TEXT("metadata_sha256"),D.MetadataSHA256);
        Source->SetStringField(TEXT("payload_sha256"),D.PayloadSHA256);Source->SetNumberField(TEXT("spatial_dimensions"),D.SpatialDimensions);
        Source->SetNumberField(TEXT("frame_count"),M.Solver->FrameCount());
        if(M.SelectedFrame>=0&&M.SelectedFrame<M.Solver->FrameCount())
        {
            const auto F=M.Solver->EvaluateFrame(M.SelectedFrame);
            Source->SetNumberField(TEXT("requested_ordinal"),M.SelectedFrame);Source->SetNumberField(TEXT("requested_step"),F.Index);
            Source->SetNumberField(TEXT("requested_time_seconds"),F.Time);
        }
        Root->SetObjectField(TEXT("recording"),Source);
    }
    else Root->SetField(TEXT("recording"),MakeShared<FJsonValueNull>());
    auto Renderer=MakeShared<FJsonObject>();Renderer->SetBoolField(TEXT("available"),IsValid(Scene));
    if(IsValid(Scene))
    {
        Renderer->SetBoolField(TEXT("current_frame"),Scene->HasCurrentFrame());Renderer->SetNumberField(TEXT("capture_count"),Scene->GetCaptureCount());
        if(Scene->HasPresentedFrame())
        {
            Renderer->SetStringField(TEXT("presented_dataset"),Scene->PresentedDatasetId());
            const auto F=Scene->PresentedFrame();Renderer->SetNumberField(TEXT("presented_step"),F.Index);
            Renderer->SetNumberField(TEXT("presented_time_seconds"),F.Time);
        }
        const auto Stats=Scene->ResourceStats();Renderer->SetNumberField(TEXT("workers"),Stats.Workers);
        Renderer->SetNumberField(TEXT("mesh_bytes"),Stats.MeshBytes);Renderer->SetNumberField(TEXT("scalar_texture_bytes"),Stats.ScalarTextureBytes);
        const auto C=Scene->CameraState();TArray<TSharedPtr<FJsonValue>> Position;
        for(int32 I=0;I<3;++I)Position.Add(MakeShared<FJsonValueNumber>(C.Position[I]));
        Renderer->SetArrayField(TEXT("camera_position_meters"),Position);Renderer->SetBoolField(TEXT("orthographic"),C.bOrthographic);
    }
    Root->SetObjectField(TEXT("renderer"),Renderer);
    FString JSON;FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&JSON));return JSON;
}
