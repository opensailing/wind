#include "StudioHome4RequestActions.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StudioHome4Recipes.h"
#include "StudioHome4Session.h"
#include "SStudioHome4Panel.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ActionsNative,"Studio.HeadlessUI.Home4.Authoring.SpecImportExportSmokeFigureAndSafeguards",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ActionsNative::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-request-actions")/FGuid::NewGuid().ToString(),Path=Root/TEXT("run_spec.json");auto Model=MakeShared<FStudioModel>(Root);auto Session=MakeShared<FStudioHome4Session>(Model);Session->ApplyRecipe(TEXT("th01-hull"));
    {auto Widget=SNew(SStudioHome4Panel).Model(Model).Session(Session).Page(TEXT("Run")).ImportPath(Path).ExportPath(Path);FStudioHeadlessSlate UI(*this,Widget,FVector2D(1040,800));UI.Press(TEXT("Home4Export"));FStudioHome4Spec Saved;FString Error;TestTrue(*Error,StudioHome4Config::Load(Path,Saved,Error));TestEqual(TEXT("Native export writes exact applied request"),StudioHome4Config::Serialize(Saved),StudioHome4Config::Serialize(*Model->Project.Draft.Home4));
        Saved.Run.Tag=TEXT("imported-request");StudioProjectIO::WriteAtomic(Path,StudioHome4Config::Serialize(Saved),Error);UI.Press(TEXT("Home4Import"));TestEqual(TEXT("Native import commits verified request"),Model->Project.Draft.Home4->Run.Tag,FString(TEXT("imported-request")));
        const auto Keep=StudioHome4Config::Serialize(*Model->Project.Draft.Home4);FFileHelper::SaveStringToFile(TEXT("{\"version\":1,\"version\":2}"),*Path);UI.Press(TEXT("Home4Import"));TestEqual(TEXT("Malformed native import keeps prior applied request"),StudioHome4Config::Serialize(*Model->Project.Draft.Home4),Keep);
        UI.Press(TEXT("Home4SmokePreset"));FStudioHome4Spec Draft;TestTrue(*Error,Session->Build(Draft,Error));TestTrue(TEXT("Smoke action gives concrete reviewed finite duration/cadences"),Draft.Run.Smoke.Get(false)&&Draft.Run.Steps==256&&Draft.Run.MeasureEvery==64&&Draft.Run.VizEvery==64&&Draft.Run.OutDirectory.IsEmpty());UI.Press(TEXT("Home4Export"));TestTrue(TEXT("Dirty request blocks export"),Session->Status.Contains(TEXT("Apply or revert")));UI.Press(TEXT("Home4Apply"));UI.Press(TEXT("Home4FigurePreset"));TestTrue(*Error,Session->Build(Draft,Error));TestTrue(TEXT("Figure action makes explicit _viz request and clears reused destinations"),Draft.Run.Tag.EndsWith(TEXT("_viz"))&&!Draft.Run.Smoke.Get(true)&&Draft.Run.VizDirectory.IsEmpty());UI.Press(TEXT("Home4Apply"));}
    {auto Widget=SNew(SStudioHome4Panel).Model(Model).Session(Session).Page(TEXT("Fluids & Interface"));FStudioHeadlessSlate UI(*this,Widget,FVector2D(1040,800));UI.Press(TEXT("Home4SafeguardsPreset"));FStudioHome4Spec Draft;FString Error;TestTrue(*Error,Session->Build(Draft,Error));TestTrue(TEXT("Documented safeguards action retains exact values"),Draft.Fluids.GradientLimitFactor==1.6&&Draft.Fluids.ForceThresholdFactor==60&&Draft.Fluids.LightForceFactor==.6&&Draft.Fluids.LightPhaseCutoff==.1);}
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4EveryRequestProtocol,"Studio.Home4.Authoring.AllRecipeProtocolsRetainEveryTypedInput",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4EveryRequestProtocol::RunTest(const FString&)
{
    for(const auto& R:StudioHome4Recipes::All()){auto S=R.Template;S.Run.BlockShape=FIntVector(16,8,4);S.Authoring.DeviceProfile=TEXT("M4-Pro-MPS-79.5M");S.Geometry.InertiaFrame=TEXT("source-xyz");const auto Text=StudioHome4RequestActions::CompleteProtocol(S);TSharedPtr<FJsonObject> O;TestTrue(TEXT("Every recipe exposes complete parseable frontend request"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O));if(!O)continue;const TSharedPtr<FJsonObject>* Spec=nullptr;FStudioHome4Spec Parsed;FString E;TestTrue(*E,O->TryGetObjectField(TEXT("run_spec"),Spec)&&StudioHome4Config::FromJSON(*Spec,Parsed,E));TestEqual(TEXT("Complete typed protocol retains exact original recipe and all edited flags"),StudioHome4Config::Serialize(Parsed),StudioHome4Config::Serialize(S));TestEqual(TEXT("Protocol identifies actual driver without inventing argument encoding"),O->GetStringField(TEXT("driver")),R.Driver);}
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-device-inputs")/FGuid::NewGuid().ToString());auto Draft=MakeShared<FStudioHome4Session>(M);Draft->ApplyRecipe(TEXT("th01-hull"));auto Panel=SNew(SStudioHome4Panel).Model(M).Session(Draft).Page(TEXT("Run"));FStudioHeadlessSlate UI(*this,Panel,FVector2D(1040,800));UI.Type(TEXT("run.blockShape"),TEXT("16,8,4"));UI.Press(TEXT("authoring.deviceProfile"));UI.Press(TEXT("authoring.deviceProfile.M4-Pro-MPS-79.5M"));UI.Press(TEXT("Home4Apply"));TestTrue(TEXT("Native device/block controls persist actual request"),M->Project.Draft.Home4->Run.BlockShape==FIntVector(16,8,4)&&M->Project.Draft.Home4->Authoring.DeviceProfile==TEXT("M4-Pro-MPS-79.5M"));return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4EveryAppendixNative,"Studio.HeadlessUI.Home4.Authoring.EveryAppendixControlAndCompleteRequest",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4EveryAppendixNative::RunTest(const FString&)
{
    struct FInput{const TCHAR* Key;const TCHAR* Value;EJson Type;const TCHAR* Flag;};
    const FInput Inputs[]={
        {TEXT("reference.lengthCells"),TEXT("384"),EJson::Number,TEXT("--L")},
        {TEXT("geometry.patchClassification"),TEXT("HKr"),EJson::String,TEXT("--config")},
        {TEXT("reference.froude"),TEXT("0.63"),EJson::Number,TEXT("--Fn")},
        {TEXT("reference.reynolds"),TEXT("321"),EJson::Number,TEXT("--Re_ref")},
        {TEXT("fluids.gravity"),TEXT("0.00013"),EJson::Number,TEXT("--g")},
        {TEXT("fluids.xi"),TEXT("6"),EJson::Number,TEXT("--xi")},
        {TEXT("reference.bond"),TEXT("250"),EJson::Number,TEXT("--Bo")},
        {TEXT("fluids.mobility"),TEXT("0.025"),EJson::Number,TEXT("--mobility")},
        {TEXT("fluids.rhoLight"),TEXT("0.02"),EJson::Number,TEXT("--rho_L")},
        {TEXT("geometry.sinkCells"),TEXT("-0.35"),EJson::Number,TEXT("--sink_cells")},
        {TEXT("geometry.trimDegrees"),TEXT("4.25"),EJson::Number,TEXT("--trim")},
        {TEXT("geometry.noEquilibrate"),TEXT("true"),EJson::Boolean,TEXT("--no_equilibrate")},
        {TEXT("lattice.padUp"),TEXT("2.1"),EJson::Number,TEXT("--pad_up")},
        {TEXT("lattice.padDown"),TEXT("4.2"),EJson::Number,TEXT("--pad_down")},
        {TEXT("lattice.padSide"),TEXT("1.3"),EJson::Number,TEXT("--pad_side")},
        {TEXT("lattice.depth"),TEXT("2.4"),EJson::Number,TEXT("--depth")},
        {TEXT("lattice.air"),TEXT("0.8"),EJson::Number,TEXT("--air")},
        {TEXT("zones.sponge"),TEXT("80"),EJson::Number,TEXT("--sponge")},
        {TEXT("zones.xBeach"),TEXT("180"),EJson::Number,TEXT("--xbeach")},
        {TEXT("zones.xBeachStrength"),TEXT("0.12"),EJson::Number,TEXT("--xbeach_strength")},
        {TEXT("zones.beachY"),TEXT("16"),EJson::Number,TEXT("--beach_y")},
        {TEXT("zones.beachGap"),TEXT("24"),EJson::Number,TEXT("--beach_gap")},
        {TEXT("zones.zoneStrength"),TEXT("0.15"),EJson::Number,TEXT("--zone_strength")},
        {TEXT("zones.floorFriction"),TEXT("0.008"),EJson::Number,TEXT("--floor_fric")},
        {TEXT("zones.massCorrection"),TEXT("global"),EJson::String,TEXT("--mass_correct")},
        {TEXT("zones.pierceBoundary"),TEXT("declared-driver-mode"),EJson::String,nullptr},
        {TEXT("run.travel"),TEXT("12"),EJson::Number,TEXT("--travel")},
        {TEXT("run.rampLength"),TEXT("3"),EJson::Number,TEXT("--ramp_L")},
        {TEXT("run.noFrameAcceleration"),TEXT("true"),EJson::Boolean,TEXT("--no_frame_accel")},
        {TEXT("run.steps"),TEXT("9876"),EJson::Number,TEXT("--steps")},
        {TEXT("run.averageLength"),TEXT("6"),EJson::Number,TEXT("--avg_L")},
        {TEXT("run.measureEvery"),TEXT("17"),EJson::Number,TEXT("--measure_every")},
        {TEXT("run.printEvery"),TEXT("23"),EJson::Number,TEXT("--print_every")},
        {TEXT("geometry.bandCells"),TEXT("9"),EJson::Number,TEXT("--band_cells")},
        {TEXT("geometry.refine"),TEXT("2"),EJson::Number,TEXT("--refine")},
        {TEXT("geometry.sdfBackend"),TEXT("CPT"),EJson::String,TEXT("--sdf_backend")},
        {TEXT("geometry.cptPath"),TEXT("/configuration-only/CPT path"),EJson::String,TEXT("--cpt_path")},
        {TEXT("run.bodyOnCpu"),TEXT("true"),EJson::Boolean,TEXT("--body_on_cpu")},
        {TEXT("run.noGpuKernels"),TEXT("true"),EJson::Boolean,TEXT("--no_gpu_kernels")},
        {TEXT("run.device"),TEXT("declared-device-0"),EJson::String,TEXT("--device")},
        {TEXT("run.initState"),TEXT("/configuration-only/original-state.npz"),EJson::String,TEXT("--init_state")},
        {TEXT("run.saveState"),TEXT("/configuration-only/requested-state.npz"),EJson::String,nullptr},
        {TEXT("run.tag"),TEXT("all-controls_viz"),EJson::String,TEXT("--tag")},
        {TEXT("run.outDirectory"),TEXT("/configuration-only/output with spaces"),EJson::String,TEXT("--outdir")},
        {TEXT("run.smoke"),TEXT("true"),EJson::Boolean,TEXT("--smoke")}
    };
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-appendix-native")/FGuid::NewGuid().ToString());M->BeginHome4Authoring();
    auto Session=MakeShared<FStudioHome4Session>(M);Session->ApplyRecipe(TEXT("th01-hull"));int32 Edited=0;
    for(const TCHAR* Page:{TEXT("Geometry"),TEXT("Fluids & Interface"),TEXT("Bodies"),TEXT("Lattice"),TEXT("Boundaries & Zones"),TEXT("Run")})
    {
        auto Panel=SNew(SStudioHome4Panel).Model(M).Session(Session).Page(Page);FStudioHeadlessSlate UI(*this,Panel,FVector2D(1040,800));
        for(const auto& Input:Inputs)
        {
            const FString Key(Input.Key);const auto* Field=StudioHome4Config::Fields().FindByPredicate([&](const auto& F){return FStudioHome4Session::Key(F)==Key;});
            if(!TestNotNull(*Key,Field))return false;
            const FString Owner=Field->Page;
            if(Owner!=Page)continue;
            bool Focused=false;for(int32 Attempt=0;Attempt<8&&!Focused;++Attempt){UI.Layout();Focused=Panel->FocusField(Key);}
            if(!TestTrue(TEXT("Appendix field scrolls into the actual native editor: ")+Key,Focused))return false;
            if(Input.Type==EJson::Boolean||!Field->Choices.IsEmpty())
            {if(!UI.Press(FName(*Key))||!UI.Press(FName(*(Key+TEXT(".")+Input.Value))))return false;}
            else if(!UI.Type(FName(*Key),Input.Value))return false;
            ++Edited;
        }
    }
    TestEqual(TEXT("Every 45 Appendix request controls were edited through real Slate events"),Edited,45);
    if(!TestTrue(*Session->Status,Session->Apply()))return false;
    const auto Spec=StudioHome4Config::ToJSON(*M->Project.Draft.Home4);
    FStudioHome4DriverCommand Command;FString Error;TestTrue(*Error,StudioHome4Config::BuildHullDriverArgv(*M->Project.Draft.Home4,TEXT("python3"),TEXT("run_hull_speed.py"),Command,Error));
    for(const auto& Input:Inputs)
    {
        FString Section,Key;FString(Input.Key).Split(TEXT("."),&Section,&Key);const auto Value=Spec->GetObjectField(Section)->TryGetField(Key);
        if(!TestTrue(FString(TEXT("Native input retains its typed request: "))+Input.Key,Value&&Value->Type==Input.Type))continue;
        if(Input.Type==EJson::Number)TestEqual(FString(TEXT("Exact Appendix numeric request: "))+Input.Key,Value->AsNumber(),FCString::Atod(Input.Value));
        else if(Input.Type==EJson::Boolean)TestTrue(FString(TEXT("Explicit Appendix switch retained: "))+Input.Key,Value->AsBool());
        else TestEqual(FString(TEXT("Exact Appendix literal request: "))+Input.Key,Value->AsString(),FString(Input.Value));
        if(Input.Flag)TestTrue(TEXT("Known Appendix flag is one independent argument: ")+FString(Input.Flag),Command.Argv.Contains(Input.Flag));
    }
    TestFalse(TEXT("Unknown pierce argument arity is never invented"),Command.Argv.Contains(TEXT("--pierce_bc")));TestFalse(TEXT("Unknown checkpoint argument arity is never invented"),Command.Argv.Contains(TEXT("--save_state")));
    TestTrue(TEXT("Both unknown driver contracts remain explicit"),Command.MissingContracts.ContainsByPredicate([](const auto& V){return V.Contains(TEXT("zones.pierceBoundary"));})&&Command.MissingContracts.ContainsByPredicate([](const auto& V){return V.Contains(TEXT("run.saveState"));}));
    FStudioHome4Spec Reopened;TestTrue(*Error,StudioHome4Config::Parse(StudioHome4Config::Serialize(*M->Project.Draft.Home4),Reopened,Error));TestEqual(TEXT("Native all-option request round trips exactly"),StudioHome4Config::Serialize(Reopened),StudioHome4Config::Serialize(*M->Project.Draft.Home4));return !HasAnyErrors();
}

#endif
