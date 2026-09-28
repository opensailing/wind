#include "StudioSnapshot.h"
#include "StudioFileDialog.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto SnapshotFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FStudioSnapshot EncodingFixture()
{
    // An encoder pattern, not a CFD field or sample recording.
    FStudioSnapshot S;S.Options.Size=FIntPoint(64,64);S.SourceSize=FIntPoint(1600,900);S.Framing=StudioSnapshot::Frame(S.SourceSize,S.Options.Size);
    S.Project=FGuid::NewGuid();S.SourceTitle=TEXT("PNG encoder pattern α");S.Identity.Dataset=TEXT("encoder-pattern");
    S.Identity.MetadataSHA256=FString::ChrN(64,'a');S.Identity.PayloadSHA256=FString::ChrN(64,'b');
    S.Identity.Ordinal=7;S.Identity.Frame={1700,2.125};S.Identity.SpatialDimensions=2;
    S.Camera.Position=FVector(.12345678901234567,2,3);S.Capture=MAX_uint64;
    S.SelectedObject=FGuid::NewGuid();
    S.DisplaySettings.VolumeOpacity=.62;S.DisplaySettings.VolumeStepVoxels=.5;
    S.DisplaySettings.VolumeClipMinimum=FVector(.1,.2,.3);S.DisplaySettings.VolumeClipMaximum=FVector(.8,.9,1);
    S.DisplaySettings.VolumeOpacityCurve=FVector(.2,.7,.9);S.DisplaySettings.bVolumeThreshold=true;
    S.DisplaySettings.VolumeThresholdMinimum=.003;S.DisplaySettings.VolumeThresholdMaximum=.031;
    S.DisplaySettings.bVolumeIsosurface=true;S.DisplaySettings.VolumeIsovalue=.021;
    S.DisplaySettings.PointSize=2.5;S.DisplaySettings.VectorScale=1.75;S.DisplaySettings.SliceAxis=2;S.DisplaySettings.SlicePosition=.125;
    S.DisplaySettings.VectorCount=321;S.DisplaySettings.bUniformVectors=true;
    S.Vectors={321,301,12.5,.0175,true};
    S.FlowBounds=FBox(FVector(-2,-3,-4),FVector(5,6,7));
    S.SliceNotices.Add(S.SelectedObject,TEXT("Encoder test: material unavailable"));
    S.Overlay.Markers.Add({S.SelectedObject,FVector(.125,.25,.375),TEXT("Resolved source point α")});
    S.Overlay.Lines.Add({S.SelectedObject,FVector(1,2,3),FVector(4,5,6),3});
    S.Pixels.Init(FColor(17,101,213,0),4096);S.ProbeCSV=TEXT("frame,unit\n1700,Pa\n");return S;
}
uint32 BigEndian(const uint8* P){return (uint32(P[0])<<24)|(uint32(P[1])<<16)|(uint32(P[2])<<8)|P[3];}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSnapshotFramingTest,"Studio.Snapshot.CenteredFramingAndSizeBounds",SnapshotFlags)
bool FStudioSnapshotFramingTest::RunTest(const FString&)
{
    const auto Square=StudioSnapshot::Frame(FIntPoint(1600,900),FIntPoint(1920,1920));
    TestTrue(TEXT("Square trims horizontal extent without stretching"),Square.Span==FVector2D(.5625,1)&&Square.Minimum==FVector2D(.21875,0));
    TestTrue(TEXT("Source camera center maps to output center"),Square.ToOutput(FVector2D(800,450),FVector2D(1600,900),FVector2D(1920,1920))==FVector2D(960,960));
    TestTrue(TEXT("Source crop corner maps exactly"),Square.ToOutput(FVector2D(350,0),FVector2D(1600,900),FVector2D(1920,1920))==FVector2D::ZeroVector);
    const auto Wide=StudioSnapshot::Frame(FIntPoint(1000,1000),FIntPoint(1600,900));
    TestTrue(TEXT("Wide output trims vertical extent"),Wide.Span==FVector2D(1,.5625)&&Wide.Minimum==FVector2D(0,.21875));
    const auto Same=StudioSnapshot::Frame(FIntPoint(1600,900),FIntPoint(3200,1800));
    TestTrue(TEXT("Higher resolution keeps the exact frustum"),Same.Span==FVector2D(1,1)&&Same.Minimum==FVector2D::ZeroVector);
    TestTrue(TEXT("Bounded 4K square allowed"),StudioSnapshot::ValidSize(FIntPoint(4096,4096)));
    TestFalse(TEXT("Unbounded allocation rejected"),StudioSnapshot::ValidSize(FIntPoint(MAX_int32,MAX_int32)));
    TestFalse(TEXT("Zero dimension rejected"),StudioSnapshot::ValidSize(FIntPoint(1920,0)));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSnapshotPNGTest,"Studio.Snapshot.PNGPixelsAndEmbeddedIdentity",SnapshotFlags)
bool FStudioSnapshotPNGTest::RunTest(const FString&)
{
    auto Snapshot=EncodingFixture();const auto Frozen=Snapshot;Snapshot.Identity.Frame={9999,1000};Snapshot.Pixels[0]=FColor::Black;
    Snapshot.DisplaySettings.VolumeOpacity=.99;Snapshot.DisplaySettings.VolumeOpacityCurve=FVector::ZeroVector;Snapshot.SliceNotices.Reset();
    Snapshot.DisplaySettings.VectorCount=4096;Snapshot.DisplaySettings.bUniformVectors=false;Snapshot.Vectors={};
    TArray64<uint8> PNG;FString Error;if(!TestTrue(TEXT("Encode frozen PNG"),StudioSnapshot::Encode(Frozen,PNG,Error)))return false;
    auto& Module=FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));const auto Reader=Module.CreateImageWrapper(EImageFormat::PNG);
    TestTrue(TEXT("Independent PNG decoder accepts metadata chunk"),Reader->SetCompressed(PNG.GetData(),PNG.Num()));
    TestEqual(TEXT("Saved width"),Reader->GetWidth(),int64(64));TestEqual(TEXT("Saved height"),Reader->GetHeight(),int64(64));
    TArray64<uint8> Raw;TestTrue(TEXT("Decode exact image bytes"),Reader->GetRaw(ERGBFormat::RGBA,8,Raw));
    if(!TestEqual(TEXT("Decoded byte count"),Raw.Num(),int64(64*64*4)))return false;
    TestTrue(TEXT("Alpha is opaque and RGB stays exact"),Raw[0]==17&&Raw[1]==101&&Raw[2]==213&&Raw[3]==255);
    FString Metadata;
    for(int64 Offset=8;Offset+12<=PNG.Num();)
    {
        const uint32 Length=BigEndian(PNG.GetData()+Offset);if(Offset+12+Length>PNG.Num())break;
        if(FMemory::Memcmp(PNG.GetData()+Offset+4,"iTXt",4)==0&&Length>14)
        {
            const auto* Start=reinterpret_cast<const ANSICHAR*>(PNG.GetData()+Offset+8+14);
            const FUTF8ToTCHAR Text(Start,Length-14);Metadata=FString(Text.Length(),Text.Get());break;
        }
        Offset+=12+Length;
    }
    TSharedPtr<FJsonObject> JSON;TestTrue(TEXT("Embedded UTF-8 identity is valid JSON"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Metadata),JSON));
    if(!JSON)return false;
    TestEqual(TEXT("Frame remains frozen"),JSON->GetNumberField(TEXT("frame")),1700.);
    TestEqual(TEXT("Physical time remains frozen"),JSON->GetNumberField(TEXT("time_seconds")),2.125);
    TestEqual(TEXT("Capture serial retains all 64 bits"),JSON->GetStringField(TEXT("capture")),LexToString(MAX_uint64));
    TestEqual(TEXT("Unicode source label retained"),JSON->GetStringField(TEXT("title")),Frozen.SourceTitle);
    TestEqual(TEXT("Probe sample payload remains from capture"),JSON->GetStringField(TEXT("probe_samples_csv")),Frozen.ProbeCSV);
    const auto Display=JSON->GetObjectField(TEXT("display_settings"));
    TestEqual(TEXT("Display metadata declares its project schema"),int32(JSON->GetNumberField(TEXT("display_schema_version"))),FStudioProject::CurrentVersion);
    TestEqual(TEXT("Volume opacity remains frozen"),Display->GetNumberField(TEXT("volumeOpacity")),.62);
    TestEqual(TEXT("Ray sampling step is retained"),Display->GetNumberField(TEXT("volumeStepVoxels")),.5);
    TestEqual(TEXT("Volume crop minimum is retained"),Display->GetArrayField(TEXT("volumeClipMinimum"))[1]->AsNumber(),.2);
    TestEqual(TEXT("Volume crop maximum is retained"),Display->GetArrayField(TEXT("volumeClipMaximum"))[0]->AsNumber(),.8);
    TestEqual(TEXT("Opacity transfer curve remains frozen"),Display->GetArrayField(TEXT("volumeOpacityCurve"))[1]->AsNumber(),.7);
    TestTrue(TEXT("Threshold and isosurface toggles are retained"),Display->GetBoolField(TEXT("volumeThreshold"))&&Display->GetBoolField(TEXT("volumeIsosurface")));
    TestEqual(TEXT("Physical threshold minimum is retained"),Display->GetNumberField(TEXT("volumeThresholdMinimum")),.003);
    TestEqual(TEXT("Physical threshold maximum is retained"),Display->GetNumberField(TEXT("volumeThresholdMaximum")),.031);
    TestEqual(TEXT("Physical isovalue is retained"),Display->GetNumberField(TEXT("volumeIsovalue")),.021);
    TestEqual(TEXT("Original point display size is retained"),Display->GetNumberField(TEXT("pointSize")),2.5);
    TestEqual(TEXT("Vector scale is retained"),Display->GetNumberField(TEXT("vectorScale")),1.75);
    TestEqual(TEXT("Vector sample count remains frozen"),Display->GetNumberField(TEXT("vectorCount")),321.);
    TestTrue(TEXT("Vector length mode remains frozen"),Display->GetBoolField(TEXT("uniformVectors")));
    const auto Vectors=JSON->GetObjectField(TEXT("vector_display"));
    TestEqual(TEXT("Captured vector count remains frozen"),Vectors->GetNumberField(TEXT("glyph_count")),301.);
    TestEqual(TEXT("Captured sample maximum remains frozen"),Vectors->GetNumberField(TEXT("sampled_maximum_speed_m_per_s")),12.5);
    TestEqual(TEXT("Captured length key remains frozen"),Vectors->GetNumberField(TEXT("reference_length_m")),.0175);
    TestTrue(TEXT("Captured length mode remains frozen"),Vectors->GetBoolField(TEXT("uniform_length")));
    TestEqual(TEXT("Cut plane position is retained"),Display->GetNumberField(TEXT("slicePosition")),.125);
    TestEqual(TEXT("Render bounds carry scene units"),JSON->GetObjectField(TEXT("flow_bounds"))->GetArrayField(TEXT("minimum_meters"))[2]->AsNumber(),-4.);
    TestEqual(TEXT("Unavailable slice status remains frozen"),JSON->GetObjectField(TEXT("slice_rendering_status"))->GetStringField(Frozen.SelectedObject.ToString()),Frozen.SliceNotices[Frozen.SelectedObject]);
    const auto Overlay=JSON->GetObjectField(TEXT("resolved_overlay"));
    const auto Marker=Overlay->GetArrayField(TEXT("markers"))[0]->AsObject();
    TestEqual(TEXT("Resolved source position is preserved"),Marker->GetArrayField(TEXT("position"))[2]->AsNumber(),.375);
    TestEqual(TEXT("Resolved marker remains tied to its saved object"),Marker->GetStringField(TEXT("object")),Frozen.SelectedObject.ToString());
    TestEqual(TEXT("Annotation endpoint is preserved"),Overlay->GetArrayField(TEXT("lines"))[0]->AsObject()->GetArrayField(TEXT("b"))[1]->AsNumber(),5.);
    const FString Path=FPaths::ProjectDir()/TEXT("tmp/debug/snapshot-encoder-pattern.png");
    TestTrue(TEXT("Save PNG through atomic writer"),StudioFileDialog::WriteAtomicBytes(Path,PNG,Error));
    auto Broken=Frozen;Broken.Pixels.Pop();TestFalse(TEXT("Partial image refused"),StudioSnapshot::Encode(Broken,PNG,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSnapshotLifecycleTest,"Studio.Snapshot.CancellationFailureAndAtomicReplacement",SnapshotFlags)
bool FStudioSnapshotLifecycleTest::RunTest(const FString&)
{
    const FString Directory=FPaths::ProjectDir()/TEXT("tmp/debug")/(TEXT("snapshot-lifecycle-")+FGuid::NewGuid().ToString());
    IFileManager::Get().MakeDirectory(*Directory,true);
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory,false,true); };
    const FString Path=Directory/TEXT("existing.png"),Original=TEXT("Existing destination must survive");
    if(!TestTrue(TEXT("Create an existing destination"),FFileHelper::SaveStringToFile(Original,*Path)))return false;
    auto Unchanged=[&]{FString Text;return FFileHelper::LoadFileToString(Text,*Path)&&Text==Original;};
    auto Await=[](FStudioSnapshotExportTask& Task)
    {
        const double Deadline=FPlatformTime::Seconds()+5.;
        while(FPlatformTime::Seconds()<Deadline)
        {
            auto Result=Task.Poll();if(Result.IsSet())return Result;
            FPlatformProcess::SleepNoStats(.001f);
        }
        return TOptional<FStudioSnapshotExportResult>();
    };
    struct FWriteGate
    {
        FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
        FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
        ~FWriteGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
    };
    FStudioSnapshotExportTask Task;auto Gate=MakeShared<FWriteGate,ESPMode::ThreadSafe>();
    Task.BeforeWriteForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(10000);};
    TestTrue(TEXT("Start encoding the first request"),Task.Start(EncodingFixture(),Path));
    TestTrue(TEXT("Real worker reaches the pre-write boundary"),Gate->Reached->Wait(5000));
    TestFalse(TEXT("Only one export can own the worker"),Task.Start(EncodingFixture(),Directory/TEXT("second.png")));
    TestTrue(TEXT("Cancellation wins before atomic replacement"),Task.Cancel());
    TestFalse(TEXT("Repeated cancellation does not claim another transition"),Task.Cancel());
    Gate->Release->Trigger();auto Result=Await(Task);
    TestTrue(TEXT("Cancelled result is explicit and has no write error"),Result.IsSet()&&Result->bCancelled&&!Result->bSuccess&&Result->Error.IsEmpty());
    TestTrue(TEXT("Cancellation preserves existing destination bytes"),Unchanged());
    TestFalse(TEXT("Rejected second request created no file"),IFileManager::Get().FileExists(*(Directory/TEXT("second.png"))));
    if(Task.IsBusy())return false;

    auto Invalid=EncodingFixture();Invalid.Pixels.Pop();
    TestTrue(TEXT("Invalid pixels are checked asynchronously"),Task.Start(MoveTemp(Invalid),Path));Result=Await(Task);
    TestTrue(TEXT("Encoding failure is reported"),Result.IsSet()&&!Result->bCancelled&&!Result->bSuccess&&!Result->Error.IsEmpty());
    TestTrue(TEXT("Encoding failure preserves existing destination"),Unchanged());
    if(Task.IsBusy())return false;

    TestTrue(TEXT("Start write beneath a regular file"),Task.Start(EncodingFixture(),Path/TEXT("blocked.png")));Result=Await(Task);
    TestTrue(TEXT("Atomic write failure is reported"),Result.IsSet()&&!Result->bCancelled&&!Result->bSuccess&&!Result->Error.IsEmpty());
    TestTrue(TEXT("Failed destination leaves original file intact"),Unchanged());
    if(Task.IsBusy())return false;

    TestTrue(TEXT("Worker can retry after failures"),Task.Start(EncodingFixture(),Path));Result=Await(Task);
    TestTrue(TEXT("Atomic replacement completes"),Result.IsSet()&&Result->bSuccess&&!Result->bCancelled&&Result->Frame.Index==1700);
    TestFalse(TEXT("Completed write cannot claim cancellation"),Task.Cancel());
    TArray64<uint8> Bytes;FFileHelper::LoadFileToArray(Bytes,*Path);
    auto& Module=FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));const auto Reader=Module.CreateImageWrapper(EImageFormat::PNG);
    TestTrue(TEXT("Replacement is a complete PNG"),Reader->SetCompressed(Bytes.GetData(),Bytes.Num())&&Reader->GetWidth()==64&&Reader->GetHeight()==64);
    return true;
}
#endif
