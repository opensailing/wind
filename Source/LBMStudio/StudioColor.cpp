#include "StudioColor.h"
#include <locale>
#include <sstream>

bool FStudioScalarStyle::operator==(const FStudioScalarStyle& O) const
{
    return Dataset==O.Dataset&&Field==O.Field&&Palette==O.Palette&&bManualRange==O.bManualRange&&
        Minimum==O.Minimum&&Maximum==O.Maximum&&LowColor==O.LowColor&&MiddleColor==O.MiddleColor&&HighColor==O.HighColor;
}
bool StudioColor::IsValid(const FStudioScalarStyle& S)
{
    auto Id=[](const FString& V)
    {
        if(V.IsEmpty()||V.Len()>128)return false;
        for(TCHAR C:V)if(C<32)return false;
        return true;
    };
    auto Color=[](const FLinearColor& C){return FMath::IsFinite(C.R)&&FMath::IsFinite(C.G)&&FMath::IsFinite(C.B)&&
        C.R>=0&&C.R<=1&&C.G>=0&&C.G<=1&&C.B>=0&&C.B<=1&&C.A==1;};
    return Id(S.Dataset)&&Id(S.Field)&&S.Palette>=0&&S.Palette<=3&&Color(S.LowColor)&&Color(S.MiddleColor)&&Color(S.HighColor)&&
        FMath::IsFinite(S.Minimum)&&FMath::IsFinite(S.Maximum)&&S.Minimum<=S.Maximum&&(!S.bManualRange||S.Minimum<S.Maximum)&&
        FMath::IsFinite(S.Maximum-S.Minimum);
}
bool StudioColor::IsValid(const TArray<FStudioScalarStyle>& Styles)
{
    if(Styles.Num()>128)return false;
    for(int32 I=0;I<Styles.Num();++I)
    {
        if(!IsValid(Styles[I]))return false;
        for(int32 J=0;J<I;++J)if(Styles[I].Dataset==Styles[J].Dataset&&Styles[I].Field==Styles[J].Field)return false;
    }
    return true;
}
FStudioColorMapping StudioColor::Resolve(const FString& Dataset,const FStudioScalarDescriptor& F,const TArray<FStudioScalarStyle>& Styles)
{
    FStudioColorMapping M;M.Minimum=F.Minimum;M.Maximum=F.Maximum;
    if(const auto* S=Styles.FindByPredicate([&](const auto& V){return V.Dataset==Dataset&&V.Field==F.Id;}))
    {
        M.Palette=S->Palette;M.bManualRange=S->bManualRange;
        M.LowColor=S->LowColor;M.MiddleColor=S->MiddleColor;M.HighColor=S->HighColor;
        if(S->bManualRange){M.Minimum=S->Minimum;M.Maximum=S->Maximum;}
    }
    return M;
}
FString StudioColor::PaletteName(int32 Palette)
{ return Palette==1?TEXT("Blue–white–red"):Palette==2?TEXT("Grayscale"):Palette==3?TEXT("Custom colors"):TEXT("Spectrum"); }
bool StudioColor::ParseHexColor(const FString& Text,FLinearColor& Out)
{
    FString S=Text.TrimStartAndEnd();S.RemoveFromStart(TEXT("#"));
    if(S.Len()!=6)return false;
    for(TCHAR C:S)if(!FChar::IsHexDigit(C))return false;
    Out=FLinearColor::FromSRGBColor(FColor::FromHex(S));Out.A=1;return true;
}
bool StudioColor::ParseNumber(const FString& Text,double& Out)
{
    const FString S=Text.TrimStartAndEnd();
    if(S.IsEmpty()||S.Len()>128)return false;
    int32 I=0,Digits=0;bool NonzeroMantissa=false;
    if(S[I]=='+'||S[I]=='-')++I;
    auto Digit=[&]{return I<S.Len()&&S[I]>='0'&&S[I]<='9';};
    while(Digit()){NonzeroMantissa|=S[I]!='0';++I;++Digits;}
    if(I<S.Len()&&S[I]=='.'){++I;while(Digit()){NonzeroMantissa|=S[I]!='0';++I;++Digits;}}
    if(!Digits)return false;
    if(I<S.Len()&&(S[I]=='e'||S[I]=='E'))
    {
        ++I;if(I<S.Len()&&(S[I]=='+'||S[I]=='-'))++I;
        const int32 Start=I;while(Digit())++I;
        if(I==Start)return false;
    }
    if(I!=S.Len())return false;
    // UI text explicitly uses a decimal point, independently of process locale.
    // Parsing runs only on Apply, never in the rendering path.
    const FTCHARToUTF8 Bytes(*S);
    std::istringstream Input(std::string(Bytes.Get(),Bytes.Length()));Input.imbue(std::locale::classic());
    double Value=0;Input>>Value;
    if(Input.fail()||!Input.eof()||!FMath::IsFinite(Value)||(Value==0&&NonzeroMantissa))return false;
    Out=Value;return true;
}
FLinearColor StudioColor::Map(double Value,const FStudioColorMapping& M)
{
    const double Span=M.Maximum-M.Minimum;
    const double T=Span>0?FMath::Clamp((Value-M.Minimum)/Span,0.,1.):.5;
    if(M.Palette==3)return T<=.5?FMath::Lerp(M.LowColor,M.MiddleColor,T*2):FMath::Lerp(M.MiddleColor,M.HighColor,(T-.5)*2);
    if(M.Palette==2)return FLinearColor(T,T,T,1);
    if(M.Palette==1)
    {
        const FLinearColor Low(.03,.15,.6),Middle(.94,.94,.94),High(.7,.035,.025);
        return T<=.5?FMath::Lerp(Low,Middle,T*2):FMath::Lerp(Middle,High,(T-.5)*2);
    }
    const FLinearColor Colors[]={FLinearColor(.025,.02,.35),FLinearColor(.015,.14,.9),FLinearColor(0,.7,.95),
        FLinearColor(.02,.65,.25),FLinearColor(.95,.85,.015),FLinearColor(1,.25,.015),FLinearColor(.8,.015,.008)};
    const double X=T*6;const int32 I=FMath::Min(5,FMath::FloorToInt(X));
    return FMath::Lerp(Colors[I],Colors[I+1],X-I);
}
