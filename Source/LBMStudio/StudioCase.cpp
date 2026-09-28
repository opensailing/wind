#include "StudioCase.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    using FObject = TSharedPtr<FJsonObject>;
    using FValues = TArray<TSharedPtr<FJsonValue>>;
    constexpr double MaxInteger = 1.e12; // Exactly representable in JSON's double storage.

    void IdField(const FObject& O, const TCHAR* Key, const FGuid& Id)
    { O->SetStringField(Key, Id.IsValid() ? Id.ToString() : FString()); }
    bool ReadId(const FObject& O, const TCHAR* Key, FGuid& Id, bool bOptional = false)
    {
        FString Text;
        if (!O->TryGetStringField(Key, Text)) return false;
        if (Text.IsEmpty() && bOptional) { Id.Invalidate(); return true; }
        return FGuid::Parse(Text, Id) && Id.IsValid();
    }
    FValues Components(std::initializer_list<double> Values)
    {
        FValues Result;
        for (double V : Values) Result.Add(MakeShared<FJsonValueNumber>(V));
        return Result;
    }
    void VectorField(const FObject& O, const TCHAR* Key, const FVector& V)
    { O->SetArrayField(Key, Components({V.X,V.Y,V.Z})); }
    bool ReadComponents(const FObject& O, const TCHAR* Key, double* Result, int32 Count)
    {
        const FValues* Values;
        if (!O->TryGetArrayField(Key, Values) || Values->Num() != Count) return false;
        for (int32 I = 0; I < Count; ++I)
            if (!(*Values)[I]->TryGetNumber(Result[I]) || !FMath::IsFinite(Result[I])) return false;
        return true;
    }
    bool ReadVector(const FObject& O, const TCHAR* Key, FVector& V)
    {
        double Values[3];
        if (!ReadComponents(O, Key, Values, 3)) return false;
        V = FVector(Values[0],Values[1],Values[2]); return true;
    }
    void OptionalField(const FObject& O, const TCHAR* Key, const TOptional<double>& V)
    {
        if (V.IsSet()) O->SetNumberField(Key,V.GetValue());
        else O->SetField(Key,MakeShared<FJsonValueNull>());
    }
    void OptionalVector(const FObject& O, const TCHAR* Key, const TOptional<FVector>& V)
    {
        if (V.IsSet()) VectorField(O,Key,V.GetValue());
        else O->SetField(Key,MakeShared<FJsonValueNull>());
    }
    bool ReadOptional(const FObject& O, const TCHAR* Key, TOptional<double>& V)
    {
        const auto Value = O->TryGetField(Key);
        if (!Value) return false;
        if (Value->Type == EJson::Null) { V.Reset(); return true; }
        double Number;
        if (!Value->TryGetNumber(Number) || !FMath::IsFinite(Number)) return false;
        V = Number; return true;
    }
    bool ReadOptionalVector(const FObject& O, const TCHAR* Key, TOptional<FVector>& V)
    {
        const auto Value = O->TryGetField(Key);
        if (!Value) return false;
        if (Value->Type == EJson::Null) { V.Reset(); return true; }
        FVector Vector;
        if (!ReadVector(O,Key,Vector)) return false;
        V = Vector; return true;
    }
    bool ReadInteger(const FObject& O, const TCHAR* Key, int64& Value)
    {
        double Number;
        if (!O->TryGetNumberField(Key,Number) || !FMath::IsFinite(Number) ||
            Number < 0 || Number > MaxInteger || Number != FMath::FloorToDouble(Number)) return false;
        Value = int64(Number); return true;
    }
    bool GoodName(const FString& Name) { return !Name.TrimStartAndEnd().IsEmpty() && Name.Len() <= 120; }
    bool GoodVector(const FVector& V)
    { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z) && V.GetAbsMax() <= 1.e8; }
    bool Positive(const TOptional<double>& V)
    { return !V.IsSet() || (FMath::IsFinite(V.GetValue()) && V.GetValue() > 0 && V.GetValue() <= 1.e12); }
    bool Finite(const TOptional<double>& V)
    { return !V.IsSet() || (FMath::IsFinite(V.GetValue()) && FMath::Abs(V.GetValue()) <= 1.e12); }
    bool GoodOptionalVector(const TOptional<FVector>& V) { return !V.IsSet() || GoodVector(V.GetValue()); }
    const TCHAR* BoundaryName(EStudioBoundaryType Type)
    {
        switch (Type)
        {
        case EStudioBoundaryType::Unassigned: return TEXT("unassigned");
        case EStudioBoundaryType::VelocityInlet: return TEXT("velocity-inlet");
        case EStudioBoundaryType::PressureOutlet: return TEXT("pressure-outlet");
        case EStudioBoundaryType::NoSlip: return TEXT("no-slip");
        case EStudioBoundaryType::Slip: return TEXT("slip");
        case EStudioBoundaryType::Symmetry: return TEXT("symmetry");
        default: return TEXT("invalid");
        }
    }
    bool ReadBoundaryType(const FObject& O, EStudioBoundaryType& Type)
    {
        FString Text;
        if (!O->TryGetStringField(TEXT("type"),Text)) return false;
        for (int32 I = 0; I <= int32(EStudioBoundaryType::Symmetry); ++I)
            if (Text == BoundaryName(EStudioBoundaryType(I))) { Type = EStudioBoundaryType(I); return true; }
        return false;
    }
    template<typename T, typename F>
    bool ReadItems(const FObject& O, const TCHAR* Key, TArray<T>& Out, int32 Limit, F Read)
    {
        const FValues* Values;
        if (!O->TryGetArrayField(Key,Values) || Values->Num() > Limit) return false;
        for (const auto& Value : *Values)
        {
            const FObject* Object; T Item;
            if (!Value->TryGetObject(Object) || !Read(*Object,Item)) return false;
            Out.Add(MoveTemp(Item));
        }
        return true;
    }
}

FStudioDomain::FStudioDomain()
{
    for (int32 I = 0; I < 6; ++I) Faces.Add(FGuid::NewGuid());
}

TSharedRef<FJsonObject> StudioCaseIO::ToJSON(const FStudioCaseDraft& D)
{
    auto O = MakeShared<FJsonObject>();
    IdField(O,TEXT("id"),D.Id); O->SetStringField(TEXT("name"),D.Name); O->SetNumberField(TEXT("revision"),double(D.Revision));
    auto Domain = MakeShared<FJsonObject>();
    IdField(Domain,TEXT("id"),D.Domain.Id); IdField(Domain,TEXT("fluidMaterialId"),D.Domain.FluidMaterialId);
    VectorField(Domain,TEXT("minMeters"),D.Domain.Min); VectorField(Domain,TEXT("maxMeters"),D.Domain.Max);
    FValues Faces;
    for (const auto& Id : D.Domain.Faces) Faces.Add(MakeShared<FJsonValueString>(Id.ToString()));
    Domain->SetArrayField(TEXT("faces"),Faces); O->SetObjectField(TEXT("domain"),Domain);
    FValues Materials;
    for (const auto& M : D.Materials)
    {
        auto Item = MakeShared<FJsonObject>(); IdField(Item,TEXT("id"),M.Id);
        Item->SetStringField(TEXT("name"),M.Name); Item->SetBoolField(TEXT("solid"),M.bSolid);
        OptionalField(Item,TEXT("densityKgM3"),M.Density); OptionalField(Item,TEXT("kinematicViscosityM2S"),M.KinematicViscosity);
        OptionalField(Item,TEXT("thermalConductivityWMK"),M.ThermalConductivity); OptionalField(Item,TEXT("specificHeatJKgK"),M.SpecificHeat);
        Materials.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("materials"),Materials);
    FValues Geometry;
    for (const auto& G : D.Geometry)
    {
        auto Item = MakeShared<FJsonObject>(); IdField(Item,TEXT("id"),G.Id); IdField(Item,TEXT("materialId"),G.MaterialId);
        Item->SetStringField(TEXT("name"),G.Name); Item->SetStringField(TEXT("sourcePath"),G.SourcePath);
        Item->SetStringField(TEXT("sourceSHA256"),G.SourceSHA256); Item->SetStringField(TEXT("format"),G.Format);
        Item->SetNumberField(TEXT("metersPerSourceUnit"),G.MetersPerSourceUnit);
        VectorField(Item,TEXT("translationMeters"),G.Translation); VectorField(Item,TEXT("scale"),G.Scale);
        Item->SetArrayField(TEXT("quaternionXYZW"),Components({G.Rotation.X,G.Rotation.Y,G.Rotation.Z,G.Rotation.W}));
        FValues Patches;
        for (const auto& P : G.Patches)
        {
            auto Patch = MakeShared<FJsonObject>(); IdField(Patch,TEXT("id"),P.Id); Patch->SetStringField(TEXT("name"),P.Name);
            Patches.Add(MakeShared<FJsonValueObject>(Patch));
        }
        Item->SetArrayField(TEXT("patches"),Patches); Geometry.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("geometry"),Geometry);
    FValues Boundaries;
    for (const auto& B : D.Boundaries)
    {
        auto Item = MakeShared<FJsonObject>(); IdField(Item,TEXT("id"),B.Id); IdField(Item,TEXT("targetId"),B.TargetId);
        Item->SetStringField(TEXT("name"),B.Name); Item->SetStringField(TEXT("type"),BoundaryName(B.Type));
        OptionalVector(Item,TEXT("velocityMS"),B.Velocity); OptionalField(Item,TEXT("pressurePa"),B.Pressure);
        OptionalField(Item,TEXT("temperatureK"),B.Temperature); Boundaries.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("boundaries"),Boundaries);
    const auto& S = D.Setup; auto Setup = MakeShared<FJsonObject>();
    OptionalVector(Setup,TEXT("inletVelocityMS"),S.InletVelocity); OptionalField(Setup,TEXT("outletPressurePa"),S.OutletPressure);
    OptionalField(Setup,TEXT("referenceLengthM"),S.ReferenceLength); OptionalField(Setup,TEXT("referenceDensityKgM3"),S.ReferenceDensity);
    OptionalField(Setup,TEXT("reynoldsNumber"),S.ReynoldsNumber); OptionalField(Setup,TEXT("relaxationTime"),S.RelaxationTime);
    OptionalField(Setup,TEXT("timeStepS"),S.TimeStep); OptionalField(Setup,TEXT("cflTarget"),S.CFLTarget);
    Setup->SetArrayField(TEXT("latticeResolution"),Components({double(S.LatticeResolution.X),double(S.LatticeResolution.Y),double(S.LatticeResolution.Z)}));
    Setup->SetStringField(TEXT("backendId"),S.BackendId); Setup->SetStringField(TEXT("collisionModel"),S.CollisionModel);
    Setup->SetStringField(TEXT("turbulenceModel"),S.TurbulenceModel); Setup->SetBoolField(TEXT("thermal"),S.bThermal);
    Setup->SetNumberField(TEXT("maxSteps"),double(S.MaxSteps)); OptionalField(Setup,TEXT("maxPhysicalTimeS"),S.MaxPhysicalTime);
    Setup->SetNumberField(TEXT("outputInterval"),double(S.OutputInterval)); Setup->SetBoolField(TEXT("checkpoints"),S.bCheckpoints);
    Setup->SetNumberField(TEXT("checkpointInterval"),double(S.CheckpointInterval)); O->SetObjectField(TEXT("setup"),Setup);
    return O;
}

bool StudioCaseIO::FromJSON(const FObject& O, FStudioCaseDraft& Out, FString& Error)
{
    Error = TEXT("Invalid case document. The current project has been kept.");
    FStudioCaseDraft D; const FObject *Domain, *Setup;
    if (!O || !ReadId(O,TEXT("id"),D.Id) || !O->TryGetStringField(TEXT("name"),D.Name) ||
        !ReadInteger(O,TEXT("revision"),D.Revision) || !O->TryGetObjectField(TEXT("domain"),Domain) ||
        !ReadId(*Domain,TEXT("id"),D.Domain.Id) || !ReadId(*Domain,TEXT("fluidMaterialId"),D.Domain.FluidMaterialId,true) ||
        !ReadVector(*Domain,TEXT("minMeters"),D.Domain.Min) || !ReadVector(*Domain,TEXT("maxMeters"),D.Domain.Max)) return false;
    const FValues* Faces;
    if (!(*Domain)->TryGetArrayField(TEXT("faces"),Faces) || Faces->Num() != 6) return false;
    D.Domain.Faces.Reset();
    for (const auto& Face : *Faces)
    {
        FString Text; FGuid Id;
        if (!Face->TryGetString(Text) || !FGuid::Parse(Text,Id)) return false;
        D.Domain.Faces.Add(Id);
    }
    if (!ReadItems(O,TEXT("materials"),D.Materials,256,[](const FObject& M, FStudioMaterial& V)
    {
        return ReadId(M,TEXT("id"),V.Id) && M->TryGetStringField(TEXT("name"),V.Name) && M->TryGetBoolField(TEXT("solid"),V.bSolid) &&
            ReadOptional(M,TEXT("densityKgM3"),V.Density) && ReadOptional(M,TEXT("kinematicViscosityM2S"),V.KinematicViscosity) &&
            ReadOptional(M,TEXT("thermalConductivityWMK"),V.ThermalConductivity) && ReadOptional(M,TEXT("specificHeatJKgK"),V.SpecificHeat);
    })) return false;
    if (!ReadItems(O,TEXT("geometry"),D.Geometry,1024,[](const FObject& G, FStudioGeometryAsset& V)
    {
        double Q[4];
        if (!ReadId(G,TEXT("id"),V.Id) || !ReadId(G,TEXT("materialId"),V.MaterialId,true) || !G->TryGetStringField(TEXT("name"),V.Name) ||
            !G->TryGetStringField(TEXT("sourcePath"),V.SourcePath) || !G->TryGetStringField(TEXT("sourceSHA256"),V.SourceSHA256) ||
            !G->TryGetStringField(TEXT("format"),V.Format) || !G->TryGetNumberField(TEXT("metersPerSourceUnit"),V.MetersPerSourceUnit) ||
            !ReadVector(G,TEXT("translationMeters"),V.Translation) || !ReadVector(G,TEXT("scale"),V.Scale) ||
            !ReadComponents(G,TEXT("quaternionXYZW"),Q,4)) return false;
        V.Rotation = FQuat(Q[0],Q[1],Q[2],Q[3]);
        return ReadItems(G,TEXT("patches"),V.Patches,4096,[](const FObject& P, FStudioSurfacePatch& Patch)
        { return ReadId(P,TEXT("id"),Patch.Id) && P->TryGetStringField(TEXT("name"),Patch.Name); });
    })) return false;
    if (!ReadItems(O,TEXT("boundaries"),D.Boundaries,4096,[](const FObject& B, FStudioBoundaryCondition& V)
    {
        return ReadId(B,TEXT("id"),V.Id) && ReadId(B,TEXT("targetId"),V.TargetId) && B->TryGetStringField(TEXT("name"),V.Name) &&
            ReadBoundaryType(B,V.Type) && ReadOptionalVector(B,TEXT("velocityMS"),V.Velocity) &&
            ReadOptional(B,TEXT("pressurePa"),V.Pressure) && ReadOptional(B,TEXT("temperatureK"),V.Temperature);
    })) return false;
    if (!O->TryGetObjectField(TEXT("setup"),Setup)) return false;
    auto& S = D.Setup; const auto& J = *Setup; double Resolution[3];
    if (!ReadOptionalVector(J,TEXT("inletVelocityMS"),S.InletVelocity) || !ReadOptional(J,TEXT("outletPressurePa"),S.OutletPressure) ||
        !ReadOptional(J,TEXT("referenceLengthM"),S.ReferenceLength) || !ReadOptional(J,TEXT("referenceDensityKgM3"),S.ReferenceDensity) ||
        !ReadOptional(J,TEXT("reynoldsNumber"),S.ReynoldsNumber) || !ReadOptional(J,TEXT("relaxationTime"),S.RelaxationTime) ||
        !ReadOptional(J,TEXT("timeStepS"),S.TimeStep) || !ReadOptional(J,TEXT("cflTarget"),S.CFLTarget) ||
        !ReadComponents(J,TEXT("latticeResolution"),Resolution,3) || !J->TryGetStringField(TEXT("backendId"),S.BackendId) ||
        !J->TryGetStringField(TEXT("collisionModel"),S.CollisionModel) || !J->TryGetStringField(TEXT("turbulenceModel"),S.TurbulenceModel) ||
        !J->TryGetBoolField(TEXT("thermal"),S.bThermal) || !ReadInteger(J,TEXT("maxSteps"),S.MaxSteps) ||
        !ReadOptional(J,TEXT("maxPhysicalTimeS"),S.MaxPhysicalTime) || !ReadInteger(J,TEXT("outputInterval"),S.OutputInterval) ||
        !J->TryGetBoolField(TEXT("checkpoints"),S.bCheckpoints) || !ReadInteger(J,TEXT("checkpointInterval"),S.CheckpointInterval)) return false;
    for (double V : Resolution) if (V < 1 || V > 1048576 || V != FMath::FloorToDouble(V)) return false;
    S.LatticeResolution = FIntVector(int32(Resolution[0]),int32(Resolution[1]),int32(Resolution[2]));
    if (!Validate(D,Error)) return false;
    Out = MoveTemp(D); Error.Empty(); return true;
}

FString StudioCaseIO::Serialize(const FStudioCaseDraft& Draft)
{
    FString Text; FJsonSerializer::Serialize(ToJSON(Draft),TJsonWriterFactory<>::Create(&Text)); return Text;
}

bool StudioCaseIO::Validate(const FStudioCaseDraft& D, FString& Error)
{
    auto Fail = [&Error](const TCHAR* Text) { Error = Text; return false; };
    TSet<FGuid> Ids;
    auto Unique = [&Ids](const FGuid& Id) { if (!Id.IsValid() || Ids.Contains(Id)) return false; Ids.Add(Id); return true; };
    if (!Unique(D.Id) || !Unique(D.Domain.Id) || !GoodName(D.Name) || D.Revision < 0 || D.Revision > MaxInteger)
        return Fail(TEXT("Case identity, name or revision is invalid."));
    if (!GoodVector(D.Domain.Min) || !GoodVector(D.Domain.Max) ||
        D.Domain.Min.X >= D.Domain.Max.X || D.Domain.Min.Y >= D.Domain.Max.Y || D.Domain.Min.Z >= D.Domain.Max.Z || D.Domain.Faces.Num() != 6)
        return Fail(TEXT("Domain bounds must enclose a nonzero volume and have six faces."));
    TSet<FGuid> Targets;
    for (const auto& Id : D.Domain.Faces)
    { if (!Unique(Id)) return Fail(TEXT("Domain face identities must be unique.")); Targets.Add(Id); }
    if (D.Materials.Num() > 256 || D.Geometry.Num() > 1024 || D.Boundaries.Num() > 4096)
        return Fail(TEXT("This case exceeds the document's object limits."));
    TSet<FGuid> Materials;
    for (const auto& M : D.Materials)
    {
        if (!Unique(M.Id) || !GoodName(M.Name) || !Positive(M.Density) || !Positive(M.KinematicViscosity) ||
            !Positive(M.ThermalConductivity) || !Positive(M.SpecificHeat)) return Fail(TEXT("Material identity or property is invalid."));
        Materials.Add(M.Id);
    }
    if (D.Domain.FluidMaterialId.IsValid())
    {
        const auto* Fluid = D.Materials.FindByPredicate([&D](const auto& M) { return M.Id == D.Domain.FluidMaterialId; });
        if (!Fluid || Fluid->bSolid) return Fail(TEXT("The domain must reference an existing fluid material."));
    }
    for (const auto& G : D.Geometry)
    {
        bool bHash = G.SourceSHA256.Len() == 64;
        for (TCHAR C : G.SourceSHA256) bHash &= FChar::IsHexDigit(C);
        if (!Unique(G.Id) || !GoodName(G.Name) || G.SourcePath.IsEmpty() || G.SourcePath.Len() > 4096 || !bHash ||
            (G.Format != TEXT("stl") && G.Format != TEXT("obj")) || !FMath::IsFinite(G.MetersPerSourceUnit) ||
            G.MetersPerSourceUnit <= 0 || G.MetersPerSourceUnit > 1.e6 || !GoodVector(G.Translation) || !GoodVector(G.Scale) ||
            G.Scale.X <= 0 || G.Scale.Y <= 0 || G.Scale.Z <= 0 || G.Rotation.ContainsNaN() ||
            !FMath::IsNearlyEqual(G.Rotation.SizeSquared(),1.,1.e-5) || G.Patches.Num() > 4096)
            return Fail(TEXT("Geometry identity, source reference, units or transform is invalid."));
        if (G.MaterialId.IsValid() && !Materials.Contains(G.MaterialId)) return Fail(TEXT("Geometry references a missing material."));
        for (const auto& P : G.Patches)
        { if (!Unique(P.Id) || !GoodName(P.Name)) return Fail(TEXT("Surface patch identity or name is invalid.")); Targets.Add(P.Id); }
    }
    TSet<FGuid> Assigned;
    for (const auto& B : D.Boundaries)
    {
        if (!Unique(B.Id) || !GoodName(B.Name) || !Targets.Contains(B.TargetId) || Assigned.Contains(B.TargetId) ||
            uint8(B.Type) > uint8(EStudioBoundaryType::Symmetry) || !GoodOptionalVector(B.Velocity) || !Finite(B.Pressure) ||
            !Finite(B.Temperature) || (B.Temperature.IsSet() && B.Temperature.GetValue() < 0))
            return Fail(TEXT("Boundary has an invalid value, missing target, or conflicting assignment."));
        Assigned.Add(B.TargetId);
    }
    const auto& S = D.Setup;
    if (!GoodOptionalVector(S.InletVelocity) || !Finite(S.OutletPressure) || !Positive(S.ReferenceLength) ||
        !Positive(S.ReferenceDensity) || !Positive(S.ReynoldsNumber) || !Positive(S.RelaxationTime) ||
        !Positive(S.TimeStep) || !Positive(S.CFLTarget) || !Positive(S.MaxPhysicalTime) ||
        S.LatticeResolution.GetMin() < 1 || S.LatticeResolution.GetMax() > 1048576 ||
        S.BackendId.Len() > 128 || S.CollisionModel.Len() > 128 || S.TurbulenceModel.Len() > 128 ||
        S.MaxSteps < 1 || S.MaxSteps > MaxInteger || S.OutputInterval < 1 || S.OutputInterval > MaxInteger ||
        S.CheckpointInterval < 1 || S.CheckpointInterval > MaxInteger)
        return Fail(TEXT("Case setup contains an invalid value. Solver-specific constraints have not been checked."));
    Error.Empty(); return true;
}

FStudioRunRecord FStudioRunRecord::Recording(const FString& InName, const FString& InDatasetId,bool bPublished)
{
    FStudioRunRecord R; R.Name=InName; R.DatasetId=InDatasetId;
    R.Origin=bPublished?EStudioRunOrigin::PublishedRecording:EStudioRunOrigin::ImportedRecording;
    R.BackendId=bPublished?TEXT("published-recording"):TEXT("imported-recording"); return R;
}
FStudioRunRecord FStudioRunRecord::Capture(const FString& InName, const FStudioCaseDraft& Draft, EStudioRunOrigin InOrigin)
{
    FStudioRunRecord R; R.Name = InName; R.Origin = InOrigin; R.BackendId = Draft.Setup.BackendId;
    R.Configuration = MakeShared<const FStudioCaseDraft>(Draft); return R;
}
TSharedRef<FJsonObject> FStudioRunRecord::ToJSON() const
{
    auto O = MakeShared<FJsonObject>(); IdField(O,TEXT("id"),Id); O->SetStringField(TEXT("name"),Name);
    O->SetStringField(TEXT("origin"),Origin == EStudioRunOrigin::PublishedRecording ? TEXT("recording") :
        Origin == EStudioRunOrigin::ImportedRecording ? TEXT("imported-recording") :
        Origin == EStudioRunOrigin::ControlHarness ? TEXT("harness") : Origin == EStudioRunOrigin::Solver ? TEXT("solver") : TEXT("invalid"));
    O->SetStringField(TEXT("backendId"),BackendId); O->SetStringField(TEXT("datasetId"),DatasetId);
    if (Configuration) O->SetObjectField(TEXT("configuration"),StudioCaseIO::ToJSON(*Configuration));
    else O->SetField(TEXT("configuration"),MakeShared<FJsonValueNull>());
    return O;
}
bool FStudioRunRecord::FromJSON(const FObject& O, FStudioRunRecord& Out, FString& Error)
{
    Error = TEXT("Invalid run record. The current project has been kept.");
    FStudioRunRecord R; FString Kind;
    if (!O || !ReadId(O,TEXT("id"),R.Id) || !O->TryGetStringField(TEXT("name"),R.Name) || !GoodName(R.Name) ||
        !O->TryGetStringField(TEXT("origin"),Kind) || !O->TryGetStringField(TEXT("backendId"),R.BackendId) ||
        R.BackendId.IsEmpty() || R.BackendId.Len() > 128 || !O->TryGetStringField(TEXT("datasetId"),R.DatasetId) || R.DatasetId.Len() > 256) return false;
    const auto Config = O->TryGetField(TEXT("configuration"));
    if (!Config) return false;
    if (Kind == TEXT("recording") || Kind == TEXT("imported-recording"))
    {
        R.Origin=Kind==TEXT("recording")?EStudioRunOrigin::PublishedRecording:EStudioRunOrigin::ImportedRecording;
        if (R.DatasetId.IsEmpty() || R.BackendId != (Kind==TEXT("recording")?TEXT("published-recording"):TEXT("imported-recording")) || Config->Type != EJson::Null) return false;
    }
    else
    {
        if (Kind != TEXT("harness") && Kind != TEXT("solver")) return false;
        R.Origin = Kind == TEXT("harness") ? EStudioRunOrigin::ControlHarness : EStudioRunOrigin::Solver;
        const FObject* C; FStudioCaseDraft Draft;
        if (!Config->TryGetObject(C) || !StudioCaseIO::FromJSON(*C,Draft,Error)) return false;
        if (Draft.Setup.BackendId != R.BackendId)
        { Error=TEXT("Run backend does not match its captured case."); return false; }
        R.Configuration = MakeShared<const FStudioCaseDraft>(MoveTemp(Draft));
    }
    Out = MoveTemp(R); Error.Empty(); return true;
}
