#!/usr/bin/env python3
"""Build and check the real CFD renderer without a window or screenshots.

Uses Metal and production AStudioScene render targets in an isolated commandlet
world. Validates original wing/volume identities, numerical pixel metrics,
camera independence, snapshots and on-demand capture. This is a short renderer
regression, not acceptance of native input, design or long-session stability.
"""
import argparse
import ast
import base64
import hashlib
import io
import datetime
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import zipfile
import zlib

from run_studio import _run_owned
from runtime_lane import serialized
from test_stability import inventory, relevant
from validate import digest


ORIGINAL_CASE_NAMES = ['wing.first', 'wing.last', 'wing.camera', 'volume.first', 'volume.last',
              'volume.inside_perspective', 'volume.inside_orthographic', 'volume.clipped',
              'volume.isosurface', 'volume.restored', 'snapshot.original_frame']
CHECK_NAMES = ['runtime.windowless', 'runtime.gpu', 'source.wing', 'wing.evolution',
               'wing.frame_keeps_camera', 'wing.camera_independent', 'assets.volume_material', 'source.volume',
               'volume.evolution', 'volume.clip_changes_pixels', 'volume.restored_pixels',
               'idle.no_captures', 'runtime.still_windowless', 'volume.independent_opacity.changes_pixels',
               'volume.independent_opacity.accounted', 'volume.independent_opacity.released']
HOME4_CASE_NAMES = ['home4.slice.xy', 'home4.slice.xz', 'home4.slice.yz',
                    'home4.mask.surface.control', 'home4.mask.surface.masked',
                    'home4.mask.glyph.control', 'home4.mask.glyph.masked']
CASE_NAMES = ORIGINAL_CASE_NAMES + HOME4_CASE_NAMES
for name in HOME4_CASE_NAMES:
    CHECK_NAMES += [name + '.' + suffix for suffix in
                    ('source_slice', 'no_volume', 'mask_geometry', 'sampling', 'readback')]



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
    for name in ORIGINAL_CASE_NAMES:
        source = sources['wing' if name.startswith('wing.') else 'volume']
        ordinal = 0 if name.endswith('.first') else max(source['frames'])
        result[name] = {**source, 'ordinal': ordinal, 'step': source['frames'][ordinal][0], 'time': source['frames'][ordinal][1]}
    return result


def embedded_archive(root, key):
    """Decode the checked-in artificial original, independently of UE receipts."""
    file, symbol = ('StudioHome4GPUFixtures.inl', 'MaskXY') if key == 'mask' else ('StudioHome4SliceFixtures.inl', key)
    text = (root/'Source/LBMStudio'/file).read_text()
    match = re.search(r'static const TCHAR '+symbol+r'\[\]=\s*(.*?);', text, re.S)
    if not match:
        raise ValueError(f'Missing artificial original {key}')
    return base64.b64decode(''.join(re.findall(r'TEXT\("([A-Za-z0-9+/=]+)"\)', match[1])), validate=True)


def npy_member(archive, name):
    """Read small explicit fixture headers without NumPy or renderer metadata."""
    data = zipfile.ZipFile(io.BytesIO(archive)).read(name+'.npy')
    if data[:8] != b'\x93NUMPY\x01\x00':
        raise ValueError('Unsupported artificial NumPy fixture header')
    length = struct.unpack('<H', data[8:10])[0]
    header = ast.literal_eval(data[10:10+length].decode('ascii'))
    raw = data[10+length:]
    shape = header['shape'];count = math.prod(shape)
    if header['fortran_order'] is not False:
        raise ValueError('Artificial GPU fixture requires C array storage')
    dtype = header['descr']
    if dtype in ('<f8', '<i8') and len(raw) == count*8:
        return shape, list(struct.unpack('<'+str(count)+('d' if dtype == '<f8' else 'q'), raw))
    if re.fullmatch(r'<U[0-9]+', dtype) and shape == ():
        return shape, raw.decode('utf-32le').rstrip('\0')
    raise ValueError('Unsupported artificial fixture member')


def verified_array(folder, descriptor, format_code='d'):
    path = Path(descriptor['path'])
    if path.name != str(path):
        raise ValueError('Fixture array is not a local explicit member')
    data = (folder/path).read_bytes()
    if hashlib.sha256(data).hexdigest() != descriptor['sha256'] or len(data) % 8:
        raise ValueError('Generated source array checksum or length invalid')
    if 'byteLength' in descriptor and len(data) != descriptor['byteLength']:
        raise ValueError('Generated source array byte length invalid')
    if 'frameCRC32' in descriptor and descriptor['frameCRC32'] != [zlib.crc32(data)]:
        raise ValueError('Generated source frame checksum invalid')
    if 'shape' in descriptor and math.prod(descriptor['shape'])*8 != len(data):
        raise ValueError('Generated source array shape invalid')
    return list(struct.unpack('<'+str(len(data)//8)+format_code, data))


def expected_home4(root, directory):
    result = {}
    for key in ('xy', 'xz', 'yz', 'mask'):
        original = embedded_archive(root, key)
        if (directory/'home4-fixtures'/f'{key}.npz').read_bytes() != original:
            raise ValueError(f'{key}: commandlet original differs from explicit test source')
        shape, _ = npy_member(original, 'phi')
        _, spec = npy_member(original, 'run_spec');spec = json.loads(spec)
        axes = spec['archive']['axisOrder']
        _, origin = npy_member(original, 'origin');_, spacing = npy_member(original, 'spacing')
        _, iteration = npy_member(original, 'iteration')
        dimensions = [shape[axes.index(axis)] if axis in axes else 1 for axis in 'xyz']
        folder = directory/'home4-fixtures'/key
        metadata = folder/'recording.json';source = json.loads(metadata.read_text())
        grid = source['structuredGrid']
        if (source['version'] != 3 or source['spatialDimensions'] != 3 or source['pointCount'] != (81 if key == 'mask' else 30)
                or source['id'] != 'HOME4_'+digest(folder/'provenance.json')[:32]
                or source['provenanceSHA256'] != digest(folder/'provenance.json')
                or grid['kind'] != 'home4_structured_slice' or grid['dimensionsXYZ'] != dimensions
                or grid['originalDimensionsXYZ'] != dimensions or grid['axisOrder'] != axes
                or grid['originalOriginXYZ'] != origin or grid['originalSpacingXYZ'] != spacing
                or grid['cropMinimumXYZ'] != [0, 0, 0] or grid['cropMaximumXYZ'] != dimensions
                or grid['previewStride'] != 1 or grid['sourceRunId'] != spec['runId']
                or grid['units']['dxMeters'] != .1 or grid['units']['dtSeconds'] != .02
                or source['frames'] != [{'index': iteration[0], 'time': iteration[0]*.02, 'label': f'step_{iteration[0]}'}]):
            raise ValueError(f'{key}: generated original header differs from independent NumPy source')
        coordinates = verified_array(folder, source['coordinates'])
        expected_coordinates = [(origin[a]+i*spacing[a])*.1 for z in range(dimensions[2])
                                for y in range(dimensions[1]) for x in range(dimensions[0])
                                for a, i in enumerate((x, y, z))]
        if coordinates != expected_coordinates or verified_array(folder, source['pointIds'], 'q') != list(range(81 if key == 'mask' else 30)):
            raise ValueError(f'{key}: original nodes duplicated, extruded or reordered')
        fields = {f['id']: f for f in source['fields']}
        # Every scalar has an explicit immutable array receipt, including masks.
        values = {name: verified_array(folder, field['array']) for name, field in fields.items()}
        if key == 'mask':
            _, tau = npy_member(original, 'tau_fld');_, control = npy_member(original, 'mask_control')
            # Canonical x-fastest ordering, independently from the source C array axes.
            tau = [tau[x*9+y] for y in range(9) for x in range(9)]
            control = [control[x*9+y] for y in range(9) for x in range(9)]
            if (values['tau_fld'] != tau or values['mask_control'] != control
                    or values['tau_margin_valid'] != [float(t > .5) for t in tau]
                    or values['log10_tau_margin'] != [math.log10(t-.5) if t > .5 else 0. for t in tau]
                    or fields['log10_tau_margin']['validityMask'] != 'tau_margin_valid'
                    or fields['mask_control'].get('validityMask')):
                raise ValueError('Artificial mask values/control or explicit validity dependency changed')
            names = [n for n in HOME4_CASE_NAMES if n.startswith('home4.mask.')]
        else:
            _, ux = npy_member(original, 'ux')
            # Original affine expectation ux=(X+2Y+3Z)/dt, before display axis transform.
            expected_ux = [(origin[0]+x*spacing[0]+2*(origin[1]+y*spacing[1])+3*(origin[2]+z*spacing[2]))*.1/.02
                           for z in range(dimensions[2]) for y in range(dimensions[1]) for x in range(dimensions[0])]
            if len(ux) != 30 or any(not math.isclose(a, b, abs_tol=1.e-10) for a, b in zip(values['ux'], expected_ux)):
                raise ValueError(f'{key}: original affine scalar contract changed')
            names = ['home4.slice.'+key]
        for name in names:
            masked = name.endswith('.masked');glyph = '.glyph.' in name
            result[name] = {'id': source['id'], 'metadata_sha256': digest(metadata), 'payload_sha256': '',
                            'reconstruction_sha256': '', 'dimensions': 3, 'ordinal': 0,
                            'step': iteration[0], 'time': iteration[0]*.02, 'interpolation': 'SourceSlice',
                            'original_nodes': 81 if key == 'mask' else 30, 'original_plane_only': True, 'volume_texture_bytes': 0,
                            'surface_triangles': (64 if masked else 128) if key == 'mask' else 40, 'glyphs': (45 if masked else 81) if glyph else 0,
                            'scalar': ('log10_tau_margin' if masked else 'mask_control') if key == 'mask' else 'ux',
                            'invalid_sample_available': not masked if key == 'mask' else key == 'xy',
                            'valid_sample_available': True if key == 'mask' else key == 'xy'}
    return result


def evaluate_home4_pixels(report, directory):
    """Verify actual target bytes and diagnostic holes; never use success flags."""
    if (not isinstance(report, dict) or not isinstance(report.get('cases'), list)
            or any(not isinstance(r, dict) or not isinstance(r.get('name'), str) for r in report['cases'])):
        return ['Invalid HOME4 pixel report structure']
    errors = [];rows = {r['name']: r for r in report['cases']}
    pixels = {}
    for name in HOME4_CASE_NAMES:
        try:
            data = (directory/(name+'.bgra')).read_bytes();row = rows[name]
            if len(data) != 640*360*4 or f'{zlib.crc32(data):08x}' != row['pixel_crc32']:
                raise ValueError('raw GPU pixels have wrong size or checksum')
            colored = sum(max(p[:3]) > 12 and max(p[:3])-min(p[:3]) > 4 for p in zip(*[iter(data)]*4))
            if colored != row['colored_pixels'] or any(a != 255 for a in data[3::4]):
                raise ValueError('raw GPU pixel metrics disagree with report')
            pixels[name] = data
        except (OSError, KeyError, ValueError) as error:
            errors.append(f'{name}: {error}')
    for mode in ('surface', 'glyph'):
        control_name = f'home4.mask.{mode}.control';mask_name = f'home4.mask.{mode}.masked'
        if control_name not in pixels or mask_name not in pixels:
            continue
        try:
            row = rows[control_name];masked = rows[mask_name]
            for key in ('camera_focus', 'camera_right', 'camera_up', 'ortho_width'):
                if row[key] != masked[key]:
                    raise ValueError('mask pair changed camera')
            focus, right, up = (row[k] for k in ('camera_focus', 'camera_right', 'camera_up'))
            width = row['ortho_width']
            if (len(focus) != 3 or any(not math.isclose(a, b, abs_tol=1.e-12) for a,b in zip(focus, [.75,.5,.875])) or not math.isfinite(width) or not 0 < width < 4
                    or any(len(v) != 3 or any(not isinstance(a, (int,float)) or not math.isfinite(a) for a in v) for v in (right, up))
                    or abs(sum(a*a for a in right)-1) > 1.e-6 or abs(sum(a*a for a in up)-1) > 1.e-6
                    or abs(right[1])+abs(up[1]) > 1.e-6):
                raise ValueError('original plane camera unavailable')
            def region(x0, x1):
                corners = [(x, .5, z) for x in (x0,x1) for z in (.78,.83)]
                projected = [(320+sum((p[i]-focus[i])*right[i] for i in range(3))*640/width,
                              180-sum((p[i]-focus[i])*up[i] for i in range(3))*640/width) for p in corners]
                xmin,xmax = math.ceil(min(p[0] for p in projected)),math.floor(max(p[0] for p in projected))
                ymin,ymax = math.ceil(min(p[1] for p in projected)),math.floor(max(p[1] for p in projected))
                if not (0 <= xmin < xmax < 640 and 0 <= ymin < ymax < 360):
                    raise ValueError('mask inspection region outside target')
                return [4*(y*640+x) for y in range(ymin,ymax+1) for x in range(xmin,xmax+1)]
            def magenta(data, offsets):
                return sum(data[i] > 80 and data[i+2] > 80 and data[i+1] < 40 for i in offsets)
            invalid, valid = region(.3,.35),region(.8,.85)
            if (magenta(pixels[control_name],invalid) < len(invalid)*.8
                    or magenta(pixels[mask_name],invalid) != 0
                    or magenta(pixels[control_name],valid) < len(valid)*.8
                    or magenta(pixels[mask_name],valid) < len(valid)*.8):
                raise ValueError('diagnostic region was drawn despite mask, or valid/control region is empty')
        except (ValueError, KeyError, TypeError, OverflowError) as error:
            errors.append(f'home4.mask.{mode}: {error}')
    return errors


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
        requires_volume = name in ORIGINAL_CASE_NAMES and name not in {'wing.first', 'wing.last', 'wing.camera', 'volume.isosurface'}
        texture = row.get('scalar_texture_bytes')
        has_texture = isinstance(texture, (int, float)) and not isinstance(texture, bool) and math.isfinite(texture) and texture > 0
        if row.get('volume_required') is not requires_volume or (requires_volume and not has_texture):
            errors.append(f'{name}: required volume texture unavailable')
        if not isinstance(row.get('pixel_crc32'), str) or not re.fullmatch('[0-9a-f]{8}', row['pixel_crc32']):
            errors.append(f'{name}: invalid pixel checksum')
        if row.get('orthographic') is not (name == 'volume.inside_orthographic' or name in HOME4_CASE_NAMES):
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
        if name in HOME4_CASE_NAMES:
            for key in ('interpolation', 'original_nodes', 'original_plane_only', 'volume_texture_bytes',
                        'surface_triangles', 'glyphs', 'scalar', 'invalid_sample_available', 'valid_sample_available'):
                if row.get(key) != identity[key] or type(row.get(key)) is not type(identity[key]):
                    errors.append(f'{name}: invalid {key} for original slice or diagnostic mask')
            if row.get('scalar_texture_bytes', 0) <= 0:
                errors.append(f'{name}: slice scalar texture unavailable')
    if set(expected) != set(CASE_NAMES):
        errors.append('Independent expectations incomplete')
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
                            'Camera/frame independence and idle capture suppression',
                            'Original HOME4 XY/XZ/YZ source slices and diagnostic masks'],
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
        expected.update(expected_home4(root, output))
        result = evaluate(report, expected)
        result['errors'] += evaluate_home4_pixels(report, output)
        result['passed'] = not result['errors']
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
