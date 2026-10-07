#include "StudioHome4FieldInventory.h"
#include "Dom/JsonObject.h"
#include <cmath>

bool StudioHome4FieldInventory::Advanced(const FString& Id)
{return Id.StartsWith(TEXT("a3_"))||Id.StartsWith(TEXT("a4_"))||Id.StartsWith(TEXT("sneq"))||Id.EndsWith(TEXT("_phi"))||Id.StartsWith(TEXT("phase_"));}
FString StudioHome4FieldInventory::Group(const FString& Id)
{
    if(Advanced(Id))return TEXT("Advanced · kinetic and phase moments");
    if(Id==TEXT("pressure")||Id==TEXT("p_star")||Id==TEXT("Pi_h")||Id==TEXT("Pi_h0"))return TEXT("Pressure decomposition");
    if(Id.StartsWith(TEXT("F_")))return TEXT("Force terms");
    if(Id.StartsWith(TEXT("tau"))||Id.StartsWith(TEXT("log10_tau")))return TEXT("Relaxation margin");
    if(Id.StartsWith(TEXT("grad_phi"))||Id.StartsWith(TEXT("limiter"))||Id.StartsWith(TEXT("threshold"))||Id==TEXT("phi"))return TEXT("Interface and safeguards");
    if(Id.StartsWith(TEXT("md_"))||Id.Contains(TEXT("band"))||Id.Contains(TEXT("ghost")))return TEXT("Multidomain topology");
    return TEXT("Flow fields");
}

bool StudioHome4FieldInventory::Augment(TMap<FString,TArray<double>>& Fields,TMap<FString,FString>& Units,
    TMap<FString,FString>& Expressions,TSet<FString>& Derived,TMap<FString,FString>& Validity,
    const FJsonObject* Spec,TOptional<double> Dx,const FStudioLoadCancellation& Cancel,FString& Error)
{
    if(Fields.IsEmpty())return true;
    const int32 Count=Fields.CreateConstIterator().Value().Num();
    auto Data=[&](const FString& Id)->const double*{const auto* V=Fields.Find(Id);return V&&V->Num()==Count?V->GetData():nullptr;};
    auto Add=[&](const FString& Id,const FString& Unit,const FString& Expression,const FString& Mask,TFunction<double(int32)> Value)
    {
        if(Fields.Contains(Id))return true; // Supplied arrays keep their original provenance.
        if(Fields.Num()>=MaximumFields){Error=TEXT("Diagnostic catalogue exceeds 128 fields.");return false;}
        TArray<double> Values;Values.SetNumUninitialized(Count);
        for(int32 I=0;I<Count;++I)
        {
            if((I&4095)==0&&Cancel&&Cancel->load()){Error=TEXT("Diagnostic calculation cancelled.");return false;}
            Values[I]=Value(I);if(!FMath::IsFinite(Values[I])){Error=TEXT("Original diagnostic overflows: ")+Id;return false;}
        }
        Fields.Add(Id,MoveTemp(Values));Units.Add(Id,Unit);Expressions.Add(Id,Expression);Derived.Add(Id);
        if(!Mask.IsEmpty())Validity.Add(Id,Mask);return true;
    };
    if(const double* Tau=Data(TEXT("tau_fld"));Tau&&Units.FindRef(TEXT("tau_fld"))==TEXT("1"))
    {
        if(!Add(TEXT("tau_margin"),TEXT("1"),TEXT("Original tau_fld - 0.5"),{},[=](int32 I){return Tau[I]-.5;})||
            !Add(TEXT("tau_margin_valid"),TEXT("1"),TEXT("Original tau_fld > 0.5; positive log-domain mask"),{},[=](int32 I){return Tau[I]>.5?1.:0.;})||
            !Add(TEXT("log10_tau_margin"),TEXT("1"),TEXT("log10(original tau_fld - 0.5); nonpositive margins masked, not clamped"),TEXT("tau_margin_valid"),[=](int32 I){return Tau[I]>.5?std::log10(Tau[I]-.5):0.;}))return false;
    }
    if(const double* Omega=Data(TEXT("vorticity_magnitude"));Omega)
    {
        const FString Rate=Units.FindRef(TEXT("vorticity_magnitude"));const FString Squared=Rate==TEXT("1/s")?TEXT("1/s2"):(Rate==TEXT("1/step")||Rate==TEXT("(lu_velocity)/(lu_length)"))?TEXT("1/step2"):TEXT("(")+Rate+TEXT(")^2");
        if(!Add(TEXT("enstrophy"),Squared,TEXT("0.5 * original-grid vorticity_magnitude^2"),TEXT("derivative_valid"),[=](int32 I){return .5*Omega[I]*Omega[I];}))return false;
        const TCHAR* Names[]={TEXT("Sxx"),TEXT("Syy"),TEXT("Szz"),TEXT("Sxy"),TEXT("Sxz"),TEXT("Syz")};const double* S[6];bool Complete=true;
        for(int32 I=0;I<6;++I){S[I]=Data(Names[I]);Complete&=S[I]!=nullptr&&(Units.FindRef(Names[I])==Rate||(Rate==TEXT("(lu_velocity)/(lu_length)")&&Units.FindRef(Names[I])==TEXT("1/step")));}
        if(Complete)
        {
            TArray<const double*> Tensor;for(const double* V:S)Tensor.Add(V);
            if(!Add(TEXT("q_stored_strain"),Squared,TEXT("0.25*|omega|^2 - 0.5*(Sxx^2+Syy^2+Szz^2+2*(Sxy^2+Sxz^2+Syz^2)); original stored S and original-grid curl"),TEXT("derivative_valid"),[=](int32 I){double Norm=0;for(int32 C=0;C<6;++C)Norm+=Tensor[C][I]*Tensor[C][I]*(C<3?1:2);return .25*Omega[I]*Omega[I]-.5*Norm;}))return false;
        }
    }
    auto Magnitude=[&](const FString& Id,const TArray<FString>& Components,const FString& Description)
    {
        if(Components.IsEmpty())return true;TArray<const double*> Values;FString Unit=Units.FindRef(Components[0]);
        for(const auto& K:Components){auto* V=Data(K);if(!V||Units.FindRef(K)!=Unit)return true;Values.Add(V);}
        return Add(Id,Unit,Description,{},[Values](int32 I){double Norm=0;for(const double* V:Values)Norm=std::hypot(Norm,V[I]);return Norm;});
    };
    for(const auto& Pair:TArray<TPair<FString,int32>>{{TEXT("a3_"),7},{TEXT("a4_"),6}})
    {
        TArray<FString> Components;for(const auto& F:Fields)if(F.Key.StartsWith(Pair.Key)&&!Derived.Contains(F.Key)&&!F.Key.EndsWith(TEXT("magnitude")))Components.Add(F.Key);Components.Sort();
        if(Components.Num()==Pair.Value&&!Magnitude(Pair.Key+TEXT("magnitude"),Components,TEXT("Euclidean component magnitude of original ")+FString::Join(Components,TEXT(", "))+TEXT("; not an assumed tensor invariant")))return false;
    }
    if(!Magnitude(TEXT("phase_J_magnitude"),{TEXT("Jx_phi"),TEXT("Jy_phi"),TEXT("Jz_phi")},TEXT("Euclidean norm of original phase-flux components"))||
        !Magnitude(TEXT("phase_P_magnitude"),{TEXT("Pxx_phi"),TEXT("Pyy_phi"),TEXT("Pzz_phi"),TEXT("Pxy_phi"),TEXT("Pxz_phi"),TEXT("Pyz_phi")},TEXT("Euclidean norm of six original phase-moment components; no tensor multiplicity assumed")))return false;
    TArray<FString> ForceGroups;for(const auto& F:Fields)if(F.Key.StartsWith(TEXT("F_"))&&F.Key.EndsWith(TEXT("_x")))ForceGroups.Add(F.Key.LeftChop(2));
    for(const auto& Prefix:ForceGroups)if(!Magnitude(Prefix+TEXT("_magnitude"),{Prefix+TEXT("_x"),Prefix+TEXT("_y"),Prefix+TEXT("_z")},TEXT("Euclidean norm of original force-term components")))return false;
    const double* GX=Data(TEXT("grad_phi_x")),*GY=Data(TEXT("grad_phi_y")),*GZ=Data(TEXT("grad_phi_z")),*Phi=Data(TEXT("phi"));
    const FString GradientUnit=Units.FindRef(TEXT("grad_phi_x"));double Xi=0;const TSharedPtr<FJsonObject>* Fluids=nullptr;
    const bool OriginalXi=Spec&&Spec->TryGetObjectField(TEXT("fluids"),Fluids)&&(*Fluids)->TryGetNumberField(TEXT("xi"),Xi)&&FMath::IsFinite(Xi)&&Xi>0;
    const bool LatticeGradient=GradientUnit==TEXT("1/cells")||GradientUnit==TEXT("1/cell")||GradientUnit==TEXT("1/lu_length");
    if(!Magnitude(TEXT("grad_phi_magnitude"),{TEXT("grad_phi_x"),TEXT("grad_phi_y"),TEXT("grad_phi_z")},TEXT("Euclidean magnitude of original phase gradient")))return false;
    if(GX&&GY&&GZ&&Phi&&OriginalXi&&GradientUnit==Units.FindRef(TEXT("grad_phi_y"))&&GradientUnit==Units.FindRef(TEXT("grad_phi_z"))&&(LatticeGradient||(GradientUnit==TEXT("1/m")&&Dx&&*Dx>0)))
    {
        if(!LatticeGradient)Xi*=*Dx;
        if(!Add(TEXT("interface_gradient_valid"),TEXT("1"),TEXT("0 < original phi < 1; tanh denominator is positive"),{},[=](int32 I){return Phi[I]>0&&Phi[I]<1?1.:0.;})||
            !Add(TEXT("grad_phi_tanh_ratio"),TEXT("1"),TEXT("|original grad_phi| / (4*phi*(1-phi)/original xi); original unit anchors only"),TEXT("interface_gradient_valid"),[=](int32 I){return Phi[I]>0&&Phi[I]<1?std::hypot(GX[I],GY[I],GZ[I])*Xi/(4*Phi[I]*(1-Phi[I])):0.;}))return false;
        const double* Ratio=Data(TEXT("grad_phi_tanh_ratio"));
        if(!Add(TEXT("limiter_engagement"),TEXT("1"),TEXT("Original-gradient tanh ratio > 1.6; criterion map, not a measured limiter firing event"),TEXT("interface_gradient_valid"),[=](int32 I){return Ratio[I]>1.6?1.:0.;}))return false;
    }
    return true;
}
