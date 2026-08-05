"""Packaging invariants.

``pyproject.toml`` uses ``[tool.setuptools.packages.find]`` and declares
``package-data`` for ``cfdviz``, both of which want a **regular** package. It
also declares a console script pointing at ``cfdviz.__main__:main``. This module
holds those declarations to the code.
"""

from __future__ import annotations

import importlib
import tomllib
from pathlib import Path

import pytest

import cfdviz

ROOT = Path(__file__).resolve().parents[1]


def pyproject() -> dict:
    return tomllib.loads((ROOT / "pyproject.toml").read_text(encoding="utf-8"))


def test_cfdviz_is_a_regular_package_not_a_namespace_package():
    """setuptools' package-data and packages.find both need a real package."""
    assert cfdviz.__file__ is not None, "cfdviz has no __init__.py"
    assert Path(cfdviz.__file__).name == "__init__.py"


def test_version_matches_pyproject():
    assert cfdviz.__version__ == pyproject()["project"]["version"]


def test_format_version_is_the_spec_version():
    assert cfdviz.FORMAT_VERSION == "1.0.0"


@pytest.mark.parametrize(
    "name",
    ["crc32c", "codecs", "colormaps", "cvf", "cvm", "cva", "manifest", "case",
     "__main__"],
)
def test_every_module_imports(name):
    importlib.import_module(f"cfdviz.{name}")


def test_console_script_target_resolves():
    scripts = pyproject()["project"]["scripts"]
    module_name, _, attribute = scripts["cfdviz"].partition(":")
    module = importlib.import_module(module_name)
    assert callable(getattr(module, attribute))


def test_top_level_re_exports_the_public_api():
    """Everything a caller needs without knowing the module layout."""
    for name in (
        "read_cvf", "write_cvf", "CVFReader",
        "read_cvm", "write_cvm",
        "read_cva", "write_cva",
        "crc32c", "validate_manifest", "validate_case",
    ):
        assert hasattr(cfdviz, name), name
        assert name in cfdviz.__all__, name


def test_all_names_are_importable():
    for name in cfdviz.__all__:
        assert hasattr(cfdviz, name), name


def test_schema_is_packaged_as_declared():
    declared = pyproject()["tool"]["setuptools"]["package-data"]["cfdviz"]
    assert "schema/*.json" in declared
    schema_dir = Path(cfdviz.__file__).parent / "schema"
    assert list(schema_dir.glob("*.json")), "no schema shipped inside the package"


def test_zstandard_is_not_a_dependency():
    """ADR 005 and spec 7: zlib ships, zstd is reserved and unimplemented."""
    project = pyproject()["project"]
    everything = list(project["dependencies"])
    for extra in project["optional-dependencies"].values():
        everything.extend(extra)
    assert not any("zstandard" in dep or "zstd" in dep for dep in everything)
