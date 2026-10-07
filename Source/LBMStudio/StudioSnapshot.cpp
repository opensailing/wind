#include "StudioSnapshot.h"
#include "StudioModel.h"
#include "StudioVolume.h"
#include "StudioPointRecording.h"
#include "Serialization/JsonReader.h"
#include "StudioFileDialog.h"
#include "ImageUtils.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"
#include "Async/Async.h"

bool StudioSnapshot::ValidSize(FIntPoint S)
{return S.X>=64&&S.Y>=64&&S.X<=4096&&S.Y<=4096&&int64(S.X)*S.Y<=16*1024*1024;}
FStudioSnapshotFraming StudioSnapshot::Frame(FIntPoint Source,FIntPoint Output)
{
    FStudioSnapshotFraming F;
    if(Source.X<=0||Source.Y<=0||Output.X<=0||Output.Y<=0)return F;
    const double Ratio=(double(Output.X)/Output.Y)/(double(Source.X)/Source.Y);
    if(Ratio<1)F.Span.X=Ratio;else F.Span.Y=1/Ratio;
    F.Minimum=(FVector2D(1,1)-F.Span)*.5;return F;
}
FVector2D FStudioSnapshotFraming::ToOutput(FVector2D P,FVector2D SourceSize,FVector2D OutputSize) const
{return (P/SourceSize-Minimum)/Span*OutputSize;}
FString StudioSnapshot::SourceMetadata(const IStudioField& Field,const FString& Scalar,const FStudioColorMapping& Mapping,bool AirMask)
{
    const auto V=Field.VolumeReconstruction();const auto Points=Field.OriginalPoints();
    if(!V||!V->OriginalGrid||!Points)return {};
    const auto& G=*V->OriginalGrid;auto Root=MakeShared<FJsonObject>();
    auto Vec=[](const FVector& P){TArray<TSharedPtr<FJsonValue>> A;for(int32 I=0;I<3;++I)A.Add(MakeShared<FJsonValueNumber>(P[I]));return A;};
    Root->SetStringField(TEXT("source_run_id"),G.SourceRunId);Root->SetStringField(TEXT("recipe_id"),G.RecipeId);Root->SetStringField(TEXT("lineage_id"),G.LineageId);
    Root->SetStringField(TEXT("manifest_sha256"),G.SourceManifestSHA256);Root->SetStringField(TEXT("manifest"),G.SourceManifestPath);
    Root->SetArrayField(TEXT("original_dimensions"),Vec(FVector(G.OriginalDimensions)));Root->SetArrayField(TEXT("selected_dimensions"),Vec(FVector(G.Dimensions)));
    Root->SetArrayField(TEXT("crop_minimum_indices"),Vec(FVector(G.CropMinimum)));Root->SetArrayField(TEXT("crop_maximum_exclusive_indices"),Vec(FVector(G.CropMaximum)));
    Root->SetNumberField(TEXT("preview_stride"),G.PreviewStride);Root->SetArrayField(TEXT("origin_meters"),Vec(G.OriginMeters));Root->SetArrayField(TEXT("spacing_meters"),Vec(G.SpacingMeters));
    Root->SetArrayField(TEXT("original_origin"),Vec(G.OriginalOrigin));Root->SetArrayField(TEXT("original_spacing"),Vec(G.OriginalSpacing));
    Root->SetStringField(TEXT("coordinate_units"),G.CoordinateUnits);Root->SetStringField(TEXT("velocity_units"),G.VelocityUnits);
    Root->SetStringField(TEXT("axis_order"),G.AxisOrder);Root->SetStringField(TEXT("metadata_order"),G.MetadataOrder);
    Root->SetStringField(TEXT("scene_axes"),TEXT("source X,Z,Y in meters"));Root->SetNumberField(TEXT("time_origin_seconds"),G.TimeOriginSeconds);
    Root->SetStringField(TEXT("nondimensional_time_origin"),TEXT("original lattice step zero / reference time"));
    const auto Map=StudioHome4Config::ToJSON(G.UnitContext());Root->SetObjectField(TEXT("units"),Map->GetObjectField(TEXT("units")));Root->SetObjectField(TEXT("reference"),Map->GetObjectField(TEXT("reference")));
    if(const auto Omega=Field.Scalar(TEXT("vorticity_magnitude"));Omega.IsSet())
    {
        const auto Held=StudioColor::Resolve(Field.Identity()->Dataset,Omega.GetValue(),{});
        const double Reference=FMath::Max(FMath::Abs(Held.Minimum),FMath::Abs(Held.Maximum));
        Root->SetNumberField(TEXT("vorticity_opacity_reference"),Reference>0?Reference:1);
        Root->SetStringField(TEXT("vorticity_opacity_reference_unit"),Omega->Unit);
    }
    Root->SetBoolField(TEXT("air_mask"),AirMask);Root->SetNumberField(TEXT("liquid_minimum"),G.LiquidMinimum);
    const auto* Descriptor=Points->Descriptor->FindField(Scalar);const auto* Values=Points->FindValues(Scalar);
    if(Descriptor)
    {
        Root->SetStringField(TEXT("derivative_validity_field"),Descriptor->ValidityMask);
        Root->SetNumberField(TEXT("first_frame_clipped_above"),Descriptor->FirstFrameClippedAbove);
    }
    FString Error;const auto Mask=StudioVolumes::SourceMask(*Points,*V,Scalar,false,Error,{},AirMask);
    if(Values&&Error.IsEmpty()&&Mask.Num()==Values->Num())
    {
        int64 Below=0,Above=0,Excluded=0,Supported=0;double Min=TNumericLimits<double>::Max(),Max=-TNumericLimits<double>::Max();
        for(int32 I=0;I<Values->Num();++I)
        {
            if(Mask[I]!=1){++Excluded;continue;}const double Value=(*Values)[I];++Supported;
            Min=FMath::Min(Min,Value);Max=FMath::Max(Max,Value);Below+=Value<Mapping.Minimum;Above+=Value>Mapping.Maximum;
        }
        Root->SetNumberField(TEXT("supported_original_nodes"),Supported);Root->SetNumberField(TEXT("excluded_original_nodes"),Excluded);
        Root->SetNumberField(TEXT("clipped_below"),Below);Root->SetNumberField(TEXT("clipped_above"),Above);
        if(Supported){Root->SetNumberField(TEXT("supported_minimum"),Min);Root->SetNumberField(TEXT("supported_maximum"),Max);}
        Root->SetStringField(TEXT("clipping_scope"),TEXT("selected original source nodes with active masks; not screen pixels"));
    }
    else Root->SetStringField(TEXT("clipping_status"),Error.IsEmpty()?TEXT("Original scalar values not retained"):Error);
    FString JSON;FJsonSerializer::Serialize(Root,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&JSON));return JSON;
}
FString StudioSnapshot::Metadata(const FStudioSnapshot& S)
{
    auto Root=MakeShared<FJsonObject>();Root->SetStringField(TEXT("format"),TEXT("LBMStudio.Snapshot"));Root->SetNumberField(TEXT("version"),1);
    Root->SetStringField(TEXT("unit_display"),S.UnitDisplay==EStudioHome4UnitDisplay::Lattice?TEXT("lattice"):S.UnitDisplay==EStudioHome4UnitDisplay::Physical?TEXT("physical"):TEXT("nondimensional"));
    TSharedPtr<FJsonObject> Original;if(!S.OriginalSourceJSON.IsEmpty()&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(S.OriginalSourceJSON),Original)&&Original)Root->SetObjectField(TEXT("original_grid"),Original);
    Root->SetStringField(TEXT("project"),S.Project.ToString());Root->SetStringField(TEXT("capture"),LexToString(S.Capture));
    if(!S.MetadataFile.IsEmpty())Root->SetStringField(TEXT("metadata_file"),S.MetadataFile);
    Root->SetStringField(TEXT("title"),S.SourceTitle);Root->SetStringField(TEXT("dataset"),S.Identity.Dataset);
    Root->SetStringField(TEXT("metadata_sha256"),S.Identity.MetadataSHA256);Root->SetStringField(TEXT("payload_sha256"),S.Identity.PayloadSHA256);
    Root->SetStringField(TEXT("reconstruction_sha256"),S.Identity.ReconstructionSHA256);
    Root->SetNumberField(TEXT("ordinal"),S.Identity.Ordinal);Root->SetNumberField(TEXT("frame"),S.Identity.Frame.Index);
    Root->SetNumberField(TEXT("time_seconds"),S.Identity.Frame.Time);Root->SetNumberField(TEXT("spatial_dimensions"),S.Identity.SpatialDimensions);
    auto Array=[](std::initializer_list<double> Values){TArray<TSharedPtr<FJsonValue>> Out;for(double V:Values)Out.Add(MakeShared<FJsonValueNumber>(V));return Out;};
    Root->SetArrayField(TEXT("size"),Array({double(S.Options.Size.X),double(S.Options.Size.Y)}));
    Root->SetArrayField(TEXT("presented_size"),Array({double(S.SourceSize.X),double(S.SourceSize.Y)}));
    Root->SetArrayField(TEXT("crop_minimum"),Array({S.Framing.Minimum.X,S.Framing.Minimum.Y}));
    Root->SetArrayField(TEXT("crop_span"),Array({S.Framing.Span.X,S.Framing.Span.Y}));
    TArray<TSharedPtr<FJsonValue>> Matrix;
    for(int32 R=0;R<4;++R)for(int32 C=0;C<4;++C)Matrix.Add(MakeShared<FJsonValueNumber>(S.Projection.M[R][C]));
    Root->SetArrayField(TEXT("projection_matrix_row_major"),Matrix);
    Root->SetArrayField(TEXT("source_offset_meters"),Array({S.Identity.SourceOffset.X,S.Identity.SourceOffset.Y,S.Identity.SourceOffset.Z}));
    Root->SetNumberField(TEXT("interpolation"),int32(S.Identity.Interpolation));
    auto Camera=MakeShared<FJsonObject>();const auto& C=S.Camera;
    Camera->SetArrayField(TEXT("position_meters"),Array({C.Position.X,C.Position.Y,C.Position.Z}));
    Camera->SetArrayField(TEXT("orientation_xyzw"),Array({C.Orientation.X,C.Orientation.Y,C.Orientation.Z,C.Orientation.W}));
    Camera->SetArrayField(TEXT("focus_meters"),Array({C.Focus.X,C.Focus.Y,C.Focus.Z}));
    Camera->SetNumberField(TEXT("horizontal_fov_degrees"),C.FieldOfView);Camera->SetBoolField(TEXT("orthographic"),C.bOrthographic);
    Camera->SetNumberField(TEXT("orthographic_width_meters"),C.OrthoWidth);Camera->SetNumberField(TEXT("orbit_distance_meters"),C.OrbitDistance);
    Camera->SetBoolField(TEXT("depth_clipping"),C.bDepthClipping);Camera->SetNumberField(TEXT("near_meters"),C.NearClipMeters);Camera->SetNumberField(TEXT("far_meters"),C.FarClipMeters);
    Root->SetObjectField(TEXT("camera"),Camera);
    auto Scalar=MakeShared<FJsonObject>();Scalar->SetStringField(TEXT("id"),S.Scalar.Id);Scalar->SetStringField(TEXT("label"),S.Scalar.Label);
    Scalar->SetStringField(TEXT("unit"),S.Scalar.Unit);Scalar->SetStringField(TEXT("origin"),S.Scalar.Origin);
    Scalar->SetArrayField(TEXT("source_range"),Array({S.Scalar.Minimum,S.Scalar.Maximum}));
    Scalar->SetArrayField(TEXT("display_range"),Array({S.Mapping.Minimum,S.Mapping.Maximum}));
    Scalar->SetNumberField(TEXT("palette"),S.Mapping.Palette);Scalar->SetBoolField(TEXT("manual_range"),S.Mapping.bManualRange);
    for(const auto& Pair:TArray<TPair<FString,FLinearColor>>{{TEXT("low"),S.Mapping.LowColor},{TEXT("middle"),S.Mapping.MiddleColor},{TEXT("high"),S.Mapping.HighColor}})
        Scalar->SetArrayField(Pair.Key,Array({Pair.Value.R,Pair.Value.G,Pair.Value.B,Pair.Value.A}));
    Root->SetObjectField(TEXT("scalar"),Scalar);Root->SetObjectField(TEXT("inspection_objects"),StudioInspectionObjects::ToJSON(S.Objects));
    Root->SetNumberField(TEXT("display_schema_version"),FStudioProject::CurrentVersion);
    Root->SetObjectField(TEXT("display_settings"),StudioProjectIO::ViewToJSON(S.DisplaySettings));
    Root->SetBoolField(TEXT("volume_renderer_active"),S.bVolumeRendererActive);
    auto Bounds=MakeShared<FJsonObject>();Bounds->SetBoolField(TEXT("valid"),S.FlowBounds.IsValid!=0);
    if(S.FlowBounds.IsValid)
    {
        Bounds->SetArrayField(TEXT("minimum_meters"),Array({S.FlowBounds.Min.X,S.FlowBounds.Min.Y,S.FlowBounds.Min.Z}));
        Bounds->SetArrayField(TEXT("maximum_meters"),Array({S.FlowBounds.Max.X,S.FlowBounds.Max.Y,S.FlowBounds.Max.Z}));
    }
    Root->SetObjectField(TEXT("flow_bounds"),Bounds);
    auto Vectors=MakeShared<FJsonObject>();
    Vectors->SetNumberField(TEXT("sample_count"),S.Vectors.SampleCount);
    Vectors->SetNumberField(TEXT("glyph_count"),S.Vectors.GlyphCount);
    Vectors->SetNumberField(TEXT("sampled_maximum_speed_m_per_s"),S.Vectors.MaximumSpeed);
    Vectors->SetNumberField(TEXT("reference_length_m"),S.Vectors.ReferenceLengthMeters);
    Vectors->SetBoolField(TEXT("uniform_length"),S.Vectors.bUniformLength);
    Root->SetObjectField(TEXT("vector_display"),Vectors);
    auto Streams=MakeShared<FJsonObject>();
    Streams->SetNumberField(TEXT("seeds"),S.Streams.Seeds);Streams->SetNumberField(TEXT("lines"),S.Streams.Lines);
    Streams->SetNumberField(TEXT("segments"),S.Streams.Segments);Streams->SetNumberField(TEXT("attempted_steps"),S.Streams.Attempts);
    Streams->SetNumberField(TEXT("tube_diameter_m"),S.Streams.WidthMeters);
    Streams->SetBoolField(TEXT("work_limit_reached"),S.Streams.bBudgetExhausted);
    Streams->SetBoolField(TEXT("automatic_inlet"),S.Streams.bAutomaticSeeds);
    Streams->SetStringField(TEXT("method"),S.Streams.Method==EStudioStreamMethod::DormandPrince45?TEXT("Instantaneous velocity streamlines; adaptive Dormand-Prince RK45 arc length"):TEXT("Instantaneous velocity streamlines; arc-length midpoint integration"));
    Streams->SetNumberField(TEXT("rejected_attempts"),S.Streams.RejectedAttempts);Streams->SetNumberField(TEXT("velocity_evaluations"),S.Streams.VelocityEvaluations);
    Streams->SetNumberField(TEXT("scalar_evaluations"),S.Streams.ScalarEvaluations);Streams->SetNumberField(TEXT("support_evaluations"),S.Streams.SupportEvaluations);
    if(S.DisplaySettings.bHome4Vorticity&&S.Scalar.Id==TEXT("q")){auto Transfer=MakeShared<FJsonObject>();Transfer->SetStringField(TEXT("color"),TEXT("q"));Transfer->SetStringField(TEXT("opacity"),TEXT("vorticity_magnitude squared / held first-frame percentile squared"));Transfer->SetStringField(TEXT("precision_cap"),TEXT("Extinction weights capped at 1e20 for finite GPU transport"));Root->SetObjectField(TEXT("vorticity_transfer"),Transfer);}
    Root->SetObjectField(TEXT("streamline_display"),Streams);
    auto Mesh=MakeShared<FJsonObject>();Mesh->SetNumberField(TEXT("triangles"),S.Mesh.Triangles);
    Mesh->SetBoolField(TEXT("derived"),S.Mesh.bDerived);Mesh->SetStringField(TEXT("notice"),S.Mesh.Notice);
    Mesh->SetBoolField(TEXT("field_fill_hidden"),S.Mesh.bFieldFillHidden);
    Root->SetObjectField(TEXT("triangle_mesh_display"),Mesh);
    auto Notices=MakeShared<FJsonObject>();
    for(const auto& Notice:S.SliceNotices)Notices->SetStringField(Notice.Key.ToString(),Notice.Value);
    Root->SetObjectField(TEXT("slice_rendering_status"),Notices);
    auto Overlay=MakeShared<FJsonObject>();Overlay->SetStringField(TEXT("coordinate_system"),TEXT("scene_meters"));
    TArray<TSharedPtr<FJsonValue>> Lines,Markers;
    for(const auto& Line:S.Overlay.Lines)
    {
        auto Item=MakeShared<FJsonObject>();Item->SetStringField(TEXT("object"),Line.Object.ToString());
        Item->SetArrayField(TEXT("a"),Array({Line.A.X,Line.A.Y,Line.A.Z}));
        Item->SetArrayField(TEXT("b"),Array({Line.B.X,Line.B.Y,Line.B.Z}));
        Item->SetNumberField(TEXT("width_slate_units"),Line.Width);Lines.Add(MakeShared<FJsonValueObject>(Item));
    }
    for(const auto& Marker:S.Overlay.Markers)
    {
        auto Item=MakeShared<FJsonObject>();Item->SetStringField(TEXT("object"),Marker.Object.ToString());
        Item->SetArrayField(TEXT("position"),Array({Marker.Position.X,Marker.Position.Y,Marker.Position.Z}));
        Item->SetStringField(TEXT("label"),Marker.Label);Markers.Add(MakeShared<FJsonValueObject>(Item));
    }
    Overlay->SetArrayField(TEXT("lines"),Lines);Overlay->SetArrayField(TEXT("markers"),Markers);
    Root->SetObjectField(TEXT("resolved_overlay"),Overlay);
    Root->SetStringField(TEXT("selected_object"),S.SelectedObject.ToString());Root->SetBoolField(TEXT("annotations"),S.Options.bAnnotations);
    Root->SetBoolField(TEXT("legend"),S.Options.bLegend);Root->SetBoolField(TEXT("frame_label"),S.Options.bFrameInfo);
    Root->SetStringField(TEXT("probe_samples_csv"),S.ProbeCSV);
    FString Out;auto Writer=TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    FJsonSerializer::Serialize(Root,Writer);return Out;
}
bool StudioSnapshot::Encode(const FStudioSnapshot& S,TArray64<uint8>& PNG,FString& Error)
{
    if(!ValidSize(S.Options.Size)||S.Pixels.Num()!=int64(S.Options.Size.X)*S.Options.Size.Y||S.SourceSize.X<=0||S.SourceSize.Y<=0||
        !S.Project.IsValid()||S.Identity.Dataset.IsEmpty()||S.Identity.Ordinal<0||!FMath::IsFinite(S.Identity.Frame.Time))
    {Error=TEXT("The snapshot is missing its image or recorded frame identity.");return false;}
    TArray<FColor> Opaque=S.Pixels;for(auto& P:Opaque)P.A=255;
    FImageUtils::PNGCompressImageArray(S.Options.Size.X,S.Options.Size.Y,Opaque,PNG);
    if(PNG.Num()<20){Error=TEXT("Could not encode the snapshot image.");return false;}
    const FTCHARToUTF8 JSON(*Metadata(S));
    TArray64<uint8> Data;const ANSICHAR Header[]="LBMStudio\0\0\0\0\0";
    Data.Append(reinterpret_cast<const uint8*>(Header),sizeof(Header)-1);Data.Append(reinterpret_cast<const uint8*>(JSON.Get()),JSON.Length());
    TArray64<uint8> Chunk;
    auto BigEndian=[&](uint32 V){Chunk.Add(V>>24);Chunk.Add(V>>16);Chunk.Add(V>>8);Chunk.Add(V);};
    BigEndian(Data.Num());const uint8 Type[]={'i','T','X','t'};Chunk.Append(Type,4);Chunk.Append(Data);
    BigEndian(FCrc::MemCrc32(Chunk.GetData()+4,Chunk.Num()-4));
    PNG.Insert(Chunk.GetData(),Chunk.Num(),PNG.Num()-12);Error.Empty();return true;
}
FStudioSnapshotExportTask::~FStudioSnapshotExportTask()
{Cancel();if(Pending.IsValid())Pending.Wait();}
bool FStudioSnapshotExportTask::Start(FStudioSnapshot Snapshot,const FString& Path)
{
    check(IsInGameThread());if(Pending.IsValid()||Path.IsEmpty()||!StudioSnapshot::ValidSize(Snapshot.Options.Size))return false;
    FModuleManager::Get().LoadModuleChecked<IModuleInterface>(TEXT("ImageWrapper"));
    Progress=MakeShared<std::atomic<EStudioSnapshotExportState>,ESPMode::ThreadSafe>(EStudioSnapshotExportState::Encoding);
    Pending=Async(EAsyncExecution::ThreadPool,[Snapshot=MoveTemp(Snapshot),Path,State=Progress
#if WITH_DEV_AUTOMATION_TESTS
        ,BeforeWrite=MoveTemp(BeforeWriteForAutomation)
#endif
    ]() mutable
    {
        FStudioSnapshotExportResult Result;Result.Path=Path;Result.Frame=Snapshot.Identity.Frame;
        Snapshot.MetadataFile=FPaths::GetBaseFilename(Path)+TEXT(".")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".json");
        Result.SidecarPath=FPaths::GetPath(Path)/Snapshot.MetadataFile;
        TArray64<uint8> PNG;
        if(State->load()==EStudioSnapshotExportState::Cancelled){Result.bCancelled=true;return Result;}
        if(!StudioSnapshot::Encode(Snapshot,PNG,Result.Error))
        {
            auto Expected=EStudioSnapshotExportState::Encoding;
            if(!State->compare_exchange_strong(Expected,EStudioSnapshotExportState::Complete)&&Expected==EStudioSnapshotExportState::Cancelled)
            {Result.bCancelled=true;Result.Error.Empty();}
            return Result;
        }
#if WITH_DEV_AUTOMATION_TESTS
        if(BeforeWrite)BeforeWrite();
#endif
        auto Expected=EStudioSnapshotExportState::Encoding;
        if(!State->compare_exchange_strong(Expected,EStudioSnapshotExportState::Writing)){Result.bCancelled=true;return Result;}
        const FStudioFileAccess Access(Path);
        // Publish the uniquely named complete metadata first. Atomic PNG replacement
        // is the commit point; its embedded metadata names this exact sidecar.
        // A failed replacement leaves the old image and its old metadata intact.
        if(!FFileHelper::SaveStringToFile(StudioSnapshot::Metadata(Snapshot),*Result.SidecarPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&IFileManager::Get(),FILEWRITE_NoReplaceExisting))
            Result.Error=TEXT("Could not write the snapshot JSON sidecar. Existing image retained.");
        else
        {
            Result.bSuccess=StudioFileDialog::WriteAtomicBytes(Path,PNG,Result.Error);
            if(!Result.bSuccess)IFileManager::Get().Delete(*Result.SidecarPath,false,true);
        }
        State->store(EStudioSnapshotExportState::Complete);return Result;
    });return true;
}
bool FStudioSnapshotExportTask::Cancel()
{
    if(!Progress)return false;auto Expected=EStudioSnapshotExportState::Encoding;
    return Progress->compare_exchange_strong(Expected,EStudioSnapshotExportState::Cancelled);
}
EStudioSnapshotExportState FStudioSnapshotExportTask::State() const
{return Progress?Progress->load():EStudioSnapshotExportState::Complete;}
TOptional<FStudioSnapshotExportResult> FStudioSnapshotExportTask::Poll()
{if(!Pending.IsValid()||!Pending.IsReady())return {};return Pending.Consume();}
