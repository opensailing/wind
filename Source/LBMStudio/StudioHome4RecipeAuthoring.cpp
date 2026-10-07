#include "StudioHome4RecipeAuthoring.h"
#include "StudioHome4Recipes.h"
#include "Dom/JsonObject.h"
#include <cmath>
namespace StudioHome4RecipeAuthoringLocal
{
bool Positive(const TOptional<double>& V){return V&&FMath::IsFinite(*V)&&*V>0;}
FString Scalar(const TSharedPtr<FJsonValue>& V)
{
    if(!V||V->Type==EJson::Null)return {};
    if(V->Type==EJson::Number)return FString::Printf(TEXT("%.9g"),V->AsNumber());
    if(V->Type==EJson::Boolean)return V->AsBool()?TEXT("On"):TEXT("Off");
    if(V->Type==EJson::String)return V->AsString();
    return {};
}
}
FString StudioHome4RecipeAuthoring::Parameters(const FString& Id)
{
    const auto* R=StudioHome4Recipes::Find(Id);if(!R)return TEXT("Choose a documented recipe.");const auto Object=StudioHome4Config::ToJSON(R->Template);FString Text=R->Name+TEXT(" · documented original request parameters\n");
    for(const auto& F:StudioHome4Config::Fields())
    {
        if(F.Section.IsEmpty()||F.Section==TEXT("authoring")||F.Key==TEXT("display")||F.Key==TEXT("backend"))continue;
        const TSharedPtr<FJsonObject>* Section=nullptr;if(!Object->TryGetObjectField(F.Section,Section))continue;
        const FString Value=StudioHome4RecipeAuthoringLocal::Scalar((*Section)->TryGetField(F.Key));if(!Value.IsEmpty())Text+=F.Label+TEXT(": ")+Value+(F.Unit.IsEmpty()?TEXT(""):TEXT(" ")+F.Unit)+TEXT("\n");
    }
    Text+=TEXT("Reference asset/anchor: ")+R->Reference+TEXT("\n")+R->Notes+TEXT("\nOriginal scientific validation requires identified source results and references.");return Text;
}
FString StudioHome4RecipeAuthoring::Relationship(const FStudioHome4Spec& S)
{
    const FString Id=S.RecipeId;
    if(Id==TEXT("rti-fakhari"))return TEXT("Resolve supplied dimensionless groups through the declared L/U/g/phase map. Reference length and speed remain explicit user choices; no initial RTI perturbation is fabricated.");
    if(Id==TEXT("breaking-wave-banari"))return TEXT("Declared finite-depth linear wave: k=2π/λ, ω=k c, g=ω²/(k tanh(kh)), a=slope/k, T=λ/c. Documented Nx and W/L/Lz/L map to the reviewed Cartesian tank. Nonlinear breaking evolution belongs to the solver.");
    if(Id==TEXT("colagrossi-wb"))return TEXT("Confirm diameter-reference: L=D, g=(U/Fr)²/D, sponge=8D. Mobility 0.05 only at Fr<0.8. Retain the chosen span and origin. No unspecified U value is selected.");
    if(Id==TEXT("oscillating-cylinder"))return TEXT("Confirm diameter-reference: KC=Umax T/D, A=KC D/(2π), f=Umax/(KC D), ν=Umax D/Re. Peak speed is separate from inlet speed. X/Y tank=20D; body/tank Z are explicit.");
    if(Id==TEXT("couette-spin"))return TEXT("Choose spin-surface-diameter (ReΩ=Us D/ν) or spin-omega-radius (ReΩ=Ω R²/ν). Both use Ω=Us/R and D=2R. This is an explicit frontend convention, not an inferred driver default. X/Y=12D; Z remains explicit.");
    if(Id==TEXT("magnus"))return TEXT("Confirm diameter-reference: L=D, X/Y=30D/16D; chosen angular speed is ramped using the cubic Run ramp. Translational Re stays UL/ν. The literature anchor is unresolved.");
    if(Id==TEXT("sedimentation"))return TEXT("Confirm sedimentation-diameter-fluid-heavy: Ga=√((ρbody/ρheavy−1)gD³)/νheavy. Body density ratio is distinct from the two-fluid density ratio. Prepare actual closed geometry then calculate mass at the resulting uniform density; free motion previews only declared initial state.");
    if(Id==TEXT("vugts-barge"))return TEXT("Confirm barge-beam-draft: transverse beam Y=(B/T)×explicit draft, retain longitudinal X and full height Z. Documented forcing allows heave or roll. Mass/CoG/inertia/K remain explicit or computed from actual closed mesh.");
    if(Id==TEXT("hydrofoil-parkin"))return TEXT("Confirm foil-cog-chord: L is chord; declared CoG is the chosen depth anchor. Set CoG depth h=1.8c below declared waterline, translating the explicit source origin by the same delta. Original geometry and span remain supplied inputs.");
    return TEXT("TH01: retain documented L=256 and explicitly choose Fr/Re/source geometry. Resolve U, ν and g only from supplied inputs. Use reviewed tank/padding and zone actions; measured G/Q/P presets require original evidence.");
}
bool StudioHome4RecipeAuthoring::Resolve(const FStudioHome4Spec& Input,FStudioHome4Spec& Out,FString& Error)
{
    using namespace StudioHome4RecipeAuthoringLocal;const auto* Recipe=StudioHome4Recipes::Find(Input.RecipeId);if(!Recipe){Error=TEXT("Choose one of the ten documented recipes first.");return false;}
    if(!StudioHome4Config::Validate(Input,Error))return false;FStudioHome4Spec S=Input;const auto D=StudioHome4Config::Derive(S);const auto L=S.Reference.LengthCells;const FString Id=S.RecipeId,C=S.Authoring.BenchmarkConvention;
    auto Fail=[&](const TCHAR* T){Error=T;return false;};
    const bool Cylinder=Id==TEXT("colagrossi-wb")||Id==TEXT("oscillating-cylinder")||Id==TEXT("couette-spin")||Id==TEXT("magnus")||Id==TEXT("sedimentation");
    if(Cylinder)
    {
        if(!Positive(L)||!S.Authoring.PrimitiveSizeCells||!S.Lattice.Extents)return Fail(TEXT("Declare reference diameter D, explicit body XYZ size/span, and tank XYZ before applying a cylinder relationship."));
        if(Id==TEXT("couette-spin")){if(C!=TEXT("spin-surface-diameter")&&C!=TEXT("spin-omega-radius"))return Fail(TEXT("Choose the explicit rotational Reynolds convention before deriving spin viscosity."));}
        else if(Id==TEXT("sedimentation")){if(C!=TEXT("sedimentation-diameter-fluid-heavy"))return Fail(TEXT("Confirm the sedimentation diameter/heavy-fluid reference convention."));}
        else if(C!=TEXT("diameter-reference"))return Fail(TEXT("Confirm L=D with the diameter-reference convention before applying cylinder relationships."));
        auto Size=*S.Authoring.PrimitiveSizeCells;Size.X=Size.Y=*L;S.Authoring.Primitive=TEXT("cylinder");S.Authoring.PrimitiveSizeCells=Size;
        for(const auto& Ratio:{S.Authoring.DomainLengthRatio,S.Authoring.DomainWidthRatio})if(Ratio&&(!FMath::IsFinite(*L* *Ratio)||*L* *Ratio<1||*L* *Ratio>1048576))return Fail(TEXT("Documented cylinder layout exceeds bounded Cartesian axis counts."));
        if(S.Authoring.DomainLengthRatio){S.Lattice.Extents->X=FMath::CeilToInt(*L* *S.Authoring.DomainLengthRatio);S.Lattice.StreamwiseCells=S.Lattice.Extents->X;}
        if(S.Authoring.DomainWidthRatio)S.Lattice.Extents->Y=FMath::CeilToInt(*L* *S.Authoring.DomainWidthRatio);
        if(Id==TEXT("colagrossi-wb"))
        {
            if(!Positive(D.Speed)||!Positive(S.Reference.Froude))return Fail(TEXT("Colagrossi gravity requires explicitly selected U and Fr."));
            S.Fluids.Gravity=FMath::Square(*D.Speed/ *S.Reference.Froude)/ *L;
            double OldScale=1;if(S.Authoring.ZoneUnits==TEXT("body-lengths"))OldScale=*L;else if(S.Authoring.ZoneUnits==TEXT("physical-metres")){if(!Positive(S.Units.DxMeters))return Fail(TEXT("Existing physical zones require dx before a root-cell sponge can be applied."));OldScale=1/ *S.Units.DxMeters;}else if(!S.Authoring.ZoneUnits.IsEmpty()&&S.Authoring.ZoneUnits!=TEXT("root-cells"))return Fail(TEXT("Existing zone coordinates require a supported declared unit."));else if(S.Authoring.ZoneUnits.IsEmpty()&&(!S.Authoring.Zones.IsEmpty()||S.Zones.XBeach||S.Zones.BeachY||S.Zones.BeachGap))return Fail(TEXT("Declare existing zone coordinate units before applying an 8D sponge."));
            for(auto& Z:S.Authoring.Zones){Z.Minimum*=OldScale;Z.Maximum*=OldScale;}for(auto* Width:{&S.Zones.XBeach,&S.Zones.BeachY,&S.Zones.BeachGap})if(*Width)**Width=**Width*OldScale;
            S.Zones.Sponge=8* *L;S.Authoring.ZoneUnits=TEXT("root-cells");if(*S.Reference.Froude<.8)S.Fluids.Mobility=.05;
        }
        if(Id==TEXT("oscillating-cylinder"))
        {
            if(!Positive(S.Reference.OscillationPeakSpeed)||!Positive(S.Reference.KeuleganCarpenter)||!Positive(S.Reference.Reynolds))return Fail(TEXT("Oscillation requires positive explicit Umax, KC and Re."));
            S.Geometry.HeaveAmplitudeCells=*S.Reference.KeuleganCarpenter* *L/(2*UE_DOUBLE_PI);S.Geometry.MotionFrequencyCyclesPerStep=*S.Reference.OscillationPeakSpeed/(*S.Reference.KeuleganCarpenter* *L);S.Fluids.NuHeavy=*S.Reference.OscillationPeakSpeed* *L/ *S.Reference.Reynolds;
        }
        if(Id==TEXT("couette-spin"))
        {
            if(!Positive(S.Reference.SpinSurfaceSpeed)||!Positive(S.Reference.RotationalReynolds))return Fail(TEXT("Spin requires supplied Us and rotational Re."));
            S.Geometry.SpinRadiansPerStep=2* *S.Reference.SpinSurfaceSpeed/ *L;
            S.Fluids.NuHeavy=C==TEXT("spin-surface-diameter")?*S.Reference.SpinSurfaceSpeed* *L/ *S.Reference.RotationalReynolds:*S.Geometry.SpinRadiansPerStep*FMath::Square(*L*.5)/ *S.Reference.RotationalReynolds;
        }
        if(Id==TEXT("magnus"))
        {
            if(!Positive(S.Geometry.SpinRadiansPerStep))return Fail(TEXT("Magnus requires an explicitly chosen positive spin rate before applying its ramped-spin request."));
            if(S.Run.RampLength&&Positive(D.Speed))S.Geometry.SpinRampSteps=*S.Run.RampLength* *L/ *D.Speed;
            if(!Positive(S.Geometry.SpinRampSteps))return Fail(TEXT("Magnus ramped spin requires explicit positive spin ramp steps, or a Run ramp length with chosen reference speed."));
        }
        if(Id==TEXT("sedimentation"))
        {
            if(!Positive(S.Reference.Galileo)||!Positive(S.Geometry.BodyFluidDensityRatio)||*S.Geometry.BodyFluidDensityRatio<=1||!Positive(D.NuHeavy)||!Positive(D.RhoHeavy))return Fail(TEXT("Sedimentation needs Ga, body/heavy density ratio>1, heavy viscosity and heavy density."));
            S.Fluids.Gravity=FMath::Square(*S.Reference.Galileo* *D.NuHeavy)/((*S.Geometry.BodyFluidDensityRatio-1)*FMath::Pow(*L,3));
        }
    }
    if(Id==TEXT("vugts-barge"))
    {
        if(C!=TEXT("barge-beam-draft")||!Positive(S.Authoring.BenchmarkDraftCells)||!Positive(S.Geometry.BeamDraftRatio)||!S.Authoring.PrimitiveSizeCells)return Fail(TEXT("Choose barge-beam-draft and declare draft plus actual longitudinal/full-height dimensions."));
        if(*S.Authoring.BenchmarkDraftCells>S.Authoring.PrimitiveSizeCells->Z)return Fail(TEXT("Declared draft cannot exceed the barge's complete height."));
        S.Authoring.Primitive=TEXT("box");S.Authoring.PrimitiveSizeCells->Y=*S.Geometry.BeamDraftRatio* *S.Authoring.BenchmarkDraftCells;
    }
    if(Id==TEXT("hydrofoil-parkin"))
    {
        if(C!=TEXT("foil-cog-chord")||!Positive(L)||!S.Authoring.WaterlineCells||!S.Geometry.CenterOfGravity||!S.Geometry.InitialPositionCells||!Positive(S.Geometry.SubmergenceChordRatio))return Fail(TEXT("Choose foil-cog-chord with explicit chord, waterline, CoG and source origin before placing submergence."));
        const double Z=*S.Authoring.WaterlineCells-*S.Geometry.SubmergenceChordRatio* *L,Delta=Z-S.Geometry.CenterOfGravity->Z;S.Geometry.CenterOfGravity->Z=Z;S.Geometry.InitialPositionCells->Z+=Delta;
    }
    if(Id==TEXT("breaking-wave-banari"))
    {
        if(!Positive(S.Authoring.WaveLengthCells)||!Positive(S.Authoring.WaveDepthCells)||!Positive(S.Reference.WavePhaseSpeed)||!S.Reference.WaveSlope||!S.Lattice.StreamwiseCells)return Fail(TEXT("Declare wavelength/depth and supplied wave phase speed/slope before applying linear-wave relationships."));
        const double K=2*UE_DOUBLE_PI/ *S.Authoring.WaveLengthCells,Omega=K* *S.Reference.WavePhaseSpeed;S.Fluids.Gravity=Omega*Omega/(K*std::tanh(K* *S.Authoring.WaveDepthCells));S.Authoring.WaveAmplitudeCells=*S.Reference.WaveSlope/K;S.Authoring.WavePeriodSteps=*S.Authoring.WaveLengthCells/ *S.Reference.WavePhaseSpeed;S.Authoring.WaveModel=TEXT("linear-gravity");
        if(!S.Lattice.WidthLengthRatio||!S.Lattice.HeightLengthRatio)return Fail(TEXT("Supply the documented wave width/height ratios for the Cartesian layout."));
        for(const auto& Ratio:{S.Lattice.WidthLengthRatio,S.Lattice.HeightLengthRatio})if(!FMath::IsFinite(*S.Lattice.StreamwiseCells* *Ratio)||*S.Lattice.StreamwiseCells* *Ratio<1||*S.Lattice.StreamwiseCells* *Ratio>1048576)return Fail(TEXT("Wave layout exceeds bounded Cartesian axis counts."));
        S.Lattice.Extents=FIntVector(int32(*S.Lattice.StreamwiseCells),FMath::CeilToInt(*S.Lattice.StreamwiseCells* *S.Lattice.WidthLengthRatio),FMath::CeilToInt(*S.Lattice.StreamwiseCells* *S.Lattice.HeightLengthRatio));
    }
    const auto Resolved=StudioHome4Config::Derive(S);
    for(auto Pair:{TPair<TOptional<double>*,const TOptional<double>*>(&S.Fluids.RhoHeavy,&Resolved.RhoHeavy),{&S.Fluids.RhoLight,&Resolved.RhoLight},{&S.Fluids.NuHeavy,&Resolved.NuHeavy},{&S.Fluids.NuLight,&Resolved.NuLight},{&S.Fluids.Sigma,&Resolved.Sigma},{&S.Fluids.Mobility,&Resolved.Mobility},{&S.Fluids.Gravity,&Resolved.Gravity}})if(!*Pair.Key&&Positive(*Pair.Value))*Pair.Key=*Pair.Value;
    if(!Cylinder&&!S.Reference.SpeedCellsPerStep&&Positive(Resolved.Speed))S.Reference.SpeedCellsPerStep=Resolved.Speed;
    if(!StudioHome4Config::Validate(S,Error))return false;Out=MoveTemp(S);Error.Empty();return true;
}
