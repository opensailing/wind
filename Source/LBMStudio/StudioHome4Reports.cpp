#include "StudioHome4Reports.h"
#include "StudioHome4ReportPlots.h"
#include "StudioProject.h"
#include "StudioHome4Recipes.h"
#include "StudioHome4Validation.h"
#include "StudioHome4SpatialDiagnostics.h"
#include "StudioFileDialog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#define UI UI_HOME4_REPORT_COMPLETION
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioHome4ReportTelemetryPrivate
{
    void Optional(const TSharedRef<FJsonObject>& O,const TCHAR* Key,const TOptional<double>& V)
    {if(V)O->SetNumberField(Key,*V);else O->SetField(Key,MakeShared<FJsonValueNull>());}
    void Unit(const TSharedRef<FJsonObject>& O,const TCHAR* Key,const TOptional<EStudioHome4UnitDisplay>& V)
    {
        if(!V){O->SetField(Key,MakeShared<FJsonValueNull>());return;}
        O->SetStringField(Key,*V==EStudioHome4UnitDisplay::Physical?TEXT("physical"):*V==EStudioHome4UnitDisplay::Nondimensional?TEXT("nondimensional"):TEXT("lattice"));
    }
    TSharedRef<FJsonObject> Normalization(const FStudioHome4Normalization& N)
    {
        auto O=MakeShared<FJsonObject>();Optional(O,TEXT("force_divisor"),N.ForceDivisor);Optional(O,TEXT("moment_divisor"),N.MomentDivisor);
        O->SetStringField(TEXT("force_label"),N.ForceLabel);O->SetStringField(TEXT("moment_label"),N.MomentLabel);return O;
    }
    TSharedRef<FJsonObject> Metadata(const FStudioHome4SourceMetadata& M)
    {
        auto O=MakeShared<FJsonObject>();Unit(O,TEXT("force_units"),M.ForceUnits);Unit(O,TEXT("energy_units"),M.EnergyUnits);
        Unit(O,TEXT("velocity_units"),M.VelocityUnits);Unit(O,TEXT("length_units"),M.LengthUnits);
        auto U=MakeShared<FJsonObject>();Optional(U,TEXT("dx_m"),M.UnitMap.Units.DxMeters);Optional(U,TEXT("dt_s"),M.UnitMap.Units.DtSeconds);
        Optional(U,TEXT("rho_kg_m3"),M.UnitMap.Units.DensityReferenceKgM3);Optional(U,TEXT("rho_lattice"),M.UnitMap.Fluids.RhoHeavy);
        Optional(U,TEXT("length_cells"),M.UnitMap.Reference.LengthCells);Optional(U,TEXT("time_steps"),M.UnitMap.Reference.TimeSteps);
        Optional(U,TEXT("speed_cells_step"),M.UnitMap.Reference.SpeedCellsPerStep);O->SetObjectField(TEXT("unit_map"),U);
        O->SetObjectField(TEXT("normalization"),Normalization(M.Normalization));auto Bodies=MakeShared<FJsonObject>();
        for(const auto& B:M.BodyNormalizations)Bodies->SetObjectField(B.Key,Normalization(B.Value));
        O->SetObjectField(TEXT("body_normalizations"),Bodies);
        O->SetStringField(TEXT("divergence_convention"),M.DivergenceConvention);O->SetStringField(TEXT("divergence_unit"),M.DivergenceUnit);O->SetStringField(TEXT("divergence_domain"),M.DivergenceDomain);
        Optional(O,TEXT("device_peak_gbps"),M.DevicePeakGBps);O->SetStringField(TEXT("device_peak_source"),M.DevicePeakSource);
        TArray<TSharedPtr<FJsonValue>> Levels;for(int32 L:M.DeclaredLevels)Levels.Add(MakeShared<FJsonValueNumber>(L));O->SetArrayField(TEXT("declared_levels"),Levels);return O;
    }
    bool Verify(const FStudioHome4TelemetryStream& Stream,const FStudioHome4TelemetryProvenance* P,const FStudioProject& Project,FString& Error)
    {
        if(!P||!P->StreamRunId.IsValid()||!P->AttachedProjectId||!P->AttachedCaseId||
            !P->AttachedProjectId->IsValid()||!P->AttachedCaseId->IsValid()||
            *P->AttachedProjectId!=Project.Id||*P->AttachedCaseId!=Project.Draft.Id||P->SourceId.IsEmpty()||
            (P->OriginalRunId&&(!P->OriginalRunId->IsValid()||*P->OriginalRunId!=P->StreamRunId))||(!P->bImportedReplay&&!P->OriginalRunId))
        {Error=TEXT("Telemetry provenance must identify this project, case and original run or independent replay.");return false;}
        auto Clean=[](const FString& S,int32 Maximum)
        {if(S.Len()>Maximum)return false;for(TCHAR C:S)if(C<32||C==127)return false;return true;};
        if(!Clean(P->SourceId,256)||!Clean(P->SourcePath,4096))
        {Error=TEXT("Telemetry source identity or path is invalid.");return false;}
        if(!P->SourceSHA256.IsEmpty())
        {
            if(P->SourceSHA256.Len()!=64){Error=TEXT("Telemetry SHA256 must contain 64 hexadecimal digits.");return false;}
            for(TCHAR C:P->SourceSHA256)if(!((C>='0'&&C<='9')||(C>='a'&&C<='f')||(C>='A'&&C<='F')))
            {Error=TEXT("Telemetry SHA256 must contain 64 hexadecimal digits.");return false;}
        }
        if(P->bImportedReplay&&(P->SourcePath.IsEmpty()||P->SourceSHA256.IsEmpty()))
        {Error=TEXT("Imported telemetry requires its verified original file path and SHA256.");return false;}
        for(const auto& S:Stream.History())if(S.Source.RunId!=P->StreamRunId||S.Source.SourceId!=P->SourceId)
        {Error=TEXT("Telemetry provenance does not match the retained original measurements.");return false;}
        for(const auto& O:Stream.OutputEvents())if(O.Source.RunId!=P->StreamRunId||O.Source.SourceId!=P->SourceId)
        {Error=TEXT("Telemetry provenance does not match retained original output events.");return false;}
        Error.Empty();return true;
    }
}

FString StudioHome4Reports::EscapeLaTeX(const FString& V)
{
    FString Out;for(const TCHAR C:V)
    {
    switch(C)
    {
        case TCHAR('\\'):Out+=TEXT("\\textbackslash{}");break;
        case TCHAR('{'):case TCHAR('}'):case TCHAR('$'):case TCHAR('&'):case TCHAR('#'):case TCHAR('_'):case TCHAR('%'):Out.AppendChar(TCHAR('\\'));Out.AppendChar(C);break;
        case TCHAR('~'):Out+=TEXT("\\textasciitilde{}");break;case TCHAR('^'):Out+=TEXT("\\textasciicircum{}");break;
        case TCHAR(0x03bc):case TCHAR(0x00b5):Out+=TEXT("\\ensuremath{\\mu}");break;
        case TCHAR(0x03bd):Out+=TEXT("\\ensuremath{\\nu}");break;
        case TCHAR(0x03c1):Out+=TEXT("\\ensuremath{\\rho}");break;
        case TCHAR(0x03c3):Out+=TEXT("\\ensuremath{\\sigma}");break;
        case TCHAR(0x03c4):Out+=TEXT("\\ensuremath{\\tau}");break;
        case TCHAR(0x03be):Out+=TEXT("\\ensuremath{\\xi}");break;
        case TCHAR(0x03c0):Out+=TEXT("\\ensuremath{\\pi}");break;
        case TCHAR(0x0394):Out+=TEXT("\\ensuremath{\\Delta}");break;
        case TCHAR(0x03a9):Out+=TEXT("\\ensuremath{\\Omega}");break;
        case TCHAR(0x00b2):Out+=TEXT("\\ensuremath{{}^{2}}");break;
        case TCHAR(0x00b3):Out+=TEXT("\\ensuremath{{}^{3}}");break;
        case TCHAR(0x00b7):Out+=TEXT("\\ensuremath{\\cdot}");break;
        case TCHAR(0x00d7):Out+=TEXT("\\ensuremath{\\times}");break;
        case TCHAR(0x2212):Out+=TEXT("\\ensuremath{-}");break;
        case TCHAR(0x2264):Out+=TEXT("\\ensuremath{\\leq}");break;
        case TCHAR(0x2265):Out+=TEXT("\\ensuremath{\\geq}");break;
        case TCHAR('\t'):Out+=TEXT(" ");break;
        case TCHAR('\r'):break;case TCHAR('\n'):Out+=TEXT("\\newline{}");break;default:if(C>=32)Out.AppendChar(C);break;
    }
    }
    return Out;
}
bool StudioHome4Reports::Export(const FString& Parent,const FString& Folder,const FStudioProject& P,const FStudioHome4TelemetryStream* Telemetry,FString& OutPath,FString& Error,const FStudioHome4ReferenceEvidence* Evidence,const FStudioHome4SpatialEvidence* Spatial,const FStudioHome4TelemetryProvenance* TelemetryProvenance,const FStudioHome4ReportInputs* Inputs)
{
    if(!P.Draft.Home4.IsSet()){Error=TEXT("Apply a HOME4 recipe or configuration before exporting its report.");return false;}
    if(Folder.IsEmpty()||Folder.Len()>100||Folder==TEXT(".")||Folder==TEXT("..")||Folder.Contains(TEXT("/"))||Folder.Contains(TEXT("\\"))||Folder.Contains(TEXT(":")))
    {Error=TEXT("Use a report folder name of 1–100 characters without path separators.");return false;}
    const auto& S=P.Draft.Home4.GetValue();if(!StudioHome4Config::Validate(S,Error))return false;
    const bool HasTelemetry=Telemetry&&(!Telemetry->History().IsEmpty()||!Telemetry->OutputEvents().IsEmpty());
    if(HasTelemetry&&!StudioHome4ReportTelemetryPrivate::Verify(*Telemetry,TelemetryProvenance,P,Error))return false;
    if(Inputs&&Inputs->WindowStart.IsSet()!=Inputs->WindowEnd.IsSet())
    {Error=TEXT("Report window requires both explicit bounds.");return false;}
    if(Inputs&&Inputs->WindowStart&&(!FMath::IsFinite(*Inputs->WindowStart)||!FMath::IsFinite(*Inputs->WindowEnd)||*Inputs->WindowStart>=*Inputs->WindowEnd||Inputs->WindowAxis.IsEmpty()))
    {Error=TEXT("Report window requires increasing finite bounds and exact original axis label.");return false;}
    if(Inputs&&Inputs->OriginalTelemetryBytes&&!Inputs->OriginalTelemetryBytes->IsEmpty()&&HasTelemetry)
    {
        const auto& Bytes=*Inputs->OriginalTelemetryBytes;uint8 Digest[32];unsigned int Count=0;
        if(Bytes.Num()>64*1024*1024||EVP_Digest(Bytes.GetData(),Bytes.Num(),Digest,&Count,EVP_sha256(),nullptr)!=1||Count!=32||BytesToHex(Digest,Count).ToLower()!=TelemetryProvenance->SourceSHA256)
        {Error=TEXT("Original telemetry bytes differ from the displayed source hash or exceed its capture budget.");return false;}
        if(TelemetryProvenance->bCapturedPrefix&&TelemetryProvenance->CapturedByteCount!=Bytes.Num())
        {Error=TEXT("Live captured prefix byte count does not match its original retained bytes.");return false;}
    }
    if(Inputs&&Inputs->Validation&&(Inputs->Validation->ProjectId!=P.Id||Inputs->Validation->CaseId!=P.Draft.Id||Inputs->Validation->RecipeId!=S.RecipeId))
    {Error=TEXT("Ladder evidence belongs to another project/case/recipe scope.");return false;}
    FStudioHome4SpatialEvidence CheckedSpatial;
    if(Spatial)
    {
        if(!Spatial->AttachedProjectId.IsSet()||!Spatial->AttachedCaseId.IsSet()||
            !Spatial->AttachedProjectId->IsValid()||!Spatial->AttachedCaseId->IsValid()||
            *Spatial->AttachedProjectId!=P.Id||*Spatial->AttachedCaseId!=P.Draft.Id)
        {Error=TEXT("Spatial evidence is not attached to this project and case. Import it for the selected scope before reporting.");return false;}
        if(!StudioHome4SpatialDiagnostics::VerifyOriginal(*Spatial,CheckedSpatial,Error))return false;
        Spatial=&CheckedSpatial;
    }
    FStudioHome4ReferenceEvidence CheckedEvidence;
    if(Evidence)
    {
        if(Evidence->RecipeId!=S.RecipeId || !Evidence->AttachedProjectId.IsValid() || !Evidence->AttachedCaseId.IsValid() ||
            Evidence->AttachedProjectId!=P.Id || Evidence->AttachedCaseId!=P.Draft.Id)
        {Error=TEXT("Reference evidence is not attached to this project, case and recipe. Import it for the selected scope before reporting.");return false;}
        int64 Points=0;for(const auto& V:Evidence->Series)Points+=V.Actual.Num();
        if(Evidence->Series.Num()>16 || Points>200000){Error=TEXT("Report reference evidence exceeds its bounded import budget.");return false;}
        FStudioHome4ReferenceExpectation Expected;Expected.RecipeId=S.RecipeId;Expected.RunId=Evidence->RunId;
        if(!StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(*Evidence),Expected,CheckedEvidence,Error))return false;
        CheckedEvidence.SourcePath=Evidence->SourcePath;CheckedEvidence.SourceSHA256=Evidence->SourceSHA256;
        if(!Evidence->OriginalBytes.IsEmpty())
        {if(!StudioHome4Validation::VerifyOriginalBytes(*Evidence,Error))return false;CheckedEvidence.OriginalBytes=Evidence->OriginalBytes;
            CheckedEvidence.ActualOriginalBytes=Evidence->ActualOriginalBytes;CheckedEvidence.ReferenceOriginalBytes=Evidence->ReferenceOriginalBytes;}
        else CheckedEvidence.OriginalBytes.Reset();
        CheckedEvidence.AttachedProjectId=Evidence->AttachedProjectId;CheckedEvidence.AttachedCaseId=Evidence->AttachedCaseId;
        Evidence=&CheckedEvidence;
    }
    if(Inputs&&Inputs->bFigurePublication)
    {
        if(!Evidence||!Inputs->PublishedEvidence||!Inputs->PublicationAbsoluteTolerance||!Inputs->PublicationRelativeTolerance)
        {Error=TEXT("Figure publication requires original current/published evidence and explicit coefficient comparison tolerances.");return false;}
        if(!PublicationCheck(*Evidence,*Inputs->PublishedEvidence,*Inputs->PublicationAbsoluteTolerance,*Inputs->PublicationRelativeTolerance,Error))return false;
    }
    const FString Destination=Parent/Folder;
    if(IFileManager::Get().DirectoryExists(*Destination)||IFileManager::Get().FileExists(*Destination))
    {Error=TEXT("That report already exists. Choose a new folder name to preserve published results.");return false;}
    FStudioFileAccess Access(Parent);FString Stage;
    if(!StudioFileDialog::CreateExportStage(Parent,Stage,Error))return false;
    auto Abort=[&]{IFileManager::Get().DeleteDirectory(*Stage,false,true);return false;};
    auto Write=[&](const TCHAR* Name,const FString& Value)
    {if(FFileHelper::SaveStringToFile(Value,*(Stage/Name),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return true;Error=TEXT("Could not write report file ")+FString(Name);return false;};
    const auto* R=StudioHome4Recipes::Find(S.RecipeId);
    FStudioHome4DriverCommand Command;FString CommandError;
    FString CLI=TEXT("Driver argument contract is not supplied. Retained settings are in run_spec.json.");
    if((!R||R->Driver==TEXT("run_hull_speed.py"))&&StudioHome4Config::BuildHullDriverArgv(S,TEXT("python3"),TEXT("run_hull_speed.py"),Command,CommandError))
    {CLI=Command.Display;for(const auto& Missing:Command.MissingContracts)CLI+=TEXT("\n# ")+Missing;}
    auto Metadata=MakeShared<FJsonObject>();Metadata->SetStringField(TEXT("format"),TEXT("LBMStudio.Home4Report"));Metadata->SetNumberField(TEXT("version"),1);
    Metadata->SetStringField(TEXT("created_utc"),FDateTime::UtcNow().ToIso8601());Metadata->SetStringField(TEXT("project_id"),P.Id.ToString());Metadata->SetStringField(TEXT("case_id"),P.Draft.Id.ToString());
    Metadata->SetStringField(TEXT("recipe_id"),S.RecipeId);Metadata->SetStringField(TEXT("lineage_id"),S.LineageId);Metadata->SetStringField(TEXT("gate_status"),TEXT("not_evaluated"));
    Metadata->SetStringField(TEXT("field_source"),P.Dataset);Metadata->SetBoolField(TEXT("contains_computed_cfd"),false);
    Metadata->SetBoolField(TEXT("contains_imported_spatial_diagnostics"),Spatial!=nullptr);
    Metadata->SetObjectField(TEXT("run_spec"),StudioHome4Config::ToJSON(S));
    Metadata->SetBoolField(TEXT("contains_telemetry"),HasTelemetry);
    Metadata->SetBoolField(TEXT("contains_imported_telemetry"),HasTelemetry&&TelemetryProvenance->bImportedReplay);
    FString TeX=StudioHome4ReportPlots::TeXPreamble()+TEXT("\\section*{")+EscapeLaTeX(P.Name)+TEXT("}\n");
    TeX+=TEXT("Recipe: ")+EscapeLaTeX(R?R->Name:TEXT("Unspecified"))+TEXT("\\\\\nLineage: ")+EscapeLaTeX(S.LineageId)+(Evidence?TEXT("\\\\\nIdentified original evidence: ")+EscapeLaTeX(Evidence->GateStatus())+TEXT(". Current draft has no transferred gate.\\\\\n"):TEXT("\\\\\nValidation gate: not evaluated. Reference evidence is not supplied.\\\\\n"));
    TeX+=TEXT("Configuration is stored in run\\_spec.json. The command is a reproducibility preview; the development adapter computes no CFD.\\\\\n");
    if(Evidence&&Evidence->RecipeCoverage()==TEXT("unknown"))TeX+=TEXT("Original recipe coverage is unknown; only supplied series are compared.\\\\\n");
    if(Spatial)
    {
        auto Provenance=MakeShared<FJsonObject>();
        Provenance->SetStringField(TEXT("original_file"),TEXT("spatial-diagnostics.json"));
        Provenance->SetStringField(TEXT("source_path"),Spatial->SourcePath);Provenance->SetStringField(TEXT("source_sha256"),Spatial->SourceSHA256);
        Provenance->SetStringField(TEXT("source_id"),Spatial->SourceId);Provenance->SetStringField(TEXT("run_id"),Spatial->RunId.ToString());
        Provenance->SetStringField(TEXT("attached_project_id"),P.Id.ToString());Provenance->SetStringField(TEXT("attached_case_id"),P.Draft.Id.ToString());
        Provenance->SetStringField(TEXT("coordinate_unit"),Spatial->CoordinateUnit);Provenance->SetStringField(TEXT("gate_status"),TEXT("not_evaluated"));
        Provenance->SetNumberField(TEXT("original_bytes"),Spatial->OriginalBytes.Num());
        Metadata->SetObjectField(TEXT("imported_spatial_diagnostics"),Provenance);
        if(!FFileHelper::SaveArrayToFile(Spatial->OriginalBytes,*(Stage/TEXT("spatial-diagnostics.json"))))
        {Error=TEXT("Could not write original spatial diagnostics.");return Abort();}
        TeX+=TEXT("\\subsection*{Imported spatial diagnostics}\nOriginal run: ")+EscapeLaTeX(Spatial->RunId.ToString())+
            TEXT("\\\\\nSource: ")+EscapeLaTeX(Spatial->SourceId)+TEXT("\\\\\nSHA256: ")+EscapeLaTeX(Spatial->SourceSHA256)+
            TEXT("\\\\\nExact original bytes are retained in spatial-diagnostics.json. The imported source does not validate the current draft or establish recipe coverage.\\\\\n");
    }
    if(R)TeX+=TEXT("Reference: ")+EscapeLaTeX(R->Reference)+TEXT("\\\\\nGate: ")+EscapeLaTeX(R->Gate)+TEXT("\\\\\n");
    const auto Derived=StudioHome4Config::Derive(S);
    TeX+=TEXT("\\subsection*{Configuration checks}\n");for(const auto& I:Derived.Issues)TeX+=EscapeLaTeX(I.Message)+TEXT("\\\\\n");
    if(HasTelemetry)
    {
        FString CSV=TEXT("step,t_lat,t_phys,t_star,mass_drift,Fx,Fy,Fz,My,mea_Fx,window_Fx,window_Fx_prev,mlups_inst,mlups_cum,nonfinite\n");
        auto N=[](const TOptional<double>& V){return V.IsSet()?FString::Printf(TEXT("%.17g"),V.GetValue()):FString();};
        for(const auto& V:Telemetry->History())
        {
            TArray<FString> Cells={V.Step.IsSet()?LexToString(V.Step.GetValue()):FString(),N(V.LatticeTime),N(V.PhysicalTime),N(V.DimensionlessTime),N(V.Mass.PhiDrift),
                N(V.Forces.Fx),N(V.Forces.Fy),N(V.Forces.Fz),N(V.Forces.My),N(V.Forces.MomentumFx),N(V.Window.Fx),N(V.Window.PreviousFx),N(V.ReportedMLUPSInstant),N(V.ReportedMLUPSCumulative),V.bNonfinite?TEXT("true"):TEXT("false")};
            CSV+=FString::Join(Cells,TEXT(","))+TEXT("\n");
        }
        auto Provenance=MakeShared<FJsonObject>();
        Provenance->SetStringField(TEXT("stream_run_id"),TelemetryProvenance->StreamRunId.ToString());
        Provenance->SetStringField(TEXT("identity_kind"),TelemetryProvenance->OriginalRunId?TEXT("owner_supplied_original_run"):TEXT("independent_replay"));
        if(TelemetryProvenance->OriginalRunId)Provenance->SetStringField(TEXT("original_run_id"),TelemetryProvenance->OriginalRunId->ToString());
        else Provenance->SetField(TEXT("original_run_id"),MakeShared<FJsonValueNull>());
        if(TelemetryProvenance->bImportedReplay)Provenance->SetStringField(TEXT("replay_id"),TelemetryProvenance->StreamRunId.ToString());
        else Provenance->SetField(TEXT("replay_id"),MakeShared<FJsonValueNull>());
        Provenance->SetBoolField(TEXT("imported_replay"),TelemetryProvenance->bImportedReplay);
        Provenance->SetBoolField(TEXT("captured_prefix"),TelemetryProvenance->bCapturedPrefix);Provenance->SetBoolField(TEXT("capture_covers_displayed_data"),TelemetryProvenance->bCaptureCoversDisplayedData);
        Provenance->SetNumberField(TEXT("captured_byte_count"),TelemetryProvenance->CapturedByteCount);
        if(Inputs&&Inputs->OriginalTelemetryBytes&&!Inputs->OriginalTelemetryBytes->IsEmpty())
        {
            const TCHAR* File=TelemetryProvenance->bCapturedPrefix?TEXT("original-live-prefix.jsonl"):TEXT("original-telemetry.jsonl");
            if(!FFileHelper::SaveArrayToFile(*Inputs->OriginalTelemetryBytes,*(Stage/File))){Error=TEXT("Could not write exact original science bytes.");return Abort();}
            Provenance->SetStringField(TEXT("original_file"),File);
        }
        else Provenance->SetStringField(TEXT("original_bytes_status"),TEXT("not_supplied; retained table is bounded original history only"));
        Provenance->SetStringField(TEXT("source_id"),TelemetryProvenance->SourceId);
        if(!TelemetryProvenance->SourcePath.IsEmpty())Provenance->SetStringField(TEXT("source_path"),TelemetryProvenance->SourcePath);
        else Provenance->SetField(TEXT("source_path"),MakeShared<FJsonValueNull>());
        if(!TelemetryProvenance->SourceSHA256.IsEmpty())Provenance->SetStringField(TEXT("source_sha256"),TelemetryProvenance->SourceSHA256);
        else Provenance->SetField(TEXT("source_sha256"),MakeShared<FJsonValueNull>());
        Provenance->SetStringField(TEXT("attached_project_id"),P.Id.ToString());Provenance->SetStringField(TEXT("attached_case_id"),P.Draft.Id.ToString());
        Provenance->SetStringField(TEXT("numeric_values"),TEXT("Original source values without conversion; missing values are blank."));
        if(const auto M=Telemetry->OriginalMetadata())Provenance->SetObjectField(TEXT("source_metadata"),StudioHome4ReportTelemetryPrivate::Metadata(*M));
        else Provenance->SetField(TEXT("source_metadata"),MakeShared<FJsonValueNull>());
        Metadata->SetObjectField(TEXT("telemetry_provenance"),Provenance);
        Metadata->SetNumberField(TEXT("retained_samples"),Telemetry->History().Num());
        TArray<TSharedPtr<FJsonValue>> Outputs;
        for(const auto& Event:Telemetry->OutputEvents())
        {
            auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("record_index"),LexToString(Event.RecordIndex));
            if(Event.Step)O->SetNumberField(TEXT("step"),double(*Event.Step));else O->SetField(TEXT("step"),MakeShared<FJsonValueNull>());
            const TCHAR* Kind=Event.Kind==EStudioHome4OutputKind::Trace?TEXT("trace"):Event.Kind==EStudioHome4OutputKind::Slice?TEXT("slice"):
                Event.Kind==EStudioHome4OutputKind::Visualization?TEXT("visualization"):TEXT("restart");
            O->SetStringField(TEXT("kind"),Kind);O->SetStringField(TEXT("reported_path"),Event.Path);
            O->SetStringField(TEXT("file_status"),TEXT("not_verified; historical source event, not a command acknowledgement"));
            Outputs.Add(MakeShared<FJsonValueObject>(O));
        }
        Metadata->SetArrayField(TEXT("telemetry_output_events"),Outputs);
        TeX+=TEXT("\\subsection*{Original science telemetry}\nRetained original samples: ")+LexToString(Telemetry->History().Num())+
            TEXT(". Values retain their original source units; missing measurements are blank in telemetry.csv.\\\\\n")+
            (TelemetryProvenance->OriginalRunId?TEXT("Owner-supplied original run: ")+EscapeLaTeX(TelemetryProvenance->OriginalRunId->ToString()):
                TEXT("Independent replay identity: ")+EscapeLaTeX(TelemetryProvenance->StreamRunId.ToString())+TEXT(". Original solver run unavailable."))+
            TEXT("\\\\\nImported replay is historical evidence. It supplies no control acknowledgement and does not validate the current draft.\\\\\n");
        if(!Write(TEXT("telemetry.csv"),CSV))return Abort();
    }
    TeX+=TEXT("\\subsection*{Applied parameters}\n\\begin{longtable}{p{0.42\\textwidth}p{0.5\\textwidth}}\nParameter & Value \\\\\\hline\n");
    const auto SpecJSON=StudioHome4Config::ToJSON(S);
    for(const auto& Field:StudioHome4Config::Fields())
    {
        const auto Section=Field.Section.IsEmpty()?SpecJSON:SpecJSON->GetObjectField(Field.Section);
        const auto V=Section->TryGetField(Field.Key);if(!V||V->Type==EJson::Null)continue;
        FString TextValue;
        if(V->Type==EJson::Number)TextValue=FString::Printf(TEXT("%.12g"),V->AsNumber());
        else if(V->Type==EJson::Boolean)TextValue=V->AsBool()?TEXT("true"):TEXT("false");
        else if(V->Type==EJson::String)TextValue=V->AsString();
        else if(V->Type==EJson::Array){TArray<FString> Values;for(const auto& Item:V->AsArray())Values.Add(FString::Printf(TEXT("%.12g"),Item->AsNumber()));TextValue=FString::Join(Values,TEXT(", "));}
        if(!TextValue.IsEmpty())TeX+=EscapeLaTeX(Field.Section+TEXT(".")+Field.Key)+TEXT(" & ")+EscapeLaTeX(TextValue+TEXT(" ")+Field.Unit)+TEXT(" \\\\")+TEXT("\n");
    }
    TeX+=TEXT("\\end{longtable}\n");
    TArray<TSharedPtr<FJsonValue>> FigureManifest;
    if(Evidence)
    {
        Metadata->SetObjectField(TEXT("imported_reference_evidence"),StudioHome4Validation::EvidenceMetadata(*Evidence));
        // The comparison applies to its identified original run, never to an edited next-run draft.
        TeX+=TEXT("\\subsection*{Imported reference comparison}\nRun: ")+EscapeLaTeX(Evidence->RunId.ToString())+TEXT("\\\\\nStatus: ")+EscapeLaTeX(Evidence->GateStatus())+TEXT("\\\\\nActual source: ")+EscapeLaTeX(Evidence->ActualSource)+TEXT("\\\\\nReference: ")+EscapeLaTeX(Evidence->ReferenceSource)+TEXT("\\\\\nThis comparison does not validate an edited configuration.\\\\\n");
        for(const auto& Series:Evidence->Series)TeX+=EscapeLaTeX(Series.Name+TEXT(" (")+Series.Unit+TEXT("): ")+Series.Gate.Reason)+TEXT("\\\\\n");
        if(!Write(TEXT("reference-evidence.json"),StudioHome4Validation::SerializeEvidence(*Evidence)))return Abort();
        if(!Evidence->OriginalBytes.IsEmpty()&&!FFileHelper::SaveArrayToFile(Evidence->OriginalBytes,*(Stage/TEXT("original-reference.json"))))
        {Error=TEXT("Could not write exact original reference source.");return Abort();}
        if(Evidence->bComposedAlignment&&(!FFileHelper::SaveArrayToFile(Evidence->ActualOriginalBytes,*(Stage/TEXT("original-actual-series.json")))||
            !FFileHelper::SaveArrayToFile(Evidence->ReferenceOriginalBytes,*(Stage/TEXT("original-reference-series.json")))))
        {Error=TEXT("Could not write independently retained exact original actual/reference sources.");return Abort();}
        auto Figure=[&](const FString& Stem,const FStudioHome4ReportPlot& Plot,const FString& Caption,const TSharedRef<FJsonObject>& Provenance,const TArray<FGuid>& Runs)
        {
            const FString Fragment=StudioHome4ReportPlots::TikZ(Plot);
            if(!Write(*(Stem+TEXT(".svg")),StudioHome4ReportPlots::SVG(Plot,Evidence->RunId.ToString()+TEXT("; ")+Caption)) ||
                !Write(*(Stem+TEXT(".tikz")),Fragment) ||
                !Write(*(Stem+TEXT(".tex")),StudioHome4ReportPlots::TeXPreamble()+Fragment+StudioHome4ReportPlots::TeXEnd()) ||
                !Write(*(Stem+TEXT(".csv")),StudioHome4ReportPlots::CSV(Plot,Runs,Runs.IsEmpty()?nullptr:&Evidence->OrderRuns)))return false;
            Provenance->SetStringField(TEXT("svg"),Stem+TEXT(".svg"));Provenance->SetStringField(TEXT("tex"),Stem+TEXT(".tex"));Provenance->SetStringField(TEXT("tikz"),Stem+TEXT(".tikz"));Provenance->SetStringField(TEXT("numeric_table"),Stem+TEXT(".csv"));
            Provenance->SetNumberField(TEXT("original_samples"),Plot.X.Num());Provenance->SetNumberField(TEXT("preview_samples"),Plot.PreviewIndices.Num());
            Provenance->SetStringField(TEXT("preview_selection"),StudioHome4ReportPlots::SelectionDescription());
            Provenance->SetStringField(TEXT("source_path"),Evidence->SourcePath);Provenance->SetStringField(TEXT("source_sha256"),Evidence->SourceSHA256);
            if(Evidence->bComposedAlignment)
            {
                Provenance->SetStringField(TEXT("alignment_policy"),Evidence->AlignmentPolicy);
                Provenance->SetStringField(TEXT("actual_original_path"),Evidence->ActualOriginalPath);Provenance->SetStringField(TEXT("reference_original_path"),Evidence->ReferenceOriginalPath);
                Provenance->SetStringField(TEXT("actual_original_sha256"),Evidence->ActualOriginalSHA256);Provenance->SetStringField(TEXT("reference_original_sha256"),Evidence->ReferenceOriginalSHA256);
                Provenance->SetStringField(TEXT("actual_original_bytes_file"),TEXT("original-actual-series.json"));Provenance->SetStringField(TEXT("reference_original_bytes_file"),TEXT("original-reference-series.json"));
            }
            Provenance->SetStringField(TEXT("reference_method"),Evidence->ReferenceMethod);
            Provenance->SetStringField(TEXT("actual_source"),Evidence->ActualSource);Provenance->SetStringField(TEXT("reference_source"),Evidence->ReferenceSource);
            Provenance->SetStringField(TEXT("evidence_run_id"),Evidence->RunId.ToString());Provenance->SetStringField(TEXT("recipe_id"),Evidence->RecipeId);
            Provenance->SetStringField(TEXT("project_id"),P.Id.ToString());Provenance->SetStringField(TEXT("case_id"),P.Draft.Id.ToString());
            Provenance->SetStringField(TEXT("original_recipe_gate"),Evidence->RecipeGateStatus());Provenance->SetStringField(TEXT("current_draft_gate"),TEXT("not_transferred"));
            FString Sidecar;FJsonSerializer::Serialize(Provenance,TJsonWriterFactory<>::Create(&Sidecar));
            if(!Write(*(Stem+TEXT(".json")),Sidecar))return false;
            FigureManifest.Add(MakeShared<FJsonValueObject>(Provenance));
            TeX+=TEXT("\\begin{figure}[htbp]\n\\centering\n\\input{")+Stem+TEXT(".tikz}\n\\caption{")+EscapeLaTeX(Caption)+TEXT("}\n\\end{figure}\n");return true;
        };
        for(int32 I=0;I<Evidence->Series.Num();++I)
        {
            const auto& V=Evidence->Series[I];FStudioHome4ReportPlot Plot;
            if(!StudioHome4ReportPlots::Reference(V,Plot,Error))return Abort();
            auto Provenance=MakeShared<FJsonObject>();Provenance->SetStringField(TEXT("kind"),TEXT("reference_overlay"));Provenance->SetStringField(TEXT("metric_id"),V.Id);Provenance->SetStringField(TEXT("unit"),V.Unit);
            Provenance->SetStringField(TEXT("x_name"),V.AbscissaName);Provenance->SetStringField(TEXT("x_unit"),V.AbscissaUnit);Provenance->SetStringField(TEXT("original_epoch"),V.AbscissaEpoch.IsEmpty()?TEXT("unknown"):V.AbscissaEpoch);Provenance->SetNumberField(TEXT("window_start"),V.Abscissae[0]);Provenance->SetNumberField(TEXT("window_end"),V.Abscissae.Last());Provenance->SetBoolField(TEXT("window_inclusive"),true);
            Provenance->SetNumberField(TEXT("absolute_tolerance"),V.AbsoluteTolerance);Provenance->SetNumberField(TEXT("relative_tolerance"),V.RelativeTolerance);
            const FString Caption=V.Name+TEXT(" [")+V.Unit+TEXT("] · evidence run ")+Evidence->RunId.ToString()+FString::Printf(TEXT(" · inclusive %s window %.17g to %.17g %s · absolute tolerance %.17g, relative %.17g. Display preview; complete data in its CSV."),*V.AbscissaName,V.Abscissae[0],V.Abscissae.Last(),*V.AbscissaUnit,V.AbsoluteTolerance,V.RelativeTolerance);
            if(!Figure(FString::Printf(TEXT("reference-%02d"),I+1),Plot,Caption,Provenance,{}))return Abort();
        }
        if(!Evidence->OrderRuns.IsEmpty())
        {
            FStudioHome4ReportPlot Plot;if(!StudioHome4ReportPlots::Convergence(*Evidence,Plot,Error))return Abort();
            auto Provenance=MakeShared<FJsonObject>();Provenance->SetStringField(TEXT("kind"),TEXT("three_run_convergence"));Provenance->SetStringField(TEXT("metric_id"),Evidence->OrderMetric);Provenance->SetStringField(TEXT("unit"),Evidence->OrderUnit);
            bool HasScalarWindow=false;for(const auto& V:Evidence->OrderRuns)HasScalarWindow|=V.Extraction.WindowStart.IsSet();
            Provenance->SetStringField(TEXT("scalar_window_status"),HasScalarWindow?TEXT("per_run_optional_metadata; no window equivalence inferred"):TEXT("not_supplied; no window equivalence inferred"));
            TArray<FGuid> Runs;TArray<TSharedPtr<FJsonValue>> RunRecords;
            FString Caption=Evidence->OrderMetric+TEXT(" [")+Evidence->OrderUnit+TEXT("] · three original scalar runs. Absent extraction metadata is unknown; no window equivalence inferred.");
            for(const auto& V:Evidence->OrderRuns){Runs.Add(V.RunId);RunRecords.Add(MakeShared<FJsonValueObject>(StudioHome4Validation::ScalarRunMetadata(V)));Caption+=TEXT(" ")+StudioHome4Validation::ScalarRunDescription(V);}
            Provenance->SetArrayField(TEXT("runs"),RunRecords);if(Evidence->ObservedOrder){Provenance->SetNumberField(TEXT("observed_order"),*Evidence->ObservedOrder);Caption+=FString::Printf(TEXT(" Observed order %.6g."),*Evidence->ObservedOrder);}else Caption+=TEXT(" Observed order unavailable for this sequence.");
            if(!Figure(TEXT("convergence"),Plot,Caption,Provenance,Runs))return Abort();
        }
    }
    if(HasTelemetry)
    {
        using namespace StudioHome4SciencePresentation;
        const FStudioHome4ReportInputs Selection=Inputs?*Inputs:FStudioHome4ReportInputs();
        auto ScienceFigure=[&](EMetric Metric,int32 Component,const FString& Body,const FString& Phase,int32 Level,const FString& Stem)
        {
            auto History=StudioHome4SciencePresentation::History(Telemetry,Metric,Selection.Display,Component,false,Body,Level,Phase);
            if(Selection.WindowStart)
            {
                if(History.Axis!=Selection.WindowAxis){Error=TEXT("Report window axis does not match the original displayed science abscissa.");return false;}
                for(int32 I=History.X.Num()-1;I>=0;--I)if(History.X[I]<*Selection.WindowStart||History.X[I]>*Selection.WindowEnd)
                {History.X.RemoveAt(I,1,EAllowShrinking::No);for(auto& Channel:History.Series)Channel.Values.RemoveAt(I,1,EAllowShrinking::No);}
            }
            FStudioHome4ReportPlot Plot;FString Failure;
            if(!StudioHome4ReportPlots::Science(History,Name(Metric),Plot,Failure))return true;
            auto Sidecar=MakeShared<FJsonObject>();Sidecar->SetStringField(TEXT("kind"),TEXT("original_science_history"));Sidecar->SetStringField(TEXT("metric"),Name(Metric));
            Sidecar->SetStringField(TEXT("stream_run_id"),TelemetryProvenance->StreamRunId.ToString());Sidecar->SetStringField(TEXT("source_id"),TelemetryProvenance->SourceId);Sidecar->SetStringField(TEXT("source_sha256"),TelemetryProvenance->SourceSHA256);
            if(TelemetryProvenance->OriginalRunId)Sidecar->SetStringField(TEXT("original_run_id"),TelemetryProvenance->OriginalRunId->ToString());
            Sidecar->SetStringField(TEXT("body_id"),Body);Sidecar->SetStringField(TEXT("phase"),Phase);Sidecar->SetNumberField(TEXT("level"),Level);Sidecar->SetNumberField(TEXT("component"),Component);
            Sidecar->SetStringField(TEXT("axis"),History.Axis);Sidecar->SetStringField(TEXT("unit"),History.Unit);Sidecar->SetStringField(TEXT("conversion_note"),History.Note);
            Sidecar->SetNumberField(TEXT("window_start"),Plot.XMin);Sidecar->SetNumberField(TEXT("window_end"),Plot.XMax);Sidecar->SetNumberField(TEXT("retained_original_samples"),Plot.X.Num());
            Sidecar->SetBoolField(TEXT("source_is_captured_prefix"),TelemetryProvenance->bCapturedPrefix);Sidecar->SetBoolField(TEXT("capture_covers_displayed_data"),TelemetryProvenance->bCaptureCoversDisplayedData);
            Sidecar->SetStringField(TEXT("missing_values"),TEXT("Blank CSV values and visible disconnected plot segments; no filling or interpolation."));Sidecar->SetStringField(TEXT("gate_status"),TEXT("not_evaluated"));
            if(const auto M=Telemetry->OriginalMetadata())Sidecar->SetObjectField(TEXT("source_metadata"),StudioHome4ReportTelemetryPrivate::Metadata(*M));
            const FString Fragment=StudioHome4ReportPlots::TikZ(Plot);FString JSON;FJsonSerializer::Serialize(Sidecar,TJsonWriterFactory<>::Create(&JSON));
            if(!Write(*(Stem+TEXT(".svg")),StudioHome4ReportPlots::SVG(Plot,TelemetryProvenance->SourceId))||!Write(*(Stem+TEXT(".tikz")),Fragment)||
                !Write(*(Stem+TEXT(".tex")),StudioHome4ReportPlots::TeXPreamble()+Fragment+StudioHome4ReportPlots::TeXEnd())||!Write(*(Stem+TEXT(".csv")),StudioHome4ReportPlots::CSV(Plot))||!Write(*(Stem+TEXT(".json")),JSON))return false;
            FigureManifest.Add(MakeShared<FJsonValueObject>(Sidecar));
            TeX+=TEXT("\\begin{figure}[htbp]\n\\centering\n\\input{")+Stem+TEXT(".tikz}\n\\caption{")+EscapeLaTeX(FString(Name(Metric))+TEXT(" · original retained science; gaps are missing measurements. ")+History.Note)+TEXT("}\n\\end{figure}\n");return true;
        };
        for(int32 I=0;I<int32(EMetric::Count);++I)
        {
            const auto Metric=EMetric(I);
            if(Metric==EMetric::Forces){for(int32 Component=0;Component<4;++Component)if(!ScienceFigure(Metric,Component,Selection.BodyId,Selection.Phase,Selection.Level,FString::Printf(TEXT("science-force-%d"),Component)))return Abort();}
            else if(!ScienceFigure(Metric,0,Selection.BodyId,Selection.Phase,Selection.Level,FString::Printf(TEXT("science-%02d"),I)))return Abort();
        }
    }
    if(Inputs&&Inputs->Validation)
    {
        const auto& State=*Inputs->Validation;auto Manifest=MakeShared<FJsonObject>();TArray<TSharedPtr<FJsonValue>> Results;
        for(int32 I=0;I<State.Results.Num();++I)
        {
            const auto& Result=State.Results[I];
            const auto* Planned=State.Ladder.FindByPredicate([&](const auto& R){return R.PlannedRunId==Result.RunId;});
            if(!Result.Evidence||!Planned||Result.Refinement!=Planned->Refinement||Result.Evidence->RunId!=Result.RunId||Result.Evidence->AttachedProjectId!=P.Id||Result.Evidence->AttachedCaseId!=P.Draft.Id||Result.Evidence->RecipeId!=S.RecipeId||
                !Result.Evidence->OriginalRunSpec||StudioHome4Config::Serialize(*Result.Evidence->OriginalRunSpec)!=StudioHome4Config::Serialize(Planned->Spec))
            {Error=TEXT("Ladder result no longer matches this scope and its exact immutable planned rung specification.");return Abort();}
            if(!StudioHome4Validation::VerifyOriginalBytes(*Result.Evidence,Error))return Abort();
            const FString File=FString::Printf(TEXT("original-rung-%02d.json"),I);
            if(!FFileHelper::SaveArrayToFile(Result.Evidence->OriginalBytes,*(Stage/File))){Error=TEXT("Could not write exact original rung source.");return Abort();}
            if(Result.Evidence->bComposedAlignment&&(!FFileHelper::SaveArrayToFile(Result.Evidence->ActualOriginalBytes,*(Stage/FString::Printf(TEXT("original-rung-%02d-actual.json"),I)))||!FFileHelper::SaveArrayToFile(Result.Evidence->ReferenceOriginalBytes,*(Stage/FString::Printf(TEXT("original-rung-%02d-reference.json"),I)))))
            {Error=TEXT("Could not write independently retained exact original rung sources.");return Abort();}
            auto Record=StudioHome4Validation::EvidenceMetadata(*Result.Evidence);Record->SetStringField(TEXT("original_file"),File);Record->SetNumberField(TEXT("refinement"),Result.Refinement);
            if(Result.Scalar)Record->SetObjectField(TEXT("extracted_scalar"),StudioHome4Validation::ScalarRunMetadata(*Result.Scalar));
            Results.Add(MakeShared<FJsonValueObject>(Record));
        }
        Manifest->SetArrayField(TEXT("original_results"),Results);Metadata->SetObjectField(TEXT("ladder_evidence"),Manifest);
        if(State.ConvergenceEvidence)
        {
            FStudioHome4ReportPlot Plot;if(!StudioHome4ReportPlots::Convergence(*State.ConvergenceEvidence,Plot,Error))return Abort();
            const auto Provenance=StudioHome4Validation::EvidenceMetadata(*State.ConvergenceEvidence);FString JSON;FJsonSerializer::Serialize(Provenance,TJsonWriterFactory<>::Create(&JSON));
            TArray<FGuid> Runs;for(const auto& V:State.ConvergenceEvidence->OrderRuns)Runs.Add(V.RunId);
            const FString Fragment=StudioHome4ReportPlots::TikZ(Plot);
            if(!Write(TEXT("ladder-convergence.svg"),StudioHome4ReportPlots::SVG(Plot,TEXT("Original selected rung triplet")))||!Write(TEXT("ladder-convergence.tikz"),Fragment)||
                !Write(TEXT("ladder-convergence.tex"),StudioHome4ReportPlots::TeXPreamble()+Fragment+StudioHome4ReportPlots::TeXEnd())||!Write(TEXT("ladder-convergence.csv"),StudioHome4ReportPlots::CSV(Plot,Runs,&State.ConvergenceEvidence->OrderRuns))||!Write(TEXT("ladder-convergence.json"),JSON))return Abort();
            TeX+=TEXT("\\subsection*{Selected original ladder convergence}\n\\input{ladder-convergence.tikz}\n");FigureManifest.Add(MakeShared<FJsonValueObject>(Provenance));
        }
    }
    Metadata->SetArrayField(TEXT("figures"),FigureManifest);
    Metadata->SetStringField(TEXT("tex_engine"),TEXT("XeLaTeX, LuaLaTeX or Tectonic; UTF-8/fontspec and TikZ"));
    Metadata->SetStringField(TEXT("recipe_coverage"),Evidence?Evidence->RecipeCoverage():TEXT("unknown"));
    Metadata->SetStringField(TEXT("original_evidence_gate"),Evidence?Evidence->RecipeGateStatus():TEXT("not_evaluated"));
    Metadata->SetStringField(TEXT("publication_check"),Inputs&&Inputs->bFigurePublication?TEXT("passed_original_coefficient_comparison"):TEXT("not_requested"));
    TeX+=StudioHome4ReportPlots::TeXEnd();FString JSON;FJsonSerializer::Serialize(Metadata,TJsonWriterFactory<>::Create(&JSON));
    if(!Write(TEXT("run_spec.json"),StudioHome4Config::Serialize(S))||!Write(TEXT("command.txt"),CLI)||!Write(TEXT("report.tex"),TeX)||!Write(TEXT("report.json"),JSON))return Abort();
    const FString Instructions=TEXT("Compile report.tex from this directory with xelatex report.tex, lualatex report.tex or tectonic report.tex. pdfLaTeX is unsupported (UTF-8/fontspec). Each figure .tex is independently compilable; its .tikz fragment is included by the report. SVG and TikZ are documented display previews. CSV and reference-evidence.json retain every original measurement at round-trip double precision. report.json contains figure/run/source/window/tolerance provenance. Convergence CSV retains optional per-run original extraction window/unit/epoch/method/source/hash; absent values are blank or null and explicitly unknown. No cross-run window equivalence is inferred. Imported comparisons do not validate the current draft or establish recipe coverage.\n");
    if(!Write(TEXT("COMPILE.txt"),Instructions))return Abort();
    if(!StudioFileDialog::PublishExportDirectory(Stage,Destination,Error))return Abort();
    OutPath=Destination;Error.Empty();return true;
}
