#include "StudioHome4RequestActions.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StudioHome4Recipes.h"
#include "StudioHome4Session.h"
#include "SStudioHome4Panel.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ActionsNative,"Studio.HeadlessUI.Home4.Authoring.SpecImportExportSmokeFigureAndSafeguards",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ActionsNative::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-request-actions")/FGuid::NewGuid().ToString(),Path=Root/TEXT("run_spec.json");auto Model=MakeShared<FStudioModel>(Root);auto Session=MakeShared<FStudioHome4Session>(Model);Session->ApplyRecipe(TEXT("th01-hull"));
    {auto Widget=SNew(SStudioHome4Panel).Model(Model).Session(Session).Page(TEXT("Run")).ImportPath(Path).ExportPath(Path);FStudioHeadlessSlate UI(*this,Widget,FVector2D(1040,800));UI.Press(TEXT("Home4Export"));FStudioHome4Spec Saved;FString Error;TestTrue(*Error,StudioHome4Config::Load(Path,Saved,Error));TestEqual(TEXT("Native export writes exact applied request"),StudioHome4Config::Serialize(Saved),StudioHome4Config::Serialize(*Model->Project.Draft.Home4));
        Saved.Run.Tag=TEXT("imported-request");StudioProjectIO::WriteAtomic(Path,StudioHome4Config::Serialize(Saved),Error);UI.Press(TEXT("Home4Import"));TestEqual(TEXT("Native import commits verified request"),Model->Project.Draft.Home4->Run.Tag,FString(TEXT("imported-request")));
        const auto Keep=StudioHome4Config::Serialize(*Model->Project.Draft.Home4);FFileHelper::SaveStringToFile(TEXT("{\"version\":1,\"version\":2}"),*Path);UI.Press(TEXT("Home4Import"));TestEqual(TEXT("Malformed native import keeps prior applied request"),StudioHome4Config::Serialize(*Model->Project.Draft.Home4),Keep);
        UI.Press(TEXT("Home4SmokePreset"));FStudioHome4Spec Draft;TestTrue(*Error,Session->Build(Draft,Error));TestTrue(TEXT("Smoke action gives concrete reviewed finite duration/cadences"),Draft.Run.Smoke.Get(false)&&Draft.Run.Steps==256&&Draft.Run.MeasureEvery==64&&Draft.Run.VizEvery==64&&Draft.Run.OutDirectory.IsEmpty());UI.Press(TEXT("Home4Export"));TestTrue(TEXT("Dirty request blocks export"),Session->Status.Contains(TEXT("Apply or revert")));UI.Press(TEXT("Home4Apply"));UI.Press(TEXT("Home4FigurePreset"));TestTrue(*Error,Session->Build(Draft,Error));TestTrue(TEXT("Figure action makes explicit _viz request and clears reused destinations"),Draft.Run.Tag.EndsWith(TEXT("_viz"))&&!Draft.Run.Smoke.Get(true)&&Draft.Run.VizDirectory.IsEmpty());UI.Press(TEXT("Home4Apply"));}
    {auto Widget=SNew(SStudioHome4Panel).Model(Model).Session(Session).Page(TEXT("Fluids & Interface"));FStudioHeadlessSlate UI(*this,Widget,FVector2D(1040,800));UI.Press(TEXT("Home4SafeguardsPreset"));FStudioHome4Spec Draft;FString Error;TestTrue(*Error,Session->Build(Draft,Error));TestTrue(TEXT("Documented safeguards action retains exact values"),Draft.Fluids.GradientLimitFactor==1.6&&Draft.Fluids.ForceThresholdFactor==60&&Draft.Fluids.LightForceFactor==.6&&Draft.Fluids.LightPhaseCutoff==.1);}
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4EveryRequestProtocol,"Studio.Home4.Authoring.AllRecipeProtocolsRetainEveryTypedInput",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4EveryRequestProtocol::RunTest(const FString&)
{
    for(const auto& R:StudioHome4Recipes::All()){auto S=R.Template;S.Run.BlockShape=FIntVector(16,8,4);S.Authoring.DeviceProfile=TEXT("M4-Pro-MPS-79.5M");S.Geometry.InertiaFrame=TEXT("source-xyz");const auto Text=StudioHome4RequestActions::CompleteProtocol(S);TSharedPtr<FJsonObject> O;TestTrue(TEXT("Every recipe exposes complete parseable frontend request"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O));if(!O)continue;const TSharedPtr<FJsonObject>* Spec=nullptr;FStudioHome4Spec Parsed;FString E;TestTrue(*E,O->TryGetObjectField(TEXT("run_spec"),Spec)&&StudioHome4Config::FromJSON(*Spec,Parsed,E));TestEqual(TEXT("Complete typed protocol retains exact original recipe and all edited flags"),StudioHome4Config::Serialize(Parsed),StudioHome4Config::Serialize(S));TestEqual(TEXT("Protocol identifies actual driver without inventing argument encoding"),O->GetStringField(TEXT("driver")),R.Driver);}
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-device-inputs")/FGuid::NewGuid().ToString());auto Draft=MakeShared<FStudioHome4Session>(M);Draft->ApplyRecipe(TEXT("th01-hull"));auto Panel=SNew(SStudioHome4Panel).Model(M).Session(Draft).Page(TEXT("Run"));FStudioHeadlessSlate UI(*this,Panel,FVector2D(1040,800));UI.Type(TEXT("run.blockShape"),TEXT("16,8,4"));UI.Press(TEXT("authoring.deviceProfile"));UI.Press(TEXT("authoring.deviceProfile.M4-Pro-MPS-79.5M"));UI.Press(TEXT("Home4Apply"));TestTrue(TEXT("Native device/block controls persist actual request"),M->Project.Draft.Home4->Run.BlockShape==FIntVector(16,8,4)&&M->Project.Draft.Home4->Authoring.DeviceProfile==TEXT("M4-Pro-MPS-79.5M"));return !HasAnyErrors();
}

#endif
