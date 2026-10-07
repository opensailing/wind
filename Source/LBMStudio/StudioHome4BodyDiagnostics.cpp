#include "StudioHome4BodyDiagnostics.h"
#include "StudioHome4SciencePresentation.h"
FStudioHome4QuasiStaticResult StudioHome4BodyDiagnostics::QuasiStatic(const FStudioHome4Sample& S,const FStudioHome4BodyMeasurement& B)
{
    FStudioHome4QuasiStaticResult R;R.Method=TEXT("Declared symmetric 2x2 K inverse; K*[heave_m,pitch_rad]=[Fz_N,My_Nm]");
    if(S.bNonfinite||B.StiffnessConvention!=TEXT("symmetric_heave_m_pitch_rad_load_Fz_N_My_Nm")||B.K33Unit!=TEXT("N/m")||B.K35Unit!=TEXT("N")||B.K55Unit!=TEXT("N m")||!B.K33||!B.K35||!B.K55)
    {R.Reason=TEXT("Explicit compatible SI stiffness components/convention unavailable; no quasi-static solve inferred.");return R;}
    const auto Force=StudioHome4SciencePresentation::Quantity(B.Forces.Fz,S,EStudioHome4Quantity::Force,EStudioHome4UnitDisplay::Physical);
    const auto Moment=StudioHome4SciencePresentation::Quantity(B.Forces.My,S,EStudioHome4Quantity::Moment,EStudioHome4UnitDisplay::Physical);
    if(!Force.Number||!Moment.Number||Force.bRawFallback||Moment.bRawFallback)
    {R.Reason=TEXT("Original force/moment conversion to SI unavailable.");return R;}
    const double Scale=FMath::Max(FMath::Abs(*B.K33),FMath::Max(FMath::Abs(*B.K35),FMath::Abs(*B.K55)));
    if(Scale<=0||!FMath::IsFinite(Scale)){R.Reason=TEXT("Zero/invalid stiffness matrix.");return R;}
    const double A=*B.K33/Scale,C=*B.K35/Scale,D=*B.K55/Scale,Det=A*D-C*C;
    if(!FMath::IsFinite(Det)||FMath::Abs(Det)<=1.e-12){R.Reason=TEXT("Singular or poorly conditioned supplied stiffness; quasi-static attitude unavailable.");return R;}
    const double F=*Force.Number/Scale,M=*Moment.Number/Scale;
    const double Heave=(D*F-C*M)/Det,Pitch=FMath::RadiansToDegrees((A*M-C*F)/Det);
    if(!FMath::IsFinite(Heave)||!FMath::IsFinite(Pitch)){R.Reason=TEXT("Quasi-static arithmetic exceeds finite range.");return R;}
    R.HeaveMeters=Heave;R.PitchDegrees=Pitch;R.Reason=TEXT("Calculated only from explicitly compatible original loads and stiffness. No tank agreement inferred.");return R;
}
