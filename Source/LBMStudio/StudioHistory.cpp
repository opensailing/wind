#include "StudioHistory.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <cstdlib>
#include <cerrno>

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioHistoryPrivate
{
constexpr int64 MaxPayloadBytes=64LL*1024*1024;
constexpr int64 MaxValues=8LL*1024*1024;
constexpr int32 MaxSamples=1000000,MaxColumns=64;

bool Clean(const FString& S,int32 Limit)
{
    if(S.TrimStartAndEnd().IsEmpty()||S.Len()>Limit) return false;
    for(TCHAR C:S) if(C<32||C==127) return false;
    return true;
}
bool HashValid(const FString& Hash)
{
    if(Hash.Len()!=64) return false;
    for(TCHAR C:Hash) if(!FChar::IsHexDigit(C)) return false;
    return true;
}
bool Identifier(const FString& Id)
{
    if(Id.IsEmpty()||Id.Len()>128) return false;
    for(TCHAR C:Id)
        if(!((C>='a'&&C<='z')||(C>='A'&&C<='Z')||(C>='0'&&C<='9')||C=='_'||C=='-'||C=='+'||C=='.')) return false;
    return true;
}
bool SHA256(const TArray<uint8>& Bytes,FString& Hash)
{
    uint8 Digest[EVP_MAX_MD_SIZE]; unsigned int Length=0;
    if(EVP_Digest(Bytes.GetData(),Bytes.Num(),Digest,&Length,EVP_sha256(),nullptr)!=1||Length!=32) return false;
    Hash=BytesToHex(Digest,Length).ToLower();return true;
}
bool Number(const FString& Token,double& Value)
{
    // Require a complete decimal token. LexFromString alone can accept a prefix.
    int32 I=0;
    if(Token.IsEmpty()||Token.Len()>64) return false;
    if(Token[I]=='+'||Token[I]=='-') ++I;
    int32 Digits=0;
    auto Digit=[](TCHAR C){return C>='0'&&C<='9';};
    while(I<Token.Len()&&Digit(Token[I])) {++I;++Digits;}
    if(I<Token.Len()&&Token[I]=='.')
    {++I;while(I<Token.Len()&&Digit(Token[I])) {++I;++Digits;}}
    if(!Digits) return false;
    if(I<Token.Len()&&(Token[I]=='e'||Token[I]=='E'))
    {
        ++I;if(I<Token.Len()&&(Token[I]=='+'||Token[I]=='-')) ++I;
        int32 ExponentDigits=0;
        while(I<Token.Len()&&Digit(Token[I])) {++I;++ExponentDigits;}
        if(!ExponentDigits) return false;
    }
    if(I!=Token.Len()) return false;
    const FTCHARToUTF8 Bytes(*Token);char* End=nullptr;
    errno=0;Value=std::strtod(Bytes.Get(),&End);
    return errno!=ERANGE&&End==Bytes.Get()+Bytes.Length()&&FMath::IsFinite(Value);
}
bool SmallFile(const FString& Path,int64 Limit,TArray<uint8>& Bytes,const FStudioLoadCancellation& Cancellation = {})
{
    const int64 Size=IFileManager::Get().FileSize(*Path);
    if(Size<=0||Size>Limit) return false;
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if(!Reader||Reader->TotalSize()!=Size) return false;
    Bytes.SetNumUninitialized(int32(Size));
    for(int64 Offset=0;Offset<Size;)
    {
        if(Cancellation&&Cancellation->load(std::memory_order_relaxed)) return false;
        const int64 Count=FMath::Min<int64>(65536,Size-Offset);
        Reader->Serialize(Bytes.GetData()+Offset,Count);Offset+=Count;
        if(Reader->IsError()) return false;
    }
    return !Reader->IsError();
}
}

const FStudioHistoryColumn* FStudioHistory::FindColumn(const FString& ColumnId) const
{ return Columns.FindByPredicate([&](const auto& C){return C.Id==ColumnId;}); }

TArray<FStudioHistoryEntry> StudioHistories::Installed()
{
    const FString Root=FPaths::ProjectContentDir()/TEXT("Samples");
    TArray<uint8> Bytes;
    if(!StudioHistoryPrivate::SmallFile(Root/TEXT("Registry/histories.json"),65536,Bytes)) return {};
    FString Text;FFileHelper::BufferToString(Text,Bytes.GetData(),Bytes.Num());
    TSharedPtr<FJsonObject> O;const TArray<TSharedPtr<FJsonValue>>* Entries=nullptr;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O)||!O||
        !O->TryGetArrayField(TEXT("histories"),Entries)||Entries->Num()>128) return {};
    TArray<FStudioHistoryEntry> Result;TSet<FString> Ids;
    for(const auto& Value:*Entries)
    {
        const TSharedPtr<FJsonObject>* Item=nullptr;FStudioHistoryEntry E;FString Relative;
        if(!Value->TryGetObject(Item)||!Item||!Item->IsValid()||
            !(*Item)->TryGetStringField(TEXT("id"),E.Id)||!StudioHistoryPrivate::Clean(E.Id,256)||Ids.Contains(E.Id)||
            !(*Item)->TryGetStringField(TEXT("title"),E.Title)||!StudioHistoryPrivate::Clean(E.Title,256)||
            !(*Item)->TryGetStringField(TEXT("metadataSHA256"),E.MetadataSHA256)||!StudioHistoryPrivate::HashValid(E.MetadataSHA256)||
            !(*Item)->TryGetStringField(TEXT("path"),Relative)||!StudioHistoryPrivate::Clean(Relative,512)||
            !FPaths::IsRelative(Relative)||Relative.Contains(TEXT(".."))||Relative.Contains(TEXT(":"))||
            Relative.Contains(TEXT("\\"))||!Relative.EndsWith(TEXT("/history.json"))) return {};
        E.Path=Root/Relative;Ids.Add(E.Id);Result.Add(MoveTemp(E));
    }
    return Result;
}

FStudioHistoryLoadResult StudioHistories::Load(const FString& Path,const FStudioLoadCancellation& Cancellation,
    const FString& ExpectedMetadataSHA256)
{
    FStudioHistoryLoadResult Result;
    auto Fail=[&](const TCHAR* Error){Result.Error=Error;return Result;};
    auto Cancelled=[&](){return Cancellation&&Cancellation->load(std::memory_order_relaxed);};
    if(Cancelled()) return Fail(TEXT("History loading cancelled."));
    FStudioFileAccess MetadataAccess(Path);
    TArray<uint8> Metadata;
    if(!StudioHistoryPrivate::SmallFile(Path,256*1024,Metadata,Cancellation)) return Fail(Cancelled()?TEXT("History loading cancelled."):TEXT("History metadata is missing or exceeds 256 KiB."));
    auto H=MakeShared<FStudioHistory,ESPMode::ThreadSafe>();
    if(!StudioHistoryPrivate::SHA256(Metadata,H->MetadataSHA256)) return Fail(TEXT("Cannot verify history metadata."));
    if(!ExpectedMetadataSHA256.IsEmpty()&&(!StudioHistoryPrivate::HashValid(ExpectedMetadataSHA256)||
        !H->MetadataSHA256.Equals(ExpectedMetadataSHA256,ESearchCase::IgnoreCase)))
        return Fail(TEXT("History metadata differs from the saved source. Locate an exact copy."));
    FString Text;FFileHelper::BufferToString(Text,Metadata.GetData(),Metadata.Num());
    TSharedPtr<FJsonObject> O;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O)||!O)
        return Fail(TEXT("History metadata is not valid JSON."));
    auto String=[&](const TCHAR* Key,FString& Out,int32 Limit=2048)
    {return O->TryGetStringField(Key,Out)&&StudioHistoryPrivate::Clean(Out,Limit);};
    double Version=0,Count=0,First=0,Last=0;FString Kind,Payload,TimeId,Origin,ProvenanceHash,LicenseHash;
    if(!O->TryGetNumberField(TEXT("version"),Version)||Version!=1||
        !String(TEXT("kind"),Kind)||Kind!=TEXT("scientific_history")||
        !String(TEXT("id"),H->Id,256)||!String(TEXT("title"),H->Title,256)||
        !String(TEXT("sourceURL"),H->SourceURL)||!String(TEXT("timeNote"),H->TimeNote)||
        !String(TEXT("origin"),Origin)||Origin!=TEXT("published_recording")||
        !String(TEXT("payload"),Payload,128)||Payload!=TEXT("history.csv")||
        !String(TEXT("timeColumn"),TimeId,128)||!StudioHistoryPrivate::Identifier(TimeId)||
        !String(TEXT("payloadSHA256"),H->PayloadSHA256)||!StudioHistoryPrivate::HashValid(H->PayloadSHA256)||
        !String(TEXT("sourceSHA256"),H->SourceSHA256)||!StudioHistoryPrivate::HashValid(H->SourceSHA256)||
        !String(TEXT("provenanceSHA256"),ProvenanceHash)||!StudioHistoryPrivate::HashValid(ProvenanceHash)||
        !String(TEXT("licenseSHA256"),LicenseHash)||!StudioHistoryPrivate::HashValid(LicenseHash)||
        !O->TryGetNumberField(TEXT("sampleCount"),Count)||!FMath::IsFinite(Count)||Count<1||Count>StudioHistoryPrivate::MaxSamples||Count!=FMath::FloorToDouble(Count)||
        !O->TryGetNumberField(TEXT("firstTime"),First)||!FMath::IsFinite(First)||
        !O->TryGetNumberField(TEXT("lastTime"),Last)||!FMath::IsFinite(Last)||Last<First)
        return Fail(TEXT("Unsupported or invalid scientific history descriptor."));
    O->TryGetStringField(TEXT("sourceDOI"),H->SourceDOI);
    if(!H->SourceDOI.IsEmpty()&&!StudioHistoryPrivate::Clean(H->SourceDOI,256)) return Fail(TEXT("History DOI is invalid."));
    if(const auto* Association=O->Values.Find(TEXT("fieldRecordingId")))
    {
        if((*Association)->Type!=EJson::Null)
        {
            FString Id;if(!(*Association)->TryGetString(Id)||!StudioHistoryPrivate::Clean(Id,256))
                return Fail(TEXT("Invalid history field-recording association."));
            H->FieldRecordingId=Id;
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* Columns=nullptr;
    if(!O->TryGetArrayField(TEXT("columns"),Columns)||Columns->Num()<2||Columns->Num()>StudioHistoryPrivate::MaxColumns||
        int64(Count)*Columns->Num()>StudioHistoryPrivate::MaxValues)
        return Fail(TEXT("History column/sample allocation exceeds the supported budget."));
    TArray<FString> Names;TSet<FString> Used;int32 TimeIndex=INDEX_NONE;
    for(const auto& Value:*Columns)
    {
        const TSharedPtr<FJsonObject>* C=nullptr;FStudioHistoryColumn Column;FString Role;
        if(!Value->TryGetObject(C)||!C||!C->IsValid()||
            !(*C)->TryGetStringField(TEXT("id"),Column.Id)||!StudioHistoryPrivate::Identifier(Column.Id)||Used.Contains(Column.Id)||
            !(*C)->TryGetStringField(TEXT("label"),Column.Label)||!StudioHistoryPrivate::Clean(Column.Label,256)||
            !(*C)->TryGetStringField(TEXT("unit"),Column.Unit)||!StudioHistoryPrivate::Clean(Column.Unit,128))
            return Fail(TEXT("History columns require unique IDs, labels and explicit units."));
        Used.Add(Column.Id);Names.Add(Column.Id);(*C)->TryGetStringField(TEXT("role"),Role);
        if(Column.Id==TimeId)
        {
            if(Role!=TEXT("time")) return Fail(TEXT("History time column is not identified as time."));
            TimeIndex=Names.Num()-1;H->TimeUnit=Column.Unit;H->Times.Reserve(int32(Count));
        }
        else
        {
            if(!Role.IsEmpty()||!(*C)->TryGetStringField(TEXT("origin"),Column.Origin)||
                (Column.Origin!=TEXT("source")&&Column.Origin!=TEXT("derived")))
                return Fail(TEXT("History value origin must be source or derived."));
            if(Column.Origin==TEXT("derived")&&(!(*C)->TryGetStringField(TEXT("expression"),Column.Expression)||!StudioHistoryPrivate::Clean(Column.Expression,2048)))
                return Fail(TEXT("Derived history values need an explicit calculation."));
            Column.Values.Reserve(int32(Count));H->Columns.Add(MoveTemp(Column));
        }
    }
    if(TimeIndex==INDEX_NONE) return Fail(TEXT("History time column is missing."));
    if(O->HasField(TEXT("referenceValues")))
    {
        const TSharedPtr<FJsonObject>* References=nullptr;
        if(!O->TryGetObjectField(TEXT("referenceValues"),References)||!References||!References->IsValid()||(*References)->Values.Num()>32)
            return Fail(TEXT("Invalid history reference quantities."));
        for(const auto& Pair:(*References)->Values)
        {
            const FString Key(Pair.Key);
            double V;
            if(!StudioHistoryPrivate::Identifier(Key)||!Pair.Value->TryGetNumber(V)||!FMath::IsFinite(V))
                return Fail(TEXT("History reference quantities need finite values and unit-bearing identifiers."));
            H->ReferenceValues.Add(Key,V);
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* Notes=nullptr;
    if(O->TryGetArrayField(TEXT("limitations"),Notes))
    {
        if(Notes->Num()>32) return Fail(TEXT("Too many history limitation notes."));
        for(const auto& N:*Notes)
        {FString Note;if(!N->TryGetString(Note)||!StudioHistoryPrivate::Clean(Note,2048)) return Fail(TEXT("Invalid history limitation note."));H->Limitations.Add(Note);}
    }
    const FString Folder=FPaths::GetPath(Path);
    for(const auto& File:{TPair<FString,FString>(TEXT("history-provenance.json"),ProvenanceHash),
                         TPair<FString,FString>(TEXT("LICENSE.txt"),LicenseHash)})
    {
        if(Cancelled()) return Fail(TEXT("History loading cancelled."));
        FStudioFileAccess Access(Folder/File.Key);TArray<uint8> Bytes;FString Hash;
        if(!StudioHistoryPrivate::SmallFile(Folder/File.Key,256*1024,Bytes,Cancellation)||!StudioHistoryPrivate::SHA256(Bytes,Hash)||!Hash.Equals(File.Value,ESearchCase::IgnoreCase))
            return Fail(Cancelled()?TEXT("History loading cancelled."):TEXT("History provenance or source notice is missing or changed."));
    }
    if(Cancelled()) return Fail(TEXT("History loading cancelled."));
    FStudioFileAccess PayloadAccess(Folder/Payload);TArray<uint8> Bytes;FString Hash;
    if(!StudioHistoryPrivate::SmallFile(Folder/Payload,StudioHistoryPrivate::MaxPayloadBytes,Bytes,Cancellation)||!StudioHistoryPrivate::SHA256(Bytes,Hash)||!Hash.Equals(H->PayloadSHA256,ESearchCase::IgnoreCase))
        return Fail(Cancelled()?TEXT("History loading cancelled."):TEXT("History payload is missing, changed or exceeds 64 MiB."));
    // Parse one bounded ASCII line at a time. No array of every CSV string.
    FString Line;int32 Row=-1;
    for(int32 I=0;I<=Bytes.Num();++I)
    {
        if((I&4095)==0&&Cancelled()) return Fail(TEXT("History loading cancelled."));
        const uint8 C=I<Bytes.Num()?Bytes[I]:'\n';
        if(C=='\n')
        {
            if(Line.EndsWith(TEXT("\r"))) Line.LeftChopInline(1);
            if(Line.IsEmpty())
            {if(I==Bytes.Num()) break;return Fail(TEXT("Empty row in history CSV."));}
            TArray<FString> Tokens;Line.ParseIntoArray(Tokens,TEXT(","),false);Line.Reset();
            if(Tokens.Num()!=Names.Num()) return Fail(TEXT("History CSV column count differs from metadata."));
            if(Row<0)
            {if(Tokens!=Names) return Fail(TEXT("History CSV headers differ from metadata."));Row=0;continue;}
            if(Row>=int32(Count)) return Fail(TEXT("History CSV contains extra samples."));
            int32 Destination=0;
            for(int32 J=0;J<Tokens.Num();++J)
            {
                double V;
                if(!StudioHistoryPrivate::Number(Tokens[J],V)) return Fail(TEXT("History contains a missing, nonfinite or invalid value."));
                if(J==TimeIndex)
                {
                    if(!H->Times.IsEmpty()&&V<=H->Times.Last()) return Fail(TEXT("History times must increase strictly."));
                    H->Times.Add(V);
                }
                else H->Columns[Destination++].Values.Add(V);
            }
            ++Row;
        }
        else
        {
            if((C<32&&C!='\r')||C>126||Line.Len()>=StudioHistoryPrivate::MaxColumns*65)
                return Fail(TEXT("History CSV contains unsupported text or an oversized row."));
            Line.AppendChar(TCHAR(C));
        }
    }
    if(Row!=int32(Count)||H->Times[0]!=First||H->Times.Last()!=Last)
        return Fail(TEXT("History sample count or time extent differs from metadata."));
    if(Cancelled()) return Fail(TEXT("History loading cancelled."));
    Result.History=MoveTemp(H);return Result;
}
