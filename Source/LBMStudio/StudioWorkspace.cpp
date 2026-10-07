/*
THESIS: Camera and display history let users explore and return without losing their working case.
OWN-WORLD: Inherit the reference's dense native Slate shell, blue-black panels, fine borders and cyan focus.
STORY: Manipulate a view, undo one gesture, redo it, continue replay independently.
FIRST VIEWPORT: Keep the scene and inspector layout; compact Undo view / Redo view controls join Camera settings and its existing 330-unit popover.
FORM: Local Operate extension of the approved screenshot; no new visual world or seed applies.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
#include "StudioWorkspace.h"
#include "StudioTheme.h"
#include "SStudioHome4Panel.h"
#include "SStudioHome4Monitors.h"
#include "SStudioHome4Lineage.h"
#include "StudioHome4Validation.h"
#include "StudioHome4Readouts.h"
#include "StudioHome4Recipes.h"
#include "SStudioFlowConditions.h"
#include "SStudioHelpPanel.h"
#include "SStudioNotifications.h"
#include "SStudioCommandInput.h"
#include "SStudioPerformancePanel.h"
#include "SStudioResultsWorkspace.h"
#include "SStudioPipelineWorkspace.h"
#include "StudioFlowViewport.h"
#include "SStudioFloatingLayer.h"
#include "StudioMenuButton.h"
#include "StudioScene.h"
#include "StudioOrientation.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "StudioFileDialog.h"
#include "StudioFieldExportUI.h"
#include "StudioSnapshotUI.h"
#include "StudioProbeScheduler.h"
#include "StudioProbeMarkers.h"
#include "StudioInspectionOverlay.h"
#include "StudioProbeProfile.h"
#include "SStudioProbeProfile.h"
#include "StudioPointRecording.h"
#include "StudioMaterials.h"
#include "StudioBoundaries.h"
#include "StudioMonitorChart.h"
#include "StudioMonitorExport.h"
#include "StudioProbeMonitor.h"
#include "StudioLogExport.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "StudioBoundarySelection.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Fonts/FontMeasure.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Input/SComboButton.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SEditableText.h"
#include "Layout/WidgetPath.h"
#include "Widgets/SLeafWidget.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"
#include "Rendering/DrawElements.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include <charconv>
#include "HAL/PlatformProcess.h"
#include <limits>

// Retained inspector forms can be hidden while a deferred focus-scroll is queued.
// A request for a control in a closed category/menu no longer belongs to the visible form.
class SRetainedFormScrollBox final : public SScrollBox
{
public:
    using FArguments=SScrollBox::FArguments;
    void Construct(const FArguments& Args){SScrollBox::Construct(Args);}
    void OnFocusChanging(const FWeakWidgetPath& Previous,const FWidgetPath& Next,const FFocusEvent& Event) override
    {
        SScrollBox::OnFocusChanging(Previous,Next,Event);
        if(Next.IsValid()&&Next.ContainsWidget(this))PendingFocus=Next.GetLastWidget();
        else {PendingFocus.Reset();ScrollIntoViewRequest=nullptr;}
    }
    void Tick(const FGeometry& Geometry,double Time,float Delta) override
    {
        // A field/palette switch can collapse the focused branch before Slate
        // executes its deferred request, without delivering a new focus path.
        if(ScrollIntoViewRequest)
        {
            if(const auto Target=PendingFocus.Pin())
            {
                TSet<TSharedRef<SWidget>> Targets;Targets.Add(Target.ToSharedRef());
                TMap<TSharedRef<SWidget>,FArrangedWidget> Arranged;FindChildGeometries(Geometry,Targets,Arranged);
                if(!Arranged.Contains(Target.ToSharedRef()))ScrollIntoViewRequest=nullptr;
            }
        }
        SScrollBox::Tick(Geometry,Time,Delta);
    }
private:
    TWeakPtr<SWidget> PendingFocus;
};

namespace StudioUI
{
    const FLinearColor BG=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("07121D")));
    const FLinearColor Panel=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("101E2B")));
    const FLinearColor Raised=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("182735")));
    const FLinearColor Line=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("304151")));
    const FLinearColor Text=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("E0E9F3")));
    const FLinearColor Muted=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("99ACBF")));
    const FLinearColor Cyan=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("00C8EC")));
    const FLinearColor Blue=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("2980FF")));
    const FLinearColor Green=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("43D981")));
    const FLinearColor Amber=FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("F0B74B")));
    const FSlateColorBrush Background(BG),PanelBrush(Panel),RaisedBrush(Raised),LineBrush(Line),White(FLinearColor::White);
    const TCHAR* GeometrySaveGuard=TEXT("Apply or revert object edits before saving or replacing this project.");
    const TCHAR* RemovedGeometrySaveGuard=TEXT("Discard the removed object's edits before saving or replacing this project.");
    const TCHAR* MaterialSaveGuard=TEXT("Apply or revert material properties before saving, closing or replacing the project.");
    const TCHAR* DomainSaveGuard=TEXT("Apply or revert the domain edits before saving or replacing this project.");
    const TCHAR* LatticeSaveGuard=TEXT("Apply or revert lattice edits before saving or replacing this project.");
    const TCHAR* BoundarySaveGuard=TEXT("Apply or revert boundary edits before saving or replacing this project.");
    const TCHAR* RunSettingsSaveGuard=TEXT("Apply or revert run parameters before saving, replacing the project or starting a new control run.");
    void ResolveSaveNotice(FStudioModel& Model,const TCHAR* Guard,const FString& Resolution)
    {
        // Reverting a form resolves its own save warning without hiding a newer job or file error.
        if(Model.Notice==Guard)Model.Notice=Resolution;
    }
    FSlateFontInfo Font(int32 Size,bool Bold) { return FCoreStyle::GetDefaultFontStyle(Bold?"Bold":"Regular",Size); }
    const FButtonStyle& ButtonStyle()
    {
        static FButtonStyle S=FButtonStyle()
            .SetNormal(FSlateRoundedBoxBrush(Raised,4.f,Line,1.f))
            .SetHovered(FSlateRoundedBoxBrush(FLinearColor(0.035,0.10,0.16),4.f,Cyan,1.f))
            .SetPressed(FSlateRoundedBoxBrush(FLinearColor(0.01,0.055,0.09),4.f,Cyan,1.f))
            .SetDisabled(FSlateRoundedBoxBrush(BG,4.f,Line,1.f))
            .SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0));
        return S;
    }
    const FButtonStyle& NavigationStyle()
    {
        static FButtonStyle S=FButtonStyle()
            .SetNormal(FSlateColorBrush(FLinearColor::Transparent))
            .SetHovered(FSlateColorBrush(Raised)).SetPressed(FSlateColorBrush(Raised))
            .SetDisabled(FSlateColorBrush(FLinearColor::Transparent))
            .SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0));
        return S;
    }
    const FEditableTextBoxStyle& InputStyle()
    {
        static FEditableTextBoxStyle S=FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
        static bool Init=false;
        if(!Init) { S.SetBackgroundImageNormal(FSlateRoundedBoxBrush(BG,3.f,Line,1.f)).SetBackgroundImageHovered(FSlateRoundedBoxBrush(BG,3.f,Muted,1.f)).SetBackgroundImageFocused(FSlateRoundedBoxBrush(BG,3.f,Cyan,1.f)).SetForegroundColor(Text).SetPadding(FMargin(7,5)); Init=true; }
        return S;
    }
    // Slate's default hint opacity dims the already-muted search foreground.
    class SProjectFilterBox : public SEditableTextBox
    {
    public:
        void Construct(const FArguments& Args)
        { SEditableTextBox::Construct(Args); EditableText->SetHintTextOpacity(1.f); }
    };
    FString ExactViewNumber(double Value)
    {
        // Shortest decimal which parses back to this exact double. Fixed
        // significant-digit output pads simple values with binary roundoff.
        ANSICHAR Buffer[128];const auto Result=std::to_chars(Buffer,Buffer+UE_ARRAY_COUNT(Buffer)-1,Value);
        if(Result.ec==std::errc()){*Result.ptr='\0';return FString(UTF8_TO_TCHAR(Buffer));}
        return FString::Printf(TEXT("%.17g"),Value);
    }
    // Exact display drafts survive rejection, but external view restores replace
    // them. A text binding would silently replace rejected text with the model.
    class SValidatedViewNumber : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SValidatedViewNumber):_Minimum(0),_Maximum(0),_Integer(false),_Digits(8){}
            SLATE_ARGUMENT(FName,InputTag)
            SLATE_ARGUMENT(TFunction<double()>,Read)
            SLATE_ARGUMENT(TFunction<uint64()>,Revision)
            SLATE_ARGUMENT(TFunction<void(double)>,Write)
            SLATE_ARGUMENT(TFunction<bool(double)>,TryWrite)
            SLATE_ARGUMENT(FString,Caption)
            SLATE_ARGUMENT(TFunction<bool()>,Current)
            SLATE_ARGUMENT(TFunction<void(FString)>,ReportError)
            SLATE_ARGUMENT(double,Minimum)
            SLATE_ARGUMENT(double,Maximum)
            SLATE_ARGUMENT(bool,Integer)
            SLATE_ARGUMENT(int32,Digits)
        SLATE_END_ARGS()
        void Construct(const FArguments& Args)
        {
            Read=Args._Read;Revision=Args._Revision;Write=Args._Write;TryWrite=Args._TryWrite;Caption=Args._Caption;Current=Args._Current;
            ReportError=Args._ReportError;Min=Args._Minimum;Max=Args._Maximum;bInteger=Args._Integer;Digits=FMath::Clamp(Args._Digits,1,17);SetCanTick(true);
            ChildSlot[SAssignNew(Editor,SProjectFilterBox).Tag(Args._InputTag).Style(&InputStyle()).Font(Font(10))
                .SelectAllTextWhenFocused(true).SelectAllTextOnCommit(true).ClearKeyboardFocusOnCommit(false)
                .ToolTipText_Lambda([this]{return FText::FromString(TEXT("Value: ")+ExactViewNumber(Read())+TEXT(". Enter a value and press Enter to apply."));})
                .OnTextChanged_Lambda([this](const FText&)
                {if(!bSynchronizing)ReportError(FString());})
                .OnTextCommitted_Lambda([this](const FText& Value,ETextCommit::Type How)
                {
                    if(How!=ETextCommit::OnEnter)return;
                    if(!Current()||Revision()!=ObservedRevision){Synchronize();return;}
                    double Number=0;
                    if(!StudioColor::ParseNumber(Value.ToString(),Number)||Number<Min||Number>Max||(bInteger&&FMath::FloorToDouble(Number)!=Number))
                    {
                        const FString Name=Caption.IsEmpty()?(bInteger?TEXT("Samples"):TEXT("Scale")):Caption;
                        const FString Error=bInteger?FString::Printf(TEXT("%s: enter a whole number from %.0f to %.0f. Previous setting retained."),*Name,Min,Max):
                            FString::Printf(TEXT("%s: enter a number from %.4g to %.4g. Previous setting retained."),*Name,Min,Max);
                        ReportError(Error);return;
                    }
                    if(Number==Read()){Synchronize();return;}
                    if(TryWrite){if(!TryWrite(Number))return;}else Write(Number);
                    Synchronize();
                })];
            Synchronize();
        }
        void Tick(const FGeometry&,double,float) override {if(Current()&&ObservedRevision!=Revision())Synchronize();}
        void OnFocusChanging(const FWeakWidgetPath& Previous,const FWidgetPath& Next,const FFocusEvent& Event) override
        {
            if(Current()&&Next.ContainsWidget(this)&&!Previous.ContainsWidget(this)&&ObservedRevision!=Revision())Synchronize();
            SCompoundWidget::OnFocusChanging(Previous,Next,Event);
        }
    private:
        void Synchronize()
        {
            ObservedRevision=Revision();bSynchronizing=true;
            Editor->SetText(FText::FromString(Digits==17?ExactViewNumber(Read()):FString::Printf(TEXT("%.*g"),Digits,Read())));
            bSynchronizing=false;ReportError(FString());
        }
        TSharedPtr<SProjectFilterBox> Editor;
        TFunction<double()> Read;TFunction<uint64()> Revision;TFunction<void(double)> Write;
        TFunction<bool(double)> TryWrite;FString Caption;
        TFunction<bool()> Current;TFunction<void(FString)> ReportError;
        double Min=0,Max=0;int32 Digits=8;uint64 ObservedRevision=0;bool bInteger=false,bSynchronizing=false;
    };
    // Numeric entries display rounded text. Focus loss alone must not write
    // that rounded value back into an exact camera position/orientation.
    class SCommittedNumber : public SNumericEntryBox<double>
    {
    public:
        void Construct(const FArguments& Args)
        {
            auto Config=Args;
            Config.OnValueChanged_Lambda([this](double){bEdited=true;});
            Config.OnValueCommitted_Lambda([this,Commit=Args._OnValueCommitted](double V,ETextCommit::Type How)
            {
                const bool bCommit=bEdited; bEdited=false;
                if(bCommit&&How!=ETextCommit::OnCleared)Commit.ExecuteIfBound(V,How);
            });
            SNumericEntryBox<double>::Construct(Config);
        }
        void OnFocusChanging(const FWeakWidgetPath& Previous,const FWidgetPath& Next,const FFocusEvent& Event) override
        {
            if(Next.ContainsWidget(this)&&!Previous.ContainsWidget(this))
                bEdited=false;
            SNumericEntryBox<double>::OnFocusChanging(Previous,Next,Event);
        }
    private:
        bool bEdited=false;
    };
    // A draft belongs to one camera revision. Explicit text ownership lets an
    // external restore replace a focused draft without committing rounded text.
    class SCameraDistanceField : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SCameraDistanceField){} SLATE_END_ARGS()
        void Construct(const FArguments&,TFunction<double()> InRead,TFunction<int32()> InRevision,TFunction<void(double)> InCommit)
        {
            Read=MoveTemp(InRead);Revision=MoveTemp(InRevision);Commit=MoveTemp(InCommit);SetCanTick(true);
            ChildSlot[SAssignNew(Editor,SEditableTextBox).Style(&InputStyle()).Font(Font(10))
                .ClearKeyboardFocusOnCommit(false).SelectAllTextWhenFocused(true).SelectAllTextOnCommit(true)
                .OnTextChanged_Lambda([this](const FText& Value)
                {
                    if(bSynchronizing)return;
                    if(!bEdited&&Value.ToString()==SynchronizedText)return;
                    if(!bEdited)EditRevision=ObservedRevision=Revision();
                    bEdited=true;
                })
                .OnTextCommitted_Lambda([this](const FText& Value,ETextCommit::Type How)
                {
                    const bool Apply=bEdited&&How!=ETextCommit::OnCleared&&EditRevision==Revision();
                    bEdited=false;
                    if(Apply)
                    {
                        double Number;
                        Commit(StudioColor::ParseNumber(Value.ToString(),Number)?Number:std::numeric_limits<double>::quiet_NaN());
                    }
                    Synchronize();
                })];
            Synchronize();
        }
        void Tick(const FGeometry&,double,float) override
        {if(ObservedRevision!=Revision())Synchronize();}
        bool SupportsKeyboardFocus() const override{return true;}
        FReply OnFocusReceived(const FGeometry&,const FFocusEvent& Event) override
        {return FReply::Handled().SetUserFocus(Editor.ToSharedRef(),Event.GetCause());}
    private:
        void Synchronize()
        {
            bEdited=false;EditRevision=ObservedRevision=Revision();
            FString Value=FString::Printf(TEXT("%.6f"),Read());
            while(Value.EndsWith(TEXT("0")))Value.LeftChopInline(1);
            if(Value.EndsWith(TEXT(".")))Value.LeftChopInline(1);
            SynchronizedText=Value;
            bSynchronizing=true;Editor->SetText(FText::FromString(Value));bSynchronizing=false;
        }
        TSharedPtr<SEditableTextBox> Editor;
        TFunction<double()> Read;
        TFunction<int32()> Revision;
        TFunction<void(double)> Commit;
        FString SynchronizedText;
        bool bEdited=false,bSynchronizing=false;
        int32 EditRevision=0,ObservedRevision=0;
    };
    // Names and int64 point IDs remain text throughout editing. A changed
    // object revision discards a stale draft before focus loss can apply it.
    class SInspectionTextField : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SInspectionTextField){} SLATE_END_ARGS()
        void Construct(const FArguments&,TFunction<FString()> InRead,TFunction<int32()> InRevision,TFunction<void(const FString&)> InCommit)
        {
            Read=MoveTemp(InRead);Revision=MoveTemp(InRevision);Commit=MoveTemp(InCommit);SetCanTick(true);
            ChildSlot[SAssignNew(Editor,SEditableTextBox).Style(&InputStyle()).Font(Font(10))
                .ClearKeyboardFocusOnCommit(false).SelectAllTextWhenFocused(true).SelectAllTextOnCommit(true)
                .OnTextChanged_Lambda([this](const FText& Value)
                {
                    if(bSynchronizing||(!bEdited&&Value.ToString()==SynchronizedText))return;
                    if(!bEdited)EditRevision=Revision();bEdited=true;
                })
                .OnTextCommitted_Lambda([this](const FText& Value,ETextCommit::Type How)
                {
                    const bool Apply=bEdited&&How!=ETextCommit::OnCleared&&EditRevision==Revision();bEdited=false;
                    if(Apply)Commit(Value.ToString());Synchronize();
                })];Synchronize();
        }
        void Tick(const FGeometry&,double,float) override {if(ObservedRevision!=Revision())Synchronize();}
        bool SupportsKeyboardFocus() const override {return true;}
        FReply OnFocusReceived(const FGeometry&,const FFocusEvent& Event) override
        {return FReply::Handled().SetUserFocus(Editor.ToSharedRef(),Event.GetCause());}
    private:
        void Synchronize()
        {
            bEdited=false;EditRevision=ObservedRevision=Revision();SynchronizedText=Read();
            bSynchronizing=true;Editor->SetText(FText::FromString(SynchronizedText));bSynchronizing=false;
        }
        TSharedPtr<SEditableTextBox> Editor;
        TFunction<FString()> Read;TFunction<int32()> Revision;TFunction<void(const FString&)> Commit;
        FString SynchronizedText;int32 EditRevision=0,ObservedRevision=0;bool bEdited=false,bSynchronizing=false;
    };
    TSharedRef<STextBlock> Label(const FString& Value,int32 Size,FLinearColor Color,bool Bold)
    { return SNew(STextBlock).Text(FText::FromString(Value)).Font(Font(Size,Bold)).ColorAndOpacity(Color); }
    TSharedRef<STextBlock> Live(TFunction<FString()> Value,int32 Size=10,FLinearColor Color=Text,bool Wrap=false)
    { return SNew(STextBlock).Text_Lambda([Value]{return FText::FromString(Value());}).Font(Font(Size)).ColorAndOpacity(Color).AutoWrapText(Wrap); }

    class SIcon : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SIcon):_Name(TEXT("box")),_Color(Text),_Size(16.f){}
            SLATE_ARGUMENT(FString,Name) SLATE_ATTRIBUTE(FLinearColor,Color) SLATE_ARGUMENT(float,Size)
        SLATE_END_ARGS()
        FString Name; TAttribute<FLinearColor> IconColor; float Size=16;
        void Construct(const FArguments& A) { Name=A._Name; IconColor=A._Color; Size=A._Size; }
        virtual FVector2D ComputeDesiredSize(float) const override {return FVector2D(Size,Size);}
        virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& O,int32 L,const FWidgetStyle&,bool) const override
        {
            const FLinearColor Color=IconColor.Get(Text);
            auto Path=[&](std::initializer_list<FVector2D> Points,float Width=1.15f)
            { TArray<FVector2D> P; for(auto V:Points) P.Add(V*G.GetLocalSize()); FSlateDrawElement::MakeLines(O,L,G.ToPaintGeometry(),P,ESlateDrawEffect::None,Color,true,Width); };
            if(Name==TEXT("run")) Path({{.25,.15},{.85,.5},{.25,.85},{.25,.15}});
            else if(Name==TEXT("help"))
            {
                TArray<FVector2D> Ring;
                for(int32 I=0;I<=24;++I){const double A=I*2.*PI/24.;Ring.Add((FVector2D(.5,.5)+FVector2D(FMath::Cos(A),FMath::Sin(A))*.43)*G.GetLocalSize());}
                FSlateDrawElement::MakeLines(O,L,G.ToPaintGeometry(),Ring,ESlateDrawEffect::None,Color,true,1.15f);
                Path({{.34,.35},{.35,.27},{.42,.22},{.55,.22},{.64,.29},{.65,.38},{.59,.45},{.51,.49},{.5,.59}});
                Path({{.5,.71},{.5,.73}},2.f);
            }
            else if(Name==TEXT("bell"))
            {
                Path({{.18,.75},{.28,.65},{.28,.36},{.32,.22},{.43,.15},{.57,.15},{.68,.22},{.72,.36},{.72,.65},{.82,.75},{.18,.75}});
                Path({{.42,.85},{.46,.9},{.54,.9},{.58,.85}});
                Path({{.5,.08},{.5,.15}});
            }
            else if(Name==TEXT("pause")) {Path({{.32,.2},{.32,.8}},2); Path({{.68,.2},{.68,.8}},2);}
            else if(Name==TEXT("stop")) Path({{.2,.2},{.8,.2},{.8,.8},{.2,.8},{.2,.2}});
            else if(Name==TEXT("step")) {Path({{.18,.2},{.68,.5},{.18,.8},{.18,.2}}); Path({{.8,.2},{.8,.8}});}
            else if(Name==TEXT("save")) {Path({{.15,.12},{.7,.12},{.87,.3},{.87,.88},{.15,.88},{.15,.12}});Path({{.3,.12},{.3,.42},{.65,.42},{.65,.12}});Path({{.32,.85},{.32,.6},{.7,.6},{.7,.85}});}
            else if(Name==TEXT("plus")) {Path({{.5,.16},{.5,.84}});Path({{.16,.5},{.84,.5}});}
            else if(Name==TEXT("export")) {Path({{.5,.1},{.5,.63},{.28,.42}});Path({{.5,.63},{.72,.42}});Path({{.15,.62},{.15,.88},{.85,.88},{.85,.62}});}
            else if(Name==TEXT("camera")) {Path({{.1,.3},{.3,.3},{.38,.16},{.65,.16},{.73,.3},{.9,.3},{.9,.86},{.1,.86},{.1,.3}});TArray<FVector2D>P;for(int I=0;I<=20;++I)P.Add((FVector2D(.5,.57)+FVector2D(FMath::Cos(I*PI/10),FMath::Sin(I*PI/10))*.19)*G.GetLocalSize());FSlateDrawElement::MakeLines(O,L,G.ToPaintGeometry(),P,ESlateDrawEffect::None,Color,true,1.1);}
            else if(Name==TEXT("fit")) {Path({{.35,.12},{.12,.12},{.12,.35}});Path({{.65,.12},{.88,.12},{.88,.35}});Path({{.12,.65},{.12,.88},{.35,.88}});Path({{.65,.88},{.88,.88},{.88,.65}});}
            else if(Name==TEXT("chart")) {Path({{.1,.1},{.1,.9},{.9,.9}});Path({{.2,.65},{.4,.4},{.55,.58},{.8,.2}});}
            else if(Name==TEXT("pan")) {Path({{.5,.1},{.5,.9}});Path({{.1,.5},{.9,.5}});Path({{.3,.3},{.5,.1},{.7,.3}});Path({{.7,.7},{.5,.9},{.3,.7}});}
            else if(Name==TEXT("zoom")) {TArray<FVector2D>P;for(int I=0;I<=20;++I)P.Add((FVector2D(.39,.39)+FVector2D(FMath::Cos(I*PI/10),FMath::Sin(I*PI/10))*.26)*G.GetLocalSize());FSlateDrawElement::MakeLines(O,L,G.ToPaintGeometry(),P,ESlateDrawEffect::None,Color,true,1.15);Path({{.58,.58},{.88,.88}});Path({{.25,.39},{.53,.39}});Path({{.39,.25},{.39,.53}});}
            else if(Name==TEXT("orbit")) {TArray<FVector2D>P;for(int I=0;I<=25;++I)P.Add((FVector2D(.5,.5)+FVector2D(FMath::Cos(I*.23)*.38,FMath::Sin(I*.23)*.25))*G.GetLocalSize());FSlateDrawElement::MakeLines(O,L,G.ToPaintGeometry(),P,ESlateDrawEffect::None,Color,true,1.2);Path({{.75,.15},{.87,.38},{.65,.35}});}
            else if(Name==TEXT("fly")) {Path({{.08,.55},{.88,.1},{.57,.9},{.44,.58},{.08,.55}});Path({{.44,.58},{.88,.1}});}
            else if(Name==TEXT("settings")) {Path({{.1,.25},{.9,.25}});Path({{.1,.5},{.9,.5}});Path({{.1,.75},{.9,.75}});Path({{.3,.12},{.3,.37}});Path({{.7,.38},{.7,.63}});Path({{.4,.63},{.4,.88}});}
            else if(Name==TEXT("select")) Path({{.2,.1},{.8,.65},{.52,.63},{.67,.89},{.51,.97},{.35,.68},{.2,.9},{.2,.1}});
            else if(Name==TEXT("check")) Path({{.14,.5},{.4,.77},{.88,.2}});
            else if(Name==TEXT("star")) Path({{.5,.08},{.62,.35},{.92,.38},{.7,.58},{.76,.9},{.5,.74},{.24,.9},{.3,.58},{.08,.38},{.38,.35},{.5,.08}});
            else if(Name==TEXT("folder")) Path({{.08,.23},{.38,.23},{.48,.36},{.92,.36},{.92,.85},{.08,.85},{.08,.23}});
            else if(Name==TEXT("undo")) {Path({{.4,.16},{.12,.4},{.4,.64}});Path({{.12,.4},{.64,.4},{.84,.58},{.84,.82}});}
            else if(Name==TEXT("redo")) {Path({{.6,.16},{.88,.4},{.6,.64}});Path({{.88,.4},{.36,.4},{.16,.58},{.16,.82}});}
            else if(Name==TEXT("collapse")) {Path({{.58,.2},{.28,.5},{.58,.8}});Path({{.86,.2},{.56,.5},{.86,.8}});}
            else if(Name==TEXT("expand")) {Path({{.14,.2},{.44,.5},{.14,.8}});Path({{.42,.2},{.72,.5},{.42,.8}});}
            else {Path({{.5,.1},{.9,.3},{.9,.75},{.5,.94},{.1,.75},{.1,.3},{.5,.1},{.5,.55},{.9,.3}});Path({{.1,.3},{.5,.55},{.5,.94}});}
            return L;
        }
    };
    TSharedRef<SWidget> Icon(const FString& Name,FLinearColor Color=Text,float Size=16) {return SNew(SIcon).Name(Name).Color(Color).Size(Size);}
    TSharedRef<SButton> Button(const FString& Value,const FString& Name,TFunction<void()> Action,FLinearColor Tint=Text)
    {
        return SNew(SButton).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(10,7)).OnClicked_Lambda([Action]{Action();return FReply::Handled();})
            [SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Icon(Name,Tint)]
            +SHorizontalBox::Slot().AutoWidth().Padding(7,0,0,0).VAlign(VAlign_Center)[Label(Value,10,Tint)]];
    }
    TSharedRef<SWidget> Section(const FString& Title,TSharedRef<SWidget> Body)
    {
        return SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,13,0,9)[Label(Title,10,Text,true)]
        +SVerticalBox::Slot().AutoHeight()[Body]
        +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0)[SNew(SBox).HeightOverride(1)[SNew(SBorder).BorderImage(&LineBrush)]];
    }
    TSharedRef<SWidget> Row(const FString& Name,TSharedRef<SWidget> Control,float Left=140)
    { return SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0,4,10,4)[Label(Name,10,Muted)]
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(Left)[Control]]; }
    TSharedRef<SWidget> Number(TFunction<double()> Read,TFunction<void(double)> Write,double Min,double Max,const FString& Unit=TEXT(""),double Delta=0.1,int32 Digits=INDEX_NONE)
    {
        return SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)
        [SNew(SCommittedNumber).Font(Font(10)).EditableTextBoxStyle(&InputStyle()).AllowSpin(false)
        .MinFractionalDigits(0).MaxFractionalDigits(Digits==INDEX_NONE?(Unit==TEXT("m²/s")?6:3):Digits)
        .MinValue(Min).MaxValue(Max).MinSliderValue(Min).MaxSliderValue(Max).Delta(Delta)
        .Value_Lambda([Read]{return TOptional<double>(Read());}).OnValueCommitted_Lambda([Write](double V,ETextCommit::Type){Write(V);})]
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5,0,0,0)[Label(Unit,9,Muted)];
    }
    TSharedRef<SWidget> Check(const FString& Name,TFunction<bool()> Read,TFunction<void(bool)> Write)
    { return SNew(SCheckBox).IsChecked_Lambda([Read]{return Read()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([Write](ECheckBoxState S){Write(S==ECheckBoxState::Checked);})[Label(Name,10)]; }
    TSharedRef<SWidget> Slider(TFunction<double()> Read,TFunction<void(double)> Write)
    { return SNew(SSlider).Value_Lambda([Read]{return static_cast<float>(Read());}).OnValueChanged_Lambda([Write](float V){Write(V);}).SliderBarColor(Line).SliderHandleColor(Blue); }
    TSharedRef<SWidget> ViewCheck(const TSharedPtr<FStudioModel>& M,const FString& Name,TFunction<bool()> Read,TFunction<void(FStudioViewSettings&,bool)> Write)
    { return Check(Name,Read,[M,Name,Write](bool V){M->EditView(Name,[Write,V](auto& S){Write(S.Display,V);});}); }
    TSharedRef<SWidget> ViewSlider(const TSharedPtr<FStudioModel>& M,const FString& Name,TFunction<double()> Read,TFunction<void(FStudioViewSettings&,double)> Write)
    {
        return SNew(SSlider).Value_Lambda([Read]{return static_cast<float>(Read());})
            .OnMouseCaptureBegin_Lambda([M,Name]{M->BeginViewEdit(Name);}).OnMouseCaptureEnd_Lambda([M]{M->EndViewEdit();})
            .OnControllerCaptureBegin_Lambda([M,Name]{M->BeginViewEdit(Name);}).OnControllerCaptureEnd_Lambda([M]{M->EndViewEdit();})
            .OnValueChanged_Lambda([M,Name,Write](float V){M->EditView(Name,[Write,V](auto& S){Write(S.Display,V);});})
            .SliderBarColor(Line).SliderHandleColor(Blue).ToolTipText(FText::FromString(Name));
    }
}
using namespace StudioUI;

DECLARE_DELEGATE_RetVal_TwoParams(bool,FStudioInspectionClick,const FGeometry&,const FPointerEvent&);
DECLARE_DELEGATE_RetVal_TwoParams(bool,FStudioInspectionHover,const FGeometry&,const TOptional<FVector2D>&);

static int32 PaintInspection(const AStudioScene& Scene,const FGeometry& G,FSlateWindowElementList& Out,int32 Layer,const FStudioProbeMarkerResult* Markers,const FStudioInspectionPlacement* Draft,TArray<FSlateRect> Reserved)
{
    const auto& M=*Scene.Model;
    if(!Scene.HasPresentedFrame()||Scene.PresentedProjectId()!=M.Project.Id||M.CameraPlacement())return Layer;
    const auto Field=Scene.PresentedField();if(!Field)return Layer;
    const auto Identity=Field->Identity();if(!Identity.IsSet())return Layer;
    const FStudioInspectionSource Source{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256};
    const auto Camera=Scene.PresentedCamera();const auto Size=G.GetLocalSize();
    const auto Texture=Scene.PresentedViewportSize();const double Aspect=Texture.Y>0?double(Texture.X)/Texture.Y:0;
    auto DrawLine=[&](FVector A,FVector B,FLinearColor Color,float Width)
    {
        FVector2D P,Q;if(StudioCameraPlacement::ProjectLine(Camera,Size,A,B,P,Q,Aspect))
            FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),TArray<FVector2D>{P,Q},ESlateDrawEffect::None,Color,true,Width);
    };
    auto Marker=[&](FVector At,const FString& Name,FLinearColor Color,bool Outline=false)
    {
        FVector2D P;if(!StudioInspectionOverlay::ProjectMarker(Camera,Size,At,P,Aspect))return;
        if(Outline)
        {
            FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),TArray<FVector2D>{P+FVector2D(-5,0),P+FVector2D(5,0)},ESlateDrawEffect::None,BG,true,4);
            FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),TArray<FVector2D>{P+FVector2D(0,-5),P+FVector2D(0,5)},ESlateDrawEffect::None,BG,true,4);
        }
        FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),TArray<FVector2D>{P+FVector2D(-5,0),P+FVector2D(5,0)},ESlateDrawEffect::None,Color,true,2);
        FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),TArray<FVector2D>{P+FVector2D(0,-5),P+FVector2D(0,5)},ESlateDrawEffect::None,Color,true,2);
        if(Name.IsEmpty())return;
        const FVector2D Extent=FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Name,Font(9));
        // Use the actual sibling overlay geometry: the legend, Views and
        // Display panels change size with the window and selected source.
        const FVector2D BoxSize=Extent+FVector2D(8,4);
        const FVector2D Candidates[]={P+FVector2D(9,-10),P+FVector2D(9,8),P-FVector2D(BoxSize.X+9,10),
            P+FVector2D(-BoxSize.X-9,8),P-FVector2D(BoxSize.X*.5,BoxSize.Y+9),P+FVector2D(-BoxSize.X*.5,9)};
        TOptional<FVector2D> LabelPosition;
        for(const auto& Candidate:Candidates)
        {
            const FVector2D LabelAt(FMath::Clamp(Candidate.X,4.,FMath::Max(4.,Size.X-BoxSize.X-4)),
                FMath::Clamp(Candidate.Y,4.,FMath::Max(4.,Size.Y-BoxSize.Y-4)));
            const FSlateRect Rect(LabelAt.X-2,LabelAt.Y-2,LabelAt.X+BoxSize.X+2,LabelAt.Y+BoxSize.Y+2);
            if(!Reserved.ContainsByPredicate([&](const auto& R){return Rect.Left<R.Right&&Rect.Right>R.Left&&Rect.Top<R.Bottom&&Rect.Bottom>R.Top;}))
            {LabelPosition=LabelAt;Reserved.Add(Rect);break;}
        }
        if(!LabelPosition.IsSet())return; // The inspector still shows the full name/value.
        const FVector2D Position=LabelPosition.GetValue();
        FSlateDrawElement::MakeBox(Out,Layer+2,G.ToPaintGeometry(Extent+FVector2D(8,4),FSlateLayoutTransform(Position)),&PanelBrush,ESlateDrawEffect::None,Panel);
        FSlateDrawElement::MakeText(Out,Layer+3,G.ToPaintGeometry(Extent,FSlateLayoutTransform(Position+FVector2D(4,2))),Name,Font(9),ESlateDrawEffect::None,Color);
    };
    auto Selected=[&](FGuid Id){return Id==M.SelectedInspectionObject&&(!Draft||Draft->ObjectId!=Id);};
    auto Color=[&](FGuid Id){return Selected(Id)?Cyan:Muted;};
    const auto Overlay=StudioInspectionOverlay::Build(M.InspectionObjects,Source,M.Project.Id,
        Draft?FGuid():M.SelectedInspectionObject,Scene.GetRenderedFlowBounds(),Markers,Identity->SpatialDimensions,Identity->SourceOffset.Y,StudioStreamlines::DomainBounds(*Field,Scene.GetRenderedFlowBounds()));
    for(const auto& Edge:Overlay.Lines)DrawLine(Edge.A,Edge.B,Color(Edge.Object),Edge.Width);
    for(const auto& Point:Overlay.Markers)Marker(Point.Position,Point.Label,Color(Point.Object));
    if(Draft&&Draft->Source==Source)
    {
        auto DraftLine=[&](FVector A,FVector B){DrawLine(A,B,BG,4);DrawLine(A,B,Amber,2);};
        const auto& Points=Draft->Accepted;
        for(int32 I=0;I<Points.Num();++I)
        {
            if(I>0)DraftLine(Points[I-1],Points[I]);
            Marker(Points[I],FString::Chr(TEXT("ABC")[I]),Amber,true);
        }
        if(Draft->Preview.IsSet())
        {
            if(!Points.IsEmpty())DraftLine(Points.Last(),*Draft->Preview);
            Marker(*Draft->Preview,FString::Chr(TEXT("ABC")[Points.Num()])+TEXT(" · preview"),Text,true);
        }
    }
    return Layer+3;
}

// Scene captures store auxiliary data in alpha. The viewport displays their RGB.
class SFlowImage : public SImage
{
public:
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& O,int32 L,const FWidgetStyle&,bool) const override
    {
        FSlateDrawElement::MakeBox(O,L,G.ToPaintGeometry(),GetImageAttribute().Get(),ESlateDrawEffect::IgnoreTextureAlpha,FLinearColor::White);
        return L;
    }
};

class SFlowViewport : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SFlowViewport){} SLATE_ARGUMENT(AStudioScene*,Scene)
        SLATE_EVENT(FStudioInspectionClick,InspectionClick)
        SLATE_EVENT(FStudioInspectionHover,InspectionHover)
        SLATE_EVENT(FSimpleDelegate,CancelInspection)
        SLATE_ATTRIBUTE(const FStudioProbeMarkerResult*,InspectionMarkers)
        SLATE_ATTRIBUTE(const FStudioInspectionPlacement*,InspectionPlacement)
        SLATE_ATTRIBUTE(uint64,InspectionPlacementRevision)
        SLATE_ATTRIBUTE(bool,InspectionActive)
        SLATE_ATTRIBUTE(FIntPoint,SnapshotSize)
        SLATE_ATTRIBUTE(EStudioViewportTool,NavigationTool)
    SLATE_END_ARGS()
    FStudioInspectionClick InspectionClick;
    FStudioInspectionHover InspectionHover;
    FSimpleDelegate CancelInspection;
    TAttribute<const FStudioProbeMarkerResult*> InspectionMarkers;
    TAttribute<const FStudioInspectionPlacement*> InspectionPlacement;
    TAttribute<uint64> InspectionPlacementRevision;
    TAttribute<bool> InspectionActive;
    TAttribute<FIntPoint> SnapshotSize;
    TAttribute<EStudioViewportTool> NavigationTool;
    EStudioViewportTool GestureTool=EStudioViewportTool::Orbit;
    FIntPoint PaintedSnapshotSize=FIntPoint::ZeroValue;
    uint64 PaintedInspectionPlacement=0;
    TOptional<FVector2D> LastPointerScreen;
    TWeakObjectPtr<AStudioScene> Scene; FSlateBrush Brush; FKey Drag; TSet<FKey> Held;
    bool bOwnEdit=false,bFlying=false;
    double WheelSeconds=0;
    FString GestureLabel;
    TOptional<StudioCameraPlacement::FDrag> PlacementDrag;
    int32 PlacementDragRevision=0;
    int32 HoverAxis=INDEX_NONE;
    int32 PaintedPlacementRevision=-1,PaintedCollectionRevision=-1;
    uint64 PaintedCapture=MAX_uint64;
    int32 InspectionPaintRevision=-1,InspectionSelectionRevision=-1;
    uint64 InspectionCapture=MAX_uint64;
    uint64 PaintedMarkerSerial=0;
    uint32 CapturedUser=0,CapturedPointer=0;
    void BeginGesture(const FString& Label)
    {
        if(!Scene.IsValid()) return;
        if(bOwnEdit && GestureLabel!=Label) EndGesture();
        if(!Scene->Model->IsViewEditActive()) Scene->Model->BeginViewEdit(Label);
        bOwnEdit=true; GestureLabel=Label;
    }
    void EndGesture()
    { if(bOwnEdit&&Scene.IsValid()) Scene->Model->EndViewEdit(); bOwnEdit=false; WheelSeconds=0; }
    void FinishInput() { EndGesture(); Held.Reset(); Drag=FKey(); bFlying=false; PlacementDrag.Reset(); Invalidate(EInvalidateWidgetReason::Paint); }
    bool OwnsPointer(const FPointerEvent& E) const
    {return E.GetUserIndex()==CapturedUser&&E.GetPointerIndex()==CapturedPointer;}
    bool HasOwnCapture()
    {
        const auto User=FSlateApplication::Get().GetUser(CapturedUser);
        return User&&User->DoesWidgetHaveCapture(SharedThis(this),CapturedPointer);
    }
    void ReleaseOwnCapture()
    {
        // A stale placement or an external focus change must release only the
        // pointer owned by this viewport, not another window/user's capture.
        const auto User=FSlateApplication::Get().GetUser(CapturedUser);
        FinishInput();
        if(User&&User->DoesWidgetHaveCapture(SharedThis(this),CapturedPointer))User->ReleaseCapture(CapturedPointer);
    }
    ~SFlowViewport() { EndGesture(); }
    void Construct(const FArguments& A)
    {
        Scene=A._Scene;InspectionClick=A._InspectionClick;InspectionHover=A._InspectionHover;CancelInspection=A._CancelInspection;
        InspectionMarkers=A._InspectionMarkers;
        InspectionPlacement=A._InspectionPlacement;InspectionPlacementRevision=A._InspectionPlacementRevision;
        InspectionActive=A._InspectionActive;
        SnapshotSize=A._SnapshotSize;NavigationTool=A._NavigationTool;
        Brush.SetResourceObject(Scene->GetRenderTarget()); Brush.DrawAs=ESlateBrushDrawType::Image;
        Brush.ImageSize=FVector2D(1280,720); SetCanTick(true);
        ChildSlot[SNew(SFlowImage).Image(&Brush)];
    }
    virtual bool SupportsKeyboardFocus() const override {return true;}
    double PlacementProjectionAspect() const
    {
        const auto Size=Scene->PresentedViewportSize();
        return Size.Y>0?double(Size.X)/Size.Y:0;
    }
    bool CanPlace() const
    {
        if(!Scene.IsValid()||Scene->Model->Workspace!=EStudioWorkspace::Solve||!Scene->Model->IsCameraPlacementCurrent()||!Scene->HasPresentedFrame())return false;
        auto Expected=Scene->Model->Project.Camera;Expected.FieldOfView=float(Expected.FieldOfView);
        Expected.OrthoWidth=double(float(Expected.OrthoWidth*100.))/100.;
        return StudioView::CameraEquals(Scene->PresentedCamera(),Expected);
    }
    bool IsPlacementDragCurrent(const FGeometry& G) const
    {
        return PlacementDrag.IsSet()&&CanPlace()&&Scene->Model->CameraPlacementRevision==PlacementDragRevision&&
            G.GetLocalSize().Equals(PlacementDrag->Viewport)&&PlacementProjectionAspect()==PlacementDrag->ProjectionAspect&&
            StudioView::CameraEquals(Scene->PresentedCamera(),PlacementDrag->Observer);
    }
    virtual int32 OnPaint(const FPaintArgs& Args,const FGeometry& G,const FSlateRect& Cull,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle& Style,bool Enabled) const override
    {
        Layer=SCompoundWidget::OnPaint(Args,G,Cull,Out,Layer,Style,Enabled);
        if(Scene.IsValid())
        {
            TArray<FSlateRect> Reserved;
            if(const auto Parent=GetParentWidget())
            {
                TFunction<void(TSharedRef<SWidget>,bool)> Reserve;
                Reserve=[&](TSharedRef<SWidget> W,bool InFloatingLayer)
                {
                    if(&W.Get()==this||!W->GetVisibility().IsVisible())return;
                    if(W->GetTag()==TEXT("FloatingViewportPanes")||(InFloatingLayer&&!W->GetTag().ToString().StartsWith(TEXT("FloatingPane_"))))
                    {
                        auto* Children=W->GetChildren();
                        for(int32 I=0;I<Children->Num();++I)Reserve(Children->GetChildAt(I),true);
                        return;
                    }
                    // Reserve actual pane rectangles, never the full transparent layer.
                    const auto& Geometry=W->GetPaintSpaceGeometry();if(Geometry.GetLocalSize().IsNearlyZero())return;
                    const auto A=G.AbsoluteToLocal(Geometry.LocalToAbsolute(FVector2D::ZeroVector));
                    const auto B=G.AbsoluteToLocal(Geometry.LocalToAbsolute(Geometry.GetLocalSize()));
                    Reserved.Emplace(A.X,A.Y,B.X,B.Y);
                };
                auto* Children=Parent->GetChildren();
                for(int32 I=0;I<Children->Num();++I)Reserve(Children->GetChildAt(I),false);
            }
            Layer=PaintInspection(*Scene,G,Out,Layer,InspectionMarkers.Get(nullptr),InspectionPlacement.Get(nullptr),MoveTemp(Reserved));
            const auto Output=SnapshotSize.Get(FIntPoint::ZeroValue);
            if(StudioSnapshot::ValidSize(Output)&&Scene->HasPresentedFrame())
            {
                const auto Frame=StudioSnapshot::Frame(Scene->PresentedViewportSize(),Output);const auto Size=G.GetLocalSize();
                const FVector2D A=Frame.Minimum*Size,B=(Frame.Minimum+Frame.Span)*Size;
                auto Shade=[&](FVector2D At,FVector2D Extent)
                {if(Extent.X>0&&Extent.Y>0)FSlateDrawElement::MakeBox(Out,Layer+1,G.ToPaintGeometry(Extent,FSlateLayoutTransform(At)),&White,ESlateDrawEffect::None,FLinearColor(0,0,0,.45));};
                Shade(FVector2D::ZeroVector,FVector2D(Size.X,A.Y));Shade(FVector2D(0,B.Y),FVector2D(Size.X,Size.Y-B.Y));
                Shade(FVector2D(0,A.Y),FVector2D(A.X,B.Y-A.Y));Shade(FVector2D(B.X,A.Y),FVector2D(Size.X-B.X,B.Y-A.Y));
                FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),TArray<FVector2D>{A,{B.X,A.Y},B,{A.X,B.Y},A},ESlateDrawEffect::None,Cyan,true,1.5);
                Layer+=2;
            }
        }
        if(!CanPlace())return Layer;
        const auto& Draft=*Scene->Model->CameraPlacement();const auto& Observer=Scene->PresentedCamera();const auto Size=G.GetLocalSize();
        const double Aspect=PlacementProjectionAspect();
        const FLinearColor AxisColors[]={FLinearColor(1,.3,.2),Green,FLinearColor(.25,.55,1)};
        auto Draw=[&](const StudioCameraPlacement::FLine& L,FLinearColor Color,float Width)
        {
            FVector2D A,B;if(StudioCameraPlacement::ProjectLine(Observer,Size,L.A,L.B,A,B,Aspect))
                FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),TArray<FVector2D>{A,B},ESlateDrawEffect::None,Color,true,Width);
        };
        for(const auto& Saved:Scene->Model->Project.Cameras)if(Saved.Id!=Draft.CameraId)
            for(const auto& L:StudioCameraPlacement::Frustum(Saved.Camera,Aspect))Draw(L,Muted*.6f,1.f);
        for(const auto& L:StudioCameraPlacement::Frustum(Draft.Camera,Aspect))Draw(L,Cyan,1.f);
        for(const auto& L:StudioCameraPlacement::Handles(Draft.Camera,Observer,Size,Draft.Tool))
            Draw(L,PlacementDrag.IsSet()&&PlacementDrag->Axis==L.Axis?Text:AxisColors[L.Axis],HoverAxis==L.Axis?3.f:2.f);
        const double Scale=StudioCameraPlacement::HandleScale(Observer,Size,Draft.Camera.Position);
        for(int32 I=0;I<3;++I)
        {
            FVector Axis=FVector::ZeroVector;Axis[I]=1;FVector2D P;
            const FVector LabelPoint=Draft.Camera.Position+Axis*Scale*1.12;
            if(Draft.Tool==EStudioCameraPlacementTool::Move&&StudioCameraPlacement::Project(Observer,Size,LabelPoint,P,Aspect)&&
                P.X>4&&P.Y>4&&P.X<Size.X-18&&P.Y<Size.Y-18)
                FSlateDrawElement::MakeText(Out,Layer+2,G.ToPaintGeometry(FVector2D(16,16),FSlateLayoutTransform(P)),
                    FString::Chr(TEXT("XYZ")[I]),Font(10,true),ESlateDrawEffect::None,AxisColors[I]);
        }
        return Layer+2;
    }
    virtual void Tick(const FGeometry& G,double T,float D) override
    {
        SCompoundWidget::Tick(G,T,D); if(!Scene.IsValid()) return;
        const auto Preview=SnapshotSize.Get(FIntPoint::ZeroValue);
        if(Preview!=PaintedSnapshotSize){PaintedSnapshotSize=Preview;Invalidate(EInvalidateWidgetReason::Paint);}
        if(InspectionHover.IsBound())InspectionHover.Execute(G,Drag.IsValid()?TOptional<FVector2D>():LastPointerScreen);
        const uint64 DraftRevision=InspectionPlacementRevision.Get(0);
        if(PaintedInspectionPlacement!=DraftRevision){PaintedInspectionPlacement=DraftRevision;Invalidate(EInvalidateWidgetReason::Paint);}
        const auto* Markers=InspectionMarkers.Get(nullptr);const uint64 MarkerSerial=Markers?Markers->Serial:0;
        if(PaintedMarkerSerial!=MarkerSerial){PaintedMarkerSerial=MarkerSerial;Invalidate(EInvalidateWidgetReason::Paint);}
        if(InspectionPaintRevision!=Scene->Model->InspectionObjectsRevision||InspectionSelectionRevision!=Scene->Model->InspectionSelectionRevision||InspectionCapture!=Scene->GetCaptureCount())
        {InspectionPaintRevision=Scene->Model->InspectionObjectsRevision;InspectionSelectionRevision=Scene->Model->InspectionSelectionRevision;InspectionCapture=Scene->GetCaptureCount();Invalidate(EInvalidateWidgetReason::Paint);}
        if(PaintedPlacementRevision!=Scene->Model->CameraPlacementRevision||PaintedCollectionRevision!=Scene->Model->CameraCollectionRevision||
            (Scene->Model->CameraPlacement()&&PaintedCapture!=Scene->GetCaptureCount()))
        {
            PaintedPlacementRevision=Scene->Model->CameraPlacementRevision;PaintedCollectionRevision=Scene->Model->CameraCollectionRevision;
            PaintedCapture=Scene->GetCaptureCount();Invalidate(EInvalidateWidgetReason::Paint);
        }
        if((PlacementDrag.IsSet()&&!IsPlacementDragCurrent(G))||(HasOwnCapture()&&!Drag.IsValid()))ReleaseOwnCapture();
        // Two samples per Slate unit keep the original mesh and thin traces smooth.
        // Scene caps both dimensions; idle views still submit no captures.
        Scene->ResizeViewport(FMath::RoundToInt(G.GetLocalSize().X*2),FMath::RoundToInt(G.GetLocalSize().Y*2));
        if(WheelSeconds>0) { WheelSeconds-=D; if(WheelSeconds<=0) EndGesture(); }
        if(!PlacementDrag.IsSet()&&(Drag==EKeys::RightMouseButton||Scene->bFreeCamera)&&HasKeyboardFocus())
        {
            FVector V(Held.Contains(EKeys::W)-Held.Contains(EKeys::S),Held.Contains(EKeys::D)-Held.Contains(EKeys::A),Held.Contains(EKeys::E)-Held.Contains(EKeys::Q));
            if(!V.IsNearlyZero())
            {
                BeginGesture(Drag.IsValid()?GestureLabel:TEXT("Fly camera")); bFlying=true;
                Scene->Fly(V.GetSafeNormal(),D*(Held.Contains(EKeys::LeftShift)?3.:1.));
            }
            else if(bFlying&&!Drag.IsValid()) { EndGesture(); bFlying=false; }
        }
    }
    virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E) override
    {
        const FKey Key=E.GetEffectingButton();
        if(!Scene.IsValid()||(Key!=EKeys::LeftMouseButton&&Key!=EKeys::MiddleMouseButton&&Key!=EKeys::RightMouseButton)) return FReply::Unhandled();
        if(HasOwnCapture()&&!OwnsPointer(E))return FReply::Unhandled();
        if(Key==EKeys::LeftMouseButton&&InspectionActive.Get(false)&&!HasKeyboardFocus())
        {
            const int32 Revision=Scene->Model->InspectionObjectsRevision;
            const FGuid Project=Scene->Model->Project.Id;
            const int32 SelectionRevision=Scene->Model->InspectionSelectionRevision;
            const bool HadPlacement=InspectionPlacement.Get(nullptr)!=nullptr;
            FSlateApplication::Get().SetUserFocus(E.GetUserIndex(),SharedThis(this),EFocusCause::Mouse);
            if(Project!=Scene->Model->Project.Id||Revision!=Scene->Model->InspectionObjectsRevision||
                SelectionRevision!=Scene->Model->InspectionSelectionRevision||(HadPlacement&&!InspectionPlacement.Get(nullptr)))
            {
                ReleaseOwnCapture();
                Scene->Model->InspectionNotice=TEXT("Inspection values applied. Click the updated object to continue.");
                return FReply::Handled();
            }
        }
        const auto Tool=NavigationTool.Get(Scene->bFreeCamera?EStudioViewportTool::Fly:EStudioViewportTool::Orbit);
        const bool NavigateOnly=Tool==EStudioViewportTool::Pan||Tool==EStudioViewportTool::Zoom;
        if(Key==EKeys::LeftMouseButton&&(!NavigateOnly||InspectionPlacement.Get(nullptr))&&!Scene->Model->CameraPlacement()&&InspectionClick.IsBound()&&InspectionClick.Execute(G,E))
        {ReleaseOwnCapture();Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled().SetUserFocus(SharedThis(this));}
        if(Scene->Model->CameraPlacement()&&!HasKeyboardFocus())
        {
            const int32 Before=Scene->Model->CameraPlacementRevision;
            FSlateApplication::Get().SetUserFocus(E.GetUserIndex(),SharedThis(this),EFocusCause::Mouse);
            // Losing numeric focus may apply a draft edit. The visible handle
            // will move on the next paint; this press must not orbit the view
            // or start a drag using the old displayed position.
            if(Scene->Model->CameraPlacementRevision!=Before)
            {
                ReleaseOwnCapture();
                if(Scene->Model->IsCameraPlacementCurrent())Scene->Model->CameraPlacementNotice=TEXT("Camera values applied. Drag the updated handle to continue.");
                return FReply::Handled();
            }
        }
        ReleaseOwnCapture(); Drag=Key;
        CapturedUser=E.GetUserIndex();CapturedPointer=E.GetPointerIndex();
        if(Key==EKeys::LeftMouseButton&&CanPlace())
        {
            const auto& Draft=*Scene->Model->CameraPlacement();const auto Size=G.GetLocalSize();const auto P=G.AbsoluteToLocal(E.GetScreenSpacePosition());
            const double Aspect=PlacementProjectionAspect();
            const int32 Axis=StudioCameraPlacement::HitHandle(StudioCameraPlacement::Handles(Draft.Camera,Scene->PresentedCamera(),Size,Draft.Tool),Scene->PresentedCamera(),Size,P,Aspect);
            if(Axis!=INDEX_NONE)
            {
                StudioCameraPlacement::FDrag Start;
                if(StudioCameraPlacement::BeginDrag(Draft.Camera,Scene->PresentedCamera(),Size,P,Draft.Tool,Axis,Start,Aspect))
                {PlacementDrag=Start;PlacementDragRevision=Scene->Model->CameraPlacementRevision;}
                else {Drag=FKey();Scene->Model->CameraPlacementNotice=TEXT("This axis is edge-on. Orbit the viewing camera, or use the numeric controls.");return FReply::Handled().SetUserFocus(SharedThis(this));}
                return FReply::Handled().SetUserFocus(SharedThis(this)).CaptureMouse(SharedThis(this));
            }
        }
        GestureTool=Key==EKeys::MiddleMouseButton?EStudioViewportTool::Pan:Key==EKeys::RightMouseButton?EStudioViewportTool::Fly:
            NavigationTool.Get(Scene->bFreeCamera?EStudioViewportTool::Fly:EStudioViewportTool::Orbit);
        BeginGesture(GestureTool==EStudioViewportTool::Pan?TEXT("Pan camera"):GestureTool==EStudioViewportTool::Zoom?TEXT("Zoom camera"):
            GestureTool==EStudioViewportTool::Fly?TEXT("Fly camera"):TEXT("Orbit camera"));
        return FReply::Handled().SetUserFocus(SharedThis(this)).CaptureMouse(SharedThis(this));
    }
    virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& E) override
    { if(!OwnsPointer(E)||E.GetEffectingButton()!=Drag) return FReply::Unhandled(); FinishInput(); return FReply::Handled().ReleaseMouseCapture(); }
    virtual void OnMouseCaptureLost(const FCaptureLostEvent& E) override
    {if(E.UserIndex==int32(CapturedUser)&&E.PointerIndex==int32(CapturedPointer))FinishInput();}
    virtual void OnFocusLost(const FFocusEvent& E) override
    {if(E.GetUser()==CapturedUser)ReleaseOwnCapture();SCompoundWidget::OnFocusLost(E);}
    virtual FReply OnMouseMove(const FGeometry& G,const FPointerEvent& E) override
    {
        if(!Scene.IsValid())return FReply::Unhandled();
        if(HasOwnCapture()&&!OwnsPointer(E))return FReply::Unhandled();
        LastPointerScreen=E.GetScreenSpacePosition();
        if(!Drag.IsValid()&&InspectionHover.IsBound()&&InspectionHover.Execute(G,LastPointerScreen))
        {Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();}
        const int32 PreviousHover=HoverAxis;
        if(CanPlace())
        {
            const auto& Draft=*Scene->Model->CameraPlacement();
            HoverAxis=StudioCameraPlacement::HitHandle(StudioCameraPlacement::Handles(Draft.Camera,Scene->PresentedCamera(),G.GetLocalSize(),Draft.Tool),
                Scene->PresentedCamera(),G.GetLocalSize(),G.AbsoluteToLocal(E.GetScreenSpacePosition()),PlacementProjectionAspect());
        }
        else HoverAxis=INDEX_NONE;
        if(PreviousHover!=HoverAxis)Invalidate(EInvalidateWidgetReason::Paint);
        if(!HasOwnCapture()||!OwnsPointer(E))return FReply::Unhandled();
        if(PlacementDrag.IsSet())
        {
            if(!IsPlacementDragCurrent(G)){FinishInput();return FReply::Handled().ReleaseMouseCapture();}
            FStudioCameraState Camera;
            if(StudioCameraPlacement::Drag(*PlacementDrag,G.AbsoluteToLocal(E.GetScreenSpacePosition()),Camera)&&Scene->Model->EditCameraPlacement(Camera))
                PlacementDragRevision=Scene->Model->CameraPlacementRevision;
            return FReply::Handled();
        }
        if(!Drag.IsValid()){FinishInput();return FReply::Handled().ReleaseMouseCapture();}
        const auto D=E.GetCursorDelta();
        if(D.IsNearlyZero()) return FReply::Handled();
        BeginGesture(GestureLabel);
        if(GestureTool==EStudioViewportTool::Pan)Scene->Pan(D.X,D.Y);
        else if(GestureTool==EStudioViewportTool::Zoom)Scene->Zoom(-D.Y*.025);
        else if(GestureTool==EStudioViewportTool::Fly)Scene->Look(D.X,D.Y);
        else Scene->Orbit(D.X,D.Y);
        return FReply::Handled();
    }
    virtual void OnMouseLeave(const FPointerEvent& E) override
    {LastPointerScreen.Reset();SCompoundWidget::OnMouseLeave(E);}
    virtual FReply OnMouseWheel(const FGeometry&,const FPointerEvent& E) override
    {
        if(!Scene.IsValid()||E.GetWheelDelta()==0) return FReply::Unhandled();
        if(PlacementDrag.IsSet())return FReply::Handled();
        if(!Drag.IsValid()&&!bFlying) { BeginGesture(TEXT("Zoom camera")); WheelSeconds=.3; }
        Scene->Zoom(E.GetWheelDelta()); return FReply::Handled();
    }
    virtual FReply OnKeyDown(const FGeometry&,const FKeyEvent& E) override
    {
        if(!Scene.IsValid()||!Scene->Model)return FReply::Unhandled();
        if(Scene->Model->IsSnapshotView()&&(E.IsCommandDown()||E.IsControlDown())&&E.GetKey()==EKeys::Z)
        {EndGesture();if(E.IsShiftDown())Scene->Model->RedoView();else Scene->Model->UndoView();return FReply::Handled();}
        if(E.IsCommandDown()||E.IsControlDown()||E.GetKey()==EKeys::Tab) return FReply::Unhandled();
        if(E.GetKey()==EKeys::Escape&&!Scene->Model->CameraPlacement()&&CancelInspection.IsBound())
        {CancelInspection.Execute();ReleaseOwnCapture();return FReply::Handled();}
        if(E.GetKey()==EKeys::F&&Scene.IsValid()) {ReleaseOwnCapture();Scene->FitCamera();return FReply::Handled();}
        if(E.GetKey()==EKeys::Escape)
        {
            if(Scene.IsValid())
            {
                if(PlacementDrag.IsSet())
                {if(Scene->Model->IsCameraPlacementCurrent()&&Scene->Model->CameraPlacementRevision==PlacementDragRevision)Scene->Model->EditCameraPlacement(PlacementDrag->Camera);}
                else if(Scene->Model->CameraPlacement())Scene->Model->CancelCameraPlacement();
            }
            ReleaseOwnCapture();return FReply::Handled();
        }
        if(E.GetKey()==EKeys::W||E.GetKey()==EKeys::A||E.GetKey()==EKeys::S||E.GetKey()==EKeys::D||E.GetKey()==EKeys::Q||E.GetKey()==EKeys::E||E.GetKey()==EKeys::LeftShift)
        { Held.Add(E.GetKey());return FReply::Handled(); }
        return FReply::Unhandled();
    }
    virtual FReply OnKeyUp(const FGeometry&,const FKeyEvent& E) override {Held.Remove(E.GetKey());return FReply::Handled();}
};
TSharedRef<SWidget> MakeStudioFlowViewport(AStudioScene* Scene,FName Tag)
{return SNew(SFlowViewport).Scene(Scene).Tag(Tag);}

class SOrientationAxes : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SOrientationAxes){} SLATE_ARGUMENT(AStudioScene*,Scene) SLATE_END_ARGS()
    TWeakObjectPtr<AStudioScene> Scene;
    void Construct(const FArguments& A) { Scene=A._Scene; }
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(80,80); }
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& O,int32 L,const FWidgetStyle&,bool) const override
    {
        if(!Scene.IsValid()) return L;
        const FQuat Q=Scene->CameraRotation().Quaternion();
        const FVector Axes[]={FVector::ForwardVector,FVector::RightVector,FVector::UpVector};
        const FLinearColor Colors[]={FLinearColor(1,.15,.08),Green,Blue};
        for(int I=0;I<3;++I)
        {
            const FVector V=Q.UnrotateVector(Axes[I]);
            const FVector2D A(38,40),B=A+FVector2D(V.Y,-V.Z)*27;
            FSlateDrawElement::MakeLines(O,L,G.ToPaintGeometry(),TArray<FVector2D>{A,B},ESlateDrawEffect::None,Colors[I],true,1.6);
            FSlateDrawElement::MakeText(O,L+1,G.ToPaintGeometry(FVector2D(14,16),FSlateLayoutTransform(B+FVector2D(-4,-7))),I==0?TEXT("X"):I==1?TEXT("Y"):TEXT("Z"),Font(9,true),ESlateDrawEffect::None,Colors[I]);
        }
        return L+1;
    }
};

/*
THESIS: Change inspection direction around the current focus without interrupting replay.
OWN-WORLD: Match the reference cube in the dense blue-black viewport, with cyan feedback.
STORY: Click a face, edge or corner; use Views for the same 26 keyboard-accessible directions.
FIRST VIEWPORT: A compact cube replaces the passive Solve axes; the field remains dominant.
FORM: Local Operate extension; focus, distance, projection, case and playback are retained.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
class SOrientationCube : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SOrientationCube){} SLATE_ARGUMENT(AStudioScene*,Scene) SLATE_END_ARGS()
    void Construct(const FArguments& A)
    {
        Scene=A._Scene;
        SetToolTipText(TAttribute<FText>::CreateLambda([this]
        {return FText::FromString(Hover.IsSet()?TEXT("View from ")+StudioOrientation::Label(Hover.GetValue()):TEXT("Click a face, edge or corner. Arrows choose a direction; Enter activates. Views offers every direction."));}));
    }
    FVector2D ComputeDesiredSize(float) const override{return FVector2D(100,110);}
    bool SupportsKeyboardFocus() const override{return true;}
    FCursorReply OnCursorQuery(const FGeometry&,const FPointerEvent&) const override
    {return FCursorReply::Cursor(Hover.IsSet()?EMouseCursor::Hand:EMouseCursor::Default);}
    FReply OnMouseMove(const FGeometry& G,const FPointerEvent& E) override
    {Hover=DirectionAt(G,G.AbsoluteToLocal(E.GetScreenSpacePosition()));Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();}
    void OnMouseLeave(const FPointerEvent& E) override
    {SLeafWidget::OnMouseLeave(E);Hover.Reset();Invalidate(EInvalidateWidgetReason::Paint);}
    FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E) override
    {
        if(E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
        Pressed=DirectionAt(G,G.AbsoluteToLocal(E.GetScreenSpacePosition()));
        if(Pressed.IsSet())KeyboardIndex=StudioOrientation::Directions().Find(Pressed.GetValue());
        return Pressed.IsSet()?FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this),EFocusCause::Mouse):FReply::Unhandled();
    }
    FReply OnMouseButtonUp(const FGeometry& G,const FPointerEvent& E) override
    {
        if(E.GetEffectingButton()!=EKeys::LeftMouseButton||!HasMouseCapture())return FReply::Unhandled();
        const auto Released=DirectionAt(G,G.AbsoluteToLocal(E.GetScreenSpacePosition()));
        if(Scene.IsValid()&&Pressed.IsSet()&&Released.IsSet()&&Pressed==Released)Scene->AlignCamera(Released.GetValue());
        Pressed.Reset();return FReply::Handled().ReleaseMouseCapture();
    }
    void OnMouseCaptureLost(const FCaptureLostEvent& E) override{Pressed.Reset();SLeafWidget::OnMouseCaptureLost(E);}
    FReply OnKeyDown(const FGeometry&,const FKeyEvent& E) override
    {
        if(!Scene.IsValid())return FReply::Unhandled();
        const auto Directions=StudioOrientation::Directions();
        if(E.GetKey()==EKeys::Left||E.GetKey()==EKeys::Up||E.GetKey()==EKeys::Right||E.GetKey()==EKeys::Down)
        {
            const int32 Delta=E.GetKey()==EKeys::Left||E.GetKey()==EKeys::Up?-1:1;
            KeyboardIndex=(KeyboardIndex+Delta+Directions.Num())%Directions.Num();
            Hover=Directions[KeyboardIndex];Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();
        }
        if(E.GetKey()==EKeys::Enter||E.GetKey()==EKeys::SpaceBar)
        {Scene->AlignCamera(Directions[KeyboardIndex]);return FReply::Handled();}
        return FReply::Unhandled();
    }
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle& Style,bool Enabled) const override
    {
        if(!Scene.IsValid())return Layer;
        const auto Q=Scene->CameraState().Orientation;
        const auto Regions=StudioOrientation::Regions(Q,CubeSize(G));
        const auto Resource=FSlateApplication::Get().GetRenderer()->GetResourceHandle(White);
        for(const auto& R:Regions)
        {
            const bool Highlight=Hover.IsSet()&&Hover.GetValue()==R.Direction;
            const FLinearColor Fill=Highlight?FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("164956"))):Raised;
            TArray<FSlateVertex> Vertices;for(const auto& P:R.Polygon)
                Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),FVector2f(P),FVector2f::ZeroVector,(Fill*Style.GetColorAndOpacityTint()).ToFColor(true)));
            FSlateDrawElement::MakeCustomVerts(Out,Layer,Resource,Vertices,TArray<SlateIndex>{0,1,2,0,2,3},nullptr,0,0);
            if(Highlight)
            {
                auto Border=R.Polygon;const FVector2D First=Border[0];Border.Add(First);
                FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Border,ESlateDrawEffect::None,Cyan,true,1.5f);
            }
            if(R.bFaceCenter)
            {
                auto Border=R.FaceOutline;const FVector2D First=Border[0];Border.Add(First);
                FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Border,ESlateDrawEffect::None,Muted,true,1.f);
                const auto Name=StudioOrientation::Label(R.Direction);
                const auto Extent=FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Name,Font(9,true));
                const FLinearColor AxisColor=R.Direction.X?FLinearColor(1,.3,.2):R.Direction.Y?Green:FLinearColor(.25,.55,1);
                FSlateDrawElement::MakeText(Out,Layer+2,G.ToPaintGeometry(Extent,FSlateLayoutTransform(R.Center-Extent*.5)),Name,Font(9,true),ESlateDrawEffect::None,AxisColor);
            }
        }
        if(HasKeyboardFocus())
        {
            const FVector2D Size=G.GetLocalSize();
            FSlateDrawElement::MakeLines(Out,Layer+3,G.ToPaintGeometry(),TArray<FVector2D>{{2,2},{Size.X-2,2},{Size.X-2,Size.Y-2},{2,Size.Y-2},{2,2}},ESlateDrawEffect::None,Cyan,true,1);
            const FString Selection=StudioOrientation::Label(StudioOrientation::Directions()[KeyboardIndex]);
            const auto Extent=FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Selection,Font(9));
            // The field can contain the same cyan as focus feedback. Keep the
            // selected direction legible over every scalar and camera angle.
            FSlateDrawElement::MakeBox(Out,Layer+4,G.ToPaintGeometry(FVector2D(Size.X-4,20),
                FSlateLayoutTransform(FVector2D(2,Size.Y-22))),&PanelBrush,ESlateDrawEffect::None,Panel);
            FSlateDrawElement::MakeText(Out,Layer+5,G.ToPaintGeometry(Extent,FSlateLayoutTransform(FVector2D((Size.X-Extent.X)*.5,Size.Y-18))),Selection,Font(9),ESlateDrawEffect::None,Cyan);
        }
        return Layer+5;
    }
private:
    TOptional<FIntVector> DirectionAt(const FGeometry& G,const FVector2D& P) const
    {
        if(!Scene.IsValid())return {};
        const auto Regions=StudioOrientation::Regions(Scene->CameraState().Orientation,CubeSize(G));
        const int32 Hit=StudioOrientation::Hit(Regions,P);return Regions.IsValidIndex(Hit)?TOptional<FIntVector>(Regions[Hit].Direction):TOptional<FIntVector>();
    }
    static FVector2D CubeSize(const FGeometry& G){return FVector2D(G.GetLocalSize().X,G.GetLocalSize().Y-20);}
    TWeakObjectPtr<AStudioScene> Scene;TOptional<FIntVector> Hover,Pressed;int32 KeyboardIndex=0;
};

static TSharedRef<SWidget> OrientationViews(AStudioScene* Scene)
{
    TWeakObjectPtr<AStudioScene> WeakScene=Scene;
    return SNew(SStudioMenuButton).Tag(TEXT("OrientationViews")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,3))
            // Placement owns the manipulation area below the cube. Its Frame
            // camera action and numeric controls remain in the right inspector.
            .Visibility_Lambda([WeakScene]{return WeakScene.IsValid()&&WeakScene->Model->CameraPlacement()?EVisibility::Collapsed:EVisibility::Visible;})
            .ToolTipText(FText::FromString(TEXT("Standard views around the current focus. Camera distance and projection stay unchanged.")))
            .OnGetMenuContent_Lambda([WeakScene]() -> TSharedRef<SWidget>
            {
                auto Items=SNew(SVerticalBox);
                Items->AddSlot().AutoHeight().Padding(0,0,0,4)[Label(TEXT("View direction"),12,Text,true)];
                Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("From scene axes · Z is up"),9,Muted)];
                for(int32 Count=1;Count<=3;++Count)
                {
                    Items->AddSlot().AutoHeight().Padding(0,6,0,4)[Label(Count==1?TEXT("Faces"):Count==2?TEXT("Edges"):TEXT("Corners"),10,Text,true)];
                    TSharedPtr<SHorizontalBox> Row;int32 Column=0;
                    for(const auto& D:StudioOrientation::Directions())
                    {
                        if(int32(D.X!=0)+int32(D.Y!=0)+int32(D.Z!=0)!=Count)continue;
                        if(Column%3==0){Row=SNew(SHorizontalBox);Items->AddSlot().AutoHeight().Padding(0,0,0,3)[Row.ToSharedRef()];}
                        Row->AddSlot().AutoWidth().Padding(0,0,3,0)[SNew(SBox).WidthOverride(86)[SNew(SButton)
                            .Tag(FName(*FString::Printf(TEXT("Orientation_%d_%d_%d"),D.X,D.Y,D.Z))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(4,5))
                            .ToolTipText(FText::FromString(TEXT("View from ")+StudioOrientation::Label(D)))
                            .OnClicked_Lambda([WeakScene,D]{if(WeakScene.IsValid())WeakScene->AlignCamera(D);FSlateApplication::Get().DismissAllMenus();return FReply::Handled();})
                            [Label(StudioOrientation::Label(D),9)]]];++Column;
                    }
                }
                Items->AddSlot().AutoHeight().Padding(0,7,0,0)[Label(TEXT("Focus and distance are retained."),9,Muted)];
                return SNew(SBox).WidthOverride(292).MaxDesiredHeight(440)[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)
                    [SNew(SScrollBox)+SScrollBox::Slot()[Items]]];
            }).ButtonContent()[Label(TEXT("Views"),9)];
}

class SJobProgress : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SJobProgress){} SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model) SLATE_END_ARGS()
    TSharedPtr<FStudioModel>M;
    void Construct(const FArguments& A){M=A._Model;}
    virtual FVector2D ComputeDesiredSize(float) const override{return FVector2D(135,150);}
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& O,int32 L,const FWidgetStyle&,bool) const override
    {
        const FVector2D Size=G.GetLocalSize(),Center(Size.X*.5,Size.Y*.43);
        const double R=FMath::Max(1.,FMath::Min(Size.X*.5-8.,FMath::Min(48.,Size.Y*.34)));
        const double Fraction=M->Progress();
        for(int Pass=0;Pass<2;++Pass){TArray<FVector2D>P;const int N=Pass?FMath::RoundToInt(80*Fraction):80;for(int I=0;I<=N;++I){const double A=-PI/2.+2.*PI*I/80.;P.Add(Center+FVector2D(FMath::Cos(A),FMath::Sin(A))*R);} if(N>0)FSlateDrawElement::MakeLines(O,L,G.ToPaintGeometry(),P,ESlateDrawEffect::None,Pass?Green:Line,true,7.);}
        const auto Measure=FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
        const auto CaptionExtent=Measure->Measure(TEXT("Replayed"),Font(9));
        int32 NumberSize=21;FVector2D NumberExtent;
        // Fit the complete two-line block inside the ring. Measure the widest
        // percentage so the type size stays steady throughout playback.
        for(;;)
        {
            NumberExtent=Measure->Measure(TEXT("100%"),Font(NumberSize));
            const FVector2D Corner(FMath::Max(NumberExtent.X,CaptionExtent.X)*.5,(NumberExtent.Y+2.+CaptionExtent.Y)*.5);
            if(Corner.Size()<=R-6.||NumberSize<=12)break;
            --NumberSize;
        }
        const double NumberY=Center.Y-(NumberExtent.Y+2.+CaptionExtent.Y)*.5;
        auto T=[&](const FString& V,double Y,int S,FLinearColor C)
        {
            const auto Extent=Measure->Measure(V,Font(S));
            FSlateDrawElement::MakeText(O,L+1,G.ToPaintGeometry(Extent,FSlateLayoutTransform(FVector2D(Center.X-Extent.X*.5,Y))),V,Font(S),ESlateDrawEffect::None,C);
        };
        T(FString::Printf(TEXT("%.0f%%"),Fraction*100),NumberY,NumberSize,Text);
        T(TEXT("Replayed"),NumberY+NumberExtent.Y+2.,9,Muted);
        T(M->StatusText(),Size.Y-28,10,M->State==EStudioRunState::Running?Green:Muted);
        return L+2;
    }
};

struct FStudioLogWorkspaceState
{
    FStudioLogView View;
    FStudioLogQuery Query;
    TArray<TSharedPtr<FStudioLogEntry>> Rows;
    TWeakPtr<SListView<TSharedPtr<FStudioLogEntry>>> List;
    TWeakPtr<SEditableTextBox> Search;
    TWeakPtr<SButton> ExpandButton;
    FStudioLogExportTask Export;
    FString Preview,RecentActivity,Detail,ExportNotice,ExportPath;
    FGuid Owner,Run,ExportOwner;
    uint64 ShownSequence=MAX_uint64,SelectedSequence=0,RecentSequence=MAX_uint64;
    bool bDirty=true,bAllProjects=false,bCurrentRun=false;
};

void SStudioWorkspace::Construct(const FArguments& A)
{
    M=A._Model;Scene=A._Scene; SetCanTick(true);
    Home4=MakeShared<FStudioHome4Session>(M);Home4Validation=MakeShared<FStudioHome4ValidationState>();
    bNewHome4Project=M->Workspace==EStudioWorkspace::Validation&&M->ProjectPath.IsEmpty()&&!M->Project.Draft.Home4.IsSet();
    PerformanceHistory=MakeShared<FStudioPerformanceHistory>();PerformanceProject=M->Project.Id;
    InspectionMarkers=MakeShared<FStudioProbeMarkerScheduler>();
    InspectionExport=MakeShared<FStudioProbeExportTask>();
    MonitorExport=MakeShared<FStudioMonitorExportTask>();
    ProbeMonitor=MakeShared<FStudioProbeMonitorSession>();
    SnapshotUI=MakeShared<FStudioSnapshotUI>();
    FieldExport=MakeShared<FStudioFieldExportUI>();
    LogState=MakeShared<FStudioLogWorkspaceState>();
    auto Restore=Button(TEXT("Restore"),TEXT("save"),[this]{if(ConfirmReplace(true)) M->RequestRecoveryOpen();});
    Restore->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !M->IsProjectOpenPending();}));
    // The sidebar is the sole workspace navigator; content starts directly below the header.
    const auto SolveView=SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,7,0)[Center()]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(322)
            .Visibility_Lambda([this]{return M->bViewportExpanded&&!bInspectionOpen&&!M->CameraPlacement()&&!bPerformanceOpen?EVisibility::Collapsed:EVisibility::Visible;})
            [SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->CameraPlacement()?1:bInspectionOpen?2:bPerformanceOpen?3:0;})
                +SWidgetSwitcher::Slot()[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->Project.Draft.Home4.IsSet()?1:0;})
                    +SWidgetSwitcher::Slot()[Settings()]+SWidgetSwitcher::Slot()[Home4Inspector()]]
                +SWidgetSwitcher::Slot()[CameraPlacementControls()]
                +SWidgetSwitcher::Slot()[InspectionControls()]
                +SWidgetSwitcher::Slot()[SAssignNew(PerformancePanel,SStudioPerformancePanel).Tag(TEXT("PerformancePanel"))
                    .Model(M).History(PerformanceHistory).OnClose_Lambda([this]{OpenPerformance(false);})]]];
    const auto SolveSurface=SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->bActivityLogExpanded?1:0;})
        +SWidgetSwitcher::Slot()[SolveView]
        +SWidgetSwitcher::Slot()[ActivityLogPanel()];
    const auto ProjectsSurface=Projects();
    const auto DashboardSurface=Dashboard();
    ChildSlot[SNew(SBorder).BorderImage(&Background).Padding(0)
    [SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[Header()]
        +SVerticalBox::Slot().AutoHeight()[WorkspaceContext()]
        +SVerticalBox::Slot().AutoHeight()
        [SNew(SBorder).BorderImage(&RaisedBrush).Padding(12,6).Visibility_Lambda([this]{return M->PendingRecovery.IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Unsaved project recovered from the previous session."),10,Amber)]
                +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[Restore]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Discard recovery"),TEXT("stop"),[this]{M->DiscardRecovery();})]]]

        +SVerticalBox::Slot().AutoHeight()
        [SNew(SBorder).BorderImage(&RaisedBrush).Padding(12,7)
            .Visibility_Lambda([this]{return M->RecordingRepair.IsSet()?EVisibility::Visible:EVisibility::Collapsed;})
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Live([this]
                {return M->RecordingRepair.IsSet()?TEXT("Could not open ")+FPaths::GetCleanFilename(M->RecordingRepair->ProjectPath)+
                    (M->RecordingRepair->bReconstruction?TEXT(". Locate its reconstruction, or open the original points. Current project retained."):
                        TEXT(". Locate the original recording folder; the current project is retained.")):FString();},10,Amber,true)]
                +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[SNew(SButton).Tag(TEXT("LocateRepairSource"))
                    .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,7))
                    .IsEnabled_Lambda([this]{return !M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();})
                    .OnClicked_Lambda([this]{RepairRecording();return FReply::Handled();})
                    [Live([this]{return M->RecordingRepair.IsSet()&&M->RecordingRepair->bReconstruction?TEXT("Locate surface…"):TEXT("Locate recording…");})]]
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[SNew(SButton).Tag(TEXT("OpenOriginalPoints"))
                    .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,7))
                    .Visibility_Lambda([this]{return M->RecordingRepair.IsSet()&&M->RecordingRepair->bReconstruction?EVisibility::Visible:EVisibility::Collapsed;})
                    .IsEnabled_Lambda([this]{return !M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();})
                    .OnClicked_Lambda([this]
                    {if(M->RecordingRepair.IsSet()&&ConfirmReplace(M->RecordingRepair->bRecovery))M->RetryWithoutReconstruction();return FReply::Handled();})
                    [Label(TEXT("Open original points"))]]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Dismiss"),TEXT("stop"),[this]{M->DismissRecordingRepair();})]]]
        +SVerticalBox::Slot().FillHeight(1)
        [SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[Navigation()]
            +SHorizontalBox::Slot().FillWidth(1).Padding(7,0,7,0)[SNew(SWidgetSwitcher)
                .WidgetIndex_Lambda([this]{return M->Workspace==EStudioWorkspace::Projects?1:M->Workspace==EStudioWorkspace::Dashboard?2:M->Workspace==EStudioWorkspace::Geometry?3:M->Workspace==EStudioWorkspace::Materials?4:M->Workspace==EStudioWorkspace::Domain?5:M->Workspace==EStudioWorkspace::BoundaryConditions?6:M->Workspace==EStudioWorkspace::Meshing?7:M->Workspace==EStudioWorkspace::Monitors?8:M->Workspace==EStudioWorkspace::Results?9:M->Workspace==EStudioWorkspace::PostProcessing?10:M->Workspace==EStudioWorkspace::Bodies?11:M->Workspace==EStudioWorkspace::Run?12:M->Workspace==EStudioWorkspace::Validation?13:M->Workspace==EStudioWorkspace::Reports?14:M->Workspace==EStudioWorkspace::Settings?15:0;})
                +SWidgetSwitcher::Slot()[SolveSurface]
                +SWidgetSwitcher::Slot()[ProjectsSurface]
                +SWidgetSwitcher::Slot()[DashboardSurface]
                +SWidgetSwitcher::Slot()[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return bHome4Geometry&&M->Project.Draft.Home4.IsSet()?1:0;})
                    +SWidgetSwitcher::Slot()[GeometryWorkspace()]+SWidgetSwitcher::Slot()[Home4Page(TEXT("Geometry"))]]
                +SWidgetSwitcher::Slot()[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->Project.Draft.Home4.IsSet()?1:0;})
                    +SWidgetSwitcher::Slot()[MaterialsWorkspace()]+SWidgetSwitcher::Slot()[Home4Page(TEXT("Fluids & Interface"))]]
                +SWidgetSwitcher::Slot()[DomainWorkspace()]
                +SWidgetSwitcher::Slot()[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->Project.Draft.Home4.IsSet()&&!bHome4FaceAssignments?1:0;})
                    +SWidgetSwitcher::Slot()[BoundaryWorkspace()]+SWidgetSwitcher::Slot()[Home4Page(TEXT("Boundaries & Zones"))]]
                +SWidgetSwitcher::Slot()[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->Project.Draft.Home4.IsSet()&&!bHome4LatticePreview?1:0;})
                    +SWidgetSwitcher::Slot()[LatticeWorkspace()]+SWidgetSwitcher::Slot()[Home4Page(TEXT("Lattice"))]]
                +SWidgetSwitcher::Slot()[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->Project.Draft.Home4.IsSet()&&!bHome4RecordingMonitors?1:0;})
                    +SWidgetSwitcher::Slot()[MonitorWorkspace()]
                    +SWidgetSwitcher::Slot()[SAssignNew(Home4Monitors,SStudioHome4Monitors).Model(M)
                        .UnitDisplay_Lambda([this]{return M->UnitDisplay;})
                        .OnLocateCell_Lambda([this](const FStudioHome4CellFacts& Facts){LocateHome4Cell(Facts);})]]
                +SWidgetSwitcher::Slot()[SAssignNew(Results,SStudioResultsWorkspace).Model(M).Scene(Scene.Get())
                    .OnInspect_Lambda([this]{Navigate(EStudioWorkspace::Solve);})
                    .OnImport_Lambda([this]{ImportRecording();})
                    .Locate([this](const FString& Id,const FString& Path){LocateRecording(Id,Path);})]
                +SWidgetSwitcher::Slot()[SAssignNew(Pipelines,SStudioPipelineWorkspace).Tag(TEXT("PipelineWorkspace")).Model(M).World(Scene->GetWorld())
                    .OnResults_Lambda([this]{Navigate(EStudioWorkspace::Results);})]
                +SWidgetSwitcher::Slot()[Home4Page(TEXT("Bodies"))]
                +SWidgetSwitcher::Slot()[Home4Page(TEXT("Run"))]
                +SWidgetSwitcher::Slot()[Home4Page(TEXT("Validation"))]
                +SWidgetSwitcher::Slot()[Home4Page(TEXT("Reports"))]
                +SWidgetSwitcher::Slot()[Home4Page(TEXT("Settings"))]]]
        +SVerticalBox::Slot().AutoHeight().Padding(12,6)
        [SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[Live([this]{return M->Workspace==EStudioWorkspace::Geometry?TEXT("CASE GEOMETRY"):M->Workspace==EStudioWorkspace::Materials?TEXT("CASE MATERIALS"):M->Workspace==EStudioWorkspace::Domain?TEXT("CASE DOMAIN"):M->Workspace==EStudioWorkspace::BoundaryConditions?TEXT("CASE BOUNDARIES"):M->Workspace==EStudioWorkspace::Meshing?TEXT("CASE LATTICE"):(M->Project.bControlHarness?TEXT("DEVELOPMENT ADAPTER"):TEXT("REPLAY · RECORDED CFD"));},8,Amber)]
            +SHorizontalBox::Slot().FillWidth(1).Padding(12,0).VAlign(VAlign_Center)[Live([this]{return M->IsProjectOpenPending()?M->ProjectOpenStatus():SnapshotUI->IsBusy()?SnapshotUI->ButtonLabel()+TEXT(" · Open Snapshot in Fields for progress or Cancel"):M->Notice.IsEmpty()?(M->Project.bControlHarness?FString(TEXT("Development controls · no CFD computed")):M->Solver->Descriptor().Title+TEXT(" · Replay")):M->Notice;},9,Muted)]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,12,0)
                [SNew(SBox).Visibility_Lambda([this]{return M->IsProjectOpenPending()?EVisibility::Visible:EVisibility::Collapsed;})
                [SNew(SButton).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,3))
                    .IsEnabled_Lambda([this]{return M->IsProjectOpening();})
                    .OnClicked_Lambda([this]{M->CancelProjectOpen();return FReply::Handled();})
                    [Label(TEXT("Cancel opening"),9)]]]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,12,0)
                [SNew(SBox).Visibility_Lambda([this]{return M->IsRecordingLoadPending()?EVisibility::Visible:EVisibility::Collapsed;})
                    [SNew(SButton).Tag(TEXT("CancelRecording")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,3))
                        .IsEnabled_Lambda([this]{return M->bRecordingLoading;})
                        .OnClicked_Lambda([this]{M->CancelRecording();M->Notice=TEXT("Recording change cancelled. Current view retained.");return FReply::Handled();})
                        [Label(TEXT("Cancel recording"),9)]]]
            +SHorizontalBox::Slot().AutoWidth()[Label(TEXT("LBM Solver Studio  0.1"),9,Muted)]]]];
    RefreshProjectLists();
}
TSharedRef<SWidget> SStudioWorkspace::Home4Page(const FString& Page)
{
    return SNew(SStudioHome4Panel).Model(M).Session(Home4).Page(Page).Validation(Home4Validation)
        .OnRecipe_Lambda([this](const FString& Id){ChooseHome4Recipe(Id);})
        .OnSubmit_Lambda([this]{DispatchControl(EStudioJobCommand::Submit);})
        .Telemetry_Lambda([this]{return Home4Monitors?Home4Monitors->DisplayedTelemetry():nullptr;});
}
bool SStudioWorkspace::EnsureHome4Resolved()
{
    if(!Home4)return true;Home4->Refresh();if(!Home4->IsDirty())return true;
    Home4->Status=TEXT("Apply or revert HOME4 configuration edits before saving, replacing the project or submitting a run.");
    Navigate(EStudioWorkspace::Run);M->Notice=Home4->Status;return false;
}
void SStudioWorkspace::ChooseHome4Recipe(const FString& Id)
{
    const auto* Recipe=StudioHome4Recipes::Find(Id);if(!Recipe||!EnsureHome4Resolved())return;
    if(bNewHome4Project)
    {
        if(!ConfirmReplace())return;
        FString Path;if(!StudioFileDialog::Project(true,M->ProjectPath,Recipe->Name,Path))return;
        auto Spec=Recipe->Template;Spec.LineageId=FGuid::NewGuid().ToString();
        if(!M->CreateProject(Path,FPaths::GetBaseFilename(Path),&Spec))return;
        bNewHome4Project=false;Home4->Revert();Scene->ApplyCamera(M->Project.Camera);M->AcceptLoadedView();
        M->Notice=TEXT("HOME4 recipe project created. Fields retains a separately identified published recording until HOME4 output is imported.");
    }
    else if(!Home4->ApplyRecipe(Id))return;
    Navigate(EStudioWorkspace::Materials);
}
void SStudioWorkspace::LocateHome4Cell(const FStudioHome4CellFacts& Facts)
{
    const auto Volume=M->Solver->VolumeReconstruction();
    const auto Stream=Home4Monitors?Home4Monitors->DisplayedTelemetry():nullptr;
    if(!Facts.Cell.IsSet()||!Volume||!Volume->OriginalGrid||!Stream||!Stream->Latest().IsSet())
    {M->Notice=TEXT("Load the matching original HOME4 grid and identified science log before locating a cell.");return;}
    const auto& Grid=*Volume->OriginalGrid;const auto& Sample=Stream->Latest().GetValue();FGuid SourceRun;
    if(!Home4Monitors->OriginalRunIdentity().IsSet()||!FGuid::Parse(Grid.SourceRunId,SourceRun)||
        SourceRun!=Home4Monitors->OriginalRunIdentity().GetValue()||SourceRun!=Sample.Source.RunId)
    {M->Notice=TEXT("The science log and recording need the same explicit original run ID. The camera was retained.");return;}
    if(Facts.Level.Get(0)!=0){M->Notice=TEXT("Select the matching multidomain level recording before locating this cell. Patches are not auto-stitched.");return;}
    const auto Cell=Facts.Cell.GetValue();FIntVector Selected;
    for(int32 A=0;A<3;++A)
    {
        const int32 Delta=Cell[A]-Grid.CropMinimum[A];
        if(Delta<0||Cell[A]>=Grid.CropMaximum[A]||Delta%Grid.PreviewStride!=0)
        {M->Notice=TEXT("This cell is outside the loaded crop or preview stride. Load the original full grid to locate it.");return;}
        Selected[A]=Delta/Grid.PreviewStride;
    }
    const FVector Source=Grid.OriginMeters+Grid.SpacingMeters*FVector(Selected);
    const FVector Position(Source.X,Source.Z,Source.Y);
    auto Camera=M->Project.Camera;const FVector Shift=Position-Camera.Focus;Camera.Position+=Shift;Camera.Focus=Position;
    M->EditView(TEXT("Locate original HOME4 cell"),[&](auto& View){View.Camera=Camera;});Scene->ApplyCamera(Camera);Navigate(EStudioWorkspace::Solve);
    FStudioProbeObject Probe;Probe.Name=FString::Printf(TEXT("Trouble cell %d,%d,%d"),Cell.X,Cell.Y,Cell.Z);Probe.A=Position;Probe.Method=EStudioProbeMethod::OriginalPoint;
    Probe.PointId=int64(Cell.X)+int64(Grid.OriginalDimensions.X)*(int64(Cell.Y)+int64(Grid.OriginalDimensions.Y)*Cell.Z);
    Probe.Field=Grid.PhaseField;const FGuid ProbeId=Probe.Id;if(M->AddProbe(Probe))M->SelectInspectionObject(ProbeId);
    M->Notice=FString::Printf(TEXT("Original cell (%d, %d, %d) · phi %s · tau %s · %s · camera focused; replay frame unchanged"),Cell.X,Cell.Y,Cell.Z,
        *(Facts.Phi.IsSet()?FString::Printf(TEXT("%.6g"),Facts.Phi.GetValue()):TEXT("not supplied")),
        *(Facts.Tau.IsSet()?FString::Printf(TEXT("%.6g"),Facts.Tau.GetValue()):TEXT("not supplied")),*Facts.Zone);
}
TSharedRef<SWidget> SStudioWorkspace::WorkspaceContext()
{
    auto Rows=SNew(SHorizontalBox);
    auto Add=[&](const TCHAR* Name,FName Tag,TFunction<bool()> Visible,TFunction<bool()> Selected,TFunction<void()> Select)
    {
        Rows->AddSlot().AutoWidth().Padding(0,0,4,0)[SNew(SButton).Tag(Tag).ButtonStyle(&NavigationStyle()).ContentPadding(FMargin(12,8))
            .Visibility_Lambda([Visible]{return Visible()?EVisibility::Visible:EVisibility::Collapsed;})
            .OnClicked_Lambda([Select]{Select();return FReply::Handled();})
            [SNew(STextBlock).Font(Font(10)).Text(FText::FromString(Name)).ColorAndOpacity_Lambda([Selected]{return Selected()?Cyan:Muted;})]];
    };
    auto Fields=[this]{return M->Workspace==EStudioWorkspace::Solve||M->Workspace==EStudioWorkspace::Results||M->Workspace==EStudioWorkspace::PostProcessing;};
    Add(TEXT("Scene"),TEXT("FieldsScene"),Fields,[this]{return M->Workspace==EStudioWorkspace::Solve;},[this]{Navigate(EStudioWorkspace::Solve);});
    Add(TEXT("Recordings & runs"),TEXT("FieldsRecordings"),Fields,[this]{return M->Workspace==EStudioWorkspace::Results;},[this]{Navigate(EStudioWorkspace::Results);});
    Add(TEXT("Analysis"),TEXT("FieldsAnalysis"),Fields,[this]{return M->Workspace==EStudioWorkspace::PostProcessing;},[this]{Navigate(EStudioWorkspace::PostProcessing);});
    auto Geometry=[this]{return M->Project.Draft.Home4.IsSet()&&M->Workspace==EStudioWorkspace::Geometry;};
    Add(TEXT("Import & surfaces"),TEXT("GeometrySurfaces"),Geometry,[this]{return !bHome4Geometry;},[this]{bHome4Geometry=false;});
    Add(TEXT("Attitude & SDF"),TEXT("GeometrySDF"),Geometry,[this]{return bHome4Geometry;},[this]{bHome4Geometry=true;});
    auto Lattice=[this]{return M->Workspace==EStudioWorkspace::Meshing||M->Workspace==EStudioWorkspace::Domain;};
    Add(TEXT("Grid & units"),TEXT("LatticeGrid"),[this,Lattice]{return Lattice()&&M->Project.Draft.Home4.IsSet();},[this]{return M->Workspace==EStudioWorkspace::Meshing&&!bHome4LatticePreview;},[this]{bHome4LatticePreview=false;Navigate(EStudioWorkspace::Meshing);});
    Add(TEXT("Domain bounds"),TEXT("LatticeDomain"),Lattice,[this]{return M->Workspace==EStudioWorkspace::Domain;},[this]{Navigate(EStudioWorkspace::Domain);});
    Add(TEXT("Occupancy preview"),TEXT("LatticeOccupancy"),Lattice,[this]{return M->Workspace==EStudioWorkspace::Meshing&&(bHome4LatticePreview||!M->Project.Draft.Home4.IsSet());},[this]{bHome4LatticePreview=true;Navigate(EStudioWorkspace::Meshing);});
    auto Zones=[this]{return M->Project.Draft.Home4.IsSet()&&M->Workspace==EStudioWorkspace::BoundaryConditions;};
    Add(TEXT("Zones & damping"),TEXT("BoundaryZones"),Zones,[this]{return !bHome4FaceAssignments;},[this]{bHome4FaceAssignments=false;});
    Add(TEXT("Face assignments"),TEXT("BoundaryFaces"),Zones,[this]{return bHome4FaceAssignments;},[this]{bHome4FaceAssignments=true;});
    auto Monitors=[this]{return M->Project.Draft.Home4.IsSet()&&M->Workspace==EStudioWorkspace::Monitors;};
    Add(TEXT("Ledgers & budgets"),TEXT("MonitorScience"),Monitors,[this]{return !bHome4RecordingMonitors;},[this]{bHome4RecordingMonitors=false;});
    Add(TEXT("Recording channels"),TEXT("MonitorRecorded"),Monitors,[this]{return bHome4RecordingMonitors;},[this]{bHome4RecordingMonitors=true;});
    return SNew(SBorder).BorderImage(&PanelBrush).Padding(203,0,8,0)
        .Visibility_Lambda([Fields,Geometry,Lattice,Zones,Monitors]{return Fields()||Geometry()||Lattice()||Zones()||Monitors()?EVisibility::Visible:EVisibility::Collapsed;})[Rows];
}
TSharedRef<SWidget> SStudioWorkspace::Home4Inspector()
{
    auto Layers=SNew(SVerticalBox);
    struct FLayer {const TCHAR* Label;const TCHAR* Id;int32 Mode;};
    const FLayer Choices[]={{TEXT("Speed slice"),TEXT("speed"),0},
        {TEXT("Positive-Q surface"),TEXT("q"),3},{TEXT("Signed helicity plane"),TEXT("helicity"),0},
        {TEXT("Vorticity · Q color, |ω|² opacity"),TEXT("q"),4}};
    for(const auto& Layer:Choices)
    {
        Layers->AddSlot().AutoHeight().Padding(0,0,0,5)[SNew(SButton).Tag(FName(*(FString(TEXT("Home4Layer."))+(Layer.Mode==4?TEXT("vorticity"):Layer.Id))))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
            .IsEnabled_Lambda([this,Id=FString(Layer.Id),Mode=Layer.Mode]{return M->Solver->VolumeReconstruction()&&M->Solver->VolumeReconstruction()->OriginalGrid&&M->Solver->Descriptor().Scalars.ContainsByPredicate([&](const auto& S){return S.Id==Id;})&&(Mode!=4||M->Solver->Descriptor().Scalars.ContainsByPredicate([](const auto& S){return S.Id==TEXT("vorticity_magnitude");}));})
            .ToolTipText(FText::FromString(TEXT("Requires the named original or explicitly derived source field. Isovalue and slice controls remain adjustable.")))
            .OnClicked_Lambda([this,Id=FString(Layer.Id),Mode=Layer.Mode]
            {
                M->EditView(TEXT("HOME4 field layer"),[&](auto& View)
                {
                    View.Display.ScalarField=Id;View.Display.bHome4Vorticity=Mode==4;View.Display.bVolume=Mode==4;View.Display.bCutPlane=Mode==0;View.Display.bVolumeIsosurface=Mode==1||Mode==2||Mode==3;
                    View.Display.VolumeIsovalue=Mode==1?.5:Mode==2?0:FMath::Max(1.e-12,M->Solver->Descriptor().Scalars.FindByPredicate([&](const auto& S){return S.Id==Id;})->DefaultDisplayMaximum.Get(1.)*.25);
                });return FReply::Handled();
            })[Label(Layer.Label,9)]];
    }
    struct FSurfaceLayer{const TCHAR* Name;const TCHAR* Id;bool FStudioViewSettings::*Member;};
    for(const auto& Layer:TArray<FSurfaceLayer>{{TEXT("Interface surface"),TEXT("phi"),&FStudioViewSettings::bHome4InterfaceSurface},{TEXT("Obstacle mask"),TEXT("solid"),&FStudioViewSettings::bHome4ObstacleSurface},{TEXT("SDF zero surface"),TEXT("sdf"),&FStudioViewSettings::bHome4SdfSurface}})
    {
        auto Toggle=Check(Layer.Name,[this,Member=Layer.Member]{return M.Get()->*Member;},[this,Member=Layer.Member](bool V){M->EditView(TEXT("HOME4 surface layer"),[&](auto& View){View.Display.*Member=V;});});
        Toggle->SetEnabled(TAttribute<bool>::CreateLambda([this,Id=FString(Layer.Id)]{return M->Solver->VolumeReconstruction()&&M->Solver->VolumeReconstruction()->OriginalGrid&&M->Solver->Descriptor().Scalars.ContainsByPredicate([&](const auto& S){return S.Id==Id;});}));
        Layers->AddSlot().AutoHeight().Padding(0,5)[Toggle];
    }
    Layers->AddSlot().AutoHeight()[Row(TEXT("Surface φ"),Number([this]{return M->Home4InterfaceIsovalue;},[this](double V){M->EditView(TEXT("Interface isovalue"),[&](auto& View){View.Display.Home4InterfaceIsovalue=V;});},.001,.999,TEXT(""),.05))];
    Layers->AddSlot().AutoHeight().Padding(0,8)[Check(TEXT("Mask air in diagnostic fields"),[this]{return M->bHome4AirMask;},[this](bool V){M->EditView(TEXT("HOME4 air mask"),[&](auto& View){View.Display.bHome4AirMask=V;});})];
    Layers->AddSlot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("Derivative fields retain their supplied validity mask and exclude one-node stencils touching air, solid or grid edges. Multidomain patches keep their own origin and spacing."),9,Muted,true)];

    return SNew(SBorder).BorderImage(&PanelBrush).Padding(10)[SNew(SScrollBox)+SScrollBox::Slot()[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,10)[Label(TEXT("Fields"),14,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]{return M->Solver->Descriptor().Title;},10,Cyan,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("Original recording · case edits do not alter recorded fields or their unit map."),9,Muted,true)]
        +SVerticalBox::Slot().AutoHeight()[Layers]
        +SVerticalBox::Slot().AutoHeight()[DisplayTools()]
        +SVerticalBox::Slot().AutoHeight()[Section(TEXT("Replay"),SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,8)[Row(TEXT("Replay speed"),Number([this]{return M->PlaybackRate;},[this](double V){M->PlaybackRate=V;},.25,4.,TEXT("×"),.25))]
            +SVerticalBox::Slot().AutoHeight()[Check(TEXT("Loop recording"),[this]{return M->bLoopPlayback;},[this](bool V){M->bLoopPlayback=V;})])]]];
}

TSharedRef<SWidget> SStudioWorkspace::Header()
{
    auto Run=Button(TEXT("Run"),TEXT("run"),[this]
    {
        DispatchControl(EStudioJobCommand::Submit);
    },Green);Run->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanControl(EStudioJobCommand::Submit);}));
    Run->SetTag(TEXT("RunControl"));
    auto Pause=Button(TEXT("Pause / Resume"),TEXT("pause"),[this]{M->Control(EStudioJobCommand::Pause);},Blue);Pause->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanControl(EStudioJobCommand::Pause);}));
    Pause->SetContent(SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Icon(TEXT("pause"),Blue,14)]
        +SHorizontalBox::Slot().AutoWidth().Padding(6,0).VAlign(VAlign_Center)[Live([this]{return (M->Project.bControlHarness?M->Job().State()==EStudioJobState::Paused:M->State==EStudioRunState::Paused)?TEXT("Resume"):TEXT("Pause");},10)]);
    auto Stop=Button(TEXT("Stop"),TEXT("stop"),[this]{M->Control(EStudioJobCommand::Stop);},FLinearColor(.9,.22,.25));Stop->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanControl(EStudioJobCommand::Stop);}));
    auto Step=Button(TEXT("Step"),TEXT("step"),[this]{M->Control(EStudioJobCommand::Step);});Step->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanControl(EStudioJobCommand::Step);}));
    Run->SetToolTipText(TAttribute<FText>::CreateLambda([this]{return FText::FromString(M->Project.bControlHarness?TEXT("Submit or resume a control-harness test. No CFD is computed."):TEXT("Start or resume the recording."));}));
    Step->SetToolTipText(TAttribute<FText>::CreateLambda([this]{return FText::FromString(M->Project.bControlHarness?TEXT("Test one step acknowledgement. The recorded field remains unchanged."):TEXT("Advance one recorded snapshot."));}));
    return SNew(SBox).HeightOverride(57)[SNew(SBorder).BorderImage(&PanelBrush).Padding(17,9)
    [SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Icon(TEXT("box"),Cyan,25)]
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10,0,22,0)[Label(TEXT("LBM Solver Studio"),15,Text,true)]
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,12,0)[Label(TEXT("Project"),9,Muted)]
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(SStudioMenuButton).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
            .OnGetMenuContent(this,&SStudioWorkspace::ProjectMenu)
            .ButtonContent()[Live([this]{return M->Project.Name+(M->bDirty?TEXT(" *"):TEXT(""));},10)]]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[Button(TEXT("New Project"),TEXT("plus"),[this]{Execute(ECommand::NewProject);})]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[Button(TEXT("Import Geometry"),TEXT("folder"),[this]{ImportGeometry();})]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,14,0)[Button(TEXT("Save"),TEXT("save"),[this]{Execute(ECommand::Save);})]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Run]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Pause]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Stop]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,14,0)[Step]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SStudioMenuButton).Tag(TEXT("ExportMenu")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7))
            .OnGetMenuContent(this,&SStudioWorkspace::ExportMenu)
            .OnMenuOpenChanged_Lambda([State=FieldExport](bool Open){State->MenuOpenChanged(Open);})
            .ButtonContent()[Live([this]{return FieldExport->IsBusy()?TEXT("Exporting…"):TEXT("Export");},10)]]
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10,0,0,0)
            [SAssignNew(NotificationsButton,SStudioMenuButton).Tag(TEXT("HeaderNotifications")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7))
                .HasDownArrow(false).AccessibleText(FText::FromString(TEXT("Notifications")))
                .ToolTipText_Lambda([this]{return FText::FromString(FString::Printf(TEXT("Notifications · %d unread"),M->Notifications().UnreadCount()));})
                .OnMenuOpenChanged_Lambda([this](bool Open){if(!Open)NotificationsButton->FocusButton();})
                .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                {
                    return SNew(SStudioNotifications).Tag(TEXT("NotificationsPanel")).Model(M)
                        .Reveal([this](uint64 Id){return RevealNotification(Id);})
                        .OnClose_Lambda([this]{NotificationsButton->SetIsOpen(false);NotificationsButton->FocusButton();});
                }).ButtonContent()[SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SIcon).Name(TEXT("bell")).Size(17)
                        .Color_Lambda([this]{return M->Notifications().UnreadCount()?Cyan:Muted;})]
                    +SHorizontalBox::Slot().AutoWidth().Padding(4,0,0,0).VAlign(VAlign_Center)[SNew(STextBlock).Tag(TEXT("NotificationBadge"))
                        .Font(Font(9,true)).ColorAndOpacity(Cyan)
                        .Visibility_Lambda([this]{return M->Notifications().UnreadCount()?EVisibility::Visible:EVisibility::Collapsed;})
                        .Text_Lambda([this]{const int32 Count=M->Notifications().UnreadCount();return FText::FromString(Count>99?TEXT("99+"):FString::FromInt(Count));})]]]
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6,0,0,0)
            [SNew(SStudioMenuButton).Tag(TEXT("GlobalUnits")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7))
                .ToolTipText(FText::FromString(TEXT("Readout units. Conversion requires the original source map; case fields keep their labeled input units.")))
                .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                {
                    auto Choices=SNew(SVerticalBox);const TCHAR* Names[]={TEXT("Lattice"),TEXT("Physical"),TEXT("Nondimensional")};
                    for(int32 I=0;I<3;++I)Choices->AddSlot().AutoHeight()[Button(Names[I],TEXT("select"),[this,I]{M->UnitDisplay=EStudioHome4UnitDisplay(I);M->SaveSession();FSlateApplication::Get().DismissAllMenus();})];
                    return Choices;
                }).ButtonContent()[Live([this]{return M->UnitDisplay==EStudioHome4UnitDisplay::Physical?TEXT("SI"):M->UnitDisplay==EStudioHome4UnitDisplay::Nondimensional?TEXT("ND"):TEXT("LU");},9,Cyan)]]
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6,0,0,0)
            [SAssignNew(HelpButton,SStudioMenuButton).Tag(TEXT("HeaderHelp")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7))
                .HasDownArrow(false).ToolTipText(FText::FromString(TEXT("Help and keyboard shortcuts · F1")))
                .AccessibleText(FText::FromString(TEXT("Help")))
                .OnMenuOpenChanged_Lambda([this](bool Open){if(!Open)HelpButton->FocusButton();})
                .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                {
                    return SNew(SStudioHelpPanel).Tag(TEXT("HelpPanel")).Model(M).Scene(Scene.Get())
                        .OnResults_Lambda([this]{FSlateApplication::Get().DismissAllMenus();Navigate(EStudioWorkspace::Results);})
                        .OnClose_Lambda([this]{HelpButton->SetIsOpen(false);HelpButton->FocusButton();});
                }).ButtonContent()[Icon(TEXT("help"),Muted,17)]]]];
}
void SStudioWorkspace::OpenHelp()
{
    HelpButton->SetIsOpen(true,true);
}
bool SStudioWorkspace::RevealNotification(uint64 Id)
{
    FString Reason;if(!M->CanRevealNotification(Id,Reason)){M->Notice=Reason;return false;}
    const FStudioNotification Entry=*M->Notifications().Find(Id);
    FSlateApplication::Get().DismissAllMenus();
    if(Entry.Kind==EStudioNotificationKind::Completed)
    {
        Navigate(EStudioWorkspace::Results);
        return Results->RevealNotification(Entry.RunId,Entry.SourceReference);
    }
    Navigate(EStudioWorkspace::Solve);RefreshActivityLog();
    auto& S=*LogState;S.Query=FStudioLogQuery();S.Query.ProjectId=M->Project.Id;
    S.bAllProjects=S.bCurrentRun=false;S.Search.Pin()->SetText(FText::GetEmpty());
    S.View.SetFollowing(true,M->ActivityLog());S.View.SetFollowing(false,M->ActivityLog());S.View.ShowRetained();
    S.SelectedSequence=Entry.LogSequence;S.bDirty=true;RefreshActivityLog();ExpandActivityLog(true);
    if(auto* Row=S.Rows.FindByPredicate([&](const auto& Value){return Value->Sequence==Entry.LogSequence;}))
    {S.List.Pin()->SetSelection(*Row);S.List.Pin()->RequestScrollIntoView(*Row);}
    return true;
}
TSharedRef<SWidget> SStudioWorkspace::Navigation()
{
    auto List=SNew(SVerticalBox);
    struct FDestination { const TCHAR* Name; const TCHAR* Icon; EStudioWorkspace Page; };
    const FDestination Items[]={
        {TEXT("Dashboard"),TEXT("box"),EStudioWorkspace::Dashboard},
        {TEXT("Projects"),TEXT("folder"),EStudioWorkspace::Projects},
        {TEXT("Geometry"),TEXT("box"),EStudioWorkspace::Geometry},
        {TEXT("Lattice"),TEXT("box"),EStudioWorkspace::Meshing},
        {TEXT("Fluids & Interface"),TEXT("box"),EStudioWorkspace::Materials},
        {TEXT("Boundaries & Zones"),TEXT("settings"),EStudioWorkspace::BoundaryConditions},
        {TEXT("Bodies"),TEXT("box"),EStudioWorkspace::Bodies},
        {TEXT("Run"),TEXT("run"),EStudioWorkspace::Run},
        {TEXT("Monitors"),TEXT("chart"),EStudioWorkspace::Monitors},
        {TEXT("Fields"),TEXT("chart"),EStudioWorkspace::Solve},
        {TEXT("Validation"),TEXT("chart"),EStudioWorkspace::Validation},
        {TEXT("Reports"),TEXT("chart"),EStudioWorkspace::Reports},
        {TEXT("Settings"),TEXT("settings"),EStudioWorkspace::Settings}};
    auto Selected=[this](EStudioWorkspace Destination)
    {return M->Workspace==Destination||(Destination==EStudioWorkspace::Solve&&(M->Workspace==EStudioWorkspace::Results||M->Workspace==EStudioWorkspace::PostProcessing))||
        (Destination==EStudioWorkspace::Meshing&&M->Workspace==EStudioWorkspace::Domain);};
    for(const auto& Item:Items)
    {
        const auto Destination=Item.Page; const FString Name=Item.Name;const int32 I=int32(Destination);
        const bool bAvailable=FStudioModel::IsWorkspaceAvailable(Destination);
        List->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(45)
            [SNew(SBorder).BorderImage_Lambda([this,Destination,Selected]{return Selected(Destination)?&RaisedBrush:&Background;}).Padding(0)
            [SAssignNew(NavigationButtons.FindOrAdd(Destination),SButton).Tag(FName(*FString::Printf(TEXT("Workspace%d"),I))).ButtonStyle(&NavigationStyle()).ContentPadding(FMargin(14,0)).IsEnabled(bAvailable)
                .ToolTipText(FText::FromString(bAvailable?Name:Name+TEXT(" — being implemented")))
                .OnClicked_Lambda([this,Destination]{Navigate(Destination);return FReply::Handled();})
                [SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Icon(Item.Icon,Muted,15)]
                    +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(8,0,0,0)
                    [SNew(STextBlock).Text(FText::FromString(Name)).Font(Font(9))
                        .Visibility_Lambda([this]{return M->bSidebarCollapsed?EVisibility::Collapsed:EVisibility::Visible;})
                        .ColorAndOpacity_Lambda([this,Destination,Selected]{return Selected(Destination)?Cyan:Muted;})]]]]];
    }
    return SNew(SBox).WidthOverride_Lambda([this]{return M->bSidebarCollapsed?52.f:196.f;})
    [SNew(SBorder).BorderImage(&Background).Padding(0,8)
    [SNew(SVerticalBox)
        +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12.f)+SScrollBox::Slot()[List]]
        +SVerticalBox::Slot().AutoHeight()[SNew(SButton).ButtonStyle(&NavigationStyle()).ContentPadding(FMargin(14,12))
            .ToolTipText_Lambda([this]{return FText::FromString(M->bSidebarCollapsed?TEXT("Expand navigation"):TEXT("Collapse navigation"));})
            .OnClicked_Lambda([this]{M->bSidebarCollapsed=!M->bSidebarCollapsed;M->SaveSession();return FReply::Handled();})
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->bSidebarCollapsed?1:0;})
                    +SWidgetSwitcher::Slot()[Icon(TEXT("collapse"),Muted,15)]+SWidgetSwitcher::Slot()[Icon(TEXT("expand"),Muted,15)]]
                +SHorizontalBox::Slot().FillWidth(1).Padding(8,0).VAlign(VAlign_Center)
                    [SNew(STextBlock).Text(FText::FromString(TEXT("Collapse"))).Font(Font(9)).ColorAndOpacity(Muted)
                        .Visibility_Lambda([this]{return M->bSidebarCollapsed?EVisibility::Collapsed:EVisibility::Visible;})]]]]];
}

/*
THESIS: Project files and recorded results stay reachable without losing the working view.
OWN-WORLD: Extend the reference's blue-black Slate panels, compact type and cyan selection.
STORY: Find a local project, inspect its state, save a copy, then return to the same Solve view.
FIRST VIEWPORT: Existing header and rail surround a file list with search/actions; current project details occupy the right column. Dashboard puts current playback above recordings and recent files.
FORM: Native workspace extension; pinned reference and approved plan determine the composition.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
TSharedRef<SWidget> SStudioWorkspace::Projects()
{
    auto Details=SNew(SVerticalBox);
    Details->AddSlot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("Active project"),13,Text,true)];
    Details->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Project name"),10,Muted)];
    Details->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SEditableTextBox).Style(&InputStyle()).Font(Font(11))
        .Text_Lambda([this]{return FText::FromString(M->Project.Name);})
        .OnTextCommitted_Lambda([this](const FText& Value,ETextCommit::Type){M->RenameProject(Value.ToString());})];
    Details->AddSlot().AutoHeight().Padding(0,0,0,12)[Check(TEXT("Favorite project"),[this]{return M->Project.bFavorite;},
        [this](bool Value){M->SetProjectFavorite(M->ProjectPath,Value);})];
    Details->AddSlot().AutoHeight().Padding(0,0,0,10)[Live([this]{return M->ProjectPath.IsEmpty()?TEXT("Choose a file location with Save As."):M->ProjectPath;},9,Muted,true)];
    Details->AddSlot().AutoHeight().Padding(0,0,0,14)[SNew(STextBlock).Font(Font(10))
        .Text_Lambda([this]{return FText::FromString(M->bDirty?TEXT("Unsaved changes"):TEXT("Saved"));})
        .ColorAndOpacity_Lambda([this]{return M->bDirty?Amber:Muted;})];
    Details->AddSlot().AutoHeight().Padding(0,0,0,6)[Button(TEXT("Save project"),TEXT("save"),[this]{Execute(ECommand::Save);})];
    Details->AddSlot().AutoHeight().Padding(0,0,0,6)[Button(TEXT("Save As…"),TEXT("save"),[this]{Execute(ECommand::SaveAs);})];
    Details->AddSlot().AutoHeight().Padding(0,0,0,6)[Button(TEXT("Duplicate project…"),TEXT("folder"),[this]{Execute(ECommand::DuplicateProject);})];
    Details->AddSlot().AutoHeight().Padding(0,0,0,6)[Button(TEXT("Open Fields"),TEXT("run"),[this]{Navigate(EStudioWorkspace::Solve);},Cyan)];
    Details->AddSlot().AutoHeight().Padding(0,0,0,6)[SNew(SStudioMenuButton).ButtonStyle(&ButtonStyle())
        .OnGetMenuContent(this,&SStudioWorkspace::AssetMenu)
        .ToolTipText(FText::FromString(TEXT("Check geometry source files and locate missing originals, including older runs")))
        .ButtonContent()[Label(TEXT("Geometry files…"))]];
    Details->AddSlot().AutoHeight()[Section(TEXT("Project contents"),SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Saved cameras"),Live([this]{return FString::FromInt(M->Project.Cameras.Num());}),75)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Recordings / runs"),Live([this]{return FString::FromInt(M->Project.Runs.Num());}),75)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Geometry assets"),Live([this]{return FString::FromInt(M->Project.Draft.Geometry.Num());}),75)]
        +SVerticalBox::Slot().AutoHeight()[Live([this]{return TEXT("Case: ")+M->Project.Draft.Name;},10,Muted,true)])];
    return SNew(SBorder).BorderImage(&PanelBrush).Padding(20,18)
    [SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,18)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Projects"),18,Text,true)]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[Button(TEXT("New project…"),TEXT("plus"),[this]{Execute(ECommand::NewProject);})]
            +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Open project…"),TEXT("folder"),[this]{Execute(ECommand::OpenProject);})]]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,22,0)[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SAssignNew(ProjectSearchBox,SProjectFilterBox).Style(&InputStyle()).Font(Font(11))
                    .ForegroundColor_Lambda([this]{return ProjectFilter.IsEmpty()?Muted:Text;})
                    .HintText(FText::FromString(TEXT("Filter recent projects by name or path")))
                    .OnTextChanged_Lambda([this](const FText& Value){ProjectFilter=Value.ToString();bProjectListsDirty=true;})]
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Check(TEXT("Favorites only"),[this]{return bFavoritesOnly;},[this](bool V){bFavoritesOnly=V;bProjectListsDirty=true;})]
                    +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Refresh"),TEXT("orbit"),[this]{M->RefreshProjectCatalog();})]]
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]{return M->bCatalogLoading?TEXT("Reading project files…"):TEXT("Recent files");},9,Muted)]
                +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12.f)
                    +SScrollBox::Slot()[SAssignNew(RecentProjectRows,SVerticalBox)]
                    +SScrollBox::Slot()[SNew(SStudioHome4Lineage).Model(M)
                        .Telemetry([this]{return Home4Monitors?Home4Monitors->DisplayedTelemetry():nullptr;})
                        .Evidence([this]{return Home4Validation->Evidence;})
                        .OnFields_Lambda([this]{Navigate(EStudioWorkspace::Solve);})]]]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(280)
                [SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12.f)
                    +SScrollBox::Slot()[Details]]]]];
}
TSharedRef<SWidget> SStudioWorkspace::Dashboard()
{
    return SNew(SBorder).BorderImage(&PanelBrush).Padding(20,18)
    [SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12.f)
    +SScrollBox::Slot()[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,18)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Dashboard"),18,Text,true)]
            +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Open Fields"),TEXT("run"),[this]{Navigate(EStudioWorkspace::Solve);},Cyan)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]{return M->Project.Name+(M->bDirty?TEXT(" · Unsaved changes"):TEXT(""));},14,Text)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,18)[Live([this]{return M->ProjectPath.IsEmpty()?TEXT("This project has not been saved to a file."):M->ProjectPath;},9,Muted,true)]
        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,28,0)[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Recording playback"),Live([this]{return M->StatusText();},10,Green))]
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Selected source frame"),Live([this]{return FString::Printf(TEXT("%d / %d"),M->SelectedFrame,FMath::Max(0,M->Frames.Num()-1));}))]
                +SVerticalBox::Slot().AutoHeight()[Row(TEXT("Source elapsed time"),Live([this]{return FString::Printf(TEXT("%.4f s"),M->DisplayFrame().Time);}))]]
            +SHorizontalBox::Slot().FillWidth(1)[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Saved camera views"),Live([this]{return FString::FromInt(M->Project.Cameras.Num());}))]
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Custom solver"),Label(TEXT("Not connected"),10,Muted))]
                +SVerticalBox::Slot().AutoHeight()[Live([]{return TEXT("Playback continues while browsing. Returning to Fields keeps your camera and review frame.");},10,Muted,true)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,22,0,8)[Label(TEXT("Latest science evidence"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight()[Live([this]
        {
            const auto T=Home4Monitors?Home4Monitors->DisplayedTelemetry():nullptr;
            if(!T||T->History().IsEmpty())return FString(TEXT("No solver measurements imported. Open Ledgers & Monitors to attach original JSONL."));
            const auto& V=T->History().Last();
            FString Summary=TEXT("REPLAY · ")+V.Source.SourceId+TEXT(" · run ")+V.Source.RunId.ToString();
            for(const auto& Health:FStudioHome4Diagnostics::Evaluate(V,Home4Monitors->DiagnosticPolicy()))
                Summary+=TEXT("\n")+Health.Label+TEXT(": ")+Health.Reason;
            if(!T->ActionRequests().IsEmpty())Summary+=TEXT("\nLatest recorded failure: ")+T->ActionRequests().Last().Reason;
            return Summary;
        },10,Muted,true)]
        +SVerticalBox::Slot().AutoHeight()[Button(TEXT("Inspect ledgers and failure location"),TEXT("probe"),[this]{Navigate(EStudioWorkspace::Monitors);})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,22,0,8)[Label(TEXT("Recordings and runs"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight()[Live([this]{return FString::Printf(TEXT("%d saved records. Inspect their provenance in Projects."),M->Project.Runs.Num());},10,Muted,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,22,0,8)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Recent projects"),12,Text,true)]
            +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("All projects"),TEXT("folder"),[this]{Navigate(EStudioWorkspace::Projects);})]]
        +SVerticalBox::Slot().AutoHeight()[SAssignNew(DashboardProjectRows,SVerticalBox)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,22,0,8)[Label(TEXT("Recent activity"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight()[Live([this]{return LogState->RecentActivity;},10,Muted,true)]]];
}
TSharedRef<SWidget> SStudioWorkspace::RecentProjectRow(const FStudioProjectSummary& Item,bool bCompact)
{
    const bool bActive=Item.Path==M->ProjectPath;
    const FString Path=Item.Path, Name=bActive?M->Project.Name:Item.Name;
    const bool bFavorite=bActive?M->Project.bFavorite:Item.bFavorite;
    const FString FocusPrefix=(bCompact?TEXT("dashboard/"):TEXT("projects/"))+Path;
    auto Actions=SNew(SHorizontalBox);
    if(!bCompact)
    {
        auto Favorite=Button(bFavorite?TEXT("Unfavorite"):TEXT("Favorite"),TEXT("star"),[this,Path,bFavorite]{M->SetProjectFavorite(Path,!bFavorite);},bFavorite?Amber:Muted);
        Favorite->SetEnabled(Item.Error.IsEmpty()||bActive);
        ProjectActionTargets.Add(FocusPrefix+TEXT("/favorite"),Favorite);
        Actions->AddSlot().AutoWidth().Padding(0,0,6,0)[Favorite];
    }
    auto Open=Button(bActive?TEXT("Open Fields"):Item.Error.IsEmpty()?TEXT("Open"):TEXT("Locate…"),TEXT("folder"),[this,Path,bActive,Missing=!Item.Error.IsEmpty()]{
        if(bActive) Navigate(EStudioWorkspace::Solve); else if(Missing) LocateProject(Path); else OpenProject(Path);});
    ProjectActionTargets.Add(FocusPrefix+TEXT("/open"),Open);
    Actions->AddSlot().AutoWidth().Padding(0,0,6,0)[Open];
    if(!bCompact)
    {
        auto Remove=Button(TEXT("Remove"),TEXT("stop"),[this,Path]{M->ForgetRecentProject(Path);});
        Remove->SetToolTipText(FText::FromString(TEXT("Remove from recent projects. Keeps the file on disk.")));
        ProjectActionTargets.Add(FocusPrefix+TEXT("/remove"),Remove);
        Actions->AddSlot().AutoWidth()[Remove];
    }
    auto Info=SNew(SVerticalBox);
    Info->AddSlot().AutoHeight().Padding(0,0,0,5)[SNew(STextBlock).Text(FText::FromString(Name)).Font(Font(11,true)).ColorAndOpacity(bActive?Cyan:Text)
        .OverflowPolicy(ETextOverflowPolicy::Ellipsis).ToolTipText(FText::FromString(Name))];
    Info->AddSlot().AutoHeight().Padding(0,0,0,5)[SNew(STextBlock).Text(FText::FromString(Path)).Font(Font(9)).ColorAndOpacity(Muted)
        .OverflowPolicy(ETextOverflowPolicy::Ellipsis).ToolTipText(FText::FromString(Path))];
    FString Detail;
    if(!Item.Error.IsEmpty()) Detail=bActive?TEXT("Active file unavailable. Use Save or Save As to keep this project."):TEXT("File unavailable — locate it or remove this entry.");
    else Detail=(bActive?TEXT("Active project · "):TEXT(""))+FString::Printf(TEXT("Camera views: %d · Recordings / runs: %d"),bActive?M->Project.Cameras.Num():Item.CameraCount,bActive?M->Project.Runs.Num():Item.RunCount);
    Info->AddSlot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Detail)).Font(Font(9)).ColorAndOpacity(Item.Error.IsEmpty()?Muted:Amber)
        .AutoWrapText(true).ToolTipText(FText::FromString(Item.Error))];
    return SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(1)[SNew(SBorder).BorderImage(&LineBrush)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,12)[Info]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Actions];
}
void SStudioWorkspace::RefreshProjectLists()
{
    if(!RecentProjectRows||!DashboardProjectRows) return;
    FString FocusKey;
    const auto Focused=FSlateApplication::Get().GetKeyboardFocusedWidget();
    for(const auto& Target:ProjectActionTargets) if(Target.Value==Focused) { FocusKey=Target.Key; break; }
    ProjectActionTargets.Reset();
    RecentProjectRows->ClearChildren(); DashboardProjectRows->ClearChildren();
    int32 Shown=0,Recent=0;
    for(const auto& Item:M->ProjectCatalog)
    {
        const bool bActive=Item.Path==M->ProjectPath;
        const FString Name=bActive?M->Project.Name:Item.Name;
        if((ProjectFilter.IsEmpty()||Name.Contains(ProjectFilter)||Item.Path.Contains(ProjectFilter)) &&
            (!bFavoritesOnly||(bActive?M->Project.bFavorite:Item.bFavorite)))
        { RecentProjectRows->AddSlot().AutoHeight()[RecentProjectRow(Item,false)]; ++Shown; }
        if(Recent++<3) DashboardProjectRows->AddSlot().AutoHeight()[RecentProjectRow(Item,true)];
    }
    if(!Shown) RecentProjectRows->AddSlot().AutoHeight().Padding(0,18)
        [Label(M->bCatalogLoading?TEXT("Loading recent projects…"):M->ProjectCatalog.IsEmpty()?TEXT("Open or save a project to add it here."):TEXT("No projects match these filters."),10,Muted)];
    if(!Recent) DashboardProjectRows->AddSlot().AutoHeight().Padding(0,10)[Label(TEXT("Open or save a project to build your recent list."),10,Muted)];
    LastCatalogRevision=M->CatalogRevision; DashboardProjectId=M->Project.Id; DashboardRunCount=M->Project.Runs.Num(); bProjectListsDirty=false;
    if(!FocusKey.IsEmpty())
    {
        if(const auto* Target=ProjectActionTargets.Find(FocusKey)) FSlateApplication::Get().SetKeyboardFocus(*Target,EFocusCause::Navigation);
        else if(M->Workspace==EStudioWorkspace::Projects) FSlateApplication::Get().SetKeyboardFocus(ProjectSearchBox,EFocusCause::Navigation);
    }
}
void SStudioWorkspace::Tick(const FGeometry& Geometry,double Time,float Delta)
{
    const double UpdateStart=FPlatformTime::Seconds();
    SCompoundWidget::Tick(Geometry,Time,Delta);
    if(PerformanceProject!=M->Project.Id)
    {PerformanceProject=M->Project.Id;bPerformanceOpen=bPerformanceFocusPending=false;PerformancePanel->ResumeReadings();}
    if(bPerformanceFocusPending&&bPerformanceOpen&&PerformancePanel->GetCachedGeometry().GetLocalSize().X>0)
    {
        FSlateApplication::Get().SetKeyboardFocus(PerformancePanel,EFocusCause::Navigation);
        bPerformanceFocusPending=!PerformancePanel->HasKeyboardFocus();
    }
    if(Pipelines)Pipelines->Synchronize();
    TickInspection();
    FieldExport->Tick(*M);
    SnapshotUI->Tick(*M);
    if(LastAssetRevision!=M->AssetRevision) RefreshAssetRows();
    if(LastCameraCollectionRevision!=M->CameraCollectionRevision||CameraRowsProjectId!=M->Project.Id) RefreshCameraRows();
    RefreshGeometryEditor();
    if(LastGeometryRevision!=M->GeometryRevision||GeometryProjectId!=M->Project.Id||GeometryCaseRevision!=M->Project.Draft.Revision)RefreshGeometryObjects();
    RefreshMaterials();
    RefreshDomain();
    RefreshBoundaries();
    RefreshLattice();
    RefreshRunSettings();
    if(FlowConditions)FlowConditions->Refresh();
    RefreshMonitors();
    RefreshActivityLog();
    if(DisplayMenuProject!=M->Project.Id||DisplayMenuSource.Pin()!=M->Solver||DisplayMenuScalar!=M->ActiveScalar().Id)
        DisplayMenuDrafts.Empty(); // Release prior recording closures even if no editor is reopened.
    if(bProjectListsDirty || LastCatalogRevision!=M->CatalogRevision || DashboardProjectId!=M->Project.Id || DashboardRunCount!=M->Project.Runs.Num()) RefreshProjectLists();
    const double UpdateEnd=FPlatformTime::Seconds();
    PerformanceHistory->Observe(UpdateEnd,(UpdateEnd-UpdateStart)*1000.,[this]{return StudioPerformance::ReadCounters(Scene.Get());});
}
void SStudioWorkspace::OpenPerformance(bool bOpen)
{
    bPerformanceOpen=bOpen;bPerformanceFocusPending=bOpen;
    if(bOpen)
    {
        bInspectionOpen=false;
    }
    else if(const auto Button=PerformanceButton.Pin())FSlateApplication::Get().SetKeyboardFocus(Button,EFocusCause::Navigation);
}
void SStudioWorkspace::Navigate(EStudioWorkspace Destination)
{
    if(Destination!=EStudioWorkspace::Validation)bNewHome4Project=false;
    if(Pipelines)Pipelines->SaveCamera();
    if(M->Navigate(Destination))
    {
        bProjectListsDirty=true;
        if(const auto* Target=NavigationButtons.Find(Destination)) FSlateApplication::Get().SetKeyboardFocus(*Target,EFocusCause::Navigation);
    }
}
void SStudioWorkspace::LocateProject(const FString& OldPath)
{
    FString Path;
    if(StudioFileDialog::Project(false,OldPath,TEXT(""),Path)) OpenProject(Path,OldPath);
}
TSharedRef<SWidget> SStudioWorkspace::Center()
{
    auto Floating=SNew(SStudioFloatingLayer).Model(M);
    Floating->AddPane(TEXT("Status"),TEXT("Playback"),SNew(SBorder).BorderImage(&PanelBrush).Padding(6,3)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth()[SNew(SBorder).BorderImage(&RaisedBrush).Padding(10,6)[Live([this]{return TEXT("Replay ")+M->StatusText().ToLower();},10,Green)]]
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(18,0)[Live([this]{return Scene->HasPresentedFrame()?FString::Printf(TEXT("Showing frame %d"),Scene->PresentedFrame().Index):TEXT("Loading frame…");},10)]
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,18,0)[Live([this]{if(!Scene->HasPresentedFrame())return FString();const auto Field=Scene->PresentedField();const auto V=Field?Field->VolumeReconstruction():nullptr;
                    if(V&&V->OriginalGrid){const auto Context=V->OriginalGrid->UnitContext();return StudioHome4Readouts::Time(Scene->PresentedFrame().Index,&Context,Scene->PresentedFrame().Time);}return FString::Printf(TEXT("%.4f s · source unit map not supplied"),Scene->PresentedFrame().Time);},10)]
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Cyan).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                    .Text_Lambda([this]{return FText::FromString((Scene->HasCurrentFrame()?FString():TEXT("Updating · "))+Scene->PresentedSource());})
                    .ToolTipText_Lambda([this]{return FText::FromString(Scene->PresentedSource());})]],FVector2D(0,0),FVector2D(4,4),true);
    Floating->AddPane(TEXT("Tools"),TEXT("Tools"),ViewTools(),FVector2D(0,0),FVector2D(4,70));
    Floating->AddPane(TEXT("Axes"),TEXT("Axes"),SNew(SOrientationCube).Scene(Scene.Get()).Tag(TEXT("OrientationCube")),FVector2D(1,0),FVector2D(-4,4));
    Floating->AddPane(TEXT("Legend"),TEXT("Color scale"),ColorLegend(),FVector2D(0,1),FVector2D(4,-6));
    Floating->AddPane(TEXT("View"),TEXT("View"),SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(SBorder).Tag(TEXT("InspectionPlacementHint")).Visibility_Lambda([this]{return IsInspectionPlacementCurrent()||bInspectionOpen||Scene->bFreeCamera||ViewportTool!=EStudioViewportTool::Orbit?EVisibility::Visible:EVisibility::Collapsed;}).BorderImage(&PanelBrush).Padding(10,7)[Live([this]() -> FString
            {if(IsInspectionPlacementCurrent())return FString::Printf(TEXT("PLACE  ·  Click %c  ·  Right-drag to look  ·  Esc cancels"),TEXT("ABC")[InspectionPlacement->Accepted.Num()]);
                if(bInspectionOpen&&ViewportTool==EStudioViewportTool::Orbit)return FString(TEXT("INSPECT  ·  Click a marker or edge  ·  Right-drag to look"));
                if(Scene->bFreeCamera)return TEXT("FLY  ·  Drag to look  ·  WASD + Q/E  ·  Shift boost");
                return ViewportTool==EStudioViewportTool::Pan?TEXT("PAN  ·  Drag to move  ·  Right-drag to look"):
                    ViewportTool==EStudioViewportTool::Zoom?TEXT("ZOOM  ·  Drag up to zoom in  ·  Down to zoom out"):
                    TEXT("ORBIT  ·  Drag to rotate  ·  Middle-drag pan  ·  Scroll zoom  ·  F fit");},9,Muted)]]
        +SVerticalBox::Slot().AutoHeight()[ViewToolbar()],FVector2D(1,1),FVector2D(-4,-6));
    return SNew(SVerticalBox)
    +SVerticalBox::Slot().FillHeight(1)
    [SNew(SBorder).BorderImage(&LineBrush).Padding(1)
        [SNew(SOverlay)
            +SOverlay::Slot()[SNew(SFlowViewport).Tag(TEXT("FlowViewport")).Scene(Scene.Get())
                .InspectionMarkers_Lambda([this]{return InspectionMarkers->Result();})
                .InspectionPlacement_Lambda([this]{return IsInspectionPlacementCurrent()?&InspectionPlacement.GetValue():nullptr;})
                .InspectionPlacementRevision_Lambda([this]{return InspectionPlacementRevision;})
                .InspectionActive_Lambda([this]{return bInspectionOpen;})
                .NavigationTool_Lambda([this]{return Scene->bFreeCamera?EStudioViewportTool::Fly:ViewportTool;})
                .SnapshotSize_Lambda([this]{return SnapshotButton&&SnapshotButton->IsOpen()&&!SnapshotUI->IsBusy()?SnapshotOutputSize():FIntPoint::ZeroValue;})
                .InspectionHover(this,&SStudioWorkspace::InspectionHover)
                .InspectionClick(this,&SStudioWorkspace::InspectionClick).CancelInspection(this,&SStudioWorkspace::CancelInspectionPlacement)]
            +SOverlay::Slot()[Floating]
        ]]
    +SVerticalBox::Slot().AutoHeight().Padding(0,6)[Timeline()]
    +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,3)[SNew(SBox).Visibility_Lambda([this]{return M->bViewportExpanded?EVisibility::Collapsed:EVisibility::Visible;})
        .HeightOverride_Lambda([this]{return GetCachedGeometry().GetLocalSize().Y<820?(M->ResidualHistory()?225.f:195.f):239.f;})[Monitors()]];
}
TSharedRef<SWidget> SStudioWorkspace::ViewTools()
{
    auto Items=SNew(SVerticalBox);auto Modes=SNew(SWrapBox).PreferredSize(48).InnerSlotPadding(FVector2D(0,3));
    auto Add=[&](const FString& Name,const FString& I,EStudioViewportTool Tool,const FString& Help)
    {
        const auto Active=[this,Tool]{return Scene->bFreeCamera?Tool==EStudioViewportTool::Fly:Tool==ViewportTool;};
        Modes->AddSlot()[SNew(SBox).WidthOverride_Lambda([this]{return GetCachedGeometry().GetLocalSize().Y<820?24.f:48.f;})
            [SNew(SButton).Tag(FName(*(TEXT("Tool")+Name))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(2,4))
                .ButtonColorAndOpacity_Lambda([Active]{return Active()?FLinearColor(.15,.48,.65):FLinearColor::White;})
                .ToolTipText(FText::FromString(Name+TEXT(" · ")+Help))
                .OnClicked_Lambda([this,Tool]
                {
                    ViewportTool=Tool==EStudioViewportTool::Fly?EStudioViewportTool::Orbit:Tool;
                    if(Scene->bFreeCamera!=(Tool==EStudioViewportTool::Fly))Scene->SetCameraMode(Tool==EStudioViewportTool::Fly);
                    return FReply::Handled();
                })
                [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)[SNew(SIcon).Name(I).Size(16).Color_Lambda([Active]{return Active()?Cyan:Muted;})]
                    +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0,4,0,0)
                    [SNew(SBox).Visibility_Lambda([this]{return GetCachedGeometry().GetLocalSize().Y<820?EVisibility::Collapsed:EVisibility::Visible;})
                        [SNew(STextBlock).Text(FText::FromString(Name)).Font(Font(8)).ColorAndOpacity_Lambda([Active]{return Active()?Cyan:Muted;})]]]]];
    };
    Add(TEXT("Orbit"),TEXT("orbit"),EStudioViewportTool::Orbit,TEXT("Drag to rotate around the focus"));
    Add(TEXT("Pan"),TEXT("pan"),EStudioViewportTool::Pan,TEXT("Drag to move the view without rotating"));
    Add(TEXT("Zoom"),TEXT("zoom"),EStudioViewportTool::Zoom,TEXT("Drag up to zoom in; down to zoom out"));
    Add(TEXT("Fly"),TEXT("fly"),EStudioViewportTool::Fly,TEXT("Drag to look; WASD + Q/E to move"));
    Items->AddSlot().AutoHeight().Padding(0,0,0,3)[Modes];
    Items->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("InspectionTools")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(4,4))
        .ToolTipText(FText::FromString(TEXT("Slices, probes and measurements")))
        .OnClicked_Lambda([this]{bInspectionOpen=!bInspectionOpen;if(bInspectionOpen)ViewportTool=EStudioViewportTool::Orbit;else CancelInspectionPlacement();return FReply::Handled();})
        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)[Icon(TEXT("select"),Muted,16)]
        +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0,4,0,0)[SNew(SBox).Visibility_Lambda([this]{return GetCachedGeometry().GetLocalSize().Y<820?EVisibility::Collapsed:EVisibility::Visible;})[Label(TEXT("Inspect"),8,Muted)]]]];
    Items->AddSlot().AutoHeight()[SNew(SStudioMenuButton).Tag(TEXT("CameraManager")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(4,4))
        .HasDownArrow(false).ToolTipText(FText::FromString(TEXT("Manage saved camera views")))
        .OnGetMenuContent(this,&SStudioWorkspace::CameraMenu)
        .ButtonContent()[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)[Icon(TEXT("camera"),Muted,16)]
            +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0,4,0,0)[SNew(SBox).Visibility_Lambda([this]{return GetCachedGeometry().GetLocalSize().Y<820?EVisibility::Collapsed:EVisibility::Visible;})[Label(TEXT("Camera"),8,Muted)]]]];
    return SNew(SBox).WidthOverride(54)[SNew(SBorder).BorderImage(&PanelBrush).Padding(3)[Items]];
}
/*
THESIS: One compact owner for view direction, projection, topology and framing.
OWN-WORLD: Inherit the pinned dense blue-black Slate workspace and cyan selection.
STORY: Inspect the recorded mesh, change the camera, expand the view, restore it.
FIRST VIEWPORT: Bottom toolbar below the input hint; sidebar remains the sole workspace navigator.
FORM: Local Operate extension; move existing actions, preserve shared view history and source identity.
FINISH: unreviewed and undocumented is unfinished; finish with review, verdict and DESIGN.md.
*/
TSharedRef<SWidget> SStudioWorkspace::ViewToolbar()
{
    return SNew(SBorder).Tag(TEXT("ViewToolbar")).BorderImage(&PanelBrush).Padding(5)
    [SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,4,0)[SNew(SButton).Tag(TEXT("FlowOverview"))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,4))
            .IsEnabled_Lambda([this]{return Scene->HasCurrentFrame()&&!M->CameraPlacement()&&!IsInspectionPlacementCurrent();})
            .ToolTipText(FText::FromString(TEXT("Frame the wing and wake, simplify layers, and freeze a custom color range sampled from this frame. Undo view restores every prior setting.")))
            .OnClicked_Lambda([this]{Scene->FlowOverview();return FReply::Handled();})[Label(TEXT("Flow overview"),9,Cyan)]]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,4,0)[OrientationViews(Scene.Get())]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,4,0)[SNew(SButton).Tag(TEXT("ViewProjection"))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,4))
            .ToolTipText(FText::FromString(TEXT("Switch perspective / orthographic projection. Camera position and focus are retained.")))
            .OnClicked_Lambda([this]{auto C=Scene->SavedCameraState();C.bOrthographic=!C.bOrthographic;Scene->RestoreCamera(C,TEXT("Camera projection"));return FReply::Handled();})
            [Live([this]{return Scene->CameraState().bOrthographic?TEXT("Orthographic"):TEXT("Perspective");},9)]]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,4,0)[SNew(SStudioMenuButton).Tag(TEXT("ViewMesh"))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,4)).ToolTipText(FText::FromString(TEXT("Actual triangle mesh display")))
            .OnGetMenuContent(this,&SStudioWorkspace::MeshMenu)
            .ButtonContent()[Live([this]{if(M->MeshStyle>0&&Scene->HasCurrentFrame()&&!Scene->PresentedMesh().Triangles)return TEXT("No mesh");
                return M->MeshStyle==2?TEXT("Wireframe"):M->MeshStyle==1?TEXT("Mesh overlay"):TEXT("Field");},9)]]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,4,0)[SNew(SButton).Tag(TEXT("ViewFit"))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,4)).ToolTipText(FText::FromString(TEXT("Fit the displayed flow region · F")))
            .OnClicked_Lambda([this]{Scene->FitCamera();return FReply::Handled();})[Icon(TEXT("fit"),Text,16)]]
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,4,0)[SNew(SButton).Tag(TEXT("ViewExpand"))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,4))
            .ToolTipText_Lambda([this]{return FText::FromString(M->bViewportExpanded?TEXT("Restore monitors and inspector"):TEXT("Expand the flow viewport; active inspection tools remain available"));})
            .OnClicked_Lambda([this]{M->bViewportExpanded=!M->bViewportExpanded;M->SaveSession();return FReply::Handled();})
            [Live([this]{return M->bViewportExpanded?TEXT("Restore"):TEXT("Expand");},9)]]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SStudioMenuButton).Tag(TEXT("ViewportSettings"))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,4)).HasDownArrow(false)
            .ToolTipText(FText::FromString(TEXT("Camera transform, lens and depth clipping")))
            .OnGetMenuContent(this,&SStudioWorkspace::ViewportMenu).ButtonContent()[Icon(TEXT("settings"),Text,16)]]];
}
TSharedRef<SWidget> SStudioWorkspace::MeshMenu()
{
    const FGuid Project=M->Project.Id;const auto Solver=M->Solver;
    auto Items=SNew(SVerticalBox);
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("Triangle mesh"),12,Text,true)];
    const TCHAR* Names[]={TEXT("Field display"),TEXT("Mesh edges over field"),TEXT("Mesh edges only")};
    for(int32 Mode=0;Mode<3;++Mode)
        Items->AddSlot().AutoHeight().Padding(0,0,0,5)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("MeshStyle%d"),Mode)))
            .ButtonStyle(&ButtonStyle()).ContentPadding(8)
            .IsEnabled_Lambda([this,Project,Solver,Mode]
            {const auto Field=Scene->PresentedField();return M->Project.Id==Project&&M->Solver==Solver&&
                (Mode==0||(Scene->HasCurrentFrame()&&Field&&Field->MeshTriangleCount()>0&&Field->MeshTriangleCount()<=StudioMeshDisplay::MaximumTriangles));})
            .OnClicked_Lambda([this,Project,Solver,Mode]
            {if(M->Project.Id==Project&&M->Solver==Solver)M->EditView(TEXT("Triangle mesh display"),[Mode](auto& S){S.Display.MeshStyle=Mode;});return FReply::Handled();})
            [Live([this,Mode,Name=FString(Names[Mode])]{return Name+(M->MeshStyle==Mode?
                (Mode>0&&Scene->HasCurrentFrame()&&!Scene->PresentedMesh().Triangles?TEXT(" · unavailable"):TEXT(" · selected")):TEXT(""));},10)]];
    Items->AddSlot().AutoHeight().Padding(0,7,0,0)[Live([this]() -> FString
    {
        if(!Scene->HasCurrentFrame())return TEXT("Updating the displayed field…");
        const auto Field=Scene->PresentedField();
        if(!Field||!Field->MeshTriangleCount())return TEXT("No triangle mesh supplied; field display retained. Attach a verified surface reconstruction to inspect its derived edges.");
        if(Field->MeshTriangleCount()>StudioMeshDisplay::MaximumTriangles)return TEXT("This mesh exceeds the display budget (131,072 faces). No partial mesh is shown.");
        return FString::Printf(TEXT("%s · %s triangles\nEdges follow exact node positions. Streamlines, vectors and inspection objects keep their own visibility."),
            Field->Reconstruction()?TEXT("Derived topology"):TEXT("Original CFD topology"),*FText::AsNumber(Field->MeshTriangleCount()).ToString());
    },9,Muted,true)];
    Items->AddSlot().AutoHeight().Padding(0,10,0,0)[ViewHistoryControls()];
    return SNew(SBox).WidthOverride(322)[SNew(SBorder).BorderImage(&PanelBrush).Padding(14)[Items]];
}
/*
THESIS: Inspect original fields as points or through explicitly attached reconstruction.
OWN-WORLD: Preserve the compact blue-black Slate Solve workspace and cyan controls.
STORY: Choose a scalar, select derived surface or original points, retain the view.
FIRST VIEWPORT: Field choice leads the 180-unit Display panel; source limits stay visible.
FORM: Local Operate extension; source identity and legend follow the presented frame.
FINISH: Packaged controls, both window sizes, scoped finish review and documentation.
*/
TSharedRef<SWidget> SStudioWorkspace::DisplayTools()
{
    auto Axis=SNew(SHorizontalBox);
    for(int I=0;I<3;++I) Axis->AddSlot().FillWidth(1).Padding(2,0)
    [SNew(SButton).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,4)).OnClicked_Lambda([this,I]{M->EditView(TEXT("Slice normal"),[this,I](auto& S){S.Display.SliceAxis=I;S.Display.SlicePosition=M->Solver->Descriptor().DisplayBounds.GetCenter()[I];});return FReply::Handled();})
        [SNew(STextBlock).Text(FText::FromString(I==0?TEXT("X"):I==1?TEXT("Y"):TEXT("Z"))).Font(Font(9)).ColorAndOpacity_Lambda([this,I]{return M->SliceAxis==I?Cyan:Muted;})]];
    auto Points=ViewCheck(M,TEXT("Source points"),[this]{return M->bSourcePoints;},[](auto& S,bool V){S.bSourcePoints=V;});Points->SetTag(TEXT("SourcePoints"));
    Points->SetToolTipText(FText::FromString(TEXT("Shows original rows with a deterministic stride when there are more than 50,000 points. CSV export retains every source row.")));
    auto Volume=ViewCheck(M,TEXT("Volume"),[this]{return M->bVolume;},[](auto& S,bool V){S.bVolume=V;});Volume->SetTag(TEXT("VolumeDisplay"));
    auto Surface=ViewCheck(M,TEXT("Reconstructed surface"),[this]{return M->bReconstructedSurface;},[](auto& S,bool V){S.bReconstructedSurface=V;});
    Surface->SetTag(TEXT("ReconstructedSurface"));
    auto Focus=ViewCheck(M,TEXT("Focus wing region"),[this]{return M->bFocusWingRegion;},[](auto& S,bool V){S.bFocusWingRegion=V;});
    Focus->SetTag(TEXT("FocusWingRegion"));
    Focus->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->bReconstructedSurface&&M->MeshStyle==0;}));
    Focus->SetToolTipText(FText::FromString(TEXT("Crop the reconstructed field around its inferred wing. Fit frames the region. Turn off for full coverage. Original points and mesh views always show the full field; raw export keeps every row.")));

    Surface->SetToolTipText(FText::FromString(TEXT("Interpolate recorded scalars on the attached derived triangles. Turn off to inspect the original source points. The source remains two-dimensional.")));
    return SNew(SBox).Tag(TEXT("DisplayInspector"))[SNew(SBorder).BorderImage(&PanelBrush).Padding(2,10)
    [SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[Label(TEXT("Recorded field display"),10,Text,true)]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("ResetViewportPanes")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(5,2))
                .ToolTipText(FText::FromString(TEXT("Restore the default positions and open every viewport panel. The camera and data stay unchanged.")))
                .OnClicked_Lambda([this]{M->FloatingPanes.Reset();M->SaveSession();return FReply::Handled();})[Label(TEXT("Reset panels"),8,Muted)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[SNew(SStudioMenuButton).Tag(TEXT("ScalarSelector")).ButtonStyle(&ButtonStyle())
                .IsEnabled_Lambda([this]{return M->Solver->Descriptor().Scalars.Num()>1;})
                .ToolTipText_Lambda([this]{return FText::FromString(M->ActiveScalar().Label+TEXT(" · ")+M->ActiveScalar().Unit);})
                .OnGetMenuContent(this,&SStudioWorkspace::ScalarMenu).ButtonContent()[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text)
                    .OverflowPolicy(ETextOverflowPolicy::Ellipsis).Text_Lambda([this]{return FText::FromString(M->ActiveScalar().Label);})]]
            +SHorizontalBox::Slot().AutoWidth().Padding(4,0,0,0)[SNew(SStudioMenuButton).Tag(TEXT("ColorSettings")).ButtonStyle(&ButtonStyle())
                .ContentPadding(FMargin(5,3)).HasDownArrow(false).ToolTipText(FText::FromString(TEXT("Color settings: palette and range")))
                .OnGetMenuContent_Lambda([this]{return CachedDisplayMenu(TEXT("Color"),[this]{return ColorMenu();});}).ButtonContent()[Icon(TEXT("settings"),Muted,14)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[ViewCheck(M,TEXT("Domain grid"),[this]{return M->bMesh;},[](auto& S,bool V){S.bMesh=V;})]
        +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([this]{return M->Solver->Descriptor().bSourcePoints?EVisibility::Visible:EVisibility::Collapsed;})
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SBox).Visibility_Lambda([this]{return M->Solver->VolumeReconstruction()?EVisibility::Visible:EVisibility::Collapsed;})
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[Volume]
                +SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[SNew(SStudioMenuButton).Tag(TEXT("VolumeSettings")).ButtonStyle(&ButtonStyle())
                    .OnGetMenuContent_Lambda([this]{return CachedDisplayMenu(TEXT("Volume"),[this]{return VolumeMenu();});}).ButtonContent()[Label(TEXT("Volume settings…"),9,Cyan)]]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SBox).Visibility_Lambda([this]{return M->Solver->Reconstruction()?EVisibility::Visible:EVisibility::Collapsed;})[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[Surface]
                +SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[Focus]]]
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([this]{return (M->bReconstructedSurface&&M->Solver->Reconstruction())||((M->bVolume||M->bVolumeIsosurface)&&M->Solver->VolumeReconstruction())?EVisibility::Collapsed:EVisibility::Visible;})
            [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Points]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Live([this]{return FString::Printf(TEXT("Point size  %.2g×"),M->PointSize);},9,Muted)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[ViewSlider(M,TEXT("Point size"),[this]{return (M->PointSize-.25)/2.75;},[](auto& S,double V){S.PointSize=.25+2.75*V;})]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[StreamlineControls()]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[VectorControls(true)]
            +SVerticalBox::Slot().AutoHeight()[Live([this]
            {
                if(M->bFocusWingRegion&&M->bReconstructedSurface&&M->MeshStyle==0&&M->Solver->Reconstruction())return TEXT("Focused 2D field · inferred wing extruded for viewing. No spanwise flow. Turn off Focus wing region for full coverage.");
                if(M->bStreamlines&&M->Solver->VolumeReconstruction())return M->Solver->VolumeReconstruction()->OriginalGrid?TEXT("Instantaneous streamlines sample original liquid cells, colored by speed."):TEXT("Instantaneous streamlines use derived 3D grid interpolation. Raw export retains every row.");
                if(M->bStreamlines&&M->Solver->Reconstruction())return TEXT("Instantaneous streamlines use derived 2D triangle interpolation. Raw export retains every row.");
                if(M->Solver->VolumeReconstruction())return (M->bVolume||M->bVolumeIsosurface)?TEXT("Derived 3D field. Original values retained; unsupported regions stay empty."):TEXT("Original 3D point subset. Volume is hidden; export retains every row.");if(M->Solver->Reconstruction()){return M->bReconstructedSurface?
                TEXT("Derived 2D surface. Colors interpolate recorded values; the inferred solid stays empty."):
                TEXT("Original points. The attached reconstruction is hidden.");}return M->Solver->Descriptor().bPointVelocity?
                TEXT("Original points and sampled vectors. No mesh supplied for slices or streamlines."):
                TEXT("Original points. Velocity vectors and a mesh for slices or streamlines are not supplied.");},9,Amber,true)]
        ]]
        +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([this]{return M->Solver->Descriptor().bSourcePoints?EVisibility::Collapsed:EVisibility::Visible;})
        [SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[ViewCheck(M,TEXT("Cut plane"),[this]{return M->bCutPlane;},[](auto& S,bool V){S.bCutPlane=V;})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Axis]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Live([this]{const auto& B=M->Solver->Descriptor().DisplayBounds;const bool Outside=M->SlicePosition<B.Min[M->SliceAxis]||M->SlicePosition>B.Max[M->SliceAxis];return Outside?FString::Printf(TEXT("%.3f m · outside bounds"),M->SlicePosition):FString::Printf(TEXT("Position    %.3f m"),M->SlicePosition);},9,Muted)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[ViewSlider(M,TEXT("Slice position"),[this]{const auto& B=M->Solver->Descriptor().DisplayBounds;return B.IsValid?FMath::Clamp((M->SlicePosition-B.Min[M->SliceAxis])/B.GetSize()[M->SliceAxis],0.,1.):.5;},[this](auto& S,double V){const auto& B=M->Solver->Descriptor().DisplayBounds;if(B.IsValid)S.SlicePosition=FMath::Lerp(B.Min[S.SliceAxis],B.Max[S.SliceAxis],V);})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[StreamlineControls()]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[VectorControls(false)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[ViewCheck(M,TEXT("Flow layers"),[this]{return M->bVolume;},[](auto& S,bool V){S.bVolume=V;})]
        +SVerticalBox::Slot().AutoHeight()[ViewSlider(M,TEXT("Flow opacity"),[this]{return M->VolumeOpacity;},[](auto& S,double V){S.VolumeOpacity=V;})]]]]];
}
/*
THESIS: Make vector density and length interpretable against the displayed CFD frame.
OWN-WORLD: Existing compact Slate Display panel, blue-black popover and cyan selection.
STORY: Choose a sample limit, proportional or equal lengths, and an exact scale; read the length key.
FIRST VIEWPORT: One settings action beside Vectors; the current frame's length key sits below it.
FORM: Local Operate extension; project/view history owns settings, captured geometry owns the key.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
FString SStudioWorkspace::VectorLegendText() const
{
    if(!M->bVectors)return TEXT("Vectors hidden");
    if(M->Solver->Descriptor().bSourcePoints&&!M->Solver->Descriptor().bPointVelocity)return TEXT("Velocity components not supplied");
    if(!Scene.IsValid()||!Scene->HasCurrentFrame())return TEXT("Updating vectors…");
    const auto& V=Scene->PresentedVectors();
    if(V.GlyphCount==0)return TEXT("No valid nonzero velocity samples");
    return V.bUniformLength?FString::Printf(TEXT("Arrow: %.4g m\nEqual length · direction only"),V.ReferenceLengthMeters):
        FString::Printf(TEXT("Arrow: %.4g m\n= %.4g m/s (sample max)"),V.ReferenceLengthMeters,V.MaximumSpeed);
}
TSharedRef<SWidget> SStudioWorkspace::VectorControls(bool bOriginalPoints)
{
    auto Toggle=ViewCheck(M,TEXT("Vectors"),[this]{return M->bVectors;},[](auto& S,bool V){S.bVectors=V;});
    Toggle->SetTag(bOriginalPoints?TEXT("PointVectors"):TEXT("FieldVectors"));
    return SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            .IsEnabled_Lambda([this]{return !M->Solver->Descriptor().bSourcePoints||M->Solver->Descriptor().bPointVelocity;})
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Toggle]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SStudioMenuButton).Tag(bOriginalPoints?TEXT("PointVectorSettings"):TEXT("FieldVectorSettings"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(5,3)).HasDownArrow(false)
                .ToolTipText(FText::FromString(TEXT("Vector settings: sample count, length mode and scale")))
                .OnGetMenuContent_Lambda([this]{return CachedDisplayMenu(TEXT("Vector"),[this]{return VectorMenu();});}).ButtonContent()[Icon(TEXT("settings"),Muted,14)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,0)[SNew(SBox)
            .Visibility_Lambda([this]{return M->bVectors?EVisibility::Visible:EVisibility::Collapsed;})
            [Live([this]{return VectorLegendText();},9,Muted,true)]];
}
TSharedRef<SWidget> SStudioWorkspace::VectorMenu()
{
    const auto Project=M->Project.Id;const auto Source=M->Solver;
    const auto Current=[this,Project,Source]{return M->Project.Id==Project&&M->Solver==Source&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();};
    const auto Error=MakeShared<FString>();
    const auto Input=[this,Current,Error](const TCHAR* Tag,TFunction<double()> Read,TFunction<void(double)> Write,double Min,double Max,bool bInteger)
    {
        return SNew(SValidatedViewNumber).InputTag(FName(Tag)).Read(MoveTemp(Read)).Revision([this]{return M->RenderIntentRevision;})
            .Write(MoveTemp(Write)).Current(Current).ReportError([Error](FString Message){*Error=MoveTemp(Message);})
            .Minimum(Min).Maximum(Max).Integer(bInteger);
    };
    auto Modes=SNew(SHorizontalBox);
    for(bool Uniform:{false,true})Modes->AddSlot().FillWidth(1).Padding(Uniform?4:0,0,0,0)
        [SNew(SButton).Tag(Uniform?TEXT("VectorUniform"):TEXT("VectorProportional"))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,7))
            .OnClicked_Lambda([this,Current,Uniform]
            {if(Current())M->EditView(TEXT("Vector length mode"),[Uniform](auto& S){S.Display.bUniformVectors=Uniform;});return FReply::Handled();})
            [SNew(STextBlock).Font(Font(10)).Text(FText::FromString(Uniform?TEXT("Equal length"):TEXT("Proportional")))
                .ColorAndOpacity_Lambda([this,Uniform]{return M->bUniformVectors==Uniform?Cyan:Text;})]];
    return SNew(SBox).WidthOverride(320).MaxDesiredHeight(450)[SNew(SBorder).BorderImage(&PanelBrush).Padding(16)
        [SNew(SRetainedFormScrollBox)+SScrollBox::Slot()[SNew(SVerticalBox).IsEnabled_Lambda(Current)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[Label(TEXT("Vector settings"),12,Text,true)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([Error]{return Error->IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
                .Padding(FMargin(0,0,0,10))[SNew(STextBlock).Tag(TEXT("VectorInputError")).Font(Font(9)).ColorAndOpacity(Amber).AutoWrapText(true)
                    .Text_Lambda([Error]{return FText::FromString(*Error);})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Maximum samples"),Input(TEXT("VectorCount"),[this]{return double(M->VectorCount);},
                [this](double V){M->EditView(TEXT("Vector samples"),[V](auto& S){S.Display.VectorCount=int32(V);});},1,4096,true),130)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[Live([this]{return M->Solver->Descriptor().bSourcePoints?
                TEXT("Evenly spaced original rows. No vector interpolation; fewer arrows where velocity is zero."):
                TEXT("Evenly spaced samples on the 2D display plane. Solid and missing samples produce no arrow.");},9,Muted,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Arrow length"),10,Text,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[Modes]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Scale"),Input(TEXT("VectorScale"),[this]{return M->VectorScale;},
                [this](double V){M->EditView(TEXT("Vector scale"),[V](auto& S){S.Display.VectorScale=V;});},.2,3,false),130)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[ViewSlider(M,TEXT("Vector scale"),[this]{return (M->VectorScale-.2)/2.8;},
                [](auto& S,double V){S.VectorScale=.2+2.8*V;})]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Live([this]{return VectorLegendText();},10,Text,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Live([this]
            {
                if(!Scene.IsValid()||!Scene->HasCurrentFrame()||!M->bVectors)return FString();
                const auto& V=Scene->PresentedVectors();return FString::Printf(TEXT("%d arrows / %d samples · frame %d"),V.GlyphCount,V.SampleCount,Scene->PresentedFrame().Index);
            },9,Muted,true)]
            +SVerticalBox::Slot().AutoHeight()[Live([this]{return M->bUniformVectors?
                TEXT("Equal lengths show direction only. Color follows the selected scalar."):
                TEXT("Lengths are proportional to speed and normalized to this sampled frame. Color follows the selected scalar.");},9,Muted,true)]]]];
}
/*
THESIS: Place reproducible streamline seeds and trace the recorded instantaneous field.
OWN-WORLD: Existing Slate Display panel, blue-black inspector, cyan actions and compact numeric rows.
STORY: Choose automatic or saved seeds, set direction and limits, place/edit a seed set, inspect the result.
FIRST VIEWPORT: One settings action beside Streamlines; captured seed/trace counts beneath it.
FORM: Local Operate extension. Shared inspection list, scene placement and view history own seed edits.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
bool SStudioWorkspace::CanShowStreamlines() const
{
    const auto& D=M->Solver->Descriptor();
    return !D.bSourcePoints||(D.bPointVelocity&&(M->Solver->Reconstruction()||M->Solver->VolumeReconstruction()));
}
FBox SStudioWorkspace::StreamlineBounds() const
{
    if(const auto Grid=M->Solver->VolumeReconstruction())
        return FBox(FVector(Grid->SourceBounds.Min.X,Grid->SourceBounds.Min.Z,Grid->SourceBounds.Min.Y),
            FVector(Grid->SourceBounds.Max.X,Grid->SourceBounds.Max.Z,Grid->SourceBounds.Max.Y));
    return M->Solver->Descriptor().DisplayBounds;
}
FString SStudioWorkspace::StreamlineSummary() const
{
    if(!CanShowStreamlines())return TEXT("Attach verified interpolation and recorded velocity to trace this source.");
    if(!M->bStreamlines)return TEXT("Streamlines hidden");
    if(!Scene.IsValid()||!Scene->HasCurrentFrame())return TEXT("Updating streamlines…");
    const auto& S=Scene->PresentedStreams();
    if(!S.Notice.IsEmpty())return S.Notice;
    return FString::Printf(TEXT("%s · %d rejected / %d attempts\n"),S.Method==EStudioStreamMethod::DormandPrince45?TEXT("Adaptive RK45"):TEXT("Midpoint"),S.RejectedAttempts,S.Attempts)+FString::Printf(TEXT("%d seed%s · %d trace%s · frame %d%s"),S.Seeds,S.Seeds==1?TEXT(""):TEXT("s"),S.Lines,S.Lines==1?TEXT(""):TEXT("s"),Scene->PresentedFrame().Index,
        S.bBudgetExhausted?TEXT("\nWork limit reached. Increase the budget or reduce seeds/length."):TEXT(""));
}
TSharedRef<SWidget> SStudioWorkspace::StreamlineControls()
{
    auto Toggle=ViewCheck(M,TEXT("Streamlines"),[this]{return M->bStreamlines;},[](auto& S,bool V){S.bStreamlines=V;});
    Toggle->SetTag(TEXT("Streamlines"));
    return SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox).IsEnabled_Lambda([this]{return CanShowStreamlines();})
            .ToolTipText_Lambda([this]{return FText::FromString(CanShowStreamlines()?TEXT("Instantaneous traces of recorded velocity"):TEXT("Requires recorded velocity and verified interpolation topology"));})
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Toggle]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SStudioMenuButton).Tag(TEXT("StreamlineSettings")).ButtonStyle(&ButtonStyle())
                .ContentPadding(FMargin(5,3)).HasDownArrow(false).ToolTipText(FText::FromString(TEXT("Streamline seeds, direction and limits")))
                .OnGetMenuContent_Lambda([this]{return CachedDisplayMenu(TEXT("Streamline"),[this]{return StreamlineMenu();});}).ButtonContent()[Icon(TEXT("settings"),Muted,14)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[SNew(SBox).Visibility_Lambda([this]{return M->bStreamlines&&CanShowStreamlines()?EVisibility::Visible:EVisibility::Collapsed;})
            .ToolTipText_Lambda([this]{return FText::FromString(StreamlineSummary());})
            [Live([this]
            {
                if(!Scene.IsValid()||!Scene->HasCurrentFrame())return FString(TEXT("Updating…"));
                const auto& S=Scene->PresentedStreams();return S.Lines?FString::Printf(TEXT("%d seed%s · %d trace%s%s"),S.Seeds,S.Seeds==1?TEXT(""):TEXT("s"),S.Lines,S.Lines==1?TEXT(""):TEXT("s"),S.bBudgetExhausted?TEXT(" · limited"):TEXT("")):FString(TEXT("No supported traces"));
            },9,Muted,true)]];
}
TSharedRef<SWidget> SStudioWorkspace::StreamlineMenu()
{
    const auto Project=M->Project.Id;const auto Source=M->Solver;
    const auto Current=[this,Project,Source]{return M->Project.Id==Project&&M->Solver==Source&&!M->IsProjectOpenPending();};
    const auto Error=MakeShared<FString>();const double Scale=StreamlineBounds().GetSize().GetMax();
    auto Items=SNew(SVerticalBox).IsEnabled_Lambda(Current);
    Items->AddSlot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("Streamline settings"),12,Text,true)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("Flow field: recorded velocity (m/s)"),10,Muted)];
    Items->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([Error]{return Error->IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
        .Padding(FMargin(0,0,0,10))[SNew(STextBlock).Tag(TEXT("StreamlineInputError")).Font(Font(9)).ColorAndOpacity(Amber).AutoWrapText(true)
            .Text_Lambda([Error]{return FText::FromString(*Error);})]];
    auto Modes=SNew(SHorizontalBox);
    for(bool Automatic:{true,false})Modes->AddSlot().FillWidth(1).Padding(Automatic?0:4,0,0,0)
        [SNew(SButton).Tag(Automatic?TEXT("StreamAutomatic"):TEXT("StreamSavedSeeds")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(6,7))
            .OnClicked_Lambda([this,Current,Automatic]{if(Current())M->EditView(TEXT("Streamline seed source"),[Automatic](auto& S){S.Display.StreamlineSettings.bAutomaticSeeds=Automatic;});return FReply::Handled();})
            .ToolTipText(FText::FromString(Automatic?TEXT("Original grids use supported liquid cell centers. Other recordings seed actual inflow/outflow faces. No positions are projected into missing data."):TEXT("Use the exact saved inlet, line, plane or point seeds.")))
            [SNew(STextBlock).Font(Font(10)).Text(FText::FromString(Automatic?TEXT("Automatic flow"):TEXT("Saved seed sets")))
                .ColorAndOpacity_Lambda([this,Automatic]{return M->StreamlineSettings.bAutomaticSeeds==Automatic?Cyan:Text;})]];
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Modes];
    auto Number=[this,Current,Error](const TCHAR* Tag,const TCHAR* Name,TFunction<double()> Read,TFunction<void(double)> Write,double Min,double Max,bool Integer=false)
    {
        return SNew(SValidatedViewNumber).InputTag(Tag).Caption(Name).Digits(17).Read(MoveTemp(Read)).Write(MoveTemp(Write))
            .Current(Current).Revision([this]{return M->RenderIntentRevision;}).Minimum(Min).Maximum(Max).Integer(Integer)
            .ReportError([Error](FString E){*Error=MoveTemp(E);});
    };
    auto Automatic=SNew(SVerticalBox);
    Automatic->AddSlot().AutoHeight().Padding(0,0,0,7)[Row(TEXT("Seed count"),Number(TEXT("StreamSeedCount"),TEXT("Seed count"),
        [this]{return double(M->StreamlineSettings.AutomaticSeedCount);},[this](double V){M->EditView(TEXT("Inlet seed count"),[V](auto& S){S.Display.StreamlineSettings.AutomaticSeedCount=int32(V);});},1,512,true),145)];
    Automatic->AddSlot().AutoHeight().Padding(0,0,0,10)[ViewSlider(M,TEXT("Inlet seed density"),[this]{return (M->StreamlineSettings.AutomaticSeedCount-1)/511.;},
        [](auto& S,double V){S.StreamlineSettings.AutomaticSeedCount=1+FMath::RoundToInt(511*V);})];
    Items->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([this]{return M->StreamlineSettings.bAutomaticSeeds?EVisibility::Visible:EVisibility::Collapsed;})[Automatic]];
    auto Manage=Button(TEXT("Edit saved seed sets…"),TEXT("select"),[this,Current]
    {
        FSlateApplication::Get().DismissAllMenus();if(!Current())return;
        bInspectionOpen=true;CancelInspectionPlacement();
        const auto* First=M->InspectionObjects.Seeds.FindByPredicate([this](const auto& S){return S.Source==M->InspectionSource();});
        M->SelectInspectionObject(First?First->Id:FGuid());RefreshInspectionControls();
    });Manage->SetTag(TEXT("StreamManageSeeds"));
    Items->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(SBox).Visibility_Lambda([this]{return M->StreamlineSettings.bAutomaticSeeds?EVisibility::Collapsed:EVisibility::Visible;})[Manage]];
    Items->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Direction"),10,Text,true)];
    auto Directions=SNew(SHorizontalBox);
    for(int32 I=0;I<3;++I)Directions->AddSlot().FillWidth(1).Padding(I?4:0,0,0,0)
        [SNew(SButton).Tag(FName(*FString::Printf(TEXT("StreamDirection%d"),I))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(5,7))
            .OnClicked_Lambda([this,Current,I]{if(Current())M->EditView(TEXT("Streamline direction"),[I](auto& S){S.Display.StreamlineSettings.Direction=EStudioStreamDirection(I);});return FReply::Handled();})
            [SNew(STextBlock).Font(Font(10)).Text(FText::FromString(I==0?TEXT("Forward"):I==1?TEXT("Backward"):TEXT("Both")))
                .ColorAndOpacity_Lambda([this,I]{return int32(M->StreamlineSettings.Direction)==I?Cyan:Text;})]];
    Items->AddSlot().AutoHeight().Padding(0,0,0,14)[Directions];
    auto Markers=Check(TEXT("Direction markers"),[this]{return M->StreamlineSettings.bDirectionMarkers;},[this,Current](bool V)
        {if(Current())M->EditView(TEXT("Streamline direction markers"),[V](auto& S){S.Display.StreamlineSettings.bDirectionMarkers=V;});});
    Markers->SetTag(TEXT("StreamDirectionMarkers"));
    Markers->SetToolTipText(FText::FromString(TEXT("Arrowheads follow recorded velocity, including on backward traces. Equal size shows direction only; color follows the selected scalar.")));
    Items->AddSlot().AutoHeight().Padding(0,0,0,12)[Markers];
    auto Distance=[&](const TCHAR* Tag,const TCHAR* Name,double FStudioStreamlineSettings::*Member,double Low,double High)
    {
        Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(Name,Number(Tag,Name,[this,Member,Scale]{return M->StreamlineSettings.*Member*Scale;},
            [this,Member,Scale,Name,Low,High](double V){M->EditView(Name,[=](auto& S){S.Display.StreamlineSettings.*Member=FMath::Clamp(V/Scale,Low,High);});},Low*Scale,High*Scale),145)];
    };
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Check(TEXT("Adaptive RK45"),[this]{return M->StreamlineSettings.Method==EStudioStreamMethod::DormandPrince45;},[this](bool V){M->EditView(TEXT("Streamline method"),[&](auto& S){S.Display.StreamlineSettings.Method=V?EStudioStreamMethod::DormandPrince45:EStudioStreamMethod::Midpoint;});})];
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("RK45 adapts within the maximum step. Every rejected attempt counts toward the work limit. Original grids seed supported liquid cells; integration never advances source time."),9,Muted,true)];
    Distance(TEXT("StreamWidth"),TEXT("Width (m)"),&FStudioStreamlineSettings::WidthFraction,1.e-6,.02);
    Distance(TEXT("StreamStep"),TEXT("Max step (m)"),&FStudioStreamlineSettings::StepFraction,1.e-5,.1);
    Distance(TEXT("StreamLength"),TEXT("Max length (m)"),&FStudioStreamlineSettings::MaximumLength,.001,100);
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Max steps / branch"),Number(TEXT("StreamMaxSteps"),TEXT("Maximum steps"),
        [this]{return double(M->StreamlineSettings.MaximumSteps);},[this](double V){M->EditView(TEXT("Streamline step limit"),[V](auto& S){S.Display.StreamlineSettings.MaximumSteps=int32(V);});},1,4096,true),145)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,12)[Row(TEXT("Total step budget"),Number(TEXT("StreamWorkBudget"),TEXT("Step budget"),
        [this]{return double(M->StreamlineSettings.WorkBudget);},[this](double V){M->EditView(TEXT("Streamline work budget"),[V](auto& S){S.Display.StreamlineSettings.WorkBudget=int32(V);});},1,65536,true),145)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Live([this]{return StreamlineSummary();},10,Text,true)];
    auto Footer=Label(TEXT("Each direction has its own length/step limit. Distances scale with the domain when switching recordings. Traces stop at missing data and solids; color follows the viewport scalar."),9,Muted);
    Footer->SetAutoWrapText(true);Items->AddSlot().AutoHeight()[Footer];
    return SNew(SBox).WidthOverride(350).MaxDesiredHeight(530)[SNew(SBorder).BorderImage(&PanelBrush).Padding(16)
        [SNew(SRetainedFormScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(8)+SScrollBox::Slot()[Items]]];
}
TSharedRef<SWidget> SStudioWorkspace::VolumeMenu()
{
    const auto VolumeNumber=[](TFunction<double()> Read,TFunction<void(double)> Write,double Min,double Max,const FString& Unit=TEXT(""),double Delta=.1)
    {return Number(MoveTemp(Read),MoveTemp(Write),Min,Max,Unit,Delta,8);};
    auto Items=SNew(SVerticalBox);
    const FGuid Project=M->Project.Id;
    const auto Source=M->Solver;
    const FString Scalar=M->ActiveScalar().Id;
    const auto Current=[this,Project,Source,Scalar]{return M->Project.Id==Project&&M->Solver==Source&&M->ActiveScalar().Id==Scalar;};
    const auto PhysicalNumber=[Current](const TCHAR* Tag,TFunction<double()> Read,TFunction<void(double)> Write,const FString& Unit)
    {
        auto Handle=MakeShared<TWeakPtr<SProjectFilterBox>>();
        const auto Input=SNew(SProjectFilterBox).Tag(Tag).Style(&InputStyle()).Font(Font(10))
            .Text_Lambda([Read]{return FText::FromString(FString::Printf(TEXT("%.17g"),Read()));})
            .SelectAllTextWhenFocused(true).ClearKeyboardFocusOnCommit(false)
            .ToolTipText(FText::FromString(TEXT("Value in source units. Decimal or scientific notation; press Enter to apply.")))
            .OnTextChanged_Lambda([Handle](const FText&){if(auto Box=Handle->Pin())Box->SetError(FText::GetEmpty());})
            .OnTextCommitted_Lambda([Current,Write,Handle](const FText& TextValue,ETextCommit::Type How)
            {
                if(How!=ETextCommit::OnEnter||!Current())return;
                double Value=0;
                if(!StudioColor::ParseNumber(TextValue.ToString(),Value)||FMath::Abs(Value)>1.e20)
                {if(auto Box=Handle->Pin())Box->SetError(FText::FromString(TEXT("Enter a finite value between -1e20 and 1e20.")));return;}
                Write(Value);
            });
        *Handle=Input;
        return SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[Input]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5,0,0,0)[Label(Unit,9,Muted)];
    };
    const auto GridBounds=[Source]{const auto V=Source->VolumeReconstruction();if(!V)return Source->Descriptor().DisplayBounds;
        return FBox(FVector(V->SourceBounds.Min.X,V->SourceBounds.Min.Z,V->SourceBounds.Min.Y),
            FVector(V->SourceBounds.Max.X,V->SourceBounds.Max.Z,V->SourceBounds.Max.Y));};
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("Volume settings"),12,Text,true)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Opacity"),VolumeNumber([this]{return M->VolumeOpacity;},
        [this,Current](double V){if(Current())M->EditView(TEXT("Volume opacity"),[&](auto& S){S.Display.VolumeOpacity=V;});},0,1))];
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Opacity across the color range"),10,Muted)];
    const TCHAR* Names[]={TEXT("Low values"),TEXT("Middle values"),TEXT("High values")};
    for(int32 A=0;A<3;++A)
        Items->AddSlot().AutoHeight().Padding(0,0,0,6)[Row(Names[A],VolumeNumber([this,A]{return M->VolumeOpacityCurve[A];},
            [this,Current,A](double V){if(Current())M->EditView(TEXT("Volume opacity curve"),[&](auto& S){S.Display.VolumeOpacityCurve[A]=V;});},0,1))];
    Items->AddSlot().AutoHeight().Padding(0,6,0,8)[Row(TEXT("Sampling step"),VolumeNumber([this]{return M->VolumeStepVoxels;},
        [this,Current](double V){if(Current())M->EditView(TEXT("Volume quality"),[&](auto& S){S.Display.VolumeStepVoxels=V;});},.25,4,TEXT("voxels"),.25))];
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("Smaller steps improve sampling and cost more GPU time."),9,Muted)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,6)[ViewCheck(M,TEXT("Limit visible values"),[this]{return M->bVolumeThreshold;},
        [](auto& S,bool V){S.bVolumeThreshold=V;})];
    for(int32 End=0;End<2;++End)
        Items->AddSlot().AutoHeight().Padding(0,0,0,6)[Row(End?TEXT("Upper value"):TEXT("Lower value"),PhysicalNumber(
            End?TEXT("VolumeThresholdMaximum"):TEXT("VolumeThresholdMinimum"),
            [this,End]{return End?M->VolumeThresholdMaximum:M->VolumeThresholdMinimum;},
            [this,Current,End](double V){if(!Current())return;
                M->EditView(TEXT("Volume threshold"),[&](auto& S){if(End)S.Display.VolumeThresholdMaximum=FMath::Max(V,S.Display.VolumeThresholdMinimum);
                    else S.Display.VolumeThresholdMinimum=FMath::Min(V,S.Display.VolumeThresholdMaximum);});},M->ActiveScalar().Unit),245)];
    auto Isosurface=ViewCheck(M,TEXT("Isosurface"),[this]{return M->bVolumeIsosurface;},[](auto& S,bool V){S.bVolumeIsosurface=V;});
    Isosurface->SetToolTipText(FText::FromString(TEXT("A surface at one scalar value, with shading to show its 3D shape. The value and original source data are unchanged.")));
    Items->AddSlot().AutoHeight().Padding(0,8,0,6)[Isosurface];
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Isosurface value"),PhysicalNumber(TEXT("VolumeIsovalue"),[this]{return M->VolumeIsovalue;},
        [this,Current](double Value){if(Current())M->EditView(TEXT("Isosurface value"),[&](auto& S){S.Display.VolumeIsovalue=Value;});},M->ActiveScalar().Unit),245)];
    Items->AddSlot().AutoHeight().Padding(0,8,0,8)[Label(TEXT("Clip region · scene coordinates"),10,Text,true)];
    for(int32 A=0;A<3;++A)for(int32 End=0;End<2;++End)
    {
        const FString Name=FString::Printf(TEXT("%s %s"),A==0?TEXT("X"):A==1?TEXT("Y"):TEXT("Z"),End?TEXT("maximum"):TEXT("minimum"));
        Items->AddSlot().AutoHeight().Padding(0,0,0,6)[Row(Name,VolumeNumber(
            [this,GridBounds,A,End]{const auto B=GridBounds();return FMath::Lerp(B.Min[A],B.Max[A],End?M->VolumeClipMaximum[A]:M->VolumeClipMinimum[A]);},
            [this,Current,GridBounds,A,End](double Value){if(!Current())return;const auto B=GridBounds();
                const double V=FMath::Clamp((Value-B.Min[A])/B.GetSize()[A],0.,1.);
                M->EditView(TEXT("Volume clipping"),[&](auto& S){if(End)S.Display.VolumeClipMaximum[A]=FMath::Max(V,S.Display.VolumeClipMinimum[A]+.00001);
                    else S.Display.VolumeClipMinimum[A]=FMath::Min(V,S.Display.VolumeClipMaximum[A]-.00001);});},-1.e8,1.e8,TEXT("m")))];
    }
    Items->AddSlot().AutoHeight().Padding(0,8,0,6)[ViewCheck(M,TEXT("Inspection slice"),[this]{return M->bCutPlane;},[](auto& S,bool V){S.bCutPlane=V;})];
    auto SliceAxes=SNew(SHorizontalBox);
    for(int32 A=0;A<3;++A)SliceAxes->AddSlot().FillWidth(1).Padding(2,0)
        [SNew(SButton).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,4))
            .OnClicked_Lambda([this,Current,GridBounds,A]
            {
                if(Current())M->EditView(TEXT("Slice axis"),[&](auto& S){S.Display.SliceAxis=A;S.Display.SlicePosition=GridBounds().GetCenter()[A];});
                return FReply::Handled();
            })
            [SNew(STextBlock).Text(FText::FromString(A==0?TEXT("X"):A==1?TEXT("Y"):TEXT("Z"))).Font(Font(9))
                .ColorAndOpacity_Lambda([this,A]{return M->SliceAxis==A?Cyan:Muted;})]];
    Items->AddSlot().AutoHeight().Padding(0,0,0,6)[Row(TEXT("Slice normal"),SliceAxes)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Slice position"),VolumeNumber([this]{return M->SlicePosition;},
        [this,Current](double V){if(Current())M->EditView(TEXT("Slice position"),[&](auto& S){S.Display.SlicePosition=V;});},-1.e8,1.e8,TEXT("m")))];
    Items->AddSlot().AutoHeight().Padding(0,6,0,0)[Button(TEXT("Reset volume settings"),TEXT("fit"),[this,Current]
    {if(Current())M->EditView(TEXT("Reset volume settings"),[this](auto& S){const FStudioViewSettings Default;const auto& Field=M->ActiveScalar();
        S.Display.VolumeClipMinimum=Default.VolumeClipMinimum;S.Display.VolumeClipMaximum=Default.VolumeClipMaximum;
        S.Display.VolumeOpacity=Default.VolumeOpacity;S.Display.VolumeOpacityCurve=Default.VolumeOpacityCurve;
        S.Display.VolumeStepVoxels=Default.VolumeStepVoxels;S.Display.bVolumeThreshold=false;
        S.Display.bVolumeIsosurface=false;S.Display.VolumeIsovalue=(Field.Minimum+Field.Maximum)*.5;
        S.Display.VolumeThresholdMinimum=Field.Minimum;S.Display.VolumeThresholdMaximum=Field.Maximum;});})];
    return SNew(SBox).WidthOverride(440).MaxDesiredHeight(520).IsEnabled_Lambda(Current)
        [SNew(SBorder).BorderImage(&PanelBrush).Padding(12)[SNew(SRetainedFormScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(10)+SScrollBox::Slot()[Items]]];
}
TSharedRef<SWidget> SStudioWorkspace::ScalarMenu()
{
    auto Items=SNew(SVerticalBox);
    for(const auto& Field:M->Solver->Descriptor().Scalars)
    {
        Items->AddSlot().AutoHeight().Padding(2)[SNew(SButton).Tag(FName(*(TEXT("Scalar_")+Field.Id))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7))
            .OnClicked_Lambda([this,Id=Field.Id]
            {
                if(M->Solver->Descriptor().Scalars.ContainsByPredicate([&](const auto& F){return F.Id==Id;}))
                    M->EditView(TEXT("Color field"),[&](auto& S){S.Display.ScalarField=Id;S.Display.bHome4Vorticity=false;});
                FSlateApplication::Get().DismissAllMenus();return FReply::Handled();
            })
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Font(10)).ColorAndOpacity(Field.Id==M->ActiveScalar().Id?Cyan:Text).Text(FText::FromString(Field.Label)).AutoWrapText(true)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,0)[Label(Field.Unit+TEXT(" · ")+Field.Origin,9,Muted)]]];
    }
    return SNew(SBox).WidthOverride(248).MaxDesiredHeight(320)[SNew(SScrollBox)+SScrollBox::Slot()[Items]];
}
FString SStudioWorkspace::FieldReadout(double Value,bool Tooltip) const
{
    const auto Field=Scene.IsValid()?Scene->PresentedField():nullptr;const auto Volume=Field?Field->VolumeReconstruction():nullptr;
    const auto& Scalar=Scene->HasPresentedFrame()?Scene->PresentedScalar():M->ActiveScalar();
    if(!Volume||!Volume->OriginalGrid)return StudioHome4Readouts::Scalar(Value,Scalar.Unit,M->UnitDisplay,nullptr,Tooltip);
    const auto Map=Volume->OriginalGrid->UnitContext();
    return StudioHome4Readouts::Scalar(Value,Scalar.Unit,M->UnitDisplay,&Map,Tooltip);
}
TSharedRef<SWidget> SStudioWorkspace::ColorLegend()
{
    auto Mapping=[this]{return Scene->HasPresentedFrame()?Scene->PresentedColorMapping():M->ActiveColorMapping();};
    auto Bar=SNew(SVerticalBox);auto Values=SNew(SVerticalBox);
    for(int32 I=0;I<33;++I)
        Bar->AddSlot().FillHeight(1)[SNew(SBorder).BorderImage(&White).Padding(0)
            .BorderBackgroundColor_Lambda([Mapping,I]{const auto C=Mapping();return StudioColor::Map(FMath::Lerp(C.Minimum,C.Maximum,1.-I/32.),C);})];
    for(int32 I=0;I<4;++I)
        Values->AddSlot().FillHeight(1)
            [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text)
                .Text_Lambda([this,Mapping,I]{const auto C=Mapping();return FText::FromString(FieldReadout(FMath::Lerp(C.Minimum,C.Maximum,1.-I/4.)));})
                .ToolTipText_Lambda([this,Mapping,I]{const auto C=Mapping();return FText::FromString(FieldReadout(FMath::Lerp(C.Minimum,C.Maximum,1.-I/4.),true));})];
    Values->AddSlot().AutoHeight()[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text)
        .Text_Lambda([this,Mapping]{return FText::FromString(FieldReadout(Mapping().Minimum));})
        .ToolTipText_Lambda([this,Mapping]{return FText::FromString(FieldReadout(Mapping().Minimum,true));})];
    return SNew(SBorder).Tag(TEXT("ColorLegend")).BorderImage(&PanelBrush).Padding(10)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,7)[Live([this]
                {const auto& F=Scene->HasPresentedFrame()?Scene->PresentedScalar():M->ActiveScalar();return F.Label;},9)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(11).HeightOverride(64)[Bar]]
                +SHorizontalBox::Slot().AutoWidth().Padding(7,0)[SNew(SBox).HeightOverride(64)[Values]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,7,0,0)[Live([this,Mapping]
                {const auto C=Mapping();const auto& S=Scene->HasPresentedFrame()?Scene->PresentedScalar():M->ActiveScalar();return FString(C.bManualRange?TEXT("Custom range"):S.DefaultDisplayMaximum.IsSet()?TEXT("First-frame 99% range"):TEXT("Source range"))+TEXT(" · ")+StudioColor::PaletteName(C.Palette);},8,Muted)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).WidthOverride(166)
                .Visibility_Lambda([this]{return Scene->PresentedMesh().Notice.IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
                .Padding(FMargin(0,8,0,0))[Live([this]
                {
                    const auto& Mesh=Scene->PresentedMesh();
                    return Mesh.Triangles?FString::Printf(TEXT("%s · %s triangles\nEdges show topology%s"),
                        Mesh.bDerived?TEXT("Derived mesh"):TEXT("Original CFD mesh"),*FText::AsNumber(Mesh.Triangles).ToString(),
                        Mesh.bFieldFillHidden?TEXT(" · field fill hidden"):TEXT("")):Mesh.Notice;
                },8,Muted,true)]]];
}
/*
THESIS: Set field colors precisely while preserving original CFD values and camera state.
OWN-WORLD: Inherit the compact blue-black Slate panels, native inputs and cyan selection.
STORY: Choose a palette, enter a range, apply it, or return to the source range.
FIRST VIEWPORT: A settings button beside the field opens a bounded 340-unit popover.
FORM: Local Operate extension; the legend describes the mapping of the presented field.
FINISH: Packaged controls at both window sizes, finish review and documentation are required.
*/
TSharedRef<SWidget> SStudioWorkspace::ColorMenu()
{
    const auto Field=M->ActiveScalar();const FGuid Owner=M->Project.Id;const FString Dataset=M->Project.Dataset;
    auto SameField=[this,Owner,Dataset,Id=Field.Id]{return Owner==M->Project.Id&&Dataset==M->Project.Dataset&&Id==M->ActiveScalar().Id;};
    auto Available=[this,SameField]{return SameField()&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();};
    const auto Initial=M->ActiveColorMapping();
    auto Minimum=MakeShared<FString>(FString::Printf(TEXT("%.17g"),Initial.Minimum));
    auto Maximum=MakeShared<FString>(FString::Printf(TEXT("%.17g"),Initial.Maximum));
    auto Message=MakeShared<FString>();auto Error=MakeShared<bool>(false);
    auto Items=SNew(SVerticalBox);
    Items->AddSlot().AutoHeight().Padding(0,0,0,4)[Label(TEXT("Color settings"),12,Text,true)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(STextBlock).Text(FText::FromString(Field.Label+TEXT(" · ")+Field.Unit)).Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Palette"),10,Text,true)];
    for(int32 Palette=0;Palette<4;++Palette)
    {
        auto Swatch=SNew(SHorizontalBox);
        for(int32 I=0;I<17;++I)Swatch->AddSlot().FillWidth(1)
            [SNew(SBorder).BorderImage(&White).Padding(0).BorderBackgroundColor_Lambda([this,Palette,I]
                {auto Preview=M->ActiveColorMapping();Preview.Palette=Palette;Preview.Minimum=0;Preview.Maximum=1;return StudioColor::Map(I/16.,Preview);})];
        Items->AddSlot().AutoHeight().Padding(0,0,0,5)
            [SNew(SButton).Tag(FName(*FString::Printf(TEXT("ColorPalette%d"),Palette))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
                .OnClicked_Lambda([this,Available,Palette,Message,Error]
                {
                    if(!Available())return FReply::Handled();
                    const auto C=M->ActiveColorMapping();
                    *Error=!M->SetScalarStyle(Palette,C.bManualRange,C.Minimum,C.Maximum);
                    *Message=*Error?M->Notice:TEXT("Palette applied.");return FReply::Handled();
                })
                [SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Font(Font(10))
                        .Text(FText::FromString(StudioColor::PaletteName(Palette)))
                        .ColorAndOpacity_Lambda([this,SameField,Palette]{return SameField()&&M->ActiveColorMapping().Palette==Palette?Cyan:Text;})]
                    +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(76).HeightOverride(10)[Swatch]]]];
    }
    auto Custom=SNew(SVerticalBox);
    auto ColorInputs=MakeShared<TArray<FString>>();
    for(const auto& C:{Initial.LowColor,Initial.MiddleColor,Initial.HighColor})ColorInputs->Add(TEXT("#")+C.ToFColorSRGB().ToHex().Left(6));
    const TCHAR* ColorNames[]={TEXT("Low color"),TEXT("Middle color"),TEXT("High color")};
    for(int32 I=0;I<3;++I)
        Custom->AddSlot().AutoHeight().Padding(0,0,0,5)[Row(ColorNames[I],
            SNew(SProjectFilterBox).Tag(FName(*FString::Printf(TEXT("CustomColor%d"),I))).Style(&InputStyle()).Font(Font(10))
                .Text(FText::FromString((*ColorInputs)[I])).SelectAllTextWhenFocused(true)
                .ToolTipText(FText::FromString(TEXT("sRGB color in #RRGGBB format. Stops are at the low, middle and high values of the color range.")))
                .OnTextChanged_Lambda([ColorInputs,I,Message,Error](const FText& Input){(*ColorInputs)[I]=Input.ToString();Message->Empty();*Error=false;}),215)];
    auto ApplyColors=Button(TEXT("Apply colors"),TEXT("check"),[this,Available,ColorInputs,Message,Error]
    {
        if(!Available())return;
        TArray<FLinearColor> Colors;Colors.SetNum(3);
        for(int32 I=0;I<3;++I)if(!StudioColor::ParseHexColor((*ColorInputs)[I],Colors[I]))
        {*Error=true;*Message=TEXT("Enter each color as six hex digits, for example #00AADD.");return;}
        const auto C=M->ActiveColorMapping();
        *Error=!M->SetScalarStyle(3,C.bManualRange,C.Minimum,C.Maximum,Colors);
        *Message=*Error?M->Notice:TEXT("Custom colors applied.");
    },Cyan);ApplyColors->SetTag(TEXT("ApplyCustomColors"));
    Custom->AddSlot().AutoHeight().Padding(0,0,0,7)[ApplyColors];
    Items->AddSlot().AutoHeight().Padding(0,5,0,0)[SNew(SBox)
        .Visibility_Lambda([this]{return M->ActiveColorMapping().Palette==3?EVisibility::Visible:EVisibility::Collapsed;})[Custom]];
    Items->AddSlot().AutoHeight().Padding(0,9,0,5)[Label(TEXT("Range"),10,Text,true)];
    const auto SourceRange=Label(FString::Printf(TEXT("Source: %.6g to %.6g %s"),Field.Minimum,Field.Maximum,*Field.Unit),9,Muted);
    SourceRange->SetAutoWrapText(true);
    SourceRange->SetToolTipText(FText::FromString(FString::Printf(TEXT("Source metadata range: %.17g to %.17g %s"),Field.Minimum,Field.Maximum,*Field.Unit)));
    Items->AddSlot().AutoHeight().Padding(0,0,0,7)[SourceRange];
    auto Input=[Message,Error](const TSharedRef<FString>& Value,const TCHAR* Tag,const FString& Name)
    {
        return SNew(SProjectFilterBox).Tag(Tag).Style(&InputStyle()).Font(Font(10)).Text(FText::FromString(*Value))
            .SelectAllTextWhenFocused(true).ClearKeyboardFocusOnCommit(false)
            .ToolTipText(FText::FromString(Name+TEXT(" in source units. Decimal or scientific notation, using a decimal point.")))
            .OnTextChanged_Lambda([Value,Message,Error](const FText& InputText){*Value=InputText.ToString();Message->Empty();*Error=false;});
    };
    const auto MinInput=Input(Minimum,TEXT("ColorMinimum"),TEXT("Minimum"));
    const auto MaxInput=Input(Maximum,TEXT("ColorMaximum"),TEXT("Maximum"));
    Items->AddSlot().AutoHeight().Padding(0,0,0,5)[Row(TEXT("Minimum"),MinInput,215)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,9)[Row(TEXT("Maximum"),MaxInput,215)];
    auto Apply=Button(TEXT("Apply range"),TEXT("check"),[this,Available,Minimum,Maximum,Message,Error]
    {
        if(!Available())return;
        double Low=0,High=0;
        if(!StudioColor::ParseNumber(*Minimum,Low)||!StudioColor::ParseNumber(*Maximum,High))
        {*Error=true;*Message=TEXT("Enter finite numbers, for example -100 or 1e-6.");return;}
        if(Low>=High||!FMath::IsFinite(High-Low))
        {*Error=true;*Message=TEXT("Minimum must be below maximum, with a finite range.");return;}
        *Error=!M->SetScalarStyle(M->ActiveColorMapping().Palette,true,Low,High);
        *Message=*Error?M->Notice:TEXT("Custom range applied.");
    },Cyan);Apply->SetTag(TEXT("ApplyColorRange"));
    const TWeakPtr<SEditableTextBox> WeakMinimum=MinInput,WeakMaximum=MaxInput;
    auto Reset=Button(TEXT("Source range"),TEXT("undo"),[this,Available,Field,Message,Error,WeakMinimum,WeakMaximum]
    {
        if(!Available())return;
        *Error=!M->SetScalarStyle(M->ActiveColorMapping().Palette,false,Field.Minimum,Field.Maximum);
        if(!*Error)
        {
            if(auto Box=WeakMinimum.Pin())Box->SetText(FText::FromString(FString::Printf(TEXT("%.17g"),Field.Minimum)));
            if(auto Box=WeakMaximum.Pin())Box->SetText(FText::FromString(FString::Printf(TEXT("%.17g"),Field.Maximum)));
        }
        *Message=*Error?M->Notice:TEXT("Source range restored.");
    });Reset->SetTag(TEXT("ResetColorRange"));
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,5,0)[Apply]+SHorizontalBox::Slot().FillWidth(1)[Reset]];
    Items->AddSlot().AutoHeight().Padding(0,0,0,6)[Live([this,SameField]
        {return !SameField()?TEXT("Reopen to view current colors"):M->ActiveColorMapping().bManualRange?TEXT("Using custom range"):TEXT("Using source range");},9,Muted)];
    Items->AddSlot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(TEXT("Values outside a custom range use its endpoint colors. Original values are preserved.")))
        .Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)];
    Items->AddSlot().AutoHeight().Padding(0,8,0,0)[SNew(STextBlock).Tag(TEXT("ColorFeedback")).Font(Font(9)).AutoWrapText(true)
        .Text_Lambda([Message]{return FText::FromString(*Message);}).ColorAndOpacity_Lambda([Error]{return *Error?Amber:Green;})];
    return SNew(SBox).WidthOverride(340).MaxDesiredHeight(490)[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Live([this,SameField]
                {return !SameField()?TEXT("Source or field changed. Reopen color settings to edit it."):
                    M->IsProjectOpenPending()||M->IsRecordingLoadPending()?TEXT("Waiting for the source to finish loading…"):TEXT("");},9,Amber,true)]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SRetainedFormScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(10)+SScrollBox::Slot()
                [SNew(SBox).Tag(TEXT("ColorFieldContext")).IsEnabled_Lambda(Available)[Items]]]]];
}
TSharedRef<SWidget> SStudioWorkspace::Timeline()
{
    return SNew(SBorder).BorderImage(&PanelBrush).Padding(10,8)
    [SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,12,0)[Live([this]{return FString::Printf(TEXT("Snapshot %d / %d"),M->SelectedFrame+1,M->Solver->FrameCount());},10)]
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Slider([this]{return M->Frames.Num()<2?0.:static_cast<double>(M->SelectedFrame)/(M->Frames.Num()-1);},[this](double V){M->Scrub(V);})]
        +SHorizontalBox::Slot().AutoWidth().Padding(12,0,0,0)[Button(TEXT("Follow replay"),TEXT("run"),[this]{M->ReturnToLive();},Cyan)]
        +SHorizontalBox::Slot().AutoWidth().Padding(8,0,0,0)[SAssignNew(SnapshotButton,SStudioMenuButton).Tag(TEXT("SnapshotOptions"))
            .ButtonStyle(&ButtonStyle()).OnGetMenuContent(this,&SStudioWorkspace::SnapshotMenu)
            .ButtonContent()[SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Icon(TEXT("camera"))]
                +SHorizontalBox::Slot().AutoWidth().Padding(6,0,0,0)[Live([this]{return SnapshotUI->ButtonLabel();},10)]]]];
}
TSharedRef<SWidget> SStudioWorkspace::Monitors()
{
    auto Card=[&](const FString& Title,TSharedRef<SWidget> Content)
    { return SNew(SBorder).BorderImage(&LineBrush).Padding(1)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[SNew(SBorder).BorderImage(&RaisedBrush).Padding(10,8)[Label(Title,10)]]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SBorder).BorderImage(&Background).Padding(9,10)[Content]]]; };
    auto Missing=[&](const FString& Detail) -> TSharedRef<SWidget>
    { return SNew(SVerticalBox)
        +SVerticalBox::Slot().FillHeight(1)[SNew(SSpacer)]
        +SVerticalBox::Slot().AutoHeight()[Label(TEXT("Not supplied"),12,Muted)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(STextBlock).Text(FText::FromString(Detail)).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SSpacer)]; };
    return SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[Card(TEXT("Residuals"),SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->ResidualHistory()?1:0;})
            +SWidgetSwitcher::Slot()[SNew(SVerticalBox)
                +SVerticalBox::Slot().FillHeight(1)[SNew(SSpacer)]
                +SVerticalBox::Slot().AutoHeight()[Live([this]{return M->IsResidualLoading()?TEXT("Verifying residual log…"):M->Project.Residual.Path.IsEmpty()?TEXT("Not supplied"):TEXT("Source unavailable");},11,Muted)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,8)[Live([this]{return M->ResidualNotice.IsEmpty()?TEXT("Import a completed OpenFOAM log in Monitors."):M->ResidualNotice;},9,Muted,true)]
                +SVerticalBox::Slot().FillHeight(1)[SNew(SSpacer)]]
            +SWidgetSwitcher::Slot()[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Amber).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                    .Text_Lambda([this]{return FText::FromString(TEXT("Independent · ")+FPaths::GetCleanFilename(M->Project.Residual.Path));})
                    .ToolTipText_Lambda([this]{return FText::FromString(M->Project.Residual.Path);})]
                +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,0)[Live([this]
                    {const auto& C=M->Project.Residual.Chart;return FString::Printf(TEXT("%d of %d series · %s"),FMath::Min(3,C.Series.Num()),C.Series.Num(),C.bLogY?TEXT("Log"):TEXT("Linear"));},8,Muted)]
                +SVerticalBox::Slot().FillHeight(1)[SNew(SStudioMonitorChart).Model(M).Compact(true).Residual(true).Tag(TEXT("ResidualPreviewChart"))]
                +SVerticalBox::Slot().AutoHeight()[Label(TEXT("i: first initial · f: last final"),8,Muted)]] )]
        +SHorizontalBox::Slot().FillWidth(.9).Padding(0,0,6,0)[Card(TEXT("Selected history"),SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->MonitorHistory()?1:0;})
            +SWidgetSwitcher::Slot()[Missing(TEXT("Choose a published history in Monitors. This recording has no force history."))]
            +SWidgetSwitcher::Slot()[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[Live([this]{const auto H=M->MonitorHistory();return H?(H->FieldRecordingId.IsSet()&&*H->FieldRecordingId==M->Project.Dataset?TEXT("Recording history"):TEXT("Independent history")):FString();},8,Amber)]
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                    .Text_Lambda([this]{const auto H=M->MonitorHistory();return H?FText::FromString(H->Title):FText();})
                    .ToolTipText_Lambda([this]{const auto H=M->MonitorHistory();return H?FText::FromString(H->Title):FText();})]
                +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,0)[SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().FillWidth(1)[SNew(SStudioMenuButton).Tag(TEXT("MonitorPreviewSeries")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(4,2))
                        .IsEnabled_Lambda([this]{return M->Project.Monitor.Series.Num()>1;})
                        .ToolTipText(FText::FromString(TEXT("Preview one selected series. Monitors shows the full selection.")))
                        .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                        {
                            auto Rows=SNew(SVerticalBox);const auto H=M->MonitorHistory();
                            if(H)for(const auto& Id:M->Project.Monitor.Series)if(const auto* C=H->FindColumn(Id))
                            {
                                auto Pick=Button(C->Label+TEXT(" (")+C->Unit+TEXT(")"),TEXT("chart"),[this,Id]{MonitorPreviewSeries=Id;FSlateApplication::Get().DismissAllMenus();});
                                Pick->SetTag(FName(*(TEXT("MonitorPreviewSeries_")+Id)));Rows->AddSlot().AutoHeight().Padding(0,3)[Pick];
                            }
                            return SNew(SBorder).BorderImage(&PanelBrush).Padding(8)[Rows];
                        }).ButtonContent()[Live([this]
                        {
                            const auto H=M->MonitorHistory();const auto& Series=M->Project.Monitor.Series;if(!H||Series.IsEmpty())return FString(TEXT("No series"));
                            const int32 Index=FMath::Max(0,Series.IndexOfByKey(MonitorPreviewSeries));const auto* C=H->FindColumn(Series[Index]);
                            return C?C->Id+TEXT(" (")+C->Unit+FString::Printf(TEXT(") · %d of %d"),Index+1,Series.Num()):FString();
                        },8,Text)]]
                    +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5,0,0,0)[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted)
                        .Text_Lambda([this]{return FText::FromString(M->Project.Monitor.bLogY?TEXT("Log"):TEXT("Linear"));})
                        .ToolTipText(FText::FromString(TEXT("The saved value scale applies to this preview. Log scale omits nonpositive samples and breaks the trace at gaps.")))]]
                +SVerticalBox::Slot().FillHeight(1)[SNew(SStudioMonitorChart).Model(M).Compact(true)
                    .Series_Lambda([this]{return MonitorPreviewSeries;}).Tag(TEXT("MonitorPreviewChart"))]] )]
        +SHorizontalBox::Slot().FillWidth(1.03).Padding(0,0,6,0)[Card(TEXT("Activity log"),SNew(SVerticalBox)
            +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox)+SScrollBox::Slot()[Live([this]{return LogState->Preview;},9,Muted,true)]]
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Live([this]{return LogState->View.IsFollowing()?TEXT("Following · UTC"):TEXT("View paused");},8,Muted)]
                +SHorizontalBox::Slot().AutoWidth()[SAssignNew(LogState->ExpandButton,SButton).Tag(TEXT("LogExpand"))
                    .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,6)).OnClicked_Lambda([this]{ExpandActivityLog(true);return FReply::Handled();})
                    [Label(TEXT("Open log"),9)]]])]
        +SHorizontalBox::Slot().FillWidth(.53)[Card(TEXT("Control status"),SNew(SVerticalBox)
            +SVerticalBox::Slot().FillHeight(1)[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->Project.bControlHarness?1:0;})
            +SWidgetSwitcher::Slot()[SNew(SJobProgress).Model(M)]
            +SWidgetSwitcher::Slot()[SNew(SVerticalBox)
                +SVerticalBox::Slot().FillHeight(1)[SNew(SSpacer)]
                +SVerticalBox::Slot().AutoHeight()[Live([this]{return StudioJobs::StateName(M->Job().State());},11,Cyan)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(STextBlock).Text(FText::FromString(TEXT("Control test\nNo CFD output"))).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]
                +SVerticalBox::Slot().FillHeight(1)[SNew(SSpacer)]]]
            +SVerticalBox::Slot().AutoHeight()[SAssignNew(PerformanceButton,SButton).Tag(TEXT("ViewPerformance"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(4,6))
                .IsEnabled_Lambda([this]{return !M->CameraPlacement()&&!InspectionPlacement;})
                .OnClicked_Lambda([this]{OpenPerformance(true);return FReply::Handled();})[Label(TEXT("View performance"),8)]] )];
}

// THESIS: Inspect published histories without assigning them to an unrelated flow recording.
// OWN-WORLD: Existing blue-black Operate workspace, compact aligned controls and fine chart axes.
// STORY: Choose a verified source, select like-unit series, inspect original samples and time windows.
// FIRST VIEWPORT: Large chart at left; source, units, series and provenance in one right inspector.
// FORM: Local native extension; sidebar owns navigation, chart settings persist in the project.
// FINISH: unreviewed and undocumented is unfinished; finish review, verdict and DESIGN.md follow.
void SStudioWorkspace::ExpandActivityLog(bool bExpand)
{
    M->bActivityLogExpanded=bExpand;
    const TSharedPtr<SWidget> Focus=bExpand?StaticCastSharedPtr<SWidget>(LogState->Search.Pin()):StaticCastSharedPtr<SWidget>(LogState->ExpandButton.Pin());
    if(Focus)FSlateApplication::Get().SetKeyboardFocus(Focus,EFocusCause::Navigation);
}
void SStudioWorkspace::RefreshActivityLog()
{
    auto& S=*LogState;
    if(const auto Result=S.Export.Poll())
    {
        if(S.ExportOwner==M->Project.Id)
        {
            S.ExportPath=Result->Path;
            S.ExportNotice=Result->bSuccess?FString::Printf(TEXT("Exported %d %s · %s"),Result->Entries,Result->Entries==1?TEXT("entry"):TEXT("entries"),*FPaths::GetCleanFilename(Result->Path)):
                TEXT("Export failed: ")+Result->Error;
            M->AddLog(S.ExportNotice,Result->bSuccess?EStudioLogSeverity::Info:EStudioLogSeverity::Error);
        }
    }
    if(S.Owner!=M->Project.Id)
    {
        S.Owner=M->Project.Id;S.bAllProjects=false;S.bCurrentRun=false;S.Query=FStudioLogQuery();
        S.View.SetFollowing(true,M->ActivityLog());S.View.ShowRetained();S.ExportNotice.Empty();S.ExportPath.Empty();
        S.bDirty=true;S.RecentSequence=MAX_uint64;M->bActivityLogExpanded=false;if(const auto Search=S.Search.Pin())Search->SetText(FText::GetEmpty());
    }
    const FGuid CurrentRun=M->Job().Run()?M->Job().Run()->GetId():FGuid();
    if(S.Run!=CurrentRun){S.Run=CurrentRun;S.bDirty=true;if(!CurrentRun.IsValid())S.bCurrentRun=false;}
    S.Query.ProjectId=S.bAllProjects?FGuid():M->Project.Id;
    S.Query.RunId=S.bCurrentRun?CurrentRun:FGuid();
    S.View.Refresh(M->ActivityLog());
    if(S.bDirty||S.ShownSequence!=S.View.CapturedSequence())
    {
        S.bDirty=false;S.ShownSequence=S.View.CapturedSequence();
        const auto Entries=S.View.Select(S.Query);S.Rows.Reset();S.Detail.Empty();
        for(const auto& Entry:Entries)
        {
            S.Rows.Add(MakeShared<FStudioLogEntry>(Entry));
            if(Entry.Sequence==S.SelectedSequence)
                S.Detail=StudioLog::Line(Entry)+TEXT("\nObserved: ")+Entry.ObservedUTC.ToIso8601()+TEXT("\nProject: ")+Entry.ProjectId.ToString()+
                    (Entry.RunId.IsValid()?TEXT("\nRun: ")+Entry.RunId.ToString():TEXT(""))+
                    (Entry.SourceReference.IsEmpty()?TEXT(""):TEXT("\nSource: ")+Entry.SourceReference);
        }
        S.Preview.Empty();
        for(int32 I=Entries.Num()-1;I>=FMath::Max(0,Entries.Num()-3);--I)
            S.Preview+=StudioLog::Line(Entries[I])+TEXT("\n\n");
        if(Entries.IsEmpty())S.Preview=TEXT("No visible entries. Open log to adjust filters or show retained entries.");
        S.List.Pin()->RequestListRefresh();
        if(S.View.IsFollowing()&&!S.Rows.IsEmpty())S.List.Pin()->RequestScrollIntoView(S.Rows.Last());
    }
    // Dashboard activity is a current-project summary, independent of log filters.
    if(S.RecentSequence==M->ActivityLog().LastSequence())return;
    S.RecentSequence=M->ActivityLog().LastSequence();
    const auto Recent=M->ActivityLog().Snapshot();S.RecentActivity.Empty();int32 Count=0;
    for(int32 I=Recent.Num()-1;I>=0&&Count<5;--I)if(Recent[I].ProjectId==M->Project.Id)
    {if(Count++)S.RecentActivity+=TEXT("\n");S.RecentActivity+=Recent[I].Message;}
    if(S.RecentActivity.IsEmpty())S.RecentActivity=TEXT("No activity for this project in this session.");
}

TSharedRef<SWidget> SStudioWorkspace::ActivityLogPanel()
{
    const auto S=LogState;
    static const FTableRowStyle LogRowStyle=[]
    {
        auto Style=FCoreStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row");
        Style.SetEvenRowBackgroundBrush(FSlateColorBrush(BG)).SetOddRowBackgroundBrush(FSlateColorBrush(Panel))
            .SetEvenRowBackgroundHoveredBrush(FSlateColorBrush(Raised)).SetOddRowBackgroundHoveredBrush(FSlateColorBrush(Raised))
            .SetActiveBrush(FSlateColorBrush(Raised)).SetActiveHoveredBrush(FSlateColorBrush(Raised))
            .SetInactiveBrush(FSlateColorBrush(Raised)).SetInactiveHoveredBrush(FSlateColorBrush(Raised))
            .SetSelectorFocusedBrush(FSlateRoundedBoxBrush(FLinearColor::Transparent,0.f,Cyan,1.f));
        return Style;
    }();
    static const FEditableTextBoxStyle LogDetailStyle=[]
    {
        auto Style=InputStyle();Style.SetBackgroundImageReadOnly(FSlateRoundedBoxBrush(BG,3.f,Line,1.f));return Style;
    }();
    auto Choices=[](const TArray<FString>& Labels,const FString& Prefix,TFunction<void(int32)> Pick)->TSharedRef<SWidget>
    {
        auto Rows=SNew(SVerticalBox);
        for(int32 I=0;I<Labels.Num();++I)
            Rows->AddSlot().AutoHeight().Padding(0,2)[SNew(SButton).Tag(FName(Prefix+FString::FromInt(I)))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7)).HAlign(HAlign_Left)
                .OnClicked_Lambda([Pick,I]{Pick(I);FSlateApplication::Get().DismissAllMenus();return FReply::Handled();})[Label(Labels[I],10)]];
        return SNew(SBorder).BorderImage(&PanelBrush).Padding(7)[Rows];
    };
    return SNew(SBorder).BorderImage(&PanelBrush).Padding(18,16)
    [SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Activity log"),18,Text,true)]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[SNew(SButton).Tag(TEXT("LogFollow"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(10,8))
                .ToolTipText(FText::FromString(TEXT("Pause freezes this view while collection continues. Follow latest resumes from the retained session entries.")))
                .OnClicked_Lambda([this,S]{S->View.SetFollowing(!S->View.IsFollowing(),M->ActivityLog());S->bDirty=true;RefreshActivityLog();return FReply::Handled();})
                [Live([S]{return S->View.IsFollowing()?TEXT("Pause view"):TEXT("Follow latest");},10,Cyan)]]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("LogRestore"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(10,8)).OnClicked_Lambda([this]{ExpandActivityLog(false);return FReply::Handled();})
                [Label(TEXT("Restore Solve"),10)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[Label(TEXT("Observed UTC · Application, playback and control events from this session"),10,Muted)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,8,0)[SAssignNew(S->Search,SProjectFilterBox).Tag(TEXT("LogSearch"))
                .Style(&InputStyle()).Font(Font(10)).ForegroundColor(Text).HintText(FText::FromString(TEXT("Search messages, sources or IDs")))
                .OnTextChanged_Lambda([S](const FText& Value){S->Query.Search=Value.ToString().Left(256);S->bDirty=true;if(Value.ToString().Len()>256)S->Search.Pin()->SetText(FText::FromString(S->Query.Search));})]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[SNew(SBox).WidthOverride(164)
                [SNew(SStudioMenuButton).Tag(TEXT("LogSource")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7))
                    .OnGetMenuContent_Lambda([S,Choices]{return Choices({TEXT("All sources"),TEXT("Application"),TEXT("Playback"),TEXT("Control harness")},TEXT("LogSource_"),[S](int32 I)
                        {S->Query.Source=I?TOptional<EStudioLogSource>(EStudioLogSource(I-1)):TOptional<EStudioLogSource>();S->bDirty=true;});})
                    .ButtonContent()[Live([S]{return S->Query.Source?FString(StudioLog::SourceName(*S->Query.Source)):TEXT("All sources");})]]]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(170)
                [SNew(SStudioMenuButton).Tag(TEXT("LogSeverity")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7))
                    .OnGetMenuContent_Lambda([S,Choices]{return Choices({TEXT("All severities"),TEXT("Warnings + errors"),TEXT("Errors only")},TEXT("LogSeverity_"),[S](int32 I)
                        {S->Query.MinimumSeverity=EStudioLogSeverity(I);S->bDirty=true;});})
                    .ButtonContent()[Live([S]{return S->Query.MinimumSeverity==EStudioLogSeverity::Info?TEXT("All severities"):
                        S->Query.MinimumSeverity==EStudioLogSeverity::Warning?TEXT("Warnings + errors"):TEXT("Errors only");})]]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,12,0)[SNew(SStudioMenuButton).Tag(TEXT("LogScope"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7))
                .OnGetMenuContent_Lambda([S,Choices]{return Choices({TEXT("Current project"),TEXT("All session projects")},TEXT("LogScope_"),[S](int32 I)
                    {S->bAllProjects=I!=0;if(S->bAllProjects)S->bCurrentRun=false;S->bDirty=true;});})
                .ButtonContent()[Live([S]{return S->bAllProjects?TEXT("All session projects"):TEXT("Current project");})]]
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(SCheckBox).Tag(TEXT("LogCurrentRun"))
                .IsEnabled_Lambda([S]{return !S->bAllProjects&&S->Run.IsValid();})
                .IsChecked_Lambda([S]{return S->bCurrentRun?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([S](ECheckBoxState State){S->bCurrentRun=State==ECheckBoxState::Checked;S->bDirty=true;})
                .ToolTipText(FText::FromString(TEXT("Show entries belonging to the current control run. Available after submitting a control job.")))
                [Label(TEXT("Current control run"),10)]]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[SNew(SButton).Tag(TEXT("LogClear"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7)).IsEnabled_Lambda([S]{return !S->Rows.IsEmpty();})
                .ToolTipText(FText::FromString(TEXT("Hide entries through the current snapshot. Retained entries are recoverable with Show retained.")))
                .OnClicked_Lambda([S]{S->View.ClearView();S->SelectedSequence=0;S->bDirty=true;return FReply::Handled();})[Label(TEXT("Clear view"),10)]]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[SNew(SButton).Tag(TEXT("LogShowRetained"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7)).IsEnabled_Lambda([S]{return S->View.HiddenSequence()!=0;})
                .OnClicked_Lambda([S]{S->View.ShowRetained();S->bDirty=true;return FReply::Handled();})[Label(TEXT("Show retained"),10)]]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("LogExport"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7)).IsEnabled_Lambda([S]{return !S->Rows.IsEmpty()&&!S->Export.IsBusy();})
                .ToolTipText(FText::FromString(TEXT("Export the visible filtered snapshot, including paused rows, to CSV. Messages and context are copied before choosing a file.")))
                .OnClicked_Lambda([this,S]
                {
                    RefreshActivityLog();auto Entries=S->View.Select(S->Query);const FGuid Owner=M->Project.Id;
                    FString Path;if(!StudioFileDialog::CSV(TEXT("activity-")+FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")),Path,
                        TEXT("Export Activity Log"),TEXT("Exports the visible snapshot with UTC observation times, severity and project/run context.")))return FReply::Handled();
                    if(S->Export.Start(MoveTemp(Entries),Path)){S->ExportOwner=Owner;S->ExportNotice=TEXT("Exporting visible entries…");S->ExportPath=Path;}
                    return FReply::Handled();
                })[Label(TEXT("Export CSV…"),10)]]]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SBorder).BorderImage(&LineBrush).Padding(1)
            [SNew(SOverlay)
                +SOverlay::Slot()[SNew(SBorder).BorderImage(&Background).Padding(0)
                    [SAssignNew(S->List,SListView<TSharedPtr<FStudioLogEntry>>).Tag(TEXT("LogList"))
                        .ListItemsSource(&S->Rows).SelectionMode(ESelectionMode::Single)
                        .OnGenerateRow_Lambda([](TSharedPtr<FStudioLogEntry> Entry,const TSharedRef<STableViewBase>& Owner)
                        {
                            const auto Color=Entry->Severity==EStudioLogSeverity::Error?FLinearColor(1,.28,.23):Entry->Severity==EStudioLogSeverity::Warning?Amber:Muted;
                            const FString Context=Entry->ObservedUTC.ToString(TEXT("%H:%M:%S"))+TEXT(" UTC  ·  ")+StudioLog::SeverityName(Entry->Severity)+TEXT("  ·  ")+StudioLog::SourceName(Entry->Source);
                            return SNew(STableRow<TSharedPtr<FStudioLogEntry>>,Owner).Style(&LogRowStyle).Padding(FMargin(10,7)).ToolTipText(FText::FromString(StudioLog::Line(*Entry)))
                                [SNew(SVerticalBox)
                                    +SVerticalBox::Slot().AutoHeight()[Label(Context,9,Color)]
                                    +SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[SNew(STextBlock).Font(Font(10)).ColorAndOpacity(Text)
                                        .Text(FText::FromString(Entry->Message.Replace(TEXT("\r"),TEXT(" ")).Replace(TEXT("\n"),TEXT(" "))+(Entry->bTruncated?TEXT(" [truncated]"):TEXT(""))))
                                        .OverflowPolicy(ETextOverflowPolicy::Ellipsis)]];
                        })
                        .OnSelectionChanged_Lambda([S](TSharedPtr<FStudioLogEntry> Entry,ESelectInfo::Type)
                        {
                            S->SelectedSequence=Entry?Entry->Sequence:0;
                            S->Detail=Entry?StudioLog::Line(*Entry)+TEXT("\nObserved: ")+Entry->ObservedUTC.ToIso8601()+TEXT("\nProject: ")+Entry->ProjectId.ToString()+
                                (Entry->RunId.IsValid()?TEXT("\nRun: ")+Entry->RunId.ToString():TEXT(""))+
                                (Entry->SourceReference.IsEmpty()?TEXT(""):TEXT("\nSource: ")+Entry->SourceReference):FString();
                        })]]
                +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)[SNew(STextBlock).Font(Font(11)).ColorAndOpacity(Muted)
                    .Visibility_Lambda([S]{return S->Rows.IsEmpty()?EVisibility::HitTestInvisible:EVisibility::Collapsed;})
                    .Text_Lambda([S]{return FText::FromString(S->View.HiddenSequence()?TEXT("View cleared. Show retained restores available entries."):
                        TEXT("No matching entries. Adjust search or filters."));})]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)[SNew(SBox).HeightOverride(108)
            .Visibility_Lambda([S]{return S->Detail.IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
            [SNew(SMultiLineEditableTextBox).Tag(TEXT("LogDetail")).Style(&LogDetailStyle).Font(Font(10)).ForegroundColor(Text).ReadOnlyForegroundColor(Text)
                .IsReadOnly(true).AutoWrapText(true).Text_Lambda([S]{return FText::FromString(S->Detail);})]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0)[SNew(SStudioCommandInput).Model(M)
            .Execute([this](EStudioCommand Command,FString& Response){return ExecuteApplicationCommand(Command,Response);})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,9,0,0)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[Live([this,S]
            {
                return FString::Printf(TEXT("%d visible · %d retained · %llu older dropped  |  %s"),S->Rows.Num(),M->ActivityLog().Num(),
                    static_cast<unsigned long long>(M->ActivityLog().EvictedCount()),S->View.IsFollowing()?TEXT("Following latest"):TEXT("View paused"))+
                    (S->View.IsFollowing()?TEXT(""):FString::Printf(TEXT(" · %llu new %s"),static_cast<unsigned long long>(S->View.PendingCount(M->ActivityLog())),S->View.PendingCount(M->ActivityLog())==1?TEXT("event"):TEXT("events")));
            },9,Muted)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,6,0,0)[SNew(STextBlock).Tag(TEXT("LogExportNotice")).Font(Font(9)).ColorAndOpacity(Cyan).AutoWrapText(true)
            .Text_Lambda([S]{return FText::FromString(S->ExportNotice);}).ToolTipText_Lambda([S]{return FText::FromString(S->ExportPath);})]];
}

TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> SStudioWorkspace::ActiveMonitorHistory() const
{return bMonitorProbe?ProbeMonitor->History():bMonitorResidual?M->ResidualHistory():M->MonitorHistory();}
const FStudioMonitorSettings& SStudioWorkspace::ActiveMonitorSettings() const
{return bMonitorProbe?ProbeMonitor->Settings():bMonitorResidual?M->Project.Residual.Chart:M->Project.Monitor;}
bool SStudioWorkspace::IsActiveMonitorLoading() const
{return bMonitorProbe?ProbeMonitor->IsBusy():bMonitorResidual?M->IsResidualLoading():M->IsMonitorLoading();}
void SStudioWorkspace::UpdateActiveMonitor(const FStudioMonitorSettings& Settings)
{if(bMonitorProbe)ProbeMonitor->UpdateSettings(Settings);else if(bMonitorResidual)M->UpdateResidualSettings(Settings);else M->UpdateMonitorSettings(Settings);}
void SStudioWorkspace::PickResidualLog(bool bLocate)
{
    FString Path;
    if(!StudioFileDialog::ResidualLog(M->Project.Residual.Path,Path))return;
    if(M->RequestResidualLog(Path,bLocate)){bMonitorResidual=true;bMonitorProbe=false;}
}

// THESIS: Trace residual values back to their original completed solver log.
// OWN-WORLD: Native blue-black Slate workspace, existing source inspector and fine chart axes.
// STORY: Import, select first-initial or last-final values, inspect source lines, export and reopen.
// FIRST VIEWPORT: One chart and source selector in Monitors; separate residual and force cards in Solve.
// FORM: Local Operate extension; no additional workspace navigation or implied run association.
// FINISH: Verified original data, both desktop sizes, fresh finish review and recorded behavior.
void SStudioWorkspace::RevealProbeHistoryFrame(int32 Sample)
{
    ProbeMonitor->Tick(M->Project.Id,M->Solver,M->InspectionObjects);
    const auto H=ProbeMonitor->History();
    if(!H||!H->ProbeHistory||!H->ProbeHistory->Frames.IsValidIndex(Sample)||M->IsProjectOpenPending()||M->IsRecordingLoadPending())return;
    const int32 Ordinal=H->ProbeHistory->Frames[Sample].Ordinal;
    M->Scrub(M->Frames.Num()>1?double(Ordinal)/(M->Frames.Num()-1):0.);
    M->SelectInspectionObject(H->ProbeHistory->Probe.Id);Navigate(EStudioWorkspace::Solve);
    bInspectionOpen=true;bPerformanceOpen=false;M->bActivityLogExpanded=false;
}
TSharedRef<SWidget> SStudioWorkspace::MonitorWorkspace()
{
    auto Binding=MakeShared<FStudioMonitorChartBinding>();
    Binding->Source=[this]{return ActiveMonitorHistory();};
    Binding->Settings=[this]() -> const FStudioMonitorSettings& {return ActiveMonitorSettings();};
    Binding->Revision=[this]{return bMonitorProbe?ProbeMonitor->Revision:bMonitorResidual?M->ResidualRevision:M->MonitorRevision;};
    Binding->Update=[this](const auto& Value){UpdateActiveMonitor(Value);};
    Binding->Loading=[this]{return IsActiveMonitorLoading();};
    Binding->SelectSample=[this](int32 Index){ProbeHistorySample=Index;};
    Binding->RevealSample=[this](int32 Index){RevealProbeHistoryFrame(Index);};
    auto Source=SNew(SStudioMenuButton).Tag(TEXT("MonitorSource")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,7))
        .IsEnabled_Lambda([this]{return !M->IsMonitorLoading()&&!M->IsResidualLoading()&&!M->IsProjectOpenPending();})
        .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
        {
            auto Rows=SNew(SVerticalBox);
            for(const auto& E:StudioHistories::Installed())
            {
                auto Pick=Button(E.Title,TEXT("chart"),[this,Id=E.Id]{FSlateApplication::Get().DismissAllMenus();bMonitorResidual=false;bMonitorProbe=false;M->RequestMonitorHistory(Id);});
                Pick->SetTag(FName(*(TEXT("MonitorSource_")+E.Id)));Rows->AddSlot().AutoHeight().Padding(0,3)[Pick];
            }
            if(!M->Project.Residual.Path.IsEmpty())
            {
                auto Pick=Button(TEXT("Residuals · ")+FPaths::GetCleanFilename(M->Project.Residual.Path),TEXT("chart"),[this]{bMonitorResidual=true;bMonitorProbe=false;FSlateApplication::Get().DismissAllMenus();});
                Pick->SetTag(TEXT("MonitorSource_Residual"));Rows->AddSlot().AutoHeight().Padding(0,3)[Pick];
            }
            Rows->AddSlot().AutoHeight().Padding(0,12,0,5)[Label(TEXT("Saved probes · current recording"),10,Muted)];
            if(!M->InspectionObjects.Probes.ContainsByPredicate([this](const auto& P){return P.Source==M->InspectionSource();}))
                Rows->AddSlot().AutoHeight().Padding(0,3)[Live([]{return TEXT("Create a probe in Solve using the Probe tool, then choose it here.");},9,Muted,true)];
            for(const auto& Probe:M->InspectionObjects.Probes)
            {
                if(!(Probe.Source==M->InspectionSource()))continue;
                auto Pick=Button(Probe.Name,TEXT("probe"),[this,Id=Probe.Id]
                {
                    FSlateApplication::Get().DismissAllMenus();const auto* P=M->FindProbe(Id);
                    if(!P||!(P->Source==M->InspectionSource())||M->IsRecordingLoadPending())return;
                    FStudioProbeHistoryRequest R;R.ProjectId=M->Project.Id;R.Probe=*P;R.Source=M->Solver;
                    R.Scalar=P->Field.IsEmpty()?M->ActiveScalar().Id:P->Field;R.LastOrdinal=M->Solver->FrameCount()-1;
                    ProbeMonitor->Select(MoveTemp(R));bMonitorProbe=true;bMonitorResidual=false;ProbeHistorySample=INDEX_NONE;
                });
                Pick->SetTag(FName(*(TEXT("MonitorProbe_")+Probe.Id.ToString(EGuidFormats::Digits))));
                Rows->AddSlot().AutoHeight().Padding(0,3)[Pick];
            }
            auto Import=Button(TEXT("Import residual log…"),TEXT("import"),[this]{FSlateApplication::Get().DismissAllMenus();PickResidualLog(false);});
            Import->SetTag(TEXT("MonitorImportResidual"));Rows->AddSlot().AutoHeight().Padding(0,10,0,3)[Import];
            auto Locate=Button(TEXT("Locate exact residual source…"),TEXT("folder"),[this]{FSlateApplication::Get().DismissAllMenus();PickResidualLog(true);});
            Locate->SetTag(TEXT("MonitorLocateResidual"));Locate->SetEnabled(!M->Project.Residual.Path.IsEmpty());Rows->AddSlot().AutoHeight().Padding(0,3)[Locate];
            return SNew(SBox).WidthOverride(440).MaxDesiredHeight(440)[SNew(SBorder).BorderImage(&PanelBrush).Padding(10)[SNew(SScrollBox)+SScrollBox::Slot()[Rows]]];
        }).ButtonContent()[Label(TEXT("Choose history"),10,Cyan)];
    auto Cancel=Button(TEXT("Cancel loading"),TEXT("stop"),[this]{if(bMonitorProbe)ProbeMonitor->Cancel();else if(bMonitorResidual)M->CancelResidualLog();else M->CancelMonitorHistory();});Cancel->SetTag(TEXT("MonitorCancel"));
    Cancel->SetEnabled(TAttribute<bool>::CreateLambda([this]{return IsActiveMonitorLoading();}));
    auto Remove=Button(TEXT("Remove"),TEXT("stop"),[this]{if(bMonitorProbe)ProbeMonitor->Clear();else if(bMonitorResidual)M->ClearResidualLog();else M->ClearMonitorHistory();});Remove->SetTag(TEXT("MonitorRemove"));
    Remove->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !M->IsProjectOpenPending()&&(bMonitorProbe?ProbeMonitor->Selection().IsSet():!ActiveMonitorSettings().HistoryId.IsEmpty());}));
    auto Fit=Button(TEXT("Fit time"),TEXT("fit"),[this]{auto S=ActiveMonitorSettings();S.bManualTime=false;UpdateActiveMonitor(S);});Fit->SetTag(TEXT("MonitorFit"));
    Fit->SetEnabled(TAttribute<bool>::CreateLambda([this]{return ActiveMonitorHistory().IsValid()&&!IsActiveMonitorLoading();}));
    auto Expand=Button(TEXT("Expand / Restore"),TEXT("expand"),[this]{bMonitorExpanded=!bMonitorExpanded;});Expand->SetTag(TEXT("MonitorExpand"));
    Expand->SetToolTipText(FText::FromString(TEXT("Expand the chart within Monitors. The source and series choices are retained.")));
    auto Export=Button(TEXT("Export history"),TEXT("export"),[this]
    {
        const auto H=ActiveMonitorHistory();if(!H||MonitorExport->IsBusy())return;
        const auto Settings=ActiveMonitorSettings();FString Path;
        MonitorExportProject=M->Project.Id;MonitorExportHistory=H;MonitorExportPath.Empty();MonitorExportSeries=FString::Join(Settings.Series,TEXT(", "));
        if(!StudioFileDialog::ProbeCSV(TEXT("history-" )+H->Id+TEXT(".csv"),Path)){MonitorExportNotice=TEXT("CSV export cancelled.");return;}
        if(MonitorExport->Start(H,Settings,Path)){MonitorExportPath=Path;MonitorExportNotice=H->ProbeHistory?TEXT("Writing frozen probe samples…"):TEXT("Writing original history rows…");}
    });Export->SetTag(TEXT("MonitorExport"));
    Export->SetEnabled(TAttribute<bool>::CreateLambda([this]{return ActiveMonitorHistory().IsValid()&&!ActiveMonitorSettings().Series.IsEmpty()&&!MonitorExport->IsBusy();}));
    auto ClearSeries=Button(TEXT("Clear series"),TEXT("stop"),[this]{auto S=ActiveMonitorSettings();S.Series.Reset();UpdateActiveMonitor(S);});
    ClearSeries->SetTag(TEXT("MonitorClearSeries"));ClearSeries->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !IsActiveMonitorLoading()&&ActiveMonitorHistory().IsValid()&&!ActiveMonitorSettings().Series.IsEmpty();}));
    auto SourceTitle=Live([this]
        {const auto H=ActiveMonitorHistory();return H?H->Title:bMonitorProbe&&ProbeMonitor->Selection().IsSet()?ProbeMonitor->Selection()->Probe.Name+TEXT(" · ")+ProbeMonitor->Selection()->Scalar:bMonitorResidual&&!M->Project.Residual.Path.IsEmpty()?FPaths::GetCleanFilename(M->Project.Residual.Path):TEXT("No history selected.");},10,Text,true);
    SourceTitle->SetWrapTextAt(284);
    auto OpenSource=Button(TEXT("Open published source"),TEXT("export"),[this]
        {const auto H=ActiveMonitorHistory();if(H&&H->SourceURL.StartsWith(TEXT("https://")))FPlatformProcess::LaunchURL(*H->SourceURL,nullptr,nullptr);});
    OpenSource->SetVisibility(TAttribute<EVisibility>::CreateLambda([this]{const auto H=ActiveMonitorHistory();return H&&H->SourceURL.StartsWith(TEXT("https://"))?EVisibility::Visible:EVisibility::Collapsed;}));
    auto ExportStatus=Live([this]{return MonitorExportNotice;},9,Amber,true);
    ExportStatus->SetToolTipText(TAttribute<FText>::CreateLambda([this]{return FText::FromString(MonitorExportPath);}));
    auto FrameInput=[this](bool First) -> TSharedRef<SWidget>
    {
        return SNew(SNumericEntryBox<int32>).Tag(First?TEXT("ProbeHistoryFirst"):TEXT("ProbeHistoryLast"))
            .Font(Font(10)).EditableTextBoxStyle(&InputStyle()).MinValue(1).AllowSpin(false)
            .IsEnabled_Lambda([this]{return ProbeMonitor->Selection().IsSet()&&!ProbeMonitor->IsBusy();})
            .Value_Lambda([this,First]() -> TOptional<int32>
                {const auto& R=ProbeMonitor->Selection();return R.IsSet()?TOptional<int32>((First?R->FirstOrdinal:R->LastOrdinal)+1):TOptional<int32>();})
            .OnValueCommitted_Lambda([this,First](int32 Value,ETextCommit::Type)
                {const auto& R=ProbeMonitor->Selection();if(R.IsSet()&&Value>=1)ProbeMonitor->SetRange(First?Value-1:R->FirstOrdinal,First?R->LastOrdinal:Value-1);});
    };
    auto Generate=Button(TEXT("Generate history"),TEXT("run"),[this]{ProbeMonitor->Generate();});Generate->SetTag(TEXT("ProbeHistoryGenerate"));
    Generate->SetEnabled(TAttribute<bool>::CreateLambda([this]{return ProbeMonitor->Selection().IsSet()&&!ProbeMonitor->IsBusy()&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();}));
    auto ProbeControls=SNew(SBox).Visibility_Lambda([this]{return bMonitorProbe?EVisibility::Visible:EVisibility::Collapsed;})
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]
                {const auto& R=ProbeMonitor->Selection();return R.IsSet()?R->Probe.Method==EStudioProbeMethod::OriginalPoint?
                    TEXT("Exact original point ID · ")+FString::Printf(TEXT("%lld"),R->Probe.PointId.GetValue()):
                    TEXT("Sample saved positions using the recording's available interpolation."):TEXT("Choose a saved probe to generate its history.");},9,Muted,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Row(TEXT("First frame"),FrameInput(true),140)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Last frame"),FrameInput(false),140)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]
                {const auto& R=ProbeMonitor->Selection();return R.IsSet()?FString::Printf(TEXT("Frames 1–%d, inclusive · field: %s"),R->Source->FrameCount(),*R->Scalar):FString();},9,Muted,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Generate]];
    auto Controls=SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("History source"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Source]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SourceTitle]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[Live([this]
        {const auto H=ActiveMonitorHistory();return bMonitorProbe?TEXT("Samples from this recording at saved probe locations. History generation does not change playback."):H?(H->FieldRecordingId.IsSet()&&*H->FieldRecordingId==M->Project.Dataset?
            TEXT("Source declares an association with the current recording."):TEXT("Independent run. Its time and values are not synchronized to the flow recording.")):
            TEXT("Histories keep their own source, time and normalization.");},9,Amber,true)]
        +SVerticalBox::Slot().AutoHeight()[ProbeControls]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[Cancel]+SHorizontalBox::Slot().FillWidth(1)[Remove]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Live([this]{return bMonitorProbe?(ProbeMonitor->IsBusy()?FString::Printf(TEXT("Sampling %d / %d original frames…"),ProbeMonitor->CompletedFrames(),ProbeMonitor->TotalFrames()):ProbeMonitor->Notice):bMonitorResidual?M->ResidualNotice:M->MonitorNotice;},9,Amber,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SNew(SBox).Visibility_Lambda([this]{return bMonitorResidual?EVisibility::Visible:EVisibility::Collapsed;})
            [Live([]{return TEXT("First initial: first solve per time block. Last final: last solve per time block. No convergence threshold is assumed. To restore a moved source, choose Locate exact residual source.");},9,Muted,true)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Series"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[Live([this]{return bMonitorProbe?TEXT("Select up to 16 positions. Every selected position retains all sampled frames."):TEXT("One unit per value axis. Clear the selection to choose another unit.");},9,Muted,true)]
        +SVerticalBox::Slot().AutoHeight()[SAssignNew(MonitorSeriesRows,SVerticalBox)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,6)[ClearSeries]
        +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,6)[Label(TEXT("Provenance & normalization"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]
        {
            const auto H=ActiveMonitorHistory();if(!H)return FString(TEXT("Select a history to view its provenance."));
            FString T=H->TimeNote+TEXT("\n");
            if(H->ProbeHistory)
            {
                const auto& R=*H->ProbeHistory;T+=TEXT("\n")+R.SourceTitle+TEXT("\n")+R.Method+TEXT("\nSource: ")+R.Identity->Dataset+
                    TEXT("\nMetadata SHA-256: ")+R.Identity->MetadataSHA256+
                    (R.Identity->ReconstructionSHA256.IsEmpty()?FString():TEXT("\nReconstruction SHA-256: ")+R.Identity->ReconstructionSHA256);
            }
            if(H->bResiduals)T+=H->SourcePath+TEXT("\n\nSHA-256: ")+H->SourceSHA256;TArray<FString> Keys;H->ReferenceValues.GetKeys(Keys);Keys.Sort();
            for(const auto& Key:Keys)T+=FString::Printf(TEXT("\n%s = %.17g"),*Key,H->ReferenceValues[Key]);
            for(const auto& Id:ActiveMonitorSettings().Series)if(const auto* C=H->FindColumn(Id))
                T+=TEXT("\n\n")+C->Label+TEXT(" · ")+C->Origin+(C->Expression.IsEmpty()?FString():TEXT("\n")+C->Expression);
            for(const auto& Note:H->Limitations)T+=TEXT("\n\n")+Note;
            return T;
        },9,Muted,true)]
        +SVerticalBox::Slot().AutoHeight()[OpenSource];
    return SNew(SBorder).BorderImage(&Background).Padding(18)
    [SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[Label(TEXT("Monitors"),22,Text,true)]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Export]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Fit]
            +SHorizontalBox::Slot().AutoWidth()[Expand]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Live([this]
        {const auto H=ActiveMonitorHistory();return H?H->Title+FString::Printf(TEXT(" · %d %s"),H->Times.Num(),H->ProbeHistory?TEXT("recorded frames"):TEXT("original samples")):TEXT("Choose a published history, a saved probe, or import a completed residual log.");},10,Muted,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[ExportStatus]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)
                [SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
                        +SHorizontalBox::Slot().FillWidth(1)[Live([this]
                        {const auto H=ActiveMonitorHistory();FString Unit;if(H)for(const auto& Id:ActiveMonitorSettings().Series)if(const auto* C=H->FindColumn(Id)){Unit=C->Unit;break;}
                            return Unit.IsEmpty()?TEXT("No series selected"):TEXT("Value (")+Unit+TEXT(")")+(ActiveMonitorSettings().bLogY?TEXT(" · logarithmic"):TEXT(" · linear"));},11,Text,true)]
                        +SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox).Tag(TEXT("MonitorLog"))
                            .IsEnabled_Lambda([this]{return ActiveMonitorHistory().IsValid()&&!IsActiveMonitorLoading();})
                            .IsChecked_Lambda([this]{return ActiveMonitorSettings().bLogY?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                            .OnCheckStateChanged_Lambda([this](ECheckBoxState State){auto S=ActiveMonitorSettings();S.bLogY=State==ECheckBoxState::Checked;UpdateActiveMonitor(S);})
                            [Label(TEXT("Log scale"),10)]]]
                    +SVerticalBox::Slot().FillHeight(1)[SNew(SStudioMonitorChart).Model(M).Binding(Binding).Compact(false).Residual_Lambda([this]{return bMonitorResidual;}).Tag(TEXT("MonitorChart"))]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)[SNew(SBox).Visibility_Lambda([this]{return bMonitorProbe?EVisibility::Visible:EVisibility::Collapsed;})
                        [SNew(SHorizontalBox)
                            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Live([this]
                            {
                                const auto H=ProbeMonitor->History();if(!H||!H->ProbeHistory||!H->ProbeHistory->Frames.IsValidIndex(ProbeHistorySample))return FString(TEXT("Hover or use ↑ / ↓ on the chart to select a recorded frame."));
                                const auto& F=H->ProbeHistory->Frames[ProbeHistorySample];return FString::Printf(TEXT("Frame %d · source step %d · %.9g s"),F.Ordinal+1,F.Frame.Index,F.Frame.Time);
                            },9,Muted,true)]
                            +SHorizontalBox::Slot().AutoWidth().Padding(10,0,0,0)[SNew(SButton).Tag(TEXT("ProbeHistoryReveal")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
                                .IsEnabled_Lambda([this]{const auto H=ProbeMonitor->History();return H&&H->ProbeHistory&&H->ProbeHistory->Frames.IsValidIndex(ProbeHistorySample)&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();})
                                .OnClicked_Lambda([this]{RevealProbeHistoryFrame(ProbeHistorySample);return FReply::Handled();})[Label(TEXT("Show frame in Solve"),9)]]]]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)[Live([this]
                    {return ActiveMonitorSettings().bLogY?TEXT("Log scale omits nonpositive samples and breaks the trace at gaps. Values are unchanged."):
                        bMonitorProbe?TEXT("Values at recorded times; spatial gaps stay empty. Pixel reduction preserves extrema; no temporal smoothing."):
                        TEXT("Original samples; pixel reduction preserves extrema. No smoothing or time offset.");},9,Muted,true)]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,6,0,0)[Live([]{return TEXT("Hover: exact sample · Scroll: zoom time · Drag: pan · + / −: zoom · ← / →: pan · Home: fit");},9,Muted,true)]]]
            +SHorizontalBox::Slot().AutoWidth().Padding(14,0,0,0)[SNew(SBox).WidthOverride(326)
                .Visibility_Lambda([this]{return bMonitorExpanded?EVisibility::Collapsed:EVisibility::Visible;})
                [SNew(SBorder).BorderImage(&PanelBrush).Padding(14)[SNew(SRetainedFormScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                    .NavigationScrollPadding(12)+SScrollBox::Slot()[Controls]]]]]];
}
void SStudioWorkspace::RefreshMonitors()
{
    ProbeMonitor->Tick(M->Project.Id,M->Solver,M->InspectionObjects);
    if(MonitorSessionProject!=M->Project.Id)
    {
        MonitorSessionProject=M->Project.Id;bMonitorProbe=false;ProbeHistorySample=INDEX_NONE;
        bMonitorResidual=M->Project.Monitor.HistoryId.IsEmpty()&&!M->Project.Residual.Path.IsEmpty();
    }
    const auto H=ActiveMonitorHistory();
    if(MonitorExportProject!=M->Project.Id||MonitorExportHistory.Pin()!=H){MonitorExportNotice.Empty();MonitorExportPath.Empty();}
    if(MonitorExport)if(const auto R=MonitorExport->Poll();R.IsSet())
        if(MonitorExportProject==M->Project.Id&&MonitorExportHistory.Pin()==H)
            MonitorExportNotice=R->bSuccess?FString::Printf(TEXT("Exported %d %s · %s · %s"),R->Samples,H&&H->ProbeHistory?TEXT("probe sample rows"):TEXT("original samples"),*MonitorExportSeries,*FPaths::GetCleanFilename(R->Path)):TEXT("History export failed: ")+R->Error;
    if(!MonitorSeriesRows||MonitorSeriesHistory==H)return;
    MonitorSeriesHistory=H;ProbeHistorySample=INDEX_NONE;MonitorSeriesRows->ClearChildren();if(!H)return;
    for(const auto& Column:H->Columns)
    {
        const FString Id=Column.Id,Unit=Column.Unit;
        MonitorSeriesRows->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SCheckBox).Tag(FName(*(TEXT("MonitorSeries_")+Id)))
            .IsEnabled_Lambda([this,H,Id,Unit]
            {
                if(ActiveMonitorHistory()!=H||IsActiveMonitorLoading())return false;
                if(!ActiveMonitorSettings().Series.Contains(Id)&&ActiveMonitorSettings().Series.Num()>=16)return false;
                for(const auto& Selected:ActiveMonitorSettings().Series)if(const auto* C=H->FindColumn(Selected))if(C->Unit!=Unit)return false;
                return true;
            })
            .IsChecked_Lambda([this,Id]{return ActiveMonitorSettings().Series.Contains(Id)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([this,Id](ECheckBoxState State)
            {auto S=ActiveMonitorSettings();if(State==ECheckBoxState::Checked)S.Series.AddUnique(Id);else S.Series.Remove(Id);UpdateActiveMonitor(S);})
            [SNew(STextBlock).Text(FText::FromString(Column.Label+TEXT(" (")+Unit+TEXT(")"))).Font(Font(10)).ColorAndOpacity(Text).AutoWrapText(true)]];
    }
}
// THESIS: Explicit toolbar mode tests job control while retaining an honest recorded view.
// OWN-WORLD: Reference blue-black Slate shell, compact CoreStyle text and existing form controls.
// STORY: Select the harness backend, run/pause/step/stop, inspect acknowledgements, save history.
// FIRST VIEWPORT: Mode and current job state at the top of the inspector; camera and flow stay live.
// FORM: Local Operate extension. No new visual identity or decorative progress for a control test.
// FINISH: unreviewed and undocumented is unfinished; finish review, verdict, and DESIGN.md follow.
TSharedRef<SWidget> SStudioWorkspace::JobControls()
{
    auto Modes=SNew(SHorizontalBox);
    for(bool Harness:{false,true})
    {
        auto Choice=Button(Harness?TEXT("Control harness"):TEXT("Replay"),Harness?TEXT("settings"):TEXT("run"),[this,Harness]{M->SetControlHarness(Harness);});
        Choice->SetEnabled(TAttribute<bool>::CreateLambda([this,Harness]{return !M->IsProjectOpenPending()&&(!M->HasActiveJob()||M->Project.bControlHarness==Harness);}));
        Choice->SetToolTipText(FText::FromString(Harness?TEXT("Select the deterministic control harness as the case backend. No CFD is computed."):TEXT("Use the toolbar to control recording playback.")));
        Modes->AddSlot().FillWidth(Harness?1.65f:1.f).Padding(0,0,4,0)[Choice];
    }
    auto Checkpoint=Button(TEXT("Checkpoint test"),TEXT("save"),[this]{M->Control(EStudioJobCommand::Checkpoint);});
    Checkpoint->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanControl(EStudioJobCommand::Checkpoint);}));
    auto Reconnect=Button(TEXT("Reconnect"),TEXT("run"),[this]{M->Control(EStudioJobCommand::Reconnect);});
    Reconnect->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanControl(EStudioJobCommand::Reconnect);}));
    return Section(TEXT("Toolbar controls"),SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[Modes]
        +SVerticalBox::Slot().AutoHeight().Padding(0,8)[Live([this]{return M->Project.bControlHarness?TEXT("Selected: Control harness"):TEXT("Selected: Recorded replay");},10,Cyan)]
        +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([this]{return M->Project.bControlHarness?EVisibility::Visible:EVisibility::Collapsed;})
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(TEXT("Tests job commands only. The viewport shows independent recorded CFD."))).Font(Font(9)).ColorAndOpacity(Amber).AutoWrapText(true)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,8)[Row(TEXT("Job state"),Live([this]{return StudioJobs::StateName(M->Job().State());},10,Cyan))]
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text_Lambda([this]{return FText::FromString(M->Project.Draft.Setup.BackendId!=M->Job().Capabilities().BackendId?TEXT("Select Control harness again to set the case backend before Run."):M->Job().Notice());}).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,8)[Row(TEXT("Elapsed wall time"),Live([this]{const auto* H=M->CurrentJobHistory();return H?FString::Printf(TEXT("%.1f s"),H->ElapsedSeconds):TEXT("—");},9))]
                +SVerticalBox::Slot().AutoHeight()[Row(TEXT("Step replies"),Live([this]{return FString::Printf(TEXT("%llu"),M->Job().CompletedStepCommands());},9))]
                +SVerticalBox::Slot().AutoHeight().Padding(0,8)[Row(TEXT("Checkpoint replies"),Live([this]{return FString::Printf(TEXT("%llu"),M->Job().CompletedCheckpointCommands());},9))]
                +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,4,0)[Checkpoint]
                    +SHorizontalBox::Slot().AutoWidth()[Reconnect]]
                +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(SStudioMenuButton).ButtonStyle(&ButtonStyle()).OnGetMenuContent_Lambda([this]
                {
                    auto Menu=SNew(SVerticalBox);
                    for(auto State:{EStudioJobState::Completed,EStudioJobState::Disconnected,EStudioJobState::Failed})
                    {
                        const FString Name=State==EStudioJobState::Completed?TEXT("Finish control test"):State==EStudioJobState::Disconnected?TEXT("Simulate disconnect"):TEXT("Simulate failure");
                        auto Action=Button(Name,TEXT("settings"),[this,State]{FSlateApplication::Get().DismissAllMenus();M->SimulateJobEvent(State);});
                        Action->SetEnabled(M->CanSimulateJobEvent(State));Menu->AddSlot().AutoHeight().Padding(3)[Action];
                    }
                    return Menu;
                }).ButtonContent()[Label(TEXT("Test events"),9)]]
            ]]);
}
// THESIS: Give each Solve setting one visible owner without losing an unfinished edit.
// OWN-WORLD: Reference blue-black native Slate inspector with compact cyan category tabs.
// STORY: Review setup, source/case physics and assignments; inspect the recorded field in Display.
// FIRST VIEWPORT: Display controls sit in the right inspector; the flow has no floating duplicate.
// FORM: Local Operate extension. Categories and cached drafts are session UI state, not CFD data.
// FINISH: unreviewed and undocumented is unfinished; finish review, verdict and DESIGN.md follow.
TSharedRef<SWidget> SStudioWorkspace::CachedDisplayMenu(FName Kind,TFunction<TSharedRef<SWidget>()> Build)
{
    if(DisplayMenuProject!=M->Project.Id||DisplayMenuSource.Pin()!=M->Solver||DisplayMenuScalar!=M->ActiveScalar().Id)
    {
        DisplayMenuDrafts.Empty();DisplayMenuProject=M->Project.Id;DisplayMenuSource=M->Solver;DisplayMenuScalar=M->ActiveScalar().Id;
        DisplayMenuRevision=M->RenderIntentRevision;
    }
    // These older forms store local range drafts; external view changes require fresh values.
    if(DisplayMenuRevision!=M->RenderIntentRevision)
    {DisplayMenuDrafts.Remove(TEXT("Color"));DisplayMenuDrafts.Remove(TEXT("Volume"));DisplayMenuRevision=M->RenderIntentRevision;}
    if(const auto* Existing=DisplayMenuDrafts.Find(Kind))return Existing->ToSharedRef();
    const auto Content=Build();DisplayMenuDrafts.Add(Kind,Content);return Content;
}
TSharedRef<SWidget> SStudioWorkspace::Settings()
{
    auto Controls=SNew(SVerticalBox);
    Controls->AddSlot().AutoHeight()[Section(TEXT("Flow conditions"),
        SAssignNew(FlowConditions,SStudioFlowConditions).Model(M).OnMaterials_Lambda([this]{Navigate(EStudioWorkspace::Materials);}))];
    Controls->AddSlot().AutoHeight()[RunSettingsControls()];
    Controls->AddSlot().AutoHeight()[JobControls()];
    Controls->AddSlot().AutoHeight()[Section(TEXT("Recorded dataset"),SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,4)[Live([this]{return M->Solver->Descriptor().Title;},10,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Choose or import recordings in Results."),9,Muted)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SStudioMenuButton).Tag(TEXT("SurfaceSettings"))
            .Visibility_Lambda([this]{return M->Solver->Descriptor().bSourcePoints?EVisibility::Visible:EVisibility::Collapsed;})
            .ButtonStyle(&ButtonStyle()).OnGetMenuContent(this,&SStudioWorkspace::SurfaceMenu)
            .ButtonContent()[Live([this]{if(M->Solver->Descriptor().SpatialDimensions==3)return M->Solver->VolumeReconstruction()?TEXT("Volume reconstruction · loaded"):TEXT("Volume reconstruction…");return M->Solver->Reconstruction()?TEXT("Surface reconstruction · loaded"):TEXT("Surface reconstruction…");},9)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Source samples"),Live([this]{const auto& D=M->Solver->Descriptor();return D.bSourcePoints?FString::Printf(TEXT("%d points · no cells"),D.NodeCount):FString::Printf(TEXT("%d mesh nodes"),D.NodeCount);}))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Source frames"),Live([this]{return FString::Printf(TEXT("%d snapshots"),M->Solver->FrameCount());}))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Source duration"),Live([this]{return FString::Printf(TEXT("%.4f s"),M->Frames.Last().Time-M->Frames[0].Time);}))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Playback length"),Live([this]{return FString::Printf(TEXT("%.1f s"),(M->Solver->FrameCount()-1)*FStudioModel::PlaybackInterval/M->PlaybackRate);}))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]{return M->Solver->Descriptor().bSourcePoints?M->Solver->Descriptor().FieldNote:TEXT("Recorded 2D fields repeat along Y; this adds no spanwise flow. Playback speed does not extend source duration.");},9,Amber,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Replay speed"),Number([this]{return M->PlaybackRate;},[this](double V){M->PlaybackRate=V;},.25,4.,TEXT("×"),.25))]
        +SVerticalBox::Slot().AutoHeight()[Check(TEXT("Loop recording"),[this]{return M->bLoopPlayback;},[this](bool V){M->bLoopPlayback=V;})])];
    Controls->AddSlot().AutoHeight()[Section(TEXT("Playback status"),SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Row(TEXT("Status"),Live([this]{return M->StatusText();},10,Green))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Row(TEXT("Recorded frames"),Live([this]{return FString::FromInt(M->Frames.Num());}))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Row(TEXT("Source elapsed time"),Live([this]{return FString::Printf(TEXT("%.4f s"),M->Frames[M->PlaybackFrame].Time);}))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Row(TEXT("Wall time"),Live([this]{return FString::Printf(TEXT("%.1f s"),M->WallSeconds);}))]
        +SVerticalBox::Slot().AutoHeight()[Row(TEXT("Data adapter"),Label(TEXT("Recorded fields"),10,Amber))])];
    auto Physics=SNew(SVerticalBox);
    Physics->AddSlot().AutoHeight()[Section(TEXT("Source conditions · read only"),SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Reynolds number"),Live([this]{return M->Solver->Descriptor().bSourcePoints?TEXT("See source reference"):TEXT("Not supplied");},9,Muted))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Mach number"),Live([this]{return M->Solver->Descriptor().bSourcePoints?TEXT("See source reference"):TEXT("Not supplied");},9,Muted))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Field coordinates"),Live([this]{return M->Solver->Descriptor().SpatialDimensions==2?TEXT("Source 2D → X/Z"):TEXT("Source XYZ → X/Z/Y");},9))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Source solver"),Label(TEXT("See source reference"),9,Muted))]
        +SVerticalBox::Slot().AutoHeight()[Row(TEXT("Stored fields"),Live([this]{return M->Solver->Descriptor().bSourcePoints?FString::Printf(TEXT("%d source arrays"),M->Solver->Descriptor().Scalars.Num()):TEXT("Velocity, p, density");},9))])];
    Physics->AddSlot().AutoHeight()[Section(TEXT("Case physics · saved draft"),SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]{return TEXT("Case: ")+M->Project.Draft.Name;},10,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Backend"),Live([this]{const auto& V=M->Project.Draft.Setup.BackendId;return V.IsEmpty()?TEXT("Not selected"):V;},9))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Collision model"),Live([this]{const auto& V=M->Project.Draft.Setup.CollisionModel;return V.IsEmpty()?TEXT("Not selected"):V;},9))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Turbulence"),Live([this]{const auto& V=M->Project.Draft.Setup.TurbulenceModel;return V.IsEmpty()?TEXT("Not selected"):V;},9))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Thermal model"),Live([this]{return M->Project.Draft.Setup.bThermal?TEXT("Enabled in draft"):TEXT("Disabled in draft");},9))]
        +SVerticalBox::Slot().AutoHeight()[Live([]{return TEXT("These are saved case settings. Recorded fields keep their original physics. Model authoring requires backend capabilities.");},9,Muted,true)])];
    auto Boundaries=SNew(SVerticalBox);
    Boundaries->AddSlot().AutoHeight()[Section(TEXT("Boundary conditions · saved draft"),SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]{return TEXT("Case: ")+M->Project.Draft.Name;},10,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]{return FString::Printf(TEXT("%d saved condition%s"),M->Project.Draft.Boundaries.Num(),M->Project.Draft.Boundaries.Num()==1?TEXT(""):TEXT("s"));},10,Cyan)]
        +SVerticalBox::Slot().AutoHeight()[Live([]{return TEXT("Assignments below belong to the saved case. Recording boundary conditions are not supplied by this data adapter.");},9,Muted,true)])];
    const auto BoundaryName=[](EStudioBoundaryType Type)->FString{return StudioBoundaries::TypeName(Type);};
    auto Faces=SNew(SVerticalBox);
    const TCHAR* FaceNames[]={TEXT("−X"),TEXT("+X"),TEXT("−Y"),TEXT("+Y"),TEXT("−Z"),TEXT("+Z")};
    for(int32 I=0;I<6;++I)Faces->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(FaceNames[I],Live([this,I,BoundaryName]
    {
        const auto& D=M->Project.Draft;if(!D.Domain.Faces.IsValidIndex(I))return FString(TEXT("Unavailable"));
        const auto* B=D.Boundaries.FindByPredicate([&](const auto& V){return V.TargetId==D.Domain.Faces[I];});
        const FString Name=D.Domain.FaceNames.IsValidIndex(I)?D.Domain.FaceNames[I]:FString();
        return Name+TEXT(" · ")+(B?BoundaryName(B->Type):FString(TEXT("Unassigned")));
    },9),190)];
    Boundaries->AddSlot().AutoHeight()[Section(TEXT("Domain faces"),Faces)];
    Boundaries->AddSlot().AutoHeight()[Section(TEXT("Surface patches"),Live([this,BoundaryName]
    {
        FString Result;for(const auto& G:M->Project.Draft.Geometry)for(const auto& P:G.Patches)
        {
            const auto* B=M->Project.Draft.Boundaries.FindByPredicate([&](const auto& V){return V.TargetId==P.Id;});
            if(!Result.IsEmpty())Result+=TEXT("\n");Result+=G.Name+TEXT(" / ")+P.Name+TEXT(": ")+(B?BoundaryName(B->Type):TEXT("Unassigned"));
        }
        return Result.IsEmpty()?TEXT("No imported surface patches in this case."):Result;
    },9,Muted,true))];
    auto Tabs=SNew(SHorizontalBox);
    const TCHAR* Names[]={TEXT("Setup"),TEXT("Physics"),TEXT("BCs"),TEXT("Display")};
    for(int32 I=0;I<4;++I)Tabs->AddSlot().FillWidth(1)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("InspectorTab%d"),I)))
        .ButtonStyle(&NavigationStyle()).ContentPadding(FMargin(2,12)).HAlign(HAlign_Center)
        .ToolTipText(FText::FromString(FString(Names[I])+TEXT(" inspector")))
        .OnClicked_Lambda([this,I]{FSlateApplication::Get().DismissAllMenus();M->InspectorTab=I;M->SaveSession();return FReply::Handled();})
        [SNew(STextBlock).Font(Font(10)).Text(FText::FromString(Names[I])).ColorAndOpacity_Lambda([this,I]{return M->InspectorTab==I?Cyan:Muted;})]];
    const auto Scroll=[](const TSharedRef<SWidget>& Content,FName Tag)->TSharedRef<SWidget>
    {return SNew(SRetainedFormScrollBox).Tag(Tag).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12.f)+SScrollBox::Slot()[Content];};
    return SNew(SBorder).Tag(TEXT("SolveInspector")).BorderImage(&PanelBrush).Padding(10,0)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[Tabs]
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(2)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).Padding(0).BorderImage(&White).BorderBackgroundColor_Lambda([this]{return M->InspectorTab==0?Cyan:Panel;})]
                +SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).Padding(0).BorderImage(&White).BorderBackgroundColor_Lambda([this]{return M->InspectorTab==1?Cyan:Panel;})]
                +SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).Padding(0).BorderImage(&White).BorderBackgroundColor_Lambda([this]{return M->InspectorTab==2?Cyan:Panel;})]
                +SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).Padding(0).BorderImage(&White).BorderBackgroundColor_Lambda([this]{return M->InspectorTab==3?Cyan:Panel;})]]]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return M->InspectorTab;})
                +SWidgetSwitcher::Slot()[Scroll(Controls,TEXT("SetupInspectorScroll"))]
                +SWidgetSwitcher::Slot()[Scroll(Physics,TEXT("PhysicsInspectorScroll"))]
                +SWidgetSwitcher::Slot()[Scroll(Boundaries,TEXT("BoundaryInspectorScroll"))]
                +SWidgetSwitcher::Slot()[Scroll(DisplayTools(),TEXT("DisplayInspectorScroll"))]]];
}

struct FStudioRunSettingsState
{
    FGuid Project;
    int64 Revision=-1;
    FStudioRunSettingsEdit Edit;
    bool bConflict=false,bSynchronizing=false;
    FString Notice;
    TWeakPtr<SEditableTextBox> Inputs[FStudioRunSettingsEdit::FieldCount];
    void Synchronize()
    {
        bSynchronizing=true;
        for(int32 Index=0;Index<FStudioRunSettingsEdit::FieldCount;++Index)
            if(auto Input=Inputs[Index].Pin())Input->SetText(FText::FromString(Edit.Values[Index]));
        bSynchronizing=false;
    }
    void Focus(int32 Index=0)
    {
        if(Index>=0&&Index<FStudioRunSettingsEdit::FieldCount)
            if(auto Input=Inputs[Index].Pin())FSlateApplication::Get().SetKeyboardFocus(Input,EFocusCause::Navigation);
    }
};
void SStudioWorkspace::RefreshRunSettings()
{
    if(!RunSettingsState)return;
    auto& State=*RunSettingsState;
    if(State.Project!=M->Project.Id||State.Edit.CaseId!=M->Project.Draft.Id)
    {
        State.Project=M->Project.Id;State.Edit.Reset(M->Project.Draft);State.bConflict=false;State.Notice.Empty();
        State.Revision=M->Project.Draft.Revision;State.Synchronize();return;
    }
    if(State.Revision==M->Project.Draft.Revision)return;
    State.Revision=M->Project.Draft.Revision;
    if(State.Edit.Matches(M->Project.Draft))return;
    if(State.Edit.IsDirty()||State.bConflict)State.bConflict=true;
    else {State.Edit.Reset(M->Project.Draft);State.Synchronize();State.Notice.Empty();}
}
bool SStudioWorkspace::EnsureFlowConditionsResolved()
{
    if(!FlowConditions||!FlowConditions->HasUnapplied())return true;
    if(!EnsurePlacementResolved())return false;
    bInspectionOpen=false;bPerformanceOpen=false;CancelInspectionPlacement();M->bViewportExpanded=false;M->bActivityLogExpanded=false;M->InspectorTab=0;
    Navigate(EStudioWorkspace::Solve);FlowConditions->RequireResolution();return false;
}
bool SStudioWorkspace::EnsureRunSettingsResolved()
{
    RefreshRunSettings();
    if(!RunSettingsState||(!RunSettingsState->bConflict&&!RunSettingsState->Edit.IsDirty()))return true;
    if(!EnsurePlacementResolved())return false;
    RunSettingsState->Notice=M->Notice=RunSettingsSaveGuard;
    bInspectionOpen=false;bPerformanceOpen=false;CancelInspectionPlacement();M->bViewportExpanded=false;M->bActivityLogExpanded=false;M->InspectorTab=0;
    Navigate(EStudioWorkspace::Solve);RunSettingsState->Focus();return false;
}

// THESIS: Author next-run limits without changing the recorded flow or active job.
// OWN-WORLD: Existing compact blue-black Solve inspector, exact fields and cyan actions.
// STORY: Enter stop/output requests, apply once, undo or reopen, then submit the saved case.
// FIRST VIEWPORT: Run parameters lead Setup; the flow and existing navigation remain visible.
// FORM: Local Operate extension. Intervals are solver steps; backend execution is explicit.
// FINISH: Native two-size evidence, scoped finish review and documentation are required.
TSharedRef<SWidget> SStudioWorkspace::RunSettingsControls()
{
    RunSettingsState=MakeShared<FStudioRunSettingsState>();RefreshRunSettings();const auto State=RunSettingsState;
    auto Available=[this]{return !M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();};
    auto Apply=[this,State,Available]
    {
        if(!Available())return;RefreshRunSettings();if(State->bConflict)return;
        if(!M->UpdateRunSettings(State->Edit)){State->Focus(State->Edit.ErrorField);return;}
        State->Edit.Reset(M->Project.Draft);State->Synchronize();State->Revision=M->Project.Draft.Revision;
        State->Notice=TEXT("Run parameters applied. Save to keep this case.");
        ResolveSaveNotice(*M,RunSettingsSaveGuard,State->Notice);
    };
    auto Input=[State,Apply,Available](int32 Index,const TCHAR* Hint)
    {
        auto Field=SNew(SProjectFilterBox).Tag(FName(*FString::Printf(TEXT("RunParameter%d"),Index)))
            .Style(&InputStyle()).Font(Font(10)).Text(FText::FromString(State->Edit.Values[Index]))
            .HintText(FText::FromString(Hint)).SelectAllTextWhenFocused(true).ClearKeyboardFocusOnCommit(false)
            .IsEnabled_Lambda(Available).ToolTipText_Lambda([State,Index]{return FText::FromString(State->Edit.Values[Index]);})
            .OnTextChanged_Lambda([State,Index](const FText& Value)
            {
                if(State->bSynchronizing)return;State->Edit.Values[Index]=Value.ToString();
                State->Edit.Error.Empty();State->Edit.ErrorField=INDEX_NONE;
                if(State->Notice!=RunSettingsSaveGuard)State->Notice.Empty();
            })
            .OnTextCommitted_Lambda([Apply](const FText&,ETextCommit::Type How){if(How==ETextCommit::OnEnter)Apply();});
        State->Inputs[Index]=Field;return Field;
    };
    auto Fields=SNew(SVerticalBox);
    Fields->AddSlot().AutoHeight().Padding(0,0,0,10)[Live([this]
    {return M->HasActiveJob()?TEXT("Next-run case. The active run keeps its submitted settings."):TEXT("Saved case requests. Recorded playback is independent.");},9,Muted,true)];
    const TCHAR* Labels[]={TEXT("Maximum solver steps"),TEXT("Maximum physical time (s)"),TEXT("Output every (solver steps)"),TEXT("Checkpoint every (solver steps)")};
    for(int32 Index=0;Index<FStudioRunSettingsEdit::FieldCount;++Index)
    {
        if(Index==FStudioRunSettingsEdit::CheckpointInterval)
            Fields->AddSlot().AutoHeight().Padding(0,4,0,10)[SNew(SCheckBox).Tag(TEXT("RunCheckpoints")).IsEnabled_Lambda(Available)
                .IsChecked_Lambda([State]{return State->Edit.bCheckpoints?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([State](ECheckBoxState Value)
                {State->Edit.bCheckpoints=Value==ECheckBoxState::Checked;if(State->Notice!=RunSettingsSaveGuard)State->Notice.Empty();})
                [Label(TEXT("Request scheduled checkpoints"),10)]];
        Fields->AddSlot().AutoHeight().Padding(0,0,0,5)[Label(Labels[Index],10,Muted)];
        Fields->AddSlot().AutoHeight().Padding(0,0,0,10)[Input(Index,Index==FStudioRunSettingsEdit::MaxPhysicalTime?TEXT("No physical-time limit"):TEXT("Whole-number steps"))];
    }
    Fields->AddSlot().AutoHeight().Padding(0,0,0,10)[Live([State]
    {return State->Edit.bCheckpoints?TEXT("Checkpoint interval is a request for the next run."):TEXT("Checkpoint scheduling is off. Its interval is retained.");},9,Muted,true)];
    auto ApplyButton=Button(TEXT("Apply parameters"),TEXT("check"),Apply,Cyan);ApplyButton->SetTag(TEXT("RunParametersApply"));
    ApplyButton->SetEnabled(TAttribute<bool>::CreateLambda([State,Available]{return Available()&&!State->bConflict&&State->Edit.IsDirty();}));
    auto Revert=Button(TEXT("Revert edits"),TEXT("undo"),[this,State,Available]
    {
        if(!Available())return;State->Edit.Reset(M->Project.Draft);State->bConflict=false;State->Synchronize();
        State->Revision=M->Project.Draft.Revision;State->Notice=TEXT("Applied run parameters restored.");ResolveSaveNotice(*M,RunSettingsSaveGuard,State->Notice);
    });Revert->SetTag(TEXT("RunParametersRevert"));
    Revert->SetEnabled(TAttribute<bool>::CreateLambda([State,Available]{return Available()&&(State->bConflict||State->Edit.IsDirty()||!State->Edit.Error.IsEmpty());}));
    Fields->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[ApplyButton]+SHorizontalBox::Slot().FillWidth(1)[Revert]];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[Live([State]
    {return State->bConflict?TEXT("Run parameters changed outside this form. Revert before applying."):
        !State->Edit.Error.IsEmpty()?State->Edit.Error:!State->Notice.IsEmpty()?State->Notice:
        State->Edit.IsDirty()?TEXT("Unapplied run parameters. Apply or revert these edits."):TEXT("");},9,Amber,true)];
    auto Undo=Button(TEXT("Undo case"),TEXT("undo"),[this]{M->UndoCase();});Undo->SetTag(TEXT("RunParametersUndo"));
    Undo->SetEnabled(TAttribute<bool>::CreateLambda([this,Available]{return Available()&&M->CanUndoCase();}));
    auto Redo=Button(TEXT("Redo case"),TEXT("redo"),[this]{M->RedoCase();});Redo->SetTag(TEXT("RunParametersRedo"));
    Redo->SetEnabled(TAttribute<bool>::CreateLambda([this,Available]{return Available()&&M->CanRedoCase();}));
    Fields->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[Undo]+SHorizontalBox::Slot().FillWidth(1)[Redo]];
    Fields->AddSlot().AutoHeight()[Live([]
    {return TEXT("No numerical backend is connected. The control harness does not enforce these limits, write flow output or create scheduled restart files.");},9,Muted,true)];
    return Section(TEXT("Run parameters"),Fields);
}


TSharedRef<SWidget> SStudioWorkspace::ViewportMenu()
{
    return SNew(SBox).WidthOverride(340).MaxDesiredHeight(530)
    [SNew(SBorder).BorderImage(&PanelBrush).Padding(14)
    [SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
    +SScrollBox::Slot()[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("Viewport camera"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Row(TEXT("Position X"),Number([this]{return Scene->CameraPosition().X;},[this](double V){auto P=Scene->CameraPosition();P.X=V;Scene->SetCameraPosition(P);},-10000,10000,TEXT("m")))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Row(TEXT("Position Y"),Number([this]{return Scene->CameraPosition().Y;},[this](double V){auto P=Scene->CameraPosition();P.Y=V;Scene->SetCameraPosition(P);},-10000,10000,TEXT("m")))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Row(TEXT("Position Z"),Number([this]{return Scene->CameraPosition().Z;},[this](double V){auto P=Scene->CameraPosition();P.Z=V;Scene->SetCameraPosition(P);},-10000,10000,TEXT("m")))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Row(TEXT("Yaw"),Number([this]{return Scene->CameraRotation().Yaw;},[this](double V){auto R=Scene->CameraRotation();R.Yaw=V;Scene->SetCameraRotation(R);},-180,180,TEXT("deg"),1))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Row(TEXT("Pitch"),Number([this]{return Scene->CameraRotation().Pitch;},[this](double V){auto R=Scene->CameraRotation();R.Pitch=V;Scene->SetCameraRotation(R);},-180,180,TEXT("deg"),1))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Roll"),Number([this]{return Scene->CameraRotation().Roll;},[this](double V){auto R=Scene->CameraRotation();R.Roll=V;Scene->SetCameraRotation(R);},-180,180,TEXT("deg"),1))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Field of view"),Number([this]{return Scene->SavedCameraState().FieldOfView;},[this](double V){auto C=Scene->SavedCameraState();C.FieldOfView=V;Scene->RestoreCamera(C,TEXT("Field of view"));},5,160,TEXT("deg"),1))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Ortho width"),Number([this]{return Scene->SavedCameraState().OrthoWidth;},[this](double V){auto C=Scene->SavedCameraState();C.OrthoWidth=V;Scene->RestoreCamera(C,TEXT("Orthographic width"));},.001,10000,TEXT("m")))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[CameraClippingControls()]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[ViewHistoryControls()]
    ]]];
}

// THESIS: Control the camera's visible depth without editing the CFD domain.
// OWN-WORLD: Existing compact blue-black Slate inspector, numeric fields and amber validation.
// STORY: Enable clipping, enter near/far distances, undo or restore a saved camera.
// FIRST VIEWPORT: Clipping joins projection in the Camera inspector; the flow remains visible.
// FORM: Local Operate extension; meter distances follow the camera in perspective and orthographic views.
// FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
TSharedRef<SWidget> SStudioWorkspace::CameraClippingControls()
{
    struct FFeedback { FGuid Project;int32 CameraRevision=-1;FString Error; };
    const auto Feedback=MakeShared<FFeedback>();
    const auto Commit=[this,Feedback](bool bEnabled,double Near,double Far)
    {
        if(M->IsProjectOpenPending())return;
        Feedback->Project=M->Project.Id;
        if(Scene->SetDepthClipping(bEnabled,Near,Far))Feedback->Error.Empty();
        else if(!FMath::IsFinite(Near)||!FMath::IsFinite(Far))Feedback->Error=TEXT("Enter a finite distance in meters. Previous range kept.");
        else Feedback->Error=FString::Printf(
            TEXT("Rejected near %.6g m / far %.6g m. Reduce near distance or increase far distance. Previous range kept."),Near,Far);
        Feedback->CameraRevision=M->CameraRevision;
    };
    const auto Input=[this,Commit](bool bNear) -> TSharedRef<SWidget>
    {
        const auto NumberInput=SNew(SCameraDistanceField,
            TFunction<double()>([this,bNear]{const auto C=Scene->SavedCameraState();return bNear?C.NearClipMeters:C.FarClipMeters;}),
            TFunction<int32()>([this]{return M->CameraRevision;}),
            TFunction<void(double)>([this,Commit,bNear](double Value)
            {const auto C=Scene->SavedCameraState();Commit(C.bDepthClipping,bNear?Value:C.NearClipMeters,bNear?C.FarClipMeters:Value);}))
            .Tag(bNear?TEXT("CameraNearClip"):TEXT("CameraFarClip"))
            .IsEnabled_Lambda([this]{return !M->IsProjectOpenPending()&&Scene->SavedCameraState().bDepthClipping;})
            .ToolTipText(FText::FromString(bNear?
                TEXT("Near-plane distance along the camera's forward direction, in meters."):
                TEXT("Far-plane distance along the camera's forward direction, in meters. Must exceed near distance.")));
        return SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[NumberInput]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5,0,0,0)[Label(TEXT("m"),9,Muted)];
    };
    const auto HasError=[this,Feedback]
    {return Feedback->Project==M->Project.Id&&Feedback->CameraRevision==M->CameraRevision&&!Feedback->Error.IsEmpty();};
    const auto Error=Live([HasError,Feedback]{return HasError()?Feedback->Error:FString();},9,Amber,true);
    Error->SetTag(TEXT("CameraClippingFeedback"));
    Error->SetVisibility(TAttribute<EVisibility>::CreateLambda([HasError]
        {return HasError()?EVisibility::Visible:EVisibility::Collapsed;}));
    return SNew(SVerticalBox).Tag(TEXT("CameraClippingControls"))
        +SVerticalBox::Slot().AutoHeight().Padding(0,2,0,7)
        [SNew(SCheckBox).Tag(TEXT("CameraDepthClipping"))
            .IsEnabled_Lambda([this]{return !M->IsProjectOpenPending();})
            .IsChecked_Lambda([this]{return Scene->SavedCameraState().bDepthClipping?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([this,Commit](ECheckBoxState State)
            {const auto C=Scene->SavedCameraState();Commit(State==ECheckBoxState::Checked,C.NearClipMeters,C.FarClipMeters);})
            [Label(TEXT("Camera depth clipping"),10)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Row(TEXT("Near distance"),Input(true))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Row(TEXT("Far distance"),Input(false))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Error]
        +SVerticalBox::Slot().AutoHeight()[Live([]{return TEXT("Distances from the camera. Fit to domain extends the far plane when needed.");},9,Muted,true)];
}

// THESIS: Place a saved camera in the flow scene while independently observing it.
// OWN-WORLD: Dense Slate inspector; existing typography, panels and axis colors.
// STORY: Select Place in scene, move or rotate, preview the draft, then Apply or Cancel.
// FIRST VIEWPORT: The placement inspector replaces Setup; the scene and replay stay live.
// FORM: Local Operate extension; clipping planes are exact, automatic depth uses a focus-distance cone.
// FINISH: Native controls, two-size captures, independent review and documentation remain required.
TSharedRef<SWidget> SStudioWorkspace::CameraPlacementControls()
{
    auto Read=[this]{const auto* D=M->CameraPlacement();return D?D->Camera:FStudioCameraState();};
    auto Fields=SNew(SVerticalBox).IsEnabled_Lambda([this]{return M->IsCameraPlacementCurrent();});
    auto Modes=SNew(SHorizontalBox);
    for(const auto Tool:{EStudioCameraPlacementTool::Move,EStudioCameraPlacementTool::Rotate})
    {
        const FString Name=Tool==EStudioCameraPlacementTool::Move?TEXT("Move"):TEXT("Rotate");
        auto Control=SNew(SButton).Tag(FName(TEXT("Placement")+Name)).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,7))
            .OnClicked_Lambda([this,Tool]{M->SetCameraPlacementTool(Tool);return FReply::Handled();})
            [SNew(STextBlock).Text(FText::FromString(Name)).Font(Font(10))
                .ColorAndOpacity_Lambda([this,Tool]{const auto* D=M->CameraPlacement();return D&&D->Tool==Tool?Cyan:Text;})];
        if(Tool==EStudioCameraPlacementTool::Move)CameraPlacementFocus=Control;
        Modes->AddSlot().FillWidth(1).Padding(0,0,5,0)[Control];
    }
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[Modes];
    auto Frame=Button(TEXT("Frame camera"),TEXT("fit"),[this]
    {
        const auto* D=M->CameraPlacement();if(!D||!M->IsCameraPlacementCurrent())return;
        const auto Size=Scene->PresentedViewportSize();const double Aspect=Size.Y>0?double(Size.X)/Size.Y:1.5;
        FBox Bounds=M->Solver->Descriptor().DisplayBounds;Bounds+=D->Camera.Position;
        // A distant far plane must not turn the local camera and wing into a
        // single pixel. Frame the pose/focus cone; actual clipping stays intact.
        auto LocalCone=D->Camera;LocalCone.bDepthClipping=false;
        for(const auto& L:StudioCameraPlacement::Frustum(LocalCone,Aspect)){Bounds+=L.A;Bounds+=L.B;}
        Scene->RestoreCamera(StudioView::FitBounds(Scene->SavedCameraState(),Bounds,Aspect,.001),TEXT("Frame saved camera"));
    });
    Frame->SetTag(TEXT("PlacementFrame"));
    Frame->SetToolTipText(FText::FromString(TEXT("Move the observing view to include the saved camera and domain. Undo view restores your previous view.")));
    Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[Frame];
    auto Preview=Button(TEXT("Preview draft"),TEXT("camera"),[this]{M->PreviewCameraPlacement();});
    Preview->SetTag(TEXT("PlacementPreview"));
    Preview->SetToolTipText(FText::FromString(TEXT("View through this draft without saving it. Undo view restores the previous viewpoint; playback and saved cameras are retained.")));
    Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[Preview];
    auto Numeric=[&](const FString& Title,const FString& Id,TFunction<double(const FStudioCameraState&)> Get,TFunction<void(FStudioCameraState&,double)> Set)
    {
        const auto Input=SNew(SCameraDistanceField,TFunction<double()>([Read,Get]{return Get(Read());}),
            TFunction<int32()>([this]{return M->CameraPlacementRevision;}),TFunction<void(double)>([this,Read,Set,Title](double V)
            {
                if(!FMath::IsFinite(V)){M->CameraPlacementNotice=Title+TEXT(": enter a finite number. Previous draft kept.");M->bCameraPlacementError=true;return;}
                auto Camera=Read();Set(Camera,V);M->EditCameraPlacement(Camera);
            }));
        Input->SetTag(FName(TEXT("Placement")+Id));Input->SetToolTipText(FText::FromString(Title));
        Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[Row(Title,Input,106)];
    };
    Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Saved camera position"),10,Text,true)];
    for(int32 I=0;I<3;++I)
    {
        const FString Axis=FString::Chr(TEXT("XYZ")[I]);
        Numeric(Axis+TEXT(" (m)"),TEXT("Position")+Axis,[I](const auto& C){return C.Position[I];},[I](auto& C,double V)
        {const double Delta=V-C.Position[I];C.Position[I]=V;C.Focus[I]+=Delta;});
    }
    Fields->AddSlot().AutoHeight().Padding(0,8,0,6)[Label(TEXT("Orientation"),10,Text,true)];
    for(int32 I=0;I<3;++I)
    {
        const FString Name=I==0?TEXT("Yaw"):I==1?TEXT("Pitch"):TEXT("Roll");
        Numeric(Name+TEXT(" (deg)"),Name,[I](const auto& C){const auto R=C.Orientation.Rotator();return I==0?R.Yaw:I==1?R.Pitch:R.Roll;},[I](auto& C,double V)
        {
            if(FMath::Abs(V)>1.e8){C.Position.X=std::numeric_limits<double>::quiet_NaN();return;}
            auto R=C.Orientation.Rotator();if(I==0)R.Yaw=V;else if(I==1)R.Pitch=V;else R.Roll=V;
            C.Orientation=R.Quaternion().GetNormalized();C.Focus=C.Position+C.Orientation.GetForwardVector()*C.OrbitDistance;
        });
    }
    Fields->AddSlot().AutoHeight().Padding(0,8,0,6)[Label(TEXT("Projection"),10,Text,true)];
    Numeric(TEXT("Focus (m)"),TEXT("Distance"),[](const auto& C){return C.OrbitDistance;},[](auto& C,double V)
    {C.OrbitDistance=V;C.Focus=C.Position+C.Orientation.GetForwardVector()*V;});
    Numeric(TEXT("FOV (deg)"),TEXT("Fov"),[](const auto& C){return C.FieldOfView;},[](auto& C,double V){C.FieldOfView=V;});
    Fields->AddSlot().AutoHeight().Padding(0,4,0,8)[Check(TEXT("Orthographic projection"),[Read]{return Read().bOrthographic;},[this,Read](bool V)
    {auto C=Read();C.bOrthographic=V;M->EditCameraPlacement(C);})];
    Numeric(TEXT("Width (m)"),TEXT("Width"),[](const auto& C){return C.OrthoWidth;},[](auto& C,double V){C.OrthoWidth=V;});
    Fields->AddSlot().AutoHeight().Padding(0,4,0,8)[Check(TEXT("Depth clipping"),[Read]{return Read().bDepthClipping;},[this,Read](bool V)
    {auto C=Read();C.bDepthClipping=V;M->EditCameraPlacement(C);})];
    Numeric(TEXT("Near (m)"),TEXT("Near"),[](const auto& C){return C.NearClipMeters;},[](auto& C,double V){C.NearClipMeters=V;});
    Numeric(TEXT("Far (m)"),TEXT("Far"),[](const auto& C){return C.FarClipMeters;},[](auto& C,double V){C.FarClipMeters=V;});
    Fields->AddSlot().AutoHeight().Padding(0,8)[Live([]{return TEXT("Depth clipping shows the near and far planes. With clipping off, the cone ends at the focus distance.");},9,Muted,true)];
    auto Apply=Button(TEXT("Apply"),TEXT("save"),[this]{if(M->ApplyCameraPlacement())Navigate(EStudioWorkspace::Solve);},Cyan);
    Apply->SetTag(TEXT("PlacementApply"));Apply->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->IsCameraPlacementCurrent();}));
    auto Cancel=Button(TEXT("Cancel"),TEXT("stop"),[this]{M->CancelCameraPlacement();Navigate(EStudioWorkspace::Solve);});Cancel->SetTag(TEXT("PlacementCancel"));
    return SNew(SBorder).Tag(TEXT("CameraPlacementInspector")).BorderImage(&PanelBrush).Padding(10)
    [SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,2,0,8)[Label(TEXT("Place saved camera"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(STextBlock).Font(Font(10)).ColorAndOpacity(Cyan).AutoWrapText(true)
            .Text_Lambda([this]{const auto* D=M->CameraPlacement();const auto* C=D?M->FindCamera(D->CameraId):nullptr;return FText::FromString(C?C->Name:FString());})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Tag(TEXT("PlacementNotice")).Font(Font(9)).AutoWrapText(true)
            .ColorAndOpacity_Lambda([this]{return M->bCameraPlacementError||!M->IsCameraPlacementCurrent()?Amber:Muted;})
            .Text_Lambda([this]{return FText::FromString(!M->IsCameraPlacementCurrent()?TEXT("Saved cameras changed. Cancel this placement and select the camera again."):M->CameraPlacementNotice);})]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12)
            +SScrollBox::Slot()[Fields]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[Apply]
            +SHorizontalBox::Slot().FillWidth(1)[Cancel]]];
}
TSharedRef<SWidget> SStudioWorkspace::ProjectMenu()
{
    auto Items=SNew(SVerticalBox);
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("PROJECT"),9,Muted,true)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SEditableTextBox).Style(&InputStyle()).Font(Font(11))
        .Text(FText::FromString(M->Project.Name)).ToolTipText(FText::FromString(TEXT("Project name")))
        .OnTextCommitted_Lambda([this](const FText& V,ETextCommit::Type){M->RenameProject(V.ToString());})];
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Check(TEXT("Favorite project"),[this]{return M->Project.bFavorite;},[this](bool V){M->SetProjectFavorite(M->ProjectPath,V);})];
    auto Add=[&](const FString& Name,ECommand Command){Items->AddSlot().AutoHeight().Padding(0,0,0,5)[Button(Name,TEXT("box"),[this,Command]{FSlateApplication::Get().DismissAllMenus();Execute(Command);})];};
    Add(TEXT("New project…       Cmd+N"),ECommand::NewProject);
    Add(TEXT("Open project…      Cmd+O"),ECommand::OpenProject);
    Add(TEXT("Save                    Cmd+S"),ECommand::Save);
    Add(TEXT("Save as…      Shift+Cmd+S"),ECommand::SaveAs);
    Add(TEXT("Duplicate project…"),ECommand::DuplicateProject);
    Items->AddSlot().AutoHeight().Padding(0,6)[Button(TEXT("Browse projects"),TEXT("folder"),[this]{FSlateApplication::Get().DismissAllMenus();Navigate(EStudioWorkspace::Projects);})];
    Items->AddSlot().AutoHeight().Padding(0,12,0,8)[Label(TEXT("RECENT PROJECTS"),9,Muted,true)];
    if(M->RecentProjects.IsEmpty()) Items->AddSlot().AutoHeight()[Label(TEXT("Saved projects appear here."),10,Muted)];
    for(const FString Path:M->RecentProjects)
        Items->AddSlot().AutoHeight().Padding(0,0,0,5)[Button(FPaths::GetBaseFilename(Path),TEXT("box"),[this,Path]{FSlateApplication::Get().DismissAllMenus();OpenProject(Path);})];
    Items->AddSlot().AutoHeight().Padding(0,12,0,5)[Button(TEXT("Restart recording"),TEXT("run"),[this]{Execute(ECommand::RestartPlayback);})];
    return SNew(SBox).WidthOverride(290)[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)[Items]];
}
TSharedRef<SWidget> SStudioWorkspace::CameraMenu()
{
    // THESIS: Save and manage exact camera positions without interrupting flow review.
    // OWN-WORLD: Existing compact blue-black Slate popover and native text controls.
    // STORY: Save the current view, activate a named view, edit its name or pose, undo an edit.
    // FIRST VIEWPORT: Creation and feedback above a bounded scrolling list; history remains reachable.
    // FORM: Local Operate extension of the approved camera popover.
    // FINISH: Packaged interaction captures, independent finish review and documentation are required.
    auto Items=SNew(SVerticalBox);
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[ViewHistoryControls()];
    Items->AddSlot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("Camera and display only. Playback is retained."),9,Muted)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1)[Label(TEXT("Saved cameras"),12,Text,true)]
        +SHorizontalBox::Slot().AutoWidth()[Live([this]{return FString::Printf(TEXT("%d / 128"),M->Project.Cameras.Num());},9,Muted)]];
    const auto Name=MakeShared<FString>();
    const FGuid Owner=M->Project.Id;
    const auto Input=SNew(SProjectFilterBox).Tag(TEXT("NewCameraName")).Style(&InputStyle()).Font(Font(10))
        .ClearKeyboardFocusOnCommit(false)
        .ForegroundColor_Lambda([Name]{return Name->IsEmpty()?Muted:Text;})
        .HintText(FText::FromString(TEXT("Name this view"))).ToolTipText(FText::FromString(TEXT("Unique saved camera name, up to 120 characters")))
        .OnTextChanged_Lambda([Name](const FText& V){*Name=V.ToString();});
    const TWeakPtr<SEditableTextBox> WeakInput=Input;
    auto SaveCamera=Button(TEXT("Save view"),TEXT("plus"),[this,Name,WeakInput,Owner]
    {
        if(Owner!=M->Project.Id) return;
        const bool Saved=M->AddCamera(*Name,Scene->SavedCameraState());
        if(const auto Box=WeakInput.Pin())
        { if(Saved){Name->Empty();Box->SetText(FText::GetEmpty());} }
    });
    SaveCamera->SetTag(TEXT("SaveCamera"));
    SaveCamera->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !M->IsProjectOpenPending()&&M->Project.Cameras.Num()<128;}));
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[Input]
        +SHorizontalBox::Slot().AutoWidth()[SaveCamera]];
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Tag(TEXT("CameraManagerNotice")).Font(Font(9)).AutoWrapText(true)
        .Text_Lambda([this]{return FText::FromString(M->CameraCollectionNotice.IsEmpty()?TEXT("Activate restores a saved view. Update replaces it with the current view."):M->CameraCollectionNotice);})
        .ColorAndOpacity_Lambda([this]{return M->bCameraCollectionError?Amber:Muted;})];
    TSharedPtr<SVerticalBox> Rows;
    Items->AddSlot().FillHeight(1)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(10)
        +SScrollBox::Slot()[SAssignNew(Rows,SVerticalBox)]];
    auto Undo=Button(TEXT("Undo saved edit"),TEXT("undo"),[this]{M->UndoSavedCameras();});
    auto Redo=Button(TEXT("Redo saved edit"),TEXT("redo"),[this]{M->RedoSavedCameras();});
    Undo->SetTag(TEXT("UndoCameraCollection"));Redo->SetTag(TEXT("RedoCameraCollection"));
    Undo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !M->IsProjectOpenPending()&&M->CanUndoSavedCameras();}));
    Redo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !M->IsProjectOpenPending()&&M->CanRedoSavedCameras();}));
    Undo->SetToolTipText(TAttribute<FText>::CreateLambda([this]{return FText::FromString(M->CanUndoSavedCameras()?M->UndoSavedCamerasLabel():TEXT("No saved-camera edits to undo"));}));
    Redo->SetToolTipText(TAttribute<FText>::CreateLambda([this]{return FText::FromString(M->CanRedoSavedCameras()?M->RedoSavedCamerasLabel():TEXT("No saved-camera edits to redo"));}));
    Items->AddSlot().AutoHeight().Padding(0,10,0,0)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[Undo]
        +SHorizontalBox::Slot().FillWidth(1)[Redo]];
    CameraNameInput=Input; CameraRows=Rows; RefreshCameraRows();
    return SNew(SBox).WidthOverride(420).MinDesiredHeight(280).MaxDesiredHeight(510)
        [SNew(SBorder).BorderImage(&PanelBrush).Padding(12)[Items]];
}

void SStudioWorkspace::RefreshCameraRows()
{
    const auto Rows=CameraRows.Pin(); if(!Rows) return;
    FName FocusKey;
    for(const auto& Target:CameraActionTargets)
        if(const auto Widget=Target.Value.Pin();Widget&&(Widget->HasKeyboardFocus()||Widget->HasFocusedDescendants()))
        {FocusKey=Target.Key;break;}
    CameraActionTargets.Reset();
    Rows->ClearChildren();
    const FGuid Owner=M->Project.Id;
    if(M->Project.Cameras.IsEmpty()) Rows->AddSlot().AutoHeight().Padding(0,10)[SNew(STextBlock)
        .Text(FText::FromString(TEXT("No saved cameras yet. Position the camera, enter a name, and save the view.")))
        .Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)];
    for(const auto& B:M->Project.Cameras)
    {
        const FGuid Id=B.Id;
        const FString Suffix=Id.ToString(EGuidFormats::Digits);
        const auto Name=SNew(SEditableTextBox).Tag(FName(TEXT("CameraName-")+Suffix)).Style(&InputStyle()).Font(Font(10))
            .ClearKeyboardFocusOnCommit(false)
            .Text(FText::FromString(B.Name)).ToolTipText(FText::FromString(TEXT("Rename saved camera; press Enter to apply")))
            .OnTextCommitted_Lambda([this,Id,Owner](const FText& TextValue,ETextCommit::Type How)
            {
                if(How==ETextCommit::OnCleared||Owner!=M->Project.Id) return;
                const auto* Saved=M->FindCamera(Id);
                if(Saved&&Saved->Name==TextValue.ToString().TrimStartAndEnd())
                { if(How==ETextCommit::OnEnter&&M->bCameraCollectionError) M->RenameCamera(Id,TextValue.ToString()); return; }
                M->RenameCamera(Id,TextValue.ToString());
            });
        CameraActionTargets.Add(Name->GetTag(),Name);
        auto Actions=SNew(SHorizontalBox);
        auto Action=[&](const TCHAR* Title,const TCHAR* IconName,const TCHAR* Hint,TFunction<void()> Apply)
        {
            auto Control=Button(Title,IconName,[this,Owner,Apply]{if(M->Project.Id==Owner)Apply();});
            Control->SetTag(FName(FString(TEXT("Camera"))+Title+TEXT("-")+Suffix));
            CameraActionTargets.Add(Control->GetTag(),Control);
            Control->SetToolTipText(FText::FromString(Hint));
            Actions->AddSlot().AutoWidth().Padding(0,0,5,0)[Control];
            return Control;
        };
        Action(TEXT("Activate"),TEXT("camera"),TEXT("Restore this camera's position, orientation and projection; playback keeps its current state"),[this,Id]
        { if(M->RestoreSavedCamera(Id)) FSlateApplication::Get().DismissAllMenus(); });
        Action(TEXT("Update"),TEXT("save"),TEXT("Replace this saved camera with the current view; Undo saved edit restores its previous pose"),[this,Id]
        {M->UpdateCamera(Id,Scene->SavedCameraState());});
        auto Copy=Action(TEXT("Copy"),TEXT("plus"),TEXT("Duplicate this saved camera with a unique name"),[this,Id]{M->DuplicateCamera(Id);});
        Copy->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->Project.Cameras.Num()<128;}));
        Action(TEXT("Delete"),TEXT("stop"),TEXT("Remove this saved camera; Undo saved edit restores it"),[this,Id]{M->DeleteCamera(Id);});
        auto Place=Button(TEXT("Place in scene"),TEXT("camera"),[this,Id,Owner]
        {
            if(Owner!=M->Project.Id||!M->BeginCameraPlacement(Id))return;
            Navigate(EStudioWorkspace::Solve);FSlateApplication::Get().DismissAllMenus();
            if(const auto Focus=CameraPlacementFocus.Pin())FSlateApplication::Get().SetKeyboardFocus(Focus,EFocusCause::Navigation);
        });
        Place->SetTag(FName(TEXT("CameraPlace-")+Suffix));CameraActionTargets.Add(Place->GetTag(),Place);
        Place->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CameraPlacement()==nullptr;}));
        Place->SetToolTipText(FText::FromString(TEXT("Move or rotate this camera in the scene, then Apply or Cancel. Use Frame camera to see it from outside.")));
        Rows->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(SVerticalBox)
            .IsEnabled_Lambda([this,Owner]{return Owner==M->Project.Id&&!M->IsProjectOpenPending();})
            +SVerticalBox::Slot().AutoHeight()[Name]
            +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,0)[Actions]
            +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,0)[Place]];
    }
    LastCameraCollectionRevision=M->CameraCollectionRevision; CameraRowsProjectId=M->Project.Id;
    // A row edit replaces widgets; keep focus within the popup on the same
    // stable action, or return to creation when the focused camera was deleted.
    if(!FocusKey.IsNone())
    {
        const auto* Target=CameraActionTargets.Find(FocusKey);
        const TSharedPtr<SWidget> Focus=Target?Target->Pin():CameraNameInput.Pin();
        if(Focus) FSlateApplication::Get().SetKeyboardFocus(Focus,EFocusCause::Navigation);
    }
}
TSharedRef<SWidget> SStudioWorkspace::ViewHistoryControls()
{
    auto Undo=Button(TEXT("Undo view"),TEXT("undo"),[this]{Execute(ECommand::UndoView);});
    auto Redo=Button(TEXT("Redo view"),TEXT("redo"),[this]{Execute(ECommand::RedoView);});
    Undo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanUndoView();}));
    Redo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanRedoView();}));
    Undo->SetToolTipText(TAttribute<FText>::CreateLambda([this]{return FText::FromString(TEXT("Undo view: ")+ (M->CanUndoView()?M->UndoViewLabel():TEXT("no earlier view"))+TEXT(" · Cmd+Option+Z"));}));
    Redo->SetToolTipText(TAttribute<FText>::CreateLambda([this]{return FText::FromString(TEXT("Redo view: ")+ (M->CanRedoView()?M->RedoViewLabel():TEXT("no later view"))+TEXT(" · Cmd+Option+Shift+Z"));}));
    return SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1).Padding(0,0,5,0)[Undo]+SHorizontalBox::Slot().FillWidth(1)[Redo];
}
bool SStudioWorkspace::Save(bool bSaveAs)
{
    if(Pipelines){Pipelines->SaveCamera();if(!Pipelines->EnsureResolved())return false;}
    if(!EnsurePlacementResolved()||!EnsureGeometryResolved()||!EnsureMaterialsResolved()||!EnsureDomainResolved()||!EnsureBoundariesResolved()||!EnsureLatticeResolved()||!EnsureRunSettingsResolved()||!EnsureFlowConditionsResolved()||!EnsureHome4Resolved())return false;
    FString Path=M->ProjectPath;
    if((bSaveAs||Path.IsEmpty()) && !StudioFileDialog::Project(true,Path,M->Project.Name,Path)) return false;
    M->Project.Camera=Scene->SavedCameraState(); return M->SaveProject(Path);
}
bool SStudioWorkspace::DispatchControl(EStudioJobCommand Command)
{
    if(Command==EStudioJobCommand::Submit&&M->Project.bControlHarness&&
        !M->Job().Can(EStudioJobCommand::Resume)&&(!EnsureRunSettingsResolved()||!EnsureFlowConditionsResolved()||!EnsureHome4Resolved()))return false;
    return M->Control(Command);
}
bool SStudioWorkspace::ExecuteApplicationCommand(EStudioCommand Command,FString& Response)
{
    if(!StudioCommands::Validate(*M,Command,Response))return false;
    switch(Command)
    {
    case EStudioCommand::ProjectSave:case EStudioCommand::ProjectSaveAs:
    {
        const FString PreviousNotice=M->Notice;
        const bool OK=Save(Command==EStudioCommand::ProjectSaveAs);
        Response=OK?TEXT("Project saved."):M->Notice!=PreviousNotice?M->Notice:TEXT("Project was not saved. Resolve pending edits or choose a save destination.");
        return OK;
    }
    case EStudioCommand::ViewFit:case EStudioCommand::ViewUndo:case EStudioCommand::ViewRedo:
        if(!EnsurePlacementResolved()){Response=M->Notice;return false;}
        Execute(Command==EStudioCommand::ViewFit?ECommand::FitCamera:Command==EStudioCommand::ViewUndo?ECommand::UndoView:ECommand::RedoView);
        Response=Command==EStudioCommand::ViewFit?TEXT("Camera fitted to the flow domain."):Command==EStudioCommand::ViewUndo?TEXT("View edit undone."):TEXT("View edit redone.");return true;
    case EStudioCommand::JobSubmit:
    {
        const bool OK=DispatchControl(EStudioJobCommand::Submit);Response=M->Notice;return OK;
    }
    default:return StudioCommands::ExecuteModel(*M,Command,Response);
    }
}
bool SStudioWorkspace::ConfirmReplace(bool bAllowRecovery)
{
    if(Pipelines){Pipelines->SaveCamera();if(!Pipelines->EnsureResolved())return false;}
    if(!EnsurePlacementResolved()||!EnsureGeometryResolved()||!EnsureMaterialsResolved()||!EnsureDomainResolved()||!EnsureBoundariesResolved()||!EnsureLatticeResolved()||!EnsureRunSettingsResolved()||!EnsureFlowConditionsResolved()||!EnsureHome4Resolved())return false;
    if(!M->CanReplaceProject())return false;
    if(!bAllowRecovery&&!M->PendingRecovery.IsEmpty()) {M->Notice=TEXT("Restore or discard the pending recovery before switching projects.");return false;}
    M->Project.Camera=Scene->SavedCameraState();
    if(!M->HasUnsavedChanges()) return true;
    const auto Choice=FMessageDialog::Open(EAppMsgType::YesNoCancel,FText::FromString(
        TEXT("Save changes to “")+M->Project.Name+TEXT("”?\nYes saves this project. No discards these changes. Cancel keeps the project open.")));
    if(Choice==EAppReturnType::Yes) return Save();
    return Choice==EAppReturnType::No;
}
bool SStudioWorkspace::EnsurePlacementResolved()
{
    if(!M->CameraPlacement())return true;
    M->bActivityLogExpanded=false;
    M->Notice=TEXT("Apply or cancel camera placement before saving, closing or replacing the project.");
    if(M->IsCameraPlacementCurrent())M->CameraPlacementNotice=M->Notice;
    Navigate(EStudioWorkspace::Solve);
    if(const auto Focus=CameraPlacementFocus.Pin())FSlateApplication::Get().SetKeyboardFocus(Focus,EFocusCause::Navigation);
    return false;
}
bool SStudioWorkspace::CanClose()
{
    if(Pipelines)Pipelines->SaveCamera();
    if(!ConfirmReplace(true)) return false;
    M->CancelProjectOpen(false);
    if(M->PendingRecovery.IsEmpty()) M->DiscardRecovery();
    // Mark the closing state clean so EndPlay cannot resurrect discarded changes.
    SnapshotUI->Shutdown();
    M->SuppressRecoveryOnClose=true; return true;
}
// THESIS: Opening prepares a verified candidate while the current scene remains usable.
// OWN-WORLD: Existing blue-black Slate shell, compact CoreStyle labels and native buttons.
// STORY: Choose a project, see its loading stage in the footer, cancel or enter the loaded view.
// FIRST VIEWPORT: Retain the full current workspace; place Cancel opening beside footer status.
// FORM: Local Operate extension of the reference shell; no new visual direction.
// FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
bool SStudioWorkspace::OpenProject(const FString& Path,const FString& ReplacedRecentPath)
{
    bNewHome4Project=false;
    if(M->IsProjectOpenPending()) {M->Notice=TEXT("A project is already opening. Cancel it before choosing another.");return false;}
    return ConfirmReplace()&&M->RequestProjectOpen(Path,ReplacedRecentPath);
}
void SStudioWorkspace::Execute(ECommand Command)
{
    if(Pipelines)Pipelines->SaveCamera();
    switch(Command)
    {
    case ECommand::Save: Save(); break;
    case ECommand::SaveAs: Save(true); break;
    case ECommand::DuplicateProject:
    {
        if(Pipelines&&!Pipelines->EnsureResolved())break;
        if(!EnsurePlacementResolved()||!EnsureGeometryResolved()||!EnsureMaterialsResolved()||!EnsureDomainResolved()||!EnsureBoundariesResolved()||!EnsureLatticeResolved()||!EnsureRunSettingsResolved()||!EnsureFlowConditionsResolved()||!EnsureHome4Resolved())break;
        if(!M->PendingRecovery.IsEmpty()) {M->Notice=TEXT("Restore or discard the pending recovery before duplicating a project.");break;}
        FString Path;
        if(!StudioFileDialog::Project(true,M->ProjectPath,M->Project.Name+TEXT(" copy"),Path)) break;
        M->Project.Camera=Scene->SavedCameraState();
        M->DuplicateProject(Path,FPaths::GetBaseFilename(Path)); bProjectListsDirty=true;
        break;
    }
    case ECommand::OpenProject:
    {
        FString Path; if(StudioFileDialog::Project(false,M->ProjectPath,TEXT(""),Path)) OpenProject(Path); break;
    }
    case ECommand::NewProject:
    {
        if(!ConfirmReplace()) break;
        bNewHome4Project=true;Navigate(EStudioWorkspace::Validation);
        M->Notice=TEXT("Choose a documented recipe to create a HOME4 project.");
        break;
    }
    case ECommand::FitCamera: Scene->FitCamera(); break;
    case ECommand::RestartPlayback: M->Reset(); break;
    case ECommand::UndoView: if(M->UndoView()) Scene->ApplyCamera(M->Project.Camera); break;
    case ECommand::RedoView: if(M->RedoView()) Scene->ApplyCamera(M->Project.Camera); break;
    }
}
FReply SStudioWorkspace::OnKeyDown(const FGeometry& Geometry,const FKeyEvent& Event)
{
    if(Event.GetKey()==EKeys::F1){OpenHelp();return FReply::Handled();}
    UE_LOG(LogTemp,Verbose,TEXT("Studio key %s command=%d control=%d shift=%d"),*Event.GetKey().ToString(),Event.IsCommandDown(),Event.IsControlDown(),Event.IsShiftDown());
    if(Event.IsCommandDown()||Event.IsControlDown())
    {
        if(Event.GetKey()==EKeys::Z&&Event.IsAltDown()&&M->Workspace==EStudioWorkspace::Solve)
        {Execute(Event.IsShiftDown()?ECommand::RedoView:ECommand::UndoView);return FReply::Handled();}
        if(Event.GetKey()==EKeys::S) {Execute(Event.IsShiftDown()?ECommand::SaveAs:ECommand::Save);return FReply::Handled();}
        if(Event.GetKey()==EKeys::O) {Execute(ECommand::OpenProject);return FReply::Handled();}
        if(Event.GetKey()==EKeys::N) {Execute(ECommand::NewProject);return FReply::Handled();}
    }
    return SCompoundWidget::OnKeyDown(Geometry,Event);
}
FReply SStudioWorkspace::OnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Event)
{
    if(Event.GetKey()==EKeys::F1)return OnKeyDown(Geometry,Event);
    const auto Focused=FSlateApplication::Get().GetKeyboardFocusedWidget();
    if(Event.GetKey()==EKeys::Escape&&Focused&&Focused->HasMouseCapture()&&Focused->GetTag().ToString().StartsWith(TEXT("PaneDrag_")))
        return FReply::Unhandled();
    if(Event.GetKey()==EKeys::Escape&&InspectionPlacement.IsSet()&&!M->bActivityLogExpanded)
    {CancelInspectionPlacement();M->InspectionNotice=TEXT("Placement cancelled. Saved coordinates retained.");return FReply::Unhandled();}
    // Only global shortcuts tunnel ahead of focused controls. The base key
    // handler turns arrows into navigation; calling it during preview would
    // swallow the chart's sample keys and text editors' caret movement.
    if((Event.IsCommandDown()||Event.IsControlDown())&&
        (Event.GetKey()==EKeys::S||Event.GetKey()==EKeys::O||Event.GetKey()==EKeys::N||
        (Event.GetKey()==EKeys::Z&&Event.IsAltDown()&&M->Workspace==EStudioWorkspace::Solve)))
        return OnKeyDown(Geometry,Event);
    return FReply::Unhandled();
}
TSharedRef<SWidget> SStudioWorkspace::ExportMenu()
{
    if(M->Workspace==EStudioWorkspace::Results&&Results->IsComparisonOpen()&&!FieldExport->IsBusy())
    {
        FString Error;auto Frozen=Results->ExportComparisonSnapshot(Error);
        return FieldExport->Menu(M.ToSharedRef(),nullptr,false,{},Error,true,MoveTemp(Frozen));
    }
    if(M->Workspace==EStudioWorkspace::PostProcessing&&!FieldExport->IsBusy())
    {
        FString Error;auto Frozen=Pipelines->ExportSnapshot(Error);
        return FieldExport->Menu(M.ToSharedRef(),nullptr,true,MoveTemp(Frozen),Error);
    }
    return FieldExport->Menu(M.ToSharedRef(),Scene.Get());
}
SStudioWorkspace::~SStudioWorkspace()
{if(SnapshotUI)SnapshotUI->Shutdown();}
FIntPoint SStudioWorkspace::SnapshotOutputSize() const
{return SnapshotUI->OutputSize();}
TSharedRef<SWidget> SStudioWorkspace::SnapshotMenu()
{
    const TWeakPtr<SStudioWorkspace> Weak=SharedThis(this);
    return SnapshotUI->Menu(M.ToSharedRef(),Scene.Get(),[Weak]
    {
        const auto Root=Weak.Pin();
        if(!Root)return FString(TEXT("The workspace has closed."));
        return Root->InspectionPlacement.IsSet()||Root->M->CameraPlacement()?
            FString(TEXT("Finish or cancel placement before taking a snapshot.")):FString();
    },[Weak](FStudioSnapshot& Image,FString& Error)
    {const auto Root=Weak.Pin();return Root&&Root->CaptureSnapshot(Image,Error);});
}
bool SStudioWorkspace::CaptureSnapshot(FStudioSnapshot& Frozen,FString& Error)
{
    TOptional<FStudioProbeResult> Samples;
    if(const auto* Probe=CurrentInspectionProbeResult(M->SelectedInspectionObject);Probe&&Probe->Status==EStudioProbeStatus::Ready)
        Samples=*Probe;
    if(!Scene.IsValid()||!Scene->CaptureSnapshot(Frozen,InspectionMarkers->Result(),Error))return false;
    if(Samples.IsSet())if(const auto* Probe=M->FindProbe(Frozen.SelectedObject))
    {
        FStudioProbeRequest Request;Request.ProjectId=Frozen.Project;Request.PresentationId=Frozen.Capture;
        Request.Probe=*Probe;Request.DisplayedScalar=Frozen.Scalar.Id;Request.Field=Scene->PresentedField();
        Samples->PresentationId=Frozen.Capture;
        if(Samples->Matches(Request))StudioProbeProfile::CSV(*Samples,Frozen.ProbeCSV,Error);
    }
    return true;
}

// THESIS: Choose verified recorded output while keeping the camera and case.
// OWN-WORLD: Reference blue-black Slate shell, flat rows and native folder panel.
// STORY: Import a recording folder, review it, and Locate an exact moved copy.
// FIRST VIEWPORT: The retained field stays visible; loading/cancel and repair are explicit.
// FORM: Compact Operate extension; provenance and source duration stay literal.
// FINISH: Finish review, verdict and design documentation close this UI slice.
void SStudioWorkspace::ImportRecording()
{
    FSlateApplication::Get().DismissAllMenus();FString Path;
    if(StudioFileDialog::RecordingFolder(FString(),Path))M->RequestExternalRecording(Path);
}
void SStudioWorkspace::LocateRecording(const FString& Id,const FString& CurrentPath)
{
    FSlateApplication::Get().DismissAllMenus();FString Path;
    if(StudioFileDialog::RecordingFolder(CurrentPath,Path))M->RequestRecordingRelink(Id,Path);
}
void SStudioWorkspace::RepairRecording()
{
    if(!M->RecordingRepair.IsSet()||!ConfirmReplace(M->RecordingRepair->bRecovery))return;
    FString Path;
    const bool Chosen=M->RecordingRepair->bReconstruction?
        StudioFileDialog::ReconstructionFolder(M->RecordingRepair->RecordingPath,Path):
        StudioFileDialog::RecordingFolder(M->RecordingRepair->RecordingPath,Path);
    if(Chosen)M->RetryRecordingRepair(Path);
}
// THESIS: Let an engineer choose and inspect a derived surface without changing recorded values.
// OWN-WORLD: Existing blue-black Slate popover, compact source labels and native folder selection.
// STORY: Read the method, attach matching topology, locate a moved copy or return to original points.
// FIRST VIEWPORT: One surface control in the Recorded dataset inspector; field and camera remain visible.
// FORM: Local Operate extension of the approved Solve workspace, with explicit source limitations.
// FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
TSharedRef<SWidget> SStudioWorkspace::SurfaceMenu()
{
    const FGuid Project=M->Project.Id;
    const TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> Source=M->Solver;
    const auto SameSource=[this,Project,Source]{return M->Project.Id==Project&&M->Solver==Source.Pin();};
    const auto Available=[this,SameSource]{return SameSource()&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending()&&!M->HasActiveJob();};
    const auto* Reference=M->Project.Recordings.FindByPredicate([this](const auto& R){return R.Id==M->Project.Dataset;});
    const bool Supported=Reference&&Reference->Format==TEXT("point_v3")&&(M->Solver->Descriptor().SpatialDimensions==2||M->Solver->Descriptor().SpatialDimensions==3);
    const FString Path=Reference&&Reference->Reconstruction.IsSet()?Reference->Reconstruction->Path:FString();
    const auto Surface=M->Solver->Reconstruction();
    const auto Volume=M->Solver->VolumeReconstruction();
    const bool Is3D=M->Solver->Descriptor().SpatialDimensions==3;
    auto Items=SNew(SVerticalBox);
    Items->AddSlot().AutoHeight().Padding(0,0,0,7)[Label(Is3D?TEXT("Volume reconstruction"):TEXT("Surface reconstruction"),12,Text,true)];
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Live([this,SameSource]
        {return SameSource()?M->Solver->Descriptor().Title:TEXT("Source changed. Reopen this menu for the current recording.");},10,Cyan,true)];
    auto Paragraph=[&](const FString& TextValue,FLinearColor Color=Muted)
    {Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(STextBlock).Text(FText::FromString(TextValue)).Font(Font(9)).ColorAndOpacity(Color).AutoWrapText(true)];};
    Paragraph(Is3D?TEXT("Attach a derived volume grid to the original 3D samples. The grid records interpolation weights; original CFD values stay unchanged."):TEXT("Attach an explicitly reconstructed surface to the original source points. Its connectivity is derived; recorded field values stay unchanged."));
    if(Volume)
    {
        Paragraph(Volume->Title,Text);Paragraph(Volume->Method);
        Paragraph(FString::Printf(TEXT("Display grid: %d × %d × %d"),Volume->Dimensions.X,Volume->Dimensions.Y,Volume->Dimensions.Z));
        for(const auto& Limitation:Volume->Limitations)Paragraph(Limitation,Amber);
    }
    if(Surface)
    {
        Paragraph(Surface->Title,Text);
        Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Derived triangles"),Label(FString::FromInt(Surface->Surface->TriangleCount()),9))];
        Paragraph(Surface->Method);
        Paragraph(Surface->BoundaryOrigin);
        for(const auto& Limitation:Surface->Limitations)Paragraph(Limitation,Amber);
    }
    else if(!Volume)Paragraph(Supported?TEXT("No reconstruction is attached. Original-point playback remains available."):
        TEXT("Reconstruction requires an imported point recording."),Amber);
    auto Choose=[this,Available,SameSource,Path](bool Relocate)
    {
        if(!Available())return;
        FSlateApplication::Get().DismissAllMenus();FString Selected;
        if(StudioFileDialog::ReconstructionFolder(Path,Selected)&&SameSource())M->RequestReconstruction(Selected,Relocate);
    };
    auto Import=Button(Is3D?(Volume?TEXT("Replace volume…"):TEXT("Import volume…")):(Surface?TEXT("Replace surface…"):TEXT("Import surface…")),TEXT("folder"),[Choose]{Choose(false);},Cyan);
    Import->SetTag(TEXT("ImportSurface"));Import->SetEnabled(TAttribute<bool>::CreateLambda([Available,Supported]{return Supported&&Available();}));
    Items->AddSlot().AutoHeight().Padding(0,3,0,7)[Import];
    auto Locate=Button(Is3D?TEXT("Locate matching volume…"):TEXT("Locate matching surface…"),TEXT("folder"),[Choose]{Choose(true);});
    Locate->SetTag(TEXT("LocateSurface"));Locate->SetEnabled(TAttribute<bool>::CreateLambda([Available,Path]{return !Path.IsEmpty()&&Available();}));
    Locate->SetToolTipText(FText::FromString(TEXT("Choose an identical copy of the saved reconstruction folder. Its content hashes must match.")));
    Items->AddSlot().AutoHeight().Padding(0,0,0,7)[Locate];
    auto Remove=Button(TEXT("Remove reconstruction"),TEXT("stop"),[this,Available]
    {if(Available()){FSlateApplication::Get().DismissAllMenus();M->RemoveReconstruction();}});
    Remove->SetTag(TEXT("RemoveSurface"));Remove->SetEnabled(TAttribute<bool>::CreateLambda([Available,Path]{return !Path.IsEmpty()&&Available();}));
    Remove->SetToolTipText(FText::FromString(TEXT("Return to original-point mode. Recording and reconstruction files stay on disk.")));
    Items->AddSlot().AutoHeight().Padding(0,0,0,7)[Remove];
    Items->AddSlot().AutoHeight().Padding(0,5,0,5)[Live([this,SameSource]
    {return !SameSource()?TEXT("Source changed. Reopen this menu."):M->IsRecordingLoadPending()?TEXT("Verifying source and surface… current view retained."):
        M->HasActiveJob()?TEXT("Finish the active control job before changing source assets."):TEXT("Save the project to retain its chosen reconstruction.");},9,Muted,true)];
    auto Cancel=Button(TEXT("Cancel loading"),TEXT("stop"),[this,SameSource]{if(SameSource())M->CancelRecording();});
    Cancel->SetTag(TEXT("CancelSurfaceLoading"));Cancel->SetEnabled(TAttribute<bool>::CreateLambda([this,SameSource]{return SameSource()&&M->bRecordingLoading;}));
    Items->AddSlot().AutoHeight()[Cancel];
    return SNew(SBox).WidthOverride(380).MaxDesiredHeight(480)[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)
        [SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(10)
            +SScrollBox::Slot()[Items]]];
}
TSharedRef<SWidget> SStudioWorkspace::AssetMenu()
{
    M->RefreshAssets();
    TSharedPtr<SVerticalBox> Rows;
    auto Refresh=Button(TEXT("Check files"),TEXT("orbit"),[this]{M->RefreshAssets();});
    Refresh->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !M->IsCheckingAssets();}));
    auto Cancel=Button(TEXT("Cancel check"),TEXT("stop"),[this]{M->CancelAssetCheck();});
    Cancel->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->IsCheckingAssets();}));
    AssetRefreshButton=Refresh;
    auto Widget=SNew(SBox).WidthOverride(620).MaxDesiredHeight(500)
    [SNew(SBorder).BorderImage(&PanelBrush).Padding(16)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Geometry files"),13,Text,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("Locate an identical original to preserve patches and run settings."),10,Muted)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[Refresh]
                +SHorizontalBox::Slot().AutoWidth()[Cancel]
                +SHorizontalBox::Slot().FillWidth(1).Padding(12,0).VAlign(VAlign_Center)
                    [Live([this]{return M->IsCheckingAssets()?TEXT("Verifying files…"):TEXT("Last check results");},9,Muted)]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Live([this]{return M->Notice;},10,Muted,true)]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12.f)
                +SScrollBox::Slot()[SAssignNew(Rows,SVerticalBox)]]]];
    AssetRows=Rows;RefreshAssetRows();return Widget;
}
void SStudioWorkspace::RefreshAssetRows()
{
    auto Rows=AssetRows.Pin();if(!Rows) return;
    const auto Focused=FSlateApplication::Get().GetKeyboardFocusedWidget();FString FocusKey;
    for(const auto& Item:AssetLocateButtons) if(Item.Value.Pin()==Focused) {FocusKey=Item.Key;break;}
    Rows->ClearChildren();AssetLocateButtons.Reset();
    if(M->AssetReferences.IsEmpty()) Rows->AddSlot().AutoHeight().Padding(0,8)
        [Label(TEXT("No geometry files are referenced by this project or its runs."),10,Muted)];
    for(const auto& Ref:M->AssetReferences)
    {
        auto Locate=Button(TEXT("Locate…"),TEXT("folder"),[this,Ref]
        {
            FString Path;
            if(StudioFileDialog::Geometry(Ref.Path,Path)) M->LocateAsset(Ref,Path);
        });
        Locate->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !M->IsCheckingAssets();}));
        Locate->SetToolTipText(FText::FromString(TEXT("Select a byte-identical copy; different contents are rejected")));
        AssetLocateButtons.Add(Ref.Key(),Locate);
        const FString Usage=(Ref.bDraft?TEXT("Current case"):TEXT("Older run only"))+
            (Ref.RunCount?FString::Printf(TEXT(" · %d saved run%s"),Ref.RunCount,Ref.RunCount==1?TEXT(""):TEXT("s")):TEXT(""));
        const auto Color=Ref.State==EStudioAssetState::Verified?Green:Ref.State==EStudioAssetState::Unchecked?Muted:Amber;
        Rows->AddSlot().AutoHeight().Padding(0,0,0,14)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(1)[SNew(SBorder).BorderImage(&LineBrush)]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,5)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Live([Name=Ref.Name]{return Name;},11,Text,true)]
                +SHorizontalBox::Slot().AutoWidth().Padding(10,0).VAlign(VAlign_Center)[Label(StudioAssets::StateText(Ref.State),9,Color)]
                +SHorizontalBox::Slot().AutoWidth()[Locate]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,4)[Label(Usage,9,Muted)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Live([Path=Ref.Path]{return Path;},9,Muted,true)]
            +SVerticalBox::Slot().AutoHeight()[Live([Detail=Ref.Detail]{return Detail;},9,Color,true)]];
    }
    LastAssetRevision=M->AssetRevision;
    if(!FocusKey.IsEmpty())
    {
        if(const auto* Button=AssetLocateButtons.Find(FocusKey)) FSlateApplication::Get().SetKeyboardFocus(Button->Pin(),EFocusCause::Navigation);
        else if(auto Refresh=AssetRefreshButton.Pin()) FSlateApplication::Get().SetKeyboardFocus(Refresh,EFocusCause::Navigation);
    }
}

/*
THESIS: Preview the original geometry, choose its physical scale and orientation, then commit one reversible case edit.
OWN-WORLD: The existing blue-black native Slate panels, fine borders, compact labels and cyan active controls.
STORY: Choose STL/OBJ → inspect shape and diagnostics → choose units/axes → import → save; recorded CFD retains its own boundary.
FIRST VIEWPORT: Object list on the left, interactive mesh preview in the center, scrollable import settings on the right; actions remain adjacent to their state.
FORM: A local authoring extension of the reference-led application. No replacement visual world, generated imagery or concept selection.
FINISH: unreviewed and undocumented is unfinished; finish with scoped native captures, review and DESIGN.md.
*/
void SStudioWorkspace::ImportGeometry()
{
    if(M->IsReadingGeometry()){M->GeometryNotice=TEXT("Wait for the current read or cancel it before choosing another file.");Navigate(EStudioWorkspace::Geometry);return;}
    FString Path;
    if(StudioFileDialog::ImportGeometry(Path)&&M->RequestGeometryImport(Path))Navigate(EStudioWorkspace::Geometry);
}
struct FStudioGeometryForm
{
    FStudioGeometryEdit Edit;
    bool bConflict=false,bSynchronizing=false,bRemoved=false;
    TWeakPtr<SEditableTextBox> Inputs[10];
    TWeakPtr<SWidget> RemovedAction;
    FString& Value(int32 Index){return Index==0?Edit.Name:Edit.Values[Index-1];}
    void Synchronize()
    {
        bSynchronizing=true;
        for(int32 Index=0;Index<10;++Index)if(auto Input=Inputs[Index].Pin())Input->SetText(FText::FromString(Value(Index)));
        bSynchronizing=false;
    }
    void FocusError()
    {
        if(Edit.ErrorField>=0&&Edit.ErrorField<10)
            if(auto Input=Inputs[Edit.ErrorField].Pin())FSlateApplication::Get().SetKeyboardFocus(Input,EFocusCause::Navigation);
    }
};
struct FStudioGeometryWorkspaceState
{
    FGuid Project,SourceSelection,Selected,Presented,PendingSelection;
    int64 Revision=-1;
    bool bImport=false;
    FString Notice;
    TMap<FGuid,TSharedPtr<FStudioGeometryForm>> Drafts;
    TMap<FGuid,TSharedPtr<SWidget>> Editors;
    TSharedPtr<SBox> Details;
    TSharedPtr<SVerticalBox> Removed;
};

void SStudioWorkspace::RefreshGeometryObjects()
{
    LastGeometryRevision=M->GeometryRevision;GeometryProjectId=M->Project.Id;GeometryCaseRevision=M->Project.Draft.Revision;
    if(!GeometryObjectRows)return;GeometryObjectRows->ClearChildren();
    if(M->Project.Draft.Geometry.IsEmpty())
    {GeometryObjectRows->AddSlot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(TEXT("Import an STL or OBJ to add geometry to this case."))).Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)];return;}
    for(const auto& Asset:M->Project.Draft.Geometry)
    {
        const FGuid Id=Asset.Id;
        GeometryObjectRows->AddSlot().AutoHeight().Padding(0,0,0,5)
        [SNew(SButton).Tag(FName(*(TEXT("GeometryRow_")+Id.ToString()))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,8)).IsEnabled_Lambda([this]{return !M->IsReadingGeometry()&&!M->IsProjectOpenPending();})
            .ToolTipText(FText::FromString(Asset.SourcePath)).OnClicked_Lambda([this,Id]{M->SelectGeometry(Id);return FReply::Handled();})
            [SNew(STextBlock).Text_Lambda([this,Id,Name=Asset.Name]
                {const auto* Form=GeometryState?GeometryState->Drafts.Find(Id):nullptr;return FText::FromString(Name+(Form&&((*Form)->Edit.IsDirty()||(*Form)->bConflict)?TEXT(" *"):TEXT("")));}).Font(Font(10)).AutoWrapText(true)
                .ColorAndOpacity_Lambda([this,Id]{return M->SelectedGeometry==Id?Cyan:Text;})]];
    }
}

void SStudioWorkspace::RefreshGeometryEditor()
{
    if(!GeometryState||!GeometryState->Details)return;
    auto& State=*GeometryState;
    if(State.Project!=M->Project.Id)
    {
        State.Project=M->Project.Id;State.Revision=-1;State.Drafts.Empty();State.Editors.Empty();
        State.SourceSelection.Invalidate();State.Selected.Invalidate();State.Presented.Invalidate();
        State.PendingSelection.Invalidate();
        State.Details->SetContent(SNullWidget::NullWidget);State.Notice.Empty();State.bImport=false;
    }
    if(State.PendingSelection.IsValid()&&!M->IsReadingGeometry()&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending())
    {
        const FGuid Id=State.PendingSelection;State.PendingSelection.Invalidate();
        if(M->Project.Draft.Geometry.ContainsByPredicate([Id](const auto& Asset){return Asset.Id==Id;}))M->SelectGeometry(Id);
    }
    if(State.SourceSelection!=M->SelectedGeometry||State.bImport!=M->bImportPreview)
    {
        State.SourceSelection=M->SelectedGeometry;State.bImport=M->bImportPreview;
        State.Selected=M->bImportPreview?FGuid():M->SelectedGeometry;
    }
    if(State.Revision!=M->Project.Draft.Revision)
    {
        State.Revision=M->Project.Draft.Revision;
        State.Removed->ClearChildren();
        for(auto It=State.Drafts.CreateIterator();It;++It)
        {
            const FGuid Id=It.Key();const auto Form=It.Value();
            const auto* Applied=M->Project.Draft.Geometry.FindByPredicate([Id](const auto& Asset){return Asset.Id==Id;});
            Form->bRemoved=!Applied;
            if(!Applied)
            {
                if(!Form->Edit.IsDirty()&&!Form->bConflict){State.Editors.Remove(Id);It.RemoveCurrent();continue;}
                Form->bConflict=true;
                auto Discard=Button(TEXT("Discard removed object's edits"),TEXT("stop"),[this,Id]
                {
                    GeometryState->Editors.Remove(Id);GeometryState->Drafts.Remove(Id);GeometryState->Revision=-1;
                    GeometryState->Selected.Invalidate();GeometryState->Presented.Invalidate();
                    if(GeometryState->PendingSelection==Id)GeometryState->PendingSelection.Invalidate();
                    GeometryState->Details->SetContent(SNullWidget::NullWidget);
                    GeometryState->Notice=TEXT("Removed object's unapplied edits discarded.");
                    ResolveSaveNotice(*M,GeometrySaveGuard,GeometryState->Notice);
                    ResolveSaveNotice(*M,RemovedGeometrySaveGuard,GeometryState->Notice);RefreshGeometryEditor();
                });
                Discard->SetTag(TEXT("DiscardRemovedGeometryDraft"));
                Form->RemovedAction=Discard;
                State.Removed->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Live([Name=Form->Edit.Saved.Name]
                        {return Name+TEXT(" was removed. Its unapplied edits are retained.");},10,Amber,true)]
                    +SVerticalBox::Slot().AutoHeight()[Discard]];
            }
            else if(!Form->Edit.Matches(*Applied))
            {
                if(Form->Edit.IsDirty()||Form->bConflict)Form->bConflict=true;
                else {Form->Edit.Reset(*Applied);Form->Synchronize();}
            }
        }
    }
    if(State.Selected.IsValid()&&!State.Drafts.Contains(State.Selected))
        if(const auto* Asset=M->Project.Draft.Geometry.FindByPredicate([&State](const auto& Item){return Item.Id==State.Selected;}))
        {
            const auto Form=MakeShared<FStudioGeometryForm>();Form->Edit.Reset(*Asset);State.Drafts.Add(State.Selected,Form);
        }
    if(State.Selected!=State.Presented)
    {
        State.Presented=State.Selected;
        State.Details->SetContent(State.Drafts.Contains(State.Selected)?GeometryObjectEditor(State.Selected):SNullWidget::NullWidget);
    }
}

bool SStudioWorkspace::EnsureGeometryResolved()
{
    RefreshGeometryEditor();
    if(!GeometryState||GeometryState->Project!=M->Project.Id)return true;
    for(const auto& Pair:GeometryState->Drafts)
    {
        if(!Pair.Value->Edit.IsDirty()&&!Pair.Value->bConflict)continue;
        M->Notice=GeometryState->Notice=Pair.Value->bRemoved?RemovedGeometrySaveGuard:GeometrySaveGuard;
        Navigate(EStudioWorkspace::Geometry);
        if(M->IsReadingGeometry())GeometryState->PendingSelection=Pair.Key;
        else {GeometryState->PendingSelection.Invalidate();M->SelectGeometry(Pair.Key);}
        GeometryState->SourceSelection=M->SelectedGeometry;GeometryState->Selected=Pair.Key;
        RefreshGeometryEditor();
        if(Pair.Value->bRemoved)
        {if(const auto Action=Pair.Value->RemovedAction.Pin())FSlateApplication::Get().SetKeyboardFocus(Action,EFocusCause::Navigation);}
        else if(const auto Field=Pair.Value->Inputs[0].Pin())FSlateApplication::Get().SetKeyboardFocus(Field,EFocusCause::Navigation);
        return false;
    }
    return true;
}

// THESIS: Position a verified case object with exact retained edits and one Apply.
// OWN-WORLD: Existing compact native Geometry inspector, cyan actions and amber recovery.
// STORY: Select an object, edit name/pose/scale, apply, inspect and save; undo restores it.
// FIRST VIEWPORT: Object list and applied mesh stay visible beside the editable transform.
// FORM: Local Operate extension; source provenance and observing cameras stay independent.
// FINISH: Two-size native evidence, scoped finish review and documentation.
TSharedRef<SWidget> SStudioWorkspace::GeometryObjectEditor(const FGuid& Id)
{
    if(const auto* Existing=GeometryState->Editors.Find(Id))return Existing->ToSharedRef();
    const auto Form=GeometryState->Drafts.FindChecked(Id);const FGuid ProjectId=M->Project.Id;
    auto Current=[this,Id,ProjectId]
    {return M->Project.Id==ProjectId&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending()&&
        M->Project.Draft.Geometry.ContainsByPredicate([Id](const auto& Asset){return Asset.Id==Id;});};
    auto Apply=[this,Id,Form,Current]
    {
        if(!Current()||Form->bConflict)return;
        if(!M->UpdateGeometry(Form->Edit)){Form->FocusError();return;}
        const auto* Asset=M->Project.Draft.Geometry.FindByPredicate([Id](const auto& Item){return Item.Id==Id;});
        Form->Edit.Reset(*Asset);Form->Synchronize();GeometryState->Notice=TEXT("Object changes applied.");
        RefreshGeometryEditor();
    };
    auto Input=[this,Form,Apply,Current](int32 Index,const TCHAR* Tag)
    {
        auto Widget=SNew(SEditableTextBox).Tag(Tag).Style(&InputStyle()).Font(Font(10))
            .Text(FText::FromString(Form->Value(Index))).SelectAllTextWhenFocused(true).ClearKeyboardFocusOnCommit(false)
            .IsEnabled_Lambda(Current).ToolTipText_Lambda([Form,Index]{return FText::FromString(Form->Value(Index));})
            .OnTextChanged_Lambda([this,Form,Index](const FText& Value)
            {
                if(Form->bSynchronizing)return;
                Form->Value(Index)=Value.ToString();
                if(!Form->Edit.Error.IsEmpty())
                {
                    if(M->GeometryNotice==Form->Edit.Error)M->GeometryNotice.Empty();
                    if(M->Notice==Form->Edit.Error)M->Notice.Empty();
                }
                Form->Edit.Error.Empty();Form->Edit.ErrorField=INDEX_NONE;
                if(GeometryState->Notice!=GeometrySaveGuard)GeometryState->Notice.Empty();
            })
            .OnTextCommitted_Lambda([Apply](const FText&,ETextCommit::Type How){if(How==ETextCommit::OnEnter)Apply();});
        Form->Inputs[Index]=Widget;return Widget;
    };
    auto Fields=SNew(SVerticalBox);
    Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Object name"),10,Muted)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[Input(0,TEXT("GeometryEditName"))];
    const TCHAR* Headings[]={TEXT("Position (m)"),TEXT("Rotation (°)"),TEXT("Scale · source axes")};
    const TCHAR* Labels[]={TEXT("X"),TEXT("Y"),TEXT("Z"),TEXT("Roll · X"),TEXT("Pitch · Y"),TEXT("Yaw · Z"),TEXT("X"),TEXT("Y"),TEXT("Z")};
    for(int32 Group=0;Group<3;++Group)
    {
        Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(Headings[Group],10,Muted)];
        auto RowBox=SNew(SHorizontalBox);
        for(int32 Axis=0;Axis<3;++Axis)
        {
            const int32 Index=Group*3+Axis;
            RowBox->AddSlot().FillWidth(1).Padding(0,0,Axis<2?6:0,0)[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,4)[Label(Labels[Index],9,Muted)]
                +SVerticalBox::Slot().AutoHeight()[Input(Index+1,*FString::Printf(TEXT("GeometryEditValue%d"),Index))]];
        }
        Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[RowBox];
    }
    Fields->AddSlot().AutoHeight().Padding(0,0,0,10)[Live([]
        {return TEXT("Scale uses original mesh axes. Rotation is about the original origin; position is in case meters. Import units remain unchanged.");},9,Muted,true)];
    auto ApplyButton=Button(TEXT("Apply changes"),TEXT("check"),Apply,Cyan);ApplyButton->SetTag(TEXT("ApplyGeometryEdits"));
    ApplyButton->SetEnabled(TAttribute<bool>::CreateLambda([this,Id,Form,Current]
        {return Current()&&!Form->bConflict&&Form->Edit.IsDirty()&&!M->bImportPreview&&!M->IsReadingGeometry()&&
            M->SelectedGeometry==Id&&M->GeometrySource.IsValid();}));
    auto Revert=Button(TEXT("Revert edits"),TEXT("undo"),[this,Id,Form,Current]
    {
        if(!Current())return;
        const auto* Applied=M->Project.Draft.Geometry.FindByPredicate([Id](const auto& Asset){return Asset.Id==Id;});
        Form->Edit.Reset(*Applied);Form->bConflict=false;Form->Synchronize();
        GeometryState->Notice=TEXT("Applied object values restored.");ResolveSaveNotice(*M,GeometrySaveGuard,GeometryState->Notice);
        ResolveSaveNotice(*M,RemovedGeometrySaveGuard,GeometryState->Notice);
    });
    Revert->SetTag(TEXT("RevertGeometryEdits"));
    Revert->SetEnabled(TAttribute<bool>::CreateLambda([Form,Current]{return Current()&&(Form->Edit.IsDirty()||Form->bConflict);}));
    Fields->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[ApplyButton]
        +SHorizontalBox::Slot().FillWidth(1)[Revert]];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,10)[Live([Form]
        {return Form->bRemoved?TEXT("This object is no longer in the case. Discard its unapplied edits above."):
            Form->bConflict?TEXT("This object changed outside the form. Revert before applying."):
            !Form->Edit.Error.IsEmpty()?Form->Edit.Error:Form->Edit.IsDirty()?TEXT("Unapplied edits. The viewport still shows the applied object."):FString();},10,Amber,true)];
    auto Widget=SNew(SBox).Tag(TEXT("GeometryObjectEditor"))[Fields];
    GeometryState->Editors.Add(Id,Widget);return Widget;
}
struct FStudioMaterialForm
{
    FStudioMaterialEdit Edit;
    bool bConflict=false;
    bool bSynchronizing=false;
    TWeakPtr<SEditableTextBox> Name;
    TWeakPtr<SEditableTextBox> Inputs[4];
    void Synchronize()
    {
        bSynchronizing=true;
        if(auto Field=Name.Pin())Field->SetText(FText::FromString(Edit.Name));
        for(int32 I=0;I<4;++I)if(auto Field=Inputs[I].Pin())Field->SetText(FText::FromString(Edit.Values[I]));
        bSynchronizing=false;
    }
};
struct FStudioMaterialWorkspaceState
{
    FGuid Project,Selected,Presented;
    int64 Revision=-1;
    TMap<FGuid,TSharedPtr<FStudioMaterialForm>> Drafts;
    TMap<FGuid,TSharedPtr<SWidget>> Editors;
    TSharedPtr<SVerticalBox> Rows,Assignments;
    TSharedPtr<SBox> Details;
    FString Notice;
};

// THESIS: Materials belong to the editable case, with unknown values left unknown.
// OWN-WORLD: Existing blue-black Slate panels, compact SI fields, cyan selection.
// STORY: Add a fluid or solid, enter known properties, apply and assign to the case.
// FIRST VIEWPORT: Material list left, retained property form center, assignments right.
// FORM: Native Operate extension, one sidebar route, no duplicate settings elsewhere.
// FINISH: Native two-size evidence, independent finish review and design documentation.
TSharedRef<SWidget> SStudioWorkspace::MaterialsWorkspace()
{
    MaterialsState=MakeShared<FStudioMaterialWorkspaceState>();
    auto Available=[this]{return !M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();};
    auto Add=[this](bool Solid)
    {FGuid Id;if(M->AddMaterial(Solid,Id)){MaterialsState->Selected=Id;MaterialsState->Notice=TEXT("Material added. Enter known properties, then apply.");RefreshMaterials();}else MaterialsState->Notice=M->Notice;};
    auto Fluid=Button(TEXT("Add fluid"),TEXT("plus"),[Add]{Add(false);},Cyan);Fluid->SetTag(TEXT("AddFluidMaterial"));Fluid->SetEnabled(TAttribute<bool>::CreateLambda(Available));
    auto Solid=Button(TEXT("Add solid"),TEXT("plus"),[Add]{Add(true);});Solid->SetTag(TEXT("AddSolidMaterial"));Solid->SetEnabled(TAttribute<bool>::CreateLambda(Available));
    auto Undo=Button(TEXT("Undo case"),TEXT("undo"),[this]{M->UndoCase();RefreshMaterials();});Undo->SetTag(TEXT("MaterialsUndo"));
    Undo->SetEnabled(TAttribute<bool>::CreateLambda([this,Available]{return Available()&&M->CanUndoCase();}));
    auto Redo=Button(TEXT("Redo case"),TEXT("redo"),[this]{M->RedoCase();RefreshMaterials();});Redo->SetTag(TEXT("MaterialsRedo"));
    Redo->SetEnabled(TAttribute<bool>::CreateLambda([this,Available]{return Available()&&M->CanRedoCase();}));
    return SNew(SBorder).Tag(TEXT("MaterialsWorkspace")).BorderImage(&PanelBrush).Padding(18)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Materials"),20,Text,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,16)[Label(TEXT("Case properties and assignments. Blank values remain unknown."),10,Muted)]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,16,0)[SNew(SBox).WidthOverride(210)
                    [SNew(SVerticalBox)
                        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Fluid]
                        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[Solid]
                        +SVerticalBox::Slot().FillHeight(1)[SNew(SRetainedFormScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                            +SScrollBox::Slot()[SAssignNew(MaterialsState->Rows,SVerticalBox)]]
                        +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,6)[Undo]
                        +SVerticalBox::Slot().AutoHeight()[Redo]]]
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,18,0)[SNew(SRetainedFormScrollBox).Tag(TEXT("MaterialPropertiesScroll"))
                    .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(8)
                    +SScrollBox::Slot()[SAssignNew(MaterialsState->Details,SBox)]]
                +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(280)[SNew(SRetainedFormScrollBox)
                    .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(8)
                    +SScrollBox::Slot()[SAssignNew(MaterialsState->Assignments,SVerticalBox)]]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,14,0,0)
                [Live([this]{return MaterialsState->Notice.IsEmpty()?TEXT("Properties are stored in SI units. Solver-specific validation is not available until an adapter supplies its rules."):MaterialsState->Notice;},10,Muted,true)]];
}

TSharedRef<SWidget> SStudioWorkspace::MaterialDetails(const FGuid& Id)
{
    if(const auto* Existing=MaterialsState->Editors.Find(Id))return Existing->ToSharedRef();
    const auto Form=MaterialsState->Drafts.FindChecked(Id);
    const FGuid ProjectId=M->Project.Id;
    auto Current=[this,Id,ProjectId,Form]
    {return M->Project.Id==ProjectId&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending()&&!Form->bConflict&&
        M->Project.Draft.Materials.ContainsByPredicate([Id](const auto& Item){return Item.Id==Id;});};
    auto Apply=[this,Form,Current]
    {
        if(!Current())return;
        const auto* Saved=M->Project.Draft.Materials.FindByPredicate([Id=Form->Edit.Saved.Id](const auto& Item){return Item.Id==Id;});
        if(!Saved||!Form->Edit.Matches(*Saved)){Form->bConflict=true;return;}
        FStudioMaterial Candidate;
        if(!Form->Edit.Build(Candidate))return;
        if(!M->UpdateMaterial(Candidate)){Form->Edit.Error=M->Notice;return;}
        Form->Edit.Reset(Candidate,true);Form->Synchronize();MaterialsState->Notice=TEXT("Material saved in the case. Recording and existing runs are unchanged.");
        ResolveSaveNotice(*M,MaterialSaveGuard,MaterialsState->Notice);RefreshMaterials();
    };
    auto Fields=SNew(SVerticalBox);
    auto Name=SNew(SEditableTextBox).Tag(TEXT("MaterialName")).Style(&InputStyle()).Font(Font(10))
        .Text(FText::FromString(Form->Edit.Name)).SelectAllTextWhenFocused(true).ClearKeyboardFocusOnCommit(false)
        .OnTextChanged_Lambda([Form](const FText& Value){if(!Form->bSynchronizing){Form->Edit.Name=Value.ToString();Form->Edit.Error.Empty();}})
        .OnTextCommitted_Lambda([Apply](const FText&,ETextCommit::Type How){if(How==ETextCommit::OnEnter)Apply();});
    Form->Name=Name;
    Fields->AddSlot().AutoHeight().Padding(0,0,0,7)[Label(TEXT("Name"),10,Muted)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,14)[Name];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,16)[Row(TEXT("Material type"),SNew(SStudioMenuButton).Tag(TEXT("MaterialType"))
        .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6)).OnGetMenuContent_Lambda([Form]
        {
            auto Choices=SNew(SVerticalBox);
            for(bool Solid:{false,true})Choices->AddSlot().AutoHeight()[Button(Solid?TEXT("Solid"):TEXT("Fluid"),TEXT("box"),[Form,Solid]
                {FSlateApplication::Get().DismissAllMenus();Form->Edit.bSolid=Solid;Form->Edit.Error.Empty();})];
            return Choices;
        }).ButtonContent()[Live([Form]{return Form->Edit.bSolid?TEXT("Solid"):TEXT("Fluid");})],150)];
    for(int32 I=0;I<4;++I)
    {
        const auto Property=EStudioMaterialProperty(I);
        auto Editor=SNew(SProjectFilterBox).Tag(FName(*FString::Printf(TEXT("MaterialProperty%d"),I)))
            .Style(&InputStyle()).Font(Font(10)).Text(FText::FromString(Form->Edit.Values[I]))
            .HintText(FText::FromString(TEXT("Unknown"))).SelectAllTextWhenFocused(true).ClearKeyboardFocusOnCommit(false)
            .OnTextChanged_Lambda([Form,I](const FText& Value){if(!Form->bSynchronizing){Form->Edit.Values[I]=Value.ToString();Form->Edit.Error.Empty();Form->Edit.ErrorProperty=INDEX_NONE;}})
            .OnTextCommitted_Lambda([Apply](const FText&,ETextCommit::Type How){if(How==ETextCommit::OnEnter)Apply();});
        Form->Inputs[I]=Editor;
        TSharedRef<SWidget> Unit=Label(StudioMaterials::UnitLabel(Property,0),9,Muted);
        if(StudioMaterials::UnitCount(Property)>1)
            Unit=SNew(SStudioMenuButton).Tag(FName(*FString::Printf(TEXT("MaterialUnit%d"),I)))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,6)).OnGetMenuContent_Lambda([Form,Property]
                {
                    auto Choices=SNew(SVerticalBox);
                    for(int32 U=0;U<StudioMaterials::UnitCount(Property);++U)
                    {
                        auto Choice=Button(StudioMaterials::UnitLabel(Property,U),TEXT("select"),[Form,Property,U]
                        {FSlateApplication::Get().DismissAllMenus();if(Form->Edit.ChangeUnit(Property,U))Form->Synchronize();});
                        Choice->SetTag(FName(*FString::Printf(TEXT("MaterialUnitOption%d_%d"),int32(Property),U)));Choices->AddSlot().AutoHeight()[Choice];
                    }
                    return Choices;
                }).ButtonContent()[Live([Form,Property]{return FString(StudioMaterials::UnitLabel(Property,Form->Edit.Units[int32(Property)]));},9)];
        Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(StudioMaterials::PropertyName(Property),10,Muted)];
        Fields->AddSlot().AutoHeight().Padding(0,0,0,14)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[Editor]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8,0,0,0)[SNew(SBox).WidthOverride(100)[Unit]]];
    }
    Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[Live([Form]
    {return Form->bConflict?TEXT("This material changed through undo or another edit. Your text is retained. Use Revert to load the saved values before applying."):Form->Edit.Error;},10,Amber,true)];
    auto ApplyButton=Button(TEXT("Apply properties"),TEXT("check"),Apply,Cyan);ApplyButton->SetTag(TEXT("ApplyMaterial"));
    ApplyButton->SetEnabled(TAttribute<bool>::CreateLambda([Current,Form]{return Current()&&Form->Edit.IsDirty();}));
    auto Revert=Button(TEXT("Revert"),TEXT("undo"),[this,Form,Id,ProjectId]
    {if(M->Project.Id!=ProjectId)return;if(const auto* Saved=M->Project.Draft.Materials.FindByPredicate([Id](const auto& Item){return Item.Id==Id;}))
        {Form->Edit.Reset(*Saved,true);Form->bConflict=false;Form->Synchronize();
            MaterialsState->Notice=TEXT("Loaded the applied material properties.");ResolveSaveNotice(*M,MaterialSaveGuard,MaterialsState->Notice);}});
    Revert->SetTag(TEXT("RevertMaterial"));Revert->SetEnabled(TAttribute<bool>::CreateLambda([Form]{return Form->bConflict||Form->Edit.IsDirty();}));
    Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8,6))
        +SWrapBox::Slot()[ApplyButton]+SWrapBox::Slot()[Revert]];
    Fields->AddSlot().AutoHeight()[Live([Form]{return Form->Edit.IsDirty()?TEXT("Unapplied properties are retained when switching materials or workspaces. Apply before saving the project."):TEXT("Applied properties are included when you save the project.");},9,Muted,true)];
    MaterialsState->Editors.Add(Id,Fields);return Fields;
}

void SStudioWorkspace::RefreshMaterials()
{
    if(!MaterialsState||!MaterialsState->Rows||!MaterialsState->Details||!MaterialsState->Assignments)return;
    auto& State=*MaterialsState;
    if(State.Project==M->Project.Id&&State.Revision==M->Project.Draft.Revision&&State.Presented==State.Selected)return;
    if(State.Project!=M->Project.Id)
    {FSlateApplication::Get().DismissAllMenus();State.Project=M->Project.Id;State.Selected.Invalidate();State.Drafts.Empty();State.Editors.Empty();State.Notice.Empty();}
    State.Revision=M->Project.Draft.Revision;
    for(auto It=State.Drafts.CreateIterator();It;++It)
        if(!M->Project.Draft.Materials.ContainsByPredicate([Id=It.Key()](const auto& Material){return Material.Id==Id;}))
        {State.Editors.Remove(It.Key());It.RemoveCurrent();}
    for(const auto& Material:M->Project.Draft.Materials)
    {
        auto& Form=State.Drafts.FindOrAdd(Material.Id);
        if(!Form){Form=MakeShared<FStudioMaterialForm>();Form->Edit.Reset(Material);}
        else if(!Form->Edit.Matches(Material))
        {
            if(Form->Edit.IsDirty())Form->bConflict=true;
            else {Form->Edit.Reset(Material,true);Form->bConflict=false;Form->Synchronize();}
        }
        else Form->bConflict=false;
    }
    if(!State.Drafts.Contains(State.Selected))State.Selected=M->Project.Draft.Materials.IsEmpty()?FGuid():M->Project.Draft.Materials[0].Id;
    State.Presented=State.Selected;
    const auto Focused=FSlateApplication::Get().GetKeyboardFocusedWidget();FGuid FocusId;
    const FName FocusTag=Focused?Focused->GetTag():NAME_None;
    if(Focused)for(const auto& Material:M->Project.Draft.Materials)
        if(FocusTag==FName(*(FString(TEXT("MaterialRow_"))+Material.Id.ToString())))FocusId=Material.Id;
    State.Rows->ClearChildren();State.Assignments->ClearChildren();
    TSharedPtr<SWidget> RestoreFocus;
    for(const auto& Material:M->Project.Draft.Materials)
    {
        const FGuid Id=Material.Id;
        auto Select=SNew(SButton).Tag(FName(*(FString(TEXT("MaterialRow_"))+Id.ToString()))).ButtonStyle(&ButtonStyle())
            .ContentPadding(FMargin(10,9)).OnClicked_Lambda([this,Id]{MaterialsState->Selected=Id;RefreshMaterials();return FReply::Handled();})
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[Live([this,Id]
                {const auto* Item=M->Project.Draft.Materials.FindByPredicate([Id](const auto& V){return V.Id==Id;});const auto* Draft=MaterialsState->Drafts.Find(Id);return Item?Item->Name+(Draft&&(*Draft)->Edit.IsDirty()?TEXT(" *"):TEXT("")):FString();},10,State.Selected==Id?Cyan:Text,true)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[Label(FString(Material.bSolid?TEXT("Solid"):TEXT("Fluid"))+FString::Printf(TEXT(" · %d assignments"),StudioMaterials::AssignmentCount(M->Project.Draft,Id)),9,Muted)]];
        State.Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Select];if(Id==FocusId)RestoreFocus=Select;
    }
    if(!State.Selected.IsValid())
    {
        State.Details->SetContent(SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("No materials yet"),14,Text,true)]
            +SVerticalBox::Slot().AutoHeight()[Live([]{return TEXT("Add a fluid or solid to define the material properties for your case. No properties are inferred from the recorded CFD.");},10,Muted,true)]);
        State.Assignments->AddSlot().AutoHeight()[Live([]{return TEXT("Assignments appear after you add a material. Domain and imported objects can be assigned separately.");},10,Muted,true)];return;
    }
    const FGuid Id=State.Selected,ProjectId=M->Project.Id;
    State.Details->SetContent(MaterialDetails(Id));
    const auto Form=State.Drafts.FindChecked(Id);
    auto Available=[this,ProjectId]{return M->Project.Id==ProjectId&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();};
    auto Duplicate=Button(TEXT("Duplicate"),TEXT("plus"),[this,Id,Available,Form]
    {if(!Available())return;if(Form->Edit.IsDirty()){MaterialsState->Notice=TEXT("Apply or revert this material before duplicating it.");return;}FGuid New;if(M->DuplicateMaterial(Id,New)){MaterialsState->Selected=New;RefreshMaterials();}else MaterialsState->Notice=M->Notice;});
    Duplicate->SetTag(TEXT("DuplicateMaterial"));Duplicate->SetEnabled(TAttribute<bool>::CreateLambda(Available));
    const int32 Count=StudioMaterials::AssignmentCount(M->Project.Draft,Id);
    auto Delete=Button(Count?TEXT("Unassign and delete"):TEXT("Delete material"),TEXT("stop"),[this,Id,Available,Form,Count]
    {
        if(!Available())return;
        if(Form->Edit.IsDirty()){MaterialsState->Notice=TEXT("Apply or revert this material before deleting it.");return;}
        if(M->DeleteMaterial(Id,Count>0)){MaterialsState->Notice=TEXT("Material removed from the case. Undo case restores its properties and assignments.");RefreshMaterials();}else MaterialsState->Notice=M->Notice;
    });
    Delete->SetTag(TEXT("DeleteMaterial"));Delete->SetEnabled(TAttribute<bool>::CreateLambda(Available));
    State.Assignments->AddSlot().AutoHeight().Padding(0,0,0,14)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Duplicate]
        +SVerticalBox::Slot().AutoHeight()[Delete]];
    State.Assignments->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("Assignments"),12,Text,true)];
    State.Assignments->AddSlot().AutoHeight().Padding(0,0,0,10)[Live([]{return TEXT("Assignments use applied properties. Changes are included in case undo and project saves.");},9,Muted,true)];
    const auto* Saved=M->Project.Draft.Materials.FindByPredicate([Id](const auto& Item){return Item.Id==Id;});
    auto Domain=SNew(SCheckBox).Tag(TEXT("MaterialDomainAssignment"))
        .IsEnabled_Lambda([Available,Solid=Saved->bSolid]{return Available()&&!Solid;})
        .IsChecked_Lambda([this,Id]{return M->Project.Draft.Domain.FluidMaterialId==Id?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([this,Id](ECheckBoxState Value){M->AssignDomainMaterial(Value==ECheckBoxState::Checked?Id:FGuid());RefreshMaterials();})
        [Label(TEXT("Fluid domain"),10)];
    State.Assignments->AddSlot().AutoHeight().Padding(0,0,0,6)[Domain];
    if(FocusTag==TEXT("MaterialDomainAssignment"))RestoreFocus=Domain;
    if(Saved->bSolid)State.Assignments->AddSlot().AutoHeight().Padding(0,0,0,10)[Live([]{return TEXT("A solid cannot be assigned as the domain fluid.");},9,Muted,true)];
    if(M->Project.Draft.Geometry.IsEmpty())State.Assignments->AddSlot().AutoHeight().Padding(0,8)[Live([]{return TEXT("Import geometry to assign materials to case objects.");},10,Muted,true)];
    for(const auto& Geometry:M->Project.Draft.Geometry)
    {
        const FGuid GeometryId=Geometry.Id;
        auto Assignment=SNew(SCheckBox).Tag(FName(*(FString(TEXT("MaterialGeometry_"))+GeometryId.ToString())))
            .IsEnabled_Lambda(Available).IsChecked_Lambda([this,GeometryId,Id]
            {const auto* Item=M->Project.Draft.Geometry.FindByPredicate([GeometryId](const auto& G){return G.Id==GeometryId;});return Item&&Item->MaterialId==Id?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([this,GeometryId,Id](ECheckBoxState Value){M->AssignGeometryMaterial(GeometryId,Value==ECheckBoxState::Checked?Id:FGuid());RefreshMaterials();})
            [Live([Name=Geometry.Name]{return Name;},10,Text,true)];
        State.Assignments->AddSlot().AutoHeight().Padding(0,6)[Assignment];
        if(FocusTag==Assignment->GetTag())RestoreFocus=Assignment;
    }
    State.Assignments->AddSlot().AutoHeight().Padding(0,18,0,0)[Live([]{return TEXT("A new assignment replaces that object's previous material. Existing run configurations keep their original materials.");},9,Muted,true)];
    if(RestoreFocus)FSlateApplication::Get().SetKeyboardFocus(RestoreFocus,EFocusCause::Navigation);
}

bool SStudioWorkspace::EnsureMaterialsResolved()
{
    RefreshMaterials();
    if(!MaterialsState||MaterialsState->Project!=M->Project.Id)return true;
    for(const auto& Pair:MaterialsState->Drafts)
        if(Pair.Value->Edit.IsDirty()||Pair.Value->bConflict)
        {
            MaterialsState->Selected=Pair.Key;
            MaterialsState->Notice=MaterialSaveGuard;
            M->Notice=MaterialsState->Notice;Navigate(EStudioWorkspace::Materials);RefreshMaterials();
            if(const auto Focus=Pair.Value->Name.Pin())FSlateApplication::Get().SetKeyboardFocus(Focus,EFocusCause::Navigation);
            return false;
        }
    return true;
}

struct FStudioDomainWorkspaceState
{
    FStudioDomainEdit Edit;
    FGuid Project;
    int64 Revision=-1;
    bool bConflict=false,bSynchronizing=false;
    FString Notice;
    uint64 EditRevision=0;
    TWeakPtr<SEditableTextBox> Inputs[21];
    bool CanPreviewDraft(const AStudioScene* Scene,FStudioDomain& Domain) const
    {
        if(!Scene||!Scene->Model||bConflict||Scene->Model->Workspace!=EStudioWorkspace::Domain||
            Project!=Scene->Model->Project.Id||Scene->Model->IsProjectOpenPending()||Scene->Model->IsRecordingLoadPending())return false;
        auto Draft=Edit;return Draft.Build(Domain);
    }
    FString& Value(int32 Index)
    {return Index<6?(Index%2?Edit.Maximum[Index/2]:Edit.Minimum[Index/2]):Index<12?Edit.FaceNames[Index-6]:Index<18?Edit.Padding[Index-12]:Edit.Dimensions[Index-18];}
    void Synchronize()
    {
        bSynchronizing=true;
        for(int32 I=0;I<21;++I)if(auto Input=Inputs[I].Pin())Input->SetText(FText::FromString(Value(I)));
        bSynchronizing=false;
    }
    void Change(int32 Index,const FString& ValueText)
    {
        if(bSynchronizing)return;
        if(Index<6)Edit.SetCoordinate(Index,ValueText);
        else if(Index>=18)Edit.SetDimension(Index-18,ValueText);
        else Value(Index)=ValueText;
        ++EditRevision;Edit.Error.Empty();if(!bConflict)Notice.Empty();
        bSynchronizing=true;
        const int32 Other=Index<6?18+Index/2:Index>=18?(Index-18)*2+1:INDEX_NONE;
        if(Other!=INDEX_NONE)if(auto Input=Inputs[Other].Pin())Input->SetText(FText::FromString(Value(Other)));
        bSynchronizing=false;
    }
    void FocusError()
    {if(Edit.ErrorField>=0&&Edit.ErrorField<21)if(auto Input=Inputs[Edit.ErrorField].Pin())FSlateApplication::Get().SetKeyboardFocus(Input,EFocusCause::SetDirectly);}
};
class SDomainViewport final : public SFlowViewport
{
public:
    SLATE_BEGIN_ARGS(SDomainViewport){}
        SLATE_ARGUMENT(AStudioScene*,Scene)
        SLATE_ARGUMENT(TSharedPtr<FStudioDomainWorkspaceState>,Form)
    SLATE_END_ARGS()
    void Construct(const FArguments& A)
    {Form=A._Form;SFlowViewport::Construct(SFlowViewport::FArguments().Scene(A._Scene));}
    ~SDomainViewport(){RestoreDrag();}
    bool CanEdit(FStudioDomain& Domain) const
    {
        return Scene.IsValid()&&Form&&Form->CanPreviewDraft(Scene.Get(),Domain);
    }
    double Aspect() const
    {const auto Size=Scene->PresentedViewportSize();return Size.Y>0?double(Size.X)/Size.Y:0.;}
    int32 Hit(const FGeometry& G,FVector2D Pixel,const FStudioDomain& Domain) const
    {
        double Best=100;int32 Face=INDEX_NONE;
        for(int32 I=0;I<6;++I)
        {
            FVector2D P;if(!StudioCameraPlacement::Project(Scene->PresentedCamera(),G.GetLocalSize(),StudioDomain::FaceCenter(Domain,I),P,Aspect()))continue;
            if(P.X<10||P.Y<10||P.X>G.GetLocalSize().X-28||P.Y>G.GetLocalSize().Y-20)continue;
            const double Distance=(Pixel-P).SizeSquared();if(Distance<Best){Best=Distance;Face=I;}
        }
        return Face;
    }
    bool Current(const FGeometry* G=nullptr) const
    {
        return FaceDrag.IsSet()&&Scene.IsValid()&&Form->Project==Scene->Model->Project.Id&&
            Scene->Model->Workspace==EStudioWorkspace::Domain&&Scene->Model->Project.Draft.Revision==CaseRevision&&
            Scene->Model->SelectedDomainFace==FaceDrag->Face&&!Form->bConflict&&Form->EditRevision==OwnedRevision&&
            StudioView::CameraEquals(Scene->CameraState(),FaceDrag->Observer)&&Aspect()==FaceDrag->ProjectionAspect&&
            (!G||G->GetLocalSize().Equals(FaceDrag->Viewport));
    }
    void RestoreDrag()
    {
        if(Current()){Form->Edit=OriginalDraft;Form->Synchronize();++Form->EditRevision;Form->Notice=TEXT("Face drag cancelled. Earlier edits retained.");}
        FaceDrag.Reset();
    }
    virtual int32 OnPaint(const FPaintArgs& Args,const FGeometry& G,const FSlateRect& Cull,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle& Style,bool Enabled) const override
    {
        Layer=SFlowViewport::OnPaint(Args,G,Cull,Out,Layer,Style,Enabled);
        FStudioDomain Domain;if(!CanEdit(Domain)||!Scene->HasDomainPreview())return Layer;
        const auto& Observer=Scene->PresentedCamera();const auto Size=G.GetLocalSize();
        if(Form->Edit.IsDirty())
        {
            FVector Corners[8];for(int32 I=0;I<8;++I)Corners[I]=FVector(I&1?Domain.Max.X:Domain.Min.X,I&2?Domain.Max.Y:Domain.Min.Y,I&4?Domain.Max.Z:Domain.Min.Z);
            for(int32 I=0;I<8;++I)for(int32 Axis=0;Axis<3;++Axis)if(!(I&(1<<Axis)))
            {FVector2D A,B;if(StudioCameraPlacement::ProjectLine(Observer,Size,Corners[I],Corners[I|(1<<Axis)],A,B,Aspect()))
                FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),TArray<FVector2D>{A,B},ESlateDrawEffect::None,Amber,true,1.4);}
        }
        for(int32 Face=0;Face<6;++Face)
        {
            FVector2D P;if(!StudioCameraPlacement::Project(Observer,Size,StudioDomain::FaceCenter(Domain,Face),P,Aspect())||
                P.X<10||P.Y<10||P.X>Size.X-28||P.Y>Size.Y-20)continue;
            const bool Active=Face==Scene->Model->SelectedDomainFace;
            const FLinearColor Color=FaceDrag.IsSet()&&FaceDrag->Face==Face?Text:Form->Edit.IsDirty()?Amber:Active?Cyan:Muted;
            FSlateDrawElement::MakeBox(Out,Layer+2,G.ToPaintGeometry(FVector2D(10,10),FSlateLayoutTransform(P-FVector2D(5,5))),&White,ESlateDrawEffect::None,Panel);
            const float Width=Face==HoverFace||Active?2.f:1.f;
            FSlateDrawElement::MakeLines(Out,Layer+3,G.ToPaintGeometry(),TArray<FVector2D>{P+FVector2D(-5,-5),P+FVector2D(5,-5),P+FVector2D(5,5),P+FVector2D(-5,5),P+FVector2D(-5,-5)},ESlateDrawEffect::None,Color,true,Width);
            FSlateDrawElement::MakeText(Out,Layer+3,G.ToPaintGeometry(FVector2D(24,14),FSlateLayoutTransform(P+FVector2D(8,-7))),
                FString::Printf(TEXT("%c%c"),Face%2?TEXT('+'):TEXT('-'),TEXT("XYZ")[Face/2]),Font(9,true),ESlateDrawEffect::None,Color);
        }
        return Layer+3;
    }
    virtual void Tick(const FGeometry& G,double T,float D) override
    {
        SFlowViewport::Tick(G,T,D);
        if(FaceDrag.IsSet()&&!Current(&G))
        {FaceDrag.Reset();Form->Notice=TEXT("The view or case changed during the drag. Current edits retained.");ReleaseOwnCapture();}
        if(PaintedRevision!=Form->EditRevision){PaintedRevision=Form->EditRevision;Invalidate(EInvalidateWidgetReason::Paint);}
    }
    virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& Event) override
    {
        FStudioDomain Domain;
        if(Event.GetEffectingButton()==EKeys::LeftMouseButton&&CanEdit(Domain)&&Scene->HasDomainPreview())
        {
            const auto Pixel=G.AbsoluteToLocal(Event.GetScreenSpacePosition());const int32 Face=Hit(G,Pixel,Domain);
            if(Face!=INDEX_NONE)
            {
                StudioDomain::FFaceDrag Start;
                if(!StudioDomain::BeginFaceDrag(Domain,Face,Scene->PresentedCamera(),G.GetLocalSize(),Pixel,Start,Aspect()))
                {Form->Notice=TEXT("This face is viewed along its normal. Orbit the view, or edit its coordinate in Bounds.");return FReply::Handled();}
                ReleaseOwnCapture();Scene->Model->SelectDomainFace(Face);OriginalDraft=Form->Edit;FaceDrag=Start;
                CaseRevision=Scene->Model->Project.Draft.Revision;OwnedRevision=Form->EditRevision;
                CapturedUser=Event.GetUserIndex();CapturedPointer=Event.GetPointerIndex();Drag=EKeys::LeftMouseButton;
                Form->Notice=TEXT("Drag to resize the amber draft outline over the applied domain. Release, then Apply domain. Escape cancels this drag.");
                return FReply::Handled().SetUserFocus(SharedThis(this)).CaptureMouse(SharedThis(this));
            }
        }
        return SFlowViewport::OnMouseButtonDown(G,Event);
    }
    virtual FReply OnMouseMove(const FGeometry& G,const FPointerEvent& Event) override
    {
        if(FaceDrag.IsSet())
        {
            if(!OwnsPointer(Event))return FReply::Unhandled();
            if(!Current(&G)){FaceDrag.Reset();FinishInput();return FReply::Handled().ReleaseMouseCapture();}
            FStudioDomain Domain;
            if(StudioDomain::DragFace(*FaceDrag,G.AbsoluteToLocal(Event.GetScreenSpacePosition()),Domain))
            {
                const int32 Face=FaceDrag->Face;
                Form->Edit.SetCoordinate(Face,StudioMaterials::ExactNumber(Face%2?Domain.Max[Face/2]:Domain.Min[Face/2]));
                Form->Synchronize();OwnedRevision=++Form->EditRevision;Invalidate(EInvalidateWidgetReason::Paint);
            }
            else Form->Notice=TEXT("The face must leave a positive domain dimension within ±1e8 m. Earlier valid draft position retained.");
            return FReply::Handled();
        }
        FStudioDomain Domain;const int32 NewHover=CanEdit(Domain)?Hit(G,G.AbsoluteToLocal(Event.GetScreenSpacePosition()),Domain):INDEX_NONE;
        if(HoverFace!=NewHover){HoverFace=NewHover;Invalidate(EInvalidateWidgetReason::Paint);}
        return SFlowViewport::OnMouseMove(G,Event);
    }
    virtual FReply OnMouseButtonUp(const FGeometry& G,const FPointerEvent& Event) override
    {
        if(FaceDrag.IsSet()&&OwnsPointer(Event)&&Event.GetEffectingButton()==EKeys::LeftMouseButton)
        {FaceDrag.Reset();Form->Notice=TEXT("Face drag ended. Apply domain to use the draft bounds, or Revert edits.");FinishInput();return FReply::Handled().ReleaseMouseCapture();}
        return SFlowViewport::OnMouseButtonUp(G,Event);
    }
    virtual FReply OnMouseWheel(const FGeometry& G,const FPointerEvent& Event) override
    {return FaceDrag.IsSet()?FReply::Handled():SFlowViewport::OnMouseWheel(G,Event);}
    virtual FReply OnKeyDown(const FGeometry& G,const FKeyEvent& Event) override
    {
        if(FaceDrag.IsSet()&&Event.GetKey()==EKeys::Escape){RestoreDrag();ReleaseOwnCapture();return FReply::Handled();}
        if(FaceDrag.IsSet())return FReply::Handled();
        return SFlowViewport::OnKeyDown(G,Event);
    }
    virtual void OnFocusLost(const FFocusEvent& Event) override
    {RestoreDrag();SFlowViewport::OnFocusLost(Event);}
    virtual void OnMouseCaptureLost(const FCaptureLostEvent& Event) override
    {if(Event.UserIndex==int32(CapturedUser)&&Event.PointerIndex==int32(CapturedPointer))RestoreDrag();SFlowViewport::OnMouseCaptureLost(Event);}
private:
    TSharedPtr<FStudioDomainWorkspaceState> Form;
    TOptional<StudioDomain::FFaceDrag> FaceDrag;
    FStudioDomainEdit OriginalDraft;
    int32 HoverFace=INDEX_NONE;
    int64 CaseRevision=0;
    uint64 OwnedRevision=0,PaintedRevision=MAX_uint64;
};

void SStudioWorkspace::RefreshDomain()
{
    if(!DomainState)return;
    auto& State=*DomainState;
    if(State.Project==M->Project.Id&&State.Revision==M->Project.Draft.Revision)return;
    if(State.Project!=M->Project.Id)
    {
        State.Edit=FStudioDomainEdit();State.Edit.Reset(M->Project.Draft.Domain);State.bConflict=false;
        State.Notice.Empty();State.Project=M->Project.Id;State.Synchronize();++State.EditRevision;
    }
    else if(!State.Edit.Matches(M->Project.Draft.Domain))
    {
        if(State.Edit.IsDirty())
        {State.bConflict=true;State.Notice=TEXT("The applied domain changed while you were editing. Your text is retained. Revert to load the current domain.");}
        else {State.Edit.Reset(M->Project.Draft.Domain);State.bConflict=false;State.Synchronize();++State.EditRevision;}
    }
    State.Revision=M->Project.Draft.Revision;
}
void SStudioWorkspace::ApplyDomain()
{
    RefreshDomain();auto& State=*DomainState;
    if(State.bConflict)return;
    FStudioDomain Domain;
    if(!State.Edit.Build(Domain)){State.Notice=State.Edit.Error;State.FocusError();return;}
    if(!M->UpdateDomain(Domain)){State.Notice=M->Notice;return;}
    State.Edit.Reset(M->Project.Draft.Domain);State.Synchronize();++State.EditRevision;State.Revision=M->Project.Draft.Revision;
    State.Notice=TEXT("Domain applied to the case. Save to keep these changes.");
    ResolveSaveNotice(*M,DomainSaveGuard,State.Notice);
}
bool SStudioWorkspace::EnsureDomainResolved()
{
    RefreshDomain();
    if(DomainState&&(DomainState->Edit.IsDirty()||DomainState->bConflict))
    {
        DomainState->Notice=DomainSaveGuard;
        M->Notice=DomainState->Notice;Navigate(EStudioWorkspace::Domain);
        if(auto Input=DomainState->Inputs[0].Pin())FSlateApplication::Get().SetKeyboardFocus(Input,EFocusCause::SetDirectly);
        return false;
    }
    return true;
}

// THESIS: Author a physical case domain against verified imported geometry.
// OWN-WORLD: Incumbent blue-black Slate scene, compact meter fields and cyan face selection.
// STORY: Check source geometry, edit bounds or fit with padding, name faces, apply and save.
// FIRST VIEWPORT: Applied 3D domain left, one retained domain inspector right.
// FORM: Local Operate extension reached only through the existing sidebar.
// FINISH: Two-size native controls/preview evidence and independent finish handoffs.
TSharedRef<SWidget> SStudioWorkspace::DomainWorkspace()
{
    DomainState=MakeShared<FStudioDomainWorkspaceState>();RefreshDomain();
    const auto State=DomainState;
    auto Available=[this]{return !M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();};
    auto Copy=[](TFunction<FString()> Read,FLinearColor Color=Muted)
    {return SNew(STextBlock).Text_Lambda([Read]{return FText::FromString(Read());}).Font(Font(10)).ColorAndOpacity(Color).AutoWrapText(true);};
    auto Layers=[this,State]() -> FString
    {
        if(!Scene.IsValid()||!Scene->HasDomainPreview())return TEXT("Domain preview unavailable");
        if(State->bConflict)return TEXT("Applied domain · Conflicting draft hidden");
        FStudioDomain Draft;
        if(State->Edit.IsDirty()&&!State->CanPreviewDraft(Scene.Get(),Draft))return TEXT("Applied domain · Invalid draft hidden");
        return State->Edit.IsDirty()?TEXT("Applied domain + amber draft outline"):TEXT("Applied domain");
    };
    auto Input=[this,State](int32 Index,const FString& Name)
    {
        auto Field=SNew(SEditableTextBox).Tag(FName(*FString::Printf(TEXT("DomainValue%d"),Index)))
            .Style(&InputStyle()).Font(Font(10)).Text(FText::FromString(State->Value(Index)))
            .ToolTipText_Lambda([State,Index,Name]{return FText::FromString(Name+TEXT(": ")+State->Value(Index));})
            .OnTextChanged_Lambda([State,Index](const FText& Value)
            {State->Change(Index,Value.ToString());})
            .OnTextCommitted_Lambda([this,Index](const FText&,ETextCommit::Type Type){if(Type==ETextCommit::OnEnter&&(Index<12||Index>=18))ApplyDomain();});
        State->Inputs[Index]=Field;return Field;
    };
    auto Fields=SNew(SVerticalBox);
    Fields->AddSlot().AutoHeight().Padding(0,0,0,7)[Label(TEXT("Bounds · meters"),12,Text,true)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,5)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(23)]
        +SHorizontalBox::Slot().FillWidth(1)[Label(TEXT("Minimum"),9,Muted)]
        +SHorizontalBox::Slot().FillWidth(1)[Label(TEXT("Maximum"),9,Muted)]];
    for(int32 Axis=0;Axis<3;++Axis)
        Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(23)[Label(FString::Chr(TEXT("XYZ")[Axis]),10,Text,true)]]
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,5,0)[Input(Axis*2,FString::Printf(TEXT("%c minimum (m)"),TEXT("XYZ")[Axis]))]
            +SHorizontalBox::Slot().FillWidth(1)[Input(Axis*2+1,FString::Printf(TEXT("%c maximum (m)"),TEXT("XYZ")[Axis]))]];
    Fields->AddSlot().AutoHeight().Padding(0,8,0,6)[Label(TEXT("Dimensions · meters"),11,Text,true)];
    auto Dimensions=SNew(SHorizontalBox);
    for(int32 Axis=0;Axis<3;++Axis)Dimensions->AddSlot().FillWidth(1).Padding(Axis?5:0,0,0,0)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,4)[Label(FString::Chr(TEXT("XYZ")[Axis]),9,Muted)]
        +SVerticalBox::Slot().AutoHeight()[Input(18+Axis,FString::Printf(TEXT("%c dimension (m), anchored at minimum"),TEXT("XYZ")[Axis]))]];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,5)[Dimensions];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("Changing a dimension keeps its minimum fixed."),9,Muted)];
    auto Apply=Button(TEXT("Apply domain"),TEXT("check"),[this]{ApplyDomain();},Cyan);Apply->SetTag(TEXT("DomainApply"));
    Apply->SetEnabled(TAttribute<bool>::CreateLambda([State,Available]{return Available()&&!State->bConflict&&State->Edit.IsDirty();}));
    auto Revert=Button(TEXT("Revert edits"),TEXT("undo"),[this,State]
    {State->Edit.Reset(M->Project.Draft.Domain);State->bConflict=false;State->Notice=TEXT("Loaded the applied case domain.");State->Revision=M->Project.Draft.Revision;State->Synchronize();++State->EditRevision;
        ResolveSaveNotice(*M,DomainSaveGuard,State->Notice);});
    Revert->SetTag(TEXT("DomainRevert"));Revert->SetEnabled(TAttribute<bool>::CreateLambda([State,Available]{return Available()&&(State->bConflict||State->Edit.IsDirty()||!State->Edit.Error.IsEmpty());}));
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Apply]+SHorizontalBox::Slot().AutoWidth()[Revert]];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[Copy(Layers)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[Copy([State]
    {return State->Notice.IsEmpty()?(State->Edit.IsDirty()?TEXT("Edits are unapplied. Apply domain to use them, or Revert edits."):TEXT("Edits are applied to the case.")):State->Notice;},Amber)];
    Fields->AddSlot().AutoHeight().Padding(0,14,0,7)[Label(TEXT("Faces"),12,Text,true)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Select a face to highlight it in the preview."),9,Muted)];
    for(int32 Face=0;Face<6;++Face)
    {
        const FString Axis=FString::Printf(TEXT("%c%c"),Face%2?TEXT('+'):TEXT('-'),TEXT("XYZ")[Face/2]);
        auto Select=SNew(SButton).Tag(FName(*FString::Printf(TEXT("DomainFace%d"),Face))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
            .ToolTipText_Lambda([this,Face]{return FText::FromString(TEXT("Highlight ")+M->Project.Draft.Domain.FaceNames[Face]);})
            .OnClicked_Lambda([this,Face]{M->SelectDomainFace(Face);return FReply::Handled();})
            [SNew(STextBlock).Text(FText::FromString(Axis)).Font(Font(10)).ColorAndOpacity_Lambda([this,Face]{return M->SelectedDomainFace==Face?Cyan:Muted;})];
        Fields->AddSlot().AutoHeight().Padding(0,0,0,5)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,7,0)[SNew(SBox).WidthOverride(42)[Select]]
            +SHorizontalBox::Slot().FillWidth(1)[Input(6+Face,Axis+TEXT(" face name"))]];
    }
    Fields->AddSlot().AutoHeight().Padding(0,16,0,7)[Label(TEXT("Fit to geometry"),12,Text,true)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[Copy([]{return FString(TEXT("Padding for the next fit, in meters. Planar geometry needs padding to enclose a 3D volume."));})];
    for(int32 Axis=0;Axis<3;++Axis)
        Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(23)[Label(FString::Chr(TEXT("XYZ")[Axis]),10,Text,true)]]
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,5,0)[Input(12+Axis*2,FString::Printf(TEXT("-%c padding (m)"),TEXT("XYZ")[Axis]))]
            +SHorizontalBox::Slot().FillWidth(1)[Input(13+Axis*2,FString::Printf(TEXT("+%c padding (m)"),TEXT("XYZ")[Axis]))]];
    auto Fit=Button(TEXT("Fit bounds with padding"),TEXT("fit"),[this,State]
    {RefreshDomain();if(State->bConflict||!M->DomainGeometry||!M->DomainGeometry->Complete())return;
        if(State->Edit.Fit(M->DomainGeometry->Bounds)){State->Synchronize();++State->EditRevision;State->Notice=TEXT("Draft bounds fitted to geometry. Apply domain to use them in the case.");}
        else {State->Notice=State->Edit.Error;State->FocusError();}});
    Fit->SetTag(TEXT("DomainFitBounds"));Fit->SetEnabled(TAttribute<bool>::CreateLambda([this,State,Available]
    {return Available()&&!State->bConflict&&!M->IsReadingDomainGeometry()&&M->DomainGeometry&&M->DomainGeometry->Complete();}));
    Fields->AddSlot().AutoHeight().Padding(0,4,0,6)[Fit];
    Fields->AddSlot().AutoHeight()[Label(TEXT("Left column: negative face · Right: positive face"),9,Muted)];
    auto Check=Button(TEXT("Check geometry"),TEXT("check"),[this]{M->RequestDomainGeometry();});Check->SetTag(TEXT("DomainCheckGeometry"));
    Check->SetEnabled(TAttribute<bool>::CreateLambda([this,Available]{return Available()&&!M->IsReadingDomainGeometry();}));
    auto Cancel=Button(TEXT("Cancel check"),TEXT("stop"),[this]{M->CancelDomainGeometry();});Cancel->SetTag(TEXT("DomainCancelCheck"));
    Cancel->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->IsReadingDomainGeometry();}));
    auto Undo=Button(TEXT("Undo case"),TEXT("undo"),[this]{M->UndoCase();RefreshDomain();});Undo->SetTag(TEXT("DomainUndo"));
    Undo->SetEnabled(TAttribute<bool>::CreateLambda([this,State,Available]{return Available()&&!State->Edit.IsDirty()&&!State->bConflict&&M->CanUndoCase();}));
    auto Redo=Button(TEXT("Redo case"),TEXT("redo"),[this]{M->RedoCase();RefreshDomain();});Redo->SetTag(TEXT("DomainRedo"));
    Redo->SetEnabled(TAttribute<bool>::CreateLambda([this,State,Available]{return Available()&&!State->Edit.IsDirty()&&!State->bConflict&&M->CanRedoCase();}));
    auto Inspector=SNew(SRetainedFormScrollBox).Tag(TEXT("DomainInspector")).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12)
        +SScrollBox::Slot().Padding(14)[SNew(SBox).IsEnabled_Lambda(Available)[Fields]];
    auto FitView=Button(TEXT("Fit view"),TEXT("fit"),[this]{if(Scene.IsValid())Scene->FitCamera();});FitView->SetTag(TEXT("DomainFitView"));
    return SNew(SVerticalBox).Tag(TEXT("DomainWorkspace"))
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SBorder).BorderImage(&PanelBrush).Padding(14,10)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Domain"),18,Text,true)]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Undo]+SHorizontalBox::Slot().AutoWidth()[Redo]]]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[SNew(SVerticalBox)
                +SVerticalBox::Slot().FillHeight(1)[SNew(SBorder).BorderImage(&LineBrush).Padding(1)[SNew(SOverlay)
                    +SOverlay::Slot()[SNew(SDomainViewport).Tag(TEXT("DomainViewport")).Scene(Scene.Get()).Form(State)]
                    +SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(12)[SNew(SBorder).BorderImage(&PanelBrush).Padding(8,5)
                        [Live(Layers,10,Muted)]]
                    +SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(12)[SNew(SOrientationAxes).Scene(Scene.Get()).Visibility(EVisibility::HitTestInvisible)]
                    +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(12)[SNew(SVerticalBox)
                        +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0,0,0,6)[FitView]
                        +SVerticalBox::Slot().AutoHeight()[SNew(SBorder).BorderImage(&PanelBrush).Padding(8,5)
                            [Label(TEXT("Square handles: resize · Drag: orbit · Middle-drag: pan · Scroll: zoom"),9,Muted)]]]]]
                +SVerticalBox::Slot().AutoHeight().Padding(0,6,0,0)[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)[SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SHorizontalBox)
                        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Geometry containment"),11,Text,true)]
                        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,5,0)[Check]+SHorizontalBox::Slot().AutoWidth()[Cancel]]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Copy([this]{return M->DomainNotice;})]
                    +SVerticalBox::Slot().AutoHeight()[SNew(SBox).MaxDesiredHeight(128)[SNew(SScrollBox)+SScrollBox::Slot()[SNew(STextBlock).Font(Font(10)).AutoWrapText(true)
                    .ColorAndOpacity_Lambda([this]
                    {
                        if(!M->DomainGeometry||!M->DomainGeometry->Complete())return Amber;
                        for(const auto& Object:M->DomainGeometry->Objects)if(!StudioDomain::Contains(M->Project.Draft.Domain,Object.Bounds))return Amber;
                        return Muted;
                    }).Text_Lambda([this]
                    {
                        if(!M->DomainGeometry)return FText::FromString(TEXT("Applied-domain containment unknown. Check geometry to verify the source files."));
                        TArray<FString> Lines;bool Outside=false,Unknown=false;
                        for(const auto& Object:M->DomainGeometry->Objects)
                        {
                            if(!Object.Error.IsEmpty()){Unknown=true;Lines.Add(Object.Name+TEXT(" · Containment unknown: ")+Object.Error);}
                            else if(StudioDomain::Contains(M->Project.Draft.Domain,Object.Bounds))Lines.Add(Object.Name+TEXT(" · Inside the applied domain"));
                            else {Outside=true;Lines.Add(Object.Name+TEXT(" · Outside the applied domain"));}
                        }
                        if(Outside)Lines.Add(TEXT("Enlarge Bounds or use Fit to geometry, then Apply domain."));
                        if(Unknown)Lines.Add(TEXT("Resolve the source files in Geometry, then Check geometry again."));
                        if(M->DomainGeometry->bPreviewLimited)Lines.Add(TEXT("Preview limit reached. Containment uses verified original geometry bounds."));
                        return FText::FromString(FString::Join(Lines,TEXT("\n")));
                    })]]]]]]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(334)[SNew(SBorder).BorderImage(&PanelBrush).Padding(0)[Inspector]]]];
}

struct FStudioLatticeWorkspaceState
{
    FGuid Project;int64 Revision=-1;FStudioLatticeEdit Edit;
    FStudioLatticePreviewSettings Preview;bool bConflict=false;FString Notice;
    TWeakPtr<SEditableTextBox> Inputs[4];
    void Synchronize()
    {for(int32 I=0;I<4;++I)if(const auto Input=Inputs[I].Pin())Input->SetText(FText::FromString(I<3?Edit.Counts[I]:Edit.RequestedSpacing));}
    void FocusError()
    {if(Edit.ErrorField>=0&&Edit.ErrorField<4)if(const auto Input=Inputs[Edit.ErrorField].Pin())FSlateApplication::Get().SetKeyboardFocus(Input,EFocusCause::SetDirectly);}
};
void SStudioWorkspace::RefreshLattice()
{
    if(!LatticeState)return;auto& State=*LatticeState;
    if(State.Project==M->Project.Id&&State.Revision==M->Project.Draft.Revision)return;
    const auto Resolution=M->Project.Draft.Setup.LatticeResolution;
    if(State.Project!=M->Project.Id)
    {
        State.Project=M->Project.Id;State.Edit.Reset(Resolution);State.bConflict=false;State.Notice.Empty();
        State.Preview={};State.Preview.Layer=Resolution.Z/2;State.Preview.MaximumSamples=8192;State.Synchronize();
    }
    else if(State.Edit.Saved!=Resolution)
    {
        if(State.Edit.IsDirty())
        {State.bConflict=true;State.Notice=TEXT("The applied resolution changed. Your text is retained. Revert to load the current counts.");}
        else {State.Edit.Reset(Resolution);State.bConflict=false;State.Synchronize();}
    }
    if(State.Preview.Axis>=0)State.Preview.Layer=FMath::Clamp(State.Preview.Layer,0,Resolution[State.Preview.Axis]-1);
    State.Revision=M->Project.Draft.Revision;
}
void SStudioWorkspace::ApplyLattice()
{
    RefreshLattice();auto& State=*LatticeState;if(State.bConflict)return;
    FIntVector Resolution;if(!State.Edit.Build(Resolution)){State.Notice=State.Edit.Error;State.FocusError();return;}
    if(!M->UpdateLatticeResolution(Resolution)){State.Notice=M->Notice;return;}
    State.Edit.Reset(Resolution);State.Synchronize();State.Revision=-1;RefreshLattice();
    State.Notice=TEXT("Resolution applied. Preview cells to inspect it, and save to keep the case.");
    ResolveSaveNotice(*M,LatticeSaveGuard,State.Notice);
}
bool SStudioWorkspace::EnsureLatticeResolved()
{
    RefreshLattice();if(!LatticeState||(!LatticeState->bConflict&&!LatticeState->Edit.IsDirty()))return true;
    LatticeState->Notice=LatticeSaveGuard;M->Notice=LatticeState->Notice;
    Navigate(EStudioWorkspace::Meshing);
    if(const auto Input=LatticeState->Inputs[0].Pin())FSlateApplication::Get().SetKeyboardFocus(Input,EFocusCause::SetDirectly);
    return false;
}

// THESIS: Inspect bounded samples of original physical cells before backend preparation.
// OWN-WORLD: Existing blue-black authoring scene, retained meter/count fields, cyan controls.
// STORY: Apply counts, choose a layer, preview occupancy, inspect from any camera perspective.
// FIRST VIEWPORT: Flexible 3D scene with one 334-unit lattice inspector.
// FORM: Meshing sidebar workspace; no second workspace navigation.
// FINISH: Native two-size controls/cancellation/camera evidence and finish review required.
TSharedRef<SWidget> SStudioWorkspace::LatticeWorkspace()
{
    LatticeState=MakeShared<FStudioLatticeWorkspaceState>();RefreshLattice();const auto State=LatticeState;
    auto Available=[this]{return !M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();};
    auto Copy=[](TFunction<FString()> Read,FLinearColor Color=Muted)
    {return SNew(STextBlock).Text_Lambda([Read]{return FText::FromString(Read());}).Font(Font(10)).ColorAndOpacity(Color).AutoWrapText(true);};
    auto Input=[this,State](int32 Index,const FString& Name)
    {
        auto Field=SNew(SEditableTextBox).Tag(FName(*FString::Printf(TEXT("LatticeValue%d"),Index))).Style(&InputStyle()).Font(Font(10))
            .Text(FText::FromString(Index<3?State->Edit.Counts[Index]:State->Edit.RequestedSpacing)).ToolTipText(FText::FromString(Name))
            .OnTextChanged_Lambda([State,Index](const FText& Value)
            {if(Index<3)State->Edit.Counts[Index]=Value.ToString();else State->Edit.RequestedSpacing=Value.ToString();State->Notice.Empty();State->Edit.Error.Empty();})
            .OnTextCommitted_Lambda([this,Index](const FText&,ETextCommit::Type Type){if(Index<3&&Type==ETextCommit::OnEnter)ApplyLattice();});
        State->Inputs[Index]=Field;return Field;
    };
    auto Fields=SNew(SVerticalBox);
    Fields->AddSlot().AutoHeight().Padding(0,0,0,7)[Label(TEXT("Resolution · cells"),12,Text,true)];
    auto Counts=SNew(SHorizontalBox);
    for(int32 Axis=0;Axis<3;++Axis)Counts->AddSlot().FillWidth(1).Padding(Axis?5:0,0,0,0)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,4)[Label(FString::Chr(TEXT("XYZ")[Axis]),9,Muted)]
        +SVerticalBox::Slot().AutoHeight()[Input(Axis,FString::Printf(TEXT("%c cell count"),TEXT("XYZ")[Axis]))]];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[Counts];
    auto Apply=Button(TEXT("Apply counts"),TEXT("check"),[this]{ApplyLattice();},Cyan);Apply->SetTag(TEXT("LatticeApply"));
    Apply->SetEnabled(TAttribute<bool>::CreateLambda([State,Available]{return Available()&&!State->bConflict&&State->Edit.IsDirty();}));
    auto Revert=Button(TEXT("Revert edits"),TEXT("undo"),[this,State]
    {State->Edit.Reset(M->Project.Draft.Setup.LatticeResolution);State->bConflict=false;State->Synchronize();State->Revision=-1;RefreshLattice();State->Notice=TEXT("Loaded the applied cell counts.");
        ResolveSaveNotice(*M,LatticeSaveGuard,State->Notice);});
    Revert->SetTag(TEXT("LatticeRevert"));Revert->SetEnabled(TAttribute<bool>::CreateLambda([State,Available]{return Available()&&(State->bConflict||State->Edit.IsDirty()||!State->Edit.Error.IsEmpty());}));
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Apply]+SHorizontalBox::Slot().AutoWidth()[Revert]];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[Copy([State]
    {return !State->Notice.IsEmpty()?State->Notice:State->Edit.IsDirty()?FString(TEXT("Unapplied counts. The scene uses the applied case.")):FString(TEXT("Counts divide the applied domain into physical cells."));},Amber)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Maximum spacing · meters"),11,Text,true)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[Input(3,TEXT("Requested maximum cell spacing in meters"))];
    auto Derive=Button(TEXT("Calculate draft counts"),TEXT("mesh"),[this,State]
    {
        RefreshLattice();if(State->bConflict)return;
        if(State->Edit.UseSpacing(M->Project.Draft.Domain)){State->Synchronize();State->Notice=TEXT("Draft counts calculated. Apply counts to change the case.");}
        else {State->Notice=State->Edit.Error;State->FocusError();}
    });Derive->SetTag(TEXT("LatticeUseSpacing"));Derive->SetEnabled(TAttribute<bool>::CreateLambda([State,Available]{return Available()&&!State->bConflict;}));
    Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[Derive];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,14)[Copy([this]
    {
        FStudioLatticeLayout Layout;FString Error;
        if(!StudioLattice::Layout(M->Project.Draft.Domain,M->Project.Draft.Setup.LatticeResolution,Layout,Error))return Error;
        return FString::Printf(TEXT("Applied: %llu cells\nSpacing X %.6g · Y %.6g · Z %.6g m"),static_cast<unsigned long long>(Layout.Cells),Layout.Spacing.X,Layout.Spacing.Y,Layout.Spacing.Z);
    })];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,7)[Label(TEXT("Preview region"),12,Text,true)];
    auto Axes=SNew(SHorizontalBox);
    for(int32 Choice=0;Choice<4;++Choice)
    {
        const int32 Axis=Choice<3?Choice:-1;
        Axes->AddSlot().FillWidth(Choice<3?1:1.8).Padding(Choice?4:0,0,0,0)[SNew(SButton)
            .Tag(FName(*FString::Printf(TEXT("LatticeAxis%d"),Choice))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,6))
            .OnClicked_Lambda([this,State,Axis]
            {State->Preview.Axis=Axis;if(Axis>=0)State->Preview.Layer=M->Project.Draft.Setup.LatticeResolution[Axis]/2;else State->Preview.MaximumSamples=FMath::Min(State->Preview.MaximumSamples,4096);return FReply::Handled();})
            [SNew(STextBlock).Text(FText::FromString(Choice<3?FString::Chr(TEXT("XYZ")[Choice]):FString(TEXT("Whole")))).Font(Font(10))
                .ColorAndOpacity_Lambda([State,Axis]{return State->Preview.Axis==Axis?Cyan:Muted;})]];
    }
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[Axes];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SVerticalBox).Visibility_Lambda([State]{return State->Preview.Axis>=0?EVisibility::Visible:EVisibility::Collapsed;})
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,4)[Copy([this,State]
        {return State->Preview.Axis>=0?FString::Printf(TEXT("Layer index · 0 to %d"),M->Project.Draft.Setup.LatticeResolution[State->Preview.Axis]-1):FString();})]
        +SVerticalBox::Slot().AutoHeight()[SNew(SNumericEntryBox<int32>).Tag(TEXT("LatticeLayer")).Font(Font(10)).AllowSpin(true).MinValue(0).MinSliderValue(0)
            .MaxValue_Lambda([this,State]{return TOptional<int32>(M->Project.Draft.Setup.LatticeResolution[FMath::Max(0,State->Preview.Axis)]-1);})
            .MaxSliderValue_Lambda([this,State]{return TOptional<int32>(M->Project.Draft.Setup.LatticeResolution[FMath::Max(0,State->Preview.Axis)]-1);})
            .Value_Lambda([State]{return TOptional<int32>(State->Preview.Layer);})
            .OnValueChanged_Lambda([this,State](int32 Value){State->Preview.Layer=FMath::Clamp(Value,0,M->Project.Draft.Setup.LatticeResolution[FMath::Max(0,State->Preview.Axis)]-1);})]];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,4)[Label(TEXT("Maximum displayed cells"),10,Muted)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SStudioMenuButton).Tag(TEXT("LatticeBudget")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
        .OnGetMenuContent_Lambda([State]
        {
            auto Items=SNew(SVerticalBox);
            for(int32 Limit:{512,2048,4096,8192,32768})
                if(State->Preview.Axis>=0||Limit<=4096)
                    Items->AddSlot().AutoHeight()[Button(FString::FromInt(Limit),TEXT("select"),[State,Limit]{State->Preview.MaximumSamples=Limit;FSlateApplication::Get().DismissAllMenus();})];
            return Items;
        })
        .ButtonContent()[Live([State]{return FString::FromInt(State->Preview.MaximumSamples);},10)]];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,10)[Copy([State]
    {return State->Preview.Axis<0?FString(TEXT("Whole-domain view samples up to 4,096 original cells. Choose Preview cells to update the view.")):FString(TEXT("Layer view samples up to 32,768 original cells. Tiles pass through their centers. Choose Preview cells to update the view."));})];
    auto Preview=Button(TEXT("Preview cells"),TEXT("mesh"),[this,State]{RefreshLattice();if(!State->bConflict&&!State->Edit.IsDirty())M->RequestLatticePreview(State->Preview);},Cyan);
    Preview->SetTag(TEXT("LatticePreview"));Preview->SetEnabled(TAttribute<bool>::CreateLambda([this,State,Available]
    {return Available()&&!State->bConflict&&!State->Edit.IsDirty()&&!M->IsBuildingLatticePreview()&&!M->IsReadingDomainGeometry();}));
    auto Cancel=SNew(SButton).Tag(TEXT("LatticeCancel")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,6))
        .IsEnabled_Lambda([this]{return M->IsBuildingLatticePreview()||M->LatticePreview.IsValid();})
        .OnClicked_Lambda([this]{M->CancelLatticePreview();return FReply::Handled();})
        [Live([this]{return M->IsBuildingLatticePreview()?TEXT("Cancel"):TEXT("Clear preview");},10)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Preview]+SHorizontalBox::Slot().AutoWidth()[Cancel]];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[Copy([this]
    {
        if(M->LatticeProgress)
        {const auto& P=*M->LatticeProgress;return P.Stage.load()<2?FString(TEXT("Indexing original geometry…")):FString::Printf(TEXT("Classified %d of %d displayed cells"),P.Completed.load(),P.Total.load());}
        return M->LatticeNotice.IsEmpty()?FString(TEXT("Choose a layer and preview the applied lattice.")):M->LatticeNotice;
    })];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,14)[Copy([this]
    {
        if(!M->LatticePreview)return FString();const auto& P=*M->LatticePreview;
        return FString::Printf(TEXT("%d displayed / %llu candidate cells%s\nOutside geometry: %d\nInside closed geometry: %d\nSurface overlap: %d\nUnknown: %d"),P.Samples.Num(),static_cast<unsigned long long>(P.Plan.CandidateCells),P.Plan.bSampled?TEXT(" · sampled"):TEXT(" · complete region"),P.Outside,P.Inside,P.Surface,P.Unknown);
    })];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Backend preparation"),12,Text,true)];
    Fields->AddSlot().AutoHeight()[Copy([]
    {return FString(TEXT("Backend lattice rules, memory estimate, refinement and production cell flags are not supplied. This preview checks geometric occupancy only."));})];
    auto Undo=Button(TEXT("Undo case"),TEXT("undo"),[this]{M->UndoCase();RefreshLattice();});Undo->SetTag(TEXT("LatticeUndo"));
    Undo->SetEnabled(TAttribute<bool>::CreateLambda([this,State,Available]{return Available()&&!State->Edit.IsDirty()&&!State->bConflict&&M->CanUndoCase();}));
    auto Redo=Button(TEXT("Redo case"),TEXT("redo"),[this]{M->RedoCase();RefreshLattice();});Redo->SetTag(TEXT("LatticeRedo"));
    Redo->SetEnabled(TAttribute<bool>::CreateLambda([this,State,Available]{return Available()&&!State->Edit.IsDirty()&&!State->bConflict&&M->CanRedoCase();}));
    auto Check=Button(TEXT("Check geometry"),TEXT("check"),[this]{M->RequestDomainGeometry();});Check->SetTag(TEXT("LatticeCheckGeometry"));
    Check->SetEnabled(TAttribute<bool>::CreateLambda([this,Available]{return Available()&&!M->IsReadingDomainGeometry();}));
    auto CancelCheck=Button(TEXT("Cancel check"),TEXT("stop"),[this]{M->CancelDomainGeometry();});CancelCheck->SetTag(TEXT("LatticeCancelCheck"));
    CancelCheck->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->IsReadingDomainGeometry();}));
    auto Fit=Button(TEXT("Fit view"),TEXT("fit"),[this]{if(Scene.IsValid()&&Scene->HasLatticePreview())Scene->FitCamera();});Fit->SetTag(TEXT("LatticeFitView"));
    Fit->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Scene.IsValid()&&Scene->HasLatticePreview();}));
    auto Projection=SNew(SButton).Tag(TEXT("LatticeProjection")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,6))
        .IsEnabled_Lambda([this]{return Scene.IsValid()&&Scene->HasLatticePreview();})
        .ToolTipText_Lambda([this]{return FText::FromString(Scene.IsValid()&&Scene->CameraState().bOrthographic?TEXT("Switch the authoring camera to perspective"):TEXT("Switch the authoring camera to orthographic"));})
        .OnClicked_Lambda([this]{if(Scene.IsValid()){auto Camera=Scene->CameraState();Camera.bOrthographic=!Camera.bOrthographic;Scene->RestoreCamera(Camera,TEXT("Authoring projection"));}return FReply::Handled();})
        [Live([this]{return Scene.IsValid()&&Scene->CameraState().bOrthographic?TEXT("Orthographic"):TEXT("Perspective");},10)];
    return SNew(SVerticalBox).Tag(TEXT("LatticeWorkspace"))
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SBorder).BorderImage(&PanelBrush).Padding(14,10)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Meshing"),18,Text,true)]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Undo]+SHorizontalBox::Slot().AutoWidth()[Redo]]]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[SNew(SVerticalBox)
                +SVerticalBox::Slot().FillHeight(1)[SNew(SBorder).BorderImage(&LineBrush).Padding(1)[SNew(SOverlay)
                    +SOverlay::Slot()[SNew(SFlowViewport).Tag(TEXT("LatticeViewport")).Scene(Scene.Get())]
                    +SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(12)[SNew(SBorder).BorderImage(&PanelBrush).Padding(8,5)[Live([this]
                    {
                        if(!M->LatticePreview)return FString(TEXT("Applied domain · Preview cells to inspect occupancy"));const auto& P=M->LatticePreview->Plan;
                        return P.Settings.Axis<0?FString(TEXT("Applied lattice · Whole domain")):FString::Printf(TEXT("Applied lattice · %c layer %d"),TEXT("XYZ")[P.Settings.Axis],P.Settings.Layer);
                    },10,Muted)]]
                    +SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(12)[SNew(SOrientationAxes).Scene(Scene.Get()).Visibility(EVisibility::HitTestInvisible)]
                    +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(12)[SNew(SBorder).BorderImage(&PanelBrush).Padding(8)[SNew(SHorizontalBox)
                        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Fit]+SHorizontalBox::Slot().AutoWidth()[Projection]]]]]
                +SVerticalBox::Slot().AutoHeight().Padding(0,6,0,0)[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)[SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SWrapBox).UseAllottedSize(true)
                        +SWrapBox::Slot().Padding(0,0,12,4)[Label(TEXT("● Outside geometry"),9,FLinearColor(.08,.48,.62))]
                        +SWrapBox::Slot().Padding(0,0,12,4)[Label(TEXT("● Inside closed geometry"),9,FLinearColor(.48,.38,.7))]
                        +SWrapBox::Slot().Padding(0,0,12,4)[Label(TEXT("● Surface"),9,Amber)]
                        +SWrapBox::Slot().Padding(0,0,0,4)[Label(TEXT("● Unknown"),9,FLinearColor(.7,.28,.34))]]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,7)[Copy([]{return FString(TEXT("3% display gaps · Drag: orbit · Right-drag: look · Middle-drag: pan · Scroll: zoom"));})]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SHorizontalBox)
                        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Check]+SHorizontalBox::Slot().AutoWidth()[CancelCheck]]
                    +SVerticalBox::Slot().AutoHeight()[Copy([this]{return M->DomainNotice;})]]]]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(334)[SNew(SBorder).BorderImage(&PanelBrush).Padding(0)
                [SNew(SRetainedFormScrollBox).Tag(TEXT("LatticeInspector")).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12)
                    +SScrollBox::Slot().Padding(14)[SNew(SBox).IsEnabled_Lambda(Available)[Fields]]]]]];
}

TSharedRef<SWidget> SStudioWorkspace::GeometryWorkspace()
{
    GeometryState=MakeShared<FStudioGeometryWorkspaceState>();
    auto Copy=[&](TFunction<FString()> Read,FLinearColor Color=Muted)
    {return SNew(STextBlock).Text_Lambda([Read]{return FText::FromString(Read());}).Font(Font(10)).ColorAndOpacity(Color).AutoWrapText(true);};
    auto Picker=[&](TFunction<FString()> Read,TArray<FString> Names,TFunction<void(int32)> Select)
    {
        return SNew(SStudioMenuButton).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
            .OnGetMenuContent_Lambda([Names,Select]
            {auto Items=SNew(SVerticalBox);for(int32 I=0;I<Names.Num();++I)Items->AddSlot().AutoHeight()[Button(Names[I],TEXT("select"),[I,Select]{FSlateApplication::Get().DismissAllMenus();Select(I);})];return Items;})
            .ButtonContent()[Live(Read,10)];
    };
    auto Undo=Button(TEXT("Undo case"),TEXT("undo"),[this]{M->UndoCase();});Undo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanUndoCase();}));
    auto Redo=Button(TEXT("Redo case"),TEXT("redo"),[this]{M->RedoCase();});Redo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanRedoCase();}));
    Undo->SetTag(TEXT("GeometryUndo"));Redo->SetTag(TEXT("GeometryRedo"));
    auto Commit=Button(TEXT("Import into case"),TEXT("plus"),[this]{M->CommitGeometryImport();},Cyan);
    Commit->SetEnabled(TAttribute<bool>::CreateLambda([this]{FStudioGeometryAsset A;FString E;return M->bImportPreview&&!M->IsReadingGeometry()&&M->GeometryAssetForPreview(A,E);}));
    auto Cancel=Button(TEXT("Cancel preview"),TEXT("stop"),[this]{M->CancelGeometryImport();});
    Cancel->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->bImportPreview||M->IsReadingGeometry();}));
    Cancel->SetVisibility(TAttribute<EVisibility>::CreateLambda([this]{return M->bImportPreview||M->IsReadingGeometry()?EVisibility::Visible:EVisibility::Collapsed;}));
    const auto AxisNames=TArray<FString>{TEXT("X"),TEXT("Y"),TEXT("Z")};
    auto Options=SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("Import settings"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Object name"),10,Muted)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SNew(SEditableTextBox).Style(&InputStyle()).Font(Font(10))
            .Text_Lambda([this]{return FText::FromString(M->ImportOptions.Name);})
            .OnTextChanged_Lambda([this](const FText& T){M->ImportOptions.Name=T.ToString();})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Source units"),Picker([this]
            {const double U=M->ImportOptions.MetersPerUnit;return U==1?TEXT("Meters"):U==.01?TEXT("Centimeters"):U==.001?TEXT("Millimeters"):U==.0254?TEXT("Inches"):TEXT("Choose units…");},
            {TEXT("Meters"),TEXT("Centimeters"),TEXT("Millimeters"),TEXT("Inches")},[this](int32 I){const double Units[]={1.,.01,.001,.0254};M->ImportOptions.MetersPerUnit=Units[I];M->GeometryOptionsChanged();}),150)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Source up"),Picker([this]{return FString::Chr(TEXT("XYZ")[M->ImportOptions.UpAxis]);},AxisNames,[this](int32 I){M->ImportOptions.UpAxis=I;M->GeometryOptionsChanged();}),150)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Source forward"),Picker([this]{return FString::Chr(TEXT("XYZ")[M->ImportOptions.ForwardAxis]);},AxisNames,[this](int32 I){M->ImportOptions.ForwardAxis=I;M->GeometryOptionsChanged();}),150)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Copy([this]
        {if(!M->GeometrySource.IsValid())return FString(TEXT("Choose a file to begin."));FStudioGeometryAsset A;FString Error;if(!M->GeometryAssetForPreview(A,Error))return Error;return FString(TEXT("Source forward maps to +X; source up maps to +Z. Original coordinates are retained."));})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Commit];
    Options->SetVisibility(TAttribute<EVisibility>::CreateLambda([this]{return M->bImportPreview?EVisibility::Visible:EVisibility::Collapsed;}));
    auto Inspector=SNew(SRetainedFormScrollBox).Tag(TEXT("GeometryInspector")).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(10)
        +SScrollBox::Slot().Padding(14)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Live([this]{return M->IsReadingGeometry()?TEXT("Reading geometry…"):M->bImportPreview?TEXT("Preview before import"):TEXT("Geometry details");},12,Text,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[Copy([this]
            {
                const auto* Form=GeometryState->Drafts.Find(GeometryState->Selected);
                if(Form&&(*Form)->bRemoved)return FString(TEXT("Selected object removed. No mesh is being previewed for it."));
                return M->GeometryNotice.IsEmpty()?TEXT("Select an object to verify its source and inspect its mesh."):M->GeometryNotice;
            })]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[Cancel]
            +SVerticalBox::Slot().AutoHeight()[Options]
            +SVerticalBox::Slot().AutoHeight()[SAssignNew(GeometryState->Removed,SVerticalBox)]
            +SVerticalBox::Slot().AutoHeight()[SAssignNew(GeometryState->Details,SBox)
                .Visibility_Lambda([this]{return M->bImportPreview?EVisibility::Collapsed:EVisibility::Visible;})]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Copy([this]{return GeometryState->Notice;})]
            +SVerticalBox::Slot().AutoHeight()[Section(TEXT("Source mesh"),Copy([this]
            {const auto Mesh=M->GeometrySource.Mesh;if(!Mesh)return FString(TEXT("No verified mesh selected."));return FString::Printf(TEXT("%s\n%d vertices · %d triangles\n%d surface patches"),*FPaths::GetCleanFilename(M->GeometrySource.Path),Mesh->Positions.Num(),Mesh->Indices.Num()/3,Mesh->PatchNames.Num());}))]
            +SVerticalBox::Slot().AutoHeight()[Section(TEXT("Dimensions"),Copy([this]
            {const auto Mesh=M->GeometrySource.Mesh;if(!Mesh)return FString(TEXT("—"));FStudioGeometryAsset A;FString E;if(!M->GeometryAssetForPreview(A,E))return FString(TEXT("Choose valid units and axes to see dimensions in meters."));const FVector Size=StudioMeshImport::TransformedBounds(*Mesh,A).GetSize();return FString::Printf(TEXT("X  %.6g m\nY  %.6g m\nZ  %.6g m"),Size.X,Size.Y,Size.Z);}))]
            +SVerticalBox::Slot().AutoHeight()[Section(TEXT("Mesh diagnostics"),Copy([this]
            {const auto Mesh=M->GeometrySource.Mesh;if(!Mesh)return FString(TEXT("Available after reading the file."));return FString::Printf(TEXT("%d boundary edges\n%d nonmanifold edges\n%d inconsistent winding edges\n%d duplicate faces\n\nSource geometry is unchanged. These checks do not establish solver readiness."),Mesh->BoundaryEdges,Mesh->NonmanifoldEdges,Mesh->InconsistentEdges,Mesh->DuplicateFaces);}))]
            +SVerticalBox::Slot().AutoHeight()[Copy([this]{const auto Mesh=M->GeometrySource.Mesh;return Mesh?FString::Join(Mesh->Notes,TEXT("\n")):FString();})]];
    auto Objects=SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("Case objects"),12,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Button(TEXT("Choose file…"),TEXT("folder"),[this]{ImportGeometry();},Cyan)]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(GeometryObjectRows,SVerticalBox)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,6)[Undo]
        +SVerticalBox::Slot().AutoHeight()[Redo];
    return SNew(SVerticalBox)
        +SVerticalBox::Slot().FillHeight(1).Padding(0,0,0,6)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[SNew(SBox).WidthOverride(190)[SNew(SBorder).BorderImage(&PanelBrush).Padding(12)[Objects]]]
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[SNew(SBorder).BorderImage(&LineBrush).Padding(1)
                [SNew(SOverlay)+SOverlay::Slot()[SNew(SFlowViewport).Scene(Scene.Get())]
                    +SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(12)[SNew(SBorder).BorderImage(&PanelBrush).Padding(8,5)
                        [Live([this]
                        {
                            if(M->bImportPreview)return M->ImportOptions.MetersPerUnit>0?TEXT("Import preview · not yet in case"):TEXT("Import preview · source units unknown");
                            if(M->IsReadingGeometry())return TEXT("Reading selected object…");
                            if(!M->GeometrySource.IsValid())return TEXT("Object preview unavailable");
                            const auto* Form=GeometryState->Drafts.Find(M->SelectedGeometry);
                            return Form&&((*Form)->Edit.IsDirty()||(*Form)->bConflict)?TEXT("Applied object · unapplied edits not shown"):TEXT("Applied case object");
                        },10,Muted)]]
                    +SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(12)[SNew(SOrientationAxes).Scene(Scene.Get()).Visibility(EVisibility::HitTestInvisible)]
                    +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(12)[SNew(SVerticalBox)
                        +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0,0,0,6)[Button(TEXT("Fit geometry"),TEXT("fit"),[this]{Scene->FitCamera();})]
                        +SVerticalBox::Slot().AutoHeight()[SNew(SBorder).BorderImage(&PanelBrush).Padding(8,5)[Label(TEXT("Drag to orbit · Middle-drag to pan · Scroll to zoom"),9,Muted)]]]]]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(304)[SNew(SBorder).BorderImage(&PanelBrush).Padding(0)[Inspector]]]];
}

// THESIS: Save physical inspection objects against the exact recorded source and frame.
// OWN-WORLD: Existing dense Slate panels, compact numeric rows and cyan selection.
// STORY: Add an object, edit or place its coordinates, inspect the displayed frame, save.
// FIRST VIEWPORT: One Inspect action opens the existing right inspector; the scene stays live.
// FORM: Local Operate extension; slices, probes and measurements share a source-bound list.
// FINISH: Native workflow, two-size captures, independent review and documentation are required.
TSharedRef<SWidget> SStudioWorkspace::InspectionControls()
{
    InspectionProbe=MakeShared<FStudioProbeScheduler>();
    auto Add=SNew(SStudioMenuButton).Tag(TEXT("AddInspection")).ButtonStyle(&ButtonStyle())
        .IsEnabled_Lambda([this]{return !M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();})
        .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
        {
            auto Items=SNew(SVerticalBox);
            const TCHAR* Names[]={TEXT("Slice plane"),TEXT("Point probe"),TEXT("Line probe"),TEXT("Distance ruler"),TEXT("Angle ruler"),
                TEXT("Inlet seeds"),TEXT("Plane seeds"),TEXT("Line seeds"),TEXT("Selected-point seeds")};
            for(int32 I=0;I<9;++I)
            {
                auto B=Button(Names[I],TEXT("plus"),[this,I]{FSlateApplication::Get().DismissAllMenus();AddInspection(I);});
                B->SetTag(FName(*FString::Printf(TEXT("AddInspection%d"),I)));Items->AddSlot().AutoHeight().Padding(0,2)[B];
            }
            return Items;
        }).ButtonContent()[Label(TEXT("Add object"),10,Cyan)];
    auto Close=Button(TEXT("Close"),TEXT("collapse"),[this]{bInspectionOpen=false;CancelInspectionPlacement();});Close->SetTag(TEXT("CloseInspection"));
    return SNew(SBorder).Tag(TEXT("InspectionPanel")).BorderImage(&PanelBrush).Padding(10)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Inspection"),12,Text,true)]
                +SHorizontalBox::Slot().AutoWidth()[Close]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Add]
            +SVerticalBox::Slot().AutoHeight().MaxHeight(138).Padding(0,0,0,8)
                [SAssignNew(InspectionListScroll,SScrollBox).Tag(TEXT("InspectionObjectList")).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(8)
                    +SScrollBox::Slot()[SAssignNew(InspectionRows,SVerticalBox)]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Live([this]{return M->InspectionNotice;},9,Amber,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SBox).Tag(TEXT("InspectionReadout"))
                .Visibility_Lambda([this]{return M->FindInspectionObject(M->SelectedInspectionObject)?EVisibility::Visible:EVisibility::Collapsed;})
                [Live([this]{return InspectionSummary();},10,Text,true)]]
            +SVerticalBox::Slot().FillHeight(1)[SAssignNew(InspectionDetailScroll,SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(10)
                +SScrollBox::Slot()[SAssignNew(InspectionDetails,SBox)]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)[ViewHistoryControls()]];
}
void SStudioWorkspace::AddInspection(int32 Kind)
{
    CancelInspectionPlacement();
    const FBox Bounds=Kind>=5?StreamlineBounds():M->Solver->Descriptor().DisplayBounds;
    FVector Center=Bounds.GetCenter();
    if(M->Solver->Descriptor().SpatialDimensions==2)Center.Y=M->Solver->Descriptor().SourceOffset.Y;
    const double Length=Bounds.GetSize().GetMax()*.1;
    if(Kind==0){FStudioSliceObject S;S.Origin=Center;M->AddSlice(S);}
    else if(Kind<=2){FStudioProbeObject P;P.Kind=Kind==1?EStudioProbeKind::Point:EStudioProbeKind::Line;P.A=Center;P.B=Center+FVector(Length,0,0);M->AddProbe(P);}
    else if(Kind<5){FStudioRulerObject R;R.Kind=Kind==3?EStudioRulerKind::Distance:EStudioRulerKind::Angle;R.A=Center;R.B=Center+FVector(Length,0,0);R.C=R.B+FVector(0,0,Length);M->AddRuler(R);}
    else
    {
        FStudioSeedObject S;S.Kind=EStudioSeedKind(Kind-5);S.A=Center;S.Count=M->StreamlineSettings.AutomaticSeedCount;
        if(S.Kind==EStudioSeedKind::Line){S.A=Center-FVector(0,0,Length);S.B=Center+FVector(0,0,Length);}
        else if(S.Kind==EStudioSeedKind::Plane){S.B=FVector(Length*2,0,0);S.C=FVector(0,0,Length*2);}
        else if(S.Kind==EStudioSeedKind::Points)S.Points={Center};
        M->AddSeed(S);SeedPointIndex=0;
    }
    bInspectionOpen=true;
}
void SStudioWorkspace::RefreshInspectionControls()
{
    if(!InspectionRows||!InspectionDetails)return;
    const bool ProjectChanged=InspectionProject!=M->Project.Id;
    if(ProjectChanged){CancelInspectionPlacement();InspectionDetailId.Invalidate();}
    const bool SelectionChanged=ProjectChanged||LastInspectionSelection!=M->InspectionSelectionRevision||InspectionDetailId!=M->SelectedInspectionObject;
    if(SelectionChanged)CancelInspectionPlacement();
    const bool RowsChanged=ProjectChanged||LastInspectionRevision!=M->InspectionObjectsRevision;
    if(RowsChanged)
    {
        const auto Focus=FSlateApplication::Get().GetKeyboardFocusedWidget();FGuid Focused;
        for(const auto& Pair:InspectionRowButtons)if(Focus&&Pair.Value.Pin()==Focus)Focused=Pair.Key;
        if(InspectionListScroll)InspectionListScroll->ScrollDescendantIntoView(TSharedPtr<SWidget>(),false);
        InspectionRows->ClearChildren();InspectionRowButtons.Empty();
        auto Append=[&](const FStudioInspectionObject& Object,const TCHAR* Kind)
        {
            const FGuid Id=Object.Id;
            auto Select=SNew(SButton).Tag(FName(*(TEXT("Inspection_")+Id.ToString(EGuidFormats::Digits))))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(6,5))
                .OnClicked_Lambda([this,Id]{M->SelectInspectionObject(Id);return FReply::Handled();})
                [SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Font(Font(10)).Text(FText::FromString(Object.Name))
                        .OverflowPolicy(ETextOverflowPolicy::Ellipsis).ColorAndOpacity_Lambda([this,Id]{return M->SelectedInspectionObject==Id?Cyan:Text;})]
                    +SHorizontalBox::Slot().AutoWidth().Padding(7,0)[Label(Kind,8,Muted)]];
            InspectionRowButtons.Add(Id,Select);InspectionRows->AddSlot().AutoHeight().Padding(0,1)[Select];
        };
        for(const auto& S:M->InspectionObjects.Slices)Append(S,TEXT("Slice"));
        for(const auto& P:M->InspectionObjects.Probes)Append(P,P.Kind==EStudioProbeKind::Point?TEXT("Point"):TEXT("Line"));
        for(const auto& R:M->InspectionObjects.Rulers)Append(R,R.Kind==EStudioRulerKind::Distance?TEXT("Distance"):TEXT("Angle"));
        for(const auto& Seed:M->InspectionObjects.Seeds)Append(Seed,TEXT("Seeds"));
        if(InspectionRowButtons.IsEmpty())InspectionRows->AddSlot().AutoHeight()[Live([]{return TEXT("Add a slice, probe, ruler or seed set.");},10,Muted,true)];
        if(Focused.IsValid())if(const auto* Button=InspectionRowButtons.Find(Focused))FSlateApplication::Get().SetKeyboardFocus(Button->Pin(),EFocusCause::Navigation);
    }
    if((RowsChanged||SelectionChanged)&&InspectionListScroll)
        if(const auto* Selected=InspectionRowButtons.Find(M->SelectedInspectionObject))
        {
            // ScrollBox arranges offscreen rows from their desired sizes.
            // Measure the rebuilt list once before its deferred scroll; an
            // unpainted row's cached geometry cannot tell us whether to scroll.
            InspectionListScroll->SlatePrepass(GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale());
            InspectionListScroll->ScrollDescendantIntoView(Selected->Pin(),false,EDescendantScrollDestination::IntoView,4);
        }
    if(SelectionChanged)
    {
        InspectionDetailId=M->SelectedInspectionObject;SeedPointIndex=0;
        InspectionDetails->SetContent(InspectionObjectControls(InspectionDetailId));
        if(InspectionDetailScroll)InspectionDetailScroll->ScrollToStart();
    }
    InspectionProject=M->Project.Id;LastInspectionRevision=M->InspectionObjectsRevision;LastInspectionSelection=M->InspectionSelectionRevision;
}
void SStudioWorkspace::TickInspection()
{
    RefreshInspectionControls();if(!InspectionProbe)return;
    if(const auto Export=InspectionExport->Poll();Export.IsSet())
    {
        const FString Detail=Export->bSuccess?FString::Printf(TEXT("Exported %s · frame %d · %.9g s to %s"),*Export->ProbeName,Export->Frame.Index,Export->Frame.Time,*Export->Path):TEXT("Probe export failed: ")+Export->Error;
        M->Notice=Export->bSuccess?FString::Printf(TEXT("CSV saved · frame %d · %.9g s"),Export->Frame.Index,Export->Frame.Time):TEXT("Could not save probe CSV. See the activity log.");
        M->InspectionNotice=M->Notice;M->AddLog(Detail);
    }
    if(M->Workspace==EStudioWorkspace::Solve&&Scene.IsValid()&&Scene->HasPresentedFrame()&&Scene->PresentedProjectId()==M->Project.Id)
    {
        FStudioProbeMarkerRequest Request;Request.Project=M->Project.Id;
        const auto Field=Scene->PresentedField();const auto Identity=Field?Field->Identity():TOptional<FStudioFieldIdentity>();
        const auto Points=Field?Field->OriginalPoints():nullptr;
        if(Identity.IsSet()&&Points)
        {
            Request.Source={Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256};Request.Offset=Identity->SourceOffset;Request.Geometry=Points->Geometry;
            for(const auto& P:M->InspectionObjects.Probes)if(P.bVisible&&P.Source==Request.Source&&P.Method==EStudioProbeMethod::OriginalPoint&&P.PointId.IsSet())
                Request.Queries.Add({P.Id,P.PointId.GetValue()});
        }
        InspectionMarkers->Submit(MoveTemp(Request));
    }
    else InspectionMarkers->Clear();
    if(InspectionPlacement.IsSet()&&!IsInspectionPlacementCurrent())
    {CancelInspectionPlacement();M->InspectionNotice=TEXT("Placement cancelled because the object or source changed. Saved coordinates retained.");}
    const auto* Probe=M->FindProbe(M->SelectedInspectionObject);
    if(!bInspectionOpen||M->Workspace!=EStudioWorkspace::Solve||!Probe||!Probe->bVisible||!Scene.IsValid()||Scene->PresentedProjectId()!=M->Project.Id)
        InspectionProbe->Clear();
    else
    {
        FStudioProbeRequest R;R.ProjectId=M->Project.Id;R.PresentationId=Scene->GetCaptureCount();R.Probe=*Probe;
        R.DisplayedScalar=Scene->PresentedScalar().Id;R.Field=Scene->PresentedField();InspectionProbe->Submit(MoveTemp(R));
    }
    InspectionProbe->Tick();
    const auto* Result=CurrentInspectionProbeResult(M->SelectedInspectionObject);
    if(Result&&Result->Status==EStudioProbeStatus::Ready&&Result->Probe.Kind==EStudioProbeKind::Line)
    {if(!InspectionProfile||!InspectionProfile->Matches(*Result))InspectionProfile=StudioProbeProfile::Build(*Result);}
    else InspectionProfile.Reset();
}
void SStudioWorkspace::CancelInspectionPlacement()
{if(InspectionPlacement.IsSet()){InspectionPlacement.Reset();++InspectionPlacementRevision;}}
bool SStudioWorkspace::IsInspectionPlacementCurrent() const
{
    return InspectionPlacement.IsSet()&&bInspectionOpen&&M->Workspace==EStudioWorkspace::Solve&&!M->CameraPlacement()&&
        !M->IsRecordingLoadPending()&&!M->IsProjectOpenPending()&&M->FindInspectionObject(InspectionPlacement->ObjectId)&&
        InspectionPlacement->IsCurrent(M->Project.Id,M->SelectedInspectionObject,M->InspectionSource(),M->InspectionObjectsRevision);
}
void SStudioWorkspace::BeginInspectionPlacement(const FGuid& Id,int32 PointIndex)
{
    if(InspectionPlacement.IsSet())
    {CancelInspectionPlacement();M->InspectionNotice=TEXT("Placement cancelled. Saved coordinates retained.");return;}
    if(!Scene.IsValid()||!Scene->HasPresentedFrame()||Scene->PresentedProjectId()!=M->Project.Id)
    {M->InspectionNotice=TEXT("Wait for the flow view before placing this object.");return;}
    const auto Field=Scene->PresentedField();const auto Identity=Field?Field->Identity():TOptional<FStudioFieldIdentity>();
    const auto* Object=M->FindInspectionObject(Id);
    if(!Object||!Identity.IsSet()||!(Object->Source==M->InspectionSource())||
        !(Object->Source==FStudioInspectionSource{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256}))
    {M->InspectionNotice=TEXT("Wait for this object's recording to appear before placing it.");return;}
    FVector Anchor=FVector::ZeroVector;int32 Count=1;
    if(const auto* S=M->FindSlice(Id))Anchor=S->Origin;
    else if(const auto* P=M->FindProbe(Id))
    {if(P->Method==EStudioProbeMethod::OriginalPoint)return;Anchor=P->A;Count=P->Kind==EStudioProbeKind::Line?2:1;}
    else if(const auto* R=M->FindRuler(Id)){Anchor=R->A;Count=R->Kind==EStudioRulerKind::Angle?3:2;}
    else if(const auto* Seed=M->FindSeed(Id))
    {
        if(Seed->Kind==EStudioSeedKind::Inlet)return;
        SeedPlacementIndex=PointIndex==INDEX_NONE?SeedPointIndex:PointIndex;
        if(Seed->Kind==EStudioSeedKind::Points)
        {
            if(SeedPlacementIndex<0||SeedPlacementIndex>Seed->Points.Num()||SeedPlacementIndex>=StudioInspectionObjects::MaxSeedsPerObject)return;
            Anchor=Seed->Points[FMath::Min(SeedPlacementIndex,Seed->Points.Num()-1)];Count=1;
        }
        else{Anchor=Seed->A;Count=Seed->Kind==EStudioSeedKind::Plane?3:2;}
    }
    InspectionPlacement=FStudioInspectionPlacement::Begin(M->Project.Id,Id,Object->Source,M->InspectionObjectsRevision,
        Anchor,Count,Identity->SpatialDimensions,Identity->SourceOffset,Scene->PresentedCamera());
    ++InspectionPlacementRevision;M->InspectionNotice.Empty();
}
FString SStudioWorkspace::InspectionPlacementText() const
{
    if(const auto* P=M->FindProbe(M->SelectedInspectionObject);P&&P->Method==EStudioProbeMethod::OriginalPoint)return FString();
    if(const auto* S=M->FindSeed(M->SelectedInspectionObject);S&&S->Kind==EStudioSeedKind::Inlet)return FString();
    if(!IsInspectionPlacementCurrent())return TEXT("Coordinates use scene axes in meters. Place in view uses the source plane for 2D data, or a plane through the current point facing the camera for 3D data.");
    const auto& Draft=*InspectionPlacement;
    return FString::Printf(TEXT("Click %c · %d/%d points placed. Amber points are a draft. Right-drag to look around; the placement plane stays fixed. Escape cancels. Saved coordinates change after the last click."),
        TEXT("ABC")[Draft.Accepted.Num()],Draft.Accepted.Num(),Draft.RequiredPoints);
}
bool SStudioWorkspace::InspectionHover(const FGeometry& G,const TOptional<FVector2D>& ScreenPosition)
{
    if(!IsInspectionPlacementCurrent()||!Scene.IsValid())return false;
    TOptional<FVector2D> Pixel;
    const auto Field=Scene->PresentedField();const auto Identity=Field?Field->Identity():TOptional<FStudioFieldIdentity>();
    if(ScreenPosition.IsSet()&&Scene->HasPresentedFrame()&&Scene->PresentedProjectId()==M->Project.Id&&Identity.IsSet()&&
        InspectionPlacement->Source==FStudioInspectionSource{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256})
        Pixel=G.AbsoluteToLocal(ScreenPosition.GetValue());
    const auto Size=Scene->PresentedViewportSize();
    if(InspectionPlacement->UpdatePreview(Scene->PresentedCamera(),G.GetLocalSize(),Pixel,Size.Y>0?double(Size.X)/Size.Y:0))++InspectionPlacementRevision;
    return true;
}
bool SStudioWorkspace::InspectionClick(const FGeometry& G,const FPointerEvent& Event)
{
    if(!InspectionPlacement.IsSet())
    {
        if(!bInspectionOpen||M->Workspace!=EStudioWorkspace::Solve||!Scene.IsValid()||
            !Scene->HasPresentedFrame()||Scene->PresentedProjectId()!=M->Project.Id||M->IsProjectOpenPending()||M->IsRecordingLoadPending())return false;
        const auto Field=Scene->PresentedField();const auto Identity=Field?Field->Identity():TOptional<FStudioFieldIdentity>();
        if(!Identity.IsSet())return false;
        const FStudioInspectionSource Source{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256};
        if(!(Source==M->InspectionSource()))return false;
        const auto Overlay=StudioInspectionOverlay::Build(M->InspectionObjects,Source,M->Project.Id,M->SelectedInspectionObject,
            Scene->GetRenderedFlowBounds(),InspectionMarkers->Result(),Identity->SpatialDimensions,Identity->SourceOffset.Y,StudioStreamlines::DomainBounds(*Field,Scene->GetRenderedFlowBounds()));
        const auto Size=Scene->PresentedViewportSize();
        const FGuid Selected=StudioInspectionOverlay::Pick(Overlay,Scene->PresentedCamera(),G.GetLocalSize(),
            G.AbsoluteToLocal(Event.GetScreenSpacePosition()),M->SelectedInspectionObject,Size.Y>0?double(Size.X)/Size.Y:0);
        if(!Selected.IsValid())return false;
        M->SelectInspectionObject(Selected);M->InspectionNotice.Empty();return true;
    }
    if(!IsInspectionPlacementCurrent())
    {CancelInspectionPlacement();M->InspectionNotice=TEXT("Placement cancelled because the object or source changed.");return true;}
    InspectionHover(G,TOptional<FVector2D>(Event.GetScreenSpacePosition()));
    if(!InspectionPlacement->AcceptPreview())
    {M->InspectionNotice=TEXT("No point on the placement plane. Orbit the view or enter coordinates.");return true;}
    ++InspectionPlacementRevision;M->InspectionNotice.Empty();
    if(!InspectionPlacement->IsComplete())return true;
    const FGuid Id=InspectionPlacement->ObjectId;const auto Points=InspectionPlacement->Accepted;const int32 Count=Points.Num();
    M->EndViewEdit();
    if(M->FindSlice(Id))M->EditSlice(Id,[&](auto& S){S.Origin=Points[0];});
    else if(M->FindProbe(Id))M->EditProbe(Id,[&](auto& P){P.A=Points[0];if(Count==2)P.B=Points[1];});
    else if(M->FindSeed(Id))
    {
        const bool Saved=M->EditSeed(Id,[&](auto& S)
        {
            if(S.Kind==EStudioSeedKind::Points)
            {if(S.Points.IsValidIndex(SeedPlacementIndex))S.Points[SeedPlacementIndex]=Points[0];else S.Points.Add(Points[0]);}
            else{S.A=Points[0];S.B=S.Kind==EStudioSeedKind::Plane?(Points[1]-S.A)*2.:Points[1];if(Count==3)S.C=(Points[2]-S.A)*2.;}
        });
        if(!Saved){InspectionPlacement->Accepted.Pop();++InspectionPlacementRevision;return true;}
        SeedPointIndex=FMath::Max(0,SeedPlacementIndex);
    }
    else M->EditRuler(Id,[&](auto& R){R.A=Points[0];R.B=Points[1];if(Count==3)R.C=Points[2];});
    M->EndViewEdit();CancelInspectionPlacement();return true;
}
TSharedRef<SWidget> SStudioWorkspace::InspectionObjectControls(const FGuid& Id)
{
    if(!M->FindInspectionObject(Id))return Live([]{return TEXT("Select an object to edit its position and inspect the displayed frame.");},10,Muted,true);
    const FGuid Project=M->Project.Id;
    auto Exists=[this,Id,Project]{return Project==M->Project.Id&&M->SelectedInspectionObject==Id&&M->FindInspectionObject(Id)&&!M->IsProjectOpenPending();};
    auto Active=[this,Id,Exists]{return Exists()&&M->FindInspectionObject(Id)->Source==M->InspectionSource()&&!M->IsRecordingLoadPending();};
    auto Items=SNew(SVerticalBox);
    auto Copy=[&](TFunction<FString()> Read,FLinearColor Color=Muted)
    {Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SBox).Visibility_Lambda([Read]{return Read().IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})[Live(Read,9,Color,true)]];};
    if(const auto* Probe=M->FindProbe(Id))
    {
        auto Field=SNew(SStudioMenuButton).Tag(TEXT("InspectionField")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda(Active)
            .OnGetMenuContent_Lambda([this,Id,Active]() -> TSharedRef<SWidget>
            {
                auto Choices=SNew(SVerticalBox);
                if(!Active())return Choices;
                auto Add=[&](const FString& FieldId,const FString& TextValue,const FName& Tag)
                {
                    auto Option=Button(TextValue,TEXT("check"),[this,Id,Active,FieldId]
                    {
                        FSlateApplication::Get().DismissAllMenus();if(!Active())return;
                        if(!FieldId.IsEmpty()&&!M->Solver->Descriptor().Scalars.ContainsByPredicate([&](const auto& S){return S.Id==FieldId;}))return;
                        CancelInspectionPlacement();M->EndViewEdit();M->EditProbe(Id,[&](auto& P){P.Field=FieldId;});M->EndViewEdit();
                    });
                    const auto Mark=Icon(TEXT("check"));Mark->SetVisibility(M->FindProbe(Id)->Field==FieldId?EVisibility::Visible:EVisibility::Hidden);
                    const auto Caption=Label(TextValue);Caption->SetAutoWrapText(true);
                    Option->SetContent(SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Mark]
                        +SHorizontalBox::Slot().FillWidth(1).Padding(7,0,0,0).VAlign(VAlign_Center)[Caption]);
                    Option->SetTag(Tag);Choices->AddSlot().AutoHeight()[Option];
                };
                Add(FString(),TEXT("Follow viewport"),TEXT("InspectionFieldFollow"));
                for(const auto& Scalar:M->Solver->Descriptor().Scalars)
                    Add(Scalar.Id,Scalar.Label+TEXT(" · ")+Scalar.Unit,FName(*(TEXT("InspectionField_")+Scalar.Id)));
                return SNew(SBox).MaxDesiredHeight(260).WidthOverride(290)[SNew(SScrollBox)
                    .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Choices]];
            }).ButtonContent()[Live([this,Id,Active]
            {
                const auto* P=M->FindProbe(Id);if(!P||P->Field.IsEmpty())return FString(TEXT("Follow viewport"));
                if(Active())for(const auto& S:M->Solver->Descriptor().Scalars)if(S.Id==P->Field)return S.Label;
                return P->Field+TEXT(" · unavailable");
            },10)];
        Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Field"),Field,175)];
        auto Retry=Button(TEXT("Retry samples"),TEXT("refresh"),[this,Id]
        {
            const auto* R=CurrentInspectionProbeResult(Id);if(!R||R->Status!=EStudioProbeStatus::FieldLoadFailed)return;
            // The retry row disappears immediately when work starts. Give its
            // focus to the persistent object row before removing that control.
            if(const auto* RowButton=InspectionRowButtons.Find(Id))FSlateApplication::Get().SetKeyboardFocus(RowButton->Pin(),EFocusCause::Navigation);
            if(InspectionDetailScroll)InspectionDetailScroll->ScrollDescendantIntoView(TSharedPtr<SWidget>(),false);
            InspectionProbe->Clear();
        });
        Retry->SetTag(TEXT("RetryProbeSamples"));
        Retry->SetVisibility(TAttribute<EVisibility>::CreateLambda([this,Id]
        {const auto* R=CurrentInspectionProbeResult(Id);return R&&R->Status==EStudioProbeStatus::FieldLoadFailed?EVisibility::Visible:EVisibility::Collapsed;}));
        Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Retry];
        if(Probe->Kind==EStudioProbeKind::Line)
        {
            Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SStudioProbeProfile).Tag(TEXT("InspectionProfile"))
                .Background(Panel).Accent(Cyan).TextColor(Text).MutedColor(Muted).GridColor(Line)
                .Profile_Lambda([this,Id]() -> TSharedPtr<const FStudioProbeProfile>
                {const auto* R=CurrentInspectionProbeResult(Id);return R&&InspectionProfile&&InspectionProfile->Matches(*R)?InspectionProfile:TSharedPtr<const FStudioProbeProfile>();})];
        }
        auto Export=Button(TEXT("Export samples CSV"),TEXT("export"),[this,Id]{ExportInspectionProbe(Id);});
        Export->SetTag(TEXT("ExportProbeCSV"));
        Export->SetEnabled(TAttribute<bool>::CreateLambda([this,Id]
        {const auto* R=CurrentInspectionProbeResult(Id);return !InspectionExport->IsBusy()&&R&&R->Status==EStudioProbeStatus::Ready;}));
        Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Export];
    }
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SInspectionTextField,
        TFunction<FString()>([this,Id]{const auto* O=M->FindInspectionObject(Id);return O?O->Name:FString();}),
        TFunction<int32()>([this]{return M->InspectionObjectsRevision;}),
        TFunction<void(const FString&)>([this,Id,Exists](const FString& Value){if(Exists())M->RenameInspectionObject(Id,Value);}))
        .Tag(TEXT("InspectionName")).IsEnabled_Lambda(Exists)];
    Copy([this,Id,Exists]{return Exists()?TEXT("Source: ")+M->FindInspectionObject(Id)->Source.Dataset:FString();});
    Copy([Active]{return Active()?FString():TEXT("This object belongs to another recording. Open its source to edit coordinates; rename, visibility and deletion remain available.");},Amber);
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Check(TEXT("Visible"),[this,Id,Exists]{return Exists()&&M->FindInspectionObject(Id)->bVisible;},[this,Id,Exists](bool B){if(Exists())M->SetInspectionObjectVisible(Id,B);})];
    auto Actions=SNew(SHorizontalBox);
    Actions->AddSlot().FillWidth(1).Padding(0,0,4,0)[Button(TEXT("Duplicate"),TEXT("plus"),[this,Id,Exists]{if(Exists())M->DuplicateInspectionObject(Id);})];
    Actions->AddSlot().FillWidth(1)[Button(TEXT("Delete"),TEXT("stop"),[this,Id,Exists]{if(Exists()){CancelInspectionPlacement();M->DeleteInspectionObject(Id);}})];
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Actions];
    auto Place=Button(TEXT("Place in view"),TEXT("select"),[this,Id,Active]
    {if(Active())BeginInspectionPlacement(Id);});
    Place->SetContent(SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Icon(TEXT("select"))]
        +SHorizontalBox::Slot().FillWidth(1).Padding(7,0,0,0).VAlign(VAlign_Center)[Live([this]{return IsInspectionPlacementCurrent()?TEXT("Cancel placement"):TEXT("Place in view");},10)]);
    Place->SetTag(TEXT("PlaceInspection"));Place->SetEnabled(TAttribute<bool>::CreateLambda([this,Id,Active]{const auto* P=M->FindProbe(Id);return Active()&&(!P||P->Method==EStudioProbeMethod::Interpolated);}));
    Place->SetVisibility(TAttribute<EVisibility>::CreateLambda([this,Id]{const auto* P=M->FindProbe(Id);const auto* S=M->FindSeed(Id);
        return (P&&P->Method==EStudioProbeMethod::OriginalPoint)||(S&&S->Kind==EStudioSeedKind::Inlet)?EVisibility::Collapsed:EVisibility::Visible;}));
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Place];Copy([this]{return InspectionPlacementText();});
    auto Numeric=[this,Id,Active](const FString& Tag,TFunction<double()> Read,TFunction<void(double)> Write)
    {
        auto Input=SNew(SCameraDistanceField,MoveTemp(Read),
            TFunction<int32()>([this]{return M->InspectionObjectsRevision;}),
            TFunction<void(double)>([this,Active,Write](double V){if(Active()){CancelInspectionPlacement();M->EndViewEdit();Write(V);M->EndViewEdit();}}));
        Input->SetTag(FName(*Tag));Input->SetEnabled(TAttribute<bool>::CreateLambda(Active));return Input;
    };
    auto Picker=[](TFunction<FString()> Read,TArray<FString> Names,TFunction<void(int32)> Select) -> TSharedRef<SWidget>
    {
        return SNew(SStudioMenuButton).ButtonStyle(&ButtonStyle())
            .OnGetMenuContent_Lambda([Names,Select]() -> TSharedRef<SWidget>
            {
                auto Items=SNew(SVerticalBox);
                for(int32 I=0;I<Names.Num();++I)Items->AddSlot().AutoHeight()[Button(Names[I],TEXT("check"),[Select,I]{Select(I);FSlateApplication::Get().DismissAllMenus();})];
                return Items;
            }).ButtonContent()[Live(Read,10)];
    };
    auto Coordinates=[&](const TCHAR* Name,TFunction<FVector()> Read,TFunction<void(FVector)> Write,TFunction<bool()> Visible=TFunction<bool()>())
    {
        auto Group=SNew(SVerticalBox);
        Group->AddSlot().AutoHeight().Padding(0,4,0,6)[Label(Name,10,Text,true)];
        for(int32 Axis=0;Axis<3;++Axis)
        {
            const FString LabelText=FString::Chr(TEXT("XYZ")[Axis])+TEXT(" (m)");
            Group->AddSlot().AutoHeight().Padding(0,0,0,5)[Row(LabelText,Numeric(FString(TEXT("Inspection"))+Name+TEXT("XYZ")[Axis],
                [Read,Axis]{return Read()[Axis];},[Read,Write,Axis](double V){auto P=Read();P[Axis]=V;Write(P);}),110)];
        }
        Items->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([Visible]{return !Visible||Visible()?EVisibility::Visible:EVisibility::Collapsed;})[Group]];
    };
    if(M->FindSlice(Id))
    {
        Coordinates(TEXT("Origin"),[this,Id]{const auto* S=M->FindSlice(Id);return S?S->Origin:FVector::ZeroVector;},[this,Id](FVector P){M->EditSlice(Id,[&](auto& S){S.Origin=P;});});
        struct FNormalDraft {FVector Value;int32 Revision=INDEX_NONE;bool bEdited=false;};
        auto Normal=MakeShared<FNormalDraft>();
        auto SynchronizeNormal=[this,Id,Normal]
        {
            if(Normal->Revision==M->InspectionObjectsRevision)return;
            const auto* Slice=M->FindSlice(Id);Normal->Value=Slice?Slice->Normal:FVector::RightVector;
            Normal->Revision=M->InspectionObjectsRevision;Normal->bEdited=false;
        };
        SynchronizeNormal();auto NormalInputs=SNew(SHorizontalBox);
        for(int32 Axis=0;Axis<3;++Axis)
        {
            auto Input=SNew(SCameraDistanceField,
                TFunction<double()>([Normal,SynchronizeNormal,Axis]{SynchronizeNormal();return Normal->Value[Axis];}),
                TFunction<int32()>([this]{return M->InspectionObjectsRevision;}),
                TFunction<void(double)>([this,Normal,SynchronizeNormal,Axis,Active](double V)
                {
                    SynchronizeNormal();if(!Active())return;
                    if(!FMath::IsFinite(V)){M->InspectionNotice=TEXT("Enter a finite normal component.");return;}
                    Normal->Value[Axis]=V;Normal->bEdited=true;
                })).IsEnabled_Lambda(Active).Tag(FName(*(FString(TEXT("InspectionNormal"))+TEXT("XYZ")[Axis])));
            NormalInputs->AddSlot().FillWidth(1).Padding(0,0,4,0)[Input];
        }
        Items->AddSlot().AutoHeight().Padding(0,5,0,5)[Label(TEXT("Normal X / Y / Z"),10,Text,true)];
        Items->AddSlot().AutoHeight().Padding(0,0,0,5)[NormalInputs];
        auto ApplyNormal=Button(TEXT("Apply normal"),TEXT("check"),[this,Id,Normal,SynchronizeNormal,Active]
        {
            // Undo, reopen and other object edits invalidate every component of
            // a pending normal. Even an immediate Apply cannot resurrect it.
            SynchronizeNormal();if(!Active()||!Normal->bEdited)return;
            const double Scale=Normal->Value.GetAbsMax();
            if(!FMath::IsFinite(Scale)||Scale==0){M->InspectionNotice=TEXT("The slice normal must have at least one nonzero component.");return;}
            const FVector Direction=(Normal->Value/Scale).GetSafeNormal();
            CancelInspectionPlacement();M->EndViewEdit();
            M->EditSlice(Id,[&](auto& S){S.Normal=Direction;});M->EndViewEdit();SynchronizeNormal();
        });
        ApplyNormal->SetTag(TEXT("InspectionApplyNormal"));ApplyNormal->SetEnabled(TAttribute<bool>::CreateLambda(Active));
        Items->AddSlot().AutoHeight().Padding(0,0,0,8)[ApplyNormal];
        auto Axes=SNew(SHorizontalBox);
        for(int32 Axis=0;Axis<3;++Axis)
        {
            auto Preset=Button(FString::Chr(TEXT("XYZ")[Axis]),TEXT("slice"),[this,Id,Axis,Active]
            {if(Active()){CancelInspectionPlacement();M->EndViewEdit();M->EditSlice(Id,[&](auto& S){S.Normal=FVector::ZeroVector;S.Normal[Axis]=1;});M->EndViewEdit();}});
            Preset->SetTag(FName(*(FString(TEXT("InspectionNormalAxis"))+TEXT("XYZ")[Axis])));Preset->SetEnabled(TAttribute<bool>::CreateLambda(Active));
            Axes->AddSlot().FillWidth(1).Padding(0,0,4,0)[Preset];
        }
        Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Axes];
        Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Opacity"),Numeric(TEXT("InspectionOpacity"),[this,Id]{const auto* S=M->FindSlice(Id);return S?S->Opacity:0.;},[this,Id](double V){M->EditSlice(Id,[&](auto& S){S.Opacity=V;});}),110)];
    }
    else if(const auto* Probe=M->FindProbe(Id))
    {
        const bool bLineProbe=Probe->Kind==EStudioProbeKind::Line;
        auto Interpolated=[this,Id]{const auto* P=M->FindProbe(Id);return P&&P->Method==EStudioProbeMethod::Interpolated;};
        if(!bLineProbe)
        {
            auto Mode=SNew(SStudioMenuButton).Tag(TEXT("InspectionSampling")).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda(Active)
                .OnGetMenuContent_Lambda([this,Id,Active]() -> TSharedRef<SWidget>
                {
                    auto Choices=SNew(SVerticalBox);
                    for(int32 I=0;I<2;++I)
                    {
                        auto Option=Button(I==0?TEXT("Interpolated position"):TEXT("Original point ID"),TEXT("check"),[this,Id,I,Active]
                        {
                            FSlateApplication::Get().DismissAllMenus();if(!Active())return;
                            if((I==0)==(M->FindProbe(Id)->Method==EStudioProbeMethod::Interpolated))return;
                            TOptional<int64> PointId;
                            if(I==1)
                            {
                                const auto Field=Scene->HasCurrentFrame()?Scene->PresentedField():nullptr;
                                const auto Points=Field?Field->OriginalPoints():nullptr;
                                if(!Points||Points->Geometry->PointIds.IsEmpty())return;
                                PointId=Points->Geometry->PointIds[0];
                            }
                            CancelInspectionPlacement();M->EndViewEdit();
                            M->EditProbe(Id,[&](auto& P){P.Method=I==0?EStudioProbeMethod::Interpolated:EStudioProbeMethod::OriginalPoint;P.PointId=PointId;});
                            M->EndViewEdit();
                        });
                        Option->SetTag(FName(*FString::Printf(TEXT("InspectionSampling%d"),I)));
                        if(I==1)Option->SetEnabled(TAttribute<bool>::CreateLambda([this,Active]
                        {const auto Field=Scene->HasCurrentFrame()?Scene->PresentedField():nullptr;return Active()&&Field&&Field->OriginalPoints().IsValid();}));
                        Choices->AddSlot().AutoHeight()[Option];
                    }
                    return Choices;
                }).ButtonContent()[Live([Interpolated]{return Interpolated()?TEXT("Interpolated position"):TEXT("Original point ID");},10)];
            Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Sampling"),Mode,175)];
            auto PointId=SNew(SInspectionTextField,
                TFunction<FString()>([this,Id]{const auto* P=M->FindProbe(Id);return P&&P->PointId.IsSet()?FString::Printf(TEXT("%lld"),P->PointId.GetValue()):FString();}),
                TFunction<int32()>([this]{return M->InspectionObjectsRevision;}),
                TFunction<void(const FString&)>([this,Id,Active](const FString& TextValue)
                {
                    if(!Active())return;
                    const FString Trimmed=TextValue.TrimStartAndEnd();int64 Value;
                    if(!LexTryParseString(Value,*Trimmed)||FString::Printf(TEXT("%lld"),Value)!=Trimmed)
                    {M->InspectionNotice=TEXT("Enter an exact signed 64-bit integer point ID, without decimals or leading zeros.");return;}
                    M->EndViewEdit();M->EditProbe(Id,[&](auto& P){if(P.Method==EStudioProbeMethod::OriginalPoint)P.PointId=Value;});M->EndViewEdit();
                })).Tag(TEXT("InspectionPointId")).IsEnabled_Lambda(Active);
            Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SBox).Visibility_Lambda([Interpolated]{return Interpolated()?EVisibility::Collapsed:EVisibility::Visible;})
                [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Original point ID"),10,Text,true)]
                    +SVerticalBox::Slot().AutoHeight()[PointId]]];
            Copy([this,Id,Interpolated]
            {
                if(Interpolated())return FString();
                const auto* R=InspectionMarkers->Result();const auto* P=M->FindProbe(Id);
                const auto Position=R&&P?R->Position(M->Project.Id,*P):TOptional<FVector>();
                if(!Position.IsSet())return FString(TEXT("No resolved point position. The ID must exist in the original recording."));
                const FVector V=Position.GetValue();return FString::Printf(TEXT("Recorded position · scene axes (m)\nX %.12g   Y %.12g   Z %.12g"),V.X,V.Y,V.Z);
            });
        }
        Coordinates(TEXT("A"),[this,Id]{const auto* P=M->FindProbe(Id);return P?P->A:FVector::ZeroVector;},[this,Id](FVector A){M->EditProbe(Id,[&](auto& P){P.A=A;});},Interpolated);
        if(bLineProbe)
        {
            Coordinates(TEXT("B"),[this,Id]{const auto* P=M->FindProbe(Id);return P?P->B:FVector::ZeroVector;},[this,Id](FVector B){M->EditProbe(Id,[&](auto& P){P.B=B;});});
            Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Samples"),Numeric(TEXT("InspectionSamples"),[this,Id]{const auto* P=M->FindProbe(Id);return P?double(P->Samples):2.;},[this,Id](double V)
                {if(V!=FMath::FloorToDouble(V)||V<2||V>StudioInspectionObjects::MaxLineSamples){M->InspectionNotice=TEXT("Enter a whole sample count from 2 to 1024.");return;}M->EditProbe(Id,[&](auto& P){P.Samples=int32(V);});}),110)];
        }
        Copy([]{return TEXT("Samples use the displayed physical frame. Choose a fixed field or follow the viewport field. Locations outside source coverage have no value.");});
        Items->AddSlot().AutoHeight().Padding(0,5,0,5)[Live([this,Id]
        {
            const auto* Result=CurrentInspectionProbeResult(Id);if(!Result)return FString(TEXT("Sampling displayed frame…"));
            if(Result->Status!=EStudioProbeStatus::Ready)return Result->Message;
            return FString::Printf(TEXT("%s · %s\nFrame %d · %.9g s\n%s"),*Result->Label,*Result->Unit,Result->Identity->Frame.Index,Result->Identity->Frame.Time,*Result->Method);
        },9,Muted,true)];
        Items->AddSlot().AutoHeight()[Live([this,Id]
        {
            const auto* R=CurrentInspectionProbeResult(Id);if(!R||R->Status!=EStudioProbeStatus::Ready)return FString();
            FString TextValue;
            for(int32 I=0;I<R->Samples.Num();++I)
            {
                const auto& S=R->Samples[I];FString Value;
                if(S.Value.IsSet())Value=FString::Printf(TEXT("%.10g %s"),S.Value.GetValue(),*R->Unit);
                else if(S.Status==EStudioProbeSampleStatus::OffPlane)Value=TEXT("No value · outside source plane");
                else if(S.Status==EStudioProbeSampleStatus::NoInterpolation)Value=TEXT("No interpolation mesh supplied");
                else if(S.Status==EStudioProbeSampleStatus::MissingPoint)Value=TEXT("No value · original point ID not found");
                else Value=TEXT("No value · outside coverage");
                TextValue+=R->Samples.Num()==1?Value:FString::Printf(TEXT("%.6g m  %s\n"),S.DistanceAlongLineMeters,*Value);
            }
            return TextValue;
        },10,Text,true)];
    }
    else if(M->FindSeed(Id))Items->AddSlot().AutoHeight()[SeedControls(Id)];
    else if(const auto* Ruler=M->FindRuler(Id))
    {
        const int32 Count=Ruler->Kind==EStudioRulerKind::Angle?3:2;
        for(int32 Point=0;Point<Count;++Point)
            Coordinates(Point==0?TEXT("A"):Point==1?TEXT("B"):TEXT("C"),[this,Id,Point]{const auto* R=M->FindRuler(Id);return R?(Point==0?R->A:Point==1?R->B:R->C):FVector::ZeroVector;},
                [this,Id,Point](FVector P){M->EditRuler(Id,[&](auto& R){if(Point==0)R.A=P;else if(Point==1)R.B=P;else R.C=P;});});
        if(Count==2)Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Row(TEXT("Unit"),Picker([this,Id]{const auto* R=M->FindRuler(Id);return R?R->Unit:FString();},
            {TEXT("m"),TEXT("cm"),TEXT("mm"),TEXT("in"),TEXT("ft")},[this,Id,Active](int32 I){if(Active()){const TCHAR* Units[]={TEXT("m"),TEXT("cm"),TEXT("mm"),TEXT("in"),TEXT("ft")};M->EndViewEdit();M->EditRuler(Id,[&](auto& R){R.Unit=Units[I];});}}),110)];
        Copy([this,Id]
        {
            const auto* R=M->FindRuler(Id);if(!R)return FString();const auto Value=StudioInspectionObjects::Measurement(*R);
            return Value.IsSet()?FString::Printf(TEXT("Measurement: %.10g %s"),Value.GetValue(),R->Kind==EStudioRulerKind::Angle?TEXT("deg"):*R->Unit):TEXT("Angle is undefined because one arm has zero length.");
        },Text);
    }
    return Items;
}

TSharedRef<SWidget> SStudioWorkspace::SeedControls(const FGuid& Id)
{
    const auto Project=M->Project.Id;const auto Source=M->Solver;
    auto Current=[this,Id,Project,Source]{const auto* S=M->FindSeed(Id);return M->Project.Id==Project&&M->Solver==Source&&
        M->SelectedInspectionObject==Id&&S&&S->Source==M->InspectionSource()&&!M->IsRecordingLoadPending();};
    auto Items=SNew(SVerticalBox).IsEnabled_Lambda(Current);
    const auto* Seed=M->FindSeed(Id);if(!Seed)return Items;
    auto Numeric=[this,Current](const FString& Tag,const FString& Caption,TFunction<double()> Read,TFunction<bool(double)> Write,double Min,double Max,bool Integer=false)
    {
        return SNew(SValidatedViewNumber).InputTag(FName(*Tag)).Caption(Caption).Digits(17).Read(MoveTemp(Read)).Current(Current)
            .Revision([this]{return uint64(M->InspectionObjectsRevision)*1024+SeedPointIndex;})
            .Minimum(Min).Maximum(Max).Integer(Integer).ReportError([this](FString E){M->InspectionNotice=MoveTemp(E);})
            .TryWrite([this,Current,Write](double V)
            {if(!Current())return false;CancelInspectionPlacement();M->EndViewEdit();const bool Good=Write(V);M->EndViewEdit();return Good;});
    };
    auto Coordinates=[&](const TCHAR* Name,FVector FStudioSeedObject::*Member)
    {
        Items->AddSlot().AutoHeight().Padding(0,6,0,6)[Label(Name,10,Text,true)];
        for(int32 Axis=0;Axis<3;++Axis)
        {
            const FString Tag=FString(TEXT("Seed"))+Name+TEXT("XYZ")[Axis];
            Items->AddSlot().AutoHeight().Padding(0,0,0,5)[Row(FString::Chr(TEXT("XYZ")[Axis])+TEXT(" (m)"),Numeric(Tag,Name,
                [this,Id,Member,Axis]{const auto* S=M->FindSeed(Id);return S?(S->*Member)[Axis]:0.;},
                [this,Id,Member,Axis](double V){return M->EditSeed(Id,[=](auto& S){(S.*Member)[Axis]=V;});},-1.e8,1.e8),110)];
        }
    };
    const TCHAR* Kind=Seed->Kind==EStudioSeedKind::Inlet?TEXT("Inlet face"):Seed->Kind==EStudioSeedKind::Plane?TEXT("Rectangular plane"):
        Seed->Kind==EStudioSeedKind::Line?TEXT("Line endpoints"):TEXT("Selected positions");
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(Kind,11,Text,true)];
    if(Seed->Kind!=EStudioSeedKind::Points)
    {
        Items->AddSlot().AutoHeight().Padding(0,0,0,7)[Row(TEXT("Seed count"),Numeric(TEXT("SeedCount"),TEXT("Seed count"),
            [this,Id]{const auto* S=M->FindSeed(Id);return S?double(S->Count):1.;},
            [this,Id](double V){return M->EditSeed(Id,[V](auto& S){S.Count=int32(V);});},1,512,true),110)];
        Items->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SSlider).MinValue(1).MaxValue(512).StepSize(1)
            .Value_Lambda([this,Id]{const auto* S=M->FindSeed(Id);return S?float(S->Count):1.f;})
            .OnMouseCaptureBegin_Lambda([this]{M->BeginViewEdit(TEXT("Seed density"));})
            .OnMouseCaptureEnd_Lambda([this]{M->EndViewEdit();})
            .OnValueChanged_Lambda([this,Id,Current](float V){if(Current())M->EditSeed(Id,[V](auto& S){S.Count=FMath::RoundToInt(V);});})];
    }
    if(Seed->Kind==EStudioSeedKind::Inlet)
    {
        for(int32 Axis=0;Axis<3;++Axis)
        {
            auto Faces=SNew(SHorizontalBox);
            for(bool Upper:{false,true})
            {
                const FString Name=FString::Chr(TEXT("XYZ")[Axis])+(Upper?TEXT(" maximum"):TEXT(" minimum"));
                auto Choice=SNew(SButton).Tag(FName(*FString::Printf(TEXT("SeedFace%d%d"),Axis,int32(Upper))))
                    .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(4,7))
                    .IsEnabled_Lambda([this,Axis]{return M->Solver->Descriptor().SpatialDimensions==3||Axis!=1;})
                    .OnClicked_Lambda([this,Id,Current,Axis,Upper]
                    {if(Current()){M->EndViewEdit();M->EditSeed(Id,[=](auto& S){S.InletAxis=Axis;S.bUpperFace=Upper;});M->EndViewEdit();}return FReply::Handled();})
                    [SNew(STextBlock).Font(Font(9)).Text(FText::FromString(Name)).ColorAndOpacity_Lambda([this,Id,Axis,Upper]
                    {const auto* S=M->FindSeed(Id);return S&&S->InletAxis==Axis&&S->bUpperFace==Upper?Cyan:Text;})];
                Faces->AddSlot().FillWidth(1).Padding(Upper?4:0,0,0,0)[Choice];
            }
            Items->AddSlot().AutoHeight().Padding(0,0,0,5)[Faces];
        }
        Items->AddSlot().AutoHeight().Padding(0,6,0,0)[Live([]{return TEXT("Seeds are inset 0.23% from this face. A 2D recording places them on its original X/Z plane.");},9,Muted,true)];
    }
    else if(Seed->Kind==EStudioSeedKind::Plane)
    {
        Coordinates(TEXT("Center"),&FStudioSeedObject::A);Coordinates(TEXT("Span U"),&FStudioSeedObject::B);Coordinates(TEXT("Span V"),&FStudioSeedObject::C);
        Items->AddSlot().AutoHeight().Padding(0,6,0,0)[Live([]{return TEXT("U and V are full span vectors. Place in view: click center, U edge, then V edge. The two edge distances define half spans.");},9,Muted,true)];
    }
    else if(Seed->Kind==EStudioSeedKind::Line)
    {Coordinates(TEXT("A"),&FStudioSeedObject::A);Coordinates(TEXT("B"),&FStudioSeedObject::B);}
    else
    {
        auto Navigation=SNew(SHorizontalBox);
        for(int32 Delta:{-1,1})
        {
            auto Move=Button(Delta<0?TEXT("Previous"):TEXT("Next"),Delta<0?TEXT("collapse"):TEXT("step"),[this,Id,Delta,Current]
            {if(Current()){CancelInspectionPlacement();SeedPointIndex=FMath::Clamp(SeedPointIndex+Delta,0,M->FindSeed(Id)->Points.Num()-1);}});
            Move->SetTag(Delta<0?TEXT("SeedPreviousPoint"):TEXT("SeedNextPoint"));
            Move->SetEnabled(TAttribute<bool>::CreateLambda([this,Id,Delta,Current]
            {const auto* S=M->FindSeed(Id);return Current()&&S&&SeedPointIndex+Delta>=0&&SeedPointIndex+Delta<S->Points.Num();}));
            Navigation->AddSlot().FillWidth(1).Padding(Delta>0?4:0,0,0,0)[Move];
        }
        Items->AddSlot().AutoHeight().Padding(0,0,0,7)[Live([this,Id]
        {const auto* S=M->FindSeed(Id);return S?FString::Printf(TEXT("Point %d of %d"),FMath::Min(SeedPointIndex,S->Points.Num()-1)+1,S->Points.Num()):FString();},10)];
        Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Navigation];
        for(int32 Axis=0;Axis<3;++Axis)Items->AddSlot().AutoHeight().Padding(0,0,0,5)
            [Row(FString::Chr(TEXT("XYZ")[Axis])+TEXT(" (m)"),Numeric(FString(TEXT("SeedPoint"))+TEXT("XYZ")[Axis],TEXT("Point coordinate"),
                [this,Id,Axis]{const auto* S=M->FindSeed(Id);return S?S->Points[FMath::Clamp(SeedPointIndex,0,S->Points.Num()-1)][Axis]:0.;},
                [this,Id,Axis](double V){return M->EditSeed(Id,[&](auto& S){S.Points[FMath::Clamp(SeedPointIndex,0,S.Points.Num()-1)][Axis]=V;});},-1.e8,1.e8),110)];
        auto Add=Button(TEXT("Pick another point"),TEXT("plus"),[this,Id,Current]
        {if(Current())BeginInspectionPlacement(Id,M->FindSeed(Id)->Points.Num());});Add->SetTag(TEXT("SeedAddPoint"));
        Add->SetEnabled(TAttribute<bool>::CreateLambda([this,Id,Current]{const auto* S=M->FindSeed(Id);return Current()&&S&&S->Points.Num()<512;}));
        auto Remove=Button(TEXT("Remove this point"),TEXT("stop"),[this,Id,Current]
        {
            if(!Current()||M->FindSeed(Id)->Points.Num()<=1)return;CancelInspectionPlacement();M->EndViewEdit();
            M->EditSeed(Id,[this](auto& S){S.Points.RemoveAt(FMath::Clamp(SeedPointIndex,0,S.Points.Num()-1));});M->EndViewEdit();
            SeedPointIndex=FMath::Min(SeedPointIndex,M->FindSeed(Id)->Points.Num()-1);
        });Remove->SetTag(TEXT("SeedRemovePoint"));
        Remove->SetEnabled(TAttribute<bool>::CreateLambda([this,Id,Current]{const auto* S=M->FindSeed(Id);return Current()&&S&&S->Points.Num()>1;}));
        Items->AddSlot().AutoHeight().Padding(0,5,0,5)[Add];Items->AddSlot().AutoHeight()[Remove];
    }
    return Items;
}

FString SStudioWorkspace::InspectionSummary() const
{
    const FGuid Id=M->SelectedInspectionObject;
    if(const auto* Slice=M->FindSlice(Id))
    {
        if(!(Slice->Source==M->InspectionSource()))return TEXT("This slice belongs to a different recording. Open its original source.");
        if(!Slice->bVisible)return TEXT("Slice hidden");
        return Scene->HasCurrentFrame()?Scene->PresentedSliceNotice(Id):TEXT("Updating slice for the displayed field…");
    }
    if(const auto* R=M->FindRuler(Id))
    {
        const auto Value=StudioInspectionObjects::Measurement(*R);
        const FString ValueText=Value.IsSet()?FString::Printf(TEXT("%.10g %s"),Value.GetValue(),R->Kind==EStudioRulerKind::Angle?TEXT("deg"):*R->Unit):TEXT("Undefined angle · an arm has zero length");
        return (IsInspectionPlacementCurrent()?FString(TEXT("Saved · ")):FString())+ValueText;
    }
    if(const auto* Seed=M->FindSeed(Id))
    {
        if(!(Seed->Source==M->InspectionSource()))return TEXT("This seed set belongs to another recording.");
        if(!Seed->bVisible)return TEXT("Seed set hidden");
        if(M->StreamlineSettings.bAutomaticSeeds)return TEXT("Automatic flow is active. Choose Saved seed sets in streamline settings to use these seeds.");
        if(Scene->HasCurrentFrame())if(const auto* Notice=Scene->PresentedStreams().SeedNotices.Find(Id))return *Notice;
        return TEXT("All seed sets:\n")+StreamlineSummary();
    }
    const auto* Probe=M->FindProbe(Id);if(!Probe)return FString();
    if(!Probe->bVisible)return TEXT("Probe hidden");
    const auto* R=CurrentInspectionProbeResult(Id);if(!R)return TEXT("Sampling displayed frame…");
    if(R->Status!=EStudioProbeStatus::Ready)return R->Message;
    const FString Stamp=(IsInspectionPlacementCurrent()?FString(TEXT("Saved position · ")):FString())+
        FString::Printf(TEXT("%s · frame %d · %.6g s"),*R->Label,R->Identity->Frame.Index,R->Identity->Frame.Time);
    int32 Valid=0;double Low=MAX_dbl,High=-MAX_dbl;
    for(const auto& S:R->Samples)if(S.Value.IsSet()){++Valid;Low=FMath::Min(Low,S.Value.GetValue());High=FMath::Max(High,S.Value.GetValue());}
    if(R->Samples.Num()==1)
    {
        if(Valid)return FString::Printf(TEXT("%.10g %s\n%s"),Low,*R->Unit,*Stamp);
        const auto Status=R->Samples[0].Status;
        return (Status==EStudioProbeSampleStatus::OffPlane?FString(TEXT("No value · outside source plane\n")):
            Status==EStudioProbeSampleStatus::NoInterpolation?FString(TEXT("No interpolation mesh supplied\n")):
            Status==EStudioProbeSampleStatus::MissingPoint?FString(TEXT("No value · original point ID not found\n")):FString(TEXT("No value · outside source coverage\n")))+Stamp;
    }
    return Valid?FString::Printf(TEXT("%.6g to %.6g %s · %d/%d samples\n%s"),Low,High,*R->Unit,Valid,R->Samples.Num(),*Stamp):TEXT("No supported samples\n")+Stamp;
}

const FStudioProbeResult* SStudioWorkspace::CurrentInspectionProbeResult(const FGuid& Id) const
{
    if(!InspectionProbe||!Scene.IsValid()||!Scene->HasPresentedFrame()||Scene->PresentedProjectId()!=M->Project.Id||
        M->SelectedInspectionObject!=Id||!bInspectionOpen||M->Workspace!=EStudioWorkspace::Solve)return nullptr;
    const auto* Probe=M->FindProbe(Id);const auto* Result=InspectionProbe->Result();
    if(!Probe||!Probe->bVisible||!Result)return nullptr;
    FStudioProbeRequest Request;Request.ProjectId=M->Project.Id;Request.PresentationId=Scene->GetCaptureCount();Request.Probe=*Probe;
    Request.DisplayedScalar=Scene->PresentedScalar().Id;Request.Field=Scene->PresentedField();
    return Result->Matches(Request)?Result:nullptr;
}

void SStudioWorkspace::ExportInspectionProbe(const FGuid& Id)
{
    const auto* Result=CurrentInspectionProbeResult(Id);
    if(InspectionExport->IsBusy()||!Result||Result->Status!=EStudioProbeStatus::Ready)
    {M->InspectionNotice=TEXT("Wait for the displayed-frame samples or the current export to finish.");return;}
    // The native modal may allow playback or source changes. Everything below
    // owns the click-time values and provenance, independently of that state.
    auto Frozen=*Result;FString Path;
    const FString Name=FPaths::MakeValidFileName(Frozen.Probe.Name)+FString::Printf(TEXT("-frame-%d"),Frozen.Identity->Frame.Index);
    if(!StudioFileDialog::ProbeCSV(Name,Path)){M->InspectionNotice=TEXT("Probe export cancelled.");return;}
    if(InspectionExport->Start(MoveTemp(Frozen),Path))M->InspectionNotice=TEXT("Writing captured probe samples…");
    else M->InspectionNotice=TEXT("The captured probe result could not be exported.");
}

struct FStudioBoundaryForm
{
    FStudioBoundaryEdit Edit;
    FStudioBoundaryTarget Target;
    bool bConflict=false,bMissing=false,bSynchronizing=false;
    TWeakPtr<SEditableTextBox> Inputs[6];
    FString& Value(int32 I){return I==0?Edit.Name:I<4?Edit.Velocity[I-1]:I==4?Edit.Pressure:Edit.Temperature;}
    void Synchronize()
    {bSynchronizing=true;for(int32 I=0;I<6;++I)if(auto Input=Inputs[I].Pin())Input->SetText(FText::FromString(Value(I)));bSynchronizing=false;}
    void FocusError()
    {if(Edit.ErrorField>=0&&Edit.ErrorField<6)if(auto Input=Inputs[Edit.ErrorField].Pin())FSlateApplication::Get().SetKeyboardFocus(Input,EFocusCause::SetDirectly);}
};
struct FStudioBoundaryWorkspaceState
{
    FGuid Project,Selected,Presented;
    int64 Revision=-1;
    TArray<FStudioBoundaryTarget> Targets;
    FString Filter,PresentedFilter;
    int32 Offset=0,PresentedOffset=INDEX_NONE,Matches=0;
    FStudioBoundaryCoverage Coverage;
    TMap<FGuid,TSharedPtr<FStudioBoundaryForm>> Drafts;
    TMap<FGuid,TSharedPtr<SWidget>> Editors;
    TSharedPtr<SVerticalBox> Rows;
    TSharedPtr<SBox> Details;
    FString Notice;
    bool bSelectSurfaces=true;
    bool HasDrafts() const
    {for(const auto& Pair:Drafts)if(Pair.Value->Edit.IsDirty()||Pair.Value->bConflict||Pair.Value->bMissing)return true;return false;}
};

DECLARE_DELEGATE_OneParam(FOnBoundaryTargetSelected,const FGuid&);
class SBoundaryViewport final : public SFlowViewport
{
public:
    SLATE_BEGIN_ARGS(SBoundaryViewport){}
        SLATE_ARGUMENT(AStudioScene*,Scene)
        SLATE_ARGUMENT(TSharedPtr<FStudioBoundaryWorkspaceState>,Form)
        SLATE_EVENT(FOnBoundaryTargetSelected,OnSelect)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        Form=Args._Form;OnSelect=Args._OnSelect;
        SFlowViewport::Construct(SFlowViewport::FArguments().Scene(Args._Scene));
    }
    bool Current() const
    {
        return Scene.IsValid()&&Scene->Model&&Form&&Form->Project==Scene->Model->Project.Id&&
            Scene->Model->Workspace==EStudioWorkspace::BoundaryConditions&&Scene->HasBoundaryPreview()&&
            !Scene->Model->IsProjectOpenPending()&&!Scene->Model->IsRecordingLoadPending()&&
            StudioView::CameraEquals(Scene->PresentedCamera(),Scene->CameraState());
    }
    FStudioCameraState SelectionCamera() const
    {
        auto Camera=Scene->PresentedCamera();
        if(!Camera.bDepthClipping)
        {
            Camera.bDepthClipping=true;Camera.NearClipMeters=FMath::Max(1.e-6,Scene->PresentedNearClipMeters());
            Camera.FarClipMeters=1.e8;
        }
        return Camera;
    }
    virtual int32 OnPaint(const FPaintArgs& Args,const FGeometry& G,const FSlateRect& Cull,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle& Style,bool Enabled) const override
    {
        Layer=SFlowViewport::OnPaint(Args,G,Cull,Out,Layer,Style,Enabled);
        if(!Current()||!Form->bSelectSurfaces)return Layer;
        const auto Handles=StudioBoundarySelection::FaceHandles(Scene->Model->Project.Draft.Domain,SelectionCamera(),G.GetLocalSize(),PlacementProjectionAspect());
        for(const auto& Handle:Handles)
        {
            const FVector2D P=Handle.Pixel;const bool Active=Handle.Target==Form->Selected;const auto Color=Active?Cyan:Muted;
            FSlateDrawElement::MakeBox(Out,Layer+1,G.ToPaintGeometry(FVector2D(10,10),FSlateLayoutTransform(P-FVector2D(5,5))),&White,ESlateDrawEffect::None,Panel);
            FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),TArray<FVector2D>{P+FVector2D(-5,-5),P+FVector2D(5,-5),P+FVector2D(5,5),P+FVector2D(-5,5),P+FVector2D(-5,-5)},ESlateDrawEffect::None,Color,true,Active?2.f:1.f);
            FSlateDrawElement::MakeText(Out,Layer+2,G.ToPaintGeometry(FVector2D(24,14),FSlateLayoutTransform(P+FVector2D(8,-7))),
                FString::Printf(TEXT("%c%c"),Handle.Face%2?TEXT('+'):TEXT('-'),TEXT("XYZ")[Handle.Face/2]),Font(9,true),ESlateDrawEffect::None,Color);
        }
        return Layer+2;
    }
    virtual void Tick(const FGeometry& G,double T,float D) override
    {
        SFlowViewport::Tick(G,T,D);
        if(Form&&bPaintedSelect!=Form->bSelectSurfaces){bPaintedSelect=Form->bSelectSurfaces;Invalidate(EInvalidateWidgetReason::Paint);}
    }
    virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& Event) override
    {
        if(!Form||!Form->bSelectSurfaces||Event.GetEffectingButton()!=EKeys::LeftMouseButton)
            return SFlowViewport::OnMouseButtonDown(G,Event);
        if(HasOwnCapture()&&!OwnsPointer(Event))return FReply::Unhandled();
        ReleaseOwnCapture();FSlateApplication::Get().SetUserFocus(Event.GetUserIndex(),SharedThis(this),EFocusCause::Mouse);
        if(!Current()){Form->Notice=TEXT("Wait for the current geometry view before selecting a boundary.");return FReply::Handled();}
        const auto Pixel=G.AbsoluteToLocal(Event.GetScreenSpacePosition());const auto Observer=SelectionCamera();const double Aspect=PlacementProjectionAspect();
        FGuid Target=StudioBoundarySelection::HitFaceHandle(StudioBoundarySelection::FaceHandles(Scene->Model->Project.Draft.Domain,Observer,G.GetLocalSize(),Aspect),Pixel);
        if(!Target.IsValid()&&Scene->Model->DomainGeometry)
        {
            StudioBoundarySelection::FPatchHit Hit;
            if(StudioBoundarySelection::PickPatch(*Scene->Model->DomainGeometry,Observer,G.GetLocalSize(),Pixel,Hit,Aspect))Target=Hit.Target;
        }
        FStudioBoundaryTarget CurrentTarget;
        if(Target.IsValid()&&StudioBoundaries::FindTarget(Scene->Model->Project.Draft,Target,CurrentTarget))
        {
            OnSelect.ExecuteIfBound(Target);Form->Notice=TEXT("Selected ")+CurrentTarget.Name+TEXT(". Choose and apply its boundary condition.");
        }
        else Form->Notice=TEXT("No visible surface here. Click a domain handle or an imported surface, or choose a target from the list.");
        Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();
    }
private:
    TSharedPtr<FStudioBoundaryWorkspaceState> Form;
    FOnBoundaryTargetSelected OnSelect;
    bool bPaintedSelect=true;
};

// THESIS: Assign known physical conditions to stable case faces and source patches.
// OWN-WORLD: Existing native Slate shell, compact SI inputs and cyan selection.
// STORY: Select a target, choose a condition, apply values, review missing coverage.
// FIRST VIEWPORT: Targets left, verified geometry center, retained physical editor and coverage right.
// FORM: Boundary Conditions sidebar owns editing; Solve shows saved summaries.
// FINISH: Native two-size workflow evidence, scoped review and documentation required.
TSharedRef<SWidget> SStudioWorkspace::BoundaryWorkspace()
{
    BoundaryState=MakeShared<FStudioBoundaryWorkspaceState>();const auto State=BoundaryState;
    auto Available=[this]{return !M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();};
    auto Undo=Button(TEXT("Undo case"),TEXT("undo"),[this]{M->UndoCase();RefreshBoundaries();});Undo->SetTag(TEXT("BoundaryUndo"));
    Undo->SetEnabled(TAttribute<bool>::CreateLambda([this,State,Available]{return Available()&&!State->HasDrafts()&&M->CanUndoCase();}));
    auto Redo=Button(TEXT("Redo case"),TEXT("redo"),[this]{M->RedoCase();RefreshBoundaries();});Redo->SetTag(TEXT("BoundaryRedo"));
    Redo->SetEnabled(TAttribute<bool>::CreateLambda([this,State,Available]{return Available()&&!State->HasDrafts()&&M->CanRedoCase();}));
    auto Check=Button(TEXT("Check geometry"),TEXT("check"),[this]{M->RequestDomainGeometry();});Check->SetTag(TEXT("BoundaryCheckGeometry"));
    Check->SetEnabled(TAttribute<bool>::CreateLambda([this,Available]{return Available()&&!M->IsReadingDomainGeometry();}));
    auto Cancel=Button(TEXT("Cancel check"),TEXT("stop"),[this]{M->CancelDomainGeometry();});Cancel->SetTag(TEXT("BoundaryCancelCheck"));
    Cancel->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->IsReadingDomainGeometry();}));
    auto Previous=Button(TEXT("Previous"),TEXT("left"),[this]{BoundaryState->Offset=FMath::Max(0,BoundaryState->Offset-128);RefreshBoundaries();});Previous->SetTag(TEXT("BoundaryPreviousPage"));
    Previous->SetEnabled(TAttribute<bool>::CreateLambda([State]{return State->Offset>0;}));
    auto Next=Button(TEXT("Next"),TEXT("right"),[this]{BoundaryState->Offset+=128;RefreshBoundaries();});Next->SetTag(TEXT("BoundaryNextPage"));
    Next->SetEnabled(TAttribute<bool>::CreateLambda([State]{return State->Offset+State->Targets.Num()<State->Matches;}));
    const auto Targets=SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Faces and patches"),11,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SProjectFilterBox).Tag(TEXT("BoundaryFilter")).Style(&InputStyle()).Font(Font(10))
            .HintText(FText::FromString(TEXT("Filter by name"))).ToolTipText(FText::FromString(TEXT("Filter faces and patches by name")))
            .OnTextChanged_Lambda([this](const FText& Value){BoundaryState->Filter=Value.ToString();BoundaryState->Offset=0;RefreshBoundaries();})]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SRetainedFormScrollBox).Tag(TEXT("BoundaryTargetsScroll")).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
            +SScrollBox::Slot()[SAssignNew(State->Rows,SVerticalBox)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,6)[Live([State]
        {return State->Matches?FString::Printf(TEXT("%d–%d of %d matching targets"),State->Offset+1,State->Offset+State->Targets.Num(),State->Matches):FString(TEXT("No matching targets"));},9,Muted,true)]
        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,4,0)[Previous]+SHorizontalBox::Slot().FillWidth(1)[Next]];
    auto Mode=[State](bool Select,const FString& Name)
    {
        return SNew(SButton).Tag(Select?TEXT("BoundarySelectMode"):TEXT("BoundaryOrbitMode")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
            .OnClicked_Lambda([State,Select]{State->bSelectSurfaces=Select;return FReply::Handled();})
            [SNew(STextBlock).Text(FText::FromString(Name)).Font(Font(10)).ColorAndOpacity_Lambda([State,Select]{return State->bSelectSurfaces==Select?Cyan:Muted;})];
    };
    auto Fit=Button(TEXT("Fit"),TEXT("fit"),[this]{if(Scene.IsValid()&&Scene->HasBoundaryPreview())Scene->FitCamera();});Fit->SetTag(TEXT("BoundaryFitView"));
    Fit->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Scene.IsValid()&&Scene->HasBoundaryPreview();}));
    auto Projection=SNew(SButton).Tag(TEXT("BoundaryProjection")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
        .IsEnabled_Lambda([this]{return Scene.IsValid()&&Scene->HasBoundaryPreview();})
        .ToolTipText_Lambda([this]{return FText::FromString(Scene.IsValid()&&Scene->CameraState().bOrthographic?TEXT("Switch the authoring camera to perspective"):TEXT("Switch the authoring camera to orthographic"));})
        .OnClicked_Lambda([this]
        {auto Camera=Scene->CameraState();Camera.bOrthographic=!Camera.bOrthographic;Scene->RestoreCamera(Camera,TEXT("Authoring projection"));return FReply::Handled();})
        [Live([this]{return Scene.IsValid()&&Scene->CameraState().bOrthographic?TEXT("Orthographic"):TEXT("Perspective");},10,Text)];
    const auto View=SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4))
            +SWrapBox::Slot()[Mode(true,TEXT("Select"))]+SWrapBox::Slot()[Mode(false,TEXT("Orbit"))]+SWrapBox::Slot()[Fit]+SWrapBox::Slot()[Projection]]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SBorder).BorderImage(&LineBrush).Padding(1)[SNew(SOverlay)
            +SOverlay::Slot()[SNew(SBoundaryViewport).Tag(TEXT("BoundaryViewport")).Scene(Scene.Get()).Form(State)
                .OnSelect_Lambda([this](const FGuid& Id){BoundaryState->Selected=Id;RefreshBoundaries();})]
            +SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(12)[SNew(SOrientationAxes).Scene(Scene.Get()).Visibility(EVisibility::HitTestInvisible)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,6)[Live([State]
        {return State->bSelectSurfaces?TEXT("Click a surface or face handle · Middle-drag: pan · Scroll: zoom"):TEXT("Drag: orbit · Right-drag: look · WASD + Q/E: fly while looking");},9,Muted,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Live([this,State]
        {
            if(M->IsReadingDomainGeometry())return FString(TEXT("Checking original geometry…"));
            if(!M->DomainGeometry)return M->DomainNotice;
            if(M->Project.Draft.Geometry.IsEmpty())return FString(TEXT("Domain faces are available. Import case geometry to assign surface conditions."));
            if(M->DomainGeometry->bPreviewLimited)return FString(TEXT("Some objects exceed the preview limit. Their patches remain available in the list."));
            FStudioBoundaryTarget Selected;
            if(StudioBoundaries::FindTarget(M->Project.Draft,State->Selected,Selected)&&Selected.DomainFace==INDEX_NONE&&
                !M->DomainGeometry->PatchBounds.Contains(Selected.Id))return FString(TEXT("This patch has no verified triangles in the preview. Check its original geometry in Geometry."));
            return M->DomainGeometry->Complete()?FString(TEXT("Original surface geometry · Selected boundary highlighted in cyan")):M->DomainNotice;
        },10,Muted,true)];
    const auto Inspector=SNew(SRetainedFormScrollBox).Tag(TEXT("BoundaryDetailsScroll"))
        .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(12)
        +SScrollBox::Slot()[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SAssignNew(State->Details,SBox)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,20,0,8)[Label(TEXT("Applied coverage"),12,Text,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[Live([State]{return FString::Printf(TEXT("%d / %d targets configured"),State->Coverage.Configured,State->Coverage.Targets);},11,Text,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[Live([State]
            {
                if(!State->Coverage.IssueCount)return FString(TEXT("All required boundary values are supplied."));
                FString Summary;const int32 Limit=FMath::Min(5,State->Coverage.Issues.Num());
                for(int32 I=0;I<Limit;++I){if(I)Summary+=TEXT("\n\n");Summary+=State->Coverage.Issues[I].Message;}
                if(Limit<State->Coverage.IssueCount)Summary+=FString::Printf(TEXT("\n\n%d additional issues need attention."),State->Coverage.IssueCount-Limit);
                return Summary;
            },10,Amber,true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Solver compatibility"),12,Text,true)]
            +SVerticalBox::Slot().AutoHeight()[Live([]{return TEXT("Not verified. The solver must provide supported boundary and thermal models before this case can be accepted for a run.");},10,Muted,true)]];
    return SNew(SBorder).Tag(TEXT("BoundaryWorkspace")).BorderImage(&PanelBrush).Padding(14)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Boundary Conditions"),18,Text,true)]
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Check]+SHorizontalBox::Slot().AutoWidth().Padding(0,0,12,0)[Cancel]
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Undo]+SHorizontalBox::Slot().AutoWidth()[Redo]]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,12,0)[SNew(SBox).WidthOverride(196)[Targets]]
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,12,0)[View]
                +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(310)[Inspector]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0)[Live([State]{return State->Notice.IsEmpty()?TEXT("Values use SI units. Blank values remain unknown; incomplete cases can be saved."):State->Notice;},10,Amber,true)]];
}
TSharedRef<SWidget> SStudioWorkspace::BoundaryDetails(const FGuid& Target)
{
    const auto State=BoundaryState;
    if(const auto* Cached=State->Editors.Find(Target))return Cached->ToSharedRef();
    const auto Form=State->Drafts.FindChecked(Target);const FGuid Project=M->Project.Id;
    auto Current=[this,Project,Form]{return M->Project.Id==Project&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending()&&!Form->bConflict&&!Form->bMissing;};
    auto Copy=[](TFunction<FString()> Read,FLinearColor Color=Muted)
    {return SNew(STextBlock).Text_Lambda([Read]{return FText::FromString(Read());}).Font(Font(10)).ColorAndOpacity(Color).AutoWrapText(true);};
    auto Input=[this,Target,Form](int32 I,const FString& Hint)
    {
        auto Widget=SNew(SProjectFilterBox).Tag(FName(*FString::Printf(TEXT("BoundaryValue%d"),I))).Style(&InputStyle()).Font(Font(10))
            .Text(FText::FromString(Form->Value(I))).HintText(FText::FromString(Hint)).ToolTipText(FText::FromString(Hint))
            .OnTextChanged_Lambda([Form,I](const FText& ValueText){if(!Form->bSynchronizing)Form->Value(I)=ValueText.ToString();})
            .OnTextCommitted_Lambda([this,Target](const FText&,ETextCommit::Type Type){if(Type==ETextCommit::OnEnter)ApplyBoundary(Target,false);});
        Form->Inputs[I]=Widget;return Widget;
    };
    auto Type=SNew(SStudioMenuButton).Tag(TEXT("BoundaryType")).ButtonStyle(&ButtonStyle())
        .OnGetMenuContent_Lambda([this,Form,Current]
        {
            auto Menu=SNew(SVerticalBox);
            for(int32 I=0;I<=int32(EStudioBoundaryType::Periodic);++I)
            {
                const auto Kind=EStudioBoundaryType(I);
                auto Choice=Button(StudioBoundaries::TypeName(Kind),TEXT("settings"),[Form,Kind]
                {Form->Edit.Type=Kind;Form->Edit.Error.Empty();FSlateApplication::Get().DismissAllMenus();});
                Choice->SetTag(FName(*FString::Printf(TEXT("BoundaryType%d"),I)));
                Choice->SetEnabled(Current()&&(Kind!=EStudioBoundaryType::Periodic||Form->Target.DomainFace!=INDEX_NONE));
                Menu->AddSlot().AutoHeight()[Choice];
            }
            return SNew(SBorder).BorderImage(&PanelBrush).Padding(6)[Menu];
        }).ButtonContent()[Live([Form]{return StudioBoundaries::TypeName(Form->Edit.Type);},10,Text)];
    auto Fields=SNew(SVerticalBox);
    Fields->AddSlot().AutoHeight().Padding(0,0,0,7)[Copy([Form]{return Form->Target.Name;},Text)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,14)[Copy([Form]{return Form->Target.DomainFace==INDEX_NONE?TEXT("Imported surface patch"):TEXT("Domain face");})];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Condition name"),10,Muted)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,12)[Input(0,TEXT("Name · 1–120 characters"))];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Condition"),10,Muted)];
    Fields->AddSlot().AutoHeight().Padding(0,0,0,14)[Type];
    auto Velocity=SNew(SVerticalBox).Visibility_Lambda([Form]{return Form->Edit.Type==EStudioBoundaryType::VelocityInlet?EVisibility::Visible:EVisibility::Collapsed;});
    Velocity->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Velocity · m/s"),11,Text,true)];
    for(int32 I=0;I<3;++I)Velocity->AddSlot().AutoHeight().Padding(0,0,0,6)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(23)[Label(FString::Chr(TEXT("XYZ")[I]),10,Muted)]]
        +SHorizontalBox::Slot().FillWidth(1)[Input(1+I,TEXT("Unknown"))]];
    Velocity->AddSlot().AutoHeight().Padding(0,0,0,12)[Copy([]{return TEXT("Enter every component, including zeros. Leave all three blank if velocity is unknown.");})];
    Fields->AddSlot().AutoHeight()[Velocity];
    Fields->AddSlot().AutoHeight()[SNew(SVerticalBox).Visibility_Lambda([Form]{return Form->Edit.Type==EStudioBoundaryType::PressureOutlet?EVisibility::Visible:EVisibility::Collapsed;})
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Outlet pressure · Pa"),11,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Input(4,TEXT("Unknown"))]];
    Fields->AddSlot().AutoHeight()[SNew(SVerticalBox).Visibility_Lambda([Form]{return Form->Edit.Type!=EStudioBoundaryType::Periodic&&Form->Edit.Type!=EStudioBoundaryType::Unassigned?EVisibility::Visible:EVisibility::Collapsed;})
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Prescribed temperature · K"),11,Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Input(5,TEXT("Optional · unknown"))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[Copy([]{return TEXT("A prescribed temperature requires an enabled thermal model and supporting solver.");})]];
    Fields->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([Form]{return Form->Edit.Type==EStudioBoundaryType::Periodic?EVisibility::Visible:EVisibility::Collapsed;})
        [Copy([this,Form]
        {
            const int32 Index=Form->Target.DomainFace;
            if(!M->Project.Draft.Domain.FaceNames.IsValidIndex(Index^1))return FString(TEXT("This target cannot be paired automatically."));
            const FString Partner=M->Project.Draft.Domain.FaceNames[Index^1];
            return Form->Edit.bHadAssignment&&Form->Edit.Saved.Type==EStudioBoundaryType::Periodic?
                TEXT("Paired with ")+Partner+TEXT(". Removing this assignment also removes its partner."):
                TEXT("Pairs with ")+Partner+TEXT(". Both assignments are applied together. The opposite face must be unassigned.");
        })]];
    auto NeedsUnpair=[Form]{return Form->Edit.bHadAssignment&&Form->Edit.Saved.Type==EStudioBoundaryType::Periodic&&Form->Edit.Type!=EStudioBoundaryType::Periodic;};
    auto Apply=Button(TEXT("Apply condition"),TEXT("check"),[this,Target]{ApplyBoundary(Target,false);},Cyan);Apply->SetTag(TEXT("BoundaryApply"));
    Apply->SetEnabled(TAttribute<bool>::CreateLambda([Current,Form,NeedsUnpair]{return Current()&&!NeedsUnpair()&&Form->Edit.IsDirty();}));
    auto Unpair=Button(TEXT("Unpair and apply"),TEXT("check"),[this,Target]{ApplyBoundary(Target,true);},Amber);Unpair->SetTag(TEXT("BoundaryUnpairApply"));
    Unpair->SetVisibility(TAttribute<EVisibility>::CreateLambda([NeedsUnpair]{return NeedsUnpair()?EVisibility::Visible:EVisibility::Collapsed;}));
    Unpair->SetEnabled(TAttribute<bool>::CreateLambda(Current));
    auto Revert=Button(TEXT("Revert edits"),TEXT("undo"),[this,Target,Form]
    {
        const auto State=BoundaryState;
        if(Form->bMissing){State->Drafts.Remove(Target);State->Editors.Remove(Target);State->Selected.Invalidate();}
        else
        {
            const auto* Saved=M->Project.Draft.Boundaries.FindByPredicate([Target](const auto& B){return B.TargetId==Target;});
            Form->Edit.Reset(Form->Target,Saved);Form->bConflict=false;Form->Synchronize();
        }
        State->Revision=-1;State->Notice=TEXT("Unapplied edits reverted.");ResolveSaveNotice(*M,BoundarySaveGuard,State->Notice);RefreshBoundaries();
    });Revert->SetTag(TEXT("BoundaryRevert"));
    Revert->SetEnabled(TAttribute<bool>::CreateLambda([Form,this,Project]{return M->Project.Id==Project&&!M->IsProjectOpenPending()&&(Form->Edit.IsDirty()||Form->bConflict||Form->bMissing||!Form->Edit.Error.IsEmpty());}));
    auto Remove=Button(TEXT("Remove assignment"),TEXT("stop"),[this,Target,Form]
    {if(!Form->Edit.IsDirty()&&M->RemoveBoundary(Target)){BoundaryState->Notice=TEXT("Assignment removed. Removing a periodic condition also removes its partner.");RefreshBoundaries();}else BoundaryState->Notice=M->Notice;});
    Remove->SetTag(TEXT("BoundaryRemove"));Remove->SetEnabled(TAttribute<bool>::CreateLambda([Current,Form]{return Current()&&Form->Edit.bHadAssignment&&!Form->Edit.IsDirty();}));
    auto Editor=SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[SNew(SBox).IsEnabled_Lambda(Current)[Fields]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,6)[Copy([Form]{return Form->bMissing?TEXT("This target was removed while you were editing. Revert to discard its retained draft."):Form->bConflict?TEXT("The saved assignment changed. Your edits are retained; revert to load its current values."):Form->Edit.Error;},Amber)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6,6))
            +SWrapBox::Slot()[Apply]+SWrapBox::Slot()[Unpair]+SWrapBox::Slot()[Revert]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SBox).Visibility_Lambda([NeedsUnpair]{return NeedsUnpair()?EVisibility::Visible:EVisibility::Collapsed;})
            [Copy([]{return TEXT("Applying this change also removes the opposite face's periodic assignment.");},Amber)]]
        +SVerticalBox::Slot().AutoHeight()[Remove];
    State->Editors.Add(Target,Editor);return Editor;
}
void SStudioWorkspace::RefreshBoundaries()
{
    if(!BoundaryState||!BoundaryState->Rows||!BoundaryState->Details)return;auto& State=*BoundaryState;
    const bool CaseChanged=State.Project!=M->Project.Id||State.Revision!=M->Project.Draft.Revision;
    if(!CaseChanged&&State.Presented==State.Selected&&State.Filter==State.PresentedFilter&&State.Offset==State.PresentedOffset)return;
    if(State.Project!=M->Project.Id)
    {
        FSlateApplication::Get().DismissAllMenus();State.Project=M->Project.Id;State.Selected.Invalidate();
        State.Drafts.Empty();State.Editors.Empty();State.Notice.Empty();State.Offset=0;
    }
    const auto& Case=M->Project.Draft;
    if(CaseChanged)State.Coverage=StudioBoundaries::Analyze(Case);
    TMap<FGuid,const FStudioBoundaryCondition*> Assignments;
    for(const auto& Assignment:Case.Boundaries)Assignments.Add(Assignment.TargetId,&Assignment);
    // Keep only visited drafts. A large imported patch catalog must not allocate
    // one editable form or Slate subtree per target.
    for(auto It=State.Drafts.CreateIterator();It;++It)
    {
        auto& Form=*It.Value();FStudioBoundaryTarget Target;
        if(!StudioBoundaries::FindTarget(Case,It.Key(),Target))
        {
            if(Form.Edit.IsDirty()||Form.bConflict){Form.bMissing=true;Form.bConflict=true;}
            else {State.Editors.Remove(It.Key());It.RemoveCurrent();}
            continue;
        }
        const auto* Assignment=Assignments.FindRef(It.Key());
        if(!Form.Edit.Matches(Assignment))
        {
            if(Form.Edit.IsDirty())Form.bConflict=true;
            else {Form.Edit.Reset(Target,Assignment);Form.bConflict=false;Form.Synchronize();}
        }
        else Form.bConflict=false;
        Form.Target=Target;Form.bMissing=false;
        if(It.Key()!=State.Selected&&!Form.Edit.IsDirty()&&!Form.bConflict)
        {State.Editors.Remove(It.Key());It.RemoveCurrent();}
    }
    FStudioBoundaryTarget Selected;
    bool SelectedExists=StudioBoundaries::FindTarget(Case,State.Selected,Selected);
    if(!SelectedExists&&!State.Drafts.Contains(State.Selected))
    {
        const auto First=StudioBoundaries::TargetPage(Case,FString(),0,1);
        State.Selected=First.Items.IsEmpty()?FGuid():First.Items[0].Id;
        SelectedExists=StudioBoundaries::FindTarget(Case,State.Selected,Selected);
    }
    if(SelectedExists)
    {
        if(!State.Drafts.Contains(State.Selected))
        {
            const auto Form=MakeShared<FStudioBoundaryForm>();Form->Target=Selected;
            Form->Edit.Reset(Selected,Assignments.FindRef(State.Selected));State.Drafts.Add(State.Selected,Form);
        }
        M->SelectBoundaryTarget(State.Selected);
    }
    else if(M->SelectedBoundaryTarget.IsValid())
    {M->SelectedBoundaryTarget.Invalidate();++M->DomainPreviewRevision;}
    auto Page=StudioBoundaries::TargetPage(Case,State.Filter,State.Offset);
    if(State.Offset>0&&State.Offset>=Page.Matches)
    {State.Offset=Page.Matches?((Page.Matches-1)/128)*128:0;Page=StudioBoundaries::TargetPage(Case,State.Filter,State.Offset);}
    State.Matches=Page.Matches;State.Targets=MoveTemp(Page.Items);
    const auto Focused=FSlateApplication::Get().GetKeyboardFocusedWidget();const FName FocusTag=Focused?Focused->GetTag():NAME_None;
    State.Rows->ClearChildren();TSharedPtr<SWidget> Restore;
    auto AddRow=[this,&State,&Assignments,FocusTag,&Restore](const FStudioBoundaryTarget& Target)
    {
        const FGuid Id=Target.Id;const auto* Assignment=Assignments.FindRef(Id);
        const auto SavedType=Assignment?Assignment->Type:EStudioBoundaryType::Unassigned;
        const auto Row=SNew(SButton).Tag(FName(*(TEXT("BoundaryTarget_")+Id.ToString()))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(10,9))
            .OnClicked_Lambda([this,Id]{BoundaryState->Selected=Id;RefreshBoundaries();return FReply::Handled();})
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,4)[SNew(STextBlock).Text(FText::FromString(Target.Name)).Font(Font(10,true))
                    .AutoWrapText(true).ColorAndOpacity_Lambda([this,Id]{return BoundaryState->Selected==Id?Cyan:Text;})]
                +SVerticalBox::Slot().AutoHeight()[Live([this,Id,SavedType]
                {
                    const auto Form=BoundaryState->Drafts.FindRef(Id);
                    if(Form&&Form->bMissing)return FString(TEXT("Removed target · retained draft"));
                    return StudioBoundaries::TypeName(SavedType)+(Form&&Form->Edit.IsDirty()?TEXT(" · editing"):TEXT(""));
                },9,Muted,true)]];
        State.Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Row];if(Row->GetTag()==FocusTag)Restore=Row;
    };
    // A selected draft stays reachable when its name is filtered out or its
    // original target is removed. Other retained drafts are visited by save guards.
    if(const auto Form=State.Drafts.FindRef(State.Selected);Form&&!State.Targets.ContainsByPredicate([&](const auto& T){return T.Id==State.Selected;}))
    {
        State.Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Selected target"),10,Muted)];AddRow(Form->Target);
        State.Rows->AddSlot().AutoHeight().Padding(0,4,0,8)[Label(TEXT("Matching targets"),10,Muted)];
    }
    for(const auto& Target:State.Targets)AddRow(Target);
    State.Revision=Case.Revision;State.Presented=State.Selected;State.PresentedFilter=State.Filter;State.PresentedOffset=State.Offset;
    if(State.Selected.IsValid())State.Details->SetContent(BoundaryDetails(State.Selected));
    else State.Details->SetContent(Label(TEXT("No boundary targets are available."),12,Muted));
    if(Restore)FSlateApplication::Get().SetKeyboardFocus(Restore,EFocusCause::Navigation);
}
void SStudioWorkspace::ApplyBoundary(const FGuid& Target,bool bUnpair)
{
    RefreshBoundaries();const auto Form=BoundaryState->Drafts.FindRef(Target);
    if(!Form||Form->bConflict||Form->bMissing)return;
    FStudioBoundaryCondition Candidate;
    if(!Form->Edit.Build(M->Project.Draft,Candidate)){BoundaryState->Notice=Form->Edit.Error;Form->FocusError();return;}
    if(!M->UpdateBoundary(Candidate,bUnpair)){BoundaryState->Notice=M->Notice;return;}
    Form->Edit.Reset(Form->Target,&Candidate);Form->Synchronize();BoundaryState->Revision=-1;
    BoundaryState->Notice=TEXT("Condition applied to the case. Save to keep this assignment.");
    ResolveSaveNotice(*M,BoundarySaveGuard,BoundaryState->Notice);RefreshBoundaries();
}
bool SStudioWorkspace::EnsureBoundariesResolved()
{
    RefreshBoundaries();if(!BoundaryState)return true;
    for(const auto& Pair:BoundaryState->Drafts)if(Pair.Value->Edit.IsDirty()||Pair.Value->bConflict||Pair.Value->bMissing)
    {
        const auto Form=Pair.Value;
        BoundaryState->Selected=Pair.Key;BoundaryState->Notice=BoundarySaveGuard;
        M->Notice=BoundaryState->Notice;Navigate(EStudioWorkspace::BoundaryConditions);RefreshBoundaries();
        if(auto Input=Form->Inputs[0].Pin())FSlateApplication::Get().SetKeyboardFocus(Input,EFocusCause::SetDirectly);
        return false;
    }
    return true;
}
