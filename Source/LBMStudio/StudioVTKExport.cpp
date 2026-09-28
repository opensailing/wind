#include "StudioVTKExport.h"
#include "StudioModel.h"

namespace
{
FString VTKAttribute(FString S)
{
    S.ReplaceInline(TEXT("&"),TEXT("&amp;"));S.ReplaceInline(TEXT("<"),TEXT("&lt;"));S.ReplaceInline(TEXT(">"),TEXT("&gt;"));
    S.ReplaceInline(TEXT("\""),TEXT("&quot;"));S.ReplaceInline(TEXT("'"),TEXT("&apos;"));return S;
}
class FVTKStream
{
public:
    FVTKStream(FArchive& In,FStudioFieldExportResult& Out,FStudioLoadCancellation Cancel,
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
    FArchive& Archive;FStudioFieldExportResult& Result;FStudioLoadCancellation Cancellation;
    TFunction<void(int64,int64)> Progress;int64 Done=0,Total=0;FString Buffer;
};


}

FStudioFieldExportResult StudioVTKExport::Write(const FStudioFieldExportRequest& Request,FArchive& Archive,
    const FStudioLoadCancellation& Cancellation,TFunction<void(int64,int64)> Progress)
{
    auto R=Request;R.Format=EStudioFieldExportFormat::VTK;
    FStudioFieldExportResult Out;
    if(!StudioFieldExport::Validate(R,Archive,Cancellation,Out))return Out;
    const auto Identity=R.Field->Identity();
    TSet<FString> Names;for(const auto& Id:R.Scalars)Names.Add(Id);
    FString PointIdName=TEXT("LBMStudioOriginalPointId");while(Names.Contains(PointIdName))PointIdName=TEXT("_")+PointIdName;
    const int32 Cells=Out.Triangles?Out.Triangles:Out.Points;
    const int64 Total=int64(Out.Points)*(R.Scalars.Num()+2)+2LL*Cells;
    FVTKStream W(Archive,Out,Cancellation,MoveTemp(Progress),Total);
    if(!W.Append(TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<VTKFile type=\"PolyData\" version=\"0.1\" byte_order=\"LittleEndian\">\n<PolyData>\n<FieldData>\n")))return Out;
    const auto Metadata=StudioFieldExport::Metadata(R,*Identity,Out.Triangles,PointIdName);const FTCHARToUTF8 UTF8(*Metadata);
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
        const auto Field=StudioFieldExport::LoadScalar(R,Id,Cancellation,Out);
        if(!Field||!W.Check())return Out;
        if(!W.Array(Id,TEXT("Float64")))return Out;
        for(int32 I=0;I<Out.Points;++I)
        {
            if(Field!=R.Field)
            {
                if(!StudioFieldExport::SamePoint(*R.Field,*Field,I))
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
