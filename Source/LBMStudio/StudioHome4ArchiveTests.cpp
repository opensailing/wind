#include "StudioHome4ArchivePrivate.h"
#include "SStudioHome4ArchivePanel.h"
#include "StudioHeadlessSlate.h"
#include "StudioModel.h"
#include "StudioVolume.h"
#include "StudioPointRecording.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SScrollBox.h"
#include "StudioHome4ArchiveFixtures.inl"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4ArchiveTestPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
struct FFixture
{
    FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())/TEXT("Automation")/(TEXT("Home4Archive_")+FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FFixture(){IFileManager::Get().MakeDirectory(*Folder,true);}
    ~FFixture(){IFileManager::Get().DeleteDirectory(*Folder,false,true);}
    FString Save(const TCHAR* Name,const TCHAR* Data)
    {TArray<uint8> Bytes;if(!FBase64::Decode(Data,Bytes))return {};const FString Path=Folder/(FString(Name)+TEXT(".npz"));return FFileHelper::SaveArrayToFile(Bytes,*Path)?Path:FString();}
    FStudioHome4ArchiveRequest Request(const FString& Path,const TCHAR* Name=TEXT("recording"))
    {
        auto Inspected=StudioHome4Archives::Inspect(Path);FStudioHome4ArchiveRequest R;R.Sources={Inspected.Source};R.OutputParent=Folder;R.FolderName=Name;
        R.SourceURI=TEXT("urn:artificial-numpy-numerical-contract-fixture");R.Attribution=TEXT("Artificial numerical unit fixture. Not solver output or an installed sample.");
        R.Mapping.AxisOrder=TEXT("xyz");R.Mapping.MetadataOrder=TEXT("xyz");R.Mapping.CoordinateUnits=TEXT("lattice");R.Mapping.VelocityUnits=TEXT("lattice");R.Mapping.DxMeters=.1;R.Mapping.DtSeconds=.02;R.Mapping.DensityReferenceKgM3=1000.;return R;
    }
};
uint64 U64(const uint8* P){uint64 V=0;for(int32 I=0;I<8;++I)V|=uint64(P[I])<<(8*I);return V;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4NPYInspectionTest,"Studio.Home4.Archive.NumpyContainersAndLiteralHeaders",StudioHome4ArchiveTestPrivate::Flags)
bool FHome4NPYInspectionTest::RunTest(const FString&)
{
    using namespace StudioHome4ArchiveTestPrivate;using namespace StudioHome4ArchiveFixtureData;FFixture F;
    for(const auto& Pair:TArray<TPair<const TCHAR*,const TCHAR*>>{{TEXT("stored"),Stored},{TEXT("compressed"),Axes_xyz},{TEXT("v2-fortran-big-endian"),FortranBigEndian},{TEXT("half"),Half}})
    {
        auto R=StudioHome4Archives::Inspect(F.Save(Pair.Key,Pair.Value));TestTrue(FString(Pair.Key)+TEXT(" NumPy headers and all CRCs accepted"),R.Error.IsEmpty());TestEqual(TEXT("Original SHA256 retained"),R.Source.SHA256.Len(),64);
        if(!R.OriginalRunSpec)continue;FString Note;R.OriginalRunSpec->TryGetStringField(TEXT("note"),Note);TestTrue(TEXT("Original UTF32 Unicode metadata decoded"),Note.Contains(TEXT("Δ 水")));
    }
    auto C=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);auto Cancelled=StudioHome4Archives::Inspect(F.Save(TEXT("cancelled"),Axes_xyz),C);TestTrue(TEXT("Cancelled inspection does not produce verified members"),!Cancelled.Error.IsEmpty()||Cancelled.bCancelled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4ArchiveSourceIdentityTest,"Studio.Home4.Archive.AxesUnitsAndOriginalIdentity",StudioHome4ArchiveTestPrivate::Flags)
bool FHome4ArchiveSourceIdentityTest::RunTest(const FString&)
{
    using namespace StudioHome4ArchiveTestPrivate;using namespace StudioHome4ArchiveFixtureData;FFixture F;
    for(const auto& Pair:TArray<TPair<const TCHAR*,const TCHAR*>>{{TEXT("xyz"),Axes_xyz},{TEXT("xzy"),Axes_xzy},{TEXT("yxz"),Axes_yxz},{TEXT("yzx"),Axes_yzx},{TEXT("zxy"),Axes_zxy},{TEXT("zyx"),Axes_zyx},{TEXT("fortran"),FortranBigEndian},{TEXT("half"),Half}})
    {
        auto R=F.Request(F.Save(Pair.Key,Pair.Value),Pair.Key);R.Mapping.AxisOrder=FString(Pair.Key)==TEXT("fortran")||FString(Pair.Key)==TEXT("half")?TEXT("xyz"):Pair.Key;
        R.CropMinimum=FIntVector(1,2,1);R.CropMaximum=FIntVector(7,8,9);R.PreviewStride=2;
        auto Converted=StudioHome4Archives::Convert(R);if(!TestTrue(FString(Pair.Key)+TEXT(" native conversion: ")+Converted.Error,Converted.bSuccess))continue;
        auto Open=StudioPointRecordings::Open(Converted.RecordingJSON);if(!TestTrue(TEXT("Published native reader validates complete source"),Open.Recording.IsValid()))continue;
        const auto& D=Open.Recording->Descriptor();TestEqual(TEXT("Selected node count"),D.PointCount,36);TestEqual(TEXT("Original iteration retained"),D.Frames[0].Index,10);TestEqual(TEXT("Original dt timestamp retained"),D.Frames[0].Time,.2);
        const auto Grid=D.StructuredGrid;if(!TestTrue(TEXT("Original grid attachment available"),Grid.IsValid()))continue;TestEqual(TEXT("Original dimensions"),Grid->OriginalDimensions,FIntVector(7,8,9));TestEqual(TEXT("Original dx"),Grid->Units.DxMeters.GetValue(),.1);TestEqual(TEXT("Original recipe lineage"),Grid->LineageId,FString(TEXT("artificial-lineage")));
        auto Geometry=Open.Recording->Geometry();TestEqual(TEXT("Original point ID before crop/stride"),Geometry->PointIds[0],int64(1+7*(2+8)));TestTrue(TEXT("Original source XYZ coordinates"),Geometry->Positions[0].Equals(FVector(.2,.4,.4),1.e-12));
        auto Frame=Open.Recording->ReadFrame(0,{TEXT("ux"),TEXT("uy"),TEXT("q"),TEXT("vorticity_z"),TEXT("limiter_mask"),TEXT("md_level"),TEXT("sponge_weight")});if(!TestTrue(TEXT("Original/derived fields load"),Frame.Frame.IsValid()))continue;
        TestEqual(TEXT("Original lattice velocity converted"),(*Frame.Frame->FindValues(TEXT("ux")))[0],-30.);TestEqual(TEXT("Original second component converted"),(*Frame.Frame->FindValues(TEXT("uy")))[0],15.);
        TestTrue(TEXT("Original-grid Q before preview selection"),FMath::Abs((*Frame.Frame->FindValues(TEXT("q")))[0]-22500.)<1.e-8);TestTrue(TEXT("Original curl unit conversion"),FMath::Abs((*Frame.Frame->FindValues(TEXT("vorticity_z")))[0]-300.)<1.e-8);
        TestEqual(TEXT("Declared original MD level retained"),(*Frame.Frame->FindValues(TEXT("md_level")))[0],1.);TestEqual(TEXT("Declared original beach weight retained"),(*Frame.Frame->FindValues(TEXT("sponge_weight")))[0],.125);
    }
    auto State=F.Request(F.Save(TEXT("declared-state"),DeclaredLatticeState),TEXT("state-recording"));State.PressureConvention=TEXT("wb_lattice");
    auto Converted=StudioHome4Archives::Convert(State);
    if(TestTrue(TEXT("Explicit fixed lattice state accepted: ")+Converted.Error,Converted.bSuccess))
    {
        auto Open=StudioPointRecordings::Open(Converted.RecordingJSON);
        if(TestTrue(TEXT("Declared state reopens"),Open.Recording.IsValid()))
        {
            auto Frame=Open.Recording->ReadFrame(0,{TEXT("rho"),TEXT("nu"),TEXT("Sxx"),TEXT("Pi_h"),TEXT("pressure"),TEXT("dissipation")});
            if(TestTrue(TEXT("Declared state fields load"),Frame.Frame.IsValid()))
            {
                TestEqual(TEXT("Density converts once"),(*Frame.Frame->FindValues(TEXT("rho")))[0],1200.);
                TestTrue(TEXT("Viscosity converts once"),FMath::IsNearlyEqual((*Frame.Frame->FindValues(TEXT("nu")))[0],.02,1.e-12));
                TestEqual(TEXT("Stored strain rate converts once"),(*Frame.Frame->FindValues(TEXT("Sxx")))[0],1.);
                TestEqual(TEXT("Stored head converts once"),(*Frame.Frame->FindValues(TEXT("Pi_h")))[0],1250.);
                TestTrue(TEXT("WB pressure uses declared lattice state"),FMath::IsNearlyEqual((*Frame.Frame->FindValues(TEXT("pressure")))[0],3000.,1.e-9));
                TestTrue(TEXT("Dissipation uses declared lattice strain/viscosity"),FMath::IsNearlyEqual((*Frame.Frame->FindValues(TEXT("dissipation")))[0],.3158,1.e-12));
            }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4ArchiveMasksTest,"Studio.Home4.Archive.MasksRangesAndTimeline",StudioHome4ArchiveTestPrivate::Flags)
bool FHome4ArchiveMasksTest::RunTest(const FString&)
{
    using namespace StudioHome4ArchiveTestPrivate;using namespace StudioHome4ArchiveFixtureData;FFixture F;
    auto R=F.Request(F.Save(TEXT("first"),Axes_xyz));R.Sources.Add(StudioHome4Archives::Inspect(F.Save(TEXT("second"),Frame20)).Source);
    auto Result=StudioHome4Archives::Convert(R);if(!TestTrue(Result.Error,Result.bSuccess))return false;auto Open=StudioPointRecordings::Open(Result.RecordingJSON);if(!TestTrue(TEXT("Two source frames reopen"),Open.Recording.IsValid()))return false;
    const auto& D=Open.Recording->Descriptor();TestEqual(TEXT("Original frame count"),D.Frames.Num(),2);TestEqual(TEXT("Second original time"),D.Frames[1].Time,.4);const auto* Phi=D.FindField(TEXT("phi"));TestEqual(TEXT("Phi overshoot is retained in complete range"),Phi->Maximum,1.2);TestEqual(TEXT("Display percentile holds first-frame ordinary range"),Phi->DisplayMaximum.GetValue(),1.);TestEqual(TEXT("Outlier display clipping reported"),Phi->FirstFrameClippedAbove,int64(1));
    FBox ActualBounds(ForceInit);for(const auto& P:Open.Recording->Geometry()->Positions)ActualBounds+=P;
    TestTrue(TEXT("Bounds exactly attest emitted coordinates, including decimal endpoints"),D.SourceBounds.Min==ActualBounds.Min&&D.SourceBounds.Max==ActualBounds.Max);
    auto Hidden=F.Request(F.Save(TEXT("hidden"),HiddenMasks),TEXT("hidden-recording"));Hidden.CropMinimum=FIntVector(1,2,1);Hidden.CropMaximum=FIntVector(7,8,9);Hidden.PreviewStride=2;Result=StudioHome4Archives::Convert(Hidden);if(!TestTrue(Result.Error,Result.bSuccess))return false;Open=StudioPointRecordings::Open(Result.RecordingJSON);if(!Open.Recording)return false;
    auto Values=Open.Recording->ReadFrame(0,{TEXT("solid"),TEXT("phi"),TEXT("solid_support"),TEXT("liquid_support"),TEXT("q")});if(!Values.Frame)return false;const int32 Row=1+3*(1+3*1);
    TestEqual(TEXT("Hidden obstacle does not replace selected raw solid"),(*Values.Frame->FindValues(TEXT("solid")))[Row],0.);TestEqual(TEXT("Original support detects skipped obstacle"),(*Values.Frame->FindValues(TEXT("solid_support")))[Row],0.);TestEqual(TEXT("Original support detects air"),(*Values.Frame->FindValues(TEXT("liquid_support")))[Row],0.);TestEqual(TEXT("Derivative halo invalid remains unavailable"),(*Values.Frame->FindValues(TEXT("derivative_valid")))[Row],0.);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4ArchiveRejectTest,"Studio.Home4.Archive.RejectUnsafeUnknownAndChangedInputs",StudioHome4ArchiveTestPrivate::Flags)
bool FHome4ArchiveRejectTest::RunTest(const FString&)
{
    using namespace StudioHome4ArchiveTestPrivate;using namespace StudioHome4ArchiveFixtureData;FFixture F;
    for(const auto& P:TArray<TPair<const TCHAR*,const TCHAR*>>{{TEXT("object"),Object},{TEXT("nonfinite"),Nonfinite},{TEXT("inexact"),Inexact},{TEXT("duplicate-json"),DuplicateJSON},{TEXT("escaped-surrogate"),EscapedSurrogate},{TEXT("traversal"),Traversal},{TEXT("case-duplicate"),CaseDuplicate}})
        TestTrue(FString(P.Key)+TEXT(" rejected by native inspection"),!StudioHome4Archives::Inspect(F.Save(P.Key,P.Value)).Error.IsEmpty());
    auto Missing=F.Request(F.Save(TEXT("missing"),MissingCore),TEXT("missing-output"));TestFalse(TEXT("Missing required velocity blocks native output"),StudioHome4Archives::Convert(Missing).bSuccess);
    auto Unknown=F.Request(F.Save(TEXT("unknown"),UnknownUnit),TEXT("unknown-output"));TestFalse(TEXT("Unknown field normalization blocks native output"),StudioHome4Archives::Convert(Unknown).bSuccess);
    auto R=F.Request(F.Save(TEXT("original"),Axes_xyz));R.Mapping.AxisOrder.Empty();TestFalse(TEXT("Unset source conventions never inferred"),StudioHome4Archives::Convert(R).bSuccess);
    R=F.Request(F.Save(TEXT("changed"),Axes_xyz));TArray<uint8> Replacement;FBase64::Decode(Frame20,Replacement);FFileHelper::SaveArrayToFile(Replacement,*R.Sources[0].Path);TestFalse(TEXT("Changed inspected source blocks publication"),StudioHome4Archives::Convert(R).bSuccess);
    FString E;TSharedPtr<FJsonObject> J;TestFalse(TEXT("Duplicate root JSON keys rejected"),StudioHome4ArchivePrivate::JSON(TEXT("{\"a\":1,\"a\":2}"),J,E));
    TestTrue(TEXT("Bounded literal JSON accepted"),StudioHome4ArchivePrivate::JSON(TEXT("{\"a\":[true,null,{\"b\":3}]}"),J,E));
    const auto Before=J;
    TestFalse(TEXT("Unpaired escaped high surrogate rejected"),StudioHome4ArchivePrivate::JSON(TEXT("{\"note\":\"\\ud800\"}"),J,E));
    TestFalse(TEXT("Unpaired escaped low surrogate rejected"),StudioHome4ArchivePrivate::JSON(TEXT("{\"note\":\"\\udc00\"}"),J,E));
    TestTrue(TEXT("Rejected JSON preserves previous parsed object"),J==Before);
    TestTrue(TEXT("Paired escaped Unicode accepted"),StudioHome4ArchivePrivate::JSON(TEXT("{\"note\":\"\\ud83d\\ude00\"}"),J,E));
    for(const auto& P:TArray<TPair<const TCHAR*,const TCHAR*>>{{TEXT("rho"),Conflicting_rho},{TEXT("nu"),Conflicting_nu},{TEXT("strain"),Conflicting_Sxx},{TEXT("head"),Conflicting_Pi_h},{TEXT("normalized-pressure"),Conflicting_p_star},{TEXT("velocity"),Conflicting_ux}})
    {
        auto Conflict=F.Request(F.Save(P.Key,P.Value),P.Key);const auto Rejected=StudioHome4Archives::Convert(Conflict);
        TestFalse(FString(P.Key)+TEXT(" conflicting source unit cannot publish"),Rejected.bSuccess);
        TestTrue(FString(P.Key)+TEXT(" original unit conflict is reported"),Rejected.Error.Contains(TEXT("fixed source contract")));
        TestFalse(TEXT("Rejected unit leaves output absent"),IFileManager::Get().DirectoryExists(*(F.Folder/Conflict.FolderName)));
    }
    auto WrongShape=F.Request(F.Save(TEXT("wrong-shape"),WrongDiagnosticShape),TEXT("wrong-shape-output"));
    const auto ShapeResult=StudioHome4Archives::Convert(WrongShape);TestFalse(TEXT("Declared diagnostic shape cannot silently drop"),ShapeResult.bSuccess);TestTrue(TEXT("Shape rejection reports original field"),ShapeResult.Error.Contains(TEXT("incompatible shape")));
    int64 Used=128;const FString Large=FString::ChrN(2*1024*1024,TCHAR(0x6c34));
    TestTrue(TEXT("UTF8 manifest reserves first large multibyte source"),StudioHome4ArchivePrivate::ReserveManifestBytes(Large,Used));
    TestTrue(TEXT("Different small source contributes cumulatively"),StudioHome4ArchivePrivate::ReserveManifestBytes(TEXT("{}"),Used));
    const int64 BeforeReject=Used;TestFalse(TEXT("Earlier large source cannot be hidden by new small row"),StudioHome4ArchivePrivate::ReserveManifestBytes(Large,Used));
    TestEqual(TEXT("Rejected manifest reservation preserves retained budget"),Used,BeforeReject);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4ArchivePublicationTest,"Studio.Home4.Archive.ExclusivePublicationAndCancellation",StudioHome4ArchiveTestPrivate::Flags)
bool FHome4ArchivePublicationTest::RunTest(const FString&)
{
    using namespace StudioHome4ArchiveTestPrivate;using namespace StudioHome4ArchiveFixtureData;FFixture F;
    auto R=F.Request(F.Save(TEXT("source"),Axes_xyz));auto C=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);auto Result=StudioHome4Archives::Convert(R,C);TestTrue(TEXT("Cancellation reported"),Result.bCancelled);TestFalse(TEXT("Cancellation leaves destination absent"),IFileManager::Get().DirectoryExists(*(F.Folder/R.FolderName)));
    IFileManager::Get().MakeDirectory(*(F.Folder/R.FolderName));FFileHelper::SaveStringToFile(TEXT("keep"),*((F.Folder/R.FolderName)/TEXT("sentinel")));Result=StudioHome4Archives::Convert(R);TestFalse(TEXT("Existing directory cannot be overwritten"),Result.bSuccess);FString Keep;FFileHelper::LoadFileToString(Keep,*((F.Folder/R.FolderName)/TEXT("sentinel")));TestEqual(TEXT("Existing output retained"),Keep,FString(TEXT("keep")));
    TArray<FString> Entries;IFileManager::Get().FindFiles(Entries,*(F.Folder/TEXT(".lbm-export-*.partial")),false,true);TestTrue(TEXT("Failed stage cleaned"),Entries.IsEmpty());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4ArchiveVTITest,"Studio.Home4.Archive.OriginalAffineVTIExport",StudioHome4ArchiveTestPrivate::Flags)
bool FHome4ArchiveVTITest::RunTest(const FString&)
{
    using namespace StudioHome4ArchiveTestPrivate;using namespace StudioHome4ArchiveFixtureData;FFixture F;auto R=F.Request(F.Save(TEXT("source"),Axes_xyz),TEXT("vti"));R.Output=EStudioHome4ArchiveOutput::VTI;R.Sources.Add(StudioHome4Archives::Inspect(F.Save(TEXT("second"),Frame20)).Source);
    auto Result=StudioHome4Archives::Convert(R);if(!TestTrue(Result.Error,Result.bSuccess))return false;TestTrue(TEXT("VTI retains no native recording claim"),Result.RecordingJSON.IsEmpty());TArray<uint8> Bytes;FFileHelper::LoadFileToArray(Bytes,*(Result.Path/TEXT("snapshot-0000000010.vti")));
    const char Marker[]="<AppendedData encoding=\"raw\">_";int32 At=INDEX_NONE;for(int32 I=0;I+int32(sizeof(Marker))-1<Bytes.Num();++I)if(!FMemory::Memcmp(Bytes.GetData()+I,Marker,sizeof(Marker)-1)){At=I+sizeof(Marker)-1;break;}
    if(!TestTrue(TEXT("VTI uses declared appended payload"),At!=INDEX_NONE))return false;FString Header;FFileHelper::BufferToString(Header,Bytes.GetData(),At);TestTrue(TEXT("Original XYZ affine extent"),Header.Contains(TEXT("WholeExtent=\"0 6 0 7 0 8\"")));TestTrue(TEXT("Original source coordinates, without Scene swaps"),Header.Contains(TEXT("Origin=\"0.10000000000000001 0.20000000000000001 0.30000000000000004\"")));
    TestEqual(TEXT("Float64 array exact byte count"),U64(Bytes.GetData()+At),uint64(504*8));FString PVD;FFileHelper::LoadFileToString(PVD,*(Result.Path/TEXT("sequence.pvd")));TestTrue(TEXT("PVD original physical times"),PVD.Contains(TEXT("timestep=\"0.20000000000000001\""))&&PVD.Contains(TEXT("timestep=\"0.40000000000000002\"")));
    auto Raw=R;Raw.FolderName=TEXT("raw-vti");Raw.bPhysicalVTI=false;Raw.Mapping.DxMeters.Reset();Raw.Mapping.DtSeconds.Reset();Raw.Mapping.DensityReferenceKgM3.Reset();
    auto RawResult=StudioHome4Archives::Convert(Raw);
    if(TestTrue(TEXT("Raw lattice VTI has explicit timestep coordinates: ")+RawResult.Error,RawResult.bSuccess))
    {
        FString RawPVD,RawManifest,RawProvenance;FFileHelper::LoadFileToString(RawPVD,*(RawResult.Path/TEXT("sequence.pvd")));FFileHelper::LoadFileToString(RawManifest,*(RawResult.Path/TEXT("source-manifest.json")));FFileHelper::LoadFileToString(RawProvenance,*(RawResult.Path/TEXT("provenance.json")));
        TestTrue(TEXT("PVD explicitly labels original solver-step coordinates"),RawPVD.Contains(TEXT("timestep unit: solver_step"))&&RawPVD.Contains(TEXT("timestep=\"10\"")));
        TSharedPtr<FJsonObject> Manifest;FString Error;
        if(TestTrue(TEXT("Raw VTI source manifest parses"),StudioHome4ArchivePrivate::JSON(RawManifest,Manifest,Error)))
        {
            const auto Source=Manifest->GetArrayField(TEXT("snapshots"))[0]->AsObject();
            TestTrue(TEXT("Unavailable physical time is null"),Source->HasTypedField<EJson::Null>(TEXT("timePhysicalSeconds")));
            TestEqual(TEXT("Step is an explicit coordinate value"),Source->GetNumberField(TEXT("timeValue")),10.);
            TestEqual(TEXT("Raw timeline unit retained"),Source->GetStringField(TEXT("timeUnit")),FString(TEXT("solver_step")));
        }
        TestTrue(TEXT("Provenance explicitly labels missing physical time"),RawProvenance.Contains(TEXT("physical time unavailable without dtSeconds")));
    }
    const FString RetainedParent=FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())/TEXT("tmp/debug/home4-native-vti-verification");IFileManager::Get().MakeDirectory(*RetainedParent,true);
    auto Retained=R;Retained.OutputParent=RetainedParent;Retained.FolderName=TEXT("artificial-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const auto Artifact=StudioHome4Archives::Convert(Retained);
    if(TestTrue(TEXT("Independent VTI verification artifact retained: ")+Artifact.Error,Artifact.bSuccess))
    {
        IFileManager::Get().Copy(*(Artifact.Path/TEXT("artificial-source-step10.npz")),*R.Sources[0].Path,false);
        IFileManager::Get().Copy(*(Artifact.Path/TEXT("artificial-source-step20.npz")),*R.Sources[1].Path,false);
        AddInfo(TEXT("Native artificial VTI artifact for independent read: ")+Artifact.Path);
    }
    return true;
}

namespace StudioHome4ArchiveTestPrivate
{
bool Reveal(FStudioHeadlessSlate& UI,const TSharedRef<SScrollBox>& Scroll,FName Tag)
{
    const auto Target=UI.Find(Tag);if(!Target)return false;
    Scroll->ScrollDescendantIntoView(Target,false,EDescendantScrollDestination::IntoView,8);
    Scroll->Tick(Scroll->GetCachedGeometry(),FPlatformTime::Seconds(),0);UI.Layout();return true;
}
class FNativeArchiveWorkflow final:public IAutomationLatentCommand
{
public:
    explicit FNativeArchiveWorkflow(FAutomationTestBase& T):Test(T){}
    ~FNativeArchiveWorkflow(){UI.Reset();Panel.Reset();Scroll.Reset();Model.Reset();Fixture.Reset();}
    bool Update()override
    {
        if(!Model)return Initialize();
        if(FPlatformTime::Seconds()-Started>20){Test.AddError(TEXT("Native archive widget stage exceeded its bounded 20-second deadline."));return true;}
        Panel->Tick(Panel->GetCachedGeometry(),FPlatformTime::Seconds(),0);Model->Tick(0);UI->Layout();
        if(Panel->IsOperationPendingForAutomation()||Model->IsRecordingLoadPending())return false;
        if(Stage==1)
        {
            if(!Test.TestEqual(TEXT("First source inspected through actual Add button"),Panel->SourceCountForAutomation(),1))return true;
            Retained();if(!Add(Second))return true;Next(2);return false;
        }
        if(Stage==2)
        {
            if(!Test.TestEqual(TEXT("Both original snapshots retained"),Panel->SourceCountForAutomation(),2))return true;
            if(!Fill())return true;
            // Request validation runs before the export picker; an invalid original anchor is transactional.
            if(!Type(TEXT("Home4ArchiveDt"),TEXT("nan"))||!Press(TEXT("Home4ArchiveImport")))return true;
            Test.TestTrue(TEXT("Bad original anchor visibly rejected"),UI->Text(TEXT("Home4ArchiveStatus")).Contains(TEXT("finite original unit anchors")));
            Retained();if(!Type(TEXT("Home4ArchiveDt"),TEXT("0.02")))return true;
            // No queued output result must cancel within the virtual test;
            // never fall through to a real macOS modal folder picker.
            if(!Press(TEXT("Home4ArchiveImport")))return true;
            Test.TestTrue(TEXT("Missing injected output folder cancels without a native dialog"),UI->Text(TEXT("Home4ArchiveStatus")).Contains(TEXT("Output folder selection cancelled"))&&!Panel->IsOperationPendingForAutomation());Retained();
            if(!Start(TEXT("scope-project")))return true;
            const auto P=Panel->ProgressForAutomation();Test.TestTrue(TEXT("Conversion progress is bounded by two original frames"),P.TotalFrames==2&&P.CompletedFrames>=0&&P.CompletedFrames<=2&&P.Bytes>=0);
            Model->Project.Id=FGuid::NewGuid();Next(3);return false;
        }
        if(Stage==3)
        {
            Retained();if(!Test.TestTrue(TEXT("Completed old-project result requires explicit Open"),!Panel->CompletedPathForAutomation().IsEmpty()&&UI->Text(TEXT("Home4ArchiveStatus")).Contains(TEXT("Project/source changed"))))return true;
            if(!Press(TEXT("Home4ArchiveOpenCompleted")))return true;
            Test.TestTrue(TEXT("Explicit Open launches the existing model recording reader"),Model->IsRecordingLoadPending());Retained();Next(4);return false;
        }
        if(Stage==4)
        {
            if(!CheckImported())return true;
            // A subsequent operation must bind to the original immutable source map,
            // even when the form is edited after it starts.
            if(!Start(TEXT("scope-source")))return true;
            Model->Solver=InitialSource;Model->Frames=InitialFrames;Model->SelectedFrame=InitialFrame;Model->Project.Dataset=InitialDataset;
            if(!Type(TEXT("Home4ArchiveDx"),TEXT("0.5")))return true;
            Next(5);return false;
        }
        if(Stage==5)
        {
            Retained();if(!Test.TestTrue(TEXT("Changed source requires explicit Open without replacing the viewer"),UI->Text(TEXT("Home4ArchiveStatus")).Contains(TEXT("Project/source changed"))))return true;
            if(!Press(TEXT("Home4ArchiveOpenCompleted")))return true;Retained();Next(6);return false;
        }
        if(Stage==6)
        {
            if(!CheckImported())return true;CaptureRetained();
            Panel->SetNextSourcePathForAutomation(First);if(!Press(TEXT("Home4ArchiveAddSource")))return true;
            if(!Test.TestTrue(TEXT("Inspection remains pending until the next controlled panel tick"),Panel->IsOperationPendingForAutomation())||!Press(TEXT("Home4ArchiveCancel")))return true;
            Next(7);return false;
        }
        if(Stage==7)
        {
            Test.TestEqual(TEXT("Cancelled inspection cannot append a source"),Panel->SourceCountForAutomation(),2);
            Test.TestTrue(TEXT("Routed cancellation status remains explicit"),UI->Text(TEXT("Home4ArchiveStatus")).Contains(TEXT("inspection cancelled")));Retained();
            // Matching scope retains the documented Convert-and-open behavior.
            if(!Type(TEXT("Home4ArchiveDx"),TEXT("0.1"))||!Start(TEXT("matching-scope")))return true;Next(8);return false;
        }
        if(Stage==8)
        {
            Test.TestTrue(TEXT("Matching scope automatically opens its authorized conversion"),Model->Solver!=RetainedSource);
            CheckImported();return true;
        }
        Test.AddError(TEXT("Unexpected native archive widget stage."));return true;
    }
private:
    bool Initialize()
    {
        Fixture=MakeUnique<FFixture>();First=Fixture->Save(TEXT("source10"),StudioHome4ArchiveFixtureData::Axes_xyz);Second=Fixture->Save(TEXT("source20"),StudioHome4ArchiveFixtureData::Frame20);
        Model=MakeShared<FStudioModel>(Fixture->Folder/TEXT("model"));Model->Project.Camera.Position=FVector(4,5,6);Model->Project.Camera.Focus=FVector(1,2,3);
        if(Model->Solver->FrameCount()>7)Model->ReviewRecordedFrame(7);
        InitialSource=Model->Solver;InitialFrames=Model->Frames;InitialFrame=Model->SelectedFrame;InitialDataset=Model->Project.Dataset;CaptureRetained();
        Panel=SNew(SStudioHome4ArchivePanel).Model(Model);
        // SWidget::Paint invokes Tick, so Find/Reveal/Press would otherwise poll
        // an inspection between the routed Add and Cancel events. The latent
        // command already drives the real panel Tick once per Update; keep
        // async completion at that explicit boundary for this fixture only.
        Panel->SetCanTick(false);
        Scroll=SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Panel.ToSharedRef()];
        UI=MakeUnique<FStudioHeadlessSlate>(Test,Scroll.ToSharedRef(),FVector2D(610,740));
        if(!Add(First))return true;Next(1);return false;
    }
    void Next(int32 S){Stage=S;Started=FPlatformTime::Seconds();}
    bool Press(FName Tag)
    {
        const bool Accepted=Reveal(*UI,Scroll.ToSharedRef(),Tag)&&UI->Press(Tag);
        if(!Accepted)Test.AddError(FString::Printf(TEXT("Native archive stage %d could not press %s: %s"),Stage,*Tag.ToString(),*UI->Text(TEXT("Home4ArchiveStatus"))));
        return Accepted;
    }
    bool Type(FName Tag,const FString& V)
    {
        const bool Accepted=Reveal(*UI,Scroll.ToSharedRef(),Tag)&&UI->Type(Tag,V);
        if(!Accepted)Test.AddError(FString::Printf(TEXT("Native archive stage %d could not edit %s: %s"),Stage,*Tag.ToString(),*UI->Text(TEXT("Home4ArchiveStatus"))));
        return Accepted;
    }
    bool Add(const FString& Path){Panel->SetNextSourcePathForAutomation(Path);return Press(TEXT("Home4ArchiveAddSource"));}
    bool Fill()
    {
        for(const FName Tag:{FName(TEXT("Home4ArchiveAxes_xyz")),FName(TEXT("Home4ArchiveMetadata_xyz")),FName(TEXT("Home4ArchiveCoordinateUnits_lattice")),FName(TEXT("Home4ArchiveVelocityUnits_lattice"))})if(!Press(Tag))return false;
        for(const auto& V:TArray<TPair<FName,FString>>{{TEXT("Home4ArchiveDx"),TEXT("0.1")},{TEXT("Home4ArchiveDt"),TEXT("0.02")},{TEXT("Home4ArchiveDensity"),TEXT("1000")},
            {TEXT("Home4ArchiveTimeOrigin"),TEXT("7")},{TEXT("Home4ArchiveCrop"),TEXT("1:7,2:8,1:9")},{TEXT("Home4ArchiveStride"),TEXT("2")},
            {TEXT("Home4ArchiveURI"),TEXT("urn:artificial-native-widget-fixture")},{TEXT("Home4ArchiveAttribution"),TEXT("Artificial numerical UI contract fixture; not solver output.")}})if(!Type(V.Key,V.Value))return false;
        return true;
    }
    bool Start(const TCHAR* Folder)
    {
        if(!Type(TEXT("Home4ArchiveFolder"),Folder))return false;
        Panel->SetNextOutputParentForAutomation(Fixture->Folder);
        if(!Press(TEXT("Home4ArchiveImport")))return false;
        return Test.TestTrue(TEXT("Routed native archive conversion starts"),Panel->IsOperationPendingForAutomation());
    }
    void CaptureRetained(){RetainedSource=Model->Solver;RetainedFrame=Model->SelectedFrame;RetainedCamera=Model->Project.Camera;RetainedDraft=StudioCaseIO::Serialize(Model->Project.Draft);}
    void Retained()
    {
        Test.TestTrue(TEXT("Pending/scoped import retains exact reader"),Model->Solver==RetainedSource);
        Test.TestEqual(TEXT("Pending/scoped import retains selected frame"),Model->SelectedFrame,RetainedFrame);
        Test.TestTrue(TEXT("Pending/scoped import retains camera"),StudioView::CameraEquals(Model->Project.Camera,RetainedCamera));
        Test.TestEqual(TEXT("Archive operation never edits next-run case"),StudioCaseIO::Serialize(Model->Project.Draft),RetainedDraft);
    }
    bool CheckImported()
    {
        if(!Test.TestTrue(TEXT("Native widget opened original structured source"),Model->Solver!=InitialSource&&Model->Solver->VolumeReconstruction()&&Model->Solver->VolumeReconstruction()->OriginalGrid))return false;
        const auto& G=*Model->Solver->VolumeReconstruction()->OriginalGrid;
        Test.TestEqual(TEXT("Widget keeps captured original dx after later edits"),G.Units.DxMeters.GetValue(),.1);
        Test.TestEqual(TEXT("Widget retains original dimensions"),G.OriginalDimensions,FIntVector(7,8,9));Test.TestEqual(TEXT("Widget retains half-open crop minimum"),G.CropMinimum,FIntVector(1,2,1));Test.TestEqual(TEXT("Widget identifies preview stride"),G.PreviewStride,2);
        Test.TestEqual(TEXT("Original frame count from supplied sources"),Model->Frames.Num(),2);
        Test.TestEqual(TEXT("Original step index retained"),Model->Frames[0].Index,10);Test.TestTrue(TEXT("Original epoch plus step time retained"),FMath::IsNearlyEqual(Model->Frames[0].Time,7.2,1.e-12));
        Test.TestEqual(TEXT("Original lineage retained through native widget"),G.LineageId,FString(TEXT("artificial-lineage")));
        Test.TestTrue(TEXT("Opening retains camera rather than fitting automatically"),StudioView::CameraEquals(Model->Project.Camera,RetainedCamera));
        Test.TestEqual(TEXT("Opening preserves original draft"),StudioCaseIO::Serialize(Model->Project.Draft),RetainedDraft);return true;
    }
    FAutomationTestBase& Test;TUniquePtr<FFixture> Fixture;TSharedPtr<FStudioModel> Model;TSharedPtr<SStudioHome4ArchivePanel> Panel;TSharedPtr<SScrollBox> Scroll;TUniquePtr<FStudioHeadlessSlate> UI;
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> InitialSource,RetainedSource;TArray<FStudioFrame> InitialFrames;FString First,Second,InitialDataset,RetainedDraft;
    FStudioCameraState RetainedCamera;int32 InitialFrame=0,RetainedFrame=0,Stage=0;double Started=0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4ArchiveCompactUI,"Studio.Home4.Archive.CompactAndWideRetainedForm",StudioHome4ArchiveTestPrivate::Flags)
bool FHome4ArchiveCompactUI::RunTest(const FString&)
{
    using namespace StudioHome4ArchiveTestPrivate;if(!TestTrue(TEXT("Slate initialized"),FSlateApplication::IsInitialized()))return false;FFixture F;
    auto Model=MakeShared<FStudioModel>(F.Folder/TEXT("layout-model"));
    for(const int32 Width:{610,960})
    {
        auto Panel=SNew(SStudioHome4ArchivePanel).Model(Model);auto Scroll=SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Panel];
        FStudioHeadlessSlate UI(*this,Scroll,FVector2D(Width,740));const FString Prefix=FString::Printf(TEXT("home4-archive-%d"),Width);
        if(!UI.Inspect(Prefix+TEXT("-source-conventions"),{TEXT("Home4ArchiveAddSource"),TEXT("Home4ArchiveAxes_xyz"),TEXT("Home4ArchiveAxes_zyx"),TEXT("Home4ArchiveMetadata_xyz"),TEXT("Home4ArchiveMetadata_array"),TEXT("Home4ArchiveCoordinateUnits_lattice"),TEXT("Home4ArchiveVelocityUnits_physical")}))return false;
        Panel->SetNextSourcePathForAutomation(TEXT(""));
        if(!UI.Press(TEXT("Home4ArchiveAddSource")))return false;
        TestTrue(TEXT("Cancelled source selection supplies a visible status"),UI.Text(TEXT("Home4ArchiveStatus")).Contains(TEXT("selection cancelled")));
        if(!Reveal(UI,Scroll,TEXT("Home4ArchiveStride"))||!UI.Type(TEXT("Home4ArchiveStride"),TEXT("2"))||!UI.Inspect(Prefix+TEXT("-crop-preview"),{TEXT("Home4ArchiveCrop"),TEXT("Home4ArchiveStride")}))return false;
        if(!Reveal(UI,Scroll,TEXT("Home4ArchiveStatus"))||!UI.Inspect(Prefix+TEXT("-publication-actions"),{TEXT("Home4ArchiveImport"),TEXT("Home4ArchiveVTI"),TEXT("Home4ArchiveCancel"),TEXT("Home4ArchiveOpenCompleted"),TEXT("Home4ArchiveStatus")}))return false;
        TestTrue(TEXT("Cancel is disabled without work"),UI.Find(TEXT("Home4ArchiveCancel"))&&!UI.Find(TEXT("Home4ArchiveCancel"))->IsEnabled());
        TestTrue(TEXT("Open completed requires verified publication"),UI.Find(TEXT("Home4ArchiveOpenCompleted"))&&!UI.Find(TEXT("Home4ArchiveOpenCompleted"))->IsEnabled());
        TestEqual(TEXT("Preview input remains retained while scrolling"),UI.Text(TEXT("Home4ArchiveStride")),FString(TEXT("2")));
    }
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4ArchiveNativeUI,"Studio.Home4.Archive.NativeInspectionConversionScopeAndOpen",StudioHome4ArchiveTestPrivate::Flags)
bool FHome4ArchiveNativeUI::RunTest(const FString&)
{
    if(!TestTrue(TEXT("Slate initialized"),FSlateApplication::IsInitialized()))return false;
    ADD_LATENT_AUTOMATION_COMMAND(StudioHome4ArchiveTestPrivate::FNativeArchiveWorkflow(*this));return true;
}
#endif
