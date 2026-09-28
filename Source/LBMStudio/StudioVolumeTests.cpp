#include "StudioVolume.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto VolumeFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
// Structural mathematics fixture only. Never installed or shown as CFD output.
TSharedRef<FStudioVolumeReconstruction,ESPMode::ThreadSafe> NumericalGrid()
{
    auto V=MakeShared<FStudioVolumeReconstruction,ESPMode::ThreadSafe>();
    auto G=MakeShared<FStudioPointGeometry,ESPMode::ThreadSafe>();
    V->Dimensions=FIntVector(3);V->SourceBounds=FBox(FVector(2,2,0),FVector(4,4,2));
    V->CylinderRadius=.5;V->SourceMetadataSHA256=FString::ChrN(64,'a');
    for(int32 I=0;I<27;++I){G->Positions.Add(V->Position(I));G->PointIds.Add(I);}
    V->Geometry=G;V->Stencils.SetNum(27);V->Classification.Init(1,27);
    for(int32 I=0;I<27;++I)
    {for(int32 J=0;J<4;++J)V->Stencils[I].Rows[J]=I;V->Stencils[I].Weights[0]=1;}
    return V;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVolumeSampling,"Studio.Volume.SamplingAndMissingData",VolumeFlags)
bool FStudioVolumeSampling::RunTest(const FString&)
{
    const auto V=NumericalGrid();TArray<double> Values;
    for(const auto& P:V->Geometry->Positions)Values.Add(2*P.X-3*P.Y+7*P.Z+11);
    for(int32 I=0;I<30;++I)
    {
        const FVector P(2.+(I%7)/3.1,2.+(I%5)/2.1,(I%9)/4.1);double Sample;
        if(!TestTrue(TEXT("Supported grid query succeeds"),V->Sample(P,Values,Sample)))return false;
        TestTrue(TEXT("Independent affine field is retained"),FMath::Abs(Sample-(2*P.X-3*P.Y+7*P.Z+11))<1.e-12);
    }
    double Sample=0;
    TestFalse(TEXT("Outside query has no invented value"),V->Sample(FVector(1,3,1),Values,Sample));
    TestTrue(TEXT("Outside value is NaN"),FMath::IsNaN(Sample));
    V->Classification[0]=0;
    TestFalse(TEXT("No interpolation across unsupported corner"),V->Sample(FVector(2.5,2.5,.5),Values,Sample));
    V->Classification[0]=2;
    TestFalse(TEXT("No interpolation across solid corner"),V->Sample(FVector(2.5,2.5,.5),Values,Sample));
    V->Classification[0]=1;Values[0]=std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Nonfinite source field stays missing"),V->Sample(FVector(2.5,2.5,.5),Values,Sample));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVolumeRays,"Studio.Volume.RayBoxClipping",VolumeFlags)
bool FStudioVolumeRays::RunTest(const FString&)
{
    const FBox B(FVector(0),FVector(1));double Entry,Exit;
    TestTrue(TEXT("Outside parallel-axis ray intersects"),StudioVolumes::Intersect(FVector(-2,.5,.5),FVector(1,0,0),B,100,Entry,Exit));
    TestEqual(TEXT("Exact entry"),Entry,2.);TestEqual(TEXT("Exact exit"),Exit,3.);
    TestTrue(TEXT("Camera inside starts at camera plane"),StudioVolumes::Intersect(FVector(.5),FVector(0,1,0),B,100,Entry,Exit));
    TestEqual(TEXT("Inside entry"),Entry,0.);TestEqual(TEXT("Inside exit"),Exit,.5);
    TestFalse(TEXT("Parallel ray outside slab rejected"),StudioVolumes::Intersect(FVector(-2,2,.5),FVector(1,0,0),B,100,Entry,Exit));
    TestFalse(TEXT("Opaque geometry before volume hides it"),StudioVolumes::Intersect(FVector(-2,.5,.5),FVector(1,0,0),B,1,Entry,Exit));
    TestTrue(TEXT("Opaque geometry inside volume clips it"),StudioVolumes::Intersect(FVector(-2,.5,.5),FVector(1,0,0),B,2.25,Entry,Exit));
    TestEqual(TEXT("Opaque depth is endpoint"),Exit,2.25);
    TestFalse(TEXT("Ray directed away rejected"),StudioVolumes::Intersect(FVector(-2,.5,.5),FVector(-1,0,0),B,100,Entry,Exit));
    TestFalse(TEXT("NaN ray rejected"),StudioVolumes::Intersect(FVector(std::numeric_limits<double>::quiet_NaN()),FVector(1,0,0),B,100,Entry,Exit));
    TestFalse(TEXT("Empty clipped region rejected"),StudioVolumes::Intersect(FVector(0),FVector(1,0,0),FBox(FVector(1),FVector(0)),100,Entry,Exit));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVolumeView,"Studio.Volume.ViewPersistenceAndUndo",VolumeFlags)
bool FStudioVolumeView::RunTest(const FString&)
{
    FStudioProject P;P.View.VolumeOpacity=.7;P.View.VolumeClipMinimum=FVector(.1,.2,.3);
    P.View.VolumeClipMaximum=FVector(.7,.8,.9);P.View.VolumeOpacityCurve=FVector(.2,.8,.1);
    P.View.VolumeStepVoxels=.5;P.View.bVolumeThreshold=true;P.View.VolumeThresholdMinimum=-120;P.View.VolumeThresholdMaximum=250;
    P.View.bVolumeIsosurface=true;P.View.VolumeIsovalue=37.25;
    FStudioProject Restored;FString Error;
    TestTrue(TEXT("Volume settings reopen"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Restored,Error));
    TestTrue(TEXT("Every volume setting survives"),StudioView::DisplayEquals(P.View,Restored.View));
    TSharedPtr<FJsonObject> Root;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),Root);
    Root->GetObjectField(TEXT("view"))->RemoveField(TEXT("volumeClipMinimum"));
    auto Encode=[&]{FString S;FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&S));return S;};
    TestFalse(TEXT("Current schema rejects missing volume settings"),StudioProjectIO::Parse(Encode(),Restored,Error));
    Root->SetNumberField(TEXT("version"),10);
    TestTrue(TEXT("Older projects migrate with full volume bounds"),StudioProjectIO::Parse(Encode(),Restored,Error));
    TestTrue(TEXT("Migration default lower bound"),Restored.View.VolumeClipMinimum.IsZero());
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/VolumeView")/FGuid::NewGuid().ToString());
    const auto Before=M.InspectionState();const int32 Frame=M.SelectedFrame;
    TestTrue(TEXT("Clip is an independent view edit"),M.EditView(TEXT("Clip volume"),[](auto& S){S.Display.VolumeClipMinimum.X=.25;}));
    TestTrue(TEXT("Clip edit undoes"),M.UndoView());TestTrue(TEXT("Undo restores exact view"),M.InspectionState().Equals(Before));
    TestTrue(TEXT("Clip edit redoes"),M.RedoView());TestEqual(TEXT("Playback is unchanged"),M.SelectedFrame,Frame);
    TestFalse(TEXT("Reversed clip box is refused"),M.EditView(TEXT("Invalid clip"),[](auto& S){S.Display.VolumeClipMinimum.X=1.;}));
    M.EditView(TEXT("Physical threshold"),[](auto& S){S.Display.VolumeThresholdMinimum=-120;S.Display.VolumeThresholdMaximum=250;S.Display.VolumeIsovalue=37.25;S.Display.bVolumeThreshold=true;});
    M.EditView(TEXT("Palette edit"),[](auto& S){S.Display.VolumeOpacity=.5;});
    TestEqual(TEXT("Physical lower threshold is stable"),M.VolumeThresholdMinimum,-120.);
    TestEqual(TEXT("Physical isovalue is stable"),M.VolumeIsovalue,37.25);
    M.EditView(TEXT("Change units"),[](auto& S){S.Display.ScalarField=TEXT("other_field");});
    TestFalse(TEXT("Switching scalar clears old-unit threshold"),M.bVolumeThreshold);
    TestFalse(TEXT("Switching scalar clears old-unit isosurface"),M.bVolumeIsosurface);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVolumeIso,"Studio.Volume.IsosurfaceAndConservativeSolidMask",VolumeFlags)
bool FStudioVolumeIso::RunTest(const FString&)
{
    // Independent analytic plane in a small structural grid, never CFD sample data.
    FStudioVolumeRenderData Grid;Grid.Dimensions=FIntVector(3);
    Grid.SourceBounds=FBox(FVector(0),FVector(2));
    for(int32 Z=0;Z<3;++Z)for(int32 Y=0;Y<3;++Y)for(int32 X=0;X<3;++X)
        Grid.Texels.Add(FVector2f(X+2*Y+3*Z,1));
    const auto Iso=StudioVolumes::Isosurface(Grid,5.25);
    TestTrue(TEXT("Physical levels beyond palette range remain valid"),Iso.Error.IsEmpty());
    TestTrue(TEXT("Analytic plane produces triangles"),Iso.Indices.Num()>0&&Iso.Indices.Num()%3==0);
    for(const auto& P:Iso.PositionsMeters)
    {
        if(!TestTrue(TEXT("All vertices lie on independent analytic plane"),FMath::Abs(P.X+2*P.Y+3*P.Z-5.25)<1.e-10))return false;
        if(!TestTrue(TEXT("Isosurface stays within source bounds"),Grid.SourceBounds.IsInsideOrOn(P)))return false;
    }
    Grid.CylinderCenter=FVector2D(1,1);Grid.CylinderRadius=.1;
    const auto Solid=StudioVolumes::Isosurface(Grid,5.25);
    TestTrue(TEXT("Cells touching solid are omitted even when all corners are fluid"),Solid.Indices.IsEmpty());
    Grid.CylinderRadius=0;for(auto& P:Grid.Texels)P.Y=0;
    TestTrue(TEXT("Missing samples cannot produce geometry"),StudioVolumes::Isosurface(Grid,5.25).Indices.IsEmpty());
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestTrue(TEXT("Cancelled extraction yields no partial result"),!StudioVolumes::Isosurface(Grid,5.25,Cancel).Error.IsEmpty());
    const auto V=NumericalGrid();V->CylinderCenter=FVector2D(2.5,2.5);V->CylinderRadius=.1;
    TArray<double> Values;Values.Init(1,27);double Value;
    TestFalse(TEXT("CPU interpolation rejects a cell crossing the solid"),V->Sample(FVector(2.1,2.1,.5),Values,Value));
    TestTrue(TEXT("CPU interpolation outside the solid cell works"),V->Sample(FVector(3.5,3.5,.5),Values,Value));
    return true;
}
#endif
