#!/usr/bin/env python3
"""Launch one Studio session and reap only its owned children/reporters."""
import datetime
from contextlib import ExitStack
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from studio_processes import REPORTERS, same_process, track_processes
from test_stability import inventory, relevant
from runtime_lane import LaneBusy, lease


def run_owned(command, root):
    try:
        with lease(root, 'Studio session'):
            return _run_owned(command, root)
    except LaneBusy as error:
        print(error, file=sys.stderr)
        return 1


def _run_owned(command, root, *, timeout=None, log_path=None, report_path=None):
    """Run inside the caller's runtime lease, with optional bounded/logged execution.

    The command is an argv array, never shell text. Headless validation shares
    the normal launcher's identity checks, signal handling and reporter cleanup.
    """
    saved = root/'Saved'
    saved.mkdir(parents=True, exist_ok=True)
    with (saved/'.studio-launch.lock').open('a+') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('LBM Solver Studio already has an active launcher.', file=sys.stderr)
            return 1
        before = inventory()
        active = {pid: row for pid, row in relevant(before).items() if Path(row['command']).name not in REPORTERS}
        if active:
            print('A Studio session, Unreal test, or engine build is already running. Wait for it to finish before starting another session.', file=sys.stderr)
            return 1
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
        output = report_path or root/'tmp/debug'/f'launch-{stamp}.json'
        output.parent.mkdir(parents=True, exist_ok=True)
        report = {'processes_before': relevant(before), 'command': command, 'errors': []}
        tracked = {}; proc = None; interrupted = 0
        started = time.time()
        started_monotonic = time.monotonic()

        def interrupt(signum, _):
            nonlocal interrupted
            interrupted = signum

        handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
        for sig in handlers:
            signal.signal(sig, interrupt)

        def track():
            current = inventory()
            track_processes(proc, before, tracked, current, started)
            return current

        code = 1
        streams = ExitStack()
        try:
            log = streams.enter_context(log_path.open('w')) if log_path else None
            proc = subprocess.Popen(command, cwd=root, start_new_session=True,
                                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT if log else None)
            report['pid'] = proc.pid
            output.write_text(json.dumps(report, indent=2)+'\n')
            while True:
                track()
                status = proc.poll()
                if status is not None:
                    code = status if status >= 0 else 128-status
                    break
                if interrupted:
                    code = 128+interrupted
                    break
                if timeout is not None and time.monotonic()-started_monotonic >= timeout:
                    code = 124
                    report['errors'].append(f'Command exceeded {timeout} seconds')
                    break
                time.sleep(.25)
            report['application_exit_code'] = proc.returncode
            report['interrupted_signal'] = interrupted
        except OSError as error:
            report['errors'].append(str(error))
            print(f'Cannot run Studio: {error}', file=sys.stderr)
        finally:
            try:
                if proc:
                    current = track()
                    survivors = {pid: row for pid, row in tracked.items() if same_process(pid, row, current)}
                    report['cleanup_required'] = list(survivors)
                    for pid, row in survivors.items():
                        if same_process(pid, row, inventory()):
                            try:
                                os.kill(pid, signal.SIGTERM)
                            except ProcessLookupError:
                                pass
                    deadline = time.monotonic()+8
                    while time.monotonic()<deadline:
                        proc.poll(); current = inventory()
                        if not any(same_process(pid, row, current) for pid, row in survivors.items()):
                            break
                        time.sleep(.1)
                    for pid, row in survivors.items():
                        if same_process(pid, row, inventory()):
                            try:
                                os.kill(pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                    proc.wait(timeout=10)
                current = inventory()
                report['owned_processes'] = tracked
                report['owned_processes_after'] = {pid: row for pid, row in tracked.items() if same_process(pid, row, current)}
                report['processes_after'] = relevant(current)
                if report['owned_processes_after']:
                    report['errors'].append('Owned processes remain after cleanup')
                report['exit_code'] = code
                report['elapsed_seconds'] = time.monotonic()-started_monotonic
                output.write_text(json.dumps(report, indent=2)+'\n')
                print(f'Studio session report: {output}', flush=True)
            finally:
                streams.close()
                for sig, previous in handlers.items():
                    signal.signal(sig, previous)
        return code or (1 if report['errors'] else 0)


def main():
    root = Path(__file__).resolve().parents[1]
    extra = sys.argv[1:]
    packaged = bool(extra and extra[0] == '--packaged')
    if packaged:
        extra = extra[1:]
        executable = root/'Packaged/Mac/LBMStudio.app/Contents/MacOS/LBMStudio'
        command = [str(executable)]
    else:
        engine = Path(os.environ.get('UE_ENGINE_PATH', '/Users/Shared/Epic Games/UE_5.8'))
        executable = engine/'Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor'
        command = [str(executable), str(root/'LBMStudio.uproject'), '-game']
    if not executable.is_file():
        print(f'Application executable is missing: {executable}', file=sys.stderr)
        return 1
    # Keep LLM active: UE 5.8's startup disable/clear races Cocoa allocations.
    # This launch workaround does not alter the installed engine or app bundle.
    return run_owned([*command, '-LLM', '-windowed', '-ResX=1320', '-ResY=740', '-nosplash', *extra], root)


if __name__ == '__main__':
    raise SystemExit(main())
