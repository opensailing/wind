#include "StudioSnapshotUI.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotSelection,"Studio.ImageSequenceUI.StrictOriginalSelection",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FSnapshotSelection::RunTest(const FString&)
{
    int32 A=17,B=19,S=23;FString Error;
    TestTrue(TEXT("One-based strided selection accepted"),StudioSnapshotUI::Selection(TEXT(" 1 "),TEXT("601"),TEXT("300"),601,A,B,S,Error));
    TestTrue(TEXT("Ordinals converted without changing stride"),A==0&&B==600&&S==300);
    TestTrue(TEXT("Inclusive endpoint need not be selected by stride"),StudioSnapshotUI::Selection(TEXT("3"),TEXT("10"),TEXT("4"),601,A,B,S,Error)&&A==2&&B==9&&S==4);
    for(const FString Bad:{TEXT(""),TEXT("0"),TEXT("-1"),TEXT("1.5"),TEXT("2e2"),TEXT("+1"),TEXT("2147483648"),TEXT("2x")})
    {
        TestFalse(TEXT("Invalid first frame rejected"),StudioSnapshotUI::Selection(Bad,TEXT("601"),TEXT("1"),601,A,B,S,Error));
        TestFalse(TEXT("Invalid stride rejected"),StudioSnapshotUI::Selection(TEXT("1"),TEXT("601"),Bad,601,A,B,S,Error));
    }
    TestFalse(TEXT("No silent range clamp"),StudioSnapshotUI::Selection(TEXT("1"),TEXT("602"),TEXT("1"),601,A,B,S,Error));
    TestFalse(TEXT("No backwards range"),StudioSnapshotUI::Selection(TEXT("5"),TEXT("4"),TEXT("1"),601,A,B,S,Error));
    TestFalse(TEXT("Image count bounded"),StudioSnapshotUI::Selection(TEXT("1"),TEXT("100001"),TEXT("1"),100001,A,B,S,Error));
    TestTrue(TEXT("Stride reduces export count"),StudioSnapshotUI::Selection(TEXT("1"),TEXT("100001"),TEXT("2"),100001,A,B,S,Error));
    return true;
}
#endif
