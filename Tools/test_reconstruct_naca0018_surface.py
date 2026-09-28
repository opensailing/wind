"""Structural geometry tests, not synthetic CFD examples."""
import tempfile
from pathlib import Path
import unittest

import numpy as np
from reconstruct_naca0018_surface import reconstruct, triangulate_fluid_surface, write_indices, sha256


class ReconstructionTests(unittest.TestCase):
    def setUp(self):
        self.xy = np.array([[-2, -2], [2, -2], [2, 2], [-2, 2],
                            [-.5, -.5], [.5, -.5], [.5, .5], [-.5, .5]])
        self.hole = np.array([4, 5, 6, 7])

    def test_retains_points_and_excludes_hole(self):
        triangles, stats = triangulate_fluid_surface(self.xy, self.hole)
        self.assertEqual(triangles.shape, (8, 3))
        self.assertEqual(stats['removedSolidTriangles'], 2)
        self.assertEqual(stats['solidPolygonAreaM2'], 1)
        self.assertEqual(stats['boundaryFluidNeighbours'], 1)
        self.assertEqual(stats['eulerCharacteristic'], 0)
        np.testing.assert_array_equal(np.unique(triangles), np.arange(8))
        np.testing.assert_array_equal(self.xy[self.hole], [[-.5, -.5], [.5, -.5], [.5, .5], [-.5, .5]])

    def test_rejects_ambiguous_or_invalid_inputs(self):
        invalid = [self.hole[::-1], np.array([4, 5, 5, 7]), np.array([4, 5, 6, 8]),
                   np.array([4., 5., 6., 7.]), np.array([4, 5]), np.array([-1, 5, 6, 7])]
        for boundary in invalid:
            with self.subTest(boundary=boundary), self.assertRaises(ValueError):
                triangulate_fluid_surface(self.xy, boundary)
        for xy in (np.vstack((self.xy, self.xy[0])), self.xy * float('nan'), self.xy[:, :1]):
            with self.subTest(shape=xy.shape), self.assertRaises(ValueError):
                triangulate_fluid_surface(xy, self.hole)

    def test_rejects_a_hole_covering_source_samples(self):
        with self.assertRaisesRegex(ValueError, 'contains additional source samples'):
            triangulate_fluid_surface(np.vstack((self.xy, [0, 0])), self.hole)

    def test_binary_contract_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            path = root / 'indices.u32'
            values = np.array([[0, 1, 2], [3, 4, 5]])
            meta = write_indices(path, values)
            self.assertEqual(meta['byteLength'], 24)
            self.assertEqual(meta['sha256'], sha256(path))
            self.assertEqual(meta['shape'], [2, 3])
            np.testing.assert_array_equal(np.fromfile(path, dtype='<u4').reshape(2, 3), values)
            with self.assertRaisesRegex(ValueError, 'existing results'):
                reconstruct(root / 'missing.h5', root / 'missing.json', root)
            self.assertEqual(path.stat().st_size, 24)

    def test_rejects_unverified_source_before_publishing(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            descriptor = root / 'recording.json'
            descriptor.write_text('{}')
            output = root / 'result'
            with self.assertRaisesRegex(ValueError, 'exact audited original'):
                reconstruct(root / 'missing.h5', descriptor, output)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
