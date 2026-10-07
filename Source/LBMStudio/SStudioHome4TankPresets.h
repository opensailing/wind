#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4TankPresets.h"
#include "StudioHome4Authoring.h"
#include "Async/Future.h"

class SStudioHome4TankPresets final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4TankPresets){}
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4AuthoringSession>,Authoring)
        SLATE_ARGUMENT(TFunction<bool(FString&)>,ImportPath)
        SLATE_ARGUMENT(TFunction<bool(FString&)>,ExportPath)
    SLATE_END_ARGS()
    void Construct(const FArguments&);
    ~SStudioHome4TankPresets();
    void Tick(const FGeometry&,double,float)override;
    bool BeginImport(const FString& Path);
    void PollImport();
    void CancelImport();
    bool IsImporting()const{return Pending.IsValid();}
#if WITH_DEV_AUTOMATION_TESTS
    void SetImportVerificationForAutomation(TFunction<void()> BeforeVerify){BeforeImportVerify=MoveTemp(BeforeVerify);}
#endif
private:
    struct FResult{FStudioHome4TankZonePreset Preset;FString Error;bool bGood=false;};
    TSharedPtr<FStudioHome4Session> Session;
    TSharedPtr<FStudioHome4AuthoringSession> Authoring;
    TFunction<bool(FString&)> ImportPath,ExportPath;
    TMap<FString,FStudioHome4TankZonePreset> Slots;
    FGuid Project,Case,ImportProject,ImportCase;
    FString Selected=TEXT("G"),ReviewBase,ImportBase,ImportName,Message;
    bool bApplying=false;
    FStudioHome4Spec ReviewedApply;
    TFuture<FResult> Pending;
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> Cancellation;
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void()> BeforeImportVerify;
#endif
    void Scope();void Select(const FString&);void Review();void Apply();void Import();void Export();
    FString Details()const;
};
