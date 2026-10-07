#include "StudioHome4EnergyBudget.h"
#include "SStudioHome4EnergyBudget.h"
#include "SStudioHome4Monitors.h"
#include "StudioHome4Session.h"
#include "StudioHome4Reports.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "InputCoreTypes.h"
#define UI UI_HOME4_BUDGET_TEST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4EnergyBudgetFixtures
{
    FString Domains(){return TEXT("[{\"role\":\"near\",\"body_id\":\"original-body\",\"coordinate_unit\":\"body-lengths\",\"frame\":\"body-CoG-local-XYZ\",\"tracking\":\"follow-body\",\"region\":\"inside-box\",\"phase_mask\":\"source-water-phi-ge-0.5\",\"minimum\":[-1,-2,-3],\"maximum\":[1,2,3]},{\"role\":\"far\",\"body_id\":\"original-body\",\"coordinate_unit\":\"body-lengths\",\"frame\":\"body-CoG-local-XYZ\",\"tracking\":\"follow-body\",\"region\":\"shell-excluding-near\",\"phase_mask\":\"source-water-phi-ge-0.5\",\"minimum\":[-4,-5,-6],\"maximum\":[4,5,6]}]");}
    FString Log(){return TEXT("{\"kind\":\"source_metadata\",\"source_metadata\":{\"budget_domain_source\":\"identified original integration domains\",\"budget_domain_convention\":\"near=inside near; far=inside far excluding near; air=source-declared phase mask\",\"budget_domains\":")+Domains()+TEXT("}}\n{\"step\":1,\"budget\":{\"W\":10,\"D_near\":2,\"D_far\":3,\"D_air\":1,\"Z_beach\":1,\"Z_floor\":1,\"dKE\":1,\"dPE\":1,\"res\":0}}\n");}
    FStudioHome4TailResult Append(FStudioHome4TelemetryStream& S,const FString& JSON)
    {const FTCHARToUTF8 B(*JSON);return S.AppendBytes(reinterpret_cast<const uint8*>(B.Get()),B.Length());}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4EnergyBoxOriginalContract,"Studio.Home4.Telemetry.OriginalEnergyDomainsUnitsBindingAndReport",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4EnergyBoxOriginalContract::RunTest(const FString&)
{
    using namespace StudioHome4EnergyBudgetFixtures;
    FStudioHome4TelemetryStream Stream;const FGuid Run=FGuid::NewGuid();Stream.BeginRun({Run,TEXT("identified original energy-domain fixture")});
    TestEqual(TEXT("Explicit original domain metadata parses with measured terms"),Append(Stream,Log()).Malformed,int32(0));
    if(!TestTrue(TEXT("Original domains independently retained"),Stream.OriginalMetadata().IsValid()&&Stream.OriginalMetadata()->EnergyBudgetDomains.Num()==2&&Stream.Latest().IsSet()))return false;
    TestEqual(TEXT("Original declared body identity remains independent"),Stream.OriginalMetadata()->EnergyBudgetDomains[0].BodyId,FString(TEXT("original-body")));
    TestEqual(TEXT("Original water selection is explicitly source-defined"),Stream.OriginalMetadata()->EnergyBudgetDomains[0].PhaseMask,FString(TEXT("source-water-phi-ge-0.5")));
    TestTrue(TEXT("Omitted air remains explicitly unidentified even when air dissipation is measured"),StudioHome4EnergyBudget::Description(Stream.OriginalMetadata()->EnergyBudgetDomains).Contains(TEXT("air domain not supplied")));
    TestEqual(TEXT("Received dissipation value unchanged"),Stream.Latest()->Budget.DissipationNear.Get(-1),2.);
    auto Changed=Log().Replace(TEXT("identified original integration domains"),TEXT("changed source"));TestTrue(TEXT("Immutable original domains reject a changed declaration"),Append(Stream,Changed).Malformed>0);
    for(const auto& Bad:TArray<FString>{Log().Replace(TEXT("body-CoG-local-XYZ"),TEXT("world-XYZ")),Log().Replace(TEXT("body-lengths"),TEXT("guessed")),Log().Replace(TEXT("\"budget_domain_source\":\"identified original integration domains\","),TEXT("")),Log().Replace(TEXT("[-1,-2,-3]"),TEXT("[2,-2,-3]")),Log().Replace(TEXT("source-water-phi-ge-0.5"),TEXT(""))})
    {FStudioHome4TelemetryStream Invalid;Invalid.BeginRun({FGuid::NewGuid(),TEXT("malformed domain fixture")});TestTrue(TEXT("Unknown frame/unit/source/reversed bounds/empty phase mask reject original domains"),Append(Invalid,Bad).Malformed>0);}
    TArray<FStudioHome4EnergyBudgetRegion> Mismatched=Stream.OriginalMetadata()->EnergyBudgetDomains;Mismatched[1].PhaseMask=TEXT("source-air-phi-lt-0.5");FString MaskError;
    TestFalse(TEXT("Far shell cannot exclude a near box with a different phase mask"),StudioHome4EnergyBudget::Validate(Mismatched,MaskError));
    const FString MixedLog=Log().Replace(TEXT("\"coordinate_unit\":\"body-lengths\",\"frame\":\"body-CoG-local-XYZ\",\"tracking\":\"follow-body\",\"region\":\"shell-excluding-near\""),TEXT("\"coordinate_unit\":\"root-cells\",\"frame\":\"body-CoG-local-XYZ\",\"tracking\":\"follow-body\",\"region\":\"shell-excluding-near\""));
    FStudioHome4TelemetryStream UnknownMap;UnknownMap.BeginRun({FGuid::NewGuid(),TEXT("original mixed-unit domain fixture")});
    TestTrue(TEXT("Original mixed-unit nesting cannot borrow the next request unit map"),Append(UnknownMap,MixedLog).Malformed>0);
    FStudioHome4TelemetryStream KnownMap;KnownMap.BeginRun({FGuid::NewGuid(),TEXT("original explicit domain unit map fixture")});
    TestEqual(TEXT("Original declared length verifies mixed-unit nesting"),Append(KnownMap,MixedLog.Replace(TEXT("\"budget_domain_source\""),TEXT("\"unit_map\":{\"length_cells\":1},\"budget_domain_source\""))).Malformed,int32(0));
    FStudioHome4TelemetryStream Legacy;Legacy.BeginRun({FGuid::NewGuid(),TEXT("legacy budget fixture")});TestEqual(TEXT("Legacy budget needs no guessed domain"),Append(Legacy,TEXT("{\"step\":1,\"budget\":{\"D_near\":2}}\n")).Malformed,int32(0));TestTrue(TEXT("Absent original domain remains explicitly unknown"),StudioHome4EnergyBudget::Description(Legacy.OriginalMetadata()?Legacy.OriginalMetadata()->EnergyBudgetDomains:TArray<FStudioHome4EnergyBudgetRegion>()).Contains(TEXT("not supplied")));
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-energy-domain")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
    auto M=MakeShared<FStudioModel>(Root/TEXT("project"));FStudioHome4Spec Request;Request.Authoring.BodyId=TEXT("different-next-request-body");M->Project.Draft.Home4=Request;
    FStudioHome4TelemetryProvenance P;P.StreamRunId=Run;P.OriginalRunId=Run;P.SourceId=TEXT("identified original energy-domain fixture");P.SourcePath=Root/TEXT("original.jsonl");P.bImportedReplay=true;P.AttachedProjectId=M->Project.Id;P.AttachedCaseId=M->Project.Draft.Id;
    const FTCHARToUTF8 Bytes(*Log());TArray<uint8> Original;Original.Append(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());uint8 Digest[32];unsigned int N=0;EVP_Digest(Original.GetData(),Original.Num(),Digest,&N,EVP_sha256(),nullptr);P.SourceSHA256=BytesToHex(Digest,N).ToLower();
    FStudioHome4ReportInputs Inputs;Inputs.OriginalTelemetryBytes=&Original;FString Path,Error;
    if(!TestTrue(TEXT("Original domain provenance exports beside independent next request"),StudioHome4Reports::Export(Root,TEXT("report"),M->SnapshotProject(),&Stream,Path,Error,nullptr,nullptr,&P,&Inputs))){AddError(Error);return false;}
    FString Manifest,Raw;FFileHelper::LoadFileToString(Manifest,*(Path/TEXT("report.json")));FFileHelper::LoadFileToString(Raw,*(Path/TEXT("original-telemetry.jsonl")));
    TestTrue(TEXT("Report retains original body/domain/source/convention"),Manifest.Contains(TEXT("original-body"))&&Manifest.Contains(TEXT("budget_domains"))&&Manifest.Contains(TEXT("far=inside far excluding near")));
    TestTrue(TEXT("Current request remains separately identified"),Manifest.Contains(TEXT("different-next-request-body")));TestEqual(TEXT("Exact source bytes survive original domain report"),Raw,Log());
    IFileManager::Get().DeleteDirectory(*Root,false,true);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4EnergyRegionNativeWorkflow,"Studio.Home4.Monitors.NativeEnergyBoxSharedEditorApplyAndScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4EnergyRegionNativeWorkflow::RunTest(const FString&)
{
    if(!FSlateApplication::IsInitialized()){AddError(TEXT("Native Slate unavailable."));return false;}
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-energy-editor")/FGuid::NewGuid().ToString());FStudioHome4Spec S;S.Authoring.BodyId=TEXT("explicit-body");S.Reference.LengthCells=10;S.Units.DxMeters=.5;M->Project.Draft.Home4=S;
    auto Editor=MakeShared<FStudioHome4Session>(M);auto Panel=SNew(SStudioHome4EnergyBudget).Model(M).Editor(Editor);FStudioHeadlessSlate UI(*this,Panel,FVector2D(1200,2400));
    UI.Press(TEXT("Home4EnergyEdit"));
    for(const auto* Role:{TEXT("near"),TEXT("far")})
    {
        const FString Prefix=TEXT("Home4Energy.")+FString(Role);UI.Focus(FName(*(Prefix+TEXT(".enabled"))));UI.Key(EKeys::SpaceBar);UI.Layout();
        UI.Type(FName(*(Prefix+TEXT(".body"))),TEXT("explicit-body"));
        auto Select=[&](const TCHAR* Field,const FString& Choice)
        {UI.Press(FName(*(Prefix+TEXT(".")+Field)));UI.Press(FName(*(TEXT("Home4Energy.choice.")+FString(Role)+TEXT(".")+Field+TEXT(".")+Choice)));};
        Select(TEXT("units"),TEXT("body-lengths"));Select(TEXT("frame"),TEXT("body-CoG-local-XYZ"));Select(TEXT("tracking"),TEXT("follow-body"));Select(TEXT("region"),FString(Role)==TEXT("far")?TEXT("shell-excluding-near"):TEXT("inside-box"));UI.Type(FName(*(Prefix+TEXT(".phaseMask"))),TEXT("source-water-phi-ge-0.5"));
        UI.Type(FName(*(Prefix+TEXT(".minimum"))),FString(Role)==TEXT("near")?TEXT("-1, -1, -1"):TEXT("-2, -2, -2"));UI.Type(FName(*(Prefix+TEXT(".maximum"))),FString(Role)==TEXT("near")?TEXT("1, 1, 1"):TEXT("2, 2, 2"));
    }
    TestEqual(TEXT("Native preview depicts explicitly entered boxes"),Panel->PreviewRegions().Num(),2);TestTrue(TEXT("Actual preview and Apply action are arranged"),UI.Inspect(TEXT("home4-energy-boxes"),{TEXT("Home4EnergyPreview"),TEXT("Home4EnergyRetain"),TEXT("Home4EnergyApply")}));
    TestTrue(TEXT("Pending text blocks uncommitted request preparation"),Editor->HasPending());UI.Press(TEXT("Home4EnergyRetain"));
    FStudioHome4Spec Retained;FString Error;if(!TestTrue(TEXT("Same retained editor builds both authored boxes"),Editor->Build(Retained,Error))){AddError(Panel->StatusText());return false;}
    TestEqual(TEXT("Retaining does not silently apply the case"),M->Project.Draft.Home4->Authoring.EnergyBudgetRegions.Num(),0);if(!TestEqual(TEXT("Native retain saves both roles"),Retained.Authoring.EnergyBudgetRegions.Num(),2))return false;
    UI.Press(TEXT("Home4EnergyApply"));if(!TestEqual(TEXT("Native Apply commits requested boxes through shared editor"),M->Project.Draft.Home4->Authoring.EnergyBudgetRegions.Num(),2))return false;
    const auto RootBox=StudioHome4EnergyBudget::RootBox(M->Project.Draft.Home4->Authoring.EnergyBudgetRegions[0],*M->Project.Draft.Home4);TestTrue(TEXT("Supplied body length determines root coordinates"),RootBox&&RootBox->Min==FVector(-10)&&RootBox->Max==FVector(10));
    auto Unknown=S;Unknown.Reference.LengthCells.Reset();TestFalse(TEXT("Missing original length cannot invent a scale"),StudioHome4EnergyBudget::RootBox(Retained.Authoring.EnergyBudgetRegions[0],Unknown).IsSet());
    auto Mixed=Retained;Mixed.Authoring.EnergyBudgetRegions[1].Units=TEXT("root-cells");Mixed.Authoring.EnergyBudgetRegions[1].Minimum=FVector(-20);Mixed.Authoring.EnergyBudgetRegions[1].Maximum=FVector(20);
    TestTrue(TEXT("Mixed declared units compare using supplied body scale"),StudioHome4EnergyBudget::ValidateRequest(Mixed,Error));Mixed.Reference.LengthCells.Reset();
    TestFalse(TEXT("Unknown mixed-unit nesting cannot be retained as valid"),StudioHome4EnergyBudget::ValidateRequest(Mixed,Error));Mixed=Retained;Mixed.Authoring.EnergyBudgetRegions[1].Units=TEXT("physical-metres");Mixed.Authoring.EnergyBudgetRegions[1].Minimum=FVector(-2);Mixed.Authoring.EnergyBudgetRegions[1].Maximum=FVector(2);
    TestFalse(TEXT("Converted near extending past physical far is rejected"),StudioHome4EnergyBudget::ValidateRequest(Mixed,Error));
    UI.Type(TEXT("Home4Energy.near.maximum"),TEXT("3, 3, 3"));UI.Press(TEXT("Home4EnergyRetain"));TestTrue(TEXT("Invalid near/far nesting is explicit"),Panel->StatusText().Contains(TEXT("inside")));TestEqual(TEXT("Invalid box keeps previous applied maximum"),M->Project.Draft.Home4->Authoring.EnergyBudgetRegions[0].Maximum,FVector(1));
    TestTrue(TEXT("Rejected user box text stays pending before scope change"),Editor->HasPending());
    M->Project.Id=FGuid::NewGuid();M->Project.Draft.Id=FGuid::NewGuid();M->Project.Draft.Home4=S;Panel->Tick(FGeometry(),0,0);UI.Layout();TestFalse(TEXT("Project switch clears foreign pending budget text"),Editor->HasPending());TestEqual(TEXT("New request inherits no previous boxes"),Panel->PreviewRegions().Num(),0);
    Panel->Tick(FGeometry(),0,0);UI.Layout();TestFalse(TEXT("Bound native text reload cannot recreate cleared pending edits"),Editor->HasPending());
    return true;
}
#endif
