#include "StudioHome4SpatialDiagnostics.h"
#include "StudioHome4JSON.h"
#include "StudioModel.h"
#include "StudioFileDialog.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"
#include <memory>
#define UI UI_HOME4_SPATIAL_DIAGNOSTICS
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioHome4SpatialPrivate
{
    constexpr int32 MaxBytes=8*1024*1024;
    constexpr double MaxInteger=9007199254740991.;
    struct FReader
    {
        bool Valid=true;FGuid Run;
        bool Keys(const TSharedPtr<FJsonObject>& O,std::initializer_list<const TCHAR*> Allowed)
        {
            if(!O){Valid=false;return false;}
            for(const auto& E:O->Values)
            {
                bool Known=E.Key==TEXT("run_id");for(const auto* K:Allowed)Known|=E.Key==K;
                if(!Known){Valid=false;return false;}
            }
            const auto V=O->TryGetField(TEXT("run_id"));
            if(V)
            {
                FGuid Id;if(V->Type!=EJson::String||!FGuid::Parse(V->AsString(),Id)||!Id.IsValid()||(Run.IsValid()&&Id!=Run)){Valid=false;return false;}
            }
            return true;
        }
        TSharedPtr<FJsonValue> Field(const TSharedPtr<FJsonObject>& O,const TCHAR* K){return O?O->TryGetField(K):nullptr;}
        void Text(const TSharedPtr<FJsonObject>& O,const TCHAR* K,FString& Out,bool Required=false,int32 Max=256)
        {
            const auto V=Field(O,K);if(!V||V->Type==EJson::Null){if(Required)Valid=false;return;}
            if(V->Type!=EJson::String){Valid=false;return;}Out=V->AsString();
            if(Out.TrimStartAndEnd().IsEmpty()||Out.Len()>Max)Valid=false;
            for(TCHAR C:Out)if(C<32||C==127)Valid=false;
        }
        void Number(const TSharedPtr<FJsonObject>& O,const TCHAR* K,TOptional<double>& Out,double Min=-1e12,double Max=1e12)
        {
            const auto V=Field(O,K);if(!V||V->Type==EJson::Null)return;double N=0;
            if(V->Type!=EJson::Number||!V->TryGetNumber(N)||!FMath::IsFinite(N)||N<Min||N>Max){Valid=false;return;}Out=N;
        }
        void Integer(const TSharedPtr<FJsonObject>& O,const TCHAR* K,TOptional<int64>& Out,int64 Min=0,int64 Max=int64(MaxInteger))
        {
            TOptional<double> N;Number(O,K,N,double(Min),double(Max));if(N){if(FMath::FloorToDouble(*N)!=*N)Valid=false;else Out=int64(*N);}
        }
        void Flag(const TSharedPtr<FJsonObject>& O,const TCHAR* K,TOptional<bool>& Out)
        {const auto V=Field(O,K);if(!V||V->Type==EJson::Null)return;if(V->Type!=EJson::Boolean)Valid=false;else Out=V->AsBool();}
        TSharedPtr<FJsonObject> Object(const TSharedPtr<FJsonObject>& O,const TCHAR* K)
        {const auto V=Field(O,K);if(!V||V->Type==EJson::Null)return {};if(V->Type!=EJson::Object){Valid=false;return {};}return V->AsObject();}
        const TArray<TSharedPtr<FJsonValue>>* Array(const TSharedPtr<FJsonObject>& O,const TCHAR* K,int32 Max)
        {const auto V=Field(O,K);if(!V||V->Type==EJson::Null)return nullptr;if(V->Type!=EJson::Array||V->AsArray().Num()>Max){Valid=false;return nullptr;}return &V->AsArray();}
        void Vector(const TSharedPtr<FJsonObject>& O,const TCHAR* K,TOptional<FVector>& Out)
        {
            const auto* A=Array(O,K,3);if(!A)return;if(A->Num()!=3){Valid=false;return;}FVector V;
            for(int32 I=0;I<3;++I){double N=0;if((*A)[I]->Type!=EJson::Number||!(*A)[I]->TryGetNumber(N)||!FMath::IsFinite(N)||FMath::Abs(N)>1e12){Valid=false;return;}V[I]=N;}Out=V;
        }
        void Quantity(const TSharedPtr<FJsonObject>& O,const TCHAR* K,FStudioHome4SpatialValue& Out)
        {
            const auto Q=Object(O,K);if(!Q)return;Keys(Q,{TEXT("value"),TEXT("unit")});Number(Q,TEXT("value"),Out.Value);Text(Q,TEXT("unit"),Out.Unit,Out.Value.IsSet(),96);
        }
        int32 Level(const TSharedPtr<FJsonObject>& O)
        {TOptional<int64> L;Integer(O,TEXT("level"),L,0,63);if(!L)Valid=false;return int32(L.Get(0));}
        void Work(const TSharedPtr<FJsonObject>& O,FStudioHome4Work& W)
        {
            if(!O)return;Keys(O,{TEXT("elapsed_seconds"),TEXT("node_updates"),TEXT("transferred_bytes"),TEXT("cumulative_elapsed_seconds"),TEXT("cumulative_node_updates")});
            Number(O,TEXT("elapsed_seconds"),W.ElapsedSeconds,0);Number(O,TEXT("cumulative_elapsed_seconds"),W.CumulativeElapsedSeconds,0);
            TOptional<int64> V;Integer(O,TEXT("node_updates"),V);if(V)W.NodeUpdates=double(*V);V.Reset();Integer(O,TEXT("transferred_bytes"),V);if(V)W.TransferredBytes=double(*V);
            V.Reset();Integer(O,TEXT("cumulative_node_updates"),V);if(V)W.CumulativeNodeUpdates=double(*V);
        }
        bool Range(const TOptional<FVector>& Min,const TOptional<FVector>& Max)
        {if(Min.IsSet()!=Max.IsSet()){Valid=false;return false;}if(Min&&Max&&(Min->X>Max->X||Min->Y>Max->Y||Min->Z>Max->Z)){Valid=false;return false;}return true;}
    };
    bool Hash(const TArray<uint8>& Bytes,FString& Out)
    {
        std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> C(EVP_MD_CTX_new(),&EVP_MD_CTX_free);uint8 D[32];unsigned int N=0;
        if(!C||EVP_DigestInit_ex(C.get(),EVP_sha256(),nullptr)!=1||EVP_DigestUpdate(C.get(),Bytes.GetData(),Bytes.Num())!=1||EVP_DigestFinal_ex(C.get(),D,&N)!=1||N!=32)return false;
        Out=BytesToHex(D,32).ToLower();return true;
    }
    bool LoadFixed(const FString& Path,const TOptional<FGuid>& Expected,FStudioHome4SpatialEvidence& Out,FString& Error,const TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe>& Cancel,const TFunction<void()>& BeforeVerify)
    {
        auto Cancelled=[&]{return Cancel&&Cancel->load(std::memory_order_relaxed);};
        auto Fail=[&](const TCHAR* Why){Error=Why;return false;};if(Cancelled())return Fail(TEXT("Spatial import cancelled."));
        FStudioFileAccess Access(Path);const auto Time=IFileManager::Get().GetTimeStamp(*Path);const auto Size=IFileManager::Get().FileSize(*Path);
        if(Size<1||Size>MaxBytes)return Fail(TEXT("Original spatial diagnostics must contain 1 byte to 8 MiB."));
        auto Read=[&](TArray<uint8>& Bytes)
        {
            TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));if(!File||File->TotalSize()!=Size)return false;
            Bytes.SetNumUninitialized(int32(Size));for(int64 At=0;At<Size;)
            {if(Cancelled())return false;const int32 N=int32(FMath::Min<int64>(65536,Size-At));File->Serialize(Bytes.GetData()+At,N);if(File->IsError())return false;At+=N;}
            return File->TotalSize()==Size;
        };
        TArray<uint8> Bytes;if(!Read(Bytes))return Fail(TEXT("Spatial source missing, changed or import cancelled."));
        if(!StudioHome4JSON::UTF8(Bytes.GetData(),Bytes.Num()))return Fail(TEXT("Original spatial diagnostics must be valid UTF-8."));
        const int32 Offset=Bytes.Num()>=3&&Bytes[0]==0xef&&Bytes[1]==0xbb&&Bytes[2]==0xbf?3:0;
        const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+Offset),Bytes.Num()-Offset);FStudioHome4SpatialEvidence Candidate;
        if(!StudioHome4SpatialDiagnostics::Parse(FString(Text.Length(),Text.Get()),Expected,Candidate,Error))return false;
        if(BeforeVerify)BeforeVerify();
        TArray<uint8> Verify;
        if(!Read(Verify)||Bytes!=Verify||IFileManager::Get().GetTimeStamp(*Path)!=Time||IFileManager::Get().FileSize(*Path)!=Size)return Fail(TEXT("Original spatial diagnostics changed during import or the import was cancelled."));
        if(Cancelled())return Fail(TEXT("Spatial import cancelled."));
        if(!Hash(Bytes,Candidate.SourceSHA256))return Fail(TEXT("Original spatial source identity could not be verified."));
        Candidate.SourcePath=FPaths::ConvertRelativePathToFull(Path);Candidate.OriginalBytes=MoveTemp(Bytes);Out=MoveTemp(Candidate);Error.Empty();return true;
    }
}

TOptional<FBox> FStudioHome4SpatialPatch::Bounds() const
{
    if(!Origin||!Spacing||!Extents)return {};
    const FVector End=*Origin+*Spacing*FVector(Extents->X-1,Extents->Y-1,Extents->Z-1);
    return End.ContainsNaN()?TOptional<FBox>():TOptional<FBox>(FBox(*Origin,End));
}
bool StudioHome4SpatialDiagnostics::Parse(const FString& JSON,const TOptional<FGuid>& Expected,FStudioHome4SpatialEvidence& Out,FString& Error)
{
    using namespace StudioHome4SpatialPrivate;Error=TEXT("Invalid original spatial diagnostics; previous evidence retained.");
    if(JSON.Len()>MaxBytes||!StudioHome4JSON::Preflight(JSON))return false;
    const FTCHARToUTF8 Bytes(*JSON);
    if(Bytes.Length()<1||Bytes.Length()>MaxBytes)return false;
    const auto Tokens=TJsonReaderFactory<>::Create(JSON);EJsonNotation Token;int32 TokenCount=0;
    while(Tokens->ReadNext(Token))if(++TokenCount>100000)return false;
    TSharedPtr<FJsonObject> O;if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON),O)||!O)return false;
    FReader R;R.Keys(O,{TEXT("schema"),TEXT("version"),TEXT("axis_order"),TEXT("coordinate_unit"),TEXT("source_id"),TEXT("patches"),TEXT("levels"),TEXT("zones"),TEXT("geometry")});
    FString Schema,Run,Axes;R.Text(O,TEXT("schema"),Schema,true);R.Text(O,TEXT("run_id"),Run,true);R.Text(O,TEXT("axis_order"),Axes,true);
    TOptional<int64> Version;R.Integer(O,TEXT("version"),Version,1,1);FStudioHome4SpatialEvidence E;
    if(Schema!=TEXT("LBMStudio.Home4SpatialDiagnostics")||Version.Get(0)!=1||Axes!=TEXT("XYZ")||!FGuid::Parse(Run,E.RunId)||!E.RunId.IsValid()||(Expected&&*Expected!=E.RunId))return false;
    R.Run=E.RunId;R.Text(O,TEXT("coordinate_unit"),E.CoordinateUnit,true,96);R.Text(O,TEXT("source_id"),E.SourceId,true);
    TSet<FString> PatchIds;
    if(const auto* Patches=R.Array(O,TEXT("patches"),64))for(const auto& V:*Patches)
    {
        if(V->Type!=EJson::Object)return false;const auto P=V->AsObject();R.Keys(P,{TEXT("id"),TEXT("level"),TEXT("origin"),TEXT("spacing"),TEXT("extents"),TEXT("cell_count"),TEXT("masks")});FStudioHome4SpatialPatch A;
        R.Text(P,TEXT("id"),A.Id,true,64);A.Level=R.Level(P);if(PatchIds.Contains(A.Id))return false;PatchIds.Add(A.Id);
        R.Vector(P,TEXT("origin"),A.Origin);R.Vector(P,TEXT("spacing"),A.Spacing);if(A.Spacing&&A.Spacing->GetMin()<=0)return false;
        TOptional<FVector> Extents;R.Vector(P,TEXT("extents"),Extents);if(Extents)
        {for(int32 I=0;I<3;++I)if((*Extents)[I]<1||(*Extents)[I]>1048576||FMath::FloorToDouble((*Extents)[I])!=(*Extents)[I])return false;A.Extents=FIntVector(int32(Extents->X),int32(Extents->Y),int32(Extents->Z));}
        R.Integer(P,TEXT("cell_count"),A.CellCount);if(A.Extents&&A.CellCount&&uint64(*A.CellCount)>uint64(A.Extents->X)*uint64(A.Extents->Y)*uint64(A.Extents->Z))return false;
        TSet<FString> Masks;
        if(const auto* M=R.Array(P,TEXT("masks"),8))for(const auto& MV:*M)
        {
            if(MV->Type!=EJson::Object)return false;const auto Mask=MV->AsObject();R.Keys(Mask,{TEXT("kind"),TEXT("field_id"),TEXT("count")});FStudioHome4SpatialMask Item;
            R.Text(Mask,TEXT("kind"),Item.Kind,true,32);R.Text(Mask,TEXT("field_id"),Item.FieldId,false,64);R.Integer(Mask,TEXT("count"),Item.Count);
            if(!TArray<FString>{TEXT("active"),TEXT("band"),TEXT("ghost"),TEXT("shell"),TEXT("solid")}.Contains(Item.Kind)||Masks.Contains(Item.Kind)||(A.CellCount&&Item.Count&&*Item.Count>*A.CellCount))return false;
            Masks.Add(Item.Kind);A.Masks.Add(MoveTemp(Item));
        }
        E.Patches.Add(MoveTemp(A));
    }
    TSet<int32> LevelIds;
    if(const auto* Levels=R.Array(O,TEXT("levels"),64))for(const auto& V:*Levels)
    {
        if(V->Type!=EJson::Object)return false;const auto L=V->AsObject();R.Keys(L,{TEXT("level"),TEXT("nu"),TEXT("sigma"),TEXT("mobility"),TEXT("gravity"),TEXT("tau"),TEXT("sponge_strength"),TEXT("band_depth"),TEXT("overlap"),TEXT("restriction_margin"),TEXT("substeps"),TEXT("performance"),TEXT("tau_floor"),TEXT("root_phase_free"),TEXT("even_wrap"),TEXT("sneq_mode")});
        FStudioHome4SpatialLevel A;A.Level=R.Level(L);if(LevelIds.Contains(A.Level))return false;LevelIds.Add(A.Level);
        R.Quantity(L,TEXT("nu"),A.Nu);R.Quantity(L,TEXT("sigma"),A.Sigma);R.Quantity(L,TEXT("mobility"),A.Mobility);R.Quantity(L,TEXT("gravity"),A.Gravity);R.Quantity(L,TEXT("tau"),A.Tau);R.Quantity(L,TEXT("sponge_strength"),A.SpongeStrength);
        R.Quantity(L,TEXT("band_depth"),A.BandDepth);R.Quantity(L,TEXT("overlap"),A.Overlap);R.Quantity(L,TEXT("restriction_margin"),A.RestrictionMargin);R.Integer(L,TEXT("substeps"),A.Substeps);R.Work(R.Object(L,TEXT("performance")),A.Work);
        R.Flag(L,TEXT("tau_floor"),A.TauFloor);R.Flag(L,TEXT("root_phase_free"),A.RootPhaseFree);R.Flag(L,TEXT("even_wrap"),A.EvenWrap);R.Text(L,TEXT("sneq_mode"),A.SneqMode);
        if(!A.SneqMode.IsEmpty()&&A.SneqMode!=TEXT("derived")&&A.SneqMode!=TEXT("dorschner"))return false;
        E.Levels.Add(MoveTemp(A));
    }
    TSet<FString> ZoneIds;
    if(const auto* Zones=R.Array(O,TEXT("zones"),64))for(const auto& V:*Zones)
    {
        if(V->Type!=EJson::Object)return false;const auto Z=V->AsObject();R.Keys(Z,{TEXT("id"),TEXT("kind"),TEXT("min"),TEXT("max"),TEXT("profile"),TEXT("level_strengths")});FStudioHome4SpatialZone A;
        R.Text(Z,TEXT("id"),A.Id,true,64);R.Text(Z,TEXT("kind"),A.Kind,true,32);if(ZoneIds.Contains(A.Id))return false;ZoneIds.Add(A.Id);
        if(!TArray<FString>{TEXT("sponge"),TEXT("beach"),TEXT("floor"),TEXT("wall"),TEXT("inlet"),TEXT("outlet")}.Contains(A.Kind))return false;
        R.Vector(Z,TEXT("min"),A.Minimum);R.Vector(Z,TEXT("max"),A.Maximum);R.Range(A.Minimum,A.Maximum);
        if(const auto P=R.Object(Z,TEXT("profile")))
        {
            R.Keys(P,{TEXT("axis"),TEXT("coordinate_unit"),TEXT("value_unit"),TEXT("x"),TEXT("values")});R.Text(P,TEXT("axis"),A.ProfileAxis,true,32);R.Text(P,TEXT("coordinate_unit"),A.ProfileCoordinateUnit,true,96);R.Text(P,TEXT("value_unit"),A.ProfileValueUnit,true,96);
            if(A.ProfileAxis!=TEXT("X")&&A.ProfileAxis!=TEXT("Y")&&A.ProfileAxis!=TEXT("Z"))return false;
            const auto* X=R.Array(P,TEXT("x"),256);const auto* Values=R.Array(P,TEXT("values"),256);if(!X||!Values||X->Num()<2||X->Num()!=Values->Num())return false;
            for(int32 I=0;I<X->Num();++I)
            {
                double PX=0,PY=0;if((*X)[I]->Type!=EJson::Number||(*Values)[I]->Type!=EJson::Number||!(*X)[I]->TryGetNumber(PX)||!(*Values)[I]->TryGetNumber(PY)||!FMath::IsFinite(PX)||!FMath::IsFinite(PY)||FMath::Abs(PX)>1e12||FMath::Abs(PY)>1e12||(I&&PX<=A.ProfileX.Last()))return false;
                A.ProfileX.Add(PX);A.ProfileValues.Add(PY);
            }
        }
        if(const auto* Strengths=R.Array(Z,TEXT("level_strengths"),64))for(const auto& SV:*Strengths)
        {
            if(SV->Type!=EJson::Object)return false;const auto S=SV->AsObject();R.Keys(S,{TEXT("level"),TEXT("value"),TEXT("unit")});const int32 L=R.Level(S);if(A.LevelStrengths.Contains(L))return false;
            FStudioHome4SpatialValue Strength;R.Number(S,TEXT("value"),Strength.Value);R.Text(S,TEXT("unit"),Strength.Unit,Strength.Value.IsSet(),96);A.LevelStrengths.Add(L,MoveTemp(Strength));
        }
        E.Zones.Add(MoveTemp(A));
    }
    TSet<FString> Bodies;
    if(const auto* Geometry=R.Array(O,TEXT("geometry"),16))for(const auto& V:*Geometry)
    {
        if(V->Type!=EJson::Object)return false;const auto G=V->AsObject();R.Keys(G,{TEXT("body_id"),TEXT("cad_source"),TEXT("sdf_backend"),TEXT("tessellation_status"),TEXT("cache_status"),TEXT("cut_link_status"),TEXT("flotation_status"),TEXT("tessellation_triangles"),TEXT("cut_links"),TEXT("cut_link_fraction"),TEXT("bounds_min"),TEXT("bounds_max"),TEXT("equilibrium_heave"),TEXT("running_heave"),TEXT("trim"),TEXT("draft"),TEXT("buoyancy"),TEXT("weight"),TEXT("reference_source")});FStudioHome4SpatialGeometry A;
        R.Text(G,TEXT("body_id"),A.BodyId,true,64);if(Bodies.Contains(A.BodyId))return false;Bodies.Add(A.BodyId);
        R.Text(G,TEXT("cad_source"),A.CADSource);R.Text(G,TEXT("sdf_backend"),A.SDFBackend);R.Text(G,TEXT("tessellation_status"),A.TessellationStatus);R.Text(G,TEXT("cache_status"),A.CacheStatus);R.Text(G,TEXT("cut_link_status"),A.CutLinkStatus);R.Text(G,TEXT("flotation_status"),A.FlotationStatus);R.Text(G,TEXT("reference_source"),A.ReferenceSource);
        R.Integer(G,TEXT("tessellation_triangles"),A.TessellationTriangles);R.Integer(G,TEXT("cut_links"),A.CutLinks);R.Number(G,TEXT("cut_link_fraction"),A.CutLinkFraction,0,1);
        R.Vector(G,TEXT("bounds_min"),A.BoundsMinimum);R.Vector(G,TEXT("bounds_max"),A.BoundsMaximum);R.Range(A.BoundsMinimum,A.BoundsMaximum);
        R.Quantity(G,TEXT("equilibrium_heave"),A.EquilibriumHeave);R.Quantity(G,TEXT("running_heave"),A.RunningHeave);R.Quantity(G,TEXT("trim"),A.Trim);R.Quantity(G,TEXT("draft"),A.Draft);R.Quantity(G,TEXT("buoyancy"),A.Buoyancy);R.Quantity(G,TEXT("weight"),A.Weight);E.Geometry.Add(MoveTemp(A));
    }
    if(!R.Valid)return false;
    E.OriginalBytes.Append(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());if(!Hash(E.OriginalBytes,E.SourceSHA256))return false;
    Out=MoveTemp(E);Error.Empty();return true;
}
bool StudioHome4SpatialDiagnostics::Load(const FString& Path,const TOptional<FGuid>& Expected,FStudioHome4SpatialEvidence& Out,FString& Error,const TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe>& Cancel)
{return StudioHome4SpatialPrivate::LoadFixed(Path,Expected,Out,Error,Cancel,{});}
bool StudioHome4SpatialDiagnostics::VerifyOriginal(const FStudioHome4SpatialEvidence& E,FStudioHome4SpatialEvidence& Out,FString& Error)
{
    if(E.OriginalBytes.IsEmpty()||E.OriginalBytes.Num()>StudioHome4SpatialPrivate::MaxBytes||!E.RunId.IsValid()||
        !StudioHome4JSON::UTF8(E.OriginalBytes.GetData(),E.OriginalBytes.Num()))
    {Error=TEXT("Retained original spatial diagnostics are missing, oversized or invalid UTF-8.");return false;}
    FString SHA;
    if(!StudioHome4SpatialPrivate::Hash(E.OriginalBytes,SHA)||SHA!=E.SourceSHA256)
    {Error=TEXT("Retained original spatial diagnostics do not match their source SHA256.");return false;}
    const int32 Offset=E.OriginalBytes.Num()>=3&&E.OriginalBytes[0]==0xef&&E.OriginalBytes[1]==0xbb&&E.OriginalBytes[2]==0xbf?3:0;
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(E.OriginalBytes.GetData()+Offset),E.OriginalBytes.Num()-Offset);
    FStudioHome4SpatialEvidence Checked;
    if(!Parse(FString(Text.Length(),Text.Get()),E.RunId,Checked,Error))return false;
    if(Checked.SourceId!=E.SourceId)
    {Error=TEXT("Retained spatial source identity does not match its original bytes.");return false;}
    Checked.OriginalBytes=E.OriginalBytes;Checked.SourceSHA256=SHA;Checked.SourcePath=E.SourcePath;
    Checked.AttachedProjectId=E.AttachedProjectId;Checked.AttachedCaseId=E.AttachedCaseId;
    Out=MoveTemp(Checked);Error.Empty();return true;
}
#if WITH_DEV_AUTOMATION_TESTS
bool StudioHome4SpatialDiagnostics::LoadWithVerificationForAutomation(const FString& Path,const TOptional<FGuid>& Expected,FStudioHome4SpatialEvidence& Out,FString& Error,TFunction<void()> BeforeVerify)
{return StudioHome4SpatialPrivate::LoadFixed(Path,Expected,Out,Error,{},BeforeVerify);}
#endif
bool StudioHome4SpatialDiagnostics::Locate(const FStudioHome4SpatialEvidence& E,const FString& Id,const FIntVector& Cell,FStudioHome4SpatialLocation& Out,FString& Error)
{
    const auto* P=E.Patches.FindByPredicate([&](const auto& Patch){return Patch.Id==Id;});
    if(!P||!P->Extents||Cell.GetMin()<0||Cell.X>=P->Extents->X||Cell.Y>=P->Extents->Y||Cell.Z>=P->Extents->Z)
    {Error=TEXT("Choose a measured patch with original extents and an in-range original [i,j,k] cell.");return false;}
    FStudioHome4SpatialLocation L;L.RunId=E.RunId;L.SourceId=E.SourceId;L.SourceSHA256=E.SourceSHA256;L.PatchId=P->Id;L.Level=P->Level;L.OriginalCell=Cell;Out=MoveTemp(L);Error.Empty();return true;
}
FStudioHome4SpatialSession::FStudioHome4SpatialSession(TSharedPtr<FStudioModel> M):Owner(M)
{if(M){ProjectId=M->Project.Id;CaseId=M->Project.Draft.Id;}}
FStudioHome4SpatialSession::~FStudioHome4SpatialSession(){Cancel();}
void FStudioHome4SpatialSession::Cancel(){if(Cancellation)Cancellation->store(true,std::memory_order_relaxed);}
void FStudioHome4SpatialSession::Scope()
{
    const auto M=Owner.Pin();
    if(!M)
    {
        if(ProjectId.IsValid()||CaseId.IsValid())
        {ProjectId.Invalidate();CaseId.Invalidate();Current.Reset();Cancel();Message=TEXT("Owning project closed; original spatial evidence cleared.");}
        return;
    }
    if(M->Project.Id==ProjectId&&M->Project.Draft.Id==CaseId)return;
    ProjectId=M->Project.Id;CaseId=M->Project.Draft.Id;Current.Reset();Cancel();Message=TEXT("Project or case changed; original spatial evidence cleared.");
}
bool FStudioHome4SpatialSession::BeginImport(const FString& Path,const TOptional<FGuid>& Expected)
{
    Scope();if(Pending.IsValid()){Message=TEXT("An original spatial source is already being read.");return false;}
    if(Path.IsEmpty()){Message=TEXT("Import cancelled; previous spatial source retained.");return false;}
    if(Expected&&!Expected->IsValid()){Message=TEXT("Expected original run ID must be a valid GUID.");return false;}
    ImportProjectId=ProjectId;ImportCaseId=CaseId;Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Message=TEXT("Reading original spatial diagnostics; previous source remains visible.");
    Pending=Async(EAsyncExecution::ThreadPool,[Path,Expected,Cancel=Cancellation]
    {FResult R;auto Candidate=MakeShared<FStudioHome4SpatialEvidence,ESPMode::ThreadSafe>();if(StudioHome4SpatialDiagnostics::Load(Path,Expected,*Candidate,R.Error,Cancel))R.Evidence=Candidate;return R;});return true;
}
void FStudioHome4SpatialSession::Poll()
{
    Scope();if(!Pending.IsValid()||!Pending.IsReady())return;auto R=Pending.Get();Pending={};const bool Cancelled=Cancellation&&Cancellation->load(std::memory_order_relaxed);Cancellation.Reset();
    if(ProjectId!=ImportProjectId||CaseId!=ImportCaseId){Message=TEXT("Project or case changed during import; evidence was not attached.");return;}
    if(Cancelled){Message=TEXT("Spatial import cancelled; previous source retained.");return;}
    if(!R.Evidence){Message=R.Error+TEXT(" Previous spatial source retained.");return;}
    if(ProjectId.IsValid())R.Evidence->AttachedProjectId=ProjectId;
    if(CaseId.IsValid())R.Evidence->AttachedCaseId=CaseId;
    Current=R.Evidence;Message=TEXT("Original spatial diagnostics imported. Recipe validation not evaluated.");
}
