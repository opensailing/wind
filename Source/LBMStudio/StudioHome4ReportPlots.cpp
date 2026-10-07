#include "StudioHome4ReportPlots.h"
#include "StudioHome4Reports.h"

namespace StudioHome4ReportPlotsPrivate
{
    FString Number(double V) { return FString::Printf(TEXT("%.17g"), V); }
    FString XML(FString V)
    { return V.Replace(TEXT("&"),TEXT("&amp;")).Replace(TEXT("<"),TEXT("&lt;")).Replace(TEXT(">"),TEXT("&gt;")).Replace(TEXT("\""),TEXT("&quot;")).Replace(TEXT("'"),TEXT("&apos;")); }
    double Fraction(double V, double Low, double High)
    {
        if (Low == High) return .5;
        const double Span = High - Low;
        // Halving first also scales opposite-sign values near DBL_MAX safely.
        return FMath::Clamp(FMath::IsFinite(Span) ? (V - Low) / Span : (V*.5 - Low*.5)/(High*.5-Low*.5), 0., 1.);
    }
    bool Prepare(FStudioHome4ReportPlot& P, FString& Error)
    {
        if (P.X.IsEmpty() || P.X.Num() > 100000 || P.Channels.IsEmpty() || P.Channels.Num() != P.Legends.Num())
        { Error=TEXT("Figure requires bounded, nonempty identified original samples."); return false; }
        P.XMin=P.X[0];P.XMax=P.X.Last();P.YMin=TNumericLimits<double>::Max();P.YMax=-TNumericLimits<double>::Max();
        for (int32 I=0;I<P.X.Num();++I) if (!FMath::IsFinite(P.X[I]) || (I && P.X[I]<=P.X[I-1]))
        { Error=TEXT("Figure abscissae must be finite and strictly increasing."); return false; }
        for (const auto& C:P.Channels)
        {
            if (C.Num()!=P.X.Num()) { Error=TEXT("Figure original arrays must align exactly."); return false; }
            for (double V:C)
            { if (!FMath::IsFinite(V)) { Error=TEXT("Figure measurements must be finite."); return false; } P.YMin=FMath::Min(P.YMin,V);P.YMax=FMath::Max(P.YMax,V); }
        }
        const int32 Count=FMath::Min(P.X.Num(),StudioHome4ReportPlots::PreviewLimit);
        for (int32 K=0;K<Count;++K) P.PreviewIndices.Add(Count==1?0:int32(int64(K)*(P.X.Num()-1)/(Count-1)));
        Error.Empty(); return true;
    }
}
FVector2D FStudioHome4ReportPlot::Normalized(int32 Channel,int32 Index) const
{
    using namespace StudioHome4ReportPlotsPrivate;
    return FVector2D(Fraction(X[Index],XMin,XMax),Fraction(Channels[Channel][Index],YMin,YMax));
}
bool StudioHome4ReportPlots::Reference(const FStudioHome4ReferenceSeries& S,FStudioHome4ReportPlot& Out,FString& Error)
{
    FStudioHome4ReportPlot P;P.Title=S.Name;P.XLabel=S.AbscissaName+TEXT(" [")+S.AbscissaUnit+TEXT("]");P.YLabel=S.Name+TEXT(" [")+S.Unit+TEXT("]");
    P.X=S.Abscissae;P.Channels={S.Actual,S.Reference};P.Legends={TEXT("Actual"),TEXT("Reference")};
    if (!StudioHome4ReportPlotsPrivate::Prepare(P,Error)) return false;Out=MoveTemp(P);return true;
}
bool StudioHome4ReportPlots::Convergence(const FStudioHome4ReferenceEvidence& E,FStudioHome4ReportPlot& Out,FString& Error)
{
    if(E.OrderRuns.Num()!=3 || E.OrderMetric.IsEmpty() || E.OrderUnit.IsEmpty()) { Error=TEXT("No identified three-run scalar convergence evidence supplied.");return false; }
    FStudioHome4ReportPlot P;P.Title=TEXT("Three-run scalar convergence");P.XLabel=TEXT("Supplied refinement factor [1]");P.YLabel=E.OrderMetric+TEXT(" [")+E.OrderUnit+TEXT("]");P.Legends={TEXT("Original scalar")};P.Channels.Add({});
    TSet<FGuid> Ids;for(const auto& R:E.OrderRuns)
    { if(!R.RunId.IsValid() || Ids.Contains(R.RunId) || R.Refinement<=0) {Error=TEXT("Convergence requires unique original run IDs and positive refinement factors.");return false;}Ids.Add(R.RunId);P.X.Add(R.Refinement);P.Channels[0].Add(R.Value); }
    if(!StudioHome4ReportPlotsPrivate::Prepare(P,Error))return false;Out=MoveTemp(P);return true;
}
const FStudioHome4ReportPlot* FStudioHome4ReferencePlotCache::Get(const TSharedPtr<const FStudioHome4ReferenceEvidence>& E,int32 Series,bool Convergence)const
{
    if(!bInitialized||Source!=E||SelectedSeries!=Series||bOrder!=Convergence)
    {
        bInitialized=true;Source=E;SelectedSeries=Series;bOrder=Convergence;Plot={};bReady=false;++Preparations;
        Failure=TEXT("not_evaluated · import actual and reference measurements");
        if(E)bReady=Convergence?StudioHome4ReportPlots::Convergence(*E,Plot,Failure):
            E->Series.IsValidIndex(Series)&&StudioHome4ReportPlots::Reference(E->Series[Series],Plot,Failure);
    }
    return bReady?&Plot:nullptr;
}
FString StudioHome4ReportPlots::SelectionDescription()
{ return TEXT("Display preview only: M=min(N,2000), source sample index floor(k*(N-1)/(M-1)), k=0..M-1; single sample uses index 0. Both endpoints retained. Straight segments join selected originals; no measurements are interpolated. Complete original arrays remain in CSV/JSON."); }
FString StudioHome4ReportPlots::SVG(const FStudioHome4ReportPlot& P,const FString& Identity)
{
    using namespace StudioHome4ReportPlotsPrivate;
    FString Out=TEXT("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"900\" height=\"460\" viewBox=\"0 0 900 460\" role=\"img\"><title>")+XML(P.Title)+TEXT("</title><desc>")+XML(Identity+TEXT(". ")+SelectionDescription())+TEXT("</desc><rect width=\"900\" height=\"460\" fill=\"white\"/><g font-family=\"sans-serif\" font-size=\"13\" fill=\"#243447\">");
    auto Text=[&](double X,double Y,const FString& V){Out+=FString::Printf(TEXT("<text x=\"%.3f\" y=\"%.3f\">"),X,Y)+XML(V)+TEXT("</text>");};
    Text(80,24,P.Title);Text(80,440,P.XLabel);Text(80,48,P.YLabel);Text(8,80,Number(P.YMax));Text(8,390,Number(P.YMin));Text(80,414,Number(P.XMin));Text(740,414,Number(P.XMax));
    Out+=TEXT("<path d=\"M80 65V390H870\" fill=\"none\" stroke=\"#8795a5\"/>");
    for(int32 C=0;C<P.Channels.Num();++C)
    {
        const FString Color=C?TEXT("#b45309"):TEXT("#007a9f");Text(500+C*155,24,P.Legends[C]);Out+=TEXT("<polyline fill=\"none\" stroke=\"")+Color+TEXT("\" stroke-width=\"2\" points=\"");
        for(int32 I:P.PreviewIndices){const auto V=P.Normalized(C,I);Out+=FString::Printf(TEXT("%.6f,%.6f "),80+790*V.X,390-325*V.Y);}Out+=TEXT("\"/>");
        if(P.X.Num()<=3)for(int32 I:P.PreviewIndices){const auto V=P.Normalized(C,I);Out+=FString::Printf(TEXT("<circle cx=\"%.6f\" cy=\"%.6f\" r=\"3\" fill=\"%s\"/>"),80+790*V.X,390-325*V.Y,*Color);}
    }
    return Out+TEXT("</g></svg>\n");
}
FString StudioHome4ReportPlots::TikZ(const FStudioHome4ReportPlot& P)
{
    using namespace StudioHome4ReportPlotsPrivate;using StudioHome4Reports::EscapeLaTeX;
    FString Out=TEXT("% ")+SelectionDescription()+TEXT("\n\\begin{tikzpicture}[x=10cm,y=4.4cm,font=\\scriptsize]\n\\draw[gray] (0,1) -- (0,0) -- (1,0);\n");
    auto Node=[&](const TCHAR* Where,double X,double Y,const FString& V){Out+=FString::Printf(TEXT("\\node[%s] at (%.6f,%.6f) {"),Where,X,Y)+EscapeLaTeX(V)+TEXT("};\n");};
    Node(TEXT("anchor=west"),0,1.22,P.Title);Node(TEXT("anchor=west"),0,1.12,P.YLabel);Node(TEXT("anchor=north"),.5,-.14,P.XLabel);Node(TEXT("anchor=east"),-.01,1,Number(P.YMax));Node(TEXT("anchor=east"),-.01,0,Number(P.YMin));Node(TEXT("anchor=north west"),0,-.01,Number(P.XMin));Node(TEXT("anchor=north east"),1,-.01,Number(P.XMax));
    for(int32 C=0;C<P.Channels.Num();++C)
    {
        const FString Color=C?TEXT("orange!80!black"):TEXT("cyan!55!black");Node(TEXT("anchor=west"),.60+C*.20,1.22,P.Legends[C]);
        if(P.X.Num()>1){Out+=TEXT("\\draw[")+Color+TEXT(",line width=.6pt] ");bool First=true;for(int32 I:P.PreviewIndices){const auto V=P.Normalized(C,I);Out+=First?TEXT(""):TEXT(" -- ");Out+=FString::Printf(TEXT("(%.8f,%.8f)"),V.X,V.Y);First=false;}Out+=TEXT(";\n");}
        if(P.X.Num()<=3)for(int32 I:P.PreviewIndices){const auto V=P.Normalized(C,I);Out+=FString::Printf(TEXT("\\fill[%s] (%.8f,%.8f) circle[radius=1.5pt];\n"),*Color,V.X,V.Y);}
    }
    return Out+TEXT("\\end{tikzpicture}\n");
}
FString StudioHome4ReportPlots::TeXPreamble()
{ return TEXT("% Compile with XeLaTeX, LuaLaTeX or Tectonic (UTF-8/fontspec).\n\\documentclass{article}\n\\usepackage[margin=2cm]{geometry}\n\\usepackage{fontspec}\n\\usepackage{longtable}\n\\usepackage{tikz}\n\\begin{document}\n"); }
FString StudioHome4ReportPlots::TeXEnd() {return TEXT("\\end{document}\n");}
FString StudioHome4ReportPlots::CSV(const FStudioHome4ReportPlot& P,const TArray<FGuid>& RunIds,const TArray<FStudioHome4ScalarRun>* ScalarRuns)
{
    using namespace StudioHome4ReportPlotsPrivate;
    const bool Metadata=ScalarRuns&&ScalarRuns->Num()==P.X.Num()&&!RunIds.IsEmpty();
    FString Out=RunIds.IsEmpty()?TEXT("source_sample,x,actual,reference"):TEXT("source_sample,run_id,refinement,value");
    if(Metadata)Out+=TEXT(",window_start,window_end,abscissa_unit,epoch,extraction_method,original_source,original_source_sha256");Out+=TEXT("\n");
    auto Quoted=[](const FString& V){return TEXT("\"")+V.Replace(TEXT("\""),TEXT("\"\""))+TEXT("\"");};
    for(int32 I=0;I<P.X.Num();++I)
    {
        Out+=LexToString(I)+TEXT(",");if(!RunIds.IsEmpty())Out+=RunIds[I].ToString()+TEXT(",");Out+=Number(P.X[I]);for(const auto& C:P.Channels)Out+=TEXT(",")+Number(C[I]);
        if(Metadata)
        {
            const auto& E=(*ScalarRuns)[I].Extraction;
            Out+=TEXT(",")+(E.WindowStart?Number(*E.WindowStart):FString())+TEXT(",")+(E.WindowEnd?Number(*E.WindowEnd):FString())+
                TEXT(",")+Quoted(E.AbscissaUnit)+TEXT(",")+Quoted(E.Epoch)+TEXT(",")+Quoted(E.Method)+TEXT(",")+Quoted(E.Source)+TEXT(",")+Quoted(E.SourceSHA256);
        }
        Out+=TEXT("\n");
    }
    return Out;
}
