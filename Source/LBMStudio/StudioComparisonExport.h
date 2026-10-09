#pragma once
#include "StudioComparison.h"
#include "StudioFieldExportTask.h"
#include "StudioView.h"

/** Two frozen original scalar snapshots. Camera metadata describes the
 * independent views; it never transforms the exported scientific values. */
struct FStudioComparisonExportRequest
{
    FStudioComparisonResult Pair;
    FString Name=TEXT("Comparison");
    FStudioCameraState PrimaryCamera,SecondaryCamera;
    bool bSharedRange=false;
    EStudioExportCoordinates Coordinates=EStudioExportCoordinates::Source;
    EStudioFieldExportFormat Format=EStudioFieldExportFormat::VTK;
};
struct FStudioComparisonExportResult
{
    bool bSuccess=false,bCancelled=false;
    FString Error,Path,Name;
    FGuid ProjectId;
    FStudioFieldIdentity PrimaryIdentity,SecondaryIdentity;
    int32 CompletedSides=0;
    int64 Bytes=0;
};
namespace StudioComparisonExport
{
    /** Worker-only validation against retained snapshot timelines and fields.
     * Does not reopen sources or require live readers to remain available. */
    bool Validate(const FStudioComparisonExportRequest& Request,FString& Error,
        const FStudioLoadCancellation& Cancellation={});
    /** Write into caller-owned private staging. Produces primary/secondary
     * .vtp or .csv and comparison.json; VTK also includes comparison.vtm.
     * Each field is bounded by the existing 512 MiB writer limit; comparison
     * metadata is capped at 1 Mi-character. Partial staging must be discarded.
     * Retains original topology, alignment/mismatch, units, source hashes,
     * cameras and method. No subtraction, resampling or reconstruction. */
    FStudioComparisonExportResult Write(const FStudioComparisonExportRequest& Request,const FString& Directory,
        const FStudioLoadCancellation& Cancellation={},TFunction<void(int64,int64)> Progress={});
}
struct FStudioComparisonExportWork;
/** One task publishes the pair atomically as a new directory. Existing
 * destinations, including ones created during the write, are never replaced. */
class FStudioComparisonExportTask
{
public:
    ~FStudioComparisonExportTask();
    bool Start(FStudioComparisonExportRequest Request,const FString& Parent,const FString& Name,FString& Error);
    bool Cancel();
    void Shutdown();
    bool IsBusy() const{return Pending.IsValid();}
    FStudioFieldExportProgress Progress() const;
    TOptional<FStudioComparisonExportResult> Poll();
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void()> BeforePublishForAutomation;
#endif
private:
    TSharedPtr<FStudioComparisonExportWork,ESPMode::ThreadSafe> Work;
    TFuture<FStudioComparisonExportResult> Pending;
    bool bShutdown=false;
};
