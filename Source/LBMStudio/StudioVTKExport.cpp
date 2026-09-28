#include "StudioVTKExport.h"
#include "StudioModel.h"
#include "StudioInspectionObjects.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
bool SameVTKFrame(const FStudioFieldIdentity& A,const FStudioFieldIdentity& B)
{
    return A.Dataset==B.Dataset&&A.MetadataSHA256==B.MetadataSHA256&&A.PayloadSHA256==B.PayloadSHA256&&
        A.ReconstructionSHA256==B.ReconstructionSHA256&&A.Ordinal==B.Ordinal&&A.Frame.Index==B.Frame.Index&&
        A.Frame.Time==B.Frame.Time&&A.SpatialDimensions==B.SpatialDimensions&&A.SourceOffset==B.SourceOffset&&A.Interpolation==B.Interpolation;
}
FString VTKAttribute(FString S)
{
    S.ReplaceInline(TEXT("&"),TEXT("&amp;"));S.ReplaceInline(TEXT("<"),TEXT("&lt;"));S.ReplaceInline(TEXT(">"),TEXT("&gt;"));
    S.ReplaceInline(TEXT("\""),TEXT("&quot;"));S.ReplaceInline(TEXT("'"),TEXT("&apos;"));return S;
}
bool VTKName(const FString& S)
{if(S.IsEmpty()||S.Len()>256)return false;for(const TCHAR C:S)if(C<32)return false;return true;}

class FVTKStream
{
public:
    FVTKStream(FArchive& In,FStudioVTKExportResult& Out,FStudioLoadCancellation Cancel,
        TFunction<void(int64,int64)> InProgress,int64 InTotal)
        :Archive(In),Result(Out),Cancellation(MoveTemp(Cancel)),Progress(MoveTemp(InProgress)),Total(InTotal){}
    bool Check()
    {
        if(Cancellation&&Cancellation->load())
        {Result.bCancelled=true;Result.Error=TEXT("Field export cancelled.");return false;}
        if(Archive.IsError()){Result.Error=TEXT("Could not write the staged VTK file. Check free space and destination access.");return false;}
        return Result.Error.IsEmpty();
    }
    bool Append(const FString& Text)
    {if(!Check())return false;Buffer+=Text;return Buffer.Len()<32768||Flush();}
    bool Flush()
    {
        if(!Check())return false;
        if(Buffer.IsEmpty())return true;
        const FTCHARToUTF8 UTF8(*Buffer);
        if(Result.Bytes+UTF8.Length()>StudioVTKExport::MaximumBytes)
        {Result.Error=TEXT("This frame export exceeds 512 MiB. Select fewer scalar arrays.");return false;}
        Archive.Serialize(const_cast<char*>(UTF8.Get()),UTF8.Length());Result.Bytes+=UTF8.Length();Buffer.Empty(32768);return Check();
    }
    void Advance()
    {++Done;if(Progress&&((Done&255)==0||Done==Total))Progress(Done,Total);}
    bool Array(const FString& Name,const TCHAR* Type,int32 Components=1,int64 Tuples=INDEX_NONE)
    {
        return Append(FString::Printf(TEXT("<DataArray type=\"%s\" Name=\"%s\" NumberOfComponents=\"%d\"%s format=\"ascii\">\n"),
            Type,*VTKAttribute(Name),Components,Tuples==INDEX_NONE?TEXT(""):*FString::Printf(TEXT(" NumberOfTuples=\"%lld\""),Tuples)));
    }
    bool EndArray(){return Append(TEXT("\n</DataArray>\n"));}
private:
    FArchive& Archive;FStudioVTKExportResult& Result;FStudioLoadCancellation Cancellation;
    TFunction<void(int64,int64)> Progress;int64 Done=0,Total=0;FString Buffer;
};

FString VTKMetadata(const FStudioVTKExportRequest& R,const FStudioFieldIdentity& I,int32 Triangles,const FString& PointIdName)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("format"),TEXT("LBMStudio.OriginalField"));J->SetNumberField(TEXT("version"),1);
    J->SetStringField(TEXT("dataset"),I.Dataset);J->SetStringField(TEXT("metadata_sha256"),I.MetadataSHA256);
    J->SetStringField(TEXT("payload_sha256"),I.PayloadSHA256);J->SetStringField(TEXT("view_reconstruction_sha256"),I.ReconstructionSHA256);
    J->SetNumberField(TEXT("frame_ordinal"),I.Ordinal);J->SetNumberField(TEXT("source_step"),I.Frame.Index);J->SetNumberField(TEXT("source_time_seconds"),I.Frame.Time);
    J->SetNumberField(TEXT("spatial_dimensions"),I.SpatialDimensions);J->SetStringField(TEXT("coordinate_unit"),TEXT("m"));
    J->SetStringField(TEXT("coordinate_system"),R.Coordinates==EStudioExportCoordinates::Source?TEXT("source_xyz"):TEXT("scene_xzy_plus_offset"));
    TArray<TSharedPtr<FJsonValue>> Offset;for(int32 A=0;A<3;++A)Offset.Add(MakeShared<FJsonValueNumber>(I.SourceOffset[A]));
    J->SetArrayField(TEXT("source_to_scene_offset_meters"),Offset);J->SetStringField(TEXT("point_id_array"),PointIdName);
    J->SetStringField(TEXT("topology"),Triangles?TEXT("original_source_triangles"):TEXT("original_points_as_vertex_cells"));
    J->SetStringField(TEXT("scalar_basis"),TEXT("Original source components; coordinate selection does not rotate scalar values."));
    J->SetBoolField(TEXT("display_reconstruction_applied"),false);
    TArray<TSharedPtr<FJsonValue>> Operations;Operations.Add(MakeShared<FJsonValueString>(TEXT("Read exact original rows; no interpolation, extrusion, clipping or resampling.")));
    if(R.Coordinates==EStudioExportCoordinates::Scene)Operations.Add(MakeShared<FJsonValueString>(TEXT("Map source XYZ to scene XZY and add the recorded display offset, in meters.")));
    J->SetArrayField(TEXT("operations"),Operations);
    TArray<TSharedPtr<FJsonValue>> Scalars;
    for(const auto& Id:R.Scalars)
    {
        const auto S=*R.Field->Scalar(Id);auto A=MakeShared<FJsonObject>();A->SetStringField(TEXT("id"),S.Id);A->SetStringField(TEXT("vtk_array"),S.Id);
        A->SetStringField(TEXT("label"),S.Label);A->SetStringField(TEXT("unit"),S.Unit);A->SetStringField(TEXT("origin"),S.Origin);
        A->SetStringField(TEXT("expression"),R.Field->ScalarExpression(Id));Scalars.Add(MakeShared<FJsonValueObject>(A));
    }
    J->SetArrayField(TEXT("scalars"),Scalars);FString Text;FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&Text));return Text;
}
}

FStudioVTKExportResult StudioVTKExport::Write(const FStudioVTKExportRequest& R,FArchive& Archive,
    const FStudioLoadCancellation& Cancellation,TFunction<void(int64,int64)> Progress)
{
    FStudioVTKExportResult Out;
    if(Cancellation&&Cancellation->load()){Out.bCancelled=true;Out.Error=TEXT("Field export cancelled.");return Out;}
    const auto Identity=R.Field?R.Field->Identity():TOptional<FStudioFieldIdentity>();
    if(!R.Field||!R.Field->IsValid()||!Identity||!Archive.IsSaving()||
        !StudioInspectionObjects::IsValid(FStudioInspectionSource{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256})||
        Identity->Ordinal<0||Identity->Frame.Index<0||!FMath::IsFinite(Identity->Frame.Time)||Identity->SourceOffset.ContainsNaN()||
        (Identity->SpatialDimensions!=2&&Identity->SpatialDimensions!=3)||
        (R.Coordinates!=EStudioExportCoordinates::Source&&R.Coordinates!=EStudioExportCoordinates::Scene))
    {Out.Error=TEXT("A verified original frame and writable staging file are required.");return Out;}
    Out.Identity=*Identity;Out.Points=R.Field->OriginalPointCount();Out.Triangles=R.Field->OriginalTriangleCount();
    if(Out.Points<=0||Out.Points>4000000||Out.Triangles<0||Out.Triangles>8000000||R.Scalars.IsEmpty()||R.Scalars.Num()>64)
    {Out.Error=TEXT("Original point/connectivity access and 1–64 selected scalar arrays are required.");return Out;}
    TSet<FString> Names;
    for(const auto& Id:R.Scalars)
    {
        const auto S=R.Field->Scalar(Id);
        if(!VTKName(Id)||Names.Contains(Id)||!S||S->Id!=Id)
        {Out.Error=TEXT("Select distinct scalar arrays supplied by this original recording.");return Out;}
        Names.Add(Id);
    }
    FString PointIdName=TEXT("LBMStudioOriginalPointId");while(Names.Contains(PointIdName))PointIdName=TEXT("_")+PointIdName;
    const int32 Cells=Out.Triangles?Out.Triangles:Out.Points;
    const int64 Total=int64(Out.Points)*(R.Scalars.Num()+2)+2LL*Cells;
    FVTKStream W(Archive,Out,Cancellation,MoveTemp(Progress),Total);
    if(!W.Append(TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<VTKFile type=\"PolyData\" version=\"0.1\" byte_order=\"LittleEndian\">\n<PolyData>\n<FieldData>\n")))return Out;
    const auto Metadata=VTKMetadata(R,*Identity,Out.Triangles,PointIdName);const FTCHARToUTF8 UTF8(*Metadata);
    if(!W.Array(TEXT("LBMStudioMetadataUTF8"),TEXT("UInt8"),1,UTF8.Length()))return Out;
    for(int32 I=0;I<UTF8.Length();++I)if(!W.Append(FString::Printf(TEXT("%u "),uint8(UTF8.Get()[I]))))return Out;
    if(!W.EndArray()||!W.Array(TEXT("TimeValue"),TEXT("Float64"),1,1)||!W.Append(FString::Printf(TEXT("%.17g"),Identity->Frame.Time))||!W.EndArray()||
        !W.Append(FString::Printf(TEXT("</FieldData>\n<Piece NumberOfPoints=\"%d\" NumberOfVerts=\"%d\" NumberOfLines=\"0\" NumberOfStrips=\"0\" NumberOfPolys=\"%d\">\n<PointData Scalars=\"%s\">\n"),
            Out.Points,Out.Triangles?0:Out.Points,Out.Triangles,*VTKAttribute(R.Scalars[0]))))return Out;
    if(!W.Array(PointIdName,TEXT("Int64")))return Out;
    for(int32 I=0;I<Out.Points;++I)
    {
        int64 Id;FVector P;if(!R.Field->OriginalPoint(I,Id,P)||P.ContainsNaN())
        {Out.Error=TEXT("An original point is unavailable or invalid.");return Out;}
        if(!W.Append(FString::Printf(TEXT("%lld\n"),Id)))return Out;W.Advance();
    }
    if(!W.EndArray())return Out;
    for(const auto& Id:R.Scalars)
    {
        auto Field=R.Field;double First;
        if(!Field->OriginalScalar(0,Id,First))Field=R.Field->LoadScalarSnapshot(Id,Cancellation,Out.Error);
        if(!W.Check())return Out;
        const auto Actual=Field?Field->Identity():TOptional<FStudioFieldIdentity>();
        const auto S=Field?Field->Scalar(Id):TOptional<FStudioScalarDescriptor>();const auto Expected=*R.Field->Scalar(Id);
        if(!Field||!Field->IsValid()||!Actual||!SameVTKFrame(*Identity,*Actual)||Field->OriginalPointCount()!=Out.Points||
            !S||S->Id!=Expected.Id||S->Unit!=Expected.Unit||S->Origin!=Expected.Origin||S->Label!=Expected.Label||
            S->Minimum!=Expected.Minimum||S->Maximum!=Expected.Maximum||
            Field->ScalarExpression(Id)!=R.Field->ScalarExpression(Id))
        {Out.Error=TEXT("A scalar read differs from the pinned original frame or its scientific meaning.");return Out;}
        if(!W.Array(Id,TEXT("Float64")))return Out;
        for(int32 I=0;I<Out.Points;++I)
        {
            if(Field!=R.Field)
            {
                int64 OriginalId,LoadedId;FVector OriginalPosition,LoadedPosition;
                if(!R.Field->OriginalPoint(I,OriginalId,OriginalPosition)||!Field->OriginalPoint(I,LoadedId,LoadedPosition)||
                    OriginalId!=LoadedId||OriginalPosition!=LoadedPosition)
                {Out.Error=TEXT("A scalar read changed the original point order or coordinates.");return Out;}
            }
            double Value;if(!Field->OriginalScalar(I,Id,Value)||!FMath::IsFinite(Value))
            {Out.Error=TEXT("A selected original scalar value is unavailable or non-finite.");return Out;}
            if(!W.Append(FString::Printf(TEXT("%.17g\n"),Value)))return Out;W.Advance();
        }
        if(!W.EndArray())return Out;
    }
    if(!W.Append(TEXT("</PointData>\n<CellData/>\n<Points>\n"))||!W.Array(TEXT("Points"),TEXT("Float64"),3))return Out;
    for(int32 I=0;I<Out.Points;++I)
    {
        int64 Id;FVector P;if(!R.Field->OriginalPoint(I,Id,P)||P.ContainsNaN()){Out.Error=TEXT("An original coordinate is unavailable or invalid.");return Out;}
        if(R.Coordinates==EStudioExportCoordinates::Scene)P=FVector(P.X,P.Z,P.Y)+Identity->SourceOffset;
        if(P.ContainsNaN()){Out.Error=TEXT("The coordinate transform is non-finite.");return Out;}
        if(!W.Append(FString::Printf(TEXT("%.17g %.17g %.17g\n"),P.X,P.Y,P.Z)))return Out;W.Advance();
    }
    const TCHAR* CellTag=Out.Triangles?TEXT("Polys"):TEXT("Verts");
    if(!W.EndArray()||!W.Append(FString::Printf(TEXT("</Points>\n<%s>\n"),CellTag))||!W.Array(TEXT("connectivity"),TEXT("Int64")))return Out;
    for(int32 I=0;I<Cells;++I)
    {
        if(Out.Triangles)
        {
            FIntVector T;if(!R.Field->OriginalTriangle(I,T)||T.GetMin()<0||T.GetMax()>=Out.Points||T.X==T.Y||T.X==T.Z||T.Y==T.Z)
            {Out.Error=TEXT("Original triangle connectivity is invalid.");return Out;}
            if(!W.Append(FString::Printf(TEXT("%d %d %d\n"),T.X,T.Y,T.Z)))return Out;
        }
        else if(!W.Append(FString::Printf(TEXT("%d\n"),I)))return Out;
        W.Advance();
    }
    if(!W.EndArray()||!W.Array(TEXT("offsets"),TEXT("Int64")))return Out;
    for(int32 I=0;I<Cells;++I)
    {if(!W.Append(FString::Printf(TEXT("%lld\n"),int64(I+1)*(Out.Triangles?3:1))))return Out;W.Advance();}
    if(!W.EndArray()||!W.Append(FString::Printf(TEXT("</%s>\n</Piece>\n</PolyData>\n</VTKFile>\n"),CellTag))||!W.Flush())return Out;
    Out.bSuccess=true;return Out;
}
