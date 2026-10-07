#include "StudioHome4Reports.h"
#include "StudioHome4ReportPlots.h"
#include "StudioProject.h"
#include "StudioHome4Recipes.h"
#include "StudioHome4Validation.h"
#include "StudioFileDialog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

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
bool StudioHome4Reports::Export(const FString& Parent,const FString& Folder,const FStudioProject& P,const FStudioHome4TelemetryStream* Telemetry,FString& OutPath,FString& Error,const FStudioHome4ReferenceEvidence* Evidence)
{
    if(!P.Draft.Home4.IsSet()){Error=TEXT("Apply a HOME4 recipe or configuration before exporting its report.");return false;}
    if(Folder.IsEmpty()||Folder.Len()>100||Folder==TEXT(".")||Folder==TEXT("..")||Folder.Contains(TEXT("/"))||Folder.Contains(TEXT("\\"))||Folder.Contains(TEXT(":")))
    {Error=TEXT("Use a report folder name of 1–100 characters without path separators.");return false;}
    const auto& S=P.Draft.Home4.GetValue();if(!StudioHome4Config::Validate(S,Error))return false;
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
        CheckedEvidence.AttachedProjectId=Evidence->AttachedProjectId;CheckedEvidence.AttachedCaseId=Evidence->AttachedCaseId;
        Evidence=&CheckedEvidence;
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
    Metadata->SetObjectField(TEXT("run_spec"),StudioHome4Config::ToJSON(S));
    const bool HasTelemetry=Telemetry&&!Telemetry->History().IsEmpty();Metadata->SetBoolField(TEXT("contains_imported_telemetry"),HasTelemetry);
    FString TeX=StudioHome4ReportPlots::TeXPreamble()+TEXT("\\section*{")+EscapeLaTeX(P.Name)+TEXT("}\n");
    TeX+=TEXT("Recipe: ")+EscapeLaTeX(R?R->Name:TEXT("Unspecified"))+TEXT("\\\\\nLineage: ")+EscapeLaTeX(S.LineageId)+(Evidence?TEXT("\\\\\nValidation gate: not evaluated. Imported comparisons apply only to the identified evidence run; recipe coverage is unknown.\\\\\n"):TEXT("\\\\\nValidation gate: not evaluated. Reference evidence is not supplied.\\\\\n"));
    TeX+=TEXT("Configuration is stored in run\\_spec.json. The command is a reproducibility preview; the development adapter computes no CFD.\\\\\n");
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
        Metadata->SetStringField(TEXT("telemetry_source"),Telemetry->History().Last().Source.SourceId);
        Metadata->SetStringField(TEXT("telemetry_run_id"),Telemetry->History().Last().Source.RunId.ToString());
        Metadata->SetNumberField(TEXT("retained_samples"),Telemetry->History().Num());
        TeX+=TEXT("\\subsection*{Imported telemetry}\nRetained original samples: ")+LexToString(Telemetry->History().Num())+TEXT(". Missing measurements are blank in telemetry.csv.\n");
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
        auto Figure=[&](const FString& Stem,const FStudioHome4ReportPlot& Plot,const FString& Caption,const TSharedRef<FJsonObject>& Provenance,const TArray<FGuid>& Runs)
        {
            const FString Fragment=StudioHome4ReportPlots::TikZ(Plot);
            if(!Write(*(Stem+TEXT(".svg")),StudioHome4ReportPlots::SVG(Plot,Evidence->RunId.ToString()+TEXT("; ")+Caption)) ||
                !Write(*(Stem+TEXT(".tikz")),Fragment) ||
                !Write(*(Stem+TEXT(".tex")),StudioHome4ReportPlots::TeXPreamble()+Fragment+StudioHome4ReportPlots::TeXEnd()) ||
                !Write(*(Stem+TEXT(".csv")),StudioHome4ReportPlots::CSV(Plot,Runs)))return false;
            Provenance->SetStringField(TEXT("svg"),Stem+TEXT(".svg"));Provenance->SetStringField(TEXT("tex"),Stem+TEXT(".tex"));Provenance->SetStringField(TEXT("tikz"),Stem+TEXT(".tikz"));Provenance->SetStringField(TEXT("numeric_table"),Stem+TEXT(".csv"));
            Provenance->SetNumberField(TEXT("original_samples"),Plot.X.Num());Provenance->SetNumberField(TEXT("preview_samples"),Plot.PreviewIndices.Num());
            Provenance->SetStringField(TEXT("preview_selection"),StudioHome4ReportPlots::SelectionDescription());
            Provenance->SetStringField(TEXT("source_path"),Evidence->SourcePath);Provenance->SetStringField(TEXT("source_sha256"),Evidence->SourceSHA256);
            Provenance->SetStringField(TEXT("actual_source"),Evidence->ActualSource);Provenance->SetStringField(TEXT("reference_source"),Evidence->ReferenceSource);
            Provenance->SetStringField(TEXT("evidence_run_id"),Evidence->RunId.ToString());Provenance->SetStringField(TEXT("recipe_id"),Evidence->RecipeId);
            Provenance->SetStringField(TEXT("project_id"),P.Id.ToString());Provenance->SetStringField(TEXT("case_id"),P.Draft.Id.ToString());
            FigureManifest.Add(MakeShared<FJsonValueObject>(Provenance));
            TeX+=TEXT("\\begin{figure}[htbp]\n\\centering\n\\input{")+Stem+TEXT(".tikz}\n\\caption{")+EscapeLaTeX(Caption)+TEXT("}\n\\end{figure}\n");return true;
        };
        for(int32 I=0;I<Evidence->Series.Num();++I)
        {
            const auto& V=Evidence->Series[I];FStudioHome4ReportPlot Plot;
            if(!StudioHome4ReportPlots::Reference(V,Plot,Error))return Abort();
            auto Provenance=MakeShared<FJsonObject>();Provenance->SetStringField(TEXT("kind"),TEXT("reference_overlay"));Provenance->SetStringField(TEXT("metric_id"),V.Id);Provenance->SetStringField(TEXT("unit"),V.Unit);
            Provenance->SetStringField(TEXT("x_name"),V.AbscissaName);Provenance->SetStringField(TEXT("x_unit"),V.AbscissaUnit);Provenance->SetNumberField(TEXT("window_start"),V.Abscissae[0]);Provenance->SetNumberField(TEXT("window_end"),V.Abscissae.Last());Provenance->SetBoolField(TEXT("window_inclusive"),true);
            Provenance->SetNumberField(TEXT("absolute_tolerance"),V.AbsoluteTolerance);Provenance->SetNumberField(TEXT("relative_tolerance"),V.RelativeTolerance);
            const FString Caption=V.Name+TEXT(" [")+V.Unit+TEXT("] · evidence run ")+Evidence->RunId.ToString()+FString::Printf(TEXT(" · inclusive %s window %.17g to %.17g %s · absolute tolerance %.17g, relative %.17g. Display preview; complete data in its CSV."),*V.AbscissaName,V.Abscissae[0],V.Abscissae.Last(),*V.AbscissaUnit,V.AbsoluteTolerance,V.RelativeTolerance);
            if(!Figure(FString::Printf(TEXT("reference-%02d"),I+1),Plot,Caption,Provenance,{}))return Abort();
        }
        if(!Evidence->OrderRuns.IsEmpty())
        {
            FStudioHome4ReportPlot Plot;if(!StudioHome4ReportPlots::Convergence(*Evidence,Plot,Error))return Abort();
            auto Provenance=MakeShared<FJsonObject>();Provenance->SetStringField(TEXT("kind"),TEXT("three_run_convergence"));Provenance->SetStringField(TEXT("metric_id"),Evidence->OrderMetric);Provenance->SetStringField(TEXT("unit"),Evidence->OrderUnit);
            Provenance->SetStringField(TEXT("scalar_window_status"),TEXT("not_supplied; no window equivalence inferred"));
            TArray<FGuid> Runs;TArray<TSharedPtr<FJsonValue>> RunRecords;
            FString Caption=Evidence->OrderMetric+TEXT(" [")+Evidence->OrderUnit+TEXT("] · three original scalar runs. Averaging window not supplied; no window equivalence inferred.");
            for(const auto& V:Evidence->OrderRuns){Runs.Add(V.RunId);auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("run_id"),V.RunId.ToString());O->SetNumberField(TEXT("refinement"),V.Refinement);O->SetNumberField(TEXT("value"),V.Value);RunRecords.Add(MakeShared<FJsonValueObject>(O));Caption+=FString::Printf(TEXT(" Refinement %.17g: run %s."),V.Refinement,*V.RunId.ToString());}
            Provenance->SetArrayField(TEXT("runs"),RunRecords);if(Evidence->ObservedOrder){Provenance->SetNumberField(TEXT("observed_order"),*Evidence->ObservedOrder);Caption+=FString::Printf(TEXT(" Observed order %.6g."),*Evidence->ObservedOrder);}else Caption+=TEXT(" Observed order unavailable for this sequence.");
            if(!Figure(TEXT("convergence"),Plot,Caption,Provenance,Runs))return Abort();
        }
    }
    Metadata->SetArrayField(TEXT("figures"),FigureManifest);
    Metadata->SetStringField(TEXT("tex_engine"),TEXT("XeLaTeX, LuaLaTeX or Tectonic; UTF-8/fontspec and TikZ"));
    Metadata->SetStringField(TEXT("recipe_coverage"),TEXT("unknown"));
    TeX+=StudioHome4ReportPlots::TeXEnd();FString JSON;FJsonSerializer::Serialize(Metadata,TJsonWriterFactory<>::Create(&JSON));
    if(!Write(TEXT("run_spec.json"),StudioHome4Config::Serialize(S))||!Write(TEXT("command.txt"),CLI)||!Write(TEXT("report.tex"),TeX)||!Write(TEXT("report.json"),JSON))return Abort();
    const FString Instructions=TEXT("Compile report.tex from this directory with xelatex report.tex, lualatex report.tex or tectonic report.tex. pdfLaTeX is unsupported (UTF-8/fontspec). Each figure .tex is independently compilable; its .tikz fragment is included by the report. SVG and TikZ are documented display previews. CSV and reference-evidence.json retain every original measurement at round-trip double precision. report.json contains figure/run/source/window/tolerance provenance. Scalar averaging windows absent from the evidence are explicitly unknown. Imported comparisons do not validate the current draft or establish recipe coverage.\n");
    if(!Write(TEXT("COMPILE.txt"),Instructions))return Abort();
    if(!StudioFileDialog::PublishExportDirectory(Stage,Destination,Error))return Abort();
    OutPath=Destination;Error.Empty();return true;
}
