#include "SStudioHome4Regions.h"
#include "StudioHome4Setup.h"
#include "StudioHome4Readouts.h"
#include "StudioModel.h"
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
EStudioHome4UnitDisplay Display(const TSharedPtr<FStudioHome4Session>& Session)
{const auto M=Session->Owner();return M?M->UnitDisplay:EStudioHome4UnitDisplay::Lattice;}
bool Convert(FVector& V,EStudioHome4UnitDisplay From,EStudioHome4UnitDisplay To,const FStudioHome4Spec& S)
{
    FVector Result;
    for(int32 I=0;I<3;++I){const auto N=StudioHome4Config::ConvertUnits(V[I],EStudioHome4Quantity::Length,From,To,S);if(!N)return false;Result[I]=*N;}
    V=Result;return true;
}
FString Tooltip(const FVector& Root,const FStudioHome4Spec& S)
{
    FString Text=TEXT("XYZ lengths in the displayed unit form. Retaining converts once into the declared request units.");
    for(int32 I=0;I<3;++I)Text+=TEXT("\n")+FString(I==0?TEXT("X: "):I==1?TEXT("Y: "):TEXT("Z: "))+StudioHome4Readouts::Tooltip(Root[I],EStudioHome4Quantity::Length,EStudioHome4UnitDisplay::Lattice,&S);
    return Text;
}
bool Inside(const FVector& Min,const FVector& Max,const FStudioHome4Spec& S,double Scale)
{
    if(!S.Lattice.Extents)return false;const FVector Tank(*S.Lattice.Extents);
    for(int32 I=0;I<3;++I)if(Min[I]*Scale<0||Max[I]*Scale>Tank[I])return false;
    return true;
}
}
void SStudioHome4Regions::Construct(const FArguments& A)
{
    Session=A._Session;bPatches=A._Patches;check(Session.IsValid());Session->Refresh();Pending=Session->Pending(PendingKey());
    if(const auto* V=Pending.Find(TEXT("_selected")))Selected=FCString::Atoi(**V);
    SetCanTick(true);auto Content=SNew(SVerticalBox);
    Content->AddSlot().AutoHeight().Padding(0,12)[StudioUI::Label(bPatches?TEXT("Authored MD patches"):TEXT("Authored 3D zones"),14,StudioUI::Text,true)];
    auto Buttons=SNew(SHorizontalBox);auto Action=[&](const FString& T,const FString& K,TFunction<void()> Fn)
    {Buttons->AddSlot().AutoWidth().Padding(0,0,6,0)[SNew(SButton).Tag(FName(*(TEXT("Home4Region.")+K))).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([Fn]{Fn();return FReply::Handled();})[StudioUI::Label(T,10)]];};
    Action(TEXT("Add"),bPatches?TEXT("addPatch"):TEXT("addZone"),[this]{Add();});
    Action(TEXT("Previous"),TEXT("previous"),[this]{if(Pending.IsEmpty()){Selected=FMath::Max(0,Selected-1);Refresh();}else Session->Status=TEXT("Retain or revert this region's text before switching.");});
    Action(TEXT("Next"),TEXT("next"),[this]{if(Pending.IsEmpty()){++Selected;Refresh();}else Session->Status=TEXT("Retain or revert this region's text before switching.");});
    Action(TEXT("Retain region"),TEXT("retain"),[this]{Commit();});
    Action(TEXT("Revert region text"),TEXT("revert"),[this]{Pending.Reset();KeepPending();Refresh();});
    Action(TEXT("Remove"),TEXT("remove"),[this]{Remove();});Content->AddSlot().AutoHeight()[Buttons];
    Content->AddSlot().AutoHeight().Padding(0,10)[SAssignNew(Rows,SVerticalBox)];ChildSlot[Content];Refresh();
}
void SStudioHome4Regions::KeepPending()
{
    if(!Pending.IsEmpty())
    {
        Pending.Add(TEXT("_selected"),LexToString(Selected));
        if(!Pending.Contains(TEXT("_base")))
        {
            FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true)){Session->Status=Error;return;}
            Pending.Add(TEXT("_base"),StudioHome4Authoring::Fingerprint(S));
            if(bPatches&&S.Authoring.Patches.IsValidIndex(Selected))Pending.Add(TEXT("_target"),S.Authoring.Patches[Selected].Id);
            if(!bPatches&&S.Authoring.Zones.IsValidIndex(Selected))Pending.Add(TEXT("_target"),S.Authoring.Zones[Selected].Id);
            Pending.Add(TEXT("_display"),LexToString(int32(CoordinateDisplay)));
            const auto M=Session->Owner();if(M){Pending.Add(TEXT("_project"),M->Project.Id.ToString());Pending.Add(TEXT("_case"),M->Project.Draft.Id.ToString());}
        }
    }
    Session->RetainPending(PendingKey(),Pending);
}
void SStudioHome4Regions::Tick(const FGeometry&,double,float)
{
    Session->Refresh();const auto Retained=Session->Pending(PendingKey());
    if(Retained.IsEmpty()&&!Pending.IsEmpty()){Pending.Reset();Refresh();}
    FStudioHome4Spec S;FString Error;if(Session->Build(S,Error,true)&&Pending.IsEmpty()&&
       (ShownSHA!=StudioHome4Authoring::Fingerprint(S)||RequestedDisplay!=StudioHome4RegionWidgetLocal::Display(Session)))Refresh();
}
void SStudioHome4Regions::Refresh()
{
    using namespace StudioHome4RegionWidgetLocal;Rows->ClearChildren();FStudioHome4Spec S;FString Error;
    if(!Session->Build(S,Error,true)){Rows->AddSlot().AutoHeight()[StudioUI::Label(Error,10,StudioUI::Amber)];return;}
    ShownSHA=StudioHome4Authoring::Fingerprint(S);ShownCount=bPatches?S.Authoring.Patches.Num():S.Authoring.Zones.Num();Selected=FMath::Clamp(Selected,0,FMath::Max(0,ShownCount-1));
    if(!ShownCount){Rows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Add an explicit region to author bounds and a geometric preview."),10,StudioUI::Muted)];return;}
    RequestedDisplay=Display(Session);CoordinateDisplay=RequestedDisplay;if(const auto* V=Pending.Find(TEXT("_display")))CoordinateDisplay=EStudioHome4UnitDisplay(FMath::Clamp(FCString::Atoi(**V),0,2));
    double Scale=1;const bool bUnitMap=bPatches||StudioHome4Setup::ZoneScale(S,Scale,Error);
    FVector Test(1);const bool bConvert=bUnitMap&&Convert(Test,EStudioHome4UnitDisplay::Lattice,CoordinateDisplay,S);
    if(!bConvert&&Pending.IsEmpty())CoordinateDisplay=EStudioHome4UnitDisplay::Lattice;
    const FString Unit=bUnitMap?StudioHome4Readouts::Unit(EStudioHome4Quantity::Length,CoordinateDisplay):S.Authoring.ZoneUnits+TEXT(" (map missing)");
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[StudioUI::Label(FString::Printf(TEXT("Region %d of %d · coordinates: %s"),Selected+1,ShownCount,*Unit),10,StudioUI::Muted)];
    auto Field=[&](const FString& Key,const FString& Label,const FString& Value,const TArray<FString>& Choices=TArray<FString>(),const FString& Help=FString())
    {
        TSharedPtr<SWidget> Input;
        if(Choices.IsEmpty())Input=SNew(SEditableTextBox).Tag(FName(*(TEXT("Home4Region.")+Key))).Style(&StudioUI::InputStyle()).Font(StudioUI::Font(10)).ToolTipText(FText::FromString(Help)).Text(FText::FromString(Pending.Contains(Key)?Pending[Key]:Value)).OnTextChanged_Lambda([this,Key](const FText& T){Pending.Add(Key,T.ToString());KeepPending();});
        else Input=SNew(SComboButton).Tag(FName(*(TEXT("Home4Region.")+Key))).ButtonStyle(&StudioUI::ButtonStyle()).ToolTipText(FText::FromString(Help)).OnGetMenuContent_Lambda([this,Key,Choices]
        {
            auto Menu=SNew(SVerticalBox);
            for(const auto& C:Choices)
            {
                Menu->AddSlot().AutoHeight()[SNew(SButton).Tag(FName(*(TEXT("Home4Region.choice.")+Key+TEXT(".")+C))).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this,Key,C]{Pending.Add(Key,C);KeepPending();FSlateApplication::Get().DismissAllMenus();Refresh();return FReply::Handled();})[StudioUI::Label(C,10)]];
            }
            return StaticCastSharedRef<SWidget>(Menu);
        }).ButtonContent()[SNew(STextBlock).Font(StudioUI::Font(10)).Text_Lambda([this,Key,Value]{return FText::FromString(Pending.Contains(Key)?Pending[Key]:Value);})];
        Rows->AddSlot().AutoHeight().Padding(0,4)[SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(165)[StudioUI::Label(Label,10,StudioUI::Muted)]]+SHorizontalBox::Slot().FillWidth(1)[Input.ToSharedRef()]];
    };
    auto Coordinates=[&](const TCHAR* Key,const TCHAR* Label,const FVector& Raw)
    {
        FVector Root=Raw*Scale,Shown=Root;if(bUnitMap)Convert(Shown,EStudioHome4UnitDisplay::Lattice,CoordinateDisplay,S);else Shown=Raw;
        Field(Key,FString(Label)+TEXT(" [")+Unit+TEXT("]"),Vector(Shown),{},bUnitMap?Tooltip(Root,S):TEXT("Unit map missing. Declare it before retaining converted coordinates."));
    };
    if(bPatches)
    {
        const auto& P=S.Authoring.Patches[Selected];Field(TEXT("id"),TEXT("Patch identity"),P.Id);Field(TEXT("body"),TEXT("Bound body identity"),P.BodyId);
        Field(TEXT("level"),TEXT("Level (root = 0)"),LexToString(P.Level));Coordinates(TEXT("origin"),TEXT("Origin XYZ"),P.Origin);
        Field(TEXT("extents"),TEXT("Extents [local cells]"),Vector(FVector(P.Extents)),{},TEXT("Integer node extents in this level's local cells; spacing is 2^−level root cells. These counts do not change with the display unit selector."));
        Field(TEXT("follow"),TEXT("Follow declared body"),P.bFollowBody?TEXT("true"):TEXT("false"),{TEXT("false"),TEXT("true")});
    }
    else
    {
        const auto& Z=S.Authoring.Zones[Selected];Field(TEXT("id"),TEXT("Zone identity"),Z.Id);
        Field(TEXT("kind"),TEXT("Affected fields"),Z.Kind,{TEXT("sponge"),TEXT("beach"),TEXT("floor"),TEXT("wave-absorption")});
        Field(TEXT("profile"),TEXT("Strength profile"),Z.Profile,{TEXT("constant"),TEXT("linear"),TEXT("cubic"),TEXT("linear-reverse"),TEXT("cubic-reverse")});
        Field(TEXT("axis"),TEXT("Profile direction"),Z.Axis,{TEXT("x"),TEXT("y"),TEXT("z")});Coordinates(TEXT("minimum"),TEXT("Minimum XYZ"),Z.Minimum);Coordinates(TEXT("maximum"),TEXT("Maximum XYZ"),Z.Maximum);
        Field(TEXT("strength"),TEXT("Root strength [0,1]"),LexToString(Z.Strength));Field(TEXT("exponent"),TEXT("Level exponent"),LexToString(Z.LevelExponent));
        FString Strengths=TEXT("Full-profile strength per declared level:");const int32 Levels=int32(S.Multidomain.Levels.Get(1));for(int32 I=0;I<Levels;++I)Strengths+=FString::Printf(TEXT(" L%d=%.6g"),I,FMath::Clamp(Z.Strength*FMath::Pow(2.,-Z.LevelExponent*I),0.,1.));Rows->AddSlot().AutoHeight().Padding(0,6)[StudioUI::Label(Strengths,9,StudioUI::Muted)];
    }
    Rows->AddSlot().AutoHeight().Padding(0,8)[SNew(STextBlock).AutoWrapText(true).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).Text(FText::FromString(bPatches?
        TEXT("Origin is a root-space length; extents are local integer cells. Follow-body binds the declared identity. Changing topology clears derived level/allocation counts; derive them again from explicit patches. Parent Apply saves retained requests."):
        TEXT("Stored coordinates use the chosen Zone units; displayed coordinates use the viewer unit form. Sponge relaxes pd/u; beach relaxes u/phi with pressure untouched; floor is friction. Cubic profile is t³; reverse is (1−t)³. Strength = clamp(s·profile·2^(−exponent·level),0,1).")))];
}
void SStudioHome4Regions::Add()
{
    Session->Refresh();if(!Pending.IsEmpty()||Session->HasPending()){Session->Status=TEXT("Retain or revert pending text before adding a region.");return;}
    FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error)){Session->Status=Error;return;}
    if(bPatches)
    {
        if(S.Authoring.Patches.Num()>=32){Session->Status=TEXT("The bounded patch limit is 32.");return;}
        FStudioHome4AuthoredPatch P;P.Id=TEXT("patch-")+FGuid::NewGuid().ToString();P.BodyId=S.Authoring.BodyId;P.Origin=S.Geometry.InitialPositionCells.Get(FVector::ZeroVector);S.Authoring.Patches.Add(P);Selected=S.Authoring.Patches.Num()-1;StudioHome4Setup::InvalidatePatchCounts(S);
    }
    else
    {
        if(S.Authoring.Zones.Num()>=128||!S.Lattice.Extents){Session->Status=TEXT("A new zone needs explicit root XYZ extents and fewer than 128 regions.");return;}
        if(S.Authoring.ZoneUnits.IsEmpty()&&S.Authoring.Zones.IsEmpty())S.Authoring.ZoneUnits=TEXT("root-cells");
        double Scale;if(!StudioHome4Setup::ZoneScale(S,Scale,Error)){Session->Status=Error;return;}
        FStudioHome4AuthoredZone Z;Z.Id=TEXT("zone-")+FGuid::NewGuid().ToString();Z.Minimum=FVector::ZeroVector;Z.Maximum=FVector(*S.Lattice.Extents)/Scale;Z.Strength=0;S.Authoring.Zones.Add(Z);Selected=S.Authoring.Zones.Num()-1;
    }
    if(Session->Replace(S,Error)){Session->Status=bPatches?TEXT("Patch added; edit root origin/local counts and derive level counts before allocation estimates."):TEXT("Zone added with explicit zero strength and full-tank bounds in the declared units. Edit its bounds/profile; Prepare previews; Apply saves.");Refresh();}else Session->Status=Error;
}
void SStudioHome4Regions::Remove()
{
    Session->Refresh();if(!Pending.IsEmpty()||Session->HasPending()){Session->Status=TEXT("Retain or revert pending text before removing a region.");return;}
    FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error)){Session->Status=Error;return;}
    if(bPatches){if(!S.Authoring.Patches.IsValidIndex(Selected))return;S.Authoring.Patches.RemoveAt(Selected);StudioHome4Setup::InvalidatePatchCounts(S);}
    else{if(!S.Authoring.Zones.IsValidIndex(Selected))return;S.Authoring.Zones.RemoveAt(Selected);}
    if(Session->Replace(S,Error)){Refresh();Session->Status=TEXT("Region removed from the retained draft; Apply saves this change.");}else Session->Status=Error;
}
bool SStudioHome4Regions::Commit()
{
    using namespace StudioHome4RegionWidgetLocal;Session->Refresh();if(Pending.IsEmpty())return true;
    const auto M=Session->Owner();FStudioHome4Spec S;FString Error;
    if(!M||!Session->Pending(PendingKey()).OrderIndependentCompareEqual(Pending)||!Session->Build(S,Error,true)||
       Pending.FindRef(TEXT("_project"))!=M->Project.Id.ToString()||Pending.FindRef(TEXT("_case"))!=M->Project.Draft.Id.ToString()||
       Pending.FindRef(TEXT("_base"))!=StudioHome4Authoring::Fingerprint(S))
    {Session->Status=TEXT("The scoped request or unit map changed while editing this region. Pending text is retained; revert it and reopen the current region.");return false;}
    const FString Target=Pending.FindRef(TEXT("_target"));
    const int32 Index=bPatches?S.Authoring.Patches.IndexOfByPredicate([&](const auto& P){return P.Id==Target;}):S.Authoring.Zones.IndexOfByPredicate([&](const auto& Z){return Z.Id==Target;});
    if(Index==INDEX_NONE){Session->Status=TEXT("The edited region identity no longer exists; pending text was not applied to another region.");return false;}
    bool Valid=true;auto Text=[&](const TCHAR* K,FString& V){if(const auto* P=Pending.Find(K))V=*P;};
    auto Num=[&](const TCHAR* K,double& V){if(const auto* P=Pending.Find(K))Valid&=StudioColor::ParseNumber(*P,V);};
    auto Vec=[&](const TCHAR* K,FVector& V){if(const auto* P=Pending.Find(K))Valid&=Vector(*P,V);};
    double Scale=1;if(!bPatches&&!StudioHome4Setup::ZoneScale(S,Scale,Error)){Session->Status=Error;return false;}
    auto Coord=[&](const TCHAR* K,FVector& V)
    {if(const auto* P=Pending.Find(K)){FVector Shown;if(!Vector(*P,Shown)||!Convert(Shown,CoordinateDisplay,EStudioHome4UnitDisplay::Lattice,S))Valid=false;else V=Shown/Scale;}};
    if(bPatches)
    {
        auto& P=S.Authoring.Patches[Index];Text(TEXT("id"),P.Id);Text(TEXT("body"),P.BodyId);
        double Level=P.Level;Num(TEXT("level"),Level);Valid&=Level>=0&&Level<=15&&FMath::FloorToDouble(Level)==Level;if(Valid)P.Level=int32(Level);
        Coord(TEXT("origin"),P.Origin);FVector Extents(P.Extents);Vec(TEXT("extents"),Extents);
        for(int32 I=0;I<3;++I)Valid&=Extents[I]>=2&&Extents[I]<=1048576&&FMath::FloorToDouble(Extents[I])==Extents[I];
        if(Valid)P.Extents=FIntVector(int32(Extents.X),int32(Extents.Y),int32(Extents.Z));
        if(const auto* F=Pending.Find(TEXT("follow"))){Valid&=*F==TEXT("true")||*F==TEXT("false");P.bFollowBody=*F==TEXT("true");}
        Valid&=!P.bFollowBody||(!P.BodyId.IsEmpty()&&P.BodyId==S.Authoring.BodyId);StudioHome4Setup::InvalidatePatchCounts(S);
    }
    else
    {
        auto& Z=S.Authoring.Zones[Index];Text(TEXT("id"),Z.Id);Text(TEXT("kind"),Z.Kind);Text(TEXT("profile"),Z.Profile);Text(TEXT("axis"),Z.Axis);
        Coord(TEXT("minimum"),Z.Minimum);Coord(TEXT("maximum"),Z.Maximum);Num(TEXT("strength"),Z.Strength);Num(TEXT("exponent"),Z.LevelExponent);
        Valid&=Inside(Z.Minimum,Z.Maximum,S,Scale);
    }
    if(!Valid){Session->Status=TEXT("Region needs finite mapped coordinates within the tank, integer counts and a valid body binding. Pending text and the previous request were kept.");return false;}
    if(!Session->Replace(S,Error)){Session->Status=Error;return false;}
    Pending.Reset();KeepPending();Selected=Index;Refresh();Session->Status=TEXT("Region retained under its exact identity. Prepare refreshes geometry; Apply saves the full case.");return true;
}
