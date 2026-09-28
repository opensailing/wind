#!/usr/bin/env python3
"""Run one owned packaged process and preserve a mixed-use acceptance report.

1200 seconds per phase is the M1 gate. Short runs are driver rehearsals.
GPU counters cover allocations tracked by Unreal's RHI, not all Metal
driver memory. macOS UsedPhysical is phys_footprint, not resident size.
"""
import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import signal
import statistics
import subprocess
import sys
import time
from studio_processes import same_process, track_processes
from runtime_lane import is_unreal_work, serialized


def inventory():
    result = subprocess.run(["ps", "-axo", "pid=,ppid=,lstart=,comm="], check=True, text=True, capture_output=True)
    rows = {}
    for line in result.stdout.splitlines():
        parts = line.split(None, 7)
        if len(parts) == 8:
            rows[int(parts[0])] = {"parent": int(parts[1]), "started": " ".join(parts[2:7]), "command": parts[7]}
    return rows


def relevant(rows):
    return {pid: item for pid, item in rows.items()
            if Path(item["command"]).name in {"CrashReportClient", "CrashReportClientEditor"}
            or is_unreal_work(pid, item["command"])}


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def analyze(path, phase_seconds, require_point=False, require_surface=False):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        samples = [{k: float(v) for k, v in row.items()} for row in csv.DictReader(stream)]
    errors = []
    if not samples:
        return {"errors": ["No resource samples"]}
    mib = 1024**2
    budgets = {"footprint_bytes": 6*1024*mib, "mesh_bytes": 128*mib, "live_frame_bytes": 25*mib,
               "live_readers": 3, "workers": 1, "sections": 7, "rhi_bytes": 1024*mib, "device_allocated_bytes": 2048*mib}
    peaks = {key: max(row[key] for row in samples) for key in budgets}
    for key, limit in budgets.items():
        if peaks[key] > limit:
            errors.append(f"{key} exceeded budget: {peaks[key]} > {limit}")
    if any(row["device_allocated_bytes"] <= 0 for row in samples):
        errors.append("Native device allocation metric unavailable")
    phases = {}
    for phase in (1, 2, 4):
        rows = [r for r in samples if r["phase"] == phase]
        if len(rows) < 3 or max(r["phase_s"] for r in rows) < phase_seconds-12:
            errors.append(f"Insufficient duration/samples for phase {phase}")
            continue
        warm = [r for r in rows if r["phase_s"] >= phase_seconds*.2]
        width = max(1, len(warm)//3)
        drift = {key: statistics.mean(r[key] for r in warm[-width:])-statistics.mean(r[key] for r in warm[:width])
                 for key in ("footprint_bytes", "rhi_bytes", "rhi_count", "device_allocated_bytes")}
        # Explicit acceptance tolerances after warm-up. Short rehearsals exercise
        # the checks but cannot demonstrate a long-term allocation plateau.
        if drift["footprint_bytes"] > (128 if phase == 4 else 256)*mib:
            errors.append(f"Phase {phase} retained footprint grew beyond tolerance")
        if drift["rhi_bytes"] > 128*mib or drift["rhi_count"] > 256:
            errors.append(f"Phase {phase} tracked RHI resources grew beyond tolerance")
        if drift["device_allocated_bytes"] > 128*mib:
            errors.append(f"Phase {phase} native device allocations grew beyond tolerance")
        phases[str(phase)] = {"samples": len(rows), "observed_seconds": max(r["phase_s"] for r in rows), "retained_drift": drift}
        if phase == 4:
            if len({r["captures"] for r in rows}) != 1:
                errors.append("Idle/minimized phase submitted additional captures")
            if not any(r["minimized"] for r in rows) or not any(not r["minimized"] for r in rows):
                errors.append("Both visible-idle and native-minimized intervals are required")
    source_coverage = {}
    if require_point:
        required = {'source_slot', 'source_frames', 'point_readers', 'point_value_bytes', 'playback_loops'}
        if not required.issubset(samples[0]):
            errors.append('Point-source telemetry missing; this build cannot accept point stability')
        else:
            for phase in (1, 2, 4):
                rows = [r for r in samples if r['phase'] == phase]
                slots = sorted({int(r['source_slot']) for r in rows})
                source_coverage[str(phase)] = slots
                if phase in (1, 4) and (slots != [2] or any(r['source_frames'] != 8000 for r in rows)):
                    errors.append(f'Phase {phase} did not retain the full point recording')
                if phase == 2 and slots != [0, 1, 2]:
                    errors.append('Mixed phase did not exercise both legacy sources and the point source')
            if not any(r['point_readers'] > 0 and r['point_value_bytes'] > 0 for r in samples):
                errors.append('No live point reader/value allocations measured')
            if phase_seconds >= 1200 and max(r['playback_loops'] for r in samples) < 1:
                errors.append('Sustained playback never completed a full point-source loop')
    if require_surface:
        required = {'surface_attached', 'surface_enabled', 'scalar_texture_bytes', 'points_enabled', 'frame_current', 'vertices'}
        if not required.issubset(samples[0]):
            errors.append('Surface telemetry missing; this build cannot accept reconstructed-surface stability')
        else:
            if any(r['scalar_texture_bytes'] > mib for r in samples):
                errors.append('Scalar texture exceeded 1 MiB budget')
            for phase in (1, 4):
                rows = [r for r in samples if r['phase'] == phase]
                if not rows or any(r['surface_attached'] != 1 or r['surface_enabled'] != 1 or r['scalar_texture_bytes'] <= 0 for r in rows):
                    errors.append(f'Phase {phase} did not retain a rendered reconstructed surface')
            mixed = [r for r in samples if r['phase'] == 2 and r['source_slot'] == 2]
            if not mixed or any(r['surface_attached'] != 1 for r in mixed):
                errors.append('Mixed phase lost reconstruction after source replacement')
            # The full pinned source has 18,706 points, each rendered as three
            # crossed quads (18 vertices). A hidden surface alone is not evidence
            # of an original-point display. Require settled, allocated geometry.
            points = any(r['surface_enabled'] == 0 and r['points_enabled'] == 1
                         and r['frame_current'] == 1 and r['scalar_texture_bytes'] == 0
                         and r['vertices'] >= 18706*18 for r in mixed)
            surface = any(r['surface_enabled'] == 1 and r['frame_current'] == 1
                          and r['scalar_texture_bytes'] > 0 for r in mixed)
            if not points or not surface:
                errors.append('Mixed phase did not measure both surface and original-point representations')
    return {"samples": len(samples), "budgets": budgets, "peaks": peaks, "phases": phases, "errors": errors,
            "source_coverage": source_coverage,
            "rhi_resource_info_available": any(row['rhi_count'] > 0 for row in samples),
            "metric_notes": "footprint_bytes is macOS phys_footprint; device_allocated_bytes is MTLDevice.currentAllocatedSize; RHI resources may be unavailable on Metal and zero does not mean zero allocations; mesh_bytes is retained CPU buffers; cache and live frame counts are separate."}


@serialized('Studio stability test')
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase_seconds', type=int, nargs='?', default=1200)
    parser.add_argument('--point-recording', type=Path,
                        help='Full audited NACA 0018 recording.json; enables long-source stability coverage')
    parser.add_argument('--surface-reconstruction', type=Path,
                        help='Audited reconstruction.json attached to the full point recording')
    args = parser.parse_args()
    phase_seconds = args.phase_seconds
    if phase_seconds < 30:
        parser.error("Each phase must be at least 30 seconds.")
    extra = []
    point_identity = None
    surface_identity = None
    if args.point_recording:
        args.point_recording = args.point_recording.expanduser().resolve()
        if (not args.point_recording.is_file() or args.point_recording.name != 'recording.json'
                or any(ord(c) < 32 or c == '"' for c in str(args.point_recording))):
            parser.error('Choose an existing local recording.json with a plain path.')
        with args.point_recording.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != '1ce4f9f4a7d71f060e60e62ecd0aa52de78e7f7d0328930ef0cdc952cd852a67':
            parser.error('Point stability requires the full audited 8000-frame descriptor.')
        point_identity = {'path': str(args.point_recording), 'metadata_sha256': digest}
        extra.append(f'-StudioPointRecording={args.point_recording}')
    if args.surface_reconstruction:
        if not point_identity:
            parser.error('Surface stability requires --point-recording.')
        args.surface_reconstruction = args.surface_reconstruction.expanduser().resolve()
        if (not args.surface_reconstruction.is_file() or args.surface_reconstruction.name != 'reconstruction.json'
                or any(ord(c) < 32 or c == '"' for c in str(args.surface_reconstruction))):
            parser.error('Choose an existing local reconstruction.json with a plain path.')
        digest = sha256(args.surface_reconstruction)
        if digest != 'b9194cdcf7500163650c6a78d696aa8214a06988e245c4e5ea707fdcb511265d':
            parser.error('Surface stability requires the audited full-source reconstruction.')
        surface_identity = {'path': str(args.surface_reconstruction), 'metadata_sha256': digest}
        extra.append(f'-StudioSurfaceReconstruction={args.surface_reconstruction}')
    root = Path(__file__).resolve().parents[1]
    app = root / "Packaged/Mac/LBMStudio.app"
    binary = app / "Contents/MacOS/LBMStudio"
    before = inventory()
    running = relevant(before)
    if any(Path(r["command"]).name != "CrashReportClient" and Path(r["command"]).name != "CrashReportClientEditor" for r in running.values()):
        raise SystemExit("Close LBMStudio and Unreal Editor before stability acceptance.")
    with (app / "Contents/Info.plist").open("rb") as stream:
        bundle = plistlib.load(stream)["CFBundleIdentifier"]
    saved = Path.home() / "Library/Containers" / bundle / "Data/Library/Application Support/Epic/LBMStudio/Saved"
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = root / "tmp/debug" / f"stability-{stamp}"
    output.mkdir(parents=True)
    report = saved / "Automation" / f"MixedUseReport-{stamp}"
    telemetry = saved / "Automation/MixedUse/resources.csv"
    telemetry.unlink(missing_ok=True)
    manifest = {"created_utc": stamp, "phase_seconds": phase_seconds,
                "acceptance": "M1_60_minute_baseline" if phase_seconds >= 1200 else "driver_rehearsal",
                "binary_sha256": sha256(binary), "startup_arguments": ["-LLM"],
                "processes_before": running, "errors": []}
    manifest['source_mix'] = 'full_point_and_both_legacy' if point_identity else 'both_legacy'
    if point_identity:
        manifest['point_recording'] = point_identity
    if surface_identity:
        manifest['surface_reconstruction'] = surface_identity
        manifest['source_mix'] = 'full_reconstructed_surface_and_both_legacy'
    manifest["source_sha256"] = {
        str(path.relative_to(root)): sha256(path)
        for directory in ("Source", "Config", "Tools") for path in sorted((root / directory).rglob("*"))
        if path.is_file() and path.suffix in {".cpp", ".h", ".mm", ".cs", ".ini", ".py", ".sh"}
    }
    tracked = {}
    started = time.monotonic()
    started_wall = time.time()
    proc = None

    def track():
        current = inventory()
        track_processes(proc, before, tracked, current, started_wall)
        return current

    try:
        with (output / "application.log").open("w") as log:
            # Match the normal launch profile, including memory-tracker overhead.
            proc = subprocess.Popen([str(binary), "-LLM", "-windowed", "-ResX=1320", "-ResY=740", "-unattended", "-stdout",
                                     "-FullStdOutLogOutput", "-StudioAutomation", "-StudioMixedUse",
                                     f"-StudioMixedPhaseSeconds={phase_seconds}", "-ExecCmds=Automation RunTests Studio.Stability.MixedUse",
                                     "-TestExit=Automation Test Queue Empty", f"-ReportExportPath={report}", *extra],
                                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            manifest["pid"] = proc.pid
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2))
            print(f"Running {manifest['acceptance']}: PID {proc.pid}; reports {output}", flush=True)
            last_progress = 0
            while proc.poll() is None:
                track()
                elapsed = time.monotonic()-started
                if elapsed-last_progress >= 60:
                    print(f"Stability process live: {elapsed:.0f}s, PID {proc.pid}", flush=True); last_progress = elapsed
                if elapsed > phase_seconds*3+240:
                    raise TimeoutError("Packaged stability run exceeded its deadline")
                time.sleep(2)
            manifest["exit_code"] = proc.returncode
            if proc.returncode:
                manifest["errors"].append(f"Application exited {proc.returncode}")
    except (KeyboardInterrupt, TimeoutError, OSError) as error:
        manifest["errors"].append(str(error) or "Interrupted")
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
                    proc.poll(); current = inventory()
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
        manifest["owned_processes"] = tracked
        current = inventory()
        manifest['owned_processes_after'] = {pid: info for pid, info in tracked.items() if same_process(pid, info, current)}
        if manifest['owned_processes_after']:
            manifest['errors'].append('Owned processes remain after cleanup')
        manifest["processes_after"] = relevant(current)
        manifest["elapsed_seconds"] = time.monotonic()-started
        if (report / "index.json").exists():
            data = (report / "index.json").read_bytes(); (output / "automation.json").write_bytes(data)
            result = json.loads(data.decode("utf-8-sig"))
            if result["succeeded"] != 1 or result["failed"] or result.get("succeededWithWarnings", 0) or result.get('notRun', 0):
                manifest["errors"].append("Automation did not complete one clean mixed-use test")
        else:
            manifest["errors"].append("Automation report missing")
        if telemetry.exists():
            (output / "resources.csv").write_bytes(telemetry.read_bytes())
            analysis = analyze(output / "resources.csv", phase_seconds, bool(point_identity), bool(surface_identity))
            (output / "resources.json").write_text(json.dumps(analysis, indent=2))
            manifest["errors"].extend(analysis["errors"])
        else:
            manifest["errors"].append("Resource telemetry missing")
        manifest["passed"] = not manifest["errors"]
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(json.dumps({"passed": manifest["passed"], "acceptance": manifest["acceptance"], "errors": manifest["errors"], "report": str(output)}, indent=2), flush=True)
    return 0 if manifest["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
