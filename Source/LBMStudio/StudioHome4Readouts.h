#pragma once
#include "StudioHome4Config.h"

/** Unit views of one quantity and its own immutable source map. Null maps expose
 * only dimensionless quantities; callers must never substitute a next-run draft. */
namespace StudioHome4Readouts
{
    /** Uses only explicit original field units; failed conversion leaves Out untouched. */
    bool ScalarValue(double Value,const FString& SourceUnit,EStudioHome4UnitDisplay Display,const FStudioHome4Spec* Map,double& Out,bool ToSource=false);
    FString ScalarUnit(const FString& SourceUnit,EStudioHome4UnitDisplay Display,const FStudioHome4Spec* Map);
    FString Value(double Value,EStudioHome4Quantity Quantity,EStudioHome4UnitDisplay From,
        EStudioHome4UnitDisplay To,const FStudioHome4Spec* SourceMap);
    FString Tooltip(double Value,EStudioHome4Quantity Quantity,EStudioHome4UnitDisplay From,
        const FStudioHome4Spec* SourceMap);
    FString Scalar(double Value,const FString& SourceUnit,EStudioHome4UnitDisplay To,const FStudioHome4Spec* SourceMap,bool Tooltip=false);
    FString Unit(EStudioHome4Quantity Quantity,EStudioHome4UnitDisplay Display);
    FString Time(int64 Step,const FStudioHome4Spec* SourceMap,TOptional<double> PhysicalTime={});
}
