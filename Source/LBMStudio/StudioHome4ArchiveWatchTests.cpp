#include "StudioHome4ArchiveWatch.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StudioHome4SliceFixtures.inl"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4WatchTestPrivate
{
class FWorkflow final:public IAutomationLatentCommand
{
public:
    explicit FWorkflow(FAutomationTestBase& T):Test(T){}
    ~FWorkflow(){Watch.Stop();IFileManager::Get().DeleteDirectory(*Folder,false,true);}
    bool Update()override
    {
        if(Folder.IsEmpty())
        {
            Started=FPlatformTime::Seconds();Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())/TEXT("Automation")/(TEXT("Watch_")+FGuid::NewGuid().ToString(EGuidFormats::Digits));IFileManager::Get().MakeDirectory(*Folder,true);
            FBase64::Decode(StudioHome4SliceFixtures::xy,Bytes);TArray<uint8> Partial;Partial.Append(Bytes.GetData(),Bytes.Num()/2);FFileHelper::SaveArrayToFile(Partial,*(Folder/TEXT("slice.npz")));
            FFileHelper::SaveArrayToFile(Bytes,*(Folder/TEXT("ignored.partial.npz")));FString Error;Test.TestTrue(TEXT("Watch starts on an existing folder"),Watch.Start(Folder,Error));return false;
        }
        if(FPlatformTime::Seconds()-Started>15){Test.AddError(FString::Printf(TEXT("Directory watch exceeded 15-second bound at stage %d: %s"),Stage,*Watch.Notice()));return true;}
        Watch.Tick(Clock++);auto Complete=Watch.TakeCompleted();
        if(Stage==0)
        {
            Test.TestTrue(TEXT("Incomplete NPZ never delivered"),Complete.IsEmpty());
            if(Watch.Notice().StartsWith(TEXT("Holding incomplete"))){FFileHelper::SaveArrayToFile(Bytes,*(Folder/TEXT("slice.npz")));Stage=1;}
        }
        else if(Stage==1&&!Complete.IsEmpty())
        {
            Test.TestEqual(TEXT("Partial files excluded, one valid source delivered"),Complete.Num(),1);Test.TestEqual(TEXT("Exact source iteration retained"),Complete[0].OriginalStep.Get(-1),10);
            AcceptedHash=Complete[0].Source.SHA256;Test.TestEqual(TEXT("Verified SHA pinned"),AcceptedHash.Len(),64);
            FFileHelper::SaveStringToFile(TEXT("changed"),*(Folder/TEXT("slice.npz")));Stage=2;
        }
        else if(Stage==2)
        {
            Test.TestTrue(TEXT("Source mutation never replaces accepted evidence"),Complete.IsEmpty());
            if(Watch.Notice().Contains(TEXT("Previously accepted snapshot changed"))){Watch.Stop();Test.TestFalse(TEXT("Watch explicitly stops"),Watch.Active());return true;}
        }
        return false;
    }
private:
    FAutomationTestBase& Test;FStudioHome4ArchiveWatch Watch;FString Folder,AcceptedHash;TArray<uint8> Bytes;double Started=0,Clock=0;int32 Stage=0;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4WatchTest,"Studio.Home4.Archive.DirectoryWatchAtomicFilesAndMutation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FHome4WatchTest::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(StudioHome4WatchTestPrivate::FWorkflow(*this));return true;}
#endif
