#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFloatingPanesTest,"Studio.FloatingPanes.BoundsPersistenceAndIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioFloatingPanesTest::RunTest(const FString&)
{
    const auto Dir=FPaths::ProjectSavedDir()/TEXT("Automation/FloatingPanes")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir);const auto Before=StudioProjectIO::Serialize(M.SnapshotProject());const auto Revision=M.RenderIntentRevision;
    for(const auto Name:StudioFloatingPanes::Names())M.FloatingPanes.Add(Name,{FVector2D(.7,.9),true,true});
    M.SaveSession();FStudioModel Read(Dir);Read.OpenSession();
    TestEqual(TEXT("Every pane preference survives relaunch"),Read.FloatingPanes.Num(),5);
    for(const auto Name:StudioFloatingPanes::Names())
    {
        const auto* S=Read.FloatingPanes.Find(Name);if(!TestNotNull(TEXT("Restored pane"),S))return false;
        TestTrue(TEXT("Exact relative position and minimized state persist"),S->Position==FVector2D(.7,.9)&&S->bMoved&&S->bMinimized);
        for(const FVector2D View:{FVector2D(1200,600),FVector2D(740,360)})for(const FVector2D Pane:{FVector2D(450,58),FVector2D(190,170),FVector2D(90,24)})
        {
            const auto P=StudioFloatingPanes::Position(*S,View,Pane,{1,1},{0,0});
            TestTrue(TEXT("Dragging, resizing and restore retain entire pane"),P.X>=8&&P.Y>=8&&P.X+Pane.X<=View.X-8&&P.Y+Pane.Y<=View.Y-8);
        }
    }
    TestEqual(TEXT("Layout cannot dirty simulation document"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestEqual(TEXT("Layout cannot rebuild CFD"),M.RenderIntentRevision,Revision);
    FFileHelper::SaveStringToFile(TEXT("{\"viewportPanes\":{\"Tools\":{\"x\":-1,\"y\":999,\"moved\":true,\"minimized\":true}}}"),*(Dir/TEXT("StudioSession.json")));
    FStudioModel Invalid(Dir);Invalid.OpenSession();TestTrue(TEXT("Malformed layout cannot hide controls"),Invalid.FloatingPanes.IsEmpty());
    return true;
}
#endif
