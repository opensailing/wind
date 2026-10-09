#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectorSession,"Studio.Inspector.SessionCategoryIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioInspectorSession::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectSavedDir()/TEXT("Automation/InspectorSession")/FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*Dir,true);FStudioModel M(Dir);
    const FString Before=StudioProjectIO::Serialize(M.SnapshotProject());const auto Revision=M.RenderIntentRevision;
    for(int32 I=0;I<4;++I)
    {
        M.InspectorTab=I;M.SaveSession();FStudioModel Reopened(Dir);Reopened.OpenSession();
        TestEqual(TEXT("Inspector preference persists"),Reopened.InspectorTab,I);
    }
    TestEqual(TEXT("Category never edits the scientific document"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestEqual(TEXT("Category never invalidates field geometry"),M.RenderIntentRevision,Revision);
    for(const TCHAR* Value:{TEXT("-1"),TEXT("4"),TEXT("1.5"),TEXT("\"Display\"")})
    {
        FFileHelper::SaveStringToFile(FString(TEXT("{\"inspectorTab\":"))+Value+TEXT("}"),*(Dir/TEXT("StudioSession.json")));
        FStudioModel Reopened(Dir);Reopened.OpenSession();TestEqual(TEXT("Malformed preference uses Display"),Reopened.InspectorTab,3);
    }
    return true;
}
#endif
