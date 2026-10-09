#!/usr/bin/env python3
"""Independently compare native selected residuals to every original time block.

Uses the previously audited Python regex parser, independent of the native
label/partition parser. No interpolated or generated scientific values are used.
"""
import argparse
import csv
import hashlib
import json
import itertools
from pathlib import Path

from extract_openfoam_residuals import parse_steps


def verify(source, native, expected_sha256):
    source, native = Path(source), Path(native)
    with source.open('rb') as stream:
        source_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
    if source_hash != expected_sha256:
        raise ValueError('Original source differs from the requested published log')
    times = records = values = 0
    with source.open(encoding='ascii') as original, native.open(newline='') as captured:
        rows = csv.DictReader(captured)
        if rows.fieldnames != ['Time', 'TimeSourceLine', 'Series', 'SourceLine', 'Value']:
            raise ValueError('Native audit has unsupported columns')
        time_lines = []

        def lines():
            for line_number, line in enumerate(original, 1):
                if line.strip().startswith('Time = '):
                    time_lines.append(line_number)
                yield line

        for step in parse_steps(lines()):
            fields = {}
            for record in step.records:
                fields.setdefault(record.field, []).append(record)
            for field, solves in sorted(fields.items()):
                for suffix, record, token in [('InitialFirst', solves[0], solves[0].initial),
                                               ('FinalLast', solves[-1], solves[-1].final)]:
                    row = next(rows, None)
                    if row is None:
                        raise ValueError(f'Native history ends before source time {step.time}')
                    if (float(row['Time']) != float(step.time) or int(row['TimeSourceLine']) != time_lines[times]
                            or row['Series'] != f'{field}.{suffix}' or int(row['SourceLine']) != record.line
                            or float(row['Value']) != float(token)):
                        raise ValueError(f'Native residual differs at time {step.time}, {field}.{suffix}: {row}')
                    values += 1
            times += 1
            records += len(step.records)
        if next(rows, None) is not None:
            raise ValueError('Native history contains extra values')
    with source.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != source_hash:
            raise ValueError('Original source changed during comparison')
    return {'source_sha256': source_hash, 'native_sha256': hashlib.sha256(native.read_bytes()).hexdigest(),
            'source_time_blocks': times, 'original_linear_solves': records,
            'selected_values_and_source_lines_compared': values,
            'selection': 'first initial and last final for every field at every original time',
            'numeric_comparison': 'exact double equality to each original decimal token', 'passed': True}


def verify_export(native, exported, source_hash):
    """Compare the production CSV to every independently checked native audit row."""
    native, exported = Path(native), Path(exported)
    time_count = values = 0
    with native.open(newline='') as audit, exported.open(newline='') as output:
        data_lines = []
        metadata = []
        for line in output:
            (metadata if line.startswith('#') else data_lines).append(line)
        if f'# original_source_sha256,{source_hash}\n' not in metadata:
            raise ValueError('Residual export lost original source identity')
        if '# source_line_numbering,one-based in the hash-verified original log\n' not in metadata:
            raise ValueError('Residual export lost source-line definition')
        rows = csv.DictReader(data_lines)
        for key, group in itertools.groupby(csv.DictReader(audit), key=lambda r: (float(r['Time']), int(r['TimeSourceLine']))):
            selected = list(group)
            columns = ['source_sample', 'solver_time (source units)', 'time_source_line']
            columns += [c for item in selected for c in [item['Series'], item['Series']+'.source_line']]
            if rows.fieldnames != columns:
                raise ValueError('Residual export columns differ from the selected original series')
            row = next(rows, None)
            if (row is None or int(row['source_sample']) != time_count
                    or float(row['solver_time (source units)']) != key[0] or int(row['time_source_line']) != key[1]):
                raise ValueError(f'Residual export has wrong source time/line at row {time_count}')
            for item in selected:
                if (float(row[item['Series']]) != float(item['Value'])
                        or int(row[item['Series']+'.source_line']) != int(item['SourceLine'])):
                    raise ValueError(f'Residual export has wrong value/source line at row {time_count}: {item["Series"]}')
                values += 1
            time_count += 1
        if next(rows, None) is not None:
            raise ValueError('Residual export contains extra rows')
    return {'sha256': hashlib.sha256(exported.read_bytes()).hexdigest(), 'time_rows': time_count,
            'values_and_source_lines_compared': values, 'passed': True}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('native', type=Path)
    parser.add_argument('--expected-sha256', required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--export-csv', type=Path)
    args = parser.parse_args()
    report = verify(args.source, args.native, args.expected_sha256)
    if args.export_csv:
        report['export'] = verify_export(args.native, args.export_csv, report['source_sha256'])
    text = json.dumps(report, indent=2)+'\n'
    if args.report:
        args.report.write_text(text)
    print(text, end='')
