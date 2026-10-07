#!/usr/bin/env python3
"""Build and check the real CFD renderer without a window or screenshots.

Uses Metal and production AStudioScene render targets in an isolated commandlet
world. Validates original wing/volume identities, numerical pixel metrics,
camera independence, snapshots and on-demand capture. This is a short renderer
regression, not acceptance of native input, design or long-session stability.
"""
import argparse
import datetime
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess

from run_studio import _run_owned
from runtime_lane import serialized
from test_stability import inventory, relevant
from validate import digest


CASE_NAMES = ['wing.first', 'wing.last', 'wing.camera', 'volume.first', 'volume.last',
              'volume.inside_perspective', 'volume.inside_orthographic', 'volume.clipped',
              'volume.isosurface', 'volume.restored', 'snapshot.original_frame']
CHECK_NAMES = ['runtime.windowless', 'runtime.gpu', 'source.wing', 'wing.evolution',
               'wing.frame_keeps_camera', 'wing.camera_independent', 'assets.volume_material', 'source.volume',
               'volume.evolution', 'volume.clip_changes_pixels', 'volume.restored_pixels',
               'idle.no_captures', 'runtime.still_windowless', 'volume.independent_opacity.changes_pixels',
               'volume.independent_opacity.accounted', 'volume.independent_opacity.released']


def expected_frames(root):
    """Read independent original headers; do not trust the renderer's timeline."""
    wing = root/'Content/Samples/MeshGraphNets_Airfoil'
    volume = root/'Content/Samples/Cylinder3D_ReaderFixture'
    wd = json.loads((wing/'recording.json').read_text())
    vd = json.loads((volume/'recording.json').read_text())
    with (wing/'flow.bin').open('rb') as stream:
        magic, version, nodes, triangles, boundary, count = struct.unpack('<6i', stream.read(24))
        if magic != 0x53553246 or version != 2 or count < 2 or nodes < 3 or triangles < 1:
            raise ValueError('Unsupported wing fixture header')
        offset = 24+16*nodes+12*triangles+4*boundary
        wing_frames = {}
        for ordinal in (0, count-1):
            stream.seek(offset+ordinal*(12+16*nodes))
            step, time = struct.unpack('<id', stream.read(12))
            wing_frames[ordinal] = (step, time)
    mapping = root/'Content/Samples/Cylinder3D_VolumeFixture/reconstruction.json'
    sources = {'wing': {'id': wd['id'], 'metadata_sha256': digest(wing/'recording.json'),
                        'payload_sha256': digest(wing/'flow.bin'), 'reconstruction_sha256': '',
                        'dimensions': 2, 'frames': wing_frames},
               'volume': {'id': vd['id'], 'metadata_sha256': digest(volume/'recording.json'),
                          'payload_sha256': '', 'reconstruction_sha256': digest(mapping), 'dimensions': 3,
                          'frames': {i: (f['index'], f['time']) for i, f in enumerate(vd['frames'])}}}
    result = {}
    for name in CASE_NAMES:
        source = sources['wing' if name.startswith('wing.') else 'volume']
        ordinal = 0 if name.endswith('.first') else max(source['frames'])
        result[name] = {**source, 'ordinal': ordinal, 'step': source['frames'][ordinal][0], 'time': source['frames'][ordinal][1]}
    return result


def evaluate(report, expected):
    """Fail on incomplete, contradictory or unavailable numerical evidence."""
    if (not isinstance(report, dict) or not isinstance(report.get('cases'), list)
            or not isinstance(report.get('checks'), list)
            or any(not isinstance(row, dict) for row in report['cases']+report['checks'])):
        return {'passed': False, 'errors': ['Invalid renderer report structure'], 'cases': 0}
    errors = []
    if (report.get('version') != 1 or report.get('mode') != 'windowless-gpu-commandlet'
            or report.get('rhi') != 'Metal' or report.get('windowless') is not True
            or report.get('screenshots') != 0 or report.get('passed') is not True or report.get('errors') != []):
        errors.append('Commandlet did not confirm a successful windowless Metal run')
    cases = report.get('cases', [])
    names = [row.get('name') for row in cases]
    if any(not isinstance(name, str) for name in names):
        return {'passed': False, 'errors': ['Invalid renderer case name'], 'cases': len(cases)}
    if len(names) != len(set(names)) or set(names) != set(CASE_NAMES):
        errors.append('Missing, extra or duplicate renderer cases')
    checks = report.get('checks', [])
    if any(not isinstance(row.get('name'), str) for row in checks):
        return {'passed': False, 'errors': ['Invalid renderer check name'], 'cases': len(cases)}
    if not set(CHECK_NAMES+CASE_NAMES).issubset({row.get('name') for row in checks}):
        errors.append('Required renderer checks did not run')
    if not checks or any(row.get('passed') is not True for row in checks):
        errors.append('A renderer invariant failed')
    for row in cases:
        name = row.get('name')
        if name not in expected:
            continue
        identity = expected[name]
        if row.get('passed') is not True or row.get('frame_matches_source') is not True:
            errors.append(f'{name}: failed original-frame render')
        for key, required in [('dataset', identity['id']), ('metadata_sha256', identity['metadata_sha256']),
                              ('payload_sha256', identity['payload_sha256']), ('reconstruction_sha256', identity['reconstruction_sha256']),
                              ('spatial_dimensions', identity['dimensions']), ('ordinal', identity['ordinal']),
                              ('original_step', identity['step']), ('original_time', identity['time']), ('width', 640), ('height', 360)]:
            if row.get(key) != required:
                errors.append(f'{name}: {key} does not match independent source evidence')
        for key, minimum, maximum in [('colored_pixels', 640*360/200+1, 640*360), ('mesh_bytes', 0, 128*1024**2),
                                      ('scalar_texture_bytes', 0, 17*1024**2)]:
            value = row.get(key)
            if (not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value)
                    or not minimum <= value <= maximum):
                errors.append(f'{name}: invalid or out-of-budget {key}')
        requires_volume = name not in {'wing.first', 'wing.last', 'wing.camera', 'volume.isosurface'}
        texture = row.get('scalar_texture_bytes')
        has_texture = isinstance(texture, (int, float)) and not isinstance(texture, bool) and math.isfinite(texture) and texture > 0
        if row.get('volume_required') is not requires_volume or (requires_volume and not has_texture):
            errors.append(f'{name}: required volume texture unavailable')
        if not isinstance(row.get('pixel_crc32'), str) or not re.fullmatch('[0-9a-f]{8}', row['pixel_crc32']):
            errors.append(f'{name}: invalid pixel checksum')
        if row.get('orthographic') is not (name == 'volume.inside_orthographic'):
            errors.append(f'{name}: wrong camera projection')
        if name == 'snapshot.original_frame':
            difference = row.get('snapshot_mean_rgb_error')
            if (not isinstance(difference, (int, float)) or isinstance(difference, bool)
                    or not math.isfinite(difference) or not 0 <= difference < 2):
                errors.append('Snapshot pixels disagree with the live production render target')
            try:
                snapshot = json.loads(row['snapshot_metadata'])
                if not isinstance(snapshot, dict):
                    raise ValueError('Snapshot metadata must be an object')
                if any(snapshot.get(k) != v for k, v in [('dataset', identity['id']), ('ordinal', identity['ordinal']),
                                                        ('frame', identity['step']), ('time_seconds', identity['time']),
                                                        ('metadata_sha256', identity['metadata_sha256']),
                                                        ('reconstruction_sha256', identity['reconstruction_sha256']), ('size', [640, 360])]):
                    errors.append('Snapshot metadata disagrees with the original source')
            except (ValueError, KeyError, TypeError):
                errors.append('Snapshot metadata unavailable or invalid')
    by_name = {row.get('name'): row for row in cases}
    if set(CASE_NAMES).issubset(by_name):
        crc = {name: row.get('pixel_crc32') for name, row in by_name.items()}
        for a, b in [('wing.first', 'wing.last'), ('wing.last', 'wing.camera'),
                     ('volume.first', 'volume.last'), ('volume.last', 'volume.clipped')]:
            if crc[a] == crc[b]:
                errors.append(f'{a} and {b}: pixels did not change')
        if crc['volume.last'] != crc['volume.restored']:
            errors.append('Restoring the volume view did not restore its pixels')
    return {'passed': not errors, 'errors': errors, 'cases': len(cases)}


@serialized('Windowless GPU validation')
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-build', action='store_true', help='Explicitly validate the existing editor module')
    parser.add_argument('--timeout', type=int, default=600, help='Includes first-use shader compilation; seconds')
    args = parser.parse_args()
    if args.timeout < 10:
        parser.error('Use a timeout of at least 10 seconds.')
    root = Path(__file__).resolve().parents[1]
    before = relevant(inventory())
    if any(Path(p['command']).name not in {'CrashReportClient', 'CrashReportClientEditor'} for p in before.values()):
        parser.error('Unreal work is already running; existing processes were left untouched.')
    engine = Path(os.environ.get('UE_ENGINE_PATH', '/Users/Shared/Epic Games/UE_5.8'))
    editor = engine/'Engine/Binaries/Mac/UnrealEditor-Cmd'
    if not editor.is_file():
        parser.error('Set UE_ENGINE_PATH to an installed Unreal Engine containing UnrealEditor-Cmd.')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = root/'tmp/debug'/f'windowless-render-{stamp}'
    output.mkdir(parents=True)
    folders = ['Source', 'Config', 'Content/Studio', 'Content/Samples/MeshGraphNets_Airfoil',
               'Content/Samples/Cylinder3D_ReaderFixture', 'Content/Samples/Cylinder3D_VolumeFixture']
    files = [root/'LBMStudio.uproject', root/'Content/Samples/Registry/recordings.json']
    files += [root/'Tools'/name for name in ('validate_render.py', 'validate.py', 'run_studio.py',
                                            'runtime_lane.py', 'studio_processes.py', 'test_stability.py')]
    files += sorted(p for folder in folders for p in (root/folder).rglob('*') if p.is_file())
    sources = {str(p.relative_to(root)): digest(p) for p in files}
    summary = {'passed': False, 'mode': 'windowless-gpu-commandlet', 'build_requested': not args.no_build,
               'report': str(output), 'errors': [], 'sources': sources, 'screenshots': 0,
               'coverage': ['Production Metal flow, volume and isosurface rendering', 'Original frame and snapshot identity',
                            'Camera/frame independence and idle capture suppression'],
               'excluded': ['Native input and dialogs', 'Visual design review', 'Long-session and packaged-app stability'],
               'processes_before': before}
    try:
        expected = expected_frames(root)
        if not args.no_build:
            command = [str(engine/'Engine/Build/BatchFiles/Mac/Build.sh'), 'LBMStudioEditor', 'Mac', 'Development',
                       f'-project={root/"LBMStudio.uproject"}', '-NoHotReload', '-DisableAdaptiveUnity']
            code = _run_owned(command, root, timeout=1200, log_path=output/'build.log', report_path=output/'build-process.json')
            if code:
                raise RuntimeError(f'Build failed ({code}); see build.log')
        summary['module_sha256'] = digest(root/'Binaries/Mac/libUnrealEditor-LBMStudio.dylib')
        command = [str(editor), str(root/'LBMStudio.uproject'), '-run=StudioRenderValidation', '-LLM', '-unattended',
                   '-AllowCommandletRendering', '-RenderOffscreen', '-nosound', '-nosplash', '-stdout', '-FullStdOutLogOutput',
                   '-notraceserver', '-NoZenAutoLaunch', '-ddc=InstalledNoZenLocalFallback', f'-StudioRenderReport={output/"renderer.json"}']
        code = _run_owned(command, root, timeout=args.timeout, log_path=output/'renderer.log', report_path=output/'process.json')
        lifecycle = json.loads((output/'process.json').read_text())
        summary.update({key: lifecycle[key] for key in ('processes_after', 'owned_processes_after', 'elapsed_seconds')})
        if code or lifecycle.get('cleanup_required'):
            summary['errors'].append(f'Renderer process failed or needed cleanup ({code}); see process.json')
        report = json.loads((output/'renderer.json').read_text(encoding='utf-8'))
        result = evaluate(report, expected)
        summary['cases'] = result['cases'];summary['errors'] += result['errors']
        log = (output/'renderer.log').read_text(errors='replace')
        if 'rhiname="Metal"' not in log:
            summary['errors'].append('Engine did not confirm Metal')
        summary['warnings'] = [line for line in log.splitlines() if ': Warning:' in line]
        if any(digest(root/path) != sha for path, sha in sources.items()):
            summary['errors'].append('Source or rendering inputs changed during validation')
        summary['passed'] = not summary['errors']
    except (OSError, ValueError, KeyError, RuntimeError, struct.error, subprocess.SubprocessError) as error:
        summary['errors'].append(str(error))
    (output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps({key: summary[key] for key in ('passed', 'mode', 'errors', 'report')}, indent=2), flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
