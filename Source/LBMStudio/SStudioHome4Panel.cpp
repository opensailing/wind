#include "SStudioHome4Panel.h"
#include "SStudioHome4Allocations.h"
#include "SStudioHome4RunControls.h"
#include "SStudioHome4Runtime.h"
#include "SStudioHome4Checkpoint.h"
#include "SStudioHome4Reports.h"
#include "StudioHome4Runtime.h"
#include "SStudioHome4Sizing.h"
#include "SStudioHome4Setup.h"
#include "StudioHome4Body.h"
#include "SStudioHome4Authoring.h"
#include "SStudioHome4Regions.h"
#include "SStudioHome4Settings.h"
#include "SStudioHome4Stiffness.h"
#include "SStudioHome4Validation.h"
#include "StudioHome4Recipes.h"
#include "StudioHome4RecipeAuthoring.h"
#include "StudioHome4RequestActions.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
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
#include "Layout/WidgetPath.h"

namespace Home4PanelLocal
{
class SEditorScroll final : public SScrollBox
{
public:
    using FArguments=SScrollBox::FArguments;
    void Construct(const FArguments& Args){SScrollBox::Construct(Args);}
    void ScrollFieldIntoView(const TSharedPtr<SWidget>& Target)
    {PendingFocus=Target;ScrollDescendantIntoView(Target,false,EDescendantScrollDestination::Center,12);}
    void OnFocusChanging(const FWeakWidgetPath& Previous,const FWidgetPath& Next,const FFocusEvent& Event)override
    {
        SScrollBox::OnFocusChanging(Previous,Next,Event);
        if(Next.IsValid()&&Next.ContainsWidget(this))PendingFocus=Next.GetLastWidget();
        else {PendingFocus.Reset();ScrollIntoViewRequest=nullptr;}
    }
    void Tick(const FGeometry& Geometry,double Time,float Delta)override
    {
        // Combo menus live outside this retained form. A closed popup target
        // must not leave a deferred scroll request against detached menu rows.
        if(ScrollIntoViewRequest)if(const auto Target=PendingFocus.Pin())
        {
            TSet<TSharedRef<SWidget>> Targets;Targets.Add(Target.ToSharedRef());
            TMap<TSharedRef<SWidget>,FArrangedWidget> Arranged;FindChildGeometries(Geometry,Targets,Arranged);
            if(!Arranged.Contains(Target.ToSharedRef()))ScrollIntoViewRequest=nullptr;
        }
        SScrollBox::Tick(Geometry,Time,Delta);
    }
private:
    TWeakPtr<SWidget> PendingFocus;
};
FStudioHome4Spec LevelMap(const FStudioHome4Spec& Root,double Scale)
{
    auto Local=Root;
    if(Local.Units.DxMeters)Local.Units.DxMeters=*Local.Units.DxMeters/Scale;
    if(Local.Units.DtSeconds)Local.Units.DtSeconds=*Local.Units.DtSeconds/Scale;
    if(Local.Reference.LengthCells)Local.Reference.LengthCells=*Local.Reference.LengthCells*Scale;
    if(Local.Reference.TimeSteps)Local.Reference.TimeSteps=*Local.Reference.TimeSteps*Scale;
    return Local;
}

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
    Model=Args._Model;Session=Args._Session;Runtime=Args._Runtime;Monitors=Args._Monitors;Authoring=Args._Authoring?Args._Authoring:MakeShared<FStudioHome4AuthoringSession>(Session);Spatial=Args._Spatial;Page=Args._Page;ImportPath=Args._ImportPath;ExportPath=Args._ExportPath;Validation=Args._Validation.IsValid()?Args._Validation:MakeShared<FStudioHome4ValidationState>();OnRecipe=Args._OnRecipe;OnFocusField=Args._OnFocusField;Telemetry=Args._Telemetry;TelemetryProvenance=Args._TelemetryProvenance;Sync();SetCanTick(true);
    auto Body=SNew(SVerticalBox);
    if(Page!=TEXT("Settings")&&Page!=TEXT("Reports"))Body->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4RecipeParameters")).AutoWrapText(true).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).Text_Lambda([this]{return FText::FromString(StudioHome4RecipeAuthoring::Parameters(Session->Get(TEXT("recipeId"))));})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,6)[SNew(STextBlock).AutoWrapText(true).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).Text_Lambda([this]{FStudioHome4Spec S;FString E;return FText::FromString(Session->Build(S,E)?StudioHome4RecipeAuthoring::Relationship(S):E);})]
        +SVerticalBox::Slot().AutoHeight()[Action(TEXT("Apply reviewed recipe relationships"),TEXT("Home4RecipeResolve"),[this]{FStudioHome4Spec S,Out;FString E;if(!CommitPending()||!Session->Build(S,E)||!StudioHome4RecipeAuthoring::Resolve(S,Out,E)||!Session->Replace(Out,E))Session->Status=E;else Session->Status=TEXT("Recipe relationships retained with explicit frontend reference convention. Review actual dimensions/physics and Apply.");Sync();})]];
    if(Page==TEXT("Lattice")||Page==TEXT("Fluids & Interface"))Body->AddSlot().AutoHeight()[SNew(SStudioHome4Sizing).Session(Session)];
    if(Page==TEXT("Validation")||Page==TEXT("Projects"))
    {
        Body->AddSlot().AutoHeight()[SNew(SStudioHome4Validation).Model(Model).State(Validation).Runtime(Runtime)];
        Body->AddSlot().AutoHeight().Padding(0,20,0,0)[Recipes()];
    }
    else
    {
        FString Previous;
        for(const auto& F:StudioHome4Config::Fields())
        {
            if(F.Page!=Page||F.Section.IsEmpty()||F.Key==TEXT("display")||F.Key==TEXT("stiffness"))continue;
            if(F.Section!=Previous)
            {Previous=F.Section;Body->AddSlot().AutoHeight().Padding(0,18,0,10)[StudioUI::Label(Home4PanelLocal::Human(Previous),13,StudioUI::Text,true)];}
            Body->AddSlot().AutoHeight().Padding(0,0,0,9)[Editor(F)];
        }
    }
    if(Page==TEXT("Lattice")||Page==TEXT("Boundaries & Zones")||Page==TEXT("Run"))Body->AddSlot().AutoHeight().Padding(0,10)[SNew(SStudioHome4Setup).Session(Session).Authoring(Authoring).Page(Page)];
    if(Page==TEXT("Lattice"))Body->AddSlot().AutoHeight()[SNew(SStudioHome4Allocations).Session(Session)];
    if(Page==TEXT("Lattice")||Page==TEXT("Boundaries & Zones"))Body->AddSlot().AutoHeight()[SNew(SStudioHome4Regions).Session(Session).Patches(Page==TEXT("Lattice"))];
    if(Page==TEXT("Geometry")||Page==TEXT("Lattice")||Page==TEXT("Bodies")||Page==TEXT("Boundaries & Zones"))Body->AddSlot().AutoHeight().Padding(0,18)[SNew(SStudioHome4Authoring).Session(Authoring).Page(Page)];
    if(Page==TEXT("Bodies"))Body->AddSlot().AutoHeight()[SNew(SStudioHome4Stiffness).Session(Session)];
    if(Spatial&&(Page==TEXT("Lattice")||Page==TEXT("Boundaries & Zones")||Page==TEXT("Geometry")))
        Body->AddSlot().AutoHeight().Padding(0,20)[SNew(SBox).HeightOverride(580)[SNew(SStudioHome4SpatialDiagnostics).Model(Model).Session(Spatial)
            .View(Page==TEXT("Lattice")?EStudioHome4SpatialView::Multidomain:Page==TEXT("Geometry")?EStudioHome4SpatialView::Geometry:EStudioHome4SpatialView::Zones)
            .OnLocate(Args._OnLocateSpatial)]];
    if(Page==TEXT("Geometry"))
    {
        auto Formats=SNew(SHorizontalBox);
        for(const TCHAR* Extension:{TEXT("step"),TEXT("iges"),TEXT("stl"),TEXT("obj")})
            Formats->AddSlot().AutoWidth().Padding(0,0,7,0)[Action(FString(TEXT("Choose "))+FString(Extension).ToUpper()+TEXT("…"),FName(*(FString(TEXT("Home4CAD."))+Extension)),[this,Ext=FString(Extension)]
            {FString Path;if(StudioFileDialog::DataFile(false,TEXT("Select body geometry"),TEXT(""),Ext,Path)){Session->Set(TEXT("geometry.sourcePath"),Path);Session->Set(TEXT("authoring.primitive"),TEXT(""));Session->Set(TEXT("authoring.sourceSHA256"),TEXT(""));Sync();}})];
        Body->InsertSlot(0).AutoHeight().Padding(0,0,0,12)[Formats];
        auto Bindings=SNew(SVerticalBox);for(const auto& Asset:Model->Project.Draft.Geometry)Bindings->AddSlot().AutoHeight().Padding(0,4)[Action(TEXT("Bind verified project geometry: ")+Asset.Name,FName(*(TEXT("Home4BindGeometry.")+Asset.Id.ToString())),[this,Asset]{FStudioHome4Spec Base,Out;FString E;if(!Session->Build(Base,E)||!StudioHome4Body::Bind(Asset,Base,Out,E)||!Session->Replace(Out,E))Session->Status=E;else Session->Status=TEXT("Bound original geometry identity and explicit source length. Review pose and Apply.");Sync();})];Body->InsertSlot(1).AutoHeight()[Bindings];

    }
    if(Page==TEXT("Bodies"))
    {
        Body->InsertSlot(0).AutoHeight().Padding(0,0,0,12)[Home4PanelLocal::Paragraph(TEXT("Body forces are inspected in Monitors as separate pressure, viscous and momentum-exchange channels. The geometric preview animates declared body kinematics and measures local SDF/link preparation cost. Hydrodynamic body response remains with the numerical solver adapter."),StudioUI::Muted)];
        Body->AddSlot().AutoHeight().Padding(0,18)[Home4PanelLocal::Paragraph(TEXT("Retabulation cost reference\nThe supplied HOME4 note reports 47.8 MLUPS for a static mask, 5.9 for an oscillating cylinder and 0.63 for sedimentation on an M1 Pro. These are different measured workloads, not a prediction for this case."),StudioUI::Muted)];
    }
    if(Page==TEXT("Boundaries & Zones"))
        Body->InsertSlot(0).AutoHeight().Padding(0,0,0,12)[Home4PanelLocal::Paragraph(TEXT("Zone extents and strength profiles belong to the run configuration. Driver-specific widths remain labeled as driver values until the solver supplies their coordinate convention. Original field masks are inspected in Fields."),StudioUI::Muted)];
    if(Page==TEXT("Settings"))
    {
        Body->AddSlot().AutoHeight()[SNew(SStudioHome4Settings).Model(Model)];
        Body->AddSlot().AutoHeight().Padding(0,0,0,10)[StudioUI::Label(TEXT("Readout units"),14,StudioUI::Text,true)];
        Body->AddSlot().AutoHeight().Padding(0,0,0,18)[Home4PanelLocal::Paragraph(TEXT("Use LU / SI / ND in the header to set the global readout mode. Converted values offer all three forms in their tooltip. A recording must supply its original unit map; editing the next-run map cannot change old results."))];
        Body->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Viewer preferences"),14,StudioUI::Text,true)];
        Body->AddSlot().AutoHeight().Padding(0,8)[Home4PanelLocal::Paragraph(TEXT("Palette, range, camera projection, clipping and layer visibility are saved with each project view. Edit them in the Fields inspector; source ranges and provenance remain attached to the original recording."))];
    }
    if(Page==TEXT("Fluids & Interface"))Body->AddSlot().AutoHeight().Padding(0,12)[Action(TEXT("Apply documented safeguard values"),TEXT("Home4SafeguardsPreset"),[this]{FStudioHome4Spec S,Out;FString E;if(!CommitPending()||!Session->Build(S,E)||!StudioHome4RequestActions::Safeguards(S,Out,E)||!Session->Replace(Out,E))Session->Status=E;else Session->Status=TEXT("Documented safeguards retained: gradient 1.6, force 60, light force 0.6, phase cutoff 0.1. Review and Apply.");Sync();})];
    if(Page==TEXT("Run"))
    {
        auto Presets=SNew(SHorizontalBox);
        Presets->AddSlot().FillWidth(1).Padding(0,0,6,0)[Action(TEXT("Smoke: 256 steps / every 64"),TEXT("Home4SmokePreset"),[this]{FStudioHome4Spec S,Out;FString E;if(!CommitPending()||!Session->Build(S,E)||!StudioHome4RequestActions::Smoke(S,256,64,Out,E)||!Session->Replace(Out,E))Session->Status=E;else Session->Status=TEXT("Reviewed frontend smoke preset: 256 steps, all outputs every 64, fresh destinations. Apply before queueing.");Sync();})];
        Presets->AddSlot().FillWidth(1)[Action(TEXT("Create _viz figure request"),TEXT("Home4FigurePreset"),[this]{FStudioHome4Spec S,Out;FString E;if(!CommitPending()||!Session->Build(S,E)||!StudioHome4RequestActions::Figure(S,Out,E)||!Session->Replace(Out,E))Session->Status=E;else Session->Status=TEXT("Figure request retained with _viz tag and fresh output destinations. Review duration/cadences and Apply.");Sync();})];
        Body->AddSlot().AutoHeight().Padding(0,10)[Presets];
        if(Runtime){Body->AddSlot().AutoHeight().Padding(0,18)[SNew(SStudioHome4Runtime).Model(Model).Runtime(Runtime)];Body->AddSlot().AutoHeight().Padding(0,18)[SNew(SStudioHome4Checkpoint).Model(Model).Editor(Session).Session(Runtime->Checkpoint())];}
        else Body->InsertSlot(0).AutoHeight().Padding(0,0,0,18)[SNew(SStudioHome4RunControls).Model(Model).OnSubmit(Args._OnSubmit)];
        Body->AddSlot().AutoHeight().Padding(0,18,0,8)[StudioUI::Label(TEXT("Equivalent command line"),13,StudioUI::Text,true)];
        Body->AddSlot().AutoHeight()[SNew(SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).Font(StudioUI::Font(10))
            .Text_Lambda([this]{return FText::FromString(Command());})];
        Body->AddSlot().AutoHeight().Padding(0,8)[Action(TEXT("Copy command"),TEXT("Home4CopyCLI"),[this]{FPlatformApplicationMisc::ClipboardCopy(*Command());Session->Status=TEXT("Command copied. No solver process was started.");})];
        Body->AddSlot().AutoHeight().Padding(0,10)[Home4PanelLocal::Paragraph(TEXT("Development adapter: use the header controls to exercise Run, Pause, Stop and Step. It computes no CFD, writes no restart state and supplies no scientific measurements. Metal, CUDA and rack execution await the solver adapter."),StudioUI::Amber)];
    }
    if(Page==TEXT("Reports"))Body->AddSlot().AutoHeight()[SNew(SStudioHome4Reports).Model(Model).Runtime(Runtime).Validation(Validation).Spatial(Spatial).Monitors(Monitors)];
    ChildSlot[SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(22)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[StudioUI::Label(Page,23,StudioUI::Text,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,16)[Home4PanelLocal::Paragraph(
                Page==TEXT("Validation")?TEXT("Start from a documented recipe. Gates require identified reference data and measured results."):
                TEXT("HOME4 D3Q27 · retained case settings. Blank values remain unspecified; Apply commits one undoable edit."),StudioUI::Muted)]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,22,0)[SAssignNew(EditorScroll,Home4PanelLocal::SEditorScroll).NavigationScrollPadding(12).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                    +SScrollBox::Slot()[Body]]
                +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(280)[Feasibility()]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,8)[SNew(SSeparator)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4Apply")).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(12,8))
                    .IsEnabled_Lambda([this]{return Session->IsDirty()&&!Session->HasConflict();})
                    .OnClicked_Lambda([this]{if(CommitPending())Session->Apply();Sync();return FReply::Handled();})[StudioUI::Label(TEXT("Apply configuration"),10,StudioUI::Cyan)]]
                +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[Action(TEXT("Revert edits"),TEXT("Home4Revert"),[this]{Session->Revert();Sync();})]
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[Action(TEXT("Import run spec…"),TEXT("Home4Import"),[this]{ImportSpec();})]
                +SHorizontalBox::Slot().AutoWidth()[Action(TEXT("Export run spec…"),TEXT("Home4Export"),[this]{ExportSpec();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,9,0,0)[SNew(STextBlock).Tag(TEXT("Home4Status")).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Amber).AutoWrapText(true)
                .Text_Lambda([this]{return FText::FromString(!Session->Status.IsEmpty()?Session->Status:PreviewError);})]]];
    Sync();
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
            .Text(FText::FromString(Session->DisplayText(F,FieldDisplay(F)))).HintText(FText::FromString(F.bOptional?TEXT("Not specified"):TEXT("")))
            .OnTextChanged_Lambda([this,F,K](const FText& T)
            {if(bSyncing)return;const auto D=FieldDisplay(F);if(D==EStudioHome4UnitDisplay::Lattice){Session->Set(K,T.ToString());return;}auto Pending=Session->Pending(TEXT("panel.units"));Pending.Add(K+TEXT(".text"),T.ToString());Pending.Add(K+TEXT(".display"),LexToString(int32(D)));Session->RetainPending(TEXT("panel.units"),Pending);})
            .OnTextCommitted_Lambda([this](const FText&,ETextCommit::Type Type){if(CommitPending()&&Type==ETextCommit::OnEnter)Session->Apply();Sync();});
        Inputs.Add(K,Edit);
    }
    FieldTargets.Add(K,Input);
    Input->SetToolTipText(TAttribute<FText>::CreateLambda([this,F]{return FText::FromString(Session->FieldTooltip(F));}));
    return SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(185)[Home4PanelLocal::Paragraph(F.Label==F.Key?Home4PanelLocal::Human(F.Key):F.Label)]]
            +SHorizontalBox::Slot().FillWidth(1).Padding(8,0)[Input.ToSharedRef()]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(83)[SNew(STextBlock).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true).Text_Lambda([this,F]{return FText::FromString(FieldUnit(F));})]]]
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
            +SVerticalBox::Slot().AutoHeight().Padding(0,6)[Home4PanelLocal::Paragraph(StudioHome4RecipeAuthoring::Parameters(R.Id),StudioUI::Muted)]
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
        Values->AddSlot().AutoHeight().Padding(0,0,0,9)[SNew(STextBlock).Tag(FName(*(FString(TEXT("Home4FeasibilityValue_"))+Name))).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Cyan).AutoWrapText(true)
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
            .ToolTipText_Lambda([this]{return FText::FromString(SummaryTooltip());})
            .Text_Lambda([this]{return FText::FromString(Summary());})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SAssignNew(IssueRows,SVerticalBox)]];
}
FString SStudioHome4Panel::Summary() const
{
    if(!PreviewError.IsEmpty())return TEXT("Correct the draft to calculate feasibility.\n\n")+PreviewError;
    FString Out=TEXT("Lattice Mach  ")+Home4PanelLocal::N(Derived.Mach)+TEXT("\nReynolds  ")+Home4PanelLocal::N(Derived.Reynolds)+TEXT("\nFroude  ")+Home4PanelLocal::N(Derived.Froude)+
        TEXT("\nBond / Weber  ")+Home4PanelLocal::N(Derived.Bond)+TEXT(" / ")+Home4PanelLocal::N(Derived.Weber)+TEXT("\nCapillary  ")+Home4PanelLocal::N(Derived.Capillary)+
        TEXT("\nPeclet / Cahn  ")+Home4PanelLocal::N(Derived.Peclet)+TEXT(" / ")+Home4PanelLocal::N(Derived.Cahn)+TEXT("\nAtwood  ")+Home4PanelLocal::N(Derived.Atwood)+
        TEXT("\n\nHeavy τ  ")+Home4PanelLocal::N(Derived.TauHeavy)+TEXT("\nLight τ  ")+Home4PanelLocal::N(Derived.TauLight)+TEXT("\nLight τ − ½  ")+Home4PanelLocal::N(Derived.TauLightMargin)+
        TEXT("\nWake wavelength  ")+(Derived.WakeWavelength?StudioHome4Readouts::Value(*Derived.WakeWavelength,EStudioHome4Quantity::Length,EStudioHome4UnitDisplay::Lattice,Model->UnitDisplay,&Preview):FString(TEXT("Not supplied")))+TEXT("\n");
    if(Derived.AllocationBytes.IsSet())Out+=FString::Printf(TEXT("\nAllocation estimate  %.2f GiB"),double(Derived.AllocationBytes.GetValue())/1073741824.);
    else Out+=TEXT("\nMemory: allocation list not supplied");
    Out+=Derived.EstimatedSeconds.IsSet()&&!Preview.Performance.MeasurementSource.IsEmpty()?FString::Printf(TEXT("\nEstimated duration  %.2f h"),Derived.EstimatedSeconds.GetValue()/3600):TEXT("\nDuration: measured throughput required");
    if(Derived.Levels.Num()>1)
    {
        Out+=TEXT("\n\nMultidomain levels");for(const auto& L:Derived.Levels)
        {
            const auto Local=Home4PanelLocal::LevelMap(Preview,L.Scale);
            auto Read=[&](const TOptional<double>& V,EStudioHome4Quantity Q){return V?StudioHome4Readouts::Value(*V,Q,EStudioHome4UnitDisplay::Lattice,Model->UnitDisplay,&Local):FString(TEXT("Not supplied"));};
            Out+=FString::Printf(TEXT("\nL%d ×%.0f · ν %s · σ %s\nM %s · g %s · τL %s"),L.Depth,L.Scale,*Read(L.NuHeavy,EStudioHome4Quantity::KinematicViscosity),*Read(L.Sigma,EStudioHome4Quantity::SurfaceTension),*Read(L.Mobility,EStudioHome4Quantity::Mobility),*Read(L.Gravity,EStudioHome4Quantity::Acceleration),*Home4PanelLocal::N(L.TauLight));
        }
    }
    Out+=TEXT("\n\nChecks");if(Derived.Issues.IsEmpty())Out+=TEXT("\nNo reported issues in supplied values. Missing solver capabilities still prevent numerical execution.");
    for(const auto& I:Derived.Issues)Out+=TEXT("\n\n")+(I.Severity==EStudioHome4IssueSeverity::Blocking?FString(TEXT("BLOCK · ")):I.Severity==EStudioHome4IssueSeverity::Warning?FString(TEXT("WARN · ")):FString())+I.Field+TEXT(": ")+I.Message;
    if(!Preview.RecipeId.IsEmpty())for(const auto& D:StudioHome4Recipes::Departures(Preview))Out+=TEXT("\n\n")+D;
    return Out;
}
FString SStudioHome4Panel::SummaryTooltip() const
{
    FString TextValue=TEXT("Derived Kn ≈ Ma/Re: ")+Home4PanelLocal::N(Derived.Knudsen);
    if(Derived.WakeWavelength)TextValue+=TEXT("\nWake wavelength:\n")+StudioHome4Readouts::Tooltip(*Derived.WakeWavelength,EStudioHome4Quantity::Length,EStudioHome4UnitDisplay::Lattice,&Preview);
    for(const auto& L:Derived.Levels)
    {
        const auto Local=Home4PanelLocal::LevelMap(Preview,L.Scale);TextValue+=FString::Printf(TEXT("\nLevel %d · dx and dt divided by %.0f"),L.Depth,L.Scale);
        auto Add=[&](const TCHAR* Label,const TOptional<double>& V,EStudioHome4Quantity Q){if(V)TextValue+=TEXT("\n")+FString(Label)+TEXT(":\n")+StudioHome4Readouts::Tooltip(*V,Q,EStudioHome4UnitDisplay::Lattice,&Local);};
        Add(TEXT("νH"),L.NuHeavy,EStudioHome4Quantity::KinematicViscosity);Add(TEXT("σ"),L.Sigma,EStudioHome4Quantity::SurfaceTension);Add(TEXT("M"),L.Mobility,EStudioHome4Quantity::Mobility);Add(TEXT("g"),L.Gravity,EStudioHome4Quantity::Acceleration);
    }
    return TextValue;
}
FString SStudioHome4Panel::Command() const
{
    if(!PreviewError.IsEmpty())return PreviewError;
    const auto* R=StudioHome4Recipes::Find(Preview.RecipeId);
    if(R&&R->Driver!=TEXT("run_hull_speed.py"))return StudioHome4RequestActions::CompleteProtocol(Preview);
    FStudioHome4DriverCommand C;FString E;
    if(!StudioHome4Config::BuildHullDriverArgv(Preview,TEXT("python3"),TEXT("run_hull_speed.py"),C,E))return E;
    FString TextValue=StudioHome4RequestActions::CompleteProtocol(Preview)+TEXT("\n\nVerified hull-driver argument preview:\n")+C.Display;for(const auto& M:C.MissingContracts)TextValue+=TEXT("\n# ")+M;return TextValue;
}
EStudioHome4UnitDisplay SStudioHome4Panel::FieldDisplay(const FStudioHome4Field& F)const
{
    const auto Pending=Session->Pending(TEXT("panel.units"));const FString Key=FStudioHome4Session::Key(F);
    if(const auto* D=Pending.Find(Key+TEXT(".display")))return EStudioHome4UnitDisplay(FCString::Atoi(**D));
    const auto Wanted=Model?Model->UnitDisplay:EStudioHome4UnitDisplay::Lattice;
    if(!F.bQuantity||Wanted==EStudioHome4UnitDisplay::Lattice)return EStudioHome4UnitDisplay::Lattice;
    FStudioHome4Spec Map;FString E;if(!Session->Build(Map,E,true)||!StudioHome4Config::FieldConversion(1,F,EStudioHome4UnitDisplay::Lattice,Wanted,Map))return EStudioHome4UnitDisplay::Lattice;
    return Wanted;
}
FString SStudioHome4Panel::FieldUnit(const FStudioHome4Field& F)const
{
    const auto D=FieldDisplay(F);const auto Wanted=Model?Model->UnitDisplay:EStudioHome4UnitDisplay::Lattice;
    if(!F.bQuantity)return F.Unit;
    if(D==EStudioHome4UnitDisplay::Lattice)return F.Unit+(Wanted!=D?TEXT(" (map required)"):TEXT(""));
    return StudioHome4Readouts::Unit(F.Quantity,D);
}
bool SStudioHome4Panel::CommitPending()
{
    auto Pending=Session->Pending(TEXT("panel.units"));
    for(const auto& F:StudioHome4Config::Fields())
    {
        const FString K=FStudioHome4Session::Key(F);const auto* T=Pending.Find(K+TEXT(".text"));if(!T)continue;
        const auto* D=Pending.Find(K+TEXT(".display"));FString E;
        if(!D||!Session->SetDisplayText(F,*T,EStudioHome4UnitDisplay(FCString::Atoi(**D)),E)){Session->Status=E;return false;}
        Pending.Remove(K+TEXT(".text"));Pending.Remove(K+TEXT(".display"));Session->RetainPending(TEXT("panel.units"),Pending);
    }
    return true;
}
void SStudioHome4Panel::Sync()
{
    Session->Refresh();bSyncing=true;const auto Pending=Session->Pending(TEXT("panel.units"));
    for(const auto& F:StudioHome4Config::Fields())if(const auto* Input=Inputs.Find(FStudioHome4Session::Key(F)))if(const auto W=Input->Pin())
    {
        const FString K=FStudioHome4Session::Key(F);const auto* P=Pending.Find(K+TEXT(".text"));
        const FString TextValue=P?*P:Session->DisplayText(F,FieldDisplay(F));
        if(W->GetText().ToString()!=TextValue)W->SetText(FText::FromString(TextValue));
    }
    bSyncing=false;
    if(Session->Build(Preview,PreviewError))Derived=StudioHome4Config::Derive(Preview);
    else { Preview=FStudioHome4Spec();Derived=FStudioHome4Derived(); }
    if(IssueRows)
    {
        IssueRows->ClearChildren();TSet<FString> Added;
        for(const auto& Issue:Derived.Issues)
        {
            FString Key=Issue.Field;if(Key==TEXT("fluids.nu"))Key=TEXT("fluids.nuHeavy");if(Key==TEXT("fluids.phaseRates"))Key=TEXT("fluids.phaseST");if(Key==TEXT("fluids.safeguards"))Key=TEXT("fluids.gradientLimiter");
            const auto* Field=StudioHome4Config::Fields().FindByPredicate([&](const auto& F){return FStudioHome4Session::Key(F)==Key;});if(!Field||Added.Contains(Key))continue;Added.Add(Key);const FString Destination=Field->Page;
            IssueRows->AddSlot().AutoHeight().Padding(0,3)[Action(TEXT("Edit ")+Field->Label,FName(*(TEXT("Home4Issue.")+Key)),[this,Key,Destination]{if(OnFocusField.IsBound())OnFocusField.Execute(Destination,Key);else if(Destination==Page){PendingLocalFocus=Key;LocalFocusRetries=120;FocusField(Key);}else Session->Status=TEXT("Open ")+Destination+TEXT(" to edit ")+Key;})];
        }
    }
}
void SStudioHome4Panel::Tick(const FGeometry& G,double T,float D)
{
    SCompoundWidget::Tick(G,T,D);Session->Refresh();
    FString Values=LexToString(Model?int32(Model->UnitDisplay):0)+TEXT("/")+LexToString(Session->HasPending());for(const auto& F:StudioHome4Config::Fields())Values+=Session->Get(FStudioHome4Session::Key(F))+TEXT("\x1e");
    for(int32 Row=0;Row<Session->AllocationCount();++Row)
        for(int32 Column=0;Column<6;++Column)Values+=Session->AllocationValue(Row,Column)+TEXT("\x1e");
    if(Values!=LastValues){LastValues=Values;Sync();}
    if(!PendingLocalFocus.IsEmpty()){if(FocusField(PendingLocalFocus)||--LocalFocusRetries<=0)PendingLocalFocus.Empty();}
}
void SStudioHome4Panel::ExportSpec()
{
    if(Session->IsDirty()){Session->Status=TEXT("Apply or revert edits before exporting the applied run specification.");return;}
    if(!Model->Project.Draft.Home4.IsSet()){Session->Status=TEXT("Choose a recipe or apply a HOME4 configuration first.");return;}
    FString Path=ExportPath;if(Path.IsEmpty()){if(FParse::Param(FCommandLine::Get(),TEXT("StudioHeadlessTests"))){Session->Status=TEXT("Headless export requires an explicit output path.");return;}if(!StudioFileDialog::DataFile(true,TEXT("Export HOME4 run specification"),TEXT("run_spec.json"),TEXT("json"),Path))return;}
    FString E;Session->Status=StudioProjectIO::WriteAtomic(Path,StudioHome4Config::Serialize(Model->Project.Draft.Home4.GetValue()),E)?TEXT("Run specification saved to ")+Path:E;
}
void SStudioHome4Panel::ImportSpec()
{
    if(Session->IsDirty()){Session->Status=TEXT("Apply or revert edits before importing a run specification.");return;}
    FString Path=ImportPath;if(Path.IsEmpty()){if(FParse::Param(FCommandLine::Get(),TEXT("StudioHeadlessTests"))){Session->Status=TEXT("Headless import requires an explicit source path.");return;}if(!StudioFileDialog::DataFile(false,TEXT("Import HOME4 run specification"),TEXT(""),TEXT("json"),Path))return;}
    FStudioFileAccess Access(Path);const int64 Size=IFileManager::Get().FileSize(*Path);
    if(Size<0||Size>1024*1024){Session->Status=TEXT("Run specification must be a readable JSON file under 1 MiB.");return;}
    FString E;FStudioHome4Spec S;
    if(!StudioHome4Config::Load(Path,S,E)){Session->Status=E.IsEmpty()?TEXT("Could not read the run specification."):E;return;}
    for(FString* Input:{&S.Geometry.SourcePath,&S.Geometry.CptPath,&S.Run.InitState})if(!Input->IsEmpty()&&FPaths::IsRelative(*Input))*Input=FPaths::ConvertRelativePathToFull(FPaths::GetPath(Path),*Input);
    if(Model->EditCase(TEXT("Import HOME4 run specification"),[&](auto& C){C.Home4=S;})){Session->Revert();Sync();Session->Status=TEXT("Run specification imported; original recording and camera retained.");}
}
void SStudioHome4Panel::ExportReport()
{
    if(Session->IsDirty()){Session->Status=TEXT("Apply or revert edits before exporting the applied report.");return;}
    FString Parent;if(!StudioFileDialog::ExportFolder(Parent))return;
    const auto* Evidence=Validation&&Validation->ProjectId==Model->Project.Id&&Validation->CaseId==Model->Project.Draft.Id&&
        Model->Project.Draft.Home4.IsSet()&&Validation->RecipeId==Model->Project.Draft.Home4->RecipeId?Validation->Evidence.Get():nullptr;
    if(Spatial)Spatial->Poll();
    const auto SpatialEvidence=Spatial?Spatial->Evidence():TSharedPtr<const FStudioHome4SpatialEvidence,ESPMode::ThreadSafe>();
    const auto ScienceProvenance=TelemetryProvenance.Get(TOptional<FStudioHome4TelemetryProvenance>());
    FString Destination,Error;Session->Status=StudioHome4Reports::Export(Parent,ReportName,Model->SnapshotProject(),Telemetry.Get(nullptr),Destination,Error,Evidence,SpatialEvidence.Get(),ScienceProvenance?&ScienceProvenance.GetValue():nullptr)?TEXT("Report saved to ")+Destination:Error;
}

bool SStudioHome4Panel::HasField(const FString& Key)const
{const auto* Found=FieldTargets.Find(Key);return Found&&Found->IsValid();}
bool SStudioHome4Panel::FocusField(const FString& Key)
{
    const auto* Found=FieldTargets.Find(Key);const auto Target=Found?Found->Pin():TSharedPtr<SWidget>();if(!Target||!Target->IsEnabled())return false;
    // Offscreen controls may not have been painted yet. Arrange/scroll them
    // before requiring cached target geometry, then retry after layout.
    if(EditorScroll)
    {
        const auto& G=EditorScroll->GetCachedGeometry();if(G.GetLocalSize().Y<=0)return false;const auto At=G.AbsoluteToLocal(Target->GetCachedGeometry().GetAbsolutePosition());const double Height=Target->GetCachedGeometry().GetLocalSize().Y;
        if(Target->GetCachedGeometry().GetLocalSize().GetMin()<=0||At.Y<0||At.Y+Height>G.GetLocalSize().Y){StaticCastSharedPtr<Home4PanelLocal::SEditorScroll>(EditorScroll)->ScrollFieldIntoView(Target);EditorScroll->Tick(G,FPlatformTime::Seconds(),0);return false;}
    }
    if(Target->GetCachedGeometry().GetLocalSize().GetMin()<=0)return false;
    TFunction<TSharedPtr<SWidget>(const TSharedRef<SWidget>&)> Keyboard=[&](const TSharedRef<SWidget>& W)->TSharedPtr<SWidget>{if(W->SupportsKeyboardFocus())return W;auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto T=Keyboard(C->GetChildAt(I)))return T;return {};};
    const auto Focus=Keyboard(Target.ToSharedRef());if(!Focus)return false;FSlateApplication::Get().SetKeyboardFocus(Focus,EFocusCause::Navigation);return Focus->HasKeyboardFocus()||Focus->HasFocusedDescendants();
}
