#!/usr/bin/env python3
"""Retain three unchanged NACA source snapshots for native reader tests only.

Not an animation sample or installed recording. Requires the audited original
HDF5, its lossless v3 conversion, numpy, h5py and pandas/PyTables. Golden values
are independently read through pandas rather than from the converted arrays.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zlib

import h5py
import numpy as np
import pandas as pd

SOURCE_SHA = '513e30f370ecdd30c5a49dfcc7b9b0caf39204b6dec08ee513a53afeba657237'
ORDINALS = [0, 4000, 7999]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False)+'\n')


def extract(source, converted, output):
    if sha(source) != SOURCE_SHA:
        raise ValueError('Expected the independently audited original HDF5')
    if output.exists():
        raise ValueError('Choose a new fixture directory')
    descriptor = json.loads((converted/'recording.json').read_text())
    output.mkdir(parents=True)
    for name in ('coordinates.f64', 'point-ids.i64', 'author-setup.md', 'zenodo-record.json'):
        shutil.copyfile(converted/name, output/name)
    golden = {'originalMemberSHA256': SOURCE_SHA, 'sourceOrdinals': ORDINALS, 'samples': []}
    # Include source edge IDs, zero-speed wall samples and intermediate rows.
    points = [0, 1, 17, 100, 2048, 8192, 12000, 18000, 18705]
    with h5py.File(source) as original, pd.HDFStore(source, 'r') as independent:
        for field in descriptor['fields']:
            key, array = field['id'], field['array']
            values = original[key+'/block0_values'][:, ORDINALS].T
            if field['static']:
                if not np.all(values == values[:1]):
                    raise ValueError('Fixture static field changed')
                values = values[0]
            path = output/array['path']
            np.asarray(values, dtype='<f8').tofile(path)
            array['sha256'] = sha(path)
            array['byteLength'] = path.stat().st_size
            array['shape'] = list(values.shape)
            if not field['static']:
                array['frameCRC32'] = [zlib.crc32(row.astype('<f8').tobytes()) for row in values]
            for point in points:
                independent_values = independent.select(key, start=point, stop=point+1).iloc[0]
                for frame, original_ordinal in enumerate(ORDINALS):
                    expected = float(independent_values.iloc[original_ordinal])
                    actual = float(values[point] if field['static'] else values[frame, point])
                    if actual != expected:
                        raise ValueError('Independent fixture reader differs')
                    golden['samples'].append({'field': key, 'frame': frame, 'point': point, 'value': expected})
        golden['points'] = [
            {'row': point, 'id': int(original['coordinates/axis1'][point]),
             'position': original['coordinates/block0_values'][point].tolist()}
            for point in points]
    descriptor['frames'] = [descriptor['frames'][i] for i in ORDINALS]
    descriptor['frameCount'] = len(ORDINALS)
    descriptor['id'] = 'NACA0018_ReaderFixture_3OriginalFrames'
    descriptor['title'] = 'Reader test fixture · 3 original NACA snapshots'
    descriptor['limitations'].append('Test-only sparse snapshots; not the longer playback recording.')
    provenance = {'sourceURL': descriptor['sourceURL'], 'originalMemberSHA256': SOURCE_SHA,
                  'parentDescriptorSHA256': sha(converted/'recording.json'),
                  'sourceOrdinals': ORDINALS, 'sourceSteps': [f['index'] for f in descriptor['frames']],
                  'changes': 'Three original snapshots retained, original point order/precision unchanged.',
                  'extractorSHA256': sha(Path(__file__)), 'goldenReader': 'pandas HDFStore public row selection'}
    write_json(output/'provenance.json', provenance)
    (output/'ATTRIBUTION.txt').write_text((converted/'ATTRIBUTION.txt').read_text()+
        '\nReader test fixture: retained original source ordinals 0, 4000 and 7999 only. '
        'This sparse fixture is not the full animation recording.\n')
    descriptor['provenanceSHA256'] = sha(output/'provenance.json')
    descriptor['attributionSHA256'] = sha(output/'ATTRIBUTION.txt')
    write_json(output/'recording.json', descriptor)
    write_json(output/'expected.json', golden)
    print(f'Retained 3 original snapshots; {len(golden["samples"])} independent golden values')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('converted', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    extract(args.original, args.converted, args.output)
