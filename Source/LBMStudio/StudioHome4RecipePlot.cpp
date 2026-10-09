#include "StudioHome4RecipePlot.h"
TArray<FStudioHome4RecipePlotFamily> StudioHome4RecipePlot::Families(const FStudioHome4Spec& Active)
{
    TArray<FStudioHome4RecipePlotFamily> Families;
    for(const auto& Recipe:StudioHome4Recipes::All())
    {
        FStudioHome4RecipePlotFamily F;F.Id=Recipe.Id;const auto& S=Recipe.Template;const auto D=StudioHome4Config::Derive(S);F.Label=Recipe.Name;
        if(S.Reference.LengthCells&&D.Mach){F.Points.Add(FVector2D(*S.Reference.LengthCells,*D.Mach));F.bOriginalPoint=true;F.Reason=TEXT("Complete supplied original setup point.");}
        else if(D.Mach)
        {F.Points={{16,*D.Mach},{8192,*D.Mach}};F.Label+=TEXT(" · supplied U, varying L");F.Reason=TEXT("Horizontal family uses supplied reference inlet speed; length remains a user choice.");}
        else if(S.Reference.LengthCells)
        {F.Points={{*S.Reference.LengthCells,0},{*S.Reference.LengthCells,.3}};F.Label+=TEXT(" · supplied L, varying Ma");F.Reason=TEXT("Vertical family uses supplied reference resolution; Mach remains a user choice.");}
        else if(S.Reference.Reynolds)
        {
            const double Tau=StudioHome4Config::Derive(Active).TauHeavy.Get(.6);
            for(int32 I=0;I<=80;++I){const double L=FMath::Pow(2.,4+9.*I/80),Ma=*S.Reference.Reynolds*(Tau-.5)/(FMath::Sqrt(3.)*L);if(Ma>=0&&Ma<=.3)F.Points.Add(FVector2D(L,Ma));}
            F.Label+=FString::Printf(TEXT(" · Re %.5g, τ %.4g"),*S.Reference.Reynolds,Tau);F.Reason=TEXT("Parameter family at the explicitly selected/current heavy relaxation time, or display τ=0.6 if unselected; no validation coordinate is assigned.");
        }
        else
        {const auto Current=StudioHome4Config::Derive(Active);
            if(Active.Reference.LengthCells&&Current.Mach){F.Points.Add(FVector2D(*Active.Reference.LengthCells,*Current.Mach));F.Label+=TEXT(" · user-selected L/Ma");F.Reason=TEXT("Coordinates come from the active authoring sizing controls, not a supplied validated recipe point. The card's independent groups remain visible.");}
            else F.Reason=TEXT("Choose L and Ma in the working sizing controls to place this recipe's requested parameter family.");}
        Families.Add(MoveTemp(F));
    }
    return Families;
}
bool StudioHome4RecipePlot::WithinNumericalEnvelopeDerived(const FStudioHome4Spec& Active,const FStudioHome4Derived& D,double Length,double Mach)
{
    if(Length<16||Length>8192||Mach<=0||Mach>.1)return false;const auto Re=Active.Reference.Reynolds?Active.Reference.Reynolds:D.Reynolds;
    if(!Re||*Re<=0)return false;const double Tau=.5+FMath::Sqrt(3.)*Mach*Length/ *Re;if(Tau<.51||Tau>2)return false;
    if(D.NuHeavy&&D.NuLight&&*D.NuHeavy>0&&.5+(Tau-.5)* *D.NuLight/ *D.NuHeavy<.51)return false;
    return true;
}

bool StudioHome4RecipePlot::WithinNumericalEnvelope(const FStudioHome4Spec& Active,double Length,double Mach)
{return WithinNumericalEnvelopeDerived(Active,StudioHome4Config::Derive(Active),Length,Mach);}
