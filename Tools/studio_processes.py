"""Identify a launched Studio process's detached macOS crash reporters.

UE's MacPlatformMisc launches CrashReportClient with POSIX_SPAWN_SETPGROUP.
Parent/group tracking alone can miss it after a fast crash and reparenting.
The engine passes a report directory containing project, owner PID and run UUID.
Only new reporters with that exact identity can be adopted for cleanup.
"""
import datetime
import os
from pathlib import Path
import re
import subprocess

REPORTERS = {'CrashReportClient', 'CrashReportClientEditor'}


def same_process(pid, expected, current):
    actual = current.get(pid)
    return bool(actual and actual['started'] == expected['started'] and actual['command'] == expected['command'])


def track_processes(proc, before, tracked, current, started_wall):
    """Extend owned identities through verified ancestry, group or crash data."""
    for pid, item in detached_reporters(current, before, proc.pid, started_wall).items():
        if pid not in tracked or not same_process(pid, tracked[pid], current):
            tracked[pid] = item
    owned = {pid for pid, item in tracked.items() if same_process(pid, item, current)}
    # An unreaped direct child cannot have had its PID reassigned. Once reaped,
    # only surviving verified identities may confer ownership on descendants.
    if proc.returncode is None:
        owned.add(proc.pid)
    for _ in range(3):
        for pid, item in current.items():
            if pid in before and pid != proc.pid:
                continue
            in_group = False
            if pid not in owned and item['parent'] not in owned:
                try:
                    in_group = os.getpgid(pid) == proc.pid
                except (ProcessLookupError, PermissionError):
                    pass
            if pid in owned or item['parent'] in owned or in_group:
                if pid not in tracked or not same_process(pid, tracked[pid], current):
                    tracked[pid] = item
                owned.add(pid)


def process_arguments(pid):
    try:
        return subprocess.run(['ps', '-ww', '-p', str(pid), '-o', 'command='],
                              check=True, text=True, capture_output=True).stdout.strip()
    except subprocess.CalledProcessError:
        # A reporter can finish between the inventory and argument query.
        return ''


def detached_reporters(current, before, owner_pid, started_wall):
    """Return reporter identities owned by this launch, never preexisting ones.

    ps timestamps have one-second precision. Command and start time remain in
    each identity so the caller can recheck PID reuse immediately before a kill.
    Do not infer ownership from the executable name or launch time alone.
    """
    report = re.compile(r'/((?:CrashReport|EnsureReport)-UE-LBMStudio-pid-'
                        + re.escape(str(owner_pid)) + r'-([0-9A-Fa-f]{32}))/?(?=$|[\s\"\'])')
    owned = {}
    for pid, item in current.items():
        if pid in before or Path(item['command']).name not in REPORTERS:
            continue
        try:
            birth = datetime.datetime.strptime(item['started'], '%a %b %d %H:%M:%S %Y').timestamp()
        except (KeyError, ValueError):
            continue
        if birth < int(started_wall):
            continue
        match = report.search(process_arguments(pid))
        if match:
            owned[pid] = {**item, 'crash_report_owner_pid': owner_pid,
                          'crash_report_run_uuid': match[2].lower()}
    return owned
