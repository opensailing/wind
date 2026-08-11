"""``cfdviz`` — the reference implementation of the CFDViz 1.1 case format.

The normative specification is ``FlowViz/Docs/CFDVIZ_FORMAT.md``. Where this
package and prose disagree, the prose wins; where the prose is silent, this
package is the tie-breaker for the Unreal reader, which is verified against it
through ``known_values.json`` (spec section 9).

A case is a *directory*::

    Sample.cfdviz/
      manifest.json           # what is in the case (spec 3)
      known_values.json       # cross-language verification bridge (spec 9)
      meshes/*.cvm            # triangle meshes (spec 5)
      meshes/*.cva            # mesh-associated arrays (spec 6)
      frames/000000/*.cvf     # bricked volume fields (spec 4)

Typical use::

    from cfdviz import read_cvf, validate_case

    report = validate_case("Sample.cfdviz")
    if not report.ok:
        raise SystemExit(report.render())

    values = read_cvf("Sample.cfdviz/frames/000000/U.cvf").values  # (X, Y, Z, C)

This is a **regular** package, not a namespace package: ``pyproject.toml``
declares ``package-data`` for the JSON Schema under ``schema/``, and setuptools
only collects package data for packages it can see through an ``__init__``.
"""

from __future__ import annotations

from .case import (
    FieldStatistics,
    ValidationReport,
    build_known_values,
    validate_case,
    verify_known_values,
    write_known_values,
)
from .codecs import (
    CODEC_LZ4,
    CODEC_NAMES,
    CODEC_NONE,
    CODEC_ZLIB,
    CODEC_ZSTD,
    ZSTD_REJECTION_MESSAGE,
    CodecError,
    UnsupportedCodecError,
    codec_id_from_name,
    compress,
    decompress,
    is_codec_available,
)
from .colormaps import COLORMAPS, ColorMap, build_lut, sample_colormap
from .crc32c import CHECK_VALUE, crc32c, self_check
from .cva import CVAData, CVAError, CVAHeader, read_cva, write_cva
from .cvf import (
    CVF_ASSOC_CELL,
    CVF_ASSOC_POINT,
    CVFData,
    CVFError,
    CVFFormatError,
    CVFHeader,
    CVFReader,
    read_cvf,
    write_cvf,
)
from .cvm import CVMData, CVMError, CVMHeader, read_cvm, write_cvm
from .manifest import (
    FORMAT_VERSION,
    ManifestError,
    is_safe_relative_path,
    load_manifest,
    load_schema,
    validate_manifest,
)
from .vtk_legacy import VTKError, VTKLegacyHeader, read_structured_points

#: Version of this package. Kept in step with ``pyproject.toml``; a test asserts
#: the two agree, because a drifting version makes a bug report unactionable.
__version__ = "1.0.0"

__all__ = [
    "__version__",
    "FORMAT_VERSION",
    # CRC-32C (spec 2)
    "crc32c",
    "self_check",
    "CHECK_VALUE",
    # Compression (spec 7)
    "CODEC_NONE",
    "CODEC_ZSTD",
    "CODEC_LZ4",
    "CODEC_ZLIB",
    "CODEC_NAMES",
    "ZSTD_REJECTION_MESSAGE",
    "CodecError",
    "UnsupportedCodecError",
    "compress",
    "decompress",
    "codec_id_from_name",
    "is_codec_available",
    # Bricked volume fields (spec 4)
    "read_cvf",
    "write_cvf",
    "CVFReader",
    "CVFData",
    "CVFHeader",
    "CVFError",
    "CVFFormatError",
    "CVF_ASSOC_CELL",
    "CVF_ASSOC_POINT",
    # External structured-volume inputs
    "read_structured_points",
    "VTKLegacyHeader",
    "VTKError",
    # Meshes and mesh arrays (spec 5 and 6)
    "read_cvm",
    "write_cvm",
    "CVMData",
    "CVMHeader",
    "CVMError",
    "read_cva",
    "write_cva",
    "CVAData",
    "CVAHeader",
    "CVAError",
    # Manifest (spec 3)
    "validate_manifest",
    "load_manifest",
    "load_schema",
    "is_safe_relative_path",
    "ManifestError",
    # Case validation and the verification bridge (spec 9 and 10)
    "validate_case",
    "ValidationReport",
    "FieldStatistics",
    "build_known_values",
    "write_known_values",
    "verify_known_values",
    # Colour mapping (spec 8)
    "COLORMAPS",
    "ColorMap",
    "build_lut",
    "sample_colormap",
]
