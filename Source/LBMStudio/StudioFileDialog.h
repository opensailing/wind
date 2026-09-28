#pragma once
#include "CoreMinimal.h"

namespace StudioFileDialog
{
    bool Project(bool bSave, const FString& CurrentPath, const FString& SuggestedName, FString& OutPath);
    bool Geometry(const FString& CurrentPath, FString& OutPath);
    bool ImportGeometry(FString& OutPath);
    /** Choose an original completed OpenFOAM log and retain native file access. */
    bool ResidualLog(const FString& CurrentPath,FString& OutPath);
    /** Choose a directory so macOS grants access to metadata and member files. */
    bool RecordingFolder(const FString& CurrentPath,FString& OutPayloadPath);
    bool ReconstructionFolder(const FString& CurrentPath,FString& OutDescriptorPath);
    bool ProbeCSV(const FString& SuggestedName,FString& OutPath);
    /** CSV destination with purpose-specific native panel copy. */
    bool CSV(const FString& SuggestedName,FString& OutPath,const FString& Title,const FString& Description);
    bool SnapshotPNG(const FString& SuggestedName,FString& OutPath);
#if WITH_DEV_AUTOMATION_TESTS
    /** Routed UI tests supply a one-shot panel result; never proves native picker access. */
    void SetNextRecordingFolderForAutomation(const FString& Folder);
    bool ConsumeRecordingFolderForAutomation(FString& OutPayloadPath,bool& bAccepted);
    void SetNextReconstructionFolderForAutomation(const FString& Folder);
    bool ConsumeReconstructionFolderForAutomation(FString& OutDescriptorPath,bool& bAccepted);
    void SetNextResidualLogForAutomation(const FString& Path);
    bool ConsumeResidualLogForAutomation(FString& OutPath,bool& bAccepted);
    void SetNextProbeCSVForAutomation(const FString& Path);
    bool ConsumeProbeCSVForAutomation(FString& OutPath,bool& bAccepted);
    void SetNextSnapshotPNGForAutomation(const FString& Path);
    bool ConsumeSnapshotPNGForAutomation(FString& OutPath,bool& bAccepted);
#endif
    void* BeginAccess(const FString& Path);
    void EndAccess(void* Token);
    void RememberAccess(const FString& Path);
    bool WriteAtomic(const FString& Path, const FString& Text, FString& Error);
    bool WriteAtomicBytes(const FString& Path,const TArray64<uint8>& Bytes,FString& Error);
}

struct FStudioFileAccess
{
    explicit FStudioFileAccess(const FString& Path) : Token(StudioFileDialog::BeginAccess(Path)) {}
    ~FStudioFileAccess() { StudioFileDialog::EndAccess(Token); }
    FStudioFileAccess(const FStudioFileAccess&)=delete;
    FStudioFileAccess& operator=(const FStudioFileAccess&)=delete;
private:
    void* Token;
};
