#include "StudioHome4ArchivePrivate.h"
#include "StudioAssets.h"
#include "StudioHome4JSON.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Containers/StringConv.h"
#include <cmath>
#include <cstring>
THIRD_PARTY_INCLUDES_START
#include <minizip/unzip.h>
THIRD_PARTY_INCLUDES_END

namespace StudioHome4ArchivePrivate
{
bool Cancelled(const FStudioLoadCancellation& C){return C&&C->load(std::memory_order_relaxed);}
bool CleanText(const FString& T,int32 Limit,bool Multi)
{
    if(T.IsEmpty()||T.Len()>Limit||T.TrimStartAndEnd().IsEmpty())return false;
    for(TCHAR C:T)if(C==0||C==127||(C<32&&!(Multi&&(C=='\n'||C=='\r'||C=='\t'))))return false;
    return true;
}
bool DecodeUTF8(const uint8* B,int32 N,FString& Out)
{
    for(int32 I=0;I<N;)
    {
        uint32 C=B[I++];int32 Rest=0;uint32 Min=0;
        if(C<128){if(C==0)return false;continue;}
        if(C>=0xc2&&C<=0xdf){C&=31;Rest=1;Min=128;}
        else if(C>=0xe0&&C<=0xef){C&=15;Rest=2;Min=2048;}
        else if(C>=0xf0&&C<=0xf4){C&=7;Rest=3;Min=65536;}
        else return false;
        if(I>N-Rest)return false;
        while(Rest--){const uint8 Q=B[I++];if((Q&0xc0)!=0x80)return false;C=(C<<6)|(Q&63);}
        if(C<Min||C>0x10ffff||(C>=0xd800&&C<=0xdfff))return false;
    }
    FUTF8ToTCHAR Converted(reinterpret_cast<const UTF8CHAR*>(B),N);
    Out=FString(Converted.Length(),Converted.Get());return true;
}
FString Serialize(const TSharedRef<FJsonObject>& O)
{FString T;FJsonSerializer::Serialize(O,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&T));return T;}
bool JSON(const FString& Text,TSharedPtr<FJsonObject>& Out,FString& Error)
{
    Error=TEXT("Original run_spec must be bounded JSON with unique keys and finite numbers.");
    if(Text.Len()>1024*1024||!StudioHome4JSON::Preflight(Text,16))return false;
    struct FScope {bool bObject=false;TSet<FString> Keys;int32 Count=0;};
    TArray<FScope> Stack;auto R=TJsonReaderFactory<>::Create(Text);EJsonNotation N;
    while(R->ReadNext(N))
    {
        if(N==EJsonNotation::Error)return false;
        if(N==EJsonNotation::ObjectEnd||N==EJsonNotation::ArrayEnd)
        {if(Stack.IsEmpty())return false;Stack.Pop(EAllowShrinking::No);continue;}
        if(!Stack.IsEmpty())
        {
            auto& S=Stack.Last();if(++S.Count>256)return false;
            if(S.bObject){const FString K=R->GetIdentifier();if(K.Len()>120||S.Keys.Contains(K))return false;S.Keys.Add(K);}
        }
        if(N==EJsonNotation::String&&R->GetValueAsString().Len()>16384)return false;
        if(N==EJsonNotation::Number&&!FMath::IsFinite(R->GetValueAsNumber()))return false;
        if(N==EJsonNotation::ObjectStart||N==EJsonNotation::ArrayStart)
        {if(Stack.Num()>=16)return false;FScope S;S.bObject=N==EJsonNotation::ObjectStart;Stack.Add(MoveTemp(S));}
    }
    if(!Stack.IsEmpty()||!R->GetErrorMessage().IsEmpty())return false;
    TSharedPtr<FJsonObject> Candidate;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Candidate)||!Candidate)return false;
    Out=MoveTemp(Candidate);Error.Empty();return true;
}
bool Hash(const FString& Path,FString& Out,const FStudioLoadCancellation& C,FString& E)
{return StudioAssets::HashFile(Path,C?C.ToSharedRef():MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false),Out,E);}

// A deliberately small literal grammar, never Python evaluation or pickle.
class FNpyHeader
{
public:
    FNpyHeader(const uint8* B,int32 N):Bytes(B),Count(N){}
    bool Parse(FStudioHome4ArchiveMember& M)
    {
        if(!Take('{'))return false;TSet<FString> Seen;
        while(true)
        {
            FString Key;if(!String(Key)||!Take(':')||Seen.Contains(Key))return false;Seen.Add(Key);
            if(Key==TEXT("descr")){if(!String(M.DType))return false;}
            else if(Key==TEXT("fortran_order"))
            {if(Word("True"))M.bFortran=true;else if(Word("False"))M.bFortran=false;else return false;}
            else if(Key==TEXT("shape"))
            {
                if(!Take('('))return false;
                if(!Take(')'))while(true)
                {
                    int64 V=0;if(!Number(V)||M.Shape.Num()>=8)return false;M.Shape.Add(V);
                    if(!Take(','))return false; // One-element tuple needs a comma.
                    if(Take(')'))break;
                    // Multi-element tuple permits closing without a final comma.
                    int32 Saved=At;int64 Next=0;
                    if(Number(Next)){Space();if(At<Count&&Bytes[At]==')'){if(M.Shape.Num()>=8)return false;M.Shape.Add(Next);++At;break;}}
                    At=Saved;
                }
            }
            else return false;
            if(Take('}'))break;if(!Take(','))return false;if(Take('}'))break;
        }
        Space();if(At!=Count||Seen.Num()!=3||M.DType.Len()<3)return false;
        const TCHAR Endian=M.DType[0],Kind=M.DType[1];
        if(Endian!='<'&&Endian!='>'&&Endian!='|'&&Endian!='=')return false;
        int64 Width=0;const FString Digits=M.DType.Mid(2);
        if(Digits.IsEmpty()||Digits.Len()>6)return false;
        for(TCHAR C:Digits){if(C<'0'||C>'9')return false;Width=Width*10+C-'0';}
        if(Kind=='U')Width*=4;
        if(Kind=='b'){if(Width!=1)return false;}
        else if(Kind=='i'||Kind=='u'){if(Width!=1&&Width!=2&&Width!=4&&Width!=8)return false;}
        else if(Kind=='f'){if(Width!=2&&Width!=4&&Width!=8)return false;}
        else if(Kind=='S'||Kind=='U'){if(!M.Shape.IsEmpty()||Width<1||Width>65536)return false;}
        else return false;
        if(Width>1&&Endian=='|'&&Kind!='S')return false;
        M.ItemBytes=int32(Width);M.Count=1;
        for(int64 V:M.Shape){if(V<0||V>16000000||(V&&M.Count>16000000/V))return false;M.Count*=V;}
        M.PayloadBytes=M.Count*M.ItemBytes;return true;
    }
private:
    void Space(){while(At<Count&&(Bytes[At]==' '||Bytes[At]=='\n'||Bytes[At]=='\r'||Bytes[At]=='\t'))++At;}
    bool Take(uint8 C){Space();if(At>=Count||Bytes[At]!=C)return false;++At;return true;}
    bool Word(const char* T){Space();const int32 N=int32(strlen(T));if(At>Count-N||memcmp(Bytes+At,T,N))return false;At+=N;return true;}
    bool Number(int64& V)
    {Space();int32 Start=At;while(At<Count&&Bytes[At]>='0'&&Bytes[At]<='9'){if(V>16000000)return false;V=V*10+Bytes[At++]-'0';}return At>Start&&V<=16000000;}
    bool String(FString& S)
    {
        Space();if(At>=Count||(Bytes[At]!='\''&&Bytes[At]!='"'))return false;const uint8 Quote=Bytes[At++];int32 Start=At;
        while(At<Count&&Bytes[At]!=Quote){if(Bytes[At]<32||Bytes[At]>126||Bytes[At]=='\\'||At-Start>=128)return false;++At;}
        if(At>=Count)return false;const auto Converted=StringCast<TCHAR>(reinterpret_cast<const ANSICHAR*>(Bytes+Start),At-Start);
        S=FString(Converted.Length(),Converted.Get());++At;return true;
    }
    const uint8* Bytes;int32 Count,At=0;
};

uint64 RawInteger(const uint8* B,int32 N,bool Little)
{uint64 V=0;for(int32 I=0;I<N;++I)V|=uint64(B[I])<<(8*(Little?I:N-1-I));return V;}
bool Numeric(const uint8* B,const FStudioHome4ArchiveMember& M,double& V)
{
    const bool Little=M.DType[0]=='<'||M.DType[0]=='|'||(M.DType[0]=='='&&PLATFORM_LITTLE_ENDIAN);
    const uint64 Bits=RawInteger(B,M.ItemBytes,Little);const TCHAR K=M.DType[1];
    if(K=='b'){if(Bits>1)return false;V=double(Bits);}
    else if(K=='u'){if(Bits>9007199254740992ULL)return false;V=double(Bits);}
    else if(K=='i')
    {
        const int32 Shift=64-M.ItemBytes*8;const int64 Signed=static_cast<int64>(Bits<<Shift)>>Shift;
        if(Signed<-9007199254740992LL||Signed>9007199254740992LL)return false;V=double(Signed);
    }
    else if(K=='f')
    {
        if(M.ItemBytes==8){FMemory::Memcpy(&V,&Bits,8);}
        else if(M.ItemBytes==4){const uint32 U=uint32(Bits);float F;FMemory::Memcpy(&F,&U,4);V=F;}
        else
        {
            const int32 E=(Bits>>10)&31,F=Bits&1023;const double Sign=(Bits&32768)?-1.:1.;
            if(E==31)return false;V=Sign*std::ldexp(E?1.+F/1024.:F/1024.,E?E-15:-14);
        }
    }
    else return false;
    return FMath::IsFinite(V);
}

struct FNpzReader::FImpl
{
    struct FEntry {unz64_file_pos Position;int64 Expanded=0;};
    TUniquePtr<FArchive> File;unzFile Zip=nullptr;FStudioLoadCancellation Cancel;
    TArray<FStudioHome4ArchiveMember> Meta;TMap<FString,FEntry> Entries;bool bIOError=false;
    ~FImpl(){if(Zip)unzClose(Zip);}
    static voidpf ZCALLBACK Open(voidpf O,const void*,int Mode)
    {auto* I=static_cast<FImpl*>(O);return Mode== (ZLIB_FILEFUNC_MODE_READ|ZLIB_FILEFUNC_MODE_EXISTING)?I:nullptr;}
    static uLong ZCALLBACK Read(voidpf,voidpf S,void* B,uLong N)
    {
        auto& I=*static_cast<FImpl*>(S);if(Cancelled(I.Cancel)||I.bIOError)return 0;
        const int64 Remaining=I.File->TotalSize()-I.File->Tell();const int64 Count=FMath::Min<int64>(N,Remaining);
        if(Count<0){I.bIOError=true;return 0;}I.File->Serialize(B,Count);I.bIOError=I.File->IsError();return I.bIOError?0:uLong(Count);
    }
    static ZPOS64_T ZCALLBACK Tell(voidpf,voidpf S){return static_cast<FImpl*>(S)->File->Tell();}
    static long ZCALLBACK Seek(voidpf,voidpf S,ZPOS64_T Offset,int Origin)
    {
        auto& I=*static_cast<FImpl*>(S);uint64 Base=Origin==ZLIB_FILEFUNC_SEEK_CUR?I.File->Tell():Origin==ZLIB_FILEFUNC_SEEK_END?I.File->TotalSize():0;
        // MiniZip uses nonnegative absolute offsets on this seekable read-only stream.
        if(Offset>uint64(I.File->TotalSize())||Base>uint64(I.File->TotalSize())-Offset){I.bIOError=true;return -1;}
        I.File->Seek(Base+Offset);I.bIOError=I.File->IsError();return I.bIOError?-1:0;
    }
    static int ZCALLBACK Close(voidpf,voidpf){return 0;}
    static int ZCALLBACK Error(voidpf,voidpf S){return static_cast<FImpl*>(S)->bIOError?1:0;}
    bool Exact(void* B,int32 N)
    {
        auto* Bytes=static_cast<uint8*>(B);int32 Done=0;
        while(Done<N){if(Cancelled(Cancel))return false;const int32 Got=unzReadCurrentFile(Zip,Bytes+Done,N-Done);if(Got<=0)return false;Done+=Got;}return true;
    }
    bool Header(FStudioHome4ArchiveMember& M,int64 Expanded)
    {
        uint8 Prefix[12];if(!Exact(Prefix,8)||memcmp(Prefix,"\x93NUMPY",6)||Prefix[7]!=0||(Prefix[6]!=1&&Prefix[6]!=2))return false;
        const int32 LengthBytes=Prefix[6]==1?2:4;if(!Exact(Prefix+8,LengthBytes))return false;
        const uint64 Length=RawInteger(Prefix+8,LengthBytes,true);if(!Length||Length>8192)return false;
        TArray<uint8> Header;Header.SetNumUninitialized(int32(Length));if(!Exact(Header.GetData(),Header.Num())||Header.Last()!='\n')return false;
        return FNpyHeader(Header.GetData(),Header.Num()).Parse(M)&&8+LengthBytes+int64(Length)+M.PayloadBytes==Expanded;
    }
};
FNpzReader::FNpzReader():Impl(MakeUnique<FImpl>()){}
FNpzReader::~FNpzReader()=default;
const TArray<FStudioHome4ArchiveMember>& FNpzReader::Members()const{return Impl->Meta;}
bool FNpzReader::Open(const FString& Path,const FStudioLoadCancellation& C,FString& E)
{
    E=TEXT("Invalid or unsupported NPZ container/header.");Impl->Cancel=C;
    Impl->File.Reset(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if(!Impl->File||Impl->File->TotalSize()<=0||Impl->File->TotalSize()>StudioHome4Archives::MaximumArchiveBytes)return false;
    zlib_filefunc64_def IO{};IO.opaque=Impl.Get();IO.zopen64_file=FImpl::Open;IO.zread_file=FImpl::Read;
    IO.ztell64_file=FImpl::Tell;IO.zseek64_file=FImpl::Seek;IO.zclose_file=FImpl::Close;IO.zerror_file=FImpl::Error;
    Impl->Zip=unzOpen2_64(nullptr,&IO);if(!Impl->Zip)return false;
    unz_global_info64 Global{};if(unzGetGlobalInfo64(Impl->Zip,&Global)!=UNZ_OK||Global.number_entry<1||Global.number_entry>128)return false;
    TSet<FString> Seen;uint64 Total=0;if(unzGoToFirstFile(Impl->Zip)!=UNZ_OK)return false;
    for(uint64 Index=0;Index<Global.number_entry;++Index)
    {
        if(Cancelled(C))return false;unz_file_info64 Info{};
        if(unzGetCurrentFileInfo64(Impl->Zip,&Info,nullptr,0,nullptr,0,nullptr,0)!=UNZ_OK||Info.size_filename<5||Info.size_filename>124||
            Info.disk_num_start!=0||(Info.flag&1)|| (Info.compression_method!=0&&Info.compression_method!=8)||
            ((Info.external_fa>>16)&0170000)==0120000||((Info.external_fa>>16)&0170000)==0040000||(Info.external_fa&16)||Info.uncompressed_size<10||!Info.compressed_size||
            Info.compressed_size>uint64(Impl->File->TotalSize())||Info.uncompressed_size>StudioHome4Archives::MaximumArchiveBytes||
            Info.uncompressed_size>Info.compressed_size*4096||Total>StudioHome4Archives::MaximumArchiveBytes-Info.uncompressed_size)return false;
        Total+=Info.uncompressed_size;char Name[125]{};
        if(unzGetCurrentFileInfo64(Impl->Zip,&Info,Name,sizeof(Name),nullptr,0,nullptr,0)!=UNZ_OK)return false;
        for(uint32 J=0;J<Info.size_filename;++J)
        {const char Q=Name[J];if(!((Q>='A'&&Q<='Z')||(Q>='a'&&Q<='z')||(Q>='0'&&Q<='9')||Q=='_'||Q=='.'||Q=='-'))return false;}
        FString Full(ANSI_TO_TCHAR(Name));if(!Full.EndsWith(TEXT(".npy"),ESearchCase::CaseSensitive))return false;
        FString Key=Full.LeftChop(4);if(Key==TEXT(".")||Key==TEXT("..")||Seen.Contains(Key.ToLower()))return false;Seen.Add(Key.ToLower());
        FImpl::FEntry Entry;Entry.Expanded=int64(Info.uncompressed_size);
        if(unzGetFilePos64(Impl->Zip,&Entry.Position)!=UNZ_OK||unzOpenCurrentFile(Impl->Zip)!=UNZ_OK)return false;
        FStudioHome4ArchiveMember M;M.Name=Key;const bool Good=Impl->Header(M,Entry.Expanded);const int32 Closed=unzCloseCurrentFile(Impl->Zip);
        if(!Good||Closed!=UNZ_OK)return false;Impl->Entries.Add(Key,Entry);Impl->Meta.Add(MoveTemp(M));
        const int32 Next=unzGoToNextFile(Impl->Zip);if(Next!=(Index+1==Global.number_entry?UNZ_END_OF_LIST_OF_FILE:UNZ_OK))return false;
    }
    E.Empty();return true;
}
bool FNpzReader::Read(const FString& Name,FNumericArray& Out,FString& E,bool bNumeric)
{
    E=TEXT("NPY payload is invalid, nonfinite, inexact or has a failed CRC: ")+Name;
    const auto* Entry=Impl->Entries.Find(Name);const auto* Meta=Impl->Meta.FindByPredicate([&](const auto& M){return M.Name==Name;});
    if(!Entry||!Meta||unzGoToFilePos64(Impl->Zip,&Entry->Position)!=UNZ_OK||unzOpenCurrentFile(Impl->Zip)!=UNZ_OK)return false;
    FNumericArray Candidate;Candidate.Member.Name=Name;bool Good=Impl->Header(Candidate.Member,Entry->Expanded);
    const auto& M=Candidate.Member;const TCHAR Kind=Good?M.DType[1]:0;
    if(Good&&Kind!='S'&&Kind!='U')
    {
        if(!bNumeric)Good=false;
        else
        {
            Candidate.Values.SetNumUninitialized(int32(M.Count));uint8 Buffer[65536];int64 Done=0;
            while(Good&&Done<M.Count)
            {
                const int32 N=int32(FMath::Min<int64>(M.Count-Done,sizeof(Buffer)/M.ItemBytes));Good=Impl->Exact(Buffer,N*M.ItemBytes);
                for(int32 I=0;Good&&I<N;++I)Good=Numeric(Buffer+I*M.ItemBytes,M,Candidate.Values[int32(Done)+I]);Done+=N;
            }
        }
    }
    else if(Good)
    {
        if(bNumeric)Good=false;
        else
        {
            TArray<uint8> B;B.SetNumUninitialized(M.ItemBytes);Good=Impl->Exact(B.GetData(),B.Num());
            if(Good&&Kind=='S'){while(!B.IsEmpty()&&B.Last()==0)B.Pop(EAllowShrinking::No);Good=DecodeUTF8(B.GetData(),B.Num(),Candidate.StringValue);}
            else if(Good)
            {
                TArray<uint8> UTF8;const bool Little=M.DType[0]=='<'||(M.DType[0]=='='&&PLATFORM_LITTLE_ENDIAN);bool End=false;
                for(int32 I=0;Good&&I<B.Num();I+=4)
                {
                    const uint32 Q=uint32(RawInteger(B.GetData()+I,4,Little));if(!Q){End=true;continue;}
                    if(End||Q>0x10ffff||(Q>=0xd800&&Q<=0xdfff)){Good=false;break;}
                    if(Q<128)UTF8.Add(uint8(Q));else if(Q<2048){UTF8.Add(0xc0|(Q>>6));UTF8.Add(0x80|(Q&63));}
                    else if(Q<65536){UTF8.Add(0xe0|(Q>>12));UTF8.Add(0x80|((Q>>6)&63));UTF8.Add(0x80|(Q&63));}
                    else{UTF8.Add(0xf0|(Q>>18));UTF8.Add(0x80|((Q>>12)&63));UTF8.Add(0x80|((Q>>6)&63));UTF8.Add(0x80|(Q&63));}
                }
                if(Good)Good=DecodeUTF8(UTF8.GetData(),UTF8.Num(),Candidate.StringValue);
            }
            if(Good&&!CleanText(Candidate.StringValue,16384,true))Good=false;
        }
    }
    uint8 Tail=0;if(Good)Good=unzReadCurrentFile(Impl->Zip,&Tail,1)==0;
    const int32 Closed=unzCloseCurrentFile(Impl->Zip);if(!Good||Closed!=UNZ_OK||Cancelled(Impl->Cancel))return false;
    Out=MoveTemp(Candidate);E.Empty();return true;
}
bool FNpzReader::Scan(FString& E)
{
    for(const auto& M:Impl->Meta){FNumericArray A;if(!Read(M.Name,A,E,M.DType[1]!='S'&&M.DType[1]!='U'))return false;}return true;
}
}
