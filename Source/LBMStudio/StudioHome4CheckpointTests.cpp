#include "StudioHome4Checkpoint.h"
#include "StudioHome4CheckpointFixtures.inl"
#include "StudioHome4Session.h"
#include "StudioModel.h"
#include "SStudioHome4Checkpoint.h"
#include "StudioHeadlessSlate.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4CheckpointTestPrivate
{
    constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
    struct FFixture
    {
        FString Root = FPaths::ProjectDir() / TEXT("tmp/debug/home4-checkpoint") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
        FFixture() { IFileManager::Get().MakeDirectory(*Root, true); }
        ~FFixture() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
        FString Save(const TCHAR* Name, const TCHAR* Encoded)
        { const FString Path = Root / (FString(Name) + TEXT(".npz")); TArray<uint8> Bytes; FBase64::Decode(Encoded, Bytes); FFileHelper::SaveArrayToFile(Bytes, *Path); return Path; }
        FStudioHome4Spec Spec(const FString& Path) const
        { FStudioHome4Spec S; S.Lattice.Extents = FIntVector(2, 3, 4); S.Run.InitState = Path; S.Run.Steps = 100; return S; }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4CheckpointAttestationTest, "Studio.Home4.Checkpoint.OriginalAttestationGridAndTensorFamilies", StudioHome4CheckpointTestPrivate::Flags)
bool FHome4CheckpointAttestationTest::RunTest(const FString&)
{
    using namespace StudioHome4CheckpointTestPrivate; using namespace StudioHome4CheckpointFixtureData;
    FFixture F; const auto Path = F.Save(TEXT("original"), Valid); auto S = F.Spec(Path);
    const auto R = StudioHome4Checkpoints::Inspect(Path, S);
    TestTrue(TEXT("Actual original attestation and streamed CRCs permit frontend grid check: ") + R.Error, R.State == EStudioHome4CheckpointState::GridCompatible);
    TestEqual(TEXT("Original source SHA retained"), R.Source.SHA256.Len(), 64);
    TestEqual(TEXT("Original source dimensions are XYZ"), R.DimensionsXYZ.Get(FIntVector::ZeroValue), FIntVector(2, 3, 4));
    TestEqual(TEXT("Original zyx array axes retained"), R.AxisOrder, FString(TEXT("zyx")));
    TestEqual(TEXT("Producer full state list retained"), R.RequiredMembers.Num(), 10);
    TestEqual(TEXT("Original solver step retained"), R.OriginalStep.Get(-1), 17);
    TestTrue(TEXT("Actual original metadata retained"), R.OriginalRunSpecJSON.Contains(TEXT("not CFD or HOME4 driver output")));
    TestTrue(TEXT("Unclassified source key keeps unknown restart status"), R.UndeclaredMembers.Contains(TEXT("unclassified")));
    S.Lattice.Extents = FIntVector(3, 3, 4);
    const auto Wrong = StudioHome4Checkpoints::Inspect(Path, S);
    TestTrue(TEXT("Known original-next-run grid mismatch blocks warm start"), Wrong.State == EStudioHome4CheckpointState::GridMismatch);
    TestTrue(TEXT("Mismatch reports both original and next grid"), Wrong.Error.Contains(TEXT("2 × 3 × 4")) && Wrong.Error.Contains(TEXT("3 × 3 × 4")));
    S.Lattice.Extents.Reset();
    TestTrue(TEXT("Unknown next grid stays unavailable"), StudioHome4Checkpoints::Inspect(Path, S).State == EStudioHome4CheckpointState::Unavailable);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4CheckpointMultilevelTest, "Studio.Home4.Checkpoint.CompleteMultilevelPatchEvidence", StudioHome4CheckpointTestPrivate::Flags)
bool FHome4CheckpointMultilevelTest::RunTest(const FString&)
{
    using namespace StudioHome4CheckpointTestPrivate; using namespace StudioHome4CheckpointFixtureData; FFixture F;
    const auto Path = F.Save(TEXT("multilevel"), Multilevel); auto S = F.Spec(Path);
    S.Multidomain.Levels = 2; S.Multidomain.LevelCells = {24, 8};
    FStudioHome4AuthoredPatch Patch; Patch.Id = TEXT("fine-body"); Patch.BodyId = TEXT("body"); Patch.Level = 1;
    Patch.Origin = FVector(.5, 1., 1.5); Patch.Extents = FIntVector(2, 2, 2); Patch.bFollowBody = true; S.Authoring.Patches.Add(Patch);
    const auto R = StudioHome4Checkpoints::Inspect(Path, S);
    TestTrue(TEXT("Actual original root and fine state families permit complete frontend grid check: ") + R.Error, R.State == EStudioHome4CheckpointState::GridCompatible);
    TestEqual(TEXT("All source-declared required keys retained across grids"), R.RequiredMembers.Num(), 20);
    TestEqual(TEXT("All original refinement levels retained"), R.LevelCount, 2);
    TestEqual(TEXT("Original fine patch metadata retained"), R.Patches.Num(), 1);
    if (R.Patches.Num() == 1)
    {
        TestEqual(TEXT("Original patch XYZ extents retained"), R.Patches[0].DimensionsXYZ, FIntVector(2, 2, 2));
        TestTrue(TEXT("Original patch origin retained in root cells"), R.Patches[0].OriginXYZ == Patch.Origin);
        TestEqual(TEXT("Original patch required state list retained"), R.Patches[0].RequiredMembers.Num(), 10);
    }
    auto Prepared = StudioHome4Checkpoints::Prepare(Path, S);
    TestTrue(TEXT("Complete multilevel imported source gets immutable prepared lease"), Prepared.Lease.IsValid());
    for (int32 Which = 0; Which < 7; ++Which)
    {
        auto Wrong = S;
        if (Which == 0) Wrong.Authoring.Patches[0].Id = TEXT("different-id");
        if (Which == 1) Wrong.Authoring.Patches[0].BodyId = TEXT("different-body");
        if (Which == 2) Wrong.Authoring.Patches[0].Origin.X += .25;
        if (Which == 3) Wrong.Authoring.Patches[0].Extents.Z = 3;
        if (Which == 4) Wrong.Authoring.Patches[0].bFollowBody = false;
        if (Which == 5) Wrong.Multidomain.LevelCells[1] = 9;
        if (Which == 6) Wrong.Authoring.Patches[0].Level = 0;
        TestTrue(FString::Printf(TEXT("Exact next-run patch property %d mismatch blocks warm start"), Which), StudioHome4Checkpoints::Inspect(Path, Wrong).State == EStudioHome4CheckpointState::GridMismatch);
    }
    auto MissingDescriptor = S; MissingDescriptor.Authoring.Patches.Reset();
    TestTrue(TEXT("No next-run patch origin is invented from archive metadata"), StudioHome4Checkpoints::Inspect(Path, MissingDescriptor).State == EStudioHome4CheckpointState::GridMismatch);
    const auto MissingPath = F.Save(TEXT("missing-patch-family"), MissingPatchFamily);
    TestTrue(TEXT("Missing fine-grid moment family blocks complete state attestation"), StudioHome4Checkpoints::Inspect(MissingPath, S).State == EStudioHome4CheckpointState::Rejected);
    const auto IncompletePath = F.Save(TEXT("incomplete-level"), IncompleteLevel);
    TestTrue(TEXT("Missing declared refinement level state stays unavailable"), StudioHome4Checkpoints::Inspect(IncompletePath, S).State == EStudioHome4CheckpointState::Unavailable);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4CheckpointStaticPatchTest, "Studio.Home4.Checkpoint.StaticPatchEmptyBodyIdentity", StudioHome4CheckpointTestPrivate::Flags)
bool FHome4CheckpointStaticPatchTest::RunTest(const FString&)
{
    using namespace StudioHome4CheckpointTestPrivate; using namespace StudioHome4CheckpointFixtureData; FFixture F;
    const auto Path = F.Save(TEXT("static-patch"), StaticPatch); auto S = F.Spec(Path);
    S.Multidomain.Levels = 2; S.Multidomain.LevelCells = {24, 8};
    FStudioHome4AuthoredPatch Patch; Patch.Id = TEXT("fine-body"); Patch.BodyId = TEXT(""); Patch.Level = 1;
    Patch.Origin = FVector(.5, 1., 1.5); Patch.Extents = FIntVector(2, 2, 2); Patch.bFollowBody = false; S.Authoring.Patches.Add(Patch);
    const auto R = StudioHome4Checkpoints::Inspect(Path, S);
    TestTrue(TEXT("Explicit empty original bodyId is compatible for a static patch: ") + R.Error, R.State == EStudioHome4CheckpointState::GridCompatible);
    if (TestEqual(TEXT("Original static patch retained"), R.Patches.Num(), 1))
        TestTrue(TEXT("Empty body identity and static policy retained exactly"), R.Patches[0].BodyId.IsEmpty() && !R.Patches[0].bFollowBody);
    auto Wrong = S; Wrong.Authoring.Patches[0].BodyId = TEXT("body");
    TestTrue(TEXT("Static patch body binding must match exact next config"), StudioHome4Checkpoints::Inspect(Path, Wrong).State == EStudioHome4CheckpointState::GridMismatch);
    Wrong = S; Wrong.Authoring.Patches[0].bFollowBody = true;
    TestTrue(TEXT("Static patch following policy must match exact next config"), StudioHome4Checkpoints::Inspect(Path, Wrong).State == EStudioHome4CheckpointState::GridMismatch);
    const auto Missing = F.Save(TEXT("missing-static-body-id"), MissingStaticBodyId);
    TestTrue(TEXT("Missing bodyId cannot inherit explicit static emptiness"), StudioHome4Checkpoints::Inspect(Missing, S).State == EStudioHome4CheckpointState::Rejected);
    const auto Following = F.Save(TEXT("following-empty-body-id"), FollowingEmptyBodyId);
    TestTrue(TEXT("Following patch cannot have an empty original bodyId"), StudioHome4Checkpoints::Inspect(Following, S).State == EStudioHome4CheckpointState::Rejected);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4CheckpointRejectTest, "Studio.Home4.Checkpoint.VizUnknownAndIncompleteStateBlocked", StudioHome4CheckpointTestPrivate::Flags)
bool FHome4CheckpointRejectTest::RunTest(const FString&)
{
    using namespace StudioHome4CheckpointTestPrivate; using namespace StudioHome4CheckpointFixtureData; FFixture F;
    for (const auto& Pair : TArray<TPair<const TCHAR*, const TCHAR*>>{{TEXT("viz-with-tensors"), Viz}, {TEXT("declared-viz"), ContradictoryViz}, {TEXT("missing-tensor"), MissingTensor}, {TEXT("wrong-components"), WrongComponents}, {TEXT("crc"), BadCRC}})
    {
        const auto Path = F.Save(Pair.Key, Pair.Value); const auto R = StudioHome4Checkpoints::Inspect(Path, F.Spec(Path));
        TestTrue(FString(Pair.Key) + TEXT(" rejected despite ux/phi presence"), R.State == EStudioHome4CheckpointState::Rejected);
        TestFalse(TEXT("Unsupported original cannot gain a prepared lease"), StudioHome4Checkpoints::Prepare(Path, F.Spec(Path)).Lease.IsValid());
    }
    for (const auto& Pair : TArray<TPair<const TCHAR*, const TCHAR*>>{{TEXT("undeclared"), NoAttestation}, {TEXT("metadata-unavailable"), MissingMetadata}})
    {
        const auto Path = F.Save(Pair.Key, Pair.Value); const auto R = StudioHome4Checkpoints::Inspect(Path, F.Spec(Path));
        TestTrue(FString(Pair.Key) + TEXT(" explicitly remains unavailable"), R.State == EStudioHome4CheckpointState::Unavailable);
    }
    const auto Path = F.Save(TEXT("cancelled"), Valid); auto C = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(true);
    const auto Cancelled = StudioHome4Checkpoints::Prepare(Path, F.Spec(Path), C);
    TestTrue(TEXT("Worker cancellation prevents a prepared lease"), Cancelled.Inspection.bCancelled && !Cancelled.Lease);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4CheckpointCopyRaceTest, "Studio.Home4.Checkpoint.PreparedCopyAndOriginalRace", StudioHome4CheckpointTestPrivate::Flags)
bool FHome4CheckpointCopyRaceTest::RunTest(const FString&)
{
    using namespace StudioHome4CheckpointTestPrivate; using namespace StudioHome4CheckpointFixtureData; FFixture F;
    const auto Path = F.Save(TEXT("pin-original"), Valid); const auto S = F.Spec(Path);
    auto P = StudioHome4Checkpoints::Prepare(Path, S);
    if (!TestTrue(TEXT("Compatible actual source has owned private preparation: ") + P.Inspection.Error, P.Lease.IsValid())) return false;
    TestTrue(TEXT("Prepared path differs from original source"), P.Lease->PreparedPath() != P.Lease->SourcePath());
    TestEqual(TEXT("Immutable prepared SHA records original bytes"), P.Lease->SHA256(), P.Inspection.Source.SHA256);
    TestEqual(TEXT("Next-run identity is SHA256"), P.Lease->SpecIdentity().Len(), 64);
    F.Save(TEXT("pin-original"), Viz);
    TestTrue(TEXT("Replacing source cannot mutate the owned queue lease"), StudioHome4Checkpoints::Inspect(P.Lease->PreparedPath(), S).State == EStudioHome4CheckpointState::GridCompatible);
    const FString PreparedPath = P.Lease->PreparedPath(); P.Lease.Reset();
    TestFalse(TEXT("Last immutable lease cleans up its private bytes"), IFileManager::Get().FileExists(*PreparedPath));
    F.Save(TEXT("pin-original"), Valid);
    const auto Raced = StudioHome4Checkpoints::PrepareWithCopyBoundaryForAutomation(Path, S, {}, [&F] { F.Save(TEXT("pin-original"), Viz); });
    TestFalse(TEXT("Original replacement at real copy verification boundary blocks lease"), Raced.Lease.IsValid());
    TestTrue(TEXT("Source race reports original identity failure"), Raced.Inspection.Error.Contains(TEXT("changed while prepared")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4CheckpointBoundsTest, "Studio.Home4.Checkpoint.MetadataAndArchiveBounds", StudioHome4CheckpointTestPrivate::Flags)
bool FHome4CheckpointBoundsTest::RunTest(const FString&)
{
    using namespace StudioHome4CheckpointTestPrivate; using namespace StudioHome4CheckpointFixtureData; FFixture F;
    const auto ArrayPath = F.Save(TEXT("array-iteration"), ArrayIteration);
    const auto Array = StudioHome4Checkpoints::Inspect(ArrayPath, F.Spec(ArrayPath));
    TestTrue(TEXT("Array-shaped step rejected before numeric allocation"), Array.State == EStudioHome4CheckpointState::Rejected && Array.Error.Contains(TEXT("scalar integer metadata")));
    const auto GridPath = F.Save(TEXT("oversize-grid"), OversizeGrid);
    const auto Grid = StudioHome4Checkpoints::Inspect(GridPath, F.Spec(GridPath));
    TestTrue(TEXT("Checkpoint cannot exceed native two million node bound"), Grid.State == EStudioHome4CheckpointState::Rejected && Grid.Error.Contains(TEXT("two million")));
    const FString Large = F.Root / TEXT("oversize-sparse.npz");
    {
        TUniquePtr<FArchive> File(IFileManager::Get().CreateFileWriter(*Large));
        if (!TestTrue(TEXT("Create bounded sparse archive fixture"), File.IsValid())) return false;
        File->Seek(StudioHome4Archives::MaximumArchiveBytes); uint8 Byte = 0; File->Serialize(&Byte, 1); File->Close();
    }
    const auto Archive = StudioHome4Checkpoints::Inspect(Large, F.Spec(Large));
    TestTrue(TEXT("Oversize archive rejected before reading payloads"), Archive.State == EStudioHome4CheckpointState::Rejected && Archive.Error.Contains(TEXT("512 MiB")));
    TestTrue(TEXT("No source hash claimed for rejected oversized bytes"), Archive.Source.SHA256.IsEmpty());
    return true;
}

namespace StudioHome4CheckpointTestPrivate
{
    class FSessionWorkflow final : public IAutomationLatentCommand
    {
    public:
        explicit FSessionWorkflow(FAutomationTestBase& InTest) : Test(InTest)
        {
            Path = F.Save(TEXT("session"), StudioHome4CheckpointFixtureData::Valid); Spec = F.Spec(Path);
            M = MakeShared<FStudioModel>(F.Root); M->Project.Draft.Home4 = Spec;
            E = MakeShared<FStudioHome4Session>(M); Session = MakeShared<FStudioHome4CheckpointSession>(M, E);
            Widget = SNew(SStudioHome4Checkpoint).Model(M).Editor(E).Session(Session);
            UI = MakeUnique<FStudioHeadlessSlate>(Test, Widget.ToSharedRef(), FVector2D(760, 480));
            Test.TestTrue(TEXT("Native controls arranged with actual shared session"), UI->Inspect(TEXT("home4-checkpoint-controls"), {TEXT("Home4CheckpointPath"), TEXT("Home4CheckpointPick"), TEXT("Home4CheckpointInspect"), TEXT("Home4CheckpointCancel"), TEXT("Home4CheckpointStatus")}));
            Test.TestTrue(TEXT("Native Inspect routes the asynchronous preparation"), UI->Press(TEXT("Home4CheckpointInspect")));
            Started = FPlatformTime::Seconds();
        }
        bool Update() override
        {
            Session->Tick(); UI->Layout();
            if (FPlatformTime::Seconds() - Started > 20) { Test.AddError(TEXT("Checkpoint asynchronous workflow exceeded its bounded deadline.")); return true; }
            if (Session->IsBusy()) return false;
            FString Error; TSharedPtr<const FStudioHome4PreparedCheckpoint, ESPMode::ThreadSafe> Lease;
            if (Stage == 0)
            {
                if (!Test.TestTrue(TEXT("Current scoped request can Submit its prepared lease: ") + Session->Status(), Session->ValidateForSubmit(Spec, Lease, Error))) return true;
                Test.TestTrue(TEXT("Native status is frontend compatibility and driver unavailability"), UI->Text(TEXT("Home4CheckpointStatus")).Contains(TEXT("Frontend grid compatible")) && UI->Text(TEXT("Home4CheckpointStatus")).Contains(TEXT("validation remains unavailable")));
                E->Set(TEXT("run.steps"), TEXT("101"));
                Test.TestFalse(TEXT("Any next-run draft edit invalidates Submit verification"), Session->ValidateForSubmit(Spec, Lease, Error));
                E->Revert(); E->Build(Spec, Error);
                Test.TestTrue(TEXT("Start source worker before draft changes"), Session->Start(Path, Spec, Error));
                E->Set(TEXT("run.steps"), TEXT("102")); Session->Tick(); Stage = 1; return false;
            }
            if (Stage == 1)
            {
                Test.TestTrue(TEXT("A stale worker cannot restore previous draft approval"), Session->State() == EStudioHome4CheckpointState::Unavailable);
                E->Build(Spec, Error); Test.TestFalse(TEXT("Changed draft cannot reuse previous result"), Session->ValidateForSubmit(Spec, Lease, Error));
                Test.TestTrue(TEXT("Start actual source before case changes"), Session->Start(Path, Spec, Error));
                M->Project.Draft.Id = FGuid::NewGuid(); Session->Tick(); Stage = 2; return false;
            }
            if (Stage == 2)
            {
                Test.TestTrue(TEXT("Case change drops worker completion"), Session->State() == EStudioHome4CheckpointState::Unavailable);
                Test.TestFalse(TEXT("Different case cannot reuse verified source"), Session->ValidateForSubmit(Spec, Lease, Error));
                E->Refresh(); E->Build(Spec, Error); Test.TestTrue(TEXT("New scoped case may inspect explicitly"), Session->Start(Path, Spec, Error));
                Test.TestTrue(TEXT("Native Cancel action is routed"), UI->Press(TEXT("Home4CheckpointCancel"))); Stage = 3; return false;
            }
            if (Stage == 3)
            {
                Test.TestTrue(TEXT("Cancelled completion never restores compatible state"), Session->State() == EStudioHome4CheckpointState::Cancelled);
                Test.TestFalse(TEXT("Cancellation blocks unsafe Submit"), Session->ValidateForSubmit(Spec, Lease, Error));
                Test.TestTrue(TEXT("Start worker before project changes"), Session->Start(Path, Spec, Error));
                M->Project.Id = FGuid::NewGuid(); Session->Tick(); Stage = 4; return false;
            }
            Test.TestTrue(TEXT("Project change drops worker completion"), Session->State() == EStudioHome4CheckpointState::Unavailable);
            Test.TestFalse(TEXT("Different project cannot reuse checkpoint"), Session->ValidateForSubmit(Spec, Lease, Error));
            SStudioHome4Checkpoint::SetNextPathForAutomation(TEXT(""));
            Test.TestTrue(TEXT("Native picker cancellation routes without a source read"), UI->Press(TEXT("Home4CheckpointPick")));
            Test.TestTrue(TEXT("Picker cancellation visible"), UI->Text(TEXT("Home4CheckpointStatus")).Contains(TEXT("selection cancelled")));
            return true;
        }
    private:
        FAutomationTestBase& Test; FFixture F; FString Path; FStudioHome4Spec Spec;
        TSharedPtr<FStudioModel> M; TSharedPtr<FStudioHome4Session> E; TSharedPtr<FStudioHome4CheckpointSession> Session;
        TSharedPtr<SStudioHome4Checkpoint> Widget; TUniquePtr<FStudioHeadlessSlate> UI; double Started = 0; int32 Stage = 0;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4CheckpointScopedSessionTest, "Studio.Home4.Checkpoint.NativeAsyncScopeRaceAndCancel", StudioHome4CheckpointTestPrivate::Flags)
bool FHome4CheckpointScopedSessionTest::RunTest(const FString&)
{ ADD_LATENT_AUTOMATION_COMMAND(StudioHome4CheckpointTestPrivate::FSessionWorkflow(*this)); return true; }
#endif
