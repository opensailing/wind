#include "SStudioHome4Setup.h"
#include "SStudioHome4Regions.h"
#include "StudioHeadlessSlate.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4SetupUITestsLocal
{
FStudioHome4Spec Spec()
{
    FStudioHome4Spec S;S.Authoring.Primitive=TEXT("box");S.Authoring.PrimitiveSizeCells=FVector(4);S.Authoring.BodyId=TEXT("setup-body");
    S.Reference.LengthCells=4;S.Reference.SpeedCellsPerStep=.02;S.Fluids.Gravity=.001;S.Fluids.RhoHeavy=1;S.Fluids.RhoLight=.1;S.Fluids.NuHeavy=.01;S.Fluids.NuLight=.005;S.Fluids.Xi=4;
    S.Geometry.InitialPositionCells=FVector(8);S.Lattice.Extents=FIntVector(16);S.Authoring.WaterlineCells=8;S.Authoring.ZoneUnits=TEXT("body-lengths");
    S.Lattice.PadUp=1;S.Lattice.PadDown=2;S.Lattice.PadSide=.5;S.Lattice.Depth=2;S.Lattice.Air=2;S.Run.RampLength=2;S.Run.NoFrameAcceleration=false;
    S.Units.DxMeters=.01;S.Units.DtSeconds=.001;S.Units.DensityReferenceKgM3=1000;return S;
}
TSharedPtr<FStudioHome4Session> Draft(const TSharedPtr<FStudioModel>& Model,const FStudioHome4Spec& S)
{auto Session=MakeShared<FStudioHome4Session>(Model);Session->ApplyRecipe(TEXT("th01-hull"));FString Error;Session->Replace(S,Error);Session->Apply();return Session;}
struct FTank final:IAutomationLatentCommand
{
    FAutomationTestBase* Test;TSharedPtr<FStudioModel> Model;TSharedPtr<FStudioHome4Session> Session;TSharedPtr<FStudioHome4AuthoringSession> Authoring;double Deadline=FPlatformTime::Seconds()+15;
    explicit FTank(FAutomationTestBase* T):Test(T)
    {
        Model=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-setup-tank")/FGuid::NewGuid().ToString());Session=Draft(Model,Spec());Authoring=MakeShared<FStudioHome4AuthoringSession>(Session);Authoring->Request();
    }
    bool Update()override
    {
        Authoring->Poll();if(Authoring->IsPreparing()&&FPlatformTime::Seconds()<Deadline)return false;
        Test->TestFalse(TEXT("Scoped preparation finishes within bounded deadline"),Authoring->IsPreparing());const auto P=Authoring->Preview();Test->TestTrue(*Authoring->Status,P&&P->IsValid());if(!P)return true;
        auto Widget=SNew(SStudioHome4Setup).Session(Session).Authoring(Authoring).Page(TEXT("Lattice"));FStudioHeadlessSlate UI(*Test,Widget,FVector2D(900,600));UI.Press(TEXT("Home4Setup.tank"));
        FStudioHome4Spec S;FString Error;Test->TestTrue(*Error,Session->Build(S,Error));Test->TestTrue(TEXT("Native layout changes actual retained XYZ/body request"),S.Lattice.Extents&&*S.Lattice.Extents==FIntVector(16,8,16)&&S.Geometry.InitialPositionCells.Get(FVector::ZeroVector)==FVector(6,4,8));
        Test->TestTrue(TEXT("Native layout leaves applied state at original bounds until Apply"),Model->Project.Draft.Home4->Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(16));
        const FString Retained=StudioHome4Config::Serialize(S);UI.Press(TEXT("Home4Setup.tank"));Session->Build(S,Error);Test->TestEqual(TEXT("Stale prior prepared tank cannot mutate the retained request twice"),StudioHome4Config::Serialize(S),Retained);
        UI.Press(TEXT("Home4Setup.counts"));Session->Build(S,Error);Test->TestTrue(TEXT("Native count action records exact actual root product"),S.Multidomain.LevelCells==TArray<int64>{2048});
        Test->TestTrue(TEXT("Shared parent Apply saves the reviewed layout"),Session->Apply());Test->TestTrue(TEXT("One real case undo restores original extents"),Model->UndoCase()&&Model->Project.Draft.Home4->Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(16));
        return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SetupTankUI,"Studio.HeadlessUI.Home4.Setup.PreparedTankActionAndUndo",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SetupTankUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(StudioHome4SetupUITestsLocal::FTank(this));return true;}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SetupZoneUI,"Studio.HeadlessUI.Home4.Setup.NativeWidthsFacesAndCubicRamp",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SetupZoneUI::RunTest(const FString&)
{
    using namespace StudioHome4SetupUITestsLocal;auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-setup-zones")/FGuid::NewGuid().ToString());auto S=Spec();S.Zones.Sponge=1;S.Zones.ZoneStrength=.5;auto Session=Draft(M,S);
    auto Widget=SNew(SStudioHome4Setup).Session(Session).Page(TEXT("Boundaries & Zones"));FStudioHeadlessSlate UI(*this,Widget,FVector2D(1000,1100));UI.Press(TEXT("Home4Setup.zones"));FString Error;Session->Build(S,Error);
    TestTrue(TEXT("Native widths action changes actual shared retained region"),S.Authoring.Zones.Num()==1&&S.Authoring.Zones[0].Minimum==FVector(3,0,0)&&S.Authoring.Zones[0].Maximum==FVector(4));
    UI.Press(TEXT("Home4Setup.face.2"));UI.Press(TEXT("Home4Setup.periodic.true"));Session->Build(S,Error);TestTrue(TEXT("Named Y face action declares actual periodic pair"),S.Zones.PeriodicY.Get(false));
    UI.Press(TEXT("Home4Setup.face.5"));UI.Press(TEXT("Home4Setup.pin.true"));Session->Build(S,Error);TestTrue(TEXT("Named Z face action retains phase-wall pin request without inventing phase"),S.Zones.PinPhaseWalls.Get(false)&&!S.Zones.PhiTop&&!S.Zones.PhiBottom);
    UI.Type(TEXT("Home4Setup.step"),TEXT("200"));TestTrue(TEXT("Native ramp readout shows actual cubic midpoint U"),UI.Text(TEXT("Home4Setup.rampDetails")).Contains(TEXT("0.01 cells/step")));
    UI.Type(TEXT("Home4Setup.step"),TEXT("nan"));TestTrue(TEXT("Invalid preview step never shows prior point as current"),UI.Text(TEXT("Home4Setup.rampDetails")).Contains(TEXT("no stale ramp point")));
    TestTrue(TEXT("Wake comparison is based on declared units"),UI.Text(TEXT("Home4Setup.widthDetails")).Contains(TEXT("expected steady hull wake")));
    TestTrue(TEXT("Parent Apply saves region and faces together"),Session->Apply());TestTrue(TEXT("Face edit participates in actual case undo"),M->UndoCase()&&!M->Project.Draft.Home4->Zones.PeriodicY&&M->Project.Draft.Home4->Authoring.Zones.IsEmpty());
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SetupRegionUnitsUI,"Studio.HeadlessUI.Home4.Setup.RegionUnitsIdentityAndStaleEdits",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SetupRegionUnitsUI::RunTest(const FString&)
{
    using namespace StudioHome4SetupUITestsLocal;auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-setup-regions")/FGuid::NewGuid().ToString());auto S=Spec();FStudioHome4AuthoredZone Z;Z.Id=TEXT("original-zone");Z.Maximum=FVector(1);Z.Strength=.2;S.Authoring.Zones.Add(Z);auto Session=Draft(M,S);M->UnitDisplay=EStudioHome4UnitDisplay::Physical;
    {
        auto Widget=SNew(SStudioHome4Regions).Session(Session).Patches(false);FStudioHeadlessSlate UI(*this,Widget,FVector2D(800,850));UI.Press(TEXT("Home4Region.addZone"));FString Error;Session->Build(S,Error);
        TestEqual(TEXT("Add preserves existing body-length coordinate declaration"),S.Authoring.ZoneUnits,FString(TEXT("body-lengths")));TestTrue(TEXT("Full-tank new region uses declared body lengths rather than root counts"),S.Authoring.Zones.Num()==2&&S.Authoring.Zones[1].Maximum==FVector(4));
        UI.Type(TEXT("Home4Region.minimum"),TEXT("0.04,0.04,0.04"));UI.Type(TEXT("Home4Region.maximum"),TEXT("0.08,0.08,0.08"));UI.Press(TEXT("Home4Region.retain"));TestTrue(*Error,Session->Build(S,Error));
        TestTrue(TEXT("SI coordinates inverse-convert once to body lengths"),S.Authoring.Zones[1].Minimum.Equals(FVector(1),1.e-12)&&S.Authoring.Zones[1].Maximum.Equals(FVector(2),1.e-12));
        UI.Type(TEXT("Home4Region.strength"),TEXT("0.7"));Session->Set(TEXT("units.dxMeters"),TEXT("0.02"));UI.Press(TEXT("Home4Region.retain"));Session->Build(S,Error,true);
        TestTrue(TEXT("Changed unit map rejects stale retained editor text"),Session->HasPending()&&S.Authoring.Zones[1].Strength==0);
        UI.Press(TEXT("Home4Region.remove"));Session->Build(S,Error,true);TestEqual(TEXT("Remove cannot discard pending editor text"),S.Authoring.Zones.Num(),2);
    }
    {
        auto Widget=SNew(SStudioHome4Regions).Session(Session).Patches(false);FStudioHeadlessSlate UI(*this,Widget,FVector2D(800,850));TestEqual(TEXT("Pending typed strength survives page reconstruction"),UI.Text(TEXT("Home4Region.strength")),FString(TEXT("0.7")));UI.Press(TEXT("Home4Region.revert"));UI.Press(TEXT("Home4Region.previous"));
        UI.Type(TEXT("Home4Region.strength"),TEXT("0.6"));FString Error;Session->Build(S,Error,true);S.Authoring.Zones.RemoveAt(0);TestTrue(*Error,Session->Replace(S,Error));UI.Press(TEXT("Home4Region.retain"));Session->Build(S,Error,true);
        TestTrue(TEXT("Removed/reordered identity cannot send stale edit into neighbor"),S.Authoring.Zones.Num()==1&&S.Authoring.Zones[0].Strength==0&&Session->HasPending());
        UI.Press(TEXT("Home4Region.revert"));
    }
    return !HasAnyErrors();
}
#endif
