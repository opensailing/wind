#include "StudioFieldDisplay.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFieldDisplaySemantics,"Studio.FieldDisplay.SelectedScalarAndIndependentVectors",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioFieldDisplaySemantics::RunTest(const FString&)
{
    FRecordedSolver Source;const auto Field=Source.CaptureField(0);
    TestEqual(TEXT("Legacy UI advertises all recorded scalar quantities"),Source.Descriptor().Scalars.Num(),5);
    // Original SU2 node, independently decoded in the published fixture audit.
    const FVector Node(.03250676393508911,0,.1194048523902893);
    const FStudioColorMapping PressureGray{2,true,98600,98800};
    FLinearColor Color;
    TestTrue(TEXT("Selected source pressure colors a flow sample"),StudioFieldDisplay::SampleColor(*Field,Node,TEXT("pressure"),PressureGray,Color));
    const float Gray=float((98699.78125-98600.)/200.);
    TestTrue(TEXT("Pressure color matches independently decoded source value"),Color.Equals(FLinearColor(Gray,Gray,Gray),1.e-7));
    FVector Velocity;TestTrue(TEXT("Original velocity available"),Field->SampleVelocity(Node,Velocity));
    TestFalse(TEXT("Pressure legend cannot color with velocity magnitude"),Color.Equals(StudioColor::Map(Velocity.Size(),PressureGray),1.e-5));

    const TArray<FVector> Positions{Node,FVector(.2,0,.15),FVector(.7,0,-.15)};
    TArray<FStudioVectorGlyph> Pressure,Speed,Density;
    TestTrue(TEXT("Build pressure-colored vectors"),StudioFieldDisplay::VectorGlyphs(*Field,Positions,TEXT("pressure"),PressureGray,.5,1.,Pressure));
    TestTrue(TEXT("Build speed-colored vectors"),StudioFieldDisplay::VectorGlyphs(*Field,Positions,TEXT("velocity_magnitude"),{0,false,0,400},.5,1.,Speed));
    TestTrue(TEXT("Build density-colored vectors"),StudioFieldDisplay::VectorGlyphs(*Field,Positions,TEXT("density"),{1,true,1.,1.4},.5,1.,Density));
    if(!TestTrue(TEXT("Original samples produced vectors"),!Pressure.IsEmpty())||
        !TestEqual(TEXT("Scalar changes retain glyph count"),Pressure.Num(),Speed.Num())||
        !TestEqual(TEXT("Different scalar units retain glyph count"),Pressure.Num(),Density.Num()))return false;
    for(int32 I=0;I<Pressure.Num();++I)
    {
        TestEqual(TEXT("Scalar choice retains vector origin"),Pressure[I].PositionMeters,Speed[I].PositionMeters);
        TestEqual(TEXT("Scalar choice retains vector direction"),Pressure[I].Direction,Speed[I].Direction);
        TestEqual(TEXT("Scalar range/units cannot resize vectors"),Pressure[I].TipMeters,Speed[I].TipMeters);
        TestEqual(TEXT("Density range also leaves lengths unchanged"),Pressure[I].TipMeters,Density[I].TipMeters);
        double Value;TestTrue(TEXT("Original pressure at glyph available"),Field->SampleScalar(Pressure[I].PositionMeters,TEXT("pressure"),Value));
        TestTrue(TEXT("Every glyph color uses selected scalar"),Pressure[I].Color.Equals(StudioColor::Map(Value,PressureGray),1.e-7));
    }
    TArray<FStudioVectorGlyph> Missing;
    TestTrue(TEXT("Unavailable scalar is a valid empty display"),StudioFieldDisplay::VectorGlyphs(*Field,Positions,TEXT("absent"),PressureGray,.5,1.,Missing));
    TestTrue(TEXT("No glyphs impersonate absent scalar values"),Missing.IsEmpty());
    Color=FLinearColor::White;
    TestFalse(TEXT("Missing streamline scalar rejected"),StudioFieldDisplay::SampleColor(*Field,Node,TEXT("absent"),PressureGray,Color));
    TestEqual(TEXT("Missing color is explicitly transparent"),Color,FLinearColor::Transparent);
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    const auto Retained=Pressure[0].TipMeters;
    TestFalse(TEXT("Cancelled glyph request rejected"),StudioFieldDisplay::VectorGlyphs(*Field,Positions,TEXT("pressure"),PressureGray,.5,1.,Pressure,Cancel));
    TestEqual(TEXT("Cancelled work cannot replace published glyphs"),Pressure[0].TipMeters,Retained);
    TestFalse(TEXT("Nonfinite display scale rejected"),StudioFieldDisplay::VectorGlyphs(*Field,Positions,TEXT("pressure"),PressureGray,.5,std::numeric_limits<double>::quiet_NaN(),Pressure));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLegacyScalarMetadata,"Studio.FieldDisplay.LegacyScalarMetadata",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioLegacyScalarMetadata::RunTest(const FString&)
{
    const FString Original=FPaths::ProjectContentDir()/TEXT("Samples/MeshGraphNets_Airfoil");
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/LegacyScalarMetadata")/FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*Root,true);
    FString Text;TSharedPtr<FJsonObject> Metadata;
    if(!TestTrue(TEXT("Read original metadata"),FFileHelper::LoadFileToString(Text,*(Original/TEXT("recording.json")))))return false;
    if(!TestTrue(TEXT("Decode original metadata"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Metadata)))return false;
    TestEqual(TEXT("Copy unchanged original payload"),IFileManager::Get().Copy(*(Root/TEXT("flow.bin")),*(Original/TEXT("flow.bin"))),COPY_OK);
    const auto Save=[&]
    {FString JSON;FJsonSerializer::Serialize(Metadata.ToSharedRef(),TJsonWriterFactory<>::Create(&JSON));return FFileHelper::SaveStringToFile(JSON,*(Root/TEXT("recording.json")));};
    const auto& Fields=Metadata->GetArrayField(TEXT("scalars"));
    if(!TestEqual(TEXT("Five supplied scalar descriptors"),Fields.Num(),5))return false;
    const auto Pressure=Fields[3]->AsObject();
    Pressure->SetStringField(TEXT("unit"),TEXT("m/s"));TestTrue(TEXT("Write incorrect pressure unit"),Save());
    FRecordedSolver WrongUnit(Root/TEXT("flow.bin"));
    TestFalse(TEXT("Wrong physical units reject source publication"),WrongUnit.LoadError().IsEmpty());
    Pressure->SetStringField(TEXT("unit"),TEXT("Pa"));
    Pressure->SetArrayField(TEXT("range"),{MakeShared<FJsonValueNumber>(0.),MakeShared<FJsonValueNumber>(1.)});
    TestTrue(TEXT("Write misleading scalar extent"),Save());FRecordedSolver WrongRange(Root/TEXT("flow.bin"));
    TestFalse(TEXT("Original values outside declared range are refused"),WrongRange.CaptureField(0)->IsValid());
    Metadata->RemoveField(TEXT("scalars"));TestTrue(TEXT("Write historical metadata without optional scalar list"),Save());
    FRecordedSolver Legacy(Root/TEXT("flow.bin"));
    TestTrue(TEXT("Historical version-one source remains readable"),Legacy.CaptureField(0)->IsValid());
    TestEqual(TEXT("Unspecified legacy ranges do not invent additional fields"),Legacy.Descriptor().Scalars.Num(),1);
    IFileManager::Get().DeleteDirectory(*Root,false,true);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVectorSampling,"Studio.FieldDisplay.BoundedDeterministicVectorSamples",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioVectorSampling::RunTest(const FString&)
{
    const FBox Bounds(FVector(-2,-1,-3),FVector(8,1,2));
    for(int32 Count:{1,17,384,4096})
    {
        const auto Rows=StudioFieldDisplay::VectorRows(18706,Count);
        TestEqual(TEXT("Requested original sample count"),Rows.Num(),Count);
        TestTrue(TEXT("Original sampling is repeatable"),Rows==StudioFieldDisplay::VectorRows(18706,Count));
        for(int32 I=0;I<Rows.Num();++I)
            TestTrue(TEXT("Each row is distinct, ordered and inside source"),Rows[I]>=0&&Rows[I]<18706&&(I==0||Rows[I]>Rows[I-1]));
        const auto Grid=StudioFieldDisplay::VectorGrid(Bounds,Count);
        TestEqual(TEXT("Requested grid count without rounding up"),Grid.Num(),Count);
        TestTrue(TEXT("Grid is repeatable"),Grid==StudioFieldDisplay::VectorGrid(Bounds,Count));
        TSet<FVector> Unique;
        for(const auto& P:Grid){TestTrue(TEXT("Every grid point stays inside display bounds"),Bounds.IsInside(P));Unique.Add(P);}
        TestEqual(TEXT("Grid never duplicates locations"),Unique.Num(),Count);
    }
    TestEqual(TEXT("Small sources use each original row once"),StudioFieldDisplay::VectorRows(3,4096).Num(),3);
    TestTrue(TEXT("Invalid counts cannot allocate unbounded samples"),StudioFieldDisplay::VectorRows(MAX_int32,4097).IsEmpty()&&
        StudioFieldDisplay::VectorGrid(Bounds,MAX_int32).IsEmpty()&&StudioFieldDisplay::VectorGrid(Bounds,0).IsEmpty());
    TestTrue(TEXT("Empty source produces no rows"),StudioFieldDisplay::VectorRows(0,384).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVectorLengths,"Studio.FieldDisplay.PhysicalVectorLengthSemantics",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioVectorLengths::RunTest(const FString&)
{
    // Analytic glyph inputs check the numerical contract; these are not a CFD recording.
    TArray<FStudioVectorSample> Samples{
        {FVector(1,2,3),FVector(3,4,0),FLinearColor::Red,true},
        {FVector(-1,-2,-3),FVector(0,0,-10),FLinearColor::Blue,true},
        {FVector::ZeroVector,FVector::ZeroVector,FLinearColor::White,true}};
    TArray<FStudioVectorGlyph> Glyphs;FStudioVectorSummary Summary;
    if(!TestTrue(TEXT("Proportional arrows built"),StudioFieldDisplay::VectorGlyphs(Samples,.2,false,Glyphs,Summary))||
        !TestEqual(TEXT("Zero velocity has no direction arrow"),Glyphs.Num(),2))return false;
    TestTrue(TEXT("Half speed has exactly half the reference length"),(Glyphs[0].TipMeters-Glyphs[0].PositionMeters).Equals(FVector(.06,.08,0),1.e-12));
    TestTrue(TEXT("Full speed has reference length and signed direction"),(Glyphs[1].TipMeters-Glyphs[1].PositionMeters).Equals(FVector(0,0,-.2),1.e-12));
    TestEqual(TEXT("Legend uses actual sampled maximum speed"),Summary.MaximumSpeed,10.);
    TestEqual(TEXT("Summary includes stationary sample"),Summary.SampleCount,3);
    TestEqual(TEXT("Legend describes generated arrows"),Summary.GlyphCount,2);
    TestTrue(TEXT("Uniform arrows built"),StudioFieldDisplay::VectorGlyphs(Samples,.2,true,Glyphs,Summary));
    for(const auto& Glyph:Glyphs)TestTrue(TEXT("Uniform arrow length is exact"),FMath::IsNearlyEqual((Glyph.TipMeters-Glyph.PositionMeters).Size(),.2,1.e-12));
    TestTrue(TEXT("Legend identifies direction-only mode"),Summary.bUniformLength);
    Samples[1].bHasColor=false;
    TestTrue(TEXT("Missing selected scalar handled"),StudioFieldDisplay::VectorGlyphs(Samples,.2,false,Glyphs,Summary));
    TestEqual(TEXT("Missing color omits that arrow"),Glyphs.Num(),1);
    TestEqual(TEXT("Color availability cannot renormalize speed"),Summary.MaximumSpeed,10.);
    TestTrue(TEXT("Remaining arrow retains physical proportion"),FMath::IsNearlyEqual((Glyphs[0].TipMeters-Glyphs[0].PositionMeters).Size(),.1,1.e-12));
    const auto Kept=Glyphs;const auto KeptSummary=Summary;
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Cancelled generation is not published"),StudioFieldDisplay::VectorGlyphs(Samples,.5,true,Glyphs,Summary,Cancel));
    TestTrue(TEXT("Cancellation keeps geometry and legend together"),Glyphs.Num()==Kept.Num()&&Glyphs[0].TipMeters==Kept[0].TipMeters&&
        Summary.ReferenceLengthMeters==KeptSummary.ReferenceLengthMeters&&Summary.bUniformLength==KeptSummary.bUniformLength);
    TestFalse(TEXT("Nonfinite length rejected"),StudioFieldDisplay::VectorGlyphs(Samples,std::numeric_limits<double>::infinity(),false,Glyphs,Summary));
    return true;
}
#endif
