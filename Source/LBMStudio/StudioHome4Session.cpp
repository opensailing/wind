#include "StudioHome4Session.h"
#include "StudioHome4Recipes.h"
#include "StudioModel.h"
#include "Dom/JsonObject.h"

namespace
{
FString ValueText(const TSharedPtr<FJsonValue>& V)
{
    if(!V||V->Type==EJson::Null)return {};
    if(V->Type==EJson::String)return V->AsString();
    if(V->Type==EJson::Boolean)return V->AsBool()?TEXT("true"):TEXT("false");
    if(V->Type==EJson::Number)return FString::Printf(TEXT("%.17g"),V->AsNumber());
    if(V->Type==EJson::Array)
    {TArray<FString> Items;for(const auto& Item:V->AsArray())Items.Add(ValueText(Item));return FString::Join(Items,TEXT(", "));}
    return {};
}
bool Number(const FString& Text,double& Out)
{
    const FString S=Text.TrimStartAndEnd();if(S.IsEmpty()||S.Len()>64)return false;
    // Validate the entire decimal token before Unreal's platform conversion.
    // Floating-point std::from_chars requires macOS 26; this app supports older Macs.
    int32 I=0,Digits=0;
    if(S[I]==TEXT('+')||S[I]==TEXT('-'))++I;
    auto Digit=[](TCHAR C){return C>=TEXT('0')&&C<=TEXT('9');};
    while(I<S.Len()&&Digit(S[I])){++I;++Digits;}
    if(I<S.Len()&&S[I]==TEXT('.'))
    {++I;while(I<S.Len()&&Digit(S[I])){++I;++Digits;}}
    if(Digits==0)return false;
    if(I<S.Len()&&(S[I]==TEXT('e')||S[I]==TEXT('E')))
    {
        ++I;if(I<S.Len()&&(S[I]==TEXT('+')||S[I]==TEXT('-')))++I;
        const int32 ExponentStart=I;while(I<S.Len()&&Digit(S[I]))++I;
        if(I==ExponentStart)return false;
    }
    if(I!=S.Len())return false;
    Out=FCString::Atod(*S);return FMath::IsFinite(Out);
}
}
FStudioHome4Session::FStudioHome4Session(TSharedPtr<FStudioModel> InModel):Model(InModel){Revert();}
bool FStudioHome4Session::HasSpec() const {const auto M=Model.Pin();return M&&M->Project.Draft.Home4.IsSet();}
void FStudioHome4Session::Revert()
{
    const auto M=Model.Pin();if(!M)return;
    Project=M->Project.Id;Case=M->Project.Draft.Id;Saved=M->Project.Draft.Home4.Get(FStudioHome4Spec());
    Baseline=M->Project.Draft.Home4.IsSet()?StudioHome4Config::Serialize(Saved):FString();Edits.Reset();
    const auto JSON=StudioHome4Config::ToJSON(Saved);
    for(const auto& Field:StudioHome4Config::Fields())
    {
        if(Field.Section.IsEmpty()){const auto V=JSON->TryGetField(Field.Key);Edits.Add(Key(Field),ValueText(V));continue;}
        const TSharedPtr<FJsonObject>* Section=nullptr;
        if(JSON->TryGetObjectField(Field.Section,Section))
        {const auto V=(*Section)->TryGetField(Field.Key);Edits.Add(Key(Field),ValueText(V));}
    }
    AllocationEdits.Reset();
    for(const auto& A:Saved.Performance.Allocations)AllocationEdits.Add({A.Name,A.Nodes.IsSet()?LexToString(*A.Nodes):FString(),LexToString(A.Components),LexToString(A.BytesPerComponent),LexToString(A.Buffers)});
    OriginalAllocations=AllocationEdits;Original=Edits;bConflict=false;Status.Empty();
}
void FStudioHome4Session::Refresh()
{
    const auto M=Model.Pin();if(!M)return;
    if(Project!=M->Project.Id||Case!=M->Project.Draft.Id){Revert();return;}
    const FString Current=M->Project.Draft.Home4.IsSet()?StudioHome4Config::Serialize(M->Project.Draft.Home4.GetValue()):FString();
    if(Current!=Baseline)
    {
        if(IsDirty()){bConflict=true;Status=TEXT("The applied HOME4 configuration changed. Your edits are retained; Revert loads the current configuration.");}
        else Revert();
    }
}
void FStudioHome4Session::Set(const FString& Key,const FString& Value){if(Edits.Contains(Key)){Edits[Key]=Value;Status.Empty();}}
FString FStudioHome4Session::Get(const FString& Key) const {const auto* V=Edits.Find(Key);return V?*V:FString();}
bool FStudioHome4Session::IsDirty() const
{if(AllocationEdits!=OriginalAllocations)return true;for(const auto& E:Edits){const auto* O=Original.Find(E.Key);if(!O||*O!=E.Value)return true;}return false;}
FString FStudioHome4Session::AllocationValue(int32 Row,int32 Column) const
{return AllocationEdits.IsValidIndex(Row)&&AllocationEdits[Row].IsValidIndex(Column)?AllocationEdits[Row][Column]:FString();}
void FStudioHome4Session::SetAllocation(int32 Row,int32 Column,const FString& Value)
{if(AllocationEdits.IsValidIndex(Row)&&AllocationEdits[Row].IsValidIndex(Column)){AllocationEdits[Row][Column]=Value;Status.Empty();}}
void FStudioHome4Session::AddAllocation()
{
    if(AllocationEdits.Num()>=256)return;
    FString Nodes,Error;FStudioHome4Spec Preview;
    if(Build(Preview,Error))
    {
        const auto Cells=StudioHome4Config::Derive(Preview).RootCells;
        if(Cells.IsSet()&&*Cells<=9007199254740991ULL)Nodes=LexToString(*Cells);
    }
    AllocationEdits.Add({TEXT("New array"),Nodes,TEXT("1"),TEXT("4"),TEXT("1")});
}
void FStudioHome4Session::RemoveAllocation(int32 Row)
{if(AllocationEdits.IsValidIndex(Row))AllocationEdits.RemoveAt(Row);}
bool FStudioHome4Session::Build(FStudioHome4Spec& Out,FString& Error) const
{
    auto JSON=StudioHome4Config::ToJSON(Saved);
    for(const auto& F:StudioHome4Config::Fields())
    {
        const FString V=Get(Key(F)).TrimStartAndEnd();const auto Section=F.Section.IsEmpty()?JSON:JSON->GetObjectField(F.Section);
        if(V.IsEmpty()&&F.bOptional){Section->SetField(F.Key,MakeShared<FJsonValueNull>());continue;}
        if(F.Type==EStudioHome4FieldType::String){Section->SetStringField(F.Key,V);continue;}
        if(F.Type==EStudioHome4FieldType::Boolean)
        {
            if(V!=TEXT("true")&&V!=TEXT("false")){Error=F.Label+TEXT(": choose On, Off or Not specified.");return false;}
            Section->SetBoolField(F.Key,V==TEXT("true"));continue;
        }
        if(F.Type==EStudioHome4FieldType::Number||F.Type==EStudioHome4FieldType::Integer)
        {
            double N;if(!Number(V,N)||N<F.Minimum||N>F.Maximum||(F.Type==EStudioHome4FieldType::Integer&&FMath::FloorToDouble(N)!=N))
            {Error=F.Label+TEXT(": enter a finite ")+(F.Type==EStudioHome4FieldType::Integer?TEXT("whole number"):TEXT("number"))+TEXT(" within the stated bounds.");return false;}
            Section->SetNumberField(F.Key,N);continue;
        }
        TArray<FString> Parts;V.ParseIntoArray(Parts,TEXT(","),false);TArray<TSharedPtr<FJsonValue>> Values;
        const bool Integer=F.Type==EStudioHome4FieldType::IntegerVector||F.Type==EStudioHome4FieldType::IntegerArray;
        if(V.IsEmpty()&&(F.Type==EStudioHome4FieldType::NumberArray||F.Type==EStudioHome4FieldType::IntegerArray))Parts.Empty();
        if(Parts.Num()>256){Error=F.Label+TEXT(": too many entries.");return false;}
        for(const auto& P:Parts)
        {
            double N;if(!Number(P,N)||N<F.Minimum||N>F.Maximum||(Integer&&FMath::FloorToDouble(N)!=N))
            {Error=F.Label+TEXT(": use comma-separated finite numbers.");return false;}
            Values.Add(MakeShared<FJsonValueNumber>(N));
        }
        Section->SetArrayField(F.Key,Values);
    }
    TArray<TSharedPtr<FJsonValue>> Allocations;
    for(const auto& Draft:AllocationEdits)
    {
        auto A=MakeShared<FJsonObject>();A->SetStringField(TEXT("name"),Draft[0]);
        const TCHAR* Keys[]={TEXT("nodes"),TEXT("components"),TEXT("bytesPerComponent"),TEXT("buffers")};
        for(int32 I=1;I<5;++I)
        {
            if(I==1&&Draft[I].TrimStartAndEnd().IsEmpty()){A->SetField(Keys[I-1],MakeShared<FJsonValueNull>());continue;}
            double N;if(!Number(Draft[I],N)||N<1||N>9007199254740991.||N!=FMath::FloorToDouble(N))
            {Error=TEXT("Allocation ")+Draft[0]+TEXT(": enter positive whole counts; blank nodes leaves the memory estimate unknown.");return false;}
            A->SetNumberField(Keys[I-1],N);
        }
        Allocations.Add(MakeShared<FJsonValueObject>(A));
    }
    JSON->GetObjectField(TEXT("performance"))->SetArrayField(TEXT("allocations"),Allocations);
    return StudioHome4Config::FromJSON(JSON,Out,Error);
}
bool FStudioHome4Session::Apply()
{
    Refresh();const auto M=Model.Pin();if(!M||bConflict)return false;
    FStudioHome4Spec Spec;if(!Build(Spec,Status))return false;
    if(!M->EditCase(TEXT("Apply HOME4 configuration"),[&](auto& D){D.Home4=Spec;})){Status=M->Notice;return false;}
    Revert();Status=TEXT("HOME4 configuration applied. Save writes it to the project.");return true;
}
bool FStudioHome4Session::ApplyRecipe(const FString& Id)
{
    Refresh();const auto M=Model.Pin();const auto* R=StudioHome4Recipes::Find(Id);if(!M||!R)return false;
    if(IsDirty()){Status=TEXT("Apply or revert HOME4 edits before choosing a recipe.");return false;}
    auto Spec=R->Template;Spec.LineageId=FGuid::NewGuid().ToString();
    if(!M->EditCase(TEXT("Start HOME4 recipe lineage"),[&](auto& D){D.Home4=Spec;D.Name=R->Name;})){Status=M->Notice;return false;}
    Revert();Status=TEXT("Recipe applied. Reference evidence and solver measurements are not supplied.");return true;
}
