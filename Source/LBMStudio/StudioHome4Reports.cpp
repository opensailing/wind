#include "StudioHome4Reports.h"
#include "StudioProject.h"
#include "StudioHome4Recipes.h"
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
        case TCHAR('\r'):break;case TCHAR('\n'):Out+=TEXT("\n");break;default:Out.AppendChar(C);break;
    }
    }
    return Out;
}
bool StudioHome4Reports::Export(const FString& Parent,const FString& Folder,const FStudioProject& P,const FStudioHome4TelemetryStream* Telemetry,FString& OutPath,FString& Error)
{
    if(!P.Draft.Home4.IsSet()){Error=TEXT("Apply a HOME4 recipe or configuration before exporting its report.");return false;}
    if(Folder.IsEmpty()||Folder.Len()>100||Folder==TEXT(".")||Folder==TEXT("..")||Folder.Contains(TEXT("/"))||Folder.Contains(TEXT("\\"))||Folder.Contains(TEXT(":")))
    {Error=TEXT("Use a report folder name of 1–100 characters without path separators.");return false;}
    const auto& S=P.Draft.Home4.GetValue();if(!StudioHome4Config::Validate(S,Error))return false;
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
    FString TeX=TEXT("\\documentclass{article}\n\\usepackage[margin=2cm]{geometry}\n\\usepackage{longtable}\n\\begin{document}\n\\section*{")+EscapeLaTeX(P.Name)+TEXT("}\n");
    TeX+=TEXT("Recipe: ")+EscapeLaTeX(R?R->Name:TEXT("Unspecified"))+TEXT("\\\\\nLineage: ")+EscapeLaTeX(S.LineageId)+TEXT("\\\\\nValidation gate: not evaluated. Reference evidence is not supplied.\\\\\n");
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
    TeX+=TEXT("\\end{document}\n");FString JSON;FJsonSerializer::Serialize(Metadata,TJsonWriterFactory<>::Create(&JSON));
    if(!Write(TEXT("run_spec.json"),StudioHome4Config::Serialize(S))||!Write(TEXT("command.txt"),CLI)||!Write(TEXT("report.tex"),TeX)||!Write(TEXT("report.json"),JSON))return Abort();
    if(!StudioFileDialog::PublishExportDirectory(Stage,Destination,Error))return Abort();
    OutPath=Destination;Error.Empty();return true;
}
