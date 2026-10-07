#include "StudioHome4Archive.h"
#include "StudioRecording.h"
#include "StudioPointRecording.h"
#include "StudioVolume.h"
#include "StudioModel.h"
#include "StudioScene.h"
#include "StudioWorkspace.h"
#include "StudioHeadlessSlate.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StudioHome4SliceFixtures.inl"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4SourceSliceTest,"Studio.Home4.Archive.OriginalPlanesWithoutExtrusion",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FHome4SourceSliceTest::RunTest(const FString&)
{
    const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())/TEXT("Automation")/(TEXT("Slices_")+FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Folder,true);struct FCleanup{FString P;~FCleanup(){IFileManager::Get().DeleteDirectory(*P,false,true);}}Cleanup{Folder};
    using namespace StudioHome4SliceFixtures;
    for(const auto& Pair:TArray<TPair<const TCHAR*,const TCHAR*>>{{TEXT("thin"),thin},{TEXT("xy"),xy},{TEXT("xz"),xz},{TEXT("yz"),yz},{TEXT("yx"),yx},{TEXT("zx"),zx},{TEXT("zy"),zy},{TEXT("xyz"),xyz},{TEXT("zxy"),zxy},{TEXT("yxz"),yxz}})
    {
        TArray<uint8> Bytes;FBase64::Decode(Pair.Value,Bytes);const FString Path=Folder/(FString(Pair.Key)+TEXT(".npz"));FFileHelper::SaveArrayToFile(Bytes,*Path);
        FStudioHome4ArchiveRequest R;R.Sources={StudioHome4Archives::Inspect(Path).Source};R.OutputParent=Folder;R.FolderName=Pair.Key;R.SourceURI=TEXT("urn:artificial-affine-slice-test");R.Attribution=TEXT("Artificial affine interpolation test; not solver output or an installed sample.");
        R.Mapping.AxisOrder=FString(Pair.Key)==TEXT("thin")?TEXT("xy"):Pair.Key;R.Mapping.MetadataOrder=TEXT("xyz");R.Mapping.CoordinateUnits=TEXT("lattice");R.Mapping.VelocityUnits=TEXT("lattice");R.Mapping.DxMeters=.1;R.Mapping.DtSeconds=.02;
        auto Result=StudioHome4Archives::Convert(R);if(!TestTrue(FString(Pair.Key)+TEXT(" planar source imports: ")+Result.Error,Result.bSuccess))continue;
        auto Open=StudioRecordings::Import(Result.RecordingJSON,0,{});if(!TestTrue(Open.Error,Open.Source.IsValid()))continue;
        const auto Volume=Open.Source->VolumeReconstruction();if(!TestTrue(TEXT("Source affine topology retained"),Volume&&Volume->OriginalGrid&&Volume->OriginalGrid->bPlanar))continue;
        TestEqual(TEXT("No nodes duplicated to fake a volume"),Open.Source->Descriptor().NodeCount,FString(Pair.Key)==TEXT("thin")?12:30);
        TestFalse(TEXT("Missing out-of-plane derivatives are not assumed zero"),Open.Source->Descriptor().Scalars.ContainsByPredicate([](const auto& F){return F.Id==TEXT("q");}));
        const auto Pressure=Open.Source->ReadScalarFrame(0,TEXT("p_star"));double PValue=0;
        const auto PCenter=Volume->SourceBounds.GetCenter();
        if(TestTrue(TEXT("Original pressure slice loads"),Pressure.Field.IsValid()))
        {
            TestTrue(TEXT("Stored pressure stays sampleable even on a two-node-wide slice"),Pressure.Field->SampleScalar(FVector(PCenter.X,PCenter.Z,PCenter.Y),TEXT("p_star"),PValue));
            TestEqual(TEXT("Original pressure value retained"),PValue,7.);
        }
        const auto Read=Open.Source->ReadScalarFrame(0,TEXT("ux"));if(!TestTrue(Read.Error,Read.Field.IsValid()))continue;
        const auto Center=Volume->SourceBounds.GetCenter();const FVector P(Center.X,Center.Z,Center.Y);double Value=0;
        TestTrue(TEXT("Bilinear sample on exact original plane"),Read.Field->SampleScalar(P,TEXT("ux"),Value));
        TestTrue(TEXT("Independent affine formula and physical scale"),FMath::IsNearlyEqual(Value,(Center.X+2*Center.Y+3*Center.Z)/.02,1.e-10));
        auto Off=Center;Off[Volume->OriginalGrid->PlaneAxis()]+=.001;
        TestFalse(TEXT("Out-of-plane sample unavailable"),Read.Field->SampleScalar(FVector(Off.X,Off.Z,Off.Y),TEXT("ux"),Value));
        TestTrue(TEXT("Plane original fixed coordinate retained"),Volume->SourceBounds.Min[Volume->OriginalGrid->PlaneAxis()]>0);
        if(FString(Pair.Key)==TEXT("thin"))
        {
            auto Model=MakeShared<FStudioModel>(Folder/TEXT("ui"));Model->Solver=Open.Source;
            TStrongObjectPtr<UWorld> World(UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,false));
            ON_SCOPE_EXIT{World->DestroyWorld(false);};
            auto* Scene=World->SpawnActor<AStudioScene>();Scene->Model=Model;Scene->ApplyCamera(Model->Project.Camera);
            auto Workspace=SNew(SStudioWorkspace).Model(Model).Scene(Scene);
            FStudioHeadlessSlate UI(*this,Workspace,FVector2D(1440,1000));
            TestTrue(TEXT("Planar display offers the real source plane"),UI.Exists(TEXT("OriginalSourcePlane")));
            TestFalse(TEXT("Planar display does not offer volume controls"),UI.Exists(TEXT("VolumeDisplay")));
            TestFalse(TEXT("Planar display cannot expose zero-span volume clipping"),UI.Exists(TEXT("VolumeSettings")));
            TestTrue(TEXT("Planar display retains optional original points"),UI.Exists(TEXT("SourcePoints")));
            const bool Before=Model->bCutPlane;
            if(UI.Press(TEXT("OriginalSourcePlane")))TestTrue(TEXT("Source plane control mutates real visibility"),Model->bCutPlane!=Before);
        }
    }
    return true;
}
#endif
