#include "StudioMonitor.h"
#include "Dom/JsonObject.h"

namespace
{
bool CleanId(const FString& Id)
{
    if(Id.IsEmpty()||Id.Len()>256)return false;
    for(TCHAR C:Id)if(C<32||C==127)return false;
    return true;
}
bool Hash(const FString& Text)
{
    if(Text.Len()!=64)return false;
    for(TCHAR C:Text)if(!FChar::IsHexDigit(C))return false;
    return true;
}
int32 Lower(const TArray<double>& Values,double Value)
{
    int32 L=0,R=Values.Num();
    while(L<R){const int32 Mid=L+(R-L)/2;if(Values[Mid]<Value)L=Mid+1;else R=Mid;}
    return L;
}
void ExpandConstant(double& Minimum,double& Maximum)
{
    if(Minimum<Maximum)return;
    const double Pad=FMath::Max(1.e-12,FMath::Abs(Minimum)*.05);
    Minimum-=Pad;Maximum+=Pad;
}
}

TSharedRef<FJsonObject> StudioMonitor::ToJSON(const FStudioMonitorSettings& S)
{
    auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("historyId"),S.HistoryId);
    O->SetStringField(TEXT("metadataSHA256"),S.MetadataSHA256);
    TArray<TSharedPtr<FJsonValue>> Series;for(const auto& Id:S.Series)Series.Add(MakeShared<FJsonValueString>(Id));
    O->SetArrayField(TEXT("series"),Series);O->SetBoolField(TEXT("logY"),S.bLogY);
    O->SetBoolField(TEXT("manualTime"),S.bManualTime);O->SetNumberField(TEXT("timeMinimum"),S.TimeMinimum);
    O->SetNumberField(TEXT("timeMaximum"),S.TimeMaximum);return O;
}
bool StudioMonitor::Validate(const FStudioMonitorSettings& S,FString& Error)
{
    Error=TEXT("Invalid monitor source, series or time range.");
    if(!FMath::IsFinite(S.TimeMinimum)||!FMath::IsFinite(S.TimeMaximum)||
        FMath::Abs(S.TimeMinimum)>1.e100||FMath::Abs(S.TimeMaximum)>1.e100||S.TimeMinimum>=S.TimeMaximum||S.Series.Num()>16)return false;
    if(S.HistoryId.IsEmpty())
    {
        if(!S.MetadataSHA256.IsEmpty()||!S.Series.IsEmpty()||S.bManualTime||S.bLogY)return false;
    }
    else if(!CleanId(S.HistoryId)||!Hash(S.MetadataSHA256))return false;
    TSet<FString> Seen;
    for(const auto& Id:S.Series){if(!CleanId(Id)||Id.Len()>128||Seen.Contains(Id))return false;Seen.Add(Id);}
    Error.Empty();return true;
}
bool StudioMonitor::FromJSON(const TSharedPtr<FJsonObject>& O,FStudioMonitorSettings& Out,FString& Error)
{
    Error=TEXT("Invalid monitor settings.");FStudioMonitorSettings S;
    const TArray<TSharedPtr<FJsonValue>>* Series=nullptr;
    if(!O||!O->TryGetStringField(TEXT("historyId"),S.HistoryId)||!O->TryGetStringField(TEXT("metadataSHA256"),S.MetadataSHA256)||
        !O->TryGetArrayField(TEXT("series"),Series)||Series->Num()>16||!O->TryGetBoolField(TEXT("logY"),S.bLogY)||
        !O->TryGetBoolField(TEXT("manualTime"),S.bManualTime)||!O->TryGetNumberField(TEXT("timeMinimum"),S.TimeMinimum)||
        !O->TryGetNumberField(TEXT("timeMaximum"),S.TimeMaximum))return false;
    for(const auto& Value:*Series){FString Id;if(!Value->TryGetString(Id))return false;S.Series.Add(Id);}
    if(!Validate(S,Error))return false;
    S.MetadataSHA256.ToLowerInline();Out=MoveTemp(S);return true;
}
bool StudioMonitor::ValidateSource(const FStudioMonitorSettings& S,const FStudioHistory& H,FString& Error)
{
    if(!Validate(S,Error))return false;
    Error=TEXT("Monitor settings do not match this verified history.");
    if(S.HistoryId!=H.Id||!S.MetadataSHA256.Equals(H.MetadataSHA256,ESearchCase::IgnoreCase)||H.Times.IsEmpty())return false;
    if(H.bResiduals)
    {
        if(H.TimeSourceLines.Num()!=H.Times.Num()||H.TimeSourceLines.ContainsByPredicate([](int32 Line){return Line<=0;}))
        {Error=TEXT("Residual history is missing original time-line references.");return false;}
    }
    FString Unit;
    for(const auto& Id:S.Series)
    {
        const auto* C=H.FindColumn(Id);
        if(!C||C->Values.Num()!=H.Times.Num())return false;
        if(H.bResiduals&&(C->SourceLines.Num()!=H.Times.Num()||C->SourceLines.ContainsByPredicate([](int32 Line){return Line<=0;})))
        {Error=TEXT("Residual series is missing original source-line references.");return false;}
        if(!Unit.IsEmpty()&&Unit!=C->Unit){Error=TEXT("Choose series with the same unit for one value axis.");return false;}
        Unit=C->Unit;
    }
    if(S.bManualTime&&(S.TimeMinimum<H.Times[0]||S.TimeMaximum>H.Times.Last()))
    {Error=TEXT("The time window must stay within the original history.");return false;}
    Error.Empty();return true;
}
FStudioMonitorSettings StudioMonitor::Defaults(const FStudioHistory& H)
{
    FStudioMonitorSettings S;S.HistoryId=H.Id;S.MetadataSHA256=H.MetadataSHA256;
    // Prefer explicitly supplied coefficient columns, never calculate absent coefficients.
    const auto* CL=H.FindColumn(TEXT("CL"));const auto* CD=H.FindColumn(TEXT("CD"));
    if(H.bResiduals)
    {
        S.bLogY=true;
        for(const auto& C:H.Columns)if(C.Id.EndsWith(TEXT(".InitialFirst"))&&S.Series.Num()<16)S.Series.Add(C.Id);
    }
    else if(CL&&CD&&CL->Unit==CD->Unit)S.Series={CL->Id,CD->Id};
    else if(!H.Columns.IsEmpty())S.Series={H.Columns[0].Id};
    return S;
}
int32 StudioMonitor::NearestSample(const TArray<double>& Times,double Time)
{
    if(Times.IsEmpty()||!FMath::IsFinite(Time))return INDEX_NONE;
    const int32 Right=Lower(Times,Time);
    if(Right==0)return 0;if(Right==Times.Num())return Times.Num()-1;
    return Time-Times[Right-1]<=Times[Right]-Time?Right-1:Right;
}
bool StudioMonitor::SetTimeWindow(const FStudioHistory& H,double Minimum,double Maximum,FStudioMonitorSettings& S,FString& Error)
{
    auto Candidate=S;Candidate.bManualTime=true;Candidate.TimeMinimum=Minimum;Candidate.TimeMaximum=Maximum;
    if(!ValidateSource(Candidate,H,Error))return false;S=MoveTemp(Candidate);return true;
}
FStudioMonitorPlot StudioMonitor::BuildPlot(const FStudioHistory& H,const FStudioMonitorSettings& S,int32 Width)
{
    FStudioMonitorPlot P;P.bLogY=S.bLogY;
    if(!ValidateSource(S,H,P.Error))return P;
    P.TimeMinimum=S.bManualTime?S.TimeMinimum:H.Times[0];P.TimeMaximum=S.bManualTime?S.TimeMaximum:H.Times.Last();
    if(!FMath::IsFinite(P.TimeMinimum)||!FMath::IsFinite(P.TimeMaximum)||
        FMath::Abs(P.TimeMinimum)>1.e100||FMath::Abs(P.TimeMaximum)>1.e100)
    {P.Error=TEXT("Source time exceeds the supported chart range.");return P;}
    ExpandConstant(P.TimeMinimum,P.TimeMaximum);
    P.FirstSample=Lower(H.Times,P.TimeMinimum);
    const int32 End=Lower(H.Times,P.TimeMaximum);
    P.LastSample=End<H.Times.Num()&&H.Times[End]==P.TimeMaximum?End:End-1;
    if(S.Series.IsEmpty()){P.Error=TEXT("Select a series to plot.");return P;}
    if(P.FirstSample>P.LastSample){P.Error=TEXT("No original samples in this time window.");return P;}
    const int32 Buckets=FMath::Clamp(Width,1,4096);
    bool bAny=false;
    double Lo=0,Hi=0;
    for(const auto& Id:S.Series)
    {
        const auto& C=*H.FindColumn(Id);FStudioMonitorTrace T;T.Id=Id;T.Label=C.Label;T.Unit=C.Unit;P.Unit=C.Unit;
        int32 Bucket=INDEX_NONE,Segment=0,LastEmittedSegment=INDEX_NONE;
        bool bGap=false;
        FIntPoint First(INDEX_NONE,0),Last=First,Minimum=First,Maximum=First;
        auto Flush=[&]
        {
            if(First.X==INDEX_NONE)return;
            TArray<FIntPoint> Points={First,Minimum,Maximum,Last};Points.Sort([](auto A,auto B){return A.X<B.X;});
            for(auto Point:Points)
            {
                if(!T.Samples.IsEmpty()&&T.Samples.Last()==Point.X)continue;
                if(LastEmittedSegment!=INDEX_NONE&&LastEmittedSegment!=Point.Y)T.Samples.Add(INDEX_NONE);
                T.Samples.Add(Point.X);LastEmittedSegment=Point.Y;
            }
            First=Last=Minimum=Maximum=FIntPoint(INDEX_NONE,0);
        };
        for(int32 I=P.FirstSample;I<=P.LastSample;++I)
        {
            const double Value=C.Values[I];
            if(S.bLogY&&Value<=0)
            {
                ++T.OmittedNonPositive;bGap=true;
                continue;
            }
            if(bGap){++Segment;bGap=false;}
            const double AxisValue=S.bLogY?FMath::LogX(10.,Value):Value;
            if(!bAny){Lo=Hi=AxisValue;bAny=true;}else{Lo=FMath::Min(Lo,AxisValue);Hi=FMath::Max(Hi,AxisValue);}
            const int32 B=FMath::Min(Buckets-1,int32((H.Times[I]-P.TimeMinimum)/(P.TimeMaximum-P.TimeMinimum)*Buckets));
            if(B!=Bucket){Flush();Bucket=B;}
            const FIntPoint Point(I,Segment);
            if(First.X==INDEX_NONE){First=Minimum=Maximum=Point;}
            else {if(Value<C.Values[Minimum.X])Minimum=Point;if(Value>C.Values[Maximum.X])Maximum=Point;}
            Last=Point;
        }
        Flush();P.Traces.Add(MoveTemp(T));
    }
    if(!bAny){P.Error=TEXT("No positive samples for a logarithmic axis.");return P;}
    ExpandConstant(Lo,Hi);const double Pad=(Hi-Lo)*.05;
    P.ValueMinimum=Lo-Pad;P.ValueMaximum=Hi+Pad;
    if(!FMath::IsFinite(P.ValueMinimum)||!FMath::IsFinite(P.ValueMaximum)||!FMath::IsFinite(Hi-Lo)||P.ValueMinimum>=P.ValueMaximum)
        P.Error=TEXT("Value range exceeds the supported chart scale. Select another series or logarithmic scale.");
    return P;
}

TSharedRef<FJsonObject> StudioResidualSettings::ToJSON(const FStudioResidualSettings& S)
{
    auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("path"),S.Path);
    O->SetObjectField(TEXT("chart"),StudioMonitor::ToJSON(S.Chart));return O;
}
bool StudioResidualSettings::Validate(const FStudioResidualSettings& S,FString& Error)
{
    if(!StudioMonitor::Validate(S.Chart,Error))return false;
    Error=TEXT("Invalid residual log path or source identity.");
    if(S.Path.IsEmpty())
    {if(!S.Chart.HistoryId.IsEmpty())return false;}
    else
    {
        if(S.Path.TrimStartAndEnd().IsEmpty()||S.Path.Len()>4096||S.Path.Contains(TEXT("://"))||
            !S.Chart.HistoryId.StartsWith(TEXT("OpenFOAM-"),ESearchCase::CaseSensitive)||
            !Hash(S.Chart.HistoryId.Mid(9)))return false;
        for(TCHAR C:S.Path)if(C<32||C==127)return false;
    }
    Error.Empty();return true;
}
bool StudioResidualSettings::FromJSON(const TSharedPtr<FJsonObject>& O,FStudioResidualSettings& Out,FString& Error)
{
    FStudioResidualSettings S;const TSharedPtr<FJsonObject>* Chart=nullptr;
    Error=TEXT("Invalid residual history settings.");
    if(!O||!O->TryGetStringField(TEXT("path"),S.Path)||!O->TryGetObjectField(TEXT("chart"),Chart)||
        !StudioMonitor::FromJSON(*Chart,S.Chart,Error)||!Validate(S,Error))return false;
    Out=MoveTemp(S);return true;
}
