#!/usr/bin/env python3
"""Run the explicit 3D volume gate; short phases only rehearse the driver."""
import argparse
import csv
import json
from pathlib import Path
import statistics
import subprocess
import sys


def analyze(path, seconds):
    with Path(path).open(encoding='utf-8-sig') as f:
        samples=[{k:float(v) for k,v in row.items()} for row in csv.DictReader(f)]
    if not samples:return {'errors':['No volume resource telemetry']}
    mib=1024**2
    budgets={'footprint_bytes':6144*mib,'mesh_bytes':128*mib,'workers':1,'sections':8,
             'scalar_texture_bytes':17*mib,'point_value_bytes':64*mib,'point_readers':3,
             'rhi_bytes':1024*mib,'device_allocated_bytes':2048*mib}
    peaks={k:max(r[k] for r in samples) for k in budgets}
    errors=[f'{k} exceeds budget: {peaks[k]} > {v}' for k,v in budgets.items() if peaks[k]>v]
    if any(r['device_allocated_bytes']<=0 for r in samples):errors.append('Native device memory unavailable')
    phases={}
    for phase in (1,2,4):
        rows=[r for r in samples if r['phase']==phase]
        if len(rows)<3 or max(r['phase_s'] for r in rows)<seconds-12:
            errors.append(f'Phase {phase} lacks required duration/samples');continue
        warm=[r for r in rows if r['phase_s']>=seconds*.2];width=max(1,len(warm)//3)
        drift={k:statistics.mean(r[k] for r in warm[-width:])-statistics.mean(r[k] for r in warm[:width])
               for k in ('footprint_bytes','rhi_bytes','rhi_count','device_allocated_bytes')}
        if drift['footprint_bytes']>(128 if phase==4 else 256)*mib:errors.append(f'Phase {phase}: retained process growth')
        if drift['rhi_bytes']>128*mib or drift['rhi_count']>256 or drift['device_allocated_bytes']>128*mib:
            errors.append(f'Phase {phase}: retained graphics resource growth')
        phases[phase]={'seconds':max(r['phase_s'] for r in rows),'samples':len(rows),'retained_drift':drift}
        if phase in (1,4) and any(r['source_frames']!=5901 or r['volume_attached']!=1 or r['volume_enabled']!=1 or r['scalar_texture_bytes']<=0 for r in rows):
            errors.append(f'Phase {phase}: full original volume missing')
        if phase==4:
            if len({r['captures'] for r in rows})!=1:errors.append('Idle/minimized captures continued')
            if {r['minimized'] for r in rows}!={0,1}:errors.append('Visible and minimized idle were not both measured')
        if phase==2 and {r['source_slot'] for r in rows}!={0,1,2}:errors.append('Mixed use did not exercise all source types')
    if seconds>=1200 and max(r['playback_loops'] for r in samples)<1:errors.append('No complete playback loop')
    return {'errors':errors,'passed':not errors,'acceptance':'60-minute volume stability' if seconds>=1200 else 'driver rehearsal only',
            'samples':len(samples),'budgets':budgets,'peaks':peaks,'phases':phases,
            'metric_notes':'footprint is macOS phys_footprint; device allocation is Metal currentAllocatedSize; zero RHI counts may indicate unavailable RHI telemetry.'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('recording',type=Path);p.add_argument('reconstruction',type=Path)
    p.add_argument('--phase-seconds',type=int,default=1200)
    args=p.parse_args();root=Path(__file__).resolve().parents[1]
    cmd=[sys.executable,str(root/'Tools/run_packaged_suite.py'),'--suite','Studio.VolumeStability.MixedUse','--count','1',
         '--name','volume-stability','--captures','VolumeStability','--timeout',str(args.phase_seconds*3+650),
         '--volume-recording',str(args.recording.resolve()),'--volume-reconstruction',str(args.reconstruction.resolve()),
         '--volume-phase-seconds',str(args.phase_seconds)]
    previous=set((root/'tmp/debug').glob('volume-stability-*'))
    result=subprocess.run(cmd)
    added=set((root/'tmp/debug').glob('volume-stability-*'))-previous
    if len(added)!=1:return result.returncode or 1
    report=added.pop();resources=report/'captures/resources.csv'
    if not resources.exists():return result.returncode or 1
    analysis=analyze(resources,args.phase_seconds)
    (report/'analysis.json').write_text(json.dumps(analysis,indent=2)+'\n')
    print(json.dumps({'report':str(report),**analysis},indent=2),flush=True)
    return result.returncode or (0 if analysis['passed'] else 1)


if __name__=='__main__':raise SystemExit(main())
