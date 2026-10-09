#include "StudioHome4FieldInventory.h"
#include "StudioHome4Archive.h"
#include "StudioPointRecording.h"
#include "StudioSourceVectors.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StudioHome4InventoryFixtures.inl"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4InventoryTestPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4InventoryFormulaTest,"Studio.Home4.Fields.SourceDiagnosticFormulaeAndValidity",StudioHome4InventoryTestPrivate::Flags)
bool FHome4InventoryFormulaTest::RunTest(const FString&)
{
    TMap<FString,TArray<double>> F;
    TMap<FString,FString> U,E,V;TSet<FString> D;
    F.Add(TEXT("tau_fld"),{.5,.6,.4});U.Add(TEXT("tau_fld"),TEXT("1"));
    F.Add(TEXT("vorticity_magnitude"),{2.,4.,6.});U.Add(TEXT("vorticity_magnitude"),TEXT("1/s"));
    F.Add(TEXT("derivative_valid"),{1.,1.,0.});U.Add(TEXT("derivative_valid"),TEXT("1"));
    F.Add(TEXT("phi"),{.5,0.,1.});U.Add(TEXT("phi"),TEXT("1"));
    for(const TCHAR* K:{TEXT("grad_phi_x"),TEXT("grad_phi_y"),TEXT("grad_phi_z")}){F.Add(K,{.2,0.,0.});U.Add(K,TEXT("1/cell"));}
    for(const TCHAR* K:{TEXT("Sxx"),TEXT("Syy"),TEXT("Szz"),TEXT("Sxy"),TEXT("Sxz"),TEXT("Syz")}){F.Add(K,{1.,1.,1.});U.Add(K,TEXT("1/s"));}
    auto Spec=MakeShared<FJsonObject>(),Fluids=MakeShared<FJsonObject>();Fluids->SetNumberField(TEXT("xi"),5.);Spec->SetObjectField(TEXT("fluids"),Fluids);FString Error;
    if(!TestTrue(Error,StudioHome4FieldInventory::Augment(F,U,E,D,V,&Spec.Get(),{}, {},Error)))return false;
    TestTrue(TEXT("Nonpositive relaxation remains visible"),F[TEXT("tau_margin")][2]<0);
    TestTrue(TEXT("Positive margin log10"),FMath::IsNearlyEqual(F[TEXT("log10_tau_margin")][1],-1.,1.e-12));
    TestEqual(TEXT("Log domain does not turn invalid into a positive margin"),F[TEXT("tau_margin_valid")][0],0.);
    TestEqual(TEXT("Log explicitly masked"),V[TEXT("log10_tau_margin")],FString(TEXT("tau_margin_valid")));
    TestEqual(TEXT("Enstrophy independent oracle"),F[TEXT("enstrophy")][1],8.);
    TestEqual(TEXT("Stored strain Q independent oracle"),F[TEXT("q_stored_strain")][0],-3.5);
    TestEqual(TEXT("Stored strain Q requires complete original curl halo"),V[TEXT("q_stored_strain")],FString(TEXT("derivative_valid")));
    TestTrue(TEXT("Tanh ratio independent oracle"),FMath::IsNearlyEqual(F[TEXT("grad_phi_tanh_ratio")][0],FMath::Sqrt(3.),1.e-12));
    TestEqual(TEXT("Limiter criterion above 1.6"),F[TEXT("limiter_engagement")][0],1.);
    TestEqual(TEXT("Pure phase denominator unavailable"),F[TEXT("interface_gradient_valid")][1],0.);
    TestTrue(TEXT("Kinetic moments grouped advanced"),StudioHome4FieldInventory::Advanced(TEXT("a3_xyz")));
    TMap<FString,TArray<double>> GradientOnly{{TEXT("grad_phi_x"),{3.}},{TEXT("grad_phi_y"),{4.}},{TEXT("grad_phi_z"),{0.}}};
    TMap<FString,FString> GradientUnits{{TEXT("grad_phi_x"),TEXT("1/m")},{TEXT("grad_phi_y"),TEXT("1/m")},{TEXT("grad_phi_z"),TEXT("1/m")}},GE,GV;TSet<FString> GD;
    TestTrue(TEXT("Gradient norm independent of phase or interface thickness"),StudioHome4FieldInventory::Augment(GradientOnly,GradientUnits,GE,GD,GV,nullptr,{}, {},Error));
    TestEqual(TEXT("Independent 3-4-5 gradient norm"),GradientOnly.FindRef(TEXT("grad_phi_magnitude"))[0],5.);
    TestFalse(TEXT("No tanh ratio without original thickness and phi"),GradientOnly.Contains(TEXT("grad_phi_tanh_ratio")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4PackedInventoryTest,"Studio.Home4.Archive.CompleteCatalogueAndPackedForceStorage",StudioHome4InventoryTestPrivate::Flags)
bool FHome4PackedInventoryTest::RunTest(const FString&)
{
    const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())/TEXT("Automation")/(TEXT("Inventory_")+FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Folder,true);
    struct FCleanup{FString Path;~FCleanup(){IFileManager::Get().DeleteDirectory(*Path,false,true);}}Cleanup{Folder};
    for(const auto& Pair:TArray<TPair<const TCHAR*,const TCHAR*>>{{TEXT("C"),StudioHome4InventoryFixtures::CompleteC},{TEXT("Fortran"),StudioHome4InventoryFixtures::CompleteFortran},{TEXT("Reverse"),StudioHome4InventoryFixtures::ReversedForce},{TEXT("UnknownOrder"),StudioHome4InventoryFixtures::UnknownForceOrder}})
    {
        FString Error;
        TArray<uint8> Bytes;FBase64::Decode(Pair.Value,Bytes);const FString Path=Folder/(FString(Pair.Key)+TEXT(".npz"));FFileHelper::SaveArrayToFile(Bytes,*Path);
        FStudioHome4ArchiveRequest R;R.Sources={StudioHome4Archives::Inspect(Path).Source};R.OutputParent=Folder;R.FolderName=Pair.Key;
        R.SourceURI=TEXT("urn:artificial-numpy-unit-fixture");R.Attribution=TEXT("Artificial independent numerical contract fixture; not solver output or an installed sample.");
        R.Mapping.AxisOrder=TEXT("xyz");R.Mapping.MetadataOrder=TEXT("xyz");R.Mapping.CoordinateUnits=TEXT("lattice");R.Mapping.VelocityUnits=TEXT("lattice");R.Mapping.DxMeters=.1;R.Mapping.DtSeconds=.02;R.Mapping.DensityReferenceKgM3=1000.;
        const auto Converted=StudioHome4Archives::Convert(R);
        if(FString(Pair.Key)==TEXT("UnknownOrder")){TestFalse(TEXT("Unknown vector component order cannot be guessed"),Converted.bSuccess);continue;}
        if(!TestTrue(FString(Pair.Key)+TEXT(" complete import: ")+Converted.Error,Converted.bSuccess))continue;
        const auto Open=StudioPointRecordings::Open(Converted.RecordingJSON);if(!TestTrue(Open.Error,Open.Recording.IsValid()))continue;
        const auto& Meta=Open.Recording->Descriptor();
        if(TestTrue(TEXT("Verified manifest produces original run details"),Meta.StructuredGrid->OriginalProvenance.IsSet()))
        {
            const auto& P=*Meta.StructuredGrid->OriginalProvenance;
            TestEqual(TEXT("Original archive path retained"),P.OriginalArchives[0],Path);
            TestEqual(TEXT("Original tag comes only from source"),P.OriginalTag,FString(TEXT("original-fixture-tag")));
            TestEqual(TEXT("Original effective backend retained"),P.OriginalBackend,FString(TEXT("artificial-test-extension")));
            TestEqual(TEXT("Explicit original rate retained"),P.MeasuredMLUPS.Get(0),12.5);
            TestEqual(TEXT("Rate pinned to verified manifest"),P.MeasurementSHA256,Meta.StructuredGrid->SourceManifestSHA256);
            TestFalse(TEXT("Partial source unit map never becomes runnable spec"),P.OriginalRunSpec.IsSet());
        }
        TestTrue(TEXT("Complete source catalog exceeds previous 32-field truncation"),Meta.Fields.Num()>32);
        const auto Read=Open.Recording->ReadFrame(0,{TEXT("F_cap_x"),TEXT("F_cap_y"),TEXT("F_cap_magnitude"),TEXT("a3_magnitude"),TEXT("a4_magnitude"),TEXT("nu"),TEXT("dissipation"),TEXT("phase_J_magnitude")});
        if(!TestTrue(Read.Error,Read.Frame.IsValid()))continue;
        TestEqual(TEXT("Packed X original value"),(*Read.Frame->FindValues(TEXT("F_cap_x")))[0],3.);
        TestEqual(TEXT("Packed Y original value"),(*Read.Frame->FindValues(TEXT("F_cap_y")))[0],4.);
        TestEqual(TEXT("Packed force norm independent 3-4-5 oracle"),(*Read.Frame->FindValues(TEXT("F_cap_magnitude")))[0],5.);
        const auto& Grid=*Meta.StructuredGrid;
        const int32 Row=1+Grid.Dimensions.X*(2+Grid.Dimensions.Y*3);
        const int32 COrdinal=(1*Grid.Dimensions.Y+2)*Grid.Dimensions.Z+3;
        TestEqual(TEXT("Packed component retains original nonconstant XYZ for both storage orders"),(*Read.Frame->FindValues(TEXT("F_cap_x")))[Row],3.+COrdinal);
        TestEqual(TEXT("Packed Y retains distinct component and axis strides"),(*Read.Frame->FindValues(TEXT("F_cap_y")))[Row],4.+2*COrdinal);
        const auto Solver=StudioRecordings::Import(Converted.RecordingJSON,0,{});
        if(TestTrue(Solver.Error,Solver.Source.IsValid()))
        {
            const auto Frame=Solver.Source->ReadScalarFrame(0,TEXT("phi"));FStudioSourceVectorRows Vector;
            if(TestTrue(Frame.Error,Frame.Field.IsValid())&&TestTrue(TEXT("Force components load independently of color scalar"),StudioSourceVectors::Read(*Frame.Field,TEXT("F_cap"),Vector,{},Error)))
            {
                TestEqual(TEXT("Force vector keeps explicit original unit"),Vector.Unit,FString(TEXT("lu_force_density")));
                TestEqual(TEXT("Vector component source XYZ maps to scene XZY"),Vector.Values[Row],FVector(3.+COrdinal,-COrdinal,4.+2*COrdinal));
                TestEqual(TEXT("Vector row count is never resampled"),Vector.Values.Num(),Meta.PointCount);
                FStudioSourceVectorRows Selected;
                if(TestTrue(TEXT("Display decimation loads selected original rows only"),StudioSourceVectors::Read(*Frame.Field,TEXT("F_cap"),Selected,{},Error,{Row,0})))
                {TestEqual(TEXT("Bounded vector scratch follows display sample budget"),Selected.Values.Num(),2);TestEqual(TEXT("Selected original identity preserved"),Selected.Values[0],Vector.Values[Row]);}
            }
        }
        TestTrue(TEXT("Seven original a3 components"),FMath::IsNearlyEqual((*Read.Frame->FindValues(TEXT("a3_magnitude")))[0],FMath::Sqrt(140.),1.e-12));
        TestTrue(TEXT("Six original a4 components"),FMath::IsNearlyEqual((*Read.Frame->FindValues(TEXT("a4_magnitude")))[0],FMath::Sqrt(91.),1.e-12));
        TestTrue(TEXT("Tau-derived original physical viscosity"),FMath::IsNearlyEqual((*Read.Frame->FindValues(TEXT("nu")))[0],.05,1.e-12));
        // S=(.01..06)/dt; diag squares=3.5, offdiag squares=19.25 -> 2*.05*(3.5+2*19.25).
        TestTrue(TEXT("Tau-based stored strain dissipation"),FMath::IsNearlyEqual((*Read.Frame->FindValues(TEXT("dissipation")))[0],4.2,1.e-12));
        FString Manifest;FFileHelper::LoadFileToString(Manifest,*(Converted.Path/TEXT("source-manifest.json")));TestTrue(TEXT("Original packed source component provenance retained"),Manifest.Contains(TEXT("retainedComponents"))&&Manifest.Contains(TEXT("F_cap_x")));
    }
    return true;
}
#endif
