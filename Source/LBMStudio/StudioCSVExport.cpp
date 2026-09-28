#include "StudioCSVExport.h"
#include "StudioModel.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace
{
FString CSVFieldCell(FString Text)
{Text.ReplaceInline(TEXT("\""),TEXT("\"\""));return TEXT("\"")+Text+TEXT("\"");}
}
FStudioFieldExportResult StudioCSVExport::Write(const FStudioFieldExportRequest& Request,FArchive& Archive,
    const FStudioLoadCancellation& Cancellation,TFunction<void(int64,int64)> Progress)
{
    auto R=Request;R.Format=EStudioFieldExportFormat::CSV;
    FStudioFieldExportResult Out;
    if(!StudioFieldExport::Validate(R,Archive,Cancellation,Out))return Out;
    auto Check=[&]()
    {
        if(Cancellation&&Cancellation->load()){Out.bCancelled=true;Out.Error=TEXT("CSV field export cancelled.");}
        else if(Archive.IsError())Out.Error=TEXT("Could not write the CSV export. Check free space and destination access.");
        return Out.Error.IsEmpty();
    };
    // Progress covers scalar staging and final rows, never an inferred time range.
    const int64 Total=int64(Out.Points)*(R.Scalars.Num()+1);int64 Done=0;
    auto Advance=[&](int64 Count){Done+=Count;if(Progress)Progress(Done,Total);};
    const FString Parent=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("ExportStaging"));FString Stage;
    if(!IFileManager::Get().MakeDirectory(*Parent,true)||!StudioFileDialog::CreateExportStage(Parent,Stage,Out.Error))
    {if(Out.Error.IsEmpty())Out.Error=TEXT("Could not create CSV column staging. Check application storage access.");return Out;}
    ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Stage,false,true);};
    TArray<FString> Paths;Paths.Reserve(R.Scalars.Num());
    TArray<double> Block;Block.SetNumUninitialized(BlockRows);
    for(int32 K=0;K<R.Scalars.Num();++K)
    {
        if(!Check())return Out;
        const auto Field=StudioFieldExport::LoadScalar(R,R.Scalars[K],Cancellation,Out);
        if(!Field||!Check())return Out;
        Paths.Add(Stage/FString::Printf(TEXT("column_%02d.bin"),K));
        TUniquePtr<FArchive> Column(IFileManager::Get().CreateFileWriter(*Paths.Last(),FILEWRITE_NoReplaceExisting));
        if(!Column){Out.Error=TEXT("Could not create a CSV column. Check free space.");return Out;}
        for(int32 First=0;First<Out.Points;First+=BlockRows)
        {
            if(!Check())return Out;const int32 Count=FMath::Min(BlockRows,Out.Points-First);
            for(int32 N=0;N<Count;++N)
            {
                if(Field!=R.Field&&!StudioFieldExport::SamePoint(*R.Field,*Field,First+N))
                {Out.Error=TEXT("A scalar read changed the original point order or coordinates.");return Out;}
                if(!Field->OriginalScalar(First+N,R.Scalars[K],Block[N])||!FMath::IsFinite(Block[N]))
                {Out.Error=TEXT("A selected original scalar value is unavailable or non-finite.");return Out;}
            }
            Column->Serialize(Block.GetData(),int64(Count)*sizeof(double));
            if(Column->IsError()){Out.Error=TEXT("Could not stage a CSV column. Check free space.");return Out;}
            Advance(Count);
        }
        const bool Closed=Column->Close()&&!Column->IsError();Column.Reset();
        if(!Closed){Out.Error=TEXT("Could not close a CSV column. Check free space.");return Out;}
    }
    if(!Check())return Out;
    TSet<FString> Names;for(const auto& S:R.Scalars)Names.Add(S);
    auto Unique=[&](FString Name){while(Names.Contains(Name))Name=TEXT("_")+Name;Names.Add(Name);return Name;};
    const FString PointId=Unique(TEXT("point_id"));TArray<FString> Coordinates;
    for(const auto* Name:{TEXT("x_m"),TEXT("y_m"),TEXT("z_m")})Coordinates.Add(Unique(Name));
    TSharedPtr<FJsonObject> Metadata;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioFieldExport::Metadata(R,Out.Identity,0,PointId)),Metadata);
    if(!Metadata){Out.Error=TEXT("Could not encode original-field metadata.");return Out;}
    TArray<TSharedPtr<FJsonValue>> Columns;for(const auto& Name:Coordinates)Columns.Add(MakeShared<FJsonValueString>(Name));
    Metadata->SetArrayField(TEXT("coordinate_columns"),Columns);
    FString JSON;FJsonSerializer::Serialize(Metadata.ToSharedRef(),TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&JSON));
    if(JSON.Len()>1024*1024){Out.Error=TEXT("CSV source metadata exceeds the 1 Mi-character limit.");return Out;}
    FString Buffer;
    auto Flush=[&]()
    {
        if(!Check())return false;const FTCHARToUTF8 Bytes(*Buffer);
        if(Out.Bytes+Bytes.Length()>StudioFieldExport::MaximumBytes)
        {Out.Error=TEXT("This CSV frame exceeds 512 MiB. Select fewer scalar arrays.");return false;}
        Archive.Serialize(const_cast<char*>(Bytes.Get()),Bytes.Length());Out.Bytes+=Bytes.Length();Buffer.Empty(32768);return Check();
    };
    auto Append=[&](const FString& Text){if(!Check())return false;Buffer+=Text;return Buffer.Len()<32768||Flush();};
    // Metadata itself is bounded and streamed through the same small chunks.
    if(!Append(TEXT("# LBMStudioMetadataUTF8 ")))return Out;
    for(int32 N=0;N<JSON.Len();N+=8192)if(!Append(JSON.Mid(N,8192)))return Out;
    if(!Append(TEXT("\n")+CSVFieldCell(PointId)))return Out;
    for(const auto& Name:Coordinates)if(!Append(TEXT(",")+CSVFieldCell(Name)))return Out;
    for(const auto& Id:R.Scalars)if(!Append(TEXT(",")+CSVFieldCell(Id)))return Out;
    if(!Append(TEXT("\n")))return Out;
    TArray<TUniquePtr<FArchive>> Readers;
    for(const auto& Path:Paths)
    {
        auto Reader=TUniquePtr<FArchive>(IFileManager::Get().CreateFileReader(*Path));
        if(!Reader||Reader->TotalSize()!=int64(Out.Points)*sizeof(double))
        {Out.Error=TEXT("A staged CSV column is incomplete or unreadable.");return Out;}
        Readers.Add(MoveTemp(Reader));
    }
    TArray<double> Values;Values.SetNumUninitialized(BlockRows*R.Scalars.Num());
    for(int32 First=0;First<Out.Points;First+=BlockRows)
    {
        if(!Check())return Out;const int32 Count=FMath::Min(BlockRows,Out.Points-First);
        for(int32 K=0;K<Readers.Num();++K)
        {
            Readers[K]->Serialize(Values.GetData()+K*BlockRows,int64(Count)*sizeof(double));
            if(Readers[K]->IsError()){Out.Error=TEXT("Could not read a staged CSV column.");return Out;}
        }
        for(int32 N=0;N<Count;++N)
        {
            int64 Id;FVector P;
            if(!R.Field->OriginalPoint(First+N,Id,P)||P.ContainsNaN())
            {Out.Error=TEXT("An original coordinate is unavailable or invalid.");return Out;}
            if(R.Coordinates==EStudioExportCoordinates::Scene)P=FVector(P.X,P.Z,P.Y)+Out.Identity.SourceOffset;
            if(P.ContainsNaN()){Out.Error=TEXT("The coordinate transform is non-finite.");return Out;}
            if(!Append(FString::Printf(TEXT("%lld,%.17g,%.17g,%.17g"),Id,P.X,P.Y,P.Z)))return Out;
            for(int32 K=0;K<Readers.Num();++K)if(!Append(FString::Printf(TEXT(",%.17g"),Values[K*BlockRows+N])))return Out;
            if(!Append(TEXT("\n")))return Out;
        }
        Advance(Count);
    }
    for(auto& Reader:Readers)if(!Reader->Close()||Reader->IsError())
    {Out.Error=TEXT("Could not finish reading a staged CSV column.");return Out;}
    Readers.Empty();if(!Flush())return Out;Out.Triangles=0;Out.bSuccess=true;return Out;
}
