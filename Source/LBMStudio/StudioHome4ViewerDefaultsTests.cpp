#include "StudioHome4ViewerDefaults.h"
#include "StudioColor.h"
#include "SStudioHome4Settings.h"
#include "StudioHeadlessSlate.h"
#include "StudioModel.h"
#include "StudioHome4Recipes.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4GlobalDefaults,"Studio.Home4.Authoring.GlobalViewerDefaultsPreserveSourceRangesAndNewRequests",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4GlobalDefaults::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-global-defaults")/FGuid::NewGuid().ToString(),Path=Root/TEXT("Home4ViewerDefaults.json");FStudioViewSettings View;View.VolumeOpacity=.47;View.ScalarField=TEXT("original-q");View.bVolumeThreshold=true;View.VolumeThresholdMinimum=100;View.VolumeThresholdMaximum=200;
    FStudioScalarStyle Global;Global.Dataset=TEXT("*");Global.Field=TEXT("*");Global.Palette=2;View.ScalarStyles.Add(Global);FStudioScalarStyle Original;Original.Dataset=TEXT("old");Original.Field=TEXT("pressure");Original.bManualRange=true;Original.Minimum=100;Original.Maximum=200;View.ScalarStyles.Add(Original);
    FStudioCameraState Camera;Camera.bOrthographic=true;FString Error;TestTrue(*Error,StudioHome4ViewerDefaults::Save(Path,View,Camera,Error));FStudioProject Project;Project.Dataset.Empty();Project.Runs.Reset();Project.Draft.Home4=StudioHome4Recipes::Find(TEXT("th01-hull"))->Template;const auto Spec=StudioHome4Config::Serialize(*Project.Draft.Home4);
    TestTrue(*Error,StudioHome4ViewerDefaults::ApplyToNewProject(Project,Error,Path));TestTrue(TEXT("Global display defaults apply to actual new request"),Project.View.VolumeOpacity==.47&&Project.Camera.bOrthographic&&Project.bHasViewerDefaults);TestEqual(TEXT("Defaults never change numerical request"),StudioHome4Config::Serialize(*Project.Draft.Home4),Spec);
    TestTrue(TEXT("Original scalar identity/ranges do not relabel unrelated data"),Project.View.ScalarField.IsEmpty()&&!Project.View.bVolumeThreshold&&Project.View.ScalarStyles.Num()==1&&Project.View.ScalarStyles[0].Dataset==TEXT("*"));
    FStudioScalarDescriptor Field;Field.Id=TEXT("pressure");Field.Minimum=-2;Field.Maximum=3;auto Mapping=StudioColor::Resolve(TEXT("new-source"),Field,Project.View.ScalarStyles);TestTrue(TEXT("Global palette retains new source's actual range"),Mapping.Palette==2&&Mapping.Minimum==-2&&Mapping.Maximum==3&&!Mapping.bManualRange);
    Project.View.ScalarStyles.Add(Original);Mapping=StudioColor::Resolve(TEXT("old"),Field,Project.View.ScalarStyles);TestTrue(TEXT("Exact source style wins over global palette"),Mapping.bManualRange&&Mapping.Minimum==100&&Mapping.Maximum==200);
    Global.bManualRange=true;TestFalse(TEXT("Wildcard manual range cannot be imported as defaults"),StudioColor::IsValid(Global));FStudioViewSettings Keep;Keep.VolumeOpacity=.123;FStudioCameraState KeepCamera;FFileHelper::SaveStringToFile(TEXT("{\"format\":\"Home4ViewerDefaults\",\"format\":\"duplicate\"}"),*Path);TestFalse(TEXT("Malformed defaults fail transactionally"),StudioHome4ViewerDefaults::Load(Path,Keep,KeepCamera,Error));TestEqual(TEXT("Failed import keeps display"),Keep.VolumeOpacity,.123);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4GlobalDefaultsUI,"Studio.HeadlessUI.Home4.Authoring.SaveGlobalViewerDefaultsFromSettings",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4GlobalDefaultsUI::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-global-settings-ui")/FGuid::NewGuid().ToString(),Path=Root/TEXT("Home4ViewerDefaults.json");auto Model=MakeShared<FStudioModel>(Root);Model->BeginHome4Authoring();auto Widget=SNew(SStudioHome4Settings).Model(Model).DefaultsPath(Path);FStudioHeadlessSlate UI(*this,Widget,FVector2D(690,700));UI.Press(TEXT("Home4Settings.palette2"));UI.Press(TEXT("Home4Settings.saveGlobalDefaults"));FStudioViewSettings Loaded;FStudioCameraState Camera;FString Error;TestTrue(*Error,StudioHome4ViewerDefaults::Load(Path,Loaded,Camera,Error));TestTrue(TEXT("Empty request stores palette without a fake source style"),Loaded.ScalarStyles.Num()==1&&Loaded.ScalarStyles[0].Dataset==TEXT("*")&&Loaded.ScalarStyles[0].Palette==2);
    const auto Spec=StudioHome4Recipes::Find(TEXT("th01-hull"))->Template;TestTrue(*Model->Notice,Model->CreateProject(Root/TEXT("new.lbms"),TEXT("With global defaults"),&Spec));TestTrue(TEXT("Actual new project consumes the saved global defaults"),Model->Project.View.ScalarStyles.Num()==1&&Model->Project.View.ScalarStyles[0].Palette==2);
    return !HasAnyErrors();
}
#endif
