#include "StudioWorkspace.h"
#include "StudioModel.h"
#include "StudioScene.h"
#include "StudioHome4Archive.h"
#include "StudioHome4Telemetry.h"
#include "StudioHome4SpatialDiagnostics.h"
#include "HAL/PlatformProcess.h"
#include "StudioHome4Recipes.h"
#include "StudioHeadlessSlate.h"
#include "StudioPointRecording.h"
#include "StudioVolume.h"
#include "StudioProbeMarkers.h"
#include "StudioInspectionOverlay.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "StudioHome4LocatorFixture.inl"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4LocatorWorkspaceTest,"Studio.HeadlessUI.Home4.OriginalCellMarkerCameraFactsAndScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FHome4LocatorWorkspaceTest::RunTest(const FString&)
{
    const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())/TEXT("Automation")/(TEXT("CellLocator_")+FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Root,true);ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Root,false,true);};
    TArray<uint8> Bytes;FBase64::Decode(StudioHome4LocatorFixture::xy,Bytes);const FString Path=Root/TEXT("artificial.npz");
    FFileHelper::SaveArrayToFile(Bytes,*Path);
    FStudioHome4ArchiveRequest R;R.Sources={StudioHome4Archives::Inspect(Path).Source};R.OutputParent=Root;R.FolderName=TEXT("original");
    R.SourceURI=TEXT("urn:artificial-locator-contract");R.Attribution=TEXT("Artificial affine geometry fixture, never a CFD sample.");
    R.Mapping.AxisOrder=TEXT("xy");R.Mapping.MetadataOrder=TEXT("xyz");R.Mapping.CoordinateUnits=R.Mapping.VelocityUnits=TEXT("lattice");R.Mapping.DxMeters=.1;R.Mapping.DtSeconds=.02;
    const auto Converted=StudioHome4Archives::Convert(R);if(!TestTrue(Converted.Error,Converted.bSuccess))return false;
    for(const int32 Level:{0,2})
    {
        if(Level==2)
        {
            FString Text;FFileHelper::LoadFileToString(Text,*Converted.RecordingJSON);TSharedPtr<FJsonObject> O;
            if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O))return false;
            auto P=MakeShared<FJsonObject>();P->SetStringField(TEXT("id"),TEXT("original-fine"));P->SetNumberField(TEXT("level"),2);
            P->SetStringField(TEXT("spatialSourceId"),TEXT("artificial-locator-topology"));P->SetStringField(TEXT("spatialSourceSHA256"),FString::ChrN(64,'b'));
            O->GetObjectField(TEXT("structuredGrid"))->SetObjectField(TEXT("patch"),P);
            Text.Empty();FJsonSerializer::Serialize(O.ToSharedRef(),TJsonWriterFactory<>::Create(&Text));FFileHelper::SaveStringToFile(Text,*Converted.RecordingJSON,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }
        const auto Open=StudioRecordings::Import(Converted.RecordingJSON,0,{});if(!TestTrue(Open.Error,Open.Source.IsValid()))return false;
        auto M=MakeShared<FStudioModel>(Root/FString::FromInt(Level));M->Solver=Open.Source;M->Frames=Open.Source->Descriptor().Frames;
        M->Project.Dataset=Open.Source->Descriptor().Id;M->Project.Draft.Home4=StudioHome4Recipes::Find(TEXT("th01-hull"))->Template;
        TStrongObjectPtr<UWorld> World(UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,false));ON_SCOPE_EXIT{World->DestroyWorld(false);};
        auto* Scene=World->SpawnActor<AStudioScene>();Scene->Model=M;Scene->ApplyCamera(M->Project.Camera);
        auto W=SNew(SStudioWorkspace).Model(M).Scene(Scene);FStudioHeadlessSlate UI(*this,W,FVector2D(1440,1000));
        const auto& G=*M->Solver->VolumeReconstruction()->OriginalGrid;
        FStudioHome4ActionRequest A;if(!TestTrue(TEXT("Original fixture carries a valid source run GUID"),FGuid::Parse(G.SourceRunId,A.Source.RunId)))return false;A.Source.SourceId=TEXT("original-fixture-log");A.Step=777;A.RecordIndex=43;
        auto& F=A.Facts;F.Cell=FIntVector(2,3,0);F.Level=Level;F.PatchId=G.PatchId;F.Phi=.42;F.Tau=.51;F.Limiter=true;F.ForceThreshold=false;F.InBand=true;F.InSponge=false;F.InBeach=true;F.InCutLinkShell=false;F.Zone=TEXT("aft-beach");
        const auto Camera=M->Project.Camera;const auto Frame=M->SelectedFrame;const auto Case=StudioCaseIO::Serialize(M->Project.Draft);
        auto Wrong=A;Wrong.Source.RunId=FGuid::NewGuid();W->LocateHome4Cell(Wrong.Facts,&Wrong);
        TestTrue(TEXT("Foreign original run cannot move camera"),StudioView::CameraEquals(M->Project.Camera,Camera));TestTrue(TEXT("Foreign run creates no marker"),M->InspectionObjects.Probes.IsEmpty());
        Wrong=A;Wrong.Facts.Level=Level+1;W->LocateHome4Cell(Wrong.Facts,&Wrong);
        TestTrue(TEXT("Wrong original level cannot create marker"),M->InspectionObjects.Probes.IsEmpty());
        if(Level>0){Wrong=A;Wrong.Facts.PatchId=TEXT("another-patch");W->LocateHome4Cell(Wrong.Facts,&Wrong);TestTrue(TEXT("Different fine patch rejected"),M->InspectionObjects.Probes.IsEmpty());}
        W->LocateHome4Cell(F,&A);UI.Layout();
        if(!TestEqual(M->Notice,M->InspectionObjects.Probes.Num(),1))return false;
        const auto& Probe=M->InspectionObjects.Probes[0];const FVector Original=G.OriginMeters+G.SpacingMeters*FVector(2,3,0),Expected(Original.X,Original.Z,Original.Y);
        TestTrue(TEXT("Camera uses original affine coordinate with XYZ to scene XZY"),M->Project.Camera.Focus.Equals(Expected,1.e-12));
        TestTrue(TEXT("Camera angle preserved while translating"),(M->Project.Camera.Position-M->Project.Camera.Focus).Equals(Camera.Position-Camera.Focus,1.e-12));
        TestTrue(TEXT("Visible selected original marker uses exact node"),Probe.bVisible&&Probe.A.Equals(Expected,1.e-12)&&Probe.PointId==TOptional<int64>(2+G.OriginalDimensions.X*3)&&M->SelectedInspectionObject==Probe.Id);
        TestTrue(TEXT("Marker retains verified source"),Probe.Source==M->InspectionSource());
        const auto Field=M->Solver->ReadScalarFrame(0,TEXT("phi"));
        if(!TestTrue(Field.Error,Field.Field.IsValid()&&Field.Field->OriginalPoints()&&Field.Field->Identity().IsSet()))return false;
        FStudioProbeMarkerRequest MarkerRequest;MarkerRequest.Project=M->Project.Id;MarkerRequest.Source=M->InspectionSource();
        MarkerRequest.Offset=Field.Field->Identity()->SourceOffset;MarkerRequest.Geometry=Field.Field->OriginalPoints()->Geometry;MarkerRequest.Queries.Add({Probe.Id,*Probe.PointId});
        const auto Resolved=StudioProbeMarkers::Resolve(MarkerRequest);
        const auto Overlay=StudioInspectionOverlay::Build(M->InspectionObjects,M->InspectionSource(),M->Project.Id,Probe.Id,M->Solver->Descriptor().DisplayBounds,&Resolved);
        const auto* Marker=Overlay.Markers.FindByPredicate([&](const auto& V){return V.Object==Probe.Id;});
        TestTrue(TEXT("Production resolver and paint geometry retain actual original marker position"),Marker&&Marker->Position.Equals(Expected,1.e-12));
        if(!UI.Inspect(TEXT("home4-locator-")+FString::FromInt(Level),{TEXT("Home4LocatedCellFacts")}))return false;
        const auto Details=UI.Text(TEXT("Home4LocatedCellFacts"));
        TestTrue(TEXT("Full original facts displayed beside field"),Details.Contains(TEXT("777"))&&Details.Contains(TEXT("Limiter yes"))&&Details.Contains(TEXT("force threshold no"))&&Details.Contains(TEXT("Band yes"))&&Details.Contains(TEXT("sponge no"))&&Details.Contains(TEXT("beach yes"))&&Details.Contains(TEXT("cut-link shell no"))&&Details.Contains(TEXT("aft-beach")));
        TestEqual(TEXT("Locating never invents a frame or scrubs to another output"),M->SelectedFrame,Frame);TestEqual(TEXT("Original case request retained"),StudioCaseIO::Serialize(M->Project.Draft),Case);
        W->LocateHome4Cell(F,&A);TestEqual(TEXT("Repeated same cell reuses marker"),M->InspectionObjects.Probes.Num(),1);
        Wrong=A;Wrong.Facts.Cell=FIntVector(G.OriginalDimensions.X,0,0);const auto LocatedCamera=M->Project.Camera;W->LocateHome4Cell(Wrong.Facts,&Wrong);
        TestTrue(TEXT("Outside original grid cannot move camera"),StudioView::CameraEquals(M->Project.Camera,LocatedCamera));
        if(Level==0)
        {
            auto Spatial=MakeShared<FJsonObject>();Spatial->SetStringField(TEXT("schema"),TEXT("LBMStudio.Home4SpatialDiagnostics"));Spatial->SetNumberField(TEXT("version"),1);
            Spatial->SetStringField(TEXT("run_id"),G.SourceRunId);Spatial->SetStringField(TEXT("source_id"),TEXT("artificial-locator-spatial"));Spatial->SetStringField(TEXT("axis_order"),TEXT("XYZ"));Spatial->SetStringField(TEXT("coordinate_unit"),G.CoordinateUnits);
            auto Patch=MakeShared<FJsonObject>();Patch->SetStringField(TEXT("id"),TEXT("root"));Patch->SetNumberField(TEXT("level"),0);
            auto XYZ=[](const FVector& V){return TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(V.X),MakeShared<FJsonValueNumber>(V.Y),MakeShared<FJsonValueNumber>(V.Z)};};
            Patch->SetArrayField(TEXT("origin"),XYZ(G.OriginalOrigin));Patch->SetArrayField(TEXT("spacing"),XYZ(G.OriginalSpacing));Patch->SetArrayField(TEXT("extents"),XYZ(FVector(G.OriginalDimensions)));
            Spatial->SetArrayField(TEXT("patches"),{MakeShared<FJsonValueObject>(Patch)});FString JSON;FJsonSerializer::Serialize(Spatial,TJsonWriterFactory<>::Create(&JSON));
            const FString SpatialPath=Root/TEXT("spatial.json");FFileHelper::SaveStringToFile(JSON,*SpatialPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            if(!TestTrue(TEXT("Load actual spatial source into workspace"),W->Home4Spatial->BeginImport(SpatialPath,A.Source.RunId)))return false;
            const double Deadline=FPlatformTime::Seconds()+5;
            while(W->Home4Spatial->IsImporting()&&FPlatformTime::Seconds()<Deadline){W->Home4Spatial->Poll();FPlatformProcess::Sleep(.002f);}
            const auto Evidence=W->Home4Spatial->Evidence();if(!TestTrue(W->Home4Spatial->Status(),Evidence.IsValid()))return false;
            FStudioHome4SpatialLocation Location;FString Error;if(!TestTrue(Error,StudioHome4SpatialDiagnostics::Locate(*Evidence,TEXT("root"),FIntVector(1,1,0),Location,Error)))return false;
            W->LocateHome4Spatial(Location);TestEqual(TEXT("Spatial location uses current recording even before a render frame is presented"),M->InspectionObjects.Probes.Num(),2);
            const FVector Point=G.OriginMeters+G.SpacingMeters*FVector(1,1,0);TestTrue(TEXT("Spatial camera uses original affine"),M->Project.Camera.Focus.Equals(FVector(Point.X,Point.Z,Point.Y),1.e-12));
            W->LocateHome4Spatial(Location);TestEqual(TEXT("Repeated spatial location creates a valid uniquely named marker"),M->InspectionObjects.Probes.Num(),3);
            TestEqual(TEXT("Spatial location keeps exact original field frame"),M->SelectedFrame,Frame);
            TestTrue(TEXT("Spatial cell does not retain another reported cell's facts"),W->Home4CellDetailsText().IsEmpty());
        }
        M->InspectionObjects.Probes[0].bVisible=false;TestTrue(TEXT("Hidden marker hides its facts"),W->Home4CellDetailsText().IsEmpty());M->InspectionObjects.Probes[0].bVisible=true;
        M->InspectionObjects.Probes[0].PointId=0;TestTrue(TEXT("Retargeted marker cannot display old cell facts"),W->Home4CellDetailsText().IsEmpty());
        M->Project.Draft.Id=FGuid::NewGuid();TestTrue(TEXT("Changed case hides old cell facts immediately"),W->Home4CellDetailsText().IsEmpty());
        TestEqual(TEXT("Virtual locator requires no captures"),Scene->GetCaptureCount(),uint64(0));
    }
    return !HasAnyErrors();
}
#endif
