#!/usr/bin/env python3
"""Run the integrated two-hour workload, or an explicitly shorter rehearsal.

Uses original 3D cylinder data plus both bundled SU2 recordings. Requires the
scientific audit environment (Pillow/NumPy/VTK and FFmpeg). Native counters,
independent resource analysis and every completed PNG/MP4 are required. This
does not accept the remaining physical UI, fidelity or full product gates.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

from test_volume_stability import analyze


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def audit_exports(captures, root, recording, reconstruction):
    import numpy as np
    from verify_rendered_image_sequence import read_png, same_view_value
    from verify_movie_export import audit as audit_movie

    sources = [root/'Content/Samples/MeshGraphNets_Airfoil/recording.json',
               root/'Content/Samples/MeshGraphNets_Airfoil_test010/recording.json', recording]
    descriptors = {json.loads(p.read_text())['id']: (p, json.loads(p.read_text())) for p in sources}
    results, seen, cancelled = [], set(), 0
    for anchor_path in sorted(captures.glob('anchors/sequence-*.png')):
        number = int(anchor_path.stem.split('-')[-1])
        output = captures/anchor_path.stem
        if number % 4 == 0:
            assert not output.exists(), 'Cancelled sequence published partial output'
            cancelled += 1
            continue
        manifest = json.loads((output/'sequence.json').read_text())
        entries = [json.loads(line) for line in (output/'frames.jsonl').read_text().splitlines()]
        original_path, source = descriptors[manifest['dataset']]
        seen.add(source['id'])
        anchor, anchor_meta = read_png(anchor_path)
        assert manifest['metadata_sha256'] == digest(original_path)
        assert manifest['image_count'] == len(entries) == 3
        first = anchor_meta['ordinal']
        assert [e['ordinal'] for e in entries] == [first, first+1, first+2]
        assert manifest['first_ordinal'] == first and manifest['last_ordinal_inclusive'] == first+2 and manifest['stride'] == 1
        original_payload = original_path.parent/'flow.bin'
        expected_payload = digest(original_payload) if source['version'] == 1 else ''
        expected_reconstruction = digest(reconstruction) if source['version'] == 3 else ''
        assert manifest['payload_sha256'] == expected_payload
        assert manifest['reconstruction_sha256'] == expected_reconstruction
        frames = []
        for entry in entries:
            ordinal = entry['ordinal']
            if source['version'] == 1:
                with original_payload.open('rb') as payload:
                    magic, version, points, triangles, boundary, count = struct.unpack('<6i', payload.read(24))
                    assert magic == 0x53553246 and version == 2 and ordinal < count
                    payload.seek(24+16*points+12*triangles+4*boundary+ordinal*(12+16*points))
                    step, seconds = struct.unpack('<id', payload.read(12))
            else:
                step, seconds = source['frames'][ordinal]['index'], source['frames'][ordinal]['time']
            assert entry['source_step'] == step and entry['source_time_seconds'] == seconds
            path = output/entry['file']; pixels, metadata = read_png(path)
            assert metadata['dataset'] == source['id'] and metadata['ordinal'] == ordinal
            assert metadata['frame'] == step and metadata['time_seconds'] == seconds
            assert metadata['metadata_sha256'] == manifest['metadata_sha256']
            assert metadata['payload_sha256'] == expected_payload and metadata['reconstruction_sha256'] == expected_reconstruction
            assert entry['png_bytes'] == path.stat().st_size and pixels.shape == anchor.shape
            assert np.all(pixels[..., 3] == 255)
            for key, value in manifest['view'].items():
                same_view_value(key, metadata[key], value)
                assert value == anchor_meta[key]
            frames.append({'ordinal': ordinal, 'step': step, 'seconds': seconds, 'sha256': digest(path)})
            if ordinal == first:
                difference = float(np.abs(pixels[..., :3].astype(np.int16)-anchor[..., :3]).mean())
                assert difference < 2, 'Export differs from direct presented anchor'
        movie = audit_movie(output)
        results.append({'path': str(output), 'dataset': source['id'], 'anchor_mean_rgb_error': difference,
                        'frames': frames, 'movie': movie})
    assert len(results) >= 3 and cancelled >= 1
    assert seen == set(descriptors), 'All three original sources must produce completed exports'
    return {'passed': True, 'completed': len(results), 'cancelled': cancelled, 'sources': sorted(seen), 'sequences': results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording', type=Path)
    parser.add_argument('reconstruction', type=Path)
    parser.add_argument('--phase-seconds', type=int, default=1200,
                        help='Base duration: playback/idle use twice this duration; below1200 is rehearsal only')
    args = parser.parse_args()
    if args.phase_seconds < 30:parser.error('Use at least30 seconds per base phase.')
    # Fail before launching a two-hour run if the independent auditor is absent.
    import numpy, PIL, vtk  # noqa: F401
    subprocess.run(['ffprobe', '-version'], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['ffmpeg', '-version'], check=True, stdout=subprocess.DEVNULL)
    root = Path(__file__).resolve().parents[1]
    previous = set((root/'tmp/debug').glob('release-stability-*'))
    command = [sys.executable, str(root/'Tools/run_packaged_suite.py'), '--suite', 'Studio.VolumeStability.MixedUse',
               '--count', '1', '--name', 'release-stability', '--captures', 'ReleaseStability',
               '--timeout', str(args.phase_seconds*6+650), '--startup-profile', 'application-default',
               '--volume-recording', str(args.recording.resolve()), '--volume-reconstruction', str(args.reconstruction.resolve()),
               '--volume-phase-seconds', str(args.phase_seconds), '--release-soak']
    result = subprocess.run(command)
    reports = set((root/'tmp/debug').glob('release-stability-*'))-previous
    if len(reports) != 1:return result.returncode or 1
    report = reports.pop(); resources = list((report/'captures').glob('*/resources.csv'))
    if len(resources) != 1:return result.returncode or 1
    analysis = analyze(resources[0], args.phase_seconds, release=True)
    (report/'analysis.json').write_text(json.dumps(analysis, indent=2)+'\n')
    export_report = {'passed': False}
    try:
        export_report = audit_exports(resources[0].parent, root, args.recording.resolve(), args.reconstruction.resolve())
    except (AssertionError, OSError, KeyError, ValueError, subprocess.SubprocessError) as error:
        export_report['error'] = str(error) or type(error).__name__
    (report/'export-audit.json').write_text(json.dumps(export_report, indent=2)+'\n')
    passed = result.returncode == 0 and analysis['passed'] and export_report['passed']
    summary = {'passed': passed, 'acceptance': analysis['acceptance'], 'report': str(report),
               'resource_errors': analysis['errors'], 'exports_passed': export_report['passed'],
               'scope': 'Current packaged mixed-use/resource and original-image/movie evidence; no full UI or release completion claim.'}
    (report/'acceptance.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
