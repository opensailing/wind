#include "StudioHome4TankPresets.h"
#include "StudioHome4Setup.h"
#include "StudioHome4JSON.h"
#include "StudioFileDialog.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#define UI UI_HOME4_TANK_PRESETS
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI
namespace StudioHome4TankPresetsLocal
{
bool Text(const FString& V,int32 Max)
{
    if(V.TrimStartAndEnd().IsEmpty()||V.Len()>Max)return false;
    for(TCHAR C:V){if(C<32||C==127)return false;}
    return true;
}
FString JSON(const TSharedRef<FJsonObject>& O){FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
bool Hash(const TArray<uint8>& Bytes,FString& Out)
{uint8 Digest[32];unsigned int N=0;if(EVP_Digest(Bytes.GetData(),Bytes.Num(),Digest,&N,EVP_sha256(),nullptr)!=1||N!=32)return false;Out=BytesToHex(Digest,N).ToLower();return true;}
bool Inside(const FVector& Min,const FVector& Max,const FIntVector& Extents)
{
    for(int32 A=0;A<3;++A){if(Min[A]<0||Max[A]>Extents[A]||!FMath::IsFinite(Min[A])||!FMath::IsFinite(Max[A]))return false;}
    return true;
}
FString Payload(const FStudioHome4Spec& S)
{
    const auto Full=StudioHome4Config::ToJSON(S);auto O=MakeShared<FJsonObject>();O->SetObjectField(TEXT("lattice"),Full->GetObjectField(TEXT("lattice")));O->SetObjectField(TEXT("zones"),Full->GetObjectField(TEXT("zones")));
    const auto Auth=Full->GetObjectField(TEXT("authoring"));auto A=MakeShared<FJsonObject>();
    for(const TCHAR* K:{TEXT("waterlineCells"),TEXT("zoneUnits"),TEXT("zones"),TEXT("boundaryWall"),TEXT("inletMode"),TEXT("outletMode"),TEXT("pierceMode")})if(const auto V=Auth->TryGetField(K))A->SetField(K,V);
    O->SetObjectField(TEXT("authoring"),A);return JSON(O);
}
bool Original(const FStudioHome4TankZonePreset& P,FString& Error)
{
    FString SHA;if(P.OriginalBytes.IsEmpty()||P.OriginalBytes.Num()>StudioHome4TankPresets::MaximumBytes||!Hash(P.OriginalBytes,SHA)||SHA!=P.SourceSHA256||!StudioHome4JSON::UTF8(P.OriginalBytes.GetData(),P.OriginalBytes.Num()))
    {Error=TEXT("Original preset bytes/hash are absent or changed.");return false;}
    const int32 Offset=P.OriginalBytes.Num()>=3&&P.OriginalBytes[0]==0xef&&P.OriginalBytes[1]==0xbb&&P.OriginalBytes[2]==0xbf?3:0;
    const FUTF8ToTCHAR T(reinterpret_cast<const ANSICHAR*>(P.OriginalBytes.GetData()+Offset),P.OriginalBytes.Num()-Offset);FStudioHome4TankZonePreset Parsed;
    if(!StudioHome4TankPresets::Parse(FString(T.Length(),T.Get()),Parsed,Error)||Parsed.Name!=P.Name||Parsed.SourceId!=P.SourceId||StudioHome4Config::Serialize(Parsed.OriginalSpec)!=StudioHome4Config::Serialize(P.OriginalSpec))
    {Error=TEXT("Retained preset definition no longer agrees with its original bytes.");return false;}
    return true;
}
}
bool StudioHome4TankPresets::Parse(const FString& JSON,FStudioHome4TankZonePreset& Out,FString& Error)
{
    using namespace StudioHome4TankPresetsLocal;const FTCHARToUTF8 B(*JSON);TSharedPtr<FJsonObject> O;
    auto Fail=[&](const TCHAR* V){Error=V;return false;};
    if(B.Length()<1||B.Length()>MaximumBytes||!StudioHome4JSON::Preflight(JSON)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON),O)||!O)return Fail(TEXT("Original G/Q/P preset requires strict JSON within 1 MiB."));
    const TSet<FString> Allowed={TEXT("schema"),TEXT("version"),TEXT("name"),TEXT("source_id"),TEXT("run_spec")};for(const auto& V:O->Values)if(!Allowed.Contains(FString(*V.Key)))return Fail(TEXT("Unknown original tank-preset envelope field."));
    FStudioHome4TankZonePreset P;FString Schema;double Version=0;const TSharedPtr<FJsonObject>* Spec=nullptr;
    if(!O->TryGetStringField(TEXT("schema"),Schema)||Schema!=TEXT("LBMStudio.Home4TankZonePreset")||!O->TryGetNumberField(TEXT("version"),Version)||Version!=1||
       !O->TryGetStringField(TEXT("name"),P.Name)||!TSet<FString>{TEXT("G"),TEXT("Q"),TEXT("P")}.Contains(P.Name)||!O->TryGetStringField(TEXT("source_id"),P.SourceId)||!Text(P.SourceId,2048)||
       !O->TryGetObjectField(TEXT("run_spec"),Spec)||!StudioHome4Config::FromJSON(*Spec,P.OriginalSpec,Error))return Fail(TEXT("Preset needs schema/version, explicit G/Q/P name, original source_id and a valid full run_spec."));
    const auto& S=P.OriginalSpec;
    const TSharedPtr<FJsonObject>* Auth=nullptr;
    if(!(*Spec)->TryGetObjectField(TEXT("authoring"),Auth)||!Auth||!Auth->IsValid())return Fail(TEXT("Original preset needs an explicit authoring object."));
    for(const TCHAR* K:{TEXT("waterlineCells"),TEXT("zoneUnits"),TEXT("zones"),TEXT("boundaryWall"),TEXT("inletMode"),TEXT("outletMode"),TEXT("pierceMode")})if(!(*Auth)->HasField(K))return Fail(TEXT("Original preset must explicitly declare every applied authoring field; implicit boundary defaults are not accepted."));
    if(S.RecipeId!=TEXT("th01-hull")||!S.Lattice.Extents||!S.Reference.LengthCells||*S.Reference.LengthCells<=0||!S.Authoring.WaterlineCells||
       *S.Authoring.WaterlineCells<0||*S.Authoring.WaterlineCells>S.Lattice.Extents->Z||S.Authoring.Zones.IsEmpty())return Fail(TEXT("TH01 preset needs original positive reference L, tank XYZ, waterline and explicit authored zone definitions. Missing G/Q/P values are never inferred."));
    double Scale=0;if(!StudioHome4Setup::ZoneScale(S,Scale,Error))return false;
    int64 Nodes=1;for(int32 A=0;A<3;++A){const int32 N=(*S.Lattice.Extents)[A];if(N<1||N>1048576||Nodes>1000000000000LL/N)return Fail(TEXT("Original preset tank exceeds bounded frontend root node counts."));Nodes*=N;}
    for(const auto& Z:S.Authoring.Zones)if(!Inside(Z.Minimum*Scale,Z.Maximum*Scale,*S.Lattice.Extents))return Fail(TEXT("Original preset zones must lie inside its declared tank with its original coordinate map."));
    P.OriginalBytes.Append(reinterpret_cast<const uint8*>(B.Get()),B.Length());if(!Hash(P.OriginalBytes,P.SourceSHA256))return Fail(TEXT("Original preset hash could not be calculated."));
    Out=MoveTemp(P);Error.Empty();return true;
}
bool StudioHome4TankPresets::Load(const FString& Path,FStudioHome4TankZonePreset& Out,FString& Error,const TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe>& Cancel,TFunction<void()> BeforeVerify)
{
    using namespace StudioHome4TankPresetsLocal;const FString Full=FPaths::ConvertRelativePathToFull(Path);auto Cancelled=[&]{return Cancel&&Cancel->load();};
    if(!Text(Full,2048)||Cancelled()){Error=TEXT("Preset import cancelled or original path is invalid.");return false;}FStudioFileAccess Access(Full);
    const int64 Size=IFileManager::Get().FileSize(*Full);const auto Stamp=IFileManager::Get().GetTimeStamp(*Full);
    if(Size<1||Size>MaximumBytes){Error=TEXT("Original preset file must be 1 byte–1 MiB.");return false;}
    auto Read=[&](TArray<uint8>& Bytes){if(Cancelled())return false;TUniquePtr<FArchive> F(IFileManager::Get().CreateFileReader(*Full,FILEREAD_Silent));if(!F||F->TotalSize()!=Size)return false;Bytes.SetNumUninitialized(int32(Size));F->Serialize(Bytes.GetData(),Size);const bool Good=!F->IsError();const bool Closed=F->Close();return Good&&Closed&&!Cancelled();};
    TArray<uint8> Bytes,Verified;if(!Read(Bytes)||!StudioHome4JSON::UTF8(Bytes.GetData(),Bytes.Num())){Error=TEXT("Original preset is unavailable, not UTF-8 or cancelled.");return false;}
    const int32 Offset=Bytes.Num()>=3&&Bytes[0]==0xef&&Bytes[1]==0xbb&&Bytes[2]==0xbf?3:0;const FUTF8ToTCHAR T(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+Offset),Bytes.Num()-Offset);FStudioHome4TankZonePreset P;
    if(!Parse(FString(T.Length(),T.Get()),P,Error))return false;
    if(BeforeVerify)BeforeVerify();
    if(!Read(Verified)||Bytes!=Verified||IFileManager::Get().FileSize(*Full)!=Size||IFileManager::Get().GetTimeStamp(*Full)!=Stamp){Error=TEXT("Original preset changed during import or was cancelled; previous slot retained.");return false;}
    if(P.SourceId.Len()+Full.Len()+3>4096){Error=TEXT("Original source identity and path exceed the retained provenance bound.");return false;}
    P.OriginalBytes=MoveTemp(Bytes);P.SourcePath=Full;if(!Hash(P.OriginalBytes,P.SourceSHA256)){Error=TEXT("Original preset hash could not be calculated.");return false;}Out=MoveTemp(P);Error.Empty();return true;
}
bool StudioHome4TankPresets::Apply(const FStudioHome4TankZonePreset& P,const FStudioHome4Spec& Current,FStudioHome4Spec& Out,FString& Error)
{
    using namespace StudioHome4TankPresetsLocal;if(!Original(P,Error)||!StudioHome4Config::Validate(Current,Error))return false;
    const auto& S=P.OriginalSpec;
    if(Current.RecipeId!=TEXT("th01-hull")||Current.Reference.LengthCells!=S.Reference.LengthCells||(S.Authoring.ZoneUnits==TEXT("physical-metres")&&Current.Units.DxMeters!=S.Units.DxMeters))
    {Error=TEXT("TH01 preset requires matching original reference L and any physical zone length map. Change neither silently.");return false;}
    FStudioHome4Spec N=Current;N.Lattice=S.Lattice;N.Zones=S.Zones;N.Authoring.WaterlineCells=S.Authoring.WaterlineCells;N.Authoring.ZoneUnits=S.Authoring.ZoneUnits;N.Authoring.Zones=S.Authoring.Zones;
    N.Authoring.BoundaryWall=S.Authoring.BoundaryWall;N.Authoring.InletMode=S.Authoring.InletMode;N.Authoring.OutletMode=S.Authoring.OutletMode;N.Authoring.PierceMode=S.Authoring.PierceMode;
    for(const auto& Patch:N.Authoring.Patches){const FVector Size=FVector(Patch.Extents)*FMath::Pow(2.,-Patch.Level);if(!Inside(Patch.Origin,Patch.Origin+Size,*N.Lattice.Extents)){Error=TEXT("Preset tank would clip a current authored patch; request retained.");return false;}}
    for(const auto& Position:{N.Geometry.InitialPositionCells,N.Geometry.CenterOfGravity})if(Position&&!Inside(*Position,*Position,*N.Lattice.Extents)){Error=TEXT("Preset tank would exclude current body origin or CoG; request retained.");return false;}
    if(N.Lattice.Extents!=Current.Lattice.Extents)
    {
        StudioHome4Setup::InvalidatePatchCounts(N);const int64 Nodes=int64(N.Lattice.Extents->X)*N.Lattice.Extents->Y*N.Lattice.Extents->Z;
        for(auto& Allocation:N.Performance.Allocations)if(Allocation.NodeScope==TEXT("root"))Allocation.Nodes=Nodes;
        N.Performance.OutputByteEstimates.Reset();N.Performance.OutputEstimateSource.Empty();N.Performance.OutputEstimateAssumption.Empty();
    }
    N.Authoring.TankZonePresetName=P.Name;N.Authoring.TankZonePresetSourceId=P.SourceId+(P.SourcePath.IsEmpty()?FString():TEXT(" | ")+P.SourcePath);N.Authoring.TankZonePresetSourceSHA256=P.SourceSHA256;
    if(!StudioHome4Config::Validate(N,Error))return false;Out=MoveTemp(N);Error.Empty();return true;
}
bool StudioHome4TankPresets::MatchesApplied(const FStudioHome4Spec& S,const FStudioHome4TankZonePreset& P)
{
    FString E;return StudioHome4TankPresetsLocal::Original(P,E)&&S.Authoring.TankZonePresetName==P.Name&&S.Authoring.TankZonePresetSourceSHA256==P.SourceSHA256&&
        S.Reference.LengthCells==P.OriginalSpec.Reference.LengthCells&&(P.OriginalSpec.Authoring.ZoneUnits!=TEXT("physical-metres")||S.Units.DxMeters==P.OriginalSpec.Units.DxMeters)&&
        StudioHome4TankPresetsLocal::Payload(S)==StudioHome4TankPresetsLocal::Payload(P.OriginalSpec);
}
bool StudioHome4TankPresets::Export(const FStudioHome4TankZonePreset& P,const FString& Destination,FString& Error)
{
    using namespace StudioHome4TankPresetsLocal;if(!Original(P,Error))return false;const FString Path=FPaths::ConvertRelativePathToFull(Destination);
    if(!Text(Path,4096)||IFileManager::Get().FileExists(*Path)){Error=TEXT("Choose a new original-preset destination; existing files are never overwritten.");return false;}FStudioFileAccess Access(Path);
    const FString Stage=Path+TEXT(".stage-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);bool Good=FFileHelper::SaveArrayToFile(P.OriginalBytes,*Stage, &IFileManager::Get(),FILEWRITE_NoReplaceExisting);
    if(Good)Good=IFileManager::Get().Move(*Path,*Stage,false,false,false,true);
    if(!Good){IFileManager::Get().Delete(*Stage,false,true);Error=TEXT("Could not publish the original preset to a new destination.");return false;}Error.Empty();return true;
}
FString StudioHome4TankPresets::Review(const FStudioHome4TankZonePreset& P)
{
    const auto& S=P.OriginalSpec;FString T=TEXT("Original ")+P.Name+TEXT(" · ")+P.SourceId+TEXT("\n")+P.SourcePath+TEXT("\nSHA256 ")+P.SourceSHA256+TEXT("\nTank XYZ ")+S.Lattice.Extents.Get(FIntVector::ZeroValue).ToString()+TEXT(" · reference L ")+FString::Printf(TEXT("%.9g cells"),S.Reference.LengthCells.Get(0))+TEXT("\nWaterline ")+FString::Printf(TEXT("%.9g root cells"),S.Authoring.WaterlineCells.Get(0))+TEXT(" · zones ")+S.Authoring.ZoneUnits;
    for(const auto& Z:S.Authoring.Zones)T+=TEXT("\n")+Z.Id+TEXT(" · ")+Z.Kind+TEXT(" · ")+Z.Minimum.ToString()+TEXT(" → ")+Z.Maximum.ToString()+TEXT(" · ")+Z.Profile+TEXT(" ")+Z.Axis+FString::Printf(TEXT(" · strength %.9g · level exponent %.9g"),Z.Strength,Z.LevelExponent);
    return T+TEXT("\nComplete original tank/zone request:\n")+StudioHome4TankPresetsLocal::Payload(S);
}
