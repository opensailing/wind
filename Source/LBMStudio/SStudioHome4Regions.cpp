#include "SStudioHome4Regions.h"
#include "StudioTheme.h"
#include "StudioColor.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Framework/Application/SlateApplication.h"
namespace StudioHome4RegionWidgetLocal
{
FString Vector(const FVector& V){return FString::Printf(TEXT("%.17g, %.17g, %.17g"),V.X,V.Y,V.Z);}
bool Vector(const FString& Text,FVector& V)
{TArray<FString> Parts;Text.ParseIntoArray(Parts,TEXT(","),false);if(Parts.Num()!=3)return false;for(int32 I=0;I<3;++I)if(!StudioColor::ParseNumber(Parts[I],V[I]))return false;return true;}
}
void SStudioHome4Regions::Construct(const FArguments& A)
{
    Session=A._Session;bPatches=A._Patches;Pending=Session->Pending(PendingKey());if(const auto* V=Pending.Find(TEXT("_selected")))Selected=FCString::Atoi(**V);SetCanTick(true);auto Content=SNew(SVerticalBox);
    Content->AddSlot().AutoHeight().Padding(0,12)[StudioUI::Label(bPatches?TEXT("Authored MD patches"):TEXT("Authored 3D zones"),14,StudioUI::Text,true)];
    auto Buttons=SNew(SHorizontalBox);auto Action=[&](const FString& T,const FString& K,TFunction<void()> Fn){Buttons->AddSlot().AutoWidth().Padding(0,0,6,0)[SNew(SButton).Tag(FName(*(TEXT("Home4Region.")+K))).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([Fn]{Fn();return FReply::Handled();})[StudioUI::Label(T,10)]];};
    Action(TEXT("Add"),bPatches?TEXT("addPatch"):TEXT("addZone"),[this]{Add();});Action(TEXT("Previous"),TEXT("previous"),[this]{if(Pending.IsEmpty()){Selected=FMath::Max(0,Selected-1);Refresh();}else Session->Status=TEXT("Commit this region's pending edits before switching.");});Action(TEXT("Next"),TEXT("next"),[this]{if(Pending.IsEmpty()){++Selected;Refresh();}else Session->Status=TEXT("Commit this region's pending edits before switching.");});
    Action(TEXT("Retain region"),TEXT("retain"),[this]{Commit();});Action(TEXT("Revert region text"),TEXT("revert"),[this]{Pending.Reset();KeepPending();Refresh();});Action(TEXT("Remove"),TEXT("remove"),[this]{Remove();});Content->AddSlot().AutoHeight()[Buttons];
    Content->AddSlot().AutoHeight().Padding(0,10)[SAssignNew(Rows,SVerticalBox)];ChildSlot[Content];Refresh();
}
void SStudioHome4Regions::KeepPending()
{if(!Pending.IsEmpty())Pending.Add(TEXT("_selected"),LexToString(Selected));Session->RetainPending(PendingKey(),Pending);}
void SStudioHome4Regions::Tick(const FGeometry&,double,float)
{if(Session->Pending(PendingKey()).IsEmpty()&&!Pending.IsEmpty()){Pending.Reset();Refresh();}FStudioHome4Spec S;FString E;if(Session->Build(S,E,true)){const int32 Count=bPatches?S.Authoring.Patches.Num():S.Authoring.Zones.Num();if(Count!=ShownCount&&Pending.IsEmpty())Refresh();}}
void SStudioHome4Regions::Refresh()
{
    using namespace StudioHome4RegionWidgetLocal;Rows->ClearChildren();FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return;ShownCount=bPatches?S.Authoring.Patches.Num():S.Authoring.Zones.Num();Selected=FMath::Clamp(Selected,0,FMath::Max(0,ShownCount-1));
    if(!ShownCount){Rows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Add an explicit region to author its bounds and preview."),10,StudioUI::Muted)];return;}
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[StudioUI::Label(FString::Printf(TEXT("Region %d of %d"),Selected+1,ShownCount),10,StudioUI::Muted)];
    auto Field=[&](const FString& Key,const FString& Label,const FString& Value,const TArray<FString>& Choices=TArray<FString>())
    {
        TSharedPtr<SWidget> Input;
        if(Choices.IsEmpty())Input=SNew(SEditableTextBox).Tag(FName(*(TEXT("Home4Region.")+Key))).Style(&StudioUI::InputStyle()).Font(StudioUI::Font(10)).Text(FText::FromString(Pending.Contains(Key)?Pending[Key]:Value)).OnTextChanged_Lambda([this,Key](const FText& T){Pending.Add(Key,T.ToString());KeepPending();});
        else Input=SNew(SComboButton).Tag(FName(*(TEXT("Home4Region.")+Key))).ButtonStyle(&StudioUI::ButtonStyle()).OnGetMenuContent_Lambda([this,Key,Choices]{auto Menu=SNew(SVerticalBox);for(const auto& C:Choices)Menu->AddSlot().AutoHeight()[SNew(SButton).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this,Key,C]{Pending.Add(Key,C);KeepPending();FSlateApplication::Get().DismissAllMenus();Refresh();return FReply::Handled();})[StudioUI::Label(C,10)]];return StaticCastSharedRef<SWidget>(Menu);}).ButtonContent()[SNew(STextBlock).Font(StudioUI::Font(10)).Text_Lambda([this,Key,Value]{return FText::FromString(Pending.Contains(Key)?Pending[Key]:Value);})];
        Rows->AddSlot().AutoHeight().Padding(0,4)[SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(165)[StudioUI::Label(Label,10,StudioUI::Muted)]]+SHorizontalBox::Slot().FillWidth(1)[Input.ToSharedRef()]];
    };
    if(bPatches){const auto& P=S.Authoring.Patches[Selected];Field(TEXT("id"),TEXT("Patch identity"),P.Id);Field(TEXT("body"),TEXT("Bound body identity"),P.BodyId);Field(TEXT("level"),TEXT("Level (root = 0)"),LexToString(P.Level));Field(TEXT("origin"),TEXT("Origin XYZ root cells"),Vector(P.Origin));Field(TEXT("extents"),TEXT("Extents local cells"),Vector(FVector(P.Extents)));Field(TEXT("follow"),TEXT("Follow declared body"),P.bFollowBody?TEXT("true"):TEXT("false"),{TEXT("false"),TEXT("true")});}
    else{const auto& Z=S.Authoring.Zones[Selected];Field(TEXT("id"),TEXT("Zone identity"),Z.Id);Field(TEXT("kind"),TEXT("Affected fields"),Z.Kind,{TEXT("sponge"),TEXT("beach"),TEXT("floor"),TEXT("wave-absorption")});Field(TEXT("profile"),TEXT("Strength profile"),Z.Profile,{TEXT("constant"),TEXT("linear"),TEXT("cubic")});Field(TEXT("axis"),TEXT("Profile direction"),Z.Axis,{TEXT("x"),TEXT("y"),TEXT("z")});Field(TEXT("minimum"),TEXT("Minimum XYZ"),Vector(Z.Minimum));Field(TEXT("maximum"),TEXT("Maximum XYZ"),Vector(Z.Maximum));Field(TEXT("strength"),TEXT("Root strength [0,1]"),LexToString(Z.Strength));Field(TEXT("exponent"),TEXT("Level exponent"),LexToString(Z.LevelExponent));}
    Rows->AddSlot().AutoHeight().Padding(0,8)[SNew(STextBlock).AutoWrapText(true).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).Text(FText::FromString(bPatches?TEXT("Spacing is 2^(−level) root cells. Origin is in root XYZ; extents count local cells. Follow-body uses the declared body's geometric kinematics. Parent Apply saves all retained requests."):TEXT("Coordinates use the explicitly chosen Zone units above. Sponge acts on pd/u; beach on u/phi while leaving pressure untouched; floor is friction; wave absorption is a declared relaxation region. Strength = s·profile·2^(−exponent·level).")))];
}
void SStudioHome4Regions::Add()
{
    if(!Pending.IsEmpty()){Session->Status=TEXT("Retain or revert this region's text before adding another.");return;}FStudioHome4Spec S;FString E;if(!Session->Build(S,E,true))return;
    if(bPatches){if(S.Authoring.Patches.Num()>=32)return;FStudioHome4AuthoredPatch P;P.Id=TEXT("patch-")+FGuid::NewGuid().ToString();P.BodyId=S.Authoring.BodyId;P.Origin=S.Geometry.InitialPositionCells.Get(FVector::ZeroVector);S.Authoring.Patches.Add(P);Selected=S.Authoring.Patches.Num()-1;}
    else{if(S.Authoring.Zones.Num()>=128)return;FStudioHome4AuthoredZone Z;Z.Id=TEXT("zone-")+FGuid::NewGuid().ToString();Z.Minimum=FVector::ZeroVector;Z.Maximum=FVector(S.Lattice.Extents.Get(FIntVector(16)));Z.Strength=.1;S.Authoring.Zones.Add(Z);S.Authoring.ZoneUnits=TEXT("root-cells");Selected=S.Authoring.Zones.Num()-1;}
    if(Session->Replace(S,E)){Session->Status=TEXT("Region added to the retained draft. Edit bounds/profile and Prepare to preview; Apply saves.");Refresh();}else Session->Status=E;
}
void SStudioHome4Regions::Remove()
{FStudioHome4Spec S;FString E;if(!Session->Build(S,E,true))return;if(bPatches){if(S.Authoring.Patches.IsValidIndex(Selected))S.Authoring.Patches.RemoveAt(Selected);}else if(S.Authoring.Zones.IsValidIndex(Selected))S.Authoring.Zones.RemoveAt(Selected);if(Session->Replace(S,E)){Pending.Reset();KeepPending();Refresh();}else Session->Status=E;}
bool SStudioHome4Regions::Commit()
{
    using namespace StudioHome4RegionWidgetLocal;FStudioHome4Spec S;FString E;if(!Session->Build(S,E,true))return false;
    bool Valid=true;auto Text=[&](const TCHAR* K,FString& V){if(const auto* P=Pending.Find(K))V=*P;};auto Num=[&](const TCHAR* K,double& V){if(const auto* P=Pending.Find(K))Valid&=StudioColor::ParseNumber(*P,V);};auto Vec=[&](const TCHAR* K,FVector& V){if(const auto* P=Pending.Find(K))Valid&=Vector(*P,V);};
    if(bPatches){if(!S.Authoring.Patches.IsValidIndex(Selected))return false;auto& P=S.Authoring.Patches[Selected];Text(TEXT("id"),P.Id);Text(TEXT("body"),P.BodyId);double Level=P.Level;Num(TEXT("level"),Level);Valid&=Level>=0&&Level<=15&&FMath::FloorToDouble(Level)==Level;if(Valid)P.Level=int32(Level);Vec(TEXT("origin"),P.Origin);FVector Extents(P.Extents);Vec(TEXT("extents"),Extents);for(int32 I=0;I<3;++I)Valid&=Extents[I]>=2&&Extents[I]<=1048576&&FMath::FloorToDouble(Extents[I])==Extents[I];if(Valid)P.Extents=FIntVector(int32(Extents.X),int32(Extents.Y),int32(Extents.Z));if(const auto* F=Pending.Find(TEXT("follow")))P.bFollowBody=*F==TEXT("true");}
    else{if(!S.Authoring.Zones.IsValidIndex(Selected))return false;auto& Z=S.Authoring.Zones[Selected];Text(TEXT("id"),Z.Id);Text(TEXT("kind"),Z.Kind);Text(TEXT("profile"),Z.Profile);Text(TEXT("axis"),Z.Axis);Vec(TEXT("minimum"),Z.Minimum);Vec(TEXT("maximum"),Z.Maximum);Num(TEXT("strength"),Z.Strength);Num(TEXT("exponent"),Z.LevelExponent);}
    if(!Valid){Session->Status=TEXT("Region has invalid finite coordinates/counts; the pending text and previous request were kept.");return false;}
    if(!Session->Replace(S,E)){Session->Status=E;return false;}Pending.Reset();KeepPending();Refresh();Session->Status=TEXT("Region retained. Prepare refreshes geometry; Apply saves the full case.");return true;
}
