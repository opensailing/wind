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

bool FStudioModel::UpdateGeometry(FStudioGeometryEdit& Edit)
{
    const auto Fail = [this, &Edit](const FString& Error)
    {
        Edit.Error = Error;
        GeometryNotice = Notice = Error;
        return false;
    };
    const auto* Current = Project.Draft.Geometry.FindByPredicate([&Edit](const auto& Asset){return Asset.Id == Edit.Saved.Id;});
    if (!Current || !Edit.Matches(*Current))
        return Fail(TEXT("This object changed or was removed. Revert to its applied values before editing."));
    if (IsProjectOpenPending() || IsRecordingLoadPending() || bImportPreview || IsReadingGeometry() ||
        SelectedGeometry != Current->Id || !GeometrySource.IsValid() ||
        GeometrySource.Path != Current->SourcePath || !GeometrySource.SHA256.Equals(Current->SourceSHA256, ESearchCase::IgnoreCase))
        return Fail(TEXT("Select this object and wait for its original mesh to be verified before applying."));

    FStudioGeometryAsset Candidate;
    if (!Edit.Build(Candidate)) return Fail(Edit.Error);
    const FBox Bounds = StudioMeshImport::TransformedBounds(*GeometrySource.Mesh, Candidate);
    if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() ||
        Bounds.Min.GetAbsMax() > 1.e8 || Bounds.Max.GetAbsMax() > 1.e8)
        return Fail(TEXT("The transformed geometry exceeds the supported coordinate range. Reduce its position or scale."));

    const auto Source = GeometrySource;
    const int64 PreviousCaseRevision = Project.Draft.Revision;
    if (!EditCase(TEXT("Edit geometry ") + Candidate.Name, [&Candidate](auto& Case)
    {
        auto* Asset = Case.Geometry.FindByPredicate([&Candidate](const auto& Item){return Item.Id == Candidate.Id;});
        Asset->Name = Candidate.Name;
        Asset->Translation = Candidate.Translation;
        Asset->Rotation = Candidate.Rotation;
        Asset->Scale = Candidate.Scale;
    })) return Fail(Notice);
    // Reuse the already verified immutable original. EditCase invalidates
    // derived domain/lattice work; no reread is needed to transform this mesh.
    if (Project.Draft.Revision != PreviousCaseRevision)
    {
        GeometrySource = Source;
        bReloadGeometry = false;
    }
    GeometryNotice = Notice = TEXT("Object changes applied. Original mesh and saved run configurations are unchanged.");
    return true;
}
