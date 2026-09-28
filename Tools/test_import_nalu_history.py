#!/usr/bin/env python3
"""Scientific conversion checks against the retained published originals."""
import csv
from decimal import Decimal
import json
from pathlib import Path
import shutil
import statistics
import tempfile
import unittest

import import_nalu_history as importer


class PublishedHistoryTests(unittest.TestCase):
    def setUp(self):
        scratch = importer.ROOT / 'tmp/debug/history-import-tests'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name)

    def test_every_original_value_and_coefficient(self):
        descriptor = importer.convert(output=self.path)
        source = importer.parse_rows((importer.SAMPLE / importer.SOURCE_NAME).read_bytes())
        with (self.path / 'history.csv').open(newline='') as stream:
            converted = list(csv.reader(stream))
        self.assertEqual(converted.pop(0), [*importer.HEADERS, 'CL', 'CD'])
        self.assertEqual(len(converted), 6967)
        for (words, _), row in zip(source, converted, strict=True):
            self.assertEqual(row[:12], words)
            # Independent decimal arithmetic checks every derived coefficient.
            expected_cl = (Decimal(words[2]) + Decimal(words[5])) / Decimal(6000)
            expected_cd = (Decimal(words[1]) + Decimal(words[4])) / Decimal(6000)
            self.assertLess(abs(Decimal(row[12]) - expected_cl), Decimal('1e-15'))
            self.assertLess(abs(Decimal(row[13]) - expected_cd), Decimal('1e-15'))
        self.assertAlmostEqual(statistics.mean(float(r[12]) for r in converted[-4000:]),
                               .9784452971963332, places=14)
        self.assertAlmostEqual(statistics.mean(float(r[13]) for r in converted[-4000:]),
                               .5163203640541667, places=14)
        self.assertEqual(descriptor['sampleCount'], len(converted))
        self.assertEqual(descriptor['firstTime'], float(converted[0][0]))
        self.assertEqual(descriptor['lastTime'], float(converted[-1][0]))
        self.assertIsNone(descriptor['fieldRecordingId'])
        self.assertEqual(descriptor['payloadSHA256'], importer.digest((self.path / 'history.csv').read_bytes()))
        self.assertEqual((self.path / 'LICENSE.txt').read_bytes(),
                         (importer.SAMPLE / 'source/LICENSE.txt').read_bytes())
        before = {p.name: p.read_bytes() for p in self.path.iterdir()}
        importer.convert(output=self.path)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.path.iterdir()})

    def test_changed_original_cannot_replace_output(self):
        copied = self.path / 'sample'
        shutil.copytree(importer.SAMPLE, copied)
        with (copied / importer.SOURCE_NAME).open('ab') as stream:
            stream.write(b'\n')
        output = self.path / 'output'
        output.mkdir()
        (output / 'history.csv').write_bytes(b'previous accepted output')
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            importer.convert(copied, output)
        self.assertEqual((output / 'history.csv').read_bytes(), b'previous accepted output')
        self.assertEqual(len(list(output.iterdir())), 1)

    def test_changed_normalization_or_notice_is_rejected(self):
        copied = self.path / 'sample'
        shutil.copytree(importer.SAMPLE, copied)
        p = copied / 'provenance.json'
        manifest = json.loads(p.read_text())
        manifest['normalization']['reference_area_m2'] = 1
        p.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'normalization'):
            importer.convert(copied, self.path / 'output')
        shutil.copyfile(importer.SAMPLE / 'provenance.json', p)
        (copied / 'source/LICENSE.txt').write_text('incomplete notice')
        with self.assertRaisesRegex(ValueError, 'notice'):
            importer.convert(copied, self.path / 'output')
        self.assertFalse((self.path / 'output').exists())

    def test_invalid_history_structure_is_rejected(self):
        lines = (importer.SAMPLE / importer.SOURCE_NAME).read_bytes().splitlines()
        invalid = [b'\n'.join([lines[0], lines[1], lines[1]]),
                   lines[0] + b'\n' + lines[1].replace(b'0.4004', b'nan'),
                   b'\n'.join([lines[0], b' '.join(lines[1].split()[:-1])]),
                   lines[0], b'Time,Fpx\n0,1']
        for data in invalid:
            with self.subTest(data=data[:40]), self.assertRaises(ValueError):
                importer.parse_rows(data)


if __name__ == '__main__':
    unittest.main()
