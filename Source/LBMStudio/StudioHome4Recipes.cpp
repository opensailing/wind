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
        if(Key==TEXT("recipeId")||Key==TEXT("lineageId")||Key==TEXT("version"))continue;
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
        Wave.Template.Fluids.Xi=5;Wave.Template.Fluids.Mobility=.02;
        Wave.Notes+=TEXT(" Nx=400, W/L=0.25, Lz/L=0.30, wave c_lat=0.015, slope=0.08. c_lat=0.02 is a documented failure; wave speed is not an inlet-speed substitute.");R.Add(Wave);
        auto Cylinder=Recipe(TEXT("colagrossi-wb"),TEXT("Colagrossi WB cylinder"),TEXT("Colagrossi 2018"),TEXT("run_cylinder3d_colagrossi.py"),TEXT("Colagrossi Cd, Cl and free-surface profiles"),TEXT("Drag, lift and surface profile agreement"));
        Cylinder.Template.Reference.Bond=200;Cylinder.Template.Fluids.PhaseSXY=1.8;Cylinder.Template.Fluids.GradientLimiter=true;
        Cylinder.Notes+=TEXT(" Use Fr-aware U and g=(U/Fr)^2/D; M=0.05 applies only when Fr<0.8. Sponge width is 8D.");R.Add(Cylinder);
        auto Osc=Recipe(TEXT("oscillating-cylinder"),TEXT("Oscillating cylinder · Dütsch"),TEXT("Dütsch oscillating cylinder"),TEXT("run_cylinder3d_oscillating.py"),TEXT("Dütsch force history"),TEXT("Force history agreement"));
        Osc.Template.Reference.Reynolds=100;Osc.Template.Geometry.BodyMotion=TEXT("forced-heave");Osc.Notes+=TEXT(" KC=5, U_max=0.04, domain=20D. Oscillation peak speed is not a steady inlet speed.");R.Add(Osc);
        auto Spin=Recipe(TEXT("couette-spin"),TEXT("Couette spin gate"),TEXT("Analytic Couette torque"),TEXT("run_cylinder3d_spinning.py"),TEXT("Analytic torque"),TEXT("Torque agreement with analytic reference"));
        Spin.Template.Geometry.BodyMotion=TEXT("forced-spin");Spin.Notes+=TEXT(" U_s=0.04, rotational Re=100, box=12D. Rotational and translational Reynolds numbers remain distinct.");R.Add(Spin);
        auto Magnus=Recipe(TEXT("magnus"),TEXT("Magnus"),TEXT("Literature anchor requires verification"),TEXT("run_md_magnus_patch.py"),TEXT("Lift reference not yet verified"),TEXT("Lift agreement; reference anchor unresolved"));
        Magnus.Template.Reference.Reynolds=100;Magnus.Template.Reference.SpeedCellsPerStep=.05;Magnus.Template.Geometry.BodyMotion=TEXT("forced-spin");Magnus.Notes+=TEXT(" 30D × 16D, ramped spin.");R.Add(Magnus);
        auto Sed=Recipe(TEXT("sedimentation"),TEXT("Sedimentation"),TEXT("Sedimenting cylinder"),TEXT("run_cylinder3d_sedimenting.py"),TEXT("Terminal velocity reference"),TEXT("Terminal velocity agreement"));
        Sed.Template.Geometry.BodyMotion=TEXT("free");Sed.Notes+=TEXT(" Body/fluid density ratio=1.25, Ga=19.6. This is not the heavy/light fluid density ratio.");R.Add(Sed);
        auto Barge=Recipe(TEXT("vugts-barge"),TEXT("Vugts barge · H2-c"),TEXT("Vugts added mass and damping"),TEXT("run_barge_roll.py"),TEXT("Vugts/BEM coefficient curves"),TEXT("Added mass and damping agreement"));
        Barge.Template.Geometry.BodyMotion=TEXT("forced-heave");Barge.Notes+=TEXT(" B/T=2, forced heave/roll.");R.Add(Barge);
        auto Hull=Recipe(TEXT("th01-hull"),TEXT("TH01 hull"),TEXT("SYRF TH01 tank and BEM"),TEXT("run_hull_speed.py"),TEXT("th01_tank_data.py; th01_bem_capytaine.py"),TEXT("Heave, trim and drag vs tank; BEM intercepts"));
        Hull.Template.Reference.LengthCells=256;Hull.Template.Geometry.BodyMotion=TEXT("fixed");Hull.Notes+=TEXT(" Choose Fr, Re_ref and measured G/Q/P zone preset. Static flotation uses run_hull_static.py.");R.Add(Hull);
        auto Foil=Recipe(TEXT("hydrofoil-parkin"),TEXT("Hydrofoil · Parkin"),TEXT("Parkin–Wu"),TEXT("run_hydrofoil_parkin.py"),TEXT("Parkin–Wu free-surface profile"),TEXT("Surface profile agreement"));
        Foil.Template.Reference.Froude=.95;Foil.Template.Reference.LengthCells=128;Foil.Template.Reference.Reynolds=4000;Foil.Notes+=TEXT(" Submergence h/c=1.8.");R.Add(Foil);
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
    if(Refinements.IsEmpty()||Refinements.Num()>12||!Base.Reference.LengthCells.IsSet()||!Base.Fluids.Xi.IsSet())
    {Error=TEXT("A ladder needs body length, interface width and 1–12 refinement factors.");return false;}
    TArray<FStudioHome4LadderRung> Result;int32 Previous=0;
    for(const int32 Scale:Refinements)
    {
        if(Scale<=Previous||Scale>64){Error=TEXT("Refinement factors must increase, from 1 to 64.");return false;}Previous=Scale;
        FStudioHome4LadderRung R;R.Refinement=Scale;R.Spec=Base;
        R.Spec.Reference.LengthCells=Base.Reference.LengthCells.GetValue()*Scale;R.Spec.Fluids.Xi=Base.Fluids.Xi.GetValue()*Scale;
        R.Spec.Multidomain.FixedCahnRefinement=true;R.Spec.Multidomain.RecipeCahn=Base.Fluids.Xi.GetValue()/Base.Reference.LengthCells.GetValue();
        if(Base.Units.DxMeters.IsSet())R.Spec.Units.DxMeters=Base.Units.DxMeters.GetValue()/Scale;
        if(Base.Units.DtSeconds.IsSet())R.Spec.Units.DtSeconds=Base.Units.DtSeconds.GetValue()/Scale;
        // Acoustic refinement retains U and dimensionless physics; nu, M, sigma scale with L, gravity inversely.
        for(auto Pair:{TPair<TOptional<double>*,const TOptional<double>*>(&R.Spec.Fluids.NuHeavy,&Base.Fluids.NuHeavy),{&R.Spec.Fluids.NuLight,&Base.Fluids.NuLight},{&R.Spec.Fluids.Mobility,&Base.Fluids.Mobility},{&R.Spec.Fluids.Sigma,&Base.Fluids.Sigma}})
            if(Pair.Value->IsSet())*Pair.Key=Pair.Value->GetValue()*Scale;
        if(Base.Fluids.Gravity.IsSet())R.Spec.Fluids.Gravity=Base.Fluids.Gravity.GetValue()/Scale;
        if(Base.Reference.TimeSteps.IsSet())R.Spec.Reference.TimeSteps=Base.Reference.TimeSteps.GetValue()*Scale;
        if(Base.Run.Steps.IsSet())
        {if(Base.Run.Steps.GetValue()>MAX_int64/Scale){Error=TEXT("Refinement step count overflows.");return false;}R.Spec.Run.Steps=Base.Run.Steps.GetValue()*Scale;}
        if(Base.Lattice.Extents.IsSet())
        {
            auto N=Base.Lattice.Extents.GetValue();for(int32 A=0;A<3;++A){if(N[A]>1048576/Scale){Error=TEXT("Refined lattice exceeds the supported axis count.");return false;}N[A]*=Scale;}R.Spec.Lattice.Extents=N;
        }
        R.Spec.Run.Tag=Base.Run.Tag+FString::Printf(TEXT("_r%d"),Scale);
        if(!StudioHome4Config::Validate(R.Spec,Error))return false;
        R.EstimatedSeconds=StudioHome4Config::Derive(R.Spec).EstimatedSeconds;Result.Add(MoveTemp(R));
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
        Max=FMath::Max(Max,E);Passed&=E<=AbsTol+RelTol*FMath::Abs(Reference[I]);
        if(Scale>0){ErrorSquared+=FMath::Square(E/Scale);RefSquared+=FMath::Square(Reference[I]/Scale);}
    }
    R.bEvaluated=true;R.bPassed=Passed;R.MaximumAbsoluteError=Max;
    if(RefSquared>0)R.RelativeL2Error=FMath::Sqrt(ErrorSquared/RefSquared);
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
