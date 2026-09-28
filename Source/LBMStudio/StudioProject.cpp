#include "StudioProject.h"
#include "StudioView.h"
#include "StudioAssetPaths.h"
#include "StudioFileDialog.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformFileManager.h"

namespace
{
    using FObject = TSharedPtr<FJsonObject>;
    TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (double V : Values) Result.Add(MakeShared<FJsonValueNumber>(V));
        return Result;
    }
    bool ReadNumber(const FObject& O, const TCHAR* Key, double& Out, double Min, double Max)
    {
        return O->TryGetNumberField(Key, Out) && FMath::IsFinite(Out) && Out >= Min && Out <= Max;
    }
    bool ReadInteger(const FObject& O, const TCHAR* Key, int32& Out, int32 Min, int32 Max)
    {
        double V;
        if (!ReadNumber(O, Key, V, Min, Max) || V != FMath::FloorToDouble(V)) return false;
        Out = int32(V); return true;
    }
    bool ReadArray(const FObject& O, const TCHAR* Key, double* Out, int32 Count)
    {
        const TArray<TSharedPtr<FJsonValue>>* A;
        if (!O->TryGetArrayField(Key, A) || A->Num() != Count) return false;
        for (int32 I=0; I<Count; ++I)
            if (!(*A)[I]->TryGetNumber(Out[I]) || !FMath::IsFinite(Out[I]) || FMath::Abs(Out[I])>1.e8) return false;
        return true;
    }
    FObject CameraJSON(const FStudioCameraState& C)
    {
        auto O = MakeShared<FJsonObject>();
        O->SetArrayField(TEXT("positionMeters"), Numbers({C.Position.X,C.Position.Y,C.Position.Z}));
        O->SetArrayField(TEXT("focusMeters"), Numbers({C.Focus.X,C.Focus.Y,C.Focus.Z}));
        O->SetArrayField(TEXT("quaternionXYZW"), Numbers({C.Orientation.X,C.Orientation.Y,C.Orientation.Z,C.Orientation.W}));
        O->SetNumberField(TEXT("orbitDistanceMeters"), C.OrbitDistance);
        O->SetNumberField(TEXT("fieldOfView"), C.FieldOfView);
        O->SetNumberField(TEXT("orthoWidthMeters"), C.OrthoWidth);
        O->SetBoolField(TEXT("orthographic"), C.bOrthographic);
        O->SetBoolField(TEXT("freeCamera"), C.bFreeCamera);
        O->SetBoolField(TEXT("depthClipping"),C.bDepthClipping);
        O->SetNumberField(TEXT("nearClipMeters"),C.NearClipMeters);
        O->SetNumberField(TEXT("farClipMeters"),C.FarClipMeters);
        return O;
    }
    bool ReadCamera(const FObject& O, FStudioCameraState& C,bool bClippingSettings)
    {
        double P[3], F[3], Q[4];
        if (!ReadArray(O,TEXT("positionMeters"),P,3) || !ReadArray(O,TEXT("focusMeters"),F,3) ||
            !ReadArray(O,TEXT("quaternionXYZW"),Q,4) ||
            !ReadNumber(O,TEXT("orbitDistanceMeters"),C.OrbitDistance,.0001,1.e8) ||
            !ReadNumber(O,TEXT("fieldOfView"),C.FieldOfView,5,160) ||
            !ReadNumber(O,TEXT("orthoWidthMeters"),C.OrthoWidth,.001,1.e6) ||
            !O->TryGetBoolField(TEXT("orthographic"),C.bOrthographic) ||
            !O->TryGetBoolField(TEXT("freeCamera"),C.bFreeCamera)) return false;
        if(bClippingSettings&&(!O->TryGetBoolField(TEXT("depthClipping"),C.bDepthClipping)||
            !O->TryGetNumberField(TEXT("nearClipMeters"),C.NearClipMeters)||
            !O->TryGetNumberField(TEXT("farClipMeters"),C.FarClipMeters)||!StudioView::IsValidClipping(C)))return false;
        C.Position = FVector(P[0],P[1],P[2]); C.Focus = FVector(F[0],F[1],F[2]);
        C.Orientation = FQuat(Q[0],Q[1],Q[2],Q[3]);
        // Reject corrupt rotations instead of silently normalizing arbitrary input.
        if (!FMath::IsNearlyEqual(C.Orientation.SizeSquared(),1.,1.e-5)) return false;
        // Preserve the serialized components; renormalizing a valid UE rotation
        // introduces numeric drift on repeated save/open cycles.
        return true;
    }
    FObject ViewJSON(const FStudioViewSettings& V)
    {
        auto O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("sliceAxis"),V.SliceAxis); O->SetNumberField(TEXT("slicePosition"),V.SlicePosition);
        O->SetNumberField(TEXT("streamlineDensity"),V.StreamlineDensity); O->SetNumberField(TEXT("vectorScale"),V.VectorScale);
        O->SetObjectField(TEXT("streamlineSettings"),StudioStreamlines::ToJSON(V.StreamlineSettings));
        O->SetNumberField(TEXT("vectorCount"),V.VectorCount);O->SetBoolField(TEXT("uniformVectors"),V.bUniformVectors);
        O->SetNumberField(TEXT("volumeOpacity"),V.VolumeOpacity); O->SetNumberField(TEXT("playbackRate"),V.PlaybackRate);
        O->SetBoolField(TEXT("loopPlayback"),V.bLoopPlayback); O->SetBoolField(TEXT("streamlines"),V.bStreamlines);
        O->SetBoolField(TEXT("vectors"),V.bVectors); O->SetBoolField(TEXT("cutPlane"),V.bCutPlane);
        O->SetBoolField(TEXT("volume"),V.bVolume); O->SetBoolField(TEXT("mesh"),V.bMesh);
        O->SetNumberField(TEXT("meshStyle"),V.MeshStyle);
        O->SetStringField(TEXT("scalarField"),V.ScalarField); O->SetBoolField(TEXT("sourcePoints"),V.bSourcePoints);
        O->SetNumberField(TEXT("pointSize"),V.PointSize);
        O->SetBoolField(TEXT("reconstructedSurface"),V.bReconstructedSurface);
        O->SetArrayField(TEXT("volumeClipMinimum"),Numbers({V.VolumeClipMinimum.X,V.VolumeClipMinimum.Y,V.VolumeClipMinimum.Z}));
        O->SetArrayField(TEXT("volumeClipMaximum"),Numbers({V.VolumeClipMaximum.X,V.VolumeClipMaximum.Y,V.VolumeClipMaximum.Z}));
        O->SetArrayField(TEXT("volumeOpacityCurve"),Numbers({V.VolumeOpacityCurve.X,V.VolumeOpacityCurve.Y,V.VolumeOpacityCurve.Z}));
        O->SetNumberField(TEXT("volumeStepVoxels"),V.VolumeStepVoxels);
        O->SetBoolField(TEXT("volumeThreshold"),V.bVolumeThreshold);
        O->SetBoolField(TEXT("volumeIsosurface"),V.bVolumeIsosurface);O->SetNumberField(TEXT("volumeIsovalue"),V.VolumeIsovalue);
        O->SetNumberField(TEXT("volumeThresholdMinimum"),V.VolumeThresholdMinimum);
        O->SetNumberField(TEXT("volumeThresholdMaximum"),V.VolumeThresholdMaximum);
        TArray<TSharedPtr<FJsonValue>> Styles;
        for(const auto& S:V.ScalarStyles)
        {
            auto Item=MakeShared<FJsonObject>();Item->SetStringField(TEXT("dataset"),S.Dataset);Item->SetStringField(TEXT("field"),S.Field);
            Item->SetNumberField(TEXT("palette"),S.Palette);Item->SetBoolField(TEXT("manualRange"),S.bManualRange);
            Item->SetNumberField(TEXT("minimum"),S.Minimum);Item->SetNumberField(TEXT("maximum"),S.Maximum);
            Item->SetArrayField(TEXT("lowColor"),Numbers({S.LowColor.R,S.LowColor.G,S.LowColor.B}));
            Item->SetArrayField(TEXT("middleColor"),Numbers({S.MiddleColor.R,S.MiddleColor.G,S.MiddleColor.B}));
            Item->SetArrayField(TEXT("highColor"),Numbers({S.HighColor.R,S.HighColor.G,S.HighColor.B}));
            Styles.Add(MakeShared<FJsonValueObject>(Item));
        }
        O->SetArrayField(TEXT("scalarStyles"),Styles);
        O->SetObjectField(TEXT("inspectionObjects"),StudioInspectionObjects::ToJSON(V.InspectionObjects));
        return O;
    }
    bool ReadView(const FObject& O, FStudioViewSettings& V, bool bLegacy, bool bPointSettings=false,bool bColorSettings=false,bool bSurfaceSettings=false,bool bVolumeSettings=false,bool bInspectionSettings=false,bool bVectorSettings=false,bool bStreamSettings=false)
    {
        if (!ReadInteger(O,TEXT("sliceAxis"),V.SliceAxis,0,2) ||
            !ReadNumber(O,TEXT("slicePosition"),V.SlicePosition,-1.e9,1.e9) ||
            !ReadNumber(O,TEXT("streamlineDensity"),V.StreamlineDensity,0,1) ||
            !ReadNumber(O,TEXT("vectorScale"),V.VectorScale,.2,3) ||
            !ReadNumber(O,TEXT("volumeOpacity"),V.VolumeOpacity,0,1) ||
            !O->TryGetBoolField(TEXT("streamlines"),V.bStreamlines) || !O->TryGetBoolField(TEXT("vectors"),V.bVectors) ||
            !O->TryGetBoolField(TEXT("cutPlane"),V.bCutPlane) || !O->TryGetBoolField(TEXT("volume"),V.bVolume) ||
            !O->TryGetBoolField(TEXT("mesh"),V.bMesh)) return false;
        if(bVectorSettings&&(!ReadInteger(O,TEXT("vectorCount"),V.VectorCount,1,4096)||
            !O->TryGetBoolField(TEXT("uniformVectors"),V.bUniformVectors)))return false;
        if(bStreamSettings)
        {
            const FObject* Settings=nullptr;
            if(!O->TryGetObjectField(TEXT("streamlineSettings"),Settings)||!StudioStreamlines::FromJSON(*Settings,V.StreamlineSettings))return false;
        }
        if ((!bLegacy || O->HasField(TEXT("playbackRate"))) && !ReadNumber(O,TEXT("playbackRate"),V.PlaybackRate,.25,4)) return false;
        if ((!bLegacy || O->HasField(TEXT("loopPlayback"))) && !O->TryGetBoolField(TEXT("loopPlayback"),V.bLoopPlayback)) return false;
        if(bPointSettings)
        {
            if(!O->TryGetStringField(TEXT("scalarField"),V.ScalarField)||V.ScalarField.Len()>128||
                !O->TryGetBoolField(TEXT("sourcePoints"),V.bSourcePoints)||!ReadNumber(O,TEXT("pointSize"),V.PointSize,.25,3))return false;
            for(TCHAR C:V.ScalarField)if(!FChar::IsAlnum(C)&&C!='_'&&C!='-'&&C!='.')return false;
        }
        if(bSurfaceSettings&&!O->TryGetBoolField(TEXT("reconstructedSurface"),V.bReconstructedSurface))return false;
        if(bInspectionSettings)
        {
            const FObject* Objects=nullptr;FString Error;
            if(!O->TryGetObjectField(TEXT("inspectionObjects"),Objects)||!StudioInspectionObjects::FromJSON(*Objects,V.InspectionObjects,Error))return false;
            int32 ObjectsVersion;
            if(bStreamSettings&&!ReadInteger(*Objects,TEXT("version"),ObjectsVersion,2,StudioInspectionObjects::CurrentVersion))return false;
        }
        if(bVolumeSettings)
        {
            double Lo[3],Hi[3],Curve[3];
            if(!ReadArray(O,TEXT("volumeClipMinimum"),Lo,3)||!ReadArray(O,TEXT("volumeClipMaximum"),Hi,3)||
                !ReadArray(O,TEXT("volumeOpacityCurve"),Curve,3)||!ReadNumber(O,TEXT("volumeStepVoxels"),V.VolumeStepVoxels,.25,4)||
                !O->TryGetBoolField(TEXT("volumeThreshold"),V.bVolumeThreshold)||
                !O->TryGetBoolField(TEXT("volumeIsosurface"),V.bVolumeIsosurface)||!ReadNumber(O,TEXT("volumeIsovalue"),V.VolumeIsovalue,-1.e20,1.e20)||
                !ReadNumber(O,TEXT("volumeThresholdMinimum"),V.VolumeThresholdMinimum,-1.e20,1.e20)||
                !ReadNumber(O,TEXT("volumeThresholdMaximum"),V.VolumeThresholdMaximum,-1.e20,1.e20)||V.VolumeThresholdMinimum>V.VolumeThresholdMaximum)return false;
            for(int32 A=0;A<3;++A)if(Lo[A]<0||Hi[A]>1||Lo[A]>=Hi[A]||Curve[A]<0||Curve[A]>1)return false;
            V.VolumeClipMinimum=FVector(Lo[0],Lo[1],Lo[2]);V.VolumeClipMaximum=FVector(Hi[0],Hi[1],Hi[2]);
            V.VolumeOpacityCurve=FVector(Curve[0],Curve[1],Curve[2]);
        }
        if(bColorSettings)
        {
            const TArray<TSharedPtr<FJsonValue>>* Styles;
            if(!O->TryGetArrayField(TEXT("scalarStyles"),Styles)||Styles->Num()>128)return false;
            for(const auto& Item:*Styles)
            {
                const FObject* Obj;FStudioScalarStyle S;
                if(!Item->TryGetObject(Obj)||!(*Obj)->TryGetStringField(TEXT("dataset"),S.Dataset)||
                    !(*Obj)->TryGetStringField(TEXT("field"),S.Field)||!ReadInteger(*Obj,TEXT("palette"),S.Palette,0,bVolumeSettings?3:2)||
                    !(*Obj)->TryGetBoolField(TEXT("manualRange"),S.bManualRange)||
                    !(*Obj)->TryGetNumberField(TEXT("minimum"),S.Minimum)||!(*Obj)->TryGetNumberField(TEXT("maximum"),S.Maximum))return false;
                if(bVolumeSettings)
                {
                    double Low[3],Middle[3],High[3];
                    if(!ReadArray(*Obj,TEXT("lowColor"),Low,3)||!ReadArray(*Obj,TEXT("middleColor"),Middle,3)||!ReadArray(*Obj,TEXT("highColor"),High,3))return false;
                    for(int32 I=0;I<3;++I)if(Low[I]<0||Low[I]>1||Middle[I]<0||Middle[I]>1||High[I]<0||High[I]>1)return false;
                    S.LowColor=FLinearColor(Low[0],Low[1],Low[2]);S.MiddleColor=FLinearColor(Middle[0],Middle[1],Middle[2]);S.HighColor=FLinearColor(High[0],High[1],High[2]);
                }
                V.ScalarStyles.Add(MoveTemp(S));
            }
            if(!StudioColor::IsValid(V.ScalarStyles))return false;
        }
        return true;
    }
    bool ReadId(const FObject& O, const TCHAR* Key, FGuid& Id)
    {
        FString S; return O->TryGetStringField(Key,S) && FGuid::Parse(S,Id) && Id.IsValid();
    }
    bool ReadName(const FObject& O, FString& Name)
    {
        return O->TryGetStringField(TEXT("name"),Name) && !Name.TrimStartAndEnd().IsEmpty() && Name.Len()<=120;
    }
}

TSharedRef<FJsonObject> StudioProjectIO::ViewToJSON(const FStudioViewSettings& View)
{return ViewJSON(View).ToSharedRef();}

FString StudioProjectIO::Serialize(const FStudioProject& P)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("format"),TEXT("LBMStudioProject")); O->SetNumberField(TEXT("version"),FStudioProject::CurrentVersion);
    O->SetStringField(TEXT("id"),P.Id.ToString()); O->SetStringField(TEXT("name"),P.Name);
    O->SetStringField(TEXT("dataset"),P.Dataset); O->SetBoolField(TEXT("favorite"),P.bFavorite);
    TArray<TSharedPtr<FJsonValue>> Recordings;
    for(const auto& R:P.Recordings)
    {
        auto Item=MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"),R.Id); Item->SetStringField(TEXT("title"),R.Title);
        Item->SetStringField(TEXT("path"),R.Path); Item->SetStringField(TEXT("metadataSHA256"),R.MetadataSHA256);
        Item->SetStringField(TEXT("format"),R.Format);
        // Retain an invalid extra hash so validation rejects it instead of
        // silently repairing an inconsistent in-memory point reference.
        if(R.Format==TEXT("flow_v2")||!R.PayloadSHA256.IsEmpty())Item->SetStringField(TEXT("payloadSHA256"),R.PayloadSHA256);
        if(R.Reconstruction.IsSet())
        {
            auto Surface=MakeShared<FJsonObject>();Surface->SetStringField(TEXT("path"),R.Reconstruction->Path);
            Surface->SetStringField(TEXT("metadataSHA256"),R.Reconstruction->MetadataSHA256);Item->SetObjectField(TEXT("reconstruction"),Surface);
        }
        else Item->SetField(TEXT("reconstruction"),MakeShared<FJsonValueNull>());
        Recordings.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("recordings"),Recordings);
    O->SetNumberField(TEXT("selectedFrame"),P.SelectedFrame);
    O->SetObjectField(TEXT("view"),ViewJSON(P.View)); O->SetObjectField(TEXT("camera"),CameraJSON(P.Camera));
    TArray<TSharedPtr<FJsonValue>> Bookmarks;
    for (const auto& B : P.Cameras)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"),B.Id.ToString()); Item->SetStringField(TEXT("name"),B.Name);
        Item->SetObjectField(TEXT("camera"),CameraJSON(B.Camera)); Bookmarks.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("cameras"),Bookmarks);
    O->SetObjectField(TEXT("draft"),StudioCaseIO::ToJSON(P.Draft));
    TArray<TSharedPtr<FJsonValue>> Runs;
    for (const auto& Run : P.Runs) Runs.Add(MakeShared<FJsonValueObject>(Run.ToJSON()));
    O->SetArrayField(TEXT("runs"),Runs);
    O->SetBoolField(TEXT("controlHarness"),P.bControlHarness);
    TArray<TSharedPtr<FJsonValue>> Jobs;
    for(const auto& H:P.JobHistory)
    {
        auto Item=MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("runId"),H.RunId.ToString());
        Item->SetStringField(TEXT("lastState"),StudioJobs::StateName(H.LastState));
        Item->SetNumberField(TEXT("elapsedSeconds"),H.ElapsedSeconds);
        Item->SetNumberField(TEXT("stepCommands"),double(H.StepCommands));
        Item->SetNumberField(TEXT("checkpointCommands"),double(H.CheckpointCommands));
        Item->SetStringField(TEXT("notice"),H.Notice);
        Jobs.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("jobHistory"),Jobs);
    if (!P.RecoverySource.IsEmpty()) O->SetStringField(TEXT("recoverySource"),P.RecoverySource);
    FString Text; FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text)); return Text;
}

bool StudioProjectIO::Parse(const FString& Text, FStudioProject& Out, FString& Error)
{
    Error=TEXT("Invalid project document. The current project has been kept.");
    FObject O;
    if (Text.Len()>4*1024*1024 || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O) || !O) return false;
    int32 Version;
    if (!ReadInteger(O,TEXT("version"),Version,2,FStudioProject::CurrentVersion))
    { Error=TEXT("Unsupported project version. Open this file with a compatible LBM Studio build."); return false; }
    FStudioProject P;
    if (Version==2)
    {
        if (!O->TryGetStringField(TEXT("sample"),P.Dataset) || !ReadView(O,P.View,true)) return false;
    }
    else
    {
        FString Format;
        const FObject *View, *Camera;
        const TArray<TSharedPtr<FJsonValue>>* Bookmarks;
        if (!O->TryGetStringField(TEXT("format"),Format) || Format!=TEXT("LBMStudioProject") ||
            !ReadId(O,TEXT("id"),P.Id) || !ReadName(O,P.Name) ||
            !O->TryGetStringField(TEXT("dataset"),P.Dataset) || !O->TryGetBoolField(TEXT("favorite"),P.bFavorite) ||
            !ReadInteger(O,TEXT("selectedFrame"),P.SelectedFrame,0,MAX_int32) ||
            !O->TryGetObjectField(TEXT("view"),View) || !ReadView(*View,P.View,false,Version>=7,Version>=8,Version>=10,Version>=12,Version>=13,Version>=14,Version>=15) ||
            (Version>=16&&!ReadInteger(*View,TEXT("meshStyle"),P.View.MeshStyle,0,2)) ||
            !O->TryGetObjectField(TEXT("camera"),Camera) || !ReadCamera(*Camera,P.Camera,Version>=11) ||
            !O->TryGetArrayField(TEXT("cameras"),Bookmarks) || Bookmarks->Num()>128) return false;
        TSet<FGuid> Ids; TSet<FString> Names;
        for (const auto& Value : *Bookmarks)
        {
            const FObject* Item; const FObject* C; FStudioCameraBookmark B;
            if (!Value->TryGetObject(Item) || !ReadId(*Item,TEXT("id"),B.Id) || !ReadName(*Item,B.Name) ||
                !(*Item)->TryGetObjectField(TEXT("camera"),C) || !ReadCamera(*C,B.Camera,Version>=11) ||
                Ids.Contains(B.Id) || Names.Contains(B.Name.ToLower())) return false;
            Ids.Add(B.Id); Names.Add(B.Name.ToLower()); P.Cameras.Add(B);
        }
        O->TryGetStringField(TEXT("recoverySource"),P.RecoverySource);
        if (Version>=4)
        {
            const FObject* Draft;
            const TArray<TSharedPtr<FJsonValue>>* Runs;
            if (!O->TryGetObjectField(TEXT("draft"),Draft)) return false;
            auto HasAuthoringFields=[](const FObject& Case)
            {
                const FObject* Domain;const TArray<TSharedPtr<FJsonValue>>* Boundaries;
                if(!Case->TryGetObjectField(TEXT("domain"),Domain)||!(*Domain)->HasField(TEXT("faceNames"))||!Case->TryGetArrayField(TEXT("boundaries"),Boundaries))return false;
                for(const auto& Value:*Boundaries){const FObject* Boundary;if(!Value->TryGetObject(Boundary)||!(*Boundary)->HasField(TEXT("pairedTargetId")))return false;}
                return true;
            };
            if(Version>=17&&!HasAuthoringFields(*Draft)){Error=TEXT("Domain face names or boundary pairing fields are missing from the project.");return false;}
            if(!StudioCaseIO::FromJSON(*Draft,P.Draft,Error)) return false;
            if (!O->TryGetArrayField(TEXT("runs"),Runs) || Runs->Num()>256)
            { Error=TEXT("Invalid or oversized run record list."); return false; }
            P.Runs.Reset(); TSet<FGuid> RunIds;
            for (const auto& Value : *Runs)
            {
                const FObject* Item; FStudioRunRecord Run;
                if (!Value->TryGetObject(Item)) { Error=TEXT("Invalid run record."); return false; }
                if(Version>=17)
                {
                    const FObject* Configuration;
                    if((*Item)->TryGetObjectField(TEXT("configuration"),Configuration)&&!HasAuthoringFields(*Configuration))
                    {Error=TEXT("The frozen configuration is missing domain names or boundary pairing fields.");return false;}
                }
                if (!FStudioRunRecord::FromJSON(*Item,Run,Error)) return false;
                if (RunIds.Contains(Run.GetId())) { Error=TEXT("Duplicate run identity."); return false; }
                RunIds.Add(Run.GetId()); P.Runs.Add(MoveTemp(Run));
            }
        }
    }
    if(Version>=5)
    {
        const TArray<TSharedPtr<FJsonValue>>* Jobs;
        if(!O->TryGetBoolField(TEXT("controlHarness"),P.bControlHarness)||
            !O->TryGetArrayField(TEXT("jobHistory"),Jobs)||Jobs->Num()>256)
        {Error=TEXT("Invalid job history or control mode.");return false;}
        TSet<FGuid> Seen;
        for(const auto& Value:*Jobs)
        {
            const FObject* Item; FStudioJobHistory H; FString State;double Steps,Checkpoints;
            if(!Value->TryGetObject(Item)||!ReadId(*Item,TEXT("runId"),H.RunId)||Seen.Contains(H.RunId)||
                !(*Item)->TryGetStringField(TEXT("lastState"),State)||!StudioJobs::ParseState(State,H.LastState)||H.LastState==EStudioJobState::Idle||
                !ReadNumber(*Item,TEXT("elapsedSeconds"),H.ElapsedSeconds,0,1.e12)||
                !ReadNumber(*Item,TEXT("stepCommands"),Steps,0,9007199254740991.)||Steps!=FMath::FloorToDouble(Steps)||
                !ReadNumber(*Item,TEXT("checkpointCommands"),Checkpoints,0,9007199254740991.)||Checkpoints!=FMath::FloorToDouble(Checkpoints)||
                !(*Item)->TryGetStringField(TEXT("notice"),H.Notice)||H.Notice.Len()>2048)
            {Error=TEXT("Invalid job history entry.");return false;}
            const auto* Run=P.Runs.FindByPredicate([&](const auto& R){return R.GetId()==H.RunId;});
            if(!Run||!Run->GetConfiguration())
            {Error=TEXT("Job history must refer to a submitted configuration.");return false;}
            H.StepCommands=uint64(Steps);H.CheckpointCommands=uint64(Checkpoints);
            Seen.Add(H.RunId);P.JobHistory.Add(MoveTemp(H));
        }
    }
    if(Version>=6)
    {
        const TArray<TSharedPtr<FJsonValue>>* Recordings;
        if(!O->TryGetArrayField(TEXT("recordings"),Recordings)||Recordings->Num()>128)
        {Error=TEXT("Invalid external recording list.");return false;}
        TSet<FString> Seen;
        for(const auto& Value:*Recordings)
        {
            const FObject* Item; FStudioRecordingReference R;
            if(!Value->TryGetObject(Item)||!(*Item)->TryGetStringField(TEXT("id"),R.Id)||
                !(*Item)->TryGetStringField(TEXT("title"),R.Title)||!(*Item)->TryGetStringField(TEXT("path"),R.Path)||
                !(*Item)->TryGetStringField(TEXT("metadataSHA256"),R.MetadataSHA256)||
                (Version>=7&&!(*Item)->TryGetStringField(TEXT("format"),R.Format))||
                (R.Format==TEXT("flow_v2")&&!(*Item)->TryGetStringField(TEXT("payloadSHA256"),R.PayloadSHA256))||
                (R.Format==TEXT("point_v3")&&(*Item)->HasField(TEXT("payloadSHA256"))))
            {Error=TEXT("Invalid or duplicate external recording reference.");return false;}
            if(Version>=9)
            {
                const auto Surface=(*Item)->TryGetField(TEXT("reconstruction"));
                if(!Surface.IsValid()){Error=TEXT("Missing recording reconstruction state.");return false;}
                if(!Surface->IsNull())
                {
                    const FObject* Object;FStudioReconstructionReference Ref;
                    if(!Surface->TryGetObject(Object)||!(*Object)->TryGetStringField(TEXT("path"),Ref.Path)||
                        !(*Object)->TryGetStringField(TEXT("metadataSHA256"),Ref.MetadataSHA256))
                    {Error=TEXT("Invalid reconstruction reference.");return false;}
                    Ref.MetadataSHA256.ToLowerInline();R.Reconstruction=MoveTemp(Ref);
                }
            }
            if(!StudioRecordings::IsValidReference(R)||Seen.Contains(R.Id))
            {Error=TEXT("Invalid or duplicate external recording reference.");return false;}
            R.MetadataSHA256=R.MetadataSHA256.ToLower(); R.PayloadSHA256=R.PayloadSHA256.ToLower();
            Seen.Add(R.Id); P.Recordings.Add(MoveTemp(R));
        }
    }
    if (P.Dataset.IsEmpty() || P.Dataset.Len()>256)
    { Error=TEXT("Project recording identity is missing or too long."); return false; }
    Out=MoveTemp(P); Error.Empty(); return true;
}

bool StudioProjectIO::Load(const FString& Path, FStudioProject& Out, FString& Error)
{
    FStudioFileAccess Access(Path);
    auto& Platform = FPlatformFileManager::Get().GetPlatformFile();
    const int64 Size=Platform.FileSize(*Path);
    FString Text;
    if (Size<0 || Size>4*1024*1024 || !FFileHelper::LoadFileToString(Text,*Path))
    { Error=TEXT("Could not read project file (missing, inaccessible, or larger than 4 MB)."); return false; }
    FStudioProject Candidate;
    if (!Parse(Text,Candidate,Error)) return false;
    // Older recovery files kept the original project's relative references.
    // New recovery files contain resolved paths and work with either base.
    const FString Owner=Candidate.RecoverySource.IsEmpty()?FPaths::ConvertRelativePathToFull(Path):Candidate.RecoverySource;
    if (!StudioAssetPaths::Resolve(Candidate,FPaths::GetPath(Owner),Error)) return false;
    Out=MoveTemp(Candidate); return true;
}

bool StudioProjectIO::WriteAtomic(const FString& Path, const FString& Text, FString& Error)
{
    FStudioFileAccess Access(Path);
    auto& Platform = FPlatformFileManager::Get().GetPlatformFile();
    const FString Full=FPaths::ConvertRelativePathToFull(Path);
    if (!Platform.CreateDirectoryTree(*FPaths::GetPath(Full)))
    { Error=TEXT("Could not create the project directory."); return false; }
#if PLATFORM_MAC
    return StudioFileDialog::WriteAtomic(Full,Text,Error);
#else
    const FString Temp=Full+TEXT(".")+FGuid::NewGuid().ToString()+TEXT(".tmp");
    const FTCHARToUTF8 Bytes(*Text);
    TUniquePtr<IFileHandle> Handle(Platform.OpenWrite(*Temp));
    bool bOK=Handle && Handle->Write(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length()) && Handle->Flush(true);
    Handle.Reset();
    if (bOK) bOK=Platform.MoveFile(*Full,*Temp);
    if (!bOK) { Platform.DeleteFile(*Temp); Error=TEXT("Save failed. Check free disk space and folder permissions. The previous project file was kept."); }
    else Error.Empty();
    return bOK;
#endif
}

FString StudioProjectIO::BackupPath(const FString& Path)
{
    return FPaths::ProjectSavedDir()/TEXT("ProjectBackups")/(FMD5::HashAnsiString(*FPaths::ConvertRelativePathToFull(Path))+TEXT(".lbms"));
}

bool StudioProjectIO::Save(const FString& Path, const FStudioProject& P, FString& Error)
{
    FStudioFileAccess Access(Path);
    if(FPlatformFileManager::Get().GetPlatformFile().DirectoryExists(*Path))
    { Error=TEXT("Cannot save a project to a directory."); return false; }
    FStudioProject Stored;
    if (!StudioAssetPaths::ForStorage(P,Path,Stored,Error)) return false;
    const FString Text=Serialize(Stored);
    FStudioProject Check;
    if (!Parse(Text,Check,Error)) return false;
    // Preserve a readable previous version before replacing the document.
    FString Previous;
    if (FFileHelper::LoadFileToString(Previous,*Path))
    {
        FStudioProject Prior; FString ParseError;
        if (Parse(Previous,Prior,ParseError))
        {
            const FString Owner=Prior.RecoverySource.IsEmpty()?FPaths::ConvertRelativePathToFull(Path):Prior.RecoverySource;
            if (!StudioAssetPaths::Resolve(Prior,FPaths::GetPath(Owner),Error)) return false;
            Previous=Serialize(Prior); // Backup lives elsewhere; retain absolute asset locations.
        }
        if (!WriteAtomic(BackupPath(Path),Previous,Error)) return false;
    }
    if(!WriteAtomic(Path,Text,Error)) return false;
    StudioFileDialog::RememberAccess(Path); return true;
}
