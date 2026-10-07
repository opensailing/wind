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
    bool Build(FStudioHome4Spec& Out,FString& Error,bool bAllowPending=false) const;
    void RetainPending(const FString& Group,const TMap<FString,FString>& Values);
    TMap<FString,FString> Pending(const FString& Group)const;
    bool HasPending()const{return !PendingGroups.IsEmpty();}
    bool Apply();
    /** Replace the retained draft transactionally, preserving the applied baseline/undo boundary. */
    bool Replace(const FStudioHome4Spec& Spec,FString& Error);
    bool DeriveFrom(const FStudioHome4Spec& Spec,const FString& ParentRun,FString& Error);
    FString DisplayText(const FStudioHome4Field& Field,EStudioHome4UnitDisplay Display) const;
    bool SetDisplayText(const FStudioHome4Field& Field,const FString& Text,EStudioHome4UnitDisplay Display,FString& Error);
    FString FieldTooltip(const FStudioHome4Field& Field) const;
    TSharedPtr<FStudioModel> Owner() const {return Model.Pin();}
    int32 AllocationCount() const {return AllocationEdits.Num();}
    FString AllocationValue(int32 Row,int32 Column) const;
    void SetAllocation(int32 Row,int32 Column,const FString& Value);
    void AddAllocation();
    void RemoveAllocation(int32 Row);
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
    TArray<TArray<FString>> AllocationEdits,OriginalAllocations;
    bool bConflict=false,bExtraDirty=false;
    TMap<FString,TMap<FString,FString>> PendingGroups;
    void LoadValues(const FStudioHome4Spec& Spec);
};
