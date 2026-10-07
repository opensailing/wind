#include "StudioHome4TankPresets.h"
#include "StudioHome4Authoring.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "StudioHome4TankPresetFixtures.inl"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4TankPresetOriginal,"Studio.Home4.Setup.OriginalNamedTankPresetsAndTransactionalBounds",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4TankPresetOriginal::RunTest(const FString&)
{
    using namespace StudioHome4TankPresetFixtures;FString E;FStudioHome4TankZonePreset P;const auto Original=Spec(true);const FString Text=Definition(TEXT("G"),Original);
    TestTrue(*E,StudioHome4TankPresets::Parse(Text,P,E));const auto SHA=P.SourceSHA256;TestTrue(TEXT("Retains exact original UTF-8 and cryptographic identity"),P.OriginalBytes.Num()==FTCHARToUTF8(*Text).Length()&&SHA.Len()==64);
    auto Current=Spec(false);Current.Run.Tag=TEXT("current-output");Current.Geometry.BodyMass=17;Current.Fluids.NuHeavy=.012;Current.Multidomain.LevelCells={4096};Current.Performance.OutputByteEstimates={1,2,3,4};FStudioHome4Allocation Allocation;Allocation.Name=TEXT("explicit root array");Allocation.NodeScope=TEXT("root");Allocation.Nodes=4096;Current.Performance.Allocations.Add(Allocation);FStudioHome4Spec Out;
    TestTrue(*E,StudioHome4TankPresets::Apply(P,Current,Out,E));
    TestTrue(TEXT("Independent supplied XYZ and actual zone geometry retained"),Out.Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(20,12,10)&&Out.Lattice.PadUp.Get(0)==2&&Out.Zones.Sponge.Get(0)==4&&Out.Authoring.Zones.Num()==1&&Out.Authoring.Zones[0].Minimum==FVector(16,0,0)&&Out.Authoring.Zones[0].Maximum==FVector(20,12,10)&&Out.Authoring.Zones[0].Strength==.3);
    TestTrue(TEXT("Unrelated current mass, fluid and output remain exact"),Out.Geometry.BodyMass.Get(0)==17&&Out.Fluids.NuHeavy.Get(0)==.012&&Out.Run.Tag==TEXT("current-output"));TestTrue(TEXT("Changed root invalidates prior node/storage totals and refreshes root allocation"),Out.Multidomain.LevelCells.IsEmpty()&&Out.Performance.OutputByteEstimates.IsEmpty()&&Out.Performance.Allocations[0].Nodes.Get(0)==2400);
    TestTrue(TEXT("Applied exact payload identified"),StudioHome4TankPresets::MatchesApplied(Out,P));Out.Zones.Sponge=3;TestFalse(TEXT("Later zone edit cannot inherit exact source-preset claim"),StudioHome4TankPresets::MatchesApplied(Out,P));
    const FString Before=StudioHome4Config::Serialize(Out);auto Bad=P;Bad.OriginalSpec.Lattice.Extents=FIntVector(1);TestFalse(TEXT("Changed retained values cannot bypass original bytes"),StudioHome4TankPresets::Apply(Bad,Current,Out,E));TestEqual(TEXT("Failed apply preserves output"),StudioHome4Config::Serialize(Out),Before);
    Current.Reference.LengthCells=8;TestFalse(TEXT("Different physical body-length reference cannot reinterpret preset"),StudioHome4TankPresets::Apply(P,Current,Out,E));Current=Spec(false);FStudioHome4AuthoredPatch Patch;Patch.Id=TEXT("fine");Patch.BodyId=Current.Authoring.BodyId;Patch.Level=1;Patch.Origin=FVector(10);Patch.Extents=FIntVector(8);Current.Authoring.Patches.Add(Patch);
    TestFalse(TEXT("Tank preset cannot clip current independent patch"),StudioHome4TankPresets::Apply(P,Current,Out,E));
    auto Invalid=Original;Invalid.Authoring.Zones[0].Maximum.X=21;TestFalse(TEXT("Out-of-tank supplied zone rejected"),StudioHome4TankPresets::Parse(Definition(TEXT("Q"),Invalid),P,E));
    TestFalse(TEXT("Unknown named preset rejected"),StudioHome4TankPresets::Parse(Definition(TEXT("R"),Original),P,E));TestFalse(TEXT("Duplicate envelope keys rejected"),StudioHome4TankPresets::Parse(TEXT("{\"schema\":1,\"schema\":2}"),P,E));
    TSharedPtr<FJsonObject> Missing;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Missing);Missing->GetObjectField(TEXT("run_spec"))->RemoveField(TEXT("authoring"));TestFalse(TEXT("Absent authoring returns failure without dereference/assert"),StudioHome4TankPresets::Parse(JSON(Missing.ToSharedRef()),P,E));
    TestFalse(TEXT("Deep original definition bounded"),StudioHome4TankPresets::Parse(TEXT("{\"name\":")+FString::ChrN(40,'[')+TEXT("0")+FString::ChrN(40,']')+TEXT("}"),P,E));
    TestEqual(TEXT("Invalid imports preserve original prior slot"),P.SourceSHA256,SHA);
    for(const FString Name:{TEXT("G"),TEXT("Q"),TEXT("P")})
    {
        FStudioHome4TankZonePreset Named;FStudioHome4Spec Applied;TestTrue(TEXT("Each named slot parses an explicit supplied definition"),StudioHome4TankPresets::Parse(Definition(Name,Original),Named,E));TestTrue(TEXT("Each named slot applies its supplied values without named defaults"),StudioHome4TankPresets::Apply(Named,Spec(false),Applied,E)&&Applied.Authoring.TankZonePresetName==Name&&Applied.Lattice.Extents.Get(FIntVector::ZeroValue)==FIntVector(20,12,10));
    }
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4TankPresetFiles,"Studio.Home4.Setup.TankPresetOriginalFilesChangeCancelAndNoOverwrite",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4TankPresetFiles::RunTest(const FString&)
{
    using namespace StudioHome4TankPresetFixtures;const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-tank-preset-files")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Root,false,true);};
    const FString Path=Root/TEXT("original.json"),Dest=Root/TEXT("copy.json"),Text=Definition(TEXT("P"),Spec(true));TestTrue(TEXT("Write artificial supplied definition"),FFileHelper::SaveStringToFile(Text,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    FStudioHome4TankZonePreset P;FString E;TestTrue(*E,StudioHome4TankPresets::Load(Path,P,E));const auto Prior=P;auto C=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);TestFalse(TEXT("Cancellation preserves prior slot"),StudioHome4TankPresets::Load(Path,P,E,C));TestEqual(TEXT("Cancelled identity unchanged"),P.SourceSHA256,Prior.SourceSHA256);
    const auto Stamp=IFileManager::Get().GetTimeStamp(*Path);FString Changed=Text;Changed.ReplaceInline(TEXT("\"P\""),TEXT("\"Q\""));
    TestFalse(TEXT("Same-size preserved-timestamp rewrite is detected"),StudioHome4TankPresets::Load(Path,P,E,{},[&]{FFileHelper::SaveStringToFile(Changed,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);IFileManager::Get().SetTimeStamp(*Path,Stamp);}));TestTrue(TEXT("Rewritten input preserves prior original bytes"),P.OriginalBytes==Prior.OriginalBytes);
    TestTrue(*E,StudioHome4TankPresets::Export(P,Dest,E));TArray<uint8> Bytes;FFileHelper::LoadFileToArray(Bytes,*Dest);TestTrue(TEXT("Export is byte-exact original"),Bytes==Prior.OriginalBytes);TestFalse(TEXT("Original publication never overwrites destination"),StudioHome4TankPresets::Export(P,Dest,E));
    return !HasAnyErrors();
}
#endif
