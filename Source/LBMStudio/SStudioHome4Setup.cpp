#include "SStudioHome4Setup.h"
#include "StudioHome4Readouts.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "StudioColor.h"
#include "SStudioHome4TankPresets.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Rendering/DrawElements.h"

namespace StudioHome4SetupWidgetLocal
{
FString Number(const TOptional<double>& N){return N?FString::Printf(TEXT("%.6g"),*N):TEXT("unknown");}
EStudioHome4UnitDisplay Display(const TSharedPtr<FStudioHome4Session>& Session)
{const auto M=Session->Owner();return M?M->UnitDisplay:EStudioHome4UnitDisplay::Lattice;}
FString Read(double N,EStudioHome4Quantity Q,const FStudioHome4Spec& S,const TSharedPtr<FStudioHome4Session>& Session)
{return StudioHome4Readouts::Value(N,Q,EStudioHome4UnitDisplay::Lattice,Display(Session),&S);}
class SRamp final:public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SRamp){}SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session)SLATE_END_ARGS()
    void Construct(const FArguments& A){Session=A._Session;SetToolTipText(FText::FromString(TEXT("Declared cubic ramp: U=U0(3s²−2s³), s=clamp(n/N,0,1), N=ramp_L·L/U0. Cyan: speed/U0. Amber: acceleration divided by peak 1.5U0/N. This is input math, not a solver output.")));}
    FVector2D ComputeDesiredSize(float)const override{return FVector2D(400,100);}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool)const override
    {
        FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return Layer;
        const auto Initial=StudioHome4Setup::Ramp(S,0);if(!Initial)return Layer;
        const double Duration=Initial->Duration;const auto D=StudioHome4Config::Derive(S);if(!D.Speed||*D.Speed<=0)return Layer;
        const FVector2D Size=G.GetLocalSize();TArray<FVector2D> U,A;
        for(int32 I=0;I<=64;++I)
        {
            const double T=FMath::Clamp(1.2*I/64,0.,1.);
            U.Add(FVector2D(12+(Size.X-24)*I/64,Size.Y-20-(Size.Y-34)*(Duration>0?3*T*T-2*T*T*T:1)));
            A.Add(FVector2D(12+(Size.X-24)*I/64,Size.Y-20-(Size.Y-34)*(Duration>0?4*T*(1-T):0)));
        }
        FSlateDrawElement::MakeLines(Out,Layer,G.ToPaintGeometry(),U,ESlateDrawEffect::None,StudioUI::Cyan,true,1.5f);
        FSlateDrawElement::MakeLines(Out,Layer,G.ToPaintGeometry(),A,ESlateDrawEffect::None,StudioUI::Amber,true,1.5f);
        FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(FVector2D(1),FSlateLayoutTransform(FVector2D(12,0))),TEXT("U/U₀ · a/a_max      n: 0 → 1.2N"),StudioUI::Font(9),ESlateDrawEffect::None,StudioUI::Muted);
        return Layer+2;
    }
private:TSharedPtr<FStudioHome4Session> Session;
};
}
void SStudioHome4Setup::Construct(const FArguments& A)
{
    Session=A._Session;Authoring=A._Authoring;Page=A._Page;check(Session.IsValid());SetCanTick(true);
    auto Rows=SNew(SVerticalBox);auto Buttons=SNew(SHorizontalBox);
    auto Action=[&](const TCHAR* Label,const TCHAR* Tag,TFunction<void()> Fn)
    {Buttons->AddSlot().AutoWidth().Padding(0,0,6,0)[SNew(SButton).Tag(FName(Tag)).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([Fn]{Fn();return FReply::Handled();})[StudioUI::Label(Label,10)]];};
    if(Page==TEXT("Lattice"))
    {
        Rows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Tank layout and authored MD counts"),14,StudioUI::Text,true)];
        Action(TEXT("Lay out tank from prepared body"),TEXT("Home4Setup.tank"),[this]{LayoutTank();});
        Action(TEXT("Derive authored level counts"),TEXT("Home4Setup.counts"),[this]{DeriveCounts();});
        Rows->AddSlot().AutoHeight().Padding(0,8)[Buttons];
        Rows->AddSlot().AutoHeight()[SNew(STextBlock).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true).Text(FText::FromString(TEXT("Pads use L: X spans body min−up·L to body max+down·L; Y uses ±side·L. Z spans waterline−depth·L to waterline+air·L. XYZ round up to root cells; body, CoG and regions translate by −tank minimum. Re-prepare to view the retained layout. Counts use full root plus disjoint nested patches; guard/halo storage remains a separately declared allocation.")))];
        Rows->AddSlot().AutoHeight().Padding(0,10)[SAssignNew(LevelRows,SVerticalBox)];
    }
    if(Page==TEXT("Boundaries & Zones"))
    {
        Rows->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(SStudioHome4TankPresets).Session(Session).Authoring(Authoring)];
        Rows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Reviewed zone geometry and named boundary faces"),14,StudioUI::Text,true)];
        Action(TEXT("Realize declared zone widths"),TEXT("Home4Setup.zones"),[this]{RealizeZones();});
        Rows->AddSlot().AutoHeight().Padding(0,8)[Buttons];
        Rows->AddSlot().AutoHeight()[SNew(STextBlock).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true).Text(FText::FromString(TEXT("Review this frontend mapping before using the action: Sponge and Beach Y are widths; X Beach is a start coordinate; Beach Gap is the minimum undamped Y gap. Each uses the declared Zone units. Side profiles increase toward each exterior wall. Floor friction occupies one root-cell layer. This declares frontend regions; CLI width conventions remain unverified.")))];
        Rows->AddSlot().AutoHeight().Padding(0,8)[SNew(STextBlock).Tag(TEXT("Home4Setup.widthDetails")).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(WidthDetails());}).ToolTipText_Lambda([this]{return FText::FromString(WidthTooltip());})];
        Rows->AddSlot().AutoHeight()[SAssignNew(FaceRows,SVerticalBox)];
        Rows->AddSlot().AutoHeight().Padding(0,8)[SNew(STextBlock).Tag(TEXT("Home4Setup.waveDetails")).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(WaveDetails());}).ToolTipText_Lambda([this]{return FText::FromString(WaveTooltip());})];
    }
    if(Page==TEXT("Run")||Page==TEXT("Boundaries & Zones"))
    {
        Rows->AddSlot().AutoHeight().Padding(0,10)[StudioUI::Label(TEXT("Declared cubic inlet ramp"),13,StudioUI::Text,true)];
        Rows->AddSlot().AutoHeight()[SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth()[StudioUI::Label(TEXT("Preview step [root steps]"),10,StudioUI::Muted)]+SHorizontalBox::Slot().FillWidth(1).Padding(8,0)[SNew(SEditableTextBox).Tag(TEXT("Home4Setup.step")).Style(&StudioUI::InputStyle()).Text(FText::FromString(StepText)).OnTextChanged_Lambda([this](const FText& T){StepText=T.ToString();double N;bStepValid=StudioColor::ParseNumber(StepText,N)&&N>=0&&N<=1.e12;if(bStepValid)Step=N;})]];
        Rows->AddSlot().AutoHeight()[SNew(StudioHome4SetupWidgetLocal::SRamp).Tag(TEXT("Home4Setup.rampCurve")).Session(Session)];
        Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4Setup.rampDetails")).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(RampDetails());}).ToolTipText_Lambda([this]{return FText::FromString(RampTooltip());})];
    }
    ChildSlot[SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(12)[Rows]];
    FStudioHome4Spec S;FString Error;if(Session->Build(S,Error,true)){RefreshLevels(S);RefreshFaces(S);ShownSHA=StudioHome4Authoring::Fingerprint(S);}
}
bool SStudioHome4Setup::Build(FStudioHome4Spec& S,FString& Error)
{Session->Refresh();if(!Session->Build(S,Error)){Session->Status=Error;return false;}return true;}
void SStudioHome4Setup::Tick(const FGeometry&,double,float)
{
    FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return;
    const FString SHA=StudioHome4Authoring::Fingerprint(S)+LexToString(int32(StudioHome4SetupWidgetLocal::Display(Session)));
    if(SHA!=ShownSHA){ShownSHA=SHA;RefreshLevels(S);RefreshFaces(S);}
}
void SStudioHome4Setup::LayoutTank()
{
    FStudioHome4Spec S,Next;FString Error;if(!Build(S,Error))return;
    if(!Authoring||Authoring->Draft()!=Session){Session->Status=TEXT("Tank layout requires the same retained authoring session as this page.");return;}
    Authoring->Poll();const auto P=Authoring->Preview();const auto M=Session->Owner();
    if(!P||!M||P->ProjectId!=M->Project.Id||P->CaseId!=M->Project.Draft.Id||P->RequestSHA256!=StudioHome4Authoring::Fingerprint(S))
    {Session->Status=TEXT("Prepare the current scoped body request before laying out the tank.");return;}
    if(!StudioHome4Setup::Tank(*P,Next,Error)||!Session->Replace(Next,Error)){Session->Status=Error;return;}
    Authoring->Cancel();RefreshLevels(Next);Session->Status=TEXT("Tank XYZ, body/CoG/regions and waterline retained together. Prepare refreshes the actual geometric preview; Apply saves.");
}
void SStudioHome4Setup::RealizeZones()
{
    FStudioHome4Spec S,Next;FString Error;if(!Build(S,Error))return;
    if(!StudioHome4Setup::Zones(S,Next,Error)||!Session->Replace(Next,Error)){Session->Status=Error;return;}
    if(Authoring)Authoring->Cancel();Session->Status=TEXT("Reviewed widths retained as real regions. Reserved frontend-width regions were reconciled; Prepare refreshes geometry and profiles; Apply saves.");
}
void SStudioHome4Setup::DeriveCounts()
{
    FStudioHome4Spec S,Next;FString Error;if(!Build(S,Error))return;
    if(!StudioHome4Setup::PatchCounts(S,Next,Error)||!Session->Replace(Next,Error)){Session->Status=Error;return;}
    RefreshLevels(Next);
    Session->Status=TEXT("Root-inclusive authored per-level node counts and scoped allocation counts retained. Apply saves; allocation components/halo storage remain explicit.");
}
void SStudioHome4Setup::RefreshLevels(const FStudioHome4Spec& S)
{
    using namespace StudioHome4SetupWidgetLocal;if(!LevelRows)return;LevelRows->ClearChildren();const auto D=StudioHome4Config::Derive(S);
    LevelRows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Level · scale/spacing · nodes · νH/νL · σ · M · g · ξ · τH/τL"),10,StudioUI::Muted)];
    for(const auto& L:D.Levels)
    {
        const FString Nodes=S.Multidomain.LevelCells.IsValidIndex(L.Depth)?LexToString(S.Multidomain.LevelCells[L.Depth]):TEXT("unknown");
        FStudioHome4Spec Local=S;if(Local.Units.DxMeters)Local.Units.DxMeters=*Local.Units.DxMeters/L.Scale;if(Local.Units.DtSeconds)Local.Units.DtSeconds=*Local.Units.DtSeconds/L.Scale;
        if(Local.Reference.LengthCells)Local.Reference.LengthCells=*Local.Reference.LengthCells*L.Scale;if(Local.Reference.TimeSteps)Local.Reference.TimeSteps=*Local.Reference.TimeSteps*L.Scale;
        FString Tooltip=TEXT("Per-level local lattice units: dx_d=dx0/k; dt_d=dt0/k; ν, σ, M, ξ scale by k; g scales by 1/k. Node counts are root/patch products, including retained coarse storage.");
        auto Quantity=[&](const TOptional<double>& V,EStudioHome4Quantity Q,const TCHAR* Name)
        {if(!V)return FString(TEXT("unknown"));Tooltip+=TEXT("\n")+FString(Name)+TEXT(": ")+StudioHome4Readouts::Tooltip(*V,Q,EStudioHome4UnitDisplay::Lattice,&Local);return Read(*V,Q,Local,Session);};
        const FString NuH=Quantity(L.NuHeavy,EStudioHome4Quantity::KinematicViscosity,TEXT("νH")),NuL=Quantity(L.NuLight,EStudioHome4Quantity::KinematicViscosity,TEXT("νL")),Sigma=Quantity(L.Sigma,EStudioHome4Quantity::SurfaceTension,TEXT("σ")),Mobility=Quantity(L.Mobility,EStudioHome4Quantity::Mobility,TEXT("M")),Gravity=Quantity(L.Gravity,EStudioHome4Quantity::Acceleration,TEXT("g")),Xi=Quantity(L.Xi,EStudioHome4Quantity::Length,TEXT("ξ"));
        FString Text=FString::Printf(TEXT("%d · 2^%d = %.0f / %.6g root cells · %s · %s/%s · %s · %s · %s · %s · %s/%s"),L.Depth,L.Depth,L.Scale,1/L.Scale,*Nodes,*NuH,*NuL,*Sigma,*Mobility,*Gravity,*Xi,*Number(L.TauHeavy),*Number(L.TauLight));
        if(L.NuHeavy&&S.Reference.LengthCells&&D.Speed&&*L.NuHeavy>0)Text+=FString::Printf(TEXT(" · effective ReH %.7g"),*D.Speed*L.Scale* *S.Reference.LengthCells/ *L.NuHeavy);
        LevelRows->AddSlot().AutoHeight().Padding(0,3)[SNew(STextBlock).Tag(FName(*FString::Printf(TEXT("Home4Setup.level.%d"),L.Depth))).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Text).AutoWrapText(true).Text(FText::FromString(Text)).ToolTipText(FText::FromString(Tooltip))];
    }
    LevelRows->AddSlot().AutoHeight().Padding(0,5)[SNew(STextBlock).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Amber).AutoWrapText(true).Text(FText::FromString(S.Multidomain.NoTauFloor?(*S.Multidomain.NoTauFloor?TEXT("Tau floor explicitly disabled; acoustic scaling preserves Re."):TEXT("Tau floor policy enabled. A declared floor changes level viscosity and effective Re; inspect each row.")):TEXT("Tau-floor policy is unspecified. Values show the declared frontend scaling; driver defaults remain unknown.")))];
    for(const auto& Z:S.Authoring.Zones)
    {
        FString Text=Z.Id+TEXT(" strength at full profile:");for(const auto& L:D.Levels)Text+=FString::Printf(TEXT(" L%d=%.6g"),L.Depth,FMath::Clamp(Z.Strength*FMath::Pow(2.,-Z.LevelExponent*L.Depth),0.,1.));
        LevelRows->AddSlot().AutoHeight()[StudioUI::Label(Text,9,StudioUI::Muted)];
    }
}
void SStudioHome4Setup::RefreshFaces(const FStudioHome4Spec& S)
{
    if(!FaceRows)return;FaceRows->ClearChildren();TArray<FStudioHome4BoundaryFace> Faces;FString Error;
    if(!StudioHome4Setup::BoundaryFaces(S,Faces,Error)){FaceRows->AddSlot().AutoHeight()[StudioUI::Label(Error,9,StudioUI::Muted)];return;}
    for(int32 I=0;I<Faces.Num();++I)
    {
        const auto& F=Faces[I];const FString Tag=FString::Printf(TEXT("Home4Setup.face.%d"),I);
        FaceRows->AddSlot().AutoHeight().Padding(0,2)[SNew(SButton).Tag(FName(*Tag)).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this,I]{SelectedFace=I;FStudioHome4Spec Current;FString E;if(Session->Build(Current,E,true))RefreshFaces(Current);return FReply::Handled();})[StudioUI::Label(F.Id+TEXT(" · ")+F.Constraint+(F.PhaseConstraint.IsEmpty()?FString():TEXT(" · ")+F.PhaseConstraint)+(F.Phase?FString::Printf(TEXT(" φ=%.5g"),*F.Phase):TEXT("")),9,I==SelectedFace?StudioUI::Cyan:StudioUI::Muted)]];
    }
    const int32 Axis=SelectedFace/2;const FString Key=Axis==0?TEXT("zones.periodicX"):Axis==1?TEXT("zones.periodicY"):TEXT("zones.periodicZ");
    auto Choices=SNew(SHorizontalBox);
    for(const auto& Choice:TArray<FString>{TEXT("true"),TEXT("false"),TEXT("")})
        Choices->AddSlot().AutoWidth().Padding(0,4,5,0)[SNew(SButton).Tag(FName(*(TEXT("Home4Setup.periodic.")+(Choice.IsEmpty()?TEXT("unset"):Choice)))).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this,Key,Choice]{SetFaceChoice(Key,Choice);return FReply::Handled();})[StudioUI::Label(Choice.IsEmpty()?TEXT("Periodic unspecified"):Choice==TEXT("true")?TEXT("Periodic pair on"):TEXT("Periodic pair off"),9)]];
    FaceRows->AddSlot().AutoHeight()[Choices];
    if(Axis>0)
    {
        auto Walls=SNew(SHorizontalBox);for(const auto& Wall:TArray<FString>{TEXT("no-slip"),TEXT("free-slip")})Walls->AddSlot().AutoWidth().Padding(0,4,5,0)[SNew(SButton).Tag(FName(*(TEXT("Home4Setup.wall.")+Wall))).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this,Wall]{SetFaceChoice(TEXT("authoring.boundaryWall"),Wall);return FReply::Handled();})[StudioUI::Label(TEXT("All wall faces: ")+Wall,9)]];FaceRows->AddSlot().AutoHeight()[Walls];
    }
    if(Axis==2)
    {
        auto Pin=SNew(SHorizontalBox);for(const auto& Choice:TArray<FString>{TEXT("true"),TEXT("false"),TEXT("")})Pin->AddSlot().AutoWidth().Padding(0,4,5,0)[SNew(SButton).Tag(FName(*(TEXT("Home4Setup.pin.")+(Choice.IsEmpty()?TEXT("unset"):Choice)))).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this,Choice]{SetFaceChoice(TEXT("zones.pinPhaseWalls"),Choice);return FReply::Handled();})[StudioUI::Label(Choice.IsEmpty()?TEXT("Phase pin unspecified"):Choice==TEXT("true")?TEXT("Top/bottom phase pin on"):TEXT("Top/bottom phase pin off"),9)]];FaceRows->AddSlot().AutoHeight()[Pin];
    }
    FaceRows->AddSlot().AutoHeight().Padding(0,5)[StudioUI::Label(TEXT("Pair and wall choices edit the declared global axis/wall contract. φ top/bottom and pierce selectors remain explicit inputs above."),9,StudioUI::Muted)];
}
void SStudioHome4Setup::SetFaceChoice(const FString& Key,const FString& Value)
{
    FStudioHome4Spec S;FString Error;if(!Build(S,Error))return;Session->Set(Key,Value);
    if(Authoring)Authoring->Cancel();if(Session->Build(S,Error)){RefreshFaces(S);Session->Status=TEXT("Named boundary request retained. Prepare refreshes the face overlay; Apply saves this change.");}else Session->Status=Error;
}
FString SStudioHome4Setup::RampDetails()const
{
    using namespace StudioHome4SetupWidgetLocal;if(!bStepValid)return TEXT("Enter a finite preview step from 0 to 1e12; no stale ramp point is shown.");
    FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return Error;const auto P=StudioHome4Setup::Ramp(S,Step);
    if(!P)return TEXT("Supply positive reference L/U and an explicit nonnegative ramp_L to evaluate the cubic ramp.");
    FString Text=TEXT("N = ")+Read(P->Duration,EStudioHome4Quantity::Time,S,Session)+TEXT(" · U(n) = ")+Read(P->Speed,EStudioHome4Quantity::Velocity,S,Session)+TEXT(" · dU/dn = ")+Read(P->Acceleration,EStudioHome4Quantity::Acceleration,S,Session);
    Text+=TEXT("\nRequested hull-frame ρ·dU/dn: heavy ")+(P->HeavyFrameForceDensity?Read(*P->HeavyFrameForceDensity,EStudioHome4Quantity::ForceDensity,S,Session):TEXT("unknown"))+TEXT(" · light ")+(P->LightFrameForceDensity?Read(*P->LightFrameForceDensity,EStudioHome4Quantity::ForceDensity,S,Session):TEXT("unknown"));
    if(!S.Run.NoFrameAcceleration)Text+=TEXT(" · explicitly choose no_frame_accel to resolve frame-force request");
    if(P->Duration==0)Text+=TEXT(" · zero-duration ramp is an instantaneous start; its impulse is not represented by a finite dU/dn");
    Text+=TEXT("\nInlet face X−: ")+S.Authoring.InletMode+TEXT(". Curve is the declared ramp function; active inlet application follows this selected contract.");return Text;
}
FString SStudioHome4Setup::WaveDetails()const
{
    using namespace StudioHome4SetupWidgetLocal;FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return Error;
    if(S.Authoring.WaveModel==TEXT("none"))return TEXT("Wave model: none. Select linear-gravity and declare λ, depth, amplitude, waterline and g for a geometric curve.");
    const auto P=StudioHome4Setup::WaveProperties(S,Error);if(!P)return Error;
    return TEXT("Linear finite-depth gravity wave: ω²=gk tanh(kh), k=2π/λ; η=waterline+a cos(kx−ωn).\nPeriod ")+Read(P->Period,EStudioHome4Quantity::Time,S,Session)+TEXT(" · phase speed ")+Read(P->PhaseSpeed,EStudioHome4Quantity::Velocity,S,Session)+TEXT(" · direction ")+S.Authoring.WaveAxis+TEXT(". The prepared 3D preview samples this declared surface. Supplied period/speed/slope must match within 1%.");
}
FString SStudioHome4Setup::WidthDetails()const
{
    using namespace StudioHome4SetupWidgetLocal;FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return Error;
    double Scale;if(!StudioHome4Setup::ZoneScale(S,Scale,Error))return Error;
    const auto D=StudioHome4Config::Derive(S);FString Text=TEXT("Declared zone units: ")+S.Authoring.ZoneUnits;
    if(D.WakeWavelength)Text+=TEXT(" · expected steady hull wake λ=2πFr²L: ")+Read(*D.WakeWavelength,EStudioHome4Quantity::Length,S,Session);
    else Text+=TEXT(" · expected steady wake wavelength unknown until L/U/g are supplied");
    auto Add=[&](const TCHAR* Label,const TOptional<double>& Value)
    {if(!Value)return;Text+=TEXT("\n")+FString(Label)+TEXT(": ")+Read(*Value*Scale,EStudioHome4Quantity::Length,S,Session);if(D.WakeWavelength&&*D.WakeWavelength>0)Text+=FString::Printf(TEXT(" · %.5g λ"),*Value*Scale/ *D.WakeWavelength);};
    Add(TEXT("Sponge width"),S.Zones.Sponge);Add(TEXT("Side beach width"),S.Zones.BeachY);Add(TEXT("Minimum clear gap"),S.Zones.BeachGap);
    if(S.Zones.XBeach&&S.Lattice.Extents)Add(TEXT("X beach width (tank X − start)"),TOptional<double>(S.Lattice.Extents->X/Scale-*S.Zones.XBeach));
    return Text;
}
FString SStudioHome4Setup::RampTooltip()const
{
    if(!bStepValid)return RampDetails();FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return Error;const auto P=StudioHome4Setup::Ramp(S,Step);if(!P)return RampDetails();
    auto Tip=[&](double V,EStudioHome4Quantity Q){return StudioHome4Readouts::Tooltip(V,Q,EStudioHome4UnitDisplay::Lattice,&S);};
    FString Text=TEXT("Ramp duration:\n")+Tip(P->Duration,EStudioHome4Quantity::Time)+TEXT("\nSpeed:\n")+Tip(P->Speed,EStudioHome4Quantity::Velocity)+TEXT("\nAcceleration:\n")+Tip(P->Acceleration,EStudioHome4Quantity::Acceleration);
    if(P->HeavyFrameForceDensity)Text+=TEXT("\nHeavy frame-force density:\n")+Tip(*P->HeavyFrameForceDensity,EStudioHome4Quantity::ForceDensity);
    if(P->LightFrameForceDensity)Text+=TEXT("\nLight frame-force density:\n")+Tip(*P->LightFrameForceDensity,EStudioHome4Quantity::ForceDensity);
    return Text;
}
FString SStudioHome4Setup::WaveTooltip()const
{
    FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return Error;const auto P=StudioHome4Setup::WaveProperties(S,Error);if(!P)return Error;
    return TEXT("Period:\n")+StudioHome4Readouts::Tooltip(P->Period,EStudioHome4Quantity::Time,EStudioHome4UnitDisplay::Lattice,&S)+TEXT("\nPhase speed:\n")+StudioHome4Readouts::Tooltip(P->PhaseSpeed,EStudioHome4Quantity::Velocity,EStudioHome4UnitDisplay::Lattice,&S)+TEXT("\nWavelength:\n")+StudioHome4Readouts::Tooltip(*S.Authoring.WaveLengthCells,EStudioHome4Quantity::Length,EStudioHome4UnitDisplay::Lattice,&S);
}
FString SStudioHome4Setup::WidthTooltip()const
{
    FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return Error;double Scale;if(!StudioHome4Setup::ZoneScale(S,Scale,Error))return Error;
    FString Text;auto Add=[&](const TCHAR* Name,const TOptional<double>& Value,double Multiplier)
    {if(Value)Text+=FString(Name)+TEXT(":\n")+StudioHome4Readouts::Tooltip(*Value*Multiplier,EStudioHome4Quantity::Length,EStudioHome4UnitDisplay::Lattice,&S)+TEXT("\n");};
    Add(TEXT("Expected wake wavelength"),StudioHome4Config::Derive(S).WakeWavelength,1);Add(TEXT("Sponge width"),S.Zones.Sponge,Scale);Add(TEXT("Side width"),S.Zones.BeachY,Scale);Add(TEXT("Minimum clear gap"),S.Zones.BeachGap,Scale);Add(TEXT("X beach start"),S.Zones.XBeach,Scale);return Text;
}
