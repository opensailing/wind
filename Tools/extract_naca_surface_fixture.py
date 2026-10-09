#!/usr/bin/env python3
"""Small topology/index test fixture using the three existing original NACA frames.

It is not an installed recording. Expected interior values are explicitly linear
interpolation checks computed directly from the original HDF5 through pandas.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

import numpy as np
import pandas as pd

SOURCE_SHA256 = '513e30f370ecdd30c5a49dfcc7b9b0caf39204b6dec08ee513a53afeba657237'
ORDINALS = [0, 4000, 7999]


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def extract(original, reconstruction, fixture_descriptor, output):
    if output.exists():
        raise ValueError('Choose a new output directory')
    if digest(original) != SOURCE_SHA256:
        raise ValueError('Expected the audited original HDF5')
    manifest = json.loads((reconstruction / 'reconstruction.json').read_text())
    fixture = json.loads(fixture_descriptor.read_text())
    if (fixture['coordinates']['sha256'] != manifest['source']['coordinatesSHA256'] or
            fixture['pointIds']['sha256'] != manifest['source']['pointIdsSHA256'] or
            fixture['pointCount'] != manifest['source']['pointCount'] or fixture['frameCount'] != 3 or
            [x['index'] for x in fixture['frames']] != [1001+i for i in ORDINALS]):
        raise ValueError('Reader fixture does not preserve the expected original rows and frame selection')
    for name in ('triangles', 'solidBoundary'):
        meta = manifest[name]
        if digest(reconstruction / meta['path']) != meta['sha256']:
            raise ValueError('Reconstruction payload hash mismatch')
    faces = np.fromfile(reconstruction / manifest['triangles']['path'], dtype='<u4').reshape(-1, 3)
    indices = np.linspace(0, len(faces)-1, 48, dtype=int)
    faces = faces[indices]
    rows = np.unique(faces)
    with pd.HDFStore(original, 'r') as h:
        xy = h.select('coordinates').values
        values = {name: np.stack([h.select(name, start=int(row), stop=int(row)+1).iloc[0, ORDINALS].values
                                 for row in rows])
                  for name in ('velocity_u', 'velocity_v', 'pressure', 'velocity_magnitude', 'cell_volume')}
    lookup = {int(row): i for i, row in enumerate(rows)}
    weights = np.array([.2, .3, .5])
    queries = []
    for face in faces:
        item = {'position': (xy[face] * weights[:, None]).sum(axis=0).tolist(), 'expected': []}
        selected = [lookup[int(row)] for row in face]
        for field, array in values.items():
            for frame in range(3):
                item['expected'].append({'field': field, 'frame': frame,
                                         'value': float(np.dot(array[selected, frame], weights))})
        queries.append(item)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix='.surface-fixture-', dir=output.parent))
    try:
        for filename in ('reconstruction.json', 'triangles.u32', 'solid-boundary.u32', 'ATTRIBUTION.txt'):
            shutil.copyfile(reconstruction / filename, temporary / filename)
        # The real geometry is unchanged; this test-only manifest explicitly binds
        # it to the existing three-frame reader fixture instead of the full sequence.
        manifest['parentReconstructionSHA256'] = digest(reconstruction / 'reconstruction.json')
        manifest['source']['recordingId'] = fixture['id']
        manifest['source']['descriptorSHA256'] = digest(fixture_descriptor)
        manifest['source']['frameCount'] = fixture['frameCount']
        manifest['limitations'].append('Test-only binding to three unchanged original frames; not an installed playback recording.')
        (temporary / 'reconstruction.json').write_text(json.dumps(manifest, indent=2) + '\n')
        expected = {'originalHDF5SHA256': SOURCE_SHA256, 'sourceOrdinals': ORDINALS,
                    'reconstructionSHA256': digest(temporary / 'reconstruction.json'),
                    'trianglesSHA256': manifest['triangles']['sha256'],
                    'coordinatesSHA256': manifest['source']['coordinatesSHA256'],
                    'pointIdsSHA256': manifest['source']['pointIdsSHA256'],
                    'extractorSHA256': digest(Path(__file__)), 'queries': queries,
                    'scope': 'Test-only reconstructed topology, shared with original coordinates in NACA0018_ReaderFixture. Not a recording or original CFD mesh.',
                    'expectedValueMethod': 'pandas direct original HDF5 row selection, weighted by (0.2,0.3,0.5); interpolation checks, not new solver output.'}
        (temporary / 'expected.json').write_text(json.dumps(expected, indent=2) + '\n')
        temporary.rename(output)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    print(f'Wrote {len(queries)} positions with {sum(len(q["expected"]) for q in queries)} independent interpolated values')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('reconstruction', type=Path)
    parser.add_argument('fixture_descriptor', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    extract(args.original, args.reconstruction, args.fixture_descriptor, args.output)
