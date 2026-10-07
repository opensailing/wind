#include "StudioHome4Recipes.h"
#include "Dom/JsonObject.h"

namespace
{
FStudioHome4Recipe Recipe(const TCHAR* Id,const TCHAR* Name,const TCHAR* Anchor,const TCHAR* Driver,const TCHAR* Reference,const TCHAR* Gate)
{
    FStudioHome4Recipe R;R.Id=Id;R.Name=Name;R.Anchor=Anchor;R.Driver=Driver;R.Reference=Reference;R.Gate=Gate;
    R.Template.RecipeId=Id;
    R.Notes=TEXT("Parameters transcribed from HOME4 FRONTEND_DESIGN, 2026-10-05. Reference data and a completed gate are required to establish validation.");
    return R;
}
void Changed(const TSharedPtr<FJsonObject>& Base,const TSharedPtr<FJsonObject>& Current,const FString& Prefix,TArray<FString>& Out)
{
    TArray<FString> Keys;for(const auto& E:Base->Values)Keys.Add(FString(*E.Key));Keys.Sort();
    for(const auto& Key:Keys)
    {
        if(Key==TEXT("recipeId")||Key==TEXT("lineageId")||Key==TEXT("branchId")||Key==TEXT("parentRunId")||Key==TEXT("parentSpecSHA256")||Key==TEXT("version"))continue;
        const auto B=Base->TryGetField(Key);const auto C=Current->TryGetField(Key);
        if(B->Type==EJson::Null)continue;
        const FString Path=Prefix.IsEmpty()?Key:Prefix+TEXT(".")+Key;
        if(B->Type==EJson::Object&&C&&C->Type==EJson::Object){Changed(B->AsObject(),C->AsObject(),Path,Out);continue;}
        bool bDifferent=!C||B->Type!=C->Type;
        if(!bDifferent)
        {
            if(B->Type==EJson::Number)bDifferent=B->AsNumber()!=C->AsNumber();
            else if(B->Type==EJson::Boolean)bDifferent=B->AsBool()!=C->AsBool();
            else if(B->Type==EJson::String){if(B->AsString().IsEmpty())continue;bDifferent=B->AsString()!=C->AsString();}
            else if(B->Type==EJson::Array)
            {
                const auto& A=B->AsArray();const auto& D=C->AsArray();if(A.IsEmpty())continue;
                bDifferent=A.Num()!=D.Num();for(int32 I=0;!bDifferent&&I<A.Num();++I)bDifferent=A[I]->Type!=D[I]->Type||A[I]->AsNumber()!=D[I]->AsNumber();
            }
        }
        if(bDifferent)Out.Add(Path+TEXT(" differs from the supplied recipe. Validation has not transferred to this change."));
    }
}
}
const TArray<FStudioHome4Recipe>& StudioHome4Recipes::All()
{
    static const TArray<FStudioHome4Recipe> Recipes=[]
    {
        TArray<FStudioHome4Recipe> R;
        auto RTI=Recipe(TEXT("rti-fakhari"),TEXT("RTI · Fakhari"),TEXT("Fakhari Rayleigh–Taylor instability"),TEXT("run_rti3d_multiphase.py"),TEXT("Spike and bubble front reference"),TEXT("Spike/bubble fronts and fixed-Cn refinement"));
        RTI.Template.Reference.Atwood=.5;RTI.Template.Reference.Reynolds=3000;RTI.Template.Reference.Capillary=.26;RTI.Template.Reference.Peclet=1000;RTI.Template.Reference.TimeSteps=16000;
        RTI.Template.Fluids.Xi=5;RTI.Template.Fluids.RhoHeavy=3;RTI.Template.Fluids.RhoLight=1;RTI.Template.Fluids.PhaseST=1;RTI.Template.Fluids.PhaseSD1=1.5;RTI.Template.Fluids.PhaseSD2=1.5;RTI.Template.Fluids.PhaseSXY=1.5;R.Add(RTI);
        auto Wave=Recipe(TEXT("breaking-wave-banari"),TEXT("Breaking wave · Banari grid 3"),TEXT("Banari breaking-wave grid 3"),TEXT("run_breaking_wave3d_md.py"),TEXT("Banari jet geometry and energy"),TEXT("Jet geometry and energy agreement"));
        Wave.Template.Fluids.Xi=5;Wave.Template.Fluids.Mobility=.02;Wave.Template.Reference.WavePhaseSpeed=.015;Wave.Template.Reference.WaveSlope=.08;
        Wave.Template.Lattice.StreamwiseCells=400;Wave.Template.Lattice.WidthLengthRatio=.25;Wave.Template.Lattice.HeightLengthRatio=.30;
        Wave.Notes+=TEXT(" Nx=400, W/L=0.25, Lz/L=0.30, wave c_lat=0.015, slope=0.08. c_lat=0.02 is a documented failure; wave speed is not an inlet-speed substitute.");R.Add(Wave);
        auto Cylinder=Recipe(TEXT("colagrossi-wb"),TEXT("Colagrossi WB cylinder"),TEXT("Colagrossi 2018"),TEXT("run_cylinder3d_colagrossi.py"),TEXT("Colagrossi Cd, Cl and free-surface profiles"),TEXT("Drag, lift and surface profile agreement"));
        Cylinder.Template.Reference.Bond=200;Cylinder.Template.Fluids.PhaseSXY=1.8;Cylinder.Template.Fluids.GradientLimiter=true;
        Cylinder.Notes+=TEXT(" Use Fr-aware U and g=(U/Fr)^2/D; M=0.05 applies only when Fr<0.8. Sponge width is 8D.");Cylinder.Template.Authoring.Primitive=TEXT("cylinder");Cylinder.Template.Authoring.SpongeLengthRatio=8;R.Add(Cylinder);
        auto Osc=Recipe(TEXT("oscillating-cylinder"),TEXT("Oscillating cylinder · Dütsch"),TEXT("Dütsch oscillating cylinder"),TEXT("run_cylinder3d_oscillating.py"),TEXT("Dütsch force history"),TEXT("Force history agreement"));
        Osc.Template.Reference.Reynolds=100;Osc.Template.Reference.KeuleganCarpenter=5;Osc.Template.Reference.OscillationPeakSpeed=.04;
        Osc.Template.Geometry.BodyMotion=TEXT("forced-heave");Osc.Notes+=TEXT(" KC=5, U_max=0.04, domain=20D. Oscillation peak speed is not a steady inlet speed.");Osc.Template.Authoring.Primitive=TEXT("cylinder");Osc.Template.Authoring.DomainLengthRatio=20;Osc.Template.Authoring.DomainWidthRatio=20;R.Add(Osc);
        auto Spin=Recipe(TEXT("couette-spin"),TEXT("Couette spin gate"),TEXT("Analytic Couette torque"),TEXT("run_cylinder3d_spinning.py"),TEXT("Analytic torque"),TEXT("Torque agreement with analytic reference"));
        Spin.Template.Geometry.BodyMotion=TEXT("forced-spin");Spin.Template.Reference.SpinSurfaceSpeed=.04;Spin.Template.Reference.RotationalReynolds=100;
        Spin.Notes+=TEXT(" U_s=0.04, rotational Re=100, box=12D. Rotational and translational Reynolds numbers remain distinct.");Spin.Template.Authoring.Primitive=TEXT("cylinder");Spin.Template.Authoring.DomainLengthRatio=12;Spin.Template.Authoring.DomainWidthRatio=12;R.Add(Spin);
        auto Magnus=Recipe(TEXT("magnus"),TEXT("Magnus"),TEXT("Literature anchor requires verification"),TEXT("run_md_magnus_patch.py"),TEXT("Lift reference not yet verified"),TEXT("Lift agreement; reference anchor unresolved"));
        Magnus.Template.Reference.Reynolds=100;Magnus.Template.Reference.SpeedCellsPerStep=.05;Magnus.Template.Geometry.BodyMotion=TEXT("forced-spin");Magnus.Notes+=TEXT(" 30D × 16D, ramped spin.");Magnus.Template.Authoring.Primitive=TEXT("cylinder");Magnus.Template.Authoring.DomainLengthRatio=30;Magnus.Template.Authoring.DomainWidthRatio=16;R.Add(Magnus);
        auto Sed=Recipe(TEXT("sedimentation"),TEXT("Sedimentation"),TEXT("Sedimenting cylinder"),TEXT("run_cylinder3d_sedimenting.py"),TEXT("Terminal velocity reference"),TEXT("Terminal velocity agreement"));
        Sed.Template.Geometry.BodyMotion=TEXT("free");Sed.Template.Geometry.BodyFluidDensityRatio=1.25;Sed.Template.Reference.Galileo=19.6;
        Sed.Notes+=TEXT(" Body/fluid density ratio=1.25, Ga=19.6. This is not the heavy/light fluid density ratio.");Sed.Template.Authoring.Primitive=TEXT("cylinder");R.Add(Sed);
        auto Barge=Recipe(TEXT("vugts-barge"),TEXT("Vugts barge · H2-c"),TEXT("Vugts added mass and damping"),TEXT("run_barge_roll.py"),TEXT("Vugts/BEM coefficient curves"),TEXT("Added mass and damping agreement"));
        Barge.Template.Geometry.BodyMotion=TEXT("forced-heave");Barge.Template.Geometry.BeamDraftRatio=2;Barge.Notes+=TEXT(" B/T=2, forced heave/roll.");Barge.Template.Authoring.Primitive=TEXT("box");R.Add(Barge);
        auto Hull=Recipe(TEXT("th01-hull"),TEXT("TH01 hull"),TEXT("SYRF TH01 tank and BEM"),TEXT("run_hull_speed.py"),TEXT("th01_tank_data.py; th01_bem_capytaine.py"),TEXT("Heave, trim and drag vs tank; BEM intercepts"));
        Hull.Template.Reference.LengthCells=256;Hull.Template.Geometry.BodyMotion=TEXT("fixed");Hull.Notes+=TEXT(" Choose Fr, Re_ref and measured G/Q/P zone preset. Static flotation uses run_hull_static.py.");R.Add(Hull);
        auto Foil=Recipe(TEXT("hydrofoil-parkin"),TEXT("Hydrofoil · Parkin"),TEXT("Parkin–Wu"),TEXT("run_hydrofoil_parkin.py"),TEXT("Parkin–Wu free-surface profile"),TEXT("Surface profile agreement"));
        Foil.Template.Reference.Froude=.95;Foil.Template.Reference.LengthCells=128;Foil.Template.Reference.Reynolds=4000;
        Foil.Template.Geometry.SubmergenceChordRatio=1.8;Foil.Notes+=TEXT(" Submergence h/c=1.8.");R.Add(Foil);
        return R;
    }();return Recipes;
}
const FStudioHome4Recipe* StudioHome4Recipes::Find(const FString& Id)
{return All().FindByPredicate([&](const auto& R){return R.Id==Id;});}
TArray<FString> StudioHome4Recipes::Departures(const FStudioHome4Spec& Spec)
{
    TArray<FString> Out;const auto* R=Find(Spec.RecipeId);
    if(!R){Out.Add(TEXT("No supplied recipe is associated with this case."));return Out;}
    Changed(StudioHome4Config::ToJSON(R->Template),StudioHome4Config::ToJSON(Spec),TEXT(""),Out);return Out;
}
bool StudioHome4Recipes::Ladder(const FStudioHome4Spec& Base,const TArray<int32>& Refinements,TArray<FStudioHome4LadderRung>& Out,FString& Error)
{
    if(!StudioHome4Config::Validate(Base,Error))return false;
    if(Refinements.IsEmpty()||Refinements.Num()>12||!Base.Reference.LengthCells||!Base.Fluids.Xi||
        *Base.Reference.LengthCells<=0||*Base.Fluids.Xi<=0)
    {Error=TEXT("A ladder needs positive body length, interface width and 1–12 refinement factors.");return false;}
    if((Base.Zones.Sponge||Base.Zones.XBeach||Base.Zones.BeachY||Base.Zones.BeachGap||!Base.Authoring.Zones.IsEmpty())&&
        Base.Authoring.ZoneUnits.IsEmpty())
    {Error=TEXT("Declare zone coordinates as root-cells, body-lengths or physical-metres before refinement.");return false;}
    // Resolve only physics derived from supplied inputs, then retain U under acoustic refinement.
    const auto Physics=StudioHome4Config::Derive(Base);
    const FString Lineage=Base.LineageId.IsEmpty()?FGuid::NewGuid().ToString():Base.LineageId;
    TArray<FStudioHome4LadderRung> Result;int32 Previous=0;
    for(const int32 Scale:Refinements)
    {
        if(Scale<=Previous||Scale>64){Error=TEXT("Refinement factors must increase, from 1 to 64.");return false;}Previous=Scale;
        const int64 Volume=int64(Scale)*Scale*Scale;
        FStudioHome4LadderRung R;R.Refinement=Scale;R.Spec=Base;R.Spec.LineageId=Lineage;

        auto Multiply=[&](TOptional<double>& Value,const TOptional<double>& Original,double Factor)
        {if(Original)Value=*Original*Factor;};
        auto Count=[&](TOptional<int64>& Value,const TOptional<int64>& Original,int64 Factor)
        {
            if(!Original)return true;
            if(*Original>1000000000000LL/Factor){Error=TEXT("Refinement count exceeds the supported integer budget.");return false;}
            Value=*Original*Factor;return true;
        };
        Multiply(R.Spec.Reference.LengthCells,Base.Reference.LengthCells,Scale);
        Multiply(R.Spec.Fluids.Xi,Base.Fluids.Xi,Scale);
        if(Physics.Speed)R.Spec.Reference.SpeedCellsPerStep=Physics.Speed;
        if(Physics.RhoHeavy)R.Spec.Fluids.RhoHeavy=Physics.RhoHeavy;
        if(Physics.RhoLight)R.Spec.Fluids.RhoLight=Physics.RhoLight;
        Multiply(R.Spec.Fluids.NuHeavy,Physics.NuHeavy,Scale);Multiply(R.Spec.Fluids.NuLight,Physics.NuLight,Scale);
        Multiply(R.Spec.Fluids.Mobility,Physics.Mobility,Scale);Multiply(R.Spec.Fluids.Sigma,Physics.Sigma,Scale);
        Multiply(R.Spec.Fluids.Gravity,Physics.Gravity,1./Scale);
        Multiply(R.Spec.Reference.TimeSteps,Base.Reference.TimeSteps,Scale);
        Multiply(R.Spec.Units.DxMeters,Base.Units.DxMeters,1./Scale);Multiply(R.Spec.Units.DtSeconds,Base.Units.DtSeconds,1./Scale);
        Multiply(R.Spec.Geometry.SinkCells,Base.Geometry.SinkCells,Scale);Multiply(R.Spec.Geometry.BandCells,Base.Geometry.BandCells,Scale);
        Multiply(R.Spec.Geometry.RetabulateEvery,Base.Geometry.RetabulateEvery,Scale);
        Multiply(R.Spec.Geometry.HeaveAmplitudeCells,Base.Geometry.HeaveAmplitudeCells,Scale);
        Multiply(R.Spec.Geometry.MotionFrequencyCyclesPerStep,Base.Geometry.MotionFrequencyCyclesPerStep,1./Scale);
        Multiply(R.Spec.Geometry.SpinRadiansPerStep,Base.Geometry.SpinRadiansPerStep,1./Scale);
        if(Base.Geometry.CenterOfGravity)R.Spec.Geometry.CenterOfGravity=*Base.Geometry.CenterOfGravity*Scale;
        if(Base.Geometry.InitialPositionCells)R.Spec.Geometry.InitialPositionCells=*Base.Geometry.InitialPositionCells*Scale;
        if(Base.Geometry.InitialAngularVelocityRadiansPerStep)R.Spec.Geometry.InitialAngularVelocityRadiansPerStep=*Base.Geometry.InitialAngularVelocityRadiansPerStep/Scale;
        Multiply(R.Spec.Geometry.BodyMass,Base.Geometry.BodyMass,double(Volume));
        for(int32 I=0;I<R.Spec.Geometry.Stiffness.Num();++I)
        {
            const int32 Power=1+(I/6>=3?1:0)+(I%6>=3?1:0);
            R.Spec.Geometry.Stiffness[I]*=FMath::Pow(double(Scale),Power);
        }
        for(auto* V:{&R.Spec.Geometry.InertiaDiagonal,&R.Spec.Geometry.InertiaProducts})if(*V)**V=**V*FMath::Pow(double(Scale),5.);
        if(R.Spec.Authoring.PrimitiveSizeCells)R.Spec.Authoring.PrimitiveSizeCells=*R.Spec.Authoring.PrimitiveSizeCells*Scale;
        Multiply(R.Spec.Authoring.WaterlineCells,Base.Authoring.WaterlineCells,Scale);
        Multiply(R.Spec.Authoring.WaveLengthCells,Base.Authoring.WaveLengthCells,Scale);
        Multiply(R.Spec.Authoring.WaveDepthCells,Base.Authoring.WaveDepthCells,Scale);
        Multiply(R.Spec.Authoring.WaveAmplitudeCells,Base.Authoring.WaveAmplitudeCells,Scale);
        Multiply(R.Spec.Authoring.WavePeriodSteps,Base.Authoring.WavePeriodSteps,Scale);
        if(Base.Authoring.ZoneUnits==TEXT("root-cells"))
        {
            Multiply(R.Spec.Zones.Sponge,Base.Zones.Sponge,Scale);Multiply(R.Spec.Zones.XBeach,Base.Zones.XBeach,Scale);
            Multiply(R.Spec.Zones.BeachY,Base.Zones.BeachY,Scale);Multiply(R.Spec.Zones.BeachGap,Base.Zones.BeachGap,Scale);
            for(auto& Z:R.Spec.Authoring.Zones){Z.Minimum*=Scale;Z.Maximum*=Scale;}
        }
        for(auto& P:R.Spec.Authoring.Patches){P.Origin*=Scale;P.Extents*=Scale;}

        Multiply(R.Spec.Multidomain.Z1,Base.Multidomain.Z1,Scale);Multiply(R.Spec.Multidomain.Z2,Base.Multidomain.Z2,Scale);
        Multiply(R.Spec.Multidomain.Margin,Base.Multidomain.Margin,Scale);Multiply(R.Spec.Multidomain.BandDepth,Base.Multidomain.BandDepth,Scale);
        Multiply(R.Spec.Multidomain.Overlap,Base.Multidomain.Overlap,Scale);Multiply(R.Spec.Multidomain.RestrictionMargin,Base.Multidomain.RestrictionMargin,Scale);
        Multiply(R.Spec.Multidomain.FinestMobility,Base.Multidomain.FinestMobility,Scale);
        if(Base.Multidomain.TauFloor)R.Spec.Multidomain.TauFloor=.5+(*Base.Multidomain.TauFloor-.5)*Scale;
        R.Spec.Multidomain.FixedCahnRefinement=true;R.Spec.Multidomain.RecipeCahn=*Base.Fluids.Xi/ *Base.Reference.LengthCells;
        for(auto Pair:{TPair<TOptional<int64>*,const TOptional<int64>*>(&R.Spec.Run.Steps,&Base.Run.Steps),
            {&R.Spec.Run.MeasureEvery,&Base.Run.MeasureEvery},{&R.Spec.Run.PrintEvery,&Base.Run.PrintEvery},
            {&R.Spec.Run.SaveEvery,&Base.Run.SaveEvery},{&R.Spec.Run.VizEvery,&Base.Run.VizEvery},
            {&R.Spec.Run.RestartEvery,&Base.Run.RestartEvery},{&R.Spec.Multidomain.MassFixEvery,&Base.Multidomain.MassFixEvery}})
            if(!Count(*Pair.Key,*Pair.Value,Scale))return false;
        if(Base.Lattice.Extents)
        {
            auto N=*Base.Lattice.Extents;for(int32 Axis=0;Axis<3;++Axis)
            {if(N[Axis]>1048576/Scale){Error=TEXT("Refined lattice exceeds the supported axis count.");return false;}N[Axis]*=Scale;}
            R.Spec.Lattice.Extents=N;
        }
        if(Base.Lattice.StreamwiseCells)
        {if(*Base.Lattice.StreamwiseCells>1048576/Scale){Error=TEXT("Refined recipe Nx exceeds the supported axis count.");return false;}R.Spec.Lattice.StreamwiseCells=*Base.Lattice.StreamwiseCells*Scale;}
        for(auto& Cells:R.Spec.Multidomain.LevelCells)
        {if(Cells>1000000000000LL/Volume){Error=TEXT("Refined level cell count exceeds the allocation budget.");return false;}Cells*=Volume;}
        for(auto& Allocation:R.Spec.Performance.Allocations)if(Allocation.Nodes)
        {if(*Allocation.Nodes>1000000000000LL/Volume){Error=TEXT("Refined allocation node count exceeds its budget.");return false;}Allocation.Nodes=*Allocation.Nodes*Volume;}
        R.Spec.Run.Tag=(Base.Run.Tag.IsEmpty()?TEXT("home4"):Base.Run.Tag)+FString::Printf(TEXT("_r%d"),Scale);
        R.Spec.ParentRunId=Base.ParentRunId;R.Spec.BranchId=FGuid::NewGuid().ToString();
        if(Scale>1)R.Spec.Run.InitState.Empty(); // A coarse restart has no certified refined-grid compatibility.
        if(!Base.Run.VizDirectory.IsEmpty())R.Spec.Run.VizDirectory=Base.Run.VizDirectory+FString::Printf(TEXT("_r%d"),Scale);
        if(!Base.Run.SaveState.IsEmpty())R.Spec.Run.SaveState=Base.Run.SaveState+FString::Printf(TEXT("_r%d"),Scale);
        if(!StudioHome4Config::Validate(R.Spec,Error))return false;
        const auto Derived=StudioHome4Config::Derive(R.Spec);R.Cells=Derived.TotalCells;R.AllocationBytes=Derived.AllocationBytes;
        // Reuse only a supplied measured rate with its attribution; never substitute a device peak.
        if(Base.Performance.MeasuredMLUPS&&!Base.Performance.MeasurementSource.TrimStartAndEnd().IsEmpty())
        {
            R.EstimatedSeconds=Derived.EstimatedSeconds;
            R.CostBasis=TEXT("Estimate assuming the supplied measured rate remains constant: ")+Base.Performance.MeasurementSource;
        }
        else R.CostBasis=TEXT("Cost unavailable without an attributed measured hardware/workload rate.");
        Result.Add(MoveTemp(R));
    }
    Out=MoveTemp(Result);Error.Empty();return true;
}
FStudioHome4GateResult StudioHome4Recipes::Compare(const TArray<double>& Actual,const TArray<double>& Reference,double AbsTol,double RelTol,const FString& Identity)
{
    FStudioHome4GateResult R;
    if(Identity.IsEmpty()||Actual.IsEmpty()||Actual.Num()!=Reference.Num()||Actual.Num()>1000000||!FMath::IsFinite(AbsTol)||!FMath::IsFinite(RelTol)||AbsTol<0||RelTol<0)
    {R.Reason=TEXT("Supply an identified reference, explicitly aligned samples and finite nonnegative tolerances.");return R;}
    double Max=0,Scale=0;for(int32 I=0;I<Actual.Num();++I)
    {if(!FMath::IsFinite(Actual[I])||!FMath::IsFinite(Reference[I])){R.Reason=TEXT("Non-finite samples cannot pass a validation gate.");return R;}Scale=FMath::Max(Scale,FMath::Max(FMath::Abs(Actual[I]),FMath::Abs(Reference[I])));}
    double ErrorSquared=0,RefSquared=0;bool Passed=true;
    for(int32 I=0;I<Actual.Num();++I)
    {
        const double E=FMath::Abs(Actual[I]-Reference[I]);if(!FMath::IsFinite(E)){R.Reason=TEXT("Sample difference exceeds numeric range.");return R;}
        const double Tolerance=AbsTol+RelTol*FMath::Abs(Reference[I]);
        if(!FMath::IsFinite(Tolerance)){R.Reason=TEXT("Tolerance arithmetic exceeds numeric range.");return R;}
        Max=FMath::Max(Max,E);Passed&=E<=Tolerance;
        if(Scale>0){ErrorSquared+=FMath::Square(E/Scale);RefSquared+=FMath::Square(Reference[I]/Scale);}
    }
    R.bEvaluated=true;R.bPassed=Passed;R.MaximumAbsoluteError=Max;
    if(RefSquared>0){const double Norm=FMath::Sqrt(ErrorSquared/RefSquared);if(FMath::IsFinite(Norm))R.RelativeL2Error=Norm;}
    R.Reason=Passed?TEXT("Every paired sample meets the supplied absolute/relative tolerance."):TEXT("At least one paired sample exceeds the supplied tolerance.");return R;
}
TOptional<double> StudioHome4Recipes::ObservedOrder(double Coarse,double Medium,double Fine,double Refinement)
{
    if(!FMath::IsFinite(Coarse)||!FMath::IsFinite(Medium)||!FMath::IsFinite(Fine)||!FMath::IsFinite(Refinement)||Refinement<=1)return {};
    const double A=Coarse-Medium,B=Medium-Fine;
    if(!FMath::IsFinite(A)||!FMath::IsFinite(B)||A==0||B==0||FMath::Sign(A)!=FMath::Sign(B))return {};
    const double P=(FMath::Loge(FMath::Abs(A))-FMath::Loge(FMath::Abs(B)))/FMath::Loge(Refinement);
    return FMath::IsFinite(P)&&P>0?TOptional<double>(P):TOptional<double>();
}
