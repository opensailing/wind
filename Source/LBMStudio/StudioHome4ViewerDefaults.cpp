#include "StudioHome4ViewerDefaults.h"
#include "StudioHome4JSON.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/Archive.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
FString StudioHome4ViewerDefaults::Path(){return FPaths::ProjectSavedDir()/TEXT("Home4ViewerDefaults.json");}
bool StudioHome4ViewerDefaults::Save(const FString& Destination,const FStudioViewSettings& View,const FStudioCameraState& Camera,FString& Error)
{
    auto Copy=View;Copy.ScalarField.Empty();Copy.VectorField=TEXT("velocity");auto Palette=Copy.ScalarStyles.FindByPredicate([](const auto& S){return S.Dataset==TEXT("*")&&S.Field==TEXT("*");});TOptional<FStudioScalarStyle> DefaultPalette;if(Palette)DefaultPalette=*Palette;Copy.ScalarStyles.Reset();if(DefaultPalette){DefaultPalette->bManualRange=false;DefaultPalette->Minimum=0;DefaultPalette->Maximum=1;Copy.ScalarStyles.Add(*DefaultPalette);}Copy.InspectionObjects=FStudioInspectionObjects();Copy.StreamlineSettings.VelocityField=TEXT("velocity");
    // No original field units, manual ranges, scientific masks or tool coordinates
    // may migrate to an unrelated newly authored project.
    Copy.bVolumeThreshold=false;Copy.bVolumeIsosurface=false;Copy.VolumeThresholdMinimum=0;Copy.VolumeThresholdMaximum=1;Copy.VolumeIsovalue=.5;
    Copy.bHome4InterfaceSurface=false;Copy.bHome4ObstacleSurface=false;Copy.bHome4SdfSurface=false;Copy.bHome4Vorticity=false;Copy.bFocusWingRegion=false;
    auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("format"),TEXT("Home4ViewerDefaults"));O->SetNumberField(TEXT("version"),1);O->SetObjectField(TEXT("view"),StudioProjectIO::ViewToJSON(Copy));O->SetObjectField(TEXT("camera"),StudioProjectIO::CameraToJSON(Camera));
    FString Text;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));FStudioViewSettings Check;FStudioCameraState CheckCamera;const TSharedPtr<FJsonObject>* V=nullptr,*C=nullptr;
    if(!O->TryGetObjectField(TEXT("view"),V)||!StudioProjectIO::ViewFromJSON(*V,Check)||!O->TryGetObjectField(TEXT("camera"),C)||!StudioProjectIO::CameraFromJSON(*C,CheckCamera)){Error=TEXT("Current viewer defaults failed native display validation.");return false;}
    return StudioProjectIO::WriteAtomic(Destination,Text,Error);
}
bool StudioHome4ViewerDefaults::Load(const FString& Source,FStudioViewSettings& View,FStudioCameraState& Camera,FString& Error)
{
    FString Text;TArray<uint8> Bytes;TSharedPtr<FJsonObject> O;
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Source));
    if(!Reader||Reader->TotalSize()<1||Reader->TotalSize()>256*1024){Error=TEXT("Viewer defaults must be a readable JSON file under 256 KiB.");return false;}
    Bytes.SetNumUninitialized(int32(Reader->TotalSize()));Reader->Serialize(Bytes.GetData(),Bytes.Num());
    if(Reader->IsError()||Reader->TotalSize()!=Bytes.Num()||!StudioHome4JSON::UTF8(Bytes.GetData(),Bytes.Num())){Error=TEXT("Viewer defaults must be bounded valid UTF-8 JSON.");return false;}
    const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());Text=FString(Decoded.Length(),Decoded.Get());
    if(!StudioHome4JSON::Preflight(Text)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O)){if(Error.IsEmpty())Error=TEXT("Viewer defaults must be valid bounded JSON.");return false;}
    FString Format;double Version;const TSharedPtr<FJsonObject>* V=nullptr,*C=nullptr;FStudioViewSettings Candidate;FStudioCameraState Pose;
    if(!O->TryGetStringField(TEXT("format"),Format)||Format!=TEXT("Home4ViewerDefaults")||!O->TryGetNumberField(TEXT("version"),Version)||Version!=1||!O->TryGetObjectField(TEXT("view"),V)||!StudioProjectIO::ViewFromJSON(*V,Candidate)||!O->TryGetObjectField(TEXT("camera"),C)||!StudioProjectIO::CameraFromJSON(*C,Pose)){Error=TEXT("Viewer defaults contain invalid display/camera settings.");return false;}
    if(!Candidate.ScalarField.IsEmpty()||Candidate.ScalarStyles.ContainsByPredicate([](const auto& S){return S.Dataset!=TEXT("*")||S.Field!=TEXT("*")||S.bManualRange;})||Candidate.bVolumeThreshold||Candidate.bVolumeIsosurface||Candidate.InspectionObjects.Slices.Num()||Candidate.InspectionObjects.Probes.Num()||Candidate.InspectionObjects.Rulers.Num()||Candidate.InspectionObjects.Seeds.Num()) {Error=TEXT("Global viewer defaults must contain only source-independent display settings and palette.");return false;}
    View=MoveTemp(Candidate);Camera=Pose;Error.Empty();return true;
}
bool StudioHome4ViewerDefaults::ApplyToNewProject(FStudioProject& Project,FString& Error,const FString& OverridePath)
{
    const FString Source=OverridePath.IsEmpty()?Path():OverridePath;if(!IFileManager::Get().FileExists(*Source)){Error.Empty();return true;}
    FStudioViewSettings View;FStudioCameraState Camera;if(!Load(Source,View,Camera,Error))return false;
    Project.View=View;Project.Camera=Camera;Project.ViewerDefaults=View;Project.ViewerCameraDefaults=Camera;Project.bHasViewerDefaults=true;return true;
}
