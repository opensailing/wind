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
    // Exercise the preset from an explicitly different user view; the new
    // document now already has an overview, which must not create a no-op undo.
    M.bVectors=true;M.bVolume=true;M.ScalarStyles.Reset();M.Project.Camera=FStudioCameraState();
    const auto Field=M.Solver->CaptureField(0);const auto Before=M.InspectionState();
    const auto Case=StudioCaseIO::Serialize(M.Project.Draft);const auto OriginalRange=M.ActiveScalar();
    FStudioInspectionState Overview;
    if(!TestTrue(TEXT("Published airfoil supports overview"),StudioFlowPresentation::Overview(*Field,M.Solver->Descriptor().DisplayBounds,
        M.ActiveScalar().Id,1.9,Before,Overview)))return false;
    TestTrue(TEXT("Readable layers"),Overview.Display.StreamlineSettings.bDirectionMarkers&&Overview.Display.bStreamlines&&Overview.Display.bCutPlane&&!Overview.Display.bVolume&&!Overview.Display.bVectors);
    TestTrue(TEXT("Valid arbitrary perspective camera"),StudioView::IsValid(Overview)&&!Overview.Camera.bOrthographic);
    const auto Domain=M.Solver->Descriptor().DisplayBounds;
    const auto FocusBounds=StudioFlowPresentation::OverviewBounds(*Field,Domain);
    TestTrue(TEXT("Overview crops peripheral context through camera framing only"),FocusBounds.IsValid&&
        Domain.IsInsideOrOn(FocusBounds.Min)&&Domain.IsInsideOrOn(FocusBounds.Max)&&FocusBounds.GetSize().X<Domain.GetSize().X*.8);
    for(const auto& P:Field->Boundary())
        if(!TestTrue(TEXT("Complete original wing remains inside overview"),FocusBounds.IsInsideOrOn(FVector(P.X,0,P.Y))))return false;
    TestTrue(TEXT("Overview brings original solid closer than whole-domain Fit"),Overview.Camera.OrbitDistance<
        StudioView::FitBounds(Overview.Camera,Domain,1.9,.01).OrbitDistance);
    for(double Aspect:{1.2,1.9,2.5})
    {
        const auto C=StudioView::FitBounds(Overview.Camera,FocusBounds,Aspect,.01);
        const double TanH=FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5));
        for(int32 I=0;I<8;++I)
        {
            const FVector P=FVector(I&1?FocusBounds.Max.X:FocusBounds.Min.X,I&2?FocusBounds.Max.Y:FocusBounds.Min.Y,
                I&4?FocusBounds.Max.Z:FocusBounds.Min.Z)-C.Position;
            const double Depth=FVector::DotProduct(P,C.Orientation.GetForwardVector());
            if(!TestTrue(TEXT("Wing/wake fit retains depth clearance and complete visible region at each aspect"),Depth>.01&&
                FMath::Abs(FVector::DotProduct(P,C.Orientation.GetRightVector()))<Depth*TanH&&
                FMath::Abs(FVector::DotProduct(P,C.Orientation.GetUpVector()))<Depth*TanH/Aspect))return false;
        }
    }
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInitialFlowViewTest,"Studio.FlowPresentation.NewDocumentView",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioInitialFlowViewTest::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/NewFlowView")/FGuid::NewGuid().ToString();
    FStudioModel M(Root);
    const auto OriginalField=M.ActiveScalar();const auto Case=StudioCaseIO::Serialize(M.Project.Draft);
    TestTrue(TEXT("New app opens with readable recorded flow"),!M.bVectors&&!M.bVolume&&M.bCutPlane&&M.bStreamlines&&
        M.StreamlineSettings.AutomaticSeedCount==60&&M.StreamlineSettings.bDirectionMarkers&&M.ActiveColorMapping().bManualRange);
    TestFalse(TEXT("Initial view has no fabricated user edits"),M.HasUnsavedChanges()||M.CanUndoView());
    TestTrue(TEXT("Initial layout fits actual aspect"),M.FitNewFlowView(2.1));
    TestFalse(TEXT("Initial fit stays clean and outside history"),M.HasUnsavedChanges()||M.CanUndoView());
    const auto Fitted=M.InspectionState();
    TestFalse(TEXT("Later resizing does not refit camera"),M.FitNewFlowView(1.1));
    TestTrue(TEXT("Later resize retains view"),M.InspectionState().Equals(Fitted));
    TestEqual(TEXT("Source extrema remain authoritative"),M.ActiveScalar().Maximum,OriginalField.Maximum);
    TestEqual(TEXT("Initial framing never changes the case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    M.NewProject(TEXT("Fresh airfoil"));TestTrue(TEXT("Explicit New remains unsaved"),M.HasUnsavedChanges());
    TestTrue(TEXT("Fresh project requests its own initial fit"),M.FitNewFlowView(1.4));
    TestTrue(TEXT("Fitting New does not pretend it is saved"),M.HasUnsavedChanges());
    M.NewProject(TEXT("Early camera edit"));M.EditCamera(TEXT("Position"),FStudioCameraState());const auto Edited=M.InspectionState();
    TestFalse(TEXT("Early user camera edit cancels automatic fit"),M.FitNewFlowView(2.));
    TestTrue(TEXT("User edit is retained exactly"),M.InspectionState().Equals(Edited));
    auto Saved=Edited;Saved.Display.bVolume=true;Saved.Display.bVectors=true;Saved.Display.ScalarStyles.Reset();
    M.EditView(TEXT("Custom saved view"),[&](auto& S){S=Saved;});
    const FString Path=Root/TEXT("saved.lbms");TestTrue(TEXT("Save custom view"),M.SaveProject(Path));
    M.NewProject(TEXT("Temporary"));TestTrue(TEXT("Open custom view"),M.LoadProject(Path));
    TestFalse(TEXT("Opened document has no initial fit"),M.FitNewFlowView(3.));
    TestTrue(TEXT("Saved layers, mapping and camera survive exactly"),M.InspectionState().Equals(Saved));
    TestFalse(TEXT("Reopened document is clean"),M.HasUnsavedChanges());
    auto RecoveryCamera=M.Project.Camera;RecoveryCamera.Position.X+=.5;M.EditCamera(TEXT("Recovery camera"),RecoveryCamera);const auto Recovery=M.InspectionState();M.WriteRecovery();
    FStudioModel Recovered(Root);Recovered.OpenSession();TestTrue(TEXT("Recover prior view"),Recovered.RestoreRecovery());
    TestFalse(TEXT("Recovery never requests automatic framing"),Recovered.FitNewFlowView(3.));
    TestTrue(TEXT("Recovered view survives exactly"),Recovered.InspectionState().Equals(Recovery));
    M.NewProject(TEXT("Save before first layout"));TestTrue(TEXT("Early save succeeds"),M.SaveProject(Root/TEXT("early.lbms")));
    TestFalse(TEXT("A saved view cannot be automatically reframed"),M.FitNewFlowView(3.));
    return true;
}
#endif
