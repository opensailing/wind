#include "SStudioHome4TankPresets.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Event.h"
#include "Widgets/Layout/SScrollBox.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "StudioHome4TankPresetFixtures.inl"
namespace StudioHome4TankPresetUITestsLocal
{
struct FImportGate
{
    FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
    FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
    std::atomic<bool> ReleasedBeforeDeadline{false};
    ~FImportGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
};
struct FNative final:IAutomationLatentCommand
{
    FAutomationTestBase& Test;FString Root,Chosen,Copy,Before;double Deadline=FPlatformTime::Seconds()+20;int32 Stage=0;
    TSharedPtr<FStudioModel> Model;TSharedPtr<FStudioHome4Session> Session;TSharedPtr<FStudioHome4AuthoringSession> Authoring;
    TSharedPtr<SStudioHome4TankPresets> Widget;TSharedPtr<SScrollBox> Scroll;TUniquePtr<FStudioHeadlessSlate> UI;
    TSharedPtr<FImportGate,ESPMode::ThreadSafe> ImportGate;
    explicit FNative(FAutomationTestBase& T):Test(T)
    {
        using namespace StudioHome4TankPresetFixtures;Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-tank-preset-native")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
        Chosen=Root/TEXT("G.json");Copy=Root/TEXT("export.json");FFileHelper::SaveStringToFile(Definition(TEXT("G"),Spec(true)),*Chosen,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        FFileHelper::SaveStringToFile(TEXT("{}"),*(Root/TEXT("bad.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        auto Clip=Spec(true);Clip.Lattice.Extents=FIntVector(4,12,10);Clip.Authoring.Zones[0].Minimum=FVector(3,0,0);Clip.Authoring.Zones[0].Maximum=FVector(4,12,10);Clip.Zones.Sponge=1;FFileHelper::SaveStringToFile(Definition(TEXT("G"),Clip),*(Root/TEXT("clip.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        Model=MakeShared<FStudioModel>(Root/TEXT("project"));Model->Project.Draft.Home4=Spec(false);Session=MakeShared<FStudioHome4Session>(Model);Authoring=MakeShared<FStudioHome4AuthoringSession>(Session);
        FString E;FStudioHome4Spec S;Session->Build(S,E);Before=StudioHome4Config::Serialize(S);
        Widget=SNew(SStudioHome4TankPresets).Session(Session).Authoring(Authoring).ImportPath([this](FString& P){P=Chosen;return true;}).ExportPath([this](FString& P){P=Copy;return true;});
        Scroll=SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Widget.ToSharedRef()];UI=MakeUnique<FStudioHeadlessSlate>(Test,Scroll.ToSharedRef(),FVector2D(700,650));
        Test.TestTrue(TEXT("Named slots and actions fit the bounded inspector host"),UI->Inspect(TEXT("home4-original-presets-missing"),{TEXT("Home4TankPreset.select.G"),TEXT("Home4TankPreset.select.Q"),TEXT("Home4TankPreset.select.P"),TEXT("Home4TankPreset.import"),TEXT("Home4TankPreset.cancel"),TEXT("Home4TankPreset.review"),TEXT("Home4TankPreset.apply"),TEXT("Home4TankPreset.export"),TEXT("Home4TankPreset.details")}));
        UI->Press(TEXT("Home4TankPreset.select.Q"));UI->Press(TEXT("Home4TankPreset.apply"));Session->Build(S,E);
        Test.TestEqual(TEXT("Undefined Q never invents values or changes current draft"),StudioHome4Config::Serialize(S),Before);Test.TestTrue(TEXT("Undefined Q is explicitly missing"),UI->Text(TEXT("Home4TankPreset.status")).Contains(TEXT("definition is missing")));
        UI->Press(TEXT("Home4TankPreset.select.G"));UI->Press(TEXT("Home4TankPreset.import"));
    }
    ~FNative(){if(ImportGate)ImportGate->Release->Trigger();UI.Reset();Scroll.Reset();Widget.Reset();Authoring.Reset();Session.Reset();Model.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);}
    bool Update()override
    {
        using namespace StudioHome4TankPresetFixtures;Widget->PollImport();Authoring->Poll();UI->Layout();if(FPlatformTime::Seconds()>Deadline){Test.AddError(TEXT("Native preset workflow exceeded bounded deadline."));return true;}
        if(Stage==9)
        {
            if(!ImportGate->Reached->Wait(0))return false;
            Test.TestTrue(TEXT("Cancellation acts on an import still held at verification"),Widget->IsImporting());
            Test.TestTrue(TEXT("Cancel action remains arranged during the bounded read"),UI->Inspect(TEXT("home4-original-presets-cancel"),{TEXT("Home4TankPreset.cancel"),TEXT("Home4TankPreset.details")}));
            UI->Press(TEXT("Home4TankPreset.cancel"));ImportGate->Release->Trigger();Widget->SetImportVerificationForAutomation({});Stage=2;return false;
        }
        if(Stage==10)
        {
            if(!ImportGate->Reached->Wait(0))return false;
            Test.TestTrue(TEXT("Project switch occurs while original import is still held"),Widget->IsImporting());
            Model->Project.Id=FGuid::NewGuid();Widget->PollImport();ImportGate->Release->Trigger();Widget->SetImportVerificationForAutomation({});Stage=4;return false;
        }
        if(Widget->IsImporting())return false;FString E;FStudioHome4Spec S;Session->Build(S,E,true);
        if(Stage==0)
        {
            Test.TestTrue(TEXT("Native import exposes supplied exact zone and source values"),UI->Text(TEXT("Home4TankPreset.details")).Contains(TEXT("original-downstream"))&&UI->Text(TEXT("Home4TankPreset.details")).Contains(TEXT("SHA256")));
            Test.TestTrue(TEXT("Imported payload review and actions remain bounded"),UI->Inspect(TEXT("home4-original-presets-reviewed"),{TEXT("Home4TankPreset.import"),TEXT("Home4TankPreset.cancel"),TEXT("Home4TankPreset.review"),TEXT("Home4TankPreset.apply"),TEXT("Home4TankPreset.export"),TEXT("Home4TankPreset.details")}));
            const auto Details=UI->Find(TEXT("Home4TankPreset.details"));Test.TestTrue(TEXT("Full JSON review uses a bounded internal scroll area"),Details&&Details->GetCachedGeometry().GetLocalSize().Y<=170.1);
            Session->Set(TEXT("fluids.nuHeavy"),TEXT("0.012"));UI->Press(TEXT("Home4TankPreset.apply"));Session->Build(S,E);Test.TestTrue(TEXT("Changed draft rejects stale reviewed preset"),S.Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(16)&&UI->Text(TEXT("Home4TankPreset.status")).Contains(TEXT("Draft changed")));
            UI->Press(TEXT("Home4TankPreset.review"));UI->Press(TEXT("Home4TankPreset.apply"));
            Stage=1;return false;
        }
        if(Stage==1)
        {
            Test.TestTrue(TEXT("Native retain copies original tank and real region into shared request"),S.Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(20,12,10)&&S.Authoring.Zones.Num()==1&&S.Authoring.Zones[0].Minimum==FVector(16,0,0)&&S.Authoring.TankZonePresetName==TEXT("G")&&S.Fluids.NuHeavy.Get(0)==.012);
            if(Authoring->IsPreparing())return false;const auto P=Authoring->Preview();Test.TestTrue(TEXT("Actual prepared tank/zone geometry uses retained preset"),P&&P->IsValid()&&P->Tank.Max==FVector(20,12,10)&&P->Regions.ContainsByPredicate([](const auto& R){return R.Id==TEXT("original-downstream")&&R.Bounds.Min==FVector(16,0,0)&&R.Bounds.Max==FVector(20,12,10);}));
            UI->Press(TEXT("Home4TankPreset.export"));TArray<uint8> Original,Exported;FFileHelper::LoadFileToArray(Original,*Chosen);FFileHelper::LoadFileToArray(Exported,*Copy);Test.TestTrue(TEXT("Native export writes exact original source bytes"),!Original.IsEmpty()&&Original==Exported);
            Test.TestTrue(TEXT("Shared Apply saves actual reviewed preset"),Session->Apply());Test.TestTrue(TEXT("Real case undo restores tank and removes inherited preset identity"),Model->UndoCase()&&Model->Project.Draft.Home4->Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(16)&&Model->Project.Draft.Home4->Authoring.TankZonePresetName.IsEmpty());Session->Refresh();
            auto Changed=Spec(true);Changed.Authoring.Zones[0].Strength=.7;FFileHelper::SaveStringToFile(Definition(TEXT("G"),Changed),*Chosen,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            UI->Press(TEXT("Home4TankPreset.review"));UI->Press(TEXT("Home4TankPreset.apply"));Stage=5;return false;
        }
        if(Stage==5)
        {
            Test.TestTrue(TEXT("Changed original file cannot apply the previously reviewed slot"),S.Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(16)&&UI->Text(TEXT("Home4TankPreset.status")).Contains(TEXT("changed since review")));
            FFileHelper::SaveStringToFile(Definition(TEXT("G"),Spec(true)),*Chosen,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);Chosen=Root/TEXT("clip.json");UI->Press(TEXT("Home4TankPreset.import"));Stage=6;return false;
        }
        if(Stage==6){UI->Press(TEXT("Home4TankPreset.apply"));Stage=7;return false;}
        if(Stage==7)
        {
            Test.TestTrue(TEXT("Shrink tank clips actual complete body despite origin/CoG on boundary and preserves draft"),S.Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(16)&&UI->Text(TEXT("Home4TankPreset.status")).Contains(TEXT("clip the complete prepared body")));
            Chosen=Root/TEXT("G.json");UI->Press(TEXT("Home4TankPreset.import"));Stage=8;return false;
        }
        if(Stage==8)
        {
            ImportGate=MakeShared<FImportGate,ESPMode::ThreadSafe>();Widget->SetImportVerificationForAutomation([Gate=ImportGate]{Gate->Reached->Trigger();Gate->ReleasedBeforeDeadline.store(Gate->Release->Wait(5000));});
            UI->Press(TEXT("Home4TankPreset.import"));Stage=9;return false;
        }
        if(Stage==2)
        {
            Test.TestTrue(TEXT("Cancellation test released its worker before the bounded wait expired"),ImportGate->ReleasedBeforeDeadline.load());
            Test.TestTrue(TEXT("Cancelled import leaves prior original slot and exact draft available"),UI->Text(TEXT("Home4TankPreset.details")).Contains(TEXT("original-downstream"))&&UI->Text(TEXT("Home4TankPreset.status")).Contains(TEXT("read cancelled"))&&S.Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(16));Chosen=Root/TEXT("bad.json");UI->Press(TEXT("Home4TankPreset.import"));Stage=3;return false;
        }
        if(Stage==3)
        {
            Test.TestTrue(TEXT("Invalid JSON leaves exact prior original slot available"),UI->Text(TEXT("Home4TankPreset.details")).Contains(TEXT("original-downstream")));Chosen=Root/TEXT("G.json");
            ImportGate=MakeShared<FImportGate,ESPMode::ThreadSafe>();Widget->SetImportVerificationForAutomation([Gate=ImportGate]{Gate->Reached->Trigger();Gate->ReleasedBeforeDeadline.store(Gate->Release->Wait(5000));});
            UI->Press(TEXT("Home4TankPreset.import"));Stage=10;return false;
        }
        Test.TestTrue(TEXT("Changed project cannot import or inherit original slot"),UI->Text(TEXT("Home4TankPreset.status")).Contains(TEXT("scope"))&&!UI->Text(TEXT("Home4TankPreset.details")).Contains(TEXT("original-downstream")));return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4TankPresetNative,"Studio.HeadlessUI.Home4.Setup.OriginalGQPSelectionImportApplyGeometryUndoAndScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4TankPresetNative::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(StudioHome4TankPresetUITestsLocal::FNative(*this));return true;}
#endif
