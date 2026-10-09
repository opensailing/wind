#!/usr/bin/env python3
"""Import one unmodified SU2 ground-truth trajectory from DeepMind's public dataset.

Uses Python's standard library; never trains a model or generates flow frames.
Original TFRecord bytes, metadata and source checksums are retained alongside it.
"""
import array
import collections
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1] / 'Content/Samples/MeshGraphNets_Airfoil'


def varint(data, position):
    value = shift = 0
    while position < len(data) and shift < 70:
        byte = data[position]
        position += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, position
        shift += 7
    raise ValueError('Invalid protobuf varint')


def fields(data):
    position = 0
    while position < len(data):
        tag, position = varint(data, position)
        if tag & 7 != 2:
            raise ValueError('Expected a length-delimited protobuf field')
        length, position = varint(data, position)
        if position + length > len(data):
            raise ValueError('Truncated protobuf field')
        yield tag >> 3, data[position:position+length]
        position += length


def masked_crc32c(data):
    table = []
    for byte in range(256):
        value = byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        table.append(value)
    crc = 0xFFFFFFFF
    for byte in data:
        crc = table[(crc ^ byte) & 255] ^ (crc >> 8)
    crc ^= 0xFFFFFFFF
    return (((crc >> 15) | (crc << 17)) + 0xA282EAD8) & 0xFFFFFFFF


def load_source():
    manifest = json.loads((ROOT/'provenance.json').read_text())
    for name, expected in manifest['sha256'].items():
        actual = hashlib.sha256((ROOT/'source'/name).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f'Source checksum mismatch: {name}')
    ordinal = manifest['trajectory_ordinal_zero_based']
    data = (ROOT/f'source/test-trajectory-{ordinal:03d}.tfrecord').read_bytes()
    length, length_crc = struct.unpack_from('<QI', data)
    if length + 16 != len(data) or masked_crc32c(data[:8]) != length_crc:
        raise ValueError('Invalid TFRecord framing')
    payload = data[12:-4]
    if masked_crc32c(payload) != struct.unpack_from('<I', data, len(data)-4)[0]:
        raise ValueError('Invalid TFRecord payload checksum')
    feature_map = dict(fields(payload))[1]
    meta = json.loads((ROOT/'source/meta.json').read_text())
    result = {}
    for _, entry in fields(feature_map):
        entry_fields = dict(fields(entry))
        name = entry_fields[1].decode()
        byte_list = dict(fields(dict(fields(entry_fields[2]))[1]))[1]
        spec = meta['features'][name]
        values = array.array('i' if spec['dtype'] == 'int32' else 'f', byte_list)
        if sys.byteorder != 'little':
            values.byteswap()
        if len(values) != math.prod(spec['shape']):
            raise ValueError(f'Invalid field shape: {name}')
        result[name] = values
    return meta, result


def import_sample():
    meta, data = load_source()
    nodes = list(zip(data['mesh_pos'][::2], data['mesh_pos'][1::2]))
    triangles = list(zip(data['cells'][::3], data['cells'][1::3], data['cells'][2::3]))
    # Recover exact inner boundary from source triangle topology, without analytic geometry.
    counts = collections.Counter(tuple(sorted(edge)) for a,b,c in triangles for edge in ((a,b),(b,c),(c,a)))
    edges = [edge for edge,count in counts.items() if count == 1 and all(data['node_type'][v] == 2 for v in edge)]
    adjacency = collections.defaultdict(list)
    for a,b in edges:
        adjacency[a].append(b)
        adjacency[b].append(a)
    assert len(adjacency) == 200 and all(len(v) == 2 for v in adjacency.values())
    boundary = [min(adjacency)]
    previous = None
    while len(boundary) < len(adjacency):
        current = boundary[-1]
        following = next(v for v in adjacency[current] if v != previous)
        assert following not in boundary
        boundary.append(following)
        previous = current
    assert boundary[0] in adjacency[boundary[-1]]
    n = len(nodes)
    frame_count = meta['trajectory_length']
    output = ROOT/'flow.bin'
    frame_checksums = []
    scalar_specs = [
        ('velocity_magnitude', 'Velocity magnitude', 'm/s', 'derived'),
        ('velocity_x', 'Velocity X', 'm/s', 'derived'),
        ('velocity_y', 'Velocity Y', 'm/s', 'derived'),
        ('pressure', 'Pressure', 'Pa', 'source'),
        ('density', 'Density', 'kg/m3', 'source'),
    ]
    scalar_ranges = [[math.inf, -math.inf] for _ in scalar_specs]
    with output.open('wb') as out:
        out.write(struct.pack('<6i',0x53553246,2,n,len(triangles),len(boundary),frame_count))
        for point in nodes:
            out.write(struct.pack('<2d',*point))
        for tri in triangles:
            out.write(struct.pack('<3i',*tri))
        for node in boundary:
            out.write(struct.pack('<i',node))
        for frame in range(frame_count):
            out.write(struct.pack('<id',frame,frame*meta['dt']))
            values = array.array('f')
            for i in range(frame*n,(frame+1)*n):
                # The paper defines the Airfoil vector field as momentum; its visualization footnote divides by density.
                rho = data['density'][i]
                row = (data['velocity'][2*i]/rho,data['velocity'][2*i+1]/rho,data['pressure'][i],rho)
                assert all(math.isfinite(value) for value in row) and row[3] > 0
                values.extend(row)
            # Measure the actually stored float32 components after rounding,
            # over every original node and snapshot. Speed is derived from those
            # stored components, independently of any displayed color range.
            for field, offset in enumerate(range(4), start=1):
                component = values[offset::4]
                scalar_ranges[field][0] = min(scalar_ranges[field][0], min(component))
                scalar_ranges[field][1] = max(scalar_ranges[field][1], max(component))
            for u, v in zip(values[::4], values[1::4]):
                speed = math.sqrt(u*u + v*v)
                scalar_ranges[0][0] = min(scalar_ranges[0][0], speed)
                scalar_ranges[0][1] = max(scalar_ranges[0][1], speed)
            if sys.byteorder != 'little':
                values.byteswap()
            payload = values.tobytes()
            frame_checksums.append(zlib.crc32(payload))
            out.write(payload)
    print(f'Imported {frame_count} published snapshots; {n} original nodes; {len(triangles)} original triangles')
    print(f'Source elapsed time: {(frame_count-1)*meta["dt"]:.4f} s. Default playback: {(frame_count-1)/20:.1f} s at 20 snapshots/s.')
    print('Fixture SHA256:',hashlib.sha256(output.read_bytes()).hexdigest())
    manifest = json.loads((ROOT/'provenance.json').read_text())
    ordinal = manifest['trajectory_ordinal_zero_based']
    descriptor = {
        'version': 1,
        'id': f'MeshGraphNets_Airfoil_test{ordinal:03d}',
        'title': f'Airfoil SU2 {ordinal:03d}',
        'sourceLabel': f'MeshGraphNets_SU2_test{ordinal:03d}',
        'sourceURL': manifest['repository'],
        'timeNote': manifest['time_note'],
        'fieldNote': manifest['field_note'],
        'spatialDimensions': 2,
        'coordinateUnit': 'm', 'velocityUnit': 'm/s',
        'pressureUnit': 'Pa', 'densityUnit': 'kg/m3',
        'scalars': [dict(id=spec[0], label=spec[1], unit=spec[2], origin=spec[3], range=extent)
                    for spec, extent in zip(scalar_specs, scalar_ranges)],
        'sourceOffset': [-0.5, 0, 0],
        'displayMin': [-1.5, -0.8, -0.8],
        'displayMax': [2.9, 0.8, 0.8],
        'payloadSHA256': hashlib.sha256(output.read_bytes()).hexdigest(),
        'frameCRC32': frame_checksums,
        'meshCRC32': zlib.crc32(output.read_bytes()[:24+16*n+12*len(triangles)+4*len(boundary)]),
    }
    (ROOT/'recording.json').write_text(json.dumps(descriptor,indent=2)+'\n')
    catalog_path = ROOT.parent/'Registry/recordings.json'
    catalog_path.parent.mkdir(parents=True,exist_ok=True)
    catalog = json.loads(catalog_path.read_text()) if catalog_path.exists() else {'recordings': []}
    entries = {entry['id']: entry for entry in catalog['recordings']}
    entries[descriptor['id']] = {'id': descriptor['id'], 'title': descriptor['title'], 'path': ROOT.name+'/flow.bin'}
    catalog_path.write_text(json.dumps({'recordings': [entries[key] for key in sorted(entries)]},indent=2)+'\n')


if __name__ == '__main__':
    if len(sys.argv) > 1:
        ROOT = Path(sys.argv[1]).resolve()
    import_sample()
