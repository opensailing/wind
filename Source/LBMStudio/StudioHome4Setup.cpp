#include "StudioHome4Setup.h"
#include <cmath>

namespace StudioHome4SetupLocal
{
constexpr int64 MaxNodes=1000000000000LL;
bool Positive(const TOptional<double>& N){return N&&FMath::IsFinite(*N)&&*N>0;}
bool Product(const FIntVector& N,int64& Out)
{
    if(N.GetMin()<1||N.GetMax()>1048576)return false;
    int64 Count=1;
    for(int32 I=0;I<3;++I){if(Count>MaxNodes/N[I])return false;Count*=N[I];}
    Out=Count;return true;
}
FBox PatchBounds(const FStudioHome4AuthoredPatch& P)
{return FBox(P.Origin,P.Origin+FVector(P.Extents)*FMath::Pow(2.,-P.Level));}
bool Contains(const FBox& Outer,const FBox& Inner)
{return Outer.IsInsideOrOn(Inner.Min)&&Outer.IsInsideOrOn(Inner.Max);}
bool Overlaps(const FBox& A,const FBox& B)
{return A.Min.X<B.Max.X&&B.Min.X<A.Max.X&&A.Min.Y<B.Max.Y&&B.Min.Y<A.Max.Y&&A.Min.Z<B.Max.Z&&B.Min.Z<A.Max.Z;}
}
bool StudioHome4Setup::ZoneScale(const FStudioHome4Spec& S,double& Out,FString& Error)
{
    double Scale=0;
    if(S.Authoring.ZoneUnits==TEXT("root-cells"))Scale=1;
    else if(S.Authoring.ZoneUnits==TEXT("body-lengths")&&StudioHome4SetupLocal::Positive(S.Reference.LengthCells))Scale=*S.Reference.LengthCells;
    else if(S.Authoring.ZoneUnits==TEXT("physical-metres")&&StudioHome4SetupLocal::Positive(S.Units.DxMeters))Scale=1/ *S.Units.DxMeters;
    if(!FMath::IsFinite(Scale)||Scale<=0){Error=TEXT("Declare zone units and their positive length map before converting coordinates or widths.");return false;}
    Out=Scale;Error.Empty();return true;
}
void StudioHome4Setup::InvalidatePatchCounts(FStudioHome4Spec& S)
{
    S.Multidomain.LevelCells.Reset();
    for(auto& A:S.Performance.Allocations)if(A.NodeScope.StartsWith(TEXT("level:")))A.Nodes.Reset();
}
bool StudioHome4Setup::Tank(const FStudioHome4AuthoringPreview& P,FStudioHome4Spec& Out,FString& Error)
{
    if(!StudioHome4Config::Validate(P.Spec,Error))return false;
    if(!P.IsValid()||!P.Body.IsValid||P.Body.Min.ContainsNaN()||P.Body.Max.ContainsNaN()||
       P.RequestSHA256!=StudioHome4Authoring::Fingerprint(P.Spec)||!P.Spec.Reference.LengthCells||
       !P.Spec.Lattice.PadUp||!P.Spec.Lattice.PadDown||!P.Spec.Lattice.PadSide||
       !P.Spec.Lattice.Depth||!P.Spec.Lattice.Air||!P.Spec.Authoring.WaterlineCells)
    {Error=TEXT("Prepare the current body with explicit L, upstream/downstream/side pads, depth, air and waterline before laying out the tank.");return false;}
    if(P.Spec.Geometry.Float.Get(false)&&!P.Spec.Geometry.NoEquilibrate.Get(false))
    {Error=TEXT("Accept the prepared equilibrium pose, then prepare again before laying out this tank.");return false;}
    FStudioHome4Spec S=P.Spec;const double L=*S.Reference.LengthCells,W=*S.Authoring.WaterlineCells;
    const FBox Bounds(FVector(P.Body.Min.X-*S.Lattice.PadUp*L,P.Body.Min.Y-*S.Lattice.PadSide*L,W-*S.Lattice.Depth*L),
                      FVector(P.Body.Max.X+*S.Lattice.PadDown*L,P.Body.Max.Y+*S.Lattice.PadSide*L,W+*S.Lattice.Air*L));
    const FVector Size=Bounds.GetSize(),Shift=-Bounds.Min;
    if(!Bounds.IsValid||Bounds.Min.ContainsNaN()||Bounds.Max.ContainsNaN()||Size.GetMin()<2||Size.GetMax()>1048576)
    {Error=TEXT("Pads/depth/air must produce finite XYZ extents from 2 to 1048576 root cells.");return false;}
    if(!StudioHome4SetupLocal::Contains(Bounds,P.Body))
    {Error=TEXT("Depth and air must enclose the complete prepared body below and above the declared waterline.");return false;}
    FIntVector N;for(int32 Axis=0;Axis<3;++Axis)N[Axis]=FMath::CeilToInt(Size[Axis]);
    int64 Root;if(!StudioHome4SetupLocal::Product(N,Root)){Error=TEXT("The tank exceeds the bounded one-trillion-node request budget.");return false;}
    S.Lattice.Extents=N;S.Lattice.StreamwiseCells=N.X;
    if(S.Lattice.WidthLengthRatio)S.Lattice.WidthLengthRatio=N.Y/L;
    if(S.Lattice.HeightLengthRatio)S.Lattice.HeightLengthRatio=N.Z/L;
    S.Geometry.InitialPositionCells=S.Geometry.InitialPositionCells.Get(FVector::ZeroVector)+Shift;
    if(S.Geometry.CenterOfGravity)S.Geometry.CenterOfGravity=*S.Geometry.CenterOfGravity+Shift;
    S.Authoring.WaterlineCells=W+Shift.Z;
    double Scale=1;if(!S.Authoring.Zones.IsEmpty()&&!ZoneScale(S,Scale,Error))return false;
    const FBox RetainedTank(FVector::ZeroVector,FVector(N));
    for(auto& Z:S.Authoring.Zones)
    {
        Z.Minimum+=Shift/Scale;Z.Maximum+=Shift/Scale;
        if(!StudioHome4SetupLocal::Contains(RetainedTank,FBox(Z.Minimum*Scale,Z.Maximum*Scale)))
        {Error=TEXT("Translated zone ")+Z.Id+TEXT(" would lie outside the new tank; review its bounds before tank layout.");return false;}
    }
    for(auto& Patch:S.Authoring.Patches)
    {
        Patch.Origin+=Shift;
        if(!StudioHome4SetupLocal::Contains(RetainedTank,StudioHome4SetupLocal::PatchBounds(Patch)))
        {Error=TEXT("Translated patch ")+Patch.Id+TEXT(" would lie outside the new tank; review its bounds before tank layout.");return false;}
    }
    // Root resizing must not preserve an old explicit allocation count. Refined topology
    // remains a separate reviewed action, so an incomplete MD draft can still lay out a tank.
    if(!S.Multidomain.LevelCells.IsEmpty())S.Multidomain.LevelCells[0]=Root;
    for(auto& A:S.Performance.Allocations)if(A.NodeScope==TEXT("root")||A.NodeScope==TEXT("level:0"))A.Nodes=Root;
    if(!StudioHome4Config::Validate(S,Error))return false;
    Out=MoveTemp(S);Error.Empty();return true;
}
bool StudioHome4Setup::PatchCounts(const FStudioHome4Spec& Input,FStudioHome4Spec& Out,FString& Error)
{
    using namespace StudioHome4SetupLocal;
    if(!StudioHome4Config::Validate(Input,Error))return false;
    int64 Root;if(!Input.Lattice.Extents||!Product(*Input.Lattice.Extents,Root))
    {Error=TEXT("Patch counts require bounded explicit root XYZ extents.");return false;}
    FStudioHome4Spec S=Input;int32 Levels=int32(S.Multidomain.Levels.Get(1));
    for(const auto& P:S.Authoring.Patches)Levels=FMath::Max(Levels,P.Level+1);
    if(Levels<1||Levels>16){Error=TEXT("MD level count must be 1–16.");return false;}
    TArray<int64> Counts;Counts.Init(0,Levels);Counts[0]=Root;
    const FBox Tank(FVector::ZeroVector,FVector(*S.Lattice.Extents));
    for(int32 I=0;I<S.Authoring.Patches.Num();++I)
    {
        const auto& P=S.Authoring.Patches[I];int64 Nodes;
        if(P.Level==0){Error=TEXT("Root is the full declared lattice; authored refined patches must have level 1–15.");return false;}
        if(P.bFollowBody&&(P.BodyId.IsEmpty()||P.BodyId!=S.Authoring.BodyId))
        {Error=TEXT("A following patch must bind the exact declared body identity.");return false;}
        const FBox Bounds=PatchBounds(P);
        if(!Contains(Tank,Bounds)){Error=TEXT("Patch ")+P.Id+TEXT(" extends outside the declared root tank.");return false;}
        bool bParent=P.Level==1;
        for(int32 J=0;J<S.Authoring.Patches.Num();++J)
        {
            const auto& Other=S.Authoring.Patches[J];
            if(Other.Level==P.Level-1&&Contains(PatchBounds(Other),Bounds))bParent=true;
            if(J<I&&Other.Level==P.Level&&Overlaps(PatchBounds(Other),Bounds))
            {Error=TEXT("Same-level patches overlap; resolve their ownership before deriving node counts.");return false;}
        }
        if(!bParent){Error=TEXT("Each fine patch must fit in an explicit patch at its immediately coarser level.");return false;}
        if(!Product(P.Extents,Nodes)||Counts[P.Level]>MaxNodes-Nodes)
        {Error=TEXT("Authored patch node counts exceed the request budget.");return false;}
        Counts[P.Level]+=Nodes;
    }
    for(int32 I=1;I<Levels;++I)if(Counts[I]==0)
    {Error=TEXT("Supply an explicit patch at every declared non-root level; missing levels have unknown counts.");return false;}
    S.Multidomain.Levels=Levels;S.Multidomain.LevelCells=MoveTemp(Counts);
    for(auto& A:S.Performance.Allocations)
    {
        if(A.NodeScope==TEXT("root"))A.Nodes=Root;
        else if(A.NodeScope.StartsWith(TEXT("level:")))
        {
            const int32 Level=FCString::Atoi(*A.NodeScope.Mid(6));
            if(!S.Multidomain.LevelCells.IsValidIndex(Level)){Error=TEXT("An allocation names an absent MD level.");return false;}
            A.Nodes=S.Multidomain.LevelCells[Level];
        }
    }
    if(!StudioHome4Config::Validate(S,Error))return false;
    Out=MoveTemp(S);Error.Empty();return true;
}
bool StudioHome4Setup::Zones(const FStudioHome4Spec& Input,FStudioHome4Spec& Out,FString& Error)
{
    if(!StudioHome4Config::Validate(Input,Error))return false;
    if(!Input.Lattice.Extents){Error=TEXT("Declare root XYZ extents before realizing zone widths.");return false;}
    double Scale;if(!ZoneScale(Input,Scale,Error))return false;
    if(!Input.Zones.Sponge&&!Input.Zones.XBeach&&!Input.Zones.BeachY&&!Input.Zones.FloorFriction)
    {Error=TEXT("Supply at least one explicit frontend width/start/friction setting before realizing regions.");return false;}
    if((Input.Zones.Sponge||Input.Zones.BeachY)&&!Input.Zones.ZoneStrength)
    {Error=TEXT("Sponge and side beaches need an explicit root zone strength; an unknown default is not substituted.");return false;}
    if(Input.Zones.XBeach&&!Input.Zones.XBeachStrength)
    {Error=TEXT("X beach needs its explicit root strength.");return false;}
    if(Input.Zones.BeachY&&!Input.Zones.BeachGap)
    {Error=TEXT("Side beaches need an explicit minimum clear gap in the declared zone units.");return false;}
    FStudioHome4Spec S=Input;const FVector Tank=FVector(*S.Lattice.Extents)/Scale;
    S.Authoring.Zones.RemoveAll([](const auto& Z){return Z.Id.StartsWith(TEXT("frontend-width-"));});
    auto Add=[&](const TCHAR* Id,const TCHAR* Kind,const TCHAR* Axis,const FVector& Min,const FVector& Max,double Strength,const TCHAR* Profile=TEXT("cubic"))
    {FStudioHome4AuthoredZone Z;Z.Id=Id;Z.Kind=Kind;Z.Axis=Axis;Z.Minimum=Min;Z.Maximum=Max;Z.Strength=Strength;Z.Profile=Profile;S.Authoring.Zones.Add(MoveTemp(Z));};
    if(S.Zones.Sponge)
    {
        const double Width=*S.Zones.Sponge;
        if(Width<=0||Width>=Tank.X){Error=TEXT("Sponge width must be positive and smaller than tank X in declared zone units.");return false;}
        Add(TEXT("frontend-width-sponge"),TEXT("sponge"),TEXT("x"),FVector(Tank.X-Width,0,0),Tank,*S.Zones.ZoneStrength);
    }
    if(S.Zones.XBeach)
    {
        const double Start=*S.Zones.XBeach;
        if(Start<0||Start>=Tank.X){Error=TEXT("Frontend X beach start must lie inside tank X.");return false;}
        Add(TEXT("frontend-width-beach-x"),TEXT("beach"),TEXT("x"),FVector(Start,0,0),Tank,*S.Zones.XBeachStrength);
    }
    if(S.Zones.BeachY)
    {
        const double Width=*S.Zones.BeachY,Gap=*S.Zones.BeachGap;
        if(Width<=0||Gap<0||2*Width+Gap>Tank.Y){Error=TEXT("Two positive side widths plus the requested minimum clear gap must fit tank Y.");return false;}
        Add(TEXT("frontend-width-beach-y-low"),TEXT("beach"),TEXT("y"),FVector::ZeroVector,FVector(Tank.X,Width,Tank.Z),*S.Zones.ZoneStrength,TEXT("cubic-reverse"));
        Add(TEXT("frontend-width-beach-y-high"),TEXT("beach"),TEXT("y"),FVector(0,Tank.Y-Width,0),Tank,*S.Zones.ZoneStrength);
    }
    if(S.Zones.FloorFriction)
        Add(TEXT("frontend-width-floor"),TEXT("floor"),TEXT("z"),FVector::ZeroVector,FVector(Tank.X,Tank.Y,FMath::Min(Tank.Z,1/Scale)),*S.Zones.FloorFriction,TEXT("constant"));
    if(!StudioHome4Config::Validate(S,Error))return false;
    Out=MoveTemp(S);Error.Empty();return true;
}
TOptional<FStudioHome4RampPoint> StudioHome4Setup::Ramp(const FStudioHome4Spec& S,double Step)
{
    FString Error;if(!StudioHome4Config::Validate(S,Error))return {};
    const auto D=StudioHome4Config::Derive(S);
    if(!StudioHome4SetupLocal::Positive(D.Speed)||!StudioHome4SetupLocal::Positive(S.Reference.LengthCells)||
       !S.Run.RampLength||!FMath::IsFinite(Step)||Step<0||Step>1.e12)return {};
    FStudioHome4RampPoint P;P.Step=Step;P.Duration=*S.Run.RampLength* *S.Reference.LengthCells/ *D.Speed;
    if(!FMath::IsFinite(P.Duration)||P.Duration<0)return {};
    if(P.Duration==0){P.Speed=*D.Speed;P.Acceleration=0;}
    else
    {
        const double T=FMath::Clamp(Step/P.Duration,0.,1.);
        P.Speed=*D.Speed*(3*T*T-2*T*T*T);
        P.Acceleration=Step<P.Duration?*D.Speed*6*T*(1-T)/P.Duration:0;
    }
    if(S.Run.NoFrameAcceleration)
    {
        const double Accel=*S.Run.NoFrameAcceleration?0:P.Acceleration;
        if(D.RhoHeavy)P.HeavyFrameForceDensity=*D.RhoHeavy*Accel;
        if(D.RhoLight)P.LightFrameForceDensity=*D.RhoLight*Accel;
    }
    if(!FMath::IsFinite(P.Speed)||!FMath::IsFinite(P.Acceleration))return {};
    return P;
}
TOptional<FStudioHome4WaveProperties> StudioHome4Setup::WaveProperties(const FStudioHome4Spec& S,FString& Error)
{
    if(!StudioHome4Config::Validate(S,Error))return {};
    const auto& A=S.Authoring;const auto D=StudioHome4Config::Derive(S);
    if(A.WaveModel!=TEXT("linear-gravity")||!StudioHome4SetupLocal::Positive(A.WaveLengthCells)||
       !StudioHome4SetupLocal::Positive(A.WaveDepthCells)||!A.WaveAmplitudeCells||!A.WaterlineCells||
       !StudioHome4SetupLocal::Positive(D.Gravity)||!S.Lattice.Extents||(*A.WaveAmplitudeCells<0))
    {Error=TEXT("Finite-depth gravity preview needs declared wavelength, depth, nonnegative amplitude, waterline, gravity and tank XYZ.");return {};}
    if(*A.WaveDepthCells>*A.WaterlineCells||*A.WaterlineCells-*A.WaveAmplitudeCells<0||
       *A.WaterlineCells+*A.WaveAmplitudeCells>S.Lattice.Extents->Z)
    {Error=TEXT("Wave depth must fit below waterline; the complete amplitude must fit inside tank Z.");return {};}
    FStudioHome4WaveProperties P;P.WaveNumber=2.0*UE_DOUBLE_PI/ *A.WaveLengthCells;
    P.AngularFrequency=FMath::Sqrt(*D.Gravity*P.WaveNumber*std::tanh(P.WaveNumber* *A.WaveDepthCells));
    P.Period=2.0*UE_DOUBLE_PI/P.AngularFrequency;P.PhaseSpeed=P.AngularFrequency/P.WaveNumber;
    if(!FMath::IsFinite(P.Period)||!FMath::IsFinite(P.PhaseSpeed)||P.AngularFrequency<=0)
    {Error=TEXT("Declared wave parameters produce a nonfinite dispersion relation.");return {};}
    if(A.WavePeriodSteps&&FMath::Abs(*A.WavePeriodSteps-P.Period)>.01*P.Period)
    {Error=TEXT("Supplied period differs from the declared finite-depth gravity dispersion by more than 1%.");return {};}
    if(S.Reference.WavePhaseSpeed&&FMath::Abs(*S.Reference.WavePhaseSpeed-P.PhaseSpeed)>.01*P.PhaseSpeed)
    {Error=TEXT("Supplied wave speed differs from the declared finite-depth gravity dispersion by more than 1%.");return {};}
    if(S.Reference.WaveSlope&&FMath::Abs(*S.Reference.WaveSlope-P.WaveNumber* *A.WaveAmplitudeCells)>.01*FMath::Max(1.e-12,P.WaveNumber* *A.WaveAmplitudeCells))
    {Error=TEXT("Supplied wave slope differs from k times the declared linear wave amplitude by more than 1%.");return {};}
    Error.Empty();return P;
}
TOptional<double> StudioHome4Setup::Wave(const FStudioHome4Spec& S,const FVector& Position,double Step,FString& Error)
{
    const auto P=WaveProperties(S,Error);if(!P)return {};
    if(Position.ContainsNaN()||Position.GetAbsMax()>1.e12||!FMath::IsFinite(Step)||FMath::Abs(Step)>1.e12)
    {Error=TEXT("Wave position and step must be finite and bounded.");return {};}
    const int32 Axis=S.Authoring.WaveAxis==TEXT("y")?1:0;
    const double Height=*S.Authoring.WaterlineCells+*S.Authoring.WaveAmplitudeCells*FMath::Cos(P->WaveNumber*Position[Axis]-P->AngularFrequency*Step);
    if(!FMath::IsFinite(Height)){Error=TEXT("Wave elevation is nonfinite.");return {};}
    Error.Empty();return Height;
}
bool StudioHome4Setup::WaveCurve(const FStudioHome4Spec& S,double Step,int32 Segments,TArray<FVector>& Out,FString& Error)
{
    if(Segments<4||Segments>1024){Error=TEXT("Wave curve requires 4–1024 bounded sample segments.");return false;}
    const auto P=WaveProperties(S,Error);if(!P)return false;
    if(!FMath::IsFinite(Step)||FMath::Abs(Step)>1.e12){Error=TEXT("Wave step must be finite and bounded.");return false;}
    const int32 Axis=S.Authoring.WaveAxis==TEXT("y")?1:0;const FVector Tank(*S.Lattice.Extents);
    TArray<FVector> Points;Points.Reserve(Segments+1);
    for(int32 I=0;I<=Segments;++I)
    {
        FVector V=Tank*.5;V[Axis]=Tank[Axis]*double(I)/Segments;
        V.Z=*S.Authoring.WaterlineCells+*S.Authoring.WaveAmplitudeCells*FMath::Cos(P->WaveNumber*V[Axis]-P->AngularFrequency*Step);
        Points.Add(V);
    }
    Out=MoveTemp(Points);Error.Empty();return true;
}
bool StudioHome4Setup::BoundaryFaces(const FStudioHome4Spec& S,TArray<FStudioHome4BoundaryFace>& Out,FString& Error)
{
    if(!StudioHome4Config::Validate(S,Error))return false;
    if(!S.Lattice.Extents){Error=TEXT("Named boundary faces need explicit XYZ tank extents.");return false;}
    const FBox Tank(FVector::ZeroVector,FVector(*S.Lattice.Extents));
    const TOptional<bool> Periodic[]={S.Zones.PeriodicX,S.Zones.PeriodicY,S.Zones.PeriodicZ};
    TArray<FStudioHome4BoundaryFace> Faces;
    for(int32 Axis=0;Axis<3;++Axis)for(int32 Side=0;Side<2;++Side)
    {
        FStudioHome4BoundaryFace F;F.Id=FString::Printf(TEXT("%s%s"),Axis==0?TEXT("X"):Axis==1?TEXT("Y"):TEXT("Z"),Side?TEXT("+"):TEXT("−"));
        F.Bounds=Tank;F.Bounds.Min[Axis]=F.Bounds.Max[Axis]=Side?Tank.Max[Axis]:0;
        F.Constraint=Periodic[Axis].Get(false)?TEXT("periodic pair"):
            Axis==0?(Side?TEXT("outlet: ")+S.Authoring.OutletMode:TEXT("inlet: ")+S.Authoring.InletMode):TEXT("wall: ")+S.Authoring.BoundaryWall;
        if(!Periodic[Axis])F.Constraint+=TEXT(" · periodic choice unspecified");
        if(Axis==2)
        {
            F.Phase=Side?S.Zones.PhiTop:S.Zones.PhiBottom;
            F.PhaseConstraint=S.Zones.PinPhaseWalls?(*S.Zones.PinPhaseWalls?TEXT("pinned phase"):TEXT("phase pinning off")):TEXT("phase pinning unspecified");
            if(S.Zones.PinPhaseWalls.Get(false)&&Periodic[Axis].Get(false))F.PhaseConstraint+=TEXT(" · conflicts with periodic Z");
        }
        else if(!Periodic[Axis].Get(false)&&Axis==1)
            F.PhaseConstraint=S.Authoring.PierceMode==TEXT("phase-pinned")?TEXT("pierce: phase-pinned at declared interface intersection; wall φ unspecified"):
                S.Authoring.PierceMode==TEXT("free-interface")?TEXT("pierce: free interface intersection"):TEXT("pierce: off; interface unchanged");
        Faces.Add(MoveTemp(F));
    }
    Out=MoveTemp(Faces);Error.Empty();return true;
}
