#include "StudioSliceRendering.h"
#include "StudioModel.h"
#include "StudioVolume.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioNamedSliceSamples,"Studio.Inspection.SliceRecordedValuesAndBounds",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioNamedSliceSamples::RunTest(const FString&)
{
    FRecordedSolver Solver;const auto Field=Solver.CaptureViewField(0,TEXT("pressure"),false);
    FStudioSliceObject S;S.Name=TEXT("Source plane");const auto& D=Solver.Descriptor();S.Source={D.Id,D.MetadataSHA256,D.PayloadSHA256};S.Normal=FVector::RightVector;
    auto Data=StudioSliceRendering::Build(*Field,D.DisplayBounds,{S},TEXT("pressure"));
    if(!TestTrue(TEXT("Published source produces field slice triangles"),Data.Indices.Num()>0))return false;
    TestTrue(TEXT("Sample grid stays within total vertex budget"),Data.PositionsMeters.Num()<=StudioSliceRendering::MaximumVertices);
    int32 Checked=0;
    for(int32 Index:Data.Indices)
    {
        const FVector P=Data.PositionsMeters[Index];double Value;
        if(!TestTrue(TEXT("Every referenced slice sample is inside source coverage"),Field->SampleScalar(P,TEXT("pressure"),Value)))return false;
        TestEqual(TEXT("Slice retains exact sampler value before palette"),Data.Scalars[Index],Value);
        TestEqual(TEXT("2D samples stay on the source plane"),P.Y,0.);++Checked;
    }
    TestTrue(TEXT("Substantial source sample coverage"),Checked>1000);
    const auto Before=Data.Indices;auto Later=Solver.CaptureViewField(20,TEXT("pressure"),false);
    auto Evolved=StudioSliceRendering::Build(*Later,D.DisplayBounds,{S},TEXT("pressure"));
    int32 Changes=0;for(int32 I=0;I<FMath::Min(Data.Scalars.Num(),Evolved.Scalars.Num());++I)if(Data.Scalars[I]!=Evolved.Scalars[I])++Changes;
    TestTrue(TEXT("Slice reflects real transient source values"),Changes>100);
    S.Origin.Y=.1;auto OffPlane=StudioSliceRendering::Build(*Field,D.DisplayBounds,{S},TEXT("pressure"));
    TestTrue(TEXT("Display extrusion does not invent a scientific plane"),OffPlane.Indices.IsEmpty()&&OffPlane.Notices.Contains(S.Id));
    S.Origin.Y=0;S.Source.MetadataSHA256=FString::ChrN(64,'f');
    TestTrue(TEXT("Foreign recording slice is not rendered"),StudioSliceRendering::Build(*Field,D.DisplayBounds,{S},TEXT("pressure")).Indices.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVolumeSliceSamples,"Studio.Inspection.VolumeSliceCoverageAndRecordedValues",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioVolumeSliceSamples::RunTest(const FString&)
{
    const FString SourcePath=FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json"),
        MappingPath=FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json");
    const auto Open=StudioRecordings::Import(SourcePath,0,{});
    if(!TestTrue(*Open.Error,Open.Source.IsValid()&&Open.Reference.IsSet()))return false;
    const auto Bound=StudioRecordings::ImportReconstruction(*Open.Reference,MappingPath,0,{});
    if(!TestTrue(*Bound.Error,Bound.Source.IsValid()&&Bound.Source->VolumeReconstruction().IsValid()))return false;
    const auto Volume=Bound.Source->VolumeReconstruction();const auto& Descriptor=Bound.Source->Descriptor();
    const auto Field=Bound.Source->CaptureViewField(0,TEXT("pressure"),false);
    // Real grid regression: nine sparse tests all pass, but an interior point
    // has no supported interpolation cell in the original reconstruction.
    const FVector A(-.009540310178725222,.006158329969354025,.0332522998081688),
        B(-.008504062190464456,.005640205975223647,.0332522998081688),
        C(-.008209428666803825,.00622947302254491,.03276124393540108),
        D(-.00924567665506459,.006747597016675287,.03276124393540108);
    double Value;
    for(const FVector& P:{A,B,C,D,(A+B)*.5,(B+C)*.5,(C+D)*.5,(D+A)*.5,(A+C)*.5})
        TestTrue(TEXT("Published regression passes old corner and midpoint coverage"),Field->SampleScalar(P,TEXT("pressure"),Value));
    int32 Missing=0;for(int32 X=1;X<8;++X)for(int32 Y=1;Y<8;++Y)
        if(!Field->SampleScalar(A+(B-A)*(X/8.)+(D-A)*(Y/8.),TEXT("pressure"),Value))++Missing;
    TestTrue(TEXT("Real unsupported interior was missed by sparse coverage checks"),Missing>0);
    auto SourcePoint=[](FVector P){return FVector(P.X,P.Z,P.Y);};FBox Region(ForceInit);
    for(const auto& P:{A,B,C,D})Region+=SourcePoint(P);
    TestFalse(TEXT("Whole-region coverage rejects that unsupported interior"),Volume->SupportsRegion(Region));

    const FString Folder=FPaths::ProjectDir()/TEXT("tmp/debug/inspection-volume-slices");IFileManager::Get().MakeDirectory(*Folder,true);
    auto Manifest=MakeShared<FJsonObject>();Manifest->SetStringField(TEXT("source"),Descriptor.MetadataSHA256);
    Manifest->SetStringField(TEXT("reconstruction"),Volume->MetadataSHA256);Manifest->SetStringField(TEXT("byteOrder"),TEXT("little"));
    Manifest->SetStringField(TEXT("coordinates"),TEXT("scene XZY in meters; only indexed vertices are scientific samples"));
    TArray<TSharedPtr<FJsonValue>> Cases;
    for(int32 Case=0;Case<4;++Case)
    {
        const int32 Frame=Case==1?2:Case==2?1:0;const FString Scalar=Case==2?TEXT("velocity_magnitude"):Case==3?TEXT("cell_volume"):TEXT("pressure");
        FStudioSliceObject S;S.Name=TEXT("Verified 3D slice");S.Source={Descriptor.Id,Descriptor.MetadataSHA256,Descriptor.PayloadSHA256};
        S.Origin=Descriptor.DisplayBounds.GetCenter();S.Normal=Case==0?FVector::RightVector:FVector(1,2,3).GetSafeNormal();S.Opacity=.8;
        if(Case==1)S.Origin.X=FMath::Lerp(Descriptor.DisplayBounds.Min.X,Descriptor.DisplayBounds.Max.X,.25);
        const auto Snapshot=Bound.Source->CaptureViewField(Frame,Scalar,false);
        const auto Data=StudioSliceRendering::Build(*Snapshot,Descriptor.DisplayBounds,{S},Scalar);
        if(!TestTrue(TEXT("Authentic 3D slice has substantial supported coverage"),Data.Indices.Num()>12000&&Data.PositionsMeters.Num()<=StudioSliceRendering::MaximumVertices))return false;
        TestFalse(TEXT("Unused GPU placeholders cannot reject valid 3D slice transport"),Data.Scalars.ContainsByPredicate([](double V){return !FMath::IsFinite(V);}));
        double Error=0;for(int32 Index:Data.Indices)
        {
            const auto P=Data.PositionsMeters[Index];
            if(!Snapshot->SampleScalar(P,Scalar,Value)){AddError(TEXT("Indexed 3D slice value has no source support"));return false;}
            Error=FMath::Max(Error,FMath::Abs(Value-Data.Scalars[Index]));
            if(FMath::Abs(FVector::DotProduct(P-S.Origin,S.Normal))>1.e-12){AddError(TEXT("Slice sample escaped its arbitrary plane"));return false;}
        }
        TestEqual(TEXT("Slice vertices retain source sample precision"),Error,0.);
        int32 InteriorChecks=0;
        for(int32 T=0;T<Data.Indices.Num();T+=3*97)
        {
            const auto P=Data.PositionsMeters[Data.Indices[T]],Q=Data.PositionsMeters[Data.Indices[T+1]],R=Data.PositionsMeters[Data.Indices[T+2]];
            for(int32 U=1;U<8;++U)for(int32 V=1;U+V<8;++V)
            {
                if(!Snapshot->SampleScalar(P+(Q-P)*(U/8.)+(R-P)*(V/8.),Scalar,Value))
                {AddError(TEXT("Rendered 3D slice bridges unsupported data"));return false;}
                ++InteriorChecks;
            }
        }
        TestTrue(TEXT("Many interior source positions remain supported"),InteriorChecks>1000);
        const FString Prefix=FString::Printf(TEXT("case-%d"),Case);TArray<double> Vertices;
        for(int32 I=0;I<Data.PositionsMeters.Num();++I)
        {const auto P=Data.PositionsMeters[I];Vertices.Append({P.X,P.Y,P.Z,Data.Scalars[I]});}
        auto Write=[&](const FString& File,const void* Pointer,int64 Bytes)
        {return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(static_cast<const uint8*>(Pointer),Bytes),*(Folder/File));};
        TestTrue(TEXT("Export native slice vertices for independent verification"),Write(Prefix+TEXT("-vertices.f64"),Vertices.GetData(),Vertices.Num()*int64(sizeof(double))));
        TestTrue(TEXT("Export native slice triangles for independent verification"),Write(Prefix+TEXT("-indices.i32"),Data.Indices.GetData(),Data.Indices.Num()*int64(sizeof(int32))));
        auto Item=MakeShared<FJsonObject>();Item->SetStringField(TEXT("prefix"),Prefix);Item->SetStringField(TEXT("field"),Scalar);
        Item->SetNumberField(TEXT("frame"),Frame);Item->SetNumberField(TEXT("vertices"),Data.PositionsMeters.Num());Item->SetNumberField(TEXT("indices"),Data.Indices.Num());
        Cases.Add(MakeShared<FJsonValueObject>(Item));
    }
    Manifest->SetArrayField(TEXT("cases"),Cases);FString Text;FJsonSerializer::Serialize(Manifest,TJsonWriterFactory<>::Create(&Text));
    TestTrue(TEXT("Save exact source and mesh audit manifest"),FFileHelper::SaveStringToFile(Text,*(Folder/TEXT("manifest.json"))));
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Cancelled whole-region support does not report coverage"),Volume->SupportsRegion(Volume->SourceBounds,Cancel));
    return true;
}
#endif
