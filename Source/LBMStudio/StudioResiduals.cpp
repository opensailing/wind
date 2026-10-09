#include "StudioResiduals.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include <cerrno>
#include <cstdlib>

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioResidualPrivate
{
constexpr int64 MaxBytes=256LL*1024*1024,MaxValues=8LL*1024*1024;
constexpr int32 MaxLine=32768,MaxFields=31,MaxTimes=1000000,MaxRecords=2000000,MaxStepRecords=4096;
// Interpretation identity changes whenever selection, units or parser semantics change.
constexpr const ANSICHAR* Interpretation="openfoam-residual-history-v1:first-initial:last-final:source-time-units";
struct FDigest
{
    EVP_MD_CTX* Context=EVP_MD_CTX_new();
    ~FDigest(){EVP_MD_CTX_free(Context);}
    bool Begin(){return Context&&EVP_DigestInit_ex(Context,EVP_sha256(),nullptr)==1;}
    bool Add(const void* Data,int64 Count){return EVP_DigestUpdate(Context,Data,Count)==1;}
    bool Finish(FString& Hash)
    {
        uint8 Bytes[EVP_MAX_MD_SIZE];unsigned int Count=0;
        if(EVP_DigestFinal_ex(Context,Bytes,&Count)!=1||Count!=32)return false;
        Hash=BytesToHex(Bytes,Count).ToLower();return true;
    }
};
bool Number(const FString& Text,double& Value)
{
    int32 I=0,Digits=0;auto Digit=[](TCHAR C){return C>='0'&&C<='9';};
    if(Text.IsEmpty()||Text.Len()>64)return false;
    if(Text[I]=='+'||Text[I]=='-')++I;
    while(I<Text.Len()&&Digit(Text[I])){++I;++Digits;}
    if(I<Text.Len()&&Text[I]=='.'){++I;while(I<Text.Len()&&Digit(Text[I])){++I;++Digits;}}
    if(!Digits)return false;
    if(I<Text.Len()&&(Text[I]=='e'||Text[I]=='E'))
    {
        ++I;if(I<Text.Len()&&(Text[I]=='+'||Text[I]=='-'))++I;
        int32 Exponents=0;while(I<Text.Len()&&Digit(Text[I])){++I;++Exponents;}if(!Exponents)return false;
    }
    if(I!=Text.Len())return false;
    FTCHARToUTF8 Bytes(*Text);char* End=nullptr;errno=0;Value=std::strtod(Bytes.Get(),&End);
    return errno!=ERANGE&&End==Bytes.Get()+Bytes.Length()&&FMath::IsFinite(Value);
}
bool FieldName(const FString& Text)
{
    auto Letter=[](TCHAR C){return (C>='A'&&C<='Z')||(C>='a'&&C<='z')||C=='_';};
    if(Text.IsEmpty()||Text.Len()>100||!Letter(Text[0]))return false;
    for(TCHAR C:Text)if(!Letter(C)&&!(C>='0'&&C<='9')&&C!='.'&&C!=':'&&C!='('&&C!=')')return false;
    return true;
}
bool Integer(const FString& Text)
{
    if(Text.IsEmpty()||Text.Len()>10)return false;
    for(TCHAR C:Text)if(C<'0'||C>'9')return false;
    return FCString::Atoi64(*Text)<=1000000000;
}
struct FSelection {double Initial=0,Final=0;int32 FirstLine=0,LastLine=0,Count=0;};
}

FStudioHistoryLoadResult StudioResiduals::Load(const FString& Path,const FStudioLoadCancellation& Cancellation,
    const FString& ExpectedMetadataSHA256)
{
    using namespace StudioResidualPrivate;
    FStudioHistoryLoadResult Result;
    auto Fail=[&](const FString& Error){Result.Error=Error;return Result;};
    auto Cancelled=[&]{return Cancellation&&Cancellation->load(std::memory_order_relaxed);};
    if(Cancelled())return Fail(TEXT("Residual log loading cancelled."));
    FStudioFileAccess Access(Path);
    const auto Before=IFileManager::Get().GetTimeStamp(*Path);
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if(!File)return Fail(TEXT("Residual log is missing or inaccessible. Locate the original completed log."));
    const int64 Size=File->TotalSize();
    if(Size<=0||Size>MaxBytes)return Fail(TEXT("Residual log must contain at most 256 MiB."));
    FDigest Digest;if(!Digest.Begin())return Fail(TEXT("Could not verify the residual log."));
    auto H=MakeShared<FStudioHistory,ESPMode::ThreadSafe>();
    H->bResiduals=true;H->SourcePath=FPaths::ConvertRelativePathToFull(Path);
    H->Title=FPaths::GetCleanFilename(Path)+TEXT(" · OpenFOAM residuals");
    H->TimeUnit=TEXT("source units");
    H->TimeNote=TEXT("Original Time = values; units are not declared by this log. No offset or resampling.");
    H->Limitations={TEXT("First initial and last final linear-solver residual per field at each source time; inner solves remain in the original log."),
        TEXT("Values retain the solver's reported normalization. No renormalization or cross-solver equivalence is asserted."),
        TEXT("Residual thresholds and convergence status are not inferred. Zero residuals remain zero and are omitted only on logarithmic axes."),
        TEXT("External source log; no association to the displayed flow recording or active job is inferred.")};
    TArray<FString> Fields;TMap<FString,FSelection> Selected;
    double Time=0,Previous=0;int32 TimeLine=0,LineNumber=0,StepRecords=0;
    bool bBanner=false,bActive=false,bEnded=false,bHasTime=false;
    FString Error,Line;
    auto Reject=[&](const FString& Detail){Error=FString::Printf(TEXT("%s (source line %d)."),*Detail,LineNumber);return false;};
    auto FinishStep=[&]
    {
        if(!bActive||!StepRecords)return Reject(TEXT("Time block has no linear solves"));
        if(H->Times.IsEmpty())
        {
            Selected.GetKeys(Fields);
            Fields.Sort([](const FString& A,const FString& B){return A.Compare(B,ESearchCase::CaseSensitive)<0;});
            for(const auto& Field:Fields)for(bool bInitial:{true,false})
            {
                FStudioHistoryColumn C;C.Id=Field+(bInitial?TEXT(".InitialFirst"):TEXT(".FinalLast"));
                C.Label=Field+(bInitial?TEXT(" · first initial"):TEXT(" · last final"));
                C.Unit=TEXT("reported residual");C.Origin=TEXT("source");
                C.Expression=bInitial?TEXT("First reported initial residual for this field at each Time = block."):
                    TEXT("Last reported final residual for this field at each Time = block.");
                H->Columns.Add(MoveTemp(C));
            }
        }
        if(Selected.Num()!=Fields.Num())return Reject(TEXT("Fields change between time blocks; no absent residual is filled with zero"));
        if(H->Times.Num()>=MaxTimes||int64(H->Times.Num()+1)*(H->Columns.Num()+1)>MaxValues)
            return Reject(TEXT("Residual history exceeds the time/value allocation budget"));
        for(int32 I=0;I<Fields.Num();++I)
        {
            const auto* S=Selected.Find(Fields[I]);if(!S)return Reject(TEXT("A field is missing from this time block"));
            H->Columns[2*I].Values.Add(S->Initial);H->Columns[2*I].SourceLines.Add(S->FirstLine);
            H->Columns[2*I+1].Values.Add(S->Final);H->Columns[2*I+1].SourceLines.Add(S->LastLine);
        }
        H->Times.Add(Time);H->TimeSourceLines.Add(TimeLine);Selected.Reset();StepRecords=0;bActive=false;return true;
    };
    auto Consume=[&]() -> bool
    {
        ++LineNumber;Line.TrimStartAndEndInline();
        if(Line.Contains(TEXT("OpenFOAM:")))
        {
            if(bBanner||bHasTime)return Reject(TEXT("Concatenated or repeated OpenFOAM log"));
            bBanner=true;
        }
        const FString TimeTail=Line.StartsWith(TEXT("Time"))?Line.Mid(4).TrimStartAndEnd():FString();
        if(TimeTail.StartsWith(TEXT("=")))
        {
            double Next=0;
            if(!bBanner||bEnded||!Number(TimeTail.Mid(1).TrimStartAndEnd(),Next)||(bHasTime&&Next<=Previous))
                return Reject(TEXT("Invalid or nonincreasing source time"));
            if(bActive&&!FinishStep())return false;
            Time=Previous=Next;TimeLine=LineNumber;bActive=bHasTime=true;return true;
        }
        if(Line.Contains(TEXT("Solving for ")))
        {
            if(!bActive||bEnded)return Reject(TEXT("Linear solve without an active time block"));
            FString Solver,Body,Field,Initial,Final,Iterations,Tail;
            if(!Line.Split(TEXT(":"),&Solver,&Body))return Reject(TEXT("Malformed linear solve"));
            Solver.TrimStartAndEndInline();Body.TrimStartAndEndInline();
            for(TCHAR C:Solver)if(FChar::IsWhitespace(C))return Reject(TEXT("Malformed solver name"));
            if(Solver.IsEmpty()||Solver.Len()>128||!Body.RemoveFromStart(TEXT("Solving for "))||
                !Body.Split(TEXT(", Initial residual ="),&Field,&Tail)||
                !Tail.Split(TEXT(", Final residual ="),&Initial,&Body)||
                !Body.Split(TEXT(", No Iterations "),&Final,&Iterations))return Reject(TEXT("Malformed residual record"));
            Field.TrimStartAndEndInline();Initial.TrimStartAndEndInline();Final.TrimStartAndEndInline();Iterations.TrimStartAndEndInline();
            double A=0,B=0;
            if(!FieldName(Field)||!Number(Initial,A)||!Number(Final,B)||A<0||B<0||!Integer(Iterations))
                return Reject(TEXT("Invalid residual or linear iteration count"));
            if(++H->SourceRecordCount>MaxRecords||++StepRecords>MaxStepRecords)
                return Reject(TEXT("Residual record budget exceeded"));
            if(!Selected.Contains(Field)&&Selected.Num()>=MaxFields)return Reject(TEXT("More than 31 residual fields"));
            auto& S=Selected.FindOrAdd(Field);
            if(!S.Count){S.Initial=A;S.FirstLine=LineNumber;}
            S.Final=B;S.LastLine=LineNumber;++S.Count;return true;
        }
        if(Line==TEXT("End"))
        {
            if(bEnded||!FinishStep())return Reject(TEXT("Unexpected end marker"));
            bEnded=true;return true;
        }
        return true;
    };
    TArray<uint8> Buffer;Buffer.SetNumUninitialized(65536);
    for(int64 Offset=0;Offset<Size;)
    {
        if(Cancelled())return Fail(TEXT("Residual log loading cancelled."));
        const int64 Count=FMath::Min<int64>(Buffer.Num(),Size-Offset);File->Serialize(Buffer.GetData(),Count);
        if(File->IsError()||!Digest.Add(Buffer.GetData(),Count))return Fail(TEXT("Could not read and verify the complete residual log."));
        for(int64 I=0;I<Count;++I)
        {
            const uint8 C=Buffer[I];
            if(C=='\n'){if(!Consume())return Fail(Error);Line.Reset();}
            else
            {
                if((C<32&&C!='\t'&&C!='\r')||C>126||Line.Len()>=MaxLine)
                    return Fail(TEXT("Residual log contains unsupported text or a line exceeding 32 KiB."));
                Line.AppendChar(TCHAR(C));
            }
        }
        Offset+=Count;
    }
    if(!Line.IsEmpty()&&!Consume())return Fail(Error);
    if(Cancelled())return Fail(TEXT("Residual log loading cancelled."));
    if(!bEnded||H->Times.IsEmpty())return Fail(TEXT("Incomplete solver log: a final End marker is required."));
    if(Size!=IFileManager::Get().FileSize(*Path)||Before!=IFileManager::Get().GetTimeStamp(*Path))
        return Fail(TEXT("Residual log changed while loading. Choose a completed immutable log."));
    if(!Digest.Finish(H->SourceSHA256))return Fail(TEXT("Could not finish the residual log checksum."));
    H->PayloadSHA256=H->SourceSHA256;H->Id=TEXT("OpenFOAM-")+H->SourceSHA256;
    FDigest Meaning;FTCHARToUTF8 Hash(*H->SourceSHA256);
    if(!Meaning.Begin()||!Meaning.Add(Interpretation,FCStringAnsi::Strlen(Interpretation))||!Meaning.Add(Hash.Get(),Hash.Length())||!Meaning.Finish(H->MetadataSHA256))
        return Fail(TEXT("Could not identify the residual interpretation."));
    if(!ExpectedMetadataSHA256.IsEmpty()&&!ExpectedMetadataSHA256.Equals(H->MetadataSHA256,ESearchCase::IgnoreCase))
        return Fail(TEXT("Residual source or interpretation differs from the saved history. Locate an exact copy."));
    Result.History=MoveTemp(H);return Result;
}
