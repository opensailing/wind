#!/usr/bin/env python3
"""Convert the verified, published NACA 0021 force history without generating data.

This fixture has no spatial field and is independent of the SU2 recordings.
The original text and complete source notice are retained beside the conversion.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT / 'Content/Samples/NaluWind_NACA0021_Re270k_AoA30'
SOURCE_NAME = 'source/Re_270k_aoa_30.dat'
SOURCE_SHA = 'f8faf9694f41ffc05f3a4662aa217414275e4850351b0262732cf3def6f4116e'
NOTICE_SHA = 'be238a4885ef46666b3ec36edcf5ff98122aa637f90bb30a7b5adab3cd7fbc45'
HEADERS = ('Time', 'Fpx', 'Fpy', 'Fpz', 'Fvx', 'Fvy', 'Fvz',
           'Mtx', 'Mty', 'Mtz', 'Y+min', 'Y+max')
REFERENCE = {'density_kg_m3': 1.2, 'freestream_speed_m_s': 50,
             'reference_area_m2': 4, 'denominator_N': 6000}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def parse_rows(data):
    """Validate columns, finite values and increasing time; retain source lexemes."""
    lines = data.decode('ascii').splitlines()
    if not lines or tuple(lines[0].split()) != HEADERS:
        raise ValueError('Unexpected force-history columns')
    result = []
    previous_time = -math.inf
    for line_number, line in enumerate(lines[1:], start=2):
        if not line.strip():
            continue
        words = line.split()
        if len(words) != len(HEADERS):
            raise ValueError(f'Wrong column count at line {line_number}')
        values = tuple(float(word) for word in words)
        if not all(math.isfinite(value) for value in values):
            raise ValueError(f'Nonfinite value at line {line_number}')
        if values[0] < 0 or values[0] <= previous_time:
            raise ValueError(f'Nonincreasing time at line {line_number}')
        previous_time = values[0]
        result.append((words, values))
    if not result:
        raise ValueError('Force history has no samples')
    return result


def atomic_write(path, data):
    descriptor, name = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def convert(sample=SAMPLE, output=None):
    sample = Path(sample)
    output = sample if output is None else Path(output)
    manifest_bytes = (sample / 'provenance.json').read_bytes()
    manifest = json.loads(manifest_bytes)
    original = (sample / SOURCE_NAME).read_bytes()
    notice = (sample / 'source/LICENSE.txt').read_bytes()
    if digest(original) != SOURCE_SHA or digest(notice) != NOTICE_SHA:
        raise ValueError('Original data or required source notice failed SHA-256 verification')
    if manifest['sha256'] != {SOURCE_NAME: SOURCE_SHA, 'source/LICENSE.txt': NOTICE_SHA}:
        raise ValueError('Provenance does not identify the verified originals')
    if any(manifest['normalization'].get(key) != value for key, value in REFERENCE.items()):
        raise ValueError('Reference values differ from the published normalization')
    rows = parse_rows(original)
    if len(rows) != 6967 or rows[0][1][0] != .4004 or rows[-1][1][0] != 3.1868:
        raise ValueError('Published sample count or source interval changed')

    buffer = io.StringIO(newline='')
    writer = csv.writer(buffer, lineterminator='\n')
    writer.writerow((*HEADERS, 'CL', 'CD'))
    for words, values in rows:
        # Authors' post-processing: pressure + viscous force in flow axes,
        # divided by dynamic pressure (1,500 Pa) and reference area (4 m²).
        cl = (values[2] + values[5]) / REFERENCE['denominator_N']
        cd = (values[1] + values[4]) / REFERENCE['denominator_N']
        writer.writerow((*words, format(cl, '.17g'), format(cd, '.17g')))
    payload = buffer.getvalue().encode('ascii')
    columns = [{'id': 'Time', 'label': 'Solver time', 'unit': 's', 'role': 'time'}]
    for index, column in enumerate(HEADERS[1:], start=1):
        unit = 'N' if index < 7 else 'N m' if index < 10 else '1'
        columns.append({'id': column, 'label': column, 'unit': unit, 'origin': 'source'})
    for column, label, expression in (
        ('CL', 'Lift coefficient', '(Fpy + Fvy) / (0.5 * 1.2 * 50^2 * 4)'),
        ('CD', 'Drag coefficient', '(Fpx + Fvx) / (0.5 * 1.2 * 50^2 * 4)'),
    ):
        columns.append({'id': column, 'label': label, 'unit': '1',
                        'origin': 'derived', 'expression': expression})
    descriptor = {
        'version': 1, 'kind': 'scientific_history',
        'id': manifest['id'], 'title': manifest['title'],
        'sourceURL': manifest['source_url'], 'sourceDOI': manifest['doi'],
        'origin': 'published_recording', 'fieldRecordingId': None,
        'payload': 'history.csv', 'payloadSHA256': digest(payload),
        'sourceSHA256': SOURCE_SHA, 'provenanceSHA256': digest(manifest_bytes),
        'licenseSHA256': NOTICE_SHA,
        'sampleCount': len(rows), 'timeColumn': 'Time',
        'firstTime': rows[0][1][0], 'lastTime': rows[-1][1][0],
        'timeNote': manifest['time']['note'], 'columns': columns,
        'referenceValues': REFERENCE, 'limitations': manifest['limitations'],
    }
    output.mkdir(parents=True, exist_ok=True)
    # Each exported copy carries the entire notice and original provenance.
    atomic_write(output / 'LICENSE.txt', notice)
    atomic_write(output / 'history-provenance.json', manifest_bytes)
    atomic_write(output / 'history.csv', payload)
    atomic_write(output / 'history.json',
                 (json.dumps(descriptor, indent=2, ensure_ascii=False) + '\n').encode())
    if output.resolve() == SAMPLE.resolve():
        catalog_path = ROOT / 'Content/Samples/Registry/histories.json'
        catalog = json.loads(catalog_path.read_text()) if catalog_path.exists() else {'histories': []}
        entries = {entry['id']: entry for entry in catalog['histories']}
        entries[descriptor['id']] = {
            'id': descriptor['id'], 'title': descriptor['title'],
            'path': SAMPLE.name + '/history.json',
            'metadataSHA256': digest((output / 'history.json').read_bytes()),
        }
        catalog_path.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(catalog_path,
                     (json.dumps({'histories': list(entries.values())}, indent=2, ensure_ascii=False) + '\n').encode())
    return descriptor


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, default=SAMPLE)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = convert(args.sample, args.output)
    print(f"Verified {result['sampleCount']} published samples: "
          f"{result['firstTime']}–{result['lastTime']} s; no flow fields.")
    print('History SHA-256:', result['payloadSHA256'])
