#!/usr/bin/env python3
"""Run one packaged acceptance suite, retain evidence, and reap owned processes."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import time

from test_stability import inventory, relevant
from studio_processes import same_process, track_processes
from runtime_lane import serialized


@serialized('Packaged acceptance')
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', required=True)
    parser.add_argument('--count', type=int, required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--width', type=int, default=1320)
    parser.add_argument('--height', type=int, default=740)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--captures')
    parser.add_argument('--startup-profile', choices=('explicit-tracking', 'application-default'),
                        default='explicit-tracking',
                        help='application-default verifies the packaged pre-main startup without a supplied -LLM')
    parser.add_argument('--point-recording', type=Path,
                        help='Full original recording.json for an explicit point-data reader or viewport gate')
    parser.add_argument('--surface-reconstruction', type=Path,
                        help='Audited reconstruction.json for the full surface playback gate')
    parser.add_argument('--volume-recording', type=Path)
    parser.add_argument('--volume-reconstruction', type=Path)
    parser.add_argument('--volume-phase-seconds', type=int, default=1200)
    parser.add_argument('--residual-log', type=Path,
                        help='Original published OpenFOAM log for the complete residual reader audit')
    args = parser.parse_args()
    full_surface_gate = args.suite in {'ScientificAcceptance.Surface.FullSequence', 'ScientificAcceptance.Inspection.FullSequence'}
    full_point_gate = args.suite in {'ScientificAcceptance.PointRecording.FullSequence',
                                    'ScientificAcceptance.PointRecording.ViewportFullSequence',
                                    'ScientificAcceptance.PointRecording.ViewportControls'} or full_surface_gate
    residual_gate = args.suite == 'ScientificAcceptance.Residuals.PublishedFullLog'
    if (not args.suite.startswith('Studio.') and not full_point_gate and not residual_gate) or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._' for c in args.suite):
        parser.error('Use one Studio automation suite prefix.')
    extra = []
    if residual_gate != bool(args.residual_log):
        parser.error('The published residual audit requires --residual-log; other suites do not accept it.')
    if args.residual_log:
        args.residual_log = args.residual_log.expanduser().resolve()
        if not args.residual_log.is_file() or any(ord(c)<32 or c=='"' for c in str(args.residual_log)):
            parser.error('Choose an existing original log with a plain local path.')
    volume_gate = args.suite == 'Studio.VolumeStability.MixedUse'
    if volume_gate != bool(args.volume_recording and args.volume_reconstruction):
        parser.error('Volume stability requires both original recording and reconstruction paths.')
    if not volume_gate and (args.volume_recording or args.volume_reconstruction):
        parser.error('Volume source arguments are only valid for volume stability.')
    if volume_gate:
        if args.volume_phase_seconds < 30:
            parser.error('Volume phases must last at least 30 seconds.')
        args.volume_recording = args.volume_recording.expanduser().resolve()
        args.volume_reconstruction = args.volume_reconstruction.expanduser().resolve()
        for name, option, filename in [('StudioVolumeRecording', args.volume_recording, 'recording.json'),
                                       ('StudioVolumeReconstruction', args.volume_reconstruction, 'reconstruction.json')]:
            if not option.is_file() or option.name != filename or any(ord(c)<32 or c=='"' for c in str(option)):
                parser.error('Expected an existing volume source file with a plain path.')
            extra.append(f'-{name}={option}')
        extra += ['-StudioVolumeSoak', f'-StudioVolumePhaseSeconds={args.volume_phase_seconds}']
    if full_point_gate != bool(args.point_recording):
        parser.error('The full point-data gate requires --point-recording; other suites do not accept it.')
    if args.point_recording:
        args.point_recording = args.point_recording.expanduser().resolve()
        if (not args.point_recording.is_file() or args.point_recording.name != 'recording.json'
                or any(ord(c) < 32 or c == '"' for c in str(args.point_recording))):
            parser.error('Choose an existing local recording.json with a plain path.')
        # Popen passes one argument; Unreal's Mac launcher quotes values with
        # spaces itself. Literal quotes here would be doubled and parsed as travel.
        extra.append(f'-StudioPointRecording={args.point_recording}')
    if full_surface_gate != bool(args.surface_reconstruction):
        parser.error('Only the full surface gate requires --surface-reconstruction.')
    if args.surface_reconstruction:
        args.surface_reconstruction = args.surface_reconstruction.expanduser().resolve()
        if (not args.surface_reconstruction.is_file() or args.surface_reconstruction.name != 'reconstruction.json'
                or any(ord(c) < 32 or c == '"' for c in str(args.surface_reconstruction))):
            parser.error('Choose an existing local reconstruction.json with a plain path.')
        extra.append(f'-StudioSurfaceReconstruction={args.surface_reconstruction}')
    if not args.name or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in args.name):
        parser.error('Report name must use lowercase letters, digits, or hyphens.')
    if args.captures and (Path(args.captures).name != args.captures or args.captures in {'.', '..'}):
        parser.error('Capture directory must be a single directory name.')
    if args.count < 1 or args.timeout < 10 or not (640 <= args.width <= 7680 and 480 <= args.height <= 4320):
        parser.error('Invalid test count, timeout or viewport size.')
    root = Path(__file__).resolve().parents[1]
    app = root/'Packaged/Mac/LBMStudio.app'
    binary = app/'Contents/MacOS/LBMStudio'
    if not binary.is_file():
        parser.error('Run Tools/package.sh first.')
    before = inventory()
    active = {pid: item for pid, item in relevant(before).items()
              if Path(item['command']).name not in {'CrashReportClient', 'CrashReportClientEditor'}}
    if active:
        parser.error('Close LBMStudio and Unreal Editor before packaged acceptance. Existing processes were left untouched.')
    with (app/'Contents/Info.plist').open('rb') as stream:
        bundle = plistlib.load(stream)['CFBundleIdentifier']
    saved = Path.home()/'Library/Containers'/bundle/'Data/Library/Application Support/Epic/LBMStudio/Saved'
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = root/'tmp/debug'/f'{args.name}-{stamp}'
    output.mkdir(parents=True)
    report = saved/'Automation'/f'{args.name}-{stamp}'
    residual_input = None
    startup_arguments = ['-LLM'] if args.startup_profile == 'explicit-tracking' else []
    manifest = {'suite': args.suite, 'expected_count': args.count, 'viewport': [args.width, args.height],
                'binary_sha256': hashlib.file_digest(binary.open('rb'), 'sha256').hexdigest(),
                'startup_profile': args.startup_profile, 'startup_arguments': startup_arguments,
                'processes_before': relevant(before), 'errors': []}
    if volume_gate:
        manifest['volume_phase_seconds'] = args.volume_phase_seconds
        for name,path in [('volume_recording',args.volume_recording),('volume_reconstruction',args.volume_reconstruction)]:
            with path.open('rb') as source:
                manifest[name]={'path':str(path.resolve()),'sha256':hashlib.file_digest(source,'sha256').hexdigest()}
    if args.point_recording:
        with args.point_recording.open('rb') as source:
            manifest['point_recording'] = {'path': str(args.point_recording),
                                          'metadata_sha256': hashlib.file_digest(source, 'sha256').hexdigest()}
    if args.residual_log:
        with args.residual_log.open('rb') as source:
            manifest['residual_log'] = {'path': str(args.residual_log),
                                        'source_sha256': hashlib.file_digest(source, 'sha256').hexdigest()}
        # Reader acceptance stages an exact copy inside its sandbox. This does
        # not claim native picker/security-bookmark acceptance for source import.
        if not 0 < args.residual_log.stat().st_size <= 256 * 1024 * 1024:
            parser.error('The residual audit source must be nonempty and at most 256 MiB.')
        residual_input = saved/'Automation'/f'residual-input-{stamp}'
        residual_input.mkdir(parents=True)
        staged_log = residual_input/'source.log'
        shutil.copyfile(args.residual_log, staged_log)
        with staged_log.open('rb') as source:
            staged_hash = hashlib.file_digest(source, 'sha256').hexdigest()
        if staged_hash != manifest['residual_log']['source_sha256']:
            shutil.rmtree(residual_input)
            parser.error('Residual source changed while staging.')
        manifest['residual_log']['staged_sha256'] = staged_hash
        manifest['residual_log']['access_scope'] = 'Exact copy in owned sandbox directory; native import picker not exercised'
        extra.append(f'-StudioResidualLog={staged_log}')
    if args.surface_reconstruction:
        with args.surface_reconstruction.open('rb') as source:
            manifest['surface_reconstruction'] = {'path': str(args.surface_reconstruction),
                                                  'metadata_sha256': hashlib.file_digest(source, 'sha256').hexdigest()}
    tracked = {}
    proc = None
    started = time.monotonic()
    started_wall = time.time()

    def track():
        current = inventory()
        track_processes(proc, before, tracked, current, started_wall)
        return current

    try:
        with (output/'application.log').open('w') as log:
            # UE 5.8 can clear LLM thread state while Cocoa still has an active
            # allocation scope at startup. Retaining tracking avoids that clear;
            # include its overhead in every resource acceptance measurement.
            proc = subprocess.Popen([str(binary), *startup_arguments, '-windowed', f'-ResX={args.width}', f'-ResY={args.height}',
                                     '-unattended', '-stdout', '-FullStdOutLogOutput', '-StudioAutomation',
                                     f'-ExecCmds=Automation RunTests {args.suite}', '-TestExit=Automation Test Queue Empty',
                                     f'-ReportExportPath={report}', *extra], stdin=subprocess.DEVNULL, stdout=log,
                                    stderr=subprocess.STDOUT, start_new_session=True)
            manifest['pid'] = proc.pid
            (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
            print(f'Running {args.suite}: PID {proc.pid}; reports {output}', flush=True)
            while True:
                track()
                code = proc.poll()
                if code is not None:
                    manifest['exit_code'] = code
                    if code:
                        manifest['errors'].append(f'Application exited {code}')
                    break
                if time.monotonic()-started > args.timeout:
                    raise TimeoutError('Packaged suite exceeded its deadline')
                time.sleep(.5)
    except (KeyboardInterrupt, TimeoutError, OSError) as error:
        manifest['errors'].append(str(error) or 'Interrupted')
    finally:
        if proc:
            current = track()
            survivors = {pid: info for pid, info in tracked.items() if same_process(pid, info, current)}
            if survivors:
                manifest['errors'].append('Owned processes required cleanup')
                for pid in survivors:
                    try:
                        os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                deadline = time.monotonic()+8
                while time.monotonic() < deadline:
                    proc.poll()
                    current = inventory()
                    if not any(same_process(pid, info, current) for pid, info in survivors.items()):
                        break
                    time.sleep(.2)
                current = inventory()
                for pid, info in survivors.items():
                    if same_process(pid, info, current):
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
            proc.wait(timeout=10)
        current = inventory()
        manifest['owned_processes_after'] = {pid: info for pid, info in tracked.items() if same_process(pid, info, current)}
        manifest['processes_after'] = relevant(current)
        manifest['elapsed_seconds'] = time.monotonic()-started
        if manifest['owned_processes_after']:
            manifest['errors'].append('Owned processes remain after cleanup')
        if args.startup_profile == 'application-default':
            log_text = (output/'application.log').read_text(errors='replace')
            if log_text.count('Studio startup: retaining memory tracking before engine initialization.') != 1:
                manifest['errors'].append('Expected exactly one pre-main tracking bootstrap')
            if 'LLM enabled CsvWriter: off TraceWriter: off' not in log_text:
                manifest['errors'].append('Engine did not confirm ordinary memory tracking')
        if (report/'index.json').is_file():
            shutil.copy2(report/'index.json', output/'automation.json')
            result = json.loads((output/'automation.json').read_text(encoding='utf-8-sig'))
            if result['succeeded'] != args.count or result['failed'] or result.get('notRun', 0) or result.get('succeededWithWarnings', 0):
                manifest['errors'].append('Automation did not finish the expected clean suites')
        else:
            manifest['errors'].append('Automation report missing')
        if args.captures:
            target = output/'captures'
            target.mkdir(exist_ok=True)
            for source in (saved/'Automation'/args.captures).glob('*'):
                if source.suffix not in {'.png', '.csv'}:
                    continue
                if source.stat().st_mtime >= started_wall:
                    shutil.copy2(source, target/source.name)
        manifest['passed'] = not manifest['errors']
        (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        if residual_input is not None:
            shutil.rmtree(residual_input)
    print(json.dumps({'passed': manifest['passed'], 'report': str(output), 'errors': manifest['errors']}, indent=2), flush=True)
    return 0 if manifest['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
