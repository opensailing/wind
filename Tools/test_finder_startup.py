#!/usr/bin/env python3
"""Verify packaged startup through LaunchServices, without a supplied -LLM.

This uses the Finder launch route, with bounded automation arguments to close
the owned app. It does not claim a physical Finder double-click or native input.
"""
import datetime
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import sys
import time
from types import SimpleNamespace

from studio_processes import REPORTERS, process_arguments, same_process, track_processes
from test_stability import inventory, relevant, sha256
from runtime_lane import serialized


def find_launched_process(before, current, binary, token, arguments=process_arguments):
    """Match a launch by its executable and unique token, never by PID alone.

    Match the actual executable and invocation token independently of the PID
    returned by LaunchServices; never adopt another process with a similar name.
    """
    candidates = [(pid, row) for pid, row in current.items()
                  if pid > 0 and pid not in before
                  and Path(row['command']).resolve() == binary.resolve()
                  and token in arguments(pid).split()]
    if len(candidates) > 1:
        raise ValueError('More than one process carries this unique launch identity')
    return candidates[0] if candidates else None


@serialized('Studio LaunchServices startup test')
def main():
    root = Path(__file__).resolve().parents[1]
    app = root/'Packaged/Mac/LBMStudio.app'
    binary = app/'Contents/MacOS/LBMStudio'
    before = inventory()
    if any(Path(row['command']).name not in REPORTERS for row in relevant(before).values()):
        raise SystemExit('Close Studio/Unreal before LaunchServices acceptance. Existing processes were left untouched.')
    bundle = plistlib.loads((app/'Contents/Info.plist').read_bytes())['CFBundleIdentifier']
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = root/'tmp/debug'/f'finder-startup-{stamp}'
    output.mkdir(parents=True)
    saved = Path.home()/'Library/Containers'/bundle/'Data/Library/Application Support/Epic/LBMStudio/Saved'
    report = saved/'Automation'/f'finder-startup-{stamp}'
    log = saved/'Logs'/f'finder-startup-{stamp}.log'
    source = output/'launch.m'
    source.write_text('''#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, const char** argv) { @autoreleasepool {
    if (argc < 2) return 2;
    NSURL* url=[NSURL fileURLWithPath:@(argv[1])];
    NSMutableArray* args=[NSMutableArray array];
    for (int i=2;i<argc;++i) [args addObject:@(argv[i])];
    NSWorkspaceOpenConfiguration* config=[NSWorkspaceOpenConfiguration configuration];
    config.arguments=args; config.createsNewApplicationInstance=YES;
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:url configuration:config
        completionHandler:^(NSRunningApplication* app, NSError* error) {
            if (!app || error) { fprintf(stderr,"%s\\n",error.localizedDescription.UTF8String); exit(1); }
            printf("%d\\n",app.processIdentifier); fflush(stdout); exit(0);
        }];
    dispatch_main();
} }
''')
    helper = output/'launch'
    subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', '-fobjc-arc',
                    '-framework', 'AppKit', str(source), '-o', str(helper)], check=True)
    arguments = ['-windowed', '-ResX=1320', '-ResY=740', '-unattended', '-StudioAutomation',
                 f'-StudioLaunchToken={stamp}',
                 f'-abslog={log}', '-ExecCmds=Automation RunTests Studio.Orientation.',
                 '-TestExit=Automation Test Queue Empty', f'-ReportExportPath={report}']
    manifest = {'binary_sha256': sha256(binary), 'launch': 'NSWorkspace / LaunchServices',
                'arguments': arguments, 'processes_before': relevant(before), 'errors': []}
    tracked = {}
    owner = None
    identity = None
    started_wall = time.time()
    started = time.monotonic()
    try:
        launched = subprocess.run([str(helper), str(app), *arguments], capture_output=True, text=True,
                                  timeout=30, check=True)
        manifest['launchservices_pid'] = int(launched.stdout.strip())
        deadline = time.monotonic()+10
        while time.monotonic() < deadline:
            match = find_launched_process(before, inventory(), binary, f'-StudioLaunchToken={stamp}')
            if match:
                pid, identity = match
                owner = SimpleNamespace(pid=pid, returncode=None)
                break
            time.sleep(.05)
        if not owner:
            raise ValueError('LaunchServices did not produce a verifiable application process')
        manifest['pid'] = owner.pid
        manifest['process_identity'] = identity
        (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        print(f'LaunchServices startup: PID {owner.pid}; reports {output}', flush=True)
        while True:
            current = inventory()
            if not identity or not same_process(owner.pid, identity, current):
                owner.returncode = 0
                track_processes(owner, before, tracked, current, started_wall)
                break
            track_processes(owner, before, tracked, current, started_wall)
            if time.monotonic()-started > 120:
                raise TimeoutError('LaunchServices acceptance exceeded its deadline')
            time.sleep(.25)
        if log.is_file():
            shutil.copy2(log, output/'application.log')
            content = log.read_text(errors='replace')
            if 'LLM enabled CsvWriter: off TraceWriter: off' not in content:
                manifest['errors'].append('Engine did not confirm ordinary memory tracking')
            if '-LLM' not in content:
                manifest['errors'].append('Engine command line does not include bootstrap argument')
        else:
            manifest['errors'].append('Application log missing')
        if (report/'index.json').is_file():
            shutil.copy2(report/'index.json', output/'automation.json')
            result = json.loads((output/'automation.json').read_text(encoding='utf-8-sig'))
            if result['succeeded'] != 3 or any(result.get(key, 0) for key in ('failed','notRun','succeededWithWarnings')):
                manifest['errors'].append('Expected three clean orientation suites')
        else:
            manifest['errors'].append('Automation report missing')
    except (OSError, ValueError, TimeoutError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        manifest['errors'].append(str(error) or 'Interrupted')
    finally:
        current = inventory()
        # A helper failure can occur after LaunchServices accepts the request.
        # Adopt only a new exact executable with this invocation's unique token.
        if not owner:
            try:
                match = find_launched_process(before, current, binary, f'-StudioLaunchToken={stamp}')
            except ValueError as error:
                manifest['errors'].append(str(error))
                match = None
            if match:
                pid, identity = match
                owner = SimpleNamespace(pid=pid, returncode=None)
                manifest['pid'] = pid
        if owner:
            if not identity or not same_process(owner.pid, identity, current):
                owner.returncode = 0
            track_processes(owner, before, tracked, current, started_wall)
        survivors = {pid: row for pid,row in tracked.items() if same_process(pid,row,current)}
        if survivors:
            manifest['errors'].append('Owned processes required cleanup')
            for pid,row in survivors.items():
                if same_process(pid,row,inventory()):
                    try: os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError: pass
            deadline = time.monotonic()+8
            while time.monotonic()<deadline:
                current = inventory()
                if not any(same_process(pid,row,current) for pid,row in survivors.items()): break
                time.sleep(.1)
            for pid,row in survivors.items():
                if same_process(pid,row,inventory()):
                    try: os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError: pass
        current = inventory()
        manifest['owned_processes_after'] = {pid:row for pid,row in tracked.items() if same_process(pid,row,current)}
        manifest['processes_after'] = relevant(current)
        manifest['elapsed_seconds'] = time.monotonic()-started
        manifest['passed'] = not manifest['errors'] and not manifest['owned_processes_after']
        (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'passed':manifest['passed'], 'report':str(output), 'errors':manifest['errors']},indent=2))
    return 0 if manifest['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
