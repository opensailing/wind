#include "StudioHome4Recipes.h"
#include "StudioProject.h"
#include "StudioAssets.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ShippedRequests,"Studio.Home4.Authoring.ShippedRequestTemplatesHaveNoRecordingIdentity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ShippedRequests::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectContentDir()/TEXT("Samples/HOME4");FString Text;TSharedPtr<FJsonObject> Catalog;
    if(!TestTrue(TEXT("Shipped native request catalog is readable"),FFileHelper::LoadFileToString(Text,*(Root/TEXT("catalog.json")))&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Catalog)))return false;
    TestEqual(TEXT("Catalog type cannot be mistaken for scientific recording"),Catalog->GetStringField(TEXT("kind")),FString(TEXT("home4-request-template-catalog")));
    const auto& Templates=Catalog->GetArrayField(TEXT("templates"));TestEqual(TEXT("All ten documented recipe requests ship"),Templates.Num(),StudioHome4Recipes::All().Num());
    TSet<FString> Seen;
    for(const auto& V:Templates)
    {
        const auto O=V->AsObject();const FString Id=O->GetStringField(TEXT("recipeId")),Relative=O->GetStringField(TEXT("path"));const auto* Recipe=StudioHome4Recipes::Find(Id);
        TestTrue(TEXT("Every asset belongs to one supplied recipe"),Recipe&&!Seen.Contains(Id));Seen.Add(Id);if(!Recipe)continue;
        TestTrue(TEXT("Catalog names internal native projects"),Relative==Id+TEXT(".lbms"));FString SHA,Error;FStudioProject P;
        TestTrue(*Error,StudioAssets::HashFile(Root/Relative,MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false),SHA,Error));TestEqual(TEXT("Template bytes match source registry"),SHA,O->GetStringField(TEXT("sha256")));
        if(!TestTrue(*Error,StudioProjectIO::Load(Root/Relative,P,Error)))continue;
        TestTrue(TEXT("Request template contains no CFD identity, frames, jobs or completed run"),P.Dataset.IsEmpty()&&P.Recordings.IsEmpty()&&P.Runs.IsEmpty()&&P.JobHistory.IsEmpty()&&!P.bControlHarness&&P.SelectedFrame==0);
        TestTrue(TEXT("Template carries its actual typed HOME4 request"),P.Draft.Home4.IsSet());if(!P.Draft.Home4)continue;
        TestEqual(TEXT("Every supplied/default field matches the executable native catalog"),StudioHome4Config::Serialize(*P.Draft.Home4),StudioHome4Config::Serialize(Recipe->Template));
    }
    return !HasAnyErrors();
}
#endif
