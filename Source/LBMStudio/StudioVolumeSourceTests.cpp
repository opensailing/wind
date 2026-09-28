#include "StudioVolume.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVolumeSource,"Studio.VolumeSource.OriginalValuesAndIndependentQueries",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioVolumeSource::RunTest(const FString&)
{
    const FString Folder=FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture"),
        Mapping=FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json");
    const auto Opened=StudioRecordings::Import(Folder/TEXT("recording.json"),0,{});
    if(!TestTrue(*Opened.Error,Opened.Source.IsValid()&&Opened.Reference.IsSet()))return false;
    const auto Bound=StudioRecordings::ImportReconstruction(*Opened.Reference,Mapping,0,{});
    if(!TestTrue(*Bound.Error,Bound.Source.IsValid()))return false;
    const auto Volume=Bound.Source->VolumeReconstruction();
    if(!TestTrue(TEXT("Verified 3D mapping attached"),Volume.IsValid()))return false;
    FString Text;TSharedPtr<FJsonObject> Expected;
    if(!TestTrue(TEXT("Independent source comparisons are available"),FFileHelper::LoadFileToString(Text,*(FPaths::GetPath(Mapping)/TEXT("independent-validation.json")))&&
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Expected)))return false;
    TestEqual(TEXT("Queries pin exact source identity"),Expected->GetStringField(TEXT("recording_sha256")),Volume->SourceMetadataSHA256);
    TestEqual(TEXT("Queries pin exact reconstruction identity"),Expected->GetStringField(TEXT("reconstruction_sha256")),Volume->MetadataSHA256);
    TestTrue(TEXT("Verifier compared original HDF5 arrays"),Expected->GetNumberField(TEXT("original_hdf5_values_compared"))>0);
    const auto& Queries=Expected->GetArrayField(TEXT("expected_queries"));
    TestTrue(TEXT("Many fields and original source times are covered"),Queries.Num()>=100);
    for(const auto& Item:Queries)
    {
        const auto Q=Item->AsObject();const FString Id=Q->GetStringField(TEXT("field"));
        const int32 Frame=Q->GetIntegerField(TEXT("frame"));const auto& P=Q->GetArrayField(TEXT("position"));
        const FVector Source(P[0]->AsNumber(),P[1]->AsNumber(),P[2]->AsNumber());
        const auto Field=Bound.Source->CaptureViewField(Frame,Id,false);
        double Actual;const double Value=Q->GetNumberField(TEXT("value"));
        if(!TestTrue(TEXT("Supported physical query is available"),Field->SampleScalar(FVector(Source.X,Source.Z,Source.Y),Id,Actual)))return false;
        const auto* Scalar=Bound.Source->Descriptor().Scalars.FindByPredicate([&](const auto& Item){return Item.Id==Id;});
        if(!TestNotNull(TEXT("Query names an original physical scalar"),Scalar))return false;
        const double Scale=FMath::Max3(FMath::Abs(Scalar->Minimum),FMath::Abs(Scalar->Maximum),1.e-300);
        if(!TestTrue(TEXT("Native grid sampling matches independent calculation relative to its physical field scale"),FMath::Abs(Actual-Value)<=Scale*1.e-8))return false;
        const auto Points=Field->OriginalPoints();
        TestNotNull(TEXT("Original float64 field remains available"),Points->FindValues(Id));
    }
    const auto Field=Bound.Source->CaptureViewField(0,Bound.Source->Descriptor().DefaultScalar,false);
    const auto OriginalField=Opened.Source->CaptureViewField(0,Opened.Source->Descriptor().DefaultScalar,false);
    TestTrue(TEXT("Attachment never modifies original scalar rows"),*Field->OriginalPoints()->FindValues(Bound.Source->Descriptor().DefaultScalar)==
        *OriginalField->OriginalPoints()->FindValues(Opened.Source->Descriptor().DefaultScalar));
    const auto Points=Field->OriginalPoints();FStudioColorMapping Colors;Colors.Minimum=0;Colors.Maximum=.1;
    const auto Render=StudioVolumes::Build(*Points,*Volume,Bound.Source->Descriptor().DefaultScalar,Colors);
    TestTrue(TEXT("Original samples produce a bounded GPU grid"),Render.Error.IsEmpty()&&Render.Texels.Num()==Volume->Stencils.Num());
    TestTrue(TEXT("Float transport retains scientific display precision"),Render.MaximumTransportError<1.e-5);
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestTrue(TEXT("Cancelled upload produces no stale field"),!StudioVolumes::Build(*Points,*Volume,Bound.Source->Descriptor().DefaultScalar,Colors,Cancel).Error.IsEmpty());
    TestFalse(TEXT("Changed reconstruction identity is rejected"),StudioVolumes::Load(Mapping,*Points->Descriptor,Points->Geometry.ToSharedRef(),{},FString::ChrN(64,'0')).Volume.IsValid());
    return true;
}
#endif
