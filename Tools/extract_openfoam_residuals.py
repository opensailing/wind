#!/usr/bin/env python3
"""Extract actual OpenFOAM residual records without inventing missing samples.

Keeps every reported solve and an explicitly labelled per-timestep selection of
the first initial and last final residual for each requested field. The output
is an external analysis directory, not a registered/redistributable app sample.
Original source bytes and value lexemes are retained. No source code is run.
"""
import argparse
import csv
from dataclasses import dataclass
from decimal import Decimal
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import tempfile

MAX_SOURCE_BYTES = 256 * 1024 * 1024
MAX_LINE_BYTES = 32768
MAX_STEPS = 1000000
MAX_RECORDS = 2000000
MAX_STEP_RECORDS = 4096
NUMBER = r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?'
FIELD = r'[A-Za-z_][A-Za-z0-9_.:()]*'
TIME = re.compile(rf'^Time\s*=\s*({NUMBER})\s*$')
SOLVE = re.compile(
    rf'^(\S+):\s+Solving for ({FIELD}),\s+Initial residual\s*=\s*({NUMBER}),'
    rf'\s+Final residual\s*=\s*({NUMBER}),\s+No Iterations\s+([0-9]+)\s*$')
OUTER = re.compile(r'^PIMPLE:\s+iteration\s+([0-9]+)\s*$')


@dataclass(frozen=True)
class Residual:
    line: int
    solver: str
    field: str
    initial: str
    final: str
    iterations: str
    outer_iteration: str


@dataclass(frozen=True)
class TimeStep:
    time: str
    records: tuple[Residual, ...]


def finite(token, line, nonnegative=False):
    value = float(token)
    if len(token) > 64 or not math.isfinite(value) or (nonnegative and value < 0) or (value == 0 and Decimal(token) != 0):
        raise ValueError(f'Invalid numeric value at source line {line}')
    return value


def parse_steps(lines):
    """Yield complete original time blocks, refusing truncated/concatenated logs.

    Structural parser tests may use tiny handwritten log snippets. Such snippets
    are never simulation samples or installed data. Actual acceptance uses the
    published original log and independently checks every extracted value.
    """
    time = None
    previous = -math.inf
    records = []
    outer = ''
    ended = False
    banner = False
    steps = total_records = 0
    for line_number, original in enumerate(lines, start=1):
        if len(original) > MAX_LINE_BYTES:
            raise ValueError(f'Oversized source line {line_number}')
        text = original.strip()
        if 'OpenFOAM:' in text:
            banner = True
        match = TIME.fullmatch(text)
        if match:
            if ended or not banner:
                raise ValueError(f'Unexpected time block at source line {line_number}')
            value = finite(match[1], line_number)
            if value <= previous:
                raise ValueError(f'Times must increase strictly (source line {line_number})')
            if time is not None:
                if not records:
                    raise ValueError(f'Time {time} has no reported linear solves')
                yield TimeStep(time, tuple(records))
            time, previous, records, outer = match[1], value, [], ''
            steps += 1
            if steps > MAX_STEPS:
                raise ValueError('Time-step budget exceeded')
            continue
        if text.startswith('Time ='):
            raise ValueError(f'Malformed time at source line {line_number}')
        match = OUTER.fullmatch(text)
        if match:
            if ended or time is None:
                raise ValueError(f'Outer iteration without a time at source line {line_number}')
            outer = match[1]
            continue
        match = SOLVE.fullmatch(text)
        if match:
            if ended or time is None:
                raise ValueError(f'Linear solve without a time at source line {line_number}')
            finite(match[3], line_number, True)
            finite(match[4], line_number, True)
            if len(match[2]) > 128 or len(match[5]) > 10 or int(match[5]) > 1000000000:
                raise ValueError(f'Invalid solve metadata at source line {line_number}')
            records.append(Residual(line_number, match[1], match[2], match[3], match[4], match[5], outer))
            total_records += 1
            if len(records) > MAX_STEP_RECORDS or total_records > MAX_RECORDS:
                raise ValueError('Residual-record budget exceeded')
            continue
        if 'Solving for ' in text:
            raise ValueError(f'Unrecognized residual record at source line {line_number}')
        if text == 'End':
            if ended or time is None or not records:
                raise ValueError(f'Unexpected end marker at source line {line_number}')
            yield TimeStep(time, tuple(records))
            time, records, ended = None, [], True
    if not ended:
        raise ValueError('Incomplete solver log: no final End marker')


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def extract(source, output, source_url='', expected_sha256=None, fields=None):
    source, output = Path(source), Path(output)
    if output.exists():
        raise ValueError('Choose a new output directory; existing results are retained')
    if not 0 < source.stat().st_size <= MAX_SOURCE_BYTES:
        raise ValueError('Source must be a nonempty log of at most 256 MiB')
    if fields is not None:
        fields = tuple(fields)
        if not fields or len(fields) > 32 or len(set(fields)) != len(fields) or any(not re.fullmatch(FIELD, f) for f in fields):
            raise ValueError('Choose 1–32 unique source field names')
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.' + output.name + '-', dir=output.parent))
    try:
        source_hash = hashlib.sha256()
        source_bytes = 0
        with source.open('rb') as original, (stage/'source.log').open('wb') as preserved:
            initial_stat = os.fstat(original.fileno())

            def lines():
                nonlocal source_bytes
                while raw := original.readline(MAX_LINE_BYTES + 1):
                    source_bytes += len(raw)
                    if source_bytes > MAX_SOURCE_BYTES or len(raw) > MAX_LINE_BYTES:
                        raise ValueError('Source byte/line budget exceeded')
                    source_hash.update(raw)
                    preserved.write(raw)
                    yield raw.decode('ascii')

            with (stage/'residual-records.csv').open('w', newline='') as all_file, (stage/'timestep-history.csv').open('w', newline='') as summary_file:
                all_rows, summary = csv.writer(all_file, lineterminator='\n'), csv.writer(summary_file, lineterminator='\n')
                all_rows.writerow(('Time', 'SourceLine', 'Solver', 'Field', 'InitialResidual', 'FinalResidual', 'LinearIterations', 'PIMPLEIteration'))
                step_count = record_count = 0
                counts = {}
                first_time = last_time = None
                for step in parse_steps(lines()):
                    if fields is None:
                        fields = tuple(dict.fromkeys(r.field for r in step.records))
                        if len(fields) > 32:
                            raise ValueError('More than 32 fields; choose the summary fields explicitly')
                    if step_count == 0:
                        first_time = step.time
                        summary.writerow(('Time', *(f'{f}.{suffix}' for f in fields for suffix in ('InitialFirst', 'FinalLast', 'SolveCount'))))
                    selected = {f: [] for f in fields}
                    for r in step.records:
                        all_rows.writerow((step.time, r.line, r.solver, r.field, r.initial, r.final, r.iterations, r.outer_iteration))
                        counts[r.field] = counts.get(r.field, 0) + 1
                        if r.field in selected:
                            selected[r.field].append(r)
                    if any(not values for values in selected.values()):
                        missing = ', '.join(f for f, values in selected.items() if not values)
                        raise ValueError(f'Time {step.time} omits selected fields: {missing}; no zero samples are substituted')
                    summary.writerow((step.time, *(v for f in fields for v in (selected[f][0].initial, selected[f][-1].final, len(selected[f])))))
                    last_time = step.time
                    step_count += 1
                    record_count += len(step.records)
            final_stat = os.fstat(original.fileno())
            if (initial_stat.st_size, initial_stat.st_mtime_ns) != (final_stat.st_size, final_stat.st_mtime_ns) or source_bytes != initial_stat.st_size:
                raise ValueError('Source changed while reading; retry a completed immutable log')
        digest = source_hash.hexdigest()
        if expected_sha256 and digest != expected_sha256.lower():
            raise ValueError('Original source SHA-256 does not match the expected recording')
        manifest = {
            'version': 1, 'kind': 'openfoam_residual_extraction', 'sourceURL': source_url,
            'sourceSHA256': digest, 'sourceBytes': source_bytes,
            'timeSteps': step_count, 'linearSolveRecords': record_count,
            'recordsPerField': counts, 'summaryFields': list(fields),
            'firstTime': first_time, 'lastTime': last_time,
            'timeMeaning': 'Original Time = values; no offset, resampling, interpolation or concatenation.',
            'residualMeaning': 'Original solver-reported initial/final residual tokens. No renormalization or cross-solver equivalence is asserted.',
            'summarySelection': 'For each source time and field: first reported initial residual, last reported final residual, and count of its linear solves. Full records remain in residual-records.csv.',
            'sourceLineMeaning': 'One-based line number in the preserved original log.',
            'limitations': ['Residual history only; no new flow fields, forces or measured GPU metrics.',
                            'Independent source log; no field-recording association is inferred.',
                            'Source permission/attribution must be established separately before redistributing this output.',
                            'External extraction only; not added to the application catalog.'],
            'files': {name: {'sha256': sha256(stage/name), 'bytes': (stage/name).stat().st_size}
                      for name in ('source.log', 'residual-records.csv', 'timestep-history.csv')},
            'extractorSHA256': sha256(__file__),
        }
        (stage/'extraction.json').write_text(json.dumps(manifest, indent=2)+'\n')
        os.rename(stage, output)
        return manifest
    except BaseException:
        shutil.rmtree(stage)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--source-url', default='')
    parser.add_argument('--expected-sha256')
    parser.add_argument('--fields', nargs='+')
    args = parser.parse_args()
    result = extract(args.source, args.output, args.source_url, args.expected_sha256, args.fields)
    print(f"Preserved {result['linearSolveRecords']} actual linear-solve records across {result['timeSteps']} source times in {args.output}")
