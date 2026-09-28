#pragma once
#include "CoreMinimal.h"
#include "StudioAssets.h"
#include "StudioCase.h"

/** Original coordinates and topology. No units or axis assumptions are baked in. */
struct FStudioImportedMesh
{
    TArray<FVector> Positions;
    TArray<int32> Indices;
    TArray<int32> TrianglePatches;
    TArray<FString> PatchNames;
    FBox Bounds = FBox(ForceInit);
    int32 BoundaryEdges = 0;
    int32 NonmanifoldEdges = 0;
    int32 InconsistentEdges = 0;
    int32 DuplicateFaces = 0;
    TArray<FString> Notes;
};
struct FStudioMeshImportResult
{
    TSharedPtr<const FStudioImportedMesh,ESPMode::ThreadSafe> Mesh;
    FString Path, Format, SHA256, Error;
    bool bCancelled = false;
    bool IsValid() const { return Mesh.IsValid() && Error.IsEmpty() && !bCancelled; }
};
struct FStudioMeshImportOptions
{
    FString Name;
    // Zero intentionally requires the user to choose the source units.
    double MetersPerUnit = 0;
    int32 UpAxis = 2;
    int32 ForwardAxis = 0;
};
namespace StudioMeshImport
{
    constexpr int64 MaxFileBytes = 64LL*1024*1024;
    constexpr int32 MaxVertices = 1500000;
    constexpr int32 MaxTriangles = 500000;
    /** Worker-only: bounded reads, checksum of the parsed bytes, cancellation. */
    FStudioMeshImportResult Read(const FString& Path, const FStudioAssetCancellation& Cancel);
    FStudioMeshImportResult Parse(TArrayView<const uint8> Bytes, const FString& Format, const FStudioAssetCancellation& Cancel);
    bool MakeAsset(const FStudioMeshImportResult& Source, const FStudioMeshImportOptions& Options, FStudioGeometryAsset& Asset, FString& Error);
    FQuat AxisRotation(int32 UpAxis, int32 ForwardAxis);
    FBox TransformedBounds(const FStudioImportedMesh& Mesh, const FStudioGeometryAsset& Asset);
    FVector TransformPosition(const FVector& Position, const FStudioGeometryAsset& Asset);
}
