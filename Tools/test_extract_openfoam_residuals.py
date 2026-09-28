"""Structural parser tests. Snippets below are not CFD examples or app samples."""
import csv
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import extract_openfoam_residuals as extractor


class ResidualExtractionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root/'input.log'
        self.output = self.root/'result'
        self.text = ('OpenFOAM: structural parser fixture\nTime = 1.25\nPIMPLE: iteration 1\n'
                     'smoothSolver: Solving for Ux, Initial residual = 1.23456789e-3, Final residual = 2e-6, No Iterations 2\n'
                     'GAMG: Solving for p, Initial residual = 3e-2, Final residual = 4e-7, No Iterations 8\n'
                     'PIMPLE: iteration 2\n'
                     'smoothSolver: Solving for Ux, Initial residual = 2e-6, Final residual = 2e-6, No Iterations 0\n'
                     'GAMG: Solving for p, Initial residual = 7e-4, Final residual = 9e-8, No Iterations 3\n'
                     'Time = 1.375\n'
                     'smoothSolver: Solving for Ux, Initial residual = 5e-5, Final residual = 8e-6, No Iterations 1\n'
                     'GAMG: Solving for p, Initial residual = 6e-4, Final residual = 1e-7, No Iterations 4\nEnd\n')
        self.source.write_text(self.text)

    def test_preserves_tokens_source_lines_and_explicit_selection(self):
        result = extractor.extract(self.source, self.output, fields=['Ux', 'p'])
        self.assertEqual(result['linearSolveRecords'], 6)
        self.assertEqual(result['timeSteps'], 2)
        self.assertEqual(result['sourceSHA256'], hashlib.sha256(self.text.encode()).hexdigest())
        self.assertEqual((self.output/'source.log').read_text(), self.text)
        with (self.output/'residual-records.csv').open() as stream:
            records = list(csv.DictReader(stream))
        self.assertEqual(records[0]['InitialResidual'], '1.23456789e-3')
        self.assertEqual(records[0]['SourceLine'], '4')
        self.assertEqual(records[2]['PIMPLEIteration'], '2')
        self.assertEqual(records[4]['PIMPLEIteration'], '')
        with (self.output/'timestep-history.csv').open() as stream:
            summary = list(csv.DictReader(stream))
        self.assertEqual(summary[0], {'Time': '1.25', 'Ux.InitialFirst': '1.23456789e-3', 'Ux.FinalLast': '2e-6', 'Ux.SolveCount': '2',
                                     'p.InitialFirst': '3e-2', 'p.FinalLast': '9e-8', 'p.SolveCount': '2'})
        self.assertEqual(summary[1]['Time'], '1.375')
        for name, details in result['files'].items():
            data = (self.output/name).read_bytes()
            self.assertEqual(details['bytes'], len(data))
            self.assertEqual(details['sha256'], hashlib.sha256(data).hexdigest())

    def test_missing_corrupt_or_nonincreasing_values_fail_transactionally(self):
        variations = [self.text.replace('Time = 1.375', 'Time = 1.25'), self.text.replace('Time = 1.375', 'Time = NaN'),
                      self.text.replace('1.23456789e-3', 'nan'), self.text.replace('1.23456789e-3', '-1'),
                      self.text.replace('1.23456789e-3', '1e999'), self.text.replace('1.23456789e-3', '1e-999'),
                      self.text.replace('No Iterations 2', 'No Iterations -1'),
                      self.text.replace('Time = 1.375', 'Time = 1.375\nTime = 1.5'), self.text.removesuffix('End\n'),
                      self.text + 'Time = 2\n', self.text.replace('OpenFOAM:', 'Unknown:')]
        for text in variations:
            with self.subTest(text=text[-80:]):
                self.source.write_text(text)
                with self.assertRaises(ValueError):
                    extractor.extract(self.source, self.output)
                self.assertFalse(self.output.exists())
                self.assertFalse(list(self.root.glob('.result-*')))

    def test_missing_field_not_filled_with_zero_and_original_not_overwritten(self):
        with self.assertRaisesRegex(ValueError, 'omits selected fields'):
            extractor.extract(self.source, self.output, fields=['Ux', 'Uz'])
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            extractor.extract(self.source, self.output, expected_sha256='0'*64)
        self.output.mkdir()
        (self.output/'keep').write_text('existing output')
        with self.assertRaisesRegex(ValueError, 'existing results'):
            extractor.extract(self.source, self.output)
        self.assertEqual((self.output/'keep').read_text(), 'existing output')
        self.assertEqual(self.source.read_text(), self.text)

    def test_record_budget_prevents_publication(self):
        with mock.patch.object(extractor, 'MAX_STEP_RECORDS', 3):
            with self.assertRaisesRegex(ValueError, 'budget'):
                extractor.extract(self.source, self.output)
        self.assertFalse(self.output.exists())
        with mock.patch.object(extractor, 'MAX_STEPS', 1):
            with self.assertRaisesRegex(ValueError, 'budget'):
                extractor.extract(self.source, self.output)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
