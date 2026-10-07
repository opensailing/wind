#!/usr/bin/env python3
"""Artificial unit fixtures for HOME4 archive contracts; these are not solver results."""
from __future__ import annotations
import contextlib
import dataclasses
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import warnings
import xml.etree.ElementTree as ET
import zipfile
import zlib

import numpy as np

from home4_archive import (ArchiveError, Limits, Mapping, SafeNPZ, bounded_json, convert_snapshots,
                           derivative_fields, derive_fields, export_vti, inspect_archive, main,
                           mapping_from_run_spec, parse_crop, range_report, read_snapshot, _publish_new_directory)


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[1] / "tmp/analysis/home4-feedback-20261007"
        scratch.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="home4-artificial-fixtures-", dir=scratch)
        self.root = Path(self.directory.name)
        self.mapping = Mapping("xyz", "lattice", "lattice", dx_m=.1, dt_s=.02, rho_ref_kg_m3=1000., metadata_order="xyz")

    def tearDown(self):
        self.directory.cleanup()

    def source(self, name="artificial.npz", *, shape=(5, 6, 7), axis_order="xyz", **overrides):
        x, y, z = np.meshgrid(np.arange(shape[0]), np.arange(shape[1]), np.arange(shape[2]), indexing="ij")
        values = dict(ux=(2*x + 3*y + 4*z).astype(float), uy=(5*x + 7*y + 11*z).astype(float),
                      uz=(13*x + 17*y + 19*z).astype(float), phi=np.ones(shape), solid=np.zeros(shape, dtype=np.uint8),
                      iteration=np.array(10, dtype=np.int64), origin=np.array([1., 2., 3.]), spacing=np.array([1., 1., 1.]))
        values.update(overrides)
        if axis_order != "xyz":
            for key in ("ux", "uy", "uz", "phi", "solid"):
                values[key] = np.transpose(values[key], tuple("xyz".index(a) for a in axis_order))
        path = self.root / name
        np.savez_compressed(path, **values)
        return path

    def malformed_zip(self, name, members):
        path = self.root / name
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                for key, data in members:
                    archive.writestr(key, data)
        return path

    def npy(self, array):
        stream = io.BytesIO()
        np.save(stream, array, allow_pickle=True)
        return stream.getvalue()

    def test_inspect_scalar_metadata_and_source_hash(self):
        source = self.source(run_spec=np.array(json.dumps({"units": {"dxMeters": .1}, "recipeId": "artificial-unit-fixture"})),
                             title=np.array("Artificial fixture, not authentic CFD"))
        report = inspect_archive(source)
        self.assertEqual(report["sourceSHA256"], hashlib.sha256(source.read_bytes()).hexdigest())
        self.assertEqual(report["members"]["iteration"]["value"], 10)
        self.assertEqual(report["runSpec"]["recipeId"], "artificial-unit-fixture")
        self.assertEqual(report["members"]["phi"]["shape"], [5, 6, 7])
        self.assertTrue(report["members"]["ux"]["finite"])

    def test_axis_order_and_crop_preserve_original_xyz_coordinates(self):
        source = self.source(axis_order="zyx")
        mapping = dataclasses.replace(self.mapping, axis_order="zyx")
        snapshot = read_snapshot(source, mapping, crop=parse_crop("1:5,2:6,1:7"), stride=2)
        self.assertEqual(snapshot.shape, (2, 2, 3))
        np.testing.assert_array_equal(snapshot.origin, [2, 4, 4])
        np.testing.assert_array_equal(snapshot.spacing, [2, 2, 2])
        self.assertEqual(snapshot.fields["ux"][1, 1, 2], 2*3 + 3*4 + 4*5)
        self.assertEqual(snapshot.original_shape, (5, 6, 7))
        self.assertAlmostEqual(snapshot.physical_time, .2)
        with self.assertRaises(ArchiveError):
            read_snapshot(source, mapping, crop=parse_crop("1:9,0:6,0:7"))

    def test_missing_conventions_and_physical_map_are_rejected(self):
        source = self.source()
        with self.assertRaises(ArchiveError):
            read_snapshot(source, dataclasses.replace(self.mapping, axis_order=""))
        with self.assertRaises(ArchiveError):
            convert_snapshots([source], self.root / "missing-map", Mapping("xyz", "lattice", "lattice", metadata_order="xyz"),
                              source_url="urn:artificial-unit-fixture", attribution="Artificial fixture")
        self.assertFalse((self.root / "missing-map").exists())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            main(["vti", str(source), "--output", str(self.root / "missing-conventions")])
        self.assertEqual(error.exception.code, 2)
        with self.assertRaisesRegex(ArchiveError, "metadata order"):
            read_snapshot(source, dataclasses.replace(self.mapping, metadata_order=None))

    def test_explicit_metadata_component_order(self):
        source = self.source(axis_order="zyx", origin=np.array([3., 2., 1.]), spacing=np.array([3., 2., 1.]))
        snapshot = read_snapshot(source, dataclasses.replace(self.mapping, axis_order="zyx", metadata_order="array"))
        np.testing.assert_array_equal(snapshot.origin, [1, 2, 3])
        np.testing.assert_array_equal(snapshot.spacing, [1, 2, 3])

    def test_reject_object_dtype_without_running_pickle(self):
        source = self.source(payload=np.array({"unsafe": "object"}, dtype=object))
        with self.assertRaisesRegex(ArchiveError, "dtype"):
            inspect_archive(source)

    def test_reject_traversal_duplicates_and_unbounded_zip(self):
        data = self.npy(np.array(1))
        for name, members in (("traversal.npz", [("../iteration.npy", data)]),
                              ("absolute.npz", [("/iteration.npy", data)]),
                              ("duplicate.npz", [("iteration.npy", data), ("iteration.npy", data)]),
                              ("case-duplicate.npz", [("iteration.npy", data), ("ITERATION.npy", data)])):
            with self.subTest(name=name), self.assertRaises(ArchiveError):
                SafeNPZ(self.malformed_zip(name, members))
        source = self.source()
        with self.assertRaisesRegex(ArchiveError, "expands"):
            SafeNPZ(source, Limits(max_uncompressed_bytes=100))
        with self.assertRaisesRegex(ArchiveError, "compression ratio"):
            SafeNPZ(source, Limits(max_compression_ratio=1))
        with self.assertRaisesRegex(ArchiveError, "member count"):
            SafeNPZ(source, Limits(max_members=2))

    def test_reject_false_npy_size_nonfinite_shapes_and_masks(self):
        data = self.npy(np.arange(5, dtype=np.float64))
        with self.assertRaisesRegex(ArchiveError, "payload size"):
            SafeNPZ(self.malformed_zip("short.npy.npz", [("x.npy", data[:-1])]))
        source = self.source(ux=np.full((5, 6, 7), np.nan))
        with self.assertRaisesRegex(ArchiveError, "Nonfinite"):
            read_snapshot(source, self.mapping)
        source = self.source("shape.npz", ux=np.zeros((2, 2, 2)))
        with self.assertRaisesRegex(ArchiveError, "shape"):
            read_snapshot(source, self.mapping)
        source = self.source("solid.npz", solid=np.full((5, 6, 7), 2))
        with self.assertRaisesRegex(ArchiveError, "binary"):
            read_snapshot(source, self.mapping)
        source = self.source("fractional-step.npz", iteration=np.array(2.5))
        with self.assertRaisesRegex(ArchiveError, "iteration"):
            read_snapshot(source, self.mapping)
        source = self.source("large-integers.npz", ux=np.full((5, 6, 7), 2**53 + 1, dtype=np.int64))
        with self.assertRaisesRegex(ArchiveError, "integer source precision"):
            read_snapshot(source, self.mapping)

    def test_independent_linear_velocity_derivative_oracles(self):
        source = self.source()
        snapshot = read_snapshot(source, self.mapping)
        d = derivative_fields(snapshot)
        valid = d["derivative_valid"] > 0
        self.assertEqual(valid.sum(), 3*4*5)
        # Independent exact derivatives of u=(2x+3y+4z,5x+7y+11z,13x+17y+19z).
        # curl=(17-11,4-13,5-3), div=2+7+19; Q=-1/2 trace(A^2).
        A = [[2, 3, 4], [5, 7, 11], [13, 17, 19]]
        trace_a2 = sum(A[i][j]*A[j][i] for i in range(3) for j in range(3))
        for key, expected in (("vorticity_x", 6), ("vorticity_y", -9), ("vorticity_z", 2),
                              ("divergence", 28), ("q", -.5*trace_a2)):
            np.testing.assert_allclose(d[key][valid], expected)
            self.assertTrue(np.all(d[key][~valid] == 0))
        self.assertAlmostEqual(d["helicity"][2, 2, 2], (2*2+3*2+4*2)*6 - (5*2+7*2+11*2)*9 + (13*2+17*2+19*2)*2)

    def test_air_and_solid_one_node_stencil_exclusion(self):
        phi = np.ones((5, 6, 7));phi[2, 2, 2] = 0
        solid = np.zeros_like(phi);solid[3, 3, 3] = 1
        snapshot = read_snapshot(self.source(phi=phi, solid=solid), self.mapping)
        derived = derive_fields(snapshot, derivatives=True)
        mask = derived["derivative_valid"]
        for point in ((2, 2, 2), (1, 2, 2), (3, 2, 2), (2, 1, 2), (2, 3, 2), (2, 2, 1), (2, 2, 3),
                      (3, 3, 3), (2, 3, 3), (3, 2, 3), (3, 3, 2)):
            self.assertEqual(mask[point], 0)
        ranges = range_report(snapshot, derived)
        self.assertTrue(ranges["ranges"]["q"]["airMaskDefault"])
        self.assertEqual(ranges["ranges"]["q"]["sampleCount"], int(mask.sum()))
        self.assertEqual(ranges["ranges"]["q"]["firstFramePercentile99"], derived["q"][mask > 0][0])
        self.assertEqual(mask[1, 1, 1], 0)  # Conservative diagonal halo exclusion.

    def test_preview_derivatives_use_original_resolution_and_mask(self):
        shape = (7, 7, 7)
        phi = np.ones(shape);phi[3, 3, 3] = 0
        x, _y, _z = np.meshgrid(*(np.arange(7),)*3, indexing="ij")
        source = self.source(shape=shape, phi=phi, ux=x.astype(float)**3)
        original = derivative_fields(read_snapshot(source, self.mapping))
        preview = derivative_fields(read_snapshot(source, self.mapping, crop=parse_crop("1:6,1:6,1:6"), stride=2))
        selection = np.ix_(np.array([1, 3, 5]), np.array([1, 3, 5]), np.array([1, 3, 5]))
        for key in preview:
            np.testing.assert_array_equal(preview[key], original[key][selection])

    def test_phase_overshoot_is_preserved_and_reported(self):
        phi = np.ones((5, 6, 7));phi[2, 2, 2] = 1.0001
        snapshot = read_snapshot(self.source(phi=phi), self.mapping)
        self.assertEqual(snapshot.fields["phi"][2, 2, 2], 1.0001)
        self.assertEqual(range_report(snapshot, derive_fields(snapshot))["phiOutsideUnitInterval"], 1)

    def test_pressure_and_stored_strain_dissipation_are_explicit(self):
        shape = (5, 6, 7)
        constants = {"rho": np.full(shape, 3.), "p_star": np.full(shape, 2.), "Pi_h": np.full(shape, 7.),
                     "Pi_h0": np.full(shape, 5.), "nu": np.full(shape, .1)}
        for key, value in zip(("Sxx", "Syy", "Szz", "Sxy", "Sxz", "Syz"), (1, 2, 3, 4, 5, 6)):
            constants[key] = np.full(shape, float(value))
        snapshot = read_snapshot(self.source(**constants), self.mapping)
        derived = derive_fields(snapshot, pressure_convention="wb_lattice")
        np.testing.assert_allclose(derived["pressure"], 4.)
        np.testing.assert_allclose(derived["dissipation"], .2*(1+4+9+2*(16+25+36)))
        self.assertNotIn("pressure", derive_fields(snapshot))
        missing = read_snapshot(self.source("no-pressure.npz"), self.mapping)
        with self.assertRaisesRegex(ArchiveError, "WB pressure needs"):
            derive_fields(missing, pressure_convention="wb_lattice")

    def test_native_conversion_matches_binary_values_hashes_and_original_ids(self):
        first = self.source("first.npz", iteration=np.array(10))
        second = self.source("second.npz", iteration=np.array(20))
        crop = parse_crop("1:5,2:6,1:7")
        output = self.root / "native"
        d = convert_snapshots([first, second], output, self.mapping, crop=crop, stride=2,
                              source_url="urn:artificial-unit-fixture", attribution="Artificial arrays for unit testing only.")
        self.assertEqual(d["frameCount"], 2);self.assertEqual(d["pointCount"], 12)
        self.assertEqual([f["index"] for f in d["frames"]], [10, 20])
        self.assertEqual([f["time"] for f in d["frames"]], [.2, .4])
        coordinates = np.fromfile(output / "coordinates.f64", dtype="<f8").reshape(-1, 3)
        np.testing.assert_allclose(coordinates[:4], [[.2, .4, .4], [.4, .4, .4], [.2, .6, .4], [.4, .6, .4]])
        ids = np.fromfile(output / "point-ids.i64", dtype="<i8")
        self.assertEqual(ids[0], 1+5*(2+6*1));self.assertEqual(ids[1], 3+5*(2+6*1))
        ux = np.fromfile(output / "ux.f64", dtype="<f8").reshape(2, -1)
        self.assertEqual(ux[0, 0], (2*1+3*2+4*1)*5)
        for field in d["fields"]:
            path = output / field["array"]["path"]
            data = path.read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), field["array"]["sha256"])
            self.assertEqual(zlib.crc32(data[:12*8]), field["array"]["frameCRC32"][0])
        self.assertEqual(hashlib.sha256((output / "provenance.json").read_bytes()).hexdigest(), d["provenanceSHA256"])
        self.assertFalse((output / "volume_reconstruction.json").exists())
        p = json.loads((output / "provenance.json").read_text())
        manifest = output / p["sourcesManifest"]["path"]
        self.assertEqual(hashlib.sha256(manifest.read_bytes()).hexdigest(), p["sourcesManifest"]["sha256"])
        snapshots = json.loads(manifest.read_text())["snapshots"]
        self.assertEqual(snapshots[0]["sourceSHA256"], hashlib.sha256(first.read_bytes()).hexdigest())
        self.assertTrue(snapshots[0]["previewOnly"])

    def test_transactional_sequence_failures_and_existing_output(self):
        first = self.source("first.npz", iteration=np.array(10))
        wrong = self.source("wrong.npz", iteration=np.array(9))
        output = self.root / "rollback"
        with self.assertRaisesRegex(ArchiveError, "increasing"):
            convert_snapshots([first, wrong], output, self.mapping, source_url="urn:artificial", attribution="Artificial fixture")
        self.assertFalse(output.exists());self.assertFalse(list(self.root.glob(".rollback-*")))
        output.mkdir();(output / "keep.txt").write_text("existing")
        with self.assertRaisesRegex(ArchiveError, "never overwritten"):
            convert_snapshots([first], output, self.mapping, source_url="urn:artificial", attribution="Artificial fixture")
        self.assertEqual((output / "keep.txt").read_text(), "existing")

    def test_atomic_exclusive_publication_does_not_replace_racing_empty_directory(self):
        stage, output = self.root / "stage", self.root / "existing-empty"
        stage.mkdir();(stage / "retained.txt").write_text("staged")
        output.mkdir()
        with self.assertRaisesRegex(ArchiveError, "publication failed"):
            _publish_new_directory(stage, output)
        self.assertEqual(list(output.iterdir()), [])
        self.assertEqual((stage / "retained.txt").read_text(), "staged")

    def test_vti_raw_payload_grid_order_masks_and_units(self):
        source = self.source()
        output = self.root / "vti"
        report = export_vti([source], output, Mapping("xyz", "lattice", "lattice", metadata_order="xyz"), derivatives=True)
        self.assertEqual(report["coordinateUnit"], "lu_length");self.assertEqual(report["timelineUnit"], "steps")
        path = output / "snapshot-0000000010.vti"
        raw = path.read_bytes();prefix, payload = raw.split(b'<AppendedData encoding="raw">_', 1)
        root = ET.fromstring(prefix + b"</VTKFile>")
        grid = root.find("ImageData")
        self.assertEqual(grid.get("Origin"), "1 2 3");self.assertEqual(grid.get("Spacing"), "1 1 1")
        self.assertEqual(grid.get("WholeExtent"), "0 4 0 5 0 6")
        arrays = root.findall("ImageData/Piece/PointData/DataArray")
        for array in arrays:
            offset = int(array.get("offset"));size = struct.unpack_from("<Q", payload, offset)[0]
            values = np.frombuffer(payload[offset+8:offset+8+size], dtype="<f8")
            if array.get("Name") == "ux":
                self.assertEqual(values[0], 0);self.assertEqual(values[1], 2);self.assertEqual(values[5], 3)
            if array.get("Name") == "solid":
                self.assertTrue(np.all(values == 0))
            if array.get("Name") == "derivative_valid":
                self.assertEqual(values.sum(), 3*4*5)
        pvd = ET.parse(output / "sequence.pvd").getroot()
        self.assertEqual(pvd.find("Collection/DataSet").get("timestep"), "10")
        mapped = export_vti([source], self.root / "physical-vti", self.mapping, physical=True)
        self.assertEqual(mapped["coordinateUnit"], "m");self.assertEqual(mapped["timelineUnit"], "s")

    def test_independent_vtk_reader_accepts_structured_grid(self):
        try:
            import vtk
            from vtk.util.numpy_support import vtk_to_numpy
        except ImportError:
            self.skipTest("Optional independent VTK reader is unavailable; binary/XML oracle remains covered")
        source = self.source()
        output = self.root / "vtk-reader"
        export_vti([source], output, self.mapping, physical=True, derivatives=True)
        reader = vtk.vtkXMLImageDataReader()
        reader.SetFileName(str(output / "snapshot-0000000010.vti"));reader.Update()
        grid = reader.GetOutput()
        self.assertEqual(grid.GetDimensions(), (5, 6, 7))
        np.testing.assert_allclose(grid.GetOrigin(), [.1, .2, .3])
        np.testing.assert_allclose(grid.GetSpacing(), [.1, .1, .1])
        velocity = vtk_to_numpy(grid.GetPointData().GetArray("velocity"))
        np.testing.assert_allclose(velocity[1], [10, 25, 65])
        self.assertEqual(vtk_to_numpy(grid.GetPointData().GetArray("derivative_valid")).sum(), 3*4*5)

    def test_bounded_strict_json_and_explicit_run_spec_conventions(self):
        for text in ('{"x":1,"x":2}', '{"x":NaN}', '{"x":1e999}'):
            with self.subTest(text=text), self.assertRaises(ArchiveError):
                bounded_json(text)
        spec = {"archive": {"axisOrder": "zyx", "coordinateUnits": "lattice", "velocityUnits": "lattice", "metadataOrder": "xyz"},
                "units": {"dxMeters": .1, "dtSeconds": .02, "densityReferenceKgM3": 1000.}}
        source = self.source(run_spec=np.array(json.dumps(spec)))
        mapping = Mapping(**mapping_from_run_spec(None, source))
        self.assertEqual(mapping.axis_order, "zyx");self.assertEqual(mapping.dx_m, .1)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(["vti", str(source), "--output", str(self.root / "cli-vti")]), 0)


if __name__ == "__main__":
    unittest.main()
