#include "StudioSourceVectors.h"
#include "StudioVolume.h"

TArray<FStudioSourceVector> StudioSourceVectors::Catalogue(const FStudioPointRecordingDescriptor& D)
{
    TMap<FString,FStudioSourceVector> Groups;TSet<FString> Invalid;
    for(const auto& F:D.Fields)if(!F.Vector.IsEmpty())
    {
        auto& G=Groups.FindOrAdd(F.Vector);G.Id=F.Vector;
        const int32 A=F.Component==TEXT("x")?0:F.Component==TEXT("y")?1:F.Component==TEXT("z")?2:INDEX_NONE;
        if(A==INDEX_NONE||(!G.Unit.IsEmpty()&&G.Unit!=F.Unit)){Invalid.Add(G.Id);continue;}
        G.Unit=F.Unit;if(!G.Components[A].IsEmpty())Invalid.Add(G.Id);G.Components[A]=F.Id;
    }
    TArray<FStudioSourceVector> Result;
    for(const auto& Pair:Groups)
    {
        const auto& G=Pair.Value;
        if(!Invalid.Contains(G.Id)&&!G.Components[0].IsEmpty()&&!G.Components[1].IsEmpty()&&
            (D.SpatialDimensions==2||!G.Components[2].IsEmpty()))Result.Add(G);
    }
    Result.Sort([](const auto& A,const auto& B){return A.Id<B.Id;});return Result;
}
bool StudioSourceVectors::Read(const IStudioField& Field,const FString& Id,FStudioSourceVectorRows& Out,
    const FStudioLoadCancellation& Cancel,FString& Error,const TArray<int32>& SelectedRows)
{
    const auto Points=Field.OriginalPoints();if(!Points){Error=TEXT("Original vector rows are unavailable.");return false;}
    const auto Groups=Catalogue(*Points->Descriptor);const auto* G=Groups.FindByPredicate([&](const auto& V){return V.Id==Id;});
    if(!G){Error=TEXT("Complete original vector components are not supplied: ")+Id;return false;}
    const auto Volume=Field.VolumeReconstruction();FStudioSourceVectorRows R;R.Unit=G->Unit;
    const int32 SourceCount=Points->Geometry->Positions.Num(),Count=SelectedRows.IsEmpty()?SourceCount:SelectedRows.Num();
    for(int32 Row:SelectedRows)if(Row<0||Row>=SourceCount){Error=TEXT("Vector sample row is outside the original recording.");return false;}
    R.Values.Init(FVector::ZeroVector,Count);R.Valid.Init(1,Count);
    for(int32 A=0;A<3;++A)
    {
        if(G->Components[A].IsEmpty())continue;
        TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Snapshot;
        auto Frame=Points;
        if(!Frame->FindValues(G->Components[A]))
        {Snapshot=Field.LoadScalarSnapshot(G->Components[A],Cancel,Error);if(!Snapshot)return false;Frame=Snapshot->OriginalPoints();}
        const auto* Values=Frame?Frame->FindValues(G->Components[A]):nullptr;
        if(!Values||Values->Num()!=SourceCount){Error=TEXT("Original vector component row mismatch.");return false;}
        TArray<uint8> Mask;
        if(Volume&&Volume->OriginalGrid)
        {
            Mask=StudioVolumes::SourceMask(*Frame,*Volume,G->Components[A],Id==TEXT("velocity"),Error,Cancel);
            if(!Error.IsEmpty()||Mask.Num()!=SourceCount)return false;
        }
        const int32 SceneAxis=A==1?2:A==2?1:0;
        for(int32 I=0;I<R.Values.Num();++I)
        {
            if((I&4095)==0&&Cancel&&Cancel->load()){Error=TEXT("Vector read cancelled.");return false;}
            const int32 Row=SelectedRows.IsEmpty()?I:SelectedRows[I];
            R.Values[I][SceneAxis]=(*Values)[Row];if((!Mask.IsEmpty()&&Mask[Row]!=1)||!FMath::IsFinite((*Values)[Row]))R.Valid[I]=0;
        }
    }
    Out=MoveTemp(R);Error.Reset();return true;
}
