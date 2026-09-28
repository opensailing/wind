#include "StudioFieldExport.h"
#include "StudioModel.h"
#include "StudioInspectionObjects.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace
{
bool SameExportFrame(const FStudioFieldIdentity& A,const FStudioFieldIdentity& B)
{
    return A.Dataset==B.Dataset&&A.MetadataSHA256==B.MetadataSHA256&&A.PayloadSHA256==B.PayloadSHA256&&
        A.ReconstructionSHA256==B.ReconstructionSHA256&&A.Ordinal==B.Ordinal&&A.Frame.Index==B.Frame.Index&&
        A.Frame.Time==B.Frame.Time&&A.SpatialDimensions==B.SpatialDimensions&&A.SourceOffset==B.SourceOffset&&A.Interpolation==B.Interpolation;
}
bool ExportName(const FString& S)
{if(S.IsEmpty()||S.Len()>256)return false;for(const TCHAR C:S)if(C<32)return false;return true;}
}
bool StudioFieldExport::Validate(const FStudioFieldExportRequest& R,FArchive& Archive,
    const FStudioLoadCancellation& Cancellation,FStudioFieldExportResult& Out)
{
    if(Cancellation&&Cancellation->load()){Out.bCancelled=true;Out.Error=TEXT("Field export cancelled.");return false;}
    const auto Identity=R.Field?R.Field->Identity():TOptional<FStudioFieldIdentity>();
    if(!R.Field||!R.Field->IsValid()||!Identity||!Archive.IsSaving()||
        !StudioInspectionObjects::IsValid(FStudioInspectionSource{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256})||
        Identity->Ordinal<0||Identity->Frame.Index<0||!FMath::IsFinite(Identity->Frame.Time)||Identity->SourceOffset.ContainsNaN()||
        (Identity->SpatialDimensions!=2&&Identity->SpatialDimensions!=3)||
        (R.Coordinates!=EStudioExportCoordinates::Source&&R.Coordinates!=EStudioExportCoordinates::Scene)||
        (R.Format!=EStudioFieldExportFormat::VTK&&R.Format!=EStudioFieldExportFormat::CSV))
    {Out.Error=TEXT("A verified original frame and writable staging file are required.");return false;}
    Out.Identity=*Identity;Out.Points=R.Field->OriginalPointCount();Out.Triangles=R.Field->OriginalTriangleCount();
    if(Out.Points<=0||Out.Points>4000000||Out.Triangles<0||Out.Triangles>8000000||R.Scalars.IsEmpty()||R.Scalars.Num()>64)
    {Out.Error=TEXT("Original point/connectivity access and 1–64 selected scalar arrays are required.");return false;}
    TSet<FString> Names;
    for(const auto& Id:R.Scalars)
    {
        const auto S=R.Field->Scalar(Id);
        if(!ExportName(Id)||Names.Contains(Id)||!S||S->Id!=Id)
        {Out.Error=TEXT("Select distinct scalar arrays supplied by this original recording.");return false;}
        Names.Add(Id);
    }
    return true;
}
TSharedPtr<const IStudioField,ESPMode::ThreadSafe> StudioFieldExport::LoadScalar(const FStudioFieldExportRequest& R,
    const FString& Id,const FStudioLoadCancellation& Cancellation,FStudioFieldExportResult& Out)
{
    auto Field=R.Field;double First;
    if(!Field->OriginalScalar(0,Id,First))Field=R.Field->LoadScalarSnapshot(Id,Cancellation,Out.Error);
    if(Cancellation&&Cancellation->load()){Out.bCancelled=true;Out.Error=TEXT("Field export cancelled.");return {}; }
    if(!Out.Error.IsEmpty())return {};
    const auto Actual=Field?Field->Identity():TOptional<FStudioFieldIdentity>();
    const auto S=Field?Field->Scalar(Id):TOptional<FStudioScalarDescriptor>();const auto Expected=*R.Field->Scalar(Id);
    if(!Field||!Field->IsValid()||!Actual||!SameExportFrame(Out.Identity,*Actual)||Field->OriginalPointCount()!=Out.Points||
        !S||S->Id!=Expected.Id||S->Unit!=Expected.Unit||S->Origin!=Expected.Origin||S->Label!=Expected.Label||
        S->Minimum!=Expected.Minimum||S->Maximum!=Expected.Maximum||
        Field->ScalarExpression(Id)!=R.Field->ScalarExpression(Id))
    {Out.Error=TEXT("A scalar read differs from the pinned original frame or its scientific meaning.");return {};}
    return Field;
}
bool StudioFieldExport::SamePoint(const IStudioField& Original,const IStudioField& Loaded,int32 Row)
{
    int64 A,B;FVector P,Q;
    return Original.OriginalPoint(Row,A,P)&&Loaded.OriginalPoint(Row,B,Q)&&A==B&&P==Q;
}

FString StudioFieldExport::Metadata(const FStudioFieldExportRequest& R,const FStudioFieldIdentity& I,int32 Triangles,const FString& PointIdName)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("format"),R.Format==EStudioFieldExportFormat::CSV?TEXT("LBMStudio.OriginalFieldCSV"):TEXT("LBMStudio.OriginalField"));J->SetNumberField(TEXT("version"),1);
    J->SetStringField(TEXT("dataset"),I.Dataset);J->SetStringField(TEXT("metadata_sha256"),I.MetadataSHA256);
    J->SetStringField(TEXT("payload_sha256"),I.PayloadSHA256);J->SetStringField(TEXT("view_reconstruction_sha256"),I.ReconstructionSHA256);
    J->SetNumberField(TEXT("frame_ordinal"),I.Ordinal);J->SetNumberField(TEXT("source_step"),I.Frame.Index);J->SetNumberField(TEXT("source_time_seconds"),I.Frame.Time);
    J->SetNumberField(TEXT("spatial_dimensions"),I.SpatialDimensions);J->SetStringField(TEXT("coordinate_unit"),TEXT("m"));
    J->SetStringField(TEXT("coordinate_system"),R.Coordinates==EStudioExportCoordinates::Source?TEXT("source_xyz"):TEXT("scene_xzy_plus_offset"));
    TArray<TSharedPtr<FJsonValue>> Offset;for(int32 A=0;A<3;++A)Offset.Add(MakeShared<FJsonValueNumber>(I.SourceOffset[A]));
    J->SetArrayField(TEXT("source_to_scene_offset_meters"),Offset);J->SetStringField(TEXT("point_id_array"),PointIdName);
    J->SetStringField(TEXT("topology"),R.Format==EStudioFieldExportFormat::CSV?TEXT("original_points_no_connectivity"):(Triangles?TEXT("original_source_triangles"):TEXT("original_points_as_vertex_cells")));
    J->SetStringField(TEXT("scalar_basis"),TEXT("Original source components; coordinate selection does not rotate scalar values."));
    J->SetBoolField(TEXT("display_reconstruction_applied"),false);
    TArray<TSharedPtr<FJsonValue>> Operations;Operations.Add(MakeShared<FJsonValueString>(TEXT("Read exact original rows; no interpolation, extrusion, clipping or resampling.")));
    if(R.Coordinates==EStudioExportCoordinates::Scene)Operations.Add(MakeShared<FJsonValueString>(TEXT("Map source XYZ to scene XZY and add the recorded display offset, in meters.")));
    J->SetArrayField(TEXT("operations"),Operations);
    TArray<TSharedPtr<FJsonValue>> Scalars;
    for(const auto& Id:R.Scalars)
    {
        const auto S=*R.Field->Scalar(Id);auto A=MakeShared<FJsonObject>();A->SetStringField(TEXT("id"),S.Id);A->SetStringField(R.Format==EStudioFieldExportFormat::CSV?TEXT("csv_column"):TEXT("vtk_array"),S.Id);
        A->SetStringField(TEXT("label"),S.Label);A->SetStringField(TEXT("unit"),S.Unit);A->SetStringField(TEXT("origin"),S.Origin);
        A->SetStringField(TEXT("expression"),R.Field->ScalarExpression(Id));Scalars.Add(MakeShared<FJsonValueObject>(A));
    }
    J->SetArrayField(TEXT("scalars"),Scalars);FString Text;if(R.Format==EStudioFieldExportFormat::CSV)FJsonSerializer::Serialize(J,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    else FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&Text));return Text;
}
