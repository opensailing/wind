#include "StudioFlowPresentation.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFlowPresentationTest,"Studio.FlowPresentation.SourceAndViewIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioFlowPresentationTest::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/FlowPresentationModel"));
    const auto Field=M.Solver->CaptureField(0);const auto Before=M.InspectionState();
    const auto Case=StudioCaseIO::Serialize(M.Project.Draft);const auto OriginalRange=M.ActiveColorMapping();
    FStudioInspectionState Overview;
    if(!TestTrue(TEXT("Published airfoil supports overview"),StudioFlowPresentation::Overview(*Field,M.Solver->Descriptor().DisplayBounds,
        M.ActiveScalar().Id,1.9,Before,Overview)))return false;
    TestTrue(TEXT("Readable layers"),Overview.Display.StreamlineSettings.bDirectionMarkers&&Overview.Display.bStreamlines&&Overview.Display.bCutPlane&&!Overview.Display.bVolume&&!Overview.Display.bVectors);
    TestTrue(TEXT("Valid arbitrary perspective camera"),StudioView::IsValid(Overview)&&!Overview.Camera.bOrthographic);
    TestTrue(TEXT("Apply as one view edit"),M.EditView(TEXT("Flow overview"),[&](auto& S){S=Overview;}));
    TestTrue(TEXT("Actual frame gets explicitly custom range"),M.ActiveColorMapping().bManualRange);
    TestTrue(TEXT("Local range improves use of the palette"),M.ActiveColorMapping().Maximum<OriginalRange.Maximum);
    TestEqual(TEXT("Same source snapshot"),M.SelectedFrame,0);
    TestEqual(TEXT("Case unaffected"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    TestTrue(TEXT("Undo restores all settings"),M.UndoView()&&M.InspectionState().Equals(Before));
    TestTrue(TEXT("Redo restores exact view"),M.RedoView()&&M.InspectionState().Equals(Overview));
    FString Error;FStudioProject Loaded;
    const auto Path=FPaths::ProjectSavedDir()/TEXT("Automation/flow-overview.lbms");
    TestTrue(TEXT("Save view"),M.SaveProject(Path));TestTrue(TEXT("Read saved view"),StudioProjectIO::Load(Path,Loaded,Error));
    TestTrue(TEXT("Lens and custom mapping persist"),FStudioInspectionState{Loaded.Camera,Loaded.View}.Equals(Overview));
    TestFalse(TEXT("Missing scalar cannot generate a range"),StudioFlowPresentation::Overview(*Field,M.Solver->Descriptor().DisplayBounds,TEXT("absent"),1.9,Before,Overview));
    const FStudioColorMapping Mapping{0,true,30000,130000};
    const auto Slice=StudioFlowPresentation::OriginalSlice(*Field,M.Solver->Descriptor().DisplayBounds,0,Mapping,TEXT("pressure"),{});
    TestTrue(TEXT("Original CFD topology reaches surface"),Slice.Positions.Num()>15000&&Slice.Positions.Num()==Slice.ScalarOpacity.Num());
    // Pressure is a linear scalar: check every clipped vertex against the independently sampled field.
    for(int32 I=0;I<Slice.Positions.Num();I+=17)
    {
        const auto P=Slice.Positions[I]/100.;double Pressure;
        if(!TestTrue(TEXT("Vertex in actual flow"),Field->SampleScalar(P,TEXT("pressure"),Pressure)))return false;
        if(!TestTrue(TEXT("Original pressure interpolates before color mapping"),FMath::Abs(Slice.ScalarOpacity[I].X-(Pressure-30000)/100000)<1.e-5))return false;
        if(!TestTrue(TEXT("Clipped to display bounds"),M.Solver->Descriptor().DisplayBounds.ExpandBy(1.e-7).IsInsideOrOn(P)))return false;
    }
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    const auto Speed=StudioFlowPresentation::OriginalSlice(*Field,M.Solver->Descriptor().DisplayBounds,0,{0,true,0,100},TEXT("velocity_magnitude"),{});
    for(int32 I=0;I+2<Speed.Positions.Num();I+=3)
    {
        const auto Center=(Speed.Positions[I]+Speed.Positions[I+1]+Speed.Positions[I+2])/300.;
        const auto Velocity=(Speed.Velocity[I]+Speed.Velocity[I+1]+Speed.Velocity[I+2])/3.;double Value;
        if(!Field->SampleScalar(Center,TEXT("velocity_magnitude"),Value))continue;
        if(!TestTrue(TEXT("Shader inputs derive speed after interpolation, matching probes"),FMath::Abs(Velocity.Size()-Value)<1.e-5))return false;
    }
    TestTrue(TEXT("Cancelled source surface cannot publish partially"),StudioFlowPresentation::OriginalSlice(*Field,M.Solver->Descriptor().DisplayBounds,0,Mapping,TEXT("pressure"),Cancel).Positions.IsEmpty());
    FStudioSeedObject Seed;Seed.Name=TEXT("Direction check");const auto Id=Field->Identity();const auto Bounds=M.Solver->Descriptor().DisplayBounds;
    Seed.Source={Id->Dataset,Id->MetadataSHA256,Id->PayloadSHA256};Seed.Kind=EStudioSeedKind::Points;
    Seed.Points={FVector(Bounds.GetCenter().X,Id->SourceOffset.Y,Bounds.Min.Z+Bounds.GetSize().Z*.8)};
    auto Settings=M.StreamlineSettings;Settings.Direction=EStudioStreamDirection::Both;FStudioStreamlineOutput Streams;
    if(!TestTrue(TEXT("Trace original field in both integration directions"),StudioStreamlines::Build(*Field,Bounds,{Seed},Settings,TEXT("pressure"),Streams,Error)))
    {AddError(Error);return false;}
    for(bool Backward:{false,true})
    {
        auto Branch=Streams;Branch.Paths.RemoveAll([Backward](const auto& P){return P.bBackward!=Backward;});
        const auto Markers=StudioFlowPresentation::DirectionMarkers(*Field,Branch,Bounds);
        if(!TestTrue(TEXT("Both tracing directions have readable markers"),!Markers.IsEmpty()))return false;
        for(const auto& Marker:Markers)
        {
            FVector V;double Pressure;
            if(!TestTrue(TEXT("Arrow follows recorded flow even on backward trace"),Field->SampleVelocity(Marker.PositionMeters,V)&&Marker.Direction.Equals(V.GetSafeNormal(),1.e-9)))return false;
            if(!TestTrue(TEXT("Arrow uses actual selected scalar"),Field->SampleScalar(Marker.PositionMeters,TEXT("pressure"),Pressure)&&Marker.Scalar==Pressure))return false;
        }
    }
    auto Many=Streams;for(int32 I=0;I<1024;++I)Many.Paths.Add(Streams.Paths[0]);
    TestTrue(TEXT("Dense traces retain bounded arrow geometry"),StudioFlowPresentation::DirectionMarkers(*Field,Many,Bounds).Num()<=2048);
    TestTrue(TEXT("Canceled markers never publish partially"),StudioFlowPresentation::DirectionMarkers(*Field,Streams,Bounds,Cancel).IsEmpty());
    Streams.Identity->Ordinal+=1;
    TestTrue(TEXT("Stale trace cannot borrow another frame's velocity"),StudioFlowPresentation::DirectionMarkers(*Field,Streams,Bounds).IsEmpty());
    return true;
}
#endif
