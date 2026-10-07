#include "SStudioHome4Panel.h"
#include "StudioHeadlessSlate.h"
#include "StudioModel.h"
#include "StudioHome4Reports.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace
{
bool Home4Widget(FAutomationTestBase& Test,int32 Width)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-ui")/FGuid::NewGuid().ToString());
    auto Session=MakeShared<FStudioHome4Session>(M);Session->ApplyRecipe(TEXT("th01-hull"));
    const auto Camera=M->Project.Camera;const auto Frame=M->SelectedFrame;
    auto Panel=SNew(SStudioHome4Panel).Model(M).Session(Session).Page(TEXT("Fluids & Interface"));
    FStudioHeadlessSlate UI(Test,Panel,FVector2D(Width,740));
    if(!UI.Type(TEXT("reference.speedCellsPerStep"),TEXT("0.04"))||!UI.Type(TEXT("reference.mach"),TEXT("0.9"))||!UI.Press(TEXT("Home4Apply")))return false;
    Test.TestEqual(TEXT("Applied source-independent lattice speed"),M->Project.Draft.Home4->Reference.SpeedCellsPerStep.Get(0),.04);
    Test.TestFalse(TEXT("Numerically infeasible draft still saved for correction"),Session->IsDirty());
    if(!UI.Type(TEXT("reference.mach"),TEXT("nan"))||!UI.Press(TEXT("Home4Apply")))return false;
    Test.TestTrue(TEXT("Invalid draft remains"),Session->IsDirty());Test.TestEqual(TEXT("Invalid draft text retained"),UI.Text(TEXT("reference.mach")),FString(TEXT("nan")));
    if(!UI.Press(TEXT("Home4Revert")))return false;
    Test.TestEqual(TEXT("Revert restores exact applied text"),UI.Text(TEXT("reference.mach")),FString(TEXT("0.90000000000000002")));
    Test.TestTrue(TEXT("Camera independent"),StudioView::CameraEquals(Camera,M->Project.Camera));Test.TestEqual(TEXT("Source frame independent"),M->SelectedFrame,Frame);
    return UI.Inspect(FString::Printf(TEXT("home4-form-%d"),Width),{TEXT("reference.speedCellsPerStep"),TEXT("reference.mach"),TEXT("Home4Apply"),TEXT("Home4Revert"),TEXT("Home4Status"),TEXT("Home4Feasibility")});
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PanelCompact,"Studio.HeadlessUI.Home4.Compact",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4PanelCompact::RunTest(const FString&){return Home4Widget(*this,1040);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PanelWide,"Studio.HeadlessUI.Home4.Wide",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4PanelWide::RunTest(const FString&){return Home4Widget(*this,1280);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ReportTest,"Studio.Home4.Reports.AtomicBundleAndEscaping",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ReportTest::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-reports")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
    auto M=MakeShared<FStudioModel>(Root/TEXT("model"));FStudioHome4Session Session(M);Session.ApplyRecipe(TEXT("th01-hull"));
    M->Project.Name=TEXT("Hull_1 & 50% {report}");FString Path,Error;
    TestTrue(TEXT("New atomic report"),StudioHome4Reports::Export(Root,TEXT("test_viz"),M->SnapshotProject(),nullptr,Path,Error));
    FString Text;TestTrue(TEXT("Latex file written"),FFileHelper::LoadFileToString(Text,*(Path/TEXT("report.tex"))));
    TestTrue(TEXT("LaTeX escaped"),Text.Contains(TEXT("Hull\\_1 \\& 50\\% \\{report\\}")));
    TestTrue(TEXT("No invented gate pass"),Text.Contains(TEXT("not evaluated")));
    TestFalse(TEXT("Published report cannot overwrite"),StudioHome4Reports::Export(Root,TEXT("test_viz"),M->SnapshotProject(),nullptr,Path,Error));
    TestFalse(TEXT("Traversal cannot export"),StudioHome4Reports::Export(Root,TEXT("../escape"),M->SnapshotProject(),nullptr,Path,Error));
    return !HasAnyErrors();
}
#endif
