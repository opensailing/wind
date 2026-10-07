#include "StudioHome4RequestActions.h"
#include "StudioHome4Recipes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
bool StudioHome4RequestActions::Smoke(const FStudioHome4Spec& Input,int64 Steps,int64 Every,FStudioHome4Spec& Out,FString& Error)
{
    if(Steps<1||Steps>1000000||Every<1||Every>Steps){Error=TEXT("Choose smoke steps from 1–1M and a cadence within that duration.");return false;}FStudioHome4Spec S=Input;
    S.Run.Smoke=true;S.Run.Steps=Steps;S.Run.MeasureEvery=Every;S.Run.PrintEvery=Every;S.Run.SaveEvery=Every;S.Run.VizEvery=Every;S.Run.RestartEvery.Reset();
    if(!S.Run.Tag.EndsWith(TEXT("_smoke")))S.Run.Tag=(S.Run.Tag.IsEmpty()?TEXT("home4"):S.Run.Tag)+TEXT("_smoke");
    S.Run.OutDirectory.Empty();S.Run.VizDirectory.Empty();S.Run.SaveState.Empty();if(!StudioHome4Config::Validate(S,Error))return false;Out=MoveTemp(S);return true;
}
bool StudioHome4RequestActions::Figure(const FStudioHome4Spec& Input,FStudioHome4Spec& Out,FString& Error)
{
    FStudioHome4Spec S=Input;if(!S.Run.Tag.EndsWith(TEXT("_viz")))S.Run.Tag=(S.Run.Tag.IsEmpty()?TEXT("home4"):S.Run.Tag)+TEXT("_viz");S.Run.Smoke=false;
    S.Run.OutDirectory.Empty();S.Run.VizDirectory.Empty();S.Run.SaveState.Empty();if(!StudioHome4Config::Validate(S,Error))return false;Out=MoveTemp(S);return true;
}
bool StudioHome4RequestActions::Safeguards(const FStudioHome4Spec& Input,FStudioHome4Spec& Out,FString& Error)
{
    FStudioHome4Spec S=Input;S.Fluids.GradientLimiter=true;S.Fluids.ForceThresholding=true;S.Fluids.GradientLimitFactor=1.6;S.Fluids.ForceThresholdFactor=60;S.Fluids.LightForceFactor=.6;S.Fluids.LightPhaseCutoff=.1;
    if(!StudioHome4Config::Validate(S,Error))return false;Out=MoveTemp(S);return true;
}
FString StudioHome4RequestActions::CompleteProtocol(const FStudioHome4Spec& S)
{
    const auto* R=StudioHome4Recipes::Find(S.RecipeId);auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("protocol"),TEXT("home4-frontend-request/1"));O->SetStringField(TEXT("driver"),R?R->Driver:TEXT("undeclared"));O->SetStringField(TEXT("execution"),TEXT("development adapter; numerical solver execution is stubbed"));O->SetObjectField(TEXT("run_spec"),StudioHome4Config::ToJSON(S));
    FString Text;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));return Text;
}
