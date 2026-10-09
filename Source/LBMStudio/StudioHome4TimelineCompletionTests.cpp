#include "SStudioHome4Timeline.h"
#include "StudioHeadlessSlate.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4TimelineTestPrivate
{
    constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
    FStudioHome4TailResult Append(FStudioHome4TelemetryStream& Stream,const TCHAR* Text)
    {const FTCHARToUTF8 Bytes(Text);return Stream.AppendBytes(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());}
    FReply Key(const TSharedRef<SStudioHome4Timeline>& Widget,FKey K)
    {return Widget->OnKeyDown(FGeometry(),FKeyEvent(K,FModifierKeysState(),0,false,0,0));}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4TimelineWithoutFieldsTest,"Studio.Home4.Readouts.TimelineWithoutFieldsAndLogExtent",StudioHome4TimelineTestPrivate::Flags)
bool FHome4TimelineWithoutFieldsTest::RunTest(const FString&)
{
    using namespace StudioHome4TimelineTestPrivate;
    // Artificial original log records exercise UI contracts, never CFD results.
    FStudioHome4TelemetryStream Stream;FStudioHome4Source Source{FGuid::NewGuid(),TEXT("timeline-no-fields")};Stream.BeginRun(Source);
    TestEqual(TEXT("Four-kind log and guard accepted"),Append(Stream,TEXT("{\"step\":0}\n{\"step\":100,\"trace\":\"trace.csv\",\"slice\":\"slice.npz\",\"snapshot\":\"viz.npz\",\"checkpoint\":\"state.npz\"}\n{\"step\":300,\"nonfinite\":true}\n{\"step\":350}\n")).Accepted,4);
    TOptional<FGuid> Run=Source.RunId;TArray<FStudioFrame> Empty;int32 Reviews=0;
    auto Widget=SNew(SStudioHome4Timeline).Tag(TEXT("Home4OriginalTimeline")).Telemetry([&]{return &Stream;}).SourceRun([&]{return Run;})
        .Frames([&]{return &Empty;}).Review([&](int32){++Reviews;});
    FStudioHeadlessSlate UI(*this,Widget,FVector2D(500,88));
    TestTrue(TEXT("Four-lane native timeline arranged without fields"),UI.Inspect(TEXT("home4-timeline-no-fields"),{TEXT("Home4OriginalTimeline")}));
    const auto Extent=Widget->RetainedStepExtent();if(!TestTrue(TEXT("Original log supplies extent without frames"),Extent.IsSet()))return false;
    TestEqual(TEXT("Original measurement supplies lower extent"),Extent->Key,int64(0));TestEqual(TEXT("Later original measurement supplies upper extent"),Extent->Value,int64(350));
    const auto Guards=Widget->RetainedGuardSteps();TestEqual(TEXT("Retained guard marked separately from checkpoint"),Guards.Num(),1);
    if(!Guards.IsEmpty())TestEqual(TEXT("Guard occurrence never uses last-good step"),Guards[0],int64(300));
    TestTrue(TEXT("Native timeline receives focus"),UI.Focus(TEXT("Home4OriginalTimeline")));UI.Key(EKeys::Home);
    for(int32 Kind=0;Kind<4;++Kind)
    {
        const auto Event=Widget->SelectedOutputEvent();if(!TestTrue(TEXT("Original output remains inspectable without any fields"),Event.IsSet()))return false;
        TestEqual(TEXT("Four original output kinds keep their own markers"),int32(Event->Kind),Kind);if(Kind<3)UI.Key(EKeys::Right);
    }
    TestEqual(TEXT("No field source means no replay movement"),Reviews,0);
    // The same positioning is used by native painting and marker hit testing.
    const auto G=Widget->GetCachedGeometry();const auto P=G.LocalToAbsolute(FVector2D(60+(G.GetLocalSize().X-66)*100./350.,7));
    Widget->OnMouseButtonDown(G,FPointerEvent(0,P,P,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
    TestTrue(TEXT("Trace hit testing works with zero field frames"),Widget->SelectedOutputEvent()&&Widget->SelectedOutputEvent()->Kind==EStudioHome4OutputKind::Trace);
    Run=FGuid::NewGuid();UI.Layout();TestFalse(TEXT("Foreign identity clears previous selection"),Widget->SelectedOutputEvent().IsSet());
    TestFalse(TEXT("Foreign source cannot inspect old log"),Key(Widget,EKeys::Home).IsEventHandled());TestTrue(TEXT("Foreign source guard hidden"),Widget->RetainedGuardSteps().IsEmpty());
    Run.Reset();TestFalse(TEXT("Missing original identity cannot navigate"),Key(Widget,EKeys::Right).IsEventHandled());TestFalse(TEXT("Missing source has no guessed extent"),Widget->RetainedStepExtent().IsSet());
    Run=Source.RunId;Stream.BeginRun(Source);Append(Stream,TEXT("{\"step\":50,\"checkpoint\":\"single.npz\"}\n"));UI.Layout();
    const auto Center=G.LocalToAbsolute(FVector2D(60+(G.GetLocalSize().X-66)*.5,58));
    Widget->OnMouseButtonDown(G,FPointerEvent(0,Center,Center,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
    TestTrue(TEXT("A single original step centers the restart marker"),Widget->SelectedOutputEvent()&&Widget->SelectedOutputEvent()->Path==TEXT("single.npz"));
    FStudioHome4TailLimits Limits;Limits.MaxHistory=1;FStudioHome4TelemetryStream Bounded(Limits);Bounded.BeginRun(Source);
    Append(Bounded,TEXT("{\"step\":300,\"nonfinite\":true}\n{\"step\":350}\n"));
    auto GuardOnly=SNew(SStudioHome4Timeline).Telemetry([&]{return &Bounded;}).SourceRun([&]{return Run;});
    TestEqual(TEXT("Guard sample is evicted from bounded measurement history"),Bounded.History().Num(),1);
    TestEqual(TEXT("Only later original sample remains"),Bounded.History()[0].Step.Get(0),int64(350));
    const auto RetainedGuards=GuardOnly->RetainedGuardSteps();TestEqual(TEXT("Original guard remains after its measurement is evicted"),RetainedGuards.Num(),1);
    if(!RetainedGuards.IsEmpty())TestEqual(TEXT("Retained guard keeps its actual original step"),RetainedGuards[0],int64(300));
    if(TestTrue(TEXT("Retained guard and later sample establish original extent"),GuardOnly->RetainedStepExtent().IsSet()))
        TestEqual(TEXT("Guard occurrence contributes retained lower extent"),GuardOnly->RetainedStepExtent()->Key,int64(300));
    Run=FGuid::NewGuid();TestTrue(TEXT("Foreign original scope hides evicted-sample guard"),GuardOnly->RetainedGuardSteps().IsEmpty());
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4TimelineLateCheckpointTest,"Studio.Home4.Readouts.TimelineLateCheckpointAndStaleMenu",StudioHome4TimelineTestPrivate::Flags)
bool FHome4TimelineLateCheckpointTest::RunTest(const FString&)
{
    using namespace StudioHome4TimelineTestPrivate;FStudioHome4TelemetryStream Stream;
    FStudioHome4Source Source{FGuid::NewGuid(),TEXT("timeline-late-checkpoint")};Stream.BeginRun(Source);
    Append(Stream,TEXT("{\"step\":100,\"snapshot\":\"viz100.npz\"}\n{\"step\":900,\"checkpoint\":\"last-good.npz\"}\n"));
    TArray<FStudioFrame> Frames;FStudioFrame Frame;Frame.Index=100;Frames.Add(Frame);TOptional<FGuid> Run=Source.RunId;
    int32 Reviews=0,Starts=0;FStudioHome4OutputEvent Received;
    auto Widget=SNew(SStudioHome4Timeline).Telemetry([&]{return &Stream;}).SourceRun([&]{return Run;}).FieldSourceRun([&]{return Run;})
        .Frames([&]{return &Frames;}).Review([&](int32){++Reviews;}).WarmStart([&](const auto& Event){++Starts;Received=Event;});
    Key(Widget,EKeys::End);const auto Selected=Widget->SelectedOutputEvent();
    if(!TestTrue(TEXT("Checkpoint after final field frame remains navigable"),Selected&&Selected->Kind==EStudioHome4OutputKind::Restart&&Selected->Step.Get(0)==900))return false;
    TestEqual(TEXT("Late checkpoint never moves replay"),Reviews,0);TestEqual(TEXT("Original checkpoint extends past viz"),Widget->RetainedStepExtent()->Value,int64(900));
    Frames.Reset();const auto Menu=Widget->MakeRestartMenu(*Selected);if(!TestTrue(TEXT("Actual context action available without fields"),Menu.IsValid()))return false;
    {
        FStudioHeadlessSlate UI(*this,Menu.ToSharedRef(),FVector2D(400,44));
        TestTrue(TEXT("Actual native warm-start button activates"),UI.Press(TEXT("Home4TimelineWarmStart")));
        TestEqual(TEXT("Native menu invokes one original checkpoint"),Starts,1);TestEqual(TEXT("Warm start keeps original path"),Received.Path,FString(TEXT("last-good.npz")));
        TestEqual(TEXT("Warm start keeps exact original record"),Received.RecordIndex,Selected->RecordIndex);
        TestTrue(TEXT("Warm start keeps original run identity"),Received.Source.RunId==Source.RunId);
        Run=FGuid::NewGuid();TestTrue(TEXT("Retained menu action can be pressed after source replacement"),UI.Press(TEXT("Home4TimelineWarmStart")));
        TestEqual(TEXT("Stale source menu fails closed"),Starts,1);
        Run=Source.RunId;Stream.BeginRun(Source);Append(Stream,TEXT("{\"step\":900,\"checkpoint\":\"changed.npz\"}\n"));
        UI.Press(TEXT("Home4TimelineWarmStart"));TestEqual(TEXT("Replaced original event cannot reuse retained action"),Starts,1);
    }
    TestEqual(TEXT("Checkpoint menu never changes field replay"),Reviews,0);return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4TimelineExactIdentityTest,"Studio.Home4.Readouts.TimelineExactFieldIdentity",StudioHome4TimelineTestPrivate::Flags)
bool FHome4TimelineExactIdentityTest::RunTest(const FString&)
{
    using namespace StudioHome4TimelineTestPrivate;FStudioHome4TelemetryStream Stream;FStudioHome4Source Source{FGuid::NewGuid(),TEXT("timeline-exact-fields")};Stream.BeginRun(Source);
    Append(Stream,TEXT("{\"step\":100,\"trace\":\"trace.csv\",\"slice\":\"slice.npz\",\"snapshot\":\"viz100.npz\",\"checkpoint\":\"restart.npz\"}\n{\"step\":150,\"snapshot\":\"viz150.npz\"}\n"));
    TArray<FStudioFrame> Frames;FStudioFrame Frame;Frame.Index=50;Frames.Add(Frame);Frame.Index=100;Frames.Add(Frame);Frame.Index=200;Frames.Add(Frame);
    TOptional<FGuid> Run=Source.RunId,FieldRun;int32 Reviews=0,Ordinal=INDEX_NONE;
    auto Widget=SNew(SStudioHome4Timeline).Telemetry([&]{return &Stream;}).SourceRun([&]{return Run;}).FieldSourceRun([&]{return FieldRun;})
        .Frames([&]{return &Frames;}).Review([&](int32 I){++Reviews;Ordinal=I;});
    auto Viz=[&]{Key(Widget,EKeys::Home);Key(Widget,EKeys::Right);Key(Widget,EKeys::Right);};
    Viz();TestEqual(TEXT("Missing field identity cannot move replay"),Reviews,0);TestEqual(TEXT("Missing field identity cannot extend original log bounds"),Widget->RetainedStepExtent()->Key,int64(100));
    FieldRun=FGuid::NewGuid();Viz();TestEqual(TEXT("Foreign field identity cannot move replay"),Reviews,0);TestEqual(TEXT("Foreign field frames do not extend log"),Widget->RetainedStepExtent()->Value,int64(150));
    FieldRun=Source.RunId;Viz();TestEqual(TEXT("Exact source run and solver step review once"),Reviews,1);TestEqual(TEXT("Review retains exact original frame ordinal"),Ordinal,1);
    TestEqual(TEXT("Matching field frame extends lower extent"),Widget->RetainedStepExtent()->Key,int64(50));TestEqual(TEXT("Matching field frame extends upper extent"),Widget->RetainedStepExtent()->Value,int64(200));
    Key(Widget,EKeys::Right);Key(Widget,EKeys::End);TestEqual(TEXT("Restart and unmatched viz never substitute nearby frame"),Reviews,1);
    Run=FGuid::NewGuid();TestFalse(TEXT("Old event selection does not highlight a different source"),Widget->SelectedOutputEvent().IsSet());
    TestFalse(TEXT("Foreign original log identity blocks review"),Key(Widget,EKeys::Home).IsEventHandled());TestEqual(TEXT("Foreign source preserves replay"),Reviews,1);
    Run=FGuid();TestFalse(TEXT("Invalid GUID is not original identity"),Key(Widget,EKeys::Home).IsEventHandled());return !HasAnyErrors();
}
#endif
