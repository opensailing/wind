#include "StudioProbeSampling.h"
#include "StudioModel.h"
#include "StudioPointRecording.h"
#include "StudioSurfaceReconstruction.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto ProbeFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FString ProbePointPath(){return FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json");}
FString ProbeSurfacePath(){return FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture/reconstruction.json");}
FStudioProbeRequest ProbeRequest(const FStudioRecordingLoadResult& Source,int32 Ordinal,const FString& Field)
{
    FStudioProbeRequest R;R.ProjectId=FGuid::NewGuid();R.PresentationId=1;R.Probe.Name=TEXT("Recorded sample");
    R.Probe.Source={Source.Reference->Id,Source.Reference->MetadataSHA256,Source.Reference->PayloadSHA256};
    R.DisplayedScalar=Field;R.Field=Source.Source->CaptureViewField(Ordinal,Field,false);return R;
}
TSharedPtr<FJsonObject> ReadProbeExpectations(const FString& Path)
{
    FString Text;TSharedPtr<FJsonObject> O;
    if(FFileHelper::LoadFileToString(Text,*Path))FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);
    return O;
}
/** Wraps actual source arrays; only provenance is deliberately changed. */
class FProbeIdentityFixture final : public IStudioField
{
public:
    explicit FProbeIdentityFixture(TSharedPtr<const IStudioField,ESPMode::ThreadSafe> In):Inner(MoveTemp(In)){}
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Inner,Loaded;
    TOptional<FStudioFieldIdentity> ChangedIdentity;
    TOptional<FStudioScalarDescriptor> ChangedScalar;
    bool IsValid() const override{return Inner->IsValid();}
    TOptional<FStudioFieldIdentity> Identity() const override{return ChangedIdentity.IsSet()?ChangedIdentity:Inner->Identity();}
    TOptional<FStudioScalarDescriptor> Scalar(const FString& Id) const override{return ChangedScalar.IsSet()?ChangedScalar:Inner->Scalar(Id);}
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> LoadScalarSnapshot(const FString&,const FStudioLoadCancellation&,FString&) const override{return Loaded;}
    bool Sample(const FVector& P,FStudioFieldValue& Out) const override{return Inner->Sample(P,Out);}
    bool SampleScalar(const FVector& P,const FString& Id,double& Out) const override{return Inner->SampleScalar(P,Id,Out);}
    bool IsSolid(const FVector& P) const override{return Inner->IsSolid(P);}
    const TArray<FVector2D>& Boundary() const override{return Inner->Boundary();}
    const TArray<FIntVector>& BoundaryTriangles() const override{return Inner->BoundaryTriangles();}
    TSharedPtr<const FStudioPointFrame,ESPMode::ThreadSafe> OriginalPoints() const override{return Inner->OriginalPoints();}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbeOriginal,"Studio.Inspection.OriginalPointSamples",ProbeFlags)
bool FStudioProbeOriginal::RunTest(const FString&)
{
    const auto Source=StudioRecordings::Import(ProbePointPath(),0,{});
    if(!TestTrue(*Source.Error,Source.Source.IsValid()&&Source.Reference.IsSet()))return false;
    const auto Expected=ReadProbeExpectations(FPaths::GetPath(ProbePointPath())/TEXT("expected.json"));
    if(!TestTrue(TEXT("Independent original-HDF5 row expectations available"),Expected.IsValid()))return false;
    int32 Checked=0,FixedChecked=0;
    for(int32 Frame=0;Frame<3;++Frame)for(const auto& Field:Source.Source->Descriptor().Scalars)
    {
        auto R=ProbeRequest(Source,Frame,Field.Id);R.Probe.Method=EStudioProbeMethod::OriginalPoint;
        auto Fixed=ProbeRequest(Source,Frame,Field.Id==TEXT("pressure")?TEXT("velocity_magnitude"):TEXT("pressure"));
        Fixed.Probe.Method=EStudioProbeMethod::OriginalPoint;Fixed.Probe.Field=Field.Id;
        TestNull(TEXT("Fixed probe scalar is absent from the displayed snapshot"),Fixed.Field->OriginalPoints()->FindValues(Field.Id));
        for(const auto& Entry:Expected->GetArrayField(TEXT("samples")))
        {
            const auto E=Entry->AsObject();if(int32(E->GetNumberField(TEXT("frame")))!=Frame||E->GetStringField(TEXT("field"))!=Field.Id)continue;
            const int32 Row=int32(E->GetNumberField(TEXT("point")));const TSharedPtr<FJsonObject>* Point=nullptr;
            for(const auto& Value:Expected->GetArrayField(TEXT("points")))
                if(int32(Value->AsObject()->GetNumberField(TEXT("row")))==Row){Value->TryGetObject(Point);break;}
            if(!TestNotNull(TEXT("Independent source point ID available"),Point))return false;
            R.Probe.PointId=int64((*Point)->GetNumberField(TEXT("id")));
            const auto Result=StudioProbeSampling::Evaluate(R);
            if(!TestTrue(TEXT("Original ID resolves one finite original value"),Result.Status==EStudioProbeStatus::Ready&&
                Result.Samples.Num()==1&&Result.Samples[0].Value.IsSet()&&Result.Identity.IsSet()))return false;
            TestEqual(TEXT("Original values are bit-exact through probe sampling"),Result.Samples[0].Value.GetValue(),E->GetNumberField(TEXT("value")));
            const auto& XY=(*Point)->GetArrayField(TEXT("position"));const FVector P(XY[0]->AsNumber(),XY[1]->AsNumber(),0);
            TestEqual(TEXT("Original axes and coordinates retained"),Result.Samples[0].SourcePosition.GetValue(),P);
            TestEqual(TEXT("Source XY maps to scene XZ"),Result.Samples[0].ScenePosition.GetValue(),FVector(P.X,0,P.Y));
            TestEqual(TEXT("Units come from source metadata"),Result.Unit,Field.Unit);
            TestEqual(TEXT("Frame ordinal belongs to immutable snapshot"),Result.Identity->Ordinal,Frame);
            TestEqual(TEXT("Descriptor hash matches verified import"),Result.Identity->MetadataSHA256,Source.Reference->MetadataSHA256);
            TestTrue(TEXT("Result matches its exact request"),Result.Matches(R));++Checked;
            Fixed.Probe.PointId=R.Probe.PointId;const auto Independent=StudioProbeSampling::Evaluate(Fixed);
            if(!TestTrue(TEXT("Independent field resolves the original point"),Independent.Status==EStudioProbeStatus::Ready&&
                Independent.Samples.Num()==1&&Independent.Samples[0].Value.IsSet()&&Independent.Identity.IsSet()))return false;
            TestEqual(TEXT("Independent field retains bit-exact original value"),Independent.Samples[0].Value.GetValue(),E->GetNumberField(TEXT("value")));
            TestEqual(TEXT("Independent field has the displayed ordinal"),Independent.Identity->Ordinal,Frame);
            TestEqual(TEXT("Independent field has original units"),Independent.Unit,Field.Unit);
            TestTrue(TEXT("Publication is bound to original presented field, not optional snapshot"),Independent.Matches(Fixed));
            TestNull(TEXT("Additional reads do not mutate the displayed frame"),Fixed.Field->OriginalPoints()->FindValues(Field.Id));++FixedChecked;
        }
    }
    TestEqual(TEXT("Every independent point/field/frame fixture checked"),Checked,135);
    TestEqual(TEXT("Every source value also checked with a different displayed scalar"),FixedChecked,135);
    auto R=ProbeRequest(Source,0,TEXT("pressure"));R.Probe.Method=EStudioProbeMethod::OriginalPoint;R.Probe.PointId=MAX_int64;
    const auto Missing=StudioProbeSampling::Evaluate(R);
    TestTrue(TEXT("Missing original ID has no nearest-point substitution"),Missing.Samples.Num()==1&&
        Missing.Samples[0].Status==EStudioProbeSampleStatus::MissingPoint&&!Missing.Samples[0].Value.IsSet()&&!Missing.Samples[0].ScenePosition.IsSet());
    R.Probe.Method=EStudioProbeMethod::Interpolated;R.Probe.PointId.Reset();R.Probe.A=FVector(.12,0,.03);
    const auto Raw=StudioProbeSampling::Evaluate(R);
    TestTrue(TEXT("Raw source points do not invent interpolation"),Raw.Samples.Num()==1&&
        Raw.Samples[0].Status==EStudioProbeSampleStatus::NoInterpolation&&!Raw.Samples[0].Value.IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbeSurface,"Studio.Inspection.PublishedSurfaceSamples",ProbeFlags)
bool FStudioProbeSurface::RunTest(const FString&)
{
    const auto Raw=StudioRecordings::Import(ProbePointPath(),0,{});
    if(!TestTrue(*Raw.Error,Raw.Source.IsValid()&&Raw.Reference.IsSet()))return false;
    const auto Source=StudioRecordings::ImportReconstruction(*Raw.Reference,ProbeSurfacePath(),0,{});
    if(!TestTrue(*Source.Error,Source.Source.IsValid()))return false;
    const auto Expected=ReadProbeExpectations(FPaths::GetPath(ProbeSurfacePath())/TEXT("expected.json"));
    if(!TestTrue(TEXT("Independent original-HDF5 interpolation expectations available"),Expected.IsValid()))return false;
    double MaximumError=0;int32 Checked=0,FixedChecked=0;
    for(int32 Frame=0;Frame<3;++Frame)for(const auto& Field:Source.Source->Descriptor().Scalars)
    {
        auto R=ProbeRequest(Source,Frame,Field.Id);
        auto Fixed=ProbeRequest(Source,Frame,Field.Id==TEXT("pressure")?TEXT("velocity_magnitude"):TEXT("pressure"));Fixed.Probe.Field=Field.Id;
        for(const auto& Entry:Expected->GetArrayField(TEXT("queries")))
        {
            const auto Q=Entry->AsObject();const auto& XY=Q->GetArrayField(TEXT("position"));R.Probe.A=FVector(XY[0]->AsNumber(),0,XY[1]->AsNumber());
            for(const auto& Value:Q->GetArrayField(TEXT("expected")))
            {
                const auto E=Value->AsObject();if(int32(E->GetNumberField(TEXT("frame")))!=Frame||E->GetStringField(TEXT("field"))!=Field.Id)continue;
                const auto Result=StudioProbeSampling::Evaluate(R);
                if(!TestTrue(TEXT("Reconstructed sample retains a value and immutable identity"),Result.Status==EStudioProbeStatus::Ready&&
                    Result.Samples.Num()==1&&Result.Samples[0].Value.IsSet()&&Result.Identity.IsSet()))return false;
                MaximumError=FMath::Max(MaximumError,FMath::Abs(Result.Samples[0].Value.GetValue()-E->GetNumberField(TEXT("value"))));
                TestEqual(TEXT("Original source index, not ordinal, labels the sample"),Result.Identity->Frame.Index,Source.Source->EvaluateFrame(Frame).Index);
                TestEqual(TEXT("Original physical time labels the sample"),Result.Identity->Frame.Time,Source.Source->EvaluateFrame(Frame).Time);
                TestEqual(TEXT("Reconstruction identity is retained separately"),Result.Identity->ReconstructionSHA256,Expected->GetStringField(TEXT("reconstructionSHA256")));
                TestTrue(TEXT("Derived topology is explicit"),Result.Identity->Interpolation==EStudioFieldInterpolation::ReconstructedTriangles);++Checked;
                Fixed.Probe.A=R.Probe.A;const auto Independent=StudioProbeSampling::Evaluate(Fixed);
                if(!TestTrue(TEXT("Independent scalar samples the same reconstructed surface"),Independent.Status==EStudioProbeStatus::Ready&&
                    Independent.Samples.Num()==1&&Independent.Samples[0].Value.IsSet()&&Independent.Identity.IsSet()))return false;
                MaximumError=FMath::Max(MaximumError,FMath::Abs(Independent.Samples[0].Value.GetValue()-E->GetNumberField(TEXT("value"))));
                TestEqual(TEXT("Optional array preserves reconstruction identity"),Independent.Identity->ReconstructionSHA256,Result.Identity->ReconstructionSHA256);++FixedChecked;
            }
        }
    }
    TestEqual(TEXT("All independent interpolated values checked"),Checked,720);
    TestEqual(TEXT("All interpolation expectations also checked with another displayed scalar"),FixedChecked,720);
    TestTrue(TEXT("Probe interpolation agrees with original-HDF5 expectations"),MaximumError<1.e-8);
    auto R=ProbeRequest(Source,0,TEXT("pressure"));R.Probe.Kind=EStudioProbeKind::Line;
    R.Probe.A=FVector(.12,0,.03);R.Probe.B=FVector(.12,.1,.03);R.Probe.Samples=3;
    const auto Line=StudioProbeSampling::Evaluate(R);
    if(!TestEqual(TEXT("Line retains all requested sample positions"),Line.Samples.Num(),3))return false;
    TestTrue(TEXT("On-plane endpoint is available"),Line.Samples[0].Value.IsSet());
    TestTrue(TEXT("Off-plane samples are missing instead of extruded CFD"),!Line.Samples[1].Value.IsSet()&&!Line.Samples[2].Value.IsSet()&&
        Line.Samples[1].Status==EStudioProbeSampleStatus::OffPlane&&Line.Samples[2].Status==EStudioProbeSampleStatus::OffPlane);
    TestEqual(TEXT("Physical line position is retained for missing samples"),Line.Samples[2].DistanceAlongLineMeters,.1);
    R.Probe.Kind=EStudioProbeKind::Point;R.Probe.A=FVector(.04,0,0);
    const auto Hole=StudioProbeSampling::Evaluate(R);
    TestTrue(TEXT("Solid hole never yields a fabricated value"),Hole.Samples.Num()==1&&!Hole.Samples[0].Value.IsSet());
    R.Probe.Field=TEXT("density");const auto Unknown=StudioProbeSampling::Evaluate(R);
    TestTrue(TEXT("Missing source field is explicit"),Unknown.Status==EStudioProbeStatus::FieldUnavailable&&Unknown.Samples.IsEmpty());
    R.Probe.Field=TEXT("velocity_u");const auto Unloaded=StudioProbeSampling::Evaluate(R);
    TestTrue(TEXT("Additional field preserves the solid hole"),Unloaded.Status==EStudioProbeStatus::Ready&&Unloaded.Samples.Num()==1&&!Unloaded.Samples[0].Value.IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbePublication,"Studio.Inspection.FrozenFrameAndPublication",ProbeFlags)
bool FStudioProbePublication::RunTest(const FString&)
{
    auto Source=StudioRecordings::Import(ProbePointPath(),0,{});
    if(!TestTrue(*Source.Error,Source.Source.IsValid()&&Source.Reference.IsSet()))return false;
    auto First=ProbeRequest(Source,0,TEXT("pressure"));First.Probe.Method=EStudioProbeMethod::OriginalPoint;First.Probe.PointId=0;
    const auto Result=StudioProbeSampling::Evaluate(First);
    if(!TestTrue(TEXT("First pinned frame sampled"),Result.Samples.Num()==1&&Result.Samples[0].Value.IsSet()))return false;
    const double Original=Result.Samples[0].Value.GetValue();auto Current=First;
    Current.PresentationId=2;Current.Field=Source.Source->CaptureViewField(2,TEXT("pressure"),false);
    const auto Later=StudioProbeSampling::Evaluate(Current);
    TestFalse(TEXT("Rapid scrub cannot publish another capture's values"),Result.Matches(Current));
    TestTrue(TEXT("New frame publishes under its own identity"),Later.Matches(Current));
    TestEqual(TEXT("Later original step"),Later.Identity->Frame.Index,9000);
    TestEqual(TEXT("Earlier original step remains unchanged"),Result.Identity->Frame.Index,1001);
    TestFalse(TEXT("Source pressure evolves between actual frames"),FMath::IsNearlyEqual(Original,Later.Samples[0].Value.GetValue(),1.e-10));
    Current=First;Current.ProjectId=FGuid::NewGuid();TestFalse(TEXT("Project replacement rejects old results"),Result.Matches(Current));
    Current=First;Current.Probe.PointId=1;TestFalse(TEXT("Same-ID probe edit rejects old results"),Result.Matches(Current));
    Current=First;Current.DisplayedScalar=TEXT("velocity_u");TestFalse(TEXT("Displayed scalar replacement rejects old results"),Result.Matches(Current));
    Current=First;Current.Field=Source.Source->CaptureViewField(0,TEXT("pressure"),false);
    TestFalse(TEXT("Source/snapshot replacement cannot reuse an old result"),Result.Matches(Current));
    Current=First;Current.Probe.Source.MetadataSHA256=FString::ChrN(64,'a');
    const auto WrongSource=StudioProbeSampling::Evaluate(Current);
    TestTrue(TEXT("Dataset label alone cannot match changed scientific data"),WrongSource.Status==EStudioProbeStatus::SourceMismatch&&WrongSource.Samples.IsEmpty());
    auto Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    const auto Cancelled=StudioProbeSampling::Evaluate(First,Cancellation);
    TestTrue(TEXT("Cancelled work has no partial values to publish"),Cancelled.Status==EStudioProbeStatus::Cancelled&&Cancelled.Samples.IsEmpty()&&!Cancelled.Matches(First));
    Source.Source.Reset();Current.Field.Reset();
    const auto Retained=StudioProbeSampling::Evaluate(First);
    TestEqual(TEXT("Retained immutable frame remains queryable after source removal"),Retained.Samples[0].Value.GetValue(),Original);
    First.Probe.Field=TEXT("velocity_u");const auto OptionalRetained=StudioProbeSampling::Evaluate(First);
    if(!TestTrue(TEXT("Retained snapshot can load an optional field after source removal"),OptionalRetained.Status==EStudioProbeStatus::Ready&&
        OptionalRetained.Samples.Num()==1&&OptionalRetained.Samples[0].Value.IsSet()))return false;
    auto FixedCurrent=First;FixedCurrent.DisplayedScalar=TEXT("velocity_v");
    TestTrue(TEXT("Fixed field is independent of viewport scalar labels"),OptionalRetained.Matches(FixedCurrent));
    FixedCurrent.Probe.Field=TEXT("cell_volume");TestFalse(TEXT("Changing fixed scalar rejects the old answer"),OptionalRetained.Matches(FixedCurrent));
    const auto OptionalCancelled=StudioProbeSampling::Evaluate(First,Cancellation);
    TestTrue(TEXT("Cancelled optional read never publishes partial values"),OptionalCancelled.Status==EStudioProbeStatus::Cancelled&&OptionalCancelled.Samples.IsEmpty());

    const auto Legacy=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/MeshGraphNets_Airfoil/flow.bin"),0,{});
    if(!TestTrue(*Legacy.Error,Legacy.Source.IsValid()&&Legacy.Reference.IsSet()))return false;
    auto L=ProbeRequest(Legacy,0,TEXT("pressure"));L.Probe.A=FVector(.03250676393508911,0,.1194048523902893);
    const auto Value=StudioProbeSampling::Evaluate(L);
    if(!TestTrue(TEXT("Legacy mesh probe preserves its verified source identity"),Value.Samples.Num()==1&&Value.Samples[0].Value.IsSet()))return false;
    TestTrue(TEXT("Legacy pressure matches independent original decode"),FMath::IsNearlyEqual(Value.Samples[0].Value.GetValue(),98699.78125,1.e-5));
    TestTrue(TEXT("Original connectivity distinguished from reconstruction"),Value.Identity->Interpolation==EStudioFieldInterpolation::SourceTriangles);
    L.Probe.A.Y=.001;const auto Extruded=StudioProbeSampling::Evaluate(L);
    TestTrue(TEXT("Legacy display extrusion cannot masquerade as a physical probe"),Extruded.Samples.Num()==1&&
        Extruded.Samples[0].Status==EStudioProbeSampleStatus::OffPlane&&!Extruded.Samples[0].Value.IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbeOptionalIdentity,"Studio.Inspection.OptionalScalarIdentityGuard",ProbeFlags)
bool FStudioProbeOptionalIdentity::RunTest(const FString&)
{
    const auto Source=StudioRecordings::Import(ProbePointPath(),0,{});
    if(!TestTrue(*Source.Error,Source.Source.IsValid()&&Source.Reference.IsSet()))return false;
    auto R=ProbeRequest(Source,0,TEXT("velocity_magnitude"));R.Probe.Field=TEXT("pressure");
    R.Probe.Method=EStudioProbeMethod::OriginalPoint;R.Probe.PointId=100;
    const auto Loaded=Source.Source->CaptureViewField(0,TEXT("pressure"),false);
    const auto Visible=MakeShared<FProbeIdentityFixture,ESPMode::ThreadSafe>(R.Field);R.Field=Visible;Visible->Loaded=Loaded;
    const auto Matched=StudioProbeSampling::Evaluate(R);
    if(!TestTrue(TEXT("Matching real pressure snapshot is accepted"),Matched.Status==EStudioProbeStatus::Ready&&Matched.Samples.Num()==1&&Matched.Samples[0].Value.IsSet()))return false;
    TestEqual(TEXT("Accepted value is the independent original source pressure"),Matched.Samples[0].Value.GetValue(),8.365919113);
    for(int32 Change=0;Change<11;++Change)
    {
        const auto Changed=MakeShared<FProbeIdentityFixture,ESPMode::ThreadSafe>(Loaded);Changed->ChangedIdentity=Loaded->Identity();
        auto& I=Changed->ChangedIdentity.GetValue();
        switch(Change)
        {
        case 0:I.Dataset+=TEXT("-other");break;
        case 1:I.MetadataSHA256=FString::ChrN(64,'a');break;
        case 2:I.PayloadSHA256=FString::ChrN(64,'b');break;
        case 3:I.ReconstructionSHA256=FString::ChrN(64,'c');break;
        case 4:++I.Ordinal;break;
        case 5:++I.Frame.Index;break;
        case 6:I.Frame.Time+=.5;break;
        case 7:I.SpatialDimensions=3;break;
        case 8:I.SourceOffset.X+=.01;break;
        case 9:I.Interpolation=EStudioFieldInterpolation::ReconstructedGrid;break;
        case 10:Changed->ChangedScalar=Loaded->Scalar(TEXT("pressure"));Changed->ChangedScalar->Unit=TEXT("other");break;
        }
        Visible->Loaded=Changed;const auto Rejected=StudioProbeSampling::Evaluate(R);
        TestTrue(*FString::Printf(TEXT("Identity mismatch %d publishes no values"),Change),Rejected.Status==EStudioProbeStatus::FrameMismatch&&Rejected.Samples.IsEmpty());
    }
    R.Probe.Field=TEXT("density");const auto Missing=StudioProbeSampling::Evaluate(R);
    TestTrue(TEXT("Absent field is rejected before optional loading, never defaulted"),Missing.Status==EStudioProbeStatus::FieldUnavailable&&Missing.Samples.IsEmpty());
    return true;
}
#endif
