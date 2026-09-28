#include "StudioModel.h"
#include "Async/Async.h"
#include "Misc/Paths.h"

bool FStudioModel::StartMeshRead(const FString& Path,bool bImport,const FGuid& Id)
{
    if(PendingMesh.IsValid()) {GeometryNotice=TEXT("Wait for the current geometry read to finish, or cancel it.");return false;}
    if(Path.IsEmpty()||FPaths::IsRelative(Path)||Path.Contains(TEXT("://"))) {GeometryNotice=TEXT("Choose a local STL or OBJ file.");return false;}
    MeshCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    ReadingMeshGeneration=++MeshGeneration;ReadingMeshProject=Project.Id;ReadingGeometryId=Id;bReadingImport=bImport;
    PendingMesh=Async(EAsyncExecution::ThreadPool,[Path,Cancel=MeshCancellation.ToSharedRef()]{return StudioMeshImport::Read(Path,Cancel);});
    GeometrySource={};bImportPreview=bImport;GeometryNotice=TEXT("Reading and checking geometry…");++GeometryRevision;return true;
}
bool FStudioModel::RequestGeometryImport(const FString& Path)
{
    if(!StartMeshRead(Path,true,FGuid()))return false;
    SelectedGeometry.Invalidate();ImportOptions={};ImportOptions.Name=FPaths::GetBaseFilename(Path).Left(120);bReloadGeometry=false;return true;
}
void FStudioModel::CancelGeometryImport()
{
    ++MeshGeneration;if(MeshCancellation)*MeshCancellation=true;
    GeometrySource={};bImportPreview=false;GeometryNotice=TEXT("Geometry preview cancelled. Case unchanged.");++GeometryRevision;bReloadGeometry=false;
}
void FStudioModel::InvalidateGeometry()
{
    InvalidateDomainGeometry();
    ++MeshGeneration;if(MeshCancellation)*MeshCancellation=true;
    GeometrySource={};bImportPreview=false;++GeometryRevision;
    if(!Project.Draft.Geometry.ContainsByPredicate([this](const auto& A){return A.Id==SelectedGeometry;}))SelectedGeometry.Invalidate();
    bReloadGeometry=SelectedGeometry.IsValid();
}
bool FStudioModel::SelectGeometry(const FGuid& Id)
{
    const auto* Asset=Project.Draft.Geometry.FindByPredicate([&Id](const auto& A){return A.Id==Id;});if(!Asset)return false;
    if(!StartMeshRead(Asset->SourcePath,false,Id))return false;
    SelectedGeometry=Id;bReloadGeometry=false;return true;
}
void FStudioModel::PollGeometry()
{
    if(PendingMesh.IsValid()&&PendingMesh.IsReady())
    {
        auto Result=PendingMesh.Get();PendingMesh=TFuture<FStudioMeshImportResult>();
        const bool Current=ReadingMeshProject==Project.Id&&ReadingMeshGeneration==MeshGeneration&&!MeshCancellation->load();MeshCancellation.Reset();
        if(Current)
        {
            if(!bReadingImport&&Result.IsValid())
            {
                const auto* A=Project.Draft.Geometry.FindByPredicate([this](const auto& Asset){return Asset.Id==ReadingGeometryId;});
                bool Matches=A&&A->SourceSHA256.Equals(Result.SHA256,ESearchCase::IgnoreCase)&&A->Patches.Num()==Result.Mesh->PatchNames.Num();
                if(Matches)for(int32 I=0;I<A->Patches.Num();++I)Matches&=A->Patches[I].Name==Result.Mesh->PatchNames[I];
                if(!Matches){Result.Mesh.Reset();Result.Error=TEXT("Source contents or patch mapping differ. Locate the original file, or import the changed mesh separately.");}
            }
            GeometrySource=MoveTemp(Result);++GeometryRevision;
            GeometryNotice=GeometrySource.IsValid()?(bReadingImport?TEXT("Preview ready. Choose source units and confirm orientation before importing."):TEXT("Original geometry verified. Viewing the saved case object.")):GeometrySource.Error;
        }
    }
    if(!PendingMesh.IsValid()&&bReloadGeometry){bReloadGeometry=false;SelectGeometry(SelectedGeometry);}
}
bool FStudioModel::GeometryAssetForPreview(FStudioGeometryAsset& Asset,FString& Error) const
{
    if(bImportPreview)return StudioMeshImport::MakeAsset(GeometrySource,ImportOptions,Asset,Error);
    const auto* A=Project.Draft.Geometry.FindByPredicate([this](const auto& G){return G.Id==SelectedGeometry;});
    if(!A||!GeometrySource.IsValid()){Error=TEXT("Select a verified geometry object.");return false;}
    Asset=*A;Error.Empty();return true;
}
bool FStudioModel::CommitGeometryImport()
{
    if(!bImportPreview||PendingMesh.IsValid()){GeometryNotice=TEXT("Wait for a geometry preview before importing.");return false;}
    FStudioGeometryAsset Asset;FString Error;
    if(!GeometryAssetForPreview(Asset,Error)){GeometryNotice=Error;return false;}
    const auto Source=GeometrySource;
    if(!EditCase(TEXT("Import ")+Asset.Name,[&Asset](auto& D){D.Geometry.Add(Asset);})) {GeometryNotice=Notice;return false;}
    SelectedGeometry=Asset.Id;GeometrySource=Source;bReloadGeometry=false;bImportPreview=false;++GeometryRevision;
    GeometryNotice=TEXT("Geometry imported. Save to keep the case object. Recorded CFD is unchanged.");Notice=GeometryNotice;return true;
}
