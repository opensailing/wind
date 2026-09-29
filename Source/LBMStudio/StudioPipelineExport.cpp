#include "StudioPipelineExport.h"
#include "StudioSavedFieldView.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace StudioPipelineExportPrivate
{
FString XML(FString S)
{S.ReplaceInline(TEXT("&"),TEXT("&amp;"));S.ReplaceInline(TEXT("<"),TEXT("&lt;"));S.ReplaceInline(TEXT(">"),TEXT("&gt;"));S.ReplaceInline(TEXT("\""),TEXT("&quot;"));S.ReplaceInline(TEXT("'"),TEXT("&apos;"));return S;}
FString CSV(FString S){S.ReplaceInline(TEXT("\""),TEXT("\"\""));return TEXT("\"")+S+TEXT("\"");}
FString Status(EStudioProbeSampleStatus S)
{
    switch(S)
    {
    case EStudioProbeSampleStatus::Value:return TEXT("value");
    case EStudioProbeSampleStatus::OutsideCoverage:return TEXT("outside_coverage");
    case EStudioProbeSampleStatus::OffPlane:return TEXT("off_source_plane");
    case EStudioProbeSampleStatus::MissingPoint:return TEXT("point_unavailable");
    case EStudioProbeSampleStatus::FieldUnavailable:return TEXT("field_unavailable");
    case EStudioProbeSampleStatus::NoInterpolation:return TEXT("no_interpolation");
    }return {};
}
FVector Position(const FVector& Scene,const FStudioPipelineExportRequest& R)
{if(R.Coordinates==EStudioExportCoordinates::Scene)return Scene;const auto P=Scene-R.Evaluation.Prepared.Recipe.Source.Identity.SourceOffset;return FVector(P.X,P.Z,P.Y);}
bool VertexPosition(const FStudioPipelineVertex& V,const FStudioPipelineExportRequest& R,FVector& Out)
{
    // A display translation need not be exactly reversible in floating point.
    // Preserve original coordinates directly whenever the vertex has a source row.
    if(R.Coordinates==EStudioExportCoordinates::Source&&V.OriginalRow!=INDEX_NONE)
    {int64 Id;return R.Evaluation.Prepared.Field->OriginalPoint(V.OriginalRow,Id,Out);}
    Out=Position(V.PositionMeters,R);return true;
}
FString Kind(EStudioPipelineOutputKind K)
{
    switch(K){case EStudioPipelineOutputKind::OriginalPoints:return TEXT("original_points");case EStudioPipelineOutputKind::Surface:return TEXT("surface");
    case EStudioPipelineOutputKind::ContourLines:return TEXT("contour_lines");case EStudioPipelineOutputKind::ContourSurface:return TEXT("contour_surface");
    case EStudioPipelineOutputKind::ProbeTable:return TEXT("probe_table");}return {};
}
class FStream
{
public:
    FStream(FArchive& In,FStudioFieldExportResult& Result,FStudioLoadCancellation Cancel,TFunction<void(int64,int64)> Callback)
        :Archive(In),Out(Result),Cancellation(MoveTemp(Cancel)),Progress(MoveTemp(Callback)){}
    bool Check()
    {
        if(Cancellation&&Cancellation->load()){Out.bCancelled=true;Out.Error=TEXT("Pipeline export cancelled.");}
        else if(Archive.IsError())Out.Error=TEXT("Could not write the staged pipeline output. Check free space and destination access.");
        return Out.Error.IsEmpty();
    }
    bool Fail(const TCHAR* Why){Out.Error=Why;return false;}
    bool Append(const FString& Text)
    {if(!Check())return false;Buffer+=Text;return Buffer.Len()<32768||Flush();}
    bool Flush()
    {
        if(!Check())return false;if(Buffer.IsEmpty())return true;const FTCHARToUTF8 Bytes(*Buffer);
        if(Out.Bytes+Bytes.Length()>StudioPipelineExport::MaximumBytes)return Fail(TEXT("Pipeline export exceeds 512 MiB. Reduce the evaluated output."));
        Archive.Serialize(const_cast<char*>(Bytes.Get()),Bytes.Length());Out.Bytes+=Bytes.Length();Buffer.Empty(32768);return Check();
    }
    bool LongText(const FString& Text)
    {
        if(!Flush())return false;const FTCHARToUTF8 Bytes(*Text);
        for(int32 First=0;First<Bytes.Length();First+=32768)
        {
            if(!Check())return false;const int32 Count=FMath::Min(32768,Bytes.Length()-First);
            if(Out.Bytes+Count>StudioPipelineExport::MaximumBytes)return Fail(TEXT("Pipeline export exceeds 512 MiB. Reduce the evaluated output."));
            Archive.Serialize(const_cast<char*>(Bytes.Get()+First),Count);Out.Bytes+=Count;
        }return Check();
    }
    bool Array(const FString& Name,const TCHAR* Type,int32 Components=1,int64 Tuples=INDEX_NONE)
    {return Append(FString::Printf(TEXT("<DataArray type=\"%s\" Name=\"%s\" NumberOfComponents=\"%d\"%s format=\"ascii\">\n"),Type,*XML(Name),Components,
        Tuples==INDEX_NONE?TEXT(""):*FString::Printf(TEXT(" NumberOfTuples=\"%lld\""),Tuples)));}
    bool EndArray(){return Append(TEXT("\n</DataArray>\n"));}
    void Advance(){++Done;if(Progress&&((Done&255)==0||Done==Total))Progress(Done,Total);}
    int64 Total=0;
private:
    FArchive& Archive;FStudioFieldExportResult& Out;FStudioLoadCancellation Cancellation;TFunction<void(int64,int64)> Progress;
    FString Buffer;int64 Done=0;
};
bool Validate(const FStudioPipelineExportRequest& R,FArchive& A,FStream& W,FStudioFieldExportResult& Result)
{
    const auto& E=R.Evaluation;const auto& P=E.Prepared;
    if(!W.Check())return false;
    if(!A.IsSaving()||!E.Matches(P.ProjectId,P.Revision,P.Recipe)||!P.Field->MatchesRecipe(P.Recipe)||
        !StudioPipelines::IsValid(P.Recipe,Result.Error)||
        (R.Coordinates!=EStudioExportCoordinates::Source&&R.Coordinates!=EStudioExportCoordinates::Scene)||
        (R.Format!=EStudioFieldExportFormat::VTK&&R.Format!=EStudioFieldExportFormat::CSV))
        return W.Fail(TEXT("A complete evaluated pipeline and writable staging file are required."));
    const auto& O=*E.Output;const auto& Scalar=P.Field->SelectedScalar();
    if(Kind(O.Kind).IsEmpty()||O.Method.IsEmpty()||O.Method.Len()>65536||Scalar.Id.IsEmpty()||
        O.Vertices.Num()>StudioPipelineEvaluation::MaxOutputVertices||O.Triangles.Num()>StudioPipelineEvaluation::MaxOutputTriangles||
        O.Lines.Num()>StudioPipelineEvaluation::MaxOutputLines||O.AllocatedBytes()>StudioPipelineEvaluation::MaxOutputBytes)
        return W.Fail(TEXT("The evaluated pipeline output is invalid or exceeds its geometry budget."));
    const bool Probe=O.Kind==EStudioPipelineOutputKind::ProbeTable,Points=O.Kind==EStudioPipelineOutputKind::OriginalPoints,Lines=O.Kind==EStudioPipelineOutputKind::ContourLines;
    if(Probe!=(O.Probe.IsSet())||(Probe&&(!O.Vertices.IsEmpty()||!O.Triangles.IsEmpty()||!O.Lines.IsEmpty()))||
        (Points&&(!O.Triangles.IsEmpty()||!O.Lines.IsEmpty()))||(Lines&&!O.Triangles.IsEmpty())||(!Lines&&!O.Lines.IsEmpty()))
        return W.Fail(TEXT("Pipeline topology does not match its evaluated output kind."));
    if(Probe&&R.Format==EStudioFieldExportFormat::VTK)return W.Fail(TEXT("Choose CSV for a probe table to retain every requested position and missing-value status."));
    TOptional<FVector2D> Range;
    auto AddValue=[&](double Value){if(!Range)Range=FVector2D(Value,Value);else{Range->X=FMath::Min(Range->X,Value);Range->Y=FMath::Max(Range->Y,Value);}};
    for(int32 N=0;N<O.Vertices.Num();++N)
    {
        if((N&255)==0&&!W.Check())return false;const auto& V=O.Vertices[N];
        if(V.PositionMeters.ContainsNaN()||Position(V.PositionMeters,R).ContainsNaN()||!FMath::IsFinite(V.Scalar)||V.OriginalRow<INDEX_NONE)
            return W.Fail(TEXT("An evaluated position, value or original-row reference is invalid."));
        if(V.OriginalRow!=INDEX_NONE)
        {
            int64 Id;FVector Original;double Value;
            if(!P.Field->OriginalPoint(V.OriginalRow,Id,Original)||Id!=V.OriginalPointId||
                FVector(Original.X,Original.Z,Original.Y)+P.Recipe.Source.Identity.SourceOffset!=V.PositionMeters||
                !P.Field->OriginalScalar(V.OriginalRow,Scalar.Id,Value)||Value!=V.Scalar)
                return W.Fail(TEXT("An original vertex differs from its pinned source row or scalar."));
        }
        else if(Points||!O.bDerivedGeometry)return W.Fail(TEXT("A derived vertex must not be labelled as original geometry."));
        AddValue(V.Scalar);
    }
    for(int32 N=0;N<O.Triangles.Num();++N)
    {
        if((N&255)==0&&!W.Check())return false;const auto T=O.Triangles[N];
        if(T.GetMin()<0||T.GetMax()>=O.Vertices.Num()||T.X==T.Y||T.X==T.Z||T.Y==T.Z)return W.Fail(TEXT("Evaluated triangle connectivity is invalid."));
    }
    for(int32 N=0;N<O.Lines.Num();++N)
    {
        if((N&255)==0&&!W.Check())return false;const auto L=O.Lines[N];
        if(L.X<0||L.Y<0||L.X>=O.Vertices.Num()||L.Y>=O.Vertices.Num()||L.X==L.Y)return W.Fail(TEXT("Evaluated line connectivity is invalid."));
    }
    if(Probe)
    {
        const auto& Table=*O.Probe;
        if(Table.Status!=EStudioProbeStatus::Ready||!Table.Identity||!StudioSavedFieldViews::SameIdentity(*Table.Identity,P.Recipe.Source.Identity)||
            Table.ProjectId!=P.ProjectId||Table.Field!=Scalar.Id||Table.Unit!=Scalar.Unit||Table.Origin!=Scalar.Origin||Table.Samples.IsEmpty()||Table.Samples.Num()>1024)
            return W.Fail(TEXT("The probe table differs from the evaluated pipeline identity."));
        for(const auto& S:Table.Samples)
        {
            if(!W.Check())return false;
            if(Status(S.Status).IsEmpty()||(S.Value.IsSet()!=(S.Status==EStudioProbeSampleStatus::Value))||
                (S.Value&&!FMath::IsFinite(*S.Value))||!FMath::IsFinite(S.DistanceAlongLineMeters)||S.DistanceAlongLineMeters<0||
                (S.ScenePosition&&(S.ScenePosition->ContainsNaN()||Position(*S.ScenePosition,R).ContainsNaN()))||
                (S.SourcePosition&&S.SourcePosition->ContainsNaN())||(S.Value&&!S.ScenePosition))
                return W.Fail(TEXT("A probe row has inconsistent coordinates, scalar availability or status."));
            if(S.Value)AddValue(*S.Value);
        }
    }
    if(Range.IsSet()!=O.Range.IsSet()||(Range&&*Range!=*O.Range))return W.Fail(TEXT("The evaluated scalar range differs from its retained values."));
    Result.Identity=P.Recipe.Source.Identity;Result.PipelineId=P.Recipe.Id;Result.PipelineName=P.Recipe.Name;
    Result.Points=O.Vertices.Num();Result.Triangles=O.Triangles.Num();Result.Lines=O.Lines.Num();Result.Rows=Probe?O.Probe->Samples.Num():O.Vertices.Num();return W.Check();
}
struct FColumns
{
    FString Row,OriginalRow,PointId,OriginalValid,X,Y,Z,Distance,Status;
    explicit FColumns(const FString& Scalar)
    {
        TSet<FString> Names{Scalar};auto Unique=[&](FString Name){while(Names.Contains(Name))Name=TEXT("_")+Name;Names.Add(Name);return Name;};
        Row=Unique(TEXT("row"));OriginalRow=Unique(TEXT("original_row"));PointId=Unique(TEXT("original_point_id"));OriginalValid=Unique(TEXT("has_original_point"));
        X=Unique(TEXT("x_m"));Y=Unique(TEXT("y_m"));Z=Unique(TEXT("z_m"));Distance=Unique(TEXT("distance_m"));Status=Unique(TEXT("status"));
    }
};
FString Metadata(const FStudioPipelineExportRequest& R,const FColumns& C)
{
    const auto& P=R.Evaluation.Prepared;const auto& O=*R.Evaluation.Output;const auto& I=P.Recipe.Source.Identity;const auto& S=P.Field->SelectedScalar();
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("format"),R.Format==EStudioFieldExportFormat::VTK?TEXT("LBMStudio.PipelineVTK"):TEXT("LBMStudio.PipelineCSV"));
    J->SetNumberField(TEXT("version"),1);J->SetStringField(TEXT("project_id"),P.ProjectId.ToString());J->SetStringField(TEXT("evaluation_revision"),LexToString(P.Revision));
    J->SetArrayField(TEXT("recipes"),StudioPipelines::ToJSON({P.Recipe}));J->SetStringField(TEXT("output_kind"),Kind(O.Kind));
    J->SetStringField(TEXT("method"),O.Method);J->SetBoolField(TEXT("derived_geometry"),O.bDerivedGeometry);
    J->SetStringField(TEXT("dataset"),I.Dataset);J->SetStringField(TEXT("metadata_sha256"),I.MetadataSHA256);J->SetStringField(TEXT("payload_sha256"),I.PayloadSHA256);
    J->SetStringField(TEXT("reconstruction_sha256"),I.ReconstructionSHA256);J->SetNumberField(TEXT("frame_ordinal"),I.Ordinal);
    J->SetNumberField(TEXT("source_step"),I.Frame.Index);J->SetNumberField(TEXT("source_time_seconds"),I.Frame.Time);J->SetNumberField(TEXT("spatial_dimensions"),I.SpatialDimensions);
    J->SetStringField(TEXT("coordinate_unit"),TEXT("m"));J->SetStringField(TEXT("coordinate_system"),R.Coordinates==EStudioExportCoordinates::Source?TEXT("source_xyz"):TEXT("scene_xzy_plus_offset"));
    TArray<TSharedPtr<FJsonValue>> Offset;for(int32 Axis=0;Axis<3;++Axis)Offset.Add(MakeShared<FJsonValueNumber>(I.SourceOffset[Axis]));J->SetArrayField(TEXT("source_to_scene_offset_meters"),Offset);
    J->SetStringField(TEXT("scalar_id"),S.Id);J->SetStringField(TEXT("scalar_label"),S.Label);J->SetStringField(TEXT("scalar_unit"),S.Unit);
    J->SetStringField(TEXT("scalar_origin"),S.Origin);J->SetStringField(TEXT("scalar_expression"),P.Field->ScalarExpression(S.Id));
    J->SetStringField(TEXT("scalar_basis"),TEXT("Source-component values and declared derived operations; coordinate selection does not rotate scalar components."));
    J->SetNumberField(TEXT("vertices"),O.Vertices.Num());J->SetNumberField(TEXT("triangles"),O.Triangles.Num());J->SetNumberField(TEXT("segments"),O.Lines.Num());
    J->SetNumberField(TEXT("probe_rows"),O.Probe?O.Probe->Samples.Num():0);
    J->SetStringField(TEXT("topology"),R.Format==EStudioFieldExportFormat::CSV?TEXT("tabular_rows_no_connectivity"):TEXT("evaluated_polydata_connectivity"));
    J->SetStringField(TEXT("original_id_rule"),TEXT("Only source vertices carry original row/ID. Derived vertices use row -1 and validity 0 in VTK (ID 0 is a placeholder), or blank CSV row/ID. Probe point IDs are present only for exact source-point probes."));
    auto Columns=MakeShared<FJsonObject>();Columns->SetStringField(TEXT("scalar"),S.Id);Columns->SetStringField(TEXT("point_id"),C.PointId);
    if(!O.Probe)Columns->SetStringField(TEXT("original_row"),C.OriginalRow);
    if(R.Format==EStudioFieldExportFormat::VTK)Columns->SetStringField(TEXT("original_valid"),C.OriginalValid);
    else
    {
        Columns->SetStringField(TEXT("row"),C.Row);Columns->SetStringField(TEXT("x"),C.X);Columns->SetStringField(TEXT("y"),C.Y);Columns->SetStringField(TEXT("z"),C.Z);
        if(O.Probe){Columns->SetStringField(TEXT("distance"),C.Distance);Columns->SetStringField(TEXT("status"),C.Status);}
    }
    J->SetObjectField(TEXT("columns"),Columns);
    if(O.Range){TArray<TSharedPtr<FJsonValue>> Range{MakeShared<FJsonValueNumber>(O.Range->X),MakeShared<FJsonValueNumber>(O.Range->Y)};J->SetArrayField(TEXT("evaluated_range"),Range);}
    else J->SetField(TEXT("evaluated_range"),MakeShared<FJsonValueNull>());
    FString JSON;FJsonSerializer::Serialize(J,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&JSON));return JSON;
}
bool WriteCSV(const FStudioPipelineExportRequest& R,const FColumns& C,const FString& Metadata,FStream& W)
{
    const auto& O=*R.Evaluation.Output;const auto& S=R.Evaluation.Prepared.Field->SelectedScalar();const bool Probe=O.Probe.IsSet();
    W.Total=FMath::Max<int64>(1,Probe?O.Probe->Samples.Num():O.Vertices.Num());
    if(!W.Append(TEXT("# LBMStudioMetadataUTF8 ")))return false;
    if(!W.LongText(Metadata))return false;
    const TArray<FString> Headers=Probe?TArray<FString>{C.Row,C.PointId,C.X,C.Y,C.Z,C.Distance,S.Id,C.Status}:
        TArray<FString>{C.Row,C.OriginalRow,C.PointId,C.X,C.Y,C.Z,S.Id};
    if(!W.Append(TEXT("\n")))return false;
    for(int32 N=0;N<Headers.Num();++N)if(!W.Append((N?TEXT(","):TEXT(""))+CSV(Headers[N])))return false;
    if(!W.Append(TEXT("\n")))return false;
    if(Probe)for(int32 N=0;N<O.Probe->Samples.Num();++N)
    {
        const auto& V=O.Probe->Samples[N];FString Coordinates=TEXT(",,");
        if(V.ScenePosition){const auto P=R.Coordinates==EStudioExportCoordinates::Source&&V.SourcePosition?*V.SourcePosition:Position(*V.ScenePosition,R);Coordinates=FString::Printf(TEXT("%.17g,%.17g,%.17g"),P.X,P.Y,P.Z);}
        if(!W.Append(FString::Printf(TEXT("%d,%s,%s,%.17g,%s,%s\n"),N,V.PointId?*LexToString(*V.PointId):TEXT(""),*Coordinates,V.DistanceAlongLineMeters,
            V.Value?*FString::Printf(TEXT("%.17g"),*V.Value):TEXT(""),*CSV(Status(V.Status)))))return false;
        W.Advance();
    }
    else for(int32 N=0;N<O.Vertices.Num();++N)
    {
        const auto& V=O.Vertices[N];FVector P;
        if(!VertexPosition(V,R,P))return W.Fail(TEXT("An original coordinate became unavailable."));
        if(!W.Append(FString::Printf(TEXT("%d,%s,%s,%.17g,%.17g,%.17g,%.17g\n"),N,V.OriginalRow==INDEX_NONE?TEXT(""):*LexToString(V.OriginalRow),
            V.OriginalRow==INDEX_NONE?TEXT(""):*LexToString(V.OriginalPointId),P.X,P.Y,P.Z,V.Scalar)))return false;
        W.Advance();
    }
    if(!Probe&&O.Vertices.IsEmpty())W.Advance();return W.Flush();
}
bool WriteVTK(const FStudioPipelineExportRequest& R,const FColumns& C,const FString& Metadata,FStream& W)
{
    const auto& O=*R.Evaluation.Output;const auto& S=R.Evaluation.Prepared.Field->SelectedScalar();
    const bool Points=O.Kind==EStudioPipelineOutputKind::OriginalPoints;const int32 Cells=Points?O.Vertices.Num():O.Triangles.Num()+O.Lines.Num();
    W.Total=5LL*O.Vertices.Num()+2LL*Cells+1;
    if(!W.Append(TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<VTKFile type=\"PolyData\" version=\"0.1\" byte_order=\"LittleEndian\">\n<PolyData>\n<FieldData>\n")))return false;
    const FTCHARToUTF8 UTF8(*Metadata);if(!W.Array(TEXT("LBMStudioMetadataUTF8"),TEXT("UInt8"),1,UTF8.Length()))return false;
    for(int32 N=0;N<UTF8.Length();++N)if(!W.Append(FString::Printf(TEXT("%u "),uint8(UTF8.Get()[N]))))return false;
    if(!W.EndArray()||!W.Array(TEXT("TimeValue"),TEXT("Float64"),1,1)||!W.Append(FString::Printf(TEXT("%.17g"),R.Evaluation.Prepared.Recipe.Source.Identity.Frame.Time))||!W.EndArray()||
        !W.Append(FString::Printf(TEXT("</FieldData>\n<Piece NumberOfPoints=\"%d\" NumberOfVerts=\"%d\" NumberOfLines=\"%d\" NumberOfStrips=\"0\" NumberOfPolys=\"%d\">\n<PointData Scalars=\"%s\">\n"),
            O.Vertices.Num(),Points?O.Vertices.Num():0,O.Lines.Num(),O.Triangles.Num(),*XML(S.Id))))return false;
    for(int32 K=0;K<4;++K)
    {
        const FString Names[]={C.OriginalRow,C.PointId,C.OriginalValid,S.Id};const TCHAR* Types[]={TEXT("Int64"),TEXT("Int64"),TEXT("UInt8"),TEXT("Float64")};
        if(!W.Array(Names[K],Types[K]))return false;
        for(const auto& V:O.Vertices)
        {
            const auto Value=K==0?LexToString(V.OriginalRow):K==1?LexToString(V.OriginalRow==INDEX_NONE?int64(0):V.OriginalPointId):
                K==2?FString(V.OriginalRow==INDEX_NONE?TEXT("0"):TEXT("1")):FString::Printf(TEXT("%.17g"),V.Scalar);
            if(!W.Append(Value+TEXT("\n")))return false;W.Advance();
        }if(!W.EndArray())return false;
    }
    if(!W.Append(TEXT("</PointData>\n<CellData/>\n<Points>\n"))||!W.Array(TEXT("Points"),TEXT("Float64"),3))return false;
    for(const auto& V:O.Vertices)
    {
        FVector P;if(!VertexPosition(V,R,P))return W.Fail(TEXT("An original coordinate became unavailable."));
        if(!W.Append(FString::Printf(TEXT("%.17g %.17g %.17g\n"),P.X,P.Y,P.Z)))return false;
        W.Advance();
    }
    if(!W.EndArray()||!W.Append(TEXT("</Points>\n")))return false;
    const TCHAR* Tag=Points?TEXT("Verts"):O.Kind==EStudioPipelineOutputKind::ContourLines?TEXT("Lines"):TEXT("Polys");const int32 Width=Points?1:O.Kind==EStudioPipelineOutputKind::ContourLines?2:3;
    if(!W.Append(FString::Printf(TEXT("<%s>\n"),Tag))||!W.Array(TEXT("connectivity"),TEXT("Int64")))return false;
    for(int32 N=0;N<Cells;++N)
    {
        const auto Row=Points?FString::Printf(TEXT("%d\n"),N):Width==2?FString::Printf(TEXT("%d %d\n"),O.Lines[N].X,O.Lines[N].Y):FString::Printf(TEXT("%d %d %d\n"),O.Triangles[N].X,O.Triangles[N].Y,O.Triangles[N].Z);
        if(!W.Append(Row))return false;W.Advance();
    }
    if(!W.EndArray()||!W.Array(TEXT("offsets"),TEXT("Int64")))return false;
    for(int32 N=0;N<Cells;++N){if(!W.Append(FString::Printf(TEXT("%lld\n"),int64(N+1)*Width)))return false;W.Advance();}
    if(!W.EndArray()||!W.Append(FString::Printf(TEXT("</%s>\n</Piece>\n</PolyData>\n</VTKFile>\n"),Tag))||!W.Flush())return false;W.Advance();return W.Check();
}
}
FStudioFieldExportResult StudioPipelineExport::Write(const FStudioPipelineExportRequest& R,FArchive& Archive,
    const FStudioLoadCancellation& Cancellation,TFunction<void(int64,int64)> Progress)
{
    using namespace StudioPipelineExportPrivate;FStudioFieldExportResult Out;FStream W(Archive,Out,Cancellation,MoveTemp(Progress));
    if(!Validate(R,Archive,W,Out))return Out;
    FColumns C(R.Evaluation.Prepared.Field->SelectedScalar().Id);const auto JSON=Metadata(R,C);
    if(JSON.Len()>1024*1024){W.Fail(TEXT("Pipeline metadata exceeds the 1 Mi-character export limit."));return Out;}
    Out.bSuccess=R.Format==EStudioFieldExportFormat::VTK?WriteVTK(R,C,JSON,W):WriteCSV(R,C,JSON,W);return Out;
}
