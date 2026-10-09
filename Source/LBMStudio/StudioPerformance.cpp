#include "StudioPerformance.h"
#include "StudioScene.h"
#include "HAL/PlatformProcess.h"
#include "RHI.h"
#if PLATFORM_MAC
#include <mach/mach.h>
#include <sys/resource.h>
#endif

bool FStudioPerformanceHistory::Observe(double Now,double UpdateMs,TFunctionRef<FStudioApplicationCounters()> Read)
{
    if(!FMath::IsFinite(Now)||Now<0||!FMath::IsFinite(UpdateMs)||UpdateMs<0||Now<=LastTick)return false;
    if(LastTick<0||Now-LastTick>2.)
    {LastTick=WindowStart=Now;Ticks=0;UpdateTotal=0;bContinuous=false;return false;}
    LastTick=Now;++Ticks;UpdateTotal+=UpdateMs;
    const double Duration=Now-WindowStart;
    if(Duration<1.)return false;
    FStudioPerformanceSample S;S.At=Now;S.IntervalSeconds=Duration;
    S.UICadenceMs=Duration*1000./Ticks;S.UIUpdateMs=UpdateTotal/Ticks;S.Counters=Read();
    if(bContinuous&&!History.IsEmpty())
    {
        const auto& P=History.Last();const double Elapsed=Now-P.At;
        if(P.Counters.ProcessCPUSeconds&&S.Counters.ProcessCPUSeconds&&
           FMath::IsFinite(*S.Counters.ProcessCPUSeconds)&&*S.Counters.ProcessCPUSeconds>=*P.Counters.ProcessCPUSeconds)
            S.ProcessCPUPercent=(*S.Counters.ProcessCPUSeconds-*P.Counters.ProcessCPUSeconds)/Elapsed*100.;
        if(P.Counters.Captures&&S.Counters.Captures&&*S.Counters.Captures>=*P.Counters.Captures)
            S.CapturesPerSecond=double(*S.Counters.Captures-*P.Counters.Captures)/Elapsed;
    }
    if(S.ProcessCPUPercent&&!FMath::IsFinite(*S.ProcessCPUPercent))S.ProcessCPUPercent.Reset();
    History.Add(MoveTemp(S));if(History.Num()>120)History.RemoveAt(0,History.Num()-120,EAllowShrinking::No);
    WindowStart=Now;Ticks=0;UpdateTotal=0;bContinuous=true;return true;
}

FStudioApplicationCounters StudioPerformance::ReadCounters(const AStudioScene* Scene)
{
    FStudioApplicationCounters C;C.Host=FPlatformProcess::ComputerName();C.Device=GRHIAdapterName;
#if PLATFORM_MAC
    // Query this process directly so a failed read is unavailable, never an
    // old cached platform value. phys_footprint includes compressed memory.
    task_vm_info_data_t VM{};mach_msg_type_number_t Count=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&VM),&Count)==KERN_SUCCESS)
        C.FootprintBytes=uint64(VM.phys_footprint);
    rusage Usage{};
    if(getrusage(RUSAGE_SELF,&Usage)==0)
        C.ProcessCPUSeconds=double(Usage.ru_utime.tv_sec)+double(Usage.ru_stime.tv_sec)+
            (double(Usage.ru_utime.tv_usec)+double(Usage.ru_stime.tv_usec))/1.e6;
#endif
    if(Scene)
    {
        C.Captures=Scene->GetCaptureCount();C.LastCaptureSubmitMs=Scene->LastCaptureSubmitMilliseconds();
        const auto Stats=Scene->ResourceStats();C.Workers=Stats.Workers;C.MeshBytes=Stats.MeshBytes;
        C.TextureBytes=Stats.RenderTargetBytes+Stats.ScalarTextureBytes;
        if(Scene->HasPresentedFrame())
        {
            C.LastFieldBuildMs=Scene->PresentedBuildMilliseconds();C.BuildSource=Scene->PresentedSource();
            C.BuildFrame=Scene->PresentedFrame().Index;
        }
    }
    return C;
}
