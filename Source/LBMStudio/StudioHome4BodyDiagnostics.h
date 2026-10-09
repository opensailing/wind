#pragma once
#include "StudioHome4Telemetry.h"
struct FStudioHome4QuasiStaticResult
{
    TOptional<double> HeaveMeters,PitchDegrees;
    FString Reason,Method;
};
namespace StudioHome4BodyDiagnostics
{
    /** Explicit SI conventions only: symmetric K*[heave_m,pitch_rad]=[Fz_N,My_Nm].
     * No implicit stiffness/axis/sign conventions or current draft inputs. */
    FStudioHome4QuasiStaticResult QuasiStatic(const FStudioHome4Sample& Sample,const FStudioHome4BodyMeasurement& Body);
}
