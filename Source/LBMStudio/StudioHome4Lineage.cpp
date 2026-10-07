#include "StudioHome4Lineage.h"
#include "StudioHome4Authoring.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
namespace StudioHome4LineageLocal
{
FString Value(const TSharedPtr<FJsonValue>& V)
{
    if(!V||V->Type==EJson::Null)return TEXT("unspecified");
    if(V->Type==EJson::Number)return FString::Printf(TEXT("%.9g"),V->AsNumber());
    if(V->Type==EJson::String)return V->AsString().IsEmpty()?TEXT("unspecified"):V->AsString();
    FString Text;auto O=MakeShared<FJsonObject>();O->SetField(TEXT("v"),V);FJsonSerializer::Serialize(O,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    const FString Prefix=TEXT("{\"v\":");if(Text.StartsWith(Prefix))Text=Text.Mid(Prefix.Len(),Text.Len()-Prefix.Len()-1);
    return Text.Len()>160?Text.Left(157)+TEXT("…"):Text;
}
void Compare(const TSharedPtr<FJsonObject>& A,const TSharedPtr<FJsonObject>& B,const FString& Prefix,TArray<FString>& Out)
{
    TSet<FString> Unique;for(const auto& E:A->Values)Unique.Add(FString(*E.Key));for(const auto& E:B->Values)Unique.Add(FString(*E.Key));TArray<FString> Keys=Unique.Array();Keys.Sort();
    for(const auto& K:Keys)
    {
        if(Prefix.IsEmpty()&&(K==TEXT("version")||K==TEXT("lineageId")||K==TEXT("parentRunId")||K==TEXT("parentSpecSHA256")||K==TEXT("branchId")))continue;
        const auto L=A->TryGetField(K),R=B->TryGetField(K);const FString Path=Prefix.IsEmpty()?K:Prefix+TEXT(".")+K;
        if(L&&R&&L->Type==EJson::Object&&R->Type==EJson::Object){Compare(L->AsObject(),R->AsObject(),Path,Out);continue;}
        if((!L&&!R)||(L&&R&&FJsonValue::CompareEqual(*L,*R)))continue;
        FString Before=Value(L),After=Value(R);
        if(Before==After&&L&&R&&L->Type==EJson::Number&&R->Type==EJson::Number){Before=FString::Printf(TEXT("%.17g"),L->AsNumber());After=FString::Printf(TEXT("%.17g"),R->AsNumber());}
        FString Label=Path;for(const auto& F:StudioHome4Config::Fields())if(F.Section+TEXT(".")+F.Key==Path){Label=F.Label;break;}
        if(Out.Num()<256)Out.Add(Label+TEXT(": ")+Before+TEXT(" → ")+After);
    }
}
}
TArray<FString> StudioHome4Lineage::Changes(const FStudioHome4Spec& Before,const FStudioHome4Spec& After)
{TArray<FString> Out;StudioHome4LineageLocal::Compare(StudioHome4Config::ToJSON(Before),StudioHome4Config::ToJSON(After),TEXT(""),Out);return Out;}
TArray<FStudioHome4LineageNode> StudioHome4Lineage::Tree(const FStudioProject& Project)
{
    TArray<FStudioHome4LineageNode> Nodes;TMap<FString,int32> Identities;TSet<FString> Ambiguous;
    auto AddId=[&](const FString& Id,int32 Index){if(Id.IsEmpty())return;if(const auto* Existing=Identities.Find(Id);Existing&&*Existing!=Index)Ambiguous.Add(Id);else Identities.Add(Id,Index);};
    for(int32 I=0;I<Project.Runs.Num();++I){FStudioHome4LineageNode N;N.RunIndex=I;N.Identity=Project.Runs[I].GetId().ToString();Nodes.Add(N);AddId(N.Identity,I);if(Project.Runs[I].GetProvenance())AddId(Project.Runs[I].GetProvenance()->RunId,I);}
    for(int32 I=0;I<Nodes.Num();++I)
    {
        auto& N=Nodes[I];const auto* C=Project.Runs[I].GetConfiguration();const auto& Original=Project.Runs[I].GetProvenance();
        const FStudioHome4Spec* S=C&&C->Home4?&C->Home4.GetValue():Original&&Original->OriginalRunSpec?&Original->OriginalRunSpec.GetValue():nullptr;
        const FString ParentId=S&&!S->ParentRunId.IsEmpty()?S->ParentRunId:Original?Original->ParentRunId:FString(),ParentSHA=S&&!S->ParentSpecSHA256.IsEmpty()?S->ParentSpecSHA256:Original?Original->ParentSpecSHA256:FString();
        if(ParentId.IsEmpty())continue;
        const int32* Parent=Identities.Find(ParentId);
        if(Ambiguous.Contains(ParentId)){N.Issue=TEXT("Ambiguous original parent identity: ")+ParentId;continue;}
        if(!Parent){N.Issue=TEXT("External/dangling parent: ")+ParentId+TEXT(" · request SHA ")+ParentSHA;continue;}
        const auto* P=Project.Runs[*Parent].GetConfiguration();const auto& PO=Project.Runs[*Parent].GetProvenance();
        const FStudioHome4Spec* ParentSpec=P&&P->Home4?&P->Home4.GetValue():PO&&PO->OriginalRunSpec?&PO->OriginalRunSpec.GetValue():nullptr;
        if(!ParentSpec){N.Issue=TEXT("Parent has no verified typed request; ancestry is unverified.");continue;}
        if(ParentSHA.IsEmpty()||ParentSHA!=StudioHome4Authoring::Fingerprint(*ParentSpec)){N.Issue=TEXT("Parent request SHA256 mismatch; edge rejected.");continue;}
        N.Parent=*Parent;if(S)N.Changes=Changes(*ParentSpec,*S);
    }
    // Check each bounded ancestry iteratively; no recursion or untrusted depth.
    for(int32 I=0;I<Nodes.Num();++I)
    {
        TSet<int32> Seen;int32 At=I;while(At!=INDEX_NONE&&!Seen.Contains(At)){Seen.Add(At);At=Nodes[At].Parent;}
        if(At!=INDEX_NONE){for(int32 Member:Seen){Nodes[Member].Parent=INDEX_NONE;Nodes[Member].Issue=TEXT("Cyclic parent references rejected.");}}
    }
    TArray<FStudioHome4LineageNode> Ordered;TArray<int32> Stack;for(int32 I=Nodes.Num()-1;I>=0;--I)if(Nodes[I].Parent==INDEX_NONE)Stack.Add(I);
    while(!Stack.IsEmpty())
    {
        const int32 I=Stack.Pop(EAllowShrinking::No);Ordered.Add(Nodes[I]);
        for(int32 Child=Nodes.Num()-1;Child>=0;--Child)if(Nodes[Child].Parent==I){Nodes[Child].Depth=Nodes[I].Depth+1;Stack.Add(Child);}
    }
    return Ordered;
}
