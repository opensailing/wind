#include "StudioHome4PatchBinding.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4PatchBindingTest,"Studio.Home4.Spatial.FinePatchOriginalAffineBinding",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FHome4PatchBindingTest::RunTest(const FString&)
{
    FStudioHome4SpatialEvidence E;E.RunId=FGuid::NewGuid();E.SourceId=TEXT("fine-grid-original");E.SourceSHA256=FString::ChrN(64,'a');E.CoordinateUnit=TEXT("lattice");
    FStudioHome4SpatialPatch P;P.Id=TEXT("body-band");P.Level=2;P.Extents=FIntVector(30,40,50);P.Origin=FVector(10,20,30);P.Spacing=FVector(.25);E.Patches.Add(P);
    FStudioPointStructuredGrid G;G.SourceRunId=E.RunId.ToString();G.PatchId=P.Id;G.PatchLevel=2;G.SpatialSourceId=E.SourceId;G.SpatialSourceSHA256=E.SourceSHA256;G.OriginalDimensions=*P.Extents;G.OriginalOrigin=*P.Origin;G.OriginalSpacing=*P.Spacing;G.CoordinateUnits=E.CoordinateUnit;FString Error;
    TestTrue(TEXT("Explicit same-run fine patch with original affine accepted"),StudioHome4PatchBinding::Matches(E,P.Id,G,Error));
    auto Bad=G;Bad.PatchId.Empty();TestFalse(TEXT("Unidentified fine patch is never mapped to root"),StudioHome4PatchBinding::Matches(E,P.Id,Bad,Error));
    Bad=G;Bad.PatchLevel=0;TestFalse(TEXT("Wrong level rejected"),StudioHome4PatchBinding::Matches(E,P.Id,Bad,Error));
    Bad=G;Bad.OriginalSpacing=FVector(1);TestFalse(TEXT("Coarse affine cannot label fine values"),StudioHome4PatchBinding::Matches(E,P.Id,Bad,Error));
    Bad=G;Bad.SpatialSourceSHA256=FString::ChrN(64,'b');TestFalse(TEXT("Rewritten evidence rejected"),StudioHome4PatchBinding::Matches(E,P.Id,Bad,Error));
    Bad=G;Bad.SpatialSourceSHA256.Empty();TestFalse(TEXT("Missing evidence hash rejected for a fine patch"),StudioHome4PatchBinding::Matches(E,P.Id,Bad,Error));
    Bad=G;Bad.SourceRunId=FGuid::NewGuid().ToString();TestFalse(TEXT("Another run rejected even with identical topology"),StudioHome4PatchBinding::Matches(E,P.Id,Bad,Error));
    Bad=G;Bad.CoordinateUnits=TEXT("physical");TestFalse(TEXT("Unit convention mismatch not rescaled silently"),StudioHome4PatchBinding::Matches(E,P.Id,Bad,Error));
    return true;
}
#endif
