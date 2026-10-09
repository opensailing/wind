"""Real CAD kernel oracles. Invoke with FreeCAD's headless Python."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(sys.executable).resolve().parent.parent / "lib"))
import FreeCAD
import Part
from home4_cad_prepare import prepare


class CADPreparation(unittest.TestCase):
    def test_step_box_volume_identity_and_no_replace(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "box.step"
            Part.makeBox(2, 3, 4).exportStep(str(source))
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            manifest = prepare(source, root / "box.stl", .05, expected_sha256=digest)
            self.assertAlmostEqual(manifest["volume_source_units3"], 24)
            self.assertTrue(manifest["closed"])
            self.assertEqual(manifest["source_sha256"], digest)
            self.assertGreaterEqual(manifest["triangles"], 12)
            before = (root / "box.stl").read_bytes()
            with self.assertRaises(ValueError):
                prepare(source, root / "box.stl", .05)
            self.assertEqual((root / "box.stl").read_bytes(), before)
            with self.assertRaises(ValueError):
                prepare(source, root / "other.stl", .05, expected_sha256="0" * 64)
            self.assertFalse((root / "other.stl").exists())

    def test_iges_box_and_invalid_tolerance(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "box.iges"
            Part.makeBox(2, 3, 4).exportIges(str(source))
            manifest = prepare(source, root / "box.stl", .05)
            self.assertTrue(manifest["source_coordinates_preserved"])
            self.assertGreater(manifest["triangles"], 0)
            with self.assertRaises(ValueError):
                prepare(source, root / "bad.stl", float("nan"))


if __name__ == "__main__":
    unittest.main()
