#include "StudioFileDialog.h"
#include "StudioRecording.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
namespace { TOptional<FString> AutomationRecordingFolder,AutomationReconstructionFolder,AutomationProbeCSV,AutomationSnapshotPNG,AutomationResidualLog; }
void StudioFileDialog::SetNextResidualLogForAutomation(const FString& Path)
{
    check(IsInGameThread());
    if(FParse::Param(FCommandLine::Get(),TEXT("StudioAutomation")))AutomationResidualLog=Path;
}
bool StudioFileDialog::ConsumeResidualLogForAutomation(FString& Out,bool& bAccepted)
{
    if(!AutomationResidualLog.IsSet())return false;
    bAccepted=!AutomationResidualLog->IsEmpty();if(bAccepted)Out=*AutomationResidualLog;
    AutomationResidualLog.Reset();return true;
}
void StudioFileDialog::SetNextSnapshotPNGForAutomation(const FString& Path)
{
    check(IsInGameThread());
    if(FParse::Param(FCommandLine::Get(),TEXT("StudioAutomation")))AutomationSnapshotPNG=Path;
}
bool StudioFileDialog::ConsumeSnapshotPNGForAutomation(FString& Out,bool& bAccepted)
{
    if(!AutomationSnapshotPNG.IsSet())return false;
    bAccepted=!AutomationSnapshotPNG->IsEmpty();if(bAccepted)Out=*AutomationSnapshotPNG;
    AutomationSnapshotPNG.Reset();return true;
}
void StudioFileDialog::SetNextProbeCSVForAutomation(const FString& Path)
{
    check(IsInGameThread());
    if(FParse::Param(FCommandLine::Get(),TEXT("StudioAutomation")))AutomationProbeCSV=Path;
}
bool StudioFileDialog::ConsumeProbeCSVForAutomation(FString& Out,bool& bAccepted)
{
    if(!AutomationProbeCSV.IsSet())return false;
    bAccepted=!AutomationProbeCSV->IsEmpty();if(bAccepted)Out=*AutomationProbeCSV;
    AutomationProbeCSV.Reset();return true;
}
void StudioFileDialog::SetNextRecordingFolderForAutomation(const FString& Folder)
{
    check(IsInGameThread());
    if(FParse::Param(FCommandLine::Get(),TEXT("StudioAutomation")))AutomationRecordingFolder=Folder;
}
bool StudioFileDialog::ConsumeRecordingFolderForAutomation(FString& Out,bool& bAccepted)
{
    if(!AutomationRecordingFolder.IsSet())return false;
    bAccepted=!AutomationRecordingFolder->IsEmpty();
    if(bAccepted)Out=StudioRecordings::DescriptorOrPayloadInFolder(*AutomationRecordingFolder);
    AutomationRecordingFolder.Reset();return true;
}
void StudioFileDialog::SetNextReconstructionFolderForAutomation(const FString& Folder)
{
    check(IsInGameThread());
    if(FParse::Param(FCommandLine::Get(),TEXT("StudioAutomation")))AutomationReconstructionFolder=Folder;
}
bool StudioFileDialog::ConsumeReconstructionFolderForAutomation(FString& Out,bool& bAccepted)
{
    if(!AutomationReconstructionFolder.IsSet())return false;
    bAccepted=!AutomationReconstructionFolder->IsEmpty();
    if(bAccepted)Out=*AutomationReconstructionFolder/TEXT("reconstruction.json");
    AutomationReconstructionFolder.Reset();return true;
}
#endif
#if !PLATFORM_MAC
bool StudioFileDialog::SnapshotPNG(const FString&,FString&) { return false; }
bool StudioFileDialog::WriteAtomicBytes(const FString&,const TArray64<uint8>&,FString& Error)
{Error=TEXT("Snapshot file export is not available on this platform.");return false;}
bool StudioFileDialog::ProbeCSV(const FString&,FString&) { return false; }
bool StudioFileDialog::CSV(const FString&,FString&,const FString&,const FString&) { return false; }
bool StudioFileDialog::RecordingFolder(const FString&,FString&) { return false; }
bool StudioFileDialog::ReconstructionFolder(const FString&,FString&) { return false; }
bool StudioFileDialog::ImportGeometry(FString&) { return false; }
bool StudioFileDialog::ResidualLog(const FString&,FString&) { return false; }
void* StudioFileDialog::BeginAccess(const FString&) { return nullptr; }
void StudioFileDialog::EndAccess(void*) {}
void StudioFileDialog::RememberAccess(const FString&) {}
bool StudioFileDialog::Project(bool, const FString&, const FString&, FString&)
{
    // Windows packaging and its native dialogs are a separately scoped platform milestone.
    return false;
}
bool StudioFileDialog::Geometry(const FString&,FString&) { return false; }
#endif
