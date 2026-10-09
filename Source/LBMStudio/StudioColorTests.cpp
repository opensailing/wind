#include "StudioColor.h"
#include "StudioModel.h"
#include "StudioPointRecording.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto ColorFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
struct FColorFiles
{
    FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/Colors")/FGuid::NewGuid().ToString();
    ~FColorFiles(){IFileManager::Get().DeleteDirectory(*Root,false,true);}
};
bool FinishColorLoad(FStudioModel& M)
{
    const double Deadline=FPlatformTime::Seconds()+20;
    while((M.IsRecordingLoadPending()||M.IsProjectOpenPending())&&FPlatformTime::Seconds()<Deadline)
    {M.Tick(0);FPlatformProcess::Sleep(.001f);}
    return !M.IsRecordingLoadPending()&&!M.IsProjectOpenPending();
}
FString ColorJSON(const TSharedPtr<FJsonObject>& Object)
{FString Result;FJsonSerializer::Serialize(Object.ToSharedRef(),TJsonWriterFactory<>::Create(&Result));return Result;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioColorMappingTest,"Studio.Colors.MappingAndTextValidation",ColorFlags)
bool FStudioColorMappingTest::RunTest(const FString&)
{
    FStudioScalarDescriptor Pressure{TEXT("pressure"),TEXT("Pressure"),TEXT("Pa"),-600,500,TEXT("supplied")};
    TArray<FStudioScalarStyle> Styles={{TEXT("sourceA"),TEXT("pressure"),1,true,-100,100}};
    const auto C=StudioColor::Resolve(TEXT("sourceA"),Pressure,Styles);
    TestTrue(TEXT("Diverging midpoint is neutral at zero in a symmetric range"),StudioColor::Map(0,C).Equals(FLinearColor(.94,.94,.94),1.e-6));
    TestTrue(TEXT("Values below range use its low endpoint"),StudioColor::Map(-1000,C)==StudioColor::Map(-100,C));
    TestTrue(TEXT("Values above range use its high endpoint"),StudioColor::Map(1000,C)==StudioColor::Map(100,C));
    const FStudioColorMapping Gray{2,true,-10,30};
    TestTrue(TEXT("Grayscale lower endpoint is black"),StudioColor::Map(-10,Gray)==FLinearColor::Black);
    TestTrue(TEXT("Grayscale upper endpoint is white"),StudioColor::Map(30,Gray)==FLinearColor::White);
    TestTrue(TEXT("Grayscale physical midpoint is half intensity"),StudioColor::Map(10,Gray)==FLinearColor(.5,.5,.5));
    TestTrue(TEXT("Spectrum reaches exact final stop"),StudioColor::Map(1,{0,false,0,1}).Equals(FLinearColor(.8,.015,.008),1.e-6));
    TestEqual(TEXT("Display range does not edit scientific metadata"),Pressure.Minimum,-600.);
    const auto Other=StudioColor::Resolve(TEXT("sourceB"),Pressure,Styles);
    TestFalse(TEXT("Unrelated source keeps automatic range"),Other.bManualRange);
    TestEqual(TEXT("Unrelated source retains original minimum"),Other.Minimum,-600.);
    TestEqual(TEXT("Unrelated source keeps default palette"),Other.Palette,0);
    Pressure.Minimum=Pressure.Maximum=3;Styles[0].bManualRange=false;
    const auto Constant=StudioColor::Resolve(TEXT("sourceA"),Pressure,Styles);
    TestTrue(TEXT("Constant source displays neutral midpoint without division by zero"),StudioColor::Map(3,Constant).Equals(FLinearColor(.94,.94,.94),1.e-6));
    Styles[0].Minimum=Styles[0].Maximum=3;
    TestTrue(TEXT("Constant automatic source range is valid"),StudioColor::IsValid(Styles));
    Styles[0].bManualRange=true;TestFalse(TEXT("Equal manual range is invalid"),StudioColor::IsValid(Styles));

    const TPair<FString,double> Valid[]={{TEXT(" -6.164060669e2 "),-616.4060669},{TEXT("+1."),1.},{TEXT(".5"),.5},
        {TEXT("1.2345678901234567e-12"),1.2345678901234567e-12},{TEXT("-0"),0.}};
    for(const auto& Entry:Valid)
    {double Out=77;TestTrue(*Entry.Key,StudioColor::ParseNumber(Entry.Key,Out));TestEqual(TEXT("Full decimal precision is retained"),Out,Entry.Value);}
    for(const TCHAR* Bad:{TEXT(""),TEXT("+"),TEXT("."),TEXT("nan"),TEXT("inf"),TEXT("1e309"),TEXT("1e-999"),TEXT("1e"),TEXT("1e+"),TEXT("0x10"),
        TEXT("1,000"),TEXT("1.2.3"),TEXT("1 2"),TEXT("3 Pa")})
    {double Out=77;TestFalse(*FString::Printf(TEXT("Reject '%s'"),Bad),StudioColor::ParseNumber(Bad,Out));TestEqual(TEXT("Rejected input preserves output"),Out,77.);}
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioColorSchemaTest,"Studio.Colors.SchemaAndMigration",ColorFlags)
bool FStudioColorSchemaTest::RunTest(const FString&)
{
    FStudioProject P,Out;FString Error;
    P.View.ScalarStyles={{P.Dataset,TEXT("velocity_magnitude"),2,true,1.2345678901234567e-12,350.12345678901234},
        {TEXT("another-source"),TEXT("pressure"),1,true,-100,100}};
    if(!TestTrue(TEXT("Current color document parses"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Out,Error)))return false;
    TestTrue(TEXT("All field styles round-trip exactly"),Out.View.ScalarStyles==P.View.ScalarStyles);
    auto Invalid=P;Invalid.View.ScalarStyles.Add(P.View.ScalarStyles[0]);
    const FString Before=StudioProjectIO::Serialize(Out);
    TestFalse(TEXT("Duplicate dataset/field settings rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Invalid),Out,Error));
    TestEqual(TEXT("Rejected document retains destination"),StudioProjectIO::Serialize(Out),Before);
    Invalid=P;Invalid.View.ScalarStyles[0].Maximum=Invalid.View.ScalarStyles[0].Minimum;
    TestFalse(TEXT("Equal custom range rejected in a saved document"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Invalid),Out,Error));
    Invalid=P;Invalid.View.ScalarStyles[0].Palette=4;
    TestFalse(TEXT("Unknown palette rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Invalid),Out,Error));
    Invalid=P;Invalid.View.ScalarStyles[0].Field=TEXT("bad\nfield");
    TestFalse(TEXT("Invalid field identity rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Invalid),Out,Error));
    Invalid=P;Invalid.View.ScalarStyles[0].Minimum=-std::numeric_limits<double>::max();Invalid.View.ScalarStyles[0].Maximum=std::numeric_limits<double>::max();
    TestFalse(TEXT("Overflowing color span rejected"),StudioColor::IsValid(Invalid.View.ScalarStyles));
    Invalid=P;Invalid.View.ScalarStyles[0].Minimum=std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Nonfinite values rejected before serialization"),StudioColor::IsValid(Invalid.View.ScalarStyles));
    TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),O);
    O->GetObjectField(TEXT("view"))->RemoveField(TEXT("scalarStyles"));
    TestFalse(TEXT("V8 requires its color state"),StudioProjectIO::Parse(ColorJSON(O),Out,Error));
    O->SetNumberField(TEXT("version"),7);
    TestTrue(TEXT("V7 migrates without color settings"),StudioProjectIO::Parse(ColorJSON(O),Out,Error));
    TestTrue(TEXT("Migrated projects retain source palette/ranges by default"),Out.View.ScalarStyles.IsEmpty());
    auto Expected=P;Expected.View.ScalarStyles.Reset();Expected.View.StreamlineSettings.Method=EStudioStreamMethod::Midpoint;
    TestEqual(TEXT("Migration preserves the rest of the complete document"),StudioProjectIO::Serialize(Out),StudioProjectIO::Serialize(Expected));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCustomColors,"Studio.Colors.CustomTransferFunction",ColorFlags)
bool FStudioCustomColors::RunTest(const FString&)
{
    FLinearColor Parsed;
    TestTrue(TEXT("Accept explicit sRGB hex color"),StudioColor::ParseHexColor(TEXT("#804020"),Parsed));
    TestTrue(TEXT("Hex color converts to linear RGB"),Parsed==FLinearColor::FromSRGBColor(FColor(128,64,32)));
    for(const TCHAR* Bad:{TEXT("#12345"),TEXT("#1234567"),TEXT("#12GG56"),TEXT("#123456FF")})
        TestFalse(TEXT("Reject invalid color without guessing"),StudioColor::ParseHexColor(Bad,Parsed));
    const FStudioColorMapping Mapping{3,true,-10,30,FLinearColor::Red,FLinearColor::Green,FLinearColor::Blue};
    TestTrue(TEXT("Physical midpoint uses custom middle color"),StudioColor::Map(10,Mapping)==FLinearColor::Green);
    TestTrue(TEXT("Interpolate field before mapping linear colors"),StudioColor::Map(0,Mapping)==FLinearColor(.5,.5,0));
    FColorFiles Files;FStudioModel M(Files.Root/TEXT("session"));
    const auto Before=M.InspectionState();
    TestTrue(TEXT("Apply colors as one view edit"),M.SetScalarStyle(3,true,-10,30,{Mapping.LowColor,Mapping.MiddleColor,Mapping.HighColor}));
    const auto Styled=M.InspectionState();
    TestTrue(TEXT("Custom colors undo together"),M.UndoView()&&M.InspectionState().Equals(Before));
    TestTrue(TEXT("Custom colors redo exactly"),M.RedoView()&&M.InspectionState().Equals(Styled));
    FStudioProject Restored;FString Error;
    TestTrue(TEXT("Custom transfer function persists"),StudioProjectIO::Parse(StudioProjectIO::Serialize(M.SnapshotProject()),Restored,Error));
    TestTrue(TEXT("Persisted custom colors are exact"),Restored.View.ScalarStyles==M.ScalarStyles);
    TestTrue(TEXT("Range edits retain custom colors"),M.SetScalarStyle(3,true,-20,60));
    TestTrue(TEXT("Custom midpoint still green after range edit"),StudioColor::Map(20,M.ActiveColorMapping())==FLinearColor::Green);
    TestFalse(TEXT("Invalid RGB components rejected"),M.SetScalarStyle(3,true,-20,60,{FLinearColor(2,0,0),FLinearColor::Green,FLinearColor::Blue}));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioColorModelTest,"Studio.Colors.HistoryPersistenceAndSourceIsolation",ColorFlags)
bool FStudioColorModelTest::RunTest(const FString&)
{
    FColorFiles Files;FStudioModel M(Files.Root/TEXT("session"));
    const auto Camera=M.Project.Camera;const FString Case=StudioCaseIO::Serialize(M.Project.Draft);
    M.Run();M.Tick(.1);M.Scrub(.7);M.PlaybackRate=4.;M.bLoopPlayback=true;
    const int32 Selected=M.SelectedFrame,Playing=M.PlaybackFrame,BeforeRevision=M.Revision;
    const auto Initial=M.InspectionState();
    TestTrue(TEXT("Set precise custom range"),M.SetScalarStyle(2,true,1.2345678901234567e-12,350.12345678901234));
    const auto Styled=M.InspectionState();
    TestTrue(TEXT("Color change invalidates rendered geometry"),M.Revision>BeforeRevision);
    TestEqual(TEXT("Color settings leave selected frame"),M.SelectedFrame,Selected);
    TestEqual(TEXT("Color settings leave playback cursor"),M.PlaybackFrame,Playing);
    TestTrue(TEXT("Playback and review continue"),M.State==EStudioRunState::Running&&M.bReviewing);
    TestTrue(TEXT("View camera untouched"),StudioView::CameraEquals(Camera,M.Project.Camera));
    TestEqual(TEXT("Case untouched"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    TestTrue(TEXT("One action undoes palette and range together"),M.UndoView());
    TestTrue(TEXT("Undo restores original display"),M.InspectionState().Equals(Initial));
    TestFalse(TEXT("One color change has one history entry"),M.CanUndoView());
    TestTrue(TEXT("Redo restores exact values"),M.RedoView()&&M.InspectionState().Equals(Styled));
    TestEqual(TEXT("History retains current playback speed"),M.PlaybackRate,4.);
    TestTrue(TEXT("History retains loop preference"),M.bLoopPlayback);
    const int32 Revision=M.Revision;
    TestTrue(TEXT("Repeated same setting is accepted"),M.SetScalarStyle(2,true,1.2345678901234567e-12,350.12345678901234));
    TestEqual(TEXT("Repeated same setting does not rebuild geometry"),M.Revision,Revision);
    for(const auto Range:{TPair<double,double>(4,3),TPair<double,double>(3,3),TPair<double,double>(0,std::numeric_limits<double>::infinity())})
        TestFalse(TEXT("Invalid range rejected"),M.SetScalarStyle(1,true,Range.Key,Range.Value));
    TestTrue(TEXT("All rejected edits retain exact display"),M.InspectionState().Equals(Styled));
    const FString Legacy=M.Project.Dataset;
    const auto SavedStyles=M.ScalarStyles;
    TestTrue(TEXT("Import actual unchanged point fixture"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json")));
    if(!TestTrue(TEXT("Point fixture opens"),FinishColorLoad(M)&&M.Solver->Descriptor().bSourcePoints))return false;
    TestFalse(TEXT("A different dataset keeps its own source range"),M.ActiveColorMapping().bManualRange);
    TestEqual(TEXT("Different dataset does not inherit grayscale"),M.ActiveColorMapping().Palette,0);
    M.EditView(TEXT("Pressure"),[](auto& S){S.Display.ScalarField=TEXT("pressure");});
    const auto Metadata=M.ActiveScalar();
    const auto BeforeField=M.Solver->CaptureViewField(0,TEXT("pressure"),false)->OriginalPoints();
    const double OriginalValue=(*BeforeField->FindValues(TEXT("pressure")))[17];
    TestTrue(TEXT("Pressure gets its own divergent range"),M.SetScalarStyle(1,true,-100,100));
    M.EditView(TEXT("Speed"),[](auto& S){S.Display.ScalarField=TEXT("velocity_magnitude");});
    TestFalse(TEXT("Pressure range never leaks into speed"),M.ActiveColorMapping().bManualRange);
    TestEqual(TEXT("Pressure palette never leaks into speed"),M.ActiveColorMapping().Palette,0);
    M.EditView(TEXT("Pressure"),[](auto& S){S.Display.ScalarField=TEXT("pressure");});
    TestEqual(TEXT("Returning to pressure restores chosen minimum"),M.ActiveColorMapping().Minimum,-100.);
    TestEqual(TEXT("Returning to pressure restores chosen palette"),M.ActiveColorMapping().Palette,1);
    TestEqual(TEXT("Source minimum remains authoritative"),M.ActiveScalar().Minimum,Metadata.Minimum);
    const auto AfterField=M.Solver->CaptureViewField(0,TEXT("pressure"),false)->OriginalPoints();
    TestEqual(TEXT("Original CFD pressure is not edited"),(*AfterField->FindValues(TEXT("pressure")))[17],OriginalValue);
    const FString File=Files.Root/TEXT("case.lbms");
    TestTrue(TEXT("Save per-source field styles"),M.SaveProject(File));
    const auto Expected=M.SnapshotProject();
    TestTrue(TEXT("Switch back to original source"),M.RequestRecording(Legacy));
    TestTrue(TEXT("Original source restored"),FinishColorLoad(M));
    TestEqual(TEXT("Available pressure selection survives a source change"),M.ActiveScalar().Id,FString(TEXT("pressure")));
    TestFalse(TEXT("Other source pressure range cannot leak into this pressure field"),M.ActiveColorMapping().bManualRange);
    M.EditView(TEXT("Original speed style"),[](auto& S){S.Display.ScalarField=TEXT("velocity_magnitude");});
    TestEqual(TEXT("Source-specific grayscale restored"),M.ActiveColorMapping().Palette,2);
    TestEqual(TEXT("Source-specific exact minimum restored"),M.ActiveColorMapping().Minimum,SavedStyles[0].Minimum);
    TestTrue(TEXT("Open saved point project"),M.RequestProjectOpen(File));
    TestTrue(TEXT("Saved point project opened"),FinishColorLoad(M));
    TestEqual(TEXT("Reopen restores exact complete project"),StudioProjectIO::Serialize(M.SnapshotProject()),StudioProjectIO::Serialize(Expected));
    TestTrue(TEXT("Reset source range retains palette"),M.SetScalarStyle(1,false,Metadata.Minimum,Metadata.Maximum));
    TestFalse(TEXT("Reset restores automatic source range"),M.ActiveColorMapping().bManualRange);
    TestEqual(TEXT("Reset uses original range"),M.ActiveColorMapping().Minimum,Metadata.Minimum);
    TestEqual(TEXT("Reset keeps palette"),M.ActiveColorMapping().Palette,1);
    M.ScalarStyles.Reset();
    for(int32 I=0;I<128;++I)M.ScalarStyles.Add({FString::Printf(TEXT("source-%d"),I),TEXT("pressure"),0,false,0,1});
    TestFalse(TEXT("Saved color settings are bounded"),M.SetScalarStyle(0,false,0,1));
    TestEqual(TEXT("Rejected overflow preserves all entries"),M.ScalarStyles.Num(),128);
    M.ScalarStyles.Last().Dataset=M.Project.Dataset;M.ScalarStyles.Last().Field=M.ActiveScalar().Id;
    TestTrue(TEXT("Existing color setting remains editable at the limit"),M.SetScalarStyle(2,true,-1,1));
    TestEqual(TEXT("Editing an existing setting does not allocate another entry"),M.ScalarStyles.Num(),128);
    return true;
}
#endif
