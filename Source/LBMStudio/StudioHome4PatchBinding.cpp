#include "StudioHome4PatchBinding.h"
bool StudioHome4PatchBinding::Matches(const FStudioHome4SpatialEvidence& E,const FString& Id,const FStudioPointStructuredGrid& G,FString& Error)
{
    FGuid Run;const auto* Patch=E.Patches.FindByPredicate([&](const auto& P){return P.Id==Id;});
    if(!Patch||!Patch->Extents||!Patch->Origin||!Patch->Spacing||!FGuid::Parse(G.SourceRunId,Run)||Run!=E.RunId)
    {Error=TEXT("Patch recording needs the same explicit original run and complete original patch affine.");return false;}
    const bool Explicit=G.PatchId==Id&&G.PatchLevel==Patch->Level&&G.SpatialSourceId==E.SourceId&&G.SpatialSourceSHA256==E.SourceSHA256&&!G.SpatialSourceSHA256.IsEmpty();
    const bool LegacyRoot=Patch->Level==0&&G.PatchId.IsEmpty();
    if((!Explicit&&!LegacyRoot)||(!G.SpatialSourceSHA256.IsEmpty()&&G.SpatialSourceSHA256!=E.SourceSHA256)||
        *Patch->Extents!=G.OriginalDimensions||!Patch->Origin->Equals(G.OriginalOrigin,1.e-12)||!Patch->Spacing->Equals(G.OriginalSpacing,1.e-12)||E.CoordinateUnit!=G.CoordinateUnits)
    {Error=TEXT("This recording does not match the selected patch ID, level, source identity and original affine. Patches are not stitched or rescaled.");return false;}
    Error.Reset();return true;
}
