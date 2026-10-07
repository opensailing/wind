#include "SStudioHome4Panel.h"
#include "SStudioHome4RunControls.h"
#include "SStudioHome4Sizing.h"
#include "SStudioHome4Validation.h"
#include "StudioHome4Recipes.h"
#include "StudioHome4Readouts.h"
#include "StudioHome4Reports.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "StudioFileDialog.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Framework/Application/SlateApplication.h"

namespace Home4PanelLocal
{
FString Human(const FString& Key)
{
    FString Out;for(int32 I=0;I<Key.Len();++I)
    {const TCHAR C=Key[I];if(I>0&&FChar::IsUpper(C)&&FChar::IsLower(Key[I-1]))Out+=TEXT(" ");Out.AppendChar(I==0?FChar::ToUpper(C):C);}return Out;
}
FString N(const TOptional<double>& Value){return Value.IsSet()?FString::Printf(TEXT("%.6g"),Value.GetValue()):TEXT("Not supplied");}
TSharedRef<STextBlock> Paragraph(const FString& TextValue,FLinearColor Color=StudioUI::Text)
{return SNew(STextBlock).Text(FText::FromString(TextValue)).Font(StudioUI::Font(10)).ColorAndOpacity(Color).AutoWrapText(true);}
}
TSharedRef<SWidget> SStudioHome4Panel::Action(const FString& TextValue,FName Tag,TFunction<void()> Callback)
{
    return SNew(SButton).Tag(Tag).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(10,7))
        .OnClicked_Lambda([Callback]{Callback();return FReply::Handled();})[StudioUI::Label(TextValue)];
}
void SStudioHome4Panel::Construct(const FArguments& Args)
{
    Model=Args._Model;Session=Args._Session;Page=Args._Page;Validation=Args._Validation.IsValid()?Args._Validation:MakeShared<FStudioHome4ValidationState>();OnRecipe=Args._OnRecipe;Telemetry=Args._Telemetry;Sync();SetCanTick(true);
    auto Body=SNew(SVerticalBox);
    if(Page==TEXT("Lattice"))Body->AddSlot().AutoHeight()[SNew(SStudioHome4Sizing).Session(Session)];
    if(Page==TEXT("Validation")||Page==TEXT("Projects"))
    {
        Body->AddSlot().AutoHeight()[SNew(SStudioHome4Validation).Model(Model).State(Validation)];
        Body->AddSlot().AutoHeight().Padding(0,20,0,0)[Recipes()];
    }
    else
    {
        FString Previous;
        for(const auto& F:StudioHome4Config::Fields())
        {
            if(F.Page!=Page||F.Section.IsEmpty()||F.Key==TEXT("display"))continue;
            if(F.Section!=Previous)
            {Previous=F.Section;Body->AddSlot().AutoHeight().Padding(0,18,0,10)[StudioUI::Label(Home4PanelLocal::Human(Previous),13,StudioUI::Text,true)];}
            Body->AddSlot().AutoHeight().Padding(0,0,0,9)[Editor(F)];
        }
    }
    if(Page==TEXT("Geometry"))
    {
        auto Formats=SNew(SHorizontalBox);
        for(const TCHAR* Extension:{TEXT("step"),TEXT("iges"),TEXT("stl"),TEXT("obj")})
            Formats->AddSlot().AutoWidth().Padding(0,0,7,0)[Action(FString(TEXT("Choose "))+FString(Extension).ToUpper()+TEXT("…"),FName(*(FString(TEXT("Home4CAD."))+Extension)),[this,Ext=FString(Extension)]
            {FString Path;if(StudioFileDialog::DataFile(false,TEXT("Select body geometry"),TEXT(""),Ext,Path)){Session->Set(TEXT("geometry.sourcePath"),Path);Sync();}})];
        Body->InsertSlot(0).AutoHeight().Padding(0,0,0,12)[Formats];
        Body->InsertSlot(1).AutoHeight().Padding(0,0,0,12)[Home4PanelLocal::Paragraph(TEXT("SDF build · awaiting geometry adapter\nTessellation cache, signed-distance band and cut-link fractions are not supplied. STEP/IGES are retained as source requests. Imported STL/OBJ surfaces can be inspected under Import & surfaces."),StudioUI::Amber)];
    }
    if(Page==TEXT("Bodies"))
    {
        Body->InsertSlot(0).AutoHeight().Padding(0,0,0,12)[Home4PanelLocal::Paragraph(TEXT("Body forces are inspected in Monitors as separate pressure, viscous and momentum-exchange channels. Moving-body retabulation is a solver operation; no body motion is simulated by this development adapter."),StudioUI::Muted)];
        Body->AddSlot().AutoHeight().Padding(0,18)[Home4PanelLocal::Paragraph(TEXT("Retabulation cost reference\nThe supplied HOME4 note reports 47.8 MLUPS for a static mask, 5.9 for an oscillating cylinder and 0.63 for sedimentation on an M1 Pro. These are different measured workloads, not a prediction for this case."),StudioUI::Muted)];
    }
    if(Page==TEXT("Boundaries & Zones"))
        Body->InsertSlot(0).AutoHeight().Padding(0,0,0,12)[Home4PanelLocal::Paragraph(TEXT("Zone extents and strength profiles belong to the run configuration. Driver-specific widths remain labeled as driver values until the solver supplies their coordinate convention. Original field masks are inspected in Fields."),StudioUI::Muted)];
    if(Page==TEXT("Settings"))
    {
        Body->AddSlot().AutoHeight().Padding(0,0,0,10)[StudioUI::Label(TEXT("Readout units"),14,StudioUI::Text,true)];
        Body->AddSlot().AutoHeight().Padding(0,0,0,18)[Home4PanelLocal::Paragraph(TEXT("Use LU / SI / ND in the header to set the global readout mode. Converted values offer all three forms in their tooltip. A recording must supply its original unit map; editing the next-run map cannot change old results."))];
        Body->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Viewer preferences"),14,StudioUI::Text,true)];
        Body->AddSlot().AutoHeight().Padding(0,8)[Home4PanelLocal::Paragraph(TEXT("Palette, range, camera projection, clipping and layer visibility are saved with each project view. Edit them in the Fields inspector; source ranges and provenance remain attached to the original recording."))];
    }
    if(Page==TEXT("Run"))
    {
        Body->InsertSlot(0).AutoHeight().Padding(0,0,0,18)[SNew(SStudioHome4RunControls).Model(Model).OnSubmit(Args._OnSubmit)];
        Body->AddSlot().AutoHeight().Padding(0,18,0,8)[StudioUI::Label(TEXT("Equivalent command line"),13,StudioUI::Text,true)];
        Body->AddSlot().AutoHeight()[SNew(SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).Font(StudioUI::Font(10))
            .Text_Lambda([this]{return FText::FromString(Command());})];
        Body->AddSlot().AutoHeight().Padding(0,8)[Action(TEXT("Copy command"),TEXT("Home4CopyCLI"),[this]{FPlatformApplicationMisc::ClipboardCopy(*Command());Session->Status=TEXT("Command copied. No solver process was started.");})];
        Body->AddSlot().AutoHeight().Padding(0,10)[Home4PanelLocal::Paragraph(TEXT("Development adapter: use the header controls to exercise Run, Pause, Stop and Step. It computes no CFD, writes no restart state and supplies no scientific measurements. Metal, CUDA and rack execution await the solver adapter."),StudioUI::Amber)];
    }
    if(Page==TEXT("Reports"))
    {
        Body->AddSlot().AutoHeight().Padding(0,0,0,10)[StudioUI::Label(TEXT("Reproducible report bundle"),15,StudioUI::Text,true)];
        Body->AddSlot().AutoHeight().Padding(0,0,0,14)[Home4PanelLocal::Paragraph(TEXT("Export run_spec.json, the command preview, a JSON provenance sidecar and report.tex together. Each export creates a new folder; existing published results are preserved."))];
        Body->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Report folder"),10,StudioUI::Muted)];
        Body->AddSlot().AutoHeight().Padding(0,6,0,12)[SNew(SEditableTextBox).Tag(TEXT("Home4ReportName")).Style(&StudioUI::InputStyle()).Font(StudioUI::Font(11)).Text(FText::FromString(ReportName))
            .OnTextChanged_Lambda([this](const FText& T){ReportName=T.ToString();})];
        Body->AddSlot().AutoHeight()[Action(TEXT("Export report bundle…"),TEXT("Home4ReportExport"),[this]{ExportReport();})];
        Body->AddSlot().AutoHeight().Padding(0,18)[Home4PanelLocal::Paragraph(TEXT("Validation: not evaluated. A report records available evidence and cannot certify an unrun solver or an unsupplied reference."),StudioUI::Amber)];
    }
    ChildSlot[SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(22)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[StudioUI::Label(Page,23,StudioUI::Text,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,16)[Home4PanelLocal::Paragraph(
                Page==TEXT("Validation")?TEXT("Start from a documented recipe. Gates require identified reference data and measured results."):
                TEXT("HOME4 D3Q27 · retained case settings. Blank values remain unspecified; Apply commits one undoable edit."),StudioUI::Muted)]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,22,0)[SNew(SScrollBox).NavigationScrollPadding(12).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                    +SScrollBox::Slot()[Body]]
                +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(280)[Feasibility()]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,8)[SNew(SSeparator)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4Apply")).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(12,8))
                    .IsEnabled_Lambda([this]{return Session->IsDirty()&&!Session->HasConflict();})
                    .OnClicked_Lambda([this]{Session->Apply();Sync();return FReply::Handled();})[StudioUI::Label(TEXT("Apply configuration"),10,StudioUI::Cyan)]]
                +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[Action(TEXT("Revert edits"),TEXT("Home4Revert"),[this]{Session->Revert();Sync();})]
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[Action(TEXT("Import run spec…"),TEXT("Home4Import"),[this]{ImportSpec();})]
                +SHorizontalBox::Slot().AutoWidth()[Action(TEXT("Export run spec…"),TEXT("Home4Export"),[this]{ExportSpec();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,9,0,0)[SNew(STextBlock).Tag(TEXT("Home4Status")).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Amber).AutoWrapText(true)
                .Text_Lambda([this]{return FText::FromString(!Session->Status.IsEmpty()?Session->Status:PreviewError);})]]];
}
TSharedRef<SWidget> SStudioHome4Panel::Editor(const FStudioHome4Field& F)
{
    const FString K=FStudioHome4Session::Key(F);TSharedPtr<SWidget> Input;
    if(F.Type==EStudioHome4FieldType::Boolean||!F.Choices.IsEmpty())
    {
        Input=SNew(SComboButton).Tag(FName(*K)).ButtonStyle(&StudioUI::ButtonStyle())
            .OnGetMenuContent_Lambda([this,F,K]
            {
                auto Rows=SNew(SVerticalBox);auto Choices=F.Choices;
                if(F.Type==EStudioHome4FieldType::Boolean)Choices={TEXT(""),TEXT("true"),TEXT("false")};
                for(const auto& C:Choices)Rows->AddSlot().AutoHeight()[Action(C.IsEmpty()?TEXT("Not specified"):C==TEXT("true")?TEXT("On"):C==TEXT("false")?TEXT("Off"):C,
                    FName(*(K+TEXT(".")+(C.IsEmpty()?TEXT("unspecified"):C))),[this,K,C]{Session->Set(K,C);FSlateApplication::Get().DismissAllMenus();})];
                return StaticCastSharedRef<SWidget>(Rows);
            }).ButtonContent()[SNew(STextBlock).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Text)
                .Text_Lambda([this,K]{const auto V=Session->Get(K);return FText::FromString(V.IsEmpty()?TEXT("Not specified"):V==TEXT("true")?TEXT("On"):V==TEXT("false")?TEXT("Off"):V);})];
    }
    else
    {
        TSharedPtr<SEditableTextBox> Edit;
        Input=SAssignNew(Edit,SEditableTextBox).Tag(FName(*K)).Style(&StudioUI::InputStyle()).Font(StudioUI::Font(10))
            .Text(FText::FromString(Session->Get(K))).HintText(FText::FromString(F.bOptional?TEXT("Not specified"):TEXT("")))
            .OnTextChanged_Lambda([this,K](const FText& T){Session->Set(K,T.ToString());})
            .OnTextCommitted_Lambda([this](const FText&,ETextCommit::Type Type){if(Type==ETextCommit::OnEnter){Session->Apply();Sync();}});
        Inputs.Add(K,Edit);
    }
    return SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(185)[Home4PanelLocal::Paragraph(F.Label==F.Key?Home4PanelLocal::Human(F.Key):F.Label)]]
            +SHorizontalBox::Slot().FillWidth(1).Padding(8,0)[Input.ToSharedRef()]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(83)[Home4PanelLocal::Paragraph(F.Unit,StudioUI::Muted)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(193,3,83,0)[Home4PanelLocal::Paragraph(F.Help,StudioUI::Muted)];
}
TSharedRef<SWidget> SStudioHome4Panel::Recipes()
{
    auto Rows=SNew(SVerticalBox);
    for(const auto& R:StudioHome4Recipes::All())
    {
        Rows->AddSlot().AutoHeight().Padding(0,0,0,16)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1)[StudioUI::Label(R.Name,14,StudioUI::Text,true)]
                +SHorizontalBox::Slot().AutoWidth()[Action(TEXT("Use recipe"),FName(*(TEXT("Recipe.")+R.Id)),[this,Id=R.Id]
                {if(OnRecipe.IsBound())OnRecipe.Execute(Id);else Session->ApplyRecipe(Id);Sync();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,6)[Home4PanelLocal::Paragraph(R.Anchor+TEXT(" · ")+R.Driver,StudioUI::Muted)]
            +SVerticalBox::Slot().AutoHeight()[Home4PanelLocal::Paragraph(TEXT("Gate: ")+R.Gate)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,6)[Home4PanelLocal::Paragraph(R.Notes,StudioUI::Muted)]
            +SVerticalBox::Slot().AutoHeight()[Home4PanelLocal::Paragraph(TEXT("Not evaluated · reference results not supplied"),StudioUI::Amber)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0)[SNew(SSeparator)]];
    }
    return Rows;
}
TSharedRef<SWidget> SStudioHome4Panel::Feasibility()
{
    auto Values=SNew(SVerticalBox);
    auto Add=[&](const TCHAR* Name,EStudioHome4Quantity Q,TFunction<TOptional<double>()> Read)
    {
        Values->AddSlot().AutoHeight().Padding(0,0,0,9)[SNew(STextBlock).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Cyan).AutoWrapText(true)
            .Text_Lambda([this,Name,Q,Read]{const auto V=Read();return FText::FromString(FString(Name)+TEXT("  ")+(V.IsSet()?StudioHome4Readouts::Value(V.GetValue(),Q,EStudioHome4UnitDisplay::Lattice,Model->UnitDisplay,&Preview):TEXT("Not supplied")));})
            .ToolTipText_Lambda([this,Q,Read]{const auto V=Read();return FText::FromString(V.IsSet()?StudioHome4Readouts::Tooltip(V.GetValue(),Q,EStudioHome4UnitDisplay::Lattice,&Preview):TEXT("Supply this case quantity to view its unit conversions."));})];
    };
    Add(TEXT("Speed"),EStudioHome4Quantity::Velocity,[this]{return Derived.Speed;});
    Add(TEXT("Heavy viscosity"),EStudioHome4Quantity::KinematicViscosity,[this]{return Derived.NuHeavy;});
    Add(TEXT("Length"),EStudioHome4Quantity::Length,[this]{return Preview.Reference.LengthCells;});
    return SNew(SScrollBox).Tag(TEXT("Home4Feasibility"))+SScrollBox::Slot()[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[StudioUI::Label(TEXT("Feasibility"),15,StudioUI::Text,true)]
        +SVerticalBox::Slot().AutoHeight()[Values]
        +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4FeasibilitySummary")).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Text).AutoWrapText(true)
            .ToolTipText_Lambda([this]{return FText::FromString(TEXT("Derived Kn ≈ Ma/Re: ")+Home4PanelLocal::N(Derived.Knudsen));})
            .Text_Lambda([this]{return FText::FromString(Summary());})]];
}
FString SStudioHome4Panel::Summary() const
{
    if(!PreviewError.IsEmpty())return TEXT("Correct the draft to calculate feasibility.\n\n")+PreviewError;
    FString Out=TEXT("Lattice Mach  ")+Home4PanelLocal::N(Derived.Mach)+TEXT("\nReynolds  ")+Home4PanelLocal::N(Derived.Reynolds)+TEXT("\nFroude  ")+Home4PanelLocal::N(Derived.Froude)+
        TEXT("\nBond / Weber  ")+Home4PanelLocal::N(Derived.Bond)+TEXT(" / ")+Home4PanelLocal::N(Derived.Weber)+TEXT("\nCapillary  ")+Home4PanelLocal::N(Derived.Capillary)+
        TEXT("\nPeclet / Cahn  ")+Home4PanelLocal::N(Derived.Peclet)+TEXT(" / ")+Home4PanelLocal::N(Derived.Cahn)+TEXT("\nAtwood  ")+Home4PanelLocal::N(Derived.Atwood)+
        TEXT("\n\nHeavy τ  ")+Home4PanelLocal::N(Derived.TauHeavy)+TEXT("\nLight τ  ")+Home4PanelLocal::N(Derived.TauLight)+TEXT("\nLight τ − ½  ")+Home4PanelLocal::N(Derived.TauLightMargin)+
        TEXT("\nWake wavelength  ")+Home4PanelLocal::N(Derived.WakeWavelength)+TEXT(" cells\n");
    if(Derived.AllocationBytes.IsSet())Out+=FString::Printf(TEXT("\nAllocation estimate  %.2f GiB"),double(Derived.AllocationBytes.GetValue())/1073741824.);
    else Out+=TEXT("\nMemory: allocation list not supplied");
    Out+=Derived.EstimatedSeconds.IsSet()&&!Preview.Performance.MeasurementSource.IsEmpty()?FString::Printf(TEXT("\nEstimated duration  %.2f h"),Derived.EstimatedSeconds.GetValue()/3600):TEXT("\nDuration: measured throughput required");
    if(Derived.Levels.Num()>1)
    {
        Out+=TEXT("\n\nMultidomain levels");for(const auto& L:Derived.Levels)
            Out+=FString::Printf(TEXT("\nL%d ×%.0f · ν %s · σ %s\nM %s · g %s · τL %s"),L.Depth,L.Scale,*Home4PanelLocal::N(L.NuHeavy),*Home4PanelLocal::N(L.Sigma),*Home4PanelLocal::N(L.Mobility),*Home4PanelLocal::N(L.Gravity),*Home4PanelLocal::N(L.TauLight));
    }
    Out+=TEXT("\n\nChecks");if(Derived.Issues.IsEmpty())Out+=TEXT("\nNo reported issues in supplied values. Missing solver capabilities still prevent numerical execution.");
    for(const auto& I:Derived.Issues)Out+=TEXT("\n\n")+(I.Severity==EStudioHome4IssueSeverity::Blocking?FString(TEXT("BLOCK · ")):I.Severity==EStudioHome4IssueSeverity::Warning?FString(TEXT("WARN · ")):FString())+I.Field+TEXT(": ")+I.Message;
    if(!Preview.RecipeId.IsEmpty())for(const auto& D:StudioHome4Recipes::Departures(Preview))Out+=TEXT("\n\n")+D;
    return Out;
}
FString SStudioHome4Panel::Command() const
{
    if(!PreviewError.IsEmpty())return PreviewError;
    const auto* R=StudioHome4Recipes::Find(Preview.RecipeId);
    if(R&&R->Driver!=TEXT("run_hull_speed.py"))return R->Driver+TEXT("\nDriver argument contract not supplied. Export run_spec.json to retain every request.");
    FStudioHome4DriverCommand C;FString E;
    if(!StudioHome4Config::BuildHullDriverArgv(Preview,TEXT("python3"),TEXT("run_hull_speed.py"),C,E))return E;
    FString TextValue=C.Display;for(const auto& M:C.MissingContracts)TextValue+=TEXT("\n# ")+M;return TextValue;
}
void SStudioHome4Panel::Sync()
{
    Session->Refresh();for(const auto& P:Inputs)if(const auto W=P.Value.Pin())
        if(W->GetText().ToString()!=Session->Get(P.Key))W->SetText(FText::FromString(Session->Get(P.Key)));
    if(Session->Build(Preview,PreviewError))Derived=StudioHome4Config::Derive(Preview);
}
void SStudioHome4Panel::Tick(const FGeometry& G,double T,float D)
{
    SCompoundWidget::Tick(G,T,D);Session->Refresh();
    FString Values;for(const auto& F:StudioHome4Config::Fields())Values+=Session->Get(FStudioHome4Session::Key(F))+TEXT("\x1e");
    if(Values!=LastValues){LastValues=Values;Sync();}
}
void SStudioHome4Panel::ExportSpec()
{
    if(Session->IsDirty()){Session->Status=TEXT("Apply or revert edits before exporting the applied run specification.");return;}
    if(!Model->Project.Draft.Home4.IsSet()){Session->Status=TEXT("Choose a recipe or apply a HOME4 configuration first.");return;}
    FString Path;if(!StudioFileDialog::DataFile(true,TEXT("Export HOME4 run specification"),TEXT("run_spec.json"),TEXT("json"),Path))return;
    FString E;Session->Status=StudioFileDialog::WriteAtomic(Path,StudioHome4Config::Serialize(Model->Project.Draft.Home4.GetValue()),E)?TEXT("Run specification saved to ")+Path:E;
}
void SStudioHome4Panel::ImportSpec()
{
    if(Session->IsDirty()){Session->Status=TEXT("Apply or revert edits before importing a run specification.");return;}
    FString Path;if(!StudioFileDialog::DataFile(false,TEXT("Import HOME4 run specification"),TEXT(""),TEXT("json"),Path))return;
    FStudioFileAccess Access(Path);const int64 Size=IFileManager::Get().FileSize(*Path);
    if(Size<0||Size>1024*1024){Session->Status=TEXT("Run specification must be a readable JSON file under 1 MiB.");return;}
    FString E;FStudioHome4Spec S;
    if(!StudioHome4Config::Load(Path,S,E)){Session->Status=E.IsEmpty()?TEXT("Could not read the run specification."):E;return;}
    if(Model->EditCase(TEXT("Import HOME4 run specification"),[&](auto& C){C.Home4=S;})){Session->Revert();Sync();Session->Status=TEXT("Run specification imported; original recording and camera retained.");}
}
void SStudioHome4Panel::ExportReport()
{
    if(Session->IsDirty()){Session->Status=TEXT("Apply or revert edits before exporting the applied report.");return;}
    FString Parent;if(!StudioFileDialog::ExportFolder(Parent))return;
    const auto* Evidence=Validation&&Validation->ProjectId==Model->Project.Id&&Validation->CaseId==Model->Project.Draft.Id&&
        Model->Project.Draft.Home4.IsSet()&&Validation->RecipeId==Model->Project.Draft.Home4->RecipeId?Validation->Evidence.Get():nullptr;
    FString Destination,Error;Session->Status=StudioHome4Reports::Export(Parent,ReportName,Model->SnapshotProject(),Telemetry.Get(nullptr),Destination,Error,Evidence)?TEXT("Report saved to ")+Destination:Error;
}
