#!/usr/bin/env python3
"""Independently verify bundled SU2 scalar metadata against unchanged payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import numpy as np

EXPECTED = {
    'MeshGraphNets_Airfoil': 'd7ad18920c911f253261948bb75d40a764e148d1b2e68e224f3f557eadaa729c',
    'MeshGraphNets_Airfoil_test010': 'b31ba4d63dd82a7a2c2f9eeb100928c9b539b76cd75ddcc29c8065c6b197010d',
}


def verify(folder):
    descriptor = json.loads((folder/'recording.json').read_text())
    payload = folder/'flow.bin'
    with payload.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        stream.seek(0)
        magic, version, nodes, triangles, boundary, frames = struct.unpack('<6i', stream.read(24))
    assert digest == EXPECTED[folder.name] == descriptor['payloadSHA256'], 'Original converted field bytes changed'
    assert magic == 0x53553246 and version == 2
    offset = 24 + nodes*16 + triangles*12 + boundary*4
    record_type = np.dtype([('step', '<i4'), ('time', '<f8'), ('values', '<f4', (nodes, 4))])
    assert offset + record_type.itemsize*frames == payload.stat().st_size
    records = np.memmap(payload, mode='r', offset=offset, dtype=record_type, shape=(frames,))
    values = records['values']
    u, v = values[:, :, 0].astype(np.float64), values[:, :, 1].astype(np.float64)
    speed = np.sqrt(u*u + v*v)
    arrays = [speed, u, v, values[:, :, 2], values[:, :, 3]]
    names = ['velocity_magnitude', 'velocity_x', 'velocity_y', 'pressure', 'density']
    units = ['m/s', 'm/s', 'm/s', 'Pa', 'kg/m3']
    assert len(descriptor['scalars']) == len(names)
    for field, name, unit, array in zip(descriptor['scalars'], names, units, arrays):
        assert field['id'] == name and field['unit'] == unit
        assert field['origin'] == ('source' if name in ('pressure', 'density') else 'derived')
        assert np.isfinite(array).all()
        assert field['range'] == [float(array.min()), float(array.max())], name
    return {'recording': folder.name, 'payload_sha256': digest, 'nodes': nodes, 'frames': frames,
            'scalar_values_examined': nodes*frames*5, 'scalars': descriptor['scalars']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]/'Content/Samples'
    results = [verify(root/name) for name in EXPECTED]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({'method': 'Independent NumPy structured payload reader; pinned preceding payload hashes; full original-node ranges.',
                                      'recordings': results}, indent=2)+'\n')
    print(f'Verified all five scalar ranges in {len(results)} unchanged original recordings.')
