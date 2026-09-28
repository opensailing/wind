#!/usr/bin/env python3
"""Serialize Studio builds, native tests and sessions across a workspace family.

The flock is authoritative. Its JSON owner description is diagnostic only; the
file is never deleted because unlinking a held lock creates a second lock lane.
Nested validation workspaces under this repository share its outermost root.
STUDIO_RUNTIME_LANE_PATH can select one explicit lock for unrelated checkouts.
"""
import argparse
from contextlib import contextmanager
import fcntl
import functools
import json
import os
from pathlib import Path
import secrets
import stat
import subprocess
import sys
import time

from studio_processes import process_arguments

FD_ENV = 'STUDIO_RUNTIME_LANE_FD'
TOKEN_ENV = 'STUDIO_RUNTIME_LANE_TOKEN'
PATH_ENV = 'STUDIO_RUNTIME_LANE_PATH'
UNREAL_EXECUTABLES = {'LBMStudio', 'UnrealEditor', 'UnrealEditor-Cmd',
                      'UnrealBuildTool', 'AutomationTool', 'ShaderCompileWorker'}


class LaneBusy(RuntimeError):
    pass


def lock_path(root):
    override = os.environ.get(PATH_ENV)
    if override:
        return Path(override).expanduser().resolve()
    root = Path(root).resolve()
    projects = [p for p in (root, *root.parents) if (p/'LBMStudio.uproject').is_file()]
    return (projects[-1] if projects else root)/'tmp/runtime/ue-execution.lock'


def inherited_descriptor(path):
    """Accept only an inherited descriptor for the same live advisory lock."""
    try:
        fd = int(os.environ.get(FD_ENV, '-1'))
        if fd < 3:
            return None
        actual, expected = os.fstat(fd), path.stat()
        if not stat.S_ISREG(actual.st_mode) or (actual.st_dev, actual.st_ino) != (expected.st_dev, expected.st_ino):
            return None
        owner = json.loads(os.pread(fd, 4096, 0))
        if not isinstance(owner, dict) or not os.environ.get(TOKEN_ENV) or owner.get('token') != os.environ[TOKEN_ENV]:
            return None
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fd
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        return None


@contextmanager
def lease(root, label):
    path = lock_path(root)
    inherited = inherited_descriptor(path)
    if inherited is not None:
        yield inherited
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            try:
                owner = json.loads(os.pread(fd, 4096, 0))
                detail = (f"{owner.get('label', 'Unreal task')} (PID {owner.get('pid', '?')})"
                          if isinstance(owner, dict) else 'another Unreal task')
            except (OSError, ValueError, TypeError):
                detail = 'another Unreal task'
            raise LaneBusy(f'Unreal execution is in use by {detail}. This command did not start. Lock: {path}') from error
        owner = {'pid': os.getpid(), 'label': label, 'root': str(Path(root).resolve()),
                 'started_unix': time.time(), 'token': secrets.token_hex(16)}
        payload = (json.dumps(owner)+'\n').encode()
        os.ftruncate(fd, 0)
        os.pwrite(fd, payload, 0)
        yield fd
    finally:
        # Closing the last inherited descriptor releases the kernel lock even
        # after a crash. Never infer a lease from a PID or stale JSON alone.
        os.close(fd)


def serialized(label):
    """Wrap a Python launch entry point through its entire cleanup/report path."""
    def decorate(function):
        @functools.wraps(function)
        def run(*args, **kwargs):
            root = Path(function.__globals__['__file__']).resolve().parents[1]
            try:
                with lease(root, label):
                    return function(*args, **kwargs)
            except LaneBusy as error:
                print(error, file=sys.stderr)
                return 2
        return run
    return decorate


def is_unreal_work(pid, command):
    name = Path(command).name
    if name in UNREAL_EXECUTABLES:
        return True
    if name in {'dotnet', 'mono'}:
        arguments = process_arguments(pid)
        return any(item in arguments for item in ('/UnrealBuildTool.dll', '/AutomationTool.dll'))
    return False


def existing_unreal_work():
    """Also refuse uncooperative native/build processes already running.

    Read-only inventory. This function never signals a process or adopts an
    unrelated crash reporter. Cooperative commands close the empty-inventory
    launch race by acquiring the lease before this check.
    """
    result = subprocess.run(['ps', '-axo', 'pid=,comm='], check=True, capture_output=True, text=True)
    active = {}
    for line in result.stdout.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        pid, command = int(parts[0]), parts[1]
        if is_unreal_work(pid, command):
            active[pid] = command
    return active


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-inherited', action='store_true')
    parser.add_argument('--label', default='Studio build or test')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.check_inherited:
        return 0 if inherited_descriptor(lock_path(root)) is not None else 1
    command = args.command[1:] if args.command and args.command[0] == '--' else args.command
    if not command:
        parser.error('Provide an argv command after --.')
    inherited = inherited_descriptor(lock_path(root)) is not None
    try:
        with lease(root, args.label) as fd:
            if not inherited:
                active = existing_unreal_work()
                if active:
                    names = ', '.join(f'{Path(command).name} (PID {pid})' for pid, command in active.items())
                    raise LaneBusy(f'Unreal work is already running: {names}. This command did not start; existing processes were left untouched.')
            owner = json.loads(os.pread(fd, 4096, 0))
            environment = dict(os.environ, **{FD_ENV: str(fd), TOKEN_ENV: owner['token'], PATH_ENV: str(lock_path(root))})
            os.set_inheritable(fd, True)
            # Exec keeps the lease through the shell command, including its
            # sequential compiler/cook/test children, without a polling parent.
            os.execvpe(command[0], command, environment)
    except (LaneBusy, OSError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
