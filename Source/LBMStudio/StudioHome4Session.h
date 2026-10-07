#pragma once
#include "CoreMinimal.h"
#include "StudioHome4Config.h"
class FStudioModel;

/** One retained editor across HOME4 pages; scientific state changes only on Apply. */
class FStudioHome4Session
{
public:
    explicit FStudioHome4Session(TSharedPtr<FStudioModel> InModel);
    void Refresh();
    void Revert();
    void Set(const FString& Key,const FString& Value);
    FString Get(const FString& Key) const;
    bool Build(FStudioHome4Spec& Out,FString& Error) const;
    bool Apply();
    bool IsDirty() const;
    bool HasConflict() const { return bConflict; }
    bool HasSpec() const;
    bool ApplyRecipe(const FString& Id);
    FString Status;
    static FString Key(const FStudioHome4Field& Field) { return Field.Section+TEXT(".")+Field.Key; }
    const TMap<FString,FString>& Values() const { return Edits; }
private:
    TWeakPtr<FStudioModel> Model;
    FGuid Project,Case;
    FString Baseline;
    FStudioHome4Spec Saved;
    TMap<FString,FString> Edits,Original;
    bool bConflict=false;
};
